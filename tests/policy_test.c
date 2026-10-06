/* policy_test.c — the app's active-tool policy (src/app/policy.c).
 *
 * Offline, no agent: exercises the active-tool selection (--tools / default_tools /
 * all / --no-tools), the strict --tools miss vs. the deferred `mcp__` name
 * and the destructive default that hides non-core destructive tools unless named.
 * Golden mode: every check prints label=0|1; the destructive/deferral diagnostics are
 * part of the golden output too.
 */
#include "agent.h"
#include "config.h"
#include "ext.h"
#include "app/policy.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

static const AgcTool all_tools[] = {
    { .name = "read",      .flags = AGENTC_TOOL_READONLY | AGENTC_TOOL_CORE },
    { .name = "edit",      .flags = AGENTC_TOOL_DESTRUCTIVE | AGENTC_TOOL_CORE },
    { .name = "evil",      .flags = AGENTC_TOOL_DESTRUCTIVE },
    { .name = "safe",      .flags = 0 },
    { .name = "mcp__s__x", .flags = AGENTC_TOOL_DESTRUCTIVE },
    { .name = "ro",        .flags = AGENTC_TOOL_READONLY },
};

static const size_t all_n = sizeof all_tools / sizeof all_tools[0];

static bool has(const AgcTool *t, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (t[i].name && agentc_streq(t[i].name, name)) return true;
    return false;
}

static int run(const ToolPolicy *pol, bool strict, AgcTool **out, size_t *n) {
    *out = NULL;
    *n = 0;
    return agentc_policy_apply(all_tools, all_n, pol, strict, out, n);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    AgcConfig cfg;
    agentc_memset(&cfg, 0, sizeof cfg);

    /* ------------------------------------------------------ "all" (filter) */
    ToolPolicy all = { &cfg, NULL, false };
    AgcTool *t = NULL;
    size_t n = 0;
    check("all.rc", run(&all, true, &t, &n) == 0);
    check("all.count", n == 4);
    check("all.core_destructive_eligible", has(t, n, "edit"));
    check("all.core_readonly_eligible", has(t, n, "read"));
    check("all.nondestructive_eligible", has(t, n, "safe"));
    check("all.noncore_destructive_hidden", !has(t, n, "evil"));
    check("all.mcp_destructive_hidden", !has(t, n, "mcp__s__x"));
    agentc_free(t);

    /* ------------------------------------- named selection overrides default */
    ToolPolicy named = { &cfg, "evil", false };
    check("named.rc", run(&named, true, &t, &n) == 0);
    check("named.noncore_destructive_selected", n == 1 && has(t, n, "evil"));
    agentc_free(t);

    ToolPolicy named_multi = { &cfg, "read, evil", false };
    check("named.multi", run(&named_multi, true, &t, &n) == 0 && n == 2 &&
                             has(t, n, "read") && has(t, n, "evil"));
    agentc_free(t);

    /* An already-selected name is skipped, in one list and across
     * repeated default_tools rows */
    ToolPolicy named_dup = { &cfg, "read,read, read", false };
    check("named.dedupe", run(&named_dup, true, &t, &n) == 0 && n == 1 && has(t, n, "read"));
    agentc_free(t);

    char *dup_defs[] = { "read", "read", "evil" };
    AgcConfig ddcfg;
    agentc_memset(&ddcfg, 0, sizeof ddcfg);
    ddcfg.default_tools = dup_defs;
    ddcfg.ndefault_tools = 3;
    ToolPolicy ddup = { &ddcfg, NULL, false };
    check("defaults.dedupe", run(&ddup, true, &t, &n) == 0 && n == 2 &&
                                  has(t, n, "read") && has(t, n, "evil"));
    agentc_free(t);

    ToolPolicy named_mcp = { &cfg, "mcp__s__x", false };
    check("named.mcp_present", run(&named_mcp, true, &t, &n) == 0 && n == 1 &&
                                   has(t, n, "mcp__s__x"));
    agentc_free(t);

    /* ------------------------------- default_tools names also override default */
    char *defs[] = { "evil" };
    AgcConfig dcfg;
    agentc_memset(&dcfg, 0, sizeof dcfg);
    dcfg.default_tools = defs;
    dcfg.ndefault_tools = 1;
    ToolPolicy dpol = { &dcfg, NULL, false };
    check("defaults.destructive_named", run(&dpol, true, &t, &n) == 0 && n == 1 &&
                                            has(t, n, "evil"));
    agentc_free(t);

    /* ------------------------------------------------------- --no-tools */
    ToolPolicy notools = { &cfg, NULL, true };
    check("no_tools.clears", run(&notools, true, &t, &n) == 0 && n == 0);
    agentc_free(t);
    ToolPolicy notools_named = { &cfg, "read", true };
    check("no_tools.beats_named", run(&notools_named, true, &t, &n) == 0 && n == 0);
    agentc_free(t);

    /* ------------------------------------------- strict vs deferred misses */
    ToolPolicy unknown = { &cfg, "nosuch", false };
    check("strict.unknown", run(&unknown, true, &t, &n) == -22 && t == NULL && n == 0);
    check("lenient.unknown", run(&unknown, false, &t, &n) == 0 && n == 0);
    agentc_free(t);

    ToolPolicy deferred = { &cfg, "mcp__late__tool", false };
    check("strict.mcp_deferred", run(&deferred, true, &t, &n) == 0 && n == 0);
    agentc_free(t);

    /* the three generic resource tools are published lazily at the
     * first resource-capable list sync, so --tools defers them by exact name;
     * no broad `mcp_` prefix (a typo still exits 2). */
    ToolPolicy res_list = { &cfg, "mcp_list_resources", false };
    check("strict.resource_deferred", run(&res_list, true, &t, &n) == 0 && n == 0);
    agentc_free(t);
    ToolPolicy res_templates = { &cfg, "mcp_list_resource_templates", false };
    check("strict.template_deferred", run(&res_templates, true, &t, &n) == 0 && n == 0);
    agentc_free(t);
    ToolPolicy res_read = { &cfg, "mcp_read_resource", false };
    check("strict.read_deferred", run(&res_read, true, &t, &n) == 0 && n == 0);
    agentc_free(t);
    ToolPolicy res_typo = { &cfg, "mcp_list_resourcess", false };
    check("strict.resource_typo", run(&res_typo, true, &t, &n) == -22 && t == NULL);
    ToolPolicy mcp_prefix = { &cfg, "mcp_other", false };
    check("strict.mcp_prefix_not_deferred", run(&mcp_prefix, true, &t, &n) == -22 && t == NULL);

    /* the deferred-mcp diagnostic must not promise a connection
     * when the mcp extension is not loaded; with it loaded the promise is
     * real. */
    check("deferred.no_ext", !agentc_ext_is_loaded("mcp"));
    agentc_ext_register_defaults(false);
    agentc_ext_load_all();
    check("deferred.ext_loaded", agentc_ext_is_loaded("mcp"));
    /* Descriptor introspection: count-first, registration order, real state. */
    size_t nd = agentc_ext_describe(NULL, 0);
    AgcExtInfo *di = agentc_alloc((nd ? nd : 1) * sizeof *di);
    size_t fd = agentc_ext_describe(di, nd);
    check("describe.count", nd == 3 && fd == 3);
    check("describe.loaded",
          fd == 3 && di[0].state == AGENTC_EXT_STATE_LOADED && di[0].known &&
              agentc_streq(di[0].name, "builtin-tools") &&
              agentc_streq(di[0].version, "1"));
    check("describe.order",
          fd == 3 && agentc_streq(di[1].name, "builtin-context") &&
              agentc_streq(di[2].name, "mcp") &&
              di[2].state == AGENTC_EXT_STATE_LOADED && !di[2].disabled);
    AgcExtInfo one;
    check("describe.bounded",
          agentc_ext_describe(&one, 1) == 1 && agentc_streq(one.name, "builtin-tools"));
    agentc_free(di);
    ToolPolicy deferred_loaded = { &cfg, "mcp__late__tool", false };
    check("strict.mcp_deferred_loaded", run(&deferred_loaded, true, &t, &n) == 0 && n == 0);
    agentc_free(t);
    agentc_ext_shutdown();

    return fails;
}
