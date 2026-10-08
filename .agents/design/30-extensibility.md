# 30 — Extensibility

Four layers, one boundary rule: **JSON at external boundaries, typed structs
internally**. Anything that crosses a process or ABI boundary speaks JSON, so it
stays stable, debuggable and language-agnostic; everything inside the process
uses enums and pointers checked by the C compiler.

The layers are: the process-level MCP client (§1), the declarative config and
markdown layer (§4), the native C ABI (§2), and the extension registry that
composes default, linked and dynamic extensions (§3). The design inspiration is
[pi](https://github.com/earendil-works/pi); where pi's extension concepts map
onto this surface they are noted inline as inspiration, not as dependencies.

---

## 1. MCP client

`src/ext/mcp.c` is the Model Context Protocol client. It is a library; the
built-in `mcp` extension is just a consumer, so a user extension can replace it.
The public contract is `include/mcp.h`.

### 1.1 Transports and negotiation

- **stdio:** `os_spawn` a child and speak JSON-RPC 2.0 over line-delimited
  messages on stdin/stdout. Servers are spawned with the process environment
  plus the configured `env` map.
- **Streamable HTTP:** `wire/http.c` plus SSE parsed from the POST response. The
  optional GET stream is not implemented.
- Protocol negotiation requests `2024-11-05` (the version every server
  implements) and accepts what the server answers. Every connect is bounded by
  `AGENTC_MCP_CONNECT_TIMEOUT_MS` (10 000 ms).
- The stdio line cap is `AGENTC_LIMIT_MCP_LINE_BYTES` (1 MiB); the HTTP response
  body cap is `AGENTC_LIMIT_MCP_BODY_BYTES` (8 MiB). `tools/list` is paginated
  and bounded by `AGENTC_LIMIT_MCP_PAGES` (100 pages).

### 1.2 Configuration

`~/.config/agentc/mcp.jsonc` and `<cwd>/.agentc/mcp.jsonc` (the project file
loads only when trusted):

```jsonc
{
  "servers": {
    "files": {
      "command": "npx",
      "args": ["-y", "@scope/server"],
      "env": { "SOME_TOKEN": "${TOKEN}" }
    },
    "remote": {
      "url": "https://…",
      "headers": { "Authorization": "Bearer ${TOKEN}" }
    }
  }
}
```

`${ENV}` expands from the environment; `enabled` and `timeout_ms` are per
server. Every enabled server is exposed; there is no per-server tool filter.

### 1.3 Lifecycle

`agentc_mcp_start(cwd, trusted)` reads the config and creates the server records
without connecting. The `mcp` extension creates them on its first registry pump
after trust settles, and `agentc_mcp_pump()` advances **exactly one server step
per call, round-robin**, so one slow server cannot starve the others. The app
runs a bounded startup pump (400 ms) before composing the tool table and exits
early once `agentc_mcp_pending()` is false (every record READY/FAILED, or no
record exists). `agentc_mcp_server_count()` is the configured-server count;
`agentc_mcp_pending()` is the readiness predicate. A FAILED server is not
pending while it waits, so the startup budget is unaffected. `agentc_mcp_tools()`
is a pure snapshot of what is connected now; its borrowed views stay valid until
the next `agentc_mcp_start()`/`agentc_mcp_shutdown()`. Shutdown closes stdin,
then `SIGTERM` then `SIGKILL`.

The app calls `agentc_mcp_set_context(cwd, trusted)` twice: a provisional
(`false`) value before `agentc_ext_load_all()`, then the final value after trust
is resolved and before the first registry pump.

### 1.4 Capabilities

The initialize response's `result.capabilities` decides which lists the connect
step fetches, in this order: `tools`, `prompts`, `resources` and resource
templates. The `resources` capability covers both `resources/list` and
`resources/templates/list`. Missing, malformed or present-but-empty capabilities
fall back to **tools-only**. A `notifications/<kind>/list_changed` is honored
only while that kind's base capability is advertised; one that arrives during an
in-flight sync is latched into a follow-up fetch. `mcp_servers_change` publishes
each server as `connecting`/`ready`/`failed`, with its advertised `caps` after
initialize, and is emitted at record creation, on READY and on FAILED.

### 1.5 Tools

Each tool is exposed as `mcp__<server>__<tool>` with the server's description
and input schema passed through. Structured content is kept; `isError` maps to
the tool result flag. `readOnlyHint:true` wins; otherwise a missing
`destructiveHint` means destructive (explicit `false` is additive), matching the
MCP schema defaults. Every MCP tool runs as a job with a timeout (`timeout_ms`,
default 60 s) and a cancel handle. A destructive MCP tool is hidden from the
default selection unless named in `--tools`/`default_tools`. Newly discovered
tools are published to the extension registry and applied at the next
turn-boundary recompose; on a re-sync, a retired or replaced tool is withdrawn
through `agentc_ext_remove_tool_internal(name)` (which never frees data an
in-flight transcript may still reference).

### 1.6 Prompts

A committed `prompts/list` registers each prompt in the one invocable-prompt
store (`src/core/prompts.h`) as `mcp__<server>__<prompt>` (source `"mcp"`,
hash-deduplicated name). It is invocable as `/name` in the TUI and through the
RPC `prompt_template` command. Invocation parses positional/named arguments
against the listed arguments (≤16 args, ≤64-byte names, required enforced) and
performs one bounded synchronous `prompts/get` (10 s). `type:"text"` blocks join
with `\n\n`; non-text blocks are skipped with a log. The result is capped at
`AGENTC_LIMIT_MCP_PROMPT_TEXT_BYTES` (64 KiB); over the cap the call fails, never
truncates. Records are capped at `AGENTC_LIMIT_MCP_PROMPTS_PER_SERVER` (128) per
server and `AGENTC_LIMIT_MCP_PROMPTS_TOTAL` (512) total; a retired-but-listed
record is removed from the store and frozen in a
`AGENTC_LIMIT_MCP_PROMPTS_FROZEN_CAP` (128) pile that is never reused.

### 1.7 Resources

`resources/list` and `resources/templates/list` fill per-server tables
(`AGENTC_LIMIT_MCP_RESOURCES_PER_SERVER` 256, total 1024). An over-long
uri/uriTemplate (`AGENTC_LIMIT_MCP_RESOURCE_URI` 2048) is skipped rather than
truncated (a clipped address would read the wrong resource); name/title (256),
description (2048) and mimeType (128) fields are truncated.

The first successful resource-capable list sync publishes three read-only tools
once — `mcp_list_resources`, `mcp_list_resource_templates` and
`mcp_read_resource {server,uri}` — through the internal add, so the next
turn-boundary recompose picks them up; they are never removed. The list tools
return paged JSON text from the local tables (`cursor` = decimal offset, 64 KiB
page). The read tool performs one bounded synchronous `resources/read` on a live
resource-capable server and returns joined `type:"text"` contents only: a `blob`
entry rejects the whole read (`binary resource not supported`), and text over
`AGENTC_LIMIT_MCP_RESOURCE_TEXT_BYTES` (256 KiB) fails without truncation. A
server that never advertises `resources` is never asked for either list.

### 1.8 Retry, backoff and idle drain

A FAILED server reconnects behind a capped exponential backoff:
`AGENTC_MCP_RETRY_BASE_MS` (1 s) doubled per consecutive failure up to 2^6 and
capped at `AGENTC_MCP_RETRY_MAX_MS` (60 s). `AGENTC_MCP_RETRY_MS` overrides the
base delay, and `0` (or a negative value) disables reconnect.

A READY stdio server is idle-drained on every pump so a `list_changed`
notification is honored without waiting for an exchange: at most 64 complete
messages are consumed per pump and leftovers stay buffered. An EOF from an idle
server fails it and arms the reconnect. HTTP has no idle channel because each
response is request-scoped. The HTTP transport wires its cancel predicate
through `agentc_http_set_cancel`, so an abandoned request returns `-ECANCELED`
instead of waiting out the 10 s timeout.

### 1.9 Memory bounds

The tool table is capped at `MCP_MAX_TOOLS_PER_SERVER` (256) and
`MCP_MAX_TOOLS_TOTAL` (1024); a publish past either budget fails the sync with
`-ENOSPC` and logs. A remote name over `MCP_MAX_TOOL_NAME` (256 bytes) is skipped
outright; a description over `MCP_MAX_TOOL_DESC` (8 KiB) is truncated with a log.
The published name is built to fit the registry's 64-byte cap, with a hash
suffix when it must be deduplicated (the base is clipped, never the hash).
Retired entries that a snapshot or transcript may still reference live in a
frozen pile capped at `MCP_FROZEN_CAP` (256); a handed-out entry's slot is never
freed or reused, and a full pile fails the server with a clear log instead of
recycling. An `inputSchema` copy is capped at `AGENTC_LIMIT_MCP_SCHEMA_BYTES`
(2048).

---

## 2. Versioned C ABI

### 2.1 Header and growth rules

`include/agentc_ext.h` is the stable extension ABI, shipped with the repo and
self-contained (only freestanding headers). `AGENTC_EXT_ABI` is `1`. One model:
built-in tools, MCP and linked extensions are all extensions.

Rules that keep the ABI alive:

1. **Append-only structs.** `AgcExtHost` and `AgcExt` carry `abi_version` and
   `struct_size`; the contribution structs (tool/command/section/status/
   provider) carry `struct_size` only, because the host knows their type at the
   `add_*` call. New fields go at the end and a caller that declares a smaller
   `struct_size` keeps working. A
   mismatched `abi_version` or a short `struct_size` is refused with a clear log.
2. **Capability gates.** Appended contribution fields are read with
   `AGENTC_EXT_FIELD_OK(struct, Type, field)`; appended host services with
   `AGENTC_EXT_HOST_HAS(host, field)`. Neither reads past the caller's declared
   `struct_size`, and `HOST_HAS` also rejects a present-but-NULL slot. An
   extension that needs a service beyond the baseline declares
   `AgcExt.required_host_size`; a value above `sizeof(AgcExtHost)` is refused at
   registration.
3. **JSON strings** for tools, hooks, status and providers; no compiler-specific
   structs and no Rust ABI. A hook result is always a JSON object (or absent); a
   malformed or non-object result is a handler failure, and override points fail
   closed. Consumer-specific payload shapes are validated at the consumer and
   ignored with a log when malformed.
4. **Memory** returned to the host comes from `host->alloc` and is freed by the
   host. Registered strings are copied at the `add_*`/`on` call, so an extension
   may reuse or free its own storage immediately.
5. **Single-threaded host.** An extension may use threads, but any call into the
   host from a worker goes through `host->defer`; `host->log` from a callback is
   prefixed `[ext:<name>] `.
6. **Crash = process crash.** A loaded extension is trusted native code with no
   sandbox. Isolating extensions in a helper process over the same hook protocol
   is not implemented.

An extension is a static object or shared library exporting exactly one
canonical symbol:

```c
int agentc_ext_init(const AgcExtHost *host, AgcExt *out);
```

The descriptor fills `abi_version`, `struct_size`, `name`, `version`, `order`,
`init`, `shutdown` (optional) and, when needed, `required_host_size`.

### 2.2 Host services

`AgcExtHost` exposes, in order:

- **memory:** `alloc` (zeroed), `free`, `log(level, msg)`;
- **output:** `out_write(void *out, bytes, n)` — the opaque sink for tool
  `run`/command/section output;
- **contributions:** `add_tool`, `add_command`, `add_section`, `add_status`,
  and the appended `add_provider`;
- **hooks:** `on(point, caps, priority, fn, ud)`, `off(handle)`,
  `emit(host, point, payload_json, out)`;
- **services:** `defer`, `is_cancelled(signal_token)`, `cwd`, `session_id`,
  `session_file`, `system_prompt`, `append_entry(type, data_json)`,
  `http_request`/`http_cancel`, `notify`, `set_status(key, text)`, `set_model`,
  `set_thinking`, `request_recompose`;
- **JSON string helpers** for C authors: `strdup_`, `json_get_str`,
  `json_get_int`, `json_get_bool`, `json_escape`, `set_title`.

`http_request` queues a request; the wire layer is not resumable, so at most one
queued request is dispatched per pump call and may occupy that call for up to
10 s. `http_cancel` drops a queued request before dispatch; a request already
inside `agentc_http_run` is interrupted on its next poll slice (the wire layer
polls the cancellation flag), so its callback receives the negative `-ECANCELED`
status and whatever partial body was decoded so far. A forbidden header
(`Host`, `Content-Length`, `Transfer-Encoding`) or malformed `headers_json` is
dropped with a log.

`set_status(key, text)` folds into an auto-clearing status segment keyed by
(owning extension, key); `set_model`/`set_thinking` apply only when the front end
installed a sink (`set_model` fails `-ENOSYS` with no sink; `set_thinking`
no-ops). `append_entry` accepts a custom type of at most 64 chars from
`[A-Za-z0-9_.:-]` and a JSON object of at most `AGENTC_EXT_APPEND_ENTRY_MAX`
(8192) bytes.

### 2.3 Synchronous tools

An `AgcExtTool` carries identity (`name` `[a-z0-9_:.-]+`, ≤64, unique; `label`;
`description`; `parameters_json` JSON Schema), prompt fragments
(`prompt_snippet`, `prompt_guidelines`), `flags`, `ud`, `timeout_ms`, and the
callbacks.

```
AGENTC_TOOL_READONLY    0x1
AGENTC_TOOL_DESTRUCTIVE 0x2
AGENTC_TOOL_SEQUENTIAL  0x4
AGENTC_TOOL_HIDDEN      0x8
AGENTC_TOOL_TIMEOUT_DEF_MS 120000
```

A synchronous tool provides `run(host, self, call, out, *is_error)` and writes
result text with `host->out_write`, returning `0` or `-errno`. `AgcExtToolCall`
gives the tool `call_id`, `name`, validated `args_json` and a `signal_token` to
pass to `host->is_cancelled`.

A tool with `run` non-NULL is synchronous even if the async trio is also filled
(the trio is ignored with a level-1 log); a partial async trio is rejected at
registration with a level-3 log.

### 2.4 Asynchronous tools

An asynchronous tool leaves `run` NULL and provides the complete appended
`start`/`step`/`stop` trio:

- `start(host, self, call, out, *is_error, **state)` returns `0` running, `1`
  complete or `<0` fatal. A fatal `start` is never followed by `stop`.
- `step(host, self, call, state, out, *is_error)` returns the same codes.
- `stop(host, self, state, reason)` is mandatory and runs exactly once for every
  `start` that returned `>= 0`, with one of:

| reason | value | delivered when |
|---|---|---|
| `AGENTC_EXT_TOOL_FINISHED` | 0 | start/step completed; `*is_error` is the result, not the reason |
| `AGENTC_EXT_TOOL_ERROR` | 1 | a fatal `step`; output is replaced by `error: tool '<name>' failed to run (<err>)` |
| `AGENTC_EXT_TOOL_TIMEOUT` | 2 | the job deadline passed; output is replaced by `error: tool '<name>' timed out` |
| `AGENTC_EXT_TOOL_CANCELLED` | 3 | the batch aborted (or a fatal step ran while cancel was set); partial output is kept |
| `AGENTC_EXT_TOOL_UNLOAD` | 4 | the owning extension was unloaded/removed/shut down mid-job |

`void *state` is extension-owned (`host->alloc` in `start`, `host->free` in
`stop`); `call` and its strings stay valid through `stop`. The host drives all
three on the main loop and calls `agentc_pump(0)` after each running step so
deferred work progresses with the extension off the stack. There is no cancel
callback: poll `host->is_cancelled(call->signal_token)`, valid from `start`
through `stop`. Async `timeout_ms` is clamped to
`AGENTC_EXT_TOOL_TIMEOUT_MAX_MS` (1 800 000 ms = 30 min) with a level-2 log; a
negative value becomes 0 (the driver's 120 s default). Start/step output is
staged per step and appended through `agentc_tool_append_output`, so it obeys
the same 50 KB inline cap and spill path as a sync result. A record with live
jobs is unlinked but not freed; retire delivers `stop(UNLOAD)` and the record is
released at shutdown or by the last job. A retired name is reusable, and a later
step on a retired record synthesizes `error: extension tool unloaded`.

The registry adapter (`src/ext/registry.c`, `ExtAsync`) holds the published call
copy, the per-job signal window, the extension state pointer and the stop-once
flag, flushes the per-step staging buffer into the job, and the appended
`AgcJob.cleanup` hook delivers the final stop reason before the driver frees
`job->priv`.

### 2.5 Commands, sections and status segments

- **Command:** `AgcExtCommand` registers a slash command (`name`,
  `description`, `run(host, ud, args_json, out)`).
- **Section:** `AgcExtSection` renders one prompt section after the four core
  sections, in ascending `priority` (ties keep registration order), through
  `render(host, ud, out)`.
- **Status:** `AgcExtStatusProvider` writes at most `max` `AgcExtStatusSegment`
  values into `out`, each with a slot (`AGENTC_PSEG_SLOT_LEFT`/`_RIGHT`), a
  priority, a style (`AGENTC_PSEG_STYLE_DIM/BOLD/ACCENT/WARN/ERROR/OK`) and
  NUL-terminated UTF-8 `text`; dynamically formatted text must be copied into
  the caller-owned `arena`. The host owns layout, separators, truncation and
  colour. A provider is pure formatting: synchronous, no allocation, no I/O, no
  terminal access; a control byte in its text drops the segment with a log. The
  host copies the text right after the callback returns.

### 2.6 Hook bus

`host->on(point, caps, priority, fn, ud)` subscribes; `host->off(handle)`
unregisters; `host->emit(host, point, payload_json, out)` publishes.
`agentc_ext_wants(point)` is the zero-cost fast path and
`agentc_ext_emit(point, payload_json)` is the internal fan-out. Handlers run in
`priority` order, ties by registration order, over a snapshot of the subscriber
set: subscribing or unregistering during dispatch does not affect the current
walk. `agentc_events_set_quiet` gates front-end delivery only; declared override
points dispatch regardless.

`caps` is fixed per point: `AGENTC_HOOK_OBSERVE` (side effects only) or
`AGENTC_HOOK_OVERRIDE` (may return a decision/replacement). A handler that
returns `1` or sets `*result_json` must have registered with `OVERRIDE`; `on`
rejects a mismatched subscription and an unknown point name with a level-3 log.
An `OBSERVE` handler that returns `1` or a result is logged and ignored. A
handler returning `0` continues, `1` records `handled`, and `<0` fails. On a
`chain` point, `1` records `handled` and later handlers still run; only `first`
and `chain-veto` short-circuit. Override failures are fail-closed; observe
failures are fail-open. A per-handler budget (50 ms) is enforced: three
consecutive overruns disable the handler of either kind (a clean call resets the
counter), and the overrunning occurrence itself follows the point's fail
policy. Emit recursion is capped at depth 32 (at the cap an override point
answers blocked, an observe point empty).

The point table (caps / policy / merge / failure):

| Point | Caps | Policy | Merge |
|---|---|---|---|
| `project_trust` | override | first | replace, fail-closed |
| `session_start` | observe | chain | replace, fail-open |
| `session_shutdown` | observe | chain | replace, fail-open |
| `resources_discover` | override | chain | fields, fail-closed |
| `input` | override | chain | fields, fail-closed |
| `before_agent_start` | override | chain | fields, fail-closed |
| `agent_start` | observe | chain | replace, fail-open |
| `agent_end` | observe | chain | replace, fail-open |
| `agent_before_settle` | override | chain | fields, fail-closed |
| `agent_settled` | observe | chain | replace, fail-open |
| `turn_start` | observe | chain | replace, fail-open |
| `turn_end` | override | chain | fields, fail-closed |
| `message_start` | observe | chain | replace, fail-open |
| `message_update` | observe | chain | replace, fail-open |
| `message_end` | override | chain | fields, fail-closed |
| `tool_call` | override | chain-veto | fields, fail-closed |
| `tool_result` | override | chain | fields, fail-closed |
| `tool_execution_start` | observe | chain | replace, fail-open |
| `tool_execution_end` | observe | chain | replace, fail-open |
| `before_provider_headers` | override | chain | fields, fail-closed |
| `before_provider_request` | override | chain | fields, fail-closed |
| `after_provider_response` | observe | chain | replace, fail-open |
| `provider_stream_event` | observe | chain | replace, fail-open |
| `session_before_compact` | override | first | replace, fail-closed |
| `session_before_switch` | override | first | replace, fail-closed |
| `session_compact` | observe | chain | replace, fail-open |
| `session_compact_failed` | observe | chain | replace, fail-open |
| `mcp_servers_change` | observe | chain | replace, fail-open |
| `model_select` | observe | chain | replace, fail-open |
| `thinking_level_select` | observe | chain | replace, fail-open |

The names are the `AGENTC_HEV_*` strings in `include/agentc_ext.h`; the same
names pi uses. `session_before_switch` has no `AGENTC_HEV_*` constant yet, so it
is subscribed with the literal name.

A result merges per the point's algebra. `replace` replaces the payload value
wholesale; `fields` replaces only the keys present in the result, and a key set
to JSON `null` deletes it. The `fields` result is a patch: the accumulator
preserves `null` markers while the next handler receives the effective payload,
and a consumer applies a returned patch with
`agentc_ext_merge_fields(base, patch)`. `before_provider_headers` is a **flat
header patch** (`{"<Header>": value|null}`), not a nested `headers` object; the
merge stays shallow and the consumer drops `Content-Length`,
`Transfer-Encoding` and `Host` (case-insensitive) so an extension cannot forge
framing. `agentc_ext_headers_to_json()` and `agentc_ext_headers_apply_patch()`
implement the header block conversion. OBSERVE points never merge a result.

`message_update` carries streamed assistant content: `{"type":"text_delta"|
"thinking_delta","text":…}`, and the retry-only rollback marker
`{"type":"message_reset"}`. An observer that accumulates deltas must discard
the current message on `message_reset`, exactly like the TUI and the JSON/RPC
front ends.

Examples:

```json
// tool_call (override; {"block":true} or a 1 return blocks; {"input":{…}} rewrites;
//            a blocked call may add "terminate":true to end the submission after the batch)
{"tool_call_id":"toolu_01…","tool_name":"bash","input":{"command":"rm -rf /"}}

// input (override; {"action":"handled"} skips the run, "transform" replaces text)
{"text":"go","source":"user"}

// before_agent_start (override; systemPrompt replaces the effective prompt for the run)
{"prompt":"go","system_prompt":"…"}

// message_end (override; the replacement message is a restricted assistant shape)
{"message":{"content":[{"type":"text","text":"…"}],"stop_reason":"stop"}}

// tool_result (override; content: string | text-block array | null)
{"tool_call_id":"toolu_01…","tool_name":"read","content":[{"type":"text","text":"…"}],"is_error":false}

// turn_end / agent_before_settle (override; entries += custom entries, continue = one more turn)
{"entries":[{"custom_type":"note","data":{"text":"…"}}],"continue":false}

// session_start
{"reason":"startup","session_id":"…","session_file":"…","previous_session_file":"…"}

// model_select / thinking_level_select (observe)
{"provider":"openai","model":"gpt-5","previous":"claude-sonnet-4-6"}
{"level":"medium","previous":"off"}
```

Override semantics beyond the merge algebra: `input`, `before_agent_start` and
`message_end` replacements reach the transcript/request; a blocked or failed
`input` makes `agentc_agent_submit` return `-1` with `AGENTC_EV_ERROR` carrying
`input blocked by extension`; `message_end` on a failed/aborted turn may replace
content but keeps the terminal stop/error; a blocked or failed
`before_provider_headers`/`before_provider_request` is terminal (no retry, no
transport call); `turn_end` and `agent_before_settle` share a 32-continuation
cap per submit. A blocked `tool_call` may set `terminate`; the submission
settles after the batch only when every call in the assistant message was
blocked with it (a mixed or normal call keeps the loop), cancellation wins, and
an explicit `turn_end.continue` still re-enters once. `session_before_switch`
gates RPC `new_session` (a cancel blocks before any file is touched).

### 2.7 Custom providers

An extension contributes a named provider from `init` through the appended
`add_provider` service (guard with `AGENTC_EXT_HOST_HAS(host, add_provider)`; a
rejected contribution is logged and never fails `init`). The typed
`AgcExtProvider` family in `include/agentc_ext.h` is the whole surface:

- **Identity:** `name`, `label`, `default_base_url`, `path`, `env_keys[3]`,
  `needs_key`, discover style, optional `auth` and static `models`.
- **Request view:** `AgcExtRequestView` plus `AgcExtMessageView`/
  `AgcExtBlockView`/`AgcExtToolView` is the host-materialized request
  (provider/model/system/thinking/max_tokens and the filtered transcript and
  visible tools). Failed/aborted and empty assistant turns are filtered out;
  `AGENTC_TOOL_HIDDEN` tools never reach the view.
- **`build_request(host, self, view, head_out, body_out)` is mandatory.** Write
  the header block and the JSON body into the two opaque sinks with
  `host->out_write`. The core inserts the blank line, sanitizes the head
  (CRLF → space, token-safe keys; `Host`/`Content-Length`/`Transfer-Encoding`
  and any occurrence of the declared auth header are dropped), adds the resolved
  credential as the declared header, `Content-Length`, and owns retries,
  timeouts, cancellation and `--record`/`--dump-wire`.
- **Stream sink:** `AgcExtStreamSink` with `text`, `thinking`, `tool_start`,
  `tool_args`, `response_id`, `usage`, `stop`, `error`, driven by
  `stream_event` (mandatory), which receives one parsed `AgcExtWireEvent` (SSE)
  at a time. `stream_open`/`stream_finish`/`stream_close` are optional
  (NULL = stateless/default); `AgcExtStream.ud` is extension state allocated in
  `open` and freed in `close`. This is SSE-only.
- **Auth is core-applied.** `AgcExtProviderAuth.kind` is `NONE`, `BEARER` or
  `HEADER` (`QUERY` is reserved and rejected at registration). The key is
  resolved by the core (flag/OAuth/env/`auth.jsonc`/config) and never appears in
  the view or reaches the extension. Discovery uses the same declaration; an
  empty `auth_headers` override (`NONE`) is authoritative and only rows without
  the hook fall back to the built-in Bearer default.
- **Static models** register at `add_provider` marked `AGENTC_MODEL_STATIC`, so
  `agentc_model_clear_dynamic()` keeps them; the provider record owns them and
  `agentc_model_clear_static()` drops them when the record is freed. They share
  the runtime table's 256 slots and carry no cost rate.
- **Validation:** name `[a-z0-9_.:-]{1,64}` unique against builtins,
  materialized presets and live rows; a `/`-leading CR/LF/space-free path; an
  `http(s)` base URL; discover style `DEFAULT`/`ANTHROPIC`/`OLLAMA`/`NONE` (the
  public ABI's `NONE` is translated to the internal no-listing value on store,
  so a custom row never inherits the internal Google probe); ≤256
  models with ≤256-byte ids; a token-safe auth header. The registry is capped at
  `AGENTC_EXT_MAX_PROVIDERS` (64). Rows are frozen until shutdown: unload
  retires them (lookups skip them; a held handle gets a clean `provider
  unloaded` error), a retired name stays reserved, and
  `agentc_ext_shutdown` frees them. `providers.<id>.base_url` and
  `api_keys.<id>` config cover an extension-provider id (user scope only).

`extensions/fake_provider/fake_provider.c` is the reference. Limits: SSE only;
no query auth; no extension-owned discovery parser; static models carry no cost
rates, per-model thinking config or multimodal channel; the generic config does
not read `providers.<id>.headers`; custom rows use `api = name`.

### 2.8 Process-wide limits

- `AGENTC_EXT_MAX_HOOKS` 128 registrations (an unknown point name is rejected at
  `on` with a level-3 log);
- `AGENTC_EXT_MAX_DEFER` 256 queued callbacks (overflow counted and logged);
- `AGENTC_EXT_MAX_HTTP` 8 queued requests, one dispatched per pump for up to
  `AGENTC_EXT_HTTP_TIMEOUT` (10 s);
- `AGENTC_EXT_MAX_EMIT_DEPTH` 32;
- 8 live `set_status` keys;
- 64 custom providers;
- `AGENTC_EXT_BUDGET_NS` 50 ms per hook handler with
  `AGENTC_EXT_MAX_OVERRUNS` 3.

### 2.9 Rust support

An `agentc-ext` crate (no_std-compatible core) wraps the raw ABI. `Host` is a
borrowed zero-cost wrapper over the vtable: memory, output, contributions,
hooks, services and JSON helpers. `Tool::new(c"name", c"desc", SCHEMA, run)` has
const builders `.label`, `.readonly`, `.destructive`, `.sequential`, `.hidden`,
`.flags`, `.snippet`, `.guidelines`, `.timeout_ms`; `to_raw()` materializes the C
struct and `register_tools(host, &[...])` registers a static table; `schema!`
materializes a JSON string as a `&'static CStr` at compile time. The crate also
mirrors the asynchronous ABI (`ToolStart`/`ToolStep`/`ToolStop`, the reason and
clamp constants, and the three `AgcExtTool` fields). Build as
`--crate-type staticlib` for the default static path; the static path links the
object and registers through the same vtable, so a Rust tool pays zero `dlopen`
and zero JSON on the tool path.

---

## 3. Extension registry and composition

`src/ext/registry.{c,h}` (`include/ext.h` is the app/core-facing API) registers,
loads, collects and tears down extensions. Extensions compose in a fixed order:

```c
agentc_ext_register_defaults(no_mcp);   /* builtin-tools, builtin-context, mcp */
agentc_ext_register_linked();           /* build/exts.c */
agentc_ext_register_dynamic();          /* <config home>/extensions/ (macOS/Windows) */
agentc_ext_apply_config(cfg);           /* extensions.disabled */
agentc_ext_load_all();                  /* init, lowest order first */
```

### 3.1 Defaults

`agentc_ext_register_defaults(no_mcp)` registers `builtin-tools`,
`builtin-context` and (unless `no_mcp`) `mcp` through the same host as any other
extension. Their only privilege is that they compile inside core and may include
`src/ext/registry_int.h` and use the internal bus directly. The `builtin-tools`
`grep`/`find`/`ls` backend is engine-selected (`.agents/design/20-core-agent.md`
§4.5); extensions are unaffected — their tools, schemas and lifecycle do not go
through the engine.

### 3.2 Statically linked extensions

`tools/gen-exts.py` reads `extensions/manifest.json`:

```json
{
  "extensions": [
    { "name": "hello",     "c":    "extensions/hello/hello.c" },
    { "name": "rust_echo", "rust": "extensions/rust_echo" },
    { "name": "fake_provider", "c": "extensions/fake_provider/fake_provider.c" },
    { "name": "async_demo", "c": "extensions/async_demo/async_demo.c" }
  ]
}
```

and emits `build/exts.c`, a `{name, entry}` table that is deliberately **not**
weak: each row is guarded by `#ifdef AGENTC_STATIC_EXT_<ident>` and is NULL
otherwise (lld's Mach-O linker rejects an unresolved weak import). A build that
links an extension passes `-DAGENTC_STATIC_EXT_<ident>` (`make release-exts`);
`src/ext/linked.c` includes the generated file and `agentc_ext_register_linked()`
walks it at startup, calling `agentc_ext_adopt(name, entry)` for each row. Each
entry exports the canonical `agentc_ext_init`; the build renames C sources with
`-Dagentc_ext_init=agentc_ext_<ident>_init`, and Rust staticlibs export that
name directly. A NULL entry logs "not linked" and is skipped, so a binary that
does not link an extension still builds.

The manifest is the build list, not just the registry list: `make ext-pipeline`
/ `make release-exts` compile every `c` entry and build the `rust` entries
through cargo from the same manifest, passing one `-DAGENTC_STATIC_EXT_<ident>`
per linked entry. The per-entry rules are `build/exts.mk`, generated by
`tools/gen-exts.py --mk-out` (temp file + `os.replace`, only when the content
changes), so a manifest generation/validation error is a hard build error and
every object depends on the generated fragment. `tools/gen-exts.py` validates
the manifest in every mode: duplicate names, a missing `c`/`rust` source, and a
name whose sanitized form is not a valid C identifier are rejected.

### 3.3 Dynamic loading

On macOS and Windows the platform loader opens a `.dylib`/`.dll` at runtime and
looks up the canonical `agentc_ext_init` (`dlopen` from libSystem for macOS,
`LoadLibraryExW` from kernel32 for Windows). The ABI and registration path are
identical to the static path, so enabling it changes only the loader. The search
is one fixed directory, `<config home>/extensions/`: suffix `.dylib`/`.dll`
(matched case-insensitively), file stem = extension name `[a-z0-9_.:-]{1,64}`,
bytewise order, at most `AGENTC_EXT_MAX_DYNAMIC` (32) candidates per scan with
one overflow log. Unknown suffixes, invalid stems, directories and (on POSIX)
group/other-writable files or directories are ignored; a missing directory is
fine, and an unreadable or failing candidate is logged and skipped. There is no
recursion, no project directory and no `extensions.paths`; `extensions.disabled`
filters dynamic extensions by name like any other. Duplicates are skipped before
the library is opened, and `agentc_ext_register_dynamic()` runs after
`register_linked()` and before `apply_config()`, so `AgcExt.order` applies too.
`--list-extensions` marks a dynamic row `(dynamic)`, and the descriptor's
`out.name` must equal the file stem.

A loaded library is trusted native code: no sandbox, no crash isolation, full
process privileges. Windows confines the loader search to the DLL's own
directory and System32 (no PATH/current-directory search; unsupported search
flags fail closed); POSIX rejects a group/other-writable file and directory
(symlinks are followed, so a TOCTOU window remains between the stat and the
open). A dynamic library's handle is closed exactly once and only when the
extension is idle — registrations removed, `shutdown()` returned, no live async
job, no callback on the stack — deferred to the last job unlink, the pump
epilogue or shutdown; a mapping still needed at shutdown stays mapped with a
warning. Linux is static-only: a `-nostdlib` static ELF has no dynamic loader,
and shipping a custom ELF loader is a non-goal.

### 3.4 Registry state and introspection

`AgcExtState` is `PENDING` (registered, `load_all` has not run), `LOADED`
(`init()` succeeded or none), `FAILED` (`init()` returned an error) or `SKIPPED`
(excluded by `extensions.disabled`). `agentc_ext_describe(out, max)` is the
count-first descriptor-table introspection for `--list-extensions` and the
startup summary: registration order, `state`, ` disabled`, `known` (false only
for an unmatched disabled name) and `dynamic`. `agentc_ext_is_loaded(name)` is
true only for a registered extension whose `init()` succeeded. After `load_all`,
a noteworthy run (a linked extension, a disabled or unknown name, a failed init,
a pending record) logs one `extensions: loaded …` summary line; a
defaults-only run stays silent.

`agentc_ext_apply_config(cfg)` excludes defaults, linked and dynamic
extensions named in `extensions.disabled`; it runs after registration and before
`load_all`, so a matching extension is removed before it can init. The
`builtin-tools` extension registers no tools at `init` in the `mcp` case: the
`mcp` extension's pump publishes tools with `agentc_ext_add_tool_internal` as
servers connect, so a fast local server's tools are part of the startup table
(`builtins → linked → mcp`) and tools from a server still connecting appear at
the next turn boundary via the recompose hook.

### 3.5 Lifecycle and ownership

Every registration (`on`, `add_*`) is tagged with the extension inside `init`
(the **owner**). `agentc_ext_unload(name)` removes every registration by
that owner first, then calls `shutdown()`; a failed `init` rolls back the same
way, and `agentc_ext_shutdown()` tears down every extension in that order and
ends with `agentc_status_reset()`. Owner rollback also drops the owner's
deferred callbacks and pending extension HTTP requests; a full
`agentc_ext_shutdown()` resets the defer ring under its lock. `off(handle)`
covers the single-handler case. The owner is saved and restored around every
hook/tool/command/section/pump/defer/HTTP callback, so attribution is correct
even for re-entrant work: `host->log` from a callback is prefixed
`[ext:<name>] `, and failure/overrun diagnostics name the extension.
`set_status` is keyed by (owner, key), so two extensions may use the same key.

The generated linked table (`agentc_ext_adopt`) and the dynamic loader
(`agentc_ext_register_dynamic`) both create records through the same
owner-tagged insertion that `agentc_ext_register` uses.

**Do not tear the registry down from `init`.** While `agentc_ext_load_all()` is
on the stack (including any nested load), `agentc_ext_shutdown()` is a logged
no-op (`error: ext: shutdown called during load; ignored`); the process-end
shutdown still tears everything down. Unloading the record being initialized is
safe: `load_all` re-checks the record after `init` and skips it when its `used`
flag was cleared.

`agentc_ext_pump()` services deferred callbacks and at most one pending
extension HTTP request per call. `agentc_ext_set_context()` publishes `cwd`,
`session_id`, `session_file` and `system_prompt` to extensions; sinks connect
`append_entry`, UI notifications and the model/thinking controls
(`agentc_ext_set_entry_sink`, `agentc_ext_set_ui_sink`,
`agentc_ext_set_model_sink`, `agentc_ext_set_thinking_sink`).
`agentc_ext_dirty()`/`agentc_ext_clear_dirty()` track whether a
registration changed and a turn-boundary recompose is pending.

### 3.6 Tool, command and section collection

`agentc_ext_tools(out, max)` is the count-first collection of the active tool
table; `agentc_ext_commands(out, max)` returns registered slash commands and
`agentc_ext_run_command(name, args)` runs one; `agentc_ext_render_sections(out)`
renders extension prompt sections. Registry storage is pointer-stable: a
handed-out `AgcTool.ud` stays valid as later registrations grow the table.
`agentc_ext_remove_tool_internal(name)` (internal bus) retires an entry without
freeing data an in-flight transcript may still reference.

---

## 4. Declarative resources

The declarative layer covers most personal customization with no code execution:
`config.jsonc`, `mcp.jsonc`, `SKILL.md` skills, `prompts/*.md` templates, named
`themes/<name>.jsonc`, `keybindings.jsonc` and `trust.jsonc`. All of it parses
with the same JSONC/JSONL/markdown helpers as the rest of the system.

`resources_discover` is the hook through which an extension contributes extra
resource roots of all three kinds. `agentc_project_apply_resources(cwd, trusted)`
consumes it once at startup and routes `skillPaths`, `promptPaths` and
`themePaths` into per-kind, process-lifetime root stores:

- `AGENTC_RESOURCE_ROOTS_MAX` 8 roots per kind;
- at most `AGENTC_RESOURCE_PATHS_MAX` (32) paths per list;
- every contributed root must be absolute, free of any `..` segment, and under
  the config home (always) or the resolved project cwd (trusted only); anything
  else is rejected and logged, and a blocked or failed hook contributes
  nothing;
- roots are deduplicated; the loaders scan the standard roots first, then each
  contributed root in order, so a later same-name resource wins.

A file prompt template loads once after startup; skills scan their roots on each
context render; theme names resolve on demand. An extension trying to add a root
at runtime would need a restart. Caps are in
`.agents/design/20-core-agent.md` §5.2 and `src/base/limits.h`.

---

## 5. RPC mode

`--mode rpc` is JSONL on stdin/stdout with agentc's own command set (not tied to
another harness). The commands are `prompt`, `prompt_template {name,args}`,
`abort`, `get_state`, `set_model`, `set_thinking_level`, `get_available_models`,
`compact`, `bash`, `new_session` and `get_messages`. `prompt_template` expands a
registered prompt (file template or MCP prompt) through the prompt registry and
submits the result like `prompt`; an unknown name or failed expansion is a
failed response, and an MCP prompt can take up to its 10 s `prompts/get` bound.
`new_session` passes the `session_before_switch` gate, then swaps the session
and emits `session_start{reason:"new"}`. The wider RPC contract, the
embeddable SDK and the framed CBOR protocol are planned in
`.agents/plans/P5-AXES.md`.

---

## 6. Deliberately out of scope

| Feature | Why not |
|---|---|
| JS/TS extensions via an embedded engine | would require a JS engine and libc; violates the freestanding minimalism goal |
| A large provider/wire matrix in core | two core wire APIs cover the majority, and an extension can add a provider without touching core |
| codemode sandbox | a WASM/JIT dependency; MCP is the interop story instead |
| virtual models / routing | deferred; the provider table can grow a router later |
| durable/client-server session layers | JSONL sessions plus RPC mode cover the need; the framed protocol is planned separately |

pi inspiration notes: pi's `pi.on("event")` string event model, its tool/command
registration and its `ctx.ui`/`ctx.*` surface are the reference for the hook
bus, tool descriptors and status/context services here. agentc keeps a single
extension model (tools + sections + hooks + providers) rather than a second
typed harness model, and its status line is a provider-built segment table,
which is stronger than a keyed status setter for layout. pi's `ctx.*` surface
also inspires the service/sink split (context values, notify/status/title,
model/thinking control).
