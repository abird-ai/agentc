/* freeze_test.c — P6-ACTORS §6b freeze/resume (see P6-INTEGRATION.md §7).
 *
 * Covers: actor/subtree/runtime freeze+resume, event emission, timers
 * suspended across a freeze (a deadline does not fire while frozen and fires
 * after resume), stdin written while frozen is buffered and flushed on resume,
 * cancel resumes a frozen actor before terminating it, and release_all reaps a
 * frozen child with no orphan.
 *
 * Deterministic golden: one `label=1|0` line per assertion, byte-compared.
 * Windows has no SIGSTOP/SIGCONT (`os_process_freeze` -> -ENOSYS), so the
 * process cases print an explicit `label=skip` there; the Wine run compares
 * against tests/data/freeze_test.windows.expected (tests/wine.sh), so a
 * regression can never masquerade as a pass.
 */
#include "agent.h"
#include "plat.h"
#include "base/limits.h"
#include "core/events.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

static void check_skip(const char *label) {
    agentc_outf("%s=skip\n", label);
}

static bool contains(const char *hay, const char *needle) {
    return hay != NULL && agentc_str_str(hay, needle) != NULL;
}

/* ------------------------------------------------------------ event capture */
typedef struct {
    int frozen, resumed, cancelled, cancel_req;
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
        return;
    switch (ev) {
    case AGENTC_EV_ACTOR_FROZEN:
        g_ev.frozen++;
        break;
    case AGENTC_EV_ACTOR_RESUMED:
        g_ev.resumed++;
        break;
    case AGENTC_EV_ACTOR_CANCELLED:
        g_ev.cancelled++;
        break;
    case AGENTC_EV_ACTOR_CANCEL_REQUESTED:
        g_ev.cancel_req++;
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

static AgcActorState state_of(AgcActorId id) {
    AgcActor *a = agentc_actor_get(id);
    return a != NULL ? a->state : AGENTC_ACTOR_FAILED;
}

/* Read until `needle` shows up or the timeout expires. */
static bool read_until(AgcActorId id, const char *needle, int timeout_ms) {
    AgcBuf all = {0};
    bool saw = false;
    i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    while ((os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000 < timeout_ms) {
        agentc_actor_tick();
        for (;;) {
            size_t n = agentc_process_read(id, &all, 0);
            if (n == 0)
                break;
        }
        if (all.p != NULL && agentc_str_str((char *)all.p, needle) != NULL) {
            saw = true;
            break;
        }
        os_poll(NULL, 0, 5);
    }
    agentc_buf_free(&all);
    return saw;
}

/* --------------------------------------------------------------- tests */

/* actor freeze/resume + timers suspended across the freeze. */
static void test_actor(bool win) {
    if (win) {
        check_skip("freeze.actor.spawn");
        check_skip("freeze.actor.frozen");
        check_skip("freeze.actor.ev_frozen");
        check_skip("freeze.actor.timer_suspended");
        check_skip("freeze.actor.resumed");
        check_skip("freeze.actor.ev_resumed");
        check_skip("freeze.actor.deadline_shifted");
        return;
    }
    agentc_actor_init();
    AgcActorId id = spawn_cmd("sleep 5", AGENTC_PROC_PIPE, 0, 150, 0, 0, "frozen");
    ev_reset(id);
    check("freeze.actor.spawn", id != 0);
    int fz = agentc_actor_freeze(id, AGENTC_FREEZE_ACTOR, 0);
    check("freeze.actor.frozen", fz == 0 && agentc_actor_frozen(id));
    check("freeze.actor.ev_frozen", g_ev.frozen == 1);
    /* the 150 ms deadline must not fire while frozen */
    os_sleep_ns(250 * 1000000);
    agentc_actor_tick();
    check("freeze.actor.timer_suspended", agentc_actor_frozen(id) && g_ev.cancelled == 0);
    int rs = agentc_actor_resume(id, AGENTC_FREEZE_ACTOR);
    check("freeze.actor.resumed", rs == 0 && !agentc_actor_frozen(id));
    check("freeze.actor.ev_resumed", g_ev.resumed == 1);
    bool term = wait_terminal(id, 3000);
    check("freeze.actor.deadline_shifted", term && state_of(id) == AGENTC_ACTOR_CANCELLED);
    agentc_actor_release_all();
}

/* subtree freeze freezes the target and its descendants but not a sibling. */
static void test_subtree(bool win) {
    if (win) {
        check_skip("freeze.subtree.parent");
        check_skip("freeze.subtree.child1");
        check_skip("freeze.subtree.child2");
        check_skip("freeze.subtree.grandchild");
        check_skip("freeze.subtree.sibling");
        check_skip("freeze.subtree.resumed");
        check_skip("freeze.subtree.sibling_after");
        return;
    }
    agentc_actor_init();
    AgcActorId p = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "p");
    AgcActorId c1 = agentc_actor_register(AGENTC_ACTOR_AGENT, p, "c1");
    AgcActorId c2 = agentc_actor_register(AGENTC_ACTOR_AGENT, p, "c2");
    AgcActorId g = agentc_actor_register(AGENTC_ACTOR_AGENT, c1, "g");
    AgcActorId sib = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "sib");
    int fz = agentc_actor_freeze(p, AGENTC_FREEZE_SUBTREE, 0);
    check("freeze.subtree.parent", fz == 0 && agentc_actor_frozen(p));
    check("freeze.subtree.child1", agentc_actor_frozen(c1));
    check("freeze.subtree.child2", agentc_actor_frozen(c2));
    check("freeze.subtree.grandchild", agentc_actor_frozen(g));
    check("freeze.subtree.sibling", !agentc_actor_frozen(sib));
    int rs = agentc_actor_resume(p, AGENTC_FREEZE_SUBTREE);
    check("freeze.subtree.resumed", rs == 0 && !agentc_actor_frozen(p) &&
                                        !agentc_actor_frozen(c1) && !agentc_actor_frozen(c2) &&
                                        !agentc_actor_frozen(g));
    check("freeze.subtree.sibling_after", !agentc_actor_frozen(sib));
    agentc_actor_release_all();
}

/* runtime freeze/resume touches every registered actor. */
static void test_runtime(bool win) {
    if (win) {
        check_skip("freeze.runtime.all_frozen");
        check_skip("freeze.runtime.resume");
        check_skip("freeze.runtime.all_resumed");
        return;
    }
    agentc_actor_init();
    AgcActorId a = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "ra");
    AgcActorId b = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "rb");
    int fz = agentc_runtime_freeze(0);
    check("freeze.runtime.all_frozen", fz == 0 && agentc_actor_frozen(a) && agentc_actor_frozen(b));
    int rs = agentc_runtime_resume();
    check("freeze.runtime.resume", rs == 0);
    check("freeze.runtime.all_resumed", !agentc_actor_frozen(a) && !agentc_actor_frozen(b));
    agentc_actor_release_all();
}

/* stdin written while frozen is buffered (no output yet) and flushed on
 * resume, so the process round-trips it. */
static void test_stdin(bool win) {
    if (win) {
        check_skip("freeze.stdin.spawn");
        check_skip("freeze.stdin.write");
        check_skip("freeze.stdin.buffered");
        check_skip("freeze.stdin.resumed");
        check_skip("freeze.stdin.echo");
        return;
    }
    agentc_actor_init();
    AgcActorId id = spawn_cmd("cat", AGENTC_PROC_PIPE_BIDI, 0, 8000, 0, 0, "cat");
    check("freeze.stdin.spawn", id != 0);
    (void)agentc_actor_freeze(id, AGENTC_FREEZE_ACTOR, 0);
    int wr = agentc_process_write(id, "late\n", 5, false);
    check("freeze.stdin.write", wr == 0);
    agentc_actor_tick();
    AgcBuf before = {0};
    (void)agentc_process_read(id, &before, 0);
    check("freeze.stdin.buffered", before.len == 0);
    agentc_buf_free(&before);
    (void)agentc_actor_resume(id, AGENTC_FREEZE_ACTOR);
    check("freeze.stdin.resumed", !agentc_actor_frozen(id));
    check("freeze.stdin.echo", read_until(id, "late", 2000));
    agentc_actor_release_all();
}

/* cancel resumes a frozen actor first, then terminates it. */
static void test_cancel(bool win) {
    if (win) {
        check_skip("freeze.cancel.frozen");
        check_skip("freeze.cancel.resumes_before_term");
        check_skip("freeze.cancel.terminal");
        return;
    }
    agentc_actor_init();
    AgcActorId id = spawn_cmd("sleep 5", AGENTC_PROC_PIPE, 0, 10000, 0, 0, "c");
    ev_reset(id);
    (void)agentc_actor_freeze(id, AGENTC_FREEZE_ACTOR, 0);
    check("freeze.cancel.frozen", agentc_actor_frozen(id));
    int rc = agentc_actor_cancel(id, AGENTC_FREEZE_ACTOR, 1);
    check("freeze.cancel.resumes_before_term",
          rc == 0 && !agentc_actor_frozen(id) && g_ev.cancel_req >= 1);
    bool term = wait_terminal(id, 3000);
    check("freeze.cancel.terminal", term && state_of(id) == AGENTC_ACTOR_CANCELLED);
    agentc_actor_release_all();
}

/* release_all reaps a frozen child (no orphan, no frozen row). */
static void test_reap(bool win) {
    if (win) {
        check_skip("freeze.reap.frozen");
        check_skip("freeze.reap.count_zero");
        check_skip("freeze.reap.process_gone");
        return;
    }
    agentc_actor_init();
    AgcActorId id = spawn_cmd("sleep 5", AGENTC_PROC_PIPE, 0, 10000, 0, 0, "r");
    AgcProcInfo pi;
    int pgid = (id != 0 && agentc_process_info(id, &pi) == 0) ? pi.pid : -1;
    (void)agentc_actor_freeze(id, AGENTC_FREEZE_ACTOR, 0);
    check("freeze.reap.frozen", agentc_actor_frozen(id));
    agentc_actor_release_all();
    check("freeze.reap.count_zero", agentc_actor_count((AgcActorKind)-1) == 0);
    check("freeze.reap.process_gone", pgid > 0 && os_kill(-pgid, 0) != 0);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bool win = agentc_streq(os_platform(), "windows");
    agentc_events_reset();
    (void)agentc_events_subscribe(on_event, NULL);

    test_actor(win);
    test_subtree(win);
    test_runtime(win);
    test_stdin(win);
    test_cancel(win);
    test_reap(win);

    agentc_events_reset();
    agentc_actor_release_all();
    return fails;
}
