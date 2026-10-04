# TODO — review-p8

Plan: Resolve the fresh P8 adversarial review of `aed30db` — findings H1-H3, M1-M11, L1-L11.
Status: DONE
Checkpoint: `5e99ca9` (user-approved snapshot; supersedes all earlier evidence)
Design contract: `.agents/design/20-core-agent.md` (helper seam: `.agents/design/30-extensibility.md` §2.1)

## Committed
<!-- append-only, oldest first; newest work at the bottom -->
- [x] Reviewed base — P7 refactor; prior `fix(p7)` fresh-review resolution `fb0f433`; P6/P5-AXES review folded into this plan (run record removed; checkpoint `5e99ca9`) — provenance: `aed30db` · after: `28a3238`, `81a01e7`
- [x] Chair decisions P8-D1..P8-D27 (per-agent routing, alias-is-a-fact, one transport rule, writer safety, docs/ownership) — pointer only (run record removed; git history at `5e99ca9`); not reproduced — provenance: `81a01e7` · after: `dbd68ec`
- [x] Workstream A — helper isolation + MCP stream/wire (A-H1/H2/H3, A-L1/L6/L7/L9, A-M10/M11): deadline-at-loop-top, `os_set_nonblock` pipe rule, head-valid gate, split GET/POST event ids, BOM/NUL SSE — provenance: `3d0b81f` · after: `4f08f76`; helper-seam status in `30-extensibility.md` §2.1
- [x] Workstream B1 — per-agent route/owner seam + session/queue switches (B-M1/M3/M4/M6/M7/M8/M9; P8-D1/D2/D3/D4): alias fail-closed, same-provider credential parity, cross-provider child keys, clear-then-replay queue — provenance: `85d9201` · after: `1d4eb0f`
- [x] Workstream B2 — config/auth writer safety + alias discoverability + atomic create (B-M2/M5a/M5c, B-L2/L3/L4/L5/L10/L11; P8-D7/D19/D20/D22/D23): `setup_root_ok`, sibling-row merge, `OS_O_EXCL` retry — provenance: `1d4eb0f`
- [x] Workstream C — alias-aware `/model` + picker, usage/README/CHANGELOG/design sync (C-M5b, C-L8; P8-D24/D25/D26/D27) — provenance: `dbd68ec` · after: `34e1643`
- [x] Council close — every numbered finding resolved or explicitly deferred — provenance: `36dce35` · after: `5e99ca9` (checkpoint)

## Uncommitted
<!-- the working list; the only place new work appears -->
- [ ] None open — every numbered finding is fixed; platform/environment limits are accepted under Deferred.

## Deferred
<!-- accepted, fail-closed, with the reason; never silently dropped -->
- [ ] P8-D27 `include/agent.h` comment reword (`route_provider`/`route_model` authoritative) — documented comment-only `-`/`+` exception to append-only; landed `dbd68ec`, recorded in the council/ABI notes.
- [ ] Platform sandboxes + environment-blocked verification (Windows restricted token, macOS Seatbelt, Linux host allowlist; no macOS SDK / real Windows host) — all fail-closed; `design/10-platform.md` §6.
- [ ] Windows PTY `os_pty_open` stays `-ENOSYS`; `tty:true` actors are Linux/macOS-only (docs corrected, code not).
- [ ] Windows pipe write end cannot be non-blocking portably (P8-D9) — one ≤4 KiB chunk may block on a hung child; child death still surfaces `-EPIPE`.
- [ ] R2 same-class sinks: `agentc_trust_set_audit` + `agentc_ext_set_entry_sink` remain process-global session pointers — follow-up: widen the seams with an owner/actor id.
- [ ] R5 startup `mode_session_create` still accepts a degraded memory-only session; R7 a direct non-alias step with no key still reaches the provider (startup gate).
- [ ] A-L7 helper seam has no production entry (SDK embedder vs `helpers` config section); A-M10 malformed GET head retried behind capped backoff; A-M11 POST-stream id kept write-only.
- [ ] One-model-step concurrency (documented `design/61-protocol.md` §7.1); `INLINE-RESIZE-DESIGN.md` and true parallel multi-session remain open (`/fork`/`parent_session`/`/tree` landed later, `8a6d62f`).
