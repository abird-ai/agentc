/* mode_rpc.c — `--mode rpc`: JSONL commands on stdin, responses/events out.
 *
 * Commands: prompt, prompt_template, abort, get_state, set_model,
 * set_thinking_level, get_available_models, compact, bash, new_session,
 * get_messages. Each reply is
 *   {"type":"response","id":…,"success":true,"data":{…}}
 *   {"type":"response","id":…,"success":false,"error":"…"}
 * Agent events are emitted with the same schema as --mode json.
 *
 * I/O and setup are shared with the other modes (app/mode.h): while a prompt is
 * running the event callback drains available input, handles `abort` immediately
 * and queues the remaining commands for after the run.
 */
#include "app/mode.h"

#include "app/setup.h"
#include "base/limits.h"
#include "core/prompts.h"
#include "ext.h"
#include "plat.h"

static void rpc_event(void *ud, int ev, const void *data);

/* The shared ctx is the first member: agentc_mode_event() and the rebuild path
 * cast the callback userdata to AgcModeCtx. */
typedef struct {
    AgcModeCtx base;
    AgcModeConfig cfg;           /* mutable copy (set_model/thinking) */
    AgcBuf inbuf;                /* partial input line */
    AgcVec pending;              /* char* lines queued during a run */
    size_t pending_head;
    bool eof;
    bool running;
} RpcCtx;

/* RPC responses are ASCII-only JSON; provider request bodies do not go
 * through this writer. */
static void jsonw_out(AgcJsonW *w, AgcBuf *b) {
    agentc_jsonw_init(w, b);
    agentc_jsonw_set_ascii(w, true);
}

/* ------------------------------------------------------------- input */

/* Extract one complete line (without "\n" / "\r"). Returns an owned string. */
static char *extract_line(AgcBuf *in) {
    size_t i = 0;
    while (i < in->len && in->p[i] != '\n') i++;
    if (i >= in->len) return NULL;
    size_t n = i;
    char *line = agentc_strdup_len((const char *)in->p, n);
    if (n && line[n - 1] == '\r') line[--n] = 0;
    agentc_memmove(in->p, in->p + i + 1, in->len - i - 1);
    in->len -= i + 1;
    return line;
}

static void rpc_read_available(RpcCtx *r) {
    u8 tmp[4096];
    for (int i = 0; i < 64; i++) {
        int n = r->base.io->read(r->base.io->in_ud, tmp, sizeof tmp);
        if (n > 0) {
            agentc_buf_push(&r->inbuf, tmp, (size_t)n);
            continue;
        }
        if (n == 0) r->eof = true;
        return;
    }
}

/* --------------------------------------------------------- responses */

/* Render the request id as a raw JSON value (number/string/null). */
static void rpc_id_json(const AgcJson *req, AgcBuf *out) {
    AgcJsonW w;
    jsonw_out(&w, out);
    AgcJson *id = agentc_json_get(req, "id");
    if (agentc_json_type(id) == AGENTC_JSON_NUM) {
        size_t n = 0;
        const char *s = agentc_json_num(id, &n);
        agentc_jsonw_raw(&w, s, n);
    } else if (agentc_json_type(id) == AGENTC_JSON_STR) {
        size_t n = 0;
        const char *s = agentc_json_str(id, &n);
        agentc_jsonw_str(&w, s, n);
    } else {
        agentc_jsonw_null(&w);
    }
}

static void rpc_response_raw(RpcCtx *r, const char *id, size_t id_len, bool ok,
                              const char *data, size_t data_len, const char *error) {
    AgcBuf b = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "response");
    agentc_jsonw_key(&w, "id");
    if (id && id_len) agentc_jsonw_raw(&w, id, id_len);
    else agentc_jsonw_null(&w);
    agentc_jsonw_key(&w, "success");
    agentc_jsonw_bool(&w, ok);
    if (ok) {
        agentc_jsonw_key(&w, "data");
        if (data && data_len) agentc_jsonw_raw(&w, data, data_len);
        else agentc_jsonw_raw(&w, "{}", 2);
    } else {
        agentc_jsonw_key(&w, "error");
        agentc_jsonw_cstr(&w, error ? error : "error");
    }
    agentc_jsonw_end(&w);
    agentc_buf_byte(&b, '\n');
    agentc_mode_out(&r->base, b.p, b.len);
    agentc_buf_free(&b);
}

static void rpc_response(RpcCtx *r, const AgcJson *req, bool ok, const char *data,
                         size_t data_len, const char *error) {
    AgcBuf id = { 0 };
    rpc_id_json(req, &id);
    rpc_response_raw(r, (const char *)id.p, id.len, ok, data, data_len, error);
    agentc_buf_free(&id);
}

static bool get_thinking(const AgcJson *req, int *out) {
    AgcJson *v = agentc_json_get(req, "level");
    if (!v) v = agentc_json_get(req, "thinking_level");
    if (agentc_json_type(v) == AGENTC_JSON_NUM) {
        i64 n = agentc_json_get_int(req, agentc_json_get(req, "level") ? "level" : "thinking_level", 0);
        if (n < 0 || n > 4) return false;
        *out = (int)n;
        return true;
    }
    const char *s = agentc_json_get_str(req, "level");
    if (!s) s = agentc_json_get_str(req, "thinking_level");
    if (!s) return false;
    if (agentc_streq(s, "off")) *out = 0;
    else if (agentc_streq(s, "low")) *out = 1;
    else if (agentc_streq(s, "medium")) *out = 3;
    else if (agentc_streq(s, "high")) *out = 4;
    else return false;
    return true;
}

static void rpc_get_state(RpcCtx *r, const AgcJson *req) {
    AgcBuf d = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &d);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "provider");
    agentc_jsonw_cstr(&w, r->base.provider ? r->base.provider : "");
    agentc_jsonw_key(&w, "model");
    agentc_jsonw_cstr(&w, r->base.model ? r->base.model : "");
    agentc_jsonw_key(&w, "thinking_level");
    agentc_jsonw_i64(&w, r->cfg.thinking);
    agentc_jsonw_key(&w, "context_tokens");
    agentc_jsonw_u64(&w, agentc_agent_context_tokens(r->base.agent));
    agentc_jsonw_key(&w, "compacted");
    agentc_jsonw_bool(&w, agentc_agent_compacted(r->base.agent));
    const AgcTranscript *tp = agentc_agent_transcript(r->base.agent);
    agentc_jsonw_key(&w, "messages");
    agentc_jsonw_u64(&w, tp ? tp->n : 0);
    agentc_jsonw_key(&w, "session_id");
    agentc_jsonw_cstr(&w, r->base.session ? agentc_session_id(r->base.session) : "");
    agentc_jsonw_end(&w);
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_buf_free(&d);
}

static void rpc_available_models(RpcCtx *r, const AgcJson *req) {
    const AgcModel *all[32];
    size_t n = agentc_model_all(all, 32);
    AgcBuf d = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &d);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "models");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < n; i++) {
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "provider");
        agentc_jsonw_cstr(&w, all[i]->provider);
        agentc_jsonw_key(&w, "id");
        agentc_jsonw_cstr(&w, all[i]->id);
        agentc_jsonw_key(&w, "api");
        agentc_jsonw_cstr(&w, all[i]->api);
        agentc_jsonw_key(&w, "ctx_window");
        agentc_jsonw_u64(&w, all[i]->ctx_window);
        agentc_jsonw_key(&w, "max_tokens");
        agentc_jsonw_u64(&w, all[i]->max_tokens);
        agentc_jsonw_key(&w, "reasoning");
        agentc_jsonw_bool(&w, all[i]->reasoning);
        agentc_jsonw_key(&w, "image");
        agentc_jsonw_bool(&w, all[i]->image);
        agentc_jsonw_end(&w);
    }
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_buf_free(&d);
}

static void rpc_messages(RpcCtx *r, const AgcJson *req) {
    const AgcTranscript *tp = agentc_agent_transcript(r->base.agent);
    AgcBuf d = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &d);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "messages");
    agentc_jsonw_arr(&w);
    if (tp) {
        for (size_t i = 0; i < tp->n; i++) {
            AgcBuf mb = { 0 };
            agentc_mode_msg_json(&mb, &tp->msgs[i]);
            agentc_jsonw_raw(&w, (const char *)mb.p, mb.len);
            agentc_buf_free(&mb);
        }
    }
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_buf_free(&d);
}

static void rpc_bash(RpcCtx *r, const AgcJson *req) {
    /* `command` names the RPC command at the top level, so the shell command
     * arrives as `input`/`cmd` or inside an `args` object. */
    const char *cmd = agentc_json_get_str(req, "input");
    if (!cmd) cmd = agentc_json_get_str(req, "cmd");
    if (!cmd) {
        AgcJson *args = agentc_json_get(req, "args");
        if (agentc_json_type(args) == AGENTC_JSON_OBJ) cmd = agentc_json_get_str(args, "command");
    }
    if (!cmd) {
        rpc_response(r, req, false, NULL, 0, "bash: missing command");
        return;
    }
    i64 timeout = agentc_json_get_int(req, "timeout", 0);
    bool is_err = false;
    char *out = agentc_tool_bash(cmd, timeout, NULL, &is_err);
    AgcBuf d = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &d);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "output");
    agentc_jsonw_cstr(&w, out ? out : "");
    agentc_jsonw_key(&w, "is_error");
    agentc_jsonw_bool(&w, is_err);
    agentc_jsonw_end(&w);
    agentc_free(out);
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_buf_free(&d);
}

/* `session_before_switch` (override/first/replace/fail-closed): returns true
 * to cancel the pending switch. `{"cancel":true}`, a blocked/failed handler, a
 * handled result with no boolean decision, or a malformed `cancel` field all
 * cancel; a well-formed `{"cancel":false}` allows, whether or not the handler
 * claimed `handled`. Fail-closed is sticky: once any handler blocks/fails, an
 * earlier well-formed `{"cancel":false}` accumulated into `result_json` must
 * not re-open the switch. */
static bool session_switch_cancelled(void) {
    if (!agentc_ext_wants("session_before_switch")) return false;
    AgcExtResult sr = agentc_ext_emit("session_before_switch", "{\"reason\":\"new\"}");
    if (sr.blocked) {
        /* A failed/overrunning/malformed handler cancels unconditionally; the
         * accumulated result may still hold an earlier handler's decision. */
        agentc_free(sr.result_json);
        return true;
    }
    bool cancel = false;
    bool decision = false;
    if (sr.result_json) {
        AgcJsonArena *ja = agentc_json_arena_new(0);
        AgcJson *o = agentc_json_parse_in(ja, sr.result_json, agentc_strlen(sr.result_json));
        if (agentc_json_type(o) != AGENTC_JSON_OBJ) {
            cancel = true;   /* malformed decision */
        } else {
            const AgcJson *cv = agentc_json_get(o, "cancel");
            if (cv) {
                int ct = agentc_json_type(cv);
                if (ct != AGENTC_JSON_TRUE && ct != AGENTC_JSON_FALSE) {
                    cancel = true;   /* malformed decision */
                } else {
                    decision = true;
                    cancel = (ct == AGENTC_JSON_TRUE);
                }
            }
        }
        agentc_json_arena_free(ja);
    }
    if (sr.handled && !decision) cancel = true;   /* handled without a decision */
    agentc_free(sr.result_json);
    return cancel;
}

static void rpc_new_session(RpcCtx *r, const AgcJson *req) {
    if (!r->base.session) {
        rpc_response(r, req, false, NULL, 0, "session storage is not configured");
        return;
    }
    /* The switch is cancellable before any file is touched: a cancel leaves
     * the current session, its transcript and the extension context intact. */
    if (session_switch_cancelled()) {
        rpc_response(r, req, false, NULL, 0, "session switch cancelled by extension");
        return;
    }
    const char *old_path = agentc_session_path(r->base.session);
    char *previous = agentc_strdup(old_path ? old_path : "");
    agentc_session_close(r->base.session);
    r->base.session = agentc_session_new(&r->cfg.session);
    /* The new session owns persistence from here: re-publish the extension
     * context/entry sink and reinstall the ctx-bound observer before any
     * message can be appended. */
    agentc_mode_rebind_session(&r->base);
    /* clear the chat transcript for the new session */
    AgcTranscript empty;
    agentc_transcript_init(&empty);
    (void)agentc_agent_load(r->base.agent, &empty);
    agentc_transcript_free(&empty);
    r->base.flushed = 0;
    agentc_mode_emit_session_start(&r->base, "new", previous ? previous : "");
    agentc_free(previous);
    agentc_mode_write_session_header(&r->base);
    AgcBuf d = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &d);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "session_id");
    agentc_jsonw_cstr(&w, r->base.session ? agentc_session_id(r->base.session) : "");
    agentc_jsonw_end(&w);
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_buf_free(&d);
}

static void rpc_set_model(RpcCtx *r, const AgcJson *req) {
    const char *model = agentc_json_get_str(req, "model");
    if (!model || !model[0]) {
        rpc_response(r, req, false, NULL, 0, "set_model: missing model");
        return;
    }
    const char *prov = agentc_json_get_str(req, "provider");
    if (prov && !agentc_setup_provider(prov)) {
        rpc_response(r, req, false, NULL, 0, "set_model: unknown provider");
        return;
    }
    char *nm = agentc_strdup(model);
    char *np = prov ? agentc_strdup(prov) : NULL;
    if (!nm || (prov && !np)) {
        agentc_free(nm);
        agentc_free(np);
        rpc_response(r, req, false, NULL, 0, "set_model: out of memory");
        return;
    }
    agentc_free(r->base.model);
    r->base.model = nm;
    if (np) {
        agentc_free(r->base.provider);
        r->base.provider = np;
    }
    r->cfg.model = r->base.model;
    r->cfg.provider = r->base.provider;
    agentc_mode_rebuild_agent(&r->base);
    agentc_agent_set_events(r->base.agent, rpc_event, r);
    AgcBuf d = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &d);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "provider");
    agentc_jsonw_cstr(&w, r->base.provider);
    agentc_jsonw_key(&w, "model");
    agentc_jsonw_cstr(&w, r->base.model);
    agentc_jsonw_end(&w);
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_buf_free(&d);
}

static void rpc_compact(RpcCtx *r, const AgcJson *req) {
    /* agentc_agent_compact runs provider parsing in its own arena; snapshot
     * the response id first so the reply survives any error path. */
    AgcBuf id = { 0 };
    rpc_id_json(req, &id);
    int rc = agentc_agent_compact(r->base.agent);
    if (rc != 0) {
        rpc_response_raw(r, (const char *)id.p, id.len, false, NULL, 0,
                         agentc_agent_last_error(r->base.agent));
        agentc_buf_free(&id);
        return;
    }
    AgcBuf d = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &d);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "compacted");
    agentc_jsonw_bool(&w, agentc_agent_compacted(r->base.agent));
    agentc_jsonw_key(&w, "context_tokens");
    agentc_jsonw_u64(&w, agentc_agent_context_tokens(r->base.agent));
    agentc_jsonw_end(&w);
    rpc_response_raw(r, (const char *)id.p, id.len, true, (const char *)d.p, d.len, NULL);
    agentc_buf_free(&d);
    agentc_buf_free(&id);
}

static bool rpc_handle(RpcCtx *r, const char *line, size_t n) {
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *req = agentc_json_parse_in(ja, line, n);
    if (agentc_json_type(req) != AGENTC_JSON_OBJ) {
        AgcBuf b = { 0 };
        AgcJsonW w;
        jsonw_out(&w, &b);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, "response");
        agentc_jsonw_key(&w, "id");
        agentc_jsonw_null(&w);
        agentc_jsonw_key(&w, "success");
        agentc_jsonw_bool(&w, false);
        agentc_jsonw_key(&w, "error");
        agentc_jsonw_cstr(&w, "invalid JSON command");
        agentc_jsonw_end(&w);
        agentc_buf_byte(&b, '\n');
        agentc_mode_out(&r->base, b.p, b.len);
        agentc_buf_free(&b);
        agentc_json_arena_free(ja);
        return true;
    }
    const char *cmd = agentc_json_get_str(req, "command");
    if (!cmd) {
        rpc_response(r, req, false, NULL, 0, "missing command");
        agentc_json_arena_free(ja);
        return true;
    }
    if (agentc_streq(cmd, "prompt")) {
        const char *text = agentc_json_get_str(req, "text");
        if (!text) text = agentc_json_get_str(req, "message");
        if (!text) text = agentc_json_get_str(req, "prompt");
        if (!text) {
            rpc_response(r, req, false, NULL, 0, "prompt: missing text");
            agentc_json_arena_free(ja);
            return true;
        }
        char *copy = agentc_strdup(text);
        AgcBuf id = { 0 };
        rpc_id_json(req, &id);
        agentc_json_arena_free(ja);
        r->running = true;
        int rc = agentc_agent_submit(r->base.agent, copy);
        r->running = false;
        agentc_free(copy);
        rpc_response_raw(r, (const char *)id.p, id.len, rc == 0, NULL, 0,
                         rc == 0 ? NULL : agentc_agent_last_error(r->base.agent));
        agentc_buf_free(&id);
        return true;
    }
    if (agentc_streq(cmd, "prompt_template")) {
        const char *name = agentc_json_get_str(req, "name");
        if (!name || !name[0]) {
            rpc_response(r, req, false, NULL, 0, "prompt_template: missing name");
            agentc_json_arena_free(ja);
            return true;
        }
        const char *args = agentc_json_get_str(req, "args");
        char *text = agentc_prompts_expand(name, args ? args : "");
        if (!text) {
            rpc_response(r, req, false, NULL, 0, "prompt_template: unknown template");
            agentc_json_arena_free(ja);
            return true;
        }
        AgcBuf id = { 0 };
        rpc_id_json(req, &id);
        agentc_json_arena_free(ja);
        r->running = true;
        int rc = agentc_agent_submit(r->base.agent, text);
        r->running = false;
        agentc_free(text);
        rpc_response_raw(r, (const char *)id.p, id.len, rc == 0, NULL, 0,
                         rc == 0 ? NULL : agentc_agent_last_error(r->base.agent));
        agentc_buf_free(&id);
        return true;
    }
    if (agentc_streq(cmd, "abort")) {
        agentc_agent_abort(r->base.agent);
        rpc_response(r, req, true, NULL, 0, NULL);
    } else if (agentc_streq(cmd, "get_state")) {
        rpc_get_state(r, req);
    } else if (agentc_streq(cmd, "set_model")) {
        rpc_set_model(r, req);
    } else if (agentc_streq(cmd, "set_thinking_level")) {
        int level = 0;
        if (!get_thinking(req, &level)) {
            rpc_response(r, req, false, NULL, 0, "set_thinking_level: invalid level");
        } else {
            r->cfg.thinking = level;
            agentc_agent_set_thinking(r->base.agent, level);
            AgcBuf d = { 0 };
            AgcJsonW w;
            jsonw_out(&w, &d);
            agentc_jsonw_obj(&w);
            agentc_jsonw_key(&w, "thinking_level");
            agentc_jsonw_i64(&w, level);
            agentc_jsonw_end(&w);
            rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
            agentc_buf_free(&d);
        }
    } else if (agentc_streq(cmd, "get_available_models")) {
        rpc_available_models(r, req);
    } else if (agentc_streq(cmd, "compact")) {
        rpc_compact(r, req);
    } else if (agentc_streq(cmd, "bash")) {
        rpc_bash(r, req);
    } else if (agentc_streq(cmd, "new_session")) {
        rpc_new_session(r, req);
    } else if (agentc_streq(cmd, "get_messages")) {
        rpc_messages(r, req);
    } else {
        rpc_response(r, req, false, NULL, 0, "unknown command");
    }
    agentc_json_arena_free(ja);
    return true;
}

/* -------------------------------------------------------- command pump */

static void rpc_queue(RpcCtx *r, char *line) {
    *(char **)agentc_vec_push(&r->pending, sizeof(char *)) = line;
}

/* Drain input that is available right now. `abort` is handled immediately;
 * every other command is queued for after the current run. */
static void rpc_pump(RpcCtx *r) {
    AgcJsonArena *ja = agentc_json_arena_new(0);
    rpc_read_available(r);
    for (;;) {
        char *line = extract_line(&r->inbuf);
        if (!line) break;
        size_t n = agentc_strlen(line);
        if (!n) {
            agentc_free(line);
            continue;
        }
        AgcJson *req = agentc_json_parse_in(ja, line, n);
        const char *cmd = agentc_json_get_str(req, "command");
        if (cmd && agentc_streq(cmd, "abort")) {
            agentc_agent_abort(r->base.agent);
            rpc_response(r, req, true, NULL, 0, NULL);
            agentc_free(line);
        } else {
            rpc_queue(r, line);
        }
    }
    agentc_json_arena_free(ja);
}

static void rpc_event(void *ud, int ev, const void *data) {
    RpcCtx *r = ud;
    agentc_mode_event(ud, ev, data);
    if (r->running) rpc_pump(r);
}

static bool rpc_next_command(RpcCtx *r, char **line) {
    if (r->pending_head < r->pending.len) {
        *line = ((char **)r->pending.p)[r->pending_head++];
        return true;
    }
    for (;;) {
        char *l = extract_line(&r->inbuf);
        if (l) {
            *line = l;
            return true;
        }
        if (r->eof) return false;
        u8 tmp[4096];
        int n = r->base.io->read(r->base.io->in_ud, tmp, sizeof tmp);
        if (n > 0) {
            agentc_buf_push(&r->inbuf, tmp, (size_t)n);
            continue;
        }
        if (n == 0) {
            r->eof = true;
            continue;
        }
        if (n == -11) {
            /* Between reads: keep deferred extension work (MCP) moving while
             * the loop waits for the next command. */
            agentc_mode_pump(NULL, 0);
            os_sleep_ns(10 * 1000000);
            continue;
        }
        r->eof = true;
    }
}

int agentc_mode_rpc_run(AgcModeCtx *c) {
    if (!c || !c->agent || !c->mcfg || !c->io || !c->io->write) return -22;
    RpcCtx r;
    agentc_memset(&r, 0, sizeof r);
    r.base = *c;
    r.cfg = *c->mcfg;
    r.base.mcfg = &r.cfg;
    r.cfg.provider = r.base.provider;
    r.cfg.model = r.base.model;
    agentc_agent_set_events(r.base.agent, rpc_event, &r);
    agentc_mode_write_session_header(&r.base);

    char *line = NULL;
    while (rpc_next_command(&r, &line)) {
        size_t n = agentc_strlen(line);
        if (n) (void)rpc_handle(&r, line, n);
        agentc_free(line);
        line = NULL;
    }
    for (size_t i = r.pending_head; i < r.pending.len; i++)
        agentc_free(((char **)r.pending.p)[i]);
    agentc_vec_free(&r.pending);
    agentc_buf_free(&r.inbuf);
    r.base.mcfg = c->mcfg;   /* the mutable copy is stack-local */
    *c = r.base;
    /* `set_model`'s rebuild and `new_session`'s rebind bound the persistence
     * observer to this function's stack frame (`&r.base`); re-point it at the
     * caller's ctx before returning so a later transcript append cannot reach
     * a dangling frame. Reusing the shared binder keeps the default-observer
     * policy in mode.c as the single source of truth. */
    agentc_mode_rebind_session(c);
    return 0;
}
