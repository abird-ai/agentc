/* hooks_test.c — P6-B universal hook pipeline golden (P6-HOOKS §12).
 *
 * Part A exercises the core registry/chain: order, edit/replace/consume,
 * defer+resume, the hot-path guard, reentry and per-stage on_error. Part B
 * (added with the stage wiring) drives every stage/kind once. Deterministic.
 */
#include "agent.h"
#include "plat.h"
#include "base/limits.h"
#include "core/events.h"
#include "ext.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

static char g_log[512];
static size_t g_log_n;

static void note(const char *s) {
    size_t n = agentc_strlen(s);
    if (g_log_n != 0 && g_log_n + 1 < sizeof g_log)
        g_log[g_log_n++] = ' ';
    for (size_t i = 0; i < n && g_log_n + 1 < sizeof g_log; i++)
        g_log[g_log_n++] = s[i];
    g_log[g_log_n] = 0;
}
static void log_reset(void) {
    g_log_n = 0;
    g_log[0] = 0;
}
static bool log_is(const char *s) {
    return agentc_streq(g_log, s);
}

static int h_named(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)m;
    (void)next;
    (void)defer;
    note((const char *)ud);
    return 0;
}
static int h_consume(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)m;
    (void)defer;
    note("consume");
    *next = NULL;
    return 0;
}
static int h_edit(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)next;
    (void)defer;
    note("edit");
    if (m)
        m->priority = 7;
    return 0;
}
static int h_body(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)next;
    (void)defer;
    note(m && m->body ? m->body : "null");
    return 0;
}
static AgcMessage g_repl;

static int h_replace(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)m;
    (void)defer;
    note("replace");
    *next = &g_repl;
    return 0;
}
static int h_defer(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)m;
    (void)next;
    note("defer");
    *defer = 0xabc;
    return 0;
}
/* A hand-parked handler that re-enters itself on resume: it marks its own token
 * and distinguishes the two invocations via agentc_hook_in_reentry(). */
static int h_self(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)m;
    (void)next;
    if (agentc_hook_in_reentry()) {
        note("self-resumed");
        return 0;
    }
    note("self-parked");
    *defer = 0x5e1f;
    agentc_hook_mark_self(*defer);
    return 0;
}
static int h_fail(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)m;
    (void)next;
    (void)defer;
    note("fail");
    return -1;
}
static int h_reenter(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)next;
    (void)defer;
    note("reenter");
    AgcMessage *o = NULL;
    int rc = agentc_hook_run("input", m, &o);
    note(rc == -16 ? "busy" : "open");
    return 0;
}

static void msg_init(AgcMessage *m, const char *body) {
    agentc_memset(m, 0, sizeof *m);
    m->class = AGENTC_MSG_CLASS_INPUT;
    m->kind = AGENTC_MSG_INPUT;
    m->body = body;
    m->body_len = body ? agentc_strlen(body) : 0;
    m->id = 1;
}

/* ---- A: registry/chain ---- */

static void test_order(void) {
    log_reset();
    AgcMessage m;
    msg_init(&m, "x");
    AgcMessage *out = NULL;
    uint64_t a = agentc_hook("input", 5, h_named, "a");
    uint64_t b = agentc_hook("input", 1, h_named, "b");
    uint64_t c = agentc_hook("input", 1, h_named, "c");
    check("H.order_registered", a != 0 && b != 0 && c != 0);
    /* re-registered with lower order wins ties by seq (b before c) */
    check("H.order_run", agentc_hook_run("input", &m, &out) == 0 && log_is("b c a"));
    agentc_hook_off(a);
    agentc_hook_off(b);
    agentc_hook_off(c);
}

static void test_wants(void) {
    check("H.wants_empty", !agentc_hook_wants("input"));
    uint64_t h = agentc_hook("input", 0, h_named, "a");
    check("H.wants_set", agentc_hook_wants("input"));
    agentc_hook_off(h);
    check("H.wants_clear", !agentc_hook_wants("input"));
}

static void test_consume(void) {
    log_reset();
    AgcMessage m;
    msg_init(&m, "x");
    AgcMessage *out = (AgcMessage *)&m;
    uint64_t a = agentc_hook("input", 0, h_named, "a");
    uint64_t b = agentc_hook("input", 1, h_consume, NULL);
    uint64_t c = agentc_hook("input", 2, h_named, "c");
    check("H.consume",
          agentc_hook_run("input", &m, &out) == 0 && log_is("a consume") && out == NULL);
    agentc_hook_off(a);
    agentc_hook_off(b);
    agentc_hook_off(c);
}

static void test_replace_edit(void) {
    log_reset();
    AgcMessage m;
    msg_init(&m, "orig");
    g_repl = m;
    g_repl.body = "replaced";
    AgcMessage *out = NULL;
    uint64_t a = agentc_hook("input", 0, h_replace, NULL);
    uint64_t b = agentc_hook("input", 1, h_body, NULL);
    check("H.replace",
          agentc_hook_run("input", &m, &out) == 0 && log_is("replace replaced") && out == &g_repl);
    agentc_hook_off(a);
    agentc_hook_off(b);

    log_reset();
    AgcMessage m2;
    msg_init(&m2, "x");
    uint64_t e = agentc_hook("input", 0, h_edit, NULL);
    check("H.edit", agentc_hook_run("input", &m2, &out) == 0 && m2.priority == 7);
    agentc_hook_off(e);
}

static void test_defer(void) {
    log_reset();
    AgcMessage m;
    msg_init(&m, "x");
    m.id = 42;
    AgcMessage *out = NULL;
    uint64_t a = agentc_hook("input", 0, h_named, "a");
    uint64_t d = agentc_hook("input", 1, h_defer, NULL);
    uint64_t b = agentc_hook("input", 2, h_named, "b");
    check("H.defer_park", agentc_hook_run("input", &m, &out) == -115 && log_is("a defer"));
    check("H.defer_resume", agentc_hook_resume(0xabc, &m) == 0 && log_is("a defer b"));
    check("H.defer_unknown", agentc_hook_resume(0xdef, &m) == -22);
    agentc_hook_off(a);
    agentc_hook_off(d);
    agentc_hook_off(b);
}

static void test_self_resume(void) {
    log_reset();
    AgcMessage m;
    msg_init(&m, "x");
    m.id = 43;
    AgcMessage *out = NULL;
    uint64_t h = agentc_hook("input", 1, h_self, NULL);
    check("H.self_park", agentc_hook_run("input", &m, &out) == -115 && log_is("self-parked"));
    check("H.self_resume",
          agentc_hook_resume(0x5e1f, &m) == 0 && log_is("self-parked self-resumed"));
    agentc_hook_off(h);
}

/* ---- B8 fold adapter: core stage + mapped legacy point, exactly once ---- */
static int g_legacy_fired;
static int legacy_fold_fn(void *ud, const char *point, const char *payload_json, char **res) {
    (void)ud;
    (void)payload_json;
    g_legacy_fired++;
    *res = agentc_strdup("{\"legacy\":true}");
    return 0;
}

static void test_fold_adapter(void) {
    const AgcExtHost *host = agentc_ext_host();
    uint64_t lh = host->on("input", AGENTC_HOOK_OVERRIDE, 0, legacy_fold_fn, NULL);
    check("H.fold_reg", lh != 0);

    g_legacy_fired = 0;
    AgcMessage m;
    msg_init(&m, "x");
    m.id = 777;
    AgcMessage *out = NULL;
    check("H.fold_run", agentc_hook_run_legacy("input", "input", &m, NULL, &out) == 0);
    check("H.fold_once", g_legacy_fired == 1);
    check("H.fold_result",
          m.result_json != NULL && agentc_str_str(m.result_json, "legacy") != NULL);
    agentc_hook_result_free(&m);
    check("H.fold_freed", m.result_json == NULL);

    /* a core handler that consumes the message suppresses the legacy dispatch */
    g_legacy_fired = 0;
    uint64_t ch = agentc_hook("input", 0, h_consume, NULL);
    msg_init(&m, "y");
    m.id = 778;
    check("H.fold_consume",
          agentc_hook_run_legacy("input", "input", &m, NULL, &out) == 0 && g_legacy_fired == 0);
    agentc_hook_off(ch);
    agentc_hook_result_free(&m);

    host->off(lh);
}

/* B8 production sites: the migrated overrides (`input`, `message_end`,
 * `turn_end`, `tool_result`) dispatch the core stage and the legacy point
 * exactly once. A legacy `input` handler that returns `handled` suppresses the
 * run; a core handler that consumes suppresses the legacy dispatch. */
void agentc_agent_test_no_backoff(AgcAgent *a);
static int g_fs_input_fired;
static int fs_input_legacy(void *ud, const char *point, const char *payload_json, char **res) {
    (void)ud;
    (void)point;
    (void)payload_json;
    g_fs_input_fired++;
    *res = agentc_strdup("{\"action\":\"handled\"}");
    return 0;
}

static void test_fold_sites(void) {
    const AgcExtHost *host = agentc_ext_host();
    uint64_t lh = host->on("input", AGENTC_HOOK_OVERRIDE, 0, fs_input_legacy, NULL);
    check("FS.legacy_reg", lh != 0);

    g_fs_input_fired = 0;
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_test_no_backoff(a);
    int rc = agentc_agent_submit(a, "go");
    check("FS.input_handled", rc == 0 && g_fs_input_fired == 1);
    check("FS.input_no_run", agentc_agent_transcript(a)->n == 0);
    agentc_agent_free(a);
    agentc_actor_release_all();

    /* A core `input` handler that consumes suppresses the legacy dispatch
     * (exactly-once). */
    uint64_t ch = agentc_hook("input", 0, h_consume, NULL);
    g_fs_input_fired = 0;
    a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_test_no_backoff(a);
    rc = agentc_agent_submit(a, "go");
    check("FS.input_consume", rc == 0 && g_fs_input_fired == 0);
    check("FS.input_consume_norun", agentc_agent_transcript(a)->n == 0);
    agentc_agent_free(a);
    agentc_actor_release_all();
    agentc_hook_off(ch);
    host->off(lh);
}

static void test_on_error(void) {
    log_reset();
    AgcMessage m;
    msg_init(&m, "x");
    m.id = 7;
    AgcMessage *out = NULL;
    uint64_t f = agentc_hook("input", 0, h_fail, NULL);
    uint64_t a = agentc_hook("input", 1, h_named, "a");
    check("H.error_pass", agentc_hook_run("input", &m, &out) == 0 && log_is("fail a"));
    agentc_hook_off(f);
    agentc_hook_off(a);

    log_reset();
    m.id = 8;
    f = agentc_hook("tool", 0, h_fail, NULL);
    a = agentc_hook("tool", 1, h_named, "a");
    check("H.error_deny", agentc_hook_run("tool", &m, &out) == -1 && log_is("fail"));
    agentc_hook_off(f);
    agentc_hook_off(a);
}

static void test_reentry(void) {
    log_reset();
    AgcMessage m;
    msg_init(&m, "x");
    m.id = 99;
    AgcMessage *out = NULL;
    uint64_t r = agentc_hook("input", 0, h_reenter, NULL);
    check("H.reentry", agentc_hook_run("input", &m, &out) == 0 && log_is("reenter busy"));
    agentc_hook_off(r);
}

static void test_unknown_stage(void) {
    check("H.unknown_stage", agentc_hook("nope", 0, h_named, "a") == 0);
    check("H.branch_enotsup", agentc_hook_branch(NULL, NULL, NULL) == -95);
}

/* ---- B: every stage fires once per boundary ---- */

void agentc_agent_test_no_backoff(AgcAgent *a);

typedef struct {
    int codes[8];
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
    int code = r->codes[r->i];
    const char *resp = r->bodies[r->i++];
    if (code != 0)
        return code;
    if (resp) {
        size_t len = agentc_strlen(resp);
        for (size_t off = 0; off < len; off += 7) {
            size_t k = len - off;
            if (k > 7)
                k = 7;
            if (on_chunk(u, resp + off, k))
                return -125;
        }
    }
    return 0;
}

static const char reply_tools[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"t1\",\"name\":\"read\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"f\\\"}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
    "\"usage\":{\"output_tokens\":3}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static const char reply_final[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m2\",\"usage\":{"
    "\"input_tokens\":8,\"output_tokens\":0}}}\n\n"
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

enum {
    ST_TURN,
    ST_STEP,
    ST_INPUT,
    ST_PROMPT,
    ST_MESSAGE,
    ST_OUTPUT,
    ST_TOOL,
    ST_SIGNAL,
    ST_EVENT,
    ST_LIFECYCLE,
    ST_COUNT
};

static const char *const g_stage_names[ST_COUNT] = {
    "turn", "step", "input", "prompt", "message", "output", "tool", "signal", "event", "lifecycle",
};
static int g_stage_count[ST_COUNT];

static int h_count(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)m;
    (void)next;
    (void)defer;
    (*(int *)ud)++;
    return 0;
}

static void test_all_stages(void) {
    static int idx[ST_COUNT];
    uint64_t handles[ST_COUNT];
    bool all_reg = true;
    for (int i = 0; i < ST_COUNT; i++) {
        idx[i] = i;
        g_stage_count[i] = 0;
        handles[i] = agentc_hook(g_stage_names[i], 0, h_count, &g_stage_count[i]);
        if (handles[i] == 0)
            all_reg = false;
    }
    check("S.reg", all_reg);
    (void)idx;

    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    AgcActorId id = agentc_agent_bind_actor(a, 0, "agent");

    AgcMessage msg;
    msg_init(&msg, "notify");
    msg.kind = AGENTC_MSG_NOTIFY;
    msg.to = id;
    msg.from = 0;
    (void)agentc_message_send(&msg);
    (void)agentc_message_drain(id, 0);

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;
    AgcTransport t = {replay_request, &r, NULL};
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    int rc = agentc_agent_submit(a, "go");
    check("S.rc", rc == 0 && r.i == 2);

    check("S.turn", g_stage_count[ST_TURN] >= 1);
    check("S.step", g_stage_count[ST_STEP] >= 1);
    check("S.input", g_stage_count[ST_INPUT] >= 1);
    check("S.prompt", g_stage_count[ST_PROMPT] >= 1);
    check("S.message", g_stage_count[ST_MESSAGE] >= 1);
    check("S.output", g_stage_count[ST_OUTPUT] >= 1);
    check("S.tool", g_stage_count[ST_TOOL] >= 1);
    check("S.signal", g_stage_count[ST_SIGNAL] >= 1);
    check("S.lifecycle", g_stage_count[ST_LIFECYCLE] >= 1);

    AgcMessage em;
    msg_init(&em, "x");
    AgcMessage *out = NULL;
    check("S.event", agentc_hook_run("event", &em, &out) == 0 && g_stage_count[ST_EVENT] >= 1);

    for (int i = 0; i < ST_COUNT; i++)
        agentc_hook_off(handles[i]);
    agentc_agent_free(a);
    agentc_actor_release_all();
}

/* The observe-only `event` stage rides the typed-event hub behind a hot-path
 * guard: an unwatched stage never builds a message, a watched one sees every
 * emitted event. */
static void test_event_stage_hub(void) {
    int n = 0;
    uint64_t h = agentc_hook("event", 0, h_count, &n);
    check("H.event_reg", h != 0);
    agentc_events_emit(NULL, AGENTC_EV_STEP_START, NULL);
    agentc_events_emit(NULL, AGENTC_EV_ERROR, "boom");
    check("H.event_hub", n >= 2);
    agentc_hook_off(h);
}

/* HOK §4: a tool marked AGENTC_TOOL_NO_HOOKS, or a run with tools.hooks off,
 * bypasses the tool call/result stages (the typed events still fire). */
static int g_th_exec_calls;
static char *th_exec(const char *args_json, bool *is_error, const volatile bool *cancel) {
    (void)args_json;
    (void)cancel;
    g_th_exec_calls++;
    if (is_error)
        *is_error = false;
    return agentc_strdup("ok");
}
static const AgcTool th_read = {
    .name = "read",
    .label = "Read",
    .desc = "r",
    .params_json = "{\"type\":\"object\"}",
    .flags = AGENTC_TOOL_READONLY,
    .exec = th_exec,
};
static const AgcTool th_read_nohooks = {
    .name = "read",
    .label = "Read",
    .desc = "r",
    .params_json = "{\"type\":\"object\"}",
    .flags = AGENTC_TOOL_READONLY | AGENTC_TOOL_NO_HOOKS,
    .exec = th_exec,
};

static int run_tool_once(const AgcTool *tool, bool hooks_on) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.bodies[0] = reply_tools;
    r.codes[1] = 0;
    r.bodies[1] = reply_final;
    AgcTransport t = {replay_request, &r, NULL};
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    agentc_agent_set_tools(a, tool, 1);
    agentc_agent_set_tools_hooks(a, hooks_on);
    (void)agentc_agent_submit(a, "go");
    agentc_agent_free(a);
    agentc_actor_release_all();
    return g_th_exec_calls;
}

static void test_tool_no_hooks(void) {
    int n = 0;
    uint64_t h = agentc_hook("tool", 0, h_count, &n);
    check("T.reg", h != 0);

    g_th_exec_calls = 0;
    (void)run_tool_once(&th_read_nohooks, true);
    check("T.nohooks_exec", g_th_exec_calls == 1);
    check("T.nohooks_bypass", n == 0);

    n = 0;
    (void)run_tool_once(&th_read, false);
    check("T.config_off_bypass", n == 0);

    n = 0;
    (void)run_tool_once(&th_read, true);
    check("T.hooks_on_fires", n >= 2);

    agentc_hook_off(h);
}

/* ---- C: hook_branch is a true runtime continuation for a background child */
static const char reply_branch[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"b1\",\"usage\":{"
    "\"input_tokens\":3,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"{\\\"verdict\\\":\\\"allow\\\"}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":2}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static AgcAgentSpec g_branch_spec;
static uint64_t g_branch_token;
static bool g_branch_resumed;

static int h_branch_park(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)next;
    /* Only the test's own prompt defers; the branch child re-emits an `input`
     * stage on its own submit, which must pass straight through. */
    if (m == NULL || m->id != 4242)
        return 0;
    note("branch");
    int rc = agentc_hook_branch(&g_branch_spec, m, defer);
    g_branch_token = defer ? *defer : 0;
    note(rc == 0 ? "parked" : "bfail");
    return 0;
}

static int h_branch_after(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)next;
    (void)defer;
    if (m == NULL || m->id != 4242)
        return 0;
    note(m->body && agentc_str_str(m->body, "allow") ? "resumed" : "resumed-bad");
    g_branch_resumed = true;
    return 0;
}

static void test_branch_async(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_branch;
    AgcAgent *root = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    AgcTransport t = {replay_request, &r, NULL};
    agentc_agent_set_transport(root, t);
    agentc_decision_set_parent(root);

    agentc_memset(&g_branch_spec, 0, sizeof g_branch_spec);
    g_branch_spec.parent = root;
    g_branch_spec.provider = "anthropic";
    g_branch_spec.model = "claude-sonnet-4-5";
    g_branch_spec.thinking = -1;
    g_branch_spec.flags = AGENTC_AGENT_F_BACKGROUND | AGENTC_AGENT_F_EPHEMERAL;

    g_branch_token = 0;
    g_branch_resumed = false;
    log_reset();
    AgcMessage m;
    msg_init(&m, "classify");
    m.id = 4242;
    uint64_t a = agentc_hook("input", 0, h_branch_park, NULL);
    uint64_t b = agentc_hook("input", 1, h_branch_after, NULL);
    AgcMessage *out = NULL;
    int rc = agentc_hook_run("input", &m, &out);
    check("B.branch_park", rc == -115 && g_branch_token != 0 && log_is("branch parked"));
    check("B.branch_pending", agentc_runtime_defer_count() >= 1);

    for (int i = 0; i < 64 && !g_branch_resumed; i++)
        agentc_runtime_tick();
    check("B.branch_resume", g_branch_resumed && log_is("branch parked resumed"));
    check("B.branch_released", agentc_runtime_defer_count() == 0);
    agentc_hook_off(a);
    agentc_hook_off(b);
    agentc_decision_set_parent(NULL);
    agentc_agent_free(root);
    agentc_actor_release_all();
}

/* ---- D: provider points fold through one `provider` stage ---- */

/* Adapter: the core `provider` chain and the mapped legacy point run exactly
 * once; a consuming core handler suppresses the legacy dispatch. */
static void test_provider_fold(void) {
    const AgcExtHost *host = agentc_ext_host();
    uint64_t lh =
        host->on("before_provider_headers", AGENTC_HOOK_OVERRIDE, 0, legacy_fold_fn, NULL);
    check("P.legacy_reg", lh != 0);
    int core = 0;
    uint64_t ch = agentc_hook("provider", 0, h_count, &core);
    check("P.stage_reg", ch != 0);

    g_legacy_fired = 0;
    AgcMessage m;
    msg_init(&m, "{\"headers\":{}}");
    m.id = 901;
    m.meta = "headers";
    AgcMessage *out = NULL;
    check("P.fold_run",
          agentc_hook_run_legacy("provider", "before_provider_headers", &m, m.body, &out) == 0);
    check("P.fold_core", core == 1);
    check("P.fold_once", g_legacy_fired == 1);
    check("P.fold_result",
          m.result_json != NULL && agentc_str_str(m.result_json, "legacy") != NULL);
    agentc_hook_result_free(&m);

    agentc_hook_off(ch);
    uint64_t cc = agentc_hook("provider", 0, h_consume, NULL);
    g_legacy_fired = 0;
    msg_init(&m, "{\"headers\":{}}");
    m.id = 902;
    check("P.fold_consume",
          agentc_hook_run_legacy("provider", "before_provider_headers", &m, m.body, &out) == 0 &&
              g_legacy_fired == 0);
    agentc_hook_result_free(&m);
    agentc_hook_off(cc);
    host->off(lh);
}

/* Production fold: all four sub-kinds reach a core `provider` handler in one
 * submit and the legacy header point still fires; under a quiet front end the
 * observe legacy point is silent while the core stage still runs. */
static int g_prov_hdr, g_prov_req, g_prov_resp, g_prov_stream;
static int h_prov(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)next;
    (void)defer;
    if (m && m->meta) {
        if (agentc_streq(m->meta, "headers"))
            g_prov_hdr++;
        else if (agentc_streq(m->meta, "request"))
            g_prov_req++;
        else if (agentc_streq(m->meta, "response"))
            g_prov_resp++;
        else if (agentc_streq(m->meta, "stream_event"))
            g_prov_stream++;
    }
    return 0;
}
static int hdr_legacy_fn(void *ud, const char *point, const char *payload_json, char **res) {
    (void)ud;
    (void)point;
    (void)payload_json;
    g_legacy_fired++;
    *res = agentc_strdup("{\"X-Fold\":\"1\"}");
    return 0;
}
static int g_sse_legacy;
static int sse_legacy_fn(void *ud, const char *point, const char *payload_json, char **res) {
    (void)ud;
    (void)point;
    (void)payload_json;
    (void)res;
    g_sse_legacy++;
    return 0;
}

static void test_provider_loop(void) {
    const AgcExtHost *host = agentc_ext_host();
    g_prov_hdr = g_prov_req = g_prov_resp = g_prov_stream = 0;
    uint64_t ch = agentc_hook("provider", 0, h_prov, NULL);
    uint64_t lh = host->on("before_provider_headers", AGENTC_HOOK_OVERRIDE, 0, hdr_legacy_fn, NULL);

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_final;
    AgcTransport t = {replay_request, &r, NULL};
    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    g_legacy_fired = 0;
    check("P.loop_rc", agentc_agent_submit(a, "go") == 0);
    check("P.loop_headers", g_prov_hdr >= 1);
    check("P.loop_request", g_prov_req >= 1);
    check("P.loop_response", g_prov_resp >= 1);
    check("P.loop_stream", g_prov_stream >= 1);
    check("P.loop_legacy", g_legacy_fired == 1);
    agentc_agent_free(a);
    agentc_actor_release_all();
    host->off(lh);

    /* Quiet suppresses the observe legacy point; the core `provider` stage is
     * a new seam and still runs. */
    agentc_hook_off(ch);
    g_prov_stream = 0;
    uint64_t ch2 = agentc_hook("provider", 0, h_prov, NULL);
    uint64_t sh = host->on("provider_stream_event", AGENTC_HOOK_OBSERVE, 0, sse_legacy_fn, NULL);
    g_sse_legacy = 0;
    agentc_events_set_quiet(true);
    Replay r2;
    agentc_memset(&r2, 0, sizeof r2);
    r2.n = 1;
    r2.codes[0] = 0;
    r2.bodies[0] = reply_final;
    AgcTransport t2 = {replay_request, &r2, NULL};
    a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, t2);
    agentc_agent_set_system(a, "sys");
    agentc_agent_test_no_backoff(a);
    check("P.quiet_rc", agentc_agent_submit(a, "go") == 0);
    check("P.quiet_legacy", g_sse_legacy == 0);
    check("P.quiet_core", g_prov_stream >= 1);
    agentc_events_set_quiet(false);
    agentc_agent_free(a);
    agentc_actor_release_all();
    host->off(sh);
    agentc_hook_off(ch2);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    agentc_log_set_level(4);
    agentc_events_reset();

    test_wants();
    test_order();
    test_consume();
    test_replace_edit();
    test_defer();
    test_self_resume();
    test_fold_adapter();
    test_fold_sites();
    test_on_error();
    test_reentry();
    test_unknown_stage();
    test_all_stages();
    test_event_stage_hub();
    test_tool_no_hooks();
    test_branch_async();
    test_provider_fold();
    test_provider_loop();

    agentc_events_reset();
    agentc_actor_release_all();
    return fails;
}
