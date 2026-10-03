# 60 — RPC v1 record layer

`--mode rpc` is agentc's language-independent control channel: JSONL commands
on stdin, responses and events on stdout. This document freezes the **record
layer** (message vocabulary, framing, codes and ordering). It is the schema
that Axis C's framed CBOR protocol reuses through one dispatcher, so anything
here is a contract, not an implementation detail.

The RPC version is `AGENTC_RPC_VERSION` in `src/app/mode.h` (currently **1**).

## 1. Framing

- One JSON value per line, terminated by `\n`. `\r\n` is accepted (a trailing
  `\r` is stripped).
- Input lines are parsed with the strict JSON parser; unknown extra keys are
  ignored. A non-object line is answered with an `invalid_json` error response.
- Output is ASCII-only JSON (non-ASCII is `\uXXXX`-escaped) and each record is
  one line.
- The stream is single-threaded and ordered: a command is fully handled
  (response written) before the next command is read, except while a prompt is
  running (see §6).

## 2. Version policy

- `AGENTC_RPC_VERSION` changes only for a **breaking** change: a removed or
  renamed command/field, a changed meaning, or changed framing.
- Additive changes (a new command, a new response/event key, a new `code` or
  `disposition` value) do **not** bump it. Clients must ignore unknown keys.
- `hello` reports the version; there is no negotiation in the JSONL framing.

## 3. Discriminator (dual shape)

Every request names its command in one of three ways (A-D1):

- canonical: `{"type":"get_state","id":1}`
- explicit: `{"type":"command","command":"get_state","id":1}`
- legacy: `{"command":"get_state","id":1}`

Resolution rule: if `type` is a string that names a known command *and* is not
one of `command`, `response`, `ui_response`, it is the command name; otherwise
the `command` string is. `response` and `ui_response` are output/client
message types and are never commands.

`id` may be a number, a string, or absent. Every response echoes it verbatim;
absent becomes `null`. Ids are opaque to the server.

## 4. Responses

```text
{"type":"response","id":<id|null>,"command":<name|null>,
 "success":<bool>,"code":"<machine>", "data":{…} | "error":"<text>"}
```

- `command` echoes the resolved command name (`null` when the request did not
  name one, e.g. a parse failure).
- `code` is the stable machine code; `error` remains the human string.
- `success:true` carries `data`; `success:false` carries `error`.
- An unknown command is **non-fatal**: `success:false`, `code:"unknown_command"`,
  and the session continues. There is no response to a dropped `ui_response`.

### 4.1 Codes

| code | meaning |
| --- | --- |
| `ok` | success |
| `unknown_command` | `type`/`command` names no command |
| `invalid_json` | input line was not a JSON object |
| `missing_command` | no command named |
| `missing_field` | a required parameter is absent |
| `missing_text` | `prompt` had no text/message/prompt field |
| `missing_name` | `prompt_template` had no name |
| `unknown_template` | `prompt_template` name is not registered |
| `invalid_level` | thinking level not in `{0,1,3,4,off,low,medium,high}` |
| `unknown_provider` | `set_model` provider is not registered |
| `out_of_memory` | allocation failed |
| `no_session` | command needs a session file and none is configured |
| `cancelled` | an extension vetoed a session switch |
| `session_failed` | session creation failed |
| `adapter_unavailable` | a P6 `queue`/`actor_*` tool adapter is absent |
| `actor_not_found` | `actor_query`/`actor_op` target does not exist |
| `no_result` | `actor_query` has no result yet |
| `denied` | trust envelope widening was refused |
| `invalid_response` | `decide`/`ui_response` payload was malformed |
| `busy` | the pending queue cannot accept the record (last resort) |
| `failed` | command-specific failure not covered above |
| `error` | provider/run failure or unclassified error |

Codes are a closed set; new ones are additive.

## 5. Commands

| command | parameters | data |
| --- | --- | --- |
| `hello` | — | `{protocol,version,commands,capabilities}` |
| `prompt` | `text`\|`message`\|`prompt` | acceptance disposition (no data); outcome via `agent_settled` |
| `prompt_template` | `name`, `args?` | acceptance `{disposition:"started"}` |
| `abort` | — | `{}` |
| `get_state` | — | state object (§5.1) |
| `set_model` | `model`, `provider?` | `{provider,model}` |
| `set_thinking_level` | `level`\|`thinking_level`\|`thinking_level_name` | `{thinking_level,thinking_level_name}` |
| `get_available_models` | — | `{models[],total,truncated?}` |
| `get_commands` | — | `{commands:[{name,description,hint,source}]}` |
| `compact` | — | `{compacted,context_tokens}` |
| `bash` | `command` (canonical), `input`\|`cmd`\|`args.command` (legacy), `timeout?` | `{output,is_error}` |
| `new_session` | — | `{session_id}` (A-D5: a cancelled switch is `success:false` + message, *not* `cancelled:true`) |
| `get_messages` | `cursor?`, `limit?` | `{messages:[…],total,next_cursor?}` |
| `queue` | tool args (e.g. `op`,`actor_id`,…) | adapter result object |
| `actor_op` | `op`, `actor_id?`, `args?` | adapter result object |
| `actor_query` | `what`, `actor_id?` | query result |
| `runtime` | `op` (`tick`/`shutdown`/`freeze`/`resume`) | `{}` |
| `get_trust` | — | trust envelope object |
| `set_trust` | `patch` | `{}` |
| `approve` / `deny` | `id`, `scope?`, `reason?` | `{}` |
| `decide` | `response`\|`data` | `{}` |
| `set_policy` | `id`, `kind`, `command?` | `{}` |
| `get_settings` | `ext?` | array of setting records |
| `set_setting` | `ext`, `key`, `value` | `{}` |
| `reset_setting` | `ext`, `key` | `{}` |

`ui_response` is accepted on input and dropped (§8).

### 5.1 `get_state`

Appends to the original fields (`provider`, `model`, `thinking_level`,
`context_tokens`, `compacted`, `messages`, `session_id`, `turn_no`,
`steerable`, `steer_hold_kind?`, `pending_approvals`, `background_actors`):

| key | type | meaning |
| --- | --- | --- |
| `protocol` | string | `"agentc-rpc"` |
| `thinking_level_name` | string | `off`/`low`/`medium`/`high` |
| `session_file` | string\|null | absolute session path, `null` if memory-only |
| `is_streaming` | bool | the dispatcher is inside a run (always false in the synchronous JSONL model; the framed protocol can observe `true`) |
| `pending_commands` | number | undelivered queued commands |
| `auto_compaction` | bool | automatic compaction enabled |

### 5.2 `get_commands`

The union of the prompt registry (`prompts.h`; `source` is `file`, an extension
name, or an MCP server) and extension commands (`source` is `"command"`), in
that order. `hint` is `""` when the source has none.

### 5.3 Models

`get_available_models` counts first (`agentc_model_all(NULL,0)`), allocates
exactly that many pointers and lists them. `total` is always the true count.
`truncated:true` is emitted **only** when that allocation failed (the OOM
last resort); it is absent otherwise. There is no fixed 32-entry cap.

### 5.4 Thinking

`thinking_level` remains the numeric form (`0` off, `1` low, `3` medium,
`4` high). `thinking_level_name` is the stable name. A request may send either
`thinking_level` (number) or `thinking_level_name` (or the legacy `level`).
Numbers `2`, and the names `minimal`, `xhigh`, `max`, are rejected with
`invalid_level`.

### 5.5 `get_messages` paging (C2)

`get_messages` returns a bounded page: `cursor` is a 0-based offset clamped to
the transcript length, `limit` is clamped to **1..1000** (default **100**).
`total` is the full transcript length; `next_cursor` is present only when more
messages remain. A client resyncs with `get_messages {cursor: next_cursor}`.
The 16 MiB frame cap still bounds a page, so lower `limit` for very large
messages.

## 6. Ordering, acceptance and backpressure

A prompt is **accepted first** (A-D2):

1. Validate the command. A pre-acceptance error (missing text, unknown
   template) is an ordinary failure response and nothing runs.
2. Otherwise write `{"type":"response","id":…,"command":"prompt","success":true,
   "code":"ok","data":{"disposition":"started"}}` *before* submitting.
3. Run the turn. Its `agent_end` event stream is unchanged.
4. After the run, emit `agent_settled` (§7). A post-acceptance failure never
   produces a second response; the `error` disposition carries the message.

While a run is active the input pump drains stdin and queues commands instead
of dispatching them (A-D3):

- `abort` is still immediate: it aborts the run and answers right away.
- `prompt`/`prompt_template` are answered
  `{"disposition":"queued"}` immediately; the ack is remembered so the queued
  record runs after settle without a second acceptance response.
- Every other command is queued and answered normally after settle.

The pending queue holds at most **256 records / 1 MiB** (A-D10). When it is
full the reader stops consuming stdin until settle frees space. A single record
larger than the byte cap is refused `code:"busy"` rather than silently dropped;
`busy` is otherwise unused, because backpressure — not refusal — is the normal
policy. Records are never lost silently.

## 7. `agent_settled`

Emitted once per accepted run, after `agent_end`:

```text
{"type":"agent_settled","disposition":"completed|aborted|error",
 "stop_reason":"stop|length|tool_use|error|aborted","error":<string|null>}
```

- `completed`: the run returned success.
- `aborted`: the run returned after a cooperative abort.
- `error`: any other terminal failure; `error` carries `last_error`.
- `stop_reason` mirrors the final assistant turn (`agentc_mode_stop_name`).

The `disposition` vocabulary shared with acceptance is
`started|queued|busy` (acceptance/queue) and `completed|aborted|error`
(settle).

## 8. UI

`ui_request` is a **fire-and-forget** server event in the JSONL profile; no
client reply is required and `ui_response` is parsed and dropped when no
dialog exists (A-D11). The framed `serve` profile (`.agents/design/61-protocol.md`
§6) implements the same `ui_request`/`ui_response` names as *dialogs*
(`select`/`confirm`/`input`/`editor`) with a parked pending dialog and a bounded
reply channel; `--mode rpc` JSONL keeps the fire-and-forget form.

```text
{"type":"ui_request","id":<n>,"method":"notify","message":<str>,"level":<int>}
{"type":"ui_request","id":<n>,"method":"setStatus","key":<str>,"text":<str>}
{"type":"ui_request","id":<n>,"method":"setTitle","title":<str>}
```

`id` is a server-side sequence for correlation. `notify`/`setTitle` come from
the extension UI sink; `setStatus` comes from the status observer seam
(`agentc_ext_set_status_observer`), because status is otherwise rendered
through the status table rather than the UI sink. `ui_response` on input is
validated and dropped:

```text
{"type":"ui_response","id":<n>,"result":<json>}   -> no output
```

## 9. Events

Agent events keep the exact `--mode json` schema
(`session`, `agent_start`, `turn_start`, `message_start`, `message_update`,
`message_end`, `tool_execution_start`, `tool_execution_end`, `turn_end`,
`agent_end`, `error`, `compaction`, …). RPC v1 adds only `agent_settled` (§7)
and `ui_request` (§8). Both are RPC-only; `--mode json` is unchanged.

## 10. Divergences from pi

- `new_session` reports a cancelled switch as `success:false` + `error`
  (A-D5), not pi's `cancelled:true`.
- `agentc` has no steering/images/session-naming/`abort_bash` in v1 (A-D9).
- `bash` is synchronous and non-streaming in v1 (A-D9).
- `agent_settled` is the agentc run-completion event; pi's equivalent outcome
  transport differs.
- F64 / binary payloads are out of scope for the JSONL framing; the framed
  profile rejects them `unsupported` (C-D1).
