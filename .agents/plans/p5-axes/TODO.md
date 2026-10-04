# TODO — P5 axes (RPC v1, libagentc SDK, framed CBOR protocol)

Plan: P5-AXES — RPC v1 record layer, embeddable `libagentc` SDK, framed CBOR protocol + `agentc serve`/client
Status: DONE
Checkpoint: `5e99ca9` (user-approved snapshot; supersedes all earlier evidence)
Design contract: `.agents/design/60-rpc.md`, `.agents/design/61-protocol.md`, `.agents/design/70-sdk.md`; pointer `.agents/design/30-extensibility.md` §5

## Committed
<!-- append-only, oldest first; newest work at the bottom -->
- [x] Axis A0 — RPC v1 record layer frozen + `AGENTC_RPC_VERSION 1` — provenance: `5450465` · design/60 §1–2
- [x] Axis A1/A2 — record layer (hello/echo/codes/state/models/thinking/`get_commands`/queue cap/`agent_settled`) + acceptance-first/queued prompts — provenance: `a562357` · after: `22bdc38` (busy code) · design/60 §4–7
- [x] Axis A3/A4 — UI sink + status observer, `ui_response` drop, `rpc_test` golden + `rpc_check.py` in e2e — provenance: `1c7fd37` · after: `53730ad` (deterministic UI trigger)
- [x] Axis B0 — SDK seams (boot/memimpl/mbedtls-shim splits, `AGENTC_EXT_NO_LINKED`, `os_embed_init`, fatal/log hooks, reset table) — provenance: `da83cc9` · design/70 §2
- [x] Axis B1 — PIC `libagentc.a`, `agentc_sdk.h` (`AGENTC_SDK_ABI 1`), one-engine `src/sdk/sdk.c` — provenance: `e068f98` · design/70 §1–2, §4
- [x] Axis B2 — `examples/embed.c`, object/symbol deny-lists (`tools/check-lib.sh`), `make libcheck` — provenance: `3021dd6`
- [x] Axis B3 — `agentc.pc`, `make install-lib`, shared lib + version map, `70-sdk.md` — provenance: `95e11f5` · design/70 §5
- [x] Axis B4 — strict no-`app/` archive — provenance: `ad4c244` (policy/project→core) · after: `b408c50` (select→core), `8fec605` (runsession→core), `7102fbf` (`LIB_EXCLUDE`+`make libstrict`), `f9de462` (AR polish), `a858077` · design/70 §1
- [x] Axis C1 framed protocol — frame — provenance: `80c66b1` · after: `3f078f8` (CBOR codec + strict UTF-8 + JSON bridge), `c7d1863` (envelopes), `8dcc9c4` (unix/tcp transport + framing-neutral dispatcher, C-D2), `1b0d568` (`agentc serve`), `2d2834b` (client + `serve.sh`/`proto_client.py`) · design/61 §1–4
- [x] Axis C2 hardening — cancel + multi-client + bounded queue (C-D4/C-D7/C-D9) — provenance: `76c9609` · after: `e09c870` (`get_messages` paging), `6938361` (CBOR/frame fuzz), `cdbdaeb`/`f1c04ce` (Wine loopback-TCP fallback), `f40e895` (fatal poll error) · design/60 §5.5, design/61 §3/§5
- [x] Axis C2 UI dialogs (feature `ui`) — dialog envelopes — provenance: `a45d499` · after: `ccbd1f4` (parked dialogs + ABI prompt sink), `8197317` (client/TUI modal), `ea60736` (one-per-connection cap) · design/61 §6
- [x] Axis C3 transport — `--host`/`--remote` TCP + fail-closed token — provenance: `94512ef` · after: `2dec908` (RFC 6455 WebSocket), `5d75ba8` (attach/detach + event filter), `e394a47` (explicit reconnect + resync), `d85a6f4` (vendored-mbedTLS server TLS), `7afc4dd` (POLLHUP/POLLERR reap) · design/61 §3a–3b
- [x] Axis C3 multi-session registry (bounded 4, fail-closed `busy`) — per-session global rebinding — provenance: `618f94c` · after: `2d6f046` (event `session_id`), `98db8bd` (`new_session` hook), `a33cfc6` (client session selection), `0904327` (bounded registry), `381d464` (client subcommands + two-session tests), `c4e4307` (pump restore), `3886915` (docs) · design/61 §7.1
- [x] Multi-session concurrency — cooperative one-model-step interleaving — provenance: `6684e3e` · after: `9c26a84` (two-session interleave + session-scoped cancel tests) · design/61 §7.1
- [x] Post-council polish — pump-driven async SDK `submit` over the cooperative run API — provenance: `42bb56d` · design/70 §6
- [x] Docs/verification — Axis A/B/C1 status `a5cde4c`; C2 gate map `eec64e8`; C3 gate map `4d8d1b3`/`bef0619`; B4 docs `a858077`; chair verification `0ba2a85`; as-built index `182f14c`; conc record `7ecd79c`; final verification `ec8d522` — provenance: `a5cde4c`

## Uncommitted
- [ ] none — every P5 axis is committed; cooperative multi-session interleaving landed, true parallelism stays deferred.

## Deferred
- [ ] true parallel multi-session execution — a single model step is not preemptible and the shared process-global registries cannot serve two sessions at once; design/61 §7.1
- [ ] platform sandbox backends (Windows/macOS/Linux) — out of scope per chair decision P5-D3 (run record removed; checkpoint `5e99ca9`)
- [ ] macOS `libso`/archive branch — implemented in the Makefile but unexercised (no macOS SDK); design/70 §1/§5
- [ ] byte-string payloads, canonical CBOR key ordering, full-range u64 > 2^53−1, f16/f32, tags, indefinite lengths, TLS-free archive — fail-closed profile limits; design/61 §7, design/70 §6
- [ ] Chord-like service routing and auto-reconnect — demand-gated (reconnect is explicit); design/61 §5a/§7
- [ ] shared-library ABI policy and Windows dynamic SDK packaging — demand-gated; design/70 §5
