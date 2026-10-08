/* args.h — shared tool-argument DOM (internal).
 *
 * One owned JSON arena per tool call: the parsed document stays valid for the
 * whole run()/start()/step() and is released in one shot. Malformed input and a
 * valid non-object root are both tolerated as root == NULL so each builtin can
 * answer with its own exact, operator-facing error text; only an arena OOM
 * fails the parse.
 */
#ifndef AGENTC_CORE_TOOLS_ARGS_H
#define AGENTC_CORE_TOOLS_ARGS_H

#include "agent.h"

typedef struct {
    AgcJsonArena *arena;   /* owned */
    AgcJson *root;         /* NULL for malformed / non-object / OOM */
} AgcToolArgs;

/* 0, or -ENOMEM (zeroed struct) when the arena cannot be allocated. */
int  agentc_tool_args_parse(AgcToolArgs *a, const char *json, size_t n);
void agentc_tool_args_free(AgcToolArgs *a);

/* NULL-safe getters; str points into the arena (lifetime = the args struct). */
const char *agentc_tool_args_str(const AgcToolArgs *a, const char *key);
/* Like agentc_tool_args_str but also reports the string length (which may
 * include embedded NUL bytes); *len is set to 0 when the result is NULL. */
const char *agentc_tool_args_str_len(const AgcToolArgs *a, const char *key, size_t *len);
i64  agentc_tool_args_int(const AgcToolArgs *a, const char *key, i64 dflt); /* present+non-num -> dflt */
bool agentc_tool_args_bool(const AgcToolArgs *a, const char *key, bool dflt);

/* Appends "error: <tool>: missing required field: <key>" and sets *is_error. */
void agentc_tool_args_missing(AgcBuf *out, const char *tool, const char *key, bool *is_error);

#endif /* AGENTC_CORE_TOOLS_ARGS_H */
