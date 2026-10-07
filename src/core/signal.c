/* signal.c — the bounded message bus (P6-B; see P6-SIGNALS.md §3/§8).
 *
 * One bounded mailbox per actor. send copies the envelope's strings, stamps a
 * per-sender monotonic seq and a global id, clamps the priority and applies the
 * overflow priority-drop policy. drain is the only place handlers run: it pops
 * in arrival order, dispatches to the actor handler (or a test override) and
 * never re-enters. Single-threaded and deterministic.
 */
#include "message.h"
#include "hook.h"

#include "base/limits.h"
#include "core/actor_int.h"
#include "plat.h"
#include "wire.h"

#define MSG_CLASS_NAME(c)                                                                          \
    ((c) == AGENTC_MSG_CLASS_CONTROL  ? "control"                                                  \
     : (c) == AGENTC_MSG_CLASS_INPUT  ? "input"                                                    \
     : (c) == AGENTC_MSG_CLASS_NOTIFY ? "notify"                                                   \
     : (c) == AGENTC_MSG_CLASS_HOOK   ? "hook"                                                     \
                                      : "?")

typedef struct {
    AgcMessage m;
    char *body;
    char *client_id;
    char *content_type;
    char *meta;
    bool used;
} MailSlot;

typedef struct {
    MailSlot slots[AGENTC_MSG_MAILBOX_MAX];
    size_t count;
    bool overflow_logged;
    AgcMessageHandler handler;
    void *handler_ud;
} MailBox;

static MailBox g_boxes[AGENTC_ACTOR_MAX];
static u64 g_msg_id;
static u64 g_sender_seq[AGENTC_ACTOR_MAX + 2];
static bool g_msg_inited;
static bool g_draining;

/* kind priority for the overflow policy: higher is kept. lifecycle (rank 5) is
 * never dropped. */
static int kind_rank(int kind) {
    switch (kind) {
    case AGENTC_MSG_FINISH:
    case AGENTC_MSG_FAIL:
    case AGENTC_MSG_CANCELLED:
    case AGENTC_MSG_FROZEN:
    case AGENTC_MSG_RESUMED:
        return 5; /* lifecycle: never dropped */
    case AGENTC_MSG_CANCEL:
    case AGENTC_MSG_FREEZE:
    case AGENTC_MSG_RESUME:
    case AGENTC_MSG_SETTLE:
        return 4;
    case AGENTC_MSG_INPUT:
    case AGENTC_MSG_NOTIFY:
    case AGENTC_MSG_ATTACH:
    case AGENTC_MSG_DETACH:
    case AGENTC_MSG_STARTED:
    case AGENTC_MSG_SET_MODEL:
    case AGENTC_MSG_SET_THINKING:
    case AGENTC_MSG_DECIDE:
    case AGENTC_MSG_DECIDED:
        return 3;
    case AGENTC_MSG_PROGRESS:
        return 2;
    case AGENTC_MSG_STALLED:
        return 1;
    default:
        return 3;
    }
}

static const char *kind_name(int kind) {
    switch (kind) {
    case AGENTC_MSG_STARTED:
        return "started";
    case AGENTC_MSG_PROGRESS:
        return "progress";
    case AGENTC_MSG_STALLED:
        return "stalled";
    case AGENTC_MSG_INPUT:
        return "input";
    case AGENTC_MSG_NOTIFY:
        return "notify";
    case AGENTC_MSG_FINISH:
        return "finish";
    case AGENTC_MSG_FAIL:
        return "fail";
    case AGENTC_MSG_CANCEL:
        return "cancel";
    case AGENTC_MSG_CANCELLED:
        return "cancelled";
    case AGENTC_MSG_FREEZE:
        return "freeze";
    case AGENTC_MSG_RESUME:
        return "resume";
    case AGENTC_MSG_FROZEN:
        return "frozen";
    case AGENTC_MSG_RESUMED:
        return "resumed";
    case AGENTC_MSG_ATTACH:
        return "attach";
    case AGENTC_MSG_DETACH:
        return "detach";
    case AGENTC_MSG_DECIDE:
        return "decide";
    case AGENTC_MSG_DECIDED:
        return "decided";
    case AGENTC_MSG_SET_MODEL:
        return "set_model";
    case AGENTC_MSG_SET_THINKING:
        return "set_thinking";
    case AGENTC_MSG_SETTLE:
        return "settle";
    case AGENTC_MSG_REALTIME_START:
        return "realtime_start";
    case AGENTC_MSG_REALTIME_STOP:
        return "realtime_stop";
    default:
        return "?";
    }
}
static int kind_from_name(const char *s) {
    for (int k = 0; k <= AGENTC_MSG_REALTIME_STOP; k++)
        if (agentc_streq(s ? s : "", kind_name(k)))
            return k;
    return -1;
}
static int class_from_name(const char *s) {
    if (agentc_streq(s ? s : "", "control"))
        return AGENTC_MSG_CLASS_CONTROL;
    if (agentc_streq(s ? s : "", "input"))
        return AGENTC_MSG_CLASS_INPUT;
    if (agentc_streq(s ? s : "", "notify"))
        return AGENTC_MSG_CLASS_NOTIFY;
    if (agentc_streq(s ? s : "", "hook"))
        return AGENTC_MSG_CLASS_HOOK;
    return 0;
}
static const char *scope_name(int scope) {
    switch (scope) {
    case AGENTC_MSG_SCOPE_TURN:
        return "turn";
    case AGENTC_MSG_SCOPE_ACTOR:
        return "actor";
    case AGENTC_MSG_SCOPE_SUBTREE:
        return "subtree";
    case AGENTC_MSG_SCOPE_RUNTIME:
        return "runtime";
    default:
        return "none";
    }
}
static int scope_from_name(const char *s) {
    if (agentc_streq(s ? s : "", "turn"))
        return AGENTC_MSG_SCOPE_TURN;
    if (agentc_streq(s ? s : "", "actor"))
        return AGENTC_MSG_SCOPE_ACTOR;
    if (agentc_streq(s ? s : "", "subtree"))
        return AGENTC_MSG_SCOPE_SUBTREE;
    if (agentc_streq(s ? s : "", "runtime"))
        return AGENTC_MSG_SCOPE_RUNTIME;
    return AGENTC_MSG_SCOPE_NONE;
}
static const char *stage_name(int stage) {
    switch (stage) {
    case AGENTC_MSG_STAGE_GRACEFUL:
        return "graceful";
    case AGENTC_MSG_STAGE_FORCE:
        return "force";
    default:
        return "none";
    }
}
static int stage_from_name(const char *s) {
    if (agentc_streq(s ? s : "", "graceful"))
        return AGENTC_MSG_STAGE_GRACEFUL;
    if (agentc_streq(s ? s : "", "force"))
        return AGENTC_MSG_STAGE_FORCE;
    return AGENTC_MSG_STAGE_NONE;
}

const char *agentc_msg_err_name(int err) {
    switch (err) {
    case -3:
        return "actor_not_found";
    case -28:
        return "queue_full";
    case -2:
        return "queue_item_not_found";
    case -11:
        return "active_turn_not_steerable";
    case -114:
        return "turn_settled";
    case -1:
        return "authority_denied";
    case -110:
        return "hook_timeout";
    case -40:
        return "hook_depth";
    case -16:
        return "hook_reentry";
    case -22:
        return "hook_malformed";
    case -115:
        return "hook_deferred";
    case -95:
        return "unsupported";
    case -38:
        return "not_implemented";
    default:
        return "error";
    }
}

/* --------------------------------------------------------------- slots */

static void slot_free(MailSlot *s) {
    agentc_free(s->body);
    agentc_free(s->client_id);
    agentc_free(s->content_type);
    agentc_free(s->meta);
    agentc_memset(s, 0, sizeof *s);
}

static void slot_set(MailSlot *s, const AgcMessage *m) {
    agentc_memset(s, 0, sizeof *s);
    s->m = *m;
    if (m->body != NULL) {
        s->body = agentc_strdup_len(m->body, m->body_len);
        s->m.body = s->body;
    }
    if (m->client_id != NULL) {
        s->client_id = agentc_strdup(m->client_id);
        s->m.client_id = s->client_id;
    }
    if (m->content_type != NULL) {
        s->content_type = agentc_strdup(m->content_type);
        s->m.content_type = s->content_type;
    }
    if (m->meta != NULL) {
        s->meta = agentc_strdup(m->meta);
        s->m.meta = s->meta;
    }
    s->used = true;
}

static MailBox *box_of(AgcActorId id) {
    if (id == 0 || id > AGENTC_ACTOR_MAX)
        return NULL;
    return &g_boxes[id - 1];
}

static int enqueue_one(AgcActorId to, const AgcMessage *stamped) {
    MailBox *b = box_of(to);
    if (b == NULL)
        return -3;
    if (b->count < AGENTC_MSG_MAILBOX_MAX) {
        slot_set(&b->slots[b->count], stamped);
        b->count++;
        return 0;
    }
    int in_rank = kind_rank(stamped->kind);
    int drop = -1, drop_rank = 99;
    for (size_t i = 0; i < b->count; i++) {
        int r = kind_rank(b->slots[i].m.kind);
        if (r <= 4 && r < drop_rank) {
            drop_rank = r;
            drop = (int)i;
        }
    }
    if (in_rank >= 5) { /* lifecycle: never dropped */
        if (drop >= 0) {
            slot_free(&b->slots[drop]);
            slot_set(&b->slots[drop], stamped);
            return 0;
        }
        if (!b->overflow_logged) {
            b->overflow_logged = true;
            agentc_logf(2, "message: mailbox full for actor %u; lifecycle dropped", (unsigned)to);
        }
        return -28; /* ENOSPC */
    }
    if (drop >= 0 && drop_rank < in_rank) {
        slot_free(&b->slots[drop]);
        slot_set(&b->slots[drop], stamped);
        return 0;
    }
    if (!b->overflow_logged) {
        b->overflow_logged = true;
        agentc_logf(2, "message: mailbox full for actor %u; dropping %s", (unsigned)to,
                    kind_name(stamped->kind));
    }
    return 0; /* priority drop */
}

/* ---------------------------------------------------------------- init */

void agentc_message_init(void) {
    for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
        MailBox *b = &g_boxes[i];
        for (size_t k = 0; k < b->count; k++)
            slot_free(&b->slots[k]);
        agentc_memset(b, 0, sizeof *b);
    }
    agentc_memset(g_sender_seq, 0, sizeof g_sender_seq);
    g_msg_id = 0;
    g_draining = false;
    g_msg_inited = true;
}

void agentc_message_clear(AgcActorId id) {
    MailBox *b = box_of(id);
    if (b == NULL)
        return;
    for (size_t k = 0; k < b->count; k++)
        slot_free(&b->slots[k]);
    agentc_memset(b, 0, sizeof *b);
}

void agentc_message_set_handler(AgcActorId id, AgcMessageHandler fn, void *ud) {
    MailBox *b = box_of(id);
    if (b == NULL)
        return;
    b->handler = fn;
    b->handler_ud = ud;
}

/* ---------------------------------------------------------------- send */

int agentc_message_send(const AgcMessage *m) {
    if (!m)
        return -22;
    if (!g_msg_inited)
        agentc_message_init();

    AgcMessage stamped = *m;
    if (stamped.priority < AGENTC_PRIO_MIN)
        stamped.priority = AGENTC_PRIO_MIN;
    if (stamped.priority > AGENTC_PRIO_MAX)
        stamped.priority = AGENTC_PRIO_MAX;
    if (stamped.body != NULL && stamped.body_len == 0)
        stamped.body_len = agentc_strlen(stamped.body);
    stamped.id = ++g_msg_id;
    u32 from = stamped.from <= AGENTC_ACTOR_MAX ? stamped.from : 0;
    stamped.seq = ++g_sender_seq[from];
    stamped.ts_ms = os_now_ns(OS_CLOCK_REALTIME) / 1000000;

    if (stamped.to == 0) {
        /* Broadcast is only valid for CANCEL (SIG §3): deliver in table order. */
        if (stamped.kind != AGENTC_MSG_CANCEL)
            return -22;
        int rc = 0;
        for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
            if (g_boxes[i].count == 0 && !agentc_actor_exists((AgcActorId)(i + 1)))
                continue;
            if (!agentc_actor_exists((AgcActorId)(i + 1)))
                continue;
            int r = enqueue_one((AgcActorId)(i + 1), &stamped);
            if (r < 0)
                rc = r;
        }
        return rc;
    }
    if (!agentc_actor_exists(stamped.to))
        return -3; /* ESRCH */
    return enqueue_one(stamped.to, &stamped);
}

/* ---------------------------------------------------------------- drain */

static size_t drain_box(AgcActorId to, size_t max) {
    MailBox *b = box_of(to);
    size_t n = 0;
    while (b != NULL && b->count > 0 && n < max) {
        MailSlot *s = &b->slots[0];
        agentc_hook_stage_emit("signal", kind_name(s->m.kind), s->m.body, to);
        if (b->handler != NULL) {
            (void)b->handler(b->handler_ud, to, &s->m);
        } else {
            (void)actor_handle_message(to, &s->m);
        }
        slot_free(s);
        if (b->count > 1)
            agentc_memmove(&b->slots[0], &b->slots[1], (b->count - 1) * sizeof(MailSlot));
        b->count--;
        agentc_memset(&b->slots[b->count], 0, sizeof(MailSlot));
        n++;
    }
    return n;
}

size_t agentc_message_drain(AgcActorId id, size_t max) {
    if (!g_msg_inited)
        return 0;
    if (g_draining)
        return 0; /* never re-enter */
    if (max == 0 || max > AGENTC_MSG_DRAIN_MAX)
        max = AGENTC_MSG_DRAIN_MAX;
    g_draining = true;
    size_t delivered = 0;
    if (id != 0) {
        delivered = drain_box(id, max);
    } else {
        for (size_t i = 0; i < AGENTC_ACTOR_MAX && delivered < max; i++)
            delivered += drain_box((AgcActorId)(i + 1), max - delivered);
    }
    g_draining = false;
    return delivered;
}

size_t agentc_message_pending(AgcActorId id) {
    if (!g_msg_inited)
        return 0;
    if (id != 0) {
        MailBox *b = box_of(id);
        return b != NULL ? b->count : 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++)
        n += g_boxes[i].count;
    return n;
}

int agentc_message_cancel_subtree(AgcActorId id, int stage) {
    if (!agentc_actor_exists(id))
        return -3;
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_CONTROL;
    m.kind = AGENTC_MSG_CANCEL;
    m.from = id;
    m.to = id;
    m.scope = AGENTC_MSG_SCOPE_SUBTREE;
    m.stage = stage == 2 ? AGENTC_MSG_STAGE_FORCE : AGENTC_MSG_STAGE_GRACEFUL;
    return agentc_message_send(&m);
}

/* ----------------------------------------------------------- JSON codec */

int agentc_message_to_json(const AgcMessage *m, AgcBuf *out) {
    if (!m || !out)
        return -22;
    AgcJsonW w;
    agentc_jsonw_init(&w, out);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "seq");
    agentc_jsonw_u64(&w, m->seq);
    agentc_jsonw_key(&w, "from");
    agentc_jsonw_u64(&w, m->from);
    agentc_jsonw_key(&w, "to");
    agentc_jsonw_u64(&w, m->to);
    agentc_jsonw_key(&w, "class");
    agentc_jsonw_cstr(&w, MSG_CLASS_NAME(m->class));
    agentc_jsonw_key(&w, "kind");
    agentc_jsonw_cstr(&w, kind_name(m->kind));
    agentc_jsonw_key(&w, "turn");
    agentc_jsonw_i64(&w, m->turn);
    agentc_jsonw_key(&w, "priority");
    agentc_jsonw_i64(&w, m->priority);
    agentc_jsonw_key(&w, "flags");
    agentc_jsonw_u64(&w, m->flags);
    agentc_jsonw_key(&w, "scope");
    agentc_jsonw_cstr(&w, scope_name(m->scope));
    agentc_jsonw_key(&w, "stage");
    agentc_jsonw_cstr(&w, stage_name(m->stage));
    if (m->client_id) {
        agentc_jsonw_key(&w, "client_id");
        agentc_jsonw_cstr(&w, m->client_id);
    }
    agentc_jsonw_key(&w, "actor_id");
    agentc_jsonw_u64(&w, m->actor_id);
    agentc_jsonw_key(&w, "depth");
    agentc_jsonw_u64(&w, m->depth);
    agentc_jsonw_key(&w, "id");
    agentc_jsonw_u64(&w, m->id);
    agentc_jsonw_key(&w, "direction");
    agentc_jsonw_u64(&w, m->direction);
    agentc_jsonw_key(&w, "final");
    agentc_jsonw_bool(&w, m->final);
    if (m->body) {
        agentc_jsonw_key(&w, "body");
        agentc_jsonw_str(&w, m->body, m->body_len);
    }
    if (m->content_type) {
        agentc_jsonw_key(&w, "content_type");
        agentc_jsonw_cstr(&w, m->content_type);
    }
    if (m->meta) {
        agentc_jsonw_key(&w, "meta");
        agentc_jsonw_cstr(&w, m->meta);
    }
    agentc_jsonw_key(&w, "ts_ms");
    agentc_jsonw_i64(&w, m->ts_ms);
    agentc_jsonw_end(&w);
    return 0;
}

int agentc_message_from_json(const char *json, size_t n, AgcMessage *out) {
    if (!json || !out)
        return -22;
    agentc_memset(out, 0, sizeof *out);
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *o = agentc_json_parse_in(a, json, n);
    if (agentc_json_type(o) != AGENTC_JSON_OBJ) {
        agentc_json_arena_free(a);
        return -22;
    }
    out->seq = (uint64_t)agentc_json_get_int(o, "seq", 0);
    out->from = (uint32_t)agentc_json_get_int(o, "from", 0);
    out->to = (uint32_t)agentc_json_get_int(o, "to", 0);
    out->class = (uint8_t)class_from_name(agentc_json_get_str(o, "class"));
    int k = kind_from_name(agentc_json_get_str(o, "kind"));
    out->kind = (uint16_t)(k >= 0 ? k : 0);
    out->turn = agentc_json_get_int(o, "turn", 0);
    out->priority = (int32_t)agentc_json_get_int(o, "priority", 0);
    out->flags = (uint32_t)agentc_json_get_int(o, "flags", 0);
    out->scope = scope_from_name(agentc_json_get_str(o, "scope"));
    out->stage = stage_from_name(agentc_json_get_str(o, "stage"));
    out->actor_id = (uint32_t)agentc_json_get_int(o, "actor_id", 0);
    out->depth = (uint16_t)agentc_json_get_int(o, "depth", 0);
    out->id = (uint64_t)agentc_json_get_int(o, "id", 0);
    out->direction = (uint8_t)agentc_json_get_int(o, "direction", 0);
    out->final = agentc_json_get_bool(o, "final", false);
    out->ts_ms = agentc_json_get_int(o, "ts_ms", 0);

    const char *s = agentc_json_get_str(o, "client_id");
    if (s)
        out->client_id = agentc_strdup(s);
    s = agentc_json_get_str(o, "content_type");
    out->content_type = agentc_strdup(s ? s : "text/plain");
    s = agentc_json_get_str(o, "meta");
    if (s)
        out->meta = agentc_strdup(s);
    const AgcJson *body = agentc_json_get(o, "body");
    size_t blen = 0;
    const char *bs = agentc_json_str(body, &blen);
    if (bs) {
        out->body = agentc_strdup_len(bs, blen);
        out->body_len = blen;
    }
    agentc_json_arena_free(a);
    return 0;
}

void agentc_message_free(AgcMessage *m) {
    if (!m)
        return;
    agentc_free((void *)m->body);
    agentc_free((void *)m->client_id);
    agentc_free((void *)m->content_type);
    agentc_free((void *)m->meta);
    agentc_memset(m, 0, sizeof *m);
}
