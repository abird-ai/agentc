/* config.h — JSONC configuration, auth and prompt resources.
 *
 * Files (all JSONC; our writer only emits plain JSON):
 *   ~/.config/agentc/config.jsonc   user config            (XDG_CONFIG_HOME)
 *   .agentc/config.jsonc            project config          (trust-gated)
 *   ~/.config/agentc/auth.jsonc     API keys                (mode 0600)
 *   ~/.config/agentc/trust.jsonc    project trust decisions
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
    char *id;                   /* the <id> in providers.<id> / api_keys.<id> */
    char *value;                /* base URL or API key (owned) */
} AgcConfigEntry;

typedef struct {
    char *default_provider;     /* "openai" */
    char *default_model;        /* "claude-sonnet-4-5" */
    char *default_thinking;     /* "off|low|medium|high" */
    char **default_tools;       /* NULL = builtin four */
    size_t ndefault_tools;
    char *theme;
    char *base_url_anthropic;
    char *base_url_openai;
    char *base_url_ollama;        /* providers.ollama.base_url */
    char *base_url_ollama_cloud;
    char *api_key_anthropic;    /* last resort; discouraged */
    char *api_key_openai;
    char *session_dir;
    int max_attempts;           /* retry policy, default 5 */
    i64 max_tokens;             /* 0 = model default */
    char *shell;                /* bash tool shell: auto|cmd|powershell|pwsh (Windows) */
    char *tui_mode;             /* TUI mode: auto|scrollback|inline|fullscreen */
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
} AgcConfig;

AgcConfig *agentc_config_load(const char *cwd);   /* never NULL; defaults on absence */
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

/* $XDG_CONFIG_HOME/agentc (or $HOME/.config/agentc); NULL when neither is set. */
const char *agentc_config_home(char *buf, size_t cap);

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
const char *agentc_config_setup_path(char *buf, size_t cap);

const char *agentc_config_base_url(const AgcConfig *c, const char *provider);
const char *agentc_config_api_key(const AgcConfig *c, const char *provider);

/* auth.jsonc / env lookup for a provider; returned string is owned by a global
 * store (agentc_auth_free() releases; call once at exit). Never returns a pointer
 * into a password prompt. */
const char *agentc_auth_key(const char *provider);
/* Store an API key for a provider in auth.jsonc (0600), preserving every other
 * provider and sibling field. Returns 0 or -errno. */
int agentc_auth_set_key(const char *provider, const char *key);
void agentc_auth_free(void);

/* --------------------------------------------------------------- pricing */
/* Local model pricing override, <config home>/agentc/pricing.jsonc (USD per
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

/* Skills: ~/.config/agentc/skills and (trusted only) .agentc/skills, one directory (or
 * *.md) per skill with SKILL.md carrying name/description frontmatter. Project
 * skills feed the system prompt, so an untrusted repo must not contribute. */
typedef struct {
    char *name;
    char *description;
    char *path;                 /* the SKILL.md path */
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
    char *hint;                 /* argument-hint frontmatter, else "" */
    char *path;
} AgcPromptTemplate;

size_t agentc_templates_load(const char *cwd, bool trusted, AgcPromptTemplate **out, size_t max);
/* Extra-roots form: <config>/prompts, <cwd>/.agentc/prompts (trusted) and each
 * extra root in order; a later template replaces an earlier same-name one. */
size_t agentc_templates_load_extra(const char *cwd, bool trusted,
                                   const char *const *extra_roots, size_t nextra,
                                   AgcPromptTemplate **out, size_t max);
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

size_t agentc_themes_load(const char *cwd, bool trusted,
                          const char *const *extra_roots, size_t nextra,
                          AgcThemeEntry **out, size_t max);
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

#endif /* AGENTC_CONFIG_H */
