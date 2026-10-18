# fc → agentc — architecture specs and port plan

Decision (revised): **agentc survives.** fc is retired after donating its runtime/core
architecture and M8 hardening. This document is in three parts:

- **Part A — fc specification** (donor, post-M8): the architecture and mechanisms to port.
- **Part B — agentc specification** (base): the architecture and seams the ports land on.
- **Part C — port plan**: per-subsystem target designs, ordered batches, verification, risks.

Sources: fresh deep reads of both trees at `fc` HEAD `5fbb874`/`cc4e132` region and `agentc`
`ca28ba1`. Claims cite files and were taken from four independent read-only reviews plus a
dedicated reversed-port architecture analysis.

---

# Part A — fc specification (donor)

## A.1 System shape

Freestanding C23 coding agent, no libc, static. One portable archive plus link-time-selected
platform/network archives: `build/libfc.a` holds every OS-free translation unit; the shipped
binary links `libplat_linux.a` + `libnet_linux.a`, tests link `libplat_headless.a` +
`libnet_mock.a` (or `libnet_linux.a` for loopback) (`fc/Makefile:120-124,227-284`). Layers
flow strictly downward: `plat` (kernel surface, no allocator) → `net` (sockets/DNS/TLS) →
`wire` (the only socket consumer; live/record/replay/memory byte streams) → `base`
(allocator, arenas, buffers, JSON, formatter, limits, output sanitizer) → `core`
(transcript, turn loop, tool registry/job driver, sessions, config/auth/compaction) →
`prov` (per-API request/SSE adapters behind a table) → `ext` (events/plugin/MCP) → `tui` →
`app` (CLI + four modes over one setup handle).

## A.2 Module contracts

| Header | Owns |
|---|---|
| `include/fc.h` | identity/version/entry only (`fc.h:5-23`) |
| `include/fc_plugin.h` | stable C99 plugin ABI: append-only structs, `abi_version`+`struct_size`, host-owned memory, one exported `FC_PLUGIN_ENTRY` (`fc_plugin.h:1-46`) |
| `src/plat/plat.h` | Layer 0: files/process/tty/time/poll; negative errno; `plat_os_name/plat_home/plat_is_abs`, `fc_spawn_group`, test hooks (`plat.h:146-206`) |
| `src/net/net.h` | sockets/DNS/TLS with absolute `uint64_t deadline_ns`; `tls_*` mbedTLS shim (`net.h:49-51`) |
| `src/wire/wire.h` | live/record/replay/memory transports + fault hooks; HTTP/SSE/URL/record/retry |
| `src/base/*.h` | `mem` (size classes + arenas), `buf` (SB/VEC), `rt`, `str` (`fc_fmt`/`fc_fmt_buf`), `utf8`, `json` (explicit arena), `jsonw`, `fs`, `env`, `log`, `loop` (plugin defer pump), `dir`, `out` (terminal-safe), `sha256` (PKCE), `limits`, `deadline` |
| `src/core/*.h` | `types` (msg/transcript), `agent` (run/abort), `session`, `config` (JSONC+trust+MCP list), `compact`, `catalog`, `auth`/`oauth`, `prompt`, `tools`+`jobs`+`walk`+`diff_text` |
| `src/prov/provider.h` | `{api, ops}` table + shared stream lifecycle + `void *priv` (`provider.h:55-83`) |
| `src/ext/*.h` | `events` (fan-out), `plugin` (static registry/host/adapter/veto), `mcp` (stdio) |
| `src/tui/*.h` | `frame` (cell grid + diff), `editor`, `input`, `term`, `theme`, `md`, `diff`, `tui` |
| `src/app/*.h` | `cli`, `setup` (one owned handle + mode policy), `headless`, `events_json`, `main` |

## A.3 M8 mechanisms (the port's payload)

| Mechanism | Where | What it guarantees |
|---|---|---|
| Output safety | `base/out.{h,c}` (`out_cp_filter`, `out_safe_write`, `out_safe_sb`); `tui/frame.c:118-165` | no renderer or terminal writer can emit C0/C1/DEL; JSON is pure ASCII (`jsonw.c:51-90`) |
| Absolute deadlines | `base/deadline.h` | one `deadline_t` passed down, never re-armed (**currently unused — see A.10**) |
| Caps at append | `base/limits.h`; HTTP headers `http.c:366-373`, SSE event `sse.c:117`, tools/TUI | remote input cannot grow buffers without bound; truncation is a parser feature |
| Process lifetime | `fc_spawn_group` (`plat/linux/sys.c:569`), non-blocking reap (`tools/bash.c:25-40,182-210`, `jobs.c:193-228`, `mcp.c:492-529`) | no blocking waits; process-group kill; bounded SIGKILL escalation |
| Ownership by construction | `app_setup_t` (`app/setup.h:20-31`) | config/model/prompt/session/API key owned by the mode handle |
| One mode setup | `app_mode_setup/teardown` (`app/setup.c:25,189`) | four modes share config/auth/prompt/session/abort; only policy differs |
| Event fan-out | `events.c:13-17,138-194` | primary sink + 7 subscribers; `ev_quiet` suppresses all incl. plugin bridge |
| Tables over chains | provider registry (`provider.c:15-71`) + `priv` (`provider.c:123-152`) | adding a provider is one file + one table row |
| One iterator/DOM | `base/dir.c`, `core/tools/args.c` | one dirent iterator; one tool-args parser for all builtins |
| Platform identity | `plat_os_name/home/is_abs`, `/` normalization (`plat.h:146-160`) | core stops assuming `/` and `$HOME`; prompt says the real OS |

Regression evidence: every mechanism has named golden tests; critical fixes (terminal
sanitizer, C1 tool closure, C4 auth merge, L1/L2 bash, L6 TUI flush) carry revert-fails
mutation proofs (`.orchestrator/fc-m8/evidence/t4-mutation-matrix.md`).

## A.4 Agent loop and data model

- `msg_t`: role + text SB + `tool_call_t[]` (`call_id[80]`, `name[64]`, args SB) + result
  `call_id` + stop reason + error flag (`core/types.h:25-38`); transcript is a `VEC` with
  `bytes` for compaction; `transcript_splice_front` for compaction.
- `agent_run` resets abort, resolves endpoint, opens **one wire for the whole run**, loops
  turns; compaction may mark the wire stale and force reconnect (`agent.c:453-525`).
- `run_turn` → `send_stream` builds the request via `ops->build_request`, sends with
  `http_fetch_carry`, checks 200, feeds `ops->stream_event`, carries leftovers.
- **Exactly-once tool closure:** a call that cannot run (failed/aborted/truncated turn)
  gets a synthetic error tool result, so `--continue` can never replay a poisoned
  transcript (`agent.c:352-368`).
- Tools run through a 32-slot job table; async tools start first, then the parallel poll
  loop drives `step`, checks abort/deadlines, fires completions in completion order; >32
  calls are chunked (`tools/jobs.c:292-450`).
- Abort: `static volatile int`, reset at each run, checked in SSE loop, job checkpoint and
  driver (`agent.c:15,19-22,191-196`).

## A.5 Providers, sessions, events, plugins, MCP

- Provider ops: `{id, api, path, build_request, headers, stream_init, stream_event,
  stream_free}`; adapter state behind `priv`; registry lazy-inits builtins and rejects
  duplicates/overflow (`provider.c:15-71`). Compaction is provider-neutral: a normal
  streamed request with `ev_quiet` (`compact.c`, agent `request_text`).
- Sessions: JSONL, `SESSION_VERSION` read and refused when newer, warn-once on malformed
  lines, append failure truncates to the recorded offset and degrades, torn tail truncated
  on open, `compaction` records replayed by reseeding the reconstruction (`session.c`).
- Events: typed `ev_t`, one primary sink + subscriber slots, quiet gate.
- Plugins: static link-time registry (weak table), tool structs validated and **copied**,
  failed init rolls back to a watermark, defer pumped after every dispatch,
  `session_shutdown` emitted; `fc_plugin.h` v1 has honest unsupported entries.
- MCP: stdio JSON-RPC child, process group, one absolute RPC deadline, 1 MiB line cap,
  `mcp__server__tool` sanitized naming with collision guard, config-owned servers,
  unregister-before-free.

## A.6 TUI

Cell grid + row-diff renderer (12 µs frames at 200×60); `tui_frame_put` sanitizes, handles
wide glyphs with continuation cells; codepoint-aware editor; input state machine with
bracketed paste and lone-ESC flush; SIGWINCH flag; scrollback with trimming; partial flush
on turn end verified in real ANSI bytes; headless plain transcript for golden tests.

## A.7 Build, tests, metrics

`fc/Makefile` (+ `build.sh`): release/debug/test/asan/fuzz/plugins/fmt, `WERROR=1`;
`tests/run.sh` with `UPDATE=1` goldens and per-test watchdog. **43/43 golden programs**,
ASan+UBSan+LSan 28 + plugin C-only, 6 fuzz harnesses run in CI, fault-injection matrix,
mutation matrix. Stripped binary 676,520 B (660 KiB), `--version` ~640 µs, frame 12 µs,
RSS ~11.0–12.0 MB (tight vs the 12 MiB budget).

## A.8 Platform/net reality

Linux x86-64 compiled, linked, run. macOS arm64 and Windows x86-64 **object-compiled only**
(10 TUs via `build.sh cross-check`), never linked/run; `-ENOSYS` for ConPTY, readlink,
fcntl, setpgid on win; hand-written `.def` import lists. One vendored mbedTLS shim on every
OS, portable DNS message parser, own resolver.

## A.9 Data/ownership conventions worth importing

Negative errno everywhere; caller-owned buffers; `mem_alloc` zeroed with a 16-byte header;
explicit `arena_t` per parse; SB/VEC growth rules; typed events at internal boundaries,
JSON only at external ones; session append typed in `agent.c` (ADR-41).

## A.10 Honest weak spots (do not port these as-is)

- `base/deadline.h` is dead code (no `.c` caller); deadlines are still raw `uint64_t` in
  most loops.
- Six `limits.h` macros are unreferenced; real caps live in local constants.
- `read` is bounded but whole-file; MCP is stdio-only and synchronous.
- mac/win unverified; RSS headroom tight; `docs/perf.md`/`docs/platforms.md` have drift.
- Plugin host is stricter than needed (`struct_size < sizeof` rejects older prefixes);
  defer is main-thread-only.
- Provider stop-reason vocabulary is provider-flavoured (`tool_use` vs `tool_calls`).
- No pty/ctty anywhere; `--resume` picker intentionally rejected.

---

# Part B — agentc specification (base)

## B.1 System shape

Freestanding C23, `-nostdinc` with vendored shim headers so a vendored library cannot reach
host libc (`agentc/Makefile:44-62`). One Makefile builds the product, tests, cross targets
and the plugin matrix; Nix wraps it for 7 release targets. Layers: `plat` (Layer 0) →
`net` (+mbedTLS on Linux, OS TLS on mac/win) → `wire` → `base` → `core`+`prov` → `ext` →
`app` → `tui`.

## B.2 Public surface

Twelve headers in `include/` are treated as frozen source contracts:
`agentc.h` (base library), `plat.h` (Layer 0), `net.h` (Layer 1), `wire.h`, `agent.h`
(messages/tools/providers/events/loop), `session.h`, `config.h`, `oauth.h`, `discover.h`,
`mcp.h`, `plugin.h` (orchestrator-facing), `agentc_plugin.h` (stable plugin ABI). Internal
helpers are declared as ad-hoc `extern` blocks in `.c` files or in two `*_int.h` headers.

## B.3 Platform layer

- `plat.h`: bootstrap (`os_init`, `os_platform`, `os_exit`, `os_getenv`), files, memory,
  processes, **shell contract** (`os_shell_kind/resolve/spawn_shell`), terminal, signals,
  time/entropy/poll (`plat.h:19-90`); `struct os_stat` with logical field order and
  `dirent64` interchange; Linux errno as the universal currency.
- Linux: per-arch syscall tables and `_start` stubs for **x86-64, aarch64, riscv64**;
  asm-generic differences absorbed in wrappers (`dup3`, `ppoll`, `clone`, `*at`,
  `renameat2`); naked signal trampolines (`plat/linux/syscall.h`).
- Windows: hand-declared `win.h` (no `windows.h`), 1024-entry fd table, `__chkstk` for
  x86-64 and arm64, wide argv/env, directory enumeration as `dirent64`, process table with
  `CreateProcessW`, **console reader thread** (input events → xterm bytes, resize → winch),
  `cmd`/`powershell`/`pwsh` resolution with loud missing-shell errors, `WSAPoll` +
  `WaitForMultipleObjects` merged poll (`plat/win/*`).
- macOS: libSystem + SDK headers only in platform files, Darwin→Linux errno table,
  `DIR*` fd table, Mach-O `_start` per arch, real `termios`, `arc4random_buf`
  (`plat/mac/*`).
- No pty/ctty on any port; shell output is pipes.

## B.4 net/wire

- `net.h`: non-blocking sockets, resumable TLS handshake, verification default ON.
- Linux TLS = vendored mbedTLS 3.6.2 behind a shim (`#embed` CA bundle, BIO hooks,
  verification + X.509 error strings, PSA core); macOS = SecureTransport with
  `SecTrustEvaluateWithError`; Windows = SChannel with `CertVerifyCertificateChainPolicy`.
- DNS hand-rolled on all three (Linux/mac duplicated; win `DnsQuery_A` + fallback).
- `wire` HTTP/SSE/JSON writer with a transport function-pointer injection seam
  (`AgcTransportFn`, `agent.h:180-195`); recording writes raw request and received bytes;
  no replay-file format.

## B.5 Agent loop and events

Single-threaded nested `for(;;)` turn loop, no explicit FSM; implicit states in
`stop_reason`; one overwriteable event callback `AgcEventFn(void*,int,const void*)`;
diff-based streaming emission (providers append, the loop emits the delta), cancel as
`const volatile bool *`, retry inside a turn (5 attempts, backoff + `Retry-After`),
cancelled/skipped tool calls back-filled with `"error: aborted"`, **strictly sequential**
tool execution (`core/agent.c`). Tool flags (`READONLY/DESTRUCTIVE/SEQUENTIAL`) and
`parallel_tool_calls` are propagated/advertised but never consulted (verified).

## B.6 Data model, tools, providers

- Transcript: growable array of messages, each with a growable block array; every string is
  a separate allocation; deep-copy on `agentc_agent_load` for `--continue`/RPC swaps.
- Tools: `{name,label,desc,params_json,flags, char *(*exec)(args,is_error,cancel)}` — **no
  userdata**, no async, result text owned; registry in one file; MCP exports via 256
  generated trampolines and plugins via 32; both ceilings hard.
- Providers: `AgcProvider` 6-field vtable; five static singletons; OpenAI-compatible
  names in an 8-slot static array; provider knowledge spread across `api_path()`, setup
  presets, auth env tables, discovery endpoints, compaction extraction, catalog and cost
  tables; compaction text-patches `"stream":true`→`false` and extracts by provider name.
- Catalog: 8 static model rows + 256-entry dynamic registry; hand-maintained cost table;
  runtime discovery (`discover.h`) with cache; `--list-models`/`--refresh-models`.

## B.7 Sessions, plugins, MCP

- Sessions: JSONL with `version:1` written in two places (core and JSON mode); loader loads
  only `type=="message"` (**compaction records are write-only**, verified `session.c:446`),
  malformed lines skipped silently; torn-tail repair via `need_newline`; per-cwd listing
  API `session_list`/`find_latest`; tool args preserved with `jsonw_raw` and recovered by a
  hand-rolled span scanner.
- Plugins: static-only registry via `__has_include` generated table (`gen-plugins.py`),
  per-field `struct_size` acceptance (`AGENTC_FIELD_OK`), atomic defer ring, host HTTP
  queue (8 in flight, header injection guard), events fan-out to plugins with veto
  convention, honest list of reserved-but-unemitted events; Rust crate mirrors the ABI —
  **but its `SEQUENTIAL`/`DESTRUCTIVE` constants are swapped vs the C header** (verified).
- MCP: stdio **and Streamable HTTP** (session id, SSE responses), `${ENV}` expansion,
  sanitized `mcp__server__tool` names with FNV suffix, paginated tool list with
  delete-unseen, stdin workaround for nonblocking read ends, memory-baseline shutdown.

## B.8 TUI, app, build

- TUI: cell grid with `CELL_CONT`/`CELL_INVALID`, differential fullscreen renderer, **inline
  scrollback as default** plus fullscreen alt-screen mode, streaming markdown (last block
  re-parsed), editor with kill ring/history (persisted) and TODO undo/selection, escape/paste
  state machine, **no mouse**, dual sanitization (grid store + emit), theme slots with
  16/256/truecolor, in-memory terminal backend for golden screens + real-pty e2e harness.
- App: CLI with onboarding (`agentc setup`), `--list-models`/`--refresh-models`/`--offline`
  etc.; modes wire themselves (no shared setup handle).
- Build/tests: Makefile arch matrix (`ARCH_FLAGS`, `WIN_ARCH`, `TEST_RUNNER`/`TEST_BINDIR`,
  `.target` stamps), `make check/wine-check/live-net/plugin-pipeline/bench/dist`;
  `flake.nix` builds 7 targets; CI runs golden under qemu (aarch64/riscv64) and Wine
  (x86-64, real cmd.exe; arm64 non-gating); tests: 22 golden programs + e2e/net/mcp/plugin
  shell suites + pty screen emulator; deterministic fuzz harness.

## B.9 Metrics

34k LOC; 75 commits; `.bss` small; ~734 KB binary (not directly comparable to fc's number:
measured differently), startup ~0.29 ms by its own bench; no libc; public headers 1,373
lines. Linux x86-64 run; aarch64/riscv64 golden under qemu; Windows x86-64 golden under
Wine; macOS arm64 golden on a native runner; never hand-driven on mac/win.

## B.10 Honest weak spots (the port fixes these)

Tool identity/trampolines and ceilings; sequential-only tools with decorative flags;
global JSON arena; provider switches + name-coupled compaction; compaction records never
replayed; fixed-capacity registries that drop silently; no pty; DNS triplicated; Rust flag
swap; docs claim "no assembly" and a nonexistent system-CA replacement; Windows poll latency
structural (10 ms pipe / 16 ms mixed); plugin `THREADSAFE` unhonoured; no dynamic plugin
loading despite the wording.

---

# Part C — port plan

**Decision:** `agentc` survives; fc's runtime/core architecture and M8 hardening are translated into agentc idioms. fc's `src/plat`, `src/net`, TUI renderer, build system, `fc.h` surface and `fc_plugin.h` are not ported. `agentc_plugin.h` is **not changed**; the plugin host adapts.

Every claim below cites the tree it came from. fc sources are fc/; everything else is agentc/. Where agentc already has the mechanism (e.g. HTTP's one absolute deadline, TUI grid-store sanitization) the delta is stated instead of a rewrite.

---

## C.1 Target architecture

```
                 app/main.c  (CLI, print mode)
                   │   app/mode.c  ── one AgcModeCtx setup/teardown (cfg·model·system·key·session·agent)
     ┌─────────────┼──────────────┬───────────────────┐
   tui/        mode_json.c     mode_rpc.c         ext/plugin.c  ext/mcp.c
   (unchanged)  (unchanged      (unchanged         └─ one v2 tool each, ud = descriptor, no trampolines
                call sites)     call sites)
     └──────────────────────────┴──────────────────────┘
                        core/
   agent.c        turn loop · request_stream(emit) · retry · exactly-once closure · chunked job batches
   events.c       typed hub: primary sink (slot 0) + subscribers ≥1 + quiet gate
   tools/
     registry.c   AgcTool v2: ud · timeout_ms · run · start/step · flags consulted
     jobs.c       bounded parallel driver: start-first, one poll loop, deadline/abort, spill cap
     args.c       AgcToolArgs (parse+defaults+missing-arg error)
     <tools>.c    sync run() bodies; bash uses start/step
   session.c      v1+v2 JSONL · version refuse · append degrade/truncate · compaction replay · listing kept
   compact.c      estimate/cut/system only; provider-neutral request lives in agent.c
   provider.h     {api, ops} table + per-stream priv + per-stream JSON arena
 prov/            anthropic.c · openai.c · codex.c behind one ops row each
 wire/            http.c (deadline) · sse.c (caps) · jsonw.c (ascii mode) · json arena-per-parse
 base/            limits.h · out.{h,c} · deadline.h · glob.c · alloc/buf/str/fmt/json
 plat/            +os_spawn_group +os_ftruncate (append-only)
```

**Ownership after the port.** `core` owns the transcript, the turn loop, the typed event hub, the tool/job driver and the session file. `prov` owns a single `{api, ops}` row per API and per-stream private state, so adding a provider is one row; `core` never switches on a provider name. `ext` owns only adapters: `plugin.c` and `mcp.c` each expose **one** executor function and carry their identity in `AgcTool.ud`; the trampoline tables and their ceilings (`mcp_int.h:23` `MCP_MAX_TOOLS 256`, `ext/plugin.c` `AGENTC_PLUGIN_MAX_TOOLS 32`) are gone. `wire` owns parsing caps and a JSON arena handle; `base` owns the cap table, the terminal-safe writer, the absolute deadline and the bounded glob. `app/mode.c` owns mode wiring that `app/main.c`, `mode_json.c` and `mode_rpc.c` currently duplicate. **Changed:** `include/agent.h` (append-only v2 fields/types), `include/agentc.h` (`AgcJsonArena`, `*_used` formatter), `include/wire.h` (`AgcJsonW` append), `include/plat.h` (`os_spawn_group`, `os_ftruncate`); `agentc_plugin.h` untouched. **Deleted:** 256 MCP trampolines + 32 plugin trampolines and their fixed caps; `api_path()` (`core/agent.c:299`); `agentc_compact_extract()` (`core/compact.c:81`, name-branching); the `"stream":true→false` string splice (`core/agent.c:404-421`); the session raw-span scanner (`core/session.c:355-420`); the exponential `glob_here()` (`core/tools/find.c:62-101`); `g_container_type[64]` file-static writer state (`wire/jsonw.c:13`); the fixed `AgcTool available[64]`/`tools[64]` arrays (`app/main.c:671,688`) and `compat[8]` (`prov/openai.c:374`).

---

## C.2 Per-subsystem port specifications

### C.2.1 Tool interface + job model

**agentc today.** `AgcTool` is `{name,label,desc,params_json,flags,exec}` (`include/agent.h:81-92`) with no userdata, no call id and no timeout; `AGENTC_TOOL_SEQUENTIAL` is defined but never consulted. The turn loop executes calls inline in source order (`core/agent.c:780-867`), passes `&a->cancel` as the only cancellation, and backfills unanswered calls **only on cancel** (`agent.c:859-867`). MCP needs a distinct function pointer per slot because there is no `ud` (`ext/mcp.c:16-17,1365-1478`); the plugin host needs 32 of them (`ext/plugin.c` `AGENTC_PLUGIN_TRAMPS`); `app/main.c` hard-caps the composed tool list at 64 entries.

**fc source of the mechanism.** `tool_t` with `ud`/`flags`/`timeout_ms`/`run`/`start`/`step`, `tool_job_t` and `TOOL_OUTPUT_MAX` (`fc/src/core/tools.h:55-120`); the driver `tool_jobs_run`, `run_parallel` (async start first, one `fc_poll` loop, per-job deadline/abort classification, completion-order callbacks) and `run_sync` (`fc/src/core/tools/jobs.c`, batch entry `tool_jobs_run`); `TF_SEQUENTIAL` forces the whole batch sequential (`jobs.c`); >32-call batches are chunked by the agent (`fc/src/core/agent.c:403-421`). Registration returns an index and `tool_unregister` exists (`tools.h`), so a failed plugin rolls back (`fc/src/ext/plugin.c:238-247`).

**Target design (agentc).** Append to `include/agent.h` after `exec`:

```c
    void *ud;                     /* tool-instance identity; NULL for builtins */
    int   timeout_ms;             /* 0 = AGENTC_TOOL_TIMEOUT_DEF_MS (120000) */
    int (*run)(const AgcTool *self, const AgcToolCall *call,
               AgcBuf *out, bool *is_error);
    int (*start)(const AgcTool *self, const AgcToolCall *call, AgcJob *job);
    int (*step)(const AgcTool *self, AgcJob *job);
```

New public types in the same header: `AgcToolCall {const char *call_id; const char *name; const char *args_json; const volatile bool *cancel;}` and `typedef struct AgcJob AgcJob;` (opaque); the full struct lives in new internal `src/core/tools/jobs.h` (`state`, `tool`, `call` **by value**, `index`, `pid`, `fd`, `deadline_ns`, `out`, `is_error`, `priv`, spill state). New `src/core/tools/jobs.c`:

- `void agentc_tool_job_init(AgcJob *)` — every negative-capable field starts at -1 (fc `tool_job_init`, batch 3).
- `int agentc_tool_jobs_run(const AgcTool *const *, const AgcToolCall *, size_t n, AgcJobCb, void *ud)` — pre-run abort gate; async `start` first in source order; sync `run` inline; one `os_poll(..., 20)` loop driving `step`; classify `-ECANCELED`/timeout/fatal exactly as fc `run_sync`/`job_timeout`/`job_cancel`; callback in completion order.
- `int agentc_tool_checkpoint(void)` — deadline/abort for chunked sync tools (fc `tool_checkpoint`, batch 8d4).
- `int agentc_tool_append_output(AgcJob *, const char *p, size_t n, size_t cap)` / `agentc_tool_finalize_output` — streaming cap with a 0600 spill file (fc L2).
- Internal cap `AGENTC_TOOL_JOBS_MAX 32` bounds the driver only; `agent.c` chunks larger batches (`fc agent.c:403-421`).
- Normalization: `run == NULL && exec != NULL` is wrapped into a `run` shim, so every existing positional initializer (`{name,label,desc,params,flags,exec}`) and every test calling `.exec(...)` keeps working. Builtins migrate to `run` inside Q2; `exec` stays public for source compatibility.

**Plug-in points.** `agent.c:775-867` is replaced by: build one `AgcToolCall` per block, veto check, `agentc_tool_jobs_run`, then append results in source order (the driver frees its buffers). `AGENTC_TOOL_SEQUENTIAL` in any tool forces the sequential path. `TF` semantics become live, which makes the Rust crate's swapped constants (`crates/agentc-plugin/src/lib.rs:45-46`: SEQUENTIAL=0x0004/DESTRUCTIVE=0x0002 vs header 0x0002/0x0004) a real bug and must be fixed in the same batch.

**Adapter conversion.**
- `ext/plugin.c`: delete `PluginExecFn`/`AGENTC_PLUGIN_TRAMPS`/`g_trampolines` and `AGENTC_PLUGIN_MAX_TOOLS`; keep the per-tool descriptor (`PluginTool`, still one copy of the ABI struct) and register one `plugin_tool_run(self=…, call=…)` with `ud = &g_tools[i]`. `AgcToolV1.execute` now receives the real `call->call_id` (today `""`, `plugin.c:388`) and `call->args_json`; `host_is_cancelled` keeps using the pending-call signal token. Registration storage becomes an `AgcVec` bounded only by memory; failure is a logged `agentc_alloc` abort per agentc convention (no silent 32-cap drop).
- `ext/mcp.c`: embed an `AgcTool` inside `McpToolEnt`, `ud = ent`, delete `MCP_SLOT_LIST`/`g_exec_table` and the `slot >= MCP_MAX_TOOLS` failure (`mcp.c:759`); `agentc_mcp_tools` walks live entries. Rename `mcp_exec_slot` to `mcp_run(const AgcTool*, const AgcToolCall*, AgcBuf*, bool*)`.
- `app/main.c`: `available`/`tools` become `AgcVec`; `select_tools` takes the vec; `--tools` filtering unchanged.

**Tests.** Port fc's `tools_jobs` sync/abort/deadline classification (fc `sync.*`, `walk.*`, `abort-par.*`, 91 checks) into a new `tests/jobs_test.c`; extend `tests/tools_test.c`, `tests/plugin_test.c`, `tests/mcp_test.c`, `tests/modes_test.c` for `ud` identity, call-id delivery, `TF_SEQUENTIAL` ordering, >32 chunking and unknown tool closure. The parallel rendezvous test (`bash` start overlap) is written against a test-local async fake, since a golden shell rendezvous is timing-flaky. **Deletion count:** ~370 lines of trampolines + caps; ~60 lines of cap plumbing.

**Rollback.** Revert the batch; `exec` and the old loop are restored; no on-disk or public-ABI residue.

---

### C.2.2 JSON re-entrancy

**agentc today.** `src/base/json.c` keeps one process-global document: `arena`, `children`, `cur/end/fail/depth` are file statics (`json.c:20-28`); `agentc_json_parse` resets the arena on entry (`json.c:258-273`) and `agentc_json_reset` recycles only the newest block (`json.c:52-60`). The public contract says so (`include/agentc.h:108-110`). It works today only because every caller copies strings immediately; the latent break is visible where a parse nests inside a live document: MCP element callbacks (`ext/mcp.c:577-590`, `620-632`) run `agentc_json_parse` while `root` from `agentc_mcp_parse_tools`/`agentc_mcp_parse_call` (`mcp.c:603,636`) is still live; plugin host helpers parse inside an event/execute callback (`ext/plugin.c:131-168`); provider map functions re-parse per SSE event (`prov/anthropic.c:274`, `openai.c:237`, `codex.c:251`).

**fc source.** `json_parse(arena_t *a, …)` takes the arena explicitly (`fc/src/base/json.h`), the provider stream owns a per-event arena (`fc/src/prov/provider.c` `prov_event_root`), and D5/mechanism 9 made the lifetime explicit.

**Target design.** Add to `include/agentc.h` (append-only):

```c
typedef struct AgcJsonArena AgcJsonArena;
AgcJsonArena *agentc_json_arena_new(size_t initial_bytes);   /* NULL on OOM */
void agentc_json_arena_free(AgcJsonArena *a);
void agentc_json_arena_clear(AgcJsonArena *a);
AgcJson *agentc_json_parse_in(AgcJsonArena *a, const char *p, size_t n);
```

The parser statics move into the struct (block chain + temp children vector + parse cursor). `agentc_json_parse` becomes `parse_in(&g_default, …)` and `agentc_json_reset` becomes `clear(&g_default)`, so tests and any unmigrated caller are byte-identical; `json_test.c`'s arena-stability checks still pass. Accessors (`agentc_json_get/…/key_at`) are unchanged and the arena must outlive every pointer derived from it.

**Call-site inventory to migrate (~40 calls, 16 src files).** `core/tools/registry.c:23`, `ls.c:87`, `find.c:375`, `grep.c:310`, `edit.c:93` (plus the nested `edits` scan); `core/config.c:437,585,618`; `core/auth.c:114,233`; `core/oauth.c:297,387,577,593`; `core/discover.c:152,161,179,249,277`; `core/session.c:223,444`; `core/compact.c:83` (deleted in Q5); `ext/plugin.c:133,334` plus the `host_json_get_*` trio (`plugin.c:131-168`); `ext/mcp.c:273,291,307,348,478,579,603,620,636,989,1074,1348` (13); `tui/theme.c:114`; `app/mode_rpc.c:379,493`; `prov/anthropic.c:274`, `prov/openai.c:237`, `prov/codex.c:251`. Tests (`json_test`, `fuzz_test`, `mcp_test`, `plugin_test`, `provider_test`) keep `agentc_json_parse`.

**Migration mechanics / ordering.** (1) Introduce the arena + shim. (2) Migrate the nested sites first: MCP span callbacks must take an arena through their `ud` context (not use the default), plugin `json_parse_path` gets a per-call arena and copies as it already does into `g_json_scratch`, providers move the arena into `AgcStreamState` (append `AgcJsonArena *arena`) cleared per event instead of a global reset. (3) Then the single-document sites. (4) In Q10, `agentc_json_reset` remains only as a test shim; no `src/` caller uses it.

**MCP raw-span scanner decision.** The `JSpan` scanner (`ext/mcp.c:40-260`) stays for *field discovery on a line*; it is not a second JSON DOM. Every parsed sub-span goes into the caller-provided arena, and the "parse then copy immediately" rule is removed. The session argument scanner (`core/session.c:355-420`) is separately deleted in Q6 by moving to a JSON-string representation (see C.2.5); that is the only scanner that fully duplicated JSON.

**Tests.** Extend `tests/json_test.c` with `arena.two-live` (parse doc A into arena A, doc B into arena B, assert A's pointers still read correctly), `arena.reuse`, `arena.free` under `agentc_mem_live`; extend `tests/mcp_test.c` with a `tools/list` whose element callback parses while the outer root is examined after the scan (mutation: revert one callback to the global arena → a named check fails).

**Rollback.** Revert; the default-arena shim means no caller-visible change.

---

### C.2.3 Provider descriptor

**agentc today.** `AgcProvider` (`include/agent.h:132-146`) is a per-adapter vtable duplicated per named provider (`prov/anthropic.c`, `openai.c`, `codex.c`); routing lives in `core/agent.c:299-304` (`api_path()` switch on `api`); per-stream state is shared: `AgcStreamState.tool_index` plus `args` (`include/agent.h:120-130`) is overloaded by each adapter; dynamic OpenAI-compatible providers are `compat[8]` static copies (`prov/openai.c:374-390`); provider knowledge is also spread over setup presets (`app/setup.c:19-31`), the auth env table (`core/auth.c:44-60`), discovery name switches (`core/discover.c:130-200`), compaction extraction by `provider == "anthropic"` (`core/compact.c:81-108`) and the stream-swap string splice (`core/agent.c:404-421`).

**fc source.** `provider_ops_t {id, api, path, build_request, headers, stream_init, stream_event, stream_free}` plus `void *priv` in `prov_stream_t` (`fc/src/prov/provider.h:55-83`); a lazy `{api, ops}` registry with typed duplicate/overflow errors (`fc/src/prov/provider.c:15-71`); shared `prov_stream_begin/release/alloc/dispose` and `prov_event_root/type`; provider-neutral compaction via `agent_request_text(..., emit=false)` over the normal stream path (`fc/src/core/agent.c:232-270`); ADR-36.

**Target design.** New internal `src/prov/provider.h` is the single table:

```c
typedef struct {
    const char *api;              /* table key */
    const char *path;             /* request path */
    const char *env_keys[3];      /* credential env vars, first non-empty wins */
    int  (*build_request)(AgcBuf *out, const AgcRequest *r,
                          const char *url_host, const char *url_path);
    int  (*map_sse)(AgcStreamState *st, const AgcSseEvent *ev);
    int  (*finish)(AgcStreamState *st);
    int  (*stream_open)(AgcStreamState *st);      /* alloc priv + arena, 0/-errno */
    void (*stream_close)(AgcStreamState *st);
} AgcProviderOps;
int  agentc_provider_register(const AgcProviderOps *ops);   /* index, -EEXIST/-EINVAL/-ENOMEM */
const AgcProviderOps *agentc_provider_by_api(const char *api);
void agentc_provider_registry_reset(void);
```

The public `AgcProvider` stays as the named handle (`agentc_prov_anthropic()` etc. still return stable pointers) and carries its three v1 function pointers, but those are wired from one ops row; a new internal `agentc_provider_ops(const AgcProvider *)` returns the row. `api_path()` is deleted; `agent.c` uses `ops->path`. Append `void *priv; AgcJsonArena *arena;` to `AgcStreamState` (`include/agent.h:120-130`); the `memset` at `agent.c:649` is replaced by `agentc_stream_state_init(a->prov, &st)` calling `stream_open`, and every exit path calls `stream_close`. Adapter-private state moves out of the shared struct: anthropic `{bool open_block; size_t tool_index;}`, openai `{size_t next_index;}`, codex `{size_t item_index;}`. Dynamic OpenAI-compatible providers become registered rows in a growable table (no `compat[8]`), with stable addresses (heap blocks never moved while a provider pointer is held).

**Compaction goes provider-neutral.** `core/compact.c` keeps only estimate/cut/system; `agentc_compact_extract` and the `"stream":false` splice are deleted. `agent.c` gains one internal `request_stream(a, view, system, bool emit, AgcStreamState *st)` used by both a turn and compaction; compaction runs it with `emit=false` (event hub quiet, Q4) and takes `st`'s accumulated text. No provider-name branch, no raw-body editing.

**Other tables.** Discovery selects its listing strategy by `ops->api`/a `discover_style` enum on the row instead of a name switch; auth resolution asks `ops->env_keys`; setup presets keep their `name/base` rows but map through `agentc_prov_openai_compatible`. Catalog and cost remain data (`prov/models.c`).

**Tests.** Extend `tests/provider_test.c`: `provider.registry.builtins`, `.additive` (fake ops row drives a fake SSE), `.duplicate`, `.invalid`, `.priv.lifecycle` (`stream_open`/`close` balance under `agentc_mem_live`); `tests/compact_test.c` asserts compaction through a non-Anthropic fake provider with no `agentc_compact_extract`; existing `build_*` byte goldens stay byte-identical.

**Rollback.** Revert the batch; descriptor rows are additive and `AgcProvider` keeps its old vtable shape.

---

### C.2.4 Event model

**agentc today.** One overwriteable callback per agent: `AgcEventFn(void*,int,const void*)` (`include/agent.h:244`), stored in `events/events_ud` (`core/agent.c:53-54`), emitted through a cancel filter (`agent.c:290-297`). The plugin bridge is a second, special-cased path (`agent.c:154-164` `want`/`notify`, `ext/plugin.c:481,500+`); `agentc_plugin_emit_agent_event` maps typed payloads and fans them to plugin handlers.

**fc source.** Fixed 8-slot fan-out, primary slot 0 behind `ev_set_sink`, `ev_subscribe`/`ev_unsubscribe`, and `ev_quiet` suppressing **every** subscriber including the plugin bridge (`fc/src/ext/events.{h,c}`, events.c:126-194, batch 8ec); ADR-41.

**Target design.** New internal `src/core/events.{h,c}`:

```c
void agentc_events_set_primary(AgcEventFn fn, void *ud);   /* slot 0, replace semantics */
int  agentc_events_subscribe(AgcEventFn fn, void *ud);     /* id >= 1, -ENOMEM when full */
void agentc_events_unsubscribe(int id);
void agentc_events_quiet(bool on);
bool agentc_events_quiet(void);
bool agentc_events_wants(void);
void agentc_events_emit(AgcAgent *a, int ev, const void *data);  /* keeps the cancel filter */
```

`agentc_agent_set_events` keeps its signature and stores the primary slot *for the agent* (multiple agents coexist in RPC rebuilds/tests); `agentc_events_emit` dispatches the agent's primary sink then the global subscriber slots in index order, with the current cancel-suppression rule applied uniformly. The plugin bridge registers its `notify` as subscriber #1 when installed (`agentc_agent_set_plugin_bridge`, `include/agent.h:307`), and `notify` checks `agentc_events_wants()` so payloads are only built when somebody listens; `agentc_plugin_wants` (`ext/plugin.c:481`) stays. `agentc_events_quiet(true)` suppresses primary + subscribers + bridge (this is what makes provider-neutral compaction silent). The typed payload structs (`AgcTextDelta`, `AgcToolExec`, `AgcCompactInfo`) stay — the mechanism ported is fan-out plus quiet, not fc's `ev_t` envelope. The session writer stays an `AgcMsgObserver` (typed transcript appends, fc ADR-41's accepted deviation).

**Tests.** New `tests/events_test.c` (golden): subscribe order primary→subs in index order, unsubscribe/reuse, full-table `-ENOMEM`, quiet suppresses primary/subscriber/bridge, restore delivers, `wants()` false with no subscribers. Extend `tests/plugin_test.c` with `plugin.event.quiet-suppresses-bridge` (fc `events.quiet.*` analogue) and a mutation proof: delete the quiet gate in `agentc_events_emit` → the named checks fail.

**Rollback.** Revert `events.c`; `set_events` still lands in slot 0 and the bridge falls back to direct calls.

---

### C.2.5 Session and compaction

**agentc today.** One file per session, append-only JSONL (`include/session.h`). The header writes `"version":1` (`core/session.c:130-150`) but **no reader checks it**; `agentc_session_open` (`session.c:215-245`) only parses the header for id/cwd; `agentc_session_load_messages` silently skips any line it cannot parse (`session.c:446`) and only accepts `type=="message"`; compaction records are written by `agentc_mode_append_compaction` (`app/mode_json.c:182-205`) and **never replayed**. Torn tails are healed by appending a newline (`need_newline`, `session.c:60,231`). Tool-call arguments are stored raw and recovered by the hand-rolled span scanner (`session.c:355-420`). `agentc_agent_compact` splices the transcript after the summary and emits `AGENTC_EV_COMPACT` (`core/agent.c:440-460`), leaving the record ordering to the front end.

**fc source.** One append path `session_write` that ftruncates to the recorded offset and degrades the session forever (`fc/src/core/session.c:23-40`); `SESSION_VERSION` read and newer versions refused (`session.c:382-390`); warn-once on skipped malformed lines; compaction record written **before** the splice and replayed by resetting the reconstruction and seeding the summary (`session.c:403-410`); torn tail truncated on reopen (`session.c:538-548`); arguments serialized as a JSON string and re-parsed (`session_append`/`append_from_json`).

**Target design.**
- `AgcSession` gains `i64 append_off; int append_err; bool degraded;`. All appends go through one `session_write()`: on failure, `os_ftruncate(fd, append_off)` (new plat primitive, append `int os_ftruncate(int fd, i64 len)` to `include/plat.h` + linux/mac/win backends), set `append_err`, return the first error forever. On open, `append_off` = offset after the last complete line, and a tail without `\n` is truncated before appending; the `need_newline` heal stays as a second guard in `session_write` (task requirement: keep agentc's repair and listing API).
- Header version: `agentc_session_open` and `agentc_session_load_messages` read `version`; `version > 1` → refuse with `-EPROTO` and a warning (fc C5). Keep the current skip-malformed behavior but count and warn once.
- Compaction replay: in the loader, `type=="compaction"` frees the reconstruction and seeds `summary` as a user message, then later message lines replay onto it (`fc session.c:403-410`). To make record-before-splice true in agentc, `compact_now` moves the `AGENTC_EV_COMPACT` emission **before** mutating `a->tr` and the front ends append the record synchronously; add `int agentc_session_append_compaction(AgcSession *, const AgcCompactInfo *)` in `session.h` (append) so `mode_json.c`/`main.c` stop hand-building `append_raw`. Document that the transcript is only mutated after the observer returned successfully — if the append fails, the session is degraded and the in-memory splice still proceeds (agentc has no session pointer in core; the alternative, a core recorder callback, is rejected as a bigger seam).
- Argument encoding: new writes emit `"arguments"` as a JSON **string** (`agentc_jsonw_str`); the loader accepts a string (decode into `tool_args`) and, for legacy files, the raw object (existing scanner path retained for exactly one release, then deleted in Q10). Delete `skip_ws`/`skip_json_string`/`skip_json_value`/`find_key_value` (`session.c:355-420`) once the legacy branch is removed. This removes the last parallel JSON parser and makes `--continue` byte-exact.
- Keep `agentc_session_list`/`agentc_session_find_latest` unchanged.

**Tests.** Extend `tests/session_test.c`/`tests/compact_test.c` with `session.version.refuse` (mutate the header to version 2), `session.degrade.*` (inject a short write; assert truncate to the last newline and no later append), `session.tail.*` (torn tail truncated on reopen), `compact.replay.*` (write messages + compaction + messages, load, assert the reconstruction starts at the summary), `session.args.string` and `session.args.legacy` (raw-object fixture still loads). Extend `tests/session_check.py`/`e2e.sh` for record order. Mutation: drop the ftruncate in `session_write` → `session.degrade.*` fails.

**Rollback.** Revert; legacy files load either way, and the `need_newline` path is untouched.

---

### C.2.6 Agent loop

**agentc today.** `agentc_agent_submit` (`core/agent.c:544+`) resets abort at entry (`:546`), retries a turn up to `max_attempts` when `pending && !s.saw_stop` (`:696-704`), emits `MSG_END`, then **always** executes every tool call the assistant produced (`:775-867`), regardless of stop reason; only a cancelled batch is closed (`:859-867`). `msg_has_content` is dead (`(void)`, `:698`). A `length`-truncated response with calls therefore executes partial-argument calls, and a non-`tool_use` stop with calls leaves them unanswered, poisoning every later request (fc C1).

**fc source.** `run_turn` computes `wants_tools = r == 0 && (stop == "tool_use" || "tool_calls") && n_calls > 0`; if calls exist and tools are not wanted (failed/aborted/truncated) each call is answered exactly once with a synthetic error result before any next request (`fc/src/core/agent.c:346-368`); abort is reset per run (`:19-22,453`); tool batches are chunked (`:403-421`).

**Target design.**
- Replace the execution block with the fc shape: `bool wants_tools = turn_ok && asst->stop_reason == AGENTC_STOP_TOOLUSE && ntc > 0;`. If `ntc > 0 && !wants_tools`, append one `AGENTC_ROLE_TOOL` result per call with `is_error=true` and text `"error: aborted before execution"` (cancel) or `"error: response ended before this call could run"` (length/error/other stop), observer-persisted; otherwise run `agentc_tool_jobs_run` (Q2) and append the results. This makes closure unconditional on failure mode and removes the `texec`-only-on-cancel special case.
- Retry interplay: closure stays **after** the retry loop (the assistant message can still be freed and rebuilt on a retry); on a retry the synthetic results never exist yet. Keep `agentc_agent_load`'s replay path unchanged.
- Delete `msg_has_content` and `last_toolcall_args_len`-as-diff helper if unused after Q2.
- `send_stream`/`request_text`: adopt one `request_stream(..., emit)` (C.2.3) so the turn and the summary share the transport/SSE/retry path; retry logic stays inside `agentc_agent_submit`.
- Abort reset: already implemented (`agent.c:546`); add the regression test fc added in T4 (`batch.abort-reset.*`).

**Tests.** Extend `tests/agent_loop_test.c`: `agent.trunc.call-closed` (mock transport returns `AGENTC_STOP_LENGTH` with one tool call; assert a tool result exists, no tool executed, and the next submit succeeds), `agent.abort-reset` (abort, then complete a second turn), `agent.error.calls-closed`. Extend `e2e.sh` with a truncating mock-provider fixture and a second `--continue` run. Mutation proof: revert the `wants_tools` condition → `agent.trunc.call-closed` fails.

**Rollback.** Revert the single function block; the old call-site loop is gone only after Q2, so Q3's diff is self-contained.

---

### C.2.7 Safety mechanisms

**(a) Terminal/JSON sanitizer parity.** agentc's TUI already sanitizes twice: grid-store (`tui/render.c:157-171`) and emit (`render.c:240-253`); `agentc_wcwidth` treats C0/C1/DEL as zero-width. The gap is every non-TUI writer: `agentc_out` is a raw `write_all(1,…)` (`base/log.c:15`, used by print mode `app/main.c:260`, setup prompts, `--list-models`, commands), stderr logging writes the would-be-length bug (below), and `agentc_jsonw` passes bytes ≥ 0x80 unchanged (`wire/jsonw.c` escape default), so a UTF-8-encoded C1 (e.g. U+009B) can reach a terminal in `--mode json/rpc`. fc's mechanism is `base/out.{h,c}` (`out_cp_filter`/`out_safe_write`/`out_safe_sb`, every terminal writer, pure-ASCII JSON). **Minimal delta:** (1) add `src/base/out.{h,c}` and route `agentc_out`/`agentc_outs`/`agentc_outf` and the logger's stderr writes through `out_safe_write`; this single choke point covers print/setup/mcp/command output because they all use `agentc_out`. The TUI's `term_write` remains the one raw writer (its input is already sanitized). (2) Append `bool ascii` to `AgcJsonW` (`include/wire.h`) and `void agentc_jsonw_set_ascii(AgcJsonW *, bool)`; when set, bytes ≥ 0x7f are escaped as `\uXXXX` (surrogate pairs for non-BMP). Set it in `mode_json.c`, `mode_rpc.c`, `ext/plugin.c` payloads and session writers; **leave provider request bodies UTF-8** so `tests/net.sh` and wire goldens stay byte-stable (the terminal-injection risk is output, not a provider body that is JSON-parsed). Tests: new `tests/safety_test.c` `safety.out.*` (OSC 52, CSI, DEL through `agentc_out`), `safety.json.*` (mode_json/mode_rpc lines contain no raw byte ≥ 0x7f), plus a TUI store assertion; mutation proof on `out_safe_write`.

**(b) `limits.h`-style caps on append.** agentc has caps but scattered: `SSE_CAP` (`wire/sse.c:21`), `H_MAX_HEAD`/`H_MAX_CHUNK_LINE` (`wire/http.c:46-49`), `MCP_MAX_MSG` (`ext/mcp.c:37`), `OUT_MAX_BYTES` (`core/tools/bash.c:12`), read/edit/16 MiB caps, `RPC_MAX_LINE` (`app/mode_rpc.c:63`). Add internal `src/base/limits.h` naming them (`AGENTC_LIMIT_SSE_EVENT_BYTES`, `_HTTP_HEADER_BYTES/_LINES`, `_MCP_LINE_BYTES` 1 MiB, `_TOOL_BYTES` 50 KiB/`_TOOL_LINES` 2000, `_READ_BYTES` 64 MiB, `_EDIT_BYTES` 8 MiB, `_RPC_LINE_BYTES` 1 MiB, `_TUI_PARTIAL_BYTES`, `_TUI_LINES`) and replace the local #defines; add the missing enforced checks: MCP `read_line` must reject a line > cap while accumulating (`mcp.c:461,510`) and `inputSchema` copies cap at 2048 bytes with a WARN. The job driver's `agentc_tool_append_output` is the one tool-output choke point.

**(c) Absolute deadline primitive.** fc's `base/deadline.h` exists but is dead code (its own review says so); agentc's HTTP already computes one absolute deadline per request and passes it down (`wire/http.c:125-135,632-640`), while the tool/MCP loops re-check elapsed times. **Decision:** port the primitive as internal `src/base/deadline.h` (`agentc_deadline_t {i64 at_ns;}`, `_none/_after_ms/_expired/_remaining_ms`) and *use* it: the new job driver stores `deadline` per job, MCP computes one deadline per `tools/call`/handshake exchange instead of per read, retry checks the remaining budget; convert `wire/http.c`'s `wait_fd` to take the type so there is exactly one implementation. No unused header is added.

**(d) Process groups, non-blocking reap, group kill.** agentc ignores `ctty` (`plat/linux/sys.c:216`), `os_spawn` has no group option, bash kills a single pid (`core/tools/bash.c:205-210` `os_kill(pid,15)`/`(pid,9)`), and MCP stop kills the direct child only. fc has `fc_spawn_group` on linux+mac (Win32 job objects) (`fc/src/plat/plat.h:204`), non-blocking `WNOHANG` reap loops (`fc/src/core/tools/bash.c`, `jobs.c`, `mcp.c` stop escalation SIGTERM → bounded grace → SIGKILL). **Target:** append `int os_spawn_group(char *const argv[], char *const envp[], const char *cwd, int in, int out, int err, int ctty);` and `int os_spawn_shell_group(const char *kind, const char *command, int in, int out, int err);` to `include/plat.h` + all three backends (linux `setpgid(0,0)` before exec; mac same; win `CREATE_NEW_PROCESS_GROUP`/job object, `os_kill(-pid)` terminates the job). Bash spawns a group and kills `-pid` with a fallback to the leader; the job driver's timeout/cancel and `mcp_stop` use the same group escalation; every wait is `os_wait(pid, true)` inside a bounded `os_poll(NULL,0,5)` loop. Tests: `tests/tools_test.c` `bash.eof-hang` (`exec 1>&-; sleep 100000` must time out, not block), `bash.group-kill` (a forked grandchild dies), `tests/mcp_test.c` `mcp.stop.sigterm-ignoring` (extend `tests/mcp_mock.py` with a mode that ignores SIGTERM). Windows/mac compile + Wine run required.

**(e) Auth store read-merge-rewrite.** `agentc_auth_set_key` already rewrites the whole DOM preserving unknown keys (`core/auth.c:200-260`). `oauth.c store_save` (`core/oauth.c:316-372`) only preserves `api_key` for the two OAuth providers (`load_legacy_keys` `oauth.c:290-315`), so login/refresh/logout silently drops e.g. `openrouter.api_key`. **Target:** one internal `auth_store_rewrite(const char *provider, const char *api_key, const char *oauth_json_or_null)` that parses the existing file into a per-call arena, re-emits every top-level pair, and replaces only the target provider's subtree; called by `agentc_auth_set_key`, `store_save` (login/refresh/logout) and nothing else. Tests: `tests/oauth_test.c` `auth.merge.*`: store `anthropic.api_key` + `openrouter.api_key`, drive a login (fixture) for `openai`, reload, assert both survive and the OAuth entry exists; remove one, assert the other survives. Mutation: revert to the OAuth-provider-only copy → named check fails.

**(f) `jsonw` correctness.** agentc emits no floats (`agentc_jsonw_u64/i64` only), so fc's `jsonw_num` C3 class does not exist; do **not** port a float writer. Fix the writer's hidden non-reentrancy (`g_container_type[64]` at `wire/jsonw.c:13` is shared by all writers) by appending `u8 ctype[64]` to `AgcJsonW`; add `int agentc_jsonw_validate_raw(const char *p, size_t n)` and use it where `agentc_jsonw_raw` embeds model-supplied args (session, provider replay) so malformed raw can never corrupt a request. Property test in `tests/jsonw_test.c`: for integers/escapes, `parse(emit(x))` round-trips; for invalid raw, the validated variant refuses.

**(g) `fc_fmt`-class would-be-length misuse.** Real agentc bugs of exactly that class: `base/log.c:29-31` and `:69-72` pass the would-be length to `write_all(2, tmp, n)` (OOB read on truncation); `core/tools/registry.c:16-19` and `core/tools/read.c:17-19` call `agentc_strdup_len(tmp, n)` with a possibly oversized `n`; `tui/editor.c:271-273` passes would-be `m` to `insert_bytes`. **Target:** append `int agentc_snprintf_used(char *out, size_t cap, const char *fmt, ...)` and `int agentc_vsnprintf_used(...)` to `include/agentc.h` (fc `fc_fmt_buf` analogue), keep `agentc_snprintf` as the snprintf-semantics function with the header comment stating it returns the would-be length, and convert the five sites. Extend `tests/fmt_test.c` with a 4 KiB `%s` into a 16-byte buffer asserting `_used == 15` and NUL termination.

**(h) Editor history parity.** agentc's history is a dynamic vector with a separate live buffer (`tui/editor.c:308-330`); `hist_pos == hist.len` is valid by construction, so fc's M2 off-by-one does not exist here. Port only fc's regression (`safety.editor.*`: push 33 entries, navigate up/down) into `tests/tui_test.c` to lock the behavior.

---

### C.2.8 Mode setup

**agentc today.** print/TUI assemble config, model, prompt, key, observer and session in `app/main.c` (~`:650-930`); JSON in `app/mode_json.c:376-388`; RPC in `app/mode_rpc.c:540-560` plus `agentc_mode_rebuild_agent`; each registers events, writes the JSONL header and flushes messages itself. fc has one `app_mode_setup`/`teardown` over an owning `app_setup_t` used by all four modes (`fc/src/app/setup.{h,c}`, batch 8e1).

**Decision:** partially import. Keep agentc's onboarding (`agentc_setup_onboard`), CLI flags and mode-specific UX exactly; extract the *mechanics* into `src/app/mode.{h,c}`:

```c
typedef struct AgcModeCtx { const AgcConfig *cfg; char *provider, *model;
                               AgcAgent *agent; AgcSession *session;
                               size_t flushed; ... } AgcModeCtx;
int  agentc_mode_setup(AgcModeCtx *, const AgcModeConfig *, const AgcModeIo *, unsigned flags);
void agentc_mode_teardown(AgcModeCtx *);
void agentc_mode_write_session_header(AgcModeCtx *);
void agentc_mode_session_flush(AgcModeCtx *);
void agentc_mode_append_compaction(AgcSession *, const AgcCompactInfo *);
```

`flags` carries session on/off and abort installation; ownership of model/key/system/session lives in the ctx. main.c/`mode_json.c`/`mode_rpc.c`/TUI call the same path; `mode_json.c`'s header builder becomes one function that emits both the JSONL header line and the session `append_raw`. No `app_setup_t` name, no signal-policy struct, no JSON/RPC UX change.

**Tests.** Extend `tests/modes_test.c` (setup/teardown leaves `agentc_mem_live` balanced and the session file closed; session-on flags for json/rpc) and keep `tests/e2e.sh` onboarding untouched. No mutation needed beyond the batch gate.

**Rollback.** Revert; it is a pure refactor.

---

### C.2.9 Shared helpers

**`tool_args_t` → `AgcToolArgs`.** fc wraps arena+parse+getters and the shared missing-argument error once (`fc/src/core/tools/args.c`). agentc repeats that block in `core/tools/registry.c:22-30` plus each exec (`ls.c:87`, `find.c:375`, `grep.c:310`, `edit.c:93`, read/bash/edit/write) and re-parses inside `agentc_tool_edit` (`edit.c:93`). New internal `src/core/tools/args.{h,c}`: `agentc_tool_args_parse(AgcToolArgs *, const char *, size_t)` (tolerant: only `-ENOMEM` fatal, malformed → `root == NULL`), `agentc_tool_args_free`, `agentc_tool_args_str/int/bool` with NULL-safe defaults, `agentc_tool_args_missing(AgcBuf *, tool, key, bool *)`. `int` is `i64` (agentc is LLP64-aware on Windows). It owns a per-call `AgcJsonArena` (Q1), so tools no longer depend on the default arena. Migrate the seven sites; `edit.c` reuses the caller's parsed root for the edits array.

**Dir iterator.** Not ported. agentc's `agentc_dir_scan` is implemented on all three backends and every walker is bounded (`core/tools/find.c:225-289`: 8000 entries, 64 depth; directory listings are sorted locally in `ls.c`/`find.c`). fc's `base/dir` would add a fourth platform abstraction for no correctness gain. Add a shared `AgcDirList` only if Q2's tool rewrite shows the sort duplicating (optional).

**Bounded glob.** agentc's `glob_here` is the exponential recursion fc fixed in L4: `*` rescans and recurses (`core/tools/find.c:62-101`), so `find {"pattern":"*a*a*a*a*a*a*a*a*a"}` hangs. Port fc's iterative two-pointer matcher with a 256-byte pattern cap and a 1,000,000-step budget (`fc/src/core/tools/walk.c` matcher half) into a new `src/base/glob.{h,c}`, keeping `agentc_tool_glob_match(pattern, text)` as the API (tests use it). Same implementation serves `find.c`, `grep.c:194`, and `.gitignore` matching. Tests: `tests/tools2_test.c` `glob.budget` (pathological pattern returns "no match" inside the golden timeout), `glob.cap`, plus the existing patterns unchanged; mutation: restore the recursive matcher → the golden times out.

---

## C.3 Ordered batches

Each batch is a mergeable unit that must pass agentc's standard gate: `make check`, `./tests/e2e.sh`, `./tests/net.sh`, `./tests/mcp.sh`, `./tests/plugin.sh`; batches touching `include/plat.h`, `src/plat/*`, `src/net/*` or golden bytes additionally run `make windows`, `make wine-check`, the qemu aarch64/riscv64 `make check` loop and `nix build .#release-all`. LOC are rough net new+changed.

### Q0 — Base primitives and formatter misuse (≈400 LOC, +~80 test)

Lands: `src/base/limits.h` (names only, existing constants referenced); `src/base/out.{h,c}` + `agentc_out`/logger rerouting; `src/base/deadline.h` + `wire/http.c:125-135,632-640` conversion; `agentc_snprintf_used`/`agentc_vsnprintf_used` + five misuse fixes (`base/log.c:29-31,69-72`, `core/tools/registry.c:16-19`, `core/tools/read.c:17-19`, `tui/editor.c:271-273`); append `u8 ctype[64]` to `AgcJsonW` and move `g_container_type` into it (`wire/jsonw.c`).

Tests: `tests/fmt_test.c` truncation; `tests/safety_test.c` `safety.out.*`; `tests/jsonw_test.c` writer reuse (two interleaved writers). **Deletions:** none. **Rollback:** revert; the only behavior change is in genuinely buggy call sites.

### Q1 — JSON arenas and migration (≈550 LOC, all 16 src files)

Lands: `AgcJsonArena` API + default-arena shim; parser statics moved into the struct; nested-site migration first (`ext/mcp.c` callbacks, `ext/plugin.c` host helpers, `prov/*.c` per-stream arena appended to `AgcStreamState`); then the single-site files (`config/auth/oauth/discover/session/theme/mode_rpc/tools`).

Tests: `json_test` `arena.*`; `mcp_test` outer-root-after-callback; `plugin_test` host JSON helper with two live docs; `fuzz_test` unchanged. **Deletions:** none (`agentc_json_reset` stays as the test shim). **Rollback:** revert; shim keeps old semantics.

### Q2 — Tool v2, job driver, trampoline deletion (≈900 LOC incl. args, −430 deleted)

Lands: `Include/agent.h` appends (`AgcTool` fields, `AgcToolCall`, `AgcJob` forward); `src/core/tools/jobs.{h,c}`; `AgcToolArgs` + 7-site migration; builtins moved to `run`; agent loop switches to `agentc_tool_jobs_run` with >32 chunking and `TF_SEQUENTIAL`; plugin trampolines/cap deleted (one `plugin_tool_run`, real call id); MCP trampolines/cap deleted (one `mcp_run`, `ud` binding); main.c dynamic tool vecs; Rust crate flag constants fixed.

Tests: new `tests/jobs_test.c` (sync deadline/abort classification, async start order, sequential flag, chunking, unknown tool, spill); `plugin_test`/`mcp_test`/`tools_test`/`tools2_test` extended; e2e tool round-trip. **Deletions:** 256+32 trampolines, `MCP_MAX_TOOLS`, `AGENTC_PLUGIN_MAX_TOOLS`, `g_exec_table`, fixed 64-entry arrays. **Rollback:** revert; `exec` remains and the old loop returns.

### Q3 — Exactly-once closure, abort reset, retry (≈160 LOC)

Lands: `wants_tools` closure in `core/agent.c` (delete only-on-cancel backfill); dead `msg_has_content` removed; `request_stream(...)` extracted for Q5.

Tests: `agent.trunc.call-closed`, `agent.error.calls-closed`, `agent.abort-reset`; e2e truncating fixture; mutation proof. **Deletions:** `:698` dead code, `:859-867` replaced. **Rollback:** revert the block.

### Q4 — Event hub (≈260 LOC)

Lands: `src/core/events.{h,c}`; `agentc_agent_set_events` → hub slot 0; plugin bridge as subscriber; `agentc_events_quiet`; `wants()` fast path.

Tests: new `tests/events_test.c`; `plugin.event.quiet-*`; mutation proof on the quiet gate. **Deletions:** direct `emit()` fan-out logic in `agent.c:290-297` moves. **Rollback:** revert; bridge falls back to direct call.

### Q5 — Provider descriptor, priv, provider-neutral compaction (≈650 LOC)

Lands: `src/prov/provider.{h,c}` `{api, ops}` table with dynamic registration; `AgcStreamState.priv/arena`; three adapters’ private state; `api_path` deleted; `agentc_request_text`/`request_stream` shared; `agentc_compact_extract` + `"stream"` splice deleted; discovery/auth lookups adapted to the row; `compat[8]` dynamic.

Tests: `provider.registry.*`, `provider.priv.lifecycle`, `compact.neutral`; existing provider byte goldens unchanged. **Deletions:** `api_path`, `agentc_compact_extract`, stream-flag splice, `compat[8]`. **Rollback:** revert; public `AgcProvider` vtable shape unchanged.

### Q6 — Sessions, compaction replay, argument encoding (≈520 LOC + plat)

Lands: `session_write` append_off/degrade; `os_ftruncate` in plat + 3 backends; version read/refuse; torn-tail truncate; compaction replay; `AGENTC_EV_COMPACT` emitted before the splice + `agentc_session_append_compaction`; arguments as a JSON string with a legacy reader.

Tests: `session.version.refuse`, `session.degrade.*`, `session.tail.*`, `compact.replay.*`, `session.args.string/legacy`; `session_check.py` updated; mutation proof on ftruncate. **Deletions:** scanner deleted in Q10 (kept for legacy in Q6). **Rollback:** revert; new format reads fine in old code? No — old code would skip string arguments, so rollback story is “do not resume sessions written by the new build; the legacy-format writer flag is the fallback” (documented).

### Q7 — Process lifetime (≈420 LOC + plat)

Lands: `os_spawn_group`/`os_spawn_shell_group` in plat + 3 backends; bash group spawn/kill + non-blocking reap; job driver timeout/cancel group kill; MCP own group + SIGTERM→grace→SIGKILL; MCP one deadline per exchange.

Tests: `bash.eof-hang`, `bash.group-kill`, `mcp.stop.sigterm-ignoring`; Wine/qemu runs. **Deletions:** single-pid kill paths, ignored `ctty`. **Rollback:** revert; single-pid kill still compiles.

### Q8 — Output safety, ASCII JSON, caps enforcement (≈380 LOC)

Lands: TUI mutation test; JSON `ascii` mode in output writers; MCP line/schema caps; tool-output spill moved to the driver; `src/base/limits.h` becomes the single source and local constants are replaced.

Tests: `safety.out.*`, `safety.json.*`, `safety.mcp.line-cap`, `tui.sanitizer`; goldens regenerated where ASCII escaping changes JSONL output only. **Deletions:** scattered cap #defines. **Rollback:** revert; provider bodies were never ASCII-escaped, so wire goldens are unaffected either way.

### Q9 — Auth merge and bounded glob (≈420 LOC)

Lands: `auth_store_rewrite` used by auth + OAuth; `src/base/glob.{h,c}` replacing `glob_here`/`glob_class`; pattern cap + step budget.

Tests: `auth.merge.*`, `glob.budget`, `glob.cap`; mutation proofs. **Deletions:** OAuth-provider-only copy code, recursive matcher. **Rollback:** revert.

### Q10 — Mode setup, shared cleanup, docs (≈450 LOC, −200 deleted)

Lands: `app/mode.{h,c}` shared setup; legacy session scanner removed; `src/` no longer calls `agentc_json_reset`; `exec`-only builtins converted to `run`; `.agents/docs/REVIEW.md` deferred items closed, AGENTS.md/API.md updated; `tests/` migrated to `run` where they exercise v2.

Tests: full matrix (golden, e2e, net, mcp, plugin, qemu, Wine, `nix build .#release-all`); benchmark/size unchanged within noise. **Deletions:** raw-span scanner, remaining dead helpers, mode duplication. **Rollback:** revert; `exec` still exists.

---

## C.4 Migration hazards

1. **ABI/public-header discipline.** Every change is append-only: `AgcTool` gains fields after `exec` (positional initializers such as `core/tools/registry.c:181-188` stay valid because appended fields zero-init); `AgcStreamState` gains `priv`/`arena` (all producers `memset` it, but the memset must be replaced by an init helper or the priv leaks); `AgcJsonW` gains `ctype`/`ascii`; `include/agentc.h` and `include/plat.h` gain new functions only. No existing field, signature or include changes. `agentc_plugin.h` is untouched; its C ABI (`AgcToolV1.execute`) gets a real call id through the host adapter, which is a semantic improvement, not an ABI change. The Rust crate constants (`crates/agentc-plugin/src/lib.rs:45-46`) are wrong today and become observable when flags are honored — fix in Q2.
2. **Plugin host adaptation and trampoline ordering.** Delete trampolines only after the v2 registration path works (`ud` + single executor), file by file: plugin first (its adapter owns a copy of the ABI struct and a pending-call table), then MCP. `agentc_plugin_tools` losing its 32-cap means `app/main.c` must size its arrays from the returned count; tests that call `agentc_plugin_tools(out, 64)` keep taking a max. Host adapters must not cast a foreign tool's `ud`: keep the MCP `other->run == mcp_run` style guard when names collide (fc S4 fix).
3. **JSON migration ordering.** The default-arena shim must be the last thing removed; migrate nested sites before simple ones. Arena lifetime is the classic failure: the arena must outlive every `AgcJson*` and all `const char*` derived from it. The MCP span callbacks and plugin JSON helpers must receive an arena via their `ud` context, never reach for a global. ASan/LSan (`make asan` is not part of agentc's gate as written — add `-fsanitize=address` to the test link when touching this, or rely on `agentc_mem_live` checks) should back the new tests.
4. **Session-format migration.** New `"arguments"` as a JSON string is unreadable by old builds; the loader accepts both shapes for one release. Compaction replay changes what `--continue` sees (intended: pre-compaction history is not resurrected) and will change `tests/session_check.py` expectations. Version-refusal will reject a session file written by a future build, as designed.
5. **Build-matrix interactions.** New internal headers must include only `agentc.h`/freestanding headers to survive `-nostdinc` (`Makefile:44-62`). `os_ftruncate` and `os_spawn_group`/`os_spawn_shell_group` must land in all three backends in the same batch or `make windows` and the Nix mac/win compile jobs fail; Windows needs `SetEndOfFile` and job-object terminate semantics, and the Windows ARM64 Wine job is non-gating but must still compile. `make wine-check` runs every golden `.exe` under a 90 s timeout (`tests/wine.sh:67`), so a new blocking test surfaces there.
6. **Silent regressions against Wine/qemu green.** The two changes most likely to move bytes are ASCII JSON (scoped to output writers, not provider bodies) and sanitizer routing of `agentc_out` (only control bytes change). Keep provider request bodies and record/replay bytes byte-identical; regenerate any `mode_json` goldens deliberately. qemu runs the same golden files, so any float formatting is forbidden (we add no float writer).
7. **Provider pointer stability.** `AgcAgent` stores `const AgcProvider *` and `AgcModel *` pointers; the dynamic provider table must allocate stable blocks (like fc's binding block, `fc/src/ext/mcp.c:637-662`) and never move a row while an agent holds it.
8. **Job-driver cancel/identity lifetime.** `AgcToolCall` is a stack struct in the agent loop; the driver must copy it into the job (fc keeps a `const tool_call_t *`, which is safe there because calls live in the stream state). The `const volatile bool *cancel` pointer must remain valid for the whole batch; `start` must not capture pointers into `call` beyond the call.
9. **Compaction quiet save/restore.** The summary request shares the stream path; `emit=false` must save and restore the previous quiet state (fc `send_stream` saves `prev_quiet`) so a manual `/compact` does not leave the hub permanently quiet.

---

## C.5 Verification of the ported base

**fc tests that become agentc tests (with mutation proofs for the critical ones):**

| fc origin | agentc test | proof |
|---|---|---|
| `events_test.c` (8ec, 26 checks) | `tests/events_test.c` | drop the quiet gate → `events.quiet.*` + `plugin.event.quiet-*` fail |
| `tools_jobs` `sync.*`/`walk.*`/`abort-par.*` (8d4) | `tests/jobs_test.c` | make `run_sync` ignore the deadline → `jobs.sync.timeout` fails |
| `withnet_agent_test` `agent.trunc.*` (25f479e) | `tests/agent_loop_test.c` `agent.trunc.call-closed` | revert `wants_tools` → fails |
| `compact_test` `compact.version.*`/`.degrade.*`/`.tail.*` (8b/8d1/8d3) | `tests/session_test.c` + `compact_test.c` | remove ftruncate → `session.degrade.*` fails |
| `safety_test.c` `safety.out.*`/`safety.json.*`/`safety.fmt.*`/`safety.glob.*` (1a/2a/4/8c) | `tests/safety_test.c` + `fmt_test.c` + `tools2_test.c` | bypass `out_safe_write` → terminal-byte check fails; recursive glob → timeout |
| `fault_test.c` `fault.child.bash.*`/`fault.child.mcp.*` (8eb) | `tests/tools_test.c` + `tests/mcp_test.c` | single-pid kill → `bash.group-kill` fails |
| `crypto_test` `auth.merge.*` (8a) | `tests/oauth_test.c` `auth.merge.*` | revert merge helper → fails |
| `provider_test` `provider.registry.*`/`.priv.*` (8e3) | `tests/provider_test.c` | restrict lookup to the first row → `provider.registry.additive` fails |
| `props_test.c` number/string/url/session round-trips (8e9) | `tests/jsonw_test.c` + `fuzz_test` seeds | sign/escape property mutation |

**agentc suites extended:** `json_test` (two live arenas), `mcp_test` (trampoline removal, >256 tools, call id, stop escalation), `plugin_test` (cap removal, v2 identity, quiet), `tools_test`/`tools2_test` (job classification, glob budget), `session_test`/`compact_test` (version, degrade, replay, dual argument shapes), `provider_test` (descriptor table, priv lifecycle), `tui_test` (sanitizer + history regression), `modes_test` (shared setup), `agent_loop_test` (closure), `fuzz_test` (bounded glob + arena fuzz corpus).

**New seam tests:** `agentc_json_parse_in` with two live documents; a fake `start`/`step` tool asserting poll-loop ordering and `tool_checkpoint`; an event subscriber plus the plugin bridge receiving one quiet-suppressed turn; `os_spawn_group` proven through a grandchild kill (also under Wine); a legacy session fixture (raw `arguments` object + version 1) proving the dual reader.

**Gate additions:** run the qemu aarch64/riscv64 golden loop for Q2/Q6/Q7/Q8/Q10 (they touch plat/goldens), `make wine-check` for Q2/Q7/Q8, and `nix build .#release-all` before tagging. Add an ASan/UBSan test link for the JSON/arena and job-driver batches if agentc's Makefile gains one; otherwise lean on `agentc_mem_live` balance checks in each new test.

---

## C.6 Risks and non-goals

**Deliberately not ported (and why).**

- fc `src/plat/`, `src/net/`, `src/net/tls_shim.c`, vendored-CA arrangement — agentc already has equivalent 3-arch + 3-OS backends and mbedTLS/SecureTransport/SChannel wrappers. Only the two append-only primitives (`os_spawn_group`, `os_ftruncate`) are imported.
- fc TUI `frame.c`/`editor.c`/`tui.c`/input renderer — agentc's TUI is further along (inline scrollback, width table, dual sanitizer); only the sanitizer choke point outside the TUI and the history regression test move.
- fc build system, `build.sh`, fuzz/UPDATE harness naming, `-Werror` plumbing — agentc's Makefile/Nix matrix is the release contract; keep it.
- fc `base/loop` — fc demoted it to a plugin defer pump (D2); agentc has no equivalent need.
- fc `base/dir` — agentc's `agentc_dir_scan` is implemented on all platforms and every walker is bounded; a new iterator buys nothing.
- fc `jsonw_num`/float formatting — agentc serializes only integers; adding floats would introduce platform-dependent formatting into golden tests.
- fc `proc_reap(pid, deadline)` naming — keep each loop's non-blocking wait (fc itself did not adopt it).
- fc session-as-event-subscriber — agentc's typed `AgcMsgObserver` is the accepted deviation (fc ADR-41).
- fc's provider-flavoured `stop_reason` normalization — a separate change; the descriptor port keeps the current vocabulary.
- fc `--resume` picker, pty/ctty support, static-plugin registry (`fc_plugin.h`), record/replay format — not part of the runtime/core architecture port.
- The plugin `AGENTC_PTOOL_THREADSAFE` promise remains unhonoured (documented in Part B); this port does not add threads.

**Top three execution risks.**

1. **JSON arena migration × trampoline removal.** Q1 and Q2 touch the same callbacks (`ext/mcp.c`, `ext/plugin.c`) from opposite directions: Q1 changes what arena a parse uses, Q2 changes how the tool is invoked. A stale global-arena parse can silently corrupt a live MCP `result` pointer. Mitigation: freeze Q1 before Q2, migrate MCP/plugin first, and add the two-live-document check to the MCP callback test.
2. **Platform backends vs the Wine/qemu green.** `os_spawn_group`/`os_ftruncate` must be correct on Windows (job objects, `SetEndOfFile`) and macOS, where CI only object-compiles; a wrong `-pid` kill on Windows can hang `make wine-check` or, worse, kill the test runner. Mitigation: append-only prototypes, per-backend implementations in the same commit, `make windows`/`make wine-check` on every Q7 commit, and a group-kill test that asserts grandchild death rather than exit codes.
3. **Session format + goldens churn.** The `arguments` string and version refusal change `--continue` bytes that `session_check.py`, `tests/data/session.expected` and e2e depend on; ASCII JSON changes `mode_json` goldens. Mitigation: dual-shape reader with a committed legacy fixture, regenerate goldens deliberately in the same commit, keep provider request bytes UTF-8 so wire/net goldens do not move, and run the full qemu/Wine matrix once per affected batch rather than only at the end.

A residual risk worth naming: the tool job driver makes sync tools (all MCP and plugin tools) run inline in the parallel loop, exactly as fc's O4 caveat describes. That is a behavior-preserving outcome, not a regression, but the port must not claim uniform parallelism: only tools with `start`/`step` (bash) overlap.
