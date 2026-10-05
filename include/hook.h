/* hook.h — the universal interception pipeline (P6-B core; see P6-HOOKS.md).
 *
 * One generic hook for every flow: turn, step, input, prompt, message, output,
 * tool, signal, event, lifecycle. Two verbs only: a handler returns the next
 * message (or NULL to consume) and may set *defer to park the flow. The
 * registry is bounded (AGENTC_HOOK_MAX_HANDLERS) and chains by (order,
 * registration seq). The legacy extension bus (`on(point,…)`) stays the
 * C-handler backend and is unaffected.
 *
 * Core-internal: AgcMessage/AgcHookFn do not cross the stable extension ABI
 * (the host slot takes a JSON payload + AgcExtHookFn, adapted internally).
 *
 * Owner: src/core/hook.c.
 */
#ifndef AGENTC_HOOK_H
#define AGENTC_HOOK_H

#include "message.h"

struct AgcAgentSpec;

/* Return 0 to continue the chain; `*next` is the message the chain continues
 * with (NULL consumes the flow and stops the walk). A non-zero `*defer` parks
 * the flow for agentc_runtime_step and yields -EINPROGRESS from run. */
typedef int (*AgcHookFn)(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer);

uint64_t agentc_hook(const char *stage, int order, AgcHookFn fn, void *ud);
void agentc_hook_off(uint64_t handle);
int agentc_hook_run(const char *stage, AgcMessage *m, AgcMessage **out);
/* Like agentc_hook_run, but writes the parked continuation token (0 on no
 * park) so a caller that must cancel or resume it can address it. */
int agentc_hook_run_defer(const char *stage, AgcMessage *m, AgcMessage **out, uint64_t *defer);
void agentc_hook_cancel(uint64_t token);

/* B8 fold foundation. Run the `stage` core chain and, unless a core handler
 * consumed the message, the mapped legacy `point` exactly once. The legacy
 * result is transferred onto `m` as owned `result_json`/`handled`/`blocked`,
 * so an override point is expressed on the one hook channel instead of a
 * second direct `agentc_ext_emit` at the call site. `payload_json` defaults to
 * the message body. Returns the hook_run code. */
int agentc_hook_run_legacy(const char *stage, const char *point, AgcMessage *m,
                           const char *payload_json, AgcMessage **out);
void agentc_hook_result_free(AgcMessage *m);
int agentc_hook_resume(uint64_t token, AgcMessage *m);
bool agentc_hook_wants(const char *stage);
int agentc_hook_branch(const struct AgcAgentSpec *spec, AgcMessage *m, uint64_t *defer);

/* Re-entry/consume seam (completion pass 2d). `agentc_hook_branch` resumes the
 * chain at the handler after the one that parked. A controller that must run
 * its own post-branch apply calls `agentc_hook_branch_self` instead: it marks
 * the returned token so `agentc_hook_resume` re-enters the *same* handler with
 * `agentc_hook_in_reentry()` true and the branch result in `m->body`. Inside a
 * self-resumed handler `agentc_hook_in_reentry()` is true only for its own
 * re-entry, so a handler that parks by hand can call `agentc_hook_mark_self`
 * with its `*defer` token too. */
int agentc_hook_branch_self(const struct AgcAgentSpec *spec, AgcMessage *m, uint64_t *defer);
void agentc_hook_mark_self(uint64_t token);
bool agentc_hook_in_reentry(void);

/* Core convenience: run one stage with a synthetic JSON-less message carrying
 * `kind` (sub-kind) and `text`. No-op when the stage has no handlers. */
void agentc_hook_stage_emit(const char *stage, const char *kind, const char *text,
                            uint32_t actor_id);

/* Fresh synthetic message id for reentry/depth identity (core-internal). */
uint64_t agentc_hook_next_id(void);

#define AGENTC_HOOK_STAGE_TURN "turn"       /* start|end|steer */
#define AGENTC_HOOK_STAGE_STEP "step"       /* start|end|model_select|compact */
#define AGENTC_HOOK_STAGE_INPUT "input"     /* user|child|queue */
#define AGENTC_HOOK_STAGE_PROMPT "prompt"   /* system */
#define AGENTC_HOOK_STAGE_MESSAGE "message" /* user|assistant|tool|system */
#define AGENTC_HOOK_STAGE_OUTPUT "output"   /* text|think|args|audio|item */
#define AGENTC_HOOK_STAGE_TOOL "tool"       /* call|result */
#define AGENTC_HOOK_STAGE_SIGNAL "signal"
#define AGENTC_HOOK_STAGE_EVENT "event"
#define AGENTC_HOOK_STAGE_LIFECYCLE "lifecycle"
#define AGENTC_HOOK_STAGE_APPROVAL "approval"
/* Appended completion-pass 2f stages: the settlement point and the compaction
 * gate get dedicated core stages so their legacy `agent_before_settle` /
 * `session_before_compact` points fold through `agentc_hook_run_legacy`
 * instead of a second direct `agentc_ext_emit`. `veto` carries the legacy
 * `tool_call` veto algebra without re-running the `tool` call decision seam. */
#define AGENTC_HOOK_STAGE_SETTLE "settle"
#define AGENTC_HOOK_STAGE_COMPACT "compact"
#define AGENTC_HOOK_STAGE_VETO "veto"
/* Appended completion-pass 2g stage: the four provider interception points
 * (`before_provider_headers`, `before_provider_request`,
 * `after_provider_response`, `provider_stream_event`) have no dedicated stage
 * of their own, so they fold through one `provider` stage with the sub-kind in
 * the message meta: headers|request (override, fail-closed) and
 * response|stream_event (observe, fail-open). The legacy point keeps its exact
 * semantics, run once through agentc_hook_run_legacy. */
#define AGENTC_HOOK_STAGE_PROVIDER "provider"

#define AGENTC_HOOK_ERR_PASS 0
#define AGENTC_HOOK_ERR_DENY 1

#endif /* AGENTC_HOOK_H */
