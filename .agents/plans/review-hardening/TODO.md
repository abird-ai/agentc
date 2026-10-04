# TODO — review-hardening

Plan: Resolve all findings of the fresh review of `agentc@36dce35` (P9): critical/major security+correctness, medium/low, and composability.
Status: DONE
Checkpoint: `5e99ca9`
Design contract: `.agents/design/20-core-agent.md`, `.agents/design/30-extensibility.md`

## Committed
<!-- append-only, oldest first; newest work at the bottom -->
### Wave A — five parallel worktrees (base `36dce35`)
- [x] WS1 tools: job-driver reentrancy (C1); `job_tools[32]` OOB/tool association (C2); engine uniformity (find sort vs grep spill); POSIX grep `-<n>-` context parse; external trust-path re-check (C7); appended `AgcToolCall.flags` (I1/D2) — provenance: `33fd7f9` · after: merge `168b9ae` · `.agents/design/20-core-agent.md`
- [x] WS3 base/wire: CBOR→JSON empty strings (C3), NUL keys, item budget; HTTP trailer split; WS handshake cap + Origin/Host policy (I2); `JsonW` invalid-JSON latch; NULL+len guards; `cbor_text` `n==0` — provenance: `306f883` · after: merge `99d5278` · `.agents/design/30-extensibility.md`
- [x] WS4 config/session: `/resume` queue replace (C4/P8-D4); refuse unreadable-file overwrite (C5/P8-D7); queue `paused` reset; event re-arm after init→shutdown→init; rebind NULL no-op — provenance: `c4494e4` · after: merge `cee9198`
- [x] WS5 TUI: `/model` full ids (C6); empty-filter picker; tab wrap; bracketed paste; `/fork` doc correction — provenance: `44a1b18` · after: merge `b6e9d05` · `.agents/design/40-tui.md`
- [x] WS7 presets: provider registry is the single preset source (D1) — provenance: `bea87bb` · after: merge `8ef6b02`

### Wave B — trust + serve/platform/ext
- [x] WS2 trust/approval: capability-based fail-closed classification (S4/D2); monotone network narrowing (S1); approval memo key over roots/net/hosts/policy (S2); child `policy_id` (S3); hosts parse; depth monotone; cancel leak — provenance: `bc8dc23` · after: merge `fea726d` · `.agents/design/20-core-agent.md`
- [x] WS6 serve/platform/ext: WS Origin/Host + token rule (S5); attacker-pre-creatable socket dir (S6); constant-time token compare (S7); idle/hello timeout (S8); bounded read loop (S9); win `boundary==NONE`; devnull cloexec; nonblock log; `serve_read_file` leak; MCP `-EINTR` (C9); win helper poll (C8) — provenance: `5fc1f26` · after: `4781cc2`, `371ca4d`, merge `e78377c` · `.agents/design/30-extensibility.md`
- [x] Integration fix: `AGENTC_TOOL_NO_APPROVAL` path uses flags-aware `agentc_trust_decide_flags` — provenance: `aca0442`

### Verification-fix waves (WSF/WSG/WSH)
- [x] WSF1: cap an incomplete WS handshake; drop ambiguous POSIX grep context parse — provenance: `e9b7676` · after: merge `2939461`
- [x] WSF2: serve idle deadline vs in-flight run; bare relative socket; timeout clamp; dir write bits — provenance: `1741ab7` · after: merge `b832f45`
- [x] WSF3: bound MCP `-EINTR` retries by the RPC deadline; publish ext network bit — provenance: `0b2d3a1` · after: merge `f95b989`
- [x] WSG1 (VC1b): stale parked verdict cleared on release+alloc; narrowed `args_json` never memoized; no owned policy-reason leak/overwrite; static ALLOW labelled `allowed`; child depth saturates; `NO_APPROVAL` runs `decide_skip_approval` (policy + flags) — provenance: `3915bdc` · after: merge `fbad8a1`
- [x] WSG2: capability-driven ext `policy_eval` (unknown ⇒ mutating); MCP `openWorldHint:true` ⇒ `AGENTC_EXT_TOOL_NET`; deterministic `mcp_test` condition waits — provenance: `9cd4cc5` · after: merge `f949a42`
- [x] WSH: skip-approval evaluates process policy on layer-2 ASK/DELEGATE (policy DENY wins); approver `ask`⇒DENY; loop blocks anything not ALLOW (incl. memoized ASK) — provenance: `4509c87` · after: merge `010222b`
- [x] Independent verification VCF1/VCF2/VCF3 (trust/approval, ext/MCP, final fix): one low leak found; blocked verdict's owned `args_json` now freed in the agent loop; no fail-open path — provenance: `5e99ca9`
- [x] Durable docs: CHANGELOG `[Unreleased]`; design/20 (capability trust, approval invariants, per-invocation jobs, session/config); design/30 (tool flags, ext `policy_eval`, MCP net bit, presets) — provenance: `5e99ca9`
- [x] Run record: MISSION/CHAIR/STATUS — provenance: `b9d75fb`, `7648d4a`, `5e99ca9`

## Uncommitted
- [ ] none — plan closed

## Deferred
- [ ] D5 god-file splits: `ext/registry.c` ~6.1k, `ext/mcp.c` ~5.0k, `core/agent.c` ~4.0k — not correctness/security; high regression risk with large static state; separate focused refactor
- [ ] Broad cross-adapter helper dedup (D3) — applied only where it removed the active trust-path bug; remainder deferred
- [ ] `agentc_policy_recompose` declaration-layer retention (WS-G1 residual): a RO/net trust filter can leave a named mutating tool in the model-facing table, though layer-2 `decide_flags` + layer-4 re-check still deny every call — needs a `policy.c` API change to distinguish deferred-MCP from fully filtered
