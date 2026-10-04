/* args.c — shared tool-argument DOM (see args.h).
 *
 * The parse is deliberately tolerant: malformed JSON, an empty string and a
 * valid root of the wrong type all become root == NULL, never an error. The
 * builtins own the wording of the resulting tool result.
 */
#include "core/tools/args.h"

#include "base/limits.h"

int agentc_tool_args_parse(AgcToolArgs *a, const char *json, size_t n) {
    a->arena = NULL;
    a->root = NULL;
    AgcJsonArena *arena = agentc_json_arena_new(AGENTC_LIMIT_TOOL_ARENA);
    if (arena == NULL) return -12; /* ENOMEM: the caller reports a fatal run failure */
    a->arena = arena;
    if (json != NULL && n > 0) {
        AgcJson *root = agentc_json_parse_in(arena, json, n);
        if (agentc_json_type(root) == AGENTC_JSON_OBJ) a->root = root;
    }
    return 0;
}

void agentc_tool_args_free(AgcToolArgs *a) {
    if (a == NULL) return;
    agentc_json_arena_free(a->arena);
    a->arena = NULL;
    a->root = NULL;
}

const char *agentc_tool_args_str(const AgcToolArgs *a, const char *key) {
    if (a == NULL || a->root == NULL || key == NULL) return NULL;
    return agentc_json_get_str(a->root, key);
}

i64 agentc_tool_args_int(const AgcToolArgs *a, const char *key, i64 dflt) {
    if (a == NULL || a->root == NULL || key == NULL) return dflt;
    return agentc_json_get_int(a->root, key, dflt);
}

bool agentc_tool_args_bool(const AgcToolArgs *a, const char *key, bool dflt) {
    if (a == NULL || a->root == NULL || key == NULL) return dflt;
    return agentc_json_get_bool(a->root, key, dflt);
}

void agentc_tool_args_missing(AgcBuf *out, const char *tool, const char *key, bool *is_error) {
    if (out != NULL)
        agentc_buf_printf(out, "error: %s: missing required field: %s", tool, key);
    if (is_error != NULL) *is_error = true;
}
