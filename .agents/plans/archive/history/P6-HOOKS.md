# P6-HOOKS — The universal interception pipeline

Status: **as-built (P6-A..E + completion passes 2a-2g).** The universal interception pipeline (all stages and the B8 fold).
As-built note: the outcome is folded into `.agents/design/30-extensibility.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

As-built deviations: `AGENTC_TOOL_NO_HOOKS` / `tools.hooks=off` are declared
but not yet enforced in the stage-emit helper; legacy `on(point,…)` handlers are
not yet folded into the `agentc_hook_run` chain (the point names are aliases and
the legacy bus keeps working); `defers` are an internal token table rather than
`runtime_defer`. Companion to `P6-PRIMITIVES.md`
(four primitives), `P6-SIGNALS.md`, `P6-TRUST.md`, `P6-DECIDERS.md`. Read
`P6-RUNTIME.md` first (R-D1…R-D31).

One generic hook for **every** flow — step, input, prompt, message, output, tool
calls, tool results, signals, events, lifecycle. Internal, external and MCP tools
all pass through the **same** stages; internal tools may opt out. Additional
agents attach as handlers.

The whole surface:

```c
/* return the next message (or NULL to consume); set *defer to park + resume */
typedef int (*AgcHookFn)(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer);

uint64_t agentc_hook(const char *stage, int order, AgcHookFn fn, void *ud);
void     agentc_hook_off(uint64_t handle);
int      agentc_hook_run(const char *stage, AgcMessage *m, AgcMessage **out);
int      agentc_hook_resume(uint64_t token, AgcMessage *m);
bool     agentc_hook_wants(const char *stage);                 /* hot-path guard */
int      agentc_hook_branch(const AgcAgentSpec *spec, AgcMessage *m, uint64_t *defer);
```

Two verbs only. Everything else is a combinator (§3).

---

## 1. Stages (nine, universal)

A stage is a message kind on the hook path. Each stage has sub-kinds so one seam
covers related events.

| stage | kinds | carries | streaming | mutability |
| --- | --- | --- | --- | --- |
| `turn` | `start`, `end`, `steer` | turn index, user input, queue/steer state | no | transform |
| `step` | `start`, `end`, `model_select`, `compact` | step index, model/tools/envelope | no | transform |
| `input` | `user`, `child`, `queue` | the incoming text + context | no | transform / consume |
| `prompt` | `system` | the assembled system prompt | no | transform |
| `message` | `user`, `assistant`, `tool`, `system` | a transcript message before append/serialize | no | transform / consume |
| `output` | `text`, `think`, `args`, `audio`, `item` | streamed deltas (media-agnostic via `content_type`) | **yes (chunks, `final`)** | transform / consume |
| `tool` | `call`, `result` | tool call (name+args+runner) or tool result | no | transform / consume |
| `signal` | (control kind) | a CONTROL message before delivery | no | transform / consume |
| `event` | (`AGENTC_EV_*`) | an event before emit | no | observe-only |
| `lifecycle` | `started/finished/failed/frozen/...` | actor lifecycle | no | observe-only |

Every message carries an opaque `content_type` (`P6-PRIMITIVES.md` §3), so
audio, transcripts and realtime items flow through the same stages as text with
no new stage or kind (`P6-VOICE.md`).

- `turn` is the **per-turn hook** (user-driven cycle: `start`/`end`/`steer`);
  `step` is the **per-step hook** (model cycle: `start`/`end`/`model_select`/
  `compact`). Together they replace the separate observe-only turn/step events
  with transformable seams.
- `tool` unifies `tool_call` + `tool_result` (before/after) and is the single
  seam for **internal, external and MCP** tools (§4).
- Observe-only stages (`event`, `lifecycle`) ignore the return.

## 2. The two verbs

| old verb | now |
| --- | --- |
| `pass` / `observe` | return `m` unchanged |
| `edit` | mutate `m`, return it |
| `replace` | return a new `AgcMessage` |
| `stop` / `consume` | `*next = NULL` |
| `defer` | set `*defer` (runtime continuation) |
| `emit` / `fork` | call `agentc_message_send` (fork = send a copy) |
| `branch` | `agentc_hook_branch(spec, m, &defer)` |

`agentc_hook_branch` spawns/uses an actor per `spec`, sends `m` as its task, and
defers until it finishes; the resumed body is the actor's result (or a parsed
verdict when `spec->verdict`). It is `actor_spawn` + `runtime_defer`, a
helper, not a primitive.

## 3. Decisions are handlers, not stages

There are **no** `decision` / `route` stages. A decision is an ordinary handler
that branches. The `P6-DECIDERS` request/response contract is just the body
schema of that branch.

| decision | stage / kind | handler |
| --- | --- | --- |
| classify & route an input | `input` | branch to a classifier/router; replace the message with routing metadata |
| approve / deny / narrow a tool call | `tool` / `call` | branch to jev; `NULL` = deny, edited `m` = narrow |
| allow a sub-agent spawn | `tool` / `call` (tool `agent`, op `spawn`) | branch to jev; consume = deny; edit clamps the child spec |
| choose model / thinking | `step` / `model_select` | branch or edit |
| compaction | `step` / `compact` | branch or edit/consume |
| steer policy | `input` / `queue` | edit delivery, or consume to hold |
| escalate | any | a handler chains to another via `hook_branch` |

So `P6-DECIDERS.md` becomes: *deciders are hook handlers on `input`/`tool`/
`step`* using `hook_branch`; the point names survive only as the `branch` spec's
intent. This removes two stages and one concept.

## 4. Universal tool execution

Every tool — internal, external, MCP — is one **runner** invoked through the same
`tool` stage:

```c
typedef enum { AGENTC_RUNNER_INTERNAL, AGENTC_RUNNER_PROCESS, AGENTC_RUNNER_MCP } AgcRunnerKind;

typedef struct AgcToolRunner {
    AgcRunnerKind kind;
    /* internal: a C function; process: argv/shell + sandbox; mcp: server+tool */
    int (*run)(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error);
    uint32_t flags;   /* AGENTC_TOOL_* incl. NO_HOOKS, NO_APPROVAL, READONLY, DESTRUCTIVE */
} AgcToolRunner;
```

The pipeline for one call is identical for all kinds:

1. `tool`/`call` hook — envelope decision, approval (branch to jev), args edit,
   or consume (deny).
2. **execute:** `INTERNAL` calls the C function in-process; `PROCESS` spawns a
   **process actor** (bash, `rg`/`fd`/POSIX engines); `MCP` calls the MCP
   server (itself a long-lived process actor).
3. `tool`/`result` hook — transform/observe the result.
4. append the `tool` transcript message.

- **Internal tools go through the same hooks by default**, so audit/policy/redact
  is uniform. `AGENTC_TOOL_NO_HOOKS` opts a tool (or a whole run via config
  `tools.hooks = off`) out of the pipeline for performance/hermetic runs.
  `AGENTC_TOOL_NO_APPROVAL` opts out of only the approval branch.
- **MCP calls use the exact same stages** (`tool`/`call`, `tool`/`result`), the
  same envelope decision, and the same approval path. There is no separate MCP
  hook point.
- The old `jobs.c` batch driver becomes the **scheduler over runners**: it keeps
  batch ordering, concurrency, timeout and result ordering; it no longer owns
  pid/fd (a process runner is a process actor it polls).

## 5. Parent/child agents are tools too

`agent op:spawn` and `agent op:send` are tool calls, so they pass through
`tool`/`call` — which is where the **spawn decision** (envelope clamp, depth,
`ALLOW_SUBAGENT`, approval) lives. No separate spawn hook. Likewise `process`
ops (`write`/`resize`/`kill`/`freeze`/`resume`) and `queue` ops are tool calls
and flow through the same stages; their effect on actors is a message.

## 6. Handler kinds (one contract)

C function · program (JSON stdin/stdout, sandboxed) · MCP tool call · model
(provider call — jev/classifier) · branch-to-agent (`hook_branch`). Program/MCP/
model handlers are internally a `defer`.

## 7. Pipeline mechanics

- **Order:** per stage, ascending `order`, then registration.
- **Chain:** next handler sees `*next`; `NULL` ends (consume).
- **Async:** `*defer` parks; actor `WAITING`; `runtime_step` resumes.
- **Hot path:** `agentc_hook_wants(stage)` — no handlers ⇒ zero work.
- **Guards:** `depth` ≤ `AGENTC_HOOK_MAX_DEPTH` (8); per-item reentry set.
- **Errors:** `hooks.<stage>.on_error` — default `pass` for transform stages
  (`step`, `input`, `prompt`, `message`, `output`, `tool`, `signal`) and `deny`
  for gate sub-kinds (`tool`/`call`, `step`/`model_select`); observe-only stages
  ignore errors.
- **No blocking:** the pipeline runs only inside `runtime_step`/the loop.

## 8. Placement (the middle)

```text
user ─▶ [input] ─▶ [step:start] ─▶ [prompt] ─▶ provider ─▶ [output] per chunk
tool ─▶ [tool:call] ─ allow │ deny │ narrow │ branch ─▶ run(internal|process|mcp)
                                                      └─▶ [tool:result] ─▶ [message]
step ─▶ [step:end]   signal/event/lifecycle ─▶ [signal]/[event]/[lifecycle]
```

Every bracket is a stage; every arrow is a primitive (message send, actor spawn,
defer). Hooks never touch tokens/output/transcript except by returning a message.

## 9. What this absorbs

- extension hook bus (`on(point,…)`) → C-handler backend (point names alias
  stages/kinds)
- approval / `tool_approval` → `tool`/`call` handler
- `P6-DECIDERS` router/jev/decision points → handlers on `input`/`tool`/`step`
- bash, `rg`/`fd`/POSIX engines, MCP tools → the `tool` stage, one runner API
- model/compaction selection → `step` sub-kinds
- redaction/translation/audit → `output`/`message`/`tool` transforms

## 10. Surfaces (fold into `P6-SURFACE.md`)

| surface | item |
| --- | --- |
| C API | `agentc_hook`, `hook_off`, `hook_run`, `hook_resume`, `hook_wants`, `hook_branch` |
| stages | `turn`, `step`, `input`, `prompt`, `message`, `output`, `tool`, `signal`, `event`, `lifecycle` |
| runners | `AgcToolRunner{kind: internal\|process\|mcp}`, `AGENTC_TOOL_NO_HOOKS`, `AGENTC_TOOL_NO_APPROVAL` |
| host | `hook(stage, order, fn, ud)`, `hook_off(handle)` (supersedes `on(point,…)`) |
| events | `AGENTC_EV_HOOK_START`, `AGENTC_EV_HOOK_END` |
| config | `hooks.<stage> = [...]`, `hooks.on_error_default`, `tools.hooks = on\|off` |
| session | `hook {stage, kind, item_id, handler, before_hash, after_hash}` |
| errors | `hook_denied`, `hook_timeout`, `hook_depth`, `hook_reentry`, `hook_malformed`, `tool_hooks_disabled` |

## 11. Worked examples

1. **Redact output:** `output` transform edits each chunk; `final` flushes.
2. **Deny/narrow a tool call (any kind):** `tool`/`call` rules → edit/consume;
   uncertain → `hook_branch` to jev.
3. **Audit internal tools:** leave hooks on; a `tool`/`call` observer logs.
   Hermetic run: `tools.hooks=off` (or per-tool `NO_HOOKS`).
4. **Route input to a child:** `input` handler branches to the classifier; the
   resumed message carries the route; the core applies it.
5. **Per-step model switch:** `step`/`model_select` handler edits the model.
6. **MCP review:** an MCP tool call flows through `tool`/`call` → `tool`/`result`
   like any other; jev can deny it.
7. **Bounded two-way agents:** `depth`/`AGENTC_HOOK_MAX_DEPTH`.

## 12. Test plan

`tests/hooks_test.c` + `.expected`: every stage/kind; continue/edit/replace/
consume; defer+resume; `hook_branch`; internal-vs-process-vs-MCP runner
uniformity; `NO_HOOKS`/`tools.hooks=off` bypass; observe-only stages; reentry/
depth; `on_error` per stage; hot-path guard; audit records byte-exact; step
hooks fire once per boundary. Determinism: run twice, identical order/outputs.

## 13. Handoff

> Land after P6-B/P6-C. Implement `include/hook.h` + `src/core/hook.c` (two-verb
> contract, registry, chain, defer, `hook_branch`) and `src/core/tool_runner.c`
> (one runner API for internal/process/mcp). Route every tool through
> `tool`/`call`→run→`tool`/`result`, including MCP and the `agent`/`process`/
> `queue` ops; honor `NO_HOOKS`. Add the `step` stage at the loop boundary.
> Map `host->on(point,…)` point→stage/kind. Fold `P6-DECIDERS` points into
> handlers. No new primitive, no disposition vocabulary, no new tool. Append to
> headers, never edit. Tests per §12.
