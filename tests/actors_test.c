/* actors_test.c — P6-A actor registry and process actors (see P6-ACTORS.md §12).
 *
 * Deterministic golden suite: it drives the real actor layer, prints one
 * `label=1|0` line per assertion, and compares byte-for-byte. On Windows the
 * process tests are documented skips (PTY is -ENOSYS there and this suite does
 * not depend on cmd.exe semantics); each prints `label=skip` and the Wine run
 * compares against tests/data/actors_test.windows.expected, so a skip can never
 * masquerade as a pass.
 */
#include "agent.h"
#include "plat.h"
#include "base/limits.h"
#include "core/events.h"
#include "core/tools/jobs.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

/* A Windows process case prints an explicit skip (tests/data/actors_test.
 * windows.expected) instead of a hard-coded 1, so it cannot hide a regression. */
static void skip(const char *label) {
    agentc_outf("%s=skip\n", label);
}

static void check_win(const char *label, bool win, bool ok) {
    if (win)
        skip(label);
    else
        check(label, ok);
}

static bool contains(const char *hay, const char *needle) {
    return hay != NULL && agentc_str_str(hay, needle) != NULL;
}

/* ------------------------------------------------------------ event capture */
typedef struct {
    int started, progress, stalled, finish, fail, cancelled, cancel_req, frozen, resumed;
    int resized, rcols, rrows;
    bool force_stage;
} EvCount;

static EvCount g_ev;
static AgcActorId g_ev_id;

static void ev_reset(AgcActorId id) {
    agentc_memset(&g_ev, 0, sizeof g_ev);
    g_ev_id = id;
}

static void on_event(void *ud, int ev, const void *data) {
    (void)ud;
    const AgcActorEvent *e = data;
    if (e == NULL)
        return;
    if (g_ev_id != 0 && e->id != g_ev_id)
        return; /* 0 = capture all */
    switch (ev) {
    case AGENTC_EV_ACTOR_STARTED:
        g_ev.started++;
        break;
    case AGENTC_EV_ACTOR_PROGRESS:
        g_ev.progress++;
        break;
    case AGENTC_EV_ACTOR_STALLED:
        g_ev.stalled++;
        break;
    case AGENTC_EV_ACTOR_FINISH:
        g_ev.finish++;
        break;
    case AGENTC_EV_ACTOR_FAIL:
        g_ev.fail++;
        break;
    case AGENTC_EV_ACTOR_CANCELLED:
        g_ev.cancelled++;
        if (e->json != NULL && agentc_str_str(e->json, "\"stage\":2") != NULL)
            g_ev.force_stage = true;
        break;
    case AGENTC_EV_ACTOR_CANCEL_REQUESTED:
        g_ev.cancel_req++;
        break;
    case AGENTC_EV_ACTOR_FROZEN:
        g_ev.frozen++;
        break;
    case AGENTC_EV_ACTOR_RESUMED:
        g_ev.resumed++;
        break;
    case AGENTC_EV_ACTOR_RESIZED:
        g_ev.resized++;
        g_ev.rcols = e->cols;
        g_ev.rrows = e->rows;
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------- helpers */
static AgcActorId spawn_cmd(const char *cmd, int mode, int idle_ms, i64 deadline_ms, int cols,
                            int rows, const char *label) {
    AgcProcSpawn spec;
    agentc_memset(&spec, 0, sizeof spec);
    spec.command = cmd;
    spec.mode = mode;
    spec.idle_ms = idle_ms;
    spec.deadline_ms = deadline_ms;
    spec.cols = cols;
    spec.rows = rows;
    spec.label = label;
    AgcActorSpec as = {.proc = &spec, .agent = NULL};
    char err[128];
    err[0] = 0;
    return agentc_actor_spawn(&as, err, sizeof err);
}

static bool terminal_state(AgcActorState s) {
    return s == AGENTC_ACTOR_SETTLED || s == AGENTC_ACTOR_FAILED || s == AGENTC_ACTOR_CANCELLED;
}

static bool wait_terminal(AgcActorId id, int timeout_ms) {
    i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    for (;;) {
        AgcActor *a = agentc_actor_get(id);
        if (a == NULL || terminal_state(a->state))
            return true;
        if ((os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000 >= timeout_ms)
            return false;
        agentc_actor_tick();
        os_poll(NULL, 0, 5);
    }
}

static void drain_all(AgcActorId id, AgcBuf *out) {
    for (;;) {
        size_t n = agentc_process_read(id, out, 0);
        if (n == 0)
            break;
    }
}

static AgcActorState state_of(AgcActorId id) {
    AgcActor *a = agentc_actor_get(id);
    return a != NULL ? a->state : AGENTC_ACTOR_FAILED;
}

/* --------------------------------------------------------------- tests */
/* register -> started -> progress -> finish; states + events + output. */
static void test_lifecycle(bool win) {
    ev_reset(0);
    AgcActorId id = win ? 0 : spawn_cmd("echo hi", AGENTC_PROC_PIPE, 0, 5000, 0, 0, "lc");
    check_win("lifecycle.exists", win, (id != 0 && agentc_actor_exists(id)));
    check_win("lifecycle.parent", win, agentc_actor_parent(id) == 0);
    check_win("lifecycle.count", win, agentc_actor_count(AGENTC_ACTOR_PROCESS) >= 1);
    bool term = win || wait_terminal(id, 4000);
    AgcProcInfo pi;
    bool info_ok = !win && agentc_process_info(id, &pi) == 0;
    check_win("lifecycle.settled", win, (term && state_of(id) == AGENTC_ACTOR_SETTLED));
    check_win("lifecycle.exit0", win, (info_ok && pi.exit_code == 0));
    check_win("lifecycle.ev_started", win, g_ev.started == 1);
    check_win("lifecycle.ev_progress", win, g_ev.progress >= 1);
    check_win("lifecycle.ev_finish", win, g_ev.finish == 1);
    AgcBuf out = {0};
    if (!win)
        drain_all(id, &out);
    check_win("lifecycle.output", win, contains((char *)out.p, "hi"));
    agentc_buf_free(&out);
    if (!win)
        agentc_actor_release(id);
    check_win("lifecycle.released", win, !agentc_actor_exists(id));
}

/* stall fires once per episode at idle_ms and clears on progress. */
static void test_stall(bool win) {
    ev_reset(0);
    AgcActorId id =
        win ? 0
            : spawn_cmd("sleep 1; echo tick; sleep 5", AGENTC_PROC_PIPE, 200, 10000, 0, 0, "stall");
    bool stalled = false, cleared = false;
    if (!win) {
        i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
        while ((os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000 < 3000) {
            agentc_actor_tick();
            if (!stalled && g_ev.stalled >= 1 && state_of(id) == AGENTC_ACTOR_STALLED) {
                stalled = true;
            }
            if (stalled && g_ev.progress >= 1 && state_of(id) == AGENTC_ACTOR_RUNNING) {
                cleared = true;
                break;
            }
            os_poll(NULL, 0, 5);
        }
    }
    check_win("stall.fired", win, stalled);
    check_win("stall.cleared", win, cleared);
    check_win("stall.once", win, g_ev.stalled == 1);
    if (!win) {
        (void)agentc_actor_cancel(id, AGENTC_FREEZE_ACTOR, 2);
        (void)wait_terminal(id, 3000);
        agentc_actor_release(id);
    }
}

/* graceful -> force escalation, kills a SIGTERM-ignoring child, once. */
static void test_cancel(bool win) {
    if (win) {
        skip("cancel.requested");
        skip("cancel.cancelled_once");
        skip("cancel.force_escalated");
        skip("cancel.state");
        skip("cancel.group_dead");
        skip("cancel.idempotent");
        return;
    }
    ev_reset(0);
    AgcActorId id = win ? 0
                        : spawn_cmd("trap '' TERM; sleep 10; sleep 10", AGENTC_PROC_PIPE, 0, 10000,
                                    0, 0, "cancel");
    if (!win) {
        /* let the shell install the trap before the first SIGTERM, so the
         * graceful signal is genuinely ignored and force escalation is tested */
        os_sleep_ns(300 * 1000000);
        agentc_actor_tick();
        agentc_actor_get(id)->grace_ms = 150;
    }
    AgcProcInfo cpi;
    agentc_process_info(id, &cpi);
    int group_pid = cpi.pid;
    int r1 = win ? 0 : agentc_actor_cancel(id, AGENTC_FREEZE_ACTOR, 1);
    check_win("cancel.requested", win, (r1 == 0 && g_ev.cancel_req >= 1));
    bool term = win || wait_terminal(id, 4000);
    check_win("cancel.cancelled_once", win, (term && g_ev.cancelled == 1));
    check_win("cancel.force_escalated", win, g_ev.force_stage);
    check_win("cancel.state", win, state_of(id) == AGENTC_ACTOR_CANCELLED);
    check_win("cancel.group_dead", win, (group_pid > 0 && os_kill(-group_pid, 0) != 0));
    /* idempotent: a second cancel after the terminal state changes nothing */
    if (!win)
        (void)agentc_actor_cancel(id, AGENTC_FREEZE_ACTOR, 2);
    check_win("cancel.idempotent", win, g_ev.cancelled == 1);
    if (!win)
        agentc_actor_release(id);
}

/* actor table bounded: the (MAX+1)th spawn is refused with the limit named. */
static void test_table_full(bool win) {
    if (win) {
        /* Same check order as the native path; the Wine run uses
         * tests/data/actors_test.windows.expected (all `=skip`). */
        skip("table.count");
        skip("table.full");
        skip("table.released");
        return;
    }
    agentc_actor_release_all();
    size_t spawned = 0;
    for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
        AgcActorId id = spawn_cmd("sleep 2", AGENTC_PROC_PIPE, 0, 5000, 0, 0, "full");
        if (id != 0)
            spawned++;
    }
    check("table.count", spawned == AGENTC_ACTOR_MAX);
    AgcProcSpawn spec;
    agentc_memset(&spec, 0, sizeof spec);
    spec.command = "sleep 2";
    spec.mode = AGENTC_PROC_PIPE;
    spec.deadline_ms = 5000;
    AgcActorSpec as = {.proc = &spec, .agent = NULL};
    char err[128] = "";
    AgcActorId over = agentc_actor_spawn(&as, err, sizeof err);
    check("table.full", over == 0 && contains(err, "actor table full"));
    agentc_actor_release_all();
    check("table.released", agentc_actor_count((AgcActorKind)-1) == 0);
}

/* PTY stdin write + close_stdin; pipe mode has no stdin (-ENOTTY). */
static void test_stdin(bool win) {
    if (win) {
        skip("stdin.pty_spawn");
        skip("stdin.pty_write");
        skip("stdin.close");
        skip("stdin.close_idempotent");
        skip("stdin.pipe_enotty");
        return;
    }
    AgcActorId id = spawn_cmd("cat", AGENTC_PROC_PTY, 0, 5000, 80, 24, "cat");
    ev_reset(id);
    check("stdin.pty_spawn", id != 0);
    int wr = agentc_process_write(id, "hello\n", 6, false);
    bool saw = false;
    if (wr == 0) {
        i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
        while ((os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000 < 2000 && !saw) {
            agentc_actor_tick();
            AgcBuf b = {0};
            drain_all(id, &b);
            if (contains((char *)b.p, "hello"))
                saw = true;
            agentc_buf_free(&b);
            os_poll(NULL, 0, 5);
        }
    }
    check("stdin.pty_write", saw);
    int c1 = agentc_process_write(id, NULL, 0, true);
    int c2 = agentc_process_write(id, NULL, 0, true);
    check("stdin.close", c1 == 0);
    check("stdin.close_idempotent", c2 == -32);
    (void)agentc_actor_cancel(id, AGENTC_FREEZE_ACTOR, 2);
    (void)wait_terminal(id, 3000);
    agentc_actor_release(id);

    AgcActorId p = spawn_cmd("cat", AGENTC_PROC_PIPE, 0, 2000, 0, 0, "catpipe");
    int pe = agentc_process_write(p, "x", 1, false);
    check("stdin.pipe_enotty", pe == -25);
    (void)agentc_actor_cancel(p, AGENTC_FREEZE_ACTOR, 2);
    (void)wait_terminal(p, 3000);
    agentc_actor_release(p);
}

/* tail ring keeps the last tail_cap bytes; the spill holds the full stream. */
static void test_tail_spill(bool win) {
    if (win) {
        skip("tail.spawn");
        skip("tail.capped");
        skip("tail.spill_exists");
        return;
    }
    AgcActorId id = spawn_cmd("dd if=/dev/zero bs=1000 count=100 2>/dev/null | tr '\\0' A",
                              AGENTC_PROC_PIPE, 0, 8000, 0, 0, "big");
    ev_reset(id);
    check("tail.spawn", id != 0);
    bool term = wait_terminal(id, 6000);
    AgcBuf tail = {0};
    (void)agentc_process_tail(id, &tail, 0);
    check("tail.capped", term && tail.len == AGENTC_PROC_TAIL_BYTES);
    agentc_buf_free(&tail);
    char path[4096] = "";
    int sp = agentc_process_spill_path(id, path, sizeof path);
    int fd = path[0] ? os_open(path, OS_O_RDONLY, 0) : -1;
    check("tail.spill_exists", sp == 0 && fd >= 0);
    if (fd >= 0)
        os_close(fd);
    if (path[0])
        os_unlink(path);
    agentc_actor_release(id);
}

/* freeze suspends timers: a deadline does not fire while frozen and does fire
 * after resume (shifted forward). */
static void test_freeze(bool win) {
    if (win) {
        skip("freeze.spawn");
        skip("freeze.frozen");
        skip("freeze.ev_frozen");
        skip("freeze.still_frozen");
        skip("freeze.resumed");
        skip("freeze.ev_resumed");
        skip("freeze.deadline_after");
        return;
    }
    AgcActorId id = spawn_cmd("sleep 5", AGENTC_PROC_PIPE, 0, 150, 0, 0, "freeze");
    ev_reset(id);
    check("freeze.spawn", id != 0);
    int fz = agentc_actor_freeze(id, AGENTC_FREEZE_ACTOR, 0);
    check("freeze.frozen", fz == 0 && agentc_actor_frozen(id));
    check("freeze.ev_frozen", g_ev.frozen == 1);
    os_sleep_ns(250 * 1000000); /* past the 150 ms deadline */
    agentc_actor_tick();
    check("freeze.still_frozen", agentc_actor_frozen(id) && g_ev.cancelled == 0);
    int rs = agentc_actor_resume(id, AGENTC_FREEZE_ACTOR);
    check("freeze.resumed", rs == 0 && !agentc_actor_frozen(id));
    check("freeze.ev_resumed", g_ev.resumed == 1);
    bool term = wait_terminal(id, 3000);
    check("freeze.deadline_after", term && state_of(id) == AGENTC_ACTOR_CANCELLED);
    agentc_actor_release(id);
}

/* release_all reaps every live (and frozen) actor with no orphan left. */
static void test_release_all(bool win) {
    if (!win) {
        (void)spawn_cmd("sleep 5", AGENTC_PROC_PIPE, 0, 10000, 0, 0, "ra1");
        (void)spawn_cmd("sleep 5", AGENTC_PROC_PIPE, 0, 10000, 0, 0, "ra2");
        agentc_actor_release_all();
    }
    check_win("release_all.empty", win, agentc_actor_count((AgcActorKind)-1) == 0);
}

static AgcActorId parse_actor_id(const char *json) {
    const char *p = json != NULL ? agentc_str_str(json, "actor_id") : NULL;
    if (p == NULL)
        return 0;
    p += 8;
    while (*p != 0 && (*p < '0' || *p > '9'))
        p++;
    size_t k = 0;
    while (p[k] >= '0' && p[k] <= '9')
        k++;
    bool ok = false;
    u64 v = agentc_parse_u64(p, k, &ok);
    return ok ? (AgcActorId)v : 0;
}

static const AgcTool *find_builtin(AgcTool *tools, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(tools[i].name, name))
            return &tools[i];
    return NULL;
}

/* bash gains background:true and yield_ms; the detached process survives the
 * batch and its stable actor id remains usable. */
static void test_bash_bg(bool win) {
    if (win) {
        skip("bash_bg.spawn");
        skip("bash_bg.id");
        skip("bash_bg.survives");
        skip("bash_bg.yield");
        return;
    }
    AgcTool tools[8];
    size_t n = agentc_tools_builtin(tools, 8);
    const AgcTool *bash = find_builtin(tools, n, "bash");
    check("bash_bg.spawn", bash != NULL && bash->start != NULL);
    if (bash == NULL || bash->start == NULL)
        return;

    AgcJob job;
    agentc_tool_job_init(&job);
    AgcToolCall call = {"t", "bash", "{\"command\":\"echo bg; sleep 5\",\"background\":true}",
                        NULL};
    job.tool = bash;
    job.call = call;
    int r = bash->start(bash, &call, &job);
    AgcActorId id = parse_actor_id((char *)job.out.p);
    check("bash_bg.id", r == 1 && id != 0);
    check("bash_bg.survives", id != 0 && agentc_actor_exists(id));
    agentc_buf_free(&job.out);
    agentc_buf_free(&job.spill_path);
    agentc_free(job.priv);
    if (id != 0)
        agentc_actor_release(id);

    AgcJob yj;
    agentc_tool_job_init(&yj);
    AgcToolCall ycall = {"t", "bash", "{\"command\":\"echo hi; sleep 5\",\"yield_ms\":200}", NULL};
    yj.tool = bash;
    yj.call = ycall;
    r = bash->start(bash, &ycall, &yj);
    i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    while (r == 0 && (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000 < 3000) {
        r = bash->step(bash, &yj);
        if (r == 0)
            os_poll(NULL, 0, 5);
    }
    AgcActorId yid = parse_actor_id((char *)yj.out.p);
    check("bash_bg.yield", r == 1 && yid != 0 && contains((char *)yj.out.p, "[running: actor_id"));
    agentc_buf_free(&yj.out);
    agentc_buf_free(&yj.spill_path);
    agentc_free(yj.priv);
    if (yid != 0)
        agentc_actor_release(yid);
}

/* the `process` tool adapter: spawn returns an id, read drains the stream. */
static void test_process_tool(bool win) {
    if (win) {
        skip("ptool.spawn");
        skip("ptool.read");
        skip("ptool.list");
        return;
    }
    AgcTool tools[8];
    size_t n = agentc_tools_builtin(tools, 8);
    const AgcTool *pt = find_builtin(tools, n, "process");
    bool err = false;
    AgcBuf out = {0};
    AgcToolCall call = {"t", "process", "{\"op\":\"spawn\",\"command\":\"echo ptool\"}", NULL};
    int rc = pt->run(pt, &call, &out, &err);
    AgcActorId id = parse_actor_id((char *)out.p);
    check("ptool.spawn", rc == 0 && id != 0);

    bool saw = false;
    for (int i = 0; i < 400 && !saw; i++) {
        agentc_actor_tick();
        agentc_buf_clear(&out);
        char args[64];
        agentc_snprintf(args, sizeof args, "{\"op\":\"read\",\"actor_id\":%u}", (unsigned)id);
        AgcToolCall rcall = {"t", "process", args, NULL};
        (void)pt->run(pt, &rcall, &out, &err);
        if (contains((char *)out.p, "ptool"))
            saw = true;
        if (!saw)
            os_poll(NULL, 0, 5);
    }
    check("ptool.read", saw);

    agentc_buf_clear(&out);
    AgcToolCall lcall = {"t", "process", "{\"op\":\"list\"}", NULL};
    (void)pt->run(pt, &lcall, &out, &err);
    check("ptool.list", contains((char *)out.p, "process"));
    agentc_buf_free(&out);
    if (id != 0)
        agentc_actor_release(id);
}

/* resize emits one typed event and one lifecycle hook per successful PTY
 * resize; a pipe actor has no TTY and refuses without either. */
static int g_hook_resized;

static int on_lifecycle(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)next;
    (void)defer;
    if (m && m->meta && agentc_streq(m->meta, "resized"))
        g_hook_resized++;
    return 0;
}

static void test_resize(bool win) {
    if (win) {
        skip("resize.spawn");
        skip("resize.ok");
        skip("resize.info");
        skip("resize.ev_once");
        skip("resize.ev_geom");
        skip("resize.hook_once");
        skip("resize.pipe_refused");
        return;
    }
    ev_reset(0);
    g_hook_resized = 0;
    uint64_t h = agentc_hook(AGENTC_HOOK_STAGE_LIFECYCLE, 0, on_lifecycle, NULL);
    AgcActorId id = spawn_cmd("sleep 5", AGENTC_PROC_PTY, 0, 8000, 80, 24, "resize");
    check("resize.spawn", id != 0);
    int r = agentc_process_resize(id, 100, 40);
    AgcProcInfo pi;
    bool info_ok = agentc_process_info(id, &pi) == 0;
    check("resize.ok", r == 0);
    check("resize.info", info_ok && pi.cols == 100 && pi.rows == 40);
    check("resize.ev_once", g_ev.resized == 1);
    check("resize.ev_geom", g_ev.rcols == 100 && g_ev.rrows == 40);
    check("resize.hook_once", g_hook_resized == 1);

    AgcActorId p = spawn_cmd("cat", AGENTC_PROC_PIPE, 0, 3000, 0, 0, "resizepipe");
    int pr = agentc_process_resize(p, 90, 30);
    check("resize.pipe_refused", pr == -25 && g_ev.resized == 1 && g_hook_resized == 1);

    if (h)
        agentc_hook_off(h);
    (void)agentc_actor_cancel(id, AGENTC_FREEZE_ACTOR, 2);
    (void)wait_terminal(id, 3000);
    agentc_actor_release(id);
    (void)agentc_actor_cancel(p, AGENTC_FREEZE_ACTOR, 2);
    (void)wait_terminal(p, 3000);
    agentc_actor_release(p);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bool win = agentc_streq(os_platform(), "windows");
    agentc_events_reset();
    (void)agentc_events_subscribe(on_event, NULL);

    test_lifecycle(win);
    test_stall(win);
    test_cancel(win);
    test_table_full(win);
    test_stdin(win);
    test_resize(win);
    test_tail_spill(win);
    test_freeze(win);
    test_release_all(win);
    test_bash_bg(win);
    test_process_tool(win);

    agentc_events_reset();
    agentc_actor_release_all();
    return fails;
}
