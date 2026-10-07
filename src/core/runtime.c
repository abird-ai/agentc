/* runtime.c — the runtime driver (P6-B; SURF §3.9, RT R-D17).
 *
 * `agentc_runtime_tick` is the single pump driver: it drains the message bus
 * (so a child's notify/approval/input can never deadlock a wait) and then
 * advances the actor registry. `runtime_freeze/resume` are the global pause;
 * `runtime_shutdown` reaps every actor.
 */
#include "agent.h"

#include "base/limits.h"
#include "core/actor_int.h"
#include "plat.h"

static bool g_rt_inited;

/* ------------------------------------------------ runtime continuations */
typedef struct {
    bool used;
    uint64_t token;
    AgcDeferFn resume;
    void *ud;
} RuntimeDefer;

static RuntimeDefer g_defers[AGENTC_RUNTIME_MAX_DEFERS];
static uint64_t g_defer_next = 1;
static size_t g_defer_count;

uint64_t agentc_runtime_defer(AgcDeferFn resume, void *ud) {
    if (resume == NULL)
        return 0;
    for (size_t i = 0; i < AGENTC_RUNTIME_MAX_DEFERS; i++) {
        if (g_defers[i].used)
            continue;
        g_defers[i].used = true;
        g_defers[i].token = ++g_defer_next;
        g_defers[i].resume = resume;
        g_defers[i].ud = ud;
        g_defer_count++;
        return g_defers[i].token;
    }
    return 0; /* bounded: the caller fails closed */
}

void agentc_runtime_cancel_defer(uint64_t token) {
    if (token == 0)
        return;
    for (size_t i = 0; i < AGENTC_RUNTIME_MAX_DEFERS; i++) {
        if (!g_defers[i].used || g_defers[i].token != token)
            continue;
        g_defers[i].used = false;
        g_defers[i].token = 0;
        g_defers[i].resume = NULL;
        g_defers[i].ud = NULL;
        if (g_defer_count > 0)
            g_defer_count--;
        return;
    }
}

size_t agentc_runtime_defer_count(void) {
    return g_defer_count;
}

/* Fire every parked continuation once. A continuation may cancel itself (the
 * common case) or register a new one; snapshot the tokens first so a mutation
 * during the walk cannot skip or double-fire a row. */
static void runtime_fire_defers(void) {
    uint64_t tokens[AGENTC_RUNTIME_MAX_DEFERS];
    size_t n = 0;
    for (size_t i = 0; i < AGENTC_RUNTIME_MAX_DEFERS; i++)
        if (g_defers[i].used)
            tokens[n++] = g_defers[i].token;
    for (size_t k = 0; k < n; k++) {
        AgcDeferFn fn = NULL;
        void *ud = NULL;
        for (size_t i = 0; i < AGENTC_RUNTIME_MAX_DEFERS; i++) {
            if (g_defers[i].used && g_defers[i].token == tokens[k]) {
                fn = g_defers[i].resume;
                ud = g_defers[i].ud;
                break;
            }
        }
        if (fn != NULL)
            fn(ud, NULL);
    }
}

int agentc_runtime_init(const struct AgcTrustEnv *root) {
    (void)root;          /* P6-C fills the trust envelope */
    agentc_actor_init(); /* also initializes messages + queue */
    agentc_memset(g_defers, 0, sizeof g_defers);
    g_defer_count = 0;
    g_rt_inited = true;
    return 0;
}

void agentc_runtime_tick(void) {
    /* Lazy and non-destructive: if the runtime was never explicitly initialized
     * the registry owns its own lazy init, so do not reset it here. */
    (void)agentc_message_drain(0, 0);
    agentc_actor_tick();
    /* Resume parked work after the actors advanced: a branch continuation sees
     * its child's terminal state here. */
    runtime_fire_defers();
}

void agentc_runtime_shutdown(const char *reason) {
    (void)reason;
    /* R-D17: issue a graceful turn cancel to the whole tree, let the driver
     * poll/reap within a bounded number of slices, then force-release. A
     * process actor gets SIGTERM here and its grace/force escalation happens
     * through agentc_actor_tick; the loop is bounded so shutdown never hangs. */
    for (size_t i = 1; i <= AGENTC_ACTOR_MAX; i++) {
        if (!agentc_actor_exists((AgcActorId)i))
            continue;
        (void)agentc_actor_cancel((AgcActorId)i, AGENTC_FREEZE_RUNTIME, 1);
    }
    for (int slice = 0; slice < AGENTC_RUNTIME_SHUTDOWN_SLICES; slice++) {
        agentc_actor_tick();
        AgcActor rows[AGENTC_ACTOR_MAX];
        size_t n = agentc_actor_list(rows, AGENTC_ACTOR_MAX);
        bool live = false;
        for (size_t i = 0; i < n; i++) {
            AgcActorState st = rows[i].state;
            if (st != AGENTC_ACTOR_SETTLED && st != AGENTC_ACTOR_FAILED &&
                st != AGENTC_ACTOR_CANCELLED) {
                live = true;
                break;
            }
        }
        if (!live)
            break;
        os_sleep_ns((i64)AGENTC_RUNTIME_SHUTDOWN_SLICE_MS * 1000000);
    }
    agentc_actor_release_all();
    agentc_memset(g_defers, 0, sizeof g_defers);
    g_defer_count = 0;
    g_rt_inited = false;
}

int agentc_runtime_freeze(uint32_t flags) {
    int rc = 0;
    for (size_t i = 1; i <= AGENTC_ACTOR_MAX; i++) {
        if (!agentc_actor_exists((AgcActorId)i))
            continue;
        int r = agentc_actor_freeze((AgcActorId)i, AGENTC_FREEZE_RUNTIME, flags);
        if (r < 0 && rc == 0)
            rc = r;
    }
    return rc;
}

int agentc_runtime_resume(void) {
    int rc = 0;
    for (size_t i = 1; i <= AGENTC_ACTOR_MAX; i++) {
        if (!agentc_actor_exists((AgcActorId)i))
            continue;
        int r = agentc_actor_resume((AgcActorId)i, AGENTC_FREEZE_RUNTIME);
        if (r < 0 && rc == 0)
            rc = r;
    }
    return rc;
}
