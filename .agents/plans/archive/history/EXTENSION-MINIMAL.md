# Minimal extension API — design and plan

Status: **superseded by `EXTENSION-SPEC.md`** (the authoritative,
implementation-ready spec). Kept as the one-page summary. `EXTENSION-TOOL-ENGINE.md`
is exploratory; the two `PI-*` files are reference.

Thesis: **minimal features, complete seams.** The core ships few capabilities, but
every extension point pi has is present, typed where its shape is ours and JSON
where the shape is external. A later extension can build the rest of the pi model
(skills, MCP resources/prompts, permission gates, transforms, continuation, custom
providers) without touching the core.

---

## 1. Principles

1. **One extension model.** Built-in tools, MCP, native plugins and future
   extensions all register through the same host. No privileged path.
2. **Core is itself extensions.** `builtin-tools` and `mcp` are default
   extensions; the app just loads them.
3. **Structure is typed, data is JSON.** Contributions you *register* are C
   structs; the values that *flow* through hooks are JSON (the house rule:
   "JSON at external boundaries, typed structs internally").
4. **Break the old ABI.** One header, one `abi_version`, no `ABI_MIN`, no v1/v2
   compatibility, no `reserved[]`.
5. **pi-compatible.** Contribution kinds and hook-point names are pi's, so a pi
   extension's mental model transfers and the docs line up.
6. **Shared bus.** The core and extensions emit on the same hook bus, so an
   extension that adds a feature can publish the standard pi point for others.

---

## 2. The API (`include/agentc_ext.h`)

Replaces `include/agentc_plugin.h`. The shape:

```c
#define AGENTC_EXT_ABI 1

typedef struct AgcExtHost AgcExtHost;

/* ---- contributions: typed ---- */
typedef struct AgcTool {                       /* existing run/start/step/ud/tmo */
    uint32_t struct_size, flags;
    const char *name, *label, *description, *parameters_json;
    const char *prompt_snippet, *prompt_guidelines;
    void *ud; int timeout_ms;
    int  (*run)(const AgcTool*, const AgcToolCall*, AgcBuf*, bool*);
    int  (*start)(const AgcTool*, const AgcToolCall*, AgcJob*);
    int  (*step)(const AgcTool*, AgcJob*);
} AgcTool;

typedef struct {
    const char *name, *description;
    void (*run)(const AgcExtHost*, const char *args_json, AgcBuf *out);
} AgcCommand;

typedef struct {                               /* pi `section()` */
    const char *key;
    int priority;                              /* lower renders first */
    int (*render)(const AgcExtHost*, AgcBuf *out, void *ud);
    void *ud;
} AgcSection;

typedef struct AgcProvider AgcProvider;         /* existing provider table */

/* ---- hooks: uniform, JSON payload ---- */
#define AGENTC_HOOK_OBSERVE  0x1u   /* side effects only; return ignored */
#define AGENTC_HOOK_OVERRIDE 0x2u   /* may return a result that replaces the default */
/* return 0 = continue; 1 = handled (stop); *out_json = pi-shaped result object */
typedef int (*AgcHookFn)(void *ud, const char *payload_json, char **out_json);

/* ---- host ---- */
struct AgcExtHost {
    uint32_t abi_version, struct_size;

    void  *(*alloc)(size_t);                   /* zeroed */
    void   (*free)(void*);
    void   (*log)(int level, const char*);

    /* register contributions (during init, or later from a hook) */
    void   (*add_tool)(const AgcTool*);
    void   (*add_command)(const AgcCommand*);
    void   (*add_section)(const AgcSection*);
    void   (*add_status)(AgcStatusProvider, void*);
    void   (*add_provider)(const AgcProvider*);

    /* subscribe to a hook point; and publish on the same bus */
    void   (*on)(const char *point, uint32_t caps, int priority, AgcHookFn, void *ud);
    void   (*emit)(const AgcExtHost*, const char *point, const char *payload_json);

    /* services */
    void   (*defer)(const AgcExtHost*, void (*fn)(void*), void*);
    int    (*is_cancelled)(const AgcExtHost*, const char *signal_token);
    const char *(*cwd)(const AgcExtHost*);
    const char *(*session_id)(const AgcExtHost*);
    const char *(*session_file)(const AgcExtHost*);
    const char *(*system_prompt)(const AgcExtHost*);
    uint64_t (*http_request)(const AgcExtHost*, const char *method, const char *url,
                             const char *headers_json, const char *body, uint64_t len,
                             void (*cb)(void*, int, const char*, const char*, uint64_t),
                             void*);
    void   (*http_cancel)(const AgcExtHost*, uint64_t request);
    void   (*notify)(const char *message, int level);
    void   (*set_status)(const char *key, const char *text);
    int    (*set_model)(const char *provider, const char *model);
    void   (*set_thinking)(const char *level);
    void   (*request_recompose)(const AgcExtHost*);   /* my tools/sections changed */
};

typedef struct {
    uint32_t abi_version, struct_size;
    const char *name, *version;
    int  (*init)(const AgcExtHost*);           /* register contributions + hooks */
    void (*shutdown)(void);                    /* idempotent; may be NULL */
} AgcExt;

/* the only exported symbol */
int agentc_ext_init(const AgcExtHost *host, AgcExt *out);
```

~24 host slots, all with a consumer. No JSON helpers (hooks carry JSON; callers who
need fields parse with `base/json.h`, which is always available in-tree).

---

## 3. What the core consumes (the seams are real)

| contribution | consumer |
|---|---|
| `add_tool` | tool registry → provider serialization + job dispatch |
| `add_command` | slash-command table |
| `add_section` | prompt builder renders sections in priority order |
| `add_status` | the existing status-line provider table (`status.c`) |
| `add_provider` | provider lookup consults a small runtime registry |
| `on` | hook bus, called from the loop at each emitted point |
| `emit` | same bus, so extensions publish standard points |

Default extensions registered by the app: **`builtin-tools`** (read/bash/edit/write/
ls/find/grep via `add_tool`) and **`mcp`** (`add_tool` as servers connect). Skills,
filesystem context and `APPEND_SYSTEM.md` become built-in **sections**, so the
prompt is assembled from one registry rather than hardcoded `prompt.c` calls.

---

## 4. Hook points

Names are pi's. **Cap** is `observe` (notify only) or `override` (may return a
result). **Pi result** is the object pi documents for that event — the same shape
the handler returns, so pi's types transfer. **Core** is whether the core emits the
point today (cheap, because the state already exists) or it is reserved for an
extension to publish.

| point | cap | pi result | core |
|---|---|---|---|
| `project_trust` | override | `{trusted, remember?}` | emits |
| `session_start` / `session_shutdown` | observe | — | emits |
| `resources_discover` | override | `{skillPaths?, promptPaths?, themePaths?}` | emits |
| `input` | override | `{action: continue\|transform\|handled}` | emits |
| `before_agent_start` | override | `{message?, systemPrompt?}` | emits |
| `agent_start` / `agent_end` | observe | — | emits |
| `agent_before_settle` | override | `{entries?, continue?}` | emits |
| `agent_settled` | observe | — | emits |
| `turn_start` | observe | — | emits |
| `turn_end` | override | `{entries?, continue?}` | emits |
| `message_start` / `message_update` | observe | — | emits |
| `message_end` | override | `{message?}` | emits |
| `tool_call` | override | `{block?, reason?, terminate?}` / mutate `input` | emits |
| `tool_result` | override | `{content?, details?, isError?, usage?}` | emits |
| `tool_execution_start/update/end` | observe | — | emits |
| `before_provider_headers` | override | mutate headers | emits |
| `before_provider_request` | override | replacement payload | emits |
| `after_provider_response` | observe | — | emits |
| `provider_stream_event` | observe | — | emits |
| `model_select` / `thinking_level_select` | observe | — | emits |
| `session_before_compact` | override | `{cancel?, compaction?}` | emits |
| `session_compact` / `session_compact_failed` | observe | — | emits |
| `session_before_fork` | override | `{cancel?, skipConversationRestore?}` | emits |
| `mcp_servers_change` | observe | — | emits |
| `session_info_changed`, `session_before_switch`, `session_before_tree`, `session_tree` | — | — | reserved |
| `context`, `context_with_system` | — | — | folded into `before_provider_request` |
| `cache_warming_decision`, `ui_prompt_start/end`, `user_bash` | — | — | reserved (no core feature) |

Reserved means: the core has no such state to hook yet. An extension that adds the
feature emits it on the shared bus (`host->emit`), and other extensions consume it.

**Dispatch.** Subscribers run in `priority` order (ties: registration order).
`OBSERVE` returns are ignored. An `OVERRIDE` handler returns a pi-shaped result
object: a decision (`block`, `cancel`, `continue: true`) or a replacement
(`input`, `systemPrompt`, `content`, `messages`, `entries`). Per point, either
first-decision-wins (`tool_call`, `project_trust`, `session_before_*`) or a chain
where each handler sees the previous result (`tool_result`, `message_end`,
`before_provider_request`). A handler that returns a result while declaring only
`OBSERVE` is logged and ignored. Hooks are synchronous and budgeted like status
providers; I/O work uses `defer` at pausable points only (`before_agent_start`,
`input`, resource discovery).

---

## 5. What this buys the "full pi model later"

With only this core, an extension can add without core changes:

- **skills** — an `add_section` that lists them + an `add_tool` to load a body;
- **MCP resources/prompts** — tools + sections (prompts) on the MCP extension;
- **permission gates** — a `tool_call` override handler;
- **transforms** — `input`, `tool_call`, `tool_result`, `before_provider_request`,
  `message_end`;
- **continuation / workflows** — `turn_end` / `agent_before_settle` inject;
- **custom providers** — `add_provider`;
- **new features with their own events** — `emit` on the shared bus.

## 6. Non-goals

No durable execution/tasks/transactions/storage. No codemode. No remote/`server`.
No UI dialogs/widgets/footer/editor. No session tree/switch. No MCP OAuth. No
`context`/`cache_warming`/`user_bash`. No wrapping middleware. No typed per-event
payload structs (JSON is one shape for all).

---

## 7. Phasing

- **P1 — the ABI.** Add `agentc_ext.h`, host/registry, hook bus with caps +
  priority. Port `builtin-tools`, `mcp`, and the plugin adapter to `AgcExt`; make
  skills/context built-in sections. Delete `agentc_plugin.h`'s v1/v2 machinery.
  Behavior-preserving. Gate: full suite green; `tests/plugin.sh` on a rebuilt
  plugin.
- **P2 — async MCP + recompose.** `agentc_mcp_start`/`pump`, late `add_tool`,
  `request_recompose` at the turn boundary, `mcp_servers_change`.
- **P3 — hook emission.** Emit the "core" points, wire the `tool_call` and
  `tool_result` overrides; fold `set_status` into sections.
- **P4 — on demand.** Provider-registry consumption, more points, an
  extension-provided skill/MCP-prompts layer.

## 8. Acceptance

- One header, one `abi_version`, no compat range; `agentc_plugin.h` gone.
- `main.c` contains no builtin/plugin/mcp merge and no prompt-section
  hardcoding; it loads default extensions.
- `builtin-tools` and `mcp` are ordinary extensions; disabling `mcp` in config
  removes it.
- An unreachable MCP server does not delay the first prompt; late tools appear at
  the next turn.
- `make check`, `tests/mcp.sh`, `tests/plugin.sh`, `tests/e2e.sh` green.
