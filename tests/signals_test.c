/* signals_test.c — P6-B message bus and unified queue golden (P6-SIGNALS §13).
 *
 * Fully scripted and deterministic (no processes, no wall clock): the same
 * expected file holds on the native and the Wine run. Prints one
 * `label=value` line per assertion and compares byte-for-byte.
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

/* --------------------------------------------------------------- A: bus */

typedef struct {
    int kinds[AGENTC_MSG_MAILBOX_MAX];
    uint32_t froms[AGENTC_MSG_MAILBOX_MAX];
    uint64_t seqs[AGENTC_MSG_MAILBOX_MAX];
    int n;
} MsgLog;

static MsgLog g_log;

static int rec_handler(void *ud, AgcActorId to, const AgcMessage *m) {
    (void)ud;
    (void)to;
    if (g_log.n < AGENTC_MSG_MAILBOX_MAX) {
        g_log.kinds[g_log.n] = m->kind;
        g_log.froms[g_log.n] = m->from;
        g_log.seqs[g_log.n] = m->seq;
        g_log.n++;
    }
    return 0;
}

static void send_input(AgcActorId to, AgcActorId from) {
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_INPUT;
    m.kind = AGENTC_MSG_INPUT;
    m.to = to;
    m.from = from;
    m.body = "x";
    (void)agentc_message_send(&m);
}

static void test_drain_order(void) {
    agentc_actor_init();
    AgcActorId a = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "a");
    AgcActorId b = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "b");
    AgcActorId c = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "c");
    agentc_message_set_handler(a, rec_handler, NULL);
    agentc_memset(&g_log, 0, sizeof g_log);
    send_input(a, b);
    send_input(a, c);
    send_input(a, b);
    check("A.pending3", agentc_message_pending(a) == 3);
    check("A.drained3", agentc_message_drain(a, 0) == 3 && g_log.n == 3);
    check("A.arrival", g_log.froms[0] == b && g_log.froms[1] == c && g_log.froms[2] == b);
    check("A.sender_seq", g_log.seqs[0] == 1 && g_log.seqs[1] == 1 && g_log.seqs[2] == 2);
    check("A.pending0", agentc_message_pending(a) == 0);
    agentc_actor_release(b);
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.kind = AGENTC_MSG_INPUT;
    m.to = b;
    m.from = a;
    check("A.esrch", agentc_message_send(&m) == -3);
    agentc_actor_release_all();
}

static void test_broadcast(void) {
    agentc_actor_init();
    AgcActorId r = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "r");
    AgcActorId x = agentc_actor_register(AGENTC_ACTOR_PROCESS, 0, "x");
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_CONTROL;
    m.kind = AGENTC_MSG_CANCEL;
    m.to = 0;
    m.from = r;
    m.scope = AGENTC_MSG_SCOPE_ACTOR;
    m.stage = AGENTC_MSG_STAGE_GRACEFUL;
    check("A.broadcast", agentc_message_send(&m) == 0 && agentc_message_pending(r) == 1 &&
                             agentc_message_pending(x) == 1);
    m.kind = AGENTC_MSG_INPUT;
    check("A.broadcast_input_rejected", agentc_message_send(&m) == -22);
    agentc_actor_release_all();
}

/* capture counts of kinds after draining the full mailbox */
static int count_kind(int kind) {
    int n = 0;
    for (int i = 0; i < g_log.n; i++)
        if (g_log.kinds[i] == kind)
            n++;
    return n;
}

static void test_overflow(void) {
    agentc_actor_init();
    AgcActorId a = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "a");
    agentc_message_set_handler(a, rec_handler, NULL);
    agentc_memset(&g_log, 0, sizeof g_log);
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_NOTIFY;
    m.kind = AGENTC_MSG_PROGRESS;
    m.to = a;
    for (int i = 0; i < AGENTC_MSG_MAILBOX_MAX; i++)
        (void)agentc_message_send(&m);
    check("A.mailbox_full", agentc_message_pending(a) == AGENTC_MSG_MAILBOX_MAX);
    m.kind = AGENTC_MSG_STALLED;
    check("A.drop_lower",
          agentc_message_send(&m) == 0 && agentc_message_pending(a) == AGENTC_MSG_MAILBOX_MAX);
    m.kind = AGENTC_MSG_FINISH;
    check("A.keep_lifecycle",
          agentc_message_send(&m) == 0 && agentc_message_pending(a) == AGENTC_MSG_MAILBOX_MAX);
    (void)agentc_message_drain(a, 0);
    check("A.lifecycle_once", count_kind(AGENTC_MSG_FINISH) == 1);
    check("A.stalled_dropped", count_kind(AGENTC_MSG_STALLED) == 0);
    agentc_actor_release_all();
}

static void test_priority_clamp(void) {
    agentc_actor_init();
    AgcActorId a = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "a");
    agentc_message_set_handler(a, rec_handler, NULL);
    agentc_memset(&g_log, 0, sizeof g_log);
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.kind = AGENTC_MSG_PROGRESS;
    m.to = a;
    m.priority = 1000;
    (void)agentc_message_send(&m);
    check("A.prio_clamped", agentc_message_pending(a) == 1);
    agentc_actor_release_all();
}

/* ------------------------------------------------------------- B: queue */

static void test_queue_order(void) {
    agentc_actor_init();
    AgcActorId q = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "q");
    (void)agentc_queue_add(q, "five", 0, 5, 0, NULL);
    (void)agentc_queue_add(q, "zero", 0, 0, 0, NULL);
    (void)agentc_queue_add(q, "neg2", 0, -2, 0, NULL);
    (void)agentc_queue_add(q, "one", 0, 1, 0, NULL);
    (void)agentc_queue_add(q, "neg1", 0, -1, 0, NULL);
    check("B.count", agentc_queue_count(q) == 5);
    AgcQueueItem items[8];
    int n = agentc_queue_list(q, items, 8);
    check("B.list_n", n == 5);
    check("B.order", items[0].priority == -2 && items[1].priority == -1 && items[2].priority == 0 &&
                         items[3].priority == 1 && items[4].priority == 5);
    check("B.text_order",
          agentc_streq(items[0].text, "neg2") && agentc_streq(items[4].text, "five"));
    agentc_actor_release_all();
}

static void test_queue_flush(void) {
    agentc_actor_init();
    AgcActorId q = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "q");
    (void)agentc_queue_add(q, "five", 0, 5, 0, NULL);
    (void)agentc_queue_add(q, "zero", 0, 0, 0, NULL);
    (void)agentc_queue_add(q, "neg2", 0, -2, 0, NULL);
    (void)agentc_queue_add(q, "one", 0, 1, 0, NULL);
    (void)agentc_queue_add(q, "neg1", 0, -1, 0, NULL);
    (void)agentc_queue_add(q, "future", 5, 0, 0, NULL);
    AgcBuf t = {0};
    uint32_t fl = 0;
    AgcDuePhase p1 = agentc_queue_flush(q, 1, &t, &fl);
    check("B.flush_preempt", p1 == AGENTC_DUE_PREEMPT);
    check("B.preempt_text", agentc_streq((char *)t.p, "neg2\nneg1"));
    AgcDuePhase p2 = agentc_queue_flush(q, 1, &t, &fl);
    check("B.flush_boundary", p2 == AGENTC_DUE_BOUNDARY);
    check("B.boundary_text", agentc_streq((char *)t.p, "zero\none\nfive"));
    check("B.future_held", agentc_queue_count(q) == 1);
    AgcDuePhase p3 = agentc_queue_flush(q, 1, &t, &fl);
    check("B.flush_none", p3 == AGENTC_DUE_NONE);
    AgcDuePhase p4 = agentc_queue_flush(q, 5, &t, &fl);
    check("B.absolute_turn", p4 == AGENTC_DUE_BOUNDARY && agentc_streq((char *)t.p, "future"));
    check("B.empty", agentc_queue_count(q) == 0);
    agentc_buf_free(&t);
    agentc_actor_release_all();
}

static void test_queue_no_coalesce(void) {
    agentc_actor_init();
    AgcActorId q = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "q");
    (void)agentc_queue_add(q, "n2", 0, -2, AGENTC_Q_F_NO_COALESCE, NULL);
    (void)agentc_queue_add(q, "n1", 0, -1, AGENTC_Q_F_NO_COALESCE, NULL);
    AgcBuf t = {0};
    uint32_t fl = 0;
    AgcDuePhase p1 = agentc_queue_flush(q, 1, &t, &fl);
    check("B.nc_first", p1 == AGENTC_DUE_PREEMPT && agentc_streq((char *)t.p, "n2") &&
                            (fl & AGENTC_Q_F_NO_COALESCE) != 0);
    AgcDuePhase p2 = agentc_queue_flush(q, 1, &t, &fl);
    check("B.nc_second", p2 == AGENTC_DUE_PREEMPT && agentc_streq((char *)t.p, "n1"));
    agentc_buf_free(&t);
    agentc_actor_release_all();
}

static void test_queue_crud(void) {
    agentc_actor_init();
    AgcActorId q = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "q");
    uint64_t a = agentc_queue_add(q, "aaa", 0, 0, 0, "cli");
    uint64_t b = agentc_queue_add(q, "bbb", 0, 1, 0, NULL);
    uint64_t c = agentc_queue_add(q, "ccc", 0, 2, 0, NULL);
    check("B.add_ids", a != 0 && b != 0 && c != 0 && a < b && b < c);
    AgcQueueItem it;
    check("B.get",
          agentc_queue_get(q, b, &it) == 0 && agentc_streq(it.text, "bbb") && it.client_id == NULL);
    check("B.get_missing", agentc_queue_get(q, 9999, &it) == -2);
    check("B.update", agentc_queue_update(q, b, "BBB", 0, -1) == 0);
    check("B.update_applied",
          agentc_queue_get(q, b, &it) == 0 && agentc_streq(it.text, "BBB") && it.priority == -1);
    /* equalize priorities so reorder is observable under the documented sort */
    (void)agentc_queue_update(q, b, "BBB", 0, 0);
    (void)agentc_queue_update(q, c, "ccc", 0, 0);
    uint64_t order[3] = {c, a, b};
    check("B.reorder", agentc_queue_reorder(q, order, 3) == 0);
    int n = agentc_queue_list(q, (AgcQueueItem *)0, 0);
    check("B.reorder_count", n == 3);
    AgcQueueItem items[3];
    (void)agentc_queue_list(q, items, 3);
    check("B.reorder_applied", items[0].id == c && items[1].id == a && items[2].id == b);
    check("B.reorder_bad", agentc_queue_reorder(q, order, 0) == 0);
    check("B.start", agentc_queue_start(q, b) == 0);
    (void)agentc_queue_get(q, b, &it);
    check("B.start_applied", it.turn == 0 && it.priority <= 0);
    check("B.delete", agentc_queue_delete(q, a) == 0 && agentc_queue_count(q) == 2);
    check("B.delete_missing", agentc_queue_delete(q, a) == -2);
    check("B.pause", agentc_queue_pause(q, true) == 0 && agentc_queue_is_paused(q));
    AgcBuf t = {0};
    uint32_t fl = 0;
    check("B.paused_no_flush", agentc_queue_flush(q, 1, &t, &fl) == AGENTC_DUE_NONE);
    check("B.resume", agentc_queue_pause(q, false) == 0 && !agentc_queue_is_paused(q));
    check("B.clear", agentc_queue_clear(q) == 0 && agentc_queue_count(q) == 0);
    agentc_buf_free(&t);
    agentc_actor_release_all();
}

static void test_queue_overflow(void) {
    agentc_actor_init();
    AgcActorId q = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "q");
    uint64_t last = 0;
    for (int i = 0; i < AGENTC_QUEUE_MAX; i++) {
        char t[8];
        agentc_snprintf(t, sizeof t, "i%d", i);
        last = agentc_queue_add(q, t, 0, 0, 0, NULL);
    }
    check("B.full", last != 0 && agentc_queue_count(q) == AGENTC_QUEUE_MAX);
    uint64_t over = agentc_queue_add(q, "over", 0, 0, 0, NULL);
    check("B.overflow", over == 0 && agentc_queue_count(q) == AGENTC_QUEUE_MAX);
    agentc_actor_release_all();
}

static void test_queue_dedupe(void) {
    agentc_actor_init();
    AgcActorId q = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "q");
    uint64_t a = agentc_queue_add(q, "first", 0, 0, AGENTC_Q_F_DEDUPE, "k1");
    check("B.dedupe_add", a != 0 && agentc_queue_count(q) == 1);
    uint64_t dup = agentc_queue_add(q, "second", 0, 5, AGENTC_Q_F_DEDUPE, "k1");
    check("B.dedupe_hit", dup == a && agentc_queue_count(q) == 1);
    AgcQueueItem it;
    check("B.dedupe_keeps_first",
          agentc_queue_get(q, a, &it) == 0 && agentc_streq(it.text, "first"));
    check("B.dedupe_count", agentc_queue_deduped(q) == 1);
    /* the same client_id without the flag adds */
    uint64_t b = agentc_queue_add(q, "again", 0, 0, 0, "k1");
    check("B.dedupe_noflag", b != 0 && b != a && agentc_queue_count(q) == 2);
    /* a distinct client_id under the flag adds */
    uint64_t c = agentc_queue_add(q, "other", 0, 0, AGENTC_Q_F_DEDUPE, "k2");
    check("B.dedupe_distinct", c != 0 && agentc_queue_count(q) == 3);
    /* an empty/absent client_id never dedupes */
    uint64_t d = agentc_queue_add(q, "e1", 0, 0, AGENTC_Q_F_DEDUPE, "");
    uint64_t e = agentc_queue_add(q, "e2", 0, 0, AGENTC_Q_F_DEDUPE, NULL);
    check("B.dedupe_noclient", d != 0 && e != 0 && d != e && agentc_queue_count(q) == 5);
    /* at the cap a duplicate still dedupes instead of reporting full */
    (void)agentc_queue_clear(q);
    uint64_t seed = agentc_queue_add(q, "seed", 0, 0, AGENTC_Q_F_DEDUPE, "kfull");
    for (int i = 1; i < AGENTC_QUEUE_MAX; i++)
        (void)agentc_queue_add(q, "i", 0, 0, 0, NULL);
    check("B.dedupe_full_pre", seed != 0 && agentc_queue_count(q) == AGENTC_QUEUE_MAX);
    uint64_t dupfull = agentc_queue_add(q, "dup", 0, 0, AGENTC_Q_F_DEDUPE, "kfull");
    check("B.dedupe_at_cap",
          dupfull == seed && agentc_queue_count(q) == AGENTC_QUEUE_MAX);
    agentc_actor_release_all();
}

static void test_queue_turn(void) {
    agentc_actor_init();
    AgcActorId q = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "q");
    check("B.turn0", agentc_queue_turn_of(q) == 0);
    agentc_queue_set_turn(q, 7);
    check("B.turn7", agentc_queue_turn_of(q) == 7 && agentc_actor_turn(q) == 7);
    agentc_actor_release_all();
}

/* ------------------------------------------- C: agent actor + control */

typedef struct {
    int cancel_req;
    int cancelled;
    int frozen;
    int resumed;
    int attached;
    int detached;
} CtrlCount;

static CtrlCount g_ctrl;

static void ctrl_event(void *ud, int ev, const void *data) {
    (void)ud;
    (void)data;
    switch (ev) {
    case AGENTC_EV_ACTOR_CANCEL_REQUESTED:
        g_ctrl.cancel_req++;
        break;
    case AGENTC_EV_ACTOR_CANCELLED:
        g_ctrl.cancelled++;
        break;
    case AGENTC_EV_ACTOR_FROZEN:
        g_ctrl.frozen++;
        break;
    case AGENTC_EV_ACTOR_RESUMED:
        g_ctrl.resumed++;
        break;
    case AGENTC_EV_ACTOR_ATTACHED:
        g_ctrl.attached++;
        break;
    case AGENTC_EV_ACTOR_DETACHED:
        g_ctrl.detached++;
        break;
    default:
        break;
    }
}

static void ctrl_send(AgcActorId to, int kind, int scope, int stage, AgcActorId from,
                      const char *body, int prio) {
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_CONTROL;
    m.kind = kind;
    m.to = to;
    m.from = from;
    m.scope = scope;
    m.stage = stage;
    m.body = body;
    m.priority = prio;
    (void)agentc_message_send(&m);
    (void)agentc_message_drain(to, 0);
}

static void test_agent_actor(void) {
    agentc_actor_init();
    agentc_events_reset();
    agentc_memset(&g_ctrl, 0, sizeof g_ctrl);
    (void)agentc_events_subscribe(ctrl_event, NULL);

    AgcAgent *ag = agentc_agent_new(NULL, "m");
    AgcActorId id = agentc_agent_bind_actor(ag, 0, "agent");
    check("C.bound", id != 0 && agentc_agent_actor(ag) == id && agentc_actor_exists(id));
    check("C.steerable", agentc_agent_steerable(ag, NULL));
    check("C.turn_no", agentc_agent_turn_no(ag) == 0);

    /* authority: a stranger cannot enqueue; the root controller can */
    AgcActorId stranger = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "other");
    ctrl_send(id, AGENTC_MSG_INPUT, 0, 0, stranger, "denied", 0);
    check("C.authority_denied", agentc_queue_count(id) == 0);
    ctrl_send(id, AGENTC_MSG_INPUT, 0, 0, 0, "allowed", 0);
    check("C.authority_ok", agentc_queue_count(id) == 1);

    /* freeze/resume */
    ctrl_send(id, AGENTC_MSG_FREEZE, AGENTC_MSG_SCOPE_ACTOR, 0, 0, NULL, 0);
    check("C.frozen", agentc_actor_frozen(id) && g_ctrl.frozen == 1);
    ctrl_send(id, AGENTC_MSG_RESUME, AGENTC_MSG_SCOPE_ACTOR, 0, 0, NULL, 0);
    check("C.resumed", !agentc_actor_frozen(id) && g_ctrl.resumed == 1);

    /* attach/detach */
    ctrl_send(id, AGENTC_MSG_ATTACH, 0, 0, 0, NULL, 0);
    ctrl_send(id, AGENTC_MSG_DETACH, 0, 0, 0, NULL, 0);
    check("C.attach", g_ctrl.attached == 1 && g_ctrl.detached == 1);

    /* scope TURN keeps the actor alive */
    ctrl_send(id, AGENTC_MSG_CANCEL, AGENTC_MSG_SCOPE_TURN, AGENTC_MSG_STAGE_GRACEFUL, 0, NULL, 0);
    AgcActor *row = agentc_actor_get(id);
    check("C.turn_keeps_actor", row != NULL && row->state != AGENTC_ACTOR_CANCELLED);
    check("C.turn_no_release", agentc_actor_exists(id));

    /* scope ACTOR cancels once */
    ctrl_send(id, AGENTC_MSG_CANCEL, AGENTC_MSG_SCOPE_ACTOR, AGENTC_MSG_STAGE_GRACEFUL, 0, NULL, 0);
    row = agentc_actor_get(id);
    check("C.actor_cancelled", row != NULL && row->state == AGENTC_ACTOR_CANCELLED);
    check("C.cancelled_once", g_ctrl.cancel_req == 1 && g_ctrl.cancelled == 1);

    /* a second (force) cancel is idempotent */
    ctrl_send(id, AGENTC_MSG_CANCEL, AGENTC_MSG_SCOPE_ACTOR, AGENTC_MSG_STAGE_FORCE, 0, NULL, 0);
    check("C.cancel_idempotent", g_ctrl.cancel_req == 1 && g_ctrl.cancelled == 1);

    agentc_agent_free(ag);
    check("C.released", !agentc_actor_exists(id));
    agentc_actor_release_all();
    agentc_events_reset();
}

/* --------------------------------------------- D: typed actor events */

typedef struct {
    int started, queued, queue_paused, notify;
} LifeCount;

static LifeCount g_life;

static void life_event(void *ud, int ev, const void *data) {
    (void)ud;
    (void)data;
    switch (ev) {
    case AGENTC_EV_ACTOR_STARTED:
        g_life.started++;
        break;
    case AGENTC_EV_ACTOR_QUEUED:
        g_life.queued++;
        break;
    case AGENTC_EV_QUEUE_PAUSED:
        g_life.queue_paused++;
        break;
    case AGENTC_EV_ACTOR_NOTIFY:
        g_life.notify++;
        break;
    default:
        break;
    }
}

static void test_lifecycle_events(void) {
    agentc_actor_init();
    agentc_events_reset();
    agentc_memset(&g_ctrl, 0, sizeof g_ctrl);
    agentc_memset(&g_life, 0, sizeof g_life);
    (void)agentc_events_subscribe(ctrl_event, NULL);
    (void)agentc_events_subscribe(life_event, NULL);

    AgcAgent *ag = agentc_agent_new(NULL, "m");
    AgcActorId id = agentc_agent_bind_actor(ag, 0, "a");
    check("D.started", g_life.started == 1);
    (void)agentc_queue_add(id, "q", 0, 0, 0, NULL);
    check("D.queued", g_life.queued == 1);
    (void)agentc_queue_pause(id, true);
    check("D.paused", g_life.queue_paused == 1);
    (void)agentc_queue_pause(id, false);
    check("D.paused2", g_life.queue_paused == 2);
    ctrl_send(id, AGENTC_MSG_NOTIFY, 0, 0, 0, "hi", 0);
    check("D.notify", g_life.notify == 1);
    ctrl_send(id, AGENTC_MSG_ATTACH, 0, 0, 0, NULL, 0);
    ctrl_send(id, AGENTC_MSG_DETACH, 0, 0, 0, NULL, 0);
    check("D.attach", g_ctrl.attached == 1 && g_ctrl.detached == 1);
    ctrl_send(id, AGENTC_MSG_FREEZE, AGENTC_MSG_SCOPE_ACTOR, 0, 0, NULL, 0);
    ctrl_send(id, AGENTC_MSG_RESUME, AGENTC_MSG_SCOPE_ACTOR, 0, 0, NULL, 0);
    check("D.freeze", g_ctrl.frozen == 1 && g_ctrl.resumed == 1);
    ctrl_send(id, AGENTC_MSG_CANCEL, AGENTC_MSG_SCOPE_ACTOR, AGENTC_MSG_STAGE_GRACEFUL, 0, NULL, 0);
    check("D.cancel", g_ctrl.cancel_req == 1 && g_ctrl.cancelled == 1);

    agentc_agent_free(ag);
    agentc_actor_release_all();
    agentc_events_reset();
}

/* -------------------------------------- E: boundary steer + auto-start */

void agentc_agent_test_no_backoff(AgcAgent *a);

typedef struct {
    int codes[8];
    const char *bodies[8];
    size_t n, i;
} Replay;

static int replay_request(void *ud, const char *url, const char *headers, const void *body,
                          size_t body_len, int (*on_chunk)(void *, const void *, size_t), void *u,
                          int timeout_ms, AgcBuf *record) {
    (void)url;
    (void)headers;
    (void)body;
    (void)body_len;
    (void)timeout_ms;
    (void)record;
    Replay *r = ud;
    if (r->i >= r->n)
        return -5;
    int code = r->codes[r->i];
    const char *resp = r->bodies[r->i++];
    if (code != 0)
        return code;
    if (resp) {
        size_t len = agentc_strlen(resp);
        for (size_t off = 0; off < len; off += 7) {
            size_t k = len - off;
            if (k > 7)
                k = 7;
            if (on_chunk(u, resp + off, k))
                return -125;
        }
    }
    return 0;
}

static const char reply_tools[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"toolu_1\",\"name\":\"read\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"f.txt\\\"}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
    "\"usage\":{\"output_tokens\":3}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static const char reply_final[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_2\",\"usage\":{"
    "\"input_tokens\":8,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"all done\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":2}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static AgcActorId g_steer_actor;
static bool g_steer_enqueued;
static int g_steered_count;
static int g_preempt_sent;

static void preempt_event(void *ud, int ev, const void *data) {
    (void)ud;
    (void)data;
    if (ev == AGENTC_EV_ACTOR_STEERED)
        g_steered_count++;
    if (ev == AGENTC_EV_TEXT_DELTA && !g_preempt_sent && g_steer_actor != 0) {
        AgcMessage m;
        agentc_memset(&m, 0, sizeof m);
        m.class = AGENTC_MSG_CLASS_INPUT;
        m.kind = AGENTC_MSG_INPUT;
        m.to = g_steer_actor;
        m.from = 0;
        m.body = "preempt text";
        m.priority = -1;
        m.flags = AGENTC_Q_F_PREEMPT_TOOLS;
        (void)agentc_message_send(&m);
        g_preempt_sent = 1;
    }
}

static void steer_event(void *ud, int ev, const void *data) {
    (void)ud;
    (void)data;
    if (ev == AGENTC_EV_MSG_END && !g_steer_enqueued && g_steer_actor != 0) {
        (void)agentc_queue_add(g_steer_actor, "steered text", 0, 0, 0, NULL);
        g_steer_enqueued = true;
    }
    if (ev == AGENTC_EV_ACTOR_STEERED)
        g_steered_count++;
}

static bool transcript_contains(const AgcTranscript *t, const char *needle) {
    for (size_t i = 0; i < t->n; i++)
        for (size_t b = 0; b < t->msgs[i].nblocks; b++)
            if (t->msgs[i].blocks[b].text != NULL &&
                agentc_str_str(t->msgs[i].blocks[b].text, needle) != NULL)
                return true;
    return false;
}

static void test_boundary_steer(void) {
    agentc_actor_init();
    agentc_events_reset();
    (void)agentc_events_subscribe(steer_event, NULL);

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;
    AgcTransport t = {replay_request, &r, NULL};
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    g_steer_actor = agentc_agent_bind_actor(a, 0, "agent");
    g_steer_enqueued = false;
    g_steered_count = 0;

    int rc = agentc_agent_submit(a, "go");
    check("E.rc", rc == 0);
    check("E.requests", r.i == 2);
    check("E.steered_once", g_steered_count == 1);
    check("E.steered_text", transcript_contains(agentc_agent_transcript(a), "steered text"));
    check("E.turn1", agentc_agent_turn_no(a) == 1);

    /* auto-start coalesces a due item and the explicit text into turn 2 */
    (void)agentc_queue_add(g_steer_actor, "auto1", 0, 0, 0, NULL);
    r.i = 0;
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;
    int rc2 = agentc_agent_submit_auto(a, "second");
    check("E.auto_rc", rc2 == 0);
    check("E.auto_turn2", agentc_agent_turn_no(a) == 2);
    check("E.auto_text", transcript_contains(agentc_agent_transcript(a), "auto1") &&
                             transcript_contains(agentc_agent_transcript(a), "second"));

    agentc_agent_free(a);
    agentc_actor_release_all();
    agentc_events_reset();
}

static void test_preempt(void) {
    agentc_actor_init();
    agentc_events_reset();

    g_preempt_sent = 0;
    g_steered_count = 0;
    (void)agentc_events_subscribe(preempt_event, NULL);

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;
    AgcTransport t = {replay_request, &r, NULL};
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    g_steer_actor = agentc_agent_bind_actor(a, 0, "agent");
    g_steer_enqueued = true; /* disable the boundary enqueue from steer_event */
    /* a running tool process child: the preempt must cancel it graceful once */
    AgcActorId tool_child = agentc_actor_register(AGENTC_ACTOR_PROCESS, g_steer_actor, "tool");

    int rc = agentc_agent_submit(a, "go");
    check("F.rc", rc == 0);
    check("F.requests", r.i == 2);
    check("F.preempt_steered", g_steered_count == 1);
    check("F.preempt_text", transcript_contains(agentc_agent_transcript(a), "preempt text"));
    check("F.turn1", agentc_agent_turn_no(a) == 1);
    AgcActor *child = agentc_actor_get(tool_child);
    check("F.tool_cancel", child != NULL && child->cancel_stage >= 1);

    agentc_agent_free(a);
    agentc_actor_release_all();
    agentc_events_reset();
}

/* ------------------------------------------- G: queue tool adapter */

static void test_queue_tool(void) {
    agentc_actor_init();
    AgcActorId q = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "q");
    AgcTool tools[16];
    size_t n = agentc_tools_builtin(tools, 16);
    const AgcTool *qt = NULL;
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(tools[i].name, "queue"))
            qt = &tools[i];
    check("G.queue_tool", qt != NULL && qt->run != NULL && (qt->flags & AGENTC_TOOL_HIDDEN) != 0);
    char args[128];
    agentc_snprintf(args, sizeof args,
                    "{\"op\":\"add\",\"actor_id\":%u,\"text\":\"x\",\"priority\":-1}", (unsigned)q);
    AgcBuf out = {0};
    bool err = false;
    AgcToolCall call = {"t", "queue", args, NULL};
    (void)qt->run(qt, &call, &out, &err);
    agentc_buf_byte(&out, 0);
    check("G.queue_add", !err && agentc_str_str((char *)out.p, "\"id\"") != NULL);
    agentc_buf_clear(&out);
    agentc_snprintf(args, sizeof args, "{\"op\":\"list\",\"actor_id\":%u}", (unsigned)q);
    (void)qt->run(qt, &call, &out, &err);
    agentc_buf_byte(&out, 0);
    check("G.queue_list", !err && agentc_str_str((char *)out.p, "x") != NULL &&
                              agentc_str_str((char *)out.p, "-1") != NULL);
    agentc_buf_clear(&out);
    agentc_snprintf(args, sizeof args,
                    "{\"op\":\"add\",\"actor_id\":%u,\"text\":\"y\",\"client_id\":\"dup\","
                    "\"dedupe\":true}",
                    (unsigned)q);
    (void)qt->run(qt, &call, &out, &err);
    agentc_buf_byte(&out, 0);
    check("G.queue_add_first", !err && agentc_str_str((char *)out.p, "\"deduped\"") == NULL);
    agentc_buf_clear(&out);
    (void)qt->run(qt, &call, &out, &err);
    agentc_buf_byte(&out, 0);
    check("G.queue_dedupe", !err && agentc_str_str((char *)out.p, "\"deduped\":true") != NULL);
    agentc_buf_free(&out);
    agentc_actor_release_all();
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    agentc_log_set_level(4);
    agentc_events_reset();
    test_drain_order();
    test_broadcast();
    test_overflow();
    test_priority_clamp();
    test_queue_order();
    test_queue_flush();
    test_queue_no_coalesce();
    test_queue_crud();
    test_queue_overflow();
    test_queue_dedupe();
    test_queue_turn();
    test_agent_actor();
    test_lifecycle_events();
    test_boundary_steer();
    test_preempt();
    test_queue_tool();
    agentc_events_reset();
    agentc_actor_release_all();
    return fails;
}
