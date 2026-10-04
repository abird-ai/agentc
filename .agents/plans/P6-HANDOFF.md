# P6-HANDOFF — Complete context handoff for the P6 runtime design

Status: **as-built (P6-A..E + completion passes 2a-2g).** The context handoff and decision index.
As-built note: the outcome is folded into `.agents/design/00-architecture.md`; the decision index is
`.agents/runs/p6-runtime-council/CHAIR.md`.

> **Note on location:** the repository convention is `.agents/design/` for
> *current, implemented* contracts and `.agents/plans/` for *forward-looking*
> work. P6 is forward-looking, so all P6 files live in `.agents/plans/`
> (there is no `.agents/docs/`). When P6 lands, its outcomes fold into
> `.agents/design/`.

---

## 0. TL;DR

P6 is a large, fully-specified redesign of the `agentc` runtime: background
processes, sub-agents, trust/sandbox, approvals, steering, freeze, realtime
voice, extension settings and status lines — expressed as **four primitives +
two hook verbs**, with everything else derived. It is Codex-0.159.3-parity by
*behavior*, not by structure. The design is complete and audited. **P6 is
as-built and green** through waves **A–E** plus the completion passes 2a–2g;
this file records the original design journey and the decision index. For the
whole arc (P6 + P5-AXES + review), start at `.agents/plans/HANDOFF-AGENTC.md`.

---

## 1. Read order (onboarding)

1. **`P6-HANDOFF.md`** (this file) — the journey, decisions index, pending items.
2. **`P6-RUNTIME.md`** — umbrella: goals, the four primitives, glossary,
   terminology (§2.1), **decision log R-D1…R-D31**, phasing, gates, open
   questions. Authoritative for decisions.
3. **`P6-PRIMITIVES.md`** — the minimal core + the async contract.
4. **`P6-SURFACE.md`** — the exhaustive surface registry (every API/event/hook/
   tool/RPC/flag/key/limit/error) + the coverage audit.
5. **`P6-API.md`** — the API in one page (layered).
6. **`P6-EXTENDING.md`** — how to build each integration kind (extension/tool/
   MCP/governance/access/hook).
7. The area spec for the wave you will implement:
   `P6-ACTORS.md` (P6-A), `P6-SIGNALS.md` + `P6-HOOKS.md` (P6-B),
   `P6-TRUST.md` (P6-C), `P6-SUBAGENTS.md` + `P6-DECIDERS.md` (P6-D),
   `P6-INTEGRATION.md` + `P6-SETTINGS.md` + `P6-STATUS.md` + `P6-VOICE.md` (P6-E).
8. Then the current-state contracts in `.agents/design/` and the code:
   `include/agent.h`, `include/plat.h`, `include/agentc_ext.h`,
   `src/core/agent.c`, `src/core/tools/jobs.{c,h}`, `src/core/tools/bash.c`,
   `src/core/events.{c,h}`, `src/app/mode.c`, `src/ext/registry.c`,
   `src/core/session.c`, `src/core/prompt.c`, `include/status.h`,
   `src/core/status.c`, `include/config.h`.

Reference (do not build on): Codex `plans/P5-AXES.md`, `P5-TOOL-ENGINE.md`; the
existing `.agents/plans/HANDOFF.md` and `INLINE-RESIZE-DESIGN.md` are unrelated
prior work.

---

## 2. The journey (every step, in order)

The design was produced iteratively. This timeline records *what was asked,
what was decided, and where it landed*. Decision IDs are R-D1…R-D31 (full text in
`P6-RUNTIME.md` §3); area-local decisions are S-D1…S-D6 (`P6-SIGNALS.md` §0).

**Step 1 — Compare agentc to Codex tool calls; background terminals.**
Question: how does Codex track background terminals (start/­poll/­feed/­kill), and
how should agentc structure that? Outcome: a persistent **process actor** with a
stable id, stdin write, streamed output, and graceful→force cancellation;
`bash` becomes a background/foreground process. → `P6-ACTORS.md`.

**Step 2 — Trust, sandbox, approval, security.**
Question: per-agent/sub-agent trust boundary; read-only vs rw; default = trust the
model; opt-in fine-grained approval delegable to other programs/models (e.g.
`jev`). Outcome: the **envelope** (`access`, `boundary`, `approval`, `network`,
roots, flags), monotonic narrowing, a delegable approval contract, sandbox
enforcement per platform. → **R-D3, R-D4, R-D5**; `P6-TRUST.md`.

**Step 3 — Sub-agent model is direct; one agent type.**
Question: children use the same model as the parent; agent startup is the same
code path as a sub-agent. Outcome: **one agent type, one constructor**, direct
model/provider/transport inheritance. → **R-D1, R-D2**; `P6-SUBAGENTS.md`.

**Step 4 — Notify + lifecycle + freeze/resume.**
Question: child↔parent signalling (started/finished/stalled/needs-cancel/
graceful/force); freeze an in-flight process at its current state and resume
later. Outcome: a **signal/message bus** with lifecycle kinds; **freeze/resume**
with timer suspension. → **R-D6, R-D7, R-D18**; `P6-SIGNALS.md`, `P6-ACTORS.md`
§6b.

**Step 5 — One queue for queue/steer/interrupt.**
Question: replace three delivery modes with a single turn+priority queue
(nice-like; `<0` immediate, `0` after the current batch). Outcome: one
**turn-targeted, priority-ordered queue**. → **R-D8**; `P6-SIGNALS.md` §5
(local S-D1…S-D6).

**Step 6 — Write the plans.**
Outcome: the initial area docs `P6-RUNTIME`, `P6-ACTORS`, `P6-SIGNALS`,
`P6-TRUST`, `P6-SUBAGENTS`, `P6-INTEGRATION`.

**Step 7 — Surface audit; minimal primitives.**
Question: cut the primitive count and the "disposition" vocabulary. Outcome:
**four primitives** (Runtime, Actor, Message, Envelope) + **two hook verbs**
(return the next message, or defer); the queue/signals/hooks become services.
→ **R-D19, R-D21, R-D22**; `P6-PRIMITIVES.md`, `P6-SURFACE.md`.

**Step 8 — Turn hook; universal tool path; decisions as handlers; internal and
MCP tools through hooks.**
Question: add a per-turn seam; make internal/external/MCP tools uniform through
the pipeline (internal may opt out). Outcome: the **10 hook stages**, one tool
runner (`INTERNAL|PROCESS|MCP`), decisions as handlers. → **R-D23**;
`P6-HOOKS.md`.

**Step 9 — Deciders, router, jev, controller decisions.**
Question: each tool call goes through jev, which decides the next model / asks
for approval / is done / spins a subagent. Outcome: **decisions are hook
handlers**; the classified router and jev are backends; a controller returns a
multi-effect record applied via primitives (`hook_branch`, `SET_MODEL`,
`SETTLE`, `CANCEL`). → **R-D20, R-D28**; `P6-DECIDERS.md` §2b.

**Step 10 — Realtime voice.**
Question: map Codex's voice architecture (realtime session + voice helper) onto
agentc. Outcome: voice is an **extension + a process-actor voice helper**; the
pipeline gains one opaque `content_type`; items promote via an `input` hook;
tool calls are uniform; barge-in is `CANCEL`. → **R-D27**; `P6-VOICE.md`.

**Step 11 — Settings and prompt contributions.**
Question: settings/settings groups and what a "section" is. Outcome: settings
grouped by the extension (`add_setting`, no group object); the "section" renamed
to a **prompt contribution** (`add_prompt`) with anchors and `APPEND`/`REPLACE`.
→ **R-D29**; `P6-SETTINGS.md`.

**Step 12 — Status lines in every region.**
Question: multiple status lines around the composer, at the end of every turn,
and after the final output. Outcome: status **regions + lifetimes**
(`STEP_END`/`TURN_END`/`FINAL_OUTPUT` + chrome), one contribution
(`add_status_line`, multi-line). → **R-D30**; `P6-STATUS.md`.

**Step 13 — Actor taxonomy; one factory; tools mirror kinds.**
Question: is `terminal` just a process/tool? Outcome: actor kinds are
**`agent | process`** (terminal = a `tty` flag); **one `actor_spawn`**; three tools
(`process`/`agent`/`queue`) mirroring the kinds. → **R-D24, R-D25, R-D26**.

**Step 14 — Terminology.**
Question: is Codex's "turn" the user turn? Outcome: **turn = user-driven cycle,
step = model cycle**; a model's multiple tool calls are **one step** (one batch).
→ **R-D31**; `P6-RUNTIME.md` §2.1.

**Step 15 — API/implementer guides + final audit.**
Outcome: `P6-API.md`, `P6-EXTENDING.md`; the final audit collapsed the last
redundancy (`AgcSignal`→`AgcMessage`, one message type) and cleaned stale claims.

---

## 3. Final design at a glance

**Four primitives + two hook verbs (the only core):**

```text
Runtime   — scheduler/reactor + continuations (fds, clock, drain)
Actor     — addressable unit of work: `agent` (model loop) | `process` (OS process)
Message   — the one communication unit: CONTROL | INPUT | NOTIFY | HOOK
Envelope  — the trust/sandbox boundary
Hook      — ordered transform chain, two verbs: return the next message | defer
```

`runtime_step` is the sole driver; no primitive blocks; all fds are one poll set.

**Derived services:** queue (INPUT subset of an inbox; `turn` + signed
`priority`), actor control, agent tree, policy/approval/decisions, settings,
status, prompt contributions, tools, RPC, host ABI, events, session.

**Tools (3):** `process`, `agent`, `queue` (op-based). `bash` = `process op:spawn`.

**Hook stages (10):** `turn, step, input, prompt, message, output, tool, signal,
event, lifecycle`.

**Extension registrations (8):** `add_tool, add_command, add_prompt,
add_status_line, add_provider, add_setting, on(point,…), hook(stage,…)`.

The full surface (functions, events, config, limits, error codes) is in
`P6-SURFACE.md`; the one-page API is `P6-API.md`; the per-persona recipes are
`P6-EXTENDING.md`.

---

## 4. Decision log index (R-D1…R-D31)

Full text: `P6-RUNTIME.md` §3. One line each:

- **R-D1** one agent type, one startup path.
- **R-D2** sub-agent model direct + inherited.
- **R-D3** trust declared per agent/sub-agent, inherited by default.
- **R-D4** two access modes (RO/RW); default RW, trust the model inside the boundary.
- **R-D5** fine-grained approval opt-in and delegable (program/model).
- **R-D6** actors, not parallel subsystems.
- **R-D7** one cancellation model: graceful→force, idempotent, scoped.
- **R-D8** one turn-targeted, priority-ordered queue.
- **R-D9** only the controller steers a turn; children notify upward.
- **R-D10** append-only interfaces.
- **R-D11** session format stays v1 (new state via `custom` records).
- **R-D12** single-threaded, pump-driven, deterministic.
- **R-D13** bounded and fail-closed.
- **R-D14** no downloads, no daemon, no JS runtime.
- **R-D15** Codex is the parity reference for behavior, not structure.
- **R-D16** two event layers: content and control.
- **R-D17** one runtime lifecycle; no blocking wait may skip the bus.
- **R-D18** freeze/resume is a first-class actor operation.
- **R-D19** five primitives; signal kind = opcode; nothing blocking. *(superseded in part)*
- **R-D20** decisions are a seam, not a proxy; deciders/routers are backends.
- **R-D21** one universal hook pipeline. *(superseded in part)*
- **R-D22** four primitives; hooks have two verbs; everything else derived.
- **R-D23** one universal path for turn/step, tools and decisions.
- **R-D24** actor kinds `agent | process`; `terminal` is a flavor.
- **R-D25** one actor factory.
- **R-D26** tools mirror actor kinds (+ queue).
- **R-D27** realtime voice is an extension + a process actor; media-agnostic pipeline.
- **R-D28** controller deciders apply multi-effect records via primitives.
- **R-D29** settings grouped by extension; `section` renamed `add_prompt`.
- **R-D30** status lines cover every region (chrome + per-turn events).
- **R-D31** terminology: turn = user cycle, step = model cycle.

Area-local: **S-D1…S-D6** (`P6-SIGNALS.md` §0) — one queue; signed priority;
`turn`-0 auto; same-turn preempt semantics; flush-whole-due-set; coalescing.

---

## 5. Pending items

**Blocking before implementation starts:**

1. ~~Nothing is committed.~~ **Resolved:** the 15 P6 docs were committed in
   `5cbb74e`, and P6-A in `3242dab` (+ follow-ups). The tree is clean.
2. Keep the tree clean; do not build on unrelated WIP.

**Open questions (from `P6-RUNTIME.md` §9):**

1. PTY on Windows in P6-A vs P6-E.
2. Sandbox backend per platform (Landlock/bwrap; seatbelt; restricted token).
3. Queue persistence granularity (default: snapshot, restore + re-validate).
4. Review-step steerability kind (reserve `review`).
5. Sub-agent transcript persistence (default: full unless `ephemeral`).
6. `AGENTC_PROC_PIPE_BIDI` correctness for MCP stdio + the voice helper.

**Optional further collapse (not applied):** drop the Tier-2 ergonomic wrappers
(`process_*`, `actor_freeze/resume`, `agent_wait/send/cancel`) and use
`message_send` + `actor_query` directly. Recommended to keep.

**Design risks to watch:** the anchor model for `add_prompt` requires a small
`prompt_core` change (5 anchor boundaries); the status registry change is
breaking; the turn/step rename is breaking. All are recorded as decisions.

---

## 6. Document map

| file | role |
| --- | --- |
| `P6-HANDOFF.md` | this file — journey, decisions index, pending, starting prompt |
| `P6-RUNTIME.md` | umbrella + decision log R-D1…R-D31 + terminology §2.1 + phasing/gates/open questions |
| `P6-PRIMITIVES.md` | four primitives, the async contract, the derivation map |
| `P6-SURFACE.md` | exhaustive surface registry + coverage audit |
| `P6-API.md` | the API in one page, layered |
| `P6-EXTENDING.md` | how to build extension/tool/MCP/governance/access/hook |
| `P6-ACTORS.md` | actors, processes/background shells, PTY, stdin, output, cancel, freeze, stall (P6-A) |
| `P6-SIGNALS.md` | message bus, unified queue, notify, steer, interrupt (P6-B) |
| `P6-HOOKS.md` | universal interception pipeline, 10 stages, two verbs (P6-B) |
| `P6-TRUST.md` | envelopes, sandbox, approvals, delegation, audit (P6-C) |
| `P6-SUBAGENTS.md` | one agent type, tree, direct model, spawn (P6-D) |
| `P6-DECIDERS.md` | decisions as hooks, router, jev, controller (P6-D) |
| `P6-INTEGRATION.md` | header/event/hook/ABI inventory, file map, test matrix, handoff (P6-E) |
| `P6-SETTINGS.md` | settings grouped by extension + prompt contributions (P6-E) |
| `P6-STATUS.md` | status lines in every region (P6-E) |
| `P6-VOICE.md` | realtime voice as an extension + process helper (P6-E) |

Supporting (current-state, in `.agents/design/`): `00-architecture.md`,
`10-platform.md`, `20-core-agent.md`, `30-extensibility.md`, `40-tui.md`,
`50-build-and-test.md`.

---

## 7. Repository reality (what exists today)

- `agentc` is freestanding C23, static, no libc, single-threaded; one flat tool
  list; one bounded job batch (`src/core/tools/jobs.c`); one agent per process.
- Existing ABI to extend: `include/agentc_ext.h` (`AgcExtHost`, `AgcExt`,
  `AgcExtTool`, `AgcExtCommand`, `AgcExtSection`→to be renamed `AgcExtPrompt`,
  `AgcExtStatusProvider`/`Segment`→to be replaced by `AgcExtStatusLine`/`Ctx`,
  `AgcExtProvider`, hooks, `AgcExtResult`), plus `include/agent.h`,
  `include/plat.h`, `include/config.h`, `include/session.h`, `include/status.h`.
- **P6-A exists.** `include/actor.h`, `src/core/actor.c`, `src/core/process.c`,
  `os_pty_*`/freeze in the plat backends, the `process` tool, and the
  actor-backed `bash` are built and green (`tests/actors_test.c`). P6-B…P6-E are
  still design/plan only.

---

## 8. Rules for the next agent (non-negotiable)

- Append-only by default: new fields at the end of structs, events after
  `AGENTC_EV_MSG_RESET`, host functions at the end of `AgcExtHost` gated by
  `AGENTC_EXT_HOST_HAS`. The only sanctioned *breaking* changes are the renames
  recorded in R-D24…R-D31 (terminal→process; section→prompt; provider/segment→
  status_line; signal→message; turn/step; run→turn).
- Freestanding: only `<stddef.h> <stdint.h> <stdbool.h> <stdarg.h>`; no libc; no
  OS headers in core; errors are negative linux errno.
- Single-threaded; `runtime_step` is the only driver; no primitive blocks; every
  blocking wait pumps `step` (R-D17).
- If a capability cannot be expressed as a **message kind, hook stage, envelope
  decision, tool runner, or host service**, stop and record a new decision —
  never add a parallel mechanism.
- Keep the gates green: `make check`, `./tests/ext.sh`, `./tests/mcp.sh`,
  `./tests/e2e.sh`, `make windows`, `wine-check` (30/30), `dyn-check`. Bash
  goldens must pass **unedited**.
- Commit only when asked; no unrelated WIP.

---

## 9. Implementation waves (summary)

- **P6-A — Actors & processes.** Actor registry, process actors, `process {op:spawn}`
  (bash), stdin, output ring/spill, `os_pty_*`, graceful→force cancel, stall,
  freeze. Acceptance: `P6-ACTORS.md` §12.
- **P6-B — Messages & pipeline.** Message bus, notify, queue, steer,
  turn/step cancel, the hook pipeline (10 stages). Acceptance: `P6-SIGNALS.md`
  §14 / `P6-HOOKS.md`.
- **P6-C — Trust & approvals.** Envelope, RO/RW + boundary enforcement,
  `os_sandbox_apply`, `tool_approval`, policy, delegation, audit. Acceptance:
  `P6-TRUST.md` §14.
- **P6-D — Sub-agents & deciders.** Unified constructor, tree, spawn tools,
  direct model; decision points + controller. Acceptance: `P6-SUBAGENTS.md` §13.
- **P6-E — Extensions & integration.** Events/hooks/ABI/RPC/TUI/config/session,
  settings, status, voice, goldens, docs fold-in. Acceptance: `P6-INTEGRATION.md`
  - each P6-E doc.

Each wave: implement → full gates → independent verify → commit. Do not start a
wave before its predecessors' gates are green. Full file map and test matrix:
`P6-INTEGRATION.md` §6/§7.

---

## 10. Starting prompt (paste this to the next agent)

> **Historical record.** P6-A…P6-E have all since landed and are committed; the
> text below is kept as the P6-A kickoff prompt, not a current work order.
>
> Work in `/home/pvl/spaces/abird/src/agentc`.
>
> You are picking up the **P6 runtime design** for `agentc`. It is fully
> designed; **P6-A (actors & processes) is implemented and committed** at
> `3242dab`, while waves B–E are still design-only. Your first job is to get
> full context, then implement the next wave.
>
> **Step 1 — read, in order:** `.agents/AGENTS.md`;
> `.agents/plans/P6-HANDOFF.md` (this context);
> `.agents/plans/P6-RUNTIME.md` (umbrella +decision log R-D1…R-D31 + terminology
> §2.1); `.agents/plans/P6-PRIMITIVES.md`; `.agents/plans/P6-SURFACE.md`;
> `.agents/plans/P6-API.md`; `.agents/plans/P6-EXTENDING.md`; then the area spec
> for the wave you will do (`P6-ACTORS.md` for P6-A, plus `P6-SIGNALS.md` /
> `P6-HOOKS.md`, `P6-TRUST.md`, `P6-SUBAGENTS.md` / `P6-DECIDERS.md`,
> `P6-INTEGRATION.md` / `P6-SETTINGS.md` / `P6-STATUS.md` / `P6-VOICE.md`).
> Then the current-state contracts `.agents/design/00..50` and the code
> (`include/agent.h`, `include/plat.h`, `include/agentc_ext.h`,
> `src/core/agent.c`, `src/core/tools/jobs.{c,h}`, `src/core/tools/bash.c`,
> `src/core/events.{c,h}`, `src/app/mode.c`, `src/ext/registry.c`,
> `src/core/session.c`, `src/core/prompt.c`, `include/status.h`,
> `src/core/status.c`, `include/config.h`).
>
> **Step 2 — before any code:** confirm the tree is clean. (The P6 docs are
> already committed, e.g. `5cbb74e`, so only unrelated WIP should be present; if
> it is, stop and ask the chair to freeze/commit/stash it.)
>
> **Step 3 — implement P6-A first** (actors & processes), following
> `.agents/plans/P6-ACTORS.md` and its acceptance §12: the `AgcActor` registry,
> process actors, the `process {op:spawn}` tool (bash), stdin, output ring/spill,
> `os_pty_*`, graceful→force cancellation, stall, and freeze. Keep `bash`'s
> foreground output byte-for-byte identical (existing goldens must pass
> unedited).
>
> **Rules:** append-only ABI (the only breaking changes are the recorded renames
> R-D24…R-D31); freestanding C23 (no libc, no OS headers in core); negative
> errno; single-threaded with `runtime_step` the only driver and no blocking
> primitive; bounded + fail-closed; if something cannot be a message kind, hook
> stage, envelope decision, tool runner or host service, stop and record a new
> decision instead of adding a parallel mechanism. Keep every gate in
> `P6-RUNTIME.md` §7 green. Commit only when asked.
>
> **First deliverable:** P6-A implemented and green, with the new golden suite
> from `P6-ACTORS.md` §12, and the same-commit updates to
> `.agents/design/20-core-agent.md` §4. Report what you changed and the gate
> results before moving on.

---

## 11. Changelog of the design (what changed and why)

- Started from a Codex comparison; adopted persistent process actors, one
  cancellation model, and a message bus.
- Collapsed the primitive set three times (Signal+Queue+Pipeline → Message +
  service hook; six → four primitives; eight dispositions → two hook verbs).
- Unified every tool path (internal/external/MCP) through one `tool` stage.
- Added: decisions as handlers (router/jev/controller), voice, settings, prompt
  contributions, status regions.
- Renamed for clarity at the end: terminal→process, section→prompt,
  provider/segment→status_line, signal→message, and turn/step/run terminology.
- Audited twice; the final audit collapsed `AgcSignal` into `AgcMessage` so there
  is exactly one message type.
