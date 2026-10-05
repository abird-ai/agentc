/* google.c — native Google Gemini (Generative Language API) adapter.
 *
 * Unlike the `gemini` OpenAI-compatible preset, this speaks the native
 * generateContent/streamGenerateContent protocol: systemInstruction, contents
 * with model functionCall parts, functionResponse tool results, and a
 * generationConfig that carries the thinking budget. The model is part of the
 * request path, so the row's path is `/models/{model}:streamGenerateContent`
 * with `alt=sse` for server-sent events; core's provider_path() substitutes
 * {model}.
 *
 * Streaming: every SSE `data:` payload is a full GenerateContentResponse. Text
 * parts append to the answer, `thought: true` parts to the thinking block, and
 * functionCall parts become tool-call blocks (the synthetic id is only for the
 * transcript; Gemini resolves results by function name).
 */
#include "agent.h"
#include "prov/provider.h"

/* internal helpers from messages.c (not part of the frozen header) */
AgcBlock *agentc_msg_block_new(AgcMsg *m, int type);
void agentc_msg_block_append(AgcBlock *b, const char *p, size_t n);
void agentc_msg_add_tool_call(AgcMsg *m, const char *id, const char *name);
size_t agentc_msg_count_tool_calls(const AgcMsg *m);

/* Native thinking budgets mirror anthropic.c: a low/medium/high level. `off`
 * leaves the model's own default in place (Gemini 2.5 Pro cannot disable
 * thinking with budget 0, and 3.x uses a different field); the TUI hides the
 * reasoning block while the level is off, so the transcript stays answer-only. */
static i64 thinking_budget(int level) {
    switch (level) {
    case 1: return 1024;
    case 2: return 2048;
    case 3: return 8192;
    case 4: return 24576;
    default: return 0;
    }
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
    if (r->api_key && r->api_key[0]) {
        agentc_buf_cstr(out, "x-goog-api-key: ");
        append_header_value(out, r->api_key);
        agentc_buf_cstr(out, "\r\n");
    }
    agentc_buf_cstr(out, "accept: text/event-stream\r\n\r\n");
}

/* Discovery auth (src/core/discover.c): the native API takes x-goog-api-key
 * instead of a Bearer token, so the row owns its discovery header. */
static void google_auth_headers(const AgcProviderOps *self, AgcBuf *out, const char *api_key) {
    (void)self;
    if (!api_key || !api_key[0]) return;
    agentc_buf_cstr(out, "x-goog-api-key: ");
    append_header_value(out, api_key);
    agentc_buf_cstr(out, "\r\n");
}

static bool assistant_has_content(const AgcMsg *m) {
    if (agentc_msg_count_tool_calls(m)) return true;
    for (size_t i = 0; i < m->nblocks; i++)
        if (m->blocks[i].type == AGENTC_BLK_TEXT && m->blocks[i].text_len) return true;
    return false;
}

static void write_user_parts(AgcJsonW *w, const AgcMsg *m) {
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "role");
    agentc_jsonw_cstr(w, "user");
    agentc_jsonw_key(w, "parts");
    agentc_jsonw_arr(w);
    bool any = false;
    for (size_t i = 0; i < m->nblocks; i++) {
        if (m->blocks[i].type != AGENTC_BLK_TEXT) continue;
        agentc_jsonw_obj(w);
        agentc_jsonw_key(w, "text");
        agentc_jsonw_str(w, m->blocks[i].text ? m->blocks[i].text : "",
                         m->blocks[i].text_len);
        agentc_jsonw_end(w);
        any = true;
    }
    if (!any) {
        /* Gemini rejects an empty parts array. */
        agentc_jsonw_obj(w);
        agentc_jsonw_key(w, "text");
        agentc_jsonw_cstr(w, "");
        agentc_jsonw_end(w);
    }
    agentc_jsonw_end(w);   /* parts */
    agentc_jsonw_end(w);   /* role object */
}

static void write_model_parts(AgcJsonW *w, const AgcMsg *m) {
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "role");
    agentc_jsonw_cstr(w, "model");
    agentc_jsonw_key(w, "parts");
    agentc_jsonw_arr(w);
    for (size_t i = 0; i < m->nblocks; i++) {
        const AgcBlock *b = &m->blocks[i];
        if (b->type == AGENTC_BLK_TEXT) {
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "text");
            agentc_jsonw_str(w, b->text ? b->text : "", b->text_len);
            agentc_jsonw_end(w);
        } else if (b->type == AGENTC_BLK_TOOLCALL) {
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "functionCall");
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "name");
            agentc_jsonw_cstr(w, b->tool_name ? b->tool_name : "");
            agentc_jsonw_key(w, "args");
            if (b->tool_args && b->tool_args[0])
                agentc_jsonw_raw(w, b->tool_args, agentc_strlen(b->tool_args));
            else
                agentc_jsonw_raw(w, "{}", 2);
            agentc_jsonw_end(w);   /* functionCall */
            agentc_jsonw_end(w);   /* part */
        }
        /* thinking blocks are not replayed */
    }
    agentc_jsonw_end(w);   /* parts */
    agentc_jsonw_end(w);   /* role object */
}

static void write_tool_response(AgcJsonW *w, const AgcMsg *m) {
    const AgcBlock *res = m->nblocks ? &m->blocks[0] : NULL;
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "functionResponse");
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "name");
    agentc_jsonw_cstr(w, res && res->tool_name ? res->tool_name : "");
    agentc_jsonw_key(w, "response");
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "result");
    agentc_jsonw_str(w, res && res->text ? res->text : "", res && res->text ? res->text_len : 0);
    agentc_jsonw_end(w);   /* response */
    agentc_jsonw_end(w);   /* functionResponse */
    agentc_jsonw_end(w);   /* part */
}

static void write_contents(AgcJsonW *w, const AgcTranscript *t) {
    agentc_jsonw_arr(w);
    for (size_t i = 0; i < t->n; i++) {
        const AgcMsg *m = &t->msgs[i];
        if (m->role == AGENTC_ROLE_USER) {
            write_user_parts(w, m);
        } else if (m->role == AGENTC_ROLE_ASSISTANT) {
            if (!agentc_provider_msg_serializable(m) || !assistant_has_content(m)) continue;
            write_model_parts(w, m);
        } else if (m->role == AGENTC_ROLE_TOOL) {
            /* group consecutive tool results into one user turn, matching the
             * order of the preceding model functionCall parts */
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "role");
            agentc_jsonw_cstr(w, "user");
            agentc_jsonw_key(w, "parts");
            agentc_jsonw_arr(w);
            while (i < t->n && t->msgs[i].role == AGENTC_ROLE_TOOL) {
                write_tool_response(w, &t->msgs[i]);
                i++;
            }
            i--;
            agentc_jsonw_end(w);   /* parts */
            agentc_jsonw_end(w);   /* role object */
        }
        /* system messages live in the top-level systemInstruction field */
    }
    agentc_jsonw_end(w);
}

static void write_tools(AgcJsonW *w, const AgcRequest *r) {
    agentc_jsonw_key(w, "tools");
    agentc_jsonw_arr(w);
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "functionDeclarations");
    agentc_jsonw_arr(w);
    for (size_t i = 0; i < r->ntools; i++) {
        const AgcTool *t = &r->tools[i];
        if (t->flags & AGENTC_TOOL_HIDDEN) continue;
        agentc_jsonw_obj(w);
        agentc_jsonw_key(w, "name");
        agentc_jsonw_cstr(w, t->name);
        agentc_jsonw_key(w, "description");
        agentc_jsonw_cstr(w, t->desc ? t->desc : "");
        agentc_jsonw_key(w, "parameters");
        if (t->params_json && t->params_json[0])
            agentc_jsonw_raw(w, t->params_json, agentc_strlen(t->params_json));
        else
            agentc_jsonw_raw(w, "{\"type\":\"object\"}", sizeof("{\"type\":\"object\"}") - 1);
        agentc_jsonw_end(w);
    }
    agentc_jsonw_end(w);   /* functionDeclarations */
    agentc_jsonw_end(w);   /* tools[0] */
    agentc_jsonw_end(w);   /* tools */
}

static int google_build(AgcBuf *out, const AgcRequest *r, const char *url_host,
                        const char *url_path) {
    (void)url_host;
    (void)url_path;
    write_headers(out, r);

    i64 max_tokens = r->max_tokens;
    const AgcModel *model = agentc_model_find(r->provider, r->model);
    if (max_tokens <= 0) max_tokens = model ? (i64)model->max_tokens : 8192;
    i64 budget = thinking_budget(r->thinking_level);
    /* maxOutputTokens covers the visible answer and the thought budget. */
    if (budget > 0 && max_tokens < budget + 1024) max_tokens = budget + 1024;

    AgcJsonW w;
    agentc_jsonw_init(&w, out);
    agentc_jsonw_obj(&w);

    if (r->system && r->system[0]) {
        agentc_jsonw_key(&w, "systemInstruction");
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "parts");
        agentc_jsonw_arr(&w);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "text");
        agentc_jsonw_cstr(&w, r->system);
        agentc_jsonw_end(&w);
        agentc_jsonw_end(&w);
        agentc_jsonw_end(&w);
    }

    agentc_jsonw_key(&w, "contents");
    const AgcTranscript *tr = r->transcript;
    if (tr) {
        write_contents(&w, tr);
    } else {
        agentc_jsonw_arr(&w);
        agentc_jsonw_end(&w);
    }

    if (agentc_request_visible_tools(r)) write_tools(&w, r);

    agentc_jsonw_key(&w, "generationConfig");
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "maxOutputTokens");
    agentc_jsonw_i64(&w, max_tokens);
    if (budget > 0) {
        agentc_jsonw_key(&w, "thinkingConfig");
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "thinkingBudget");
        agentc_jsonw_i64(&w, budget);
        agentc_jsonw_key(&w, "includeThoughts");
        agentc_jsonw_bool(&w, true);
        agentc_jsonw_end(&w);
    }
    agentc_jsonw_end(&w);   /* generationConfig */

    agentc_jsonw_end(&w);
    return 0;
}

/* ------------------------------------------------------------- SSE map */

static void set_err(AgcStreamState *st, const char *s) {
    st->stop_reason = AGENTC_STOP_ERROR;
    agentc_snprintf(st->error, sizeof st->error, "%s", s ? s : "provider error");
}

static int map_finish_reason(AgcStreamState *st, const char *s) {
    if (!s || !s[0]) return AGENTC_STOP_PENDING;
    if (agentc_streq(s, "STOP"))
        return agentc_msg_count_tool_calls(st->msg) ? AGENTC_STOP_TOOLUSE
                                                    : AGENTC_STOP_STOP;
    if (agentc_streq(s, "MAX_TOKENS")) return AGENTC_STOP_LENGTH;
    return AGENTC_STOP_ERROR;
}

static AgcBlock *last_block(AgcMsg *m, int type) {
    for (size_t i = m->nblocks; i > 0; i--)
        if (m->blocks[i - 1].type == type) return &m->blocks[i - 1];
    return NULL;
}

static void append_to_type(AgcMsg *m, int type, const char *s, size_t n) {
    if (!s || n == 0) return;
    AgcBlock *b = last_block(m, type);
    if (!b) b = agentc_msg_block_new(m, type);
    agentc_msg_block_append(b, s, n);
}

static void append_function_call(AgcStreamState *st, const AgcJson *call) {
    const char *name = agentc_json_get_str(call, "name");
    char id[48];
    agentc_snprintf(id, sizeof id, "call_%zu", agentc_msg_count_tool_calls(st->msg));
    agentc_msg_add_tool_call(st->msg, id, name);
    AgcBlock *b = &st->msg->blocks[st->msg->nblocks - 1];
    const AgcJson *args = agentc_json_get(call, "args");
    AgcBuf ab = { 0 };
    AgcJsonW aw;
    agentc_jsonw_init(&aw, &ab);
    if (args) agentc_json_emit(&aw, args);
    else agentc_jsonw_raw(&aw, "{}", 2);
    if (ab.p) agentc_msg_block_append(b, (const char *)ab.p, ab.len);
    agentc_buf_free(&ab);
}

static int google_map(AgcStreamState *st, const AgcSseEvent *ev) {
    if (!st || !ev) return -1;
    if (!ev->data || ev->data_len == 0) return 0;
    if (ev->data_len == 6 && agentc_memeq(ev->data, "[DONE]", 6)) return 0;
    if (!st->arena) st->arena = agentc_json_arena_new(0);
    AgcJson *d = agentc_json_parse_in(st->arena, ev->data, ev->data_len);
    if (!d) {
        set_err(st, "invalid SSE JSON");
        return -1;
    }

    AgcJson *err = agentc_json_get(d, "error");
    if (err) {
        const char *msg = agentc_json_get_str(err, "message");
        set_err(st, msg && msg[0] ? msg : "provider error");
        return 0;
    }

    const char *mv = agentc_json_get_str(d, "modelVersion");
    if (mv && !st->response_id[0])
        agentc_snprintf(st->response_id, sizeof st->response_id, "%s", mv);

    AgcJson *cands = agentc_json_get(d, "candidates");
    AgcJson *cand = agentc_json_at(cands, 0);
    if (cand) {
        AgcJson *content = agentc_json_get(cand, "content");
        AgcJson *parts = agentc_json_get(content, "parts");
        size_t np = agentc_json_len(parts);
        for (size_t i = 0; i < np; i++) {
            AgcJson *part = agentc_json_at(parts, i);
            size_t tlen = 0;
            const char *text = agentc_json_str(agentc_json_get(part, "text"), &tlen);
            if (text && tlen) {
                bool thought = agentc_json_get_bool(part, "thought", false);
                append_to_type(st->msg, thought ? AGENTC_BLK_THINK : AGENTC_BLK_TEXT, text, tlen);
            }
            AgcJson *call = agentc_json_get(part, "functionCall");
            if (call) append_function_call(st, call);
        }
        const char *fr = agentc_json_get_str(cand, "finishReason");
        if (fr && fr[0]) {
            st->saw_stop = true;
            st->stop_reason = map_finish_reason(st, fr);
            if (st->stop_reason == AGENTC_STOP_ERROR && !st->error[0]) {
                const char *fm = agentc_json_get_str(cand, "finishMessage");
                agentc_snprintf(st->error, sizeof st->error, "%s",
                                fm && fm[0] ? fm : fr);
            }
        }
    }

    AgcJson *u = agentc_json_get(d, "usageMetadata");
    if (u) {
        st->usage_input = (u32)agentc_json_get_int(u, "promptTokenCount", st->usage_input);
        st->usage_output =
            (u32)agentc_json_get_int(u, "candidatesTokenCount", st->usage_output);
        st->usage_reasoning = (u32)agentc_json_get_int(u, "thoughtsTokenCount", 0);
        st->usage_cache_read = (u32)agentc_json_get_int(u, "cachedContentTokenCount", 0);
    }

    AgcJson *pf = agentc_json_get(d, "promptFeedback");
    const char *br = pf ? agentc_json_get_str(pf, "blockReason") : NULL;
    if (br && br[0]) {
        agentc_snprintf(st->error, sizeof st->error, "blocked: %s", br);
        st->stop_reason = AGENTC_STOP_ERROR;
        st->saw_stop = true;
    }
    return 0;
}

static int google_finish(AgcStreamState *st) {
    if (!st) return -1;
    if (!st->saw_stop && st->stop_reason == AGENTC_STOP_PENDING) {
        /* the connection dropped before a finishReason: never accept it */
        set_err(st, "stream ended before a finish reason");
        return -1;
    }
    if (st->stop_reason == AGENTC_STOP_PENDING) {
        st->stop_reason = agentc_msg_count_tool_calls(st->msg) ? AGENTC_STOP_TOOLUSE
                                                               : AGENTC_STOP_STOP;
    }
    return st->stop_reason == AGENTC_STOP_ERROR ? -1 : 0;
}

AgcProviderOps agentc_google_ops = {
    .name = "google",
    .api = "google-generativeai",
    .path = "/models/{model}:streamGenerateContent?alt=sse",
    .default_base_url = "https://generativelanguage.googleapis.com/v1beta",
    .env_keys = { "GEMINI_API_KEY", "GOOGLE_API_KEY", "GOOGLE_GENERATIVEAI_API_KEY" },
    .needs_key = 1,
    .discover_style = AGENTC_DISCOVER_GOOGLE,
    .max_tokens_key = "maxOutputTokens",
    .build_request = google_build,
    .map_sse = google_map,
    .finish = google_finish,
    .stream_open = NULL,
    .stream_close = NULL,
    .auth_headers = google_auth_headers,
    .handle = NULL,
};

const AgcProvider *agentc_prov_google(void) {
    return agentc_provider_handle(&agentc_google_ops);
}
