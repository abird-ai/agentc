/* mode_serve.c — `agentc serve`: the framed CBOR session server (P5 Axis C).
 *
 * One process owns a bounded registry of independent engines (C-D4) and a
 * unix-domain (or loopback-TCP) listener (C-D3). Connections perform a `hello`
 * handshake, then send `request` envelopes; responses go to the requesting
 * connection and agent events are broadcast to every attached connection.
 * Commands are decoded through the shared RPC dispatcher (app/rpc_dispatch.c),
 * so `serve` and `--mode rpc` cannot drift.
 *
 * C2 makes the server non-blocking at the event/pump seams:
 *   - every connection has a bounded output buffer; a slow consumer that
 *     overflows it is closed (that connection only, C-D7);
 *   - a `request` that arrives during a run is decoded and queued (a prompt is
 *     answered `{"disposition":"queued"}`), then dispatched after that
 *     session settles; the `abort` method is still immediate (A-D3);
 *   - the out-of-band `cancel` envelope aborts the run of the connection's own
 *     session immediately (C-D9); it is read by an input pump installed as the
 *     agent pump and the wire poll hook, so a stalled HTTP/SSE read can be
 *     interrupted.
 *   - an envelope violation (unknown property/kind, malformed CBOR) is terminal
 *     for that connection only (C-D7).
 *
 * P5 multi-session concurrency: the dispatcher is put in `defer_run` mode, so a
 * prompt starts a resumable run (agentc_agent_run_begin) instead of driving it
 * to completion. The poll loop advances each session's active run by one bounded
 * model-step slice per iteration (serve_advance_runs), rebinding the
 * process-global services to that session first (agentc_mode_activate). A run on
 * one session therefore never blocks a run on another; cancelling one touches
 * only that session's agent, and a settle frees its slice. The wire poll hook and
 * agent pump stay the server's own. See `.agents/design/61-protocol.md` §7.1.
 */
#include "app/mode.h"
#include "app/rpc_dispatch.h"

#include "cbor.h"
#include "cbor_json.h"
#include "frame.h"
#include "net.h"
#include "plat.h"
#include "protocol.h"
#include "ws.h"

#define SERVE_MAX_CONNS 8u
#define SERVE_BACKLOG 16
/* Bounded multi-session registry (P5 C3). Each slot owns an independent engine
 * (agent + session/transcript); execution stays serialized. */
#define SERVE_MAX_SESSIONS 4u
/* Pending request queue (bounded, fail-closed). The queue only grows while a
 * run is active; a connection that keeps sending faster than the run drains is
 * refused `busy` once the cap is reached. */
#define SERVE_PENDING_MAX 256u
/* Per-connection output cap (C-D12: 32 MiB pending). One slow consumer can
 * never grow the server without bound; the connection is closed instead. */
#define SERVE_OUT_MAX (32u * 1024u * 1024u)
/* At most this many dialogs may be parked; a new one is refused `busy`
 * (fail-closed). One dialog is expected per connection. */
#define SERVE_DIALOG_MAX 16u
/* A parked dialog that is never answered denies after this bound. */
#define SERVE_DIALOG_TIMEOUT_MS 30000
/* Default loopback port for a bare `--host ADDR` (P5 C3). */
#define SERVE_DEFAULT_TCP_PORT 8790
/* S8: a connection that has not completed hello within this window is reaped;
 * a peer that stalls after hello is reaped once it is idle this long. Both are
 * overridable for tests via AGENTC_SERVE_(HELLO|IDLE)_TIMEOUT_MS. */
#define SERVE_HELLO_TIMEOUT_MS 10000
#define SERVE_IDLE_TIMEOUT_MS 600000
/* N1: upper bound for the test overrides. `ms * 1000000` must stay well inside
 * i64, so a bogus AGENTC_SERVE_*_TIMEOUT_MS cannot wrap the deadline negative
 * (which would reap every connection immediately). 24 h. */
#define SERVE_TIMEOUT_MAX_MS (24LL * 60LL * 60LL * 1000LL)
/* S9: most bytes one connection may feed in a single serve_service_input call.
 * The read loop is bounded so a fast sender cannot monopolize the single
 * threaded loop; level-triggered POLLIN services the remainder next pass. */
#define SERVE_READ_BUDGET (256u * 1024u)

/* Graceful shutdown: the signal handler only latches this flag (async-signal-
 * safe, no allocation), and the serve loop/dialog pump polls it. On return the
 * normal teardown unlinks the socket and releases every session. */
static volatile int g_serve_stop;

static void serve_signal(int sig) {
    (void)sig;
    g_serve_stop = 1;
}

static const char RPC_QUEUED[] = "{\"disposition\":\"queued\"}";

typedef struct Serve Serve;

/* One session slot: an independent agent/session engine. `rpc` is first so its
 * AgcModeCtx base sits at offset 0 and the shared mode callbacks (which cast
 * their userdata) keep working; `io`/`capture` are per-session so events from
 * one session never leak into another's framing. */
typedef struct ServeSession {
    AgcRpc rpc;
    Serve *s;
    AgcModeIo io;
    AgcBuf capture;
    char id[64];
    u32 index;
    u64 run_conn_id; /* connection owning the in-flight run (0 = none);
                      * routing survives a conn reap (stable id) */
    bool used;
} ServeSession;

typedef struct {
    int fd;
    u64 id; /* stable connection identity (action routing) */
    AgcFrameDecoder dec;
    AgcBuf out;
    bool hello_ok;
    bool dead;
    /* WebSocket transport (P5 C3): handshake buffer, then a message decoder.
     * A WS binary message carries one framed-protocol record (u32 length +
     * CBOR), so `dec` is reused unchanged after the upgrade. */
    bool ws;
    bool ws_ready;
    AgcBuf ws_in;
    AgcWsDecoder ws_dec;
    /* Attachment routing (P5 C3, C-D4): every handshaken connection is
     * attached to the process's primary session by default. `detach` removes
     * it from the event broadcast; `attach` (re)subscribes, selecting the
     * session (`session`) and optionally narrowing to a list of event types. */
    u32 session; /* attached session index (routing target) */
    bool attached;
    char *ev_filter[AGENTC_PROTO_MAX_ATTACH_EVENTS];
    size_t nev_filter;
    /* TLS transport (P5 C3): per-connection server endpoint. */
    AgcTls *tls;
    bool tls_ready;
    short tls_want; /* 0 none, 1 read, 2 write (mirrors agentc_tls_want) */
    i64 deadline_ns; /* S8: absolute handshake deadline before hello_ok, then a
                      * sliding idle deadline reset on each read */
} ServeConn;

/* One decoded request awaiting dispatch. The owning connection is referenced by
 * its stable id so a reap cannot leave a dangling pointer. */
typedef struct {
    u64 conn_id;
    char *line; /* owned JSON command record */
    bool acked; /* already answered `queued` */
} ServeAction;

/* One parked UI dialog (P5 C2). `client_req` dialogs were raised by a
 * `ui_prompt` request and expect a `response` when answered; pipeline dialogs
 * are polled by serve_ui_prompt()'s pump loop. `value` is the answer as owned
 * JSON text; `req_id` is the owned request-id JSON for a client request. */
typedef struct {
    bool used;
    u64 id; /* wire dialog id (a uint) */
    u64 conn_id;
    bool client_req;
    char *req_id;
    bool answered;
    bool ok;
    char *value;
    size_t value_len;
    char text[AGENTC_PROTO_MAX_UI_PROMPT];
    i64 deadline_ns;
} ServeDialog;

typedef struct Serve {
    ServeSession sessions[SERVE_MAX_SESSIONS];
    size_t nsessions;
    AgcModeCtx *base; /* the caller's engine (session 0) */
    const AgcModeConfig *base_mcfg;
    const AgcModeIo *orig_io;
    ServeConn conns[SERVE_MAX_CONNS];
    size_t nconns;
    u64 next_conn_id;
    int lfd;
    char server_id[37];
    char transport[8];
    char path[560];
    const char *token;
    ServeConn *current;            /* connection owning the in-flight response */
    ServeSession *current_session; /* session owning the in-flight response */
    bool busy;                     /* a dispatch or run slice is on the stack; a
                                    * re-entrant input service must not drain/start */
    u64 seq;
    bool shutdown;
    AgcVec actions; /* ServeAction queue (head..len pending) */
    size_t actions_head;
    u64 dialog_seq;
    ServeDialog dialogs[SERVE_DIALOG_MAX];
    bool ws;         /* the listener speaks WebSocket (C3) */
    bool ws_loopback; /* the WS bind is a loopback address (Origin/Host policy) */
    bool tls;        /* the listener speaks TLS (C3) */
    AgcBuf tls_cert; /* owned PEM buffers */
    AgcBuf tls_key;
    i64 hello_timeout_ms; /* S8 */
    i64 idle_timeout_ms;  /* S8 */
} Serve;

static void serve_conn_touch(ServeConn *c, Serve *s);

/* --------------------------------------------------------------- framing */

static bool serve_out_room(ServeConn *c, size_t n) {
    size_t used = c->out.len < SERVE_OUT_MAX ? c->out.len : SERVE_OUT_MAX;
    if (n > SERVE_OUT_MAX - used) {
        c->dead = true; /* slow-consumer overflow: close it */
        return false;
    }
    return true;
}

/* Raw append (handshake response / control frames): never WS-wrapped. */
static void serve_conn_push_raw(ServeConn *c, const void *p, size_t n) {
    if (c->dead)
        return;
    if (!serve_out_room(c, n))
        return;
    agentc_buf_push(&c->out, p, n);
}

/* Append one framed-protocol record. On a WebSocket connection it is sent as
 * one unmasked binary message carrying the same bytes (C3). */
static void serve_conn_push(ServeConn *c, const void *p, size_t n) {
    if (c->dead)
        return;
    if (c->ws_ready) {
        size_t hdr = n < 126 ? 2 : (n <= 0xFFFF ? 4 : 10);
        if (!serve_out_room(c, hdr + n))
            return;
        u8 h[10];
        size_t hn = 0;
        h[hn++] = 0x82; /* FIN | binary */
        if (n < 126) {
            h[hn++] = (u8)n;
        } else if (n <= 0xFFFF) {
            h[hn++] = 126;
            h[hn++] = (u8)(n >> 8);
            h[hn++] = (u8)n;
        } else {
            h[hn++] = 127;
            for (int i = 7; i >= 0; i--)
                h[hn++] = (u8)((u64)n >> (8 * i));
        }
        agentc_buf_push(&c->out, h, hn);
        agentc_buf_push(&c->out, p, n);
    } else {
        if (!serve_out_room(c, n))
            return;
        agentc_buf_push(&c->out, p, n);
    }
}

static int serve_conn_send(ServeConn *c, const void *p, size_t n) {
    if (!c->tls)
        return agentc_net_send(c->fd, p, n);
    int w = agentc_tls_write(c->tls, p, n);
    if (w == -11)
        c->tls_want = (short)agentc_tls_want(c->tls);
    return w;
}

static int serve_conn_recv(ServeConn *c, void *p, size_t n) {
    if (!c->tls)
        return agentc_net_recv(c->fd, p, n);
    int r = agentc_tls_read(c->tls, p, n);
    if (r == -11)
        c->tls_want = (short)agentc_tls_want(c->tls);
    return r;
}

static void serve_conn_flush(ServeConn *c) {
    while (!c->dead && c->out.len) {
        int w = serve_conn_send(c, c->out.p, c->out.len);
        if (w < 0)
            return; /* keep the rest for POLLOUT */
        if (w == 0)
            return;
        size_t left = c->out.len - (size_t)w;
        agentc_memmove(c->out.p, c->out.p + w, left);
        c->out.len = left;
        if (c->out.p)
            c->out.p[left] = 0;
    }
}

static void serve_filter_clear(ServeConn *c) {
    for (size_t i = 0; i < c->nev_filter; i++)
        agentc_free(c->ev_filter[i]);
    c->nev_filter = 0;
}

/* True if the connection's attachment wants an event of `ev_type` (NULL = any). */
static bool serve_conn_wants(const ServeConn *c, const char *ev_type) {
    if (!c->attached)
        return false;
    if (c->nev_filter == 0 || !ev_type)
        return true;
    for (size_t i = 0; i < c->nev_filter; i++)
        if (c->ev_filter[i] && agentc_streq(c->ev_filter[i], ev_type))
            return true;
    return false;
}

static void serve_broadcast(Serve *s, const ServeSession *ss, const void *p, size_t n,
                            const char *ev_type) {
    for (size_t i = 0; i < s->nconns; i++) {
        if (s->conns[i].dead || !s->conns[i].hello_ok)
            continue;
        if (s->conns[i].session != ss->index)
            continue;
        if (!serve_conn_wants(&s->conns[i], ev_type))
            continue;
        serve_conn_push(&s->conns[i], p, n);
        serve_conn_flush(&s->conns[i]);
    }
}

/* agentc_mode_event() writes here while an event is being framed. */
static void serve_capture(void *ud, const void *p, size_t n) {
    AgcBuf *b = ud;
    agentc_buf_push(b, p, n);
}

static void serve_emit(Serve *s, ServeConn *to, ServeSession *session, const char *line, size_t n);

static void serve_event(void *ud, int ev, const void *data);

/* ------------------------------------------------------- JSON <-> CBOR */

/* Emit one JSON writer value as CBOR, via the bridge (F64 => null: the
 * dispatcher never emits a float in v1). */
static void serve_wvalue_from_json(AgcCborW *w, const AgcJson *v) {
    if (!v) {
        agentc_cborw_null(w);
        return;
    }
    AgcBuf jb = {0};
    AgcJsonW jw;
    agentc_jsonw_init(&jw, &jb);
    agentc_json_emit(&jw, v);
    AgcCborArena *ca = agentc_cbor_arena_new(0);
    int err = 0;
    AgcCbor *cv = ca ? agentc_cbor_from_json(ca, (const char *)jb.p, jb.len, &err) : NULL;
    if (cv)
        agentc_cborw_value(w, cv);
    else
        agentc_cborw_null(w);
    agentc_cbor_arena_free(ca);
    agentc_buf_free(&jb);
}

/* Emit one CBOR JSON value into a JSON writer (params splicing, id echo). */
static void serve_wjson_from_cbor(AgcJsonW *w, const AgcCbor *v) {
    AgcBuf t = {0};
    if (v && agentc_cbor_to_json(v, &t) == 0)
        agentc_jsonw_raw(w, (const char *)t.p, t.len);
    else
        agentc_jsonw_null(w);
    agentc_buf_free(&t);
}

static void serve_json_key_cbor(AgcJsonW *w, const AgcCbor *k) {
    size_t kn = 0;
    const u8 *kp = agentc_cbor_text(k, &kn);
    AgcBuf kb = {0};
    if (kp)
        agentc_buf_push(&kb, kp, kn);
    agentc_buf_byte(&kb, 0);
    agentc_jsonw_key(w, kb.p ? (const char *)kb.p : "");
    agentc_buf_free(&kb);
}

/* Build the dispatcher command line for one request envelope. */
static void serve_build_command(const AgcProtoMsg *m, AgcBuf *out) {
    AgcJsonW w;
    agentc_jsonw_init(&w, out);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "command");
    agentc_jsonw_str(&w, m->method, m->method_len);
    agentc_jsonw_key(&w, "id");
    serve_wjson_from_cbor(&w, m->id);
    if (m->params && agentc_cbor_type(m->params) == AGENTC_CBOR_MAP) {
        for (size_t i = 0; i < agentc_cbor_len(m->params); i++) {
            AgcCbor *k = agentc_cbor_key(m->params, i);
            if (agentc_cbor_type(k) != AGENTC_CBOR_TEXT)
                continue;
            serve_json_key_cbor(&w, k);
            serve_wjson_from_cbor(&w, agentc_cbor_val(m->params, i));
        }
    } else if (m->params) {
        agentc_jsonw_key(&w, "params");
        serve_wjson_from_cbor(&w, m->params);
    }
    agentc_jsonw_end(&w);
}

/* True when a prompt/prompt_template request would be accepted at dispatch
 * time; an invalid queued record is stored un-acked so its pre-acceptance error
 * is the single response (A-D3, same rule as --mode rpc). */
static bool serve_prompt_valid(const AgcProtoMsg *m) {
    if (agentc_str_eq(m->method, m->method_len, "prompt", 6)) {
        return agentc_cbor_get(m->params, "text") != NULL ||
               agentc_cbor_get(m->params, "message") != NULL ||
               agentc_cbor_get(m->params, "prompt") != NULL;
    }
    AgcCbor *name = agentc_cbor_get(m->params, "name");
    size_t n = 0;
    const u8 *p = name ? agentc_cbor_text(name, &n) : NULL;
    return p != NULL && n > 0;
}

/* Build and queue one framed CBOR record from a dispatcher JSON line. */
static void serve_emit(Serve *s, ServeConn *to, ServeSession *session, const char *line, size_t n) {
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *o = ja ? agentc_json_parse_in(ja, line, n) : NULL;
    const char *type = agentc_json_get_str(o, "type");
    AgcBuf frame = {0};
    AgcCborW w;
    if (agentc_cborw_framed_init(&w, &frame) != 0) {
        agentc_json_arena_free(ja);
        return;
    }
    if (agentc_streq(type, "response")) {
        agentc_cborw_map(&w, 4);
        agentc_cborw_text(&w, "kind", 4);
        agentc_cborw_text(&w, "response", 8);
        agentc_cborw_text(&w, "id", 2);
        serve_wvalue_from_json(&w, agentc_json_get(o, "id"));
        bool ok = agentc_json_get_bool(o, "success", false);
        agentc_cborw_text(&w, "ok", 2);
        agentc_cborw_bool(&w, ok);
        if (ok) {
            agentc_cborw_text(&w, "result", 6);
            AgcJson *d = agentc_json_get(o, "data");
            if (d)
                serve_wvalue_from_json(&w, d);
            else
                agentc_cborw_null(&w);
        } else {
            agentc_cborw_text(&w, "error", 5);
            agentc_cborw_map(&w, 2);
            agentc_cborw_text(&w, "code", 4);
            const char *code = agentc_json_get_str(o, "code");
            agentc_cborw_text(&w, code ? code : "error", agentc_strlen(code ? code : "error"));
            agentc_cborw_text(&w, "message", 7);
            const char *msg = agentc_json_get_str(o, "error");
            agentc_cborw_text(&w, msg ? msg : "", agentc_strlen(msg ? msg : ""));
        }
    } else {
        bool tagged = session && session->id[0];
        agentc_cborw_map(&w, tagged ? 4 : 3);
        agentc_cborw_text(&w, "kind", 4);
        agentc_cborw_text(&w, "event", 5);
        agentc_cborw_text(&w, "seq", 3);
        agentc_cborw_uint(&w, ++s->seq);
        if (tagged) {
            agentc_cborw_text(&w, "session_id", 10);
            agentc_cborw_text(&w, session->id, agentc_strlen(session->id));
        }
        agentc_cborw_text(&w, "event", 5);
        serve_wvalue_from_json(&w, o);
    }
    if (agentc_cborw_finish(&w) == 0) {
        if (to && !to->dead) {
            serve_conn_push(to, frame.p, frame.len);
            serve_conn_flush(to);
        } else if (session) {
            serve_broadcast(s, session, frame.p, frame.len, type);
        }
    }
    agentc_buf_free(&frame);
    agentc_json_arena_free(ja);
}

static void serve_event(void *ud, int ev, const void *data) {
    ServeSession *ss = ud;
    Serve *s = ss->s;
    ss->capture.len = 0;
    agentc_mode_event(ss, ev, data);
    if (ss->capture.len)
        serve_emit(s, NULL, ss, (const char *)ss->capture.p, ss->capture.len);
}

/* The dispatcher sink: a response goes to the requesting connection, anything
 * else (agent_settled, UI) is tagged with the active session and broadcast to
 * that session's attachments. */
static void serve_sink(void *ud, const char *line, size_t n) {
    Serve *s = ud;
    serve_emit(s, s->current, s->current_session, line, n);
}

/* ---------------------------------------------------- request routing */

/* Rebind the shared process-global services to `ss` before dispatching one of
 * its requests or advancing its run slice. Multiple sessions run concurrently
 * (interleaved, single-threaded), so exactly one session's globals are bound at
 * a time; the wire poll hook and agent pump stay the server's own (they are
 * session-agnostic). */
static void serve_activate(ServeSession *ss) {
    if (!ss || !ss->used)
        return;
    ss->rpc.base.mcfg = &ss->rpc.cfg;
    agentc_mode_activate(&ss->rpc.base);
}

static void serve_route_respond(Serve *s, ServeConn *c, const AgcJson *req, const char *cmd,
                                bool ok, const char *data, size_t data_len, const char *error) {
    ServeSession *ss = &s->sessions[c->session];
    ServeConn *saved = s->current;
    ServeSession *saved_sess = s->current_session;
    s->current = c;
    s->current_session = ss;
    agentc_rpc_respond(&ss->rpc, req, cmd, ok, data, data_len, error);
    s->current = saved;
    s->current_session = saved_sess;
}

static ServeConn *serve_find_conn(Serve *s, u64 id) {
    for (size_t i = 0; i < s->nconns; i++)
        if (s->conns[i].id == id)
            return &s->conns[i];
    return NULL;
}

/* Dispatch every queued request that is ready. An action is only dispatched
 * when its session has no run in flight (that session's queued commands wait for
 * its own settle, so one session never blocks another); the vector is left
 * intact when the head action is deferred. A re-entrant input service during a
 * dispatch/run slice is a no-op (s->busy), so the globals cannot be rebound
 * underneath a step. */
static void serve_drain_actions(Serve *s) {
    if (s->busy)
        return;
    while (s->actions_head < s->actions.len) {
        ServeAction *a = &((ServeAction *)s->actions.p)[s->actions_head];
        u64 conn_id = a->conn_id;
        char *line = a->line;
        bool acked = a->acked;
        ServeConn *c = serve_find_conn(s, conn_id);
        if (!c || c->dead || !c->hello_ok) {
            /* The requester is gone: drop its queued record and move on. */
            s->actions_head++;
            agentc_free(line);
            continue;
        }
        ServeSession *ss = &s->sessions[c->session];
        if (ss->rpc.running)
            return; /* preserve per-session arrival order */
        /* Detach the owned fields before dispatching: the run's input service can
         * push more actions and realloc the vector out from under `a`. */
        a->line = NULL;
        s->actions_head++;
        ServeConn *saved = s->current;
        ServeSession *saved_sess = s->current_session;
        serve_activate(ss);
        s->current = c;
        s->current_session = ss;
        s->busy = true;
        (void)agentc_rpc_dispatch(&ss->rpc, line, agentc_strlen(line), acked);
        s->busy = false;
        /* A prompt starts a resumable run; remember which connection owns it so
         * dialogs and the settle route to the requester even after it detaches. */
        if (ss->rpc.running)
            ss->run_conn_id = conn_id;
        s->current = saved;
        s->current_session = saved_sess;
        agentc_free(line);
    }
    agentc_vec_free(&s->actions);
    s->actions_head = 0;
}

/* Advance every session's in-flight run by one bounded model-step slice. Each
 * session is rebound to the process globals for its own slice (model/thinking
 * sinks, ext context/entry/veto, trust/audit, queue owner, event callback); a
 * settle frees the slice. A run whose owner connection is gone still advances,
 * with the outcome broadcast to the session's surviving attachments. */
static void serve_advance_runs(Serve *s) {
    for (size_t i = 0; i < s->nsessions; i++) {
        ServeSession *ss = &s->sessions[i];
        if (!ss->used || !ss->rpc.running)
            continue;
        ServeConn *owner = serve_find_conn(s, ss->run_conn_id);
        if (owner && (owner->dead || !owner->hello_ok))
            owner = NULL;
        ServeConn *saved = s->current;
        ServeSession *saved_sess = s->current_session;
        serve_activate(ss);
        s->current = owner;
        s->current_session = ss;
        s->busy = true;
        bool more = agentc_rpc_run_step(&ss->rpc);
        s->busy = false;
        s->current = saved;
        s->current_session = saved_sess;
        if (!more) {
            ss->run_conn_id = 0;
        } else if (owner) {
            /* F3: a progressing run keeps its owner connection alive across
             * slices, so a short idle deadline cannot reap it mid-run. */
            serve_conn_touch(owner, s);
        }
    }
}

/* ------------------------------------------------------ UI dialogs (C2) */

/* Answer text for a scalar CBOR value (bool -> true/false, text -> text,
 * uint -> decimal). Bounded and fail-closed for other types. */
static void serve_dialog_text(const AgcCbor *v, char *out, size_t cap) {
    if (!out || cap == 0)
        return;
    out[0] = 0;
    if (!v)
        return;
    switch (agentc_cbor_type(v)) {
    case AGENTC_CBOR_BOOL:
        agentc_snprintf(out, cap, "%s", agentc_cbor_bool(v) ? "true" : "false");
        break;
    case AGENTC_CBOR_TEXT: {
        size_t n = 0;
        const u8 *p = agentc_cbor_text(v, &n);
        if (n >= cap)
            n = cap - 1;
        if (p)
            agentc_memcpy(out, p, n);
        out[n] = 0;
        break;
    }
    case AGENTC_CBOR_UINT:
        agentc_snprintf(out, cap, "%llu", (unsigned long long)agentc_cbor_uint(v));
        break;
    default:
        break;
    }
}

static ServeDialog *serve_dialog_find(Serve *s, u64 conn_id, u64 id) {
    for (size_t i = 0; i < SERVE_DIALOG_MAX; i++) {
        ServeDialog *d = &s->dialogs[i];
        if (d->used && d->conn_id == conn_id && d->id == id)
            return d;
    }
    return NULL;
}

static ServeDialog *serve_dialog_alloc(Serve *s, u64 conn_id) {
    /* One parked dialog per connection: the connection is the reply channel. */
    for (size_t i = 0; i < SERVE_DIALOG_MAX; i++)
        if (s->dialogs[i].used && s->dialogs[i].conn_id == conn_id)
            return NULL;
    for (size_t i = 0; i < SERVE_DIALOG_MAX; i++) {
        if (s->dialogs[i].used)
            continue;
        ServeDialog *d = &s->dialogs[i];
        agentc_memset(d, 0, sizeof *d);
        d->used = true;
        d->conn_id = conn_id;
        d->id = ++s->dialog_seq;
        d->deadline_ns = os_now_ns(OS_CLOCK_MONOTONIC) + (i64)SERVE_DIALOG_TIMEOUT_MS * 1000000;
        return d;
    }
    return NULL;
}

static void serve_dialog_dispose(ServeDialog *d) {
    if (!d || !d->used)
        return;
    agentc_free(d->req_id);
    agentc_free(d->value);
    agentc_memset(d, 0, sizeof *d);
}

/* Encode a `response` envelope to `c`. */
static void serve_send_response(Serve *s, ServeConn *c, const AgcCbor *id, bool ok,
                                const AgcCbor *result, const char *code, const char *message) {
    (void)s;
    if (!c || c->dead)
        return;
    AgcBuf frame = {0};
    AgcCborW w;
    agentc_cborw_framed_init(&w, &frame);
    agentc_proto_enc_response(&w, id, ok, result, code, message);
    agentc_cborw_finish(&w);
    serve_conn_push(c, frame.p, frame.len);
    serve_conn_flush(c);
    agentc_buf_free(&frame);
}

/* Send a `ui_request` for a parked dialog. `params` is borrowed and encoded
 * immediately. Returns false when the requester is already gone. */
static bool serve_dialog_send(Serve *s, ServeConn *c, ServeDialog *d, const char *method,
                              const AgcCbor *params) {
    (void)s;
    if (!c || c->dead || !c->hello_ok)
        return false;
    AgcBuf frame = {0};
    AgcCborW w;
    agentc_cborw_framed_init(&w, &frame);
    agentc_proto_enc_ui_request(&w, d->id, method, params);
    agentc_cborw_finish(&w);
    serve_conn_push(c, frame.p, frame.len);
    serve_conn_flush(c);
    agentc_buf_free(&frame);
    return true;
}

/* Store a live answer value as both owned JSON (for a client reply) and its
 * bounded text form (for a pipeline waiter). */
static void serve_dialog_store(ServeDialog *d, const AgcCbor *result) {
    agentc_free(d->value);
    d->value = NULL;
    d->value_len = 0;
    d->text[0] = 0;
    if (!result)
        return;
    AgcBuf jb = {0};
    if (agentc_cbor_to_json(result, &jb) == 0 && jb.p) {
        d->value = agentc_strdup_len((const char *)jb.p, jb.len);
        d->value_len = jb.len;
    }
    agentc_buf_free(&jb);
    serve_dialog_text(result, d->text, sizeof d->text);
}

/* Complete a `ui_prompt` request with its parked answer and free the slot. */
static void serve_dialog_complete(Serve *s, ServeDialog *d) {
    ServeConn *c = serve_find_conn(s, d->conn_id);
    /* agentc_cbor_from_json() clears its arena on entry, so the request id and
     * the answer get separate arenas. */
    AgcCborArena *ca_id = agentc_cbor_arena_new(0);
    AgcCborArena *ca_res = agentc_cbor_arena_new(0);
    int err = 0;
    AgcCbor *idv = (d->req_id && ca_id)
                       ? agentc_cbor_from_json(ca_id, d->req_id, agentc_strlen(d->req_id), &err)
                       : NULL;
    if (d->ok && d->value && ca_res) {
        AgcCbor *val = agentc_cbor_from_json(ca_res, d->value, d->value_len, &err);
        AgcCbor *res = agentc_cbor_new_map(ca_res, 1);
        agentc_cbor_map_set(res, 0, agentc_cbor_new_text(ca_res, "value", 5), val);
        serve_send_response(s, c, idv, true, res, NULL, NULL);
    } else {
        serve_send_response(s, c, idv, false, NULL, "denied", "dialog denied");
    }
    agentc_cbor_arena_free(ca_id);
    agentc_cbor_arena_free(ca_res);
    serve_dialog_dispose(d);
}

/* Expire every unanswered dialog past its deadline (deny, fail-closed). A
 * client request is answered `denied`; a pipeline waiter sees `answered` and
 * unblocks. */
static void serve_dialog_expire(Serve *s) {
    i64 now = os_now_ns(OS_CLOCK_MONOTONIC);
    for (size_t i = 0; i < SERVE_DIALOG_MAX; i++) {
        ServeDialog *d = &s->dialogs[i];
        if (!d->used || d->answered || now < d->deadline_ns)
            continue;
        d->answered = true;
        d->ok = false;
        if (d->client_req)
            serve_dialog_complete(s, d);
    }
}

/* A `ui_prompt` request parks a dialog and returns without blocking; the reply
 * arrives as a `ui_response` (or the deadline denies). */
static void serve_ui_prompt_request(Serve *s, ServeConn *c, const AgcProtoMsg *m) {
    AgcCbor *methodv = agentc_cbor_get(m->params, "method");
    size_t mn = 0;
    const u8 *mp = methodv ? agentc_cbor_text(methodv, &mn) : NULL;
    AgcCbor *promptv = agentc_cbor_get(m->params, "prompt");
    size_t pn = 0;
    const u8 *pp = promptv ? agentc_cbor_text(promptv, &pn) : NULL;
    AgcCbor *options = agentc_cbor_get(m->params, "options");
    AgcCbor *timeoutv = agentc_cbor_get(m->params, "timeout_ms");
    bool bad = !agentc_proto_ui_method_valid((const char *)mp, mn) || pp == NULL || pn == 0 ||
               pn > AGENTC_PROTO_MAX_UI_PROMPT;
    if (!bad && timeoutv && agentc_cbor_type(timeoutv) != AGENTC_CBOR_UINT)
        bad = true;
    if (!bad && options) {
        if (agentc_cbor_type(options) != AGENTC_CBOR_ARRAY || agentc_cbor_len(options) < 1 ||
            agentc_cbor_len(options) > AGENTC_PROTO_MAX_UI_OPTIONS)
            bad = true;
        for (size_t i = 0; !bad && i < agentc_cbor_len(options); i++) {
            size_t on = 0;
            AgcCbor *o = agentc_cbor_at(options, i);
            if (!o || agentc_cbor_type(o) != AGENTC_CBOR_TEXT || agentc_cbor_text(o, &on) == NULL ||
                on == 0)
                bad = true;
        }
    }
    if (agentc_str_eq((const char *)mp, mn, "select", 6) && (options == NULL || bad))
        bad = true; /* select needs a bounded choice list */
    if (bad) {
        serve_send_response(s, c, m->id, false, NULL, "invalid_response",
                            "ui_prompt: invalid dialog");
        return;
    }
    ServeDialog *d = serve_dialog_alloc(s, c->id);
    if (!d) {
        serve_send_response(s, c, m->id, false, NULL, "busy", "ui_prompt: too many dialogs");
        return;
    }
    d->client_req = true;
    if (timeoutv) {
        u64 ms = agentc_cbor_uint(timeoutv);
        if (ms < 1)
            ms = 1;
        if (ms > 600000)
            ms = 600000;
        d->deadline_ns = os_now_ns(OS_CLOCK_MONOTONIC) + (i64)ms * 1000000;
    }
    AgcBuf idj = {0};
    if (agentc_cbor_to_json(m->id, &idj) == 0 && idj.p)
        d->req_id = agentc_strdup_len((const char *)idj.p, idj.len);
    agentc_buf_free(&idj);
    if (!serve_dialog_send(s, c, d, (const char *)mp, m->params)) {
        serve_dialog_dispose(d);
        return;
    }
}

/* A `ui_response` completes a matching parked dialog. With no match (or a
 * reply from another connection) it is validated and dropped. */
static void serve_ui_response(Serve *s, ServeConn *c, const AgcProtoMsg *m) {
    ServeDialog *d = serve_dialog_find(s, c->id, agentc_cbor_uint(m->id));
    if (!d || d->answered)
        return;
    d->answered = true;
    d->ok = m->ok;
    if (m->ok && m->result)
        serve_dialog_store(d, m->result);
    if (d->client_req)
        serve_dialog_complete(s, d);
}

/* The installed UI prompt sink: raise a dialog on the connection owning the
 * run and pump (bounded) until the reply arrives. No blocking primitive is
 * used; serve_pump services the wire on every slice. */
static int serve_ui_prompt(void *ud, const char *method, const char *prompt,
                           const char *const *options, size_t noptions, const char *deflt,
                           char *out, size_t out_cap) {
    Serve *s = ud;
    if (!s || !s->current || s->current->dead || !s->current->hello_ok)
        return -38;
    if (!agentc_proto_ui_method_valid(method, method ? agentc_strlen(method) : 0))
        return -22;
    if (!prompt || agentc_strlen(prompt) > AGENTC_PROTO_MAX_UI_PROMPT)
        return -22;
    if (noptions > AGENTC_PROTO_MAX_UI_OPTIONS)
        return -22;
    ServeDialog *d = serve_dialog_alloc(s, s->current->id);
    if (!d)
        return -28;
    AgcCborArena *ca = agentc_cbor_arena_new(0);
    AgcCborArena *ca_def = deflt ? agentc_cbor_arena_new(0) : NULL;
    int err = 0;
    AgcCbor *dv = NULL;
    if (deflt && ca_def) {
        dv = agentc_cbor_from_json(ca_def, deflt, agentc_strlen(deflt), &err);
        if (!dv)
            dv = agentc_cbor_new_text(ca_def, deflt, agentc_strlen(deflt));
    }
    size_t nk = 1 + (noptions ? 1 : 0) + (deflt ? 1 : 0);
    AgcCbor *pm = agentc_cbor_new_map(ca, nk);
    size_t k = 0;
    agentc_cbor_map_set(pm, k++, agentc_cbor_new_text(ca, "prompt", 6),
                        agentc_cbor_new_text(ca, prompt, agentc_strlen(prompt)));
    if (noptions) {
        AgcCbor *arr = agentc_cbor_new_array(ca, noptions);
        for (size_t i = 0; i < noptions; i++)
            agentc_cbor_array_set(arr, i,
                                  agentc_cbor_new_text(ca, options[i] ? options[i] : "",
                                                       options[i] ? agentc_strlen(options[i]) : 0));
        agentc_cbor_map_set(pm, k++, agentc_cbor_new_text(ca, "options", 7), arr);
    }
    if (deflt)
        agentc_cbor_map_set(pm, k++, agentc_cbor_new_text(ca, "default", 7), dv);
    serve_dialog_send(s, s->current, d, method, pm);
    agentc_cbor_arena_free(ca);
    agentc_cbor_arena_free(ca_def);

    int rc = -125; /* ECANCELED: fail-closed deny/timeout */
    while (!d->answered && !s->shutdown && !g_serve_stop) {
        ServeConn *owner = serve_find_conn(s, d->conn_id);
        if (!owner || owner->dead)
            break;
        if (os_now_ns(OS_CLOCK_MONOTONIC) > d->deadline_ns) {
            d->answered = true;
            d->ok = false;
            break;
        }
        agentc_pump(10);
    }
    if (d->answered && d->ok) {
        size_t n = agentc_strlen(d->text);
        if (out && out_cap) {
            if (n >= out_cap)
                n = out_cap - 1;
            agentc_memcpy(out, d->text, n);
            out[n] = 0;
        }
        rc = 0;
    }
    serve_dialog_dispose(d);
    return rc;
}

/* Tool-approval front end (P5 C2): a served run raises a confirm dialog. A
 * missing/denied answer is the fail-closed deny. */
static int serve_approval_ui(void *ud, const AgcApprovalRequest *req, AgcApprovalResult *out) {
    Serve *s = ud;
    if (!s || !s->current)
        return -38;
    char prompt[256];
    agentc_snprintf(prompt, sizeof prompt, "Allow %s? (%s)",
                    (req && req->tool) ? req->tool : "tool",
                    (req && req->trigger) ? req->trigger : "approval");
    bool allow = false;
    if (agentc_ui_prompt_confirm(prompt, &allow) != 0)
        return -125;
    out->id = req ? req->id : 0;
    out->decision = allow ? AGENTC_DEC_ALLOW : AGENTC_DEC_DENY;
    out->scope = AGENTC_SCOPE_ONCE;
    out->reason = agentc_strdup(allow ? "approved by client" : "denied by client");
    return 0;
}

/* ---------------------------------------------------------- handshake */

static void serve_send_hello_error(Serve *s, ServeConn *c, const char *code, const char *msg) {
    (void)s;
    AgcBuf frame = {0};
    AgcCborW w;
    agentc_cborw_framed_init(&w, &frame);
    agentc_proto_enc_hello_error(&w, code, msg);
    agentc_cborw_finish(&w);
    serve_conn_push(c, frame.p, frame.len);
    serve_conn_flush(c);
    agentc_buf_free(&frame);
    c->dead = true;
}

/* S7: length-checked constant-time token compare. The server token length is
 * not secret, but comparing byte-by-byte with an early exit leaks a match
 * prefix through timing; fold every byte and the length into one accumulator. */
static bool serve_token_eq(const char *a, const char *b) {
    if (!a || !b)
        return false;
    size_t la = agentc_strlen(a), lb = agentc_strlen(b);
    volatile unsigned diff = (unsigned)(la ^ lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

/* S5 host/origin policy helpers. `has_port` splits off a trailing `:port`
 * (ignored) from either a hostname or a bracketed IPv6 literal. */
static void serve_host_span(const char *s, const char **host, size_t *hlen) {
    const char *h = s;
    size_t n = agentc_strlen(s);
    if (n && h[0] == '[') {
        size_t i = 1;
        while (i < n && h[i] != ']')
            i++;
        *host = h + 1;
        *hlen = i - 1;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (h[i] == ':') {
            n = i;
            break;
        }
    }
    *host = h;
    *hlen = n;
}

static bool serve_host_is_loopback(const char *s) {
    const char *h = NULL;
    size_t n = 0;
    serve_host_span(s, &h, &n);
    if (n == 0)
        return false;
    if (n == 9 && agentc_str_ieq(h, n, "localhost", 9))
        return true;
    if (n == 3 && agentc_str_eq(h, n, "::1", 3))
        return true;
    if (n > 5 && agentc_str_eq(h, 4, "127.", 4)) {
        /* 127.0.0.0/8. Require three dot-separated numeric labels so a host
         * like 127.0.0.1.evil cannot pass. */
        int dots = 0, digits = 0;
        for (size_t i = 4; i < n; i++) {
            char c = h[i];
            if (c == '.') {
                if (digits == 0)
                    return false;
                dots++;
                digits = 0;
            } else if (c >= '0' && c <= '9') {
                if (++digits > 3)
                    return false;
            } else {
                return false;
            }
        }
        return dots == 2 && digits > 0;
    }
    return false;
}

static bool serve_origin_is_loopback(const char *o) {
    const char *p = o;
    if (agentc_str_starts(p, agentc_strlen(p), "http://"))
        p += 7;
    else if (agentc_str_starts(p, agentc_strlen(p), "https://"))
        p += 8;
    else
        return false;
    size_t n = agentc_strlen(p);
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '/')
            n = i;
    }
    char host[128];
    if (n == 0 || n >= sizeof host)
        return false;
    agentc_memcpy(host, p, n);
    host[n] = 0;
    return serve_host_is_loopback(host);
}

/* S5: a loopback-bound WS server is reachable from any page the user visits;
 * require a loopback Host and either no Origin or a loopback Origin. A remote
 * bind is already token-gated, so its Host/Origin is left to that gate. */
static bool serve_ws_allow(void *ud, const AgcWsRequest *req) {
    Serve *s = ud;
    if (!req || !req->has_host)
        return false;
    if (!s || !s->ws_loopback)
        return true;
    if (!serve_host_is_loopback(req->host))
        return false;
    if (req->has_origin && !serve_origin_is_loopback(req->origin))
        return false;
    return true;
}

static void serve_handshake(Serve *s, ServeConn *c, const AgcProtoMsg *m) {
    if (m->version != AGENTC_PROTO_VERSION) {
        serve_send_hello_error(s, c, "version_mismatch", "protocol version must be 1");
        return;
    }
    if (s->token && !(m->token && serve_token_eq(m->token, s->token))) {
        serve_send_hello_error(s, c, "unauthorized", "bad token");
        return;
    }
    static const char *features[] = {"cancel", "ui", "attach", "multi_session"};
    AgcBuf frame = {0};
    AgcCborW w;
    agentc_cborw_framed_init(&w, &frame);
    const char *sess = s->sessions[0].used ? s->sessions[0].id : NULL;
    agentc_proto_enc_hello_ack(&w, AGENTC_PROTO_VERSION, s->server_id, sess, features, 4);
    agentc_cborw_finish(&w);
    serve_conn_push(c, frame.p, frame.len);
    serve_conn_flush(c);
    agentc_buf_free(&frame);
    c->hello_ok = true;
    c->attached = true; /* default attachment: the primary session, all events */
    c->session = 0;
    /* S8: a completed handshake converts the absolute hello deadline into the
     * sliding idle deadline so a long-lived idle attachment is not reaped. */
    c->deadline_ns = os_now_ns(OS_CLOCK_MONOTONIC) + s->idle_timeout_ms * 1000000;
}

/* `attach` (C-D4/C3): (re)subscribe this connection to a session, optionally
 * narrowing the event broadcast to a list of event types. The session id is
 * looked up in the bounded registry (a foreign id is refused `not_found`);
 * without one the connection's current attachment is kept. */
static void serve_attach(Serve *s, ServeConn *c, const AgcProtoMsg *m) {
    if (m->session) {
        u32 found = SERVE_MAX_SESSIONS;
        for (size_t i = 0; i < SERVE_MAX_SESSIONS; i++)
            if (s->sessions[i].used && agentc_streq(m->session, s->sessions[i].id)) {
                found = (u32)i;
                break;
            }
        if (found == SERVE_MAX_SESSIONS) {
            serve_send_response(s, c, m->id, false, NULL, "not_found", "no such session");
            return;
        }
        c->session = found;
    }
    const char *sess = s->sessions[c->session].used ? s->sessions[c->session].id : NULL;
    serve_filter_clear(c);
    c->attached = true;
    for (size_t i = 0; i < m->nevents && c->nev_filter < AGENTC_PROTO_MAX_ATTACH_EVENTS; i++)
        c->ev_filter[c->nev_filter++] = agentc_strdup(m->events[i]);
    AgcCborArena *ca = agentc_cbor_arena_new(0);
    AgcCbor *res = NULL;
    if (ca) {
        res = agentc_cbor_new_map(ca, 2);
        agentc_cbor_map_set(res, 0, agentc_cbor_new_text(ca, "session_id", 10),
                            sess ? agentc_cbor_new_text(ca, sess, agentc_strlen(sess))
                                 : agentc_cbor_new_null(ca));
        AgcCbor *arr = agentc_cbor_new_array(ca, m->nevents);
        for (size_t i = 0; i < m->nevents; i++)
            agentc_cbor_array_set(
                arr, i, agentc_cbor_new_text(ca, m->events[i], agentc_strlen(m->events[i])));
        agentc_cbor_map_set(res, 1, agentc_cbor_new_text(ca, "events", 6), arr);
    }
    serve_send_response(s, c, m->id, true, res, NULL, NULL);
    agentc_cbor_arena_free(ca);
}

/* `detach` (C-D4): stop receiving the event broadcast; the connection stays
 * open and may `attach` again. */
static void serve_detach(Serve *s, ServeConn *c, const AgcProtoMsg *m) {
    (void)s;
    serve_filter_clear(c);
    c->attached = false;
    AgcCborArena *ca = agentc_cbor_arena_new(0);
    AgcCbor *res = ca ? agentc_cbor_new_map(ca, 0) : NULL;
    serve_send_response(s, c, m->id, true, res, NULL, NULL);
    agentc_cbor_arena_free(ca);
}

/* One decoded frame. -> 0 continue, negative errno = terminal for the connection. */
static int serve_frame(Serve *s, ServeConn *c, const AgcFrame *f) {
    int err = 0;
    /* The decode arena is reused across frames (single-threaded). A frame
     * callback never dispatches a run, so the arena is never live across a
     * re-entrant feed. */
    static AgcCborArena *arena; /* process-global: serve is one engine */
    if (!arena)
        arena = agentc_cbor_arena_new(0);
    AgcCbor *v = agentc_cbor_decode(arena, f->payload, f->len, NULL, &err);
    if (!v)
        return err ? err : -22;
    AgcProtoMsg m;
    if (agentc_proto_decode(v, &m) != 0)
        return -71;
    if (m.kind == AGENTC_PROTO_HELLO) {
        serve_handshake(s, c, &m);
        return 0;
    }
    if (!c->hello_ok) {
        serve_send_hello_error(s, c, "protocol", "hello required");
        return 0;
    }
    if (m.kind == AGENTC_PROTO_REQUEST && agentc_str_eq(m.method, m.method_len, "ui_prompt", 9)) {
        /* A dialog parks and returns; the reply arrives later as a
         * `ui_response` (or the deadline denies). Never blocks the frame. */
        serve_ui_prompt_request(s, c, &m);
        return 0;
    }
    if (m.kind == AGENTC_PROTO_UI_RESPONSE) {
        /* With no matching pending dialog this is validated and dropped. */
        serve_ui_response(s, c, &m);
        return 0;
    }
    if (m.kind == AGENTC_PROTO_ATTACH) {
        serve_attach(s, c, &m);
        return 0;
    }
    if (m.kind == AGENTC_PROTO_DETACH) {
        serve_detach(s, c, &m);
        return 0;
    }
    if (m.kind == AGENTC_PROTO_REQUEST) {
        AgcBuf cmd = {0};
        serve_build_command(&m, &cmd);
        bool prompt = agentc_str_eq(m.method, m.method_len, "prompt", 6) ||
                      agentc_str_eq(m.method, m.method_len, "prompt_template", 15);
        bool abort_m = agentc_str_eq(m.method, m.method_len, "abort", 5);
        ServeSession *rq = &s->sessions[c->session];
        if (rq->rpc.running && abort_m) {
            /* abort stays immediate while the requester's session has a run
             * (A-D3); it cancels only that run, never a sibling session's. */
            agentc_agent_abort(rq->rpc.base.agent);
            AgcJsonArena *ja = agentc_json_arena_new(0);
            AgcJson *req = agentc_json_parse_in(ja, (const char *)cmd.p, cmd.len);
            serve_route_respond(s, c, req, m.method, true, NULL, 0, NULL);
            agentc_json_arena_free(ja);
            agentc_buf_free(&cmd);
            return 0;
        }
        bool acked = false;
        if (rq->rpc.running && prompt && serve_prompt_valid(&m)) {
            AgcJsonArena *ja = agentc_json_arena_new(0);
            AgcJson *req = agentc_json_parse_in(ja, (const char *)cmd.p, cmd.len);
            serve_route_respond(s, c, req, m.method, true, RPC_QUEUED, sizeof RPC_QUEUED - 1, NULL);
            agentc_json_arena_free(ja);
            acked = true;
        } else if (s->actions.len - s->actions_head >= SERVE_PENDING_MAX) {
            /* Bounded queue (global): fail closed instead of growing without
             * bound. Never silently dropped. */
            AgcJsonArena *ja = agentc_json_arena_new(0);
            AgcJson *req = agentc_json_parse_in(ja, (const char *)cmd.p, cmd.len);
            serve_route_respond(s, c, req, m.method, false, NULL, 0, "busy: pending queue full");
            agentc_json_arena_free(ja);
            agentc_buf_free(&cmd);
            return 0;
        }
        ServeAction *a = agentc_vec_push(&s->actions, sizeof *a);
        a->conn_id = c->id;
        a->line = agentc_strdup_len((const char *)cmd.p, cmd.len);
        a->acked = acked;
        agentc_buf_free(&cmd);
        return 0;
    }
    if (m.kind == AGENTC_PROTO_CANCEL) {
        ServeSession *rq = &s->sessions[c->session];
        if (rq->rpc.running)
            agentc_agent_abort(rq->rpc.base.agent);
        return 0;
    }
    return -71; /* unknown kind: terminal for this connection */
}

typedef struct {
    Serve *s;
    ServeConn *c;
} ServeFeed;

static int serve_feed_cb(void *ud, const AgcFrame *f) {
    ServeFeed *fc = ud;
    return serve_frame(fc->s, fc->c, f);
}

typedef struct {
    Serve *s;
    ServeConn *c;
} ServeWsFeed;

/* One WebSocket message/control frame. A binary message carries one
 * framed-protocol record; it is fed through the frame decoder so the CBOR
 * payload path is shared with the raw transport. */
static int serve_ws_cb(void *ud, int opcode, const u8 *payload, size_t len) {
    ServeWsFeed *wf = ud;
    if (opcode == AGENTC_WS_BINARY) {
        ServeFeed fc = {wf->s, wf->c};
        return agentc_frame_decoder_feed(&wf->c->dec, payload, len, serve_feed_cb, &fc);
    }
    if (opcode == AGENTC_WS_PING) {
        AgcBuf fr = {0};
        if (agentc_ws_encode_pong(&fr, payload, len) == 0) {
            serve_conn_push_raw(wf->c, fr.p, fr.len);
            serve_conn_flush(wf->c);
        }
        agentc_buf_free(&fr);
        return 0;
    }
    if (opcode == AGENTC_WS_CLOSE) {
        AgcBuf fr = {0};
        if (agentc_ws_encode_close(&fr, 1000) == 0) {
            serve_conn_push_raw(wf->c, fr.p, fr.len);
            serve_conn_flush(wf->c);
        }
        agentc_buf_free(&fr);
        wf->c->dead = true;
        return 0;
    }
    return 0; /* PONG: nothing to do */
}

/* -------------------------------------------------------------- listener */

/* S6: create the socket directory tree and then require the leaf directory
 * (the one that will hold the socket) to be a real directory owned by this
 * process's real uid, not a symlink. This refuses an attacker-planted
 * /tmp/agentc-<uid> or a socket dir redirected through a symlink. Intermediate
 * components are created as before: a symlinked /tmp (macOS) must keep working,
 * and the untrusted case is the leaf itself. -> 0 | -errno. */
static int serve_check_dir(const char *path) {
    struct os_stat st;
    int r = os_lstat(path, &st);
    if (r != 0)
        return r;
    u32 ft = st.st_mode & 0170000u;
    if (ft == 0120000u)
        return -13; /* EACCES: a symlinked socket dir is refused */
    if (ft != 0040000u)
        return -20; /* ENOTDIR */
    if (st.st_uid != os_getuid())
        return -13; /* EACCES: not ours */
    if ((st.st_mode & 022u) != 0)
        return -13; /* EACCES: group/other-writable socket dir (N2) */
    return 0;
}

static int serve_mkdirs(const char *file) {
    char dir[600];
    size_t n = agentc_strlen(file);
    if (n == 0 || n >= sizeof dir)
        return -36; /* ENAMETOOLONG */
    agentc_memcpy(dir, file, n + 1);
    char *slash = NULL;
    for (size_t i = 0; i < n; i++)
        if (dir[i] == '/')
            slash = dir + i;
    /* F4: a bare filename has no parent directory to create or validate; the
     * listener binds it relative to the current working directory. Likewise a
     * leading-slash-only path's parent is the root, which already exists. */
    if (!slash || slash == dir)
        return 0;
    *slash = 0;
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            (void)os_mkdir(dir, 0700);
            *p = '/';
        }
    }
    int r = os_mkdir(dir, 0700);
    if (r != 0 && r != -17 /* EEXIST */)
        return r;
    return serve_check_dir(dir);
}

static int serve_default_path(char *out, size_t cap) {
    const char *xdg = os_getenv("XDG_RUNTIME_DIR");
    int w;
    if (xdg && xdg[0]) {
        w = agentc_snprintf(out, cap, "%s/abird/agentc/serve.sock", xdg);
    } else {
        w = agentc_snprintf(out, cap, "/tmp/agentc-%u/serve.sock", (unsigned)os_getuid());
    }
    return (w < 0 || (size_t)w >= cap) ? -36 : 0;
}

/* Read a bounded file (PEM certificate/key) into an owned buffer. On any
 * error the partially filled buffer is released (P8: the leak). */
static int serve_read_file(const char *path, AgcBuf *out) {
    int fd = os_open(path, OS_O_RDONLY, 0);
    if (fd < 0)
        return fd;
    u8 tmp[65536];
    int rc = 0;
    for (;;) {
        int n = os_read(fd, tmp, sizeof tmp);
        if (n < 0) {
            rc = n;
            break;
        }
        if (n == 0) {
            rc = out->len ? 0 : -22;
            break;
        }
        if (out->len + (size_t)n > 1024u * 1024u) {
            rc = -36;
            break;
        }
        agentc_buf_push(out, tmp, (size_t)n);
    }
    os_close(fd);
    if (rc != 0)
        agentc_buf_free(out);
    return rc;
}

static int serve_parse_tcp(const char *spec, u32 *ip_be, u16 *port) {
    const char *colon = NULL;
    for (const char *p = spec; *p; p++)
        if (*p == ':')
            colon = p;
    char host[128];
    size_t hn = colon ? (size_t)(colon - spec) : agentc_strlen(spec);
    if (hn == 0 || hn >= sizeof host)
        return -22;
    agentc_memcpy(host, spec, hn);
    host[hn] = 0;
    struct agentc_ip4 ip;
    if (!agentc_net_is_ip4(host, &ip))
        return -22;
    agentc_memcpy(ip_be, ip.b, 4);
    if (!colon) {
        *port = SERVE_DEFAULT_TCP_PORT;
        return 0;
    }
    bool ok = false;
    i64 p = agentc_parse_i64(colon + 1, agentc_strlen(colon + 1), &ok);
    if (!ok || p < 0 || p > 65535)
        return -22;
    *port = (u16)p;
    return 0;
}

static void serve_ready(Serve *s) {
    AgcBuf b = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "transport");
    agentc_jsonw_cstr(&w, s->transport);
    agentc_jsonw_key(&w, "path");
    agentc_jsonw_cstr(&w, s->path);
    agentc_jsonw_key(&w, "server_id");
    agentc_jsonw_cstr(&w, s->server_id);
    agentc_jsonw_key(&w, "pid");
    agentc_jsonw_i64(&w, os_getpid());
    agentc_jsonw_key(&w, "session_id");
    agentc_jsonw_cstr(&w, s->sessions[0].used ? s->sessions[0].id : "");
    agentc_jsonw_key(&w, "token_file");
    agentc_jsonw_null(&w);
    if (s->ws) {
        agentc_jsonw_key(&w, "ws");
        agentc_jsonw_bool(&w, true);
    }
    if (s->tls) {
        agentc_jsonw_key(&w, "tls");
        agentc_jsonw_bool(&w, true);
    }
    agentc_jsonw_end(&w);
    agentc_buf_byte(&b, '\n');
    (void)os_write(1, b.p, b.len);
    agentc_buf_free(&b);
}

/* ------------------------------------------------------------------ loop */

static void serve_close_conn(Serve *s, size_t i) {
    ServeConn *c = &s->conns[i];
    if (c->tls)
        agentc_tls_close(c->tls);
    agentc_net_close(c->fd);
    agentc_frame_decoder_free(&c->dec);
    agentc_ws_decoder_free(&c->ws_dec);
    agentc_buf_free(&c->ws_in);
    agentc_buf_free(&c->out);
    serve_filter_clear(c);
    c->dead = true;
}

/* Advance a pending server TLS handshake. -> 0 done/in progress, negative errno
 * fatal. The caller polls the direction reported by agentc_tls_want(). */
static int serve_tls_progress(ServeConn *c) {
    if (!c->tls || c->tls_ready)
        return 0;
    int r = agentc_tls_handshake(c->tls);
    if (r == 0) {
        c->tls_ready = true;
        c->tls_want = 0;
        return 0;
    }
    if (r == -11) {
        c->tls_want = (short)agentc_tls_want(c->tls);
        return 0;
    }
    return r;
}

static void serve_conn_touch(ServeConn *c, Serve *s) {
    /* Before hello the deadline is the absolute handshake deadline set at
     * accept; once helloed it becomes a sliding idle deadline. */
    if (c->hello_ok)
        c->deadline_ns = os_now_ns(OS_CLOCK_MONOTONIC) + s->idle_timeout_ms * 1000000;
}

/* F3: the connection that owns a session's in-flight run. `s->current` is only
 * set for the duration of one dispatch/slice, so the idle reaper must also
 * consult the stable `run_conn_id`; otherwise a silent client whose run is
 * progressing gets marked dead and its response is lost. */
static bool serve_conn_owns_run(const Serve *s, const ServeConn *c) {
    for (size_t i = 0; i < SERVE_MAX_SESSIONS; i++) {
        const ServeSession *ss = &s->sessions[i];
        if (ss->used && ss->rpc.running && ss->run_conn_id != 0 && ss->run_conn_id == c->id)
            return true;
    }
    return false;
}

/* S8: mark connections whose handshake or idle deadline elapsed. A silent peer
 * holding one of the bounded slots is reclaimed here (never the connection that
 * owns the in-flight response, nor one that owns a running session). */
static void serve_expire(Serve *s) {
    i64 now = os_now_ns(OS_CLOCK_MONOTONIC);
    for (size_t i = 0; i < s->nconns; i++) {
        if (s->conns[i].dead || s->conns[i].deadline_ns == 0)
            continue;
        if (s->current == &s->conns[i])
            continue;
        if (serve_conn_owns_run(s, &s->conns[i]))
            continue;
        if (now >= s->conns[i].deadline_ns)
            s->conns[i].dead = true;
    }
}

static void serve_reap(Serve *s) {
    /* Never reap the connection that owns the in-flight response: its pointer is
     * live on the stack through the run. */
    if (s->current != NULL)
        return;
    for (size_t i = 0; i < s->nconns;) {
        if (s->conns[i].dead) {
            u64 dead_id = s->conns[i].id;
            for (size_t k = 0; k < SERVE_DIALOG_MAX; k++)
                if (s->dialogs[k].used && s->dialogs[k].conn_id == dead_id)
                    serve_dialog_dispose(&s->dialogs[k]);
            serve_close_conn(s, i);
            s->conns[i] = s->conns[s->nconns - 1];
            s->nconns--;
        } else {
            i++;
        }
    }
}

/* Read and decode whatever is ready on every connection, then drain the ready
 * request queue. `timeout_ms` bounds the wait; re-entered from the agent
 * event/pump seams with 0. Returns a negative errno on a fatal poll error (a
 * signal interrupt is retried), 0 otherwise. */
static int serve_service_input(Serve *s, int timeout_ms) {
    struct os_pollfd fds[SERVE_MAX_CONNS + 1];
    int nfds = 0;
    fds[nfds].fd = s->lfd;
    fds[nfds].events = OS_POLLIN;
    fds[nfds].revents = 0;
    nfds++;
    for (size_t i = 0; i < s->nconns; i++) {
        ServeConn *cn = &s->conns[i];
        short ev;
        if (cn->tls && !cn->tls_ready)
            ev = (cn->tls_want == 2) ? OS_POLLOUT : OS_POLLIN;
        else
            ev = OS_POLLIN;
        if (cn->out.len)
            ev = (short)(ev | OS_POLLOUT);
        fds[nfds].fd = cn->fd;
        fds[nfds].events = ev;
        fds[nfds].revents = 0;
        nfds++;
    }
    int pr = os_poll(fds, nfds, timeout_ms);
    if (pr < 0 && pr != -4)
        return pr;             /* -EINTR retries; anything else is fatal */
    size_t polled = s->nconns; /* only these have a valid pollfd entry */
    if (pr > 0 && (fds[0].revents & OS_POLLIN)) {
        for (;;) {
            int nfd = agentc_net_accept(s->lfd);
            if (nfd == -11 || nfd == -38)
                break;
            if (nfd < 0)
                break;
            if (s->nconns >= SERVE_MAX_CONNS) {
                agentc_net_close(nfd);
                continue;
            }
            ServeConn *nc = &s->conns[s->nconns++];
            agentc_memset(nc, 0, sizeof *nc);
            nc->fd = nfd;
            nc->id = ++s->next_conn_id;
            nc->deadline_ns =
                os_now_ns(OS_CLOCK_MONOTONIC) + s->hello_timeout_ms * 1000000; /* S8 */
            agentc_frame_decoder_init(&nc->dec);
            nc->ws = s->ws;
            if (nc->ws)
                agentc_ws_decoder_init(&nc->ws_dec);
            if (s->tls) {
                char terr[256];
                nc->tls =
                    agentc_tls_server_new(nfd, s->tls_cert.p, s->tls_cert.len + 1, s->tls_key.p,
                                          s->tls_key.len + 1, terr, sizeof terr);
                if (!nc->tls) {
                    s->nconns--;
                    agentc_net_close(nfd);
                    continue;
                }
                nc->tls_want = 0;
            }
        }
    }
    for (size_t i = 0; i < polled; i++) {
        ServeConn *conn = &s->conns[i];
        short rev = fds[i + 1].revents;
        /* A peer close may surface as POLLHUP/POLLERR rather than POLLIN
         * (notably on Windows); read first so any buffered bytes drain, then let
         * a 0/negative recv mark the connection dead. */
        bool readable = (rev & (OS_POLLIN | OS_POLLHUP | OS_POLLERR)) != 0;
        bool writable = (rev & OS_POLLOUT) != 0;
        if (conn->tls && !conn->tls_ready) {
            if ((readable || writable) && serve_tls_progress(conn) < 0)
                conn->dead = true;
            readable = (rev & (OS_POLLIN | OS_POLLHUP | OS_POLLERR)) != 0 && conn->tls_ready;
            writable = writable && conn->tls_ready;
        }
        if (conn->dead)
            continue;
        if (readable) {
            u8 buf[65536];
            size_t budget = SERVE_READ_BUDGET; /* S9: bounded work per call */
            for (;;) {
                int n = serve_conn_recv(conn, buf, sizeof buf);
                if (n == -11)
                    break;
                if (n <= 0) {
                    conn->dead = true;
                    break;
                }
                serve_conn_touch(conn, s);
                bool last = (size_t)n >= budget;
                if (last)
                    budget = 0;
                else
                    budget -= (size_t)n;
                if (conn->ws && !conn->ws_ready) {
                    /* HTTP upgrade: accumulate a bounded header block. */
                    agentc_buf_push(&conn->ws_in, buf, (size_t)n);
                    AgcBuf resp = {0};
                    int hr = agentc_ws_server_handshake_with(&conn->ws_in, &resp, serve_ws_allow, s);
                    if (resp.len) {
                        serve_conn_push_raw(conn, resp.p, resp.len);
                        serve_conn_flush(conn);
                    }
                    agentc_buf_free(&resp);
                    if (hr < 0) {
                        conn->dead = true;
                        break;
                    }
                    if (hr == 1) {
                        conn->ws_ready = true;
                        if (conn->ws_in.len) {
                            ServeWsFeed wf = {s, conn};
                            int wr = agentc_ws_decoder_feed(&conn->ws_dec, conn->ws_in.p,
                                                            conn->ws_in.len, serve_ws_cb, &wf);
                            conn->ws_in.len = 0;
                            if (wr != 0) {
                                conn->dead = true;
                                break;
                            }
                        }
                    }
                    if (last)
                        break;
                    continue;
                }
                if (conn->ws) {
                    ServeWsFeed wf = {s, conn};
                    int wr =
                        agentc_ws_decoder_feed(&conn->ws_dec, buf, (size_t)n, serve_ws_cb, &wf);
                    if (wr != 0) {
                        conn->dead = true;
                        break;
                    }
                    if (last)
                        break;
                    continue;
                }
                ServeFeed fc = {s, conn};
                int fr = agentc_frame_decoder_feed(&conn->dec, buf, (size_t)n, serve_feed_cb, &fc);
                if (fr != 0) {
                    conn->dead = true;
                    break;
                }
                if (last)
                    break;
            }
        }
        if (writable)
            serve_conn_flush(conn);
    }
    serve_expire(s);
    serve_reap(s);
    serve_dialog_expire(s);
    serve_drain_actions(s);
    return 0;
}

/* The agent pump and the wire poll hook chain the shared mode pump (MCP/re-sync,
 * bus/actor tick) and then service the served sockets with a zero timeout, so a
 * `cancel` can interrupt a stalled run. */
static void serve_pump(void *ud, int timeout_ms) {
    Serve *s = ud;
    if (timeout_ms > 0)
        serve_service_input(s, 0);
    agentc_mode_pump(NULL, timeout_ms);
}

static void serve_poll_hook(void *ud, int timeout_ms) {
    Serve *s = ud;
    agentc_mode_pump(NULL, timeout_ms);
    serve_service_input(s, 0);
}

/* Optional `new_session` registry hook (P5 C3). Selects an existing session by
 * id, or creates a fresh independent engine (its own agent/session/transcript)
 * in a free slot. The requesting connection (s->current) is moved to the
 * resulting session. The registry is bounded; a full registry fails closed. */
static int serve_session_new(void *ud, const char *spec, char out_id[64]) {
    Serve *s = ud;
    if (spec && spec[0]) {
        for (size_t i = 0; i < SERVE_MAX_SESSIONS; i++) {
            if (!s->sessions[i].used || !agentc_streq(spec, s->sessions[i].id))
                continue;
            if (out_id)
                agentc_snprintf(out_id, 64, "%s", s->sessions[i].id);
            if (s->current)
                s->current->session = (u32)i;
            return 0;
        }
        return -2; /* ENOENT: no such session */
    }
    size_t idx = SERVE_MAX_SESSIONS;
    for (size_t i = 0; i < SERVE_MAX_SESSIONS; i++)
        if (!s->sessions[i].used) {
            idx = i;
            break;
        }
    if (idx == SERVE_MAX_SESSIONS)
        return -28; /* ENOSPC: bounded registry full */

    ServeSession *ss = &s->sessions[idx];
    agentc_memset(ss, 0, sizeof *ss);
    ss->s = s;
    ss->index = (u32)idx;
    ss->rpc.cfg = *s->base_mcfg;
    ss->rpc.cfg.session_spec = NULL; /* a fresh session, not the primary spec */
    ss->rpc.cfg.continue_last = false;
    ss->io.event = serve_event;
    ss->io.event_ud = ss;
    ss->io.write = serve_capture;
    ss->io.out_ud = &ss->capture;
    ss->io.observer = NULL; /* default ctx-bound flush observer */
    int rc = agentc_mode_setup(&ss->rpc.base, &ss->rpc.cfg, &ss->io,
                               AGENTC_MODE_F_SESSION | AGENTC_MODE_F_ABORT);
    if (rc != 0) {
        agentc_buf_free(&ss->capture);
        agentc_memset(ss, 0, sizeof *ss);
        /* setup tears the globals down on failure; the server still owns the
         * pump/poll hook and the caller's session. */
        agentc_http_set_poll_hook(serve_poll_hook, s);
        agentc_pump_install(serve_pump, s);
        if (s->current_session)
            serve_activate(s->current_session);
        return rc;
    }
    ss->used = true;
    agentc_rpc_set_sink(&ss->rpc, serve_sink, s);
    ss->rpc.event = serve_event;
    ss->rpc.event_ud = ss;
    ss->rpc.session_new = serve_session_new;
    ss->rpc.session_new_ud = s;
    ss->rpc.defer_run = true; /* the serve loop steps this session's runs */
    agentc_snprintf(ss->id, sizeof ss->id, "%s",
                    ss->rpc.base.session ? agentc_session_id(ss->rpc.base.session) : "");
    s->nsessions++;
    /* agentc_mode_setup installed its own pump/poll hook; the server owns them. */
    agentc_http_set_poll_hook(serve_poll_hook, s);
    agentc_pump_install(serve_pump, s);
    /* setup left the globals on the new session; put the caller's back. */
    if (s->current_session)
        serve_activate(s->current_session);
    if (out_id)
        agentc_snprintf(out_id, 64, "%s", ss->id);
    if (s->current)
        s->current->session = (u32)idx;
    return 0;
}

/* S8 timeout override for tests: a positive integer milliseconds, else `def`. */
static i64 serve_timeout_env(const char *name, i64 def) {
    const char *v = os_getenv(name);
    if (!v || !v[0])
        return def;
    bool ok = false;
    i64 n = agentc_parse_i64(v, agentc_strlen(v), &ok);
    if (!ok || n <= 0)
        return def;
    /* N1: bound the override so `ms * 1000000` cannot signed-overflow into a
     * negative deadline that reaps every connection. */
    return n > SERVE_TIMEOUT_MAX_MS ? SERVE_TIMEOUT_MAX_MS : n;
}

int agentc_mode_serve_run(AgcModeCtx *c, const AgcServeOptions *opt) {
    if (!c || !c->agent || !c->mcfg)
        return -22;
    Serve s;
    agentc_memset(&s, 0, sizeof s);
    s.base = c;
    s.base_mcfg = c->mcfg;

    /* Session 0 wraps the caller's already-built engine (agent + session). */
    ServeSession *ss0 = &s.sessions[0];
    ss0->s = &s;
    ss0->index = 0;
    ss0->used = true;
    ss0->rpc.base = *c;
    ss0->rpc.cfg = *c->mcfg;
    ss0->rpc.base.mcfg = &ss0->rpc.cfg;
    ss0->rpc.cfg.provider = ss0->rpc.base.provider;
    ss0->rpc.cfg.model = ss0->rpc.base.model;
    ss0->rpc.event = serve_event;
    ss0->rpc.event_ud = ss0;
    ss0->rpc.session_new = serve_session_new;
    ss0->rpc.session_new_ud = &s;
    ss0->rpc.defer_run = true; /* the serve loop steps this session's runs */
    agentc_rpc_set_sink(&ss0->rpc, serve_sink, &s);
    s.orig_io = c->io;
    if (c->io)
        ss0->io = *c->io;
    ss0->io.event = serve_event;
    ss0->io.event_ud = ss0;
    ss0->io.write = serve_capture;
    ss0->io.out_ud = &ss0->capture;
    ss0->io.observer = NULL; /* default ctx-bound flush observer */
    ss0->io.observer_ud = NULL;
    ss0->rpc.base.io = &ss0->io;
    agentc_snprintf(ss0->id, sizeof ss0->id, "%s",
                    ss0->rpc.base.session ? agentc_session_id(ss0->rpc.base.session) : "");
    s.nsessions = 1;
    s.token = opt ? opt->token : NULL;
    s.ws = opt ? opt->ws : false;
    s.tls = opt && opt->tls_cert && opt->tls_cert[0];
    /* S5: a WebSocket listener must never be tokenless - the browser WebSocket
     * API bypasses same-origin, so an accidental --ws would expose the session
     * to any page. Fail closed before binding. */
    if (s.ws && !(s.token && s.token[0])) {
        agentc_logf(3, "serve: --ws requires a non-empty --token (fail-closed)");
        return -22;
    }
    s.hello_timeout_ms = serve_timeout_env("AGENTC_SERVE_HELLO_TIMEOUT_MS", SERVE_HELLO_TIMEOUT_MS);
    s.idle_timeout_ms = serve_timeout_env("AGENTC_SERVE_IDLE_TIMEOUT_MS", SERVE_IDLE_TIMEOUT_MS);
    if (s.tls) {
        if (!agentc_tls_server_supported()) {
            agentc_logf(3, "serve: --tls is not supported on this platform");
            return -38;
        }
        int er = serve_read_file(opt->tls_cert, &s.tls_cert);
        if (er != 0)
            return er;
        er = serve_read_file(opt->tls_key, &s.tls_key);
        if (er != 0) {
            agentc_buf_free(&s.tls_cert);
            return er;
        }
    }

    /* Listener: unix primary, loopback TCP fallback (C-D3); `--tcp` and the C3
     * `--host`/`--remote` binds are both explicit TCP listeners. WebSocket is
     * a TCP-only upgrade (C3). */
    const char *tcp_spec = opt ? opt->tcp : NULL;
    if ((!tcp_spec || !tcp_spec[0]) && opt && opt->host && opt->host[0])
        tcp_spec = opt->host;
    if (tcp_spec && tcp_spec[0]) {
        u32 ip = 0;
        u16 port = 0;
        if (serve_parse_tcp(tcp_spec, &ip, &port) != 0)
            return -22;
        /* Fail-closed: a bind outside 127.0.0.0/8 exposes the session to the
         * network and must be token-authenticated. */
        u8 o[4];
        agentc_memcpy(o, &ip, 4);
        if (o[0] != 127 && !(opt && opt->token && opt->token[0]))
            return -22;
        s.ws_loopback = (o[0] == 127);
        s.lfd = agentc_net_tcp_listen(ip, port, SERVE_BACKLOG);
        agentc_snprintf(s.transport, sizeof s.transport, "tcp");
        if (s.lfd >= 0) {
            char a[512];
            if (agentc_net_local_addr(s.lfd, a, sizeof a) == 0)
                agentc_snprintf(s.path, sizeof s.path, "%s", a[4] ? a + 4 : a);
        }
    } else {
        if (s.ws || s.tls)
            return -22; /* --ws/--tls require a TCP listener */
        if (opt && opt->socket_path && opt->socket_path[0]) {
            agentc_snprintf(s.path, sizeof s.path, "%s", opt->socket_path);
        } else if (serve_default_path(s.path, sizeof s.path) != 0) {
            return -36;
        }
        int dr = serve_mkdirs(s.path);
        if (dr != 0)
            return dr;
        char sockpath[512];
        agentc_snprintf(sockpath, sizeof sockpath, "%s", s.path);
        s.lfd = agentc_net_unix_listen(sockpath, SERVE_BACKLOG);
        agentc_snprintf(s.transport, sizeof s.transport, "unix");
    }
    if (s.lfd < 0)
        return s.lfd;

    agentc_proto_uuid(s.server_id);
    /* P8-D1: session 0's agents were bound to the caller's ctx at setup; the
     * persistence observers derive from the agent, so re-point them at this
     * stack copy for the server's lifetime and back before returning. */
    agentc_mode_agents_rebind(c, &ss0->rpc.base);
    agentc_agent_set_events(ss0->rpc.base.agent, serve_event, ss0);
    AgcRpcUiSaved ui_saved;
    agentc_rpc_ui_install(&ss0->rpc, &ui_saved);
    AgcExtUiPromptFn prompt_prev = NULL;
    void *prompt_prev_ud = NULL;
    agentc_ext_get_ui_prompt(&prompt_prev, &prompt_prev_ud);
    agentc_ext_set_ui_prompt(serve_ui_prompt, &s);
    agentc_approval_set_ui(serve_approval_ui, &s);
    agentc_http_set_poll_hook(serve_poll_hook, &s);
    agentc_pump_install(serve_pump, &s);
    /* Reuse the app's signal mechanism: a SIGINT/SIGTERM latches the shutdown
     * flag, the loop drains and the teardown below unlinks the socket. */
    g_serve_stop = 0;
    (void)os_sig_install(OS_SIGINT, serve_signal);
    (void)os_sig_install(OS_SIGTERM, serve_signal);
    serve_ready(&s);

    while (!s.shutdown) {
        if (g_serve_stop) {
            s.shutdown = true;
            break;
        }
        if (serve_service_input(&s, 100) < 0)
            break;
        /* Advance each session's active run by one bounded slice per loop, so a
         * stalled run on one session never blocks progress on another. */
        serve_advance_runs(&s);
        /* Idle extension work (MCP connect/re-sync, actor tick) runs with the
         * primary session's globals bound, matching the single-session default. */
        agentc_mode_activate(&ss0->rpc.base);
        agentc_mode_pump(&ss0->rpc.base, 0);
    }

    if (agentc_streq(s.transport, "unix"))
        (void)agentc_net_unlink_socket(s.path);
    for (size_t i = 0; i < s.nconns; i++)
        serve_close_conn(&s, i);
    agentc_net_close(s.lfd);
    agentc_http_set_poll_hook(agentc_mode_pump, NULL);
    agentc_pump_install(agentc_mode_pump, NULL);
    agentc_rpc_ui_restore(&ui_saved);
    agentc_approval_set_ui(NULL, NULL);
    agentc_ext_set_ui_prompt(prompt_prev, prompt_prev_ud);
    agentc_mode_agents_rebind(&ss0->rpc.base, c);
    for (size_t i = 0; i < SERVE_DIALOG_MAX; i++)
        serve_dialog_dispose(&s.dialogs[i]);
    for (size_t i = s.actions_head; i < s.actions.len; i++)
        agentc_free(((ServeAction *)s.actions.p)[i].line);
    agentc_vec_free(&s.actions);
    /* Tear down every extra session, then hand session 0 back to the caller. */
    for (size_t i = 1; i < SERVE_MAX_SESSIONS; i++) {
        if (!s.sessions[i].used)
            continue;
        agentc_mode_teardown(&s.sessions[i].rpc.base);
        agentc_buf_free(&s.sessions[i].capture);
    }
    ss0->rpc.base.io = c->io;
    ss0->rpc.base.mcfg = c->mcfg;
    *c = ss0->rpc.base;
    agentc_buf_free(&ss0->capture);
    agentc_buf_free(&s.tls_cert);
    agentc_buf_free(&s.tls_key);
    return 0;
}
