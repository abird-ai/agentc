/* resources_test.c — context-file ordering, skill discovery/validation,
 * prompt templates, resource roots and the prompt registry. Everything under
 * /tmp. */
#include "agentc.h"
#include "config.h"
#include "ext.h"
#include "wire.h"
#include "base/limits.h"
#include "app/project.h"
#include "core/prompt.h"
#include "core/prompts.h"

/* internal helpers (not in the frozen headers) */
void agentc_test_setenv(const char *name, const char *value);
void agentc_test_clearenv(void);
void agentc_rm_rf(const char *path);
int agentc_write_file_atomic(const char *path, const void *data, size_t len, int mode);

#define ROOT "/tmp/agentc-res-test"
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

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = agentc_strlen(s), fl = agentc_strlen(suffix);
    return sl >= fl && agentc_memeq(s + sl - fl, suffix, fl);
}

static void wf(const char *path, const char *text) {
    if (agentc_write_file_atomic(path, text, agentc_strlen(text), 0644) != 0)
        agentc_logf(3, "cannot write %s", path);
}

/* prompt-registry expand fixture */
static char *reg_expand(void *ud, const char *args) {
    (void)ud;
    AgcBuf b = { 0 };
    agentc_buf_printf(&b, "reg:%s", args ? args : "");
    return (char *)b.p;
}

/* ------------------------------------- resources_discover hook fixture */
/* entry 0 of promptPaths is valid, then a run of invalid paths burns the
 * per-list entry budget, then a final valid path sits past the cap: the cap is
 * checked before validation, so that last path must never be validated or
 * consumed (and the invalid run must stop warning at the cap). */
static int res_hook(void *ud, const char *point, const char *payload, char **result_json) {
    (void)ud;
    (void)point;
    (void)payload;
    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "skillPaths");
    agentc_jsonw_arr(&w);
    agentc_jsonw_cstr(&w, CONFDIR "/agentc/extra-skills");
    agentc_jsonw_cstr(&w, PROJ "/.agentc/extra");
    agentc_jsonw_cstr(&w, PROJ "/../evil");
    agentc_jsonw_cstr(&w, "/outside/skills");
    agentc_jsonw_cstr(&w, "relative");
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "promptPaths");
    agentc_jsonw_arr(&w);
    agentc_jsonw_cstr(&w, CONFDIR "/agentc/extra-prompts");
    for (int i = 0; i <= AGENTC_RESOURCE_PATHS_MAX - 2; i++) {
        char evil[160];
        agentc_snprintf(evil, sizeof evil, PROJ "/../cap-%d", i);
        agentc_jsonw_cstr(&w, evil);
    }
    agentc_jsonw_cstr(&w, CONFDIR "/agentc/extra-prompts-cap");
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "themePaths");
    agentc_jsonw_arr(&w);
    agentc_jsonw_cstr(&w, CONFDIR "/agentc/extra-themes");
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    *result_json = agentc_strdup((const char *)b.p);
    agentc_buf_free(&b);
    return 1;
}

static int res_ext_init(const AgcExtHost *host) {
    host->on("resources_discover", AGENTC_HOOK_OVERRIDE, 0, res_hook, NULL);
    return 0;
}

/* The first handler contributes a valid path, the second fails
 * (fail-closed). The accumulated result must be discarded wholesale: a blocked
 * emit still carries the earlier handler's accumulator. */
static int res_blocked_hook(void *ud, const char *point, const char *payload,
                            char **result_json) {
    (void)ud;
    (void)point;
    (void)payload;
    *result_json = agentc_strdup(
        "{\"skillPaths\":[\"" CONFDIR "/agentc/blocked-skills\"]}");
    return 1;
}

static int res_fail_hook(void *ud, const char *point, const char *payload,
                         char **result_json) {
    (void)ud;
    (void)point;
    (void)payload;
    if (result_json) *result_json = NULL;
    return -1;
}

static const AgcExt g_res_ext = {
    .abi_version = AGENTC_EXT_ABI,
    .struct_size = sizeof(AgcExt),
    .name = "resources-fixture",
    .version = "1",
    .order = 0,
    .init = res_ext_init,
};

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    agentc_test_clearenv();
    agentc_test_setenv("HOME", ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", CONFDIR);
    agentc_test_setenv("XDG_DATA_HOME", ROOT "/data");
    agentc_rm_rf(ROOT);

    /* ------------------------------------------------------- context files */
    wf(CONFDIR "/agentc/AGENTC.md", "config-agentc\n");
    wf(CONFDIR "/agentc/AGENTS.override.md", "config-override\n");
    wf(CONFDIR "/agentc/AGENTS.md", "config-agents\n");
    wf(CONFDIR "/agentc/CLAUDE.md", "config-claude\n");
    wf(PROJ "/AGENTS.md", "root-agents\n");
    wf(PROJ "/CLAUDE.md", "root-claude\n");
    wf(PROJ "/a/AGENTC.md", "a-agentc\n");
    wf(PROJ "/a/b/AGENTC.md", "b-agentc\n");
    wf(PROJ "/a/b/AGENTS.md", "b-agents\n");
    wf(ROOT "/other/AGENTS.md", "other\n");

    AgcContextFiles *cf = agentc_context_files_load(PROJ "/a/b", true);
    check("context_count", cf && cf->n == 9);
    bool order = cf && cf->n == 9 &&
                 ends_with(cf->paths[0], CONFDIR "/agentc/AGENTC.md") &&
                 ends_with(cf->paths[1], CONFDIR "/agentc/AGENTS.override.md") &&
                 ends_with(cf->paths[2], CONFDIR "/agentc/AGENTS.md") &&
                 ends_with(cf->paths[3], CONFDIR "/agentc/CLAUDE.md") &&
                 ends_with(cf->paths[4], PROJ "/AGENTS.md") &&
                 ends_with(cf->paths[5], PROJ "/CLAUDE.md") &&
                 ends_with(cf->paths[6], PROJ "/a/AGENTC.md") &&
                 ends_with(cf->paths[7], PROJ "/a/b/AGENTC.md") &&
                 ends_with(cf->paths[8], PROJ "/a/b/AGENTS.md");
    check("context_order", order);
    check("context_texts",
          cf && cf->n == 9 && agentc_streq(cf->texts[0], "config-agentc\n") &&
              agentc_streq(cf->texts[7], "b-agentc\n") && cf->lens[8] == 9);

    AgcContextFiles *untrusted = agentc_context_files_load(PROJ "/a/b", false);
    check("context_untrusted", untrusted && untrusted->n == 4 &&
                                   ends_with(untrusted->paths[3], CONFDIR "/agentc/CLAUDE.md"));
    agentc_context_files_free(untrusted);

    AgcBuf sys = { 0 };
    agentc_buf_cstr(&sys, "BASE\n");
    agentc_prompt_add_project_context(&sys, cf);
    const char *s = (const char *)sys.p;
    size_t wrappers = 0;
    for (const char *p = s; (p = agentc_str_str(p, "<project_instructions")); p++) wrappers++;
    check("context_prompt_wrappers", wrappers == 9);
    check("context_prompt_path",
          contains(s, "<project_instructions path=\"" CONFDIR "/agentc/AGENTC.md\">") &&
              contains(s, "config-agentc") && contains(s, "</project_instructions>"));
    agentc_context_files_free(cf);
    agentc_buf_clear(&sys);

    /* ---------------------------------------------------------------- skills */
    wf(CONFDIR "/agentc/skills/one/SKILL.md",
       "---\nname: one\ndescription: One skill\n---\nbody one\n");
    wf(CONFDIR "/agentc/skills/two/SKILL.md", "---\nname: two\n---\nbody two\n");
    wf(CONFDIR "/agentc/skills/bad/SKILL.md", "---\nname: Bad_Name\n---\nbody bad\n");
    wf(CONFDIR "/agentc/skills/nested/inner/SKILL.md", "---\nname: inner\n---\nbody inner\n");
    wf(CONFDIR "/agentc/skills/loose.md",
       "---\nname: loose\ndescription: Loose skill\n---\nbody\n");
    wf(CONFDIR "/agentc/skills/esc/SKILL.md",
       "---\nname: esc\ndescription: A & B <c>\n---\nbody\n");
    wf(PROJ "/.agentc/skills/proj/SKILL.md", "---\nname: proj\ndescription: Project skill\n---\nbody\n");

    AgcSkill *skills = NULL;
    size_t nskills = agentc_skills_load(PROJ, true, &skills, 64);
    check("skills_count", nskills == 7);
    bool names = nskills == 7 && agentc_streq(skills[0].name, "Bad_Name") &&
                 agentc_streq(skills[1].name, "esc") && agentc_streq(skills[2].name, "inner") &&
                 agentc_streq(skills[3].name, "loose") && agentc_streq(skills[4].name, "one") &&
                 agentc_streq(skills[5].name, "proj") && agentc_streq(skills[6].name, "two");
    check("skills_names", names);
    check("skills_description",
          nskills == 7 && agentc_streq(skills[4].description, "One skill") &&
              agentc_streq(skills[6].description, "") &&
              agentc_streq(skills[5].description, "Project skill"));
    check("skills_location", nskills == 7 && ends_with(skills[4].path, "/skills/one/SKILL.md") &&
                                 ends_with(skills[5].path, PROJ "/.agentc/skills/proj/SKILL.md"));
    check("skills_invalid_still_loaded", nskills == 7 && agentc_streq(skills[0].name, "Bad_Name"));

    agentc_prompt_add_skills(&sys, skills, nskills);
    s = (const char *)sys.p;
    check("skills_prompt_block",
          contains(s, "<available_skills>") && contains(s, "</available_skills>") &&
              contains(s, "<name>one</name>") && contains(s, "<name>proj</name>") &&
              contains(s, "<location>") && contains(s, "/skills/one/SKILL.md</location>"));
    check("skills_prompt_escaped", contains(s, "A &amp; B &lt;c&gt;"));
    agentc_skills_free(skills, nskills);
    agentc_buf_clear(&sys);

    /* an untrusted project contributes no skills to the system prompt */
    AgcSkill *untr = NULL;
    size_t nuntr = agentc_skills_load(PROJ, false, &untr, 64);
    check("skills_untrusted", nuntr == 6);
    agentc_skills_free(untr, nuntr);

    /* A lone-quote frontmatter value must not strip past the value: the
     * single-quote name and double-quote description are one byte each, so an
     * unconditional v++/ve-- would underflow (ve - v == SIZE_MAX) and copy off
     * the heap. The quote is kept verbatim, hence the invalid-name warning. */
    wf(CONFDIR "/agentc/quote-skills/q/SKILL.md",
       "---\nname: '\ndescription: \"\n---\nquoted body\n");
    {
        const char *qroots[] = { CONFDIR "/agentc/quote-skills" };
        AgcSkill *q = NULL;
        size_t nq = agentc_skills_load_extra(PROJ, false, qroots, 1, &q, 64);
        check("skills_quote_no_underflow",
              nq == 7 && agentc_streq(q[0].name, "'") &&
                  agentc_streq(q[0].description, "\""));
        agentc_skills_free(q, nq);
    }

    /* --------------------------------------------- resources_discover */
    wf(CONFDIR "/agentc/extra-skills/extra/SKILL.md",
       "---\nname: extra\ndescription: Extra skill\n---\nbody extra\n");
    wf(PROJ "/.agentc/extra/projextra/SKILL.md",
       "---\nname: projextra\ndescription: Project extra\n---\nbody\n");
    wf(CONFDIR "/agentc/extra-prompts/extra.md",
       "---\ndescription: Extra prompt\nargument-hint: \"[x]\"\n---\nextra $1\n");
    wf(CONFDIR "/agentc/extra-themes/x.jsonc",
       "{ \"base\": \"light\", \"accent\": \"#010203\" }\n");
    wf(CONFDIR "/agentc/themes/sol.jsonc", "{ \"accent\": \"#040506\" }\n");
    wf(CONFDIR "/agentc/themes/Bad Name.jsonc", "{}\n");
    wf(PROJ "/.agentc/themes/proj.jsonc", "{ \"accent\": \"#070809\" }\n");
    wf(PROJ "/.agentc/themes/sol.jsonc", "{ \"accent\": \"#0A0B0C\" }\n");

    /* the path gate: absolute, no "..", config dir always, cwd when trusted */
    check("path.config_always",
          agentc_project_validate_path(PROJ, false, CONFDIR "/agentc/extra-skills") == 0);
    check("path.project_trusted",
          agentc_project_validate_path(PROJ, true, PROJ "/.agentc/extra") == 0);
    check("path.project_untrusted",
          agentc_project_validate_path(PROJ, false, PROJ "/.agentc/extra") != 0);
    check("path.relative", agentc_project_validate_path(PROJ, true, "skills") != 0);
    check("path.dotdot", agentc_project_validate_path(PROJ, true, PROJ "/../evil") != 0);
    check("path.outside", agentc_project_validate_path(PROJ, true, ROOT "/other/skills") != 0);
    check("path.prefix_boundary",
          agentc_project_validate_path(PROJ, true, PROJ "-evil/skills") != 0);
    check("path.empty", agentc_project_validate_path(PROJ, true, "") != 0);

    const char *extra_roots[] = { CONFDIR "/agentc/extra-skills" };
    AgcSkill *ex = NULL;
    size_t nex = agentc_skills_load_extra(PROJ, true, extra_roots, 1, &ex, 64);
    bool ex_ok = false;
    for (size_t i = 0; i < nex; i++)
        if (agentc_streq(ex[i].name, "extra")) ex_ok = true;
    check("skills_extra_root", nex == 8 && ex_ok);
    agentc_skills_free(ex, nex);

    /* end to end: the hook result is merged, gated and consumed into the
     * per-kind root stores; the skill block still renders. */
    agentc_ext_register(agentc_builtin_context_ext());
    agentc_ext_register(&g_res_ext);
    agentc_ext_load_all();
    agentc_builtin_context_set(PROJ, true);
    agentc_project_apply_resources(PROJ, true);

    const char *const *sroots = NULL;
    const char *const *proots = NULL;
    const char *const *troots = NULL;
    size_t nsroots = agentc_resource_skill_roots(&sroots);
    size_t nproots = agentc_resource_prompt_roots(&proots);
    size_t ntroots = agentc_resource_theme_roots(&troots);
    check("roots.skill_store",
          nsroots == 2 && agentc_streq(sroots[0], CONFDIR "/agentc/extra-skills") &&
              agentc_streq(sroots[1], PROJ "/.agentc/extra"));
    /* 1 valid + 31 invalid + 1 valid promptPaths: the 33rd entry trips the cap
     * before validation, so the trailing extra-prompts-cap is never consumed
     * (a post-validation cap would consume it and leave two roots) */
    bool cap_path_seen = false;
    for (size_t i = 0; i < nproots; i++)
        if (agentc_streq(proots[i], CONFDIR "/agentc/extra-prompts-cap")) cap_path_seen = true;
    check("roots.prompt_dedup",
          nproots == 1 && agentc_streq(proots[0], CONFDIR "/agentc/extra-prompts"));
    check("roots.prompt_cap_before_validate", !cap_path_seen);
    check("roots.theme_store",
          ntroots == 1 && agentc_streq(troots[0], CONFDIR "/agentc/extra-themes"));

    /* `/skill:<name>` body lookup: frontmatter stripped, unknown NULL. The
     * builtin-context cwd/trust and the skill root store are set by now. */
    size_t blen = 0;
    char *sbody = agentc_builtin_context_skill_body("extra", &blen);
    check("skill_body.read", sbody && blen == 10 && agentc_streq(sbody, "body extra"));
    agentc_free(sbody);
    check("skill_body.unknown", agentc_builtin_context_skill_body("nope", &blen) == NULL);

    /* theme name validation and resolution */
    char tbuf[4096];
    check("theme_path.extra",
          agentc_theme_path("x", tbuf, sizeof tbuf) == 0 &&
              ends_with(tbuf, "/agentc/extra-themes/x.jsonc"));
    check("theme_path.config",
          agentc_theme_path("sol", tbuf, sizeof tbuf) == 0 &&
              ends_with(tbuf, "/agentc/themes/sol.jsonc"));
    check("theme_path.empty", agentc_theme_path("", tbuf, sizeof tbuf) == -22);
    check("theme_path.dot", agentc_theme_path(".", tbuf, sizeof tbuf) == -22);
    check("theme_path.dotdot", agentc_theme_path("..", tbuf, sizeof tbuf) == -22);
    check("theme_path.sep", agentc_theme_path("a/b", tbuf, sizeof tbuf) == -22);
    check("theme_path.bslash", agentc_theme_path("a\\b", tbuf, sizeof tbuf) == -22);
    check("theme_path.missing", agentc_theme_path("nope", tbuf, sizeof tbuf) == -2);

    /* theme listing: config + trusted project + the extra root, later wins,
     * invalid stems skipped, sorted by name */
    AgcThemeEntry *themes = NULL;
    size_t nth = agentc_themes_load(PROJ, true, troots, ntroots, &themes, AGENTC_THEMES_MAX);
    check("themes_count", nth == 3);
    check("themes_sorted",
          nth == 3 && agentc_streq(themes[0].name, "proj") &&
              agentc_streq(themes[1].name, "sol") && agentc_streq(themes[2].name, "x"));
    check("themes_later_wins", nth == 3 && ends_with(themes[1].path, PROJ "/.agentc/themes/sol.jsonc"));
    agentc_themes_free(themes, nth);
    themes = NULL;
    nth = agentc_themes_load(PROJ, false, troots, ntroots, &themes, AGENTC_THEMES_MAX);
    check("themes_untrusted", nth == 2 && agentc_streq(themes[0].name, "sol") &&
                                  agentc_streq(themes[1].name, "x"));
    agentc_themes_free(themes, nth);

    /* root store cap and dedup: distinct roots stop at the cap (with a log),
     * a repeated root is a silent no-op */
    for (int i = 0; i < AGENTC_RESOURCE_ROOTS_MAX + 2; i++) {
        char dir[128];
        agentc_snprintf(dir, sizeof dir, CONFDIR "/agentc/cap-%d", i);
        agentc_resource_add_theme_root(dir);
    }
    check("roots.cap", agentc_resource_theme_roots(NULL) == AGENTC_RESOURCE_ROOTS_MAX);
    agentc_resource_add_theme_root(troots[0]);
    check("roots.dedup", agentc_resource_theme_roots(NULL) == AGENTC_RESOURCE_ROOTS_MAX);

    AgcBuf sb = { 0 };
    agentc_ext_prompt_build(&sb, NULL, 0);
    const char *sp = (const char *)sb.p;
    check("res.skill_paths_consumed",
          contains(sp, "<name>extra</name>") && contains(sp, "<name>projextra</name>"));
    check("res.rejected_kept_out", !contains(sp, "evil") && !contains(sp, "outside"));
    agentc_buf_free(&sb);
    agentc_ext_shutdown();

    /* a fail-closed handler failure discards the whole chain: the earlier
     * handler's path must not be applied (and must not persist as an extra root
     * for later prompt builds either) */
    wf(CONFDIR "/agentc/blocked-skills/blocked/SKILL.md",
       "---\nname: blocked\ndescription: Blocked skill\n---\nbody\n");
    agentc_ext_register(agentc_builtin_context_ext());
    uint64_t rbh = agentc_ext_host()->on("resources_discover", AGENTC_HOOK_OVERRIDE, 0,
                                         res_blocked_hook, NULL);
    uint64_t rfh = agentc_ext_host()->on("resources_discover", AGENTC_HOOK_OVERRIDE, 0,
                                         res_fail_hook, NULL);
    agentc_ext_load_all();
    agentc_builtin_context_set(PROJ, true);
    agentc_project_apply_resources(PROJ, true);
    AgcBuf sb2 = { 0 };
    agentc_ext_prompt_build(&sb2, NULL, 0);
    check("res.blocked_not_consumed", !contains((const char *)sb2.p, "<name>blocked</name>"));
    agentc_buf_free(&sb2);
    agentc_ext_host()->off(rbh);
    agentc_ext_host()->off(rfh);
    agentc_ext_shutdown();

    /* ------------------------------------------------------------- templates */
    wf(CONFDIR "/agentc/prompts/greet.md",
       "---\ndescription: Greets people\nargument-hint: \"[name]\"\n---\n"
       "Hello $1 and $2. all=$@ args=$ARGUMENTS def=${1:-world} ${2:-there} slice=${@:2}"
       " braced=${ARGUMENTS} at=${@}\n");
    wf(CONFDIR "/agentc/prompts/only.md", "---\ndescription: Only args\n---\nrun ${@:-nothing} end\n");
    wf(PROJ "/.agentc/prompts/proj.md", "Project prompt\nsecond line\n");
    wf(CONFDIR "/agentc/prompts/Bad Name.md", "skipped\n");

    AgcPromptTemplate *templates = NULL;
    size_t ntpl = agentc_templates_load(PROJ, true, &templates, 64);
    check("templates_count", ntpl == 3);
    bool tnames = ntpl == 3 && agentc_streq(templates[0].name, "greet") &&
                  agentc_streq(templates[1].name, "only") && agentc_streq(templates[2].name, "proj");
    check("templates_names", tnames);
    check("templates_descriptions",
          ntpl == 3 && agentc_streq(templates[0].description, "Greets people") &&
              agentc_streq(templates[2].description, "Project prompt"));
    check("templates_paths", ntpl == 3 && ends_with(templates[0].path, "/prompts/greet.md"));
    check("templates_hint", ntpl == 3 && agentc_streq(templates[0].hint, "[name]") &&
                                agentc_streq(templates[1].hint, ""));

    if (ntpl == 3) {
        char *e = agentc_template_expand(&templates[0], "Alice Bob");
        check("expand_positional", agentc_streq(e, "Hello Alice and Bob. all=Alice Bob args=Alice Bob "
                                                "def=Alice Bob slice=Bob braced=Alice Bob at=Alice Bob"));
        agentc_free(e);
        e = agentc_template_expand(&templates[0], "");
        check("expand_defaults", agentc_streq(e, "Hello  and . all= args= def=world there slice= braced= at="));
        agentc_free(e);
        e = agentc_template_expand(&templates[0], "\"Alice Smith\" Bob");
        check("expand_quoted", agentc_streq(e, "Hello Alice Smith and Bob. all=Alice Smith Bob "
                                           "args=Alice Smith Bob def=Alice Smith Bob slice=Bob "
                                           "braced=Alice Smith Bob at=Alice Smith Bob"));
        agentc_free(e);
        e = agentc_template_expand(&templates[1], "");
        check("expand_fallback_empty", agentc_streq(e, "run nothing end"));
        agentc_free(e);
        e = agentc_template_expand(&templates[1], "x y");
        check("expand_fallback_args", agentc_streq(e, "run x y end"));
        agentc_free(e);
    }
    agentc_templates_free(templates, ntpl);

    AgcPromptTemplate *tuntr = NULL;
    size_t ntuntr = agentc_templates_load(PROJ, false, &tuntr, 64);
    check("templates_untrusted", ntuntr == 2);
    agentc_templates_free(tuntr, ntuntr);

    /* extra-root form sees the resources_discover prompt root */
    AgcPromptTemplate *tx = NULL;
    size_t ntx = agentc_templates_load_extra(PROJ, true, proots, nproots, &tx, 64);
    check("templates_extra_root", ntx == 4 && agentc_streq(tx[0].name, "extra") &&
                                        agentc_streq(tx[3].name, "proj"));
    agentc_templates_free(tx, ntx);

    /* ----------------------------------------------------- prompt registry */
    check("prompts.file_load", agentc_prompts_load_file_templates(PROJ, true) == 4);
    check("prompts.has_file", agentc_prompts_has("greet") && agentc_prompts_has("extra") &&
                                  !agentc_prompts_has("Bad Name"));
    char *pe = agentc_prompts_expand("greet", "Alice Bob");
    check("prompts.expand_file",
          pe && agentc_streq(pe, "Hello Alice and Bob. all=Alice Bob args=Alice Bob "
                                 "def=Alice Bob slice=Bob braced=Alice Bob at=Alice Bob"));
    agentc_free(pe);
    pe = agentc_prompts_expand("extra", "one");
    check("prompts.expand_extra", pe && agentc_streq(pe, "extra one"));
    agentc_free(pe);
    check("prompts.unknown_expand", agentc_prompts_expand("nope", "") == NULL);
    check("prompts.list_count", agentc_prompts_list(NULL, 0) == 4);

    AgcPromptInfo pinfo[8];
    size_t npinfo = agentc_prompts_list(pinfo, 8);
    check("prompts.list_views", npinfo == 4 && agentc_streq(pinfo[1].name, "greet") &&
                                     agentc_streq(pinfo[1].hint, "[name]"));

    check("prompts.invalid_name",
          agentc_prompts_register("Bad_Name", "", "", "test", NULL, reg_expand) == -22);
    check("prompts.register",
          agentc_prompts_register("ok", "Ok prompt", "", "test", NULL, reg_expand) == 0 &&
              agentc_prompts_has("ok"));
    pe = agentc_prompts_expand("ok", "x");
    check("prompts.expand_registered", pe && agentc_streq(pe, "reg:x"));
    agentc_free(pe);
    /* later same-name registration wins; the retired record stays allocated */
    check("prompts.reregister",
          agentc_prompts_register("ok", "Ok v2", "", "test", NULL, reg_expand) == 0 &&
              agentc_prompts_list(NULL, 0) == 5);
    check("prompts.remove", agentc_prompts_remove("ok") && !agentc_prompts_has("ok") &&
                                !agentc_prompts_remove("ok"));
    check("prompts.register_source",
          agentc_prompts_register("mcp1", "", "", "mcp", NULL, reg_expand) == 0 &&
              agentc_prompts_register("mcp2", "", "", "mcp", NULL, reg_expand) == 0 &&
              agentc_prompts_register("other", "", "", "ext", NULL, reg_expand) == 0);
    check("prompts.clear_source", agentc_prompts_clear_source("mcp") == 2 &&
                                       !agentc_prompts_has("mcp1") &&
                                       agentc_prompts_has("other"));
    check("prompts.live_after", agentc_prompts_list(NULL, 0) == 5);

    /* cap: the store never grows past AGENTC_PROMPTS_MAX, but dead (removed)
     * records are reusable, so the cap is on LIVE registrations. Four dead
     * slots exist here (ok v1, ok v2, mcp1, mcp2); 503 fresh names fill the
     * store, then those four are reused, and the next fresh name is refused. */
    size_t filled = 0;
    char fname[32];
    int last = 0;
    for (int i = 0; i < AGENTC_PROMPTS_MAX + 8; i++) {
        agentc_snprintf(fname, sizeof fname, "fill%03d", i);
        last = agentc_prompts_register(fname, "", "", "fill", NULL, reg_expand);
        if (last != 0) break;
        filled++;
    }
    check("prompts.cap", filled == (size_t)AGENTC_PROMPTS_MAX - 5 && last == -28);
    check("prompts.cap_full", agentc_prompts_list(NULL, 0) == AGENTC_PROMPTS_MAX);

    agentc_rm_rf(ROOT);
    agentc_test_clearenv();
    return fails;
}
