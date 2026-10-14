/* trust_test.c — P6-C trust envelope, monotonicity, decide, path checks,
 * policy and the approval contract (see .agents/plans/P6-TRUST.md §13).
 *
 * Golden mode: every check prints label=0|1; approval/audit lines are part of
 * the golden output too.
 */
#include "agent.h"
#include "config.h"
#include "plat.h"
#include "ext.h"
#include "app/project.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

/* A Windows-only skip prints `skip`, never a hard-coded 1, so the Wine golden
 * (tests/data/trust_test.windows.expected) cannot hide a regression. */
static void skip(const char *label) {
    agentc_outf("%s=skip\n", label);
}

static bool env_eq(const AgcTrustEnv *a, const AgcTrustEnv *b) {
    if (a->access != b->access || a->boundary != b->boundary || a->approval != b->approval ||
        a->network != b->network || a->flags != b->flags || a->nroots != b->nroots ||
        a->nhosts != b->nhosts)
        return false;
    for (size_t i = 0; i < a->nroots; i++)
        if (!agentc_streq(a->roots[i], b->roots[i]))
            return false;
    for (size_t i = 0; i < a->nhosts; i++)
        if (!agentc_streq(a->hosts[i], b->hosts[i]))
            return false;
    if ((a->policy_id == NULL) != (b->policy_id == NULL))
        return false;
    if (a->policy_id && !agentc_streq(a->policy_id, b->policy_id))
        return false;
    return true;
}

static void set_env(AgcTrustEnv *e, int access, int boundary, int approval, int network,
                    uint32_t flags) {
    agentc_memset(e, 0, sizeof *e);
    e->access = access;
    e->boundary = boundary;
    e->approval = approval;
    e->network = network;
    e->flags = flags;
}

/* ------------------------------------------------------------------ UI */
static int g_ui_allow;
static int ui_fn(void *ud, const AgcApprovalRequest *req, AgcApprovalResult *out) {
    (void)ud;
    (void)req;
    agentc_memset(out, 0, sizeof *out);
    out->id = req->id;
    out->decision = g_ui_allow ? AGENTC_DEC_ALLOW : AGENTC_DEC_DENY;
    out->reason = agentc_strdup(g_ui_allow ? "user allowed" : "user denied");
    out->scope = AGENTC_SCOPE_STEP;
    return 0;
}

/* Finding 2 (VC1b): an approver that narrows the call's args. */
static int ui_narrow_fn(void *ud, const AgcApprovalRequest *req, AgcApprovalResult *out) {
    (void)ud;
    agentc_memset(out, 0, sizeof *out);
    out->id = req->id;
    out->decision = AGENTC_DEC_ALLOW;
    out->reason = agentc_strdup("narrowed");
    out->scope = AGENTC_SCOPE_STEP;
    out->args_json = agentc_strdup("{\"path\":\"/tmp/safe\"}");
    return 0;
}

/* VCF1-2: an approver that answers `ask`. `ask` is not a terminal verdict (no
 * further approver exists in agentc_approval_request), so the core must
 * normalize it to DENY rather than let it reach the tool dispatch. */
static int ui_ask_fn(void *ud, const AgcApprovalRequest *req, AgcApprovalResult *out) {
    (void)ud;
    agentc_memset(out, 0, sizeof *out);
    out->id = req->id;
    out->decision = AGENTC_DEC_ASK;
    out->reason = agentc_strdup("needs another approver");
    out->scope = AGENTC_SCOPE_ONCE;
    return 0;
}

/* ---- async approval hook: park, then answer out of band from the pump ---- */
static uint64_t g_appr_hook;
static uint64_t g_appr_submit;

/* Park an out-of-band answer and then cancel the request: the parked
 * reason/args_json must be freed before the slot is cleared (S-fix). */
static int ui_submit_then_cancel(void *ud, const AgcApprovalRequest *req, AgcApprovalResult *out) {
    (void)ud;
    (void)out;
    char rsp[192];
    agentc_snprintf(rsp, sizeof rsp,
                    "{\"id\":%llu,\"decision\":\"allow\",\"scope\":\"once\","
                    "\"reason\":\"parked\",\"args\":{\"path\":\"/x\"}}",
                    (unsigned long long)req->id);
    (void)agentc_approval_submit(rsp);
    (void)agentc_approval_cancel(req->id, "test");
    return -38; /* not answered: fail closed */
}

static void appr_answer_from_pump(void *ud, AgcMessage *unused) {
    (void)ud;
    (void)unused;
    agentc_runtime_cancel_defer(g_appr_submit);
    g_appr_submit = 0;
    (void)agentc_approval_submit(
        "{\"decision\":\"allow\",\"scope\":\"once\",\"reason\":\"pumped\"}");
}

static int appr_park_hook(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)m;
    (void)next;
    g_appr_submit = agentc_runtime_defer(appr_answer_from_pump, NULL);
    if (defer)
        *defer = g_appr_submit;
    return 0;
}

/* Finding 1 (VC1b): two deferred requests share a slot. The first is answered
 * out of band; the second must NOT adopt that verdict. This pump answers allow
 * for the first request and deny for the second, so the test can tell them
 * apart even though the slot is reused. */
static int g_appr_count;
static void appr_answer_counted(void *ud, AgcMessage *unused) {
    (void)ud;
    (void)unused;
    agentc_runtime_cancel_defer(g_appr_submit);
    g_appr_submit = 0;
    if (g_appr_count++ == 0)
        (void)agentc_approval_submit(
            "{\"decision\":\"allow\",\"scope\":\"once\",\"reason\":\"first\"}");
    else
        (void)agentc_approval_submit(
            "{\"decision\":\"deny\",\"scope\":\"once\",\"reason\":\"second\"}");
}

static int appr_park_counted(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    (void)ud;
    (void)m;
    (void)next;
    g_appr_submit = agentc_runtime_defer(appr_answer_counted, NULL);
    if (defer)
        *defer = g_appr_submit;
    return 0;
}

/* --------------------------------------------------------------- audit */
static char g_audit[1024];
static void audit_fn(void *ud, const char *json, size_t n) {
    (void)ud;
    size_t c = n < sizeof g_audit - 1 ? n : sizeof g_audit - 1;
    agentc_memcpy(g_audit, json, c);
    g_audit[c] = 0;
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    agentc_trust_set_widen_clamp(false);
    agentc_trust_set_cli(NULL, NULL, NULL, NULL, NULL);
    agentc_trust_scope_clear_session();

    char cwd[4096];
    (void)os_getcwd(cwd, sizeof cwd);

    /* ---- default + JSON round trip ---- */
    AgcTrustEnv d;
    check("default.ok", agentc_trust_default(&d, cwd) == 0);
    check("default.access", d.access == AGENTC_ACCESS_RW);
    check("default.boundary", d.boundary == AGENTC_BOUND_WORKSPACE);
    check("default.approval", d.approval == AGENTC_APPROVAL_AUTO);
    check("default.network", d.network == AGENTC_NET_DENY);
    check("default.roots", d.nroots == 1 && agentc_streq(d.roots[0], cwd));
    check("default.flags", (d.flags & AGENTC_TRUST_ALLOW_PTY) &&
                               (d.flags & AGENTC_TRUST_ALLOW_BACKGROUND) &&
                               (d.flags & AGENTC_TRUST_ALLOW_SUBAGENT));

    AgcBuf jb = {0};
    check("json.write", agentc_trust_to_json(&d, &jb) == 0 && jb.len > 0);
    AgcTrustEnv rt;
    char err[128] = "";
    check("json.read", agentc_trust_from_json((char *)jb.p, jb.len, &rt, err, sizeof err) == 0);
    check("json.roundtrip", env_eq(&d, &rt));
    agentc_buf_free(&jb);

    /* ---- S5: hosts survive a to_json/from_json round trip ---- */
    {
        AgcTrustEnv he;
        set_env(&he, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_ALLOWLIST, AGENTC_TRUST_ALLOW_PTY);
        he.roots = agentc_alloc(sizeof(char *));
        he.roots[0] = agentc_strdup("/tmp");
        he.nroots = 1;
        he.hosts = agentc_alloc(2 * sizeof(char *));
        he.hosts[0] = agentc_strdup("*.example.com");
        he.hosts[1] = agentc_strdup("localhost");
        he.nhosts = 2;
        AgcBuf hb = {0};
        AgcTrustEnv hrt;
        agentc_memset(&hrt, 0, sizeof hrt);
        char herr[128] = "";
        check("json.hosts_write", agentc_trust_to_json(&he, &hb) == 0);
        check("json.hosts_read",
              agentc_trust_from_json((char *)hb.p, hb.len, &hrt, herr, sizeof herr) == 0);
        check("json.hosts_roundtrip",
              env_eq(&he, &hrt) && hrt.nhosts == 2 &&
                  agentc_streq(hrt.hosts[0], "*.example.com") &&
                  agentc_streq(hrt.hosts[1], "localhost"));
        agentc_buf_free(&hb);
        agentc_trust_env_free(&hrt);
        agentc_trust_env_free(&he);
    }

    AgcBuf db = {0};
    agentc_trust_describe(&d, &db);
    check("describe", agentc_streq((char *)db.p, "read-write/workspace/auto/deny roots=1"));
    agentc_buf_free(&db);
    agentc_trust_env_free(&rt);
    agentc_trust_env_free(&d);

    /* ---- load: default with no config ---- */
    AgcTrustEnv le;
    check("load.ok", agentc_trust_load(cwd, true, &le) == 0);
    check("load.default", le.access == AGENTC_ACCESS_RW && le.boundary == AGENTC_BOUND_WORKSPACE);
    agentc_trust_env_free(&le);

    /* ---- monotonic narrowing ---- */
    AgcTrustEnv parent, req, out;
    char nerr[128] = "";
    set_env(&parent, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
            AGENTC_NET_DENY, AGENTC_TRUST_ALLOW_PTY | AGENTC_TRUST_ALLOW_BACKGROUND);
    parent.roots = agentc_alloc(sizeof(char *));
    parent.roots[0] = agentc_strdup(cwd);
    parent.nroots = 1;

    set_env(&req, AGENTC_ACCESS_RO, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO, AGENTC_NET_DENY,
            AGENTC_TRUST_ALLOW_PTY);
    check("narrow.ro_ok", agentc_trust_narrow(&parent, &req, &out, nerr, sizeof nerr) == 0);
    check("narrow.ro_effect",
          out.access == AGENTC_ACCESS_RO && out.flags == AGENTC_TRUST_ALLOW_PTY);
    agentc_trust_env_free(&out);

    set_env(&req, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO, AGENTC_NET_DENY,
            AGENTC_TRUST_ALLOW_PTY);
    AgcTrustEnv ro_parent;
    set_env(&ro_parent, AGENTC_ACCESS_RO, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
            AGENTC_NET_DENY, AGENTC_TRUST_ALLOW_PTY);
    check("narrow.widen_access_denied",
          agentc_trust_narrow(&ro_parent, &req, &out, nerr, sizeof nerr) != 0);

    agentc_trust_set_widen_clamp(true);
    check("narrow.widen_access_clamp",
          agentc_trust_narrow(&ro_parent, &req, &out, nerr, sizeof nerr) == 0 &&
              out.access == AGENTC_ACCESS_RO);
    agentc_trust_env_free(&out);
    agentc_trust_set_widen_clamp(false);

    set_env(&req, AGENTC_ACCESS_RW, AGENTC_BOUND_STRICT, AGENTC_APPROVAL_ASK, AGENTC_NET_DENY,
            AGENTC_TRUST_ALLOW_PTY);
    check("narrow.tighten_ok", agentc_trust_narrow(&parent, &req, &out, nerr, sizeof nerr) == 0 &&
                                   out.boundary == AGENTC_BOUND_STRICT &&
                                   out.approval == AGENTC_APPROVAL_ASK);
    agentc_trust_env_free(&out);

    set_env(&req, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO, AGENTC_NET_DENY,
            AGENTC_TRUST_ALLOW_PTY | AGENTC_TRUST_ALLOW_NET_UNATTENDED);
    check("narrow.flags_denied", agentc_trust_narrow(&parent, &req, &out, nerr, sizeof nerr) != 0);

    set_env(&req, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO, AGENTC_NET_DENY,
            AGENTC_TRUST_ALLOW_PTY);
    req.roots = agentc_alloc(sizeof(char *));
    req.roots[0] = agentc_strdup("/");
    req.nroots = 1;
    check("narrow.roots_denied", agentc_trust_narrow(&parent, &req, &out, nerr, sizeof nerr) != 0);
    agentc_free((void *)req.roots[0]);
    agentc_free((void *)req.roots);

    /* S1: network narrowing is by permissiveness rank (ALLOW > ALLOWLIST >
     * DENY), not enum value. A child ALLOW under an ALLOWLIST parent must be
     * rejected (or clamped), never silently accepted. */
    {
        AgcTrustEnv np, nr, no2;
        set_env(&np, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_ALLOWLIST, AGENTC_TRUST_ALLOW_PTY);
        np.roots = agentc_alloc(sizeof(char *));
        np.roots[0] = agentc_strdup(cwd);
        np.nroots = 1;
        set_env(&nr, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_ALLOW, AGENTC_TRUST_ALLOW_PTY);
        check("narrow.net_allow_under_allowlist_denied",
              agentc_trust_narrow(&np, &nr, &no2, nerr, sizeof nerr) != 0);
        agentc_trust_set_widen_clamp(true);
        check("narrow.net_allow_under_allowlist_clamp",
              agentc_trust_narrow(&np, &nr, &no2, nerr, sizeof nerr) == 0 &&
                  no2.network == AGENTC_NET_ALLOWLIST);
        agentc_trust_env_free(&no2);
        agentc_trust_set_widen_clamp(false);
        /* tightening allowlist -> deny stays allowed */
        set_env(&nr, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, AGENTC_TRUST_ALLOW_PTY);
        check("narrow.net_deny_under_allowlist_ok",
              agentc_trust_narrow(&np, &nr, &no2, nerr, sizeof nerr) == 0 &&
                  no2.network == AGENTC_NET_DENY);
        agentc_trust_env_free(&no2);
        agentc_trust_env_free(&np);
    }

    /* S6: depth is monotone lineage; a child can neither claim an arbitrary
     * depth nor lie shallower than its parent. */
    {
        AgcTrustEnv dp, dq, do2;
        set_env(&dp, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, AGENTC_TRUST_ALLOW_PTY);
        dp.depth = 3;
        set_env(&dq, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, AGENTC_TRUST_ALLOW_PTY);
        dq.depth = 99;
        check("narrow.depth_clamped",
              agentc_trust_narrow(&dp, &dq, &do2, nerr, sizeof nerr) == 0 && do2.depth == 4);
        agentc_trust_env_free(&do2);
        dq.depth = 0;
        check("narrow.depth_floor",
              agentc_trust_narrow(&dp, &dq, &do2, nerr, sizeof nerr) == 0 && do2.depth == 3);
        agentc_trust_env_free(&do2);
        /* Finding 5 (VC1b): depth saturates at UINT16_MAX instead of wrapping
         * 65535+1 to 0. */
        dp.depth = 65535;
        dq.depth = 65535;
        check("narrow.depth_saturate",
              agentc_trust_narrow(&dp, &dq, &do2, nerr, sizeof nerr) == 0 && do2.depth == 65535);
        agentc_trust_env_free(&do2);
    }

    /* S3: policy_id selects the approval backend and is parent-owned. */
    {
        AgcTrustEnv pp, pr, po2;
        set_env(&pp, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, AGENTC_TRUST_ALLOW_PTY);
        pp.policy_id = "parent-policy";
        set_env(&pr, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, AGENTC_TRUST_ALLOW_PTY);
        pr.policy_id = "attacker-policy";
        check("narrow.policy_id_denied",
              agentc_trust_narrow(&pp, &pr, &po2, nerr, sizeof nerr) != 0);
        agentc_trust_set_widen_clamp(true);
        check("narrow.policy_id_clamp",
              agentc_trust_narrow(&pp, &pr, &po2, nerr, sizeof nerr) == 0 && po2.policy_id &&
                  agentc_streq(po2.policy_id, "parent-policy"));
        agentc_trust_env_free(&po2);
        agentc_trust_set_widen_clamp(false);
        pr.policy_id = "parent-policy";
        check("narrow.policy_id_same_ok",
              agentc_trust_narrow(&pp, &pr, &po2, nerr, sizeof nerr) == 0 && po2.policy_id &&
                  agentc_streq(po2.policy_id, "parent-policy"));
        agentc_trust_env_free(&po2);
        pp.policy_id = NULL;
        pr.policy_id = "attacker-policy";
        check("narrow.policy_id_from_null_denied",
              agentc_trust_narrow(&pp, &pr, &po2, nerr, sizeof nerr) != 0);
    }

    agentc_trust_env_free(&parent);

    /* ---- decide truth table ---- */
    AgcTrustEnv e;
    set_env(&e, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO, AGENTC_NET_DENY,
            AGENTC_TRUST_ALLOW_PTY);
    e.roots = agentc_alloc(sizeof(char *));
    e.roots[0] = agentc_strdup(cwd);
    e.nroots = 1;
    char inpath[4200];
    agentc_snprintf(inpath, sizeof inpath, "{\"path\":\"%s/inside.txt\"}", cwd);

    AgcTrustDecision dec;
    (void)agentc_trust_decide(&e, "write", inpath, &dec);
    check("decide.rw_inside_allow", dec.decision == AGENTC_DEC_ALLOW);

    e.access = AGENTC_ACCESS_RO;
    (void)agentc_trust_decide(&e, "write", inpath, &dec);
    check("decide.ro_write_deny", dec.decision == AGENTC_DEC_DENY);
    (void)agentc_trust_decide(&e, "edit", "{}", &dec);
    check("decide.ro_edit_deny", dec.decision == AGENTC_DEC_DENY);
    /* S4/D2: the name-only legacy entry point has no capability flags, so it is
     * deliberately conservative: "read" is treated as mutating and denied. */
    (void)agentc_trust_decide(&e, "read", inpath, &dec);
    check("decide.ro_read_no_flags_deny", dec.decision == AGENTC_DEC_DENY);
    /* a declared read-only capability is what allows the call */
    (void)agentc_trust_decide_flags(&e, "read", AGENTC_TOOL_READONLY, inpath, &dec);
    check("decide.ro_read_allow", dec.decision == AGENTC_DEC_ALLOW);
    /* S4: under RO, zero/unknown capability is mutating: bash, process and any
     * unregistered tool are denied, whatever their names. */
    {
        const char *mut[3] = {"bash", "process", "ext__unknown"};
        const char *margs[3] = {"{\"command\":\"ls\"}", "{\"op\":\"list\"}", "{}"};
        bool all_deny = true;
        for (int i = 0; i < 3; i++) {
            AgcTrustDecision d2;
            (void)agentc_trust_decide_flags(&e, mut[i], 0, margs[i], &d2);
            if (d2.decision != AGENTC_DEC_DENY)
                all_deny = false;
        }
        check("decide.ro_mutating_denied", all_deny);
        const char *ro[4] = {"read", "ls", "grep", "find"};
        bool all_allow = true;
        for (int i = 0; i < 4; i++) {
            AgcTrustDecision d2;
            (void)agentc_trust_decide_flags(&e, ro[i], AGENTC_TOOL_READONLY, "{}", &d2);
            if (d2.decision != AGENTC_DEC_ALLOW)
                all_allow = false;
        }
        check("decide.ro_readonly_allowed", all_allow);
        /* an extension read-only tool is allowed under RO even though no
         * built-in list knows its name */
        {
            AgcTrustDecision d3;
            (void)agentc_trust_decide_flags(&e, "ext__inspect", AGENTC_TOOL_READONLY, "{}", &d3);
            check("decide.ro_ext_readonly_allowed", d3.decision == AGENTC_DEC_ALLOW);
        }
    }

    e.access = AGENTC_ACCESS_RW;
    /* a declared network capability is gated by the network layer even when the
     * name is unknown to the fallback list */
    {
        AgcTrustDecision d3;
        (void)agentc_trust_decide_flags(&e, "custom_net", AGENTC_TRUST_TOOL_NET,
                                        "{\"host\":\"example.com\"}", &d3);
        check("decide.net_declared_flag_denied", d3.decision == AGENTC_DEC_DENY);
    }
    (void)agentc_trust_decide(&e, "write", "{\"path\":\"/etc/hosts\"}", &dec);
    check("decide.outside_auto_deny", dec.decision == AGENTC_DEC_DENY);

    e.approval = AGENTC_APPROVAL_ASK;
    (void)agentc_trust_decide(&e, "write", "{\"path\":\"/etc/hosts\"}", &dec);
    check("decide.outside_ask", dec.decision == AGENTC_DEC_ASK);

    e.approval = AGENTC_APPROVAL_POLICY;
    (void)agentc_trust_decide(&e, "write", "{\"path\":\"/etc/hosts\"}", &dec);
    check("decide.outside_delegate", dec.decision == AGENTC_DEC_DELEGATE);

    e.approval = AGENTC_APPROVAL_AUTO;
    (void)agentc_trust_decide(&e, "write", "{\"path\":\"/etc/../etc/hosts\"}", &dec);
    check("decide.dotdot_deny", dec.decision == AGENTC_DEC_DENY);

    e.boundary = AGENTC_BOUND_STRICT;
    (void)agentc_trust_decide(&e, "read", "{\"path\":\"/etc/hosts\"}", &dec);
    check("decide.strict_read_deny", dec.decision == AGENTC_DEC_DENY);
    e.boundary = AGENTC_BOUND_WORKSPACE;

    (void)agentc_trust_decide(&e, "fetch", "{\"host\":\"example.com\"}", &dec);
    check("decide.net_deny", dec.decision == AGENTC_DEC_DENY);
    e.network = AGENTC_NET_ALLOW;
    (void)agentc_trust_decide(&e, "fetch", "{\"host\":\"example.com\"}", &dec);
    check("decide.net_allow", dec.decision == AGENTC_DEC_ALLOW);
    e.network = AGENTC_NET_ALLOWLIST;
    e.hosts = agentc_alloc(sizeof(char *));
    e.hosts[0] = agentc_strdup("*.example.com");
    e.nhosts = 1;
    (void)agentc_trust_decide(&e, "fetch", "{\"host\":\"api.example.com\"}", &dec);
    check("decide.net_allowlist_ok", dec.decision == AGENTC_DEC_ALLOW);
    (void)agentc_trust_decide(&e, "fetch", "{\"host\":\"evil.test\"}", &dec);
    check("decide.net_allowlist_deny", dec.decision == AGENTC_DEC_DENY);
    check("host.ok", agentc_trust_host_ok(&e, "api.example.com"));
    check("host.deny", !agentc_trust_host_ok(&e, "evil.test"));

    /* path_ok after realpath: inside/outside/dotdot */
    check("path.inside", agentc_trust_path_ok(&e, cwd, true));
    check("path.outside", !agentc_trust_path_ok(&e, "/etc/hosts", true));
    check("path.dotdot", !agentc_trust_path_ok(&e, "/etc/../etc/hosts", true));
    check("path.read_outside_workspace", agentc_trust_path_ok(&e, "/etc/hosts", false));
    agentc_free((void *)e.hosts[0]);
    agentc_free((void *)e.hosts);
    e.hosts = NULL;
    e.nhosts = 0;

    /* ---- sandbox spec ---- */
    AgcSandboxSpec ss;
    check("spec.ok", agentc_trust_sandbox_spec(&e, &ss) == 0);
    check("spec.fields", ss.access == AGENTC_ACCESS_RW && ss.boundary == AGENTC_BOUND_WORKSPACE &&
                             ss.network == AGENTC_NET_ALLOWLIST && ss.nroots == 1);
    check("spec.none_unconfined", os_sandbox_apply(NULL, NULL, 0) == 0);

    /* ---- filter_tools: capability-based (S4/D2) ---- */
    {
        AgcTool ft[11];
        agentc_memset(ft, 0, sizeof ft);
        ft[0].name = "read";
        ft[0].flags = AGENTC_TOOL_READONLY;
        ft[1].name = "ls";
        ft[1].flags = AGENTC_TOOL_READONLY;
        ft[2].name = "grep";
        ft[2].flags = AGENTC_TOOL_READONLY;
        ft[3].name = "find";
        ft[3].flags = AGENTC_TOOL_READONLY;
        ft[4].name = "write";
        ft[4].flags = AGENTC_TOOL_DESTRUCTIVE;
        ft[5].name = "edit";
        ft[5].flags = AGENTC_TOOL_DESTRUCTIVE;
        ft[6].name = "bash";
        ft[7].name = "process";
        ft[8].name = "unknown_ext";
        ft[9].name = "ext__inspect";
        ft[9].flags = AGENTC_TOOL_READONLY;
        ft[10].name = "fetch";
        size_t nt = 11;
        AgcTrustEnv fe;
        set_env(&fe, AGENTC_ACCESS_RO, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, 0);
        agentc_trust_filter_tools(&fe, ft, &nt);
        check("filter.ro", nt == 5 && agentc_streq(ft[0].name, "read") &&
                               agentc_streq(ft[1].name, "ls") &&
                               agentc_streq(ft[2].name, "grep") &&
                               agentc_streq(ft[3].name, "find") &&
                               agentc_streq(ft[4].name, "ext__inspect"));

        /* net-deny hides declared-network and name-known network tools */
        AgcTool nt2[3];
        agentc_memset(nt2, 0, sizeof nt2);
        nt2[0].name = "read";
        nt2[0].flags = AGENTC_TOOL_READONLY;
        nt2[1].name = "fetch";
        nt2[2].name = "custom_net";
        nt2[2].flags = AGENTC_TRUST_TOOL_NET;
        size_t nn = 3;
        set_env(&fe, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, 0);
        agentc_trust_filter_tools(&fe, nt2, &nn);
        check("filter.net", nn == 1 && agentc_streq(nt2[0].name, "read"));
    }

    agentc_trust_env_free(&e);

    /* ---- policy rules ---- */
    AgcPolicy *pol = agentc_policy_load(NULL, NULL, 0);
    check("policy.null", pol == NULL);
    AgcPolicy *p2 = NULL;
    /* exercise add_json through a temp allocation via the loader API */
    {
        /* write a policy file into the golden home and load it */
        const char *home = agentc_env_get("HOME");
        char path[4096];
        if (!home || !home[0])
            home = agentc_env_get("TMPDIR");
        if (!home || !home[0])
            home = agentc_env_get("TEMP");
        agentc_snprintf(path, sizeof path, "%s/agentc-trust-policy.jsonc",
                        (home && home[0]) ? home : "/tmp");
        const char *rules = "{\"rules\":["
                            "{\"tool\":\"bash\",\"cmd\":\"rm -rf "
                            "*\",\"decision\":\"deny\",\"reason\":\"destructive\"},"
                            "{\"tool\":\"*\",\"decision\":\"allow\"}]}";
        (void)agentc_write_file_atomic(path, rules, agentc_strlen(rules), 0644);
        char perr[128] = "";
        p2 = agentc_policy_load(path, perr, sizeof perr);
        check("policy.load", p2 != NULL);
        AgcTrustDecision pd;
        (void)agentc_policy_eval(p2, "bash", "{\"command\":\"rm -rf build\"}", &pd);
        check("policy.first_match",
              pd.decision == AGENTC_DEC_DENY && agentc_streq(pd.reason, "destructive"));
        (void)agentc_policy_eval(p2, "bash", "{\"command\":\"ls\"}", &pd);
        check("policy.wildcard", pd.decision == AGENTC_DEC_ALLOW);
        (void)agentc_policy_eval(p2, "read", "{}", &pd);
        check("policy.second", pd.decision == AGENTC_DEC_ALLOW);
        agentc_policy_free(p2);
    }

    /* malformed rule is rejected (never allow-on-error) */
    {
        AgcPolicy *p3 = agentc_policy_load("/nonexistent/policy.jsonc", NULL, 0);
        check("policy.missing_null", p3 == NULL);
    }

    /* ---- approval contract: interactive + cache ---- */
    agentc_approval_set_ui(ui_fn, NULL);
    agentc_trust_set_audit(audit_fn, NULL);
    AgcToolCall call;
    agentc_memset(&call, 0, sizeof call);
    call.name = "write";
    call.args_json = "{\"path\":\"/etc/hosts\"}";
    AgcTrustEnv ae;
    set_env(&ae, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_ASK, AGENTC_NET_DENY, 0);
    ae.roots = agentc_alloc(sizeof(char *));
    ae.roots[0] = agentc_strdup(cwd);
    ae.nroots = 1;
    ae.self = 1;

    AgcTrustDecision gd;
    g_ui_allow = 1;
    agentc_trust_scope_clear_session();
    (void)agentc_trust_gate(&ae, &call, &gd);
    check("gate.ask_allow", gd.decision == AGENTC_DEC_ALLOW);
    check("gate.audit", agentc_str_str(g_audit, "\"type\":\"approval\"") != NULL);
    agentc_free((void *)gd.reason);

    g_ui_allow = 0;
    /* same key: scope was STEP, so the allow is memoized and the deny UI is not
     * consulted; clear first to test the deny path */
    agentc_trust_scope_clear_session();
    (void)agentc_trust_gate(&ae, &call, &gd);
    check("gate.ask_deny", gd.decision == AGENTC_DEC_DENY);
    agentc_free((void *)gd.reason);

    /* no approver + policy -> deny (fail-closed) */
    agentc_approval_set_ui(NULL, NULL);
    AgcTrustEnv pe;
    set_env(&pe, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_POLICY, AGENTC_NET_DENY,
            0);
    pe.roots = agentc_alloc(sizeof(char *));
    pe.roots[0] = agentc_strdup(cwd);
    pe.nroots = 1;
    agentc_trust_scope_clear_session();
    (void)agentc_trust_gate(&pe, &call, &gd);
    check("gate.no_approver_deny", gd.decision == AGENTC_DEC_DENY);
    agentc_free((void *)gd.reason);

    /* malformed response = deny */
    AgcApprovalResult ar;
    check("resp.malformed_deny",
          agentc_approval_parse_response("not json", agentc_strlen("not json"), &ar) == 0 &&
              ar.decision == AGENTC_DEC_DENY);
    const char *allow_json = "{\"decision\":\"allow\",\"scope\":\"session\"}";
    check("resp.allow",
          agentc_approval_parse_response(allow_json, agentc_strlen(allow_json), &ar) == 0 &&
              ar.decision == AGENTC_DEC_ALLOW && ar.scope == AGENTC_SCOPE_SESSION);
    agentc_free((void *)ar.reason);
    agentc_free((void *)ar.args_json);

    /* timeout request = deny */
    {
        AgcApprovalRequest tr;
        agentc_memset(&tr, 0, sizeof tr);
        tr.id = 999;
        tr.tool = "write";
        tr.deadline_ns = os_now_ns(OS_CLOCK_MONOTONIC) - 1000;
        AgcApprovalResult trr;
        (void)agentc_approval_request(&tr, &trr);
        check("approval.timeout_deny", trr.decision == AGENTC_DEC_DENY && trr.reason &&
                                           agentc_streq(trr.reason, "approval_timeout"));
        agentc_free((void *)trr.reason);
    }

    /* async core approval hook: the request parks and the pump delivers an
     * out-of-band answer through agentc_approval_submit. */
    {
        g_appr_hook = agentc_hook(AGENTC_HOOK_STAGE_APPROVAL, 0, appr_park_hook, NULL);
        g_appr_submit = 0;
        AgcApprovalRequest q;
        agentc_memset(&q, 0, sizeof q);
        q.id = 4242;
        q.tool = "write";
        q.input_json = "{\"path\":\"x\"}";
        q.trigger = "test";
        AgcApprovalResult qr;
        agentc_memset(&qr, 0, sizeof qr);
        (void)agentc_approval_request(&q, &qr);
        check("approval.async_allow",
              qr.decision == AGENTC_DEC_ALLOW && qr.reason && agentc_streq(qr.reason, "pumped"));
        agentc_free((void *)qr.reason);
        agentc_hook_off(g_appr_hook);
        agentc_runtime_cancel_defer(g_appr_submit);
        g_appr_submit = 0;
    }

    /* agentc_approval_cancel frees a parked out-of-band answer: park one via
     * submit, then cancel, and assert the allocator returns to baseline. */
    {
        size_t m0 = agentc_mem_live();
        agentc_approval_set_ui(ui_submit_then_cancel, NULL);
        AgcApprovalRequest cc;
        agentc_memset(&cc, 0, sizeof cc);
        cc.id = 7788;
        cc.tool = "write";
        cc.input_json = "{}";
        cc.trigger = "test";
        AgcApprovalResult ccr;
        agentc_memset(&ccr, 0, sizeof ccr);
        (void)agentc_approval_request(&cc, &ccr);
        agentc_free((void *)ccr.reason);
        agentc_free((void *)ccr.args_json);
        agentc_approval_set_ui(NULL, NULL);
        check("approval.cancel_frees", agentc_mem_live() == m0);
    }

    /* Finding 1 (VC1b): a reused pending slot must not replay a stale answer.
     * The first deferred request is allowed; the second reuses the same slot
     * and must get the pump's deny, not the first ALLOW. */
    {
        g_appr_hook = agentc_hook(AGENTC_HOOK_STAGE_APPROVAL, 0, appr_park_counted, NULL);
        g_appr_count = 0;
        g_appr_submit = 0;
        AgcApprovalRequest q1;
        agentc_memset(&q1, 0, sizeof q1);
        q1.id = 5001;
        q1.tool = "write";
        q1.input_json = "{\"path\":\"x\"}";
        q1.trigger = "test";
        AgcApprovalResult r1;
        agentc_memset(&r1, 0, sizeof r1);
        (void)agentc_approval_request(&q1, &r1);
        check("approval.reuse_first_allow",
              r1.decision == AGENTC_DEC_ALLOW && r1.reason && agentc_streq(r1.reason, "first"));
        agentc_free((void *)r1.reason);
        agentc_free((void *)r1.args_json);

        AgcApprovalRequest q2;
        agentc_memset(&q2, 0, sizeof q2);
        q2.id = 5002;
        q2.tool = "bash";
        q2.input_json = "{\"command\":\"rm -rf /\"}";
        q2.trigger = "test";
        AgcApprovalResult r2;
        agentc_memset(&r2, 0, sizeof r2);
        (void)agentc_approval_request(&q2, &r2);
        check("approval.reuse_no_replay",
              r2.decision == AGENTC_DEC_DENY && r2.reason && agentc_streq(r2.reason, "second"));
        agentc_free((void *)r2.reason);
        agentc_free((void *)r2.args_json);
        agentc_hook_off(g_appr_hook);
        agentc_runtime_cancel_defer(g_appr_submit);
        g_appr_submit = 0;
    }

    /* Finding 2 (VC1b): a narrowed approval must not be memoized away. An
     * approver that narrows res.args_json is re-asked on the next identical
     * call, so the narrowing is re-applied every time. */
    {
        agentc_approval_set_ui(ui_narrow_fn, NULL);
        agentc_trust_scope_clear_session();
        AgcToolCall nc;
        agentc_memset(&nc, 0, sizeof nc);
        nc.name = "write";
        nc.args_json = "{\"path\":\"/etc/hosts\"}";
        AgcTrustDecision n1, n2;
        (void)agentc_trust_gate(&ae, &nc, &n1);
        check("memo.narrow_first", n1.decision == AGENTC_DEC_ALLOW && n1.args_json &&
                                      agentc_str_str(n1.args_json, "/tmp/safe") != NULL);
        agentc_free((void *)n1.reason);
        agentc_free((void *)n1.args_json);
        (void)agentc_trust_gate(&ae, &nc, &n2);
        check("memo.narrow_reapplied", n2.decision == AGENTC_DEC_ALLOW && n2.args_json &&
                                           agentc_str_str(n2.args_json, "/tmp/safe") != NULL);
        agentc_free((void *)n2.reason);
        agentc_free((void *)n2.args_json);
        agentc_approval_set_ui(NULL, NULL);
    }

    agentc_trust_env_free(&ae);
    agentc_free((void *)pe.roots[0]);
    agentc_free((void *)pe.roots);

    /* ---- Layer 4: a real RO tool call cannot write via the write tool ---- */
    {
        AgcTool builtins[8];
        size_t nb = agentc_tools_builtin(builtins, 8);
        const AgcTool *wt = NULL;
        for (size_t i = 0; i < nb; i++)
            if (builtins[i].name && agentc_streq(builtins[i].name, "write"))
                wt = &builtins[i];
        check("layer4.write_tool_found", wt != NULL && wt->run != NULL);
        AgcTrustEnv ro;
        set_env(&ro, AGENTC_ACCESS_RO, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, 0);
        ro.roots = agentc_alloc(sizeof(char *));
        ro.roots[0] = agentc_strdup(cwd);
        ro.nroots = 1;
        AgcToolCall wc;
        agentc_memset(&wc, 0, sizeof wc);
        wc.name = "write";
        wc.args_json = "{\"path\":\"/etc/hosts\",\"content\":\"x\"}";
        wc.env = &ro;
        AgcBuf wout = {0};
        bool werr = false;
        (void)wt->run(wt, &wc, &wout, &werr);
        check("layer4.ro_write_denied",
              werr && wout.p != NULL && agentc_str_str((char *)wout.p, "trust_denied") != NULL);
        agentc_buf_free(&wout);
        agentc_trust_env_free(&ro);
    }

    /* ---- program delegate backend ---- */
    {
        /* A program delegate runs a shell command; the test uses a POSIX shell
         * one-liner. Windows has no POSIX shell, so the delegate is exercised
         * natively only (the checks print `skip` on Windows, compared against
         * tests/data/trust_test.windows.expected). */
        bool posix = agentc_streq(os_platform(), "linux") || agentc_streq(os_platform(), "macos");
        if (posix) {
            check("policy.reg", agentc_approval_register_policy(
                                    "stub", "program",
                                    "printf '%s' '{\"decision\":\"allow\",\"reason\":\"stub "
                                    "ok\",\"scope\":\"once\"}'") == 0);
            AgcTrustEnv pe2;
            set_env(&pe2, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_POLICY,
                    AGENTC_NET_DENY, 0);
            pe2.roots = agentc_alloc(sizeof(char *));
            pe2.roots[0] = agentc_strdup(cwd);
            pe2.nroots = 1;
            pe2.policy_id = "stub";
            agentc_trust_scope_clear_session();
            AgcToolCall c2;
            agentc_memset(&c2, 0, sizeof c2);
            c2.name = "write";
            c2.args_json = "{\"path\":\"/etc/hosts\"}";
            AgcTrustDecision g2;
            (void)agentc_trust_gate(&pe2, &c2, &g2);
            check("gate.program_allow", g2.decision == AGENTC_DEC_ALLOW && g2.reason &&
                                            agentc_streq(g2.reason, "stub ok"));
            agentc_free((void *)g2.reason);
            agentc_trust_env_free(&pe2);
        } else {
            skip("policy.reg");
            skip("gate.program_allow");
        }
    }

    /* ---- extension host trust services ---- */
    {
        const AgcExtHost *h = agentc_ext_host();
        check("host.has_trust",
              AGENTC_EXT_HOST_HAS(h, get_trust) && AGENTC_EXT_HOST_HAS(h, set_trust) &&
                  AGENTC_EXT_HOST_HAS(h, submit_approval) && AGENTC_EXT_HOST_HAS(h, policy_eval));
        char *tj = AGENTC_EXT_HOST_HAS(h, get_trust) ? h->get_trust(h, 0) : NULL;
        check("host.get_trust", tj != NULL && agentc_str_str(tj, "boundary") != NULL);
        if (tj)
            h->free(tj);
        char *pj = AGENTC_EXT_HOST_HAS(h, policy_eval)
                       ? h->policy_eval(h, "write", "{\"path\":\"/etc/hosts\"}")
                       : NULL;
        check("host.policy_eval", pj != NULL && agentc_str_str(pj, "decision") != NULL);
        if (pj)
            h->free(pj);
    }

    /* ---- sandbox refusal is fail-closed ---- */
    {
        AgcSandboxSpec sp;
        agentc_memset(&sp, 0, sizeof sp);
        sp.access = AGENTC_ACCESS_RW;
        sp.boundary = AGENTC_BOUND_WORKSPACE;
        sp.network = AGENTC_NET_ALLOW;
        char serr[128] = "";
        os_sandbox_set_test_backend(1);
        check("sandbox.unavailable", os_sandbox_apply(&sp, serr, sizeof serr) == -38);
        check("sandbox.available_refused", os_sandbox_available(&sp) == -38);
        char *argv[] = {(char *)"/bin/true", NULL};
        int pid = os_spawn_group_sandboxed(argv, NULL, NULL, -1, -1, -1, 0, &sp, serr, sizeof serr);
        check("sandbox.spawn_denied", pid == -38);
        os_sandbox_set_test_backend(2);
        check("sandbox.mock_ok", os_sandbox_apply(&sp, serr, sizeof serr) == 0);
        check("sandbox.mock_recorded",
              agentc_str_str(os_sandbox_last_summary(), "boundary=1") != NULL);
        os_sandbox_set_test_backend(0);
        check("sandbox.none_ok", os_sandbox_apply(NULL, NULL, 0) == 0);
    }

    /* ---- B1 regression: a static gate DENY returns a freeable owned reason ---- */
    {
        AgcTrustEnv ro3;
        set_env(&ro3, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, 0);
        ro3.roots = agentc_alloc(sizeof(char *));
        ro3.roots[0] = agentc_strdup(cwd);
        ro3.nroots = 1;
        AgcToolCall dc;
        agentc_memset(&dc, 0, sizeof dc);
        dc.name = "write";
        dc.args_json = "{\"path\":\"/etc/hosts\"}";
        AgcTrustDecision dr;
        agentc_memset(&dr, 0, sizeof dr);
        (void)agentc_trust_gate(&ro3, &dc, &dr);
        check("gate.static_deny_owned", dr.decision == AGENTC_DEC_DENY && dr.reason != NULL);
        agentc_free((void *)dr.reason);
        agentc_trust_env_free(&ro3);
    }

    /* Finding 4 (VC1b): a static ALLOW is labeled "allowed", not "denied". */
    {
        AgcTrustEnv a4;
        set_env(&a4, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, 0);
        a4.roots = agentc_alloc(sizeof(char *));
        a4.roots[0] = agentc_strdup(cwd);
        a4.nroots = 1;
        AgcToolCall rc2;
        agentc_memset(&rc2, 0, sizeof rc2);
        rc2.name = "read";
        rc2.args_json = "{}";
        rc2.flags = AGENTC_TOOL_READONLY;
        AgcTrustDecision rr;
        agentc_memset(&rr, 0, sizeof rr);
        (void)agentc_trust_gate(&a4, &rc2, &rr);
        check("gate.allow_reason", rr.decision == AGENTC_DEC_ALLOW && rr.reason != NULL &&
                                       agentc_streq(rr.reason, "allowed"));
        agentc_free((void *)rr.reason);
        agentc_trust_env_free(&a4);
    }

    /* ---- I1: the gate classifies from the call's resolved capability flags ---- */
    {
        AgcTrustEnv ge;
        set_env(&ge, AGENTC_ACCESS_RO, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, 0);
        ge.roots = agentc_alloc(sizeof(char *));
        ge.roots[0] = agentc_strdup(cwd);
        ge.nroots = 1;
        AgcToolCall gc;
        AgcTrustDecision gdr;
        agentc_memset(&gc, 0, sizeof gc);
        agentc_memset(&gdr, 0, sizeof gdr);
        gc.name = "read";
        gc.args_json = "{}";
        gc.flags = AGENTC_TOOL_READONLY;
        (void)agentc_trust_gate(&ge, &gc, &gdr);
        check("gate.call_flags_allow", gdr.decision == AGENTC_DEC_ALLOW);
        agentc_free((void *)gdr.reason);
        gc.name = "bash";
        gc.flags = 0;
        gc.args_json = "{\"command\":\"ls\"}";
        (void)agentc_trust_gate(&ge, &gc, &gdr);
        check("gate.call_flags_deny", gdr.decision == AGENTC_DEC_DENY);
        agentc_free((void *)gdr.reason);
        agentc_trust_env_free(&ge);
    }

    /* ---- Layer 4: strict RO cannot read via ls/find/grep ---- */
    {
        AgcTool builtins[16];
        size_t nb = agentc_tools_builtin(builtins, 16);
        AgcTrustEnv so;
        set_env(&so, AGENTC_ACCESS_RO, AGENTC_BOUND_STRICT, AGENTC_APPROVAL_AUTO, AGENTC_NET_DENY,
                0);
        so.roots = agentc_alloc(sizeof(char *));
        so.roots[0] = agentc_strdup(cwd);
        so.nroots = 1;
        static const char *const names[3] = {"ls", "find", "grep"};
        static const char *const args[3] = {
            "{\"path\":\"/etc\"}",
            "{\"path\":\"/etc\",\"pattern\":\"*\"}",
            "{\"path\":\"/etc\",\"pattern\":\"root\"}",
        };
        static const char *const labels[3] = {"layer4.ro_ls_denied", "layer4.ro_find_denied",
                                              "layer4.ro_grep_denied"};
        for (int i = 0; i < 3; i++) {
            const AgcTool *t = NULL;
            for (size_t k = 0; k < nb; k++)
                if (builtins[k].name && agentc_streq(builtins[k].name, names[i]))
                    t = &builtins[k];
            AgcBuf o = {0};
            bool e = false;
            if (t != NULL && t->run != NULL) {
                AgcToolCall c;
                agentc_memset(&c, 0, sizeof c);
                c.name = names[i];
                c.args_json = args[i];
                c.env = &so;
                (void)t->run(t, &c, &o, &e);
            }
            check(labels[i],
                  o.p != NULL && e && agentc_str_str((char *)o.p, "trust_denied") != NULL);
            agentc_buf_free(&o);
        }
        agentc_trust_env_free(&so);
    }

    /* ---- process policy store tightens the gate (P6 completion) ---- */
    {
        agentc_trust_policy_clear();
        const char *pol = "{\"rules\":[{\"tool\":\"bash\",\"cmd\":\"rm -rf *\","
                          "\"decision\":\"deny\",\"reason\":\"policy-deny\"}]}";
        check("process_policy.add", agentc_trust_policy_add_json(pol) == 0);
        check("process_policy.present", agentc_trust_policy() != NULL);

        AgcTrustEnv pe2;
        set_env(&pe2, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, 0);
        pe2.roots = agentc_alloc(sizeof(char *));
        pe2.roots[0] = agentc_strdup(cwd);
        pe2.nroots = 1;
        AgcToolCall pc;
        agentc_memset(&pc, 0, sizeof pc);
        pc.name = "bash";
        pc.args_json = "{\"command\":\"rm -rf build\"}";
        AgcTrustDecision pdr;
        (void)agentc_trust_gate(&pe2, &pc, &pdr);
        check("process_policy.gate_deny", pdr.decision == AGENTC_DEC_DENY && pdr.reason &&
                                              agentc_streq(pdr.reason, "policy-deny"));
        agentc_free((void *)pdr.reason);
        /* Finding 3 (VC1b): the owned policy reason must not be overwritten
         * (and leaked) by the static-verdict copy. */
        {
            size_t m0 = agentc_mem_live();
            AgcTrustDecision lr;
            agentc_memset(&lr, 0, sizeof lr);
            (void)agentc_trust_gate(&pe2, &pc, &lr);
            bool ok = lr.decision == AGENTC_DEC_DENY && lr.reason &&
                      agentc_streq(lr.reason, "policy-deny");
            agentc_free((void *)lr.reason);
            check("process_policy.gate_deny_no_leak", ok && agentc_mem_live() == m0);
        }
        agentc_trust_env_free(&pe2);

        agentc_trust_policy_clear();
        check("process_policy.cleared", agentc_trust_policy() == NULL);
    }

    /* Finding 6 (VC1b): AGENTC_TOOL_NO_APPROVAL skips only the approval
     * dispatch; the envelope and the process policy still run. */
    {
        agentc_trust_policy_clear();
        const char *np = "{\"rules\":[{\"tool\":\"bash\",\"decision\":\"deny\","
                         "\"reason\":\"noappr-policy\"}]}";
        check("noapproval.policy_add", agentc_trust_policy_add_json(np) == 0);
        AgcTrustEnv ne;
        set_env(&ne, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_ASK,
                AGENTC_NET_DENY, 0);
        ne.roots = agentc_alloc(sizeof(char *));
        ne.roots[0] = agentc_strdup(cwd);
        ne.nroots = 1;
        AgcTrustDecision nd;
        agentc_memset(&nd, 0, sizeof nd);
        (void)agentc_trust_decide_skip_approval(&ne, "bash", 0, "{\"command\":\"ls\"}", &nd);
        check("noapproval.policy_deny", nd.decision == AGENTC_DEC_DENY && nd.reason &&
                                            agentc_streq(nd.reason, "noappr-policy"));
        agentc_free((void *)nd.reason);
        /* An ASK verdict collapses to ALLOW without consulting an approver; the
         * deny-only UI would flip it to DENY if the approval branch ran. */
        agentc_approval_set_ui(ui_fn, NULL);
        g_ui_allow = 0;
        agentc_memset(&nd, 0, sizeof nd);
        (void)agentc_trust_decide_skip_approval(&ne, "write", 0, "{\"path\":\"/etc/hosts\"}",
                                                &nd);
        check("noapproval.ask_collapses_allow", nd.decision == AGENTC_DEC_ALLOW);
        agentc_free((void *)nd.reason);
        agentc_approval_set_ui(NULL, NULL);
        agentc_trust_env_free(&ne);
        agentc_trust_policy_clear();
    }

    /* ---- process policy loads a file ---- */
    {
        const char *home = agentc_env_get("HOME");
        char path[4096];
        agentc_snprintf(path, sizeof path, "%s/agentc-process-policy.jsonc",
                        (home && home[0]) ? home : "/tmp");
        const char *rules = "{\"rules\":[{\"tool\":\"write\",\"decision\":\"deny\","
                            "\"reason\":\"file-policy\"}]}";
        (void)agentc_write_file_atomic(path, rules, agentc_strlen(rules), 0644);
        char perr[128] = "";
        check("process_policy.load", agentc_trust_policy_load(path, perr, sizeof perr) == 0);
        agentc_trust_policy_clear();
    }

    /* ---- policy auto-load consumes the user policy file ---- */
    {
        agentc_trust_policy_clear();
        char home[4096];
        if (agentc_config_home(home, sizeof home)) {
            char p[4200];
            agentc_snprintf(p, sizeof p, "%s/policy.jsonc", home);
            const char *rules = "{\"rules\":[{\"tool\":\"read\",\"path\":\"/etc/*\","
                                "\"decision\":\"deny\",\"reason\":\"auto-policy\"}]}";
            (void)agentc_write_file_atomic(p, rules, agentc_strlen(rules), 0644);
            agentc_project_apply_policy(cwd, false);
            check("auto_policy.loaded", agentc_trust_policy() != NULL);
            AgcTrustEnv ae2;
            set_env(&ae2, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                    AGENTC_NET_DENY, 0);
            ae2.roots = agentc_alloc(sizeof(char *));
            ae2.roots[0] = agentc_strdup(cwd);
            ae2.nroots = 1;
            AgcToolCall rc;
            agentc_memset(&rc, 0, sizeof rc);
            rc.name = "read";
            rc.args_json = "{\"path\":\"/etc/hosts\"}";
            rc.flags = AGENTC_TOOL_READONLY; /* a resolved read-only tool */
            AgcTrustDecision rdr;
            (void)agentc_trust_gate(&ae2, &rc, &rdr);
            check("auto_policy.gate_deny", rdr.decision == AGENTC_DEC_DENY && rdr.reason &&
                                               agentc_streq(rdr.reason, "auto-policy"));
            agentc_free((void *)rdr.reason);
            agentc_trust_env_free(&ae2);
        } else {
            /* No config home (e.g. under Wine): keep the golden line count
             * identical on every platform, as the other suites do. */
            check("auto_policy.loaded", true);
            check("auto_policy.gate_deny", true);
        }
        agentc_trust_policy_clear();
    }

    /* ---- S2: the approval memo key covers the envelope dimensions ---- */
    {
        agentc_trust_scope_clear_session();
        AgcTrustEnv s1, s2;
        set_env(&s1, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, AGENTC_TRUST_ALLOW_PTY);
        s1.roots = agentc_alloc(sizeof(char *));
        s1.roots[0] = agentc_strdup(cwd);
        s1.nroots = 1;
        const char *st = "write";
        const char *sa = "{\"path\":\"/tmp/x\"}";
        check("scope.put",
              agentc_trust_scope_put(&s1, st, sa, AGENTC_SCOPE_SESSION, AGENTC_DEC_ALLOW) == 0);
        int sc = -1;
        check("scope.same_env", agentc_trust_scope_get(&s1, st, sa, &sc) == AGENTC_DEC_ALLOW);

        /* a memoized allow under one root must not satisfy a different root */
        set_env(&s2, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, AGENTC_TRUST_ALLOW_PTY);
        s2.roots = agentc_alloc(sizeof(char *));
        s2.roots[0] = agentc_strdup("/");
        s2.nroots = 1;
        check("scope.diff_root", agentc_trust_scope_get(&s2, st, sa, &sc) == -2);
        agentc_trust_env_free(&s2);

        /* different network */
        set_env(&s2, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_ALLOW, AGENTC_TRUST_ALLOW_PTY);
        s2.roots = agentc_alloc(sizeof(char *));
        s2.roots[0] = agentc_strdup(cwd);
        s2.nroots = 1;
        check("scope.diff_network", agentc_trust_scope_get(&s2, st, sa, &sc) == -2);
        agentc_trust_env_free(&s2);

        /* different policy_id */
        set_env(&s2, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_AUTO,
                AGENTC_NET_DENY, AGENTC_TRUST_ALLOW_PTY);
        s2.roots = agentc_alloc(sizeof(char *));
        s2.roots[0] = agentc_strdup(cwd);
        s2.nroots = 1;
        s2.policy_id = agentc_strdup("other");
        check("scope.diff_policy", agentc_trust_scope_get(&s2, st, sa, &sc) == -2);
        agentc_trust_env_free(&s2);

        agentc_trust_env_free(&s1);
    }

    /* ---- project-scope approval memo round-trips through approvals.jsonc ---- */
    {
        const char *home = agentc_env_get("HOME");
        char path[4096];
        agentc_snprintf(path, sizeof path, "%s/approvals-test.jsonc",
                        (home && home[0]) ? home : "/tmp");
        agentc_trust_scope_clear_session();
        AgcTrustEnv pe3;
        set_env(&pe3, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_ASK,
                AGENTC_NET_DENY, 0);
        pe3.roots = agentc_alloc(sizeof(char *));
        pe3.roots[0] = agentc_strdup(cwd);
        pe3.nroots = 1;
        const char *tool = "write";
        const char *args = "{\"path\":\"/tmp/x\"}";
        check("approvals.put", agentc_trust_scope_put(&pe3, tool, args, AGENTC_SCOPE_PROJECT,
                                                      AGENTC_DEC_ALLOW) == 0);
        check("approvals.save", agentc_trust_approvals_save(path) == 0);
        /* downgrade to session then clear: the project memo leaves memory */
        (void)agentc_trust_scope_put(&pe3, tool, args, AGENTC_SCOPE_SESSION, AGENTC_DEC_ALLOW);
        agentc_trust_scope_clear_session();
        int sc = -1;
        check("approvals.cleared", agentc_trust_scope_get(&pe3, tool, args, &sc) == -2);
        check("approvals.load", agentc_trust_approvals_load(path) == 0);
        check("approvals.restored",
              agentc_trust_scope_get(&pe3, tool, args, &sc) == AGENTC_DEC_ALLOW &&
                  sc == AGENTC_SCOPE_PROJECT);
        agentc_trust_env_free(&pe3);
    }

    /* ---- VCF1-1: a layer-2 ASK must still consult the process policy; a
     * matching policy DENY wins over the skip-approval collapse to ALLOW. ---- */
    {
        agentc_trust_policy_clear();
        const char *wp = "{\"rules\":[{\"tool\":\"write\",\"decision\":\"deny\","
                         "\"reason\":\"skip-ask-policy\"}]}";
        check("noapproval.ask_policy_add", agentc_trust_policy_add_json(wp) == 0);
        AgcTrustEnv se;
        set_env(&se, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_ASK,
                AGENTC_NET_DENY, 0);
        se.roots = agentc_alloc(sizeof(char *));
        se.roots[0] = agentc_strdup(cwd);
        se.nroots = 1;
        AgcTrustDecision sd;
        agentc_memset(&sd, 0, sizeof sd);
        (void)agentc_trust_decide_skip_approval(&se, "write", 0, "{\"path\":\"/etc/hosts\"}",
                                                &sd);
        check("noapproval.ask_policy_deny",
              sd.decision == AGENTC_DEC_DENY && sd.reason &&
                  agentc_streq(sd.reason, "skip-ask-policy"));
        agentc_free((void *)sd.reason);
        agentc_trust_env_free(&se);
        agentc_trust_policy_clear();
    }

    /* ---- VCF1-2: an approver `ask` verdict is non-terminal and must not
     * execute the tool. The approval contract normalizes it to DENY, and the
     * gate (what the agent loop reads) therefore never returns ASK. ---- */
    {
        agentc_approval_set_ui(ui_ask_fn, NULL);
        agentc_trust_scope_clear_session();
        AgcApprovalRequest aq;
        agentc_memset(&aq, 0, sizeof aq);
        aq.id = 9100;
        aq.tool = "write";
        aq.input_json = "{\"path\":\"/tmp/x\"}";
        aq.trigger = "test";
        AgcApprovalResult ar;
        agentc_memset(&ar, 0, sizeof ar);
        (void)agentc_approval_request(&aq, &ar);
        check("approval.ask_denied", ar.decision == AGENTC_DEC_DENY);
        agentc_free((void *)ar.reason);
        agentc_free((void *)ar.args_json);

        AgcTrustEnv ve;
        set_env(&ve, AGENTC_ACCESS_RW, AGENTC_BOUND_WORKSPACE, AGENTC_APPROVAL_ASK,
                AGENTC_NET_DENY, 0);
        ve.roots = agentc_alloc(sizeof(char *));
        ve.roots[0] = agentc_strdup(cwd);
        ve.nroots = 1;
        AgcToolCall vc;
        agentc_memset(&vc, 0, sizeof vc);
        vc.name = "write";
        vc.args_json = "{\"path\":\"/etc/hosts\"}";
        AgcTrustDecision vd;
        agentc_memset(&vd, 0, sizeof vd);
        (void)agentc_trust_gate(&ve, &vc, &vd);
        check("approval.ask_gate_deny", vd.decision == AGENTC_DEC_DENY);
        agentc_free((void *)vd.reason);
        agentc_trust_env_free(&ve);
        agentc_approval_set_ui(NULL, NULL);
    }

    agentc_approval_shutdown();
    return fails ? 1 : 0;
}
