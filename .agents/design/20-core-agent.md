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
`set_retry`, `set_record`, `set_insecure`, `set_token_budget`), the observer that persists
transcript messages, the tool-veto and recompose callbacks, and the transcript
accessors. `agentc_agent_load()` deep-copies a replayed transcript before the
first submit.

The loop does not use a global state enum; it is a nested loop inside
`agentc_agent_submit` with a retry loop around each request and an outer settle
loop. A run ends through one shared finish tail (recompose → `agent_end` →
`agent_before_settle` → `agent_settled`).

The front end drives cooperative pumping with `agentc_pump_install()` and
`agentc_pump()`; long-running tools call the pump so the UI can repaint and read
input while work proceeds. `agentc_pump()` is the single runtime driver:
`agentc_runtime_tick()` drains the message bus and then ticks the actor
registry (P6-B, RT R-D17), so a child's notify/input can never deadlock a
foreground wait.

P6-B adds the agent-actor seam: `agentc_agent_bind_actor()` registers the root
agent as an actor (children land with P6-D), and `agentc_agent_submit_auto()`
coalesces due queue items into the next submission. `agentc_agent_turn_no()`
and `agentc_agent_steerable()` expose the turn counter and the non-steerable
hold. The agent's `AgcActor` row carries `turn_seq`/`steerable`.

**Token budget (P6-SURFACE §18 #23).** `AgcAgentSpec.token_budget` (and
`agentc_agent_set_token_budget()`) sets a soft cap on the agent's model tokens
**per submitted turn**: `usage.input + output + cache_read + cache_write`, summed
over every successful step of the turn and read back through
`agentc_agent_tokens_spent()`. `0` means no cap and inherits the parent's value
for a child; a negative value is an explicit uncapped override. The cap is
enforced **between** model steps (at the top of the step loop, after the previous
step's whole tool batch has been answered), so it never strands a `tool_call`
without a result: the runtime emits `actor_notify` once per turn (payload
`{"reason":"token_budget","budget":N,"spent":M}`) and settles the turn with the
last assistant message as its output. A run that hit the cap does not
auto-continue, and the spend resets at the next submit, so reaching the cap never
bricks the agent. `agentc_agent_token_budget()`/`agentc_agent_tokens_spent()` and
the matching `AgcAgentInfo` fields expose the effective cap and running total.

### 2.2 One run

1. **Input hook.** The `input` override may handle, transform or block the user
   text. A blocked/failed handler sets the last error, emits `AGENTC_EV_ERROR`
   and returns `-1`; a handled input returns `0` without a run.
2. **System prompt.** The per-run override is reset; the effective prompt is
   built or refreshed and published to the extension context; the
   `before_agent_start` override may replace it for the run.
3. **Agent start.** `AGENTC_EV_AGENT_START` is emitted and the user message is
   appended and passed to the observer.
4. **Step loop** (repeats until a terminal stop or the continuation budget is
   spent):
   - refresh the auto system prompt, emit `AGENTC_EV_STEP_START`;
   - check the compaction threshold before the request;
   - push an assistant placeholder and emit `AGENTC_EV_MSG_START`;
   - run the retry loop for the request (build → transport → SSE map →
     `finish`), recording usage and `cost_micro`;
   - run `message_end`, emit `AGENTC_EV_MSG_END`, persist through the observer;
   - if the turn produced tool calls with stop `TOOLUSE`, prepare and run the
     batch (see §4.2), apply `tool_result` overrides, and append one
     `AGENTC_ROLE_TOOL` result per call **in source order**;
   - run `step_end`, emit `AGENTC_EV_STEP_END`, and decide whether to continue.
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

`AGENT_CONTINUATION_MAX` is 32: `step_end.continue` and
`agent_before_settle.continue` share that per-submit budget.

**P6-B safe points and steering.** When the agent is bound to an actor, the loop
drains its mailbox at three safe points: before the request, after `message_end`,
and after each tool batch. A drain may deliver CONTROL messages (cancel/freeze/
settle) and queue the INPUT items. Due BOUNDARY items (`priority >= 0`) are
coalesced into one user message and appended at the pre-request/post-tool safe
points (same turn, next request); a due PREEMPT item (`priority < 0`) arriving
mid-stream aborts the SSE read (`AGENTC_EV_MSG_RESET`), drops the partial
assistant and re-issues with the whole due set. During a non-steerable step
(`compact`) the due set is held; an item carrying
`AGENTC_Q_F_CANCEL_NONSTEERABLE` cancels the turn so it is resubmitted. The
turn counter increments once per submission.

An enqueue carrying `AGENTC_Q_F_DEDUPE` with a non-empty `client_id` is
idempotent: if an item with the same `client_id` is still queued (not yet
delivered), it is not added again, `agentc_queue_add` returns the existing
item's id, and no input event or steer request fires. `agentc_queue_deduped(id)`
counts the drops; the `queue`/`agent` tools take `client_id` + `dedupe` and
return `{"id":N,"deduped":true}` on a drop (RPC forwards the arguments
verbatim).

A `tool_call` veto may set `terminate` together with `block`; only when **every
call in the assistant message** was blocked with `terminate` does the
submission settle after the batch (the assistant keeps stop reason
`tool_use`). A mixed or normal call keeps the loop; cancellation is checked
first and wins; an explicit `step_end.continue` still re-enters once under the
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
`step_start`, `message_start`/`message_update`/`message_end`,
`tool_call`/`tool_execution_start`/`tool_execution_end`/`tool_result`,
`step_end`, `agent_end`, `agent_before_settle`, `agent_settled`,
`before_provider_headers`, `before_provider_request`, `after_provider_response`,
`provider_stream_event`, `session_before_compact`/`session_compact`/
`session_compact_failed`, `model_select` and `thinking_level_select`. The
`tool_call` veto is also installed directly as `AgcToolVetoFn`; the recompose
hook re-runs the app's active-tool selection.

P6-B adds the universal `agentc_hook` pipeline (`include/hook.h`,
`src/core/hook.c`) alongside the extension bus. Ten stages — `turn`, `step`,
`input`, `prompt`, `message`, `output`, `tool`, `signal`, `event`, `lifecycle`
— are wired into the loop and the bus; `agentc_hook_wants(stage)` is the
hot-path guard, so an unwatched stage is zero work and the legacy extension
bus stays byte-exact. Handlers chain by `(order, registration seq)` and may
edit, replace or consume the message or defer. `src/core/signal.c` fires
`signal` before dispatch and `src/core/actor.c` fires `lifecycle` from
`actor_event`.

### 2.6 Agent events

`include/agent.h` declares the front-end event enum:

```text
AGENTC_EV_AGENT_START, STEP_START, MSG_START, TEXT_DELTA, THINK_DELTA,
TOOL_ARGS_DELTA, MSG_END, TOOL_EXEC_START, TOOL_EXEC_END, STEP_END,
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

A curated, generated catalog is a planned scale-up path; the runtime catalog
above is what ships.

**Virtual models (P7-W4).** A **virtual model (alias)** is declared by a
top-level `models.<alias>` config object: `{"provider":"…","model":"…"}`
(`agentc_config_model_routes()` and the `models` block). Each alias registers an
`AgcModel` row carrying
`AGENTC_MODEL_VIRTUAL`; the concrete target is stored authoritatively in the
appended `route_provider`/`route_model` fields (`agentc_model_register_virtual`),
with `route` kept only as the owned display string (so a provider id may itself
contain `/`). It never shadows a catalog or dynamic id (first-wins is symmetric:
`agentc_model_register_dynamic`/`_static` refuse an id held by an alias) and
survives `agentc_model_clear_dynamic()`; `agentc_model_clear_virtual()` drops the
aliases, and `agentc_config_load` clears then republishes them so a removed alias
does not linger (a config (re)load is thus a process-visible side effect on the
process-global model registry; `agentc_model_reset()` drops them, 70-sdk.md §2).
At the top of each model step (and in compaction) the agent
resolves the alias to an effective `{provider, model, base_url, api_key}` for
that request only — `a->prov`/`a->model` keep the alias as the transcript
identity, so no provider rebuild happens. An unknown target provider or a broken
route fails the step closed (`AGENTC_STOP_ERROR`, no request). A route emits
`AGENTC_EV_MODEL_ROUTE` (observe point `model_route`) and a `custom type:"model"`
session record for replay. `AgcDecisionRecord.provider` is applied only when it
names the caller's current provider; a cross-provider decision route remains a
recorded decision (the decider returns no next message rather than sending a
model id to the wrong wire). A routed target resolves its endpoint and
credential through a **per-agent** route-target resolver — the appended
`AgcAgentSpec.route` (`AgcRouteSeam`: `provider`/`provider_ud` and
`target`/`target_ud`), installed by the mode layer and inherited by children;
`route_resolve()` selects it from the agent and never reads a process global,
so a background agent of one session can never resolve through another
session's context. The resolver mirrors the primary order (config `base_url`;
OAuth > provider env > `auth.jsonc` > config `api_keys`); a same-provider
routed alias uses the agent's own effective endpoint and credential, the
agent's own credential is never sent to a different routed host, and an absent
key for a routed target fails closed. The per-agent owner
(`AgcAgentSpec.owner`) is what the session observers use to persist a
queue/model-route record, so route persistence follows the emitting agent too.
The app auto-pick treats an alias as self-describing
(`agentc_model_find_virtual`), so it neither runs discovery for it nor registers a
duplicate dynamic row (main.c).

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
  low value (e.g. its own 0.6.0) silently hides every model. Anthropic's
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
- Results cache to `~/.config/abird/agentc/models-cache.jsonc` (per provider, with a
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
  `~/.config/abird/agentc/setup.jsonc` (`{default_provider, default_model}`), loaded
  before `config.jsonc` so an explicit config wins, and merges
  `~/.config/abird/agentc/auth.jsonc` without dropping sibling fields such as a
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
4. stored API key from `~/.config/abird/agentc/auth.jsonc` (mode 0600 enforced, with a
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
- **Store:** `~/.config/abird/agentc/auth.jsonc`, written atomically (temp+rename)
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

The app's active-tool policy (`src/core/policy.c`) applies `--tools` over
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
| --- | --- |
| `read` | whole-file read, 2000-line / 50 KB line-aligned head with an `offset` continuation notice; 64 MiB file cap; binary sniff |
| `bash` | spawns `[$SHELL, -lc, cmd]` on POSIX, `[cmd, /c, cmd]` on Windows; environment extras `AGENTC_SESSION_ID/FILE/PROVIDER/MODEL`; 30-minute hard cap; SIGTERM then SIGKILL on timeout/cancel; exit code and wall time in the result; spill to `$TMPDIR` |
| `edit` | reads the whole file, requires each `oldText` to match exactly once in the original text, applies right-to-left, preserves CRLF/BOM, writes via temp+rename, returns a short hunk summary; `AGENTC_LIMIT_EDIT_BYTES` (8 MiB) cap |
| `write` | creates parents, atomic temp+rename replace; `agentc_tool_write_len` writes exactly the given byte count (embedded NULs preserved) |
| `ls` | sorted listing with a `/` suffix for directories |
| `find` | glob walk, `.gitignore`-aware, bounded to 8000 visited entries, byte-suffix `no matches` |
| `grep` | literal + basic regex, `-i`, context, global `limit`, 500-char line cap |
| `process` | process-actor adapter (P6-A): `spawn`/`write`/`read`/`resize`/`kill`/`freeze`/`resume`/`list` over `agentc_process_*` |
| `queue` | P6-B adapter over the unified queue (`add`/`list`/`update`/`delete`/`reorder`/`start`/`pause`/`resume`/`clear`); registered `AGENTC_TOOL_CORE \| AGENTC_TOOL_HIDDEN`, so it is executable but not declared to the model until a front end enables it |

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

### 4.6 Process actors (P6-A)

`include/actor.h` + `src/core/actor.c` add the runtime **actor registry**: a
bounded table (`AGENTC_ACTOR_MAX`, 64) of addressable units of work with the
kinds `AGENTC_ACTOR_AGENT` and `AGENTC_ACTOR_PROCESS`. P6-A implements the
process kind (agent actors land with P6-D); a terminal is just a process actor
with `tty = true` (R-D24). The registry owns each row's label and, for a
process, the `AgcProcess` payload; `agentc_actor_tick()` is the only place a
process is advanced and is called from `agentc_pump()` and the agent loop's
poll slices. Lifecycle is `STARTING → RUNNING → STALLED` (advisory, once per
episode) with `FROZEN` as a non-terminal pause and `SETTLED | FAILED |
CANCELLED` terminal. An `agentc_atexit` chain (base/log.c) reaps live actors on
`agentc_die`.

`src/core/process.c` owns spawning and I/O. Pipe mode merges stdout+stderr and
has no stdin; PTY mode (`os_pty_open`) gives a writable master used for both
directions. Output lands in three sinks: a bounded `pending` buffer drained by
`agentc_process_read`, a bounded `tail` ring (`AGENTC_PROC_TAIL_BYTES`) for
UI/list, and — once the stream exceeds the tail — a 0600 spill file holding the
full stream (`agentc_process_spill_path`). Cancellation is the one model:
`agentc_actor_cancel` sends SIGTERM on the group, starts a `grace_ms` timer, and
the tick escalates to SIGKILL; `cancelled` is emitted exactly once. Freeze
(`agentc_actor_freeze`/`_resume`) SIGSTOPs/SIGCONTs the group and suspends the
deadline, grace and stall timers, shifting them forward on resume (R-D18). The
one factory is `agentc_actor_spawn(&AgcActorSpec)` (R-D25).

The model-facing **`process`** tool is a thin adapter over the registry
(`spawn|write|read|resize|kill|freeze|resume|list`); its spawn returns
`{"actor_id":N}` and the process survives the batch and the step (R-D26).
`bash` keeps its exact legacy output and job-driver scheduling, but now spawns
its child through `agentc_actor_spawn` and drains it via the actor
(`AGENTC_PROC_F_NO_SPILL` leaves the job's own 0600 spill authoritative). Its
foreground/background split is unchanged: a foreground `bash` job stops when
the direct child exits (draining what is buffered, per the legacy rule) and
releases the actor; an unfinished background spawn keeps the stable actor id.

A successful `agentc_process_resize` updates the PTY actor's `cols`/`rows` and
emits exactly one `AGENTC_EV_ACTOR_RESIZED` through the shared `actor_event`
path, carrying the geometry in `AgcActorEvent.cols`/`.rows` and firing the
`actor_resized` observe hook (P6-SURFACE §5.2). A non-TTY actor refuses with
`-ENOTTY` and emits nothing.

### 4.7 Sub-agents, the agent tree and the decision seam (P6-D)

P6-D unifies root and child into **one type, one constructor** (R-D1).
`agentc_agent_create(const AgcAgentSpec *)` (`src/core/agent.c`) is the only
startup path: it resolves provider/model/thinking/transport **directly** from
the parent unless the spec overrides (R-D2; there is deliberately no
`default_subagent_model`), resolves the envelope by inheritance or
`agentc_trust_narrow` (a widening request fails before any actor is created),
deep-copies and envelope/depth-filters the tool table, registers an agent actor
row with `parent`/`depth`, and either submits the initial task (`wait:true`) or
marks it runnable (`wait:false`). `agentc_agent_new` is a thin wrapper and
`src/app/mode.c`'s `mode_configure_agent`/`agentc_mode_rebuild_agent` build a
spec instead of calling the old constructor. The sub-agent defaults come from
`trust.subagent.*` in `config.jsonc` (`inherit`, `max_depth`) and are applied by
`apply_config` to the formerly process-global accessors. A child inherits a bounded
(`AGENTC_AGENT_CTX_KEEP`) parent context only under
`AGENTC_AGENT_F_INHERIT_CONTEXT`; otherwise its transcript holds just the task
plus the auto system prompt.

The actor registry **is** the tree. `agentc_agent_list/info/find/parent/wait/
send/cancel/result` (`src/core/agent_spawn.c` + `agent.c`) project agent rows.
Ownership is explicit: the root `AgcAgent` is owned by the app
(`owned_by_actor = false`), a child by its registry row (`owned_by_actor =
true`); `agentc_actor_release` recurses descendants first and
`agent_actor_release` frees a child exactly once, clearing `actor->agent`. A
waiting parent services the bus: `agentc_agent_wait` pumps
`agentc_runtime_tick` each iteration (R-D17).

The model-facing **`agent`** op tool (`spawn|wait|send|interrupt|freeze|resume|
result|list`) reads the calling actor from the appended `AgcToolCall.actor_id`,
enforces the depth ceiling (`depth_limit`), the R-D9 controller authority
(`authority_denied`) and the envelope (the call flows through the P6-C `tool`
gate), and returns a child's final text as an ordinary tool result.

Decisions are **hook handlers, not stages** (R-D20/R-D23).
`include/decide.h` and `src/core/decide.c` provide the decider registry, the
multi-effect `AgcDecisionRecord` parser/validator and `agentc_decide` (a
`hook_branch` sugar). A controller handler on `tool`/`call` returns the call message
(`allow`/`narrow`/a replacement `agent op:spawn` call) or `NULL` (deny) and
applies its non-call effects only through primitives (`message_send` of
CONTROL `SET_MODEL`/`SET_THINKING`/`SETTLE`/`CANCEL`, `INPUT`, or a branch for
approval). `agentc_hook_branch` builds an ephemeral child agent on the shared
transport. A `WAIT` child runs inside `agentc_agent_create` and the verdict is
resolved inline (byte-for-byte the historical behavior); a `BACKGROUND` branch
is a **true continuation**: the chain parks on `agentc_runtime_defer` and
`agentc_runtime_tick` resumes it (`agentc_hook_resume`) once the branch actor
reaches terminal, then releases it. The runtime keeps a bounded
`AGENTC_RUNTIME_MAX_DEFERS` continuation table and fires parked continuations
after the actor tick; a full table fails closed rather than running unbounded.

Decider backends (P6-DECIDERS §5): `LOCAL` returns `command`/a canned verdict;
`PROGRAM` runs `spec->command` as a BIDI **process actor**, writes the decision
envelope to stdin and reads one response line (bounded by
`AGENTC_DECIDER_TIMEOUT_MS`), parsing it with `agentc_decision_parse`;
malformed/absent/timeout output becomes a fail-closed `deny`. `MODEL` branches
to an ephemeral child; `MCP` currently fails closed with `deny` (no synchronous
MCP invocation is reachable from core). `AgcExtHost.decider_register`
(HAS-gated, JSON descriptor) registers any of these through the stable ABI.

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
| --- | --- | --- |
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
- **Skills:** `~/.config/abird/agentc/skills/**/SKILL.md` plus project and
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
(`src/core/project.c`, `.agents/design/30-extensibility.md`): an explicit
`--approve`/`--no-approve` wins and skips the handler; `yes`/`no` decides;
`undecided` falls back to the saved decision; `remember:true` saves it; a
blocked or malformed handler is fail-closed. A project that becomes trusted only
through the hook cannot retroactively load its `.agentc/config.jsonc` in the
same run (a warning says so; restart to load it).

### 5.4 Configuration

`~/.config/abird/agentc/config.jsonc` and project `.agentc/config.jsonc` are JSONC
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

**Custom base URL and ad-hoc provider (P7).** `--base-url U` overrides the
selected provider's endpoint (and works for `openai`, the Ollama presets and any
gateway); combined with an unknown `--provider <id>` it materializes an ad-hoc
OpenAI-compatible gateway with no config entry, so
`agentc --provider my-gw --base-url https://host/v1 --api-key … -p …` runs
directly. `agentc setup` offers a **Custom endpoint** choice that prompts for a
provider name, a base URL and an optional key, then persists
`providers.<id>.base_url` into the agentc-owned `setup.jsonc`
(`agentc_config_setup_set_provider`), which the loader reads with user scope
(same generic-provider rules). `agentc_config_save_setup` is merge-preserving so
the custom entry survives the default write. `OLLAMA_HOST` and
`providers.ollama.base_url` remain the ways to point the Ollama preset at a
non-default host.

`~/.config/abird/agentc/trust.jsonc` stores project trust decisions with canonical
paths.

### 5.5 Trust envelopes and sandbox (P6-C)

`AgcTrustEnv` (`include/trust.h`) is the per-agent security boundary:
`access` (`read-only|read-write`), `boundary` (`none|workspace|strict`),
`approval` (`auto|ask|policy`), `network` (`deny|allow|allowlist`), owned
`roots`/`hosts`, a `policy_id` and a flags word (PTY/background/subagent). The
built-in default is `read-write/workspace/auto/deny` with `roots=[cwd]`,
preserving the historical behavior. `agentc_trust_load` resolves CLI > trusted
project > user > default; a widening request is denied by default
(`trust.widen=deny`) or clamped (`clamp`).

Enforcement is four layers, each of which can only tighten:

1. **Declaration** (`src/core/policy.c`): an RO envelope hides `write`/`edit`
   from the model-facing tool table; a net-deny envelope hides network tools.
2. **Static policy** (`agentc_trust_decide`): RO denies every mutation, a write
   outside the roots is denied under `auto`/`strict`, asked under `ask` and
   delegated under `policy`; strict reads are limited to the roots; network
   tools honor `network`/`hosts`.
3. **Hook/delegation** (`tool_approval`): the §7.1 request/response JSON with
   three backends (interactive UI, external program, extension hook) and
   `once|step|session|project` memoization in `agentc_trust_scope_*`.
4. **Kernel enforcement**: `os_spawn_*_sandboxed` applies `os_sandbox_apply` in
   the child between fork and execve; Linux uses Landlock (fs writes) plus a
   seccomp filter (AF_INET/AF_INET6) and fails closed (`-ENOSYS` denies the
   spawn). In-process `read`/`write`/`edit` re-check the resolved path with
   `agentc_trust_path_ok` after `os_realpath`; `ls`/`find`/`grep` run the same
   strict-root check so no read tool can escape a `strict` boundary. Every
   process spawn uses its per-call envelope, falling back to the ambient
   `agentc_trust_current()` so a direct/RPC spawn is confined too.

```jsonc
"trust": {
  "access": "read-write",
  "sandbox": "workspace",
  "approval": "auto",
  "network": "deny",
  "widen": "deny",
  "roots": ["."],
  "approval_timeout_ms": 120000
}
```

CLI: `--access`, `--sandbox`, `--approval`, `--policy`, `--network`. Events
`AGENTC_EV_TRUST_SET`/`APPROVAL_REQUEST`/`APPROVAL_RESULT`/`SANDBOX_DENIED` and
the `trust_select`/`tool_approval`/`sandbox_denied`/`policy_rules` hooks carry
the state to front ends/extensions; approvals are audited as a
`custom type:"approval"` session record (input hashed, never raw). Project-scope
approval persistence lives in a separate `approvals.jsonc` (C-D11).

---

## 6. Sessions

### 6.1 Files

`$XDG_DATA_HOME/abird/agentc/sessions/--<sanitized-cwd>--/<timestamp>_<8hex>.jsonl`
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

**P6-B queue records (planned).** SIG §5.6/C-D9 call for each queue mutation to
be appended as a `custom type:"queue"` record from the `AGENTC_EV_ACTOR_QUEUED`
observer and restored + revalidated on resume. The observer and the event are in
place; the session-side append/restore is not yet wired (see the P6-B as-built
deviation note).

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
- **Fork and tree (P7).** `/fork` calls `agentc_mode_fork_session()`: it creates a
  new session whose header carries `parent_session` = the current id, copies the
  live transcript into the new file so the fork is self-contained, keeps the
  agent transcript (unlike `/new`), and emits `session_start{reason:"fork"}`. It
  runs the same `session_before_switch` veto with `reason:"fork"`; the TUI refuses
  `/fork` while a run is in flight (the mode function itself has no busy check,
  like `/new`). A forked file gets a fresh id and copies the `message` transcript
  plus the parent's `custom` records (agent tree, queue snapshots, notices,
  routes), so it is a self-contained point-in-time snapshot. `/tree` opens a picker over
  `agentc_session_tree()` (the fork forest: `{path,id,parent_id,timestamp}` per
  session, indented by lineage depth); selecting a node resumes it through the
  same path as `/resume`. `agentc_session_parent()` reads a session's parent id;
  an absent `parent_session` header key means a root session (no version bump:
  unknown keys are ignored).
- `agentc_session_load_messages()` replays a file into a transcript;
  `agentc_agent_load()` deep-copies it into an idle agent before the first
  submit.
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
`session_start{reason:"new", previous_session_file}` (or `"fork"` for
`agentc_mode_fork_session`, which keeps the transcript). No `session_shutdown` is
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
| --- | --- |
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
`--model M`, `--api-key K`, `--base-url URL`, `--system S`,
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
resolves to an OpenAI-compatible gateway row instead of failing (§3.2); an
unknown id combined with `--base-url` materializes an ad-hoc gateway for that
run. `--base-url` alone applies to the default provider.
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

## P6-E additions (settings, prompt anchors, status notices, content type)

- **Prompt anchors.** `agentc_ext_prompt_build` resolves
  `--system > REPLACE(ALL) > per-anchor REPLACE > core default` and wraps
  `add_prompt` APPENDs by `(priority, registration)`; the four core anchors are
  still rendered byte-identically when no contribution targets them.
  `builtin-context` contributes at `ALL/APPEND/AFTER/100`.
- **Settings.** `src/core/settings.c` holds declared settings keyed by
  `(extension, key)` and resolves them from `settings.<ext>.<key>` in the user
  config, the trusted project config and `auth.jsonc` (secrets); `host->add_setting`
  /`setting_get`/`setting_set` are the extension surface, `--set`/`--list-settings`
  and RPC `get_settings`/`set_setting`/`reset_setting` the front ends. Invalid
  values fall back to the declared default with one warning.
- **Status notices.** `emit()` in `agent.c` renders the `STEP_END`/`TURN_END`/
  `FINAL_OUTPUT` regions (via `agentc_status_emit`) and appends the rows through
  `agentc_ext_append_entry("notice", …)`, so they land in the session as
  `custom` records. The path is skipped when no line is registered.
- **`content_type`.** `AgcMessage.content_type` is opaque media metadata that
  round-trips through the JSON codec (default `text/plain`); the core never
  switches on it. `AGENTC_MSG_REALTIME_START/_STOP` are the voice CONTROL kinds.

## P6 completion pass 2 (runtime reentrancy + graceful shutdown)

- **Reentrancy-safe scratch bodies (audit M5).** `agentc_hook_branch` and
  `agentc_controller_apply` no longer hand out one process-global `AgcBuf`.
  Each invocation takes a slot from a depth-bounded ring
  (`AGENTC_HOOK_MAX_DEPTH`), so a nested branch reachable through the
  synchronous child run or a hook resume cannot clobber the outer body. The
  branch/decide goldens are byte-identical.
- **R-D17 graceful shutdown (audit M3).** `agentc_runtime_shutdown` now issues a
  graceful turn cancel (`stage 1`) to the whole tree, drives `agentc_actor_tick`
  for a bounded number of 10 ms slices (polling/reaping process actors and
  escalating grace→force), and only then force-releases every row. The loop is
  bounded by `AGENTC_RUNTIME_SHUTDOWN_SLICES`, so `runtime shutdown` can never
  hang on a wedged process actor.

### P6 completion pass 2: process policy + `policy_rules`

`src/core/trust.c` owns a process-lifetime `AgcPolicy` store
(`agentc_trust_policy_load`/`_add_json`/`_clear`). At startup the app calls
`agentc_project_apply_policy(cwd, trusted)`, which auto-loads the user
`$XDG_CONFIG_HOME/abird/agentc/policy.jsonc` and (when trusted)
`<cwd>/.agentc/policy.jsonc`, then emits the `policy_rules` point and consumes
the validated `policyPaths` contribution (bounded by
`AGENTC_POLICY_PATHS_MAX`, each path re-checked by
`agentc_project_validate_path`). `agentc_trust_gate` evaluates the store only
when `agentc_trust_decide` returned `ALLOW`, so a hard layer-2 deny/ask/delegate
is never loosened; first-match-wins is inside `agentc_policy_eval`.

`AgcExtResult.policyPaths`/`trustPatch` are reserved (always NULL): the
contributions ride `result_json` (`policyPaths` key / `trust_select` point) for
append-only ABI stability.

### P6 completion pass 2: `approvals.jsonc` project memo

Project-scope approval memos persist in a separate `approvals.jsonc` under the
config dir (C-D11), never overloaded onto `trust.jsonc`. `agentc_trust_load`
computes the path, reads the `projects` map into the scope table and records
`trust.approval.persist`; `agentc_trust_scope_put` rewrites the file on a
project-scope allow when persistence is on. `once`/`step`/`session` stay in
memory, `agentc_trust_scope_clear_session` keeps only the project rows, and a
malformed file is ignored (fail-closed, never trusted).

### P6 completion pass 2: resume reconstructs the read-only agent tree

`agentc_mode_setup` arms the global event observers before the root agent is
created, so the root `ACTOR_STARTED` becomes a `custom type:"agent"` record
instead of being missed. On a resumed session `agentc_mode_load_agent_tree`
count-first reads the records (`agentc_session_load_agents`) into
`AgcModeCtx.persisted_agents`; `agentc_mode_resume_session` and `new_session`
refresh/clear it, and teardown frees it. No child is re-run. RPC
`actor_query {what:"persisted_agents"}` exposes the rows read-only
(`"persisted":true`).

### P6 completion pass 2b: freeze golden + Windows skip goldens

`tests/freeze_test.c` is the dedicated P6-ACTORS §6b golden: it freezes/resumes
an actor, a subtree and the whole runtime, asserts the frozen timer is suspended
(a deadline does not fire until resume shifts it), buffers stdin written while
frozen and flushes it on resume, checks that cancel resumes a frozen actor
before terminating it, and that `agentc_actor_release_all` reaps a frozen child
with no orphan. The process cases are `-ENOSYS` on Windows; `tests/wine.sh` now
prefers `tests/data/<name>.windows.expected` when present, and freeze_test ships
one whose lines read `label=skip`, so a Windows skip is explicit and cannot
masquerade as a pass.

### P6 completion pass 2b: Windows skip goldens

The same `<name>.windows.expected` mechanism covers `actors_test` (the whole
process state machine), `subagents_test` (`persist.*`), `voice_test`
(`bidi_echo`), `trust_test` (`policy.reg`/`gate.program_allow`) and
`deciders_test` (`D.program_allow`). Those checks now print `label=skip` on
Windows instead of short-circuiting to a hard-coded `1`, so the Wine gate
documents exactly what it does not prove.

### P6 completion pass 2b: MCP decider backend

`mcp_decide()` in `src/core/decide.c` resolves the decider's `command`
(a canonical `mcp__<server>__<tool>` name) against the live extension tool
table (`agentc_ext_tools` count-first snapshot), runs it synchronously and
returns its text as the decision record. `AGENTC_DEFAULT`/`run`/`exec` tools
are both supported; an async-only tool has no synchronous answer and fails
closed. A missing tool, a run error, empty output or a record that
`agentc_decision_parse` rejects becomes the fail-closed `deny`
`{"reason":"mcp decider failed"}`. The controller path is unchanged: it is
still the ordinary `tool`/`call` hook, so an MCP decider is registered through
the same decider registry (`host->decider_register`, `kind:"mcp"`).

### P6 completion pass 2b: prompt-contribution golden

`tests/prompt_test.c` drives the real host `add_prompt` slot and then
`agentc_prompt_build`/`agentc_ext_prompt_replace`: two REPLACE contributions for
one anchor prove the lower-priority winner is rendered and the loser dropped;
APPEND before/after ordering is asserted; REPLACE suppresses the core default
for its anchor; and an ALL-level REPLACE suppresses the whole four-anchor walk
while the ALL before/after APPENDs remain. Only `label=1|0` lines are printed
(the Environment anchor embeds the cwd), so the same golden holds under Wine.

### P6 completion pass 2d: async decision re-entry and approval from the pump

`include/hook.h`/`src/core/hook.c` gain a **self-resume/consume seam**:
`agentc_hook_branch_self` marks the branch token, so `agentc_hook_resume`
re-enters the *same* handler (not `i+1`) with `agentc_hook_in_reentry()` true
and the branch result in `m->body`. A handler that parks by hand can call
`agentc_hook_mark_self(token)` itself. The controller uses this: on re-entry it
parses the decision response already in `m->body` and runs its own
`agentc_controller_apply` instead of branching again.

`src/core/decide.c` adds `agentc_decide_async` and a bounded pending table
(`AGENTC_DECISION_PENDING_MAX`). A MODEL decider called through the async path
uses a self-resuming **background** branch; the hook token is registered with
the correlation id, and `agentc_decision_submit` resumes the parked controller
with the out-of-band response (validate-only when no row matches, preserving
`D.parse_ok`). The synchronous `agentc_decide` keeps the `WAIT` branch for
existing callers, so every prior golden is byte-identical.

`src/core/approval.c` gains a `tool_approval`-class core hook stage
(`AGENTC_HOOK_STAGE_APPROVAL`) and `agentc_hook_run_defer`/`agentc_hook_cancel`.
When an `approval` handler parks (it may `agentc_hook_branch_self` a decider or
schedule a runtime continuation), `agentc_approval_request` drives the one
`agentc_pump` until `agentc_approval_submit` parks an answer on the pending row
or the deadline expires (fail-closed `approval_timeout`). The row transfers
ownership of the answer to the caller. With no `approval` handler registered
the path is untouched, so the existing synchronous trust/approval goldens are
byte-identical.

**Resumable one-model-step scheduler slices (completion pass 2e).**
`agentc_actor_tick` now advances a background agent exactly one model step per
slice. The turn loop was renamed `agent_run_turn(a, text, background)` and is
driven from an appended core-internal `AgcRunState` on `struct AgcAgent`:

- `agentc_agent_submit` calls `agent_run_turn(..., false)` and drives the state
  machine to completion synchronously, so it never yields and the transcript
  mutation order (and therefore `tools_test`/`compact_test`/`agent_loop_test`
  bytes) is unchanged.
- `agent_actor_step` calls `agent_run_turn(..., true)` once per actor tick. At
the post-tool safe point — after the whole tool batch has returned, before the
next provider request (d-plan §3.4/§7.3) — a background run saves `rc`,
`terminal_stop`, `continuations` and `have_transport`, sets `resume_inner`, and
returns `AGENTC_RUN_YIELD`. The next tick resumes at the top of the inner step
loop and skips the per-turn setup (queue turn bump / steer-flag reset).

Because the yield is *after* the batch has returned and `slots` are freed, no
`job_tools`/`slots`/`JobCtx` pointer is live across a slice, and no transcript
iterator (`asst`/`asst_idx`) or attempt counter needs to persist: the next slice
recomputes them. `AgcRunState` therefore holds only the four run counters plus
the two booleans. The state is per-agent, so nested foreground submits (a parent
spawning a `WAIT` child) are unaffected; a cancel delivered between slices is
left intact because the resume path does not clear `a->cancel`.

The reproduction is now a golden: `tests/subagents_test.c`
`test_background_slice` drives a background child whose replay transport serves
a tool-use turn then a final text turn and asserts `r.i == 1` after the first
`agentc_actor_tick`, `r.i == 2` after the second, and a settled actor with the
expected four-message transcript. Every pre-existing golden is byte-identical;
only the seven `slice.*` lines are appended to both `subagents_test` goldens.

**No-tool continuation yields per tick (completion pass 2f).** The first cut
only yielded on the tool-batch path, so a no-tool `step_end.continue` coalesced
its continuation model step into the same actor tick. The no-tool branch now
saves the same run counters and returns `AGENTC_RUN_YIELD` before `continue`
when `background` is true, so a background agent advances exactly one model
step per `agentc_actor_tick` in every case. A foreground submit passes
`background=false` and never yields, so the synchronous transcript order and
every byte-exact golden are unchanged. `tests/subagents_test.c`
`test_background_no_tool_slice` registers a consuming `turn` end handler that
returns `continue` once, drives two ticks over a two-step no-tool replay turn,
and asserts `r.i == 1` then `r.i == 2` with a settled actor; both
`subagents_test` goldens gained the `notool.*` lines.

## P6 completion pass 2g: provider-hook fold

The four provider interception points are the last direct `agentc_ext_emit`
sites. They now fold through one appended core-internal `provider` hook stage
(`AGENTC_HOOK_STAGE_PROVIDER`), with the sub-kind in the message meta:

| sub-kind | legacy point | verb | gate |
| --- | --- | --- | --- |
| `headers` | `before_provider_headers` | override | fail-closed, never quiet-gated |
| `request` | `before_provider_request` | override | fail-closed, never quiet-gated |
| `response` | `after_provider_response` | observe | fail-open, quiet-gated |
| `stream_event` | `provider_stream_event` | observe | fail-open, quiet-gated |

`request_stream` and `on_sse_event` build the same payload JSON as before and
dispatch it with `agentc_hook_run_legacy(AGENTC_HOOK_STAGE_PROVIDER,
"<legacy point>", …)`, so the new core `provider` chain and the mapped legacy
point run exactly once and the transferred `result_json`/`blocked`/`handled` is
interpreted at the call site exactly as the former direct emit did:

- `headers` — the flat `agentc_ext_headers_apply_patch` patch (null deletes);
  a blocked result or a parked/failed stage fails the request with
  `REQ_BLOCKED`.
- `request` — the effective fields patch replaces the serialized body when it
  carries a `payload` object; a blocked result fails the request.
- `response` / `stream_event` — observe only; the result is freed unread.

Because `agentc_hook_run_legacy` does not quiet-gate the legacy bus, the helper
passes `point = NULL` under a quiet front end so the observe legacy points stay
suppressed, while the new core `provider` stage may still run. Overrides are
never quiet-gated. Every pre-existing golden (including `agent_loop_test`, which
asserts the exact provider payloads) is byte-identical; `hooks_test` gained the
`P.*` checks and the adapter/quiet coverage.

## P6 as-built summary (core agent)

- **Step loop.** `agent_run_turn(a, text, background)` is driven from the
  appended `AgcRunState`; `agentc_agent_submit` runs it synchronously to
  completion (never yields), while `agent_actor_step` advances a background
  agent one model step per actor tick and returns `AGENTC_RUN_YIELD` at the
  post-tool or no-tool `step_end.continue` safe point.
- **Interception.** Every override/observe point — input, message end, step
  end, tool call/result, settle, compact, the provider points and the typed
  `event` stage — now rides one pipeline (the B8 fold is complete).
- **Actors and processes.** The actor table owns agent and process rows;
  `runtime_step`/`runtime_tick`/`agentc_pump` are the only driver; freeze,
  resume, cancel, reap and the bounded continuation table live in
  `src/core/runtime.c`.
- **Trust, approvals and decisions.** The four-layer envelope, the approval
  contract (sync UI / program / `tool_approval` / async core hook), the policy
  store and the LOCAL/PROGRAM/MCP/MODEL deciders are as recorded in §5.5 and
  §4.7.

## review-hardening as-built (core agent)

- **Capability, not names, drives trust.** `AgcToolCall.flags` (appended) carries
  the resolved tool's `AgcTool.flags` into `agentc_trust_gate` /
  `agentc_trust_decide_flags`: a tool is *mutating* unless it declares
  `AGENTC_TOOL_READONLY` (zero/unknown ⇒ mutating, fail closed), and
  `AGENTC_TRUST_TOOL_NET` marks a network tool. `agentc_trust_filter_tools`
  classifies from the `AgcTool` array directly. This replaces the hardcoded
  `write|edit` mutation list, so read-only denies `bash`/`process` too.
- **Envelope narrowing is monotone and complete.** Network comparison is by
  permissiveness rank (`allow > allowlist > deny`), `policy_id` is parent-owned,
  depth saturates at `UINT16_MAX`, and the approval memo key covers
  roots/network/hosts/flags/policy_id.
- **Approval invariants.** A parked verdict is cleared on slot release and
  allocation (never replayed onto a reused slot); an approver-narrowed `args` is
  not memoized; a non-ALLOW/non-DENY approver verdict (including `ask`) is
  normalised to DENY; `AGENTC_TOOL_NO_APPROVAL` runs decide + policy and only
  collapses ASK/DELEGATE.
- **Tool-job driver.** The batch state is per-invocation (heap `JobsCtx`, a
  save/restore `g_ctx` for `agentc_tool_checkpoint`), so a nested `wait:true`
  subagent or an `agentc_pump` from a tool reader cannot clobber an outer batch;
  the result loop carries the resolved tool per slot.
- **Session/queue and config.** `/resume`/`/new` replace (never merge) the queue
  and restore `paused`; config/auth writers refuse a present-but-unreadable file
  (`agentc_read_file_checked`); the SDK re-arms its persistence observers after
  `agentc_events_reset`.
- **Policy rule ordering.** Policy matching is first-match-wins, so authors must
  put `deny` rules before broader `allow` rules; a leading `allow` can shadow a
  later `deny` for the same tool. This is the documented policy-language
  semantics, noted here because both trust paths must agree on it.
