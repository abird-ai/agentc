/* actor.c — the runtime actor registry (P6-A; see plans/P6-ACTORS.md).
 *
 * One bounded table per process. The registry owns each row's label and, for
 * process actors, the AgcProcess payload; the kind-specific behavior lives in
 * core/process.c. Lifecycle: register(STARTING) -> RUNNING -> STALLED ->
 * terminal (SETTLED|FAILED|CANCELLED), with FROZEN as a non-terminal pause.
 * agentc_actor_tick() is the only place a process is advanced; it is called
 * from agentc_pump and from the agent loop's poll slices.
 */
#include "core/actor_int.h"
#include "core/agent_int.h"
#include "hook.h"

#include "base/limits.h"
#include "core/events.h"
#include "plat.h"

static AgcActor g_actors[AGENTC_ACTOR_MAX];
static bool g_inited;
static bool g_ticking;

/* --------------------------------------------------------------- helpers */

i64 actor_now(void) {
    return os_now_ns(OS_CLOCK_MONOTONIC);
}

static AgcActor *actor_of(AgcActorId id) {
    if (id == 0 || id > AGENTC_ACTOR_MAX)
        return NULL;
    AgcActor *a = &g_actors[id - 1];
    return a->id == id ? a : NULL;
}

static bool terminal(AgcActorState s) {
    return s == AGENTC_ACTOR_SETTLED || s == AGENTC_ACTOR_FAILED || s == AGENTC_ACTOR_CANCELLED;
}

static char *dup_label(const char *s) {
    if (s == NULL || s[0] == 0)
        return NULL;
    size_t n = agentc_strlen(s);
    if (n > AGENTC_ACTOR_LABEL_MAX)
        n = AGENTC_ACTOR_LABEL_MAX;
    return agentc_strdup_len(s, n);
}

static void ensure_init(void) {
    if (g_inited)
        return;
    g_inited = true;
    agentc_memset(g_actors, 0, sizeof g_actors);
    agentc_message_init();
    agentc_queue_init();
    agentc_atexit(agentc_actor_release_all);
}

void agentc_actor_init(void) {
    if (g_inited)
        agentc_actor_release_all();
    agentc_memset(g_actors, 0, sizeof g_actors);
    agentc_message_init();
    agentc_queue_init();
    g_inited = true;
    agentc_atexit(agentc_actor_release_all);
}

/* -------------------------------------------------------------- lifecycle */

const char *agentc_actor_state_name(AgcActorState s) {
    switch (s) {
    case AGENTC_ACTOR_STARTING:
        return "starting";
    case AGENTC_ACTOR_RUNNING:
        return "running";
    case AGENTC_ACTOR_WAITING:
        return "waiting";
    case AGENTC_ACTOR_STALLED:
        return "stalled";
    case AGENTC_ACTOR_FROZEN:
        return "frozen";
    case AGENTC_ACTOR_SETTLED:
        return "settled";
    case AGENTC_ACTOR_FAILED:
        return "failed";
    case AGENTC_ACTOR_CANCELLED:
        return "cancelled";
    default:
        return "?";
    }
}

AgcActorId agentc_actor_register(AgcActorKind kind, AgcActorId parent, const char *label) {
    ensure_init();
    for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
        if (g_actors[i].id != 0)
            continue;
        AgcActor *a = &g_actors[i];
        agentc_memset(a, 0, sizeof *a);
        a->id = (AgcActorId)(i + 1);
        a->kind = kind;
        a->parent = parent;
        AgcActor *p = actor_of(parent);
        a->depth = p != NULL ? (uint16_t)(p->depth + 1) : 0;
        a->state = AGENTC_ACTOR_STARTING;
        a->started_ns = a->last_progress_ns = actor_now();
        a->idle_ms =
            kind == AGENTC_ACTOR_PROCESS ? AGENTC_PROC_IDLE_DEF_MS : AGENTC_AGENT_IDLE_DEF_MS;
        a->label = dup_label(label);
        return a->id;
    }
    return 0;
}

AgcActor *agentc_actor_get(AgcActorId id) {
    return actor_of(id);
}

AgcActorId agentc_actor_parent(AgcActorId id) {
    AgcActor *a = actor_of(id);
    return a != NULL ? a->parent : 0;
}

bool agentc_actor_exists(AgcActorId id) {
    return actor_of(id) != NULL;
}

int agentc_actor_info(AgcActorId id, AgcActor *out) {
    AgcActor *a = actor_of(id);
    if (a == NULL || out == NULL)
        return -3; /* ESRCH */
    *out = *a;
    return 0;
}

int64_t agentc_actor_turn(AgcActorId id) {
    AgcActor *a = actor_of(id);
    return a != NULL ? a->turn_seq : 0;
}

int agentc_actor_set_label(AgcActorId id, const char *label) {
    AgcActor *a = actor_of(id);
    if (a == NULL)
        return -3;
    agentc_free(a->label);
    a->label = dup_label(label);
    return 0;
}

size_t agentc_actor_count(AgcActorKind kind_or_any) {
    size_t n = 0;
    for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
        if (g_actors[i].id == 0)
            continue;
        if ((int)kind_or_any >= 0 && g_actors[i].kind != kind_or_any)
            continue;
        n++;
    }
    return n;
}

AgcActorId agentc_actor_root(void) {
    for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
        AgcActor *a = &g_actors[i];
        if (a->id != 0 && a->kind == AGENTC_ACTOR_AGENT && a->parent == 0)
            return a->id;
    }
    return 0;
}

size_t agentc_actor_list(AgcActor *out, size_t max) {
    size_t n = agentc_actor_count((AgcActorKind)-1);
    if (out == NULL)
        return n;
    size_t k = 0;
    for (size_t i = 0; i < AGENTC_ACTOR_MAX && k < max; i++) {
        if (g_actors[i].id == 0)
            continue;
        out[k++] = g_actors[i];
    }
    return k;
}

/* Fill the event payload and dispatch through the hub with no owning agent, so
 * per-slot subscribers (the extension bridge, front ends, tests) all see it. */
void actor_event(AgcActor *a, int ev, const char *text, const char *json, int exit_code) {
    const char *kind = "lifecycle";
    switch (ev) {
    case AGENTC_EV_ACTOR_STARTED:
        kind = "started";
        break;
    case AGENTC_EV_ACTOR_PROGRESS:
        kind = "progress";
        break;
    case AGENTC_EV_ACTOR_STALLED:
        kind = "stalled";
        break;
    case AGENTC_EV_ACTOR_FINISH:
        kind = "finished";
        break;
    case AGENTC_EV_ACTOR_FAIL:
        kind = "failed";
        break;
    case AGENTC_EV_ACTOR_CANCELLED:
        kind = "cancelled";
        break;
    case AGENTC_EV_ACTOR_FROZEN:
        kind = "frozen";
        break;
    case AGENTC_EV_ACTOR_RESUMED:
        kind = "resumed";
        break;
    case AGENTC_EV_ACTOR_CHECKPOINTED:
        kind = "checkpointed";
        break;
    case AGENTC_EV_ACTOR_RESIZED:
        kind = "resized";
        break;
    default:
        break;
    }
    agentc_hook_stage_emit("lifecycle", kind, text, a != NULL ? a->id : 0);
    AgcActorEvent e;
    agentc_memset(&e, 0, sizeof e);
    e.id = a->id;
    e.parent_id = a->parent;
    e.depth = a->depth;
    e.kind = a->kind;
    e.state = a->state;
    e.text = text;
    e.json = json;
    e.exit_code = exit_code;
    if (a->started_ns != 0)
        e.duration_ms = (actor_now() - a->started_ns) / 1000000;
    if (ev == AGENTC_EV_ACTOR_RESIZED && a->proc != NULL) {
        e.cols = a->proc->cols;
        e.rows = a->proc->rows;
    }
    agentc_events_emit(NULL, ev, &e);
}

void actor_mark_progress(AgcActor *a) {
    a->last_progress_ns = actor_now();
    if (a->state == AGENTC_ACTOR_STALLED) {
        a->state = AGENTC_ACTOR_RUNNING;
        a->stalled_seen = false;
    }
}

void actor_finish_state(AgcActor *a, AgcActorState state, const char *json) {
    if (terminal(a->state))
        return;
    a->state = state;
    if (state == AGENTC_ACTOR_SETTLED) {
        actor_event(a, AGENTC_EV_ACTOR_FINISH, NULL, json,
                    a->proc != NULL ? a->proc->exit_code : 0);
    } else if (state == AGENTC_ACTOR_FAILED) {
        actor_event(a, AGENTC_EV_ACTOR_FAIL, NULL, json, a->proc != NULL ? a->proc->exit_code : 0);
    } else if (state == AGENTC_ACTOR_CANCELLED) {
        if (a->cancel_seen)
            return;
        a->cancel_seen = true;
        actor_event(a, AGENTC_EV_ACTOR_CANCELLED, NULL, json, 0);
    }
}

/* ------------------------------------------------------------------- tick */

void agentc_actor_tick(void) {
    if (!g_inited || g_ticking)
        return;
    g_ticking = true;
    i64 now = actor_now();

    for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
        AgcActor *a = &g_actors[i];
        if (a->id == 0 || terminal(a->state))
            continue;

        if (a->kind == AGENTC_ACTOR_PROCESS && a->proc != NULL)
            process_actor_poll(a);
        if (terminal(a->state))
            continue;
        /* P6-D: advance a background agent actor one (whole) turn. The submit is
         * reentrancy-guarded by g_ticking, so nested ticks are cheap no-ops. */
        if (a->kind == AGENTC_ACTOR_AGENT)
            agent_actor_step(a);
        if (terminal(a->state))
            continue;
        if (a->state == AGENTC_ACTOR_FROZEN || a->frozen_ns != 0)
            continue;

        /* stalled: advisory, once per episode, cleared by progress */
        if ((a->state == AGENTC_ACTOR_RUNNING || a->state == AGENTC_ACTOR_STALLED) &&
            a->idle_ms > 0 && !(a->proc != NULL && a->proc->reaped)) {
            i64 idle_ms = (now - a->last_progress_ns) / 1000000;
            if (idle_ms >= a->idle_ms && !a->stalled_seen) {
                a->stalled_seen = true;
                a->state = AGENTC_ACTOR_STALLED;
                actor_event(a, AGENTC_EV_ACTOR_STALLED, NULL, NULL, 0);
            }
        }

        /* hard deadline and cancel-grace escalation */
        if (a->deadline_ns != 0 && now >= a->deadline_ns) {
            a->deadline_ns = 0;
            (void)agentc_actor_cancel(a->id, AGENTC_FREEZE_ACTOR, 2);
        }
        if (a->grace_deadline_ns != 0 && now >= a->grace_deadline_ns) {
            a->grace_deadline_ns = 0;
            (void)agentc_actor_cancel(a->id, AGENTC_FREEZE_ACTOR, 2);
        }
    }
    g_ticking = false;
}

/* ---------------------------------------------------------------- control */

static void cancel_tree(AgcActorId id, int scope, int stage) {
    if (scope == AGENTC_FREEZE_SUBTREE || scope == AGENTC_FREEZE_RUNTIME) {
        for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
            AgcActor *c = &g_actors[i];
            if (c->id == 0 || c->parent != id)
                continue;
            (void)agentc_actor_cancel(c->id, scope, stage);
        }
    }
}

int agentc_actor_cancel(AgcActorId id, int scope, int stage) {
    AgcActor *a = actor_of(id);
    if (a == NULL)
        return -3; /* ESRCH */
    if (terminal(a->state))
        return 0;
    if (stage < 1)
        stage = 1;

    cancel_tree(id, scope, stage);
    if (a->state == AGENTC_ACTOR_FROZEN) {
        if (a->kind == AGENTC_ACTOR_PROCESS && a->proc != NULL)
            (void)process_actor_resume(a);
        a->state = AGENTC_ACTOR_RUNNING;
        a->frozen_ns = 0;
    }
    if (stage <= a->cancel_stage)
        return 0; /* idempotent / no downgrade */
    a->cancel_stage = stage;
    actor_event(a, AGENTC_EV_ACTOR_CANCEL_REQUESTED, NULL, NULL, 0);

    if (a->kind == AGENTC_ACTOR_PROCESS && a->proc != NULL) {
        AgcProcess *p = a->proc;
        if (stage == 1) {
            if (p->fd_in >= 0 && !p->tty) {
                os_close(p->fd_in);
                p->fd_in = -1;
                p->stdin_open = false;
            }
            if (p->pid > 0)
                os_kill(-p->pid, 15 /* SIGTERM */);
            int grace = a->grace_ms > 0 ? a->grace_ms : AGENTC_CANCEL_GRACE_DEF_MS;
            a->grace_deadline_ns = actor_now() + (i64)grace * 1000000;
        } else if (p->pid > 0) {
            os_kill(-p->pid, 9 /* SIGKILL */);
        }
    } else if (a->kind == AGENTC_ACTOR_AGENT) {
        /* P6-D: an agent actor has no OS signal to escalate; a cancel request
         * terminates it (the loop is synchronous, so there is no in-flight
         * step to interrupt here). Idempotent via terminal(). */
        actor_finish_state(a, AGENTC_ACTOR_CANCELLED, NULL);
    }
    return 0;
}

int agentc_actor_freeze(AgcActorId id, int scope, uint32_t flags) {
    (void)flags;
    AgcActor *a = actor_of(id);
    if (a == NULL)
        return -3;
    if (scope == AGENTC_FREEZE_SUBTREE || scope == AGENTC_FREEZE_RUNTIME) {
        for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
            AgcActor *c = &g_actors[i];
            if (c->id != 0 && c->parent == id)
                (void)agentc_actor_freeze(c->id, scope, flags);
        }
    }
    if (terminal(a->state) || a->state == AGENTC_ACTOR_FROZEN)
        return 0;
    if (a->kind == AGENTC_ACTOR_PROCESS && a->proc != NULL) {
        int r = process_actor_freeze(a);
        if (r < 0)
            return r;
    }
    a->frozen_ns = actor_now();
    a->state = AGENTC_ACTOR_FROZEN;
    actor_event(a, AGENTC_EV_ACTOR_FROZEN, NULL, NULL, 0);
    return 0;
}

int agentc_actor_resume(AgcActorId id, int scope) {
    AgcActor *a = actor_of(id);
    if (a == NULL)
        return -3;
    if (scope == AGENTC_FREEZE_SUBTREE || scope == AGENTC_FREEZE_RUNTIME) {
        for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
            AgcActor *c = &g_actors[i];
            if (c->id != 0 && c->parent == id)
                (void)agentc_actor_resume(c->id, scope);
        }
    }
    if (a->state != AGENTC_ACTOR_FROZEN)
        return 0;
    if (a->kind == AGENTC_ACTOR_PROCESS && a->proc != NULL)
        (void)process_actor_resume(a);
    i64 delta = actor_now() - a->frozen_ns;
    if (a->deadline_ns != 0)
        a->deadline_ns += delta;
    if (a->grace_deadline_ns != 0)
        a->grace_deadline_ns += delta;
    a->last_progress_ns = actor_now();
    a->frozen_ns = 0;
    a->state = AGENTC_ACTOR_RUNNING;
    a->stalled_seen = false;
    if (a->kind == AGENTC_ACTOR_PROCESS && a->proc != NULL)
        process_actor_after_resume(a);
    actor_event(a, AGENTC_EV_ACTOR_RESUMED, NULL, NULL, 0);
    return 0;
}

bool agentc_actor_frozen(AgcActorId id) {
    AgcActor *a = actor_of(id);
    return a != NULL && a->state == AGENTC_ACTOR_FROZEN;
}

int agentc_actor_checkpoint(AgcActorId id, const char *dir, char *err, size_t cap) {
    AgcActor *a = actor_of(id);
    if (a == NULL)
        return -3;
    if (a->kind != AGENTC_ACTOR_PROCESS || a->proc == NULL || a->proc->pid <= 0)
        return -38;
    int r = os_process_checkpoint(a->proc->pid, dir, err, cap);
    if (r == 0)
        actor_event(a, AGENTC_EV_ACTOR_CHECKPOINTED, NULL, dir, 0);
    return r;
}

/* ---------------------------------------------------------------- release */

void agentc_actor_release(AgcActorId id) {
    AgcActor *a = actor_of(id);
    if (a == NULL)
        return;
    if (a->kind == AGENTC_ACTOR_AGENT) {
        for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
            AgcActor *c = &g_actors[i];
            if (c->id != 0 && c->parent == id)
                agentc_actor_release(c->id);
        }
    }
    if (a->kind == AGENTC_ACTOR_PROCESS && a->proc != NULL)
        process_actor_release(a);
    if (a->kind == AGENTC_ACTOR_AGENT)
        agent_actor_release(a);
    agentc_message_clear(id);
    agentc_queue_clear(id);
    agentc_free(a->label);
    a->label = NULL;
    a->id = 0;
    a->proc = NULL;
    a->agent = NULL;
}

void agentc_actor_release_all(void) {
    for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
        if (g_actors[i].id != 0)
            agentc_actor_release(g_actors[i].id);
    }
}

/* ------------------------------------------------------------------ query */

AgcActorId agentc_actor_spawn(const AgcActorSpec *spec, char *err, size_t cap) {
    if (err != NULL && cap > 0)
        err[0] = 0;
    if (spec == NULL || (spec->proc == NULL && spec->agent == NULL)) {
        if (err != NULL && cap > 0)
            agentc_snprintf(err, cap, "actor spec: no spec");
        return 0;
    }
    if (spec->proc != NULL && spec->agent != NULL) {
        if (err != NULL && cap > 0)
            agentc_snprintf(err, cap, "actor spec: exactly one of proc/agent");
        return 0;
    }
    if (spec->agent != NULL) {
        AgcAgent *a = agentc_agent_create(spec->agent);
        if (a == NULL) {
            if (err != NULL && cap > 0)
                agentc_snprintf(err, cap, "agent spawn failed");
            return 0;
        }
        return agentc_agent_actor(a);
    }
    return process_actor_spawn(0, spec->proc, err, cap);
}

int agentc_actor_query(AgcActorId id, AgcBuf *out_json) {
    AgcActor *a = actor_of(id);
    if (a == NULL || out_json == NULL)
        return -3;
    agentc_buf_printf(out_json,
                      "{\"id\":%u,\"kind\":\"%s\",\"state\":\"%s\",\"parent\":%u,"
                      "\"depth\":%u",
                      (unsigned)a->id, a->kind == AGENTC_ACTOR_PROCESS ? "process" : "agent",
                      agentc_actor_state_name(a->state), (unsigned)a->parent, (unsigned)a->depth);
    if (a->label != NULL)
        agentc_buf_printf(out_json, ",\"label\":\"%s\"", a->label);
    if (a->proc != NULL) {
        agentc_buf_printf(out_json,
                          ",\"pid\":%d,\"tty\":%s,\"eof\":%s,\"total_bytes\":%llu,"
                          "\"exit_code\":%d",
                          a->proc->pid, a->proc->tty ? "true" : "false",
                          a->proc->eof ? "true" : "false", (unsigned long long)a->proc->total_bytes,
                          a->proc->exit_code);
    }
    agentc_buf_cstr(out_json, "}");
    return 0;
}

/* ------------------------------------------------ message dispatch */

/* Translate the distinct AgcMsgScope enum to the actor freeze scope (C-D1). */
static int scope_to_freeze(int scope) {
    switch (scope) {
    case AGENTC_MSG_SCOPE_SUBTREE:
        return AGENTC_FREEZE_SUBTREE;
    case AGENTC_MSG_SCOPE_RUNTIME:
        return AGENTC_FREEZE_RUNTIME;
    case AGENTC_MSG_SCOPE_ACTOR:
    case AGENTC_MSG_SCOPE_TURN:
    default:
        return AGENTC_FREEZE_ACTOR;
    }
}

int actor_handle_message(AgcActorId to, const AgcMessage *m) {
    AgcActor *a = actor_of(to);
    if (a == NULL || m == NULL)
        return -3; /* ESRCH */
    if (a->kind == AGENTC_ACTOR_AGENT)
        return agent_handle_message(a, m);
    switch (m->kind) {
    case AGENTC_MSG_INPUT:
        if (a->kind == AGENTC_ACTOR_PROCESS)
            return agentc_process_write(to, m->body, m->body_len, false);
        return 0;
    case AGENTC_MSG_CANCEL:
        return agentc_actor_cancel(to, scope_to_freeze(m->scope),
                                   m->stage == AGENTC_MSG_STAGE_FORCE ? 2 : 1);
    case AGENTC_MSG_FREEZE:
        return agentc_actor_freeze(to, scope_to_freeze(m->scope), m->flags);
    case AGENTC_MSG_RESUME:
        return agentc_actor_resume(to, scope_to_freeze(m->scope));
    default:
        return 0;
    }
}
