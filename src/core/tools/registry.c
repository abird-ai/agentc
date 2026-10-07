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
#include "base/limits.h"
#include "ext.h"
#include "core/tools/args.h"
#include "core/tools/engine.h"
#include "core/agent_int.h"
#include "ext/registry_int.h"

char *agentc_tool_read_err(bool *is_error, const char *fmt, ...);

/* v2 async pair owned by bash.c; the job driver drives it through jobs.h. */
int bash_start(const AgcTool *self, const AgcToolCall *call, AgcJob *job);
int bash_step(const AgcTool *self, AgcJob *job);

/* --------------------------------------------------------------- helpers */

/* P6-C Layer 4: re-check the resolved path against the owning envelope after
 * realpath so `..` and symlinks cannot escape. A failure is a normal tool error
 * (never a provider error) and names the access mode. */
static bool trust_path_gate(const AgcToolCall *call, const char *tool, const char *path, bool write,
                            AgcBuf *out, bool *is_error) {
    if (!call || call->env == NULL)
        return true;
    if (agentc_trust_path_ok(call->env, path, write))
        return true;
    const char *why = (write && call->env->access == AGENTC_ACCESS_RO)
                          ? "read-only access denies writes"
                          : "path is outside the trust boundary";
    agentc_buf_printf(out, "error: %s: trust_denied: %s", tool, why);
    if (is_error)
        *is_error = true;
    return false;
}

/* A present but non-numeric field must keep the exact legacy error text, so
 * the callers check the node type before falling back to the defaulting
 * getter (present+non-num -> dflt). */
static bool wrong_type(const AgcToolArgs *a, const char *key) {
    AgcJson *v = agentc_json_get(a->root, key);
    return v != NULL && agentc_json_type(v) != AGENTC_JSON_NUM;
}

static int args_oom(bool *is_error) {
    if (is_error)
        *is_error = true;
    return -12; /* ENOMEM */
}

static void append_owned(AgcBuf *out, char *res, bool err, bool *is_error) {
    if (res != NULL) {
        agentc_buf_cstr(out, res);
        agentc_free(res);
    }
    if (is_error)
        *is_error = err;
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

static int read_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error) {
    (void)self;
    if (is_error)
        *is_error = false;
    AgcToolArgs a;
    if (agentc_tool_args_parse(&a, call->args_json,
                               call->args_json ? agentc_strlen(call->args_json) : 0) != 0)
        return args_oom(is_error);
    if (a.root == NULL) {
        agentc_buf_cstr(out, "error: read: arguments must be a JSON object");
        if (is_error)
            *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    const char *path = agentc_tool_args_str(&a, "path");
    if (path == NULL) {
        agentc_tool_args_missing(out, "read", "path", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }
    if (!trust_path_gate(call, "read", path, false, out, is_error)) {
        agentc_tool_args_free(&a);
        return 0;
    }
    if (wrong_type(&a, "offset") || wrong_type(&a, "limit")) {
        agentc_buf_cstr(out, "error: read: offset and limit must be numbers");
        if (is_error)
            *is_error = true;
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

static int edit_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error) {
    (void)self;
    if (is_error)
        *is_error = false;
    AgcToolArgs a;
    if (agentc_tool_args_parse(&a, call->args_json,
                               call->args_json ? agentc_strlen(call->args_json) : 0) != 0)
        return args_oom(is_error);
    if (a.root == NULL) {
        agentc_buf_cstr(out, "error: edit: arguments must be a JSON object");
        if (is_error)
            *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    const char *path = agentc_tool_args_str(&a, "path");
    if (path == NULL) {
        agentc_tool_args_missing(out, "edit", "path", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }
    if (!trust_path_gate(call, "edit", path, true, out, is_error)) {
        agentc_tool_args_free(&a);
        return 0;
    }
    AgcJson *edits = agentc_json_get(a.root, "edits");
    if (agentc_json_type(edits) != AGENTC_JSON_ARR) {
        agentc_buf_cstr(out, "error: edit: edits must be an array");
        if (is_error)
            *is_error = true;
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

static int write_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error) {
    (void)self;
    if (is_error)
        *is_error = false;
    AgcToolArgs a;
    if (agentc_tool_args_parse(&a, call->args_json,
                               call->args_json ? agentc_strlen(call->args_json) : 0) != 0)
        return args_oom(is_error);
    if (a.root == NULL) {
        agentc_buf_cstr(out, "error: write: arguments must be a JSON object");
        if (is_error)
            *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    const char *path = agentc_tool_args_str(&a, "path");
    if (path == NULL) {
        agentc_tool_args_missing(out, "write", "path", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }
    if (!trust_path_gate(call, "write", path, true, out, is_error)) {
        agentc_tool_args_free(&a);
        return 0;
    }
    size_t content_len = 0;
    const char *content = agentc_tool_args_str_len(&a, "content", &content_len);
    if (content == NULL) {
        agentc_tool_args_missing(out, "write", "content", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }
    bool err = false;
    char *res = agentc_tool_write_len(path, content, content_len, &err);
    append_owned(out, res, err, is_error);
    agentc_tool_args_free(&a);
    return 0;
}

/* ------------------------------------------------------- ls/find/grep */
int agentc_tool_ls_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error);
int agentc_tool_find_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error);
int agentc_tool_grep_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error);

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
    "\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout in milliseconds\"},"
    "\"background\":{\"type\":\"boolean\",\"description\":\"Return {actor_id} immediately and keep "
    "it running\"},"
    "\"yield_ms\":{\"type\":\"integer\",\"description\":\"Run at most this long, then return the "
    "actor_id\"}},"
    "\"required\":[\"command\"]}";

/* --------------------------------------------------------------- process */
static const char process_desc[] =
    "Manage background processes (process actors). op=spawn starts a shell "
    "command and returns an actor_id; op=read drains its new output; "
    "op=write sends stdin (tty only); op=resize/kill/freeze/resume/list manage "
    "them. A spawned process survives the tool batch and the step.";
static const char process_params[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"op\":{\"type\":\"string\",\"description\":\"spawn|write|read|resize|kill|freeze|resume|"
    "list\"},"
    "\"command\":{\"type\":\"string\",\"description\":\"Shell command (spawn)\"},"
    "\"shell\":{\"type\":\"string\",\"description\":\"auto|sh|cmd|powershell|pwsh\"},"
    "\"actor_id\":{\"type\":\"integer\"},"
    "\"data\":{\"type\":\"string\",\"description\":\"bytes for op=write\"},"
    "\"close_stdin\":{\"type\":\"boolean\"},"
    "\"tty\":{\"type\":\"boolean\",\"description\":\"allocate a PTY (writable stdin)\"},"
    "\"cols\":{\"type\":\"integer\"},\"rows\":{\"type\":\"integer\"},"
    "\"timeout\":{\"type\":\"integer\",\"description\":\"hard cap ms (0 = 30 min)\"},"
    "\"stage\":{\"type\":\"integer\",\"description\":\"kill stage: 1 graceful, 2 force\"},"
    "\"label\":{\"type\":\"string\"}},"
    "\"required\":[\"op\"]}";

static int process_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error) {
    (void)self;
    if (is_error)
        *is_error = false;
    AgcToolArgs a;
    if (agentc_tool_args_parse(&a, call->args_json,
                               call->args_json ? agentc_strlen(call->args_json) : 0) != 0)
        return -12;
    if (a.root == NULL) {
        agentc_buf_cstr(out, "error: process: arguments must be a JSON object");
        if (is_error)
            *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    const char *op = agentc_tool_args_str(&a, "op");
    if (op == NULL) {
        agentc_tool_args_missing(out, "process", "op", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }

    if (agentc_streq(op, "spawn")) {
        const char *command = agentc_tool_args_str(&a, "command");
        if (command == NULL) {
            agentc_tool_args_missing(out, "process", "command", is_error);
            agentc_tool_args_free(&a);
            return 0;
        }
        AgcProcSpawn spec;
        agentc_memset(&spec, 0, sizeof spec);
        spec.command = command;
        spec.shell = agentc_tool_args_str(&a, "shell");
        spec.mode = agentc_tool_args_bool(&a, "tty", false) ? AGENTC_PROC_PTY : AGENTC_PROC_PIPE;
        spec.cols = (int)agentc_tool_args_int(&a, "cols", 80);
        spec.rows = (int)agentc_tool_args_int(&a, "rows", 24);
        spec.deadline_ms = agentc_tool_args_int(&a, "timeout", 0);
        spec.label = agentc_tool_args_str(&a, "label");
        spec.env = call->env;
        char err[256];
        AgcActorSpec aspec = {.proc = &spec, .agent = NULL};
        AgcActorId aid = agentc_actor_spawn(&aspec, err, sizeof err);
        if (aid == 0) {
            agentc_buf_printf(out, "error: process: %s", err[0] ? err : "spawn failed");
            if (is_error)
                *is_error = true;
        } else {
            agentc_buf_printf(out, "{\"actor_id\":%u}", (unsigned)aid);
        }
        agentc_tool_args_free(&a);
        return 0;
    }

    if (agentc_streq(op, "list")) {
        AgcActor rows[AGENTC_ACTOR_MAX];
        size_t n = agentc_actor_list(rows, AGENTC_ACTOR_MAX);
        for (size_t i = 0; i < n; i++) {
            agentc_buf_printf(out, "%u\t%s\t%s\t%d\t%s\n", (unsigned)rows[i].id,
                              rows[i].kind == AGENTC_ACTOR_PROCESS ? "process" : "agent",
                              agentc_actor_state_name(rows[i].state),
                              rows[i].proc != NULL ? rows[i].proc->pid : 0,
                              rows[i].label != NULL ? rows[i].label : "");
        }
        if (n == 0)
            agentc_buf_cstr(out, "(no actors)\n");
        agentc_tool_args_free(&a);
        return 0;
    }

    AgcActorId id = (AgcActorId)agentc_tool_args_int(&a, "actor_id", 0);
    if (id == 0 || !agentc_actor_exists(id)) {
        agentc_buf_cstr(out, "error: process: actor_not_found");
        if (is_error)
            *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }

    int r = 0;
    if (agentc_streq(op, "write")) {
        size_t dlen = 0;
        const char *data = agentc_tool_args_str_len(&a, "data", &dlen);
        if (data == NULL) {
            agentc_tool_args_missing(out, "process", "data", is_error);
            agentc_tool_args_free(&a);
            return 0;
        }
        r = agentc_process_write(id, data, dlen, agentc_tool_args_bool(&a, "close_stdin", false));
    } else if (agentc_streq(op, "read")) {
        AgcBuf tmp = {0};
        (void)agentc_process_read(id, &tmp, 0);
        if (tmp.len == 0)
            agentc_buf_cstr(out, "(no new output)\n");
        else
            agentc_buf_push(out, tmp.p, tmp.len);
        agentc_buf_free(&tmp);
        AgcProcInfo info;
        agentc_process_info(id, &info);
        agentc_buf_printf(out, "[state: %s, exit: %d]\n",
                          agentc_actor_state_name((AgcActorState)info.state), info.exit_code);
        agentc_tool_args_free(&a);
        return 0;
    } else if (agentc_streq(op, "resize")) {
        r = agentc_process_resize(id, (int)agentc_tool_args_int(&a, "cols", 0),
                                  (int)agentc_tool_args_int(&a, "rows", 0));
    } else if (agentc_streq(op, "kill")) {
        r = agentc_process_kill(id, (int)agentc_tool_args_int(&a, "stage", 1));
    } else if (agentc_streq(op, "freeze")) {
        r = agentc_actor_freeze(id, AGENTC_FREEZE_ACTOR, 0);
    } else if (agentc_streq(op, "resume")) {
        r = agentc_actor_resume(id, AGENTC_FREEZE_ACTOR);
    } else {
        agentc_buf_printf(out, "error: process: unknown op '%s'", op);
        if (is_error)
            *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    if (r < 0) {
        agentc_buf_printf(out, "error: process: %s failed (%d)", op, r);
        if (is_error)
            *is_error = true;
    } else {
        agentc_buf_cstr(out, "ok\n");
    }
    agentc_tool_args_free(&a);
    return 0;
}

/* AGENTC_TOOL_CORE is internal (include/agent.h): it marks the builtins as
 * core-owned so the app's default selection never hides them. */
/* --------------------------------------------------------------- queue */
static const char queue_desc[] =
    "Manage an agent's pending input queue. op=add enqueues text for a turn "
    "with a signed priority (negative preempts); op=list/update/delete/reorder/"
    "start/pause/resume/clear manage it.";
static const char queue_params[] = "{\"type\":\"object\",\"properties\":{"
                                   "\"op\":{\"type\":\"string\",\"description\":\"add|list|update|"
                                   "delete|reorder|start|pause|resume|clear\"},"
                                   "\"actor_id\":{\"type\":\"integer\"},"
                                   "\"text\":{\"type\":\"string\"},"
                                   "\"id\":{\"type\":\"integer\"},"
                                   "\"turn\":{\"type\":\"integer\"},"
                                   "\"priority\":{\"type\":\"integer\"},"
                                   "\"flags\":{\"type\":\"integer\"},"
                                   "\"client_id\":{\"type\":\"string\"},"
                                   "\"dedupe\":{\"type\":\"boolean\"},"
                                   "\"ids\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},"
                                   "\"paused\":{\"type\":\"boolean\"}},"
                                   "\"required\":[\"op\"]}";

static void queue_emit_item_json(AgcJsonW *w, const AgcQueueItem *it) {
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "id");
    agentc_jsonw_u64(w, it->id);
    agentc_jsonw_key(w, "turn");
    agentc_jsonw_i64(w, it->turn);
    agentc_jsonw_key(w, "priority");
    agentc_jsonw_i64(w, it->priority);
    agentc_jsonw_key(w, "flags");
    agentc_jsonw_u64(w, it->flags);
    agentc_jsonw_key(w, "text");
    agentc_jsonw_cstr(w, it->text ? it->text : "");
    if (it->client_id) {
        agentc_jsonw_key(w, "client_id");
        agentc_jsonw_cstr(w, it->client_id);
    }
    agentc_jsonw_end(w);
}

static int queue_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error) {
    (void)self;
    if (is_error)
        *is_error = false;
    AgcToolArgs a;
    if (agentc_tool_args_parse(&a, call->args_json,
                               call->args_json ? agentc_strlen(call->args_json) : 0) != 0)
        return -12;
    if (a.root == NULL) {
        agentc_buf_cstr(out, "error: queue: arguments must be a JSON object");
        if (is_error)
            *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    const char *op = agentc_tool_args_str(&a, "op");
    if (op == NULL) {
        agentc_tool_args_missing(out, "queue", "op", is_error);
        agentc_tool_args_free(&a);
        return 0;
    }
    AgcActorId id = (AgcActorId)agentc_tool_args_int(&a, "actor_id", 0);
    if (id == 0 || !agentc_actor_exists(id)) {
        agentc_buf_cstr(out, "error: queue: actor_not_found");
        if (is_error)
            *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }

    if (agentc_streq(op, "list")) {
        AgcJsonW w;
        agentc_jsonw_init(&w, out);
        agentc_jsonw_arr(&w);
        AgcQueueItem items[AGENTC_QUEUE_MAX];
        int n = agentc_queue_list(id, items, AGENTC_QUEUE_MAX);
        if (n < 0)
            n = 0;
        for (int i = 0; i < n; i++)
            queue_emit_item_json(&w, &items[i]);
        agentc_jsonw_end(&w);
        agentc_tool_args_free(&a);
        return 0;
    }
    if (agentc_streq(op, "add")) {
        const char *text = agentc_tool_args_str(&a, "text");
        if (text == NULL) {
            agentc_tool_args_missing(out, "queue", "text", is_error);
            agentc_tool_args_free(&a);
            return 0;
        }
        const char *cid = agentc_tool_args_str(&a, "client_id");
        uint32_t fl = (uint32_t)agentc_tool_args_int(&a, "flags", 0) |
                      (agentc_tool_args_bool(&a, "dedupe", false) ? AGENTC_Q_F_DEDUPE : 0u);
        bool deduped =
            cid && cid[0] && (fl & AGENTC_Q_F_DEDUPE) && agentc_queue_has_client(id, cid);
        uint64_t iid =
            agentc_queue_add(id, text, agentc_tool_args_int(&a, "turn", 0),
                             (int32_t)agentc_tool_args_int(&a, "priority", 0), fl, cid);
        if (iid == 0) {
            agentc_buf_cstr(out, "error: queue: queue_full");
            if (is_error)
                *is_error = true;
        } else if (deduped) {
            agentc_buf_printf(out, "{\"id\":%llu,\"deduped\":true}",
                              (unsigned long long)iid);
        } else {
            agentc_buf_printf(out, "{\"id\":%llu}", (unsigned long long)iid);
        }
        agentc_tool_args_free(&a);
        return 0;
    }

    uint64_t item = (uint64_t)agentc_tool_args_int(&a, "id", 0);
    int r = 0;
    if (agentc_streq(op, "update")) {
        r = agentc_queue_update(id, item, agentc_tool_args_str(&a, "text"),
                                agentc_tool_args_int(&a, "turn", -1),
                                (int32_t)agentc_tool_args_int(&a, "priority", 0));
    } else if (agentc_streq(op, "delete")) {
        r = agentc_queue_delete(id, item);
    } else if (agentc_streq(op, "start")) {
        r = agentc_queue_start(id, item);
    } else if (agentc_streq(op, "pause") || agentc_streq(op, "resume")) {
        r = agentc_queue_pause(id, agentc_streq(op, "pause"));
    } else if (agentc_streq(op, "clear")) {
        r = agentc_queue_clear(id);
    } else if (agentc_streq(op, "reorder")) {
        const AgcJson *ids = agentc_json_get(a.root, "ids");
        uint64_t vals[AGENTC_QUEUE_MAX];
        size_t n = 0;
        if (agentc_json_type(ids) == AGENTC_JSON_ARR) {
            size_t total = agentc_json_len(ids);
            for (size_t i = 0; i < total && n < AGENTC_QUEUE_MAX; i++) {
                size_t l = 0;
                const char *s = agentc_json_num(agentc_json_at(ids, i), &l);
                bool ok = false;
                uint64_t v = agentc_parse_u64(s ? s : "0", s ? l : 1, &ok);
                if (ok)
                    vals[n++] = v;
            }
        }
        r = agentc_queue_reorder(id, vals, n);
    } else {
        agentc_buf_printf(out, "error: queue: unknown op '%s'", op);
        if (is_error)
            *is_error = true;
        agentc_tool_args_free(&a);
        return 0;
    }
    if (r < 0) {
        agentc_buf_printf(out, "error: queue: %s", agentc_msg_err_name(r));
        if (is_error)
            *is_error = true;
    } else {
        agentc_buf_cstr(out, "{\"count\":");
        agentc_buf_u64(out, (uint64_t)agentc_queue_count(id));
        agentc_buf_cstr(out, "}");
    }
    agentc_tool_args_free(&a);
    return 0;
}

/* --------------------------------------------------------------- agent */
static const char agent_desc[] =
    "Manage sub-agents (agent actors). op=spawn starts a child with the parent's "
    "current provider/model/transport and returns its result (wait:true, the "
    "default) or {agent_id} (wait:false); op=wait/result read a child; "
    "op=send steers it; op=interrupt/freeze/resume/list manage the tree.";
static const char agent_params[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"op\":{\"type\":\"string\",\"description\":\"spawn|wait|send|interrupt|freeze|resume|result|"
    "list\"},"
    "\"task\":{\"type\":\"string\",\"description\":\"initial task (spawn)\"},"
    "\"model\":{\"type\":\"string\"},\"provider\":{\"type\":\"string\"},"
    "\"thinking\":{\"type\":\"integer\"},\"trust\":{\"type\":\"string\"},"
    "\"wait\":{\"type\":\"boolean\"},\"inherit_context\":{\"type\":\"boolean\"},"
    "\"label\":{\"type\":\"string\"},"
    "\"agent_id\":{\"type\":\"integer\"},\"text\":{\"type\":\"string\"},"
    "\"turn\":{\"type\":\"integer\"},\"priority\":{\"type\":\"integer\"},"
    "\"flags\":{\"type\":\"integer\"},"
    "\"client_id\":{\"type\":\"string\"},\"dedupe\":{\"type\":\"boolean\"},"
    "\"scope\":{\"type\":\"string\",\"description\":\"step|actor\"},"
    "\"stage\":{\"type\":\"integer\"},\"timeout_ms\":{\"type\":\"integer\"}},"
    "\"required\":[\"op\"]}";

static const AgcTool builtins[] = {
    {"read", "Read", read_desc, read_params, AGENTC_TOOL_READONLY | AGENTC_TOOL_CORE, NULL,
     .run = read_run},
    {"bash", "Bash", bash_desc_sh, bash_params, AGENTC_TOOL_CORE, NULL, .start = bash_start,
     .step = bash_step},
    {"edit", "Edit", edit_desc, edit_params,
     AGENTC_TOOL_DESTRUCTIVE | AGENTC_TOOL_SEQUENTIAL | AGENTC_TOOL_CORE, NULL, .run = edit_run},
    {"write", "Write", write_desc, write_params,
     AGENTC_TOOL_DESTRUCTIVE | AGENTC_TOOL_SEQUENTIAL | AGENTC_TOOL_CORE, NULL, .run = write_run},
    {"ls", "List", ls_desc, ls_params, AGENTC_TOOL_READONLY | AGENTC_TOOL_CORE, NULL,
     .run = agentc_tool_ls_run},
    {"find", "Find", NULL, find_params, AGENTC_TOOL_READONLY | AGENTC_TOOL_CORE, NULL,
     .run = agentc_tool_find_run},
    {"grep", "Grep", NULL, grep_params, AGENTC_TOOL_READONLY | AGENTC_TOOL_CORE, NULL,
     .run = agentc_tool_grep_run},
    /* Appended after the legacy seven so existing positional consumers keep
     * their indices; the process tool is P6-A's only new model-facing tool. */
    {"process", "Process", process_desc, process_params, AGENTC_TOOL_CORE, NULL,
     .run = process_run},
    /* P6-B queue primitive (hidden until enabled; P6-SIGNALS §11). */
    {"queue", "Queue", queue_desc, queue_params, AGENTC_TOOL_CORE | AGENTC_TOOL_HIDDEN, NULL,
     .run = queue_run},
    /* P6-D agent primitive (R-D26): spawn/wait/send/interrupt/freeze/resume/
     * result/list over the agent tree. */
    {"agent", "Agent", agent_desc, agent_params, AGENTC_TOOL_CORE, NULL, .run = agent_run},
};

/* returns the builtin count when out == NULL; otherwise fills min(count, max)
 * entries and returns that count. This is the single selection path: the
 * process-global shell variant for `bash` and the resolved engine's desc+run for
 * `grep`/`find` are applied here exactly once, so `builtin_tools_init` need not
 * (and must not) patch the rows again. Tests get the same effective rows. */
size_t agentc_tools_builtin(AgcTool *out, size_t max) {
    size_t n = sizeof builtins / sizeof builtins[0];
    if (out == NULL)
        return n;
    if (n > max)
        n = max;
    for (size_t i = 0; i < n; i++) {
        out[i] = builtins[i];
        if (agentc_streq(out[i].name, "bash")) {
            const char *k = os_shell_kind(agentc_config_shell());
            if (k != NULL && agentc_streq(k, "cmd"))
                out[i].desc = bash_desc_cmd;
            else if (k != NULL && (agentc_streq(k, "powershell") || agentc_streq(k, "pwsh")))
                out[i].desc = bash_desc_ps;
            else
                out[i].desc = bash_desc_sh;
            continue;
        }
        AgcToolEngineSel sel;
        if (agentc_tool_engine_select(out[i].name, &sel)) {
            out[i].desc = sel.desc;
            if (sel.run != NULL)
                out[i].run = sel.run;
        }
    }
    return n;
}

/* ------------------------------------------------------- builtin-tools ext */

static int builtin_tools_init(const AgcExtHost *host) {
    (void)host;
    int mode = agentc_tool_engine_parse(agentc_config_tools_engine());
    if (mode < 0) {
        agentc_logf(2, "tools: invalid engine '%s'; using external", agentc_config_tools_engine());
        mode = AGENTC_TOOL_ENGINE_EXTERNAL;
    }
    agentc_tool_engine_init(mode, os_getenv("PATH"));

    /* agentc_tools_builtin() already applied the engine/shell selection, so the
     * rows are registered exactly as returned: no second patch path. */
    AgcTool tmp[sizeof builtins / sizeof builtins[0]];
    size_t n = agentc_tools_builtin(tmp, sizeof tmp / sizeof tmp[0]);
    for (size_t i = 0; i < n; i++)
        (void)agentc_ext_add_tool_internal(&tmp[i]);
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

const AgcExt *agentc_builtin_tools_ext(void) {
    return &g_builtin_tools;
}
