/* queue.c — the unified per-agent queue (P6-B; see P6-SIGNALS.md §5).
 *
 * One queue per agent actor. An item is `(turn, priority, enqueued_ms, id)`:
 * `turn` decides which turn (0 = auto), the priority sign decides preempt
 * (< 0) vs boundary (>= 0) and the magnitude orders within a phase. The queue
 * is bounded by AGENTC_QUEUE_MAX; add rejects at the cap (return 0). Single
 * threaded; each mutation emits AGENTC_EV_ACTOR_QUEUED (or _QUEUE_PAUSED).
 */
#include "message.h"

#include "base/limits.h"
#include "core/actor_int.h"
#include "core/events.h"

typedef struct {
    AgcQueueItem items[AGENTC_QUEUE_MAX];
    size_t count;
    bool paused;
    u64 next_id;
    u64 deduped; /* appended P7: enqueues dropped by client_id dedupe */
} Queue;

static Queue g_queues[AGENTC_ACTOR_MAX];
static bool g_queue_inited;
static i64 g_queue_tick; /* logical enqueue clock (deterministic) */

static Queue *queue_of(AgcActorId id) {
    if (id == 0 || id > AGENTC_ACTOR_MAX)
        return NULL;
    if (!agentc_actor_exists(id))
        return NULL;
    return &g_queues[id - 1];
}

static void item_free(AgcQueueItem *it) {
    agentc_free(it->client_id);
    agentc_free(it->text);
    agentc_memset(it, 0, sizeof *it);
}

static void queue_emit(AgcActorId id, int op, const AgcQueueItem *it) {
    AgcActor *a = agentc_actor_get(id);
    AgcQueueEvent e;
    agentc_memset(&e, 0, sizeof e);
    e.actor_id = id;
    e.op = op;
    if (it != NULL) {
        e.item_id = it->id;
        e.turn = it->turn;
        e.priority = it->priority;
    }
    e.count = a != NULL ? g_queues[id - 1].count : 0;
    agentc_events_emit(a != NULL ? a->agent : NULL, AGENTC_EV_ACTOR_QUEUED, &e);
    if (op == 6)
        agentc_events_emit(a != NULL ? a->agent : NULL, AGENTC_EV_QUEUE_PAUSED, &e);
}

void agentc_queue_init(void) {
    for (size_t i = 0; i < AGENTC_ACTOR_MAX; i++) {
        for (size_t k = 0; k < g_queues[i].count; k++)
            item_free(&g_queues[i].items[k]);
        agentc_memset(&g_queues[i], 0, sizeof g_queues[i]);
    }
    g_queue_tick = 0;
    g_queue_inited = true;
}

int agentc_queue_clear(AgcActorId id) {
    Queue *q = queue_of(id);
    if (q == NULL)
        return -3;
    for (size_t k = 0; k < q->count; k++)
        item_free(&q->items[k]);
    q->count = 0;
    /* P8-D4: clearing drops the pause too, so a paused queue does not leak its
     * paused state into a fresh/cleared one. */
    q->paused = false;
    queue_emit(id, 5, NULL);
    return 0;
}

/* --------------------------------------------------------------- mutators */

uint64_t agentc_queue_add(AgcActorId id, const char *text, int64_t turn, int32_t priority,
                          uint32_t flags, const char *client_id) {
    if (!g_queue_inited)
        agentc_queue_init();
    Queue *q = queue_of(id);
    if (q == NULL)
        return 0;
    /* Appended P7: idempotent enqueue. Scan the not-yet-delivered queue before
     * the cap check so a full queue still dedupes instead of reporting full;
     * first-write-wins (the existing item keeps its schedule). */
    if ((flags & AGENTC_Q_F_DEDUPE) && client_id && client_id[0]) {
        for (size_t i = 0; i < q->count; i++)
            if (q->items[i].client_id && agentc_streq(q->items[i].client_id, client_id)) {
                q->deduped++;
                return q->items[i].id;
            }
    }
    if (q->count >= AGENTC_QUEUE_MAX)
        return 0; /* queue_full (-ENOSPC via adapter) */
    if (priority < AGENTC_PRIO_MIN)
        priority = AGENTC_PRIO_MIN;
    if (priority > AGENTC_PRIO_MAX)
        priority = AGENTC_PRIO_MAX;
    AgcQueueItem *it = &q->items[q->count];
    agentc_memset(it, 0, sizeof *it);
    it->id = ++q->next_id;
    it->turn = turn;
    it->priority = priority;
    it->flags = flags;
    it->text = agentc_strdup(text ? text : "");
    it->client_id = client_id ? agentc_strdup(client_id) : NULL;
    it->enqueued_ms = ++g_queue_tick;
    q->count++;
    queue_emit(id, 0, it);
    return it->id;
}

size_t agentc_queue_deduped(AgcActorId id) {
    Queue *q = queue_of(id);
    return q ? (size_t)q->deduped : 0;
}

bool agentc_queue_has_client(AgcActorId id, const char *client_id) {
    Queue *q = queue_of(id);
    if (q == NULL || client_id == NULL || client_id[0] == 0)
        return false;
    for (size_t i = 0; i < q->count; i++)
        if (q->items[i].client_id && agentc_streq(q->items[i].client_id, client_id))
            return true;
    return false;
}

/* Stable sort indices by (turn, priority, enqueued_ms, id). */
static void sorted_ids(Queue *q, size_t *idx) {
    for (size_t i = 0; i < q->count; i++)
        idx[i] = i;
    for (size_t i = 1; i < q->count; i++) {
        size_t key = idx[i];
        const AgcQueueItem *k = &q->items[key];
        size_t j = i;
        while (j > 0) {
            const AgcQueueItem *a = &q->items[idx[j - 1]];
            bool before =
                a->turn < k->turn ||
                (a->turn == k->turn && (a->priority < k->priority ||
                                        (a->priority == k->priority &&
                                         (a->enqueued_ms < k->enqueued_ms ||
                                          (a->enqueued_ms == k->enqueued_ms && a->id < k->id)))));
            if (before)
                break;
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = key;
    }
}

int agentc_queue_list(AgcActorId id, AgcQueueItem *out, size_t max) {
    Queue *q = queue_of(id);
    if (q == NULL)
        return -3;
    if (out == NULL)
        return (int)q->count;
    size_t idx[AGENTC_QUEUE_MAX];
    sorted_ids(q, idx);
    size_t k = 0;
    for (size_t i = 0; i < q->count && k < max; i++)
        out[k++] = q->items[idx[i]];
    return (int)k;
}

static AgcQueueItem *find_item(Queue *q, uint64_t item, size_t *out_idx) {
    for (size_t i = 0; i < q->count; i++) {
        if (q->items[i].id == item) {
            if (out_idx)
                *out_idx = i;
            return &q->items[i];
        }
    }
    return NULL;
}

int agentc_queue_get(AgcActorId id, uint64_t item, AgcQueueItem *out) {
    Queue *q = queue_of(id);
    if (q == NULL)
        return -3;
    AgcQueueItem *it = find_item(q, item, NULL);
    if (it == NULL || out == NULL)
        return -2;
    *out = *it;
    return 0;
}

size_t agentc_queue_count(AgcActorId id) {
    Queue *q = queue_of(id);
    return q != NULL ? q->count : 0;
}

int agentc_queue_update(AgcActorId id, uint64_t item, const char *text, int64_t turn,
                        int32_t priority) {
    Queue *q = queue_of(id);
    if (q == NULL)
        return -3;
    AgcQueueItem *it = find_item(q, item, NULL);
    if (it == NULL)
        return -2;
    if (text != NULL) {
        agentc_free(it->text);
        it->text = agentc_strdup(text);
    }
    if (turn >= 0)
        it->turn = turn;
    if (priority < AGENTC_PRIO_MIN)
        priority = AGENTC_PRIO_MIN;
    if (priority > AGENTC_PRIO_MAX)
        priority = AGENTC_PRIO_MAX;
    it->priority = priority;
    queue_emit(id, 1, it);
    return 0;
}

int agentc_queue_delete(AgcActorId id, uint64_t item) {
    Queue *q = queue_of(id);
    if (q == NULL)
        return -3;
    size_t idx = 0;
    AgcQueueItem *it = find_item(q, item, &idx);
    if (it == NULL)
        return -2;
    uint64_t iid = it->id;
    item_free(it);
    if (idx + 1 < q->count)
        agentc_memmove(&q->items[idx], &q->items[idx + 1],
                       (q->count - idx - 1) * sizeof(AgcQueueItem));
    q->count--;
    agentc_memset(&q->items[q->count], 0, sizeof(AgcQueueItem));
    AgcQueueItem tmp;
    agentc_memset(&tmp, 0, sizeof tmp);
    tmp.id = iid;
    queue_emit(id, 2, &tmp);
    return 0;
}

int agentc_queue_reorder(AgcActorId id, const uint64_t *ids, size_t n) {
    Queue *q = queue_of(id);
    if (q == NULL)
        return -3;
    if (ids == NULL && n > 0)
        return -22;
    AgcQueueItem tmp[AGENTC_QUEUE_MAX];
    bool taken[AGENTC_QUEUE_MAX];
    agentc_memset(taken, 0, sizeof taken);
    size_t k = 0;
    for (size_t i = 0; i < n && k < q->count; i++) {
        size_t idx = 0;
        AgcQueueItem *it = find_item(q, ids[i], &idx);
        if (it == NULL)
            return -2;
        if (taken[idx])
            continue;
        taken[idx] = true;
        tmp[k++] = *it;
    }
    for (size_t i = 0; i < q->count; i++)
        if (!taken[i])
            tmp[k++] = q->items[i];
    /* Re-stamp the arrival order so the documented (turn,priority,enqueued_ms,id)
     * sort reflects the requested order. */
    for (size_t i = 0; i < k; i++)
        tmp[i].enqueued_ms = ++g_queue_tick;
    agentc_memcpy(q->items, tmp, k * sizeof(AgcQueueItem));
    queue_emit(id, 3, NULL);
    return 0;
}

int agentc_queue_start(AgcActorId id, uint64_t item) {
    Queue *q = queue_of(id);
    if (q == NULL)
        return -3;
    AgcQueueItem *it = find_item(q, item, NULL);
    if (it == NULL)
        return -2;
    it->turn = 0;
    if (it->priority > 0)
        it->priority = 0;
    queue_emit(id, 4, it);
    return 0;
}

int agentc_queue_pause(AgcActorId id, bool paused) {
    Queue *q = queue_of(id);
    if (q == NULL)
        return -3;
    q->paused = paused;
    queue_emit(id, 6, NULL);
    return 0;
}

bool agentc_queue_is_paused(AgcActorId id) {
    Queue *q = queue_of(id);
    return q != NULL && q->paused;
}

int64_t agentc_queue_turn_of(AgcActorId id) {
    return agentc_actor_turn(id);
}

void agentc_queue_set_turn(AgcActorId id, int64_t turn_seq) {
    AgcActor *a = agentc_actor_get(id);
    if (a != NULL)
        a->turn_seq = turn_seq;
}

/* ---------------------------------------------------------------- flush */

/* True when the item targets the active turn: turn==0 (auto/current) or an
 * absolute turn <= active_turn. */
static bool due(const AgcQueueItem *it, int64_t active_turn) {
    if (it->turn == 0)
        return true;
    return it->turn <= active_turn;
}

AgcDuePhase agentc_queue_flush(AgcActorId id, int64_t active_turn, AgcBuf *out_text,
                               uint32_t *out_flags) {
    Queue *q = queue_of(id);
    if (out_text)
        agentc_buf_clear(out_text);
    if (out_flags)
        *out_flags = 0;
    if (q == NULL || q->count == 0 || q->paused)
        return AGENTC_DUE_NONE;

    size_t idx[AGENTC_QUEUE_MAX];
    sorted_ids(q, idx);

    /* Split into preempt (<0) and boundary (>=0) blocks, both sorted. */
    size_t pre[AGENTC_QUEUE_MAX], bnd[AGENTC_QUEUE_MAX];
    size_t np = 0, nb = 0;
    for (size_t i = 0; i < q->count; i++) {
        AgcQueueItem *it = &q->items[idx[i]];
        if (!due(it, active_turn))
            continue;
        if (it->priority < 0)
            pre[np++] = idx[i];
        else
            bnd[nb++] = idx[i];
    }
    if (np == 0 && nb == 0)
        return AGENTC_DUE_NONE;

    size_t *block = np > 0 ? pre : bnd;
    size_t bn = np > 0 ? np : nb;
    AgcDuePhase phase = np > 0 ? AGENTC_DUE_PREEMPT : AGENTC_DUE_BOUNDARY;

    AgcQueueItem *first = &q->items[block[0]];
    if (out_flags)
        *out_flags = first->flags;
    bool no_coalesce = (first->flags & AGENTC_Q_F_NO_COALESCE) != 0;

    /* Which physical indices to remove: one item for NO_COALESCE, the whole
     * block otherwise. */
    size_t chosen[AGENTC_QUEUE_MAX];
    size_t nc = no_coalesce ? 1 : bn;
    for (size_t i = 0; i < nc; i++)
        chosen[i] = block[i];
    /* Remove in descending physical order so earlier removals do not shift the
     * still-pending indices: sort ascending, then walk the array from the end. */
    for (size_t i = 1; i < nc; i++) {
        size_t key = chosen[i];
        size_t j = i;
        while (j > 0 && chosen[j - 1] > key) {
            chosen[j] = chosen[j - 1];
            j--;
        }
        chosen[j] = key;
    }
    if (no_coalesce) {
        if (out_text)
            agentc_buf_cstr(out_text, first->text ? first->text : "");
    } else {
        for (size_t i = 0; i < bn; i++) {
            AgcQueueItem *it = &q->items[block[i]];
            if (i > 0 && out_text)
                agentc_buf_byte(out_text, '\n');
            if (out_text)
                agentc_buf_cstr(out_text, it->text ? it->text : "");
        }
    }

    /* Remove the selected physical indices (descending so shifts stay valid). */
    for (size_t i = nc; i > 0; i--) {
        size_t pos = chosen[i - 1];
        item_free(&q->items[pos]);
        if (pos + 1 < q->count)
            agentc_memmove(&q->items[pos], &q->items[pos + 1],
                           (q->count - pos - 1) * sizeof(AgcQueueItem));
        q->count--;
        agentc_memset(&q->items[q->count], 0, sizeof(AgcQueueItem));
    }
    queue_emit(id, 2, NULL);
    return phase;
}
