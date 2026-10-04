# agentc extension API — full design spec (v3)

Status: **P1/P2 delivered; P3 (hooks) fully delivered, including wave 4
(`model_select`/`thinking_level_select` + the app model/thinking sinks); the
post-review fix waves R1–R3b (runtime wiring/CI/docs, registry/core
correctness, UX/observability/build/MCP memory) are delivered, and this spec
describes that final behavior. P4.1 (typed custom providers, §11.1) is
implemented, independently verified and committed. P4.2
(skills/prompts/themes + MCP prompts/resources; §6.2/§7) is implemented,
independently verified and committed (feature + docs commits); this spec
documents that behavior; P4.3 (asynchronous extension tools; §3.7, D110–D124)
and P4.4 (reserved points + completion; D125–D137) are implemented,
independently verified and committed, and this spec records that behavior.** v3
incorporates the P1 implementation decisions: the one-word convention
(extension/ext only), the `AgcExt.order` field, the default-extension privilege,
and the resolution of the two open decisions. v2 incorporated the fresh-eyes
review. Supersedes v1 and `EXTENSION-MINIMAL.md`; `EXTENSION-TOOL-ENGINE.md` is
exploration, the `PI-*` files are reference.

Thesis: **minimal features, complete seams.** The core ships few capabilities but
exposes every point needed to grow into the pi model through an extension.

---

## 0. v3 changes (P1 implementation record)

**Convention.** There is one word for the model: **extension** (`ext` for
symbols). The old word "plugin" is gone from live code and docs. A statically
linked extension is just an extension that arrives through the generated table
(`extensions/manifest.json` -> `tools/gen-exts.py` -> `build/exts.c` ->
`src/ext/linked.c`). There is no separate plugin ABI, header, or registry.

**`AgcExt.order`.** Extensions declare an `int32_t order`; `agentc_ext_load_all`
initializes them lowest order first, ties by registration order. This is the
extension-level ordering knob; contributions already have section/status/hook
priorities, and tools follow extension then registration order.

**Default extensions.** `builtin-tools`, `builtin-context` and `mcp` are ordinary
extensions registered by `agentc_ext_register_defaults()`. They use the same
`AgcExtHost` surface as a linked extension. Their **only** privilege is that they
compile inside core and may include `src/ext/registry_int.h` to register internal
`AgcTool`s (e.g. `bash`'s async `start`/`step`) and call the internal bus
directly. No other privileged path exists.

**O1 resolved:** `--system` hard-replaces the whole render (matching pi's
`forceSystemPrompt`); extension sections are skipped. There is no
`--append-system-prompt`.

**O2 resolved:** extension init order is defaults-first — `builtin-tools`,
`builtin-context`, `mcp`, then linked extensions (ties by registration order;
`AgcExt.order` sorts). The historical `builtins -> plugins -> mcp`
**registration** order became `builtins -> mcp -> linked` in P1, but since P2
that order is not the live tools table: `mcp` registers nothing at `init` and
appends its tools from the registry pump when a server connects, so the table is
`builtins -> linked` at startup with MCP appended on connect. No existing golden
encoded the old ordering (the suite never links MCP and a linked extension at
once), so none needed regenerating; `ext_test` pins the generic `AgcExt.order`
sort plus registration ties, not the builtin/mcp/linked sequence.

**Spec corrections found against the code:**
- The appended JSON/`strdup_`/`set_title` ergonomics from the old ABI are retained
  at the end of `AgcExtHost`; the spec's §3.2 listing omitted them, which would
  have forced every small C extension to write a parser for no gain.
- `src/ext/plugin.c` became `src/ext/registry.c` + `src/ext/linked.c` (host,
  registry and hook bus in one; generated-table adapter in the other), not a
  single rewritten `plugin.c`.

**P3 status.** The P1 deferrals are now delivered: the trust reordering and
`project_trust` consumption of §8, the `resources_discover` gating of §8, and
the "destructive extension tools hidden by default" rule of §10 (D52 resolved:
`readOnlyHint` wins, `destructiveHint` defaults to true, core builtins carry the
core marker and stay eligible). Every P3 core emit site is live, including
`model_select`/`thinking_level_select` (wave 4, emitted on a model/thinking
change) and the app model/thinking sinks. MCP is still synchronous in P1; the
`start`/`pump` split is P2.

**Post-review fix record (R1–R3b, delivered).** A post-P3 review found runtime
wiring, registry, on-ramp and documentation drift; the fixes below are the
current, authoritative behavior and supersede any older wording elsewhere.

- **R1 — wiring, CI, prompt, docs.** One `agentc_mode_pump` duty is installed by
  `agentc_mode_setup` as both the wire poll hook and the agent pump for every
  mode: deferred extension work runs during blocking HTTP waits, tool runs and
  idle loops. The TUI chains its own poll hook through it and restores it after
  a submit; the RPC read-wait and the TUI idle tick call it; `agentc_ext_pump`
  carries a re-entrancy guard, so a pump-within-a-pump returns immediately. The
  startup pump is bounded to 400 ms and exits as soon as `agentc_mcp_pending()`
  is false (or no server record exists). In auto mode (no explicit `--system`)
  the agent rebuilds the system prompt from the live tool table before every
  turn, so a turn-boundary recompose reaches the next request; an explicit
  `--system` and a per-run `before_agent_start` `systemPrompt` override stay
  fixed for the run, and the effective prompt is re-published to the extension
  context before every request. CI runs `make`, `make ext-pipeline` and
  `make check` (which includes `check-ext`) with clang/make/python3 from apt,
  then runs `tests/ext.sh` under `nix develop` — cargo comes from the flake dev
  shell (Ubuntu's cargo is too old for the workspace lockfile), not apt. The
  build fragment `build/exts.mk` is a real make target written atomically by
  `tools/gen-exts.py`, and a generation error fails the build.
- **R2 — registry/core correctness.** The registry restores the owning
  extension around hook/tool/command/section/pump/defer/HTTP callbacks, prefixes
  `host->log` with `[ext:<name>] ` and names the extension in failure/overrun
  diagnostics. Three consecutive overruns disable a handler (a clean call
  resets the counter); the overrunning occurrence is fail-closed on an override
  point and fail-open on an observe point. `on` rejects an unknown hook point;
  `agentc_ext_wants` is backed by a live-registration counter. Emit recursion is
  capped at depth 32 (an override point at the cap answers blocked, an observe
  point empty). `set_status` is keyed by (owner, key), 8 live keys
  process-wide. ABI growth goes through `AGENTC_EXT_FIELD_OK` /
  `AGENTC_EXT_HOST_HAS` and `AgcExt.required_host_size`. `resources_discover` is
  fail-closed (a blocked handler consumes nothing); `message_end` cannot rescue
  a failed/aborted turn and validates `tool_call` entries; `turn_end.continue`
  is honored on every non-hard turn (including the truncation-with-tool_use
  path); `select_tools` dedupes repeated names. A blocked `input` sets the
  error, emits `AGENTC_EV_ERROR` and makes `agentc_agent_submit` return -1.
  `agentc_http_set_cancel` and forced `\uXXXX` escaping of response-header JSON
  landed; the app frees the tool table on the OAuth refusal path.
- **R3 — UX/observability/build/MCP memory.** MCP tool entries are bounded
  (256 per server, 1024 total); an over-long remote name is skipped rather than
  truncated (a truncated name would call the wrong tool), descriptions are
  truncated at 8 KiB, exposed names are deduped with a hash suffix inside the
  64-byte registry cap. The frozen pile (retired but handed out) is capped at
  256; because a stale `AgcTool.ud` may still reference an entry until the next
  recompose, a handed-out entry is never freed or reused — hitting the cap
  fails the server with a clear log instead. MCP HTTP requests carry the cancel predicate.
  The Windows shell resolver falls back to `%windir%` when `%SystemRoot%` is
  absent. `--list-extensions` and the post-`load_all` summary report state via
  `agentc_ext_describe`/`AgcExtInfo`; the D52 `--help` note and the MCP-disabled
  deferral diagnostic are live. The TUI installs a UI sink (`notify`;
  `set_title` falls back to a notice because the terminal layer has no title
  op). The generated build fragment is a real target with atomic writes and
  surfaced errors (with `make check` running `check-ext`). The
  `AgcWantsFn`/`AgcNotifyFn` bridge is retired in favor of direct registry calls
  plus `agentc_ext_append_entry`; the internal fields helper is
  `json_fields_apply(base, over, keep_nulls)`; `load_all` runs bounded rounds and
  shutdown walks the live record list to completion.

---

## 1. Principles

1. **One model.** `builtin-tools` and `mcp` are ordinary default extensions. No
   privileged path.
2. **Structure typed, data JSON.** Contributions are C structs; hook payloads and
   results are JSON.
3. **Break the old ABI.** `agentc_plugin.h` is deleted; one `agentc_ext.h`, one
   `abi_version`, no `ABI_MIN`, no `reserved[]`, no `offsetof` gates.
4. **pi-compatible names and mental model.** Hook-point names are pi's.
5. **Two capabilities.** `OBSERVE` (notify) and `OVERRIDE` (may decide/replace).
6. **Shared bus, core-authoritative decisions.** Extensions can emit points, but
   only the core consumes control decisions for core points.

**Scope, and what is deliberately deferred.** Out now: durable execution/tasks,
codemode, remote/server, UI dialogs, session tree/switch, MCP OAuth, wrapping
middleware, custom providers (delivered in P4.1, §11.1), asynchronous
extension tools (delivered in P4.3, §3.7).

---

## 2. ABI policy

- One header `include/agentc_ext.h`; `AGENTC_EXT_ABI 1`.
- `AgcExtHost`/`AgcExt` carry `abi_version` + `struct_size`. **Contribution
  structs carry `struct_size` only** (the host knows their type at the `add_*`
  call). Structs are append-only from this baseline.
- One host shape, no version range. A mismatch is refused with a clear log.
- Static registration. Each extension exports the canonical symbol
  `agentc_ext_init`, compiled per extension with
  `-Dagentc_ext_init=agentc_ext_<ident>_init` (exactly today's pattern, emitted
  per entry by `tools/gen-exts.py` into `build/exts.mk`). `tools/gen-exts.py`
  emits a table of `{name, int (*entry)(const AgcExtHost*, AgcExt*)}`.
- **P4.5 dynamic registration (macOS/Windows only).** A loadable `.dylib`/
  `.dll` exports the canonical `agentc_ext_init` — no per-ident rename, because
  the host looks up exactly that name with `os_ext_sym` — and is adopted through
  the same descriptor path as a linked extension. The library lives in
  `<config home>/extensions/` under the fixed suffix/stem/cap policy (§9), the
  registry closes it only when idle (§5), and the Linux freestanding static
  build keeps the compile-time stub (`os_ext_available()` false, no dynamic
  loader).

---

## 3. Header (`include/agentc_ext.h`)

### 3.1 Types

```c
#define AGENTC_EXT_ABI 1

#define AGENTC_TOOL_READONLY    0x1u
#define AGENTC_TOOL_DESTRUCTIVE 0x2u
#define AGENTC_TOOL_SEQUENTIAL  0x4u
#define AGENTC_TOOL_HIDDEN      0x8u
#define AGENTC_TOOL_TIMEOUT_DEF_MS 120000

#define AGENTC_HOOK_OBSERVE  0x1u   /* side effects only */
#define AGENTC_HOOK_OVERRIDE 0x2u   /* may return a decision/replacement */

typedef struct AgcExtHost AgcExtHost;

/* ---- tool call view (published; append-only). The old ABI passed these as
 * separate strings; a struct is easier to extend and lets a tool read args. */
typedef struct AgcExtToolCall {
    uint32_t    struct_size;
    const char *call_id;
    const char *name;
    const char *args_json;      /* validated arguments object */
    const char *signal_token;   /* pass to host->is_cancelled */
} AgcExtToolCall;

/* ---- tools: synchronous by default; asynchronous when the appended trio is
 * provided and `run` is NULL (see §3.7) ---- */
#define AGENTC_EXT_TOOL_FINISHED  0
#define AGENTC_EXT_TOOL_ERROR     1
#define AGENTC_EXT_TOOL_TIMEOUT   2
#define AGENTC_EXT_TOOL_CANCELLED 3
#define AGENTC_EXT_TOOL_UNLOAD    4

#define AGENTC_EXT_TOOL_TIMEOUT_MAX_MS 1800000   /* async clamp: 30 min */

typedef struct AgcExtTool {
    uint32_t    struct_size;
    uint32_t    flags;                 /* AGENTC_TOOL_* */
    const char *name;                  /* [a-z0-9_:.-]+, <=64, unique */
    const char *label;
    const char *description;
    const char *parameters_json;       /* JSON Schema object */
    const char *prompt_snippet;        /* optional # Tools line */
    const char *prompt_guidelines;     /* optional # Rules bullets */
    void       *ud;
    int         timeout_ms;            /* 0 => default; async clamps to MAX */
    /* append result text to `out` via host->out_write; return 0 | -errno */
    int (*run)(const AgcExtHost *host, const AgcExtTool *self,
               const AgcExtToolCall *call, void *out, bool *is_error);
    /* P4.3 appended (read with AGENTC_EXT_FIELD_OK); `run` non-NULL wins:
     * start -> 0 running / 1 complete / <0 fatal (`*state` extension-owned);
     * step -> 0 running / 1 complete / <0 fatal;
     * stop -> exactly once per start >= 0, one AGENTC_EXT_TOOL_* reason. */
    int  (*start)(const AgcExtHost *host, const struct AgcExtTool *self,
                  const AgcExtToolCall *call, void *out, bool *is_error,
                  void **state);
    int  (*step)(const AgcExtHost *host, const struct AgcExtTool *self,
                 const AgcExtToolCall *call, void *state, void *out,
                 bool *is_error);
    void (*stop)(const AgcExtHost *host, const struct AgcExtTool *self,
                 void *state, int reason);
} AgcExtTool;

typedef struct AgcExtCommand {
    uint32_t    struct_size;
    const char *name;
    const char *description;
    void       *ud;
    void (*run)(const AgcExtHost *host, void *ud, const char *args_json, void *out);
} AgcExtCommand;

typedef struct AgcExtSection {
    uint32_t    struct_size;
    const char *key;
    int32_t     priority;              /* lower first */
    void       *ud;
    int (*render)(const AgcExtHost *host, void *ud, void *out);  /* via out_write */
} AgcExtSection;

typedef struct AgcExtStatusSegment {
    uint32_t    struct_size;
    uint32_t    slot;                  /* 0 left, 1 right */
    int32_t     priority;
    uint32_t    style;                 /* dim/bold/accent/warn/error/ok */
    const char *text;
} AgcExtStatusSegment;

typedef size_t (*AgcExtStatusProvider)(void *ud, AgcExtStatusSegment *out,
                                       size_t max, char *arena, size_t arena_cap);

typedef int (*AgcExtHookFn)(void *ud, const char *point,
                            const char *payload_json, char **result_json);
/* return: 0 = continue, 1 = handled/stop, <0 = error (see §5.5) */

/* dispatch result, shared by core-internal emit and host->emit */
typedef struct AgcExtResult {
    uint32_t    struct_size;
    int         handled;       /* a handler returned 1 */
    int         blocked;       /* handled and the result asked to block/cancel */
    char       *result_json;   /* host-allocated; free with host->free */
} AgcExtResult;
```

### 3.2 Host

```c
struct AgcExtHost {
    uint32_t abi_version;              /* == AGENTC_EXT_ABI */
    uint32_t struct_size;

    void  *(*alloc)(size_t n);         /* zeroed */
    void   (*free)(void *p);
    void   (*log)(int level, const char *msg);

    /* output sinks for run/render/command (opaque; never expose AgcBuf) */
    void   (*out_write)(void *out, const char *bytes, size_t n);

    /* contributions */
    void   (*add_tool)(const AgcExtTool *tool);
    void   (*add_command)(const AgcExtCommand *cmd);
    void   (*add_section)(const AgcExtSection *section);
    void   (*add_status)(AgcExtStatusProvider provider, void *ud);

    /* hooks */
    uint64_t (*on)(const char *point, uint32_t caps, int priority,
                   AgcExtHookFn fn, void *ud);      /* 0 = rejected */
    void     (*off)(uint64_t handle);
    void     (*emit)(const AgcExtHost *host, const char *point,
                     const char *payload_json, AgcExtResult *out);

    /* services */
    void   (*defer)(const AgcExtHost *host, void (*fn)(void *ud), void *ud);
    int    (*is_cancelled)(const AgcExtHost *host, const char *signal_token);
    const char *(*cwd)(const AgcExtHost *host);
    const char *(*session_id)(const AgcExtHost *host);
    const char *(*session_file)(const AgcExtHost *host);
    const char *(*system_prompt)(const AgcExtHost *host);
    void   (*append_entry)(const char *custom_type, const char *data_json);

    uint64_t (*http_request)(const AgcExtHost *host, const char *method,
                             const char *url, const char *headers_json,
                             const char *body, uint64_t body_len,
                             void (*cb)(void *ud, int status,
                                        const char *headers_json,
                                        const char *body, uint64_t body_len),
                             void *ud);
    void   (*http_cancel)(const AgcExtHost *host, uint64_t request);

    void   (*notify)(const char *message, int level);
    void   (*set_status)(const char *key, const char *text);
    int    (*set_model)(const char *provider, const char *model);
    void   (*set_thinking)(const char *level);
    void   (*request_recompose)(const AgcExtHost *host);

    /* appended P4.1: register a custom provider. The contribution is validated
     * and copied; guard with AGENTC_EXT_HOST_HAS(host, add_provider) */
    void   (*add_provider)(const AgcExtProvider *provider);
};

typedef struct AgcExt {
    uint32_t abi_version;              /* = AGENTC_EXT_ABI */
    uint32_t struct_size;
    const char *name;
    const char *version;
    int32_t     order;                 /* lower first; ties by registration order */
    int  (*init)(const AgcExtHost *host);
    void (*shutdown)(void);
    uint32_t    required_host_size;    /* appended: min AgcExtHost.struct_size */
} AgcExt;

int agentc_ext_init(const AgcExtHost *host, AgcExt *out);
```

Appended contribution fields are read through `AGENTC_EXT_FIELD_OK(struct, Type,
field)` and appended host services through `AGENTC_EXT_HOST_HAS(host, field)`;
neither reads past the caller's declared `struct_size`, and `HOST_HAS` also
rejects a present-but-NULL slot. `AgcExt.required_host_size` (appended) states
the minimum `AgcExtHost.struct_size` the extension needs; any value above
`sizeof(AgcExtHost)` is refused at registration with a clear log.

**P4.3 appended no host service.** An async tool uses the existing
`is_cancelled`, `defer` and `http_request` services and keeps its per-job state
in the appended `AgcExtTool` fields; the new `AgcJob.cleanup` hook is
core-internal (the internal job driver, §4.7), not part of `AgcExtHost`.

### 3.3 What changed from v1 (review-driven)

- **`out_write` added** (was promised but missing) and `AgcExtToolCall`
  **published** (was opaque, so tools could not read args or cancellation).
- **`add_provider` removed** from ABI 1. The real `AgcProviderOps` needs
  `discover_style`, `stream_open`, `stream_close`, typed `AgcRequest`/
  `AgcStreamState`, and rejects duplicate names — none of which a `void*` vtable
  can express. Custom providers move to P4 with a typed design. `AgcExtProvider`
  is deleted. **Delivered in P4.1 (§11.1):** the slot is back as an appended host
  service and the typed `AgcExtProvider` family is the P4.1 addition.
- **`on` returns a handle; `off` added.** v1 had no unregister, so `shutdown`
  left dangling callbacks.
- **`emit` takes an `AgcExtResult` out-param** so extension-published points can
  be decision points, not only notifications.
- **Async `start`/`step` removed (v1–v2), re-added in P4.3.** The first
  revision removed the proposed async fields because a tool had no job
  output/error channel; extension tools were synchronous with a timeout. P4.3
  re-adds them as appended `AgcExtTool` fields plus a per-step staging path
  (§3.7) — still opt-in and ABI-compatible: `run` stays the default and wins
  when both are declared.

### 3.4 Ownership

- Registered strings are **copied by the host** at the `add_*` call.
- `payload_json` is host-owned for the call only.
- `*result_json` returned by a hook is `host->alloc`ed; the host frees it.
- `AgcExtResult.result_json` from `emit` is host-allocated; the caller frees it
  with `host->free`.
- `host->alloc/free/log` are main-loop only. A worker returns via `host->defer`.
- Async tool state (`start`'s `*state`) is extension-owned: allocated with
  `host->alloc` and released in `stop`; the host never frees it (§3.7).

### 3.5 Registration ownership and rollback

The registry tags every registration (`on`, `add_*`) with the extension currently
inside `init` (**owner**). `agentc_ext_unload(name)` removes every registration
by that owner **first**, then calls `shutdown()`; `agentc_ext_shutdown()` tears
down every extension the same way and ends with `agentc_status_reset()`. A failed
`init` rolls back the same way (registrations removed, then `shutdown()`).
**Entry purity (P4.5).** The owner id is allocated *before* the descriptor call
— `agentc_ext_adopt` and the dynamic loader both owner-tag the entry phase — so
an entry must be descriptor-only: registering contributions belongs in `init`.
Anything the entry registers anyway is attributed to the extension and rolled
back when the descriptor is refused; the same owner id later runs `init` under
`load_all`.

This is the robust form of the old `remove_*` bookkeeping (`src/ext/registry.c`
`owner_remove`, called by the failed-init branch of `load_all`) and removes the
need for an explicit `remove_*` API. `off(handle)` covers the single-handler
case. Owner rollback also covers **asynchronous work**: a torn-down owner's
deferred callbacks and pending extension HTTP requests are dropped together with
its registrations, so a dead extension cannot fire later; `agentc_ext_shutdown()`
resets the defer ring under its lock.

### 3.6 Concurrency

Hooks are **synchronous** and budgeted. pi's handlers are `Promise`-returning and
awaited; that model is **not** reproduced. Override points must decide
synchronously; I/O-bound work may **not** return a decision (it can only observe
via `defer`). This is a documented divergence, not an accident (§12).

### 3.7 Asynchronous tools (P4.3)

An extension tool is synchronous by default: it sets `run` and appends its
result text with `host->out_write` (capped/truncated by the job driver as usual).
An **asynchronous** tool leaves `run` NULL and provides the complete
`start`/`step`/`stop` trio; if `run` is set, the trio is ignored with a level-1
log and `run` wins. Registration reads the trio behind `AGENTC_EXT_FIELD_OK`, so
an older/smaller contribution stays synchronous; a missing member is rejected
with a level-3 `incomplete async trio` log.

**Codes.** `start` returns `0` running, `1` complete, `<0` fatal; `step` the
same. A `start` that returns `<0` is never followed by `stop` (the extension
must not leak `*state`). Any other positive value is treated as running.

**Non-blocking contract.** The host drives all three on the main loop. `start`,
`step` and `stop` must not block; I/O goes through `host->defer` or
`host->http_request`, which the adapter services with `agentc_pump(0)` after
each running step (the extension is off the stack, so deferred work — including
a deferred unload — can proceed). A blocking callback cannot be preempted
(§12).

**State ownership.** `void *state` is extension-owned: `host->alloc` in `start`,
`host->free` in `stop`; the host never frees it. The host owns only the internal
adapter record; the published `AgcExtToolCall` (and its strings) stays valid
from `start` through `stop`.

**Exactly-once `stop`.** For every `start` that returned `>= 0`, `stop` runs
exactly once with one of:

| reason | value | delivered when |
|---|---|---|
| `AGENTC_EXT_TOOL_FINISHED` | 0 | the tool completed (start/step returned 1); `*is_error` is the result, not the reason |
| `AGENTC_EXT_TOOL_ERROR` | 1 | a fatal `step` (`< 0`); the driver replaces staged output with `error: tool '<name>' failed to run (<err>)` |
| `AGENTC_EXT_TOOL_TIMEOUT` | 2 | the per-job deadline passed; staged output is replaced by `error: tool '<name>' timed out` |
| `AGENTC_EXT_TOOL_CANCELLED` | 3 | the batch aborted, or a fatal step returned while the cancel flag was set; partial output is kept |
| `AGENTC_EXT_TOOL_UNLOAD` | 4 | the owning extension was unloaded/removed/shut down while the job was live |

`FINISHED` means "no more steps", not success. Tool errors are **tool results**
(a transcript `tool` message with `is_error`), not agent failures: the loop
answers the call and runs another turn. An abort still ends the run with
`last_error = "aborted"`.

**Signal token.** The published `call.signal_token` is valid from `start`
through `stop` (a unique 32-byte adapter token per job), so `stop` still sees
the final cancellation state. The token answers cancellation only — there is no
timeout query (§12).

**Output.** `start`/`step` output goes into a per-step staging buffer and is
appended with `agentc_tool_append_output`, so it obeys the same 50 KB inline
byte cap (`AGENTC_LIMIT_TOOL_BYTES`) and spill/truncation path as a synchronous
result. A retiral observed on a later
step synthesizes `error: extension tool unloaded` without calling the
extension. `stop` must not register contributions and should not block.

**Unload/retire.** A tool record with live async jobs is unlinked from the
active table but not freed (the live job holds `tool.ud`); each live job gets
`stop(UNLOAD)` at retiral, and the record is released at `agentc_ext_shutdown`
(or by the last job when shutdown already ran — the `free_pending` deviation in
`.agents/plans/P4.3-ASYNC-TOOLS.md`). A retired tool name may be reused, since
lookups only reach the active table.

**Timeout clamp.** Async `timeout_ms` is clamped to
`AGENTC_EXT_TOOL_TIMEOUT_MAX_MS` (1 800 000 ms, 30 min) with a level-2 log; a
negative value normalizes to 0 (the driver default of 120 s). Synchronous tools
are unchanged.

**Internal driver hook.** `AgcJob.cleanup(self, job, reason)` is an appended
core-internal hook (`src/core/tools/jobs.h`); the adapter sets it in `start` and
the driver invokes it exactly once on fatal/timeout/cancel/release, before
`job->priv` is freed. Reasons mirror the stop reasons
(`ERROR`/`TIMEDOUT`/`CANCELLED`/`RELEASE` →
`ERROR`/`TIMEOUT`/`CANCELLED`/`UNLOAD`).

**Sequential flag.** Async tools are not implicitly sequential: the batch
driver's async fan-out and step interleave are unchanged, and
`AGENTC_TOOL_SEQUENTIAL` still applies when declared.
`extensions/async_demo/async_demo.c` is the reference implementation and the
linked `tests/ext.sh` checks cover finish, tool-result error, fatal, timeout,
cancel and the extension's own poll branch (the unload/retire paths are covered
by the `ext_test` fixture).

---

## 4. Hook bus

**Registration.** `host->on(point, caps, priority, fn, ud)`. Order: `priority`
ascending, ties by registration order. `caps` is validated at registration
against the point's required caps (§4.1); a mismatch returns `0` and logs.

**Dispatch.** Internal `agentc_ext_emit(point, payload_json) -> AgcExtResult`
walks a snapshot of subscribers (registration during dispatch is visible only to
the next emit, never to the current walk).

### 4.1 Points

`caps` = required capability. `policy` = composition. `merge` = how a result
combines. `on-fail` = behavior when a handler errors or overruns budget.

| point | caps | policy | merge | on-fail | payload | result |
|---|---|---|---|---|---|---|
| `project_trust` | override | first | — | closed | `{cwd}` | `{trusted:"yes"\|"no"\|"undecided", remember?}` |
| `session_start` | observe | chain | replace | open | `{reason, session_id, session_file, previous_session_file?}` | — |
| `session_shutdown` | observe | chain | replace | open | `{reason}` | — |
| `resources_discover` | override | chain | fields | closed | `{cwd, reason}` | `{skillPaths?, promptPaths?, themePaths?}` (all three path lists consumed, §8) |
| `input` | override | chain | fields | closed | `{text, source}` | `{action:"continue"\|"transform"\|"handled", text?}`; images not emitted |
| `before_agent_start` | override | chain | fields | closed | `{prompt, system_prompt}` | `{message?, systemPrompt?}` |
| `agent_start` | observe | chain | replace | open | `{}` | — |
| `agent_end` | observe | chain | replace | open | `{stop_reason}` | — |
| `agent_before_settle` | override | chain | fields | closed | `{outcome:"completed"\|"aborted"\|"error"}` | `{entries?, continue?}` (32-cap, §4.2) |
| `agent_settled` | observe | chain | replace | open | `{}` | — |
| `turn_start` | observe | chain | replace | open | `{turn_index}` | — |
| `turn_end` | override | chain | fields | closed | `{turn_index, stop_reason}` | `{entries?, continue?}` (32-cap, §4.2) |
| `message_start` | observe | chain | replace | open | `{role}` | — |
| `message_update` | observe | chain | replace | open | `{type, text}` | — |
| `message_end` | override | chain | fields | closed | `{role:"assistant", stop_reason, usage}` | `{message?}` restricted shape (details below) |
| `tool_call` | override | **chain-veto** | fields | closed | `{tool_call_id, tool_name, input}` | `{block?, reason?, input?, terminate?}` |
| `tool_result` | override | chain | fields | closed | `{tool_call_id, tool_name, content:[{type:"text",text}], is_error}` | `{content?, is_error?}` — content forms below |
| `tool_execution_start` | observe | chain | replace | open | `{tool_call_id, tool_name, input}` | — |
| `tool_execution_end` | observe | chain | replace | open | `{tool_call_id, tool_name, input, result, is_error, duration_ms}` | — |
| `before_provider_headers` | override | chain | fields (null deletes) | closed | `{provider, api, model, headers}` | flat header patch `{ "<Header>": value\|null }` |
| `before_provider_request` | override | chain | fields | closed | `{provider, api, model, url, payload}` | `{payload}` |
| `after_provider_response` | observe | chain | replace | open | `{provider, status, headers, transport_error}`; `headers` is the transport's raw object or `{}` | — |
| `provider_stream_event` | observe | chain | replace | open | `{provider, api, model, event, data}` | — |
| `session_before_compact` | override | first | — | closed | `{reason, first_kept, messages}` (no `entries`/`instructions`) | `{cancel?, summary?}` |
| `session_before_switch` | override | first | replace | closed | `{reason:"new"}` | `{cancel?}` |
| `session_compact` | observe | chain | replace | open | `{reason, from_extension}` | — |
| `session_compact_failed` | observe | chain | replace | open | `{reason, aborted, error?}` | — |
| `mcp_servers_change` | observe | chain | replace | open | `{servers:[{name,kind,status,caps?}]}` (status: connecting/ready/failed; caps: tools/prompts/resources/resource_templates post-initialize) | — |
| `model_select` | observe | chain | replace | open | `{provider, model, previous}` | — |
| `thinking_level_select` | observe | chain | replace | open | `{level, previous}` (names: off/low/medium/high) | — |

**Tool-call veto decision (P3, delivered).** The core calls the veto as a
decision callback:

```c
void (*AgcToolVetoFn)(void *ud, const char *tool_call_id, const char *tool_name,
                      const char *args_json, AgcToolVetoDecision *out);
typedef struct {
    int   block;       /* set => block the call */
    char *reason;      /* optional diagnostic; core-owned, agentc_free */
    char *args_json;   /* replacement input; NULL = unchanged; core-owned */
    int   terminate;   /* P4.4a: honored only with block; see §4.2 */
} AgcToolVetoDecision;
```

The core owns the two strings (`agentc_free`) and frees them after the call;
NULL fields mean unchanged. The adapter `agentc_ext_tool_veto` fills `block` when
a handler returned `1`, set `{"block":true}`, or failed (the point is
fail-closed), copies the result's `reason` for the core diagnostic, copies a
rewritten `input` as the replacement args, and parses `terminate` from the
result. `terminate` is honored only alongside `block`: a `terminate` without
`block` is logged (level 1) and dropped, because a call that keeps running must
not end the batch. P3 delivered the consumer: the agent threads a rewritten
`input` into the job (the replacement args own the batch lifetime) and turns
`block` into a pre-error tool result `error: tool blocked: <reason>`. P4.4a
added the terminate consumer (batch agreement; §4.2).

**P3 payload/result details (as implemented).**

- `message_end` result `{message?}`: `message` is restricted. `role` must be
  absent or `"assistant"`; `content` must be an array whose entries are `text` /
  `thinking` objects (optional string `text`) or `tool_call` objects
  (`id`/`name`/`arguments`; a tool_call entry needs a non-empty string `id` and
  `name`, `arguments` is optional). `stop_reason` and `error` may be replaced,
  **except on a failed/aborted turn, where they keep the terminal failure** —
  content may still be replaced, but a hook cannot flip the message back into a
  serializable/persistable turn. `usage` and `ts` are never replaceable. A
  malformed shape is logged and the original message is kept untouched.
- `tool_result` result `content`: a string, a `[{type:"text", text}]` array
  (joined with newlines), or `null`/`[]` (empty result). Any other shape is
  logged and the raw result is kept. `is_error` overrides the flag when present.
- `turn_end`/`agent_before_settle` result `entries`: `[{custom_type, data}]`
  appended through the host entry sink; malformed entries are logged and
  skipped. `continue` asks for one more turn: on `turn_end` only for a non-hard
  turn (a truncation that still carries `tool_use` is non-hard and honors it on
  the tool path too); on `agent_before_settle` only when the run ended cleanly.
  Both points share a **32-continuation cap per submit** (§4.2).
- `before_provider_headers` result: flat patch `{ "<Header>": value|null }`; see
  §10 for the forbidden headers and duplicate rules.
- `before_provider_request` result `payload` must be an object, otherwise the
  original body is kept.
- `session_before_switch` result (P4.4a): the RPC `new_session` switch is
  cancelled by a blocked or failed handler, a `handled` result without a
  boolean `cancel`, a malformed `cancel`, or `{"cancel":true}`; a well-formed
  `{"cancel":false}` allows the switch whether or not the handler claimed
  `handled`. Cancelling answers the command with
  `session switch cancelled by extension` and leaves the session, its
  transcript and the extension context untouched.

**Reserved** (no core emit site and **no point-table entry**):
`session_info_changed`, `session_before_fork`, `session_before_tree`,
`session_tree`, `cache_warming_decision`, `ui_prompt_start`, `ui_prompt_end`,
`user_bash`. The contract is the R2 unknown-point rule: `host->on` for a
reserved name is rejected with a level-3 log and returns `0` (an extension
cannot subscribe to it), `agentc_ext_wants` is false, and `host->emit` for it is
inert (an empty result that cannot block). A reserved name only becomes real
when the core adds a table row and an emit site. `user_bash` (O-4) documents its
future site as `rpc_bash` in `src/app/mode_rpc.c`; no core emit exists.
`context`/`context_with_system` fold into `before_provider_request`.

> v1 wrongly listed `session_before_fork` (no fork feature) and
> `tool_execution_update` (no seam); both are removed from the active table.
> `model_select`/`thinking_level_select` were moved to reserved in v1 and are
> now active P3 points with live emit sites (§4.7).

**Unknown points are rejected (R2).** `host->on(point, ...)` accepts only the
names in the table above; anything else is refused with a level-3 log and
returns `0`. `agentc_ext_wants`/`agentc_ext_emit` on an unknown name are inert
(`wants` false, emit returns an empty result that cannot block).

### 4.2 Policy semantics

- **chain**: each handler receives the previous handler's result merged in; the
  final merged value is the dispatch result. A handler returning `1` records
  `handled` but does **not** stop the walk: on a `chain` point later handlers
  still run. Only `first` and `chain-veto` short-circuit. (The `AgcExtHookFn`
  comment in `agentc_ext.h` was corrected: `1` means "handled", not "stop" for
  chain points.)
- **first**: the first handler returning `1` wins; later handlers are not called.
- **chain-veto** (`tool_call` only): every handler sees the current `input`. Any
  handler may set `{"block":true}` (with `reason`) or return `1` — either
  short-circuits and blocks — or replace `input` (threads to the next handler;
  such a handler returns `0`). A handler failure also blocks: the point is
  fail-closed. The adapter consumes the `reason`, the rewritten `input` and the
  `terminate` hint through `AgcToolVetoDecision` (§4.1); the agent-side consumer
  is live (P3; terminate P4.4a). This fixes v1, where `HOOK_FIRST` silently
  dropped an argument rewrite.
- **terminate (pi-exact, P4.4a):** only a blocked call can terminate, and the
  adapter drops a `terminate` that arrives without `block`. The agent ends the
  submission after the tool batch only when **every call in the assistant
  message** was blocked with `terminate` (a mixed or normal call keeps the loop
  running). Cancellation is checked first and wins; an explicit
  `turn_end.continue` still re-enters the loop once under the shared
  continuation cap; `agent_end` keeps the assistant stop reason
  (`stop_reason:"tool_use"`) — there is no `terminated` field.
- **continuation budget**: `turn_end` and `agent_before_settle` share one
  per-submit cap of 32 honored `continue` requests (`AGENT_CONTINUATION_MAX`);
  past it the continuation is logged and ignored, so a handler cannot spin the
  agent forever. `turn_end` honors the request on every non-hard turn, whether
  the turn ended without tool calls or with a tool batch still to answer.
- **request block (`REQ_BLOCKED`)**: a blocked or failed (fail-closed)
  `before_provider_headers` / `before_provider_request` is terminal for the
  request. The agent sets `REQ_BLOCKED`, records
  `request blocked by extension` as the error, emits the error terminal path,
  skips retries and tools, and makes no transport call. A blocked compaction
  summary request fails compaction with
  `compaction: request blocked by extension`.

### 4.3 Merge algebra

- **replace**: the result object replaces the payload value wholesale.
- **fields**: for each key present in the result, replace that key; a key present
  with JSON `null` deletes it; absent keys are unchanged.
- **A `fields` result is a patch, not a new object.** `AgcExtResult.result_json`
  is the accumulated patch: the accumulator overlays each handler's keys and
  **preserves the JSON `null` delete markers**, while the payload handed to the
  next handler is the *effective* payload already patched. Consumers apply the
  returned patch with the helper `agentc_ext_merge_fields(base, patch)` (the
  public wrapper over the internal `json_fields_apply(base, patch, false)`;
  `keep_nulls=true` builds the accumulated patch during a walk). The caller
  frees the result with `agentc_free`.
- `tool_call`/`tool_result`/`message_end`/`before_provider_headers` merge
  `fields`. `before_provider_headers` is special in *shape* only: its result is a
  **flat header patch** `{ "<Header>": value|null }` — merged directly by the
  consumer so a single `null` deletes one header — never a nested
  `{headers:{...}}`. The generic merge stays shallow and never descends into a
  value. Incoming duplicate headers are collapsed to the last occurrence in the
  payload; the patch replaces every occurrence of a patched name in place (or
  drops it for `null`), preserves order, appends unmatched keys in patch order,
  and lets the last duplicate patch key win. See §10 for the forbidden headers.

### 4.4 Capability gating

Both the `1` return **and** `*result_json` require `OVERRIDE`. An `OBSERVE`
handler that returns `1` or a result is an error: logged, ignored, and (for
override points) treated as a failed handler (§4.5). Required caps are declared
per point (§4.1); `on` rejects a mismatched subscription.

### 4.5 Failure, budget, and fast path

- A handler returning `<0` is a failure. **Observe** points are fail-open (log,
  continue). **Override** points are fail-closed: the operation is blocked with a
  diagnostic (`error: tool blocked: extension <name> failed` / trust denied).
- Per-point budget (default 2 ms, `AGENTC_EXT_BUDGET_NS`). A per-handler
  consecutive-overrun counter is reset by a call within budget; **three**
  consecutive overruns (`AGENTC_EXT_MAX_OVERRUNS`) disable the handler
  process-wide, after which `agentc_ext_wants` no longer reports it and it is
  skipped. The overrunning occurrence itself follows the point's failure mode:
  fail-open for observe (log, continue) and fail-closed for override (block
  with the same diagnostic as a `<0` return). This fixes the v1 fail-open hole.
- **`agentc_ext_wants(point)`** returns true if any subscriber exists. Every emit
  site guards on it before building a payload. Without it, override points would
  serialize the whole request/headers/body even with no extensions.

### 4.6 The typed hub is separate

`src/core/events.c` stays the front-end bus (typed structs, cheap streaming
deltas). The registry is the extension bus and is **always called directly** at
each site; it bridges the typed hub for observe points only when
`agentc_ext_wants(point)` is true. This avoids v1's double-dispatch ambiguity and
keeps `message_update` free when no extension subscribes. `agentc_events_set_quiet`
suppresses front-end delivery only; declared `OVERRIDE` points must still
dispatch, and the override emit sites must not go through the quiet-gated
`want()`. P3 delivered the agent-side `want()` split: `want_override` (registry
`agentc_ext_wants` only) for result-consuming points and `want_observe`
(registry wants plus the quiet gate) for observe points with no typed event.

### 4.7 Emit sites (live map)

**P3 status:** the rows below are the live emit sites. P3 landed `project_trust`
(emitted before trust resolution and consumed), the `resources_discover`
consumer, `input`, `before_agent_start`, the direct
`message_end`/`tool_result`/`turn_end` overrides (before append/observer),
`agent_before_settle`/`agent_settled`, the `before_provider_headers` flat patch
and the consuming `before_provider_request`, `after_provider_response` response
headers, `provider_stream_event` model, the compact points, and
`model_select`/`thinking_level_select` with the app model/thinking sinks. Observe
points with a typed front-end event stay on the quiet-gated typed bridge;
result-consuming points are dispatched directly with `agentc_ext_emit` behind
`agentc_ext_wants` (§4.6). `AGENTC_EV_ERROR` / `AGENTC_EV_COMPACT` are unmapped;
compaction is observed via `session_compact` (success) and
`session_compact_failed` (failure) with their §4.1 payload shapes. P4.4a added
`session_before_switch` (the RPC new-session gate) and the all-terminate batch
consumer; `session_start` now goes through one shared payload helper called by
the startup and new-session sites.

| point | site |
|---|---|
| `project_trust` | `src/app/project.c:93–101`, called from `main.c:513` **before** trust resolution; result consumed (§8) |
| `session_start` | shared payload helper `mode.c:511–534`; startup `mode.c:703–705`, new-session `mode_rpc.c:315` |
| `session_shutdown` | `main.c:653–654,670–671,709–710` |
| `agent_start` | `agent.c:1580` via the typed bridge (`registry.c:2105`) |
| `agent_end` | `agent.c:1930`; typed payload `{stop_reason}` |
| `turn_start` | `agent.c:1606` |
| `turn_end` | direct override at `agent.c:646` (emit `:658`), called `agent.c:1778,1794,1817,1912`; the typed front-end event stays `{}` |
| `message_start` | `agent.c:1617` |
| `message_update` | `agent.c:1250` (`emit_text`) |
| `message_end` | direct override `agent.c:507` (emit `:533`), dispatched before append/observer at `agent.c:1774`/`:1786` |
| `tool_execution_start/end` | `agent.c:1340,1354` (completion order) |
| `tool_result` | direct override `agent.c:554` (emit `:578`), consumed at `agent.c:1896` before append/observer |
| `before_provider_headers` | `agent.c:1410`, after the head/body split |
| `before_provider_request` | `agent.c:1453` |
| `after_provider_response` | `agent.c:1517` (typed bridge, observe) |
| `provider_stream_event` | `agent.c:1275` (typed bridge, observe) |
| `tool_call` | adapter `registry.c:3532–3575` (rewritten `input`, `reason`, the `terminate` gate); agent consumer `agent.c:1919` (rewritten `input` + `reason`) and `agent.c:2006` (all-terminate batch) |
| `session_before_compact`/`session_compact*` | `agent.c:1002–1035` (`compact_failed`/`compact_completed`) and `agent.c:1043–1090` (`compact_now`) |
| `session_before_switch` | `mode_rpc.c:261–281` (emit `:263`), called from `rpc_new_session` at `mode_rpc.c:297` |
| `mcp_servers_change` | `mcp.c:3211–3246`; emitted at record creation `mcp.c:3967`, READY `mcp.c:3335`, FAILED `mcp.c:3495` |
| `model_select`/`thinking_level_select` | `agent.c:703–771` (`agentc_agent_set_model` / `agentc_agent_set_thinking`); effective model change on rebuild `mode.c:333–385` |
| `input` | `agent.c:331` (emit `:343`), called at `agent.c:1566` |
| `before_agent_start` | `agent.c:374` (emit `:385`), called at `agent.c:1579` |
| `agent_before_settle` | `agent.c:677` (emit `:687`), called at `agent.c:1934` |
| `agent_settled` | `agent.c:1938–1941` |
| `resources_discover` | `src/app/project.c:176–200`, called from `main.c:515` after trust |

**Job-driver note (P4.3).** The internal driver (`src/core/tools/jobs.c`) gained
an appended `AgcJob.cleanup(self, job, reason)` hook (`src/core/tools/jobs.h`),
invoked exactly once on fatal, timeout, cancel and release, immediately before
`job->priv` is freed. The driver clears the pointer before calling, so the
release sweep cannot call it twice. Async extension tools set it in their
adapter `start`; the job reason maps to the tool `stop` reason
(`AGENTC_JOB_STOP_ERROR/TIMEDOUT/CANCELLED/RELEASE` →
`AGENTC_EXT_TOOL_ERROR/TIMEOUT/CANCELLED/UNLOAD`). This hook is core-internal,
not ABI surface.

---

## 5. Registry and lifecycle

New module `src/ext/registry.c` (+ internal `src/ext/registry_int.h`). The
registry's app-facing API uses **internal** types (`AgcTool`, `AgcBuf`); only the
extension ABI uses `AgcExt*`.

```c
/* process-wide host vtable (include/agentc_ext.h types) */
const AgcExtHost *agentc_ext_host(void);

void   agentc_ext_register_defaults(bool no_mcp);   /* builtin-tools, builtin-context, mcp */
void   agentc_ext_register(const AgcExt *ext);      /* pending; no init yet */
int    agentc_ext_adopt(const char *name, int (*entry)(const AgcExtHost *, AgcExt *));
void   agentc_ext_register_linked(void);            /* build/exts.c */
void   agentc_ext_register_dynamic(void);           /* P4.5: <config home>/extensions/, macOS/Windows */
void   agentc_ext_load_all(void);                   /* init, lowest `order` first, bounded rounds */
void   agentc_ext_unload(const char *name);         /* owner registrations first, then shutdown */
void   agentc_ext_apply_config(const AgcConfig *cfg);
void   agentc_ext_pump(void);                       /* deferred queue, one HTTP, pump hooks */

size_t agentc_ext_tools(AgcTool *out, size_t max);  /* count-first */
typedef struct { const char *name; const char *description; } AgcExtCommandInfo;
size_t agentc_ext_commands(AgcExtCommandInfo *out, size_t max);
char  *agentc_ext_run_command(const char *name, const char *args);
void   agentc_ext_render_sections(AgcBuf *out);     /* sorted extension sections */

bool         agentc_ext_wants(const char *point);
AgcExtResult agentc_ext_emit(const char *point, const char *payload_json);
void         agentc_ext_tool_veto(void *ud, const char *tool_call_id,
                                  const char *tool_name, const char *args_json,
                                  AgcToolVetoDecision *out);
/* apply a fields patch: keys replace, JSON null deletes (caller frees) */
char        *agentc_ext_merge_fields(const char *base_json, const char *patch_json);
bool   agentc_ext_dirty(void);
void   agentc_ext_clear_dirty(void);
void   agentc_ext_emit_agent_event(int ev, const void *data);
void   agentc_ext_shutdown(void);

/* descriptor introspection: count-first, registration order, real state */
size_t agentc_ext_describe(AgcExtInfo *out, size_t max);
bool   agentc_ext_is_loaded(const char *name);

void   agentc_ext_set_context(const AgcExtContext *ctx);
typedef void (*AgcExtEntrySink)(void *ud, const char *type, const char *data_json);
void   agentc_ext_append_entry(const char *type, const char *data_json);
void   agentc_ext_set_entry_sink(AgcExtEntrySink sink, void *ud);
void   agentc_ext_set_ui_sink(const AgcExtUiSink *sink, void *ud);
void   agentc_ext_set_model_sink(AgcExtModelFn cb, void *ud);
void   agentc_ext_set_thinking_sink(AgcExtThinkingFn cb, void *ud);

const AgcExt *agentc_builtin_tools_ext(void);
const AgcExt *agentc_builtin_context_ext(void);
const AgcExt *agentc_mcp_ext(void);
```

`agentc_ext_remove_tool_internal(name)` (declared in `src/ext/registry_int.h`, the
internal bus) retires a tool by name, marks the registry dirty, and **never
frees handed-out data**; MCP re-sync uses it to retire and replace tools (§7).

The `main.c` composition becomes:

```c
agentc_ext_register_defaults(no_mcp);        /* builtin-tools always; mcp unless --no-mcp */
agentc_ext_register_linked();
agentc_ext_register_dynamic();              /* P4.5: no-op without a platform loader */
agentc_ext_apply_config(cfg);                /* extensions.disabled: defaults + linked + dynamic */
agentc_ext_load_all();                       /* BEFORE trust resolution */
/* ... trust resolution, consuming project_trust (see §8) ... */
size_t navail = agentc_ext_tools(NULL, 0);
AgcTool *available = agentc_alloc((navail ? navail : 1) * sizeof *available);
agentc_ext_tools(available, navail);
/* --no-tools / --tools (strict; deferred mcp__ names) / default_tools, main.c */
```

`agentc_ext_apply_config` reads `extensions.disabled` (§9) after
`register_linked`/`register_dynamic`, so default, linked and dynamic extensions
can be excluded. `--no-mcp` and
`--no-tools` remain locals and are applied as above; `default_tools` is unchanged.
`--tools` stays strict: a name matching no collected tool exits 2 — **except** a
name beginning `mcp__` and the three exact resource-tool names
(`mcp_list_resources`, `mcp_list_resource_templates`, `mcp_read_resource`),
which are deferred when their server/list has not arrived yet. The
bounded R1 startup pump (400 ms, main.c) runs before this check and exits early
once `agentc_mcp_pending()` is false (or no server record exists), so a fast
local server's tools are already in the table; a still-missing name is deferred
and `agentc_policy_recompose` selects it once the server connects and the app recomposes
at the turn boundary. A missing non-MCP name still exits 2. With MCP disabled
the deferred name never resolves, is not an error, and is logged as "MCP is
disabled in this session" instead of promising a connection. See §16.

P4.5 adds **optional dynamic registration**. `agentc_ext_register_dynamic()`
scans one fixed directory — `<config home>/extensions/` — through the platform
loader (`os_ext_*`; `docs/10-platform.md` §1) and registers each candidate as a
pending extension (`AgcExtInfo.dynamic` marks it; `--list-extensions` appends
`(dynamic)`). Duplicates are skipped before the library is opened, and a
candidate whose open, symbol lookup, descriptor or registration fails is logged
and skipped without disturbing a good sibling. On a platform without a loader
(the Linux static build) the call is a no-op. Running it after
`register_linked()` and before `apply_config()` gives linked and dynamic
extensions the same `AgcExt.order` and `extensions.disabled` treatment.

**Dynamic close discipline.** A dynamic library's handle is closed exactly once
and only when the extension is idle: registrations removed, `shutdown()`
returned, no live async job for the owner, and no extension callback on the
stack (`owner_get()==0`, not pumping). `agentc_ext_unload()`, a failed `init`,
and `agentc_ext_shutdown()` request the close; when that moment is busy the
close is deferred to the last job unlink, the pump epilogue or shutdown. At
shutdown a mapping still needed by a live job (or by a callback on the stack)
is kept mapped with a warning — the process is exiting, so leaving it mapped is
the safe fallback (D141-b). A loaded library is trusted native code (§10).

**Shutdown during load (post-P4 close-out).** `agentc_ext_load_all` keeps a
`g_loading` depth across its bounded rounds and any nested `load_all` an `init`
starts. While it is nonzero, `agentc_ext_shutdown()` is a logged no-op
(`error: ext: shutdown called during load; ignored`): the record vector is live
under the running load loop, and freeing it there would leave the loop with
stale records. The process-end shutdown still tears everything down. A
re-entrant `agentc_ext_unload` of the record being initialized stays safe —
`load_all` re-acquires the record after each `init` and skips it when its `used`
flag was cleared (D149).

### 5.1 Recompose

The public `add_tool` (and the internal `agentc_ext_add_tool_internal` used by
pump-discovered MCP tools) and `request_recompose` set `dirty`;
`add_command`/`add_section` do not, because commands and sections are read live
from the registry at menu/render time. **Recompose is applied only after
`agentc_tool_jobs_run` returns**, never from `agentc_ext_pump`.
Rationale (review blocker 24): `bash.c:304` and the TUI poll (`tui.c:1669`) call
`agentc_pump` mid-job; applying `agentc_agent_set_tools` there frees the array
holding `AgcJob.tool` pointers → use-after-free.

**Turn boundary (exact):** after the tool batch returns and before the next
provider request; and immediately before a terminal `agent_end` on abort/error.
The core applies `dirty` at those two points. In print mode this means late MCP
tools appear on the **next turn**; the acceptance wording is narrowed to that
(they cannot appear mid-`agentc_agent_submit`).

**Who re-applies the filter (P2 addition).** The spec left unanswered how the
app's active-tool policy (`--tools` / `default_tools` / `--no-tools`) survives a
recompose, since that policy lives in `main.c` while the core drives the turn
loop. The implementation adds an app-installed hook:

```c
/* agent.h */
typedef void (*AgcRecomposeFn)(void *ud, AgcAgent *a);
void agentc_agent_set_recompose(AgcAgent *a, AgcRecomposeFn fn, void *ud);
```

The agent calls `a->recompose` at the two boundaries above (never mid-batch); the
callback checks `agentc_ext_dirty()`, clears it, re-collects `agentc_ext_tools`,
re-runs the same selection policy, and installs the result with
`agentc_agent_set_tools`. `main.c` supplies it and threads it through
`AgcModeConfig` so `agentc_mode_rebuild_agent` reinstalls it too. This keeps the
filter rule in the app layer instead of teaching the registry about CLI flags.

### 5.2 Tool identity across a swap

The assistant message's tool calls are resolved against a **snapshot** of the
table taken at the start of the batch, so a mid-batch recompose cannot change
resolution. MCP entries referenced by an in-flight transcript are never freed
(`mcp.c` already keeps handed-out entries alive).
`agentc_ext_remove_tool_internal(name)` preserves that guarantee: it retires a
tool from the table but never frees a record that may still be referenced by an
in-flight transcript.

---

## 6. Core as extensions

### 6.1 `builtin-tools`

`src/core/tools/registry.c`'s `builtins[]` (`:244–259`) is registered via
`agentc_ext_add_tool_internal` (the internal bus of §0/§11) by the
`builtin-tools` extension; it does not go through `host->add_tool`. **The `bash`
description must still be rewritten per `agentc_config_shell()`/`os_shell_kind()`**
(`:263–279`) — a naive copy loses the cmd/PowerShell variants.

### 6.2 Prompt sections

`agentc_prompt_build` (`prompt.c:59–93`) becomes `agentc_ext_prompt_build(AgcBuf*)`,
rendering four core sections in their current order (`preamble`, `tools`,
`rules`, `environment`) plus extension sections. The current helpers
`agentc_prompt_add_project_context` (`prompt.c:29`) and `agentc_prompt_add_skills`
(`prompt.c:40`), called from `main.c:631/635`, become sections registered by a
`builtin-context` extension. `agentc_prompt_build` is also called from
`agent.c:795`; that path routes through `agentc_ext_prompt_build`.

In auto mode (no explicit `--system`) the agent rebuilds the prompt from the
live tool table before every turn via `refresh_system()`, so a turn-boundary
recompose is reflected in the next provider request. An explicit `--system`
hard-replaces the build and stays fixed for the run; a per-run
`before_agent_start` `systemPrompt` override wins over both and is re-published
to the extension context on every request. The tool list is included in the
prompt, so a late MCP tool is advertised at the same boundary that installs it.

**P4.2 — prompt templates, `/skill:<name>` and themes (delivered).** The
prompt/context side grew three resource families that share one set of roots and
validation rules:

- **One invocable-prompt registry** (`src/core/prompts.h`, cap
  `AGENTC_PROMPTS_MAX` 512). File templates and MCP prompts (and, later,
  extension prompts) register pointer-stable records; a later same-name
  registration retires the earlier one. `agentc_prompts_list()` is the
  count-first borrowed view the TUI menu uses, `agentc_prompts_has/expand` is
  invocation, and `agentc_prompts_clear_source(source)` retires one owner's
  records. Records are never freed, so a menu that borrowed a name pointer keeps
  valid memory for the process lifetime; the cap counts retired records too.
- **File templates** are `*.md` under `<config>/prompts`, `<cwd>/.agentc/prompts`
  (trusted) and each `resources_discover` prompt root, in that order; a later
  same-name template wins. The stem must match `[a-z0-9._:-]{1,64}` (an invalid
  stem is skipped with a warning) and frontmatter may carry
  `description`/`argument-hint`, both stored in the record (the TUI menu shows
  the description; `hint` is available to front ends from
  `agentc_prompts_list`). Expansion keeps the existing `$1..$n` / `$@` /
  `${N:-default}` grammar.
- **`/name`** invokes a registered prompt from the TUI (and
  `agentc_prompts_expand` backs the RPC `prompt_template` command): the expanded
  text is submitted through the normal submit path, exactly once. Built-in slash
  commands shadow a same-named prompt, and a prompt shadows a same-named
  extension command; the menu applies the same rule.
- **`/skill:<name>`** reads the `SKILL.md` body for `name` from the standard and
  resource skill roots, strips the frontmatter, caps the body at
  `AGENTC_SKILL_SUBMIT_MAX` (256 KiB), appends the trailing arguments and submits
  it; unknown or over-cap skills post a notice instead of falling through to the
  model.
- **Named themes** resolve to `themes/<name>.jsonc` files (stem
  `[A-Za-z0-9._-]{1,64}`, `.`/`..` rejected) from `<config>/themes`,
  `<cwd>/.agentc/themes` (trusted) and each `resources_discover` theme root, with
  the same later-wins scan and a 64-entry per-scan cap. The effective base is
  chosen by the built-in name (`dark`/`light`), then the legacy
  `<config>/theme.jsonc` `base`, then the named file's `base`; the legacy file's
  slot overrides apply next and the named file's slots last, so the most specific
  file wins slot by slot. `system` uses the `COLORFGBG` heuristic (last field;
  0–6 and 8 dark, 7 and 9–15 light; absent/unparseable falls back to dark);
  OSC-11 is not implemented. `/theme <name>` applies a name at runtime (unknown
  names log once and keep the current theme); the config `theme` key is the
  startup default (`"system"`), and project themes resolve only under the app's
  already-resolved trust verdict, which the app threads into the TUI.
  `resources_discover` theme roots are the most specific name-resolution level.

### 6.3 Status

`add_status` forwards to `agentc_status_register` (`status.c:35`). `status.h` is
repointed to `agentc_ext.h` and `AgcStatusSegment`/`AgcStatusProvider` become the
`AgcExtStatus*` types (§3); `status_builtin.c` uses them unchanged in shape.
`set_status` folds into the same table as an auto-clearing segment **keyed by
(owning extension, key)**, so two extensions may use the same key without
collision; eight live keys are allowed process-wide and a ninth is logged and
ignored. The registry owns the text; NULL or empty `text` clears a key, and
unload/shutdown removes it with its owner. A text change or clear bumps the
status version (`agentc_status_invalidate()`), so the TUI snapshot rebuilds, and
the backing provider registration is idempotent and re-arms after an external
`agentc_status_reset()` (the fixed staleness bug). `notify`
and `set_title` stay optional front-end UI-sink services that fall back to the
host log when no front end installs them; the TUI installs a sink whose
`set_title` falls back to a chat notice because the terminal layer has no title
operation. `set_model`/`set_thinking` are optional
front-end service sinks: the registry plumbing is covered by `ext_test`, and P3
installs them in the app (`agentc_mode_setup`, cleared in teardown): a
same-provider `set_model` goes through the agent setter, a different provider is
refused with `-ENOSYS` (an unsafe rebuild from a tool run or under a cached
front-end agent; provider switching stays exclusive to the RPC command, which
owns a safe idle rebuild), and `set_thinking` accepts `off|low|medium|high` (§13).

---

## 7. MCP as a default extension

- `agentc_mcp_start(cwd, trusted)` — read config, create `McpServer` records,
  return immediately. No connect.
- `agentc_mcp_pump(void)` — **advances exactly one server one step per call**,
  round-robin across the server list, within a small budget. A slow or
  unreachable server therefore delays only its own slice; the per-server state
  machine lives in `mcp_server_step`.
- **`agentc_mcp_tools(out, max)` is a pure snapshot.** The pump owns every
  connect and re-sync (`mcp_server_step` / `mcp_sync_handle_response`), so the
  registry's collect step never blocks for the timeout.
- **Capability negotiation (P4.2).** The initialize response's
  `result.capabilities` decides which lists the connect round fetches: `tools`,
  `prompts`, `resources` and resource templates (`resources` covers both
  `resources/list` and `resources/templates/list`). Missing, malformed or
  present-but-empty capabilities fall back to **tools-only** (the pre-P4.2
  behavior). Each kind's `listChanged` bit is recorded; a
  `notifications/<kind>/list_changed` is honored only while the kind's base
  capability is advertised (otherwise it is debug-logged and never fetched).
- **Per-kind list sync (P4.2).** After `notifications/initialized` every
  advertised kind is fetched in enum order — tools → prompts → resources →
  resource templates. Each kind owns its own pending/again latches, pagination
  cursor and page count (100-page cap); a notification that arrives while that
  kind's sync is in flight latches a follow-up that runs after the current
  commit, and a failed re-sync on an already-ready server keeps the previous
  tables and leaves the latch pending. A tools-cap failure (`-ENOSPC`/frozen
  cap) fails the whole server (R3); a prompt/resource cap failure abandons that
  refresh only.
- **MCP prompts (P4.2).** A committed `prompts/list` registers each prompt in
  the core prompt registry as `mcp__<server>__<prompt>` (64-byte name cap,
  hash-deduped) with source `"mcp"`, so it is invocable like a file template
  (`/name` in the TUI, `prompt_template` over RPC). Invocation parses
  positional/named arguments against the listed prompt arguments (≤16 args,
  ≤64-byte names, required arguments enforced, unknown/extra arguments logged
  and ignored) and performs one bounded synchronous `prompts/get`
  (`MCP_PROMPT_TIMEOUT_MS` 10 s). Only `type:"text"` blocks are joined with
  `"\n\n"` (non-text blocks are skipped and logged) and the result is capped at
  `AGENTC_LIMIT_MCP_PROMPT_TEXT_BYTES` (64 KiB) — over the cap the call fails,
  it never returns a truncated prompt. Prompt records are capped at 128/server
  and 512 total with a 128-entry frozen pile of retired-but-listed records
  (removed from the registry, never reused).
- **MCP resources (P4.2).** `resources/list` and `resources/templates/list` are
  committed into per-server resource/template tables (256/server, 1024 total;
  a capped commit keeps the previous table). Field caps:
  `AGENTC_LIMIT_MCP_RESOURCE_URI` 2048 (an over-long uri/uriTemplate is skipped,
  never clipped), name/title 256, description 2048, mime 128 (truncated with a
  log). The tables are exposed through three generic read-only tools —
  `mcp_list_resources`, `mcp_list_resource_templates` and
  `mcp_read_resource {server,uri}` — published lazily at the first successful
  resource-capable list sync (the internal add marks the registry dirty, so the
  app's next-turn recompose picks them up; they are never removed). The list
  tools return paged JSON text from the local table (`cursor` = decimal offset;
  64 KiB page cap; `server` filters one live resource-capable server);
  `mcp_read_resource` performs one bounded synchronous `resources/read` on a
  live resource-capable server and returns joined `type:"text"` contents only —
  a `blob` entry rejects the whole read with `binary resource not supported`,
  and a result over `AGENTC_LIMIT_MCP_RESOURCE_TEXT_BYTES` (256 KiB) fails
  without truncation. A server that never advertises `resources` is never asked
  for either list.
- Registers as the `mcp` extension: its `init` registers a periodic pump through
  the internal bus (`agentc_ext_add_pump`, `registry_int.h`); the registry's
  `agentc_ext_pump` invokes it on every main-loop pump. The pump's **first call**
  reads the config and creates the server records — deliberately after
  `load_all`, when trust and the final context are settled (D57); the app's
  bounded startup pump (400 ms, R1) runs it before the first tool composition
  and exits early once `agentc_mcp_pending()` is false. It registers no tools at
  `init`; each committed `tools/list`
  publishes newly discovered tools with `agentc_ext_add_tool_internal` (which
  marks the registry dirty) and emits `mcp_servers_change`. An entry is marked
  published **only when the add succeeds**; a duplicate or failed publish is
  logged once and left unmarked rather than assumed registered. A re-sync that
  retires or replaces a tool withdraws the registry entry through
  `agentc_ext_remove_tool_internal(name)` (internal bus; marks dirty, never
  frees handed-out data) before the fresh entry is published, so the next
  collected table shows the new description and no longer advertises the removed
  tool.
- **Memory bounds (R3).** The tool table is bounded: `MCP_MAX_TOOLS_PER_SERVER`
  256 and `MCP_MAX_TOOLS_TOTAL` 1024 (a publish past either budget fails the
  sync with `-ENOSPC` and is logged); a remote tool name over
  `MCP_MAX_TOOL_NAME` 256 bytes is skipped (never truncated — a truncated name
  would call the wrong tool); a description over `MCP_MAX_TOOL_DESC` 8 KiB is
  truncated with a log. Published names are built inside the registry's 64-byte
  name cap (`MCP_EXPOSED_NAME_MAX`): a collision appends a hash suffix supplied
  by `agentc_mcp_name_hash`, clipping the base but never the hash. Retired but
  handed-out entries live in a frozen pile capped at `MCP_FROZEN_CAP` 256.
  A handed-out entry is never freed or its slot reused — a live `AgcTool.ud`
  may still reference it until the next turn-boundary recompose — so when the
  pile is full a new identity fails the server with a clear log instead of
  recycling; only entries that were never handed out are ever freed/reused.
- **HTTP cancel (R3).** The HTTP transport passes its cancel predicate to the
  wire layer with `agentc_http_set_cancel`, so an abandoned MCP request returns
  `-ECANCELED` instead of waiting out the request timeout. `agentc_mcp_server_count()`
  is the configured-server count (terminal records stay counted);
  `agentc_mcp_pending()` is the readiness predicate (false once every record is
  READY/FAILED, or when none exists) that the startup pump polls.
- **Idle stdio drain (P4.4b, O-7).** A READY stdio server can push
  `notifications/<kind>/list_changed` (or any other notification) while no
  request is outstanding. Every pump drains it: read whatever is buffered, honor
  at most 64 complete messages (`MCP_IDLE_DRAIN_MAX_MSGS`; leftovers stay in
  `rbuf` for the next pump), dispatch notifications through the same
  list-changed latch, and drop a response that belongs to no awaited request.
  EOF from an idle server fails it, which arms the reconnect below. HTTP has no
  idle channel: a response is request-scoped, so a stream notification is only
  seen inside the request that carries it.
- **Reconnect/backoff (P4.4b, O-6).** A `FAILED` record stays non-pending while
  it waits (the startup budget is unaffected). Once `retry_at` passes, the pump
  tears the transport down, clears the session/buffers and every in-flight sync
  bit, and re-enters `NEW` with a fresh connect budget; the failure counter is
  kept so the backoff keeps growing. The delay is `AGENTC_MCP_RETRY_BASE_MS`
  (1 s) doubled per failure up to shift 6, capped at `AGENTC_MCP_RETRY_MAX_MS`
  (60 s). `AGENTC_MCP_RETRY_MS` overrides the base delay: a positive value is
  clamped to `[1, 60 s]`, `0` (or a negative value) disables reconnect
  (`FAILED` is terminal), and an unset or malformed value keeps the default. `agentc_mcp_retry_delay_ms()` is
  internal (`src/ext/mcp_int.h`, exposed for tests). `retry_count` is only reset
  when the record is created (`agentc_mcp_start`), so after the first failure
  the backoff shift accumulates per record even across successful reconnects.
- **`mcp_servers_change` status (P4.4b, O-5).** The payload is
  `{"servers":[{"name","kind","status","caps"?}]}` with every non-NULL record
  (not only the READY ones) and `status` one of `connecting`/`ready`/`failed`;
  `caps` is present only after an accepted initialize and lists `tools`,
  `prompts`, `resources`, `resource_templates` (the `resources` MCP capability
  gates both resource list kinds). It is emitted when the records are created (the first pump), when a
  server becomes READY, and when one fails, so a front end can render the
  server list without polling the extension.
- **Bounded stdio write (post-P4 close-out).** A request is written through
  `agentc_mcp_write_all` in `PIPE_BUF` (4096-byte) chunks against a deadline, so
  a request larger than the pipe buffer cannot block the main loop on a full
  pipe. The first chunk is attempted **write-first**: an empty anonymous pipe
  accepts data even where `os_poll()` never reports `POLLOUT` for a write end
  (Wine's write-only pipes match no poll branch), so polling before the first
  byte would time a small request out. Only an `EAGAIN`/`EINTR` probe falls
  into the bounded poll path; later chunks poll before writing. **Residual
  (Windows):** an anonymous-pipe write is blocking and `os_poll()` has no
  write-pipe branch, so after the first chunk a full pipe can sleep past the
  deadline and a request larger than one `PIPE_BUF` still times out; the
  complete fix belongs in `src/plat/win/sys.c` (§12).
- `agentc_mcp_shutdown` unchanged.
- `agentc_mcp_load(cwd, trusted)` is retained as a **synchronous test helper**
  (`start` + pump-to-completion) so the offline `mcp_test`/`mcp.sh` harness keeps
  one entry point; the app path is `start` + the registry pump.
- **HTTP steppability (spec deviation, found in P2):** `agentc_http_run` is not
  resumable (it owns a socket and polls to completion), so the HTTP transport
  cannot be split into read/write steps the way stdio can. The stdio path is a
  true non-blocking state machine; the HTTP path advances one bounded request per
  pump call (short timeout slice, retried next pump) with the TUI's existing
  `agentc_http_set_poll_hook` keeping the frame live. A future wire-layer
  stepping API would remove the difference.

`main.c` sets a provisional `agentc_mcp_set_context(cwd, false)` before
`agentc_ext_load_all` and the final `(cwd, trusted)` after trust resolution
(§8), then runs the bounded startup pump; the pump's first call creates the
server records, and later pumps advance the connect/re-sync state machines. Shutdown goes through `agentc_ext_shutdown`, which invokes
the `mcp` extension's `agentc_mcp_shutdown`. `--no-mcp` gates
`agentc_ext_register_defaults`.

---

## 8. Trust ordering (security)

**Delivered in P3.** Extensions are registered, config-filtered and loaded
**before** trust is resolved; `project_trust` is then emitted and its result is
consumed. The app order (`main.c:501–516`) is: `register_defaults` →
`register_linked` → `apply_config` → provisional `agentc_mcp_set_context(cwd,
false)` → `agentc_ext_load_all()` → `agentc_project_resolve_trust(cwd,
cli_trust)` → final `agentc_mcp_set_context(cwd, trusted)` →
`agentc_project_apply_resources(cwd, trusted)` →
`agentc_builtin_context_set(cwd, trusted)`. No registry pump runs between
`load_all` and the final context; the MCP extension creates its servers on the
first pump with the final context (D57).

`src/app/project.c` implements the precedence:

1. An explicit `--approve`/`--no-approve` verdict wins and the hook is **not
   consulted**.
2. Otherwise the first `project_trust` handler decision is consumed:
   `trusted:"yes"|"no"` decides, `trusted:"undecided"` falls back to
   `agentc_trust_resolve`, and `remember:true` persists the decision via
   `agentc_trust_save`.
3. A blocked handler, a `handled` result without a decision object, or a
   malformed decision is **fail-closed**: the project stays untrusted and the
   fallback / `remember` are skipped.

**Documented limitation.** The project config has already been loaded when the
hook decides (config load happens before extension load, in `main.c`). A project
that becomes trusted only through this hook cannot retroactively apply
`<cwd>/.agentc/config.jsonc` in the same run; `project.c` logs
`warning: project became trusted after config was loaded; … restart to load it`.
The trust verdict itself (context files, skills, prompts, MCP context) is fully
effective this run.

`resources_discover` (emitted at startup with `{cwd, reason:"startup"}`) is
consumed after trust: every path must be absolute, free of any `..` segment,
and under the config home (always) or the project cwd (only when trusted);
out-of-bounds paths are rejected and logged, never trusted to the handler. A
blocked or failed handler rejects the whole batch (**fail-closed**: no paths are
consumed) and the rejection is logged. Since P4.2 all three lists are consumed
into per-kind, process-lifetime root stores (`skillPaths`, `promptPaths`,
`themePaths`) that feed the skills/template/theme loaders in registration order;
each list accepts at most `AGENTC_RESOURCE_PATHS_MAX` (32) paths and each store
is deduplicated and capped at `AGENTC_RESOURCE_ROOTS_MAX` (8), so a repeated
discovery or a hostile hook cannot grow the set.

**Session rebind (P4.4a).** The RPC `new_session` command is the one session
switch. `session_before_switch` (`override/first`, fail-closed) is emitted before
any file is touched and cancels the switch (a blocked/failed handler, a
`handled` result without a boolean decision, a malformed `cancel`, or
`{"cancel":true}`), leaving the current session, its transcript and the
extension context intact; the RPC answers
`session switch cancelled by extension`. An allowed switch closes the old
session, creates the new one and calls `agentc_mode_rebind_session`, which
re-publishes the new `session_id`/`session_file` to the extension context and the
entry sink (the only context fields that move) and reinstalls the ctx-bound
persistence observer before any message can be appended — a later flush can
never reach the closed old session. `session_start{reason:"new",
previous_session_file}` is then emitted through the same helper as startup, and
no `session_shutdown` is emitted (extensions are process-lifetime). `set_model`
rebuilds the agent and re-installs the observer (still bound to the RPC frame
until the run returns); `agentc_mode_rpc_run` then calls
`agentc_mode_rebind_session(c)` before returning, re-pointing the persistence
observer at the caller's `AgcModeCtx`, so a later transcript append cannot
reach the dead frame (D150). The run's `rpc_event` event pump stays bound to
the stack frame by design: production tears down immediately after the run,
and nothing observes afterwards.

---

## 9. Config

Add to `config.[ch]`:

```jsonc
"extensions": { "disabled": ["mcp"] }
```

`AgcConfig` gains an `extensions_disabled` string list; `apply_config` parses it;
`agentc_ext_apply_config` excludes extensions **by name** — default and linked
alike. The app applies it **after** `agentc_ext_register_linked()` and before
`agentc_ext_load_all()`, so a linked extension named in `extensions.disabled` is
removed before it can init (`main.c` order: `register_defaults` →
`register_linked` → `apply_config` → `load_all`). `mcp.jsonc` stays
(`include/mcp.h:3–14`). Static and dynamic extensions only; no
`extensions.paths`. P4.5 fixes the dynamic location instead: one
`<config home>/extensions/` directory, suffix `.dylib` (macOS) / `.dll`
(Windows; ASCII case-insensitive match), stem = the extension name
`[a-z0-9_.:-]{1,64}`, bytewise-sorted and capped at 32 candidates per scan with
one overflow log. Unknown suffixes, invalid stems, directories and (on POSIX)
group/other-writable files or directories are ignored; a missing directory is
fine. There is no project directory, no recursion and no runtime kill switch
(`extensions.disabled` filters by name). The config home must be an absolute
path: a relative `XDG_CONFIG_HOME` is refused with `ext: dynamic scan skipped:
config home is not absolute` and the scan is skipped, so the optional scan can
never resolve against the process CWD and load libraries from wherever agentc
happens to run (D151).

P4.2 makes the existing `theme` key live: the app passes `cfg->theme` (default
`"system"`, §6.2) into `agentc_tui_run`, which resolves it before the first
frame (`/theme <name>` can change it later). The P4.1 generic
`providers.<id>.base_url` / `api_keys.<id>` handling is unchanged (user scope
only, ≤32 entries each, ids ≤64 bytes; the named presets win).

---

## 10. Security (specific)

- **`before_provider_headers`** — the result is a **flat header patch**
  `{ "<Header>": value|null }` (not `{headers:{...}}`), merged by the consumer so
  a single `null` deletes one header; the generic `fields` merge stays shallow.
  The round-trip reuses `hdr_key_ok`, the CRLF sanitizer (the `'\r'`/`'\n'` →
  space rewrite in `hdr_string`), and the header-JSON helpers (the exposed
  `agentc_ext_headers_to_json` for the payload; the internal
  `headers_json_to_lines`/`hdr_parse_lines` for parsing the block and patch); it may not
  set `Content-Length`, `Transfer-Encoding` or `Host` (case-insensitive; such
  keys are dropped with a log), and a patch that does not parse as a flat object
  is logged and the original header block kept. Incoming duplicate headers are
  collapsed to the last occurrence in the payload; the patch replaces every
  occurrence of a patched name in place or drops it for `null`, preserves
  order, and appends unmatched keys in patch order with the last duplicate patch
  key winning.
- **Response-header JSON (R2).** `agentc_http_headers_json` (the
  `after_provider_response` payload and the extension HTTP response shape) is a
  per-response cache that is forced to ASCII: every byte of a header value is
  emitted as a `\uXXXX` escape, because header bytes are attacker-controlled and
  need not be valid UTF-8. Duplicate names are collapsed to the last occurrence.
- **`before_provider_request`** replacement must be a JSON object; its length is
  taken from the replaced buffer (`body_len`), so `Content-Length` and
  `--record` reflect the replacement.
- **Destructive tools (D52, delivered).** `AGENTC_TOOL_DESTRUCTIVE` tools that
  do **not** carry the core marker (`AGENTC_TOOL_CORE`, internal to
  `include/agent.h`) are **hidden by default** (not auto-selected) unless named
  in `--tools` / `default_tools`, so an extension or MCP server cannot silently
  gain destructive default access. Core builtins (`edit`/`write`) carry the core
  marker and stay eligible. The MCP hint mapping resolves D52: `readOnlyHint:
  true` wins, otherwise `destructiveHint` defaults to **true** (the MCP schema
  default), so an annotation-free server tool reads as destructive and is
  hidden until named; a tool named in `--tools`/`default_tools` is selected even
  when it is destructive. One aggregate diagnostic (count plus up to 8 names)
  is logged per selection that hides anything. The strict-`--tools` `mcp__`
  deferral (§5/§16) is unchanged: a named destructive MCP tool is selected once
  its server connects.
- **`append_entry`** validates `custom_type` charset/length and that the payload is
  a JSON object; rejected before a session exists.
- **`emit`** is core-authoritative: an extension may publish observe points, but
  only the core consumes decisions for core points; extension-emitted payloads for
  control points are advisory and logged.
- **Dynamic extension loading (P4.5).** macOS `dlopen(RTLD_NOW|RTLD_LOCAL)`;
  Windows `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
  LOAD_LIBRARY_SEARCH_SYSTEM32` only — the library's own directory plus System32,
  no PATH/current-directory search. If those search flags are unsupported the
  call fails closed (`ERROR_INVALID_PARAMETER`); it never retries with the
  unsafe default search order. POSIX additionally requires the library file and
  its `extensions/` directory not to be group/other-writable; symlinks are
  followed, so the check applies to the target and a TOCTOU window remains
  between the scan stat and the open. A loaded library is **trusted native
  code**: it runs in-process with no sandbox, no crash isolation and the full
  process privileges, exactly like a statically linked extension (an extension
  crash crashes the process). The macOS `-nostdlib` `dlopen`, a real Windows
  kernel and ARM64/universal runtime are exercised by the `tests/dyn.sh` CI
  jobs (`make dyn-check`, `macos-14`, `windows-latest`), not by the Linux golden
  suite.

---

## 11. Migration (P1 + P2 + P3 as implemented)

| file | change |
|---|---|
| `include/agentc_ext.h` | **new** (§3, plus the appended JSON/strdup ergonomics) |
| `include/agentc_plugin.h` | **deleted** |
| `include/status.h` | repointed to `agentc_ext.h`; `AgcExtStatus*` types; `agentc_status_remove` added |
| `include/ext.h` | core/app API: `agentc_ext_register_defaults`, `_register`, `_register_linked`, `_load_all`, `_unload`, `_apply_config`, `_pump`, `_tools`, `_commands`, `_merge_fields`, `_emit`, `_wants`, `_shutdown`, `_host` |
| `src/ext/registry.c` | **new** — host vtable, descriptor/contribution registries, hook bus, owner rollback, deferred queue, HTTP, context/sinks; `agentc_ext_apply_config` covers default and linked names (applied after `register_linked`); `agentc_ext_remove_tool_internal` is the MCP retire seam |
| `src/ext/registry_int.h` | **new** — internal bus (`agentc_ext_add_tool_internal`, `agentc_ext_remove_tool_internal`, `agentc_ext_add_pump`, includes `ext.h`) |
| `src/ext/linked.c` | **new** — includes generated `build/exts.c`, defines `agentc_ext_register_linked` |
| `src/ext/plugin.c`, `src/ext/plugin_int.h` | **deleted** (absorbed by `registry.c`/`linked.c`) |
| `src/ext/mcp.c` | P2: non-blocking per-server connect/re-sync state machine (`agentc_mcp_start`/`_pump`), pure `agentc_mcp_tools` snapshot, pump-owned re-sync, `mcp_servers_change`, publication via `agentc_ext_add_tool_internal`; `agentc_mcp_load` kept as a sync test helper. P2 fix wave: one server per pump call (round-robin), in-flight `list_changed` latch, retiral/replacement via `agentc_ext_remove_tool_internal`, publish marks an entry registered only on a successful add. P3: D52 hint mapping (`readOnlyHint` wins, `destructiveHint` defaults true) |
| `src/core/tools/registry.c` | `builtin-tools` `AgcExt`; registers the `builtins[]` table via `agentc_ext_add_tool_internal`; keeps the shell-desc rewrite; core builtins carry the internal `AGENTC_TOOL_CORE` marker (P3) |
| `src/core/prompt.c`, `src/core/prompt.h` | `agentc_ext_prompt_build` + `builtin-context` section; `--system` hard-replaces |
| `src/core/status.c`, `src/core/status_builtin.c` | `AgcExtStatus*` types; `agentc_status_remove` |
| `src/core/agent.c`, `include/agent.h` | `agentc_agent_set_ext_bridge` (retired in R3; the core now calls the registry directly and uses `agentc_ext_append_entry`); P2: `agentc_agent_set_recompose` + `recompose_now()` at the turn boundary/before terminal `agent_end`; P1-hardening: `AgcToolVetoDecision` veto signature; P3: rewritten `input` threaded, core emit sites live (direct overrides + typed bridge), `want_override`/`want_observe` split |
| `src/app/mode.h`, `src/app/mode.c` | `AgcModeConfig.recompose` + `recompose_ud`, installed on every agent (including rebuilds) |
| `src/app/mode.c` | old inline veto dropped; installs `agentc_ext_tool_veto` and the ext bridge (the bridge wrapper is retired in R3; `agentc_mode_setup` now installs the one `agentc_mode_pump` duty); P3: installs the model/thinking sinks in `agentc_mode_setup` (cleared in teardown) and emits `model_select` on an effective rebuild |
| `src/app/project.c`, `src/app/project.h` | **new (P3)** — trust precedence/consumption and the root-validated `resources_discover` consumer |
| `src/app/policy.c`, `src/app/policy.h` | **new (P3)** — the `--tools`/`default_tools`/"all" selection and the D52 non-core destructive default, shared by startup and recompose |
| `src/app/main.c` | composition block → `agentc_ext_register_defaults`/`_register_linked`/`_apply_config`/`_load_all`; `agentc_ext_tools`; `agentc_ext_prompt_build`; `agentc_ext_shutdown`; P2: the recompose callback (`agentc_policy_recompose`) re-applies `--tools`/`default_tools`; P2 fix wave: strict `--tools` defers a not-yet-connected `mcp__` name (a missing non-MCP name still exits 2); P3: trust/resources ordering (`project.c`, `main.c:501–516`) and the policy extraction (`policy.c`) |
| `src/core/config.c`, `include/config.h` | `extensions.disabled` parsed into `extensions_disabled` |
| `tools/gen-exts.py` | table of `{name, entry(const AgcExtHost*, AgcExt*)}`; `agentc_ext_<ident>_init`; includes `ext/registry_int.h`; every mode validates the manifest (duplicate names, missing source, and a name whose sanitized identifier is not a valid C identifier); `--list-c`/`--list-defines` drive the build |
| `Makefile` | `EXT_REG`/`EXT_DEFS`/`linked.o`/`ext-pipeline`/`release-exts`; symbols `agentc_ext_*_init`; the build list is the manifest: every `c` entry is compiled and linked (not just `hello`) and the `rust` entries are built through cargo, so more than one of either kind links without a Makefile edit |
| `extensions/hello`, `extensions/rust_echo` | ported to `AgcExt`; flags are `AGENTC_TOOL_*` and `THREADSAFE` is gone |
| `crates/agentc-ext/src/lib.rs` | **rewritten**: new `AgcExt`/`AgcExtHost` mirror; no `AGENTC_ABI_VERSION_MIN`, offset asserts or `offset_of` gates |
| `tests/status_test.c` | repointed to `agentc_ext_host()`/`add_status` |
| `tests/ext_test.c` | rewritten for the registry + hook bus; `agentc_ext_adopt` |
| `tests/mcp_test.c`, `tests/mcp.sh` | P2: `start`-before-connect, pure-snapshot, partial-connect-across-pumps, pump-only re-sync, in-flight shutdown; re-sync retiral/replacement (a changed description yields a fresh entry, a removed tool disappears from the next snapshot) |
| `tests/tui_e2e.py` | stale default-model expectation refreshed (`gpt-5 (openai)`) |
| `include/agentc_ext.h` | P4.1: appended provider contribution (`AgcExtProvider`, request-view/auth/model/stream/sink/usage/wire-event structs and their constants) and the `add_provider` host slot after `set_title` |
| `src/ext/registry.c` | P4.1: `ProviderRec` registry, registration validation (name/path/url/callbacks/discover/models/auth, cap 64), the request adapter (view materialization, head sanitize, core auth), the SSE adapter (`g_ext_sink`), and provider lifetime (retire on owner removal, free at shutdown) |
| `src/prov/provider.h`, `src/prov/provider.c` | P4.1: appended row fields `is_ext`/`ext_rec`/`retired`/`auth_headers`; `agentc_provider_all`, `agentc_provider_open_ops`, `agentc_provider_retire_ext`; `registry_reset` returns to the pristine baseline; the open-stream table tracks the row **before** `stream_open` |
| `src/prov/models.c`, `include/agent.h` | P4.1: `AgcModel.flags` + `AGENTC_MODEL_STATIC`, `agentc_model_register_static`, `agentc_model_clear_static`, `agentc_model_is_static`; static and discovered rows share the 256-slot table with a one-time full log |
| `src/core/discover.c` | P4.1: per-row `auth_headers` in discovery (NULL = the built-in Bearer default) |
| `src/core/config.c`, `include/config.h` | P4.1: generic `providers.<id>.base_url` / `api_keys.<id>` entries (user scope only, cap 32, id ≤64); the four named presets still win |
| `src/app/main.c`, `src/app/setup.c`, `src/app/setup.h` | P4.1: composition before onboarding as well as before provider resolution; `seed_presets` + `agentc_provider_all` enumeration in `--list-models`, the onboarding menu and the unknown-provider diagnostic; extension providers appear in the picker |
| `crates/agentc-ext/src/lib.rs` | P4.1: Rust mirrors for the provider structs/constants and `Host::add_provider` |
| `extensions/fake_provider/`, `extensions/manifest.json` | P4.1: the reference provider example (Bearer auth, static model, canned SSE, forbidden-header probe) and its manifest entry |
| P4.1 tests | `tests/ext_test.c` (registry/validation/adapters/caps/static models + harness mode), `tests/agent_loop_test.c` (loop paths), `tests/config_test.c` (generic config), `tests/e2e.sh`/`tests/ext.sh` (the linked fake provider probe) |
| docs | `extensions/README.md`, `.agents/docs/*`, `README.md`, `.agents/AGENTS.md` — extension convention |

`agentc_ext_adopt` is kept as `agentc_ext_adopt(const char *name, int (*entry)(const AgcExtHost*, AgcExt*))`, so the generated registry and tests keep one entry point.

### 11.1 P4.1 — custom providers (delivered, independently verified, committed)

An extension `init` may call the appended host service `add_provider` (guard it
with `AGENTC_EXT_HOST_HAS`; a failure is logged and does not fail `init`). The
contribution is a typed `AgcExtProvider` plus the view/sink family:
`AgcExtRequestView`/`AgcExtMessageView`/`AgcExtBlockView`/`AgcExtToolView` (the
host-materialized request), `AgcExtProviderAuth`, `AgcExtProviderModel`,
`AgcExtStream`, `AgcExtStreamSink`, `AgcExtUsage`, `AgcExtWireEvent`, and the
role/block/stop/discover/auth/usage constants (`include/agentc_ext.h:246–436`).

- **Request view and two-sink contract.** The host builds one allocation per
  build attempt (freed after the call) with the filtered transcript (empty or
  non-serializable assistant turns skipped, `AGENTC_TOOL_HIDDEN` tools skipped,
  a missing schema defaults to `{"type":"object"}`) and calls `build_request`
  with it. The extension writes the head block into `head_out` and the JSON body
  into `body_out` via `host->out_write` (mandatory; `stream_event` is mandatory
  too; `stream_open`/`stream_finish`/`stream_close` are optional, NULL = a
  stateless provider). The core inserts the blank line, sanitizes the head (the
  `'\r'`/`'\n'` → space rewrite, token-safe keys, the forbidden `Host` /
  `Content-Length` / `Transfer-Encoding` trio and any occurrence of the declared
  auth header dropped), adds the auth line, `Content-Length`, retries, timeouts,
  cancellation and `--record`/`--dump-wire` (`registry.c:2153–2320`).
- **Auth and key secrecy.** The core resolves the credential (the same order as
  any provider: flag, OAuth, env keys, `auth.jsonc`, config) and applies it per
  the row's declared `auth.kind`: `NONE`, `BEARER` (a missing header defaults to
  `Authorization`, a missing prefix to `"Bearer "`) or `HEADER` (a token-safe
  header is required and the prefix is prepended verbatim). `QUERY` is reserved
  and rejected at registration. The key is never in the request view and never
  reaches the extension.
- **SSE sink semantics.** The core parses SSE and hands the extension one
  `AgcExtWireEvent` at a time; the extension calls the typed `AgcExtStreamSink`
  (`text`/`thinking`/`tool_start`/`tool_args`/`response_id`/`usage`/`stop`/
  `error`). Each call applies immediately to the core stream state; payloads are
  borrowed for the call (the sink copies them). `stop`/`error` set the stop
  state; after `stream_finish` the built-in resolve/validate applies (a stream
  that never signalled a stop is `stream ended before finish`, otherwise a
  pending stop resolves to `tool_use` when the message carries tool calls, else
  `stop`). Arbitrary framing is deferred: P4.1 is SSE-only.
- **Registration validation and lifetime.** Validation covers `struct_size`
  (including `stream_close`), the name charset/length (≤64), a `/`-leading
  CR/LF/space-free path, an `http(s)` base URL, the mandatory callbacks, the
  discover style, model ids/limits, the auth struct; duplicates against live
  rows (builtins, materialized presets, other extensions) are rejected and the
  registry is capped at **64** providers. Records live in a pointer vector so
  they never move; owner removal marks them retired (`alive=false`,
  `ops.retired=true`) instead of freeing them, lookups skip retired rows, and an
  in-flight handle still resolves to a clean `-ENOSYS` / `provider unloaded`
  error (`registry.c:2075–2687`, `owner_remove` at :2864). A retired row's name
  stays reserved until shutdown (re-registration is refused with `-EEXIST`), so
  a held handle can never be redirected to a new provider.
  `agentc_ext_shutdown` marks every ext row retired, drops the rows from the
  provider registry and frees the records; there is no refcount in P4.1.
- **Discovery and static models.** Discovery reuses the built-in styles
  `DEFAULT`/`ANTHROPIC`/`OLLAMA`/`NONE`; a `CUSTOM` extension parser is deferred.
  The per-row `auth_headers` hook gives discovery the declared header; an empty
  override is authoritative (`AUTH_NONE` sends no auth line), and only rows with
  no hook fall back to the built-in `authorization: Bearer`.
  `add_provider` also registers the optional static model metadata through
  `agentc_model_register_static`; the row is marked `AGENTC_MODEL_STATIC`, so
  `agentc_model_clear_dynamic` keeps it and `agentc_model_clear_static` (called
  when the record is freed) drops it. Static and discovered rows share the
  256-slot table; a full table logs once.
- **Generic config.** User config may carry `providers.<id>.base_url` and
  `api_keys.<id>` for any id (extension providers, user gateways): user scope
  only, at most 32 entries each, ids over 64 bytes ignored; the named
  `providers`/`api_keys` presets are consulted first.
- **Composition order.** By the time provider resolution runs, extensions are
  composed: `register_defaults` → `register_linked` → `apply_config` →
  provisional MCP context → `load_all` → trust → final MCP context/resources →
  policy. P4.1 moved the block above provider resolution **and** above first-run
  onboarding (the `agentc setup` subcommand composes too, with MCP skipped);
  `--list-models`/`--list-extensions` early returns shut the registry down. The
  unknown-provider diagnostic lists the live names, and the onboarding menu
  offers loaded extension providers after the presets.

---

## 12. Known limitations (documented divergence from pi)

- Handlers are synchronous; pi's async/awaited handlers cannot port. I/O-bound
  decisions are not supported.
- `host->on` returns a handle but not a closure; `message_update` carries no
  message id/role (pi carries the partial message).
- `input.action:"handled"` skips the run quietly: no `agent_start`/`agent_end`/
  `agent_settled` and no transcript message, and `agentc_agent_submit` returns 0.
  R2 distinguishes a blocked/failed handler: it sets `last_error` to
  `input blocked by extension`, emits `AGENTC_EV_ERROR`, and makes submit return
  -1, so a front end reports the refusal instead of a silent no-op.
- `tool_call.terminate` exists, but only as the pi batch hint: it is honored on
  a blocked call and ends the submission only when **every call in the assistant
  message** was blocked with it; a mixed or normal call keeps the loop. There is
  no result-level terminate channel — `turn_end`/`agent_before_settle` cannot ask
  to stop (their `continue` asks for one more turn), and `agent_end` keeps
  `stop_reason:"tool_use"` (no `terminated` field).
- Reserved points stay unrealized: `session_info_changed`,
  `session_before_fork`, `session_before_tree`, `session_tree`,
  `cache_warming_decision`, `ui_prompt_start`/`_end` and `user_bash` have no
  point-table row, so `on` rejects them and `emit` is inert. `user_bash`'s
  future site is `rpc_bash` (`src/app/mode_rpc.c`); no core emit exists.
- `input` has no `images` field: `agentc_agent_submit` is text-only in this
  revision (the v1 `images`/`content` fields are reverted). No
  content/structuredContent/image channel on the ABI; tool output is text.
- `model_select`/`thinking_level_select` are **observe-only**: a handler cannot
  select a different model/level (pi's hooks may). The app's `set_thinking` sink
  accepts only `off|low|medium|high`; an unknown name is logged and ignored. The
  points fire on a real change only, and the app installs the
  `set_model`/`set_thinking` sinks in `agentc_mode_setup` so a
  front-end/extension change reaches the mode context.
**P4.2 skills/prompts/themes + MCP content (as delivered).**

- **Startup-only roots.** `resources_discover` runs once at startup and the
  contributed roots are never rescanned, so an extension cannot add a root
  later; a restart is needed. File prompt templates are read once (`main.c`),
  while skill roots are scanned on each context render and theme names resolve
  on demand, so a file added under an existing root can be picked up without a
  restart.
- **No public ABI content contributions.** Prompts, themes and skills are not an
  `AgcExt` contribution kind: the ABI has no `add_prompt`/`add_theme`/
  `add_resource` slot. Extensions reach these stores only through
  `resources_discover` roots; the core `agentc_prompts_register()` API is
  internal-facing and used by file templates and MCP.
- **Synchronous MCP prompt invocation.** `prompts/get` is fetched inline with a
  10 s bound, so a slow prompt blocks the submit/menu path; there is no async
  prompt fetch.
- **MCP resources are text-only.** A `blob` content entry rejects the read
  (`binary resource not supported`); there is no image/embedded-resource channel
  and no notification subscription.
- **No runtime re-discovery / subscription.** MCP list re-sync happens on the
  pump after `list_changed`; there is no `resources/subscribe`/`unsubscribe` and
  no server-initiated content push. Since P4.4b a READY **stdio** server is
  idle-drained on every pump (bounded), so a `list_changed` sent without an
  exchange is picked up on the next pump; HTTP has no idle channel (a response
  is request-scoped), so it is still only seen inside the request that carries
  it.
- **Theme detection is COLORFGBG-only.** `system` reads `$COLORFGBG` and falls
  back to dark; the OSC-11 terminal query is not implemented (planned).
- **File-name validation and caps.** Prompt/template names
  `[a-z0-9._:-]{1,64}`, theme stems `[A-Za-z0-9._-]{1,64}` (`.`/`..` rejected);
  invalid stems are skipped with a warning. 64 file templates and 64 themes per
  scan; 512 prompt registrations (retired records included); 256 KiB
  `/skill:<name>` submit; 8 resource roots per kind and 32 paths per
  `resources_discover` list.
- `session_before_compact` carries `{reason, first_kept, messages}` (no
  `entries`, no `instructions`); it can cancel or supply the summary verbatim,
  but cannot edit the kept prefix.
- `message_end` may only rewrite a restricted assistant shape (see §4.1); a
  replacement cannot change `usage`/`ts`, and a malformed `content` array is
  ignored rather than partially applied. On a failed/aborted turn the content
  may still be replaced, but `stop_reason`/`error` keep the terminal failure,
  so a hook cannot rescue an unserializable turn.
- `turn_end` and `agent_before_settle` share a 32-continuation cap per submit.
- The header patch never applies `Content-Length`/`Transfer-Encoding`/`Host`
  and collapses incoming duplicate header names to their last occurrence.

**P4.1 custom providers (as delivered).**

- SSE-only: the extension receives parsed `AgcExtWireEvent`s and maps them to
  the typed sink; there is no raw-framing/`stream_bytes` hook, so a non-SSE
  dialect cannot use the extension provider path.
- Auth is a core-applied header. `AGENTC_EXT_AUTH_QUERY` (query-parameter auth)
  is reserved and rejected at registration; OAuth providers are not supported.
- No extension-owned discovery parser: `CUSTOM` is not in the discover enum; the
  four styles are `DEFAULT`, `ANTHROPIC`, `OLLAMA`, `NONE`, and discovery uses
  the row's `auth_headers` hook (NULL on builtin rows = `authorization: Bearer`).
- Static provider models carry no cost rates (`agentc_model_cost` returns 0), no
  per-model thinking config and no multimodal request channel; the model `image`
  flag is metadata only and the request view is text/thinking/tool-call.
- Generic config covers `providers.<id>.base_url` and `api_keys.<id>` only;
  `providers.<id>.headers` is not parsed. Custom rows use `api = name` — sharing
  an existing wire api is deferred.
- P4.1 has no refcount: a retired row's record lives until
  `agentc_ext_shutdown`, so an in-flight handle resolves to a clean
  `provider unloaded` error instead of a dangling callback, and a retired
  name stays reserved until then.

**P4.3 async extension tools (as delivered).**

- The host cannot preempt a blocking `start`/`step`/`stop`: timeout and
  cancellation checks happen between callbacks, so a callback that never returns
  stalls the driver exactly as a blocking synchronous `run` does. There is no
  per-step watchdog and no thread/worker contract.
- `host->is_cancelled` answers cancellation only: there is no timeout query and
  no "deadline soon" signal, so a tool cannot learn it is about to be timed out
  (it can only keep its own clock).
- `stop` is teardown-only: it must not register contributions and should not
  block or call back into the host beyond `free`/`log`/`is_cancelled`.
- Unload does not wait for live jobs: each gets `stop(UNLOAD)` immediately and
  the record is released after the last job unlinks. P4.5 completes the dynamic
  half: a dynamic library's handle is closed only after the last job unlinks
  (the close is deferred, never a join/wait), so no code is released while a
  live job can still call it.
- No progress ABI (no partial-result update event), no structured content and no
  per-extension quotas; step output stays text through the job's
  cap/spill/truncation path.

**P4.5 dynamic extension loading (as delivered).**

- **macOS/Windows only; Linux stays static-only.** The freestanding `-nostdlib`
  static ELF has no dynamic loader (`os_ext_available()` false,
  `os_ext_suffix()` NULL, open/sym `-ENOSYS`); there is no custom ELF loader.
- **One fixed directory, no configurable path.** `<config home>/extensions/`
  is scanned once per composition: suffix `.dylib`/`.dll` (case-insensitive),
  stem = extension name `[a-z0-9_.:-]{1,64}`, bytewise order, cap 32 with one
  overflow log; no recursion, no project directory, no `extensions.paths`, no
  runtime kill switch.
- **No ABI change.** A dynamic library exports the canonical
  `agentc_ext_init` and is adopted like a linked extension; `AgcExtInfo.dynamic`
  and the `(dynamic)` marker are host-side only.
- **Trusted native code, no isolation.** No sandbox, no crash isolation, no
  signing/notarization/quarantine policy; POSIX only rejects a
  group/other-writable file and directory (symlinks followed, TOCTOU residual).
  Only place libraries you trust in that directory.
- **Close waits for idle, exactly once.** Registrations removed, `shutdown()`
  returned, no live async job, no extension callback on the stack. A busy close
  is deferred to job unlink/pump/shutdown; a mapping still busy at shutdown is
  kept mapped with a warning (D141-b).
- **Platform runtime is CI-gated.** The macOS `dlopen` from the `-nostdlib`
  binary, a real Windows `LoadLibraryExW`, ARM64/universal runtime and
  Gatekeeper/signing are covered (when green) by the `tests/dyn.sh` CI jobs —
  `make dyn-check` under Wine, `macos-14` and `windows-latest`; a local Linux
  green proves only the fake-loader lifecycle and the cross builds.
- **No per-extension permissions, no runtime load policy, no hot reload.** The
  directory is scanned once at startup; adding or replacing a library needs a
  restart.

**Post-P4 close-out (as delivered).**

- **stdio write residual (Windows and macOS).** `agentc_mcp_write_all`'s first
  `PIPE_BUF` chunk is write-first (D148), so a small request reaches a
  Wine/Windows pipe that `os_poll()` cannot report as writable. But Windows
  anonymous-pipe writes are blocking and `os_poll()` has no write-pipe branch,
  and the macOS parent write end is blocking too (`os_pipe` leaves it so): once
  the first chunk is in, a request pipe that fills can sleep past the deadline,
  and a request larger than one `PIPE_BUF` still times out. The full fix
  belongs in `src/plat/win/sys.c` plus a non-blocking macOS parent write end.

---

## 13. Testing

- `tests/ext_test.c`: registry order (the generic `AgcExt.order` sort plus
  registration ties), `OBSERVE`/`OVERRIDE` dispatch, **chain composition across
  2–3 handlers** (assert B sees A's result and the final value reaches the core),
  chain-veto arg rewrite + block, `first` short-circuit, capability-mismatch
  rejection at `on`, `<0` failure fail-open/closed, `wants` fast path, new
  abi/size rejection, rollback by owner on failed `init`, section ordering,
  merge algebra (`fields` merge and `replace` honored at override points), an
  `OBSERVE` handler returning `1` rejected with a log, `off()` during dispatch
  not affecting the current walk (true snapshot), the dirty flag (public
  `add_tool`/`request_recompose`), unload order (registrations removed before
  `shutdown()`), `set_status` segments, and the `set_model`/`set_thinking` sink
  plumbing.
- `tests/e2e.sh`: `--system --dump-wire` hard-replace check — the dumped request
  carries the supplied system text and no extension core sections.
- `tests/status_test.c` repointed.
- `tests/mcp.sh`: `start` returns before connect; partial connect across pump
  calls; `list_changed` re-sync does not block; tools appear next turn; shutdown
  during in-flight connect; stubborn child.
- **P1-hardening wave adds:** the storage-stability regression (a handed-out
  `AgcTool.ud` stays valid as later tools grow the registry), async owner
  teardown (a torn-down owner's `defer` callbacks and HTTP requests are dropped),
  malformed `fields` results, `set_status` invalidation (version bump; survives
  an external `agentc_status_reset()`), `agentc_ext_register` ABI/size
  validation, `agentc_ext_remove_tool_internal`, `agentc_ext_merge_fields`, chain
  `rc == 1` (handled, chain continues), and `append_entry` object validation.
- **P3 tests (delivered):** `tests/agent_loop_test.c` covers the veto
  rewrite/block, `input` transform/handled/fail-closed, `before_agent_start`
  (`systemPrompt` reaches the request and the extension context; `message`
  logged), `message_end` replace/invalid/tool-call rebuild and ordering before
  the typed event/observer, `tool_result` result shapes (string/text-array/
  null/empty/shape/is_error) and order before append, `turn_end`
  continue/entries/hard, `agent_before_settle` continue/entries and the shared
  settle cap (33 requests for a 32-cap), `agent_settled` once per run,
  `before_provider_headers` flat patch + forbidden-header rejection + payload
  shape, `before_provider_request` replacement (`body_len` follows),
  `REQ_BLOCKED` (one terminal error, no retry, no transport call), and the
  provider observe bridge with response headers. `tests/agent_loop_test.c`
  covers `model_select` (same-provider switch, no-op silence, `wants` fast path,
  effective provider/model change on rebuild) and `thinking_level_select`
  (numeric change, name normalization, clamping, silence); `tests/modes_test.c`
  covers the app sinks (same-provider apply, unknown-provider rejection,
  provider-switch refusal, `set_thinking`). `tests/config_test.c` covers
  `project_trust` yes/no/undecided fallback, CLI-over-hook, `remember`, saved
  verdict after unload, and fail-closed malformed/failed handlers.
  `tests/resources_test.c` covers the path gate (absolute/no-`..`/config vs
  project roots/prefix boundary), rejected paths kept out, and consumed
  `skillPaths`. `tests/compact_test.c` covers `session_before_compact`
  cancel/summary/default and the `session_compact`/`session_compact_failed`
  payloads. `tests/policy_test.c` covers the destructive default (core
  destructive eligible, non-core and MCP destructive hidden, named selected,
  read-only eligible, defaults).
- **R-wave tests (delivered):** `ext_test` adds owner attribution across
  hook/tool/command/defer callbacks, named failure/overrun diagnostics,
  unknown-point rejection, the `wants` counter, 3-strike overrun containment for
  observe and override (`overrun_observe_*`, `overrun_override_*`), the emit
  recursion cap (32; override blocked, observe empty), `set_status` (owner,key)
  collisions and the 8-key table, `AGENTC_EXT_FIELD_OK`/`HOST_HAS` and
  `required_host_size`, append-entry caps, HTTP hygiene (NULL body, forbidden or
  malformed headers still run the request), the `agentc_ext_pump` re-entrancy
  guard, and the core `agentc_ext_append_entry` sink. `agent_loop_test` adds the
  per-turn auto-prompt rebuild (`refresh_*`), a blocked `input` returning -1
  with an error event, `message_end` on failed/aborted turns (content replaced,
  terminal kept, not persistable), `turn_end.continue` on a truncation turn,
  `agentc_http_set_cancel` (pre-connect and mid-stream `-ECANCELED`,
  `"cancelled"`), and forced `\uXXXX` header-JSON escaping. `mcp_test` adds
  `pending_start`/`pending_settled`, the per-server/total caps, the frozen-pile
  frozen-cap/churn bounds, long-name handling, description truncation and cancel.
  `policy_test` adds `--tools`/`default_tools` dedupe, the MCP-disabled deferral
  diagnostic, and `agentc_ext_describe`; `modes_test` adds the RPC idle pump.
- **P4.4 closed the still-untested list:** the failed-`init` rollback of
  commands/sections/status/pumps, `host->request_recompose`, equal-priority hook
  ties and the app-level late `mcp__` recompose selection now have tests (the
  P4.4 tests bullet below).
- **P4.1 tests (delivered; clean-tree run and independent audit recorded in
  `EXTENSION-HANDOFF.md` §8).** `tests/ext_test.c` covers the
  provider registry and adapters: registration/validation logs (struct size,
  name, path, URL, callbacks, discover style, model ids, auth, QUERY), duplicate
  rejection (fixture, builtin, preset), the auth styles (HEADER, BEARER default
  prefix, NONE stateless) and the `auth_headers` output, `agentc_provider_all`
  count-first with `openai`'s two rows, the view/body build with head
  sanitization (forbidden trio and the declared auth header dropped, core auth
  added), the sink mapping (text/thinking/tool/usage/response id) with
  `finish`/`close`/reopen, retiral + clean dead-row errors + memory baseline,
  the 64-provider cap, and static models (register/update/survive
  `clear_dynamic`/`clear_static`/256-slot cap). Harness mode (built by
  `make ext-pipeline`, asserted by `tests/ext.sh`) checks the linked
  `fake_provider`: registration, static model, sanitized request, canned SSE and
  the `--list-models` line. `tests/agent_loop_test.c` drives the fixture
  provider through the replay transport: view filters, a tool-call round-trip
  with core auth and sanitized headers, ordered stream deltas/usage, an error
  turn, retry reopening the stream, cancel closing it, an unload mid-test
  failing cleanly, HEADER-auth discovery, and provider enumeration.
  `tests/config_test.c` covers generic `providers.<id>`/`api_keys.<id>` entries
  (named presets win, project scope ignored, caps 32/64, object-valued keys
  skipped). `tests/e2e.sh` probes `--provider fake --list-models --offline`
  (skipped when the binary links no extension); goldens were regenerated and the
  Rust crate mirrors the new structs. The clean-tree gate run and independent
  audit are recorded in `EXTENSION-HANDOFF.md` §8.
- **P4.2 tests (delivered and verified; see `EXTENSION-HANDOFF.md` §8).**
  `tests/themes_test.c` (new) covers base/precedence
  (built-in → legacy `theme.jsonc` → named file, slot by slot), the COLORFGBG
  `system` cases (dark/light/ANSI-light/bogus/unset), unknown names keeping the
  theme, path validation (`""`, `.`, `..`, separators, spaces, over-long,
  missing), the resources_discover root override and the 64-entry scan cap.
  `tests/mcp_test.c` adds capabilities parsing (absent/malformed/empty →
  tools-only, per-kind bits + `listChanged`), `prompts/list` parsing (fields,
  args, long-name skip/description truncation, RPC error), `prompts/get`
  parsing (text join, empty, non-text skip, over-cap and separator accounting),
  the prompt registry commit (add/update/recommit/retire/dedup/frozen cap), the
  resource/template parsers and read parser (text join, blob rejection,
  over-cap) and the resource/table registries. `tests/mcp.sh` adds the live
  stdio capability variants: prompts-only, tools-only (a `prompts/list_changed`
  is ignored and no `prompts/list` is sent), resources-only (no `tools/list` or
  `prompts/list`), an all-kinds server asserting prompts are fetched before
  resources, and the prompt/resource tool labels (read text/blob/big/fail/
  unknown, list one/all/unknown, templates, list_changed, shutdown, memory
  baseline). `tests/tui_test.c` adds `/theme <name>`/unknown notices, the
  `/skill:` body/frontmatter/unknown/over-cap paths and the prompt-command
  paths (file expansion, one submit, registered fixture, menu entry, built-in
  shadowing). `tests/modes_test.c` adds the RPC `prompt_template`
  success/unknown/missing cases. `tests/resources_test.c` adds the three root
  stores (dedup/cap), `skill_body.read/unknown`, theme path resolution, the
  template `argument-hint` and extra root, and the prompt registry
  (register/re-register/remove/source-clear/cap/expand). `tests/policy_test.c`
  adds the exact-name deferral of the three resource tools and the rejection of
  a typo/`mcp_`-prefix name. Goldens (`resources_test`, `mcp_test`,
  `modes_test`, `policy_test`, `tui_test`) were regenerated.
- **P4.3 tests (delivered and committed; the gate run and independent
  verification are recorded in `EXTENSION-HANDOFF.md` §8).** `tests/ext_test.c` covers the
  registration gates (complete trio, partial-trio rejection, `run`-wins
  ignoring the trio and still executing, a baseline-size struct with `run`
  staying sync, the tiny-struct rejection, the 1 800 000 ms clamp and negative
  normalization), incremental step output, completion in `start`, result error,
  fatal/timeout/cancel/unload with exactly one `stop` and the right reason,
  owner/log attribution in `start`/`step`/`stop`, the still-valid token inside
  `stop`, retire/reuse/dirty behavior, the allocator baseline, and the `D-A9`
  pump. `tests/jobs_test.c` covers the `AgcJob.cleanup` hook on
  fatal/timeout/cancel/release with `job->priv` still valid and exactly one
  call. `tests/agent_loop_test.c` drives a five-tool async fixture through the
  replay loop: incremental transcript text and events, a timeout tool-result
  error followed by another turn, abort inside a step (one `stop(CANCELLED)`,
  `last_error = "aborted"`, no retry), and two async tools interleaving one
  step per driver tick (`a0b0a1b1`) without wall-clock assumptions.
  `tests/ext.sh` asserts the linked `async_demo` through its `async-demo-debug`
  test command (finish, `fail_at` tool-result error, fatal, timeout, cancel)
  plus the direct-adapter poll branch; the `ext_test`, `jobs_test` and
  `agent_loop_test` goldens were regenerated and the Rust crate mirrors the
  appended fields.
- **P4.4 tests (delivered in the working tree; the gate run is open until
  P4.4c).** `tests/modes_test.c` covers the persistence owner and the switch: a
  prompt after `set_model` persists to the live file; a prompt after
  `new_session` lands in the new file (the old file stays untouched) and
  `host->session_file()`/`session_id` follow the new session; a cancelling
  `session_before_switch` leaves the session/file/transcript intact and answers
  `session switch cancelled by extension`; an allowed switch emits exactly one
  `session_start{reason:"new"}` carrying the old file as
  `previous_session_file`. `tests/agent_loop_test.c` covers terminate
  all/mixed/continue/cancel (the all-terminate batch settles with
  `agent_end.stop_reason == "tool_use"`; a mixed batch runs another turn;
  `turn_end.continue` still wins; abort wins over terminate). `tests/ext_test.c`
  covers the `session_before_switch` caps/policy, the `terminate` parse gate
  (`terminate` without `block` is dropped with a log), the failed-init rollback
  of commands/sections/status/pumps, `host->request_recompose`, equal-priority
  hook ties, the late `mcp__` recompose selection, and the in-flight extension
  HTTP cancel (`-ECANCELED` with the partial body; the fixture warms the
  provider/net-mock caches before the allocator baseline). `tests/mcp_test.c`
  covers the retry delay table (base << min(count,6), cap, env override and `0`
  disabling), a FAILED HTTP server that re-arms and recovers (non-pending while
  it waits), the `mcp_servers_change` payload (records created as `connecting`,
  then `ready`+caps, then `failed`) and the `--live-idle` mode; `tests/mcp.sh`
  drives `--live-idle` against the stdio mock's idle timer
  (`MCP_MOCK_IDLE_NOTIFY_MS`) and runs the existing `--live-p2` scenario with
  `AGENTC_MCP_RETRY_MS=0`.
- **P4.5 tests (delivered in the working tree; the recorded run is in
  `EXTENSION-HANDOFF.md` §8).** `tests/dyn_test.c` (+ golden
  `tests/data/dyn_test.expected`, 79 checks) drives the scan → open → adopt →
  register → load → unload/shutdown lifecycle against a fake injectable loader:
  suffix case-insensitivity, stem validation, sort order, the 32-cap overflow
  log, no recursion, missing-dir, `path_ok` modes (regular/missing/dir + POSIX
  modes), incomplete-loader-table rejection (Finding 2), duplicate skip before
  open, open/sym/ABI/name failures isolated with rollback + close, D140
  stem⇔`out.name`, adopt owner rollback, load order + `extensions.disabled`,
  idle close exactly once, live-job deferral until last unlink (`stop(UNLOAD)`
  first), close from a defer callback at the pump epilogue, the self-unloading
  status provider (Finding 1: no close inside the provider; close at the pump
  epilogue), shutdown close-when-idle vs keep-mapped-when-busy, no
  use-after-close and the allocator baseline. The Windows run asserts the same
  golden output (POSIX-only cases normalize through `os_platform()`), so the
  suite runs natively and under Wine. `tests/dyn.sh` is the real-loader gate:
  macOS `make ext-dylib` + `dlopen`, Windows/`--wine` `make win-ext-dylib` +
  `LoadLibraryExW` (dynamic row loads, `hello_echo` registered vs a bogus-name
  control, isolation of a bad-image/no-symbol/duplicate/invalid-stem sibling,
  platform error text), while plain Linux builds `build/extensions/hello.so`
  as a PIC/ABI check and skips the runtime. CI runs `make dyn-check` in the
  `linux` job (before the golden Wine suite), `sh tests/dyn.sh` on `macos-14`
  and `bash tests/dyn.sh --windows` on `windows-latest`.
- `make check`, `tests/e2e.sh`, `tests/mcp.sh`, `tests/ext.sh`: reported green
  on the R1–R3b tree; the final P3 gate run, the R1–R3b run and the P4.1 and
  P4.2 runs are recorded in `EXTENSION-HANDOFF.md` §8, as is the P4.3 run
  (including the F1/F2 memory-safety fixes). `make check` includes the extension
  pipeline (`check-ext`: compile, link and run every manifest example); CI runs
  plain `make`, `make ext-pipeline` and `make check` with apt clang/make/python3,
  and runs `tests/ext.sh` under `nix develop` (cargo from the dev shell, not
  apt) — the C and Rust examples are covered on every push, not only at release,
  and `check-ext` skips cleanly without python3 or on a cross build.

## 14. Phasing

- **P0/P1 — ABI + registry (delivered).** `include/agentc_ext.h`, `include/ext.h`,
  `src/ext/registry.c`, `src/ext/registry_int.h`, `src/ext/linked.c`; the hook bus;
  `builtin-tools`, `builtin-context`, `mcp` (still synchronous), linked
  extensions, sections, status types; the one-word convention. `status_test`,
  `ext_test`, the Makefile symbols, the Rust crate, both examples and the live
  docs were ported. O1 and O2 resolved (see §0); prompt/request goldens
  regenerated. The §8 trust reordering and the §10
  destructive-extension-tools-hidden rule were deferred to P3 and are delivered
  (§14).
- **P2 — async MCP + recompose (delivered).** `agentc_mcp_start`/`_pump`,
  pure `agentc_mcp_tools` snapshot, pump-owned re-sync, `mcp_servers_change`, and
  the app-installed `agentc_agent_set_recompose` hook applied at the exact turn
  boundary (§5.1). Acceptance met: an unreachable MCP server does not delay the
  first prompt; late tools appear at the next turn boundary. Stdio is a true
  non-blocking state machine; HTTP advances one bounded request per pump step
  because the wire layer is not resumable (§7). The **P2 fix wave** completed the
  promise: retiral/replacement reaches the registry
  (`agentc_ext_remove_tool_internal`; publish marks an entry registered only on a
  successful add), the pump advances exactly one server per call (round-robin),
  an in-flight `list_changed` is latched, strict `--tools` defers
  not-yet-connected `mcp__` names, `extensions.disabled` covers linked
  extensions (applied after `register_linked`), and the build compiles/links the
  whole manifest (every C entry; Rust from the same manifest).
- **P3 — hooks (delivered).** The `AgcToolVetoDecision` consumer
  (rewritten `input` threaded, `reason` in the diagnostic); core emit sites with
  `wants` guards; overrides for
  `tool_call`/`tool_result`/`message_end`/`input`/`before_agent_start`/provider
  points; the `tool_result`/`message_end` moves before append/observer; settle
  continuation (shared 32-cap); the §8 trust reordering + `resources_discover`
  gating; the §10 destructive-tool default with D52 resolved; wave 4
  `model_select`/`thinking_level_select` emit sites and the app model/thinking
  sinks.
- **Post-review fix waves R1–R3b (delivered).** Runtime wiring (one
  `agentc_mode_pump` duty, the 400 ms early-exit startup pump, the per-turn auto
  prompt rebuild), CI and build-fragment correctness (real generated target,
  atomic writes, `check-ext` under `make check`); registry/core fixes (owner
  restore and named diagnostics, 3-strike overrun containment, unknown-point
  rejection, `wants` counter, emit recursion cap, keyed `set_status`, ABI growth
  gates, fail-closed `resources_discover`, terminal-preserving `message_end`,
  `turn_end.continue` on non-hard turns, `select_tools` dedupe, the
  blocked-`input` error event, `agentc_http_set_cancel`, ASCII header JSON); and
  UX/observability/build/MCP-memory fixes (`--list-extensions`, the startup
  summary, the D52 help note, the MCP-disabled deferral diagnostic, the TUI UI
  sink, bounded MCP entries with a hard frozen cap, Windows `%windir%`
  fallback, bridge retirement, `agentc_ext_append_entry`, `json_fields_apply`,
  bounded `load_all` and live shutdown iteration).
- **P4.1 — typed custom providers (delivered, independently verified and
  committed).** The appended `add_provider` host slot, the typed view/sink ABI,
  registry records with retirement, core-applied auth, static models, generic
  config and discovery auth (behavior in §11.1; design record
  `.agents/plans/P4.1-PROVIDERS.md`); commit `f5de256` plus its docs commit.
- **P4.2 — skills/prompts/themes + MCP prompts/resources (delivered and
  independently verified; committed).** One invocable-prompt registry (§6.2); file
  templates and `/skill:<name>` in the TUI and RPC (`prompt_template`); named
  themes with COLORFGBG `system` and `/theme <name>`; `resources_discover`
  consumes `skillPaths`/`promptPaths`/`themePaths`; MCP capability negotiation
  and per-kind list sync; MCP prompts as registry commands with a bounded
  synchronous `prompts/get`; resources as three lazily published read-only
  tools. Three sub-waves: P4.2a (resource roots + prompt registry + themes),
  P4.2b1 (MCP prompts), P4.2b2 (MCP resources).
- **P4.3 — async extension tools (delivered, independently verified; committed
  with the feature/docs commits).** The appended `start`/`step`/`stop` trio, the
  mandatory `stop` reasons, extension-owned state, per-step staging output, the
  internal `AgcJob.cleanup` hook, retire-not-free + `stop(UNLOAD)`, the signal
  window, the per-step pump, the `run`-wins rule, the 30-min async clamp and the
  Rust mirror (§3.7; D110–D123; deviations recorded in
  `.agents/plans/P4.3-ASYNC-TOOLS.md`). Sub-waves: P4.3a (ABI/driver/adapter/
  retire/Rust + `ext_test`/`jobs_test`) and P4.3b (`async_demo` + `ext.sh` +
  `agent_loop_test` + goldens); P4.3c (docs + verification + commit)
  completed.
- **P4.4 — reserved points and completion (delivered, independently verified;
  committed with the feature/docs commits).** P4.4a: the persistence
  owner fix (mode-ctx observer, `agentc_mode_rebind_session`,
  `agentc_agent_transport` preservation), `tool_call.terminate` (all-terminate
  batch, cancel/continue precedence) and `session_before_switch` +
  `session_start{reason:"new"}`. P4.4b: in-flight extension HTTP cancel, MCP
  idle stdio drain, the `mcp_servers_change` status/caps payload, MCP
  reconnect/backoff, and the tests-only completions (failed-init rollback,
  `request_recompose`, equal-priority ties, late `mcp__` recompose). Delivered
  deviations are D134/D135; design/delivery record
  `.agents/plans/P4.4-COMPLETION.md`. Still deferred: prompts reload, OSC-11
  theme detection, resumable extension HTTP, image/content, `user_bash` emit,
  per-step watchdog and the progress ABI (demand-gated).
- **P4.5 — dynamic extension loading (delivered; platform runtime gated on
  CI).** Optional on macOS/Windows while Linux stays static-only: the `plat`
  loader surface (`os_ext_open/sym/close/available/suffix`), the injectable
  `AgcExtDynLoader` seam and scan/path policy in `src/ext/dynlib.{c,int.h}`,
  `agentc_ext_register_dynamic()` in the three composition sites,
  `AgcExtInfo.dynamic`/`(dynamic)`, the `DynRec` idle-only exactly-once close
  discipline (deferred to job unlink/pump/shutdown; keep-mapped warning when
  busy at shutdown), the adopt entry owner-tagging/rollback and `load_all`
  re-entrant-unload guard, and the Finding-1 status trampoline + Finding-2
  loader-table validation. P4.5b adds `make ext-dylib`/`win-ext-dylib`,
  `tests/dyn.sh`, `make dyn-check` and the CI runtime jobs. See
  `.agents/plans/P4.5-DYNAMIC.md` and D138–D147.

## 15. Acceptance

- One header, one `abi_version`, no compat range; `agentc_plugin.h` gone and
  nothing includes it.
- `main.c` has no builtin/extension/mcp merge and no hardcoded prompt sections.
- `builtin-tools` and `mcp` are ordinary extensions; disabling `mcp` removes it.
- Override failures are fail-closed; observe overruns fail open, and both
  observe and override handlers are disabled after three consecutive budget
  overruns (a clean call resets the counter). The overrunning occurrence follows
  the point's failure mode (blocked for override, ignored for observe).
- `on` rejects a point outside the §4.1 table; emit recursion is capped at 32;
  `set_status` is keyed by (owner, key) with 8 live keys.
- Examples + Rust crate build against `AgcExt`; `make check`, `tests/e2e.sh`,
  `tests/mcp.sh`, `tests/ext.sh` green. (An unreachable MCP server delaying the
  first prompt is a P2 acceptance item, not P1.)
- P3 acceptance: a loaded `project_trust` handler decides before the saved
  verdict (fail-closed on failure/malformed) and `resources_discover` paths are
  root-gated; extension/MCP destructive tools are hidden from the default
  selection unless named; `tool_result`/`message_end` replacements reach the
  transcript before append/observer; a blocked request override is terminal
  (`REQ_BLOCKED`); `model_select`/`thinking_level_select` observe a real
  model/thinking change.
- P4.1 acceptance (delivered and independently verified; one major discovery
  auth fix and five minors landed before freeze): an extension registered
  during `init` contributes a provider visible to `--provider`, `--list-models`,
  onboarding and the unknown-provider diagnostic; the core applies the resolved
  key as the declared auth header and never puts it in the request view; the
  extension's head is sanitized (forbidden trio and its own auth line dropped)
  and its SSE mapping reaches the transcript with usage and stop; a retired row
  is skipped by lookups but still yields `provider unloaded` through an
  in-flight handle; static models survive a discovery refresh and are dropped
  when the record is freed.
- P4.2 acceptance (delivered and committed): file templates, `/skill:` and
  `/theme <name>` resolve through the documented roots and precedence; the
  config `theme` key is applied before the first frame; `resources_discover`
  consumes all three path lists under the root/path caps; MCP capability gating
  fetches only advertised lists and ignores an unadvertised `list_changed`;
  prompts appear in the one registry as `mcp__…` commands and expand through a
  bounded synchronous `prompts/get`; the three resource tools appear only after
  a resource-capable list sync and `--tools` defers them by exact name;
  resources are text-only (`blob` rejected).
- P4.3 acceptance (delivered and committed): an async-only tool
  registers and runs through the job driver with `run == NULL`; a partial trio
  is rejected and `run` wins when both are declared; `stop` runs exactly once
  per successful `start` with `FINISHED`/`ERROR`/`TIMEOUT`/`CANCELLED`/`UNLOAD`;
  step output is incremental and capped like a sync result; a fatal step
  replaces staged output; a timeout is a tool-result error and the loop runs
  another turn; abort delivers exactly one `stop(CANCELLED)` and ends the run
  with `last_error = "aborted"`; two async tools interleave one step per driver
  tick; unload/remove/shutdown mid-job synthesizes the unloaded result and the
  retired record is freed after the last job unlinks; async `timeout_ms` clamps
  at 1 800 000 ms.
- P4.4 acceptance (delivered, independently verified; the audit's fail-closed
  fix and caps-name alignment are D137): a prompt after RPC `set_model`/`new_session` persists to the
  live session file and `host->session_file()` follows it; a cancelling
  `session_before_switch` touches nothing and returns the
  `session switch cancelled by extension` error, while an allowed switch emits
  one `session_start{reason:"new", previous_session_file}`; an all-blocked
  terminate batch settles with `agent_end.stop_reason:"tool_use"` while a mixed
  batch and an explicit `turn_end.continue` keep the loop and abort wins; MCP
  `mcp_servers_change` carries `connecting`/`ready`/`failed` with caps after
  initialize, and a FAILED server re-arms behind the backoff (or stays terminal
  with `AGENTC_MCP_RETRY_MS=0`) without inflating the startup pending budget; an
  idle stdio `list_changed` is honored on the next pump; an in-flight extension
  HTTP request cancelled with `http_cancel` gets `-ECANCELED` and its partial
  body.
- P4.5 acceptance (P4.5a verified locally with the fake loader; P4.5b's
  platform-runtime half is gated on the CI jobs): a valid `.dylib`/`.dll` in
  `<config home>/extensions/` whose stem is the extension name loads through
  `agentc_ext_init` and appears as `(dynamic)`; a duplicate name is skipped
  before the library is opened and invalid/unreadable/bad-image/no-symbol
  candidates are logged and skipped while a good sibling still loads; the
  Linux static build is a no-op (`os_ext_available()` false); an idle dynamic
  extension is closed exactly once on unload, a busy one only after its last
  async job unlinks, and a mapping still busy at shutdown stays mapped with a
  warning; `agentc_ext_adopt` rolls back registrations made by a refused entry;
  an incomplete injected loader table is refused; a status provider that
  unloads its own library mid-snapshot is not closed under its own call.

## 16. Open decisions — resolved

1. **`--system` and sections** (§6.2): **resolved = hard-replace.** `--system`
   replaces the whole render (matching pi's `forceSystemPrompt`); extension
   sections are skipped. `--append-system-prompt` does not exist and is not added.
2. **Composition order**: **resolved = defaults-first.** Init order is
   `builtin-tools`, `builtin-context`, `mcp`, then linked extensions, sorted by
   `AgcExt.order` (lower first) with registration order as the tiebreak. Because
   P2 moved MCP tool publication into the registry pump, the live tools table at
   startup is `builtins -> linked` and MCP appends on connect — the historical
   `builtins -> plugins -> mcp` registration order is not the tool order. No
   existing golden encoded either order (the suite never links MCP and a linked
   extension at once), so none needed regenerating.
3. **Strict `--tools` and MCP**: **resolved = defer `mcp__` names.** A name
   beginning `mcp__` that is not present at startup is deferred, not a fatal
   exit-2 error, and is selected when the server connects (the turn-boundary
   recompose re-runs the same `--tools` policy). A missing non-MCP name still
   exits 2, so a typo in a builtin or linked name is still caught at startup.
   P4.2 extends the same rule to the three exact resource-tool names
   (`mcp_list_resources`, `mcp_list_resource_templates`, `mcp_read_resource`),
   which are published only at the first resource-capable list sync; the prefix
   is deliberately not broadened, so a typo in any other name still exits 2.
4. **Destructive-tool default (D52)**: **resolved = MCP schema default.**
   `readOnlyHint:true` wins; otherwise `destructiveHint` defaults to **true**,
   matching the MCP schema. Core builtins carry `AGENTC_TOOL_CORE` and stay
   eligible in the "all" selection; a non-core (extension or MCP) destructive
   tool is hidden unless named in `--tools`/`default_tools`, with one aggregate
   diagnostic per selection. The strict `--tools` `mcp__` deferral still applies
   (a named destructive MCP tool is selected when its server connects).
5. **P4.1 custom providers**: **resolved.** Cross the ABI as a
   host-materialized request view and two opaque sinks; core-applied auth with
   the key never in the view; provider rows frozen until shutdown and retired,
   not freed, on unload; a retired name stays reserved until shutdown;
   duplicate names rejected and the registry capped at 64;
   SSE only (typed wire events); `build_request`/`stream_event` mandatory,
   lifecycle callbacks optional; static model metadata marked and shared in the
   256-slot table; generic `providers.<id>`/`api_keys.<id>` config; existing
   discovery styles with a per-row auth hook; custom rows use `api = name`.
   Query auth, a custom discovery parser, cost rates, per-model thinking config,
   multimodal, OAuth providers and `providers.<id>.headers` remain deferred.
   See `.agents/plans/P4.1-PROVIDERS.md` and D81–D96.
6. **P4.2 skills/prompts/themes + MCP content**: **resolved = delivered as
   §6.2/§7.** No new ABI surface (extensions reach these stores only through
   `resources_discover` roots); discovery is startup-only; TUI dispatch
   precedence is built-in > prompt > extension command; theme precedence is
   built-in name → legacy `theme.jsonc` base → named file base with config slots
   then named slots; `agentc_tui_run` takes the config theme name plus the app's
   resolved trust verdict; `system` uses COLORFGBG only; MCP prompts are
   registry commands with a bounded synchronous `prompts/get`; MCP resources are
   three lazily published read-only tools with exact-name policy deferral;
   capability gating and per-kind `list_changed` latches drive the list sync;
   resources are text-only; prompt/theme file names are validated and capped.
   See D97–D108.
7. **P4.3 async extension tools**: **resolved = delivered as §3.7.** The trio is
   appended to `AgcExtTool` (no new contribution struct and no new host
   service), `run` wins, `stop` is mandatory and exactly once with the five
   reasons, state is extension-owned, start/step output goes through the job
   staging path, the signal token spans `start`..`stop`, the adapter pumps once
   per running step, async tools are not implicitly sequential, the Rust mirror
   and the async timeout clamp landed, and a record with live jobs is retired
   and released only after the last job unlinks (`free_pending` when shutdown
   raced a live job). `AgcJob.cleanup` is the internal driver hook. The
   delivered deviations are D122/D123. See `.agents/plans/P4.3-ASYNC-TOOLS.md`.
8. **P4.4 reserved points + completion**: **resolved = delivered as
   `.agents/plans/P4.4-COMPLETION.md`.** `tool_call.terminate` is the pi batch
   hint (blocked calls only, whole-message agreement, cancel wins,
   `turn_end.continue` still re-enters once, `agent_end` stays
   `stop_reason:"tool_use"`); `session_before_switch`
   (`override/first/replace/fail-closed`, payload `{"reason":"new"}`, result
   `{"cancel":true}`) gates the RPC `new_session` switch and
   `session_start{reason:"new"}` follows it; `user_bash`, prompts reload,
   OSC-11, resumable extension HTTP, image/content, per-step watchdog and the
   progress ABI stay deferred; the reserved names have no point-table row (`on`
   rejects, `emit` inert). MCP idle stdio drain, reconnect/backoff and the
   `mcp_servers_change` status/caps payload are delivered. See D125–D136.
9. **P4.5 dynamic extension loading**: **resolved = delivered on macOS/Windows,
   Linux static-only.** One fixed config-home directory; loader suffix matching
   (case-insensitive); stem validation `[a-z0-9_.:-]{1,64}`; bytewise order and
   cap 32; POSIX group/other-writable rejection; `RTLD_NOW|RTLD_LOCAL` on macOS
   and `LoadLibraryExW` with the DLL-dir + System32 search flags on Windows
   (fail closed); idle-only exactly-once close with the shutdown keep-mapped
   fallback; no ABI change. Platform runtime claims are gated on the CI jobs;
   the Linux build remains without a dynamic loader. See
   `.agents/plans/P4.5-DYNAMIC.md` and D138–D147.

---

## Appendix — review dispositions

From the three fresh-eyes reviews; each was accepted unless noted.

**Blockers fixed:** `out_write` missing; opaque `AgcExtToolCall` with no arg/cancel
access; no async-job output channel (removed async from ABI); custom-provider
vtable unusable (removed `add_provider`); no unregister/owner (handle + owner
tagging); `request_recompose` race via `pump` mid-batch (apply after jobs);
`tool_result`/`message_end` overrides after persistence (moved before
append/observer); observe/override overrun fail-open (fail-closed for override).

**Majors fixed:** `--no-mcp`/`--no-tools`/strict `--tools` threading;
`agentc_ext_tools` signature; `status.h`/`status_test.c`/`registry_int.h` missing
from migration; Makefile symbol lines; `gen-exts.py` table shape;
`agentc_ext_adopt` story; Rust crate rewrite scope; flag-value/`THREADSAFE`
note; `wants` fast path; `tool_execution_update` removed; project_trust ordering;
resources trust gating; header injection/Content-Length; destructive-tool
default; `append_entry` validation; emit return for decision points; chain vs
first for `tool_call`; merge algebra; capability gating; handler error protocol;
`agentc_mcp_tools` blocking re-sync; MCP print-mode acceptance.

**Removed from active points:** `session_before_fork`, `model_select`,
`thinking_level_select` (to reserved until emit sites exist)
— *and* `thinking_level_select`/`model_select` sites were then specified in §4.7;
they stay active with new sites. (The reviews disagreed; resolved in favour of
adding the sites.)

**Nits fixed:** line references (`agent.c:686–693`/`:721`, `Makefile:74`,
`registry.c:242–276`, `mcp.c:550/1180`, `mcp.c:1131/1229→1290`); `SETTLE` wording;
`compact_now` location; "every struct carries abi_version" corrected.
