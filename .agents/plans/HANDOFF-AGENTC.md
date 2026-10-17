# HANDOFF — agentc: P5-AXES + P6 runtime (master context handoff)

Status: **complete and gate-green**, except the explicitly deferred platform
sandboxes. This is the single entry point for a new agent. Read it first, then
follow §3's read order. It covers the whole arc: the P5 tool-engine work, the P6
runtime (design → implementation → completion), the P5-AXES protocol/SDK work,
the independent review and its resolutions, and the release.

Owner of record: the chair (this file is the durable handoff). Every decision,
plan and run log it points at is committed under `.agents/`.

---

## 0. TL;DR

`agentc` is a freestanding C23 coding agent (no libc, static, single-threaded).
On top of the pre-existing tool-engine work (`P5-TOOL-ENGINE`), two large plans
were **designed and then implemented to completion**:

- **P6 — the runtime**: background processes, a message bus + priority queue,
  steering/cancel, a universal 10-stage hook pipeline, trust/approvals/sandbox
  envelope, sub-agents/deciders, extension settings, status lines, realtime
  voice. Expressed as **four primitives** (Runtime, Actor, Message, Envelope) +
  **two hook verbs** (return the next message, or defer). Codex-parity by
  behavior, not structure.
- **P5-AXES — the control surfaces**: **RPC v1** (`--mode rpc`), the embeddable
  **`libagentc` SDK** (incl. the strict no-`app/` archive), and the **framed CBOR
  protocol** (`agentc serve`/`agentc-client`, unix/TCP/WebSocket/TLS transports,
  `ui` dialogs, attach/detach, multi-session with cooperative interleaving).

All gates are green: `make check`, `ext.sh`, `mcp.sh`, `e2e.sh`, `serve.sh`,
`make windows`, `make libcheck`, `make libstrict`, wine-check **47/47**,
`dyn-check`, `make settings-e2e`, plus lint gates. The project is at release
**v0.8.0**. `--mode rpc` JSONL and every pre-existing golden are byte-identical.

Only one thing is explicitly deferred and out of scope: the **platform sandbox
backends** (Windows restricted-token, macOS Seatbelt, Linux host allowlist —
all fail-closed). Everything else remaining is either an environment limitation
(no macOS SDK here) or a documented, fail-closed parity choice.

---

## 1. What `agentc` is

A freestanding C23 coding agent: **no libc**, static binaries, **single-threaded**
and pump-driven. Layers: `plat.h` (Layer 0, OS), `net.h` (Layer 1), `wire.h`
(HTTP/SSE), then core. Memory via `agentc_alloc/realloc/free`; errors are
negative linux errno; a small set of public headers is the whole contract.
It is inspired by [pi](https://github.com/earendil-works/pi) (MIT); the design
record notes where each idea is adopted and where `agentc` diverges.

The working agreement (hard rules, layout, gates) is **`.agents/AGENTS.md`** —
read it before touching code. The index of everything agent-facing is
**`.agents/README.md`**.

---

## 2. Rules that must never be violated

1. **Freestanding**: only `<stddef.h> <stdint.h> <stdbool.h> <stdarg.h>`; no
   libc, no OS headers in core (`src/core`, `src/base`). OS code lives in
   `src/plat/<os>` and `src/net/<os>`.
2. **Append-only interfaces** by default: new struct fields at the end, new
   events after the current enum tail, new host functions at the end of
   `AgcExtHost` gated by `AGENTC_EXT_HOST_HAS`. The only sanctioned breaking
   changes are the recorded renames **R-D24…R-D31**.
3. **Negative linux errno**; `0`/positive means success.
4. **Single-threaded**; the pump is the only driver; **no blocking primitive**;
   every blocking wait must pump the runtime (R-D17).
5. **Bounded and fail-closed**: every accumulator has a limit; a missing/
   malformed approver or sandbox failure is a denial, never an allow.
6. **No stdout/stderr** except through `agentc_out*`/`agentc_log*`.
7. **Golden tests** for every parser/formatter/state machine; goldens are
   byte-exact.
8. If a capability cannot be a **message kind, hook stage, envelope decision,
   tool runner, or host service**, stop and record a decision — never add a
   parallel mechanism.
9. Keep every gate green (§7). Commit with `git commit` (signed).

---

## 3. Read order (onboarding)

1. **This file** — the whole arc, decisions index, pending items, starting prompt.
2. **`.agents/AGENTS.md`** — working agreement, hard rules, layout, gates.
3. **`.agents/README.md`** — the index of `design/`, `plans/`, `runs/`.
4. **`.agents/design/00-architecture.md`** — principles, component map, constraints.
5. The P6 umbrella **`.agents/plans/P6-RUNTIME.md`** (four primitives, glossary,
   **decision log R-D1…R-D31**, phasing, gates) and its read-order
   `.agents/plans/P6-HANDOFF.md`.
6. **`.agents/plans/P5-AXES.md`** — the RPC/SDK/protocol plan (Axis A/B/C, frozen
   decisions A-D/B-D/C-D).
7. The current-state contracts `.agents/design/10-platform.md`,
   `20-core-agent.md`, `30-extensibility.md`, `40-tui.md`, `50-build-and-test.md`,
   and the three new ones `60-rpc.md`, `61-protocol.md`, `70-sdk.md`.
8. The council records under `.agents/runs/` (see §9): the P6 council
   (`p6-runtime-council/` — CHAIR/STATUS/abi/audit + per-area plans) and the P5
   council (`p5-axes-council/`) and the fresh review (`.agents/runs/final-review.md`).

---

## 4. The full journey (chronological, from the beginning)

### Phase 0 — P5-TOOL-ENGINE (pre-existing, as-built)
External `rg`/`fd`/POSIX `grep`/`find` vs built-in implementations, selected
once at startup through `src/core/tools/engine.{c,h}` and invisible to the
model. Decisions **T-D1…T-D12** live in `.agents/plans/P5-TOOL-ENGINE.md`; the
as-built behavior is `design/20-core-agent.md` §4.5.

### Phase 1 — P6 design (15 iterative steps; decisions R-D1…R-D31)
The design was produced by an iterative dialogue; each step is a decision:

1. **Codex comparison; background terminals** → a persistent **process actor**
   (stable id, stdin, streamed output, graceful→force cancel); `bash` becomes a
   background/foreground process. → `P6-ACTORS.md`.
2. **Trust/sandbox/approval/security** → the **envelope** (access, boundary,
   approval, network, roots, flags), monotonic narrowing, delegable approval.
   → **R-D3, R-D4, R-D5**; `P6-TRUST.md`.
3. **Sub-agents: one type, one constructor, direct model** → **R-D1, R-D2**;
   `P6-SUBAGENTS.md`.
4. **Notify + lifecycle + freeze/resume** → signal/message bus; freeze suspends
   timers. → **R-D6, R-D7, R-D18**; `P6-SIGNALS.md`, `P6-ACTORS.md` §6b.
5. **One queue for queue/steer/interrupt** → a turn-targeted, signed-priority
   queue. → **R-D8**; `P6-SIGNALS.md` §5 (S-D1…S-D6).
6. **Write the plans** (the initial area docs).
7. **Surface audit; minimal primitives** → **four primitives + two hook verbs**.
   → **R-D19, R-D21, R-D22**; `P6-PRIMITIVES.md`, `P6-SURFACE.md`.
8. **Turn/step hook; universal tool path; decisions as handlers** → the **ten
   hook stages**, one tool runner. → **R-D23**; `P6-HOOKS.md`.
9. **Deciders/router/jev; controller decisions** → decisions are hook handlers.
   → **R-D20, R-D28**; `P6-DECIDERS.md` §2b.
10. **Realtime voice** → an extension + a process actor; media-agnostic pipeline
    (`content_type`). → **R-D27**; `P6-VOICE.md`.
11. **Settings + prompt contributions** → settings grouped by extension; the
    vague "section" renamed to **`add_prompt`**. → **R-D29**; `P6-SETTINGS.md`.
12. **Status lines in every region** → regions + lifetimes. → **R-D30**;
    `P6-STATUS.md`.
13. **Actor taxonomy; one factory; tools mirror kinds** → kinds `agent|process`
    (`terminal` = a `tty` flag), one `actor_spawn`, three op-based tools
    (`process`/`agent`/`queue`). → **R-D24, R-D25, R-D26**.
14. **Terminology** → **turn** = user cycle, **step** = model cycle. → **R-D31**.
15. **API/implementer guides + final audit** → `P6-API.md`, `P6-EXTENDING.md`.

### Phase 2 — P6 implementation (waves A–E)
Each wave: implement → full gates → independent verify → commit.

- **P6-A — actors & processes** (`include/actor.h`, `src/core/actor.c`,
  `src/core/process.c`): bounded actor registry, pipe+PTY process actors,
  `os_pty_*`, stdin, output ring + spill, graceful→force cancel, stall, freeze;
  `bash` runs through the actor; the model-facing `process` tool. Landed in
  `3242dab` (+ follow-ups `9bbfef4`, `ebc581c`, closure `48297e8`).
  Acceptance `P6-ACTORS.md` §12; golden `tests/actors_test.c`.
- **P6-B — messages, queue, hooks**: `include/message.h`, `include/hook.h`,
  `src/core/{signal,queue,hook,runtime}.c`, the 10-stage pipeline, the turn+
  priority queue, steer/preempt, turn/actor cancel, adapters (`queue` tool, RPC),
  app root-actor binding, queue persistence. Landed `5244a81`…`c97bb8b` +
  round-2 `3464061`…`0be6507`.
- **P6-C — trust & approvals**: `include/trust.h`, `src/core/{trust,approval}.c`,
  `os_sandbox_apply` + `*_sandboxed` spawns (Linux Landlock+seccomp; macOS
  Seatbelt code; Windows/others fail-closed), RO/RW at four layers, the
  `tool_approval` contract, `approvals.jsonc`. Landed `b1e4c29`…`5b4ae1f`.
- **P6-D — sub-agents & deciders**: unified `agentc_agent_create`, the agent
  tree, `agent` op tool, direct model inheritance, decisions/controller,
  `hook_branch`, agent persistence. Landed `22ec001`…`7ee6797`.
- **P6-E — extensions/integration**: settings (`add_setting`), prompt
  contributions (`add_prompt`), status regions (`add_status_line`), voice
  contract, TUI chrome, docs fold-in; the R-D29/R-D30 renames. Landed
  `4d75971`…`d252862`.
- **Completion passes 2a–2g** (post-wave polish driven by the review): async
  decision/approval seams, the full B8 legacy-hook fold, the resumable
  one-model-step scheduler (`AGENTC_RUN_YIELD`), mid-stream steer + TUI approval
  overlay, the built-in `actors` status line, TUI `/settings`, M6 tests, the
  `abird/agentc` paths, and the MCP stubborn-shutdown/orphan fix.
  Records: `runs/p6-runtime-council/STATUS.md`, `CHAIR.md`.

### Phase 3 — independent audit of P6
`.agents/runs/p6-runtime-council/audit.md` found and the passes fixed: freeing a
borrowed trust `reason` (allocator corruption on every denial), inert host ABI
stubs, unpumped approval waits, and more. `runs/p6-runtime-council/abi.md` is the
append-only ABI inventory and conflict matrix.

### Phase 4 — the `abird/agentc` user-path rename
User config/data/state moved under a vendor dir: `<XDG>/abird/agentc/...`
(project `.agentc/` unchanged). Applied and committed `4e929dd`; later a
transparent legacy fallback was added (`65f19ff`) so existing users are not
orphaned.

### Phase 5 — P5-AXES design + implementation
The plan `.agents/plans/P5-AXES.md` (written against an older baseline) was
reconciled against the P6 tree by the P5 council
(`.agents/runs/p5-axes-council/CHAIR.md`, `STATUS.md`) and implemented axis by
axis:

- **Axis A — RPC v1** (`5450465`…`22bdc38`): `design/60-rpc.md`, `AGENTC_RPC_VERSION`,
  dual discriminator, `command`+`code` responses, acceptance-first/queued prompts,
  `get_commands`, thinking names, model-cap fix, queue backpressure, UI sink +
  status observer, `rpc_test` + `rpc_check.py`. `--mode rpc` stays byte-identical.
- **Axis B — `libagentc` SDK** (`da83cc9`…`4c0ca5b`, B4 `ad4c244`…`f9de462`):
  boot/memimpl/mbedtls-shim seams, `os_embed_init`, `agentc_fatal_install`, a PIC
  `build/libagentc.a`, `include/agentc_sdk.h`, `src/sdk/sdk.c`, `examples/embed.c`
  + `make libcheck`, `agentc.pc`/`install-lib`/shared lib + version map,
  `design/70-sdk.md`; **B4** split `setup.c`→`core/select.c`, run/session→
  `core/runsession.c`, so the archive excludes all `src/app/` (`make libstrict`).
- **Axis C1 — framed CBOR protocol** (`80c66b1`…`2d2834b`): frame decoder, CBOR
  DOM + strict UTF-8 + JSON bridge, protocol v1 envelopes, unix/tcp transport,
  the framing-neutral RPC dispatcher, `agentc serve`, synchronous client +
  `proto_client.py` + `serve.sh`.
- **Axis C2 — hardening** (`76c9609`…`eec64e8`): cancel wired across serve,
  multi-client serialization + connection cap, `get_messages` paging, CBOR/frame
  fuzz, Windows TCP fallback under Wine.
- **UI dialogs** (`a45d499`…`167e2a0`): `ui_request`/`ui_response`
  (select|confirm|input|editor), a non-blocking parked-dialog pump, the `ui`
  feature, ABI prompt sink, TUI modal, client subcommand.
- **Axis C3** (`94512ef`…`bef0619`): `--host/--remote` TCP bind, RFC 6455
  WebSocket, vendored-mbedTLS server TLS, `attach`/`detach` routing + event
  filter, explicit reconnect + resync.
- **Multi-session** (`0904327`…`3886915`) and **cooperative interleaving**
  (`6684e3e`…`7ecd79c`): a bounded 4-session registry; runs advance one model
  step per slice across sessions via the cooperative run API +
  `agentc_mode_activate`.

### Phase 6 — release + docs fold-in
`2522de0` **release v0.8.0** (VERSION + CHANGELOG covering P6 and P5); design
docs `60/61/70` and the P5/P6 as-built sections; `.agents/README.md` indexes P5.

### phase 7 — final review + resolution
`.agents/runs/final-review.md` (fresh adversarial audit) found: a CBOR
trailing-bytes bug, the missing `AGENTC_EV_ACTOR_RESIZED` event/hook, leaked
processes poisoning the serve/e2e gates, no graceful `serve` shutdown, an
untested settings editor, an SDK doc/code mismatch, terminology drift, and a
config-path fallback gap, plus stale doc claims. **All were resolved:**

| finding | fix | commit |
|---|---|---|
| CBOR accepted trailing bytes | decode must consume the whole payload; fuzz seed; arena clamp | `0121c62` |
| `ACTOR_RESIZED` missing | event + hook + typed `cols/rows`, emitted from `agentc_process_resize` | `16bb549` |
| gate fragility (leaked processes) | shared bounded `tests/harness.sh` cleanup (serve/e2e/mcp) | `1d5552d` |
| `serve` no graceful shutdown | async-signal-safe SIGINT/SIGTERM → clean exit + unlink | `bc23441` |
| settings-editor e2e untested | `make settings-e2e` + `tui_e2e.py` | `cce0b56` |
| SDK `submit` doc mismatch | unified on the cooperative run loop | `40248da` |
| R-D31 terminology drift | model-cycle events `TURN_*`→`STEP_*`; `turn` fires at `AGENT_START/END`; RPC wire names frozen | `84799a3`, `7910b8a` |
| `abird/agentc` orphaned users | transparent legacy fallback in the config layer | `65f19ff` |
| stale docs | corrected across design/plans/STATUS/abi | `44f67a0`, `7392799` |
| `agentc_rm_rf` couldn't delete dirs | added `os_rmdir` (AT_REMOVEDIR / RemoveDirectoryW) | `c4a0473` |

### Phase 8 — housekeeping
`abird/agentc` rename, `HANDOFF-AGENTC.md` (this file), README/index fixes, and
(a concurrent writer in the same repo added, independently) nix flake
modularization, `clean`/`purge`, docs moves and lint gates.

---

## 5. Decision index (where each family lives)

| family | scope | authoritative file |
|---|---|---|
| **R-D1…R-D31** | P6 runtime design (primitives, hooks, actors, trust, sub-agents, settings, status, voice, terminology) | `.agents/plans/P6-RUNTIME.md` §3 |
| **S-D1…S-D6** | P6 queue/steer semantics | `.agents/plans/P6-SIGNALS.md` §0/§5 |
| **T-D1…T-D12** | tool engine (external vs built-in) | `.agents/plans/P5-TOOL-ENGINE.md` |
| **A-D1…A-D12, B-D1…B-D6, C-D1…C-D12**, codec decisions | P5 RPC/SDK/protocol | `.agents/plans/P5-AXES.md` §2 |
| **C-D1…C-D14** | P6 council chair decisions (scope enums, mailbox overflow, terminology, ABI freeze, renames) | `.agents/runs/p6-runtime-council/CHAIR.md` |
| **P5-D1…P5-D4** | P5 council chair decisions (current-state deltas, axis order, scope guard, gates) | `.agents/runs/p5-axes-council/CHAIR.md` |
| per-pass implementor notes | the exact choices/deviations of every completion pass | `.agents/runs/p6-runtime-council/CHAIR.md` + `STATUS.md`, `p5-axes-council/STATUS.md` |

---

## 6. Current state — modules and commit landmarks

Public headers: `agentc.h`, `agent.h`, `plat.h`, `net.h`, `wire.h`, `agentc_ext.h`,
`actor.h`, `message.h`, `hook.h`, `trust.h`, `decide.h`, `settings.h`, `status.h`,
`config.h`, `session.h`, `oauth.h`, `mcp.h`, `discover.h`, `voice.h`, `cbor.h`,
`cbor_json.h`, `frame.h`, `protocol.h`, `ws.h`, `agentc_sdk.h`, `agentc_client.h`.

New core modules (P6): `actor.c`, `process.c`, `signal.c`, `queue.c`, `hook.c`,
`runtime.c`, `trust.c`, `approval.c`, `agent_spawn.c`, `decide.c`, `settings.c`,
`voice.c`, `runsession.c`, `select.c`, `policy.c`, `project.c`.
New app/wire/sdk (P5): `mode_serve.c`, `rpc_dispatch.c`, `sdk/sdk.c`,
`wire/{frame,proto,client,ws}.c`, `net/linux/unix.c`, `tools/agentc-client.c`.
Tests: 46 golden binaries (`tests/*.c`) + shells (`run.sh`, `e2e.sh`, `ext.sh`,
`mcp.sh`, `serve.sh`, `net.sh`, `dyn.sh`, `wine.sh`, `harness.sh`), 97 expected files.

Landmark commits (oldest→newest): `5cbb74e` (freeze the 15 P6 plans) · `3242dab`
(P6-A) · `48297e8` (P6-A closure) · `5244a81`…`c97bb8b` + `3464061`…`0be6507`
(P6-B) · `b1e4c29`…`5b4ae1f` (P6-C) · `22ec001`…`7ee6797` (P6-D) · `4d75971`…
`d252862` (P6-E) · `4e929dd` (abird paths) · `5450465`…`22bdc38` (P5-A) ·
`da83cc9`…`f9de462` (P5-B/B4) · `80c66b1`…`2d2834b` (P5-C1) · `76c9609`…`eec64e8`
(C2) · `a45d499`…`167e2a0` (UI dialogs) · `94512ef`…`bef0619` (C3) ·
`6684e3e`…`7ecd79c` (interleaving) · `2522de0` (v0.8.0) · `0121c62`…`c4a0473`
(review resolution). Run `git log --oneline` for the live list.

---

## 7. Gates (run before every commit; all must be green)

```sh
make check                       # golden suite (offline, deterministic)
./tests/ext.sh                   # C + Rust extension examples
./tests/mcp.sh                   # stdio MCP against a python mock
./tests/e2e.sh                   # mock provider: round-trip, resume, TUI, serve, rpc-check
sh tests/serve.sh                # framed-CBOR server: protocol, client, ws, tls, dialogs, multi-session
make windows                     # Windows x86-64 cross build (LLVM on PATH)
make lib && make libcheck && make libstrict   # SDK archive + deny-lists + strict no-app
nix develop -c make wine-check   # Windows golden suite under Wine (47/47)
nix develop -c make dyn-check    # dynamic-extension runtime under Wine
make settings-e2e                # /settings editor e2e (release-exts)
```

Determinism: goldens are byte-exact; do not edit a golden to make a broken
change pass. `tests/harness.sh` reaps stale processes/sockets before and after
serve/e2e/mcp. If `mcp.sh`'s `stubborn_shutdown` ever looks red, re-run once.

---

## 8. Pending items (exhaustive)

### (a) Out of scope — platform sandboxes (fail-closed)
Windows restricted-token/low-integrity, macOS Seatbelt, Linux host allowlist;
`os_sandbox_apply` returns `-ENOSYS` where unimplemented and the spawn is
**denied**. Recorded in `design/10-platform.md` §6. **Windows PTY is `-ENOSYS`**
(`tty:true` actors are Linux/macOS only) — a documented real asymmetry.

### (b) Environment-blocked here
No macOS SDK offline → no native mac build, no Seatbelt verification, the macOS
`libso`/archive branch is unexercised. No real Windows host → the restricted-token
boundary cannot be verified (Wine's token/integrity support is incomplete).

### (c) Narrow, documented constraints (non-sandbox)
- **Multi-session concurrency granularity is one model step**: a blocking provider
  read inside a step holds the single-threaded loop, so a sibling session's next
  step waits (`design/61-protocol.md` §7.1).
- **Settings-editor e2e** needs the `release-exts` build (a setting-declaring
  extension); it skips cleanly otherwise.
- The `--mode rpc` `get_messages` gained additive paging (`total`/`next_cursor`).
- `make libso` relies on `-lgcc` for mbedTLS's `__udivti3` (localized by the
  version map); a compiler-rt-only toolchain would need a local shim.

### (d) Pre-existing features
**Landed in P7 (this handoff's postscript):** `token_budget` (P6-SURFACE §18
#23), `/fork`/`parent_session`/`/tree`, the MCP optional GET stream, extension
helper-process isolation, virtual models/routing, and `client_id` dedupe. Also
landed: custom provider base URLs and ad-hoc custom providers. Council record:
`.agents/runs/p7-council/`.

**Still deferred:** `INLINE-RESIZE-DESIGN.md` (the ADR-30 flow model in
`design/40-tui.md` §1.4 is the as-built inline resize behavior; the Design 2
rewrite was not pursued).

### (e) Housekeeping to know about
- A **concurrent writer** has been committing unrelated hygiene in this repo
  (nix flake modularization, `clean`/`purge`, docs moves, lint gates). It also
  produced a nix refactor snapshot preserved by the chair in **`git stash@{0}`**.
  Reconcile/drop that stash as you see fit; it is not part of P5/P6.

---

## 9. Where everything is recorded

- **Working agreement / rules**: `.agents/AGENTS.md`.
- **Index**: `.agents/README.md`.
- **Current-state contracts**: `.agents/design/00…70-*.md`. Each ends with an
  as-built section for its phase. These are the code's source of truth.
- **Forward/landed plans**: `.agents/plans/` — `P5-AXES.md`, `P5-TOOL-ENGINE.md`,
  `P6-*.md` (all 16 P6 docs, each marked as-built), and this file.
- **Run records**: `.agents/runs/` —
  - `p6-runtime-council/`: `CHAIR.md` (decisions C-D1…C-D14 + every pass's notes),
    `STATUS.md` (the running log), `abi.md` (append-only inventory / conflict
    matrix / reviewer checklist), `audit.md` (independent audit), and the
    per-area implementation plans `b-plan.md`, `c-plan.md`, `d-plan.md`,
    `e-plan.md`.
  - `p5-axes-council/`: `CHAIR.md` (P5-D1…D4), `STATUS.md` (axis commit maps,
    gate results, residuals).
  - `final-review.md`: the fresh adversarial review and its resolution table.
- **Changelog / version**: `CHANGELOG.md`, `VERSION` (0.8.0).

---

## 10. Starting prompt for the next agent

> Work in `/home/pvl/spaces/abird/src/agentc` — `agentc`, a freestanding C23
> coding agent (no libc, static, single-threaded, pump-driven; negative errno;
> append-only ABI except the sanctioned renames R-D24…R-D31; bounded and
> fail-closed; byte-exact golden tests; commit with signed `git commit`).
>
> **Read first, in order:** `.agents/plans/HANDOFF-AGENTC.md` (the master
> handoff — the whole arc: P5-TOOL-ENGINE, the P6 runtime design R-D1…R-D31 and
> its implementation waves A–E, the completion passes and audit, the
> `abird/agentc` path rename, the P5-AXES plan and its axes A/B/C, the fresh
> review and its resolutions, release v0.8.0, and every pending item); then
> `.agents/AGENTS.md` (working agreement and hard rules); then
> `.agents/README.md`; then `.agents/design/00…70-*.md` (current-state
> contracts); then the plan for whatever you work on under `.agents/plans/`;
> then the council records under `.agents/runs/` (`p6-runtime-council/`,
> `p5-axes-council/`, `final-review.md`).
>
> **Everything is implemented and gate-green** (`make check`, `ext.sh`,
> `mcp.sh`, `e2e.sh`, `serve.sh`, `make windows`, `make libcheck`,
> `make libstrict`, `nix develop -c make wine-check` 47/47, `make dyn-check`,
> `make settings-e2e`). Keep them all green. `--mode rpc` JSONL and every
> existing golden must stay byte-identical.
>
> **The only explicitly deferred, out-of-scope work is the platform sandbox
> backends** (Windows restricted-token, macOS Seatbelt, Linux host allowlist —
> fail-closed; `design/10-platform.md` §6), plus the documented narrow
> constraints and unrelated pre-existing features listed in
> `HANDOFF-AGENTC.md` §8. If you are asked to do more, pick from that list and
> follow the same discipline: read the relevant spec, make the smallest
> append-only change, add a byte-exact golden, keep every gate green, and update
> the matching `design/` doc and the council record in the same change. If a
> capability cannot be expressed as a message kind, hook stage, envelope
> decision, tool runner, or host service, stop and record a new decision rather
> than adding a parallel mechanism.
>
> **Before you start:** confirm the tree is clean (`git status`). A concurrent
> writer has been making unrelated hygiene commits and left a nix refactor in
> `git stash@{0}` — do not build on unrelated WIP; if you need a clean base,
> commit/stash only your own files explicitly (never `git add -A`). Report your
> changes and the gate results as you go.
