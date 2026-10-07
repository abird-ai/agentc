/* process.c — process actors: spawn, pipe/PTY I/O, output ring + spill,
 * cancellation escalation and freeze (P6-A; see plans/P6-ACTORS.md).
 *
 * One process actor is a registered child OS process. Pipe mode merges
 * stdout+stderr and has no stdin; PTY mode gives a writable master used for
 * both input and output. Output is appended to three sinks: `pending` (drained
 * by a reader, bounded by AGENTC_PROC_READ_MAX), `tail` (the last
 * AGENTC_PROC_TAIL_BYTES for UI/list) and, once the stream exceeds the tail, a
 * 0600 spill file holding the full stream.
 */
#include "core/actor_int.h"

#include "base/limits.h"
#include "core/events.h"
#include "plat.h"
#include "trust.h"

#define PROC_EAGAIN (-11)
#define PROC_EINTR (-4)
#define PROC_EIO (-5)

static int write_all_fd(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n > 0) {
        int w = os_write(fd, q, n);
        if (w < 0) {
            if (w == PROC_EINTR)
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

/* $TMPDIR/agentc-proc-XXXXXXXX-N.log, 0600, mirroring the bash tool's naming. */
static int open_spill(AgcBuf *path) {
    const char *tmpdir = os_getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == 0)
        tmpdir = "/tmp";
    static u32 counter;
    u64 rnd = 0;
    (void)os_random(&rnd, sizeof rnd);
    char buf[4096];
    int n = agentc_snprintf(buf, sizeof buf, "%s/agentc-proc-%08llx-%u.log", tmpdir,
                            (unsigned long long)rnd, counter++);
    if (n <= 0 || (size_t)n >= sizeof buf)
        return -36;
    int fd = os_open(buf, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC | OS_O_CLOEXEC, 0600);
    if (fd < 0)
        return fd;
    agentc_buf_cstr(path, buf);
    return fd;
}

/* Keep the newest `cap` bytes in b (drop the oldest on overflow). Used for the
 * tail ring and the bounded pending reader buffer. */
static void buf_append_capped(AgcBuf *b, const u8 *p, size_t n, size_t cap) {
    if (cap == 0 || n == 0)
        return;
    if (n >= cap) {
        agentc_buf_clear(b);
        agentc_buf_push(b, p + (n - cap), cap);
        return;
    }
    if (b->len + n > cap) {
        size_t drop = b->len + n - cap;
        if (drop >= b->len) {
            agentc_buf_clear(b);
        } else {
            agentc_memmove(b->p, b->p + drop, b->len - drop);
            b->len -= drop;
        }
    }
    agentc_buf_push(b, p, n);
}

/* Append one output chunk to the actor's sinks. */
static void process_output(AgcActor *a, const u8 *p, size_t n) {
    AgcProcess *pr = a->proc;
    if (n == 0)
        return;
    u64 before = pr->total_bytes;

    pr->total_bytes += n;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n')
            pr->total_lines++;
        pr->last = p[i];
    }

    if (pr->spill_fd < 0 && !pr->no_spill && (before + n) > AGENTC_PROC_TAIL_BYTES) {
        int fd = open_spill(&pr->spill_path);
        if (fd >= 0) {
            pr->spill_fd = fd;
            /* `tail` still holds every byte seen so far (it only overflows once
             * the stream exceeds the tail), so this preserves the full stream. */
            if (pr->tail.len > 0 && write_all_fd(fd, pr->tail.p, pr->tail.len) != 0) {
                os_close(fd);
                pr->spill_fd = -1;
                agentc_buf_clear(&pr->spill_path);
                pr->spill_failed = true;
            }
        } else {
            pr->spill_failed = true;
        }
    }
    if (pr->spill_fd >= 0 && write_all_fd(pr->spill_fd, p, n) != 0) {
        os_close(pr->spill_fd);
        pr->spill_fd = -1;
        agentc_buf_clear(&pr->spill_path);
        pr->spill_failed = true;
    }

    buf_append_capped(&pr->tail, p, n, pr->tail_cap);
    buf_append_capped(&pr->pending, p, n, AGENTC_PROC_READ_MAX);
}

bool process_actor_terminal(const AgcActor *a) {
    return a->state == AGENTC_ACTOR_SETTLED || a->state == AGENTC_ACTOR_FAILED ||
           a->state == AGENTC_ACTOR_CANCELLED;
}

/* Drain the read fd (until EAGAIN), reap the child, and settle the state. */
void process_actor_poll(AgcActor *a) {
    AgcProcess *p = a->proc;
    if (p == NULL || process_actor_terminal(a))
        return;

    if (a->state == AGENTC_ACTOR_FROZEN || a->frozen_ns != 0)
        return;

    if (a->state == AGENTC_ACTOR_STARTING)
        a->state = AGENTC_ACTOR_RUNNING;

    if (!p->eof && p->fd_out >= 0) {
        u8 tmp[65536];
        bool got = false;
        for (;;) {
            /* Defensive readability probe: a blocking backend fd (a PTY master
             * without O_NONBLOCK, say) must never wedge the single-threaded
             * tick. A non-blocking fd pays only one extra poll. */
            struct os_pollfd pfd = {p->fd_out, OS_POLLIN, 0};
            if (os_poll(&pfd, 1, 0) <= 0)
                break;
            int n = os_read(p->fd_out, tmp, sizeof tmp);
            if (n > 0) {
                process_output(a, tmp, (size_t)n);
                got = true;
                continue;
            }
            if (n == 0 || (p->tty && n == PROC_EIO)) {
                os_close(p->fd_out);
                p->fd_out = -1;
                if (p->tty)
                    p->fd_in = -1; /* master is a single fd */
                p->eof = true;
                break;
            }
            if (n == PROC_EAGAIN || n == PROC_EINTR)
                break;
            p->read_err = n;
            break;
        }
        if (got) {
            actor_mark_progress(a);
            actor_event(a, AGENTC_EV_ACTOR_PROGRESS, NULL, NULL, 0);
        }
    }

    if (!p->reaped && p->pid > 0) {
        int w = os_wait(p->pid, true);
        if (w != -1) {
            p->reaped = true;
            p->wait_status = w;
            if ((w & 0x7f) == 0)
                p->exit_code = (w >> 8) & 0xff;
            else
                p->exit_code = 128 + (w & 0x7f);
            if (p->fd_in >= 0 && !p->tty) {
                os_close(p->fd_in);
                p->fd_in = -1;
                p->stdin_open = false;
            }
        }
    }

    if (p->read_err != 0) {
        char reason[64];
        agentc_snprintf(reason, sizeof reason, "{\"read_err\":%d}", p->read_err);
        actor_finish_state(a, AGENTC_ACTOR_FAILED, reason);
        return;
    }
    if (p->reaped && (p->eof || p->fd_out < 0)) {
        if (a->cancel_stage != 0) {
            char jbuf[32];
            agentc_snprintf(jbuf, sizeof jbuf, "{\"stage\":%d}", a->cancel_stage);
            actor_finish_state(a, AGENTC_ACTOR_CANCELLED, jbuf);
        } else {
            actor_finish_state(a, AGENTC_ACTOR_SETTLED, NULL);
        }
    }
}

/* --------------------------------------------------------------- spawn */

static const char *spec_label(const AgcProcSpawn *spec, const char *cmd) {
    if (spec->label != NULL && spec->label[0] != 0)
        return spec->label;
    if (cmd != NULL && cmd[0] != 0)
        return cmd;
    if (spec->argv != NULL && spec->argv[0] != NULL)
        return spec->argv[0];
    return "process";
}

AgcActorId process_actor_spawn(AgcActorId parent, const AgcProcSpawn *spec, char *err, size_t cap) {
    if (err != NULL && cap > 0)
        err[0] = 0;
    if (spec == NULL || (spec->command == NULL && spec->argv == NULL)) {
        if (err != NULL && cap > 0)
            agentc_snprintf(err, cap, "process: command is required");
        return 0;
    }

    const char *skind = NULL;
    if (spec->command != NULL) {
        skind = os_shell_kind(spec->shell);
        if (skind == NULL) {
            if (err != NULL && cap > 0)
                agentc_snprintf(err, cap, "process: unknown shell '%s'",
                                spec->shell != NULL ? spec->shell : "auto");
            return 0;
        }
    }

    /* P6-C/P6 completion: every spawn carries the owning agent's envelope. A
     * per-call env wins; otherwise fall back to the ambient envelope so a
     * direct/RPC spawn is confined exactly like an agent-driven one. NULL means
     * unconfined (legacy callers with no active agent). A non-none boundary is
     * applied in the child and a failure denies the spawn. */
    const struct AgcTrustEnv *env = spec->env ? spec->env : agentc_trust_current();
    AgcSandboxSpec sb;
    const AgcSandboxSpec *sbsp = NULL;
    if (env != NULL) {
        (void)agentc_trust_sandbox_spec(env, &sb);
        sbsp = &sb;
    }

    int out = -1, slave = -1;
    bool tty = spec->mode == AGENTC_PROC_PTY;
    if (tty) {
        int m = -1, s = -1;
        int r = os_pty_open(&m, &s, spec->cols, spec->rows);
        if (r < 0) {
            if (err != NULL && cap > 0)
                agentc_snprintf(err, cap, "process: tty unavailable (%d)", r);
            return 0;
        }
        out = m;
        slave = s;
    } else {
        int fds[2];
        int r = os_pipe(fds);
        if (r < 0) {
            if (err != NULL && cap > 0)
                agentc_snprintf(err, cap, "process: pipe failed (%d)", r);
            return 0;
        }
        out = fds[0];
        int devnull = os_open("/dev/null", OS_O_RDONLY | OS_O_CLOEXEC, 0);
        /* P6-E: BIDI is a pipe with a writable stdin pipe (voice/MCP stdio).
         * The reserved STDIN_PIPE flag maps onto the same path. */
        bool bidi =
            spec->mode == AGENTC_PROC_PIPE_BIDI || (spec->flags & AGENTC_PROC_F_STDIN_PIPE) != 0;
        int inpipe[2] = {-1, -1};
        int child_stdin = devnull;
        if (bidi) {
            if (os_pipe(inpipe) < 0) {
                os_close(out);
                if (devnull >= 0)
                    os_close(devnull);
                if (err != NULL && cap > 0)
                    agentc_snprintf(err, cap, "process: bidi stdin pipe failed");
                return 0;
            }
            child_stdin = inpipe[0];
        }
        int pid;
        if (skind != NULL)
            pid = os_spawn_shell_group_sandboxed(skind, spec->command, child_stdin, fds[1], fds[1],
                                                 sbsp, err, cap);
        else
            pid = os_spawn_group_sandboxed(spec->argv, NULL, spec->cwd, child_stdin, fds[1], fds[1],
                                           0, sbsp, err, cap);
        if (devnull >= 0)
            os_close(devnull);
        if (bidi && inpipe[0] >= 0)
            os_close(inpipe[0]);
        os_close(fds[1]);
        if (pid < 0) {
            if (bidi && inpipe[1] >= 0)
                os_close(inpipe[1]);
            if (sbsp != NULL && env != NULL && env->boundary != AGENTC_BOUND_NONE) {
                AgcActorEvent ev;
                agentc_memset(&ev, 0, sizeof ev);
                ev.text = (err != NULL && err[0]) ? err : "sandbox denied";
                agentc_events_emit(NULL, AGENTC_EV_SANDBOX_DENIED, &ev);
            }
            os_close(out);
            if (err != NULL && cap > 0 && err[0] == 0)
                agentc_snprintf(err, cap, "process: spawn failed (%d)", pid);
            return 0;
        }
        AgcActorId id =
            agentc_actor_register(AGENTC_ACTOR_PROCESS, parent, spec_label(spec, spec->command));
        if (id == 0) {
            os_kill(-pid, 9);
            os_close(out);
            if (err != NULL && cap > 0)
                agentc_snprintf(err, cap, "process: actor table full (%d)", AGENTC_ACTOR_MAX);
            return 0;
        }
        AgcActor *a = agentc_actor_get(id);
        AgcProcess *p = agentc_alloc(sizeof *p); /* zeroed */
        agentc_memset(p, 0, sizeof *p);
        p->pid = pid;
        p->fd_out = out;
        p->fd_in = -1;
        p->fd_err = -1;
        p->tty = false;
        p->stdin_open = false;
        if (bidi) {
            p->fd_in = inpipe[1];
            p->stdin_open = true;
        }
        p->last = '\n';
        p->wait_status = -99;
        p->spill_fd = -1;
        p->no_spill = (spec->flags & AGENTC_PROC_F_NO_SPILL) != 0;
        p->tail_cap = AGENTC_PROC_TAIL_BYTES;
        p->cmd = agentc_strdup(
            spec->command != NULL ? spec->command : (spec->argv[0] != NULL ? spec->argv[0] : ""));
        p->cwd = spec->cwd != NULL ? agentc_strdup(spec->cwd) : NULL;
        a->proc = p;
        a->state = AGENTC_ACTOR_RUNNING;
        a->grace_ms = 0;
        i64 deadline = spec->deadline_ms > 0 ? spec->deadline_ms : AGENTC_PROC_DEADLINE_DEF_MS;
        a->deadline_ns = deadline > 0 ? actor_now() + deadline * 1000000 : 0;
        if (spec->idle_ms > 0)
            a->idle_ms = spec->idle_ms;
        actor_event(a, AGENTC_EV_ACTOR_STARTED, NULL, NULL, 0);
        return id;
    }

    /* PTY path: one master fd used for both directions. */
    int pid = -1;
    if (skind != NULL)
        pid = os_spawn_shell_group_sandboxed(skind, spec->command, slave, slave, slave, sbsp, err,
                                             cap);
    else
        pid = os_spawn_group_sandboxed(spec->argv, NULL, spec->cwd, slave, slave, slave, 1, sbsp,
                                       err, cap);
    if (slave >= 0)
        os_close(slave);
    if (pid < 0) {
        if (sbsp != NULL && env != NULL && env->boundary != AGENTC_BOUND_NONE) {
            AgcActorEvent ev;
            agentc_memset(&ev, 0, sizeof ev);
            ev.text = (err != NULL && err[0]) ? err : "sandbox denied";
            agentc_events_emit(NULL, AGENTC_EV_SANDBOX_DENIED, &ev);
        }
        if (out >= 0)
            os_close(out);
        if (err != NULL && cap > 0 && err[0] == 0)
            agentc_snprintf(err, cap, "process: spawn failed (%d)", pid);
        return 0;
    }
    AgcActorId id =
        agentc_actor_register(AGENTC_ACTOR_PROCESS, parent, spec_label(spec, spec->command));
    if (id == 0) {
        os_kill(-pid, 9);
        if (out >= 0)
            os_close(out);
        if (err != NULL && cap > 0)
            agentc_snprintf(err, cap, "process: actor table full (%d)", AGENTC_ACTOR_MAX);
        return 0;
    }
    AgcActor *a = agentc_actor_get(id);
    AgcProcess *p = agentc_alloc(sizeof *p);
    agentc_memset(p, 0, sizeof *p);
    p->pid = pid;
    p->fd_out = out;
    p->fd_in = out;
    p->fd_err = -1;
    p->tty = true;
    p->stdin_open = true;
    p->cols = spec->cols;
    p->rows = spec->rows;
    p->last = '\n';
    p->wait_status = -99;
    p->spill_fd = -1;
    p->no_spill = (spec->flags & AGENTC_PROC_F_NO_SPILL) != 0;
    p->tail_cap = AGENTC_PROC_TAIL_BYTES;
    p->cmd = agentc_strdup(spec->command != NULL ? spec->command
                                                 : (spec->argv[0] != NULL ? spec->argv[0] : ""));
    p->cwd = spec->cwd != NULL ? agentc_strdup(spec->cwd) : NULL;
    a->proc = p;
    a->state = AGENTC_ACTOR_RUNNING;
    i64 deadline = spec->deadline_ms > 0 ? spec->deadline_ms : AGENTC_PROC_DEADLINE_DEF_MS;
    a->deadline_ns = deadline > 0 ? actor_now() + deadline * 1000000 : 0;
    if (spec->idle_ms > 0)
        a->idle_ms = spec->idle_ms;
    actor_event(a, AGENTC_EV_ACTOR_STARTED, NULL, NULL, 0);
    return id;
}

/* Public single-actor advance (actor.h): a foreground consumer can pump one
 * process without ticking the whole table. */
int agentc_process_poll(AgcActorId id) {
    AgcActor *a = agentc_actor_get(id);
    if (a == NULL || a->proc == NULL)
        return -3;
    process_actor_poll(a);
    return 0;
}

int process_actor_freeze(AgcActor *a) {
    if (a->proc == NULL || a->proc->pid <= 0)
        return 0;
    return os_process_freeze(a->proc->pid, true);
}

int process_actor_resume(AgcActor *a) {
    if (a->proc == NULL || a->proc->pid <= 0)
        return 0;
    return os_process_resume(a->proc->pid, true);
}

void process_actor_release(AgcActor *a) {
    AgcProcess *p = a->proc;
    if (p == NULL)
        return;
    if (a->state == AGENTC_ACTOR_FROZEN || a->frozen_ns != 0)
        (void)process_actor_resume(a);
    if (p->pid > 0 && !p->reaped) {
        if (a->cancel_stage != 2) {
            os_kill(-p->pid, 15);
            for (int i = 0; i < 20; i++) {
                if (os_wait(p->pid, true) != -1) {
                    p->reaped = true;
                    break;
                }
                os_poll(NULL, 0, 5);
            }
        }
        if (!p->reaped) {
            os_kill(-p->pid, 9);
            for (int i = 0; i < 200; i++) {
                if (os_wait(p->pid, true) != -1) {
                    p->reaped = true;
                    break;
                }
                os_poll(NULL, 0, 5);
            }
        }
    }
    if (p->fd_out >= 0)
        os_close(p->fd_out);
    if (p->fd_in >= 0 && p->fd_in != p->fd_out)
        os_close(p->fd_in);
    p->fd_out = p->fd_in = -1;
    if (p->spill_fd >= 0)
        os_close(p->spill_fd);
    p->spill_fd = -1;
    agentc_buf_free(&p->pending);
    agentc_buf_free(&p->tail);
    agentc_buf_free(&p->spill_path);
    agentc_buf_free(&p->stdin_buf);
    agentc_free(p->cmd);
    agentc_free(p->cwd);
    agentc_free(p);
    a->proc = NULL;
}

/* ---------------------------------------------------------------- I/O */

int agentc_process_write(AgcActorId id, const char *bytes, size_t n, bool close_stdin) {
    AgcActor *a = agentc_actor_get(id);
    if (a == NULL || a->proc == NULL)
        return -3; /* ESRCH */
    if (process_actor_terminal(a))
        return -3;
    AgcProcess *p = a->proc;
    if (p->fd_in < 0) {
        if (close_stdin)
            return -32; /* EPIPE: already closed */
        return -25;     /* ENOTTY: pipe mode has no stdin */
    }
    if (close_stdin && !p->stdin_open)
        return -32;
    if (n > AGENTC_PROC_WRITE_MAX)
        return -34; /* ERANGE */
    if (n > 0 && bytes == NULL)
        return -22;

    if (a->state == AGENTC_ACTOR_FROZEN) {
        if (p->stdin_buf.len + n > AGENTC_PROC_FREEZE_IN_MAX)
            return -11; /* EAGAIN */
        agentc_buf_push(&p->stdin_buf, bytes, n);
    } else if (n > 0) {
        int w = write_all_fd(p->fd_in, bytes, n);
        if (w < 0)
            return w;
    }
    if (close_stdin) {
        if (!p->tty) {
            os_close(p->fd_in);
            p->fd_in = -1;
        }
        p->stdin_open = false;
    }
    return 0;
}

/* Flush stdin buffered while frozen. Called on resume. */
static void process_flush_stdin(AgcProcess *p) {
    if (p->stdin_buf.len == 0)
        return;
    if (p->fd_in >= 0)
        (void)write_all_fd(p->fd_in, p->stdin_buf.p, p->stdin_buf.len);
    agentc_buf_clear(&p->stdin_buf);
}

size_t agentc_process_read(AgcActorId id, AgcBuf *out, size_t max) {
    AgcActor *a = agentc_actor_get(id);
    if (a == NULL || a->proc == NULL || out == NULL)
        return 0;
    AgcProcess *p = a->proc;
    size_t n = p->pending.len;
    if (max > 0 && n > max)
        n = max;
    if (n == 0)
        return 0;
    agentc_buf_push(out, p->pending.p, n);
    if (n < p->pending.len) {
        agentc_memmove(p->pending.p, p->pending.p + n, p->pending.len - n);
        p->pending.len -= n;
    } else {
        agentc_buf_clear(&p->pending);
    }
    return n;
}

size_t agentc_process_tail(AgcActorId id, AgcBuf *out, size_t max) {
    AgcActor *a = agentc_actor_get(id);
    if (a == NULL || a->proc == NULL || out == NULL)
        return 0;
    AgcProcess *p = a->proc;
    size_t n = p->tail.len;
    if (max > 0 && n > max)
        n = max;
    if (n == 0)
        return 0;
    agentc_buf_push(out, p->tail.p + (p->tail.len - n), n);
    return n;
}

int agentc_process_info(AgcActorId id, AgcProcInfo *out) {
    AgcActor *a = agentc_actor_get(id);
    if (a == NULL || a->proc == NULL || out == NULL)
        return -3;
    AgcProcess *p = a->proc;
    agentc_memset(out, 0, sizeof *out);
    out->id = a->id;
    out->state = a->state;
    out->pid = p->pid;
    out->tty = p->tty;
    out->stdin_open = p->stdin_open;
    out->cols = p->cols;
    out->rows = p->rows;
    out->exit_code = p->exit_code;
    out->total_bytes = p->total_bytes;
    out->total_lines = p->total_lines;
    out->duration_ms = a->started_ns != 0 ? (actor_now() - a->started_ns) / 1000000 : 0;
    out->eof = p->eof;
    out->reaped = p->reaped;
    out->cmd = p->cmd;
    out->spill_path = p->spill_path.p != NULL ? (const char *)p->spill_path.p : "";
    return 0;
}

int agentc_process_resize(AgcActorId id, int cols, int rows) {
    AgcActor *a = agentc_actor_get(id);
    if (a == NULL || a->proc == NULL)
        return -3;
    if (!a->proc->tty)
        return -25; /* ENOTTY */
    int r = os_pty_resize(a->proc->fd_out, cols, rows);
    if (r == 0) {
        a->proc->cols = cols;
        a->proc->rows = rows;
        actor_event(a, AGENTC_EV_ACTOR_RESIZED, NULL, NULL, 0);
    }
    return r;
}

int agentc_process_kill(AgcActorId id, int stage) {
    return agentc_actor_cancel(id, AGENTC_FREEZE_ACTOR, stage);
}

int agentc_process_spill_path(AgcActorId id, char *out, size_t cap) {
    AgcActor *a = agentc_actor_get(id);
    if (a == NULL || a->proc == NULL || out == NULL || cap == 0)
        return -3;
    if (a->proc->spill_path.p == NULL) {
        out[0] = 0;
        return -2; /* ENOENT: no spill */
    }
    agentc_snprintf(out, cap, "%s", (const char *)a->proc->spill_path.p);
    return 0;
}

int agentc_process_set_stdin(AgcActorId id, bool open) {
    AgcActor *a = agentc_actor_get(id);
    if (a == NULL || a->proc == NULL)
        return -3;
    AgcProcess *p = a->proc;
    if (open) {
        if (p->fd_in >= 0)
            return 0;
        if (!p->tty)
            return -25; /* ENOTTY: pipe mode has no stdin */
        return -5;
    }
    if (p->fd_in >= 0 && !p->tty) {
        os_close(p->fd_in);
        p->fd_in = -1;
    }
    p->stdin_open = false;
    return 0;
}

/* Called by agentc_actor_resume after the OS resume; flush buffered stdin. */
void process_actor_after_resume(AgcActor *a) {
    if (a->proc != NULL)
        process_flush_stdin(a->proc);
}
