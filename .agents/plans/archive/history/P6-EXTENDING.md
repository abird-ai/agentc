# P6-EXTENDING — Build an extension, tool, MCP server, governance, access, or hook

Status: **as-built (P6-A..E + completion passes 2a-2g).** The extension/tool/MCP/governance guide.
As-built note: the outcome is folded into `.agents/design/30-extensibility.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

## 0. Surface map (where everything lives)

```text
primitives   Runtime · Actor · Message · Envelope          (+ hook: next | defer)
services     hook pipeline · queue · deciders/router/jev · approval/policy
adapters     tools(3) · RPC · host ABI · CLI · config · events · session · MCP
```

The **one extension ABI** is `include/agentc_ext.h` (append-only). P6 adds to it;
it does not replace it. Rules that always apply:

- Append-only: new fields at the end of structs, new events after
  `AGENTC_EV_MSG_RESET`, new host functions at the end of `AgcExtHost`.
- Every struct starts with `struct_size`; gate appended fields with
  `AGENTC_EXT_FIELD_OK(x, field)` and host services with
  `AGENTC_EXT_HOST_HAS(host, field)`.
- `AgcExt.abi_version == AGENTC_EXT_ABI`; set `required_host_size` if you need
  newer host services.
- Host-allocated strings come from `host->alloc` and are freed with `host->free`.
- No stdout/stderr; use `host->out_write` / `host->log` / `host->notify`.
- Errors are negative linux errno; JSON crosses the ABI, typed structs stay
  internal.

Discovery: registered defaults (`agentc_ext_register_defaults`) + statically
linked extensions (`agentc_ext_register_linked`) + dynamic libraries on macOS/
Windows (`agentc_ext_register_dynamic`); `extensions.disabled` config excludes
by name. `agentc_ext_load_all` runs `init` in ascending `order`.

## 1. Extension author (the base)

What you write:

```c
int agentc_ext_init(const AgcExtHost *host, AgcExt *out);   /* the only symbol */
```

```c
static int my_init(const AgcExtHost *h) { /* register contributions */ return 0; }
static void my_shutdown(void) { /* free extension-owned state */ }

int agentc_ext_init(const AgcExtHost *host, AgcExt *out) {
    *out = (AgcExt){ .abi_version = AGENTC_EXT_ABI, .struct_size = sizeof *out,
                     .name = "my-ext", .version = "1.0.0", .order = 100,
                     .init = my_init, .shutdown = my_shutdown,
                     .required_host_size = 0 };
    return 0;
}
```

Register in `init` (all validated + copied; a bad contribution is dropped, not
fatal):

| register | to add |
| --- | --- |
| `host->add_tool` | tools (§2) |
| `host->add_command` | `/name` commands (`AgcExtCommand`) |
| `host->add_prompt` | a **system-prompt contribution** (`AgcExtPrompt` + `anchor` + `mode` APPEND/REPLACE). Anchor `PREAMBLE\|TOOLS\|RULES\|ENVIRONMENT\|ALL`;`REPLACE` overrides a core section (including the preamble). Not settings, not user input. |
| `host->add_status_line` | a status line in any region + lifetime (`AgcExtStatusLine`, `P6-STATUS.md`) |
| `host->add_provider` | a custom provider (`AgcExtProvider`) |
| `host->add_setting` | a typed **setting**, grouped by the extension's own `name` (no group object) — `P6-SETTINGS.md` |
| `host->on(point, caps, priority, fn, ud)` | a point hook (`AGENTC_HEV_*`) |
| `host->hook(stage, order, fn, ud)` | a pipeline hook (§6) |

Host services you may use: `alloc/free`, `strdup_`, `json_get_str/int/bool`,
`json_escape`, `log`, `out_write`, `append_entry` (session), `notify`,
`set_title`, `set_status`, `cwd/session_id/session_file/system_prompt`,
`defer` + `is_cancelled`, `http_request` + `http_cancel`,
`set_model`/`set_thinking`/`request_recompose`.

Read your own settings with `host->setting_get(ext,key)` (`P6-SETTINGS.md`).
`add_prompt` adds **system-prompt** text (not user input); `add_setting`
configures a value (grouped under the extension).

### 1b. Status lines — every region

Status is no longer one row. `host->add_status_line(&AgcExtStatusLine)` places a
line in any **region** with a **lifetime** (`P6-STATUS.md`):

- **Regions:** `ABOVE_COMPOSER`, `BELOW_COMPOSER`, `STATUS_LEFT`,
  `STATUS_RIGHT`, `BELOW_STATUS` (chrome); `STEP_END`, `TURN_END`, `FINAL_OUTPUT`
  (event).
- **Lifetimes:** `PERSISTENT` (chrome; polled version-driven), `PER_TURN`,
  `ONCE` (event regions; rendered at the boundary and appended as a notice).
- `render(host, ud, ctx, out)` writes **one or more lines** (multi-line);
  `ctx` carries `region`, `turn`, `step`, `stop_reason`, `final`, `text`. Empty
  output renders nothing.
- **Order:** `(region, priority, registration)`; bounds: max lines per region,
  global row cap, width clip. **Remove:** `agentc_status_remove(region,…)` or
  return empty. Built-ins use the same table (no privileged path).
- This replaces the old provider/segment pair (breaking; `add_status` →
  `add_status_line`).

Resources: contribute skill/prompt/theme roots with the
`resources_discover` point (return `resources` JSON); project roots load only
when the project is trusted. The built-in `context` extension already supplies
`AGENT.md`/`AGENTS.md` and skills.

Gotchas: `init` runs before trust is final for project resources (the app calls
`resources_discover` again after trust); never block in `init`; `shutdown` is
process-lifetime (not per session).

## 2. Tool author

Implement `AgcExtTool` (append-only) and register with `host->add_tool`.

```c
static int my_run(const AgcExtHost *h, const AgcExtTool *self,
                  const AgcExtToolCall *call, void *out, bool *is_error) {
    const char *p = h->json_get_str(call->args_json, "path", "");
    h->out_write(out, "result…", 8);
    return 0;
}
```

- **Sync:** set `run`. **Async:** leave `run` NULL and provide
  `start`/`step`/`stop`; poll `h->is_cancelled(call->signal_token)`; `stop` is
  called exactly once with `FINISHED|ERROR|TIMEOUT|CANCELLED|UNLOAD`. Do all I/O
  via `host->defer`/`host->http_request` — never block.
- **Flags:** `AGENTC_TOOL_READONLY`, `_DESTRUCTIVE`, `_SEQUENTIAL`, `_HIDDEN`,
  `_CORE`, and P6: `_NO_HOOKS` (skip the pipeline), `_NO_APPROVAL`.
- Schema: `parameters_json` (JSON Schema) + `prompt_snippet`/`guidelines`.
- **Execution kinds** (P6): an extension tool is `INTERNAL` (in-process). A
  `PROCESS`-backed tool is the built-in `process` actor path; an MCP tool is
  `MCP`. You rarely write those; you write `INTERNAL`.
- **Routing (P6):** every call goes `tool`/`call` → run → `tool`/`result`; the
  envelope and approval apply automatically. Opt out only when necessary.
- Output is capped (`AGENTC_LIMIT_TOOL_BYTES`/`_LINES`); return `-errno` only for
  real faults; validation failures should be tool results, not errors.

## 3. MCP author

MCP is a **built-in client**; a server author implements the protocol, not the
ABI. What you get for free (P6):

- `mcp.servers` config spawns each server as a **`process` actor** under the
  agent's envelope (stdio JSON-RPC). Add `AGENTC_PROC_PIPE_BIDI` when landing.
- Server tools appear as `mcp__<server>__<tool>`; resource tools as
  `mcp_list_resources` / `mcp_list_resource_templates` / `mcp_read_resource`.
- **MCP calls use the same pipeline**: `tool`/`call` → run → `tool`/`result`,
  the same envelope decision and the same approval branch. There is no MCP-only
  hook. A `tool`/`call` handler (e.g. jev) sees them like any tool.
- Tool names, schemas and argument errors are uniform; the client enforces
  line/schema caps.
- An MCP server may also be a **governance backend** (§4): register it as a
  decider/policy handler and it answers policy decisions.

Nothing MCP-specific to register in an extension unless you are wrapping a
non-standard transport (then contribute a `PROCESS` runner).

## 4. Governance author (policy, decider, router, controller/jev)

You build one or more of:

**a) Local rules (`policy.jsonc` / a `policy_rules` contribution).**
`agentc_policy_load` + `agentc_policy_add_json`; evaluated by
`agentc_policy_eval(rule, tool, args) → allow|deny|ask|delegate`. First-match
wins; fail-closed.

**b) A decider backend.** `decider_register(spec)` with `kind: local|program|
mcp|model`. Program: JSON on stdin, decision JSON on stdout, run in a sandboxed
`process`. Model (jev): a provider call with a reviewer system prompt. MCP: a
tool call. Selected per stage via config `hooks.<stage>` / `deciders.<id>`.

**c) A hook handler (the actual integration point).** Register on the data
stages — not a decision stage:

```c
static int jev_on_tool(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    if (!is_tool_call(m)) return 0, *next = m;        /* allow */
    return agentc_hook_branch(jev_spec, m, defer);    /* defer; resume with verdict */
}
```

On resume, apply the **decision record** (P6-DECIDERS §2b): return the
tool-call message (allow/narrow), `NULL` (deny), or a **replacement** call
(`agent op:spawn` to spin a subagent); and apply side effects with
`message_send`: CONTROL `SET_MODEL`/`SET_THINKING`/`SETTLE`, INPUT for
queue/steer, `CANCEL{scope:turn}`. Nothing else is interpreted by the core.

**d) Envelope/approval.** `agentc_trust_narrow(parent, req)` to clamp;
`agentc_trust_decide(env, call)` for the built-in gate; branch to a human for
interactive approval (`P6-TRUST` §7.2); `submit_approval(response)` to answer
out-of-band. Every verdict is audited (session `decision` record).

**Routing/classification** is the same: a handler on `input` that replaces the
message meta with a route (target/model/tools/trust/queue/budget). This *is*
Codex's guardian/auto-review, with no dedicated subsystem.

## 5. User-access author (trust, auth, session, approvals)

You work with the **envelope** and the auth layer, not a new mechanism.

- **Envelope:** `AgcTrustEnv { access(RO/RW), boundary(none/workspace/strict),
  approval(auto/ask/policy), network, roots, hosts, flags }`. Default
  `rw/workspace/auto` (trust the model inside the boundary). Resolve with
  `agentc_trust_resolve(cwd, project_trusted, cli_verdict, out)`; clamp children
  with `agentc_trust_narrow`. Flags include `ALLOW_PTY`, `ALLOW_BACKGROUND`,
  `ALLOW_SUBAGENT`, and `ALLOW_MIC`/`ALLOW_SPEAKER` (voice, R-D27).
- **Project trust:** `project_trust` hook + `~/.config/abird/agentc/trust.jsonc` +
  `--approve/--no-approve`; an untrusted project can only *tighten*. Config,
  skills, prompts, themes and MCP from a project load only when trusted.
- **Auth:** provider keys from `--api-key` → stored OAuth → env var → stored
  API key → config `api_keys`; OAuth (Claude/ChatGPT) via `agentc login`.
- **Access decisions:** CLI `--access/--sandbox/--approval/--network/--policy`;
  config `trust.*`; RPC `set_trust`/`set_policy`; host `get_trust`/`set_trust`.
- **Approvals:** scopes `once|step|session|project`; `trust.approval.persist`
  gates saving project-scope rules. Session `trust`/`approval` records are the
  audit trail.
- **Voice/audio access:** enabling voice is a command that flows through the
  envelope; mic/speaker denied by default.

You implement this by contributing config defaults, a `policy_rules`/`trust_select`
handler, or a front end that answers approvals — not by touching the core.

## 6. Pipeline extension (hook) author

The whole hook surface (R-D22/R-D23):

```c
typedef int (*AgcHookFn)(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer);
uint64_t agentc_hook(const char *stage, int order, AgcHookFn fn, void *ud);
void     agentc_hook_off(uint64_t handle);
int      agentc_hook_run(const char *stage, AgcMessage *m, AgcMessage **out);
int      agentc_hook_resume(uint64_t token, AgcMessage *m);
bool     agentc_hook_wants(const char *stage);
int      agentc_hook_branch(const AgcAgentSpec *spec, AgcMessage *m, uint64_t *defer);
```

- **Ten stages:** `turn, step, input, prompt, message, output, tool, signal, event,
  lifecycle` (sub-kinds: `step`=start/end/model_select/compact/steer;
  `tool`=call/result; `input`=user/child/queue; `output`=text/think/args/audio/
  item). Registration order = `order` ascending.
- **Two verbs:** return the next message (edit/replace; `NULL` = consume) or set
  `defer` (async; resume via `hook_resume`).
- **Media-agnostic:** `m->content_type` (`text/plain`, `application/json`,
  `audio/pcm;…`, `application/x-realtime-item`) is opaque to the core.
- **Combinators:** `emit`/`fork` = `message_send`; `branch` = `hook_branch`.
- **Config:** `hooks.<stage> = [ {order, program|decider|mcp, on_error} ]`;
  `on_error` = `pass` (transform stages) or `deny` (gate sub-kinds).
- **Guards:** `AGENTC_HOOK_MAX_DEPTH` (8), per-item reentry set; `hook_wants` is
  the hot-path guard (no handlers ⇒ zero cost).
- **Legacy:** `host->on(point, …)` still works; point names alias stages/kinds.

Minimal hook:

```c
static int redact(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    if (m->content_type && strcmp(m->content_type, "audio/pcm")==0) { *next = m; return 0; }
    /* replace the body with a redacted copy */
    *next = redacted_copy(m); return 0;
}
agentc_hook("output", 10, redact, NULL);
```

## 7. Cross-persona surface you touch

| persona | writes | calls |
| --- | --- | --- |
| extension | `agentc_ext_init`, `AgcExt`, `host->add_*` | host services, `on`/`hook` |
| tool | `AgcExtTool` (+ sync/async fns) | `json_get_*`, `out_write`, `is_cancelled`, `defer/http` |
| MCP | server protocol (external) | — (built-in client) |
| governance | `AgcExtHookFn` + policy/decider specs | `hook_branch`, `message_send`, `trust_decide/narrow`, `policy_eval`, `submit_approval` |
| access | config/`trust.*`, auth, `policy_rules`/`trust_select` | `trust_resolve/narrow`, `get_trust/set_trust`, RPC `set_*` |
| pipeline | `AgcHookFn` + `agentc_hook` | `hook_run/resume/wants/branch`, `message_send` |

## 8. Read next

- `P6-API.md` — the layered API in one page.
- `P6-PRIMITIVES.md` — the four primitives + two hook verbs.
- `P6-HOOKS.md` — stages, the two-verb contract, universal tool path.
- `P6-TRUST.md` — envelope, sandbox, approvals, delegation.
- `P6-DECIDERS.md` — router/jev/controller (§2b multi-effect).
- `P6-VOICE.md` — realtime voice as an extension + process helper.
- `P6-SURFACE.md` — the exhaustive registry.
- `include/agentc_ext.h` — the frozen ABI; `design/30-extensibility.md` — current
  host surface and hook-point semantics.

## 9. Handoff note

> This is the implementer index; it introduces no new surface. When you build an
> extension/tool/MCP/governance/access/hook, use only the surfaces listed here
> and in `P6-SURFACE.md`. Append to headers, never edit; gate appended fields;
> no blocking in host callbacks. If you need a capability that is not expressible
> as a message kind, a hook stage, an envelope decision, a tool runner, or a host
> service, stop and record why as a new decision — do not add a parallel
> mechanism.
