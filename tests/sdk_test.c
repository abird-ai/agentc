/* sdk_test.c — pump-driven SDK run path (P5/P6 SDK gap).
 *
 * Drives one agent synchronously with `agentc_sdk_agent_submit` and one with
 * `agentc_sdk_agent_submit_async`/`agentc_sdk_agent_step` over an injected
 * replay transport, then asserts the transcript and result are identical and
 * that the asynchronous path actually yields across model steps. Also checks
 * that `agentc_sdk_agent_abort` from the host loop settles the run as aborted.
 *
 * Portable to the Windows golden suite: the transport is injected, so no socket
 * or host service is touched.
 */
#include "agentc_sdk.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

/* ----------------------------------------------------- replaying transport */

typedef struct {
    int codes[8];
    const char *bodies[8];
    size_t n, i;
    size_t requests;
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
    const char *resp = r->bodies[r->i];
    r->i++;
    r->requests++;
    if (code != 0)
        return code;
    if (resp) {
        size_t len = agentc_strlen(resp);
        for (size_t off = 0; off < len; off += 7) {
            size_t n = len - off;
            if (n > 7)
                n = 7;
            if (on_chunk(u, resp + off, n))
                return -125;
        }
    }
    return 0;
}

/* --------------------------------------------------------- stub tool */

static int stub_calls;

static char *stub_exec(const char *args_json, bool *is_error, const volatile bool *cancel) {
    (void)cancel;
    stub_calls++;
    if (is_error)
        *is_error = false;
    AgcBuf b = {0};
    agentc_buf_printf(&b, "stub(%s)", args_json ? args_json : "");
    return (char *)b.p;
}

static const AgcTool stub_tool = {.name = "read",
                                  .label = "Read",
                                  .desc = "stub read",
                                  .params_json = "{\"type\":\"object\"}",
                                  .flags = AGENTC_TOOL_READONLY,
                                  .exec = stub_exec};

/* ------------------------------------------------------------- responses */

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

/* --------------------------------------------------------------- helpers */

static void dump_transcript(const AgcTranscript *t, AgcBuf *out) {
    agentc_buf_printf(out, "n=%zu\n", t ? t->n : 0);
    if (!t)
        return;
    for (size_t i = 0; i < t->n; i++) {
        const AgcMsg *m = &t->msgs[i];
        agentc_buf_printf(out, "m%zu role=%d stop=%d blocks=%zu err=%s\n", i, m->role,
                          m->stop_reason, m->nblocks, m->error ? m->error : "-");
        for (size_t j = 0; j < m->nblocks; j++) {
            const AgcBlock *b = &m->blocks[j];
            agentc_buf_printf(out, " b%zu type=%d", j, b->type);
            if (b->text) {
                agentc_buf_cstr(out, " text=");
                agentc_buf_push(out, b->text, b->text_len);
            }
            if (b->tool_name) {
                agentc_buf_cstr(out, " tool=");
                agentc_buf_cstr(out, b->tool_name);
            }
            if (b->tool_id) {
                agentc_buf_cstr(out, " id=");
                agentc_buf_cstr(out, b->tool_id);
            }
            if (b->tool_args) {
                agentc_buf_cstr(out, " args=");
                agentc_buf_cstr(out, b->tool_args);
            }
            agentc_buf_byte(out, '\n');
        }
    }
}

static AgcSdkAgent *new_agent(Replay *r, const char *system) {
    AgcSdkAgentOptions o;
    agentc_memset(&o, 0, sizeof o);
    o.struct_size = (uint32_t)sizeof o;
    o.thinking = -1;
    o.provider = "anthropic";
    o.model = "claude-sonnet-4-5";
    o.api_key = "test";
    o.system = system;
    o.transport.request = replay_request;
    o.transport.ud = r;
    o.transport_set = true;
    o.tools = &stub_tool;
    o.ntools = 1;
    o.tools_set = true;
    AgcSdkAgent *a = agentc_sdk_agent_new(&o);
    if (!a)
        agentc_outf("agent_new_error=%s\n", agentc_sdk_last_error());
    return a;
}

static int sdk_start(void) {
    AgcSdkConfig cfg;
    agentc_memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = (uint32_t)sizeof cfg;
    cfg.log_level = 4; /* quiet: the golden is the API trace */
    return agentc_sdk_init(&cfg);
}

/* ---------------------------------------------------------- sync vs async */

static void test_sync_equals_async(void) {
    static const char system[] = "sys";

    /* Synchronous reference run. */
    int rc = sdk_start();
    check("init_ok", rc == 0);
    Replay rs;
    agentc_memset(&rs, 0, sizeof rs);
    rs.n = 2;
    rs.codes[0] = 0;
    rs.bodies[0] = reply_tools;
    rs.codes[1] = 0;
    rs.bodies[1] = reply_final;
    AgcSdkAgent *a = new_agent(&rs, system);
    check("sync_agent", a != NULL);
    stub_calls = 0;
    int sync_rc = agentc_sdk_agent_submit(a, "go");
    const AgcTranscript *st = agentc_sdk_agent_transcript(a);
    char sync_err[64];
    sync_err[0] = 0;
    const char *se = agentc_sdk_agent_last_error(a);
    if (se)
        agentc_snprintf(sync_err, sizeof sync_err, "%s", se);
    AgcBuf sync_dump = {0};
    dump_transcript(st, &sync_dump);
    int sync_tools = stub_calls;
    bool sync_active = agentc_sdk_agent_run_active(a);
    agentc_sdk_agent_free(a);
    agentc_sdk_shutdown();

    /* Asynchronous run over the same replay. */
    rc = sdk_start();
    check("init2_ok", rc == 0);
    Replay ra;
    agentc_memset(&ra, 0, sizeof ra);
    ra.n = 2;
    ra.codes[0] = 0;
    ra.bodies[0] = reply_tools;
    ra.codes[1] = 0;
    ra.bodies[1] = reply_final;
    a = new_agent(&ra, system);
    check("async_agent", a != NULL);
    stub_calls = 0;
    int begin_rc = agentc_sdk_agent_submit_async(a, "go");
    check("async_begin_ok", begin_rc == 0);
    check("async_active_after_begin", agentc_sdk_agent_run_active(a));
    int busy_rc = agentc_sdk_agent_submit_async(a, "again");
    check("async_busy", busy_rc == -16);
    int async_rc = begin_rc;
    int steps = 0;
    int last = AGENTC_AGENT_RUN_YIELD;
    while (last == AGENTC_AGENT_RUN_YIELD) {
        agentc_sdk_pump(a, 0);
        last = agentc_sdk_agent_step(a);
        steps++;
        if (steps > 16)
            break; /* bounded: never spin forever */
    }
    async_rc = last;
    const AgcTranscript *at = agentc_sdk_agent_transcript(a);
    char async_err[64];
    async_err[0] = 0;
    const char *ae = agentc_sdk_agent_last_error(a);
    if (ae)
        agentc_snprintf(async_err, sizeof async_err, "%s", ae);
    AgcBuf async_dump = {0};
    dump_transcript(at, &async_dump);
    int async_tools = stub_calls;
    bool async_active = agentc_sdk_agent_run_active(a);
    agentc_sdk_agent_free(a);
    agentc_sdk_shutdown();

    check("async_yielded", steps == 2);
    check("sync_rc_ok", sync_rc == 0);
    check("async_rc_ok", async_rc == 0);
    check("sync_error_none", sync_err[0] == 0);
    check("async_error_none", async_err[0] == 0);
    check("sync_active_cleared", !sync_active);
    check("async_active_cleared", !async_active);
    check("sync_tool_once", sync_tools == 1);
    check("async_tool_once", async_tools == 1);
    check("requests_equal", rs.requests == 2 && ra.requests == 2);
    bool same = sync_dump.len == async_dump.len &&
                (sync_dump.len == 0 || agentc_memeq(sync_dump.p, async_dump.p, sync_dump.len));
    check("transcript_equal", same);
    /* Print the reference transcript so the golden pins its exact shape. */
    agentc_outs("transcript=");
    if (sync_dump.p)
        agentc_out(sync_dump.p, sync_dump.len);
    agentc_out_nl();
    agentc_buf_free(&sync_dump);
    agentc_buf_free(&async_dump);
}

/* ------------------------------------------------------ abort from the loop */

static void test_abort_from_host_loop(void) {
    int rc = sdk_start();
    check("abort_init_ok", rc == 0);
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;
    AgcSdkAgent *a = new_agent(&r, "sys");
    check("abort_agent", a != NULL);
    int begin = agentc_sdk_agent_submit_async(a, "go");
    check("abort_begin_ok", begin == 0);
    int first = agentc_sdk_agent_step(a);
    check("abort_first_yield", first == AGENTC_AGENT_RUN_YIELD);
    agentc_sdk_agent_abort(a); /* from the host loop, between slices */
    int last = first;
    int steps = 0;
    while (last == AGENTC_AGENT_RUN_YIELD) {
        last = agentc_sdk_agent_step(a);
        steps++;
        if (steps > 16)
            break;
    }
    const char *err = agentc_sdk_agent_last_error(a);
    const AgcTranscript *t = agentc_sdk_agent_transcript(a);
    bool aborted_tail = t && t->n > 0 && t->msgs[t->n - 1].stop_reason == AGENTC_STOP_ABORTED;
    check("abort_rc", last < 0);
    check("abort_error", err && agentc_streq(err, "aborted"));
    check("abort_stop", aborted_tail);
    check("abort_not_active", !agentc_sdk_agent_run_active(a));
    check("abort_second_request_not_made", r.requests == 1);
    agentc_sdk_agent_free(a);
    agentc_sdk_shutdown();
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    test_sync_equals_async();
    test_abort_from_host_loop();
    agentc_outf("sdk tests: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
