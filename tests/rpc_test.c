/* rpc_test.c — deterministic RPC v1 record-layer trace.
 *
 * Drives agentc_mode_rpc_run with in-memory stdio and a replaying transport and
 * prints a normalized trace (one line per response/event/UI request) plus
 * focused checks. The golden is tests/data/rpc_test.expected. The trace strips
 * volatile fields (session ids/timestamps, model catalogs, message bodies) so
 * the schema and disposition vocabulary are asserted byte-for-byte.
 */
#include "agent.h"
#include "config.h"
#include "ext.h"
#include "plat.h"
#include "session.h"
#include "app/mode.h"
#include "core/prompts.h"

/* internal helper from core/agent.c (not part of the frozen headers) */
void agentc_agent_test_no_backoff(AgcAgent *a);

#define RPC_ROOT "/tmp/agentc-rpc-test"

static int fails;
static AgcBuf g_trace; /* normalized trace, printed after the run */

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

static bool contains(const char *s, const char *needle) {
    return s && agentc_str_str(s, needle) != NULL;
}

static int count_substr(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0])
        return 0;
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

/* Append one normalized trace row. */
#define tracef(...)                                                                                \
    do {                                                                                           \
        agentc_buf_printf(&g_trace, __VA_ARGS__);                                                  \
        agentc_buf_byte(&g_trace, '\n');                                                           \
    } while (0)

/* ------------------------------------------------------------ memory I/O */

typedef struct {
    const char *data;
    size_t len, pos;
    int would_blocks; /* -11 replies to hand before EOF (pumps deferred work) */
} MemIn;

static int mem_read(void *ud, void *buf, size_t cap) {
    MemIn *m = ud;
    if (m->would_blocks > 0) {
        m->would_blocks--;
        return -11;
    }
    if (m->pos >= m->len)
        return 0;
    size_t n = m->len - m->pos;
    if (n > cap)
        n = cap;
    agentc_memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return (int)n;
}

static void mem_write(void *ud, const void *buf, size_t n) {
    agentc_buf_push(ud, buf, n);
}

/* ------------------------------------------------------- replay transport */

typedef struct {
    const char *bodies[4];
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
    const char *resp = r->bodies[r->i++];
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

static const char reply_final[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"done\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":2}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

/* ---------------------------------------------------------- normalization */

/* Render the request id as a stable token: number, "string", or -. */
static void id_token(const AgcJson *o, AgcBuf *out) {
    AgcJson *jid = agentc_json_get(o, "id");
    int t = agentc_json_type(jid);
    if (t == AGENTC_JSON_NUM) {
        size_t n = 0;
        const char *s = agentc_json_num(jid, &n);
        agentc_buf_push(out, s, n);
    } else if (t == AGENTC_JSON_STR) {
        size_t n = 0;
        const char *s = agentc_json_str(jid, &n);
        agentc_buf_printf(out, "\"%s\"", s ? s : "");
    } else {
        agentc_buf_cstr(out, "-");
    }
}

/* One output line -> one normalized trace row. */
static void trace_row(const char *line, size_t n) {
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *o = agentc_json_parse_in(ja, line, n);
    const char *type = agentc_json_get_str(o, "type");
    if (agentc_streq(type, "response")) {
        AgcBuf id = {0};
        id_token(o, &id);
        const char *cmd = agentc_json_get_str(o, "command");
        const char *code = agentc_json_get_str(o, "code");
        tracef("response id=%s command=%s success=%d code=%s", id.p ? (const char *)id.p : "-",
               cmd ? cmd : "-", agentc_json_get_bool(o, "success", false) ? 1 : 0,
               code ? code : "-");
        agentc_buf_free(&id);
    } else if (agentc_streq(type, "agent_settled")) {
        const char *d = agentc_json_get_str(o, "disposition");
        const char *sr = agentc_json_get_str(o, "stop_reason");
        AgcJson *e = agentc_json_get(o, "error");
        tracef("settled disposition=%s stop=%s error=%s", d ? d : "-", sr ? sr : "-",
               agentc_json_type(e) == AGENTC_JSON_STR ? agentc_json_get_str(o, "error") : "-");
    } else if (agentc_streq(type, "ui_request")) {
        const char *m = agentc_json_get_str(o, "method");
        if (agentc_streq(m, "notify")) {
            tracef("ui method=notify message=%s level=%lld",
                   agentc_json_get_str(o, "message") ? agentc_json_get_str(o, "message") : "",
                   (long long)agentc_json_get_int(o, "level", 0));
        } else if (agentc_streq(m, "setStatus")) {
            const char *tx = agentc_json_get_str(o, "text");
            tracef("ui method=setStatus key=%s text=%s",
                   agentc_json_get_str(o, "key") ? agentc_json_get_str(o, "key") : "",
                   tx ? tx : "-");
        } else {
            tracef("ui method=setTitle title=%s",
                   agentc_json_get_str(o, "title") ? agentc_json_get_str(o, "title") : "");
        }
    } else {
        tracef("event %s", type ? type : "?");
    }
    agentc_json_arena_free(ja);
}

static void dump_trace(const AgcBuf *out) {
    const char *p = (const char *)out->p;
    size_t len = out->len, i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && p[i] != '\n')
            i++;
        if (i > start)
            trace_row(p + start, i - start);
        if (i < len)
            i++;
    }
}

/* ------------------------------------------------------------------ cases */

static char *prompt_expand(void *ud, const char *args) {
    (void)ud;
    AgcBuf b = {0};
    agentc_buf_printf(&b, "greet(%s)", args ? args : "");
    return (char *)b.p;
}

static void ui_defer(void *ud) {
    (void)ud;
    const AgcExtHost *h = agentc_ext_host();
    h->notify("hello-ui", 1);
    h->set_status("busy", "working");
    h->set_status("busy", ""); /* clear -> text null */
    h->set_title("rpc test");
}

static void fill_cfg(AgcModeConfig *c) {
    agentc_memset(c, 0, sizeof *c);
    c->provider = "anthropic";
    c->model = "claude-sonnet-4-5";
    c->api_key = "test";
    c->system = "sys";
    c->max_attempts = 1;
    c->session.cwd = RPC_ROOT;
    c->session.memory_only = true;
    c->auto_compact = true;
}

static void test_rpc_v1(void) {
    static const char commands[] =
        "{\"type\":\"hello\",\"id\":1}\n"
        "{\"id\":\"s1\",\"command\":\"get_state\"}\n"
        "{\"id\":3,\"command\":\"get_commands\"}\n"
        "{\"id\":4,\"command\":\"get_available_models\"}\n"
        "{\"command\":\"get_state\"}\n"
        "{\"type\":\"command\",\"command\":\"set_thinking_level\",\"id\":6,\"level\":\"high\"}\n"
        "{\"id\":7,\"command\":\"set_thinking_level\",\"thinking_level_name\":\"minimal\"}\n"
        "{\"id\":8,\"command\":\"set_thinking_level\",\"thinking_level\":2}\n"
        "{\"id\":9,\"command\":\"bash\",\"input\":\"echo hi\"}\n"
        "{\"id\":10,\"command\":\"get_messages\"}\n"
        "{\"id\":11,\"command\":\"compact\"}\n"
        "{\"id\":12,\"command\":\"bogus\"}\n"
        "not json\n"
        "{\"id\":14}\n"
        "{\"type\":\"ui_response\",\"id\":15,\"result\":{}}\n"
        "{\"id\":16,\"command\":\"get_settings\"}\n"
        "{\"id\":17,\"command\":\"prompt\",\"text\":\"first\"}\n"
        "{\"id\":18,\"command\":\"prompt\",\"text\":\"second\"}\n"
        "{\"id\":19,\"command\":\"get_settings\",\"ext\":\"nope\"}\n";

    if (agentc_prompts_register("greet", "say hi", "{name}", "rpc-test", NULL, prompt_expand) != 0)
        check("rpc.register", false);

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.bodies[0] = reply_final;
    r.bodies[1] = reply_final;

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    MemIn in = {commands, agentc_strlen(commands), 0, 2};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    if (ok) {
        agentc_agent_set_transport(c.agent, (AgcTransport){replay_request, &r, NULL});
        agentc_agent_test_no_backoff(c.agent);
        const AgcExtHost *h = agentc_ext_host();
        h->defer(h, ui_defer, NULL);
    }
    int rc = ok ? agentc_mode_rpc_run(&c) : -1;
    dump_trace(&out);
    agentc_buf_byte(&out, 0);
    agentc_outf("%s", g_trace.p ? (const char *)g_trace.p : "");
    const char *o = (const char *)out.p;

    check("rpc.rc", ok && rc == 0);
    check("rpc.hello", contains(o, "\"command\":\"hello\"") &&
                           contains(o, "\"protocol\":\"agentc-rpc\"") &&
                           contains(o, "\"version\":1") && contains(o, "\"capabilities\"") &&
                           contains(o, "\"get_commands\""));
    check("rpc.state_protocol", contains(o, "\"protocol\":\"agentc-rpc\""));
    check("rpc.state_name", contains(o, "\"thinking_level_name\":\"off\""));
    check("rpc.state_session_file", contains(o, "\"session_file\":null"));
    check("rpc.state_stream_flags", contains(o, "\"is_streaming\":false") &&
                                        contains(o, "\"pending_commands\":0") &&
                                        contains(o, "\"auto_compaction\":true"));
    check("rpc.get_commands", contains(o, "\"name\":\"greet\"") &&
                                  contains(o, "\"source\":\"rpc-test\"") &&
                                  contains(o, "\"hint\":\"{name}\""));
    check("rpc.models_total", contains(o, "\"total\":"));
    check("rpc.thinking_high", contains(o, "\"thinking_level_name\":\"high\""));
    check("rpc.thinking_reject_minimal", contains(o, "invalid_level"));
    check("rpc.thinking_reject_two", contains(o, "\"command\":\"set_thinking_level\""));
    bool win = agentc_streq(os_platform(), "windows");
    if (win) {
        check("rpc.bash", contains(o, "\"command\":\"bash\",\"success\":true") &&
                              contains(o, "\"is_error\":true"));
    } else {
        check("rpc.bash",
              contains(o, "\"command\":\"bash\",\"success\":true") && contains(o, "hi\\n"));
    }
    check("rpc.unknown", contains(o, "\"command\":\"bogus\"") && contains(o, "unknown_command"));
    check("rpc.invalid_json", contains(o, "\"command\":null") && contains(o, "invalid_json"));
    check("rpc.missing_command",
          contains(o, "\"id\":14,\"command\":null") && contains(o, "missing_command"));
    check("rpc.ui_dropped", agentc_str_str(o, "\"id\":15,\"command\"") == NULL);
    check("rpc.accept_started", contains(o, "\"id\":17,\"command\":\"prompt\"") &&
                                    contains(o, "\"disposition\":\"started\""));
    check("rpc.queued", contains(o, "\"id\":18,\"command\":\"prompt\"") &&
                            contains(o, "\"disposition\":\"queued\""));
    check("rpc.settled",
          contains(o, "\"disposition\":\"completed\"") && contains(o, "\"stop_reason\":\"stop\""));
    check("rpc.settled_count", count_substr((const char *)g_trace.p, "settled ") == 2);

    agentc_prompts_remove("greet");
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    test_rpc_v1();
    agentc_buf_free(&g_trace);
    return fails;
}
