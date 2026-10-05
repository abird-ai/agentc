/* config.h — JSONC configuration, auth and prompt resources.
 *
 * Files (all JSONC; our writer only emits plain JSON):
 *   ~/.config/abird/agentc/config.jsonc   user config            (XDG_CONFIG_HOME)
 *   .agentc/config.jsonc                  project config          (trust-gated)
 *   ~/.config/abird/agentc/auth.jsonc     API keys                (mode 0600)
 *   ~/.config/abird/agentc/trust.jsonc    project trust decisions
 *
 * Resolution order for a provider key: --api-key flag > ANTHROPIC_API_KEY /
 * OPENAI_API_KEY env > auth.jsonc > config.jsonc api_keys.
 */
#ifndef AGENTC_CONFIG_H
#define AGENTC_CONFIG_H

#include "agentc.h"

/* Generic config entries: user-scope providers.<id>.base_url and
 * api_keys.<id>. Both lists are capped and ids longer than the name maximum
 * are ignored, so a hostile config cannot grow the loader without bound. The
 * four named presets below win over a generic entry with the same id. */
#define AGENTC_CONFIG_GENERIC_MAX 32
#define AGENTC_CONFIG_GENERIC_NAME_MAX 64

typedef struct {
    char *id;    /* the <id> in providers.<id> / api_keys.<id> */
    char *value; /* base URL or API key (owned) */
} AgcConfigEntry;

/* Appended (W4, virtual models): one `models.<alias>` entry. The alias names a
 * virtual model that resolves per model step to the concrete {provider, model}
 * target; the alias itself stays the transcript identity. Bounded and
 * fail-closed: an entry missing a non-empty provider or model is dropped, never
 * partially stored. */
typedef struct {
    char *alias;    /* the <alias> key */
    char *provider; /* concrete target provider, e.g. "anthropic" */
    char *model;    /* concrete target model id, e.g. "claude-sonnet-4-5" */
} AgcConfigModelRoute;

typedef struct {
    char *default_provider; /* "openai" */
    char *default_model;    /* "claude-sonnet-4-5" */
    char *default_thinking; /* "off|low|medium|high" */
    char **default_tools;   /* NULL = builtin four */
    size_t ndefault_tools;
    char *theme;
    char *base_url_anthropic;
    char *base_url_openai;
    char *base_url_ollama; /* providers.ollama.base_url */
    char *base_url_ollama_cloud;
    char *api_key_anthropic; /* last resort; discouraged */
    char *api_key_openai;
    char *session_dir;
    int max_attempts;           /* retry policy, default 5 */
    i64 max_tokens;             /* 0 = model default */
    char *shell;                /* bash tool shell: auto|cmd|powershell|pwsh (Windows) */
    char **extensions_disabled; /* extensions.disabled: default exts to skip */
    size_t nextensions_disabled;

    /* Generic providers.<id>.base_url and api_keys.<id> entries
     * for provider ids without a named field (extension providers, user
     * gateways). User scope only, each capped at AGENTC_CONFIG_GENERIC_MAX. */
    AgcConfigEntry *providers;
    size_t nproviders;
    AgcConfigEntry *api_keys;
    size_t napi_keys;

    /* Appended P5: tools.engine ("external" | "internal"); resolved by
     * src/core/tools/engine.c at builtin-tools init time. */
    char *tools_engine;

    /* Appended: the Codex/ChatGPT `client_version` agentc declares to
     * `GET {base}/models` (the backend gates model visibility on it). NULL = use
     * the OPENAI_CLIENT_VERSION / AGENTC_CODEX_CLIENT_VERSION env override or the
     * built-in default. */
    char *openai_client_version;

    /* Appended P6-B: queue/hook policy (SURF §3.4/6c). */
    bool queue_auto_start;        /* trust.queue.auto_start, default true */
    int queue_submit;             /* trust.queue.submit, default 0 */
    int queue_urgent;             /* trust.queue.urgent, default -1 */
    bool queue_drop_partial;      /* trust.queue.drop_partial, default true */
    bool queue_paused;            /* trust.queue.paused, default false */
    int queue_max;                /* trust.queue.max, default 32 */
    char *hooks_on_error_default; /* hooks.on_error_default, "pass"|"deny" */
    bool tools_hooks;             /* tools.hooks, default true */

    /* Appended P6-C: trust.* (SURF §3.4). NULL/0 = built-in default. */
    char *trust_access;    /* "read-only" | "read-write" */
    char *trust_sandbox;   /* "none" | "workspace" | "strict" */
    char *trust_approval;  /* "auto" | "ask" | "policy" */
    char *trust_network;   /* "deny" | "allow" | "allowlist" */
    char *trust_widen;     /* "deny" | "clamp" */
    char *trust_policy_id; /* named policy or inline JSONC */
    char **trust_roots;
    size_t ntrust_roots;
    char **trust_hosts;
    size_t ntrust_hosts;
    i64 trust_approval_timeout_ms; /* 0 = default 120000 */
    bool trust_approval_persist;
    char *trust_sandbox_backend; /* auto|landlock|bwrap|seatbelt|token */

    /* Appended W4: parsed top-level `models.<alias>` virtual routes (owned).
     * Upserted by alias across config files; capped at AGENTC_CONFIG_MODELS_MAX. */
    AgcConfigModelRoute *model_routes;
    size_t nmodel_routes;
} AgcConfig;

AgcConfig *agentc_config_load(const char *cwd); /* never NULL; defaults on absence */
/* A pristine config with the built-in defaults and no file I/O. Used by the
 * SDK when load_user_config/load_project_config are off (B-D2 security-first
 * default). Free with agentc_config_free() like any loaded config. */
AgcConfig *agentc_config_defaults(void);
void agentc_config_free(AgcConfig *c);

/* Shell for the bash tool. `agentc_config_shell` returns the effective kind
 * ("auto" until a config/env value applies): on Windows "auto" is cmd.exe;
 * "powershell" prefers PowerShell 7 and falls back to built-in 5.1; "pwsh"
 * requires PowerShell 7. On POSIX only "auto"/"sh" are valid, so a
 * Windows-only value fails loudly instead of silently running /bin/sh. The
 * AGENTC_SHELL environment variable overrides config.jsonc. */
const char *agentc_config_shell(void);
void agentc_config_set_shell(const char *kind);

/* Core-tool backend engine (P5). "external" (default) prefers the environment's
 * ripgrep/fd/grep/find found on PATH and falls back to the built-in
 * implementations; "internal" forces the built-ins and never spawns. Precedence:
 * --tools-engine > AGENTC_TOOLS_ENGINE > tools.engine in config.jsonc > default.
 * Invalid values are ignored with a warning by the consuming code. */
const char *agentc_config_tools_engine(void);
void agentc_config_set_tools_engine(const char *value);

/* $XDG_CONFIG_HOME/abird/agentc (or $HOME/.config/abird/agentc); NULL when neither is set.
 * When the new root has never been created but the legacy `<base>/agentc` one
 * exists, the legacy root is returned for transparent read+write compatibility. */
const char *agentc_config_home(char *buf, size_t cap);

/* $XDG_DATA_HOME/abird/agentc (or $HOME/.local/share/abird/agentc), with the same
 * legacy-root fallback as agentc_config_home. */
const char *agentc_data_home(char *buf, size_t cap);

/* $XDG_STATE_HOME/abird/agentc (or $HOME/.local/state/abird/agentc), with the same
 * legacy-root fallback as agentc_config_home. */
const char *agentc_state_home(char *buf, size_t cap);

/* Environment lookup (no libc getenv). */
const char *agentc_env_get(const char *name);

/* Shared file helpers (implemented in config.c; used across core). */
char *agentc_read_file_owned(const char *path, size_t *out_len);
bool agentc_path_join(char *out, size_t cap, const char *dir, const char *name);
int agentc_mkdir_parents(const char *path);
int agentc_write_file_atomic(const char *path, const void *data, size_t len, int mode);
/* First-run choices written by the onboarding flow land in setup.jsonc, which
 * loads between the built-in defaults and config.jsonc (config wins). */
int agentc_config_save_setup(const char *provider, const char *model);
/* Rewrite setup.jsonc's default_provider/default_model, preserving every other
 * top-level key a user may have added. Used by `agentc login` so the provider
 * just authenticated becomes the default for the next start. Returns 0 on
 * success, 1 when config.jsonc pins default_provider (setup.jsonc would be a
 * no-op, so nothing is written and the caller should tell the user), or a
 * negative errno. */
int agentc_config_setup_set_default(const char *provider, const char *model);
/* Persist a custom provider endpoint in setup.jsonc (agentc-owned, first-run
 * scope), merge-preserving every other key: providers.<id>.base_url. The loader
 * reads setup.jsonc with user scope, so the entry materializes an
 * OpenAI-compatible gateway on the next start. Returns 0 or a negative errno. */
int agentc_config_setup_set_provider(const char *id, const char *base_url);
const char *agentc_config_setup_path(char *buf, size_t cap);

const char *agentc_config_base_url(const AgcConfig *c, const char *provider);
const char *agentc_config_api_key(const AgcConfig *c, const char *provider);

/* P6-B queue/hook policy accessors (never NULL; defaults when unset). */
bool agentc_config_queue_auto_start(const AgcConfig *c);
int agentc_config_queue_submit(const AgcConfig *c);
int agentc_config_queue_urgent(const AgcConfig *c);
bool agentc_config_queue_drop_partial(const AgcConfig *c);
bool agentc_config_queue_paused(const AgcConfig *c);
int agentc_config_queue_max(const AgcConfig *c);
const char *agentc_config_hooks_on_error(const AgcConfig *c);
bool agentc_config_tools_hooks(const AgcConfig *c);

/* P6-C trust accessors (never NULL; built-in defaults when unset). */
const char *agentc_config_trust_access(const AgcConfig *c);
const char *agentc_config_trust_sandbox(const AgcConfig *c);
const char *agentc_config_trust_approval(const AgcConfig *c);
const char *agentc_config_trust_network(const AgcConfig *c);
const char *agentc_config_trust_widen(const AgcConfig *c);
const char *agentc_config_trust_policy_id(const AgcConfig *c);
const char *const *agentc_config_trust_roots(const AgcConfig *c, size_t *n);
const char *const *agentc_config_trust_hosts(const AgcConfig *c, size_t *n);
i64 agentc_config_trust_timeout_ms(const AgcConfig *c);
bool agentc_config_trust_persist(const AgcConfig *c);
const char *agentc_config_trust_sandbox_backend(const AgcConfig *c);

/* Appended P6-D: sub-agent scheduling defaults (trust.subagent.*). No
 * `default_subagent_model` exists (R-D2): a child inherits the parent's current
 * provider/model/thinking/transport. The no-argument accessors read the
 * process-global defaults so a core tool can consult them without a config
 * pointer; the setter exists for tests and the app. */
bool agentc_config_subagent_inherit(void);  /* default true */
int agentc_config_subagent_max_depth(void); /* default AGENTC_AGENT_MAX_DEPTH */
void agentc_config_set_subagent_max_depth(int depth);
const char *agentc_config_deciders_json(void); /* deciders.<id> block, borrowed */
void agentc_config_set_deciders_json(const char *json);

/* Appended W4: count-first enumeration of the configured `models.<alias>`
 * virtual routes, in declaration order. With out == NULL returns the total;
 * otherwise fills up to max and returns how many were written. The strings are
 * borrowed from the config. The store is bounded at AGENTC_CONFIG_MODELS_MAX;
 * malformed entries never reach it. */
#define AGENTC_CONFIG_MODELS_MAX 64
size_t agentc_config_model_routes(const AgcConfig *c, AgcConfigModelRoute *out, size_t max);

/* auth.jsonc / env lookup for a provider; returned string is owned by a global
 * store (agentc_auth_free() releases; call once at exit). Never returns a pointer
 * into a password prompt. */
const char *agentc_auth_key(const char *provider);
/* Store an API key for a provider in auth.jsonc (0600), preserving every other
 * provider and sibling field. Returns 0 or -errno. */
int agentc_auth_set_key(const char *provider, const char *key);
void agentc_auth_free(void);

/* --------------------------------------------------------------- pricing */
/* Local model pricing override, <config home>/abird/agentc/pricing.jsonc (USD per
 * million tokens). It is loaded once at startup and always wins over both
 * discovered provider pricing and the built-in catalog. Returns the number of
 * entries applied (0 when the file is absent) or a negative errno; `err`
 * receives a short reason when non-NULL. */
int agentc_pricing_load(char *err, size_t cap);
/* GET https://openrouter.ai/api/v1/models, parse each data[].id + pricing and
 * MERGE the rates into pricing.jsonc (unknown keys preserved, atomic 0600),
 * then apply them. No API key is needed (public endpoint). Returns the number
 * of models synced or a negative errno. */
int agentc_pricing_sync_openrouter(char *err, size_t cap);

/* ------------------------------------------------------------- resources */

/* Context files: AGENTC.md/AGENTS.md/AGENTS.override.md/CLAUDE.md from the config
 * dir and every cwd ancestor (nearest last). Project files load only when
 * `trusted`. Each text is NUL-terminated and owned. */
typedef struct {
    char **paths;
    size_t *lens;
    char **texts;
    size_t n;
} AgcContextFiles;

AgcContextFiles *agentc_context_files_load(const char *cwd, bool trusted);
void agentc_context_files_free(AgcContextFiles *cf);
/* Appends <project_instructions path="…">…</project_instructions> sections. */
void agentc_prompt_add_project_context(AgcBuf *sys, const AgcContextFiles *cf);

/* Skills: ~/.config/abird/agentc/skills and (trusted only) .agentc/skills, one directory (or
 * *.md) per skill with SKILL.md carrying name/description frontmatter. Project
 * skills feed the system prompt, so an untrusted repo must not contribute. */
typedef struct {
    char *name;
    char *description;
    char *path; /* the SKILL.md path */
} AgcSkill;

size_t agentc_skills_load(const char *cwd, bool trusted, AgcSkill **out, size_t max);
void agentc_skills_free(AgcSkill *skills, size_t n);
/* Appends the <available_skills> block. */
void agentc_prompt_add_skills(AgcBuf *sys, const AgcSkill *skills, size_t n);

/* Prompt templates: *.md files in the prompts/ directory of the config dir,
 * project .agentc/prompts (trusted), and resources_discover promptPaths; the
 * file stem is the command name ([a-z0-9._:-]{1,64}; invalid stems are
 * skipped), frontmatter may carry description/argument-hint. */
typedef struct {
    char *name;
    char *description;
    char *hint; /* argument-hint frontmatter, else "" */
    char *path;
} AgcPromptTemplate;

size_t agentc_templates_load(const char *cwd, bool trusted, AgcPromptTemplate **out, size_t max);
/* Extra-roots form: <config>/prompts, <cwd>/.agentc/prompts (trusted) and each
 * extra root in order; a later template replaces an earlier same-name one. */
size_t agentc_templates_load_extra(const char *cwd, bool trusted, const char *const *extra_roots,
                                   size_t nextra, AgcPromptTemplate **out, size_t max);
void agentc_templates_free(AgcPromptTemplate *t, size_t n);
/* Expands $1..$n, $@/${ARGUMENTS}, ${N:-default}. Returns an owned string. */
char *agentc_template_expand(const AgcPromptTemplate *t, const char *args);

/* Named themes: *.jsonc files in the themes/ directory of the config dir,
 * project .agentc/themes (trusted), and resources_discover themePaths; the
 * file stem ([A-Za-z0-9._-]{1,64}) is the theme name. */
typedef struct {
    char *name;
    char *path;
} AgcThemeEntry;

size_t agentc_themes_load(const char *cwd, bool trusted, const char *const *extra_roots,
                          size_t nextra, AgcThemeEntry **out, size_t max);
void agentc_themes_free(AgcThemeEntry *t, size_t n);
/* Validated <name>.jsonc path for a named theme; searches the resource roots
 * (most specific first) then the config themes dir. 0 | -EINVAL | -ENOENT. */
int agentc_theme_path(const char *name, char *buf, size_t cap);

/* Project trust: --approve/--no-approve override; otherwise the saved decision
 * for the closest ancestor of cwd, else `default`. */
bool agentc_trust_resolve(const char *cwd, int cli_verdict /* -1 unset, 0 no, 1 yes */,
                          bool default_trusted);
/* Set by the CLI before agentc_config_load/agentc_trust_resolve: --approve/--no-approve
 * override (-1 unset), and the built-in default for an unset decision. */
void agentc_config_set_cli_trust(int verdict);
bool agentc_config_default_trusted(void);
void agentc_trust_save(const char *cwd, bool trusted);

/* Appended hardening (P8-D7 gap): like agentc_read_file_owned, but distinguishes
 * an absent path from one that exists yet could not be read. On success returns
 * the owned buffer and sets *out_err = 0. On failure returns NULL and sets
 * *out_err: 0 for absent (-ENOENT), else the negative errno (open/read error, or
 * -EIO when the file exceeds the 16 MiB cap). Merge writers use this to refuse
 * (rather than rename a fresh document over a file they cannot read). */
char *agentc_read_file_checked(const char *path, size_t *out_len, int *out_err);

#endif /* AGENTC_CONFIG_H */
