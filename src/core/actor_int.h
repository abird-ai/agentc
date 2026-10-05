/* actor_int.h — private seam between core/actor.c and core/process.c (P6-A).
 * Not a public or extension ABI. */
#ifndef AGENTC_CORE_ACTOR_INT_H
#define AGENTC_CORE_ACTOR_INT_H

#include "actor.h"
#include "message.h"

/* process.c implements the kind-specific behavior. */
AgcActorId process_actor_spawn(AgcActorId parent, const AgcProcSpawn *spec, char *err, size_t cap);
void process_actor_release(AgcActor *a);
void process_actor_poll(AgcActor *a); /* read/reap/terminal */
int process_actor_freeze(AgcActor *a);
int process_actor_resume(AgcActor *a);
void process_actor_after_resume(AgcActor *a);
bool process_actor_terminal(const AgcActor *a);

/* actor.c services used by process.c. */
i64 actor_now(void);
void actor_mark_progress(AgcActor *a);
void actor_event(AgcActor *a, int ev, const char *text, const char *json, int exit_code);
void actor_finish_state(AgcActor *a, AgcActorState state, const char *json);

/* actor.c implements the default message dispatch (process kinds in P6-B;
 * agent kinds added in B3). signal.c calls it when no override handler is set. */
int actor_handle_message(AgcActorId to, const AgcMessage *m);
/* agent.c implements the agent-kind branch of the default dispatch (B3). */
int agent_handle_message(AgcActor *a, const AgcMessage *m);

#endif /* AGENTC_CORE_ACTOR_INT_H */
