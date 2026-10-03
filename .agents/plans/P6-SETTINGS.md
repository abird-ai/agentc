# P6-SETTINGS — Extension settings (grouped by extension) and prompt contributions

Status: **as-built (P6-A..E + completion passes 2a-2g).** Extension settings and prompt contributions.
As-built note: the outcome is folded into `.agents/design/30-extensibility.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

## 0. Naming (settled)

| concept | name | what it is |
| --- | --- | --- |
| **setting** | `AgcExtSetting` + `host->add_setting` | a typed **value the user configures**; automatically grouped under the **extension's own name** (no separate group object) |
| **prompt contribution** | `AgcExtPrompt` + `host->add_prompt` | **system-prompt text** inserted **before** or **after** the four core sections |
| **prompt transform** | the `prompt` hook stage (`P6-HOOKS.md`) | rewrite the whole assembled prompt |
| **user input** | the `input` hook stage | the user/task text — *not* a prompt contribution |

So: `add_setting` configures; `add_prompt` adds prompt text; the `prompt` stage
transforms; the `input` stage handles user input. "Section" was a vague name; the
contribution is a **prompt contribution**, spelled `add_prompt`.

## 1. The gap this closes

`AgcExtHost` has no settings/config accessor — extensions cannot declare typed
config or read `config.jsonc`. This adds a **service over the existing JSONC
config loader** (not a primitive): one call, grouped by extension, with defaults,
validation, a UI/CLI/RPC surface, and secrets handled correctly.

## 2. Settings — one call, grouped by extension

```c
typedef enum {
    AGENTC_SET_BOOL = 0, AGENTC_SET_INT, AGENTC_SET_FLOAT, AGENTC_SET_STRING,
    AGENTC_SET_ENUM, AGENTC_SET_PATH, AGENTC_SET_LIST,     /* list of strings */
} AgcSettingType;

#define AGENTC_SETTING_SECRET   0x1u  /* store in auth.jsonc, never print */
#define AGENTC_SETTING_ADVANCED 0x2u  /* hidden behind /settings --all */
#define AGENTC_SETTING_RESTART  0x4u  /* applies on restart */
#define AGENTC_SETTING_PROJECT  0x8u  /* project scope allowed (trust-gated) */

typedef struct AgcExtSetting {
    uint32_t    struct_size;
    const char *key;            /* unique within the extension, [a-z0-9_.-]+ */
    const char *label;
    const char *description;
    int         type;           /* AgcSettingType */
    const char *default_json;   /* default value (JSON) */
    const char *enum_json;      /* ["a","b"] for ENUM */
    const char *min_json;       /* optional bound */
    const char *max_json;       /* optional bound */
    const char *unit;           /* optional, e.g. "ms", "tokens" */
    uint32_t    flags;          /* AGENTC_SETTING_* */
} AgcExtSetting;

/* Register in init. The setting is owned by the registering extension; its
 * group key is the extension's `name`. No group object, no group call. */
void (*add_setting)(const AgcExtHost *, const AgcExtSetting *);
```

- The **group is the extension** (`AgcExt.name`). The group label is the
  extension's display name; add `display_name` (and an optional
  `settings_priority`) to `AgcExt` if a human label/order is wanted; otherwise
  `name` and registration `order` are used.
- `/settings` therefore shows **one group per extension**, in extension order.
- A setting may also be contributed by the app/core (built-ins) using the same
  registry with a reserved group name.

## 3. Storage and resolution

```jsonc
"settings": {
  "realtime-voice": { "enabled": true, "mode": "webrtc", "voice": "alloy" },
  "governance":     { "controller": "jev", "per_tool": true }
}
```

- **User** config: `~/.config/abird/agentc/config.jsonc`.
- **Project** config: `.agentc/config.jsonc` — only for `AGENTC_SETTING_PROJECT`
  settings and only when the project is trusted (tighten-only otherwise).
- **Resolution:** CLI `--set <ext>.<key>=value` > trusted project > user >
  declared `default_json`.
- **Unknown keys preserved** (JSONC forward-compat); only declared settings are
  validated/exposed.
- **Secrets** (`SECRET`) live in `~/.config/abird/agentc/auth.jsonc` (0600), never in
  config, never printed (only `…last4`).
- **Validation:** type/enum/bounds on load; invalid → default + one warning
  (fail-safe).
- **Write:** atomic temp+rename to the correct scope, as existing config writes.

## 4. Accessors

Core:

```c
const char *agentc_settings_get_json(const char *ext, const char *key);   /* resolved */
bool        agentc_settings_get_bool(const char *ext, const char *key, bool dflt);
long long   agentc_settings_get_int(const char *ext, const char *key, long long dflt);
double      agentc_settings_get_float(const char *ext, const char *key, double dflt);
int         agentc_settings_set(const char *ext, const char *key, const char *value_json);
int         agentc_settings_reset(const char *ext, const char *key);
size_t      agentc_settings_list(AgcSettingInfo *out, size_t max);       /* count-first */
```

Host (an extension reads its own settings; `ext` is its registered name):

```c
char *(*setting_get)(const AgcExtHost *, const char *ext, const char *key); /* JSON */
int   (*setting_set)(const AgcExtHost *, const char *ext, const char *key,
                     const char *value_json);
```

A `SECRET` value is never returned through a model tool; only trusted UI code.

## 5. Front ends

- **TUI** `/settings [ext]`: one group per extension; typed editors (toggle,
  number, enum, path, list); shows current + default; `--all` reveals `ADVANCED`;
  `RESTART` fields show a restart hint.
- **RPC:** `get_settings {ext?}`, `set_setting {ext,key,value}`,
  `reset_setting {ext,key}`.
- **CLI:** `--set <ext>.<key>=value` (repeatable), `--list-settings`.
- **Events/hooks:** `AGENTC_EV_SETTINGS_CHANGED {ext,key,scope}` and a
  `settings_change` hook (observe/validate; a handler may reject a value,
  fail-closed). `RESTART` emits but does not live-apply.

## 6. Prompt contributions (the renamed "section")

The four core sections are literal text the core writes in
`src/core/prompt.c` `prompt_core()` — **preamble**, **tools** (one line per
active, non-hidden tool), **rules**, **environment** (cwd + platform). They are
not a registry. Extension/app contributions address them by **anchor** and can
**append around** or **replace** them:

```c
#define AGENTC_PROMPT_BEFORE  0u
#define AGENTC_PROMPT_AFTER   1u
#define AGENTC_PROMPT_APPEND  0u   /* add around the anchor (default) */
#define AGENTC_PROMPT_REPLACE 1u   /* supersede the anchor's default render */
#define AGENTC_ANCHOR_ALL          0u
#define AGENTC_ANCHOR_PREAMBLE     1u
#define AGENTC_ANCHOR_TOOLS        2u
#define AGENTC_ANCHOR_RULES        3u
#define AGENTC_ANCHOR_ENVIRONMENT  4u

typedef struct AgcExtPrompt {
    uint32_t    struct_size;
    const char *key;
    int32_t     priority;                /* lower first within an anchor */
    void       *ud;
    int (*render)(const AgcExtHost *host, void *ud, void *out);  /* append UTF-8 */
    int         anchor;                  /* which core section (or ALL) */
    int         position;                /* BEFORE | AFTER (APPEND only) */
    int         mode;                    /* APPEND | REPLACE */
} AgcExtPrompt;

void (*add_prompt)(const AgcExtHost *, const AgcExtPrompt *);
```

### Who populates each section, and how to override it

| anchor | default populated by | override |
| --- | --- | --- |
| **preamble** | core constant identity text | `REPLACE` (was not possible before) |
| **tools** | the live tool table (name/desc; a tool's `prompt_snippet` overrides its line) | `APPEND`/`REPLACE`; hidden tools are omitted |
| **rules** | core bullets **+ each tool's `prompt_guidelines`** | `APPEND` (add bullets) / `REPLACE` |
| **environment** | core `cwd` + `os_platform()` | `APPEND`/`REPLACE` (app adds date/model/trust/sandbox) |

- **Precedence:** `--system` > `REPLACE(ALL)` > per-anchor `REPLACE` (highest
  priority wins; losers logged) > core default; `APPEND` renders around the
  resolved anchor in `priority` order.
- **Preamble override:** `{anchor=PREAMBLE, mode=REPLACE}` replaces the identity
  text — the one section that had no override path.
- **Tools/rules are tool-driven:** a tool contributes its `# Tools` line and its
  `# Rules` bullets at registration; a `prompt`-stage hook can rewrite either.
- Core change: `prompt_core` resolves each anchor (default renderer or the
  winning `REPLACE`) and calls `agentc_ext_render_sections(out, anchor)` at each
  boundary for the `APPEND` contributions.

### Resolution algorithm (authoritative)

```text
build_prompt(tools, out):
  if --system:            out = system_override; return    # wins everything
  append(ALL, BEFORE)                                      # priority order
  if REPLACE(ALL):        render(that)                     # supersedes the whole core block
  else:
    for anchor in [PREAMBLE, TOOLS, RULES, ENVIRONMENT]:
      append(anchor, BEFORE)
      render( REPLACE(anchor) ?? core_default(anchor, tools) )
      append(anchor, AFTER)
  append(ALL, AFTER)
  hook_run("prompt", assembled)                            # may still rewrite
```

- `REPLACE(anchor)` = highest-priority `REPLACE` for that anchor (ties by
  registration order); `position` is ignored for `REPLACE`.
- `REPLACE(ALL)` supersedes the whole block; per-anchor `REPLACE` is then moot
  (logged). Precedence: `--system` > `REPLACE(ALL)` > per-anchor `REPLACE` > core
  default; `APPEND` wraps the resolved content in `(position, priority)` order.
- Core defaults: PREAMBLE = identity constant; TOOLS = one row per active
  non-hidden tool (`name: prompt_snippet ?? desc`); RULES = core bullets + each
  tool's `prompt_guidelines`; ENVIRONMENT = `cwd` + `os_platform()`.
- `render` returning `<0` is skipped with a log; a missing `render` is dropped at
  registration. Re-adding the same `key` replaces your prior contribution.
- To rewrite the whole prompt, use the `prompt` stage hook; to add around **user
  input**, use the `input` stage hook (`add_prompt` never touches user input).
- There is no legacy alias — `add_section` is removed and the struct renamed
  `AgcExtPrompt` (a deliberate breaking change; P6 is pre-release).

## 7. Example (voice settings + a prompt contribution)

```c
static const AgcExtSetting voice_enabled = {
  .struct_size=sizeof voice_enabled, .key="enabled", .label="Enable voice",
  .type=AGENTC_SET_BOOL, .default_json="false" };
static const AgcExtSetting voice_mode = {
  .struct_size=sizeof voice_mode, .key="mode", .label="Transport",
  .type=AGENTC_SET_ENUM, .default_json="\"webrtc\"",
  .enum_json="[\"webrtc\",\"ws\"]", .flags=AGENTC_SETTING_RESTART };
static int render_identity(const AgcExtHost *h, void *ud, void *out) {
  h->out_write(out, "You are Acme's coding agent.\n", 29); return 0;
}
static int render_voice_hint(const AgcExtHost *h, void *ud, void *out) {
  h->out_write(out, "Voice is available; use /voice.\n", 31); return 0;
}
/* init: */
h->add_setting(h, &voice_enabled);     /* group = this extension's name */
h->add_setting(h, &voice_mode);
h->add_prompt(h, &(AgcExtPrompt){ .struct_size=sizeof(AgcExtPrompt),
    .key="identity", .anchor=AGENTC_ANCHOR_PREAMBLE, .mode=AGENTC_PROMPT_REPLACE,
    .render=render_identity });        /* override the preamble */
h->add_prompt(h, &(AgcExtPrompt){ .struct_size=sizeof(AgcExtPrompt),
    .key="rules-voice", .anchor=AGENTC_ANCHOR_RULES, .position=AGENTC_PROMPT_AFTER,
    .mode=AGENTC_PROMPT_APPEND, .priority=20,
    .render=render_voice_hint });      /* append a rule */
/* runtime: */ const char *mode = h->setting_get(h, "realtime-voice", "mode");
```

## 8. Minimality

- No new primitive: settings are a registry + validation + accessors over the
  JSONC config loader; prompt contributions are text into the `prompt` stage.
- ABI growth is append-only: `AgcExtSetting`, `add_setting`, `setting_get/set`,
  `add_prompt`, and a `position` field appended to `AgcExtPrompt`.
- No settings group object (the extension is the group). Front ends are adapters.

## 9. Handoff

> Add `AgcExtSetting` + `add_setting` + `setting_get`/`setting_set` to
> `include/agentc_ext.h`; rename `AgcExtSection`→`AgcExtPrompt` and
> `add_section`→`add_prompt` (breaking change, no alias) and add a `position`
> field. Implement `src/core/settings.c` over the existing
> `config.{c,h}` JSONC load/write path and `auth.jsonc` for secrets; add
> `/settings`, RPC `get_settings`/`set_setting`, CLI `--set`/`--list-settings`,
> the `settings_change` hook and `AGENTC_EV_SETTINGS_CHANGED`. No settings group
> object. Tests: `tests/settings_test.c` (defaults, resolution order, validation/
> fallback, secret redaction, project scope trust-gating, persistence round-trip,
> prompt before/after ordering, byte-exact `/settings` render).
