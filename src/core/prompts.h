/* prompts.h — invocable prompt registry (src/core/prompts.c).
 *
 * File prompt templates (`/name`), extension-contributed prompts and, later,
 * MCP prompts all register here. Records are pointer-stable and never freed:
 * removing one only clears its `live` flag, so a TUI menu that borrowed a
 * name pointer keeps pointing at valid memory. The cap (AGENTC_PROMPTS_MAX)
 * bounds total registrations, including retired ones.
 *
 * Names are validated as [a-z0-9._:-]{1,64}. A later registration with the
 * same name retires the earlier record (later wins), matching the resource
 * root ordering.
 */
#ifndef AGENTC_CORE_PROMPTS_H
#define AGENTC_CORE_PROMPTS_H

#include "agentc.h"

/* Borrowed view of one live record, as the slash-command menu needs it. */
typedef struct {
    const char *name;         /* stable for the process lifetime */
    const char *description;  /* never NULL; may be empty */
    const char *hint;         /* never NULL; may be empty */
} AgcPromptInfo;

/* Expands one invocation to an owned string (NULL on failure). */
typedef char *(*AgcPromptExpandFn)(void *ud, const char *args);

/* Register `name` (validated, copied) with an optional description/hint and
 * an expand callback. `source` tags records for bulk retirement (for example
 * an extension name or "file"); NULL is stored as "". Returns 0, -EINVAL for
 * an invalid name or -ENOSPC when the registry is full. */
int agentc_prompts_register(const char *name, const char *desc, const char *hint,
                            const char *source, void *ud, AgcPromptExpandFn expand);

/* Retire the live record named `name`. Returns false when it does not exist. */
bool agentc_prompts_remove(const char *name);

/* Retire every live record whose source equals `source`. Returns the count. */
size_t agentc_prompts_clear_source(const char *source);

/* Count-first list of live records in registration order. With out==NULL
 * returns the total; otherwise fills min(total, max) borrowed views. */
size_t agentc_prompts_list(AgcPromptInfo *out, size_t max);

bool agentc_prompts_has(const char *name);

/* Expand `name` with `args` (NULL means ""). Owned string, or NULL when the
 * name is unknown or its expand callback failed. */
char *agentc_prompts_expand(const char *name, const char *args);

/* Scan the file prompt templates (<config>/prompts, trusted project prompts,
 * then the resources_discover prompt roots) into the registry under source
 * "file"; later roots win, invalid stems are skipped with a warning. Returns
 * the number of records registered. */
size_t agentc_prompts_load_file_templates(const char *cwd, bool trusted);

#endif /* AGENTC_CORE_PROMPTS_H */
