# P5 — full plan: RPC contract, embeddable SDK, framed CBOR protocol

Status: **as-built (Axes A, B and C, all gate-green).** The frozen as-built
contracts live in `.agents/design/60-rpc.md` (RPC v1 record layer),
`61-protocol.md` (the framed CBOR protocol, `agentc serve`/`agentc-client`, and
the cooperatively interleaved multi-session registry) and `70-sdk.md` (the
`libagentc` SDK and
its packaging); `.agents/design/30-extensibility.md` §5 is the pointer into
them. The implementation status log, gate results and residual list are in
`.agents/plans/p5-axes/TODO.md`. The planning-seat reports and chair
decisions below are the frozen record and are **not** rewritten.

Commit map (baseline before the council: `af41704`, P6 complete):

| Wave | Commits | Outcome |
| --- | --- | --- |
| A — RPC v1 | `5450465` A0 doc+version · `a562357` A1/A2 record layer + acceptance/queue · `1c7fd37` A3/A4 UI sink + `rpc_test`/`rpc_check.py` · `53730ad` determinism fix · `22bdc38` busy code | `--mode rpc` v1, `AGENTC_RPC_VERSION 1` |
| B — `libagentc` SDK | `da83cc9` B0 seams · `e068f98` B1 archive + `agentc_sdk.h` · `3021dd6` B2 `examples/embed.c` + `make libcheck` · `95e11f5` B3 packaging + `70-sdk.md` · `4c0ca5b` docs | `make lib`/`libcheck`/`install-lib` |
| B4 — strict archive | `ad4c244` policy/project → core · `b408c50` `src/core/select.c` · `8fec605` `src/core/runsession.c` · `7102fbf` `LIB_EXCLUDE` + `make libstrict` · `f9de462` gate polish | no `src/app/*.o` in the archive |
| C1 — frame/CBOR/serve/client | `80c66b1` frame · `3f078f8` CBOR codec + JSON bridge · `c7d1863` envelopes · `8dcc9c4` unix/tcp transport + dispatcher split · `1b0d568` `agentc serve` · `2d2834b` client + `serve.sh` | framed protocol + serve + client |
| C2 — hardening + UI dialogs | `76c9609` cancel/queue/multi-client · `e09c870` `get_messages` paging · `6938361` CBOR/frame fuzz · `cdbdaeb`/`f1c04ce`/`f40e895` Wine/poll · `a45d499`..`ea60736` UI dialogs | bounded, fail-closed serve |
| C3 — remote/WS/TLS + multi-session | `94512ef`..`7afc4dd` remote/WebSocket/TLS, attach/detach, reconnect/resync · `618f94c`..`3886915` bounded multi-session registry | up to 4 engines per process |
| C3 — multi-session concurrency | `6684e3e` resumable one-step runs + interleaved `serve` slices · `9c26a84` two-session interleave/cancel tests | runs interleave at model-step safe points |

Multiple sessions per process is landed as a bounded (4) registry whose runs
interleave cooperatively at model-step safe points; true parallelism is not
possible while the shared process-global registries exist (constraint in
`.agents/design/61-protocol.md` §7.1).
Supersedes the “P5 option” discussion;
integrates the three planning-seat reports (RPC+SDK, protocol+transport,
CBOR codec) with chair decisions. Feed into
`.agents/design/30-extensibility.md` and the design record when implementation lands.

> Baseline note: this plan was written against `6330a82` with a 60-file WIP;
> the tree baseline described in §0 is historical and HEAD has since moved on.

## 0. Baseline and preconditions

- Implementation must start from a **frozen clean commit**. At plan time the
  working tree carries **60 modified files** of unrelated WIP (TUI, tools,
  providers, tests, goldens; last write 12:39) on top of `6330a82`; the P5
  plan reads that tree but must not be built on it. Decide: commit, stash, or
  branch the WIP first (chair/user call).
- The P5-A dispatcher extraction freezes the P5-A schema as implemented in
  `src/app/mode_rpc.c`; if that WIP touches it, land the WIP first.
- Linux is the primary local target; macOS is cross-compile + CI runtime;
  Windows is Wine-verified where possible.

## 1. The three axes

| Axis | Problem it solves | pi analog |
| --- | --- | --- |
| **A — RPC v1** | language-independent control of a spawned agentc process | `pi --mode rpc` (JSONL over stdio) |
| **B — `libagentc` SDK** | hosts embed the engine in-process | `createAgentSession` SDK |
| **C — framed protocol** | long-lived session server + typed client | `pi-protocol`/`pi-client`/`pi-server` (experimental) |

One command/event **vocabulary**, two framings (JSONL for A, framed CBOR for
C), one dispatcher.

## 2. Frozen decisions (chair)

### 2.1 RPC (A)

- **A-D1 dual discriminator**: `{"type":"prompt",…}` canonical;
  `{"type":"command","command":"prompt",…}` and bare `{"command":…}` accepted
  (legacy); rule: a string `type` that names a known command (except
  `command`/`response`/`ui_response`) is the name, else `command` is.
- **A-D2 acceptance-first**: `prompt`/`prompt_template` respond
  `{"disposition":"started"}` before submitting; the run outcome arrives as an
  `agent_settled` event (`completed|aborted|error`). Post-acceptance failures
  never produce a second response.
- **A-D3 queued disposition**: while a run is active, respond
  `{"disposition":"queued"}`, remember the ack, run it after settle.
- **A-D4 responses gain `command` echo + machine `code`**; `id` is number or
  string, absent ⇒ `null`; unknown command is a non-fatal `unknown_command`.
- **A-D5 keep the existing `new_session` cancel shape** (`success:false` + message),
  documented as a divergence from pi’s `cancelled:true`.
- **A-D6 thinking**: keep numeric `thinking_level`, add
  `thinking_level_name`; accept both; reject `minimal/xhigh/max` (0/1/3/4 only).
- **A-D7 fix the 32-model cap** in `get_available_models` (count-first, add
  `total`, `truncated` only on OOM).
- **A-D8 add `get_commands`** (`name`,`description`,`hint`,`source`) from the
  prompt registry + extension commands; needs `source` appended to
  `AgcPromptInfo`.
- **A-D9 defer** bash streaming/`abort_bash`/`excludeFromContext`,
  `compact.customInstructions`/`summary`, session naming, images, steering.
- **A-D10 pending cap 256 records / 1 MiB**; stop reading stdin when full;
  `busy` only as a last resort; never silent loss.
- **A-D11 UI fire-and-forget now**: `ui_request` `notify`/`setStatus`/
  `setTitle`; `ui_response` parsed and dropped when no dialog exists.
  Dialogs (`select|confirm|input|editor`) are defined in the spec but
  feature-gated and deferred (needs ABI dialog slots + a blocking pump).
  *(As-built: the dialogs landed in C2 — `src/app/mode_serve.c` and
  `.agents/design/60-rpc.md` §6; see the status and commit map at the top of
  this file. The deferral text is the frozen planning record.)*
- **A-D12 canonical doc**: new `.agents/design/60-rpc.md`; §1.2 of
  `.agents/design/30-extensibility.md` becomes a pointer.

### 2.2 SDK (B)

- **B-D1 archive composition**: `build/libagentc.a` includes base/core/ext/
  prov/wire/net/plat + shared app services (`mode.c`, `setup.c`, `policy.c`,
  `project.c`); excludes `_start`/boot, `main.c`, `mode_json.c`,
  `mode_rpc.c`, `tui/**`, mocks, the libc-named shims, and the linked-table
  variant. **Landed (B4):** the split moved provider selection to
  `src/core/select.c` and the run/session mechanics to `src/core/runsession.c`
  (`policy.c`/`project.c` to core too), so the strict archive contains no
  `app/` object. See `.agents/design/70-sdk.md` §1.
- **B-D2 one engine / one SDK agent per process** (`-EBUSY` otherwise),
  re-init after shutdown supported; explicit reset list in the docs.
- **B-D3 PIC archive** (`-fPIC` object set), proven by a default-flag PIE
  link in `make libcheck`.
- **B-D4 TLS in the main archive** (no-TLS variant deferred).
- **B-D5 seams**: split boot (`src/plat/<os>/boot.c`) and libc-named shims
  (`src/base/memimpl.c`, `third_party/mbedtls_shim.c`); add
  `os_embed_init(argv,envp)` and `agentc_fatal_install(fn,ud)`.
- **B-D6 packaging now**: `make lib`, `make libcheck`, `make install-lib`,
  `agentc.pc`; shared lib + version script + CMake wrapper in B3.

### 2.3 Protocol (C) and codec

- **C-D1 native CBOR payloads constrained to strict JSON** — pi parity:
  `params`/`result`/`event` are CBOR values (maps/arrays/scalars) validated
  by the strict-JSON predicate, **not** CBOR text strings containing JSON.
  Consequence: a JSON↔CBOR bridge is required in C1 (small: agentc’s JSON
  layer is integer-only — no float text conversion on the wire path; F64
  payloads are rejected `unsupported` in v1, documented as a pi divergence).
- **C-D2 one vocabulary, two framings** via an extracted
  `src/app/rpc_dispatch.{c,h}`; `--mode rpc` output stays byte-identical
  (golden proof).
- **C-D3 Unix-domain socket primary, loopback-TCP fallback, no named pipes,
  no TLS in v1**; socket `0600` in a `0700` dir, optional `--token`.
- **C-D4 connection = attachment; bounded multi-session registry (C3);** every
  connection is attached to session 0 by default; `new_session` creates/selects
  a session (bounded at 4, fail-closed `busy`) and moves the requester to it;
  `attach` selects the session + event filter; `detach` releases the attachment
  (not the session); events carry `session_id` and broadcast to that session's
  attachments; each session's active run is advanced one bounded model-step
  slice per serve loop iteration, with its own request queue and session-scoped
  `cancel`/`abort` (61-protocol.md §7.1).
- **C-D5 `agentc serve` subcommand**, default `$XDG_RUNTIME_DIR/abird/agentc/serve.sock`,
  ready JSON line on stdout with transport/path/server_id/pid/token_file.
- **C-D6 exact version match; additive `features` array** (`cancel` in v1;
  `ui` in C2; `attach` in C3); unknown features ignored.
- **C-D7 envelope violations are terminal for the connection**; slow-consumer
  overflow closes that connection.
- **C-D8 client is synchronous with explicit reconnect**, no auto-replay;
  resync via `get_messages`.
- **C-D9 `cancel` is an out-of-band envelope**; `abort` remains a method.
- **C-D10 unknown-property rejection at the envelope level only**; payload
  semantics follow the P5-A vocabulary.
- **C-D11 UI envelope schemas defined, emitted only under feature `ui`.**
- **C-D12 limits**: 16 MiB frame/payload, 64 CBOR depth, 1M items, 64-byte
  ids/methods, 8 connections, 32 MiB pending.
- **Codec decisions**: own `AgcCbor` DOM (not `AgcJson`) because agentc has no
  binary64↔text conversion and CBOR floats are bit-exact; byte strings kept
  in the codec and rejected by the protocol profile; pi-safe integer range by
  default with an opt-in full range; `u32` BE frame prefix with
  validate-before-copy and sticky failure; `max_items = 2^20` as intentional
  hardening beyond pi.

## 3. Axis A — RPC v1 (implementation sketch)

- `src/app/mode.h`: `AGENTC_RPC_VERSION 1`.
- `src/app/mode_rpc.c`: hello (protocol/version/commands/capabilities),
  `command` echo + `code` errors, `get_state` additions (`protocol`,
  `thinking_level_name`, `session_file`, `is_streaming`,
  `pending_commands`, `auto_compaction`), `get_commands`, models fix,
  thinking names, queue ack flag + cap, `agent_settled`, UI sink + a new
  `agentc_ext_set_status_observer` in `ext.h`/registry, canonical `bash`
  param.
- `src/core/prompts.{h,c}`: append `source` to `AgcPromptInfo`, fill it.
- Tests: new `tests/rpc_test.c` (deterministic trace golden) covering every
  command/error/id/queue/acceptance/UI case; update `modes_test` for
  acceptance; new `tests/rpc_check.py` e2e subprocess driver in `e2e.sh`.
- Docs: `.agents/design/60-rpc.md` (framing, version policy, families, command
  table, events, UI, codes, ordering/backpressure, pi divergences).

## 4. Axis B — `libagentc` SDK (implementation sketch)

- **Seams (B0)**: `src/plat/<os>/boot.c` split; `src/base/memimpl.c` split of
  the libc-named `memcpy/memmove/memset/memcmp`; `third_party/mbedtls_shim.c`
  split of libc/compiler-rt-named shims; `AGENTC_EXT_NO_LINKED` guard in
  `src/ext/linked.c`; `os_embed_init` (plat.h); `agentc_fatal_install`
  (agentc.h); `agentc_config_defaults` if needed.
- **Build (B1)**: `LIB_*` object set with `-fPIC`, `build/libagentc.a`,
  `AR rcsD`; `make lib`.
- **Header (B1)**: `include/agentc_sdk.h` — `AGENTC_SDK_ABI 1`,
  `AgcSdkConfig` (cwd/homes/argv/envp/config/load flags/startup pump/log/
  fatal), `agentc_sdk_init/shutdown/version/host`, `AgcSdkAgentOptions`,
  `agentc_sdk_agent_new/free/agent/session`, transport/events/observer/tools
  injection, `submit`/`abort`/`pump`/`new_session`/`transcript`/`last_error`,
  count-first `tools`/`providers`/`provider`/`needs_key`.
- **Implementation**: `src/sdk/sdk.c` wraps an `AgcModeCtx` (first member of
  `AgcSdkAgent`) and composes exactly like `main.c` (config → extensions →
  context → trust/resources → tools → bounded pump) with security-first
  defaults (`offline=true`, `no_mcp=true`, `load_user_config=false`).
- **Global-state contract**: one engine; reset table (ext registry, models,
  prompts with a new `agentc_prompts_reset`, status, auth/OAuth, setup
  context, events, pump/poll hooks, JSON arena); explicitly non-reset
  (TLS globals, preset provider rows, log level, env); host owns signals/
  stdio/terminal.
- **Gate (`make libcheck`)**: archive deny-list; symbol deny-list via
  `tools/check-lib.sh` (no `memcpy`/`calloc`/`exit`/`_start`…); link
  `examples/embed.c` against the archive with default PIE flags; run it and
  `cmp` the golden; a second init/shutdown cycle with a stable
  `agentc_mem_live()`.
- **Packaging (B3)**: `agentc.pc`, `install-lib PREFIX=`, docs
  `.agents/design/70-sdk.md`, shared lib + version map (`agentc_*` exported,
  `local: *`), mac archive.
- **B4 (built)**: split `setup.c`→`src/core/select.c` (provider selection,
  credentials, model filter, `provider_names`), `mode.c` run/session
  mechanics→`src/core/runsession.c`, and moved `policy.c`/`project.c` to core;
  `setup.c` keeps live discovery/onboarding and `mode.c` keeps the TUI notice
  replay. `LIB_EXCLUDE` now drops all of `src/app/%`, and `make libstrict`
  proves the strict no-app archive still links/runs `examples/embed.c`.

## 5. Axis C — framed protocol (implementation sketch)

- **Frame** (`include/frame.h`? see codec plan; `src/wire/frame.c`): 4-byte BE
  length + payload; streaming decoder with fragmentation/coalescing,
  validate-before-copy, sticky failure, 16 MiB cap.
- **Codec** (`include/cbor.h`, `src/base/cbor.c`): `AgcCbor` DOM + arena,
  strict-UTF-8 validator, decoder (definite-length only; major 0/1/2/3/4/5/7;
  no tags/indefinite/f16/f32/simple; duplicate text keys rejected; finite f64;
  pi-safe integers by default), streaming writer with minimal-width args and
  framed init, limits (`AGENTC_CBOR_*`), `agentc_cbor_is_json`.
- **Bridge** (`src/base/cbor_json.{c,h}` or inside `cbor.c`): JSON text ↔
  `AgcCbor` value for the integer/string/bool/null/array/object vocabulary
  (the dispatcher stays `AgcJson`-based); F64 ⇒ `unsupported` in v1.
- **Envelopes** (`include/protocol.h`, `src/wire/proto.c`): `hello`/`hello_ack`/
  `hello_error`, `request{id,method,params}`, `response{id,ok,result|error}`,
  `cancel`, `event{seq,event}`, reserved `ui_request`/`ui_response`; exact
  version 1; `server_id` UUID; closed error-code set; unknown-property
  rejection; per-connection pending cap.
- **Transport** (`include/net.h` + `src/net/<os>/unix.c`): `unix_listen`,
  `unix_connect`, `tcp_listen`, `accept`, `local_addr`, `unlink_socket`;
  `plat.h` `os_chmod`/`os_getuid`; Windows AF_UNIX with TCP fallback; stale
  socket recovery.
- **Serve** (`src/app/mode_serve.c` + `agentc serve` in `main.c`): poll loop,
  accept, per-connection decoder/outbuf, broadcast via the injected mode
  `write`, `agentc_mode_pump`, `agentc_rpc_handle`, queue, shutdown/draining,
  ready line, token auth, perms.
- **Dispatcher** (`src/app/rpc_dispatch.{c,h}`): `AgcRpc` over `AgcModeCtx`,
  framing-neutral sinks; JSONL keeps byte-identical output.
- **Client** (`include/agentc_client.h`, `src/wire/client.c`,
  `tools/agentc-client.c`): connect/handshake, `request` (pump events),
  `cancel`, `pump`, explicit `reconnect`, server/session/feature accessors;
  tool subcommands `prompt`/`events`/`call`/`state`/`cancel`.
- **Interop**: `tests/proto_client.py` (stdlib socket + minimal CBOR) is the
  cross-language proof and part of `tests/serve.sh`.
- **Phasing**: C1 frame+codec+bridge+envelopes+unix+serve+client; C2 cancel
  fully wired, UI dialogs (feature `ui`, ABI dialog slots, `ui_prompt_*`
  points), multi-client hardening, Windows AF_UNIX/Wine, fuzz, `get_messages`
  paging; C3 WebSocket/TCP-remote/TLS, attach/detach routing, multiple
  sessions, reconnect policy.

## 6. Ordering and workstreams

| Wave | Content | Depends |
| --- | --- | --- |
| **A0** | 60-rpc.md + version constant | frozen base |
| **A1** | record layer (hello/echo/codes/state/models/thinking/get_commands/queue cap/settled) | A0 |
| **A2** | acceptance-first + queued dispositions + `modes_test` update | A1 |
| **A3** | UI sink/status observer + `ui_response` handling | A1 |
| **A4** | `rpc_test` + `rpc_check.py` + e2e | A1–A3 |
| **B0** | boot/memimpl/shim splits, `NO_LINKED`, fatal/env seams | frozen base |
| **B1** | `make lib` + `agentc_sdk.h` + `src/sdk/sdk.c` | B0 |
| **B2** | `examples/embed.c` + `make libcheck` + CI | B1 |
| **B3** | `.pc`, install, 70-sdk.md, shared lib+map, mac | B2 |
| **C1** | frame + cbor + bridge + envelopes + unix transport + serve + client | A1 (dispatcher), B1 (serve links the lib) |
| **C2** | cancel, UI dialogs, multi-client, Windows/Wine, fuzz, paging | C1 |
| **C3** | remote/WebSocket/TLS, attach routing, bounded multi-session | C2 + demand |

Parallel-safe: A and B0/B1 (disjoint except `Makefile`/`agent.h`), then C1
depends on A1 and B1. Worktree isolation for `Makefile`, `main.c`,
`mode_rpc.c`, `mode.h`, `include/agent.h`.

## 7. Tests and CI

- New goldens: `rpc_test`, `frame_test`, `cbor_test`, `proto_test`, `embed`
  (from libcheck); `modes_test` byte-identical through the dispatcher
  refactor; `fuzz_test` extended with CBOR/frame seeds.
- e2e: `rpc_check.py` (stdio subprocess), `serve.sh` + `proto_client.py`
  (unix socket, token, perms, cancel, session switch, TCP fallback where
  applicable), `make win-check`/Wine for the golden binaries and best-effort
  serve.
- CI: Linux `make check` + `make client && serve.sh` + `make libcheck`;
  macos `serve.sh` + dylib path; windows golden suite + `serve.sh --wine`
  (skip-clean); cross/qemu goldens unchanged.

## 8. Risks and blockers

1. **Dirty baseline** (§0) — must freeze first.
2. `_start`/env/fatal paths block native hosting (B0 seams).
3. libc-named symbols in `str.o`/mbedtls glue would interpose a libc host
   (B0 splits + deny-list gate).
4. Non-PIC core objects cannot link into a PIE host (B1 PIC archive).
5. Generated linked table baked into the archive (`NO_LINKED` variant).
6. Process-global registries prevent N engines (documented one-engine contract;
   isolation deferred).
7. The CBOR codec is the highest-risk artifact (~700 lines + ~200 frame);
   RFC/pi vectors + fuzz + allocator baselines are mandatory.
8. The JSON↔CBOR bridge (C-D1) is required for the vocabulary; keep it
   integer-only on the wire path and reject F64 payloads in v1.
9. Windows AF_UNIX behavior is Wine-version dependent; TCP fallback is the
   safety net (OC).
10. Synchronous server ⇒ cancel/dialog latency bounded by poll slices: the
    serve poll hook must always be installed.
11. Two writers on one session file remain unsafe (pre-existing); v1 serve
    adds no locking (candidate for `os_flock` in C2).

## 9. Open decisions (ratified unless noted)

A-D1…A-D12, B-D1…B-D6, C-D1…C-D12, and the codec decisions above are
**accepted** as written. Remaining to confirm during implementation:

- C1 `serve.sh` Wine AF_UNIX spike before freezing the Windows test path
  (fall back to TCP in Wine).
- `get_messages` paging in C2 (C1: 16 MiB cap + explicit `too_large`).
- Slow-consumer policy (close in v1; gap-marker drop only if demanded).
- Ship `agentc-client` in `dist`/`install` (recommended yes).
- Float payload support (deferred with `num.c`; reject `unsupported` in v1).

## 10. Deferred / demand-gated

*(As-built pointer: the C3 remote/WebSocket/TLS, attach routing and bounded
multi-session registry all landed, with cooperative interleaving at model-step
safe points — see the status and commit map at the top of this file and
`.agents/design/61-protocol.md` §7.1. The entries below are the frozen
planning-record of what was demand-gated at the time.)*
pi-native extras: auto-reconnect remains out of scope for now (reconnect is
explicit in C3; see 61-protocol.md §5a), plus true parallel multi-session
execution (sessions are independent, bounded at 4, and interleave cooperatively
at model-step safe points, but a step is not preemptible and the shared
process-global registries cannot serve two sessions at once; 61-protocol.md
§7.1),
Chord-like service routing, byte-string payloads in the profile, canonical CBOR
key ordering, full-range u64 beyond 2^53−1, f16/f32, tags, indefinite lengths,
TLS-free archive, shared-library ABI policy, Windows dynamic SDK packaging.
