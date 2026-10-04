/* events.c — typed agent-event hub: per-agent primary, one global primary slot
 * and fixed subscriber slots behind a single quiet gate.
 *
 * Dispatch order is the agent's own primary (agentc_agent_set_events), the
 * global primary (slot 0) and then subscriber slots 1..AGENTC_EVENTS_MAX-1 in
 * ascending id order. Quiet suppresses all of it, including the extension bridge
 * subscriber, so no front end has to special-case a stream. Single-threaded,
 * like the rest of core; the table is static and emit never allocates.
 */
#include "core/events.h"

static bool g_quiet;
static AgcEventFn g_fn[AGENTC_EVENTS_MAX];
static void *g_ud[AGENTC_EVENTS_MAX];

void agentc_events_set_primary(AgcEventFn fn, void *ud) {
    g_fn[0] = fn;
    g_ud[0] = ud;
}

int agentc_events_subscribe(AgcEventFn fn, void *ud) {
    if (fn == NULL) return -22;         /* EINVAL */
    for (int i = 1; i < AGENTC_EVENTS_MAX; i++) {
        if (g_fn[i] == NULL) {
            g_fn[i] = fn;
            g_ud[i] = ud;
            return i;
        }
    }
    return -12;                         /* ENOMEM: slots 1..7 are full */
}

void agentc_events_unsubscribe(int id) {
    if (id <= 0 || id >= AGENTC_EVENTS_MAX) return;   /* never drop slot 0 */
    g_fn[id] = NULL;
    g_ud[id] = NULL;
}

void agentc_events_set_quiet(bool on) {
    g_quiet = on;
}

bool agentc_events_quiet(void) {
    return g_quiet;
}

bool agentc_events_wants(void) {
    for (int i = 0; i < AGENTC_EVENTS_MAX; i++)
        if (g_fn[i] != NULL) return true;
    return false;
}

void agentc_events_emit(AgcAgent *a, int ev, const void *data) {
    if (g_quiet) return;                        /* suppresses bridge + primary */
    if (agentc_agent_event_muted(a, ev)) return;
    agentc_agent_event_own(a, ev, data);
    for (int i = 0; i < AGENTC_EVENTS_MAX; i++)
        if (g_fn[i] != NULL) g_fn[i](g_ud[i], ev, data);
}

void agentc_events_reset(void) {
    agentc_memset(g_fn, 0, sizeof g_fn);
    agentc_memset(g_ud, 0, sizeof g_ud);
    g_quiet = false;
}
