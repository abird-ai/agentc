/* rpc_dispatch.c — one command vocabulary for JSONL (--mode rpc) and framed
 * CBOR (serve). The framing-neutral sink receives one JSON record per call;
 * the wrapper adds its own transport. See rpc_dispatch.h. */
#include "app/rpc_dispatch.h"

#include "app/setup.h"
#include "base/limits.h"
#include "core/prompts.h"
#include "ext.h"
#include "settings.h"
#include "plat.h"

/* Acceptance payloads (A-D2). */
static const char RPC_STARTED[] = "{\"disposition\":\"started\"}";

/* `get_messages` paging (C2). The response never exceeds `limit` records; a
 * cursor is clamped to the transcript length, so a hostile cursor cannot force a
 * large allocation. */
#define RPC_MSG_DEFAULT_LIMIT 100u
#define RPC_MSG_MAX_LIMIT 1000u

void agentc_rpc_set_sink(AgcRpc *r, AgcRpcSink sink, void *ud) {
    if (!r)
        return;
    r->sink = sink;
    r->sink_ud = ud;
}

bool agentc_rpc_running(const AgcRpc *r) {
    return r && r->running;
}
size_t agentc_rpc_pending(const AgcRpc *r) {
    return r ? r->pending_count : 0;
}

/* Every response/event line goes to the sink (mode_out for JSONL, the
 * connection for serve). */
static void rpc_out(AgcRpc *r, const char *p, size_t n) {
    if (r->sink)
        r->sink(r->sink_ud, p, n);
    else
        agentc_mode_out(&r->base, p, n);
}

/* RPC responses are ASCII-only JSON; provider request bodies do not go
 * through this writer. */
static void jsonw_out(AgcJsonW *w, AgcBuf *b) {
    agentc_jsonw_init(w, b);
    agentc_jsonw_set_ascii(w, true);
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

/* Stable machine code for a failure text (A-D4). A dynamic/unmapped error
 * falls back to "error"; success is always "ok". */
const char *agentc_rpc_code_for(const char *error) {
    if (!error || !error[0])
        return "error";
    if (agentc_streq(error, "unknown command"))
        return "unknown_command";
    if (agentc_streq(error, "invalid JSON command"))
        return "invalid_json";
    if (agentc_streq(error, "missing command"))
        return "missing_command";
    if (agentc_streq(error, "prompt: missing text"))
        return "missing_text";
    if (agentc_streq(error, "prompt_template: missing name"))
        return "missing_name";
    if (agentc_streq(error, "prompt_template: unknown template"))
        return "unknown_template";
    if (agentc_streq(error, "set_thinking_level: invalid level"))
        return "invalid_level";
    if (agentc_streq(error, "set_model: missing model"))
        return "missing_field";
    if (agentc_streq(error, "set_model: unknown provider"))
        return "unknown_provider";
    if (agentc_streq(error, "busy: record too large"))
        return "busy";
    if (agentc_streq(error, "busy: pending queue full"))
        return "busy";
    if (agentc_streq(error, "busy: session registry full"))
        return "busy";
    if (agentc_streq(error, "not_found: no such session"))
        return "not_found";
    if (agentc_streq(error, "set_model: out of memory"))
        return "out_of_memory";
    if (agentc_streq(error, "out of memory"))
        return "out_of_memory";
    if (agentc_streq(error, "bash: missing command"))
        return "missing_field";
    if (agentc_streq(error, "session storage is not configured"))
        return "no_session";
    if (agentc_streq(error, "session switch cancelled by extension"))
        return "cancelled";
    if (agentc_streq(error, "new_session failed"))
        return "session_failed";
    if (agentc_streq(error, "adapter unavailable") ||
        agentc_streq(error, "actor_op: adapter unavailable"))
        return "adapter_unavailable";
    if (agentc_streq(error, "actor_op: missing op"))
        return "missing_field";
    if (agentc_streq(error, "actor_query: actor_not_found"))
        return "actor_not_found";
    if (agentc_streq(error, "actor_query: no result"))
        return "no_result";
    if (agentc_streq(error, "actor_query: failed"))
        return "failed";
    if (agentc_streq(error, "runtime: missing op"))
        return "missing_field";
    if (agentc_streq(error, "runtime: unknown op"))
        return "unknown_command";
    if (agentc_streq(error, "set_trust: missing patch"))
        return "missing_field";
    if (agentc_streq(error, "set_trust: widening denied"))
        return "denied";
    if (agentc_streq(error, "no such pending approval"))
        return "no_result";
    if (agentc_streq(error, "set_policy: missing id/kind"))
        return "missing_field";
    if (agentc_streq(error, "set_policy failed"))
        return "failed";
    if (agentc_streq(error, "decide: invalid response"))
        return "invalid_response";
    if (agentc_streq(error, "set_setting: missing ext/key") ||
        agentc_streq(error, "reset_setting: missing ext/key"))
        return "missing_field";
    if (agentc_streq(error, "set_setting failed") || agentc_streq(error, "reset_setting failed"))
        return "failed";
    return "error";
}

static void rpc_response_raw(AgcRpc *r, const char *id, size_t id_len, bool ok, const char *data,
                             size_t data_len, const char *error) {
    AgcBuf b = {0};
    AgcJsonW w;
    jsonw_out(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "response");
    agentc_jsonw_key(&w, "id");
    if (id && id_len)
        agentc_jsonw_raw(&w, id, id_len);
    else
        agentc_jsonw_null(&w);
    agentc_jsonw_key(&w, "command");
    if (r->cur_cmd)
        agentc_jsonw_cstr(&w, r->cur_cmd);
    else
        agentc_jsonw_null(&w);
    agentc_jsonw_key(&w, "success");
    agentc_jsonw_bool(&w, ok);
    agentc_jsonw_key(&w, "code");
    agentc_jsonw_cstr(&w, ok ? "ok" : agentc_rpc_code_for(error));
    if (ok) {
        agentc_jsonw_key(&w, "data");
        if (data && data_len)
            agentc_jsonw_raw(&w, data, data_len);
        else
            agentc_jsonw_raw(&w, "{}", 2);
    } else {
        agentc_jsonw_key(&w, "error");
        agentc_jsonw_cstr(&w, error ? error : "error");
    }
    agentc_jsonw_end(&w);
    agentc_buf_byte(&b, '\n');
    rpc_out(r, (const char *)b.p, b.len);
    agentc_buf_free(&b);
}

static void rpc_response(AgcRpc *r, const AgcJson *req, bool ok, const char *data, size_t data_len,
                         const char *error) {
    AgcBuf id = {0};
    rpc_id_json(req, &id);
    rpc_response_raw(r, (const char *)id.p, id.len, ok, data, data_len, error);
    agentc_buf_free(&id);
}

/* The dispatchable command vocabulary (also reported by `hello`). */
static const char *const rpc_commands[] = {
    "hello",
    "prompt",
    "prompt_template",
    "abort",
    "get_state",
    "set_model",
    "set_thinking_level",
    "get_available_models",
    "get_commands",
    "compact",
    "bash",
    "new_session",
    "get_messages",
    "queue",
    "actor_op",
    "actor_query",
    "runtime",
    "get_trust",
    "set_trust",
    "approve",
    "deny",
    "decide",
    "set_policy",
    "get_settings",
    "set_setting",
    "reset_setting",
};
#define RPC_COMMAND_COUNT (sizeof rpc_commands / sizeof rpc_commands[0])

static bool rpc_known_command(const char *name) {
    if (!name)
        return false;
    for (size_t i = 0; i < RPC_COMMAND_COUNT; i++)
        if (agentc_streq(name, rpc_commands[i]))
            return true;
    return false;
}

/* A-D1 dual discriminator: a `type` string naming a known command (except the
 * envelope words) is the name, else the `command` string is. */
static const char *rpc_command_name(const AgcJson *req) {
    const char *type = agentc_json_get_str(req, "type");
    if (type && !agentc_streq(type, "command") && !agentc_streq(type, "response") &&
        !agentc_streq(type, "ui_response") && rpc_known_command(type))
        return type;
    return agentc_json_get_str(req, "command");
}

static const char *thinking_name_of(int level) {
    switch (level) {
    case 0:
        return "off";
    case 1:
        return "low";
    case 3:
        return "medium";
    default:
        return "high";
    }
}

static bool thinking_from_name(const char *s, int *out) {
    if (!s)
        return false;
    if (agentc_streq(s, "off"))
        *out = 0;
    else if (agentc_streq(s, "low"))
        *out = 1;
    else if (agentc_streq(s, "medium"))
        *out = 3;
    else if (agentc_streq(s, "high"))
        *out = 4;
    else
        return false;
    return true;
}

/* A-D6: numeric 0/1/3/4 or the stable name; the unused numeric 2 and the names
 * `minimal`/`xhigh`/`max` are rejected. */
static bool get_thinking(const AgcJson *req, int *out) {
    static const char *const keys[] = {"thinking_level_name", "level", "thinking_level"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        AgcJson *v = agentc_json_get(req, keys[i]);
        if (!v)
            continue;
        int t = agentc_json_type(v);
        if (t == AGENTC_JSON_NUM) {
            i64 n = agentc_json_get_int(req, keys[i], 0);
            if (n == 0 || n == 1 || n == 3 || n == 4) {
                *out = (int)n;
                return true;
            }
            return false;
        }
        if (t == AGENTC_JSON_STR)
            return thinking_from_name(agentc_json_get_str(req, keys[i]), out);
        return false;
    }
    return false;
}

static void rpc_get_state(AgcRpc *r, const AgcJson *req) {
    AgcBuf d = {0};
    AgcJsonW w;
    jsonw_out(&w, &d);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "protocol");
    agentc_jsonw_cstr(&w, "agentc-rpc");
    agentc_jsonw_key(&w, "provider");
    agentc_jsonw_cstr(&w, r->base.provider ? r->base.provider : "");
    agentc_jsonw_key(&w, "model");
    agentc_jsonw_cstr(&w, r->base.model ? r->base.model : "");
    agentc_jsonw_key(&w, "thinking_level");
    agentc_jsonw_i64(&w, r->cfg.thinking);
    agentc_jsonw_key(&w, "thinking_level_name");
    agentc_jsonw_cstr(&w, agentc_agent_thinking(r->base.agent));
    agentc_jsonw_key(&w, "context_tokens");
    agentc_jsonw_u64(&w, agentc_agent_context_tokens(r->base.agent));
    agentc_jsonw_key(&w, "compacted");
    agentc_jsonw_bool(&w, agentc_agent_compacted(r->base.agent));
    const AgcTranscript *tp = agentc_agent_transcript(r->base.agent);
    agentc_jsonw_key(&w, "messages");
    agentc_jsonw_u64(&w, tp ? tp->n : 0);
    agentc_jsonw_key(&w, "session_id");
    agentc_jsonw_cstr(&w, r->base.session ? agentc_session_id(r->base.session) : "");
    agentc_jsonw_key(&w, "session_file");
    {
        const char *sf = r->base.session ? agentc_session_path(r->base.session) : NULL;
        if (sf)
            agentc_jsonw_cstr(&w, sf);
        else
            agentc_jsonw_null(&w);
    }
    agentc_jsonw_key(&w, "turn_no");
    agentc_jsonw_i64(&w, agentc_agent_turn_no(r->base.agent));
    const char *hold = NULL;
    bool steerable = agentc_agent_steerable(r->base.agent, &hold);
    agentc_jsonw_key(&w, "steerable");
    agentc_jsonw_bool(&w, steerable);
    if (hold) {
        agentc_jsonw_key(&w, "steer_hold_kind");
        agentc_jsonw_cstr(&w, hold);
    }
    agentc_jsonw_key(&w, "pending_approvals");
    agentc_jsonw_i64(&w, (i64)agentc_approval_pending(NULL, 0));
    agentc_jsonw_key(&w, "background_actors");
    {
        AgcActor rows[AGENTC_ACTOR_MAX];
        size_t an = agentc_actor_list(rows, AGENTC_ACTOR_MAX);
        i64 bg = 0;
        for (size_t i = 0; i < an; i++)
            if (rows[i].kind == AGENTC_ACTOR_AGENT && rows[i].parent != 0)
                bg++;
        agentc_jsonw_i64(&w, bg);
    }
    agentc_jsonw_key(&w, "is_streaming");
    agentc_jsonw_bool(&w, r->running);
    agentc_jsonw_key(&w, "pending_commands");
    agentc_jsonw_u64(&w, r->pending_count);
    agentc_jsonw_key(&w, "auto_compaction");
    agentc_jsonw_bool(&w, r->cfg.auto_compact);
    agentc_jsonw_end(&w);
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_buf_free(&d);
}

static void rpc_available_models(AgcRpc *r, const AgcJson *req) {
    /* Count first, then list every model: a single provider can exceed any fixed
     * buffer (A-D7). `truncated` is emitted only when the count-sized allocation
     * fails (the OOM last resort); there is no fixed 32-entry cap. */
    size_t total = agentc_model_all(NULL, 0);
    const AgcModel **all = total ? agentc_alloc(total * sizeof *all) : NULL;
    bool truncated = total != 0 && all == NULL;
    size_t n = all ? agentc_model_all(all, total) : 0;
    AgcBuf d = {0};
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
    agentc_jsonw_key(&w, "total");
    agentc_jsonw_u64(&w, total);
    if (truncated) {
        agentc_jsonw_key(&w, "truncated");
        agentc_jsonw_bool(&w, true);
    }
    agentc_jsonw_end(&w);
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_free(all);
    agentc_buf_free(&d);
}

static void rpc_hello(AgcRpc *r, const AgcJson *req) {
    static const char *const caps[] = {
        "acceptance",     "command_echo",  "machine_code",       "get_commands",
        "thinking_names", "agent_settled", "ui_fire_and_forget",
    };
    AgcBuf d = {0};
    AgcJsonW w;
    jsonw_out(&w, &d);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "protocol");
    agentc_jsonw_cstr(&w, "agentc-rpc");
    agentc_jsonw_key(&w, "version");
    agentc_jsonw_i64(&w, AGENTC_RPC_VERSION);
    agentc_jsonw_key(&w, "commands");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < RPC_COMMAND_COUNT; i++)
        agentc_jsonw_cstr(&w, rpc_commands[i]);
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "capabilities");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++)
        agentc_jsonw_cstr(&w, caps[i]);
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_buf_free(&d);
}

/* A-D8: prompts first (source = file/extension/MCP), then extension commands
 * (source = "command"). */
static void rpc_get_commands(AgcRpc *r, const AgcJson *req) {
    size_t np = agentc_prompts_list(NULL, 0);
    AgcPromptInfo *prompts = np ? agentc_alloc(np * sizeof *prompts) : NULL;
    np = prompts ? agentc_prompts_list(prompts, np) : 0;
    size_t nc = agentc_ext_commands(NULL, 0);
    AgcExtCommandInfo *cmds = nc ? agentc_alloc(nc * sizeof *cmds) : NULL;
    nc = cmds ? agentc_ext_commands(cmds, nc) : 0;
    AgcBuf d = {0};
    AgcJsonW w;
    jsonw_out(&w, &d);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "commands");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < np; i++) {
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "name");
        agentc_jsonw_cstr(&w, prompts[i].name ? prompts[i].name : "");
        agentc_jsonw_key(&w, "description");
        agentc_jsonw_cstr(&w, prompts[i].description ? prompts[i].description : "");
        agentc_jsonw_key(&w, "hint");
        agentc_jsonw_cstr(&w, prompts[i].hint ? prompts[i].hint : "");
        agentc_jsonw_key(&w, "source");
        agentc_jsonw_cstr(&w, prompts[i].source ? prompts[i].source : "");
        agentc_jsonw_end(&w);
    }
    for (size_t i = 0; i < nc; i++) {
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "name");
        agentc_jsonw_cstr(&w, cmds[i].name ? cmds[i].name : "");
        agentc_jsonw_key(&w, "description");
        agentc_jsonw_cstr(&w, cmds[i].description ? cmds[i].description : "");
        agentc_jsonw_key(&w, "hint");
        agentc_jsonw_cstr(&w, "");
        agentc_jsonw_key(&w, "source");
        agentc_jsonw_cstr(&w, "command");
        agentc_jsonw_end(&w);
    }
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_free(prompts);
    agentc_free(cmds);
    agentc_buf_free(&d);
}

static void rpc_messages(AgcRpc *r, const AgcJson *req) {
    const AgcTranscript *tp = agentc_agent_transcript(r->base.agent);
    size_t total = tp ? tp->n : 0;
    /* Paging (C2): `cursor` is a 0-based offset clamped to the transcript,
     * `limit` is clamped to 1..RPC_MSG_MAX_LIMIT. Without params a bounded
     * default page is returned; `total`/`next_cursor` let a client resync. */
    i64 cursor = agentc_json_get_int(req, "cursor", 0);
    if (cursor < 0)
        cursor = 0;
    i64 limit = agentc_json_get_int(req, "limit", (i64)RPC_MSG_DEFAULT_LIMIT);
    if (limit < 1)
        limit = 1;
    if (limit > (i64)RPC_MSG_MAX_LIMIT)
        limit = RPC_MSG_MAX_LIMIT;
    size_t start = (size_t)cursor;
    if (start > total)
        start = total;
    size_t end = start + (size_t)limit;
    if (end > total || end < start)
        end = total;
    AgcBuf d = {0};
    AgcJsonW w;
    jsonw_out(&w, &d);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "messages");
    agentc_jsonw_arr(&w);
    for (size_t i = start; i < end; i++) {
        AgcBuf mb = {0};
        agentc_mode_msg_json(&mb, &tp->msgs[i]);
        agentc_jsonw_raw(&w, (const char *)mb.p, mb.len);
        agentc_buf_free(&mb);
    }
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "total");
    agentc_jsonw_u64(&w, total);
    if (end < total) {
        agentc_jsonw_key(&w, "next_cursor");
        agentc_jsonw_u64(&w, end);
    }
    agentc_jsonw_end(&w);
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_buf_free(&d);
}

/* ------------------------------------------------------ P6-B RPC adapters */

static const AgcTool *rpc_find_tool(const char *name) {
    static AgcTool tools[64];
    size_t n = agentc_ext_tools(tools, 64);
    for (size_t i = 0; i < n; i++)
        if (tools[i].name && agentc_streq(tools[i].name, name))
            return &tools[i];
    return NULL;
}

static void rpc_tool(AgcRpc *r, const AgcJson *req, const char *tool_name) {
    AgcJson *args = agentc_json_get(req, "args");
    AgcBuf aj = {0};
    if (agentc_json_type(args) == AGENTC_JSON_OBJ) {
        AgcJsonW w;
        jsonw_out(&w, &aj);
        agentc_json_emit(&w, args);
    } else {
        agentc_buf_cstr(&aj, "{}");
    }
    const AgcTool *t = rpc_find_tool(tool_name);
    if (t == NULL || t->run == NULL) {
        rpc_response(r, req, false, NULL, 0, "adapter unavailable");
        agentc_buf_free(&aj);
        return;
    }
    AgcBuf out = {0};
    bool is_error = false;
    AgcToolCall call = {"rpc", tool_name, (const char *)aj.p, NULL, .flags = t->flags};
    (void)t->run(t, &call, &out, &is_error);
    agentc_buf_byte(&out, 0);
    if (is_error)
        rpc_response(r, req, false, NULL, 0, (const char *)out.p);
    else
        rpc_response(r, req, true, (const char *)out.p, out.len - 1, NULL);
    agentc_buf_free(&out);
    agentc_buf_free(&aj);
}

static void rpc_queue_cmd(AgcRpc *r, const AgcJson *req) {
    rpc_tool(r, req, "queue");
}

static void rpc_actor_op(AgcRpc *r, const AgcJson *req) {
    const char *op = agentc_json_get_str(req, "op");
    if (!op) {
        rpc_response(r, req, false, NULL, 0, "actor_op: missing op");
        return;
    }
    AgcJson *args = agentc_json_get(req, "args");
    AgcBuf aj = {0};
    AgcJsonW w;
    jsonw_out(&w, &aj);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "op");
    agentc_jsonw_cstr(&w, agentc_streq(op, "enqueue") ? "add" : op);
    uint32_t aid = (uint32_t)agentc_json_get_int(req, "actor_id", 0);
    if (aid != 0) {
        agentc_jsonw_key(&w, "actor_id");
        agentc_jsonw_u64(&w, aid);
    }
    if (agentc_json_type(args) == AGENTC_JSON_OBJ) {
        for (size_t i = 0;; i++) {
            const AgcJson *k = agentc_json_key_at(args, i);
            if (!k)
                break;
            size_t kn = 0;
            const char *ks = agentc_json_str(k, &kn);
            agentc_jsonw_key(&w, ks ? ks : "");
            agentc_json_emit(&w, agentc_json_val_at(args, i));
        }
    }
    agentc_jsonw_end(&w);
    const char *tool_name;
    if (agentc_streq(op, "enqueue")) {
        tool_name = "queue";
    } else if (agentc_streq(op, "spawn") && agentc_json_type(args) == AGENTC_JSON_OBJ &&
               agentc_json_get(args, "task") != NULL) {
        tool_name = "agent";
    } else if ((agentc_streq(op, "wait") || agentc_streq(op, "send") ||
                agentc_streq(op, "interrupt") || agentc_streq(op, "result") ||
                agentc_streq(op, "freeze") || agentc_streq(op, "resume") ||
                agentc_streq(op, "list")) &&
               aid != 0 && agentc_agent_find(aid) != NULL) {
        tool_name = "agent";
    } else {
        tool_name = "process";
    }
    const AgcTool *t = rpc_find_tool(tool_name);
    if (t == NULL || t->run == NULL) {
        rpc_response(r, req, false, NULL, 0, "actor_op: adapter unavailable");
        agentc_buf_free(&aj);
        return;
    }
    AgcBuf out = {0};
    bool is_error = false;
    AgcToolCall call = {"rpc", tool_name, (const char *)aj.p, NULL, .flags = t->flags};
    call.actor_id = agentc_agent_actor(r->base.agent);
    (void)t->run(t, &call, &out, &is_error);
    agentc_buf_byte(&out, 0);
    if (is_error)
        rpc_response(r, req, false, NULL, 0, (const char *)out.p);
    else
        rpc_response(r, req, true, (const char *)out.p, out.len - 1, NULL);
    agentc_buf_free(&out);
    agentc_buf_free(&aj);
}

static void rpc_actor_query_cmd(AgcRpc *r, const AgcJson *req) {
    AgcActorId id = (AgcActorId)agentc_json_get_int(req, "actor_id", 0);
    if (id == 0)
        id = agentc_agent_actor(r->base.agent);
    const char *what = agentc_json_get_str(req, "what");
    if (id == 0 || !agentc_actor_exists(id)) {
        rpc_response(r, req, false, NULL, 0, "actor_query: actor_not_found");
        return;
    }
    if (what && agentc_streq(what, "agents")) {
        size_t total = agentc_agent_list(NULL, 0);
        AgcAgentInfo info[AGENTC_ACTOR_MAX];
        size_t n = total > AGENTC_ACTOR_MAX ? AGENTC_ACTOR_MAX : total;
        (void)agentc_agent_list(info, n);
        AgcBuf out = {0};
        agentc_buf_cstr(&out, "[");
        for (size_t i = 0; i < n; i++) {
            if (i)
                agentc_buf_cstr(&out, ",");
            agentc_buf_printf(&out,
                              "{\"agent_id\":%u,\"parent_id\":%u,\"depth\":%u,"
                              "\"is_subagent\":%s,\"model\":\"%s\",\"provider\":\"%s\"}",
                              (unsigned)info[i].id, (unsigned)info[i].parent_id,
                              (unsigned)info[i].depth, info[i].is_subagent ? "true" : "false",
                              info[i].model ? info[i].model : "",
                              info[i].provider ? info[i].provider : "");
        }
        agentc_buf_cstr(&out, "]");
        rpc_response(r, req, true, (const char *)out.p, out.len, NULL);
        agentc_buf_free(&out);
        return;
    }
    if (what && agentc_streq(what, "persisted_agents")) {
        size_t pn = 0;
        const AgcSessionAgent *pa = agentc_mode_persisted_agents(&r->base, &pn);
        AgcBuf out = {0};
        agentc_buf_cstr(&out, "[");
        for (size_t i = 0; i < pn; i++) {
            if (i)
                agentc_buf_cstr(&out, ",");
            agentc_buf_printf(&out,
                              "{\"agent_id\":%u,\"parent_id\":%u,\"depth\":%u,"
                              "\"exit_code\":%d,\"op\":\"%s\",\"model\":\"%s\","
                              "\"provider\":\"%s\",\"persisted\":true}",
                              (unsigned)pa[i].id, (unsigned)pa[i].parent_id, (unsigned)pa[i].depth,
                              pa[i].exit_code, pa[i].op ? pa[i].op : "",
                              pa[i].model ? pa[i].model : "", pa[i].provider ? pa[i].provider : "");
        }
        agentc_buf_cstr(&out, "]");
        rpc_response(r, req, true, (const char *)out.p, out.len, NULL);
        agentc_buf_free(&out);
        return;
    }
    if (what && agentc_streq(what, "result")) {
        AgcBuf out = {0};
        int rc = agentc_agent_result(id, &out);
        if (rc == 0) {
            rpc_response(r, req, true, (const char *)out.p, out.len, NULL);
        } else {
            rpc_response(r, req, false, NULL, 0,
                         rc == -61 ? "actor_query: no result" : "actor_query: failed");
        }
        agentc_buf_free(&out);
        return;
    }
    if (what && agentc_streq(what, "queue")) {
        AgcBuf aj = {0};
        agentc_buf_printf(&aj, "{\"op\":\"list\",\"actor_id\":%u}", (unsigned)id);
        const AgcTool *t = rpc_find_tool("queue");
        AgcBuf out = {0};
        bool is_error = false;
        if (t && t->run) {
            AgcToolCall call = {"rpc", "queue", (const char *)aj.p, NULL, .flags = t->flags};
            (void)t->run(t, &call, &out, &is_error);
        }
        agentc_buf_byte(&out, 0);
        if (is_error)
            rpc_response(r, req, false, NULL, 0, (const char *)out.p);
        else
            rpc_response(r, req, true, (const char *)out.p, out.len - 1, NULL);
        agentc_buf_free(&out);
        agentc_buf_free(&aj);
        return;
    }
    AgcBuf d = {0};
    if (agentc_actor_query(id, &d) != 0) {
        rpc_response(r, req, false, NULL, 0, "actor_query: failed");
        agentc_buf_free(&d);
        return;
    }
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_buf_free(&d);
}

static void rpc_runtime_cmd(AgcRpc *r, const AgcJson *req) {
    const char *op = agentc_json_get_str(req, "op");
    if (!op) {
        rpc_response(r, req, false, NULL, 0, "runtime: missing op");
        return;
    }
    if (agentc_streq(op, "tick"))
        agentc_runtime_tick();
    else if (agentc_streq(op, "shutdown"))
        agentc_runtime_shutdown("rpc");
    else if (agentc_streq(op, "freeze"))
        agentc_runtime_freeze(0);
    else if (agentc_streq(op, "resume"))
        agentc_runtime_resume();
    else {
        rpc_response(r, req, false, NULL, 0, "runtime: unknown op");
        return;
    }
    rpc_response(r, req, true, NULL, 0, NULL);
}

/* ------------------------------------------------------------- P6-C trust */
static void rpc_get_trust(AgcRpc *r, const AgcJson *req) {
    AgcBuf b = {0};
    (void)agentc_trust_to_json(agentc_trust_current(), &b);
    rpc_response(r, req, true, b.p ? (const char *)b.p : "{}", b.len, NULL);
    agentc_buf_free(&b);
}

static void rpc_set_trust(AgcRpc *r, const AgcJson *req) {
    const char *patch = agentc_json_get_str(req, "patch");
    if (!patch) {
        rpc_response(r, req, false, NULL, 0, "set_trust: missing patch");
        return;
    }
    const AgcExtHost *h = agentc_ext_host();
    int rc = AGENTC_EXT_HOST_HAS(h, set_trust) ? h->set_trust(h, 0, patch) : -38;
    rpc_response(r, req, rc == 0, NULL, 0, rc == 0 ? NULL : "set_trust: widening denied");
}

static void rpc_approve_cmd(AgcRpc *r, const AgcJson *req, bool allow) {
    i64 id = agentc_json_get_int(req, "id", 0);
    const char *scope = agentc_json_get_str(req, "scope");
    const char *reason = agentc_json_get_str(req, "reason");
    if (!scope)
        scope = "once";
    AgcBuf b = {0};
    AgcJsonW w;
    jsonw_out(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "id");
    agentc_jsonw_u64(&w, (u64)id);
    agentc_jsonw_key(&w, "decision");
    agentc_jsonw_cstr(&w, allow ? "allow" : "deny");
    agentc_jsonw_key(&w, "scope");
    agentc_jsonw_cstr(&w, scope);
    if (reason) {
        agentc_jsonw_key(&w, "reason");
        agentc_jsonw_cstr(&w, reason);
    }
    agentc_jsonw_end(&w);
    int rc = agentc_approval_submit((const char *)b.p);
    agentc_buf_free(&b);
    rpc_response(r, req, rc == 0, NULL, 0, rc == 0 ? NULL : "no such pending approval");
}

static void rpc_set_policy(AgcRpc *r, const AgcJson *req) {
    const char *id = agentc_json_get_str(req, "id");
    const char *kind = agentc_json_get_str(req, "kind");
    const char *command = agentc_json_get_str(req, "command");
    if (!id || !kind) {
        rpc_response(r, req, false, NULL, 0, "set_policy: missing id/kind");
        return;
    }
    int rc = agentc_approval_register_policy(id, kind, command);
    rpc_response(r, req, rc == 0, NULL, 0, rc == 0 ? NULL : "set_policy failed");
}

static void rpc_bash(AgcRpc *r, const AgcJson *req) {
    /* `command` names the RPC command when it comes from `type`; with the
     * legacy bare shape the canonical parameter is `input` (pi's `command`
     * would collide with the discriminator). `cmd`/`args.command` stay. */
    const char *cmd = agentc_json_get_str(req, "input");
    if (!cmd)
        cmd = agentc_json_get_str(req, "cmd");
    if (!cmd) {
        /* Only a `type`-resolved request frees the top-level `command` for the
         * shell text; in the bare shape it is still the RPC command name. */
        const char *type = agentc_json_get_str(req, "type");
        if (type && agentc_streq(type, "bash"))
            cmd = agentc_json_get_str(req, "command");
    }
    if (!cmd) {
        AgcJson *args = agentc_json_get(req, "args");
        if (agentc_json_type(args) == AGENTC_JSON_OBJ)
            cmd = agentc_json_get_str(args, "command");
    }
    if (!cmd) {
        rpc_response(r, req, false, NULL, 0, "bash: missing command");
        return;
    }
    i64 timeout = agentc_json_get_int(req, "timeout", 0);
    bool is_err = false;
    char *out = agentc_tool_bash(cmd, timeout, NULL, &is_err);
    AgcBuf d = {0};
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

static void rpc_new_session(AgcRpc *r, const AgcJson *req) {
    if (r->session_new) {
        char id[64] = {0};
        const char *spec = agentc_json_get_str(req, "session");
        int rc = r->session_new(r->session_new_ud, spec, id);
        if (rc == -125) {
            rpc_response(r, req, false, NULL, 0, "session switch cancelled by extension");
            return;
        }
        if (rc == -28) { /* ENOSPC: the bounded session registry is full */
            rpc_response(r, req, false, NULL, 0, "busy: session registry full");
            return;
        }
        if (rc == -2) { /* ENOENT: requested session id does not exist */
            rpc_response(r, req, false, NULL, 0, "not_found: no such session");
            return;
        }
        if (rc != 0) {
            rpc_response(r, req, false, NULL, 0, "new_session failed");
            return;
        }
        AgcBuf d = {0};
        AgcJsonW w;
        jsonw_out(&w, &d);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "session_id");
        agentc_jsonw_cstr(&w, id);
        agentc_jsonw_end(&w);
        rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
        agentc_buf_free(&d);
        return;
    }
    if (!r->base.session) {
        rpc_response(r, req, false, NULL, 0, "session storage is not configured");
        return;
    }
    int rc = agentc_mode_new_session(&r->base);
    if (rc == -125) {
        rpc_response(r, req, false, NULL, 0, "session switch cancelled by extension");
        return;
    }
    if (rc != 0) {
        rpc_response(r, req, false, NULL, 0, "new_session failed");
        return;
    }
    /* The session header is part of the JSON/RPC stdout stream (the file's own
     * header is written by agentc_session_new); the TUI must never call this. */
    agentc_mode_write_session_header(&r->base);
    AgcBuf d = {0};
    AgcJsonW w;
    jsonw_out(&w, &d);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "session_id");
    agentc_jsonw_cstr(&w, r->base.session ? agentc_session_id(r->base.session) : "");
    agentc_jsonw_end(&w);
    rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
    agentc_buf_free(&d);
}

static void rpc_set_model(AgcRpc *r, const AgcJson *req) {
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
    agentc_agent_set_events(r->base.agent, r->event, r->event_ud);
    AgcBuf d = {0};
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

static void rpc_compact(AgcRpc *r, const AgcJson *req) {
    /* agentc_agent_compact runs provider parsing in its own arena; snapshot
     * the response id first so the reply survives any error path. */
    AgcBuf id = {0};
    rpc_id_json(req, &id);
    int rc = agentc_agent_compact(r->base.agent);
    if (rc != 0) {
        rpc_response_raw(r, (const char *)id.p, id.len, false, NULL, 0,
                         agentc_agent_last_error(r->base.agent));
        agentc_buf_free(&id);
        return;
    }
    AgcBuf d = {0};
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

static void rpc_get_settings(AgcRpc *r, const AgcJson *req) {
    const char *only = agentc_json_get_str(req, "ext");
    size_t n = agentc_settings_list(NULL, 0);
    AgcSettingInfo *info = agentc_alloc((n ? n : 1) * sizeof *info);
    n = agentc_settings_list(info, n);
    AgcBuf b = {0};
    AgcJsonW w;
    jsonw_out(&w, &b);
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < n; i++) {
        if (only && !agentc_streq(info[i].ext, only))
            continue;
        const char *cur = agentc_settings_get_json(info[i].ext, info[i].key);
        int scope = agentc_settings_scope_of(info[i].ext, info[i].key);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "ext");
        agentc_jsonw_cstr(&w, info[i].ext);
        agentc_jsonw_key(&w, "key");
        agentc_jsonw_cstr(&w, info[i].key);
        agentc_jsonw_key(&w, "label");
        agentc_jsonw_cstr(&w, info[i].label ? info[i].label : "");
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_i64(&w, info[i].type);
        agentc_jsonw_key(&w, "flags");
        agentc_jsonw_u64(&w, info[i].flags);
        agentc_jsonw_key(&w, "default");
        agentc_jsonw_raw(&w, info[i].default_json, agentc_strlen(info[i].default_json));
        agentc_jsonw_key(&w, "value");
        agentc_jsonw_raw(&w, cur ? cur : "null", agentc_strlen(cur ? cur : "null"));
        agentc_jsonw_key(&w, "scope");
        agentc_jsonw_i64(&w, scope);
        agentc_jsonw_end(&w);
    }
    agentc_jsonw_end(&w);
    rpc_response(r, req, true, (const char *)b.p, b.len, NULL);
    agentc_buf_free(&b);
    agentc_free(info);
}

static void rpc_set_setting(AgcRpc *r, const AgcJson *req) {
    const char *ext = agentc_json_get_str(req, "ext");
    const char *key = agentc_json_get_str(req, "key");
    if (!ext || !key) {
        rpc_response(r, req, false, NULL, 0, "set_setting: missing ext/key");
        return;
    }
    const AgcJson *val = agentc_json_get(req, "value");
    AgcBuf b = {0};
    if (val) {
        AgcJsonW w;
        jsonw_out(&w, &b);
        agentc_json_emit(&w, val);
    }
    int rc = agentc_settings_set(ext, key, b.p ? (const char *)b.p : "null");
    agentc_buf_free(&b);
    rpc_response(r, req, rc == 0, NULL, 0, rc == 0 ? NULL : "set_setting failed");
}

static void rpc_reset_setting(AgcRpc *r, const AgcJson *req) {
    const char *ext = agentc_json_get_str(req, "ext");
    const char *key = agentc_json_get_str(req, "key");
    if (!ext || !key) {
        rpc_response(r, req, false, NULL, 0, "reset_setting: missing ext/key");
        return;
    }
    int rc = agentc_settings_reset(ext, key);
    rpc_response(r, req, rc == 0, NULL, 0, rc == 0 ? NULL : "reset_setting failed");
}

/* ------------------------------------------------------------ UI (A-D11) */

/* Fire-and-forget ui_request. Methods: notify/setStatus/setTitle. */
static void rpc_ui_emit(AgcRpc *r, const char *method, const char *key, const char *value,
                        int level) {
    AgcBuf b = {0};
    AgcJsonW w;
    jsonw_out(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "ui_request");
    agentc_jsonw_key(&w, "id");
    agentc_jsonw_u64(&w, ++r->ui_seq);
    agentc_jsonw_key(&w, "method");
    agentc_jsonw_cstr(&w, method);
    if (agentc_streq(method, "notify")) {
        agentc_jsonw_key(&w, "message");
        agentc_jsonw_cstr(&w, value ? value : "");
        agentc_jsonw_key(&w, "level");
        agentc_jsonw_i64(&w, level);
    } else if (agentc_streq(method, "setStatus")) {
        agentc_jsonw_key(&w, "key");
        agentc_jsonw_cstr(&w, key ? key : "");
        agentc_jsonw_key(&w, "text");
        agentc_jsonw_cstr(&w, value ? value : "");
    } else { /* setTitle */
        agentc_jsonw_key(&w, "title");
        agentc_jsonw_cstr(&w, value ? value : "");
    }
    agentc_jsonw_end(&w);
    agentc_buf_byte(&b, '\n');
    rpc_out(r, (const char *)b.p, b.len);
    agentc_buf_free(&b);
}

static void rpc_ui_notify(void *ud, const char *message, int level) {
    rpc_ui_emit(ud, "notify", NULL, message, level);
}
static void rpc_ui_set_title(void *ud, const char *title) {
    rpc_ui_emit(ud, "setTitle", NULL, title, 0);
}
static void rpc_status_observer(void *ud, const char *key, const char *text) {
    rpc_ui_emit(ud, "setStatus", key, text, 0);
}

/* --------------------------------------------------------- settled event */

static int rpc_last_stop(const AgcRpc *r) {
    const AgcTranscript *tp = agentc_agent_transcript(r->base.agent);
    if (!tp)
        return AGENTC_STOP_STOP;
    for (size_t i = tp->n; i > 0; i--)
        if (tp->msgs[i - 1].role == AGENTC_ROLE_ASSISTANT)
            return tp->msgs[i - 1].stop_reason;
    return AGENTC_STOP_STOP;
}

/* A-D2: one outcome event per accepted run; a post-acceptance failure never
 * produces a second response. */
static void rpc_emit_settled(AgcRpc *r, int rc) {
    const char *err = agentc_agent_last_error(r->base.agent);
    int stop = rpc_last_stop(r);
    bool aborted =
        rc != 0 && ((err && agentc_streq(err, "aborted")) || stop == AGENTC_STOP_ABORTED);
    AgcBuf b = {0};
    AgcJsonW w;
    jsonw_out(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "agent_settled");
    agentc_jsonw_key(&w, "disposition");
    agentc_jsonw_cstr(&w, rc == 0 ? "completed" : (aborted ? "aborted" : "error"));
    agentc_jsonw_key(&w, "stop_reason");
    agentc_jsonw_cstr(&w, agentc_mode_stop_name(stop));
    agentc_jsonw_key(&w, "error");
    if (rc == 0)
        agentc_jsonw_null(&w);
    else
        agentc_jsonw_cstr(&w, err ? err : "error");
    agentc_jsonw_end(&w);
    agentc_buf_byte(&b, '\n');
    rpc_out(r, (const char *)b.p, b.len);
    agentc_buf_free(&b);
}

static void rpc_run_text(AgcRpc *r, char *text) {
    r->running = true;
    if (r->defer_run) {
        /* Start the run but do not drive it: the front end owns the loop and
         * rebinds its process globals per session slice. A begin failure is
         * terminal immediately (agent_settled). */
        int brc = agentc_agent_run_begin(r->base.agent, text);
        if (brc != 0) {
            r->running = false;
            rpc_emit_settled(r, brc);
        }
        return;
    }
    int rc = agentc_agent_submit(r->base.agent, text);
    r->running = false;
    rpc_emit_settled(r, rc);
}

bool agentc_rpc_run_step(AgcRpc *r) {
    if (r == NULL || !r->running)
        return false;
    int rc = agentc_agent_run_step(r->base.agent);
    if (rc == AGENTC_AGENT_RUN_YIELD)
        return true;
    r->running = false;
    rpc_emit_settled(r, rc);
    return false;
}

bool agentc_rpc_dispatch(AgcRpc *r, const char *line, size_t n, bool acked) {
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *req = agentc_json_parse_in(ja, line, n);
    if (agentc_json_type(req) != AGENTC_JSON_OBJ) {
        r->cur_cmd = NULL;
        rpc_response_raw(r, NULL, 0, false, NULL, 0, "invalid JSON command");
        agentc_json_arena_free(ja);
        return true;
    }
    /* A client dialog reply is parsed and dropped (dialogs deferred, A-D11). */
    const char *type = agentc_json_get_str(req, "type");
    if (type && agentc_streq(type, "ui_response")) {
        agentc_json_arena_free(ja);
        return true;
    }
    const char *cmd = rpc_command_name(req);
    r->cur_cmd = cmd;
    if (!cmd) {
        rpc_response(r, req, false, NULL, 0, "missing command");
        agentc_json_arena_free(ja);
        return true;
    }
    if (agentc_streq(cmd, "prompt")) {
        const char *text = agentc_json_get_str(req, "text");
        if (!text)
            text = agentc_json_get_str(req, "message");
        if (!text)
            text = agentc_json_get_str(req, "prompt");
        if (!text) {
            rpc_response(r, req, false, NULL, 0, "prompt: missing text");
            agentc_json_arena_free(ja);
            return true;
        }
        char *copy = agentc_strdup(text);
        if (!copy) {
            rpc_response(r, req, false, NULL, 0, "out of memory");
            agentc_json_arena_free(ja);
            return true;
        }
        if (!acked)
            rpc_response(r, req, true, RPC_STARTED, sizeof RPC_STARTED - 1, NULL);
        agentc_json_arena_free(ja);
        rpc_run_text(r, copy);
        agentc_free(copy);
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
        if (!acked)
            rpc_response(r, req, true, RPC_STARTED, sizeof RPC_STARTED - 1, NULL);
        agentc_json_arena_free(ja);
        rpc_run_text(r, text);
        agentc_free(text);
        return true;
    }
    if (agentc_streq(cmd, "abort")) {
        agentc_agent_abort(r->base.agent);
        rpc_response(r, req, true, NULL, 0, NULL);
    } else if (agentc_streq(cmd, "hello")) {
        rpc_hello(r, req);
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
            AgcBuf d = {0};
            AgcJsonW w;
            jsonw_out(&w, &d);
            agentc_jsonw_obj(&w);
            agentc_jsonw_key(&w, "thinking_level");
            agentc_jsonw_i64(&w, level);
            agentc_jsonw_key(&w, "thinking_level_name");
            agentc_jsonw_cstr(&w, thinking_name_of(level));
            agentc_jsonw_end(&w);
            rpc_response(r, req, true, (const char *)d.p, d.len, NULL);
            agentc_buf_free(&d);
        }
    } else if (agentc_streq(cmd, "get_available_models")) {
        rpc_available_models(r, req);
    } else if (agentc_streq(cmd, "get_commands")) {
        rpc_get_commands(r, req);
    } else if (agentc_streq(cmd, "compact")) {
        rpc_compact(r, req);
    } else if (agentc_streq(cmd, "bash")) {
        rpc_bash(r, req);
    } else if (agentc_streq(cmd, "new_session")) {
        rpc_new_session(r, req);
    } else if (agentc_streq(cmd, "get_messages")) {
        rpc_messages(r, req);
    } else if (agentc_streq(cmd, "queue")) {
        rpc_queue_cmd(r, req);
    } else if (agentc_streq(cmd, "actor_op")) {
        rpc_actor_op(r, req);
    } else if (agentc_streq(cmd, "actor_query")) {
        rpc_actor_query_cmd(r, req);
    } else if (agentc_streq(cmd, "runtime")) {
        rpc_runtime_cmd(r, req);
    } else if (agentc_streq(cmd, "get_trust")) {
        rpc_get_trust(r, req);
    } else if (agentc_streq(cmd, "set_trust")) {
        rpc_set_trust(r, req);
    } else if (agentc_streq(cmd, "approve")) {
        rpc_approve_cmd(r, req, true);
    } else if (agentc_streq(cmd, "deny")) {
        rpc_approve_cmd(r, req, false);
    } else if (agentc_streq(cmd, "decide")) {
        const char *resp = agentc_json_get_str(req, "response");
        if (resp == NULL)
            resp = agentc_json_get_str(req, "data");
        if (resp == NULL || agentc_decision_submit(resp) != 0)
            rpc_response(r, req, false, NULL, 0, "decide: invalid response");
        else
            rpc_response(r, req, true, "{}", 2, NULL);
    } else if (agentc_streq(cmd, "set_policy")) {
        rpc_set_policy(r, req);
    } else if (agentc_streq(cmd, "get_settings")) {
        rpc_get_settings(r, req);
    } else if (agentc_streq(cmd, "set_setting")) {
        rpc_set_setting(r, req);
    } else if (agentc_streq(cmd, "reset_setting")) {
        rpc_reset_setting(r, req);
    } else {
        rpc_response(r, req, false, NULL, 0, "unknown command");
    }
    agentc_json_arena_free(ja);
    return true;
}

/* ---------------------------------------------------------- public shims */

const char *agentc_rpc_command_name(const AgcJson *req) {
    return rpc_command_name(req);
}

void agentc_rpc_respond(AgcRpc *r, const AgcJson *req, const char *cmd, bool ok, const char *data,
                        size_t data_len, const char *error) {
    r->cur_cmd = cmd;
    rpc_response(r, req, ok, data, data_len, error);
}

void agentc_rpc_ui_install(AgcRpc *r, AgcRpcUiSaved *saved) {
    if (!r || !saved)
        return;
    agentc_ext_get_ui_sink(&saved->ui, &saved->ui_ud);
    AgcExtUiSink ui;
    agentc_memset(&ui, 0, sizeof ui);
    ui.notify = rpc_ui_notify;
    ui.set_title = rpc_ui_set_title;
    agentc_ext_set_ui_sink(&ui, r);
    agentc_ext_get_status_observer(&saved->so, &saved->so_ud);
    agentc_ext_set_status_observer(rpc_status_observer, r);
}

void agentc_rpc_ui_restore(AgcRpcUiSaved *saved) {
    if (!saved)
        return;
    agentc_ext_set_status_observer(saved->so, saved->so_ud);
    agentc_ext_set_ui_sink(&saved->ui, saved->ui_ud);
}
