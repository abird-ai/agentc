# Proposal — one extension model for tools, MCP, skills and hooks

Status: **exploration, superseded.** Not the plan. The plan is
`EXTENSION-MINIMAL.md`, which keeps the tool-source seam and the off-startup-path
MCP connect and drops everything below (ABI break, typed hooks, naming, sections,
durable, resources/prompts). Kept for context and as a source of ideas if a bigger
surface is ever wanted.

Goal: built-in tools, MCP, native plugins, **skills/context resources** and
**lifecycle hooks** become implementations of one **extension** model. A single
engine composes them. MCP stops being a special case wired into `src/app/main.c` and
becomes a default extension that ships enabled.

---

## 1. Today's shape (why this is worth doing)

Every contribution kind is hand-merged in `main.c`:

```c
/* tools — src/app/main.c:552 */
size_t nbuiltin = agentc_tools_builtin(NULL, 0);
size_t nplugin  = agentc_plugin_tools(NULL, 0);
size_t nmcp    = no_mcp ? 0 : (agentc_mcp_load(cwd, trusted) ? agentc_mcp_tools(NULL, 0) : 0);
/* …merge three arrays… */

/* context — src/app/main.c:634 */
size_t nskills = agentc_skills_load(cwd, trusted, &skills, 256);
agentc_prompt_add_skills(&sys, skills, nskills);
/* plus context files, templates, addendum, … */
```

Symptoms:

- **Every new source edits the app.** A provider means touching `main.c`, the flag
  parser and shutdown, not just writing a module.
- **MCP has a private lifecycle.** `agentc_mcp_load/_shutdown/_tools` are called
  directly; it never subscribes to the event bus, so the reserved
  `mcp_servers_change` never fires. The doc already claims "the built-in `mcp`
  extension is just a consumer, so a user plugin can replace it"
  (`docs/30-extensibility.md` §1.1) — there is no such seam in the code.
- **Startup blocks.** `agentc_mcp_load` connects every server synchronously, up to
  10 s each, before the first prompt.
- **Skills are a closed path.** Filesystem-only discovery, one hardcoded prompt
  section. `resources_discover` (already reserved in `include/agentc_plugin.h`) is
  never emitted, so an extension cannot contribute a skill or a context fragment.
- **MCP prompts are dead.** `mcp_initialize_params` sends `capabilities: {}`; the
  client never calls `prompts/list` or `prompts/get`. MCP's server-provided prompts —
  which are structurally skills — are invisible.
- **Hooks are observe-and-veto only.** See §4.
- **Two tool structs and a growing compat burden.** `AgcTool` and `AgcToolV1` mirror
  each other, translated by `map_tool_flags`; the host supports `ABI_MIN 1..2`, uses
  `struct_size` + `offsetof` gates and `reserved[8]`.

Only the *executor* is already uniform (`jobs.c`, `find_tool`, `AgcTool.run/start/
step`). The missing half is uniform **registration, lifecycle and composition**.

---

## 2. The model: contributions, not tools

Treat a tool as one kind of **contribution** to the agent. The other kinds are
commands, prompt/context fragments, resources (skill roots), status segments and
hooks/events. The status line already proves the pattern and the doc states the rule
explicitly:

> "The built-in … segments register through the same table a plugin uses … so there
> is no privileged path for either side." — `docs/30-extensibility.md` §3.5

Apply that to everything:

```
                config.jsonc / mcp.jsonc / skills / plugins
                                  │
                 ┌────────────────▼─────────────────┐
                 │  contribution registries          │  src/ext/registry.c
                 │  tools · prompt · resources ·     │
                 │  commands · status · hooks        │
                 └──┬───────────┬───────────┬────────┘
                    │           │           │
             builtin-tools     mcp     native plugins + user extensions
                    │           │           │
                    └───────────┴───────────┘
                                  │  compose
                         agent loop · prompt · TUI
```

### 2.1 `AgcExtension` (one interface)

```c
typedef struct AgcExtension {
    const char *name;        /* unique: "builtin-tools", "mcp", plugin name */
    const char *version;
    u32 flags;               /* EXT_DEFAULT (on unless disabled) */

    int  (*load)(struct AgcExtension *self, const AgcExtHost *host);
    void (*unload)(struct AgcExtension *self);        /* idempotent */

    size_t (*tools)(struct AgcExtension *self, AgcTool *out, size_t max);
    size_t (*prompt)(struct AgcExtension *self, AgcBuf *sys);          /* §5 */
    size_t (*commands)(struct AgcExtension *self, AgcExtCommand *out, size_t max);
    size_t (*resources)(struct AgcExtension *self, AgcSkillRoot *out, size_t max);

    void *ud;
} AgcExtension;
```

Only the callbacks that are actually implemented need to be non-NULL. Count-first
(`out == NULL`) is the convention, as everywhere else. Hooks are registered in
`load` via the host (§4).

### 2.2 `AgcExtHost` (internal) and trust levels

One **shape**, two **trust levels**:

- **Internal extensions** (builtins, MCP) are in-tree code. They implement
  `AgcExtension` for the contribution side, and are allowed to include internal
  headers (`src/…`, `plat.h`, `wire.h`) for anything the vtable does not cover —
  MCP's `os_spawn`/pipe/`os_poll` stdio transport, config access, session internals.
- **Native plugins** are external. They get the frozen `AgcHostV1` vtable only, and
  are adapted to `AgcExtension` by `src/ext/plugin.c`.

`AgcExtHost` is the internal host surface: `alloc/free/log`, `on_event`/`on_hook`/
`emit`/`emit_veto`, `cwd/session_id/session_file/system_prompt`, config/trust
accessors, `notify/set_status/register_command`, `http_request/http_cancel`, and
`request_recompose()` ("my tool/prompt/hook list changed; re-read me at the next safe
point").

### 2.3 Composition engine (`src/ext/registry.c`)

```c
void   agentc_ext_init(const AgcExtHost *host);
void   agentc_ext_register(const AgcExtension *ext);
void   agentc_ext_register_defaults(void);          /* builtin-tools, mcp */
void   agentc_ext_apply_config(const AgcConfig *c);
void   agentc_ext_load_all(void);
void   agentc_ext_pump(void);

size_t agentc_ext_tools(AgcTool *out, size_t max);
size_t agentc_ext_prompt(AgcBuf *sys);              /* §5 */
size_t agentc_ext_commands(AgcExtCommand *out, size_t max);
bool   agentc_ext_dirty(void);
```

`main.c` becomes:

```c
agentc_ext_init(&host);
agentc_ext_register_defaults();
agentc_plugin_register_all();
agentc_ext_apply_config(cfg);
agentc_ext_prompt(&sys);            /* context files + skills + extension fragments */
AgcTool *tools = compose(agentc_ext_tools, filters);   /* --tools / default_tools */
agentc_agent_set_tools(agent, tools, ntools);
```

### 2.4 Tool-set lifecycle

1. Startup: load extensions → compose → install.
2. Any extension sets the dirty flag (MCP connect, `tools/list_changed`, a plugin's
   late `register_tool`, a skill provider).
3. The front end calls `engine_recompose_if_dirty()` from the poll loop; it **refuses
   while a turn is running** and applies at `turn_end`.
4. Recompose re-applies filters, re-installs `agentc_agent_set_tools`, rebuilds the
   prompt, and emits `mcp_servers_change`.

Composition order is deterministic (extensions in registration order, tools sorted by
name) so non-deterministic connect/network completion cannot destabilise golden and
replay tests.

---

## 3. ABI: break it once, now

**Verdict: yes, break it — but only as part of this unification, and only once.**

The current dual-struct + version-range machinery exists to support a plugin
ecosystem that does not yet exist (`plugins/hello`, `plugins/rust_echo`). If we keep
`AgcHostV1` *and* add `AgcExtHost`, we end up with **four** struct families (two tool
descriptors, two host vtables) and a third translation layer. Resetting to one
baseline is strictly less code.

### What the break deletes

| Deleted | Replaced by |
|---|---|
| `AgcToolV1` + `map_tool_flags` | the single `AgcTool` (`run`/`start`/`step`/`ud`/`timeout_ms`) |
| `AGENTC_ABI_VERSION_MIN` + version-range acceptance | one `AGENTC_ABI_VERSION`; a mismatched struct is refused with a clear log |
| `offsetof`-gated field access (the `status_register` dance) | direct field access against the frozen struct |
| `reserved[8]` / `reserved[4]` | none |
| `tool_done(call, result_json)` + JSON parse/escape | `run` fills an `AgcBuf *out`, exactly like builtins |
| JSON `on_event` as the *hook* contract | typed hooks (§4); the string bus stays for observation |
| `json_get_str/int/bool`, `json_escape` as host contract | kept only as *conveniences*, not load-bearing |

### What the break keeps

`abi_version` and `struct_size` on every struct, and the append-only rule **from the
new baseline**. The mechanism is cheap and is what lets v1.0 grow; what we drop is the
burden of carrying v1/v2 shapes. Keep the exported symbol name
`agentc_plugin_init`.

### Where JSON stays

Breaking the ABI does **not** mean JSON everywhere-or-nowhere. The existing rule
still holds — JSON for boundaries whose shape is defined outside us:

- tool `parameters_json` (JSON Schema, sent to the model),
- model-defined tool arguments and result content,
- provider request/response bodies,
- session entries, MCP wire traffic, RPC mode, and the observation-only event bus.

Everything that is *our* contract — hook calls, tool results, status — becomes typed.

### The honest gap: stdio MCP replacing

To let a **plugin** replace `mcp` (the doc's claim), the host ABI also needs
process spawning and fd/poll access, or MCP-as-plugin is HTTP-only. Two options:

1. add `spawn`/`watch_fd`/`pipe` to the host vtable (bigger, riskier surface), or
2. state the limit: a plugin can replace the **HTTP** transport; stdio MCP is
   internal-only until there is demand for the spawn API.

Recommendation: **(2) now**, and note it in the doc. Internal extensions keep full
privilege; the public ABI stays small.

### Rust crate

`crates/agentc-plugin` is regenerated against the new structs. Mechanical: it already
uses `#[repr(C)]` wrappers; the JSON helpers it exposes become typed callbacks.

---

## 4. Hooks

Hooks are the event bus grown up: the existing `on_event`/`notify` path is the right
idea with the wrong capabilities.

### 4.1 What exists today

Emitted and **observe-only** (`notify` → `agentc_plugin_emit`, returns `void`, payload
JSON): `agent_start`, `turn_start`, `message_start`, `message_end`, `turn_end`,
`agent_end`, `error`, `compact`, `message_update`, `tool_execution_start/end`,
`tool_result`, `before_provider_request`, `after_provider_response`,
`provider_stream_event`, `session_start`, `session_shutdown`, `project_trust`.

Vetoable: `tool_call` only (boolean, via `agentc_agent_set_tool_veto`).

Reserved but **never emitted**: `input`, `before_agent_start`, `model_select`,
`thinking_level_change`, `resources_discover`, `mcp_servers_change`.

Documented but **not implemented**: `finishTurn` continuation; "replace
content/is_error" for `tool_result`; mutating the request in
`before_provider_request` (the payload carries the whole body, but read-only).

So: **observe and veto exist; transform, inject and continue do not.**

### 4.2 Capability taxonomy

| Capability | Meaning | Return |
|---|---|---|
| observe | side effects only (status, logs, telemetry) | ignored |
| veto | block the operation | 0/1 |
| transform | receive a mutable payload, may replace it | new payload or "unchanged" |
| inject | add content to a transcript/prompt | entry or none |
| continue | request another turn after settle | 0/1 |

A hook point declares which it supports; a registration declares which it wants; the
engine rejects (with a log) any hook that exceeds its declaration.

### 4.3 Hook points, including the prompt brackets

```
session_start ............... observe + inject    resources, skills, status
agent_start_before ......... transform           system prompt / context
  input ..................... transform + inject  ← BEFORE the prompt
  provider_headers_before ... transform          headers
  provider_request_before ... transform          body
  provider_stream_update .... observe
  provider_response_after ... transform
  tool_call ................. veto + transform   args
  tool_result ............... transform           content / is_error
  turn_end .................. observe + inject + continue   ← AFTER the prompt
agent_settle_before ......... observe + inject + continue   final boundary
agent_settled ............... observe
agent_end ................... observe
session_end ................. observe
```

- **`input`** — before the user prompt enters the transcript: rewrite the text,
  attach context/images, add a status segment. Not emitted today.
- **`turn_end`** — after the model answers: inject a follow-up, set status, request
  one continuation. Emitted today but observe-only.
- **`agent_settle_before` / `agent_settled`** — the final boundary after the last
  turn (§4.9). `agent_settle_before` is where the doc's `finishTurn` continuation
  belongs; `agent_settled` is the “run is over, nothing more will run” notification.
- **`provider_request_before` / `provider_response_after`** — per model call inside a
  turn; emitted today as `before_provider_request`/`after_provider_response`,
  read-only; become transform. See §4.8 for the rename.

Names above are canonical (§4.8).

### 4.4 Status before and after a prompt

Two channels, both already reachable by any extension:

- **Polled segments** (`status_register`). The unified table; lifecycle state
  (`running`/`waiting`/`idle`) is provider input, so a provider renders "before/after
  prompt" states with no hook at all.
- **Push** (`set_status(key,text)` / `notify`). Callable from any hook handler: a
  `turn_start` hook adds "before prompt" status, a `turn_end` hook adds "after
  prompt", visible immediately.

The gaps are only the *points* and the *shape*: `turn_start`/`turn_end` fire today but
`input` does not, and push status is keyed and blunt (no slot/priority/style). Fold
`set_status` into the segment model as an auto-clearing low-priority segment so push
and poll are one rendering path.

### 4.5 Async hooks

Observe/veto/transform must be synchronous and budgeted like status providers (a
blocking hook is disabled with a warning). I/O-bound work (MCP `prompts/get`, an LLM
guardrail) is **not** a transform:

1. A hook may `host->defer` and finish later **only** at points where the engine can
   pause — prompt build (`agent_start_before`, `input`) and resource discovery — never
   mid-stream.
2. Late results that change contributions go through the same dirty-flag /
   turn-boundary recompose path as tools and skills (§2.4).

### 4.6 Registration and ordering

```c
typedef struct {
    const char *point;                 /* "input", "tool_call", … */
    u32 caps;                          /* OBSERVE|VETO|TRANSFORM|INJECT|CONTINUE */
    int32_t priority;                  /* lower first; ties keep registration order */
    AgcHookFn fn;
    void *ud;
} AgcHook;

void host->on_hook(const AgcHook *reg);
```

Deterministic order; a transform chain feeds each hook the previous result; the
declared capability is validated. Errors are logged and skipped — a hook blocks only
through an explicit veto. This mirrors the status table's discipline (priority, one
shared table, bad input dropped, budget enforced).

### 4.7 Keep the string event bus

Observation stays on the cheap string bus (`on_event`) for logging/telemetry; hooks
are the typed, capability-checked interception path. One registry, two views —
consistent with "JSON at boundaries, typed internally".

### 4.8 Naming: provenance and one convention

Checked against pi's shipped `dist/core/extensions/types.d.ts`, which holds the
authoritative `ExtensionEvent` union (broader than the prose docs). pi defines:
`project_trust`, `resources_discover`, `mcp_servers_change`,
`session_start/shutdown/before_compact/compact/compact_failed/before_fork/before_switch/
before_tree/tree/info_changed`, `context`, `context_with_system`,
`cache_warming_decision`, `before_provider_request`, `before_provider_headers`,
`after_provider_response`, `provider_stream_event`, `before_agent_start`,
`agent_start`, `agent_end`, `agent_before_settle`, `agent_settled`,
`ui_prompt_start/end`, `turn_start/end`, `message_start/update/end`,
`tool_execution_start/update/end`, `model_select`, `thinking_level_select`,
`user_bash`, `input`, `tool_call`, `tool_result`.

So agentc's names **are** pi's names, largely verbatim — including
`before_provider_request` and `after_provider_response` (an earlier draft of this
doc wrongly called those agentc additions). The only drift is
`thinking_level_change` (pi: `thinking_level_select`); agentc adds `compact` and
`error`.

pi's convention is mixed, but the **only** violations of "modifier after subject"
are `before`/`after` placed in front or infix: `before_agent_start`,
`before_provider_request`, `before_provider_headers`, `after_provider_response`,
`agent_before_settle`, `session_before_*`. Everything else already reads
`<subject>_<phase>` (`agent_start`, `turn_end`, `message_update`, `tool_call`,
`tool_result`, `model_select`, …).

**Rule: `<subject>_<phase>`; modifiers are always suffixes. `before` and `after`
never prefix or infix.**

- interception windows (mutable/veto/transform, §4.2): `_before`, `_after`;
- notifications: `_start`, `_update`, `_end`, `_settled`;
- domain phases: `_select`, `_change`, `_discover`, `_compact`.

Because we are resetting the plugin ABI anyway, take the break and stop carrying
pi's inconsistency. Rename table — everything not listed already complies:

| pi / today | canonical |
|---|---|
| `before_agent_start` | `agent_start_before` |
| `before_provider_request` | `provider_request_before` |
| `before_provider_headers` | `provider_headers_before` |
| `after_provider_response` | `provider_response_after` |
| `agent_before_settle` | `agent_settle_before` |
| `session_before_compact` | `session_compaction_before` |
| `session_before_fork` | `session_fork_before` |
| `session_before_switch` | `session_switch_before` |
| `session_before_tree` | `session_tree_before` |
| `session_shutdown` | `session_end` (phase symmetry with `session_start`) |
| `provider_stream_event` | `provider_stream_update` (`event` is not a phase) |
| `thinking_level_change` (agentc) | `thinking_level_select` |
| `compact` (agentc) | `compaction_end` (+ `compaction_start`, `compaction_failed`) |
| `error` | `agent_error` (subject-only names are reserved for inputs: `input`, `context`) |

The **internal enum is a third vocabulary** — `AGENTC_EV_MSG_START`,
`AGENTC_EV_TOOL_EXEC_START`, `AGENTC_EV_TEXT_DELTA` — which is why `plugin.c` maps
names by hand. Derive it from the canonical table (`AGENTC_EV_<SUBJECT>_<PHASE>`)
so the C name, the string boundary and the docs are one vocabulary.

### 4.9 Adopting pi's missing events

With names canonicalized, adopt the ones whose state agentc already has or whose
capability the hook pipeline needs; skip the rest.

**Adopt now:**

| pi event | canonical | Why |
|---|---|---|
| `agent_before_settle` | `agent_settle_before` | agentc's loop already has a `SETTLE` state; this is the home for the doc's `finishTurn` continuation and for injecting a follow-up after the last turn |
| `agent_settled` | `agent_settled` | the run is fully done — status reset, telemetry, no continuation accepted |
| `before_provider_headers` | `provider_headers_before` | modify auth/routing/telemetry headers; orthogonal to the body transform |
| `session_before_compact`, `session_compact`, `session_compact_failed` | `session_compaction_before/end/failed` | agentc has compaction; lets an extension observe or veto it |
| `thinking_level_select` | `thinking_level_select` | agentc reserved it under the wrong phase (`_change`) |
| `model_select` | `model_select` | already reserved |
| `resources_discover` | `resources_discover` | skills and resources (§5.2) |

**Defer or skip:**

| pi event | Disposition |
|---|---|
| `context`, `context_with_system` | fold into `provider_request_before` (typed body transform). pi splits messages-only from whole-transcript; revisit only if that distinction is needed |
| `cache_warming_decision` | agentc has no prompt-cache warming (`cache_retention` is separate); adopt if warming lands |
| `user_bash` | agentc's shell is the `bash` tool; there is no user-initiated shell outside the model. Revisit with a `!cmd` escape |
| `ui_prompt_start` / `ui_prompt_end` | arrive with the reserved RPC `ui_request/response` dialog mechanism, not before |
| `session_before_fork` | adopt as `session_fork_before` when `/fork` becomes a hookable boundary (low cost) |
| `session_before_switch`, `session_before_tree`, `session_tree`, `session_info_changed` | defer until `/tree` and session switching land |

Net: **yes to the settle pair** (agentc has the state; it is where continuation
belongs), plus the header hook, compaction trio and the already-reserved
select/discover events; skip the ones whose subsystem agentc does not have.

---

## 5. Skills

### 5.1 What they are today

`SKILL.md` with `name`/`description` frontmatter, discovered under
`<config>/skills/**` and (trusted) `<cwd>/.agentc/skills/**`, emitted as an
`<available_skills>` block; the model `read`s the file on demand. Prompt templates
(`prompts/*.md`) are the same shape, invoked as `/<name>` with `$1..$n` expansion.

A skill is therefore: **discoverable metadata + a lazily loaded text body**. That is
a context contribution with an on-demand body.

### 5.2 Skills through the extension model

Make resources and prompt fragments contributions, exactly parallel to tools and
status:

- **Prompt/context registry.** `agentc_ext_prompt(&sys)` composes sections from
  built-in providers (context files, `APPEND_SYSTEM.md`, the static preamble) and from
  extensions. A `skill` provider is just one contributor; there is no hardcoded
  `agentc_prompt_add_skills` call in `main.c`.
- **Resource registry.** `resources()` returns skill roots. Built-in `skills` and
  `templates` providers cover the filesystem paths; an extension can add its own
  root or register a non-filesystem skill.
- **`resources_discover` fires.** Emitted at session start (the reserved name becomes
  real), letting an extension contribute skill roots and prompt fragments before the
  prompt is built.

### 5.3 The payoff: MCP prompts become skills

MCP defines `prompts/list` + `prompts/get` for server-provided prompt templates.
Advertise the capability, and expose each server prompt as a skill whose "body" is
fetched via `prompts/get` instead of a `read`. Then:

- the file system, MCP servers, and extensions all produce the same skill entries in
  the same `<available_skills>` block;
- the on-demand model is preserved (metadata up front, body only when used);
- MCP's `notifications/prompts/list_changed` and the tool `list_changed` share one
  recompose path (a late prompt appears at the next turn boundary, same as a late
  tool).

This is the strongest argument for the unification: it turns a dead protocol feature
into free functionality with no new UI.

### 5.4 Skill = declarative by default, extension when it needs code

Keep the "no code execution in the declarative layer" principle
(`docs/30-extensibility.md` §2). A skill is data. **Promotion is explicit:**

- plain skill → metadata + body;
- skill frontmatter may gain `allowed-tools` (a policy contribution consumed by the
  `tool_call` veto path) and later `command`;
- a skill that ships scripts/tools is materialised as an `AgcExtension` that registers
  those tools (or a provider extension registers it).

The escalation ladder is: *declarative skill → skill with a script/tool → extension*.
No level silently executes code.

### 5.5 Invocation

`/skill:<name>` and extension `register_command` go through one command registry. A
skill is a command whose expansion injects the body; a template is a command whose
expansion interpolates arguments. Today those are three separate mechanisms
(`/skill:`, templates, `register_command`); the model makes them one.

### 5.6 Trust

Project skills already load only when trusted (`agentc_skills_load(cwd, trusted, …)`)
because they feed the system prompt. Extension- and MCP-contributed skills need the
same gate: a contributed resource carries its trust provenance, and the prompt
composition skips untrusted project-scoped contributions. `project_trust` /
`resources_discover` are the hooks.

---

## 6. Config

```jsonc
"extensions": {
  "disabled": ["mcp"],                       // default extensions to turn off
  "mcp": { "connect_timeout_ms": 10000, "prompts": true },
  "builtin": { "enabled": true }
}
```

`mcp.jsonc` keeps server definitions. `extensions.paths` is the hook for dynamic
loading (v2, macOS/Windows only) and stays out of scope here.

---

## 7. Phases

- **P1 — registries, no behavior change.** Introduce `AgcExtension`/`AgcExtHost` and
  move the tool *and* prompt/context merges out of `main.c` into the engine. Builtins,
  MCP, plugins and the skill/template providers all become contributions. Still
  synchronous. Gate: full suite green.
- **P2 — hooks.** Typed hook pipeline with capabilities and priority; canonicalize
the names (§4.8) and derive the internal enum from them; emit `input`,
`agent_start_before`, `agent_settle_before`, `agent_settled`; add
`provider_headers_before` and the `session_compaction_*` trio; upgrade `tool_call`,
`tool_result`, `provider_request_before`/`provider_response_after` to transform;
implement `turn_end`/`agent_settle_before` continuation. Fold push status into the
segment model. Gate: a plugin rewrites user input, transforms a tool result and
injects a follow-up at settle; the golden suite is unchanged without such a plugin.
- **P3 — the ABI reset.** Collapse `AgcToolV1`/`AgcHostV1` into the unified typed
  interface, drop v1/v2 compat, regenerate the Rust crate and the two example plugins,
  re-record plugin tests. Land while the registry is fresh, before any real
  third-party plugin exists. Gate: `tests/plugin.sh` green on a rebuilt plugin.
- **P4 — async MCP + recompose + resources_discover.** Background connect; late
  tool/prompt registration at turn boundaries; emit `mcp_servers_change` and
  `resources_discover`. Gate: an unreachable server no longer delays the first prompt;
  its tools appear mid-session.
- **P5 — MCP prompts as skills.** Advertise the capability, map `prompts/*` onto the
  skill table, share the `list_changed` recompose path.
- **P6 — optional ABI additions.** Expose spawn/watch or `register_extension` to
  plugins only if demand appears.

---

## 8. Risks

| Risk | Mitigation |
|---|---|
| The break strands plugin authors | Do it now, pre-1.0, with two example plugins; bump to a documented v1.0 baseline and commit to append-only after |
| Four struct families if we unify and keep compat | The break is the point: unify *and* reset, don't layer |
| Recompose races an in-flight tool batch | Idle-only application, queued to `turn_end` |
| A transform hook corrupts state or diverges replay | Capability validation, deterministic order, budget/disable, golden tests without hooks |
| Non-deterministic MCP order breaks golden/replay | Sort composed tools/skills by name |
| Skills silently execute code | Declarative stays data; promotion to an extension is explicit |
| Untrusted project/mcp skills leak into the prompt | Carry trust provenance on every contribution; reuse the `skills_load(cwd, trusted)` gate |
| Scope creep (marketplace, dynamic loading, subagents) | Out of scope; v2 demand-gated |

---

## 9. Acceptance criteria

- `main.c` contains no `agentc_mcp_*`, `agentc_plugin_tools`, `agentc_skills_load` or
  `agentc_prompt_add_skills` calls; it calls the engine.
- One `AgcTool` struct, one host vtable, one hook registry, one `ABI_VERSION` with no
  version range, and **one naming rule**: every event/hook is `<subject>_<phase>`
  with `before`/`after` as suffixes (§4.8), reflected by the C enum, the string
  boundary and the docs.
- A new tool, prompt, skill source or hook is one `AgcExtension` vtable plus a
  registration line — no changes to `agent.c` or `jobs.c`.
- `input`, `agent_start_before`, `turn_end`, `agent_settle_before` (continuation)
  and transformable `tool_call`/`tool_result`/`provider_request_before` all work; a
  hook can set status both before and after a prompt.
- `provider_headers_before` and `session_compaction_before/end/failed` are emitted;
  `agent_settled` fires exactly once per run.
- `mcp` can be disabled or (HTTP) replaced purely by config/plugin.
- MCP startup never blocks the first prompt; late tools and prompts are picked up at
  the next turn boundary.
- MCP server prompts appear in `<available_skills>` and load via `prompts/get`.
- `make check`, `tests/e2e.sh`, `tests/mcp.sh`, `tests/plugin.sh` green.
