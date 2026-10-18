# P6-INTEGRATION — Cross-cutting wiring, file map, test matrix, handoff

Status: **as-built (P6-A..E + completion passes 2a-2g).** The cross-cutting wiring, file map and test matrix.
As-built note: the outcome is folded into `.agents/design/50-build-and-test.md` and `.agents/design/30-extensibility.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

Purpose: give an external LLM/agent a single checklist that, if followed, lands
P6 without inventing surface or breaking the append-only rule.

> **`P6-SURFACE.md` is the authoritative, complete surface registry and coverage
audit** (every API/event/hook/tool/RPC/flag/key/limit/error code, plus gaps).
This file gives the wiring and file ownership; when the two list the same
surface, they must agree. Fold `P6-SURFACE.md` §21 into the area specs as each
wave lands.

---

## 1. Append-only interface inventory (exact)

Everything below is **appended**; no existing field or signature changes.

### 1.1 `include/plat.h` (Layer 0)

Append after the existing process block:

```c
/* PTY (P6-ACTORS §9) */
int os_pty_open(int *master, int *slave, int cols, int rows);
int os_pty_resize(int fd, int cols, int rows);
int os_pty_size(int fd, int *cols, int *rows);
/* Sandbox (P6-TRUST §8) */
typedef struct AgcSandboxSpec { /* see P6-TRUST §8 */ } AgcSandboxSpec;
int os_sandbox_apply(const AgcSandboxSpec *spec, char *err, size_t errcap);
/* Freeze/resume (P6-ACTORS §6b, R-D18). POSIX: SIGSTOP/SIGCONT on the group;
 * checkpoint is the optional CRIU backend (-ENOSYS elsewhere). */
#define OS_SIGSTOP 19
#define OS_SIGCONT 18
int os_process_freeze(int pid, bool group);
int os_process_resume(int pid, bool group);
int os_process_checkpoint(int pid, const char *dir, char *err, size_t errcap);
```

Implementations: `src/plat/linux/sys.c`, `src/plat/mac/*`, `src/plat/win/*`
(`os_pty_*` may return `-ENOSYS` on Windows for P6-A),
`src/plat/headless.c` (deterministic stubs + a recorded syscall log).

### 1.2 `include/agent.h` (core public)

Append, in this order, after the existing event enum / near the tool block:

1. `AgcAccess`, `AgcBoundary`, `AgcApproval`, `AgcNetwork`, `AgcTrustEnv`,
   `AGENTC_TRUST_*` flags, `AgcTrustDecision`, `AgcSandboxSpec` (or include a new
   `include/trust.h` from `agent.h`; either is acceptable if append-only).
2. `AgcActorId`, `AgcActorKind`, `AgcActorState`, `AgcActor`, `AgcProcess`,
   `AgcProcSpawn`, `AGENTC_PROC_*`, `AgcProcInfo` (or a new `include/actor.h`).
3. `AgcMessage` + signal enums (or a new `include/message.h`).
4. `AgcAgentSpec`, `AGENTC_AGENT_F_*`, `AgcAgentInfo`.
5. Event constants appended after `AGENTC_EV_MSG_RESET`:
   `AGENTC_EV_TRUST_SET`, `AGENTC_EV_APPROVAL_REQUEST`,
   `AGENTC_EV_APPROVAL_RESULT`, `AGENTC_EV_SANDBOX_DENIED`, and the
   `AGENTC_EV_ACTOR_*` set (see §2).
6. `AgcActorEvent`, `AgcApprovalEvent` payload structs.
7. Function declarations: `agentc_actor_*`, `agentc_process_*`,
   `agentc_message_*`, `agentc_queue_*`, `agentc_trust_*`, `agentc_approval_*`,
   `agentc_agent_create`, `agentc_agent_list`.

A new header is preferred for cohesion (`include/actor.h`, `include/message.h`,
`include/trust.h`), all included by `agent.h` at the end so existing includers
keep working.

### 1.3 `include/agentc_ext.h` (extension ABI)

- Append hook-name string constants after `AGENTC_HEV_THINKING_SELECT`:
  the `AGENTC_HEV_ACTOR_*` set (§3), `AGENTC_HEV_TRUST_SELECT`,
  `AGENTC_HEV_TOOL_APPROVAL`, `AGENTC_HEV_SANDBOX_DENIED`,
  `AGENTC_HEV_POLICY_RULES`.
- Append host functions to `struct AgcExtHost` **at the end**, each gated by
  `AGENTC_EXT_HOST_HAS(h, field)`:

```c
/* P6 host additions (appended; each guarded by AGENTC_EXT_HOST_HAS). The host
 * surface mirrors the primitives: spawn, one op, one query, read, await, the
 * signal/queue codecs, and trust/approval/policy. No per-feature host methods.
 * Owned strings (out_json, returned JSON) come from host->alloc and are freed
 * with host->free. */
int    (*actor_spawn)(const AgcExtHost *, const char *spec_json, uint32_t *out_id);
int    (*actor_op)(const AgcExtHost *, uint32_t actor_id, const char *op,
                   const char *args_json, char **out_json);
int    (*actor_query)(const AgcExtHost *, uint32_t actor_id, const char *what, char **out_json);
size_t (*actor_read)(const AgcExtHost *, uint32_t actor_id, char *out, size_t cap);
int    (*actor_await)(const AgcExtHost *, uint32_t actor_id, uint32_t kind_mask,
                      void (*cb)(void *, uint32_t, int, const char *), void *ud);
int    (*message_send)(const AgcExtHost *, const char *signal_json);
int    (*message_from_json)(const AgcExtHost *, const char *json, void *out_signal);
char  *(*message_to_json)(const AgcExtHost *, const void *signal);
int    (*queue_op)(const AgcExtHost *, uint32_t actor_id, const char *op,
                   const char *args_json, char **out_json);
char  *(*get_trust)(const AgcExtHost *, uint32_t actor_id);
int    (*set_trust)(const AgcExtHost *, uint32_t actor_id, const char *patch_json);
int    (*submit_approval)(const AgcExtHost *, const char *response_json);
int    (*policy_eval)(const AgcExtHost *, const char *tool, const char *args_json, char **out_json);
uint64_t (*hook)(const AgcExtHost *, const char *stage, int order,
                 int (*fn)(void *, void *, void **, uint64_t *), void *ud);
void  (*hook_off)(const AgcExtHost *, uint64_t handle);
int   (*decider_register)(const AgcExtHost *, const char *spec_json);
```

- Bump nothing: `abi_version` stays; appended fields + `struct_size` checks carry
  compatibility (`AGENTC_EXT_FIELD_OK`).
- Add `policyPaths` (and optionally `actorHooks`) to `AgcExtResult` at the end,
  gated by the same `struct_size` discipline.

### 1.4 `include/config.h` and `include/session.h`

- `config.h`: append `trust` accessors (`agentc_config_trust_access`,
  `_boundary`, `_approval`, `_network`, `_roots`, `_policy_id`, `_widen`,
  `_approval_timeout_ms`, `_subagent_inherit`, `_subagent_max_depth`) and a
  `policies` lookup. Do not change existing signatures.
- `session.h`: no signature changes. New records go through
  `agentc_session_append_raw` (v1 preserved). Add optional read helpers
  (`agentc_session_load_agents`) if resume needs to reconstruct the tree; append
  only.

## 2. Event additions (final list) and hub wiring

Append after `AGENTC_EV_MSG_RESET` in `include/agent.h`:

```text
AGENTC_EV_TRUST_SET
AGENTC_EV_APPROVAL_REQUEST
AGENTC_EV_APPROVAL_RESULT
AGENTC_EV_SANDBOX_DENIED
AGENTC_EV_ACTOR_STARTED
AGENTC_EV_ACTOR_PROGRESS
AGENTC_EV_ACTOR_STALLED
AGENTC_EV_ACTOR_INPUT
AGENTC_EV_ACTOR_STEERED
AGENTC_EV_ACTOR_QUEUED
AGENTC_EV_ACTOR_NOTIFY
AGENTC_EV_ACTOR_FINISH
AGENTC_EV_ACTOR_FAIL
AGENTC_EV_ACTOR_CANCEL_REQUESTED
AGENTC_EV_ACTOR_CANCELLED
AGENTC_EV_ACTOR_FROZEN
AGENTC_EV_ACTOR_RESUMED
AGENTC_EV_ACTOR_CHECKPOINTED
AGENTC_EV_ACTOR_ATTACHED
AGENTC_EV_ACTOR_DETACHED
AGENTC_EV_HOOK_START
AGENTC_EV_HOOK_END
```

Wiring (`src/core/events.c` / `src/ext/registry.c`):

- All new events dispatch through the existing hub: per-agent callback first,
  then slots, honoring the quiet gate.
- The extension bridge (`agentc_ext_emit_agent_event`) maps each new event to its
  `AGENTC_HEV_ACTOR_*`/`trust_*`/`approval_*` point and JSON payload. Add cases
  in the switch; do not disturb existing cases.
- Content events keep their current names and payloads (R-D16). New fields
  (`agent_id`) on existing payloads are appended, so old consumers ignore them.

## 3. Hook point additions

| constant | value | kind | payload |
| --- | --- | --- | --- |
| `AGENTC_HEV_ACTOR_STARTED` | `actor_started` | observe | `{actor_id,parent_id,depth,kind,label,provider?,model?}` |
| `AGENTC_HEV_ACTOR_PROGRESS` | `actor_progress` | observe | `{actor_id,total_bytes,lines}` |
| `AGENTC_HEV_ACTOR_STALLED` | `actor_stalled` | observe | `{actor_id,idle_ms}` |
| `AGENTC_HEV_ACTOR_INPUT` | `actor_input` | override (transform/block) | `{actor_id,turn,priority,text}` |
| `AGENTC_HEV_ACTOR_STEERED` | `actor_steered` | observe | `{actor_id,at,turn}` (`at` = `preempt`\|`boundary`) |
| `AGENTC_HEV_ACTOR_NOTIFY` | `actor_notify` | observe | `{actor_id,kind,json}` |
| `AGENTC_HEV_ACTOR_FINISH` | `actor_finish` | observe | `{actor_id,exit_code,duration_ms}` |
| `AGENTC_HEV_ACTOR_FAIL` | `actor_fail` | observe | `{actor_id,error}` |
| `AGENTC_HEV_ACTOR_CANCEL_REQUESTED` | `actor_cancel_requested` | observe | `{actor_id,scope,stage}` |
| `AGENTC_HEV_ACTOR_CANCELLED` | `actor_cancelled` | observe | `{actor_id,scope,stage,reason}` |
| `AGENTC_HEV_ACTOR_FROZEN` | `actor_frozen` | observe | `{actor_id,scope}` |
| `AGENTC_HEV_ACTOR_RESUMED` | `actor_resumed` | observe | `{actor_id,scope,frozen_ms}` |
| `AGENTC_HEV_ACTOR_CHECKPOINTED` | `actor_checkpointed` | observe | `{actor_id,path}` |
| `AGENTC_HEV_TRUST_SELECT` | `trust_select` | override (narrow) | `{agent_id,parent_id,requested,effective}` |
| `AGENTC_HEV_TOOL_APPROVAL` | `tool_approval` | chain-veto/async | the §P6-TRUST §7.1 contract |
| `AGENTC_HEV_SANDBOX_DENIED` | `sandbox_denied` | observe | `{agent_id,tool,reason}` |
| `AGENTC_HEV_POLICY_RULES` | `policy_rules` | contribute | `{paths:[…]}` |

The existing `input` and `tool_call` semantics are unchanged; `actor_input` is
additive and covers all deliveries (including process stdin), while `input`
remains the top-level text override.

## 4. Pump / tick / drain integration points

`agentc_actor_tick` + `agentc_message_drain` must run from exactly these places
so there are no missed signals and no reentrancy:

1. `agentc_pump(timeout_ms)` — the single cooperative pump. Add the tick+drain
   at its top. This covers TUI repaint, foreground tool loops
   (`bash_step_once` already calls `agentc_pump`), and `agent op:wait`.
2. The agent loop's three safe points in `src/core/agent.c`:
   - before request build (boundary flush lands here),
   - after `message_end` (queue scheduling),
   - after each tool batch (preempt flush, then boundary flush; queued start).
3. `agentc_actor_release` — drain-and-discard the released actor's mailbox.
4. **Every blocking wait** (`agentc_agent_wait`, interactive approval,
   `agent op:wait`, the TUI idle loop) — its pump loop must call `runtime_tick`
   (actor tick + signal drain), never a bare `os_poll`/sleep. This is R-D17 and
   the deadlock-avoidance rule (`P6-SURFACE.md` §18 #4): a parent waiting on a
   child must still service that child's `notify`/approval.

Rules: the drain is bounded per call (P6-SIGNALS §8); handlers run only inside
the drain; a handler may enqueue but not recurse; no wait may skip the drain.

### 4.1 Typed error codes

Programmatic failures use the codes enumerated in `P6-SURFACE.md` §14
(`active_turn_not_steerable`, `queue_full`, `actor_not_found`, `widen_denied`,
`trust_denied`, `approval_timeout`, `sandbox_unavailable`, `pty_unavailable`,
`depth_limit`, …) in the RPC `code` field (P5-AXES A-D4) and in the tool-error
text. Failures are tool results, never provider errors.

## 5. Session, config, CLI, RPC, TUI, status

### 5.1 Session (v1 preserved, R-D11)

New `custom` record types (all via `agentc_session_append_raw`):

| type | data |
| --- | --- |
| `agent` | `{op:spawn\|finish\|fail\|cancel, agent_id, parent_id, depth, provider, model, task_hash, result_hash}` |
| `queue` | `{items:[{id,text,turn,priority,enqueued_ms}]}` (snapshot; add/update/delete/reorder) |
| `approval` | `{agent_id, tool, input_hash, decision, reason, scope, policy_id}` |
| `actors` | optional spawn/exit audit for processes: `{op, actor_id, parent_id, cmd_hash, exit_code}` |

Unknown-to-old-readers behavior is already tolerated (malformed lines skipped
with one aggregate warning); verify `message` entries can carry optional
`agent_id`/`parent_id`, otherwise use an `agent_message` custom wrapper.

### 5.2 Config keys (append to the schema; unknown keys preserved)

```text
trust.access            read-only|read-write        (default read-write)
trust.sandbox           none|workspace|strict      (default workspace)
trust.approval          auto|ask|policy            (default auto)
trust.network           deny|allow|allowlist       (default deny)
trust.widen             deny|clamp                 (default deny)
trust.roots             [paths]                    (default ["."])
trust.approval_timeout_ms                          (default 120000)
trust.subagent.inherit  bool                       (default true)
trust.subagent.max_depth int                       (default 3)
trust.queue.auto_start  bool                       (default true)
trust.queue.submit      priority -20..19           (default 0; submit keybinding)
trust.queue.urgent      priority -20..-1           (default -1; urgent keybinding)
trust.queue.drop_partial bool                      (default true)
trust.freeze.backend    live|checkpoint            (default live)
trust.freeze.agent_mode drain|abort                (default drain)
trust.freeze.max_ms     int                        (default 0 = unbounded)
trust.freeze.stdin_max  int                        (default 65536)
policies.<id>           {kind:model|program, …}
```

### 5.3 CLI

```text
--access read-only|rw
--sandbox none|workspace|strict
--approval auto|ask|policy
--policy <id|file>
--network deny|allow
```

Plus the existing `--approve`/`--no-approve` (project trust, now also seeds the
project default envelope).

### 5.4 RPC (additive to `src/app/mode_rpc.c`; op-based, R-D19)

```text
actor_op    {actor_id?, op, args}
runtime     {op: tick|shutdown|freeze|resume}
actor_query {actor_id?, what}
queue       {op, ...}
approve     {id, decision, scope, args?}
deny        {id, reason}
set_trust   {actor_id, patch}
set_policy  {id|file}
get_state   {…turn_no, steerable, pending_approvals, background_actors}
```

No named per-feature commands ship; hosts call the op form. Plus the new
`AGENTC_EV_*` events on the existing stream. Response/echo shapes follow
P5-AXES A-D1/A-D4 (dual discriminator, `id`, `code`).

### 5.5 TUI

- Queue pane (items show `run`/`priority`, editable/reorderable); submit
  enqueues `{turn:0, priority:trust.queue.submit}`, urgent enqueues
  `trust.queue.urgent` (default `-1`, preempt); Ctrl-C = turn cancel.
- `/ps` actor list with attach/detach/kill.
- Approval dialog (interactive approver).
- Status segment: `N bg` (background actors) via the existing status registry
  (`include/status.h`, `src/core/status_builtin.c`); never a private path into
  `comp_footer()`.

### 5.6 Status

Register one built-in status segment `actors` (background process count and, if
any, stalled count). Extensions may add their own.

## 6. File map and ownership

New files:

| file | owns |
| --- | --- |
| `include/actor.h`, `src/core/actor.c` | actor registry, tick, release, atexit reap |
| `src/core/process.c` | process spawn/read/write/tail, cancel escalation, stall |
| `include/message.h`, `src/core/signal.c` | signal type, bus, mailboxes, drain |
| `src/core/queue.c` | message queue ops + persistence |
| `include/trust.h` (or `agent.h` block), `src/core/trust.c` | envelope, decide, rules |
| `src/core/approval.c` | approval dispatch (interactive/program/model), scopes; generalized to `src/core/decide.c` (decision points, `P6-DECIDERS.md`) |
| `src/core/agent_spawn.c` (or `agent.c`) | `agentc_agent_create`, tree |
| `include/hook.h`, `src/core/hook.c` | the universal interception pipeline (stages, two-verb contract, defer, branch) `P6-HOOKS.md` |
| `src/core/tool_runner.c` | one runner API for internal/process/mcp tools; `jobs.c` schedules over it `P6-HOOKS.md` §4 |

Modified files (append-only or additive):

| file | change |
| --- | --- |
| `include/plat.h` | `os_pty_*`, `os_sandbox_apply` |
| `src/plat/linux/sys.c`, `src/plat/mac/*`, `src/plat/win/*`, `src/plat/headless.c` | implement the above |
| `include/agent.h` | event constants, payload structs, function decls |
| `include/agentc_ext.h` | hook names, host funcs, `AgcExtResult` fields |
| `src/core/events.c` | dispatch new events |
| `src/ext/registry.c` | bridge new events to hooks; host func impls |
| `src/core/agent.c` | constructor call, drain points, agent actor registration |
| `src/core/tools/bash.c` | use process actors; background/yield_ms; keep output byte-exact |
| `src/core/tools/jobs.c` | `job->actor`, `job->owns_actor`; skip release for background |
| `src/core/tools/registry.c` | register `process`/`agent`/`queue` (three op-based tools; R-D26) |
| `src/app/mode.c`, `src/app/mode_rpc.c`, `src/app/main.c`, `src/core/policy.c` | constructor call, RPC commands, CLI flags, tool selection |
| `src/core/session.c` | `custom` record helpers/readback |
| `src/core/config.c` | `trust`/`policies` parsing + accessors |
| `src/tui/*` | queue pane, `/ps`, approval dialog, status |
| `src/base/limits.h` | new bounds |
| `tests/*` | new golden suites + mock extensions |

## 7. Test matrix

New golden suites (each `.c` + `data/*.expected`, wired into `make check` /
`tests/run.sh`):

| suite | covers |
| --- | --- |
| `tests/actors_test.c` | actor state machine, spawn/read/write/cancel/stall, bash parity |
| `tests/signals_test.c` | queue (turn/priority), preempt/boundary flush, cancel scope, notify, mailbox |
| `tests/freeze_test.c` | freeze/resume an actor/subtree/runtime; timers suspended (no stall/timeout across a freeze); stdin buffering; cancel-resumes-before-term; atexit reap of frozen children |
| `tests/trust_test.c` | decide truth table, monotonicity, rules, approval contract, sandbox |
| `tests/subagents_test.c` | unified constructor, tree, direct model, spawn tools, persistence |

Shell suites: extend `tests/e2e.sh` (pty: background process round-trip,
steer/interrupt, approval dialog) and `tests/ext.sh` (an extension using the new
host/actor/approval APIs).

Determinism requirements: fake clock, scripted process mock in
`src/plat/headless.c` with a recorded kill/syscall log, mock transport. Every
new parser/formatter/state machine has a byte-exact golden (house rule).

Regression: existing `tools_test` bash goldens must pass **without edits**;
existing `--mode rpc`/`--mode json`/session/`ext`/`mcp` suites green;
`wine-check` 30/30 and `dyn-check` green.

## 8. Documentation fold-in (same change as the behavior)

| doc | update |
| --- | --- |
| `design/00-architecture.md` §4 component map, §6 event loop | add actor/signal/trust modules and the pump tick |
| `design/20-core-agent.md` §2 loop, §4 tools, §6 sessions | unified constructor, actor drain points, process actors, queue/steer, trust |
| `design/30-extensibility.md` §2.6 hooks, §2.5 ABI | new hook points + host functions |
| `design/40-tui.md` | queue pane, `/ps`, approval dialog, status segment |
| `design/50-build-and-test.md` | new test suites |
| `.agents/README.md` | index P6 files |
| `plans/P6-*.md` | mark as-built, fold outcomes into `design/` |

## 9. Full implementation handoff

### 9.1 Order

```text
P6-A actors/processes  →  P6-B signals  →  P6-C trust/approvals  →  P6-D subagents  →  P6-E integration
```

P6-E is not a single PR: each wave folds its own integration and docs. P6-E's
remaining work (cross-surfaces, status, goldens consolidation) lands after D.

### 9.2 Preconditions (before P6-A)

- Tree is clean; no unrelated WIP. If present, stop and ask the chair to
  freeze/commit/stash.
- `make check`, `./tests/ext.sh`, `./tests/mcp.sh`, `./tests/e2e.sh`,
  `make windows`, `wine-check`, `dyn-check` baseline green.

### 9.3 Per-wave acceptance

Each wave's acceptance section is authoritative: `P6-ACTORS.md` §12,
`P6-SIGNALS.md` §14, `P6-TRUST.md` §14, `P6-SUBAGENTS.md` §13, plus this file's
§7 and P6-RUNTIME §7 gates.

### 9.4 Parallelization (optional)

The waves are strictly ordered, but within a wave work can be split by file
ownership:

- P6-A: platform (`os_pty_*`) vs core (`actor.c`/`process.c`) vs bash refactor.
- P6-B: bus (`signal.c`) vs queue (`queue.c`) vs RPC/tools.
- P6-C: decide/rules (`trust.c`) vs approval dispatch (`approval.c`) vs platform
  sandboxes.
- P6-D: constructor/tree vs spawn tools vs persistence.

Use worktrees with disjoint file ownership; never edit a file another stream
owns. Append contract changes to the header in the owning branch and note them
in the handoff. Merge at the wave gate.

### 9.5 Standing handoff for an external agent

> You are implementing P6 of `agentc`. Start by reading
> `.agents/plans/P6-RUNTIME.md` in full, then `P6-PRIMITIVES.md` (the minimal
> primitive core + async contract), then `P6-SURFACE.md` (the complete surface
> registry), then the area spec for the wave you were assigned. Work only that
> wave. Obey the append-only rule: add fields at
> the end of structs, add events after `AGENTC_EV_MSG_RESET`, add hook names as
> new constants, add host functions at the end of `AgcExtHost` and gate them
> with `AGENTC_EXT_HOST_HAS`. The tool/RPC/host surface is **op-based** (R-D19):
> build the four primitives and express every named API as a derived adapter;
> do not add per-feature tools, RPC commands, or host methods. Keep the session
> at version 1; persist new state
> through `custom` records. Keep the single-threaded, pump-driven model; no new
> threads, no libc, no OS headers in core. No primitive blocks; every wait pumps
> `runtime_step` (R-D17). Every parser/formatter/state machine
> gets a byte-exact golden test. Run the full gate set before reporting.
> Acceptance is the wave's acceptance section. Record any deviation as a numbered
> decision in the relevant P6 file before committing, and update the matching
> `design/` doc in the same change.

## 10. Risk register

| risk | mitigation |
| --- | --- |
| bash output drift during actor refactor | keep the composition string in one function; the existing bash goldens are the contract (must pass unedited) |
| signal reentrancy / starvation | handlers only inside the drain; bounded drain per call; deterministic order |
| envelope widening bug | intersection test matrix + "deny on error"; root ceiling immutable |
| sandbox platform gaps | `-ENOSYS` → refuse `boundary != none` with an actionable message; `boundary=none` is explicit |
| actor leak | bounded table + deadline + atexit reap + depth-first release; `list` for debugging |
| append-only ABI break | `struct_size`/`AGENTC_EXT_HOST_HAS`/`AGENTC_EXT_FIELD_OK` discipline; no edits to existing fields |
| session reader confusion | new state only via `custom`; verify `message` tolerance or use a wrapper |
| single-threaded fairness | actor tick advances background agents one step per slice; ordered table walk |
| model override surprises | direct inheritance by default; overrides recorded + audited |

## 11. Consolidated open questions (resolve before the dependent wave)

1. PTY on Windows in P6-A vs P6-E (P6-ACTORS §9). Default: pipe-only on Windows
   for P6-A; `tty:true` errors clearly.
2. Linux sandbox backend (Landlock vs bwrap) for P6-C (P6-TRUST §8). Default:
   the minimum that honors `read-only` + `workspace`.
3. Queue persistence granularity (P6-SIGNALS §5). Default: snapshot per op;
   restore + re-validate.
4. Session `message` entries: can they carry optional `agent_id`/`parent_id`, or
   is a `custom` wrapper required? Verify against `src/core/session.c` before
   P6-D.
5. Sub-agent transcript persistence: full vs summarized (P6-SUBAGENTS §8).
   Default: full unless `EPHEMERAL`.
6. Review-step steerability `kind` reserved now (`review`), implemented later.

## 12. Definition of done for P6

- All five area specs' acceptance criteria met.
- P6-RUNTIME §7 gates green at every landing.
- New golden suites pass; existing suites unchanged and green.
- `design/` docs updated in the same commits; `plans/P6-*.md` marked as-built and
  folded into `design/`.
- `.agents/README.md` indexes P6.
- No existing header field or signature edited; session version still 1.
- A short as-built note in each P6 file recording any deviation from this plan.
