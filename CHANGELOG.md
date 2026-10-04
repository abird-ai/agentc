# Changelog

All notable changes to agentc are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Releases before 0.7.0 predate this file; see the tag's generated notes on
GitHub.

## [Unreleased]

### Added

- **MCP optional GET event stream (P7-W2).** A READY HTTP server holds one
  `Accept: text/event-stream` GET (`agentc_http_stream_open/pump/close`), sending
  `Mcp-Session-Id` and `Last-Event-ID`; id-less notifications route through the
  same handler as POST notifications. `405` disables it; a stream error
  reconnects under the retry backoff without failing the server.
- **Extension helper-process isolation (P7-W3).**
  `agentc_ext_register_helper()` spawns a helper over the length-prefixed framed
  JSON protocol (`include/frame.h`), installs proxy tools/hooks/commands from a
  manifest handshake, and fails closed (retiring the helper's rows) on
  EOF/timeout/malformed without taking the host down.
- **Virtual models / routing (P7-W4).** A `models.<alias>` config declares
  `{provider, model}`; the target is stored authoritatively as
  `AgcModel.route_provider`/`route_model` (`route` is the display string). The
  alias resolves per model step to an effective `{provider, model, base_url,
  api_key}` while the transcript keeps the alias, fails closed on a broken route,
  emits `model_route` and a `custom type:"model"` session record, and applies an
  `AgcDecisionRecord.provider` only when it names the caller's provider. A routed
  target resolves its endpoint and credential through one app-installed resolver
  (`agentc_agent_set_route_target_resolver`; OAuth/env/`auth.jsonc`/config) and
  never receives the agent's own key. `agentc_config_load` clears and republishes
  the aliases; `agentc_model_clear_virtual()` drops them explicitly.
- **Session fork and tree (P7-W1).** `/fork` branches the live conversation into
  a new session whose header records `parent_session` and whose file carries a
  copy of the transcript and the parent's `custom` records (agent tree, queue,
  notices, routes) as a self-contained snapshot (`agentc_mode_fork_session`,
  `session_before_switch` reason `"fork"`); `/tree` opens a picker over
  `agentc_session_tree()` (the fork forest, indented by lineage depth) and resumes
  the selected session. `agentc_session_parent()` exposes the lineage id; no
  header version bump.
- **Idempotent queue enqueues (`client_id` dedupe, P7-W5).**
  `AGENTC_Q_F_DEDUPE` drops an enqueue whose `client_id` is already queued
  (before the cap check, first-write-wins, returning the existing item's id);
  `agentc_queue_deduped()` counts the drops and the `queue`/`agent` tools take
  `client_id` + `dedupe` and report `"deduped":true`. No input event or steer
  fires on a drop. Supersedes the P6-SURFACE §19 out-of-scope note.
- **Custom provider endpoints (P7).** `--base-url U` now works alone (applied
  to the default provider) or with an unknown `--provider <id>` to define an
  ad-hoc OpenAI-compatible gateway for one run, with no config entry. `agentc
  setup` gained a **Custom endpoint** choice that prompts for a provider name,
  base URL and optional key; it persists `providers.<id>.base_url` to
  `setup.jsonc` via the new `agentc_config_setup_set_provider()` (whose loader
  scope already accepts generic providers). `agentc_config_save_setup()` is now
  merge-preserving so that entry survives the default write. Ollama keeps
  `OLLAMA_HOST` and `providers.ollama.base_url`.
- **Agent token budget (P6-SURFACE §18 #23).** `AgcAgentSpec.token_budget`
  (and `agentc_agent_set_token_budget`) sets a cumulative soft cap on an
  agent's model tokens (`usage.input + output + cache reads/writes`), inherited
  by children when the spec leaves it `0`. Reaching it between model steps emits
  `actor_notify` and settles the turn without another provider request;
  `agentc_agent_token_budget`/`agentc_agent_tokens_spent` and the appended
  `AgcAgentInfo` fields expose the cap and the running total.
  `tests/budget_test.c`; `design/20-core-agent.md` §2.1.

### Fixed

- **Review-hardening council — capability-based trust, pipeline and wire
  correctness.** The trust layer classifies a tool from its declared
  capability, not its name: a tool is *mutating* unless it declares
  `AGENTC_TOOL_READONLY` (unknown/zero flags ⇒ mutating, fail closed), and a
  tool may declare `AGENTC_TRUST_TOOL_NET` (`AGENTC_EXT_TOOL_NET`, `0x80`) to be
  gated as a network tool. The resolved flags are carried on the appended
  `AgcToolCall.flags` and used by `agentc_trust_gate`/`agentc_trust_decide_flags`
  and the extension host's `policy_eval`. Network narrowing, the approval memo
  key and `policy_id` ownership are now monotone/complete (a child cannot widen
  `allowlist`→`allow` or redirect the approver); a stale parked approval verdict
  is never replayed onto a reused slot; an approver-narrowed `args` is not
  memoized; an approver `ask` no longer executes the tool. The tool-job driver
  is per-invocation (nested `wait:true` subagents and `agentc_pump` no longer
  clobber an outer batch) and the source-ordered result loop no longer reads
  `job_tools[]` out of bounds past 32 calls. External `grep`/`find` re-check the
  trust path gate on every rung and normalise ordering/truncation across
  backends. CBOR→JSON no longer drops empty strings (invalid JSON), escapes NUL
  map keys, and the item budget is single-source; HTTP trailers are kept out of
  the head; `AgcJsonW` latches a depth-overflow failure. `/resume` now replaces
  (not merges) the queue, config/auth writers refuse a present-but-unreadable
  file, and the SDK re-arms its persistence observers after `events_reset`. The
  session server rejects a `--ws` upgrade from a foreign `Origin`, requires a
  token for `--ws`, refuses an unsafe socket dir, compares the token in constant
  time, bounds the handshake buffer and idle peers, and Windows sandbox helpers
  poll before writing; the extension tool-flag mask forwards the network bit.
  Provider presets are enumerated from the registry (one source of truth).
- **Review-fix council A — extension/MCP isolation.** `helper_read_frame` now
  checks its deadline at the top of every loop iteration (a dribbling helper
  cannot hold the host past the budget; clamped `AGENTC_HELPER_TIMEOUT_MS`), and
  both helper and MCP pipe write ends are non-blocking and `CLOEXEC` via the
  appended `os_set_nonblock` seam (the Linux-only `/proc/self/fd` reopen is
  gone). A dead helper is reaped with SIGKILL escalation and its slot reused.
  MCP stream classification is gated on a complete, valid head (`head_valid`);
  non-2xx SSE is not a usable stream, and the GET resumption id is its own
  field. SSE ignores a NUL in `id:` and strips a leading BOM. Design/30 §2.1
  records the still-missing helper production entry.
- **Review-fix council B — per-agent routing, session and config safety.**
  Route resolution is per-agent (`AgcAgentSpec.route`, an `AgcRouteSeam`) and
  never reads a process global; an alias is a remembered fact on the agent and
  fails closed (`-ENOENT`) if its row is cleared. A routed target uses the
  agent's own endpoint/credential when the provider is unchanged and fails
  closed without a key otherwise; a cross-provider child no longer inherits the
  parent's credential. `/new`/`/fork` refuse a degraded memory-only session,
  `/resume` clears then replays the queue, and `/new` starts empty. Config
  writers refuse to destroy a non-empty unparseable `setup.jsonc`/`auth.jsonc`
  and merge `providers.<id>` siblings; `--list-models` and the setup picker list
  virtual-model aliases; atomic writes use `O_EXCL` with a bounded EEXIST retry.
- **Review-fix council C — alias-aware `/model` and docs sync.** `/model
  <alias>` was misread as another provider's model and refused with a broken
  `--provider` remediation; it now switches in place (step routing resolves the
  concrete target) and reports `model: <alias> (alias -> provider/model)`. The
  `/model` picker lists aliases first with that label, ahead of the provider
  catalog, and reports the route on accept. `--help` now says `--thinking`
  defaults to **medium** (matching the code, README and design docs; it said
  "default off") and `--model` accepts an alias. Docs: design/40 §6.1/§6.3
  document the alias-aware picker and `/fork`/`/tree`; the stale "split on the
  first `/`" contract comments in `include/agent.h` and `src/prov/models.c` name
  `route_provider`/`route_model` as authoritative; design/20 §3.7/§7.2 and
  design/70 describe the per-agent route seam and the config-publish side effect
  and no longer advertise `--model provider/id[:thinking]`; the stale "virtual
  models/routing deferred" row is removed from design/30 §6; the concept is
  named "virtual model (alias)" on first use.

## [0.8.0] - 2026-10-15

The P6 runtime and the P5 axes: the actor/runtime core, trust and sub-agents,
and the language-independent control surfaces (RPC v1, the framed CBOR protocol
and the embeddable SDK).

### Added

- **Runtime, actors and processes (P6).** A bounded `agent | process` actor
  registry over pipes and PTYs with
  `spawn|write|read|resize|kill|freeze|resume|list`, spill-backed output and
  `STALLED` handling; the model-facing `process` tool is an adapter and `bash`
  spawns through the actor. A resumable scheduler advances a background agent
  one model step per actor tick (`AGENTC_RUN_YIELD`).
- **Messages, queue, steer and cancel (P6).** A bounded message bus and unified
  priority queue with a documented drop order, `notify`/steer/interrupt, and
  graceful-to-force cancellation.
- **Hooks (P6).** The universal ten-stage hook pipeline
  (`turn/step/input/prompt/message/output/tool/signal/event/lifecycle`) with two
  verbs (`next`/`defer`) and the B8 fold of every legacy `on(point,…)` override
  onto it.
- **Trust, approvals and sandbox (P6).** Inherited monotone trust envelopes,
  four enforcement layers, per-call rules, `approvals.jsonc` project memos, the
  approval contract (interactive UI / policy program / `tool_approval` / async
  core hook) and the Layer-0 sandbox spec.
- **Sub-agents and deciders (P6).** One unified agent constructor, the agent
  tree, the `agent` tool, and the LOCAL/PROGRAM/MCP/MODEL decision backends with
  message lineage and persisted `custom type:"agent"` records.
- **Settings, status and voice (P6).** Extension-declared grouped settings,
  anchor/REPLACE prompt contributions, status lines in every region, and the
  realtime-voice extension with its framed helper codec.
- **RPC v1 (`--mode rpc`).** A frozen JSONL record layer (`AGENTC_RPC_VERSION`
  1): acceptance-first prompts, queued commands while a run is active,
  `agent_settled`, `get_commands`, count-first model listing, cursor-paged
  `get_messages` and the fire-and-forget UI sink.
- **Framed CBOR protocol (`agentc serve`/`agentc-client`).** A `u32`-length frame
  around strict-JSON CBOR envelopes, over a Unix socket (loopback TCP fallback),
  `--host`/`--remote`, an RFC 6455 WebSocket upgrade and vendored-mbedTLS TLS
  (Linux). `cancel` is out of band; `ui_request`/`ui_response` dialogs and tool
  approvals are parked fail-closed; `attach`/`detach` route events;
  `agentc-client` speaks it synchronously with explicit reconnect/resync.
- **Multi-session `serve`.** A bounded registry of up to 4 independent,
  serialized engines per process, each with its own agent and transcript;
  `new_session`/`attach` move a connection between them.
- **`libagentc` SDK.** `make lib` builds a PIC archive that embeds the engine
  (`include/agentc_sdk.h`, one engine / one agent per process); `make libcheck`
  proves the deny-lists, a default-PIE host link and a stable arena; `make
  libstrict` proves the archive contains no `src/app/*.o`; `make install-lib`
  installs the headers, library and `agentc.pc`.

### Changed

- The extension ABI stays `AGENTC_EXT_ABI 1`; the R-D29/R-D30 vtable renames
  (`AgcExtSection`→`AgcExtPrompt`, the provider status pair→`AgcExtStatusLine`)
  are an in-place pre-release recompile. New events, host slots, limits and
  config fields are appended behind the existing growth rules.
- The R-D31 terminology rename is applied: the model cycle is `step`
  (`AGENTC_EV_STEP_START`/`_END`, `step_index`, the `step` hook stage and the
  `step_start`/`step_end` extension points), and the user cycle is `turn` (the
  `AGENTC_EV_AGENT_START`..`AGENTC_EV_AGENT_END`/`agent_settled` span, the `turn`
  hook stage). The `--mode rpc` wire names `turn_start`/`turn_end` are frozen.
- The per-user tree prefers `$XDG_*/abird/agentc`; when it has never been
  created and the legacy `$XDG_*/agentc` exists, the legacy path is used
  transparently for reads and writes (one notice per process), so existing
  config, auth, sessions, themes and history are not orphaned by the move.
- The `libagentc` archive is strict: `policy.c`/`project.c`/`select.c`/
  `runsession.c` moved into `src/core`, and `LIB_EXCLUDE` drops all of
  `src/app/`.

### Deferred

- True multi-session concurrency: sessions are bounded and serialized because
  the extension/prompt/auth/settings registries, the runtime tick and the
  HTTP/UI sinks stay process-global.
- Platform sandboxes: the Windows restricted-token and macOS Seatbelt backends
  and the Linux host allowlist remain fail-closed and documented.

[0.8.0]: https://github.com/abird-ai/agentc/compare/v0.7.0...v0.8.0

## [0.7.0] - 2026-10-13

### Added

- **Process actors (P6-A).** A bounded actor registry with a process kind:
  `spawn|write|read|resize|kill|freeze|resume|list` over pipes and PTYs, with a
  spill file for the full stream and `STALLED`/cancel/freeze handling. The
  model-facing `process` tool is a thin adapter; `bash` spawns through the actor
  while keeping its legacy output and job-driver scheduling.
- **TUI transcript bands.** User prompts, error notices and tool cards render as
  full-width, padded background bands: muted blue for prompts, muted red for
  errors, and the existing outcome tints for tool cards (green ok / red err /
  grey pending). Output text uses the default foreground; user prompts no longer
  carry a `>` marker.
- **TUI scrollbar.** A minimal muted thumb in a reserved right-hand gutter, plus
  mouse-wheel scrolling.
- **TUI header/exit trace.** The intro header is the first transcript block, and
  the compact exit dump keeps the whole last message (not just its last row).

### Changed

- **TUI is fullscreen-only.** The interactive view always uses the alternate
  screen; the inline and scrollback renderers and the `tui_mode` config key are
  gone.

### Fixed

- Exiting an empty session prints just the banner — no repeated header and no
  dangling `...` separator.
- Markdown hard breaks open the next row instead of joining the previous one.

[0.7.0]: https://github.com/abird-ai/agentc/compare/v0.6.0...v0.7.0
