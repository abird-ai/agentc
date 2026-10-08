/* mcp_test.c — offline tests for the MCP client (src/ext/mcp.c).
 *
 * Golden mode (no arguments, used by tests/run.sh):
 *   - ${ENV} expansion
 *   - config parsing (comments, trailing commas, disabled servers, env,
 *     headers, args, missing file)
 *   - tool-name sanitization and collision suffixes
 *   - JSON-RPC encode/decode against canned byte streams (initialize,
 *     notification, tools/list, tools/call, error, timeout, EOF, cancel)
 *   - tool annotation -> AGENTC_TOOL_* flag mapping
 *   - handed-out registry entries are never freed/reused; the frozen cap
 *     refuses further identities and fails the offending server
 *   - agentc_mem_live stability across registry/tools/shutdown cycles
 *
 * Live mode (tests/mcp.sh): `build/mcp_test --live` loads the config from
 * XDG_CONFIG_HOME and drives the real stdio transport against a mock server.
 */
#include "ext/mcp_int.h"
#include "base/limits.h"
#include "core/prompts.h"
#include "net/net_internal.h"
#include "ext.h"

/* shared test helpers from src/core/config.c */
void agentc_test_setenv(const char *name, const char *value);
void agentc_test_clearenv(void);
void agentc_rm_rf(const char *path);
int agentc_write_file_atomic(const char *path, const void *data, size_t len, int mode);

#define TEST_ROOT "/tmp/agentc-mcp-test"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

/* --------------------------------------------------------------- readers */
typedef struct {
    const char *data;
    size_t len, off;
    bool eof, eagain;
} MemRd;

static int mem_read(void *ud, void *p, size_t n) {
    MemRd *m = ud;
    if (m->eagain) return -11;
    if (m->off >= m->len) return m->eof ? 0 : -11;
    size_t take = m->len - m->off;
    if (take > n) take = n;
    agentc_memcpy(p, m->data + m->off, take);
    m->off += take;
    return (int)take;
}

static int mem_wait(void *ud, int timeout_ms) {
    (void)ud;
    (void)timeout_ms;
    return 0;
}

static int g_notifies;
static bool g_saw_list_changed;

static void on_notify(void *ud, const char *json, size_t n) {
    (void)ud;
    (void)n;
    g_notifies++;
    if (agentc_str_str(json, "list_changed")) g_saw_list_changed = true;
}

static McpCfg *cfg_find(AgcVec *v, const char *name) {
    for (size_t i = 0; i < v->len; i++) {
        McpCfg *c = &((McpCfg *)v->p)[i];
        if (agentc_streq(c->name, name)) return c;
    }
    return NULL;
}

static const AgcTool *find_tool(const AgcTool *tools, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++) {
        if (tools[i].name && agentc_streq(tools[i].name, name)) return &tools[i];
    }
    return NULL;
}

/* v2 tool invocation: run() appends to an AgcBuf and the helper returns the
 * owned, NUL-terminated text (the same contract as the old exec()). */
static char *run_tool_id(const AgcTool *t, const char *call_id, const char *args,
                         bool *is_error) {
    AgcToolCall call = { call_id, t->name, args ? args : "{}", NULL };
    AgcBuf out = { 0 };
    bool err = false;
    int rc = t->run(t, &call, &out, &err);
    if (is_error) *is_error = err || rc < 0;
    if (!out.p) agentc_buf_cstr(&out, "");
    return (char *)out.p;
}

static char *run_tool(const AgcTool *t, const char *args, bool *is_error) {
    return run_tool_id(t, "call-1", args, is_error);
}

/* ------------------------------------------------------------ test cases */
static void test_expand(void) {
    agentc_test_setenv("MCP_TOKEN", "sekret");
    agentc_test_setenv("MCP_EMPTY", "");
    char *s;
    s = agentc_mcp_expand_env("Bearer ${MCP_TOKEN}");
    check("expand.basic", agentc_streq(s, "Bearer sekret"));
    agentc_free(s);
    s = agentc_mcp_expand_env("${MCP_MISSING}x");
    check("expand.missing", agentc_streq(s, "x"));
    agentc_free(s);
    s = agentc_mcp_expand_env("$MCP_TOKEN");
    check("expand.nobrace", agentc_streq(s, "$MCP_TOKEN"));
    agentc_free(s);
    s = agentc_mcp_expand_env("a${MCP_EMPTY}b${MCP_UNKNOWN}c");
    check("expand.empty", agentc_streq(s, "abc"));
    agentc_free(s);
    s = agentc_mcp_expand_env("${MCP_TOKEN");
    check("expand.unterminated", agentc_streq(s, "${MCP_TOKEN"));
    agentc_free(s);
    s = agentc_mcp_expand_env("${}");
    check("expand.noname", agentc_streq(s, "${}"));
    agentc_free(s);
}

static void test_names(void) {
    char buf[192];
    agentc_mcp_name_sanitize("My Server.v2", agentc_strlen("My Server.v2"), buf, sizeof buf);
    check("name.sanitize", agentc_streq(buf, "my_server_v2"));
    agentc_mcp_tool_name("My.Server", "Get File(1)", buf, sizeof buf);
    check("name.tool", agentc_streq(buf, "mcp__my_server__get_file_1_"));
    check("name.hash",
          agentc_mcp_name_hash("a", "b") == agentc_mcp_name_hash("a", "b") &&
              agentc_mcp_name_hash("a", "b") != agentc_mcp_name_hash("a", "c"));

    agentc_mcp_registry_reset();
    (void)agentc_mcp_registry_add("a.b", "tool", "one", "{}", AGENTC_TOOL_READONLY);
    (void)agentc_mcp_registry_add("a_b", "tool", "two", NULL, 0);
    AgcTool tl[4];
    size_t n = agentc_mcp_tools(tl, 4);
    check("collision.count", n == 2);
    check("collision.first", n == 2 && agentc_streq(tl[0].name, "mcp__a_b__tool"));
    check("collision.second",
          n == 2 && agentc_str_starts(tl[1].name, agentc_strlen(tl[1].name), "mcp__a_b__tool_") &&
              !agentc_streq(tl[0].name, tl[1].name));
    check("flags.readonly", n == 2 && tl[0].flags == AGENTC_TOOL_READONLY && tl[1].flags == 0);
    check("tool.label", n == 2 && agentc_streq(tl[0].label, "mcp a.b"));
    check("tool.schema", n == 2 && agentc_streq(tl[0].params_json, "{}") &&
                             agentc_streq(tl[1].params_json, "{\"type\":\"object\"}"));
    check("tool.exec", n == 2 && tl[0].run != NULL && tl[1].run != NULL &&
                           tl[0].exec == NULL && tl[1].exec == NULL &&
                           tl[0].ud != tl[1].ud);
    agentc_mcp_registry_reset();
}

static void test_config(void) {
    agentc_test_setenv("MCP_PKG", "@scope/server");
    agentc_test_setenv("MCP_TOKEN", "tok");
    const char *cfg =
        "{\n"
        "  // servers\n"
        "  \"servers\": {\n"
        "    \"files\": { \"command\": \"npx\", \"args\": [\"-y\", \"${MCP_PKG}\",],\n"
        "                \"env\": {\"TOKEN\": \"${MCP_TOKEN}\", \"MISSING\": \"x${NOPE}y\"},\n"
        "                \"timeout_ms\": 1234, },\n"
        "    \"remote\": { \"url\": \"https://example.test/mcp?k=${MCP_TOKEN}\",\n"
        "                  \"headers\": {\"Authorization\": \"Bearer ${MCP_TOKEN}\"}, },\n"
        "    \"off\": { \"command\": \"/bin/true\", \"enabled\": false },\n"
        "  },\n"
        "}\n";
    AgcVec cfgs = { 0 };
    int rc = agentc_mcp_cfg_parse(cfg, agentc_strlen(cfg), &cfgs);
    check("cfg.parse", rc == 0 && cfgs.len == 3);
    McpCfg *files = cfg_find(&cfgs, "files");
    McpCfg *remote = cfg_find(&cfgs, "remote");
    McpCfg *off = cfg_find(&cfgs, "off");
    check("cfg.files", files && files->enabled && agentc_streq(files->command, "npx") &&
                           files->timeout_ms == 1234 && files->url == NULL);
    check("cfg.args", files && files->args.len == 2 &&
                         agentc_streq(((char **)files->args.p)[0], "-y") &&
                         agentc_streq(((char **)files->args.p)[1], "@scope/server"));
    check("cfg.env", files && files->env.len == 2 &&
                         agentc_streq(((char **)files->env.p)[0], "TOKEN=tok") &&
                         agentc_streq(((char **)files->env.p)[1], "MISSING=xy"));
    check("cfg.url", remote && agentc_streq(remote->url, "https://example.test/mcp?k=tok") &&
                         remote->enabled && remote->timeout_ms == 60000);
    check("cfg.headers",
          remote && remote->headers &&
              agentc_streq(remote->headers, "Authorization: Bearer tok\r\n"));
    check("cfg.disabled", off && !off->enabled && agentc_streq(off->command, "/bin/true"));
    agentc_mcp_cfgs_free(&cfgs);

    /* A header key that is not an RFC token (colon/space/CR/LF) is dropped
     * rather than emitted into the request block; an oversized timeout is
     * clamped to the 24h ceiling. */
    const char *hard =
        "{\"servers\":{\"hdr\":{\"url\":\"http://h\","
        "\"headers\":{\"Good\":\"1\",\"X: Injected\":\"2\",\"X.Api!Key\":\"3\"},"
        "\"timeout_ms\":9000000000000}}}\n";
    AgcVec hc = { 0 };
    check("cfg.hard_parse", agentc_mcp_cfg_parse(hard, agentc_strlen(hard), &hc) == 0 &&
                                hc.len == 1);
    McpCfg *hdr = cfg_find(&hc, "hdr");
    check("cfg.header_injection",
          hdr && hdr->headers &&
              agentc_streq(hdr->headers, "Good: 1\r\nX.Api!Key: 3\r\n"));
    check("cfg.timeout_clamp", hdr && hdr->timeout_ms == 86400000);
    agentc_mcp_cfgs_free(&hc);

    AgcVec bad = { 0 };
    check("cfg.malformed", agentc_mcp_cfg_parse("{ nope", 6, &bad) == -22 && bad.len == 0);
    check("cfg.empty", agentc_mcp_cfg_parse("", 0, &bad) == 0 && bad.len == 0);
    agentc_mcp_cfgs_free(&bad);
}

static void test_load_files(void) {
    agentc_rm_rf(TEST_ROOT);
    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");
    agentc_test_clearenv();
    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");

    check("load.missing", agentc_mcp_load(TEST_ROOT "/noproj", false) == 0);
    check("load.count0", agentc_mcp_server_count() == 0);
    check("load.tools0", agentc_mcp_tools(NULL, 0) == 0);
    agentc_mcp_shutdown();
    agentc_mcp_shutdown(); /* idempotent */

    const char *disabled =
        "{\"servers\":{\"a\":{\"command\":\"/bin/true\",\"enabled\":false},"
        "\"b\":{\"url\":\"https://example.test/mcp\",\"enabled\":false}}}\n";
    check("load.write",
          agentc_write_file_atomic(TEST_ROOT "/config/agentc/mcp.jsonc", disabled, agentc_strlen(disabled),
                               0644) == 0);
    check("load.disabled", agentc_mcp_load(TEST_ROOT, false) == 0);
    check("load.disabled_count", agentc_mcp_server_count() == 0);
    agentc_mcp_shutdown();

    const char *empty = "{\"servers\":{}}\n";
    check("load.empty_write",
          agentc_write_file_atomic(TEST_ROOT "/config/agentc/mcp.jsonc", empty, agentc_strlen(empty), 0644) ==
              0);
    check("load.empty", agentc_mcp_load(TEST_ROOT, false) == 0);
    agentc_mcp_shutdown();
    agentc_rm_rf(TEST_ROOT);
}

static void test_write_bounded(void) {
    /* The reader drains the 5-byte request (as the MCP read loop does); after
     * that nothing reads fds[0], so once the pipe buffer fills, the bounded
     * write must give up at the deadline instead of blocking the main loop.
     * The drain matters under Wine: its 4 KiB anonymous pipe would otherwise
     * block the write-first probe on the unread bytes. */
    int fds[2] = { -1, -1 };
    check("write.pipe", os_pipe(fds) == 0);
    if (fds[0] < 0 || fds[1] < 0) {
        check("write.small", false);
        check("write.timeout", false);
        check("write.cancel", false);
        return;
    }
    check("write.small", agentc_mcp_write_all(fds[1], 0, "hello", 5, 200, NULL, NULL) == 0);
    u8 drained[16];
    while (os_read(fds[0], drained, sizeof drained) > 0) { }
    size_t n = 1u << 20;
    u8 *buf = agentc_alloc(n);
    agentc_memset(buf, 'x', n);
    volatile bool cancel = false;
    i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    int rc = agentc_mcp_write_all(fds[1], 0, buf, n, 200, &cancel, NULL);
    i64 ms = (os_now_ns(OS_CLOCK_MONOTONIC) - t0) / 1000000;
    check("write.timeout", rc == -110 && ms < 5000);
    cancel = true;
    check("write.cancel", agentc_mcp_write_all(fds[1], 0, buf, n, 200, &cancel, NULL) == -125);
    agentc_free(buf);
    os_close(fds[0]);
    os_close(fds[1]);
}

static void test_handed_out(void) {
    /* An entry handed to the app is immutable: a later re-sync that changes a
     * description retires the old identity (keeping its strings readable) and
     * publishes a new one; executing the retired entry is a clean error. */
    agentc_mcp_registry_reset();
    (void)agentc_mcp_registry_add("srv", "echo", "one", "{\"type\":\"object\"}",
                                  AGENTC_TOOL_READONLY);
    AgcTool tl[4];
    size_t n = agentc_mcp_tools(tl, 4);
    check("handed.count", n == 1);
    const char *desc_before = n == 1 ? tl[0].desc : NULL;
    void *ud_before = n == 1 ? tl[0].ud : NULL;
    (void)agentc_mcp_registry_add("srv", "echo", "two", "{\"type\":\"object\"}",
                                  AGENTC_TOOL_READONLY);
    AgcTool tl2[4];
    size_t n2 = agentc_mcp_tools(tl2, 4);
    check("handed.old_stable", desc_before && agentc_streq(desc_before, "one"));
    check("handed.new_desc", n2 == 1 && agentc_streq(tl2[0].desc, "two"));
    check("handed.new_ud", n2 == 1 && tl2[0].ud != ud_before);
    bool err = false;
    char *res = n == 1 ? run_tool(&tl[0], "{}", &err) : NULL;
    check("handed.retired_error",
          err && res && agentc_str_str(res, "no longer available") != NULL);
    agentc_free(res);
    /* Churn must never recycle the retired block: its strings stay readable
     * and every fresh identity is a different AgcTool.ud. */
    for (int i = 0; i < 8; i++) {
        char desc[32];
        agentc_snprintf(desc, sizeof desc, "v%d", i);
        (void)agentc_mcp_registry_add("srv", "echo", desc, "{\"type\":\"object\"}",
                                      AGENTC_TOOL_READONLY);
        AgcTool ct[2];
        (void)agentc_mcp_tools(ct, 2);
    }
    AgcTool tlf[2];
    size_t nf = agentc_mcp_tools(tlf, 2);
    check("handed.never_reused",
          nf == 1 && tlf[0].ud != ud_before && desc_before &&
              agentc_streq(desc_before, "one"));
    agentc_mcp_registry_reset();
}

static void test_rpc(void) {
    char *req = agentc_mcp_rpc_request(7, "initialize", "{}");
    check("rpc.request",
          agentc_streq(
              req,
              "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"initialize\",\"params\":{}}\n"));
    agentc_free(req);
    char *ntf = agentc_mcp_rpc_notify("notifications/initialized", NULL);
    check("rpc.notify",
          agentc_streq(ntf, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"));
    agentc_free(ntf);

    /* chunked line framing */
    AgcBuf in = { 0 };
    char *msg = NULL;
    agentc_buf_cstr(&in, "{\"a\":1}");
    check("stdio.partial", agentc_mcp_stdio_next(&in, &msg) == 0);
    agentc_buf_cstr(&in, "\n{\"b\":2}\n");
    check("stdio.first", agentc_mcp_stdio_next(&in, &msg) == 1 && agentc_streq(msg, "{\"a\":1}"));
    agentc_free(msg);
    check("stdio.second", agentc_mcp_stdio_next(&in, &msg) == 1 && agentc_streq(msg, "{\"b\":2}"));
    agentc_free(msg);
    check("stdio.drained", agentc_mcp_stdio_next(&in, &msg) == 0);
    agentc_buf_free(&in);

    const char *stream =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":99,\"result\":{\"stale\":true}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2024-11-05\"}}\n";
    MemRd rd = { stream, agentc_strlen(stream), 0, false, false };
    McpReader reader = { mem_read, mem_wait, &rd };
    AgcBuf buf = { 0 };
    char *out = NULL;
    g_notifies = 0;
    g_saw_list_changed = false;
    int rc = agentc_mcp_rpc_await(&reader, &buf, 1, 1000, NULL, on_notify, NULL, &out);
    check("rpc.await", rc == 0 && out && agentc_str_str(out, "2024-11-05") != NULL);
    check("rpc.notify_seen", g_notifies == 1 && g_saw_list_changed);
    check("rpc.consume", buf.len == 0);
    agentc_free(out);
    agentc_buf_free(&buf);

    const char *errstream =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32601,\"message\":\"nope\"}}\n";
    MemRd rd2 = { errstream, agentc_strlen(errstream), 0, false, false };
    McpReader reader2 = { mem_read, mem_wait, &rd2 };
    out = NULL;
    rc = agentc_mcp_rpc_await(&reader2, &buf, 2, 1000, NULL, NULL, NULL, &out);
    char *text = NULL;
    bool is_err = false;
    int pr = out ? agentc_mcp_parse_call(out, agentc_strlen(out), &text, &is_err) : -1;
    check("rpc.error_response",
          rc == 0 && pr == 0 && is_err && text && agentc_streq(text, "MCP error -32601: nope"));
    agentc_free(text);
    agentc_free(out);
    agentc_buf_free(&buf);

    MemRd quiet = { NULL, 0, 0, false, true };
    McpReader rq = { mem_read, mem_wait, &quiet };
    out = NULL;
    rc = agentc_mcp_rpc_await(&rq, &buf, 3, 20, NULL, NULL, NULL, &out);
    check("rpc.timeout", rc == -110 && out == NULL);
    agentc_buf_free(&buf);

    MemRd eof = { NULL, 0, 0, true, false };
    McpReader re = { mem_read, mem_wait, &eof };
    out = NULL;
    rc = agentc_mcp_rpc_await(&re, &buf, 4, 1000, NULL, NULL, NULL, &out);
    check("rpc.eof", rc == -104);
    agentc_buf_free(&buf);

    bool cancel = true;
    MemRd never = { NULL, 0, 0, false, true };
    McpReader rn = { mem_read, mem_wait, &never };
    out = NULL;
    rc = agentc_mcp_rpc_await(&rn, &buf, 5, 1000, &cancel, NULL, NULL, &out);
    check("rpc.cancel", rc == -125);
    agentc_buf_free(&buf);
}

static void test_line_cap(void) {
    /* a > 1 MiB line is rejected while accumulating... */
    const size_t cap = AGENTC_LIMIT_MCP_LINE_BYTES;
    AgcBuf in = { 0 };
    char *msg = NULL;
    for (size_t i = 0; i <= cap; i++) agentc_buf_byte(&in, 'a');
    check("safety.mcp.line-cap",
          agentc_mcp_stdio_next(&in, &msg) == -7 && msg == NULL);
    /* ...and once the newline makes it a complete over-long line */
    agentc_buf_byte(&in, '\n');
    check("safety.mcp.line-cap-newline",
          agentc_mcp_stdio_next(&in, &msg) == -7 && msg == NULL);
    agentc_buf_free(&in);

    /* the incremental read path stops once the buffer would exceed the cap */
    AgcBuf big = { 0 };
    for (size_t i = 0; i < cap + 1; i++) agentc_buf_byte(&big, 'a');
    MemRd rd = { (const char *)big.p, big.len, 0, false, false };
    McpReader reader = { mem_read, mem_wait, &rd };
    AgcBuf buf = { 0 };
    char *out = NULL;
    int rc = agentc_mcp_rpc_await(&reader, &buf, 1, 1000, NULL, NULL, NULL, &out);
    check("safety.mcp.line-cap-read", rc == -7 && out == NULL);
    agentc_free(out);
    agentc_buf_free(&buf);
    agentc_buf_free(&big);

    /* an oversized inputSchema must never be cut mid-JSON: fall back to a
     * minimal valid object rather than embedding a truncated document */
    AgcBuf tools = { 0 };
    agentc_buf_cstr(&tools,
                    "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
                    "{\"name\":\"big\",\"inputSchema\":{\"pad\":\"");
    for (size_t i = 0; i < AGENTC_LIMIT_MCP_SCHEMA_BYTES + 64; i++)
        agentc_buf_byte(&tools, 'x');
    agentc_buf_cstr(&tools, "\"}}]}}\n");
    AgcVec tv = { 0 };
    rc = agentc_mcp_parse_tools((const char *)tools.p, tools.len, &tv, NULL);
    McpParsedTool *t = tv.len == 1 ? &((McpParsedTool *)tv.p)[0] : NULL;
    check("safety.mcp.schema-cap",
          rc == 0 && t && t->schema && agentc_streq(t->schema, "{\"type\":\"object\"}"));
    agentc_mcp_parsed_free(&tv);
    agentc_buf_free(&tools);

    /* a non-object inputSchema falls back too (providers embed it as an object) */
    const char *bad_schema =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"tools\":["
        "{\"name\":\"bad\",\"inputSchema\":[1,2,3]}]}}\n";
    AgcVec bv = { 0 };
    rc = agentc_mcp_parse_tools(bad_schema, agentc_strlen(bad_schema), &bv, NULL);
    McpParsedTool *bt = bv.len == 1 ? &((McpParsedTool *)bv.p)[0] : NULL;
    check("safety.mcp.schema-nonobj",
          rc == 0 && bt && bt->schema && agentc_streq(bt->schema, "{\"type\":\"object\"}"));
    agentc_mcp_parsed_free(&bv);
}

static void test_parse_tools(void) {
    const char *tools_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
        "{\"name\":\"echo\",\"description\":\"echo it\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}},"
        "\"annotations\":{\"readOnlyHint\":true}},"
        "{\"name\":\"fail\",\"inputSchema\":{\"type\":\"object\"},"
        "\"annotations\":{\"destructiveHint\":true}},"
        /* annotation-free and empty-annotation tools follow the MCP
         * schema default (destructive), explicit false is additive. */
        "{\"name\":\"plain\",\"inputSchema\":{\"type\":\"object\"}},"
        "{\"name\":\"additive\",\"inputSchema\":{\"type\":\"object\"},"
        "\"annotations\":{\"destructiveHint\":false}},"
        "{\"name\":\"rowins\",\"inputSchema\":{\"type\":\"object\"},"
        "\"annotations\":{\"readOnlyHint\":true,\"destructiveHint\":true}},"
        "{\"name\":\"emptyann\",\"inputSchema\":{\"type\":\"object\"},"
        "\"annotations\":{}}"
        "],\"nextCursor\":\"page 2\"}}\n";
    AgcVec tv = { 0 };
    char *cur = NULL;
    int rc = agentc_mcp_parse_tools(tools_msg, agentc_strlen(tools_msg), &tv, &cur);
    check("tools.parse", rc == 0 && tv.len == 6 && cur && agentc_streq(cur, "page 2"));
    McpParsedTool *a = tv.len == 6 ? &((McpParsedTool *)tv.p)[0] : NULL;
    McpParsedTool *b = tv.len == 6 ? &((McpParsedTool *)tv.p)[1] : NULL;
    McpParsedTool *plain = tv.len == 6 ? &((McpParsedTool *)tv.p)[2] : NULL;
    McpParsedTool *additive = tv.len == 6 ? &((McpParsedTool *)tv.p)[3] : NULL;
    McpParsedTool *rowins = tv.len == 6 ? &((McpParsedTool *)tv.p)[4] : NULL;
    McpParsedTool *emptyann = tv.len == 6 ? &((McpParsedTool *)tv.p)[5] : NULL;
    check("tools.name", a && agentc_streq(a->name, "echo") && agentc_streq(a->desc, "echo it"));
    check("tools.schema",
          a && a->schema &&
              agentc_streq(a->schema,
                       "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}"));
    check("tools.flags", a && a->flags == AGENTC_TOOL_READONLY && b &&
                             b->flags == AGENTC_TOOL_DESTRUCTIVE);
    check("tools.flags_default_destructive",
          plain && plain->flags == AGENTC_TOOL_DESTRUCTIVE && emptyann &&
              emptyann->flags == AGENTC_TOOL_DESTRUCTIVE);
    check("tools.flags_explicit_additive", additive && additive->flags == 0);
    check("tools.flags_readonly_wins", rowins && rowins->flags == AGENTC_TOOL_READONLY);
    check("flags.helper_readonly_wins",
          agentc_mcp_flags_from_hints(true, true) == AGENTC_TOOL_READONLY);
    check("flags.helper_additive", agentc_mcp_flags_from_hints(false, false) == 0);
    check("flags.helper_default",
          agentc_mcp_flags_from_hints(false, true) == AGENTC_TOOL_DESTRUCTIVE);
    check("tools.nodesc", b && agentc_streq(b->desc, ""));
    agentc_free(cur);
    agentc_mcp_parsed_free(&tv);

    /* A blank/whitespace nextCursor is end-of-list, not a page-1 retry. */
    const char *blank_cursor =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"tools\":[],\"nextCursor\":\"  \"}}\n";
    rc = agentc_mcp_parse_tools(blank_cursor, agentc_strlen(blank_cursor), &tv, &cur);
    check("tools.blank_cursor", rc == 0 && cur == NULL);
    agentc_mcp_parsed_free(&tv);

    const char *rpc_err =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32601,\"message\":\"no\"}}\n";
    rc = agentc_mcp_parse_tools(rpc_err, agentc_strlen(rpc_err), &tv, NULL);
    check("tools.rpc_error", rc == -5 && tv.len == 0);
    agentc_mcp_parsed_free(&tv);
}

static void test_parse_call(void) {
    const char *ok_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":["
        "{\"type\":\"text\",\"text\":\"hello\"},"
        "{\"type\":\"image\",\"data\":\"aGk=\",\"mimeType\":\"image/png\"}],"
        "\"structuredContent\":{\"echo\":\"hello\"}}}\n";
    char *text = NULL;
    bool err = false;
    int rc = agentc_mcp_parse_call(ok_msg, agentc_strlen(ok_msg), &text, &err);
    check("call.ok", rc == 0 && !err);
    check("call.text",
          text &&
              agentc_streq(text,
                       "hello\n{\"type\":\"image\",\"data\":\"aGk=\",\"mimeType\":\"image/png\"}\n"
                       "{\"echo\":\"hello\"}"));
    agentc_free(text);

    const char *err_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"content\":[{\"type\":\"text\",\"text\":"
        "\"boom\"}],\"isError\":true}}\n";
    rc = agentc_mcp_parse_call(err_msg, agentc_strlen(err_msg), &text, &err);
    check("call.iserror", rc == 0 && err && text && agentc_streq(text, "boom"));
    agentc_free(text);

    const char *rpc_err =
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"error\":{\"code\":-32602,\"message\":\"bad args\"}}\n";
    rc = agentc_mcp_parse_call(rpc_err, agentc_strlen(rpc_err), &text, &err);
    check("call.rpc_error", rc == 0 && err && text &&
                                agentc_streq(text, "MCP error -32602: bad args"));
    agentc_free(text);
}

/* -------------------------------------------------- capabilities/prompts */
static void test_capabilities(void) {
    const char *absent =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2024-11-05\"}}";
    check("caps.absent", agentc_mcp_caps_parse(absent, agentc_strlen(absent)) == MCP_CAP_TOOLS);
    const char *malformed =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":\"nope\"}}";
    check("caps.malformed",
          agentc_mcp_caps_parse(malformed, agentc_strlen(malformed)) == MCP_CAP_TOOLS);
    const char *empty =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{}}}";
    check("caps.empty",
          agentc_mcp_caps_parse(empty, agentc_strlen(empty)) == MCP_CAP_TOOLS);
    const char *no_result = "{\"jsonrpc\":\"2.0\",\"id\":1}";
    check("caps.no_result",
          agentc_mcp_caps_parse(no_result, agentc_strlen(no_result)) == MCP_CAP_TOOLS);
    const char *tools =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{\"tools\":{"
        "\"listChanged\":true}}}}";
    check("caps.tools", agentc_mcp_caps_parse(tools, agentc_strlen(tools)) ==
                            (MCP_CAP_TOOLS | MCP_CAP_TOOLS_LIST_CHANGED));
    const char *prompts_only =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{\"prompts\":{}}}}";
    check("caps.prompts_only",
          agentc_mcp_caps_parse(prompts_only, agentc_strlen(prompts_only)) == MCP_CAP_PROMPTS);
    const char *all =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{"
        "\"tools\":{},\"prompts\":{\"listChanged\":true},"
        "\"resources\":{\"listChanged\":true}}}}";
    check("caps.all", agentc_mcp_caps_parse(all, agentc_strlen(all)) ==
                          (MCP_CAP_TOOLS | MCP_CAP_PROMPTS | MCP_CAP_RESOURCES |
                           MCP_CAP_PROMPTS_LIST_CHANGED | MCP_CAP_RESOURCES_LIST_CHANGED));
}

static void test_parse_prompts(void) {
    const char *msg =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"prompts\":["
        "{\"name\":\"greet\",\"title\":\"Greeting\",\"description\":\"say hi\","
        "\"arguments\":[{\"name\":\"name\",\"description\":\"who\",\"required\":true},"
        "{\"name\":\"tone\"}]},"
        "{\"name\":\"summary\"}"
        "],\"nextCursor\":\"p2\"}}\n";
    AgcVec pv = { 0 };
    char *cur = NULL;
    int rc = agentc_mcp_parse_prompts(msg, agentc_strlen(msg), &pv, &cur);
    check("prompts.parse", rc == 0 && pv.len == 2 && cur && agentc_streq(cur, "p2"));
    McpParsedPrompt *g = pv.len == 2 ? &((McpParsedPrompt *)pv.p)[0] : NULL;
    McpParsedPrompt *s = pv.len == 2 ? &((McpParsedPrompt *)pv.p)[1] : NULL;
    check("prompts.fields",
          g && agentc_streq(g->name, "greet") && agentc_streq(g->title, "Greeting") &&
              agentc_streq(g->description, "say hi") && g->nargs == 2);
    check("prompts.arg_required",
          g && agentc_streq(g->args[0].name, "name") &&
              agentc_streq(g->args[0].description, "who") && g->args[0].required);
    check("prompts.arg_optional",
          g && agentc_streq(g->args[1].name, "tone") &&
              agentc_streq(g->args[1].description, "") && !g->args[1].required);
    check("prompts.no_args", s && s->nargs == 0 && agentc_streq(s->title, "") &&
                                 agentc_streq(s->description, ""));
    agentc_free(cur);
    agentc_mcp_parsed_prompts_free(&pv);

    /* an over-long remote name is skipped; a description is truncated */
    AgcBuf b = { 0 };
    agentc_buf_cstr(&b, "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"prompts\":[{\"name\":\"");
    for (int i = 0; i < 300; i++) agentc_buf_byte(&b, 'n');
    agentc_buf_cstr(&b, "\"},{\"name\":\"big\",\"description\":\"");
    for (int i = 0; i < AGENTC_LIMIT_MCP_PROMPT_DESC + 100; i++) agentc_buf_byte(&b, 'd');
    agentc_buf_cstr(&b, "\"}]}}\n");
    rc = agentc_mcp_parse_prompts((const char *)b.p, b.len, &pv, NULL);
    McpParsedPrompt *bp = pv.len == 1 ? &((McpParsedPrompt *)pv.p)[0] : NULL;
    check("prompts.long_name_skipped", rc == 0 && pv.len == 1 && bp &&
                                            agentc_streq(bp->name, "big"));
    check("prompts.desc_truncated",
          bp && agentc_strlen(bp->description) == AGENTC_LIMIT_MCP_PROMPT_DESC);
    agentc_mcp_parsed_prompts_free(&pv);
    agentc_buf_free(&b);

    /* argument descriptions are capped like every other MCP field (truncate,
     * never skip: the argument itself stays usable) */
    AgcBuf db = { 0 };
    agentc_buf_cstr(&db, "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"prompts\":[{\"name\":\"dargs\",\"arguments\":[{\"name\":\"a\",\"description\":\"");
    for (int i = 0; i < AGENTC_LIMIT_MCP_PROMPT_DESC + 64; i++) agentc_buf_byte(&db, 'd');
    agentc_buf_cstr(&db, "\"}]}]}}\n");
    rc = agentc_mcp_parse_prompts((const char *)db.p, db.len, &pv, NULL);
    McpParsedPrompt *dp = pv.len == 1 ? &((McpParsedPrompt *)pv.p)[0] : NULL;
    check("prompts.arg_desc_truncated",
          rc == 0 && dp && dp->nargs == 1 && dp->args[0].description &&
              agentc_strlen(dp->args[0].description) == AGENTC_LIMIT_MCP_PROMPT_DESC);
    agentc_mcp_parsed_prompts_free(&pv);
    agentc_buf_free(&db);

    /* argument shapes: non-objects/empty names/long names are skipped, the
     * list is capped at AGENTC_LIMIT_MCP_PROMPT_ARGS */
    AgcBuf ab = { 0 };
    agentc_buf_cstr(&ab, "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"prompts\":[{\"name\":\"args\",\"arguments\":[");
    agentc_buf_cstr(&ab, "1,{\"name\":\"\"},\"x\",");
    agentc_buf_cstr(&ab, "{\"name\":\"");
    for (int i = 0; i < AGENTC_LIMIT_MCP_ARG_NAME + 1; i++) agentc_buf_byte(&ab, 'z');
    agentc_buf_cstr(&ab, "\"},");
    for (int i = 0; i < AGENTC_LIMIT_MCP_PROMPT_ARGS + 4; i++) {
        char frag[32];
        agentc_snprintf(frag, sizeof frag, "{\"name\":\"a%d\"},", i);
        agentc_buf_cstr(&ab, frag);
    }
    agentc_buf_cstr(&ab, "{\"name\":\"tail\"}]}]}}\n");
    rc = agentc_mcp_parse_prompts((const char *)ab.p, ab.len, &pv, NULL);
    McpParsedPrompt *ap = pv.len == 1 ? &((McpParsedPrompt *)pv.p)[0] : NULL;
    check("prompts.arg_shapes", rc == 0 && ap && ap->nargs == AGENTC_LIMIT_MCP_PROMPT_ARGS &&
                                     agentc_streq(ap->args[0].name, "a0"));
    agentc_mcp_parsed_prompts_free(&pv);
    agentc_buf_free(&ab);

    const char *rpc_err =
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"error\":{\"code\":-32601,\"message\":\"no\"}}\n";
    rc = agentc_mcp_parse_prompts(rpc_err, agentc_strlen(rpc_err), &pv, NULL);
    check("prompts.rpc_error", rc == -5 && pv.len == 0);
    agentc_mcp_parsed_prompts_free(&pv);
}

static void test_parse_prompt_get(void) {
    const char *join =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"messages\":["
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"a\"},"
        "{\"type\":\"image\",\"data\":\"aGk=\"}]},"
        "{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":\"b\"}}]}}\n";
    char *text = NULL;
    int rc = agentc_mcp_parse_prompt_get(join, agentc_strlen(join), &text);
    check("get.join_text", rc == 0 && text && agentc_streq(text, "a\n\nb"));
    agentc_free(text);

    const char *empty =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"messages\":[]}}\n";
    rc = agentc_mcp_parse_prompt_get(empty, agentc_strlen(empty), &text);
    check("get.empty", rc == 0 && text && text[0] == 0);
    agentc_free(text);

    const char *err =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32602,\"message\":\"nope\"}}\n";
    check("get.rpc_error", agentc_mcp_parse_prompt_get(err, agentc_strlen(err), &text) == -5 &&
                               text == NULL);
    check("get.no_result",
          agentc_mcp_parse_prompt_get("{\"jsonrpc\":\"2.0\",\"id\":4}",
                                      agentc_strlen("{\"jsonrpc\":\"2.0\",\"id\":4}"), &text) == -5);

    /* the cap counts the separators too, and must never truncate */
    AgcBuf big = { 0 };
    agentc_buf_cstr(&big,
                "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"messages\":[{\"content\":"
                "{\"type\":\"text\",\"text\":\"");
    for (size_t i = 0; i <= AGENTC_LIMIT_MCP_PROMPT_TEXT_BYTES; i++) agentc_buf_byte(&big, 'x');
    agentc_buf_cstr(&big, "\"}}]}}\n");
    rc = agentc_mcp_parse_prompt_get((const char *)big.p, big.len, &text);
    check("get.over_cap", rc == -7 && text == NULL);
    agentc_buf_free(&big);

    AgcBuf two = { 0 };
    agentc_buf_cstr(&two, "{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":{\"messages\":[{\"content\":[{\"type\":\"text\",\"text\":\"");
    for (size_t i = 0; i < AGENTC_LIMIT_MCP_PROMPT_TEXT_BYTES / 2; i++) agentc_buf_byte(&two, 'x');
    agentc_buf_cstr(&two, "\"},{\"type\":\"text\",\"text\":\"");
    for (size_t i = 0; i < AGENTC_LIMIT_MCP_PROMPT_TEXT_BYTES / 2; i++) agentc_buf_byte(&two, 'x');
    agentc_buf_cstr(&two, "\"}]}]}}\n");
    rc = agentc_mcp_parse_prompt_get((const char *)two.p, two.len, &text);
    check("get.separator_counted", rc == -7 && text == NULL);
    agentc_buf_free(&two);
}

/* ---------------------------------------------------- resource parsers */
static void test_parse_resources(void) {
    const char *msg =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"resources\":["
        "{\"uri\":\"file:///a.txt\",\"name\":\"a\",\"title\":\"A\","
        "\"description\":\"first\",\"mimeType\":\"text/plain\"},"
        "{\"uri\":\"file:///b\",\"name\":\"b\"},"
        "{\"uri\":\"file:///c\",\"name\":\"\"},"
        "{\"name\":\"no-uri\"}"
        "],\"nextCursor\":\"r2\"}}\n";
    AgcVec rv = { 0 };
    char *cur = NULL;
    int rc = agentc_mcp_parse_resources(msg, agentc_strlen(msg), &rv, &cur);
    check("resources.parse", rc == 0 && rv.len == 2 && cur && agentc_streq(cur, "r2"));
    const McpParsedResource *a = rv.len == 2 ? &((McpParsedResource *)rv.p)[0] : NULL;
    const McpParsedResource *b = rv.len == 2 ? &((McpParsedResource *)rv.p)[1] : NULL;
    check("resources.fields",
          a && agentc_streq(a->uri, "file:///a.txt") && agentc_streq(a->name, "a") &&
              agentc_streq(a->title, "A") && agentc_streq(a->description, "first") &&
              agentc_streq(a->mime, "text/plain"));
    check("resources.optional_empty",
          b && agentc_streq(b->title, "") && agentc_streq(b->description, "") &&
              agentc_streq(b->mime, ""));
    agentc_free(cur);
    agentc_mcp_parsed_resources_free(&rv);

    /* an over-long uri is skipped (a clipped address reads the wrong
     * resource); over-long display fields are truncated to their caps. */
    AgcBuf lb = { 0 };
    agentc_buf_cstr(&lb, "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"resources\":[{\"uri\":\"");
    for (size_t i = 0; i < AGENTC_LIMIT_MCP_RESOURCE_URI + 1; i++) agentc_buf_byte(&lb, 'u');
    agentc_buf_cstr(&lb, "\",\"name\":\"n\"},{\"uri\":\"file:///ok\",\"name\":\"");
    for (size_t i = 0; i < AGENTC_LIMIT_MCP_RESOURCE_NAME + 10; i++) agentc_buf_byte(&lb, 'n');
    agentc_buf_cstr(&lb, "\",\"title\":\"");
    for (size_t i = 0; i < AGENTC_LIMIT_MCP_RESOURCE_NAME + 10; i++) agentc_buf_byte(&lb, 't');
    agentc_buf_cstr(&lb, "\",\"description\":\"");
    for (size_t i = 0; i < AGENTC_LIMIT_MCP_RESOURCE_DESC + 10; i++) agentc_buf_byte(&lb, 'd');
    agentc_buf_cstr(&lb, "\",\"mimeType\":\"");
    for (size_t i = 0; i < AGENTC_LIMIT_MCP_RESOURCE_MIME + 10; i++) agentc_buf_byte(&lb, 'm');
    agentc_buf_cstr(&lb, "\"}]}}\n");
    rc = agentc_mcp_parse_resources((const char *)lb.p, lb.len, &rv, NULL);
    const McpParsedResource *big = rv.len == 1 ? &((McpParsedResource *)rv.p)[0] : NULL;
    check("resources.long_uri_skipped", rc == 0 && rv.len == 1);
    check("resources.long_fields_truncated",
          big && agentc_strlen(big->name) == AGENTC_LIMIT_MCP_RESOURCE_NAME &&
              agentc_strlen(big->title) == AGENTC_LIMIT_MCP_RESOURCE_NAME &&
              agentc_strlen(big->description) == AGENTC_LIMIT_MCP_RESOURCE_DESC &&
              agentc_strlen(big->mime) == AGENTC_LIMIT_MCP_RESOURCE_MIME);
    agentc_mcp_parsed_resources_free(&rv);
    agentc_buf_free(&lb);

    const char *rpc_err =
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"error\":{\"code\":-32601,\"message\":\"no\"}}\n";
    rc = agentc_mcp_parse_resources(rpc_err, agentc_strlen(rpc_err), &rv, NULL);
    check("resources.rpc_error", rc == -5 && rv.len == 0);
    agentc_mcp_parsed_resources_free(&rv);
    check("resources.malformed",
          agentc_mcp_parse_resources("not json", 8, &rv, NULL) == -71 && rv.len == 0);
    agentc_mcp_parsed_resources_free(&rv);
}

static void test_parse_resource_templates(void) {
    const char *msg =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"resourceTemplates\":["
        "{\"uriTemplate\":\"file:///{path}\",\"name\":\"t\",\"description\":\"d\","
        "\"mimeType\":\"text/plain\"},"
        "{\"name\":\"no-template\"}"
        "],\"nextCursor\":\"tp2\"}}\n";
    AgcVec tv = { 0 };
    char *cur = NULL;
    int rc = agentc_mcp_parse_resource_templates(msg, agentc_strlen(msg), &tv, &cur);
    check("templates.parse", rc == 0 && tv.len == 1 && cur && agentc_streq(cur, "tp2"));
    const McpParsedResourceTemplate *t = tv.len == 1 ? &((McpParsedResourceTemplate *)tv.p)[0] : NULL;
    check("templates.fields",
          t && agentc_streq(t->uri_template, "file:///{path}") &&
              agentc_streq(t->name, "t") && agentc_streq(t->description, "d") &&
              agentc_streq(t->mime, "text/plain"));
    agentc_free(cur);
    agentc_mcp_parsed_resource_templates_free(&tv);

    AgcBuf lb = { 0 };
    agentc_buf_cstr(&lb, "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"resourceTemplates\":[{\"uriTemplate\":\"");
    for (size_t i = 0; i < AGENTC_LIMIT_MCP_RESOURCE_URI + 1; i++) agentc_buf_byte(&lb, 'u');
    agentc_buf_cstr(&lb, "\",\"name\":\"n\"}]}}\n");
    rc = agentc_mcp_parse_resource_templates((const char *)lb.p, lb.len, &tv, NULL);
    check("templates.long_uri_skipped", rc == 0 && tv.len == 0);
    agentc_mcp_parsed_resource_templates_free(&tv);
    agentc_buf_free(&lb);

    const char *rpc_err =
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"error\":{\"code\":-32601,\"message\":\"no\"}}\n";
    rc = agentc_mcp_parse_resource_templates(rpc_err, agentc_strlen(rpc_err), &tv, NULL);
    check("templates.rpc_error", rc == -5 && tv.len == 0);
    agentc_mcp_parsed_resource_templates_free(&tv);
}

static void test_parse_resource_read(void) {
    const char *join =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"contents\":["
        "{\"uri\":\"file:///a\",\"mimeType\":\"text/plain\",\"text\":\"alpha\"},"
        "{\"uri\":\"file:///a\",\"text\":\"beta\"}]}}\n";
    char *text = NULL;
    int rc = agentc_mcp_parse_resource_read(join, agentc_strlen(join), &text);
    check("read.join_text", rc == 0 && text && agentc_streq(text, "alpha\n\nbeta"));
    agentc_free(text);

    const char *empty =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"contents\":[]}}\n";
    rc = agentc_mcp_parse_resource_read(empty, agentc_strlen(empty), &text);
    check("read.empty", rc == 0 && text && text[0] == 0);
    agentc_free(text);

    const char *blob =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"contents\":["
        "{\"uri\":\"file:///b.bin\",\"blob\":\"aGk=\",\"mimeType\":\"application/octet-stream\"}]}}\n";
    rc = agentc_mcp_parse_resource_read(blob, agentc_strlen(blob), &text);
    check("read.blob_rejected", rc == MCP_ERR_BINARY && text == NULL);

    const char *mixed =
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"contents\":["
        "{\"uri\":\"file:///a\",\"text\":\"head\"},"
        "{\"uri\":\"file:///b\",\"blob\":\"aGk=\"}]}}\n";
    rc = agentc_mcp_parse_resource_read(mixed, agentc_strlen(mixed), &text);
    check("read.blob_rejects_all", rc == MCP_ERR_BINARY && text == NULL);

    const char *err =
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"error\":{\"code\":-32602,\"message\":\"nope\"}}\n";
    check("read.rpc_error",
          agentc_mcp_parse_resource_read(err, agentc_strlen(err), &text) == -5 && text == NULL);
    check("read.no_result",
          agentc_mcp_parse_resource_read("{\"jsonrpc\":\"2.0\",\"id\":6}",
                                         agentc_strlen("{\"jsonrpc\":\"2.0\",\"id\":6}"),
                                         &text) == -5);

    /* the cap counts the separators too, and must never truncate */
    AgcBuf big = { 0 };
    agentc_buf_cstr(&big, "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"contents\":[{\"text\":\"");
    for (size_t i = 0; i <= AGENTC_LIMIT_MCP_RESOURCE_TEXT_BYTES; i++) agentc_buf_byte(&big, 'x');
    agentc_buf_cstr(&big, "\"}]}}\n");
    rc = agentc_mcp_parse_resource_read((const char *)big.p, big.len, &text);
    check("read.over_cap", rc == -7 && text == NULL);
    agentc_buf_free(&big);

    AgcBuf two = { 0 };
    agentc_buf_cstr(&two, "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{\"contents\":[{\"text\":\"");
    for (size_t i = 0; i < AGENTC_LIMIT_MCP_RESOURCE_TEXT_BYTES / 2; i++) agentc_buf_byte(&two, 'x');
    agentc_buf_cstr(&two, "\"},{\"text\":\"");
    for (size_t i = 0; i < AGENTC_LIMIT_MCP_RESOURCE_TEXT_BYTES / 2; i++) agentc_buf_byte(&two, 'x');
    agentc_buf_cstr(&two, "\"}]}}\n");
    rc = agentc_mcp_parse_resource_read((const char *)two.p, two.len, &text);
    check("read.separator_counted", rc == -7 && text == NULL);
    agentc_buf_free(&two);
}

static void test_arena_isolation(void) {
    /* A default-arena document another component holds must survive every
     * callback parse path (the old nested parse cleared the shared arena). */
    const char *tools_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
        "{\"name\":\"echo\",\"description\":\"echo it\","
        "\"inputSchema\":{\"type\":\"object\"},"
        "\"annotations\":{\"readOnlyHint\":true}}],\"nextCursor\":\"p2\"}}\n";
    const char *call_msg =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":["
        "{\"type\":\"text\",\"text\":\"hello\"}],"
        "\"structuredContent\":{\"echo\":\"hello\"}}}\n";
    const char *cfg_msg =
        "{\"servers\":{\"files\":{\"command\":\"npx\",\"args\":[\"-y\"],"
        "\"env\":{\"TOKEN\":\"t\"},\"headers\":{\"Authorization\":\"Bearer t\"}}}}";

    AgcJson *keep = agentc_json_parse("{\"keep\":\"v\"}", 12);
    check("mcp.arena.hold", agentc_json_is(agentc_json_get(keep, "keep"), "v"));

    AgcVec tv = { 0 };
    char *cur = NULL;
    bool iso = true;
    int rc = agentc_mcp_parse_tools(tools_msg, agentc_strlen(tools_msg), &tv, &cur);
    bool tools_iso = rc == 0 && agentc_json_is(agentc_json_get(keep, "keep"), "v");
    iso = iso && tools_iso;
    check("mcp.arena.doc-isolation.tools", tools_iso);
    agentc_free(cur);
    agentc_mcp_parsed_free(&tv);

    keep = agentc_json_parse("{\"keep\":\"v\"}", 12);
    char *text = NULL;
    bool err = false;
    rc = agentc_mcp_parse_call(call_msg, agentc_strlen(call_msg), &text, &err);
    bool call_iso = rc == 0 && agentc_json_is(agentc_json_get(keep, "keep"), "v");
    iso = iso && call_iso;
    check("mcp.arena.doc-isolation.call", call_iso);
    agentc_free(text);

    keep = agentc_json_parse("{\"keep\":\"v\"}", 12);
    AgcVec cfgs = { 0 };
    rc = agentc_mcp_cfg_parse(cfg_msg, agentc_strlen(cfg_msg), &cfgs);
    bool cfg_iso = rc == 0 && cfgs.len == 1 &&
                   agentc_json_is(agentc_json_get(keep, "keep"), "v");
    iso = iso && cfg_iso;
    check("mcp.arena.doc-isolation.cfg", cfg_iso);
    check("mcp.arena.doc-isolation", iso);
    agentc_mcp_cfgs_free(&cfgs);
}

/* ------------------------------------------- HTTP transport via mock net */
static void mock_push_data(AgcBuf *s, const void *p, size_t n) {
    static const char hex[] = "0123456789abcdef";
    const u8 *q = p;
    agentc_buf_cstr(s, "data ");
    for (size_t i = 0; i < n; i++) {
        agentc_buf_byte(s, (u8)hex[q[i] >> 4]);
        agentc_buf_byte(s, (u8)hex[q[i] & 0xf]);
    }
    agentc_buf_byte(s, '\n');
}

static void mock_push_json_hdr(AgcBuf *s, const char *extra, const char *body) {
    AgcBuf r = { 0 };
    agentc_buf_printf(&r,
                  "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n%s"
                  "Content-Length: %llu\r\n\r\n%s",
                  extra, (unsigned long long)agentc_strlen(body), body);
    mock_push_data(s, r.p, r.len);
    agentc_buf_free(&r);
}

static void mock_push_json(AgcBuf *s, const char *body) { mock_push_json_hdr(s, "", body); }

static void mock_push_accepted(AgcBuf *s) {
    const char *r = "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n";
    mock_push_data(s, r, agentc_strlen(r));
}

static void mock_push_sse(AgcBuf *s, const char *json) {
    AgcBuf r = { 0 };
    agentc_buf_cstr(&r,
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"
                "event: message\ndata: ");
    agentc_buf_cstr(&r, json);
    agentc_buf_cstr(&r, "\n\n");
    mock_push_data(s, r.p, r.len);
    agentc_buf_free(&r);
}

/* One SSE body carrying a notification and then the matching result: the wire
 * layer scans events until the wanted id, and the interleaved notification is
 * what latches the follow-up re-sync. */
static void mock_push_sse2(AgcBuf *s, const char *first, const char *second) {
    AgcBuf r = { 0 };
    agentc_buf_cstr(&r,
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"
                "event: message\ndata: ");
    agentc_buf_cstr(&r, first);
    agentc_buf_cstr(&r, "\n\nevent: message\ndata: ");
    agentc_buf_cstr(&r, second);
    agentc_buf_cstr(&r, "\n\n");
    mock_push_data(s, r.p, r.len);
    agentc_buf_free(&r);
}

static void test_http_mock(void) {
    agentc_rm_rf(TEST_ROOT);
    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");

    const char *cfg =
        "{\"servers\":{\"web\":{\"url\":\"http://api.test/mcp\","
        "\"timeout_ms\":5000}}}\n";
    check("http.cfg",
          agentc_write_file_atomic(TEST_ROOT "/config/agentc/mcp.jsonc", cfg, agentc_strlen(cfg), 0644) == 0);

    AgcBuf script = { 0 };
    agentc_buf_cstr(&script, "dns api.test 203.0.113.5\n");
    mock_push_json_hdr(&script, "Mcp-Session-Id: sess-1\r\n",
                       "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":"
                       "\"2024-11-05\"}}");
    mock_push_accepted(&script);
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"echo\","
                   "\"description\":\"e\",\"inputSchema\":{\"type\":\"object\"},"
                   "\"annotations\":{\"readOnlyHint\":true}}],\"nextCursor\":\"p2\"}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"tools\":[{\"name\":\"fail\","
                   "\"description\":\"f\",\"inputSchema\":{\"type\":\"object\"},"
                   "\"annotations\":{\"destructiveHint\":true}}]}}");
    mock_push_sse(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"content\":[{\"type\":"
                   "\"text\",\"text\":\"http hello\"}],\"structuredContent\":"
                   "{\"echo\":\"http hello\"}}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"content\":[{\"type\":"
                   "\"text\",\"text\":\"http fail\"}],\"isError\":true}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":{\"content\":[{\"type\":"
                   "\"text\",\"text\":\"call id ok\"}]}}");
    check("http.mock_write",
          agentc_write_file_atomic(TEST_ROOT "/http.mock", script.p, script.len, 0644) == 0);
    agentc_buf_free(&script);
    check("http.mock_load", agentc_mock_load(TEST_ROOT "/http.mock") == 0);

    check("http.load", agentc_mcp_load(TEST_ROOT, false) == 1);
    AgcTool tl[8];
    size_t n = agentc_mcp_tools(tl, 8);
    check("http.tools", n == 2);
    const AgcTool *echo = find_tool(tl, n, "mcp__web__echo");
    const AgcTool *fail = find_tool(tl, n, "mcp__web__fail");
    check("http.expose", echo != NULL && fail != NULL);
    if (echo) {
        bool err = false;
        char *res = run_tool(echo, "{\"text\":\"x\"}", &err);
        check("http.call_sse", !err && res && agentc_str_str(res, "http hello") != NULL &&
                                   agentc_str_str(res, "{\"echo\":\"http hello\"}") != NULL);
        agentc_free(res);
    } else {
        check("http.call_sse", false);
    }
    if (fail) {
        bool err = false;
        char *res = run_tool(fail, "{}", &err);
        check("http.call_json_error", err && res && agentc_streq(res, "http fail"));
        agentc_free(res);
    } else {
        check("http.call_json_error", false);
    }
    if (echo) {
        bool err = false;
        char *res = run_tool_id(echo, "call_mcp_1", "{}", &err);
        check("mcp.call-id", !err && res && agentc_streq(res, "call id ok"));
        agentc_free(res);
    } else {
        check("mcp.call-id", false);
    }
    agentc_mcp_shutdown();
    agentc_mock_reset();
    agentc_rm_rf(TEST_ROOT);
}

static void test_grace_reclaim(void) {
    /* Three successful syncs over the HTTP mock: #1 publishes v1, #2 replaces
     * it (freezing the handed-out v1 entry), #3 is a no-op whose commit frees
     * the v1 entry (one-sync grace). The block count must drop at #3. */
    agentc_rm_rf(TEST_ROOT);
    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");
    const char *cfg =
        "{\"servers\":{\"web\":{\"url\":\"http://api.test/mcp\",\"timeout_ms\":5000}}}\n";
    check("grace.cfg",
          agentc_write_file_atomic(TEST_ROOT "/config/agentc/mcp.jsonc", cfg,
                                   agentc_strlen(cfg), 0644) == 0);
    AgcBuf script = { 0 };
    agentc_buf_cstr(&script, "dns api.test 203.0.113.5\n");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":"
                   "\"2024-11-05\"}}");
    mock_push_accepted(&script);
    mock_push_sse2(&script,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"echo\","
        "\"description\":\"v1\",\"inputSchema\":{\"type\":\"object\"}}]}}");
    mock_push_sse2(&script,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"tools\":[{\"name\":\"echo\","
        "\"description\":\"v2\",\"inputSchema\":{\"type\":\"object\"}}]}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"tools\":[{\"name\":\"echo\","
                   "\"description\":\"v2\",\"inputSchema\":{\"type\":\"object\"}}]}}");
    check("grace.mock",
          agentc_write_file_atomic(TEST_ROOT "/grace.mock", script.p, script.len, 0644) == 0);
    agentc_buf_free(&script);
    check("grace.mock_load", agentc_mock_load(TEST_ROOT "/grace.mock") == 0);

    agentc_mcp_start(TEST_ROOT, false);
    AgcTool tl[2];
    bool v1 = false;
    for (int i = 0; i < 20000 && !v1; i++) {
        size_t n = agentc_mcp_tools(tl, 2);
        v1 = n == 1 && agentc_streq(tl[0].desc, "v1");
        if (!v1) { agentc_mcp_pump(); os_poll(NULL, 0, 1); }
    }
    check("grace.v1", v1);   /* tl[0] is handed_out: its retirement freezes it */
    bool v2 = false;
    for (int i = 0; i < 20000 && !v2; i++) {
        AgcTool cur[2];
        size_t n = agentc_mcp_tools(cur, 2);
        v2 = n == 1 && agentc_streq(cur[0].desc, "v2");
        if (!v2) { agentc_mcp_pump(); os_poll(NULL, 0, 1); }
    }
    check("grace.v2", v2);
    /* v1 was handed out and then retired. The no-op sync #3 must neither free
     * nor reuse it: the old block stays readable and memory does not drop. */
    const char *v1_desc = v1 ? tl[0].desc : NULL;
    void *v1_ud = v1 ? tl[0].ud : NULL;
    size_t frozen = agentc_mem_live();
    agentc_mcp_pump();   /* sync #3: nothing may be reclaimed */
    check("grace.frozen_kept", agentc_mem_live() == frozen);
    AgcTool cur[2];
    check("grace.still_v2",
          agentc_mcp_tools(cur, 2) == 1 && agentc_streq(cur[0].desc, "v2"));
    check("grace.v1_identity_stable",
          v1_desc && agentc_streq(v1_desc, "v1") && v1_ud && cur[0].ud != v1_ud);
    agentc_mcp_shutdown();
    agentc_mock_reset();
    agentc_rm_rf(TEST_ROOT);
}

static void test_retry_delays(void) {
    /* base << min(retry_count,6), capped; env overrides the base and 0 disables */
    agentc_test_setenv("AGENTC_MCP_RETRY_MS", "10");
    check("retry.delay_base", agentc_mcp_retry_delay_ms(0) == 10);
    check("retry.delay_double",
          agentc_mcp_retry_delay_ms(1) == 20 && agentc_mcp_retry_delay_ms(2) == 40);
    check("retry.delay_shift_cap",
          agentc_mcp_retry_delay_ms(6) == 640 && agentc_mcp_retry_delay_ms(7) == 640 &&
              agentc_mcp_retry_delay_ms(99) == 640);
    agentc_test_setenv("AGENTC_MCP_RETRY_MS", "0");
    check("retry.delay_disabled",
          agentc_mcp_retry_delay_ms(0) == 0 && agentc_mcp_retry_delay_ms(3) == 0);
    agentc_test_setenv("AGENTC_MCP_RETRY_MS", "999999");
    check("retry.delay_env_clamped",
          agentc_mcp_retry_delay_ms(0) == AGENTC_MCP_RETRY_MAX_MS);
    agentc_test_setenv("AGENTC_MCP_RETRY_MS", "1000");
    check("retry.delay_max_cap",
          agentc_mcp_retry_delay_ms(6) == AGENTC_MCP_RETRY_MAX_MS &&
              agentc_mcp_retry_delay_ms(9) == AGENTC_MCP_RETRY_MAX_MS);
    agentc_test_setenv("AGENTC_MCP_RETRY_MS", "junk");
    check("retry.delay_bad_env_default",
          agentc_mcp_retry_delay_ms(0) == AGENTC_MCP_RETRY_BASE_MS);
    agentc_test_setenv("AGENTC_MCP_RETRY_MS", "");
}

/* A FAILED HTTP server must rearm after the backoff and recover. The mock
 * answers the first initialize with HTTP 500 and the retried one with a valid
 * result, so the same record transitions FAILED -> NEW -> READY. */
static void test_retry_reconnect(void) {
    agentc_rm_rf(TEST_ROOT);
    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");
    agentc_test_setenv("AGENTC_MCP_RETRY_MS", "150");
    const char *cfg =
        "{\"servers\":{\"web\":{\"url\":\"http://api.test/mcp\",\"timeout_ms\":5000}}}\n";
    check("retry.cfg",
          agentc_write_file_atomic(TEST_ROOT "/config/agentc/mcp.jsonc", cfg,
                                   agentc_strlen(cfg), 0644) == 0);
    AgcBuf script = { 0 };
    agentc_buf_cstr(&script, "dns api.test 203.0.113.5\n");
    const char *fail500 = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
    mock_push_data(&script, fail500, agentc_strlen(fail500));
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":"
                   "\"2024-11-05\",\"capabilities\":{\"tools\":{}}}}");
    mock_push_accepted(&script);
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":"
                   "\"echo\",\"description\":\"e\",\"inputSchema\":{\"type\":\"object\"}}]}}");
    check("retry.mock_write",
          agentc_write_file_atomic(TEST_ROOT "/retry.mock", script.p, script.len, 0644) == 0);
    agentc_buf_free(&script);
    check("retry.mock_load", agentc_mock_load(TEST_ROOT "/retry.mock") == 0);

    agentc_mcp_start(TEST_ROOT, false);
    check("retry.server_count", agentc_mcp_server_count() == 1);
    check("retry.pending_start", agentc_mcp_pending());

    /* the first initialize fails: FAILED and not pending (startup budget safe) */
    bool failed = false;
    for (int i = 0; i < 500 && !failed; i++) {
        agentc_mcp_pump();
        os_poll(NULL, 0, 1);
        if (!agentc_mcp_pending()) failed = true;
    }
    check("retry.failed", failed && agentc_mcp_tools(NULL, 0) == 0);
    bool stayed = true;
    for (int i = 0; i < 3 && stayed; i++) {
        agentc_mcp_pump();
        os_poll(NULL, 0, 1);
        if (agentc_mcp_pending()) stayed = false;
    }
    check("retry.failed_not_pending", stayed);

    /* after the small backoff the record rearms and connects */
    bool recovered = false;
    for (int i = 0; i < 5000 && !recovered; i++) {
        agentc_mcp_pump();
        os_poll(NULL, 0, 1);
        recovered = agentc_mcp_tools(NULL, 0) == 1;
    }
    AgcTool tl[2];
    size_t n = agentc_mcp_tools(tl, 2);
    check("retry.recovered",
          recovered && n == 1 && agentc_streq(tl[0].name, "mcp__web__echo"));

    agentc_mcp_shutdown();
    agentc_test_setenv("AGENTC_MCP_RETRY_MS", "");
    agentc_mock_reset();
    agentc_rm_rf(TEST_ROOT);
}

/* A JSON-RPC error to initialize is a failed handshake: the server must fail
 * without sending notifications/initialized or syncing any list. */
static void test_init_error(void) {
    agentc_rm_rf(TEST_ROOT);
    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");
    agentc_test_setenv("AGENTC_MCP_RETRY_MS", "0");
    const char *cfg =
        "{\"servers\":{\"web\":{\"url\":\"http://api.test/mcp\",\"timeout_ms\":5000}}}\n";
    check("initerr.cfg",
          agentc_write_file_atomic(TEST_ROOT "/config/agentc/mcp.jsonc", cfg,
                                   agentc_strlen(cfg), 0644) == 0);
    AgcBuf script = { 0 };
    agentc_buf_cstr(&script, "dns api.test 203.0.113.5\n");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,"
                   "\"message\":\"boom\"}}");
    check("initerr.mock_write",
          agentc_write_file_atomic(TEST_ROOT "/initerr.mock", script.p, script.len, 0644) == 0);
    agentc_buf_free(&script);
    check("initerr.mock_load", agentc_mock_load(TEST_ROOT "/initerr.mock") == 0);

    agentc_mcp_start(TEST_ROOT, false);
    bool done = false;
    for (int i = 0; i < 500 && !done; i++) {
        agentc_mcp_pump();
        os_poll(NULL, 0, 1);
        if (!agentc_mcp_pending()) done = true;
    }
    check("initerr.terminal", done && agentc_mcp_tools(NULL, 0) == 0);
    const AgcBuf *sent = agentc_mock_sent();
    const char *sp = sent && sent->p ? (const char *)sent->p : "";
    size_t sl = sent ? sent->len : 0;
    check("initerr.no_notify",
          agentc_str_find(sp, sl, "notifications/initialized",
                          agentc_strlen("notifications/initialized")) < 0);
    check("initerr.no_sync",
          agentc_str_find(sp, sl, "tools/list", agentc_strlen("tools/list")) < 0);
    agentc_mcp_shutdown();
    agentc_test_setenv("AGENTC_MCP_RETRY_MS", "");
    agentc_mock_reset();
    agentc_rm_rf(TEST_ROOT);
}

static void test_caps(void) {
    /* Per-server cap: 256 live tools fit; the 257th new identity is refused
     * with -ENOSPC. An update of a live identity never counts. */
    agentc_mcp_registry_reset();
    bool added = true;
    for (int i = 0; i < 256; i++) {
        char tool[32];
        agentc_snprintf(tool, sizeof tool, "tool_%d", i);
        if (agentc_mcp_registry_add("srv", tool, "d", "{\"type\":\"object\"}",
                                    AGENTC_TOOL_READONLY) != 0) {
            added = false;
            break;
        }
    }
    check("caps.per_server_fit", added && agentc_mcp_tools(NULL, 0) == 256);
    check("caps.per_server_exceeded",
          agentc_mcp_registry_add("srv", "tool_256", "d", "{\"type\":\"object\"}",
                                  AGENTC_TOOL_READONLY) == -28);
    check("caps.update_ok",
          agentc_mcp_registry_add("srv", "tool_0", "changed", "{\"type\":\"object\"}",
                                  AGENTC_TOOL_READONLY) == 0);
    AgcTool tmp[8];
    check("caps.fetch_limited", agentc_mcp_tools(tmp, 8) == 8);

    /* Total cap: four servers at the per-server cap fill the total; the next
     * identity on a fifth server is refused. */
    agentc_mcp_registry_reset();
    added = true;
    for (int s = 0; s < 4; s++) {
        char srv[16];
        agentc_snprintf(srv, sizeof srv, "srv_%d", s);
        for (int i = 0; i < 256; i++) {
            char tool[32];
            agentc_snprintf(tool, sizeof tool, "tool_%d", i);
            if (agentc_mcp_registry_add(srv, tool, "d", "{\"type\":\"object\"}",
                                        AGENTC_TOOL_READONLY) != 0) {
                added = false;
                break;
            }
        }
    }
    check("caps.total_fit", added && agentc_mcp_tools(NULL, 0) == 1024);
    check("caps.total_exceeded",
          agentc_mcp_registry_add("srv_4", "tool", "d", "{\"type\":\"object\"}",
                                  AGENTC_TOOL_READONLY) == -28);
    check("caps.total_exceeded_fetch", agentc_mcp_tools(NULL, 0) == 1024);
    agentc_mcp_registry_reset();
}

static void test_long_names(void) {
    /* The exposed name must stay inside the registry's 64-byte limit while
     * remaining unique even when two very long server/tool names sanitize to
     * the same 95+95 prefix (the hash suffix is preserved, the base is
     * clipped). */
    agentc_mcp_registry_reset();
    char server[128], tool[128];
    agentc_memset(server, 'a', sizeof server - 1);
    server[sizeof server - 1] = 0;
    agentc_memset(tool, 'b', sizeof tool - 1);
    tool[sizeof tool - 1] = 0;
    check("longname.add1",
          agentc_mcp_registry_add(server, tool, "d", "{\"type\":\"object\"}",
                                  AGENTC_TOOL_READONLY) == 0);
    char server2[132], tool2[132];
    agentc_snprintf(server2, sizeof server2, "%sX", server);
    agentc_snprintf(tool2, sizeof tool2, "%sY", tool);
    check("longname.add2",
          agentc_mcp_registry_add(server2, tool2, "d", "{\"type\":\"object\"}",
                                  AGENTC_TOOL_READONLY) == 0);
    AgcTool tl[4];
    size_t n = agentc_mcp_tools(tl, 4);
    bool bounded = n == 2;
    for (size_t i = 0; i < n; i++) {
        if (!tl[i].name) { bounded = false; continue; }
        size_t l = agentc_strlen(tl[i].name);
        if (l < 5 || l > 63) bounded = false;
        for (size_t k = 0; k < l; k++) {
            char c = tl[i].name[k];
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            if (!ok) bounded = false;
        }
    }
    check("longname.bounded", bounded);
    check("longname.unique", n == 2 && !agentc_streq(tl[0].name, tl[1].name));
    check("longname.prefix",
          n == 2 && agentc_str_starts(tl[0].name, agentc_strlen(tl[0].name), "mcp__"));
    agentc_mcp_registry_reset();
}

static void test_tool_limits(void) {
    /* A remote tool name over the cap is skipped: truncating it would call the
     * wrong tool on tools/call. The diagnostic names the length, never the
     * (unbounded) string. */
    AgcBuf msg = { 0 };
    agentc_buf_cstr(&msg, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":[{\"name\":\"");
    for (int i = 0; i < 300; i++) agentc_buf_byte(&msg, 'n');
    agentc_buf_cstr(&msg, "\",\"inputSchema\":{\"type\":\"object\"}}]}}\n");
    AgcVec tv = { 0 };
    int rc = agentc_mcp_parse_tools((const char *)msg.p, msg.len, &tv, NULL);
    check("limits.tool_name_dropped", rc == 0 && tv.len == 0);
    agentc_mcp_parsed_free(&tv);
    agentc_buf_free(&msg);

    /* A description over 8 KiB is truncated, never dropped: the tool stays
     * usable and no request can carry an unbounded string. */
    AgcBuf m2 = { 0 };
    agentc_buf_cstr(&m2, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"tools\":[{\"name\":\"big\","
                        "\"description\":\"");
    for (int i = 0; i < 9000; i++) agentc_buf_byte(&m2, 'd');
    agentc_buf_cstr(&m2, "\",\"inputSchema\":{\"type\":\"object\"}}]}}\n");
    AgcVec tv2 = { 0 };
    rc = agentc_mcp_parse_tools((const char *)m2.p, m2.len, &tv2, NULL);
    McpParsedTool *bt = tv2.len == 1 ? &((McpParsedTool *)tv2.p)[0] : NULL;
    check("limits.desc_truncated",
          rc == 0 && bt && agentc_strlen(bt->desc) == 8192 && bt->desc[0] == 'd');
    agentc_mcp_parsed_free(&tv2);
    agentc_buf_free(&m2);
}

static void test_churn_reclaim(void) {
    /* A description churn never frees or reuses a retired entry: the first
     * handed-out block stays readable and every change publishes a fresh
     * identity until the frozen cap refuses the next one. Refusals stop the
     * allocation growth, so memory plateaus instead of following the change
     * count. */
    agentc_mcp_registry_reset();
    agentc_log_set_level(4);   /* cap refusals are logged; keep the golden quiet */
    (void)agentc_mcp_registry_add("srv", "echo", "desc-0", "{\"type\":\"object\"}",
                                  AGENTC_TOOL_READONLY);
    AgcTool first[2];
    size_t first_n = agentc_mcp_tools(first, 2);
    const char *first_desc = first_n == 1 ? first[0].desc : NULL;
    void *first_ud = first_n == 1 ? first[0].ud : NULL;
    int accepted = 0;
    bool refused = false;
    for (int i = 1; i < 20000; i++) {
        char desc[64];
        agentc_snprintf(desc, sizeof desc, "desc-%d", i);
        int rc = agentc_mcp_registry_add("srv", "echo", desc, "{\"type\":\"object\"}",
                                         AGENTC_TOOL_READONLY);
        if (rc != 0) {
            refused = true;
            break;
        }
        accepted++;
        AgcTool tl[2];
        (void)agentc_mcp_tools(tl, 2);   /* the registry holds a copy: handed_out */
    }
    check("churn.live", agentc_mcp_tools(NULL, 0) == 1);
    check("churn.cap_refused", refused && accepted == 256);
    check("churn.old_identity_stable",
          first_desc && agentc_streq(first_desc, "desc-0"));
    AgcTool now[2];
    size_t now_n = agentc_mcp_tools(now, 2);
    check("churn.new_identity", now_n == 1 && now[0].ud != first_ud);
    size_t after = agentc_mem_live();
    /* churn past the cap must not grow memory */
    for (int i = 0; i < 1000; i++)
        (void)agentc_mcp_registry_add("srv", "echo", "late", "{\"type\":\"object\"}",
                                      AGENTC_TOOL_READONLY);
    check("churn.mem_bounded", agentc_mem_live() == after);
    agentc_log_set_level(-1);
    agentc_mcp_registry_reset();
}

static void test_frozen_cap_server(void) {
    /* End to end through the HTTP pump: a server whose tools/list keeps
     * changing a handed-out identity fills the frozen pile; the change past
     * the cap fails the server (logged) instead of recycling the old block,
     * and the live table plus memory stop growing. */
    agentc_rm_rf(TEST_ROOT);
    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");
    const char *cfg =
        "{\"servers\":{\"web\":{\"url\":\"http://api.test/mcp\",\"timeout_ms\":5000}}}\n";
    check("fcap.cfg",
          agentc_write_file_atomic(TEST_ROOT "/config/agentc/mcp.jsonc", cfg,
                                   agentc_strlen(cfg), 0644) == 0);
    AgcBuf script = { 0 };
    agentc_buf_cstr(&script, "dns api.test 203.0.113.5\n");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":"
                   "\"2024-11-05\"}}");
    mock_push_accepted(&script);
    for (int i = 0; i <= 258; i++) {
        char notif[96], result[192];
        agentc_snprintf(notif, sizeof notif,
                        "{\"jsonrpc\":\"2.0\",\"method\":"
                        "\"notifications/tools/list_changed\"}");
        agentc_snprintf(result, sizeof result,
                        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"tools\":["
                        "{\"name\":\"echo\",\"description\":\"v%d\","
                        "\"inputSchema\":{\"type\":\"object\"}}]}}",
                        i + 2, i);
        mock_push_sse2(&script, notif, result);
    }
    check("fcap.mock",
          agentc_write_file_atomic(TEST_ROOT "/fcap.mock", script.p, script.len, 0644) == 0);
    agentc_buf_free(&script);
    check("fcap.mock_load", agentc_mock_load(TEST_ROOT "/fcap.mock") == 0);

    agentc_mcp_start(TEST_ROOT, false);
    const char *v0_desc = NULL;
    void *v0_ud = NULL;
    int last_ok = -1;
    for (int round = 0; round <= 258; round++) {
        char want[32];
        agentc_snprintf(want, sizeof want, "v%d", round);
        bool got = false;
        for (int i = 0; i < 50 && !got; i++) {
            AgcTool tl[2];
            size_t n = agentc_mcp_tools(tl, 2);   /* hands the entry out */
            if (n == 1 && agentc_streq(tl[0].desc, want)) {
                got = true;
                if (round == 0) { v0_desc = tl[0].desc; v0_ud = tl[0].ud; }
            } else {
                agentc_mcp_pump();
                os_poll(NULL, 0, 1);
            }
        }
        if (!got) break;
        last_ok = round;
    }
    check("fcap.accepted", last_ok == 256);
    AgcTool cur[2];
    size_t n = agentc_mcp_tools(cur, 2);
    check("fcap.live_stable", n == 1 && agentc_streq(cur[0].desc, "v256"));
    check("fcap.old_identity_stable",
          v0_desc && agentc_streq(v0_desc, "v0") && cur[0].ud != v0_ud);
    size_t after = agentc_mem_live();
    for (int i = 0; i < 100; i++) { agentc_mcp_pump(); os_poll(NULL, 0, 1); }
    check("fcap.mem_bounded", agentc_mem_live() == after);
    agentc_mcp_shutdown();
    agentc_mock_reset();
    agentc_rm_rf(TEST_ROOT);
}

/* ---------------------------------------------------- prompt registry */
static const char *prompt_desc_of(const char *name) {
    AgcPromptInfo info[64];
    size_t n = agentc_prompts_list(info, 64);
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(info[i].name, name)) return info[i].description;
    return NULL;
}

/* Commit one prompt list built in code (test fixture). */
static int prompt_commit_one(const char *server, const char *name, const char *desc) {
    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "jsonrpc");
    agentc_jsonw_cstr(&w, "2.0");
    agentc_jsonw_key(&w, "id");
    agentc_jsonw_u64(&w, 1);
    agentc_jsonw_key(&w, "result");
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "prompts");
    agentc_jsonw_arr(&w);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "name");
    agentc_jsonw_cstr(&w, name);
    agentc_jsonw_key(&w, "description");
    agentc_jsonw_cstr(&w, desc);
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    AgcVec pv = { 0 };
    int rc = agentc_mcp_parse_prompts((const char *)b.p, b.len, &pv, NULL);
    if (rc == 0) rc = agentc_mcp_prompt_commit(server, &pv);
    agentc_mcp_parsed_prompts_free(&pv);
    agentc_buf_free(&b);
    return rc;
}

static void test_prompt_registry(void) {
    agentc_mcp_prompt_registry_reset();
    const char *two =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"prompts\":["
        "{\"name\":\"greet\",\"description\":\"hi\"},"
        "{\"name\":\"a b\",\"description\":\"space\"},"
        "{\"name\":\"a_b\",\"description\":\"underscore\"}]}}\n";
    AgcVec pv = { 0 };
    int rc = agentc_mcp_parse_prompts(two, agentc_strlen(two), &pv, NULL);
    check("prompts.commit", rc == 0 && agentc_mcp_prompt_commit("srv", &pv) == 0 &&
                               agentc_mcp_prompt_count() == 3);
    agentc_mcp_parsed_prompts_free(&pv);
    char n1[80], n2[80], n3[80];
    check("prompts.exposed",
          agentc_mcp_prompt_exposed("srv", "greet", n1, sizeof n1) &&
              agentc_streq(n1, "mcp__srv__greet"));
    check("prompts.registry", agentc_prompts_has(n1) &&
                                  agentc_streq(prompt_desc_of(n1), "hi"));
    check("prompts.dedup",
          agentc_mcp_prompt_exposed("srv", "a b", n2, sizeof n2) &&
              agentc_mcp_prompt_exposed("srv", "a_b", n3, sizeof n3) &&
              !agentc_streq(n2, n3) &&
              agentc_str_starts(n2, agentc_strlen(n2), "mcp__srv__a_b") &&
              agentc_str_starts(n3, agentc_strlen(n3), "mcp__srv__a_b"));

    /* an unchanged re-commit reuses the records (no registry slot burned) */
    size_t reg_before = agentc_prompts_list(NULL, 0);
    rc = agentc_mcp_parse_prompts(two, agentc_strlen(two), &pv, NULL);
    check("prompts.recommit_same", rc == 0 && agentc_mcp_prompt_commit("srv", &pv) == 0 &&
                                       agentc_prompts_list(NULL, 0) == reg_before);
    agentc_mcp_parsed_prompts_free(&pv);

    /* a changed description freezes the old live entry and registers the new
     * record under the same exposed name (later wins); the prompts the list
     * omits are retired */
    const char *upd =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"prompts\":["
        "{\"name\":\"greet\",\"description\":\"hi2\"},"
        "{\"name\":\"a b\"},{\"name\":\"a_b\"}]}}\n";
    rc = agentc_mcp_parse_prompts(upd, agentc_strlen(upd), &pv, NULL);
    check("prompts.update", rc == 0 && agentc_mcp_prompt_commit("srv", &pv) == 0 &&
                                agentc_mcp_prompt_count() == 3 &&
                                agentc_streq(prompt_desc_of(n1), "hi2") &&
                                agentc_prompts_has(n2));
    agentc_mcp_parsed_prompts_free(&pv);

    /* an empty list retires every live entry by name */
    AgcVec empty = { 0 };
    check("prompts.retire_empty", agentc_mcp_prompt_commit("srv", &empty) == 0 &&
                                      agentc_mcp_prompt_count() == 0 &&
                                      !agentc_prompts_has(n1) && !agentc_prompts_has(n2) &&
                                      !agentc_prompts_has(n3));

    /* frozen cap: 128 changed re-syncs succeed, the 129th is refused instead
     * of recycling the retired entries */
    agentc_mcp_prompt_registry_reset();
    check("prompts.churn_first", prompt_commit_one("srv", "echo", "v0") == 0 &&
                                     agentc_mcp_prompt_count() == 1);
    int accepted = 0;
    bool refused = false;
    for (int i = 1; i <= 200; i++) {
        char desc[32];
        agentc_snprintf(desc, sizeof desc, "v%d", i);
        int crc = prompt_commit_one("srv", "echo", desc);
        if (crc != 0) {
            refused = crc == -31;
            break;
        }
        accepted++;
    }
    check("prompts.frozen_cap", refused && accepted == 128 &&
                                    agentc_mcp_prompt_count() == 1);

    /* per-server live cap: 128 new identities commit, the 129th is -ENOSPC */
    agentc_mcp_prompt_registry_reset();
    AgcBuf cap = { 0 };
    agentc_buf_cstr(&cap, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"prompts\":[");
    for (int i = 0; i <= AGENTC_LIMIT_MCP_PROMPTS_PER_SERVER; i++) {
        char frag[32];
        agentc_snprintf(frag, sizeof frag, "%s{\"name\":\"p%03d\"}", i ? "," : "", i);
        agentc_buf_cstr(&cap, frag);
    }
    agentc_buf_cstr(&cap, "]}}");
    rc = agentc_mcp_parse_prompts((const char *)cap.p, cap.len, &pv, NULL);
    check("prompts.per_server_cap", rc == 0 && pv.len == AGENTC_LIMIT_MCP_PROMPTS_PER_SERVER + 1 &&
                                       agentc_mcp_prompt_commit("srv", &pv) == -28 &&
                                       agentc_mcp_prompt_count() == AGENTC_LIMIT_MCP_PROMPTS_PER_SERVER);
    agentc_mcp_parsed_prompts_free(&pv);
    agentc_buf_free(&cap);
    agentc_mcp_prompt_registry_reset();
}

/* End to end over the HTTP mock: capability-gated two-kind sync, prompt
 * pagination, the in-flight list_changed latch, argument encoding, text join,
 * the error path and the 64 KiB cap. */
static void test_prompt_mock(void) {
    agentc_rm_rf(TEST_ROOT);
    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");
    agentc_mcp_prompt_registry_reset();
    const char *cfg =
        "{\"servers\":{\"web\":{\"url\":\"http://api.test/mcp\",\"timeout_ms\":5000}}}\n";
    check("prompt.cfg",
          agentc_write_file_atomic(TEST_ROOT "/config/agentc/mcp.jsonc", cfg,
                                   agentc_strlen(cfg), 0644) == 0);
    AgcBuf script = { 0 };
    agentc_buf_cstr(&script, "dns api.test 203.0.113.5\n");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":"
                   "\"2024-11-05\",\"capabilities\":{\"tools\":{},\"prompts\":{"
                   "\"listChanged\":true}}}}");
    mock_push_accepted(&script);
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"echo\","
                   "\"description\":\"e\",\"inputSchema\":{\"type\":\"object\"}}]}}");
    /* round 1 page 1: the prompts list_changed arrives while the page is in
     * flight, so its follow-up round must survive the commit */
    mock_push_sse2(&script,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/prompts/list_changed\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"prompts\":["
        "{\"name\":\"greet\",\"description\":\"greet v1\",\"arguments\":["
        "{\"name\":\"name\",\"required\":true},{\"name\":\"tone\"}]},"
        "{\"name\":\"a b\"}],\"nextCursor\":\"pp2\"}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"prompts\":["
                   "{\"name\":\"a_b\"},{\"name\":\"summary\",\"arguments\":[{\"name\":\"count\"}]},"
                   "{\"name\":\"old\"},{\"name\":\"image\"},{\"name\":\"fail\"},"
                   "{\"name\":\"big\"},{\"name\":\"trigger\"}]}}");
    /* round 2: greet changed, `old` removed, `fresh` added */
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"prompts\":["
                   "{\"name\":\"greet\",\"description\":\"greet v2\",\"arguments\":["
                   "{\"name\":\"name\",\"required\":true},{\"name\":\"tone\"}]},"
                   "{\"name\":\"a b\"},{\"name\":\"fresh\"}],\"nextCursor\":\"pp2\"}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":{\"prompts\":["
                   "{\"name\":\"a_b\"},{\"name\":\"summary\",\"arguments\":[{\"name\":\"count\"}]},"
                   "{\"name\":\"image\"},{\"name\":\"fail\"},"
                   "{\"name\":\"big\"},{\"name\":\"trigger\"}]}}");
    /* expands: greet, summary, image, fail, big */
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"messages\":["
                   "{\"content\":[{\"type\":\"text\",\"text\":\"greet-1\"},"
                   "{\"type\":\"text\",\"text\":\"greet-2\"}]}]}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{\"messages\":["
                   "{\"content\":{\"type\":\"text\",\"text\":\"summary text\"}}]}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":{\"messages\":["
                   "{\"content\":[{\"type\":\"text\",\"text\":\"before\"},"
                   "{\"type\":\"image\",\"data\":\"aGk=\"},"
                   "{\"type\":\"text\",\"text\":\"after\"}]}]}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":10,\"error\":{\"code\":-32602,"
                   "\"message\":\"no such prompt\"}}");
    AgcBuf big = { 0 };
    agentc_buf_cstr(&big,
                "{\"jsonrpc\":\"2.0\",\"id\":11,\"result\":{\"messages\":[{\"content\":"
                "{\"type\":\"text\",\"text\":\"");
    for (size_t i = 0; i <= AGENTC_LIMIT_MCP_PROMPT_TEXT_BYTES; i++) agentc_buf_byte(&big, 'x');
    agentc_buf_cstr(&big, "\"}}]}}");
    mock_push_json(&script, (const char *)big.p);
    agentc_buf_free(&big);
    check("prompt.mock_write",
          agentc_write_file_atomic(TEST_ROOT "/prompt.mock", script.p, script.len, 0644) == 0);
    agentc_buf_free(&script);
    check("prompt.mock_load_file", agentc_mock_load(TEST_ROOT "/prompt.mock") == 0);

    check("prompt.load", agentc_mcp_load(TEST_ROOT, false) == 1);
    check("prompt.tools", agentc_mcp_tools(NULL, 0) == 1);
    check("prompt.count", agentc_mcp_prompt_count() == 9);
    char greet[80], summary[80], image[80], fail[80], big_name[80], fresh[80], old[80];
    check("prompt.greet", agentc_mcp_prompt_exposed("web", "greet", greet, sizeof greet));
    check("prompt.summary", agentc_mcp_prompt_exposed("web", "summary", summary, sizeof summary));
    check("prompt.image", agentc_mcp_prompt_exposed("web", "image", image, sizeof image));
    check("prompt.fail", agentc_mcp_prompt_exposed("web", "fail", fail, sizeof fail));
    check("prompt.big", agentc_mcp_prompt_exposed("web", "big", big_name, sizeof big_name));
    check("prompt.fresh", agentc_mcp_prompt_exposed("web", "fresh", fresh, sizeof fresh));
    check("prompt.old_removed", !agentc_mcp_prompt_exposed("web", "old", old, sizeof old));
    check("prompt.greet_v2", agentc_streq(prompt_desc_of(greet), "greet v2"));
    check("prompt.registry_live", agentc_prompts_has(greet) && agentc_prompts_has(fresh));

    /* args: key=value and positional, encoded into prompts/get */
    const AgcBuf *sent = agentc_mock_sent();
    char *text = agentc_prompts_expand(greet, "name=World tone=warm");
    check("prompt.expand_text", text && agentc_streq(text, "greet-1\n\ngreet-2"));
    agentc_free(text);
    check("prompt.args_kv",
          sent && sent->p && agentc_str_str((const char *)sent->p,
                                            "\"arguments\":{\"name\":\"World\",\"tone\":\"warm\"}") != NULL);
    text = agentc_prompts_expand(summary, "3");
    check("prompt.expand_positional", text && agentc_streq(text, "summary text"));
    agentc_free(text);
    check("prompt.args_positional",
          sent && sent->p && agentc_str_str((const char *)sent->p,
                                            "\"arguments\":{\"count\":\"3\"}") != NULL);

    text = agentc_prompts_expand(image, "");
    check("prompt.text_only", text && agentc_streq(text, "before\n\nafter"));
    agentc_free(text);
    check("prompt.error", agentc_prompts_expand(fail, "") == NULL);
    check("prompt.over_cap", agentc_prompts_expand(big_name, "") == NULL);
    size_t sent_before = sent ? sent->len : 0;
    check("prompt.missing_required", agentc_prompts_expand(greet, "") == NULL);
    check("prompt.no_request", sent && sent->len == sent_before);

    agentc_mcp_shutdown();
    check("prompt.shutdown_retired", agentc_mcp_prompt_count() == 0 &&
                                          !agentc_prompts_has(greet));
    agentc_mock_reset();
    agentc_rm_rf(TEST_ROOT);
}

/* B: the core prompt registry is capped and records are never freed. A
 * replacement must reuse a retired slot, otherwise the MCP
 * retire-then-register commit loses the prompt forever once the cap is hit. */
static void test_prompt_cap_reuse(void) {
    char name[40], last[40] = "";
    int rc = 0, guard = 0;
    for (int i = 0;; i++) {
        agentc_snprintf(name, sizeof name, "capfill_%04d", i);
        rc = agentc_prompts_register(name, "cap", "", "test", NULL, NULL);
        if (rc != 0) break;
        agentc_snprintf(last, sizeof last, "%s", name);
        if (++guard > AGENTC_PROMPTS_MAX * 2 + 16) break;
    }
    check("prompt_cap.full_blocks", rc == -28);
    check("prompt_cap.all_live", agentc_prompts_list(NULL, 0) == AGENTC_PROMPTS_MAX);
    check("prompt_cap.retire", last[0] && agentc_prompts_remove(last));
    check("prompt_cap.reuse",
          agentc_prompts_register("capfill_reuse", "cap", "", "test", NULL, NULL) == 0 &&
              agentc_prompts_has("capfill_reuse"));
    check("prompt_cap.reuse_count", agentc_prompts_list(NULL, 0) == AGENTC_PROMPTS_MAX);
}

/* A full MCP shutdown releases every entry, including ones handed out through
 * agentc_mcp_tools(); that snapshot API is therefore only valid until the next
 * shutdown (documented contract in include/mcp.h). The registry-held path is
 * retire-safe via the registry's internal_tool_run trampoline (covered by
 * ext_test), so the agent never calls a freed entry. */
static void test_shutdown_frozen(void) {
    agentc_mcp_registry_reset();
    size_t base = agentc_mem_live();
    check("shutrel.add",
          agentc_mcp_registry_add("srv", "echo", "d", "{\"type\":\"object\"}",
                                  AGENTC_TOOL_READONLY) == 0);
    AgcTool held[2];
    size_t n = agentc_mcp_tools(held, 2);   /* hands the entry out */
    check("shutrel.handed", n == 1 && held[0].run != NULL);
    agentc_mcp_shutdown();
    check("shutrel.empty", agentc_mcp_tools(NULL, 0) == 0);
    agentc_json_reset();
    check("shutrel.freed", agentc_mem_live() == base);
    agentc_mcp_registry_reset();
}

/* B: the session id echoed into the next request is attacker-controlled. A
 * lone control byte in the value (the HTTP tokenizer only splits on CRLF
 * pairs) must not inject a header line on the following request. */
static void test_session_id_injection(void) {
    agentc_rm_rf(TEST_ROOT);
    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");
    const char *cfg =
        "{\"servers\":{\"web\":{\"url\":\"http://api.test/mcp\",\"timeout_ms\":5000}}}\n";
    check("sid.cfg",
          agentc_write_file_atomic(TEST_ROOT "/config/agentc/mcp.jsonc", cfg,
                                   agentc_strlen(cfg), 0644) == 0);
    AgcBuf script = { 0 };
    agentc_buf_cstr(&script, "dns api.test 203.0.113.5\n");
    mock_push_json_hdr(&script, "Mcp-Session-Id: ok\ninjected: 1\r\n",
                       "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":"
                       "\"2024-11-05\"}}");
    mock_push_accepted(&script);
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"echo\","
                   "\"inputSchema\":{\"type\":\"object\"}}]}}");
    check("sid.mock_write",
          agentc_write_file_atomic(TEST_ROOT "/sid.mock", script.p, script.len, 0644) == 0);
    agentc_buf_free(&script);
    check("sid.mock_load", agentc_mock_load(TEST_ROOT "/sid.mock") == 0);

    agentc_log_set_level(4);   /* the invalid-id warning is not the subject */
    agentc_mcp_start(TEST_ROOT, false);
    bool ready = false;
    for (int i = 0; i < 20000 && !ready; i++) {
        ready = !agentc_mcp_pending() && agentc_mcp_tools(NULL, 0) == 1;
        if (!ready) { agentc_mcp_pump(); os_poll(NULL, 0, 1); }
    }
    agentc_log_set_level(-1);
    check("sid.ready", ready);
    const AgcBuf *sent = agentc_mock_sent();
    const char *sp = sent && sent->p ? (const char *)sent->p : "";
    size_t sl = sent ? sent->len : 0;
    check("sid.no_injection",
          agentc_str_find(sp, sl, "injected: 1", agentc_strlen("injected: 1")) < 0);
    agentc_mcp_shutdown();
    agentc_mock_reset();
    agentc_rm_rf(TEST_ROOT);
}

/* ------------------------------------------------ resource registry */
static void mk_resource(AgcVec *v, const char *uri, size_t pad) {
    McpParsedResource r;
    agentc_memset(&r, 0, sizeof r);
    r.uri = agentc_strdup(uri);
    r.name = agentc_strdup("n");
    r.title = agentc_strdup_len("tttttttt", pad < 8 ? pad : 8);
    r.description = agentc_strdup_len("dddddddd", pad < 8 ? pad : 8);
    r.mime = agentc_strdup("text/plain");
    *(McpParsedResource *)agentc_vec_push(v, sizeof r) = r;
}

static void test_resource_registry(void) {
    agentc_mcp_resource_registry_reset();
    const char *two =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"resources\":["
        "{\"uri\":\"file:///a.txt\",\"name\":\"a\"},"
        "{\"uri\":\"file:///b\",\"name\":\"b\"}]}}\n";
    AgcVec rv = { 0 };
    int rc = agentc_mcp_parse_resources(two, agentc_strlen(two), &rv, NULL);
    check("rreg.commit",
          rc == 0 && agentc_mcp_resource_commit("web", &rv, false) == 0 &&
              agentc_mcp_resource_count(false) == 2);
    agentc_mcp_parsed_resources_free(&rv);

    char *json = NULL;
    check("rreg.json_one",
          agentc_mcp_resource_list_json(false, "web", NULL, &json) == 0 && json &&
              agentc_streq(json,
                       "{\"server\":\"web\",\"resources\":["
                       "{\"server\":\"web\",\"uri\":\"file:///a.txt\",\"name\":\"a\","
                       "\"title\":\"\",\"description\":\"\",\"mimeType\":\"\"},"
                       "{\"server\":\"web\",\"uri\":\"file:///b\",\"name\":\"b\","
                       "\"title\":\"\",\"description\":\"\",\"mimeType\":\"\"}]}"));
    agentc_free(json);
    json = NULL;
    check("rreg.json_all",
          agentc_mcp_resource_list_json(false, NULL, NULL, &json) == 0 && json &&
              agentc_str_str(json, "\"server\":null") != NULL &&
              agentc_str_str(json, "file:///a.txt") != NULL);
    agentc_free(json);

    const char *tmpl =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"resourceTemplates\":["
        "{\"uriTemplate\":\"file:///{path}\",\"name\":\"t\"}]}}\n";
    AgcVec tv = { 0 };
    rc = agentc_mcp_parse_resource_templates(tmpl, agentc_strlen(tmpl), &tv, NULL);
    check("rreg.template_commit",
          rc == 0 && agentc_mcp_resource_commit("web", &tv, true) == 0 &&
              agentc_mcp_resource_count(true) == 1);
    agentc_mcp_parsed_resource_templates_free(&tv);
    json = NULL;
    check("rreg.json_template",
          agentc_mcp_resource_list_json(true, "web", NULL, &json) == 0 && json &&
              agentc_streq(json,
                       "{\"server\":\"web\",\"templates\":["
                       "{\"server\":\"web\",\"uriTemplate\":\"file:///{path}\","
                       "\"name\":\"t\",\"description\":\"\",\"mimeType\":\"\"}]}"));
    agentc_free(json);

    /* an unsupported cursor is a typed error, never an empty page */
    json = NULL;
    check("rreg.bad_cursor",
          agentc_mcp_resource_list_json(false, "web", "nope", &json) == -22 && json == NULL);

    /* the 64 KiB page cap: 11 entries of ~6 KiB paginate instead of failing */
    agentc_mcp_resource_registry_reset();
    AgcVec big = { 0 };
    char uri[AGENTC_LIMIT_MCP_RESOURCE_URI + 1];
    agentc_memset(uri, 'u', sizeof uri - 1);
    uri[sizeof uri - 1] = 0;
    for (int i = 0; i < 11; i++) {
        McpParsedResource r;
        agentc_memset(&r, 0, sizeof r);
        char name[16];
        agentc_snprintf(name, sizeof name, "r%d", i);
        r.uri = agentc_strdup(uri);
        r.name = agentc_strdup(name);
        r.title = agentc_strdup(uri);
        r.description = agentc_strdup(uri);
        r.mime = agentc_strdup("text/plain");
        *(McpParsedResource *)agentc_vec_push(&big, sizeof r) = r;
    }
    check("rreg.cap_commit", agentc_mcp_resource_commit("web", &big, false) == 0 &&
                                 agentc_mcp_resource_count(false) == 11);
    agentc_mcp_parsed_resources_free(&big);
    char *page1 = NULL;
    check("rreg.page1", agentc_mcp_resource_list_json(false, NULL, NULL, &page1) == 0 && page1 &&
                             agentc_str_str(page1, "\"nextCursor\":") != NULL &&
                             agentc_strlen(page1) <= AGENTC_LIMIT_MCP_RESOURCES_JSON_BYTES);
    const char *nc = page1 ? agentc_str_str(page1, "\"nextCursor\":\"") : NULL;
    char cursor[32] = { 0 };
    if (nc) {
        nc += agentc_strlen("\"nextCursor\":\"");
        size_t k = 0;
        while (nc[k] && nc[k] != '"' && k < sizeof cursor - 1) {
            cursor[k] = nc[k];
            k++;
        }
    }
    char *page2 = NULL;
    check("rreg.page2", cursor[0] &&
                             agentc_mcp_resource_list_json(false, NULL, cursor, &page2) == 0 &&
                             page2 && agentc_str_str(page2, "\"nextCursor\":") == NULL);
    agentc_free(page1);
    agentc_free(page2);

    /* per-server and total caps refuse, keeping the previous table */
    agentc_mcp_resource_registry_reset();
    AgcVec capv = { 0 };
    for (int i = 0; i < AGENTC_LIMIT_MCP_RESOURCES_PER_SERVER; i++) {
        char u[32];
        agentc_snprintf(u, sizeof u, "u%d", i);
        mk_resource(&capv, u, 0);
    }
    check("rreg.per_server_fit",
          agentc_mcp_resource_commit("srv", &capv, false) == 0 &&
              agentc_mcp_resource_count(false) == AGENTC_LIMIT_MCP_RESOURCES_PER_SERVER);
    mk_resource(&capv, "overflow", 0);
    check("rreg.per_server_cap", agentc_mcp_resource_commit("srv", &capv, false) == -28 &&
                                      agentc_mcp_resource_count(false) ==
                                          AGENTC_LIMIT_MCP_RESOURCES_PER_SERVER);
    agentc_mcp_parsed_resources_free(&capv);

    agentc_mcp_resource_registry_reset();
    for (int s = 0; s < 4; s++) {
        char srv[16];
        agentc_snprintf(srv, sizeof srv, "s%d", s);
        AgcVec sv = { 0 };
        for (int i = 0; i < AGENTC_LIMIT_MCP_RESOURCES_PER_SERVER; i++) {
            char u[32];
            agentc_snprintf(u, sizeof u, "u%d_%d", s, i);
            mk_resource(&sv, u, 0);
        }
        if (agentc_mcp_resource_commit(srv, &sv, false) != 0) fails = 1;
        agentc_mcp_parsed_resources_free(&sv);
    }
    AgcVec one = { 0 };
    mk_resource(&one, "u_one", 0);
    check("rreg.total_cap", agentc_mcp_resource_count(false) == AGENTC_LIMIT_MCP_RESOURCES_TOTAL &&
                               agentc_mcp_resource_commit("s4", &one, false) == -28 &&
                               agentc_mcp_resource_count(false) == AGENTC_LIMIT_MCP_RESOURCES_TOTAL);
    agentc_mcp_parsed_resources_free(&one);
    agentc_mcp_resource_registry_reset();
}

/* --------------------------------------------- resource tools/mock */
static size_t rtool_count(const char *name) {
    size_t total = agentc_ext_tools(NULL, 0);
    AgcTool *arr = agentc_alloc((total ? total : 1) * sizeof *arr);
    size_t n = agentc_ext_tools(arr, total), found = 0;
    for (size_t i = 0; i < n; i++)
        if (arr[i].name && agentc_streq(arr[i].name, name)) found++;
    agentc_free(arr);
    return found;
}

/* Run one registry tool by name. */
static bool rtool_run(const char *name, const char *args, char **out, bool *is_error) {
    if (out) *out = NULL;
    if (is_error) *is_error = true;
    size_t total = agentc_ext_tools(NULL, 0);
    AgcTool *arr = agentc_alloc((total ? total : 1) * sizeof *arr);
    size_t n = agentc_ext_tools(arr, total);
    AgcTool tool;
    agentc_memset(&tool, 0, sizeof tool);
    bool have = false;
    for (size_t i = 0; i < n; i++)
        if (arr[i].name && agentc_streq(arr[i].name, name)) {
            tool = arr[i];
            have = true;
            break;
        }
    agentc_free(arr);
    if (!have || !tool.run) return false;
    bool err = false;
    char *res = run_tool(&tool, args, &err);
    if (out) *out = res;
    else agentc_free(res);
    if (is_error) *is_error = err;
    return true;
}

static void test_resource_mock(void) {
    agentc_rm_rf(TEST_ROOT);
    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");
    agentc_mcp_resource_registry_reset();
    agentc_ext_shutdown();   /* a fresh publication attempt for this phase */
    const char *cfg =
        "{\"servers\":{\"web\":{\"url\":\"http://api.test/mcp\",\"timeout_ms\":5000}}}\n";
    check("resmock.cfg",
          agentc_write_file_atomic(TEST_ROOT "/config/agentc/mcp.jsonc", cfg,
                                   agentc_strlen(cfg), 0644) == 0);
    AgcBuf script = { 0 };
    agentc_buf_cstr(&script, "dns api.test 203.0.113.5\n");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":"
                   "\"2024-11-05\",\"capabilities\":{\"resources\":{\"listChanged\":true}}}}");
    mock_push_accepted(&script);
    /* resources round 1 page 1: the list_changed arrives in flight, so the
     * resources kind latches a second round before templates run */
    mock_push_sse2(&script,
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/resources/list_changed\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"resources\":["
        "{\"uri\":\"file:///a1\",\"name\":\"a1\"}],\"nextCursor\":\"r2\"}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"resources\":["
                   "{\"uri\":\"file:///b\",\"name\":\"b\"}]}}");
    /* the latched follow-up replaces the table */
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"resources\":["
                   "{\"uri\":\"file:///a2\",\"name\":\"a2\"},"
                   "{\"uri\":\"file:///b\",\"name\":\"b\"}]}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"resourceTemplates\":["
                   "{\"uriTemplate\":\"file:///{path}\",\"name\":\"t\"}]}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":{\"contents\":["
                   "{\"uri\":\"file:///a2\",\"mimeType\":\"text/plain\",\"text\":\"alpha\"},"
                   "{\"uri\":\"file:///a2\",\"text\":\"beta\"}]}}");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"contents\":["
                   "{\"uri\":\"file:///b.bin\",\"blob\":\"aGk=\"}]}}");
    check("resmock.mock_write",
          agentc_write_file_atomic(TEST_ROOT "/res.mock", script.p, script.len, 0644) == 0);
    agentc_buf_free(&script);
    check("resmock.mock_load", agentc_mock_load(TEST_ROOT "/res.mock") == 0);

    agentc_ext_register_defaults(false);
    agentc_ext_load_all();
    check("resmock.lazy", rtool_count("mcp_list_resources") == 0);
    bool settled = false;
    for (int i = 0; i < 20000 && !settled; i++) {
        settled = !agentc_mcp_pending() && agentc_mcp_resource_count(true) == 1;
        if (!settled) {
            agentc_ext_pump();
            os_poll(NULL, 0, 1);
        }
    }
    check("resmock.settled", settled);
    check("resmock.tools_published",
          rtool_count("mcp_list_resources") == 1 &&
              rtool_count("mcp_list_resource_templates") == 1 &&
              rtool_count("mcp_read_resource") == 1);
    check("resmock.resources", agentc_mcp_resource_count(false) == 2 &&
                                    agentc_mcp_resource_count(true) == 1);
    {
        size_t total = agentc_ext_tools(NULL, 0);
        AgcTool *arr = agentc_alloc((total ? total : 1) * sizeof *arr);
        size_t n = agentc_ext_tools(arr, total);
        bool ro = true;
        for (size_t i = 0; i < n; i++)
            if (arr[i].name && agentc_str_str(arr[i].name, "mcp_") == arr[i].name &&
                (arr[i].flags & AGENTC_TOOL_READONLY) == 0)
                ro = false;
        check("resmock.tools_readonly", ro);
        agentc_free(arr);
    }

    char *out = NULL;
    bool err = true;
    check("resmock.list_all",
          rtool_run("mcp_list_resources", "{}", &out, &err) && !err && out &&
              agentc_str_str(out, "\"server\":null") != NULL &&
              agentc_str_str(out, "file:///a2") != NULL &&
              agentc_str_str(out, "file:///b") != NULL);
    agentc_free(out);
    check("resmock.list_one",
          rtool_run("mcp_list_resources", "{\"server\":\"web\"}", &out, &err) && !err &&
              out && agentc_str_str(out, "\"server\":\"web\"") != NULL);
    agentc_free(out);
    check("resmock.list_unknown",
          rtool_run("mcp_list_resources", "{\"server\":\"nope\"}", &out, &err) && err &&
              out &&
              agentc_str_str(out, "not connected with the resource capability") != NULL);
    agentc_free(out);
    check("resmock.templates",
          rtool_run("mcp_list_resource_templates", "{}", &out, &err) && !err && out &&
              agentc_str_str(out, "file:///{path}") != NULL);
    agentc_free(out);
    check("resmock.read_text",
          rtool_run("mcp_read_resource", "{\"server\":\"web\",\"uri\":\"file:///a2\"}",
                    &out, &err) &&
              !err && out && agentc_streq(out, "alpha\n\nbeta"));
    agentc_free(out);
    check("resmock.read_blob",
          rtool_run("mcp_read_resource", "{\"server\":\"web\",\"uri\":\"file:///b.bin\"}",
                    &out, &err) &&
              err && out && agentc_str_str(out, "binary resource not supported") != NULL);
    agentc_free(out);

    /* a server without the resource capability (or not connected) never sends
     * a request */
    const AgcBuf *sent = agentc_mock_sent();
    size_t sent_before = sent ? sent->len : 0;
    check("resmock.read_unknown",
          rtool_run("mcp_read_resource", "{\"server\":\"nope\",\"uri\":\"u\"}", &out, &err) &&
              err && out &&
              agentc_str_str(out, "not connected with the resource capability") != NULL);
    agentc_free(out);
    check("resmock.malformed_args",
          rtool_run("mcp_list_resources", "not json", &out, &err) && err && out &&
              agentc_str_str(out, "malformed") != NULL);
    agentc_free(out);
    sent = agentc_mock_sent();
    check("resmock.no_request", sent && sent->len == sent_before);

    agentc_ext_shutdown();
    agentc_mcp_shutdown();
    check("resmock.shutdown_empty", agentc_mcp_resource_count(false) == 0 &&
                                        agentc_mcp_resource_count(true) == 0);
    agentc_mock_reset();
    agentc_rm_rf(TEST_ROOT);
}

static void test_resource_gating(void) {
    agentc_rm_rf(TEST_ROOT);
    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");
    agentc_mcp_resource_registry_reset();
    agentc_ext_shutdown();
    const char *cfg =
        "{\"servers\":{\"web\":{\"url\":\"http://api.test/mcp\",\"timeout_ms\":5000}}}\n";
    check("rgate.cfg",
          agentc_write_file_atomic(TEST_ROOT "/config/agentc/mcp.jsonc", cfg,
                                   agentc_strlen(cfg), 0644) == 0);
    AgcBuf script = { 0 };
    agentc_buf_cstr(&script, "dns api.test 203.0.113.5\n");
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":"
                   "\"2024-11-05\",\"capabilities\":{\"tools\":{}}}}");
    mock_push_accepted(&script);
    mock_push_json(&script,
                   "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":["
                   "{\"name\":\"echo\",\"inputSchema\":{\"type\":\"object\"}}]}}");
    check("rgate.mock_write",
          agentc_write_file_atomic(TEST_ROOT "/rgate.mock", script.p, script.len, 0644) == 0);
    agentc_buf_free(&script);
    check("rgate.mock_load", agentc_mock_load(TEST_ROOT "/rgate.mock") == 0);

    agentc_ext_register_defaults(false);
    agentc_ext_load_all();
    bool settled = false;
    for (int i = 0; i < 20000 && !settled; i++) {
        settled = !agentc_mcp_pending() && agentc_mcp_tools(NULL, 0) == 1;
        if (!settled) {
            agentc_ext_pump();
            os_poll(NULL, 0, 1);
        }
    }
    check("rgate.settled", settled);
    check("rgate.tools_absent",
          rtool_count("mcp_list_resources") == 0 &&
              rtool_count("mcp_list_resource_templates") == 0 &&
              rtool_count("mcp_read_resource") == 0);
    check("rgate.tables_empty", agentc_mcp_resource_count(false) == 0 &&
                                    agentc_mcp_resource_count(true) == 0);
    const AgcBuf *sent = agentc_mock_sent();
    check("rgate.no_resource_request",
          sent && sent->p && agentc_str_str((const char *)sent->p, "resources/") == NULL);
    agentc_ext_shutdown();
    agentc_mcp_shutdown();
    agentc_mock_reset();
    agentc_rm_rf(TEST_ROOT);
}

static void test_mem_cycles(void) {
    /* keep loads sandboxed regardless of the real environment */
    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");
    agentc_rm_rf(TEST_ROOT);

    agentc_mcp_registry_reset();
    (void)agentc_json_parse("[1]", 3);
    agentc_json_reset();
    size_t base = agentc_mem_live();
    for (int i = 0; i < 3; i++) {
        if (i == 0) check("mem.load0", agentc_mcp_load(TEST_ROOT, false) == 0);
        (void)agentc_mcp_registry_add("srv", "tool", "desc", "{\"type\":\"object\"}",
                                  AGENTC_TOOL_READONLY);
        /* resource commits/resets share the same baseline */
        AgcVec rv = { 0 };
        const char *rmsg =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"resources\":["
            "{\"uri\":\"u\",\"name\":\"n\"}]}}";
        if (agentc_mcp_parse_resources(rmsg, agentc_strlen(rmsg), &rv, NULL) == 0)
            (void)agentc_mcp_resource_commit("srv", &rv, false);
        agentc_mcp_parsed_resources_free(&rv);
        /* Snapshot without an out buffer: handing the entry out would freeze
         * it for the process lifetime and break the memory baseline. */
        if (agentc_mcp_tools(NULL, 0) != 1) fails = 1;
        agentc_mcp_shutdown();
    }
    AgcTool tl[2];
    check("mem.shutdown_empty", agentc_mcp_tools(tl, 2) == 0 && agentc_mcp_server_count() == 0);
    agentc_json_reset();
    check("mem.live", agentc_mem_live() == base);
    /* set_context copies the cwd; shutdown must free it (no leak). */
    agentc_mcp_set_context(TEST_ROOT, false);
    agentc_mcp_shutdown();
    check("cwd.freed", agentc_mem_live() == base);
    /* The cwd context is now a static buffer: clear it explicitly (no free). */
    agentc_mcp_set_context(NULL, false);
    agentc_rm_rf(TEST_ROOT);
}

/* The mcp pump passes the process cwd context (g_mcp_cwd) straight into
 * agentc_mcp_start, which calls agentc_mcp_shutdown and frees that context.
 * A trusted project config must still be loaded after the copy-before-shutdown
 * fix; without it this path reads freed memory and drops the project server. */
static void test_trusted_project_cwd(void) {
    agentc_test_setenv("HOME", TEST_ROOT "/tcwd-home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/tcwd-config");
    agentc_rm_rf(TEST_ROOT);
    /* The servers only need to exist as records: the check below is the count,
     * not a running process. Use an absolute path that cannot exist on any
     * host, so no spawn succeeds and (for the Wine suite) no Unix ELF is handed
     * to the Windows loader; an absolute path also keeps the MCP layer from
     * wrapping the name in /bin/sh. */
    const char *user_cfg =
        "{\"servers\":{\"userSrv\":{\"command\":\"/nonexistent/agentc-test-srv\"}}}";
    const char *proj_cfg =
        "{\"servers\":{\"projSrv\":{\"command\":\"/nonexistent/agentc-test-srv\"}}}";
    check("tcwd.user_cfg",
          agentc_write_file_atomic(TEST_ROOT "/tcwd-config/agentc/mcp.jsonc", user_cfg,
                                   agentc_strlen(user_cfg), 0644) == 0);
    check("tcwd.proj_cfg",
          agentc_write_file_atomic(TEST_ROOT "/tcwd-proj/.agentc/mcp.jsonc", proj_cfg,
                                   agentc_strlen(proj_cfg), 0644) == 0);

    agentc_mcp_set_context(TEST_ROOT "/tcwd-proj", true);
    agentc_log_set_level(4);   /* the dummy server fails; keep the golden quiet */
    agentc_ext_register_defaults(false);
    agentc_ext_load_all();
    /* One pump creates the records for both configs. */
    for (int i = 0; i < 4; i++) agentc_ext_pump();
    agentc_log_set_level(0);
    check("tcwd.both_servers", agentc_mcp_server_count() == 2);
    agentc_ext_shutdown();
    agentc_mcp_shutdown();
    agentc_mcp_set_context(NULL, false);
    agentc_rm_rf(TEST_ROOT);
}

/* --------------------------------------------------------------- live mode */
static int live_fails;

static void lcheck(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) live_fails = 1;
}

static int live_main(void) {
    /* warm the JSON arena and its child-vector scratch so the leak check
     * compares steady states */
    (void)agentc_json_parse("{\"warm\":[1]}", 12);
    agentc_json_reset();
    size_t base = agentc_mem_live();

    /* start() returns before any server is connected. */
    agentc_mcp_start(NULL, false);
    lcheck("start_no_tools", agentc_mcp_tools(NULL, 0) == 0);
    lcheck("pending_start", agentc_mcp_pending());
    /* tools() is a pure snapshot: no connect, no blocking I/O. */
    i64 snap0 = os_now_ns(OS_CLOCK_MONOTONIC);
    size_t snap = agentc_mcp_tools(NULL, 0);
    i64 snap_ms = (os_now_ns(OS_CLOCK_MONOTONIC) - snap0) / 1000000;
    lcheck("snapshot_new_fast", snap == 0 && snap_ms < 1000);
    lcheck("server_count", agentc_mcp_server_count() == 2);
    /* One pump is only the transport setup: still connecting, not ready. */
    agentc_mcp_pump();
    lcheck("partial_no_ready", agentc_mcp_tools(NULL, 0) == 0);
    /* Pump until the mock server becomes READY (bounded). */
    size_t connected = 0;
    i64 cap = os_now_ns(OS_CLOCK_MONOTONIC) + 15000000000LL;
    for (int i = 0; i < 20000; i++) {
        connected = agentc_mcp_tools(NULL, 0);
        if (connected > 0) break;
        agentc_mcp_pump();
        os_poll(NULL, 0, 1);
        if (os_now_ns(OS_CLOCK_MONOTONIC) > cap) break;
    }
    lcheck("load", connected > 0);
    /* Both servers (mock ready, bad failed) are terminal, so the app's startup
     * pump would exit early instead of spending the whole budget. */
    lcheck("pending_settled", !agentc_mcp_pending());
    AgcTool tools[8];
    size_t n = agentc_mcp_tools(tools, 8);
    lcheck("tools", n == 5);
    const AgcTool *echo = find_tool(tools, n, "mcp__mock__echo");
    const AgcTool *fail = find_tool(tools, n, "mcp__mock__fail");
    const AgcTool *big = find_tool(tools, n, "mcp__mock__bigschema");
    const AgcTool *bad = find_tool(tools, n, "mcp__mock__badschema");
    const AgcTool *trig = find_tool(tools, n, "mcp__mock__trigger");
    lcheck("expose_echo", echo != NULL);
    lcheck("expose_fail", fail != NULL);
    lcheck("expose_bigschema", big != NULL);
    lcheck("expose_badjsonschema", bad != NULL);
    lcheck("expose_trigger", trig != NULL);
    lcheck("echo_readonly", echo && (echo->flags & AGENTC_TOOL_READONLY));
    lcheck("schema_fallback_big",
           big && agentc_streq(big->params_json, "{\"type\":\"object\"}"));
    lcheck("schema_fallback_bad",
           bad && agentc_streq(bad->params_json, "{\"type\":\"object\"}"));

    if (echo) {
        bool err = false;
        char *res = run_tool(echo, "{\"text\":\"hello mcp\"}", &err);
        const char *want = os_getenv("MCP_EXPECT_ECHO");
        if (!want) want = "hello mcp";
        lcheck("echo_result", !err && res && agentc_str_str(res, want) != NULL);
        lcheck("echo_structured", res && agentc_str_str(res, "structuredContent") == NULL &&
                                     agentc_str_str(res, "{\"echo\":") != NULL);
        agentc_free(res);
    } else {
        lcheck("echo_result", false);
        lcheck("echo_structured", false);
    }
    if (fail) {
        bool err = false;
        char *res = run_tool(fail, "{}", &err);
        lcheck("fail_is_error", err && res && agentc_str_str(res, "boom") != NULL);
        agentc_free(res);
    } else {
        lcheck("fail_is_error", false);
    }

    /* Keep the handed-out table across a server-side re-sync that changes a
     * description and removes tools: no string may be freed or slot recycled. */
    const char *echo0_desc = echo ? echo->desc : NULL;
    const char *fail0_desc = fail ? fail->desc : NULL;
    void *echo0_ud = echo ? echo->ud : NULL;
    void *fail0_ud = fail ? fail->ud : NULL;
    if (trig) {
        bool err = false;
        char *res = run_tool(trig, "{}", &err);
        lcheck("trigger_call", !err && res != NULL);
        agentc_free(res);
    } else {
        lcheck("trigger_call", false);
    }
    /* The list_changed notification sets sync_pending during the tool call;
     * tools() must stay a pure snapshot and the pump owns the re-sync. */
    lcheck("resync_before_pump", agentc_mcp_tools(NULL, 0) == 5);
    /* Overlap: send the re-sync request, then run a synchronous exchange
     * before the pump reads the reply. Two pumps make one full round-robin
     * rotation, so the sync is in SYNC_WAIT on the mock regardless of the
     * cursor. The exchange consumes and drops the pump's tools/list response
     * as an unknown id, so the sync times out on an already-ready server; it
     * must be re-armed, not abandoned, and the retry below commits the
     * mutated list. */
    agentc_mcp_pump();   /* READY -> SYNC_SEND: tools/list hits the wire */
    agentc_mcp_pump();   /* the other server only; mock stays in SYNC_WAIT */
    if (echo) {
        bool err = false;
        char *res = run_tool(echo, "{\"text\":\"overlap\"}", &err);
        lcheck("resync_overlap_exchange", !err && res != NULL);
        agentc_free(res);
    } else {
        lcheck("resync_overlap_exchange", false);
    }
    i64 race_cap = os_now_ns(OS_CLOCK_MONOTONIC) + 7000000000LL;
    while (agentc_mcp_tools(NULL, 0) == 5 && os_now_ns(OS_CLOCK_MONOTONIC) < race_cap) {
        agentc_mcp_pump();
        os_poll(NULL, 0, 5);
    }
    lcheck("resync_overlap_retry", agentc_mcp_tools(NULL, 0) == 4);
    for (int i = 0; i < 20000; i++) {
        if (agentc_mcp_tools(NULL, 0) == 4) break;
        agentc_mcp_pump();
        os_poll(NULL, 0, 1);
    }
    AgcTool tools2[8];
    size_t n2 = agentc_mcp_tools(tools2, 8);
    lcheck("resync_tools", n2 == 4);
    const AgcTool *echo1 = find_tool(tools2, n2, "mcp__mock__echo");
    const AgcTool *fresh = find_tool(tools2, n2, "mcp__mock__fresh");
    const AgcTool *fail1 = find_tool(tools2, n2, "mcp__mock__fail");
    lcheck("resync_echo_desc", echo1 && agentc_streq(echo1->desc, "echo v2 (mutated)"));
    lcheck("resync_fresh", fresh != NULL);
    lcheck("resync_fail_removed", fail1 == NULL);
    lcheck("resync_old_echo_desc",
           echo0_desc && agentc_streq(echo0_desc, "echo the text argument"));
    lcheck("resync_old_fail_desc", fail0_desc && agentc_streq(fail0_desc, "always fails"));
    lcheck("resync_old_ud_stable", echo0_ud != NULL && fail0_ud != NULL);
    if (echo) {
        bool err = false;
        char *res = run_tool(echo, "{\"text\":\"x\"}", &err);
        lcheck("resync_old_echo_error",
               err && res && agentc_str_str(res, "no longer available") != NULL);
        agentc_free(res);
    } else {
        lcheck("resync_old_echo_error", false);
    }
    if (fail) {
        bool err = false;
        char *res = run_tool(fail, "{}", &err);
        lcheck("resync_old_fail_error",
               err && res && agentc_str_str(res, "no longer available") != NULL);
        agentc_free(res);
    } else {
        lcheck("resync_old_fail_error", false);
    }

    /* A shutdown while a connect is in flight must tear everything down and
     * return the module to its memory baseline. */
    agentc_mcp_start(NULL, false);
    agentc_mcp_pump();   /* spawn the transport; handshake still in flight */
    agentc_mcp_shutdown();

    agentc_mcp_shutdown();
    agentc_json_reset();
    lcheck("mem_live", agentc_mem_live() == base);
    return live_fails;
}

/* ================================================= registry/pump live mode
 *
 * tests/mcp.sh writes the config roots and the p2 mock, then runs
 * `mcp_test --live-p2`. Four phases:
 *   registry  real registry + stdio mock: publish, mcp_servers_change,
 *             re-sync update and retiral (no ghosts in agentc_ext_tools)
 *   inflight  a list_changed delivered during tools/list must survive the
 *             commit as a follow-up sync
 *   dup       a colliding published name fails once and is not retried on
 *             later re-syncs (one log line, foreign tool stays)
 *   pump      two stalled HTTP servers: one pump call touches one server
 */

static int p2_fails;

static void pcheck(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) p2_fails = 1;
}

static int g_srv_change;
static char g_srv_payload[1024];          /* last emitted payload */
static char g_srv_first[1024];            /* first payload (records created) */
static bool g_srv_first_seen;

static int p2_on_servers_change(void *ud, const char *point, const char *payload_json,
                                char **result_json) {
    (void)ud; (void)point; (void)result_json;
    g_srv_change++;
    if (payload_json) {
        if (!g_srv_first_seen) {
            g_srv_first_seen = true;
            agentc_snprintf(g_srv_first, sizeof g_srv_first, "%s", payload_json);
        }
        agentc_snprintf(g_srv_payload, sizeof g_srv_payload, "%s", payload_json);
    }
    return 0;
}

static AgcTool *p2_collect(size_t *n_out) {
    size_t total = agentc_ext_tools(NULL, 0);
    AgcTool *arr = agentc_alloc((total ? total : 1) * sizeof *arr);
    size_t n = agentc_ext_tools(arr, total);
    if (n_out) *n_out = n;
    return arr;
}

static size_t p2_count_named(const char *name) {
    size_t n = 0, found = 0;
    AgcTool *arr = p2_collect(&n);
    for (size_t i = 0; i < n; i++)
        if (arr[i].name && agentc_streq(arr[i].name, name)) found++;
    agentc_free(arr);
    return found;
}

static bool p2_tool_desc_is(const char *name, const char *desc) {
    size_t n = 0;
    AgcTool *arr = p2_collect(&n);
    bool ok = false;
    for (size_t i = 0; i < n; i++)
        if (arr[i].name && agentc_streq(arr[i].name, name)) {
            ok = arr[i].desc && agentc_streq(arr[i].desc, desc);
            break;
        }
    agentc_free(arr);
    return ok;
}

/* Run a tool as the registry exposes it. The copied AgcTool is only valid for
 * the duration of the call (the record behind it is stable until a re-sync). */
static bool p2_run_registry_tool(const char *name, const char *args) {
    size_t n = 0;
    AgcTool *arr = p2_collect(&n);
    AgcTool tool;
    agentc_memset(&tool, 0, sizeof tool);
    bool have = false;
    for (size_t i = 0; i < n; i++)
        if (arr[i].name && agentc_streq(arr[i].name, name)) {
            tool = arr[i];
            have = true;
            break;
        }
    agentc_free(arr);
    if (!have || !tool.run) return false;
    bool err = false;
    char *res = run_tool(&tool, args, &err);
    bool ok = !err && res != NULL;
    agentc_free(res);
    return ok;
}

static void p2_phase_registry(void) {
    g_srv_change = 0;
    g_srv_payload[0] = 0;
    g_srv_first[0] = 0;
    g_srv_first_seen = false;
    (void)agentc_ext_host()->on("mcp_servers_change", AGENTC_HOOK_OBSERVE, 0,
                                p2_on_servers_change, NULL);
    agentc_ext_register_defaults(false);
    agentc_ext_load_all();

    pcheck("ext.no_tools_before_pump", p2_count_named("mcp__mock__echo") == 0);
    bool published = false;
    for (int i = 0; i < 20000 && !published; i++) {
        published = p2_count_named("mcp__mock__echo") > 0;
        if (!published) {
            agentc_ext_pump();
            os_poll(NULL, 0, 1);
        }
    }
    pcheck("ext.tool_published", published);
    pcheck("ext.tool_desc_initial",
           p2_tool_desc_is("mcp__mock__echo", "echo the text argument"));
    pcheck("ext.fail_published", p2_count_named("mcp__mock__fail") == 1);
    int connect_changes = g_srv_change;
    /* payload: record creation publishes every server as connecting and
     * withholds caps until initialize completed. */
    pcheck("ext.servers_change_connect",
           connect_changes >= 1 && g_srv_first_seen &&
               agentc_str_str(g_srv_first,
                              "\"name\":\"mock\",\"kind\":\"stdio\",\"status\":\"connecting\"") != NULL &&
               agentc_str_str(g_srv_first,
                              "\"name\":\"bad\",\"kind\":\"stdio\",\"status\":\"connecting\"") != NULL &&
               agentc_str_str(g_srv_first, "\"caps\"") == NULL);
    /* after the mock is ready the payload carries ready+caps for it and failed
     * for the bad server (failure emit); the failed record has no caps. */
    pcheck("ext.servers_change_ready",
           agentc_str_str(g_srv_payload,
                          "\"name\":\"mock\",\"kind\":\"stdio\",\"status\":\"ready\","
                          "\"caps\":[\"tools\"]") != NULL);
    pcheck("ext.servers_change_failed",
           agentc_str_str(g_srv_payload,
                          "\"name\":\"bad\",\"kind\":\"stdio\",\"status\":\"failed\"}") != NULL);

    pcheck("ext.trigger_call", p2_run_registry_tool("mcp__mock__trigger", "{}"));
    bool resynced = false;
    for (int i = 0; i < 20000 && !resynced; i++) {
        resynced = p2_count_named("mcp__mock__fresh") == 1 &&
                   p2_tool_desc_is("mcp__mock__echo", "echo v2 (mutated)") &&
                   p2_count_named("mcp__mock__fail") == 0;
        if (!resynced) {
            agentc_ext_pump();
            os_poll(NULL, 0, 1);
        }
    }
    pcheck("ext.resync_applied", resynced);
    pcheck("ext.resync_echo_desc",
           p2_tool_desc_is("mcp__mock__echo", "echo v2 (mutated)"));
    pcheck("ext.resync_fresh", p2_count_named("mcp__mock__fresh") == 1);
    pcheck("ext.resync_fail_removed", p2_count_named("mcp__mock__fail") == 0);
    pcheck("ext.echo_unique", p2_count_named("mcp__mock__echo") == 1);
    pcheck("ext.servers_change_resync", g_srv_change > connect_changes);
    agentc_ext_shutdown();
}

static void p2_phase_inflight(void) {
    const char *xdg = os_getenv("MCP_INFLIGHT_XDG");
    if (xdg) agentc_test_setenv("XDG_CONFIG_HOME", xdg);
    agentc_mcp_start(NULL, false);
    bool v2 = false;
    for (int i = 0; i < 20000 && !v2; i++) {
        AgcTool tl[4];
        size_t n = agentc_mcp_tools(tl, 4);
        for (size_t k = 0; k < n; k++)
            if (tl[k].name && agentc_streq(tl[k].name, "mcp__p2__echo") &&
                tl[k].desc && agentc_streq(tl[k].desc, "v2"))
                v2 = true;
        if (!v2) {
            agentc_mcp_pump();
            os_poll(NULL, 0, 1);
        }
    }
    pcheck("inflight.followup_sync", v2);
    agentc_mcp_shutdown();
}

static int p2_foreign_run(const AgcExtHost *host, const AgcExtTool *self,
                          const AgcExtToolCall *call, void *out, bool *is_error) {
    (void)self; (void)call;
    if (is_error) *is_error = false;
    host->out_write(out, "foreign", 7);
    return 0;
}

static void p2_phase_dup(void) {
    const char *xdg = os_getenv("MCP_DUP_XDG");
    if (xdg) agentc_test_setenv("XDG_CONFIG_HOME", xdg);

    /* This name collides with the mcp-published echo: its add must fail and be
     * skipped on every later re-sync instead of retried and re-logged. */
    AgcExtTool ft;
    agentc_memset(&ft, 0, sizeof ft);
    ft.struct_size = sizeof ft;
    ft.name = "mcp__p2__echo";
    ft.label = "foreign echo";
    ft.description = "foreign";
    ft.parameters_json = "{\"type\":\"object\"}";
    ft.run = p2_foreign_run;
    agentc_ext_host()->add_tool(&ft);

    g_srv_change = 0;
    (void)agentc_ext_host()->on("mcp_servers_change", AGENTC_HOOK_OBSERVE, 0,
                                p2_on_servers_change, NULL);
    agentc_ext_register_defaults(false);
    agentc_ext_load_all();

    bool ready = false;
    for (int i = 0; i < 20000 && !ready; i++) {
        ready = p2_count_named("mcp__p2__poke") == 1;
        if (!ready) {
            agentc_ext_pump();
            os_poll(NULL, 0, 1);
        }
    }
    pcheck("dup.poke_published", ready);
    pcheck("dup.foreign_echo_kept", p2_tool_desc_is("mcp__p2__echo", "foreign"));

    bool poked = true;
    for (int round = 0; round < 3 && poked; round++) {
        int before = g_srv_change;
        poked = p2_run_registry_tool("mcp__p2__poke", "{}");
        bool synced = false;
        for (int i = 0; i < 20000 && poked && !synced; i++) {
            synced = g_srv_change > before;
            if (!synced) {
                agentc_ext_pump();
                os_poll(NULL, 0, 1);
            }
        }
        if (poked && !synced) poked = false;
    }
    pcheck("dup.poke_resyncs", poked);
    pcheck("dup.foreign_echo_stable", p2_tool_desc_is("mcp__p2__echo", "foreign"));
    agentc_ext_shutdown();
}

static bool p2_file_has(const char *path, const char *needle) {
    size_t n = 0;
    char *s = agentc_read_file_owned(path, &n);
    if (!s) return false;
    bool ok = agentc_str_str(s, needle) != NULL;
    agentc_free(s);
    return ok;
}

static bool p2_wait_file(const char *path, const char *needle) {
    for (int i = 0; i < 300; i++) {
        if (p2_file_has(path, needle)) return true;
        os_poll(NULL, 0, 10);
    }
    return false;
}

static void p2_phase_pump(void) {
    const char *xdg = os_getenv("MCP_PUMP_XDG");
    const char *loga = os_getenv("MCP_PUMP_LOGA");
    const char *logb = os_getenv("MCP_PUMP_LOGB");
    if (xdg) agentc_test_setenv("XDG_CONFIG_HOME", xdg);
    agentc_mcp_start(NULL, false);
    pcheck("pump.server_count", agentc_mcp_server_count() == 2);
    if (!loga || !logb) {
        pcheck("pump.one_per_call", false);
        pcheck("pump.second_call", false);
        agentc_mcp_shutdown();
        return;
    }
    /* Two round-robin calls move both servers NEW -> INIT_SEND; the next call
     * must write initialize to server a only (one step per pump), and the one
     * after that to server b. The mocks log every request they receive. */
    agentc_mcp_pump();
    agentc_mcp_pump();
    agentc_mcp_pump();
    bool a_first = p2_wait_file(loga, "initialize");
    /* Give a loop-all implementation ample time to expose server b's write
     * before asserting it is absent. */
    for (int i = 0; i < 50 && !p2_file_has(logb, "initialize"); i++)
        os_poll(NULL, 0, 10);
    bool b_early = p2_file_has(logb, "initialize");
    pcheck("pump.one_per_call", a_first && !b_early);
    agentc_mcp_pump();
    pcheck("pump.second_call", p2_wait_file(logb, "initialize"));
    agentc_mcp_shutdown();
}

static int live_p2_main(void) {
    (void)agentc_json_parse("{\"warm\":[1]}", 12);
    agentc_json_reset();
    size_t base = agentc_mem_live();

    p2_phase_registry();
    p2_phase_inflight();
    p2_phase_dup();
    p2_phase_pump();

    agentc_ext_shutdown();
    agentc_mcp_shutdown();
    agentc_test_clearenv();
    agentc_json_reset();
    pcheck("p2.mem_live", agentc_mem_live() == base);
    return p2_fails;
}

/* ================================================== prompts live mode
 *
 * tests/mcp.sh writes one config per capability variant and runs
 * `mcp_test --live-prompt` against the stdio mock:
 *   MCP_MOCK_CAPS=all      both lists, prompt expands, trigger re-sync
 *   MCP_MOCK_CAPS=prompts  prompts only (no tools/list)
 *   MCP_MOCK_CAPS=tools    a tools-only server that emits a prompts
 *                          list_changed; prompts must never be fetched
 */
static int lp_fails;

static void lpcheck(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) lp_fails = 1;
}

static bool lp_exposed(const char *prompt, char *out, size_t cap) {
    return agentc_mcp_prompt_exposed("mock", prompt, out, cap);
}

static int live_prompt_main(void) {
    (void)agentc_json_parse("{\"warm\":[1]}", 12);
    agentc_json_reset();
    agentc_mcp_start(NULL, false);
    i64 cap = os_now_ns(OS_CLOCK_MONOTONIC) + 15000000000LL;
    for (int i = 0; i < 20000 && agentc_mcp_pending(); i++) {
        agentc_mcp_pump();
        os_poll(NULL, 0, 1);
        if (os_now_ns(OS_CLOCK_MONOTONIC) > cap) break;
    }
    lpcheck("prompt.settled", !agentc_mcp_pending());
    agentc_outf("prompt.tools=%llu\n", (unsigned long long)agentc_mcp_tools(NULL, 0));
    agentc_outf("prompt.count=%llu\n", (unsigned long long)agentc_mcp_prompt_count());

    char greet[80] = { 0 }, summary[80] = { 0 }, image[80] = { 0 }, fail[80] = { 0 };
    char big[80] = { 0 }, trigger[80] = { 0 }, fresh[80] = { 0 };
    char coll1[80] = { 0 }, coll2[80] = { 0 };
    bool have_greet = lp_exposed("greet", greet, sizeof greet);
    lpcheck("prompt.greet", have_greet);
    lpcheck("prompt.dedup",
            lp_exposed("collide a", coll1, sizeof coll1) &&
                lp_exposed("collide_a", coll2, sizeof coll2) && !agentc_streq(coll1, coll2));
    lp_exposed("summary", summary, sizeof summary);
    lp_exposed("image", image, sizeof image);
    lp_exposed("fail", fail, sizeof fail);
    lp_exposed("big", big, sizeof big);
    bool have_trigger = lp_exposed("trigger", trigger, sizeof trigger);

    if (have_greet) {
        char *text = agentc_prompts_expand(greet, "name=World tone=warm");
        lpcheck("prompt.expand_greet", text && agentc_str_str(text, "Hello World") &&
                                           agentc_str_str(text, "tone=warm"));
        agentc_free(text);
    } else {
        lpcheck("prompt.expand_greet", false);
    }
    if (summary[0]) {
        char *text = agentc_prompts_expand(summary, "3");
        lpcheck("prompt.expand_positional", text && agentc_streq(text, "summary 3"));
        agentc_free(text);
    } else {
        lpcheck("prompt.expand_positional", false);
    }
    if (image[0]) {
        char *text = agentc_prompts_expand(image, "");
        lpcheck("prompt.expand_image", text && agentc_streq(text, "before\n\nafter"));
        agentc_free(text);
    } else {
        lpcheck("prompt.expand_image", false);
    }
    lpcheck("prompt.expand_fail", fail[0] && agentc_prompts_expand(fail, "") == NULL);
    lpcheck("prompt.expand_big", big[0] && agentc_prompts_expand(big, "") == NULL);
    lpcheck("prompt.expand_missing", have_greet && agentc_prompts_expand(greet, "") == NULL);

    if (have_trigger) {
        char *text = agentc_prompts_expand(trigger, "");
        bool ok = text && agentc_streq(text, "mutated");
        agentc_free(text);
        lpcheck("prompt.trigger", ok);
        bool resynced = false;
        for (int i = 0; i < 20000 && !resynced; i++) {
            resynced = lp_exposed("fresh", fresh, sizeof fresh) &&
                       lp_exposed("greet", greet, sizeof greet) &&
                       agentc_streq(prompt_desc_of(greet), "greet v2") &&
                       !lp_exposed("summary", summary, sizeof summary);
            if (!resynced) {
                agentc_mcp_pump();
                os_poll(NULL, 0, 1);
            }
        }
        lpcheck("prompt.resync", resynced);
        lpcheck("prompt.greet_v2", agentc_streq(prompt_desc_of(greet), "greet v2"));
        lpcheck("prompt.summary_removed", !lp_exposed("summary", summary, sizeof summary));
    } else {
        lpcheck("prompt.trigger", false);
        lpcheck("prompt.resync", false);
        lpcheck("prompt.greet_v2", false);
        lpcheck("prompt.summary_removed", false);
    }
    agentc_mcp_shutdown();
    lpcheck("prompt.shutdown", agentc_mcp_prompt_count() == 0);
    return lp_fails;
}

/* ================================================ resources live mode
 *
 * tests/mcp.sh runs `mcp_test --live-resource` against the stdio mock for
 * three capability variants (MCP_EXPECT_RESOURCES selects the assertions):
 *   resources  resources-only server: full list/templates/read/list_changed
 *   all-res    tools+prompts+resources: same, plus the sync-order log check
 *   tools      tools-only server: the resource tools must stay unpublished and
 *              no resources request may be sent
 * The registry path (register_defaults + load_all + ext_pump) is exercised so
 * lazy publication and the three generic tools run end to end.
 */
static int lr_fails;

static void lrcheck(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) lr_fails = 1;
}

static int live_resource_main(void) {
    (void)agentc_json_parse("{\"warm\":[1]}", 12);
    agentc_json_reset();
    size_t base = agentc_mem_live();
    bool expect = true;
    const char *ev = os_getenv("MCP_EXPECT_RESOURCES");
    if (ev && agentc_streq(ev, "0")) expect = false;
    /* The core prompt registry never frees its records by design, so a
     * variant that also syncs prompts cannot return to the process baseline;
     * MCP_SKIP_MEM=1 (the all-res variant) suppresses only that check. */
    const char *sm = os_getenv("MCP_SKIP_MEM");
    bool check_mem = !(sm && agentc_streq(sm, "1"));

    agentc_ext_register_defaults(false);
    agentc_ext_load_all();
    lrcheck("res.no_tools_before_pump", rtool_count("mcp_list_resources") == 0);
    /* The pump's first call creates the server records, so pending() is false
     * before that: require at least one pump and a settled server. */
    bool settled = false;
    for (int i = 0; i < 20000 && !settled; i++) {
        agentc_ext_pump();
        os_poll(NULL, 0, 1);
        settled = agentc_mcp_server_count() > 0 && !agentc_mcp_pending();
    }
    lrcheck("res.settled", settled);

    if (!expect) {
        lrcheck("res.tools_absent",
                rtool_count("mcp_list_resources") == 0 &&
                    rtool_count("mcp_list_resource_templates") == 0 &&
                    rtool_count("mcp_read_resource") == 0);
        lrcheck("res.tables_empty", agentc_mcp_resource_count(false) == 0 &&
                                        agentc_mcp_resource_count(true) == 0);
        agentc_ext_shutdown();
        agentc_json_reset();
        if (check_mem) lrcheck("res.mem_live", agentc_mem_live() == base);
        return lr_fails;
    }

    lrcheck("res.tools_published",
            rtool_count("mcp_list_resources") == 1 &&
                rtool_count("mcp_list_resource_templates") == 1 &&
                rtool_count("mcp_read_resource") == 1);
    {
        size_t total = agentc_ext_tools(NULL, 0);
        AgcTool *arr = agentc_alloc((total ? total : 1) * sizeof *arr);
        size_t n = agentc_ext_tools(arr, total);
        bool ro = true;
        for (size_t i = 0; i < n; i++)
            if (arr[i].name && agentc_str_str(arr[i].name, "mcp_") == arr[i].name &&
                (arr[i].flags & AGENTC_TOOL_READONLY) == 0)
                ro = false;
        lrcheck("res.tools_readonly", ro);
        agentc_free(arr);
    }
    lrcheck("res.resources", agentc_mcp_resource_count(false) == 3 &&
                                  agentc_mcp_resource_count(true) == 2);

    char *out = NULL;
    bool err = true;
    lrcheck("res.list_all",
            rtool_run("mcp_list_resources", "{}", &out, &err) && !err && out &&
                agentc_str_str(out, "\"server\":null") != NULL &&
                agentc_str_str(out, "file:///a.txt") != NULL &&
                agentc_str_str(out, "file:///blob.bin") != NULL);
    agentc_free(out);
    lrcheck("res.list_one",
            rtool_run("mcp_list_resources", "{\"server\":\"mock\"}", &out, &err) &&
                !err && out && agentc_str_str(out, "\"server\":\"mock\"") != NULL);
    agentc_free(out);
    lrcheck("res.list_unknown",
            rtool_run("mcp_list_resources", "{\"server\":\"nope\"}", &out, &err) &&
                err && out &&
                agentc_str_str(out, "not connected with the resource capability") != NULL);
    agentc_free(out);
    lrcheck("res.templates",
            rtool_run("mcp_list_resource_templates", "{}", &out, &err) && !err && out &&
                agentc_str_str(out, "file:///{path}") != NULL &&
                agentc_str_str(out, "db://{table}/{id}") != NULL);
    agentc_free(out);
    lrcheck("res.read_text",
            rtool_run("mcp_read_resource",
                      "{\"server\":\"mock\",\"uri\":\"file:///a.txt\"}", &out, &err) &&
                !err && out && agentc_streq(out, "alpha\n\nbeta"));
    agentc_free(out);
    lrcheck("res.read_blob",
            rtool_run("mcp_read_resource",
                      "{\"server\":\"mock\",\"uri\":\"file:///blob.bin\"}", &out, &err) &&
                err && out && agentc_str_str(out, "binary resource not supported") != NULL);
    agentc_free(out);
    lrcheck("res.read_big",
            rtool_run("mcp_read_resource",
                      "{\"server\":\"mock\",\"uri\":\"file:///big.txt\"}", &out, &err) &&
                err && out && agentc_str_str(out, "256") != NULL);
    agentc_free(out);
    lrcheck("res.read_fail",
            rtool_run("mcp_read_resource",
                      "{\"server\":\"mock\",\"uri\":\"file:///fail\"}", &out, &err) &&
                err && out != NULL);
    agentc_free(out);
    lrcheck("res.read_unknown",
            rtool_run("mcp_read_resource", "{\"server\":\"nope\",\"uri\":\"u\"}",
                      &out, &err) &&
                err && out &&
                agentc_str_str(out, "not connected with the resource capability") != NULL);
    agentc_free(out);

    lrcheck("res.trigger",
            rtool_run("mcp_read_resource",
                      "{\"server\":\"mock\",\"uri\":\"file:///trigger\"}", &out, &err) &&
                !err && out && agentc_streq(out, "mutated"));
    agentc_free(out);
    bool changed = false;
    for (int i = 0; i < 20000 && !changed; i++) {
        char *j = NULL;
        if (agentc_mcp_resource_list_json(false, NULL, NULL, &j) == 0)
            changed = j && agentc_str_str(j, "first v2") != NULL;
        agentc_free(j);
        if (!changed) {
            agentc_ext_pump();
            os_poll(NULL, 0, 1);
        }
    }
    lrcheck("res.list_changed", changed);

    agentc_ext_shutdown();
    agentc_mcp_shutdown();
    lrcheck("res.shutdown", agentc_mcp_resource_count(false) == 0 &&
                                agentc_mcp_resource_count(true) == 0 &&
                                rtool_count("mcp_list_resources") == 0);
    agentc_json_reset();
    if (check_mem) lrcheck("res.mem_live", agentc_mem_live() == base);
    return lr_fails;
}

/* ================================================== idle drain live mode
 *
 * tests/mcp.sh runs `mcp_test --live-idle` against the stdio mock with
 * MCP_MOCK_IDLE_NOTIFY_MS set: after the initial sync the idle server pushes
 * notifications/tools/list_changed on its own. The driver only pumps (never
 * calls a tool) and asserts the mutated list is fetched, proving the READY
 * drain honors an idle notification without an exchange.
 */

static int idle_fails;

static void icheck(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) idle_fails = 1;
}

static int live_idle_main(void) {
    (void)agentc_json_parse("{\"warm\":[1]}", 12);
    agentc_json_reset();
    size_t base = agentc_mem_live();

    agentc_mcp_start(NULL, false);
    bool ready = false;
    for (int i = 0; i < 20000 && !ready; i++) {
        AgcTool tl[8];
        size_t n = agentc_mcp_tools(tl, 8);
        ready = n == 5 && find_tool(tl, n, "mcp__mock__fail") != NULL &&
                find_tool(tl, n, "mcp__mock__fresh") == NULL;
        if (!ready) {
            agentc_mcp_pump();
            os_poll(NULL, 0, 1);
        }
    }
    icheck("idle.initial_tools", ready);

    /* Idle pumps only: the server's own idle timer pushes the notification and
     * the next pump's READY drain must pick it up and fetch the new list. */
    bool mutated = false;
    for (int i = 0; i < 20000 && !mutated; i++) {
        AgcTool tl[8];
        size_t n = agentc_mcp_tools(tl, 8);
        mutated = find_tool(tl, n, "mcp__mock__fresh") != NULL &&
                  find_tool(tl, n, "mcp__mock__fail") == NULL;
        if (!mutated) {
            agentc_mcp_pump();
            os_poll(NULL, 0, 1);
        }
    }
    icheck("idle.list_changed_drained", mutated);
    icheck("idle.tools_mutated", agentc_mcp_tools(NULL, 0) == 4);

    const char *log = os_getenv("MCP_MOCK_LOG");
    if (log && log[0]) {
        size_t ln = 0;
        char *text = agentc_read_file_owned(log, &ln);
        icheck("idle.log_read", text != NULL);
        if (text) {
            icheck("idle.no_client_exchange",
                   agentc_str_str(text, "tools/call") == NULL);
            icheck("idle.resynced", agentc_str_str(text, "tools/list") != NULL);
            agentc_free(text);
        }
    } else {
        icheck("idle.log_read", false);
        icheck("idle.no_client_exchange", false);
        icheck("idle.resynced", false);
    }

    agentc_mcp_shutdown();
    agentc_json_reset();
    icheck("idle.mem_live", agentc_mem_live() == base);
    return idle_fails;
}

int agentc_main(int argc, char **argv) {
    if (argc > 1 && agentc_streq(argv[1], "--live")) return live_main();
    if (argc > 1 && agentc_streq(argv[1], "--live-p2")) return live_p2_main();
    if (argc > 1 && agentc_streq(argv[1], "--live-prompt")) return live_prompt_main();
    if (argc > 1 && agentc_streq(argv[1], "--live-resource")) return live_resource_main();
    if (argc > 1 && agentc_streq(argv[1], "--live-idle")) return live_idle_main();

    agentc_test_setenv("HOME", TEST_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TEST_ROOT "/config");
    agentc_rm_rf(TEST_ROOT);

    test_expand();
    test_names();
    test_handed_out();
    test_config();
    test_load_files();
    test_rpc();
    test_write_bounded();
    test_line_cap();
    test_parse_tools();
    test_parse_call();
    test_capabilities();
    test_parse_prompts();
    test_parse_prompt_get();
    test_parse_resources();
    test_parse_resource_templates();
    test_parse_resource_read();
    test_arena_isolation();
    test_http_mock();
    test_grace_reclaim();
    test_retry_delays();
    test_retry_reconnect();
    test_init_error();
    test_caps();
    test_long_names();
    test_tool_limits();
    test_churn_reclaim();
    test_frozen_cap_server();
    test_prompt_registry();
    test_prompt_mock();
    test_resource_registry();
    test_resource_mock();
    test_resource_gating();
    test_mem_cycles();
    test_trusted_project_cwd();
    test_prompt_cap_reuse();
    test_shutdown_frozen();
    test_session_id_injection();

    agentc_test_clearenv();
    agentc_rm_rf(TEST_ROOT);
    return fails;
}
