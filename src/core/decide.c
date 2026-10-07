/* decide.c — the decision seam (P6-D; see .agents/plans/P6-DECIDERS.md).
 *
 * No new stage, primitive, or actor kind: a decider is a hook handler that
 * branches to a backend and returns a message. This file owns the decider
 * registry, the multi-effect record parser/validator/applier, memoization, and
 * the default controller/router handlers. A LOCAL decider with `command` set
 * returns it verbatim (a deterministic rule/embedding seam); a MODEL decider
 * branches to an ephemeral child on the shared transport via agentc_hook_branch.
 */
#include "agent.h"
#include "base/limits.h"
#include "config.h"
#include "core/events.h"
#include "ext.h"

struct AgcControllerCtx {
    AgcDeciderSpec spec;
    char *id, *intent, *provider, *model, *command, *system;
};

/* ------------------------------------------------------------- registry */
typedef struct {
    bool used;
    AgcDeciderSpec spec;
    char *id, *intent, *provider, *model, *command, *system;
} DeciderRec;

static DeciderRec g_deciders[AGENTC_DECIDER_MAX];
static size_t g_decider_count;

static const char *ddup(const char *s) {
    return s ? agentc_strdup(s) : NULL;
}

int agentc_decider_register(const AgcDeciderSpec *spec) {
    if (spec == NULL || spec->id == NULL)
        return -22;
    for (size_t i = 0; i < AGENTC_DECIDER_MAX; i++) {
        DeciderRec *r = &g_deciders[i];
        if (r->used && !agentc_streq(r->id, spec->id))
            continue;
        if (!r->used) {
            r->used = true;
            g_decider_count++;
        } else {
            agentc_free(r->id);
            agentc_free(r->intent);
            agentc_free(r->provider);
            agentc_free(r->model);
            agentc_free(r->command);
            agentc_free(r->system);
        }
        r->spec = *spec;
        r->id = (char *)ddup(spec->id);
        r->intent = (char *)ddup(spec->intent);
        r->provider = (char *)ddup(spec->provider);
        r->model = (char *)ddup(spec->model);
        r->command = (char *)ddup(spec->command);
        r->system = (char *)ddup(spec->system);
        return 0;
    }
    return -28; /* ENOSPC */
}

void agentc_decider_reset(void) {
    for (size_t i = 0; i < AGENTC_DECIDER_MAX; i++) {
        DeciderRec *r = &g_deciders[i];
        if (!r->used)
            continue;
        agentc_free(r->id);
        agentc_free(r->intent);
        agentc_free(r->provider);
        agentc_free(r->model);
        agentc_free(r->command);
        agentc_free(r->system);
    }
    agentc_memset(g_deciders, 0, sizeof g_deciders);
    g_decider_count = 0;
    agentc_decision_pending_reset();
}

size_t agentc_decider_count(void) {
    return g_decider_count;
}

const char *agentc_decider_verdict_name(int verdict) {
    switch (verdict) {
    case AGENTC_VERDICT_ALLOW:
        return "allow";
    case AGENTC_VERDICT_DENY:
        return "deny";
    case AGENTC_VERDICT_NARROW:
        return "narrow";
    case AGENTC_VERDICT_REPLACE:
        return "replace";
    default:
        return "unknown";
    }
}

/* ----------------------------------------------------------- memoization */
typedef struct {
    bool used;
    char *key;
    char *resp;
} Memo;

static Memo g_memo[AGENTC_DECISION_CACHE_MAX];
static int g_memo_count;

static u64 fnv1a(const char *p, size_t n) {
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static char *memo_get(const AgcDeciderSpec *s, const char *body, size_t n) {
    char key[160];
    agentc_snprintf(key, sizeof key, "%s|%s|%llx", s->id ? s->id : "", s->intent ? s->intent : "",
                    (unsigned long long)fnv1a(body, n));
    for (size_t i = 0; i < AGENTC_DECISION_CACHE_MAX; i++)
        if (g_memo[i].used && agentc_streq(g_memo[i].key, key))
            return g_memo[i].resp;
    return NULL;
}

static char *memo_put(const AgcDeciderSpec *s, const char *body, size_t n, const char *resp) {
    char key[160];
    agentc_snprintf(key, sizeof key, "%s|%s|%llx", s->id ? s->id : "", s->intent ? s->intent : "",
                    (unsigned long long)fnv1a(body, n));
    for (size_t i = 0; i < AGENTC_DECISION_CACHE_MAX; i++) {
        if (!g_memo[i].used) {
            g_memo[i].used = true;
            g_memo[i].key = agentc_strdup(key);
            g_memo[i].resp = agentc_strdup(resp);
            g_memo_count++;
            return g_memo[i].resp;
        }
        if (agentc_streq(g_memo[i].key, key)) {
            agentc_free(g_memo[i].resp);
            g_memo[i].resp = agentc_strdup(resp);
            return g_memo[i].resp;
        }
    }
    return NULL;
}

int agentc_decision_memo_count(void) {
    return g_memo_count;
}

/* ------------------------------------------------ out-of-band pending rows */
/* A parked async decision: the hook defer token to resume, the correlation id,
 * and the meta/actor the resumed controller needs. Bounded and fail-closed: a
 * full table refuses the async park. */
typedef struct {
    bool used;
    uint64_t token;
    uint64_t id;
    uint32_t actor_id;
    char *meta;
} PendingDecision;

static PendingDecision g_dpending[AGENTC_DECISION_PENDING_MAX];

void agentc_decision_pending_reset(void) {
    for (size_t i = 0; i < AGENTC_DECISION_PENDING_MAX; i++) {
        if (!g_dpending[i].used)
            continue;
        agentc_free(g_dpending[i].meta);
    }
    agentc_memset(g_dpending, 0, sizeof g_dpending);
}

static bool decision_pending_add(uint64_t token, const AgcMessage *req) {
    for (size_t i = 0; i < AGENTC_DECISION_PENDING_MAX; i++) {
        if (g_dpending[i].used)
            continue;
        g_dpending[i].used = true;
        g_dpending[i].token = token;
        g_dpending[i].id = req ? req->id : 0;
        g_dpending[i].actor_id = req ? req->actor_id : 0;
        g_dpending[i].meta = (req && req->meta) ? agentc_strdup(req->meta) : NULL;
        return true;
    }
    return false; /* bounded: the caller falls back to the sync deny */
}

int agentc_decision_pending_count(void) {
    int n = 0;
    for (size_t i = 0; i < AGENTC_DECISION_PENDING_MAX; i++)
        if (g_dpending[i].used)
            n++;
    return n;
}

/* ------------------------------------------------------- record parsing */
static char *json_dup(const AgcJson *v) {
    if (v == NULL)
        return NULL;
    AgcBuf b = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_json_emit(&w, v);
    agentc_buf_byte(&b, 0);
    return (char *)b.p;
}

static int verdict_from_str(const char *s) {
    if (s == NULL)
        return -1;
    if (agentc_streq(s, "allow"))
        return AGENTC_VERDICT_ALLOW;
    if (agentc_streq(s, "deny"))
        return AGENTC_VERDICT_DENY;
    if (agentc_streq(s, "narrow"))
        return AGENTC_VERDICT_NARROW;
    if (agentc_streq(s, "replace"))
        return AGENTC_VERDICT_REPLACE;
    if (agentc_streq(s, "ask"))
        return AGENTC_VERDICT_NARROW; /* approval-gated */
    return -1;
}

int agentc_decision_parse(const char *json, AgcDecisionRecord *out, char *err, size_t cap) {
    if (err && cap)
        err[0] = 0;
    if (out == NULL || json == NULL)
        return -22;
    agentc_memset(out, 0, sizeof *out);
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = ja ? agentc_json_parse_in(ja, json, agentc_strlen(json)) : NULL;
    if (root == NULL || agentc_json_type(root) != AGENTC_JSON_OBJ) {
        if (err && cap)
            agentc_snprintf(err, cap, "malformed decision");
        if (ja)
            agentc_json_arena_free(ja);
        return -22;
    }
    int v = verdict_from_str(agentc_json_get_str(root, "verdict"));
    if (v < 0) {
        if (err && cap)
            agentc_snprintf(err, cap, "unknown verdict");
        agentc_json_arena_free(ja);
        return -22;
    }
    out->verdict = v;
    out->id = (uint64_t)agentc_json_get_int(root, "id", 0);
    out->reason = json_dup(agentc_json_get(root, "reason"));
    out->args_json = json_dup(agentc_json_get(root, "args"));
    out->replace_json = json_dup(agentc_json_get(root, "replace"));
    out->provider = json_dup(agentc_json_get(root, "provider"));
    out->model = json_dup(agentc_json_get(root, "model"));
    out->thinking = json_dup(agentc_json_get(root, "thinking"));
    out->done = agentc_json_get_bool(root, "done", false);
    AgcJson *ap = agentc_json_get(root, "approval");
    if (agentc_json_type(ap) == AGENTC_JSON_OBJ) {
        out->approval_required = agentc_json_get_bool(ap, "required", false);
        const char *r = agentc_json_get_str(ap, "reason");
        out->approval_reason = r ? agentc_strdup(r) : NULL;
    }
    AgcJson *q = agentc_json_get(root, "queue");
    if (agentc_json_type(q) == AGENTC_JSON_OBJ) {
        out->queue_text = json_dup(agentc_json_get(q, "text"));
        out->queue_turn = agentc_json_get_int(q, "turn", 0);
        out->queue_priority = (int32_t)agentc_json_get_int(q, "priority", 0);
    }
    agentc_json_arena_free(ja);
    return 0;
}

void agentc_decision_record_free(AgcDecisionRecord *rec) {
    if (rec == NULL)
        return;
    agentc_free(rec->args_json);
    agentc_free(rec->replace_json);
    agentc_free(rec->approval_reason);
    agentc_free(rec->provider);
    agentc_free(rec->model);
    agentc_free(rec->thinking);
    agentc_free(rec->queue_text);
    agentc_free(rec->reason);
    agentc_memset(rec, 0, sizeof *rec);
}

/* ------------------------------------------------------------- decide */
static AgcAgent *g_decision_parent;

void agentc_decision_set_parent(struct AgcAgent *parent) {
    g_decision_parent = parent;
}

static void emit_decision(int ev, const char *json) {
    agentc_events_emit(NULL, ev, json);
}

/* ------------------------------------------------------ mcp backend */
/* The MCP decider spec names a canonical `mcp__<server>__<tool>` tool. Run the
 * registered tool synchronously and use its text as the decision record; a
 * missing tool, a run error or malformed/absent text is a hard error so the
 * caller fails closed (`deny`). The tool table is snapshot-count-first, so the
 * lookup is bounded by the live registry size. */
static int mcp_decide(const AgcDeciderSpec *spec, const AgcMessage *req, AgcBuf *out) {
    if (spec->command == NULL || spec->command[0] == 0)
        return -22;
    size_t total = agentc_ext_tools(NULL, 0);
    if (total == 0)
        return -3; /* ESRCH: no tools registered */
    AgcTool *tools = agentc_alloc(total * sizeof *tools);
    if (tools == NULL)
        return -12;
    size_t n = agentc_ext_tools(tools, total);
    const AgcTool *hit = NULL;
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(tools[i].name, spec->command)) {
            hit = &tools[i];
            break;
        }
    if (hit == NULL) {
        agentc_free(tools);
        return -3;
    }
    const char *args = (req->body != NULL && req->body[0]) ? req->body : "{}";
    AgcToolCall call;
    agentc_memset(&call, 0, sizeof call);
    call.name = spec->command;
    call.args_json = args;
    call.flags = hit->flags;
    bool is_error = false;
    int rc = 0;
    if (hit->run != NULL) {
        rc = hit->run(hit, &call, out, &is_error);
    } else if (hit->exec != NULL) {
        char *text = hit->exec(args, &is_error, NULL);
        if (text != NULL)
            agentc_buf_cstr(out, text);
        agentc_free(text);
    } else {
        rc = -38; /* ENOSYS: an async-only tool has no synchronous answer */
    }
    agentc_free(tools);
    if (rc != 0 || is_error || out->len == 0)
        return -5; /* EIO: fail closed */
    return 0;
}

/* ----------------------------------------------------- program backend */
/* JSON stdio over a process actor: write the decision envelope to stdin, close
 * it, and read one response line. Malformed/oversized/absent output and a
 * missed deadline are the caller's fail-closed deny; this never blocks the
 * caller's pump (R-D17) and the actor is always released. */
static int program_decide(const AgcDeciderSpec *spec, const AgcMessage *req, AgcBuf *out) {
    if (spec->command == NULL || spec->command[0] == 0)
        return -22;
    AgcProcSpawn ps;
    agentc_memset(&ps, 0, sizeof ps);
    ps.command = spec->command;
    ps.mode = AGENTC_PROC_PIPE_BIDI;
    ps.deadline_ms = AGENTC_DECIDER_TIMEOUT_MS;
    ps.env = agentc_trust_current();
    ps.label = "decider";
    AgcActorSpec as;
    agentc_memset(&as, 0, sizeof as);
    as.proc = &ps;
    char err[128] = {0};
    AgcActorId id = agentc_actor_spawn(&as, err, sizeof err);
    if (id == 0)
        return -5; /* EIO: fail-closed deny at the caller */

    AgcBuf rq = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &rq);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "decider");
    agentc_jsonw_cstr(&w, spec->id ? spec->id : "");
    agentc_jsonw_key(&w, "intent");
    agentc_jsonw_cstr(&w, spec->intent ? spec->intent : "");
    agentc_jsonw_key(&w, "body");
    const char *b = req->body ? req->body : "{}";
    agentc_jsonw_raw(&w, b, req->body_len ? req->body_len : agentc_strlen(b));
    agentc_jsonw_end(&w);
    if (rq.p != NULL)
        (void)agentc_process_write(id, (const char *)rq.p, rq.len, true);
    else
        (void)agentc_process_write(id, "", 0, true);
    agentc_buf_free(&rq);

    AgcBuf acc = {0};
    i64 deadline = os_now_ns(OS_CLOCK_MONOTONIC) + (i64)AGENTC_DECIDER_TIMEOUT_MS * 1000000;
    for (;;) {
        agentc_process_poll(id);
        AgcBuf chunk = {0};
        for (;;) {
            size_t n = agentc_process_read(id, &chunk, 0);
            if (n == 0)
                break;
            agentc_buf_push(&acc, chunk.p, n);
            agentc_buf_clear(&chunk);
        }
        agentc_buf_free(&chunk);
        AgcProcInfo pi;
        agentc_process_info(id, &pi);
        bool done = (pi.eof && pi.reaped) || os_now_ns(OS_CLOCK_MONOTONIC) > deadline;
        if (acc.len > AGENTC_DECIDER_MAX_BYTES) { /* bounded: fail closed */
            agentc_actor_release(id);
            agentc_buf_free(&acc);
            return -61;
        }
        if (done)
            break;
        agentc_pump(10);
    }
    agentc_actor_release(id);
    if (acc.p == NULL)
        return -61; /* ENODATA: no output */

    /* first complete line, right-trimmed */
    size_t n = acc.len;
    for (size_t i = 0; i < acc.len; i++)
        if (acc.p[i] == '\n') {
            n = i;
            break;
        }
    while (n > 0 && (acc.p[n - 1] == '\r' || acc.p[n - 1] == ' ' || acc.p[n - 1] == '\t'))
        n--;
    if (n == 0) {
        agentc_buf_free(&acc);
        return -61;
    }
    agentc_buf_push(out, acc.p, n);
    agentc_buf_free(&acc);
    return 0;
}

static int decide_impl(const AgcDeciderSpec *spec, AgcMessage *req, uint64_t *defer, bool async);

int agentc_decide(const AgcDeciderSpec *spec, AgcMessage *req, uint64_t *defer) {
    return decide_impl(spec, req, defer, false);
}

int agentc_decide_async(const AgcDeciderSpec *spec, AgcMessage *req, uint64_t *defer) {
    return decide_impl(spec, req, defer, true);
}

static int decide_impl(const AgcDeciderSpec *spec, AgcMessage *req, uint64_t *defer, bool async) {
    if (defer)
        *defer = 0;
    if (spec == NULL || req == NULL || spec->id == NULL)
        return -22;
    const char *body = req->body ? req->body : "";
    size_t blen = req->body_len ? req->body_len : agentc_strlen(body);

    emit_decision(AGENTC_EV_DECISION_REQUEST, body);

    char *hit = memo_get(spec, body, blen);
    if (hit != NULL) {
        req->body = hit;
        req->body_len = agentc_strlen(hit);
        emit_decision(AGENTC_EV_DECISION_RESULT, hit);
        return 0;
    }

    AgcBuf resp = {0};
    if (spec->kind == AGENTC_DECIDER_LOCAL) {
        if (spec->command != NULL)
            agentc_buf_cstr(&resp, spec->command);
        else if (spec->flags & AGENTC_DECIDER_DEFAULT)
            agentc_buf_cstr(&resp, "{\"verdict\":\"allow\"}");
        else
            agentc_buf_cstr(&resp, "{\"verdict\":\"deny\",\"reason\":\"no local rule\"}");
    } else if (spec->kind == AGENTC_DECIDER_PROGRAM) {
        AgcBuf pr = {0};
        int prc = program_decide(spec, req, &pr);
        bool ok = false;
        if (prc == 0 && pr.p != NULL) {
            AgcDecisionRecord rec;
            char perr[64] = {0};
            if (agentc_decision_parse((const char *)pr.p, &rec, perr, sizeof perr) == 0) {
                agentc_decision_record_free(&rec);
                ok = true;
            }
        }
        if (ok)
            agentc_buf_push(&resp, pr.p, pr.len);
        else
            agentc_buf_cstr(&resp, "{\"verdict\":\"deny\",\"reason\":\"program decider failed\"}");
        agentc_buf_free(&pr);
    } else if (spec->kind == AGENTC_DECIDER_MCP) {
        AgcBuf mr = {0};
        int mrc = mcp_decide(spec, req, &mr);
        bool ok = false;
        if (mrc == 0 && mr.p != NULL) {
            AgcDecisionRecord rec;
            char merr[64] = {0};
            if (agentc_decision_parse((const char *)mr.p, &rec, merr, sizeof merr) == 0) {
                agentc_decision_record_free(&rec);
                ok = true;
            }
        }
        if (ok)
            agentc_buf_push(&resp, mr.p, mr.len);
        else
            agentc_buf_cstr(&resp, "{\"verdict\":\"deny\",\"reason\":\"mcp decider failed\"}");
        agentc_buf_free(&mr);
    } else {
        /* program/mcp/model: branch to an ephemeral child on the shared
         * transport, or (for program/mcp) the registered backend. The child's
         * final text is the response. */
        AgcAgentSpec as;
        agentc_memset(&as, 0, sizeof as);
        as.parent = g_decision_parent;
        as.provider = spec->provider;
        as.model = spec->model;
        as.thinking = -1;
        as.system = spec->system;
        as.flags = AGENTC_AGENT_F_WAIT | AGENTC_AGENT_F_EPHEMERAL;
        if (async)
            as.flags = AGENTC_AGENT_F_BACKGROUND | AGENTC_AGENT_F_EPHEMERAL;
        uint64_t d = 0;
        int rc = async ? agentc_hook_branch_self(&as, req, &d) : agentc_hook_branch(&as, req, &d);
        if (d != 0) {
            /* Keep a bounded out-of-band row so agentc_decision_submit can
             * answer before the branch child finishes. A full table is not a
             * hard error: the branch itself is the answer channel. */
            if (async)
                (void)decision_pending_add(d, req);
            if (defer)
                *defer = d;
            agentc_buf_free(&resp);
            return 0;
        }
        if (rc != 0) {
            agentc_buf_free(&resp);
            return rc;
        }
        agentc_buf_push(&resp, req->body, req->body_len);
    }
    if (resp.p == NULL)
        agentc_buf_byte(&resp, 0);
    char *stored = memo_put(spec, body, blen, resp.p ? (const char *)resp.p : "");
    agentc_buf_free(&resp);
    if (stored != NULL) {
        req->body = stored;
        req->body_len = agentc_strlen(stored);
    }
    emit_decision(AGENTC_EV_DECISION_RESULT, req->body);
    if (spec->intent != NULL && agentc_streq(spec->intent, "route"))
        emit_decision(AGENTC_EV_ROUTE_DECIDED, req->body);
    return 0;
}

int agentc_decision_submit(const char *response_json) {
    /* Out-of-band answer seam: validate the shape so a front end cannot inject
     * a malformed verdict, then resume the parked controller at its own handler
     * index (the self-resume seam) with the response as the message body. With
     * no matching pending row this is validate-only, preserving the legacy
     * synchronous contract. */
    if (response_json == NULL)
        return -22;
    AgcDecisionRecord rec;
    char err[96] = {0};
    if (agentc_decision_parse(response_json, &rec, err, sizeof err) != 0)
        return -22;

    PendingDecision *hit = NULL;
    if (rec.id != 0) {
        for (size_t i = 0; i < AGENTC_DECISION_PENDING_MAX; i++)
            if (g_dpending[i].used && g_dpending[i].id == rec.id) {
                hit = &g_dpending[i];
                break;
            }
    } else {
        size_t n = 0;
        PendingDecision *only = NULL;
        for (size_t i = 0; i < AGENTC_DECISION_PENDING_MAX; i++)
            if (g_dpending[i].used) {
                n++;
                only = &g_dpending[i];
            }
        if (n == 1)
            hit = only;
    }
    if (hit == NULL) {
        agentc_decision_record_free(&rec);
        return 0; /* validate-only */
    }

    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_HOOK;
    m.kind = AGENTC_MSG_INPUT;
    m.id = hit->id;
    m.actor_id = hit->actor_id;
    m.meta = hit->meta;
    m.body = response_json;
    m.body_len = agentc_strlen(response_json);
    uint64_t token = hit->token;
    hit->used = false;
    agentc_free(hit->meta);
    hit->meta = NULL;
    hit->token = 0;
    agentc_decision_record_free(&rec);
    (void)agentc_hook_resume(token, &m);
    return 0;
}

/* ------------------------------------------------- controller application */
/* Per-invocation narrowed/replaced bodies. A ring indexed by invocation keeps
 * a nested controller call from clobbering a body the outer walk still holds
 * (audit M5); the bound is the hook depth cap. */
static AgcBuf g_decision_body[AGENTC_HOOK_MAX_DEPTH];
static unsigned g_decision_ring;

static AgcBuf *decision_buffer(void) {
    AgcBuf *b = &g_decision_body[g_decision_ring++ % AGENTC_HOOK_MAX_DEPTH];
    agentc_buf_clear(b);
    return b;
}

static int ctrl_send(AgcMessage *m, int kind, const char *body) {
    AgcMessage c;
    agentc_memset(&c, 0, sizeof c);
    c.class = AGENTC_MSG_CLASS_CONTROL;
    c.kind = kind;
    c.to = m->actor_id;
    if (body != NULL) {
        c.body = body;
        c.body_len = agentc_strlen(body);
    }
    return agentc_message_send(&c);
}

int agentc_controller_apply(AgcMessage *m, const AgcDecisionRecord *rec, AgcMessage **next,
                            uint64_t *defer, AgcBuf *err) {
    if (defer)
        *defer = 0;
    if (m == NULL || rec == NULL || next == NULL)
        return -22;

    /* Validate + side effects first, then the call-level verdict (P6-DECIDERS
     * §2b.3). Each effect is an ordinary primitive call. */
    if (rec->approval_required) {
        AgcApprovalRequest areq;
        agentc_memset(&areq, 0, sizeof areq);
        areq.actor_id = m->actor_id;
        areq.tool = m->meta ? m->meta : "tool";
        areq.input_json = m->body;
        areq.trigger = "decision";
        AgcApprovalResult ares;
        agentc_memset(&ares, 0, sizeof ares);
        int arc = agentc_approval_request(&areq, &ares);
        if (arc != 0 || ares.decision != AGENTC_DEC_ALLOW) {
            if (err)
                agentc_buf_cstr(err, "approval denied");
            *next = NULL;
            return 0;
        }
    }
    /* W4 note: a decision record may carry a `provider` + `model` route. There
     * is no in-place provider switch (the adapter is fixed at construction), so
     * this cannot invoke the virtual-model resolver directly; the consistent
     * behavior is to apply a provider+model route only when it names the calling
     * agent's current provider, and to deny otherwise rather than send a model id
     * to the wrong wire. A virtual-model alias sent through SET_MODEL is then
     * resolved per step by route_resolve() in agent.c. A `provider` with no model
     * is informational only and ignored (never denies). */
    if (rec->provider != NULL && rec->provider[0] && rec->model != NULL && rec->model[0] != 0) {
        AgcAgent *owner = m->actor_id ? agentc_agent_find(m->actor_id) : NULL;
        const AgcProvider *cur = owner ? agentc_agent_provider(owner) : NULL;
        if (cur == NULL || !agentc_streq(cur->name, rec->provider)) {
            if (err)
                agentc_buf_cstr(err, "provider switch requires a virtual model route");
            *next = NULL;
            return 0;
        }
    }
    if (rec->model != NULL)
        (void)ctrl_send(m, AGENTC_MSG_SET_MODEL, rec->model);
    if (rec->thinking != NULL)
        (void)ctrl_send(m, AGENTC_MSG_SET_THINKING, rec->thinking);
    if (rec->done)
        (void)ctrl_send(m, AGENTC_MSG_SETTLE, NULL);
    if (rec->queue_text != NULL)
        (void)ctrl_send(m, AGENTC_MSG_INPUT, rec->queue_text);

    switch (rec->verdict) {
    case AGENTC_VERDICT_DENY:
        *next = NULL;
        return 0;
    case AGENTC_VERDICT_NARROW:
        if (rec->args_json != NULL) {
            AgcBuf *b = decision_buffer();
            agentc_buf_cstr(b, rec->args_json);
            m->body = (const char *)b->p;
            m->body_len = b->len;
        }
        *next = m;
        return 0;
    case AGENTC_VERDICT_REPLACE:
        if (rec->replace_json != NULL) {
            AgcBuf *b = decision_buffer();
            agentc_buf_cstr(b, rec->replace_json);
            m->body = (const char *)b->p;
            m->body_len = b->len;
            m->meta = "agent";
        }
        *next = m;
        return 0;
    case AGENTC_VERDICT_ALLOW:
    default:
        *next = m;
        return 0;
    }
}

/* ----------------------------------------------------- default handlers */
AgcControllerCtx *agentc_controller_new(const AgcDeciderSpec *spec) {
    if (spec == NULL)
        return NULL;
    AgcControllerCtx *c = agentc_alloc(sizeof *c);
    agentc_memset(c, 0, sizeof *c);
    c->spec = *spec;
    c->id = (char *)ddup(spec->id);
    c->intent = (char *)ddup(spec->intent);
    c->provider = (char *)ddup(spec->provider);
    c->model = (char *)ddup(spec->model);
    c->command = (char *)ddup(spec->command);
    c->system = (char *)ddup(spec->system);
    c->spec.id = c->id;
    c->spec.intent = c->intent;
    c->spec.provider = c->provider;
    c->spec.model = c->model;
    c->spec.command = c->command;
    c->spec.system = c->system;
    return c;
}

void agentc_controller_free(AgcControllerCtx *ctx) {
    if (ctx == NULL)
        return;
    agentc_free(ctx->id);
    agentc_free(ctx->intent);
    agentc_free(ctx->provider);
    agentc_free(ctx->model);
    agentc_free(ctx->command);
    agentc_free(ctx->system);
    agentc_free(ctx);
}

/* Apply the decision record already present in `m->body` (the synchronous
 * decide result or the self-resumed branch/out-of-band response). */
static int controller_apply_body(AgcControllerCtx *ctx, AgcMessage *m, AgcMessage **next,
                                 uint64_t *defer) {
    AgcDecisionRecord rec;
    char err[96] = {0};
    if (agentc_decision_parse(m->body ? m->body : "", &rec, err, sizeof err) != 0) {
        /* fail-closed for approve/spawn gates; default (pass) for a route. */
        bool is_route = agentc_streq(ctx->spec.intent ? ctx->spec.intent : "", "route");
        if (is_route && !(ctx->spec.flags & AGENTC_DECIDER_FAIL_CLOSED)) {
            if (next)
                *next = m;
            return 0;
        }
        if (next)
            *next = NULL;
        return 0;
    }
    AgcBuf e = {0};
    int rc = agentc_controller_apply(m, &rec, next, defer, &e);
    agentc_decision_record_free(&rec);
    agentc_buf_free(&e);
    return rc;
}

static int controller_common(AgcControllerCtx *ctx, AgcMessage *m, AgcMessage **next,
                             uint64_t *defer) {
    if (next)
        *next = m;
    if (defer)
        *defer = 0;
    if (ctx == NULL || m == NULL)
        return 0;
    /* Only a real call body is a decision input; the observe-only result emit
     * passes a NULL body and must pass through. */
    if (m->body == NULL)
        return 0;

    /* Self-resume seam: the async branch (or agentc_decision_submit) has
     * completed, so the body is now the decision response and this handler runs
     * its own post-decide apply instead of branching again. */
    if (agentc_hook_in_reentry())
        return controller_apply_body(ctx, m, next, defer);

    uint64_t d = 0;
    int rc = agentc_decide_async(&ctx->spec, m, &d);
    if (d != 0) {
        if (defer)
            *defer = d;
        return 0;
    }
    if (rc != 0) {
        if (next)
            *next = NULL;
        return 0;
    }
    return controller_apply_body(ctx, m, next, defer);
}

int agentc_controller_handler(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    return controller_common(ud, m, next, defer);
}

int agentc_router_handler(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    return controller_common(ud, m, next, defer);
}
