/*
 * registry_int.h — internal extension bus.
 *
 * Core default extensions (builtin-tools, builtin-context, mcp) compile against
 * this header. Because they are linked into the binary they may register tools
 * with asynchronous start/step and call the internal emit directly; that is the
 * only privilege they get over a statically-linked extension, which sees only
 * include/agentc_ext.h. Everything a linked extension can do — add_tool,
 * add_section, add_status, on, emit — is available here through ext.h.
 *
 * Include this header only from src/.
 */
#ifndef AGENTC_EXT_REGISTRY_INT_H
#define AGENTC_EXT_REGISTRY_INT_H

#include "ext.h"

/*
 * Register an internal AgcTool (run, or start/step for the async job driver).
 * The registry deep-copies the struct and its owned string fields, so the
 * caller may release its own storage immediately; `ud` and the callbacks are
 * kept as-is (borrowed). Returns 0 | -errno. Owner-tagged like every other
 * registration, so a failed default init rolls it back. A successful add after
 * load marks the registry dirty (recompose at the next turn boundary).
 */
int agentc_ext_add_tool_internal(const AgcTool *tool);

/*
 * Remove an internal AgcTool by name from the registry. The matching record
 * (and the copies the registry made) is dropped and the registry is marked
 * dirty; the tool's `ud` is deliberately NOT freed, because a transcript or
 * job may still hold the handed-out AgcTool. Returns 0 | -ENOENT (-2).
 */
int agentc_ext_remove_tool_internal(const char *name);

/*
 * Register a periodic pump callback (default extensions only; called from
 * agentc_ext_pump on the main loop). Owner-tagged so unload removes it.
 * The callback returns 0 or -errno; the registry ignores failures.
 */
void agentc_ext_add_pump(int (*fn)(void *ud), void *ud);

#endif /* AGENTC_EXT_REGISTRY_INT_H */
