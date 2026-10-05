/* openai.c — OpenAI Chat Completions adapter.
 *
 * build_request writes the provider headers + JSON body; map_sse maps
 * choices[].delta fragments (text, reasoning, tool calls keyed by index) and
 * the final usage chunk; finish resolves a pending stop.
 */
#include "agent.h"
#include "prov/provider.h"

/* internal helpers from messages.c (not part of the frozen header) */
AgcBlock *agentc_msg_block_new(AgcMsg *m, int type);
void agentc_msg_block_append(AgcBlock *b, const char *p, size_t n);
size_t agentc_msg_count_tool_calls(const AgcMsg *m);
const AgcTranscript *agentc_transcript_build(void);

static const char *reasoning_effort(int level) {
    if (level <= 0) return NULL;
    if (level >= 4) return "high";
    if (level == 3) return "medium";
    return "low";
}

static void write_headers(AgcBuf *out, const AgcRequest *r) {
    agentc_buf_cstr(out, "content-type: application/json\r\n");
    if (r->api_key && r->api_key[0]) {
        agentc_buf_cstr(out, "authorization: Bearer ");
        agentc_buf_cstr(out, r->api_key);
        agentc_buf_cstr(out, "\r\n");
    }
    agentc_buf_cstr(out, "accept: text/event-stream\r\n\r\n");
}

/* An assistant turn that has neither text nor tool calls would serialise to
 * `"content": null` (and no tool_calls). Ollama's OpenAI layer rejects exactly
 * that with 400 "invalid message content type: <nil>". The agent loop keeps an
 * empty assistant message as the in-progress slot for the current turn, so it
 * must not reach the wire; the same applies to a replayed thinking-only turn. */
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

    /* concatenate text blocks into one string */
    size_t text_len = 0;
    for (size_t i = 0; i < m->nblocks; i++)
        if (m->blocks[i].type == AGENTC_BLK_TEXT) text_len += m->blocks[i].text_len;

    agentc_jsonw_key(w, "content");
    if (text_len == 0) {
        agentc_jsonw_null(w);
    } else {
        AgcBuf text = { 0 };
        for (size_t i = 0; i < m->nblocks; i++) {
            if (m->blocks[i].type == AGENTC_BLK_TEXT && m->blocks[i].text)
                agentc_buf_push(&text, m->blocks[i].text, m->blocks[i].text_len);
        }
        agentc_jsonw_str(w, (const char *)text.p, text.len);
        agentc_buf_free(&text);
    }

    size_t ntc = agentc_msg_count_tool_calls(m);
    if (ntc) {
        agentc_jsonw_key(w, "tool_calls");
        agentc_jsonw_arr(w);
        for (size_t i = 0; i < m->nblocks; i++) {
            const AgcBlock *b = &m->blocks[i];
            if (b->type != AGENTC_BLK_TOOLCALL) continue;
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "id");
            agentc_jsonw_cstr(w, b->tool_id ? b->tool_id : "");
            agentc_jsonw_key(w, "type");
            agentc_jsonw_cstr(w, "function");
            agentc_jsonw_key(w, "function");
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "name");
            agentc_jsonw_cstr(w, b->tool_name ? b->tool_name : "");
            agentc_jsonw_key(w, "arguments");
            const char *args = (b->tool_args && b->tool_args[0]) ? b->tool_args : "{}";
            agentc_jsonw_cstr(w, args);
            agentc_jsonw_end(w);
            agentc_jsonw_end(w);
        }
        agentc_jsonw_end(w);
    }
    agentc_jsonw_end(w);
}

static void write_messages(AgcJsonW *w, const AgcRequest *r, const AgcTranscript *t) {
    agentc_jsonw_arr(w);
    if (r->system && r->system[0]) {
        agentc_jsonw_obj(w);
        agentc_jsonw_key(w, "role");
        agentc_jsonw_cstr(w, "system");
        agentc_jsonw_key(w, "content");
        agentc_jsonw_cstr(w, r->system);
        agentc_jsonw_end(w);
    }
    for (size_t i = 0; i < t->n; i++) {
        const AgcMsg *m = &t->msgs[i];
        if (m->role == AGENTC_ROLE_USER) {
            AgcBuf text = { 0 };
            for (size_t j = 0; j < m->nblocks; j++) {
                if (m->blocks[j].type == AGENTC_BLK_TEXT && m->blocks[j].text)
                    agentc_buf_push(&text, m->blocks[j].text, m->blocks[j].text_len);
            }
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "role");
            agentc_jsonw_cstr(w, "user");
            agentc_jsonw_key(w, "content");
            agentc_jsonw_str(w, text.p ? (const char *)text.p : "", text.p ? text.len : 0);
            agentc_jsonw_end(w);
            agentc_buf_free(&text);
        } else if (m->role == AGENTC_ROLE_ASSISTANT) {
            if (!agentc_provider_msg_serializable(m) || !assistant_has_content(m)) continue;
            write_assistant(w, m);
        } else if (m->role == AGENTC_ROLE_TOOL) {
            const AgcBlock *res = m->nblocks ? &m->blocks[0] : NULL;
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "role");
            agentc_jsonw_cstr(w, "tool");
            agentc_jsonw_key(w, "tool_call_id");
            agentc_jsonw_cstr(w, res && res->tool_id ? res->tool_id : "");
            agentc_jsonw_key(w, "content");
            agentc_jsonw_str(w, res && res->text ? res->text : "",
                         res && res->text ? res->text_len : 0);
            agentc_jsonw_end(w);
        }
    }
    agentc_jsonw_end(w);
}

static int openai_build(AgcBuf *out, const AgcRequest *r, const char *url_host,
                        const char *url_path) {
    (void)url_host;
    (void)url_path;
    write_headers(out, r);

    i64 max_tokens = r->max_tokens;
    const AgcModel *model = agentc_model_find(r->provider, r->model);
    if (max_tokens <= 0 && model) max_tokens = (i64)model->max_tokens;

    AgcJsonW w;
    agentc_jsonw_init(&w, out);
    agentc_jsonw_obj(&w);

    agentc_jsonw_key(&w, "model");
    agentc_jsonw_cstr(&w, r->model ? r->model : "");

    agentc_jsonw_key(&w, "messages");
    const AgcTranscript *tr = r->transcript;
    if (tr) {
        write_messages(&w, r, tr);
    } else {
        agentc_jsonw_arr(&w);
        agentc_jsonw_end(&w);
    }

    if (agentc_request_visible_tools(r)) {
        agentc_jsonw_key(&w, "tools");
        agentc_jsonw_arr(&w);
        for (size_t i = 0; i < r->ntools; i++) {
            const AgcTool *t = &r->tools[i];
            if (t->flags & AGENTC_TOOL_HIDDEN) continue;
            agentc_jsonw_obj(&w);
            agentc_jsonw_key(&w, "type");
            agentc_jsonw_cstr(&w, "function");
            agentc_jsonw_key(&w, "function");
            agentc_jsonw_obj(&w);
            agentc_jsonw_key(&w, "name");
            agentc_jsonw_cstr(&w, t->name);
            agentc_jsonw_key(&w, "description");
            agentc_jsonw_cstr(&w, t->desc ? t->desc : "");
            agentc_jsonw_key(&w, "parameters");
            if (t->params_json && t->params_json[0])
                agentc_jsonw_raw(&w, t->params_json, agentc_strlen(t->params_json));
            else
                agentc_jsonw_raw(&w, "{\"type\":\"object\"}",
                                 sizeof("{\"type\":\"object\"}") - 1);
            agentc_jsonw_end(&w);
            agentc_jsonw_end(&w);
        }
        agentc_jsonw_end(&w);
    }

    const char *effort = reasoning_effort(r->thinking_level);
    if (effort) {
        agentc_jsonw_key(&w, "reasoning_effort");
        agentc_jsonw_cstr(&w, effort);
    }

    agentc_jsonw_key(&w, "stream");
    agentc_jsonw_bool(&w, true);
    agentc_jsonw_key(&w, "stream_options");
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "include_usage");
    agentc_jsonw_bool(&w, true);
    agentc_jsonw_end(&w);

    if (max_tokens > 0) {
        /* The output-cap field name is provider data (ops->max_tokens_key):
         * Ollama's OpenAI-compatible layer maps `max_tokens` to num_predict and
         * silently ignores `max_completion_tokens`, while first-party OpenAI
         * reasoning models require the latter. Custom endpoints default to
         * `max_tokens` (the broader-compatibility choice). */
        agentc_jsonw_key(&w, agentc_provider_max_tokens_key(r->provider));
        agentc_jsonw_i64(&w, max_tokens);
    }

    agentc_jsonw_end(&w);
    return 0;
}

/* ------------------------------------------------------------- SSE map */

static int map_finish_reason(AgcStreamState *st, const char *s) {
    if (!s) return 0;
    if (agentc_streq(s, "stop")) {
        st->stop_reason = AGENTC_STOP_STOP;
    } else if (agentc_streq(s, "length")) {
        st->stop_reason = AGENTC_STOP_LENGTH;
    } else if (agentc_streq(s, "tool_calls")) {
        st->stop_reason = AGENTC_STOP_TOOLUSE;
    } else if (agentc_streq(s, "content_filter")) {
        st->stop_reason = AGENTC_STOP_ERROR;
        agentc_snprintf(st->error, sizeof st->error, "content filter");
    }
    return 0;
}

static void append_stream(AgcMsg *m, int type, const char *s, size_t n) {
    if (!s || n == 0) return;
    AgcBlock *b = (m->nblocks && m->blocks[m->nblocks - 1].type == type)
                     ? &m->blocks[m->nblocks - 1]
                     : agentc_msg_block_new(m, type);
    agentc_msg_block_append(b, s, n);
}

/* Adapter-private stream state: a bounded map from the OpenAI tool-call
 * `index` to the position of its block in st->msg->blocks. Positions are stable
 * because blocks are append-only during a stream, while block pointers are not
 * (agentc_msg_block_new can realloc msg->blocks). The lazy create keeps direct
 * map_sse callers (tests) working even without stream_state_init. */
/* Single fallback slot for out-of-range/negative tool_call indices: reused
 * while the same index repeats so one out-of-range call split across chunks
 * stays a single block instead of growing msg->nblocks on every delta. */
typedef struct {
    bool set;
    i64 idx;
    size_t block;
} OpenAiLastIndex;

/* Bound the number of tool blocks one stream can create so a hostile server
 * cannot grow msg->nblocks without limit. */
#define OPENAI_MAX_TOOL_BLOCKS 256

typedef struct {
    size_t by_idx[OPENAI_MAX_TOOL_BLOCKS]; /* tool_call index -> block position */
    OpenAiLastIndex last_unmapped;
    size_t tool_blocks; /* new tool blocks created this stream */
} OpenAiPriv;

static OpenAiPriv *openai_priv(AgcStreamState *st) {
    if (!st->priv) {
        OpenAiPriv *pv = agentc_alloc(sizeof *pv);
        for (size_t i = 0; i < sizeof pv->by_idx / sizeof pv->by_idx[0]; i++)
            pv->by_idx[i] = (size_t)-1;
        st->priv = pv;
    }
    return st->priv;
}

static int openai_stream_open(AgcStreamState *st) {
    (void)openai_priv(st);
    return 0;
}

static void openai_stream_close(AgcStreamState *st) {
    agentc_free(st->priv);
    st->priv = NULL;
}

static int openai_map(AgcStreamState *st, const AgcSseEvent *ev) {
    if (!st || !ev) return -1;
    if (!ev->data || ev->data_len == 0) return 0;
    if (ev->data_len == 6 && agentc_memeq(ev->data, "[DONE]", 6)) {
        st->saw_stop = true;
        return 0;
    }
    if (!st->arena) st->arena = agentc_json_arena_new(0);
    AgcJson *d = agentc_json_parse_in(st->arena, ev->data, ev->data_len);
    if (!d) {
        st->stop_reason = AGENTC_STOP_ERROR;
        agentc_snprintf(st->error, sizeof st->error, "invalid SSE JSON");
        return -1;
    }

    AgcJson *err = agentc_json_get(d, "error");
    if (err) {
        const char *msg = agentc_json_get_str(err, "message");
        st->stop_reason = AGENTC_STOP_ERROR;
        agentc_snprintf(st->error, sizeof st->error, "%s", msg ? msg : "provider error");
        return 0;
    }

    const char *id = agentc_json_get_str(d, "id");
    if (id && !st->response_id[0])
        agentc_snprintf(st->response_id, sizeof st->response_id, "%s", id);

    AgcJson *choices = agentc_json_get(d, "choices");
    AgcJson *choice = agentc_json_at(choices, 0);
    if (choice) {
        AgcJson *delta = agentc_json_get(choice, "delta");
        if (delta) {
            size_t len = 0;
            const char *content = agentc_json_str(agentc_json_get(delta, "content"), &len);
            if (content && len) append_stream(st->msg, AGENTC_BLK_TEXT, content, len);

            const char *reason = agentc_json_str(agentc_json_get(delta, "reasoning_content"), &len);
            if (!reason) reason = agentc_json_str(agentc_json_get(delta, "reasoning"), &len);
            if (reason && len) append_stream(st->msg, AGENTC_BLK_THINK, reason, len);

            AgcJson *tcs = agentc_json_get(delta, "tool_calls");
            if (tcs && agentc_json_type(tcs) == AGENTC_JSON_ARR) {
                OpenAiPriv *pv = openai_priv(st);
                for (size_t i = 0; i < agentc_json_len(tcs); i++) {
                    AgcJson *tc = agentc_json_at(tcs, i);
                    i64 idx = agentc_json_get_int(tc, "index", (i64)i);
                    bool bounded = idx >= 0 &&
                                   (size_t)idx < sizeof pv->by_idx / sizeof pv->by_idx[0];
                    size_t bi = (size_t)-1;
                    if (bounded && pv->by_idx[(size_t)idx] != (size_t)-1) {
                        bi = pv->by_idx[(size_t)idx];
                    } else if (bounded) {
                        if (pv->tool_blocks < OPENAI_MAX_TOOL_BLOCKS) {
                            agentc_msg_block_new(st->msg, AGENTC_BLK_TOOLCALL);
                            bi = st->msg->nblocks - 1;
                            pv->by_idx[(size_t)idx] = bi;
                            pv->tool_blocks++;
                        }
                    } else if (pv->last_unmapped.set && pv->last_unmapped.idx == idx) {
                        /* Reuse the fallback block while this out-of-range index
                         * repeats; only create when the index changes. */
                        bi = pv->last_unmapped.block;
                    } else if (pv->tool_blocks < OPENAI_MAX_TOOL_BLOCKS) {
                        agentc_msg_block_new(st->msg, AGENTC_BLK_TOOLCALL);
                        bi = st->msg->nblocks - 1;
                        pv->last_unmapped.set = true;
                        pv->last_unmapped.idx = idx;
                        pv->last_unmapped.block = bi;
                        pv->tool_blocks++;
                    }
                    if (bi == (size_t)-1) continue; /* hostile cap: drop extras */
                    AgcBlock *b = &st->msg->blocks[bi];
                    const char *tc_id = agentc_json_get_str(tc, "id");
                    AgcJson *fn = agentc_json_get(tc, "function");
                    const char *name = fn ? agentc_json_get_str(fn, "name") : NULL;
                    size_t alen = 0;
                    const char *args =
                        fn ? agentc_json_str(agentc_json_get(fn, "arguments"), &alen) : NULL;

                    if (tc_id && (!b->tool_id || !b->tool_id[0])) {
                        agentc_free(b->tool_id);
                        b->tool_id = agentc_strdup(tc_id);
                    }
                    if (name && (!b->tool_name || !b->tool_name[0])) {
                        agentc_free(b->tool_name);
                        b->tool_name = agentc_strdup(name);
                    }
                    if (args && alen) agentc_msg_block_append(b, args, alen);
                }
            }
        }
        const char *fr = agentc_json_get_str(choice, "finish_reason");
        map_finish_reason(st, fr);
    }

    AgcJson *u = agentc_json_get(d, "usage");
    if (u && agentc_json_type(u) == AGENTC_JSON_OBJ) {
        st->usage_input = (u32)agentc_json_get_int(u, "prompt_tokens", st->usage_input);
        st->usage_output = (u32)agentc_json_get_int(u, "completion_tokens", st->usage_output);
        AgcJson *pd = agentc_json_get(u, "prompt_tokens_details");
        if (pd) {
            i64 cached = agentc_json_get_int(pd, "cached_tokens", 0);
            if (cached > 0) st->usage_cache_read = (u32)cached;
        }
        AgcJson *cd = agentc_json_get(u, "completion_tokens_details");
        if (cd) {
            st->usage_reasoning =
                (u32)agentc_json_get_int(cd, "reasoning_tokens", (i64)st->usage_reasoning);
        }
    }
    return 0;
}

static int openai_finish(AgcStreamState *st) {
    if (!st) return -1;
    if (!st->saw_stop && st->stop_reason == AGENTC_STOP_PENDING) {
        agentc_snprintf(st->error, sizeof st->error, "stream ended before [DONE]");
        st->stop_reason = AGENTC_STOP_ERROR;
        return -1;
    }
    if (st->stop_reason == AGENTC_STOP_PENDING) {
        st->stop_reason =
            agentc_msg_count_tool_calls(st->msg) ? AGENTC_STOP_TOOLUSE : AGENTC_STOP_STOP;
    }
    return st->stop_reason == AGENTC_STOP_ERROR ? -1 : 0;
}

AgcProviderOps agentc_openai_ops = {
    .name = "openai",
    .api = "openai-chat",
    .path = "/chat/completions",
    .default_base_url = "https://api.openai.com/v1",
    .env_keys = { "OPENAI_API_KEY", NULL, NULL },
    .needs_key = 1,
    .discover_style = AGENTC_DISCOVER_DEFAULT,
    .max_tokens_key = "max_completion_tokens",
    .build_request = openai_build,
    .map_sse = openai_map,
    .finish = openai_finish,
    .stream_open = openai_stream_open,
    .stream_close = openai_stream_close,
    .handle = NULL,
};

const AgcProvider *agentc_prov_openai(void) {
    return agentc_provider_handle(&agentc_openai_ops);
}

/* Ollama's /v1 endpoint is OpenAI-compatible; only the name, the default base
 * URL and the auth requirements differ. */
AgcProviderOps agentc_ollama_ops = {
    .name = "ollama",
    .api = "openai-chat",
    .path = "/chat/completions",
    .default_base_url = "http://127.0.0.1:11434/v1",
    .env_keys = { "OLLAMA_API_KEY", NULL, NULL },
    .needs_key = 0,
    .discover_style = AGENTC_DISCOVER_OLLAMA,
    .max_tokens_key = "max_tokens",
    .build_request = openai_build,
    .map_sse = openai_map,
    .finish = openai_finish,
    .stream_open = openai_stream_open,
    .stream_close = openai_stream_close,
    .handle = NULL,
};

AgcProviderOps agentc_ollama_cloud_ops = {
    .name = "ollama-cloud",
    .api = "openai-chat",
    .path = "/chat/completions",
    .default_base_url = "https://ollama.com/v1",
    .env_keys = { "OLLAMA_CLOUD_API_KEY", "OLLAMA_API_KEY", NULL },
    .needs_key = 1,
    .discover_style = AGENTC_DISCOVER_DEFAULT,
    .max_tokens_key = "max_tokens",
    .build_request = openai_build,
    .map_sse = openai_map,
    .finish = openai_finish,
    .stream_open = openai_stream_open,
    .stream_close = openai_stream_close,
    .handle = NULL,
};

const AgcProvider *agentc_prov_ollama(void) {
    return agentc_provider_handle(&agentc_ollama_ops);
}

const AgcProvider *agentc_prov_ollama_cloud(void) {
    return agentc_provider_handle(&agentc_ollama_cloud_ops);
}

/* Dynamically named OpenAI-compatible providers (user endpoints).  The row and
 * its handle are heap blocks that are never moved or freed, so the returned
 * pointer stays valid for the process lifetime. */
const AgcProvider *agentc_prov_openai_compatible(const char *name, const char *base_url) {
    if (!name || !name[0]) return NULL;
    const AgcProviderOps *existing = agentc_provider_by_name(name);
    if (existing) return agentc_provider_handle((AgcProviderOps *)existing);
    AgcProviderOps *ops = agentc_alloc(sizeof *ops);
    agentc_memcpy(ops, &agentc_openai_ops, sizeof *ops);
    ops->name = agentc_strdup(name);
    ops->default_base_url = agentc_strdup(base_url ? base_url : "");
    /* a user endpoint speaks the broad OpenAI-compatible dialect: `max_tokens`
     * is the field compatible gateways accept, unlike first-party OpenAI. */
    ops->max_tokens_key = "max_tokens";
    /* a user endpoint has no registry credential env vars */
    ops->env_keys[0] = NULL;
    ops->env_keys[1] = NULL;
    ops->env_keys[2] = NULL;
    ops->handle = NULL;
    if (agentc_provider_register(ops) < 0) {
        agentc_free((char *)ops->name);
        agentc_free((char *)ops->default_base_url);
        agentc_free(ops);
        return NULL;
    }
    return agentc_provider_handle(ops);
}
