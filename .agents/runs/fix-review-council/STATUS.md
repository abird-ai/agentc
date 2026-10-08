# fix-review-council — status

All review findings fixed across five parallel streams, integrated and verified by
the chair.

## Verification (frozen tree)
- `make check` — 32/32 golden tests + extension pipeline, no compiler warnings.
- `./tests/e2e.sh` — mock provider round-trip, `--continue`, pty TUI, onboarding.
- `./tests/mcp.sh` — MCP stdio against the python mock.
- `./tests/ext.sh` — C + Rust extension pipeline.
- `./tests/net.sh` — live loopback HTTP/SSE/TLS.
- `make windows` / `make windows WIN_ARCH=arm64` — both link clean (validates the
  SChannel layout and the new `agentc_net_poll` on Windows).
- Wine unavailable here, so `make wine-check` was not run.

## Fixed
High: Win SChannel `SCHANNEL_CRED` field order + `WinTimeStamp`; retry
`message_reset` (core + TUI rollback + JSON/RPC forwarding); explicit-length
write / read binary sniff; validating UTF-8 in render/editor/markdown; extension
discover-style ABI translation.
Medium: abort `preserve_terminal`; abort-interruptible backoff; retry jitter;
alias-safe agent setters; Anthropic content-block routing; HTTP send EINTR;
`agentc_net_poll` seam; config Ollama leak; auth.jsonc fallback; TLS identity for
IP literals (NO_SNI no longer suppresses verification); tool `O_CLOEXEC`;
non-blocking job step; TUI row clamp + shared budget + live-message commit;
prompt lifetime contract.
Low: a long tail — spill fallback bounded, SSE EOF finish, header whitespace,
mock short cap, transport URL error, discover out leak, status idempotency,
codex `response.incomplete`, RPC read guard, setup auth leak, owned base-url
cache, provider-row ownership note, MCP path truncation, `.gitignore` empty
pattern, input `last_cr`, raw-mode failure, render EL SGR, real errno in core.

Design docs updated in step: `00-architecture.md`, `10-platform.md`,
`20-core-agent.md`, `30-extensibility.md`, `40-tui.md`.

## Known residual / intentional
- Committed terminal rows cannot be unprinted; the live-message commit rule keeps
  an in-progress (retryable) message out of permanent scrollback so a retry rolls
  back cleanly.
- `agentc_setup_base_url` returns a per-slot owned cache; env is treated as
  process-stable.
- `agentc_prov_openai_compatible` heap rows are process-lifetime producer-owned
  (documented at the reset site).
- Prompt/file-template `ud` for a reused slot is process-bounded (documented).
- MCP stale-pid "kill recycled pid" claim was disproven (`os_wait` returns -2 and
  the `!= -1` test treats it as exited); no change made.

## Round 2 — regression review of the fix set
Five independent reviewers re-examined the uncommitted diff. Confirmed findings
and fixes:
- **HTTP send `-EINTR` bypassed the deadline** (new busy-loop) → added the
  wall-clock deadline check to `http_send_request`, matching recv.
- **SSE finish could hard-fail a truncated stream** instead of retrying →
  `agentc_sse_finish` now discards an unterminated final field line and only
  flushes a complete-but-unterminated event (WHATWG), so partial JSON never
  reaches the mapper. Golden updated; the mapper-error/retry path is preserved.
- **TUI continuation turns merged into the previous block**, defeating
  `message_reset` rollback → `chat_seal()` forces a new block at each
  `MSG_START`/`MSG_RESET`.
- **Scrollback lost its reserved row** in the unified budget → it now budgets
  `rows - 1` (normal-buffer draws can never scroll), matching the old formula.
- **`msg_mark` went stale after `/clear`/`/new`** → cleared on both commands and
  clamped in `MSG_RESET`; `block_is_live` guards `msg_mark <= chat.n`.
- **`input_idle` cleared `last_cr` every tick** → CRLF fold now bounded by the
  idle window via `cr_at_ns`.
- **`message_reset` was invisible to extensions** → mapped onto the
  `message_update` observe point as `{"type":"message_reset"}`; documented.
- **`bash_finish` advertised a fresh spill after a spill failure** → it now
  honors `spill_failed` and treats a failed write as unsaved.
- **Anthropic unmapped text/thinking deltas were dropped** → lenient fallback to
  the last block of the type; `input_json_delta` stays strict; the fallback slot
  is tool-only to avoid cross-type splicing.
- **`setup` base-url cache** → the `/v1` env form is copied into the slot too, so
  the returned pointer is always the owned per-slot buffer; contract reworded.
- Docs/comments corrected (`wait_fd` contract, `agentc_ext.h` discover namespace,
  `40-tui.md` render-diff SGR reset, bash driver-step comment).

Added `tests/data/http_wscolon.mock` + a replay case for the new header
whitespace rejection.

Re-verified: `make check` (32/32 + extensions, no warnings), `e2e.sh`,
`mcp.sh`, `ext.sh`, `net.sh`, `make windows`, `make windows WIN_ARCH=arm64`.

## Round 3 — user-reported login/logout/model bug (pre-existing, not a regression)
Diagnosis: `agentc login openai` stored the OAuth credential but never changed the
active default provider, so the next `agentc` kept using a stale `setup.jsonc`
default (ollama-cloud); `/model` then changed only the model id, pointing the
ollama endpoint at an openai model (404); `logout` rejected any provider other
than anthropic|openai. None of these paths were touched by rounds 1–2.

Fixes:
- `agentc login <provider>` now writes `setup.jsonc` (merge-preserving) with that
  provider as `default_provider` and `default_model` cleared for rediscovery, and
  reports when `config.jsonc` pins `default_provider` (which wins) instead of
  silently writing an overridden file. New `agentc_config_setup_set_default()`.
- `agentc logout [provider]` accepts any provider id and clears both a
  subscription token and an `auth.jsonc` api key.
- TUI `/model <id>` is provider-aware: a model owned by another provider is
  refused with a restart hint; an unknown id is set with a warning.
- README updated.

Verified: `make check` (32/32 + extensions, no warnings), `e2e.sh`, `make windows`,
and an isolated-home run of `logout` for arbitrary providers.
