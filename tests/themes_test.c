/* themes_test.c — named theme resolution and precedence.
 *
 * Precedence under test: built-in base -> <config>/agentc/theme.jsonc -> the
 * named file, most specific winning slot by slot. Everything under /tmp.
 */
#include "agentc.h"
#include "config.h"
#include "base/limits.h"
#include "core/prompt.h"
#include "tui/theme.h"

/* internal helpers (not in the frozen headers) */
void agentc_test_setenv(const char *name, const char *value);
void agentc_test_clearenv(void);
void agentc_rm_rf(const char *path);
int agentc_write_file_atomic(const char *path, const void *data, size_t len, int mode);

#define ROOT "/tmp/agentc-themes-test"
#define CONFDIR ROOT "/config"
#define PROJ ROOT "/proj"
#define EXTRA ROOT "/extra-themes"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = agentc_strlen(s), fl = agentc_strlen(suffix);
    return sl >= fl && agentc_memeq(s + sl - fl, suffix, fl);
}

static void wf(const char *path, const char *text) {
    if (agentc_write_file_atomic(path, text, agentc_strlen(text), 0644) != 0)
        agentc_logf(3, "cannot write %s", path);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    agentc_test_clearenv();
    agentc_test_setenv("HOME", ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", CONFDIR);
    agentc_rm_rf(ROOT);

    /* legacy single-file override */
    wf(CONFDIR "/agentc/theme.jsonc", "{ \"accent\": \"#111111\", \"muted\": \"#222222\" }\n");
    /* named themes: sol switches the base and sets two slots; dual is shadowed
     * by the extra root later */
    wf(CONFDIR "/agentc/themes/sol.jsonc",
       "{ \"base\": \"light\", \"accent\": \"#333333\", \"thinking\": \"#0F0F0F\" }\n");
    wf(CONFDIR "/agentc/themes/dual.jsonc", "{ \"accent\": \"#444444\" }\n");
    wf(EXTRA "/dual.jsonc", "{ \"accent\": \"#555555\" }\n");
    wf(EXTRA "/extra1.jsonc", "{ \"base\": \"light\", \"ok\": \"#00FF00\" }\n");
    /* invalid stems are skipped with a warning */
    wf(CONFDIR "/agentc/themes/Bad Name.jsonc", "{}\n");
    wf(CONFDIR "/agentc/themes/.jsonc", "{}\n");
    wf(CONFDIR "/agentc/themes/..jsonc", "{}\n");

    /* --------------------------------------------------- base and precedence */
    Theme t;
    theme_init(&t, 1, false);
    check("base.dark", t.dark == 1 && t.rgb[TH_ACCENT] == 0x89B4FA);
    check("base.force_bg_off", !t.force_bg);
    check("init.config_load", (theme_init(&t, 1, true), t.dark == 1 && t.rgb[TH_ACCENT] == 0x111111));

    check("apply.dark", theme_apply_named(&t, "dark"));
    check("apply.dark.force_bg", t.force_bg);
    check("precedence.config_over_base",
          t.dark == 1 && t.rgb[TH_ACCENT] == 0x111111 && t.rgb[TH_MUTED] == 0x222222);
    check("apply.sol", theme_apply_named(&t, "sol"));
    check("apply.sol.force_bg_off", !t.force_bg);
    check("precedence.named_over_config",
          t.dark == 0 && t.rgb[TH_ACCENT] == 0x333333 && t.rgb[TH_MUTED] == 0x222222);
    check("precedence.slot_override", t.rgb[TH_THINKING] == 0x0F0F0F);
    check("apply.light", theme_apply_named(&t, "light"));
    check("apply.light.force_bg", t.force_bg);
    check("precedence.light_base", t.dark == 0 && t.rgb[TH_ACCENT] == 0x111111);

    /* ------------------------------------------------------ COLORFGBG system */
    agentc_test_setenv("COLORFGBG", "15;0");
    check("system.dark", theme_apply_named(&t, "system") && t.dark == 1);
    agentc_test_setenv("COLORFGBG", "0;15");
    check("system.light", theme_apply_named(&t, "system") && t.dark == 0);
    agentc_test_setenv("COLORFGBG", "9");
    check("system.ansi_light", theme_apply_named(&t, "system") && t.dark == 0);
    agentc_test_setenv("COLORFGBG", "bogus");
    check("system.fallback_dark", theme_apply_named(&t, "system") && t.dark == 1);
    /* a long digit run is out of range and must degrade to dark instead of
     * overflowing the accumulator; leading zeros within three digits still
     * parse (015 is index 15) */
    agentc_test_setenv("COLORFGBG", "0;999999999999999999999999");
    check("system.long_digits_dark", theme_apply_named(&t, "system") && t.dark == 1);
    agentc_test_setenv("COLORFGBG", "0000000000000000000000007");
    check("system.long_run_dark", theme_apply_named(&t, "system") && t.dark == 1);
    agentc_test_setenv("COLORFGBG", "0;015");
    check("system.leading_zero_index", theme_apply_named(&t, "system") && t.dark == 0);
    agentc_test_setenv("COLORFGBG", NULL);
    check("system.unset_dark", theme_apply_named(&t, "system") && t.dark == 1);
    check("system.force_bg_off", !t.force_bg);

    /* ------------------------------------------------- unknown and invalid */
    check("apply.dark.again", theme_apply_named(&t, "dark"));
    check("unknown.keeps_theme",
          !theme_apply_named(&t, "nope") && t.dark == 1 && t.rgb[TH_ACCENT] == 0x111111);
    /* the unknown-name log fires once per process; this second call stays quiet */
    check("unknown.again", !theme_apply_named(&t, "also-nope"));

    char buf[4096];
    check("path.empty", agentc_theme_path("", buf, sizeof buf) == -22);
    check("path.dot", agentc_theme_path(".", buf, sizeof buf) == -22);
    check("path.dotdot", agentc_theme_path("..", buf, sizeof buf) == -22);
    check("path.sep", agentc_theme_path("a/b", buf, sizeof buf) == -22);
    check("path.bslash", agentc_theme_path("a\\b", buf, sizeof buf) == -22);
    check("path.space", agentc_theme_path("a b", buf, sizeof buf) == -22);
    check("path.missing", agentc_theme_path("nope", buf, sizeof buf) == -2);
    char long_name[AGENTC_THEME_NAME_MAX + 2];
    for (size_t i = 0; i < sizeof long_name - 1; i++) long_name[i] = 'a';
    long_name[sizeof long_name - 1] = 0;
    check("path.too_long", agentc_theme_path(long_name, buf, sizeof buf) == -22);
    check("path.config", agentc_theme_path("sol", buf, sizeof buf) == 0 &&
                             ends_with(buf, CONFDIR "/agentc/themes/sol.jsonc"));
    check("path.config_missing", agentc_theme_path("extra1", buf, sizeof buf) == -2);

    /* the resources_discover extra root is more specific than the config dir */
    agentc_resource_add_theme_root(EXTRA);
    check("path.extra_override", agentc_theme_path("dual", buf, sizeof buf) == 0 &&
                                     ends_with(buf, EXTRA "/dual.jsonc"));
    check("apply.extra_over_config", theme_apply_named(&t, "dual") && t.rgb[TH_ACCENT] == 0x555555);
    check("apply.extra_base", theme_apply_named(&t, "extra1") && t.dark == 0 &&
                                  t.rgb[TH_OK] == 0x00FF00);

    /* ------------------------------------------------------------- listing */
    const char *const *roots = NULL;
    size_t nroots = agentc_resource_theme_roots(&roots);
    AgcThemeEntry *list = NULL;
    size_t n = agentc_themes_load(PROJ, true, roots, nroots, &list, AGENTC_THEMES_MAX);
    check("load.count", n == 3);
    check("load.sorted", n == 3 && agentc_streq(list[0].name, "dual") &&
                             agentc_streq(list[1].name, "extra1") &&
                             agentc_streq(list[2].name, "sol"));
    check("load.extra_override", n == 3 && ends_with(list[0].path, EXTRA "/dual.jsonc"));
    agentc_themes_free(list, n);

    /* project scope is trust-gated */
    wf(PROJ "/.agentc/themes/proj.jsonc", "{ \"accent\": \"#070809\" }\n");
    list = NULL;
    n = agentc_themes_load(PROJ, false, roots, nroots, &list, AGENTC_THEMES_MAX);
    check("load.untrusted", n == 3);
    agentc_themes_free(list, n);
    list = NULL;
    n = agentc_themes_load(PROJ, true, roots, nroots, &list, AGENTC_THEMES_MAX);
    check("load.trusted", n == 4 && agentc_streq(list[0].name, "dual") &&
                              agentc_streq(list[3].name, "sol"));
    agentc_themes_free(list, n);

    /* the app's resolved verdict feeds theme_set_project_root, which is what
     * lets `/theme <name>` resolve a project theme; untrusted clears it
     * (the old TUI re-derivation read an --approve/hook-only project as
     * untrusted and left the theme unreachable). */
    theme_set_project_root(PROJ, true);
    check("apply.project_trusted", theme_apply_named(&t, "proj") &&
                                       t.rgb[TH_ACCENT] == 0x070809);
    theme_set_project_root(PROJ, false);
    check("apply.project_untrusted", !theme_apply_named(&t, "proj"));
    theme_set_project_root(NULL, false);

    /* cap: 70 extra valid files push the result to the 64-entry limit */
    for (int i = 0; i < 70; i++) {
        char path[128];
        agentc_snprintf(path, sizeof path, CONFDIR "/agentc/themes/cap%02d.jsonc", i);
        wf(path, "{}\n");
    }
    list = NULL;
    n = agentc_themes_load(PROJ, true, roots, nroots, &list, AGENTC_THEMES_MAX);
    check("load.cap", n == 64 && agentc_streq(list[0].name, "cap00") &&
                          agentc_streq(list[63].name, "cap63"));
    agentc_themes_free(list, n);

    agentc_rm_rf(ROOT);
    agentc_test_clearenv();
    return fails;
}
