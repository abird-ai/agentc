/* approval.c — the P6-C approval contract and its synchronous delivery forms.
 *
 * One JSON contract (P6-TRUST §7.1) with three backends: an interactive UI
 * callback, an external policy program (request on stdin, one response line on
 * stdout), and the extension `tool_approval` hook. Unknown/malformed/oversized
 * responses and a missed deadline are a DENY (fail-closed). Scopes memoize in
 * agentc_trust_scope_*; a deny is never cached.
 *
 * Single-threaded: a delivery that cannot answer immediately returns deny.
 * The front end may call agentc_approval_submit with a response at any time.
 */
#include "agent.h"

#include "base/limits.h"
#include "core/events.h"
#include "ext.h"
#include "plat.h"

/* --------------------------------------------------------------- state */
typedef struct {
    AgcApprovalRequest req; /* strings borrowed from the live call */
    bool used;
    bool answered;            /* an out-of-band answer is parked in `answer` */
    AgcApprovalResult answer; /* owned while `answered` */
} Pending;
static Pending g_pending[AGENTC_APPROVAL_MAX_PENDING];
static uint64_t g_next_id = 1;

static AgcApprovalAskFn g_ui;
static void *g_ui_ud;

typedef struct {
    char *id;
    char *kind;
    char *command;
} PolicyBackend;
static PolicyBackend g_policies[16];
static size_t g_npolicies;

uint64_t agentc_approval_next_id(void) {
    return g_next_id++;
}

void agentc_approval_set_ui(AgcApprovalAskFn fn, void *ud) {
    g_ui = fn;
    g_ui_ud = ud;
}

int agentc_approval_register_policy(const char *id, const char *kind, const char *command) {
    if (!id || !kind)
        return -22;
    for (size_t i = 0; i < g_npolicies; i++) {
        if (!agentc_streq(g_policies[i].id, id))
            continue;
        agentc_free(g_policies[i].kind);
        agentc_free(g_policies[i].command);
        g_policies[i].kind = agentc_strdup(kind);
        g_policies[i].command = command ? agentc_strdup(command) : NULL;
        return 0;
    }
    if (g_npolicies >= sizeof g_policies / sizeof g_policies[0])
        return -28; /* ENOSPC */
    g_policies[g_npolicies].id = agentc_strdup(id);
    g_policies[g_npolicies].kind = agentc_strdup(kind);
    g_policies[g_npolicies].command = command ? agentc_strdup(command) : NULL;
    g_npolicies++;
    return 0;
}

static const PolicyBackend *policy_find(const char *id) {
    if (!id)
        return NULL;
    for (size_t i = 0; i < g_npolicies; i++)
        if (agentc_streq(g_policies[i].id, id))
            return &g_policies[i];
    return NULL;
}

void agentc_approval_shutdown(void) {
    for (size_t i = 0; i < g_npolicies; i++) {
        agentc_free(g_policies[i].id);
        agentc_free(g_policies[i].kind);
        agentc_free(g_policies[i].command);
    }
    g_npolicies = 0;
    for (size_t i = 0; i < AGENTC_APPROVAL_MAX_PENDING; i++) {
        agentc_free((void *)g_pending[i].answer.reason);
        agentc_free((void *)g_pending[i].answer.args_json);
    }
    agentc_memset(g_pending, 0, sizeof g_pending);
    g_ui = NULL;
    g_ui_ud = NULL;
}

/* ------------------------------------------------------------- codec */

int agentc_approval_request_json(const AgcApprovalRequest *r, AgcBuf *out) {
    if (!r || !out)
        return -22;
    AgcJsonW w;
    agentc_jsonw_init(&w, out);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "approval_request");
    agentc_jsonw_key(&w, "id");
    agentc_jsonw_u64(&w, r->id);
    agentc_jsonw_key(&w, "agent_id");
    agentc_jsonw_u64(&w, r->actor_id);
    agentc_jsonw_key(&w, "parent_agent_id");
    agentc_jsonw_u64(&w, r->parent_agent_id);
    agentc_jsonw_key(&w, "depth");
    agentc_jsonw_u64(&w, r->depth);
    agentc_jsonw_key(&w, "tool");
    agentc_jsonw_cstr(&w, r->tool ? r->tool : "");
    agentc_jsonw_key(&w, "input");
    const char *in = r->input_json ? r->input_json : "{}";
    agentc_jsonw_raw(&w, in, agentc_strlen(in));
    agentc_jsonw_key(&w, "trigger");
    agentc_jsonw_cstr(&w, r->trigger ? r->trigger : "unknown");
    agentc_jsonw_key(&w, "options");
    agentc_jsonw_arr(&w);
    agentc_jsonw_cstr(&w, "allow");
    agentc_jsonw_cstr(&w, "deny");
    agentc_jsonw_cstr(&w, "ask");
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "reason_required");
    agentc_jsonw_bool(&w, true);
    agentc_jsonw_end(&w);
    return 0;
}

int agentc_approval_parse_response(const char *json, size_t n, AgcApprovalResult *out) {
    if (!out)
        return -22;
    agentc_memset(out, 0, sizeof *out);
    out->decision = AGENTC_DEC_DENY;
    out->scope = AGENTC_SCOPE_ONCE;
    if (!json || n == 0 || n > AGENTC_APPROVAL_MAX_BYTES)
        return 0;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *o = agentc_json_parse_in(ja, json, n);
    if (agentc_json_type(o) != AGENTC_JSON_OBJ) {
        agentc_json_arena_free(ja);
        return 0; /* malformed = deny */
    }
    out->id = (uint64_t)agentc_json_get_int(o, "id", 0);
    const char *d = agentc_json_get_str(o, "decision");
    if (d) {
        if (agentc_streq(d, "allow"))
            out->decision = AGENTC_DEC_ALLOW;
        else if (agentc_streq(d, "deny"))
            out->decision = AGENTC_DEC_DENY;
        else if (agentc_streq(d, "ask"))
            out->decision = AGENTC_DEC_ASK;
        else {
            agentc_json_arena_free(ja);
            return 0;
        }
    }
    const char *reason = agentc_json_get_str(o, "reason");
    if (reason)
        out->reason = agentc_strdup(reason);
    const char *scope = agentc_json_get_str(o, "scope");
    if (scope) {
        if (agentc_streq(scope, "once"))
            out->scope = AGENTC_SCOPE_ONCE;
        else if (agentc_streq(scope, "step"))
            out->scope = AGENTC_SCOPE_STEP;
        else if (agentc_streq(scope, "session"))
            out->scope = AGENTC_SCOPE_SESSION;
        else if (agentc_streq(scope, "project"))
            out->scope = AGENTC_SCOPE_PROJECT;
        else
            out->scope = AGENTC_SCOPE_ONCE;
    }
    AgcJson *args = agentc_json_get(o, "args");
    if (args && agentc_json_type(args) != AGENTC_JSON_NULL) {
        AgcBuf b = {0};
        AgcJsonW iw;
        agentc_jsonw_init(&iw, &b);
        agentc_json_emit(&iw, args);
        if (b.p)
            out->args_json = agentc_strdup((const char *)b.p);
        agentc_buf_free(&b);
    }
    agentc_json_arena_free(ja);
    return 0;
}

/* ---------------------------------------------------------- pending table */
static Pending *pending_find(uint64_t id) {
    for (size_t i = 0; i < AGENTC_APPROVAL_MAX_PENDING; i++)
        if (g_pending[i].used && g_pending[i].req.id == id)
            return &g_pending[i];
    return NULL;
}

/* Free a parked out-of-band answer (reason/args_json are owned while
 * `answered`) and clear the rest of the answer state. Called on cancel, on
 * slot release and on slot allocation so a reused slot never inherits a stale
 * verdict (finding 1). */
static void pending_clear_answer(Pending *p) {
    agentc_free((void *)p->answer.reason);
    agentc_free((void *)p->answer.args_json);
    agentc_memset(&p->answer, 0, sizeof p->answer);
    p->answered = false;
}

bool agentc_approval_is_pending(uint64_t id) {
    return pending_find(id) != NULL;
}

int agentc_approval_pending(const AgcApprovalRequest **out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < AGENTC_APPROVAL_MAX_PENDING; i++) {
        if (!g_pending[i].used)
            continue;
        if (out && n < max)
            out[n] = &g_pending[i].req;
        n++;
    }
    return (int)n;
}

int agentc_approval_cancel(uint64_t id, const char *reason) {
    Pending *p = pending_find(id);
    if (!p)
        return -2; /* ENOENT */
    (void)reason;
    /* S-fix: free a parked out-of-band answer before clearing the slot.
     * reason/args_json are owned by the parked AgcApprovalResult. */
    pending_clear_answer(p);
    p->used = false;
    return 0;
}

/* ------------------------------------------------------- program backend */
static int read_bounded(int fd, char *buf, size_t cap, int timeout_ms) {
    size_t total = 0;
    i64 deadline = os_now_ns(OS_CLOCK_MONOTONIC) + (i64)timeout_ms * 1000000;
    while (total + 1 < cap) {
        int n = os_read(fd, buf + total, cap - 1 - total);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n == 0)
            break;
        if (n == -11 /* EAGAIN */) {
            if (os_now_ns(OS_CLOCK_MONOTONIC) > deadline)
                return -110; /* ETIMEDOUT */
            /* R-D17: every blocking wait drives the one runtime driver, so a
             * child's notify/approval is serviced while a program delegate runs.
             * With no front-end pump installed this is a bounded tick slice. */
            agentc_pump(20);
            continue;
        }
        if (n == -4 /* EINTR */)
            continue;
        return n;
    }
    buf[total] = 0;
    return (int)total;
}

static int run_program(const PolicyBackend *pb, const AgcApprovalRequest *req, char *out,
                       size_t cap, int timeout_ms) {
    if (!pb->command || !pb->command[0])
        return -22;
    AgcBuf rq = {0};
    agentc_approval_request_json(req, &rq);
    int pin[2], pout[2];
    if (os_pipe(pin) < 0 || os_pipe(pout) < 0) {
        agentc_buf_free(&rq);
        return -5; /* EIO */
    }
    char err[128] = "";
    int pid = os_spawn_shell_group_sandboxed(NULL, pb->command, pin[0], pout[1], pout[1], NULL, err,
                                             sizeof err);
    os_close(pin[0]);
    os_close(pout[1]);
    if (pid < 0) {
        os_close(pin[1]);
        os_close(pout[0]);
        agentc_buf_free(&rq);
        return pid;
    }
    size_t off = 0;
    while (off < rq.len) {
        int n = os_write(pin[1], rq.p + off, rq.len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == -4)
            continue;
        break;
    }
    os_close(pin[1]);
    agentc_buf_free(&rq);
    int n = read_bounded(pout[0], out, cap, timeout_ms);
    os_close(pout[0]);
    /* Reap (bounded): the program has closed stdout, so it is exiting. Pump the
     * runtime each slice (R-D17) instead of a bare sleep. */
    for (int i = 0; i < 100; i++) {
        int st = os_wait(pid, true);
        if (st != -1)
            break;
        agentc_pump(2);
    }
    return n;
}

/* ------------------------------------------------------------- dispatch */
static void emit_event(int ev, uint64_t id, const AgcTrustEnv *env, const char *tool,
                       const char *trigger, const AgcApprovalResult *res) {
    AgcApprovalEvent e;
    agentc_memset(&e, 0, sizeof e);
    e.id = id;
    e.actor_id = env ? env->self : 0;
    e.tool = tool;
    e.trigger = trigger;
    e.decision = res ? res->decision : AGENTC_DEC_ALLOW;
    e.reason = res ? res->reason : NULL;
    e.scope = res ? res->scope : AGENTC_SCOPE_ONCE;
    e.policy_id = env ? env->policy_id : NULL;
    agentc_events_emit(NULL, ev, &e);
}

int agentc_approval_request(const AgcApprovalRequest *req, AgcApprovalResult *out) {
    if (!req || !out)
        return -22;
    agentc_memset(out, 0, sizeof *out);
    out->id = req->id;
    out->decision = AGENTC_DEC_DENY;
    out->scope = AGENTC_SCOPE_ONCE;

    /* deadline already passed */
    if (req->deadline_ns && os_now_ns(OS_CLOCK_MONOTONIC) > req->deadline_ns) {
        out->reason = agentc_strdup("approval_timeout");
        return 0;
    }

    /* park it so agentc_approval_submit/cancel can address it */
    Pending *slot = NULL;
    for (size_t i = 0; i < AGENTC_APPROVAL_MAX_PENDING; i++)
        if (!g_pending[i].used) {
            slot = &g_pending[i];
            break;
        }
    if (slot) {
        /* Finding 1 (VC1b): never inherit a parked verdict from a previous
         * request that used this slot. */
        pending_clear_answer(slot);
        slot->used = true;
        slot->req = *req;
    }

    emit_event(AGENTC_EV_APPROVAL_REQUEST, req->id, req->env, req->tool, req->trigger, NULL);

    char rbuf[AGENTC_APPROVAL_MAX_BYTES];
    bool answered = false;
    int timeout_ms = 120000;

    /* 1. named policy backend */
    if (req->env && req->env->policy_id) {
        const PolicyBackend *pb = policy_find(req->env->policy_id);
        if (pb && agentc_streq(pb->kind, "program")) {
            int n = run_program(pb, req, rbuf, sizeof rbuf, timeout_ms);
            if (n == -110) {
                out->reason = agentc_strdup("approval_timeout");
                answered = true;
            } else if (n > 0) {
                (void)agentc_approval_parse_response(rbuf, (size_t)n, out);
                answered = true;
            }
        }
    }

    /* 2. interactive UI */
    if (!answered && g_ui) {
        int rc = g_ui(g_ui_ud, req, out);
        if (rc == 0)
            answered = true;
    }

    /* 3. extension tool_approval hook */
    if (!answered && agentc_ext_wants("tool_approval")) {
        AgcBuf rq = {0};
        agentc_approval_request_json(req, &rq);
        AgcExtResult r = agentc_ext_emit("tool_approval", rq.p ? (const char *)rq.p : "{}");
        agentc_buf_free(&rq);
        if (r.result_json) {
            (void)agentc_approval_parse_response(r.result_json, agentc_strlen(r.result_json), out);
            answered = true;
        }
        agentc_free(r.result_json);
    }

    /* 4. async core `approval` hook. It is an ordinary hook chain, so a handler
     * may agentc_hook_branch(_self) a decider/model and park the request. The
     * request then drives the one runtime pump until an out-of-band answer
     * arrives through agentc_approval_submit, bounded by the same deadline; a
     * missing answer is the fail-closed deny. */
    if (!answered && agentc_hook_wants(AGENTC_HOOK_STAGE_APPROVAL)) {
        AgcBuf rq = {0};
        agentc_approval_request_json(req, &rq);
        AgcMessage hm;
        agentc_memset(&hm, 0, sizeof hm);
        hm.class = AGENTC_MSG_CLASS_HOOK;
        hm.kind = AGENTC_MSG_INPUT;
        hm.id = req->id;
        hm.actor_id = req->actor_id;
        hm.meta = req->trigger;
        hm.body = rq.p ? (const char *)rq.p : "{}";
        hm.body_len = rq.len;
        AgcMessage *hout = NULL;
        uint64_t hdefer = 0;
        int hrc = agentc_hook_run_defer(AGENTC_HOOK_STAGE_APPROVAL, &hm, &hout, &hdefer);
        if (hrc == -115 && hdefer != 0) {
            i64 deadline = os_now_ns(OS_CLOCK_MONOTONIC) + (i64)timeout_ms * 1000000;
            while (slot != NULL && !slot->answered && os_now_ns(OS_CLOCK_MONOTONIC) <= deadline)
                agentc_pump(10);
            if (slot != NULL && slot->answered) {
                out->decision = slot->answer.decision;
                out->scope = slot->answer.scope;
                out->id = slot->answer.id;
                agentc_free((void *)out->reason);
                out->reason = slot->answer.reason;
                agentc_free((void *)out->args_json);
                out->args_json = slot->answer.args_json;
                slot->answer.reason = NULL;
                slot->answer.args_json = NULL;
                answered = true;
            } else {
                out->reason = agentc_strdup("approval_timeout");
                answered = true;
            }
            agentc_hook_cancel(hdefer);
        } else if (hrc == 0 && hout != NULL && hout->body != NULL && hout->body != hm.body) {
            (void)agentc_approval_parse_response(hout->body, hout->body_len, out);
            answered = true;
        }
        agentc_buf_free(&rq);
    }

    if (slot) {
        /* Finding 1 (VC1b): the completion path transfers reason/args_json
         * into `out`; reset `answered`, `decision`, `scope` and `id` so the
         * next request that reuses this slot cannot adopt the old verdict. */
        pending_clear_answer(slot);
        slot->used = false;
    }

    if (!answered) {
        agentc_free((void *)out->reason);
        out->reason = agentc_strdup("approval required but no approver is configured");
        out->decision = AGENTC_DEC_DENY;
        emit_event(AGENTC_EV_APPROVAL_RESULT, req->id, req->env, req->tool, req->trigger, out);
        return 0;
    }
    /* VCF1-2: `ask` is advertised in the request but is not a terminal answer
     * here -- there is no further approver to consult. Accept only ALLOW/DENY
     * and normalize every other verdict (ASK and any out-of-range value) to
     * DENY, so a non-terminal answer can never reach the agent loop, which
     * gates only on DENY. */
    if (out->decision != AGENTC_DEC_ALLOW && out->decision != AGENTC_DEC_DENY) {
        agentc_free((void *)out->reason);
        out->reason = agentc_strdup("approver returned a non-terminal decision");
        out->decision = AGENTC_DEC_DENY;
    }
    emit_event(AGENTC_EV_APPROVAL_RESULT, req->id, req->env, req->tool, req->trigger, out);
    return 0;
}

int agentc_approval_submit(const char *response_json) {
    if (!response_json)
        return -22;
    AgcApprovalResult res;
    agentc_approval_parse_response(response_json, agentc_strlen(response_json), &res);
    /* The synchronous dispatch has already consumed any UI/program answer; a
     * submitted response is parked on the matching request so a pumping request
     * (the async `approval` hook) can consume it. It is never an answer to the
     * legacy synchronous forms, so an unmatched id stays -ENOENT. */
    Pending *p = res.id ? pending_find(res.id) : NULL;
    if (p == NULL && res.id == 0) {
        /* No id: an out-of-band answer with exactly one parked request targets
         * it (the decision-submit convention). */
        size_t n = 0;
        for (size_t i = 0; i < AGENTC_APPROVAL_MAX_PENDING; i++)
            if (g_pending[i].used) {
                n++;
                p = &g_pending[i];
            }
        if (n != 1)
            p = NULL;
    }
    if (!p) {
        agentc_free((void *)res.reason);
        agentc_free((void *)res.args_json);
        return -2; /* ENOENT: no pending request with that id */
    }
    agentc_free((void *)p->answer.reason);
    agentc_free((void *)p->answer.args_json);
    p->answer = res; /* ownership of reason/args_json transfers */
    p->answered = true;
    return 0;
}

/* ------------------------------------------------------------- the gate */
int agentc_trust_gate(const AgcTrustEnv *env, const AgcToolCall *call, AgcTrustDecision *out) {
    if (!out)
        return -22;
    agentc_trust_decide_flags(env, call ? call->name : NULL, call ? call->flags : 0,
                              call ? call->args_json : NULL, out);
    /* Process policy (P6 completion): a matching rule may only tighten a
     * default allow. A hard layer-2 deny/ask/delegate is never overridden, so
     * policy evaluation runs only when decide() said ALLOW. First-match-wins
     * is enforced inside agentc_policy_eval. */
    /* Findings 3/4 (VC1b): track whether `out->reason` is already an owned
     * heap copy. decide_flags leaves a borrowed literal (or NULL for ALLOW);
     * the policy block below allocates. Only copy a borrowed/NULL reason, and
     * free an owned one before replacing it, so neither path leaks and a
     * static ALLOW is never mislabeled "denied". */
    bool reason_owned = false;
    if (out->decision == AGENTC_DEC_ALLOW) {
        const AgcPolicy *pol = agentc_trust_policy();
        if (pol != NULL) {
            AgcTrustDecision pd;
            if (agentc_policy_eval(pol, call ? call->name : NULL, call ? call->args_json : NULL,
                                   &pd) == 0 &&
                pd.decision != AGENTC_DEC_ALLOW) {
                out->decision = pd.decision;
                out->scope = pd.scope;
                out->policy_id = pd.policy_id;
                out->reason = agentc_strdup(pd.reason ? pd.reason : "policy rule");
                reason_owned = true;
            }
        }
    }
    /* A static verdict carries a borrowed literal reason (trust.h); the gate
     * contract is that the caller may always free `out->reason`, so take an
     * owned copy on this path too. A borrowed reason freed by the agent loop is
     * allocator corruption (audit B1). */
    if (out->decision == AGENTC_DEC_ALLOW || out->decision == AGENTC_DEC_DENY) {
        if (!reason_owned) {
            const char *src =
                out->reason ? out->reason
                            : (out->decision == AGENTC_DEC_ALLOW ? "allowed" : "denied");
            out->reason = agentc_strdup(src);
        }
        return 0;
    }

    /* memoized verdict? only allow/narrow verdicts are cached */
    int scope = AGENTC_SCOPE_ONCE;
    int cached = agentc_trust_scope_get(env, call ? call->name : NULL,
                                        call ? call->args_json : NULL, &scope);
    if (cached >= 0) {
        out->decision = cached;
        out->scope = scope;
        if (reason_owned)
            agentc_free((void *)out->reason);
        out->reason = agentc_strdup("approval memoized");
        return 0;
    }

    AgcApprovalRequest req;
    agentc_memset(&req, 0, sizeof req);
    req.id = agentc_approval_next_id();
    req.actor_id = env ? env->self : 0;
    req.parent_agent_id = env ? env->parent : 0;
    req.depth = env ? env->depth : 0;
    req.tool = call ? call->name : NULL;
    req.input_json = call ? call->args_json : NULL;
    req.trigger = out->reason ? out->reason : "outside-roots";
    req.env = env;
    req.deadline_ns = os_now_ns(OS_CLOCK_MONOTONIC) + (i64)120000 * 1000000;

    AgcApprovalResult res;
    agentc_approval_request(&req, &res);

    /* Audit: hash the input, never store it raw. */
    {
        const char *in = (call && call->args_json) ? call->args_json : "{}";
        u64 h = 1469598103934665603ull;
        // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound)
        for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
            h ^= *p;
            h *= 1099511628211ull;
        }
        char abuf[512];
        agentc_snprintf(abuf, sizeof abuf,
                        "{\"type\":\"approval\",\"tool\":\"%s\",\"input_hash\":\"%016llx\","
                        "\"decision\":%d,\"reason\":\"%s\",\"scope\":%d}",
                        (call && call->name) ? call->name : "", (unsigned long long)h, res.decision,
                        res.reason ? res.reason : "", res.scope);
        agentc_trust_audit("approval", abuf);
    }

    out->decision = res.decision;
    out->scope = res.scope;
    if (reason_owned)
        agentc_free((void *)out->reason);
    if (res.reason)
        out->reason = agentc_strdup(res.reason);
    else
        out->reason = agentc_strdup("approved");
    if (res.args_json)
        out->args_json = agentc_strdup(res.args_json);
    /* Finding 2 (VC1b): never memoize a narrowed approval. The memo is keyed on
     * the original args and carries no narrowed args, so a later hit would
     * replay a bare ALLOW and discard the approver's narrowing. Re-ask instead
     * (fail closed). */
    if (res.decision == AGENTC_DEC_ALLOW && res.scope != AGENTC_SCOPE_ONCE &&
        res.args_json == NULL)
        agentc_trust_scope_put(env, call->name, call->args_json, res.scope, res.decision);
    agentc_free((void *)res.reason);
    agentc_free((void *)res.args_json);
    return 0;
}

/* Finding 6 (VC1b): AGENTC_TOOL_NO_APPROVAL opts a tool out of only the
 * approval branch, not out of the envelope or the process policy. Run the same
 * layer-2 decide and process-policy tightening as agentc_trust_gate, then
 * collapse a non-terminal ASK/DELEGATE to ALLOW instead of dispatching an
 * approver. A hard DENY is never relaxed. Always returns an owned reason.
 *
 * VCF1-1: the process policy runs whenever layer 2 did not already return a
 * hard DENY (`!= AGENTC_DEC_DENY`), not only on ALLOW. An ASK/DELEGATE from
 * layer 2 (e.g. a path outside the workspace under approval=ask/policy) must
 * still be checked by the policy, and a matching policy DENY wins; only the
 * remaining ASK/DELEGATE is then collapsed to ALLOW. */
int agentc_trust_decide_skip_approval(const AgcTrustEnv *e, const char *tool, uint32_t flags,
                                      const char *args_json, AgcTrustDecision *out) {
    if (!out)
        return -22;
    agentc_trust_decide_flags(e, tool, flags, args_json, out);
    bool reason_owned = false;
    if (out->decision != AGENTC_DEC_DENY) {
        const AgcPolicy *pol = agentc_trust_policy();
        if (pol != NULL) {
            AgcTrustDecision pd;
            if (agentc_policy_eval(pol, tool, args_json, &pd) == 0 &&
                pd.decision != AGENTC_DEC_ALLOW) {
                out->decision = pd.decision;
                out->scope = pd.scope;
                out->policy_id = pd.policy_id;
                out->reason = agentc_strdup(pd.reason ? pd.reason : "policy rule");
                reason_owned = true;
            }
        }
    }
    if (out->decision == AGENTC_DEC_ASK || out->decision == AGENTC_DEC_DELEGATE) {
        out->decision = AGENTC_DEC_ALLOW;
        if (!reason_owned)
            out->reason = agentc_strdup(out->reason ? out->reason : "allowed");
        return 0;
    }
    if (out->decision == AGENTC_DEC_ALLOW || out->decision == AGENTC_DEC_DENY) {
        if (!reason_owned) {
            const char *src =
                out->reason ? out->reason
                            : (out->decision == AGENTC_DEC_ALLOW ? "allowed" : "denied");
            out->reason = agentc_strdup(src);
        }
    }
    return 0;
}
