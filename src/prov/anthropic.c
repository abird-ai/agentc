/* anthropic.c — Anthropic Messages API adapter.
 *
 * build_request writes the provider headers + the JSON body (the loop splits
 * them at the blank line before handing them to the transport). map_sse turns
 * Messages SSE events into transcript blocks and usage; finish resolves a
 * pending stop reason.
 */
#include "agent.h"
#include "prov/provider.h"

/* internal helpers from messages.c (not part of the frozen header) */
AgcBlock *agentc_msg_block_new(AgcMsg *m, int type);
void agentc_msg_block_append(AgcBlock *b, const char *p, size_t n);
void agentc_msg_add_tool_call(AgcMsg *m, const char *id, const char *name);
size_t agentc_msg_count_tool_calls(const AgcMsg *m);
size_t agentc_msg_count_tool_calls(const AgcMsg *m);

static i64 thinking_budget(int level) {
    switch (level) {
    case 1: return 1024;
    case 2: return 2048;
    case 3: return 8192;
    case 4: return 24576;
    default: return 0;
    }
}

static bool is_oauth_token(const char *key) {
    /* Claude subscription access tokens are minted with the sk-ant-oat prefix
     * (API keys use sk-ant-api...). The CLI identity headers mirror the official client. */
    return key && agentc_str_starts(key, agentc_strlen(key), "sk-ant-oat");
}

static void append_header_value(AgcBuf *out, const char *v) {
    for (const char *k = v; k && *k; k++) {
        u8 c = (u8)*k;
        if (c < 0x20 || c == 0x7f) continue;   /* a CR/LF in a key would forge a header */
        agentc_buf_byte(out, c);
    }
}

static void write_headers(AgcBuf *out, const AgcRequest *r) {
    agentc_buf_cstr(out, "content-type: application/json\r\n");
    if (is_oauth_token(r->api_key)) {
        agentc_buf_cstr(out, "authorization: Bearer ");
        append_header_value(out, r->api_key);
        agentc_buf_cstr(out,
                    "\r\nuser-agent: claude-cli/2.1.280\r\n"
                    "x-app: cli\r\nanthropic-beta: oauth-2025-04-20\r\n");
    } else if (r->api_key && r->api_key[0]) {
        agentc_buf_cstr(out, "x-api-key: ");
        append_header_value(out, r->api_key);
        agentc_buf_cstr(out, "\r\n");
    }
    agentc_buf_cstr(out,
                "anthropic-version: 2023-06-01\r\n"
                "accept: text/event-stream\r\n"
                "\r\n");
}

/* Discovery auth (src/core/discover.c): the request adapter picks x-api-key for
 * API keys and a Bearer token for Claude subscription OAuth tokens, so the row
 * owns its discovery header to keep both paths identical. discover.c adds
 * anthropic-version itself for this style. */
static void anthropic_auth_headers(const AgcProviderOps *self, AgcBuf *out,
                                   const char *api_key) {
    (void)self;
    if (!api_key || !api_key[0]) return;
    if (is_oauth_token(api_key)) {
        agentc_buf_cstr(out, "authorization: Bearer ");
        append_header_value(out, api_key);
        /* the subscription endpoint expects the CLI identity headers too */
        agentc_buf_cstr(out,
                    "\r\nuser-agent: claude-cli/2.1.280\r\n"
                    "x-app: cli\r\nanthropic-beta: oauth-2025-04-20\r\n");
        return;
    }
    agentc_buf_cstr(out, "x-api-key: ");
    append_header_value(out, api_key);
    agentc_buf_cstr(out, "\r\n");
}

/* An assistant turn with neither text nor tool calls is invalid on the wire:
 * Ollama/OpenAI reject `content: null` and Anthropic expects a non-empty content
 * array. The agent loop's in-progress assistant slot is exactly that, and the
 * same applies to a replayed thinking-only turn (thinking is not replayed). */
static bool assistant_has_content(const AgcMsg *m) {
    if (agentc_msg_count_tool_calls(m)) return true;
    for (size_t i = 0; i < m->nblocks; i++)
        if (m->blocks[i].type == AGENTC_BLK_TEXT && m->blocks[i].text_len) return true;
    return false;
}

static void write_assistant(AgcJsonW *w, const AgcMsg *m) {
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "role");
    agentc_jsonw_cstr(w, "assistant");
    agentc_jsonw_key(w, "content");
    agentc_jsonw_arr(w);
    for (size_t i = 0; i < m->nblocks; i++) {
        const AgcBlock *b = &m->blocks[i];
        if (b->type == AGENTC_BLK_TEXT) {
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "type");
            agentc_jsonw_cstr(w, "text");
            agentc_jsonw_key(w, "text");
            agentc_jsonw_str(w, b->text ? b->text : "", b->text_len);
            agentc_jsonw_end(w);
        } else if (b->type == AGENTC_BLK_TOOLCALL) {
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "type");
            agentc_jsonw_cstr(w, "tool_use");
            agentc_jsonw_key(w, "id");
            agentc_jsonw_cstr(w, b->tool_id ? b->tool_id : "");
            agentc_jsonw_key(w, "name");
            agentc_jsonw_cstr(w, b->tool_name ? b->tool_name : "");
            agentc_jsonw_key(w, "input");
            if (b->tool_args && b->tool_args[0])
                agentc_jsonw_raw(w, b->tool_args, agentc_strlen(b->tool_args));
            else
                agentc_jsonw_raw(w, "{}", 2);
            agentc_jsonw_end(w);
        }
        /* thinking blocks are not replayed: no signature is retained */
    }
    agentc_jsonw_end(w);
    agentc_jsonw_end(w);
}

static void write_user_text(AgcJsonW *w, const AgcMsg *m) {
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "role");
    agentc_jsonw_cstr(w, "user");
    agentc_jsonw_key(w, "content");
    agentc_jsonw_arr(w);
    for (size_t i = 0; i < m->nblocks; i++) {
        if (m->blocks[i].type != AGENTC_BLK_TEXT) continue;
        agentc_jsonw_obj(w);
        agentc_jsonw_key(w, "type");
        agentc_jsonw_cstr(w, "text");
        agentc_jsonw_key(w, "text");
        agentc_jsonw_str(w, m->blocks[i].text ? m->blocks[i].text : "", m->blocks[i].text_len);
        agentc_jsonw_end(w);
    }
    if (m->nblocks == 0) {
        agentc_jsonw_obj(w);
        agentc_jsonw_key(w, "type");
        agentc_jsonw_cstr(w, "text");
        agentc_jsonw_key(w, "text");
        agentc_jsonw_cstr(w, "");
        agentc_jsonw_end(w);
    }
    agentc_jsonw_end(w);
    agentc_jsonw_end(w);
}

static void write_messages(AgcJsonW *w, const AgcTranscript *t) {
    agentc_jsonw_arr(w);
    for (size_t i = 0; i < t->n; i++) {
        const AgcMsg *m = &t->msgs[i];
        if (m->role == AGENTC_ROLE_USER) {
            write_user_text(w, m);
        } else if (m->role == AGENTC_ROLE_ASSISTANT) {
            if (!agentc_provider_msg_serializable(m) || !assistant_has_content(m)) continue;
            write_assistant(w, m);
        } else if (m->role == AGENTC_ROLE_TOOL) {
            /* group consecutive tool results into one user turn */
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "role");
            agentc_jsonw_cstr(w, "user");
            agentc_jsonw_key(w, "content");
            agentc_jsonw_arr(w);
            while (i < t->n && t->msgs[i].role == AGENTC_ROLE_TOOL) {
                const AgcMsg *tm = &t->msgs[i];
                const AgcBlock *res = tm->nblocks ? &tm->blocks[0] : NULL;
                agentc_jsonw_obj(w);
                agentc_jsonw_key(w, "type");
                agentc_jsonw_cstr(w, "tool_result");
                agentc_jsonw_key(w, "tool_use_id");
                agentc_jsonw_cstr(w, res && res->tool_id ? res->tool_id : "");
                agentc_jsonw_key(w, "content");
                agentc_jsonw_arr(w);
                agentc_jsonw_obj(w);
                agentc_jsonw_key(w, "type");
                agentc_jsonw_cstr(w, "text");
                agentc_jsonw_key(w, "text");
                agentc_jsonw_str(w, res && res->text ? res->text : "",
                             res && res->text ? res->text_len : 0);
                agentc_jsonw_end(w);
                agentc_jsonw_end(w);
                agentc_jsonw_end(w);
                i++;
            }
            i--;
            agentc_jsonw_end(w);
            agentc_jsonw_end(w);
        }
        /* system messages live in the top-level system field */
    }
    agentc_jsonw_end(w);
}

static void write_tools(AgcJsonW *w, const AgcRequest *r) {
    agentc_jsonw_key(w, "tools");
    agentc_jsonw_arr(w);
    for (size_t i = 0; i < r->ntools; i++) {
        const AgcTool *t = &r->tools[i];
        if (t->flags & AGENTC_TOOL_HIDDEN) continue;
        agentc_jsonw_obj(w);
        agentc_jsonw_key(w, "name");
        agentc_jsonw_cstr(w, t->name);
        agentc_jsonw_key(w, "description");
        agentc_jsonw_cstr(w, t->desc ? t->desc : "");
        agentc_jsonw_key(w, "input_schema");
        if (t->params_json && t->params_json[0])
            agentc_jsonw_raw(w, t->params_json, agentc_strlen(t->params_json));
        else
            agentc_jsonw_raw(w, "{\"type\":\"object\"}",
                             sizeof("{\"type\":\"object\"}") - 1);
        agentc_jsonw_end(w);
    }
    agentc_jsonw_end(w);
}

static int anthropic_build(AgcBuf *out, const AgcRequest *r, const char *url_host,
                           const char *url_path) {
    (void)url_host;
    (void)url_path;
    write_headers(out, r);

    i64 max_tokens = r->max_tokens;
    const AgcModel *model = agentc_model_find(r->provider, r->model);
    if (max_tokens <= 0) max_tokens = model ? (i64)model->max_tokens : 4096;
    i64 budget = thinking_budget(r->thinking_level);
    if (budget > 0 && max_tokens < budget + 1024) max_tokens = budget + 1024;

    AgcJsonW w;
    agentc_jsonw_init(&w, out);
    agentc_jsonw_obj(&w);

    agentc_jsonw_key(&w, "model");
    agentc_jsonw_cstr(&w, r->model ? r->model : "");
    agentc_jsonw_key(&w, "max_tokens");
    agentc_jsonw_i64(&w, max_tokens);

    if (r->system && r->system[0]) {
        agentc_jsonw_key(&w, "system");
        agentc_jsonw_arr(&w);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, "text");
        agentc_jsonw_key(&w, "text");
        agentc_jsonw_cstr(&w, r->system);
        agentc_jsonw_end(&w);
        agentc_jsonw_end(&w);
    }

    agentc_jsonw_key(&w, "messages");
    const AgcTranscript *tr = r->transcript;
    if (tr) {
        write_messages(&w, tr);
    } else {
        agentc_jsonw_arr(&w);
        agentc_jsonw_end(&w);
    }

    if (agentc_request_visible_tools(r)) write_tools(&w, r);

    agentc_jsonw_key(&w, "stream");
    agentc_jsonw_bool(&w, true);

    if (budget > 0) {
        agentc_jsonw_key(&w, "thinking");
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, "enabled");
        agentc_jsonw_key(&w, "budget_tokens");
        agentc_jsonw_i64(&w, budget);
        agentc_jsonw_end(&w);
    }

    agentc_jsonw_end(&w);
    return 0;
}

/* ------------------------------------------------------------- SSE map */

static int map_stop_reason(const char *s) {
    if (!s) return AGENTC_STOP_PENDING;
    if (agentc_streq(s, "end_turn") || agentc_streq(s, "stop_sequence") || agentc_streq(s, "pause_turn"))
        return AGENTC_STOP_STOP;
    if (agentc_streq(s, "max_tokens")) return AGENTC_STOP_LENGTH;
    if (agentc_streq(s, "tool_use")) return AGENTC_STOP_TOOLUSE;
    if (agentc_streq(s, "refusal")) return AGENTC_STOP_ERROR;
    return AGENTC_STOP_PENDING;
}

static void set_err(AgcStreamState *st, const char *s) {
    st->stop_reason = AGENTC_STOP_ERROR;
    agentc_snprintf(st->error, sizeof st->error, "%s", s ? s : "provider error");
}

/* Adapter-private stream state: a bounded map from each started content-block
 * `index` (text, thinking and tool_use alike) to the position of its block in
 * st->msg->blocks. Positions are stable because blocks are append-only during
 * a stream, while block pointers are not (agentc_msg_block_new can realloc
 * msg->blocks). This keeps interleaved blocks (e.g. text, tool_use, text) in
 * order instead of merging every delta into the last block of its type. Only a
 * tool_use may use the fallback slot for an out-of-range index (so a later
 * input_json_delta cannot land in another type's block); text/thinking falls
 * back to the last block of its own type instead. A single fallback slot covers
 * out-of-range/negative indices. The lazy create keeps direct map_sse callers
 * (tests) working even without stream_state_init. */
#define ANTHROPIC_MAX_TOOL_BLOCKS 256

typedef struct {
    size_t by_idx[ANTHROPIC_MAX_TOOL_BLOCKS]; /* content-block index -> block position */
    bool fb_set;
    i64 fb_idx;
    size_t fb_block;
    size_t tool_blocks;
} AnthropicPriv;

static AnthropicPriv *anthropic_priv(AgcStreamState *st) {
    if (!st->priv) {
        AnthropicPriv *pv = agentc_alloc(sizeof *pv);
        for (size_t i = 0; i < sizeof pv->by_idx / sizeof pv->by_idx[0]; i++)
            pv->by_idx[i] = (size_t)-1;
        st->priv = pv;
    }
    return st->priv;
}

static void block_map_set(AnthropicPriv *pv, size_t block, i64 idx, bool allow_fallback) {
    if (idx >= 0 && (size_t)idx < sizeof pv->by_idx / sizeof pv->by_idx[0]) {
        pv->by_idx[(size_t)idx] = block;
    } else if (allow_fallback) {
        pv->fb_set = true;
        pv->fb_idx = idx;
        pv->fb_block = block;
    }
}

static size_t block_map_get(const AnthropicPriv *pv, i64 idx) {
    if (idx >= 0 && (size_t)idx < sizeof pv->by_idx / sizeof pv->by_idx[0] &&
        pv->by_idx[(size_t)idx] != (size_t)-1)
        return pv->by_idx[(size_t)idx];
    if (pv->fb_set && pv->fb_idx == idx) return pv->fb_block;
    return (size_t)-1;
}

/* Last block of a type, for a lenient stream that sends deltas without a
 * preceding content_block_start. Returns NULL when none exists. */
static AgcBlock *last_of_type(AgcMsg *m, int type) {
    for (size_t i = m->nblocks; i > 0; i--)
        if (m->blocks[i - 1].type == type) return &m->blocks[i - 1];
    return NULL;
}

static int anthropic_stream_open(AgcStreamState *st) {
    (void)anthropic_priv(st);
    return 0;
}

static void anthropic_stream_close(AgcStreamState *st) {
    agentc_free(st->priv);
    st->priv = NULL;
}

static int anthropic_map(AgcStreamState *st, const AgcSseEvent *ev) {
    if (!st || !ev) return -1;
    if (!ev->data || ev->data_len == 0) return 0;
    if (!st->arena) st->arena = agentc_json_arena_new(0);
    AgcJson *d = agentc_json_parse_in(st->arena, ev->data, ev->data_len);
    if (!d) {
        set_err(st, "invalid SSE JSON");
        return -1;
    }
    const char *type = agentc_json_get_str(d, "type");
    if (!type) return 0;

    if (agentc_streq(type, "message_start")) {
        AgcJson *msg = agentc_json_get(d, "message");
        const char *id = agentc_json_get_str(msg, "id");
        if (id) agentc_snprintf(st->response_id, sizeof st->response_id, "%s", id);
        AgcJson *u = agentc_json_get(msg, "usage");
        if (u) {
            st->usage_input = (u32)agentc_json_get_int(u, "input_tokens", st->usage_input);
            st->usage_output = (u32)agentc_json_get_int(u, "output_tokens", st->usage_output);
            st->usage_cache_read = (u32)agentc_json_get_int(u, "cache_read_input_tokens", 0);
            st->usage_cache_write = (u32)agentc_json_get_int(u, "cache_creation_input_tokens", 0);
        }
    } else if (agentc_streq(type, "content_block_start")) {
        AgcJson *cb = agentc_json_get(d, "content_block");
        const char *ct = agentc_json_get_str(cb, "type");
        if (!ct) return 0;
        size_t bi = (size_t)-1;
        if (agentc_streq(ct, "text")) {
            AgcBlock *b = agentc_msg_block_new(st->msg, AGENTC_BLK_TEXT);
            const char *txt = agentc_json_get_str(cb, "text");
            if (txt && txt[0]) agentc_msg_block_append(b, txt, agentc_strlen(txt));
            bi = st->msg->nblocks - 1;
        } else if (agentc_streq(ct, "thinking")) {
            AgcBlock *b = agentc_msg_block_new(st->msg, AGENTC_BLK_THINK);
            const char *th = agentc_json_get_str(cb, "thinking");
            if (th && th[0]) agentc_msg_block_append(b, th, agentc_strlen(th));
            bi = st->msg->nblocks - 1;
        } else if (agentc_streq(ct, "tool_use")) {
            AnthropicPriv *pv = anthropic_priv(st);
            if (pv->tool_blocks < ANTHROPIC_MAX_TOOL_BLOCKS) {
                const char *id = agentc_json_get_str(cb, "id");
                const char *name = agentc_json_get_str(cb, "name");
                agentc_msg_add_tool_call(st->msg, id, name);
                pv->tool_blocks++;
                bi = st->msg->nblocks - 1;
            }
        }
        /* Record every started block, not just tool_use, so a later delta lands
         * in the block its own index opened rather than the last of its type. A
         * text/thinking index outside the map's range is handled by the type
         * fallback at delta time, not the shared fallback slot. */
        if (bi != (size_t)-1)
            block_map_set(anthropic_priv(st), bi, agentc_json_get_int(d, "index", 0),
                          agentc_streq(ct, "tool_use"));
    } else if (agentc_streq(type, "content_block_delta")) {
        AgcJson *delta = agentc_json_get(d, "delta");
        const char *dt = agentc_json_get_str(delta, "type");
        if (!dt) return 0;
        size_t bi = block_map_get(anthropic_priv(st), agentc_json_get_int(d, "index", 0));
        bool mapped = bi != (size_t)-1 && bi < st->msg->nblocks;
        size_t len = 0;
        if (agentc_streq(dt, "text_delta")) {
            const char *s = agentc_json_str(agentc_json_get(delta, "text"), &len);
            if (s && len) {
                AgcBlock *b = (mapped && st->msg->blocks[bi].type == AGENTC_BLK_TEXT)
                                  ? &st->msg->blocks[bi]
                                  : last_of_type(st->msg, AGENTC_BLK_TEXT);
                if (!b) b = agentc_msg_block_new(st->msg, AGENTC_BLK_TEXT);
                agentc_msg_block_append(b, s, len);
            }
        } else if (agentc_streq(dt, "thinking_delta")) {
            const char *s = agentc_json_str(agentc_json_get(delta, "thinking"), &len);
            if (s && len) {
                AgcBlock *b = (mapped && st->msg->blocks[bi].type == AGENTC_BLK_THINK)
                                  ? &st->msg->blocks[bi]
                                  : last_of_type(st->msg, AGENTC_BLK_THINK);
                if (!b) b = agentc_msg_block_new(st->msg, AGENTC_BLK_THINK);
                agentc_msg_block_append(b, s, len);
            }
        } else if (agentc_streq(dt, "input_json_delta")) {
            /* Strict: an unmapped tool argument must be dropped, not appended to
             * another call's block, or a hostile payload would splice into a
             * legitimate call and be executed as its arguments. */
            if (!mapped) return 0;
            const char *s = agentc_json_str(agentc_json_get(delta, "partial_json"), &len);
            if (s && len) agentc_msg_block_append(&st->msg->blocks[bi], s, len);
        }
    } else if (agentc_streq(type, "message_delta")) {
        AgcJson *delta = agentc_json_get(d, "delta");
        const char *sr = agentc_json_get_str(delta, "stop_reason");
        int mapped = map_stop_reason(sr);
        if (mapped != AGENTC_STOP_PENDING) st->stop_reason = mapped;
        AgcJson *u = agentc_json_get(d, "usage");
        if (u) {
            st->usage_output = (u32)agentc_json_get_int(u, "output_tokens", st->usage_output);
            st->usage_input = (u32)agentc_json_get_int(u, "input_tokens", st->usage_input);
            st->usage_cache_read =
                (u32)agentc_json_get_int(u, "cache_read_input_tokens", st->usage_cache_read);
            st->usage_cache_write =
                (u32)agentc_json_get_int(u, "cache_creation_input_tokens", st->usage_cache_write);
        }
    } else if (agentc_streq(type, "message_stop")) {
        st->saw_stop = true;
    } else if (agentc_streq(type, "error")) {
        AgcJson *e = agentc_json_get(d, "error");
        const char *msg = e ? agentc_json_get_str(e, "message") : NULL;
        const char *et = e ? agentc_json_get_str(e, "type") : NULL;
        if (msg && et)
            agentc_snprintf(st->error, sizeof st->error, "%s: %s", et, msg);
        else
            agentc_snprintf(st->error, sizeof st->error, "%s", msg ? msg : "provider error");
        st->stop_reason = AGENTC_STOP_ERROR;
    }
    return 0;
}

static int anthropic_finish(AgcStreamState *st) {
    if (!st) return -1;
    if (!st->saw_stop && st->stop_reason == AGENTC_STOP_PENDING) {
        /* the connection dropped before message_stop: never accept it as a turn */
        agentc_snprintf(st->error, sizeof st->error, "stream ended before message_stop");
        st->stop_reason = AGENTC_STOP_ERROR;
        return -1;
    }
    if (st->stop_reason == AGENTC_STOP_PENDING) {
        st->stop_reason =
            agentc_msg_count_tool_calls(st->msg) ? AGENTC_STOP_TOOLUSE : AGENTC_STOP_STOP;
    }
    return st->stop_reason == AGENTC_STOP_ERROR ? -1 : 0;
}

AgcProviderOps agentc_anthropic_ops = {
    .name = "anthropic",
    .api = "anthropic-messages",
    .path = "/v1/messages",
    .default_base_url = "https://api.anthropic.com",
    .env_keys = { "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", NULL },
    .needs_key = 1,
    .discover_style = AGENTC_DISCOVER_ANTHROPIC,
    .max_tokens_key = "max_tokens",
    .build_request = anthropic_build,
    .map_sse = anthropic_map,
    .finish = anthropic_finish,
    .stream_open = anthropic_stream_open,
    .stream_close = anthropic_stream_close,
    .auth_headers = anthropic_auth_headers,
    .handle = NULL,
};

const AgcProvider *agentc_prov_anthropic(void) {
    return agentc_provider_handle(&agentc_anthropic_ops);
}
