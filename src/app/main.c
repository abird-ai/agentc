/* main.c — entry point, flags, session wiring and print mode.
 *
 * Print mode runs one prompt and streams assistant text to stdout, tool
 * activity to stderr; exit code 0 on success, 1 on failure. When a session is
 * active (the default) every transcript message produced by the run is
 * appended to the JSONL file, so --continue can replay it.
 */
#include "agent.h"
#include "config.h"
#include "discover.h"
#include "mcp.h"
#include "ext.h"
#include "setup.h"
#include "session.h"
#include "oauth.h"
#include "tui/tui.h"
#include "plat.h"
#include "prov/provider.h"
#include "app/mode.h"
#include "app/policy.h"
#include "app/project.h"
#include "core/prompt.h"
#include "core/prompts.h"
#include "core/tools/engine.h"

static int stdio_read(void *ud, void *buf, size_t cap) {
    int fd = (int)(size_t)ud;
    struct os_pollfd p = { fd, OS_POLLIN, 0 };
    int r = os_poll(&p, 1, 0);
    if (r < 0) return r;
    if (r == 0) return -11;                   /* would block */
    return os_read(fd, buf, cap);
}

static void stdio_write(void *ud, const void *buf, size_t n) {
    int fd = (int)(size_t)ud;
    const u8 *p = buf;
    while (n) {
        int w = os_write(fd, p, n);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}

/* internal helper (not part of the frozen headers) */

static const char USAGE[] =
    "agentc - a minimal coding agent\n"
            "\n"
            "usage: agentc [options]\n"
            "\n"
            "  Without -p or --mode the interactive TUI starts in this terminal.\n"
            "\n"
            "       agentc setup [--offline]  first-run setup (provider, credentials, model)\n"
            "       agentc login [provider] [--manual]\n"
            "                                 sign in with a subscription (anthropic|openai);\n"
            "                                 --manual prints the URL and reads the code from stdin\n"
            "       agentc logout [provider]  remove the stored credential for a provider\n"
            "  -p, --print PROMPT      run one prompt and print the answer\n"
            "      --mode MODE         json (event JSONL) or rpc (JSONL commands)\n"
            "      --tui-mode MODE     scrollback (append-only), inline (default, owned\n"
            "                          bottom region), fullscreen (alternate screen) or\n"
            "                          auto (inline)\n"
            "      --provider P        openai (default), anthropic, google (native Gemini),\n"
            "                          ollama, ollama-cloud, an OpenAI-compatible preset\n"
            "                          (openrouter, xai, deepseek, groq, mistral, together,\n"
            "                          gemini), or a provider contributed by a loaded\n"
            "                          extension\n"
            "      --model M           model id (provider default when omitted)\n"
            "      --api-key K         API key (env/auth.jsonc fallback)\n"
            "      --base-url U        override the API base URL\n"
            "      --system S          override the system prompt\n"
            "      --thinking LEVEL    off|low|medium|high (default off)\n"
            "      --max-tokens N      maximum output tokens\n"
            "      --tools a,b         enable only the listed tools (builtin, extension or MCP)\n"
            "      --no-tools          disable all tools\n"
            "      --tools-engine MODE external (default: prefer ripgrep/fd/grep/find on PATH,\n"
            "                          fall back to the built-ins) or internal (force built-ins)\n"
            "      --no-mcp            do not connect MCP servers\n"
            "      --continue          resume the newest session for this cwd\n"
            "      --resume            like --continue (picker arrives with the TUI)\n"
            "      --session PATH|ID   open a session file or id\n"
            "      --session-dir DIR   store sessions in DIR\n"
            "      --no-session        run without writing a session\n"
            "      --approve           trust the project for this run\n"
            "      --no-approve        do not trust the project for this run\n"
            "      --list-sessions     list stored sessions (newest first)\n"
            "      --list-models [S]   list known models (optionally filtered)\n"
            "      --list-extensions   list registered extensions and their state\n"
            "      --refresh-models    refresh the provider model list (ignores the cache)\n"
            "      --sync-pricing      fetch OpenRouter pricing into pricing.jsonc\n"
            "      --offline           never make discovery requests\n"
            "      --record FILE       save raw wire bytes\n"
            "      --dump-wire         dump raw wire bytes to stderr\n"
            "      --insecure          skip TLS verification\n"
            "\n"
            "  MCP/extension tools without a readOnlyHint are treated as destructive and\n"
            "  must be named in --tools/default_tools.\n"
            "  -v, --version           print the version\n"
            "  -h, --help              show this help\n";

static void usage_fd(int fd) {
    (void)os_write(fd, USAGE, agentc_strlen(USAGE));
}

static void usage(void) { usage_fd(1); }


static bool file_exists(const char *path) {
    int fd = os_open(path, OS_O_RDONLY, 0);
    if (fd < 0) return false;
    os_close(fd);
    return true;
}

static const char *need_arg(int argc, char **argv, int *i, const char *flag) {
    if (*i + 1 >= argc) {
        agentc_logf(3, "missing argument for %s", flag);
        usage_fd(2);
        return NULL;
    }
    return argv[++*i];
}

static int parse_thinking(const char *v, bool *ok) {
    *ok = true;
    if (agentc_streq(v, "off")) return 0;
    if (agentc_streq(v, "low")) return 1;
    if (agentc_streq(v, "medium")) return 3;
    if (agentc_streq(v, "high")) return 4;
    *ok = false;
    return 0;
}

static void write_fd(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n) {
        int w = os_write(fd, q, n);
        if (w <= 0) return;
        q += w;
        n -= (size_t)w;
    }
}

/* ----------------------------------------------------------- print events */

/* Print output only. Persistence is owned by the mode context (app/mode.c):
 * the observer installed there is bound to the ctx, so an RPC `set_model`
 * rebuild or a `new_session` swap cannot leave a stale agent/session behind a
 * front-end-local struct. */
static void on_event(void *ud, int ev, const void *data) {
    AgcModeCtx *c = ud;
    switch (ev) {
    case AGENTC_EV_TEXT_DELTA: {
        const AgcTextDelta *d = data;
        agentc_out(d->text, d->len);
        break;
    }
    case AGENTC_EV_THINK_DELTA:
        break;
    case AGENTC_EV_TOOL_EXEC_START: {
        const AgcToolExec *e = data;
        agentc_logf(1, "tool %s %s", e->tool_name ? e->tool_name : "?",
                e->args_json ? e->args_json : "");
        break;
    }
    case AGENTC_EV_TOOL_EXEC_END: {
        const AgcToolExec *e = data;
        (void)c;
        agentc_logf(e->is_error ? 2 : 1, "tool %s %s (%lldms)",
                e->tool_name ? e->tool_name : "?", e->is_error ? "failed" : "ok",
                (long long)e->duration_ms);
        break;
    }
    case AGENTC_EV_MSG_END:
        break;   /* the mode observer persists it */
    case AGENTC_EV_COMPACT: {
        const AgcCompactInfo *ci = data;
        agentc_mode_note_compaction(c, ci);
        agentc_logf(1, "compacted context: %u tokens, kept %u messages%s", ci->tokens_before,
                ci->kept_messages, ci->automatic ? " (automatic)" : "");
        break;
    }
    case AGENTC_EV_ERROR:
        agentc_logf(3, "%s", (const char *)data);
        break;
    default:
        break;
    }
}

/* The TUI owns no session context of its own, so its compaction event is
 * forwarded to the mode context: this persists the checkpoint and keeps the
 * session flush index in sync with the spliced transcript. */
static void tui_mode_compact(void *ud, const AgcCompactInfo *ci) {
    agentc_mode_note_compaction(ud, ci);
}

/* Startup bounded pump. The mcp extension only creates its server records on
 * the first registry pump (after the final trust context), so a fast local
 * server would otherwise not appear until request #1's wire poll hook. Run a
 * wall-clock budget of app-level pumps instead of adding a registry API: a
 * fast server settles in milliseconds and the loop exits as soon as every
 * record is READY/FAILED; an unreachable one cannot delay startup beyond the
 * budget. The first pump is cheap and creates no records when mcp is disabled
 * or no server is configured, so a plain build pays nothing; the only output
 * is one debug line hidden at the default log level. */
#define STARTUP_PUMP_BUDGET_MS 400
static void startup_pump(void) {
    i64 start = os_now_ns(OS_CLOCK_MONOTONIC);
    agentc_ext_pump();
    if (agentc_mcp_server_count() == 0) return;
    while (agentc_mcp_pending()) {
        if (os_now_ns(OS_CLOCK_MONOTONIC) - start >=
            (i64)STARTUP_PUMP_BUDGET_MS * 1000000)
            break;
        agentc_ext_pump();
        os_sleep_ns(1000000);   /* 1 ms: a bounded wait, not a busy spin */
    }
    agentc_logf(0, "mcp: startup pump settled after %lldms (%s)",
                (long long)((os_now_ns(OS_CLOCK_MONOTONIC) - start) / 1000000),
                agentc_mcp_pending() ? "budget reached" : "all servers settled");
}

/* ------------------------------------------------- extension diagnostics */

static const char *ext_state_name(int state) {
    switch (state) {
    case AGENTC_EXT_STATE_PENDING: return "pending";
    case AGENTC_EXT_STATE_LOADED:  return "loaded";
    case AGENTC_EXT_STATE_FAILED:  return "failed";
    default:                       return "skipped";
    }
}

static AgcExtInfo *ext_collect(size_t *n_out) {
    size_t n = agentc_ext_describe(NULL, 0);
    if (n_out) *n_out = n;
    if (!n) return NULL;
    AgcExtInfo *info = agentc_alloc(n * sizeof *info);
    n = agentc_ext_describe(info, n);
    if (n_out) *n_out = n;
    return info;
}

/* --list-extensions: one aligned line per registered extension, mirroring
 * --list-models. An unknown-disabled name is a config typo with no descriptor,
 * so it is flagged inline. */
static void ext_print_list(void) {
    size_t n = 0;
    AgcExtInfo *info = ext_collect(&n);
    for (size_t i = 0; i < n; i++) {
        agentc_outf("%-8s %-24s %s%s%s\n", ext_state_name(info[i].state),
                    info[i].name ? info[i].name : "?",
                    info[i].version ? info[i].version : "",
                    info[i].dynamic ? " (dynamic)" : "",
                    info[i].known ? "" : " (unknown disabled name)");
    }
    agentc_free(info);
}

static bool ext_name_is_default(const char *name) {
    return name && (agentc_streq(name, "builtin-tools") ||
                    agentc_streq(name, "builtin-context") ||
                    agentc_streq(name, "mcp"));
}

/* One info-level summary after load_all, and only when extensions are actually
 * part of this session (a linked extension, a disabled/unknown name or a failed
 * init): a stock run with nothing but the three defaults stays silent. */
static void ext_log_summary(void) {
    size_t n = 0;
    AgcExtInfo *info = ext_collect(&n);
    if (n == 0) { agentc_free(info); return; }
    size_t loaded = 0, disabled = 0, unknown = 0;
    bool noteworthy = false;
    AgcBuf names = { 0 };
    for (size_t i = 0; i < n; i++) {
        if (info[i].state == AGENTC_EXT_STATE_LOADED) {
            loaded++;
            if (names.len) agentc_buf_cstr(&names, ", ");
            agentc_buf_cstr(&names, info[i].name ? info[i].name : "?");
            if (!ext_name_is_default(info[i].name)) noteworthy = true;
        } else if (info[i].disabled) {
            if (info[i].known) disabled++;
            else unknown++;
            noteworthy = true;
        } else {
            noteworthy = true;   /* failed init or a pending record */
        }
    }
    if (noteworthy)
        agentc_logf(1, "extensions: loaded %llu (%s); disabled (%llu); unknown-disabled (%llu)",
                    (unsigned long long)loaded, names.p ? (const char *)names.p : "",
                    (unsigned long long)disabled, (unsigned long long)unknown);
    agentc_buf_free(&names);
    agentc_free(info);
}

/* The app's active-tool policy and destructive default live in
 * src/app/policy.c; the trust/resources ordering lives in src/app/project.c.
 * main.c only orchestrates. */

int agentc_main(int argc, char **argv) {
    const char *prompt = NULL;
    const char *provider_flag = NULL;
    const char *model_flag = NULL;
    const char *api_key_flag = NULL;
    const char *base_url_flag = NULL;
    const char *system_flag = NULL;
    const char *record_file = NULL;
    const char *tools_flag = NULL;
    const char *tools_engine_flag = NULL;
    const char *session_flag = NULL;
    const char *session_dir_flag = NULL;
    const char *mode_flag = NULL;
    int thinking_flag = 0;
    i64 max_tokens_flag = 0;
    bool thinking_set = false, max_tokens_set = false;
    bool dump_wire = false, insecure = false, no_tools = false, no_mcp = false;
    bool print_mode = false;
    int tui_mode = AGENTC_TUI_INLINE;   /* owned bottom region (default) */
    bool tui_mode_set = false;
    bool list_sessions = false, no_session = false, cont = false, resume = false;
    bool list_models_flag = false, list_extensions = false, refresh_models = false, offline = false;
    bool sync_pricing = false;
    const char *models_filter = NULL;
    int cli_trust = -1;

    /* ------------------------------------------------------------ setup */
    if (argc > 1 && agentc_streq(argv[1], "setup")) {
        bool off = false;
        for (int i = 2; i < argc; i++)
            if (agentc_streq(argv[i], "--offline")) off = true;
        AgcConfig *scfg = agentc_config_load(NULL);
        (void)agentc_pricing_load(NULL, 0);
        /* The preset menu offers loaded extension providers, so compose them
         * first. MCP is skipped: setup only picks a provider/model and must
         * not start a server. */
        agentc_ext_register_defaults(true);
        agentc_ext_register_linked();
        agentc_ext_register_dynamic();
        agentc_ext_apply_config(scfg);
        agentc_ext_load_all();
        int src = agentc_setup_onboard(&scfg, NULL, off);
        agentc_ext_shutdown();
        agentc_auth_free();
        agentc_config_free(scfg);
        return src == 0 ? 0 : 1;
    }

    /* ------------------------------------------------------ login / logout */
    if (argc > 1 && (agentc_streq(argv[1], "login") || agentc_streq(argv[1], "logout"))) {
        bool login = agentc_streq(argv[1], "login");
        const char *prov = NULL;
        bool manual = false;
        for (int i = 2; i < argc; i++) {
            const char *a = argv[i];
            if (agentc_streq(a, "-h") || agentc_streq(a, "--help")) {
                agentc_outs("usage: agentc login [anthropic|openai] [--manual]\n"
                        "       agentc logout [provider]\n"
                        "       --manual: print the URL and paste the code back "
                        "(remote/headless)\n");
                return 0;
            }
            if (agentc_streq(a, "--manual") || agentc_streq(a, "--paste")) {
                manual = true;
                continue;
            }
            if (a[0] == '-' && a[1]) {
                agentc_logf(3, "%s: unknown option '%s'", login ? "login" : "logout", a);
                return 2;
            }
            if (!prov) {
                prov = a;
                continue;
            }
            agentc_logf(3, "%s: unexpected argument '%s'", login ? "login" : "logout", a);
            return 2;
        }
        if (!prov || !prov[0]) prov = "openai";
        if (login) {
            /* Only the two subscription providers have an OAuth flow. */
            if (!agentc_streq(prov, "anthropic") && !agentc_streq(prov, "openai")) {
                agentc_logf(3, "login: unknown provider '%s' (anthropic|openai)", prov);
                return 2;
            }
            int rc = manual ? agentc_oauth_login_manual(prov) : agentc_oauth_login(prov);
            if (rc != 0) {
                char err[256];
                agentc_snprintf(err, sizeof err, "%s",
                            agentc_oauth_last_error() ? agentc_oauth_last_error()
                                                      : "oauth failed");
                agentc_logf(3, "login: %s", err);
                agentc_auth_free();
                return 1;
            }
            /* Make the provider just authenticated the default, so the next plain
             * `agentc` start uses it instead of a stale configured default. */
            int wrc = agentc_config_setup_set_default(prov, "");
            if (wrc == 0) {
                agentc_outf("default provider set to %s\n", prov);
            } else if (wrc == 1) {
                agentc_logf(2, "note: config.jsonc sets default_provider; "
                            "update it to switch to %s", prov);
            } else {
                agentc_logf(2, "could not set the default provider (errno %d)", wrc);
            }
            agentc_auth_free();
            return 0;
        }
        /* logout: remove whatever credential is stored for this id. A subscription
         * token is OAuth-only; an api_key can belong to any provider, so clear
         * both instead of rejecting ids outside anthropic|openai. */
        if (agentc_streq(prov, "anthropic") || agentc_streq(prov, "openai"))
            (void)agentc_oauth_logout(prov);
        int rc = agentc_auth_set_key(prov, NULL);
        if (rc != 0) {
            agentc_logf(3, "logout: cannot update auth.jsonc (errno %d)", rc);
            agentc_auth_free();
            return 1;
        }
        agentc_outf("removed stored credential for %s\n", prov);
        agentc_auth_free();
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (agentc_streq(a, "--version") || agentc_streq(a, "-v")) {
            agentc_outf("agentc %s\n", AGENTC_VERSION);
            return 0;
        }
        if (agentc_streq(a, "--help") || agentc_streq(a, "-h")) {
            usage();
            return 0;
        }
        if (agentc_streq(a, "-p") || agentc_streq(a, "--print")) {
            print_mode = true;
            prompt = need_arg(argc, argv, &i, a);
            if (!prompt) return 2;
        } else if (agentc_streq(a, "--provider")) {
            provider_flag = need_arg(argc, argv, &i, a);
            if (!provider_flag) return 2;
        } else if (agentc_streq(a, "--model")) {
            model_flag = need_arg(argc, argv, &i, a);
            if (!model_flag) return 2;
        } else if (agentc_streq(a, "--api-key")) {
            api_key_flag = need_arg(argc, argv, &i, a);
            if (!api_key_flag) return 2;
        } else if (agentc_streq(a, "--base-url")) {
            base_url_flag = need_arg(argc, argv, &i, a);
            if (!base_url_flag) return 2;
        } else if (agentc_streq(a, "--system")) {
            system_flag = need_arg(argc, argv, &i, a);
            if (!system_flag) return 2;
        } else if (agentc_streq(a, "--thinking")) {
            const char *v = need_arg(argc, argv, &i, a);
            if (!v) return 2;
            bool ok = false;
            thinking_flag = parse_thinking(v, &ok);
            if (!ok) {
                agentc_logf(3, "invalid --thinking value: %s", v);
                return 2;
            }
            thinking_set = true;
        } else if (agentc_streq(a, "--max-tokens")) {
            const char *v = need_arg(argc, argv, &i, a);
            if (!v) return 2;
            bool ok = false;
            max_tokens_flag = agentc_parse_i64(v, agentc_strlen(v), &ok);
            if (!ok || max_tokens_flag < 0) {
                agentc_logf(3, "invalid --max-tokens value: %s", v);
                return 2;
            }
            max_tokens_set = true;
        } else if (agentc_streq(a, "--tui-mode")) {
            const char *v = need_arg(argc, argv, &i, a);
            if (!v) return 2;
            if (agentc_tui_mode_parse(v, &tui_mode) != 0) {
                agentc_logf(3, "invalid --tui-mode value: %s (scrollback|inline|fullscreen|auto)", v);
                return 2;
            }
            tui_mode_set = true;
        } else if (agentc_streq(a, "--mode")) {
            mode_flag = need_arg(argc, argv, &i, a);
            if (!mode_flag) return 2;
            if (!agentc_streq(mode_flag, "json") && !agentc_streq(mode_flag, "rpc")) {
                agentc_logf(3, "invalid --mode value: %s", mode_flag);
                return 2;
            }
        } else if (agentc_streq(a, "--record")) {
            record_file = need_arg(argc, argv, &i, a);
            if (!record_file) return 2;
        } else if (agentc_streq(a, "--tools")) {
            tools_flag = need_arg(argc, argv, &i, a);
            if (!tools_flag) return 2;
        } else if (agentc_streq(a, "--tools-engine")) {
            const char *v = need_arg(argc, argv, &i, a);
            if (!v) return 2;
            if (!agentc_streq(v, "internal") && !agentc_streq(v, "external")) {
                agentc_logf(3, "invalid --tools-engine value: %s (internal|external)", v);
                usage_fd(2);
                return 2;
            }
            tools_engine_flag = v;
        } else if (agentc_streq(a, "--continue")) {
            cont = true;
        } else if (agentc_streq(a, "--resume")) {
            resume = true;
        } else if (agentc_streq(a, "--session")) {
            session_flag = need_arg(argc, argv, &i, a);
            if (!session_flag) return 2;
        } else if (agentc_streq(a, "--session-dir")) {
            session_dir_flag = need_arg(argc, argv, &i, a);
            if (!session_dir_flag) return 2;
        } else if (agentc_streq(a, "--no-session")) {
            no_session = true;
        } else if (agentc_streq(a, "--approve")) {
            cli_trust = 1;
        } else if (agentc_streq(a, "--no-approve")) {
            cli_trust = 0;
        } else if (agentc_streq(a, "--list-sessions")) {
            list_sessions = true;
        } else if (agentc_streq(a, "--list-models")) {
            list_models_flag = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') models_filter = argv[++i];
        } else if (agentc_streq(a, "--list-extensions")) {
            list_extensions = true;
        } else if (agentc_streq(a, "--refresh-models")) {
            refresh_models = true;
        } else if (agentc_streq(a, "--sync-pricing")) {
            sync_pricing = true;
        } else if (agentc_streq(a, "--offline")) {
            offline = true;
        } else if (agentc_streq(a, "--dump-wire")) {
            dump_wire = true;
        } else if (agentc_streq(a, "--insecure")) {
            insecure = true;
        } else if (agentc_streq(a, "--no-tools")) {
            no_tools = true;
        } else if (agentc_streq(a, "--no-mcp")) {
            no_mcp = true;
        } else {
            agentc_logf(3, "unknown argument: %s", a);
            usage_fd(2);
            return 2;
        }
    }

    char cwd[4096];
    if (os_getcwd(cwd, sizeof cwd) < 0) cwd[0] = 0;

    agentc_config_set_cli_trust(cli_trust);
    AgcConfig *cfg = agentc_config_load(cwd);
    /* --tools-engine beats AGENTC_TOOLS_ENGINE and tools.engine; the load above
     * already published the env/config/default value. */
    if (tools_engine_flag) agentc_config_set_tools_engine(tools_engine_flag);
    (void)agentc_pricing_load(NULL, 0);
    if (sync_pricing) {
        char perr[256] = "";
        int pn = agentc_pricing_sync_openrouter(perr, sizeof perr);
        if (pn < 0) {
            agentc_logf(3, "pricing sync failed: %s", perr[0] ? perr : "unknown error");
            agentc_config_free(cfg);
            return 1;
        }
        agentc_outf("synced %d model(s) from OpenRouter\n", pn);
        agentc_config_free(cfg);
        return 0;
    }
    /* flags beat config.jsonc; an unknown config spelling is reported and ignored
     * rather than silently changing the default */
    if (!tui_mode_set && cfg->tui_mode && cfg->tui_mode[0]) {
        int m = AGENTC_TUI_INLINE;
        if (agentc_tui_mode_parse(cfg->tui_mode, &m) == 0) tui_mode = m;
        else agentc_logf(2, "invalid tui_mode value in config: %s (using inline)", cfg->tui_mode);
    }

    const char *session_dir = NULL;
    if (session_dir_flag) session_dir = session_dir_flag;
    else if (cfg->session_dir && cfg->session_dir[0]) session_dir = cfg->session_dir;

    if (list_sessions) {
        size_t n = 0;
        char **list = agentc_session_list(session_dir, &n, 0);
        for (size_t i = 0; i < n; i++) {
            agentc_outs(list[i]);
            agentc_out_nl();
        }
        agentc_sessions_free(list, n);
        agentc_config_free(cfg);
        return 0;
    }

    /* --list-extensions: load the descriptor table so the reported state is
     * real, print it and exit. No provider or trust resolution is needed: the
     * table is config-filtered and init() only registers deferred work. */
    if (list_extensions) {
        agentc_ext_register_defaults(no_mcp);
        agentc_ext_register_linked();
        agentc_ext_register_dynamic();
        agentc_ext_apply_config(cfg);
        agentc_ext_load_all();
        ext_print_list();
        agentc_ext_shutdown();
        agentc_config_free(cfg);
        return 0;
    }

    /* Publish the endpoint/credential context before extension composition so
     * an extension provider's discovery and credential resolution see
     * --base-url; re-published after onboarding in case it wrote a credential. */
    agentc_setup_set_context(cfg, api_key_flag, base_url_flag);

    /* Extensions: the core defaults (builtin-tools, builtin-context and, unless
     * --no-mcp, mcp) are ordinary extensions; statically-linked ones come from
     * the generated table. load_all initializes lowest `order` first. This runs
     * before onboarding (so its preset menu can offer loaded extension
     * providers) and before provider resolution (so --provider <ext>,
     * --list-models and model auto-pick see them). Trust stays after it. */
    agentc_ext_register_defaults(no_mcp);
    agentc_ext_register_linked();
    agentc_ext_register_dynamic();
    agentc_ext_apply_config(cfg);   /* by name: defaults and linked alike */
    /* Provisional and safe: no server may start before trust is known. */
    agentc_mcp_set_context(cwd, false);
    agentc_ext_load_all();
    ext_log_summary();
    /* One startup line naming the core-tool backend chosen at builtin-tools
     * init; kept out of the extension init so golden tests that load
     * extensions stay independent of the host PATH. The TUI carries the same
     * summary in its banner, so only the non-TUI modes log it. */
    if (print_mode || mode_flag) agentc_tool_engine_log();

    /* first run: no explicit choice, no credential anywhere, interactive ->
     * walk the user through picking a provider (agentc setup does it again later) */
    if (!provider_flag && !model_flag && !api_key_flag && !print_mode && !list_models_flag &&
        !list_sessions) {
        char sp[4200];
        int c0 = 0, r0 = 0;
        bool tty = os_tty_size(0, &c0, &r0) == 0;
        bool have_setup = agentc_config_setup_path(sp, sizeof sp) && file_exists(sp);
        const char *dprov = cfg->default_provider ? cfg->default_provider : "openai";
        if (tty && !have_setup && !agentc_auth_key(dprov) && !agentc_oauth_logged_in(dprov)) {
            if (agentc_setup_onboard(&cfg, base_url_flag, offline) != 0) {
                agentc_ext_shutdown();
                agentc_config_free(cfg);
                return 1;
            }
        }
    }

    /* Re-publish the credential-resolution context once: discovery, the agent
     * rebuild and every later provider lookup must agree on whether `openai`
     * means Chat Completions (explicit key) or Codex (OAuth). */
    agentc_setup_set_context(cfg, api_key_flag, base_url_flag);

    const char *provider = (provider_flag && provider_flag[0]) ? provider_flag
                           : (cfg->default_provider && cfg->default_provider[0])
                               ? cfg->default_provider
                               : "openai";
    const AgcProvider *prov = agentc_setup_provider(provider);
    if (!prov) {
        const char *names[64];
        size_t nn = agentc_setup_provider_names(names, 64);
        AgcBuf known = { 0 };
        for (size_t i = 0; i < nn; i++) {
            if (agentc_streq(names[i], provider)) continue;
            if (known.len) agentc_buf_cstr(&known, ", ");
            agentc_buf_cstr(&known, names[i]);
        }
        agentc_logf(3, "unknown provider: %s (known: %s)", provider,
                    known.p ? (const char *)known.p : "none");
        agentc_buf_free(&known);
        agentc_ext_shutdown();
        agentc_config_free(cfg);
        return 2;
    }

    if (list_models_flag) {
        int lrc = agentc_setup_list_models(cfg, models_filter, refresh_models, offline);
        agentc_ext_shutdown();
        agentc_config_free(cfg);
        return lrc;
    }

    const char *model = (model_flag && model_flag[0]) ? model_flag : NULL;
    if (!model && cfg->default_model && cfg->default_model[0]) {
        /* trust the configured default for this provider (or when it is a known
         * model id, e.g. the built-in anthropic default), but never mix the two
         * `openai` wires (a chat model must not default onto the Codex api). */
        const char *cfg_prov = cfg->default_provider ? cfg->default_provider : "openai";
        const AgcModel *dm = agentc_model_find(provider, cfg->default_model);
        bool api_ok = !dm || !dm->api || !prov->api || agentc_streq(dm->api, prov->api);
        if ((agentc_streq(provider, cfg_prov) || dm) && api_ok)
            model = cfg->default_model;
    }
    /* --refresh-models must refresh the provider's list even when the configured
     * model already resolves; auto-pick stays inert for a selected model. */
    if (refresh_models || !model || !agentc_model_find(provider, model)) {
        /* discover (cache first, then the network unless --offline); the return
         * value is informational here because the static catalog stands in when
         * a provider (Codex) has no listing. */
        (void)agentc_setup_discover(cfg, provider, true, offline, refresh_models);
        if (!model) {
            const AgcModel *all[1024];
            size_t n = agentc_model_filter(provider, prov->api, all, 1024);
            if (n) model = all[0]->id;
        }
        if (model && !agentc_model_find(provider, model)) {
            const char *eff = agentc_setup_base_url(cfg, provider, base_url_flag);
            if (!eff) eff = prov->default_base_url;
            agentc_model_register_dynamic(provider, model, prov->api, eff, 0, 0, false, false);
        }
    }
    if (!model || !model[0]) {
        agentc_logf(3, "no model selected for %s; pass --model or run `agentc setup`", provider);
        agentc_ext_shutdown();
        agentc_config_free(cfg);
        return 2;
    }

    int thinking = thinking_flag;
    if (!thinking_set && cfg->default_thinking && cfg->default_thinking[0]) {
        bool ok = false;
        int t = parse_thinking(cfg->default_thinking, &ok);
        if (ok) thinking = t;
        else agentc_logf(2, "warning: ignoring invalid default_thinking: %s", cfg->default_thinking);
    }
    i64 max_tokens = max_tokens_set ? max_tokens_flag : cfg->max_tokens;

    /* Trust is resolved only now that extensions exist: a loaded
     * project_trust handler decides before the saved verdict; an explicit CLI
     * verdict wins over both. No registry pump may run between load_all and
     * the final set_context; the mcp extension creates its servers on the
     * first pump with the final context. */
    const bool trusted = agentc_project_resolve_trust(cwd, cli_trust);
    agentc_mcp_set_context(cwd, trusted);
    agentc_project_apply_resources(cwd, trusted);
    (void)agentc_prompts_load_file_templates(cwd, trusted);
    agentc_builtin_context_set(cwd, trusted);

    /* Startup bounded pump (see above): MCP tools from fast local servers are
     * part of the table below, so request #1 advertises them. */
    startup_pump();

    /* Compose every available tool first and only then apply --no-tools /
     * --tools / config defaults, so the filters see the same list the model
     * would. Count-first: the arrays are exactly as large as the registry. */
    size_t navail = agentc_ext_tools(NULL, 0);
    AgcTool *available = agentc_alloc((navail ? navail : 1) * sizeof *available);
    navail = agentc_ext_tools(available, navail);
    ToolPolicy rctx = { cfg, tools_flag, no_tools };
    AgcTool *tools = NULL;
    size_t ntools = 0;
    if (agentc_policy_apply(available, navail, &rctx, true, &tools, &ntools) != 0) {
        agentc_free(available);
        agentc_ext_shutdown();
        agentc_config_free(cfg);
        return 2;
    }
    agentc_free(available);
    agentc_ext_clear_dirty();   /* the startup set is the baseline, not a change */

    /* Explicit API keys (--api-key > env > config) beat a stored OAuth token:
     * the provider choice above routed an explicit key to Chat Completions, so
     * the OAuth token must not be substituted for it. */
    const char *api_key = agentc_setup_explicit_key(cfg, provider, api_key_flag);
    if (!api_key || !api_key[0]) api_key = agentc_auth_key(provider);
    if (!api_key || !api_key[0]) {
        /* A stored OAuth credential owns the provider: a failed refresh is an
         * error, not a reason to try config api_keys or the environment. */
        if (agentc_oauth_logged_in(provider)) {
            const char *err = agentc_oauth_last_error();
            agentc_logf(3, "oauth: %s", err ? err : "no valid token");
            agentc_free(tools);
            agentc_ext_shutdown();
            agentc_config_free(cfg);
            return 2;
        }
        api_key = agentc_config_api_key(cfg, provider);
    }
    if ((!api_key || !api_key[0]) && agentc_setup_needs_key(provider)) {
        /* suggest the current provider when the registry knows it, anthropic
         * otherwise (the old test was a hardcoded anthropic/openai pair) */
        const char *hint =
            agentc_provider_by_name(provider) ? provider : "openai";
        agentc_logf(2,
                "no API key for %s: pass --api-key, set the provider env var, or run "
                "`agentc setup` / `agentc login %s`",
                provider, hint);
        api_key = NULL;
        if (print_mode) {
            agentc_free(tools);
            agentc_ext_shutdown();
            agentc_config_free(cfg);
            return 2;
        }
    }

    const char *base_url = agentc_setup_base_url(cfg, provider, base_url_flag);

    /* System prompt: --system is the explicit hard-replace; otherwise NULL
     * means auto, and the agent rebuilds the prompt from the live tool table
     * before each turn (a turn-boundary recompose is reflected in the next
     * request). The string is borrowed from argv. */
    const char *system_text = system_flag;

    /* --------------------------------------------------------- mode setup */
    /* Session selection, agent construction and the extension wiring all live in
     * agentc_mode_setup (src/app/mode.c); only the CLI flags stay here. */
    AgcSessionOptions so;
    agentc_memset(&so, 0, sizeof so);
    so.cwd = cwd;
    so.dir = session_dir;
    so.memory_only = no_session;

    AgcModeCtx mc;
    agentc_memset(&mc, 0, sizeof mc);

    AgcModeIo mio;
    agentc_memset(&mio, 0, sizeof mio);
    mio.read = stdio_read;
    mio.write = stdio_write;
    mio.in_ud = (void *)(size_t)0;
    mio.out_ud = (void *)(size_t)1;
    /* Persistence is owned by the mode context: a NULL observer selects the
     * ctx-bound mode_flush_observer, so RPC's set_model/new_session swaps can
     * never leave a stale agent/session behind a front-end-local struct. */
    mio.observer = NULL;
    mio.observer_ud = NULL;
    if (print_mode) {
        mio.event = on_event;
        mio.event_ud = &mc;
    }

    AgcModeConfig mcfg;
    agentc_memset(&mcfg, 0, sizeof mcfg);
    mcfg.cfg = cfg;
    mcfg.provider = provider;
    mcfg.model = model;
    mcfg.api_key = api_key;
    mcfg.base_url = base_url;
    mcfg.system = system_text;
    mcfg.tools = tools;
    mcfg.ntools = ntools;
    mcfg.thinking = thinking;
    mcfg.max_tokens = max_tokens;
    mcfg.max_attempts = cfg->max_attempts;
    mcfg.insecure = insecure;
    mcfg.auto_compact = true;
    mcfg.compact_reserve = 16384;
    mcfg.compact_keep = 20000;
    mcfg.session = so;
    mcfg.session_spec = session_flag;
    mcfg.continue_last = cont || resume;
    mcfg.recompose = agentc_policy_recompose;
    mcfg.recompose_ud = &rctx;

    agentc_net_init();
    int src = agentc_mode_setup(&mc, &mcfg, &mio, AGENTC_MODE_F_SESSION | AGENTC_MODE_F_ABORT);
    if (src != 0) {
        agentc_free(tools);
        agentc_auth_free();
        agentc_ext_shutdown();
        agentc_config_free(cfg);
        return src == -2 ? 2 : 1;
    }

    /* --------------------------------------------------------------- modes */
    if (mode_flag) {
        int mrc;
        if (agentc_streq(mode_flag, "json")) {
            if (!prompt) {
                agentc_logf(3, "--mode json needs a prompt (-p PROMPT)");
                mrc = 2;
            } else {
                mrc = agentc_mode_json_run(&mc, prompt);
            }
        } else {
            mrc = agentc_mode_rpc_run(&mc);
        }
        if (agentc_ext_wants("session_shutdown")) {
            AgcExtResult sr = agentc_ext_emit("session_shutdown", "{\"reason\":\"quit\"}");
            agentc_free(sr.result_json);
        }
        agentc_ext_shutdown();
        agentc_mode_teardown(&mc);
        agentc_free(tools);
        agentc_auth_free();
        agentc_config_free(cfg);
        return mrc == 0 ? 0 : (mrc == 2 ? 2 : 1);
    }

    if (!print_mode) {
        int trc = agentc_tui_run(mc.agent, prompt, tui_mode, cfg->theme, trusted,
                                 ntools > 0, tui_mode_compact, &mc);
        if (trc != 0)
            agentc_logf(3, "cannot start the terminal UI (no tty?); use -p for print mode");
        if (agentc_ext_wants("session_shutdown")) {
            AgcExtResult sr = agentc_ext_emit("session_shutdown", "{\"reason\":\"quit\"}");
            agentc_free(sr.result_json);
        }
        agentc_ext_shutdown();
        agentc_mode_teardown(&mc);
        agentc_free(tools);
        agentc_auth_free();
        /* The TUI runs agent turns, whose agentc_policy_recompose reads
         * rctx.cfg; keep the config alive until the agent is gone, like the
         * print paths do. */
        agentc_config_free(cfg);
        return trc == 0 ? 0 : 1;
    }

    AgcBuf rec = { 0 };
    if (record_file || dump_wire) agentc_agent_set_record(mc.agent, &rec);

    int rc = agentc_agent_submit(mc.agent, prompt ? prompt : "");
    if (rc == 0) agentc_out_nl();

    if (record_file) {
        int fd = os_open(record_file, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, 0644);
        if (fd < 0) {
            agentc_logf(3, "cannot write --record file: %s", record_file);
            rc = -1;
        } else {
            write_fd(fd, rec.p, rec.len);
            os_close(fd);
        }
    }
    if (dump_wire) write_fd(2, rec.p, rec.len);
    agentc_buf_free(&rec);

    if (rc != 0) {
        const char *err = agentc_agent_last_error(mc.agent);
        if (err) agentc_logf(3, "%s", err);
    }
    if (agentc_ext_wants("session_shutdown")) {
        AgcExtResult sr = agentc_ext_emit("session_shutdown", "{\"reason\":\"quit\"}");
        agentc_free(sr.result_json);
    }
    agentc_ext_shutdown();
    agentc_mode_teardown(&mc);
    agentc_free(tools);
    agentc_auth_free();
    agentc_config_free(cfg);
    return rc == 0 ? 0 : 1;
}
