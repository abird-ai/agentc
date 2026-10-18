# P6-API — The API surface, layered

Status: **as-built (P6-A..E + completion passes 2a-2g).** The layered API surface.
As-built note: the outcome is folded into `.agents/design/30-extensibility.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

## Mental model

```text
adapters      tools (3) · RPC · host ABI · CLI · config · events · session
services      hook pipeline · queue · deciders/router/jev · approval/policy · settings
core          4 primitives + 2 hook verbs
              Runtime · Actor (agent|process) · Message · Envelope
              hook: return the next message | defer
```

Rule: **if it can be a service, it is.** New capability = a new **message kind**
or a new **hook stage**, never a new primitive, tool, RPC, or host method.

## Tier 1 — Primitive API (the only core)

```c
/* Runtime — scheduler/reactor + continuations */
int      agentc_runtime_init(const AgcTrustEnv *root);
int      agentc_runtime_step(int timeout_ms);           /* the only driver */
size_t   agentc_runtime_pollfds(struct os_pollfd *out, size_t max);
uint64_t agentc_runtime_defer(void (*resume)(void *, AgcMessage *), void *ud);
void     agentc_runtime_cancel_defer(uint64_t token);
void     agentc_runtime_shutdown(const char *reason);

/* Actor — one factory + uniform I/O surface */
AgcActorId agentc_actor_spawn(const AgcActorSpec *, char *err, size_t);
/* AgcActorSpec { const AgcProcSpawn *proc; const AgcAgentSpec *agent; } — one set */
int        agentc_actor_query(AgcActorId, AgcBuf *out_json);   /* state|info|queue|trust|result */
size_t     agentc_actor_read(AgcActorId, AgcBuf *out, size_t max);
int        agentc_actor_pollfds(AgcActorId, struct os_pollfd *out, size_t max);
void       agentc_actor_release(AgcActorId);
size_t     agentc_actor_list(AgcActor *out, size_t max);

/* Message — one communication verb (CONTROL + INPUT + NOTIFY + HOOK) */
int  agentc_message_send(const AgcMessage *);
int  agentc_message_to_json(const AgcMessage *, AgcBuf *);
int  agentc_message_from_json(const char *, size_t, AgcMessage *);

/* Envelope — trust/sandbox boundary */
int  agentc_trust_default(AgcTrustEnv *, const char *cwd);
int  agentc_trust_resolve(const char *cwd, bool project_trusted, int cli_verdict, AgcTrustEnv *out);
int  agentc_trust_narrow(const AgcTrustEnv *parent, const AgcTrustEnv *req, AgcTrustEnv *out,
                         char *err, size_t cap);
int  agentc_trust_decide(const AgcTrustEnv *, const AgcMessage *call, AgcTrustDecision *out);
int  agentc_trust_to_json/from_json/describe/free/clone(...);

/* Hook — the whole pipeline surface (two verbs) */
typedef int (*AgcHookFn)(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer);
uint64_t agentc_hook(const char *stage, int order, AgcHookFn fn, void *ud);
void     agentc_hook_off(uint64_t handle);
int      agentc_hook_run(const char *stage, AgcMessage *m, AgcMessage **out);
int      agentc_hook_resume(uint64_t token, AgcMessage *m);
bool     agentc_hook_wants(const char *stage);                 /* hot-path guard */
int      agentc_hook_branch(const AgcAgentSpec *spec, AgcMessage *m, uint64_t *defer);
```

## Tier 2 — Services (thin wrappers over Tier 1)

- **Actor control:** `agentc_process_write/read/tail/info/resize/kill/spill_path/set_stdin`;
  `agentc_actor_freeze/resume/frozen/checkpoint`; `agentc_actor_get/info/parent/exists/count/root/set_label`.
- **Queue (INPUT subset of an inbox):** `agentc_queue_add/list/update/delete/reorder/start/get/count/clear/pause/is_paused/run_of`.
- **Agent tree:** `agentc_agent_create/new/find/info/parent/list/wait/send/cancel/result/turn_no/steerable`; plus existing `submit/abort/load/set_*`.
- **Decision/approval/policy:** `agentc_decide(spec,m,&defer)`, `agentc_decision_submit`; `agentc_approval_request/submit/pending/cancel/scope_remember/is_pending`; `agentc_policy_load/add_json/free/eval`.
- **Trust helpers:** `agentc_trust_sandbox_spec/path_ok/host_ok/load/save/env_clone/env_free`.
- **Settings:** `agentc_settings_get_json/str/int/bool/float`, `set/reset/list`.
- **Prompt:** the `prompt` hook stage (transform) + `add_prompt` contributions (below).

## Tier 3 — Extension contributions (`init`) and host services

Register (all copied + validated; append-only ABI, gate newer ones with
`AGENTC_EXT_HOST_HAS`):

| call | contributes |
| --- | --- |
| `add_tool(&AgcExtTool)` | a tool |
| `add_command(&AgcExtCommand)` | a `/name` command |
| `add_prompt(&AgcExtPrompt)` | **system-prompt text** (`anchor` ALL/PREAMBLE/TOOLS/RULES/ENVIRONMENT × mode APPEND/REPLACE) |
| `add_status_line(&AgcExtStatusLine)` | a status line in any region (`ABOVE_COMPOSER/BELOW_COMPOSER/STATUS_LEFT/RIGHT/BELOW_STATUS/STEP_END/TURN_END`) with lifetime |
| `add_provider(&AgcExtProvider)` | a wire provider |
| `add_setting(&AgcExtSetting)` | a typed setting, grouped by the extension's `name` |
| `on(point, caps, pri, fn, ud)` | a legacy point hook |
| `hook(stage, order, fn, ud)` | a pipeline hook |

Host services: `alloc/free/strdup_`, `json_get_str/int/bool`, `json_escape`,
`log`, `out_write`, `append_entry`, `notify/set_title/set_status`,
`cwd/session_id/session_file/system_prompt`, `defer`+`is_cancelled`,
`http_request/http_cancel`, `set_model/set_thinking/request_recompose`,
`setting_get/setting_set`, `actor_spawn/actor_op/actor_query/actor_read/actor_await`,
`message_send/message_from_json/message_to_json`, `queue_op`, `get_trust/set_trust`,
`submit_approval`, `policy_eval`, `hook/hook_off`, `decider_register`.

## Tier 4 — Adapters

**Tools (3, op-based, one per actor kind + queue):**
`process {op: spawn|write|read|resize|kill|freeze|resume|list|attach|detach}` ·
`agent {op: spawn|wait|send|interrupt|freeze|resume|result|list}` ·
`queue {op: add|list|update|delete|reorder|start|pause|resume|clear}`.
`bash` is an alias for `process {op:spawn}`. Every tool (internal/external/MCP)
runs through the `tool` stage; internal may opt out (`AGENTC_TOOL_NO_HOOKS`).

**RPC:** `actor_op` · `actor_query` · `runtime {op}` · `queue {op}` · `approve` ·
`deny` · `set_trust` · `set_policy` · `get_settings`/`set_setting` · extended `get_state`.

**Hook stages (10):** `turn, step, input, prompt, message, output, tool, signal, event,
lifecycle` — sub-kinds: turn=start/end/steer · step=start/end/model_select/compact ·
tool=call/result · input=user/child/queue · message=user/assistant/tool/system ·
output=text/think/args/audio/item. Message `class`: `CONTROL | INPUT | NOTIFY | HOOK`;
CONTROL kinds include `CANCEL/FREEZE/RESUME/ATTACH/DETACH/WRITE/RESIZE/DECIDE/
DECIDED/SET_MODEL/SET_THINKING/SETTLE`.

**Events** (after `AGENTC_EV_MSG_RESET`): `TRUST_SET`, `APPROVAL_REQUEST/RESULT`,
`SANDBOX_DENIED`, `ACTOR_*` (started/progress/stalled/input/steered/queued/
notify/finish/fail/cancel_requested/cancelled/frozen/resumed/checkpointed/
attached/detached/resized), `AGENT_RESULT`, `QUEUE_PAUSED`,
`DECISION_REQUEST/RESULT`, `ROUTE_DECIDED`, `HOOK_START/END`, `SETTINGS_CHANGED`.

**CLI:** `--access --sandbox --approval --policy --network --max-depth
--no-background --dump-actors --shutdown-grace --set --list-settings`.

**Config:** `trust.*`, `policies.<id>`, `deciders.<id>`, `hooks.<stage>`,
`tools.hooks`, `route.*`, `settings.<extension>.<key>`.

**Session (`custom`, v1):** `agent`, `queue`, `approval`, `actor`, `trust`,
`decision`, `hook`.

## Which do I call?

| I want to… | call |
| --- | --- |
| drive the runtime | `agentc_runtime_step` |
| start an agent / process | `agentc_actor_spawn` (or `agent`/`process` tools) |
| read a process/agent | `agentc_actor_read` / `agentc_actor_query` |
| control an actor | `agentc_message_send` a CONTROL message |
| queue / steer an agent | `agentc_queue_add` (INPUT message) |
| intercept any flow | `agentc_hook(stage, …)` |
| attach another agent to decide | `agentc_hook_branch(spec, m, &defer)` |
| decide allow/deny/route | hook handler on `tool`/`input` |
| bound what an actor may do | `AgcTrustEnv` + `agentc_trust_decide` |
| add prompt text / override the preamble | `add_prompt({anchor,mode})` |
| add a user setting | `add_setting(…)` |
| add a status line (chrome or per-step) | `add_status_line({region,lifetime})` |
| run in an external event loop | `agentc_runtime_pollfds` + `step(0)` |

## Naming rules

1. `agentc_<primitive>_<verb>` in the primitive layer; services reuse it.
2. Types `Agc<Primitive>`; payloads `Agc<Thing>Event`.
3. Negative linux errno; JSON at the ABI boundary, typed structs internally.
4. Append-only: new fields at the end, events after `MSG_RESET`, host functions at
   the end of `AgcExtHost` gated by `AGENTC_EXT_HOST_HAS`.
