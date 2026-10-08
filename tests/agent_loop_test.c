/* agent_loop_test.c — the turn loop with an injected replaying transport and
 * a stub tool: event trace, transcript shape and retry behaviour.
 */
#include "agent.h"
#include "app/mode.h"
#include "app/setup.h"
#include "discover.h"
#include "ext.h"
#include "ext/registry_int.h"
#include "net/net_internal.h"
#include "plat.h"
#include "prov/provider.h"

/* internal test hook from agent.c */
void agentc_agent_test_no_backoff(AgcAgent *a);

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

/* internal test hooks (config.c) */
void agentc_test_setenv(const char *name, const char *value);
void agentc_test_clearenv(void);
void agentc_rm_rf(const char *path);

/* internal tool entry points (tools/find.c, tools/grep.c) */
char *agentc_tool_find(const char *pattern, const char *path, i64 limit, bool *is_error);
char *agentc_tool_grep(const char *pattern, const char *path, const char *glob,
                   bool ignore_case, bool literal, i64 context, i64 limit,
                   bool *is_error);

/* ----------------------------------------------------- replaying transport */

typedef struct {
    int codes[64];
    const char *bodies[64];
    size_t n, i;
    size_t requests;              /* total transport calls, incl. repeats */
    bool repeat_last;             /* serve the last response after n is exhausted */
    AgcBuf last_body;
    size_t last_body_len;
    AgcBuf last_headers;          /* captured only when capture_headers is set */
    AgcBuf first_body;            /* the first request, same gate */
    AgcBuf first_headers;
    bool capture_headers;
    int chunks;
} Replay;

static int replay_request(void *ud, const char *url, const char *headers, const void *body,
                          size_t body_len, int (*on_chunk)(void *, const void *, size_t),
                          void *u, int timeout_ms, AgcBuf *record) {
    (void)url;
    (void)headers;
    (void)timeout_ms;
    (void)record;
    Replay *r = ud;
    agentc_buf_clear(&r->last_body);
    agentc_buf_push(&r->last_body, body, body_len);
    r->last_body_len = body_len;
    if (r->capture_headers) {
        agentc_buf_clear(&r->last_headers);
        if (headers) agentc_buf_cstr(&r->last_headers, headers);
        if (r->requests == 0) {
            agentc_buf_clear(&r->first_headers);
            if (headers) agentc_buf_cstr(&r->first_headers, headers);
            agentc_buf_clear(&r->first_body);
            if (body && body_len) agentc_buf_push(&r->first_body, body, body_len);
        }
    }
    int code;
    const char *resp;
    if (r->i < r->n) {
        code = r->codes[r->i];
        resp = r->bodies[r->i];
        r->i++;
    } else if (r->repeat_last && r->n > 0) {
        code = r->codes[r->n - 1];
        resp = r->bodies[r->n - 1];
    } else {
        return -5;
    }
    r->requests++;
    if (code != 0) return code;
    if (resp) {
        size_t len = agentc_strlen(resp);
        for (size_t off = 0; off < len; off += 7) {
            size_t n = len - off;
            if (n > 7) n = 7;
            r->chunks++;
            if (on_chunk(u, resp + off, n)) return -125;
        }
    }
    return 0;
}

/* --------------------------------------------------------- stub tool */

static int stub_calls;

static char *stub_exec(const char *args_json, bool *is_error, const volatile bool *cancel) {
    (void)cancel;
    stub_calls++;
    if (is_error) *is_error = false;
    AgcBuf b = { 0 };
    agentc_buf_printf(&b, "stub(%s)", args_json ? args_json : "");
    return (char *)b.p;
}

static const AgcTool stub_tool = {
    .name = "read", .label = "Read", .desc = "stub read",
    .params_json = "{\"type\":\"object\"}", .flags = AGENTC_TOOL_READONLY,
    .exec = stub_exec
};

/* Registered but not declared to the model. */
static const AgcTool hidden_tool = {
    .name = "secret", .label = "Secret", .desc = "hidden helper",
    .params_json = "{\"type\":\"object\"}",
    .flags = AGENTC_TOOL_READONLY | AGENTC_TOOL_HIDDEN, .exec = stub_exec,
};

/* A tool registered in the registry after startup: the app-installed recompose
 * callback picks it up at the turn boundary, so the provider request after the
 * tool batch advertises it (internal bus; async MCP uses the same seam). */
static int late_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                    bool *is_error) {
    (void)self;
    (void)call;
    if (is_error) *is_error = false;
    agentc_buf_cstr(out, "late-ok");
    return 0;
}
static const AgcTool late_tool = {
    .name = "late_tool", .label = "Late tool", .desc = "registered after startup",
    .params_json = "{\"type\":\"object\"}", .flags = AGENTC_TOOL_READONLY,
    .run = late_run,
};

/* The app's recompose policy in miniature: re-read the registry and install. */
static int g_recompose_calls;
static void test_recompose_cb(void *ud, AgcAgent *a) {
    (void)ud;
    if (!agentc_ext_dirty()) return;
    agentc_ext_clear_dirty();
    size_t n = agentc_ext_tools(NULL, 0);
    AgcTool *tools = agentc_alloc((n ? n : 1) * sizeof *tools);
    n = agentc_ext_tools(tools, n);
    agentc_agent_set_tools(a, tools, n);
    agentc_free(tools);
    g_recompose_calls++;
}

/* Observer used by the orphan-result regression: counts persisted tool
 * messages. A failed turn must never produce one. */
typedef struct {
    int tool_msgs;
} OrphanWatch;

static void orphan_observer(void *ud, const AgcMsg *m) {
    OrphanWatch *ow = ud;
    if (m->role == AGENTC_ROLE_TOOL) ow->tool_msgs++;
}

/* Observer used by the message_end terminal-failure regression: counts the
 * assistant messages a front end (the app's session_observer) would persist. */
typedef struct {
    int persistable;
} PersistWatch;

static void persist_observer(void *ud, const AgcMsg *m) {
    PersistWatch *pw = ud;
    if (m && m->role == AGENTC_ROLE_ASSISTANT && agentc_mode_msg_persistable(m))
        pw->persistable++;
}

/* --------------------------------------------------------- direct hooks */

/* Records each direct-hook dispatch: the first and last payloads, the call
 * count, and an optional JSON patch the handler returns for the first
 * `result_calls` calls. */
typedef struct {
    AgcBuf payload;
    AgcBuf first_payload;
    int calls;
    const char *result;
    int result_calls;
} HookRec;

static int hook_rec_fn(void *ud, const char *point, const char *payload_json,
                       char **result_json) {
    (void)point;
    HookRec *h = ud;
    h->calls++;
    agentc_buf_clear(&h->payload);
    if (payload_json) agentc_buf_cstr(&h->payload, payload_json);
    if (h->calls == 1) {
        agentc_buf_clear(&h->first_payload);
        if (payload_json) agentc_buf_cstr(&h->first_payload, payload_json);
    }
    if (result_json && h->result && h->calls <= h->result_calls)
        *result_json = agentc_strdup(h->result);
    return 0;
}

static uint64_t hook_rec_on(const char *point, uint32_t caps, HookRec *h) {
    return agentc_ext_host()->on(point, caps, 0, hook_rec_fn, h);
}

static void hook_rec_clear(HookRec *h) {
    agentc_buf_free(&h->payload);
    agentc_buf_free(&h->first_payload);
    agentc_memset(h, 0, sizeof *h);
}

static bool buf_eq(const AgcBuf *b, const char *s) {
    return b->p != NULL && agentc_streq((const char *)b->p, s);
}

static bool buf_has(const AgcBuf *b, const char *needle) {
    return b->p != NULL && agentc_str_str((const char *)b->p, needle) != NULL;
}

static int count_occurrences(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return 0;
    int n = 0;
    size_t nl = agentc_strlen(needle);
    for (size_t i = 0; i + nl <= agentc_strlen(hay);) {
        if (agentc_memeq(hay + i, needle, nl)) {
            n++;
            i += nl;
        } else {
            i++;
        }
    }
    return n;
}

static int fail_hook_fn(void *ud, const char *point, const char *payload_json,
                        char **result_json) {
    (void)ud;
    (void)point;
    (void)payload_json;
    if (result_json) *result_json = NULL;
    return -1;
}

/* Ordering probes for the message_end test: the typed event and the observer
 * record how many hook calls had happened when they ran. */
static HookRec *g_msg_end_hook;
static int g_msg_end_hook_seen;
static int g_msg_obs_hook_seen;
static int g_obs_calls;
static AgcBuf g_obs_text;
static HookRec *g_settled_hook;
static int g_settle_seen_at_agent_end;

typedef struct {
    AgcBuf log;
    int calls;
} EntrySink;

static void test_entry_sink(void *ud, const char *type, const char *data_json) {
    EntrySink *s = ud;
    s->calls++;
    agentc_buf_printf(&s->log, "%s:%s\n", type, data_json ? data_json : "null");
}

static void msg_order_observer(void *ud, const AgcMsg *m) {
    (void)ud;
    if (!m || m->role != AGENTC_ROLE_ASSISTANT) return;
    g_obs_calls++;
    agentc_buf_clear(&g_obs_text);
    for (size_t i = 0; i < m->nblocks; i++)
        if (m->blocks[i].type == AGENTC_BLK_TEXT && m->blocks[i].text)
            agentc_buf_cstr(&g_obs_text, m->blocks[i].text);
    g_msg_obs_hook_seen = g_msg_end_hook ? g_msg_end_hook->calls : -1;
}

/* ------------------------------------------------------------- event trace */

typedef struct {
    AgcBuf tr;
} Trace;

/* Last AGENTC_EV_AGENT_END payload captured by collector(); the same
 * terminal_stop value the extension bridge serializes as {stop_reason}. */
static int g_agent_end_stop;
static bool g_agent_end_stop_seen;

static AgcAgent *abort_agent;

static void collector(void *ud, int ev, const void *data) {
    Trace *t = ud;
    switch (ev) {
    case AGENTC_EV_AGENT_START: agentc_buf_cstr(&t->tr, "agent_start\n"); break;
    case AGENTC_EV_TURN_START: agentc_buf_cstr(&t->tr, "turn_start\n"); break;
    case AGENTC_EV_MSG_START: agentc_buf_cstr(&t->tr, "msg_start\n"); break;
    case AGENTC_EV_MSG_RESET: agentc_buf_cstr(&t->tr, "msg_reset\n"); break;
    case AGENTC_EV_TEXT_DELTA: {
        const AgcTextDelta *d = data;
        agentc_buf_cstr(&t->tr, "text:");
        agentc_buf_push(&t->tr, d->text, d->len);
        agentc_buf_byte(&t->tr, '\n');
        break;
    }
    case AGENTC_EV_THINK_DELTA: {
        const AgcTextDelta *d = data;
        agentc_buf_cstr(&t->tr, "think:");
        agentc_buf_push(&t->tr, d->text, d->len);
        agentc_buf_byte(&t->tr, '\n');
        break;
    }
    case AGENTC_EV_TOOL_ARGS_DELTA: {
        const AgcTextDelta *d = data;
        agentc_buf_cstr(&t->tr, "args:");
        agentc_buf_push(&t->tr, d->text, d->len);
        agentc_buf_byte(&t->tr, '\n');
        break;
    }
    case AGENTC_EV_MSG_END: {
        const AgcMsg *m = data;
        agentc_buf_printf(&t->tr, "msg_end:%d:%d\n", m->role, m->stop_reason);
        if (g_msg_end_hook) g_msg_end_hook_seen = g_msg_end_hook->calls;
        break;
    }
    case AGENTC_EV_TOOL_EXEC_START: {
        const AgcToolExec *e = data;
        agentc_buf_printf(&t->tr, "tool_start:%s:%s\n", e->tool_name, e->args_json);
        break;
    }
    case AGENTC_EV_TOOL_EXEC_END: {
        const AgcToolExec *e = data;
        agentc_buf_printf(&t->tr, "tool_end:%s:%s:%s\n", e->tool_name, e->result,
                      e->is_error ? "err" : "ok");
        break;
    }
    case AGENTC_EV_TURN_END: agentc_buf_cstr(&t->tr, "turn_end\n"); break;
    case AGENTC_EV_AGENT_END:
        agentc_buf_cstr(&t->tr, "agent_end\n");
        if (data) {
            g_agent_end_stop = *(const int *)data;
            g_agent_end_stop_seen = true;
        }
        if (g_settled_hook) g_settle_seen_at_agent_end = g_settled_hook->calls;
        break;
    case AGENTC_EV_ERROR: agentc_buf_printf(&t->tr, "error:%s\n", (const char *)data); break;
    default: break;
    }
    if (abort_agent && ev == AGENTC_EV_TEXT_DELTA) agentc_agent_abort(abort_agent);
}

/* --------------------------------------------------------------- responses */

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

/* ------------------------------------------------------------------- tests */

static void test_tool_round_trip(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    check("loop_rc", rc == 0);
    check("loop_requests", r.i == 2);
    check("loop_tool_calls", stub_calls == 1);

    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("loop_msgs", tp && tp->n == 4);
    bool roles = tp && tp->n == 4 && tp->msgs[0].role == AGENTC_ROLE_USER &&
                 tp->msgs[1].role == AGENTC_ROLE_ASSISTANT &&
                 tp->msgs[2].role == AGENTC_ROLE_TOOL && tp->msgs[3].role == AGENTC_ROLE_ASSISTANT;
    check("loop_roles", roles);
    bool tool_result = tp && tp->n == 4 && tp->msgs[2].nblocks == 1 &&
                       agentc_streq(tp->msgs[2].blocks[0].text, "stub({\"path\":\"f.txt\"})") &&
                       agentc_streq(tp->msgs[2].blocks[0].tool_id, "toolu_1");
    check("loop_tool_result", tool_result);
    bool final_text = tp && tp->n == 4 && tp->msgs[3].nblocks == 1 &&
                      agentc_streq(tp->msgs[3].blocks[0].text, "all done") &&
                      tp->msgs[3].stop_reason == AGENTC_STOP_STOP;
    check("loop_final_text", final_text);
    check("loop_rebuilt_request",
          agentc_str_str((const char *)r.last_body.p, "tool_result") != NULL &&
              agentc_str_str((const char *)r.last_body.p, "stub(") != NULL);

    agentc_outs((const char *)tr.tr.p);
    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

static void test_retry_429(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 3;
    r.codes[0] = 429;
    r.codes[1] = 429;
    r.codes[2] = 0;
    r.bodies[2] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_retry(a, 3);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);

    int rc = agentc_agent_submit(a, "go");
    check("retry_rc", rc == 0);
    check("retry_attempts", r.i == 3);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("retry_msgs", tp && tp->n == 2 && agentc_agent_last_error(a) == NULL);
    bool final_text = tp && tp->n == 2 && tp->msgs[1].nblocks == 1 &&
                      agentc_streq(tp->msgs[1].blocks[0].text, "all done");
    check("retry_final_text", final_text);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

static void test_retry_dropped(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = NULL; /* clean transport, zero bytes: dropped before content */
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_retry(a, 2);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);

    int rc = agentc_agent_submit(a, "go");
    check("retry_drop_rc", rc == 0);
    check("retry_drop_attempts", r.i == 2);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* A dropped stream that already streamed text retries with AGENTC_EV_MSG_RESET
 * so a front end discards the abandoned attempt's deltas before the next try. */
static const char reply_drop_after_text[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"partial\"}}\n\n";

static void test_retry_reset(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_drop_after_text;   /* text streamed, then dropped */
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_retry(a, 2);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);

    int rc = agentc_agent_submit(a, "go");
    check("retry_reset_rc", rc == 0);
    check("retry_reset_requests", r.i == 2);
    check("retry_reset_event",
          agentc_str_str((const char *)tr.tr.p, "msg_reset\n") != NULL);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool final_text = tp && tp->n == 2 && tp->msgs[1].nblocks == 1 &&
                      agentc_streq(tp->msgs[1].blocks[0].text, "all done");
    check("retry_reset_final_text", final_text);
    agentc_outs((const char *)tr.tr.p);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

static void test_http_error(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 401;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_retry(a, 2);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);

    int rc = agentc_agent_submit(a, "go");
    check("http_error_rc", rc == -1);
    const char *err = agentc_agent_last_error(a);
    check("http_error_msg", err && agentc_str_str(err, "401") != NULL);
    check("http_error_no_retry", r.i == 1);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

static void test_abort(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    abort_agent = a;

    int rc = agentc_agent_submit(a, "go");
    abort_agent = NULL;
    check("abort_rc", rc == -1);
    const char *err = agentc_agent_last_error(a);
    check("abort_msg", err && agentc_streq(err, "aborted"));
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("abort_msg_state", tp && tp->n == 2 && tp->msgs[1].stop_reason == AGENTC_STOP_ABORTED);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* --------------------------------------------- loop cases */

/* Two tool calls in one response: results and events keep source order. */
static const char reply_two_tools[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"toolu_1\",\"name\":\"read\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"a.txt\\\"}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"toolu_2\",\"name\":\"read\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"b.txt\\\"}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
    "\"usage\":{\"output_tokens\":6}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static void test_two_tools(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_two_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    check("two_tools_rc", rc == 0);
    check("two_tools_calls", stub_calls == 2);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool order = tp && tp->n == 5 && tp->msgs[2].role == AGENTC_ROLE_TOOL &&
                 tp->msgs[3].role == AGENTC_ROLE_TOOL &&
                 agentc_streq(tp->msgs[2].blocks[0].text, "stub({\"path\":\"a.txt\"})") &&
                 agentc_streq(tp->msgs[3].blocks[0].text, "stub({\"path\":\"b.txt\"})") &&
                 agentc_streq(tp->msgs[2].blocks[0].tool_id, "toolu_1") &&
                 agentc_streq(tp->msgs[3].blocks[0].tool_id, "toolu_2");
    check("two_tools_order", order);
    agentc_outs((const char *)tr.tr.p);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* The first tool aborts the turn; the unanswered call is backfilled with
 * "error: aborted" and emits no start/end events. */
static AgcAgent *kill_agent;

static char *kill_exec(const char *args_json, bool *is_error, const volatile bool *cancel) {
    (void)cancel;
    stub_calls++;
    if (is_error) *is_error = false;
    if (kill_agent) agentc_agent_abort(kill_agent);
    AgcBuf b = { 0 };
    agentc_buf_printf(&b, "kill(%s)", args_json ? args_json : "");
    return (char *)b.p;
}

static const AgcTool kill_tool = {
    .name = "read", .label = "Read", .desc = "kill read",
    .params_json = "{\"type\":\"object\"}", .flags = AGENTC_TOOL_READONLY,
    .exec = kill_exec,
};

static void test_abort_backfill(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_two_tools;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &kill_tool, 1);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    stub_calls = 0;
    kill_agent = a;

    int rc = agentc_agent_submit(a, "go");
    kill_agent = NULL;
    check("abort2_rc", rc == -1);
    check("abort2_calls", stub_calls == 1);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool backfill = tp && tp->n == 4 &&
                    agentc_streq(tp->msgs[2].blocks[0].text, "kill({\"path\":\"a.txt\"})") &&
                    agentc_streq(tp->msgs[3].blocks[0].text, "error: aborted") &&
                    tp->msgs[3].error != NULL;
    check("abort2_backfill", backfill);
    bool no_second_start = agentc_str_str((const char *)tr.tr.p,
                                          "tool_start:read:{\"path\":\"b.txt\"}") == NULL;
    check("abort2_no_start", no_second_start);
    agentc_outs((const char *)tr.tr.p);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* A vetoed call still gets start/end events and the old blocked text. */
static void always_veto(void *ud, const char *call_id, const char *tool_name,
                        const char *args, AgcToolVetoDecision *out) {
    (void)ud;
    (void)call_id;
    (void)tool_name;
    (void)args;
    out->block = 1;
    out->reason = agentc_strdup("denied for testing");
}

static void test_veto(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_two_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_set_tool_veto(a, always_veto, NULL);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    check("veto_rc", rc == 0);
    check("veto_no_exec", stub_calls == 0);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool text = tp && tp->n == 5 &&
                agentc_streq(tp->msgs[2].blocks[0].text,
                             "error: tool blocked: denied for testing") &&
                agentc_streq(tp->msgs[3].blocks[0].text,
                             "error: tool blocked: denied for testing") &&
                tp->msgs[2].error != NULL && tp->msgs[3].error != NULL;
    check("veto_text", text);
    agentc_outs((const char *)tr.tr.p);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* A veto that replaces the args instead of blocking: the stub tool must see
 * the rewritten JSON, and both calls must still run. */
static void rewrite_veto(void *ud, const char *call_id, const char *tool_name,
                         const char *args, AgcToolVetoDecision *out) {
    (void)ud;
    (void)call_id;
    (void)tool_name;
    (void)args;
    out->args_json = agentc_strdup("{\"path\":\"rewritten.txt\"}");
}

static void test_veto_rewrite(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_two_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_set_tool_veto(a, rewrite_veto, NULL);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    check("veto2_rc", rc == 0);
    check("veto2_exec", stub_calls == 2);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool rewritten = tp && tp->n == 5 &&
                     agentc_streq(tp->msgs[2].blocks[0].text,
                                  "stub({\"path\":\"rewritten.txt\"})") &&
                     agentc_streq(tp->msgs[3].blocks[0].text,
                                  "stub({\"path\":\"rewritten.txt\"})");
    check("veto2_args_replaced", rewritten);
    agentc_outs((const char *)tr.tr.p);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* pi-exact terminate fixtures. */
static void terminate_all_veto(void *ud, const char *call_id, const char *tool_name,
                               const char *args, AgcToolVetoDecision *out) {
    (void)ud;
    (void)call_id;
    (void)tool_name;
    (void)args;
    out->block = 1;
    out->terminate = 1;
    out->reason = agentc_strdup("terminate for testing");
}

static void terminate_first_veto(void *ud, const char *call_id, const char *tool_name,
                                 const char *args, AgcToolVetoDecision *out) {
    (void)ud;
    (void)tool_name;
    (void)args;
    if (!agentc_streq(call_id, "toolu_1")) return;
    out->block = 1;
    out->terminate = 1;
    out->reason = agentc_strdup("terminate for testing");
}

/* Block+terminate everything and cancel the run from the veto: abort must win
 * over terminate. */
static void terminate_cancel_veto(void *ud, const char *call_id, const char *tool_name,
                                  const char *args, AgcToolVetoDecision *out) {
    terminate_all_veto(NULL, call_id, tool_name, args, out);
    if (ud) agentc_agent_abort(ud);
}

/* Every call blocked with terminate: the submission settles after the batch,
 * with one request, no execution, and agent_end stop_reason still tool_use. */
static void test_terminate_all(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_two_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_set_tool_veto(a, terminate_all_veto, NULL);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    HookRec settled;
    agentc_memset(&settled, 0, sizeof settled);
    uint64_t sh = hook_rec_on("agent_settled", AGENTC_HOOK_OBSERVE, &settled);
    stub_calls = 0;
    g_agent_end_stop_seen = false;
    g_agent_end_stop = AGENTC_STOP_PENDING;

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool blocked = tp && tp->n == 4 && tp->msgs[1].stop_reason == AGENTC_STOP_TOOLUSE &&
                   tp->msgs[2].error != NULL && tp->msgs[3].error != NULL &&
                   agentc_streq(tp->msgs[2].blocks[0].text,
                                "error: tool blocked: terminate for testing") &&
                   agentc_streq(tp->msgs[3].blocks[0].text,
                                "error: tool blocked: terminate for testing");
    check("term_rc", rc == 0);
    check("term_one_request", r.i == 1);
    check("term_no_exec", stub_calls == 0);
    check("term_blocked_results", blocked);
    check("term_agent_end_tool_use",
          g_agent_end_stop_seen && g_agent_end_stop == AGENTC_STOP_TOOLUSE);
    check("term_settled_once", settled.calls == 1);
    agentc_outs((const char *)tr.tr.p);

    agentc_ext_host()->off(sh);
    hook_rec_clear(&settled);
    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* One terminating call mixed with a normal one never terminates the batch: the
 * normal call runs and the loop issues the next provider request. */
static void test_terminate_mixed(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_two_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_set_tool_veto(a, terminate_first_veto, NULL);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool mixed = tp && tp->n == 5 &&
                 agentc_streq(tp->msgs[2].blocks[0].text,
                              "error: tool blocked: terminate for testing") &&
                 agentc_streq(tp->msgs[3].blocks[0].text, "stub({\"path\":\"b.txt\"})") &&
                 agentc_streq(tp->msgs[4].blocks[0].text, "all done");
    check("term_mixed_rc", rc == 0);
    check("term_mixed_two_requests", r.i == 2);
    check("term_mixed_one_exec", stub_calls == 1);
    check("term_mixed_transcript", mixed);
    agentc_outs((const char *)tr.tr.p);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* turn_end.continue still wins over an all-terminate batch: one continuation
 * re-enters the loop under the shared cap. */
static void test_terminate_continue(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_two_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_set_tool_veto(a, terminate_all_veto, NULL);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"continue\":true}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("turn_end", AGENTC_HOOK_OVERRIDE, &h);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool final = tp && tp->n == 5 && tp->msgs[4].role == AGENTC_ROLE_ASSISTANT &&
                 agentc_streq(tp->msgs[4].blocks[0].text, "all done");
    check("term_cont_rc", rc == 0);
    check("term_cont_two_requests", r.i == 2);
    check("term_cont_hook_calls", h.calls == 2);
    check("term_cont_final", final);
    agentc_outs((const char *)tr.tr.p);

    agentc_ext_host()->off(hh);
    hook_rec_clear(&h);
    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* abort during the veto wins over terminate: no second request, rc -1. */
static void test_terminate_cancel(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_two_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_set_tool_veto(a, terminate_cancel_veto, a);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    check("term_cancel_rc", rc == -1);
    check("term_cancel_one_request", r.i == 1);
    check("term_cancel_error", agentc_streq(agentc_agent_last_error(a), "aborted"));
    check("term_cancel_event", count_occurrences((const char *)tr.tr.p, "error:aborted") == 1);
    agentc_outs((const char *)tr.tr.p);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* The direct tool_result payload is byte-identical to the old inline builder. */
static void test_ext_payload(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_test_no_backoff(a);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    uint64_t hh = hook_rec_on("tool_result", AGENTC_HOOK_OVERRIDE, &h);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    static const char expect[] =
        "{\"tool_call_id\":\"toolu_1\",\"tool_name\":\"read\",\"content\":[{\"type\":"
        "\"text\",\"text\":\"stub({\\\"path\\\":\\\"f.txt\\\"})\"}],\"is_error\":false}";
    check("payload_rc", rc == 0);
    check("payload_count", h.calls == 1);
    check("payload_json", buf_eq(&h.payload, expect));
    agentc_outf("payload_json=%s\n", h.payload.p ? (const char *)h.payload.p : "");

    agentc_ext_host()->off(hh);
    hook_rec_clear(&h);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* --------------------------------------------- closure cases */

/* A max_tokens stop that still carries a tool_use block (partial args) must not
 * execute: the call is closed with one synthetic error result, and the next
 * submit runs normally. */
static const char reply_trunc[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"toolu_1\",\"name\":\"read\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"f.txt\\\"\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"max_tokens\"},"
    "\"usage\":{\"output_tokens\":3}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static void test_trunc_call_closed(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_trunc;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    check("trunc_rc", rc == 0);
    check("trunc_no_exec", stub_calls == 0);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool kept = tp && tp->n == 3 && tp->msgs[1].role == AGENTC_ROLE_ASSISTANT &&
                tp->msgs[1].nblocks == 1 &&
                tp->msgs[1].blocks[0].type == AGENTC_BLK_TOOLCALL &&
                agentc_streq(tp->msgs[1].blocks[0].tool_id, "toolu_1") &&
                tp->msgs[1].stop_reason == AGENTC_STOP_LENGTH;
    check("trunc_call_kept", kept);
    bool closed = tp && tp->n == 3 && tp->msgs[2].role == AGENTC_ROLE_TOOL &&
                  tp->msgs[2].nblocks == 1 &&
                  agentc_streq(tp->msgs[2].blocks[0].text,
                               "error: response ended before this call could run") &&
                  agentc_streq(tp->msgs[2].blocks[0].tool_id, "toolu_1") &&
                  tp->msgs[2].error != NULL;
    check("trunc_closed", closed);
    check("trunc_no_start", agentc_str_str((const char *)tr.tr.p, "tool_start") == NULL);

    int rc2 = agentc_agent_submit(a, "again");
    check("trunc_resume_rc", rc2 == 0);
    check("trunc_requests", r.i == 2);
    agentc_outs((const char *)tr.tr.p);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* A stream that ends before message_stop leaves the call unanswered unless the
 * error path closes it; retries are exhausted with max_attempts = 1. */
static const char reply_partial[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"toolu_7\",\"name\":\"read\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"f.txt\\\"}\"}}\n\n";

/* A1 regression: a stream that announces a tool_use and then ends before any
 * stop marker (max_attempts = 1) must fail WITHOUT synthesizing a tool result.
 * The failed assistant is not serialized/persisted, so an orphan `tool` result
 * would poison the session on --continue. */
static void test_error_calls_closed(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_partial;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_set_retry(a, 1);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    OrphanWatch ow;
    agentc_memset(&ow, 0, sizeof ow);
    agentc_agent_set_observer(a, orphan_observer, &ow);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    check("error_closed_rc", rc == -1);
    check("error_closed_no_exec", stub_calls == 0);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool no_orphan = tp && tp->n == 2 && tp->msgs[1].role == AGENTC_ROLE_ASSISTANT &&
                     tp->msgs[1].stop_reason == AGENTC_STOP_ERROR;
    for (size_t i = 0; no_orphan && i < tp->n; i++)
        if (tp->msgs[i].role == AGENTC_ROLE_TOOL) no_orphan = false;
    check("error_closed_no_orphan", no_orphan);
    check("error_closed_observer_no_tool", ow.tool_msgs == 0);
    const char *err = agentc_agent_last_error(a);
    check("error_closed_msg", err && agentc_streq(err, "stream ended prematurely"));

    /* A following turn must not re-send the failed turn's tool_use: the failed
     * assistant is skipped by the provider, so the request has no tool_use (and
     * no orphan tool_result) even though it still holds the call in memory. */
    r.n = 2;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;
    int rc2 = agentc_agent_submit(a, "again");
    check("error_closed_next_ok", rc2 == 0);
    check("error_closed_next_no_tooluse",
          r.last_body.p != NULL &&
              agentc_str_str((const char *)r.last_body.p, "tool_use") == NULL);
    agentc_outs((const char *)tr.tr.p);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* An aborted stream that already produced a call must not synthesize a result
 * (the aborted assistant is not persisted); the next submit resets a->cancel at
 * entry and completes normally. */
static const char reply_call_then_text[] =
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
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"partial\"}}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":3}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static void test_abort_reset(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_call_then_text;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    stub_calls = 0;
    abort_agent = a;

    int rc = agentc_agent_submit(a, "go");
    abort_agent = NULL;
    check("abort_reset_rc", rc == -1);
    check("abort_reset_no_exec", stub_calls == 0);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool no_orphan = tp && tp->n == 2 && tp->msgs[1].role == AGENTC_ROLE_ASSISTANT &&
                     tp->msgs[1].stop_reason == AGENTC_STOP_ABORTED;
    for (size_t i = 0; no_orphan && i < tp->n; i++)
        if (tp->msgs[i].role == AGENTC_ROLE_TOOL) no_orphan = false;
    check("abort_reset_no_orphan", no_orphan);
    check("abort_reset_no_start", agentc_str_str((const char *)tr.tr.p, "tool_start") == NULL);

    int rc2 = agentc_agent_submit(a, "again");
    check("abort_reset_rc2", rc2 == 0);
    check("abort_reset_requests", r.i == 2);
    agentc_outs((const char *)tr.tr.p);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* The agent owns a deep copy of the tool table, so a registry may free
 * its source strings and array immediately after installing them. */
static void test_tool_copy_ownership(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_test_no_backoff(a);
    stub_calls = 0;

    AgcTool *src = agentc_alloc(sizeof *src);
    src[0] = stub_tool;
    src[0].name = agentc_strdup("read");
    src[0].label = agentc_strdup("Read");
    src[0].desc = agentc_strdup("stub read");
    src[0].params_json = agentc_strdup("{\"type\":\"object\"}");
    agentc_agent_set_tools(a, src, 1);
    agentc_free((char *)src[0].name);
    agentc_free((char *)src[0].label);
    agentc_free((char *)src[0].desc);
    agentc_free((char *)src[0].params_json);
    agentc_free(src);

    /* Clearing and repeated replacement stay safe and consistent. */
    agentc_agent_set_tools(a, NULL, 0);
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_set_tools(a, &stub_tool, 1);

    int rc = agentc_agent_submit(a, "go");
    check("tool_copy_rc", rc == 0);
    check("tool_copy_exec", stub_calls == 1);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    const char *sys = tp ? tp->system : NULL;
    check("tool_copy_prompt", sys && agentc_str_str(sys, "- read: stub read") != NULL);

    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* A hidden tool runs but is not named in the composed # Tools section. */
static void test_hidden_tool_prompt(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    /* no set_system: submit composes the prompt from the tool table */
    AgcTool tools[2];
    tools[0] = stub_tool;
    tools[1] = hidden_tool;
    agentc_agent_set_tools(a, tools, 2);
    agentc_agent_test_no_backoff(a);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    const char *sys = tp ? tp->system : NULL;
    check("hidden_prompt_rc", rc == 0);
    check("hidden_prompt_visible", sys && agentc_str_str(sys, "- read: stub read") != NULL);
    check("hidden_prompt_secret_absent", sys && agentc_str_str(sys, "secret") == NULL);

    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* A late tool added to the registry must reach the agent's installed table at
 * the turn boundary, so the provider request after the tool batch carries it. */
static void test_recompose_late_tool(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_set_recompose(a, test_recompose_cb, NULL);
    agentc_agent_test_no_backoff(a);
    stub_calls = 0;
    g_recompose_calls = 0;

    check("recompose_add", agentc_ext_add_tool_internal(&late_tool) == 0);
    check("recompose_dirty", agentc_ext_dirty());
    int rc = agentc_agent_submit(a, "go");
    check("recompose_rc", rc == 0);
    check("recompose_tool_calls", stub_calls == 1);
    check("recompose_called", g_recompose_calls == 1);
    check("recompose_clean", !agentc_ext_dirty());
    check("recompose_next_request",
          r.last_body.p != NULL &&
              agentc_str_str((const char *)r.last_body.p, "late_tool") != NULL);

    check("recompose_remove", agentc_ext_remove_tool_internal("late_tool") == 0);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* B: a recompose that changes the tool table also refreshes the built system
 * prompt for the next provider request; an explicit system prompt and a
 * before_agent_start systemPrompt override are not clobbered by the refresh. */
static void test_recompose_prompt_refresh(void) {
    /* auto: turn 2's system prompt carries the late tool, turn 1's does not */
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;
    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    /* no set_system: the built prompt is refreshed per turn */
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_set_recompose(a, test_recompose_cb, NULL);
    agentc_agent_test_no_backoff(a);
    HookRec reqs;
    agentc_memset(&reqs, 0, sizeof reqs);
    uint64_t rh = hook_rec_on("before_provider_request", AGENTC_HOOK_OVERRIDE, &reqs);

    check("refresh_add", agentc_ext_add_tool_internal(&late_tool) == 0);
    g_recompose_calls = 0;
    check("refresh_submit", agentc_agent_submit(a, "go") == 0);
    check("refresh_called", g_recompose_calls == 1);
    check("refresh_request_count", reqs.calls == 2);
    check("refresh_first_prompt",
          !buf_has(&reqs.first_payload, "- late_tool: registered after startup"));
    check("refresh_second_prompt",
          buf_has(&reqs.payload, "- late_tool: registered after startup"));
    agentc_ext_host()->off(rh);
    hook_rec_clear(&reqs);
    agentc_buf_free(&r.last_body);
    check("refresh_remove", agentc_ext_remove_tool_internal("late_tool") == 0);
    agentc_agent_free(a);

    /* explicit --system stays fixed even after the recompose */
    Replay r2;
    agentc_memset(&r2, 0, sizeof r2);
    r2.n = 2;
    r2.codes[0] = 0;
    r2.bodies[0] = reply_tools;
    r2.codes[1] = 0;
    r2.bodies[1] = reply_final;
    AgcAgent *b = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(b, (AgcTransport){ replay_request, &r2, NULL });
    agentc_agent_set_system(b, "sys");
    agentc_agent_set_tools(b, &stub_tool, 1);
    agentc_agent_set_recompose(b, test_recompose_cb, NULL);
    agentc_agent_test_no_backoff(b);
    HookRec reqs2;
    agentc_memset(&reqs2, 0, sizeof reqs2);
    uint64_t rh2 = hook_rec_on("before_provider_request", AGENTC_HOOK_OVERRIDE, &reqs2);
    check("refresh_explicit_add", agentc_ext_add_tool_internal(&late_tool) == 0);
    check("refresh_explicit_submit", agentc_agent_submit(b, "go") == 0);
    check("refresh_explicit_requests", reqs2.calls == 2);
    check("refresh_explicit_prompt",
          buf_has(&reqs2.payload, "\"text\":\"sys\"") &&
              !buf_has(&reqs2.payload, "- late_tool: registered after startup"));
    agentc_ext_host()->off(rh2);
    hook_rec_clear(&reqs2);
    agentc_buf_free(&r2.last_body);
    check("refresh_explicit_remove", agentc_ext_remove_tool_internal("late_tool") == 0);
    agentc_agent_free(b);

    /* a before_agent_start override wins for the rest of the run over the
     * per-turn rebuild */
    Replay r3;
    agentc_memset(&r3, 0, sizeof r3);
    r3.n = 2;
    r3.codes[0] = 0;
    r3.bodies[0] = reply_tools;
    r3.codes[1] = 0;
    r3.bodies[1] = reply_final;
    AgcAgent *c = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(c, (AgcTransport){ replay_request, &r3, NULL });
    agentc_agent_set_tools(c, &stub_tool, 1);
    agentc_agent_set_recompose(c, test_recompose_cb, NULL);
    agentc_agent_test_no_backoff(c);
    HookRec bas;
    agentc_memset(&bas, 0, sizeof bas);
    bas.result = "{\"systemPrompt\":\"SYS-EXT\"}";
    bas.result_calls = 1;
    uint64_t bh = hook_rec_on("before_agent_start", AGENTC_HOOK_OVERRIDE, &bas);
    HookRec reqs3;
    agentc_memset(&reqs3, 0, sizeof reqs3);
    uint64_t rh3 = hook_rec_on("before_provider_request", AGENTC_HOOK_OVERRIDE, &reqs3);
    check("refresh_override_add", agentc_ext_add_tool_internal(&late_tool) == 0);
    check("refresh_override_submit", agentc_agent_submit(c, "go") == 0);
    check("refresh_override_requests", reqs3.calls == 2);
    check("refresh_override_second",
          buf_has(&reqs3.payload, "SYS-EXT") &&
              !buf_has(&reqs3.payload, "- late_tool: registered after startup"));
    const AgcTranscript *tp = agentc_agent_transcript(c);
    const AgcExtHost *host = agentc_ext_host();
    check("refresh_override_transcript", tp && tp->system && agentc_streq(tp->system, "SYS-EXT"));
    check("refresh_override_context",
          host->system_prompt(host) && agentc_streq(host->system_prompt(host), "SYS-EXT"));
    agentc_ext_host()->off(bh);
    agentc_ext_host()->off(rh3);
    hook_rec_clear(&bas);
    hook_rec_clear(&reqs3);
    agentc_buf_free(&r3.last_body);
    check("refresh_override_remove", agentc_ext_remove_tool_internal("late_tool") == 0);
    agentc_agent_free(c);
}

/* ------------------------------------------------------------ overrides */

/* input transform: the effective user text reaches the transcript and request. */
static void test_input_transform(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"action\":\"transform\",\"text\":\"transformed\"}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("input", AGENTC_HOOK_OVERRIDE, &h);
    HookRec settled;
    agentc_memset(&settled, 0, sizeof settled);
    uint64_t sh = hook_rec_on("agent_settled", AGENTC_HOOK_OBSERVE, &settled);

    int rc = agentc_agent_submit(a, "original");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("input_tf_rc", rc == 0);
    check("input_tf_payload",
          buf_eq(&h.first_payload, "{\"text\":\"original\",\"source\":\"user\"}"));
    check("input_tf_transcript",
          tp && tp->n == 2 && tp->msgs[0].role == AGENTC_ROLE_USER &&
              agentc_streq(tp->msgs[0].blocks[0].text, "transformed"));
    check("input_tf_request",
          r.last_body.p != NULL &&
              agentc_str_str((const char *)r.last_body.p, "transformed") != NULL &&
              agentc_str_str((const char *)r.last_body.p, "original") == NULL);
    check("input_tf_events", count_occurrences((const char *)tr.tr.p, "agent_start\n") == 1 &&
                                 count_occurrences((const char *)tr.tr.p, "agent_end\n") == 1);
    check("input_tf_settled", settled.calls == 1);

    agentc_ext_host()->off(hh);
    agentc_ext_host()->off(sh);
    hook_rec_clear(&h);
    hook_rec_clear(&settled);
    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* input handled (action) and input blocked (fail-closed handler): no run; the
 * block is a visible error with a negative submit result, not a quiet no-run. */
static void test_input_handled(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"action\":\"handled\"}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("input", AGENTC_HOOK_OVERRIDE, &h);
    HookRec settled;
    agentc_memset(&settled, 0, sizeof settled);
    uint64_t sh = hook_rec_on("agent_settled", AGENTC_HOOK_OBSERVE, &settled);

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("input_handled_rc", rc == 0);
    check("input_handled_no_request", r.i == 0);
    check("input_handled_no_events", tr.tr.len == 0);
    check("input_handled_no_msgs", tp && tp->n == 0);
    check("input_handled_no_settle", settled.calls == 0);
    check("input_handled_payload",
          buf_eq(&h.first_payload, "{\"text\":\"go\",\"source\":\"user\"}"));
    agentc_ext_host()->off(hh);
    agentc_ext_host()->off(sh);
    hook_rec_clear(&h);
    hook_rec_clear(&settled);
    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);

    /* a failed (fail-closed) input handler blocks the run the same way */
    Replay r2;
    agentc_memset(&r2, 0, sizeof r2);
    r2.n = 1;
    r2.codes[0] = 0;
    r2.bodies[0] = reply_final;
    AgcTransport t2 = { replay_request, &r2, NULL };
    AgcAgent *a2 = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a2, t2);
    agentc_agent_set_system(a2, "sys");
    agentc_agent_test_no_backoff(a2);
    Trace tr2;
    agentc_memset(&tr2, 0, sizeof tr2);
    agentc_agent_set_events(a2, collector, &tr2);
    uint64_t fh = agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 0, fail_hook_fn, NULL);

    int rc2 = agentc_agent_submit(a2, "go");
    check("input_blocked_rc", rc2 == -5);
    check("input_blocked_no_request", r2.i == 0);
    check("input_blocked_error",
          agentc_streq(agentc_agent_last_error(a2), "input blocked by extension"));
    check("input_blocked_error_event",
          agentc_streq((const char *)tr2.tr.p, "error:input blocked by extension\n"));
    agentc_ext_host()->off(fh);
    agentc_buf_free(&tr2.tr);
    agentc_buf_free(&r2.last_body);
    agentc_agent_free(a2);
}

/* before_agent_start: a systemPrompt replacement reaches the request body and
 * the extension context; message is logged and ignored. */
static void test_before_agent_start(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"systemPrompt\":\"SYS-EXT\",\"message\":\"hello from ext\"}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("before_agent_start", AGENTC_HOOK_OVERRIDE, &h);

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("bas_rc", rc == 0);
    check("bas_payload",
          buf_eq(&h.first_payload, "{\"prompt\":\"go\",\"system_prompt\":\"sys\"}"));
    check("bas_transcript", tp && tp->system && agentc_streq(tp->system, "SYS-EXT"));
    check("bas_request", r.last_body.p != NULL &&
                             agentc_str_str((const char *)r.last_body.p, "SYS-EXT") != NULL);
    const AgcExtHost *host = agentc_ext_host();
    check("bas_context", host->system_prompt(host) &&
                             agentc_streq(host->system_prompt(host), "SYS-EXT"));

    agentc_ext_host()->off(hh);
    hook_rec_clear(&h);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* message_end replacement runs before the typed event and the observer. */
static void test_message_end_replace(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\","
               "\"text\":\"hooked\"},{\"type\":\"thinking\",\"text\":\"why\"}],"
               "\"stop_reason\":\"length\",\"error\":null}}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("message_end", AGENTC_HOOK_OVERRIDE, &h);
    g_msg_end_hook = &h;
    g_msg_end_hook_seen = -1;
    g_msg_obs_hook_seen = -1;
    g_obs_calls = 0;
    agentc_buf_clear(&g_obs_text);
    agentc_agent_set_observer(a, msg_order_observer, NULL);

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool blocks = tp && tp->n == 2 && tp->msgs[1].nblocks == 2 &&
                  tp->msgs[1].blocks[0].type == AGENTC_BLK_TEXT &&
                  agentc_streq(tp->msgs[1].blocks[0].text, "hooked") &&
                  tp->msgs[1].blocks[1].type == AGENTC_BLK_THINK &&
                  agentc_streq(tp->msgs[1].blocks[1].text, "why");
    check("msg_end_rc", rc == 0);
    check("msg_end_blocks", blocks);
    check("msg_end_stop", tp && tp->msgs[1].stop_reason == AGENTC_STOP_LENGTH);
    check("msg_end_hook_before_typed", g_msg_end_hook_seen == 1);
    check("msg_end_hook_before_observer", g_msg_obs_hook_seen == 1);
    check("msg_end_observer_text",
          g_obs_calls == 1 && buf_eq(&g_obs_text, "hooked"));
    check("msg_end_payload", buf_has(&h.first_payload, "\"role\":\"assistant\"") &&
                                 buf_has(&h.first_payload, "\"stop_reason\":\"stop\"") &&
                                 buf_has(&h.first_payload, "\"cost_micro\":"));

    agentc_ext_host()->off(hh);
    g_msg_end_hook = NULL;
    hook_rec_clear(&h);
    agentc_buf_free(&tr.tr);
    agentc_buf_free(&g_obs_text);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* One strict-content case: the whole replacement is rejected and the original
 * message (text + stop) stays untouched. */
static void msg_end_invalid_content_case(const char *name, const char *result) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;
    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = result;
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("message_end", AGENTC_HOOK_OVERRIDE, &h);
    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    char label[96];
    agentc_snprintf(label, sizeof label, "msg_end_%s_kept", name);
    check(label, rc == 0 && tp && tp->n == 2 && tp->msgs[1].nblocks == 1 &&
                   tp->msgs[1].blocks[0].type == AGENTC_BLK_TEXT &&
                   agentc_streq(tp->msgs[1].blocks[0].text, "all done"));
    agentc_snprintf(label, sizeof label, "msg_end_%s_stop", name);
    check(label, tp && tp->msgs[1].stop_reason == AGENTC_STOP_STOP);
    agentc_ext_host()->off(hh);
    hook_rec_clear(&h);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* A role mismatch or malformed content leaves the original message untouched. */
static void test_message_end_invalid(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;
    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result =
        "{\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"nope\"}]}}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("message_end", AGENTC_HOOK_OVERRIDE, &h);
    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("msg_end_badrole_rc", rc == 0);
    check("msg_end_badrole_kept",
          tp && tp->n == 2 && tp->msgs[1].nblocks == 1 &&
              agentc_streq(tp->msgs[1].blocks[0].text, "all done") &&
              tp->msgs[1].stop_reason == AGENTC_STOP_STOP);
    agentc_ext_host()->off(hh);
    hook_rec_clear(&h);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);

    Replay r2;
    agentc_memset(&r2, 0, sizeof r2);
    r2.n = 1;
    r2.codes[0] = 0;
    r2.bodies[0] = reply_final;
    AgcTransport t2 = { replay_request, &r2, NULL };
    AgcAgent *a2 = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a2, t2);
    agentc_agent_set_system(a2, "sys");
    agentc_agent_test_no_backoff(a2);
    HookRec h2;
    agentc_memset(&h2, 0, sizeof h2);
    h2.result = "{\"message\":{\"content\":[{\"type\":\"bogus\"}]}}";
    h2.result_calls = 1;
    uint64_t hh2 = hook_rec_on("message_end", AGENTC_HOOK_OVERRIDE, &h2);
    int rc2 = agentc_agent_submit(a2, "go");
    const AgcTranscript *tp2 = agentc_agent_transcript(a2);
    check("msg_end_badcontent_rc", rc2 == 0);
    check("msg_end_badcontent_kept",
          tp2 && tp2->n == 2 && tp2->msgs[1].nblocks == 1 &&
              tp2->msgs[1].blocks[0].type == AGENTC_BLK_TEXT &&
              agentc_streq(tp2->msgs[1].blocks[0].text, "all done"));
    agentc_ext_host()->off(hh2);
    hook_rec_clear(&h2);
    agentc_buf_free(&r2.last_body);
    agentc_agent_free(a2);

    /* A tool_call entry needs a non-empty string id and name, and
     * `arguments` must be absent/string/object/null; each violation rejects the
     * whole content replacement so the original blocks are kept. */
    msg_end_invalid_content_case(
        "toolcall_noid",
        "{\"message\":{\"content\":[{\"type\":\"tool_call\",\"name\":\"read\","
        "\"arguments\":{}}],\"stop_reason\":\"tool_use\"}}");
    msg_end_invalid_content_case(
        "toolcall_noname",
        "{\"message\":{\"content\":[{\"type\":\"tool_call\",\"id\":\"toolu_1\","
        "\"arguments\":{}}],\"stop_reason\":\"tool_use\"}}");
    msg_end_invalid_content_case(
        "toolcall_badargs",
        "{\"message\":{\"content\":[{\"type\":\"tool_call\",\"id\":\"toolu_1\","
        "\"name\":\"read\",\"arguments\":42}],\"stop_reason\":\"tool_use\"}}");
}

/* A message_end replacement cannot rescue a failed turn. Content is
 * replaceable, but stop_reason/error stay terminal, so the message is neither
 * persisted nor serialized. */
static void test_message_end_terminal(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 401;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_retry(a, 1);
    agentc_agent_test_no_backoff(a);
    PersistWatch pw;
    agentc_memset(&pw, 0, sizeof pw);
    agentc_agent_set_observer(a, persist_observer, &pw);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"message\":{\"content\":[{\"type\":\"text\",\"text\":"
               "\"hooked failure\"}],\"stop_reason\":\"stop\",\"error\":null}}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("message_end", AGENTC_HOOK_OVERRIDE, &h);

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    const AgcMsg *m = tp && tp->n == 2 ? &tp->msgs[1] : NULL;
    check("msg_end_term_rc", rc == -1);
    check("msg_end_term_content",
          m && m->nblocks == 1 && m->blocks[0].type == AGENTC_BLK_TEXT &&
              agentc_streq(m->blocks[0].text, "hooked failure"));
    check("msg_end_term_stop", m && m->stop_reason == AGENTC_STOP_ERROR);
    check("msg_end_term_error", m && m->error != NULL && m->error[0] != 0);
    check("msg_end_term_not_serializable", m && !agentc_provider_msg_serializable(m));
    check("msg_end_term_not_persistable", m && !agentc_mode_msg_persistable(m));
    check("msg_end_term_not_persisted", pw.persistable == 0);

    agentc_ext_host()->off(hh);
    hook_rec_clear(&h);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* The same guard covers an aborted turn: the hook may replace the content but
 * cannot flip ABORTED to a final stop, so nothing is persisted/serialized. */
static void test_message_end_terminal_aborted(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    PersistWatch pw;
    agentc_memset(&pw, 0, sizeof pw);
    agentc_agent_set_observer(a, persist_observer, &pw);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"message\":{\"content\":[{\"type\":\"text\",\"text\":"
               "\"hooked abort\"}],\"stop_reason\":\"stop\",\"error\":null}}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("message_end", AGENTC_HOOK_OVERRIDE, &h);
    abort_agent = a;

    int rc = agentc_agent_submit(a, "go");
    abort_agent = NULL;
    const AgcTranscript *tp = agentc_agent_transcript(a);
    const AgcMsg *m = tp && tp->n == 2 ? &tp->msgs[1] : NULL;
    check("msg_end_abort_rc", rc == -1);
    check("msg_end_abort_content",
          m && m->nblocks == 1 && m->blocks[0].type == AGENTC_BLK_TEXT &&
              agentc_streq(m->blocks[0].text, "hooked abort"));
    check("msg_end_abort_stop", m && m->stop_reason == AGENTC_STOP_ABORTED);
    check("msg_end_abort_error", m && m->error != NULL && agentc_streq(m->error, "aborted"));
    check("msg_end_abort_not_serializable", m && !agentc_provider_msg_serializable(m));
    check("msg_end_abort_not_persistable", m && !agentc_mode_msg_persistable(m));
    check("msg_end_abort_not_persisted", pw.persistable == 0);

    agentc_ext_host()->off(hh);
    hook_rec_clear(&h);
    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* A message_end replacement may rebuild a tool_call block; the rebuilt call is
 * then resolved and executed like a provider one. */
static void test_message_end_tool_call(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_test_no_backoff(a);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"message\":{\"content\":[{\"type\":\"tool_call\",\"id\":\"toolu_9\","
               "\"name\":\"read\",\"arguments\":{\"path\":\"z.txt\"}}],"
               "\"stop_reason\":\"tool_use\"}}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("message_end", AGENTC_HOOK_OVERRIDE, &h);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool rebuilt = tp && tp->n == 4 && tp->msgs[1].nblocks == 1 &&
                   tp->msgs[1].blocks[0].type == AGENTC_BLK_TOOLCALL &&
                   agentc_streq(tp->msgs[1].blocks[0].tool_id, "toolu_9") &&
                   agentc_streq(tp->msgs[1].blocks[0].tool_name, "read") &&
                   agentc_streq(tp->msgs[1].blocks[0].tool_args, "{\"path\":\"z.txt\"}") &&
                   tp->msgs[1].stop_reason == AGENTC_STOP_TOOLUSE;
    check("msg_end_toolcall_rc", rc == 0);
    check("msg_end_toolcall_rebuilt", rebuilt);
    check("msg_end_toolcall_exec", stub_calls == 1);
    check("msg_end_toolcall_result",
          tp && tp->n == 4 && tp->msgs[2].role == AGENTC_ROLE_TOOL &&
              agentc_streq(tp->msgs[2].blocks[0].tool_id, "toolu_9") &&
              agentc_streq(tp->msgs[2].blocks[0].text, "stub({\"path\":\"z.txt\"})"));

    agentc_ext_host()->off(hh);
    hook_rec_clear(&h);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* tool_result result shapes: string, text-array, null/empty, is_error, and an
 * ignored shape. */
static void tool_result_case(const char *name, const char *hook_result,
                             const char *expect_text, bool expect_error) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_test_no_backoff(a);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = hook_result;
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("tool_result", AGENTC_HOOK_OVERRIDE, &h);

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    bool text_ok = rc == 0 && tp && tp->n == 4 && tp->msgs[2].role == AGENTC_ROLE_TOOL &&
                   tp->msgs[2].nblocks == 1 && tp->msgs[2].blocks[0].text &&
                   agentc_streq(tp->msgs[2].blocks[0].text, expect_text);
    bool err_ok = tp && ((tp->msgs[2].error != NULL) == expect_error);
    char label[64];
    agentc_snprintf(label, sizeof label, "tool_result_%s", name);
    check(label, text_ok && err_ok);

    agentc_ext_host()->off(hh);
    hook_rec_clear(&h);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

static void test_tool_result_override(void) {
    tool_result_case("string", "{\"content\":\"REPLACED\"}", "REPLACED", false);
    tool_result_case("array",
                     "{\"content\":[{\"type\":\"text\",\"text\":\"a\"},{\"type\":"
                     "\"text\",\"text\":\"b\"}]}",
                     "a\nb", false);
    tool_result_case("null", "{\"content\":null}", "", false);
    tool_result_case("empty", "{\"content\":[]}", "", false);
    tool_result_case("is_error", "{\"is_error\":true}",
                     "stub({\"path\":\"f.txt\"})", true);
    tool_result_case("shape", "{\"content\":42}",
                     "stub({\"path\":\"f.txt\"})", false);
}

/* The hook runs before the tool message is appended to the transcript. */
static AgcAgent *g_order_agent;
static int g_order_tool_msgs;
static int g_order_hook_calls;

static int order_hook_fn(void *ud, const char *point, const char *payload_json,
                         char **result_json) {
    (void)ud;
    (void)point;
    (void)payload_json;
    if (result_json) *result_json = NULL;
    g_order_hook_calls++;
    if (g_order_hook_calls == 1 && g_order_agent) {
        const AgcTranscript *tp = agentc_agent_transcript(g_order_agent);
        g_order_tool_msgs = 0;
        if (tp)
            for (size_t i = 0; i < tp->n; i++)
                if (tp->msgs[i].role == AGENTC_ROLE_TOOL) g_order_tool_msgs++;
    }
    return 0;
}

static void test_tool_result_order(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_test_no_backoff(a);
    uint64_t hh = agentc_ext_host()->on("tool_result", AGENTC_HOOK_OVERRIDE, 0,
                                        order_hook_fn, NULL);
    g_order_agent = a;
    g_order_tool_msgs = -1;
    g_order_hook_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("tool_result_order_rc", rc == 0);
    check("tool_result_order_before_append", g_order_tool_msgs == 0);
    check("tool_result_order_appended",
          tp && tp->n == 4 && tp->msgs[2].role == AGENTC_ROLE_TOOL);

    g_order_agent = NULL;
    agentc_ext_host()->off(hh);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* turn_end entries + continue: one extra request, no second agent_start. */
static void test_turn_end_continue(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    EntrySink es;
    agentc_memset(&es, 0, sizeof es);
    agentc_ext_set_entry_sink(test_entry_sink, &es);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"entries\":[{\"custom_type\":\"note\",\"data\":{\"x\":1}}],"
               "\"continue\":true}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("turn_end", AGENTC_HOOK_OVERRIDE, &h);

    int rc = agentc_agent_submit(a, "go");
    static const char expect_trace[] = "agent_start\n"
                                       "turn_start\nmsg_start\ntext:all done\nmsg_end:2:1\n"
                                       "turn_end\n"
                                       "turn_start\nmsg_start\ntext:all done\nmsg_end:2:1\n"
                                       "turn_end\n"
                                       "agent_end\n";
    check("turn_end_rc", rc == 0);
    check("turn_end_extra_request", r.i == 2);
    check("turn_end_once_per_turn", h.calls == 2);
    check("turn_end_first_index", buf_has(&h.first_payload, "\"turn_index\":0") &&
                                      buf_has(&h.first_payload, "\"stop_reason\":\"stop\""));
    check("turn_end_second_index", buf_has(&h.payload, "\"turn_index\":1"));
    check("turn_end_entries",
          es.calls == 1 && buf_eq(&es.log, "note:{\"x\":1}\n"));
    check("turn_end_trace", agentc_streq((const char *)tr.tr.p, expect_trace));

    agentc_ext_host()->off(hh);
    agentc_ext_set_entry_sink(NULL, NULL);
    hook_rec_clear(&h);
    agentc_buf_free(&es.log);
    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* Entries apply on a hard exit; continue does not. */
static void test_turn_end_entries_hard(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 401;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_retry(a, 1);
    agentc_agent_test_no_backoff(a);
    EntrySink es;
    agentc_memset(&es, 0, sizeof es);
    agentc_ext_set_entry_sink(test_entry_sink, &es);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"entries\":[{\"custom_type\":\"hard\",\"data\":{\"e\":true}}],"
               "\"continue\":true}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("turn_end", AGENTC_HOOK_OVERRIDE, &h);

    int rc = agentc_agent_submit(a, "go");
    check("turn_end_hard_rc", rc == -1);
    check("turn_end_hard_entries",
          es.calls == 1 && buf_eq(&es.log, "hard:{\"e\":true}\n"));
    check("turn_end_hard_no_continue", r.i == 1);

    agentc_ext_host()->off(hh);
    agentc_ext_set_entry_sink(NULL, NULL);
    hook_rec_clear(&h);
    agentc_buf_free(&es.log);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* agent_before_settle continue re-enters the turn loop; agent_settled fires
 * once, after the final agent_end. */
static void test_settle_continue(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"continue\":true}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("agent_before_settle", AGENTC_HOOK_OVERRIDE, &h);
    HookRec settled;
    agentc_memset(&settled, 0, sizeof settled);
    uint64_t sh = hook_rec_on("agent_settled", AGENTC_HOOK_OBSERVE, &settled);
    g_settled_hook = &settled;
    g_settle_seen_at_agent_end = -1;

    int rc = agentc_agent_submit(a, "go");
    static const char expect_trace[] = "agent_start\n"
                                       "turn_start\nmsg_start\ntext:all done\nmsg_end:2:1\nturn_end\n"
                                       "agent_end\n"
                                       "turn_start\nmsg_start\ntext:all done\nmsg_end:2:1\nturn_end\n"
                                       "agent_end\n";
    check("settle_cont_rc", rc == 0);
    check("settle_cont_requests", r.i == 2);
    check("settle_cont_one_agent_start",
          count_occurrences((const char *)tr.tr.p, "agent_start\n") == 1);
    check("settle_cont_two_agent_end",
          count_occurrences((const char *)tr.tr.p, "agent_end\n") == 2);
    check("settle_cont_settled_once", settled.calls == 1);
    check("settle_cont_after_agent_end", g_settle_seen_at_agent_end == 0);
    check("settle_cont_trace", agentc_streq((const char *)tr.tr.p, expect_trace));
    check("settle_cont_payload", buf_has(&h.first_payload, "\"outcome\":\"completed\""));

    agentc_ext_host()->off(hh);
    agentc_ext_host()->off(sh);
    g_settled_hook = NULL;
    hook_rec_clear(&h);
    hook_rec_clear(&settled);
    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* The shared continuation cap stops a runaway before_settle continue at 32. */
static void test_settle_cap(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;
    r.repeat_last = true;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"continue\":true}";
    h.result_calls = 1000;
    uint64_t hh = hook_rec_on("agent_before_settle", AGENTC_HOOK_OVERRIDE, &h);
    HookRec settled;
    agentc_memset(&settled, 0, sizeof settled);
    uint64_t sh = hook_rec_on("agent_settled", AGENTC_HOOK_OBSERVE, &settled);

    int rc = agentc_agent_submit(a, "go");
    check("cap_rc", rc == 0);
    check("cap_requests", r.requests == 33);
    check("cap_settle_calls", h.calls == 33);
    check("cap_settled_once", settled.calls == 1);

    agentc_ext_host()->off(hh);
    agentc_ext_host()->off(sh);
    hook_rec_clear(&h);
    hook_rec_clear(&settled);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* agent_settled fires exactly once per run: success, error, and never on the
 * input:handled path. */
static void test_agent_settled_once(void) {
    HookRec settled;
    agentc_memset(&settled, 0, sizeof settled);
    uint64_t sh = hook_rec_on("agent_settled", AGENTC_HOOK_OBSERVE, &settled);

    Replay ok;
    agentc_memset(&ok, 0, sizeof ok);
    ok.n = 1;
    ok.codes[0] = 0;
    ok.bodies[0] = reply_final;
    AgcTransport tok = { replay_request, &ok, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, tok);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    int rc = agentc_agent_submit(a, "go");
    check("settled_success_rc", rc == 0);
    check("settled_success_once", settled.calls == 1);
    agentc_buf_free(&ok.last_body);
    agentc_agent_free(a);

    Replay bad;
    agentc_memset(&bad, 0, sizeof bad);
    bad.n = 1;
    bad.codes[0] = 401;
    AgcTransport tbad = { replay_request, &bad, NULL };
    AgcAgent *b = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(b, tbad);
    agentc_agent_set_system(b, "sys");
    agentc_agent_set_retry(b, 1);
    agentc_agent_test_no_backoff(b);
    HookRec shk;
    agentc_memset(&shk, 0, sizeof shk);
    uint64_t shh = hook_rec_on("agent_before_settle", AGENTC_HOOK_OVERRIDE, &shk);
    int rc2 = agentc_agent_submit(b, "go");
    check("settled_error_rc", rc2 == -1);
    check("settled_error_once", settled.calls == 2);
    check("settled_error_outcome", buf_has(&shk.first_payload, "\"outcome\":\"error\""));
    agentc_ext_host()->off(shh);
    hook_rec_clear(&shk);
    agentc_buf_free(&bad.last_body);
    agentc_agent_free(b);

    Replay skip;
    agentc_memset(&skip, 0, sizeof skip);
    skip.n = 1;
    skip.codes[0] = 0;
    skip.bodies[0] = reply_final;
    AgcTransport tskip = { replay_request, &skip, NULL };
    AgcAgent *c = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(c, tskip);
    agentc_agent_set_system(c, "sys");
    agentc_agent_test_no_backoff(c);
    HookRec ih;
    agentc_memset(&ih, 0, sizeof ih);
    ih.result = "{\"action\":\"handled\"}";
    ih.result_calls = 1;
    uint64_t ihh = hook_rec_on("input", AGENTC_HOOK_OVERRIDE, &ih);
    int rc3 = agentc_agent_submit(c, "go");
    check("settled_handled_rc", rc3 == 0);
    check("settled_handled_none", settled.calls == 2);
    agentc_ext_host()->off(ihh);
    hook_rec_clear(&ih);
    agentc_buf_free(&skip.last_body);
    agentc_agent_free(c);

    agentc_ext_host()->off(sh);
    hook_rec_clear(&settled);
}

/* ------------------------------------------------------ provider hooks */

/* before_provider_headers: a flat patch reaches the wire, including a brand-new
 * header; content-length/host/transfer-encoding from the extension never do. */
static void test_provider_headers_patch(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;
    r.capture_headers = true;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"X-Ext\":\"1\",\"Content-Length\":\"999\",\"Host\":\"evil\","
               "\"Transfer-Encoding\":\"chunked\"}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("before_provider_headers", AGENTC_HOOK_OVERRIDE, &h);

    int rc = agentc_agent_submit(a, "go");
    check("hdrx_rc", rc == 0);
    check("hdrx_patched_on_wire",
          r.last_headers.p &&
              agentc_str_str((const char *)r.last_headers.p, "X-Ext: 1\r\n") != NULL);
    check("hdrx_original_kept",
          r.last_headers.p &&
              agentc_str_str((const char *)r.last_headers.p,
                             "anthropic-version: 2023-06-01") != NULL);
    check("hdrx_forbidden_absent",
          r.last_headers.p &&
              agentc_str_str((const char *)r.last_headers.p, "Content-Length") == NULL &&
              agentc_str_str((const char *)r.last_headers.p, "Transfer-Encoding") == NULL &&
              agentc_str_str((const char *)r.last_headers.p, "Host:") == NULL);
    check("hdrx_payload",
          buf_has(&h.first_payload, "\"provider\":\"anthropic\"") &&
              buf_has(&h.first_payload, "\"api\":\"anthropic-messages\"") &&
              buf_has(&h.first_payload, "\"headers\":{") &&
              buf_has(&h.first_payload, "\"content-type\":\"application/json\""));
    check("hdrx_dispatched_once", h.calls == 1);

    agentc_ext_host()->off(hh);
    hook_rec_clear(&h);
    agentc_buf_free(&r.last_body);
    agentc_buf_free(&r.last_headers);
    agentc_agent_free(a);
}

/* before_provider_request: a returned payload object becomes the sent body and
 * its length (Content-Length/--record follow body_len). */
static void test_provider_request_replace(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"payload\":{\"model\":\"replaced\",\"stream\":true}}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("before_provider_request", AGENTC_HOOK_OVERRIDE, &h);

    int rc = agentc_agent_submit(a, "go");
    static const char replaced[] = "{\"model\":\"replaced\",\"stream\":true}";
    check("reqx_rc", rc == 0);
    check("reqx_body_replaced",
          r.last_body.p && agentc_streq((const char *)r.last_body.p, replaced));
    check("reqx_body_len", r.last_body_len == agentc_strlen(replaced));
    check("reqx_original_seen",
          buf_has(&h.first_payload, "\"payload\":{") &&
              buf_has(&h.first_payload, "\"model\":\"claude-sonnet-4-5\"") &&
              buf_has(&h.first_payload, "\"url\":\"https://api.anthropic.com/"));

    agentc_ext_host()->off(hh);
    hook_rec_clear(&h);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* A fail-closed handler on either request override fails the turn with the
 * REQ_BLOCKED sentinel: one terminal error, no retry, no transport call. */
static void test_provider_block(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_retry(a, 3);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    uint64_t hh = agentc_ext_host()->on("before_provider_headers",
                                        AGENTC_HOOK_OVERRIDE, 0, fail_hook_fn, NULL);

    int rc = agentc_agent_submit(a, "go");
    check("blk_rc", rc == -1);
    check("blk_no_request", r.requests == 0 && r.i == 0);
    const char *err = agentc_agent_last_error(a);
    check("blk_error", err && agentc_streq(err, "request blocked by extension"));
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("blk_shape",
          tp && tp->n == 2 && tp->msgs[1].stop_reason == AGENTC_STOP_ERROR &&
              tp->msgs[1].error != NULL);
    check("blk_trace",
          count_occurrences((const char *)tr.tr.p, "error:request blocked by extension\n") == 1 &&
              count_occurrences((const char *)tr.tr.p, "tool_start") == 0);

    agentc_ext_host()->off(hh);
    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);

    /* the request override blocks the same way (and frees the built request) */
    Replay r2;
    agentc_memset(&r2, 0, sizeof r2);
    r2.n = 1;
    r2.codes[0] = 0;
    r2.bodies[0] = reply_final;
    AgcTransport t2 = { replay_request, &r2, NULL };
    AgcAgent *b = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(b, t2);
    agentc_agent_set_system(b, "sys");
    agentc_agent_set_retry(b, 3);
    agentc_agent_test_no_backoff(b);
    uint64_t bh = agentc_ext_host()->on("before_provider_request",
                                        AGENTC_HOOK_OVERRIDE, 0, fail_hook_fn, NULL);
    check("blk2_rc", agentc_agent_submit(b, "go") == -1);
    check("blk2_no_request", r2.requests == 0);
    check("blk2_error", agentc_streq(agentc_agent_last_error(b),
                                       "request blocked by extension"));
    agentc_ext_host()->off(bh);
    agentc_buf_free(&r2.last_body);
    agentc_agent_free(b);
}

/* after_provider_response gains the transport's response headers;
 * provider_stream_event gains the model. Both are quiet-gated observe points
 * with no typed event, so the agent emits them through the registry directly. */
static const char *fake_response_headers(void *ud) {
    (void)ud;
    return "{\"x-test\":\"1\"}";
}

static void test_provider_observe(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;

    AgcTransport t = { replay_request, &r, fake_response_headers };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    HookRec resp;
    agentc_memset(&resp, 0, sizeof resp);
    uint64_t rh = hook_rec_on("after_provider_response", AGENTC_HOOK_OBSERVE, &resp);
    HookRec sse;
    agentc_memset(&sse, 0, sizeof sse);
    uint64_t sh = hook_rec_on("provider_stream_event", AGENTC_HOOK_OBSERVE, &sse);

    int rc = agentc_agent_submit(a, "go");
    check("obs_rc", rc == 0);
    check("obs_headers",
          resp.calls == 1 &&
              buf_has(&resp.first_payload, "\"status\":") &&
              buf_has(&resp.first_payload, "\"headers\":{\"x-test\":\"1\"}") &&
              buf_has(&resp.first_payload, "\"transport_error\":0"));
    check("obs_sse_model",
          sse.calls > 0 &&
              buf_has(&sse.first_payload, "\"model\":\"claude-sonnet-4-5\"") &&
              buf_has(&sse.first_payload, "\"provider\":\"anthropic\"") &&
              buf_has(&sse.first_payload, "\"event\":\"message_start\"") &&
              buf_has(&sse.first_payload, "\"data\":{"));

    agentc_ext_host()->off(rh);
    agentc_ext_host()->off(sh);
    hook_rec_clear(&resp);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);

    /* a transport without response_headers reports an empty object */
    Replay r2;
    agentc_memset(&r2, 0, sizeof r2);
    r2.n = 1;
    r2.codes[0] = 0;
    r2.bodies[0] = reply_final;
    AgcTransport t2 = { replay_request, &r2, NULL };
    AgcAgent *b = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(b, t2);
    agentc_agent_set_system(b, "sys");
    agentc_agent_test_no_backoff(b);
    HookRec resp2;
    agentc_memset(&resp2, 0, sizeof resp2);
    uint64_t rh2 = hook_rec_on("after_provider_response", AGENTC_HOOK_OBSERVE, &resp2);
    check("obs2_rc", agentc_agent_submit(b, "go") == 0);
    check("obs2_headers_default",
          resp2.calls == 1 && buf_has(&resp2.first_payload, "\"headers\":{}"));
    agentc_ext_host()->off(rh2);
    hook_rec_clear(&resp2);
    agentc_buf_free(&sse.payload);
    agentc_buf_free(&sse.first_payload);
    agentc_buf_free(&r2.last_body);
    agentc_agent_free(b);
}

/* ------------------------------------------------------- select points */

/* model_select: a same-provider switch emits once with the previous id; an
 * unchanged id is silent, and removing the subscriber silences the fast path. */
static void test_model_select_same_provider(void) {
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    uint64_t hh = hook_rec_on("model_select", AGENTC_HOOK_OBSERVE, &h);

    check("model_sel_rc", agentc_agent_set_model(a, "claude-opus-4-1") == 0);
    check("model_sel_calls", h.calls == 1);
    check("model_sel_payload",
          buf_eq(&h.payload,
                 "{\"provider\":\"anthropic\",\"model\":\"claude-opus-4-1\","
                 "\"previous\":\"claude-sonnet-4-5\"}"));
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("model_sel_transcript", tp && agentc_streq(tp->model, "claude-opus-4-1"));

    check("model_sel_noop_rc", agentc_agent_set_model(a, "claude-opus-4-1") == 0);
    check("model_sel_noop_silent", h.calls == 1);
    check("model_sel_empty", agentc_agent_set_model(a, "") == -22);
    check("model_sel_null", agentc_agent_set_model(NULL, "x") == -22);

    /* wants fast path: with the only subscriber gone, a change is silent */
    agentc_ext_host()->off(hh);
    check("model_sel_off_rc", agentc_agent_set_model(a, "claude-haiku-4-5") == 0);
    check("model_sel_off_silent", h.calls == 1);

    hook_rec_clear(&h);
    agentc_agent_free(a);
}

/* thinking_level_select: every numeric change emits once, the level names
 * normalise 0/1/2/3/4 -> off/low/low/medium/high, and an unchanged level is
 * silent. */
static void test_thinking_select(void) {
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    uint64_t hh = hook_rec_on("thinking_level_select", AGENTC_HOOK_OBSERVE, &h);

    agentc_agent_set_thinking(a, 3);
    check("think_sel_medium",
          h.calls == 1 && buf_eq(&h.payload, "{\"level\":\"medium\",\"previous\":\"off\"}"));
    agentc_agent_set_thinking(a, 3);
    check("think_sel_noop", h.calls == 1);
    agentc_agent_set_thinking(a, 4);
    check("think_sel_high",
          h.calls == 2 && buf_eq(&h.payload, "{\"level\":\"high\",\"previous\":\"medium\"}"));
    /* numeric 2 normalizes to "low"; 2 -> 1 is still a numeric change */
    agentc_agent_set_thinking(a, 2);
    check("think_sel_low",
          h.calls == 3 && buf_eq(&h.payload, "{\"level\":\"low\",\"previous\":\"high\"}"));
    agentc_agent_set_thinking(a, 1);
    check("think_sel_low_norm",
          h.calls == 4 && buf_eq(&h.payload, "{\"level\":\"low\",\"previous\":\"low\"}"));
    /* the setter clamps out-of-range values before comparing */
    agentc_agent_set_thinking(a, 99);
    check("think_sel_clamp",
          h.calls == 5 && buf_eq(&h.payload, "{\"level\":\"high\",\"previous\":\"low\"}"));

    agentc_ext_host()->off(hh);
    agentc_agent_set_thinking(a, 0);
    check("think_sel_off_silent", h.calls == 5);

    hook_rec_clear(&h);
    agentc_agent_free(a);
}

/* model_select on rebuild: an effective provider/model change emits the new
 * pair once the rebuilt agent is installed, an unchanged rebuild stays silent,
 * and the rebuilt agent still emits observe points through the registry. */
static void test_model_select_rebuild(void) {
    AgcModeConfig cfg;
    agentc_memset(&cfg, 0, sizeof cfg);
    cfg.provider = "anthropic";
    cfg.model = "claude-sonnet-4-5";
    cfg.max_attempts = 1;
    cfg.session.cwd = "/tmp";
    cfg.session.memory_only = true;
    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);
    AgcModeCtx c;
    if (agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION | AGENTC_MODE_F_ABORT) != 0) {
        check("model_sel_rebuild_setup", false);
        return;
    }
    check("model_sel_rebuild_setup", true);

    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    uint64_t hh = hook_rec_on("model_select", AGENTC_HOOK_OBSERVE, &h);

    check("model_sel_rebuild_same", agentc_mode_rebuild_agent(&c) != NULL);
    check("model_sel_rebuild_same_silent", h.calls == 0);

    agentc_free(c.provider);
    c.provider = agentc_strdup("openai");
    agentc_free(c.model);
    c.model = agentc_strdup("gpt-5");
    check("model_sel_rebuild_change", agentc_mode_rebuild_agent(&c) != NULL);
    check("model_sel_rebuild_calls", h.calls == 1);
    check("model_sel_rebuild_payload",
          buf_eq(&h.payload,
                 "{\"provider\":\"openai\",\"model\":\"gpt-5\","
                 "\"previous\":\"claude-sonnet-4-5\"}"));
    const AgcTranscript *tp = agentc_agent_transcript(c.agent);
    check("model_sel_rebuild_agent",
          tp && agentc_streq(tp->provider, "openai") && agentc_streq(tp->model, "gpt-5"));

    /* Back to anthropic so the rebuilt agent can stream the replay fixture;
     * the subscriber is gone, so this second change is silent. */
    agentc_ext_host()->off(hh);
    agentc_free(c.provider);
    c.provider = agentc_strdup("anthropic");
    agentc_free(c.model);
    c.model = agentc_strdup("claude-sonnet-4-5");
    check("model_sel_rebuild_back", agentc_mode_rebuild_agent(&c) != NULL);

    /* The rebuild reinstall keeps the core's direct observe dispatch: from the
     * rebuilt agent, provider_stream_event must reach the registry. */
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;
    agentc_agent_set_transport(c.agent, (AgcTransport){ replay_request, &r, NULL });
    agentc_agent_test_no_backoff(c.agent);
    HookRec sse;
    agentc_memset(&sse, 0, sizeof sse);
    uint64_t sh = hook_rec_on("provider_stream_event", AGENTC_HOOK_OBSERVE, &sse);
    int rc = agentc_agent_submit(c.agent, "go");
    check("model_sel_rebuild_submit", rc == 0);
    check("model_sel_rebuild_bridge",
          sse.calls > 0 && buf_has(&sse.first_payload, "\"provider\":\"anthropic\""));

    agentc_ext_host()->off(sh);
    hook_rec_clear(&sse);
    hook_rec_clear(&h);
    agentc_buf_free(&r.last_body);
    agentc_mode_teardown(&c);
}

/* A non-hard turn that still carries tool calls (a max_tokens
 * truncation) honors turn_end `continue` through the same shared cap instead of
 * dropping it and settling. */
static void test_turn_end_continue_trunc(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_trunc;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_test_no_backoff(a);
    HookRec h;
    agentc_memset(&h, 0, sizeof h);
    h.result = "{\"continue\":true}";
    h.result_calls = 1;
    uint64_t hh = hook_rec_on("turn_end", AGENTC_HOOK_OVERRIDE, &h);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("turn_end_trunc_rc", rc == 0);
    check("turn_end_trunc_extra_request", r.i == 2);
    check("turn_end_trunc_hook_twice", h.calls == 2);
    check("turn_end_trunc_first_stop", buf_has(&h.first_payload, "\"stop_reason\":\"length\""));
    check("turn_end_trunc_no_exec", stub_calls == 0);
    check("turn_end_trunc_closed",
          tp && tp->n == 4 && tp->msgs[2].role == AGENTC_ROLE_TOOL &&
              agentc_streq(tp->msgs[2].blocks[0].text,
                           "error: response ended before this call could run"));
    check("turn_end_trunc_final",
          tp && tp->n == 4 && tp->msgs[3].role == AGENTC_ROLE_ASSISTANT &&
              agentc_streq(tp->msgs[3].blocks[0].text, "all done"));

    agentc_ext_host()->off(hh);
    hook_rec_clear(&h);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* --------------------------------------- wire cancellation + escaping */

/* cancel predicate: already true before run => the request is abandoned before
 * any transport work with the distinct -ECANCELED code. */
static void test_http_cancel_pre(void) {
    volatile bool cancel = true;
    AgcHttp *h = agentc_http_new("GET", "http://127.0.0.1:1/never", NULL, NULL, 0);
    check("http_cancel_pre_new", h != NULL);
    if (!h) return;
    agentc_http_set_cancel(h, &cancel);
    int rc = agentc_http_run(h, NULL, NULL, 1000);
    check("http_cancel_pre_rc", rc == -125);
    check("http_cancel_pre_err", agentc_streq(agentc_http_error(h), "cancelled"));
    agentc_http_free(h);
}

/* cancel set from the first body chunk => the poll loop abandons the request
 * without waiting out the timeout. */
typedef struct {
    AgcBuf body;
    volatile bool *cancel;
} CancelCtx;

static int cancel_on_body(void *ud, const void *p, size_t n) {
    CancelCtx *c = ud;
    agentc_buf_push(&c->body, p, n);
    *c->cancel = true;
    return 0;
}

static bool write_text_file(const char *path, const char *text) {
    int fd = os_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t len = agentc_strlen(text);
    bool ok = os_write(fd, text, len) == (int)len;
    os_close(fd);
    return ok;
}

static void test_http_cancel_midstream(void) {
    const char *path = "/tmp/agentc-http-cancel.mock";
    const char *script =
        "dns cancel.test 192.0.2.51\n"
        "data 485454502f312e3120323030204f4b0d0a436f6e74656e742d4c656e6774683a2031300d0a"
        "0d0a616263\n"
        "eagain\n";
    check("http_cancel_mid_write", write_text_file(path, script));
    check("http_cancel_mid_load", agentc_mock_load(path) == 0);
    volatile bool cancel = false;
    CancelCtx ctx = { { 0 }, &cancel };
    AgcHttp *h = agentc_http_new("GET", "http://cancel.test/", NULL, NULL, 0);
    check("http_cancel_mid_new", h != NULL);
    if (!h) return;
    agentc_http_set_cancel(h, &cancel);
    int rc = agentc_http_run(h, cancel_on_body, &ctx, 5000);
    check("http_cancel_mid_rc", rc == -125);
    check("http_cancel_mid_partial",
          ctx.body.len == 3 && agentc_memeq(ctx.body.p, "abc", 3));
    check("http_cancel_mid_err", agentc_streq(agentc_http_error(h), "cancelled"));
    agentc_http_free(h);
    agentc_buf_free(&ctx.body);
}

/* Control and non-UTF-8 header bytes must still yield valid JSON;
 * this document escapes every byte >= 0x7f instead of emitting it raw. */
static void test_http_headers_json_escape(void) {
    const char *path = "/tmp/agentc-http-headers.mock";
    const char *script =
        "dns hdr.test 192.0.2.77\n"
        "data 485454502f312e3120323030204f4b0d0a582d43746c3a20610162ff0d0a0d0a\n"
        "eof\n";
    check("hdrjson_write", write_text_file(path, script));
    check("hdrjson_load", agentc_mock_load(path) == 0);
    AgcHttp *h = agentc_http_new("GET", "http://hdr.test/", NULL, NULL, 0);
    check("hdrjson_new", h != NULL);
    if (!h) return;
    int rc = agentc_http_run(h, NULL, NULL, 5000);
    const char *json = agentc_http_headers_json(h);
    check("hdrjson_rc", rc == 0);
    check("hdrjson_escaped", json && agentc_streq(json, "{\"X-Ctl\":\"a\\u0001b\\u00ff\"}"));
    agentc_http_free(h);
}

/* ------------------------------------------------- typed extension provider */

/* The golden suite does not link the manifest examples, so this in-file
 * fixture mirrors extensions/fake_provider: Bearer auth, a body built from the
 * materialised request view and a canned JSON event vocabulary. The counters
 * prove stream_open/stream_close pairing across attempts and cancel. */
static int g_ep_open_calls;
static int g_ep_close_calls;

static int ep_build_request(const AgcExtHost *host, const AgcExtProvider *self,
                            const AgcExtRequestView *req, void *head_out, void *body_out) {
    (void)self;
    /* legitimate line, then the forbidden trio, an extension-supplied auth
     * line and a CRLF injection -- the core adapter sanitizes all of it */
    host->out_write(head_out, "x-epfix: 1\r\n", 12);
    host->out_write(head_out, "Host: evil.test\r\n", 17);
    host->out_write(head_out, "content-length: 999\r\n", 20);
    host->out_write(head_out, "Transfer-Encoding: chunked\r\n", 27);
    host->out_write(head_out, "Authorization: Bearer extension-key\r\n", 37);
    host->out_write(head_out, "x-inj: a\rb\r\n", 12);

    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "provider");
    agentc_jsonw_cstr(&w, req->provider ? req->provider : "");
    agentc_jsonw_key(&w, "model");
    agentc_jsonw_cstr(&w, req->model ? req->model : "");
    agentc_jsonw_key(&w, "system");
    agentc_jsonw_cstr(&w, req->system ? req->system : "");
    agentc_jsonw_key(&w, "max_tokens");
    agentc_jsonw_i64(&w, req->max_tokens);
    agentc_jsonw_key(&w, "thinking");
    agentc_jsonw_i64(&w, req->thinking_level);
    agentc_jsonw_key(&w, "roles");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < req->nmessages; i++)
        agentc_jsonw_i64(&w, (i64)req->messages[i].role);
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "tools");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < req->ntools; i++)
        agentc_jsonw_cstr(&w, req->tools[i].name ? req->tools[i].name : "");
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "texts");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < req->nmessages; i++) {
        const AgcExtMessageView *m = &req->messages[i];
        for (size_t j = 0; j < m->nblocks; j++) {
            const AgcExtBlockView *blk = &m->blocks[j];
            if (blk->text) agentc_jsonw_str(&w, blk->text, blk->text_len);
        }
    }
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "tool_ids");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < req->nmessages; i++) {
        const AgcExtMessageView *m = &req->messages[i];
        for (size_t j = 0; j < m->nblocks; j++)
            if (m->blocks[j].tool_id) agentc_jsonw_cstr(&w, m->blocks[j].tool_id);
    }
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "tool_calls");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < req->nmessages; i++) {
        const AgcExtMessageView *m = &req->messages[i];
        for (size_t j = 0; j < m->nblocks; j++) {
            const AgcExtBlockView *blk = &m->blocks[j];
            if (blk->type != AGENTC_EXT_BLK_TOOLCALL) continue;
            AgcBuf one = { 0 };
            agentc_buf_cstr(&one, blk->tool_name ? blk->tool_name : "");
            agentc_buf_cstr(&one, ":");
            agentc_buf_cstr(&one, blk->tool_args ? blk->tool_args : "");
            agentc_jsonw_cstr(&w, (const char *)one.p);
            agentc_buf_free(&one);
        }
    }
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    host->out_write(body_out, (const char *)b.p, b.len);
    agentc_buf_free(&b);
    return 0;
}

static int ep_stream_event(const AgcExtHost *host, const AgcExtProvider *self,
                           AgcExtStream *st, const AgcExtWireEvent *ev,
                           const AgcExtStreamSink *sink) {
    (void)self;
    if (!ev || !ev->data) return 0;
    if (ev->data_len == 6 && agentc_streq(ev->data, "[DONE]")) {
        sink->stop(st, AGENTC_EXT_STOP_STOP);
        return 0;
    }
    const char *kind = host->json_get_str(ev->data, "kind", "");
    if (agentc_streq(kind, "text")) {
        const char *text = host->json_get_str(ev->data, "text", "");
        sink->text(st, text, agentc_strlen(text));
    } else if (agentc_streq(kind, "thinking")) {
        const char *text = host->json_get_str(ev->data, "text", "");
        sink->thinking(st, text, agentc_strlen(text));
    } else if (agentc_streq(kind, "tool_call")) {
        const char *id = host->json_get_str(ev->data, "id", "");
        const char *name = host->json_get_str(ev->data, "name", "");
        const char *args = host->json_get_str(ev->data, "args", "");
        sink->tool_start(st, id, name);
        if (args[0]) sink->tool_args(st, args, agentc_strlen(args));
    } else if (agentc_streq(kind, "usage")) {
        AgcExtUsage u;
        agentc_memset(&u, 0, sizeof u);
        u.struct_size = sizeof u;
        u.fields = AGENTC_EXT_USAGE_INPUT | AGENTC_EXT_USAGE_OUTPUT |
                   AGENTC_EXT_USAGE_CACHE_READ | AGENTC_EXT_USAGE_REASONING;
        u.input = (u32)host->json_get_int(ev->data, "input", 0);
        u.output = (u32)host->json_get_int(ev->data, "output", 0);
        u.cache_read = (u32)host->json_get_int(ev->data, "cache_read", 0);
        u.reasoning = (u32)host->json_get_int(ev->data, "reasoning", 0);
        sink->usage(st, &u);
    } else if (agentc_streq(kind, "stop")) {
        const char *reason = host->json_get_str(ev->data, "reason", "stop");
        int sr = agentc_streq(reason, "tool_use") ? AGENTC_EXT_STOP_TOOLUSE
                 : agentc_streq(reason, "length") ? AGENTC_EXT_STOP_LENGTH
                 : agentc_streq(reason, "error")  ? AGENTC_EXT_STOP_ERROR
                                                   : AGENTC_EXT_STOP_STOP;
        sink->stop(st, sr);
    } else if (agentc_streq(kind, "error")) {
        sink->error(st, host->json_get_str(ev->data, "message", "provider error"));
    }
    return 0;
}

static int ep_stream_open(const AgcExtHost *host, const AgcExtProvider *self,
                          AgcExtStream *st) {
    (void)self;
    g_ep_open_calls++;
    int *events = host->alloc(sizeof *events);
    if (!events) return -12;
    *events = 0;
    st->ud = events;
    return 0;
}

static void ep_stream_close(const AgcExtHost *host, const AgcExtProvider *self,
                            AgcExtStream *st) {
    (void)self;
    g_ep_close_calls++;
    host->free(st->ud);
    st->ud = NULL;
}

static const AgcExtProviderAuth g_ep_auth = {
    .struct_size = sizeof(AgcExtProviderAuth),
    .kind = AGENTC_EXT_AUTH_BEARER,
    .header = "Authorization",
    .prefix = "Bearer ",
};

static const AgcExtProviderModel g_ep_models[] = {
    { .struct_size = sizeof(AgcExtProviderModel), .id = "ep-model", .name = "EP Model",
      .ctx_window = 4096, .max_tokens = 512, .reasoning = 0, .image = 0 },
};

static const AgcExtProvider g_ep_desc = {
    .struct_size = sizeof(AgcExtProvider),
    .name = "epfix",
    .label = "Loop provider fixture",
    .default_base_url = "http://ep.test/v1",
    .path = "/chat",
    .env_keys = { "EPFIX_API_KEY", NULL, NULL },
    .needs_key = 1,
    .discover_style = AGENTC_EXT_DISCOVER_NONE,
    .auth = &g_ep_auth,
    .models = g_ep_models,
    .nmodels = sizeof g_ep_models / sizeof g_ep_models[0],
    .build_request = ep_build_request,
    .stream_open = ep_stream_open,
    .stream_event = ep_stream_event,
    .stream_close = ep_stream_close,
};

static int ep_init(const AgcExtHost *host) {
    host->add_provider(&g_ep_desc);
    return 0;
}

static int ep_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    out->abi_version = AGENTC_EXT_ABI;
    out->struct_size = sizeof *out;
    out->name = "loop-ep-ext";
    out->version = "1";
    out->init = ep_init;
    return 0;
}

static const AgcProvider *ep_setup(void) {
    agentc_ext_shutdown();
    g_ep_open_calls = 0;
    g_ep_close_calls = 0;
    agentc_ext_adopt("loop-ep-ext", ep_entry);
    agentc_ext_load_all();
    const AgcProviderOps *ops = agentc_provider_by_name("epfix");
    return ops ? agentc_provider_handle((AgcProviderOps *)ops) : NULL;
}

/* The canned SSE for this fixture. Each `data:` payload is one event. */
static const char ep_reply_tools[] =
    "event: ep\n"
    "data: {\"kind\":\"thinking\",\"text\":\"plan\"}\n\n"
    "event: ep\n"
    "data: {\"kind\":\"text\",\"text\":\"working\"}\n\n"
    "event: ep\n"
    "data: {\"kind\":\"tool_call\",\"id\":\"call-ep1\",\"name\":\"read\","
    "\"args\":\"{\\\"path\\\":\\\"f.txt\\\"}\"}\n\n"
    "event: ep\n"
    "data: {\"kind\":\"usage\",\"input\":7,\"output\":9,\"cache_read\":2,"
    "\"reasoning\":1}\n\n"
    "event: ep\n"
    "data: {\"kind\":\"stop\",\"reason\":\"tool_use\"}\n\n";

static const char ep_reply_final[] =
    "event: ep\n"
    "data: {\"kind\":\"text\",\"text\":\"all done\"}\n\n"
    "event: ep\n"
    "data: {\"kind\":\"usage\",\"input\":3,\"output\":2}\n\n"
    "event: ep\n"
    "data: [DONE]\n\n";

static const char ep_reply_error[] =
    "event: ep\n"
    "data: {\"kind\":\"error\",\"message\":\"ep boom\"}\n\n";

/* A fresh view build with a filtered transcript: a failed assistant and an
 * empty assistant never serialize, and a HIDDEN tool never reaches the body. */
static void test_ext_provider_view(void) {
    const AgcProvider *p = ep_setup();
    check("ep_view_handle", p != NULL);
    if (!p) return;
    AgcTranscript t;
    agentc_transcript_init(&t);
    AgcMsg *u = agentc_transcript_push(&t, AGENTC_ROLE_USER);
    agentc_msg_add_text(u, "hello", 5);
    AgcMsg *bad = agentc_transcript_push(&t, AGENTC_ROLE_ASSISTANT);
    agentc_msg_add_text(bad, "boom", 4);
    bad->stop_reason = AGENTC_STOP_ERROR;
    (void)agentc_transcript_push(&t, AGENTC_ROLE_ASSISTANT);
    AgcMsg *a = agentc_transcript_push(&t, AGENTC_ROLE_ASSISTANT);
    a->stop_reason = AGENTC_STOP_TOOLUSE;
    agentc_msg_add_tool_call(a, "call-view", "read");
    agentc_msg_tool_args_append(a, "{\"path\":\"f.txt\"}", 16);
    AgcMsg *tool = agentc_transcript_push(&t, AGENTC_ROLE_TOOL);
    agentc_msg_add_text(tool, "result", 6);
    tool->blocks[0].tool_id = agentc_strdup("call-view");

    AgcTool tools[2];
    tools[0] = stub_tool;
    tools[1] = hidden_tool;

    AgcRequest req;
    agentc_memset(&req, 0, sizeof req);
    req.provider = "epfix";
    req.model = "ep-model";
    req.api_key = "sk-ep";
    req.system = "sys";
    req.transcript = &t;
    req.tools = tools;
    req.ntools = 2;
    req.thinking_level = 2;
    req.max_tokens = 1234;

    AgcBuf out = { 0 };
    int rc = p->build_request(&out, &req, NULL, "/chat");
    check("ep_view_rc", rc == 0);
    i64 k = agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4);
    check("ep_view_split", k >= 0);
    const char *body = (const char *)out.p + k + 4;
    check("ep_view_roles", agentc_str_str(body, "\"roles\":[1,2,3]") != NULL);
    check("ep_view_texts",
          agentc_str_str(body, "\"texts\":[\"hello\",\"result\"]") != NULL);
    check("ep_view_tools_visible",
          agentc_str_str(body, "\"tools\":[\"read\"]") != NULL &&
              agentc_str_str(body, "secret") == NULL);
    check("ep_view_tool_call",
          agentc_str_str(body, "read:{\\\"path\\\":\\\"f.txt\\\"}") != NULL);
    check("ep_view_tool_id", agentc_str_str(body, "\"tool_ids\":[\"call-view\"]") != NULL);
    check("ep_view_numbers",
          agentc_str_str(body, "\"max_tokens\":1234") != NULL &&
              agentc_str_str(body, "\"thinking\":2") != NULL &&
              agentc_str_str(body, "\"system\":\"sys\"") != NULL);
    agentc_buf_free(&out);
    agentc_transcript_free(&t);
    agentc_ext_shutdown();
}

static void test_ext_provider_roundtrip(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = ep_reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = ep_reply_final;
    r.capture_headers = true;

    const AgcProvider *p = ep_setup();
    check("ep_rt_handle", p != NULL);
    if (!p) return;
    AgcAgent *a = agentc_agent_new(p, "ep-model");
    agentc_agent_set_transport(a, (AgcTransport){ replay_request, &r, NULL });
    agentc_agent_set_api_key(a, "sk-ep");
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    stub_calls = 0;

    int rc = agentc_agent_submit(a, "go");
    check("ep_rt_rc", rc == 0);
    check("ep_rt_requests", r.i == 2);
    check("ep_rt_tool_calls", stub_calls == 1);
    check("ep_rt_streams", g_ep_open_calls == 2 && g_ep_close_calls == 2);

    const char *h1 = r.first_headers.p ? (const char *)r.first_headers.p : "";
    check("ep_rt_header_ok", agentc_str_str(h1, "x-epfix: 1") != NULL);
    check("ep_rt_auth_core", agentc_str_str(h1, "Authorization: Bearer sk-ep") != NULL);
    check("ep_rt_host_dropped", agentc_str_str(h1, "Host:") == NULL);
    check("ep_rt_cl_dropped", agentc_str_str(h1, "content-length:") == NULL);
    check("ep_rt_te_dropped", agentc_str_str(h1, "Transfer-Encoding:") == NULL);
    check("ep_rt_ext_auth_dropped", agentc_str_str(h1, "extension-key") == NULL);
    check("ep_rt_injection_spaced", agentc_str_str(h1, "x-inj: a b") != NULL);

    const char *b1 = r.first_body.p ? (const char *)r.first_body.p : "";
    check("ep_rt_first_roles", agentc_str_str(b1, "\"roles\":[1]") != NULL);
    const char *b2 = r.last_body.p ? (const char *)r.last_body.p : "";
    check("ep_rt_second_roles", agentc_str_str(b2, "\"roles\":[1,2,3]") != NULL);
    check("ep_rt_second_tool_result", agentc_str_str(b2, "stub(") != NULL);
    check("ep_rt_second_tool_call", agentc_str_str(b2, "read:{\\\"path\\\":\\\"f.txt\\\"}") != NULL);

    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("ep_rt_msgs", tp && tp->n == 4);
    bool asst_blocks = tp && tp->n == 4 && tp->msgs[1].nblocks == 3 &&
                       tp->msgs[1].blocks[0].type == AGENTC_BLK_THINK &&
                       agentc_streq(tp->msgs[1].blocks[0].text, "plan") &&
                       tp->msgs[1].blocks[1].type == AGENTC_BLK_TEXT &&
                       agentc_streq(tp->msgs[1].blocks[1].text, "working") &&
                       tp->msgs[1].blocks[2].type == AGENTC_BLK_TOOLCALL &&
                       agentc_streq(tp->msgs[1].blocks[2].tool_id, "call-ep1") &&
                       agentc_streq(tp->msgs[1].blocks[2].tool_name, "read") &&
                       agentc_streq(tp->msgs[1].blocks[2].tool_args, "{\"path\":\"f.txt\"}");
    check("ep_rt_block_order", asst_blocks);
    check("ep_rt_stop_tooluse", tp && tp->n == 4 && tp->msgs[1].stop_reason == AGENTC_STOP_TOOLUSE);
    check("ep_rt_usage",
          tp && tp->n == 4 && tp->msgs[1].usage.input == 7 && tp->msgs[1].usage.output == 9 &&
              tp->msgs[1].usage.cache_read == 2 && tp->msgs[1].usage.reasoning == 1);
    check("ep_rt_final",
          tp && tp->n == 4 && tp->msgs[3].nblocks == 1 &&
              agentc_streq(tp->msgs[3].blocks[0].text, "all done") &&
              tp->msgs[3].stop_reason == AGENTC_STOP_STOP &&
              tp->msgs[3].usage.input == 3 && tp->msgs[3].usage.output == 2);

    /* the typed deltas arrive in stream order */
    const char *trace = (const char *)tr.tr.p;
    const char *think_at = agentc_str_str(trace, "think:plan");
    const char *text_at = agentc_str_str(trace, "text:working");
    const char *args_at = agentc_str_str(trace, "args:");
    check("ep_rt_delta_order",
          think_at && text_at && args_at && think_at < text_at && text_at < args_at);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_buf_free(&r.last_headers);
    agentc_buf_free(&r.first_body);
    agentc_buf_free(&r.first_headers);
    agentc_agent_free(a);
    agentc_ext_shutdown();
}

static void test_ext_provider_error(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = ep_reply_error;

    const AgcProvider *p = ep_setup();
    check("ep_err_handle", p != NULL);
    if (!p) return;
    AgcAgent *a = agentc_agent_new(p, "ep-model");
    agentc_agent_set_transport(a, (AgcTransport){ replay_request, &r, NULL });
    agentc_agent_set_api_key(a, "sk-ep");
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);

    int rc = agentc_agent_submit(a, "go");
    check("ep_err_rc", rc == -1);
    const char *err = agentc_agent_last_error(a);
    check("ep_err_message", err && agentc_streq(err, "ep boom"));
    check("ep_err_streams", g_ep_open_calls == 1 && g_ep_close_calls == 1);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("ep_err_state",
          tp && tp->n == 2 && tp->msgs[1].stop_reason == AGENTC_STOP_ERROR);

    agentc_buf_free(&r.last_body);
    agentc_buf_free(&r.last_headers);
    agentc_buf_free(&r.first_body);
    agentc_buf_free(&r.first_headers);
    agentc_agent_free(a);
    agentc_ext_shutdown();
}

static void test_ext_provider_retry(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 429;
    r.bodies[0] = NULL;
    r.codes[1] = 0;
    r.bodies[1] = ep_reply_final;

    const AgcProvider *p = ep_setup();
    check("ep_retry_handle", p != NULL);
    if (!p) return;
    AgcAgent *a = agentc_agent_new(p, "ep-model");
    agentc_agent_set_transport(a, (AgcTransport){ replay_request, &r, NULL });
    agentc_agent_set_api_key(a, "sk-ep");
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_retry(a, 3);
    agentc_agent_test_no_backoff(a);

    int rc = agentc_agent_submit(a, "go");
    check("ep_retry_rc", rc == 0);
    check("ep_retry_two_requests", r.i == 2);
    check("ep_retry_reopened", g_ep_open_calls == 2 && g_ep_close_calls == 2);
    check("ep_retry_error_cleared", agentc_agent_last_error(a) == NULL);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("ep_retry_final",
          tp && tp->n == 2 && agentc_streq(tp->msgs[1].blocks[0].text, "all done"));

    agentc_buf_free(&r.last_body);
    agentc_buf_free(&r.last_headers);
    agentc_buf_free(&r.first_body);
    agentc_buf_free(&r.first_headers);
    agentc_agent_free(a);
    agentc_ext_shutdown();
}

static void test_ext_provider_cancel(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = ep_reply_tools;

    const AgcProvider *p = ep_setup();
    check("ep_cancel_handle", p != NULL);
    if (!p) return;
    AgcAgent *a = agentc_agent_new(p, "ep-model");
    agentc_agent_set_transport(a, (AgcTransport){ replay_request, &r, NULL });
    agentc_agent_set_api_key(a, "sk-ep");
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_test_no_backoff(a);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    abort_agent = a;

    int rc = agentc_agent_submit(a, "go");
    abort_agent = NULL;
    check("ep_cancel_rc", rc == -1);
    const char *err = agentc_agent_last_error(a);
    check("ep_cancel_error", err && agentc_streq(err, "aborted"));
    check("ep_cancel_one_request", r.requests == 1);
    check("ep_cancel_closed", g_ep_open_calls == 1 && g_ep_close_calls == 1);
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("ep_cancel_state",
          tp && tp->n == 2 && tp->msgs[1].stop_reason == AGENTC_STOP_ABORTED);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_buf_free(&r.last_headers);
    agentc_buf_free(&r.first_body);
    agentc_buf_free(&r.first_headers);
    agentc_agent_free(a);
    agentc_ext_shutdown();
}

static void test_ext_provider_unload(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = ep_reply_final;

    const AgcProvider *p = ep_setup();
    check("ep_unload_handle", p != NULL);
    if (!p) return;
    AgcAgent *a = agentc_agent_new(p, "ep-model");
    agentc_agent_set_transport(a, (AgcTransport){ replay_request, &r, NULL });
    agentc_agent_set_api_key(a, "sk-ep");
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);

    agentc_ext_unload("loop-ep-ext");
    check("ep_unload_lookup", agentc_provider_by_name("epfix") == NULL);

    int rc = agentc_agent_submit(a, "go");
    check("ep_unload_rc", rc == -1);
    const char *err = agentc_agent_last_error(a);
    check("ep_unload_error", err != NULL && err[0]);
    check("ep_unload_no_request", r.requests == 0);
    check("ep_unload_no_stream", g_ep_open_calls == 0 && g_ep_close_calls == 0);

    agentc_buf_free(&r.last_body);
    agentc_buf_free(&r.last_headers);
    agentc_buf_free(&r.first_body);
    agentc_buf_free(&r.first_headers);
    agentc_agent_free(a);
    agentc_ext_shutdown();
}

/* ------------------------------------------- discovery auth + enumeration */

/* A HEADER-auth row must reach discovery with its declared header, not the
 * Bearer default; the mock backend captures the request bytes. */
static void test_ext_discovery_auth(void) {
    /* a fixed empty home keeps the --list-models pass below cache/credential
     * free (no live network either: the enumeration is offline) */
    agentc_test_setenv("HOME", "/tmp/agentc-ep-home");
    agentc_test_setenv("XDG_CONFIG_HOME", "/tmp/agentc-ep-home/.config");
    agentc_test_setenv("EPFIX_API_KEY", "k1");
    agentc_rm_rf("/tmp/agentc-ep-home");
    ep_setup();
    AgcExtProviderAuth ha = g_ep_auth;
    ha.kind = AGENTC_EXT_AUTH_HEADER;
    ha.header = "X-Ep-Key";
    ha.prefix = NULL;
    AgcExtProvider hdr = g_ep_desc;
    hdr.name = "ephdr";
    hdr.label = NULL;
    hdr.auth = &ha;
    hdr.discover_style = AGENTC_EXT_DISCOVER_DEFAULT;
    agentc_ext_host()->add_provider(&hdr);
    const AgcProviderOps *ops = agentc_provider_by_name("ephdr");
    check("ep_auth_header_row", ops != NULL);

    const char *script =
        "dns ep.test 192.0.2.90\n"
        "data 485454502f312e3120323030204f4b0d0a436f6e74656e742d547970653a2061"
        "70706c69636174696f6e2f6a736f6e0d0a436f6e74656e742d4c656e6774683a"
        "2033300d0a0d0a7b2264617461223a5b7b226964223a2265702d6d6f64656c2d"
        "32227d5d7d\n";
    check("ep_auth_mock_write",
          write_text_file("/tmp/agentc-ep-discovery.mock", script));
    check("ep_auth_mock_load", agentc_mock_load("/tmp/agentc-ep-discovery.mock") == 0);

    AgcDiscovered *m = NULL;
    char err[128] = "";
    size_t n = agentc_discover_models("ephdr", "http://ep.test/v1", "k1", &m, 8, 3000, err,
                                      sizeof err);
    check("ep_auth_discover", n == 1 && m && agentc_streq(m[0].id, "ep-model-2"));
    const AgcBuf *sent = agentc_mock_sent();
    const char *req = sent && sent->p ? (const char *)sent->p : "";
    check("ep_auth_header_sent", agentc_str_str(req, "X-Ep-Key: k1") != NULL);
    check("ep_auth_not_bearer", agentc_str_str(req, "authorization: Bearer") == NULL);

    /* A CR/LF in an extension-supplied key is untrusted input: it must not forge
     * an extra discovery header line, matching the builtin hooks' sanitizing. */
    check("ep_auth_inject_mock_load",
          agentc_mock_load("/tmp/agentc-ep-discovery.mock") == 0);
    AgcDiscovered *im = NULL;
    char ierr[128] = "";
    size_t in = agentc_discover_models("ephdr", "http://ep.test/v1", "k1\r\nX-Forged: 1", &im,
                                      8, 3000, ierr, sizeof ierr);
    const AgcBuf *isent = agentc_mock_sent();
    const char *ireq = isent && isent->p ? (const char *)isent->p : "";
    check("ep_auth_key_sanitized",
          in == 1 && agentc_str_str(ireq, "\r\nX-Forged") == NULL &&
              agentc_str_str(ireq, "X-Ep-Key: k1X-Forged: 1") != NULL);
    agentc_discover_free(im, in);

    /* An AUTH_NONE row with a resolved key must send no auth line at all: the
     * row's empty override is authoritative, not a Bearer fallback. */
    AgcExtProviderAuth na = g_ep_auth;
    na.kind = AGENTC_EXT_AUTH_NONE;
    na.header = NULL;
    na.prefix = NULL;
    AgcExtProvider none = g_ep_desc;
    none.name = "epnone";
    none.label = NULL;
    none.auth = &na;
    none.models = NULL;
    none.nmodels = 0;
    none.discover_style = AGENTC_EXT_DISCOVER_DEFAULT;
    agentc_ext_host()->add_provider(&none);
    check("ep_auth_none_row", agentc_provider_by_name("epnone") != NULL);
    check("ep_auth_none_mock_load", agentc_mock_load("/tmp/agentc-ep-discovery.mock") == 0);
    AgcDiscovered *nm = NULL;
    char nerr[128] = "";
    size_t nn = agentc_discover_models("epnone", "http://ep.test/v1", "k1", &nm, 8, 3000,
                                       nerr, sizeof nerr);
    check("ep_auth_none_discover", nn == 1 && nm && agentc_streq(nm[0].id, "ep-model-2"));
    const AgcBuf *nsent = agentc_mock_sent();
    const char *nreq = nsent && nsent->p ? (const char *)nsent->p : "";
    check("ep_auth_none_no_auth",
          agentc_str_str(nreq, "authorization") == NULL &&
              agentc_str_str(nreq, "Authorization") == NULL);
    agentc_discover_free(nm, nn);
    /* the real enumeration entry point prints the row's model from the cache
     * (offline), so a HEADER-auth provider works through --list-models */
    check("ep_auth_cache",
          n == 1 && agentc_discover_cache_save("ephdr", "http://ep.test/v1", m, n) == 0);
    check("ep_auth_list_rc", agentc_setup_list_models(NULL, "ephdr", false, true) == 0);
    agentc_discover_free(m, n);
    agentc_ext_shutdown();
    agentc_rm_rf("/tmp/agentc-ep-home");
    agentc_test_clearenv();
}

/* agentc_provider_all()/agentc_setup_provider_names() see the extension row and
 * the static model survives a dynamic clear (the list-models enumeration is
 * row-driven). */
static void test_ext_provider_enumeration(void) {
    ep_setup();
    const char *names[64];
    size_t nn = agentc_setup_provider_names(names, 64);
    bool saw_ep = false;
    for (size_t i = 0; i < nn; i++)
        if (agentc_streq(names[i], "epfix")) saw_ep = true;
    check("ep_enum_row", saw_ep);
    const AgcProviderOps *all[96];
    size_t n = agentc_provider_all(all, 96);
    size_t hits = 0;
    for (size_t i = 0; i < n; i++)
        if (all[i]->name && agentc_streq(all[i]->name, "epfix")) hits++;
    check("ep_enum_once", hits == 1);
    agentc_model_clear_dynamic("epfix");
    const AgcModel *m = agentc_model_find("epfix", "ep-model");
    check("ep_enum_static_survives", m && agentc_model_is_static(m));
    agentc_ext_shutdown();
}

/* --------------------------------------- async tools through the loop */

/* One in-file async extension adopted through agentc_ext_adopt. Every tool
 * takes the start/step/stop path; the turn loop must drive them through the
 * same job table a synchronous run() uses. aloop_abort calls back into the
 * running agent from inside a step: that is the deterministic window where
 * the batch's cancel flag flips and the driver delivers stop(CANCELLED). */

enum { ALOOP_OK = 0, ALOOP_SLOW, ALOOP_ABORT, ALOOP_A, ALOOP_B, ALOOP_TOOLS };

typedef struct {
    u32 magic;
    int tool;
    int i;
    int steps;
    char tag;
} AloopState;

#define ALOOP_MAGIC 0x414c4f4fu

static AgcAgent *g_aloop_agent;
static AgcBuf g_aloop_seq;              /* interleave trace: "a0b0a1b1" */
static int g_aloop_starts, g_aloop_stepped, g_aloop_stops;
static int g_aloop_stop_count[ALOOP_TOOLS];
static int g_aloop_stop_reason[ALOOP_TOOLS];
static int g_aloop_pumps;

static void aloop_reset(void) {
    agentc_buf_clear(&g_aloop_seq);
    g_aloop_starts = g_aloop_stepped = g_aloop_stops = 0;
    agentc_memset(g_aloop_stop_count, 0, sizeof g_aloop_stop_count);
    agentc_memset(g_aloop_stop_reason, 0, sizeof g_aloop_stop_reason);
}

static int aloop_start(const AgcExtHost *host, const AgcExtTool *self,
                       const AgcExtToolCall *call, void *out, bool *is_error,
                       void **state) {
    (void)call;
    (void)out;
    AloopState *st = host->alloc(sizeof *st);
    if (!st) return -12;
    st->magic = ALOOP_MAGIC;
    st->tool = *(const int *)self->ud;
    st->i = 0;
    st->steps = st->tool == ALOOP_OK ? 3 : 2;
    st->tag = st->tool == ALOOP_B ? 'b' : 'a';
    g_aloop_starts++;
    if (is_error) *is_error = false;
    *state = st;
    return 0;
}

static int aloop_step(const AgcExtHost *host, const AgcExtTool *self,
                      const AgcExtToolCall *call, void *state, void *out,
                      bool *is_error) {
    (void)self;
    (void)call;
    AloopState *st = state;
    if (!st || st->magic != ALOOP_MAGIC) return -22;
    g_aloop_stepped++;
    if (st->tool == ALOOP_SLOW) return 0;   /* the driver times it out */
    if (st->tool == ALOOP_ABORT) {
        if (g_aloop_agent) agentc_agent_abort(g_aloop_agent);
        return 0;   /* next driver tick: cancel -> "error: aborted" */
    }
    char line[3] = { st->tag, (char)('0' + st->i), '\n' };
    host->out_write(out, line, sizeof line);
    agentc_buf_push(&g_aloop_seq, line, 2);   /* step trace, no newline */
    st->i++;
    if (is_error) *is_error = false;
    return st->i >= st->steps ? 1 : 0;
}

static void aloop_stop(const AgcExtHost *host, const AgcExtTool *self, void *state,
                       int reason) {
    (void)self;
    AloopState *st = state;
    g_aloop_stops++;
    if (st && st->magic == ALOOP_MAGIC) {
        g_aloop_stop_count[st->tool]++;
        g_aloop_stop_reason[st->tool] = reason;
    }
    if (st) host->free(st);
}

static int aloop_ids[ALOOP_TOOLS] = { ALOOP_OK, ALOOP_SLOW, ALOOP_ABORT, ALOOP_A,
                                      ALOOP_B };

static const AgcExtTool aloop_tools[ALOOP_TOOLS] = {
    { .struct_size = sizeof(AgcExtTool), .flags = AGENTC_TOOL_READONLY,
      .name = "aloop_ok", .label = "ok", .description = "streams three steps",
      .parameters_json = "{\"type\":\"object\"}", .ud = &aloop_ids[ALOOP_OK],
      .start = aloop_start, .step = aloop_step, .stop = aloop_stop },
    { .struct_size = sizeof(AgcExtTool), .flags = AGENTC_TOOL_READONLY,
      .name = "aloop_slow", .label = "slow", .description = "never completes",
      .parameters_json = "{\"type\":\"object\"}", .ud = &aloop_ids[ALOOP_SLOW],
      .timeout_ms = 1, .start = aloop_start, .step = aloop_step, .stop = aloop_stop },
    { .struct_size = sizeof(AgcExtTool), .flags = AGENTC_TOOL_READONLY,
      .name = "aloop_abort", .label = "abort", .description = "aborts the turn",
      .parameters_json = "{\"type\":\"object\"}", .ud = &aloop_ids[ALOOP_ABORT],
      .start = aloop_start, .step = aloop_step, .stop = aloop_stop },
    { .struct_size = sizeof(AgcExtTool), .flags = AGENTC_TOOL_READONLY,
      .name = "aloop_a", .label = "a", .description = "batch tool a",
      .parameters_json = "{\"type\":\"object\"}", .ud = &aloop_ids[ALOOP_A],
      .start = aloop_start, .step = aloop_step, .stop = aloop_stop },
    { .struct_size = sizeof(AgcExtTool), .flags = AGENTC_TOOL_READONLY,
      .name = "aloop_b", .label = "b", .description = "batch tool b",
      .parameters_json = "{\"type\":\"object\"}", .ud = &aloop_ids[ALOOP_B],
      .start = aloop_start, .step = aloop_step, .stop = aloop_stop },
};

static int aloop_init(const AgcExtHost *host) {
    for (size_t i = 0; i < ALOOP_TOOLS; i++) host->add_tool(&aloop_tools[i]);
    return 0;
}

static int aloop_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    out->abi_version = AGENTC_EXT_ABI;
    out->struct_size = sizeof *out;
    out->name = "aloop";
    out->version = "0.1";
    out->order = 0;
    out->init = aloop_init;
    return 0;
}

/* A fresh registry and agent with the five fixture tools installed. */
static AgcAgent *aloop_setup(Replay *r, Trace *tr) {
    agentc_ext_shutdown();
    agentc_ext_adopt("aloop", aloop_entry);
    agentc_ext_load_all();
    AgcTool tools[ALOOP_TOOLS];
    size_t n = agentc_ext_tools(tools, ALOOP_TOOLS);
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, (AgcTransport){ replay_request, r, NULL });
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, tools, n);
    agentc_agent_test_no_backoff(a);
    if (tr) agentc_agent_set_events(a, collector, tr);
    aloop_reset();
    return a;
}

static void aloop_pump(void *ud, int timeout_ms) {
    (void)ud;
    (void)timeout_ms;
    g_aloop_pumps++;
    agentc_ext_pump();
}

/* true when `a` occurs before `z` in the trace. */
static bool trace_before(const AgcBuf *b, const char *a, const char *z) {
    if (!b->p) return false;
    i64 ka = agentc_str_find((const char *)b->p, b->len, a, agentc_strlen(a));
    i64 kz = agentc_str_find((const char *)b->p, b->len, z, agentc_strlen(z));
    return ka >= 0 && kz > ka;
}

static const char reply_aloop_ok[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"alu_1\",\"name\":\"aloop_ok\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
    "\"usage\":{\"output_tokens\":3}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static const char reply_aloop_slow[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"alu_2\",\"name\":\"aloop_slow\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
    "\"usage\":{\"output_tokens\":3}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static const char reply_aloop_abort[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"alu_3\",\"name\":\"aloop_abort\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
    "\"usage\":{\"output_tokens\":3}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

/* Two tool_use blocks in one assistant turn: source order drives both the
 * start order and the transcript order. */
static const char reply_aloop_pair[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"alu_a\",\"name\":\"aloop_a\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"alu_b\",\"name\":\"aloop_b\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
    "\"usage\":{\"output_tokens\":6}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

/* incremental steps, transcript text, one stop(FINISHED) */
static void test_async_loop_ok(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_aloop_ok;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    AgcAgent *a = aloop_setup(&r, &tr);

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("aloop_ok_rc", rc == 0);
    check("aloop_ok_requests", r.i == 2);
    check("aloop_ok_events",
          count_occurrences((const char *)tr.tr.p, "tool_start:aloop_ok:") == 1 &&
              count_occurrences((const char *)tr.tr.p, "tool_end:aloop_ok:") == 1);
    check("aloop_ok_steps", g_aloop_starts == 1 && g_aloop_stepped == 3);
    check("aloop_ok_stop_once",
          g_aloop_stops == 1 && g_aloop_stop_count[ALOOP_OK] == 1 &&
              g_aloop_stop_reason[ALOOP_OK] == AGENTC_EXT_TOOL_FINISHED);
    check("aloop_ok_transcript",
          tp && tp->n == 4 && tp->msgs[2].role == AGENTC_ROLE_TOOL &&
              agentc_streq(tp->msgs[2].blocks[0].text, "a0\na1\na2\n") &&
              agentc_streq(tp->msgs[2].blocks[0].tool_id, "alu_1") &&
              tp->msgs[2].error == NULL);
    check("aloop_ok_final",
          tp && tp->n == 4 && agentc_streq(tp->msgs[3].blocks[0].text, "all done") &&
              agentc_agent_last_error(a) == NULL);
    agentc_outs((const char *)tr.tr.p);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* a timeout is a tool-result error: the loop answers the call, records it and
 * runs a second provider turn (tool error != agent failure) */
static void test_async_loop_timeout(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_aloop_slow;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    AgcAgent *a = aloop_setup(&r, &tr);

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("aloop_timeout_rc", rc == 0);
    check("aloop_timeout_requests", r.i == 2);
    check("aloop_timeout_stop_once",
          g_aloop_stops == 1 && g_aloop_stop_count[ALOOP_SLOW] == 1 &&
              g_aloop_stop_reason[ALOOP_SLOW] == AGENTC_EXT_TOOL_TIMEOUT);
    check("aloop_timeout_tool_error",
          tp && tp->n == 4 && tp->msgs[2].role == AGENTC_ROLE_TOOL &&
              agentc_streq(tp->msgs[2].blocks[0].text,
                           "error: tool 'aloop_slow' timed out\n") &&
              tp->msgs[2].error != NULL);
    check("aloop_timeout_final",
          tp && tp->n == 4 && agentc_streq(tp->msgs[3].blocks[0].text, "all done") &&
              agentc_agent_last_error(a) == NULL);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* abort from inside a step: the driver fills the empty result with the
 * synthetic "error: aborted" and delivers exactly one stop(CANCELLED) */
static void test_async_loop_abort(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_aloop_abort;

    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    AgcAgent *a = aloop_setup(&r, &tr);
    g_aloop_agent = a;

    int rc = agentc_agent_submit(a, "go");
    g_aloop_agent = NULL;
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("aloop_abort_rc", rc == -1);
    check("aloop_abort_error",
          agentc_agent_last_error(a) &&
              agentc_streq(agentc_agent_last_error(a), "aborted"));
    check("aloop_abort_stop_once",
          g_aloop_stops == 1 && g_aloop_stop_count[ALOOP_ABORT] == 1 &&
              g_aloop_stop_reason[ALOOP_ABORT] == AGENTC_EXT_TOOL_CANCELLED);
    check("aloop_abort_result",
          tp && tp->n == 3 && tp->msgs[2].role == AGENTC_ROLE_TOOL &&
              agentc_streq(tp->msgs[2].blocks[0].text, "error: aborted") &&
              tp->msgs[2].error != NULL);
    check("aloop_abort_no_retry", r.i == 1);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* two async tools in one batch: source-ordered starts, then one interleaved
 * step per running job per driver tick (no wall-clock assumptions) */
static void test_async_loop_interleave(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_aloop_pair;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    AgcAgent *a = aloop_setup(&r, &tr);

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("aloop_pair_rc", rc == 0);
    check("aloop_pair_requests", r.i == 2);
    check("aloop_pair_interleave", buf_eq(&g_aloop_seq, "a0b0a1b1"));
    check("aloop_pair_starts",
          g_aloop_starts == 2 &&
              trace_before(&tr.tr, "tool_start:aloop_a:", "tool_start:aloop_b:"));
    check("aloop_pair_ends",
          trace_before(&tr.tr, "tool_end:aloop_a:", "tool_end:aloop_b:") &&
              g_aloop_stop_count[ALOOP_A] == 1 &&
              g_aloop_stop_count[ALOOP_B] == 1 &&
              g_aloop_stop_reason[ALOOP_A] == AGENTC_EXT_TOOL_FINISHED &&
              g_aloop_stop_reason[ALOOP_B] == AGENTC_EXT_TOOL_FINISHED);
    check("aloop_pair_transcript",
          tp && tp->n == 5 && tp->msgs[2].role == AGENTC_ROLE_TOOL &&
              tp->msgs[3].role == AGENTC_ROLE_TOOL &&
              agentc_streq(tp->msgs[2].blocks[0].text, "a0\na1\n") &&
              agentc_streq(tp->msgs[3].blocks[0].text, "b0\nb1\n") &&
              agentc_streq(tp->msgs[2].blocks[0].tool_id, "alu_a") &&
              agentc_streq(tp->msgs[3].blocks[0].tool_id, "alu_b"));
    agentc_outs((const char *)tr.tr.p);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* The adapter's D-A9 pump must be installed for the loop tests, exactly as
 * agentc_mode_setup installs it for the app. */
static void test_async_loop(void) {
    g_aloop_pumps = 0;
    agentc_pump_install(aloop_pump, NULL);
    test_async_loop_ok();
    test_async_loop_timeout();
    test_async_loop_abort();
    test_async_loop_interleave();
    agentc_pump_install(NULL, NULL);
    check("aloop_pumped", g_aloop_pumps > 0);
    agentc_ext_shutdown();
    agentc_buf_free(&g_aloop_seq);
}

/* A caller holding the pre-close assistant pointer must not read the freed
 * message array. close_tool_calls() pushes a tool result, which reallocs the
 * transcript when it is at capacity; this observer makes that use-after-free
 * deterministic without a sanitizer by reusing the just-freed old block (same
 * size class) and zeroing it. With the re-fetch fix the stale pointer is never
 * read, so the poisoned block is invisible and the real stop_reason survives. */
static AgcAgent *g_poison_agent;
static bool g_poisoned;

static void realloc_poison_observer(void *ud, const AgcMsg *m) {
    (void)ud;
    if (g_poisoned || !m || m->role != AGENTC_ROLE_TOOL) return;
    const AgcTranscript *tp = agentc_agent_transcript(g_poison_agent);
    if (!tp || tp->cap < 2) return;
    void *reuse = agentc_alloc((tp->cap / 2) * sizeof(AgcMsg));
    agentc_free(reuse);
    g_poisoned = true;
}

static void test_close_calls_realloc(void) {
    /* Six preloaded messages leave the transcript at n=6/cap=8. The submit
     * pushes user and assistant (n=8/cap=8), so close_tool_calls' tool push is
     * the one that grows the array to cap=16. */
    AgcTranscript pre;
    agentc_transcript_init(&pre);
    for (int i = 0; i < 6; i++) {
        AgcMsg *m = agentc_transcript_push(&pre, AGENTC_ROLE_USER);
        agentc_msg_add_text(m, "seed", 4);
    }

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_trunc;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;

    AgcTransport t = { replay_request, &r, NULL };
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_set_tools(a, &stub_tool, 1);
    agentc_agent_test_no_backoff(a);
    check("realloc_close_load", agentc_agent_load(a, &pre) == 0);
    agentc_transcript_free(&pre);
    Trace tr;
    agentc_memset(&tr, 0, sizeof tr);
    agentc_agent_set_events(a, collector, &tr);
    g_poison_agent = a;
    g_poisoned = false;
    agentc_agent_set_observer(a, realloc_poison_observer, NULL);
    g_agent_end_stop_seen = false;
    g_agent_end_stop = AGENTC_STOP_PENDING;

    int rc = agentc_agent_submit(a, "go");
    const AgcTranscript *tp = agentc_agent_transcript(a);
    check("realloc_close_rc", rc == 0);
    check("realloc_close_poisoned", g_poisoned);
    check("realloc_close_stop", g_agent_end_stop_seen &&
                                     g_agent_end_stop == AGENTC_STOP_LENGTH);
    check("realloc_close_shape",
          tp && tp->n == 9 && tp->cap >= 16 && tp->msgs[8].role == AGENTC_ROLE_TOOL);

    agentc_buf_free(&tr.tr);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
}

/* A nested .gitignore's anchored and multi-component rules are relative to the
 * directory that owns the file, not to the walk root. Before the fix the
 * walk-root-relative path never matched the real directory prefix, so
 * `sub/.gitignore` with `/secret.txt` and `foo/bar` hid nothing. */
static void test_nested_gitignore(void) {
    agentc_rm_rf("/tmp/agentc-agent-loop-ignore");
    (void)agentc_tool_write("/tmp/agentc-agent-loop-ignore/sub/.gitignore",
                            "/secret.txt\nfoo/bar\n", NULL);
    (void)agentc_tool_write("/tmp/agentc-agent-loop-ignore/sub/secret.txt", "needle\n", NULL);
    (void)agentc_tool_write("/tmp/agentc-agent-loop-ignore/sub/foo/bar", "needle\n", NULL);
    (void)agentc_tool_write("/tmp/agentc-agent-loop-ignore/sub/visible.txt", "needle\n", NULL);

    bool err = false;
    char *res = agentc_tool_find("*", "/tmp/agentc-agent-loop-ignore", 0, &err);
    check("nested_ignore_find_secret",
          !err && agentc_str_str(res, "secret.txt") == NULL);
    check("nested_ignore_find_multicomponent",
          !err && agentc_str_str(res, "bar") == NULL);
    check("nested_ignore_find_visible",
          !err && agentc_str_str(res, "visible.txt") != NULL);
    agentc_free(res);

    res = agentc_tool_grep("needle", "/tmp/agentc-agent-loop-ignore", NULL, false, true, 0, 0,
                           &err);
    check("nested_ignore_grep_secret",
          !err && agentc_str_str(res, "secret.txt") == NULL);
    check("nested_ignore_grep_multicomponent",
          !err && agentc_str_str(res, "bar") == NULL);
    check("nested_ignore_grep_visible",
          !err && agentc_str_str(res, "visible.txt") != NULL);
    agentc_free(res);

    agentc_rm_rf("/tmp/agentc-agent-loop-ignore");
}

/* Exponential backtracking in the grep regex (many `a*` groups against a long
 * run of `a`) must exhaust a step budget and report no match promptly instead
 * of hanging. */
static void test_grep_re_budget(void) {
    const char *root = "/tmp/agentc-agent-loop-grepbudget";
    agentc_rm_rf(root);
    AgcBuf line = { 0 };
    for (int i = 0; i < 400; i++) agentc_buf_byte(&line, 'a');
    agentc_buf_byte(&line, '\n');
    (void)agentc_tool_write("/tmp/agentc-agent-loop-grepbudget/run.txt", (const char *)line.p,
                            NULL);
    agentc_buf_free(&line);

    AgcBuf pat = { 0 };
    for (int i = 0; i < 24; i++) agentc_buf_cstr(&pat, "a*");
    agentc_buf_byte(&pat, 'b');
    bool err = false;
    char *res = agentc_tool_grep((const char *)pat.p, root, NULL, false, false, 0, 0, &err);
    check("grep_re_budget_nomatch", !err && res && agentc_streq(res, "no matches\n"));
    agentc_free(res);
    agentc_buf_free(&pat);

    /* A late match on a long single line is linear work, not backtracking, and
     * must not be rejected by the budget (the old flat cap failed near 500 KB). */
    const char *root2 = "/tmp/agentc-agent-loop-greplong";
    agentc_rm_rf(root2);
    AgcBuf longline = { 0 };
    for (int i = 0; i < 800000; i++) agentc_buf_byte(&longline, 'a');
    agentc_buf_cstr(&longline, "zzz\n");
    (void)agentc_tool_write("/tmp/agentc-agent-loop-greplong/run.txt",
                            (const char *)longline.p, NULL);
    agentc_buf_free(&longline);
    bool err2 = false;
    char *res2 = agentc_tool_grep("z+z", root2, NULL, false, false, 0, 0, &err2);
    check("grep_re_longline_match", !err2 && res2 && !agentc_streq(res2, "no matches\n"));
    agentc_free(res2);
    agentc_rm_rf(root2);
    agentc_rm_rf(root);
}

/* Defense in depth for the regex recursion: a pattern with one atom per
 * quantifier must not overflow the stack. Over-long patterns fall back to a
 * literal search (safe, linear), and re_here also caps recursion depth; both
 * paths must stay prompt. */
static void test_grep_re_huge_pattern(void) {
    const char *root = "/tmp/agentc-agent-loop-grephuge";
    agentc_rm_rf(root);
    (void)agentc_tool_write("/tmp/agentc-agent-loop-grephuge/run.txt", "aaaa\n", NULL);

    AgcBuf pat = { 0 };
    for (int i = 0; i < 100000; i++) agentc_buf_cstr(&pat, "a*");
    bool err = false;
    i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    char *res = agentc_tool_grep((const char *)pat.p, root, NULL, false, false, 0, 0, &err);
    i64 ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
    check("grep_re_huge_pattern",
          !err && res && agentc_streq(res, "no matches\n") && ms < 5000);
    agentc_free(res);
    agentc_buf_free(&pat);

    /* Under the length cap but with enough quantifiers to exceed the recursion
     * depth: still bounded, still no match, still prompt. */
    AgcBuf pat2 = { 0 };
    for (int i = 0; i < 2000; i++) agentc_buf_cstr(&pat2, "a*");
    agentc_buf_byte(&pat2, 'b');
    bool err2 = false;
    t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    char *res2 = agentc_tool_grep((const char *)pat2.p, root, NULL, false, false, 0, 0, &err2);
    ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
    check("grep_re_depth_nomatch",
          !err2 && res2 && agentc_streq(res2, "no matches\n") && ms < 5000);
    agentc_free(res2);
    agentc_buf_free(&pat2);

    agentc_rm_rf(root);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    test_tool_round_trip();
    test_retry_429();
    test_retry_dropped();
    test_retry_reset();
    test_http_error();
    test_abort();
    test_two_tools();
    test_abort_backfill();
    test_veto();
    test_veto_rewrite();
    test_terminate_all();
    test_terminate_mixed();
    test_terminate_continue();
    test_terminate_cancel();
    test_ext_payload();
    test_trunc_call_closed();
    test_error_calls_closed();
    test_abort_reset();
    test_tool_copy_ownership();
    test_hidden_tool_prompt();
    test_recompose_late_tool();
    test_recompose_prompt_refresh();
    test_input_transform();
    test_input_handled();
    test_before_agent_start();
    test_message_end_replace();
    test_message_end_invalid();
    test_message_end_terminal();
    test_message_end_terminal_aborted();
    test_message_end_tool_call();
    test_tool_result_override();
    test_tool_result_order();
    test_turn_end_continue();
    test_turn_end_entries_hard();
    test_turn_end_continue_trunc();
    test_settle_continue();
    test_settle_cap();
    test_agent_settled_once();
    test_provider_headers_patch();
    test_provider_request_replace();
    test_provider_block();
    test_provider_observe();
    test_model_select_same_provider();
    test_thinking_select();
    test_model_select_rebuild();
    test_http_cancel_pre();
    test_http_cancel_midstream();
    test_http_headers_json_escape();
    test_ext_provider_view();
    test_ext_provider_roundtrip();
    test_ext_provider_error();
    test_ext_provider_retry();
    test_ext_provider_cancel();
    test_ext_provider_unload();
    test_ext_discovery_auth();
    test_ext_provider_enumeration();
    test_async_loop();
    test_close_calls_realloc();
    test_nested_gitignore();
    test_grep_re_budget();
    test_grep_re_huge_pattern();

    return fails;
}
