/* engine.h — internal: core-tool backend engine (P5-TOOL-ENGINE).
 *
 * `grep` and `find` keep their names, JSON schemas and normalized output, but
 * the backend is a process-wide choice:
 *   - AGENTC_TOOL_ENGINE_EXTERNAL (default): walk the descriptor table at
 *     builtin-tools init time and pin the first rung that resolves, with the
 *     built-in implementation as the last resort
 *     (grep: ripgrep -> POSIX grep -> built-in;
 *      find: fd       -> POSIX find -> built-in);
 *   - AGENTC_TOOL_ENGINE_INTERNAL: force the built-ins and never spawn.
 * `ls` is always built-in.
 *
 * engine.c is the single source of truth: the table carries, per rung, the
 * binary, the description, the argv builder and the run function, and
 * resolution/logging/reset all iterate it. Adding a tool or a rung is a table
 * edit, not a new function. Tool names, JSON schemas, argument-validation
 * errors and normalized output are identical for every backend;
 * only `desc` changes, so the model is told the regex dialect and the
 * file-selection rules it is getting.
 *
 * Core-internal (like prov/provider.h): grep.c/find.c/registry.c and
 * tests/engine_test.c include it directly; it is not part of the public ABI.
 */
#ifndef AGENTC_CORE_TOOLS_ENGINE_H
#define AGENTC_CORE_TOOLS_ENGINE_H

#include "agent.h"

/* Modes. */
#define AGENTC_TOOL_ENGINE_EXTERNAL 0   /* prefer PATH tools; built-in last resort */
#define AGENTC_TOOL_ENGINE_INTERNAL 1   /* force the built-ins; never spawn */

/* Backend names used in logs, descriptions and selection. */
#define AGENTC_ENGINE_RG      "ripgrep"
#define AGENTC_ENGINE_GREP    "grep"
#define AGENTC_ENGINE_FD      "fd"
#define AGENTC_ENGINE_FIND    "find"
#define AGENTC_ENGINE_BUILTIN "built-in"

/* Parse "external" | "internal" (exact). -> AGENTC_TOOL_ENGINE_* | -1. */
int agentc_tool_engine_parse(const char *value);

/* Resolve the process-wide engine once. `path_env` is the PATH value to search
 * (NULL = none); production passes os_getenv("PATH"). Idempotent: the first
 * call wins, later calls are ignored, so a turn-boundary recompose never
 * re-resolves. Resolution is silent: call agentc_tool_engine_summary() (or
 * agentc_tool_engine_log() on non-TUI paths) once from the application startup
 * path to surface the chosen engines. */
void agentc_tool_engine_init(int mode, const char *path_env);

/* Succinct, human-facing summary of the resolved engines, e.g.
 * `grep->rg find->fd ls->built-in`. The banner/log add the `tools: ` prefix.
 * Never NULL; empty before init. The returned pointer is into static storage,
 * valid until the next call. */
const char *agentc_tool_engine_summary(void);

/* Emit the one startup info line (`tools: <summary>`) for non-TUI startup
 * paths. The TUI banner carries the summary itself instead. Called by the app
 * after extensions load, NOT by init, so extension-loading golden tests stay
 * independent of the host PATH (wine-check must stay green). */
void agentc_tool_engine_log(void);

/* Effective backend name for a managed tool ("grep"/"find"); the built-in name
 * for anything else. Never NULL. */
const char *agentc_tool_engine_backend(const char *tool);

/* Pure: description for `tool` on `backend` (never NULL). Unknown pairs -> "".
 * Lets tests pin every backend's text without depending on the host's PATH. */
const char *agentc_tool_engine_desc_for(const char *tool, const char *backend);

/* Tests only: forget the resolved engine so the next init resolves again. */
void agentc_tool_engine_reset(void);

/* Selection handed to builtin-tools init. `desc` is never NULL; `run` is NULL
 * when the built-in implementation must be kept (internal mode, last resort, or
 * a tool with no external rung). */
typedef struct {
    const char *desc;
    int (*run)(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error);
} AgcToolEngineSel;

/* Fills *sel for a managed tool ("grep"/"find"); false for any other name. */
bool agentc_tool_engine_select(const char *tool, AgcToolEngineSel *sel);

/* ---- shared argument views (defined by grep.c / find.c) ------------------ */
/* Every backend parses and validates through these, so the accepted schema and
 * the error text stay byte-identical to the built-ins. The parser owns a small
 * JSON arena and the string views point into it; call the matching `_free` on
 * every path. Returns 0 = proceed, 1 = answered (out and *is_error already hold
 * the exact tool text). A defensive NULL-arena branch answers with an
 * out-of-memory error instead of a parse error; with the small initial block
 * (AGENTC_LIMIT_TOOL_ARENA) the allocator aborts on OOM like the rest of the
 * core, so that branch is a guard for a future larger block, not a normal path. */
typedef struct {
    AgcJsonArena *arena;
    const char *pattern, *path, *glob;
    bool icase, literal;
    bool null_sep;   /* engine-set, not model-facing: pass/parse grep -Z */
    i64 context, limit;
} AgcGrepArgs;

typedef struct {
    AgcJsonArena *arena;
    const char *pattern, *path;
    i64 limit;
} AgcFindArgs;

int agentc_tool_grep_args(const AgcToolCall *call, AgcGrepArgs *ga, AgcBuf *out,
                          bool *is_error);
void agentc_tool_grep_args_free(AgcGrepArgs *ga);
int agentc_tool_find_args(const AgcToolCall *call, AgcFindArgs *fa, AgcBuf *out,
                          bool *is_error);
void agentc_tool_find_args_free(AgcFindArgs *fa);

/* ---- argv mapping ------------------------------------------------------- */
/* Appends the NUL-terminated argv (including the trailing NULL sentinel) for
 * `tool` on `backend` to `argv` (an AgcVec of char*, as consumed by
 * os_spawn_group). `args` is a `const AgcGrepArgs *` for grep and a
 * `const AgcFindArgs *` for find. Only external backends are valid: returns
 * 0 | -EINVAL. Strings are freshly allocated; the caller frees each entry and
 * the vec. Patterns are positional after `--`, so a leading '-' can never
 * become a flag. */
int agentc_tool_engine_build_argv(const char *tool, const char *backend, const void *args,
                                  AgcVec *argv);

/* ---- grep hit normalization (pure; used by the run functions and tests) -- */
/* One match or context record, normalized from a backend's stdout. The string
 * views are borrowed: for rg they live in the caller's `scratch` arena and are
 * valid until the next rg_hit call in that arena; for POSIX grep they point
 * into `line`. The collector copies what it keeps. */
typedef struct {
    const char *path;
    size_t path_len;
    u64 line;
    const char *text;
    size_t text_len;
    bool is_match;
    bool text_cut;   /* collector stored only a prefix of `text` */
} AgcGrepHit;

/* Parse one `rg --json` line. `scratch` is a caller-owned arena reused across
 * lines (never NULL; a NULL scratch returns 0 so a caller that failed to
 * allocate treats the run as an error, not as "no matches"). Returns 1 when the
 * record is a match/context event with a path and line text, 0 for
 * begin/end/summary/malformed/blank. */
int agentc_tool_engine_rg_hit(AgcJsonArena *scratch, const char *line, size_t n,
                              AgcGrepHit *hit);

/* Parse one POSIX `grep -H` line (`file:line:text` for a match,
 * `file-line-text` for context). Returns 1 on a record, 0 for a blank line or a
 * line without a `[-:]<digits>[-:]` boundary. */
int agentc_tool_engine_grep_hit(const char *line, size_t n, AgcGrepHit *hit);

/* Parse one NUL-delimited POSIX `grep -H -Z` record (`file\0line:text` for a
 * match, `file\0line-text` for context). Returns 1 on a record, 0 when there is
 * no NUL or no numeric boundary (which also ignores the `--` group separator).
 * The path is everything before the first NUL, so a name such as
 * `report-2024-q1.txt` is never mis-split. */
int agentc_tool_engine_grep_hit_null(const char *line, size_t n, AgcGrepHit *hit);

/* Append one hit as `path:line:text\n`, applying AGENTC_LIMIT_GREP_LINE with a
 * `...` suffix (also when the caller set `text_cut` because it stored only a
 * prefix). One formatter for the collector and the tests. */
void agentc_tool_engine_emit_hit(AgcBuf *out, const AgcGrepHit *hit);

#endif /* AGENTC_CORE_TOOLS_ENGINE_H */
