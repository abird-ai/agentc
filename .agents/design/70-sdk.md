# 70 — `libagentc` SDK (Axis B)

`libagentc.a` embeds the agentc engine in a host process. It is the in-process
counterpart of `--mode rpc` (60-rpc.md): one command/event vocabulary, two
homes (JSONL over stdio, or the SDK's typed callbacks). Axis C's framed
protocol will reuse the same dispatcher, not this document.

The public ABI is `include/agentc_sdk.h` (`AGENTC_SDK_ABI 1`). It is
append-only: every struct begins with a `struct_size` the caller sets to its
own `sizeof`, and new fields/functions are only appended.

## 1. Archive composition (B-D1, B-D3, B-D4, B4)

`make lib` produces `build/libagentc.a` with `AR rcsD` from a PIC object set
(`-fPIC`), so a stock PIE host can link it with default flags. The archive is
**strict**: it contains no `src/app/*.o` at all. Axis B4 moved the last shared
services out of `app/` into core:

| included | excluded |
| --- | --- |
| `src/base`, `src/core` (incl. `select.c`, `runsession.c`, `policy.c`, `project.c`), `src/ext`, `src/prov`, `src/wire`, `src/net/<os>`, `src/plat/<os>` | **all of `src/app/**`** (`main.c`, `mode.c`, `mode_json.c`, `mode_rpc.c`, `mode_serve.c`, `rpc_dispatch.c`, `setup.c`, `policy.c`, `project.c`) |
| TLS for Linux (`mbedtls` library + `mbedtls_glue.c` + `tls_shim.c`, B-D4) | `src/plat/<os>/boot.c` (the `_start`/`win_start` entry), `src/tui/**`, `src/net/mock.c` |
| | `src/base/memimpl.c`, `third_party/mbedtls_shim.c` (the libc-named shims) |
| | the generated linked-extension table; `linked.c` is compiled `-DAGENTC_EXT_NO_LINKED` |

The archive **references** libc/compiler-rt symbols (`memcpy`, `calloc`,
`snprintf`, `inet_pton`, …) but never **defines** them, so an embedding host's
libc owns those names. `tools/check-lib.sh` (`make libcheck`) enforces the
object and symbol deny-lists, including a `src/app/*.o` denial derived from the
live source tree, links `examples/embed.c` against the archive with default
(PIE) flags, runs it, compares `tests/data/embed.expected`, and asserts
`agentc_mem_live()` is stable across a second init/shutdown. `make libstrict` is
the named strict-archive gate: run from CI it asserts no `src/app/*.o` is
present and repeats the PIE link/run.

## 2. One engine / one agent (B-D2)

- `agentc_sdk_init()` initializes the process-global engine; a second call
  returns `-EBUSY`.
- `agentc_sdk_agent_new()` creates the single `AgcSdkAgent`; a second call
  fails (see `agentc_sdk_last_error()`).
- `agentc_sdk_agent_free()` / `agentc_sdk_shutdown()` tear down and reset.

### Reset table

`agentc_sdk_shutdown()` resets the registries the engine owns:

| reset | by |
| --- | --- |
| extension registry, tools, hooks, provider rows, status | `agentc_ext_shutdown()` |
| model runtime rows (incl. config-published virtual-model aliases) + pricing overrides | `agentc_model_reset()` |
| core prompt registry (file templates included) | `agentc_prompts_reset()` |
| auth store + OAuth credential cache | `agentc_auth_free()` |
| setup context | `agentc_setup_set_context(NULL, …)` |
| typed event hub (slots + quiet) | `agentc_events_reset()` |
| wire poll hook / agent pump | `agentc_http_set_poll_hook(NULL)`, `agentc_pump_install(NULL)` |
| JSON default arena | `agentc_json_reset()` |
| settings, deciders | `agentc_settings_shutdown()`, `agentc_decider_reset()` |
| log/fatal sinks | `agentc_log_install(NULL)`, `agentc_fatal_install(NULL)` |

Deliberately **not** reset (documented process-lifetime state): TLS platform
globals (idempotent), the static provider/model catalog, the log threshold and
the process environment. A host that wants a different environment publishes it
through `AgcSdkConfig.argv/envp` (`os_embed_init`), not after init.

## 3. Security-first defaults

`AgcSdkConfig` is zero-initialized safe: the host must opt in to user/project
config (`load_user_config`/`load_project_config`), network discovery
(`allow_network`), MCP (`enable_mcp`), dynamic extension libraries
(`load_dynamic_extensions`) and project trust (`trusted`). `AgcSdkAgentOptions`
opts in to session persistence (`persist_session`); the default is a memory-only
session. The engine installs no signal handlers, does not touch the terminal
and does not own stdio.

## 4. Lifecycle and pumping

```c
AgcSdkConfig cfg = {0}; cfg.struct_size = sizeof cfg;
agentc_sdk_init(&cfg);

AgcSdkAgentOptions o = {0}; o.struct_size = sizeof o; o.thinking = -1;
o.provider = "openai"; o.model = "gpt-5"; o.events = on_event;
AgcSdkAgent *a = agentc_sdk_agent_new(&o);

agentc_sdk_agent_submit(a, "hello");   /* runs to completion */
const AgcTranscript *t = agentc_sdk_agent_transcript(a);
agentc_sdk_agent_free(a);
agentc_sdk_shutdown();
```

- `submit` is synchronous in v1: it runs the whole model/tool turn loop.
  `agentc_sdk_pump()` (also installed as the agent's poll hook) services
  deferred extension work and the actor bus; a host that drives its own event
  loop can call it between runs.
- `submit_async`/`step` expose the same turn as a cooperative state machine. The
  host starts it once and then advances it from its own loop:

  ```c
  if (agentc_sdk_agent_submit_async(a, "hello") == 0) {
      int rc;
      while ((rc = agentc_sdk_agent_step(a)) == AGENTC_AGENT_RUN_YIELD) {
          agentc_sdk_pump(a, 0);        /* deferred/extension/actor work */
          if (host_wants_abort()) agentc_sdk_agent_abort(a);
      }
  }
  ```

  `step` returns `AGENTC_AGENT_RUN_YIELD` (1) while more model steps remain,
  `0` once settled (transcript and `last_error` final), or a negative errno on
  failure. `agentc_sdk_agent_run_active()` reports whether a run is in flight;
  a second `submit_async` while one is active is `-EBUSY`. `agentc_sdk_agent_submit`
  is exactly this loop driven to settle before it returns, so the two paths
  produce the same transcript and result. `agentc_sdk_agent_abort` is only
  meaningful between steps (or from a callback the engine invokes); because the
  engine is single-threaded, abort is observed at the next step boundary, not
  mid-call.
- `agentc_sdk_agent_abort()` is only meaningful from a callback the engine
  invokes (a pump/event/tool callback); the engine is single-threaded.
- Transport, events, observer and an explicit tool table can be injected at
  construction (`AgcSdkAgentOptions`) or replaced later
  (`agentc_sdk_agent_set_*`). An injected transport survives a provider/model
  rebuild.

## 5. Packaging (B-D6)

- `make pc` writes `build/agentc.pc`; `make install-lib PREFIX=…` installs the
  headers (`$(PREFIX)/include/agentc`, including the generated `version.h` and
  `base/limits.h`), `libagentc.a`, `libagentc.so`/`.so.1` (Linux) and
  `agentc.pc`.
- `make libso` builds the shared library with `tools/libagentc.map`, which
  exports `agentc_*` and localizes `*`; the version script's `local: *` also
  contains the archive's internal symbols.
- macOS ships the static archive for now (`make lib`); a `dylib`/install-name
  policy is deferred until the platform is exercised natively.

## 6. Known limits

- One engine and one agent per process **through this SDK**; the SDK keeps the
  B-D2 contract (a second `agentc_sdk_init`/`agent_new` is `-EBUSY`). Process
  `agentc serve` is the front end that hosts a bounded multi-session registry
  and interleaves its runs; it does so through the core resumable-run API
  (`agentc_agent_run_begin/step`), not by relaxing the SDK's one-engine rule.
- The archive is strict (no `src/app/*.o`); the app front ends (JSONL,
  `serve`, onboarding orchestration) are native-binary-only. Hosts that want
  those behaviors embed through the SDK API instead.
- `submit` is a synchronous wrapper over the pump-driven
  `submit_async`/`step` path. The state machine is cooperative and
  single-threaded: a single model step is not preemptible, so a host that wants
  abort latency must call `step`, `pump` and `abort` from that same loop (abort
  is observed at the next step boundary). The `serve` front end advances several
  sessions with the same core API and rebinds the process-global services per
  slice; the SDK does not expose that rebinding because it owns exactly one
  agent.
- The archive references the host libc; a no-libc host is not supported (that is
  what the freestanding binary is for).
- Route resolution for a virtual-model (alias) target is **per-agent**: the
  appended `AgcAgentSpec.route` (`AgcRouteSeam`) lets a host install a
  config-aware resolver on its agent (the mode layer does this for the app
  front ends). The SDK installs none by default, so a routed target falls back
  to the target provider's default base URL and `agentc_auth_key()` (never the
  agent's own key). Aliases themselves live in the process-global model
  registry: config loads publish them and `agentc_sdk_shutdown()` ->
  `agentc_model_reset()` drops them (§2).
- `load_user_config` with a different `HOME` after init is unsupported; the
  environment is read before init.
