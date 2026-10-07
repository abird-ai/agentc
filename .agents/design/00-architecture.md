# 00 — Architecture

`agentc` is a coding agent written in freestanding C23. It ships as a single
static binary with no libc, no CRT and no runtime dependency on Linux, speaks to
model providers over its own HTTP/SSE and TLS stack, and runs tools, MCP servers
and C/Rust extensions in one address space behind a versioned C ABI.

This document states the goals, the non-negotiable constraints, the component
map and the invariants that hold across the tree. The layer contracts live in
`.agents/design/10-platform.md` (Layer 0/1), `.agents/design/20-core-agent.md`
(loop, messages, tools, sessions, providers), `.agents/design/30-extensibility.md`
(extensions, MCP, skills, modes) and `.agents/design/40-tui.md` (terminal UI).

## 1. Goals

- **One static binary.** A user downloads an executable and runs it. There is no
  installer, package manager, interpreter or shared library to resolve at load
  time on Linux.
- **Small and fast.** Startup under 1 ms, idle RSS under 12 MB, a
  single-threaded core, no garbage collector, and no per-token syscall.
- **Cross-platform from one tree.** Linux (x86-64, aarch64, riscv64), macOS
  (arm64, x86-64) and Windows (x86-64, plus an arm64 build) differ only in
  `plat/` and `net/`; the build selects the source list.
- **Direct OS access.** The agent talks to the kernel through its own contracts
  rather than through a hosted runtime.
- **Extensible after the fact.** MCP and a versioned C ABI let tools, providers,
  commands, prompts, themes and status segments be added without rebuilding the
  core or destabilising it.
- **Predictable and machine-readable.** Typed streaming events, a JSONL session
  format, and byte-for-byte golden tests over every parser and formatter.
- **MIT-licensed, with no runtime-fetched code.**

## 2. Non-negotiable constraints

1. **Freestanding C23.** The build is
   `clang -std=c23 -ffreestanding -nostdlib -static`, with only the four
   freestanding headers (`<stddef.h> <stdint.h> <stdbool.h> <stdarg.h>`), an own
   `_start`, no libc and no CRT. The compiler may emit calls to
   `memcpy`/`memmove`/`memset`/`memcmp`; those are provided in `src/base/str.c`.
   The Linux static build never `dlopen`s; macOS and Windows can load user
   extension libraries through the OS loader (optional, see
   `.agents/design/30-extensibility.md`).
2. **One static binary with no runtime dependency.** TLS is a vendored
   freestanding mbedTLS build on Linux and the OS stack on macOS and Windows;
   assets (prompt, skills, themes, CA bundle) are embedded at build time.
3. **Error convention: negative Linux errno.** `-EAGAIN`, `-EINTR`, `-ETIMEDOUT`
   and every other failure crosses the tree as a negative Linux errno; `0` or a
   positive value means success. Each OS adapter translates its own codes into
   that one language, so "may block", "retry", "timeout" and "fatal" are
   distinguishable without a second enum.
4. **Layer boundaries are enforced by construction.** Core code includes
   `plat.h`/`net.h` and never an OS header; `plat/<os>/` and `net/<os>/` never
   include core headers; the build picks the source list rather than dispatching
   at runtime. The only exceptions are test doubles (`plat/headless.c`,
   `net/mock.c`) and the TLS handshake state machine.
5. **No stdout/stderr writes except through `agentc_out*`/`agentc_log*`**, so the
   output channel and log level stay controllable for embedders and tests.
6. **Append-only contracts.** Public headers (`include/agentc.h`, `plat.h`,
   `net.h`, `wire.h`, `agent.h`, `session.h`, `config.h`, `oauth.h`, `mcp.h`,
   `ext.h`, `agentc_ext.h`) grow by appending fields and functions, never by
   editing existing fields or signatures.
7. **MIT.** Everything in the tree is written here.

## 3. Layer boundaries

Networking and the operating system are the only parts that cannot be shared C,
so the platform surface is split in two and selected at link time:

- **Layer 0 — `plat.h`.** Files, processes, terminals, time, entropy, memory
  mapping, signals, environment and spawn groups. A C function contract
  implemented per OS (raw syscalls on Linux, libSystem on macOS, Win32 on
  Windows).
- **Layer 1 — `net.h`.** Sockets, DNS and TLS. Sockets and DNS diverge too much
  across OSes to share one surface, and TLS is a state machine, so they get a
  separate header selected at link time.

## 4. Component map

| Component | Root | Responsibility |
|---|---|---|
| **base** | `src/base/` | Allocator, `AgcBuf`/`AgcVec`, strings and bytes, formatting, logging, the JSON parser + arena, globbing, deadlines. |
| **plat** | `src/plat/<os>/` | Layer 0: files, processes, terminals, time, entropy, mmap, signals, environment, spawn groups, dynamic-library loading hooks. Also `plat/headless.c` and test doubles. |
| **net** | `src/net/<os>/` | Layer 1: sockets, DNS, TLS. `net/mock.c` is the replay backend for tests; the Linux TLS shim drives vendored freestanding mbedTLS. |
| **wire** | `src/wire/` | URL parsing, HTTP/1.1 including chunked transfer, hand-rolled SSE, and the JSON writer. |
| **core** | `src/core/` | Messages and content blocks, prompt assembly, the agent loop, transcript, tools and the async job table, sessions, model discovery, auth/OAuth, config, resources, prompts, the status registry and the typed event hub. |
| **prov** | `src/prov/` | A provider descriptor registry, one adapter per wire dialect, and the model catalog plus runtime model registry. |
| **app** | `src/app/` | `main.c` (CLI, modes), `setup.c` (first-run, discovery, onboarding), `mode.c` (shared mode setup/teardown), policy and project trust. |
| **tui** | `src/tui/` | Terminal backend, cell grid renderer, editor, input parser, markdown renderer, chat components, themes, status footer. |
| **ext** | `src/ext/` | The extension host and registry, the MCP client, the static loader and the macOS/Windows dynamic loader. `include/agentc_ext.h` is the public ABI; `include/ext.h` is the internal host surface. |

`include/` holds the contracts; `src/` holds one module per file with a header
comment that states the contract.

## 5. Memory and ownership discipline

- **Allocator.** Power-of-two size classes (32 B–64 KiB) carved from 1 MiB `mmap`
  chunks; larger blocks get their own mapping. `agentc_alloc` returns **zeroed**
  memory; `agentc_free` returns small blocks to per-class freelists and unmaps
  large ones; `agentc_realloc` uses `mremap` where available.
- **Containers.** `AgcBuf` (ptr/len/cap byte buffer) with
  `reserve/push/cstr/u64/…` and `AgcVec` (fixed-size items) with `vec_push`.
  There is no general hashmap: every hash table in the tree is bespoke and small.
- **JSON is a recursive-descent parser into a chained arena**, one document at a
  time, with `get/get_str/get_int/at/len/type` and a writer (`wire/jsonw.c`). One
  reset frees the document; nothing is reference-counted. `AgcJsonArena` plus
  `agentc_json_parse_in/clear` supports multiple documents and key/value
  iteration; the single-document `agentc_json_parse()` remains for simple
  callers.
- **The composer document** is a flat `AgcBuf` with a byte cursor and anchor;
  the kill ring, history and collapsed paste bodies are separate owned buffers.
  There is no general undo tree.
- **Ownership is by convention and comment** ("free it"), never enforced.
  Long-lived structs document their owner in a header comment.
- **Long-lived structures** (the agent, session, transcript, tool jobs, status
  registry, theme roots) are allocated once and freed on shutdown. Short-lived
  JSON documents are freed by one arena reset, so parsing a request does not
  leave partial state behind.
- **The render path** performs no per-cell, per-row or per-glyph allocation; grid
  storage changes only on resize.
- **Fatal paths.** `agentc_die` runs the registered `agentc_atexit` cleanup hook,
  so an out-of-memory exit restores the terminal instead of leaving it in raw
  mode on the alternate screen.

## 6. Event loop and concurrency

- The core has no general reactor. Each layer polls with `os_poll` (Layer 0)
  on its own bounded timeout: the TUI and the tool job table poll in short slices
  (5–20 ms), and `agentc_pump_install()` plus a shared HTTP poll hook let a
  streaming request keep input, timers and deferred extension work flowing while
  it is in flight. A turn is draw-if-dirty → flush → poll → callbacks → tick.
- Timers are cooperative (`*_timeout() -> ms` plus `*_tick()`), aggregated by the
  app; there is no `timerfd`.
- The core is single-threaded. Threads exist only on Windows (console reader and
  ConPTY writer).
- `os_spawn` uses `fork`+`execve` with `dup2` stdio, `setsid`+`TIOCSCTTY` for a
  controlling terminal and signal resets; the read end is non-blocking and polled
  by the tool job table. PTYs are `/dev/ptmx` + `TIOCSWINSZ`. Windows uses
  `CreateProcessW` plus job objects and ConPTY with a writer thread and a ring
  buffer.
- Tool calls in one turn execute **concurrently** through a job table; tool-end
  events fire in completion order and tool-result messages append in source
  order.
- The agent loop emits typed events in a fixed order — `agent_start`,
  `turn_start`, `message_start/update/end`, `tool_execution_start/update/end`,
  `turn_end`, `agent_end` — through one typed event hub
  (`src/core/events.h`). Exactly one observer writes the session file.

## 7. Extension philosophy: JSON at boundaries, typed structs internally

- **Every `const char *` that crosses the extension ABI is a NUL-terminated
  UTF-8 JSON string** unless a field's comment says otherwise. Internally the
  same data is carried as C structs (`AgcMsg`, `AgcUsage`, `AgcTool`, …) with
  explicit types; JSON is parsed at the edge and produced at the edge, never as
  the in-process representation.
- **One model.** Built-in tools, MCP servers and statically linked extensions are
  all extensions: they register tools, providers, commands, prompts, themes,
  hooks and status segments through the same host surface.
- **One ABI, append-only.** `include/agentc_ext.h` carries a single
  `abi_version`, `struct_size` checks and append-only structs. New fields go at
  the end; incompatible changes bump the version. `AGENTC_EXT_FIELD_OK` and
  `AGENTC_EXT_HOST_HAS` gate appended contribution and host fields so a caller
  never reads past its own `struct_size`.
- **The host owns lifetimes.** Memory returned to the host comes from
  `host->alloc` and the host frees it; registered strings are copied at the
  `add_`/`on` call, so an extension may free its own storage immediately.
- **Safety caps.** Hook handlers, status keys, extension HTTP requests, deferred
  callbacks, appended session entries, and custom providers are all bounded
  process-wide; overflow is counted and logged. A malformed contribution is
  dropped rather than trusted.
- **Tools may be synchronous or asynchronous**, with `ud`, `timeout_ms`, `run`,
  `start` and `step` on the tool record; the tool engine drives them
  (`.agents/plans/P5-TOOL-ENGINE.md`).
- **Providers are data.** A provider is a descriptor row (name, API dialect, base
  URL, auth) plus optional static model metadata; adding a chat-completions-style
  endpoint is a preset entry, not a new code path. Extensions can contribute a
  wire dialect and models through `add_provider`.
- **Resources and UI.** `resources_discover` contributes skill, prompt and theme
  roots; `set_status` and `add_status` feed the status-line registry; `notify`
  and `set_title` reach the active front end through an injected UI sink. There
  is no privileged path into the core.
- See `.agents/design/30-extensibility.md` for the full surface and the planned
  RPC/SDK axes in `.agents/plans/P5-AXES.md`.

## 8. Performance and size budget

| Budget | Target |
|---|---|
| Startup (Linux x86-64, release) | under 1 ms, own `_start`, no libc init |
| Idle RSS | under 12 MB |
| Release binary | a few hundred KB static (Linux x86-64 release is about 750 KiB) |
| Render cadence | dirty-driven; at most one frame per 100 ms while a run is active |
| Core threads | one |

How the budget is kept:

- a chained arena per JSON document, freed with one reset;
- a zero-copy TLS → HTTP → SSE pipeline with no intermediate DOM for streamed
  deltas;
- coalesced renders: deltas append to a buffer and are committed once per frame,
  not once per token;
- `TCP_NODELAY` on request sockets and vectorized `memchr` loops with a scalar
  fallback;
- static buffers for prompt construction;
- grid storage allocated on resize, not per frame;
- `epoll` on Linux as MCP fan-out grows.

Every published performance claim compares `agentc` against **a high-level agent
runtime** *and* against **a plain C baseline of the same core** (hosted C, libc
allowed, no agent scaffolding), never against a high-level runtime alone. Method
and hardware accompany every number.

## 9. Testing

- `tests/*.c` are standalone programs linked against the base library and the
  test doubles; stdout/stderr is compared byte-for-byte with
  `tests/data/*.expected`. Non-determinism is a bug, so time, randomness and
  network are injectable.
- A headless backend plus a scripted transcript DSL (`key/type/click/…/resize/quit`
  and `print-*` state dumps) drives the UI; `tests/run.sh` compares the whole
  transcript under a fake `HOME`/`XDG_*`.
- Shell suites cover what goldens cannot: `tests/e2e.sh` (mock provider through a
  pty, sessions, onboarding), `net.sh` (live loopback HTTP/SSE/TLS), `mcp.sh`
  (stdio MCP against a mock server), `ext.sh` (C and Rust extensions).
- Fuzzers drive json/sse/markdown/input/diff with random bytes under a debug
  build with guard pages and red zones around arenas.
- **Rule: if it parses or formats anything, it has a golden test.**

## 10. Out of scope

A JavaScript/TypeScript extension runtime; a wide provider matrix; image or
classifier models; durable client-server layers; a sandboxed code-mode;
telemetry; HTML export; GUI toolkits; and any cross-agent file-format
compatibility. Extensions are C/Rust extensions, MCP servers, config, skills and
prompt templates — nothing else.

## 11. Why C23 and not hand-written assembly

For a program of this shape — static, single-threaded, syscall-bound, no runtime
— freestanding C23 is equivalent to assembly on the metrics that matter:

- **Startup.** Own `_start`, no libc init, no relocations beyond the ELF loader:
  a handful of syscalls and page faults for the skeleton.
- **RSS.** The budget is a data-structure property, not a language property; the
  allocator and arena design are identical in either language.
- **No-runtime guarantee.** The same `-nostdlib -static` link and direct
  syscalls; the compiler cannot smuggle in a runtime that is not on the link
  line.
- **Velocity and density.** The modules that dominate the product — HTTP/SSE
  parsing, the JSON writer, the editor, the TUI renderer — are faster to write
  and smaller for the same product in C. Assembly's edge is on tiny hand-picked
  kernels; across a ~20k-line agent it inverts.
- **Cross-platform targets.** macOS, Windows and the non-x86-64 Linux
  architectures are straight clang targets; only `plat/` and `net/` differ.
- **Auditability.** A C header plus `-ffreestanding -nostdlib` is a checkable
  contract.

## Inspiration

The interactive coding UX and the extension model are inspired by
[pi](https://github.com/earendil-works/pi), a minimal coding agent. Everything
else — the freestanding C23 implementation, the allocator, wire stack, tool
engine and terminal UI — is built here.
