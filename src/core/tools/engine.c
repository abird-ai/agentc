/* engine.c — core-tool backend engine (see core/tools/engine.h, P5-TOOL-ENGINE).
 *
 * One ordered descriptor table is the single source of truth: per rung it
 * carries the binary, the POSIX-only / exit-1-means-no-match flags, the display
 * token, the description text and the typed argv builder. Resolution, logging,
 * reset and argv lookup all iterate that table; adding a tool or a rung is a
 * table edit. There is no per-tool resolve function.
 *
 * External rungs are driven by one shared process reader (`proc_spawn` /
 * `proc_pump` / `proc_error`): spawn an argv (no shell, explicit empty
 * environment), read both pipes non-blockingly, honor agentc_tool_checkpoint(),
 * and always kill + reap the process group before returning.
 * Output is bounded by AGENTC_LIMIT_TOOL_BYTES while it is collected; grep
 * collects records so context is only emitted around accepted matches, exactly
 * reproducing the built-in window.
 */
#include "core/tools/engine.h"

#include "base/limits.h"
#include "core/tools/jobs.h"
#include "plat.h"

/* Reader bounds: stderr is capped here; stdout lines are buffered up to the
 * shared pending cap (AGENTC_LIMIT_GREP_PENDING, 16 MiB). A line under the cap
 * still yields its 500-char prefix; a line that reaches the cap stops the
 * reader with the byte marker rather than parsing a truncated record (see
 * grep_sink). */
#define ENGINE_MAX_STDERR 4096

#define ENGINE_EINVAL (-22)

/* ------------------------------------------------------------ descriptions */
/* Descriptions verbatim (single line each). The built-in variants are the registry
 * text with " (built-in engine)" appended. */
static const char rg_desc[] =
    "Search file contents with ripgrep (rg). `pattern` is a Rust regular expression "
    "(backreferences and lookaround are not supported); set `literal: true` for plain "
    "text. Respects `.gitignore`/`.ignore` and skips hidden files unless `path` names "
    "one. `glob` is a gitignore-style filter passed to `--glob`. Output is "
    "`path:line:text` with an optional context window; lines are capped at 500 "
    "characters.";

static const char grep_desc[] =
    "Search file contents with the system grep (POSIX extended regex: no `\\d` "
    "classes, no lazy quantifiers); set `literal: true` for plain text. Ignored and "
    "hidden files are searched (`.git` is skipped best-effort); binary files are "
    "skipped. `glob` is a shell-style file-name filter passed to `--include`. Output "
    "is `path:line:text` with an optional context window; lines are capped at 500 "
    "characters.";

static const char grep_builtin_desc[] =
    "Search file contents with a literal string or a small regex (., *, +, ?, ^, $, "
    "[abc], [^abc], \\ escapes). Output is path:line:text with an optional context "
    "window; lines are capped at 500 characters. (built-in engine)";

static const char fd_desc[] =
    "Find files whose path or name matches a glob pattern (`*`, `?`, `**`, `[abc]`). "
    "Uses fd: respects `.gitignore`, skips hidden entries and does not follow symlinked "
    "directories. Output is one path per line.";

static const char find_posix_desc[] =
    "Find files with the system find. Pattern translation is approximate (`**` may not "
    "match zero directories; a pattern with `/` is matched with `-path` against the "
    "path as printed by find, so under the default `.` a root-relative pattern may not "
    "match); `.gitignore` and hidden files are not skipped. Output is one path per "
    "line.";

static const char find_builtin_desc[] =
    "Find files whose path or name matches a glob pattern (*, ?, **, [abc]). Respects "
    ".gitignore files, does not follow symlinked directories and is bounded to 8000 "
    "visited entries. (built-in engine)";

/* --------------------------------------------------------- descriptor table */
typedef int (*AgcEngineArgvFn)(const void *args, AgcVec *argv);

typedef struct {
    const char *tool;    /* "grep" | "find" */
    const char *backend; /* AGENTC_ENGINE_* */
    const char *binary;  /* PATH name, NULL for the built-in rung */
    bool posix_only;     /* never attempted on Windows */
    bool exit1_nomatch;  /* exit 1 means "no match", not an error */
    const char *display; /* info-line token */
    const char *desc;
    AgcEngineArgvFn build_argv; /* NULL for the built-in rung */
    int (*run)(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error);
} AgcEngineEntry;

/* Table order is the chain: the first row that resolves wins; the built-in row
 * is the unconditional last resort. */
static int build_argv_grep_rg(const void *args, AgcVec *argv);
static int build_argv_grep_posix(const void *args, AgcVec *argv);
static int build_argv_find_fd(const void *args, AgcVec *argv);
static int build_argv_find_posix(const void *args, AgcVec *argv);
static int grep_rg_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                       bool *is_error);
static int grep_posix_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                          bool *is_error);
static int find_fd_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                       bool *is_error);
static int find_posix_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                          bool *is_error);

static const AgcEngineEntry g_engine[] = {
    { "grep", AGENTC_ENGINE_RG, "rg", false, true, "rg (Rust regex)", rg_desc,
      build_argv_grep_rg, grep_rg_run },
    { "grep", AGENTC_ENGINE_GREP, "grep", true, true, "grep (POSIX)", grep_desc,
      build_argv_grep_posix, grep_posix_run },
    { "grep", AGENTC_ENGINE_BUILTIN, NULL, false, false, "built-in", grep_builtin_desc, NULL,
      NULL },
    { "find", AGENTC_ENGINE_FD, "fd", false, true, "fd (glob)", fd_desc, build_argv_find_fd,
      find_fd_run },
    { "find", AGENTC_ENGINE_FIND, "find", true, false, "find (POSIX)", find_posix_desc,
      build_argv_find_posix, find_posix_run },
    { "find", AGENTC_ENGINE_BUILTIN, NULL, false, false, "built-in", find_builtin_desc, NULL,
      NULL },
};

/* -------------------------------------------------------------- pin state */
typedef struct {
    const char *tool;
    const AgcEngineEntry *sel;
    char bin[4096];
} AgcEnginePin;

static AgcEnginePin g_pins[8];
static size_t g_npins;
static bool g_initialized;
/* Whether the pinned POSIX grep understands `-Z` (NUL file-name delimiter).
 * Probed once at init only for the POSIX grep row; cleared by reset. */
static bool g_grep_null;

static const AgcEngineEntry *entry_lookup(const char *tool, const char *backend) {
    if (tool == NULL || backend == NULL) return NULL;
    for (size_t i = 0; i < sizeof g_engine / sizeof g_engine[0]; i++)
        if (agentc_streq(g_engine[i].tool, tool) &&
            agentc_streq(g_engine[i].backend, backend))
            return &g_engine[i];
    return NULL;
}

static const AgcEngineEntry *entry_builtin(const char *tool) {
    return entry_lookup(tool, AGENTC_ENGINE_BUILTIN);
}

static const AgcEnginePin *pin_selected(const char *tool) {
    if (tool == NULL) return NULL;
    for (size_t i = 0; i < g_npins; i++)
        if (agentc_streq(g_pins[i].tool, tool)) return &g_pins[i];
    return NULL;
}

static const AgcEngineEntry *entry_selected(const char *tool) {
    const AgcEnginePin *p = pin_selected(tool);
    if (p != NULL) return p->sel;
    return entry_builtin(tool);
}

static const char *pin_bin(const char *tool) {
    const AgcEnginePin *p = pin_selected(tool);
    return p != NULL ? p->bin : NULL;
}

static void pin_add(const char *tool, const AgcEngineEntry *sel, const char *bin) {
    if (g_npins >= sizeof g_pins / sizeof g_pins[0]) return;
    AgcEnginePin *p = &g_pins[g_npins++];
    p->tool = tool;
    p->sel = sel;
    p->bin[0] = 0;
    if (bin != NULL) {
        size_t n = agentc_strlen(bin);
        if (n >= sizeof p->bin) n = sizeof p->bin - 1;
        agentc_memcpy(p->bin, bin, n);
        p->bin[n] = 0;
    }
}

/* Probe whether the pinned POSIX grep accepts `-Z`/`--null` (a NUL after the
 * file name) by observing the exact behavior the reader depends on: run it over
 * a one-line temp file and require a NUL byte on stdout. Exit status alone is
 * not enough -- a grep whose usage error exits 1 (e.g. BusyBox) would look
 * "supported", and a wrong guess would silently turn every match into "no
 * matches" on this rung. argv-only, empty env, stdin/stderr /dev/null, stdout
 * piped and drained under a small byte cap; the poll and reap each cap at
 * ~100 ms; never runs on Windows (the POSIX row is posix_only). */
static bool grep_supports_null(const char *bin) {
    if (bin == NULL || bin[0] == 0) return false;
    u64 rnd = 0;
    if (os_random(&rnd, sizeof rnd) != 0) return false;
    const char *dirs[2];
    size_t ndirs = 0;
    const char *tmpdir = os_getenv("TMPDIR");
    if (tmpdir != NULL && tmpdir[0] != 0) dirs[ndirs++] = tmpdir;
    dirs[ndirs++] = "/tmp";
    const char line[] = "agentc-probe\n";
    char path[4200];
    bool have = false;
    for (size_t di = 0; di < ndirs && !have; di++) {
        int pn = agentc_snprintf(path, sizeof path, "%s/agentc-probe-%08llx", dirs[di],
                                 (unsigned long long)rnd);
        if (pn <= 0 || (size_t)pn >= sizeof path) continue;
        int fd = os_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC | OS_O_CLOEXEC, 0600);
        if (fd < 0) continue;
        int wr;
        do {
            wr = os_write(fd, line, sizeof line - 1);
        } while (wr == -4);   /* EINTR */
        have = wr == (int)(sizeof line - 1);
        os_close(fd);
        if (!have) os_unlink(path);
    }
    if (!have) return false;

    int op[2] = { -1, -1 };
    if (os_pipe(op) < 0) {
        os_unlink(path);
        return false;
    }
    int devnull_in = os_open("/dev/null", OS_O_RDONLY | OS_O_CLOEXEC, 0);
    int devnull_err = os_open("/dev/null", OS_O_WRONLY | OS_O_CLOEXEC, 0);
    char *const argv[] = { (char *)bin, "-H", "-Z", "-e", "agentc-probe", path, NULL };
    char *const envp[] = { NULL };
    int pid = os_spawn_group(argv, envp, NULL, devnull_in, op[1], devnull_err, 0);
    if (devnull_in >= 0) os_close(devnull_in);
    if (devnull_err >= 0) os_close(devnull_err);
    os_close(op[1]);

    bool ok = false;
    if (pid >= 0) {
        struct os_pollfd pfd = { op[0], OS_POLLIN, 0 };
        size_t total = 0;
        for (int i = 0; i < 20 && !ok && total < 4096; i++) {
            int pr = os_poll(&pfd, 1, 5);
            if (pr <= 0 || !(pfd.revents & (OS_POLLIN | OS_POLLHUP))) continue;
            u8 buf[512];
            for (;;) {
                if (total >= 4096) break;
                int nr = os_read(op[0], buf, sizeof buf);
                if (nr == -4) continue;   /* EINTR: retry */
                if (nr <= 0) break;
                total += (size_t)nr;
                for (int k = 0; k < nr; k++)
                    if (buf[k] == 0) { ok = true; break; }
                if (ok) break;
            }
        }
    }
    os_close(op[0]);
    if (pid >= 0) {
        bool done = false;
        for (int i = 0; i < 20 && !done; i++) {
            int w = os_wait(pid, true);
            if (w == -2 || w >= 0) { done = true; break; }
            os_poll(NULL, 0, 5);
        }
        if (!done) {
            os_kill(-pid, 9);
            (void)os_wait(pid, true);
        }
    }
    os_unlink(path);
    return ok;
}

/* --------------------------------------------------------------- mode/init */
int agentc_tool_engine_parse(const char *value) {
    if (value == NULL) return -1;
    if (agentc_streq(value, "external")) return AGENTC_TOOL_ENGINE_EXTERNAL;
    if (agentc_streq(value, "internal")) return AGENTC_TOOL_ENGINE_INTERNAL;
    return -1;
}

/* One pass over the table. For each row whose tool is not yet pinned: INTERNAL
 * skips every binary row; a POSIX-only row is skipped on Windows; the NULL
 * binary pins the built-in; otherwise the row pins when it resolves on PATH.
 * Because every tool ends with a built-in row, every managed tool ends pinned. */
void agentc_tool_engine_init(int mode, const char *path_env) {
    if (g_initialized) return;   /* first call wins */
    g_initialized = true;
    if (mode != AGENTC_TOOL_ENGINE_INTERNAL) mode = AGENTC_TOOL_ENGINE_EXTERNAL;
    g_npins = 0;

    for (size_t i = 0; i < sizeof g_engine / sizeof g_engine[0]; i++) {
        const AgcEngineEntry *e = &g_engine[i];
        if (pin_selected(e->tool) != NULL) continue;
        if (mode == AGENTC_TOOL_ENGINE_INTERNAL && e->binary != NULL) continue;
        if (e->posix_only && agentc_streq(os_platform(), "windows")) continue;
        if (e->binary == NULL) {
            pin_add(e->tool, e, NULL);
            continue;
        }
        char bin[4096];
        if (os_which_in(path_env, e->binary, bin, sizeof bin) == 0) {
            pin_add(e->tool, e, bin);
            if (agentc_streq(e->backend, AGENTC_ENGINE_GREP))
                g_grep_null = grep_supports_null(bin);
        }
    }
}

const char *agentc_tool_engine_summary(void) {
    static char buf[256];
    buf[0] = 0;
    if (!g_initialized) return buf;
    size_t off = 0;
    for (size_t i = 0; i < g_npins && off + 1 < sizeof buf; i++) {
        /* Succinct token: the resolved rung's PATH name, never the verbose
         * description token the model-facing text carries. */
        const AgcEngineEntry *e = g_pins[i].sel;
        const char *tok = (e != NULL && e->binary != NULL) ? e->binary
                                                           : AGENTC_ENGINE_BUILTIN;
        off += (size_t)agentc_snprintf_used(buf + off, sizeof buf - off, "%s%s->%s",
                                            i > 0 ? " " : "", g_pins[i].tool, tok);
    }
    if (off + 1 < sizeof buf)
        agentc_snprintf_used(buf + off, sizeof buf - off, "%sls->built-in",
                             g_npins > 0 ? " " : "");
    return buf;
}

void agentc_tool_engine_log(void) {
    if (!g_initialized) return;
    agentc_logf(1, "tools: %s", agentc_tool_engine_summary());
}

const char *agentc_tool_engine_backend(const char *tool) {
    const AgcEngineEntry *e = entry_selected(tool);
    return e != NULL ? e->backend : AGENTC_ENGINE_BUILTIN;
}

const char *agentc_tool_engine_desc_for(const char *tool, const char *backend) {
    const AgcEngineEntry *e = entry_lookup(tool, backend);
    return e != NULL ? e->desc : "";
}

void agentc_tool_engine_reset(void) {
    g_initialized = false;
    g_npins = 0;
    g_grep_null = false;
}

bool agentc_tool_engine_select(const char *tool, AgcToolEngineSel *sel) {
    if (sel == NULL) return false;
    const AgcEngineEntry *e = entry_selected(tool);
    if (e == NULL) return false;
    sel->desc = e->desc;
    sel->run = e->run;
    return true;
}

/* ------------------------------------------------------------ argv mapping */
static void argv_push(AgcVec *v, const char *s) {
    char **slot = agentc_vec_push(v, sizeof(char *));
    *slot = s != NULL ? agentc_strdup(s) : NULL;
}

static void argv_push_i64(AgcVec *v, i64 x) {
    char tmp[32];
    size_t n = agentc_fmt_i64(tmp, x);
    char **slot = agentc_vec_push(v, sizeof(char *));
    *slot = agentc_strdup_len(tmp, n);
}

static void argv_free(AgcVec *v) {
    if (v == NULL) return;
    char **a = (char **)v->p;
    for (size_t i = 0; i < v->len; i++) agentc_free(a[i]);
    agentc_vec_free(v);
}

static i64 clamp_context(i64 c) {
    if (c < 0) return 0;
    return c > AGENTC_LIMIT_GREP_CONTEXT ? AGENTC_LIMIT_GREP_CONTEXT : c;
}

static i64 clamp_limit(i64 l) {
    if (l <= 0) return AGENTC_LIMIT_GREP_MATCHES;
    return l > AGENTC_LIMIT_GREP_MATCHES_MAX ? AGENTC_LIMIT_GREP_MATCHES_MAX : l;
}

static i64 clamp_find_limit(i64 l) {
    if (l <= 0) return AGENTC_LIMIT_FIND_PATHS;
    return l > AGENTC_LIMIT_FIND_PATHS_MAX ? AGENTC_LIMIT_FIND_PATHS_MAX : l;
}

static int build_argv_grep_rg(const void *args, AgcVec *argv) {
    const AgcGrepArgs *ga = args;
    argv_push(argv, "rg");
    argv_push(argv, "--json");
    if (ga->icase) argv_push(argv, "--ignore-case");
    if (ga->literal) argv_push(argv, "-F");
    if (ga->glob != NULL && ga->glob[0] != 0) {
        argv_push(argv, "--glob");
        argv_push(argv, ga->glob);
    }
    i64 ctx = clamp_context(ga->context);
    if (ctx > 0) {
        argv_push(argv, "--context");
        argv_push_i64(argv, ctx);
    }
    /* --max-count is limit+1: rg must hand the reader one match past the cap so
     * it can observe that a match was dropped; the effective global cap
     * stays `limit`. */
    argv_push(argv, "--max-count");
    argv_push_i64(argv, clamp_limit(ga->limit) + 1);
    argv_push(argv, "--");
    argv_push(argv, ga->pattern != NULL ? ga->pattern : "");
    argv_push(argv, (ga->path != NULL && ga->path[0] != 0) ? ga->path : ".");
    argv_push(argv, NULL);
    return 0;
}

static int build_argv_grep_posix(const void *args, AgcVec *argv) {
    const AgcGrepArgs *ga = args;
    argv_push(argv, "grep");
    argv_push(argv, "-r");
    argv_push(argv, "-n");
    argv_push(argv, "-H");
    if (ga->null_sep) argv_push(argv, "-Z");
    argv_push(argv, "-I");
    argv_push(argv, ga->literal ? "-F" : "-E");
    if (ga->icase) argv_push(argv, "-i");
    if (ga->glob != NULL && ga->glob[0] != 0) {
        argv_push(argv, "--include");
        argv_push(argv, ga->glob);
    }
    i64 ctx = clamp_context(ga->context);
    if (ctx > 0) {
        argv_push(argv, "-C");
        argv_push_i64(argv, ctx);
    }
    argv_push(argv, "--exclude-dir");
    argv_push(argv, ".git");
    /* -m limit+1: the POSIX rung must also expose the dropped match. */
    argv_push(argv, "-m");
    argv_push_i64(argv, clamp_limit(ga->limit) + 1);
    argv_push(argv, "--");
    argv_push(argv, ga->pattern != NULL ? ga->pattern : "");
    argv_push(argv, (ga->path != NULL && ga->path[0] != 0) ? ga->path : ".");
    argv_push(argv, NULL);
    return 0;
}

static int build_argv_find_fd(const void *args, AgcVec *argv) {
    const AgcFindArgs *fa = args;
    argv_push(argv, "fd");
    argv_push(argv, "--glob");
    /* limit+1: hand the reader one path past the cap so it can observe the
     * dropped path (same trick as grep's --max-count); the effective global cap
     * stays `limit`. */
    argv_push(argv, "--max-results");
    argv_push_i64(argv, clamp_find_limit(fa->limit) + 1);
    argv_push(argv, "--");
    argv_push(argv, fa->pattern != NULL ? fa->pattern : "");
    argv_push(argv, (fa->path != NULL && fa->path[0] != 0) ? fa->path : ".");
    argv_push(argv, NULL);
    return 0;
}

static int build_argv_find_posix(const void *args, AgcVec *argv) {
    const AgcFindArgs *fa = args;
    const char *pattern = fa->pattern != NULL ? fa->pattern : "";
    const char *raw = (fa->path != NULL && fa->path[0] != 0) ? fa->path : ".";
    /* GNU find parses a leading '-', '!' or '(' / ')' in the first operand as
     * the start of the expression (e.g. `find -delete ...`), and POSIX find has
     * no `--` to stop that (verified: `find -- -print` still runs -print). Anchor
     * such a path with `./` so a model-supplied path can never become an action;
     * a normal path is passed through unchanged. */
    char *anchored = NULL;
    if (raw[0] == '-' || raw[0] == '!' || raw[0] == '(' || raw[0] == ')') {
        size_t n = agentc_strlen(raw);
        anchored = agentc_alloc(n + 3);
        anchored[0] = '.';
        anchored[1] = '/';
        agentc_memcpy(anchored + 2, raw, n + 1);
        raw = anchored;
    }
    argv_push(argv, "find");
    /* -H: follow a symlink named on the command line (the search root) but not
     * nested symlinked directories -- exactly the built-in/fd semantics, so a
     * symlinked `path` still lists its entries. POSIX; no effect on a real
     * directory or file operand. */
    argv_push(argv, "-H");
    argv_push(argv, raw);
    agentc_free(anchored);
    /* No slash -> match the basename; otherwise match the whole path. The
     * `-path` pattern is verbatim (the `**` cross-/ approximation is documented
     * in the description). */
    if (agentc_str_str(pattern, "/") != NULL) {
        argv_push(argv, "-path");
        argv_push(argv, pattern);
    } else {
        argv_push(argv, "-name");
        argv_push(argv, pattern);
    }
    argv_push(argv, NULL);
    return 0;
}

int agentc_tool_engine_build_argv(const char *tool, const char *backend, const void *args,
                                  AgcVec *argv) {
    if (tool == NULL || backend == NULL || args == NULL || argv == NULL) return ENGINE_EINVAL;
    const AgcEngineEntry *e = entry_lookup(tool, backend);
    if (e == NULL || e->build_argv == NULL) return ENGINE_EINVAL;
    return e->build_argv(args, argv);
}

/* ------------------------------------------------------------- normalizers */
int agentc_tool_engine_rg_hit(AgcJsonArena *scratch, const char *line, size_t n,
                              AgcGrepHit *hit) {
    if (scratch == NULL || line == NULL || n == 0 || hit == NULL) return 0;
    agentc_memset(hit, 0, sizeof *hit);
    AgcJson *root = agentc_json_parse_in(scratch, line, n);
    if (root == NULL) return 0;
    const char *type = agentc_json_get_str(root, "type");
    if (type == NULL || !(agentc_streq(type, "match") || agentc_streq(type, "context")))
        return 0;
    AgcJson *data = agentc_json_get(root, "data");
    AgcJson *pathv = agentc_json_get(data, "path");
    AgcJson *linesv = agentc_json_get(data, "lines");
    /* Non-UTF8 paths use `path.bytes` (base64); reading only `.text` is a documented delta. */
    const char *ptext = agentc_json_get_str(pathv, "text");
    const char *ltext = agentc_json_get_str(linesv, "text");
    if (ptext == NULL || ltext == NULL) return 0;
    size_t tlen = agentc_strlen(ltext);
    while (tlen > 0 && (ltext[tlen - 1] == '\n' || ltext[tlen - 1] == '\r')) tlen--;
    hit->path = ptext;
    hit->path_len = agentc_strlen(ptext);
    hit->line = (u64)agentc_json_get_int(data, "line_number", 0);
    hit->text = ltext;
    hit->text_len = tlen;
    hit->is_match = agentc_streq(type, "match");
    return 1;
}

/* Locate the first `[-:]<digits>[-:]` boundary at or after index 1. Returns the
 * separator index (between path and digits) or (size_t)-1 when there is none;
 * num_start/num_end are only meaningful on a hit. */
static size_t posix_boundary(const char *line, size_t n, size_t *num_start, size_t *num_end) {
    for (size_t i = 1; i < n; i++) {
        if (line[i] != ':' && line[i] != '-') continue;
        size_t d = i + 1, d0 = d;
        while (d < n && line[d] >= '0' && line[d] <= '9') d++;
        if (d == d0) continue;
        if (d >= n) return (size_t)-1;
        if (line[d] != ':' && line[d] != '-') continue;
        *num_start = d0;
        *num_end = d;
        return i;
    }
    return (size_t)-1;
}

/* Shared digits+trim tail for the textual and NUL-delimited POSIX parsers:
 * `sep` is the path length (index of the delimiter after the path), [ns, ne) is
 * the digit run, and line[ne] is the `:`/`-` that selects match vs context. */
static int grep_hit_tail(const char *line, size_t n, size_t sep, size_t ns, size_t ne,
                         AgcGrepHit *hit) {
    u64 lineno = 0;
    for (size_t k = ns; k < ne; k++) lineno = lineno * 10 + (u64)(line[k] - '0');
    const char *text = line + ne + 1;
    size_t tlen = n - (ne + 1);
    while (tlen > 0 && (text[tlen - 1] == '\n' || text[tlen - 1] == '\r')) tlen--;
    hit->path = line;
    hit->path_len = sep;
    hit->line = lineno;
    hit->text = text;
    hit->text_len = tlen;
    hit->is_match = line[ne] == ':';
    return 1;
}

int agentc_tool_engine_grep_hit(const char *line, size_t n, AgcGrepHit *hit) {
    if (line == NULL || n == 0 || hit == NULL) return 0;
    agentc_memset(hit, 0, sizeof *hit);
    size_t ns = 0, ne = 0;
    size_t sep = posix_boundary(line, n, &ns, &ne);
    if (sep == (size_t)-1) return 0;
    return grep_hit_tail(line, n, sep, ns, ne, hit);
}

int agentc_tool_engine_grep_hit_null(const char *line, size_t n, AgcGrepHit *hit) {
    if (line == NULL || n == 0 || hit == NULL) return 0;
    agentc_memset(hit, 0, sizeof *hit);
    size_t z = 0;
    while (z < n && line[z] != '\0') z++;
    if (z >= n) return 0;   /* no NUL: not a -Z record (e.g. the `--` separator) */
    size_t d = z + 1, d0 = d;
    while (d < n && line[d] >= '0' && line[d] <= '9') d++;
    if (d == d0) return 0;  /* no numeric boundary after the path */
    if (d >= n) return 0;
    if (line[d] != ':' && line[d] != '-') return 0;
    return grep_hit_tail(line, n, z, d0, d, hit);
}

void agentc_tool_engine_emit_hit(AgcBuf *out, const AgcGrepHit *hit) {
    if (out == NULL || hit == NULL) return;
    agentc_buf_push(out, hit->path, hit->path_len);
    agentc_buf_byte(out, ':');
    agentc_buf_u64(out, hit->line);
    agentc_buf_byte(out, ':');
    size_t take = hit->text_len < AGENTC_LIMIT_GREP_LINE ? hit->text_len : AGENTC_LIMIT_GREP_LINE;
    agentc_buf_push(out, hit->text, take);
    if (take < hit->text_len || hit->text_cut) agentc_buf_cstr(out, "...");
    agentc_buf_byte(out, '\n');
}

/* --------------------------------------------------------- external reader */
static void append_stderr(AgcBuf *b, const u8 *p, size_t n) {
    if (b->len >= ENGINE_MAX_STDERR) return;
    size_t room = ENGINE_MAX_STDERR - b->len;
    if (n > room) n = room;
    agentc_buf_push(b, p, n);
}

static void trim_stderr(AgcBuf *b) {
    size_t s = 0, e = b->len;
    while (s < e && (b->p[s] == ' ' || b->p[s] == '\t' || b->p[s] == '\n' || b->p[s] == '\r'))
        s++;
    while (e > s && (b->p[e - 1] == ' ' || b->p[e - 1] == '\t' || b->p[e - 1] == '\n' ||
                     b->p[e - 1] == '\r'))
        e--;
    if (s > 0 && e > s) agentc_memmove(b->p, b->p + s, e - s);
    b->len = e - s;
    agentc_buf_byte(b, 0);   /* NUL-terminate without counting it */
    b->len--;
}

/* SIGTERM the group, a bounded ~100 ms grace, then SIGKILL; always reap.
 * os_wait returns a raw status (>= 0), -1 for "no child ready yet", or -2 for
 * an unknown/already-reaped pid; -2 must never be mistaken for a status (it
 * would decode as exit 254). */
static int child_reap(int pid, bool force) {
    if (pid <= 0) return -1;
    if (force) {
        os_kill(-pid, 15);
        for (int i = 0; i < 20; i++) {
            int w = os_wait(pid, true);
            if (w == -2) return -1;
            if (w >= 0) return w;
            os_poll(NULL, 0, 5);
        }
        os_kill(-pid, 9);
    }
    for (int i = 0; i < 200; i++) {
        int w = os_wait(pid, true);
        if (w == -2) return -1;
        if (w >= 0) return w;
        os_poll(NULL, 0, 5);
    }
    os_kill(-pid, 9);
    for (int i = 0; i < 20; i++) {
        int w = os_wait(pid, true);
        if (w == -2) return -1;
        if (w >= 0) return w;
        os_poll(NULL, 0, 5);
    }
    return -1;
}

typedef struct {
    int pid;          /* > 0 child; negative errno once spawn was attempted */
    int out_fd;       /* stdout read end, -1 when closed */
    int err_fd;       /* stderr read end, -1 when closed */
    int status;       /* raw os_wait status, -1 when never reaped */
    bool read_err;    /* a read/poll failed */
    bool sink_stop;   /* the sink asked to stop (limit/byte cap) */
    bool spawn_failed;
    int stop;         /* checkpoint result while pumping */
    AgcBuf err;       /* capped stderr, owned */
} AgcProc;

/* 0 on success, -1 before/at spawn failure. `p->spawn_failed` distinguishes a
 * failed os_spawn_group from a failed pipe. */
static int proc_spawn(AgcProc *p, AgcVec *argv) {
    agentc_memset(p, 0, sizeof *p);
    p->pid = -1;
    p->out_fd = -1;
    p->err_fd = -1;
    p->status = -1;

    int op[2] = { -1, -1 };
    int ep[2] = { -1, -1 };
    if (os_pipe(op) < 0) return -1;
    if (os_pipe(ep) < 0) {
        os_close(op[0]);
        os_close(op[1]);
        return -1;
    }

    int devnull = os_open("/dev/null", OS_O_RDONLY | OS_O_CLOEXEC, 0);
    /* An explicit empty environment, not NULL: execve with a NULL envp is not
     * portable (and a config file such as RIPGREP_CONFIG_PATH must not change
     * what the model sees). argv[0] is absolute, so no PATH lookup is needed. */
    char *const empty_envp[] = { NULL };
    int pid = os_spawn_group((char *const *)argv->p, empty_envp, NULL, devnull, op[1], ep[1], 0);
    if (devnull >= 0) os_close(devnull);
    os_close(op[1]);
    os_close(ep[1]);
    if (pid < 0) {
        os_close(op[0]);
        os_close(ep[0]);
        p->pid = pid;
        p->spawn_failed = true;
        return -1;
    }
    p->pid = pid;
    p->out_fd = op[0];
    p->err_fd = ep[0];
    return 0;
}

static void proc_free(AgcProc *p) {
    if (p == NULL) return;
    if (p->out_fd >= 0) os_close(p->out_fd);
    if (p->err_fd >= 0) os_close(p->err_fd);
    agentc_buf_free(&p->err);
}

/* Replace argv[0] with the pinned absolute binary, spawn the group and, on
 * failure, write the standard error result. Returns 0 on success; on failure
 * the caller frees its args and returns 0. */
static int external_spawn(const AgcEngineEntry *e, const char *tool, const char *backend,
                          AgcVec *argv, AgcBuf *out, bool *is_error, AgcProc *p) {
    const char *prog = pin_bin(tool);
    if (prog == NULL || prog[0] == 0) prog = e != NULL ? e->binary : tool;
    agentc_free(((char **)argv->p)[0]);
    ((char **)argv->p)[0] = agentc_strdup(prog != NULL ? prog : tool);

    if (proc_spawn(p, argv) != 0) {
        if (p->spawn_failed)
            agentc_buf_printf(
                out,
                "error: %s: failed to start %s (errno %d); use --tools-engine internal\n",
                tool, backend, p->pid);
        else
            agentc_buf_printf(
                out, "error: %s: %s: pipe failed; use --tools-engine internal\n", tool,
                backend);
        if (is_error) *is_error = true;
        proc_free(p);
        return -1;
    }
    return 0;
}

static void proc_pump(AgcProc *p, bool (*sink)(void *, const u8 *, size_t), void *ud) {
    for (;;) {
        agentc_pump(20);
        p->stop = agentc_tool_checkpoint();
        if (p->stop != 0) break;

        struct os_pollfd fds[2];
        int nf = 0;
        if (p->out_fd >= 0) {
            fds[nf].fd = p->out_fd;
            fds[nf].events = OS_POLLIN;
            fds[nf].revents = 0;
            nf++;
        }
        if (p->err_fd >= 0) {
            fds[nf].fd = p->err_fd;
            fds[nf].events = OS_POLLIN;
            fds[nf].revents = 0;
            nf++;
        }
        if (nf == 0) break;

        int pr = os_poll(fds, nf, 20);
        if (pr < 0) {
            if (pr != -4) { p->read_err = true; break; }   /* EINTR retried */
            continue;
        }
        if (pr == 0) continue;

        for (int k = 0; k < nf && !p->read_err && !p->sink_stop; k++) {
            int fd = fds[k].fd;
            if (!(fds[k].revents & (OS_POLLIN | OS_POLLHUP | OS_POLLERR))) continue;
            u8 tmp[8192];
            for (;;) {
                int nr = os_read(fd, tmp, sizeof tmp);
                if (nr > 0) {
                    if (fd == p->err_fd) {
                        append_stderr(&p->err, tmp, (size_t)nr);
                    } else if (sink != NULL && sink(ud, tmp, (size_t)nr)) {
                        p->sink_stop = true;
                        break;
                    }
                } else if (nr == 0) {
                    if (fd == p->err_fd) { os_close(p->err_fd); p->err_fd = -1; }
                    else { os_close(p->out_fd); p->out_fd = -1; }
                    break;
                } else if (nr == -4) {
                    continue;   /* EINTR: retry the interrupted read */
                } else if (nr == -11) {
                    break;   /* EAGAIN: no more buffered bytes */
                } else {
                    p->read_err = true;
                    break;
                }
            }
        }
        if (p->sink_stop || p->read_err) break;
    }

    p->status = child_reap(p->pid, p->stop != 0 || p->sink_stop || p->read_err);
    if (p->out_fd >= 0) { os_close(p->out_fd); p->out_fd = -1; }
    if (p->err_fd >= 0) { os_close(p->err_fd); p->err_fd = -1; }
}

static int proc_exit_code(const AgcProc *p) {
    if (p->status == -1) return -1;
    if ((p->status & 0x7f) == 0) return (p->status >> 8) & 0xff;
    return 128 + (p->status & 0x7f);
}

/* True when the completed backend is a tool error. `early` marks a cap-driven
 * stop: it suppresses only OUR kill (a signal-derived status); a normal exit
 * `>= 2` (with stderr) is still a real error even when the cap also fired.
 * Exit 0 and exit 1 on an exit1_nomatch rung are success; any other status
 * prefers the trimmed stderr text, falling back to the raw status. */
static bool proc_error(const AgcEngineEntry *e, AgcProc *p, const char *tool,
                       const char *backend, bool early, AgcBuf *out) {
    if (p->read_err || p->status == -1) {
        agentc_buf_printf(out, "error: %s: %s failed to run; use --tools-engine internal\n", tool,
                          backend);
        return true;
    }
    bool signaled = (p->status & 0x7f) != 0;
    if (early && signaled) return false;
    int code = proc_exit_code(p);
    if (code == 0) return false;
    if (code == 1 && e != NULL && e->exit1_nomatch) return false;
    /* The proc owns the stderr buffer; trimming it in place is safe. */
    AgcBuf *err = &p->err;
    trim_stderr(err);
    if (err->len > 0)
        agentc_buf_printf(out, "error: %s: %s: %s\n", tool, backend, (const char *)err->p);
    else
        agentc_buf_printf(out, "error: %s: %s exited with status %d; use --tools-engine internal\n",
                          tool, backend, code);
    return true;
}

/* Append `marker` so it is reliably visible through the job driver. If the
 * collected output plus the marker exceeds AGENTC_LIMIT_TOOL_BYTES, drop whole
 * lines off the end until it fits, then append. The tool thus always composes
 * its own final text (out->len <= cap) and the driver's finalize adds nothing,
 * so exactly one marker is visible. */
static void finish_output(AgcBuf *out, const char *marker) {
    size_t mlen = marker != NULL ? agentc_strlen(marker) : 0;
    while (out->len > 0 && out->len + mlen > (size_t)AGENTC_LIMIT_TOOL_BYTES) {
        /* Drop only the last line: if the buffer ends in '\n' the final complete
         * line starts after the preceding '\n'; an unterminated tail is dropped
         * whole. Earlier complete lines stay. */
        size_t end = out->len;
        if (out->p[end - 1] == '\n') end--;
        size_t start = end;
        while (start > 0 && out->p[start - 1] != '\n') start--;
        out->len = start;
    }
    if (mlen > 0) agentc_buf_push(out, marker, mlen);
}

/* ---------------------------------------------------------- grep collector */
typedef struct {
    char *path;
    size_t path_len;
    u64 line;
    char *text;
    size_t text_len;
    size_t group;   /* records of one file share a group id */
    bool is_match;
    bool text_cut;  /* only a prefix of the line text was stored */
    bool accepted;
} GrepRecord;

typedef struct {
    AgcBuf line;           /* pending bytes of the current stdout line */
    AgcVec recs;           /* GrepRecord, owned */
    AgcJsonArena *scratch; /* reused across rg lines */
    char *prev_path;       /* last record's path, for group ids */
    size_t prev_path_len;
    size_t nmatches;       /* matches seen so far */
    size_t bytes;          /* path+text bytes stored */
    size_t group;
    i64 limit;
    bool json;
    bool null_sep;
    bool limit_hit;
    bool bytes_hit;
} GrepReader;

/* Copy one hit into `r`; returns true when the byte budget is exceeded. The
 * prospective size is tested before anything is copied, so a single huge record
 * is never materialised; the pending-line cap (AGENTC_LIMIT_GREP_PENDING) is
 * what bounds one stdout line before it reaches here. */
static bool grep_record_store(GrepReader *r, const AgcGrepHit *hit) {
    /* Store only the prefix the formatter can ever print (the 500-char cap), so
     * a very long source line costs a bounded amount of memory and still yields
     * its prefix like the built-in (the pending-line cap bounds one raw line).
     * Charge the *emitted* size (prefix + per-record formatting): that makes the
     * budget an upper bound on the final text, so a no-marker result can never
     * grow past the cap and be trimmed silently by finish_output. */
    const size_t overhead = 32;   /* two ':' + 20 digits + "..." + '\n' */
    size_t tlen = hit->text_len;
    bool cut = false;
    if (tlen > AGENTC_LIMIT_GREP_LINE) {
        tlen = AGENTC_LIMIT_GREP_LINE;
        cut = true;
    }
    if (hit->path_len + tlen + overhead > (size_t)AGENTC_LIMIT_TOOL_BYTES - r->bytes) {
        r->bytes_hit = true;
        return true;
    }
    GrepRecord *rec = agentc_vec_push(&r->recs, sizeof *rec);
    bool same = r->prev_path != NULL && r->prev_path_len == hit->path_len &&
                agentc_memeq(r->prev_path, hit->path, hit->path_len);
    if (!same) {
        r->group++;
        agentc_free(r->prev_path);
        r->prev_path = agentc_strdup_len(hit->path, hit->path_len);
        r->prev_path_len = hit->path_len;
    }
    rec->group = r->group;
    rec->path = agentc_strdup_len(hit->path, hit->path_len);
    rec->path_len = hit->path_len;
    rec->line = hit->line;
    rec->text = agentc_strdup_len(hit->text, tlen);
    rec->text_len = tlen;
    rec->text_cut = cut;
    rec->is_match = hit->is_match;
    rec->accepted = false;
    r->bytes += hit->path_len + tlen + overhead;
    return false;
}

/* One complete stdout line (without its '\n'): normalize it and store the
 * record, accepting only the first `limit` matches. Shared by the streaming
 * sink and the post-pump flush. Returns true when the reader must stop. */
static bool grep_line(GrepReader *r, const char *line, size_t len) {
    AgcGrepHit hit;
    int got = r->json ? agentc_tool_engine_rg_hit(r->scratch, line, len, &hit)
                      : (r->null_sep ? agentc_tool_engine_grep_hit_null(line, len, &hit)
                                     : agentc_tool_engine_grep_hit(line, len, &hit));
    if (!got) return false;
    if (hit.is_match) {
        /* The (limit+1)-th match stops the reader and is never stored. Its
         * context, if already collected, is dropped by the window filter. */
        if ((i64)r->nmatches >= r->limit) {
            r->limit_hit = true;
            return true;
        }
        r->nmatches++;
    }
    return grep_record_store(r, &hit);
}

static bool grep_sink(void *ud, const u8 *p, size_t n) {
    GrepReader *r = ud;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != '\n') {
            if (r->line.len >= AGENTC_LIMIT_GREP_PENDING) {
                r->bytes_hit = true;
                return true;
            }
            agentc_buf_byte(&r->line, p[i]);
            continue;
        }
        if (r->line.len > 0 && grep_line(r, (const char *)r->line.p, r->line.len)) return true;
        agentc_buf_clear(&r->line);
    }
    return false;
}

/* Emit accepted matches and only context within ±ctx lines of an accepted match
 * in the same file group. Group-scoped: the accepted matches of one group are a
 * contiguous stretch of `acc`, so per group we scan only that stretch and stop
 * as soon as the match is too far after the context line. Window arithmetic is
 * done as an absolute difference, never `a + ctx`, so a hostile (max-u64) line
 * number cannot wrap. Returns the number of records emitted. */
static size_t grep_emit(AgcBuf *out, const GrepReader *r, i64 ctx) {
    size_t n = r->recs.len;
    if (n == 0) return 0;
    GrepRecord *rec = (GrepRecord *)r->recs.p;
    size_t nacc = 0, ord = 0, maxgroup = 0;
    for (size_t i = 0; i < n; i++) {
        rec[i].accepted = rec[i].is_match && (i64)ord < r->limit;
        if (rec[i].is_match) ord++;
        if (rec[i].accepted) nacc++;
        if (rec[i].group > maxgroup) maxgroup = rec[i].group;
    }
    if (nacc == 0) return 0;
    size_t *acc = agentc_alloc(nacc * sizeof(size_t));
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (rec[i].accepted) acc[k++] = i;

    size_t *glo = agentc_alloc((maxgroup + 2) * sizeof(size_t));
    size_t *ghi = agentc_alloc((maxgroup + 2) * sizeof(size_t));
    for (size_t g = 0; g <= maxgroup + 1; g++) glo[g] = ghi[g] = 0;
    for (size_t j = 0; j < nacc; j++) {
        size_t g = rec[acc[j]].group;
        if (ghi[g] == 0) glo[g] = j;
        ghi[g] = j + 1;
    }

    size_t emitted = 0;
    for (size_t i = 0; i < n; i++) {
        if (!rec[i].is_match) {
            bool near = false;
            for (size_t j = glo[rec[i].group]; j < ghi[rec[i].group]; j++) {
                u64 ml = rec[acc[j]].line, cl = rec[i].line;
                if (ml > cl) {
                    if (ml - cl > (u64)ctx) break;   /* later matches are farther */
                } else if (cl - ml > (u64)ctx) {
                    continue;
                }
                near = true;
                break;
            }
            if (!near) continue;
        } else if (!rec[i].accepted) {
            continue;
        }
        AgcGrepHit h;
        h.path = rec[i].path;
        h.path_len = rec[i].path_len;
        h.line = rec[i].line;
        h.text = rec[i].text;
        h.text_len = rec[i].text_len;
        h.is_match = rec[i].is_match;
        h.text_cut = rec[i].text_cut;
        agentc_tool_engine_emit_hit(out, &h);
        emitted++;
    }
    agentc_free(glo);
    agentc_free(ghi);
    agentc_free(acc);
    return emitted;
}

static void grep_reader_free(GrepReader *r) {
    GrepRecord *rec = (GrepRecord *)r->recs.p;
    for (size_t i = 0; i < r->recs.len; i++) {
        agentc_free(rec[i].path);
        agentc_free(rec[i].text);
    }
    agentc_vec_free(&r->recs);
    agentc_buf_free(&r->line);
    agentc_json_arena_free(r->scratch);
    agentc_free(r->prev_path);
    r->scratch = NULL;
    r->prev_path = NULL;
}

static int grep_external_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                             bool *is_error, const char *backend, bool json) {
    (void)self;
    if (is_error) *is_error = false;
    const AgcEngineEntry *e = entry_lookup("grep", backend);

    AgcGrepArgs ga;
    if (agentc_tool_grep_args(call, &ga, out, is_error) != 0) {
        agentc_tool_grep_args_free(&ga);
        return 0;
    }
    /* The -Z delimiter is only meaningful for the POSIX grep rung and only when
     * its probe succeeded; rg (JSON) and the built-in are unaffected. */
    ga.null_sep = g_grep_null && agentc_streq(backend, AGENTC_ENGINE_GREP);

    AgcVec argv = { 0 };
    if (agentc_tool_engine_build_argv("grep", backend, &ga, &argv) != 0) {
        argv_free(&argv);
        agentc_tool_grep_args_free(&ga);
        agentc_buf_printf(
            out, "error: grep: %s: cannot build command; use --tools-engine internal\n", backend);
        if (is_error) *is_error = true;
        return 0;
    }
    i64 limit = clamp_limit(ga.limit);
    i64 ctx = clamp_context(ga.context);
    bool null_sep = ga.null_sep;
    agentc_tool_grep_args_free(&ga);   /* argv owns its own copies now */

    GrepReader r;
    agentc_memset(&r, 0, sizeof r);
    r.json = json;
    r.null_sep = null_sep;
    r.limit = limit;
    if (json) {
        /* Allocate the rg scratch arena before spawning: a NULL scratch would
         * make rg_hit reject every line and silently report "no matches". */
        r.scratch = agentc_json_arena_new(AGENTC_LIMIT_TOOL_ARENA);
        if (r.scratch == NULL) {
            argv_free(&argv);
            agentc_buf_cstr(out, "error: grep: out of memory");
            if (is_error) *is_error = true;
            return 0;
        }
    }

    AgcProc p;
    if (external_spawn(e, "grep", backend, &argv, out, is_error, &p) != 0) {
        argv_free(&argv);
        grep_reader_free(&r);
        return 0;
    }
    argv_free(&argv);

    proc_pump(&p, grep_sink, &r);

    /* A backend may end its last line without a newline: flush it through the
     * same per-line logic when the run was not cut short. */
    if (p.stop == 0 && !p.sink_stop && !p.read_err && r.line.len > 0)
        (void)grep_line(&r, (const char *)r.line.p, r.line.len);

    if (p.stop != 0) {
        /* Cancelled or timed out: hand the partial output to the driver. */
        grep_emit(out, &r, ctx);
        int stop = p.stop;
        grep_reader_free(&r);
        proc_free(&p);
        return stop;
    }

    bool early = r.limit_hit || r.bytes_hit;
    bool err = proc_error(e, &p, "grep", backend, early, out);
    if (!err) {
        size_t emitted = grep_emit(out, &r, ctx);
        char mbuf[64];
        const char *marker = "";
        if (r.limit_hit) {
            agentc_snprintf(mbuf, sizeof mbuf, "[grep truncated at %lld matches]\n",
                            (long long)limit);
            marker = mbuf;
        } else if (r.bytes_hit) {
            marker = "[grep output truncated]\n";
        } else if (emitted == 0) {
            marker = "no matches\n";
        }
        finish_output(out, marker);
    }

    if (is_error) *is_error = err;
    grep_reader_free(&r);
    proc_free(&p);
    return 0;
}

static int grep_rg_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                       bool *is_error) {
    return grep_external_run(self, call, out, is_error, AGENTC_ENGINE_RG, true);
}

static int grep_posix_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                          bool *is_error) {
    return grep_external_run(self, call, out, is_error, AGENTC_ENGINE_GREP, false);
}

/* ---------------------------------------------------------- find collector */
typedef struct {
    AgcBuf line;    /* pending bytes of the current stdout line */
    AgcVec paths;   /* owned char *, at most `limit` of them */
    size_t bytes;   /* path bytes stored (+1 each) */
    size_t limit;
    bool limit_hit;
    bool bytes_hit;
    char *root;     /* owned; normalized POSIX-find operand, NULL when unused */
    size_t root_len;
    bool drop_root; /* POSIX find prints the search root; fd/built-in do not */
    bool win;       /* '\\' is a directory separator (Windows fd only) */
} FindReader;

/* Length of `p[0..len)` with trailing directory separators stripped, never
 * collapsing a filesystem root: an all-separator path (`/`, `//`) or a Windows
 * drive root (`X:\`) is returned unchanged. One rule for the line strip and the
 * search-root normalization so they can never disagree. */
static size_t path_strip_trailing_sep(const char *p, size_t len, bool win) {
    if (len <= 1 || !(p[len - 1] == '/' || (win && p[len - 1] == '\\'))) return len;
    bool all_sep = true;
    for (size_t k = 0; k < len; k++)
        if (p[k] != '/' && !(win && p[k] == '\\')) { all_sep = false; break; }
    while (!all_sep && len > 1 &&
           (p[len - 1] == '/' || (win && p[len - 1] == '\\')) &&
           !(win && len == 3 && p[1] == ':'))
        len--;
    return len;
}

/* One complete stdout line (without its '\n'): CR is trimmed, trailing
 * directory separators are stripped and a directory search root is dropped
 * before the emptiness test so a "\r"-only line never becomes an empty path.
 * The prospective size is tested before the path is copied, so one huge path is
 * never materialised. Shared by the streaming sink and the post-pump flush.
 * Returns true when the sink must stop. */
static bool find_line(FindReader *r, const char *line, size_t len) {
    while (len > 0 && line[len - 1] == '\r') len--;
    /* G1: fd marks directories with a trailing separator and the built-in does
     * not; strip it so every rung emits the same path (see the helper). */
    len = path_strip_trailing_sep(line, len, r->win);
    /* G1: POSIX find echoes a directory search root as its first line; drop that
     * exact line (not stored, not counted). A file operand is kept. */
    if (r->drop_root && len == r->root_len && agentc_memeq(line, r->root, len))
        return false;
    if (len == 0) return false;
    if (r->paths.len >= r->limit) {
        r->limit_hit = true;
        return true;
    }
    if (len + 1 > (size_t)AGENTC_LIMIT_TOOL_BYTES - r->bytes) {
        r->bytes_hit = true;
        return true;
    }
    *(char **)agentc_vec_push(&r->paths, sizeof(char *)) = agentc_strdup_len(line, len);
    r->bytes += len + 1;
    return false;
}

/* Split one stdout chunk into paths. The pending line is bounded by
 * AGENTC_LIMIT_FIND_PATH; a path that would exceed it cannot be represented, so
 * the run stops and is reported as truncated (never stored truncated, never
 * dropped silently). `bytes_hit` covers both "too much output" (find_line's
 * AGENTC_LIMIT_TOOL_BYTES budget) and "one path longer than
 * AGENTC_LIMIT_FIND_PATH". When the (limit+1)-th path arrives it is dropped and
 * limit_hit is set; the caller stops and kills the child. Returns true when the
 * sink wants to stop. */
static bool find_sink(void *ud, const u8 *p, size_t n) {
    FindReader *r = ud;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != '\n') {
            if (r->line.len >= AGENTC_LIMIT_FIND_PATH) {
                r->bytes_hit = true;
                return true;
            }
            agentc_buf_byte(&r->line, p[i]);
            continue;
        }
        if (r->line.len > 0 && find_line(r, (const char *)r->line.p, r->line.len))
            return true;
        agentc_buf_clear(&r->line);
    }
    return false;
}

static int path_cmp(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] != 0 && a[i] == b[i]) i++;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}

/* Stable iterative (bottom-up) merge sort. agentc_alloc aborts on OOM. */
static void sort_paths(char **a, size_t n) {
    if (n < 2) return;
    char **tmp = agentc_alloc(n * sizeof(char *));
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t i = 0; i < n; i += 2 * width) {
            size_t mid = (i + width < n) ? i + width : n;
            size_t end = (i + 2 * width < n) ? i + 2 * width : n;
            size_t l = i, rr = mid, k = i;
            while (l < mid && rr < end)
                tmp[k++] = (path_cmp(a[l], a[rr]) <= 0) ? a[l++] : a[rr++];
            while (l < mid) tmp[k++] = a[l++];
            while (rr < end) tmp[k++] = a[rr++];
        }
        for (size_t i = 0; i < n; i++) a[i] = tmp[i];
    }
    agentc_free(tmp);
}

static void free_paths(AgcVec *paths) {
    for (size_t i = 0; i < paths->len; i++) agentc_free(((char **)paths->p)[i]);
    agentc_vec_free(paths);
}

/* Release everything a FindReader owns. Used on every exit of
 * find_external_run, including the parse/spawn failure paths. */
static void find_reader_free(FindReader *r) {
    if (r == NULL) return;
    free_paths(&r->paths);
    agentc_buf_free(&r->line);
    agentc_free(r->root);
    r->root = NULL;
}

/* Sort and emit the collected paths, one per line. The cancel path calls this
 * directly so a cancelled run carries no marker and no "no matches" fallback. */
static void find_emit_paths(AgcBuf *out, FindReader *r) {
    char **paths = (char **)r->paths.p;
    size_t np = r->paths.len;
    sort_paths(paths, np);
    for (size_t i = 0; i < np; i++) {
        agentc_buf_cstr(out, paths[i]);
        agentc_buf_byte(out, '\n');
    }
}

/* Sort, emit, then the count-cap, byte-cap or no-match marker. */
static void find_emit(AgcBuf *out, FindReader *r, i64 limit) {
    find_emit_paths(out, r);
    char mbuf[64];
    const char *marker = "";
    if (r->limit_hit) {
        agentc_snprintf(mbuf, sizeof mbuf, "[find truncated at %lld paths]\n",
                        (long long)limit);
        marker = mbuf;
    } else if (r->bytes_hit) {
        marker = "[find output truncated]\n";
    } else if (r->paths.len == 0) {
        marker = "no matches\n";
    }
    finish_output(out, marker);
}

static int find_external_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                             bool *is_error, const char *backend) {
    (void)self;
    if (is_error) *is_error = false;
    const AgcEngineEntry *e = entry_lookup("find", backend);

    FindReader r;
    agentc_memset(&r, 0, sizeof r);
    r.win = agentc_streq(os_platform(), "windows");

    AgcFindArgs fa;
    if (agentc_tool_find_args(call, &fa, out, is_error) != 0) {
        agentc_tool_find_args_free(&fa);
        find_reader_free(&r);
        return 0;
    }

    AgcVec argv = { 0 };
    if (agentc_tool_engine_build_argv("find", backend, &fa, &argv) != 0) {
        argv_free(&argv);
        agentc_tool_find_args_free(&fa);
        agentc_buf_printf(
            out, "error: find: %s: cannot build command; use --tools-engine internal\n", backend);
        if (is_error) *is_error = true;
        find_reader_free(&r);
        return 0;
    }
    i64 limit = clamp_find_limit(fa.limit);
    agentc_tool_find_args_free(&fa);   /* argv owns its own copies now */

    /* G1: POSIX find echoes the search operand as its first output line; fd and
     * the built-in never do. When the operand resolves to a directory, remember
     * it (via path_strip_trailing_sep) so find_line can drop exactly that
     * line. A file operand is left in place (find prints it and it is wanted). */
    if (agentc_streq(backend, AGENTC_ENGINE_FIND)) {
        /* find -H <operand> ...: the operand is the first argument that is not
         * an option (it can never start with '-' -- a leading '-' is anchored
         * to './'), so this stays correct if another leading option is added. */
        char **a = (char **)argv.p;
        const char *op = NULL;
        for (size_t i = 1; a != NULL && a[i] != NULL; i++) {
            if (a[i][0] != '-') { op = a[i]; break; }
        }
        if (op != NULL) {
            struct os_stat st;
            if (os_stat(op, &st) == 0 && (st.st_mode & 0170000u) == 0040000u) {
                size_t rl = path_strip_trailing_sep(op, agentc_strlen(op), false);
                r.root = agentc_strdup_len(op, rl);
                r.root_len = rl;
                r.drop_root = true;
            }
        }
    }
    r.limit = (size_t)limit;

    AgcProc p;
    if (external_spawn(e, "find", backend, &argv, out, is_error, &p) != 0) {
        argv_free(&argv);
        find_reader_free(&r);
        return 0;
    }
    argv_free(&argv);

    proc_pump(&p, find_sink, &r);

    /* A backend may end its last path without a newline: flush it through the
     * same per-line logic when the run was not cut short. */
    if (p.stop == 0 && !p.sink_stop && !p.read_err && r.line.len > 0)
        (void)find_line(&r, (const char *)r.line.p, r.line.len);

    if (p.stop != 0) {
        find_emit_paths(out, &r);   /* no marker / no-match fallback on cancel */
        int stop = p.stop;
        find_reader_free(&r);
        proc_free(&p);
        return stop;
    }

    bool early = r.limit_hit || r.bytes_hit;
    bool err = proc_error(e, &p, "find", backend, early, out);
    if (!err) find_emit(out, &r, limit);

    if (is_error) *is_error = err;
    find_reader_free(&r);
    proc_free(&p);
    return 0;
}

static int find_fd_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                       bool *is_error) {
    return find_external_run(self, call, out, is_error, AGENTC_ENGINE_FD);
}

static int find_posix_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                          bool *is_error) {
    return find_external_run(self, call, out, is_error, AGENTC_ENGINE_FIND);
}
