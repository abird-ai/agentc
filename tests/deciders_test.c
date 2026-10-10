/* deciders_test.c — P6-D decision seam: controller/router, parsing, fail-closed,
 * memoization, events, and a model decider branched to an ephemeral child. */
#include "agent.h"
#include "core/events.h"
#include "ext.h"
#include "plat.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

/* A Windows skip prints `skip` (tests/data/deciders_test.windows.expected),
 * never a hard-coded 1. */
static void skip(const char *label) {
    agentc_outf("%s=skip\n", label);
}

/* ----------------------------------------------------- replaying transport */
typedef struct {
    int codes[16];
    const char *bodies[16];
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

static const char reply_verdict[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"v1\",\"usage\":{"
    "\"input_tokens\":3,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"{\\\"verdict\\\":\\\"allow\\\",\\\"reason\\\":\\\"ok\\\"}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":2}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

/* --------------------------------------------------------------- events */
static int g_req, g_res, g_route;
static void on_event(void *ud, int ev, const void *data) {
    (void)ud;
    (void)data;
    switch (ev) {
    case AGENTC_EV_DECISION_REQUEST:
        g_req++;
        break;
    case AGENTC_EV_DECISION_RESULT:
        g_res++;
        break;
    case AGENTC_EV_ROUTE_DECIDED:
        g_route++;
        break;
    default:
        break;
    }
}

static AgcDeciderSpec local_spec(const char *id, const char *resp, uint32_t flags) {
    AgcDeciderSpec s;
    agentc_memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    s.id = id;
    s.intent = "approve";
    s.kind = AGENTC_DECIDER_LOCAL;
    s.command = resp;
    s.flags = flags;
    return s;
}

static int run_controller(AgcControllerCtx *ctx, const char *body, AgcMessage *m,
                          AgcMessage **next) {
    agentc_memset(m, 0, sizeof *m);
    m->class = AGENTC_MSG_CLASS_HOOK;
    m->kind = AGENTC_MSG_INPUT;
    m->meta = "read";
    m->body = body;
    m->body_len = agentc_strlen(body);
    m->id = 7;
    uint64_t defer = 0;
    *next = m;
    return agentc_controller_handler(ctx, m, next, &defer);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    agentc_log_set_level(4);

    AgcDeciderSpec s =
        local_spec("ctl-narrow", "{\"verdict\":\"narrow\",\"args\":{\"path\":\"narrowed\"}}", 0);
    check("D.register", agentc_decider_register(&s) == 0 && agentc_decider_count() >= 1);
    AgcControllerCtx *ctx = agentc_controller_new(&s);
    check("D.ctx", ctx != NULL);

    AgcMessage m, *next = NULL;
    (void)run_controller(ctx, "{\"path\":\"orig\"}", &m, &next);
    check("D.narrow",
          next != NULL && next->body != NULL && agentc_str_str(next->body, "narrowed") != NULL);
    int memo1 = agentc_decision_memo_count();
    (void)run_controller(ctx, "{\"path\":\"orig\"}", &m, &next);
    check("D.memo", agentc_decision_memo_count() == memo1 && next != NULL &&
                        agentc_str_str(next->body, "narrowed") != NULL);
    agentc_controller_free(ctx);

    AgcDeciderSpec d = local_spec("ctl-deny", "{\"verdict\":\"deny\",\"reason\":\"nope\"}", 0);
    (void)agentc_decider_register(&d);
    AgcControllerCtx *dc = agentc_controller_new(&d);
    (void)run_controller(dc, "{\"x\":1}", &m, &next);
    check("D.deny", next == NULL);
    agentc_controller_free(dc);

    AgcDeciderSpec rep =
        local_spec("ctl-replace",
                   "{\"verdict\":\"replace\",\"replace\":{\"op\":\"spawn\",\"task\":\"x\"}}", 0);
    (void)agentc_decider_register(&rep);
    AgcControllerCtx *repc = agentc_controller_new(&rep);
    (void)run_controller(repc, "{\"x\":1}", &m, &next);
    check("D.replace", next != NULL && next->meta != NULL && agentc_streq(next->meta, "agent") &&
                           next->body != NULL && agentc_str_str(next->body, "spawn") != NULL);
    agentc_controller_free(repc);

    AgcDeciderSpec bad = local_spec("ctl-bad", "not-json", 0);
    (void)agentc_decider_register(&bad);
    AgcControllerCtx *bc = agentc_controller_new(&bad);
    (void)run_controller(bc, "{\"x\":1}", &m, &next);
    check("D.failclosed", next == NULL);
    agentc_controller_free(bc);

    AgcDeciderSpec route = local_spec("router", "not-json", 0);
    route.intent = "route";
    (void)agentc_decider_register(&route);
    AgcControllerCtx *rt = agentc_controller_new(&route);
    (void)run_controller(rt, "{\"x\":1}", &m, &next);
    check("D.route_default", next == &m);
    agentc_controller_free(rt);

    check("D.parse_ok", agentc_decision_submit("{\"verdict\":\"allow\"}") == 0);
    check("D.parse_bad", agentc_decision_submit("garbage") == -22);

    (void)agentc_events_subscribe(on_event, NULL);
    s = local_spec("ctl-events", "{\"verdict\":\"allow\"}", 0);
    (void)agentc_decider_register(&s);
    AgcControllerCtx *ec = agentc_controller_new(&s);
    (void)run_controller(ec, "{\"x\":1}", &m, &next);
    check("D.events", g_req >= 1 && g_res >= 1);
    agentc_controller_free(ec);

    /* model decider: branch to an ephemeral child on the shared transport */
    Replay r;
    agentc_memset(&r, 0, sizeof r);
    r.n = 1;
    r.codes[0] = 0;
    r.bodies[0] = reply_verdict;
    AgcAgentSpec as;
    agentc_memset(&as, 0, sizeof as);
    as.provider_handle = agentc_prov_anthropic();
    as.provider = "anthropic";
    as.model = "claude-sonnet-4-5";
    as.thinking = -1;
    as.flags = AGENTC_AGENT_F_WAIT;
    AgcTransport t = {replay_request, &r, NULL};
    as.transport = t;
    as.transport_set = true;
    AgcAgent *root = agentc_agent_create(&as);
    agentc_decision_set_parent(root);
    AgcDeciderSpec ms;
    agentc_memset(&ms, 0, sizeof ms);
    ms.id = "jev";
    ms.intent = "approve";
    ms.kind = AGENTC_DECIDER_MODEL;
    uint64_t defer = 0;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_HOOK;
    m.body = "{\"tool\":\"bash\"}";
    m.body_len = agentc_strlen(m.body);
    int drc = agentc_decide(&ms, &m, &defer);
    check("D.model_branch",
          drc == 0 && defer == 0 && m.body != NULL && agentc_str_str(m.body, "allow") != NULL);
    agentc_decision_set_parent(NULL);
    agentc_agent_free(root);
    agentc_actor_release_all();

    /* async decision seam: a controller with a MODEL decider parks on a
     * self-resuming background branch and registers an out-of-band pending
     * row; agentc_decision_submit resumes the same handler, which now applies
     * the decision instead of branching again. */
    {
        Replay ar;
        agentc_memset(&ar, 0, sizeof ar);
        ar.n = 1;
        ar.codes[0] = 0;
        ar.bodies[0] = reply_verdict;
        AgcAgentSpec aas;
        agentc_memset(&aas, 0, sizeof aas);
        aas.provider_handle = agentc_prov_anthropic();
        aas.provider = "anthropic";
        aas.model = "claude-sonnet-4-5";
        aas.thinking = -1;
        aas.flags = AGENTC_AGENT_F_WAIT;
        AgcTransport at = {replay_request, &ar, NULL};
        aas.transport = at;
        aas.transport_set = true;
        AgcAgent *aroot = agentc_agent_create(&aas);
        agentc_decision_set_parent(aroot);
        AgcDeciderSpec ays;
        agentc_memset(&ays, 0, sizeof ays);
        ays.id = "ctl-async";
        ays.intent = "approve";
        ays.kind = AGENTC_DECIDER_MODEL;
        AgcControllerCtx *actx = agentc_controller_new(&ays);
        uint64_t ah = agentc_hook("input", 0, agentc_controller_handler, actx);
        AgcMessage am;
        agentc_memset(&am, 0, sizeof am);
        am.class = AGENTC_MSG_CLASS_HOOK;
        am.kind = AGENTC_MSG_INPUT;
        am.body = "{\"tool\":\"bash\"}";
        am.body_len = agentc_strlen(am.body);
        am.id = 5150;
        AgcMessage *anext = NULL;
        int arc2 = agentc_hook_run("input", &am, &anext);
        check("D.async_park", arc2 == -115);
        check("D.async_pending", agentc_decision_pending_count() == 1);
        int src = agentc_decision_submit(
            "{\"verdict\":\"narrow\",\"args\":{\"path\":\"narrowed\"},\"id\":5150}");
        check("D.async_submit", src == 0 && agentc_decision_pending_count() == 0);
        /* a second submit with the same id has no pending row: validate-only */
        check("D.async_submit_again",
              agentc_decision_submit("{\"verdict\":\"allow\",\"id\":5150}") == 0 &&
                  agentc_decision_pending_count() == 0);
        agentc_hook_off(ah);
        agentc_controller_free(actx);
        agentc_decision_set_parent(NULL);
        agentc_agent_free(aroot);
        agentc_actor_release_all();
        for (int i = 0; i < 8 && agentc_runtime_defer_count() != 0; i++)
            agentc_runtime_tick();
        check("D.async_released", agentc_runtime_defer_count() == 0);
    }

    /* program decider: JSON stdio over a process actor. POSIX shells provide
     * printf; Windows has no POSIX shell, so the allow path prints the same
     * label without running it, while the fail-closed path runs everywhere. */
    bool posix = agentc_streq(os_platform(), "linux") || agentc_streq(os_platform(), "macos");
    AgcDeciderSpec ps;
    agentc_memset(&ps, 0, sizeof ps);
    ps.id = "prog";
    ps.intent = "approve";
    ps.kind = AGENTC_DECIDER_PROGRAM;
    ps.command = "printf '%s' '{\"verdict\":\"allow\",\"reason\":\"prog ok\"}'";
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_HOOK;
    m.body = "{\"tool\":\"read\"}";
    m.body_len = agentc_strlen(m.body);
    uint64_t pdefer = 0;
    if (posix) {
        int prc = agentc_decide(&ps, &m, &pdefer);
        check("D.program_allow",
              prc == 0 && pdefer == 0 && m.body != NULL && agentc_str_str(m.body, "allow") != NULL);
    } else {
        skip("D.program_allow");
    }

    /* a decider program that cannot run fails closed with a deny verdict */
    AgcDeciderSpec badp;
    agentc_memset(&badp, 0, sizeof badp);
    badp.id = "prog-missing";
    badp.intent = "approve";
    badp.kind = AGENTC_DECIDER_PROGRAM;
    badp.command = "/nonexistent/agentc-decider-xyz";
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_HOOK;
    m.body = "{\"tool\":\"read\"}";
    m.body_len = agentc_strlen(m.body);
    pdefer = 0;
    int brc = agentc_decide(&badp, &m, &pdefer);
    check("D.program_failclosed",
          brc == 0 && pdefer == 0 && m.body != NULL && agentc_str_str(m.body, "deny") != NULL);

    /* mcp decider: a tool name that is not registered fails closed with a deny
     * verdict. The live allow path needs a connected MCP server; the mock
     * server in tests/mcp.sh covers it. */
    AgcDeciderSpec mcp;
    agentc_memset(&mcp, 0, sizeof mcp);
    mcp.id = "mcp-missing";
    mcp.intent = "approve";
    mcp.kind = AGENTC_DECIDER_MCP;
    mcp.command = "mcp__nope__nope";
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_HOOK;
    m.body = "{\"tool\":\"read\"}";
    m.body_len = agentc_strlen(m.body);
    uint64_t mdefer = 0;
    int mrc = agentc_decide(&mcp, &m, &mdefer);
    check("D.mcp_failclosed",
          mrc == 0 && mdefer == 0 && m.body != NULL && agentc_str_str(m.body, "deny") != NULL);

    /* host decider_register slot (append-only, HAS-gated) */
    {
        const AgcExtHost *h = agentc_ext_host();
        check("D.host_slot", AGENTC_EXT_HOST_HAS(h, decider_register));
        int hrc = AGENTC_EXT_HOST_HAS(h, decider_register)
                      ? h->decider_register(h, "{\"id\":\"host-prog\",\"intent\":\"approve\","
                                               "\"kind\":\"program\",\"command\":\"true\"}")
                      : -1;
        check("D.host_register", hrc == 0 && agentc_decider_count() >= 1);
        check("D.host_bad_kind",
              h->decider_register(h, "{\"id\":\"x\",\"kind\":\"bogus\"}") == -22);
    }

    agentc_decider_reset();
    return fails;
}
