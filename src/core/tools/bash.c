/* bash.c — the shell tool: the configured shell (POSIX /bin/sh -lc; Windows
 * cmd.exe or PowerShell) via os_spawn_shell, poll-based read loop,
 * cancel/timeout honored, truncated head + full output spilled to $TMPDIR.
 *
 * bash_start/bash_step are the v2 async pair driven by core/tools/jobs.c;
 * agentc_tool_bash is the synchronous wrapper and must keep producing the same
 * bytes it always has (the tools_test bash_* goldens depend on it).
 */
#include "agent.h"
#include "config.h"
#include "plat.h"
#include "base/limits.h"
#include "core/actor_int.h"
#include "core/tools/args.h"
#include "core/tools/jobs.h"

char *agentc_tool_read_err(bool *is_error, const char *fmt, ...);

#define BASH_MAX_MS 1800000 /* 30 min hard cap */

static int write_all_fd(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n) {
        int w = os_write(fd, q, n);
        if (w < 0) {
            if (w == -4)
                continue;
            return w;
        }
        if (w == 0)
            return -5;
        q += w;
        n -= (size_t)w;
    }
    return 0;
}

static int open_spill(AgcBuf *path) {
    const char *tmpdir = os_getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0])
        tmpdir = "/tmp";
    static u32 counter;
    u64 rnd = 0;
    (void)os_random(&rnd, sizeof rnd);
    char buf[4096];
    int n = agentc_snprintf(buf, sizeof buf, "%s/agentc-%08llx-%u.log", tmpdir,
                            (unsigned long long)rnd, counter++);
    if (n <= 0 || (size_t)n >= sizeof buf)
        return -36;
    int fd = os_open(buf, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC | OS_O_CLOEXEC, 0600);
    if (fd < 0)
        return fd;
    agentc_buf_cstr(path, buf);
    return fd;
}

/* Actionable message for -ENOENT from os_spawn_shell: which executable was
 * missing and how to fix it, per the configured kind. */
static char *bash_shell_missing(bool *is_error, const char *kind) {
    if (agentc_streq(kind, "cmd"))
        return agentc_tool_read_err(is_error,
                                    "error: bash: cmd.exe not found (looked for %%COMSPEC%% and "
                                    "%%SystemRoot%%\\System32\\cmd.exe)");
    if (agentc_streq(kind, "pwsh"))
        return agentc_tool_read_err(is_error,
                                    "error: bash: pwsh.exe (PowerShell 7) not found on PATH; "
                                    "install PowerShell 7 or set shell to \"powershell\"");
    if (agentc_streq(kind, "powershell"))
        return agentc_tool_read_err(is_error,
                                    "error: bash: PowerShell not found (looked for pwsh.exe on "
                                    "PATH and "
                                    "%%SystemRoot%%\\System32\\WindowsPowerShell\\v1.0\\"
                                    "powershell.exe); install PowerShell or set shell to "
                                    "\"cmd\"");
    return agentc_tool_read_err(is_error, "error: bash: /bin/sh not found");
}

/* Everything the loop needs besides the job-owned output/spill buffers. The
 * driver frees this through job->priv when the batch returns. */
typedef struct {
    i64 start_ns;
    i64 timeout_ms;
    u64 total_lines;
    u8 last;    /* last byte read; decides the trailing newline */
    int reaped; /* wait status from the non-blocking probe, or -99 */
    int read_err;
    bool direct; /* set by agentc_tool_bash: keep its exact old loop */
    bool started;
    bool eof;
    bool aborted;
    bool timed_out;
    bool background; /* return the actor id immediately */
    bool yielded;    /* hit yield_ms while still running */
    i64 yield_ms;
    bool done; /* finish already composed the result */
} BashState;

/* Release the process actor backing the job. Runs on the driver's fatal,
 * timeout, cancel and release sweeps and is idempotent: bash_finish clears
 * job->actor once it has released the actor itself. */
static void bash_cleanup(const AgcTool *self, AgcJob *job, int reason) {
    (void)self;
    (void)reason;
    if (job->actor != 0) {
        agentc_actor_release(job->actor);
        job->actor = 0;
        job->pid = -1;
        job->fd = -1;
    }
}

/* Validate, spawn and fill job->pid/fd. Returns 1 when the answer is already in
 * job->out (validation or spawn failure) and 0 when the job is running. */
static int bash_begin(const char *command, i64 timeout_ms, bool background, i64 yield_ms,
                      const AgcTrustEnv *env, AgcJob *job) {
    job->is_error = false;
    if (!command || !command[0]) {
        agentc_buf_cstr(&job->out, "error: bash: command is required");
        job->is_error = true;
        return 1;
    }

    /* Which shell to run is a platform question: POSIX only has /bin/sh, while
     * Windows resolves cmd/PowerShell from the config key (see plat.h). An
     * unknown kind fails here, before any process is spawned. */
    const char *kind = agentc_config_shell();
    const char *skind = os_shell_kind(kind);
    if (skind == NULL) {
        agentc_buf_printf(&job->out,
                          "error: bash: unknown shell '%s' (expected auto, cmd, "
                          "powershell or pwsh)",
                          kind && kind[0] ? kind : "auto");
        job->is_error = true;
        return 1;
    }

    i64 timeout = timeout_ms > 0 ? timeout_ms : BASH_MAX_MS;
    if (timeout > BASH_MAX_MS)
        timeout = BASH_MAX_MS;

    i64 start_ns = os_now_ns(OS_CLOCK_MONOTONIC);
    /* The child is a process actor (P6-A): the actor owns the pid and the read
     * end, and bash drains it. AGENTC_PROC_F_NO_SPILL keeps the actor from
     * opening a second spill file; the job's own spill holds the display head. */
    AgcProcSpawn spec;
    agentc_memset(&spec, 0, sizeof spec);
    spec.command = command;
    spec.shell = kind;
    spec.mode = AGENTC_PROC_PIPE;
    /* Match the job deadline's slack below: bash's own `elapsed > timeout`
     * check must win the race so the operator-facing `[timed out after Nms]`
     * text is composed instead of the actor's generic deadline cancel. */
    spec.deadline_ms = timeout + 50;
    spec.flags = AGENTC_PROC_F_NO_SPILL;
    spec.env = env;
    char aerr[256];
    AgcActorSpec aspec = {.proc = &spec, .agent = NULL};
    AgcActorId actor = agentc_actor_spawn(&aspec, aerr, sizeof aerr);
    if (actor == 0) {
        if (agentc_str_str(aerr, "spawn failed (-2)") != NULL) {
            bool unused = false;
            char *msg = bash_shell_missing(&unused, skind);
            agentc_buf_cstr(&job->out, msg ? msg : "");
            agentc_free(msg);
        } else if (aerr[0] != 0) {
            agentc_buf_printf(&job->out, "error: bash: %s", aerr);
        } else {
            agentc_buf_cstr(&job->out, "error: bash: spawn failed");
        }
        job->is_error = true;
        return 1;
    }
    AgcProcess *proc = agentc_actor_get(actor)->proc;

    BashState *st = agentc_alloc(sizeof *st); /* zeroed, aborts on OOM */
    st->start_ns = start_ns;
    st->timeout_ms = timeout;
    st->last = '\n';
    st->reaped = -99;
    job->priv = st;
    job->actor = actor;
    job->pid = proc->pid; /* group leader, for cleanup and the group kill */
    job->fd = -1;         /* the actor owns the read end */
    job->cleanup = bash_cleanup;
    /* Give bash's own timeout real slack against the driver's generic one: a
     * step that returns 0 has just checked `elapsed_ms > timeout`, and the
     * driver's `now >= deadline` runs a few instructions later (possibly
     * after a scheduling gap), so the deadline must sit beyond that check.
     * Bash then always emits its operator-facing `[timed out after Nms]`. */
    job->deadline_ns = start_ns + (timeout + 50) * 1000000;
    st->background = background;
    st->yield_ms = yield_ms;
    if (background) {
        /* Detach: the actor survives the batch and the step, so its id stays
         * valid for `process read`/`kill`. The job is already done. */
        agentc_buf_printf(&job->out, "{\"actor_id\":%u}", (unsigned)actor);
        job->actor = 0;
        job->cleanup = NULL;
        job->pid = -1;
        st->done = true;
        return 1;
    }
    return 0;
}

/* Post-loop result: kill/reap if needed, then compose the exact legacy text
 * (line-truncated head + status trailer + spill notice) into job->out. */
static void bash_finish(AgcJob *job) {
    BashState *st = job->priv;

    if (job->total_bytes && st->last != '\n')
        st->total_lines++;

    /* line-only truncation: spill the whole (small) output as well. A prior
     * spill failure is authoritative: never open a fresh spill that would hold
     * only the capped head yet be advertised as the full output. */
    bool spilled = job->spill_fd >= 0;
    if (!spilled && !job->spill_failed && st->total_lines > AGENTC_LIMIT_TOOL_LINES) {
        int fd = open_spill(&job->spill_path);
        if (fd >= 0 && write_all_fd(fd, job->out.p, job->out.len) == 0) {
            job->spill_fd = fd;
            spilled = true;
        } else {
            if (fd >= 0)
                os_close(fd);
            agentc_buf_clear(&job->spill_path);
            job->spill_failed = true;
        }
    }

    AgcProcInfo pi;
    agentc_process_info(job->actor, &pi);
    if (!st->yielded && (st->aborted || st->timed_out || st->read_err != 0) && pi.pid > 0) {
        /* Group SIGTERM, a bounded ~100 ms grace, then group SIGKILL so the
         * whole tree dies, not just the shell. Each probe advances the actor
         * (non-blocking os_wait inside a poll slice), so the actor remains the
         * one reaper and its exit_code is authoritative. */
        os_kill(-pi.pid, 15);
        for (int i = 0; i < 20; i++) {
            agentc_process_poll(job->actor);
            agentc_process_info(job->actor, &pi);
            if (pi.reaped)
                break;
            os_poll(NULL, 0, 5);
        }
        os_kill(-pi.pid, 9);
    }
    if (!st->yielded && !pi.reaped && pi.pid > 0) {
        /* Bounded reap: SIGKILL is immediate unless the child is stuck in
         * uninterruptible sleep; never block the loop on it. */
        for (int i = 0; i < 200; i++) {
            agentc_process_poll(job->actor);
            agentc_process_info(job->actor, &pi);
            if (pi.reaped)
                break;
            os_poll(NULL, 0, 5);
        }
    }
    i64 dur_ms = (os_now_ns(OS_CLOCK_MONOTONIC) - st->start_ns) / 1000000;
    int exit_code = pi.reaped ? pi.exit_code : 0;

    if (job->spill_fd >= 0) {
        os_close(job->spill_fd);
        job->spill_fd = -1;
    }
    AgcActorId run_id = job->actor;
    if (st->yielded) {
        /* Keep the actor alive for a later `process read|write|kill`; the
         * driver must not release it on the batch sweep. */
        job->cleanup = NULL;
        job->actor = 0;
        job->pid = -1;
    } else {
        if (job->actor != 0)
            agentc_actor_release(job->actor);
        job->actor = 0;
        job->pid = -1;
    }

    /* truncate the displayed head at 2000 lines / 50 KB, line-aligned */
    size_t cut = 0;
    u64 show_lines = 0;
    {
        size_t pos = 0;
        while (pos < job->out.len && show_lines < AGENTC_LIMIT_TOOL_LINES &&
               cut < AGENTC_LIMIT_TOOL_BYTES) {
            while (pos < job->out.len && job->out.p[pos] != '\n')
                pos++;
            if (pos < job->out.len)
                pos++;
            show_lines++;
            cut = pos;
        }
    }
    bool truncated =
        cut < job->out.len || show_lines < st->total_lines || spilled || job->spill_failed;

    AgcBuf res = {0};
    if (cut)
        agentc_buf_push(&res, job->out.p, cut);
    if (res.len && res.p[res.len - 1] != '\n')
        agentc_buf_byte(&res, '\n');
    if (st->yielded) {
        agentc_buf_printf(&res, "[running: actor_id %u]\n", (unsigned)run_id);
    } else if (st->aborted) {
        agentc_buf_cstr(&res, "[aborted]\n");
        job->is_error = true;
    } else if (st->timed_out) {
        agentc_buf_printf(&res, "[timed out after %lldms]\n", (long long)st->timeout_ms);
        job->is_error = true;
    } else if (st->read_err != 0) {
        agentc_buf_printf(&res, "[read error: %d]\n", st->read_err);
        job->is_error = true;
    } else {
        agentc_buf_printf(&res, "[exit code: %d, %lldms]\n", exit_code, (long long)dur_ms);
        if (exit_code != 0)
            job->is_error = true;
    }
    if (truncated && spilled) {
        agentc_buf_printf(&res, "[output truncated: showing %llu of %llu lines; full output: %s]\n",
                          (unsigned long long)show_lines, (unsigned long long)st->total_lines,
                          job->spill_path.p ? (const char *)job->spill_path.p : "");
    } else if (truncated) {
        agentc_buf_printf(&res,
                          "[output truncated: showing %llu of %llu lines; full output could "
                          "not be saved]\n",
                          (unsigned long long)show_lines, (unsigned long long)st->total_lines);
    }

    agentc_buf_free(&job->out);
    job->out = res;
    job->spill_notified = true; /* bash wrote its own operator-facing notice */
}

/* One loop iteration; 0 = keep stepping, 1 = finished (job->out is the answer). */
static int bash_step_once(AgcJob *job) {
    BashState *st = job->priv;
    if (st == NULL || st->done)
        return 1;

    AgcProcInfo pi;
    agentc_process_info(job->actor, &pi);

    bool finish = (pi.eof && pi.reaped) || st->aborted || st->timed_out || st->read_err != 0;
    if (!finish && job->call.cancel != NULL && *job->call.cancel) {
        st->aborted = true;
        finish = true;
    }
    /* The synchronous wrapper keeps the legacy check-before-I/O order. A driver
     * step is non-blocking: run_async_to_done() calls step() before its first
     * poll, so a step that finds no data returns at once and the driver sleeps
     * in its own 20 ms poll before the next step. The timeout check below runs
     * from the second step on (st->started) and on every direct-wrapper step. */
    if (!finish && (st->direct || st->started)) {
        i64 elapsed = (os_now_ns(OS_CLOCK_MONOTONIC) - st->start_ns) / 1000000;
        if (elapsed > st->timeout_ms) {
            st->timed_out = true;
            finish = true;
        }
    }

    if (!finish) {
        agentc_pump(20); /* the pump ticks process actors; let the UI repaint */
        agentc_process_poll(job->actor);
        /* Drain everything the actor buffered. A backgrounded grandchild
         * inherits the pipe write end and can hold it open after the direct
         * child exits, so once the direct child is reaped and nothing more is
         * buffered, stop (the legacy rule). */
        AgcBuf chunk = {0};
        size_t drained = 0;
        for (;;) {
            size_t n = agentc_process_read(job->actor, &chunk, 0);
            if (n == 0)
                break;
            drained += n;
            for (size_t i = 0; i < n; i++) {
                if (chunk.p[i] == '\n')
                    st->total_lines++;
                st->last = chunk.p[i];
            }
            (void)agentc_tool_append_output(job, (const char *)chunk.p, n, 0);
            agentc_buf_clear(&chunk);
        }
        agentc_buf_free(&chunk);

        agentc_process_info(job->actor, &pi);
        st->eof = pi.eof;
        if (pi.reaped && !pi.eof && drained == 0)
            finish = true;
        if (pi.eof && pi.reaped)
            finish = true;
    }

    /* Checks run once per iteration, after the I/O: a step that reports
     * "running" has just verified both flags, so the driver's generic cancel
     * and timeout (deadline set with slack by bash_start) cannot pre-empt
     * bash's own [aborted] / [timed out after Nms] text. */
    if (!finish && job->call.cancel != NULL && *job->call.cancel) {
        st->aborted = true;
        finish = true;
    }
    if (!finish) {
        i64 elapsed = (os_now_ns(OS_CLOCK_MONOTONIC) - st->start_ns) / 1000000;
        if (elapsed > st->timeout_ms) {
            st->timed_out = true;
            finish = true;
        }
    }
    if (!finish && st->yield_ms > 0 && (st->direct || st->started)) {
        i64 elapsed = (os_now_ns(OS_CLOCK_MONOTONIC) - st->start_ns) / 1000000;
        if (elapsed >= st->yield_ms) {
            st->yielded = true;
            finish = true;
        }
    }

    st->started = true;
    if (!finish)
        return 0;
    bash_finish(job);
    st->done = true;
    return 1;
}

/* ---- v2 interface ------------------------------------------------------ */

int bash_start(const AgcTool *self, const AgcToolCall *call, AgcJob *job) {
    (void)self;
    AgcToolArgs a;
    if (agentc_tool_args_parse(&a, call->args_json,
                               call->args_json ? agentc_strlen(call->args_json) : 0) != 0)
        return -12; /* ENOMEM */
    if (a.root == NULL) {
        agentc_buf_cstr(&job->out, "error: bash: arguments must be a JSON object");
        job->is_error = true;
        agentc_tool_args_free(&a);
        return 1;
    }
    const char *command = agentc_tool_args_str(&a, "command");
    if (command == NULL) {
        agentc_tool_args_missing(&job->out, "bash", "command", &job->is_error);
        agentc_tool_args_free(&a);
        return 1;
    }
    AgcJson *tv = agentc_json_get(a.root, "timeout");
    if (tv && agentc_json_type(tv) != AGENTC_JSON_NUM) {
        agentc_buf_cstr(&job->out, "error: bash: timeout must be a number");
        job->is_error = true;
        agentc_tool_args_free(&a);
        return 1;
    }
    i64 timeout = agentc_tool_args_int(&a, "timeout", 0);
    AgcJson *yv = agentc_json_get(a.root, "yield_ms");
    if (yv && agentc_json_type(yv) != AGENTC_JSON_NUM) {
        agentc_buf_cstr(&job->out, "error: bash: yield_ms must be a number");
        job->is_error = true;
        agentc_tool_args_free(&a);
        return 1;
    }
    bool background = agentc_tool_args_bool(&a, "background", false);
    i64 yield_ms = agentc_tool_args_int(&a, "yield_ms", 0);
    int r = bash_begin(command, timeout, background, yield_ms, call->env, job);
    agentc_tool_args_free(&a);
    return r;
}

int bash_step(const AgcTool *self, AgcJob *job) {
    (void)self;
    return bash_step_once(job);
}

char *agentc_tool_bash(const char *command, i64 timeout_ms, const volatile bool *cancel,
                       bool *is_error) {
    AgcJob job;
    agentc_tool_job_init(&job);
    job.call.cancel = cancel;

    int r = bash_begin(command, timeout_ms, false, 0, NULL, &job);
    if (job.priv != NULL)
        ((BashState *)job.priv)->direct = true;
    while (r == 0)
        r = bash_step_once(&job);

    if (is_error)
        *is_error = job.is_error;
    if (r < 0) {
        if (is_error)
            *is_error = true;
        agentc_buf_clear(&job.out);
        agentc_buf_cstr(&job.out, "error: bash: failed to run");
    }

    char *res = NULL;
    if (job.out.p) {
        res = (char *)job.out.p;
        job.out.p = NULL;
        job.out.len = 0;
        job.out.cap = 0;
    } else {
        res = agentc_strdup_len("", 0);
    }
    agentc_free(job.priv);
    if (job.actor != 0)
        agentc_actor_release(job.actor);
    if (job.fd >= 0)
        os_close(job.fd);
    if (job.spill_fd >= 0)
        os_close(job.spill_fd);
    agentc_buf_free(&job.spill_path);
    agentc_buf_free(&job.out);
    return res;
}
