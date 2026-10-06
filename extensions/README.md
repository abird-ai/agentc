# Native extensions

`agentc` extensions are C (or Rust) objects linked into the binary, or — on
macOS and Windows — a shared library loaded at runtime. On Linux the binary is
freestanding and static, so there is no `dlopen`: only linked extensions exist.
`tools/gen-exts.py` reads `extensions/manifest.json` and
emits a `{name, entry}` table (`build/exts.c`) that `src/ext/linked.c` includes
and `agentc_ext_register_linked()` walks at startup. Each row is guarded by
`#ifdef AGENTC_STATIC_EXT_<ident>` and is `NULL` otherwise — deliberately **not**
a weak reference, because lld's Mach-O linker rejects a weak import it cannot
resolve.

```
extensions/
  manifest.json          which extensions are part of this build
  hello/hello.c          C example: echo tool + tool_call veto (HELLO_VETO)
  rust_echo/             Rust example (staticlib) using crates/agentc-ext
  fake_provider/         C example: typed custom provider (see below)
  async_demo/async_demo.c  C example: asynchronous tools (see below)
crates/agentc-ext/       no_std wrapper for include/agentc_ext.h
```

The ABI itself is frozen in `include/agentc_ext.h`; extensions only include that
header (`<stddef.h>`/`<stdint.h>` are enough otherwise). Host-side helpers live
in `include/ext.h` (the app-facing registry API) and `src/ext/registry_int.h`
(internal to core) and are not part of the extension contract.

## Build

```sh
# 1. build the extension harness: every C + Rust entry in the manifest
make ext-pipeline          # -> build/ext_harness (C + Rust examples + fixture)

# 2. build a release binary with every manifest extension statically linked
make release-exts          # -> build/agentc

# 3. optional: build the hello example as a runtime-loadable shared object
make ext-dylib             # -> build/extensions/hello.so   (hello.dylib on macOS)
make win-ext-dylib         # -> build/extensions/hello.dll  (+ nosym.dll), cross
```

The manifest **is** the build list. `tools/gen-exts.py` writes both `build/exts.c`
(the `#ifdef`-guarded registry) and `build/exts.mk` (the per-entry build rules the
Makefile `-include`s): each `c` entry is compiled with
`-Dagentc_ext_init=agentc_ext_<ident>_init` plus its manifest `defines`, each
`rust` entry is built with cargo into `build/cargo/<ident>/release/lib<lib>.a`
and force-linked with `-Wl,-u,agentc_ext_<ident>_init`. A new C or Rust extension
needs only a `manifest.json` entry — no Makefile edit. `release-exts` also passes
`-DAGENTC_STATIC_EXT_<ident>` per linked entry; a binary that links no extension
still builds (the row is `NULL` and `agentc_ext_adopt` logs `not linked` and skips
it). `build/exts.mk` is a real make target, not a parse-time side effect:
`gen-exts.py --mk-out` writes it atomically when the manifest or the generator
changes, every object depends on it, and a validation error fails the build. The
generated rules are also the readable reference for compiling one extension by
hand while debugging.

Symbols are per extension (`agentc_ext_<manifest name>_init`) because a static
binary cannot have several `agentc_ext_init` definitions. C sources keep writing
`agentc_ext_init` and the build renames it with `-D`; Rust staticlibs export the
per-ident symbol directly (never the canonical `agentc_ext_init`, which would
collide when two Rust staticlibs link one binary).

### Linking into `build/agentc`

`make release-exts` builds a release binary with every extension in
`extensions/manifest.json` statically linked (C sources are compiled with
`-Dagentc_ext_init=agentc_ext_<ident>_init`, Rust staticlibs under
`build/cargo/<ident>/release/` are picked up automatically and forced in with
`-Wl,-u,agentc_ext_<ident>_init`). `make ext-pipeline` builds the test harness
instead.

## Runtime wiring (main.c)

```c
agentc_ext_register_defaults(no_mcp);          /* builtin-tools, builtin-context, mcp */
agentc_ext_register_linked();                  /* build/exts.c */
agentc_ext_register_dynamic();                 /* <config home>/extensions/, macOS/Windows */
agentc_ext_apply_config(cfg);                  /* extensions.disabled: defaults + linked + dynamic */
agentc_ext_load_all();                         /* init, lowest order first */
...
agentc_ext_emit(name, payload_json);           /* per hook point (string payloads) */
...
agentc_ext_pump();                             /* one shared pump duty per main-loop tick */
```

`main.c` composition stays `register_defaults` → `register_linked` →
`register_dynamic` → `apply_config` → `load_all`; the app installs
`agentc_mode_pump` as both the wire
poll hook and the agent pump for every mode, so deferred extension work also runs
during blocking HTTP waits and tool runs. `--list-extensions` prints the
registered descriptors and their `pending`/`loaded`/`failed`/`skipped` state.

Also available in `include/ext.h`:

- `agentc_ext_set_context()` — cwd/session id/session file/system prompt.
- `agentc_ext_set_entry_sink()` — forwards `append_entry` to the session JSONL.
- `agentc_ext_set_ui_sink()` — forwards notify/set_status/set_title.
- `agentc_ext_tools()` / `agentc_ext_commands()` / `agentc_ext_run_command()`.
- `agentc_ext_unload()` / `agentc_ext_shutdown()` — drop one extension or the whole
  registry (tests use this to check the allocator baseline).

`agentc_ext_emit()` returns an `AgcExtResult`; the core reads its decision for
control points (`tool_call`, `input`).

## Dynamic extensions

On macOS and Windows the host also scans one fixed directory at startup:
`<config home>/extensions/` (`$XDG_CONFIG_HOME/agentc/extensions/`, or
`$HOME/.config/agentc/extensions/` when `XDG_CONFIG_HOME` is unset; the Windows
jobs set `XDG_CONFIG_HOME` explicitly). The file stem is the extension name —
`[a-z0-9_.:-]{1,64}` — and the suffix is `.dylib` (macOS) or `.dll` (Windows,
matched case-insensitively). Files are taken in bytewise stem order, at most 32
per scan (one overflow log); a missing directory is fine. There is no recursion,
no project directory and no `extensions.paths`; `extensions.disabled` filters
dynamic extensions by name exactly like linked ones, because
`agentc_ext_register_dynamic()` runs after `register_linked()` and before
`apply_config()`.

The library must export the canonical `agentc_ext_init` — no per-ident rename,
because the host looks up exactly that name:

```sh
make ext-dylib              # macOS/Linux: build/extensions/hello.dylib or .so
make win-ext-dylib          # cross: build/extensions/hello.dll + nosym.dll
# install (example):
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/agentc/extensions"
cp build/extensions/hello.dylib "${XDG_CONFIG_HOME:-$HOME/.config}/agentc/extensions/hello.dylib"
agentc --list-extensions    # loaded ... hello ... (dynamic)
```

The same `extensions/hello/hello.c` example works both ways; the library is
registered and initialized exactly like a linked extension (same descriptor,
`init`/`shutdown`, hooks, tools).

Rules and safety:

- A loaded library is **trusted native code**: it runs in-process with no
  sandbox, no crash isolation and the full process privileges. Only install
  libraries you trust. There is no signing/notarization/quarantine policy.
- Windows restricts the loader search to the DLL's own directory plus System32
  (no `PATH`/current-directory search) and fails closed if the OS does not
  support those search flags. On POSIX the library file and its `extensions/`
  directory must not be group/other-writable; symlinks are followed, so a TOCTOU
  window remains between the scan check and the open.
- A dynamic library's handle is closed exactly once and only when the extension
  is idle: registrations removed, `shutdown()` returned, no live async job, no
  callback on the stack. A busy close is deferred to the last job unlink, the
  pump epilogue or shutdown; a library still needed when the process shuts down
  stays mapped with a warning. `agentc_ext_unload(name)` is the registry API that
  requests it (there is no CLI unload command).
- The descriptor's `out.name` must equal the file stem; duplicates are skipped
  before the library is opened, and a failing candidate (bad image, missing
  symbol, refused descriptor) is logged and skipped without disturbing a good
  sibling.
- Linux stays static-only: `--list-extensions` shows no dynamic rows and
  `agentc_ext_register_dynamic()` is a no-op. There is no custom ELF loader.
- The macOS/Windows runtime halves are CI-gated (`make dyn-check` under Wine,
  `tests/dyn.sh` on `macos-14` and `windows-latest`); a local Linux run covers
  the fake-loader golden lifecycle and the cross builds only.

`tests/dyn_test.c` (+ `tests/data/dyn_test.expected`, 79 checks) is the offline
golden lifecycle with a fake injectable loader (scan/filter/order/cap,
registration isolation, close discipline); `tests/dyn.sh [--mac|--windows|--wine]`
is the real-loader gate (and builds `hello.so` on plain Linux as a PIC/ABI
check).

## Status line segments

An extension can contribute to the TUI status line with `host->add_status()`
(see `.agents/design/30-extensibility.md` §2.5 for the full contract):

```c
static size_t my_status(void *userdata, AgcExtStatusSegment *out, size_t max,
                        char *arena, size_t arena_cap) {
    if (max < 1) return 0;
    out[0].struct_size = sizeof out[0];
    out[0].slot = AGENTC_PSEG_SLOT_LEFT;   /* or _RIGHT */
    out[0].priority = 100;                 /* lower runs first */
    out[0].style = AGENTC_PSEG_STYLE_DIM;
    out[0].text = "my-extension";          /* or format into `arena` */
    return 1;
}

/* init */
host->add_status(my_status, NULL);
```

Rules of thumb: text is plain UTF-8 (the host drops control bytes and clips to the
terminal width), the provider must not allocate, do I/O or block, and it is only
called when the status state changes, not every frame. `extensions/hello/hello.c`
registers a `hello` segment; `tests/ext.sh` asserts it through the harness.

## Async tools

An extension tool is synchronous by default (`run`). An I/O-bound tool can
instead be asynchronous: leave `run` NULL and provide the complete appended
`start`/`step`/`stop` trio (`run` non-NULL wins; a partial trio is rejected at
registration). The host drives the tool on the main loop, so `start`/`step` must
not block — use `host->defer`/`host->http_request`, and the adapter pumps after
each running step.

```c
static int my_start(const AgcExtHost *host, const AgcExtTool *self,
                    const AgcExtToolCall *call, void *out, bool *is_error,
                    void **state) {
    MyState *st = host->alloc(sizeof *st);   /* extension-owned */
    if (!st) return -12;                     /* start < 0: stop is NOT called */
    *state = st;
    return 0;                                /* 0 running, 1 complete, <0 fatal */
}

static int my_step(const AgcExtHost *host, const AgcExtTool *self,
                   const AgcExtToolCall *call, void *state, void *out,
                   bool *is_error) {
    (void)state;
    if (host->is_cancelled(host, call->signal_token)) {
        *is_error = true;                    /* the result is an error */
        return 1;                            /* stop(FINISHED) */
    }
    host->out_write(out, "tick\n", 5);       /* per-step staged output */
    return 0;                                /* still running */
}

static void my_stop(const AgcExtHost *host, const AgcExtTool *self, void *state,
                    int reason) {
    host->free(state);                       /* exactly once: FINISHED/ERROR/
                                                TIMEOUT/CANCELLED/UNLOAD */
}
```

Rules:

- `stop(state, reason)` is mandatory and runs exactly once for every `start`
  that returned `>= 0`; `FINISHED` means "no more steps", and `*is_error` is
  the result. Reasons: `AGENTC_EXT_TOOL_FINISHED` (0), `_ERROR` (1),
  `_TIMEOUT` (2), `_CANCELLED` (3), `_UNLOAD` (4). A fatal step or timeout
  replaces staged output with the driver's error text; cancel keeps partial
  output.
- `void *state` is extension-owned (`host->alloc` in `start`, `host->free` in
  `stop`); the host never frees it. The `call` view (including `signal_token`)
  stays valid through `stop`.
- Cancellation is polled with `host->is_cancelled(call->signal_token)`; the
  token is valid from `start` through `stop`. There is no timeout query.
- Async `timeout_ms` is clamped to `AGENTC_EXT_TOOL_TIMEOUT_MAX_MS` (1 800 000 ms,
  30 min) with a log; `0`/negative means the driver's 120 s default. Tool errors
  are tool results, not agent failures.
- A tool unloaded/removed mid-job gets `stop(UNLOAD)` and the record is held
  until its last job unlinks; a later step synthesizes
  `error: extension tool unloaded`.
- Limits: no preemption of a blocking callback, no per-step watchdog, no
  progress/structured-content ABI. See
  `.agents/design/30-extensibility.md` §2.4.

`extensions/async_demo/async_demo.c` is the reference (completing, optional
`fail_at` tool-result error, fatal, timeout and cancel tools plus the
`async-demo-debug` counter command used by `tests/ext.sh`).

## Custom providers

An extension can register a named provider from `init` with the appended host
service `add_provider`. The core owns transport, retries, timeouts,
cancellation, `--record`/`--dump-wire`, `Content-Length`, header sanitization
and credential resolution; the extension only describes its wire dialect.

```c
static const AgcExtProvider my_provider = {
    .struct_size = sizeof(AgcExtProvider),
    .name = "myprov",                    /* [a-z0-9_.:-]{1,64}, unique */
    .label = "My provider",
    .default_base_url = "https://api.example/v1",
    .path = "/chat",
    .env_keys = { "MYPROV_API_KEY", NULL, NULL },
    .needs_key = 1,
    .discover_style = AGENTC_EXT_DISCOVER_NONE,
    .auth = &my_auth,                    /* NONE/BEARER/HEADER; QUERY rejected */
    .models = my_models,                 /* optional static metadata */
    .nmodels = sizeof my_models / sizeof my_models[0],
    .build_request = my_build_request,   /* mandatory */
    .stream_event = my_stream_event,     /* mandatory */
    .stream_open = my_stream_open,       /* optional (NULL = stateless) */
    .stream_finish = NULL,               /* optional */
    .stream_close = my_stream_close,     /* optional */
};

static int my_init(const AgcExtHost *host) {
    if (!AGENTC_EXT_HOST_HAS(host, add_provider)) return 0;  /* older host */
    host->add_provider(&my_provider);
    return 0;
}
```

- `build_request(host, self, view, head_out, body_out)` writes the head block
  and the JSON body into two opaque sinks with `host->out_write`. The view
  (`AgcExtRequestView`) carries provider/model/system/thinking/max_tokens, the
  filtered transcript and the visible tools; every pointer is borrowed for the
  call. The core inserts the blank line, sanitizes the head (drops `Host`,
  `Content-Length`, `Transfer-Encoding` and any occurrence of the declared auth
  header), adds `Content-Length`, and appends the resolved credential as the
  declared auth line. The key is never in the view and never reaches the
  extension.
- `stream_event(host, self, st, event, sink)` receives one parsed SSE event at a
  time and calls the typed `AgcExtStreamSink`: `text`, `thinking`, `tool_start`,
  `tool_args`, `response_id`, `usage`, `stop` (`AGENTC_EXT_STOP_*`), `error`.
  The sink copies every payload immediately, so do not retain pointers from the
  event. Custom providers are SSE-only.
- `stream_open` allocates `st->ud` with `host->alloc`; `stream_close` frees it.
  Both are optional (NULL = a stateless provider). `stream_finish` runs before
  the core resolves the stop; a stream that never signalled a stop fails with
  `stream ended before finish`.
- `auth` is an `AgcExtProviderAuth` (`kind`, `header`, `prefix`). `BEARER`
  defaults the header to `Authorization` and the prefix to `"Bearer "`; `HEADER`
  requires an explicit token-safe header and prepends its prefix verbatim. The
  core applies the credential.
- `models` registers static metadata (`id`, `name`, context window, max tokens,
  reasoning/image flags). Static models survive discovery refreshes, share the
  256-slot runtime table, and are dropped when the extension shuts down.
- The row is validated and copied at `add_provider`; a rejection is logged and
  does not fail `init`. Duplicate names are ignored, the registry caps at 64
  providers, and the row is retired (not freed) when the extension unloads, so
  an in-flight agent gets a clean `provider unloaded` error.
- `providers.<id>.base_url` and `api_keys.<id>` generic user-config entries work
  for an extension provider id too; `--provider <ext>`, `--list-models`, the
  setup picker and the unknown-provider diagnostic all see loaded extension rows
  because composition runs before provider resolution.

`extensions/fake_provider/fake_provider.c` is the complete reference
(request/body/SSE/auth/static model plus an optional forbidden-header probe),
linked from `extensions/manifest.json` and asserted by `tests/ext.sh` through
`build/ext_harness`. Not yet supported: query auth, a custom discovery parser,
cost rates, per-model thinking config, multimodal, OAuth providers and
`providers.<id>.headers`.

## Resource roots and MCP content

`resources_discover` now consumes all three path lists: `skillPaths`,
`promptPaths` and `themePaths`. Validated paths (absolute, no `..`, under the
config home or the trusted project cwd) go into per-kind, process-lifetime root
stores (8 roots each, at most 32 paths per list, deduplicated); the
skills/template/theme loaders scan them in registration order, so a later
same-named resource wins and a root added after startup needs a restart.

- Skill roots feed `SKILL.md` discovery and the TUI `/skill:<name>` command
  (the frontmatter-stripped body is capped at 256 KiB).
- Prompt roots register `*.md` templates (name `[a-z0-9._:-]{1,64}`,
  `description`/`argument-hint` frontmatter) in the one invocable-prompt
  registry, alongside MCP prompts; `/name` (TUI) and the RPC `prompt_template`
  command expand `$1..$n`, `$@`, `${N:-default}`.
- Theme roots resolve `themes/<name>.jsonc` (stem `[A-Za-z0-9._-]{1,64}`) for
  `/theme <name>` and the startup `theme` config key; the extra roots are the
  most specific level.

MCP content: a server that advertises `prompts`/`resources` in its initialize
capabilities is asked for those lists (capability-gated; missing capabilities
fall back to tools-only). `prompts/list` entries become
`mcp__<server>__<prompt>` commands in the same prompt registry, and invocation
does a bounded synchronous `prompts/get` (10 s; `text` blocks only, 64 KiB
cap). `resources/list` + `resources/templates/list` are exposed through the
three generic read-only tools `mcp_list_resources`,
`mcp_list_resource_templates` and `mcp_read_resource {server,uri}` (published
once at the first resource-capable list sync; text contents only — a `blob` is
rejected).

Connection status: a READY stdio server is idle-drained, so an idle
`list_changed` is picked up on the next pump without an exchange; a FAILED
server reconnects behind a capped exponential backoff (base 1 s doubled per
failure, max 60 s) and stays non-pending while it waits — `AGENTC_MCP_RETRY_MS`
overrides the base delay and a zero or negative value disables reconnect.
`mcp_servers_change`
publishes each server's `connecting`/`ready`/`failed` status plus (after
initialize) its advertised `caps`, so a front end can render a server list
without polling.

## Notes and current limits

- `tool_call` may return `{"block":true,"terminate":true}`; the submission
  ends after the batch only if **every** call in the assistant message was
  blocked with `terminate` (a mixed/normal call keeps the loop; cancel wins,
  `turn_end.continue` still re-enters once). `session_before_switch` gates RPC
  `new_session` (`{"cancel":true}`, a blocked/failed handler or a malformed
  decision cancels before any file is touched).
- Deferred work queued with `host->defer` is drained by `agentc_ext_pump()`.
  Extension HTTP requests are queued (8 max) and one pending request runs per pump
  (10 s budget); `host->http_cancel` drops a queued request and interrupts one
  already in flight on its next poll slice, whose callback then gets the
  negative `-ECANCELED` status and the partial body. A synchronous `run` cannot
  wait for the callback (I/O-bound work observes through `defer`); an async tool
  is pumped between its steps.
- Hard process limits: 128 hook handlers (an unknown point name is rejected at
  `on`), 8 live `set_status` keys, 32 levels of emit recursion, and the
  `append_entry` type/size caps. Three consecutive overruns of the 2 ms handler
  budget disable a handler; override overruns also block that occurrence. See
  `.agents/design/30-extensibility.md` §2.8 for the full list.
- Tools are synchronous by default: `AgcExtTool.run(...)` appends result text
  to the opaque `out` sink with `host->out_write` and returns `0 | -errno`. An
  async tool leaves `run` NULL and provides `start`/`step`/`stop` (see the Async
  tools section); `run` wins when both are declared.
- `json_get_str` results are host-owned and stay valid until the next
  `agentc_ext_pump()`; each helper call re-parses, so read integer/bool fields
  before string fields in one `run`.
- Custom providers are SSE-only and the core applies their auth; see the
  Custom providers section above for the full contract and the limits.
- Caps: 64 file templates and 64 themes per scan, 512 prompt registrations
  (retired records included), 8 resource roots per kind and 32 paths per
  `resources_discover` list, 256 KiB per `/skill:` submit; MCP prompts are
  128/server and 512 total (128 frozen retired entries), MCP resources
  256/server and 1024 total, prompt text 64 KiB and resource text 256 KiB
  (over the cap the call fails, never truncates). Skill/prompt/theme content has
  **no ABI contribution slot**: `resources_discover` roots are the only
  extension path, and they are consumed once at startup.
- Dynamic extensions are macOS/Windows only, one fixed config-home
  directory, trusted native code, idle-only close; see the Dynamic extensions
  section above. The Linux freestanding static build never loads one.

## Tests

```sh
./tests/ext.sh             # builds `make test ext-pipeline`, then asserts the harness
./tests/run.sh             # includes the in-test fixture golden (ext_test)
./tests/dyn.sh             # builds hello.so; loads it on macOS, skips on Linux
make dyn-check             # the same smoke under Wine (skips without wine)
```

`make check` builds and runs the same pipeline (`check-ext`) before the golden
suite. `tests/ext.sh` skips the Rust half with a clear message when `cargo` is
not installed.
