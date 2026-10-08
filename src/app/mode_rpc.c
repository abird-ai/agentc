/* mode_rpc.c — `--mode rpc`: JSONL commands on stdin, responses/events out.
 *
 * The canonical contract is .agents/design/60-rpc.md, frozen at
 * AGENTC_RPC_VERSION (app/mode.h). Each reply is
 *   {"type":"response","id":…,"command":…,"success":true,"code":"ok","data":{…}}
 *   {"type":"response","id":…,"command":…,"success":false,"code":"…","error":"…"}
 * Agent events keep the --mode json schema; RPC adds agent_settled and the
 * fire-and-forget ui_request.
 *
 * The command vocabulary lives in app/rpc_dispatch.c (C-D2); this file owns the
 * JSONL framing: stdin line buffering, the bounded pending queue and the
 * accept-first/queued dispositions while a run is active.
 */
#include "app/rpc_dispatch.h"

#include "base/limits.h"
#include "ext.h"
#include "plat.h"

/* Pending-queue backpressure (A-D10). While a run is active the pump stops
 * reading stdin once this much is queued; a single record above the byte cap is
 * refused `busy` rather than dropped silently. */
#define RPC_PENDING_MAX_RECORDS 256u
#define RPC_PENDING_MAX_BYTES (1u << 20)

static const char RPC_QUEUED[] = "{\"disposition\":\"queued\"}";

/* One queued command. `acked` marks a prompt already answered `queued` during
 * the run, so dispatching it after settle must not answer a second time. */
typedef struct {
    char *line;
    bool acked;
} RpcPending;

/* The shared ctx is the first member (via AgcRpc): agentc_mode_event() and the
 * rebuild path cast the callback userdata to AgcModeCtx. */
typedef struct {
    AgcRpc rpc;
    AgcBuf inbuf;   /* partial input line */
    AgcVec pending; /* RpcPending records queued during a run */
    size_t pending_head;
    size_t pending_bytes; /* bytes of lines from pending_head..len */
    bool eof;
} RpcCtx;

/* Responses and settled/UI records are written to stdout by the dispatcher's
 * sink; agent events keep the shared --mode json writer. */
static void rpc_sink(void *ud, const char *line, size_t n) {
    RpcCtx *r = ud;
    agentc_mode_out(&r->rpc.base, line, n);
}

/* ------------------------------------------------------------- input */

/* Extract one complete line (without "\n" / "\r"). Returns an owned string. */
static char *extract_line(AgcBuf *in) {
    size_t i = 0;
    while (i < in->len && in->p[i] != '\n')
        i++;
    if (i >= in->len)
        return NULL;
    size_t n = i;
    char *line = agentc_strdup_len((const char *)in->p, n);
    if (n && line[n - 1] == '\r')
        line[--n] = 0;
    agentc_memmove(in->p, in->p + i + 1, in->len - i - 1);
    in->len -= i + 1;
    return line;
}

static size_t rpc_pending_count(const RpcCtx *r) {
    return r->pending.len - r->pending_head;
}

static bool rpc_pending_full(const RpcCtx *r) {
    return rpc_pending_count(r) >= RPC_PENDING_MAX_RECORDS ||
           r->pending_bytes >= RPC_PENDING_MAX_BYTES;
}

static void rpc_pending_push(RpcCtx *r, char *line, bool acked) {
    RpcPending *p = agentc_vec_push(&r->pending, sizeof *p);
    p->line = line;
    p->acked = acked;
    r->pending_bytes += agentc_strlen(line) + 1;
}

static RpcPending *rpc_pending_pop(RpcCtx *r) {
    if (r->pending_head >= r->pending.len)
        return NULL;
    RpcPending *p = &((RpcPending *)r->pending.p)[r->pending_head++];
    r->pending_bytes -= agentc_strlen(p->line) + 1;
    return p;
}

/* Drop delivered records from the front so the array cannot grow without bound
 * across a long session. Safe after the popped line has been consumed. */
static void rpc_pending_compact(RpcCtx *r) {
    if (r->pending_head == 0)
        return;
    if (r->pending_head >= r->pending.len) {
        agentc_vec_free(&r->pending);
        r->pending_head = 0;
        r->pending_bytes = 0;
        return;
    }
    size_t n = r->pending.len - r->pending_head;
    agentc_memmove(r->pending.p, (RpcPending *)r->pending.p + r->pending_head,
                   n * sizeof(RpcPending));
    r->pending.len = n;
    r->pending_head = 0;
}

static void rpc_read_available(RpcCtx *r) {
    u8 tmp[4096];
    for (int i = 0; i < 64; i++) {
        if (rpc_pending_full(r))
            return; /* backpressure: stop consuming */
        int n = r->rpc.base.io->read(r->rpc.base.io->in_ud, tmp, sizeof tmp);
        if (n > 0) {
            agentc_buf_push(&r->inbuf, tmp, (size_t)n);
            continue;
        }
        if (n == 0)
            r->eof = true;
        return;
    }
}

/* -------------------------------------------------------- command pump */

/* A prompt is worth a `queued` ack only if it would be accepted at dispatch
 * time; otherwise it is queued un-acked and answered with its pre-acceptance
 * error after settle (no double response). */
static bool rpc_prompt_valid(const AgcJson *req, const char *cmd) {
    if (agentc_streq(cmd, "prompt"))
        return agentc_json_get_str(req, "text") || agentc_json_get_str(req, "message") ||
               agentc_json_get_str(req, "prompt");
    const char *name = agentc_json_get_str(req, "name");
    return name && name[0];
}

/* Drain input that is available right now. `abort` is handled immediately;
 * prompts are acked `queued`; everything else is queued for after the run
 * (A-D3). The queue is bounded and the reader stops once it is full (A-D10). */
static void rpc_pump(RpcCtx *r) {
    AgcJsonArena *ja = agentc_json_arena_new(0);
    rpc_read_available(r);
    for (;;) {
        char *line = extract_line(&r->inbuf);
        if (!line)
            break;
        size_t n = agentc_strlen(line);
        if (!n) {
            agentc_free(line);
            continue;
        }
        AgcJson *req = agentc_json_parse_in(ja, line, n);
        const char *type = agentc_json_get_str(req, "type");
        if (type && agentc_streq(type, "ui_response")) {
            agentc_free(line); /* dialog reply: parsed and dropped */
            continue;
        }
        const char *cmd = agentc_rpc_command_name(req);
        r->rpc.pending_count = rpc_pending_count(r);
        if (cmd && agentc_streq(cmd, "abort")) {
            (void)agentc_rpc_dispatch(&r->rpc, line, n, false);
            agentc_free(line);
            continue;
        }
        bool prompt = cmd && (agentc_streq(cmd, "prompt") || agentc_streq(cmd, "prompt_template"));
        if (prompt) {
            if (n + 1 > RPC_PENDING_MAX_BYTES) { /* last-resort refusal */
                agentc_rpc_respond(&r->rpc, req, cmd, false, NULL, 0, "busy: record too large");
                agentc_free(line);
                continue;
            }
            if (rpc_prompt_valid(req, cmd)) {
                agentc_rpc_respond(&r->rpc, req, cmd, true, RPC_QUEUED, sizeof RPC_QUEUED - 1,
                                   NULL);
                rpc_pending_push(r, line, true);
            } else {
                rpc_pending_push(r, line, false);
            }
        } else {
            rpc_pending_push(r, line, false);
        }
    }
    agentc_json_arena_free(ja);
}

static void rpc_event(void *ud, int ev, const void *data) {
    RpcCtx *r = ud;
    agentc_mode_event(ud, ev, data);
    if (agentc_rpc_running(&r->rpc))
        rpc_pump(r);
}

static bool rpc_handle(RpcCtx *r, const char *line, size_t n, bool acked) {
    r->rpc.pending_count = rpc_pending_count(r);
    return agentc_rpc_dispatch(&r->rpc, line, n, acked);
}

static bool rpc_next_command(RpcCtx *r, char **line, bool *acked) {
    RpcPending *p = rpc_pending_pop(r);
    if (p) {
        *line = p->line;
        *acked = p->acked;
        return true;
    }
    for (;;) {
        char *l = extract_line(&r->inbuf);
        if (l) {
            *line = l;
            *acked = false;
            return true;
        }
        if (r->eof)
            return false;
        u8 tmp[4096];
        int n = r->rpc.base.io->read(r->rpc.base.io->in_ud, tmp, sizeof tmp);
        if (n > 0) {
            agentc_buf_push(&r->inbuf, tmp, (size_t)n);
            continue;
        }
        if (n == 0) {
            r->eof = true;
            continue;
        }
        if (n == -11) {
            /* Between reads: keep deferred extension work (MCP) moving while
             * the loop waits for the next command. */
            agentc_mode_pump(NULL, 0);
            os_sleep_ns(10 * 1000000);
            continue;
        }
        r->eof = true;
    }
}

int agentc_mode_rpc_run(AgcModeCtx *c) {
    if (!c || !c->agent || !c->mcfg || !c->io || !c->io->read || !c->io->write)
        return -22;
    RpcCtx r;
    agentc_memset(&r, 0, sizeof r);
    r.rpc.base = *c;
    r.rpc.cfg = *c->mcfg;
    r.rpc.base.mcfg = &r.rpc.cfg;
    r.rpc.cfg.provider = r.rpc.base.provider;
    r.rpc.cfg.model = r.rpc.base.model;
    r.rpc.event = rpc_event;
    r.rpc.event_ud = &r;
    agentc_rpc_set_sink(&r.rpc, rpc_sink, &r);
    /* The queue/agent/model-route persistence observers derive their owner from
     * the emitting agent (P8-D1); re-point this ctx's agents at the live stack
     * copy for the run. The caller's ctx is restored on the way out. */
    agentc_mode_agents_rebind(c, &r.rpc.base);
    agentc_agent_set_events(r.rpc.base.agent, rpc_event, &r);
    /* UI sink + status observer for the duration of the run (A-D11); the
     * previous owners (the TUI's sink, if any) are restored on the way out. */
    AgcRpcUiSaved ui_saved;
    agentc_rpc_ui_install(&r.rpc, &ui_saved);
    agentc_mode_write_session_header(&r.rpc.base);

    char *line = NULL;
    bool acked = false;
    while (rpc_next_command(&r, &line, &acked)) {
        size_t n = agentc_strlen(line);
        if (n)
            (void)rpc_handle(&r, line, n, acked);
        agentc_free(line);
        line = NULL;
        rpc_pending_compact(&r);
    }
    for (size_t i = r.pending_head; i < r.pending.len; i++)
        agentc_free(((RpcPending *)r.pending.p)[i].line);
    agentc_vec_free(&r.pending);
    agentc_buf_free(&r.inbuf);
    agentc_rpc_ui_restore(&ui_saved);
    r.rpc.base.mcfg = c->mcfg; /* the mutable copy is stack-local */
    *c = r.rpc.base;
    agentc_mode_agents_rebind(&r.rpc.base, c);
    /* `set_model`'s rebuild and `new_session`'s rebind bound the persistence
     * observer to this function's stack frame (`&r.rpc.base`); re-point it at
     * the caller's ctx before returning so a later transcript append cannot reach
     * a dangling frame. Reusing the shared binder keeps the default-observer
     * policy in mode.c as the single source of truth. */
    agentc_mode_rebind_session(c);
    return 0;
}
