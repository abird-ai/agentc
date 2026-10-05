/*
 * registry_int.h — internal extension bus.
 *
 * Core default extensions (builtin-tools, builtin-context, mcp) compile against
 * this header. Because they are linked into the binary they may register tools
 * with asynchronous start/step and call the internal emit directly; that is the
 * only privilege they get over a statically-linked extension, which sees only
 * include/agentc_ext.h. Everything a linked extension can do — add_tool,
 * add_prompt, add_status_line, on, emit — is available here through ext.h.
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

/* ------------------------------------------------- helper-process extensions */
/*
 * W3: run an extension in a separate helper process over the length-prefixed
 * framed JSON protocol in include/frame.h:
 *   request   {"id":N,"op":"hook"|"tool"|"command"|"shutdown",
 *              "point":...,"name":...,"payload":<json>}
 *   response  {"id":N,"ok":bool,"result":<json>|null,"error":"..."}
 * The helper sends one manifest frame on startup; register reads it (bounded,
 * synchronous) and records the extension owner-tagged like a linked one. Proxy
 * tools/hooks/commands are published from the manifest during
 * agentc_ext_load_all(), so a config-disabled helper never publishes rows. On
 * helper EOF/timeout/malformed/missing-id the proxy returns a negative code
 * (override points fail closed, observe points skip), the helper is marked dead
 * and its rows retired; the host keeps running. Limit: a helper cannot call back
 * into the host (no nested emits, tool calls or services).
 *
 * Returns 0 | -EEXIST (repeated name) | -EINVAL | -EPROTO | transport -errno;
 * -E2BIG (a manifest over a cap) | transport -errno. `argv[0]` must be an
 * executable path: the helper is spawned with an empty environment (no `PATH`),
 * so a bare name is not resolved. `cwd` may be NULL (inherit), and `err`
 * receives a short diagnostic.
 */
int agentc_ext_register_helper(const char *name, char *const *argv, const char *cwd, char *err,
                               size_t cap);

/* Test/diagnostic seam: signal the named helper's process group (a crash
 * simulation). Reaping/row retirement happen on the next proxy call. Returns
 * 0 | -ENOENT | -errno. */
int agentc_ext_helper_kill(const char *name, int sig);

/* Test seam: the helper vector's slot count (including freed NULL slots). Used
 * by tests/ext_test.c to assert that register/unload churn reuses a freed slot
 * (L6) instead of growing the vector without bound. */
size_t agentc_ext_helper_slot_count(void);

#endif /* AGENTC_EXT_REGISTRY_INT_H */
