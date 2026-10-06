/* events_test.c — core typed event hub: dispatch order (agent primary, global
 * primary, subscribers in ascending id order), subscriber id reuse, the full
 * table, the quiet gate, wants() and the per-agent cancel filter.
 */
#include "agent.h"
#include "core/events.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

/* Shared trace: every sink appends its one-letter label. */
static AgcBuf trace;

static void trace_clear(void) { trace.len = 0; }

static bool trace_is(const char *want) {
    return agentc_str_eq((const char *)trace.p, trace.len, want, agentc_strlen(want));
}

static void sink(void *ud, int ev, const void *data) {
    (void)ev;
    (void)data;
    agentc_buf_byte(&trace, (u8)*(const char *)ud);
}

static char L_A = 'A', L_B = 'B', L_C = 'C', L_D = 'D';

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    size_t base = agentc_mem_live();
    agentc_events_reset();

    AgcAgent *agent = agentc_agent_new(NULL, NULL);
    agentc_agent_set_events(agent, sink, &L_D);

    /* order: global primary A, then subscribers B (id 1) and C (id 2); the
     * agent's own D leads the chain when one is passed in. */
    agentc_events_set_primary(sink, &L_A);
    int id_b = agentc_events_subscribe(sink, &L_B);
    int id_c = agentc_events_subscribe(sink, &L_C);
    trace_clear();
    agentc_events_emit(NULL, AGENTC_EV_TURN_START, NULL);
    bool order_global = trace_is("ABC");
    trace_clear();
    agentc_events_emit(agent, AGENTC_EV_TURN_START, NULL);
    check("events.order.primary",
          id_b == 1 && id_c == 2 && order_global && trace_is("DABC"));

    /* a released id is reused by the next subscribe and lands in the same slot */
    agentc_events_unsubscribe(id_b);
    trace_clear();
    agentc_events_emit(agent, AGENTC_EV_TURN_START, NULL);
    bool after_unsub = trace_is("DAC");
    int id_b2 = agentc_events_subscribe(sink, &L_B);
    trace_clear();
    agentc_events_emit(agent, AGENTC_EV_TURN_START, NULL);
    check("events.subscribe.reuse", after_unsub && id_b2 == 1 && trace_is("DABC"));

    /* full table: ids 1..7 are the whole subscriber space */
    agentc_events_reset();
    int ids[AGENTC_EVENTS_MAX];
    int n = 0;
    int rc = 0;
    while (n < AGENTC_EVENTS_MAX && (rc = agentc_events_subscribe(sink, &L_B)) >= 0)
        ids[n++] = rc;
    bool full_ok = rc == -12 && n == AGENTC_EVENTS_MAX - 1;
    for (int i = 0; i < n; i++) agentc_events_unsubscribe(ids[i]);
    check("events.full", full_ok && !agentc_events_wants());

    /* quiet suppresses primary + subscribers, restoring delivers again */
    agentc_events_set_primary(sink, &L_A);
    (void)agentc_events_subscribe(sink, &L_B);
    agentc_events_set_quiet(true);
    trace_clear();
    agentc_events_emit(NULL, AGENTC_EV_TURN_START, NULL);
    bool suppressed = trace.len == 0;
    agentc_events_set_quiet(false);
    trace_clear();
    agentc_events_emit(NULL, AGENTC_EV_TURN_START, NULL);
    check("events.quiet", suppressed && !agentc_events_quiet() && trace_is("AB"));

    /* wants(): true for either the primary or any subscriber slot */
    agentc_events_reset();
    bool wants_none = !agentc_events_wants();
    agentc_events_set_primary(sink, &L_A);
    bool wants_primary = agentc_events_wants();
    agentc_events_set_primary(NULL, NULL);
    bool wants_cleared = !agentc_events_wants();
    int sub = agentc_events_subscribe(sink, &L_B);
    bool wants_sub = agentc_events_wants();
    agentc_events_unsubscribe(sub);
    check("events.wants",
          wants_none && wants_primary && wants_cleared && wants_sub &&
              !agentc_events_wants());

    /* cancel filter: an aborted agent suppresses ordinary events but the
     * terminal ones (here MSG_END) still reach every slot */
    agentc_events_reset();
    agentc_events_set_primary(sink, &L_A);
    (void)agentc_events_subscribe(sink, &L_B);
    agentc_agent_set_events(agent, sink, &L_D);
    agentc_agent_abort(agent);
    trace_clear();
    agentc_events_emit(agent, AGENTC_EV_TURN_START, NULL);
    bool muted = trace.len == 0;
    trace_clear();
    agentc_events_emit(agent, AGENTC_EV_MSG_END, NULL);
    check("events.cancel-filter", muted && trace_is("DAB"));

    agentc_agent_free(agent);
    agentc_events_reset();
    agentc_buf_free(&trace);
    check("events.mem", agentc_mem_live() == base);
    return fails;
}
