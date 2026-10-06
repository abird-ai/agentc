/* jobs_test.c — the tool job driver: sync/async classification, start and
 * completion order, sequential no-overlap, exec normalization, the streaming
 * output cap and spill notices, and memory balance.
 */
#include "agent.h"
#include "core/tools/jobs.h"
#include "plat.h"

#define MAXC 40

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

/* ------------------------------------------------------------- fake tools */

static int g_sync_calls;
static int g_exec_calls;

static int echo_run(const AgcTool *self, const AgcToolCall *call,
                    AgcBuf *out, bool *is_error) {
    (void)self;
    g_sync_calls++;
    agentc_buf_cstr(out, call->args_json != NULL ? call->args_json : "");
    *is_error = false;
    return 0;
}

static int fail_run(const AgcTool *self, const AgcToolCall *call,
                    AgcBuf *out, bool *is_error) {
    (void)self;
    (void)call;
    agentc_buf_cstr(out, "expected failure");
    *is_error = true;
    return 0;
}

/* Sleeps past its own 1 ms deadline, then asks the driver's checkpoint. */
static int slow_run(const AgcTool *self, const AgcToolCall *call,
                    AgcBuf *out, bool *is_error) {
    (void)self;
    (void)call;
    (void)out;
    (void)is_error;
    os_sleep_ns(20 * 1000000);
    return agentc_tool_checkpoint();
}

static char *exec_only_exec(const char *args_json, bool *is_error,
                            const volatile bool *cancel) {
    (void)cancel;
    g_exec_calls++;
    AgcBuf b = { 0 };
    agentc_buf_printf(&b, "exec(%s)", args_json != NULL ? args_json : "");
    if (is_error) *is_error = false;
    return (char *)b.p;
}

typedef struct {
    int start_seq;
    int polls_left;
} AsyncState;

static int g_start_counter;
static int g_start_order[8];
static int g_start_n;
static int g_active;
static int g_max_active;

static int async_start(const AgcTool *self, const AgcToolCall *call, AgcJob *job) {
    (void)call;
    (void)job;
    AsyncState *s = self->ud;
    s->start_seq = ++g_start_counter;
    if (g_start_n < 8) g_start_order[g_start_n++] = s->start_seq;
    g_active++;
    if (g_active > g_max_active) g_max_active = g_active;
    return 0;
}

static int async_step(const AgcTool *self, AgcJob *job) {
    (void)job;
    AsyncState *s = self->ud;
    if (s->polls_left <= 0) {
        g_active--;
        return 1;
    }
    s->polls_left--;
    return 0;
}

static const AgcTool echo_tool = {
    .name = "sync.echo", .label = "Echo", .desc = "", .params_json = "{}",
    .flags = 0, .exec = NULL, .ud = NULL, .timeout_ms = 0,
    .run = echo_run, .start = NULL, .step = NULL,
};

static const AgcTool fail_tool = {
    .name = "sync.fail", .label = "Fail", .desc = "", .params_json = "{}",
    .flags = 0, .exec = NULL, .ud = NULL, .timeout_ms = 0,
    .run = fail_run, .start = NULL, .step = NULL,
};

static const AgcTool slow_tool = {
    .name = "sync.slow", .label = "Slow", .desc = "", .params_json = "{}",
    .flags = 0, .exec = NULL, .ud = NULL, .timeout_ms = 1,
    .run = slow_run, .start = NULL, .step = NULL,
};

static const AgcTool exec_tool = {
    .name = "exec-only", .label = "Exec", .desc = "", .params_json = "{}",
    .flags = 0, .exec = exec_only_exec, .ud = NULL, .timeout_ms = 0,
    .run = NULL, .start = NULL, .step = NULL,
};

static AgcTool async_tool(const char *name, unsigned flags, AsyncState *st) {
    AgcTool t = {
        .name = name, .label = name, .desc = "", .params_json = "{}",
        .flags = flags, .exec = NULL, .ud = st, .timeout_ms = 0,
        .run = NULL, .start = async_start, .step = async_step,
    };
    return t;
}

static void reset_async(void) {
    g_start_counter = 0;
    g_start_n = 0;
    g_active = 0;
    g_max_active = 0;
}

/* --------------------------------------------- cleanup hook */

#define JCLEAN_MAGIC 0xC1EA9001u

typedef struct {
    u32 magic;
    int calls;
} JClean;

static JClean *g_cleanup_seen;
static int g_cleanup_calls;
static int g_cleanup_reason;
static bool g_cleanup_priv_ok;

static void cleanup_hook(const AgcTool *self, AgcJob *job, int reason) {
    (void)self;
    g_cleanup_calls++;
    g_cleanup_reason = reason;
    JClean *p = (JClean *)job->priv;
    g_cleanup_seen = p;
    g_cleanup_priv_ok = p != NULL && p->magic == JCLEAN_MAGIC;
}

/* 0 = completes and leaves cleanup set (release), 1 = fatal step,
 * 2 = step sleeps past the deadline (timeout), 3 = step sets cancel. */
typedef struct {
    int mode;
} CleanState;

static bool g_clean_cancel;

static int clean_start(const AgcTool *self, const AgcToolCall *call, AgcJob *job) {
    (void)call;
    (void)self;
    JClean *p = agentc_alloc(sizeof *p);
    p->magic = JCLEAN_MAGIC;
    job->priv = p;
    job->cleanup = cleanup_hook;
    return 0;
}

static int clean_step(const AgcTool *self, AgcJob *job) {
    (void)job;
    const CleanState *s = self->ud;
    if (s->mode == 1) return -22;   /* fatal */
    if (s->mode == 2) {
        os_sleep_ns(20 * 1000000);
        return 0;                   /* the deadline is long past */
    }
    if (s->mode == 3) {
        g_clean_cancel = true;
        return 0;
    }
    return 1;                       /* complete; cleanup left for release */
}

static AgcTool clean_tool(const char *name, CleanState *st, int timeout_ms) {
    AgcTool t = {
        .name = name, .label = name, .desc = "", .params_json = "{}",
        .flags = 0, .exec = NULL, .ud = st, .timeout_ms = timeout_ms,
        .run = NULL, .start = clean_start, .step = clean_step,
    };
    return t;
}

static void reset_cleanup(void) {
    g_cleanup_seen = NULL;
    g_cleanup_calls = 0;
    g_cleanup_reason = 0;
    g_cleanup_priv_ok = false;
    g_clean_cancel = false;
}

/* ----------------------------------------- cleanup name ordering */

/* The cleanup hook wipes the tool name in place. The driver must build its
 * fatal/timeout message before cleanup runs, or the text would read the wiped
 * buffer (the real registry frees the record and its name there). */

static char *g_dyn_name;
static int g_dyn_cleanups;
static int g_dyn_reason;
static bool g_dyn_priv_ok;

static void dyn_cleanup(const AgcTool *self, AgcJob *job, int reason) {
    (void)self;
    g_dyn_cleanups++;
    g_dyn_reason = reason;
    g_dyn_priv_ok = job->priv != NULL && *(u32 *)job->priv == JCLEAN_MAGIC;
    if (g_dyn_name) agentc_memset(g_dyn_name, 'X', agentc_strlen(g_dyn_name));
}

/* 1 = fatal step, 2 = timeout. */
typedef struct {
    int mode;
} DynState;

static int dyn_start(const AgcTool *self, const AgcToolCall *call, AgcJob *job) {
    (void)self;
    (void)call;
    JClean *p = agentc_alloc(sizeof *p);
    p->magic = JCLEAN_MAGIC;
    job->priv = p;
    job->cleanup = dyn_cleanup;
    return 0;
}

static int dyn_step(const AgcTool *self, AgcJob *job) {
    (void)job;
    const DynState *s = self->ud;
    if (s->mode == 1) return -22;   /* fatal */
    os_sleep_ns(20 * 1000000);      /* land the 1 ms deadline */
    return 0;
}

static AgcTool dyn_tool(DynState *st, int timeout_ms) {
    AgcTool t = {
        .name = g_dyn_name, .label = g_dyn_name, .desc = "", .params_json = "{}",
        .flags = 0, .exec = NULL, .ud = st, .timeout_ms = timeout_ms,
        .run = NULL, .start = dyn_start, .step = dyn_step,
    };
    return t;
}

/* ------------------------------------------------------------- collector */

typedef struct {
    AgcBuf ev;
    AgcBuf result[MAXC];
    bool is_error[MAXC];
    int state[MAXC];
    bool started[MAXC];
    bool done[MAXC];
    i64 duration[MAXC];
    int done_order[MAXC];
    int ndone;
} Ctx;

static int on_job(void *ud, int what, const AgcJob *job) {
    Ctx *c = ud;
    size_t i = job->index;
    if (i >= MAXC) return 0;
    if (what == AGENTC_JOB_STARTED) {
        c->started[i] = true;
        agentc_buf_printf(&c->ev, "s%zu ", i);
        return 0;
    }
    if (job->out.len > 0) agentc_buf_push(&c->result[i], job->out.p, job->out.len);
    c->is_error[i] = job->is_error;
    c->state[i] = job->state;
    c->duration[i] = job->duration_ms;
    c->done[i] = true;
    if (c->ndone < MAXC) c->done_order[c->ndone++] = (int)i;
    agentc_buf_printf(&c->ev, "d%zu ", i);
    return 0;
}

static void ctx_free(Ctx *c) {
    agentc_buf_free(&c->ev);
    for (size_t i = 0; i < MAXC; i++) agentc_buf_free(&c->result[i]);
}

static const char *ctx_out(const Ctx *c, size_t i) {
    return c->result[i].p != NULL ? (const char *)c->result[i].p : "";
}

/* --------------------------------------------------------------- helpers */

static AgcToolCall make_call(const char *id, const char *name, const char *args,
                                const volatile bool *cancel) {
    AgcToolCall call = { id, name, args, cancel };
    return call;
}

static bool read_file(const char *path, char *buf, size_t cap) {
    int fd = os_open(path, OS_O_RDONLY, 0);
    if (fd < 0) return false;
    size_t n = 0;
    while (n + 1 < cap) {
        int r = os_read(fd, buf + n, cap - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    os_close(fd);
    buf[n] = 0;
    return true;
}

/* ---------------------------------------------------------------- cases */

static void test_unknown(void) {
    bool cancel = false;
    AgcToolCall call = make_call("c1", "nope", "{}", &cancel);
    const AgcTool *tools[1] = { NULL };
    const char *pre[1] = { NULL };
    Ctx c;
    agentc_memset(&c, 0, sizeof c);

    int r = agentc_tool_jobs_run(tools, pre, &call, 1, on_job, &c);
    bool ok = r == 0 && c.started[0] && c.done[0] && c.is_error[0] && c.state[0] == 2 &&
              agentc_streq(ctx_out(&c, 0), "error: unknown tool: nope");
    check("jobs.unknown", ok);
    ctx_free(&c);
}

static void test_pre_error(void) {
    bool cancel = false;
    AsyncState st;
    agentc_memset(&st, 0, sizeof st);
    AgcTool at = async_tool("async.fake", 0, &st);
    const AgcTool *tools[2] = { &echo_tool, &at };
    AgcToolCall calls[2];
    calls[0] = make_call("c1", "sync.echo", "hello", &cancel);
    calls[1] = make_call("c2", "async.fake", "{}", &cancel);
    const char *pre[2] = { "error: tool blocked: denied by policy",
                           "error: tool blocked: denied by policy" };
    Ctx c;
    agentc_memset(&c, 0, sizeof c);
    g_sync_calls = 0;
    reset_async();

    int r = agentc_tool_jobs_run(tools, pre, calls, 2, on_job, &c);
    bool ok = r == 0 && c.started[0] && c.started[1] && c.done[0] && c.done[1] &&
              c.is_error[0] && c.is_error[1] &&
              agentc_streq(ctx_out(&c, 0), "error: tool blocked: denied by policy") &&
              agentc_streq(ctx_out(&c, 1), "error: tool blocked: denied by policy") &&
              g_sync_calls == 0 && g_start_n == 0;
    check("jobs.pre_error", ok);
    ctx_free(&c);
}

static void test_sync_ok(void) {
    bool cancel = false;
    AgcToolCall call = make_call("c1", "sync.echo", "hello world", &cancel);
    const AgcTool *tools[1] = { &echo_tool };
    Ctx c;
    agentc_memset(&c, 0, sizeof c);
    g_sync_calls = 0;

    int r = agentc_tool_jobs_run(tools, NULL, &call, 1, on_job, &c);
    bool ok = r == 0 && c.started[0] && c.done[0] && !c.is_error[0] && c.state[0] == 2 &&
              agentc_streq(ctx_out(&c, 0), "hello world") && g_sync_calls == 1 &&
              c.duration[0] >= 0;
    check("jobs.sync.ok", ok);
    ctx_free(&c);
}

static void test_sync_error(void) {
    bool cancel = false;
    AgcToolCall call = make_call("c1", "sync.fail", "{}", &cancel);
    const AgcTool *tools[1] = { &fail_tool };
    Ctx c;
    agentc_memset(&c, 0, sizeof c);

    int r = agentc_tool_jobs_run(tools, NULL, &call, 1, on_job, &c);
    bool ok = r == 0 && c.started[0] && c.done[0] && c.is_error[0] && c.state[0] == 2 &&
              agentc_streq(ctx_out(&c, 0), "expected failure");
    check("jobs.sync.error", ok);
    ctx_free(&c);
}

static void test_sync_timeout(void) {
    bool cancel = false;
    AgcToolCall call = make_call("c1", "sync.slow", "{}", &cancel);
    const AgcTool *tools[1] = { &slow_tool };
    Ctx c;
    agentc_memset(&c, 0, sizeof c);

    int r = agentc_tool_jobs_run(tools, NULL, &call, 1, on_job, &c);
    bool ok = r == 0 && c.started[0] && c.done[0] && c.is_error[0] && c.state[0] == 2 &&
              agentc_streq(ctx_out(&c, 0), "error: tool 'sync.slow' timed out\n");
    check("jobs.sync.timeout", ok);
    ctx_free(&c);
}

static void test_exec_shim(void) {
    bool cancel = false;
    AgcToolCall call = make_call("c1", "exec-only", "{\"x\":1}", &cancel);
    const AgcTool *tools[1] = { &exec_tool };
    Ctx c;
    agentc_memset(&c, 0, sizeof c);
    g_exec_calls = 0;

    int r = agentc_tool_jobs_run(tools, NULL, &call, 1, on_job, &c);
    bool ok = r == 0 && c.started[0] && c.done[0] && !c.is_error[0] && g_exec_calls == 1 &&
              agentc_streq(ctx_out(&c, 0), "exec({\"x\":1})");
    check("jobs.exec-shim", ok);
    ctx_free(&c);
}

static void test_async_start_order(void) {
    bool cancel = false;
    AsyncState st[3];
    agentc_memset(st, 0, sizeof st);
    AgcTool t[3];
    const AgcTool *tools[3];
    AgcToolCall calls[3];
    for (size_t i = 0; i < 3; i++) {
        t[i] = async_tool("async.fake", 0, &st[i]);
        tools[i] = &t[i];
        calls[i] = make_call("c", "async.fake", "{}", &cancel);
    }
    Ctx c;
    agentc_memset(&c, 0, sizeof c);
    reset_async();

    int r = agentc_tool_jobs_run(tools, NULL, calls, 3, on_job, &c);
    bool ok = r == 0 && c.started[0] && c.started[1] && c.started[2] &&
              c.done[0] && c.done[1] && c.done[2] &&
              g_start_order[0] == 1 && g_start_order[1] == 2 && g_start_order[2] == 3 &&
              agentc_str_starts((const char *)c.ev.p, c.ev.len, "s0 s1 s2 ");
    check("jobs.async.start-order", ok);
    ctx_free(&c);
}

static void test_async_complete_order(void) {
    bool cancel = false;
    AsyncState st[2];
    agentc_memset(st, 0, sizeof st);
    st[0].polls_left = 1;   /* finishes on the second drive iteration */
    st[1].polls_left = 0;   /* finishes on the first */
    AgcTool t[2];
    const AgcTool *tools[2];
    AgcToolCall calls[2];
    for (size_t i = 0; i < 2; i++) {
        t[i] = async_tool("async.fake", 0, &st[i]);
        tools[i] = &t[i];
        calls[i] = make_call("c", "async.fake", "{}", &cancel);
    }
    Ctx c;
    agentc_memset(&c, 0, sizeof c);
    reset_async();

    int r = agentc_tool_jobs_run(tools, NULL, calls, 2, on_job, &c);
    bool ok = r == 0 && c.ndone == 2 && c.done_order[0] == 1 && c.done_order[1] == 0 &&
              c.started[0] && c.started[1];
    check("jobs.async.complete-order", ok);
    ctx_free(&c);
}

static void test_sequential_no_overlap(void) {
    bool cancel = false;
    AsyncState st[2];
    agentc_memset(st, 0, sizeof st);
    AgcTool t[2];
    const AgcTool *tools[2];
    AgcToolCall calls[2];
    for (size_t i = 0; i < 2; i++) {
        t[i] = async_tool("async.fake", i == 0 ? AGENTC_TOOL_SEQUENTIAL : 0, &st[i]);
        tools[i] = &t[i];
        calls[i] = make_call("c", "async.fake", "{}", &cancel);
    }
    Ctx c;
    agentc_memset(&c, 0, sizeof c);
    reset_async();

    int r = agentc_tool_jobs_run(tools, NULL, calls, 2, on_job, &c);
    bool ok = r == 0 && g_max_active == 1 && c.ndone == 2 &&
              c.done_order[0] == 0 && c.done_order[1] == 1 &&
              agentc_streq((const char *)c.ev.p, "s0 d0 s1 d1 ");
    check("jobs.sequential.no-overlap", ok);
    ctx_free(&c);
}

static void test_overflow(void) {
    bool cancel = false;
    enum { N = AGENTC_TOOL_JOBS_MAX + 1 };
    AgcToolCall calls[N];
    const AgcTool *tools[N];
    for (size_t i = 0; i < N; i++) {
        calls[i] = make_call("c", "sync.echo", "{}", &cancel);
        tools[i] = &echo_tool;
    }
    Ctx c;
    agentc_memset(&c, 0, sizeof c);
    g_sync_calls = 0;

    int r = agentc_tool_jobs_run(tools, NULL, calls, N, on_job, &c);
    bool ok = r == -34 && c.ndone == N && !c.started[0] && c.is_error[0] &&
              c.state[0] == 2 && c.is_error[N - 1] &&
              agentc_streq(ctx_out(&c, 0), "error: too many tool calls\n") &&
              agentc_streq(ctx_out(&c, N - 1), "error: too many tool calls\n") &&
              g_sync_calls == 0;
    check("jobs.overflow.32", ok);
    ctx_free(&c);
}

static void test_cleanup_hook(void) {
    bool cancel = false;
    Ctx c;

    /* fatal step -> cleanup(ERROR) exactly once, with priv still valid */
    CleanState fatal = { 1 };
    AgcTool ft = clean_tool("clean.fatal", &fatal, 0);
    const AgcTool *ftools[1] = { &ft };
    AgcToolCall fcall = make_call("c", "clean.fatal", "{}", &cancel);
    agentc_memset(&c, 0, sizeof c);
    reset_cleanup();
    (void)agentc_tool_jobs_run(ftools, NULL, &fcall, 1, on_job, &c);
    check("cleanup.fatal", g_cleanup_calls == 1 && g_cleanup_reason == 1 &&
                          g_cleanup_priv_ok && g_cleanup_seen != NULL &&
                          c.is_error[0]);
    ctx_free(&c);

    /* timeout -> cleanup(TIMEDOUT); the driver must not call it again at
     * release (the hook clears itself before running) */
    CleanState tmo = { 2 };
    AgcTool tt = clean_tool("clean.timeout", &tmo, 1);
    const AgcTool *ttools[1] = { &tt };
    AgcToolCall tcall = make_call("c", "clean.timeout", "{}", &cancel);
    agentc_memset(&c, 0, sizeof c);
    reset_cleanup();
    (void)agentc_tool_jobs_run(ttools, NULL, &tcall, 1, on_job, &c);
    check("cleanup.timeout", g_cleanup_calls == 1 && g_cleanup_reason == 2 &&
                            g_cleanup_priv_ok && c.is_error[0]);
    ctx_free(&c);

    /* cancel -> cleanup(CANCELLED) */
    CleanState can = { 3 };
    AgcTool ct = clean_tool("clean.cancel", &can, 0);
    const AgcTool *ctools[1] = { &ct };
    AgcToolCall ccall = make_call("c", "clean.cancel", "{}", &g_clean_cancel);
    agentc_memset(&c, 0, sizeof c);
    reset_cleanup();
    (void)agentc_tool_jobs_run(ctools, NULL, &ccall, 1, on_job, &c);
    check("cleanup.cancel", g_cleanup_calls == 1 && g_cleanup_reason == 3 &&
                           g_cleanup_priv_ok);
    ctx_free(&c);

    /* normal completion with the hook still set -> cleanup(RELEASE) runs
     * before the driver frees priv */
    CleanState rel = { 0 };
    AgcTool rt = clean_tool("clean.release", &rel, 0);
    const AgcTool *rtools[1] = { &rt };
    AgcToolCall rcall = make_call("c", "clean.release", "{}", &cancel);
    agentc_memset(&c, 0, sizeof c);
    reset_cleanup();
    (void)agentc_tool_jobs_run(rtools, NULL, &rcall, 1, on_job, &c);
    check("cleanup.release", g_cleanup_calls == 1 && g_cleanup_reason == 4 &&
                            g_cleanup_priv_ok && !c.is_error[0]);
    ctx_free(&c);
}

static void test_cleanup_name_order(void) {
    bool cancel = false;
    Ctx c;

    /* fatal: cleanup wipes tool->name; the message still needs the real one */
    DynState fatal = { 1 };
    g_dyn_name = agentc_strdup("dyn.fatal");
    AgcTool ft = dyn_tool(&fatal, 0);
    const AgcTool *ftools[1] = { &ft };
    AgcToolCall fcall = make_call("c", "dyn.fatal", "{}", &cancel);
    agentc_memset(&c, 0, sizeof c);
    g_dyn_cleanups = 0;
    g_dyn_reason = 0;
    g_dyn_priv_ok = false;
    (void)agentc_tool_jobs_run(ftools, NULL, &fcall, 1, on_job, &c);
    check("cleanup.name.fatal",
          g_dyn_cleanups == 1 && g_dyn_reason == 1 && g_dyn_priv_ok &&
              agentc_streq(ctx_out(&c, 0),
                           "error: tool 'dyn.fatal' failed to run (-22)\n"));
    ctx_free(&c);
    agentc_free(g_dyn_name);
    g_dyn_name = NULL;

    /* timeout: same guarantee on the timed-out path */
    DynState tmo = { 2 };
    g_dyn_name = agentc_strdup("dyn.timeout");
    AgcTool tt = dyn_tool(&tmo, 1);
    const AgcTool *ttools[1] = { &tt };
    AgcToolCall tcall = make_call("c", "dyn.timeout", "{}", &cancel);
    agentc_memset(&c, 0, sizeof c);
    g_dyn_cleanups = 0;
    g_dyn_reason = 0;
    g_dyn_priv_ok = false;
    (void)agentc_tool_jobs_run(ttools, NULL, &tcall, 1, on_job, &c);
    check("cleanup.name.timeout",
          g_dyn_cleanups == 1 && g_dyn_reason == 2 && g_dyn_priv_ok &&
              agentc_streq(ctx_out(&c, 0), "error: tool 'dyn.timeout' timed out\n"));
    ctx_free(&c);
    agentc_free(g_dyn_name);
    g_dyn_name = NULL;
}

static void test_append_cap(void) {
    AgcJob j;
    agentc_tool_job_init(&j);
    (void)agentc_tool_append_output(&j, "abcdefghij", 10, 4);
    (void)agentc_tool_append_output(&j, "klm", 3, 4);
    int fin = agentc_tool_finalize_output(&j);

    char full[64];
    bool read_ok = j.spill_path.len > 0 &&
                   read_file((const char *)j.spill_path.p, full, sizeof full);
    bool ok = j.total_bytes == 13 && read_ok && agentc_streq(full, "abcdefghijklm") &&
              agentc_str_starts((const char *)j.out.p, j.out.len, "abcd") &&
              agentc_str_str((const char *)j.out.p, "[output truncated: full output: ") != NULL &&
              fin == 1 && j.spill_fd == -1;
    check("jobs.append.cap", ok);

    agentc_buf_free(&j.out);
    agentc_buf_free(&j.spill_path);
}

static void test_append_spill_notice(void) {
    AgcJob j;
    agentc_tool_job_init(&j);
    (void)agentc_tool_append_output(&j, "abcdefghij", 10, 4);
    j.spill_notified = true;   /* bash writes its own operator-facing notice */
    int fin = agentc_tool_finalize_output(&j);

    bool ok = fin == 1 && agentc_streq((const char *)j.out.p, "abcd") &&
              j.spill_path.len > 0;
    check("jobs.append.spill-notice", ok);

    agentc_buf_free(&j.out);
    agentc_buf_free(&j.spill_path);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    size_t mem_base = agentc_mem_live();

    test_unknown();
    test_pre_error();
    test_sync_ok();
    test_sync_error();
    test_sync_timeout();
    test_exec_shim();
    test_async_start_order();
    test_async_complete_order();
    test_sequential_no_overlap();
    test_overflow();
    test_append_cap();
    test_append_spill_notice();
    test_cleanup_hook();
    test_cleanup_name_order();

    check("jobs.mem", agentc_mem_live() == mem_base);
    return fails;
}
