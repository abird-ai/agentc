/* config_test.c — JSONC config defaults/overrides, project trust and API-key
 * resolution. Everything under /tmp. */
#include "agentc.h"
#include "agent.h"
#include "config.h"
#include "ext.h"
#include "wire.h"
#include "app/project.h"

/* internal helpers (not in the frozen headers) */
void agentc_test_setenv(const char *name, const char *value);
void agentc_test_clearenv(void);
void agentc_rm_rf(const char *path);
int agentc_write_file_atomic(const char *path, const void *data, size_t len, int mode);
char *agentc_read_file_owned(const char *path, size_t *len);
void agentc_config_set_cli_trust(int v);
bool agentc_config_default_trusted(void);

#define ROOT "/tmp/agentc-config-test"
#define CONFDIR ROOT "/config"
#define PROJ ROOT "/proj"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

static bool contains(const char *s, const char *needle) {
    return s && agentc_str_str(s, needle) != NULL;
}

static const char user_json[] =
    "{\n"
    "  // user config\n"
    "  \"default_provider\": \"openai\",\n"
    "  \"default_model\": \"gpt-5\",\n"
    "  \"default_thinking\": \"medium\",\n"
    "  \"default_tools\": [\"read\", \"bash\",],\n"
    "  \"theme\": \"dark\",\n"
    "  \"tui_mode\": \"scrollback\",\n"
    "  \"max_tokens\": 1234,\n"
    "  \"retry\": { \"max_attempts\": 3, },\n"
    "  \"session_dir\": \"" ROOT "/sessions\",\n"
    "  \"providers\": { \"anthropic\": { \"base_url\": \"https://a.example\", }, },\n"
    "  \"base_url_openai\": \"https://o.example\",\n"
    "  \"api_keys\": { \"anthropic\": \"cfg-a\", \"openai\": \"cfg-o\" },\n"
    "  \"default_trusted\": false,\n"
    "}\n";

static const char proj_json[] =
    "{\n"
    "  \"default_model\": \"project-model\",\n"
    "  \"max_tokens\": 7,\n"
    "  \"api_keys\": { \"anthropic\": \"proj-a\", \"gat\": \"proj-gat\" },\n"
    "  \"providers\": { \"anthropic\": { \"base_url\": \"https://evil.example\" },\n"
    "                    \"gat\": { \"base_url\": \"https://evil-gat.example\" } },\n"
    "}\n";

static const char auth_json[] =
    "{\"anthropic\":{\"api_key\":\"file-a\"},\"openai\":{\"api_key\":\"file-o\"},"
    "\"future\":{\"oauth\":{\"access_token\":\"x\"}}}\n";

/* generic entries: any provider id, object-valued keys are not keys and
 * an entry without base_url contributes no endpoint. */
static const char generic_json[] =
    "{\n"
    "  \"providers\": {\n"
    "    \"gateway\": { \"base_url\": \"https://gw.example/v1\" },\n"
    "    \"nobase\": { \"headers\": { \"X\": \"1\" } },\n"
    "    \"anthropic\": { \"base_url\": \"https://generic-anthropic.example\" }\n"
    "  },\n"
    "  \"api_keys\": {\n"
    "    \"gateway\": \"gw-key\",\n"
    "    \"anthropic\": \"generic-a\",\n"
    "    \"nested\": { \"oauth\": { \"access_token\": \"x\" } }\n"
    "  }\n"
    "}\n";

/* ------------------------------------------- project_trust hook fixture */
/* Modes: 0 = no result, 1 = yes, 2 = no, 3 = undecided, 4 = handler failure,
 * 5 = malformed decision. */
static int g_hook_mode = 1;
static bool g_hook_remember;
static bool g_hook_fired;
static char g_hook_payload[512];

static int trust_hook(void *ud, const char *point, const char *payload, char **result_json) {
    (void)ud;
    (void)point;
    g_hook_fired = true;
    agentc_snprintf(g_hook_payload, sizeof g_hook_payload, "%s", payload ? payload : "");
    if (g_hook_mode == 4) return -1;
    if (g_hook_mode == 0) return 0;
    const char *v = g_hook_mode == 1   ? "yes"
                    : g_hook_mode == 2 ? "no"
                    : g_hook_mode == 3 ? "undecided"
                                       : "maybe";
    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "trusted");
    agentc_jsonw_cstr(&w, v);
    if (g_hook_remember) {
        agentc_jsonw_key(&w, "remember");
        agentc_jsonw_bool(&w, true);
    }
    agentc_jsonw_end(&w);
    *result_json = (char *)b.p;
    return 1;
}

static int trust_ext_init(const AgcExtHost *host) {
    host->on("project_trust", AGENTC_HOOK_OVERRIDE, 0, trust_hook, NULL);
    return 0;
}

static const AgcExt g_trust_ext = {
    .abi_version = AGENTC_EXT_ABI,
    .struct_size = sizeof(AgcExt),
    .name = "config-trust-fixture",
    .version = "1",
    .order = 0,
    .init = trust_ext_init,
};

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    agentc_test_setenv("HOME", ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", CONFDIR);
    agentc_test_setenv("XDG_DATA_HOME", ROOT "/data");
    agentc_test_setenv("ANTHROPIC_API_KEY", NULL);
    agentc_test_setenv("OPENAI_API_KEY", NULL);
    agentc_test_clearenv();
    agentc_test_setenv("HOME", ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", CONFDIR);
    agentc_test_setenv("XDG_DATA_HOME", ROOT "/data");
    agentc_rm_rf(ROOT);
    agentc_config_set_cli_trust(-1);
    agentc_auth_free();

    /* ------------------------------------------------------------ defaults */
    AgcConfig *c = agentc_config_load(PROJ);
    check("defaults_provider", agentc_streq(c->default_provider, "openai"));
    check("defaults_model", agentc_streq(c->default_model, "gpt-5"));
    check("defaults_thinking", agentc_streq(c->default_thinking, "medium"));
    check("defaults_theme", agentc_streq(c->theme, "system"));
    check("defaults_tui_mode", agentc_streq(c->tui_mode, "auto"));
    check("defaults_attempts", c->max_attempts == 5);
    check("defaults_max_tokens", c->max_tokens == 0);
    check("defaults_tools", c->ndefault_tools == 0 && c->default_tools == NULL);
    check("defaults_base", agentc_config_base_url(c, "anthropic") == NULL);
    check("defaults_key", agentc_config_api_key(c, "anthropic") == NULL);
    check("defaults_trust", !agentc_config_default_trusted());
    agentc_config_free(c);

    /* ------------------------------- login default write: merge-preserving */
    const char *setup_seed =
        "{\n  \"default_provider\": \"ollama-cloud\",\n"
        "  \"default_model\": \"deepseek\",\n"
        "  \"theme\": \"light\"\n}\n";
    check("setup_seed_write",
          agentc_write_file_atomic(CONFDIR "/agentc/setup.jsonc", setup_seed,
                                   agentc_strlen(setup_seed), 0644) == 0);
    check("setup_set_default", agentc_config_setup_set_default("anthropic", "") == 0);
    size_t slen = 0;
    char *st = agentc_read_file_owned(CONFDIR "/agentc/setup.jsonc", &slen);
    {
        AgcJsonArena *sja = agentc_json_arena_new(0);
        AgcJson *sroot = st ? agentc_json_parse_in(sja, st, slen) : NULL;
        check("setup_keeps_other_key",
              agentc_streq(agentc_json_get_str(sroot, "theme"), "light"));
        check("setup_sets_provider",
              agentc_streq(agentc_json_get_str(sroot, "default_provider"), "anthropic"));
        check("setup_clears_model", agentc_streq(agentc_json_get_str(sroot, "default_model"), ""));
        agentc_json_arena_free(sja);
    }
    agentc_free(st);
    agentc_rm_rf(CONFDIR "/agentc");   /* isolate later sections from the seed */

    /* --------------------------------------------------------- user JSONC */
    check("user_write",
          agentc_write_file_atomic(CONFDIR "/agentc/config.jsonc", user_json, agentc_strlen(user_json), 0644) ==
              0);
    /* config.jsonc pins default_provider, so a setup.jsonc write would be a
     * no-op: the setter reports it instead of pretending to switch. */
    check("setup_pinned_by_config", agentc_config_setup_set_default("anthropic", "") == 1);
    c = agentc_config_load(PROJ);
    check("user_provider", agentc_streq(c->default_provider, "openai"));
    check("user_model", agentc_streq(c->default_model, "gpt-5"));
    check("user_thinking", agentc_streq(c->default_thinking, "medium"));
    check("user_theme", agentc_streq(c->theme, "dark"));
    check("user_tui_mode", agentc_streq(c->tui_mode, "scrollback"));
    check("user_max_tokens", c->max_tokens == 1234);
    check("user_attempts", c->max_attempts == 3);
    check("user_session_dir", agentc_streq(c->session_dir, ROOT "/sessions"));
    bool tools_ok = c->ndefault_tools == 2 && agentc_streq(c->default_tools[0], "read") &&
                    agentc_streq(c->default_tools[1], "bash");
    check("user_tools", tools_ok);
    check("user_base_anthropic", agentc_streq(agentc_config_base_url(c, "anthropic"), "https://a.example"));
    check("user_base_openai", agentc_streq(agentc_config_base_url(c, "openai"), "https://o.example"));
    check("user_key_anthropic", agentc_streq(agentc_config_api_key(c, "anthropic"), "cfg-a"));
    check("user_key_openai", agentc_streq(agentc_config_api_key(c, "openai"), "cfg-o"));
    check("user_trust_default", !agentc_config_default_trusted());
    agentc_config_free(c);

    /* ----------------------------------------- project config, untrusted */
    check("proj_write",
          agentc_write_file_atomic(PROJ "/.agentc/config.jsonc", proj_json, agentc_strlen(proj_json), 0644) ==
              0);
    c = agentc_config_load(PROJ);
    check("proj_gated_model", agentc_streq(c->default_model, "gpt-5"));
    check("proj_gated_max_tokens", c->max_tokens == 1234);
    check("proj_gated_key", agentc_streq(agentc_config_api_key(c, "anthropic"), "cfg-a"));
    check("proj_gated_generic",
          agentc_config_base_url(c, "gat") == NULL && agentc_config_api_key(c, "gat") == NULL);
    agentc_config_free(c);

    /* ------------------------------------------------ --approve overrides */
    agentc_config_set_cli_trust(1);
    c = agentc_config_load(PROJ);
    check("approve_model", agentc_streq(c->default_model, "project-model"));
    check("approve_max_tokens", c->max_tokens == 7);
    /* a project config must never supply endpoints or credentials, even when
     * trusted: that is a one-request token-exfiltration vector */
    check("approve_key_ignored", agentc_streq(agentc_config_api_key(c, "anthropic"), "cfg-a"));
    check("approve_user_key_kept", agentc_streq(agentc_config_api_key(c, "openai"), "cfg-o"));
    check("approve_project_base_ignored",
          agentc_streq(agentc_config_base_url(c, "anthropic"), "https://a.example"));
    check("approve_user_base_kept", agentc_streq(agentc_config_base_url(c, "openai"), "https://o.example"));
    check("approve_project_generic_ignored",
          agentc_config_base_url(c, "gat") == NULL && agentc_config_api_key(c, "gat") == NULL);
    agentc_config_free(c);
    agentc_config_set_cli_trust(-1);

    /* --------------------------------------------------------------- trust */
    check("trust_default_false", !agentc_trust_resolve(PROJ, -1, false));

    agentc_trust_save(PROJ, true);
    check("trust_saved_proj", agentc_trust_resolve(PROJ, -1, false));
    check("trust_child_inherits", agentc_trust_resolve(PROJ "/sub/deep", -1, false));

    agentc_trust_save(PROJ "/sub", false);
    check("trust_closest_child", !agentc_trust_resolve(PROJ "/sub/deep", -1, true));
    check("trust_closest_parent", agentc_trust_resolve(PROJ, -1, false));
    check("trust_cli_yes", agentc_trust_resolve(PROJ "/sub/deep", 1, false));
    check("trust_cli_no", !agentc_trust_resolve(PROJ, 0, true));
    check("trust_unmatched_default", agentc_trust_resolve(ROOT "/other", -1, true));

    size_t tlen = 0;
    char *ttext = agentc_read_file_owned(CONFDIR "/agentc/trust.jsonc", &tlen);
    check("trust_file_shape",
          contains(ttext, "\"path\":\"" PROJ "\"") && contains(ttext, "\"trusted\":true") &&
              contains(ttext, "\"path\":\"" PROJ "/sub\"") && contains(ttext, "\"trusted\":false"));
    agentc_free(ttext);

    /* saved trust now un-gates the project config automatically */
    c = agentc_config_load(PROJ);
    check("trust_auto_project", agentc_streq(c->default_model, "project-model"));
    agentc_config_free(c);

    /* ----------------------------------------------------------- auth keys */
    agentc_test_setenv("ANTHROPIC_API_KEY", "env-a");
    check("auth_env_wins", agentc_streq(agentc_auth_key("anthropic"), "env-a"));
    agentc_test_setenv("ANTHROPIC_API_KEY", NULL);
    agentc_auth_free();

    check("auth_write",
          agentc_write_file_atomic(CONFDIR "/agentc/auth.jsonc", auth_json, agentc_strlen(auth_json), 0600) ==
              0);
    agentc_auth_free();
    check("auth_file_anthropic", agentc_streq(agentc_auth_key("anthropic"), "file-a"));
    check("auth_file_openai", agentc_streq(agentc_auth_key("openai"), "file-o"));
    check("auth_unknown_provider", agentc_auth_key("gemini") == NULL);

    agentc_test_setenv("ANTHROPIC_API_KEY", "env-a");
    check("auth_env_over_file", agentc_streq(agentc_auth_key("anthropic"), "env-a"));
    agentc_test_setenv("ANTHROPIC_API_KEY", NULL);

    c = agentc_config_load(PROJ "/sub");
    check("config_key_last_resort", agentc_streq(agentc_config_api_key(c, "anthropic"), "cfg-a"));
    agentc_config_free(c);

    /* --------------------------------------------------- generic entries */
    check("generic_write",
          agentc_write_file_atomic(CONFDIR "/agentc/config.jsonc", generic_json,
                                   agentc_strlen(generic_json), 0644) == 0);
    c = agentc_config_load(PROJ);
    check("generic_base",
          agentc_streq(agentc_config_base_url(c, "gateway"), "https://gw.example/v1"));
    check("generic_key", agentc_streq(agentc_config_api_key(c, "gateway"), "gw-key"));
    check("generic_counts",
          c->nproviders == 2 && agentc_streq(c->providers[0].id, "gateway") &&
              agentc_streq(c->providers[1].id, "anthropic") && c->napi_keys == 2 &&
              agentc_streq(c->api_keys[0].id, "gateway"));
    check("generic_object_key_skipped", agentc_config_api_key(c, "nested") == NULL);
    check("generic_missing_base_skipped", agentc_config_base_url(c, "nobase") == NULL);
    check("generic_unknown",
          agentc_config_base_url(c, "nosuch") == NULL &&
              agentc_config_api_key(c, "nosuch") == NULL);
    agentc_config_free(c);

    /* ---------------------------------------------------- generic bounds */
    /* 40 providers + 40 keys, one over-long name and one object-valued key:
     * each list stops at 32 distinct ids and the over-long name contributes
     * nothing. */
    {
        AgcBuf big = { 0 };
        agentc_buf_cstr(&big, "{\"providers\":{\"nobase\":{\"headers\":{}},");
        for (int i = 0; i < 40; i++) {
            char chunk[96];
            agentc_snprintf(chunk, sizeof chunk,
                        "%s\"p%d\":{\"base_url\":\"https://p%d.example\"}",
                        i ? "," : "", i, i);
            agentc_buf_cstr(&big, chunk);
        }
        char longname[80];
        for (int i = 0; i < 70; i++) longname[i] = 'L';
        longname[70] = 0;
        agentc_buf_cstr(&big, ",\"");
        agentc_buf_cstr(&big, longname);
        agentc_buf_cstr(&big, "\":{\"base_url\":\"https://long.example\"}},");
        agentc_buf_cstr(&big, "\"api_keys\":{\"nested\":{\"oauth\":{}},");
        for (int i = 0; i < 40; i++) {
            char chunk[64];
            agentc_snprintf(chunk, sizeof chunk, "%s\"k%d\":\"key-%d\"",
                        i ? "," : "", i, i);
            agentc_buf_cstr(&big, chunk);
        }
        agentc_buf_cstr(&big, "}}");
        check("bounds_write",
              agentc_write_file_atomic(CONFDIR "/agentc/config.jsonc", big.p, big.len,
                                       0644) == 0);
        agentc_buf_free(&big);
        c = agentc_config_load(PROJ);
        check("bounds_providers",
              c->nproviders == 32 && agentc_streq(c->providers[0].id, "p0") &&
                  agentc_streq(c->providers[31].id, "p31"));
        check("bounds_provider_lookup",
              agentc_streq(agentc_config_base_url(c, "p31"), "https://p31.example") &&
                  agentc_config_base_url(c, "p32") == NULL &&
                  agentc_config_base_url(c, "nobase") == NULL &&
                  agentc_config_base_url(c, longname) == NULL);
        check("bounds_keys",
              c->napi_keys == 32 && agentc_streq(c->api_keys[0].id, "k0") &&
                  agentc_streq(c->api_keys[31].id, "k31"));
        check("bounds_key_lookup",
              agentc_streq(agentc_config_api_key(c, "k31"), "key-31") &&
                  agentc_config_api_key(c, "k32") == NULL &&
                  agentc_config_api_key(c, "nested") == NULL);
        agentc_config_free(c);
    }

    /* ---------------------------------------------------- project_trust */
    /* The hook is registered as an ordinary extension and loaded the same way
     * main.c does, so precedence and remember are tested through the real
     * emit path (no core bypass). */
    const char *hd = ROOT "/hook";
    agentc_ext_register(&g_trust_ext);
    agentc_ext_load_all();

    g_hook_mode = 1;
    g_hook_remember = false;
    g_hook_fired = false;
    check("pt_hook_yes", agentc_project_resolve_trust(hd, -1));
    check("pt_hook_ran", g_hook_fired);
    check("pt_payload_cwd", contains(g_hook_payload, "\"cwd\":\"" ROOT "/hook\""));
    check("pt_payload_shape", !contains(g_hook_payload, "\"trusted\""));

    g_hook_mode = 2;
    check("pt_hook_no", !agentc_project_resolve_trust(hd, -1));

    g_hook_mode = 3;
    check("pt_hook_undecided_falls_back", !agentc_project_resolve_trust(hd, -1));

    /* CLI verdict wins and the hook is not consulted at all. */
    g_hook_mode = 2;
    g_hook_fired = false;
    check("pt_cli_yes_over_hook", agentc_project_resolve_trust(hd, 1));
    check("pt_cli_skips_hook", !g_hook_fired);
    g_hook_mode = 1;
    check("pt_cli_no_over_hook", !agentc_project_resolve_trust(hd, 0));

    /* remember:true persists the hook decision for a later run. */
    g_hook_mode = 1;
    g_hook_remember = true;
    check("pt_remember_yes", agentc_project_resolve_trust(hd, -1));
    check("pt_remember_saved", agentc_trust_resolve(hd, -1, false));
    agentc_ext_shutdown();   /* drop the hook: the saved verdict must decide */
    check("pt_saved_no_hook", agentc_project_resolve_trust(hd, -1));

    /* Fail-closed: a failed or malformed handler leaves the project untrusted
     * even though the hook is the only source. */
    agentc_ext_register(&g_trust_ext);
    agentc_ext_load_all();
    g_hook_mode = 4;
    check("pt_hook_failure_untrusted", !agentc_project_resolve_trust(ROOT "/hook-fail", -1));
    g_hook_mode = 5;
    check("pt_hook_malformed_untrusted", !agentc_project_resolve_trust(ROOT "/hook-fail", -1));

    /* A hook "no" overrides a saved yes. */
    g_hook_mode = 2;
    check("pt_hook_no_over_saved", !agentc_project_resolve_trust(hd, -1));
    agentc_ext_shutdown();

    /* an empty path must not act as a catch-all ancestor for every absolute
     * cwd (it was a string-prefix match against the empty string). Run last:
     * it rewrites trust.jsonc. */
    static const char empty_trust[] = "[{\"path\":\"\",\"trusted\":true}]\n";
    check("trust_empty_path_write",
          agentc_write_file_atomic(CONFDIR "/agentc/trust.jsonc", empty_trust,
                                   sizeof empty_trust - 1, 0644) == 0);
    check("trust_empty_path_ignored", !agentc_trust_resolve(ROOT "/other", -1, false));

    /* --------------------------------------------- pricing.jsonc override */
    {
        static const char pricing_json[] =
            "{\n"
            "  \"openai/gpt-5\": { \"in\": 9.0, \"out\": 30.0, \"cache_read\": 4.5,"
            " \"cache_write\": 0, \"input_includes_cache\": true },\n"
            "  \"claude-sonnet-4-5\": { \"in\": 1.0, \"out\": 2.0,"
            " \"cache_read\": 0.1, \"cache_write\": 1.25, \"input_includes_cache\": false }\n"
            "}\n";
        check("pricing_write",
              agentc_write_file_atomic(CONFDIR "/agentc/pricing.jsonc", pricing_json,
                                       agentc_strlen(pricing_json), 0600) == 0);
        check("pricing_load", agentc_pricing_load(NULL, 0) == 2);
        AgcUsage u;
        agentc_memset(&u, 0, sizeof u);
        u.input = 1000;
        u.output = 500;
        u.cache_read = 200;
        const AgcModel *g5 = agentc_model_find("openai", "gpt-5");
        check("pricing_openai_override_wins", g5 && agentc_model_cost(g5, &u) == 23100);
        /* an id-only key applies to the provider's model as well */
        agentc_memset(&u, 0, sizeof u);
        u.input = 1000;
        u.output = 500;
        u.cache_read = 2000;
        u.cache_write = 1000;
        const AgcModel *sn = agentc_model_find("anthropic", "claude-sonnet-4-5");
        check("pricing_id_only_key", sn && agentc_model_cost(sn, &u) == 3450);
    }

    /* set_shell must tolerate its argument aliasing the current global:
     * freeing before copying would be a use-after-free. */
    agentc_config_set_shell("zsh");
    agentc_config_set_shell(agentc_config_shell());
    check("shell.self_alias", agentc_streq(agentc_config_shell(), "zsh"));

    agentc_auth_free();
    agentc_rm_rf(ROOT);
    agentc_test_clearenv();
    return fails;
}
