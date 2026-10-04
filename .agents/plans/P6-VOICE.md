# P6-VOICE — Realtime voice as an extension

Status: **as-built (P6-A..E + completion passes 2a-2g).** Realtime voice as an extension.
As-built note: the outcome is folded into `.agents/design/30-extensibility.md` and `.agents/design/40-tui.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

Goal: add Codex-class **realtime voice** (mic/speaker, live transcription,
spoken agent replies, voice-driven tool calls) **without a new primitive, a new
actor kind, a new tool, or a new host capability**. It is an **extension** plus
one **`process` actor** (the voice helper), riding the existing message, hook,
and envelope machinery.

---

## 1. What Codex does (grounded in 0.159.3)

- A **thread-scoped realtime session** to a realtime backend, started/stopped via
  `thread/realtime/{start,stop,listVoices}` and fed with
  `appendAudio` (base64 PCM), `appendText`, `appendSpeech` (TTS).
- Output is streamed as `outputAudioDelta` (PCM chunks) and `transcriptDelta`
  (role + text), plus `itemAdded/Started/Completed` and `Sdp` (WebRTC).
- A native **voice helper** process owns the audio devices and the WebRTC client
  (`realtime-webrtc/src/client.rs`); Codex talks to it.
- Realtime items are **promoted** into the main thread (`bemItemPromoted`), and
  the main agent's responses are **handed off** into the session as spoken items
  (`codexResponseHandoffMode`, `codexResponsesAsItems`).
- Tool calls raised in the realtime session execute in the normal thread.
- Config: `audio` (mic/speaker/enabled), `realtime.*`
  (`ws_base_url`/`webrtc_call_base_url`/`ws_mode`/`backend_prompt`/
  `startup_context`/`start_instructions`), feature `realtime_conversation`;
  TUI `toggle_voice` / `toggle_voice_mute`.

## 2. The mapping (the whole idea)

| Codex | P6 |
| --- | --- |
| voice helper process (WebRTC + audio devices) | a **`process` actor** (no new kind) |
| `appendAudio` / `outputAudioDelta` | `input`/`output` messages with `content_type: audio/pcm;…` |
| `appendText` / `transcriptDelta` | `input`/`output` messages with `content_type: application/x-realtime-item` (or text) |
| `itemAdded/Started/Completed` | `NOTIFY`/HOOK messages (same content type) |
| `bemItemPromoted` | a **hook handler** on `input`/`message` that returns a normal message (promotion) |
| `codexResponseHandoff` (speak agent replies) | a **hook handler** on `output` that emits a `speech` item |
| realtime tool calls | ordinary **`tool`/`call` messages** (same pipeline, envelope, approval) |
| `Sdp` (WebRTC) | extension/helper-internal; surfaced as a message for the UI if wanted |
| barge-in / interrupt | `CANCEL{scope: turn}` |
| `toggle_voice` / `toggle_voice_mute` | extension **command** + **status** segment |
| `audio` / `realtime.*` config, feature flag | extension config `realtime.*` |

So: **the helper is a process actor, the session is a stream of messages, items
are promoted by a hook, tool calls are uniform, and the core stays media-blind.**

## 3. The one core addition: a content type on messages

The pipeline is currently text/JSON. Make it **media-agnostic** with one field:

```c
struct AgcMessage {
    ...
    const char *content_type;   /* "text/plain" | "application/json" |
                                   "audio/pcm;rate=24000;ch=1" |
                                   "application/x-realtime-item" | … */
    const char *body; size_t body_len;   /* already bytes */
    ...
};
```

- `output` / `input` / `hook` messages can now carry audio or item JSON without
  the core knowing what they are. Hooks transform or pass them unchanged.
- No new stage, no new message kind enum for media. `content_type` is opaque to
  the core; only the voice extension interprets it.

That is the entire core change. Everything else is extension + config + command.

## 4. The realtime session

- **Scoped to an agent actor** (`actor_id`), like Codex scopes it to a thread.
- **Start/stop** are CONTROL messages (`REALTIME_START`/`REALTIME_STOP`) or the
  extension command; they carry a JSON body (voice, model, instructions,
  `startup_context`) that the extension parses — the core does not.
- **Audio in**: the extension reads mic frames (via the helper) and pushes an
  `input` message (`content_type: audio/pcm`). `input` hooks can gate/redact.
- **Audio out / TTS**: the extension receives `output` messages for assistant
  text and emits `speech` items to the helper (`appendSpeech`).
- **Transcripts**: `input`/`output` items (`application/x-realtime-item`); the TUI
  renders them, the transcript writer records them via `message`/`app_session`.
- **Barge-in**: the helper (VAD) or the user triggers `CANCEL{scope:turn}`; the
  step stops, the queue survives, and the extension flushes pending audio.
- **Voices / models**: extension config/query (`/voice list`), cached.

## 5. The voice helper is a `process` actor

Why: WebRTC, audio devices, codecs (Opus/PCM), and TLS/WS belong **outside** the
freestanding core. A helper process is exactly what Codex does and exactly what
P6's `process` actor provides.

- Spawned by the extension via `agentc_actor_spawn({.proc=…})` under the agent's
  envelope; sandboxed, cancellable, freezable, listable, attachable — for free.
- **Framed stdio**: length-prefixed PCM frames + JSON control on stdin/stdout.
  This needs the process actor to have a **writable stdin pipe**, i.e.
  bidirectional pipe mode (`AGENTC_PROC_PIPE_BIDI`), which **MCP servers also
  need** (stdio JSON-RPC). This is a real core refinement and part of R-D27.
- The extension owns the protocol; the core only reads/writes bytes (`INPUT`
  write, `actor_read`).
- The helper's liveness shows up as `STARTED/PROGRESS/STALLED/FINISH`, so a dead
  or wedged helper is visible and cancellable like any process.

No new host capability is required: the extension uses `actor_spawn`,
`message_send` (INPUT bytes), `actor_read`, and `hook_run` — all existing.

## 6. What a voice extension registers (the extension contract)

A `realtime-voice` extension is an ordinary extension:

- **hooks**: `input` (mic/transcript ingest, promotion), `output` (speak agent
  replies, translate), `step` (barge-in bookkeeping), `signal` (cancel handling).
- **command** `/voice [start|stop|mute|list]`.
- **status segment** `voice` (state, muted, level).
- **config** `realtime.{enabled,mode(webrtc|ws),base_url,voice,model,mic,speaker}`.
- **tools**: none required (voice is I/O, not a capability). Optionally a
  `voice` command tool behind a flag, but not part of the core tool set.
- **process actor**: the helper, spawned on start.
- **session entries**: transcript items and the session record are written via
  the normal observer/`append_entry` path.

Because it is an extension, it composes with everything already designed:
approval (`tool_call`), envelopes (mic/speaker caps §7), audit (`hook` records),
and the actor tree (the helper shows in `/ps`).

## 7. Security

Voice introduces two capabilities the envelope should gate:

```c
#define AGENTC_TRUST_ALLOW_MIC     0x10u
#define AGENTC_TRUST_ALLOW_SPEAKER 0x20u
```

- Default: mic/speaker **denied** unless the user enables voice (a repo can
  require approval to enable → the extension's start is a `tool`/`command` that
  flows through the envelope).
- The helper runs under the agent's envelope; no network unless
  `trust.network` allows (the realtime backend is a host in `trust.hosts`).
- Transcripts are redactable by `output`/`input` hooks like any text.

## 8. Codex-parity surface (all extension-level)

| Codex method/notification | P6 |
| --- | --- |
| `thread/realtime/start` | CONTROL `REALTIME_START` (extension JSON body) |
| `appendAudio` / `outputAudioDelta` | `input`/`output` `audio/pcm` messages |
| `appendText` / `transcriptDelta` | `input`/`output` item messages |
| `appendSpeech` | `output` item (speech) |
| `itemAdded/Started/Completed` | `NOTIFY`/HOOK item messages |
| `bemItemPromoted` | `input`/`message` hook handler |
| `transcriptDone` | item final (`final=true`) |
| `Sdp` | helper-internal; optional message |
| `listVoices` | extension command/query |
| `stop` | CONTROL `REALTIME_STOP` |
| `toggle_voice`/`mute` | command/status |
| `realtime.*`, `audio` config | extension config |

## 9. Optional future axis (not needed now)

A pure-agentc realtime client (no helper) would need a Layer-1 **WebSocket/stream**
capability (`os_websocket_open/read/write`) plus a host `stream_*` method. That
is deliberately deferred: the helper-process design needs **zero** new core
capability and mirrors Codex. If a WS transport is later wanted, it slots in as
one net.h addition + one host method, still no new primitive.

## 10. Why it fits "cleanly, naturally, beautifully"

- **One new field** (`content_type`) makes the pipeline media-agnostic; no new
  stage, kind, actor kind, tool, or host method.
- **The helper is a process actor** — every existing guarantee (sandbox, cancel,
  freeze, list, attach, stall) applies unchanged.
- **Items promote through a hook** — the same mechanism as routing/approval, not
  a special realtime path.
- **Realtime tool calls are ordinary tool calls** — same envelope, approval,
  hooks, audit.
- **Barge-in is just `CANCEL{scope:turn}`** — the existing cancellation model.
- Voice is therefore an **extension + config + one process**, consistent with
  MCP: the core stays small and media-blind while the capability is first-class.

## 11. Example flow

```text
/voice start → extension spawns helper (process actor, PIPE_BIDI)
             → CONTROL REALTIME_START {mode:webrtc, voice:"…"}
mic frames  → INPUT audio/pcm → [input hook] → session
backend     → NOTIFY realtime-item → [input hook] promotes → normal user message
model reply → OUTPUT text → [output hook] → speech item → helper speaker
backend tool call → TOOL/call → envelope+approval → execute → TOOL/result → session
user barges  → CANCEL{scope:turn} → step stops, audio flushes
/voice stop  → CONTROL REALTIME_STOP → helper released
```

## 12. Appendix — helper framed-stdio protocol (a sketch)

The extension↔helper protocol is **extension-defined**, carried on the helper
`process` actor's `PIPE_BIDI` stdio. A minimal framed design:

**Frames** (both directions): `u32 be length` + `u8 type` + payload.

| type | dir | payload |
| --- | --- | --- |
| `0x01 CTRL_JSON` | ext↔helper | a JSON control object (below) |
| `0x02 PCM_IN` | ext→helper | mic/append audio (PCM) to send to the backend |
| `0x03 PCM_OUT` | helper→ext | output audio to play (or forward) |
| `0x04 TRANSCRIPT` | helper→ext | `{role, text, final}` |
| `0x05 ITEM` | helper→ext | raw realtime item JSON (promotion source) |
| `0x06 TOOLCALL` | helper→ext | `{call_id, name, args}` from the backend |
| `0x07 TOOLRESULT` | ext→helper | `{call_id, result, is_error}` to feed back |
| `0x08 ERROR` | helper→ext | `{code, message}` |

**Control JSON (`0x01`)** examples:

```jsonc
{"op":"start","mode":"webrtc|ws","base_url":"…","voice":"…","model":"…",
 "instructions":"…","startup_context":"…","mic":true,"speaker":true}
{"op":"speak","text":"…"}              // appendSpeech / TTS
{"op":"append_text","role":"user","text":"…"}
{"op":"mute","target":"mic|speaker","on":true}
{"op":"list_voices"}
{"op":"stop"}
```

**How the extension uses it (unchanged primitives):**

- spawn helper: `agentc_actor_spawn(&(AgcActorSpec){.proc=&(AgcProcSpawn){
  .command="agentc-voice-helper", .mode=AGENTC_PROC_PIPE_BIDI, .env=…}})`.
- send: `agentc_message_send` CONTROL `WRITE` (bytes) or `actor_op write`.
- receive: `agentc_actor_read` + a state machine; each `TRANSCRIPT`/`ITEM` pushes
  an `input`/`output` message (`content_type: application/x-realtime-item`);
  each `PCM_OUT` an `output` `audio/pcm` message; each `TOOLCALL` a normal
  `tool`/`call` message.
- stop: CONTROL `CANCEL{scope:actor}` → graceful→force; release the helper.

**Extension hook set (the whole voice integration):**

| hook | stage | job |
| --- | --- | --- |
| ingest mic/transcript | `input` | relay to session; promote items to normal messages |
| speak replies | `output` | send assistant text to helper as `speak` |
| barge-in | `signal`/`step` | flush audio, `CANCEL{scope:turn}` |
| redact/translate | `output`/`input` | edit content before it is spoken/shown |

That's the complete helper + extension surface; the core sees only messages and
one process actor.

## 13. Handoff

> Design addendum; land after P6-A (process actors) and P6-B/P6-C. Add
> `content_type` to `AgcMessage`; add `AGENTC_PROC_PIPE_BIDI` to the process spec
> (writable stdin pipe — **also required for MCP**); add the `AGENTC_TRUST_ALLOW_
> MIC/SPEAKER` flags. Then a `realtime-voice` extension contributes hooks,
> command, status, config and the helper process. No new primitive, stage, actor
> kind, tool, or host method. Record the `P6-VOICE`-specific decisions (content
> type, bidi pipe, mic/speaker caps) as R-D27 in `P6-RUNTIME.md`.
