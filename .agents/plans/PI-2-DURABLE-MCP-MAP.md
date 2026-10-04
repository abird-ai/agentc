# pi 2.0 (monorepo) — durable, MCP, and the new extension surface

Reference for the extension work. Source: the pi monorepo checkout at
`/home/pvl/spaces/abird/research/pi` (all packages `1.0.0`, `main`), specifically
`packages/durable/src/index.ts`, `packages/mcp/src/index.ts` and
`packages/coding-agent/src/core/extensions/types.ts`. Complements
`PI-EXTENSION-MAP.md` (which covered the shipped 1.0.0 `pi.*` API).

Legend for **agentc**: `yes` / `partial` / `plan` / `gap` / `n/a`, as before.

---

## 0. What is "pi 2.0" here

The monorepo splits one binary-era agent into packages:

| package | role |
|---|---|
| `pi-ai` | providers, models, messages, streams |
| `pi-agent-core` | the low-level agent loop |
| `pi-coding-agent` | CLI, TUI, sessions, **extensions (`pi.*`)** |
| `pi-durable` | **durable execution harness** (new) |
| `pi-mcp` | **MCP client** (new) |
| `pi-protocol` / `pi-client` / `pi-server` | remote/RPC + session protocol |
| `pi-codemode` | codemode scripting |
| `pi-chord` | replicated/observable state |
| `pi-tui`, `pi-telemetry`, `pi-evals` | UI, telemetry, evaluation |

**Important:** the coding-agent extension API (`pi.on`, `registerTool`, the 41
events) is **byte-for-byte the same** as the shipped 1.0.0 (verified against
`types.ts` line 1540). So for agentc, "pi 2.0 additions" are **not new events** —
they are the `durable` and `mcp` package APIs, plus a **second extension model**
inside `pi-durable`.

---

## 1. pi-durable — full API

`@earendil-works/pi-durable` is a durable, resumable, transactional agent harness.
Everything is committed through a `Session`; tasks survive restart; conversations
fork; documents are typed and versioned.

### 1.1 Extension model (durable)

| Export | What it is | agentc mapping |
|---|---|---|
| `defineExtension(ext)` | identity fn typing `Extension`; `{ name, tools?, sections?, hooks?, wraps?, tasks? }` | proposed `AgcExtension` — close match |
| `defineTool(tool)` | identity fn typing `ToolRegistration` (`pi-ai Tool` + replay/limits/hooks) | `AgcTool` — close |
| `section(key, render, {tag})` | a system-prompt section rendered per request (tagged `<key>…</key>`) | proposed prompt registry |
| `hook(task, handlers)` | typed per-task hook handlers, matched by task name | proposed hooks (typed, but agentc groups by event name) |
| `wrapTool(tool, fn)` | middleware around a tool where the extension is selected | gap (agentc has no wrapping/middleware) |
| `wrapSection(key, fn)` | middleware around a prompt section | gap |
| `createRegistry()` | app-owned `Registry` of extensions; `install`/`uninstall`/`snapshot`/`subscribe` | proposed extension registry — close |

`Extension` fields: `name`, `tools`, `sections`, `hooks`, `wraps`, `tasks`. This
validates the agentc plan's shape almost exactly (tools + prompt sections + hooks)
and adds **wrapping** and **per-extension durable tasks**.

### 1.2 Harness

| Export | What it does | agentc mapping |
|---|---|---|
| `Harness` (class) | durable agent over one `Session`: `resume`, `root`, `createConversation`, `conversation`, `getTask`, `inspect`, `submission`, `abortSubmission`, `abortTask`, `waitForTask`, `waitForIdle`, `usage`, `taskGraph`, `watchTaskGraph` | `agentc_agent_*` — partial (no durability/task graph) |
| `HarnessOptions` | `models`, `registry`, `settings?`, `env?`, `conversationCreated?`, `now?`, `onReport?` | config + plugins — partial |
| `HarnessSettings` / `Settings` | stream/retry/compaction/toolExecution/steering/followUp modes | `config.jsonc` — partial |
| `HarnessInspection` / `TaskInspection` | point-in-time view of live tasks + submissions, scheduler state | gap |
| `AgentDoc` / `configure` / `LiveDoc` / `InboxDoc` / `UsageDoc` | built-in durable documents (agent state, live run state, queue, usage) | session entries — partial |
| `DEFAULT_COMPACTION_POLICY` / `DEFAULT_RETRY_POLICY` | defaults | config defaults — partial |

### 1.3 Conversation

| Export | What it does | agentc mapping |
|---|---|---|
| `Conversation` (iface) | `agent`, `configure`, `submit`, `reset`, `compact`, `commit`, `context`, `entries`, `fork`, `abort`, `waitForIdle`, `viewState`, `watch` | `agentc_agent_*` + `/fork` — partial |
| `ConversationHandle` | invocation-bound `submit`/`abort`/`waitForIdle` | gap |
| `ConversationCreateOptions` / `ConversationInit` | ownership, agent change, init callback | gap |
| `createSession(storage)` | the session kernel: one mutation line, documents, publication | `core/session.c` — partial (JSONL, no transactions) |
| `ConversationView` / `watch` | observable structural view | TUI state — partial |

Queue semantics: `whenBusy: "steer" | "followUp" | "reject"`, `QueueMode`
`all | one-at-a-time`, `steeringMode`/`followUpMode` settings. agentc has
`agent_steer`/`agent_followup` — **partial** (no persisted queue, no modes).

### 1.4 Tasks (durable execution)

| Export | What it does | agentc mapping |
|---|---|---|
| `defineTask({name, version, initial, phases, abort, migrate?, hooks?})` | a durable state machine: phases, abort handler, migration | gap |
| `GenerationTask` / `GenerationHooks` | the built-in model-run task; hooks `beforeRequest`, `afterResponse`, `onYield` (continue), `afterTools` | agent loop + hooks — partial |
| `ToolTask` / `ToolHooks` | the built-in tool task; `beforeTool` (block/replace args), `afterTool` (replace result) | jobs + `tool_call`/`tool_result` — partial |
| `CompactionTask` / `CompactionHooks` | compaction task; `beforeCompact` (decline/summarize) | compaction — partial |
| `TaskGraph` / `TaskGraphWatch` / `taskGraph()` | live task/ownership graph | gap |
| `SettledTask` / `AnyTask` / `TaskOutcome` / `TaskState` / `TaskRuntime` | task records, outcomes, runtime API | gap |

`ToolExecutionApi` (passed to tools): `taskId`, `conversationId`, `callId`,
`registry`, `agent()`, `env`, `output()`, `diagnostic()`, `details()`, `commit()`,
`memo()`, `createTask()`, `getTask()`, `waitForTask()`, `conversation()`. agentc's
`AgcJob` gives `out`/cancel only — **gap** for diagnostics/details/commit/memo/
child tasks.

`ToolExecutionResult`: `content`, `isError`, `details`, `diagnostics`, `usage`,
`control` (`addTools`, `terminate`, `handoff`). agentc result = text + is_error —
**partial**; `control.terminate` and `control.handoff` are gaps.

`ToolRegistration` extras: `replay` (`safe`/`unsafe` on recovery),
`executionMode`, `prepareArguments`, `outputLimits` (`maxBytes`/`maxLines`/`retain`).
agentc has `timeout_ms`/sequential and truncation config — **partial**.

### 1.5 Documents, entries, transactions, storage

| Export | What it does | agentc mapping |
|---|---|---|
| `defineDoc` / `defineDocFamily` | typed, versioned, scoped documents (session/conversation/task) + history | gap (custom session entries are untyped) |
| `Tx` | transaction handle: `entry`, `task`, `create*`, `write*`, document reads | gap |
| `Storage` | `commit(writes)`, `mintId`, `conversation`, `scanConversations`, `entry`, `scanEntries`, `findLatestHeadMarker`, `task`, `scanTasks`, `scanSubmissions`, … | `core/session.c` — partial |
| `MemoryStorage`, sqlite, jsonl storages | backends | JSONL only — partial |
| `defineEntry` / `UserEntry` / `AssistantEntry` / `SystemEntry` / `ToolResultEntry` / `ResetEntry` / `CompactionEntry` | typed transcript entries | `Msg` roles — partial |
| `watchEvents` / `AgentEvent` / `AgentEventStream` / `MessageChange` / `SnapshotEvent` | durable event stream observation | `core/events.c` — partial (no durable stream) |
| `ConversationBusy` / `ReadAfterWrite` / `StorageRejected` | typed errors | errno — partial |

---

## 2. pi-mcp — full API

`@earendil-works/pi-mcp` is a standalone MCP client: tools **and resources and
prompts**, stdio + Streamable HTTP, OAuth, and an in-memory test transport.

### 2.1 Client

| Export | What it does | agentc mapping |
|---|---|---|
| `McpClient` (class) | one MCP connection over a transport | `mcp.c` server conn — partial |
| `new McpClient({name, version, capabilities?, protocolVersion?, requestTimeoutMs?, roots?})` | client identity + capabilities + roots | fixed `clientInfo`, `capabilities: {}` — partial |
| `connect(transport)` | initialize handshake; returns `InitializeResult` | `mcp_server_connect` — yes |
| `listTools(opts)` | tools with `inputSchema`/annotations | `agentc_mcp_parse_tools` — yes |
| `callTool(name, args, opts)` | invoke a tool | `mcp_run` — yes |
| `listResources` / `listResourcesPage` / `readResource(uri)` | **resource** enumeration + read | **gap** |
| `listResourceTemplates` / `listResourceTemplatesPage` | **resource templates** | **gap** |
| `ping(opts)` | liveness | gap |
| `notify(method, params)` | send a notification | partial |
| `setRequestHandler(method, fn)` | answer server→client requests (sampling, roots) | gap |
| `onNotification(method, fn)` | subscribe to server notifications (`tools/list_changed`, …) | `agentc_mcp` re-sync — partial |
| `onError(fn)` / `onClose(fn)` | lifecycle listeners | partial |
| `close()` | shutdown | `agentc_mcp_shutdown` — yes |
| `McpRequestOptions` | `signal`, `timeoutMs`, `onProgress` | cancel + timeout — partial (no progress) |

### 2.2 Transports

| Export | What it does | agentc mapping |
|---|---|---|
| `StdioTransport` / `StdioTransportOptions` | child-process JSON-RPC lines | `mcp.c` stdio — yes |
| `StreamableHttpTransport` / options | HTTP + SSE, sessions, reconnect | `mcp.c` HTTP — yes |
| `McpHttpError`, `McpAuthRequiredError`, `McpSessionExpiredError` | typed HTTP errors | partial |
| `McpTransport` + message/error/close listeners | transport interface | internal — partial |
| `InMemoryTransport` / `createInMemoryTransportPair` (testing) | in-process transport pair for tests | gap (agentc uses a python mock) |

### 2.3 Protocol

| Export | What it does | agentc mapping |
|---|---|---|
| `McpClient` protocol types | `Tool`, `ToolAnnotations`, `ToolExecution`, `Resource`, `ResourceTemplate`, `ListToolsResult`, `ListResourcesResult`, `ReadResourceResult`, `ServerCapabilities`, `ClientCapabilities`, `InitializeParams/Result`, `Root`, `ProgressNotification`, `CancelledNotification`, `Implementation` | partial (agentc parses tools only) |
| `ContentBlock` union | `TextContent`, `ImageContent`, `AudioContent`, `EmbeddedResourceContent`, `ResourceLinkContent`, `BlobResourceContents`, `TextResourceContents`, `LlmContent`, `CallToolResult`, `toLlmContent` | text-only — partial |
| JSON-RPC: `parseJsonRpcMessage`, `isJsonRpc*`, `JSON_RPC_ERROR_CODES`, `JsonRpc*` types | framing | `mcp_int.h` — yes |
| `McpError`, `McpAbortError`, `McpTimeoutError`, `McpConnectionClosedError` | error taxonomy | errno — partial |
| `LATEST_PROTOCOL_VERSION`, `SUPPORTED_PROTOCOL_VERSIONS` | protocol negotiation | `2024-11-05` — partial |

### 2.4 OAuth (MCP)

| Export | What it does | agentc mapping |
|---|---|---|
| `McpOAuthProvider` / `McpOAuthProviderOptions` / `McpOAuthState` / `McpOAuthStateStore` / `MemoryOAuthStateStore` | OAuth provider + state | gap |
| `authorizeMcp`, `startAuthorization`, `exchangeAuthorizationCode`, `refreshAuthorization`, `registerClient` (DCR), `stepUpScope` | full OAuth 2.1 + PKCE + DCR + step-up | gap (agentc provider OAuth only) |
| `discoverAuthorizationServerMetadata`, `discoverProtectedResourceMetadata`, `discoverOAuthServerInfo`, `parseWwwAuthenticate`, `selectResource`, `resourceUrlFromServerUrl` | discovery | gap |
| `OAuthCallbackServer` / pages | loopback callback | `core/oauth.c` has a loopback server for providers — partial |
| `adaptOAuthProvider`, `AddClientAuthentication`, `TokenRequestOptions` | bridges | gap |
| `AuthProvider` / `McpFetch` / `UnauthorizedContext` | bearer supply + refresh-on-401 | gap |
| OAuth error types | typed | gap |

---

## 3. New additions (relative to shipped 1.0.0), separately

Nothing below existed in the `pi.*` API mapped in `PI-EXTENSION-MAP.md`; the
coding-agent extension event/method list is unchanged.

### 3.1 New capability areas

1. **Durable execution** — `Session`/`Tx`/`Storage`, typed/versioned documents,
   transaction commits, resume after restart, fork, exact entry history,
   rebuildable views (`docs/durable` semantics).
2. **Durable tasks** — `defineTask` with phases, `abort`, `migrate`, typed hooks,
   task ownership graph, child tasks (`ToolExecutionApi.createTask`), budgets and
   `usage`; `TaskGraph`/`inspect`.
3. **A second, typed extension model** — `Registry` + `Extension{tools, sections,
   hooks, wraps, tasks}` with `defineExtension`/`defineTool`/`section`/`hook`/
   `wrapTool`/`wrapSection`. Distinct from the `pi.on("event")` string model.
4. **Prompt sections as first-class contributions** — with wrapping middleware.
5. **Typed per-task hooks** — `GenerationHooks`, `ToolHooks`, `CompactionHooks`
   instead of string event names.
6. **Richer tool results** — `details`, `diagnostics`, `usage`, `control`
   (`addTools`, `terminate`, `handoff`), `replay`, `outputLimits`.
7. **MCP resources and prompts** — `listResources`/`readResource`/
   `listResourceTemplates`; content blocks incl. images/audio/embedded resources.
8. **MCP OAuth** — discovery, DCR, PKCE, loopback callback, step-up scopes,
   refresh-on-401, `AuthProvider`.
9. **MCP in-memory transport + protocol constants** for testing and negotiation.
10. **Remote/observable runtime** — `pi-protocol`, `pi-client`, `pi-server`,
    `pi-chord` (replicated state, watches), `pi-codemode`, `pi-telemetry`.

### 3.2 Mapping the additions to agentc

| New pi 2.0 area | agentc today | agentc plan |
|---|---|---|
| Durable `Session`/`Tx`/documents | JSONL sessions, `append_entry` | out of scope (agentc is a local CLI, not a durable service) — `n/a` unless abird needs it |
| Durable tasks / task graph / child tasks | none | `n/a` for agentc core; relevant to abird's durable agency, not this agent |
| Typed extension model (Registry/Extension) | `AgcHostV1` tools/events | **adopt the shape** — this is the `AgcExtension` in `EXTENSION-TOOL-ENGINE.md` |
| `section` + `wrapSection` | prompt build in `prompt.c` | adopt sections; **wraps are a genuine new idea** worth adding |
| `hook(task, handlers)` typed | string events | agentc keeps string event names but typed payloads/caps |
| Tool `details`/`diagnostics`/`usage`/`control` | text + is_error | adopt `diagnostics`, `usage`, `terminate`; `handoff`/`addTools` plan |
| `replay`, `outputLimits` | timeouts + truncation | adopt `outputLimits`; `replay` only with durable tasks |
| MCP tools | `mcp.c` | yes |
| MCP resources | none | **gap — add `listResources`/`readResource`** |
| MCP prompts | none | **gap — the skills payoff** (`EXTENSION-TOOL-ENGINE.md` §5.3) |
| MCP OAuth | none (provider OAuth only) | gap; relevant for hosted MCP servers |
| MCP in-memory transport | python mock in tests | nice-to-have for `tests/mcp.sh` |
| Protocol/client/server, chord, codemode | RPC mode + codemode excluded | `n/a` (deliberately out of scope) |

### 3.3 The one structural lesson

pi 2.0 now has **two** extension models: the session/UI `pi.on("event")` API and
the durable harness `Registry`/`Extension` API. agentc's proposal deliberately has
**one**. The durable model is the closer ancestor of `AgcExtension` (typed tools +
sections + hooks), so agentc should take its **shape** (sections, wraps, typed
tool results) without taking its **machinery** (durable tasks, transactions,
storage) — that machinery is a service concern, not a coding-agent one.

---

## 4. Summary

- **pi-durable**: a durable harness — `Session`/`Tx`/documents, `Harness`,
  `Conversation`, `defineTask` phases/migrate, `Registry`+`Extension`
  (tools/sections/hooks/wraps/tasks), typed hooks (`Generation`/`Tool`/
  `Compaction`), rich tool results. agentc maps the **extension shape** closely and
  has none of the **durability**.
- **pi-mcp**: a full MCP client — tools, **resources**, **prompts**, stdio +
  Streamable HTTP, **OAuth**, in-memory testing. agentc implements only tools +
  the two transports.
- **New additions**: durability, durable tasks, a typed extension model, prompt
  sections + wraps, typed hooks, richer tool results, MCP resources/prompts/OAuth.
  The coding-agent event/method surface itself is unchanged from 1.0.0.
- **Biggest agentc gaps worth closing**: MCP resources/prompts, `section`/`wrap`,
  tool `diagnostics`/`usage`/`control`, and the `Registry`/`Extension` shape (which
  the pending `AgcExtension` already mirrors). Durability and remote runtime are
  out of scope for the coding agent.
