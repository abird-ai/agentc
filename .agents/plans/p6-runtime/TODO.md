# TODO — p6-runtime

Plan: P6 runtime — actors/processes, message bus + priority queue, 10-stage hook pipeline, trust/approvals/sandbox, sub-agents/deciders, settings/status/voice. Codex-parity by behavior (four primitives + two hook verbs).
Status: DONE
Checkpoint: `5e99ca9` (user-approved snapshot)
Design contract: `.agents/design/20-core-agent.md` (loop/actors/tools/hooks) · `.agents/design/00-architecture.md` (primitives) · `.agents/design/10-platform.md` (sandbox) · `.agents/design/30-extensibility.md` (ABI/settings) · `.agents/design/40-tui.md` (status/approval)
Decisions: `R-D1`..`R-D31` (`.agents/plans/P6-RUNTIME.md` §3) · `C-D1`..`C-D14` (council chair; run record removed) · `S-D1`..`S-D6` (`.agents/plans/P6-SIGNALS.md` §5) · ABI inventory (run record removed; checkpoint `5e99ca9`)

## Committed

### P6-A — actors & processes
- [x] Actor registry + pipe/PTY process actors, `os_pty_*`, stdin, output ring/spill, graceful→force cancel, stall, freeze — provenance: `3242dab` · after: `9bbfef4` (mac leader-pid freeze), `ebc581c` (child pgid) · `.agents/design/20-core-agent.md` §4.6
- [x] `bash` routed through the actor + model-facing `process` tool; Wine golden parity, P6-A closure — provenance: `48297e8`

### P6-B — messages, queue, hooks
- [x] Header/limit scaffolding (B0); bounded message bus with priority-drop mailboxes and deterministic drain (B1) — provenance: `5244a81` · after: `27b5637`
- [x] One turn-targeted `(turn,priority)` queue, no delivery enum (B2, S-D1..S-D6); agent-actor binding, control scopes, `runtime_tick` (B3) — provenance: `86d4167` · after: `9690edd`
- [x] Actor/queue typed events → hook points incl. observe-only `event` stage (B4/B12); boundary steer + auto-start (B5); mid-stream preempt + non-steerable hold (B6) — provenance: `2c0711d` · after: `520798b`, `5f7498e`, `8079e2d`
- [x] Universal hook pipeline (registry/chain/two-verb defer) + ten stages in loop/bus (B7/B8) — provenance: `10baa6d` · after: `d664242` · `P6-HOOKS.md`
- [x] `queue` tool + RPC adapters (B9, `1563563`); app root-actor binding/`runtime_tick`/auto-start (B11), `AGENTC_TOOL_NO_HOOKS` (B13), tool-process preempt cancel (B14), queue persistence (B15) — provenance: `3464061` · after: `78bb6d6`, `0872d3b`, `dcf89be`; B10 docs/as-built + round-2 record (`c97bb8b`, `0be6507`)

### P6-C — trust, approvals, sandbox
- [x] Trust envelopes, policy, approval contract, per-platform sandbox surface (steps 1-3) — provenance: `b1e4c29` · `P6-TRUST.md`
- [x] Four-layer RO/RW/boundary enforcement; trust/approval/sandbox events + hooks + host services (steps 4-6) — provenance: `50eecc3` · after: `e4b156a` (docs + Windows goldens)
- [x] Host trust-service tests + single `tool_approval` dispatch — provenance: `2ac53af` · after: `d160179` (denial text/`trust_denied`), `5b4ae1f` (fixture hygiene)

### P6-D — sub-agents & deciders
- [x] Unified `agentc_agent_create` (one type), agent tree/lineage, direct model/provider/transport inheritance — provenance: `22ec001` · `P6-SUBAGENTS.md`
- [x] Decision seam + `hook_branch` + `agent` op tool + `custom type:"agent"` persistence — provenance: `22ec001` · after: `abef9e3`, `510dd1e` (RPC agent ops/decide, Windows-safe golden) · `P6-DECIDERS.md`
- [x] `agent_result` hook-point bridge (`2f7ab46`); P6-D as-built notes and deviations (`7ee6797`)

### P6-E — extensions & integration
- [x] R-D29/R-D30 renames: `AgcExtPrompt`/`add_prompt`, `AgcExtStatusLine`/`add_status_line` — provenance: `4d75971`
- [x] Extension settings service + host slots + CLI/RPC + `settings_change`/event — provenance: `d711f80` · `P6-SETTINGS.md`
- [x] Status regions/lifetimes + `STEP_END`/`TURN_END`/`FINAL_OUTPUT` notices — provenance: `c48c396` · `P6-STATUS.md`
- [x] Voice `content_type`/REALTIME kinds/BIDI pipe/mic+speaker trust/stub extension — provenance: `9ae2c83` · after: `d252862` (Wine BIDI echo) · `P6-VOICE.md`
- [x] TUI chrome regions above/below composer and below status — provenance: `5c1a5ed`
- [x] Docs fold-in, plan as-built headers, Windows `_fltused` (`0014e8e`); council final status, gate matrix and open items (`431cb6f`)

### Completion passes 2a–2g
- [x] 2a — async runtime continuations + true BACKGROUND `hook_branch` defer (`bc8c1f2`); audit B1/M2 owned gate reason + pumped program-approval wait (`5857775`)
- [x] 2a — program decider over a BIDI process actor + HAS-gated `decider_register` (`187de06`; `5c19ede`); `trust.subagent.*` config (`d86917b`)
- [x] 2a — Layer-4 `ls`/`find`/`grep` + ambient spawn envelope (`24db619`; `0fb2c91`); built-in `actors` status line, `get_state`, event/scope/`ACTOR_INPUT` fixes, `message_send` slot + stub removal (`bad9a3b`, `8d2b226`, `ce91c48`, `717207c`, `f1e130a`)
- [x] 2a — audit M3/M5 reentrancy + bounded shutdown (`890b431`), M1 policy auto-load (`fbd8988`), `approvals.jsonc` (`64c965a`), resume read-only tree (`1bd3982`)
- [x] 2b — M6 `freeze_test` golden + M7 explicit `skip` tokens (`39ff3f4`, `4361cf9`, `c504384`); MCP decider fail-closed (`34b59aa`); prompt-contribution golden (`f7afa13`); realpath/macOS/Linux docs (`e268232`)
- [x] 2c — bounded framed voice helper codec (`0fc1317`); TUI `/settings` picker + secret `…last4` + notice replay (`c53c888`); `ext.sh` ABI checks (`ae2c7a8`)
- [x] 2d — decision self-resume + pending/resume table (`bc05d53`); async `approval` hook from the pump (`23fc5d9`); B8 foundation (`d53feb1`); e2e background round-trip + nested re-entry reset (`f0572b2`, `905d859`)
- [x] 2e — resumable one-model-step scheduler `AGENTC_RUN_YIELD` (`e5332ae`); B8 fold of `input`/`message_end`/`turn_end`/`tool_result` (`675d742`); records `276b4d5`, `15b3bc9`
- [x] 2f — B8 fold of `tool_call`/`agent_before_settle`/`session_before_compact` + no-tool slice (`55561f8`); mid-SSE cancel + paced steer e2e (`b104029`); async TUI approval overlay e2e (`42d8218`)
- [x] 2f — platform attempts documented fail-closed + gate matrix (`07c9c9e`, `83b4222`); MCP `stubborn_shutdown` fix (`c082c03`) + chair verification (`d0eb4a6`)
- [x] 2g — provider-hook fold via `provider` stage, B8 complete (`fdff120`); TUI approval polish (`42abd37`); as-built docs fold-in + platform deferral (`76a4ee7`); chair: P6 complete (`af41704`)
- [x] Post-sign-off — pump-driven async SDK submit (`42bb56d`); TUI `/settings` single-value editor (`339e250`); cumulative `token_budget` soft cap (`c2fa1d9`); checkpoint commit (`5e99ca9`)

### Decision & evidence pointers
- [x] `R-D1`..`R-D31` decided in `.agents/plans/P6-RUNTIME.md` §3 (no runtime change without a new decision)
- [x] `C-D1`..`C-D14` chair decisions (run record removed; git history at `5e99ca9`); `S-D1`..`S-D6` queue/steer decisions in `.agents/plans/P6-SIGNALS.md` §5
- [x] Independent audit findings (B1/B2, M1..M7, m1..m8) + append-only ABI inventory (run record removed; git history at `5e99ca9`)

## Uncommitted
- [ ] True async approval `runtime_defer` resume path — core `approval` stage (`23fc5d9`) and decision self-resume (`bc05d53`) landed, but a fully parked controller/`tool_approval` chain re-entered via a `runtime_defer` handle is still the documented seam (fail-closed until then). Owner: core/approval.
- [ ] Background child one-step-per-tick scheduling — `agent_actor_step` yields one model step per `agentc_actor_tick` at the post-tool/no-tool `step_end` (`e5332ae`, `55561f8`); a sub-step yield inside a long provider read (the one-model-step multi-session granularity limit, `.agents/design/61-protocol.md` §7.1) is not implemented. Owner: core/agent.
- [ ] Resume re-creates read-only actor rows — `agentc_session_load_agents`/`persisted_agents` (`1bd3982`) rebuild the data tree only; execution rows are not re-created. Owner: app/session.
- [ ] `tests/mcp.sh` `stubborn_shutdown` — focused mode + no-orphan check landed (`c082c03`); residual flake risk is environmental, not P6. Owner: tests.

## Deferred
- [ ] Windows restricted-token/low-integrity sandbox — fail-closed `-ENOSYS`; `os_sandbox_available` refuses and `os_spawn_*_sandboxed` denies (`os_sandbox_apply` seam). `.agents/design/10-platform.md` §6
- [ ] macOS Seatbelt sandbox — compiled but unverified (no offline cross SDK); `strict` returns `-ENOSYS`. `.agents/design/10-platform.md` §6
- [ ] Linux host allowlist — only `network=deny` kernel-enforced (seccomp); `allowlist` stays the Layer-2 declared-tool check (Landlock rules are port-scoped). `.agents/design/10-platform.md` §6
- [ ] Windows PTY — `-ENOSYS`; `tty:true` actors are Linux/macOS only. `.agents/design/10-platform.md` §6
- [ ] `INLINE-RESIZE-DESIGN.md` Design 2 rewrite — not pursued; the ADR-30 flow model is the as-built inline resize. `.agents/design/40-tui.md` §1.4
