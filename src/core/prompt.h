/* prompt.h — internal prompt-assembly contract (src/core/prompt.c).
 *
 * The four core sections are rendered by agentc_ext_prompt_build, which then
 * appends the extension-contributed sections. agentc_prompt_build is the
 * owned-string convenience used by the agent loop.
 */
#ifndef AGENTC_CORE_PROMPT_H
#define AGENTC_CORE_PROMPT_H

#include "agent.h"
#include "config.h"

char *agentc_prompt_build(const AgcTool *tools, size_t ntools);
void agentc_ext_prompt_build(AgcBuf *out, const AgcTool *tools, size_t ntools);

/* cwd/trusted for the builtin-context section (project files and skills). */
void agentc_builtin_context_set(const char *cwd, bool trusted);

/* ---------------------------------------------------------- resource roots */
/* Extra roots contributed by resources_discover (already validated by
 * src/app/project.c) and consumed by the skills/templates/themes loaders.
 * Process-lifetime, deduplicated and capped at AGENTC_RESOURCE_ROOTS_MAX.
 * The accessors return a borrowed snapshot (never NULL; count 0 when empty);
 * roots scan in registration order, so a later same-named resource wins. */
void agentc_resource_add_skill_root(const char *dir);
void agentc_resource_add_prompt_root(const char *dir);
void agentc_resource_add_theme_root(const char *dir);
size_t agentc_resource_skill_roots(const char *const **out);
size_t agentc_resource_prompt_roots(const char *const **out);
size_t agentc_resource_theme_roots(const char *const **out);

/* --------------------------------------------------------------- loaders */
/* Extra-roots form of agentc_skills_load (src/core/resources.c): the standard
 * roots first, then each extra root in order. Used by the builtin-context
 * loader; declared here so resources.c and prompt.c share one contract. */
size_t agentc_skills_load_extra(const char *cwd, bool trusted,
                                const char *const *extra_roots, size_t nextra,
                                AgcSkill **out, size_t max);

/* Prompt/template name gate: [a-z0-9._:-]{1,64}. Shared by the prompt
 * registry (core/prompts.c) and the file-template loader (core/resources.c). */
bool agentc_prompt_name_valid(const char *name);

/* Reads the frontmatter-stripped body of the skill at `path`; owned, NULL on
 * a read failure. `out_len` receives the body length. */
char *agentc_skill_body_read(const char *path, size_t *out_len);

/* Body of the skill named `name` across the standard and resource roots,
 * frontmatter stripped. Owned, NULL when unknown. The TUI `/skill:<name>`
 * command uses this. */
char *agentc_builtin_context_skill_body(const char *name, size_t *out_len);

#endif /* AGENTC_CORE_PROMPT_H */
