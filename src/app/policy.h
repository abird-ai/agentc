/* policy.h — the app's active-tool policy.
 *
 * --tools wins over config default_tools, which wins over "all"; --no-tools
 * clears the table. Startup and the turn-boundary recompose both select
 * through agentc_policy_apply so the two paths cannot drift apart. In the
 * "all" branch a non-core AGENTC_TOOL_DESTRUCTIVE tool is hidden unless it is
 * named: an extension must not silently gain destructive default access.
 *
 * Internal app header, not part of the frozen public surface.
 */
#ifndef AGENTC_APP_POLICY_H
#define AGENTC_APP_POLICY_H

#include "agent.h"
#include "config.h"

typedef struct {
    const AgcConfig *cfg;   /* borrowed; provides default_tools */
    const char *tools_flag; /* borrowed --tools value, NULL when unset */
    bool no_tools;
} ToolPolicy;

/* Allocate and select the active tool table from `all` per `pol`. `strict`
 * makes an absent non-MCP --tools name a hard start-up error; a missing
 * `mcp__` name, and the three generic resource-tool names, are always deferred
 * to the recompose pass.
 * Returns 0 and fills `*out` and `*out_n` (free *out with agentc_free), or -EINVAL on
 * a strict miss. */
int agentc_policy_apply(const AgcTool *all, size_t nall, const ToolPolicy *pol,
                        bool strict, AgcTool **out, size_t *out_n);

/* AgcRecomposeFn body used by the app: re-selects and installs the table when
 * the registry is dirty. Keeps the previous table when every named tool is
 * still absent (only deferred MCP names), never silently emptying it. */
void agentc_policy_recompose(void *ud, AgcAgent *a);

#endif /* AGENTC_APP_POLICY_H */
