/* hook.c — the universal interception pipeline (P6-B core; see P6-HOOKS.md).
 *
 * A bounded registry of AgcHookFn handlers keyed by stage. run walks the chain
 * in (order, registration seq) order; a handler returns the next message (NULL
 * consumes the flow) and may set *defer to park. Guards: a global depth cap and
 * a per-message reentry set. Per-stage errors default to pass for transform
 * stages and deny for the `tool` gate. The legacy extension bus is unaffected;
 * B8 adapts `on(point,…)` handlers into this chain.
 */
#include "hook.h"

#include "agent_int.h"
#include "base/limits.h"
#include "core/events.h"
#include "ext.h"
#include "plat.h"
#include "wire.h"

typedef struct {
    bool used;
    char *stage;
    int order;
    u64 seq;
    AgcHookFn fn;
    void *ud;
} HookRec;

static HookRec g_hooks[AGENTC_HOOK_MAX_HANDLERS];
static u64 g_hook_seq;

u64 agentc_hook_next_id(void) {
    return ++g_hook_seq;
}
static int g_depth;
static int g_active; /* handlers currently on the stack */
static u64 g_active_ids[AGENTC_HOOK_MAX_DEPTH];

static const char *const g_stages[] = {
    AGENTC_HOOK_STAGE_TURN,
    AGENTC_HOOK_STAGE_STEP,
    AGENTC_HOOK_STAGE_INPUT,
    AGENTC_HOOK_STAGE_PROMPT,
    AGENTC_HOOK_STAGE_MESSAGE,
    AGENTC_HOOK_STAGE_OUTPUT,
    AGENTC_HOOK_STAGE_TOOL,
    AGENTC_HOOK_STAGE_SIGNAL,
    AGENTC_HOOK_STAGE_EVENT,
    AGENTC_HOOK_STAGE_LIFECYCLE,
    "approval",
    AGENTC_HOOK_STAGE_SETTLE,
    AGENTC_HOOK_STAGE_COMPACT,
    AGENTC_HOOK_STAGE_VETO,
    AGENTC_HOOK_STAGE_PROVIDER,
};

static bool stage_valid(const char *stage) {
    for (size_t i = 0; i < sizeof g_stages / sizeof g_stages[0]; i++)
        if (agentc_streq(g_stages[i], stage))
            return true;
    return false;
}

/* The `tool` gate (call sub-kind) is fail-closed; every other stage passes on a
 * handler error (HOK §7). */
static bool stage_is_gate(const char *stage) {
    return agentc_streq(stage, AGENTC_HOOK_STAGE_TOOL);
}

uint64_t agentc_hook(const char *stage, int order, AgcHookFn fn, void *ud) {
    if (stage == NULL || fn == NULL)
        return 0;
    if (!stage_valid(stage)) {
        agentc_logf(3, "hook: unknown stage %s", stage);
        return 0;
    }
    for (size_t i = 0; i < AGENTC_HOOK_MAX_HANDLERS; i++) {
        if (g_hooks[i].used)
            continue;
        g_hooks[i].used = true;
        g_hooks[i].stage = agentc_strdup(stage);
        g_hooks[i].order = order;
        g_hooks[i].seq = ++g_hook_seq;
        g_hooks[i].fn = fn;
        g_hooks[i].ud = ud;
        return g_hooks[i].seq;
    }
    agentc_logf(3, "hook: too many handlers");
    return 0;
}

void agentc_hook_off(uint64_t handle) {
    if (handle == 0)
        return;
    for (size_t i = 0; i < AGENTC_HOOK_MAX_HANDLERS; i++) {
        if (g_hooks[i].used && g_hooks[i].seq == handle) {
            agentc_free(g_hooks[i].stage);
            agentc_memset(&g_hooks[i], 0, sizeof g_hooks[i]);
            return;
        }
    }
}

bool agentc_hook_wants(const char *stage) {
    if (stage == NULL)
        return false;
    for (size_t i = 0; i < AGENTC_HOOK_MAX_HANDLERS; i++)
        if (g_hooks[i].used && agentc_streq(g_hooks[i].stage, stage))
            return true;
    return false;
}

/* Snapshot of the stage's handlers, sorted by (order, seq). */
typedef struct {
    AgcHookFn fn;
    void *ud;
    int order;
    u64 seq;
} HookSnap;

static size_t snapshot(const char *stage, HookSnap *out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < AGENTC_HOOK_MAX_HANDLERS && n < max; i++) {
        if (!g_hooks[i].used || !agentc_streq(g_hooks[i].stage, stage))
            continue;
        out[n].fn = g_hooks[i].fn;
        out[n].ud = g_hooks[i].ud;
        out[n].order = g_hooks[i].order;
        out[n].seq = g_hooks[i].seq;
        n++;
    }
    for (size_t i = 1; i < n; i++) {
        HookSnap key = out[i];
        size_t j = i;
        while (j > 0 && (out[j - 1].order > key.order ||
                         (out[j - 1].order == key.order && out[j - 1].seq > key.seq))) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return n;
}

static bool reentry_enter(u64 id) {
    if (id == 0)
        return true;
    for (int i = 0; i < g_active; i++)
        if (g_active_ids[i] == id)
            return false;
    if (g_active >= AGENTC_HOOK_MAX_DEPTH)
        return false;
    g_active_ids[g_active++] = id;
    return true;
}

static void reentry_leave(u64 id) {
    if (id == 0)
        return;
    for (int i = 0; i < g_active; i++) {
        if (g_active_ids[i] == id) {
            g_active_ids[i] = g_active_ids[--g_active];
            return;
        }
    }
}

/* --------------------------------------------------------------- defer */

typedef struct {
    bool used;
    u64 token;
    char *stage;
    size_t next;
    bool self; /* re-enter the parked handler, not the next one */
} DeferRec;

static DeferRec g_defers[AGENTC_HOOK_MAX_DEFERS];

/* Handlers that asked for a same-handler re-entry. Marked before walk parks (a
 * handler cannot set *defer until after it decides to branch), consumed by the
 * matching defer_park. Token-keyed so a nested branch cannot mis-mark. */
static u64 g_self_marks[AGENTC_HOOK_MAX_DEFERS];
static size_t g_self_n;
static bool g_reentry; /* true only inside a self-resumed handler body */

void agentc_hook_mark_self(uint64_t token) {
    if (token == 0)
        return;
    for (size_t i = 0; i < g_self_n; i++)
        if (g_self_marks[i] == token)
            return;
    if (g_self_n < AGENTC_HOOK_MAX_DEFERS)
        g_self_marks[g_self_n++] = token;
}

static bool self_consume(uint64_t token) {
    for (size_t i = 0; i < g_self_n; i++) {
        if (g_self_marks[i] == token) {
            g_self_marks[i] = g_self_marks[--g_self_n];
            return true;
        }
    }
    return false;
}

bool agentc_hook_in_reentry(void) {
    return g_reentry;
}

static u64 defer_park(u64 token, const char *stage, size_t next, bool self) {
    for (size_t i = 0; i < AGENTC_HOOK_MAX_DEFERS; i++) {
        if (g_defers[i].used && g_defers[i].token != token)
            continue;
        if (g_defers[i].used)
            agentc_free(g_defers[i].stage);
        g_defers[i].used = true;
        g_defers[i].token = token;
        g_defers[i].stage = agentc_strdup(stage);
        g_defers[i].next = next;
        g_defers[i].self = self;
        return token;
    }
    return 0; /* defer table full: treated as a hard error */
}

static int walk(const char *stage, AgcMessage *m, HookSnap *list, size_t n, size_t start,
                AgcMessage **out, bool reentry, uint64_t *token_out) {
    AgcMessage *cur = m;
    for (size_t i = start; i < n; i++) {
        AgcMessage *next = cur;
        u64 defer = 0;
        g_depth++;
        bool prev = g_reentry;
        g_reentry = (reentry && i == start);
        int rc = list[i].fn(list[i].ud, cur, &next, &defer);
        g_reentry = prev;
        g_depth--;
        if (defer != 0) {
            bool self = self_consume(defer);
            u64 token = defer_park(defer, stage, self ? i : i + 1, self);
            if (out)
                *out = cur;
            if (token_out)
                *token_out = token;
            return token != 0 ? -115 /* EINPROGRESS */ : -1;
        }
        if (rc < 0) {
            if (stage_is_gate(stage))
                return -1; /* EPERM: deny */
            agentc_logf(2, "hook: %s handler failed, passing", stage);
            continue;
        }
        if (next == NULL) {
            if (out)
                *out = NULL; /* consumed */
            return 0;
        }
        cur = next;
    }
    if (out)
        *out = cur;
    return 0;
}

int agentc_hook_run(const char *stage, AgcMessage *m, AgcMessage **out) {
    return agentc_hook_run_defer(stage, m, out, NULL);
}

int agentc_hook_run_defer(const char *stage, AgcMessage *m, AgcMessage **out, uint64_t *defer) {
    if (defer)
        *defer = 0;
    if (out)
        *out = m;
    if (stage == NULL || m == NULL)
        return -22;
    if (!agentc_hook_wants(stage))
        return 0;
    if (g_depth >= AGENTC_HOOK_MAX_DEPTH)
        return -40; /* ELOOP: depth */
    if (!reentry_enter(m->id))
        return -16; /* EBUSY: reentry */
    HookSnap list[AGENTC_HOOK_MAX_HANDLERS];
    size_t n = snapshot(stage, list, AGENTC_HOOK_MAX_HANDLERS);
    int rc = walk(stage, m, list, n, 0, out, false, defer);
    reentry_leave(m->id);
    return rc;
}

int agentc_hook_resume(uint64_t token, AgcMessage *m) {
    if (token == 0 || m == NULL)
        return -22;
    for (size_t i = 0; i < AGENTC_HOOK_MAX_DEFERS; i++) {
        DeferRec *d = &g_defers[i];
        if (!d->used || d->token != token)
            continue;
        HookSnap list[AGENTC_HOOK_MAX_HANDLERS];
        size_t n = snapshot(d->stage, list, AGENTC_HOOK_MAX_HANDLERS);
        size_t start = d->next;
        bool self = d->self;
        char *stage = d->stage;
        d->used = false;
        d->token = 0;
        d->next = 0;
        d->self = false;
        d->stage = NULL;
        if (start > n)
            start = n;
        AgcMessage *out = m;
        int rc = walk(stage, m, list, n, start, &out, self, NULL);
        agentc_free(stage);
        return rc;
    }
    return -22; /* unknown token */
}

/* Discard a parked chain without running it (the async approval answer path).
 * The same numeric token also keys a runtime branch continuation, which cleans
 * itself up when its child reaches terminal. */
void agentc_hook_cancel(uint64_t token) {
    if (token == 0)
        return;
    for (size_t i = 0; i < AGENTC_HOOK_MAX_DEFERS; i++) {
        DeferRec *d = &g_defers[i];
        if (!d->used || d->token != token)
            continue;
        agentc_free(d->stage);
        agentc_memset(d, 0, sizeof *d);
        return;
    }
}

/* ---- branch continuations: spawn an actor and resume the chain on terminal -- */
typedef struct {
    uint64_t token;
    AgcActorId child;
    AgcMessage msg; /* shallow copy; body/meta owned below */
    char *body_copy;
    char *meta_copy;
} BranchCtx;

static AgcBuf g_branch_out[AGENTC_HOOK_MAX_DEPTH];
static unsigned g_branch_ring;

/* Each branch invocation gets its own result buffer from a depth-bounded ring,
 * so a nested branch reachable through the synchronous child run or a hook
 * resume cannot clobber the outer body (audit M5). */
static AgcBuf *branch_buffer(void) {
    AgcBuf *b = &g_branch_out[g_branch_ring++ % AGENTC_HOOK_MAX_DEPTH];
    agentc_buf_clear(b);
    return b;
}

static void branch_resume(void *ud, AgcMessage *unused) {
    (void)unused;
    BranchCtx *b = ud;
    AgcActor *row = agentc_actor_get(b->child);
    bool done = row == NULL || row->state == AGENTC_ACTOR_SETTLED ||
                row->state == AGENTC_ACTOR_FAILED || row->state == AGENTC_ACTOR_CANCELLED;
    if (!done)
        return; /* not yet: fire again on the next tick */

    AgcBuf out = {0};
    if (row != NULL)
        (void)agentc_agent_result(b->child, &out);
    AgcBuf *bo = branch_buffer();
    if (out.p != NULL)
        agentc_buf_push(bo, out.p, out.len);
    else
        agentc_buf_byte(bo, 0);
    agentc_buf_free(&out);
    b->msg.body = bo->p ? (const char *)bo->p : "";
    b->msg.body_len = bo->len;

    agentc_runtime_cancel_defer(b->token);
    (void)agentc_hook_resume(b->token, &b->msg);
    agentc_free(b->body_copy);
    agentc_free(b->meta_copy);
    agentc_actor_release(b->child);
    agentc_free(b);
}

int agentc_hook_branch(const struct AgcAgentSpec *spec, AgcMessage *m, uint64_t *defer) {
    if (defer)
        *defer = 0;
    /* A NULL spec is the legacy "no branch target" call (P6-B's -ENOTSUP). */
    if (spec == NULL)
        return -95;
    if (m == NULL)
        return -22;
    /* A branch is a one-shot ephemeral child agent (P6-PRIMITIVES §4): its task
     * is the current message body; its final text is the branch response. It
     * inherits the current agent's provider/model/transport (R-D2), so a model
     * decider adds no primitive and no transport of its own. */
    AgcAgentSpec s = *spec;
    if (s.parent == NULL) {
        AgcAgent *cur = agentc_agent_find(agentc_agent_current());
        s.parent = cur;
    }
    if (s.task == NULL)
        s.task = m->body ? m->body : "";
    bool background = (s.flags & AGENTC_AGENT_F_BACKGROUND) != 0;
    s.flags |= AGENTC_AGENT_F_EPHEMERAL;
    if (background)
        s.flags |= AGENTC_AGENT_F_BACKGROUND;
    AgcAgent *child = agentc_agent_create(&s);
    if (child == NULL)
        return -1;
    AgcActorId cid = agentc_agent_actor(child);
    AgcActor *row = agentc_actor_get(cid);
    bool settled = row == NULL || row->state == AGENTC_ACTOR_SETTLED ||
                   row->state == AGENTC_ACTOR_FAILED || row->state == AGENTC_ACTOR_CANCELLED;

    /* WAIT (the default) runs the child inside create(), so the result is ready
     * now and the legacy synchronous contract is preserved byte-for-byte. */
    if (settled) {
        AgcBuf out = {0};
        (void)agentc_agent_result(cid, &out);
        AgcBuf *bo = branch_buffer();
        if (out.p != NULL)
            agentc_buf_push(bo, out.p, out.len);
        else
            agentc_buf_byte(bo, 0);
        agentc_buf_free(&out);
        m->body = bo->p ? (const char *)bo->p : "";
        m->body_len = bo->len;
        return 0;
    }

    /* BACKGROUND (or a decider that must not block): a true continuation. Park
     * the chain on a runtime defer and let runtime_tick resume it once the
     * child actor reaches terminal. A full defer table fails closed. */
    BranchCtx *b = agentc_alloc(sizeof *b);
    if (b == NULL)
        return -1;
    agentc_memset(b, 0, sizeof *b);
    b->child = cid;
    b->msg = *m;
    b->body_copy = m->body ? agentc_strdup_len(m->body, m->body_len) : NULL;
    b->meta_copy = m->meta ? agentc_strdup(m->meta) : NULL;
    b->msg.body = b->body_copy;
    b->msg.meta = b->meta_copy;
    b->token = agentc_runtime_defer(branch_resume, b);
    if (b->token == 0) {
        agentc_free(b->body_copy);
        agentc_free(b->meta_copy);
        agentc_free(b);
        (void)agentc_actor_cancel(cid, AGENTC_FREEZE_ACTOR, 2);
        return -1; /* fail closed: never run unbounded work */
    }
    if (defer)
        *defer = b->token;
    return 0;
}

int agentc_hook_branch_self(const struct AgcAgentSpec *spec, AgcMessage *m, uint64_t *defer) {
    uint64_t token = 0;
    int rc = agentc_hook_branch(spec, m, &token);
    if (defer)
        *defer = token;
    if (token != 0)
        agentc_hook_mark_self(token);
    return rc;
}

void agentc_hook_result_free(AgcMessage *m) {
    if (m == NULL)
        return;
    agentc_free(m->result_json);
    m->result_json = NULL;
    m->handled = false;
    m->blocked = false;
    m->consumed = false;
}

int agentc_hook_run_legacy(const char *stage, const char *point, AgcMessage *m,
                           const char *payload_json, AgcMessage **out) {
    if (out)
        *out = m;
    if (m == NULL)
        return -22;
    AgcMessage *cur = m;
    int rc = agentc_hook_run(stage, m, &cur);
    if (rc < 0)
        return rc; /* parked (-EINPROGRESS) or denied */
    if (cur == NULL) {
        m->consumed = true; /* core consumed: no legacy dispatch */
        return 0;
    }
    if (point == NULL)
        return 0;
    const char *payload = payload_json ? payload_json : (m->body ? m->body : "{}");
    AgcExtResult r = agentc_ext_emit(point, payload);
    m->handled = r.handled ? true : false;
    m->blocked = r.blocked ? true : false;
    agentc_free(m->result_json);
    m->result_json = r.result_json; /* transfer ownership */
    if (out)
        *out = cur;
    return 0;
}

void agentc_hook_stage_emit(const char *stage, const char *kind, const char *text,
                            uint32_t actor_id) {
    if (!stage || !agentc_hook_wants(stage))
        return;
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_HOOK;
    m.kind = AGENTC_MSG_INPUT;
    m.actor_id = actor_id;
    m.meta = kind;
    m.body = text ? text : "";
    m.body_len = text ? agentc_strlen(text) : 0;
    m.id = ++g_hook_seq; /* synthetic identity for reentry/depth */
    AgcMessage *out = NULL;
    (void)agentc_hook_run(stage, &m, &out);
}
