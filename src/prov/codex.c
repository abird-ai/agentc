/* codex.c — OpenAI Codex (ChatGPT subscription) adapter.
 *
 * A ChatGPT OAuth credential is not an API key: it is only accepted by the
 * Codex backend (`https://chatgpt.com/backend-api/codex`) and only speaks the
 * Responses API. The request carries the ChatGPT account id in
 * `chatgpt-account-id`; results arrive as `response.*` SSE events.
 *
 * Selected automatically when the stored "openai" credential is an OAuth one;
 * with an API key the Chat Completions adapter is used instead.
 */
#include "agent.h"
#include "oauth.h"
#include "prov/provider.h"

/* internal helpers from messages.c (not part of the frozen header) */
AgcBlock *agentc_msg_block_new(AgcMsg *m, int type);
void agentc_msg_block_append(AgcBlock *b, const char *p, size_t n);
size_t agentc_msg_count_tool_calls(const AgcMsg *m);

/* ------------------------------------------------------------------ build */

static const char *reasoning_effort(int level) {
    if (level <= 0) return NULL;
    if (level >= 4) return "high";
    if (level == 3) return "medium";
    return "low";
}

static void append_header_value(AgcBuf *out, const char *v) {
    for (const char *k = v; k && *k; k++) {
        u8 c = (u8)*k;
        if (c < 0x20 || c == 0x7f) continue;   /* a CR/LF in a value would forge a header */
        agentc_buf_byte(out, c);
    }
}

static void write_headers(AgcBuf *out, const AgcRequest *r) {
    agentc_buf_cstr(out, "content-type: application/json\r\n");
    if (r->api_key && r->api_key[0]) {
        agentc_buf_cstr(out, "authorization: Bearer ");
        append_header_value(out, r->api_key);
        agentc_buf_cstr(out, "\r\n");
    }
    if (r->account_id && r->account_id[0]) {
        agentc_buf_cstr(out, "chatgpt-account-id: ");
        append_header_value(out, r->account_id);
        agentc_buf_cstr(out, "\r\n");
    }
    agentc_buf_cstr(out, "originator: agentc\r\n");
    agentc_buf_cstr(out, "openai-beta: responses=experimental\r\n");
    agentc_buf_cstr(out, "accept: text/event-stream\r\n\r\n");
}

/* Discovery auth for `GET {base}/models`: the Codex backend wants the same
 * identity as a request (Bearer OAuth token + chatgpt-account-id + originator).
 * The account id is derived from the stored OAuth credential. */
static void codex_auth_headers(const AgcProviderOps *self, AgcBuf *out,
                               const char *api_key) {
    if (api_key && api_key[0]) {
        agentc_buf_cstr(out, "authorization: Bearer ");
        append_header_value(out, api_key);
        agentc_buf_cstr(out, "\r\n");
    }
    const char *acct = agentc_oauth_account_id(self ? self->name : NULL);
    if (acct && acct[0]) {
        agentc_buf_cstr(out, "chatgpt-account-id: ");
        append_header_value(out, acct);
        agentc_buf_cstr(out, "\r\n");
    }
    agentc_buf_cstr(out, "originator: agentc\r\n");
    agentc_buf_cstr(out, "openai-beta: responses=experimental\r\n");
}

/* One `input` item per message, in Responses spelling. */
static void write_input_item(AgcJsonW *w, const AgcMsg *m) {
    if (m->role == AGENTC_ROLE_ASSISTANT) {
        size_t text_len = 0;
        for (size_t i = 0; i < m->nblocks; i++)
            if (m->blocks[i].type == AGENTC_BLK_TEXT) text_len += m->blocks[i].text_len;
        if (text_len) {
            AgcBuf text = { 0 };
            for (size_t i = 0; i < m->nblocks; i++)
                if (m->blocks[i].type == AGENTC_BLK_TEXT && m->blocks[i].text)
                    agentc_buf_push(&text, m->blocks[i].text, m->blocks[i].text_len);

            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "type");
            agentc_jsonw_cstr(w, "message");
            agentc_jsonw_key(w, "role");
            agentc_jsonw_cstr(w, "assistant");
            agentc_jsonw_key(w, "content");
            agentc_jsonw_arr(w);
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "type");
            agentc_jsonw_cstr(w, "output_text");
            agentc_jsonw_key(w, "text");
            agentc_jsonw_str(w, (const char *)text.p, text.len);
            agentc_jsonw_end(w);
            agentc_jsonw_end(w);
            agentc_jsonw_end(w);
            agentc_buf_free(&text);
        }
        for (size_t i = 0; i < m->nblocks; i++) {
            const AgcBlock *b = &m->blocks[i];
            if (b->type != AGENTC_BLK_TOOLCALL) continue;
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "type");
            agentc_jsonw_cstr(w, "function_call");
            agentc_jsonw_key(w, "call_id");
            agentc_jsonw_cstr(w, b->tool_id ? b->tool_id : "");
            agentc_jsonw_key(w, "name");
            agentc_jsonw_cstr(w, b->tool_name ? b->tool_name : "");
            agentc_jsonw_key(w, "arguments");
            agentc_jsonw_cstr(w, (b->tool_args && b->tool_args[0]) ? b->tool_args : "{}");
            agentc_jsonw_end(w);
        }
        return;
    }
    if (m->role == AGENTC_ROLE_TOOL) {
        const AgcBlock *res = m->nblocks ? &m->blocks[0] : NULL;
        agentc_jsonw_obj(w);
        agentc_jsonw_key(w, "type");
        agentc_jsonw_cstr(w, "function_call_output");
        agentc_jsonw_key(w, "call_id");
        agentc_jsonw_cstr(w, res && res->tool_id ? res->tool_id : "");
        agentc_jsonw_key(w, "output");
        agentc_jsonw_str(w, res && res->text ? res->text : "",
                         res && res->text ? res->text_len : 0);
        agentc_jsonw_end(w);
        return;
    }
    /* user */
    AgcBuf text = { 0 };
    for (size_t i = 0; i < m->nblocks; i++)
        if (m->blocks[i].type == AGENTC_BLK_TEXT && m->blocks[i].text)
            agentc_buf_push(&text, m->blocks[i].text, m->blocks[i].text_len);
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "type");
    agentc_jsonw_cstr(w, "message");
    agentc_jsonw_key(w, "role");
    agentc_jsonw_cstr(w, "user");
    agentc_jsonw_key(w, "content");
    agentc_jsonw_arr(w);
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "type");
    agentc_jsonw_cstr(w, "input_text");
    agentc_jsonw_key(w, "text");
    agentc_jsonw_str(w, text.p ? (const char *)text.p : "", text.p ? text.len : 0);
    agentc_jsonw_end(w);
    agentc_jsonw_end(w);
    agentc_jsonw_end(w);
    agentc_buf_free(&text);
}

static int codex_build(AgcBuf *out, const AgcRequest *r, const char *url_host,
                       const char *url_path) {
    (void)url_host;
    (void)url_path;
    write_headers(out, r);

    AgcJsonW w;
    agentc_jsonw_init(&w, out);
    agentc_jsonw_obj(&w);

    agentc_jsonw_key(&w, "model");
    agentc_jsonw_cstr(&w, r->model ? r->model : "");

    if (r->system && r->system[0]) {
        agentc_jsonw_key(&w, "instructions");
        agentc_jsonw_cstr(&w, r->system);
    }

    agentc_jsonw_key(&w, "input");
    agentc_jsonw_arr(&w);
    if (r->transcript) {
        for (size_t i = 0; i < r->transcript->n; i++) {
            const AgcMsg *m = &r->transcript->msgs[i];
            /* A failed/aborted assistant (partial args, no persisted results)
             * must not reach the Responses input. */
            if (!agentc_provider_msg_serializable(m)) continue;
            write_input_item(&w, m);
        }
    }
    agentc_jsonw_end(&w);

    if (agentc_request_visible_tools(r)) {
        agentc_jsonw_key(&w, "tools");
        agentc_jsonw_arr(&w);
        for (size_t i = 0; i < r->ntools; i++) {
            const AgcTool *t = &r->tools[i];
            if (t->flags & AGENTC_TOOL_HIDDEN) continue;
            agentc_jsonw_obj(&w);
            agentc_jsonw_key(&w, "type");
            agentc_jsonw_cstr(&w, "function");
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
            agentc_jsonw_key(&w, "strict");
            agentc_jsonw_bool(&w, false);
            agentc_jsonw_end(&w);
        }
        agentc_jsonw_end(&w);
        agentc_jsonw_key(&w, "tool_choice");
        agentc_jsonw_cstr(&w, "auto");
        agentc_jsonw_key(&w, "parallel_tool_calls");
        agentc_jsonw_bool(&w, true);
    }

    const char *effort = reasoning_effort(r->thinking_level);
    if (effort) {
        agentc_jsonw_key(&w, "reasoning");
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "effort");
        agentc_jsonw_cstr(&w, effort);
        agentc_jsonw_key(&w, "summary");
        agentc_jsonw_cstr(&w, "auto");
        agentc_jsonw_end(&w);
        agentc_jsonw_key(&w, "include");
        agentc_jsonw_arr(&w);
        agentc_jsonw_cstr(&w, "reasoning.encrypted_content");
        agentc_jsonw_end(&w);
    }

    /* the subscription backend rejects stored responses */
    agentc_jsonw_key(&w, "store");
    agentc_jsonw_bool(&w, false);
    agentc_jsonw_key(&w, "stream");
    agentc_jsonw_bool(&w, true);

    /* The Responses API output cap is `max_output_tokens` (not the Chat
     * Completions spelling). Emit it from the request when a cap is set. */
    if (r->max_tokens > 0) {
        agentc_jsonw_key(&w, "max_output_tokens");
        agentc_jsonw_i64(&w, r->max_tokens);
    }

    agentc_jsonw_end(&w);
    return 0;
}

/* ---------------------------------------------------------------- SSE map */

static void append_stream(AgcMsg *m, int type, const char *s, size_t n) {
    if (!s || n == 0) return;
    AgcBlock *b = (m->nblocks && m->blocks[m->nblocks - 1].type == type)
                     ? &m->blocks[m->nblocks - 1]
                     : agentc_msg_block_new(m, type);
    agentc_msg_block_append(b, s, n);
}

static void map_usage(AgcStreamState *st, const AgcJson *u) {
    st->usage_input = (u32)agentc_json_get_int(u, "input_tokens", st->usage_input);
    st->usage_output = (u32)agentc_json_get_int(u, "output_tokens", st->usage_output);
    AgcJson *id = agentc_json_get(u, "input_tokens_details");
    if (id) {
        i64 cached = agentc_json_get_int(id, "cached_tokens", 0);
        if (cached > 0) st->usage_cache_read = (u32)cached;
    }
    AgcJson *od = agentc_json_get(u, "output_tokens_details");
    if (od)
        st->usage_reasoning =
            (u32)agentc_json_get_int(od, "reasoning_tokens", (i64)st->usage_reasoning);
}

/* Adapter-private state: maps a Responses function_call item to the position of
 * its block in st->msg->blocks. The position is stable (blocks are append-only)
 * while the pointer is not (agentc_msg_block_new can realloc). Both the item id
 * and the event output_index are recorded so parallel calls' argument deltas
 * can be routed; a delta that matches neither is dropped rather than appended
 * to an unrelated tool block. */
enum { CODEX_MAX_CALLS = 32 };
typedef struct {
    char *item_id; /* item.id (owned) */
    char *call_id; /* item.call_id (owned) */
    bool has_index;
    i64 output_index;
    size_t block;
} CodexCall;
typedef struct {
    CodexCall calls[CODEX_MAX_CALLS];
    size_t ncalls;
} CodexPriv;

static CodexPriv *codex_priv(AgcStreamState *st) {
    if (!st->priv) st->priv = agentc_alloc(sizeof(CodexPriv));
    return st->priv;
}

static CodexCall *codex_find(CodexPriv *pv, const char *item_id, i64 output_index,
                             bool has_index) {
    for (size_t i = 0; i < pv->ncalls; i++) {
        CodexCall *c = &pv->calls[i];
        if (item_id && ((c->item_id && agentc_streq(c->item_id, item_id)) ||
                        (c->call_id && agentc_streq(c->call_id, item_id))))
            return c;
    }
    if (has_index)
        for (size_t i = 0; i < pv->ncalls; i++)
            if (pv->calls[i].has_index && pv->calls[i].output_index == output_index)
                return &pv->calls[i];
    return NULL;
}

/* `response.output_item.added`: a function_call starts a tool-call block. */
/* Last TOOLCALL block in the message, or NULL. Used only for the legacy
 * Responses stream that carries no item id / output_index at all. */
static AgcBlock *codex_last_toolcall(AgcMsg *m) {
    for (size_t i = m->nblocks; i > 0; i--)
        if (m->blocks[i - 1].type == AGENTC_BLK_TOOLCALL) return &m->blocks[i - 1];
    return NULL;
}

static void map_item_added(AgcStreamState *st, const AgcJson *d) {
    AgcJson *item = agentc_json_get(d, "item");
    const char *type = agentc_json_get_str(item, "type");
    if (!type || !agentc_streq(type, "function_call")) return;
    /* Bound block creation to the tracked-call cap: an untracked call would
     * later run with empty arguments, so drop it instead (like openai). */
    CodexPriv *pv = codex_priv(st);
    if (pv->ncalls >= CODEX_MAX_CALLS) return;
    AgcBlock *b = agentc_msg_block_new(st->msg, AGENTC_BLK_TOOLCALL);
    size_t bi = st->msg->nblocks - 1;
    const char *call_id = agentc_json_get_str(item, "call_id");
    const char *item_id = agentc_json_get_str(item, "id");
    const char *name = agentc_json_get_str(item, "name");
    b->tool_id = agentc_strdup(call_id ? call_id : (item_id ? item_id : ""));
    b->tool_name = agentc_strdup(name ? name : "");
    const char *args = agentc_json_get_str(item, "arguments");
    if (args && args[0]) agentc_msg_block_append(b, args, agentc_strlen(args));

    CodexCall *c = &pv->calls[pv->ncalls++];
    c->item_id = item_id ? agentc_strdup(item_id) : NULL;
    c->call_id = call_id ? agentc_strdup(call_id) : NULL;
    c->has_index = agentc_json_get(d, "output_index") != NULL;
    c->output_index = agentc_json_get_int(d, "output_index", 0);
    c->block = bi;
}

static int codex_map(AgcStreamState *st, const AgcSseEvent *ev) {
    if (!st || !ev) return -1;
    if (!ev->data || ev->data_len == 0) return 0;
    if (!st->arena) st->arena = agentc_json_arena_new(0);
    AgcJson *d = agentc_json_parse_in(st->arena, ev->data, ev->data_len);
    if (!d) {
        st->stop_reason = AGENTC_STOP_ERROR;
        agentc_snprintf(st->error, sizeof st->error, "invalid SSE JSON");
        return -1;
    }

    const char *type = agentc_json_get_str(d, "type");
    if (!type) return 0;

    if (agentc_streq(type, "error") || agentc_streq(type, "response.failed")) {
        AgcJson *resp = agentc_json_get(d, "response");
        AgcJson *err = agentc_json_get(d, "error");
        if (!err) err = agentc_json_get(resp, "error");
        const char *msg = agentc_json_get_str(err, "message");
        st->stop_reason = AGENTC_STOP_ERROR;
        agentc_snprintf(st->error, sizeof st->error, "%s", msg ? msg : "provider error");
        return 0;
    }

    if (agentc_streq(type, "response.created")) {
        AgcJson *resp = agentc_json_get(d, "response");
        const char *id = agentc_json_get_str(resp, "id");
        if (id && !st->response_id[0])
            agentc_snprintf(st->response_id, sizeof st->response_id, "%s", id);
    } else if (agentc_streq(type, "response.output_text.delta")) {
        size_t n = 0;
        const char *delta = agentc_json_str(agentc_json_get(d, "delta"), &n);
        append_stream(st->msg, AGENTC_BLK_TEXT, delta, n);
    } else if (agentc_streq(type, "response.reasoning_summary_text.delta") ||
               agentc_streq(type, "response.reasoning_text.delta")) {
        size_t n = 0;
        const char *delta = agentc_json_str(agentc_json_get(d, "delta"), &n);
        append_stream(st->msg, AGENTC_BLK_THINK, delta, n);
    } else if (agentc_streq(type, "response.output_item.added")) {
        map_item_added(st, d);
    } else if (agentc_streq(type, "response.function_call_arguments.delta")) {
        size_t n = 0;
        const char *delta = agentc_json_str(agentc_json_get(d, "delta"), &n);
        if (delta && n) {
            const char *item_id = agentc_json_get_str(d, "item_id");
            bool has_index = agentc_json_get(d, "output_index") != NULL;
            i64 output_index = agentc_json_get_int(d, "output_index", 0);
            CodexCall *c = codex_find(codex_priv(st), item_id, output_index, has_index);
            /* Route to a call we can identify. A delta that names an id/index we
             * never mapped (e.g. a function_call past CODEX_MAX_CALLS) is dropped
             * rather than appended to the last tool block, which would corrupt a
             * legitimate call. Only a delta with NO id and NO index (the legacy
             * single-call stream) falls back to the last tool block. */
            AgcBlock *b = NULL;
            if (c) b = &st->msg->blocks[c->block];
            else if (!item_id && !has_index) b = codex_last_toolcall(st->msg);
            if (b) agentc_msg_block_append(b, delta, n);
        }
    } else if (agentc_streq(type, "response.completed")) {
        AgcJson *resp = agentc_json_get(d, "response");
        const char *id = agentc_json_get_str(resp, "id");
        if (id && !st->response_id[0])
            agentc_snprintf(st->response_id, sizeof st->response_id, "%s", id);
        AgcJson *u = agentc_json_get(resp, "usage");
        if (agentc_json_type(u) == AGENTC_JSON_OBJ) map_usage(st, u);
        st->saw_stop = true;
    } else if (agentc_streq(type, "response.incomplete")) {
        /* The Responses API ends a truncated turn with `response.incomplete`
         * rather than `response.completed`; without this branch finish() would
         * report "stream ended before response.completed" and drop the usage. */
        AgcJson *resp = agentc_json_get(d, "response");
        const char *id = agentc_json_get_str(resp, "id");
        if (id && !st->response_id[0])
            agentc_snprintf(st->response_id, sizeof st->response_id, "%s", id);
        AgcJson *u = agentc_json_get(resp, "usage");
        if (agentc_json_type(u) == AGENTC_JSON_OBJ) map_usage(st, u);
        AgcJson *det = agentc_json_get(resp, "incomplete_details");
        const char *reason = det ? agentc_json_get_str(det, "reason") : NULL;
        if (reason && agentc_streq(reason, "max_output_tokens")) {
            st->stop_reason = AGENTC_STOP_LENGTH;
        } else {
            st->stop_reason = AGENTC_STOP_ERROR;
            agentc_snprintf(st->error, sizeof st->error, "response incomplete%s%s",
                            reason ? ": " : "", reason ? reason : "");
        }
        st->saw_stop = true;
    }

    return 0;
}

static int codex_finish(AgcStreamState *st) {
    if (!st) return -1;
    if (!st->saw_stop && st->stop_reason == AGENTC_STOP_PENDING) {
        agentc_snprintf(st->error, sizeof st->error, "stream ended before response.completed");
        st->stop_reason = AGENTC_STOP_ERROR;
        return -1;
    }
    if (st->stop_reason == AGENTC_STOP_PENDING) {
        st->stop_reason =
            agentc_msg_count_tool_calls(st->msg) ? AGENTC_STOP_TOOLUSE : AGENTC_STOP_STOP;
    }
    return st->stop_reason == AGENTC_STOP_ERROR ? -1 : 0;
}

static int codex_stream_open(AgcStreamState *st) {
    (void)codex_priv(st);
    return 0;
}

static void codex_stream_close(AgcStreamState *st) {
    CodexPriv *pv = st->priv;
    if (pv) {
        for (size_t i = 0; i < pv->ncalls; i++) {
            agentc_free(pv->calls[i].item_id);
            agentc_free(pv->calls[i].call_id);
        }
        agentc_free(pv);
    }
    st->priv = NULL;
}

AgcProviderOps agentc_openai_codex_ops = {
    .name = "openai",
    .api = "openai-codex-responses",
    .path = "/responses",
    .default_base_url = "https://chatgpt.com/backend-api/codex",
    .env_keys = { "OPENAI_API_KEY", NULL, NULL },
    .needs_key = 1,
    .discover_style = AGENTC_DISCOVER_CODEX,
    .max_tokens_key = "max_output_tokens",
    .build_request = codex_build,
    .map_sse = codex_map,
    .finish = codex_finish,
    .stream_open = codex_stream_open,
    .stream_close = codex_stream_close,
    .auth_headers = codex_auth_headers,
    .handle = NULL,
};

const AgcProvider *agentc_prov_openai_codex(void) {
    return agentc_provider_handle(&agentc_openai_codex_ops);
}
