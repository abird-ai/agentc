/* setup.c — provider registry, model discovery orchestration and onboarding. */
#include "setup.h"
#include "discover.h"
#include "oauth.h"
#include "plat.h"
#include "wire.h"
#include "base/out.h"
#include "prov/provider.h"
#include "tui/pick.h"

#define DISCOVER_TTL_MS (24LL * 3600 * 1000)
#define DISCOVER_TIMEOUT_MS 2500
#define MODELS_MAX 256

/* ---------------------------------------------------------- provider registry */
/* The credential kind drives the provider choice. `openai` names two wire
 * backends: Chat Completions with an API key and the Codex Responses API with a
 * ChatGPT OAuth credential. Only an explicit --api-key flag wins over a stored
 * OAuth token; a provider env var or a config api_key does not reroute the wire
 * (see agentc_setup_resolve_key). The published context lets callers without the
 * CLI flag (the front-end agent rebuild) make the same choice; it owns copies,
 * never a config pointer, so it stays valid after the caller frees its config. */
static char *g_ctx_cli_key;          /* owned copy of --api-key, or NULL */
static char *g_ctx_cli_base;         /* owned copy of --base-url, or NULL */
static AgcConfigEntry *g_ctx_providers;   /* owned copy of config providers.<id> */
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
    if (!cfg || cfg->nproviders == 0) return;
    g_ctx_providers = agentc_alloc(cfg->nproviders * sizeof *g_ctx_providers);
    for (size_t i = 0; i < cfg->nproviders; i++) {
        const AgcConfigEntry *e = &cfg->providers[i];
        if (!e->id || !e->id[0] || !e->value || !e->value[0]) continue;
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

const char *agentc_setup_cli_key(void) { return g_ctx_cli_key; }
const char *agentc_setup_cli_base_url(void) { return g_ctx_cli_base; }

const char *agentc_setup_explicit_key(const AgcConfig *cfg, const char *name, const char *flag) {
    if (flag && flag[0]) return flag;
    const AgcProviderOps *ops = agentc_provider_by_name(name);
    if (ops) {
        for (size_t j = 0; j < 3 && ops->env_keys[j]; j++) {
            const char *v = agentc_env_get(ops->env_keys[j]);
            if (v && v[0]) return v;
        }
    }
    if (cfg) {
        const char *v = agentc_config_api_key(cfg, name);
        if (v && v[0]) return v;
    }
    return NULL;
}

/* Effective request credential in the documented order: explicit --api-key flag
 * > stored OAuth credential (fail-closed: a refresh failure is an error, never a
 * fallback) > provider env var > auth.jsonc > config api_keys. main.c and the
 * provider choice both go through this rule so they cannot disagree. */
const char *agentc_setup_resolve_key(const AgcConfig *cfg, const char *name, const char *flag) {
    if (flag && flag[0]) return flag;
    /* agentc_auth_key already implements OAuth > provider env > auth.jsonc and
     * returns NULL (without consulting anything else) when OAuth refresh fails. */
    const char *key = agentc_auth_key(name);
    if (key && key[0]) return key;
    if (agentc_oauth_logged_in(name)) return NULL;
    if (cfg) {
        const char *v = agentc_config_api_key(cfg, name);
        if (v && v[0]) return v;
    }
    return NULL;
}

/* Only an explicit --api-key flag overrides a stored OAuth credential: a
 * provider env var or a config api_key does not change the wire. */
static const AgcProvider *openai_choice(const char *flag) {
    if (flag && flag[0]) return agentc_prov_openai();
    if (agentc_oauth_logged_in("openai")) return agentc_prov_openai_codex();
    return agentc_prov_openai();
}

static const AgcProvider *provider_choice(const AgcConfig *cfg, const char *name,
                                          const char *flag) {
    if (!name) return NULL;
    if (agentc_streq(name, "openai")) return openai_choice(flag);
    /* by-name lookup materializes the OpenAI-compatible presets on demand */
    const AgcProviderOps *ops = agentc_provider_by_name(name);
    if (ops) return agentc_provider_handle((AgcProviderOps *)ops);
    /* An unknown id with a configured providers.<id>.base_url is a user gateway:
     * materialize an OpenAI-compatible row. Any other unknown id stays unknown. */
    if (cfg) {
        const char *base = agentc_config_base_url(cfg, name);
        if (base && base[0]) return agentc_prov_openai_compatible_gateway(name, base);
    }
    return NULL;
}

const AgcProvider *agentc_setup_provider_for(const AgcConfig *cfg, const char *name,
                                             const char *flag) {
    return provider_choice(cfg, name, flag);
}

const AgcProvider *agentc_setup_provider(const char *name) {
    if (!name) return NULL;
    if (agentc_streq(name, "openai")) return openai_choice(g_ctx_cli_key);
    const AgcProviderOps *ops = agentc_provider_by_name(name);
    if (ops) return agentc_provider_handle((AgcProviderOps *)ops);
    /* A config providers.<id>.base_url with no matching registry row is a user
     * gateway. Resolving it here (rather than at context publish) lets a loaded
     * extension register its own row first, so the config value is an override. */
    for (size_t i = 0; i < g_ctx_nproviders; i++)
        if (agentc_streq(g_ctx_providers[i].id, name))
            return agentc_prov_openai_compatible_gateway(name, g_ctx_providers[i].value);
    return NULL;
}

size_t agentc_model_filter(const char *provider, const char *api, const AgcModel **out,
                           size_t max) {
    const AgcModel *all[2048];
    size_t total = agentc_model_all(all, 1024);
    size_t k = 0;
    for (size_t i = 0; i < total; i++) {
        const AgcModel *m = all[i];
        if (provider && !agentc_streq(m->provider, provider)) continue;
        if (api && m->api && !agentc_streq(m->api, api)) continue;
        if (out) {
            if (k >= max) break;
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
    if (!name) return NULL;
    const char *v = (flag && flag[0]) ? flag : (cfg ? agentc_config_base_url(cfg, name) : NULL);
    if (v && v[0]) return v;
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
    if (!env || !env[0]) return NULL;
    size_t n = agentc_strlen(env);
    bool has_v1 = n >= 3 && agentc_streq(env + n - 3, "/v1");
    size_t need = has_v1 ? n + 1 : n + 4;   /* optional "/v1" + NUL */
    if (g_env_base_cap[slot] < need) {
        agentc_free(g_env_base[slot]);
        g_env_base[slot] = agentc_alloc(need);
        g_env_base_cap[slot] = need;
    }
    agentc_snprintf(g_env_base[slot], need, has_v1 ? "%s" : "%s/v1", env);
    return g_env_base[slot];
}

/* ------------------------------------------------------------- discovery */
size_t agentc_setup_discover(const AgcConfig *cfg, const char *name, bool live, bool offline,
                         bool force) {
    const AgcProvider *p = agentc_setup_provider_for(cfg, name, agentc_setup_cli_key());
    if (!p) return 0;
    /* the subscription backend has no model listing; the catalog stands in */
    const AgcProviderOps *ops = agentc_provider_ops(p);
    if (ops && ops->discover_style == AGENTC_DISCOVER_NONE) return 0;
    /* Drop this provider's previously discovered models first: if the endpoint
     * changed, the cache below is a miss and the old entries must not survive
     * as selectable models for the new base URL. */
    agentc_model_clear_dynamic(name);
    const char *base = agentc_setup_base_url(cfg, name, agentc_setup_cli_base_url());
    const char *eff = (base && base[0]) ? base : p->default_base_url;
    /* an explicit API key (or OAuth/env/auth.jsonc) must be used for discovery
     * too, so the key and the chosen wire agree. */
    const char *key = agentc_setup_resolve_key(cfg, name, agentc_setup_cli_key());

    AgcDiscovered *m = NULL;
    size_t n = agentc_discover_cache_load(name, eff, &m, MODELS_MAX, NULL);
    size_t known = n;
    if (n) {
        agentc_discover_register(name, p->api, eff, m, n);
        agentc_discover_free(m, n);
    }
    if (!live || offline) return n;
    if (agentc_setup_needs_key(name) && !(key && key[0])) return n;

    i64 fetched = 0;
    AgcDiscovered *cached = NULL;
    size_t cn = agentc_discover_cache_load(name, eff, &cached, MODELS_MAX, &fetched);
    agentc_discover_free(cached, cn);
    i64 now = os_now_ns(OS_CLOCK_REALTIME) / 1000000;
    if (!force && cn > 0 && now - fetched < DISCOVER_TTL_MS) return n;

    char err[160] = "";
    n = agentc_discover_models_ops(ops, eff, key, &m, MODELS_MAX, DISCOVER_TIMEOUT_MS, err,
                                   sizeof err);
    if (n == 0) {
        /* the cached models were already registered above: report them so the
         * caller can still auto-select instead of erroring "no model selected". */
        agentc_logf(0, "discovery: %s: %s", name, err[0] ? err : "unavailable");
        return known;
    }
    (void)agentc_discover_cache_save(name, eff, m, n);
    /* drop the cache-loaded set before installing the live one: a refresh must
     * not leave models that the endpoint no longer reports. */
    agentc_model_clear_dynamic(name);
    agentc_discover_register(name, p->api, eff, m, n);
    agentc_discover_free(m, n);
    return n;
}

/* ------------------------------------------------------------ list models */

/* The OpenAI-compatible presets are registered lazily by provider.c on the
 * first by-name lookup, and the registry has no preset-table accessor, so the
 * app seeds these names before it walks the live rows. Everything after this
 * seed (including extension providers) comes from agentc_provider_all. */
static const char *const g_preset_names[] = {
    "openrouter", "xai", "deepseek", "groq", "mistral", "together", "gemini",
};

static void seed_presets(const AgcConfig *cfg, const char *flag) {
    for (size_t i = 0; i < sizeof g_preset_names / sizeof g_preset_names[0]; i++)
        (void)agentc_setup_provider_for(cfg, g_preset_names[i], flag);
    /* a generic providers.<id> entry may name a preset too (or a provider the
     * config already knows); materialize it the same way. */
    if (cfg)
        for (size_t i = 0; i < cfg->nproviders; i++)
            (void)agentc_setup_provider_for(cfg, cfg->providers[i].id, flag);
}

size_t agentc_setup_provider_names(const char **out, size_t max) {
    seed_presets(NULL, agentc_setup_cli_key());
    const AgcProviderOps *rows[128];
    size_t n = agentc_provider_all(rows, 128);
    const char *names[128];
    size_t total = 0;
    for (size_t i = 0; i < n && total < 128; i++) {
        const char *name = rows[i]->name;
        if (!name) continue;
        /* openai has two wire rows (chat + codex); a name list wants one. */
        bool dup = false;
        for (size_t k = 0; k < total && !dup; k++)
            if (agentc_streq(names[k], name)) dup = true;
        if (!dup) names[total++] = name;
    }
    size_t wrote = 0;
    if (out)
        while (wrote < total && wrote < max) {
            out[wrote] = names[wrote];
            wrote++;
        }
    return out ? wrote : total;
}

int agentc_setup_list_models(const AgcConfig *cfg, const char *filter, bool refresh, bool offline) {
    seed_presets(cfg, agentc_setup_cli_key());

    /* A full pass starts from a clean discovered set: rows left by an earlier
     * single-provider refresh must not occupy the table and starve the
     * providers this pass registers. Static extension rows survive. */
    agentc_model_clear_dynamic(NULL);

    /* One entry per distinct provider name; the registry rows are the universe
     * (builtins, materialized presets, extension providers). A name with two
     * wire rows (openai chat/codex) resolves through the credential-kind
     * choice, and display filters on that one api, exactly as before. */
    const AgcProviderOps *rows[128];
    size_t nrows = agentc_provider_all(rows, 128);
    const char *names[128];
    const char *apis[128];
    bool usable[128];
    size_t n = 0;
    for (size_t i = 0; i < nrows; i++) {
        const char *name = rows[i]->name;
        if (!name) continue;
        bool dup = false;
        for (size_t k = 0; k < n && !dup; k++)
            if (agentc_streq(names[k], name)) dup = true;
        if (dup) continue;
        const AgcProvider *p = agentc_setup_provider_for(cfg, name, agentc_setup_cli_key());
        if (!p) continue;
        names[n] = name;
        apis[n] = p->api;
        const char *key = agentc_setup_resolve_key(cfg, name, agentc_setup_cli_key());
        /* a provider is listed when it needs no key (local Ollama) or has a
         * credential somewhere (env, auth.jsonc, config api_keys or OAuth) */
        usable[n] = !agentc_setup_needs_key(name) || (key && key[0]) || agentc_oauth_logged_in(name);
        if (usable[n]) agentc_setup_discover(cfg, name, true, offline, refresh);
        n++;
    }

    const AgcModel *all[2048];
    size_t total = agentc_model_all(all, 1024);
    size_t shown = 0;
    for (size_t i = 0; i < total; i++) {
        const AgcModel *m = all[i];
        bool prov_usable = false;
        for (size_t k = 0; k < n; k++) {
            if (!agentc_streq(m->provider, names[k])) continue;
            /* the `openai` name spans two apis: only show the one this run uses */
            if (m->api && apis[k] && !agentc_streq(m->api, apis[k])) continue;
            prov_usable = usable[k];
        }
        if (!prov_usable) continue;
        if (filter && filter[0]) {
            bool hit = agentc_str_str(m->provider, filter) != NULL ||
                       agentc_str_str(m->id, filter) != NULL;
            if (!hit) continue;
        }
        agentc_outf("%-14s %-40s ctx=%-8u max=%-7u %s%s%s\n", m->provider, m->id, m->ctx_window,
                m->max_tokens, m->reasoning ? "reasoning " : "", m->image ? "image " : "",
                agentc_model_is_static(m)
                    ? "static"
                    : (agentc_model_is_dynamic(m) ? "discovered" : ""));
        shown++;
    }
    agentc_outf("%u model(s)\n", (unsigned)shown);
    return 0;
}

/* ------------------------------------------------------------- onboarding */
/* One line-ending policy for the whole codebase (see tui/input.c): a CR ends
 * the line and the LF of a CRLF pair is swallowed; a lone LF ends it too. The
 * flag never blocks waiting to see whether an LF follows a CR, because on a
 * real console CR is the only terminator and lookahead would hang. */
static bool g_pending_cr;

/* Read one byte from stdin, waiting rather than failing when the descriptor is
 * non-blocking and momentarily empty (a real Windows console is an anonymous
 * pipe, and any pipe-fed run can starve). Returns 1, 0 on EOF, or negative
 * errno. */
static int setup_getc(u8 *out) {
    for (;;) {
        u8 c = 0;
        int r = os_read(0, &c, 1);
        if (r == -11 || r == -4) {                     /* EAGAIN / EINTR */
            struct os_pollfd pfd = { .fd = 0, .events = OS_POLLIN, .revents = 0 };
            int pr = os_poll(&pfd, 1, -1);
            if (pr < 0 && pr != -4) return pr;
            continue;                                 /* -4 here just waits again */
        }
        if (r <= 0) return r;
        *out = c;
        return 1;
    }
}

/* Cooked-mode line input (the terminal is in its normal state before the TUI
 * starts). Returns false on EOF/quit. */
static bool read_line(const char *prompt, char *buf, size_t cap) {
    agentc_out(prompt, agentc_strlen(prompt));
    size_t n = 0;
    for (;;) {
        u8 c = 0;
        int r = setup_getc(&c);
        if (r <= 0) return false;
        if (c == '\r') { g_pending_cr = true; break; }
        if (c == '\n') {
            if (g_pending_cr) { g_pending_cr = false; continue; }
            break;
        }
        g_pending_cr = false;
        if (c == 3 || c == 4) return false;              /* Ctrl+C / Ctrl+D */
        if ((c == 8 || c == 127) && n > 0) {             /* backspace */
            n--;
            agentc_out_raw("\b \b", 3);
            continue;
        }
        if (n + 1 < cap) {
            buf[n++] = (char)c;
            agentc_out_raw(&c, 1);
        }
    }
    buf[n] = 0;
    /* trailing blanks are noise for every caller (choice numbers, model ids) */
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
    agentc_out("\n", 1);
    return true;
}

/* Read a secret without echo. */
static bool read_secret(const char *prompt, char *buf, size_t cap) {
    agentc_out(prompt, agentc_strlen(prompt));
    void *saved = NULL;
    bool raw = os_tty_raw(0, &saved) == 0;
    /* The pending-CR flag is only meaningful while the input byte contract
     * holds: a console in cooked mode translates CR to NL, while a pipe/file
     * delivers its bytes as-is, so a flag set under one contract would swallow
     * the next line's '\n' under the other. Clear it only when fd 0 really is
     * the input terminal (os_tty_raw can succeed on Windows via a console
     * stdout while stdin is redirected, which must not count); a pipe/file
     * keeps the flag, which is what makes CRLF work there. */
    if (raw && os_tty_isatty(0)) g_pending_cr = false;
    size_t n = 0;
    for (;;) {
        u8 c = 0;
        int r = setup_getc(&c);
        if (r <= 0) break;
        if (c == '\r') { g_pending_cr = true; break; }
        if (c == '\n') {
            if (g_pending_cr) { g_pending_cr = false; continue; }
            break;
        }
        g_pending_cr = false;
        if (c == 3 || c == 4) break;
        if ((c == 8 || c == 127) && n > 0) { n--; continue; }
        if (n + 1 < cap) buf[n++] = (char)c;
    }
    buf[n] = 0;
    if (raw) {
        os_tty_restore(0, saved);
        if (os_tty_isatty(0)) g_pending_cr = false;
    }
    agentc_out("\n", 1);
    return n > 0;
}

/* Interactive picker for the setup flow. Returns the selected index, -1 when the
 * user cancels (Escape), or -2 when no terminal is available so the caller can
 * fall back to the line-based menu (scripts, pipes, tests). */
static int setup_pick(const char *title, const char *const *names,
                      const char *const *descs, size_t n, size_t initial) {
    int c = 0, r = 0;
    if (!names || n == 0 || os_tty_size(0, &c, &r) != 0) return -2;
    return agentc_tui_pick_tty(title, names, descs, n, initial);
}

/* One "which model?" question for a provider; the answer is written into
 * `model` (falls back to the first catalog/discovered entry). */
static void choose_model(const AgcConfig *cfg, const char *provider, bool offline, char *model,
                         size_t cap) {
    model[0] = 0;
    const AgcProvider *p = agentc_setup_provider_for(cfg, provider, agentc_setup_cli_key());
    const char *api = p ? p->api : NULL;
    size_t found = agentc_setup_discover(cfg, provider, true, offline, false);
    const AgcModel *list[32];
    size_t ln = agentc_model_filter(provider, api, list, 32);
    if (ln == 0) {
        agentc_outs("No models found. Type a model id (empty to skip): ");
        char line[128];
        if (read_line("", line, sizeof line) && line[0]) agentc_snprintf(model, cap, "%s", line);
        return;
    }
    if (found == 0) agentc_logf(0, "discovery: %s: using the built-in list", provider);
    const char *names[32];
    char descs[32][48];
    const char *dptr[32];
    for (size_t i = 0; i < ln && i < 32; i++) {
        names[i] = list[i]->id;
        agentc_snprintf(descs[i], sizeof descs[i], "%s%s",
                        list[i]->reasoning ? "reasoning" : "",
                        agentc_model_is_static(list[i])
                            ? (list[i]->reasoning ? "  static" : "static")
                            : (agentc_model_is_dynamic(list[i])
                                   ? (list[i]->reasoning ? "  discovered" : "discovered")
                                   : ""));
        dptr[i] = descs[i];
    }
    int pk = setup_pick("pick a model", names, dptr, ln, 0);
    if (pk >= 0 && (size_t)pk < ln) {
        agentc_snprintf(model, cap, "%s", list[pk]->id);
        return;
    }
    if (pk == -1) return;   /* cancelled: the caller sees an empty model */
    /* No terminal: the numbered / free-text fallback. */
    for (size_t i = 0; i < ln; i++)
        agentc_outf("  %u) %s%s%s\n", (unsigned)(i + 1), list[i]->id,
                list[i]->reasoning ? "  (reasoning)" : "",
                agentc_model_is_static(list[i])
                    ? "  (static)"
                    : (agentc_model_is_dynamic(list[i]) ? "  (discovered)" : ""));
    char line[64];
    if (!read_line("Model [1]: ", line, sizeof line)) return;
    if (!line[0]) {
        agentc_snprintf(model, cap, "%s", list[0]->id);
        return;
    }
    i64 idx = 0;
    bool ok = false;
    idx = agentc_parse_i64(line, agentc_strlen(line), &ok);
    if (ok && idx >= 1 && (size_t)idx <= ln) agentc_snprintf(model, cap, "%s", list[idx - 1]->id);
    else agentc_snprintf(model, cap, "%s", line);
}

static int probe_ollama(void) {
    AgcHttp *h = agentc_http_new("GET", "http://127.0.0.1:11434/api/tags", NULL, NULL, 0);
    if (!h) return -1;
    int rc = agentc_http_run(h, NULL, NULL, 500);
    int status = agentc_http_status(h);
    agentc_http_free(h);
    return (rc == 0 && status == 200) ? 0 : -1;
}

int agentc_setup_onboard(AgcConfig **cfgp, const char *base_url_flag, bool offline) {
    (void)base_url_flag;
    AgcConfig *cfg = *cfgp;
    bool ollama_up = !offline && probe_ollama() == 0;
    char prov0[80];
    agentc_snprintf(prov0, sizeof prov0, "Ollama (local%s)",
                    ollama_up ? ", detected on 127.0.0.1:11434" : "");
    static const char *pnames[8] = {
        "Ollama (local)", "Ollama Cloud", "Anthropic", "OpenAI",
        "Other OpenAI-compatible", "Google Gemini", "Sync OpenRouter pricing", "Quit",
    };
    const char *pdescs[8] = {
        ollama_up ? "detected on 127.0.0.1:11434" : "no API key needed",
        "API key from ollama.com",
        "Claude subscription login or API key",
        "ChatGPT subscription login or API key",
        "OpenRouter, xAI, DeepSeek, Groq, ...",
        "native API, GEMINI_API_KEY",
        "fetch pricing.jsonc",
        "cancel setup",
    };
    char line[128];
    int choice = 0;
    int pk = setup_pick("agentc first run - pick a model provider", pnames, pdescs, 8, 0);
    if (pk == -2) {
        agentc_outs("\nagentc first run - pick a model provider\n\n");
        agentc_outf("  1) %s\n", prov0);
        agentc_outs("  2) Ollama Cloud (API key from ollama.com)\n");
        agentc_outs("  3) Anthropic (Claude subscription login or API key)\n");
        agentc_outs("  4) OpenAI (ChatGPT subscription login or API key)\n");
        agentc_outs("  5) Other OpenAI-compatible provider (OpenRouter, xAI, DeepSeek, Groq, ...)\n");
        agentc_outs("  6) Google Gemini (native API, GEMINI_API_KEY)\n");
        agentc_outs("  7) Sync model pricing from OpenRouter\n");
        agentc_outs("  q) quit\n\n");
        if (!read_line("Choice [1]: ", line, sizeof line)) return 1;
        choice = line[0] ? (int)agentc_parse_i64(line, agentc_strlen(line), NULL) : 1;
    } else if (pk < 0) {
        agentc_outs("setup cancelled\n");
        return 1;
    } else {
        if (pk == 7) {
            agentc_outs("setup cancelled\n");
            return 1;
        }
        choice = pk + 1;
    }

    char provider[64] = "";
    char model[160] = "";
    char key[512] = "";

    if (choice == 1) {
        agentc_snprintf(provider, sizeof provider, "ollama");
        choose_model(cfg, provider, offline, model, sizeof model);
        if (!model[0]) {
            agentc_outs("no local models found - start one with 'ollama pull llama3.2'\n");
            return 1;
        }
    } else if (choice == 2) {
        agentc_snprintf(provider, sizeof provider, "ollama-cloud");
        if (!read_secret("Ollama Cloud API key: ", key, sizeof key)) return 1;
        if (agentc_auth_set_key(provider, key) != 0) {
            agentc_logf(3, "cannot write auth.jsonc");
            return 1;
        }
        choose_model(cfg, provider, offline, model, sizeof model);
    } else if (choice == 3 || choice == 4) {
        bool anthropic = choice == 3;
        agentc_snprintf(provider, sizeof provider, "%s", anthropic ? "anthropic" : "openai");
        agentc_outs("  [l] subscription login (browser or paste)   [k] API key\n");
        const char *mnames[2] = { "Subscription login", "API key" };
        const char *mdescs[2] = { "browser or paste", "paste an API key" };
        int mk = setup_pick("credentials", mnames, mdescs, 2, 1);
        bool login;
        if (mk == -2) {
            if (!read_line("Method [k]: ", line, sizeof line)) return 1;
            login = (line[0] == 'l' || line[0] == 'L');
        } else if (mk < 0) {
            return 1;
        } else {
            login = (mk == 0);
        }
        if (login) {
            if (agentc_oauth_login(provider) != 0) {
                const char *err = agentc_oauth_last_error();
                agentc_logf(3, "login: %s", err && err[0] ? err : "failed");
                return 1;
            }
        } else {
            if (!read_secret(anthropic ? "Anthropic API key: " : "OpenAI API key: ", key,
                             sizeof key))
                return 1;
            if (agentc_auth_set_key(provider, key) != 0) {
                agentc_logf(3, "cannot write auth.jsonc");
                return 1;
            }
        }
        choose_model(cfg, provider, offline, model, sizeof model);
    } else if (choice == 5) {
        const size_t npreset = sizeof g_preset_names / sizeof g_preset_names[0];
        /* Loaded extension providers follow the presets. Enumerating rows is
         * non-blocking (no discovery, no network) and the menu is bounded. */
        const AgcProviderOps *ext[16];
        size_t next = 0;
        const AgcProviderOps *rows[128];
        size_t nrows = agentc_provider_all(rows, 128);
        for (size_t i = 0; i < nrows && next < sizeof ext / sizeof ext[0]; i++)
            if (rows[i]->is_ext && rows[i]->name) ext[next++] = rows[i];
        const char *snames[64];
        const char *sdescs[64];
        size_t nn = 0;
        for (size_t i = 0; i < npreset && nn < 64; i++) {
            snames[nn] = g_preset_names[i];
            sdescs[nn] = "OpenAI-compatible";
            nn++;
        }
        for (size_t i = 0; i < next && nn < 64; i++) {
            snames[nn] = ext[i]->name;
            sdescs[nn] = "extension";
            nn++;
        }
        int p = 0;
        int pp = setup_pick("provider", snames, sdescs, nn, 0);
        if (pp == -2) {
            for (size_t i = 0; i < npreset; i++)
                agentc_outf("  %u) %s\n", (unsigned)(i + 1), g_preset_names[i]);
            for (size_t i = 0; i < next; i++)
                agentc_outf("  %u) %s (extension)\n", (unsigned)(npreset + i + 1), ext[i]->name);
            if (!read_line("Provider [1]: ", line, sizeof line)) return 1;
            p = line[0] ? (int)agentc_parse_i64(line, agentc_strlen(line), NULL) : 1;
        } else if (pp < 0) {
            return 1;
        } else {
            p = pp + 1;
        }
        if (p < 1 || (size_t)p > npreset + next) return 1;
        if ((size_t)p <= npreset) {
            agentc_snprintf(provider, sizeof provider, "%s", g_preset_names[p - 1]);
            if (!read_secret("API key: ", key, sizeof key)) return 1;
            if (agentc_auth_set_key(provider, key) != 0) {
                agentc_logf(3, "cannot write auth.jsonc");
                return 1;
            }
        } else {
            const AgcProviderOps *row = ext[(size_t)p - npreset - 1];
            agentc_snprintf(provider, sizeof provider, "%s", row->name);
            if (row->needs_key) {
                if (!read_secret("API key: ", key, sizeof key)) return 1;
                if (agentc_auth_set_key(provider, key) != 0) {
                    agentc_logf(3, "cannot write auth.jsonc");
                    return 1;
                }
            }
        }
        choose_model(cfg, provider, offline, model, sizeof model);
    } else if (choice == 6) {
        agentc_snprintf(provider, sizeof provider, "google");
        if (!read_secret("Google Gemini API key: ", key, sizeof key)) return 1;
        if (agentc_auth_set_key(provider, key) != 0) {
            agentc_logf(3, "cannot write auth.jsonc");
            return 1;
        }
        choose_model(cfg, provider, offline, model, sizeof model);
    } else if (choice == 7) {
        char err[256] = "";
        int n = agentc_pricing_sync_openrouter(err, sizeof err);
        if (n < 0) {
            agentc_logf(3, "pricing sync: %s", err[0] ? err : "failed");
            return 1;
        }
        agentc_outf("pricing: synced %d model(s) from OpenRouter\n", n);
        return 0;
    } else {
        agentc_outs("setup cancelled\n");
        return 1;
    }

    int rc = agentc_config_save_setup(provider, model);
    if (rc != 0) {
        agentc_logf(3, "cannot write the setup file (errno %d)", rc);
        return 1;
    }
    char sp[4200];
    agentc_outs("\n");
    if (provider[0]) {
        agentc_outf("provider: %s", provider);
        if (model[0]) agentc_outf("   model: %s", model);
        agentc_outs("\n");
    }
    if (agentc_config_setup_path(sp, sizeof sp)) agentc_outf("saved to %s\n", sp);
    agentc_outs("change it any time with `agentc setup` or by editing the file.\n\n");

    /* reload the config so the caller sees the new defaults */
    agentc_config_free(cfg);
    cfg = agentc_config_load(NULL);
    *cfgp = cfg;
    return 0;
}
