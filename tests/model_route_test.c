/* model_route_test.c — W4 virtual models / routing.
 *
 * Deterministic golden suite: an injected capturing transport, no processes.
 * It prints one `label=1|0` line per assertion and compares byte-for-byte.
 *
 * Coverage:
 *   - `models.<alias>` config parsing (bounded store, malformed entries dropped)
 *     and publication into the process model registry;
 *   - a virtual alias that does NOT shadow a catalog/dynamic model id;
 *   - per-step resolution to the concrete {provider, model}: the request URL host
 *     and JSON `"model"` field prove the routed target while the transcript keeps
 *     the alias;
 *   - the AGENTC_EV_MODEL_ROUTE event and the persisted `custom type:"model"`
 *     session record;
 *   - a routed request byte-identical to the equivalent non-virtual request;
 *   - a broken virtual target fails closed with no request.
 *
 * The Windows session-persistence check prints `skip` (tests/data/
 * model_route_test.windows.expected). */
#include "agent.h"
#include "config.h"
#include "core/agent_int.h"
#include "core/events.h"
#include "core/runsession.h"
#include "plat.h"
#include "session.h"

static int fails;

/* internal helper from config.c (declared ad hoc, like tests/config_test.c) */
void agentc_test_setenv(const char *name, const char *value);

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

/* Windows persistence is a documented skip; print `skip`, never a hard-coded 1. */
static void check_win(const char *label, bool win, bool ok) {
    if (win)
        agentc_outf("%s=skip\n", label);
    else
        check(label, ok);
}

/* ----------------------------------------------------- capturing transport */
typedef struct {
    int codes[16];
    const char *bodies[16];
    size_t n, i;
    size_t calls;
    char last_url[1200];
    char last_headers[4096];
    char last_body[8192];
    size_t last_body_len;
} Cap;

static int cap_request(void *ud, const char *url, const char *headers, const void *body,
                       size_t body_len, int (*on_chunk)(void *, const void *, size_t), void *u,
                       int timeout_ms, AgcBuf *record) {
    (void)timeout_ms;
    (void)record;
    Cap *c = ud;
    c->calls++;
    if (headers) {
        size_t l = agentc_strlen(headers);
        if (l >= sizeof c->last_headers)
            l = sizeof c->last_headers - 1;
        agentc_memcpy(c->last_headers, headers, l);
        c->last_headers[l] = 0;
    } else {
        c->last_headers[0] = 0;
    }
    if (url) {
        size_t l = agentc_strlen(url);
        if (l >= sizeof c->last_url)
            l = sizeof c->last_url - 1;
        agentc_memcpy(c->last_url, url, l);
        c->last_url[l] = 0;
    } else {
        c->last_url[0] = 0;
    }
    const char *b = (const char *)body;
    size_t bl = b ? body_len : 0;
    if (bl >= sizeof c->last_body)
        bl = sizeof c->last_body - 1;
    if (bl)
        agentc_memcpy(c->last_body, b, bl);
    c->last_body[bl] = 0;
    c->last_body_len = bl;

    if (c->i >= c->n)
        return -5;
    int code = c->codes[c->i];
    const char *resp = c->bodies[c->i];
    c->i++;
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

/* OpenAI Chat Completions stream: proves the effective provider's adapter runs. */
static const char reply_openai_ok[] =
    "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},"
    "\"finish_reason\":null}]}\n\n"
    "data: {\"id\":\"chatcmpl-1\",\"choices\":[{\"index\":0,\"delta\":{},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":9,\"completion_tokens\":2,"
    "\"prompt_tokens_details\":{\"cached_tokens\":1}}}\n\n"
    "data: [DONE]\n\n";

static const char reply_anthropic_ok[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\",\"usage\":{"
    "\"input_tokens\":5,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
    "\"text_delta\",\"text\":\"ok\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":2}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

/* ------------------------------------------------- per-agent route seams */
/* P8-D1 (B-M8): a route-target resolver rides the agent, never a process global.
 * The routed target must use its own endpoint and credential, never the agent's. */
static bool route_target_cb(void *ud, const char *provider, AgcRouteTarget *out) {
    (void)ud;
    if (!agentc_streq(provider, "target-x"))
        return false;
    out->api_key = "route-key-123";
    out->base_url = "https://routed.example/v1";
    return true;
}

/* The same shape with the target's own default host; used where the test only
 * needs the routed step to clear the fail-closed key guard. */
static bool route_target_keyed_cb(void *ud, const char *provider, AgcRouteTarget *out) {
    (void)ud;
    if (!agentc_streq(provider, "target-x"))
        return false;
    out->api_key = "routed-key";
    out->base_url = "https://target.example/v1";
    return true;
}

/* Two independent seams for the seam.per_agent regression (different hosts and
 * credentials in one process). */
static bool route_target_a_cb(void *ud, const char *provider, AgcRouteTarget *out) {
    (void)ud;
    if (!agentc_streq(provider, "target-x"))
        return false;
    out->api_key = "key-A";
    out->base_url = "https://a.example/v1";
    return true;
}

static bool route_target_b_cb(void *ud, const char *provider, AgcRouteTarget *out) {
    (void)ud;
    if (!agentc_streq(provider, "target-x"))
        return false;
    out->api_key = "key-B";
    out->base_url = "https://b.example/v1";
    return true;
}

/* --------------------------------------------------------- route events */
typedef struct {
    int count;
    char virt[64], prov[64], model[64];
} EvCap;

static void copy_bounded(char *dst, size_t cap, const char *src) {
    if (!src)
        src = "";
    size_t n = agentc_strlen(src);
    if (n >= cap)
        n = cap - 1;
    agentc_memcpy(dst, src, n);
    dst[n] = 0;
}

static void ev_cb(void *ud, int ev, const void *data) {
    EvCap *e = ud;
    if (ev != AGENTC_EV_MODEL_ROUTE || data == NULL || e == NULL)
        return;
    const AgcModelRouteEvent *r = data;
    e->count++;
    copy_bounded(e->virt, sizeof e->virt, r->virtual_model);
    copy_bounded(e->prov, sizeof e->prov, r->provider);
    copy_bounded(e->model, sizeof e->model, r->model);
}

static AgcAgent *make_agent(const AgcProvider *prov, const char *model, Cap *cap) {
    AgcAgentSpec s;
    agentc_memset(&s, 0, sizeof s);
    s.provider_handle = prov;
    s.provider = prov ? prov->name : NULL;
    s.model = model;
    s.thinking = -1;
    s.label = "route";
    s.flags = AGENTC_AGENT_F_WAIT;
    AgcTransport t = {cap_request, cap, NULL};
    s.transport = t;
    s.transport_set = true;
    AgcAgent *a = agentc_agent_create(&s);
    if (a)
        agentc_agent_set_system(a, "sys");
    return a;
}

/* ------------------------------------------------------- config parsing */
static void test_config_parse(void) {
    /* Isolate the user config root so the parse is independent of the runner's
     * HOME/XDG (wine-check does not scrub the environment). */
    char root[256];
    agentc_snprintf(root, sizeof root, "/tmp/agentc-model-route-%lld",
                    (long long)os_now_ns(OS_CLOCK_MONOTONIC));
    agentc_test_setenv("XDG_CONFIG_HOME", root);

    char base[4096], path[4300];
    const char *ch = agentc_config_home(base, sizeof base);
    bool have = ch != NULL && agentc_path_join(path, sizeof path, ch, "config.jsonc");
    static const char cfg[] = "{\"models\":{"
                              "\"cfg-fast\":{\"provider\":\"target-x\",\"model\":\"m-target\"},"
                              "\"cfg-bad\":{\"provider\":\"target-x\"},"
                              "\"cfg-num\":123,"
                              "\"cfg-flag\":true}}";
    check("config.write",
          have && agentc_mkdir_parents(path) == 0 &&
              agentc_write_file_atomic(path, cfg, sizeof cfg - 1, 0644) == 0);

    AgcConfig *c = agentc_config_load(NULL);
    AgcConfigModelRoute routes[4];
    size_t n = agentc_config_model_routes(c, routes, 4);
    check("config.routes", c != NULL && n == 1);
    check("config.fields", n == 1 && agentc_streq(routes[0].alias, "cfg-fast") &&
                               agentc_streq(routes[0].provider, "target-x") &&
                               agentc_streq(routes[0].model, "m-target"));
    check("config.registered", agentc_model_find_virtual("cfg-fast") != NULL);
    check("config.absent", agentc_config_model_routes(NULL, NULL, 0) == 0);
    agentc_config_free(c);
}

/* ------------------------------------------------- registry / shadowing */
static void test_registry(void) {
    const AgcProvider *t = agentc_prov_openai_compatible("target-x", "https://target.example/v1");
    check("registry.provider", t != NULL && agentc_streq(t->name, "target-x"));

    agentc_model_register_virtual("fast", "target-x", "m-target");
    const AgcModel *vm = agentc_model_find_virtual("fast");
    check("registry.virtual",
          vm != NULL && (vm->flags & AGENTC_MODEL_VIRTUAL) != 0 && vm->route != NULL &&
              agentc_streq(vm->route, "target-x/m-target"));

    /* A virtual alias is self-describing: it is not a concrete model under the
     * selected provider, so the app auto-pick (main.c) must consult
     * agentc_model_find_virtual to avoid registering a duplicate dynamic row. */
    check("registry.not_concrete", agentc_model_find("anthropic", "fast") == NULL &&
                                       agentc_model_find_virtual("fast") != NULL);

    /* An alias must not shadow a real catalog id (first-wins). */
    agentc_model_register_virtual("claude-sonnet-4-5", "target-x", "m-target");
    check("registry.shadow_refused",
          agentc_model_find_virtual("claude-sonnet-4-5") == NULL &&
              agentc_model_find("anthropic", "claude-sonnet-4-5") != NULL);

    /* Malformed targets are ignored, never half-registered. */
    agentc_model_register_virtual("bad1", "", "m");
    agentc_model_register_virtual("bad2", "target-x", "");
    check("registry.malformed", agentc_model_find_virtual("bad1") == NULL &&
                                    agentc_model_find_virtual("bad2") == NULL);

    /* A provider id may itself contain '/' (the route keeps provider and model
     * separate, so nothing is mis-split); the target fields are exact. */
    agentc_model_register_virtual("slashy", "target-x/evil", "m");
    {
        const AgcModel *sv = agentc_model_find_virtual("slashy");
        check("registry.slash_provider",
              sv != NULL && agentc_streq(sv->route_provider, "target-x/evil") &&
                  agentc_streq(sv->route_model, "m"));
    }

    /* first-wins is symmetric: an alias holds its id, so a later concrete
     * registration must not take it. */
    agentc_model_register_virtual("dup-id", "target-x", "m-target");
    agentc_model_register_dynamic("anthropic", "dup-id", "anthropic-messages",
                                  "https://api.anthropic.com", 0, 0, false, false);
    check("registry.concrete_refused", agentc_model_find("anthropic", "dup-id") == NULL &&
                                           agentc_model_find_virtual("dup-id") != NULL);
}

/* ------------------------------------------- resolve to a different host */
static void test_route_host_and_model(void) {
    (void)agentc_prov_openai_compatible("target-x", "https://target.example/v1");
    agentc_model_register_virtual("fast", "target-x", "m-target");

    Cap cap;
    agentc_memset(&cap, 0, sizeof cap);
    cap.n = 1;
    cap.codes[0] = 0;
    cap.bodies[0] = reply_openai_ok;

    /* The agent is constructed on anthropic but its alias routes to target-x. */
    AgcAgent *a = make_agent(agentc_prov_anthropic(), "fast", &cap);
    /* target-x needs a key; the routed step resolves it through the per-agent
     * seam (P8-D3/B-M6 fails closed otherwise). */
    AgcRouteSeam seam;
    agentc_memset(&seam, 0, sizeof seam);
    seam.target = route_target_keyed_cb;
    agentc_agent_set_route_seam(a, &seam);
    EvCap ev;
    agentc_memset(&ev, 0, sizeof ev);
    agentc_agent_set_events(a, ev_cb, &ev);
    int rc = agentc_agent_submit(a, "hi");
    check("route.submit", rc == 0);
    check("route.calls", cap.calls == 1);
    check("route.url_host", agentc_str_str(cap.last_url, "target.example") != NULL);
    check("route.json_model", agentc_str_str(cap.last_body, "\"model\":\"m-target\"") != NULL);
    check("route.no_alias_leak", agentc_str_str(cap.last_body, "\"model\":\"fast\"") == NULL);
    /* The alias stays the transcript identity even after the routed request. */
    const AgcTranscript *tr = agentc_agent_transcript(a);
    check("route.alias_transcript",
          agentc_streq(agentc_agent_model(a), "fast") && tr != NULL &&
              agentc_streq(tr->model, "fast") && tr->provider != NULL);
    check("route.event", ev.count == 1 && agentc_streq(ev.virt, "fast") &&
                             agentc_streq(ev.prov, "target-x") &&
                             agentc_streq(ev.model, "m-target"));
    agentc_agent_free(a);
    agentc_actor_release_all();
}

/* -------------------------------- routed request == equivalent non-virtual */
static void test_nonvirtual_identical(void) {
    const AgcProvider *t = agentc_prov_openai_compatible("target-x", "https://target.example/v1");
    agentc_model_register_virtual("priced", "anthropic", "claude-sonnet-4-5");

    Cap direct, routed;
    agentc_memset(&direct, 0, sizeof direct);
    agentc_memset(&routed, 0, sizeof routed);
    direct.n = routed.n = 1;
    direct.codes[0] = routed.codes[0] = 0;
    direct.bodies[0] = routed.bodies[0] = reply_anthropic_ok;

    /* Direct, non-virtual model: no route event and (below) the same bytes. */
    AgcAgent *da = make_agent(agentc_prov_anthropic(), "claude-sonnet-4-5", &direct);
    /* The alias routes back to the agent's own provider, so it uses the agent's
     * effective credential (P8-D3/B-M4); both sides need the same one for the
     * byte-identity assertion. */
    agentc_agent_set_api_key(da, "direct-key");
    EvCap dev;
    agentc_memset(&dev, 0, sizeof dev);
    agentc_agent_set_events(da, ev_cb, &dev);
    int drc = agentc_agent_submit(da, "hi");
    check("identical.direct_rc", drc == 0);
    check("identical.no_route_event", dev.count == 0);

    /* The same target reached through the virtual alias. */
    AgcAgent *ra = make_agent(agentc_prov_anthropic(), "priced", &routed);
    agentc_agent_set_api_key(ra, "direct-key");
    int rrc = agentc_agent_submit(ra, "hi");
    check("identical.routed_rc", rrc == 0);

    bool same_url = agentc_streq(direct.last_url, routed.last_url);
    bool same_body = direct.last_body_len == routed.last_body_len &&
                     agentc_memeq(direct.last_body, routed.last_body, direct.last_body_len);
    check("identical.url", same_url);
    check("identical.body", same_body);

    /* A catalog-rate target proves cost lookup used the resolved model, not the
     * alias (whose own row has no rate). */
    const AgcTranscript *tr = agentc_agent_transcript(ra);
    bool cost_known = false;
    for (size_t i = 0; i < tr->n; i++)
        if (tr->msgs[i].role == AGENTC_ROLE_ASSISTANT &&
            tr->msgs[i].stop_reason == AGENTC_STOP_STOP && tr->msgs[i].usage.cost_micro > 0)
            cost_known = true;
    check("identical.cost_target", cost_known);
    check("identical.alias_transcript", agentc_streq(agentc_agent_model(ra), "priced"));
    check("identical.target_provider", t != NULL);

    agentc_agent_free(da);
    agentc_agent_free(ra);
    agentc_actor_release_all();
}

/* --------------------------------------------------------- fail closed */
static void test_unknown_fails_closed(void) {
    agentc_model_register_virtual("broken", "no-such-provider-xyz", "whatever");

    Cap cap;
    agentc_memset(&cap, 0, sizeof cap);
    cap.n = 1;
    cap.codes[0] = 0;
    cap.bodies[0] = reply_openai_ok;

    AgcAgent *a = make_agent(agentc_prov_anthropic(), "broken", &cap);
    int rc = agentc_agent_submit(a, "hi");
    const char *err = agentc_agent_last_error(a);
    check("broken.rc", rc != 0);
    check("broken.no_request", cap.calls == 0);
    check("broken.error", err != NULL && agentc_str_str(err, "unknown provider") != NULL);
    const AgcTranscript *tr = agentc_agent_transcript(a);
    bool errored = false;
    for (size_t i = 0; i < tr->n; i++)
        if (tr->msgs[i].role == AGENTC_ROLE_ASSISTANT &&
            tr->msgs[i].stop_reason == AGENTC_STOP_ERROR)
            errored = true;
    check("broken.step_error", errored);
    agentc_agent_free(a);
    agentc_actor_release_all();
}

/* ----------------------------------------------- session record persistence */
static int collect_custom(void *ud, const char *data_json) {
    char *dst = ud;
    if (data_json) {
        size_t n = agentc_strlen(data_json);
        if (n > 511)
            n = 511;
        agentc_memcpy(dst, data_json, n);
        dst[n] = 0;
    }
    return 0;
}

static void test_session_record(void) {
    bool win = agentc_streq(os_platform(), "windows");
    AgcSessionOptions o;
    agentc_memset(&o, 0, sizeof o);
    o.cwd = ".";
    AgcSession *s = agentc_session_new(&o);
    check_win("persist.session", win, s != NULL && agentc_session_path(s) != NULL);

    if (s != NULL) {
        agentc_mode_note_model_route(s, "fast", "target-x", "m-target");
        char data[512] = {0};
        int rc = agentc_session_load_custom(s, "model", collect_custom, data);
        check_win("persist.record",
                  win,
                  rc == 0 && agentc_str_str(data, "\"virtual\":\"fast\"") != NULL &&
                      agentc_str_str(data, "\"provider\":\"target-x\"") != NULL &&
                      agentc_str_str(data, "\"model\":\"m-target\"") != NULL);
        agentc_session_close(s);
    } else {
        check_win("persist.record", win, false);
    }
}

/* ----------------------------------------- cross-provider route credential */
static void test_route_key(void) {
    (void)agentc_prov_openai_compatible("target-x", "https://target.example/v1");
    agentc_model_register_virtual("fast", "target-x", "m-target");
    Cap cap;
    agentc_memset(&cap, 0, sizeof cap);
    cap.n = 1;
    cap.codes[0] = 0;
    cap.bodies[0] = reply_openai_ok;
    AgcAgent *a = make_agent(agentc_prov_anthropic(), "fast", &cap);
    /* P8-D1: the resolver rides the agent, not a process global. The routed
     * target must use its own endpoint and credential, never the agent's. */
    AgcRouteSeam seam;
    agentc_memset(&seam, 0, sizeof seam);
    seam.target = route_target_cb;
    agentc_agent_set_route_seam(a, &seam);
    int rc = agentc_agent_submit(a, "hi");
    check("routekey.rc", rc == 0 && cap.calls == 1);
    check("routekey.used",
          agentc_str_str(cap.last_headers, "authorization: Bearer route-key-123") != NULL);
    check("routekey.base_url", agentc_str_str(cap.last_url, "routed.example") != NULL);
    agentc_agent_free(a);
}

/* --------------------------------------- P8-D1: resolution is per-agent */
static void test_seam_per_agent(void) {
    (void)agentc_prov_openai_compatible("target-x", "https://target.example/v1");
    agentc_model_register_virtual("dual", "target-x", "m-target");

    Cap cap_a, cap_b;
    agentc_memset(&cap_a, 0, sizeof cap_a);
    agentc_memset(&cap_b, 0, sizeof cap_b);
    cap_a.n = cap_b.n = 1;
    cap_a.codes[0] = cap_b.codes[0] = 0;
    cap_a.bodies[0] = cap_b.bodies[0] = reply_openai_ok;

    AgcAgent *a = make_agent(agentc_prov_anthropic(), "dual", &cap_a);
    AgcAgent *b = make_agent(agentc_prov_anthropic(), "dual", &cap_b);
    AgcRouteSeam sa, sb;
    agentc_memset(&sa, 0, sizeof sa);
    agentc_memset(&sb, 0, sizeof sb);
    sa.target = route_target_a_cb;
    sb.target = route_target_b_cb;
    agentc_agent_set_route_seam(a, &sa);
    agentc_agent_set_route_seam(b, &sb);

    int ra = agentc_agent_submit(a, "hi");
    int rb = agentc_agent_submit(b, "hi");
    /* A two-resolver process is the regression: with the old global, the last
     * installed resolver would serve both agents. */
    check("seam.per_agent.a",
          ra == 0 && cap_a.calls == 1 && agentc_str_str(cap_a.last_url, "a.example") != NULL &&
              agentc_str_str(cap_a.last_headers, "Bearer key-A") != NULL);
    check("seam.per_agent.b",
          rb == 0 && cap_b.calls == 1 && agentc_str_str(cap_b.last_url, "b.example") != NULL &&
              agentc_str_str(cap_b.last_headers, "Bearer key-B") != NULL);
    agentc_agent_free(a);
    agentc_agent_free(b);
}

/* --------------------------- P8-D3/B-M4: routed same-provider parity */
static void test_parity_same_provider(void) {
    agentc_model_register_virtual("samep", "anthropic", "claude-sonnet-4-5");
    Cap cap;
    agentc_memset(&cap, 0, sizeof cap);
    cap.n = 1;
    cap.codes[0] = 0;
    cap.bodies[0] = reply_anthropic_ok;

    AgcAgentSpec s;
    agentc_memset(&s, 0, sizeof s);
    s.provider_handle = agentc_prov_anthropic();
    s.provider = "anthropic";
    s.model = "samep";
    s.thinking = -1;
    s.label = "route";
    s.flags = AGENTC_AGENT_F_WAIT;
    s.api_key = "primary-key";
    s.base_url = "https://primary.example/v1";
    AgcTransport t = {cap_request, &cap, NULL};
    s.transport = t;
    s.transport_set = true;
    AgcAgent *a = agentc_agent_create(&s);
    /* A routed alias back to the agent's own provider keeps the agent's effective
     * endpoint and credential, and never consults the cross-provider resolver. */
    AgcRouteSeam seam;
    agentc_memset(&seam, 0, sizeof seam);
    seam.target = route_target_cb; /* would return routed.example/route-key-123 */
    agentc_agent_set_route_seam(a, &seam);
    int rc = agentc_agent_submit(a, "hi");
    check("parity.same_provider_rc", rc == 0 && cap.calls == 1);
    check("parity.same_provider_base_url",
          agentc_str_str(cap.last_url, "primary.example") != NULL &&
              agentc_str_str(cap.last_url, "routed.example") == NULL);
    check("parity.same_provider_key",
          agentc_str_str(cap.last_headers, "x-api-key: primary-key") != NULL &&
              agentc_str_str(cap.last_headers, "route-key-123") == NULL);
    agentc_agent_free(a);
}

/* ---------------------------- P8-D3/B-M6: an absent routed key fails closed */
static void test_keyguard(void) {
    (void)agentc_prov_openai_compatible("target-x", "https://target.example/v1");
    agentc_model_register_virtual("nokey", "target-x", "m-target");
    Cap cap;
    agentc_memset(&cap, 0, sizeof cap);
    cap.n = 1;
    cap.codes[0] = 0;
    cap.bodies[0] = reply_openai_ok;
    AgcAgent *a = make_agent(agentc_prov_anthropic(), "nokey", &cap);
    int rc = agentc_agent_submit(a, "hi");
    const char *err = agentc_agent_last_error(a);
    check("keyguard.fail_closed",
          rc != 0 && cap.calls == 0 && err != NULL &&
              agentc_str_str(err, "no API key") != NULL);
    agentc_agent_free(a);
}

/* ------------------------ P8-D3/B-M7: cross-provider child credentials */
static void test_xprov_child(void) {
    (void)agentc_prov_openai_compatible("target-x", "https://target.example/v1");
    (void)agentc_prov_openai_compatible("target-y", "https://target-y.example/v1");

    /* Same provider: the child inherits the parent's credential (regression pin). */
    Cap cap1;
    agentc_memset(&cap1, 0, sizeof cap1);
    cap1.n = 1;
    cap1.codes[0] = 0;
    cap1.bodies[0] = reply_anthropic_ok;
    AgcAgent *p1 = make_agent(agentc_prov_anthropic(), "claude-sonnet-4-5", &cap1);
    agentc_agent_set_api_key(p1, "parent-key");
    AgcAgentSpec c1;
    agentc_memset(&c1, 0, sizeof c1);
    c1.parent = p1;
    c1.provider = "anthropic";
    c1.model = "claude-sonnet-4-5";
    c1.task = "hi";
    c1.flags = AGENTC_AGENT_F_WAIT;
    AgcAgent *ch1 = agentc_agent_create(&c1);
    check("xprov.same_provider_inherits",
          ch1 != NULL && cap1.calls == 1 &&
              agentc_str_str(cap1.last_headers, "x-api-key: parent-key") != NULL);
    agentc_agent_free(ch1);
    agentc_agent_free(p1);

    /* Cross provider: the child resolves its own key/endpoint through the
     * inherited seam, and never sends the parent's credential. */
    Cap cap2;
    agentc_memset(&cap2, 0, sizeof cap2);
    cap2.n = 1;
    cap2.codes[0] = 0;
    cap2.bodies[0] = reply_openai_ok;
    AgcAgent *p2 = make_agent(agentc_prov_anthropic(), "claude-sonnet-4-5", &cap2);
    agentc_agent_set_api_key(p2, "parent-key");
    AgcRouteSeam seam;
    agentc_memset(&seam, 0, sizeof seam);
    seam.target = route_target_cb;
    agentc_agent_set_route_seam(p2, &seam);
    AgcAgentSpec c2;
    agentc_memset(&c2, 0, sizeof c2);
    c2.parent = p2;
    c2.provider = "target-x";
    c2.model = "m-target";
    c2.task = "hi";
    c2.flags = AGENTC_AGENT_F_WAIT;
    AgcAgent *ch2 = agentc_agent_create(&c2);
    check("xprov.child_key",
          ch2 != NULL && cap2.calls == 1 &&
              agentc_str_str(cap2.last_url, "routed.example") != NULL &&
              agentc_str_str(cap2.last_headers, "Bearer route-key-123") != NULL &&
              agentc_str_str(cap2.last_headers, "parent-key") == NULL);
    agentc_agent_free(ch2);
    agentc_agent_free(p2);

    /* A cross-provider child with no resolvable key for a needs_key provider is
     * refused; no partial agent and no request. */
    Cap cap3;
    agentc_memset(&cap3, 0, sizeof cap3);
    cap3.n = 1;
    cap3.codes[0] = 0;
    cap3.bodies[0] = reply_openai_ok;
    AgcAgent *p3 = make_agent(agentc_prov_anthropic(), "claude-sonnet-4-5", &cap3);
    agentc_agent_set_api_key(p3, "parent-key");
    AgcAgentSpec c3;
    agentc_memset(&c3, 0, sizeof c3);
    c3.parent = p3;
    c3.provider = "target-y";
    c3.model = "m";
    c3.task = "hi";
    c3.flags = AGENTC_AGENT_F_WAIT;
    AgcAgent *ch3 = agentc_agent_create(&c3);
    check("xprov.child_refused", ch3 == NULL && cap3.calls == 0);
    agentc_agent_free(p3);
}

/* ------------------------ P8-D2/B-M9: a cleared alias fails closed */
static void test_alias_fact(void) {
    (void)agentc_prov_openai_compatible("target-x", "https://target.example/v1");
    agentc_model_register_virtual("vanish", "target-x", "m-target");

    Cap cap;
    agentc_memset(&cap, 0, sizeof cap);
    cap.n = 1;
    cap.codes[0] = 0;
    cap.bodies[0] = reply_anthropic_ok; /* the reselected model is anthropic */
    AgcAgent *a = make_agent(agentc_prov_anthropic(), "vanish", &cap);
    agentc_agent_set_api_key(a, "k");
    agentc_model_clear_virtual(); /* the live alias row disappears */
    int rc = agentc_agent_submit(a, "hi");
    const char *err = agentc_agent_last_error(a);
    check("aliasfact.cleared_fails_closed",
          rc != 0 && cap.calls == 0 && err != NULL &&
              agentc_str_str(err, "no longer configured") != NULL);
    const AgcTranscript *tr = agentc_agent_transcript(a);
    bool errored = false;
    for (size_t i = 0; i < tr->n; i++)
        if (tr->msgs[i].role == AGENTC_ROLE_ASSISTANT &&
            tr->msgs[i].stop_reason == AGENTC_STOP_ERROR)
            errored = true;
    check("aliasfact.step_error", errored);

    /* Re-selecting a concrete model clears the fact and the agent recovers. */
    int mrc = agentc_agent_set_model(a, "claude-sonnet-4-5");
    int rc2 = agentc_agent_submit(a, "hi");
    check("aliasfact.reselect_recovers", mrc == 0 && rc2 == 0 && cap.calls == 1);
    agentc_agent_free(a);

    /* A concrete-model agent is unaffected by a virtual-registry clear. */
    Cap cap2;
    agentc_memset(&cap2, 0, sizeof cap2);
    cap2.n = 1;
    cap2.codes[0] = 0;
    cap2.bodies[0] = reply_anthropic_ok;
    AgcAgent *b = make_agent(agentc_prov_anthropic(), "claude-sonnet-4-5", &cap2);
    agentc_agent_set_api_key(b, "k");
    agentc_model_clear_virtual();
    int rc3 = agentc_agent_submit(b, "hi");
    check("aliasfact.non_alias_unaffected", rc3 == 0 && cap2.calls == 1);
    agentc_agent_free(b);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    agentc_log_set_level(4);
    /* Clean runtime registry (the const catalog stays). */
    agentc_model_reset();
    test_config_parse();
    test_registry();
    test_route_host_and_model();
    test_route_key();
    test_seam_per_agent();
    test_parity_same_provider();
    test_keyguard();
    test_nonvirtual_identical();
    test_unknown_fails_closed();
    test_xprov_child();
    test_session_record();
    /* Last: clears every alias row. */
    test_alias_fact();
    agentc_model_reset();
    return fails;
}
