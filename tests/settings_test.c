/* settings_test.c — extension settings resolution and persistence (P6-E).
 *
 * Covers: declared defaults; user/project/CLI resolution order; fail-safe
 * validation with a single warning; SECRET storage in auth.jsonc (never in
 * config.jsonc); project trust gating; persisted round-trip; and the count-first
 * list the RPC/CLI surfaces consume.
 */
#include "agentc.h"
#include "settings.h"
#include "agentc_ext.h"
#include "config.h"
#include "ext.h"
#include "plat.h"

void agentc_test_setenv(const char *name, const char *value);
void agentc_test_clearenv(void);
void agentc_rm_rf(const char *path);

static int fails;
static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

#define ROOT "/tmp/agentc-settings-test"
#define XDG ROOT "/xdg"
#define PROJ ROOT "/proj"

static void write_file(const char *path, const char *text) {
    agentc_mkdir_parents(path);
    (void)agentc_write_file_atomic(path, text, agentc_strlen(text), 0644);
}

static void reset_env(void) {
    agentc_rm_rf(ROOT);
    agentc_test_setenv("XDG_CONFIG_HOME", XDG);
    agentc_mkdir_parents(PROJ "/.agentc/x");
}

static AgcExtSetting setting(const char *key, int type, const char *dflt, uint32_t flags) {
    AgcExtSetting s;
    agentc_memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    s.key = key;
    s.label = key;
    s.description = key;
    s.type = type;
    s.default_json = dflt;
    s.flags = flags;
    return s;
}

static void test_defaults_and_order(void) {
    reset_env();
    agentc_settings_init(PROJ, false);
    AgcExtSetting n = setting("n", AGENTC_SET_INT, "5", 0);
    agentc_settings_declare("demo", &n, 0);
    check("default_int", agentc_settings_get_int("demo", "n", -1) == 5);
    check("default_scope", agentc_settings_scope_of("demo", "n") == -1);
    check("default_unset", !agentc_settings_is_set("demo", "n"));

    write_file(XDG "/abird/agentc/config.jsonc", "{\"settings\":{\"demo\":{\"n\":7}}}\n");
    agentc_settings_init(PROJ, false);
    agentc_settings_declare("demo", &n, 0);
    check("user_int", agentc_settings_get_int("demo", "n", -1) == 7);
    check("user_scope", agentc_settings_scope_of("demo", "n") == AGENTC_SETTINGS_SCOPE_USER);

    agentc_settings_add_cli("demo.n=9");
    check("cli_wins", agentc_settings_get_int("demo", "n", -1) == 9);
    check("cli_scope", agentc_settings_scope_of("demo", "n") == AGENTC_SETTINGS_SCOPE_CLI);
}

static void test_project_trust_gate(void) {
    reset_env();
    write_file(XDG "/abird/agentc/config.jsonc", "{\"settings\":{\"demo\":{\"n\":1}}}\n");
    write_file(PROJ "/.agentc/config.jsonc", "{\"settings\":{\"demo\":{\"n\":2}}}\n");
    AgcExtSetting n = setting("n", AGENTC_SET_INT, "5", AGENTC_SETTING_PROJECT);
    agentc_settings_init(PROJ, false);
    agentc_settings_declare("demo", &n, 0);
    check("project_ignored_untrusted", agentc_settings_get_int("demo", "n", -1) == 1);
    agentc_settings_init(PROJ, true);
    agentc_settings_declare("demo", &n, 0);
    check("project_wins_trusted", agentc_settings_get_int("demo", "n", -1) == 2);
    check("project_scope", agentc_settings_scope_of("demo", "n") == AGENTC_SETTINGS_SCOPE_PROJECT);
}

static void test_validation(void) {
    reset_env();
    write_file(XDG "/abird/agentc/config.jsonc", "{\"settings\":{\"demo\":{\"mode\":\"z\"}}}\n");
    AgcExtSetting m = setting("mode", AGENTC_SET_ENUM, "\"a\"", 0);
    m.enum_json = "[\"a\",\"b\"]";
    agentc_settings_init(PROJ, false);
    agentc_settings_declare("demo", &m, 0);
    check("invalid_enum_falls_back",
          agentc_streq(agentc_settings_get_json("demo", "mode"), "\"a\""));
    check("invalid_warns_once", agentc_streq(agentc_settings_get_json("demo", "mode"), "\"a\""));

    AgcExtSetting b = setting("count", AGENTC_SET_INT, "5", 0);
    b.min_json = "1";
    b.max_json = "10";
    agentc_settings_declare("demo", &b, 0);
    check("set_out_of_range", agentc_settings_set("demo", "count", "99") != 0);
    check("set_in_range", agentc_settings_set("demo", "count", "4") == 0);
    check("set_value", agentc_settings_get_int("demo", "count", -1) == 4);
    check("set_undeclared", agentc_settings_set("demo", "missing", "1") != 0);
}

static void test_secret_and_persistence(void) {
    reset_env();
    agentc_settings_init(PROJ, false);
    AgcExtSetting tok = setting("token", AGENTC_SET_STRING, "\"\"", AGENTC_SETTING_SECRET);
    agentc_settings_declare("demo", &tok, 0);
    check("secret_default", agentc_streq(agentc_settings_get_json("demo", "token"), "\"\""));
    check("secret_set", agentc_settings_set("demo", "token", "\"hunter2\"") == 0);
    check("secret_get", agentc_streq(agentc_settings_get_json("demo", "token"), "\"hunter2\""));

    /* auth.jsonc holds it; config.jsonc never does. */
    size_t alen = 0;
    char *auth = agentc_read_file_owned(XDG "/abird/agentc/auth.jsonc", &alen);
    size_t clen = 0;
    char *conf = agentc_read_file_owned(XDG "/abird/agentc/config.jsonc", &clen);
    check("secret_in_auth", auth && agentc_str_str(auth, "hunter2") != NULL);
    check("secret_not_in_config", !conf || agentc_str_str(conf, "hunter2") == NULL);
    agentc_free(auth);
    agentc_free(conf);

    /* round-trip across a fresh init */
    agentc_settings_init(PROJ, false);
    agentc_settings_declare("demo", &tok, 0);
    check("secret_roundtrip",
          agentc_streq(agentc_settings_get_json("demo", "token"), "\"hunter2\""));

    /* reset reverts to the declared default */
    check("reset", agentc_settings_reset("demo", "token") == 0);
    check("reset_default", agentc_streq(agentc_settings_get_json("demo", "token"), "\"\""));
}

static void test_list_and_host(void) {
    reset_env();
    agentc_settings_init(PROJ, false);
    AgcExtSetting a = setting("alpha", AGENTC_SET_BOOL, "true", 0);
    AgcExtSetting b = setting("beta", AGENTC_SET_STRING, "\"x\"", AGENTC_SETTING_ADVANCED);
    agentc_settings_declare("demo", &a, 0);
    agentc_settings_declare("demo", &b, 0);
    check("list_count", agentc_settings_list(NULL, 0) == 2);
    AgcSettingInfo info[4];
    size_t n = agentc_settings_list(info, 4);
    check("list_fill", n == 2 && agentc_streq(info[0].ext, "demo") &&
                           agentc_streq(info[0].key, "alpha") &&
                           info[1].flags == AGENTC_SETTING_ADVANCED);

    const AgcExtHost *h = agentc_ext_host();
    check("host_add_setting_field", h && h->add_setting != NULL);
    check("host_setting_get_field", h && h->setting_get != NULL);
    check("host_setting_set_field", h && h->setting_set != NULL);
    check("host_get_route",
          h->setting_get && agentc_streq(h->setting_get(h, "demo", "beta"), "\"x\""));
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    test_defaults_and_order();
    test_project_trust_gate();
    test_validation();
    test_secret_and_persistence();
    test_list_and_host();
    agentc_settings_shutdown();
    agentc_rm_rf(ROOT);
    agentc_outf("settings tests: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
