# pi ↔ agentc extension surface map

Reference for the extension work in `EXTENSION-TOOL-ENGINE.md`. pi's surface is
read from the shipped `dist/core/extensions/types.d.ts` (the `ExtensionEvent`,
`ExtensionAPI`, `ExtensionContext` and `ToolDefinition` unions) plus
`docs/extensions.md`; agentc's from `include/agentc_plugin.h`, `include/plugin.h`
and the emission sites in `src/`.

Legend for **agentc status**:

- `yes` — emitted/available today
- `partial` — present but weaker (observe-only, or a sub-field missing)
- `plan` — designed, not implemented (see `EXTENSION-TOOL-ENGINE.md` §4.9)
- `gap` — no agentc equivalent and no current plan
- `n/a` — pi feature agentc deliberately does not have
- `fold` — pi's event/call is subsumed by a broader agentc hook

pi canonicalization: **superseded by D5.** The extension spec uses pi hook
names verbatim; the earlier `<subject>_<phase>` rename proposal was rejected and
its column has been deleted. The authoritative names are in
`.agents/plans/EXTENSION-SPEC.md` (see handoff D5) and `include/ext.h`.

---

## 1. Events (41)

| # | pi event | What it does in pi | agentc today | status |
|---|---|---|---|---|
| 1 | `project_trust` | decide whether project resources load; `yes`/`no`/`undecided` + remember | `project_trust` (emitted) | yes |
| 2 | `resources_discover` | after `session_start`, add skill/prompt/theme paths | reserved | plan P4 |
| 3 | `session_start` | session started/loaded/reloaded; carries reason + prior file | emitted | yes |
| 4 | `session_info_changed` | session display name changed | — | gap |
| 5 | `session_before_switch` | before switching session; cancellable | — | gap |
| 6 | `session_before_fork` | before forking; cancellable | — | plan |
| 7 | `session_before_compact` | before compaction; cancel or supply the summary | `compact` (observe, different shape) | plan P2 |
| 8 | `session_compact` | compaction succeeded | `compact` | plan P2 |
| 9 | `session_compact_failed` | compaction failed/aborted | — | plan P2 |
| 10 | `session_shutdown` | runtime torn down (quit/reload/new/fork) | emitted | yes |
| 11 | `mcp_servers_change` | extension registered/unregistered an MCP server | reserved | plan P4 |
| 12 | `session_before_tree` | before session-tree navigation; cancellable | — | gap |
| 13 | `session_tree` | after tree navigation | — | gap |
| 14 | `context` | transform conversation messages (no system) before the LLM call | — | fold |
| 15 | `context_with_system` | same but the full transcript, handler owns prompt+tools | — | fold |
| 16 | `cache_warming_decision` | override an idle prompt-cache refresh | — | n/a |
| 17 | `before_provider_request` | replace the request payload | emitted, observe-only | partial → P2 |
| 18 | `before_provider_headers` | mutate assembled request headers in place | — | plan P2 |
| 19 | `after_provider_response` | observe status/headers before consuming the stream | emitted, observe-only | yes |
| 20 | `provider_stream_event` | each parsed provider stream event before normalization | emitted, observe-only | yes |
| 21 | `before_agent_start` | rewrite the user prompt / system-prompt sections before the run | reserved | plan P2 |
| 22 | `agent_start` | low-level agent run begins | emitted | yes |
| 23 | `agent_end` | run ends; carries the messages | emitted | yes |
| 24 | `agent_before_settle` | final boundary; append entries, request one continuation | — | plan P2 |
| 25 | `agent_settled` | run fully settled; no retry/compaction/continuation will run | — | plan P2 |
| 26 | `ui_prompt_start` | a blocking extension UI prompt begins | — | plan P6 |
| 27 | `ui_prompt_end` | a blocking extension UI prompt ends | — | plan P6 |
| 28 | `turn_start` | a turn begins | emitted | yes |
| 29 | `turn_end` | a turn ends; inject entries, request one continuation | emitted, observe-only | partial → P2 |
| 30 | `message_start` | a message begins (user/assistant/tool) | emitted | yes |
| 31 | `message_update` | token-by-token streaming update | emitted | yes |
| 32 | `message_end` | a message ends; can replace the finalized message | emitted, observe-only | partial → P2 |
| 33 | `tool_execution_start` | a tool starts executing | emitted | yes |
| 34 | `tool_execution_update` | partial/streaming tool output | `update` callback exists; not bridged to events | partial |
| 35 | `tool_execution_end` | a tool finishes | emitted | yes |
| 36 | `model_select` | a new model was selected | reserved | plan |
| 37 | `thinking_level_select` | a new thinking level was selected | reserved as `thinking_level_change` | plan |
| 38 | `tool_call` | mutate args or block execution; `terminate` hint | emitted + veto | partial → P2 |
| 39 | `tool_result` | transform content/structuredContent/isError/usage | emitted, observe-only | partial → P2 |
| 40 | `user_bash` | user ran `!`/`!!`; extension can supply operations | — | n/a |
| 41 | `input` | transform or handle user input before the loop | reserved | plan P2 |

**agentc-only events** (not pi): the old `error`/`compact` bridge names are no
longer emitted; extension-emitted unknown points are inert (no table row, `on`
is rejected). The pi-named `agent_end` / `session_compact` /
`session_compact_failed` points are the live ones.

---

## 2. Extension API — `pi.*` (31 methods)

| pi method | What it does | agentc host (v2) | status |
|---|---|---|---|
| `on(event, handler)` | subscribe to an event/hook; returns unsubscribe | `host->on_event` | partial → P2 |
| `registerTool(tool)` | register a callable tool | `host->register_tool` | yes |
| `registerCommand(name, opts)` | register a slash command | `host->register_command` | yes |
| `registerShortcut(key, opts)` | register a keyboard shortcut | — | gap |
| `registerFlag(name, opts)` | register a CLI flag | — | gap |
| `getFlag(name)` | read a CLI flag | — | gap |
| `registerMessageRenderer(type, fn)` | custom renderer for a custom message | — | gap |
| `registerMarkdownTransformer(fn)` | transform user/assistant markdown before render | — | gap |
| `registerEntryRenderer(type, fn)` | custom renderer for a custom session entry | — | gap |
| `sendMessage(msg, opts)` | inject a custom message; optionally trigger a turn | `host->append_entry` (entry only) | partial |
| `sendUserMessage(content, opts)` | send a user message; queues steer/followUp | — | gap |
| `appendEntry(type, data)` | persist a custom session entry | `host->append_entry` | yes |
| `setSessionName(name)` | set the session display name | — | gap |
| `getSessionName()` | get the session display name | — | gap |
| `setLabel(entryId, label)` | bookmark an entry | — | gap |
| `exec(cmd, args, opts)` | run a shell command | — | gap |
| `getActiveTools()` | names of tools declared to the model | internal bitset only | gap |
| `getAllTools()` | all tools with schema/exposure/source | `host->register_tool` is write-only | gap |
| `setActiveTools(names)` | set the model-visible tool set | internal only | gap |
| `getSettings()` | effective merged settings | — | plan |
| `getCommands()` | available slash commands | — | gap |
| `setModel(model)` | switch model (auth-checked) | `host->set_model(provider, model)` | yes |
| `getThinkingLevel()` | current thinking level | — | gap |
| `setThinkingLevel(level)` | set thinking level | `host->set_thinking_level` | yes |
| `registerProvider(name, cfg)` | add/override a model provider (models, baseUrl, oauth, stream) | — | gap |
| `unregisterProvider(name)` | remove a provider | — | gap |
| `registerMcpServer(name, cfg)` | register an MCP server for the session | file config only | gap → P4 |
| `unregisterMcpServer(name)` | remove an extension's MCP server | — | gap → P4 |
| `getMcpServers()` | every extension-registered MCP server | — | gap → P4 |
| `registerVirtualModel(m)` | register a routing virtual model | — | n/a (v2) |
| `unregisterVirtualModel(p, id)` | remove a virtual model | — | n/a (v2) |
| `events` (EventBus) | inter-extension pub/sub | — | gap |

Async note: pi handlers return `Promise` and are awaited (stream order for
`provider_stream_event`). agentc is a C ABI with no coroutines: observe/veto/
transform hooks are synchronous and budgeted; I/O-bound hooks defer
(`EXTENSION-TOOL-ENGINE.md` §4.5).

---

## 3. Extension context — `ctx.*`

| pi field/method | What it does | agentc host (v2) | status |
|---|---|---|---|
| `ui` | dialog/widget/status surface | `notify`/`set_status`/`set_title` | partial |
| `mode` | `tui`/`rpc`/`json`/`print` | — | gap |
| `hasUI` | whether dialog UI is available | — | gap |
| `cwd` | working directory | `host->cwd` | yes |
| `sessionManager` | read-only session manager | `session_id`/`session_file` | partial |
| `modelRegistry` | API-key/model resolution | — | gap |
| `model` | current model | — (set-only) | gap |
| `scopedModels` | models enabled for this session | — | gap |
| `thinkingLevel` | current thinking level | — | gap |
| `isIdle()` | agent not streaming | — | gap |
| `isProjectTrusted()` | project trust active | — | gap |
| `signal` | abort signal | `is_cancelled(signal_token)` | partial |
| `abort()` | abort the run | — (internal `agent_abort`) | gap |
| `hasPendingMessages()` | queued steer/followUp present | — | gap |
| `shutdown()` | exit the process | — | gap |
| `getContextUsage()` | tokens/window/percent | — | gap |
| `compact(opts)` | trigger compaction | — | gap |
| `getSystemPrompt()` | effective system prompt | `host->system_prompt` | yes |
| `tools` + `executeTool(name,args)` | nested tool calls with full pipeline | — | gap (v2) |
| command ctx: `getSystemPromptOptions` | structured system-prompt options | — | gap |
| command ctx: `waitForIdle()` | await idle | — | gap |
| command ctx: `newSession()`/`fork()`/`navigateTree()`/`switchSession()`/`reload()` | session control | — | gap |
| `sendMessage`/`sendUserMessage` (replaced ctx) | inject into a new session | — | gap |

### `ctx.ui` (30 methods)

| Group | pi methods | agentc mapping |
|---|---|---|
| dialogs | `select`, `confirm`, `input`, `editor`, `custom` | — (RPC `ui_request/response` reserved) |
| notify | `notify` | `host->notify` — yes |
| status | `setStatus` | `host->set_status` — yes |
| working | `setWorkingMessage`, `setWorkingVisible`, `setWorkingIndicator`, `setHiddenThinkingLabel` | partial (spinner state) |
| widgets | `setWidget`, `setFooter`, `setHeader`, `setTitle` | `set_title` yes; widgets/footer/header gap |
| editor | `pasteToEditor`, `setEditorText`, `getEditorText`, `setEditorComponent`, `getEditorComponent`, `addAutocompleteProvider` | gap |
| theme | `theme`, `getAllThemes`, `getTheme`, `setTheme` | themes exist in config; no extension API |
| tool view | `getToolsExpanded`, `setToolsExpanded` | gap |
| input | `onTerminalInput` | gap |

agentc's status line is a provider-built segment table (`status_register`), which
is *stronger* than pi's keyed `setStatus` for layout; the plan folds `set_status`
into that table (`EXTENSION-TOOL-ENGINE.md` §4.4).

---

## 4. Tool definition fields

| pi `ToolDefinition` | What it does | agentc `AgcTool` / `AgcToolV1` | status |
|---|---|---|---|
| `name`, `label`, `description` | identity + model-facing text | same | yes |
| `promptSnippet` | one-line entry in the tools prompt section | `AgcToolV1.prompt_snippet` | yes |
| `promptGuidelines` | bullets in the Guidelines section | `AgcToolV1.prompt_guidelines` | yes |
| `parameters` (TypeBox) | input schema | `params_json` (JSON Schema string) | yes |
| `outputSchema` | structured-content schema | — | gap |
| `execute(id, params, signal, onUpdate, ctx)` | the implementation | `run` / `start`+`step` | yes |
| `executionMode` | sequential/parallel | `AGENTC_TOOL_SEQUENTIAL` | yes |
| `exposure` | direct/model-only/codemode/deferred/hidden | `AGENTC_TOOL_HIDDEN` only | partial |
| `namespace` | group (e.g. `mcp__docs`) | name prefix convention | partial |
| `annotations` | readOnly/destructive/idempotent/openWorld | `AGENTC_TOOL_READONLY/DESTRUCTIVE` (no idempotent/openWorld) | partial |
| `defaultActive` | active on registration | active-set bitset | yes |
| `prepareLoadout(loadout)` | rewrite declared descriptions/hide declarations | — (host uses `prepareLoadout`-like prompt build) | gap |
| `prepareArguments(args)` | pre-validate coercion shim | `agentc_tool_args_*` coercion | partial |
| `constrainedSampling` | provider-side constrained request | — | n/a |
| `renderCall` / `renderResult` | custom TUI rendering | built-in tool cards only | gap |

---

## 5. Hook result types → agentc capabilities

| pi result type | Fields | agentc capability |
|---|---|---|
| `ToolCallEventResult` | `block`, `reason`, `terminate` | veto (transform args); `terminate` gap |
| `ToolResultEventResult` | `content`, `details`, `structuredContent`, `isError`, `usage` | transform (P2) |
| `MessageEndEventResult` | `message` | transform (P2) |
| `BeforeAgentStartEventResult` | `message`, `systemPrompt` | transform + inject (P2) |
| `ContextEventResult` | `messages` | transform, folded |
| `BeforeProviderRequestEventResult` | payload (unknown) | transform (P2) |
| `InputEventResult` | `action: continue/transform/handled` | veto + transform + inject (P2) |
| `BoundaryResult` (`turn_end`, `agent_before_settle`) | `entries`, `continue` | inject + continue (P2) |
| `BeforeProviderHeadersEvent` | mutate `headers` in place | transform (P2) |
| `SessionBefore*Result` | `cancel`, override fields | veto (P2 for compaction) |
| `UserBashEventResult` | `operations` / `result` | n/a |

---

## 6. Summary

- **Events**: 41 pi events. agentc emits 15 (`yes`), 6 are observe-only but need
  transform (`partial`), 13 are reserved/planned, 6 are gaps or deliberately
  n/a. The settle pair, `before_provider_headers`, the compaction trio and the
  `input`/`before_agent_start` transforms are the high-value additions.
- **API**: agentc covers the core (`on`, tool/command registration, model,
  appendEntry, status, system prompt). Big gaps are provider registration, MCP
  server registration, tool query/set, nested `executeTool`, dialogs and
  rendering — most of which are v2.
- **Context**: agentc exposes cwd/session/system prompt; pi's much richer `ctx`
  (idle, abort, usage, compact, session control) is largely absent.
- **Tools**: descriptors map well; pi's `prepareLoadout`, `outputSchema`,
  annotations beyond readOnly/destructive, and custom rendering do not.
