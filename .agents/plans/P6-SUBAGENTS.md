# P6-SUBAGENTS — One agent type, the agent tree, direct model inheritance

Status: **as-built (P6-A..E + completion passes 2a-2g).** The agent tree and direct-model spawns.
As-built note: the outcome is folded into `.agents/design/20-core-agent.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

Scope: unifying root and sub-agent into one constructor and lifecycle; the agent
tree registry; direct (in-process, inherited) model/provider/transport; the
spawn/wait/send/interrupt/list tool surface; context isolation and result
handoff; persistence; scheduling; events/hooks; tests.

Non-goals: the actor registry mechanics (`P6-ACTORS.md`), the signal vocabulary
(`P6-SIGNALS.md`), the envelope policy (`P6-TRUST.md`).

---

## 1. Current state (facts to build on)

- `AgcAgent` is opaque; created by `agentc_agent_new(provider, model)` and
  configured with setters: `set_api_key`, `set_base_url`, `set_system`,
  `set_transport`, `set_tools`, `set_events`, `set_thinking`, `set_model`,
  `set_max_tokens`, `set_retry`, `set_record`, `set_insecure`, `set_tool_veto`,
  `set_recompose`, `set_observer`, `set_auto_compact`, `set_compact_limits`.
  `agentc_agent_load` deep-copies a replayed transcript; `agentc_agent_submit`
  runs one turn to completion; `agentc_agent_abort` cancels.
- The app's setup lives in `src/app/mode.c`: `mode_configure_agent` (the single
  configure path) and `agentc_mode_rebuild_agent` (model/provider swap). Both
  create a fresh `AgcAgent` and re-apply setters. This is the code path R-D1
  says root and child must share.
- The event hub (`src/core/events.c`) dispatches the **per-agent callback**
  (`agentc_agent_set_events`) first and separately, then the process-wide slots.
  Multiple agents already coexist in principle.
- There is no parent/child relationship, no tree, no spawn tool, and no way to
  run two agents in one session.
- `AGENT_CONTINUATION_MAX` is 32 (`step_end.continue` + `agent_before_settle`).

## 2. One agent type, one constructor

No new type. `AgcAgent` remains the only agent; add a spec and a single
constructor that both root and child use.

```c
/* include/agent.h (append-only) */
typedef struct AgcAgentSpec {
    struct AgcAgent *parent;    /* NULL for the root */
    const char *task;           /* initial user/task text; NULL for root */
    const char *provider;       /* NULL => inherit parent's current provider */
    const char *model;          /* NULL => inherit parent's current model (direct) */
    int         thinking;       /* -1 => inherit */
    i64         max_tokens;     /* 0  => inherit */
    const AgcTrustEnv *trust;   /* NULL => inherit; else validated narrow/own (P6-TRUST) */
    const AgcTool *tools; size_t ntools;  /* NULL => parent's effective table */
    const char *system;         /* NULL => auto-build from the live tool table */
    AgcMsgObserver observer; void *observer_ud;
    AgcEventFn     events;   void *events_ud;
    uint32_t    flags;          /* AGENTC_AGENT_F_* */
} AgcAgentSpec;

#define AGENTC_AGENT_F_INHERIT_CONTEXT 0x1u  /* copy a bounded parent context */
#define AGENTC_AGENT_F_EPHEMERAL       0x2u  /* do not persist the transcript */
#define AGENTC_AGENT_F_WAIT            0x4u  /* run to completion before returning (spawn default) */

AgcAgent *agentc_agent_create(const AgcAgentSpec *spec);   /* NULL on failure */
```

`agentc_agent_create` performs, in one place, the whole startup:

1. Register an agent actor (`agentc_actor_register(AGENT, parent, label)`), set
   `depth`, resolve lineage.
2. Resolve provider/model/thinking/transport: inherit from `parent` when unset
   (§4), else the provided values; apply `set_transport` (the shared HTTP
   transport, or a test double).
3. Resolve the envelope: inherit or validate `own` against the parent (P6-TRUST
   §3); attach it to the actor's `env`.
4. Deep-copy the tool table: `tools` if given, else the parent's effective
   table, filtered by the envelope (RO hides write tools) and by depth
   (`agent op:spawn` removed at `max_depth`).
5. Build the effective system prompt (`system` or auto) using the same
   `agentc_ext_prompt_build`, including the envelope and `is_subagent` marker.
6. Wire observer/events/veto/recompose/pump and, for a child, install the
   completion callback that produces the `FINISH` signal.
7. If `task` is set, submit it through the normal path; if `WAIT`, run to
   completion before returning (scheduling §6).

`agentc_agent_new(prov, model)` becomes a thin wrapper:
`agentc_agent_create(&(AgcAgentSpec){.parent=NULL,.provider=prov,.model=model})`.
`mode_configure_agent` becomes a caller of `agentc_agent_create` (it keeps the
app-only bits: session binding, sinks, CLI parsing). The invariant: **there is
no second startup path.**

A child's teardown mirrors startup: on `FINISH`/`FAIL`/`CANCELLED`, release
descendants, then `agentc_actor_release(id)` clears `actor->agent` and frees the
`AgcAgent`.

## 3. The agent tree registry

The actor registry (`P6-ACTORS.md` §2) *is* the tree: agent actors carry
`parent`/`depth`, and `AgcActor.agent` points at the live `AgcAgent`. Add a
convenience view:

```c
/* Agent-only projection of the actor table. Count-first. */
size_t agentc_agent_list(AgcAgentInfo *out, size_t max);
typedef struct {
    uint32_t id, parent_id;
    uint16_t depth;
    int      state;            /* AgcActorState */
    const char *model, *provider, *task;
    const AgcTrustEnv *trust;
    i64      duration_ms;
    int      exit_code;
} AgcAgentInfo;
```

- Ids are stable and stamped on every event. `id`/`parent_id`/`depth` are the
  lineage contract for UI, session, approvals and debugging.
- The root is `parent_id == 0`, `depth == 0`.
- `agentc_actor_release` on an agent is depth-first; a child never outlives its
  parent.

## 4. Direct model and inheritance

R-D2, made concrete:

- `provider == NULL` → the parent's current provider (the parent may have
  switched via `/model`).
- `model == NULL` → the parent's current model. There is **no**
  `default_subagent_model` config and no separate sub-agent model registry.
- `thinking == -1`, `max_tokens == 0` → inherit.
- `transport` is the same in-process `AgcTransport` (`agentc_transport_http` or
  a test double), shared. No second provider stack, no re-auth, no subprocess.
- Auth/OAuth: the child resolves credentials through the same
  `agentc_setup_resolve_key` path on demand; a stored OAuth credential is shared
  (single process).
- An explicit override is allowed and recorded: `{agent_id, provider, model,
  reason}` in the `AGENTC_EV_ACTOR_STARTED` payload and as a `custom` session
  record. The override is envelope-checked (a child cannot use a model to gain a
  capability; capability is the envelope, not the model).
- Compaction, retry and event hub are the shared core code; the child compacts
  its **own** transcript (§7).

## 5. Tool surface

Registered by `builtin-tools`, hidden until enabled. Per R-D19 / `P6-PRIMITIVES`
§6 the model-facing surface is **op-based**: one `agent` tool (`P6-SIGNALS` §11),
not five named tools. The ops cover the Codex delegated-agent semantics without
per-op tool names.

| op | args | behavior |
| --- | --- | --- |
| `spawn` | `{task, model?, provider?, thinking?, trust?, tools?, wait?, inherit_context?}` | `actor_spawn` with `parent=self`; returns `{agent_id}`, or with `wait:true` (default) the child result |
| `wait` | `{agent_id, timeout_ms?}` | `actor_await` + `actor_read` until `FINISH`/`FAIL`; returns the result |
| `send` | `{agent_id, text, turn?, priority?, flags?}` | `queue_add` into the child (`P6-SIGNALS` §5) |
| `interrupt` | `{agent_id, scope?: step\|actor, stage?}` | `signal(CANCEL)` to the child |
| `freeze` / `resume` | `{agent_id, scope?}` | `signal(FREEZE/RESUME)` |
| `result` | `{agent_id}` | `actor_read`/`actor_query` of the final result |
| `list` | `{}` | `actor_list` filtered to agents |

Rules:

- `agent op:spawn` is a tool call and therefore flows through the trust pipeline
  (P6-TRUST §4): `ALLOW_SUBAGENT` gates it; a widening envelope request needs an
  approval; at `max_depth` `spawn` is hidden (declaration) and denied
  (enforcement).
- The child's tool table = the parent's effective table minus `agent op:spawn`
  at depth ≥ `max_depth`, filtered by access mode. The other `agent` ops
  (`wait`/`send`/`interrupt`/`freeze`/`resume`/`result`/`list`) remain available
  at any depth.
- `wait:false` returns the handle immediately (background child); the parent
  later calls `agent op:wait`, or receives `FINISH` as a `NOTIFY`/event.
- A child may spawn its own children (up to `max_depth`), using `agent op:spawn`.

## 6. Scheduling (single-threaded, cooperative)

- **Default: run-to-completion, pumped.** `agent op:spawn{wait:true}` runs the
  child's turns inside the parent's turn, driven by `agentc_pump` so the TUI
  repaints and input/abort/steer stay live. This is the simplest correct model
  and matches "direct".
- **Background child:** `wait:false` registers the child and returns; the parent
  continues. `agentc_actor_tick` advances background agent actors at each pump
  slice (one step step per slice), so multiple children make progress
  cooperatively without threads.
- **Interleaving:** the actor tick drives at most one step of each runnable
  background agent per slice; ordering is table order (deterministic). A future
  round-robin scheduler is a policy change, not an API change.
- **Cancellation:** parent cancel cascades depth-first (P6-SIGNALS §7); a child's
  `step` cancel leaves its background processes alive (they belong to its
  lifetime), `actor` cancel releases them.
- Steer/queue authority: the parent is the child's controller (R-D9); a child
  `notify`s upward.

## 7. Context, transcript, result handoff, compaction

- **Isolation by default.** A child starts with an empty transcript and only its
  `task`, plus the auto system prompt built from the child's effective tool
  table and the shared context/skills machinery
  (`agentc_builtin_context_set`). The parent transcript is **not** shared.
- `AGENTC_AGENT_F_INHERIT_CONTEXT` (opt-in) copies a **bounded** subset: the
  last `AGENTC_AGENT_CTX_KEEP` messages (proposed 8) or, if a summary exists, the
  summary + last 2. Never the raw parent transcript.
- **Result handoff.** The child's final assistant text (last `STOP`/`LENGTH`
  message) plus `{usage, agent_id, duration_ms, exit_code}` becomes the
  `agent op:spawn`/`op:wait` tool result — an ordinary tool result. A `FAIL` or
  `CANCELLED` child returns an error result with the reason.
- **Compaction** runs per agent via the existing `src/core/compact.c` on the
  child's own transcript; the parent is untouched. A quiet compaction on a child
  does not emit content events to the parent (the quiet gate is per-emit; the
  child's own events are gated).
- A child's tool calls are persisted with `agent_id`/`parent_id` stamps so the
  tree replays (§8).

## 8. Persistence and resume

- R-D11: session format stays v1. Child and root messages are written through
  the same observer; each `message` entry gains optional `agent_id`/`parent_id`
  fields (unknown to old readers, preserved as strings by the existing JSONC
  tolerance — verify before landing; if `message` entries are strict, use a
  `custom` `agent_message` wrapper instead).
- A child's lifecycle is recorded as `custom` records:
  `{"type":"agent","op":"spawn|finish|fail|cancel","agent_id":…,"parent_id":…,
  "model":…,"provider":…,"task_hash":…,"result_hash":…}`. `task`/`result` text is
  stored in full unless `EPHEMERAL`.
- On resume, the tree is reconstructed from those records; children are shown in
  `/agents` (read-only history) but not re-run. The root transcript replays as
  today. `EPHEMERAL` children leave no records (still shown live).
- Open question (P6-RUNTIME §9): full child transcript vs summarized trace.
  Default: full, unless `EPHEMERAL`.

## 9. Interaction with trust and signals

- **Envelope:** the spec's `trust` is resolved in `agentc_agent_create` step 3;
  `AGENTC_EV_TRUST_SET` reports it; `AGENTC_HEV_TRUST_SELECT` may observe/narrow.
  The child's sandbox applies to every process/tool it spawns (P6-TRUST §8).
- **Signals:** `STARTED`/`PROGRESS`/`STALLED`/`FINISH`/`FAIL`/`NOTIFY` are
  emitted by the child to its parent; `INPUT`/`CANCEL` flow parent→child.
- **Approvals:** a child's `tool_approval` routes to the approver for the
  child's envelope, falling back up the chain (P6-TRUST §7.3).

## 10. Events and hooks

- Reuse `AGENTC_EV_ACTOR_*` (P6-SIGNALS §10) for lifecycle; a child's content
  events (`message_*`, `tool_execution_*`) also fire through the same hub and are
  attributed by `agent_id`.
- `AGENTC_EV_ACTOR_STARTED` for an agent carries
  `{agent_id,parent_id,depth,provider,model,trust,is_subagent:true}`.
- New hook points: none required beyond P6-SIGNALS/B; `agent_start` and
  `agent_end` payloads gain `agent_id`/`parent_id`/`depth`/`is_subagent`.
- `AGENTC_HEV_ACTOR_*` observers can render the tree.

## 11. Failure modes

| Failure | Behavior |
| --- | --- |
| spawn at `max_depth` | tool hidden + layer-2 deny with a clear reason |
| widening trust request | denied/clamped (P6-TRUST §3) |
| model/provider override invalid | spawn fails with a typed error; no partial agent |
| actor table full | spawn refused (P6-ACTORS §2.2) |
| child fails/cancels | parent gets an error/aborted tool result with the reason |
| child stalled | parent receives `STALLED`; chooses input/cancel/wait |
| parent cancelled while child runs | depth-first cascade; children reach `CANCELLED` |
| background child leak | bounded by `deadline_ms`/`max_depth`; reaped on parent release/new_session/exit |
| resume with an orphan child record | reconstructed read-only in `/agents`; never re-run |

## 12. Golden test plan (new `tests/subagents_test.c` + `.expected`)

Deterministic: mock provider (`tests/mock_provider.py`-style) + mock transport;
fake clock; recorded signal/event log.

- Unified constructor: root and child produce the same event sequence shape;
  assert the app path and the spawn path call the same `agentc_agent_create`.
- Direct model: child with no override uses the parent's current model; after a
  parent `/model` swap, a newly spawned child uses the new model; an explicit
  override is recorded.
- Tree: `agent op:list` shows lineage/depth/state; parent release cascades.
- spawn/wait/send/interrupt: `wait:true` returns the child result; `wait:false`
  - `agent op:wait`; `agent op:send` steers a child step; `agent op:interrupt`
  cancels step vs actor.
- Envelope inheritance: child inherits; `own` narrows; widening denied; RO child
  cannot write.
- Depth limit: at `max_depth`, `agent op:spawn` hidden + denied.
- Context isolation: child transcript empty except `task`; `INHERIT_CONTEXT`
  copies exactly `CTX_KEEP`.
- Persistence: child `custom` records byte-exact; resume reconstructs the tree
  read-only.
- Compaction: child compacts its own transcript; parent untouched.

## 13. Acceptance for P6-D

1. `agentc_agent_create` exists; `agentc_agent_new` delegates to it; the app's
   `mode_configure_agent` calls it. No second startup path.
2. Direct inheritance of provider/model/thinking/transport; explicit override
   recorded and envelope-checked; no sub-agent-model config.
3. The agent tree exists with lineage; depth-first release; `agent op:list`
   projection.
4. The `agent` tool ops spawn/wait/send/interrupt/freeze/resume/result/list
   implemented as the op-based tool and RPC; depth/authority/envelope enforced.
5. Context isolation + bounded inherit + ordinary-tool-result handoff.
6. Per-agent compaction; per-agent persistence stamps; resume reconstructs the
   tree read-only.
7. Every §12 golden passes; P6-RUNTIME §7 gates green.
8. `design/20-core-agent.md` documents the unified constructor, the tree, and
   the spawn tools in the same change.

## 14. Handoff (P6-D)

> Implement `plans/P6-SUBAGENTS.md` only, on top of green P6-A/B/C. Add
> `AgcAgentSpec`/`AgcAgentInfo` and `agentc_agent_create`/`agentc_agent_list` to
> `include/agent.h` (append-only); implement the constructor in
> `src/core/agent.c` (or a new `src/core/agent_spawn.c`) reusing the existing
> setters and the actor registry; make `agentc_agent_new` and
> `src/app/mode.c`'s `mode_configure_agent` call it; add the spawn tools to the
> builtin-tools registry and the RPC commands to `src/app/mode_rpc.c`; add the
> per-agent compaction/context/persistence glue. Do not change the session
> header version. Acceptance and tests are §13/§12. Append to headers; never
> edit. Keep every gate green.

## 15. Audit addenda (authoritative in `P6-SURFACE.md`)

This file predates the surface audit; `P6-SURFACE.md` §3.8/§18 adds the explicit
tree/control APIs: `agentc_agent_find/info/parent/wait/send/cancel/result/
turn_no/steerable/result_set`, the `agent_result` event/hook, `AgcAgentSpec.env`,
and the `depth_limit`/`authority_denied` error codes. **`agentc_agent_wait` must
service the signal bus** (R-D17) so a waiting parent can answer a child's
`notify`/approval; a bare blocking wait is a bug. Fold these in; no decision
(R-D1/R-D2/R-D9) changes.
