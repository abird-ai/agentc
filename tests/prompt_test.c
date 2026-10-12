/* prompt_test.c — dedicated golden for prompt contribution resolution
 * (P6-SETTINGS §6 / R-D29): anchor + position ordering for APPEND, the
 * priority/tie rule for REPLACE, REPLACE suppressing the core default render,
 * and an ALL-level REPLACE suppressing the whole anchor walk. It registers
 * contributions through the real host `add_prompt` slot (owner 0, no extension
 * lifecycle), then assembles with `agentc_prompt_build`.
 *
 * Only `label=1|0` checks are printed (the Environment anchor would otherwise
 * embed the host cwd), so the same expected file holds under Wine.
 */
#include "agent.h"
#include "ext.h"
#include "plat.h"
#include "core/prompt.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

static bool has(const char *hay, const char *needle) {
    return hay != NULL && agentc_str_str(hay, needle) != NULL;
}

/* --------------------------------------------------------------- renderers */
static int r_pre_before(const AgcExtHost *h, void *ud, void *out) {
    (void)ud;
    h->out_write(out, "PRE-BEFORE\n", 11);
    return 0;
}
static int r_tools_after(const AgcExtHost *h, void *ud, void *out) {
    (void)ud;
    h->out_write(out, "TOOLS-AFTER\n", 12);
    return 0;
}
static int r_all_after(const AgcExtHost *h, void *ud, void *out) {
    (void)ud;
    h->out_write(out, "ALL-AFTER\n", 10);
    return 0;
}
static int r_rules_hi(const AgcExtHost *h, void *ud, void *out) {
    (void)ud;
    h->out_write(out, "RULES-HI\n", 9);
    return 0;
}
static int r_rules_lo(const AgcExtHost *h, void *ud, void *out) {
    (void)ud;
    h->out_write(out, "RULES-LO\n", 9);
    return 0;
}
static int r_tools_replace(const AgcExtHost *h, void *ud, void *out) {
    (void)ud;
    h->out_write(out, "TOOLS-REPLACED\n", 15);
    return 0;
}
static int r_all_replace(const AgcExtHost *h, void *ud, void *out) {
    (void)ud;
    h->out_write(out, "ONLY-ALL\n", 9);
    return 0;
}

/* ------------------------------------------------------------- fixtures */
#define PROMPT(key_, prio_, render_, anchor_, pos_, mode_)                                         \
    {sizeof(AgcExtPrompt), (key_), (prio_), NULL, (render_), (anchor_), (pos_), (mode_)}

/* One tool so the default TOOLS anchor has something to render. */
static const AgcTool g_tool = {.name = "demo", .desc = "demo tool"};

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    agentc_log_set_level(4); /* quiet: the REPLACE-loser notice is expected */
    const AgcExtHost *host = agentc_ext_host();

    static const AgcExtPrompt pre = PROMPT("pre", 10, r_pre_before, AGENTC_ANCHOR_PREAMBLE,
                                           AGENTC_PROMPT_BEFORE, AGENTC_PROMPT_APPEND);
    static const AgcExtPrompt tools_after =
        PROMPT("tools-after", 10, r_tools_after, AGENTC_ANCHOR_TOOLS, AGENTC_PROMPT_AFTER,
               AGENTC_PROMPT_APPEND);
    static const AgcExtPrompt all_after = PROMPT("all-after", 20, r_all_after, AGENTC_ANCHOR_ALL,
                                                 AGENTC_PROMPT_AFTER, AGENTC_PROMPT_APPEND);
    static const AgcExtPrompt rules_hi = PROMPT("rules-hi", 10, r_rules_hi, AGENTC_ANCHOR_RULES,
                                                AGENTC_PROMPT_AFTER, AGENTC_PROMPT_REPLACE);
    static const AgcExtPrompt rules_lo = PROMPT("rules-lo", 5, r_rules_lo, AGENTC_ANCHOR_RULES,
                                                AGENTC_PROMPT_AFTER, AGENTC_PROMPT_REPLACE);
    static const AgcExtPrompt tools_rep =
        PROMPT("tools-rep", 10, r_tools_replace, AGENTC_ANCHOR_TOOLS, AGENTC_PROMPT_BEFORE,
               AGENTC_PROMPT_REPLACE);
    static const AgcExtPrompt all_rep = PROMPT("all-rep", 1, r_all_replace, AGENTC_ANCHOR_ALL,
                                               AGENTC_PROMPT_AFTER, AGENTC_PROMPT_REPLACE);

    host->add_prompt(&pre);
    host->add_prompt(&tools_after);
    host->add_prompt(&all_after);
    host->add_prompt(&rules_hi);
    host->add_prompt(&rules_lo);
    host->add_prompt(&tools_rep);

    char *p = agentc_prompt_build(&g_tool, 1);
    check("prompt.append_before", has(p, "PRE-BEFORE"));
    check("prompt.append_after", has(p, "TOOLS-AFTER") && has(p, "ALL-AFTER"));
    check("prompt.order_before_core", has(p, "PRE-BEFORE") && has(p, "You are agentc"));
    /* REPLACE suppresses the default render for its anchor. */
    check("prompt.tools_replace", has(p, "TOOLS-REPLACED") && !has(p, "# Tools"));
    check("prompt.rules_replace", has(p, "RULES-LO") && !has(p, "# Rules"));
    /* priority: the lower value wins, the loser is dropped. */
    check("prompt.replace_priority", !has(p, "RULES-HI"));
    /* untouched anchors keep their core default. */
    check("prompt.preamble_default", has(p, "You are agentc"));
    check("prompt.env_default", has(p, "# Environment"));
    agentc_free(p);

    /* The public resolver returns the same winner. */
    AgcBuf rb = {0};
    bool got = agentc_ext_prompt_replace(&rb, AGENTC_ANCHOR_RULES);
    check("prompt.api_replace",
          got && has((char *)rb.p, "RULES-LO") && !has((char *)rb.p, "RULES-HI"));
    agentc_buf_free(&rb);

    /* An ALL-level REPLACE suppresses the whole anchor walk; only the ALL
     * before/after APPENDs around it remain. */
    host->add_prompt(&all_rep);
    p = agentc_prompt_build(&g_tool, 1);
    check("prompt.all_replace", has(p, "ONLY-ALL") && has(p, "ALL-AFTER") &&
                                    !has(p, "You are agentc") && !has(p, "PRE-BEFORE") &&
                                    !has(p, "# Environment"));
    agentc_free(p);
    return fails;
}
