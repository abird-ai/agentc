# TODO — review-fixes

Plan: Land the fix-review-council five-stream fix series and the review-fixes-council P8 workstreams (code + goldens + design).
Status: DONE
Checkpoint: `5e99ca9`
Design contract: `.agents/design/00-architecture.md`, `.agents/design/20-core-agent.md`, `.agents/design/30-extensibility.md`, `.agents/design/40-tui.md`

## Committed
<!-- append-only, oldest first; newest work at the bottom -->
### fix-review-council — five streams (base pre-`fecc97b`)
- [x] Shared append-only declarations: `AGENTC_EV_MSG_RESET`, `agentc_tool_write_len()`, `agentc_net_poll()` — provenance: `fecc97b`
- [x] Stream 1 net/wire/TLS: Win `SCHANNEL_CRED` field order + `WinTimeStamp`; IP-literal cert identity (NO_SNI no longer suppresses); HTTP send `-EINTR`; `agentc_net_poll` seam; `:`-whitespace reject; SSE EOF finish; mock short-cap; URL error — provenance: `b929444` · after: `.agents/design/10-platform.md`
- [x] Stream 2 tools: `O_CLOEXEC` on spill/devnull/temp; explicit-length write; `read` binary sniff; non-blocking step/bounded reap; spill fallback; run-vs-start precedence; `.gitignore` empty pattern — provenance: `49c450f`
- [x] Stream 3 core: retry `MSG_RESET`; `preserve_terminal`; abort-interruptible backoff; retry jitter; alias-safe setters; prompt lifetime contract; discover/config/auth/status errno + leak fixes — provenance: `3d0ab33` · after: `.agents/design/00-architecture.md`, `.agents/design/20-core-agent.md`
- [x] Stream 4 TUI: validating UTF-8 in render/editor/markdown; row clamp; `MSG_RESET` rollback (`chat_truncate`); `last_cr`; raw-mode failure; render EL SGR; shared layout budget — provenance: `a848a18` · after: `.agents/design/40-tui.md`
- [x] Stream 5 app/ext/prov: discover-style ABI translation; Anthropic content-block routing; codex `response.incomplete`; RPC read guard; setup auth leak; owned base-url; provider-row ownership; MCP path truncation — provenance: `bc4dc6e` · after: `.agents/design/30-extensibility.md`
- [x] Regression round 2 (send deadline guard; SSE WHATWG finish; `chat_seal`; scrollback row; `msg_mark`; `last_cr` window; extension `message_reset`; spill-failure; Anthropic lenient deltas; base-url cache; docs) — folded into the five stream shas; no separate commit
- [x] Round 3 login/logout/model bug: `login` writes `setup.jsonc` default; `logout` accepts any provider; provider-aware TUI `/model`; README — provenance: `9a7ede4`
- [x] Run record: PLAN/STATUS — provenance: `04bc319`

### review-fixes-council — waves 1–3 (P8; base `8c83a1b`)
- [x] Chair declarations (`api: append review-fix declarations`) — provenance: `0cae6cc`
- [x] WS1 harness: hermetic `HOME`/`XDG_*` in run.sh/ext.sh — provenance: `08b521e`
- [x] WS2 base: parser/glob/fmt/deadline hardening — provenance: `1427b38`
- [x] WS3 wire: TE overrides CL; only final chunked dechunks — provenance: `9f4f05f`
- [x] WS3 ext: atomic MCP refresh + outer cancel scope — provenance: `e1c6e13`
- [x] WS4 TUI: markdown continuation + startup picker order — provenance: `8e4af23`
- [x] WS5 tools/agent: EINTR retries + NUL-preserving tool result on wire/session — provenance: `5f1d8bd`
- [x] WS6/WS7 core + providers/app: dynamic model reset/cap 2048; auth precedence; custom gateways; Codex `max_output_tokens`; Retry-After date; session header; `${ARGUMENTS}` — provenance: `7cdd84d`
- [x] WS8 docs reconciled to shipped behavior — provenance: `a916c50`
- [x] Independent verification: markdown bullet regression + Retry-After/EINTR findings — folded into `8e4af23` / `7cdd84d`; no separate sha
- [x] Run record: PLAN/STATUS — provenance: `477c407`

## Uncommitted
- [ ] none — plan closed; accepted residuals live under Deferred

## Deferred
- [ ] Extension `tool_execution_end` / `tool_result` hook serializes with `strlen`, so an embedded NUL is truncated (core transcript/wire/session preserve it) — pre-existing, documented, out of scope
- [ ] `ls` collects the whole directory to report a true truncation count; `find` is bounded — accepted, needs a streaming suffix sort
- [ ] Prompt/file-template `ud` for a reused slot is process-bounded; header contract only — documented, not freed
- [ ] `agentc_prov_openai_compatible` heap rows are process-lifetime producer-owned — documented at the reset site
