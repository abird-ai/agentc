/* select.c — provider selection, credentials and model filtering (Axis B4).
 *
 * Core half of the historical src/app/setup.c, extracted so the strict no-app
 * libagentc archive can link provider selection without the interactive
 * onboarding/discovery orchestration that stays in src/app/setup.c.
 *
 * The credential kind drives the provider choice. `openai` names two wire
 * backends: Chat Completions with an API key and the Codex Responses API with a
 * ChatGPT OAuth credential. Only an explicit --api-key flag wins over a stored
 * OAuth token; a provider env var or a config api_key does not reroute the wire
 * (see agentc_setup_resolve_key). The published context lets callers without the
 * CLI flag (the front-end agent rebuild) make the same choice; it owns copies,
 * never a config pointer, so it stays valid after the caller frees its config.
 */
#include "core/select.h"
#include "discover.h"
#include "oauth.h"
#include "wire.h"
#include "prov/provider.h"

/* ---------------------------------------------------------- published context */

static char *g_ctx_cli_key;             /* owned copy of --api-key, or NULL */
static char *g_ctx_cli_base;            /* owned copy of --base-url, or NULL */
static AgcConfigEntry *g_ctx_providers; /* owned copy of config providers.<id> */
static size_t g_ctx_nproviders;

/* Keep an owned copy of the config's providers.<id> entries. A user gateway is
 * only materialized when a lookup actually needs it (after extensions have had
 * a chance to register their own row), so a providers.<ext-id>.base_url is an
 * override of the extension row rather than a name conflict. */
static void ctx_set_providers(const AgcConfig *cfg) {
    for (size_t i = 0; i < g_ctx_nproviders; i++) {
        agentc_free(g_ctx_providers[i].id);
        agentc_free(g_ctx_providers[i].value);
    }
    agentc_free(g_ctx_providers);
    g_ctx_providers = NULL;
    g_ctx_nproviders = 0;
    if (!cfg || cfg->nproviders == 0)
        return;
    g_ctx_providers = agentc_alloc(cfg->nproviders * sizeof *g_ctx_providers);
    for (size_t i = 0; i < cfg->nproviders; i++) {
        const AgcConfigEntry *e = &cfg->providers[i];
        if (!e->id || !e->id[0] || !e->value || !e->value[0])
            continue;
        g_ctx_providers[g_ctx_nproviders].id = agentc_strdup(e->id);
        g_ctx_providers[g_ctx_nproviders].value = agentc_strdup(e->value);
        g_ctx_nproviders++;
    }
}

void agentc_setup_set_context(const AgcConfig *cfg, const char *cli_key, const char *cli_base) {
    agentc_free(g_ctx_cli_key);
    g_ctx_cli_key = (cli_key && cli_key[0]) ? agentc_strdup(cli_key) : NULL;
    agentc_free(g_ctx_cli_base);
    g_ctx_cli_base = (cli_base && cli_base[0]) ? agentc_strdup(cli_base) : NULL;
    ctx_set_providers(cfg);
    /* Publish the Codex client_version the config asks for; discovery resolves
     * it ahead of the built-in default (env still wins). */
    agentc_discover_set_codex_client_version(cfg ? cfg->openai_client_version : NULL);
}

const char *agentc_setup_cli_key(void) {
    return g_ctx_cli_key;
}
const char *agentc_setup_cli_base_url(void) {
    return g_ctx_cli_base;
}

/* ------------------------------------------------------------- credentials */

const char *agentc_setup_explicit_key(const AgcConfig *cfg, const char *name, const char *flag) {
    if (flag && flag[0])
        return flag;
    const AgcProviderOps *ops = agentc_provider_by_name(name);
    if (ops) {
        for (size_t j = 0; j < 3 && ops->env_keys[j]; j++) {
            const char *v = agentc_env_get(ops->env_keys[j]);
            if (v && v[0])
                return v;
        }
    }
    if (cfg) {
        const char *v = agentc_config_api_key(cfg, name);
        if (v && v[0])
            return v;
    }
    return NULL;
}

/* Effective request credential in the documented order: explicit --api-key flag
 * > stored OAuth credential (fail-closed: a refresh failure is an error, never a
 * fallback) > provider env var > auth.jsonc > config api_keys. main.c and the
 * provider choice both go through this rule so they cannot disagree. */
const char *agentc_setup_resolve_key(const AgcConfig *cfg, const char *name, const char *flag) {
    if (flag && flag[0])
        return flag;
    /* agentc_auth_key already implements OAuth > provider env > auth.jsonc and
     * returns NULL (without consulting anything else) when OAuth refresh fails. */
    const char *key = agentc_auth_key(name);
    if (key && key[0])
        return key;
    if (agentc_oauth_logged_in(name))
        return NULL;
    if (cfg) {
        const char *v = agentc_config_api_key(cfg, name);
        if (v && v[0])
            return v;
    }
    return NULL;
}

/* --------------------------------------------------------------- provider */

/* Only an explicit --api-key flag overrides a stored OAuth credential: a
 * provider env var or a config api_key does not change the wire. */
static const AgcProvider *openai_choice(const char *flag) {
    if (flag && flag[0])
        return agentc_prov_openai();
    if (agentc_oauth_logged_in("openai"))
        return agentc_prov_openai_codex();
    return agentc_prov_openai();
}

static const AgcProvider *provider_choice(const AgcConfig *cfg, const char *name,
                                          const char *flag) {
    if (!name)
        return NULL;
    if (agentc_streq(name, "openai"))
        return openai_choice(flag);
    /* by-name lookup materializes the OpenAI-compatible presets on demand */
    const AgcProviderOps *ops = agentc_provider_by_name(name);
    if (ops)
        return agentc_provider_handle((AgcProviderOps *)ops);
    /* An unknown id with a configured providers.<id>.base_url is a user gateway:
     * materialize an OpenAI-compatible row. Any other unknown id stays unknown. */
    if (cfg) {
        const char *base = agentc_config_base_url(cfg, name);
        if (base && base[0])
            return agentc_prov_openai_compatible_gateway(name, base);
    }
    /* Appended (P7): an unknown id with an explicit --base-url is an ad-hoc
     * custom OpenAI-compatible provider, so `--provider <id> --base-url <u>`
     * needs no config entry. */
    const char *cli = agentc_setup_cli_base_url();
    if (cli && cli[0])
        return agentc_prov_openai_compatible_gateway(name, cli);
    return NULL;
}

const AgcProvider *agentc_setup_provider_for(const AgcConfig *cfg, const char *name,
                                             const char *flag) {
    return provider_choice(cfg, name, flag);
}

const AgcProvider *agentc_setup_provider(const char *name) {
    if (!name)
        return NULL;
    if (agentc_streq(name, "openai"))
        return openai_choice(g_ctx_cli_key);
    const AgcProviderOps *ops = agentc_provider_by_name(name);
    if (ops)
        return agentc_provider_handle((AgcProviderOps *)ops);
    /* A config providers.<id>.base_url with no matching registry row is a user
     * gateway. Resolving it here (rather than at context publish) lets a loaded
     * extension register its own row first, so the config value is an override. */
    for (size_t i = 0; i < g_ctx_nproviders; i++)
        if (agentc_streq(g_ctx_providers[i].id, name))
            return agentc_prov_openai_compatible_gateway(name, g_ctx_providers[i].value);
    /* Appended (P7): an explicit --base-url with an unknown --provider is an
     * ad-hoc custom OpenAI-compatible provider. */
    if (g_ctx_cli_base && g_ctx_cli_base[0])
        return agentc_prov_openai_compatible_gateway(name, g_ctx_cli_base);
    return NULL;
}

/* ------------------------------------------------------------ model list */

size_t agentc_model_filter(const char *provider, const char *api, const AgcModel **out,
                           size_t max) {
    const AgcModel *all[2048];
    size_t total = agentc_model_all(all, 1024);
    size_t k = 0;
    for (size_t i = 0; i < total; i++) {
        const AgcModel *m = all[i];
        if (provider && !agentc_streq(m->provider, provider))
            continue;
        if (api && m->api && !agentc_streq(m->api, api))
            continue;
        if (out) {
            if (k >= max)
                break;
            out[k] = m;
        }
        k++;
    }
    return k;
}

bool agentc_setup_needs_key(const char *name) {
    const AgcProviderOps *ops = agentc_provider_by_name(name);
    return !ops || ops->needs_key;
}

const char *agentc_setup_base_url(const AgcConfig *cfg, const char *name, const char *flag) {
    if (!name)
        return NULL;
    const char *v = (flag && flag[0]) ? flag : (cfg ? agentc_config_base_url(cfg, name) : NULL);
    if (v && v[0])
        return v;
    /* One cached, process-lifetime buffer per env variable: a single scratch
     * buffer would let a later call for another provider clobber a pointer the
     * caller still holds. Only the env value that lacks a trailing /v1 needs a
     * new string; flag/config values above are borrowed and returned unchanged. */
    static char *g_env_base[3];
    static size_t g_env_base_cap[3];
    const char *env = NULL;
    int slot = -1;
    if (agentc_streq(name, "ollama")) {
        env = agentc_env_get("OLLAMA_HOST");
        slot = 0;
    } else if (agentc_streq(name, "ollama-cloud")) {
        env = agentc_env_get("OLLAMA_CLOUD_BASE_URL");
        slot = 1;
    } else if (agentc_streq(name, "openai")) {
        env = agentc_env_get("OPENAI_BASE_URL");
        slot = 2;
    }
    if (!env || !env[0])
        return NULL;
    size_t n = agentc_strlen(env);
    bool has_v1 = n >= 3 && agentc_streq(env + n - 3, "/v1");
    size_t need = has_v1 ? n + 1 : n + 4; /* optional "/v1" + NUL */
    if (g_env_base_cap[slot] < need) {
        agentc_free(g_env_base[slot]);
        g_env_base[slot] = agentc_alloc(need);
        g_env_base_cap[slot] = need;
    }
    agentc_snprintf(g_env_base[slot], need, has_v1 ? "%s" : "%s/v1", env);
    return g_env_base[slot];
}

/* --------------------------------------------------------- provider names */

/* The OpenAI-compatible presets are registered lazily by provider.c on the
 * first by-name lookup. The registry exposes the preset table itself
 * (agentc_provider_presets), so this seeds from that one source of truth and
 * never hardcodes a name or count. Everything after this seed (including
 * extension providers) comes from agentc_provider_all. */
static void seed_presets(const AgcConfig *cfg, const char *flag) {
    const AgcProviderOps *presets[32];
    size_t np = agentc_provider_presets(presets, sizeof presets / sizeof presets[0]);
    for (size_t i = 0; i < np; i++)
        (void)agentc_setup_provider_for(cfg, presets[i]->name, flag);
    /* a generic providers.<id> entry may name a preset too (or a provider the
     * config already knows); materialize it the same way. */
    if (cfg)
        for (size_t i = 0; i < cfg->nproviders; i++)
            (void)agentc_setup_provider_for(cfg, cfg->providers[i].id, flag);
}

void agentc_setup_seed_presets(const AgcConfig *cfg, const char *flag) {
    seed_presets(cfg, flag);
}

size_t agentc_setup_provider_names(const char **out, size_t max) {
    seed_presets(NULL, agentc_setup_cli_key());
    const AgcProviderOps *rows[128];
    size_t n = agentc_provider_all(rows, 128);
    const char *names[128];
    size_t total = 0;
    for (size_t i = 0; i < n && total < 128; i++) {
        const char *name = rows[i]->name;
        if (!name)
            continue;
        /* openai has two wire rows (chat + codex); a name list wants one. */
        bool dup = false;
        for (size_t k = 0; k < total && !dup; k++)
            if (agentc_streq(names[k], name))
                dup = true;
        if (!dup)
            names[total++] = name;
    }
    size_t wrote = 0;
    if (out)
        while (wrote < total && wrote < max) {
            out[wrote] = names[wrote];
            wrote++;
        }
    return out ? wrote : total;
}
