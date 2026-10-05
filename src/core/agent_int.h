/* agent_int.h — private seam for the P6-D agent constructor, tree and tool.
 * Not a public or extension ABI. Implemented by src/core/agent.c; consumed by
 * src/core/agent_spawn.c and src/core/tools/registry.c.
 */
#ifndef AGENTC_CORE_AGENT_INT_H
#define AGENTC_CORE_AGENT_INT_H

#include "agent.h"

/* P8-D1 (B-M8): the provider-resolution and route-target seams moved to the
 * public include/agent.h as the per-agent AgcRouteSeam; the process-global
 * setters are gone. */

/* The `agent` op-based tool executor (agent_spawn.c), registered by registry.c. */
int agent_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error);

/* Private accessors used by agent_spawn.c (struct AgcAgent is private). */
const char *agent_priv_model(const AgcAgent *a);
const char *agent_priv_provider(const AgcAgent *a);
const char *agent_priv_task(const AgcAgent *a);
const char *agent_priv_result(const AgcAgent *a);
bool agent_priv_is_subagent(const AgcAgent *a);
uint16_t agent_priv_depth(const AgcAgent *a);
bool agent_priv_ephemeral(const AgcAgent *a);
int64_t agent_priv_turn_no(const AgcAgent *a);
bool agent_priv_steerable(const AgcAgent *a, const char **kind);
int agent_priv_capture_result(AgcAgent *a, int exit_code);

/* actor.c delegates agent-actor lifecycle here. */
void agent_actor_step(AgcActor *row);
void agent_actor_release(AgcActor *row);

/* Core-internal resumable-slice result: a background agent yielded at the step
 * boundary and must be resumed by the next actor tick. Never surfaced by the
 * public `agentc_agent_submit`, which drives the same state machine to
 * completion synchronously. */
#define AGENTC_RUN_YIELD 1

#endif /* AGENTC_CORE_AGENT_INT_H */
