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

/* Stable sub-kind for the `event` stage (HOK §1: the kinds are the AGENTC_EV_*
 * names). Kept next to the hub so the stage payload can never drift from the
 * enum ordering. */
static const char *agentc_event_kind_name(int ev) {
    switch (ev) {
    case AGENTC_EV_AGENT_START:
        return "agent_start";
    case AGENTC_EV_STEP_START:
        return "step_start";
    case AGENTC_EV_MSG_START:
        return "message_start";
    case AGENTC_EV_TEXT_DELTA:
        return "text_delta";
    case AGENTC_EV_THINK_DELTA:
        return "thinking_delta";
    case AGENTC_EV_TOOL_ARGS_DELTA:
        return "tool_args_delta";
    case AGENTC_EV_MSG_END:
        return "message_end";
    case AGENTC_EV_TOOL_EXEC_START:
        return "tool_execution_start";
    case AGENTC_EV_TOOL_EXEC_END:
        return "tool_execution_end";
    case AGENTC_EV_STEP_END:
        return "step_end";
    case AGENTC_EV_AGENT_END:
        return "agent_end";
    case AGENTC_EV_ERROR:
        return "error";
    case AGENTC_EV_COMPACT:
        return "compact";
    case AGENTC_EV_MSG_RESET:
        return "message_reset";
    case AGENTC_EV_TRUST_SET:
        return "trust_set";
    case AGENTC_EV_APPROVAL_REQUEST:
        return "approval_request";
    case AGENTC_EV_APPROVAL_RESULT:
        return "approval_result";
    case AGENTC_EV_SANDBOX_DENIED:
        return "sandbox_denied";
    case AGENTC_EV_ACTOR_STARTED:
        return "actor_started";
    case AGENTC_EV_ACTOR_PROGRESS:
        return "actor_progress";
    case AGENTC_EV_ACTOR_STALLED:
        return "actor_stalled";
    case AGENTC_EV_ACTOR_INPUT:
        return "actor_input";
    case AGENTC_EV_ACTOR_STEERED:
        return "actor_steered";
    case AGENTC_EV_ACTOR_QUEUED:
        return "actor_queued";
    case AGENTC_EV_ACTOR_NOTIFY:
        return "actor_notify";
    case AGENTC_EV_ACTOR_FINISH:
        return "actor_finish";
    case AGENTC_EV_ACTOR_FAIL:
        return "actor_fail";
    case AGENTC_EV_ACTOR_CANCEL_REQUESTED:
        return "actor_cancel_requested";
    case AGENTC_EV_ACTOR_CANCELLED:
        return "actor_cancelled";
    case AGENTC_EV_ACTOR_FROZEN:
        return "actor_frozen";
    case AGENTC_EV_ACTOR_RESUMED:
        return "actor_resumed";
    case AGENTC_EV_ACTOR_CHECKPOINTED:
        return "actor_checkpointed";
    case AGENTC_EV_ACTOR_ATTACHED:
        return "actor_attached";
    case AGENTC_EV_ACTOR_DETACHED:
        return "actor_detached";
    case AGENTC_EV_HOOK_START:
        return "hook_start";
    case AGENTC_EV_HOOK_END:
        return "hook_end";
    case AGENTC_EV_QUEUE_PAUSED:
        return "queue_paused";
    case AGENTC_EV_AGENT_RESULT:
        return "agent_result";
    case AGENTC_EV_DECISION_REQUEST:
        return "decision_request";
    case AGENTC_EV_DECISION_RESULT:
        return "decision_result";
    case AGENTC_EV_ROUTE_DECIDED:
        return "route_decided";
    case AGENTC_EV_SETTINGS_CHANGED:
        return "settings_change";
    case AGENTC_EV_ACTOR_RESIZED:
        return "actor_resized";
    case AGENTC_EV_MODEL_ROUTE:
        return "model_route";
    default:
        return "event";
    }
}

void agentc_events_set_primary(AgcEventFn fn, void *ud) {
    g_fn[0] = fn;
    g_ud[0] = ud;
}

int agentc_events_subscribe(AgcEventFn fn, void *ud) {
    if (fn == NULL)
        return -22; /* EINVAL */
    for (int i = 1; i < AGENTC_EVENTS_MAX; i++) {
        if (g_fn[i] == NULL) {
            g_fn[i] = fn;
            g_ud[i] = ud;
            return i;
        }
    }
    return -12; /* ENOMEM: slots 1..7 are full */
}

void agentc_events_unsubscribe(int id) {
    if (id <= 0 || id >= AGENTC_EVENTS_MAX)
        return; /* never drop slot 0 */
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
        if (g_fn[i] != NULL)
            return true;
    return false;
}

bool agentc_events_subscribed(AgcEventFn fn) {
    if (fn == NULL)
        return false;
    for (int i = 0; i < AGENTC_EVENTS_MAX; i++)
        if (g_fn[i] == fn)
            return true;
    return false;
}

void agentc_events_emit(AgcAgent *a, int ev, const void *data) {
    if (g_quiet)
        return; /* suppresses bridge + primary */
    if (agentc_agent_event_muted(a, ev))
        return;
    /* P6-B: route every emitted event through the observe-only `event` stage.
     * The guard makes an unwatched stage zero work on the hot path. */
    if (agentc_hook_wants(AGENTC_HOOK_STAGE_EVENT))
        agentc_hook_stage_emit(AGENTC_HOOK_STAGE_EVENT, agentc_event_kind_name(ev), NULL,
                               agentc_agent_actor(a));
    agentc_agent_event_own(a, ev, data);
    for (int i = 0; i < AGENTC_EVENTS_MAX; i++)
        if (g_fn[i] != NULL)
            g_fn[i](g_ud[i], ev, data);
}

void agentc_events_reset(void) {
    agentc_memset(g_fn, 0, sizeof g_fn);
    agentc_memset(g_ud, 0, sizeof g_ud);
    g_quiet = false;
}
