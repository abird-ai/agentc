/* message.h — the runtime control/notify bus and the unified queue (P6-B).
 *
 * `AgcMessage` is the CONTROL + INPUT + NOTIFY + HOOK class of the one Message
 * primitive; `agentc_queue_add` is a convenience over the same envelope (see
 * .agents/plans/P6-PRIMITIVES.md §3, P6-SIGNALS.md). Every message is bounded
 * and addressed (`from`/`to`/`id`); one bounded mailbox per actor; drain is
 * deterministic (per-sender seq + global arrival order). Single-threaded; a
 * handler may synthesize at most one downstream message.
 *
 * One queue per agent actor: an item is `(turn, priority, enqueued_ms, id)`.
 * `turn` decides which turn; the priority sign decides preempt (< 0) vs
 * boundary (>= 0) and the magnitude orders within a phase.
 *
 * Owners: src/core/signal.c (bus), src/core/queue.c (queue).
 */
#ifndef AGENTC_MESSAGE_H
#define AGENTC_MESSAGE_H

#include "actor.h"

/* envelope class (P6-PRIMITIVES §3) */
enum {
    AGENTC_MSG_CLASS_CONTROL = 0,
    AGENTC_MSG_CLASS_INPUT,
    AGENTC_MSG_CLASS_NOTIFY,
    AGENTC_MSG_CLASS_HOOK,
};

typedef enum { /* SIG §2; opcode = kind */
               AGENTC_MSG_STARTED = 0,
               AGENTC_MSG_PROGRESS,
               AGENTC_MSG_STALLED,
               AGENTC_MSG_INPUT,
               AGENTC_MSG_NOTIFY,
               AGENTC_MSG_FINISH,
               AGENTC_MSG_FAIL,
               AGENTC_MSG_CANCEL,
               AGENTC_MSG_CANCELLED,
               AGENTC_MSG_FREEZE,
               AGENTC_MSG_RESUME,
               AGENTC_MSG_FROZEN,
               AGENTC_MSG_RESUMED,
               AGENTC_MSG_ATTACH,
               AGENTC_MSG_DETACH,
               AGENTC_MSG_DECIDE,
               AGENTC_MSG_DECIDED,
               AGENTC_MSG_SET_MODEL,
               AGENTC_MSG_SET_THINKING,
               AGENTC_MSG_SETTLE,
               /* Appended P6-E: realtime voice session control (P6-VOICE §4). */
               AGENTC_MSG_REALTIME_START,
               AGENTC_MSG_REALTIME_STOP,
} AgcMessageKind;

typedef enum {
    AGENTC_MSG_SCOPE_NONE = 0,
    AGENTC_MSG_SCOPE_TURN,
    AGENTC_MSG_SCOPE_ACTOR,
    AGENTC_MSG_SCOPE_SUBTREE,
    AGENTC_MSG_SCOPE_RUNTIME,
} AgcMsgScope;

typedef enum {
    AGENTC_MSG_STAGE_NONE = 0,
    AGENTC_MSG_STAGE_GRACEFUL,
    AGENTC_MSG_STAGE_FORCE,
} AgcMsgStage;

/* PRI §3, verbatim, plus ts_ms appended at the end. class/kind/turn/priority/
 * flags are the INPUT key; scope/stage the CONTROL key. Strings in the struct
 * are borrowed unless produced by agentc_message_from_json (then owned and
 * released by agentc_message_free). */
typedef struct AgcMessage {
    uint64_t seq;
    uint32_t from, to;
    uint8_t class; /* AGENTC_MSG_CLASS_* (field name per PRI §3) */
    uint16_t kind;
    int64_t turn;
    int32_t priority;
    uint32_t flags;
    int scope;
    int stage;
    const char *client_id;
    uint32_t actor_id;
    uint16_t depth;
    uint64_t id;
    uint8_t direction;
    bool final;
    const char *body;
    size_t body_len;
    const char *content_type;
    const char *meta;
    i64 ts_ms; /* appended P6-B */
    /* Appended P6 completion 2d (B8 fold): the mapped legacy `on(point,...)`
     * bus result expressed on the hook channel. `result_json`/`handled`/
     * `blocked` mirror `AgcExtResult`; `result_json` is owned and released with
     * `agentc_hook_result_free` (never serialized). */
    char *result_json;
    bool handled;
    bool blocked;
    /* Appended P6 completion pass 2e (B8): a core stage handler consumed the
     * flow (`agentc_hook_run` returned a NULL next), so the mapped legacy point
     * was suppressed. Distinguishes "consumed by core" from "legacy ran and
     * made no decision", which share NULL `result_json`. */
    bool consumed;
} AgcMessage;

typedef struct { /* SIG §5.1 */
    uint64_t id;
    int64_t turn;
    int32_t priority;
    uint32_t flags;
    char *client_id; /* owned by the queue */
    char *text;      /* owned by the queue */
    i64 enqueued_ms;
} AgcQueueItem;

#define AGENTC_Q_F_PREEMPT_TOOLS 0x1u
#define AGENTC_Q_F_NO_COALESCE 0x2u
#define AGENTC_Q_F_CANCEL_NONSTEERABLE 0x4u
/* Appended P7: idempotent enqueue. When set with a non-empty `client_id`, an
 * item whose `client_id` is already queued (not yet delivered) is not added
 * again; `agentc_queue_add` returns the existing item's id. */
#define AGENTC_Q_F_DEDUPE 0x8u
#define AGENTC_PRIO_MIN (-20)
#define AGENTC_PRIO_MAX (19)
#define AGENTC_PRIO_PREEMPT (-1)
#define AGENTC_PRIO_BOUNDARY (0)

/* A per-actor handler. Return 0 to continue; negative is logged. The default
 * dispatch (actor.c) drives process/agent behavior; a test may override it with
 * agentc_message_set_handler. */
typedef int (*AgcMessageHandler)(void *ud, AgcActorId to, const AgcMessage *m);

/* ------------------------------------------------------------------ bus */
void agentc_message_init(void);
int agentc_message_send(const AgcMessage *m);           /* 0 | -ENOSPC | -ESRCH | -EINVAL */
size_t agentc_message_drain(AgcActorId id, size_t max); /* 0 id = all actors */
size_t agentc_message_pending(AgcActorId id);
void agentc_message_clear(AgcActorId id);
int agentc_message_to_json(const AgcMessage *m, AgcBuf *out);
int agentc_message_from_json(const char *json, size_t n, AgcMessage *out);
void agentc_message_free(AgcMessage *m);
int agentc_message_cancel_subtree(AgcActorId id, int stage);
/* test/agent seam: an override replaces the default handler for one actor. */
void agentc_message_set_handler(AgcActorId id, AgcMessageHandler fn, void *ud);

/* ---------------------------------------------------------------- queue */
uint64_t agentc_queue_add(AgcActorId id, const char *text, int64_t turn, int32_t priority,
                          uint32_t flags, const char *client_id);
/* Cumulative count of enqueues dropped by a `client_id` match under
 * AGENTC_Q_F_DEDUPE (appended P7), for observability. 0 for an unknown actor. */
size_t agentc_queue_deduped(AgcActorId id);

/* True when an item with this client_id is queued and not yet delivered
 * (appended P7). The sound pre-check for AGENTC_Q_F_DEDUPE: never infer a
 * dedupe from the cumulative counter, which a re-entrant subscriber can
 * advance. */
bool agentc_queue_has_client(AgcActorId id, const char *client_id);
int agentc_queue_list(AgcActorId id, AgcQueueItem *out, size_t max); /* count-first */
int agentc_queue_get(AgcActorId id, uint64_t item, AgcQueueItem *out);
size_t agentc_queue_count(AgcActorId id);
int agentc_queue_update(AgcActorId id, uint64_t item, const char *text, int64_t turn,
                        int32_t priority);
int agentc_queue_delete(AgcActorId id, uint64_t item);
int agentc_queue_reorder(AgcActorId id, const uint64_t *ids, size_t n);
int agentc_queue_start(AgcActorId id, uint64_t item);
int agentc_queue_clear(AgcActorId id);
int agentc_queue_pause(AgcActorId id, bool paused);
bool agentc_queue_is_paused(AgcActorId id);
int64_t agentc_queue_turn_of(AgcActorId id);

/* core-only due-set helper used by the agent loop (not a public surface) */
typedef enum { AGENTC_DUE_NONE = 0, AGENTC_DUE_BOUNDARY, AGENTC_DUE_PREEMPT } AgcDuePhase;
AgcDuePhase agentc_queue_flush(AgcActorId id, int64_t active_turn, AgcBuf *out_text,
                               uint32_t *out_flags);
void agentc_queue_set_turn(AgcActorId id, int64_t turn_seq);
void agentc_queue_init(void);

/* C errno -> RPC error name (signal.c). Never NULL. */
const char *agentc_msg_err_name(int err);

#endif /* AGENTC_MESSAGE_H */
