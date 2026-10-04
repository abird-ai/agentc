# P6-SURFACE — Complete surface registry and coverage audit

Status: **as-built (P6-A..E + completion passes 2a-2g).** The surface registry and coverage audit.
As-built note: the outcome is folded into `.agents/design/30-extensibility.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

> For the **minimal primitive core** (runtime/actor/signal/queue/envelope), the
> async contract, and the derivation of every surface below, see
> `P6-PRIMITIVES.md`. This file is the exhaustive *adapter* registry: it lists
> every named API/event/hook/tool/RPC/flag/key/limit/error code. Per
> `P6-PRIMITIVES.md` §6, many entries here are thin wrappers over
> `agentc_message_send`/`agentc_actor_read`/`agentc_actor_query` and may be
> collapsed to op-based forms.

Read with `P6-RUNTIME.md` (decisions R-D1…R-D31, area-local S-D1…S-D6).

Marking:

- **[NEW]** — added by this audit (not yet in the area specs; fold into them
  when the owning wave lands).
- Everything else is already declared in the area specs; listed here so the
  whole surface is in one place.
- **Deferred** / **Out of scope** are called out explicitly in §18–§19.

---

## 1. Layer 0 — `include/plat.h` (appended)

| function | purpose | status |
| --- | --- | --- |
| `os_pty_open(int *master, int *slave, int cols, int rows)` | allocate a PTY pair | declared |
| `os_pty_resize(int fd, int cols, int rows)` | resize a PTY | declared |
| `os_pty_size(int fd, int *cols, int *rows)` | query a PTY | declared |
| `os_sandbox_apply(const AgcSandboxSpec *, char *err, size_t)` | apply the effective sandbox before exec | declared |
| `os_test_clock_set(i64 ns)` | **[NEW]** injectable monotonic clock for deterministic goldens (headless backend only; compiled out of release) | audit |
| `os_test_syscall_log(...)` | **[NEW]** recorded kill/spawn log for goldens (headless backend only) | audit |

No existing `plat.h` signature changes. `os_kill`/`os_wait`/`os_pipe`/
`os_spawn_group`/`os_which`/`os_tty_*` are reused unchanged.

## 2. Core headers and types

New headers: `include/actor.h`, `include/message.h`, `include/trust.h`; all
included from `include/agent.h` at the end (append-only).

| type | home | purpose | status |
| --- | --- | --- | --- |
| `AgcActorId`, `AgcActorKind`, `AgcActorState`, `AgcActor` | `actor.h` | actor registry row | declared |
| `AgcProcess`, `AgcProcSpawn`, `AgcProcInfo` | `actor.h` | process payload/spec/info | declared |
| `AgcMessage`, `AgcMessageKind`, `AgcMsgScope`, `AgcMsgStage` | `message.h` | control/notify message | declared |
| `AgcQueueItem`, `AGENTC_Q_F_*`, `AGENTC_PRIO_*` | `message.h` | queue item + flags + priority range | declared |
| `AgcAccess`, `AgcBoundary`, `AgcApproval`, `AgcNetwork`, `AgcTrustEnv`, `AgcTrustDecision`, `AgcSandboxSpec`, `AGENTC_TRUST_*` | `trust.h` | trust envelope + decision + sandbox spec | declared |
| `AgcPolicyRule`, `AgcPolicy` | `trust.h` | **[NEW]** compiled rule set (opaque) | audit |
| `AgcApprovalRequest`, `AgcApprovalResult` | `trust.h` | **[NEW]** typed approval structs (JSON is the wire form) | audit |
| `AgcAgentSpec`, `AGENTC_AGENT_F_*`, `AgcAgentInfo` | `agent.h` | unified agent constructor + tree view | declared |
| `AgcActorEvent` | `agent.h` | actor event payload | declared |
| `AgcTrustEvent`, `AgcApprovalEvent`, `AgcQueueEvent` | `agent.h` | **[NEW]** payloads for the trust/approval/queue events | audit |

## 3. Core C APIs

### 3.1 Actor (`src/core/actor.c`)

`agentc_actor_init`, `agentc_actor_register`, `agentc_actor_get`,
`agentc_actor_parent`, `agentc_actor_list`, `agentc_actor_release`,
`agentc_actor_tick`, `agentc_actor_state_name` — declared.

**[NEW] audit additions:**

| function | purpose |
| --- | --- |
| `bool agentc_actor_exists(AgcActorId)` | existence check without a pointer |
| `int agentc_actor_info(AgcActorId, AgcActor *out)` | copy a snapshot |
| `int agentc_actor_set_label(AgcActorId, const char *)` | update the UI label |
| `size_t agentc_actor_count(AgcActorKind kind_or_any)` | count without a list |
| `AgcActorId agentc_actor_root(void)` | the root agent actor |
| `int agentc_actor_freeze(AgcActorId, int scope, uint32_t flags)` | suspend an actor/subtree/global at its current state (R-D18) |
| `int agentc_actor_resume(AgcActorId, int scope)` | continue a frozen actor/subtree/global |
| `bool agentc_actor_frozen(AgcActorId)` | query the frozen state |
| `int agentc_actor_checkpoint(AgcActorId, const char *dir, char *err, size_t)` | optional process-tree checkpoint (CRIU; `-ENOSYS` elsewhere) |

### 3.2 Process (`src/core/process.c`)

`agentc_actor_spawn`, `agentc_process_write`, `agentc_process_read`,
`agentc_process_tail`, `agentc_process_info` — declared.

**[NEW] audit additions:**

| function | purpose |
| --- | --- |
| `int agentc_process_resize(AgcActorId, int cols, int rows)` | PTY resize (currently only via `INPUT{resize}`) |
| `int agentc_process_kill(AgcActorId, int stage)` | sugar over `CANCEL{scope:actor}` |
| `int agentc_process_spill_path(AgcActorId, char *out, size_t cap)` | locate the full-output spill for a late reader |
| `int agentc_process_set_stdin(AgcActorId, bool open)` | explicit stdin open/close (idempotent) |

### 3.3 Signal (`src/core/signal.c`)

`agentc_message_init`, `agentc_message_send`, `agentc_message_drain`,
`agentc_message_pending`, `agentc_message_clear` — declared.

**[NEW] audit additions:**

| function | purpose |
| --- | --- |
| `int agentc_message_to_json(const AgcMessage *, AgcBuf *out)` | serialize for RPC/hooks/audit |
| `int agentc_message_from_json(const char *, size_t, AgcMessage *out)` | parse an RPC/host-supplied signal |
| `int agentc_message_cancel_subtree(AgcActorId, int stage)` | broadcast cancel (depth-first) without hand-building a signal |

### 3.4 Queue (`src/core/queue.c`)

`agentc_queue_add`, `agentc_queue_list`, `agentc_queue_update`,
`agentc_queue_delete`, `agentc_queue_reorder`, `agentc_queue_start` — declared.

**[NEW] audit additions:**

| function | purpose |
| --- | --- |
| `int agentc_queue_get(AgcActorId, uint64_t item, AgcQueueItem *out)` | fetch one item |
| `size_t agentc_queue_count(AgcActorId)` | depth without a list |
| `int agentc_queue_clear(AgcActorId)` | drop all pending items (audited) |
| `int agentc_queue_pause(AgcActorId, bool paused)` | pause auto-drain (mirrors Codex `queue paused`) |
| `bool agentc_queue_is_paused(AgcActorId)` | query pause state |
| `int64_t agentc_queue_turn_of(AgcActorId)` | the agent's current `turn_seq` (or via agent API) |

### 3.5 Trust (`src/core/trust.c`)

`agentc_trust_decide`, `agentc_trust_resolve`, `agentc_trust_save` — declared.

**[NEW] audit additions:**

| function | purpose |
| --- | --- |
| `void agentc_trust_env_free(AgcTrustEnv *)` | release an owned envelope |
| `int agentc_trust_env_clone(const AgcTrustEnv *, AgcTrustEnv *out)` | deep copy |
| `int agentc_trust_default(AgcTrustEnv *out, const char *cwd)` | the built-in default |
| `int agentc_trust_narrow(const AgcTrustEnv *parent, const AgcTrustEnv *requested, AgcTrustEnv *out, char *err, size_t)` | monotonic intersection |
| `int agentc_trust_from_json(const char *, size_t, AgcTrustEnv *out, char *err, size_t)` | parse a spec/envelope |
| `int agentc_trust_to_json(const AgcTrustEnv *, AgcBuf *out)` | serialize (prompt, UI, audit) |
| `int agentc_trust_describe(const AgcTrustEnv *, AgcBuf *out)` | human/system-prompt rendering |
| `int agentc_trust_sandbox_spec(const AgcTrustEnv *, AgcSandboxSpec *out)` | build the Layer-0 spec |
| `bool agentc_trust_path_ok(const AgcTrustEnv *, const char *path, bool write)` | path check helper |
| `bool agentc_trust_host_ok(const AgcTrustEnv *, const char *host)` | host check helper |
| `bool agentc_trust_load(const char *cwd, bool project_trusted, AgcTrustEnv *out)` | load user (+trusted project) policy |

### 3.6 Policy (`src/core/trust.c`)

**[NEW] audit additions:**

| function | purpose |
| --- | --- |
| `AgcPolicy *agentc_policy_load(const char *path, char *err, size_t)` | load/compile a `policy.jsonc` |
| `int agentc_policy_add_json(AgcPolicy *, const char *json)` | contribute rules (hook/config) |
| `void agentc_policy_free(AgcPolicy *)` | release |
| `int agentc_policy_eval(const AgcPolicy *, const char *tool, const char *args_json, AgcTrustDecision *out)` | first-match evaluation |

### 3.7 Approval (`src/core/approval.c`)

Dispatch described in `P6-TRUST.md` §7; the C surface was implicit. **[NEW]**
explicit API:

| function | purpose |
| --- | --- |
| `int agentc_approval_request(AgcActorId, const AgcToolCall *, AgcApprovalRequest *out)` | build a request (JSON-ready) |
| `int agentc_approval_submit(const char *response_json)` | deliver a response (from UI/program/model) |
| `size_t agentc_approval_pending(AgcActorId, AgcApprovalResult *out, size_t max)` | list pending |
| `int agentc_approval_cancel(uint64_t approval_id, const char *reason)` | resolve as deny |
| `int agentc_approval_scope_remember(uint64_t approval_id, int scope)` | persist/memoize a decision |
| `bool agentc_approval_is_pending(AgcActorId)` | query |

### 3.8 Agent tree (`src/core/agent_spawn.c` / `agent.c`)

`agentc_agent_create`, `agentc_agent_list` — declared. Existing setters/submit/
abort/load/compact are unchanged.

**[NEW] audit additions:**

| function | purpose |
| --- | --- |
| `AgcAgent *agentc_agent_find(AgcActorId)` | live agent by actor id |
| `int agentc_agent_info(AgcActorId, AgcAgentInfo *out)` | snapshot |
| `AgcActorId agentc_agent_parent(AgcActorId)` | lineage |
| `int agentc_agent_wait(AgcActorId, i64 timeout_ms, AgcBuf *result)` | pumped wait; **services the signal bus** (see §18) |
| `int agentc_agent_send(AgcActorId, const char *text, int64_t step, int priority, uint32_t flags, const char *client_id)` | sugar over `agentc_queue_add` |
| `int agentc_agent_cancel(AgcActorId, int scope, int stage)` | sugar over the cancel signal |
| `int agentc_agent_result(AgcActorId, AgcBuf *out)` | final text + usage/exit |
| `int64_t agentc_agent_turn_no(const AgcAgent *)` | current turn counter |
| `bool agentc_agent_steerable(const AgcAgent *, const char **kind)` | steerability + reserved kind |
| `int agentc_agent_result_set(AgcActorId, const char *text, int exit_code)` | set the child result once |

### 3.9 Runtime (`src/core/runtime.c`) **[NEW]**

| function | purpose |
| --- | --- |
| `int agentc_runtime_init(const AgcTrustEnv *root)` | init actors + signals + trust + queue; install atexit reap |
| `void agentc_runtime_tick(void)` | one tick (actor tick + signal drain); called by the pump |
| `void agentc_runtime_shutdown(const char *reason)` | graceful→force reap all actors, flush session, close |
| `int agentc_runtime_freeze(uint32_t flags)` | freeze every actor (global pause, R-D18) |
| `int agentc_runtime_resume(void)` | resume a global freeze |
| `size_t agentc_runtime_actor_snapshot(AgcActorInfo *out, size_t max)` | UI/debug snapshot |

## 4. Events

### 4.1 Enum additions (append after `AGENTC_EV_MSG_RESET`)

Trust/approval: `AGENTC_EV_TRUST_SET`, `AGENTC_EV_APPROVAL_REQUEST`,
`AGENTC_EV_APPROVAL_RESULT`, `AGENTC_EV_SANDBOX_DENIED`.

Actor: `AGENTC_EV_ACTOR_STARTED`, `_PROGRESS`, `_STALLED`, `_INPUT`,
`_STEERED`, `_QUEUED`, `_NOTIFY`, `_FINISH`, `_FAIL`, `_CANCEL_REQUESTED`,
`_CANCELLED`, `_ATTACHED`, `_DETACHED`.

**[NEW] audit additions (gaps):**

| event | why |
| --- | --- |
| `AGENTC_EV_ACTOR_RESIZED` | PTY resize is otherwise invisible to UIs |
| `AGENTC_EV_AGENT_RESULT` | parent-side final result without subscribing to the child's stream |
| `AGENTC_EV_QUEUE_PAUSED` | pause/resume observability |
| `AGENTC_EV_ACTOR_FROZEN` | freeze confirmed |
| `AGENTC_EV_ACTOR_RESUMED` | resume confirmed |
| `AGENTC_EV_ACTOR_CHECKPOINTED` | optional checkpoint written (carries path) |

### 4.2 Payload structs

`AgcActorEvent` (declared), plus **[NEW]** `AgcTrustEvent`
`{actor_id, effective_json}`, `AgcApprovalEvent`
`{id, actor_id, tool, decision, scope, reason}`, `AgcQueueEvent`
`{actor_id, op, item_id, step, priority, count}`.

### 4.3 JSON payload contract

Every event payload is a flat JSON object; the actor payload carries
`{actor_id, parent_id, depth, kind, state, turn?, priority?, at?, exit_code?,
duration_ms?}`. Field names are stable (RPC/session/hook consumers depend on
them).

## 5. Hook points (append to `include/agentc_ext.h`)

`actor_started`, `actor_progress`, `actor_stalled`, `actor_input`,
`actor_steered`, `actor_notify`, `actor_finish`, `actor_fail`,
`actor_cancel_requested`, `actor_cancelled`, `trust_select`, `tool_approval`,
`sandbox_denied`, `policy_rules` — declared.

> **These are aliases for pipeline stages/kinds (R-D23, `P6-HOOKS.md` §1),** not
> separate mechanisms. `tool_approval`→`tool`/`call`; the step points→`step`;
> message points→`message`; the actor points→`lifecycle`/`signal`. Decisions
> (`route`, `approve`, `spawn`, `model_select`, `compact`, `steer_policy`) are
> handlers on `input`/`tool`/`step`, not points. Registration is
> `agentc_hook(stage, order, fn, ud)` (host `hook`); the point names remain valid
> as compatibility aliases.

**[NEW] audit additions:**

| point | kind | why |
| --- | --- | --- |
| `actor_resized` | observe | parity with the new event |
| `agent_result` | observe | parent-side result |
| `queue_changed` | observe | pause/add/update/delete/reorder in one point |
| `runtime_init` / `runtime_shutdown` | observe | extensions can prepare/clean resources with actors |

## 6. Extension host ABI (`AgcExtHost`, appended, each gated by `AGENTC_EXT_HOST_HAS`)

Per R-D25 the host surface mirrors the primitives — one spawn, one
`actor_op`, one `actor_query`, `actor_read`, `actor_await`, the signal/queue
codecs, and trust/approval/policy. There are **no per-feature host methods**.
Canonical list (also in `P6-INTEGRATION.md` §1.3): `actor_spawn`,
`actor_spawn`, `actor_op`, `actor_query`, `actor_read`, `actor_await`,
`message_send`, `message_from_json`, `message_to_json`, `queue_op`, `get_trust`,
`set_trust`, `submit_approval`, `policy_eval`, `hook`, `hook_off`,
`decider_register`, `add_status_line`. Owned JSON is freed with `host->free`.

Earlier named host wrappers (`actor_list/cancel/input/freeze/resume`,
`agent_spawn/list/send/wait/result`, `process_spawn/read/write/tail/resize/
kill`) are **not** shipped as host methods; extensions call `actor_op`/
`actor_query` with the op/what vocabulary, or `message_send` directly.

`AgcExtResult` append: `policyPaths` (declared), plus `trustPatch`
(an extension narrowing/overriding an envelope at `trust_select`).

## 6b. Deciders and router (P6-DECIDERS) **[design addendum]**

No new stage or primitive (R-D23): a decider is a **hook handler** that
`hook_branch`es to a backend and returns the (routed/edited/consumed) message.
Decisions live on the data stages, not their own stage:

| intent | stage | backend |
| --- | --- | --- |
| classify/route | `input` | `local`/`model` (classifier) |
| approve/deny/narrow | `tool`/`call` | `local`/`program`/`mcp`/**jev** |
| sub-agent spawn | `tool`/`call` (`agent` op `spawn`) | jev |
| model_select / compact | `step` | local/model |

**Controller decision (R-D28):** one `tool`/`call` handler that branches to jev
for every call and applies a **multi-effect** record — the call-level verdict is
the returned message (allow/narrow/deny/**replace with `agent op:spawn`**), and
other effects are primitives: `hook_branch` to a human for approval,
`message_send` of CONTROL **`SET_MODEL`/`SET_THINKING`/`SETTLE`**, INPUT for
queue/steer, `CANCEL` to stop a turn. No core action-list interpreter.

Surfaces:

| surface | item |
| --- | --- |
| signal kinds | `AGENTC_MSG_DECIDE`, `AGENTC_MSG_DECIDED` (the branch round-trip) |
| C API | `agentc_decide(spec, m, &defer)` (sugar over `hook_branch`), `agentc_decision_submit(response_json)` |
| host | `decider_register(host, spec_json)` |
| config | `deciders.<id> {kind: local\|program\|mcp\|model, …}`; a per-stage `decider` handler in `hooks.<stage>` |
| events | `AGENTC_EV_DECISION_REQUEST`, `AGENTC_EV_DECISION_RESULT`, `AGENTC_EV_ROUTE_DECIDED` |
| session | `decision {intent, stage, actor_id, decider, model?, input_hash, verdict, scope, reason, cost_micro}` |
| errors | `decision_denied`, `decision_timeout`, `decision_cycle`, `decision_hops`, `route_invalid`, `route_widen_denied` |

Decision points: `route`, `approve`, `model_select`, `spawn`, `compact`,
`steer_policy`, `escalate`. The **classified router** is a decider at `route`;
**jev** is a `model` decider (typically `approve`, optionally `route`/`escalate`).
A decision may only narrow the envelope; the core validates and applies it via
the primitives. See `P6-DECIDERS.md`.

## 6c. Hook pipeline (P6-HOOKS) **[design addendum]**

A **service**, not a primitive (R-D22): an ordered transform chain over messages.
Two verbs only. Surfaces:

| surface | item |
| --- | --- |
| C API | `agentc_hook(stage, order, fn, ud)`, `agentc_hook_off(handle)`, `agentc_hook_run(stage, m, out)`, `agentc_hook_resume(token, m)`, `agentc_hook_wants(stage)`, `agentc_hook_branch(spec, m, &defer)` |
| contract | `AgcHookFn(ud, m, next, defer)` — return the next message (or `NULL` to consume), set `defer` to park |
| derived verbs | `pass`/`edit`/`replace`/`observe` = which message you return; `stop` = `NULL`; `emit`/`fork` = `message_send`; `branch` = `hook_branch` |
| stages | `turn`, `step`, `input`, `prompt`, `message`, `output`, `tool`, `signal`, `event`, `lifecycle` |
| runners | `AgcToolRunner{kind: internal\|process\|mcp}`, `AGENTC_TOOL_NO_HOOKS`, `AGENTC_TOOL_NO_APPROVAL`; every external/MCP process is a process actor |
| host | `hook(stage, order, fn, ud)` → handle; `hook_off(handle)` (supersedes `on(point, …)`, kept as alias) |
| events | `AGENTC_EV_HOOK_START`, `AGENTC_EV_HOOK_END` |
| config | `hooks.<stage> = [ {order, program\|decider\|mcp, on_error}, … ]`, `hooks.on_error_default` |
| session | `hook {stage, item_id, handler, before_hash, after_hash}` |
| RPC | `hook_list {stage}`, `hook_set {stage, spec}` |
| errors | `hook_denied`, `hook_timeout`, `hook_depth`, `hook_reentry`, `hook_malformed` |
| guard | `AGENTC_HOOK_MAX_DEPTH` (default 8), per-item reentry set |

`hook_branch` spawns/uses an actor for the message and `defer`s the chain until
it finishes (result or parsed verdict). The extension hook bus is the C-handler
backend; `P6-TRUST` approval is a `tool_call` handler; `P6-DECIDERS` router/jev
are handlers on `route`/`decision`. See `P6-HOOKS.md`.

## 6d. Realtime voice (P6-VOICE) **[design addendum]**

No new primitive, actor kind, tool or host method. The core addition is one
opaque field (`AgcMessage.content_type`) plus a process spec mode and two trust
flags; the rest is an extension.

| surface | item |
| --- | --- |
| message | `AgcMessage.content_type` (`text/plain`, `application/json`, `audio/pcm;…`, `application/x-realtime-item`) — opaque to the core |
| process | `AGENTC_PROC_PIPE_BIDI` (writable stdin pipe; **also required by MCP stdio servers**) |
| trust | `AGENTC_TRUST_ALLOW_MIC`, `AGENTC_TRUST_ALLOW_SPEAKER` |
| control | `REALTIME_START` / `REALTIME_STOP` messages (extension JSON body) |
| extension | hooks on `input`/`output`/`step`/`signal`, command `/voice`, status `voice`, config `realtime.*`, the voice helper as a `process` actor |

The voice helper owns WebRTC/audio; items promote via an `input` hook; realtime
tool calls are ordinary `tool` calls; barge-in is `CANCEL{scope:turn}`. See
`P6-VOICE.md`.

## 6e. Settings and prompt contributions (P6-SETTINGS) **[design addendum]**

A **service over the JSONC config loader** (no new primitive).

**Settings** are typed values, grouped by the **extension's own name** (no group
object). Values live in `config.jsonc` under `settings.<extension>.<key>`;
secrets in `auth.jsonc`.

| surface | item |
| --- | --- |
| type | `AgcExtSetting {key,label,description,type,default_json,enum_json,min/max,unit,flags}` |
| ABI | `add_setting`; host `setting_get`/`setting_set` (`ext`,`key`) |
| core | `agentc_settings_get_json/str/int/bool/float`, `set/reset/list` |
| front ends | TUI `/settings [ext]` (one group per extension); RPC `get_settings`/`set_setting`/`reset_setting`; CLI `--set`/`--list-settings` |
| events/hooks | `AGENTC_EV_SETTINGS_CHANGED`, `settings_change` hook |
| flags | `SECRET` (auth.jsonc), `ADVANCED`, `RESTART`, `PROJECT` (trust-gated) |

**Prompt contributions** (the renamed "section") add **system-prompt text**
before/after the four core sections:

| surface | item |
| --- | --- |
| type | `AgcExtPrompt {key,priority,render,anchor,position,mode}` |
| ABI | `add_prompt` |
| anchor | `ALL` (default) \| `PREAMBLE` \| `TOOLS` \| `RULES` \| `ENVIRONMENT` |
| mode | `APPEND` (default; `position` BEFORE/AFTER) \| `REPLACE` (supersede the anchor's default render) |
| precedence | `--system` > `REPLACE(ALL)` > per-anchor `REPLACE` (priority) > core default; `APPEND` around it |
| note | **preamble is now overridable** (`anchor=PREAMBLE, mode=REPLACE`); tools/rules are tool-driven (`prompt_snippet`/`prompt_guidelines`); environment is core/app-driven |

Not user input (that is the `input` hook stage) and not settings. See
`P6-SETTINGS.md`.

## 6f. Status lines (P6-STATUS) **[design addendum]**

A **service over the status registry** (no new primitive): one contribution per
region + lifetime, multi-line.

| surface | item |
| --- | --- |
| types | `AgcExtStatusLine {region,lifetime,priority,style,render}`, `AgcExtStatusCtx {region,step,stop_reason,final,text}` |
| ABI | `add_status_line` (replaces `add_status`/provider/segment — breaking) |
| regions | `ABOVE_COMPOSER`, `BELOW_COMPOSER`, `STATUS_LEFT`, `STATUS_RIGHT`, `BELOW_STATUS`, `STEP_END`, `TURN_END` |
| lifetimes | `PERSISTENT` (chrome, polled), `PER_TURN`, `ONCE` (event, appended as a notice) |
| core | `agentc_status_snapshot(region,…)`, `agentc_status_emit(region,ctx)`, `agentc_status_remove/reset/version` |
| session | event lines persist as `custom` `notice` entries |

See `P6-STATUS.md`.

## 7. Tools (finalized: three op-based tools; registered by `builtin-tools`)

Per R-D26 agentc ships exactly **three** model-facing tools — one per actor kind
(`process`, `agent`) plus `queue`. `bash` is a legacy alias for
`process {op: spawn}` (kept for familiarity, not a separate tool). Canonical
schemas are in `P6-SIGNALS.md` §11.

| tool | op / args | primitives |
| --- | --- | --- |
| `process` | `{op: spawn\|write\|read\|resize\|kill\|freeze\|resume\|list\|attach\|detach, actor_id?, command?, shell?, tty?, background?, yield_ms?, cols?, rows?, …}` | `actor_spawn` (proc spec) + `message_send(INPUT/RESIZE/CANCEL/FREEZE/RESUME)` + `actor_read` + `actor_list` |
| `agent` | `{op: spawn\|wait\|send\|interrupt\|freeze\|resume\|result\|list, …}` | `actor_spawn` (agent spec) + `actor_await` + `queue_add` + `message_send(CANCEL/FREEZE/RESUME)` + `actor_read` + `actor_list` |
| `queue` | `{op: add\|list\|update\|delete\|reorder\|start\|pause\|resume\|clear, …}` | queue primitive |

- `process spawn` returns `{actor_id}` (background) or the turn result
  (foreground); `process read` drains it. `bash` is an alias for `process spawn`.
- No standalone resize/freeze/resume/output tools — they are `process` ops.
- `get_trust` is not a tool; the envelope is in the system prompt (declaration)
  and readable via `actor_query`.
- **Every tool runs through the `tool` stage** (`P6-HOOKS.md` §4): internal
  (C call), external (`rg`/`fd`/POSIX engines → process actor), and MCP (server
  process actor) alike, with the same envelope/approval path. Internal tools go
  through hooks by default and may opt out (`AGENTC_TOOL_NO_HOOKS`/
  `tools.hooks=off`). `agent`/`process`/`queue` ops are tool calls too, so the
  spawn/permission decisions are `tool`/`call` handlers.

## 8. RPC (`--mode rpc`, additive; finalized op-based)

Canonical: `actor_op {actor_id?, op, args}`, `actor_query {actor_id?, what}`,
`queue {op, …}`, `approve {id, decision, scope}`, `deny {id, reason}`,
`set_trust {actor_id, patch}`, `set_policy {id|file}`, plus an extended
`get_state`. These cover every earlier named command; no per-feature command
ships.

```text
actor_op    {actor_id?, op, args}
            op = spawn_process | spawn_agent | write | read | resize |
                 cancel | freeze | resume | attach | detach | enqueue |
                 label | checkpoint | release
actor_query {actor_id?, what}
            what = state | info | queue | trust | result | list | runtime |
                   pending_approvals
queue       {op, ...}
            op = add | list | update | delete | reorder | start | pause |
                 resume | clear
runtime     {op: tick | shutdown | freeze | resume}
approve     {id, decision, scope, args?}
deny        {id, reason}
set_trust   {actor_id, patch}
set_policy  {id|file}
get_state   {…turn_no, steerable, pending_approvals, background_actors}
```

Mapping of the earlier named commands: `actor_list/info/read/resize/input/
cancel/attach/detach/freeze/resume/checkpoint` → `actor_op`/`actor_query`;
`queue_*` → `queue`; `runtime_shutdown/freeze/resume` → `runtime {op}`;
`agent_list/result` → `actor_query`;
`get_trust` → `actor_query what:trust`; `steer`/`interrupt` → `queue`/
`actor_op enqueue|cancel`.

## 9. CLI flags

Declared: `--access`, `--sandbox`, `--approval`, `--policy`, `--network`
(plus existing `--approve`/`--no-approve`).

**[NEW] audit additions:**

| flag | purpose |
| --- | --- |
| `--max-depth N` | sub-agent depth cap |
| `--no-background` | refuse background process actors (hermetic runs) |
| `--dump-actors` | print the actor tree at exit (debug) |
| `--shutdown-grace MS` | cancel grace override |

## 10. Config keys (append; unknown keys preserved)

Declared: `trust.{access,sandbox,approval,network,widen,roots,
approval_timeout_ms,queue.auto_start,queue.submit,queue.urgent,
queue.drop_partial,subagent.inherit,subagent.max_depth}`, `policies.<id>`.

**[NEW] audit additions:**

| key | default | purpose |
| --- | --- | --- |
| `trust.cancel.grace_ms` | 3000 | graceful→force |
| `trust.term.idle_ms` | 30000 | process stall |
| `trust.agent.idle_ms` | 120000 | agent stall |
| `trust.term.deadline_ms` | 1800000 | background hard cap |
| `trust.actor.max` | 64 | actor table |
| `trust.queue.paused` | false | start paused |
| `trust.queue.max` | 32 | queue bound |
| `trust.sandbox.backend` | auto | `landlock\|bwrap\|seatbelt\|token` |
| `trust.freeze.backend` | live | `live\|checkpoint` (CRIU opt-in) |
| `trust.freeze.agent_mode` | drain | `drain\|abort` for an in-flight request |
| `trust.freeze.max_ms` | 0 | auto-resume after a frozen interval (0 = unbounded) |
| `trust.freeze.stdin_max` | 65536 | buffer for stdin written to a frozen process |
| `trust.approval.persist` | false | allow `project`-scope rules to be saved |

## 11. Session record types (`custom`, v1 preserved)

Declared: `agent`, `queue`, `approval`, `actors`. **[NEW]:** `trust`
(root/effective envelope snapshot at session start and on every `trust_set`),
`actor` (one spawn/exit record; `actors` may be the plural snapshot — pick one
and document).

## 12. Status segments

Declared: `actors` (background count, stalled count). **[NEW]:** include
`queue` depth and current `turn_no` in the same segment (avoid a second status
path). Extensions register through `agentc_status_register` only.

## 13. Limits (`src/base/limits.h`)

Consolidated (existing + declared + **[NEW]**):

```text
AGENTC_ACTOR_MAX                64    [declared]
AGENTC_PROC_TAIL_BYTES          65536 [declared]
AGENTC_PROC_WRITE_MAX           65536 [declared]
AGENTC_PROC_READ_MAX            262144[declared]
AGENTC_PROC_IDLE_DEF_MS         30000 [declared]
AGENTC_PROC_DEADLINE_DEF_MS     1800000 [declared]
AGENTC_AGENT_IDLE_DEF_MS        120000 [declared]
AGENTC_CANCEL_GRACE_DEF_MS      3000  [declared]
AGENTC_MSG_MAILBOX_MAX          64    [declared]
AGENTC_MSG_DRAIN_MAX            64    [declared]
AGENTC_QUEUE_MAX                32    [declared]
AGENTC_TRUST_MAX_ROOTS          16    [declared]
AGENTC_TRUST_MAX_HOSTS          32    [declared]
AGENTC_POLICY_MAX_RULES         256   [declared]
AGENTC_APPROVAL_MAX_BYTES       65536 [declared]
AGENTC_AGENT_CTX_KEEP           8     [declared]
AGENTC_AGENT_MAX_DEPTH          3     [NEW] default max_depth
AGENTC_ACTOR_LABEL_MAX          128   [NEW]
AGENTC_MSG_MAX_TEXT          65536 [NEW] one enqueue/stdin payload
AGENTC_TRUST_MAX_POLICY_BYTES   262144[NEW] policy file cap
AGENTC_PROC_FREEZE_IN_MAX       65536 [NEW] stdin buffer while frozen
AGENTC_FREEZE_MAX_MS            0     [NEW] 0 = unbounded auto-resume
```

`AGENTC_LIMIT_TOOL_BYTES`/`_LINES` remain the display caps.

## 14. Error codes (RPC `code` / typed C errors) **[NEW, consolidated]**

| code | origin |
| --- | --- |
| `actor_not_found` | signal/actor ops on a released id |
| `actor_limit` | `AGENTC_ACTOR_MAX` reached |
| `queue_full` | `AGENTC_QUEUE_MAX` reached |
| `queue_item_not_found` | update/delete/reorder/get |
| `active_turn_not_steerable` | enqueue into a compaction/review step |
| `turn_settled` | target run already finished with no remaining safe point |
| `authority_denied` | child enqueues into parent |
| `widen_denied` | envelope widening without approval |
| `trust_denied` | static policy deny |
| `approval_timeout` | approver did not answer |
| `approval_malformed` | bad approver response (deny) |
| `sandbox_unavailable` | no backend for the requested boundary |
| `sandbox_denied` | kernel denial at runtime |
| `pty_unavailable` | `tty:true` on a platform/backend without PTY |
| `stdin_closed` | write to a closed pipe process |
| `depth_limit` | `agent op:spawn` at `max_depth` |
| `busy` | reserved: last-resort backpressure |

These populate the P5-AXES A-D4 `code` field and the tool-error text (failures
are tool results, never provider errors).

## 15. TUI surfaces

Declared: queue pane (step/priority, editable/reorderable), submit/urgent
keybindings, Ctrl-C turn cancel, `/ps` actor list with attach/detach/kill,
approval dialog, `actors` status segment.

**[NEW]:** `/queue` command (list/pause/clear), `/agents` command (tree view,
read-only history), `/trust` command (show effective envelope), resize
handling for an attached process.

## 16. Files

New: `include/{actor,signal,trust}.h`; `src/core/{actor,process,signal,queue,
trust,approval,agent_spawn,runtime}.c`; tests (below).

Modified (append/additive): `include/plat.h`, `include/agent.h`,
`include/agentc_ext.h`, `include/config.h`, `src/plat/{linux,mac,win,headless}`,
`src/core/{agent,events,session,config}.c`, `src/core/tools/{bash,jobs,
registry}.c`, `src/app/{main,mode,mode_rpc,policy}.c`, `src/tui/*`,
`src/base/limits.h`.

## 17. Tests

Declared: `tests/actors_test.c`, `tests/signals_test.c`, `tests/trust_test.c`,
`tests/subagents_test.c` + `.expected`; e2e/ext shell extensions.

**[NEW]:** `tests/queue_test.c` (step/priority matrix in isolation) OR fold into
`signals_test.c` (preferred — keep one suite); `tests/runtime_test.c` for
init/shutdown/atexit reap and the actor snapshot. All wired into `make check`.

## 18. Gap audit — what was missing and the resolution

This is the point of the audit. Each row: a scenario we might need → the
surface that covers it, or the fix applied.

| # | Scenario / need | Covered by | Resolution |
| --- | --- | --- | --- |
| 1 | Read a background process's stdout | `agentc_process_read/tail` | **fix:** `process op:read` tool + `actor_read`/`actor_query` RPC + host `actor_read` |
| 2 | Resize a PTY (UI attach / spawn) | no surface before | **fix:** `agentc_process_resize` + RPC `actor_op resize` + attach behavior + spawn `cols/rows` + event `actor_resized`; tool via `process op:resize` |
| 3 | Query an actor/agent without a pointer | only `list` before | **fix:** `agentc_actor_info/exists`, `agentc_agent_info/find/parent` |
| 4 | Parent waits on a child while the child needs approval → deadlock | not addressed | **fix (critical):** `agentc_agent_wait` and interactive approval waits MUST run `agentc_runtime_tick` (actor tick + signal drain) in their pump loop; state it in `P6-SIGNALS`/`P6-TRUST`/`P6-SUBAGENTS`. No blocking wait may skip the bus. |
| 5 | Step identity for `expectedTurnId`-style preconditions | `turn_seq` only | **fix:** `agentc_agent_turn_no`, `get_state.turn_no`; enqueue already carries `step` |
| 6 | Queue pause/resume (Codex has it) | none | **fix:** `agentc_queue_pause/is_paused`, `queue_pause/resume` RPC, config `trust.queue.paused`, event `queue_paused` |
| 7 | Queue clear / count / fetch-one | partial | **fix:** `agentc_queue_clear/count/get` |
| 8 | Approval typed API + persistence of a rule | dispatch described, no names | **fix:** §3.7 `agentc_approval_*`; `add_rule` field in the response; `agc_approval_scope_remember`; config `trust.approval.persist` |
| 9 | Policy load/eval API | described in prose | **fix:** §3.6 `agentc_policy_*` |
| 10 | Envelope clone/narrow/describe/parse | only decide/resolve | **fix:** §3.5 helpers |
| 11 | Sandbox spec builder | mentioned | **fix:** `agentc_trust_sandbox_spec` |
| 12 | Signal JSON codec for RPC/hooks/audit | implicit | **fix:** `agentc_message_to_json/from_json` |
| 13 | Extensions that own processes/queues/agents | only a few host funcs | **fix:** expand host ABI (§6) |
| 14 | Runtime init/shutdown + graceful reap entry point | spread across actor/user | **fix:** `agentc_runtime_init/tick/shutdown`; add `runtime_shutdown` RPC and cleanup ordering |
| 15 | PTY resize/finish observability | none | **fix:** `actor_resized` event/hook |
| 16 | Parent-side result event | only the tool result | **fix:** `agent_result` event/hook, `agentc_agent_result` |
| 17 | Typed error codes for programmatic consumers | prose only | **fix:** §14 enumerated |
| 18 | Per-actor env/cwd for spawned processes | `AgcProcSpawn.cwd`, envp implicit | **fix:** document `cwd` (declared) + **[NEW]** `env` (small explicit map) on `AgcProcSpawn`/`AgcAgentSpec` |
| 19 | Observability/debug of live actors | `/ps` only | **fix:** `--dump-actors`, `agentc_runtime_actor_snapshot` |
| 20 | Hermetic runs must refuse background/PTY | flag not present | **fix:** `--no-background`, `AGENTC_TRUST_ALLOW_BACKGROUND/PTY` already gate it |
| 21 | Idempotency/correlation for enqueues | `client_id` declared | **done:** `AGENTC_Q_F_DEDUPE` drops a repeated `client_id` before the cap check (first-write-wins, returns the existing id); `agentc_queue_deduped()` counts drops; the `queue`/`agent` tools take `client_id` + `dedupe`; RPC forwards verbatim; no input event/steer on a drop |
| 22 | Steerability query for callers | event only | **fix:** `agentc_agent_steerable`, `get_state.steerable` |
| 23 | Token/runtime budget per actor | `deadline_ms` only | **done:** `deadline_ms` (declared) enforced; **[NEW]** `AgcAgentSpec.token_budget`/`AgcAgentInfo.token_budget`/`tokens_spent` enforced as a soft cap that emits `actor_notify` and settles between steps (`agentc_agent_set_token_budget`/`agentc_agent_tokens_spent`; `tests/budget_test.c`) |
| 24 | Session read-back of actors/queue/trust on resume | records declared | covered; add `agentc_session_load_actors/queue/trust` readers (append-only) |
| 25 | Multiple UI attaches to one actor | bounded mailbox only | covered by `ATTACH`/`DETACH` + `AGENTC_MSG_MAILBOX_MAX`; document a max-attach (`AGENTC_ACTOR_ATTACH_MAX` **[NEW]**) |
| 26 | Attached process resize reflow | none | covered by fix #2 + `actor_resized` |
| 27 | MCP/dynamic tool approvals are uniform | generic `tool` field | covered; document that MCP tool names (`mcp__…`) are matched by the same rules |
| 28 | Cancel resolves the queue | not stated | **fix:** state that `cancel{scope:actor}` leaves the queue until release; `release` clears it; `cancel{scope:turn}` keeps it |
| 29 | Graceful shutdown of a running turn (Codex `suspend_turn_and_shutdown`) | not present | **fix:** `agentc_runtime_shutdown` does graceful turn cancel → reap; expose `runtime_shutdown{grace_ms}` |
| 30 | Recover a step across restart | none | **Out of scope** (no daemon); document. |
| 31 | Freeze an in-flight process and resume later | none | **fix:** `FREEZE`/`RESUME` signals, `agentc_actor_freeze/resume/frozen`, `agentc_runtime_freeze/resume`, events `ACTOR_FROZEN/RESUMED/CHECKPOINTED`, hooks, tools `process`/`agent` `op:freeze\|resume`, RPC`actor_op {op: freeze\|resume\|checkpoint}`; timers suspended while frozen; optional CRIU checkpoint across restart (R-D18) |

## 19. Deliberately out of scope (so an implementer does not add them)

> Superseded in part by P7: server-side `client_id` dedupe was moved into scope
> as `AGENTC_Q_F_DEDUPE` (§21 #21); the rest stands. The original bullet read
> "Server-side dedupe of `client_id` (caller concern)".

- App-server/daemon/remote protocol; `command/*` vs `process/*` split.
- Code-mode/JS tool (`exec` with a JS namespace); sandboxed code execution.
- Threads/multi-process scheduling; all actors are single-threaded and pumped.
- Session header version bump; new state stays in `custom` records.
- Server-side dedupe of `client_id` (caller concern).
- Cross-session step recovery; network-fetched tools; a wide provider matrix.

## 20. Coverage checklist (scenario → surfaces)

| capability | surfaces |
| --- | --- |
| start/poll/feed/kill a shell | `bash{background,yield}`, `process{op: read\|write\|resize\|kill\|list}`; actor/process C+RPC+host APIs |
| run a sub-agent | `agent op:spawn`, `actor_spawn`, tree APIs |
| wait for / talk to a sub-agent | `agent{op: wait\|send\|interrupt\|result\|list}`,`agentc_agent_wait/send/cancel/result`,`agent_result` event |
| steer/queue/interrupt a step | `queue{op: add}`, `agent op:send`, `agent op:interrupt`, `step`/`priority` |
| declare/see a trust boundary | `AgcTrustEnv`, `get_trust`/`set_trust`/`trust.desc`, `trust_select`, `trust` session record |
| approve a call (interactive/program/model) | `tool_approval`, `approve`/`deny`, `agentc_approval_*`, `policies.<id>` |
| enforce a sandbox | `os_sandbox_apply`, `agentc_trust_sandbox_spec`, `sandbox_denied` |
| observe everything live | events `AGENTC_EV_ACTOR_*`/trust/approval + hooks + `/ps`/`/agents` + status |
| replay/audit everything | session `agent`/`queue`/`approval`/`actors`/`trust` records + `--dump-actors` |
| deterministic tests | `os_test_clock_set`, `os_test_syscall_log`, golden suites |
| graceful stop | `CANCEL` graceful→force, `agentc_runtime_shutdown`, `runtime_shutdown` RPC, atexit reap |
| freeze/resume an in-flight unit | freeze/resume signals, `agentc_actor_freeze/resume`, `agentc_runtime_freeze/resume`, `process`/`agent` `op:freeze\|resume`,`actor_op {op: freeze\|resume\|checkpoint}` RPC; optional checkpoint |

---

## 21. Follow-ups to fold into the area specs (when each wave lands)

> The tool/RPC vocabulary is **finalized op-based** (R-D19; `P6-PRIMITIVES.md`
> §6): three tools (`process`/`agent`/`queue`) and the
> `actor_op`/`actor_query`/`queue`/`approve`/`deny`/`set_*` RPC. Every area spec
> and this registry already use that vocabulary; do not reintroduce per-feature
> names as first-class.

1. `P6-ACTORS.md`: add §3 `agentc_process_resize/kill/spill_path/set_stdin`,
   `AgcProcSpawn.env`, `actor_resized` event/hook, `AGENTC_ACTOR_ATTACH_MAX`.
2. `P6-SIGNALS.md`: add §3 `agentc_message_to_json/from_json/cancel_subtree`;
   §5 `agentc_queue_get/count/clear/pause/is_paused/run_of`, event
   `queue_paused`, `AGENTC_MSG_MAX_TEXT`; §9 error codes. (Queue
   pause/resume/clear are `queue {op}` in the finalized RPC.)
3. `P6-TRUST.md`: add §3.5–3.7 C APIs, `token_budget` note (landed: §18 #23),
   `trust.approval.
   persist`, `trust.sandbox.backend`, `AGENTC_TRUST_MAX_POLICY_BYTES`.
4. `P6-SUBAGENTS.md`: add §3.8 tree APIs + `agent_result` event/hook;
   `agentc_agent_wait` MUST service the bus; `AgcAgentSpec.env`.
5. `P6-INTEGRATION.md`: fold §14 error codes, §18 #4 (wait services signals),
   and the expanded host ABI into its inventory; keep this file as the index.
6. `P6-RUNTIME.md`: add R-D17 (runtime init/shutdown + wait-servicing) and
   reference this file.
