/* events.h — core-internal typed agent-event hub (see core/events.c).
 *
 * Slot 0 is the global primary with replace-the-slot semantics; slots 1..7 are
 * handed out by agentc_events_subscribe(). A slot is free iff its fn is NULL.
 * The extension bridge is an ordinary subscriber, so agentc_events_quiet()
 * suppresses it together with every front end (this is what makes a quiet
 * compaction stream silent).
 *
 * The per-agent callback installed by agentc_agent_set_events() is not part of
 * this table: agentc_events_emit() dispatches it first and keeps its
 * one-callback-per-agent semantics, so multiple agents coexist.
 */
#ifndef AGENTC_CORE_EVENTS_H
#define AGENTC_CORE_EVENTS_H

#include "agent.h"

#define AGENTC_EVENTS_MAX 8          /* slot 0 = global primary, 1..7 subscribers */

void agentc_events_set_primary(AgcEventFn fn, void *ud);  /* slot 0, replace; NULL clears */
int  agentc_events_subscribe(AgcEventFn fn, void *ud);    /* id 1..7, -ENOMEM when full */
void agentc_events_unsubscribe(int id);
/* C has no overloading: the setter mirrors the quiet-gate semantics and the
 * getter keeps the agentc_events_quiet() name used by the bridge and the
 * tests. */
void agentc_events_set_quiet(bool on);
bool agentc_events_quiet(void);
bool agentc_events_wants(void);   /* any of slot 0/1..7 set */
void agentc_events_emit(AgcAgent *a, int ev, const void *data);
void agentc_events_reset(void);   /* tests only: clear slots + quiet */

/* Internal bridge implemented by core/agent.c: the hub owns the suppression
 * policy but struct AgcAgent is private to agent.c. Not an application or
 * extension API; tests exercise the behavior through agentc_events_emit(). */
bool agentc_agent_event_muted(const AgcAgent *a, int ev);
void agentc_agent_event_own(AgcAgent *a, int ev, const void *data);

#endif /* AGENTC_CORE_EVENTS_H */
