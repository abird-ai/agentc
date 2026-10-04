/* jobs.c — bounded tool job driver (see core/tools/jobs.h).
 *
 * One batch at a time: g_jobs is static. Async tools start first in source
 * order so their children overlap the slow synchronous tools; completion
 * callbacks arrive in completion order. Every job is finalized (spill closed,
 * truncation notice appended) before AGENTC_JOB_DONE, and the whole batch is
 * released before agentc_tool_jobs_run() returns.
 */
#include "core/tools/jobs.h"

#include "base/limits.h"
#include "plat.h"

/* Linux errno values, negative, as everywhere in the core. */
#define JOB_ECANCELED (-125)
#define JOB_ETIMEDOUT (-110)
#define JOB_EINVAL    (-22)
#define JOB_ERANGE    (-34)
#define JOB_EINTR     (-4)

enum { JOB_FREE = 0, JOB_RUNNING, JOB_DONE, JOB_CANCELLED };

static AgcJob g_jobs[AGENTC_TOOL_JOBS_MAX];

/* The job whose synchronous run() is executing, for agentc_tool_checkpoint().
 * The job table is documented as non-reentrant, so one slot is enough. */
static AgcJob *g_current;

/* When the job actually started running (not when the batch was set up), per
 * slot; job_reset clears it so a pre-failed job reports duration 0. */
static i64 g_start_ns[AGENTC_TOOL_JOBS_MAX];

static bool job_aborted(const AgcJob *j) {
    return j->call.cancel != NULL && *j->call.cancel;
}

static bool jobs_aborted(size_t n) {
    for (size_t i = 0; i < n; i++)
        if (job_aborted(&g_jobs[i])) return true;
    return false;
}

static int job_checkpoint(const AgcJob *j) {
    if (j != NULL && job_aborted(j)) return JOB_ECANCELED;
    if (j != NULL && j->deadline_ns != 0 &&
        os_now_ns(OS_CLOCK_MONOTONIC) >= j->deadline_ns)
        return JOB_ETIMEDOUT;
    return 0;
}

int agentc_tool_checkpoint(void) { return job_checkpoint(g_current); }

void agentc_tool_job_init(AgcJob *j) {
    agentc_memset(j, 0, sizeof *j);
    j->state = JOB_FREE;
    j->pid = -1;
    j->fd = -1;
    j->spill_fd = -1;
}

static u64 ms_to_ns(int ms) {
    if (ms <= 0) ms = AGENTC_TOOL_TIMEOUT_DEF_MS;
    return (u64)ms * 1000000ull;
}

static const char *args_of(const AgcJob *j) {
    return j->call.args_json != NULL ? j->call.args_json : "{}";
}

static void job_reset(AgcJob *j, const AgcTool *tool, const AgcToolCall *call,
                      size_t index) {
    agentc_tool_job_init(j);
    j->tool = tool;
    if (call != NULL) j->call = *call;
    j->index = index;
    if (tool != NULL)
        j->deadline_ns = os_now_ns(OS_CLOCK_MONOTONIC) + (i64)ms_to_ns(tool->timeout_ms);
    g_start_ns[index] = 0;
}

/* ------------------------------------------------------------- lifecycle */

static void job_started(AgcJob *j, AgcJobCb cb, void *ud) {
    j->state = JOB_RUNNING;
    g_start_ns[j->index] = os_now_ns(OS_CLOCK_MONOTONIC);
    if (cb != NULL) (void)cb(ud, AGENTC_JOB_STARTED, j);
}

static void job_finished(AgcJob *j, AgcJobCb cb, void *ud) {
    i64 start = g_start_ns[j->index];
    if (start != 0)
        j->duration_ms = (os_now_ns(OS_CLOCK_MONOTONIC) - start) / 1000000;
    (void)agentc_tool_finalize_output(j);
    if (cb != NULL) (void)cb(ud, AGENTC_JOB_DONE, j);
}

static void job_pre_error(AgcJob *j, const char *text) {
    agentc_buf_clear(&j->out);
    agentc_buf_cstr(&j->out, text != NULL ? text : "");
    j->is_error = true;
    j->state = JOB_DONE;
}

static void job_unknown(AgcJob *j) {
    agentc_buf_clear(&j->out);
    agentc_buf_printf(&j->out, "error: unknown tool: %s",
                      j->call.name != NULL ? j->call.name : "");
    j->is_error = true;
    j->state = JOB_DONE;
}

static void job_no_runner(AgcJob *j) {
    agentc_buf_clear(&j->out);
    agentc_buf_printf(&j->out, "error: tool '%s' has no runner\n",
                      j->tool != NULL ? j->tool->name : j->call.name);
    j->is_error = true;
    j->state = JOB_DONE;
}

static void close_fd(AgcJob *j) {
    if (j->fd >= 0) {
        os_close(j->fd);
        j->fd = -1;
    }
}

/* Bounded reap: SIGKILL was already sent, so this cannot take long.  Every
 * wait is the non-blocking form inside a poll slice; a child stuck in
 * uninterruptible sleep must not wedge the driver (or shutdown). */
static void reap(AgcJob *j) {
    if (j->pid <= 0) {
        j->pid = -1;
        return;
    }
    for (int i = 0; i < 200; i++) {
        if (os_wait(j->pid, true) != -1) {
            j->pid = -1;
            return;
        }
        os_poll(NULL, 0, 5);   /* up to ~1 s total */
    }
    j->pid = -1;
}

static void kill_hard(AgcJob *j) {
    if (j->pid <= 0) return;
    os_kill(-j->pid, 9);       /* group; os_kill falls back to the leader */
    reap(j);
}

/* Abort: SIGTERM the group, a bounded ~100 ms grace, then SIGKILL the group
 * and reap without ever blocking the driver loop. */
static void kill_soft(AgcJob *j) {
    if (j->pid <= 0) return;
    os_kill(-j->pid, 15);
    for (int i = 0; i < 20; i++) {
        if (os_wait(j->pid, true) != -1) {
            j->pid = -1;
            return;
        }
        os_poll(NULL, 0, 5);   /* ~100 ms total, 5 ms slices */
    }
    kill_hard(j);
}

/* The cleanup hook: run once, then clear it, so the release sweep can
 * never call it a second time. The callback sees job->priv before the driver
 * frees it. */
static void job_cleanup(AgcJob *j, int reason) {
    void (*fn)(const AgcTool *, AgcJob *, int) = j->cleanup;
    if (fn == NULL) return;
    j->cleanup = NULL;
    fn(j->tool, j, reason);
}

static void job_fatal(AgcJob *j, int err) {
    /* Build the message before cleanup: for an async extension tool the
     * cleanup may unlink and release the record that owns tool->name. */
    AgcBuf msg = { 0 };
    if (j->tool != NULL)
        agentc_buf_printf(&msg, "error: tool '%s' failed to run (%d)\n",
                          j->tool->name, err);
    job_cleanup(j, AGENTC_JOB_STOP_ERROR);
    close_fd(j);
    kill_hard(j);
    agentc_buf_clear(&j->out);
    if (msg.p != NULL) agentc_buf_push(&j->out, msg.p, msg.len);
    agentc_buf_free(&msg);
    j->is_error = true;
    j->state = JOB_DONE;
}

static void job_timeout(AgcJob *j) {
    /* Same as job_fatal: the name must be read before cleanup can release the
     * record that owns it. */
    AgcBuf msg = { 0 };
    const char *name = j->tool != NULL ? j->tool->name
                                       : (j->call.name != NULL ? j->call.name : "");
    agentc_buf_printf(&msg, "error: tool '%s' timed out\n", name);
    job_cleanup(j, AGENTC_JOB_STOP_TIMEDOUT);
    close_fd(j);
    kill_hard(j);
    agentc_buf_clear(&j->out);
    if (msg.p != NULL) agentc_buf_push(&j->out, msg.p, msg.len);
    agentc_buf_free(&msg);
    j->is_error = true;
    j->state = JOB_DONE;
}

/* Keep whatever partial output the run produced; an empty one still needs an
 * answer for the model. */
static void job_cancel(AgcJob *j) {
    job_cleanup(j, AGENTC_JOB_STOP_CANCELLED);
    close_fd(j);
    kill_soft(j);
    if (j->out.len == 0) agentc_buf_cstr(&j->out, "error: aborted");
    j->is_error = true;
    j->state = JOB_CANCELLED;
}

static void jobs_release(size_t n) {
    for (size_t i = 0; i < n; i++) {
        AgcJob *j = &g_jobs[i];
        close_fd(j);
        kill_hard(j);
        job_cleanup(j, AGENTC_JOB_STOP_RELEASE);
        agentc_free(j->priv);
        j->priv = NULL;
        agentc_buf_free(&j->out);
        agentc_buf_free(&j->spill_path);
        g_start_ns[i] = 0;
        j->state = JOB_FREE;
    }
}

static size_t running_count(size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (g_jobs[i].state == JOB_RUNNING) k++;
    return k;
}

/* ---------------------------------------------------------- output + spill */

static int write_span(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n > 0) {
        int w = os_write(fd, q, n);
        if (w < 0) {
            if (w == JOB_EINTR) continue;
            return w;
        }
        if (w == 0) return -5;
        q += w;
        n -= (size_t)w;
    }
    return 0;
}

/* $TMPDIR/agentc-XXXXXXXX-N.log, 0600, mirroring the bash tool's naming. */
static int open_spill(AgcBuf *path) {
    const char *tmpdir = os_getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == 0) tmpdir = "/tmp";
    static u32 counter;
    u64 rnd = 0;
    (void)os_random(&rnd, sizeof rnd);
    char buf[4096];
    int n = agentc_snprintf(buf, sizeof buf, "%s/agentc-%08llx-%u.log", tmpdir,
                            (unsigned long long)rnd, counter++);
    if (n <= 0 || (size_t)n >= sizeof buf) return JOB_ERANGE;
    int fd = os_open(buf, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, 0600);
    if (fd < 0) return fd;
    agentc_buf_cstr(path, buf);
    return fd;
}

int agentc_tool_append_output(AgcJob *j, const char *p, size_t n, size_t cap) {
    if (j == NULL) return JOB_EINVAL;
    if (cap == 0) cap = AGENTC_LIMIT_TOOL_BYTES;
    if (p == NULL || n == 0) return 0;
    j->total_bytes += n;

    if (j->spill_fd < 0 && j->out.len + n <= cap) {
        agentc_buf_push(&j->out, p, n);
        return 0;
    }
    if (j->spill_fd < 0) {
        int fd = open_spill(&j->spill_path);
        if (fd < 0) {
            /* No temp space: keep the capped head only, stay bounded. */
            if (j->out.len < cap) {
                size_t head = cap - j->out.len;
                if (head > n) head = n;
                agentc_buf_push(&j->out, p, head);
            }
            return 0;
        }
        j->spill_fd = fd;
        if (j->out.len > 0 && write_span(fd, j->out.p, j->out.len) != 0) {
            os_close(fd);
            j->spill_fd = -1;
            agentc_buf_clear(&j->spill_path);
            return 0;
        }
    }
    if (write_span(j->spill_fd, p, n) != 0) {
        os_close(j->spill_fd);
        j->spill_fd = -1;
        agentc_buf_clear(&j->spill_path);
        return 0;
    }
    size_t room = j->out.len < cap ? cap - j->out.len : 0;
    if (room > n) room = n;
    if (room > 0) agentc_buf_push(&j->out, p, room);
    return 0;
}

int agentc_tool_finalize_output(AgcJob *j) {
    if (j == NULL) return 0;
    size_t cap = AGENTC_LIMIT_TOOL_BYTES;
    bool truncated = false;

    if (j->spill_fd >= 0) {
        os_close(j->spill_fd);
        j->spill_fd = -1;
        truncated = true;
    }
    /* A tool that composed its own final text (bash sets spill_notified after
     * bash_finish) already applied the display cap and wrote its own notice:
     * the generic cap must not trim the status trailer or add a second notice. */
    if (j->spill_notified) return truncated ? 1 : 0;
    if (j->out.len > cap) {
        if (j->spill_path.len == 0) {
            int fd = open_spill(&j->spill_path);
            if (fd >= 0) {
                (void)write_span(fd, j->out.p, j->out.len);
                os_close(fd);
            }
        }
        size_t cut = cap;
        size_t nl = cut;
        while (nl > 0 && j->out.p[nl - 1] != '\n') nl--;
        if (nl > 0) cut = nl;   /* keep the last complete line */
        j->out.len = cut;
        if (j->out.p != NULL) j->out.p[cut] = 0;
        truncated = true;
    }
    if (truncated && !j->spill_notified) {
        if (j->spill_path.len > 0)
            agentc_buf_printf(&j->out, "[output truncated: full output: %s]\n",
                              (const char *)j->spill_path.p);
        else
            agentc_buf_cstr(&j->out, "[output truncated: full output could not be saved]\n");
    }
    return truncated ? 1 : 0;
}

/* ------------------------------------------------------------- execution */

/* v1 normalization: an exec-only tool becomes a run() that returns no negative
 * codes; its owned result is copied into the job buffer and freed here. */
static int run_via_exec(AgcJob *j, bool *is_error) {
    bool err = false;
    char *res = j->tool->exec(args_of(j), &err, j->call.cancel);
    if (res == NULL) {
        err = true;
        res = agentc_strdup("error: tool returned no result");
    }
    (void)agentc_tool_append_output(j, res, agentc_strlen(res), 0);
    agentc_free(res);
    *is_error = err;
    return 0;
}

/* Sync path. Classification: cancel keeps partial output, timeout and fatal
 * replace it, everything else is a normal result. */
static void run_sync(AgcJob *j) {
    bool is_error = false;
    int r;

    g_current = j;
    if (j->tool->run != NULL)
        r = j->tool->run(j->tool, &j->call, &j->out, &is_error);
    else
        r = run_via_exec(j, &is_error);
    g_current = NULL;

    if (r == JOB_ECANCELED) {
        job_cancel(j);
        return;
    }
    if (r == JOB_ETIMEDOUT ||
        (j->deadline_ns != 0 && os_now_ns(OS_CLOCK_MONOTONIC) >= j->deadline_ns)) {
        job_timeout(j);
        return;
    }
    if (r < 0) {
        job_fatal(j, r);
        return;
    }
    j->is_error = is_error;
    j->state = JOB_DONE;
}

static void run_async_to_done(AgcJob *j) {
    int r;

    if (j->tool->step == NULL) {
        job_fatal(j, JOB_EINVAL);
        return;
    }
    r = j->tool->start(j->tool, &j->call, j);
    if (r < 0) {
        job_fatal(j, r);
        return;
    }
    if (r == 1) {
        j->state = JOB_DONE;
        return;
    }
    j->state = JOB_RUNNING;
    for (;;) {
        struct os_pollfd pfd;
        int nfds = 0;

        r = j->tool->step(j->tool, j);
        if (r == 1) {
            j->state = JOB_DONE;
            return;
        }
        if (r < 0) {
            job_fatal(j, r);
            return;
        }
        if (job_aborted(j)) {
            job_cancel(j);
            return;
        }
        if (j->deadline_ns != 0 && os_now_ns(OS_CLOCK_MONOTONIC) >= j->deadline_ns) {
            job_timeout(j);
            return;
        }
        if (j->fd >= 0) {
            pfd.fd = j->fd;
            pfd.events = OS_POLLIN;
            pfd.revents = 0;
            nfds = 1;
        }
        os_poll(nfds ? &pfd : NULL, nfds, 20);
    }
}

static int run_sequential(size_t n, const char *const *pre_errors, AgcJobCb cb, void *ud) {
    for (size_t i = 0; i < n; i++) {
        AgcJob *j = &g_jobs[i];

        if (job_aborted(j)) {
            /* Answer the call without a start event, exactly like the old
             * backfill path in the agent loop. */
            job_cancel(j);
            job_finished(j, cb, ud);
            continue;
        }
        job_started(j, cb, ud);
        if (pre_errors != NULL && pre_errors[i] != NULL)
            job_pre_error(j, pre_errors[i]);
        else if (j->tool == NULL)
            job_unknown(j);
        else if (j->tool->run != NULL || j->tool->exec != NULL)
            run_sync(j);
        else if (j->tool->start != NULL)
            run_async_to_done(j);
        else
            job_no_runner(j);
        job_finished(j, cb, ud);
    }
    return 0;
}

static int run_parallel(size_t n, const char *const *pre_errors, AgcJobCb cb, void *ud) {
    /* The batch starts aborted: every call is answered without a start event. */
    if (jobs_aborted(n)) {
        for (size_t i = 0; i < n; i++) {
            job_cancel(&g_jobs[i]);
            job_finished(&g_jobs[i], cb, ud);
        }
        return 0;
    }

    /* Async jobs first, in source order, so their children overlap the sync
     * jobs that follow. */
    for (size_t i = 0; i < n; i++) {
        AgcJob *j = &g_jobs[i];
        int r;

        if (j->tool == NULL || j->tool->start == NULL) continue;
        /* A vetoed tool must not run; leave it for the source-ordered pass
         * below, which answers it without invoking start(). */
        if (pre_errors != NULL && pre_errors[i] != NULL) continue;
        job_started(j, cb, ud);
        if (j->tool->step == NULL) {
            job_fatal(j, JOB_EINVAL);
            job_finished(j, cb, ud);
            continue;
        }
        r = j->tool->start(j->tool, &j->call, j);
        if (r < 0) {
            job_fatal(j, r);
            job_finished(j, cb, ud);
        } else if (r == 1) {
            j->state = JOB_DONE;
            job_finished(j, cb, ud);
        } else {
            j->state = JOB_RUNNING;
        }
    }

    /* Then the sync jobs (and vetoed/unknown calls), in source order. */
    for (size_t i = 0; i < n; i++) {
        AgcJob *j = &g_jobs[i];

        if (j->state != JOB_FREE) continue;   /* an async job already ran it */
        if (job_aborted(j)) {
            job_cancel(j);
            job_finished(j, cb, ud);
            continue;
        }
        job_started(j, cb, ud);
        if (pre_errors != NULL && pre_errors[i] != NULL)
            job_pre_error(j, pre_errors[i]);
        else if (j->tool == NULL)
            job_unknown(j);
        else if (j->tool->run != NULL || j->tool->exec != NULL)
            run_sync(j);
        else
            job_no_runner(j);
        job_finished(j, cb, ud);
    }

    /* Drive the async jobs until none is running. */
    while (running_count(n) > 0) {
        struct os_pollfd fds[AGENTC_TOOL_JOBS_MAX];
        int nfds = 0;

        for (size_t i = 0; i < n; i++) {
            if (g_jobs[i].state == JOB_RUNNING && g_jobs[i].fd >= 0) {
                fds[nfds].fd = g_jobs[i].fd;
                fds[nfds].events = OS_POLLIN;
                fds[nfds].revents = 0;
                nfds++;
            }
        }
        os_poll(nfds ? fds : NULL, nfds, 20);

        if (jobs_aborted(n)) {
            for (size_t i = 0; i < n; i++) {
                if (g_jobs[i].state == JOB_RUNNING) {
                    job_cancel(&g_jobs[i]);
                    job_finished(&g_jobs[i], cb, ud);
                }
            }
            break;
        }
        for (size_t i = 0; i < n; i++) {
            AgcJob *j = &g_jobs[i];
            int r;

            if (j->state != JOB_RUNNING) continue;
            if (job_aborted(j)) {
                job_cancel(j);
                job_finished(j, cb, ud);
                continue;
            }
            r = j->tool->step(j->tool, j);
            if (r == 1) {
                j->state = JOB_DONE;
                job_finished(j, cb, ud);
            } else if (r < 0) {
                job_fatal(j, r);
                job_finished(j, cb, ud);
            } else if (j->deadline_ns != 0 &&
                       os_now_ns(OS_CLOCK_MONOTONIC) >= j->deadline_ns) {
                job_timeout(j);
                job_finished(j, cb, ud);
            }
        }
    }
    return 0;
}

/* --------------------------------------------------------------- entry */

int agentc_tool_jobs_run(const AgcTool *const *tools, const char *const *pre_errors,
                         const AgcToolCall *calls, size_t n,
                         AgcJobCb cb, void *ud) {
    if (n == 0) return 0;
    if (n > AGENTC_TOOL_JOBS_MAX) {
        /* The batch cannot run, but the provider needs an answer for every
         * call; answer each one before returning. */
        for (size_t i = 0; i < n; i++) {
            AgcJob j;
            agentc_tool_job_init(&j);
            j.tool = tools != NULL ? tools[i] : NULL;
            if (calls != NULL) j.call = calls[i];
            j.index = i;
            j.state = JOB_DONE;
            j.is_error = true;
            agentc_buf_cstr(&j.out, "error: too many tool calls\n");
            if (cb != NULL) (void)cb(ud, AGENTC_JOB_DONE, &j);
            agentc_buf_free(&j.out);
        }
        return JOB_ERANGE;
    }

    bool sequential = false;
    for (size_t i = 0; i < n; i++) {
        job_reset(&g_jobs[i], tools != NULL ? tools[i] : NULL,
                  calls != NULL ? &calls[i] : NULL, i);
        if (tools != NULL && tools[i] != NULL &&
            (tools[i]->flags & AGENTC_TOOL_SEQUENTIAL) != 0)
            sequential = true;
    }

    int r = sequential ? run_sequential(n, pre_errors, cb, ud)
                       : run_parallel(n, pre_errors, cb, ud);
    jobs_release(n);
    return r;
}
