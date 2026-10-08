# 20 — Core agent

The core is a freestanding C23 agent loop with an explicit data model, an
injected transport, and a bounded, non-blocking tool driver. It includes only
`agentc.h` plus the layer headers it uses (`plat.h`, `net.h`, `wire.h`); it never
includes an OS header. All state is explicit and allocation ownership is
documented per structure.

The design inspiration is [pi](https://github.com/earendil-works/pi); the
provider, session, tool and hook contracts are covered in
`.agents/design/30-extensibility.md`, `.agents/design/40-tui.md`, and the two
plans `.agents/plans/P5-AXES.md` and `.agents/plans/P5-TOOL-ENGINE.md`.

---

## 1. Data model

### 1.1 Transcript and messages

`AgcTranscript` (`include/agent.h`) is the single session transcript: a growable
vector of `AgcMsg` headers, with the owned model/provider names and a system
prompt snapshot. Nothing is reference-counted.

```c
typedef struct {
    int        type;        /* AGENTC_BLK_TEXT, _THINK, _TOOLCALL */
    char      *text;        /* TEXT/THINK body (owned) */
    size_t     text_len;
    char      *tool_id;     /* TOOLCALL (owned) */
    char      *tool_name;
    char      *tool_args;   /* raw JSON text (owned), parsed lazily */
} AgcBlock;

typedef struct {
    int        role;        /* AGENTC_ROLE_SYSTEM/_USER/_ASSISTANT/_TOOL */
    AgcBlock  *blocks;
    size_t     nblocks, blocks_cap;
    AgcUsage   usage;       /* input/output/cache read/write/reasoning + cost_micro */
    int        stop_reason; /* AGENTC_STOP_* */
    char      *error;       /* owned, or NULL */
    u64        ts_ms;
} AgcMsg;

typedef struct {
    AgcMsg     *msgs;
    size_t      n, cap;
    const char *model;      /* borrowed */
    const char *provider;   /* borrowed */
    const char *system;     /* owned system prompt snapshot */
} AgcTranscript;
```

Ownership: the transcript owns every message and every string reachable from a
message. `agentc_transcript_push` appends a zeroed message, `agentc_msg_add_text`
and `agentc_msg_add_tool_call` append blocks, `agentc_msg_tool_args_append`
appends streaming argument text, and `agentc_transcript_free`/`agentc_msg_free`
release them.

Tool-call arguments are kept as the **raw JSON text** the provider streamed.
Streaming deltas append to the tool-call block; the document is parsed once when
the call is prepared (and, for previews, on demand). This avoids reparsing a
partial object per delta.

A tool result is an `AGENTC_ROLE_TOOL` message whose first block is text carrying
the provider call id in `tool_id`; providers read exactly that field when
converting the transcript to wire messages. A failed result is recorded in the
message's `error` field (`"error"`), which the session serializer writes as
`is_error`.

### 1.2 Stop reasons

`AGENTC_STOP_PENDING`, `AGENTC_STOP_STOP`, `AGENTC_STOP_LENGTH`,
`AGENTC_STOP_TOOLUSE`, `AGENTC_STOP_ERROR`, `AGENTC_STOP_ABORTED`. The set is
deliberately small; a deferred stop is not represented.

A failed (`ERROR`) or aborted (`ABORTED`) assistant turn stays in the transcript
for the UI and telemetry but is never serialized to a provider and is never
persisted: it can carry partial tool-call arguments whose results were not
persisted, so sending it would produce a tool call with no matching result.
`agentc_provider_msg_serializable()` is the eligibility predicate
(`src/prov/provider.h`).

### 1.3 Memory, buffers and JSON arenas

- The base allocator (`src/base/mem.c`) uses power-of-two size classes up to
  64 KiB and 1 MiB chunks mapped with `os_map`. `agentc_alloc` aborts on OOM and
  is annotated `returns_nonnull`; `agentc_alloc_try` is the large-block variant
  that can return NULL.
- `AgcBuf` is the growable byte buffer used for request bodies, SSE scratch,
  session lines and tool output. It has reserve/push/cstr/printf/clear/free.
- `AgcJsonArena` owns one parsed document (`agentc_json_arena_new`,
  `agentc_json_parse_in`, `agentc_json_arena_clear`, `agentc_json_arena_free`).
  `agentc_json_parse()` keeps a process default arena for simple callers. A
  parsed document's pointers are valid until its arena is cleared or freed.
- A model request owns a per-stream JSON arena in `AgcStreamState`; the agent
  loop creates and releases it around each request.
- Tool argument parsers and the ripgrep line scratch start on a shared
  `AGENTC_LIMIT_TOOL_ARENA` (16 KiB) arena that doubles on demand, rather than
  the 1 MiB JSON default, so a single `grep`/`find` call does not map a
  megabyte to parse one argument object.

### 1.4 Limits

Hard caps live in `src/base/limits.h`, one place per accumulator fed by a peer
or a file: `AGENTC_LIMIT_TOOL_BYTES` (50 000) and `AGENTC_LIMIT_TOOL_LINES`
(2000) for displayed tool output, `AGENTC_LIMIT_SSE_EVENT_BYTES` (1 MiB),
`AGENTC_LIMIT_HTTP_HEADER_BYTES` (64 KiB), `AGENTC_LIMIT_READ_BYTES` (64 MiB),
`AGENTC_LIMIT_EDIT_BYTES` (8 MiB), the grep/find budgets, and the MCP budgets.

---

## 2. Turn loop

### 2.1 Entry points and state

`src/core/agent.c` implements one agent per `AgcAgent`, advanced synchronously by
the front end. The public entry points are:

```c
int  agentc_agent_submit(AgcAgent *a, const char *text); /* one run to completion */
void agentc_agent_abort(AgcAgent *a);                    /* cancel request + jobs */
```

plus the setters in `include/agent.h` (`set_system`, `set_tools`,
`set_transport`, `set_events`, `set_thinking`, `set_model`, `set_max_tokens`,
`set_retry`, `set_record`, `set_insecure`), the observer that persists
transcript messages, the tool-veto and recompose callbacks, and the transcript
accessors. `agentc_agent_load()` deep-copies a replayed transcript before the
first submit.

The loop does not use a global state enum; it is a nested loop inside
`agentc_agent_submit` with a retry loop around each request and an outer settle
loop. A run ends through one shared finish tail (recompose → `agent_end` →
`agent_before_settle` → `agent_settled`).

The front end drives cooperative pumping with `agentc_pump_install()` and
`agentc_pump()`; long-running tools call the pump so the UI can repaint and read
input while work proceeds.

### 2.2 One run

1. **Input hook.** The `input` override may handle, transform or block the user
   text. A blocked/failed handler sets the last error, emits `AGENTC_EV_ERROR`
   and returns `-1`; a handled input returns `0` without a run.
2. **System prompt.** The per-run override is reset; the effective prompt is
   built or refreshed and published to the extension context; the
   `before_agent_start` override may replace it for the run.
3. **Agent start.** `AGENTC_EV_AGENT_START` is emitted and the user message is
   appended and passed to the observer.
4. **Turn loop** (repeats until a terminal stop or the continuation budget is
   spent):
   - refresh the auto system prompt, emit `AGENTC_EV_TURN_START`;
   - check the compaction threshold before the request;
   - push an assistant placeholder and emit `AGENTC_EV_MSG_START`;
   - run the retry loop for the request (build → transport → SSE map →
     `finish`), recording usage and `cost_micro`;
   - run `message_end`, emit `AGENTC_EV_MSG_END`, persist through the observer;
   - if the turn produced tool calls with stop `TOOLUSE`, prepare and run the
     batch (see §4.2), apply `tool_result` overrides, and append one
     `AGENTC_ROLE_TOOL` result per call **in source order**;
   - run `turn_end`, emit `AGENTC_EV_TURN_END`, and decide whether to continue.
5. **Finish tail.** `recompose_now` refreshes the tool table at the turn
   boundary; `AGENTC_EV_AGENT_END` carries the terminal stop reason; the
   `agent_before_settle` override may request one more turn; `agent_settled` is
   emitted once per submit.

Failed and aborted turns take a hard exit: their `message_end` still runs, an
`AGENTC_EV_ERROR` carries the message, and no tools run. A turn that completed
with a wire stop reason other than `tool_use` but carries calls (a serializer
mismatch) answers each with an `error: …` result before it settles, so a
persisted transcript never holds a call without a result. An aborted or failed
turn is neither serialized nor persisted, so its calls are deliberately left
unanswered: synthesizing results there would persist a `tool` result with no
matching `tool_call` on replay.

`AGENT_CONTINUATION_MAX` is 32: `turn_end.continue` and
`agent_before_settle.continue` share that per-submit budget.

A `tool_call` veto may set `terminate` together with `block`; only when **every
call in the assistant message** was blocked with `terminate` does the
submission settle after the batch (the assistant keeps stop reason
`tool_use`). A mixed or normal call keeps the loop; cancellation is checked
first and wins; an explicit `turn_end.continue` still re-enters once under the
shared cap.

### 2.3 Retries

`src/core/retry.c` owns the policy. Retryable: transient connect/DNS/TLS errors,
HTTP 408/409/429/5xx, and a dropped stream before any content or stop marker.
Backoff is `500 ms * 2^n`, capped at 60 s, honoring `Retry-After` (integer
seconds or an IMF-fixdate HTTP date, clamped to 1 h), with ±12.5% jitter from
`os_random`; the default maximum is 5
attempts (`config retry.max_attempts`). Non-retryable: 401/403/404,
context-length 400s, and verification failures. The request is rebuilt from the
transcript on every attempt because the send buffer lives in the request
lifetime. The wait between attempts sleeps in ≤50 ms slices and aborts as soon
as `agentc_agent_abort()` sets the cancel flag.

When a stream ends with no stop marker and attempts remain, the loop retries
even if text already streamed; when attempts are exhausted it reports
`stream ended prematurely`. To keep a retry from appending a second copy to the
front end, an attempt that already delivered text, reasoning or tool-argument
deltas emits `AGENTC_EV_MSG_RESET` before the next attempt: front ends discard
what they buffered for the `message_start` they are re-running.

### 2.4 Compaction

`src/core/compact.c` provides the pure pieces and `agent.c` owns the
summarization request:

- **Estimate:** the last assistant message's usage plus `chars/4` of trailing
  messages (block overhead included), via `agentc_compact_estimate()`.
- **Trigger:** automatic when `estimate > ctx_window - reserve_tokens`
  (`compact_reserve` defaults to 16 384).
- **Cut:** `agentc_compact_cut()` walks back from the tail accumulating
  `keep_recent_tokens` (default 20 000) and never splits a tool result from its
  call.
- **Action:** one quiet summarization request through the normal stream path
  (tools off, thinking off, a 
  `max_tokens` derived from `reserve_tokens`) produces the
  `## Goal / Constraints / Progress / Decisions / Next Steps / Critical Context`
  checkpoint (`agentc_compact_system_prompt()`). The
  `session_before_compact` override may cancel compaction or supply a non-empty
  `summary` verbatim, in which case the provider request is skipped. The
  checkpoint is emitted with `AGENTC_EV_COMPACT` before the transcript splice,
  then replaces the compacted prefix as a user message. `agentc_agent_compact()`
  is the manual entry point.

### 2.5 Hooks at the loop boundary

The loop calls the extension hook bus at these points (full contract in
`30-extensibility.md` §2.6): `input`, `before_agent_start`, `agent_start`,
`turn_start`, `message_start`/`message_update`/`message_end`,
`tool_call`/`tool_execution_start`/`tool_execution_end`/`tool_result`,
`turn_end`, `agent_end`, `agent_before_settle`, `agent_settled`,
`before_provider_headers`, `before_provider_request`, `after_provider_response`,
`provider_stream_event`, `session_before_compact`/`session_compact`/
`session_compact_failed`, `model_select` and `thinking_level_select`. The
`tool_call` veto is also installed directly as `AgcToolVetoFn`; the recompose
hook re-runs the app's active-tool selection.

### 2.6 Agent events

`include/agent.h` declares the front-end event enum:

```
AGENTC_EV_AGENT_START, TURN_START, MSG_START, TEXT_DELTA, THINK_DELTA,
TOOL_ARGS_DELTA, MSG_END, TOOL_EXEC_START, TOOL_EXEC_END, TURN_END,
AGENT_END, ERROR, COMPACT
```

Streaming deltas carry `AgcTextDelta` (`text`, `len`); tool execution events
carry `AgcToolExec` (`call_id`, `tool_name`, `args_json`, `result`, `is_error`,
`duration_ms`); compaction carries `AgcCompactInfo`.

`src/core/events.{c,h}` is the typed hub: slot 0 is the process-wide primary and
slots 1..7 are process-wide subscribers (`agentc_events_subscribe`); the
per-agent callback installed by `agentc_agent_set_events()` is dispatched first
and separately, so multiple agents coexist. A global quiet gate
(`agentc_events_set_quiet`) suppresses every sink including the extension
bridge, which is what makes a quiet compaction stream silent.
`agentc_events_wants()` is the hot-path check. The extension bridge
(`agentc_ext_emit_agent_event`) maps each event to its hook point and JSON
payload; JSON is emitted only for the session log, `--mode json`, RPC and
extensions.

Internal consumers receive typed state. The provider adapter writes the
`AgcStreamState` for a built-in provider directly (`map_sse` sets the message
blocks, usage, response id and stop reason); an extension provider drives the
same state through `AgcExtStreamSink` calls (`text`, `thinking`, `tool_start`,
`tool_args`, `response_id`, `usage`, `stop`, `error`). JSON is emitted only for
the session log, `--mode json`, RPC and extensions.

---

## 3. Providers

### 3.1 Adapter table

`AgcProvider` (`include/agent.h`) is the public vtable: `build_request`, `map_sse`,
`finish`. `build_request` writes the provider head, the blank line and the JSON
body into one `AgcBuf`; the loop splits head and body at `\r\n\r\n`. Request
bodies stream through the `agentc_jsonw_*` writer (no DOM). `map_sse` maps one
`AgcSseEvent`; `finish` closes the assistant message, usage and stop reason.

Adapter-private stream state lives in `AgcStreamState.priv`, owned by
`stream_open`/`stream_close`. `agentc_stream_state_init()` memsets the state,
sets the message/stop reason, allocates the per-stream JSON arena and calls
`stream_open`; `agentc_stream_state_close()` calls `stream_close` and releases
the arena.

### 3.2 Descriptor registry

`src/prov/provider.{c,h}` holds one `AgcProviderOps` row per named provider:
wire `api`, request `path`, `default_base_url`, credential `env_keys[3]`,
`needs_key`, discovery style, the JSON key for the output cap
(`max_tokens_key`), the adapter hooks, and an optional discovery auth-header
hook (`auth_headers`). Providers are data, not branches in the agent loop. Rows
are keyed by name; `agentc_provider_by_api()` returns the first row for an api.
The public `AgcProvider` handle is mirrored lazily from the row.

Registry API (`src/prov/provider.h`):

- `agentc_provider_register`, `agentc_provider_by_name`,
  `agentc_provider_by_api`, `agentc_provider_ops`, `agentc_provider_handle`;
- `agentc_provider_open_ops(st)` recovers the row inside a stream callback;
- `agentc_provider_all()` is the count-first enumeration of live,
  non-retired rows, deduplicated by name+api in registration order;
- `agentc_provider_registry_reset()` and `agentc_provider_retire_ext()` support
  extension teardown;
- `agentc_provider_max_tokens_key(provider)` resolves the output-token JSON key
  (default `max_completion_tokens`);
- `agentc_request_visible_tools()` counts tools the model may see (skips
  `AGENTC_TOOL_HIDDEN`).

Built-in rows: `anthropic-messages`, `openai-chat` (`openai`, `ollama`,
`ollama-cloud`), `openai-codex-responses`, and `google-generativeai`. Row
factory functions return process-stable handles. `agentc_prov_openai_compatible`
registers a dynamically named OpenAI-compatible row for a user endpoint, and
`agentc_prov_openai_compatible_gateway()` registers the config-declared
`providers.<id>.base_url` variant that does not require a credential (both are
heap rows, never moved, process lifetime).

Preset rows for `openrouter`, `xai`, `deepseek`, `groq`, `mistral`, `together`
and `gemini` are lazily materialized from the `openai-chat` hooks on first
by-name lookup. Rows that share a name but not an api (OpenAI's two wire
protocols) stay distinct.

| Provider | Base URL | Auth |
| --- | --- | --- |
| `anthropic` | `https://api.anthropic.com` | `ANTHROPIC_API_KEY` / `ANTHROPIC_AUTH_TOKEN` |
| `openai` | `https://api.openai.com/v1` | `OPENAI_API_KEY` |
| `openai` (subscription) | `https://chatgpt.com/backend-api/codex` | ChatGPT OAuth |
| `google` | `https://generativelanguage.googleapis.com/v1beta` | `GEMINI_API_KEY` / `GOOGLE_API_KEY` |
| `ollama` | `http://127.0.0.1:11434/v1` | none (`OLLAMA_API_KEY` honored if set) |
| `ollama-cloud` | `https://ollama.com/v1` | `OLLAMA_API_KEY` |
| `openrouter` | `https://openrouter.ai/api/v1` | `OPENROUTER_API_KEY` |
| `xai` | `https://api.x.ai/v1` | `XAI_API_KEY` |
| `deepseek` | `https://api.deepseek.com/v1` | `DEEPSEEK_API_KEY` |
| `groq` | `https://api.groq.com/openai/v1` | `GROQ_API_KEY` |
| `mistral` | `https://api.mistral.ai/v1` | `MISTRAL_API_KEY` |
| `together` | `https://api.together.ai/v1` | `TOGETHER_API_KEY` |
| `gemini` | `https://generativelanguage.googleapis.com/v1beta/openai` | `GEMINI_API_KEY` |

`OLLAMA_HOST` (native host, `/v1` appended when missing) overrides the local
base URL; `OLLAMA_CLOUD_BASE_URL` and `OPENAI_BASE_URL` override theirs.
`providers.<id>.base_url` in config overrides any row, including an extension
provider.

### 3.3 Anthropic Messages

`POST {base}/v1/messages` with `x-api-key`, `anthropic-version: 2023-06-01`,
`accept: text/event-stream` and an optional `anthropic-beta` list. Body:
`{model, max_tokens, system:[{type:text}], messages:[…],
tools:[{name,description,input_schema}], stream:true, thinking?:{type:enabled,
budget_tokens}}`. Tool results group into one `user` turn; thinking blocks are
not replayed (no signature is retained) and `cache_control` is not emitted. SSE
mapping: `message_start`, `content_block_start` (text/thinking/tool_use),
`content_block_delta` (`text_delta`/`thinking_delta`/`input_json_delta`),
`content_block_stop`, `message_delta` (stop_reason + usage), `message_stop`.
Stop map: `end_turn|stop_sequence|pause_turn → stop`, `max_tokens → length`,
`tool_use → toolUse`, `refusal → error`.

### 3.4 OpenAI Chat Completions

`POST {base}/chat/completions` with `Authorization: Bearer`; body `{model,
messages, tools, stream:true, stream_options:{include_usage:true},
max_completion_tokens?}`. Assistant content is a string; each tool result is its
own `role:"tool"` message. No adapter carries image or other multimodal content
(`AgcBlock` has only text/thinking/tool-call); the catalog's `image` flag is
capability metadata only. SSE mapping:
`choices[0].delta.content`, `delta.reasoning_content|reasoning`,
`delta.tool_calls[i]` keyed by index (id/name on first delta, arguments
accumulated), `finish_reason`, and the final usage chunk. `[DONE]` is the
sentinel; a missing `finish_reason` with tool calls present maps to `toolUse`.
The row's `max_tokens_key` selects the output-cap field (`max_tokens` for the
compatible presets and user endpoints, `max_completion_tokens` for first-party
OpenAI, `max_output_tokens` for the Codex Responses adapter, §3.5).

### 3.5 ChatGPT subscription (Codex Responses)

A ChatGPT subscription credential is not an API key. When `openai` is
authenticated by OAuth, the adapter switches to
`POST https://chatgpt.com/backend-api/codex/responses` (the Responses API) with
`chatgpt-account-id` (parsed from the `id_token` claim
`https://api.openai.com/auth`), `originator: agentc` and
`openai-beta: responses=experimental`. The body uses `instructions` for the
system prompt and an `input` array of `message`/`function_call`/
`function_call_output` items (`store:false`, `stream:true`), and emits
`max_output_tokens` from the request's `max_tokens` cap. The SSE mapper
reads `response.output_text.delta`, `response.reasoning_summary_text.delta`,
`response.output_item.added`, `response.function_call_arguments.delta` and
`response.completed` (usage). The backend also lists models at `{base}/models`
(`AGENTC_DISCOVER_CODEX`, §3.8), so newer ChatGPT models appear in `/model`
and auto-pick; the built-in catalog (`gpt-5-codex`, `codex-mini-latest`) is the
stand-in when a probe fails or is skipped with `--offline`. The same
credential precedence as §3.9 decides the wire: only an explicit `--api-key`
flag overrides a stored OAuth credential and keeps Chat Completions; a provider
env var or config `api_keys` entry does not reroute it.

### 3.6 Native Google Gemini

`src/prov/google.c` is the native adapter, not the `gemini` OpenAI-compatible
preset. It posts to `/v1beta/models/{model}:streamGenerateContent?alt=sse` (the
core substitutes `{model}` in the row path), authenticates with
`x-goog-api-key`, and serializes `systemInstruction`, `contents` (`model`
functionCall parts, `user` functionResponse results), `functionDeclarations`
and `generationConfig` (`maxOutputTokens` plus a `thinkingConfig.thinkingBudget`
for a non-off thinking level). The stream mapper reads text and `thought:true`
parts, functionCall arguments, `finishReason`, `usageMetadata` and
`promptFeedback.blockReason`. Discovery is the native `models` list
(`AGENTC_DISCOVER_GOOGLE`), whose names carry a `models/` prefix that is
stripped.

### 3.7 Model catalog

`src/prov/models.c` ships a small hand-maintained catalog (Anthropic, OpenAI,
Codex and Google entries) with context window, output limit, capability flags
(`reasoning`, `image`; the `image` flag is capability metadata only — no adapter
carries image content, §3.4) and per-MTok rates for `agentc_model_cost()`. Unknown
rates leave `AGENTC_MODEL_RATE_KNOWN` clear and cost returns `-1`;
`AGENTC_MODEL_INPUT_INCLUDES_CACHE` marks providers whose `input` already counts
cache-read tokens (OpenAI and Google prompt token counts), so cache reads are
subtracted before fresh input is billed.

Discovered and extension-contributed models live in a runtime table of 2048
slots (`DYN_MAX`), shared by dynamic and static rows; the cap is sized so a full
pass can hold several providers at the per-provider discovery bound (256).
`AGENTC_MODEL_STATIC` marks a
model contributed by an extension provider: `agentc_model_clear_dynamic()` keeps
it, and `agentc_model_clear_static(provider)` drops it when the provider record
is freed. A full multi-provider discovery pass starts with
`agentc_model_clear_dynamic(NULL)`, which drops every discovered row while
keeping static extension rows, so a large provider's discoveries cannot starve a
provider registered later in the same pass. Static models
carry no cost rate. Registration and lookup: `agentc_model_register_dynamic`,
`agentc_model_register_static`, `agentc_model_find`, `agentc_model_all`
(count-first), `agentc_model_dynamic_count`,
`agentc_model_is_dynamic`/`_is_static`.

`agentc_rate_parse_scaled()` parses a non-negative decimal scalar for the cost
engine; `agentc_model_stage_rate` stages discovered pricing and
`agentc_model_set_rate_override` records a local `pricing.jsonc` override that
wins over any model field.

A curated generated catalog (`runtime/catalog.json` → `tools/gen-catalog.py` →
`build/catalog.c`) is a planned scale-up path; the runtime catalog above is what
ships.

### 3.8 Model discovery and onboarding

`src/core/discover.c` fills the gap between the built-in catalog and the models
a user has:

- `agentc_discover_models(provider, base, key, …)` issues `GET {base}/models`
  for OpenAI-compatible providers and `GET {base}/v1/models` (with
  `anthropic-version`) for Anthropic. Ollama probes the native
  `GET {host}/api/tags` first (which also reports family and parameter size) and
  falls back to `{host}/v1/models`, where `host` is the base with a trailing
  slash and a `/v1` suffix trimmed. The ChatGPT Codex backend has a list at
  `{base}/models` (`AGENTC_DISCOVER_CODEX`, `{models:[{slug,display_name,…}]}`),
  authenticated with the OAuth Bearer + `chatgpt-account-id` + `originator`; it is
  what makes the newer ChatGPT models appear in `/model` and auto-pick. The
  request declares a semver `client_version` (default `1.0.0`, from the
  `openai_client_version` config key or the `OPENAI_CLIENT_VERSION` /
  `AGENTC_CODEX_CLIENT_VERSION` env override): the backend returns only models
  whose `minimal_client_version` is at most it, so this is a capability
  declaration, not a client identity — agentc stays `originator: agentc`, and a
  low value (e.g. its own 0.5.0) silently hides every model. Anthropic's
  `/v1/models` is fetched with `?limit=1000` so a large account is not truncated
  at the default page size. Discovery
  styles are `AGENTC_DISCOVER_DEFAULT`, `_ANTHROPIC`, `_OLLAMA`, `_GOOGLE`,
  `_CODEX`, `_NONE`. A row may supply its own discovery auth through the internal
  `auth_headers` hook; an empty `NONE` override is authoritative and only rows
  without the hook fall back to `authorization: Bearer`. The built-in Anthropic
  and Codex rows use the hook so discovery matches their request auth.
- Callers that already resolved a provider row (the setup/auto-pick path) use
  `agentc_discover_models_ops(ops, …)`; this is what disambiguates the two
  `openai` wires (chat vs Codex) that share a name.
- Responses parse into `AgcDiscovered` and merge into the runtime registry;
  discovered rows are flagged dynamic and can be dropped without touching the
  compiled catalog. The OpenAI-shaped parser also reads gateway metadata:
  `context_length`/`top_provider.context_length`,
  `top_provider.max_completion_tokens`, `architecture.input_modalities` (or the
  input half of `architecture.modality`), and a `reasoning`/`include_reasoning`
  entry in `supported_parameters`; an explicit `vision` wins over the
  architecture fields.
- Results cache to `~/.config/agentc/models-cache.jsonc` (per provider, with a
  fetch timestamp). The cache is consulted first, refreshed when stale, and
  merged provider-wise. A full `--list-models`/`--refresh-models` pass clears all
  discovered rows up front (`agentc_model_clear_dynamic(NULL)`, §3.7) so every
  provider in the pass gets a full listing budget.
- Discovery serves `--list-models`, `--refresh-models`, `agentc setup` and the
  agent's model auto-pick. `--refresh-models` forces a listing even when the
  configured model already resolves. It is skipped with `--offline`, and a
  failed or timed-out probe (2.5 s) is never fatal: the built-in list stands in.
- `src/app/setup.c` (`agentc setup`) offers local Ollama (probing `/api/tags`),
  Ollama Cloud, Anthropic (subscription login or API key), OpenAI and the
  OpenAI-compatible presets, then picks a model from discovery. It writes
  `~/.config/agentc/setup.jsonc` (`{default_provider, default_model}`), loaded
  before `config.jsonc` so an explicit config wins, and merges
  `~/.config/agentc/auth.jsonc` without dropping sibling fields such as a
  subscription `oauth` object. A bare `agentc` run offers onboarding only when
  there is no explicit flag, no setup file, no credential and stdin is a tty.

### 3.9 Auth resolution order

Per request, first match wins (`src/app/setup.c` `agentc_setup_resolve_key()`,
built on `src/core/auth.c` `agentc_auth_key()`):

1. explicit `--api-key` flag (handled by the app);
2. stored OAuth credential for the provider — a stored credential owns the
   provider, ambient env is not consulted behind it, and a failed refresh is an
   error, never a silent fallback;
3. provider env vars (`ANTHROPIC_API_KEY`, `ANTHROPIC_AUTH_TOKEN`,
   `OPENAI_API_KEY`, `OPENROUTER_API_KEY`, `XAI_API_KEY`, `GEMINI_API_KEY`,
   `GOOGLE_API_KEY`, `DEEPSEEK_API_KEY`, `GROQ_API_KEY`, `MISTRAL_API_KEY`,
   `TOGETHER_API_KEY`, `OLLAMA_API_KEY`, …);
4. stored API key from `~/.config/agentc/auth.jsonc` (mode 0600 enforced, with a
   warning if too open); the file is a generic `{provider: {api_key}}` map;
5. config `api_keys` object, last resort for scripts.

Header order (last wins): provider defaults → model headers → config headers →
auth. An extension provider declares how the resolved key is applied
(`NONE`/`BEARER`/`HEADER`); the core writes the header and the key never reaches
the extension.

### 3.10 OAuth subscription logins

`src/core/oauth.c` supports Claude Pro/Max and ChatGPT subscriptions.

- **Flow:** authorization code + PKCE (S256). `agentc login anthropic` prints
  or opens the authorize URL through the platform layer, starts a loopback HTTP
  server on a random `127.0.0.1` socket (reusing the `wire/http.c` parser in
  server mode), validates `state`, exchanges the code at the provider token
  endpoint, and persists the credential.
- **Store:** `~/.config/agentc/auth.jsonc`, written atomically (temp+rename)
  with mode 0600:
  ```jsonc
  {
    "anthropic": {
      "oauth": {
        "access_token": "sk-ant-oat…",
        "refresh_token": "…",
        "expires_at": 1759872000,
        "account_id": "…"
      }
    }
  }
  ```
- **Refresh:** five minutes before expiry, on the main loop; a refresh failure
  surfaces as a provider error with a `/login` hint.
- **Provider specifics:** Claude uses Bearer auth plus its CLI identity headers;
  ChatGPT uses `chatgpt-account-id` from the `id_token` and the Codex Responses
  endpoint. Both are isolated behind the same auth structure.
- **Commands:** `agentc login|logout [provider]`, `/login`, `/logout` in the
  TUI. `auth.jsonc` is never printed in full (only `…last4`).

---

## 4. Tools

### 4.1 Tool interface and registry

`AgcTool` (`include/agent.h`) is the internal tool record. The
`exec(args_json, &is_error, cancel)` entry point is one option, and the appended
fields make a tool either synchronous or asynchronous:

```c
struct AgcTool {
    const char *name, *label, *desc, *params_json;  /* JSON Schema object */
    u32 flags;                                       /* AGENTC_TOOL_* */
    char *(*exec)(const char *, bool *, const volatile bool *);
    void *ud;             /* instance identity; NULL for builtins */
    int   timeout_ms;     /* 0 => AGENTC_TOOL_TIMEOUT_DEF_MS (120000) */
    int (*run)(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error);
    int (*start)(const AgcTool *self, const AgcToolCall *call, AgcJob *job);
    int (*step)(const AgcTool *self, AgcJob *job);   /* 0 running, 1 done, <0 fatal */
};
```

Flags: `AGENTC_TOOL_READONLY`, `AGENTC_TOOL_DESTRUCTIVE`,
`AGENTC_TOOL_SEQUENTIAL`, `AGENTC_TOOL_HIDDEN`, and the core-internal
`AGENTC_TOOL_CORE`. A tool with `run` non-NULL is synchronous; a tool with
`start`/`step` non-NULL (and `run` NULL) is asynchronous. A tool with
`exec` non-NULL and `run` NULL is normalized through the `exec` entry point by
the driver.

Tools are contributed through the extension ABI and collected by the extension
registry (`src/ext/registry.c`); `agentc_ext_tools(out, max)` is the count-first
collection. `builtin-tools`, `mcp` and linked extensions all contribute through
the same registry — a built-in has no separate tool path. The selected tools are
deep-copied into the agent by `agentc_agent_set_tools` (the caller's array and
owned strings may be freed immediately; `ud` and the function pointers are kept
as-is).

The app's active-tool policy (`src/app/policy.c`) applies `--tools` over
`default_tools` over "all", deduplicates repeated names, and in the "all" branch
hides a non-core `AGENTC_TOOL_DESTRUCTIVE` tool unless it is named (core
builtins carry `AGENTC_TOOL_CORE` and stay eligible). One aggregate diagnostic
logs what was hidden. A deferred `mcp__…` name is selected once its server
publishes the tool at a turn-boundary recompose; with MCP disabled it is
reported as disabled rather than promised.

### 4.2 Bounded job driver

`src/core/tools/jobs.{c,h}` owns the batch execution:

- One batch at a time; the driver keeps a static table of
  `AGENTC_TOOL_JOBS_MAX` (32) slots and is not re-entrant.
- Async tools are started first, in source order, so their children overlap the
  slow synchronous tools. Every step is driven by one `os_poll(…, 20)` loop, so
  no call blocks the main loop for more than about 20 ms.
- A per-job absolute monotonic deadline bounds execution; `timeout_ms` of 0
  selects `AGENTC_TOOL_TIMEOUT_DEF_MS` (120 000). A tool may shorten its own
  deadline in `start`.
- The driver owns `job->out`, `job->spill_path` and `job->priv`, closes
  `job->fd` and reaps `job->pid`, and frees everything when the batch returns;
  the completion callback must copy what it needs.
- Output stays inline up to `AGENTC_LIMIT_TOOL_BYTES` (50 000); bytes beyond
  that spill to a 0600 file in `$TMPDIR`. `agentc_tool_finalize_output` applies
  the generic 2000-line/50 KB display cap and truncation notice; a tool that
  composes its own final text (bash) sets `spill_notified` and is left
  untouched.
- A per-tool `AGENTC_TOOL_SEQUENTIAL` flag forces the whole batch sequential
  (`edit` and `write` carry it so parallel writes to one file serialize).
- `AgcJob.cleanup` runs exactly once on fatal, timeout, cancel and release with
  an `AGENTC_JOB_STOP_*` reason, before `priv` is released.
- `agentc_tool_checkpoint()` is the cooperative checkpoint a synchronous `run()`
  body polls; it returns `0`, `-ECANCELED` or `-ETIMEDOUT`.
- `pre_errors[i] != NULL` marks a call as pre-failed by the veto: it is answered
  with that text without running; a NULL tool entry answers
  `error: unknown tool: <name>`.

### 4.3 Built-in tools

`agentc_tools_builtin()` fills the built-in table:

| Tool | Behaviour |
|---|---|
| `read` | whole-file read, 2000-line / 50 KB line-aligned head with an `offset` continuation notice; 64 MiB file cap; binary sniff |
| `bash` | spawns `[$SHELL, -lc, cmd]` on POSIX, `[cmd, /c, cmd]` on Windows; environment extras `AGENTC_SESSION_ID/FILE/PROVIDER/MODEL`; 30-minute hard cap; SIGTERM then SIGKILL on timeout/cancel; exit code and wall time in the result; spill to `$TMPDIR` |
| `edit` | reads the whole file, requires each `oldText` to match exactly once in the original text, applies right-to-left, preserves CRLF/BOM, writes via temp+rename, returns a short hunk summary; `AGENTC_LIMIT_EDIT_BYTES` (8 MiB) cap |
| `write` | creates parents, atomic temp+rename replace; `agentc_tool_write_len` writes exactly the given byte count (embedded NULs preserved) |
| `ls` | sorted listing with a `/` suffix for directories |
| `find` | glob walk, `.gitignore`-aware, bounded to 8000 visited entries, byte-suffix `no matches` |
| `grep` | literal + basic regex, `-i`, context, global `limit`, 500-char line cap |

The built-in scanner implementations are the always-available last resort of the
selectable tool engine (§4.5); the binary never downloads a tool.

### 4.4 Argument handling

Each tool parses its arguments through the shared `AgcToolArgs` DOM
(`src/core/tools/args.{c,h}`): one arenas-per-call parse with NULL-safe string,
integer and boolean getters. Malformed JSON and a non-object root both become
`root == NULL`; each builtin answers with its own exact error text (for example
`error: <tool>: missing required field: <key>`). A malformed or missing argument
always becomes an error **tool result**, never a provider error. The
`parameters_json` field of each tool is the JSON Schema declared to the model.

### 4.5 Tool engine

`grep`, `find` and `ls` keep their names, JSON schemas and normalized output; the
**backend** is a process-wide choice resolved once at `builtin-tools` init in
`src/core/tools/engine.c` (core-internal). Two modes:

- `external` (default): resolve each tool's chain and pin the first available
  rung — `grep`: ripgrep (`rg`) → POSIX `grep` → built-in; `find`: `fd` → POSIX
  `find` → built-in; `ls`: built-in only (there is no portable external `ls`,
  and pi keeps `ls` in-process). The POSIX rungs are POSIX-only; on Windows the
  chains are `rg → built-in` and `fd → built-in`, never `find.exe`/`findstr`.
- `internal`: force the built-ins and never spawn a child. It is the documented
  pin for hermetic/offline runs and byte-identical eval prompts.

Modes are selected by `--tools-engine` > `AGENTC_TOOLS_ENGINE` >
`tools.engine` in config > the `external` default; invalid values are ignored
with a warning.

The ordered descriptor table in `engine.c` is the single source of truth: each
row carries the tool, backend, binary, posix-only flag, whether exit 1 means
"no match", the display name, the description, the `build_argv` builder and the
`run` function. Resolution, selection, argv lookup, logging and reset iterate
the table. Both external tools share one bounded process reader (`proc_spawn` /
`proc_pump`): two pipes, an empty environment, non-blocking polled reads, a
checkpoint per turn, capped stderr, and a group kill plus bounded reap on stop,
cancel or timeout. Resolution uses `os_which`/`os_which_in` (`include/plat.h`),
which returns an absolute path, honors `PATHEXT` on Windows and the execute bit
on POSIX, and never invokes a shell. The chosen rung is logged once, e.g.
`tools: grep -> rg (Rust regex); find -> fd (glob); ls -> built-in`.

Tool names, JSON schemas, argument-validation errors and the model-visible
argument set are identical across backends; only the **description** is generated
per effective backend so the model is told the regex dialect, glob semantics and
file-selection behaviour it actually gets. Invocation is argv-only (no shell):
`rg`/`grep`/`fd` are spawned with `--` before the pattern, and the POSIX `find`
rung anchors a leading `-`/`!`/`(`/`)` path as `./<path>` so a model-supplied
`path` can never become a find action such as `-delete`.

Normalized output: grep is `path:line:text` with the 500-character
(`AGENTC_LIMIT_GREP_LINE`) cap and `...` suffix; find is one path per line
(external rungs bytewise-sorted, the built-in depth-first); `no matches\n` on
empty; `[grep truncated at N matches]\n` and `[find truncated at N paths]\n`;
`limit` is a global cap. `is_error` is set only for a real failure (an
`rg`/`grep` exit of 1 is "no matches", not an error). Both readers enforce an
output-byte safety budget (`AGENTC_LIMIT_TOOL_BYTES`, 50 000) charged over the
emitted size and append a distinct `[grep output truncated]\n` /
`[find output truncated]\n` marker. Every final marker is appended with room
reserved, so the job driver's finalize adds nothing; a cancelled run emits only
partial output and no marker. The POSIX `grep` rung probes `-Z` once and parses
NUL-delimited records when supported; otherwise it falls back to a best-effort
parser that splits `path:line:text` on the first `[-:]<digits>[-:]` run, with
the documented filename limitation. The built-ins are the only backend that
promises the 8000-visited-entry cap and byte-level binary sniffing; external
rungs are bounded by `limit`, the output caps and the tool timeout. Exact parity
is impossible, so the generated description names the effective backend and its
deltas. See `.agents/plans/P5-TOOL-ENGINE.md` for the full backend matrix.

---

## 5. System prompt, resources, trust and config

### 5.1 Prompt sections

`agentc_ext_prompt_build(AgcBuf *out, const AgcTool *tools, size_t ntools)`
renders **four core sections in a fixed order** — `preamble` (identity, date
and cwd), `tools` (one line per active, non-hidden tool), `rules` (global rules
plus tool guidelines), `environment` (cwd + platform from `os_platform()`) —
then appends extension-contributed sections in ascending `priority`.
`agentc_prompt_build()` is the owned-string convenience the agent loop uses.

| Section | Source | Owner |
|---|---|---|
| `preamble` | identity text, date, cwd | core |
| `tools` | one line per active, non-hidden tool | core |
| `rules` | global rules plus tool guidelines | core |
| `environment` | cwd + platform | core |
| `context` | `<project_instructions>` around AGENTS files + `<available_skills>` | `builtin-context` extension |

`--system` hard-replaces the whole render: the agent passes the supplied string
and never calls `agentc_ext_prompt_build`, so extension sections are skipped. In
auto mode (no explicit `--system`) the agent rebuilds the prompt from the live
tool table before every turn, so a turn-boundary recompose reaches the next
request. A `before_agent_start` `systemPrompt` override replaces the effective
prompt for the rest of the run and is re-published to the extension context.

### 5.2 Context, skills, prompts, themes

- **Context files:** `AGENTC.md` / `AGENTS.md` / `AGENTS.override.md` /
  `CLAUDE.md` from the config dir and every ancestor of cwd (nearest last),
  loaded by `builtin-context`; appended as `<project_instructions>` sections.
- **Skills:** `~/.config/agentc/skills/**/SKILL.md` plus project and
  `resources_discover` roots. Frontmatter carries `name`/`description` only; the
  body loads on demand when the model reads the file. `/skill:<name>` reads the
  body, strips frontmatter, caps the submit at `AGENTC_SKILL_SUBMIT_MAX`
  (256 KiB), appends trailing arguments and submits through the normal path.
- **Prompt templates:** `prompts/*.md` in the config dir, the trusted project
  `.agentc/prompts` and each `resources_discover` prompt root. The stem must be
  `[a-z0-9._:-]{1,64}`; frontmatter may carry `description`/`argument-hint`. The
  template registers in the one invocable-prompt registry
  (`src/core/prompts.h`), and `/name` (or RPC `prompt_template`) expands
  `$1..$n`, `$@`/`${@}`/`${ARGUMENTS}` and `${N:-default}`, then submits the
  result. A later same-name registration wins; a built-in slash command shadows a
  same-named prompt, and a prompt shadows a same-named extension command.
  `AGENTC_PROMPTS_MAX` is 512 total registrations and `AGENTC_TEMPLATES_MAX` is
  64 per scan.
- **Themes:** `themes/<name>.jsonc` (stem `[A-Za-z0-9._-]{1,64}`,
  `AGENTC_THEMES_MAX` 64 per scan) from the config dir, the trusted project
  `.agentc/themes` and each `resources_discover` theme root. `/theme <name>`
  applies one at runtime; the config `theme` key (default `system`) is the
  startup theme, and `system` reads `$COLORFGBG` (dark when absent). See
  `.agents/design/40-tui.md`.

### 5.3 Project trust

Project `.agentc/*` resources load only when trusted. The `project_trust`
override handler is consumed before the saved verdict
(`src/app/project.c`, `.agents/design/30-extensibility.md`): an explicit
`--approve`/`--no-approve` wins and skips the handler; `yes`/`no` decides;
`undecided` falls back to the saved decision; `remember:true` saves it; a
blocked or malformed handler is fail-closed. A project that becomes trusted only
through the hook cannot retroactively load its `.agentc/config.jsonc` in the
same run (a warning says so; restart to load it).

### 5.4 Configuration

`~/.config/agentc/config.jsonc` and project `.agentc/config.jsonc` are JSONC
(VS Code dialect: `//`, `/* */`, trailing commas). The same parser and writer
handle every config surface, so a config value pastes straight into a request or
session line; unknown keys are preserved as strings for forward compatibility.

```jsonc
{
  "default_provider": "openai",
  "default_model": "gpt-5",
  "default_thinking": "medium",
  "default_tools": ["read", "bash", "edit", "write"],
  "theme": "system",
  "shell": "auto",
  "tools": { "engine": "external" },

  "providers": {
    "openrouter": { "base_url": "https://openrouter.ai/api/v1" },
    "<extension-provider-id>": { "base_url": "https://gateway.example/v1" }
  },

  "compaction": { "enabled": true, "reserve_tokens": 16384, "keep_recent_tokens": 20000 },
  "retry": { "max_attempts": 5 },

  "api_keys": {},

  "extensions": { "disabled": [] },

  "mcp": {
    "servers": {
      "files": { "command": "npx", "args": ["-y", "@scope/server"] }
    }
  }
}
```

`providers.<id>`/`api_keys.<id>` are generic: any id (an extension provider or a
user gateway) gets a base-URL override and a config key, capped at
`AGENTC_CONFIG_GENERIC_MAX` (32) entries each with ids up to 64 bytes, user
scope only; the named presets win over a generic duplicate. An id the registry
does not know whose `providers.<id>.base_url` is set materializes an
OpenAI-compatible gateway row (§3.2), so `--provider <id>`, `--list-models` and
auth resolution work without a linked extension; an unknown id with no base URL
stays unknown. `tools.engine` selects the core-tool backend (§4.5). Project
config is subject to the trust rules above.

`~/.config/agentc/trust.jsonc` stores project trust decisions with canonical
paths.

---

## 6. Sessions

### 6.1 Files

`$XDG_DATA_HOME/agentc/sessions/--<sanitized-cwd>--/<timestamp>_<8hex>.jsonl`
(config `session_dir`, env `AGENTC_SESSION_DIR`, `--session-dir`). Files are
created mode 0600 and are append-only, with one write choke point. A failed
append rolls the file back to its previous offset with `os_ftruncate` and marks
the session degraded (read-only) with a log.

The first line is a header:
`{"type":"session","version":1,"id":"…","timestamp":<unix_ms>,"cwd":"…"}`.
Messages follow as serde-native entries: snake_case fields and `{"type":"…"}`
tagged objects, so every record round-trips through `serde_json` in a Rust tool
with no glue code. Tool-call `arguments` are stored as a JSON **string** holding
the raw args text (the `--mode json`/RPC event payloads instead carry the raw
JSON value; §7.1). A crash-truncated final line is repaired with a newline
before the next append so the next record cannot grow onto the partial one.

### 6.2 Entries and version gate

The session writes `session`, `message` and `compaction` entries plus generic
`custom` records appended with `agentc_session_append_raw()`. Tree links use
`id`/`parent_id`. On open and replay the header `version` is read strictly; a
`version` greater than 1 is refused with `EPROTO` rather than reinterpreted.
Malformed lines are skipped with one aggregate warning.

### 6.3 Resume and continue

- `--continue` and `--resume` open the in-TUI session picker before the first
  prompt on an interactive run (newest first, labelled with the opening line;
  Escape keeps the newest session the mode already opened); an explicit prompt
  skips the picker and runs immediately against the session the mode opened, and
  a scripted, print or non-tty run keeps resuming the newest session for cwd.
  `--session <id|path>` opens a named file; `--list-sessions` lists them (newest
  first). The picker's
  metadata comes from `agentc_session_age_label()` + `agentc_session_summary()`.
  In the TUI, `/resume` and `/continue` open the same picker at any point and
  `agentc_mode_resume_session()` performs the swap; a mid-run request aborts the
  turn first so no tool or request is live when the session changes.
- In the TUI, `/new` calls `agentc_mode_new_session()` — the same swap RPC's
  `new_session` command uses (close the file, create and rebind a new one, clear
  the agent transcript, emit `session_start`, write the new header) — then clears
  the view. `/compact` runs `agentc_agent_compact()` and the checkpoint is
  persisted through the compaction event. Both refuse while a run is in flight.
- `agentc_session_load_messages()` replays a file into a transcript;
  `agentc_agent_load()` deep-copies it into an idle agent before the first
  submit.
- `/fork`, `parent_session` and `/tree` navigation are not implemented.
- `--no-session` sets `memory_only` and disables persistence.

### 6.4 Persistence ownership and the new-session switch

The mode context (`src/app/mode.c`) is the single persistence owner: transcript
messages are persisted through its flush observer, so a rebuild cannot leave a
stale observer behind a front-end-local structure. `new_session` first cancels
through `session_before_switch` (`{"reason":"new"}`, fail-closed: a blocked,
failed or malformed handler, or `{"cancel":true}`, cancels before any file is
touched). An allowed switch closes the previous session, creates the
replacement, rebinds
the extension context/entry sink and the observer, and emits
`session_start{reason:"new", previous_session_file}`. No `session_shutdown` is
emitted, because extensions are process-lifetime. The startup path uses the same
payload with `reason:"startup"` or `"resume"`.

### 6.5 Compaction replay

A `compaction` entry carries the summary and `kept_messages`. On replay, the
summary is installed as a user message in front of the retained tail: the replay
keeps the last `kept_messages` messages that were appended to the file before
the compaction record, so a `--continue` never silently drops the tail. Tool
call/result pairing is reconciled after replay.

---

## 7. Modes, flags, status and discovery

### 7.1 Modes

| Mode | Behaviour |
|---|---|
| TUI (default) | full-screen or inline renderer (`.agents/design/40-tui.md`), stdin raw mode |
| `-p`/`--print` | run one prompt, stream assistant text to stdout, exit 0/1 |
| `--mode json` | JSONL of the session header plus agent events (stable documented schema) |
| `--mode rpc` | JSONL commands in, responses and events out (`prompt`, `prompt_template`, `abort`, `get_state`, `set_model`, `set_thinking_level`, `get_available_models`, `compact`, `bash`, `new_session`, `get_messages`) |

Subcommands: `agentc setup [--offline]`, `agentc login|logout [provider]`.
`agentc update`, `agentc config` and `agentc mcp` are unimplemented and exit 2
with `unknown argument`.

In `--mode json` and `--mode rpc` message payloads, a tool-call block's
`arguments` is the raw JSON value the model emitted, whereas the session JSONL
stores the same field as a JSON string (§6.1); the `tool_execution_start` event
carries it as `args`.

### 7.2 Flags

`-p/--print PROMPT`, `--mode json|rpc`, `--tui-mode MODE`, `--provider P`,
`--model provider/id[:thinking]`, `--api-key K`, `--base-url URL`, `--system S`,
`--thinking off|low|medium|high`, `--max-tokens N`, `--continue`, `--resume`,
`--session PATH|ID`, `--session-dir DIR`, `--no-session`, `--tools a,b`,
`--no-tools`, `--tools-engine external|internal`, `--no-mcp`,
`--approve`/`--no-approve`, `--list-sessions`, `--list-models [S]`,
`--list-extensions`, `--refresh-models`, `--sync-pricing`, `--offline`,
`--record FILE`, `--dump-wire`, `--insecure`, `--version`, `--help`.

`--provider` accepts any live provider name, including an extension provider;
extensions compose before provider resolution and before onboarding, so
`--provider <ext>`, `--list-models`, the setup picker and model auto-pick include
extension rows. An unknown id with a configured `providers.<id>.base_url`
resolves to an OpenAI-compatible gateway row instead of failing (§3.2).
`--tools` is strict: an unmatched name exits 2, except a deferred
`mcp__<server>__<tool>` name and the three resource-tool names
(`mcp_list_resources`, `mcp_list_resource_templates`, `mcp_read_resource`),
which resolve once their server publishes them at a turn-boundary recompose.
`--list-extensions` prints one `state name version` line per registered
extension in registration order (states `pending`/`loaded`/`failed`/`skipped`)
and marks a dynamic row `(dynamic)`.

`--record FILE` writes the raw wire stream (request head, response head, SSE
body) captured as it is sent and received; `--dump-wire` writes the same stream
to stderr when a print-mode run finishes. There is no runtime replay flag; the
offline tests link the scripted `src/net/mock.c` backend.

### 7.3 Status line

The TUI status row is a provider-built segment list, not hardcoded chrome
(`include/status.h`). The built-in model/thinking/tokens/cost and state/ready
segments register through the same `agentc_status_register()` table an extension
uses. `agentc_status_snapshot()` validates and copies segments, sorts by
(slot, priority, registration order) and returns them; the TUI rebuilds its
snapshot only when `agentc_status_version()` changes, so an idle status row runs
no provider code. Caps: `AGENTC_STATUS_MAX_SEGMENTS` 24, eight segments per
provider, a 2 ms budget per provider (a provider over budget is disabled for the
process), 192-byte text, and a 1024-byte arena. A control byte in segment text
is dropped with a log. `host->set_status(key, text)` folds into an auto-clearing
segment keyed by (owning extension, key); eight live keys are allowed.

### 7.4 Model and thinking sinks

The extension host carries `set_model`/`set_thinking` service sinks and the
`model_select`/`thinking_level_select` observe points. In the app, `set_model`
switches the model (same provider); a provider switch is refused `-ENOSYS`
(provider switching stays with the RPC command's idle rebuild). `set_thinking`
accepts `off|low|medium|high` and emits `thinking_level_select` on a changed
level. `agentc_agent_set_model()` emits `model_select` on an id change;
`agentc_agent_set_thinking()` clamps to 0..4 and emits on a numeric change.
