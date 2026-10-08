/* session.c — append-only JSONL session storage for --continue/--resume.
 *
 * One file per session:
 *   $XDG_DATA_HOME/agentc/sessions/--<sanitized-cwd>--/<timestamp>_<8hex>.jsonl
 * The --session-dir flag, config session_dir and AGENTC_SESSION_DIR name the
 * directory that holds the files directly (no cwd subdirectory is added).
 *
 * Line 1 is the session header, every following line one entry. Message lines
 * use the frozen shape documented in include/session.h; tool-call arguments are
 * emitted as a JSON string holding the raw args text. Files written with the
 * raw-object form do not replay: their arguments load as "{}". A
 * compaction line resets the replay to its summary.
 */
#include "agentc.h"
#include "plat.h"
#include "session.h"

typedef int (*AgcDirFn)(void *ud, const char *name, const char *full, bool is_dir);

extern const char *agentc_env_get(const char *name);
extern char *agentc_read_file_owned(const char *path, size_t *len);
extern int agentc_mkdir_parents(const char *path);
extern bool agentc_path_join(char *out, size_t cap, const char *dir, const char *name);
extern const char *agentc_data_home(char *buf, size_t cap);
extern int agentc_dir_scan(const char *dir, AgcDirFn cb, void *ud);
extern void agentc_sort_strings(char **a, size_t n, bool descending);

/* internal helpers from messages.c (not part of the frozen header) */
AgcBlock *agentc_msg_block_new(AgcMsg *m, int type);
void agentc_msg_block_append(AgcBlock *b, const char *p, size_t n);
void agentc_msg_add_think(AgcMsg *m);

#define PATH_MAX_ 4096

/* Session files are ASCII-only JSONL: the loader accepts the \\uXXXX
 * escapes transparently and the bytes survive any locale. */
static void jsonw_out(AgcJsonW *w, AgcBuf *b) {
    agentc_jsonw_init(w, b);
    agentc_jsonw_set_ascii(w, true);
}

struct AgcSession {
    char *path;      /* owned; NULL in memory_only mode */
    char *id;        /* owned 8-hex id */
    char *cwd;       /* owned */
    int fd;
    bool memory_only;
    bool need_newline;   /* the file ends mid-line (crash-truncated): repair before appending */
    i64 append_off;      /* bytes of the file after the last complete line */
    int append_err;      /* first append error; sticky, no bytes are written after it */
    bool degraded;       /* an append failed: the session is read-only from now on */
    bool test_fail_next; /* internal test hook: fail the next session_write with -EIO */
};

/* Internal test hook (declared ad hoc by tests/session_test.c): force the next
 * torn-tail truncate at open to report failure. A process-global because the
 * truncate runs before agentc_session_open returns a handle to flag. */
static bool g_test_fail_truncate;

/* ------------------------------------------------------------- utilities */

static int write_all(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n) {
        int w = os_write(fd, q, n);
        if (w < 0) {
            if (w == -4 /* EINTR */) continue;
            return w;
        }
        if (w == 0) return -5;
        q += w;
        n -= (size_t)w;
    }
    return 0;
}

/* The one append choke point: every record goes through here. It owns the
 * torn-tail repair, the running offset and the sticky first error. On a failed
 * or short write the file is rolled back to the last complete line with
 * os_ftruncate and the session stays degraded (read-only) for good: the fd is
 * O_APPEND, so without the rollback every later record would grow onto the
 * partial line. */
static int session_write(AgcSession *s, const void *p, size_t n) {
    if (!s || s->memory_only || s->fd < 0) return 0;
    if (s->append_err != 0) return s->append_err;

    int r = 0;
    if (s->test_fail_next) {
        s->test_fail_next = false;
        r = -5; /* EIO, injected by the internal test hook */
    } else {
        if (s->need_newline) {
            r = write_all(s->fd, "\n", 1);
            if (r == 0) {
                s->need_newline = false;
                s->append_off += 1;
            }
        }
        if (r == 0 && n > 0) {
            r = write_all(s->fd, p, n);
            if (r == 0) s->append_off += (i64)n;
        }
    }
    if (r != 0) {
        s->degraded = true;
        s->append_err = r;
        agentc_logf(2, "session: append failed (%d); rolled back to %lld bytes; "
                       "session is read-only from now on",
                r, (long long)s->append_off);
        if (s->append_off > 0) (void)os_ftruncate(s->fd, s->append_off);
        return r;
    }
    return 0;
}

/* Internal test hook (declared ad hoc by tests/session_test.c): the next
 * session_write fails with -EIO before writing anything, so the rollback-to-
 * append_off path is observable. */
void agentc_session_test_fail_next_append(AgcSession *s) {
    if (s) s->test_fail_next = true;
}

void agentc_session_test_fail_next_truncate(bool fail) { g_test_fail_truncate = fail; }

static void gen_id(char out[9]) {
    static const char hex[] = "0123456789abcdef";
    u8 r[4];
    if (os_random(r, sizeof r) < 0) agentc_memset(r, 0, sizeof r);
    for (size_t i = 0; i < 4; i++) {
        out[2 * i] = hex[r[i] >> 4];
        out[2 * i + 1] = hex[r[i] & 0xF];
    }
    out[8] = 0;
}

static void sanitize_cwd(const char *cwd, char *out, size_t cap) {
    size_t o = 0;
    size_t i = 0;
    if (cwd && (cwd[0] == '/' || cwd[0] == '\\')) i = 1; /* absolute paths keep their leading slash */
    for (; cwd && cwd[i] && o + 1 < cap; i++) {
        char c = cwd[i];
        if (c == '/' || c == '\\' || c == ':') c = '-';
        out[o++] = c;
    }
    out[o] = 0;
}

static i64 now_ms(void) {
    i64 ns = os_now_ns(OS_CLOCK_REALTIME);
    return ns > 0 ? ns / 1000000 : 0;
}

static const char *session_env_dir(void) {
    const char *env = agentc_env_get("AGENTC_SESSION_DIR");
    return (env && env[0]) ? env : NULL;
}

/* Default per-cwd directory: <data home>/sessions/--<sanitized cwd>-- */
static bool default_dir_for_cwd(const char *cwd, char *out, size_t cap) {
    char base[PATH_MAX_];
    if (!agentc_data_home(base, sizeof base)) return false;
    char san[PATH_MAX_];
    sanitize_cwd(cwd, san, sizeof san);
    int n = agentc_snprintf(out, cap, "%s/sessions/--%s--", base, san);
    return n > 0 && (size_t)n < cap;
}

static const char *role_name(int role) {
    switch (role) {
    case AGENTC_ROLE_SYSTEM: return "system";
    case AGENTC_ROLE_USER: return "user";
    case AGENTC_ROLE_ASSISTANT: return "assistant";
    case AGENTC_ROLE_TOOL: return "tool";
    default: return "user";
    }
}

static int role_id(const char *name) {
    if (!name) return -1;
    if (agentc_streq(name, "system")) return AGENTC_ROLE_SYSTEM;
    if (agentc_streq(name, "user")) return AGENTC_ROLE_USER;
    if (agentc_streq(name, "assistant")) return AGENTC_ROLE_ASSISTANT;
    if (agentc_streq(name, "tool")) return AGENTC_ROLE_TOOL;
    return -1;
}

/* Strict version read: the field is written as a plain integer, so a number
 * that is not one (a float/exponent) is malformed. Refuse such a file (return
 * a value > 1) instead of defaulting to 1 and misreading a newer format. */
static i64 session_version(const AgcJson *root) {
    const AgcJson *v = agentc_json_get(root, "version");
    if (!v || agentc_json_type(v) != AGENTC_JSON_NUM) return 1;
    size_t n = 0;
    const char *p = agentc_json_num(v, &n);
    bool ok = false;
    i64 r = agentc_parse_i64(p ? p : "", n, &ok);
    return ok ? r : 2;
}

static int session_write_header(AgcSession *s) {
    if (!s || s->memory_only || s->fd < 0) return 0;
    AgcBuf b = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "session");
    agentc_jsonw_key(&w, "version");
    agentc_jsonw_u64(&w, 1);
    agentc_jsonw_key(&w, "id");
    agentc_jsonw_cstr(&w, s->id);
    agentc_jsonw_key(&w, "timestamp");
    agentc_jsonw_i64(&w, now_ms());
    agentc_jsonw_key(&w, "cwd");
    agentc_jsonw_cstr(&w, s->cwd ? s->cwd : "");
    agentc_jsonw_end(&w);
    agentc_buf_byte(&b, '\n');
    int rc = session_write(s, b.p, b.len);
    agentc_buf_free(&b);
    return rc;
}

static AgcSession *session_alloc(void) {
    AgcSession *s = agentc_alloc(sizeof *s);
    s->fd = -1;
    return s;
}

/* --------------------------------------------------------------- lifecycle */

AgcSession *agentc_session_new(const AgcSessionOptions *o) {
    AgcSession *s = session_alloc();
    bool memory = o && o->memory_only;
    s->memory_only = memory;

    char cwdbuf[PATH_MAX_];
    const char *cwd = (o && o->cwd && o->cwd[0]) ? o->cwd : NULL;
    if (!cwd) {
        if (os_getcwd(cwdbuf, sizeof cwdbuf) < 0) cwdbuf[0] = 0;
        cwd = cwdbuf;
    }
    s->cwd = agentc_strdup(cwd ? cwd : "");

    char id[9];
    if (o && o->id && o->id[0]) {
        agentc_snprintf(id, sizeof id, "%s", o->id);
        id[8] = 0;
    } else {
        gen_id(id);
    }
    s->id = agentc_strdup(id);

    if (memory) return s;

    char dir[PATH_MAX_];
    const char *d = (o && o->dir && o->dir[0]) ? o->dir : NULL;
    if (!d) d = session_env_dir();
    if (!d) {
        if (!default_dir_for_cwd(s->cwd, dir, sizeof dir)) {
            agentc_logf(2, "warning: cannot resolve session directory; session is memory-only");
            s->memory_only = true;
            return s;
        }
        d = dir;
    }

    size_t need = agentc_strlen(d) + 64;
    s->path = agentc_alloc(need);
    agentc_snprintf(s->path, need, "%s/%lld_%s.jsonl", d, (long long)now_ms(), s->id);

    (void)agentc_mkdir_parents(s->path);
    s->fd = os_open(s->path, OS_O_WRONLY | OS_O_CREAT | OS_O_APPEND, 0600);
    if (s->fd < 0) {
        agentc_logf(2, "warning: cannot create session %s (errno %d)", s->path, s->fd);
        agentc_free(s->path);
        s->path = NULL;
        s->memory_only = true;
        return s;
    }
    int rc = session_write_header(s);
    if (rc < 0) agentc_logf(2, "warning: cannot write session header (errno %d)", rc);
    return s;
}

AgcSession *agentc_session_open(const char *path) {
    if (!path || !path[0]) return NULL;
    AgcSession *s = session_alloc();
    s->path = agentc_strdup(path);

    /* The fd is opened before the file is read so a missing header can be
     * appended through the same choke point; O_APPEND keeps every write at the
     * end even after a ftruncate rollback. */
    (void)agentc_mkdir_parents(path);
    s->fd = os_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_APPEND, 0600);
    if (s->fd < 0) {
        agentc_session_close(s);
        return NULL;
    }

    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    bool have_header = false;
    if (text && len > 0) {
        size_t le = 0;
        while (le < len && text[le] != '\n') le++;
        AgcJsonArena *ja = agentc_json_arena_new(0);
        AgcJson *root = agentc_json_parse_in(ja, text, le);
        if (agentc_json_is(agentc_json_get(root, "type"), "session")) {
            i64 ver = session_version(root);
            if (ver > 1) {
                /* A file from a newer agentc may not mean what we think it
                 * means: refuse instead of guessing (C.2.5). */
                agentc_logf(2, "session: version %lld is newer than 1; refusing",
                        (long long)ver);
                agentc_json_arena_free(ja);
                agentc_free(text);
                agentc_session_close(s);
                return NULL;
            }
            const char *id = agentc_json_get_str(root, "id");
            const char *cwd = agentc_json_get_str(root, "cwd");
            if (id && id[0]) s->id = agentc_strdup(id);
            if (cwd) s->cwd = agentc_strdup(cwd);
            have_header = true;
        }
        agentc_json_arena_free(ja);
    }

    if (text && len > 0) {
        /* append_off is the offset after the last complete line. A tail
         * without its newline is a torn write (crash, ENOSPC): truncate it
         * before appending or the next record would grow onto the partial one
         * (C.2.5). If the truncate fails, need_newline makes session_write
         * repair the line break before the next record instead. */
        size_t off = len;
        while (off > 0 && text[off - 1] != '\n') off--;
        s->append_off = (i64)off;
        if (off < len) {
            bool trunc_ok;
            if (g_test_fail_truncate) {
                g_test_fail_truncate = false;
                trunc_ok = false;
            } else {
                trunc_ok = os_ftruncate(s->fd, (i64)off) == 0;
            }
            if (trunc_ok) {
                s->need_newline = false;
            } else {
                /* The torn tail survives: the repair write appends after the
                 * real physical end, so record that end. Otherwise a later
                 * rollback would truncate into the torn line. */
                s->need_newline = true;
                s->append_off = (i64)len;
            }
        }
    }
    agentc_free(text);

    if (!s->id) {
        char id[9];
        gen_id(id);
        s->id = agentc_strdup(id);
    }
    if (!s->cwd) {
        char cwdbuf[PATH_MAX_];
        if (os_getcwd(cwdbuf, sizeof cwdbuf) < 0) cwdbuf[0] = 0;
        s->cwd = agentc_strdup(cwdbuf);
    }

    if (!have_header) {
        int rc = session_write_header(s);
        if (rc < 0) agentc_logf(2, "warning: cannot write session header (errno %d)", rc);
    }
    return s;
}

void agentc_session_close(AgcSession *s) {
    if (!s) return;
    if (s->fd >= 0) os_close(s->fd);
    agentc_free(s->path);
    agentc_free(s->id);
    agentc_free(s->cwd);
    agentc_free(s);
}

const char *agentc_session_path(const AgcSession *s) { return s ? s->path : NULL; }
const char *agentc_session_id(const AgcSession *s) { return s ? s->id : NULL; }

/* ---------------------------------------------------------------- append */

int agentc_session_append_message(AgcSession *s, const AgcMsg *m) {
    if (!s || !m) return -22;

    AgcBuf b = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "message");
    agentc_jsonw_key(&w, "role");
    agentc_jsonw_cstr(&w, role_name(m->role));
    agentc_jsonw_key(&w, "ts");
    agentc_jsonw_u64(&w, m->ts_ms);
    agentc_jsonw_key(&w, "content");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < m->nblocks; i++) {
        const AgcBlock *blk = &m->blocks[i];
        if (blk->type == AGENTC_BLK_TEXT) {
            agentc_jsonw_obj(&w);
            agentc_jsonw_key(&w, "type");
            agentc_jsonw_cstr(&w, "text");
            agentc_jsonw_key(&w, "text");
            agentc_jsonw_str(&w, blk->text ? blk->text : "", blk->text_len);
            agentc_jsonw_end(&w);
        } else if (blk->type == AGENTC_BLK_THINK) {
            agentc_jsonw_obj(&w);
            agentc_jsonw_key(&w, "type");
            agentc_jsonw_cstr(&w, "thinking");
            agentc_jsonw_key(&w, "thinking");
            agentc_jsonw_str(&w, blk->text ? blk->text : "", blk->text_len);
            agentc_jsonw_end(&w);
        } else if (blk->type == AGENTC_BLK_TOOLCALL) {
            agentc_jsonw_obj(&w);
            agentc_jsonw_key(&w, "type");
            agentc_jsonw_cstr(&w, "tool_call");
            agentc_jsonw_key(&w, "id");
            agentc_jsonw_cstr(&w, blk->tool_id ? blk->tool_id : "");
            agentc_jsonw_key(&w, "name");
            agentc_jsonw_cstr(&w, blk->tool_name ? blk->tool_name : "");
            agentc_jsonw_key(&w, "arguments");
            if (blk->tool_args && blk->tool_args[0])
                agentc_jsonw_str(&w, blk->tool_args, agentc_strlen(blk->tool_args));
            else
                agentc_jsonw_str(&w, "{}", 2);
            agentc_jsonw_end(&w);
        }
    }
    agentc_jsonw_end(&w);
    if (m->role == AGENTC_ROLE_TOOL) {
        const char *call_id = (m->nblocks && m->blocks[0].tool_id) ? m->blocks[0].tool_id : "";
        agentc_jsonw_key(&w, "tool_call_id");
        agentc_jsonw_cstr(&w, call_id);
        agentc_jsonw_key(&w, "is_error");
        agentc_jsonw_bool(&w, m->error != NULL);
    }
    agentc_jsonw_end(&w);
    agentc_buf_byte(&b, '\n');

    int rc = session_write(s, b.p, b.len);
    agentc_buf_free(&b);
    return rc;
}

int agentc_session_append_raw(AgcSession *s, const char *json, size_t n) {
    if (!s || !json) return -22;
    int rc = session_write(s, json, n);
    if (rc < 0) return rc;
    if (n == 0 || json[n - 1] != '\n') {
        char nl = '\n';
        rc = session_write(s, &nl, 1);
    }
    return rc;
}

/* The compaction checkpoint: written before the transcript splice so a crash
 * cannot lose the checkpoint. Fields mirror AGENTC_EV_COMPACT. */
int agentc_session_append_compaction(AgcSession *s, const AgcCompactInfo *ci) {
    if (!s || !ci) return -22;

    AgcBuf b = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "compaction");
    agentc_jsonw_key(&w, "ts");
    agentc_jsonw_i64(&w, now_ms());
    agentc_jsonw_key(&w, "tokens_before");
    agentc_jsonw_u64(&w, ci->tokens_before);
    agentc_jsonw_key(&w, "kept_messages");
    agentc_jsonw_u64(&w, ci->kept_messages);
    agentc_jsonw_key(&w, "automatic");
    agentc_jsonw_bool(&w, ci->automatic);
    agentc_jsonw_key(&w, "summary");
    if (ci->summary) agentc_jsonw_cstr(&w, ci->summary);
    else agentc_jsonw_null(&w);
    agentc_jsonw_end(&w);
    agentc_buf_byte(&b, '\n');

    int rc = session_write(s, b.p, b.len);
    agentc_buf_free(&b);
    return rc;
}

/* ---------------------------------------------------------------- replay */

/* A load can see a torn turn: an assistant with tool calls but no results
 * (crash after the assistant was persisted, before its tools ran), or a tool
 * result with no assistant call (older agents persisted a failed turn's
 * synthesized results). Either one makes every later request 400, so reconcile
 * the pairing once per load. Linear in the transcript; one tool run is short. */
static bool tool_run_has_id(const AgcMsg *msgs, size_t from, size_t to, const char *id) {
    if (!id) return false;
    for (size_t i = from; i < to; i++) {
        const AgcMsg *m = &msgs[i];
        if (m->role != AGENTC_ROLE_TOOL || m->nblocks == 0) continue;
        const char *rid = m->blocks[0].tool_id;
        if (rid && agentc_streq(rid, id)) return true;
    }
    return false;
}

static void tool_call_block_release(AgcBlock *b) {
    agentc_free(b->text);
    agentc_free(b->tool_id);
    agentc_free(b->tool_name);
    agentc_free(b->tool_args);
    agentc_memset(b, 0, sizeof *b);
}

static void reconcile_tool_pairs(AgcTranscript *t) {
    size_t w = 0, i = 0;
    while (i < t->n) {
        AgcMsg *m = &t->msgs[i];
        bool final_assistant = m->role == AGENTC_ROLE_ASSISTANT &&
                               m->stop_reason != AGENTC_STOP_ERROR &&
                               m->stop_reason != AGENTC_STOP_ABORTED;
        if (final_assistant) {
            size_t run_end = i + 1;
            while (run_end < t->n && t->msgs[run_end].role == AGENTC_ROLE_TOOL) run_end++;
            size_t calls_before = m->nblocks;
            /* Keep only the tool calls that have a result in the following run. */
            size_t bw = 0;
            for (size_t b = 0; b < m->nblocks; b++) {
                AgcBlock *blk = &m->blocks[b];
                if (blk->type == AGENTC_BLK_TOOLCALL &&
                    !tool_run_has_id(t->msgs, i + 1, run_end, blk->tool_id)) {
                    tool_call_block_release(blk);
                    continue;
                }
                if (bw != b) m->blocks[bw] = *blk;
                bw++;
            }
            m->nblocks = bw;
            if (m->nblocks == 0 && calls_before > 0) {
                /* every call was orphaned: drop the torn assistant and its run */
                agentc_msg_free(m);
                for (size_t j = i + 1; j < run_end; j++) agentc_msg_free(&t->msgs[j]);
                i = run_end;
                continue;
            }
            if (w != i) t->msgs[w] = *m;
            size_t asst_w = w;   /* the assistant slot: w advances past the results */
            w++;
            /* Keep only the results that answer a surviving call. */
            for (size_t j = i + 1; j < run_end; j++) {
                AgcMsg *r = &t->msgs[j];
                const char *id = (r->nblocks && r->blocks[0].tool_id) ? r->blocks[0].tool_id
                                                                       : NULL;
                bool keep = false;
                for (size_t b = 0; b < t->msgs[asst_w].nblocks && !keep; b++) {
                    AgcBlock *blk = &t->msgs[asst_w].blocks[b];
                    if (blk->type == AGENTC_BLK_TOOLCALL && blk->tool_id && id &&
                        agentc_streq(blk->tool_id, id))
                        keep = true;
                }
                if (!keep) {
                    agentc_msg_free(r);
                    continue;
                }
                if (w != j) t->msgs[w] = *r;
                w++;
            }
            i = run_end;
            continue;
        }
        if (m->role == AGENTC_ROLE_TOOL) {
            /* No assistant call in scope: an orphan left by an older failed turn. */
            agentc_msg_free(m);
            i++;
            continue;
        }
        if (w != i) t->msgs[w] = *m;
        w++;
        i++;
    }
    t->n = w;
}

int agentc_session_load_messages(const AgcSession *s, AgcTranscript *tr) {
    if (!s || !tr) return -22;
    if (!s->path) return 0;
    size_t len = 0;
    char *text = agentc_read_file_owned(s->path, &len);
    if (!text) return -1;
    AgcJsonArena *ja = agentc_json_arena_new(0);

    size_t skipped = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t ls = pos;
        while (pos < len && text[pos] != '\n') pos++;
        size_t le = pos;
        if (pos < len) pos++;
        if (le > ls && text[le - 1] == '\r') le--;
        if (ls == le) continue;

        AgcJson *root = agentc_json_parse_in(ja, text + ls, le - ls);
        if (!root) {
            skipped++;                                        /* skip bad lines */
            continue;
        }
        const char *type = agentc_json_get_str(root, "type");
        if (type && agentc_streq(type, "session")) {
            i64 ver = session_version(root);
            if (ver > 1) {
                /* Same refusal as agentc_session_open: the loader must not
                 * reinterpret a newer file (C.2.5). EPROTO = 71 on linux. */
                agentc_logf(2, "session: version %lld is newer than 1; refusing",
                        (long long)ver);
                agentc_json_arena_free(ja);
                agentc_free(text);
                return -71;
            }
            continue;
        }
        if (type && agentc_streq(type, "compaction")) {
            /* The checkpoint replaces the compacted prefix, but the tail that
             * was kept must survive: those messages were appended to the file
             * before this record, so retain the last `kept_messages` of the
             * replay and put the summary in front of them. Anything else would
             * silently drop the tail from --continue. `system` is owned by the
             * caller and is not touched. */
            const char *summary = agentc_json_get_str(root, "summary");
            size_t keep = (size_t)agentc_json_get_int(root, "kept_messages", 0);
            if (keep > tr->n) keep = tr->n;
            size_t drop = tr->n - keep;
            for (size_t i = 0; i < drop; i++) agentc_msg_free(&tr->msgs[i]);
            if (tr->cap < keep + 1) {
                tr->cap = keep + 1;
                tr->msgs = agentc_realloc(tr->msgs, tr->cap * sizeof(AgcMsg));
            }
            /* grow before the move: with keep == cap the destination index keep
             * would otherwise be one past the array. */
            if (keep) agentc_memmove(&tr->msgs[1], &tr->msgs[drop], keep * sizeof(AgcMsg));
            AgcMsg cs;
            agentc_memset(&cs, 0, sizeof cs);
            cs.role = AGENTC_ROLE_USER;
            cs.ts_ms = (u64)(os_now_ns(OS_CLOCK_REALTIME) / 1000000);
            if (summary) agentc_msg_add_text(&cs, summary, agentc_strlen(summary));
            tr->msgs[0] = cs;
            tr->n = keep + 1;
            continue;
        }
        if (!agentc_json_is(agentc_json_get(root, "type"), "message")) continue;
        int role = role_id(agentc_json_get_str(root, "role"));
        if (role < 0) continue;

        AgcMsg *m = agentc_transcript_push(tr, role);
        i64 ts = agentc_json_get_int(root, "ts", 0);
        if (ts > 0) m->ts_ms = (u64)ts;

        AgcJson *content = agentc_json_get(root, "content");
        for (size_t i = 0; i < agentc_json_len(content); i++) {
            AgcJson *blk = agentc_json_at(content, i);
            const char *bt = agentc_json_get_str(blk, "type");
            if (bt && agentc_streq(bt, "text")) {
                size_t tn = 0;
                const char *tx = agentc_json_str(agentc_json_get(blk, "text"), &tn);
                agentc_msg_add_text(m, tx ? tx : "", tn);
            } else if (bt && agentc_streq(bt, "thinking")) {
                agentc_msg_add_think(m);
                size_t tn = 0;
                const char *tx = agentc_json_str(agentc_json_get(blk, "thinking"), &tn);
                agentc_msg_block_append(&m->blocks[m->nblocks - 1], tx ? tx : "", tn);
            } else if (bt && agentc_streq(bt, "tool_call")) {
                const char *id = agentc_json_get_str(blk, "id");
                const char *name = agentc_json_get_str(blk, "name");
                agentc_msg_add_tool_call(m, id ? id : "", name ? name : "");
                size_t alen = 0;
                const char *args = agentc_json_str(agentc_json_get(blk, "arguments"), &alen);
                if (args) {
                    /* "arguments" is a JSON string holding the raw args text;
                     * the DOM already decoded it. */
                    agentc_msg_tool_args_append(m, args, alen);
                } else {
                    agentc_msg_tool_args_append(m, "{}", 2);
                }
            }
        }

        if (role == AGENTC_ROLE_TOOL) {
            const char *call_id = agentc_json_get_str(root, "tool_call_id");
            if (call_id && m->nblocks > 0) {
                agentc_free(m->blocks[0].tool_id);
                m->blocks[0].tool_id = agentc_strdup(call_id);
            }
            if (agentc_json_get_bool(root, "is_error", false)) {
                agentc_free(m->error);
                m->error = agentc_strdup("error");
            }
        }
    }
    reconcile_tool_pairs(tr);
    agentc_json_arena_free(ja);
    agentc_free(text);
    if (skipped > 0) {
        /* One warning, not one per line: a torn tail or a corrupted line is
         * normal after a crash, and a flood of warnings helps nobody. */
        agentc_logf(2, "session: %zu malformed line(s) skipped", skipped);
    }
    return 0;
}

/* ---------------------------------------------------------------- listing */

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = agentc_strlen(s), fl = agentc_strlen(suffix);
    return sl >= fl && agentc_memeq(s + sl - fl, suffix, fl);
}

static int list_cb(void *ud, const char *name, const char *full, bool is_dir) {
    (void)full;
    if (is_dir || !ends_with(name, ".jsonl")) return 0;
    AgcVec *v = ud;
    *(char **)agentc_vec_push(v, sizeof(char *)) = agentc_strdup(name);
    return 0;
}

char **agentc_session_list(const char *dir, size_t *count, size_t max) {
    if (count) *count = 0;
    char dflt[PATH_MAX_];
    if (!dir || !dir[0]) {
        dir = session_env_dir();
        if (!dir) {
            char cwdbuf[PATH_MAX_];
            if (os_getcwd(cwdbuf, sizeof cwdbuf) < 0) return NULL;
            if (!default_dir_for_cwd(cwdbuf, dflt, sizeof dflt)) return NULL;
            dir = dflt;
        }
    }

    AgcVec names = { 0 };
    if (agentc_dir_scan(dir, list_cb, &names) < 0 && names.len == 0) {
        agentc_vec_free(&names);
        return NULL;
    }
    agentc_sort_strings((char **)names.p, names.len, true); /* newest first */

    size_t n = names.len;
    if (max && n > max) n = max;
    char **out = NULL;
    if (n) {
        out = agentc_alloc(n * sizeof(char *));
        for (size_t i = 0; i < n; i++) {
            char full[PATH_MAX_];
            if (!agentc_path_join(full, sizeof full, dir, ((char **)names.p)[i])) {
                out[i] = agentc_strdup(((char **)names.p)[i]);
            } else {
                out[i] = agentc_strdup(full);
            }
        }
    }
    for (size_t i = 0; i < names.len; i++) agentc_free(((char **)names.p)[i]);
    agentc_vec_free(&names);
    if (count) *count = n;
    return out;
}

/* Read only the first (header) line of a session file and copy its recorded
 * cwd. Mirrors the header handling in agentc_session_open; returns false when
 * the line is missing or is not a readable v1 session header. The buffer grows
 * until the newline so a header longer than the initial chunk still resolves;
 * it is capped at 64 KiB, and a line larger than that is treated as no match
 * rather than costing an unbounded read. */
static bool session_header_cwd(const char *path, char *out, size_t cap) {
    out[0] = 0;
    if (!path || !path[0]) return false;
    int fd = os_open(path, OS_O_RDONLY, 0);
    if (fd < 0) return false;
    const size_t max = 65536;
    size_t bufsz = 8192;
    char *text = agentc_alloc(bufsz);
    size_t len = 0;
    bool overflow = false;
    for (;;) {
        if (len == bufsz) {
            if (bufsz >= max) {
                overflow = true;
                break;
            }
            size_t nsz = bufsz * 2;
            if (nsz > max) nsz = max;
            text = agentc_realloc(text, nsz);
            bufsz = nsz;
        }
        int n = os_read(fd, text + len, bufsz - len);
        if (n < 0) {
            os_close(fd);
            agentc_free(text);
            return false;
        }
        if (n == 0) break;
        len += (size_t)n;
        bool nl = false;
        for (size_t i = 0; i < len && !nl; i++) nl = text[i] == '\n';
        if (nl) break;
    }
    os_close(fd);
    if (overflow) {
        agentc_free(text);
        return false;
    }
    size_t le = 0;
    while (le < len && text[le] != '\n') le++;
    if (le == 0) {
        agentc_free(text);
        return false;
    }
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(ja, text, le);
    bool ok = false;
    if (agentc_json_is(agentc_json_get(root, "type"), "session") &&
        session_version(root) <= 1) {
        const char *cwd = agentc_json_get_str(root, "cwd");
        if (cwd) {
            agentc_snprintf(out, cap, "%s", cwd);
            ok = true;
        }
    }
    agentc_json_arena_free(ja);
    agentc_free(text);
    return ok;
}

char *agentc_session_find_latest(const char *dir, const char *cwd) {
    char dflt[PATH_MAX_];
    /* An explicit dir (--session-dir / AGENTC_SESSION_DIR) may be shared by
     * several projects; filter it by the recorded cwd so --continue does not
     * resume another project's conversation. The default per-cwd location
     * needs no filter. */
    bool explicit_dir = (dir && dir[0]) || session_env_dir() != NULL;
    if (!dir || !dir[0]) {
        dir = session_env_dir();
        if (!dir) {
            if (!cwd || !cwd[0]) return NULL;
            if (!default_dir_for_cwd(cwd, dflt, sizeof dflt)) return NULL;
            dir = dflt;
        }
    }
    bool filter = explicit_dir && cwd && cwd[0];
    size_t n = 0;
    char **list = agentc_session_list(dir, &n, filter ? 0 : 1);
    if (!list || n == 0) {
        agentc_sessions_free(list, n);
        return NULL;
    }
    size_t pick = 0;
    if (filter) {
        for (size_t i = 0; i < n; i++) {
            char hcwd[PATH_MAX_];
            if (session_header_cwd(list[i], hcwd, sizeof hcwd) && agentc_streq(hcwd, cwd)) {
                pick = i;
                break;
            }
        }
        /* no file records this cwd: fall back to the globally newest so a
         * shared dir with only foreign files still resumes something. */
    }
    char *latest = list[pick];
    list[pick] = NULL;
    agentc_sessions_free(list, n);
    return latest;
}

void agentc_sessions_free(char **paths, size_t count) {
    if (!paths) return;
    for (size_t i = 0; i < count; i++) agentc_free(paths[i]);
    agentc_free(paths);
}

/* ------------------------------------------------------------ picker summary */

/* The header and the first user message sit at the top of the file; a bounded
 * prefix is enough and keeps a picker over many sessions cheap. */
#define SESSION_SUMMARY_CAP (64 * 1024)

int agentc_session_summary(const char *path, i64 *timestamp_ms, char *preview, size_t cap) {
    if (timestamp_ms) *timestamp_ms = 0;
    if (preview && cap) preview[0] = 0;
    if (!path || !path[0]) return -22;
    int fd = os_open(path, OS_O_RDONLY, 0);
    if (fd < 0) return fd;
    char *buf = agentc_alloc(SESSION_SUMMARY_CAP);
    size_t len = 0;
    while (len < SESSION_SUMMARY_CAP) {
        int n = os_read(fd, buf + len, SESSION_SUMMARY_CAP - len);
        if (n < 0) {
            if (n == -4) continue;   /* EINTR */
            os_close(fd);
            agentc_free(buf);
            return n;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    os_close(fd);

    AgcJsonArena *ja = agentc_json_arena_new(0);
    size_t pos = 0;
    while (pos < len) {
        size_t ls = pos;
        while (pos < len && buf[pos] != '\n') pos++;
        size_t le = pos;
        if (pos < len) pos++;
        if (le > ls && buf[le - 1] == '\r') le--;
        if (ls == le) continue;
        AgcJson *root = agentc_json_parse_in(ja, buf + ls, le - ls);
        if (!root) continue;
        const char *type = agentc_json_get_str(root, "type");
        if (type && agentc_streq(type, "session")) {
            if (timestamp_ms) *timestamp_ms = agentc_json_get_int(root, "timestamp", 0);
            continue;
        }
        if (!(type && agentc_streq(type, "message"))) continue;
        const char *role = agentc_json_get_str(root, "role");
        if (!(role && agentc_streq(role, "user"))) continue;
        /* the frozen message shape holds an array of content blocks; the first
         * text block is the user's opening line */
        const AgcJson *content = agentc_json_get(root, "content");
        const char *text = NULL;
        if (agentc_json_type(content) == AGENTC_JSON_ARR) {
            size_t cn = agentc_json_len(content);
            for (size_t i = 0; i < cn && !text; i++) {
                const AgcJson *b = agentc_json_at(content, i);
                const char *bt = agentc_json_get_str(b, "type");
                if (bt && agentc_streq(bt, "text")) text = agentc_json_get_str(b, "text");
            }
        }
        if (text && preview && cap) {
            size_t k = 0;
            for (const char *p = text; *p && k + 1 < cap; p++) {
                char c = *p;
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                preview[k++] = c;
            }
            preview[k] = 0;
        }
        break;   /* only the first user message matters */
    }
    agentc_json_arena_free(ja);
    agentc_free(buf);
    return 0;
}
