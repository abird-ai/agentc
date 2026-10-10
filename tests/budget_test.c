/* budget_test.c — the agent token budget (P6-SURFACE §18 #23).
 *
 * A cumulative soft cap on an agent's model tokens: reaching it between two
 * model steps emits `actor_notify` and settles the turn without another
 * provider request. Deterministic golden suite: an injected replay transport,
 * no processes. It prints one `label=1|0` line per assertion and compares
 * byte-for-byte (tests/data/budget_test.expected).
 */
#include "agent.h"
#include "config.h"
#include "core/agent_int.h"
#include "core/events.h"
#include "hook.h"
#include "plat.h"
#include "session.h"

static int fails;

/* internal test hook from agent.c */
void agentc_agent_test_no_backoff(AgcAgent *a);

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

/* Step 1: text "answer" + one `budgettool` call; usage input=4 output=6. */
static const char reply_tool_step[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"b1\",\"usage\":{"
    "\"input_tokens\":4,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"answer\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"t1\",\"name\":\"budgettool\",\"input\":{}}}\n\n"
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

/* Step 2 (only reached when there is no cap): text "second"; input=2 output=1. */
static const char reply_final[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"b2\",\"usage\":{"
    "\"input_tokens\":2,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"second\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":1}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static int budget_tool(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error) {
    (void)self;
    (void)call;
    if (is_error)
        *is_error = false;
    agentc_buf_cstr(out, "tool-ok");
    return 0;
}

static const AgcTool budget_tool_def = {
    .name = "budgettool",
    .label = "Budget",
    .desc = "d",
    .params_json = "{\"type\":\"object\"}",
    .flags = AGENTC_TOOL_CORE,
    .run = budget_tool,
};

static AgcAgent *make_agent(Replay *r, i64 budget) {
    AgcAgentSpec s;
    agentc_memset(&s, 0, sizeof s);
    s.provider_handle = agentc_prov_anthropic();
    s.provider = "anthropic";
    s.model = "claude-sonnet-4-5";
    s.thinking = -1;
    s.label = "root";
    s.flags = AGENTC_AGENT_F_WAIT;
    s.token_budget = budget;
    AgcTransport t = {replay_request, r, NULL};
    s.transport = t;
    s.transport_set = true;
    AgcAgent *a = agentc_agent_create(&s);
    if (a) {
        agentc_agent_set_tools(a, &budget_tool_def, 1);
        agentc_agent_test_no_backoff(a);
    }
    return a;
}

/* ------------------------------------------------------------- notify count */
typedef struct {
    int notify;
    char text[64];
    char json[128];
} BudgetEvents;

static BudgetEvents g_be;

/* The event payload is borrowed for the callback only, so copy what we assert. */
static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = src ? agentc_strlen(src) : 0;
    if (n >= cap)
        n = cap - 1;
    if (n)
        agentc_memcpy(dst, src, n);
    dst[n] = 0;
}

static void budget_event(void *ud, int ev, const void *data) {
    (void)ud;
    if (ev == AGENTC_EV_ACTOR_NOTIFY) {
        const AgcActorEvent *e = data;
        g_be.notify++;
        if (e) {
            copy_str(g_be.text, sizeof g_be.text, e->text);
            copy_str(g_be.json, sizeof g_be.json, e->json);
        }
    }
}

/* --------------------------------------------------------------- test cases */
static void test_budget_stops(void) {
    agentc_actor_init();
    agentc_events_reset();
    agentc_memset(&g_be, 0, sizeof g_be);
    (void)agentc_events_subscribe(budget_event, NULL);

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_tool_step; /* step 2 deliberately absent */
    AgcAgent *a = make_agent(&r, 10);
    check("budget.create", a != NULL && agentc_agent_actor(a) != 0);
    check("budget.preset", agentc_agent_token_budget(a) == 10);

    int rc = agentc_agent_submit(a, "go");
    check("budget.rc", rc == 0);
    check("budget.one_request", r.i == 1); /* step 2 was not issued */
    check("budget.spent", agentc_agent_tokens_spent(a) == 10);
    check("budget.notify", g_be.notify == 1);
    check("budget.notify_reason",
          agentc_str_str(g_be.json, "\"reason\":\"token_budget\"") != NULL &&
              agentc_str_str(g_be.json, "\"spent\":10") != NULL);

    AgcAgentInfo ai;
    check("budget.info", agentc_agent_info(agentc_agent_actor(a), &ai) == 0 && ai.token_budget == 10 &&
                             ai.tokens_spent == 10);
    const AgcTranscript *tr = agentc_agent_transcript(a);
    /* user + assistant(text+tool_call) + tool result; the assistant is the
     * turn's final output and its call carries the stop reason. */
    check("budget.transcript",
          tr != NULL && tr->n == 3 && tr->msgs[1].role == AGENTC_ROLE_ASSISTANT &&
              tr->msgs[1].stop_reason == AGENTC_STOP_TOOLUSE && tr->msgs[2].role == AGENTC_ROLE_TOOL);

    agentc_agent_free(a);
    agentc_actor_release_all();
}

/* The cap is per submitted turn: a later submit gets a fresh allowance and runs
 * again (the old cumulative behavior bricked the agent), and the notify fires
 * once per turn. */
static void test_budget_per_turn(void) {
    agentc_actor_init();
    agentc_events_reset();
    agentc_memset(&g_be, 0, sizeof g_be);
    (void)agentc_events_subscribe(budget_event, NULL);

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 3;
    r.codes[0] = r.codes[1] = r.codes[2] = 0;
    r.bodies[0] = reply_tool_step; /* turn 1: spends 10, hits the cap */
    r.bodies[1] = reply_tool_step; /* turn 2: spends 10, hits the cap */
    r.bodies[2] = reply_final;     /* turn 3: spends 3, completes */
    AgcAgent *a = make_agent(&r, 10);
    check("budget2.create", a != NULL);
    check("budget2.t1", agentc_agent_submit(a, "go") == 0 && r.i == 1 &&
                            agentc_agent_tokens_spent(a) == 10 && g_be.notify == 1);
    check("budget2.t2", agentc_agent_submit(a, "again") == 0 && r.i == 2 && g_be.notify == 2);
    check("budget2.reset", agentc_agent_tokens_spent(a) == 10); /* turn 2's fresh spend */
    check("budget2.t3", agentc_agent_submit(a, "third") == 0 && r.i == 3 &&
                            agentc_agent_tokens_spent(a) == 3 && g_be.notify == 2);

    agentc_agent_free(a);
    agentc_actor_release_all();
}

static void test_no_budget_continues(void) {
    agentc_actor_init();
    agentc_events_reset();
    agentc_memset(&g_be, 0, sizeof g_be);
    (void)agentc_events_subscribe(budget_event, NULL);

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = 0;
    r.codes[1] = 0;
    r.bodies[0] = reply_tool_step;
    r.bodies[1] = reply_final;
    AgcAgent *a = make_agent(&r, 0); /* 0 = no cap */
    check("nobudget.create", a != NULL);
    int rc = agentc_agent_submit(a, "go");
    check("nobudget.rc", rc == 0);
    check("nobudget.two_requests", r.i == 2);
    check("nobudget.spent", agentc_agent_tokens_spent(a) == 13);
    check("nobudget.no_notify", g_be.notify == 0);

    agentc_agent_free(a);
    agentc_actor_release_all();
}

static void test_budget_inherit(void) {
    agentc_actor_init();
    agentc_events_reset();
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    AgcAgent *root = make_agent(&r, 50);
    check("inherit.root", root != NULL && agentc_agent_token_budget(root) == 50);

    AgcAgentSpec cs;
    agentc_memset(&cs, 0, sizeof cs);
    cs.parent = root;
    cs.thinking = -1;
    AgcAgent *child = agentc_agent_create(&cs);
    check("inherit.child", child != NULL && agentc_agent_token_budget(child) == 50);

    cs.token_budget = -1; /* explicit override: uncapped */
    AgcAgent *free_child = agentc_agent_create(&cs);
    check("inherit.uncapped", free_child != NULL && agentc_agent_token_budget(free_child) == -1);

    agentc_actor_release(agentc_agent_actor(child));
    agentc_actor_release(agentc_agent_actor(free_child));
    agentc_actor_release(agentc_agent_actor(root));
    agentc_actor_release_all();
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    agentc_log_set_level(4);
    test_budget_stops();
    test_budget_per_turn();
    test_no_budget_continues();
    test_budget_inherit();
    return fails;
}
