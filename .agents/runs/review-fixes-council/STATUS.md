# STATUS — resolved

All review findings addressed. Tree is uncommitted; final gate green.

- [x] WS1 harness: tests/run.sh + tests/ext.sh isolate HOME/XDG (fixes the red
      `make check` for any developer with ~/.config/agentc/auth.jsonc).
- [x] WS2 base: unsigned glob class ranges; JSON rejects raw C0; fmt h/z/t;
      saturating deadline.
- [x] WS3 ext+wire: MCP prompts/list commit is journaled+atomic with retry;
      stdio cap is per unterminated line; nested extension callbacks preserve the
      outer cancellation window; NULL-job guards; TE overrides CL, only final
      chunked dechunks.
- [x] WS4 tui: bullet continuation has no spurious marker; startup picker opens
      before any prompt (skipped when a prompt is supplied).
- [x] WS5 tools/agent: reads retry EINTR; length-aware tool result preserves
      embedded NUL to the wire/session.
- [x] WS6 core: dynamic model table raised to 2048 and reset per pass via
      `agentc_model_clear_dynamic(NULL)` (static rows survive); auth follows
      flag > OAuth > env > auth.jsonc > config, fail-closed; custom
      `providers.<id>.base_url` gateways materialize; Codex emits
      `max_output_tokens` + bounded call blocks; header sanitize; RPC models
      1024; owned Codex client_version; config EINTR/leak/alias; session header
      rewrite + single-write append_raw; `${ARGUMENTS}`/`${@}`; bounded find scan.
- [x] chair: setup.h prototype; Retry-After IMF-fixdate with digit-overflow
      bounds; EINTR at session/oauth/mcp/engine remaining sites; remove redundant
      `agentc_model_clear_all_dynamic`; `agentc_model_capacity()` for the cap test.
- [x] WS8 docs reconciled to shipped behavior (README quick start, auth order,
      Retry-After date, model reset/cap, gateways, Codex cap, `/fork`/images/
      cache_control marked absent, hermetic suite, RPC-vs-session args).

Verification (independent, adversarial):
- model registry clear/static/compaction: REAL; cap now sized for several
  providers (2048 = 8x the 256 per-provider discovery bound).
- auth resolver + gateway + Codex + RPC: REAL; gateway rows are process-lifetime
  like every dynamically-named compatible row (documented).
- MCP rollback + per-line cap + signal restore + HTTP framing: REAL.
- Retry-After + EINTR: PARTIAL first pass (signed overflow, 3 missed sites) ->
  fixed and re-checked.
- TUI markdown: NEW regression (sibling bullets drifted) -> fixed (pad is the
  source indentation width, not the absolute buffer index).
- NUL/JSONC/glob/fmt/deadline/resources/docs: REAL.

Gates: `make check` 31/31 + check-ext; `./tests/e2e.sh`, `./tests/mcp.sh`,
`./tests/ext.sh`; `make windows` (build/agentc.exe). All green.

Residual (pre-existing, documented not fixed):
- Extension `tool_execution_end` payload and the `tool_result` hook still
  serialize the result with `strlen`, so an extension observing an embedded NUL
  sees it truncated (core transcript/wire/session preserve it).
- `ls` still collects the whole directory (its suffix sort needs the full set to
  report the true truncation count); `find` is bounded.
