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
#include "core/tools/args.h"
#include "core/tools/jobs.h"

char *agentc_tool_read_err(bool *is_error, const char *fmt, ...);

#define BASH_MAX_MS 1800000        /* 30 min hard cap */

static int write_all_fd(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n) {
        int w = os_write(fd, q, n);
        if (w < 0) {
            if (w == -4) continue;
            return w;
        }
        if (w == 0) return -5;
        q += w;
        n -= (size_t)w;
    }
    return 0;
}

static int open_spill(AgcBuf *path) {
    const char *tmpdir = os_getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
    static u32 counter;
    u64 rnd = 0;
    (void)os_random(&rnd, sizeof rnd);
    char buf[4096];
    int n = agentc_snprintf(buf, sizeof buf, "%s/agentc-%08llx-%u.log", tmpdir,
                        (unsigned long long)rnd, counter++);
    if (n <= 0 || (size_t)n >= sizeof buf) return -36;
    int fd = os_open(buf, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC | OS_O_CLOEXEC, 0600);
    if (fd < 0) return fd;
    agentc_buf_cstr(path, buf);
    return fd;
}

/* Actionable message for -ENOENT from os_spawn_shell: which executable was
 * missing and how to fix it, per the configured kind. */
static char *bash_shell_missing(bool *is_error, const char *kind) {
    if (agentc_streq(kind, "cmd"))
        return agentc_tool_read_err(
            is_error, "error: bash: cmd.exe not found (looked for %%COMSPEC%% and "
                      "%%SystemRoot%%\\System32\\cmd.exe)");
    if (agentc_streq(kind, "pwsh"))
        return agentc_tool_read_err(
            is_error, "error: bash: pwsh.exe (PowerShell 7) not found on PATH; "
                      "install PowerShell 7 or set shell to \"powershell\"");
    if (agentc_streq(kind, "powershell"))
        return agentc_tool_read_err(
            is_error, "error: bash: PowerShell not found (looked for pwsh.exe on "
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
    u8 last;              /* last byte read; decides the trailing newline */
    int reaped;           /* wait status from the non-blocking probe, or -99 */
    int read_err;
    bool direct;          /* set by agentc_tool_bash: keep its exact old loop */
    bool started;
    bool eof;
    bool aborted;
    bool timed_out;
    bool done;            /* finish already composed the result */
} BashState;

/* Validate, spawn and fill job->pid/fd. Returns 1 when the answer is already in
 * job->out (validation or spawn failure) and 0 when the job is running. */
static int bash_begin(const char *command, i64 timeout_ms, AgcJob *job) {
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
    if (timeout > BASH_MAX_MS) timeout = BASH_MAX_MS;

    int fds[2];
    int pr = os_pipe(fds);
    if (pr < 0) {
        agentc_buf_printf(&job->out, "error: bash: pipe failed (errno %d)", pr);
        job->is_error = true;
        return 1;
    }
    int devnull = os_open("/dev/null", OS_O_RDONLY | OS_O_CLOEXEC, 0);
    i64 start_ns = os_now_ns(OS_CLOCK_MONOTONIC);
    int pid = os_spawn_shell_group(kind, command, devnull, fds[1], fds[1]);
    if (devnull >= 0) os_close(devnull);
    os_close(fds[1]);
    if (pid < 0) {
        os_close(fds[0]);
        if (pid == -2) {
            bool unused = false;
            char *msg = bash_shell_missing(&unused, skind);
            agentc_buf_cstr(&job->out, msg ? msg : "");
            agentc_free(msg);
        } else {
            agentc_buf_printf(&job->out, "error: bash: spawn failed (errno %d)", pid);
        }
        job->is_error = true;
        return 1;
    }

    BashState *st = agentc_alloc(sizeof *st);   /* zeroed, aborts on OOM */
    st->start_ns = start_ns;
    st->timeout_ms = timeout;
    st->last = '\n';
    st->reaped = -99;
    job->priv = st;
    job->pid = pid;
    job->fd = fds[0];
    /* Give bash's own timeout real slack against the driver's generic one: a
     * step that returns 0 has just checked `elapsed_ms > timeout`, and the
     * driver's `now >= deadline` runs a few instructions later (possibly
     * after a scheduling gap), so the deadline must sit beyond that check.
     * Bash then always emits its operator-facing `[timed out after Nms]`. */
    job->deadline_ns = start_ns + (timeout + 50) * 1000000;
    return 0;
}

/* Post-loop result: kill/reap if needed, then compose the exact legacy text
 * (line-truncated head + status trailer + spill notice) into job->out. */
static void bash_finish(AgcJob *job) {
    BashState *st = job->priv;

    if (job->total_bytes && st->last != '\n') st->total_lines++;

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
            if (fd >= 0) os_close(fd);
            agentc_buf_clear(&job->spill_path);
            job->spill_failed = true;
        }
    }

    if (st->aborted || st->timed_out || st->read_err != 0) {
        if (job->pid > 0) {
            /* Group SIGTERM, a bounded ~100 ms grace, then group SIGKILL so
             * the whole tree dies, not just the shell. Every wait is the
             * non-blocking form inside a poll slice. */
            if (st->reaped == -99) {
                os_kill(-job->pid, 15);
                for (int i = 0; i < 20; i++) {
                    int w = os_wait(job->pid, true);
                    if (w != -1) {
                        st->reaped = w;
                        break;
                    }
                    os_poll(NULL, 0, 5);
                }
            }
            os_kill(-job->pid, 9);
        }
    }
    int status = -1;
    if (st->reaped != -99) {
        status = st->reaped;
    } else if (job->pid > 0) {
        /* Bounded reap: SIGKILL is immediate unless the child is stuck in
         * uninterruptible sleep; never block the loop on it. */
        for (int i = 0; i < 200; i++) {
            int w = os_wait(job->pid, true);
            if (w != -1) {
                status = w;
                break;
            }
            os_poll(NULL, 0, 5);
        }
    }
    i64 dur_ms = (os_now_ns(OS_CLOCK_MONOTONIC) - st->start_ns) / 1000000;

    if (job->fd >= 0) {
        os_close(job->fd);
        job->fd = -1;
    }
    if (job->spill_fd >= 0) {
        os_close(job->spill_fd);
        job->spill_fd = -1;
    }
    job->pid = -1;

    int exit_code;
    if ((status & 0x7f) == 0)
        exit_code = (status >> 8) & 0xff;
    else
        exit_code = 128 + (status & 0x7f);

    /* truncate the displayed head at 2000 lines / 50 KB, line-aligned */
    size_t cut = 0;
    u64 show_lines = 0;
    {
        size_t pos = 0;
        while (pos < job->out.len && show_lines < AGENTC_LIMIT_TOOL_LINES &&
               cut < AGENTC_LIMIT_TOOL_BYTES) {
            while (pos < job->out.len && job->out.p[pos] != '\n') pos++;
            if (pos < job->out.len) pos++;
            show_lines++;
            cut = pos;
        }
    }
    bool truncated = cut < job->out.len || show_lines < st->total_lines || spilled ||
                     job->spill_failed;

    AgcBuf res = { 0 };
    if (cut) agentc_buf_push(&res, job->out.p, cut);
    if (res.len && res.p[res.len - 1] != '\n') agentc_buf_byte(&res, '\n');
    if (st->aborted) {
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
        if (exit_code != 0) job->is_error = true;
    }
    if (truncated && spilled) {
        agentc_buf_printf(&res,
                          "[output truncated: showing %llu of %llu lines; full output: %s]\n",
                          (unsigned long long)show_lines,
                          (unsigned long long)st->total_lines,
                          job->spill_path.p ? (const char *)job->spill_path.p : "");
    } else if (truncated) {
        agentc_buf_printf(&res,
                          "[output truncated: showing %llu of %llu lines; full output could "
                          "not be saved]\n",
                          (unsigned long long)show_lines,
                          (unsigned long long)st->total_lines);
    }

    agentc_buf_free(&job->out);
    job->out = res;
    job->spill_notified = true;   /* bash wrote its own operator-facing notice */
}

/* One loop iteration; 0 = keep stepping, 1 = finished (job->out is the answer). */
static int bash_step_once(AgcJob *job) {
    BashState *st = job->priv;
    if (st == NULL || st->done) return 1;

    bool finish = (st->eof && st->reaped != -99) || st->aborted || st->timed_out ||
                  st->read_err != 0;
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
        u8 tmp[65536];
        agentc_pump(20); /* let an interactive front end repaint and read input */
        /* A background child (server &, nohup ...) inherits the write end and
         * would hold the pipe open forever. Once the direct child exits, drain
         * what is buffered and stop. */
        int w = os_wait(job->pid, true);
        bool child_exited = w != -1;
        if (child_exited && st->reaped == -99) st->reaped = w;   /* keep status */
        if (job->fd < 0) {
            /* EOF while the child is still alive (`exec 1>&-; sleep ...`):
             * keep polling with a fixed bounded slice and let the deadline
             * check below fire; never fall into a blocking reap. The direct
             * wrapper owns its pacing; a driver step relies on the driver's
             * single 20 ms poll for the wait instead of sleeping here. */
            if (st->direct) os_poll(NULL, 0, 20);
            if (st->eof && st->reaped != -99) finish = true;
        } else {
            struct os_pollfd pfd = { job->fd, OS_POLLIN, 0 };
            /* A driver step must not block: the driver has already polled
             * this fd, so take what is ready and return at once when the pipe
             * is still empty. The synchronous wrapper keeps its own 20 ms
             * wait so its legacy check-before-I/O order is unchanged. */
            int p = os_poll(&pfd, 1, st->direct ? 20 : 0);
            if (p < 0) {
                if (p != -4 /* EINTR */) {
                    st->read_err = p;
                    finish = true;
                }
            } else if (p == 0) {
                if (child_exited) {
                    st->eof = true;
                    finish = true;
                }
            } else {
                int n = os_read(job->fd, tmp, sizeof tmp);
                if (n > 0) {
                    for (int i = 0; i < n; i++) {
                        if (tmp[i] == '\n') st->total_lines++;
                        st->last = tmp[i];
                    }
                    (void)agentc_tool_append_output(job, (const char *)tmp, (size_t)n, 0);
                } else if (n == 0) {
                    os_close(job->fd);
                    job->fd = -1;
                    st->eof = true;
                } else if (n != -11 /* EAGAIN */) {
                    st->read_err = n;
                }
                if (child_exited && (n == 0 || n == -11)) st->eof = true;
                /* EOF only ends the loop once the child has exited too. */
                if (st->read_err != 0 || (st->eof && st->reaped != -99)) finish = true;
            }
        }
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

    st->started = true;
    if (!finish) return 0;
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
    int r = bash_begin(command, timeout, job);
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

    int r = bash_begin(command, timeout_ms, &job);
    if (job.priv != NULL) ((BashState *)job.priv)->direct = true;
    while (r == 0) r = bash_step_once(&job);

    if (is_error) *is_error = job.is_error;
    if (r < 0) {
        if (is_error) *is_error = true;
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
    if (job.fd >= 0) os_close(job.fd);
    if (job.spill_fd >= 0) os_close(job.spill_fd);
    agentc_buf_free(&job.spill_path);
    agentc_buf_free(&job.out);
    return res;
}
