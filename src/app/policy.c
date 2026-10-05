/* policy.c — the app's active-tool policy and the destructive-tool
 * default (see .agents/design/30-extensibility.md).
 *
 * A tool is eligible unless the "all" branch applies the destructive filter:
 * a non-core AGENTC_TOOL_DESTRUCTIVE tool (an extension or MCP tool) is
 * skipped unless it is named in --tools/default_tools. Core builtins carry
 * AGENTC_TOOL_CORE and stay eligible. Every filtered selection logs one line
 * with the count and (bounded) names so the user knows why a tool is missing.
 */
#include "agent.h"
#include "ext.h"
#include "app/policy.h"

/* Names appended to the destructive-tool diagnostic before it is elided. */
#define POLICY_NAMES_MAX 8
#define POLICY_NAMES_BUF 512

/* Select a subset of the tools by name. `selected` is the number already in
 * `out` from an earlier call (default_tools are selected one name-list at a
 * time); a name already selected is skipped so `--tools read,read` and a
 * repeated default_tools entry each produce one table entry. Returns the new
 * selected count, or (size_t)-1 when `strict` and a non-MCP name is unknown. A
 * name beginning `mcp__`, and the three generic resource-tool names, are
 * always deferred when absent, not fatal: with async MCP the server may not
 * have connected yet (and the resource tools are published only at the first
 * resource-capable list sync), and the turn-boundary recompose selects the
 * tool once it appears. */
static size_t select_tools(const AgcTool *all, size_t nall, AgcTool *out, size_t cap,
                           const char *list, bool strict, size_t selected) {
    size_t k = selected;
    const char *p = list;
    while (p && *p) {
        const char *e = p;
        while (*e && *e != ',') e++;
        const char *s = p;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        const char *t = e;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t')) t--;
        if (t > s) {
            size_t nl = (size_t)(t - s);
            size_t match = 0;
            bool found = false;
            for (size_t i = 0; i < nall && !found; i++)
                if (all[i].name &&
                    agentc_str_eq(all[i].name, agentc_strlen(all[i].name), s, nl)) {
                    found = true;
                    match = i;
                }
            if (found) {
                bool dup = false;
                for (size_t i = 0; i < k && !dup; i++)
                    if (out[i].name &&
                        agentc_str_eq(out[i].name, agentc_strlen(out[i].name), s, nl))
                        dup = true;
                if (!dup && k < cap) out[k++] = all[match];
            } else {
                char name[128];
                if (nl >= sizeof name) nl = sizeof name - 1;
                agentc_memcpy(name, s, nl);
                name[nl] = 0;
                bool mcp_deferred = nl >= 5 && name[0] == 'm' && name[1] == 'c' &&
                                    name[2] == 'p' && name[3] == '_' && name[4] == '_';
                /* The generic MCP resource tools are published lazily at the
                 * first resource-capable list sync, long after --tools is
                 * parsed, so they are deferred by exact name. Deliberately not
                 * a broad `mcp_` prefix: a typo in any other name still exits
                 * 2 at startup. */
                bool resource_tool_deferred =
                    agentc_streq(name, "mcp_list_resources") ||
                    agentc_streq(name, "mcp_list_resource_templates") ||
                    agentc_streq(name, "mcp_read_resource");
                if (mcp_deferred || resource_tool_deferred) {
                    /* Only a loaded mcp extension can ever connect, so
                     * never promise a connection that cannot happen. */
                    if (agentc_ext_is_loaded("mcp"))
                        agentc_logf(1, "deferred MCP tool: %s (selected when its server connects)",
                                name);
                    else
                        agentc_logf(2, "deferred MCP tool: %s (MCP is disabled in this session)",
                                name);
                } else if (strict) {
                    agentc_logf(3, "unknown tool: %s", name);
                    return (size_t)-1;
                } else {
                    agentc_logf(2, "warning: ignoring unknown tool: %s", name);
                }
            }
        }
        p = *e ? e + 1 : e;
    }
    return k;
}

/* The "all" branch: take everything, minus the non-core destructive tools
 * default. Returns the selected count and logs one diagnostic when anything was
 * filtered. */
static size_t select_all(const AgcTool *all, size_t nall, AgcTool *out, size_t cap) {
    size_t n = 0, hidden = 0, shown = 0;
    AgcBuf names = { 0 };
    for (size_t i = 0; i < nall; i++) {
        if ((all[i].flags & AGENTC_TOOL_DESTRUCTIVE) && !(all[i].flags & AGENTC_TOOL_CORE)) {
            hidden++;
            if (shown < POLICY_NAMES_MAX && names.len < POLICY_NAMES_BUF && all[i].name) {
                if (shown) agentc_buf_cstr(&names, ", ");
                agentc_buf_cstr(&names, all[i].name);
                shown++;
            }
            continue;
        }
        if (n < cap) out[n++] = all[i];
    }
    if (hidden) {
        agentc_logf(2,
                "policy: %llu destructive extension tool%s hidden by default: %s%s "
                "(name them in --tools or default_tools)",
                (unsigned long long)hidden, hidden == 1 ? "" : "s",
                names.p ? (const char *)names.p : "?",
                hidden > shown ? ", ..." : "");
    }
    agentc_buf_free(&names);
    return n;
}

int agentc_policy_apply(const AgcTool *all, size_t nall, const ToolPolicy *pol,
                        bool strict, AgcTool **out, size_t *out_n) {
    if (!pol || !out || !out_n) return -22;   /* EINVAL */
    AgcTool *tools = agentc_alloc((nall ? nall : 1) * sizeof *tools);
    size_t n = 0;
    if (!pol->no_tools) {
        if (pol->tools_flag) {
            n = select_tools(all, nall, tools, nall, pol->tools_flag, strict, 0);
            if (n == (size_t)-1) {
                agentc_free(tools);
                return -22;
            }
        } else if (pol->cfg && pol->cfg->ndefault_tools) {
            for (size_t i = 0; i < pol->cfg->ndefault_tools; i++)
                n = select_tools(all, nall, tools, nall, pol->cfg->default_tools[i], false, n);
        } else {
            n = select_all(all, nall, tools, nall);
        }
    }
    *out = tools;
    *out_n = n;
    return 0;
}

/* Rebuild the agent's active tool table after a late contribution (MCP connect
 * during a run). Runs at the agent's turn boundary; a no-op when the registry
 * is clean. With strict=false a genuinely missing non-MCP name is logged by
 * select_tools and skipped, never silently turned into an empty table. */
void agentc_policy_recompose(void *ud, AgcAgent *a) {
    ToolPolicy *pol = ud;
    if (!pol || !a || !agentc_ext_dirty()) return;
    agentc_ext_clear_dirty();
    size_t navail = agentc_ext_tools(NULL, 0);
    AgcTool *available = agentc_alloc((navail ? navail : 1) * sizeof *available);
    navail = agentc_ext_tools(available, navail);
    AgcTool *tools = NULL;
    size_t ntools = 0;
    int rc = agentc_policy_apply(available, navail, pol, false, &tools, &ntools);
    agentc_free(available);
    if (rc != 0) return;   /* strict=false cannot miss; keep the previous table */
    if (ntools == 0 && !pol->no_tools &&
        (pol->tools_flag || (pol->cfg && pol->cfg->ndefault_tools))) {
        /* Every named tool is still absent (e.g. only deferred mcp__ names):
         * keep the previous table instead of silently emptying it. */
        agentc_free(tools);
        return;
    }
    agentc_agent_set_tools(a, tools, ntools);
    agentc_free(tools);
}
