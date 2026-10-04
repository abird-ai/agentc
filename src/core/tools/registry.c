/* registry.c — built-in tool registry and JSON argument validation.
 *
 * Every validation failure becomes an error tool result string, never a
 * provider error. The builtins implement the v2 run()/start() entry points and
 * append their answer to the job's output buffer; the v1 exec field stays NULL.
 * The public agentc_tool_* helpers keep their owned-result contract.
 */
#include "agent.h"
#include "config.h"
#include "plat.h"
#include "ext.h"
#include "core/tools/args.h"
#include "core/tools/engine.h"
#include "ext/registry_int.h"

char *agentc_tool_read_err(bool *is_error, const char *fmt, ...);

/* v2 async pair owned by bash.c; the job driver drives it through jobs.h. */
int bash_start(const AgcTool *self, const AgcToolCall *call, AgcJob *job);
int bash_step(const AgcTool *self, AgcJob *job);

/* --------------------------------------------------------------- helpers */

/* A present but non-numeric field must keep the exact legacy error text, so
 * the callers check the node type before falling back to the defaulting
 * getter (present+non-num -> dflt). */
static bool wrong_type(const AgcToolArgs *a, const char *key) {
    AgcJson *v = agentc_json_get(a->root, key);
    return v != NULL && agentc_json_type(v) != AGENTC_JSON_NUM;
}

static int args_oom(bool *is_error) {
    if (is_error) *is_error = true;
    return -12; /* ENOMEM */
}

static void append_owned(AgcBuf *out, char *res, bool err, bool *is_error) {
    if (res != NULL) {
        agentc_buf_cstr(out, res);
        agentc_free(res);
    }
    if (is_error) *is_error = err;
}

/* ------------------------------------------------------------------ read */
static const char read_desc[] =
    "Read a text file. Returns up to 2000 lines or 50 KB with a continuation "
    "offset; use offset/limit to page through large files.";
static const char read_params[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path to read\"},"
    "\"offset\":{\"type\":\"integer\",\"description\":\"Zero-based line offset\"},"
    "\"limit\":{\"type\":\"integer\",\"description\":\"Maximum lines to return\"}},"
    "\"required\":[\"path\"]}";

static int read_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                    bool *is_error) {
    (void)self;
    if (is_error) *is_error = false;
    AgcToolArgs a;
    if (agentc_tool_args_parse(&a, call->args_json,
                               call->args_json ? agentc_strlen(call->args_json) : 0) != 0)
        return args_oom(is_error);
    if (a.root == NULL) {
        agentc_buf_cstr(out, "error: read: arguments must be a JSON object");
        if (is_error) *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    const char *path = agentc_tool_args_str(&a, "path");
    if (path == NULL) {
        agentc_tool_args_missing(out, "read", "path", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }
    if (wrong_type(&a, "offset") || wrong_type(&a, "limit")) {
        agentc_buf_cstr(out, "error: read: offset and limit must be numbers");
        if (is_error) *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    i64 offset = agentc_tool_args_int(&a, "offset", 0);
    i64 limit = agentc_tool_args_int(&a, "limit", 0);
    bool err = false;
    char *res = agentc_tool_read(path, offset, limit, &err);
    append_owned(out, res, err, is_error);
    agentc_tool_args_free(&a);
    return 0;
}

/* ------------------------------------------------------------------ edit */
static const char edit_desc[] =
    "Edit a text file. Each oldText must match exactly once in the original "
    "text; edits are applied right-to-left and preserve BOM/CRLF.";
static const char edit_params[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path to edit\"},"
    "\"edits\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"oldText\":{\"type\":\"string\"},\"newText\":{\"type\":\"string\"}},"
    "\"required\":[\"oldText\",\"newText\"]}}},"
    "\"required\":[\"path\",\"edits\"]}";

static int edit_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                    bool *is_error) {
    (void)self;
    if (is_error) *is_error = false;
    AgcToolArgs a;
    if (agentc_tool_args_parse(&a, call->args_json,
                               call->args_json ? agentc_strlen(call->args_json) : 0) != 0)
        return args_oom(is_error);
    if (a.root == NULL) {
        agentc_buf_cstr(out, "error: edit: arguments must be a JSON object");
        if (is_error) *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    const char *path = agentc_tool_args_str(&a, "path");
    if (path == NULL) {
        agentc_tool_args_missing(out, "edit", "path", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }
    AgcJson *edits = agentc_json_get(a.root, "edits");
    if (agentc_json_type(edits) != AGENTC_JSON_ARR) {
        agentc_buf_cstr(out, "error: edit: edits must be an array");
        if (is_error) *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    bool err = false;
    char *res = agentc_tool_edit(path, call->args_json, &err);
    append_owned(out, res, err, is_error);
    agentc_tool_args_free(&a);
    return 0;
}

/* ----------------------------------------------------------------- write */
static const char write_desc[] =
    "Write a file, creating parent directories. The replacement is atomic "
    "(temp file + rename).";
static const char write_params[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path to write\"},"
    "\"content\":{\"type\":\"string\",\"description\":\"Full file content\"}},"
    "\"required\":[\"path\",\"content\"]}";

static int write_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                     bool *is_error) {
    (void)self;
    if (is_error) *is_error = false;
    AgcToolArgs a;
    if (agentc_tool_args_parse(&a, call->args_json,
                               call->args_json ? agentc_strlen(call->args_json) : 0) != 0)
        return args_oom(is_error);
    if (a.root == NULL) {
        agentc_buf_cstr(out, "error: write: arguments must be a JSON object");
        if (is_error) *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    const char *path = agentc_tool_args_str(&a, "path");
    if (path == NULL) {
        agentc_tool_args_missing(out, "write", "path", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }
    const char *content = agentc_tool_args_str(&a, "content");
    if (content == NULL) {
        agentc_tool_args_missing(out, "write", "content", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }
    bool err = false;
    char *res = agentc_tool_write(path, content, &err);
    append_owned(out, res, err, is_error);
    agentc_tool_args_free(&a);
    return 0;
}

/* ------------------------------------------------------- ls/find/grep */
int agentc_tool_ls_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                       bool *is_error);
int agentc_tool_find_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                         bool *is_error);
int agentc_tool_grep_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                         bool *is_error);

static const char ls_desc[] =
    "List one directory, sorted by name, with a trailing / on directories. "
    "Hidden files are included; the listing is truncated at `limit` entries.";
static const char ls_params[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"Directory to list (default .)\"},"
    "\"limit\":{\"type\":\"integer\",\"description\":\"Maximum entries to return\"}}}";

static const char find_params[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"pattern\":{\"type\":\"string\",\"description\":\"Glob pattern, e.g. **/*.c\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Directory to search (default .)\"},"
    "\"limit\":{\"type\":\"integer\",\"description\":\"Maximum paths to return\"}},"
    "\"required\":[\"pattern\"]}";

static const char grep_params[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"pattern\":{\"type\":\"string\",\"description\":\"Text or regex to search for\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"File or directory (default .)\"},"
    "\"glob\":{\"type\":\"string\",\"description\":\"Only search files matching this glob\"},"
    "\"ignore_case\":{\"type\":\"boolean\",\"description\":\"Case-insensitive search\"},"
    "\"literal\":{\"type\":\"boolean\",\"description\":\"Treat pattern as plain text\"},"
    "\"context\":{\"type\":\"integer\",\"description\":\"Lines of context around matches\"},"
    "\"limit\":{\"type\":\"integer\",\"description\":\"Maximum matches to return\"}},"
    "\"required\":[\"pattern\"]}";

/* ----------------------------------------------------------------- bash */
/* The shell differs per platform (and per `shell` config), so the description
 * follows os_shell_kind() rather than letting the model assume every command
 * is bash. Keep each variant short and explicit about the syntax. */
static const char bash_desc_sh[] =
    "Run a shell command with /bin/sh -lc. Output is truncated to 2000 lines "
    "or 50 KB; the full output is saved to a temp file. Returns the exit code.";
static const char bash_desc_cmd[] =
    "Run a command with cmd.exe (/d /s /c). This is Windows cmd syntax, not "
    "bash: use %VAR% (not $VAR) and & to chain commands. Output is truncated "
    "to 2000 lines or 50 KB; the full output is saved to a temp file. Returns "
    "the exit code.";
static const char bash_desc_ps[] =
    "Run a command with PowerShell (-NoProfile -NonInteractive "
    "-ExecutionPolicy Bypass -Command). This is PowerShell syntax, not bash. "
    "Output is truncated to 2000 lines or 50 KB; the full output is saved to a "
    "temp file. Returns the exit code.";
static const char bash_params[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"Shell command to run\"},"
    "\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout in milliseconds\"}},"
    "\"required\":[\"command\"]}";

/* AGENTC_TOOL_CORE is internal (include/agent.h): it marks the builtins as
 * core-owned so the app's default selection never hides them. */
static const AgcTool builtins[] = {
    { "read", "Read", read_desc, read_params, AGENTC_TOOL_READONLY | AGENTC_TOOL_CORE, NULL,
      .run = read_run },
    { "bash", "Bash", bash_desc_sh, bash_params, AGENTC_TOOL_CORE, NULL,
      .start = bash_start, .step = bash_step },
    { "edit", "Edit", edit_desc, edit_params,
      AGENTC_TOOL_DESTRUCTIVE | AGENTC_TOOL_SEQUENTIAL | AGENTC_TOOL_CORE, NULL, .run = edit_run },
    { "write", "Write", write_desc, write_params,
      AGENTC_TOOL_DESTRUCTIVE | AGENTC_TOOL_SEQUENTIAL | AGENTC_TOOL_CORE, NULL, .run = write_run },
    { "ls", "List", ls_desc, ls_params, AGENTC_TOOL_READONLY | AGENTC_TOOL_CORE, NULL,
      .run = agentc_tool_ls_run },
    { "find", "Find", NULL, find_params, AGENTC_TOOL_READONLY | AGENTC_TOOL_CORE, NULL,
      .run = agentc_tool_find_run },
    { "grep", "Grep", NULL, grep_params, AGENTC_TOOL_READONLY | AGENTC_TOOL_CORE, NULL,
      .run = agentc_tool_grep_run },
};

/* returns the builtin count when out == NULL; otherwise fills min(count, max)
 * entries and returns that count */
size_t agentc_tools_builtin(AgcTool *out, size_t max) {
    size_t n = sizeof builtins / sizeof builtins[0];
    if (out == NULL) return n;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = builtins[i];
    for (size_t i = 0; i < n; i++) {
        if (agentc_streq(out[i].name, "bash")) {
            const char *k = os_shell_kind(agentc_config_shell());
            if (k != NULL && agentc_streq(k, "cmd"))
                out[i].desc = bash_desc_cmd;
            else if (k != NULL && (agentc_streq(k, "powershell") || agentc_streq(k, "pwsh")))
                out[i].desc = bash_desc_ps;
            else
                out[i].desc = bash_desc_sh;
        } else if (agentc_streq(out[i].name, "grep")) {
            out[i].desc = agentc_tool_engine_desc_for("grep", AGENTC_ENGINE_BUILTIN);
        } else if (agentc_streq(out[i].name, "find")) {
            out[i].desc = agentc_tool_engine_desc_for("find", AGENTC_ENGINE_BUILTIN);
        }
    }
    return n;
}

/* ------------------------------------------------------- builtin-tools ext */

static int builtin_tools_init(const AgcExtHost *host) {
    (void)host;
    int mode = agentc_tool_engine_parse(agentc_config_tools_engine());
    if (mode < 0) {
        agentc_logf(2, "tools: invalid engine '%s'; using external",
                    agentc_config_tools_engine());
        mode = AGENTC_TOOL_ENGINE_EXTERNAL;
    }
    agentc_tool_engine_init(mode, os_getenv("PATH"));

    AgcTool tmp[sizeof builtins / sizeof builtins[0]];
    size_t n = agentc_tools_builtin(tmp, sizeof tmp / sizeof tmp[0]);
    for (size_t i = 0; i < n; i++) {
        AgcToolEngineSel sel;
        if (agentc_tool_engine_select(tmp[i].name, &sel)) {
            tmp[i].desc = sel.desc;
            if (sel.run != NULL) tmp[i].run = sel.run;
        }
        (void)agentc_ext_add_tool_internal(&tmp[i]);
    }
    return 0;
}

static const AgcExt g_builtin_tools = {
    .abi_version = AGENTC_EXT_ABI,
    .struct_size = sizeof(AgcExt),
    .name = "builtin-tools",
    .version = "1",
    .order = 0,
    .init = builtin_tools_init,
};

const AgcExt *agentc_builtin_tools_ext(void) { return &g_builtin_tools; }
