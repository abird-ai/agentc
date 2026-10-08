# agentc

A minimal, extensible coding agent in **freestanding C23** — zero libc, static
binaries, direct platform networking, vendored mbedTLS, and a versioned C ABI for C
and Rust extensions.

- **~0.7 MB RSS at cold start**, **~2–4 MB** in a working session — roughly 0.7 MB plus
  2.5× the on-disk transcript
- Under 1 ms to launch; the 12 MB session budget holds up to ~4 MB transcripts
- **~916 KiB stripped** (~1.9 MiB with debug info) — 312× smaller on disk than codex, and
  ~285× less session memory than Claude Code
- **One tree → six targets:** Linux x86-64/aarch64/riscv64, macOS arm64/x86-64, Windows x86-64
- **MCP first** — tools, prompts, resources — then a versioned C ABI (`include/agentc_ext.h`)
- Benchmarked against [pi](https://github.com/earendil-works/pi) for UX and extensibility

`make bench` reports cold-start time, peak RSS and binary size for agentc on your machine;
[Footprint](#footprint) measures the same four numbers against codex, Claude Code and pi.

## Footprint

Four agents doing the same job, measured on one Linux x86-64 machine. Everything below is
observed, not estimated: peak RSS is `VmHWM`.

| agent | version | on-disk payload | cold start | cold-start RSS | first TUI paint | session RSS |
|---|---|---:|---:|---:|---:|---:|
| **agentc** | 0.5.0 | **916 KiB** static binary, no runtime | **0.3 ms** | **0.7 MB** | **~1 ms** | **0.7 MB** |
| codex | 0.161.0 | 279 MiB native binary | 9 ms | 25 MB | ~20 ms | 85 MB |
| Claude Code | 2.1.293 | 241 MiB single-file binary | 9 ms | 39 MB | ~180 ms | 191 MB |
| pi | 0.99.2 | 17.5 MiB bundle + Node 24 (75 MiB) | 249 ms | 113 MB | ~800 ms | 209 MB |

- **Cold start** is `--version` (process start to exit, best of three) via `tools/measure.c`.
- **Session RSS** is the peak while the TUI sits at its prompt in a pty, sampled from
  `/proc/<pid>/status` every 50 ms; **first TUI paint** is the first byte it writes. Both
  include whatever the agent does before it paints.
- **Artifacts:** agentc is `make release`; codex and Claude Code are their vendor Linux
  x86-64 release tarballs — self-contained native executables at 279 MiB and 241 MiB; pi is
  its npm bundle plus the Node runtime it requires (Node is shared, not pi's own).
- The gap is the runtime, not the feature set. agentc ships no interpreter and no GC, so at
  the prompt it maps ~0.4 MB of its own text and holds ~0.2 MB of heap. The other three
  hold 85 MB (codex), 188 MB (Claude Code) and 139 MB (pi) of heap and shared memory —
  before a transcript exists.

## Quick start

```sh
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/abird-ai/agentc/master/install.sh | sh

# Windows (PowerShell)
irm https://raw.githubusercontent.com/abird-ai/agentc/master/install.ps1 | iex

agentc setup                      # pick a provider, store credentials, pick a model
agentc "summarise this repository"
```

![agentc running in a terminal: version 0.5.0, the selected tool backends, the ready prompt and the status line](assets/quickstart.svg)

The installer verifies the release archive's SHA-256 and installs to `~/.local/bin`
(`AGENTC_INSTALL_DIR` overrides, `AGENTC_VERSION=0.5.0` pins).

## Build and test

```sh
make                      # -> build/agentc   (native: Linux x86-64, macOS)
make check                # offline golden suite + extension pipeline
make bench                # startup time, RSS, binary size
make install PREFIX=~/.local
make dist                 # release tarball + SHA256 -> dist/

make ext-pipeline         # C + Rust extension harness -> build/ext_harness
./tests/e2e.sh            # mock provider: tool round-trip, --continue, pty TUI
./tests/net.sh            # live loopback HTTP/SSE/TLS
./tests/mcp.sh            # MCP stdio against a python mock

make windows              # cross build -> build/agentc.exe (LLVM on PATH)
make wine-check           # run the Windows golden suite under Wine (Linux)
nix build                 # or nix develop
```

**Release matrix** — built by Nix on one Linux runner, attached to the tag:

```sh
nix build .#release-all
ls result-release/bin/
# agentc-linux-x86_64  agentc-linux-aarch64  agentc-linux-riscv64
# agentc-windows-x86_64  agentc-macos-arm64  agentc-macos-x86_64
# agentc-macos-universal
```

- Linux builds are freestanding on three architectures.
- Windows links no CRT/SDK; import libraries are generated from `src/win/*.def`.
- macOS slices carry the Apple SDK (fetched by Nix) and link libSystem through LLVM's
  Mach-O linker; `llvm-lipo` merges them into a universal binary.

To cross build by hand:

```sh
make release CC="clang --target=aarch64-unknown-linux-gnu" ARCH_FLAGS=-fno-pic \
     REL_OBJDIR=build/obj/aarch64/release OUT=build/agentc-aarch64
```

## Platforms

| target | builds | CI | driven by hand |
|---|---|---|---|
| Linux x86-64 | yes | golden, e2e, live net/TLS, MCP, extensions | yes |
| Linux aarch64 | yes | golden suite under `qemu-user` | yes |
| Linux riscv64 | yes | golden suite under `qemu-user` | yes |
| macOS arm64 | yes | golden suite, native Mac runner | no |
| macOS x86-64 | yes | same Mac runner | no |
| Windows x86-64 | yes | golden suite under Wine, real `cmd.exe` | no |
| Windows arm64 | yes (PE header checked) | native Wine on an ARM64 runner (non-gating) | no |

*Driven by hand* means a person ran a real session on that platform — not the same as a
suite passing in CI. Linux is exercised end to end, cross-built binaries included.

- **Windows CI covers:** the Win32 syscall surface, CRT-free startup, file/console/socket
  paths, the `WSAPoll` translation, and real `cmd.exe` execution (capture, redirection,
  exit codes, cancellation, background drain, truncation).
- **Still unverified:** Wine is not Windows — SChannel/TLS against a real provider, real
  console behaviour, PowerShell 5.1/`pwsh` execution and Windows ARM64 (Wine does not
  emulate a CPU) are untested. macOS and Windows produce valid Mach-O/PE images, but
  nobody has driven them by hand.

### Help wanted: macOS and Windows

- **macOS** — `make && make check` is enough; mention Apple silicon or Intel, since the
  two slices are built separately.
- **Windows** — `agentc --version`, `--list-models`, then a prompt that calls a tool.
  Most useful from a machine that only has Windows PowerShell 5.1.
- **TUI** — platform differences bite first at resize, unicode width, Ctrl-C, paste, and
  the inline bottom region versus `--tui-mode fullscreen`.
- **Reporting** — an issue with the binary you downloaded and your OS version.

Patches welcome: `src/plat/<os>` and `src/net/<os>` are the only directories a port touches.

**Toolchain:** `clang -std=c23 -ffreestanding -nostdlib -static` (no libc, own allocator,
own `_start`), GNU make, optional python3 for the extension registry. Freestanding targets
build with `-nostdinc` plus `third_party/freestanding/`, so a host libc header cannot leak
in through a vendored library.

**Shell on Windows:** `cmd.exe` by default. The `shell` config key selects `auto`, `cmd`,
`powershell` (7 if installed, otherwise 5.1) or `pwsh`; `AGENTC_SHELL` overrides the file,
and a missing requested shell is a clear error, never a silent fallback.

**Dynamic extensions:** CI builds and loads a real extension library on macOS/Windows
(`make dyn-check`), so runtime loading there is CI-gated rather than hand-tested.

## Providers, models and first-run setup

```sh
agentc setup [--offline]                  # provider, credentials, model
agentc --list-models [filter]             # catalog + discovered
agentc --refresh-models                   # ignore the 24 h discovery cache
agentc --provider ollama --model llama3.2
agentc --provider xai --model grok-4 --api-key $XAI_API_KEY
```

- **Built in:** `anthropic`, `openai`, `google` (native Gemini API), `ollama` (local, no
  key), `ollama-cloud`, plus the OpenAI-compatible presets `openrouter`, `xai`, `deepseek`,
  `groq`, `mistral`, `together`, `gemini` — one adapter, different base URL.
- **Discovery:** built-in catalog plus `GET /models` on the configured endpoint (Ollama's
  `/api/tags` first, so family and size show in the picker). Cached per provider for 24 h in
  `~/.config/agentc/models-cache.jsonc`; `--offline` disables probes and a cache miss is
  never fatal.
- **First run:** a bare `agentc` on a tty with no credential, setup file or flags offers a
  short menu (local Ollama is probed for you), writing `setup.jsonc` and `auth.jsonc` (0600,
  merged so existing keys and OAuth logins survive).
- **Precedence:** flags › `config.jsonc` › `setup.jsonc`; credentials `--api-key` › OAuth ›
  provider env vars › `auth.jsonc`.
- **Extensions add providers too:** a linked extension registers its own wire dialect (request
  builder + SSE mapping) and model metadata via `add_provider`, while core keeps transport,
  retries, credentials and header sanitization (`extensions/fake_provider/` is the reference).

## MCP

- **Tools**, plus prompts and resources:
  - server `prompts` become `/mcp__<server>__<prompt>` commands (bounded synchronous `prompts/get`)
  - server `resources` go through three generic read-only tools: `mcp_list_resources`,
    `mcp_list_resource_templates`, `mcp_read_resource` (text only; `blob` entries are rejected)
- **Extensions** can add skill, prompt and theme roots via `resources_discover`, so prompts and
  named themes ship with the extension without an ABI change.
- **Limits** (over the cap the call fails rather than truncating): 512 prompt registrations,
  128 MCP prompts/server and 512 total, 256 resources/server and 1024 total, 64 KiB prompt text,
  256 KiB resource text, 64 file templates/themes per scan.
- **Resilience:** a failed server retries behind capped exponential backoff
  (`AGENTC_MCP_RETRY_MS=0` disables), status is published through `mcp_servers_change`, and an
  idle stdio server's `list_changed` is picked up on the next pump without a request.

## Interactive use

- `/model [id]` report or switch · `/help` · `/theme [dark|light|<name>]` (named themes from
  `themes/<name>.jsonc`) · `/skill:<name>` · any registered prompt template, including MCP
  `mcp__<server>__<prompt>`, runs as `/name` · `/clear` · `/new` · `/quit`
- **TUI modes:** `inline` (default) owns a fixed region at the bottom of the terminal and keeps
  finished transcript blocks in the terminal's own scrollback, so shell history above the region
  survives. `scrollback` is the append-only renderer; `fullscreen` uses the alternate screen.
  `--tui-mode auto` and the `tui_mode` config key resolve to inline.

## Decisions

- **Language:** freestanding C23; no assembly, no runtime.
- **TLS:** vendored freestanding mbedTLS 3.6.2 on Linux; the OS stacks elsewhere
  (SecureTransport, SChannel). The TLS path never `dlopen`s.
- **Ollama:** through its OpenAI-compatible `/v1` endpoint — one wire path for every
  OpenAI-style provider, no second SSE mapper.
- **Formats:** JSONC config/auth/themes, serde-default JSONL sessions; no cross-agent
  file-format compatibility.
- **Auth:** OAuth subscription logins (Claude Pro/Max, ChatGPT) ship in the initial release.
- **Extensibility:** MCP first, then a versioned C ABI whose tools may be synchronous or
  asynchronous. Extensions link into the binary, or on macOS/Windows load at runtime from
  `<config home>/extensions/` (the Linux freestanding build is static-only).
- **Parallel tool execution** on by default; **MIT** licensed.

## Related and thanks

`agentc` is built with heavy inspiration from [pi](https://github.com/earendil-works/pi) by
Mario Zechner, a minimal, extensible coding agent whose UX, extension model, tool set and
durable/MCP concepts shaped this design. pi is MIT licensed; no pi code or text is copied here.

- [Model Context Protocol](https://modelcontextprotocol.io) — the standard `agentc` speaks
- [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) — the vendored TLS backend on Linux
- [curl CA extract](https://curl.se/ca/cacert.pem) — Mozilla CA trust anchors, replaced by a
  system bundle when one is present
- Built with [clang/LLVM](https://llvm.org), [Nix](https://nixos.org),
  [Rust/cargo](https://www.rust-lang.org) and GNU make

The architecture and design record live in [.agents/](.agents/) — [design/](.agents/design/)
for module contracts, [plans/](.agents/plans/) for forward-looking work. They are agent-facing
and are not part of the release artifacts.
