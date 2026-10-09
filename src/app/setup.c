/* setup.c — app-side model discovery orchestration and first-run onboarding.
 *
 * Provider selection, credential resolution and model filtering live in
 * src/core/select.c (Axis B4); this file orchestrates the live discovery
 * requests and the interactive onboarding flow that only the app uses. */
#include "setup.h"
#include "discover.h"
#include "oauth.h"
#include "plat.h"
#include "wire.h"
#include "base/out.h"
#include "prov/provider.h"
#ifndef AGENTC_SDK_NO_TUI
#include "tui/pick.h"
#endif

#define DISCOVER_TTL_MS (24LL * 3600 * 1000)
#define DISCOVER_TIMEOUT_MS 2500
#define MODELS_MAX 256

/* ------------------------------------------------------------- discovery */
size_t agentc_setup_discover(const AgcConfig *cfg, const char *name, bool live, bool offline,
                             bool force) {
    const AgcProvider *p = agentc_setup_provider_for(cfg, name, agentc_setup_cli_key());
    if (!p)
        return 0;
    /* the subscription backend has no model listing; the catalog stands in */
    const AgcProviderOps *ops = agentc_provider_ops(p);
    if (ops && ops->discover_style == AGENTC_DISCOVER_NONE)
        return 0;
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
    if (!live || offline)
        return n;
    if (agentc_setup_needs_key(name) && !(key && key[0]))
        return n;

    i64 fetched = 0;
    AgcDiscovered *cached = NULL;
    size_t cn = agentc_discover_cache_load(name, eff, &cached, MODELS_MAX, &fetched);
    agentc_discover_free(cached, cn);
    i64 now = os_now_ns(OS_CLOCK_REALTIME) / 1000000;
    if (!force && cn > 0 && now - fetched < DISCOVER_TTL_MS)
        return n;

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

int agentc_setup_list_models(const AgcConfig *cfg, const char *filter, bool refresh, bool offline) {
    agentc_setup_seed_presets(cfg, agentc_setup_cli_key());

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
        if (!name)
            continue;
        bool dup = false;
        for (size_t k = 0; k < n && !dup; k++)
            if (agentc_streq(names[k], name))
                dup = true;
        if (dup)
            continue;
        const AgcProvider *p = agentc_setup_provider_for(cfg, name, agentc_setup_cli_key());
        if (!p)
            continue;
        names[n] = name;
        apis[n] = p->api;
        const char *key = agentc_setup_resolve_key(cfg, name, agentc_setup_cli_key());
        /* a provider is listed when it needs no key (local Ollama) or has a
         * credential somewhere (env, auth.jsonc, config api_keys or OAuth) */
        usable[n] =
            !agentc_setup_needs_key(name) || (key && key[0]) || agentc_oauth_logged_in(name);
        if (usable[n])
            agentc_setup_discover(cfg, name, true, offline, refresh);
        n++;
    }

    const AgcModel *all[2048];
    size_t total = agentc_model_all(all, 1024);
    size_t shown = 0;
    for (size_t i = 0; i < total; i++) {
        const AgcModel *m = all[i];
        if (m->flags & AGENTC_MODEL_VIRTUAL) {
            /* B-M5a (P8-D23): a config-declared alias is a selection target
             * like any model row; the CLI list and get_available_models must
             * agree. Aliases are listed regardless of target-provider
             * usability; an unusable target fails closed at resolve time. */
            if (filter && filter[0]) {
                bool hit = agentc_str_str(m->id, filter) != NULL ||
                           (m->route && agentc_str_str(m->route, filter) != NULL);
                if (!hit)
                    continue;
            }
            agentc_outf("%-14s %-40s ctx=%-8u max=%-7u alias -> %s\n", m->provider, m->id,
                        m->ctx_window, m->max_tokens, m->route ? m->route : "?");
            shown++;
            continue;
        }
        bool prov_usable = false;
        for (size_t k = 0; k < n; k++) {
            if (!agentc_streq(m->provider, names[k]))
                continue;
            /* the `openai` name spans two apis: only show the one this run uses */
            if (m->api && apis[k] && !agentc_streq(m->api, apis[k]))
                continue;
            prov_usable = usable[k];
        }
        if (!prov_usable)
            continue;
        if (filter && filter[0]) {
            bool hit = agentc_str_str(m->provider, filter) != NULL ||
                       agentc_str_str(m->id, filter) != NULL;
            if (!hit)
                continue;
        }
        agentc_outf("%-14s %-40s ctx=%-8u max=%-7u %s%s%s\n", m->provider, m->id, m->ctx_window,
                    m->max_tokens, m->reasoning ? "reasoning " : "", m->image ? "image " : "",
                    agentc_model_is_static(m) ? "static"
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
        if (r == -11 || r == -4) { /* EAGAIN / EINTR */
            struct os_pollfd pfd = {.fd = 0, .events = OS_POLLIN, .revents = 0};
            int pr = os_poll(&pfd, 1, -1);
            if (pr < 0 && pr != -4)
                return pr;
            continue; /* -4 here just waits again */
        }
        if (r <= 0)
            return r;
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
        if (r <= 0)
            return false;
        if (c == '\r') {
            g_pending_cr = true;
            break;
        }
        if (c == '\n') {
            if (g_pending_cr) {
                g_pending_cr = false;
                continue;
            }
            break;
        }
        g_pending_cr = false;
        if (c == 3 || c == 4)
            return false;                    /* Ctrl+C / Ctrl+D */
        if ((c == 8 || c == 127) && n > 0) { /* backspace */
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
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t'))
        buf[--n] = 0;
    agentc_out("\n", 1);
    return true;
}

/* Read a secret without echo. `required` rejects an empty line; otherwise an
 * empty line yields an empty buffer (an optional credential). Returns false on
 * EOF/quit, or when `required` and nothing was typed. */
static bool read_secret_line(const char *prompt, char *buf, size_t cap, bool required) {
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
    if (raw && os_tty_isatty(0))
        g_pending_cr = false;
    bool eof = false;
    size_t n = 0;
    for (;;) {
        u8 c = 0;
        int r = setup_getc(&c);
        if (r <= 0) {
            eof = true;
            break;
        }
        if (c == '\r') {
            g_pending_cr = true;
            break;
        }
        if (c == '\n') {
            if (g_pending_cr) {
                g_pending_cr = false;
                continue;
            }
            break;
        }
        g_pending_cr = false;
        if (c == 3 || c == 4) {
            eof = true;
            break;
        }
        if ((c == 8 || c == 127) && n > 0) {
            n--;
            continue;
        }
        if (n + 1 < cap)
            buf[n++] = (char)c;
    }
    buf[n] = 0;
    if (raw) {
        os_tty_restore(0, saved);
        if (os_tty_isatty(0))
            g_pending_cr = false;
    }
    agentc_out("\n", 1);
    return required ? (n > 0) : (n > 0 || !eof);
}

static bool read_secret(const char *prompt, char *buf, size_t cap) {
    return read_secret_line(prompt, buf, cap, true);
}

/* Interactive picker for the setup flow. Returns the selected index, -1 when the
 * user cancels (Escape), or -2 when no terminal is available so the caller can
 * fall back to the line-based menu (scripts, pipes, tests). */
static int setup_pick(const char *title, const char *const *names, const char *const *descs,
                      size_t n, size_t initial) {
    int c = 0, r = 0;
    if (!names || n == 0 || os_tty_size(0, &c, &r) != 0)
        return -2;
#ifdef AGENTC_SDK_NO_TUI
    /* The embeddable archive has no terminal UI: fail closed (the caller falls
     * back to the line-based menu or the host drives onboarding itself). */
    (void)title;
    (void)descs;
    (void)initial;
    return -2;
#else
    return agentc_tui_pick_tty(title, names, descs, n, initial);
#endif
}

/* One "which model?" question for a provider; the answer is written into
 * `model` (falls back to the first catalog/discovered entry). */
static void choose_model(const AgcConfig *cfg, const char *provider, bool offline, char *model,
                         size_t cap) {
    model[0] = 0;
    const AgcProvider *p = agentc_setup_provider_for(cfg, provider, agentc_setup_cli_key());
    const char *api = p ? p->api : NULL;
    size_t found = agentc_setup_discover(cfg, provider, true, offline, false);
    /* B-M5c (P8-D23): config-declared aliases are selection targets like
     * concrete models. Offer them first (labelled like the TUI and the
     * --list-models output), then this provider's catalog/discovered rows, so
     * --list-models, RPC, the setup picker and /model agree. */
    const AgcModel *list[64];
    size_t ln = 0;
    {
        const AgcModel *all[2048];
        size_t an = agentc_model_all(all, 1024);
        for (size_t i = 0; i < an && ln < 64; i++)
            if (all[i]->flags & AGENTC_MODEL_VIRTUAL)
                list[ln++] = all[i];
    }
    const AgcModel *conc[64];
    size_t cn = agentc_model_filter(provider, api, conc, 64);
    for (size_t i = 0; i < cn && ln < 64; i++)
        list[ln++] = conc[i];
    if (ln == 0) {
        agentc_outs("No models found. Type a model id (empty to skip): ");
        char line[128];
        if (read_line("", line, sizeof line) && line[0])
            agentc_snprintf(model, cap, "%s", line);
        return;
    }
    if (found == 0 && cn > 0)
        agentc_logf(0, "discovery: %s: using the built-in list", provider);
    const char *names[64];
    char descs[64][72];
    const char *dptr[64];
    for (size_t i = 0; i < ln; i++) {
        names[i] = list[i]->id;
        if (list[i]->flags & AGENTC_MODEL_VIRTUAL)
            agentc_snprintf(descs[i], sizeof descs[i], "alias -> %s",
                            list[i]->route ? list[i]->route : "?");
        else
            agentc_snprintf(descs[i], sizeof descs[i], "%s%s", list[i]->reasoning ? "reasoning" : "",
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
    if (pk == -1)
        return; /* cancelled: the caller sees an empty model */
    /* No terminal: the numbered / free-text fallback. */
    for (size_t i = 0; i < ln; i++) {
        if (list[i]->flags & AGENTC_MODEL_VIRTUAL)
            agentc_outf("  %u) %s  alias -> %s\n", (unsigned)(i + 1), list[i]->id,
                        list[i]->route ? list[i]->route : "?");
        else
            agentc_outf("  %u) %s%s%s\n", (unsigned)(i + 1), list[i]->id,
                        list[i]->reasoning ? "  (reasoning)" : "",
                        agentc_model_is_static(list[i])
                            ? "  (static)"
                            : (agentc_model_is_dynamic(list[i]) ? "  (discovered)" : ""));
    }
    char line[64];
    if (!read_line("Model [1]: ", line, sizeof line))
        return;
    if (!line[0]) {
        agentc_snprintf(model, cap, "%s", list[0]->id);
        return;
    }
    i64 idx = 0;
    bool ok = false;
    idx = agentc_parse_i64(line, agentc_strlen(line), &ok);
    if (ok && idx >= 1 && (size_t)idx <= ln)
        agentc_snprintf(model, cap, "%s", list[idx - 1]->id);
    else
        agentc_snprintf(model, cap, "%s", line);
}

static int probe_ollama(void) {
    AgcHttp *h = agentc_http_new("GET", "http://127.0.0.1:11434/api/tags", NULL, NULL, 0);
    if (!h)
        return -1;
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
    static const char *pnames[9] = {
        "Ollama (local)",
        "Ollama Cloud",
        "Anthropic",
        "OpenAI",
        "Other OpenAI-compatible",
        "Google Gemini",
        "Custom endpoint (OpenAI-compatible; use /v1 for Ollama)",
        "Sync OpenRouter pricing",
        "Quit",
    };
    const char *pdescs[9] = {
        ollama_up ? "detected on 127.0.0.1:11434" : "no API key needed",
        "API key from ollama.com",
        "Claude subscription login or API key",
        "ChatGPT subscription login or API key",
        "OpenRouter, xAI, DeepSeek, Groq, ...",
        "native API, GEMINI_API_KEY",
        "name + base URL (and optional key)",
        "fetch pricing.jsonc",
        "cancel setup",
    };
    char line[128];
    int choice = 0;
    int pk = setup_pick("agentc first run - pick a model provider", pnames, pdescs, 9, 0);
    if (pk == -2) {
        agentc_outs("\nagentc first run - pick a model provider\n\n");
        agentc_outf("  1) %s\n", prov0);
        agentc_outs("  2) Ollama Cloud (API key from ollama.com)\n");
        agentc_outs("  3) Anthropic (Claude subscription login or API key)\n");
        agentc_outs("  4) OpenAI (ChatGPT subscription login or API key)\n");
        agentc_outs(
            "  5) Other OpenAI-compatible provider (OpenRouter, xAI, DeepSeek, Groq, ...)\n");
        agentc_outs("  6) Google Gemini (native API, GEMINI_API_KEY)\n");
        agentc_outs("  7) Custom endpoint (OpenAI-compatible; use /v1 for Ollama)\n");
        agentc_outs("  8) Sync model pricing from OpenRouter\n");
        agentc_outs("  q) quit\n\n");
        if (!read_line("Choice [1]: ", line, sizeof line))
            return 1;
        choice = line[0] ? (int)agentc_parse_i64(line, agentc_strlen(line), NULL) : 1;
    } else if (pk < 0) {
        agentc_outs("setup cancelled\n");
        return 1;
    } else {
        if (pk == 8) {
            agentc_outs("setup cancelled\n");
            return 1;
        }
        choice = pk + 1;
    }

    char provider[AGENTC_CONFIG_GENERIC_NAME_MAX + 1] = "";
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
        if (!read_secret("Ollama Cloud API key: ", key, sizeof key))
            return 1;
        if (agentc_auth_set_key(provider, key) != 0) {
            agentc_logf(3, "cannot write auth.jsonc");
            return 1;
        }
        choose_model(cfg, provider, offline, model, sizeof model);
    } else if (choice == 3 || choice == 4) {
        bool anthropic = choice == 3;
        agentc_snprintf(provider, sizeof provider, "%s", anthropic ? "anthropic" : "openai");
        agentc_outs("  [l] subscription login (browser or paste)   [k] API key\n");
        const char *mnames[2] = {"Subscription login", "API key"};
        const char *mdescs[2] = {"browser or paste", "paste an API key"};
        int mk = setup_pick("credentials", mnames, mdescs, 2, 1);
        bool login;
        if (mk == -2) {
            if (!read_line("Method [k]: ", line, sizeof line))
                return 1;
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
        /* The preset menu is the registry's preset table (the single source of
         * truth, also used by --list-models and the --provider help); loaded
         * extension providers follow. Enumerating rows is non-blocking (no
         * discovery, no network) and the menu is bounded. */
        const AgcProviderOps *preset[16];
        size_t npreset = agentc_provider_presets(preset, sizeof preset / sizeof preset[0]);
        const AgcProviderOps *ext[16];
        size_t next = 0;
        const AgcProviderOps *rows[128];
        size_t nrows = agentc_provider_all(rows, 128);
        for (size_t i = 0; i < nrows && next < sizeof ext / sizeof ext[0]; i++)
            if (rows[i]->is_ext && rows[i]->name)
                ext[next++] = rows[i];
        const char *snames[64];
        const char *sdescs[64];
        size_t nn = 0;
        for (size_t i = 0; i < npreset && nn < 64; i++) {
            snames[nn] = preset[i]->name;
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
                agentc_outf("  %u) %s\n", (unsigned)(i + 1), preset[i]->name);
            for (size_t i = 0; i < next; i++)
                agentc_outf("  %u) %s (extension)\n", (unsigned)(npreset + i + 1), ext[i]->name);
            if (!read_line("Provider [1]: ", line, sizeof line))
                return 1;
            p = line[0] ? (int)agentc_parse_i64(line, agentc_strlen(line), NULL) : 1;
        } else if (pp < 0) {
            return 1;
        } else {
            p = pp + 1;
        }
        if (p < 1 || (size_t)p > npreset + next)
            return 1;
        if ((size_t)p <= npreset) {
            agentc_snprintf(provider, sizeof provider, "%s", preset[p - 1]->name);
            if (!read_secret("API key: ", key, sizeof key))
                return 1;
            if (agentc_auth_set_key(provider, key) != 0) {
                agentc_logf(3, "cannot write auth.jsonc");
                return 1;
            }
        } else {
            const AgcProviderOps *row = ext[(size_t)p - npreset - 1];
            agentc_snprintf(provider, sizeof provider, "%s", row->name);
            if (row->needs_key) {
                if (!read_secret("API key: ", key, sizeof key))
                    return 1;
                if (agentc_auth_set_key(provider, key) != 0) {
                    agentc_logf(3, "cannot write auth.jsonc");
                    return 1;
                }
            }
        }
        choose_model(cfg, provider, offline, model, sizeof model);
    } else if (choice == 6) {
        agentc_snprintf(provider, sizeof provider, "google");
        if (!read_secret("Google Gemini API key: ", key, sizeof key))
            return 1;
        if (agentc_auth_set_key(provider, key) != 0) {
            agentc_logf(3, "cannot write auth.jsonc");
            return 1;
        }
        choose_model(cfg, provider, offline, model, sizeof model);
    } else if (choice == 7) {
        /* Custom OpenAI-compatible endpoint (for Ollama use its /v1 URL): a
         * provider id plus a base URL, persisted to setup.jsonc as
         * providers.<id>.base_url. The key is optional (a local gateway may need
         * none). */
        /* B-L11: read into a provider-sized buffer and reprompt on an over-long
         * name, so the writer's -EINVAL guard is reachable and the intended id
         * is preserved instead of silently truncated. */
        for (;;) {
            if (!read_line("Provider name (e.g. my-gateway): ", line, sizeof line) || !line[0])
                return 1;
            if (agentc_strlen(line) <= AGENTC_CONFIG_GENERIC_NAME_MAX)
                break;
            agentc_outf("provider name is too long (max %d characters)\n",
                        AGENTC_CONFIG_GENERIC_NAME_MAX);
        }
        agentc_snprintf(provider, sizeof provider, "%s", line);
        char custom_base[512];
        if (!read_line("Base URL (e.g. https://host.example/v1): ", custom_base,
                       sizeof custom_base) ||
            !custom_base[0])
            return 1;
        if (!read_secret_line("API key (empty for none): ", key, sizeof key, false))
            return 1;
        if (key[0] && agentc_auth_set_key(provider, key) != 0) {
            agentc_logf(3, "cannot write auth.jsonc");
            return 1;
        }
        if (agentc_config_setup_set_provider(provider, custom_base) != 0) {
            agentc_logf(3, "cannot write setup.jsonc");
            return 1;
        }
        /* Reload so the new providers.<id>.base_url reaches discovery. */
        agentc_config_free(cfg);
        cfg = agentc_config_load(NULL);
        *cfgp = cfg;
        choose_model(cfg, provider, offline, model, sizeof model);
    } else if (choice == 8) {
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
        if (model[0])
            agentc_outf("   model: %s", model);
        agentc_outs("\n");
    }
    if (agentc_config_setup_path(sp, sizeof sp))
        agentc_outf("saved to %s\n", sp);
    agentc_outs("change it any time with `agentc setup` or by editing the file.\n\n");

    /* reload the config so the caller sees the new defaults */
    agentc_config_free(cfg);
    cfg = agentc_config_load(NULL);
    *cfgp = cfg;
    return 0;
}
