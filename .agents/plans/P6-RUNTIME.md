# P6 — Runtime: actors, processes, trust, sub-agents, steering, voice

Status: **as-built (P6-A..E + completion passes 2a-2g).** Umbrella spec for the runtime primitives and the decision log.
As-built note: the outcome is folded into `.agents/design/00-architecture.md` and `.agents/design/20-core-agent.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

Finalized shape (R-D22/R-D23/R-D26/R-D31): **four primitives** — Runtime, Actor
(`agent | process`), Message (unifies control signals + ordered input),
Envelope — and **two hook verbs** (return the next message, or defer). The
pipeline has **ten stages** (`turn`, `step`, `input`, `prompt`, `message`,
`output`, `tool`, `signal`, `event`, `lifecycle`); tools are **three** op-based
tools that mirror the actor kinds plus queue (`process`/`agent`/`queue`); queue,
signal kinds, hooks, deciders/router/jev, controller decisions, approval,
settings and status are *derived services*. Minimal core and async contract:
`plans/P6-PRIMITIVES.md`; the layered API: `plans/P6-API.md`; implementer guide:
`plans/P6-EXTENDING.md`; exhaustive registry: `plans/P6-SURFACE.md`.

Complements (does not replace) P5:

- `plans/P5-AXES.md` — RPC v1, `libagentc` SDK, framed CBOR. P6 adds commands
  and events to that vocabulary; it does not change the framing.
- `plans/P5-TOOL-ENGINE.md` — external/built-in `grep`/`find`/`ls`. Unchanged by
  P6; the tool table still comes from `agentc_ext_tools`.

Related P6 files:

| File | Covers |
| --- | --- |
| `plans/P6-API.md` | the API surface, layered (read this to understand it) |
| `plans/P6-EXTENDING.md` | implementer guide: extension / tool / MCP / governance / access / hook |
| `plans/P6-PRIMITIVES.md` | minimal core: four primitives (runtime/actor/message/envelope) + two hook verbs |
| `plans/P6-SURFACE.md` | complete surface registry + coverage audit (authoritative inventory) |
| `plans/P6-ACTORS.md` | actor registry, processes/background shells, PTY, stdin, output, cancel, freeze, stall |
| `plans/P6-SIGNALS.md` | message bus: notify, step+priority queue, steer, interrupt, deciders |
| `plans/P6-TRUST.md` | trust envelopes, read-only/read-write, sandbox, approvals, delegation |
| `plans/P6-DECIDERS.md` | decision points, the jev decider, the classified router, controller decisions |
| `plans/P6-HOOKS.md` | the universal interception pipeline: stages, two verbs, universal tool path |
| `plans/P6-VOICE.md` | realtime voice as an extension + process actor (media-agnostic pipeline) |
| `plans/P6-SETTINGS.md` | extension settings (grouped by extension) + prompt contributions |
| `plans/P6-STATUS.md` | status lines in every region (chrome + per-step events) |
| `plans/P6-SUBAGENTS.md` | one agent type, unified constructor, agent tree, direct model, spawn tools |
| `plans/P6-INTEGRATION.md` | header/event/hook/ABI inventory, file map, test matrix, master handoff |

---

## 0. Why (the problems P6 solves)

Today `agentc` has one flat tool list, one bounded job batch
(`src/core/tools/jobs.c`), and one agent per process driven by
`agentc_agent_submit` (`src/core/agent.c`). This is deliberate and works, but it
cannot express four things users and models increasingly need, and Codex
0.159.3 (the reference here) does express them:

1. **Long-running shells.** `bash` (`src/core/tools/bash.c`) runs to completion
   or is killed by cancel/timeout. A server, a watch loop, or an interactive
   debugger cannot be started, polled, fed stdin, or killed independently.
   Codex: `exec_command` → integer `session_id`, then `write_stdin`, terminate.
2. **Sub-agents.** There is no way for an agent to delegate a subtree of work to
   a child that shares the same model, provider and transport. Codex:
   `spawn_agent` / `wait_agent` / `send_message` / `interrupt_agent`.
3. **Security boundaries per unit of work.** `agentc` has project trust
   (`include/config.h` `agentc_trust_resolve`) but no read-only/rw mode, no
   sandbox, and only a blocking `tool_call` veto — no way to declare "this
   subtree may read but not write" and act inside it without prompts. Codex:
   `SandboxPolicy` + `ApprovalPolicy` + `CommandExecutionRequestApproval`.
4. **Controlling a turn in flight.** `agentc_agent_abort` is a single hard stop.
   There is no queue, no steer, no graceful-then-force, no "stop this turn but
   keep the agent", no freeze. Codex: `turn/steer`, `thread/queue/*`,
   `turn/interrupt`, SIGTERM→SIGKILL.

P6 adds all four **without** adopting Codex's app-server breadth (393 schema
files, `command/*` and `process/*` families, threads server). It does so with
four primitives and append-only additions to the existing headers.

## 1. The four primitives

```text
Runtime   — the single-threaded scheduler/reactor: fds, clock, continuations.
Actor     — an addressable unit of work: `agent` (model loop) | `process` (OS process).
Message   — the one communication unit: CONTROL signals AND ordered input (turn, priority).
Envelope  — the trust/sandbox boundary an actor carries.
```

Everything else is derived: **queue** is the INPUT subset of an inbox;
**signal kinds** are message kinds; the **hook pipeline** is an ordered transform
chain with two verbs; **deciders/router/jev/controller**, **approval**, **policy**,
**settings**, **tools**, **RPC**, **events**, **session** are services/adapters.
Full detail and the async contract: `P6-PRIMITIVES.md`. A **hook** returns the
next message (or `NULL` to consume) or sets `defer` (a runtime continuation).

## 2. Glossary (normative)

- **Actor** — a registered unit of work with a stable id. Kinds: `agent`,
  `process`. Fields and lifecycle: `P6-ACTORS.md` §2.
- **Agent actor** — an `AgcAgent` instance (root or child) registered as an
  actor. The root is `parent_id == NULL`, `depth == 0`.
- **Process actor** — a child OS process (pipe or PTY) registered as an actor,
  owned by the agent that spawned it. Shells, background servers, external tool
  engines and MCP servers are all process actors.
- **Envelope** — `AgcTrustEnv`: access (`read-only|read-write`), boundary
  (`none|workspace|strict`), approval (`auto|ask|policy`), roots, network, and
  per-subtree flags. `P6-TRUST.md` §2.
- **Message** — the one communication unit `{seq, from, to, class, kind, turn,
  priority, flags, actor_id, depth, id, direction, final, body, body_len,
  content_type, meta, ts}`. Classes `CONTROL | INPUT | NOTIFY | HOOK`.
- **Queue item / turn target / priority** — agent input is the INPUT subset of
  the inbox; each item carries a `turn` (0 = auto, >0 absolute) and a signed
  `nice`-like `priority` (`<0` preempts now, `>=0` lands at the next safe
  boundary; ascending value = earlier). `P6-SIGNALS.md` §5.
- **Signal** — historical name for a CONTROL-class message.
- **Cancel scope** — `turn` (the user-driven cycle; Codex `turn/interrupt`) or
  `actor` (kill). §7.
- **Graceful / force** — the two-stage cancellation escalation. §7.
- **Progress / stalled** — liveness. `P6-ACTORS.md` §7.
- **Freeze / resume** — suspend an actor at its current state and resume later.
  `P6-ACTORS.md` §6b.
- **Direct model** — a child agent uses the parent's *current* provider, model,
  thinking and transport unless it explicitly overrides. `P6-SUBAGENTS.md` §4.
- **Narrow-only** — a child envelope is an intersection of its parent's; a child
  cannot gain a capability its parent lacks without an audited approval.
- **Root ceiling** — the root agent's envelope is the maximum for the tree.
- **Pipeline / hook** — the ordered transform chain over every flow, two verbs.
  `P6-HOOKS.md`.
- **Decider / router / jev / controller** — hook handlers that answer a decision
  and branch. `P6-DECIDERS.md`.
- **Setting** — typed extension configuration over `config.jsonc`, grouped by
the extension's own name (no group object). Distinguish from a **prompt
contribution** (`add_prompt`: system-prompt text) and **user input** (the `input`
hook stage). `P6-SETTINGS.md`.

### 2.1 Hierarchy — turn / step / message / block (authoritative)

**This is the terminology used across every P6 doc.** It matches Codex: a
**turn** is the user-driven cycle (Codex `turn/start`, `turn/steer`,
`turn/interrupt`); **step** is the inner model cycle (Codex does not expose a
public name for it).

| term | definition | bounded by |
| --- | --- | --- |
| **Turn** | one `agentc_agent_submit`: a user input drives the agent to settle | `agent_start`…`agent_end`/`agent_settled` |
| **Step** | one model request/response **+ the tool batch it requested** | `step_start`…`step_end`; `1..N` per turn, `AGENT_CONTINUATION_MAX` (32) |
| **Message** | a `user`/`assistant`/`system`/`tool` transcript message | one assistant message per step (streamed) |
| **Block** | a piece inside a message: `text`, `thinking`, `tool_call` | many per message |
| **Tool call** | one `tool_call` block (id, name, args) | many per assistant message |
| **Tool batch** | every tool call in one assistant message, run together | results appended **within the same step** |
| **Tool result** | a `tool` message answering one call (carries the call id) | one per call |
| **User input** | the first `user` message of a turn | — |
| **Thinking** | reasoning `thinking` blocks (not user-facing output) | per assistant message |
| **Final output** | the last assistant `text` of the turn (a step with `stop`, no calls) | — |
| **Retry** | a repeated request attempt **within the same step** (`message_reset`) | creates no new step |
| **Settle** | turn completion (`agent_settled`) | once per turn |

**Tool calls belong to the same step, not separate steps.** A step is *one model
response plus its whole batch*; the batch may contain many calls. The next step
begins when the model is called again with the tool results.

```text
TURN  (agent_start)                                   ← user-driven cycle
├─ user message "fix the bug"                         ← user input
├─ STEP 1  (step_start)                               ← model request #1
│   assistant message: thinking + text + tool_call(read), tool_call(grep)
│   tool results: read→…, grep→…                       ← same step (one batch)
│   (step_end)
├─ STEP 2  (step_start)
│   assistant message: text + tool_call(edit) + tool result(edit→ok)
│   (step_end)
├─ STEP 3  (step_start)
│   assistant message: text "done: …"                  ← final output
│   (step_end)
└─ settle (agent_end / agent_settled)
```

**Consequences used elsewhere:** the queue/steer/cancel target a **turn**
(`turn` field, `cancel{scope:turn}`, `active_turn_not_steerable`); a steer lands
at a **step** boundary within that turn (or preempts the current step,
`priority<0`); the `step` hook stage is the model cycle
(`start`/`end`/`model_select`/`compact`/`steer`); the `STEP_END`/`TURN_END` status
regions are the step end and the turn's final output respectively.

## 3. Decision log (authoritative)

These decisions are user-directed; implementation must not silently change them.
IDs are `R-Dn` (runtime). Area-local decisions live in the area files.

### R-D1 — One agent type, one startup path

There is no separate "sub-agent" type or startup path. `AgcAgent` is the only
agent. Root and child are both produced by the unified constructor. The app's
current `mode_configure_agent` / `agentc_mode_rebuild_agent` become callers of
that constructor. *Rejected:* a distinct `SubAgent` type or a child-only spawn
function (guarantees divergent lifecycles). See `P6-SUBAGENTS.md` §1–2.

### R-D2 — The sub-agent model is direct and inherited

A child defaults to the parent's current provider/model/thinking and the same
in-process transport. There is no separate "sub-agent model" configuration key
and no process boundary. A child may override explicitly; the override is
recorded and envelope-checked. *Rejected:* a configured cheap sub-agent model by
default (surprising), or spawning children as separate processes (breaks the
one-static-binary, single-threaded posture and the shared transport).
See `P6-SUBAGENTS.md` §4.

### R-D3 — Trust is declared per agent/sub-agent, inherited by default

An agent or sub-agent *specifies* its boundary: inherit the parent envelope, or
declare its own. Nested boundaries are allowed; they may only narrow.
See `P6-TRUST.md` §2–3.

### R-D4 — Two access modes: read-only and read-write. Default is read-write

Inside its boundary the agent is trusted and **no approval is raised**. Default
envelope = `access=read-write`, `boundary=workspace`, `approval=auto`. This is the
"trust the model" default. *Rejected:* default `ask` (prompt fatigue), default
`read-only` (breaks normal editing). See `P6-TRUST.md` §2, §4.

### R-D5 — Fine-grained approval is opt-in and delegable

Per-tool-call approval rules may be supplied. A decision may be handed to an
external program and/or to a reviewer model (e.g. `jev`) through one stable JSON
contract. Approvals are scoped (`once|step|session|project`) so identical calls
do not re-prompt. See `P6-TRUST.md` §5–7.

### R-D6 — Actors, not parallel subsystems

Background processes, foreground `bash`, and agents are all actors. Cancellation,
stdin/message, notify, stall, freeze and queue are all messages.
See `P6-ACTORS.md`, `P6-SIGNALS.md`.

### R-D7 — One cancellation model: graceful then force, idempotent

`cancel` carries a scope (`step|actor`) and stages: `cancel_graceful` first, then
automatic `cancel_force` after `grace_ms`. Applies identically to shells and
agents at every depth. `cancelled` is emitted exactly once.
See `P6-SIGNALS.md` §7.

### R-D8 — One turn-targeted, priority-ordered queue (queue + steer + interrupt)

Agent input is a **single queue**; there is no `delivery` enum and no `preempt`
bitmask. Each item carries a `turn` and a signed `nice`-like `priority`:
`turn` selects the turn (0 = auto: current if active else next; >0 absolute),
and the priority selects the phase/order — `<0` preempts immediately (mid-stream
and, by default, mid-step: "not waiting for the tool call"), `>=0` lands at the
next safe boundary in ascending priority order. The queue is editable
(add/list/update/delete/reorder/start). A non-steerable turn holds the item for
the next turn and returns `active_turn_not_steerable` (or cancel+resubmit with a
flag). *Rejected:* three named delivery modes (redundant, ambiguous) and an
explicit `preempt` bitmask (subsumed by the priority sign plus one flag).
See `P6-SIGNALS.md` §5–6, §9 (local decisions S-D1…S-D6).

### R-D9 — Only the controller steers a turn

A parent (or the user) may steer/queue/interrupt a child's turn. A child signals
upward with `notify`; it never steers the parent's turn. Matches Codex's "only
user input can steer a turn" (Codex `turn` = P6 turn). See `P6-SIGNALS.md` §6.

### R-D10 — Append-only interfaces

All new fields are appended to existing structs; all new events are appended to
the `AGENTC_EV_*` enum after `AGENTC_EV_MSG_RESET`; all new hook point names are
new string constants; all new host functions are appended to `AgcExtHost` and
gated with `AGENTC_EXT_HOST_HAS` / `AGENTC_EXT_FIELD_OK`. No existing field or
signature is edited. See `P6-INTEGRATION.md` §2.

### R-D11 — Session format stays v1

New state (actors, queue, approvals, child messages) is persisted through
`custom` session records via `agentc_session_append_raw`. The session header
`version` is not bumped; the existing `>1` refusal stays.
See `P6-INTEGRATION.md` §6.

### R-D12 — Single-threaded, pump-driven, deterministic

No new threads. Actors are advanced by the existing cooperative pump
(`agentc_pump`, `os_poll`). Signal drain order is deterministic so the golden
transcript suite covers it. See `P6-INTEGRATION.md` §7.

### R-D13 — Bounded and fail-closed

Actor count, mailbox depth, queue length, output rings, and approval scope are
all bounded (`AGENTC_LIMIT_*`, `src/base/limits.h`). Lifecycle signals are never
dropped on overflow; `progress`/`notify` are dropped first. A missing/malformed
approver or sandbox failure is a denial, never an allow.
See `P6-TRUST.md` §9, `P6-SIGNALS.md` §8.

### R-D14 — No downloads, no daemon, no JS runtime

Consistent with `design/00-architecture.md` §10. P6 does not add a code-mode JS
sandbox, an app-server daemon, or a network-fetched tool. The actor/signal design
reaches Codex's *semantics* with local, static primitives.

### R-D15 — Codex is the parity reference for behavior, not for structure

Where Codex names a concept (`exec_command`, `write_stdin`, `turn/steer`,
`thread/queue/*`, `turn/interrupt`, `spawn_agent`), P6 adopts the *semantics* and
records the mapping. Where Codex's structure conflicts with `agentc` constraints
(app-server, separate `command/*` and `process/*` families), P6 chooses the
smaller unified form and documents the divergence.

### R-D16 — Two event layers: content and control

The existing content events (`message_*`, `tool_execution_*`, `turn_*`) are
unchanged. New lifecycle/control facts ride the signal control layer. Front ends
and extensions may subscribe to either. See `P6-INTEGRATION.md` §3.

### R-D17 — One runtime lifecycle; no blocking wait may skip the bus

`agentc_runtime_init/tick/shutdown` own actor + signal + trust + queue init and
reap; the pump calls `runtime_tick`. **Every blocking wait** (`agentc_agent_wait`,
interactive approval, `agent op:wait`, the TUI idle loop) must run the tick/drain
inside its pump loop, so a waiting parent can still service a child's `notify`
and approval requests. A wait that blocks without draining is a bug (it can
deadlock the tree). `runtime_shutdown` performs a graceful turn cancel →
actor reap, bounded. See `P6-SURFACE.md` §18 #4, `P6-INTEGRATION.md` §4.

### R-D18 — Freeze/resume is a first-class actor operation

An actor can be **frozen at its current state and resumed later**, without
killing it. Freeze scopes are actor, subtree and runtime/global. Process actors
`SIGSTOP`/`SIGCONT` their group; agent actors freeze cooperatively at a safe
point (in-flight request `DRAIN` by default, `ABORT` optional). All timers
(deadline, grace, stall) are suspended while frozen and shifted forward on
resume, so freezing never causes a timeout. Cancel resumes before terminating.
LIVE freeze lives only for the `agentc` process; an optional checkpoint backend
(Linux CRIU) may persist a process tree across restarts, but agent step state is
recovered from the session (no in-memory checkpoint). See `P6-ACTORS.md` §6b,
`P6-SIGNALS.md` §7b.

### R-D19 — Five primitives; the signal kind is the opcode; nothing blocking. *(superseded in part by R-D22)*

The irreducible core is Runtime, Actor, Signal, Queue, Envelope. Control ops are
**signal kinds**, not separate functions or tools; new capabilities are new
signal kinds, so the primitive function set is closed and does not grow per
feature. Nothing blocks; `runtime_step` is the only driver; all fds are exposed
to one poll set. See `P6-PRIMITIVES.md`, `P6-HOOKS.md`.

### R-D20 — Decisions are a seam, not a proxy; deciders and routers are backends

A **decision point** (`route`, `approve`, `model_select`, `spawn`, `compact`,
`steer_policy`, `escalate`) emits a typed request and accepts a typed response.
A **decider** (local rules, external program, MCP server, or a review **model
such as jev**) answers one or more points; the **classified router** is a decider
at `route` that classifies the input and chooses the actor/model/tools/envelope/
budget. Deciders never sit in the token/data path and never execute anything;
they can only *narrow* the envelope, and the core validates and applies the
verdict through the existing primitives. See `P6-DECIDERS.md`.

### R-D21 — One universal hook pipeline for every flow. *(superseded in part by R-D22)*

Input, output, tool calls, tool results, messages, prompts, signals, events and
decisions all pass through a single ordered **hook pipeline**. A hook is a small
handler; see R-D22 for the reduced contract (two verbs). This is the one
interception mechanism: the extension hook bus is its C-handler backend,
`P6-TRUST` approval is a `tool` handler, and `P6-DECIDERS` router/jev are
handlers on `input`/`tool`/`step`. It is a **service**, not a primitive (R-D22).
See `P6-HOOKS.md`.

### R-D22 — Four primitives; hooks have two verbs; everything else is derived

The irreducible core is **Runtime, Actor, Message, Envelope**. **Message**
unifies the old Signal and Queue (control and ordered input are one envelope with
`class` + `(turn, priority)`). A **hook** is an ordered transform over a message
with exactly **two verbs**: return the next message (covers pass/edit/replace/
observe, and consume via `NULL`) or **defer** (a runtime continuation, covers
async/branch/await). `branch`, `emit`, `fork`, deciders, the classified router,
jev, controller decisions and approval are one-line combinators over
`actor_spawn` + `message_send` + `runtime_defer`, not dispositions the core
interprets. No primitive blocks. See `P6-PRIMITIVES.md`, `P6-HOOKS.md`.

### R-D23 — One universal path for turn/step, tools, and decisions; primitives are uniform

The pipeline has **ten stages**: `turn` (user cycle: start/end/steer), `step`
(model cycle: start/end/model_select/compact), `input`, `prompt`, `message`,
`output`, `tool` (call/result), `signal`, `event`, `lifecycle`. The `turn` and
`step` stages give transformable per-turn and per-step seams (replacing the
observe-only events). **Every tool — internal, external and
MCP — is one runner** (`INTERNAL`/`PROCESS`/`MCP`) invoked through the same
`tool` stage and the same envelope/approval path; internal tools go through hooks
by default and may opt out (`AGENTC_TOOL_NO_HOOKS`/`tools.hooks=off`). Every
external process (bash, `rg`/`fd`/POSIX engines, MCP servers) is a **process
actor**; `jobs.c` becomes a scheduler over runners and no longer owns pid/fd.
**Decisions are handlers, not stages**: `route`, `approve`, `spawn`,
`model_select`, `compact`, `steer_policy` are `hook_branch` handlers on
`input`/`tool`/`step`, so the `decision`/`route` stages are removed. The four
primitives are unchanged and now apply uniformly across agents, processes, tools
and MCP. See `P6-HOOKS.md` §1/§3/§4, `P6-ACTORS.md` §8.1.

### R-D24 — Actor kinds are `agent | process`; `terminal` is a flavor

There are two actor kinds: `agent` (an in-process model loop) and `process` (a
running OS process). The earlier "terminal" kind is renamed: a terminal is just
a **process actor with `tty=true`** (a flag), not a distinct kind. Shells,
background servers, external tool engines and MCP servers are all `process`
actors. Renames: `AgcTermSpawn`→`AgcProcSpawn`, `AgcTerminal`→`AgcProcess`,
`agentc_terminal_*`→`agentc_process_*`, `AGENTC_ACTOR_TERMINAL`→
`AGENTC_ACTOR_PROCESS`, `AGENTC_TERM_*`→`AGENTC_PROC_*`.

### R-D25 — One actor factory

`agentc_actor_spawn(const AgcActorSpec *)` with a tagged spec
`{ const AgcProcSpawn *proc; const AgcAgentSpec *agent; }` (exactly one set). The
kind-specific builders are private. A host/RPC `actor_spawn(spec_json, &id)`
replaces the two host methods. Spawn returns the id synchronously (the one
deliberate case where a primitive function beats a message).

### R-D26 — Tools mirror actor kinds (+ queue)

Three model-facing tools: `process` (spawn/manage process actors),
`agent` (spawn/manage agent actors), `queue` (ordered input). `bash` becomes
`process {op: spawn}` (an optional alias, not a separate tool). Runner kinds
(`INTERNAL|PROCESS|MCP`) describe *execution*, distinct from actor kinds.

### R-D27 — Realtime voice is an extension + a process actor; the pipeline is media-agnostic

Codex-class voice adds **no primitive, actor kind, tool, or host method**. The
pipeline gains one field, `AgcMessage.content_type` (opaque to the core), so
audio/transcripts/realtime items ride the same stages; the **voice helper**
(WebRTC + audio devices) is an ordinary **`process` actor**;
`bemItemPromoted`-style promotion is a hook handler; realtime tool calls are
ordinary `tool` calls; barge-in is `CANCEL{scope:turn}`. Two consequences: the
process actor must support a **writable stdin pipe**
(`AGENTC_PROC_PIPE_BIDI`, also required by MCP stdio servers), and the envelope
gains `AGENTC_TRUST_ALLOW_MIC`/`_SPEAKER`. See `P6-VOICE.md`.

### R-D28 — Controller deciders apply multi-effect records via primitives

A "controller" (Codex's guardian/auto-review) is a decider handler on
`tool`/`call` (and optionally `step`) that branches to jev for every tool call
and returns a multi-effect decision record. **The core interprets no action
list**: the *call-level* verdict is which tool-call message the handler returns
(allow = same, narrow = edited, deny = `NULL`, spin-a-subagent = a replacement
`agent op:spawn` call), and every other effect is an ordinary primitive call
(`hook_branch` to a human for approval, `message_send` of CONTROL `SET_MODEL`/
`SET_THINKING`/`SETTLE`, INPUT for queue/steer, `CANCEL` to stop a turn). Three
new CONTROL kinds: `SET_MODEL`, `SET_THINKING`, `SETTLE` (finish the turn, keep
the actor). No new primitive, stage, actor kind, tool, or host method. See
`P6-DECIDERS.md` §2b.

### R-D29 — Settings are grouped by extension (no group object); `section` is renamed `add_prompt`

Extensions declare typed **settings** with `add_setting`; the group is the
extension's own `name` — there is **no** settings-group contribution. Values live
in `config.jsonc` under `settings.<extension>.<key>`; secrets in `auth.jsonc`;
resolution CLI > trusted project > user > declared default; invalid values fall
back to the default (fail-safe). Read/write via `setting_get`/`setting_set` or
core `agentc_settings_*`. Surfaces: `/settings [ext]` (TUI, one group per
extension), RPC `get_settings`/`set_setting`, CLI `--set`/`--list-settings`, hook
`settings_change`, event `AGENTC_EV_SETTINGS_CHANGED`. This is a service over the
JSONC config loader, not a primitive.

Separately, the vague **`section`** contribution is **renamed (breaking, no
alias)**: it contributes **system-prompt text**, so the API is
`add_prompt(&AgcExtPrompt)` with an `anchor` (`ALL` default, or
`PREAMBLE|TOOLS|RULES|ENVIRONMENT`) and a `mode`
(`APPEND` with `position=BEFORE|AFTER`, or `REPLACE` to supersede the anchor's
default). **The preamble is now overridable** (`anchor=PREAMBLE, mode=REPLACE`);
tools/rules are tool-driven (name/desc/`prompt_snippet` + `prompt_guidelines`);
environment is core/app-driven but overridable. Precedence: `--system` >
`REPLACE(ALL)` > per-anchor `REPLACE` > core default; `APPEND` around it. It answers "can
this be used for the system prompt too?" — yes, that *is* its purpose. User input
is the `input` hook stage, not a prompt contribution. See `P6-SETTINGS.md`.

### R-D30 — Status lines cover every region (chrome + per-step events)

The status registry is generalized from one row to **regions** and
**lifetimes**: chrome regions (`ABOVE_COMPOSER`, `BELOW_COMPOSER`,
`STATUS_LEFT`, `STATUS_RIGHT`, `BELOW_STATUS`) render persistently and are
polled version-driven; event regions (`STEP_END`, `TURN_END`) render once at the
step/turn boundary and are appended as transcript **notice** entries. One
contribution: `add_status_line(&AgcExtStatusLine)` with `region`, `lifetime`
(`PERSISTENT|PER_TURN|ONCE`), `priority`, `style`, and a `render` callback that
writes one or more lines (multiple status lines). This is a **service over the
status registry + the step/output boundaries**, not a primitive. See
`P6-STATUS.md`.

### R-D31 — Terminology: **turn** = user-driven cycle, **step** = model cycle

The vocabulary is fixed to match Codex (which calls the user-driven cycle a
"turn") and to give the inner model cycle a clean name:

- **Turn** — one `agentc_agent_submit`: a user input drives the agent to settle
  (`agent_start`…`agent_end`). Codex's `turn`. The **queue/steer/cancel target a
  turn** (`turn` field, `cancel{scope:turn}`, `active_turn_not_steerable`).
- **Step** — one model request/response **plus its tool batch**
  (`step_start`…`step_end`); `1..N` per turn, bounded by `AGENT_CONTINUATION_MAX`.
  A model's multiple tool calls are **one step** (one batch), not several.
- **Message** / **Block** (`text|thinking|tool_call`) / **Tool call** /
  **Tool batch** / **Tool result** / **User input** / **Final output** /
  **Thinking** / **Retry** / **Settle** — as defined in §2.1.

Renames applied: model cycle `turn`→`step` (`step_start/end`, `step_index`,
`AGENTC_EV_STEP_*`, `AGENTC_HEV_STEP_*`, the `step` hook stage); user cycle
`run`→`turn` (`turn` field, `run_seq`→`turn_seq`, `SCOPE_RUN`→`SCOPE_TURN`,
`run_no`→`turn_no`, `queue_run_of`→`queue_turn_of`, `run_settled`→
`turn_settled`; `active_run_not_steerable`→`active_turn_not_steerable`). The
status regions are `STEP_END` (per model step), `TURN_END` (per turn) and
`FINAL_OUTPUT` (final assistant message). See §2.1 and `P6-SIGNALS.md` §5.

## 4. Cross-cutting invariants

1. Exactly one agent type; root is `parent_id == NULL`. One constructor, one
   lifecycle. (R-D1)
2. Provider/model/thinking/transport are inherited by default. (R-D2)
3. Envelopes narrow monotonically; the root envelope is the ceiling. (R-D3, R-D13)
4. Inside an envelope, the default is auto-allow and no prompt. (R-D4)
5. One signal vocabulary; no tool has a private cancel/stdin/notify path. (R-D6)
6. Cancellation is graceful→force, idempotent, scoped, identical at all depths.
   (R-D7)
7. `progress`→`stalled` is advisory; only cancel/deadline terminates. (R-D7)
8. Only the controller steers; children notify upward. (R-D9)
9. Child results return as ordinary tool results. (`P6-SUBAGENTS.md` §7)
10. Append-only ABI; session stays v1. (R-D10, R-D11)
11. Single-threaded; deterministic drain order; no blocking primitive. (R-D12,
    R-D17, R-D22)
12. Every accumulator is bounded; errors are fail-closed. (R-D13)

## 5. Codex parity summary (detail in area files)

| Codex | P6 |
| --- | --- |
| `exec_command{cmd, yield_time_ms}` | spawn process actor; handle if not finished by yield |
| `write_stdin{session_id, chars}` | `input{to, bytes}` (process) |
| `command/exec/terminate` | `CANCEL{scope:actor}` (graceful→force) |
| `command/exec/outputDelta` | `PROGRESS` message (streamed, spooled under cap) |
| `command/exec/resize` | `RESIZE` message on a PTY actor |
| `turn/steer` | enqueue `{turn:0, priority:0}` (or `priority:-1` to preempt now) |
| `activeTurnNotSteerable` | `active_turn_not_steerable{kind}` |
| `turn/interrupt` | `CANCEL{scope:turn}` |
| `thread/queue/*`, `codex queue` | `queue_{add,list,update,delete,reorder,start}` on the agent actor, keyed by `run`/`priority` |
| `spawn_agent`/`wait_agent`/`send_message`/`interrupt_agent` | `agent op:spawn` / `op:wait` / `op:send` / `op:interrupt` |
| approvals + sandbox | envelope + `tool_approval` hook + `os_sandbox_apply` |
| guardian / auto-review | controller decider on `tool`/`step` (R-D28) |
| realtime voice (`thread/realtime/*`, voice helper) | extension + process helper + `content_type` (R-D27) |

## 6. Phasing

Each wave: implement → full gates → independent verify → commit. A wave must not
start before its predecessors' gates are green.

- **P6-A — Actors & processes.** `AgcActor` registry, process actors, the
  `process {op:spawn}` (bash) tool, stdin, output ring/spill, `os_pty_*`,
  graceful→force cancel, stall, freeze. Spec: `P6-ACTORS.md`.
- **P6-B — Messages & pipeline.** The message bus, notify, lifecycle events,
  mailbox, queue, steer, turn vs actor cancel, the hook pipeline (ten stages).
  Specs: `P6-SIGNALS.md`, `P6-HOOKS.md`.
- **P6-C — Trust & approvals.** `AgcTrustEnv`, RO/RW + boundary enforcement,
  `os_sandbox_apply` per platform, `tool_approval` contract, program/model
  delegation, policy, audit. Spec: `P6-TRUST.md`.
- **P6-D — Sub-agents & deciders.** Unified constructor, agent tree, spawn tools,
  direct model inheritance; decision points and controller records.
  Specs: `P6-SUBAGENTS.md`, `P6-DECIDERS.md`.
- **P6-E — Extensions & integration.** Events/hooks/ABI/RPC/TUI/config/session
  wiring, settings, voice, goldens, docs fold-in.
  Specs: `P6-INTEGRATION.md`, `P6-SETTINGS.md`, `P6-VOICE.md`.

Dependencies: B needs A and the agent loop; C needs the message bus for approvals
and A for sandboxed spawns; D needs A/B for lifecycle and C for envelopes; E is
continuous but lands last.

## 7. Gates (every wave)

- `make check` green (golden suite, offline).
- `./tests/ext.sh`, `./tests/mcp.sh`, `./tests/e2e.sh` green.
- `make windows` builds; `nix develop -c make wine-check` (30/30) and
  `make dyn-check` stay green.
- New golden tests for every parser/formatter/state machine touched.
- No new threads; no libc; no edits to existing header fields/signatures.
- The design docs (`design/20-core-agent.md` §4, `design/30-extensibility.md`
  hook list, `design/40-tui.md`) are updated in the same change that lands the
  behavior.

## 8. Handoff prompt (for the first implementing agent)

> Work in `/home/pvl/spaces/abird/src/agentc`.
>
> Read, in order: `.agents/AGENTS.md`; `.agents/plans/P6-RUNTIME.md` (this file —
> the umbrella and decision log R-D1…R-D31); then `P6-PRIMITIVES.md` (the minimal
> primitive core and async contract), then `P6-SURFACE.md` (the complete surface
> registry), then the area spec for the wave you were assigned; then the
> current-state contracts `.agents/design/00-architecture.md`,
> `20-core-agent.md` (§4 tools, §2 loop), `30-extensibility.md` (hooks/ABI),
> `40-tui.md`; and the code: `include/agent.h`, `include/plat.h`,
> `include/agentc_ext.h`, `src/core/agent.c`, `src/core/tools/jobs.{c,h}`,
> `src/core/tools/bash.c`, `src/core/events.{c,h}`, `src/app/mode.c`,
> `src/ext/registry.c`, `src/core/session.c`, `include/config.h`.
>
> **Check the tree is clean first.** If unrelated WIP is present, stop and ask
> the chair to freeze/commit/stash it; do not build on it.
>
> Implement **P6-A** first (actors & processes). Acceptance for P6-A is in
> `P6-ACTORS.md` §12. Do not start P6-B/C/D until P6-A's gates are green. Commit
> only when asked; keep every gate in §7 green; append to headers, never edit.

## 9. Open questions (resolve before the dependent wave)

1. **PTY on Windows.** ConPTY exists in `os_spawn`; confirm whether a background
   process can be PTY-backed on Windows in P6-A or is deferred to P6-E.
   (`P6-ACTORS.md` §9.)
2. **Sandbox backend per platform.** Landlock vs bwrap on Linux; seatbelt on
   macOS; restricted token vs AppContainer on Windows. (`P6-TRUST.md` §8.)
3. **Queue persistence granularity.** Whole queue restored on resume vs
   re-validated per item. Default: snapshot per op; restore, re-validate, drop
   items that no longer pass the envelope. (`P6-SIGNALS.md` §5.)
4. **Review-step steerability.** `review` is not implemented; when it is, it is
   non-steerable like compaction. Record the `kind` now.
5. **Sub-agent transcript persistence.** Full child transcript vs task+result+
   summarized trace. Default: full under `custom` records unless
   `flags.ephemeral`. (`P6-SUBAGENTS.md` §8.)
6. **Process bidi pipe.** Confirm `AGENTC_PROC_PIPE_BIDI` is the right form for
   MCP stdio and the voice helper (`P6-VOICE.md` §5).

## 10. What P6 is not

- Not an app-server. No daemon, no remote client, no `command/*`/`process/*`
  split. `plans/P5-AXES.md` covers a framed protocol if that is ever wanted.
- Not a code-mode/JS runtime. Tools stay flat. (R-D14)
- Not a per-feature tool/RPC set. Exactly three op-based tools and the op-based
  RPC additions ship; new capabilities are new message kinds or hook stages, not
  new names. (R-D26)
- Not a rewrite of the agent loop. `agentc_agent_submit` keeps its nested loop;
  P6 adds actor registration, message drain points, and envelope checks around it.
- Not a new session format. (R-D11)
