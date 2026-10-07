# AGENTS.md — working on agentc

`agentc` is a freestanding C23 coding agent. No libc, no runtime, static binaries.
The architecture and module contracts live in `design/`; the public interfaces are
the headers in `../include/`. Paths below are relative to the repository root: run
everything from there, not from this directory.

`agentc` takes heavy design inspiration from [pi](https://github.com/earendil-works/pi)
(MIT, © Mario Zechner). The design record notes where an idea comes from pi and
where we deliberately diverge; treat pi as the reference UX and extension model,
not as a code source.

## Build and test

```sh
make                   # release binary -> build/agentc (Linux or macOS host)
make test              # test binaries  -> build/test/<name>
make check             # test + ./tests/run.sh (offline golden suite + extensions)
make debug             # unoptimized binary -> build/agentc-debug
make windows           # Windows x86-64 cross build -> build/agentc.exe (LLVM on PATH)
make windows WIN_ARCH=arm64     # ... or Windows ARM64
make release PLATFORM=mac CC='clang --target=arm64-apple-darwin -isysroot $SDK -fuse-ld=lld' \
     OUT=build/agentc-macos-arm64   # macOS cross build (Apple SDK; nix does this)
make win-tests         # Windows golden tests -> build/test/<name>.exe
make wine-check        # Windows golden suite under Wine (Linux; real cmd.exe)
nix build .#release-all  # linux x86-64/aarch64/riscv64, windows x86-64/aarch64,
                         # macos arm64/x86-64 + universal
make live-net          # live HTTP/SSE/TLS client -> build/net_live
make ext-pipeline      # C + Rust extension harness -> build/ext_harness
make release-exts      # release with extensions/manifest.json linked in
make bench             # startup, RSS, binary size
make install PREFIX=~/.local
make dist              # release tarball + sha256 -> dist/

./build/agentc             # interactive TUI; inline bottom region by default
./build/agentc --tui-mode fullscreen   # alternate-screen viewport
./build/agentc --tui-mode scrollback   # append-only, no owned region

./tests/run.sh         # golden-file suite (offline, deterministic)
./tests/e2e.sh         # mock provider: round-trip, --continue, pty TUI
./tests/net.sh         # live loopback HTTP/SSE/TLS
./tests/mcp.sh         # stdio MCP against a python mock
./tests/ext.sh         # C + Rust extension examples
sh tests/run-built.sh  # golden suite for prebuilt test binaries (macOS)
```

Always run `make check` (or `./tests/run.sh`) before committing; it must stay green.
Before a commit that touches providers or the TUI also run `./tests/e2e.sh` (mock
provider, pty TUI, onboarding) — it needs no network.

## Hard rules

- Freestanding only: `<stddef.h> <stdint.h> <stdbool.h> <stdarg.h>` are the
  only permitted includes. No stdio/stdlib/string/unistd. The contracts are
  `agentc.h`, `plat.h`, `net.h`, `wire.h`, `agent.h`, `session.h`, `config.h`,
  `oauth.h`, `mcp.h`, `ext.h` and `agentc_ext.h`; extend them by appending,
  never by editing existing fields or signatures.
- The TUI status line is a provider-built segment list (`include/status.h`): the
  built-ins (`src/core/status_builtin.c`) and extensions both register through
  the status table (`agentc_status_register()`, reached from `host->add_status`).
  Never add a private path into `comp_footer()`.
- Errors are negative linux errno values; 0/positive means success.
- Memory: `agentc_alloc`/`agentc_realloc`/`agentc_free` (zeroed allocations).
  Long-lived structures document ownership in a comment. The compiler may emit
  calls to `memcpy/memmove/memset/memcmp`; they are provided by `src/base/str.c`.
- Core code calls only Layer 0 (`plat.h`) and Layer 1 (`net.h`) — never an OS
  API directly. Platform code lives in `src/plat/<os>/` and `src/net/<os>/`.
- No stdout/stderr writes except through `agentc_out*`/`agentc_log*`.
- One module per file, header comments explain the contract, golden tests for
  every parser or formatter.
- Providers are data, not code paths: a new OpenAI-compatible endpoint is a preset
  entry in `src/app/setup.c` plus an env-var row in `src/core/auth.c`. Model lists
  are discovered at runtime (`src/core/discover.c`) into the dynamic registry in
  `src/prov/models.c`; keep the built-in catalog small.
- `grep`/`find`/`ls` keep one name and one JSON schema; the backend (external
  `rg`/`fd`/POSIX vs the built-in implementations) is selected once at startup
  through `src/core/tools/engine.{c,h}` and is invisible to the model.

## Layout

```
.agents/    this directory: AGENTS.md, design/ (current-state contracts), plans/
include/    agentc.h base · plat.h Layer 0 · net.h Layer 1 · wire.h HTTP/SSE · agent.h core
src/base/   allocator, buffers, strings, fmt, log, json, glob, out, deadline
src/plat/   linux/ · mac/ · win/   (one implementation per OS)
src/net/    linux/ sockets+dns+tls_shim · mock.c replay backend for tests
src/wire/   url, http, sse, jsonw
src/core/   messages, prompt, agent loop, tools/ (incl. engine), discover, auth
src/prov/   anthropic, openai (+ ollama/presets), codex, google, models
src/app/    main.c (modes, CLI) · setup.c (providers, discovery, onboarding) ·
            mode.c (shared mode setup: agent/session, JSONL events, rebuild)
src/tui/    terminal renderer, input, editor, streaming transcript, tool cards
src/ext/    extension registry, MCP client, dynamic loader (macOS/Windows)
tests/      *.c + data/*.expected golden files, e2e/net/mcp/ext shells
third_party/ vendored mbedTLS 3.6.2 + CA bundle
crates/     agentc-ext, the Rust wrapper over the C extension ABI
extensions/ C + Rust example extensions and their manifest
tools/      generators (assets, catalog, extension registry) and benchmarks
```

## Architecture seams

One line each; details live in the headers and `design/`.

- Per-document JSON arenas: `AgcJsonArena` + `agentc_json_parse_in`; the default
  arena `agentc_json_parse()` remains for simple callers.
- Tool v2: `ud/timeout_ms/run/start/step` plus the bounded driver in
  `src/core/tools/jobs.h`; the engine layer (`src/core/tools/engine.h`) selects
  the external or built-in runner per tool.
- Typed event hub `src/core/events.h`: per-agent primary + subscribers + a global
  quiet gate that also silences the extension bridge.
- Provider registry `src/prov/provider.h`: one descriptor row per provider, plus
  dynamic rows added through the extension ABI.
- Session durability: one append choke point, `os_ftruncate` rollback, newer
  version refusal, compaction replay, arguments stored as a JSON string.
- Platform: `os_spawn_group` / group kill and bounded non-blocking reaps.
- Wire: `AgcJsonW.ascii` (opt-in `\uXXXX` output) and MCP line/schema caps.
- `src/base/out.{h,c}` is the single terminal-safe output choke point;
  `src/base/glob.c` is the bounded iterative matcher.
- `src/app/mode.{h,c}` is the shared setup for print/TUI/JSON/RPC front ends.

## Working in parallel

Agents may work in git worktrees (`../agentc-<topic>` branches) with disjoint
file ownership and merge at agreed gates. Never edit files another stream owns;
if you need a contract change, append to the header in your branch and note it
in your report. Commits: `git -c commit.gpgsign=false commit`.
