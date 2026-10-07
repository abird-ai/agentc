/* runsession.c — shared run/session mechanics (see core/runsession.h).
 *
 * Event stream (JSONL, one object per line):
 *   session header, agent_start, turn_start, message_start, message_update
 *   (delta-only), message_end, tool_execution_start/end, turn_end, agent_end,
 *   error, compaction. Message objects carry role/ts/content/usage/stop_reason.
 *
 * Event and observer callbacks are injected through AgcModeIo so tests can
 * drive the modes with in-memory buffers; reader contract:
 *   >0  bytes read
 *    0  end of input
 *   -11  no data right now (would block)
 *   <0  error
 */
#include "core/runsession.h"

#include "core/select.h"
#include "base/limits.h"
#include "core/agent_int.h"
#include "core/events.h"
#include "core/prompt.h"
#include "plat.h"
#include "ext.h"

/* internal helper from config.c (not part of the frozen header) */
bool agentc_path_join(char *out, size_t cap, const char *dir, const char *name);

/* app wiring defined below but used by the rebuild path */
static void mode_configure_agent(AgcAgent *a, const AgcModeConfig *cfg);
/* front-end services installed by agentc_mode_setup */
static int mode_ext_set_model(void *ud, const char *provider, const char *model);
static void mode_ext_set_thinking(void *ud, const char *level);
/* P8-D1 (B-M8): the mode layer's per-agent route seam (defined below). */
static const AgcProvider *mode_route_provider(void *ud, const char *name);
static bool mode_route_target(void *ud, const char *provider, AgcRouteTarget *out);

/* The mode-layer seam installed on every root agent (and inherited by children).
 * The provider resolver is process-shared data (one setup context), but the seam
 * rides the agent so route_resolve reads no global and one ctx's teardown can
 * never clear another's resolution. */
static AgcRouteSeam mode_seam(AgcModeCtx *c) {
    AgcRouteSeam s = {mode_route_provider, c, mode_route_target, c};
    return s;
}

/* Every JSON document these front ends emit is ASCII-only: non-ASCII
 * bytes become \\uXXXX escapes. Provider request writers are separate and keep
 * raw UTF-8 (the default). */
static void jsonw_out(AgcJsonW *w, AgcBuf *b) {
    agentc_jsonw_init(w, b);
    agentc_jsonw_set_ascii(w, true);
}

bool agentc_mode_msg_persistable(const AgcMsg *m) {
    if (!m || m->role == AGENTC_ROLE_SYSTEM)
        return false;
    return !(m->role == AGENTC_ROLE_ASSISTANT &&
             (m->stop_reason == AGENTC_STOP_ERROR || m->stop_reason == AGENTC_STOP_ABORTED));
}

const char *agentc_mode_stop_name(int stop) {
    switch (stop) {
    case AGENTC_STOP_STOP:
        return "stop";
    case AGENTC_STOP_LENGTH:
        return "length";
    case AGENTC_STOP_TOOLUSE:
        return "tool_use";
    case AGENTC_STOP_ERROR:
        return "error";
    case AGENTC_STOP_ABORTED:
        return "aborted";
    default:
        return "pending";
    }
}

static const char *mode_role_name(int role) {
    switch (role) {
    case AGENTC_ROLE_USER:
        return "user";
    case AGENTC_ROLE_ASSISTANT:
        return "assistant";
    case AGENTC_ROLE_TOOL:
        return "tool";
    default:
        return "system";
    }
}

/* Serialize one AgcMsg as a JSON object (no surrounding newline). */
void agentc_mode_msg_json(AgcBuf *b, const AgcMsg *m) {
    AgcJsonW w;
    jsonw_out(&w, b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "role");
    agentc_jsonw_cstr(&w, mode_role_name(m->role));
    agentc_jsonw_key(&w, "ts");
    agentc_jsonw_u64(&w, m->ts_ms);
    agentc_jsonw_key(&w, "stop_reason");
    agentc_jsonw_cstr(&w, agentc_mode_stop_name(m->stop_reason));
    agentc_jsonw_key(&w, "usage");
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "input");
    agentc_jsonw_u64(&w, m->usage.input);
    agentc_jsonw_key(&w, "output");
    agentc_jsonw_u64(&w, m->usage.output);
    agentc_jsonw_key(&w, "cache_read");
    agentc_jsonw_u64(&w, m->usage.cache_read);
    agentc_jsonw_key(&w, "cache_write");
    agentc_jsonw_u64(&w, m->usage.cache_write);
    agentc_jsonw_key(&w, "reasoning");
    agentc_jsonw_u64(&w, m->usage.reasoning);
    agentc_jsonw_key(&w, "cost_micro");
    agentc_jsonw_i64(&w, m->usage.cost_micro);
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "error");
    if (m->error)
        agentc_jsonw_cstr(&w, m->error);
    else
        agentc_jsonw_null(&w);
    agentc_jsonw_key(&w, "content");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < m->nblocks; i++) {
        const AgcBlock *blk = &m->blocks[i];
        agentc_jsonw_obj(&w);
        if (blk->type == AGENTC_BLK_TEXT) {
            agentc_jsonw_key(&w, "type");
            agentc_jsonw_cstr(&w, "text");
            agentc_jsonw_key(&w, "text");
            agentc_jsonw_str(&w, blk->text ? blk->text : "", blk->text_len);
        } else if (blk->type == AGENTC_BLK_THINK) {
            agentc_jsonw_key(&w, "type");
            agentc_jsonw_cstr(&w, "thinking");
            agentc_jsonw_key(&w, "text");
            agentc_jsonw_str(&w, blk->text ? blk->text : "", blk->text_len);
        } else {
            agentc_jsonw_key(&w, "type");
            agentc_jsonw_cstr(&w, "tool_call");
            agentc_jsonw_key(&w, "id");
            agentc_jsonw_cstr(&w, blk->tool_id ? blk->tool_id : "");
            agentc_jsonw_key(&w, "name");
            agentc_jsonw_cstr(&w, blk->tool_name ? blk->tool_name : "");
            agentc_jsonw_key(&w, "arguments");
            if (blk->tool_args && blk->tool_args[0])
                agentc_jsonw_raw(&w, blk->tool_args, agentc_strlen(blk->tool_args));
            else
                agentc_jsonw_raw(&w, "{}", 2);
        }
        agentc_jsonw_end(&w);
    }
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
}

void agentc_mode_out(AgcModeCtx *c, const void *p, size_t n) {
    if (n && c->io && c->io->write)
        c->io->write(c->io->out_ud, p, n);
}

void agentc_mode_out_line(AgcModeCtx *c, AgcBuf *b) {
    agentc_buf_byte(b, '\n');
    agentc_mode_out(c, b->p, b->len);
    agentc_buf_free(b);
}

void agentc_mode_write_session_header(AgcModeCtx *c) {
    AgcBuf b = {0};
    AgcJsonW w;
    jsonw_out(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "session");
    agentc_jsonw_key(&w, "version");
    agentc_jsonw_u64(&w, 1);
    agentc_jsonw_key(&w, "id");
    agentc_jsonw_cstr(&w, c->session ? agentc_session_id(c->session) : "");
    agentc_jsonw_key(&w, "timestamp");
    agentc_jsonw_i64(&w, os_now_ns(OS_CLOCK_REALTIME) / 1000000);
    agentc_jsonw_key(&w, "cwd");
    agentc_jsonw_cstr(&w, (c->mcfg && c->mcfg->session.cwd) ? c->mcfg->session.cwd : "");
    agentc_jsonw_end(&w);
    agentc_mode_out_line(c, &b);
}

/* Append every transcript message added since the last flush. */
void agentc_mode_session_flush(AgcModeCtx *c) {
    if (!c || !c->session)
        return;
    const AgcTranscript *tp = agentc_agent_transcript(c->agent);
    if (!tp)
        return;
    /* A compaction splice shrinks the transcript below the recorded flush
     * index; re-sync before iterating or every later message is skipped. */
    if (tp->n < c->flushed)
        c->flushed = tp->n;
    for (; c->flushed < tp->n; c->flushed++) {
        const AgcMsg *m = &tp->msgs[c->flushed];
        if (!agentc_mode_msg_persistable(m))
            continue;
        (void)agentc_session_append_message(c->session, m);
    }
}

/* Default observer: persist each finalized transcript message. Front ends that
 * need their own policy (print/TUI) inject one through AgcModeIo. */
static void mode_flush_observer(void *ud, const AgcMsg *m) {
    (void)m;
    agentc_mode_session_flush(ud);
}

/* ------------------------------------------------------ queue persistence */
/* SIG §5.6 / R-D11: each AGENTC_EV_ACTOR_QUEUED mutation appends a
 * `custom type:"queue"` snapshot; on resume the last snapshot is re-applied
 * after re-validation, dropping invalid items with one NOTIFY. P8-D1 (B-M8):
 * the owner is derived per emitting agent (below), so a background agent of one
 * session can never persist through another session's context. The guards query
 * the hub rather than a cached id so an agentc_events_reset() (SDK shutdown) is
 * seen and the next setup re-subscribes instead of silently going dormant. */

/* P8-D1 (B-M8): the owning context of the agent that emitted `id`. Derived from
 * the emitting agent's owner binding — never a process global. `owner` is opaque
 * to the core; NULL = not front-end-owned (an embedder agent never persists). */
static AgcModeCtx *mode_ctx_of(AgcActorId id) {
    AgcAgent *a = id != 0 ? agentc_agent_find(id) : NULL;
    void *owner = a != NULL ? agentc_agent_owner(a) : NULL;
    return (AgcModeCtx *)owner;
}

/* App-internal (P8-D1/B-M8): re-point every live agent owned by `from` at `to`.
 * The RPC/serve front ends run on a stack copy of the mode context; the agent
 * owner (and the persistence derived from it) must follow the live copy and
 * return before the frame goes away. Bounded by the actor table; no-op for a
 * self-rebind or NULL. */
void agentc_mode_agents_rebind(AgcModeCtx *from, AgcModeCtx *to) {
    /* Contract (runsession.h): a NULL owner binding means "unowned", so a NULL
     * `from` would match every unowned agent and is not a rebind. Likewise there
     * is nothing to rebind to a NULL `to`. Both are no-ops. */
    if (from == NULL || to == NULL || from == to)
        return;
    for (AgcActorId id = 1; id <= AGENTC_ACTOR_MAX; id++) {
        AgcAgent *a = agentc_agent_find(id);
        if (a != NULL && agentc_agent_owner(a) == from)
            agentc_agent_set_owner(a, to);
    }
}

static void mode_queue_snapshot_json(AgcJsonW *w, AgcActorId root, bool paused) {
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "actor_id");
    agentc_jsonw_u64(w, root);
    agentc_jsonw_key(w, "paused");
    agentc_jsonw_bool(w, paused);
    agentc_jsonw_key(w, "items");
    agentc_jsonw_arr(w);
    AgcQueueItem items[AGENTC_QUEUE_MAX];
    int n = agentc_queue_list(root, items, AGENTC_QUEUE_MAX);
    for (int i = 0; i < n; i++) {
        agentc_jsonw_obj(w);
        agentc_jsonw_key(w, "id");
        agentc_jsonw_u64(w, items[i].id);
        agentc_jsonw_key(w, "turn");
        agentc_jsonw_i64(w, items[i].turn);
        agentc_jsonw_key(w, "priority");
        agentc_jsonw_i64(w, items[i].priority);
        agentc_jsonw_key(w, "flags");
        agentc_jsonw_u64(w, items[i].flags);
        agentc_jsonw_key(w, "text");
        agentc_jsonw_cstr(w, items[i].text ? items[i].text : "");
        if (items[i].client_id) {
            agentc_jsonw_key(w, "client_id");
            agentc_jsonw_cstr(w, items[i].client_id);
        }
        agentc_jsonw_end(w);
    }
    agentc_jsonw_end(w);
    agentc_jsonw_end(w);
}

static void mode_queue_observe(void *ud, int ev, const void *data) {
    (void)ud;
    if (ev != AGENTC_EV_ACTOR_QUEUED && ev != AGENTC_EV_QUEUE_PAUSED)
        return;
    const AgcQueueEvent *q = data;
    AgcModeCtx *c = mode_ctx_of(q != NULL ? q->actor_id : 0);
    if (!c || !c->agent || !c->session || !c->persist_ok)
        return;
    AgcActorId root = agentc_agent_actor(c->agent);
    if (root == 0 || (q != NULL && q->actor_id != root))
        return;
    if (agentc_session_path(c->session) == NULL)
        return;
    AgcBuf line = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &line);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "custom");
    agentc_jsonw_key(&w, "custom_type");
    agentc_jsonw_cstr(&w, "queue");
    agentc_jsonw_key(&w, "data");
    mode_queue_snapshot_json(&w, root, agentc_queue_is_paused(root));
    agentc_jsonw_end(&w);
    (void)agentc_session_append_raw(c->session, (const char *)line.p, line.len);
    agentc_buf_free(&line);
}

/* P6-D: the agent lifecycle as `custom type:"agent"` records so resume can
 * reconstruct the tree read-only (P6-SUBAGENTS §8). Spawn carries the
 * ACTOR_STARTED override payload; finish/cancel/fail carry the exit result. */
static void mode_agent_observe(void *ud, int ev, const void *data) {
    (void)ud;
    const AgcActorEvent *e = data;
    const AgcAgentResultEvent *r = data;
    AgcModeCtx *c = (ev == AGENTC_EV_AGENT_RESULT) ? mode_ctx_of(r ? r->id : 0)
                                                  : mode_ctx_of(e ? e->id : 0);
    if (!c || !c->session || !c->persist_ok)
        return;
    if (agentc_session_path(c->session) == NULL)
        return;
    AgcBuf line = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &line);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "custom");
    agentc_jsonw_key(&w, "custom_type");
    agentc_jsonw_cstr(&w, "agent");
    agentc_jsonw_key(&w, "data");
    agentc_jsonw_obj(&w);
    if (ev == AGENTC_EV_ACTOR_STARTED && e != NULL && e->kind == AGENTC_ACTOR_AGENT) {
        AgcAgentInfo info;
        agentc_jsonw_key(&w, "op");
        agentc_jsonw_cstr(&w, e->parent_id != 0 ? "spawn" : "root");
        agentc_jsonw_key(&w, "agent_id");
        agentc_jsonw_u64(&w, e->id);
        agentc_jsonw_key(&w, "parent_id");
        agentc_jsonw_u64(&w, e->parent_id);
        agentc_jsonw_key(&w, "depth");
        agentc_jsonw_u64(&w, e->depth);
        if (agentc_agent_info(e->id, &info) == 0) {
            agentc_jsonw_key(&w, "provider");
            agentc_jsonw_cstr(&w, info.provider ? info.provider : "");
            agentc_jsonw_key(&w, "model");
            agentc_jsonw_cstr(&w, info.model ? info.model : "");
        }
        if (e->json != NULL) {
            agentc_jsonw_key(&w, "info");
            agentc_jsonw_raw(&w, e->json, agentc_strlen(e->json));
        }
    } else if (ev == AGENTC_EV_AGENT_RESULT) {
        const AgcAgentResultEvent *r = data;
        agentc_jsonw_key(&w, "op");
        agentc_jsonw_cstr(&w, "finish");
        agentc_jsonw_key(&w, "agent_id");
        agentc_jsonw_u64(&w, r ? r->id : 0);
        agentc_jsonw_key(&w, "parent_id");
        agentc_jsonw_u64(&w, r ? r->parent_id : 0);
        agentc_jsonw_key(&w, "exit_code");
        agentc_jsonw_i64(&w, r ? r->exit_code : 0);
        agentc_jsonw_key(&w, "duration_ms");
        agentc_jsonw_i64(&w, r ? r->duration_ms : 0);
    } else if ((ev == AGENTC_EV_ACTOR_FAIL || ev == AGENTC_EV_ACTOR_CANCELLED) && e != NULL &&
               e->kind == AGENTC_ACTOR_AGENT) {
        agentc_jsonw_key(&w, "op");
        agentc_jsonw_cstr(&w, ev == AGENTC_EV_ACTOR_FAIL ? "fail" : "cancel");
        agentc_jsonw_key(&w, "agent_id");
        agentc_jsonw_u64(&w, e->id);
        agentc_jsonw_key(&w, "parent_id");
        agentc_jsonw_u64(&w, e->parent_id);
    } else {
        agentc_jsonw_end(&w);
        agentc_jsonw_end(&w);
        agentc_buf_free(&line);
        return;
    }
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    (void)agentc_session_append_raw(c->session, (const char *)line.p, line.len);
    agentc_buf_free(&line);
}

/* W4: write one `custom type:"model"` record naming the virtual alias and the
 * concrete target it resolved to. Exposed for the golden test and the typed
 * observer below; the format mirrors the other custom records. */
void agentc_mode_note_model_route(AgcSession *s, const char *virtual_model, const char *provider,
                                  const char *model) {
    if (!s || !virtual_model || !provider || !model)
        return;
    AgcBuf line = {0};
    AgcJsonW w;
    jsonw_out(&w, &line);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "custom");
    agentc_jsonw_key(&w, "custom_type");
    agentc_jsonw_cstr(&w, "model");
    agentc_jsonw_key(&w, "data");
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "virtual");
    agentc_jsonw_cstr(&w, virtual_model);
    agentc_jsonw_key(&w, "provider");
    agentc_jsonw_cstr(&w, provider);
    agentc_jsonw_key(&w, "model");
    agentc_jsonw_cstr(&w, model);
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    (void)agentc_session_append_raw(s, (const char *)line.p, line.len);
    agentc_buf_free(&line);
}

/* W4: observe AGENTC_EV_MODEL_ROUTE and persist the route as a
 * `custom type:"model"` record through the one session append choke point. */
static void mode_model_observe(void *ud, int ev, const void *data) {
    (void)ud;
    if (ev != AGENTC_EV_MODEL_ROUTE)
        return;
    const AgcModelRouteEvent *e = data;
    AgcModeCtx *c = mode_ctx_of(e != NULL ? e->actor_id : 0);
    if (!c || !c->session || !c->persist_ok)
        return;
    if (agentc_session_path(c->session) == NULL)
        return;
    if (e == NULL || e->virtual_model == NULL || e->provider == NULL || e->model == NULL)
        return;
    agentc_mode_note_model_route(c->session, e->virtual_model, e->provider, e->model);
}

/* Reconstruct the read-only persisted agent tree from the session records. */
void agentc_mode_load_agent_tree(AgcModeCtx *c) {
    if (!c)
        return;
    if (c->persisted_agents) {
        agentc_session_agents_free(c->persisted_agents, c->npersisted_agents);
        c->persisted_agents = NULL;
        c->npersisted_agents = 0;
    }
    if (!c->session || agentc_session_path(c->session) == NULL)
        return;
    size_t n = agentc_session_load_agents(c->session, NULL, 0);
    if (n == 0)
        return;
    if (n > AGENTC_ACTOR_MAX)
        n = AGENTC_ACTOR_MAX;
    AgcSessionAgent *rows = agentc_alloc(n * sizeof *rows);
    if (!rows)
        return;
    agentc_memset(rows, 0, n * sizeof *rows);
    c->npersisted_agents = agentc_session_load_agents(c->session, rows, n);
    c->persisted_agents = rows;
}

const AgcSessionAgent *agentc_mode_persisted_agents(const AgcModeCtx *c, size_t *n) {
    if (n)
        *n = c ? c->npersisted_agents : 0;
    return c ? c->persisted_agents : NULL;
}

static int mode_queue_collect_cb(void *ud, const char *data_json) {
    AgcBuf *last = ud;
    agentc_buf_clear(last);
    if (data_json)
        agentc_buf_cstr(last, data_json);
    return 0;
}

/* Re-apply the last persisted snapshot to the (already bound) root actor.
 * P8-D4 (B-M3) / C4: a switch always REPLACES the live queue. The reset runs
 * before either early-out (no record, malformed record) so the previous
 * session's prompts can never leak into the new one. */
static void mode_restore_queue(AgcModeCtx *c) {
    if (!c || !c->agent || !c->session)
        return;
    AgcActorId root = agentc_agent_actor(c->agent);
    if (root == 0 || agentc_session_path(c->session) == NULL)
        return;
    AgcBuf last = {0};
    if (agentc_session_load_custom(c->session, "queue", mode_queue_collect_cb, &last) != 0) {
        agentc_buf_free(&last);
        return;
    }
    bool saved = c->persist_ok;
    c->persist_ok = false; /* replay must not re-append (P8-D1/D4) */
    (void)agentc_queue_clear(root); /* reset-then-replay: replace, never merge (B-M3) */
    if (last.p == NULL) {
        c->persist_ok = saved;
        agentc_buf_free(&last);
        return;
    }
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *o = agentc_json_parse_in(ja, (const char *)last.p, last.len);
    const AgcJson *items = agentc_json_get(o, "items");
    if (agentc_json_type(items) == AGENTC_JSON_ARR) {
        size_t dropped = 0;
        size_t n = agentc_json_len(items);
        for (size_t i = 0; i < n; i++) {
            const AgcJson *it = agentc_json_at(items, i);
            const char *text = agentc_json_get_str(it, "text");
            i64 turn = agentc_json_get_int(it, "turn", 0);
            i64 prio = agentc_json_get_int(it, "priority", 0);
            i64 flags = agentc_json_get_int(it, "flags", 0);
            const char *cid = agentc_json_get_str(it, "client_id");
            size_t tlen = text ? agentc_strlen(text) : 0;
            bool valid = text != NULL && tlen > 0 && turn >= 0 && prio >= AGENTC_PRIO_MIN &&
                         prio <= AGENTC_PRIO_MAX && tlen <= AGENTC_MSG_MAX_TEXT;
            if (!valid) {
                dropped++;
                continue;
            }
            /* A replay is not a user enqueue: drop the DEDUPE bit so a snapshot
             * can never silently merge two same-client_id rows on restore. */
            if (agentc_queue_add(root, text, turn, (int)prio,
                                 (uint32_t)flags & ~AGENTC_Q_F_DEDUPE, cid) == 0)
                dropped++;
        }
        if (dropped > 0) {
            AgcMessage m;
            agentc_memset(&m, 0, sizeof m);
            m.class = AGENTC_MSG_CLASS_NOTIFY;
            m.kind = AGENTC_MSG_NOTIFY;
            m.to = root;
            m.from = 0;
            m.body = "queue: dropped invalid item(s) on resume";
            m.body_len = agentc_strlen(m.body);
            (void)agentc_message_send(&m);
        }
    }
    /* Restore the pause flag too: a resume must reproduce the persisted
     * scheduler state, and the reset above dropped any stale pause. Only a true
     * snapshot needs to emit (the clear already set paused=false). */
    if (agentc_json_type(o) == AGENTC_JSON_OBJ && agentc_json_get_bool(o, "paused", false))
        (void)agentc_queue_pause(root, true);
    c->persist_ok = saved;
    agentc_json_arena_free(ja);
    agentc_buf_free(&last);
}

void agentc_mode_append_compaction(AgcSession *s, const AgcCompactInfo *ci) {
    (void)agentc_session_append_compaction(s, ci);
}

void agentc_mode_note_compaction(AgcModeCtx *c, const AgcCompactInfo *ci) {
    if (!c || !ci || !c->session)
        return;
    /* The loader retains the last `kept_messages` PERSISTED messages of the
     * replay, but the core count is in-memory. agentc_mode_msg_persistable()
     * skips failed/aborted assistant turns, so a retained tail can hold fewer
     * messages in the JSONL than the core kept. Persist the persistable count
     * of the retained tail or replay reaches past it into the compacted prefix
     * and resurrects a message the summary replaced. */
    AgcCompactInfo rec = *ci;
    const AgcTranscript *tp = c->agent ? agentc_agent_transcript(c->agent) : NULL;
    if (tp) {
        size_t kept = ci->kept_messages;
        size_t start = tp->n > kept ? tp->n - kept : 0;
        size_t persistable = 0;
        for (size_t i = start; i < tp->n; i++)
            if (agentc_mode_msg_persistable(&tp->msgs[i]))
                persistable++;
        rec.kept_messages = (u32)persistable;
    }
    agentc_mode_append_compaction(c->session, &rec);
    c->flushed = (size_t)ci->kept_messages + 1;
}

static void mode_event_common(AgcModeCtx *c, int ev, const void *data) {
    AgcBuf b = {0};
    AgcJsonW w;
    switch (ev) {
    case AGENTC_EV_AGENT_START:
        agentc_buf_cstr(&b, "{\"type\":\"agent_start\"}");
        break;
    case AGENTC_EV_STEP_START:
        agentc_buf_cstr(&b, "{\"type\":\"turn_start\"}");
        break;
    case AGENTC_EV_MSG_START: {
        const AgcMsg *m = data;
        jsonw_out(&w, &b);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, "message_start");
        agentc_jsonw_key(&w, "message");
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "role");
        agentc_jsonw_cstr(&w, mode_role_name(m->role));
        agentc_jsonw_key(&w, "ts");
        agentc_jsonw_u64(&w, m->ts_ms);
        agentc_jsonw_end(&w);
        agentc_jsonw_end(&w);
        break;
    }
    case AGENTC_EV_TEXT_DELTA:
    case AGENTC_EV_THINK_DELTA:
    case AGENTC_EV_TOOL_ARGS_DELTA: {
        const AgcTextDelta *d = data;
        const char *kind = ev == AGENTC_EV_TEXT_DELTA    ? "text"
                           : ev == AGENTC_EV_THINK_DELTA ? "thinking"
                                                         : "tool_args";
        jsonw_out(&w, &b);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, "message_update");
        agentc_jsonw_key(&w, "delta");
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, kind);
        agentc_jsonw_key(&w, "text");
        agentc_jsonw_str(&w, d->text ? d->text : "", d->len);
        agentc_jsonw_end(&w);
        agentc_jsonw_end(&w);
        break;
    }
    case AGENTC_EV_MSG_END: {
        const AgcMsg *m = data;
        jsonw_out(&w, &b);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, "message_end");
        agentc_jsonw_key(&w, "message");
        AgcBuf mb = {0};
        agentc_mode_msg_json(&mb, m);
        agentc_jsonw_raw(&w, (const char *)mb.p, mb.len);
        agentc_buf_free(&mb);
        agentc_jsonw_end(&w);
        break;
    }
    case AGENTC_EV_TOOL_EXEC_START: {
        const AgcToolExec *e = data;
        jsonw_out(&w, &b);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, "tool_execution_start");
        agentc_jsonw_key(&w, "tool_call_id");
        agentc_jsonw_cstr(&w, e->call_id ? e->call_id : "");
        agentc_jsonw_key(&w, "tool_name");
        agentc_jsonw_cstr(&w, e->tool_name ? e->tool_name : "");
        agentc_jsonw_key(&w, "args");
        agentc_jsonw_raw(&w, e->args_json ? e->args_json : "{}",
                         e->args_json ? agentc_strlen(e->args_json) : 2);
        agentc_jsonw_end(&w);
        break;
    }
    case AGENTC_EV_TOOL_EXEC_END: {
        const AgcToolExec *e = data;
        jsonw_out(&w, &b);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, "tool_execution_end");
        agentc_jsonw_key(&w, "tool_call_id");
        agentc_jsonw_cstr(&w, e->call_id ? e->call_id : "");
        agentc_jsonw_key(&w, "tool_name");
        agentc_jsonw_cstr(&w, e->tool_name ? e->tool_name : "");
        agentc_jsonw_key(&w, "result");
        agentc_jsonw_cstr(&w, e->result ? e->result : "");
        agentc_jsonw_key(&w, "is_error");
        agentc_jsonw_bool(&w, e->is_error);
        agentc_jsonw_key(&w, "duration_ms");
        agentc_jsonw_i64(&w, e->duration_ms);
        agentc_jsonw_end(&w);
        break; /* the session observer already persisted the message */
    }
    case AGENTC_EV_MSG_RESET:
        /* A retried attempt replaces what already streamed for the current
         * message; JSON/RPC consumers discard its deltas on this marker. */
        agentc_buf_cstr(&b, "{\"type\":\"message_reset\"}");
        break;
    case AGENTC_EV_STEP_END:
        agentc_buf_cstr(&b, "{\"type\":\"turn_end\"}");
        break;
    case AGENTC_EV_AGENT_END:
        agentc_buf_cstr(&b, "{\"type\":\"agent_end\"}");
        break;
    case AGENTC_EV_ERROR: {
        const char *err = data;
        jsonw_out(&w, &b);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, "error");
        agentc_jsonw_key(&w, "error");
        agentc_jsonw_cstr(&w, err ? err : "error");
        agentc_jsonw_end(&w);
        break;
    }
    case AGENTC_EV_COMPACT: {
        const AgcCompactInfo *ci = data;
        jsonw_out(&w, &b);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, "compaction");
        agentc_jsonw_key(&w, "tokens_before");
        agentc_jsonw_u64(&w, ci->tokens_before);
        agentc_jsonw_key(&w, "kept_messages");
        agentc_jsonw_u64(&w, ci->kept_messages);
        agentc_jsonw_key(&w, "automatic");
        agentc_jsonw_bool(&w, ci->automatic);
        agentc_jsonw_key(&w, "summary");
        if (ci->summary)
            agentc_jsonw_cstr(&w, ci->summary);
        else
            agentc_jsonw_null(&w);
        agentc_jsonw_end(&w);
        if (c->session) {
            agentc_mode_note_compaction(c, ci);
            /* The event fires before the splice in compact_now(); the new
             * transcript length is exactly checkpoint + kept messages, so use
             * that instead of the pre-splice count the transcript still shows. */
        }
        break;
    }
    default:
        return;
    }
    agentc_mode_out_line(c, &b);
}

void agentc_mode_event(void *ud, int ev, const void *data) {
    mode_event_common(ud, ev, data);
}

/* Rebuild the agent (same transcript/config) under c->provider / c->model.
 * A change of the effective provider or model emits the observe-only
 * `model_select` point ({provider, model, previous}) after the new agent is
 * fully installed; the model/thinking sinks close over the mode context, so
 * they keep working across the swap. */
AgcAgent *agentc_mode_rebuild_agent(AgcModeCtx *c) {
    const AgcProvider *p = agentc_setup_provider(c->provider);
    if (!p)
        p = agentc_prov_openai();
    /* The old transcript borrows the old agent's strings; snapshot them for the
     * change check and the payload before the old agent is freed. */
    const AgcTranscript *old = agentc_agent_transcript(c->agent);
    const char *old_provider = (old && old->provider) ? old->provider : "";
    const char *old_model = (old && old->model) ? old->model : "";
    AgcTransport transport;
    agentc_memset(&transport, 0, sizeof transport);
    AgcRouteSeam prev_seam;
    agentc_memset(&prev_seam, 0, sizeof prev_seam);
    if (c->agent) {
        transport = agentc_agent_transport(c->agent);
        prev_seam = agentc_agent_route_seam(c->agent);
    }
    /* R-D1: the app builds the spec and calls the one constructor. */
    AgcAgentSpec aspec;
    agentc_memset(&aspec, 0, sizeof aspec);
    aspec.provider_handle = p;
    aspec.provider = c->provider;
    aspec.model = c->model;
    aspec.thinking = -1;
    aspec.label = "agent";
    aspec.flags = AGENTC_AGENT_F_WAIT;
    /* P8-D1 (B-M8): an injected/test seam survives the swap exactly like the
     * injected transport; otherwise the mode seam is re-installed identically. */
    if (prev_seam.provider != NULL || prev_seam.target != NULL)
        aspec.route = prev_seam;
    else
        aspec.route = mode_seam(c);
    aspec.owner = c;
    if (transport.request) {
        aspec.transport = transport;
        aspec.transport_set = true;
    }
    AgcAgent *na = agentc_agent_create(&aspec);
    if (!na)
        return NULL;
    mode_configure_agent(na, c->mcfg);
    /* mode_configure_agent wires the production HTTP transport; an injected one
     * (tests/embedders) survives the swap, exactly like the hooks below. */
    if (transport.request)
        agentc_agent_set_transport(na, transport);
    const AgcTranscript *nt = agentc_agent_transcript(na);
    const char *new_provider = (nt && nt->provider) ? nt->provider : "";
    const char *new_model = (nt && nt->model) ? nt->model : "";
    bool changed = !agentc_streq(new_provider, old_provider) || !agentc_streq(new_model, old_model);
    bool emit = changed && agentc_ext_wants("model_select");
    char *previous = emit ? agentc_strdup(old_model) : NULL;
    (void)agentc_agent_load(na, agentc_agent_transcript(c->agent));
    /* Carry the pending queue across the rebuild (the old actor's queue is
     * freed with the agent) and keep the queue observer from persisting the
     * transient empty/duplicate snapshots. */
    AgcQueueItem qsave[AGENTC_QUEUE_MAX];
    size_t qsave_n = 0;
    if (c->agent != NULL && agentc_agent_actor(c->agent) != 0) {
        AgcQueueItem tmp[AGENTC_QUEUE_MAX];
        int qn = agentc_queue_list(agentc_agent_actor(c->agent), tmp, AGENTC_QUEUE_MAX);
        for (int i = 0; i < qn && qsave_n < AGENTC_QUEUE_MAX; i++) {
            qsave[qsave_n] = tmp[i];
            qsave[qsave_n].text = agentc_strdup(tmp[i].text ? tmp[i].text : "");
            qsave[qsave_n].client_id = tmp[i].client_id ? agentc_strdup(tmp[i].client_id) : NULL;
            qsave_n++;
        }
    }
    bool qpersist_saved = c->persist_ok;
    c->persist_ok = false;
    agentc_agent_free(c->agent);
    c->agent = na;

    /* The old agent owned the front end hooks; reinstall them on the new one. */
    if (c->flags & AGENTC_MODE_F_ABORT)
        agentc_agent_set_tool_veto(na, agentc_ext_tool_veto, NULL);
    AgcMsgObserver obs = (c->io && c->io->observer) ? c->io->observer : mode_flush_observer;
    void *obs_ud = (c->io && c->io->observer) ? c->io->observer_ud : c;
    agentc_agent_set_observer(na, obs, obs_ud);
    AgcEventFn ev = (c->io && c->io->event) ? c->io->event : agentc_mode_event;
    void *ev_ud = (c->io && c->io->event) ? c->io->event_ud : c;
    agentc_agent_set_events(na, ev, ev_ud);

    if (emit) {
        AgcBuf pb = {0};
        AgcJsonW w;
        jsonw_out(&w, &pb);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "provider");
        agentc_jsonw_cstr(&w, new_provider);
        agentc_jsonw_key(&w, "model");
        agentc_jsonw_cstr(&w, new_model);
        agentc_jsonw_key(&w, "previous");
        agentc_jsonw_cstr(&w, previous ? previous : "");
        agentc_jsonw_end(&w);
        AgcExtResult r = agentc_ext_emit("model_select", (const char *)pb.p);
        agentc_free(r.result_json);
        agentc_buf_free(&pb);
        agentc_free(previous);
    }
    /* The root agent actor is bound per agent instance (P6-B, C-D8): the old
     * actor was released with the old agent, the new one gets a fresh row so
     * the bus/queue/steer surfaces stay live across a rebuild. */
    (void)agentc_agent_bind_actor(na, 0, "agent");
    agentc_agent_set_auto_start(na, agentc_config_queue_auto_start(c->cfg));
    for (size_t i = 0; i < qsave_n; i++) {
        (void)agentc_queue_add(agentc_agent_actor(na), qsave[i].text, qsave[i].turn,
                               qsave[i].priority, qsave[i].flags & ~AGENTC_Q_F_DEDUPE,
                               qsave[i].client_id);
        agentc_free(qsave[i].text);
        agentc_free(qsave[i].client_id);
    }
    c->persist_ok = qpersist_saved;
    return na;
}

/* ------------------------------------------------------------ session specs */

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = agentc_strlen(s), fl = agentc_strlen(suffix);
    return sl >= fl && agentc_memeq(s + sl - fl, suffix, fl);
}

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            b = p + 1;
    return b;
}

static bool id_matches(const char *path, const char *id) {
    const char *b = base_name(path);
    if (!ends_with(b, ".jsonl"))
        return false;
    size_t bl = agentc_strlen(b);
    size_t stem = bl - 6;
    size_t il = agentc_strlen(id);
    if (stem == il && agentc_str_eq(b, il, id, il))
        return true;
    if (stem >= il + 1 && b[stem - il - 1] == '_' && agentc_str_eq(b + stem - il, il, id, il))
        return true;
    return false;
}

static char *resolve_session_arg(const char *dir, const char *spec) {
    if (!spec || !spec[0])
        return NULL;
    if (agentc_str_str(spec, "/"))
        return agentc_strdup(spec);
    if (ends_with(spec, ".jsonl")) {
        char full[4096];
        if (dir && dir[0] && agentc_path_join(full, sizeof full, dir, spec))
            return agentc_strdup(full);
        return agentc_strdup(spec);
    }
    size_t n = 0;
    char **list = agentc_session_list(dir, &n, 0);
    char *found = NULL;
    for (size_t i = 0; i < n && !found; i++)
        if (id_matches(list[i], spec))
            found = agentc_strdup(list[i]);
    agentc_sessions_free(list, n);
    return found;
}

/* Create or reopen the session selected by the mode config. Returns 0, -2 for
 * a user-visible --session failure (the caller maps it to exit 2) or -12. */
static int mode_session_create(const AgcModeConfig *cfg, AgcSession **out, bool *resumed) {
    *out = NULL;
    if (resumed)
        *resumed = false;
    if (cfg->session.memory_only) {
        *out = agentc_session_new(&cfg->session);
        return *out ? 0 : -12;
    }
    if (cfg->session_spec && cfg->session_spec[0]) {
        char *path = resolve_session_arg(cfg->session.dir, cfg->session_spec);
        if (!path) {
            agentc_logf(3, "session not found: %s", cfg->session_spec);
            return -2;
        }
        AgcSession *s = agentc_session_open(path);
        agentc_free(path);
        if (!s) {
            agentc_logf(3, "cannot open session: %s", cfg->session_spec);
            return -2;
        }
        *out = s;
        if (resumed)
            *resumed = true;
        return 0;
    }
    if (cfg->continue_last) {
        char *latest = agentc_session_find_latest(cfg->session.dir, cfg->session.cwd);
        AgcSession *s = NULL;
        if (latest) {
            s = agentc_session_open(latest);
            agentc_free(latest);
        }
        if (!s) {
            agentc_logf(2, "no session found for this directory; starting a new one");
            s = agentc_session_new(&cfg->session);
        } else if (resumed) {
            *resumed = true;
        }
        if (!s)
            return -12;
        *out = s;
        return 0;
    }
    *out = agentc_session_new(&cfg->session);
    return *out ? 0 : -12;
}

/* ---------------------------------------------------------- app wiring */
/* Moved from main.c: the extension context, entry sink, veto and pump are part
 * of the shared cooperative-abort setup, not of any one mode. */

/* Custom session entries requested by extensions. */
static void ext_entry_sink(void *ud, const char *type, const char *data_json) {
    AgcSession *s = ud;
    if (!s)
        return;
    AgcBuf line = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &line);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "custom");
    agentc_jsonw_key(&w, "custom_type");
    agentc_jsonw_cstr(&w, type ? type : "");
    agentc_jsonw_key(&w, "data");
    const char *dj = data_json ? data_json : "null";
    agentc_jsonw_raw(&w, dj, agentc_strlen(dj));
    agentc_jsonw_end(&w);
    agentc_session_append_raw(s, (const char *)line.p, line.len);
    agentc_buf_free(&line);
}

/* P6-C audit sink: an approval/denial becomes a `custom type:"approval"`
 * session record (the audit JSON already carries its own type). */
static void mode_trust_audit(void *ud, const char *json, size_t n) {
    (void)n;
    AgcSession *s = ud;
    if (!s || !json)
        return;
    ext_entry_sink(s, "approval", json);
}

void agentc_mode_emit_session_start(AgcModeCtx *c, const char *reason,
                                    const char *previous_session_file) {
    if (!c || !agentc_ext_wants("session_start"))
        return;
    AgcBuf pb = {0};
    AgcJsonW w;
    jsonw_out(&w, &pb);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "reason");
    agentc_jsonw_cstr(&w, reason ? reason : "startup");
    agentc_jsonw_key(&w, "session_id");
    agentc_jsonw_cstr(&w, c->session ? agentc_session_id(c->session) : "");
    agentc_jsonw_key(&w, "session_file");
    agentc_jsonw_cstr(
        &w, (c->session && agentc_session_path(c->session)) ? agentc_session_path(c->session) : "");
    if (previous_session_file && previous_session_file[0]) {
        agentc_jsonw_key(&w, "previous_session_file");
        agentc_jsonw_cstr(&w, previous_session_file);
    }
    agentc_jsonw_end(&w);
    AgcExtResult sr = agentc_ext_emit("session_start", (const char *)pb.p);
    agentc_free(sr.result_json);
    agentc_buf_free(&pb);
}

void agentc_mode_rebind_session(AgcModeCtx *c) {
    if (!c)
        return;
    /* The extension context and entry sink are part of the cooperative-abort
     * wiring installed at setup. Other context fields (cwd, system_prompt) are
     * preserved: agentc_ext_set_context() ignores NULL and an empty
     * session_file clears it. */
    if (c->flags & AGENTC_MODE_F_ABORT) {
        AgcExtContext ctx;
        agentc_memset(&ctx, 0, sizeof ctx);
        ctx.session_id = c->session ? agentc_session_id(c->session) : "";
        ctx.session_file =
            (c->session && agentc_session_path(c->session)) ? agentc_session_path(c->session) : "";
        agentc_ext_set_context(&ctx);
        agentc_ext_set_entry_sink(ext_entry_sink, c->session);
    }
    /* The persistence owner must be this context: an RPC session swap frees
     * the session an older ctx-bound observer still points at. */
    AgcMsgObserver obs = (c->io && c->io->observer) ? c->io->observer : mode_flush_observer;
    void *obs_ud = (c->io && c->io->observer) ? c->io->observer_ud : c;
    if (c->agent)
        agentc_agent_set_observer(c->agent, obs, obs_ud);
}

/* `session_before_switch` (override/first/replace/fail-closed): returns true
 * to cancel the pending switch. `{"cancel":true}`, a blocked/failed handler, a
 * handled result with no boolean decision, or a malformed `cancel` field all
 * cancel; a well-formed `{"cancel":false}` allows, whether or not the handler
 * claimed `handled`. Fail-closed is sticky: once any handler blocks/fails, an
 * earlier well-formed `{"cancel":false}` accumulated into `result_json` must
 * not re-open the switch. */
static bool session_switch_cancelled(const char *reason) {
    if (!agentc_ext_wants("session_before_switch"))
        return false;
    char payload[64];
    agentc_snprintf(payload, sizeof payload, "{\"reason\":\"%s\"}", reason ? reason : "new");
    AgcExtResult sr = agentc_ext_emit("session_before_switch", payload);
    if (sr.blocked) {
        /* A failed/overrunning/malformed handler cancels unconditionally; the
         * accumulated result may still hold an earlier handler's decision. */
        agentc_free(sr.result_json);
        return true;
    }
    bool cancel = false;
    bool decision = false;
    if (sr.result_json) {
        AgcJsonArena *ja = agentc_json_arena_new(0);
        AgcJson *o = agentc_json_parse_in(ja, sr.result_json, agentc_strlen(sr.result_json));
        if (agentc_json_type(o) != AGENTC_JSON_OBJ) {
            cancel = true; /* malformed decision */
        } else {
            const AgcJson *cv = agentc_json_get(o, "cancel");
            if (cv) {
                int ct = agentc_json_type(cv);
                if (ct != AGENTC_JSON_TRUE && ct != AGENTC_JSON_FALSE) {
                    cancel = true; /* malformed decision */
                } else {
                    decision = true;
                    cancel = (ct == AGENTC_JSON_TRUE);
                }
            }
        }
        agentc_json_arena_free(ja);
    }
    if (sr.handled && !decision)
        cancel = true; /* handled without a decision */
    agentc_free(sr.result_json);
    return cancel;
}

/* Close the current session and install `fresh` as the persistence owner:
 * rebind the extension context/observer before any append can happen, then
 * announce the swap. `reason` is "new" or "resume". */
static void mode_install_session(AgcModeCtx *c, AgcSession *fresh, const char *reason) {
    const char *old_path = agentc_session_path(c->session);
    char *previous = agentc_strdup(old_path ? old_path : "");
    agentc_session_close(c->session);
    c->session = fresh;
    c->session_resumed = agentc_streq(reason, "resume");
    agentc_mode_rebind_session(c);
    agentc_mode_emit_session_start(c, reason, previous ? previous : "");
    agentc_free(previous);
}

int agentc_mode_new_session(AgcModeCtx *c) {
    if (!c || !c->mcfg)
        return -22;
    if (!c->session)
        return -22;
    /* The switch is cancellable before any file is touched: a cancel leaves
     * the current session, its transcript and the extension context intact. */
    if (session_switch_cancelled("new"))
        return -125; /* -ECANCELED */
    /* Create the replacement before dropping the old session so an allocation
     * failure cannot leave the context with no session at all. */
    AgcSession *fresh = agentc_session_new(&c->mcfg->session);
    if (!fresh)
        return -12;
    if (!c->mcfg->session.memory_only && agentc_session_path(fresh) == NULL) {
        /* P8-D4 (B-M1): the file could not be created and agentc_session_new
         * degraded to memory-only. Refuse the swap: the working session and its
         * transcript stay intact and the front end reports the failure. An
         * explicitly requested --no-session /new still works. */
        agentc_session_close(fresh);
        return -5; /* EIO */
    }
    /* clear the chat transcript for the new session (no append, so the old
     * observer cannot write); rebind happens in mode_install_session */
    AgcTranscript empty;
    agentc_transcript_init(&empty);
    (void)agentc_agent_load(c->agent, &empty);
    agentc_transcript_free(&empty);
    mode_install_session(c, fresh, "new");
    c->flushed = 0;
    /* P8-D4 (B-M3): /new starts with an empty queue. Clear after install so the
     * empty snapshot persists to the new file, matching live state. */
    AgcActorId root = agentc_agent_actor(c->agent);
    if (root != 0)
        (void)agentc_queue_clear(root);
    agentc_mode_load_agent_tree(c); /* fresh session: no persisted rows */
    return 0;
}

/* Re-append one parent `custom` record into a forked session (P7). */
static int fork_copy_custom(void *ud, const char *type, const char *data) {
    AgcSession *dst = ud;
    AgcBuf b = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "custom");
    agentc_jsonw_key(&w, "custom_type");
    agentc_jsonw_cstr(&w, type);
    agentc_jsonw_key(&w, "data");
    agentc_jsonw_raw(&w, data, agentc_strlen(data));
    agentc_jsonw_end(&w);
    int rc = agentc_session_append_raw(dst, (const char *)b.p, b.len);
    agentc_buf_free(&b);
    return rc;
}

/* Fork the current conversation into a new session file (P7): a new header with
 * `parent_session` set to the current id, the live transcript and the parent's
 * `custom` records (agent tree, queue, notices, routes) copied in, and the agent
 * kept (unlike /new). Refuses a memory-only session (nothing to fork). No
 * transcript load happens: rebinding the observer cannot re-append the copied
 * rows. */
int agentc_mode_fork_session(AgcModeCtx *c) {
    if (!c || !c->mcfg || !c->agent || !c->session)
        return -22;
    if (c->mcfg->session.memory_only)
        return -22; /* nothing to fork to */
    if (session_switch_cancelled("fork"))
        return -125; /* -ECANCELED */
    const char *parent = agentc_session_id(c->session);
    if (!parent || !parent[0])
        return -22;
    AgcSessionOptions o = c->mcfg->session;
    o.parent_session = parent;
    o.id = NULL; /* a fork is a NEW session: never reuse the parent's id */
    AgcSession *fresh = agentc_session_new(&o);
    if (!fresh)
        return -12;
    if (agentc_session_path(fresh) == NULL) {
        /* P8-D4 (B-M1): a fork that cannot persist is refused (the requested
         * memory-only case is already rejected above). */
        agentc_session_close(fresh);
        return -5; /* EIO */
    }
    const AgcTranscript *tr = agentc_agent_transcript(c->agent);
    if (tr) {
        for (size_t i = 0; i < tr->n; i++) {
            if (!agentc_mode_msg_persistable(&tr->msgs[i]))
                continue;
            int rc = agentc_session_append_message(fresh, &tr->msgs[i]);
            if (rc < 0) {
                /* A short/failed write leaves the fork incomplete and the file
                 * read-only; do not pretend success or advance `flushed`. */
                agentc_logf(2, "fork: append failed (errno %d); fork not created", rc);
                agentc_session_close(fresh);
                return rc;
            }
        }
    }
    /* Copy the parent's custom records (agent tree, queue snapshots, notices,
     * routes) so the fork is a self-contained point-in-time snapshot, not just
     * the message transcript. */
    if (agentc_session_load_custom_all(c->session, fork_copy_custom, fresh) < 0) {
        agentc_logf(2, "fork: custom record copy failed; fork not created");
        agentc_session_close(fresh);
        return -5;
    }
    mode_install_session(c, fresh, "fork");
    /* every live entry is considered written (copied or deliberately skipped) */
    c->flushed = tr ? tr->n : 0;
    agentc_mode_load_agent_tree(c);
    return 0;
}

int agentc_mode_resume_session(AgcModeCtx *c, const char *path) {
    if (!c || !c->mcfg || !c->agent || !path || !path[0])
        return -22;
    if (!c->session || c->mcfg->session.memory_only)
        return -22;
    AgcSession *s = agentc_session_open(path);
    if (!s)
        return -2; /* ENOENT */
    AgcTranscript tr;
    agentc_transcript_init(&tr);
    if (agentc_session_load_messages(s, &tr) != 0) {
        agentc_transcript_free(&tr);
        agentc_session_close(s);
        return -5; /* EIO */
    }
    /* Replace the agent transcript before rebinding persistence: the load does
     * not append, so the old observer never sees it. */
    (void)agentc_agent_load(c->agent, &tr);
    size_t loaded = tr.n;
    agentc_transcript_free(&tr);
    mode_install_session(c, s, "resume");
    c->flushed = loaded;
    mode_restore_queue(c);
    agentc_mode_load_agent_tree(c);
    return 0;
}

/* App model service: a same-provider switch goes through the agent (which
 * emits `model_select`). A provider switch is refused here: it needs an idle
 * rebuild, which only the RPC command path owns; rebuilding here could free the
 * agent that is executing a tool or that a front end still holds. */
static int mode_ext_set_model(void *ud, const char *provider, const char *model) {
    AgcModeCtx *c = ud;
    if (!c || !model || !model[0])
        return -22;
    if (provider && provider[0] && (!c->provider || !agentc_streq(provider, c->provider))) {
        if (!agentc_setup_provider(provider))
            return -22;
        agentc_logf(2, "ext: set_model: provider switch requires the RPC command");
        return -38; /* ENOSYS: no safe rebuild from this sink */
    }
    return agentc_agent_set_model(c->agent, model);
}

/* App thinking service: only the CLI/RPC names are accepted; an unknown name
 * is logged and ignored. The agent setter emits `thinking_level_select` on a
 * numeric change. */
static void mode_ext_set_thinking(void *ud, const char *level) {
    AgcModeCtx *c = ud;
    if (!c || !c->agent)
        return;
    int v;
    if (level && agentc_streq(level, "off"))
        v = 0;
    else if (level && agentc_streq(level, "low"))
        v = 1;
    else if (level && agentc_streq(level, "medium"))
        v = 3;
    else if (level && agentc_streq(level, "high"))
        v = 4;
    else {
        agentc_logf(2, "ext: set_thinking: unknown level '%s'", level ? level : "");
        return;
    }
    agentc_agent_set_thinking(c->agent, v);
}

/* The one main-loop pump duty. agentc_mode_setup installs this as both the
 * wire poll hook and the agent pump for every mode, so deferred extension work
 * (MCP connect/re-sync) runs during blocking HTTP waits, tool runs and idle
 * loops. A front end with its own poll hook (the TUI) chains through it instead
 * of installing a competing hook. The re-entrancy guard lives here. */
void agentc_mode_pump(void *ud, int timeout_ms) {
    (void)ud;
    (void)timeout_ms;
    agentc_ext_pump();
    /* R-D17: the bus and actor registry advance during every blocking wait, so
     * a preempt/notify/approval can never deadlock the foreground loop. */
    agentc_runtime_tick();
}

/* --------------------------------------------------------------- setup */

static void mode_configure_agent(AgcAgent *a, const AgcModeConfig *cfg) {
    agentc_agent_set_api_key(a, cfg->api_key);
    if (cfg->base_url)
        agentc_agent_set_base_url(a, cfg->base_url);
    agentc_agent_set_thinking(a, cfg->thinking);
    agentc_agent_set_max_tokens(a, cfg->max_tokens);
    agentc_agent_set_retry(a, cfg->max_attempts);
    agentc_agent_set_insecure(a, cfg->insecure);
    agentc_agent_set_tools(a, cfg->tools, cfg->ntools);
    agentc_agent_set_system(a, cfg->system); /* NULL = auto/build per turn */
    agentc_agent_set_recompose(a, cfg->recompose, cfg->recompose_ud);
    agentc_agent_set_auto_compact(a, cfg->auto_compact);
    agentc_agent_set_compact_limits(a, cfg->compact_reserve, cfg->compact_keep);
    agentc_agent_set_tools_hooks(a, agentc_config_tools_hooks(cfg->cfg));
    agentc_agent_set_trust(a, agentc_trust_current());
    agentc_agent_set_transport(a, agentc_transport_http(cfg->insecure));
}

/* Build/publish the extension context and entry sink for `c` (P6-C). Shares the
 * one system-prompt build with agentc_mode_setup so activation cannot drift. */
static void mode_install_ext_context(AgcModeCtx *c) {
    if (!c || !c->mcfg)
        return;
    const AgcModeConfig *cfg = c->mcfg;
    AgcExtContext pctx;
    agentc_memset(&pctx, 0, sizeof pctx);
    /* Publish the prompt the first request will carry: an explicit one, or the
     * same build the agent will refresh per turn in auto mode. */
    char *initial_system = NULL;
    const char *ctx_system = cfg->system;
    if (!ctx_system) {
        initial_system = agentc_prompt_build(cfg->tools, cfg->ntools);
        ctx_system = initial_system ? initial_system : "";
    }
    pctx.cwd = cfg->session.cwd;
    pctx.session_id = c->session ? agentc_session_id(c->session) : "";
    pctx.session_file = c->session ? agentc_session_path(c->session) : NULL;
    pctx.system_prompt = ctx_system;
    agentc_ext_set_context(&pctx);
    agentc_free(initial_system);
    agentc_ext_set_entry_sink(ext_entry_sink, c->session);
    if (c->agent)
        agentc_agent_set_tool_veto(c->agent, agentc_ext_tool_veto, NULL);
}

static bool mode_route_target(void *ud, const char *provider, AgcRouteTarget *out);

/* Rebind every process-global service to `c` (see runsession.h). The wire poll
 * hook and the agent pump are deliberately excluded: the front end owns them. */
void agentc_mode_activate(AgcModeCtx *c) {
    if (!c)
        return;
    agentc_ext_set_model_sink(mode_ext_set_model, c);
    agentc_ext_set_thinking_sink(mode_ext_set_thinking, c);
    if (c->flags & AGENTC_MODE_F_ABORT)
        mode_install_ext_context(c);
    if (c->trust)
        agentc_trust_set_current(c->trust);
    agentc_trust_set_audit(mode_trust_audit, c->session);
    if (c->flags & AGENTC_MODE_F_SESSION)
        c->persist_ok = true;
    if (c->agent && c->io && c->io->event)
        agentc_agent_set_events(c->agent, c->io->event, c->io->event_ud);
}

/* P8-D1 (B-M8): the mode-layer provider resolver. The setup context is one
 * process-wide catalog, but the seam rides the agent, so route_resolve reads no
 * global and a ctx's teardown can never clear another's resolution. */
static const AgcProvider *mode_route_provider(void *ud, const char *name) {
    (void)ud;
    return agentc_setup_provider(name);
}

/* P7-W4: a routed target provider resolves its endpoint and credential through
 * the same config-aware path the primary provider uses (OAuth > provider env >
 * auth.jsonc > config api_keys; config base_url). The CLI --api-key belongs to
 * the primary provider, so it is not passed here. */
static bool mode_route_target(void *ud, const char *provider, AgcRouteTarget *out) {
    AgcModeCtx *c = ud;
    if (c == NULL || provider == NULL || out == NULL)
        return false;
    out->api_key = agentc_setup_resolve_key(c->cfg, provider, NULL);
    out->base_url = agentc_config_base_url(c->cfg, provider);
    return true;
}

int agentc_mode_setup(AgcModeCtx *c, const AgcModeConfig *cfg, const AgcModeIo *io,
                      unsigned flags) {
    if (!c || !cfg)
        return -22;
    agentc_memset(c, 0, sizeof *c);
    c->cfg = cfg->cfg;
    c->mcfg = cfg;
    c->io = io;
    c->flags = flags;

    /* Front-end model/thinking services. They close over the mode context, so a
     * later rebuild (new AgcAgent) keeps them effective; teardown clears them. */
    agentc_ext_set_model_sink(mode_ext_set_model, c);
    agentc_ext_set_thinking_sink(mode_ext_set_thinking, c);

    /* The one pump duty: every mode gets it as the wire poll hook and the
     * agent pump, so extension work runs during HTTP waits, tools and idle
     * loops. Teardown clears both. */
    agentc_http_set_poll_hook(agentc_mode_pump, NULL);
    agentc_pump_install(agentc_mode_pump, NULL);

    c->provider = agentc_strdup((cfg->provider && cfg->provider[0]) ? cfg->provider : "openai");
    c->model = agentc_strdup(cfg->model ? cfg->model : "");
    if (!c->provider || !c->model) {
        agentc_mode_teardown(c);
        return -12;
    }

    if (flags & AGENTC_MODE_F_SESSION) {
        int rc = mode_session_create(cfg, &c->session, &c->session_resumed);
        if (rc != 0) {
            agentc_mode_teardown(c);
            return rc;
        }
        /* Arm persistence before the root agent is created so the root
         * ACTOR_STARTED becomes a `custom type:"agent"` record (P6 completion
         * pass 2); the session header already exists. Re-arm after an
         * agentc_events_reset() cleared the hub: the table, not a cached id, is
         * the source of truth. */
        if (!agentc_events_subscribed(mode_queue_observe))
            (void)agentc_events_subscribe(mode_queue_observe, NULL);
        if (!agentc_events_subscribed(mode_agent_observe))
            (void)agentc_events_subscribe(mode_agent_observe, NULL);
        if (!agentc_events_subscribed(mode_model_observe))
            (void)agentc_events_subscribe(mode_model_observe, NULL);
        c->persist_ok = true;
    }

    const AgcProvider *p = agentc_setup_provider(c->provider);
    if (!p)
        p = agentc_prov_openai();
    AgcAgentSpec aspec;
    agentc_memset(&aspec, 0, sizeof aspec);
    aspec.provider_handle = p;
    aspec.provider = c->provider;
    aspec.model = c->model;
    aspec.thinking = -1;
    aspec.label = "agent";
    aspec.flags = AGENTC_AGENT_F_WAIT;
    /* P8-D1 (B-M8): install the per-agent route seam + owner *before* create, so
     * the root ACTOR_STARTED fired inside create is persisted to this ctx. */
    aspec.route = mode_seam(c);
    aspec.owner = c;
    c->agent = agentc_agent_create(&aspec);
    if (!c->agent) {
        agentc_mode_teardown(c);
        return -12;
    }
    /* P6-C: resolve the effective envelope once and install it as the root
     * ceiling and the current envelope (process spawns resolve it too). */
    char cwdbuf[4096];
    const char *tcwd = cfg->session.cwd;
    if (!tcwd || !tcwd[0]) {
        (void)os_getcwd(cwdbuf, sizeof cwdbuf);
        tcwd = cwdbuf;
    }
    c->trust = agentc_alloc(sizeof *c->trust);
    if (agentc_trust_load(tcwd, false, c->trust) != 0)
        (void)agentc_trust_default(c->trust, tcwd);
    agentc_trust_set_current(c->trust);
    agentc_trust_set_audit(mode_trust_audit, c->session);
    mode_configure_agent(c->agent, cfg);

    if (flags & AGENTC_MODE_F_ABORT) {
        mode_install_ext_context(c);
    }

    /* Resume only after the extension context is set, matching the historical
     * main.c order (the replay must not reach the observer). */
    if (c->session && (cfg->session_spec || cfg->continue_last) && !cfg->session.memory_only) {
        AgcTranscript tmp;
        agentc_transcript_init(&tmp);
        (void)agentc_session_load_messages(c->session, &tmp);
        if (tmp.n)
            (void)agentc_agent_load(c->agent, &tmp);
        agentc_transcript_free(&tmp);
    }
    const AgcTranscript *tp = agentc_agent_transcript(c->agent);
    c->flushed = tp ? tp->n : 0;

    /* Reconstruct the read-only persisted tree for a resumed session. */
    if (c->session && (cfg->session_spec || cfg->continue_last) && !cfg->session.memory_only)
        agentc_mode_load_agent_tree(c);

    /* Bind the root agent actor (P6-B, C-D8) and arm auto-start from config.
     * The bus/queue/steer surfaces are live for the app's whole lifetime; the
     * agent free releases the actor, so teardown/rebuild need no extra call. */
    (void)agentc_agent_bind_actor(c->agent, 0, "agent");
    agentc_agent_set_auto_start(c->agent, agentc_config_queue_auto_start(cfg->cfg));

    AgcMsgObserver obs = (io && io->observer) ? io->observer : mode_flush_observer;
    void *obs_ud = (io && io->observer) ? io->observer_ud : c;
    agentc_agent_set_observer(c->agent, obs, obs_ud);

    if (flags & AGENTC_MODE_F_ABORT) {
        /* Resume/continue: the session being picked up is the previous one. */
        agentc_mode_emit_session_start(
            c, (cfg->session_spec || cfg->continue_last) ? "resume" : "startup",
            c->session_resumed && c->session ? agentc_session_path(c->session) : NULL);
    }

    if (io && io->event)
        agentc_agent_set_events(c->agent, io->event, io->event_ud);

    /* Queue persistence owner + subscriber (SIG §5.6) were armed before the
     * root agent was created; the restore runs last so the bound actor exists. */
    mode_restore_queue(c);
    return 0;
}

void agentc_mode_teardown(AgcModeCtx *c) {
    if (!c)
        return;
    /* Stop this ctx's queue persistence before the agent's actor (and its queue)
     * is released, so a teardown does not append to a session being closed. Per
     * ctx (P8-D1): never disarms another live session's persistence. */
    c->persist_ok = false;
    /* setup installed the entry/session and model/thinking sinks; clear them
     * before the context they close over goes away */
    agentc_ext_set_entry_sink(NULL, NULL);
    agentc_ext_set_model_sink(NULL, NULL);
    agentc_ext_set_thinking_sink(NULL, NULL);
    agentc_http_set_poll_hook(NULL, NULL);
    agentc_pump_install(NULL, NULL);
    agentc_trust_set_audit(NULL, NULL);
    if (c->trust && agentc_trust_current() == c->trust)
        agentc_trust_set_current(NULL);
    agentc_agent_free(c->agent);
    agentc_session_close(c->session);
    if (c->persisted_agents) {
        agentc_session_agents_free(c->persisted_agents, c->npersisted_agents);
        c->persisted_agents = NULL;
        c->npersisted_agents = 0;
    }
    agentc_free(c->provider);
    agentc_free(c->model);
    if (c->trust) {
        agentc_trust_env_free(c->trust);
        agentc_free(c->trust);
    }
    agentc_memset(c, 0, sizeof *c);
}
