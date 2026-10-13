/* subagents_test.c — P6-D unified constructor, agent tree, agent tool.
 *
 * Deterministic golden suite: an injected replay transport, no processes. It
 * prints one `label=1|0` line per assertion and compares byte-for-byte. The
 * Windows persistence checks print `skip` (tests/data/subagents_test.
 * windows.expected). */
#include "agent.h"
#include "core/agent_int.h"
#include "config.h"
#include "core/events.h"
#include "plat.h"
#include "session.h"
#include "hook.h"

static int fails;

/* internal test hook from agent.c */
void agentc_agent_test_no_backoff(AgcAgent *a);

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

/* Windows persistence is a documented skip (tests/data/subagents_test.
 * windows.expected): print `skip`, never a hard-coded 1. */
static void skip(const char *label) {
    agentc_outf("%s=skip\n", label);
}

static void check_win(const char *label, bool win, bool ok) {
    if (win)
        skip(label);
    else
        check(label, ok);
}

/* ----------------------------------------------------- replaying transport */
typedef struct {
    int codes[32];
    const char *bodies[32];
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

static const char reply_child_ok[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"c1\",\"usage\":{"
    "\"input_tokens\":3,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"child-ok\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":2}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static const char reply_parent_done[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"p2\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"parent-done\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":2}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static const char reply_agent_call[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"p1\",\"usage\":{"
    "\"input_tokens\":4,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"a1\",\"name\":\"agent\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{\\\"op\\\":\\\"spawn\\\",\\\"task\\\":"
    "\\\"do-child\\\"}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
    "\"usage\":{\"output_tokens\":6}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

/* the child's transport reply, shared by the branch/decision tests */
static const char reply_verdict[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"v1\",\"usage\":{"
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

static const AgcTool agent_tool = {
    .name = "agent",
    .label = "Agent",
    .desc = "d",
    .params_json = "{\"type\":\"object\"}",
    .flags = AGENTC_TOOL_CORE,
    .run = agent_run,
};

static AgcAgent *make_root(Replay *r, const AgcTool *tools, size_t nt) {
    AgcAgentSpec s;
    agentc_memset(&s, 0, sizeof s);
    s.provider_handle = agentc_prov_anthropic();
    s.provider = "anthropic";
    s.model = "claude-sonnet-4-5";
    s.thinking = -1;
    s.label = "root";
    s.flags = AGENTC_AGENT_F_WAIT;
    AgcTransport t = {replay_request, r, NULL};
    s.transport = t;
    s.transport_set = true;
    AgcAgent *a = agentc_agent_create(&s);
    if (a && tools)
        agentc_agent_set_tools(a, tools, nt);
    return a;
}

/* --------------------------------------------------------- constructor */
static void test_constructor(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_child_ok;
    AgcAgent *root = make_root(&r, NULL, 0);
    check("create.root", root != NULL && agentc_agent_actor(root) != 0);
    AgcActorId rid = agentc_agent_actor(root);

    AgcAgentSpec cs;
    agentc_memset(&cs, 0, sizeof cs);
    cs.parent = root;
    cs.task = "hello-child";
    cs.thinking = -1;
    cs.flags = AGENTC_AGENT_F_WAIT;
    AgcAgent *child = agentc_agent_create(&cs);
    check("create.child", child != NULL && agentc_agent_actor(child) != 0);
    check("inherit.model", agentc_streq(agentc_agent_model(child), agentc_agent_model(root)));
    check("inherit.transport", agentc_agent_transport(child).ud == agentc_agent_transport(root).ud);
    AgcAgentInfo ci;
    check("inherit.depth", agentc_agent_info(agentc_agent_actor(child), &ci) == 0 &&
                               ci.depth == 1 && ci.parent_id == rid && ci.is_subagent);

    /* explicit override records override=true in ACTOR_STARTED */
    AgcAgentSpec os;
    agentc_memset(&os, 0, sizeof os);
    os.parent = root;
    os.model = "other-model";
    os.task = "x";
    os.thinking = -1;
    os.flags = AGENTC_AGENT_F_WAIT;
    AgcAgent *oc = agentc_agent_create(&os);
    check("override.model", oc != NULL && agentc_streq(agentc_agent_model(oc), "other-model"));
    check("tree.parent", agentc_agent_parent(agentc_agent_actor(oc)) == rid);
    check("tree.find", agentc_agent_find(agentc_agent_actor(oc)) == oc);
    check("tree.list", agentc_agent_list(NULL, 0) >= 3);

    agentc_agent_free(root);
    agentc_actor_release_all();
    check("tree.released", agentc_actor_count(AGENTC_ACTOR_AGENT) == 0);
}

/* ------------------------------------------------- child result as tool result */
static void test_child_tool_result(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 3;
    r.codes[0] = r.codes[1] = r.codes[2] = 0;
    r.bodies[0] = reply_agent_call;
    r.bodies[1] = reply_child_ok;
    r.bodies[2] = reply_parent_done;
    AgcAgent *root = make_root(&r, &agent_tool, 1);
    agentc_agent_set_system(root, "sys");
    agentc_agent_test_no_backoff(root);
    int rc = agentc_agent_submit(root, "go");
    check("tool.spawn.rc", rc == 0);
    const AgcTranscript *tr = agentc_agent_transcript(root);
    bool found = false;
    for (size_t i = 0; i < tr->n; i++) {
        const AgcMsg *m = &tr->msgs[i];
        if (m->role != AGENTC_ROLE_TOOL || m->nblocks == 0)
            continue;
        const AgcBlock *b = &m->blocks[0];
        if (b->text && agentc_streq(b->text, "child-ok"))
            found = true;
    }
    check("tool.spawn.result", found);
    agentc_agent_free(root);
    agentc_actor_release_all();
}

/* ------------------------------------------------------------- depth limit */
static void test_depth_limit(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_child_ok;
    AgcAgent *root = make_root(&r, NULL, 0);
    int save = agentc_config_subagent_max_depth();
    agentc_config_set_subagent_max_depth(1);
    AgcToolCall call;
    agentc_memset(&call, 0, sizeof call);
    call.actor_id = agentc_agent_actor(root);
    call.name = "agent";
    call.args_json = "{\"op\":\"spawn\",\"task\":\"nope\"}";
    AgcBuf out = {0};
    bool is_err = false;
    (void)agent_run(NULL, &call, &out, &is_err);
    check("depth.limit",
          is_err && out.p != NULL && agentc_str_str((const char *)out.p, "depth_limit") != NULL);
    agentc_buf_free(&out);
    agentc_config_set_subagent_max_depth(save);
    agentc_agent_free(root);
    agentc_actor_release_all();
}

/* ----------------------------------------------------- context isolation */
static void test_context_isolation(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = r.codes[1] = 0;
    r.bodies[0] = reply_child_ok;
    r.bodies[1] = reply_child_ok;
    AgcAgent *root = make_root(&r, NULL, 0);
    AgcAgentSpec cs;
    agentc_memset(&cs, 0, sizeof cs);
    cs.parent = root;
    cs.task = "isolated";
    cs.thinking = -1;
    cs.flags = AGENTC_AGENT_F_WAIT;
    AgcAgent *child = agentc_agent_create(&cs);
    const AgcTranscript *ct = agentc_agent_transcript(child);
    check("context.isolated", ct != NULL && ct->n == 2 && ct->msgs[0].nblocks > 0 &&
                                  agentc_streq(ct->msgs[0].blocks[0].text, "isolated"));
    agentc_agent_free(root);
    agentc_actor_release_all();
}

/* ------------------------------------------------------ cancel propagation */
static void test_cancel_tree(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_child_ok;
    AgcAgent *root = make_root(&r, NULL, 0);
    AgcAgentSpec cs;
    agentc_memset(&cs, 0, sizeof cs);
    cs.parent = root;
    cs.thinking = -1;
    cs.flags = AGENTC_AGENT_F_WAIT; /* no task: just a tree node */
    AgcAgent *child = agentc_agent_create(&cs);
    AgcAgentSpec gs = cs;
    gs.parent = child;
    AgcAgent *gc = agentc_agent_create(&gs);
    AgcActorId root_id = agentc_agent_actor(root);
    (void)agentc_actor_cancel(root_id, AGENTC_FREEZE_SUBTREE, 2);
    AgcAgentInfo ci, gi;
    agentc_agent_info(agentc_agent_actor(child), &ci);
    agentc_agent_info(agentc_agent_actor(gc), &gi);
    check("cancel.child", ci.state == AGENTC_ACTOR_CANCELLED);
    check("cancel.grandchild", gi.state == AGENTC_ACTOR_CANCELLED);
    agentc_agent_free(root);
    agentc_actor_release_all();
}

/* ----------------------------------------- hook_branch to an ephemeral child */
static void test_branch(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_verdict;
    AgcAgent *root = make_root(&r, NULL, 0);
    agentc_decision_set_parent(root);
    AgcAgentSpec bs;
    agentc_memset(&bs, 0, sizeof bs);
    bs.provider = "anthropic";
    bs.model = "claude-sonnet-4-5";
    bs.thinking = -1;
    bs.parent = root;
    bs.flags = AGENTC_AGENT_F_WAIT | AGENTC_AGENT_F_EPHEMERAL;
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_HOOK;
    m.body = "{\"tool\":\"read\"}";
    uint64_t defer = 0;
    int rc = agentc_hook_branch(&bs, &m, &defer);
    check("branch.rc", rc == 0 && defer == 0);
    check("branch.result", m.body != NULL && agentc_str_str(m.body, "allow") != NULL);
    agentc_decision_set_parent(NULL);
    agentc_agent_free(root);
    agentc_actor_release_all();
}

/* --------------------------------------------- persistence + tree replay */
static AgcSession *g_obs_session;
static void obs_sink(void *ud, const AgcMsg *m) {
    (void)ud;
    if (g_obs_session)
        (void)agentc_session_append_message(g_obs_session, m);
}

static void test_persistence(void) {
    bool win = agentc_streq(os_platform(), "windows");
    AgcSessionOptions o;
    agentc_memset(&o, 0, sizeof o);
    o.cwd = ".";
    g_obs_session = agentc_session_new(&o);
    check_win("persist.session", win,
              (g_obs_session != NULL && agentc_session_path(g_obs_session) != NULL));

    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_child_ok;
    AgcAgent *root = make_root(&r, NULL, 0);
    agentc_agent_set_observer(root, obs_sink, NULL);
    agentc_agent_set_system(root, "sys");
    agentc_agent_test_no_backoff(root);
    (void)agentc_agent_submit(root, "hi");
    AgcActorId rid = agentc_agent_actor(root);

    AgcTranscript tr;
    agentc_transcript_init(&tr);
    (void)agentc_session_load_messages(g_obs_session, &tr);
    bool stamped = false;
    for (size_t i = 0; i < tr.n; i++)
        if (tr.msgs[i].role == AGENTC_ROLE_USER && tr.msgs[i].agent_id == rid)
            stamped = true;
    check_win("persist.stamp", win, stamped);
    agentc_transcript_free(&tr);

    const char rec[] = "{\"type\":\"custom\",\"custom_type\":\"agent\",\"data\":{\"op\":\"spawn\","
                       "\"agent_id\":7,\"parent_id\":2,\"depth\":1,\"provider\":\"anthropic\","
                       "\"model\":\"m\"}}\n";
    (void)agentc_session_append_raw(g_obs_session, rec, sizeof rec - 1);
    AgcSessionAgent agents[4];
    size_t n = agentc_session_load_agents(g_obs_session, agents, 4);
    check_win("persist.agents", win,
              (n >= 1 && agents[0].id == 7 && agents[0].parent_id == 2 && agents[0].depth == 1 &&
               agentc_streq(agents[0].op, "spawn")));
    agentc_session_agents_free(agents, n < 4 ? n : 4);

    agentc_agent_free(root);
    agentc_actor_release_all();
    agentc_session_close(g_obs_session);
    g_obs_session = NULL;
}

/* -------------------------------- background one-model-step-per-tick slice */
/* A tool-use turn followed by a final text turn: the background child must issue
 * exactly one provider request per actor tick (P6 completion pass 2e). */
static const char reply_echo_call[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"e1\",\"usage\":{"
    "\"input_tokens\":4,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"tool_use\",\"id\":\"e1\",\"name\":\"echo\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"input_json_delta\",\"partial_json\":\"{}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},"
    "\"usage\":{\"output_tokens\":6}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

static int echo_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out, bool *is_error) {
    (void)self;
    (void)call;
    agentc_buf_cstr(out, "ok");
    *is_error = false;
    return 0;
}

static const AgcTool echo_tool = {
    .name = "echo",
    .label = "Echo",
    .desc = "d",
    .params_json = "{\"type\":\"object\"}",
    .flags = AGENTC_TOOL_CORE,
    .run = echo_run,
};

static void test_background_slice(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = r.codes[1] = 0;
    r.bodies[0] = reply_echo_call; /* step 1: one tool_use turn */
    r.bodies[1] = reply_child_ok;  /* step 2: final text */
    AgcAgent *root = make_root(&r, &echo_tool, 1);
    agentc_agent_test_no_backoff(root);

    AgcAgentSpec cs;
    agentc_memset(&cs, 0, sizeof cs);
    cs.parent = root;
    cs.task = "bg-task";
    cs.thinking = -1;
    cs.flags = AGENTC_AGENT_F_BACKGROUND;
    AgcAgent *child = agentc_agent_create(&cs);
    check("slice.create", child != NULL && agentc_agent_actor(child) != 0);
    if (child == NULL) {
        agentc_agent_free(root);
        agentc_actor_release_all();
        return;
    }
    agentc_agent_test_no_backoff(child);
    AgcActorId cid = agentc_agent_actor(child);
    /* A background child registers but issues no request until a tick. */
    check("slice.pre", r.i == 0);

    /* First tick: exactly one model step (provider response + tool batch). */
    agentc_actor_tick();
    check("slice.step1", r.i == 1);

    /* Second tick: the next step, which settles the turn. */
    agentc_actor_tick();
    check("slice.step2", r.i == 2);
    const AgcTranscript *tr = agentc_agent_transcript(child);
    check("slice.transcript", tr != NULL && tr->n == 4);
    AgcBuf res = {0};
    check("slice.result", agentc_agent_result(cid, &res) == 0 && res.p != NULL &&
                              agentc_streq((const char *)res.p, "child-ok"));
    agentc_buf_free(&res);
    AgcActor *row = agentc_actor_get(cid);
    check("slice.settled", row != NULL && row->state == AGENTC_ACTOR_SETTLED);

    agentc_agent_free(root);
    agentc_actor_release_all();
}

/* A no-tool `step_end.continue` must also yield one step per actor tick rather
 * than coalescing the extra model step into the same slice. A core `step` end
 * handler returns continue once (and consumes, so the mapped legacy point
 * cannot overwrite the result). */
static int g_no_tool_cont_calls;
static int no_tool_continue_fn(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)defer;
    *next = m;
    if (m->meta != NULL && agentc_streq(m->meta, "end") && g_no_tool_cont_calls == 0) {
        g_no_tool_cont_calls++;
        m->result_json = agentc_strdup("{\"continue\":true}");
        *next = NULL; /* consume: the legacy point must not overwrite the result */
    }
    return 0;
}

static void test_background_no_tool_slice(void) {
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 2;
    r.codes[0] = r.codes[1] = 0;
    r.bodies[0] = reply_child_ok;
    r.bodies[1] = reply_child_ok;
    AgcAgent *root = make_root(&r, &echo_tool, 1);
    agentc_agent_test_no_backoff(root);

    g_no_tool_cont_calls = 0;
    uint64_t hh = agentc_hook(AGENTC_HOOK_STAGE_STEP, 0, no_tool_continue_fn, NULL);
    check("notool.reg", hh != 0);

    AgcAgentSpec cs;
    agentc_memset(&cs, 0, sizeof cs);
    cs.parent = root;
    cs.task = "bg-notool";
    cs.thinking = -1;
    cs.flags = AGENTC_AGENT_F_BACKGROUND;
    AgcAgent *child = agentc_agent_create(&cs);
    check("notool.create", child != NULL && agentc_agent_actor(child) != 0);
    if (child == NULL) {
        agentc_hook_off(hh);
        agentc_agent_free(root);
        agentc_actor_release_all();
        return;
    }
    agentc_agent_test_no_backoff(child);
    AgcActorId cid = agentc_agent_actor(child);

    /* First tick: one no-tool model step, then the continue yields. */
    agentc_actor_tick();
    check("notool.step1", r.i == 1);
    /* Second tick: the continuation step settles the turn. */
    agentc_actor_tick();
    check("notool.step2", r.i == 2);
    const AgcTranscript *tr = agentc_agent_transcript(child);
    check("notool.transcript", tr != NULL && tr->n == 3);
    AgcBuf res = {0};
    check("notool.result", agentc_agent_result(cid, &res) == 0 && res.p != NULL &&
                               agentc_streq((const char *)res.p, "child-ok"));
    agentc_buf_free(&res);
    AgcActor *row = agentc_actor_get(cid);
    check("notool.settled", row != NULL && row->state == AGENTC_ACTOR_SETTLED);
    check("notool.cont_once", g_no_tool_cont_calls == 1);

    agentc_hook_off(hh);
    agentc_agent_free(root);
    agentc_actor_release_all();
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    agentc_log_set_level(4);
    test_constructor();
    test_child_tool_result();
    test_depth_limit();
    test_context_isolation();
    test_cancel_tree();
    test_branch();
    test_persistence();
    test_background_slice();
    test_background_no_tool_slice();
    return fails;
}
