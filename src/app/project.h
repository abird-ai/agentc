/* project.h — trust resolution and resources_discover consumption.
 *
 * Internal app header, not part of the frozen public surface. main.c
 * orchestrates; the ordering rules live here:
 *   register defaults/linked -> apply config -> provisional mcp context
 *   (untrusted) -> load_all -> agentc_project_resolve_trust() -> final mcp
 *   context -> agentc_project_apply_resources() -> builtin context.
 */
#ifndef AGENTC_APP_PROJECT_H
#define AGENTC_APP_PROJECT_H

#include "agentc.h"

/* Resolve the final project-trust verdict after extensions are loaded.
 * Precedence (fixed):
 *   1. an explicit CLI verdict (-1 unset, 0 no, 1 yes) wins;
 *   2. a consumed project_trust hook decision ("yes"/"no"; "undecided" falls
 *      through), with remember:true persisted through agentc_trust_save();
 *   3. agentc_trust_resolve(cwd, cli_verdict, agentc_config_default_trusted()).
 * A blocked, failed or malformed hook is fail-closed: untrusted. When trust is
 * granted only now, the <cwd>/.agentc/config.jsonc limitation is logged
 * because the config was already loaded without the project file. */
bool agentc_project_resolve_trust(const char *cwd, int cli_verdict);

/* Emit resources_discover {cwd, reason:"startup"} when an extension watches
 * it, then consume the result: validated skillPaths/promptPaths/themePaths are
 * added to the process-lifetime resource root stores (src/core/resources.c).
 * A blocked hook contributes nothing. Each list consumes at most
 * AGENTC_RESOURCE_PATHS_MAX paths; the excess is logged and ignored. */
void agentc_project_apply_resources(const char *cwd, bool trusted);

/* Path gate (exposed for tests): 0 when `path` is absolute, has no ".."
 * segment, and is under the config dir (always allowed) or under cwd (allowed
 * only when trusted); -EINVAL otherwise. */
int agentc_project_validate_path(const char *cwd, bool trusted, const char *path);

#endif /* AGENTC_APP_PROJECT_H */
