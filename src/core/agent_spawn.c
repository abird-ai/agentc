/* agent_spawn.c — the op-based `agent` tool (P6-D).
 *
 * One model-facing tool mirroring the agent actor kind (R-D26): spawn | wait |
 * send | interrupt | freeze | resume | result | list. The calling actor is
 * `call->actor_id`; the tool lives in agent_spawn.c so agent.c keeps the
 * constructor/loop. Depth, authority and envelope are enforced here because the
 * `tool` gate is the universal path (P6-HOOKS §5).
 */
#include "agent.h"
#include "agent_int.h"
#include "base/limits.h"
#include "config.h"
#include "core/tools/args.h"
#include "plat.h"

static bool as_bool_default(const AgcToolArgs *a, const char *key, bool dflt) {
    return agentc_tool_args_bool(a, key, dflt);
}

/* R-D31/C-D3: the user cycle is `turn`; `step` is the model cycle. */
static const char *scope_name(int scope) {
    return scope == AGENTC_MSG_SCOPE_TURN ? "turn" : "actor";
}

static void emit_agent_error(AgcBuf *out, const char *why, bool *is_error) {
    agentc_buf_printf(out, "error: agent: %s", why);
    if (is_error)
        *is_error = true;
}

int agent_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error) {
    (void)self;
    if (is_error)
        *is_error = false;
    AgcToolArgs a;
    if (agentc_tool_args_parse(&a, call->args_json,
                               call->args_json ? agentc_strlen(call->args_json) : 0) != 0)
        return -12;
    if (a.root == NULL) {
        emit_agent_error(out, "arguments must be a JSON object", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }
    const char *op = agentc_tool_args_str(&a, "op");
    if (op == NULL) {
        agentc_tool_args_missing(out, "agent", "op", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }

    /* The calling actor: the agent that owns this tool call. */
    AgcAgent *caller = call->actor_id != 0 ? agentc_agent_find(call->actor_id) : NULL;

    if (agentc_streq(op, "list")) {
        size_t total = agentc_agent_list(NULL, 0);
        AgcAgentInfo info[AGENTC_ACTOR_MAX];
        size_t n = total > AGENTC_ACTOR_MAX ? AGENTC_ACTOR_MAX : total;
        (void)agentc_agent_list(info, n);
        for (size_t i = 0; i < n; i++) {
            agentc_buf_printf(out, "%u\t%u\t%u\t%s\t%s\t%s\n", (unsigned)info[i].id,
                              (unsigned)info[i].parent_id, (unsigned)info[i].depth,
                              agentc_actor_state_name((AgcActorState)info[i].state),
                              info[i].model ? info[i].model : "", info[i].task ? info[i].task : "");
        }
        if (n == 0)
            agentc_buf_cstr(out, "(no agents)\n");
        agentc_tool_args_free(&a);
        return 0;
    }

    if (agentc_streq(op, "spawn")) {
        if (caller == NULL) {
            emit_agent_error(out, "no calling agent", is_error);
            agentc_tool_args_free(&a);
            return 0;
        }
        const char *task = agentc_tool_args_str(&a, "task");
        if (task == NULL) {
            agentc_tool_args_missing(out, "agent", "task", is_error);
            agentc_tool_args_free(&a);
            return 0;
        }
        int max_depth = agentc_config_subagent_max_depth();
        if ((int)agentc_agent_depth(caller) + 1 >= max_depth) {
            emit_agent_error(out, "depth_limit", is_error);
            agentc_tool_args_free(&a);
            return 0;
        }
        AgcTrustEnv parsed;
        bool parsed_ok = false;
        const char *trust_json = agentc_tool_args_str(&a, "trust");
        if (trust_json != NULL) {
            char err[128] = {0};
            agentc_memset(&parsed, 0, sizeof parsed);
            if (agentc_trust_from_json(trust_json, agentc_strlen(trust_json), &parsed, err,
                                       sizeof err) != 0) {
                emit_agent_error(out, "invalid trust envelope", is_error);
                agentc_tool_args_free(&a);
                return 0;
            }
            parsed_ok = true;
        }
        bool wait = as_bool_default(&a, "wait", true);
        bool inherit_ctx = as_bool_default(&a, "inherit_context", false);
        AgcAgentSpec spec;
        agentc_memset(&spec, 0, sizeof spec);
        spec.parent = caller;
        spec.task = task;
        spec.provider = agentc_tool_args_str(&a, "provider");
        spec.model = agentc_tool_args_str(&a, "model");
        spec.thinking = agentc_json_get(a.root, "thinking")
                            ? (int)agentc_tool_args_int(&a, "thinking", -1)
                            : -1;
        spec.trust = parsed_ok ? &parsed : NULL;
        spec.flags = (wait ? AGENTC_AGENT_F_WAIT : AGENTC_AGENT_F_BACKGROUND) |
                     (inherit_ctx ? AGENTC_AGENT_F_INHERIT_CONTEXT : 0);
        spec.label = agentc_tool_args_str(&a, "label");
        AgcAgent *child = agentc_agent_create(&spec);
        if (parsed_ok)
            agentc_trust_env_free(&parsed);
        if (child == NULL) {
            emit_agent_error(out, "spawn failed", is_error);
            agentc_tool_args_free(&a);
            return 0;
        }
        AgcActorId cid = agentc_agent_actor(child);
        if (!wait) {
            agentc_buf_printf(out, "{\"agent_id\":%u}", (unsigned)cid);
            agentc_tool_args_free(&a);
            return 0;
        }
        AgcBuf res = {0};
        int wrc = agentc_agent_wait(cid, 0, &res);
        if (wrc == 0 && res.p != NULL) {
            agentc_buf_push(out, res.p, res.len);
        } else {
            AgcAgentInfo info;
            if (agentc_agent_info(cid, &info) == 0 && info.state == AGENTC_ACTOR_FAILED)
                emit_agent_error(out, "child failed", is_error);
            else if (wrc < 0 && wrc != -61)
                emit_agent_error(out, "wait failed", is_error);
            /* no result (e.g. empty final text) is an empty tool result */
        }
        agentc_buf_free(&res);
        agentc_tool_args_free(&a);
        return 0;
    }

    AgcActorId id = (AgcActorId)agentc_tool_args_int(&a, "agent_id", 0);
    if (id == 0 || agentc_agent_find(id) == NULL) {
        emit_agent_error(out, "actor_not_found", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }

    if (agentc_streq(op, "wait") || agentc_streq(op, "result")) {
        AgcBuf res = {0};
        i64 timeout = agentc_tool_args_int(&a, "timeout_ms", 0);
        int rc = agentc_streq(op, "wait") ? agentc_agent_wait(id, timeout, &res)
                                          : agentc_agent_result(id, &res);
        if (rc == -110) {
            emit_agent_error(out, "wait timeout", is_error);
        } else if (rc == -61) {
            emit_agent_error(out, "no result", is_error);
        } else if (rc < 0) {
            emit_agent_error(out, "result unavailable", is_error);
        } else if (res.p != NULL) {
            agentc_buf_push(out, res.p, res.len);
        }
        agentc_buf_free(&res);
        agentc_tool_args_free(&a);
        return 0;
    }

    if (agentc_streq(op, "send")) {
        const char *text = agentc_tool_args_str(&a, "text");
        if (text == NULL) {
            agentc_tool_args_missing(out, "agent", "text", is_error);
            agentc_tool_args_free(&a);
            return 0;
        }
        const char *cid = agentc_tool_args_str(&a, "client_id");
        uint32_t fl = (uint32_t)agentc_tool_args_int(&a, "flags", 0) |
                      (agentc_tool_args_bool(&a, "dedupe", false) ? AGENTC_Q_F_DEDUPE : 0u);
        bool deduped =
            cid && cid[0] && (fl & AGENTC_Q_F_DEDUPE) && agentc_queue_has_client(id, cid);
        int rc = agentc_agent_send(id, text, agentc_tool_args_int(&a, "turn", 0),
                                   (int)agentc_tool_args_int(&a, "priority", 0), fl, cid);
        if (rc == -1)
            emit_agent_error(out, "authority_denied", is_error);
        else if (rc == -28)
            emit_agent_error(out, "queue_full", is_error);
        else if (rc < 0)
            emit_agent_error(out, "send failed", is_error);
        else if (deduped)
            agentc_buf_cstr(out, "{\"deduped\":true}\n");
        else
            agentc_buf_cstr(out, "ok\n");
        agentc_tool_args_free(&a);
        return 0;
    }

    if (agentc_streq(op, "interrupt")) {
        const char *scope = agentc_tool_args_str(&a, "scope");
        int sc = (scope && agentc_streq(scope, "actor")) ? AGENTC_MSG_SCOPE_ACTOR
                                                         : AGENTC_MSG_SCOPE_TURN;
        int stage = (int)agentc_tool_args_int(&a, "stage", 1);
        int rc = agentc_agent_cancel(id, sc, stage);
        if (rc < 0)
            emit_agent_error(out, "interrupt failed", is_error);
        else
            agentc_buf_printf(out, "ok %s\n", scope_name(sc));
        agentc_tool_args_free(&a);
        return 0;
    }

    if (agentc_streq(op, "freeze") || agentc_streq(op, "resume")) {
        int rc = agentc_streq(op, "freeze") ? agentc_actor_freeze(id, AGENTC_FREEZE_ACTOR, 0)
                                            : agentc_actor_resume(id, AGENTC_FREEZE_ACTOR);
        if (rc < 0)
            emit_agent_error(out, "freeze/resume failed", is_error);
        else
            agentc_buf_cstr(out, "ok\n");
        agentc_tool_args_free(&a);
        return 0;
    }

    agentc_buf_printf(out, "error: agent: unknown op '%s'", op);
    if (is_error)
        *is_error = true;
    agentc_tool_args_free(&a);
    return 0;
}
