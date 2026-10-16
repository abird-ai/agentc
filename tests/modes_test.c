/* modes_test.c — --mode json / --mode rpc driven with in-memory stdio and a
 * replaying transport. The golden output is a normalized event-type trace plus
 * boolean checks, so timing fields never leak into the expected file.
 *
 * The front ends share one setup path (src/app/mode.c): each test builds an
 * AgcModeCtx with agentc_mode_setup, injects the replay transport on
 * ctx.agent and drives the mode entry point.
 */
#include "agent.h"
#include "config.h"
#include "ext.h"
#include "plat.h"
#include "session.h"
#include "app/mode.h"
#include "core/prompts.h"

/* internal helper from core/config.c (not part of the frozen headers) */
char *agentc_read_file_owned(const char *path, size_t *len);
void agentc_agent_test_no_backoff(AgcAgent *a);

#define MODE_ROOT "/tmp/agentc-modes-test"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

static bool contains(const char *s, const char *needle) {
    return s && agentc_str_str(s, needle) != NULL;
}

/* True when a response for `id` echoes `cmd` with the given success value
 * (RPC v1 responses carry the command echo and machine code). */
static bool has_resp(const char *o, const char *id, const char *cmd, const char *success) {
    AgcBuf pat = {0};
    agentc_buf_printf(&pat, "\"id\":%s,\"command\":\"%s\",\"success\":%s", id, cmd, success);
    bool r = contains(o, (const char *)pat.p);
    agentc_buf_free(&pat);
    return r;
}

/* ------------------------------------------------------------ memory I/O */

typedef struct {
    const char *data;
    size_t len, pos;
} MemIn;

static int mem_read(void *ud, void *buf, size_t cap) {
    MemIn *m = ud;
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

/* A stand-in for print's on_event: --mode json must keep its own schema even
 * when the CLI also set print mode (-p). */
static void stub_event(void *ud, int ev, const void *data) {
    (void)ud;
    (void)ev;
    (void)data;
}

/* ------------------------------------------------------- replay transport */

typedef struct {
    const char *bodies[8];
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

/* Extract the first "type" value of every JSONL line, in order. */
static void trace_types(const char *jsonl, AgcBuf *out) {
    const char *p = jsonl;
    while (p && *p) {
        const char *nl = agentc_str_str(p, "\n");
        const char *key = agentc_str_str(p, "\"type\":\"");
        if (key && (!nl || key < nl)) {
            key += 8;
            const char *e = key;
            while (*e && *e != '"')
                e++;
            if (out->len)
                agentc_buf_byte(out, ',');
            agentc_buf_push(out, key, (size_t)(e - key));
        }
        if (!nl)
            break;
        p = nl + 1;
    }
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

static void fill_cfg(AgcModeConfig *c) {
    agentc_memset(c, 0, sizeof *c);
    c->provider = "anthropic";
    c->model = "claude-sonnet-4-5";
    c->api_key = "test";
    c->system = "sys";
    c->tools = &stub_tool;
    c->ntools = 1;
    c->max_attempts = 3;
    c->session.cwd = MODE_ROOT;
    c->session.memory_only = true;
    c->auto_compact = true;
    c->compact_reserve = 16384;
    c->compact_keep = 20000;
}

/* ------------------------------------------------------------ json mode */

static void test_json_mode(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.bodies[0] = reply_tools;
    r.bodies[1] = reply_final;

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.id = "json0001";
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, NULL, &out, stub_event, NULL, NULL, NULL};
    AgcModeCtx c;
    if (agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) != 0)
        fails = 1;
    agentc_agent_set_transport(c.agent, (AgcTransport){replay_request, &r, NULL});
    agentc_agent_test_no_backoff(c.agent);
    stub_calls = 0;

    int rc = agentc_mode_json_run(&c, "go");
    check("json_rc", rc == 0);
    check("json_requests", r.i == 2);
    check("json_tool_calls", stub_calls == 1);

    AgcBuf trace = {0};
    agentc_buf_byte(&out, 0); /* NUL-terminate for the C-string helpers */
    trace_types((const char *)out.p, &trace);
    agentc_outf("json_events=%s\n", trace.p ? (const char *)trace.p : "");
    const char *expect = "session,agent_start,turn_start,message_start,message_update,"
                         "message_end,tool_execution_start,tool_execution_end,turn_end,turn_start,"
                         "message_start,message_update,message_end,turn_end,agent_end";
    check("json_event_trace", trace.p && agentc_streq((const char *)trace.p, expect));
    check("json_delta_only", contains((const char *)out.p, "\"message_update\""));
    check("json_message_end", contains((const char *)out.p, "\"message_end\""));
    check("json_tool_result", contains((const char *)out.p, "stub({\\\"path\\\":\\\"f.txt\\\"})"));
    check("json_final_text", contains((const char *)out.p, "\"text\":\"all done\""));
    agentc_buf_free(&trace);

    /* the memory-only session has no file but the transcript was flushed */
    const AgcTranscript *tp = agentc_agent_transcript(c.agent);
    check("json_transcript", tp && tp->n == 4 && tp->msgs[3].role == AGENTC_ROLE_ASSISTANT);
    check("json_session_memory_only", agentc_session_path(c.session) == NULL);
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

/* ------------------------------------------------------------- rpc mode */

static void test_rpc_commands(void) {
    static const char commands[] =
        "{\"id\":1,\"command\":\"get_state\"}\n"
        "{\"id\":2,\"command\":\"get_available_models\"}\n"
        "{\"id\":\"three\",\"command\":\"set_thinking_level\",\"level\":\"high\"}\n"
        "{\"id\":4,\"command\":\"bash\",\"args\":{\"command\":\"echo hi\"}}\n"
        "{\"id\":5,\"command\":\"set_model\",\"model\":\"gpt-5\",\"provider\":\"openai\"}\n"
        "{\"id\":6,\"command\":\"get_state\"}\n"
        "{\"id\":7,\"command\":\"get_messages\"}\n"
        "{\"id\":8,\"command\":\"compact\"}\n"
        "{\"id\":9,\"command\":\"new_session\"}\n"
        "{\"id\":10,\"command\":\"get_messages\"}\n"
        "{\"id\":11,\"command\":\"bogus\"}\n";

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.id = "rpc00001";
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    if (agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) != 0)
        fails = 1;
    agentc_agent_test_no_backoff(c.agent);

    /* Keep this RPC trace independent of the host sandbox backend; trust_test
     * covers the real Landlock/Seatbelt paths. */
    os_sandbox_set_test_backend(2);
    int rc = agentc_mode_rpc_run(&c);
    os_sandbox_set_test_backend(0);
    agentc_buf_byte(&out, 0);
    check("rpc_rc", rc == 0);
    const char *o = (const char *)out.p;

    check("rpc_state1",
          has_resp(o, "1", "get_state", "true") && contains(o, "\"provider\":\"anthropic\"") &&
              contains(o, "\"model\":\"claude-sonnet-4-5\"") && contains(o, "\"messages\":0") &&
              contains(o, "\"pending_approvals\":0") && contains(o, "\"background_actors\":0"));
    check("rpc_models", has_resp(o, "2", "get_available_models", "true") &&
                            contains(o, "\"id\":\"gpt-5\"") &&
                            contains(o, "\"ctx_window\":400000"));
    check("rpc_thinking", has_resp(o, "\"three\"", "set_thinking_level", "true") &&
                              contains(o, "\"thinking_level\":4"));
    /* /bin/sh or the configured Windows shell (cmd.exe / PowerShell): `echo
     * hi` means the same in all three, though Windows output ends in CRLF. On
     * Windows the ambient-envelope fallback makes the spawn fail closed (no
     * sandbox backend), so the denial is the asserted behavior there. */
    bool win = agentc_streq(os_platform(), "windows");
    if (win) {
        check("rpc_bash", has_resp(o, "4", "bash", "true") && contains(o, "\"is_error\":true"));
    } else {
        check("rpc_bash", has_resp(o, "4", "bash", "true") && contains(o, "\"is_error\":false") &&
                              (contains(o, "hi\\n")));
    }
    check("rpc_set_model",
          has_resp(o, "5", "set_model", "true") && contains(o, "\"model\":\"gpt-5\""));
    check("rpc_state2", has_resp(o, "6", "get_state", "true") &&
                            contains(o, "\"model\":\"gpt-5\"") &&
                            contains(o, "\"provider\":\"openai\""));
    check("rpc_messages",
          has_resp(o, "7", "get_messages", "true") && contains(o, "\"messages\":[]"));
    check("rpc_compact_noop",
          has_resp(o, "8", "compact", "true") && contains(o, "\"compacted\":false"));
    check("rpc_new_session",
          has_resp(o, "9", "new_session", "true") && count_substr(o, "\"type\":\"session\"") == 2);
    check("rpc_new_session_clears",
          has_resp(o, "10", "get_messages", "true") && contains(o, "\"messages\":[]"));
    check("rpc_bogus", has_resp(o, "11", "bogus", "false") && contains(o, "unknown_command") &&
                           contains(o, "unknown command"));
    check("rpc_session_events", contains(o, "\"type\":\"session\""));

    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

/* `get_messages` is paged (C2): `limit`/`cursor`, `total`, `next_cursor?`. */
static void test_rpc_messages_paging(void) {
    static const char commands[] =
        "{\"id\":1,\"command\":\"get_messages\",\"limit\":2}\n"
        "{\"id\":2,\"command\":\"get_messages\",\"limit\":2,\"cursor\":2}\n"
        "{\"id\":3,\"command\":\"get_messages\",\"limit\":2,\"cursor\":99}\n";
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.id = "rpcpg001";
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    if (agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) != 0)
        fails = 1;
    agentc_agent_test_no_backoff(c.agent);
    AgcTranscript t;
    agentc_transcript_init(&t);
    for (int i = 0; i < 5; i++) {
        AgcMsg *m = agentc_transcript_push(&t, AGENTC_ROLE_USER);
        char txt[32];
        agentc_snprintf(txt, sizeof txt, "msg-%d", i);
        agentc_msg_add_text(m, txt, agentc_strlen(txt));
    }
    (void)agentc_agent_load(c.agent, &t);
    agentc_transcript_free(&t);
    int rc = agentc_mode_rpc_run(&c);
    agentc_buf_byte(&out, 0);
    check("rpc_page_rc", rc == 0);
    const char *o = (const char *)out.p;
    check("rpc_page_first", has_resp(o, "1", "get_messages", "true") &&
                                contains(o, "\"total\":5") && contains(o, "msg-0") &&
                                contains(o, "msg-1") && count_substr(o, "\"next_cursor\"") == 2);
    check("rpc_page_second", has_resp(o, "2", "get_messages", "true") &&
                                 contains(o, "\"next_cursor\":4") && contains(o, "msg-2") &&
                                 contains(o, "msg-3") && !contains(o, "msg-4"));
    check("rpc_page_clamped", has_resp(o, "3", "get_messages", "true") &&
                                  contains(o, "\"total\":5") && contains(o, "\"messages\":[]"));
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

/* `get_available_models` must not silently truncate: one provider alone can
 * exceed the old 32-entry buffer, so a model registered late must still appear. */
static void test_rpc_available_models_wide(void) {
    char id[48];
    for (int i = 0; i < 40; i++) {
        agentc_snprintf(id, sizeof id, "bulk-%d", i);
        agentc_model_register_dynamic("modes-bulk", id, "anthropic-messages", "http://x", 0, 0,
                                      false, false);
    }
    static const char commands[] = "{\"id\":1,\"command\":\"get_available_models\"}\n";
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = true;
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    if (agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) != 0) {
        check("mode.rpc.models_wide.setup", false);
    } else {
        int rc = agentc_mode_rpc_run(&c);
        agentc_buf_byte(&out, 0);
        check("mode.rpc.models_wide.rc", rc == 0);
        check("mode.rpc.models_wide.last", contains((const char *)out.p, "\"id\":\"bulk-39\""));
        agentc_mode_teardown(&c);
    }
    agentc_buf_free(&out);
    agentc_model_clear_dynamic("modes-bulk");
}

static void test_rpc_prompt_abort(void) {
    static const char commands[] = "{\"id\":1,\"command\":\"prompt\",\"text\":\"go\"}\n"
                                   "{\"id\":2,\"command\":\"abort\"}\n";

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = reply_final;

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.id = "rpc00002";
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    if (agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) != 0)
        fails = 1;
    agentc_agent_set_transport(c.agent, (AgcTransport){replay_request, &r, NULL});
    agentc_agent_test_no_backoff(c.agent);

    int rc = agentc_mode_rpc_run(&c);
    agentc_buf_byte(&out, 0);
    check("abort_rc", rc == 0);
    const char *o = (const char *)out.p;
    check("abort_response",
          has_resp(o, "1", "prompt", "true") && contains(o, "\"disposition\":\"started\""));
    check("abort_cmd_response", has_resp(o, "2", "abort", "true"));
    check("abort_event", contains(o, "\"type\":\"agent_settled\"") &&
                             contains(o, "\"disposition\":\"aborted\"") &&
                             contains(o, "\"type\":\"agent_end\""));

    const AgcTranscript *tp = agentc_agent_transcript(c.agent);
    check("abort_transcript", tp && tp->n == 2 && tp->msgs[1].stop_reason == AGENTC_STOP_ABORTED);

    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

/* --------------------------------------------------------- rpc prompt_template */

static char *tpl_expand(void *ud, const char *args) {
    (void)ud;
    AgcBuf b = {0};
    agentc_buf_printf(&b, "expanded(%s)", args ? args : "");
    return (char *)b.p;
}

static void test_rpc_prompt_template(void) {
    static const char commands[] =
        "{\"id\":1,\"command\":\"prompt_template\",\"name\":\"tpl\",\"args\":\"Alice\"}\n"
        "{\"id\":2,\"command\":\"prompt_template\",\"name\":\"nope\"}\n"
        "{\"id\":3,\"command\":\"prompt_template\"}\n";

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = reply_final;

    if (agentc_prompts_register("tpl", "test template", "", "modes-test", NULL, tpl_expand) != 0) {
        check("rpc.template.register", false);
        return;
    }
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.id = "rpc00004";
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    if (ok) {
        agentc_agent_set_transport(c.agent, (AgcTransport){replay_request, &r, NULL});
        agentc_agent_test_no_backoff(c.agent);
    }
    int rc = ok ? agentc_mode_rpc_run(&c) : -1;
    agentc_buf_byte(&out, 0);
    const char *o = (const char *)out.p;
    const AgcTranscript *tp = ok ? agentc_agent_transcript(c.agent) : NULL;
    bool expanded = tp && tp->n >= 1 && tp->msgs[0].nblocks == 1 &&
                    agentc_streq(tp->msgs[0].blocks[0].text, "expanded(Alice)");
    check("rpc.template.rc", rc == 0);
    check("rpc.template.submit", has_resp(o, "1", "prompt_template", "true") &&
                                     contains(o, "\"disposition\":\"started\"") && expanded);
    check("rpc.template.unknown",
          has_resp(o, "2", "prompt_template", "false") && contains(o, "unknown template"));
    check("rpc.template.missing",
          has_resp(o, "3", "prompt_template", "false") && contains(o, "missing name"));
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
    check("rpc.template.remove", agentc_prompts_remove("tpl"));
}

/* ------------------------------------------------- shared setup and session */

/* The mode pump must run while the RPC loop waits for the next command. A
 * reader that reports `would block` a few times lets the deferred queue drain
 * through the app-installed pump before the command arrives. */
static int g_rpc_defer_calls;
static void rpc_defer_cb(void *ud) {
    (void)ud;
    g_rpc_defer_calls++;
}

typedef struct {
    int would_blocks;
    const char *data;
    size_t len, pos;
} BlockingIn;

static int blocking_read(void *ud, void *buf, size_t cap) {
    BlockingIn *m = ud;
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

static void test_rpc_idle_pump(void) {
    static const char commands[] = "{\"id\":1,\"command\":\"get_state\"}\n";
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = true;
    BlockingIn in = {3, commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {blocking_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    if (agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) != 0) {
        check("mode.rpc.pump.setup", false);
        return;
    }
    g_rpc_defer_calls = 0;
    const AgcExtHost *h = agentc_ext_host();
    h->defer(h, rpc_defer_cb, NULL);
    int rc = agentc_mode_rpc_run(&c);
    agentc_buf_byte(&out, 0);
    check("mode.rpc.pump.rc", rc == 0);
    check("mode.rpc.pump.deferred", g_rpc_defer_calls == 1);
    check("mode.rpc.pump.replied", has_resp((const char *)out.p, "1", "get_state", "true"));
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

static void test_mode_setup_balanced(void) {
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/setup";
    cfg.session.id = "setup001";
    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);

    /* Warm the provider registry: its lazily created rows stay live for the
     * process and would otherwise count as leaked. */
    AgcModeCtx warm;
    if (agentc_mode_setup(&warm, &cfg, &io, AGENTC_MODE_F_SESSION) == 0)
        agentc_mode_teardown(&warm);

    size_t base = agentc_mem_live();
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    char path[4096] = "";
    bool has_path = false;
    if (ok && c.session && agentc_session_path(c.session)) {
        agentc_snprintf(path, sizeof path, "%s", agentc_session_path(c.session));
        has_path = true;
    }
    agentc_mode_teardown(&c);
    size_t live = agentc_mem_live();

    size_t len = 0;
    char *text = has_path ? agentc_read_file_owned(path, &len) : NULL;
    bool balanced = ok && has_path && text && contains(text, "\"type\":\"session\"") &&
                    live == base && c.agent == NULL && c.session == NULL && c.provider == NULL;
    agentc_free(text);
    check("mode.setup.balanced", balanced);
}

static void test_json_session_on(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.bodies[0] = reply_tools;
    r.bodies[1] = reply_final;

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/json";
    cfg.session.id = "json0002";
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, NULL, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    if (ok) {
        agentc_agent_set_transport(c.agent, (AgcTransport){replay_request, &r, NULL});
        agentc_agent_test_no_backoff(c.agent);
    }

    int rc = ok ? agentc_mode_json_run(&c, "go") : -1;
    const char *path = c.session ? agentc_session_path(c.session) : NULL;
    size_t len = 0;
    char *text = path ? agentc_read_file_owned(path, &len) : NULL;
    agentc_buf_byte(&out, 0);
    bool wrote = ok && rc == 0 && text && contains(text, "\"role\":\"user\"") &&
                 contains(text, "\"text\":\"all done\"") &&
                 contains((const char *)out.p, "\"type\":\"session\"");
    agentc_free(text);
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
    check("mode.json.session-on", wrote);
}

static void test_rpc_session_on(void) {
    static const char commands[] = "{\"id\":1,\"command\":\"get_state\"}\n"
                                   "{\"id\":2,\"command\":\"prompt\",\"text\":\"go\"}\n";

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = reply_final;

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/rpc";
    cfg.session.id = "rpc00003";
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    if (ok) {
        agentc_agent_set_transport(c.agent, (AgcTransport){replay_request, &r, NULL});
        agentc_agent_test_no_backoff(c.agent);
    }

    int rc = ok ? agentc_mode_rpc_run(&c) : -1;
    const char *path = c.session ? agentc_session_path(c.session) : NULL;
    size_t len = 0;
    char *text = path ? agentc_read_file_owned(path, &len) : NULL;
    agentc_buf_byte(&out, 0);
    bool wrote = ok && rc == 0 && text && contains(text, "\"text\":\"all done\"") &&
                 has_resp((const char *)out.p, "2", "prompt", "true") &&
                 contains((const char *)out.p, "\"disposition\":\"started\"") &&
                 contains((const char *)out.p, "\"type\":\"session\"");
    agentc_free(text);
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
    check("mode.rpc.session-on", wrote);
}

/* ------------------------------------------- persistence owner / switch */

/* Records a `session_before_switch` payload and returns `{"cancel":true}`. */
static int g_switch_before_calls;
static char g_switch_before_payload[160];

static int switch_before_cancel(void *ud, const char *point, const char *payload_json,
                                char **result_json) {
    (void)ud;
    (void)point;
    g_switch_before_calls++;
    size_t n = 0;
    while (payload_json && payload_json[n] && n < sizeof g_switch_before_payload - 1) {
        g_switch_before_payload[n] = payload_json[n];
        n++;
    }
    g_switch_before_payload[n] = 0;
    if (result_json)
        *result_json = agentc_strdup("{\"cancel\":true}");
    return 1;
}

/* Records `session_start` payloads. */
static int g_switch_start_calls;
static char g_switch_start_payload[1024];

static int switch_start_rec(void *ud, const char *point, const char *payload_json,
                            char **result_json) {
    (void)ud;
    (void)point;
    g_switch_start_calls++;
    size_t n = 0;
    while (payload_json && payload_json[n] && n < sizeof g_switch_start_payload - 1) {
        g_switch_start_payload[n] = payload_json[n];
        n++;
    }
    g_switch_start_payload[n] = 0;
    if (result_json)
        *result_json = NULL;
    return 0;
}

/* set_model rebuilds the agent; the transcript observer must stay bound to the
 * live mode ctx so a prompt after the swap still reaches the session file. */
static void test_rpc_set_model_persist(void) {
    static const char commands[] =
        "{\"id\":1,\"command\":\"set_model\",\"model\":\"claude-opus-4-1\"}\n"
        "{\"id\":2,\"command\":\"prompt\",\"text\":\"go\"}\n";

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = reply_final;

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/setmodel";
    cfg.session.id = "setm0001";
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION | AGENTC_MODE_F_ABORT) == 0;
    if (ok) {
        agentc_agent_set_transport(c.agent, (AgcTransport){replay_request, &r, NULL});
        agentc_agent_test_no_backoff(c.agent);
    }
    int rc = ok ? agentc_mode_rpc_run(&c) : -1;
    const char *path = c.session ? agentc_session_path(c.session) : NULL;
    size_t len = 0;
    char *text = path ? agentc_read_file_owned(path, &len) : NULL;
    agentc_buf_byte(&out, 0);
    check("mode.persist.set_model.rc", ok && rc == 0);
    check("mode.persist.set_model.reply", has_resp((const char *)out.p, "1", "set_model", "true") &&
                                              has_resp((const char *)out.p, "2", "prompt", "true"));
    check("mode.persist.set_model.wrote", text && contains(text, "\"text\":\"all done\""));
    agentc_free(text);
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

/* new_session swaps the persistence owner: the prompt after the switch lands in
 * the new file (never the closed old one) and the host context follows. */
static void test_rpc_new_session_persist(void) {
    static const char commands[] = "{\"id\":1,\"command\":\"new_session\"}\n"
                                   "{\"id\":2,\"command\":\"prompt\",\"text\":\"go\"}\n";

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = reply_final;

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/newsession";
    cfg.session.id = "switch001";
    cfg.session_spec = MODE_ROOT "/newsession/old.jsonl";
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION | AGENTC_MODE_F_ABORT) == 0;
    char old_path[4096] = "";
    if (ok && c.session && agentc_session_path(c.session))
        agentc_snprintf(old_path, sizeof old_path, "%s", agentc_session_path(c.session));
    if (ok) {
        agentc_agent_set_transport(c.agent, (AgcTransport){replay_request, &r, NULL});
        agentc_agent_test_no_backoff(c.agent);
    }
    int rc = ok ? agentc_mode_rpc_run(&c) : -1;
    const char *new_path = c.session ? agentc_session_path(c.session) : NULL;
    size_t nlen = 0;
    char *new_text = new_path ? agentc_read_file_owned(new_path, &nlen) : NULL;
    size_t olen = 0;
    char *old_text = old_path[0] ? agentc_read_file_owned(old_path, &olen) : NULL;
    const AgcExtHost *h = agentc_ext_host();
    bool swapped = ok && new_path && !agentc_streq(new_path, MODE_ROOT "/newsession/old.jsonl");
    bool context_follows = h->session_file(h) && new_path &&
                           agentc_streq(h->session_file(h), new_path) && h->session_id(h) &&
                           c.session &&
                           agentc_streq(h->session_id(h), agentc_session_id(c.session));
    agentc_buf_byte(&out, 0);
    check("mode.switch.new.rc", ok && rc == 0);
    check("mode.switch.new.swapped", swapped);
    check("mode.switch.new.wrote", new_text && contains(new_text, "\"text\":\"all done\""));
    check("mode.switch.new.old_untouched", old_text == NULL || !contains(old_text, "all done"));
    check("mode.switch.new.context_follows", context_follows);
    check("mode.switch.new.reply", has_resp((const char *)out.p, "1", "new_session", "true"));
    agentc_free(new_text);
    agentc_free(old_text);
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

/* Regression: the rebuild/rebind paths bind the transcript observer to the
 * RPC frame (`&r.base`), and that frame is dead once agentc_mode_rpc_run
 * returns. Appending after the run must still persist through the caller's
 * ctx. clobber_stack() reuses the dead frame's stack region with a poison
 * pattern so a stale binding cannot accidentally still work. */
static void clobber_stack(void) {
    volatile u8 poison[4096];
    for (size_t i = 0; i < sizeof poison; i++)
        poison[i] = 0xAB;
}

static void test_rpc_observer_after_run(void) {
    static const char commands[] =
        "{\"id\":1,\"command\":\"set_model\",\"model\":\"claude-opus-4-1\"}\n";

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/observer";
    cfg.session.id = "observ01";
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION | AGENTC_MODE_F_ABORT) == 0;
    if (ok) {
        Replay r;
        agentc_memset(&r, 0, sizeof r);
        agentc_agent_set_transport(c.agent, (AgcTransport){replay_request, &r, NULL});
        agentc_agent_test_no_backoff(c.agent);
    }
    int rc = ok ? agentc_mode_rpc_run(&c) : -1;
    check("mode.observer.rc", ok && rc == 0);

    /* The set_model command rebuilt the agent, so the observer was bound to
     * the now-dead RPC frame. The event callback is also frame-bound by design
     * (it is the run's own pump; production tears down after the run), so this
     * test replaces it with a no-op sink and isolates the observer rebind.
     * Poison the old frame, then append a message; the observer must persist it
     * into the caller's session. */
    agentc_agent_set_events(c.agent, stub_event, NULL);
    clobber_stack();
    agentc_agent_test_no_backoff(c.agent);
    (void)agentc_agent_submit(c.agent, "post-run");
    const char *path = c.session ? agentc_session_path(c.session) : NULL;
    size_t len = 0;
    char *text = path ? agentc_read_file_owned(path, &len) : NULL;
    check("mode.observer.persists", text && contains(text, "post-run"));
    agentc_free(text);
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

/* A cancelling session_before_switch handler leaves the session, the file and
 * the transcript untouched; the later prompt still persists to the old file. */
static void test_rpc_session_switch_cancel(void) {
    static const char commands[] = "{\"id\":1,\"command\":\"new_session\"}\n"
                                   "{\"id\":2,\"command\":\"prompt\",\"text\":\"go\"}\n";

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = reply_final;

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/switchcancel";
    cfg.session.id = "switch002";
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION | AGENTC_MODE_F_ABORT) == 0;
    char old_path[4096] = "";
    if (ok && c.session && agentc_session_path(c.session))
        agentc_snprintf(old_path, sizeof old_path, "%s", agentc_session_path(c.session));
    const AgcExtHost *h = agentc_ext_host();
    g_switch_before_calls = 0;
    g_switch_before_payload[0] = 0;
    uint64_t bh =
        ok ? h->on("session_before_switch", AGENTC_HOOK_OVERRIDE, 0, switch_before_cancel, NULL)
           : 0;
    if (ok) {
        agentc_agent_set_transport(c.agent, (AgcTransport){replay_request, &r, NULL});
        agentc_agent_test_no_backoff(c.agent);
    }
    int rc = ok ? agentc_mode_rpc_run(&c) : -1;
    const char *path = c.session ? agentc_session_path(c.session) : NULL;
    size_t len = 0;
    char *text = path ? agentc_read_file_owned(path, &len) : NULL;
    agentc_buf_byte(&out, 0);
    check("mode.switch.cancel.rc", ok && rc == 0);
    check("mode.switch.cancel.reply",
          has_resp((const char *)out.p, "1", "new_session", "false") &&
              contains((const char *)out.p, "session switch cancelled by extension"));
    check("mode.switch.cancel.same_session", path && agentc_streq(path, old_path));
    check("mode.switch.cancel.before_payload",
          g_switch_before_calls == 1 && contains(g_switch_before_payload, "\"reason\":\"new\""));
    check("mode.switch.cancel.prompt_persists", text && contains(text, "\"text\":\"all done\""));
    if (bh)
        h->off(bh);
    agentc_free(text);
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

/* ------------------------------------ sticky fail-closed switch regression */

/* Handler A for the sticky regression: contributes a well-formed
 * {"cancel":false} but returns 0, so POLICY_FIRST does not short-circuit the
 * chain and a later handler still gets to run. */
static int g_switch_allow_calls;
static int switch_before_allow(void *ud, const char *point, const char *payload_json,
                               char **result_json) {
    (void)ud;
    (void)point;
    (void)payload_json;
    g_switch_allow_calls++;
    if (result_json)
        *result_json = agentc_strdup("{\"cancel\":false}");
    return 0;
}

/* Handler variant claiming the decision (rc = 1): a single well-formed
 * {"cancel":false} must still allow the switch. */
static int switch_before_allow_handled(void *ud, const char *point, const char *payload_json,
                                       char **result_json) {
    (void)ud;
    (void)point;
    (void)payload_json;
    g_switch_allow_calls++;
    if (result_json)
        *result_json = agentc_strdup("{\"cancel\":false}");
    return 1;
}

/* Handler B: fails after A's decision was accumulated. */
static int g_switch_fail_calls;
static int switch_before_fail(void *ud, const char *point, const char *payload_json,
                              char **result_json) {
    (void)ud;
    (void)point;
    (void)payload_json;
    g_switch_fail_calls++;
    if (result_json)
        *result_json = NULL;
    return -1;
}

/* Handler B: overruns the fail-closed budget (AGENTC_EXT_BUDGET_NS is 50 ms). */
static int g_switch_overrun_calls;
static int switch_before_overrun(void *ud, const char *point, const char *payload_json,
                                 char **result_json) {
    (void)ud;
    (void)point;
    (void)payload_json;
    g_switch_overrun_calls++;
    if (result_json)
        *result_json = NULL;
    i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    while (os_now_ns(OS_CLOCK_MONOTONIC) - t0 < 80000000LL) {
    }
    return 0;
}

/* Regression (fail-closed precedence): A leaves a well-formed
 * {"cancel":false} in the accumulated emit result, then B fails (rc < 0) or
 * overruns. Both are fail-closed and sticky: the parsed earlier decision must
 * not overwrite `blocked` and re-open the switch. The new session file must not
 * appear and `session_start` must not fire. */
static void run_switch_sticky(const char *tag,
                              int (*second)(void *, const char *, const char *, char **),
                              int *second_calls) {
    static const char commands[] = "{\"id\":1,\"command\":\"new_session\"}\n"
                                   "{\"id\":2,\"command\":\"prompt\",\"text\":\"go\"}\n";
    char dir[128], spec[192], label[96];

    agentc_snprintf(dir, sizeof dir, MODE_ROOT "/switchsticky%s", tag);
    agentc_snprintf(spec, sizeof spec, "%s/old.jsonl", dir);

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = reply_final;

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = dir;
    cfg.session.id = "sticky01";
    cfg.session_spec = spec;
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION | AGENTC_MODE_F_ABORT) == 0;
    char old_path[4096] = "";
    if (ok && c.session && agentc_session_path(c.session))
        agentc_snprintf(old_path, sizeof old_path, "%s", agentc_session_path(c.session));
    size_t files_before = 0;
    char **before = agentc_session_list(dir, &files_before, 0);
    agentc_sessions_free(before, files_before);
    const AgcExtHost *h = agentc_ext_host();
    g_switch_allow_calls = 0;
    *second_calls = 0;
    g_switch_start_calls = 0;
    g_switch_start_payload[0] = 0;
    uint64_t ah =
        ok ? h->on("session_before_switch", AGENTC_HOOK_OVERRIDE, 0, switch_before_allow, NULL) : 0;
    uint64_t bh2 = ok ? h->on("session_before_switch", AGENTC_HOOK_OVERRIDE, 1, second, NULL) : 0;
    uint64_t sh = ok ? h->on("session_start", AGENTC_HOOK_OBSERVE, 0, switch_start_rec, NULL) : 0;
    if (ok) {
        agentc_agent_set_transport(c.agent, (AgcTransport){replay_request, &r, NULL});
        agentc_agent_test_no_backoff(c.agent);
    }
    int rc = ok ? agentc_mode_rpc_run(&c) : -1;
    const char *path = c.session ? agentc_session_path(c.session) : NULL;
    size_t flen = 0;
    char *text = path ? agentc_read_file_owned(path, &flen) : NULL;
    size_t files_after = 0;
    char **after = agentc_session_list(dir, &files_after, 0);
    agentc_sessions_free(after, files_after);
    agentc_buf_byte(&out, 0);

    agentc_snprintf(label, sizeof label, "mode.switch.sticky_%s.rc", tag);
    check(label, ok && rc == 0);
    agentc_snprintf(label, sizeof label, "mode.switch.sticky_%s.reply", tag);
    check(label, has_resp((const char *)out.p, "1", "new_session", "false") &&
                     contains((const char *)out.p, "session switch cancelled by extension"));
    agentc_snprintf(label, sizeof label, "mode.switch.sticky_%s.chain", tag);
    check(label, g_switch_allow_calls == 1 && *second_calls == 1);
    agentc_snprintf(label, sizeof label, "mode.switch.sticky_%s.same_session", tag);
    check(label, path && agentc_streq(path, old_path));
    agentc_snprintf(label, sizeof label, "mode.switch.sticky_%s.no_new_file", tag);
    check(label, files_after == files_before);
    agentc_snprintf(label, sizeof label, "mode.switch.sticky_%s.no_session_start", tag);
    check(label, g_switch_start_calls == 0);
    agentc_snprintf(label, sizeof label, "mode.switch.sticky_%s.prompt_persists", tag);
    check(label, text && contains(text, "\"text\":\"all done\""));

    if (ah)
        h->off(ah);
    if (bh2)
        h->off(bh2);
    if (sh)
        h->off(sh);
    agentc_free(text);
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

static void test_rpc_session_switch_sticky_fail(void) {
    run_switch_sticky("fail", switch_before_fail, &g_switch_fail_calls);
}

static void test_rpc_session_switch_sticky_overrun(void) {
    run_switch_sticky("overrun", switch_before_overrun, &g_switch_overrun_calls);
}

/* A lone override handler claiming the decision with a well-formed
 * {"cancel":false} allows the switch: the sticky blocked check must not swallow
 * a legitimate decision. */
static void test_rpc_session_switch_allow_handler(void) {
    static const char commands[] = "{\"id\":1,\"command\":\"new_session\"}\n";

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/switchallowhandler";
    cfg.session.id = "allow001";
    cfg.session_spec = MODE_ROOT "/switchallowhandler/old.jsonl";
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION | AGENTC_MODE_F_ABORT) == 0;
    char old_path[4096] = "";
    if (ok && c.session && agentc_session_path(c.session))
        agentc_snprintf(old_path, sizeof old_path, "%s", agentc_session_path(c.session));
    const AgcExtHost *h = agentc_ext_host();
    g_switch_allow_calls = 0;
    g_switch_start_calls = 0;
    g_switch_start_payload[0] = 0;
    uint64_t ah = ok ? h->on("session_before_switch", AGENTC_HOOK_OVERRIDE, 0,
                             switch_before_allow_handled, NULL)
                     : 0;
    uint64_t sh = ok ? h->on("session_start", AGENTC_HOOK_OBSERVE, 0, switch_start_rec, NULL) : 0;
    int rc = ok ? agentc_mode_rpc_run(&c) : -1;
    const char *new_path = c.session ? agentc_session_path(c.session) : NULL;
    agentc_buf_byte(&out, 0);
    check("mode.switch.allow_handler.rc", ok && rc == 0);
    check("mode.switch.allow_handler.reply",
          has_resp((const char *)out.p, "1", "new_session", "true"));
    check("mode.switch.allow_handler.decided", g_switch_allow_calls == 1);
    check("mode.switch.allow_handler.started_once",
          g_switch_start_calls == 1 && contains(g_switch_start_payload, "\"reason\":\"new\""));
    check("mode.switch.allow_handler.swapped",
          new_path && old_path[0] && !agentc_streq(new_path, old_path));
    if (ah)
        h->off(ah);
    if (sh)
        h->off(sh);
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

/* An allowing switch emits exactly one `session_start` with reason "new", the
 * new session id/file, and the old file as previous_session_file. */
static void test_rpc_session_switch_allow(void) {
    static const char commands[] = "{\"id\":1,\"command\":\"new_session\"}\n";

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/switchallow";
    cfg.session.id = "switch003";
    /* A deterministic old file so the timestamped new session cannot collide
     * with it inside the same millisecond. */
    cfg.session_spec = MODE_ROOT "/switchallow/old.jsonl";
    MemIn in = {commands, agentc_strlen(commands), 0};
    AgcBuf out = {0};
    AgcModeIo io = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION | AGENTC_MODE_F_ABORT) == 0;
    char old_path[4096] = "";
    if (ok && c.session && agentc_session_path(c.session))
        agentc_snprintf(old_path, sizeof old_path, "%s", agentc_session_path(c.session));
    const AgcExtHost *h = agentc_ext_host();
    g_switch_start_calls = 0;
    g_switch_start_payload[0] = 0;
    uint64_t sh = ok ? h->on("session_start", AGENTC_HOOK_OBSERVE, 0, switch_start_rec, NULL) : 0;
    int rc = ok ? agentc_mode_rpc_run(&c) : -1;
    const char *new_path = c.session ? agentc_session_path(c.session) : NULL;
    agentc_buf_byte(&out, 0);
    check("mode.switch.allow.rc", ok && rc == 0);
    check("mode.switch.allow.reply", has_resp((const char *)out.p, "1", "new_session", "true"));
    check("mode.switch.allow.started_once",
          g_switch_start_calls == 1 && contains(g_switch_start_payload, "\"reason\":\"new\""));
    check("mode.switch.allow.new_file", new_path && contains(g_switch_start_payload, new_path));
    check("mode.switch.allow.previous_file",
          old_path[0] && contains(g_switch_start_payload, "previous_session_file") &&
              contains(g_switch_start_payload, old_path));
    check("mode.switch.allow.swapped", new_path && !agentc_streq(new_path, old_path));
    if (sh)
        h->off(sh);
    agentc_mode_teardown(&c);
    agentc_buf_free(&out);
}

/* Root cause: the COMPACT event fires before compact_now() splices, so
 * the flush index must be set to the post-splice length (checkpoint + kept),
 * not to the pre-splice transcript length the event still sees. */
static void test_mode_compaction_flush(void) {
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/compactflush";
    cfg.session.id = "comp0001";

    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);
    AgcModeCtx c;
    if (agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) != 0) {
        check("mode.compact.flush.setup", false);
        return;
    }

    /* Five pre-compaction messages are persisted. */
    AgcTranscript pre;
    agentc_transcript_init(&pre);
    for (size_t i = 0; i < 5; i++) {
        AgcMsg *m = agentc_transcript_push(&pre, AGENTC_ROLE_USER);
        agentc_msg_add_text(m, "pre", 3);
    }
    (void)agentc_agent_load(c.agent, &pre);
    agentc_transcript_free(&pre);
    agentc_mode_session_flush(&c);
    check("mode.compact.flush.pre", c.flushed == 5);

    AgcCompactInfo ci;
    agentc_memset(&ci, 0, sizeof ci);
    ci.tokens_before = 1000;
    ci.kept_messages = 2;
    ci.summary = "summary";
    agentc_mode_event(&c, AGENTC_EV_COMPACT, &ci);
    check("mode.compact.flush.index", c.flushed == 3);

    /* Splice to checkpoint + 2 kept, then a new turn appends one more. */
    AgcTranscript post;
    agentc_transcript_init(&post);
    AgcMsg *cp = agentc_transcript_push(&post, AGENTC_ROLE_USER);
    agentc_msg_add_text(cp, "summary", 7);
    for (size_t i = 0; i < 2; i++) {
        AgcMsg *k = agentc_transcript_push(&post, AGENTC_ROLE_USER);
        agentc_msg_add_text(k, "kept", 4);
    }
    AgcMsg *nf = agentc_transcript_push(&post, AGENTC_ROLE_USER);
    agentc_msg_add_text(nf, "post-compaction", 15);
    (void)agentc_agent_load(c.agent, &post);
    agentc_transcript_free(&post);
    agentc_mode_session_flush(&c);

    const char *path = agentc_session_path(c.session);
    size_t len = 0;
    char *text = path ? agentc_read_file_owned(path, &len) : NULL;
    check("mode.compact.flush.persisted", text && contains(text, "post-compaction"));
    agentc_free(text);
    agentc_mode_teardown(&c);
}

/* A compaction splice shrinks the transcript below the recorded flush
 * index; without re-syncing, every later message is silently skipped. */
static void test_mode_flush_clamp(void) {
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/flushclamp";
    cfg.session.id = "flush001";

    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);
    AgcModeCtx c;
    if (agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) != 0) {
        check("mode.flush.clamp.setup", false);
        return;
    }

    /* Three pre-compaction messages are persisted; flushed == 3. */
    AgcTranscript pre;
    agentc_transcript_init(&pre);
    for (size_t i = 0; i < 3; i++) {
        AgcMsg *m = agentc_transcript_push(&pre, AGENTC_ROLE_USER);
        agentc_msg_add_text(m, "pre", 3);
    }
    (void)agentc_agent_load(c.agent, &pre);
    agentc_transcript_free(&pre);
    agentc_mode_session_flush(&c);
    check("mode.flush.clamp.pre", c.flushed == 3);

    /* Simulate the splice: the transcript shrinks to the checkpoint while the
     * recorded flush index is still the pre-splice count. */
    AgcTranscript post;
    agentc_transcript_init(&post);
    AgcMsg *cp = agentc_transcript_push(&post, AGENTC_ROLE_USER);
    agentc_msg_add_text(cp, "summary", 7);
    (void)agentc_agent_load(c.agent, &post);
    agentc_transcript_free(&post);
    check("mode.flush.clamp.stale", c.flushed == 3);
    agentc_mode_session_flush(&c);
    check("mode.flush.clamp.resync", c.flushed == 1);

    /* A message appended after the shrink must still reach the session. */
    AgcTranscript after;
    agentc_transcript_init(&after);
    AgcMsg *keep = agentc_transcript_push(&after, AGENTC_ROLE_USER);
    agentc_msg_add_text(keep, "summary", 7);
    AgcMsg *nf = agentc_transcript_push(&after, AGENTC_ROLE_USER);
    agentc_msg_add_text(nf, "after-compaction", 16);
    (void)agentc_agent_load(c.agent, &after);
    agentc_transcript_free(&after);
    agentc_mode_session_flush(&c);
    check("mode.flush.clamp.index", c.flushed == 2);

    const char *path = agentc_session_path(c.session);
    size_t len = 0;
    char *text = path ? agentc_read_file_owned(path, &len) : NULL;
    check("mode.flush.clamp.persisted", text && contains(text, "after-compaction"));
    agentc_free(text);
    agentc_mode_teardown(&c);
}

/* Regression: a failed/aborted assistant turn in the retained tail is not
 * persisted, so the checkpoint must record the number of PERSISTED tail
 * messages, not the core's in-memory kept count. Otherwise replay keeps too
 * many persisted messages and pulls a compacted prefix message back in. */
static void test_mode_compaction_failed_tail(void) {
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/compactfail";
    cfg.session.id = "cfail001";

    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);
    AgcModeCtx c;
    if (agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) != 0) {
        check("mode.compact.fail.setup", false);
        return;
    }

    /* Pre-splice transcript: a compacted prefix plus a retained tail whose
     * middle message is an aborted assistant turn (not persistable). */
    AgcTranscript pre;
    agentc_transcript_init(&pre);
    AgcMsg *p0 = agentc_transcript_push(&pre, AGENTC_ROLE_USER);
    agentc_msg_add_text(p0, "prefix", 6);
    AgcMsg *k0 = agentc_transcript_push(&pre, AGENTC_ROLE_USER);
    agentc_msg_add_text(k0, "keep-a", 6);
    AgcMsg *bad = agentc_transcript_push(&pre, AGENTC_ROLE_ASSISTANT);
    agentc_msg_add_text(bad, "aborted", 7);
    bad->stop_reason = AGENTC_STOP_ABORTED;
    AgcMsg *k1 = agentc_transcript_push(&pre, AGENTC_ROLE_USER);
    agentc_msg_add_text(k1, "keep-b", 6);
    (void)agentc_agent_load(c.agent, &pre);
    agentc_transcript_free(&pre);

    agentc_mode_session_flush(&c);
    check("mode.compact.fail.flushed", c.flushed == 4);

    /* The core retains the last 3 messages (keep-a, the aborted turn, keep-b);
     * only two of those reach the JSONL. */
    AgcCompactInfo ci;
    agentc_memset(&ci, 0, sizeof ci);
    ci.tokens_before = 1000;
    ci.kept_messages = 3;
    ci.summary = "summary";
    agentc_mode_event(&c, AGENTC_EV_COMPACT, &ci);
    check("mode.compact.fail.index", c.flushed == 4);

    const char *path = agentc_session_path(c.session);
    char *spath = path ? agentc_strdup(path) : NULL;
    size_t len = 0;
    char *text = spath ? agentc_read_file_owned(spath, &len) : NULL;
    check("mode.compact.fail.record", text && contains(text, "\"kept_messages\":2"));
    agentc_free(text);
    agentc_mode_teardown(&c);

    AgcSession *r = spath ? agentc_session_open(spath) : NULL;
    AgcTranscript tr;
    agentc_transcript_init(&tr);
    check("mode.compact.fail.reopen", r != NULL);
    check("mode.compact.fail.load", r != NULL && agentc_session_load_messages(r, &tr) == 0);
    /* summary + keep-a + keep-b; "prefix" must stay compacted. */
    check("mode.compact.fail.no_resurrect",
          tr.n == 3 && agentc_streq(tr.msgs[0].blocks[0].text, "summary") &&
              agentc_streq(tr.msgs[1].blocks[0].text, "keep-a") &&
              agentc_streq(tr.msgs[2].blocks[0].text, "keep-b"));
    agentc_transcript_free(&tr);
    if (r)
        agentc_session_close(r);
    agentc_free(spath);
}

/* ----------------------------------------------------------- app sinks */

static int g_sel_calls;
static char g_sel_payload[160];

static int select_rec(void *ud, const char *point, const char *payload_json, char **result_json) {
    (void)ud;
    (void)point;
    if (result_json)
        *result_json = NULL;
    g_sel_calls++;
    size_t n = 0;
    if (payload_json)
        while (payload_json[n] && n < sizeof g_sel_payload - 1) {
            g_sel_payload[n] = payload_json[n];
            n++;
        }
    g_sel_payload[n] = 0;
    return 0;
}

/* The app installs the model/thinking sinks in mode setup: a same-provider
 * switch reaches the agent directly, an unknown provider is rejected, a
 * provider switch is refused (-ENOSYS; RPC owns the idle rebuild), thinking
 * applies (idempotently), and teardown removes the services again. */
static void test_app_sinks(void) {
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = true;
    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);
    AgcModeCtx c;
    if (agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) != 0) {
        check("app.sinks.setup", false);
        return;
    }
    const AgcExtHost *h = agentc_ext_host();

    check("app.sinks.model_same_provider",
          h->set_model("anthropic", "claude-opus-4-1") == 0 &&
              agentc_streq(agentc_agent_transcript(c.agent)->model, "claude-opus-4-1"));
    check("app.sinks.model_empty_provider",
          h->set_model("", "claude-haiku-4-5") == 0 &&
              agentc_streq(agentc_agent_transcript(c.agent)->model, "claude-haiku-4-5"));
    check("app.sinks.model_empty", h->set_model("anthropic", "") == -22);
    check("app.sinks.model_unknown_provider", h->set_model("bogus", "x") == -22);
    check("app.sinks.model_unknown_keeps_state",
          agentc_streq(agentc_agent_transcript(c.agent)->model, "claude-haiku-4-5") &&
              agentc_streq(c.provider, "anthropic"));
    check("app.sinks.model_provider_refused", h->set_model("openai", "gpt-5") == -38);
    check("app.sinks.model_provider_keeps_state",
          agentc_streq(agentc_agent_transcript(c.agent)->provider, "anthropic") &&
              agentc_streq(agentc_agent_transcript(c.agent)->model, "claude-haiku-4-5") &&
              agentc_streq(c.provider, "anthropic"));

    /* thinking: the event proves the sink ran and the idempotent second call
     * proves the agent stored the level; unknown names are logged and ignored */
    g_sel_calls = 0;
    g_sel_payload[0] = 0;
    uint64_t th = h->on("thinking_level_select", AGENTC_HOOK_OBSERVE, 0, select_rec, NULL);
    h->set_thinking("high");
    check("app.sinks.thinking_applies",
          g_sel_calls == 1 &&
              agentc_streq(g_sel_payload, "{\"level\":\"high\",\"previous\":\"off\"}"));
    h->set_thinking("high");
    check("app.sinks.thinking_noop", g_sel_calls == 1);
    h->set_thinking("bogus");
    check("app.sinks.thinking_invalid", g_sel_calls == 1);
    h->set_thinking("low");
    check("app.sinks.thinking_low",
          g_sel_calls == 2 &&
              agentc_streq(g_sel_payload, "{\"level\":\"low\",\"previous\":\"high\"}"));
    h->off(th);

    agentc_mode_teardown(&c);
    check("app.sinks.cleared", h->set_model("anthropic", "x") == -38);
}

/* P6-B: the app binds the root agent actor and arms auto-start from config.
 * A turn-targeted item (turn 2) survives the first turn's safe point and is
 * auto-started into a second turn when the first settles (SIG §5.5). */
static void test_mode_root_actor(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.bodies[0] = reply_final;
    r.bodies[1] = reply_final;

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = true;
    AgcBuf out = {0};
    AgcModeIo io = {NULL, mem_write, NULL, &out, NULL, NULL, NULL, NULL};
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    AgcActorId id = ok ? agentc_agent_actor(c.agent) : 0;
    check("mode.actor.bound", ok && id != 0 && agentc_actor_exists(id));
    check("mode.actor.steerable", ok && agentc_agent_steerable(c.agent, NULL));
    if (ok) {
        agentc_agent_set_transport(c.agent, (AgcTransport){replay_request, &r, NULL});
        agentc_agent_test_no_backoff(c.agent);
        (void)agentc_queue_add(id, "auto-item", 2, 0, 0, NULL);
    }
    int rc = ok ? agentc_mode_json_run(&c, "go") : -1;
    const AgcTranscript *tp = ok ? agentc_agent_transcript(c.agent) : NULL;
    bool has = false;
    if (tp)
        for (size_t i = 0; i < tp->n; i++)
            for (size_t b = 0; b < tp->msgs[i].nblocks; b++)
                if (tp->msgs[i].blocks[b].text &&
                    agentc_str_str(tp->msgs[i].blocks[b].text, "auto-item"))
                    has = true;
    check("mode.autostart.rc", ok && rc == 0);
    check("mode.autostart.requests", r.i == 2);
    check("mode.autostart.turn", ok && agentc_agent_turn_no(c.agent) == 2);
    check("mode.autostart.text", has);
    agentc_mode_teardown(&c);
    check("mode.actor.released", id != 0 && !agentc_actor_exists(id));
    agentc_buf_free(&out);
}

/* SIG §5.6 / R-D11: queue mutations persist as `custom type:"queue"`
 * snapshots; a resume re-applies the last snapshot after re-validation and
 * drops invalid items with one NOTIFY. */
static void test_mode_queue_persist(void) {
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/queue";
    cfg.session.id = "queue001";
    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    AgcActorId id = ok ? agentc_agent_actor(c.agent) : 0;
    check("mode.queue.bound", id != 0);
    if (ok) {
        (void)agentc_queue_add(id, "keep-me", 1, 0, 0, "cli");
        (void)agentc_queue_add(id, "also-me", 2, 1, 0, NULL);
    }
    const char *path = c.session ? agentc_session_path(c.session) : NULL;
    char *p1 = path ? agentc_strdup(path) : NULL;
    agentc_mode_teardown(&c);
    size_t len = 0;
    char *text = p1 ? agentc_read_file_owned(p1, &len) : NULL;
    check("mode.queue.persisted", text && contains(text, "\"custom_type\":\"queue\"") &&
                                      contains(text, "keep-me") && contains(text, "also-me"));
    agentc_free(text);

    /* Append a final snapshot with one valid and one invalid item, then resume.
     * The record's actor_id is ignored; the live root actor is the target. */
    if (p1) {
        AgcSession *s = agentc_session_open(p1);
        static const char rec[] =
            "{\"type\":\"custom\",\"custom_type\":\"queue\",\"data\":{"
            "\"items\":[{\"turn\":3,\"priority\":0,\"flags\":0,\"text\":\"restored-ok\"},"
            "{\"turn\":-5,\"priority\":99,\"flags\":0,\"text\":\"\"}]}}";
        if (s) {
            (void)agentc_session_append_raw(s, rec, agentc_strlen(rec));
            agentc_session_close(s);
        }
    }

    AgcModeConfig cfg2;
    fill_cfg(&cfg2);
    cfg2.session.memory_only = false;
    cfg2.session.dir = MODE_ROOT "/queue";
    cfg2.session.id = "queue002";
    cfg2.session_spec = p1;
    AgcModeCtx c2;
    bool ok2 = p1 && agentc_mode_setup(&c2, &cfg2, &io, AGENTC_MODE_F_SESSION) == 0;
    AgcActorId id2 = ok2 ? agentc_agent_actor(c2.agent) : 0;
    check("mode.queue.restored_count", ok2 && agentc_queue_count(id2) == 1);
    AgcQueueItem it;
    bool got = id2 != 0 && agentc_queue_list(id2, &it, 1) == 1 &&
               agentc_streq(it.text, "restored-ok") && it.turn == 3;
    check("mode.queue.restored_item", got);
    check("mode.queue.dropped_notify", id2 != 0 && agentc_message_pending(id2) >= 1);
    agentc_mode_teardown(&c2);
    agentc_free(p1);
}

/* ------------------------------------------- persisted read-only agent tree */
static void test_persisted_agent_tree(void) {
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/ptree";
    cfg.session.id = "ptree001";
    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    const char *path = (ok && c.session) ? agentc_session_path(c.session) : NULL;
    char *p1 = path ? agentc_strdup(path) : NULL;
    agentc_mode_teardown(&c);

    size_t len = 0;
    char *text = p1 ? agentc_read_file_owned(p1, &len) : NULL;
    check("mode.ptree.root_record",
          text && contains(text, "\"custom_type\":\"agent\"") && contains(text, "\"op\":\"root\""));
    agentc_free(text);

    /* resume and read the tree through the RPC surface; nothing re-runs */
    static const char cmds[] =
        "{\"id\":1,\"command\":\"actor_query\",\"what\":\"persisted_agents\"}\n";
    MemIn in = {cmds, agentc_strlen(cmds), 0};
    AgcBuf out = {0};
    AgcModeIo io2 = {mem_read, mem_write, &in, &out, NULL, NULL, NULL, NULL};
    AgcModeConfig cfg2;
    fill_cfg(&cfg2);
    cfg2.session.memory_only = false;
    cfg2.session.dir = MODE_ROOT "/ptree";
    cfg2.session.id = "ptree002";
    cfg2.session_spec = p1;
    AgcModeCtx c2;
    bool ok2 = p1 && agentc_mode_setup(&c2, &cfg2, &io2, AGENTC_MODE_F_SESSION) == 0;
    int rc = ok2 ? agentc_mode_rpc_run(&c2) : -1;
    agentc_buf_byte(&out, 0);
    const char *o = (const char *)out.p;
    check("mode.ptree.resumed", ok2 && rc == 0);
    check("mode.ptree.readonly_view",
          contains(o, "\"persisted\":true") && contains(o, "\"op\":\"root\""));
    size_t pn = 0;
    (void)agentc_mode_persisted_agents(ok2 ? &c2 : NULL, &pn);
    check("mode.ptree.loaded", pn >= 1);
    if (ok2)
        agentc_mode_teardown(&c2);
    agentc_buf_free(&out);
    agentc_free(p1);
}

/* ------------------------------------------------ persisted notice replay */

typedef struct {
    char buf[256];
    size_t n;
} NoticeSink;

static void notice_sink(void *ud, const char *text) {
    NoticeSink *ns = ud;
    size_t l = agentc_strlen(text);
    if (ns->n + l >= sizeof ns->buf)
        l = sizeof ns->buf - 1 - ns->n;
    agentc_memcpy(ns->buf + ns->n, text, l);
    ns->n += l;
    ns->buf[ns->n] = 0;
}

/* A resumed session's `custom type:"notice"` rows stream back through
 * agentc_mode_replay_notices in file order (P6 completion pass 2c). */
static void test_notice_replay(void) {
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/notice";
    cfg.session.id = "notice01";
    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    if (ok && c.session) {
        static const char rec[] =
            "{\"type\":\"custom\",\"custom_type\":\"notice\",\"data\":{"
            "\"region\":1,\"turn\":2,\"step\":0,\"text\":\"compacted 12 -> 3\"}}";
        (void)agentc_session_append_raw(c.session, rec, agentc_strlen(rec));
        NoticeSink ns;
        agentc_memset(&ns, 0, sizeof ns);
        agentc_mode_replay_notices(&c, notice_sink, &ns);
        check("mode.notice.replay", agentc_streq(ns.buf, "compacted 12 -> 3"));
        agentc_mode_replay_notices(NULL, notice_sink, &ns); /* NULL-safe */
        check("mode.notice.null", true);
    } else {
        check("mode.notice.replay", false);
    }
    agentc_mode_teardown(&c);
}

/* P8-D1 (B-M8): the persistence owner rides the agent, so with two live
 * contexts A's route/queue records land in A's file, never B's (the old global
 * owner ended at the last setup = B). */
static void test_route_persist_owner(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.bodies[0] = reply_final;

    AgcModeConfig cfgA, cfgB;
    fill_cfg(&cfgA);
    cfgA.session.memory_only = false;
    cfgA.session.dir = MODE_ROOT "/rpowner_a";
    cfgA.session.id = "rpoa0001";
    cfgB = cfgA;
    cfgB.session.dir = MODE_ROOT "/rpowner_b";
    cfgB.session.id = "rpob0001";

    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);
    AgcModeCtx a, b;
    bool oka = agentc_mode_setup(&a, &cfgA, &io, AGENTC_MODE_F_SESSION) == 0;
    bool okb = agentc_mode_setup(&b, &cfgB, &io, AGENTC_MODE_F_SESSION) == 0;
    /* setup's trust load reloads the process model registry, so register the
     * alias only now, then select it on A's agent (setting the alias fact). */
    agentc_model_register_virtual("mrouter", "anthropic", "claude-sonnet-4-5");
    if (oka) {
        (void)agentc_agent_set_model(a.agent, "mrouter");
        agentc_agent_set_transport(a.agent, (AgcTransport){replay_request, &r, NULL});
        agentc_agent_test_no_backoff(a.agent);
        AgcActorId id = agentc_agent_actor(a.agent);
        (void)agentc_queue_add(id, "qa", 1, 0, 0, NULL);
        (void)agentc_agent_submit(a.agent, "hi");
    }
    const char *pa = (oka && a.session) ? agentc_session_path(a.session) : NULL;
    const char *pb = (okb && b.session) ? agentc_session_path(b.session) : NULL;
    char *ta = pa ? agentc_read_file_owned(pa, NULL) : NULL;
    char *tb = pb ? agentc_read_file_owned(pb, NULL) : NULL;
    check("mode.rpowner.a_route",
          ta && contains(ta, "\"custom_type\":\"model\"") && contains(ta, "mrouter"));
    check("mode.rpowner.a_queue",
          ta && contains(ta, "\"custom_type\":\"queue\"") && contains(ta, "qa"));
    check("mode.rpowner.b_clean", tb != NULL && !contains(tb, "mrouter") && !contains(tb, "qa"));
    agentc_free(ta);
    agentc_free(tb);
    agentc_mode_teardown(&a);
    agentc_mode_teardown(&b);
    agentc_model_clear_virtual();
}

/* P8-D4 (B-M1): a new/fork that cannot create its file is refused with -EIO;
 * the working session and its transcript stay intact. An explicitly requested
 * memory-only /new still works. */
static void test_new_session_refuses_degrade(void) {
    (void)agentc_mkdir_parents(MODE_ROOT "/degrade_marker/x");
    (void)agentc_write_file_atomic(MODE_ROOT "/degrade_blocker", "x", 1, 0644);

    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/degrade_ok";
    cfg.session.id = "degok001";
    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    char before_path[4096] = "";
    char before_id[16] = "";
    size_t before_n = 0;
    if (ok && c.session) {
        const char *p = agentc_session_path(c.session);
        if (p)
            agentc_snprintf(before_path, sizeof before_path, "%s", p);
        agentc_snprintf(before_id, sizeof before_id, "%s", agentc_session_id(c.session));
        const AgcTranscript *tr = agentc_agent_transcript(c.agent);
        before_n = tr ? tr->n : 0;
    }
    /* Every subsequent switch uses an uncreatable dir (a regular file is on the
     * path). The front ends read c->mcfg->session, so mutate the live config. */
    cfg.session.dir = MODE_ROOT "/degrade_blocker/sess";
    /* The refused create emits a timestamped warning; silence it for this
     * section so the golden stays deterministic (the contract under test is the
     * -EIO refusal, not the log line). Restore the default after. */
    agentc_log_set_level(4);
    int rc_new = agentc_mode_new_session(&c);
    agentc_log_set_level(-1);
    const AgcTranscript *tr = c.agent ? agentc_agent_transcript(c.agent) : NULL;
    check("mode.new.refuses_degrade",
          ok && rc_new == -5 && c.session && agentc_session_path(c.session) &&
              agentc_streq(agentc_session_path(c.session), before_path) &&
              agentc_streq(agentc_session_id(c.session), before_id) && tr && tr->n == before_n);
    agentc_log_set_level(4);
    int rc_fork = agentc_mode_fork_session(&c);
    agentc_log_set_level(-1);
    check("mode.fork.refuses_degrade",
          ok && rc_fork == -5 && c.session && agentc_session_path(c.session) &&
              agentc_streq(agentc_session_path(c.session), before_path));
    /* Control: a requested memory-only new still succeeds (path-less session). */
    cfg.session.memory_only = true;
    int rc_mem = agentc_mode_new_session(&c);
    check("mode.new.memory_ok", ok && rc_mem == 0 && c.session && agentc_session_path(c.session) == NULL);
    agentc_mode_teardown(&c);
}

/* P8-D4 (B-M3): a session switch owns its queue. /new clears; /resume replaces
 * (never merges) from the file's last snapshot; /fork keeps the live queue. */
static void test_session_switch_queue(void) {
    AgcModeConfig cfg;
    fill_cfg(&cfg);
    cfg.session.memory_only = false;
    cfg.session.dir = MODE_ROOT "/switchq";
    AgcModeIo io;
    agentc_memset(&io, 0, sizeof io);
    AgcModeCtx c;
    bool ok = agentc_mode_setup(&c, &cfg, &io, AGENTC_MODE_F_SESSION) == 0;
    AgcActorId id = ok ? agentc_agent_actor(c.agent) : 0;
    if (ok) {
        (void)agentc_queue_add(id, "a", 1, 0, 0, NULL);
        (void)agentc_queue_add(id, "b", 2, 0, 0, NULL);
    }
    const char *p0 = (ok && c.session) ? agentc_session_path(c.session) : NULL;
    char old_path[4096] = "";
    if (p0)
        agentc_snprintf(old_path, sizeof old_path, "%s", p0);

    int rc_new = agentc_mode_new_session(&c);
    check("mode.switchq.new_clears",
          ok && rc_new == 0 && agentc_queue_count(agentc_agent_actor(c.agent)) == 0);
    AgcActorId id2 = ok ? agentc_agent_actor(c.agent) : 0;
    if (ok && id2)
        (void)agentc_queue_add(id2, "c", 3, 0, 0, NULL);

    int rc_resume = ok ? agentc_mode_resume_session(&c, old_path) : -1;
    AgcActorId id3 = ok ? agentc_agent_actor(c.agent) : 0;
    AgcQueueItem items[AGENTC_QUEUE_MAX];
    int n = id3 ? agentc_queue_list(id3, items, AGENTC_QUEUE_MAX) : 0;
    bool texts = n == 2 && agentc_streq(items[0].text, "a") && agentc_streq(items[1].text, "b");
    check("mode.switchq.resume_replaces", ok && rc_resume == 0 && n == 2 && texts);

    /* /fork keeps its live queue (P8-D4 / R6). */
    if (ok && id3)
        (void)agentc_queue_add(id3, "d", 4, 0, 0, NULL);
    int rc_fork = ok ? agentc_mode_fork_session(&c) : -1;
    int n_fork = id3 ? agentc_queue_count(id3) : 0;
    const char *fp = (ok && c.session) ? agentc_session_path(c.session) : NULL;
    char *ft = fp ? agentc_read_file_owned(fp, NULL) : NULL;
    check("mode.switchq.fork_keeps",
          ok && rc_fork == 0 && n_fork == 3 && ft && contains(ft, "\"custom_type\":\"queue\"") &&
              contains(ft, "a") && contains(ft, "b") && contains(ft, "d"));
    agentc_free(ft);
    agentc_mode_teardown(&c);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* The Windows runner selects the shell per run; tests never load config
     * files, so apply the same environment override the product reads. */
    const char *shell = os_getenv("AGENTC_SHELL");
    if (shell && shell[0])
        agentc_config_set_shell(shell);
    test_json_mode();
    test_rpc_commands();
    test_rpc_messages_paging();
    test_rpc_available_models_wide();
    test_rpc_prompt_abort();
    test_rpc_prompt_template();
    test_rpc_idle_pump();
    test_mode_setup_balanced();
    test_json_session_on();
    test_rpc_session_on();
    test_rpc_set_model_persist();
    test_rpc_new_session_persist();
    test_rpc_observer_after_run();
    test_rpc_session_switch_cancel();
    test_rpc_session_switch_sticky_fail();
    test_rpc_session_switch_sticky_overrun();
    test_rpc_session_switch_allow_handler();
    test_rpc_session_switch_allow();
    test_mode_compaction_flush();
    test_mode_flush_clamp();
    test_mode_compaction_failed_tail();
    test_app_sinks();
    test_mode_root_actor();
    test_mode_queue_persist();
    test_notice_replay();
    test_persisted_agent_tree();
    test_route_persist_owner();
    test_new_session_refuses_degrade();
    test_session_switch_queue();
    return fails;
}
