/* mode.c — shared mode mechanics (see app/mode.h).
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
#include "app/mode.h"

#include "app/setup.h"
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

/* Every JSON document these front ends emit is ASCII-only: non-ASCII
 * bytes become \\uXXXX escapes. Provider request writers are separate and keep
 * raw UTF-8 (the default). */
static void jsonw_out(AgcJsonW *w, AgcBuf *b) {
    agentc_jsonw_init(w, b);
    agentc_jsonw_set_ascii(w, true);
}

bool agentc_mode_msg_persistable(const AgcMsg *m) {
    if (!m || m->role == AGENTC_ROLE_SYSTEM) return false;
    return !(m->role == AGENTC_ROLE_ASSISTANT &&
             (m->stop_reason == AGENTC_STOP_ERROR || m->stop_reason == AGENTC_STOP_ABORTED));
}

const char *agentc_mode_stop_name(int stop) {
    switch (stop) {
    case AGENTC_STOP_STOP: return "stop";
    case AGENTC_STOP_LENGTH: return "length";
    case AGENTC_STOP_TOOLUSE: return "tool_use";
    case AGENTC_STOP_ERROR: return "error";
    case AGENTC_STOP_ABORTED: return "aborted";
    default: return "pending";
    }
}

static const char *mode_role_name(int role) {
    switch (role) {
    case AGENTC_ROLE_USER: return "user";
    case AGENTC_ROLE_ASSISTANT: return "assistant";
    case AGENTC_ROLE_TOOL: return "tool";
    default: return "system";
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
    if (m->error) agentc_jsonw_cstr(&w, m->error);
    else agentc_jsonw_null(&w);
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
    if (n && c->io && c->io->write) c->io->write(c->io->out_ud, p, n);
}

void agentc_mode_out_line(AgcModeCtx *c, AgcBuf *b) {
    agentc_buf_byte(b, '\n');
    agentc_mode_out(c, b->p, b->len);
    agentc_buf_free(b);
}

void agentc_mode_write_session_header(AgcModeCtx *c) {
    AgcBuf b = { 0 };
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
    if (!c || !c->session) return;
    const AgcTranscript *tp = agentc_agent_transcript(c->agent);
    if (!tp) return;
    /* A compaction splice shrinks the transcript below the recorded flush
     * index; re-sync before iterating or every later message is skipped. */
    if (tp->n < c->flushed) c->flushed = tp->n;
    for (; c->flushed < tp->n; c->flushed++) {
        const AgcMsg *m = &tp->msgs[c->flushed];
        if (!agentc_mode_msg_persistable(m)) continue;
        (void)agentc_session_append_message(c->session, m);
    }
}

/* Default observer: persist each finalized transcript message. Front ends that
 * need their own policy (print/TUI) inject one through AgcModeIo. */
static void mode_flush_observer(void *ud, const AgcMsg *m) {
    (void)m;
    agentc_mode_session_flush(ud);
}

void agentc_mode_append_compaction(AgcSession *s, const AgcCompactInfo *ci) {
    (void)agentc_session_append_compaction(s, ci);
}

void agentc_mode_note_compaction(AgcModeCtx *c, const AgcCompactInfo *ci) {
    if (!c || !ci || !c->session) return;
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
            if (agentc_mode_msg_persistable(&tp->msgs[i])) persistable++;
        rec.kept_messages = (u32)persistable;
    }
    agentc_mode_append_compaction(c->session, &rec);
    c->flushed = (size_t)ci->kept_messages + 1;
}

static void mode_event_common(AgcModeCtx *c, int ev, const void *data) {
    AgcBuf b = { 0 };
    AgcJsonW w;
    switch (ev) {
    case AGENTC_EV_AGENT_START:
        agentc_buf_cstr(&b, "{\"type\":\"agent_start\"}");
        break;
    case AGENTC_EV_TURN_START:
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
        const char *kind = ev == AGENTC_EV_TEXT_DELTA ? "text"
                           : ev == AGENTC_EV_THINK_DELTA ? "thinking" : "tool_args";
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
        AgcBuf mb = { 0 };
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
        break;   /* the session observer already persisted the message */
    }
    case AGENTC_EV_MSG_RESET:
        /* A retried attempt replaces what already streamed for the current
         * message; JSON/RPC consumers discard its deltas on this marker. */
        agentc_buf_cstr(&b, "{\"type\":\"message_reset\"}");
        break;
    case AGENTC_EV_TURN_END:
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
        if (ci->summary) agentc_jsonw_cstr(&w, ci->summary);
        else agentc_jsonw_null(&w);
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
    if (!p) p = agentc_prov_openai();
    /* The old transcript borrows the old agent's strings; snapshot them for the
     * change check and the payload before the old agent is freed. */
    const AgcTranscript *old = agentc_agent_transcript(c->agent);
    const char *old_provider = (old && old->provider) ? old->provider : "";
    const char *old_model = (old && old->model) ? old->model : "";
    AgcTransport transport;
    agentc_memset(&transport, 0, sizeof transport);
    if (c->agent) transport = agentc_agent_transport(c->agent);
    AgcAgent *na = agentc_agent_new(p, c->model);
    if (!na) return NULL;
    mode_configure_agent(na, c->mcfg);
    /* mode_configure_agent wires the production HTTP transport; an injected one
     * (tests/embedders) survives the swap, exactly like the hooks below. */
    if (transport.request) agentc_agent_set_transport(na, transport);
    const AgcTranscript *nt = agentc_agent_transcript(na);
    const char *new_provider = (nt && nt->provider) ? nt->provider : "";
    const char *new_model = (nt && nt->model) ? nt->model : "";
    bool changed = !agentc_streq(new_provider, old_provider) ||
                   !agentc_streq(new_model, old_model);
    bool emit = changed && agentc_ext_wants("model_select");
    char *previous = emit ? agentc_strdup(old_model) : NULL;
    (void)agentc_agent_load(na, agentc_agent_transcript(c->agent));
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
        AgcBuf pb = { 0 };
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
        if (*p == '/') b = p + 1;
    return b;
}

static bool id_matches(const char *path, const char *id) {
    const char *b = base_name(path);
    if (!ends_with(b, ".jsonl")) return false;
    size_t bl = agentc_strlen(b);
    size_t stem = bl - 6;
    size_t il = agentc_strlen(id);
    if (stem == il && agentc_str_eq(b, il, id, il)) return true;
    if (stem >= il + 1 && b[stem - il - 1] == '_' && agentc_str_eq(b + stem - il, il, id, il))
        return true;
    return false;
}

static char *resolve_session_arg(const char *dir, const char *spec) {
    if (!spec || !spec[0]) return NULL;
    if (agentc_str_str(spec, "/")) return agentc_strdup(spec);
    if (ends_with(spec, ".jsonl")) {
        char full[4096];
        if (dir && dir[0] && agentc_path_join(full, sizeof full, dir, spec)) return agentc_strdup(full);
        return agentc_strdup(spec);
    }
    size_t n = 0;
    char **list = agentc_session_list(dir, &n, 0);
    char *found = NULL;
    for (size_t i = 0; i < n && !found; i++)
        if (id_matches(list[i], spec)) found = agentc_strdup(list[i]);
    agentc_sessions_free(list, n);
    return found;
}

/* Create or reopen the session selected by the mode config. Returns 0, -2 for
 * a user-visible --session failure (the caller maps it to exit 2) or -12. */
static int mode_session_create(const AgcModeConfig *cfg, AgcSession **out, bool *resumed) {
    *out = NULL;
    if (resumed) *resumed = false;
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
        if (resumed) *resumed = true;
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
        if (!s) return -12;
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
    if (!s) return;
    AgcBuf line = { 0 };
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

void agentc_mode_emit_session_start(AgcModeCtx *c, const char *reason,
                                    const char *previous_session_file) {
    if (!c || !agentc_ext_wants("session_start")) return;
    AgcBuf pb = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &pb);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "reason");
    agentc_jsonw_cstr(&w, reason ? reason : "startup");
    agentc_jsonw_key(&w, "session_id");
    agentc_jsonw_cstr(&w, c->session ? agentc_session_id(c->session) : "");
    agentc_jsonw_key(&w, "session_file");
    agentc_jsonw_cstr(&w, (c->session && agentc_session_path(c->session))
                              ? agentc_session_path(c->session)
                              : "");
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
    if (!c) return;
    /* The extension context and entry sink are part of the cooperative-abort
     * wiring installed at setup. Other context fields (cwd, system_prompt) are
     * preserved: agentc_ext_set_context() ignores NULL and an empty
     * session_file clears it. */
    if (c->flags & AGENTC_MODE_F_ABORT) {
        AgcExtContext ctx;
        agentc_memset(&ctx, 0, sizeof ctx);
        ctx.session_id = c->session ? agentc_session_id(c->session) : "";
        ctx.session_file = (c->session && agentc_session_path(c->session))
                               ? agentc_session_path(c->session)
                               : "";
        agentc_ext_set_context(&ctx);
        agentc_ext_set_entry_sink(ext_entry_sink, c->session);
    }
    /* The persistence owner must be this context: an RPC session swap frees
     * the session an older ctx-bound observer still points at. */
    AgcMsgObserver obs = (c->io && c->io->observer) ? c->io->observer : mode_flush_observer;
    void *obs_ud = (c->io && c->io->observer) ? c->io->observer_ud : c;
    if (c->agent) agentc_agent_set_observer(c->agent, obs, obs_ud);
}

/* `session_before_switch` (override/first/replace/fail-closed): returns true
 * to cancel the pending switch. `{"cancel":true}`, a blocked/failed handler, a
 * handled result with no boolean decision, or a malformed `cancel` field all
 * cancel; a well-formed `{"cancel":false}` allows, whether or not the handler
 * claimed `handled`. Fail-closed is sticky: once any handler blocks/fails, an
 * earlier well-formed `{"cancel":false}` accumulated into `result_json` must
 * not re-open the switch. */
static bool session_switch_cancelled(void) {
    if (!agentc_ext_wants("session_before_switch")) return false;
    AgcExtResult sr = agentc_ext_emit("session_before_switch", "{\"reason\":\"new\"}");
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
            cancel = true;   /* malformed decision */
        } else {
            const AgcJson *cv = agentc_json_get(o, "cancel");
            if (cv) {
                int ct = agentc_json_type(cv);
                if (ct != AGENTC_JSON_TRUE && ct != AGENTC_JSON_FALSE) {
                    cancel = true;   /* malformed decision */
                } else {
                    decision = true;
                    cancel = (ct == AGENTC_JSON_TRUE);
                }
            }
        }
        agentc_json_arena_free(ja);
    }
    if (sr.handled && !decision) cancel = true;   /* handled without a decision */
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
    if (!c || !c->mcfg) return -22;
    if (!c->session) return -22;
    /* The switch is cancellable before any file is touched: a cancel leaves
     * the current session, its transcript and the extension context intact. */
    if (session_switch_cancelled()) return -125;   /* -ECANCELED */
    /* Create the replacement before dropping the old session so an allocation
     * failure cannot leave the context with no session at all. */
    AgcSession *fresh = agentc_session_new(&c->mcfg->session);
    if (!fresh) return -12;
    /* clear the chat transcript for the new session (no append, so the old
     * observer cannot write); rebind happens in mode_install_session */
    AgcTranscript empty;
    agentc_transcript_init(&empty);
    (void)agentc_agent_load(c->agent, &empty);
    agentc_transcript_free(&empty);
    mode_install_session(c, fresh, "new");
    c->flushed = 0;
    return 0;
}

int agentc_mode_resume_session(AgcModeCtx *c, const char *path) {
    if (!c || !c->mcfg || !c->agent || !path || !path[0]) return -22;
    if (!c->session || c->mcfg->session.memory_only) return -22;
    AgcSession *s = agentc_session_open(path);
    if (!s) return -2;   /* ENOENT */
    AgcTranscript tr;
    agentc_transcript_init(&tr);
    if (agentc_session_load_messages(s, &tr) != 0) {
        agentc_transcript_free(&tr);
        agentc_session_close(s);
        return -5;       /* EIO */
    }
    /* Replace the agent transcript before rebinding persistence: the load does
     * not append, so the old observer never sees it. */
    (void)agentc_agent_load(c->agent, &tr);
    size_t loaded = tr.n;
    agentc_transcript_free(&tr);
    mode_install_session(c, s, "resume");
    c->flushed = loaded;
    return 0;
}

/* App model service: a same-provider switch goes through the agent (which
 * emits `model_select`). A provider switch is refused here: it needs an idle
 * rebuild, which only the RPC command path owns; rebuilding here could free the
 * agent that is executing a tool or that a front end still holds. */
static int mode_ext_set_model(void *ud, const char *provider, const char *model) {
    AgcModeCtx *c = ud;
    if (!c || !model || !model[0]) return -22;
    if (provider && provider[0] && (!c->provider || !agentc_streq(provider, c->provider))) {
        if (!agentc_setup_provider(provider)) return -22;
        agentc_logf(2, "ext: set_model: provider switch requires the RPC command");
        return -38;   /* ENOSYS: no safe rebuild from this sink */
    }
    return agentc_agent_set_model(c->agent, model);
}

/* App thinking service: only the CLI/RPC names are accepted; an unknown name
 * is logged and ignored. The agent setter emits `thinking_level_select` on a
 * numeric change. */
static void mode_ext_set_thinking(void *ud, const char *level) {
    AgcModeCtx *c = ud;
    if (!c || !c->agent) return;
    int v;
    if (level && agentc_streq(level, "off")) v = 0;
    else if (level && agentc_streq(level, "low")) v = 1;
    else if (level && agentc_streq(level, "medium")) v = 3;
    else if (level && agentc_streq(level, "high")) v = 4;
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
}

/* --------------------------------------------------------------- setup */

static void mode_configure_agent(AgcAgent *a, const AgcModeConfig *cfg) {
    agentc_agent_set_api_key(a, cfg->api_key);
    if (cfg->base_url) agentc_agent_set_base_url(a, cfg->base_url);
    agentc_agent_set_thinking(a, cfg->thinking);
    agentc_agent_set_max_tokens(a, cfg->max_tokens);
    agentc_agent_set_retry(a, cfg->max_attempts);
    agentc_agent_set_insecure(a, cfg->insecure);
    agentc_agent_set_tools(a, cfg->tools, cfg->ntools);
    agentc_agent_set_system(a, cfg->system);   /* NULL = auto/build per turn */
    agentc_agent_set_recompose(a, cfg->recompose, cfg->recompose_ud);
    agentc_agent_set_auto_compact(a, cfg->auto_compact);
    agentc_agent_set_compact_limits(a, cfg->compact_reserve, cfg->compact_keep);
    agentc_agent_set_transport(a, agentc_transport_http(cfg->insecure));
}

int agentc_mode_setup(AgcModeCtx *c, const AgcModeConfig *cfg, const AgcModeIo *io,
                      unsigned flags) {
    if (!c || !cfg) return -22;
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
    }

    const AgcProvider *p = agentc_setup_provider(c->provider);
    if (!p) p = agentc_prov_openai();
    c->agent = agentc_agent_new(p, c->model);
    if (!c->agent) {
        agentc_mode_teardown(c);
        return -12;
    }
    mode_configure_agent(c->agent, cfg);

    if (flags & AGENTC_MODE_F_ABORT) {
        AgcExtContext pctx;
        agentc_memset(&pctx, 0, sizeof pctx);
        /* Publish the prompt the first request will carry: an explicit one, or
         * the same build the agent will refresh per turn in auto mode. */
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
        agentc_agent_set_tool_veto(c->agent, agentc_ext_tool_veto, NULL);
    }

    /* Resume only after the extension context is set, matching the historical
     * main.c order (the replay must not reach the observer). */
    if (c->session && (cfg->session_spec || cfg->continue_last) && !cfg->session.memory_only) {
        AgcTranscript tmp;
        agentc_transcript_init(&tmp);
        (void)agentc_session_load_messages(c->session, &tmp);
        if (tmp.n) (void)agentc_agent_load(c->agent, &tmp);
        agentc_transcript_free(&tmp);
    }
    const AgcTranscript *tp = agentc_agent_transcript(c->agent);
    c->flushed = tp ? tp->n : 0;

    AgcMsgObserver obs = (io && io->observer) ? io->observer : mode_flush_observer;
    void *obs_ud = (io && io->observer) ? io->observer_ud : c;
    agentc_agent_set_observer(c->agent, obs, obs_ud);

    if (flags & AGENTC_MODE_F_ABORT) {
        /* Resume/continue: the session being picked up is the previous one. */
        agentc_mode_emit_session_start(
            c, (cfg->session_spec || cfg->continue_last) ? "resume" : "startup",
            c->session_resumed && c->session ? agentc_session_path(c->session) : NULL);
    }

    if (io && io->event) agentc_agent_set_events(c->agent, io->event, io->event_ud);
    return 0;
}

void agentc_mode_teardown(AgcModeCtx *c) {
    if (!c) return;
    /* setup installed the entry/session and model/thinking sinks; clear them
     * before the context they close over goes away */
    agentc_ext_set_entry_sink(NULL, NULL);
    agentc_ext_set_model_sink(NULL, NULL);
    agentc_ext_set_thinking_sink(NULL, NULL);
    agentc_http_set_poll_hook(NULL, NULL);
    agentc_pump_install(NULL, NULL);
    agentc_agent_free(c->agent);
    agentc_session_close(c->session);
    agentc_free(c->provider);
    agentc_free(c->model);
    agentc_memset(c, 0, sizeof *c);
}
