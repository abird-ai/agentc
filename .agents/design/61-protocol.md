# 61 — Framed CBOR protocol (`agentc serve`)

Status: Axis C1 landed; C2 additions (cancel, multi-client hardening, paging,
UI dialogs) documented here; C3 adds the WebSocket/TLS transports, attach
routing and a bounded multi-session registry. Source of truth for the framed
transport that `agentc serve` speaks and `agentc-client` / `tests/proto_client.py`
consume. The shared command vocabulary is `.agents/design/60-rpc.md`; this
document only covers the framing, transport, envelope and server policy.

## 1. Framing

One connection is a stream of frames:

```text
u32 payload_len (big-endian) | payload[payload_len]
```

`payload_len` excludes the 4-byte prefix and is capped at 16 MiB
(`AGENTC_FRAME_MAX_BYTES`, C-D12). The decoder (`src/wire/frame.c`) coalesces
and fragments arbitrarily; a declared length over the cap is rejected *before*
any payload is copied, and a protocol error latches a sticky failure. The CBOR
payload is exactly one definite-length value that must consume the whole
payload: `agentc_cbor_decode` rejects any trailing byte with `-EPROTO`
(`src/base/cbor.c`; strict UTF-8, no tags / indefinite / f16 / f32, pi-safe
integers by default, duplicate text keys rejected).

## 2. Envelopes

Every payload is a map with a text `kind`:

| kind | shape |
| --- | --- |
| `hello` | `{kind, version:1, client?, token?}` |
| `hello_ack` | `{kind, version:1, server_id, session_id?, features:[...]}` |
| `hello_error` | `{kind, code, message}` (sent, then the connection closes) |
| `request` | `{kind, id:<uint\|text>, method, params?:<json>}` |
| `response` | `{kind, id, ok, result?:<json> \| error:{code,message}}` |
| `cancel` | `{kind, id}` — out-of-band abort of the run owning `id` |
| `event` | `{kind, seq:<uint>, session_id?:<text>, event:<json>}` |
| `ui_request` | `{kind, id:<uint>, method, params:<json>?}` — server dialog (feature `ui`) |
| `ui_response` | `{kind, id:<uint>, ok:<bool>, result:<json>?}` — client dialog reply |
| `attach` | `{kind, id:<uint\|text>, session?:<text>, events?:[<text>...]}` — request a session/subscription filter (feature `attach`, C-D4) |
| `detach` | `{kind, id:<uint\|text>}` — stop the event broadcast (feature `attach`) |

`params`/`result`/`event` carry strict-JSON-constrained CBOR (C-D1); floats and
byte strings are rejected `unsupported`. Unknown properties are rejected at the
envelope level (`-EPROTO`), as are malformed CBOR and unknown kinds; either is
**terminal for that connection only** (C-D7). `features` is additive and
unknown feature names are ignored by clients; v1 advertises `cancel`, `ui`,
`attach` and `multi_session`. An `event` envelope is tagged with the
`session_id` it originated from (C3).
A `ui_request` method is one of `select|confirm|input|editor`, its `id` is a
bounded server dialog sequence (a uint, never text), and `params` is a
strict-JSON map. A `ui_response` with `ok:false` carries no `result`.

## 3. Transport and server policy (C-D3/C-D4)

- Unix-domain socket primary (`0600` in a `0700` directory), loopback TCP
  fallback (`--tcp IP:PORT`). C3 adds the explicit bind options `--host
  IP[:PORT]` (default port 8790) and `--remote` (alias for `--host 0.0.0.0`),
  which select a TCP listener just like `--tcp`. A bind outside `127.0.0.0/8`
  is refused unless `--token` is set (fail-closed): exposing the session to
  the network always requires authentication. Stale socket files are
  recovered. Optional `--token`; a mismatched token is `hello_error:unauthorized`.
  The ready line reports `transport:"tcp"` and `path` as `IP:PORT`.
- Windows (`src/net/win/unix.c`) has no AF_UNIX in the freestanding ws2_32
  layer, so it always uses the TCP fallback. `tests/serve.sh --wine` binds an
  ephemeral TCP port and runs the control-command smoke suite under Wine when
  wine and `build/agentc.exe` are available, and skips cleanly otherwise
  (whether a given Wine version exposes AF_UNIX is version-dependent).
- Connection = attachment. **A bounded registry of up to 4 sessions per
  process** (C3). Session 0 is the process's initial engine; `new_session`
  creates a fresh independent engine (its own agent, session file/transcript and
  provider/model state) in a free slot and moves the requesting connection to
  it, or selects an existing session by the optional `session` id. A full
  registry fails closed `busy`; an unknown selected id is `not_found`. A
  handshaken connection is attached to session 0 by default, receives that
  session's event broadcast, and may `attach` to any registry session (the
  `session` id must exist, else `not_found`) with an optional `events`
  subscription filter; `detach` (or a closed connection) releases the
  attachment but **not** the session. A `response` goes only to the requesting
  connection; an `event` carries its originating `session_id` and is broadcast
  only to that session's attachments. There is still no transcript replay on
  attach: a client resyncs with `get_messages` (C-D8). Execution is
  **interleaved cooperatively**: the serve poll loop advances each session's
  active run by one bounded model-step slice per iteration, rebinding the
  process-global services to that session first (§7.1).
- At most **8 connections**; excess accepted sockets are closed immediately.
- Per-connection output is capped at **32 MiB** (C-D12). A slow consumer that
  overflows it is closed — that connection only, never the server.
- The ready line is one JSON object on stdout:
  `{transport,path,server_id,pid,session_id,token_file,ws?}` (`ws:true` when
  `--ws` is active).
- **Graceful shutdown**: `SIGINT`/`SIGTERM` latch a serve-loop stop flag (the
  async-signal-safe handler only sets it). The loop drains, the normal teardown
  unlinks the Unix socket and releases every session, and the process exits `0`.
  A second signal before teardown completes is harmless (the flag is already
  set); a `SIGKILL` is still recovered by the stale-socket probe on restart.

## 3a. WebSocket transport (C3)

`agentc serve --host IP:PORT --ws` turns the TCP listener into an RFC 6455
endpoint. The client sends an HTTP/1.1 `Upgrade: websocket` request with
`Sec-WebSocket-Version: 13`, a `Sec-WebSocket-Key` and the subprotocol
`Sec-WebSocket-Protocol: agentc.v1`; the server answers `101 Switching
Protocols` (echoing the computed `Sec-WebSocket-Accept` and the negotiated
subprotocol) and then speaks the framed protocol. The handshake is bounded by
an 8 KiB request-header cap and is fail-closed (a missing upgrade, version,
key or subprotocol is a `400` and the connection closes).

Each WebSocket **binary** message carries exactly one framed-protocol record
(the same `u32 length | CBOR payload` bytes the raw transport carries), so the
frame decoder and all of its caps are reused unchanged. Server messages are
unmasked; client messages must be masked. Text messages (`1003`), reserved
bits, unmasked frames, fragmented/oversized control frames, a declared length
over 16 MiB and a reassembled message over 16 MiB are protocol errors and close
the connection. Ping is answered with Pong; a Close is echoed with `1000`. The
parser is unit-tested by `tests/ws_test.c` (handshake, masking, fragmentation,
interleaved control frames, the cap, and oversized-frame close) and the Python
cross-language client exercises the full suite over WebSocket
(`proto_client.py --ws`).

## 3b. TLS transport (C3)

`agentc serve --host IP:PORT --tls CERT KEY` wraps the TCP listener (and, when
combined, the WebSocket upgrade) in TLS using the vendored mbedTLS 3.6. CERT and
KEY are PEM files read into bounded buffers at startup (no `stdio`); the server
presents CERT and does not request a client certificate in v1. The handshake is
TLS 1.2/1.3, resumable and poll-driven exactly like the HTTP client
(`agentc_tls_handshake` + `agentc_tls_want`). TLS is Linux-only: the vendored
mbedTLS server translation units (`MBEDTLS_SSL_SRV_C`, `ssl_tls12_server.c`,
`ssl_tls13_server.c`) are compiled on Linux; the macOS/Windows backends report
`agentc_tls_server_supported() == false` and `--tls` fails closed (-ENOSYS).
A missing/unreadable certificate or key, or an unsupported platform, aborts
startup before the ready line. The framed protocol (and WebSocket messages) ride
unchanged over the TLS stream. `tests/serve.sh` generates a throwaway
self-signed certificate with `openssl` and runs the Python client with `--tls`
(certificate verification disabled), skipping cleanly when `openssl` is absent.

## 4. Runs, cancel and the request queue

`serve` is single-threaded but non-blocking at the event and pump seams. The
dispatcher (`src/app/rpc_dispatch.c`) is shared with `--mode rpc`, so the
command semantics are identical.

- A `prompt`/`prompt_template` request is **acceptance-first**: the response
  `{"disposition":"started"}` is written before the run starts; the outcome
  arrives later as an `agent_settled` event.
- While a session's run is active, a `request` for that session is decoded and
  **queued**; a valid prompt is answered `{"disposition":"queued"}` and its ack
  remembered. After that session settles its queue is drained in arrival order.
  Requests for a different, idle session are dispatched immediately and do not
  wait. The queue is bounded at **256 records**; a record that would overflow it
  is refused `busy` (fail-closed, never silently dropped). `abort` is still
  answered immediately.
- The out-of-band **`cancel`** envelope aborts the active run of the
  connection's own session (C-D9); a sibling session is untouched. It is read by
  an input pump installed as both the agent pump and the wire poll hook, so a
  stalled HTTP/SSE read is interrupted within one poll slice; the run settles
  `agent_settled` with `disposition:"aborted"`. `abort` remains the method form.
- The agent event/pump seams re-enter the input service with a zero timeout, so
  a queued request can never be dispatched re-entrantly on top of an active run
  slice.

`--mode rpc` JSONL is unchanged: `serve` layers the framing on the same record
layer.

## 5. `get_messages` paging

C1 returned the whole transcript, bounded only by the 16 MiB frame cap. C2 adds
cursor paging (additive keys; the pre-existing `messages` array is unchanged):

| param | meaning |
| --- | --- |
| `cursor` | 0-based index of the first message to return (default 0) |
| `limit` | page size, clamped to **1..1000**, default **100** |

Response: `{messages:[...], total:<n>, next_cursor:<n>?}`. `next_cursor` is
present only when more messages remain; a client resyncs by requesting with that
cursor. The 16 MiB frame cap still bounds one page, so a page of very large
messages can still exceed it; callers lower `limit` when that happens.

## 5a. Reconnect and resync (C-D8)

Reconnect is explicit: `agentc_client_reconnect()` closes the socket, dials
again and re-runs the hello handshake. There is **no automatic replay** of the
transcript or of events a client may have missed while disconnected. A client
that needs to catch up resyncs with `get_messages` paging from cursor `0`
(`agentc-client resync [LIMIT]` does exactly this and reports
`{total,messages,pages}`). `agentc-client reconnect` prints
`{reconnected:true,replayed:false,server_id,session_id}` to make the no-replay
contract observable.

## 6. UI and dialogs (C-D11)

v1 advertises the `ui` feature. `ui_request`/`ui_response` are the dialog
envelopes; the request methods are `select|confirm|input|editor`, the `id` is a
bounded server dialog sequence (a uint, never text), and `params` is a
strict-JSON map carrying `prompt` (text), `options` (`select` only, 1..64 text
entries) and an optional `default`.

A client raises a dialog with a `request` of method `ui_prompt` whose params
carry the dialog `method` plus its payload (and an optional `timeout_ms`,
clamped to 1..600000). The server assigns a dialog id, sends a `ui_request` to
the requester, **parks** the dialog and keeps servicing every connection; a
matching `ui_response` completes the parked request with `{value:<answer>}`. A
deny (`ok:false`) or the bounded deadline (default 30 s) answers
`ok:false code:"denied"` (fail-closed). At most 16 dialogs may be parked; a
further one is refused `busy`. A `ui_response` with no matching pending dialog
(or from another connection) is validated and **dropped**.

The core exposes one prompt sink (`agentc_ext_set_ui_prompt`, plus the
`agentc_ui_prompt_{confirm,select,input,editor}` points). `serve` installs a
sink that raises the dialog on the connection owning the current run and pumps
its own bounded loop until the reply arrives; it also installs its tool-approval
front end through that sink, so a served run that needs approval raises a
`confirm` dialog. With no sink installed, or after the deadline, the prompt
fails closed. The interactive TUI answers from its modal; a non-interactive
connection gets the bounded default / deny.

## 7. Divergences from pi

Same set as `.agents/design/60-rpc.md` §10; additionally the framed profile
rejects byte strings and floats, has a fixed 8-connection cap and a bounded
(4-session) registry, and offers attach/detach session routing with event
subscription filters.

### 7.1 Multiple sessions: implemented, and the remaining constraint

`agentc serve` keeps a **bounded registry of up to 4 sessions** per process.
Each slot owns an independent engine: its own `AgcModeCtx`, `AgcAgent`,
`AgcSession` (file or memory-only), transcript, provider/model selection and
persistence observer. `new_session` creates a new engine in a free slot (or
selects an existing id) and returns its id; the requesting connection is moved
to it. `attach` selects the session a connection receives events from and
optionally narrows the event-type subscription. Events carry their originating
`session_id`. The P6-D multi-agent-per-process machinery (a per-agent event
callback, observer, actor row and queue) is reused directly.

**Runs from different sessions interleave** (P5 concurrency). The shared RPC
dispatcher runs in `defer_run` mode for serve: an accepted `prompt` calls
`agentc_agent_run_begin()` and returns instead of driving the run to
completion. The serve loop then calls `serve_advance_runs()`, which advances
each session's active run by **one bounded model-step slice** per iteration via
the P6 resumable scheduler (`agentc_agent_run_step()` / `AGENTC_RUN_YIELD` at
the post-tool safe point) and emits `agent_settled` from the dispatcher when a
run settles. Before each slice `agentc_mode_activate()` rebinds the
process-global services to that session:

- the extension model/thinking sinks,
- the extension context (`cwd`/`session_id`/`session_file`/system prompt) and
  entry sink, and the tool veto,
- the current trust envelope and its audit sink,
- the queue/agent persistence owner, and
- the agent's event callback (`serve_event` with the session as userdata).

The wire poll hook and the agent pump stay the server's own; they are
session-agnostic. `cancel` (and the `abort` method) now route to the
**requester's own session**: cancelling one run never touches a sibling's, and
a session reaching settle frees its slice. Each session's queued requests wait
only for that session's run (a global bounded queue still caps the backlog at
256 records). The single-session path preserves the previous event order.

What remains shared, and is the exact constraint: a **single model step is not
preemptible**. The extension registry, the prompt registry, the auth/OAuth and
settings stores, the process-global actor registry and runtime tick, the HTTP
poll hook and pump slots, and the single set of UI/approval sinks are rebound
per slice but cannot be used by two sessions at the same instant. Because the
slice boundary is the post-tool safe point, a session whose step blocks on a
slow provider HTTP/SSE read holds the single-threaded loop inside that step:
the wire poll hook still services input and a `cancel` can abort the stalled
read, but a sibling session's *next* step waits for the read to return. The
granularity of concurrency is therefore one model step, not a true parallel
run; this is documented rather than hidden. (Two writers on one session file
remain unsafe; v1 serve adds no locking.) Adding a session is bounded at 4 and
fails closed `busy`; `set_model`/`set_thinking_level` affect only the
requesting session.
