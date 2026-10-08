/* ext_test.c — extension registry + host vtable contract.
 *
 * Golden mode (tests/run.sh): registers an in-file fixture through
 * agentc_ext_adopt, exercises tools/events/hooks/sections/commands/defer and
 * asserts golden output, then agentc_ext_shutdown and the allocator baseline.
 *
 * Harness mode (tests/ext.sh): `build/ext_harness --harness` loads the real
 * generated registry (examples linked in) and checks them.
 *
 * The fixture export is renamed by ext.sh for the harness build so the Rust
 * example's canonical agentc_ext_init does not collide with it.
 */
#include "agentc.h"
#include "app/setup.h"
#include "app/policy.h"
#include "wire.h"
#include "plat.h"
#include "status.h"
#include "ext.h"
#include "ext/registry_int.h"
#include "core/events.h"
#include "core/tools/jobs.h"
#include "prov/provider.h"
#include "net/net_internal.h"

/* internal test hook from agent.c (the recompose selection test injects a
 * replay transport, so retries must not re-consume the canned reply) */
void agentc_agent_test_no_backoff(AgcAgent *a);

#ifndef AGENTC_EXT_TEST_ENTRY
#define AGENTC_EXT_TEST_ENTRY agentc_ext_init
#endif

void agentc_test_setenv(const char *name, const char *value);
void agentc_rm_rf(const char *path);
int agentc_write_file_atomic(const char *path, const void *data, size_t len, int mode);

#define TEST_ROOT "/tmp/agentc-ext-test"

static int fails;
static int g_shutdown_called;
static const AgcExtHost *g_host;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

static bool cstr_eq(const char *a, const char *b) {
    return a && b && agentc_streq(a, b);
}

/* ----------------------------------------------------------- fixture tools */

/* Marked identity for the fixture tool; the stability test proves it survives
 * the registry's internal reallocations. */
static int g_fixture_ud_marker;

static int fx_run(const AgcExtHost *host, const AgcExtTool *self,
                  const AgcExtToolCall *call, void *out, bool *is_error) {
    if (is_error) *is_error = false;
    const char *args = call && call->args_json ? call->args_json : "{}";
    if (self && self->ud == &g_fixture_ud_marker &&
        host->json_get_bool(args, "check_ud", 0)) {
        host->out_write(out, "ud-ok", 5);
        return 0;
    }
    if (host->json_get_bool(args, "echo_call_id", 0)) {
        const char *id = call && call->call_id ? call->call_id : "";
        host->out_write(out, id, agentc_strlen(id));
        return 0;
    }
    if (host->json_get_bool(args, "cancel_check", 0)) {
        const char *s = host->is_cancelled(host, call ? call->signal_token : NULL)
                            ? "cancelled" : "running";
        host->out_write(out, s, agentc_strlen(s));
        return 0;
    }
    const char *text = host->json_get_str(args, "text", NULL);
    if (!text) {
        if (is_error) *is_error = true;
        const char *e = "error: text required";
        host->out_write(out, e, agentc_strlen(e));
        return 0;
    }
    host->out_write(out, text, agentc_strlen(text));
    return 0;
}

static int fx_on_event(void *ud, const char *point, const char *payload_json,
                       char **result_json) {
    (void)ud;
    (void)point;
    if (result_json) *result_json = NULL;
    return g_host && g_host->json_get_bool(payload_json, "input.veto", 0) ? 1 : 0;
}

static int g_typed_events;
static int fx_turn_end(void *ud, const char *point, const char *payload_json,
                       char **result_json) {
    (void)ud;
    (void)point;
    (void)payload_json;
    if (result_json) *result_json = NULL;
    g_typed_events++;
    return 0;
}

static int g_turn_start_events;
static int fx_turn_start(void *ud, const char *point, const char *payload_json,
                         char **result_json) {
    (void)ud;
    (void)point;
    (void)payload_json;
    if (result_json) *result_json = NULL;
    g_turn_start_events++;
    return 0;
}

static void fx_command(const AgcExtHost *host, void *ud, const char *args_json,
                       void *out) {
    (void)ud;
    (void)host;
    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "command");
    agentc_jsonw_cstr(&w, "fx-echo");
    agentc_jsonw_key(&w, "args");
    agentc_jsonw_cstr(&w, args_json ? args_json : "");
    agentc_jsonw_end(&w);
    agentc_ext_host()->out_write(out, (const char *)b.p, b.len);
    agentc_buf_free(&b);
}

static const char fx_params[] = "{\"type\":\"object\"}";

static size_t fx_status(void *ud, AgcExtStatusSegment *out, size_t max,
                        char *arena, size_t arena_cap) {
    (void)ud;
    (void)arena;
    (void)arena_cap;
    if (max < 1) return 0;
    out[0].struct_size = sizeof out[0];
    out[0].slot = AGENTC_PSEG_SLOT_LEFT;
    out[0].priority = 50;
    out[0].style = AGENTC_PSEG_STYLE_DIM;
    out[0].text = "fixture-status";
    return 1;
}

static int fx_init(const AgcExtHost *host) {
    g_host = host;
    static const AgcExtTool tool = {
        .struct_size = sizeof(AgcExtTool),
        .flags = AGENTC_TOOL_READONLY | AGENTC_TOOL_SEQUENTIAL,
        .name = "fixture_echo",
        .label = "Fixture echo",
        .description = "Echo text from the test fixture.",
        .parameters_json = fx_params,
        .ud = &g_fixture_ud_marker,
        .run = fx_run,
    };
    host->add_tool(&tool);
    host->on("tool_call", AGENTC_HOOK_OVERRIDE, 0, fx_on_event, NULL);
    host->on("turn_end", AGENTC_HOOK_OVERRIDE, 0, fx_turn_end, NULL);
    static const AgcExtCommand cmd = {
        .struct_size = sizeof(AgcExtCommand),
        .name = "fx-echo",
        .description = "echo the argument",
        .run = fx_command,
    };
    host->add_command(&cmd);
    host->add_status(fx_status, NULL);
    host->set_status("fixture-status", "on");
    return 0;
}

static void fx_shutdown(void) { g_shutdown_called = 1; }

int AGENTC_EXT_TEST_ENTRY(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    out->abi_version = AGENTC_EXT_ABI;
    out->struct_size = sizeof *out;
    out->name = "fixture";
    out->version = "0.1";
    out->order = 0;
    out->init = fx_init;
    out->shutdown = fx_shutdown;
    return 0;
}

/* --------------------------------------------------------- provider fixture */

/* A second fixture extension registers a custom provider from init(). The
 * provider tests drive its build_request and stream_event directly through the
 * public provider handle. */
static int g_prov_ud_marker;
static int g_prov_open_calls;
static int g_prov_close_calls;

static int prov_build(const AgcExtHost *host, const AgcExtProvider *self,
                      const AgcExtRequestView *req, void *head_out, void *body_out) {
    /* A good line, the forbidden trio, a duplicate auth line, an injected CRLF
     * and a malformed line -- the adapter must sanitize all of it. */
    const char *head =
        "x-fixture: 1\r\n"
        "Host: evil\r\n"
        "content-length: 999\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Authorization: extension-key\r\n"
        "x-inj: a\rb\r\n"
        "Injected: b\r\n";
    host->out_write(head_out, head, agentc_strlen(head));
    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "provider");
    agentc_jsonw_cstr(&w, req->provider ? req->provider : "");
    agentc_jsonw_key(&w, "model");
    agentc_jsonw_cstr(&w, req->model ? req->model : "");
    agentc_jsonw_key(&w, "system");
    agentc_jsonw_cstr(&w, req->system ? req->system : "");
    agentc_jsonw_key(&w, "thinking");
    agentc_jsonw_i64(&w, req->thinking_level);
    agentc_jsonw_key(&w, "max_tokens");
    agentc_jsonw_i64(&w, req->max_tokens);
    agentc_jsonw_key(&w, "nmessages");
    agentc_jsonw_u64(&w, req->nmessages);
    agentc_jsonw_key(&w, "roles");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < req->nmessages; i++)
        agentc_jsonw_u64(&w, req->messages[i].role);
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "ntools");
    agentc_jsonw_u64(&w, req->ntools);
    agentc_jsonw_key(&w, "tool0");
    agentc_jsonw_cstr(&w, req->ntools ? req->tools[0].name : "");
    agentc_jsonw_key(&w, "ud");
    agentc_jsonw_bool(&w, self->ud == &g_prov_ud_marker);
    agentc_jsonw_end(&w);
    host->out_write(body_out, (const char *)b.p, b.len);
    agentc_buf_free(&b);
    return 0;
}

static int prov_stream_event(const AgcExtHost *host, const AgcExtProvider *self,
                             AgcExtStream *st, const AgcExtWireEvent *ev,
                             const AgcExtStreamSink *sink) {
    (void)self;
    if (!ev || !ev->data) return 0;
    if (cstr_eq(ev->data, "[done]")) {
        sink->stop(st, AGENTC_EXT_STOP_STOP);
        return 0;
    }
    const char *text = host->json_get_str(ev->data, "text", NULL);
    if (text) sink->text(st, text, agentc_strlen(text));
    const char *think = host->json_get_str(ev->data, "think", NULL);
    if (think) sink->thinking(st, think, agentc_strlen(think));
    const char *id = host->json_get_str(ev->data, "id", NULL);
    if (id) sink->response_id(st, id);
    const char *err = host->json_get_str(ev->data, "error", NULL);
    if (err) sink->error(st, err);
    if (host->json_get_bool(ev->data, "tool", 0))
        sink->tool_start(st, "call-9", "fixture_echo");
    const char *args = host->json_get_str(ev->data, "args", NULL);
    if (args) sink->tool_args(st, args, agentc_strlen(args));
    if (host->json_get_bool(ev->data, "usage_short", 0)) {
        /* struct_size stops before `reasoning`: the sink's field gate must
         * reject the whole struct instead of reading past it. */
        AgcExtUsage u;
        agentc_memset(&u, 0, sizeof u);
        u.struct_size = (uint32_t)offsetof(AgcExtUsage, reasoning);
        u.fields = AGENTC_EXT_USAGE_INPUT | AGENTC_EXT_USAGE_REASONING;
        u.input = 77;
        u.reasoning = 5;
        sink->usage(st, &u);
    }
    if (host->json_get_bool(ev->data, "usage", 0)) {
        AgcExtUsage u;
        agentc_memset(&u, 0, sizeof u);
        u.struct_size = sizeof u;
        u.fields = AGENTC_EXT_USAGE_INPUT | AGENTC_EXT_USAGE_OUTPUT |
                   AGENTC_EXT_USAGE_REASONING;
        u.input = 11;
        u.output = 22;
        u.reasoning = 3;
        sink->usage(st, &u);
    }
    if (host->json_get_bool(ev->data, "stop_tool", 0))
        sink->stop(st, AGENTC_EXT_STOP_TOOLUSE);
    return 0;
}

static int prov_stream_open(const AgcExtHost *host, const AgcExtProvider *self,
                            AgcExtStream *st) {
    (void)self;
    g_prov_open_calls++;
    st->ud = host->alloc(8);   /* extension-owned stream state */
    return 0;
}

static void prov_stream_close(const AgcExtHost *host, const AgcExtProvider *self,
                              AgcExtStream *st) {
    (void)self;
    g_prov_close_calls++;
    host->free(st->ud);
    st->ud = NULL;
}

static const AgcExtProviderAuth g_prov_auth = {
    .struct_size = sizeof(AgcExtProviderAuth),
    .kind = AGENTC_EXT_AUTH_BEARER,
    .header = "Authorization",
    .prefix = "Bearer ",
};

static const AgcExtProviderModel g_prov_models[] = {
    { .struct_size = sizeof(AgcExtProviderModel), .id = "fx-small", .name = "Fixture Small",
      .ctx_window = 8000, .max_tokens = 1000, .reasoning = 0, .image = 1 },
    { .struct_size = sizeof(AgcExtProviderModel), .id = "fx-big", .name = "Fixture Big",
      .ctx_window = 32000, .max_tokens = 4000, .reasoning = 1, .image = 0 },
};

static const AgcExtProvider g_prov_desc = {
    .struct_size = sizeof(AgcExtProvider),
    .name = "provfix",
    .label = "Fixture Provider",
    .default_base_url = "https://fixture.example.test/v1",
    .path = "/chat",
    .env_keys = { "PROVFIX_API_KEY", NULL, NULL },
    .needs_key = 1,
    .discover_style = AGENTC_EXT_DISCOVER_NONE,
    .auth = &g_prov_auth,
    .models = g_prov_models,
    .nmodels = 2,
    .ud = &g_prov_ud_marker,
    .build_request = prov_build,
    .stream_open = prov_stream_open,
    .stream_event = prov_stream_event,
    .stream_close = prov_stream_close,
};

static int prov_fixture_init(const AgcExtHost *host) {
    host->add_provider(&g_prov_desc);
    return 0;
}

static int prov_fixture_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    out->abi_version = AGENTC_EXT_ABI;
    out->struct_size = sizeof *out;
    out->name = "provfix-ext";
    out->version = "1";
    out->order = 0;
    out->init = prov_fixture_init;
    out->shutdown = NULL;
    return 0;
}

/* ------------------------------------------------- custom provider tests */

static const AgcProvider *prov_setup(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("provfix-ext", prov_fixture_entry);
    agentc_ext_load_all();
    const AgcProviderOps *ops = agentc_provider_by_name("provfix");
    return ops ? agentc_provider_handle((AgcProviderOps *)ops) : NULL;
}

static size_t prov_count_named(const char *name) {
    const AgcProviderOps *all[96];
    size_t n = agentc_provider_all(all, 96);
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (all[i]->name && cstr_eq(all[i]->name, name)) k++;
    return k;
}

static void test_provider_registration(void) {
    const AgcProvider *h = prov_setup();
    const AgcProviderOps *ops = agentc_provider_by_name("provfix");
    check("provider_registered", h != NULL && ops != NULL);
    check("provider_api_is_name", ops && cstr_eq(ops->api, "provfix"));
    check("provider_path", ops && cstr_eq(ops->path, "/chat"));
    check("provider_base_url",
          h && cstr_eq(h->default_base_url, "https://fixture.example.test/v1"));
    check("provider_ext_marked", ops && ops->is_ext && !ops->retired && ops->ext_rec != NULL);
    /* The public NONE (=3) collides with the internal GOOGLE (=3); it must be
     * translated to the internal NONE so discovery returns before probing. A
     * pre-registered dynamic row proves the early return keeps it intact. */
    check("provider_discover_none_style", ops && ops->discover_style == AGENTC_DISCOVER_NONE);
    agentc_model_register_dynamic("provfix", "fx-probe-marker", "provfix",
                                  "https://fixture.example.test/v1", 0, 0, false, false);
    check("provider_discover_none_not_probed",
          agentc_setup_discover(NULL, "provfix", true, false, false) == 0 &&
              agentc_model_find("provfix", "fx-probe-marker") != NULL);
    agentc_model_clear_dynamic("provfix");
    check("provider_env_keys",
          ops && cstr_eq(ops->env_keys[0], "PROVFIX_API_KEY") && ops->env_keys[1] == NULL);
    check("provider_handle_stable", agentc_provider_handle((AgcProviderOps *)ops) == h);
    const AgcModel *m = agentc_model_find("provfix", "fx-small");
    check("provider_model_static",
          m && agentc_model_is_static(m) && m->ctx_window == 8000 &&
              m->max_tokens == 1000 && m->image);
    check("provider_model_two", agentc_model_find("provfix", "fx-big") != NULL);
    check("provider_by_api", agentc_provider_by_api("provfix") == ops);
    check("provider_all_fixture_once", prov_count_named("provfix") == 1);
    AgcBuf ah = { 0 };
    ops->auth_headers(ops, &ah, "sk-x");
    check("provider_auth_headers",
          ah.p && cstr_eq((char *)ah.p, "Authorization: Bearer sk-x\r\n"));
    agentc_buf_free(&ah);
    /* duplicate of the same fixture name is rejected */
    AgcExtProvider dup = g_prov_desc;
    agentc_ext_host()->add_provider(&dup);
    check("provider_dup_fixture", prov_count_named("provfix") == 1);
    /* duplicate of a builtin is rejected (openai owns two rows:
     * openai-chat and openai-codex-responses) */
    dup.name = "openai";
    agentc_ext_host()->add_provider(&dup);
    check("provider_dup_builtin", prov_count_named("openai") == 2);
    /* duplicate of a lazily materialized preset is rejected too */
    dup.name = "openrouter";
    agentc_ext_host()->add_provider(&dup);
    check("provider_dup_preset",
          prov_count_named("openrouter") == 1 &&
              agentc_provider_by_name("openrouter") != NULL);

    /* AUTH_HEADER: the declared header and the verbatim prefix. */
    AgcExtProviderAuth ha = g_prov_auth;
    ha.kind = AGENTC_EXT_AUTH_HEADER;
    ha.header = "X-Api-Key";
    ha.prefix = NULL;
    AgcExtProvider ph = g_prov_desc;
    ph.name = "provhdr";
    ph.label = NULL;
    ph.auth = &ha;
    ph.env_keys[0] = NULL;
    ph.env_keys[1] = NULL;
    ph.env_keys[2] = NULL;
    ph.models = NULL;
    ph.nmodels = 0;
    agentc_ext_host()->add_provider(&ph);
    const AgcProviderOps *hdr_ops = agentc_provider_by_name("provhdr");
    AgcBuf hb = { 0 };
    if (hdr_ops) hdr_ops->auth_headers(hdr_ops, &hb, "k1");
    check("provider_auth_header_kind",
          hdr_ops != NULL && hb.p && cstr_eq((char *)hb.p, "X-Api-Key: k1\r\n"));
    agentc_buf_free(&hb);

    /* AUTH_BEARER with no prefix defaults to "Bearer ". */
    AgcExtProviderAuth ba2 = g_prov_auth;
    ba2.prefix = NULL;
    AgcExtProvider pb = g_prov_desc;
    pb.name = "provbear";
    pb.label = NULL;
    pb.auth = &ba2;
    pb.env_keys[0] = NULL;
    pb.env_keys[1] = NULL;
    pb.env_keys[2] = NULL;
    pb.models = NULL;
    pb.nmodels = 0;
    agentc_ext_host()->add_provider(&pb);
    const AgcProviderOps *bear_ops = agentc_provider_by_name("provbear");
    AgcBuf bb = { 0 };
    if (bear_ops) bear_ops->auth_headers(bear_ops, &bb, "k2");
    check("provider_auth_bearer_default",
          bear_ops != NULL && bb.p &&
              cstr_eq((char *)bb.p, "Authorization: Bearer k2\r\n"));
    agentc_buf_free(&bb);

    /* AUTH_NONE plus NULL stream_open/finish/close: the stateless path. */
    AgcExtProvider ps = g_prov_desc;
    ps.name = "provstat";
    ps.label = NULL;
    ps.auth = NULL;
    ps.env_keys[0] = NULL;
    ps.env_keys[1] = NULL;
    ps.env_keys[2] = NULL;
    ps.models = NULL;
    ps.nmodels = 0;
    ps.stream_open = NULL;
    ps.stream_finish = NULL;
    ps.stream_close = NULL;
    agentc_ext_host()->add_provider(&ps);
    const AgcProviderOps *stat_ops = agentc_provider_by_name("provstat");
    check("provider_stateless_register", stat_ops != NULL);
    if (stat_ops) {
        const AgcProvider *sh = agentc_provider_handle((AgcProviderOps *)stat_ops);
        AgcStreamState sst;
        AgcMsg smsg;
        agentc_memset(&smsg, 0, sizeof smsg);
        smsg.role = AGENTC_ROLE_ASSISTANT;
        int src = agentc_stream_state_init(sh, &sst);
        sst.msg = &smsg;
        AgcSseEvent sev;
        agentc_memset(&sev, 0, sizeof sev);
        sev.event = "content";
        sev.data = "{\"text\":\"ok\"}";
        sev.data_len = agentc_strlen(sev.data);
        (void)sh->map_sse(&sst, &sev);
        sev.data = "[done]";
        sev.data_len = 6;
        (void)sh->map_sse(&sst, &sev);
        check("provider_stateless_stream",
              src == 0 && sh->finish(&sst) == 0 && smsg.nblocks == 1 &&
                  cstr_eq(smsg.blocks[0].text, "ok"));
        agentc_stream_state_close(&sst);
        agentc_msg_free(&smsg);
        AgcBuf nb = { 0 };
        stat_ops->auth_headers(stat_ops, &nb, "k");
        check("provider_auth_none", nb.p == NULL);
        agentc_buf_free(&nb);
    }
}

static void test_provider_all(void) {
    (void)prov_setup();
    const AgcProviderOps *all[96];
    size_t total = agentc_provider_all(NULL, 0);
    size_t n = agentc_provider_all(all, 96);
    check("provider_all_count_first", n == total && total >= 6);
    size_t openai_n = 0, provfix_n = 0;
    bool chat = false, codex = false;
    for (size_t i = 0; i < n; i++) {
        if (cstr_eq(all[i]->name, "openai")) {
            openai_n++;
            if (cstr_eq(all[i]->api, "openai-chat")) chat = true;
            if (cstr_eq(all[i]->api, "openai-codex-responses")) codex = true;
        }
        if (cstr_eq(all[i]->name, "provfix")) provfix_n++;
    }
    check("provider_all_two_openai", openai_n == 2 && chat && codex);
    check("provider_all_fixture", provfix_n == 1);
}

static AgcExtProviderModel g_many_models[257];
static char g_bad_long_id[300];

static void test_provider_validation(void) {
    (void)prov_setup();
    const AgcExtHost *host = agentc_ext_host();
    size_t before = agentc_provider_all(NULL, 0);
    AgcExtProvider p;

    p = g_prov_desc;
    p.name = "val-short";
    p.struct_size = (uint32_t)offsetof(AgcExtProvider, stream_event);
    host->add_provider(&p);

    p = g_prov_desc;
    p.name = "Bad Name!";
    host->add_provider(&p);
    p.name = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    host->add_provider(&p);

    p = g_prov_desc;
    p.name = "val-path";
    p.path = "chat";
    host->add_provider(&p);
    p.path = "/a b";
    host->add_provider(&p);

    p = g_prov_desc;
    p.name = "val-url";
    p.default_base_url = "ftp://x";
    host->add_provider(&p);
    p.default_base_url = "https://exa mple.test";
    host->add_provider(&p);

    p = g_prov_desc;
    p.name = "val-missing";
    p.build_request = NULL;
    host->add_provider(&p);
    p.build_request = prov_build;
    p.stream_event = NULL;
    host->add_provider(&p);

    p = g_prov_desc;
    p.name = "val-discover";
    p.discover_style = 9;
    host->add_provider(&p);

    p = g_prov_desc;
    p.name = "val-many";
    p.models = g_many_models;
    p.nmodels = 257;
    host->add_provider(&p);

    static AgcExtProviderModel longm;
    longm = g_prov_models[0];
    agentc_memset(g_bad_long_id, 'a', sizeof g_bad_long_id - 1);
    g_bad_long_id[sizeof g_bad_long_id - 1] = 0;
    longm.id = g_bad_long_id;
    p = g_prov_desc;
    p.name = "val-longid";
    p.models = &longm;
    p.nmodels = 1;
    host->add_provider(&p);

    AgcExtProviderAuth auth;
    p = g_prov_desc;
    p.name = "val-auth-q";
    auth = g_prov_auth;
    auth.kind = AGENTC_EXT_AUTH_QUERY;
    p.auth = &auth;
    host->add_provider(&p);

    p.name = "val-auth-hdr";
    auth = g_prov_auth;
    auth.kind = AGENTC_EXT_AUTH_HEADER;
    auth.header = NULL;
    p.auth = &auth;
    host->add_provider(&p);
    auth.header = "X Key";
    host->add_provider(&p);

    p.name = "val-auth-pfx";
    auth = g_prov_auth;
    auth.prefix = "Bearer\r\nx";
    p.auth = &auth;
    host->add_provider(&p);

    p.name = "val-auth-small";
    auth = g_prov_auth;
    auth.struct_size =
        (uint32_t)(offsetof(AgcExtProviderAuth, kind) + sizeof auth.kind);
    p.auth = &auth;
    host->add_provider(&p);

    /* over-long registration strings are dropped like any other
     * invalid contribution (base url <= 1024, header/env keys <= 128). */
    static char long_url[1200];
    static char long_key[160];
    agentc_memset(long_url, 'a', sizeof long_url - 1);
    long_url[sizeof long_url - 1] = 0;
    agentc_memcpy(long_url, "https://", 8);
    agentc_memset(long_key, 'a', sizeof long_key - 1);
    long_key[sizeof long_key - 1] = 0;

    p = g_prov_desc;
    p.name = "val-longurl";
    p.default_base_url = long_url;
    host->add_provider(&p);

    p = g_prov_desc;
    p.name = "val-longenv";
    p.env_keys[0] = long_key;
    host->add_provider(&p);

    p = g_prov_desc;
    p.name = "val-longhdr";
    auth = g_prov_auth;
    auth.header = long_key;
    p.auth = &auth;
    host->add_provider(&p);

    check("provider_validation_no_rows", agentc_provider_all(NULL, 0) == before);
}

static void test_provider_request(void) {
    AgcTranscript t;
    agentc_transcript_init(&t);
    AgcMsg *u = agentc_transcript_push(&t, AGENTC_ROLE_USER);
    agentc_msg_add_text(u, "hello", 5);
    /* failed assistant turns and empty assistant turns never reach the view */
    AgcMsg *bad = agentc_transcript_push(&t, AGENTC_ROLE_ASSISTANT);
    agentc_msg_add_text(bad, "boom", 4);
    bad->stop_reason = AGENTC_STOP_ERROR;
    (void)agentc_transcript_push(&t, AGENTC_ROLE_ASSISTANT);
    AgcMsg *a = agentc_transcript_push(&t, AGENTC_ROLE_ASSISTANT);
    a->stop_reason = AGENTC_STOP_TOOLUSE;
    agentc_msg_add_tool_call(a, "call-1", "fixture_echo");
    agentc_msg_tool_args_append(a, "{\"text\":\"x\"}", 12);
    AgcMsg *tool = agentc_transcript_push(&t, AGENTC_ROLE_TOOL);
    agentc_msg_add_text(tool, "result", 6);
    tool->blocks[0].tool_id = agentc_strdup("call-1");

    AgcTool tools[2];
    agentc_memset(tools, 0, sizeof tools);
    tools[0].name = "fixture_echo";
    tools[0].desc = "echo";
    tools[0].params_json = "{\"type\":\"object\"}";
    tools[0].flags = AGENTC_TOOL_READONLY;
    tools[1].name = "hidden_tool";
    tools[1].desc = "hidden";
    tools[1].flags = AGENTC_TOOL_HIDDEN;

    AgcRequest req;
    agentc_memset(&req, 0, sizeof req);
    req.provider = "provfix";
    req.model = "fx-small";
    req.api_key = "sk-test";
    req.system = "sys";
    req.transcript = &t;
    req.tools = tools;
    req.ntools = 2;
    req.thinking_level = 2;
    req.max_tokens = 1234;

    const AgcProvider *h = prov_setup();
    AgcBuf out = { 0 };
    int rc = h->build_request(&out, &req, NULL, "/chat");
    check("provider_build_rc", rc == 0);
    i64 k = agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4);
    check("provider_build_split", k >= 0);
    char *headers = agentc_strdup_len((const char *)out.p, (size_t)k);
    check("provider_head_fixture", agentc_str_str(headers, "x-fixture: 1") != NULL);
    check("provider_head_host_dropped", agentc_str_str(headers, "Host:") == NULL);
    check("provider_head_cl_dropped", agentc_str_str(headers, "content-length:") == NULL);
    check("provider_head_te_dropped",
          agentc_str_str(headers, "Transfer-Encoding:") == NULL);
    check("provider_head_auth_ext_dropped",
          agentc_str_str(headers, "extension-key") == NULL);
    check("provider_head_auth_added",
          agentc_str_str(headers, "Authorization: Bearer sk-test") != NULL);
    check("provider_head_injection_spaced",
          agentc_str_str(headers, "x-inj: a b") != NULL);
    const char *body = (const char *)out.p + k + 4;
    check("provider_body_provider", agentc_str_str(body, "\"provider\":\"provfix\"") != NULL);
    check("provider_body_model", agentc_str_str(body, "\"model\":\"fx-small\"") != NULL);
    check("provider_body_nmessages", agentc_str_str(body, "\"nmessages\":3") != NULL);
    check("provider_body_roles", agentc_str_str(body, "\"roles\":[1,2,3]") != NULL);
    check("provider_body_tools",
          agentc_str_str(body, "\"ntools\":1") != NULL &&
              agentc_str_str(body, "hidden_tool") == NULL);
    check("provider_body_ud", agentc_str_str(body, "\"ud\":true") != NULL);
    agentc_free(headers);
    agentc_buf_free(&out);
    agentc_transcript_free(&t);
}

static void test_provider_stream(void) {
    const AgcProvider *h = prov_setup();
    check("provider_stream_handle", h != NULL);
    AgcStreamState st;
    int rc = agentc_stream_state_init(h, &st);
    check("provider_stream_open", rc == 0 && g_prov_open_calls == 1);
    AgcMsg msg;
    agentc_memset(&msg, 0, sizeof msg);
    msg.role = AGENTC_ROLE_ASSISTANT;
    st.msg = &msg;

    AgcSseEvent ev;
    agentc_memset(&ev, 0, sizeof ev);
    ev.event = "content";
    ev.data = "{\"text\":\"hello\",\"id\":\"resp-1\",\"usage\":true}";
    ev.data_len = agentc_strlen(ev.data);
    check("provider_map_text", h->map_sse(&st, &ev) == 0);
    ev.data = "{\"think\":\"hmm\"}";
    ev.data_len = agentc_strlen(ev.data);
    (void)h->map_sse(&st, &ev);
    ev.data = "{\"tool\":true,\"args\":\"{\\\"a\\\":1}\"}";
    ev.data_len = agentc_strlen(ev.data);
    (void)h->map_sse(&st, &ev);
    check("provider_sink_text",
          msg.nblocks >= 1 && msg.blocks[0].type == AGENTC_BLK_TEXT &&
              cstr_eq(msg.blocks[0].text, "hello"));
    check("provider_sink_think",
          msg.nblocks >= 2 && msg.blocks[1].type == AGENTC_BLK_THINK &&
              cstr_eq(msg.blocks[1].text, "hmm"));
    check("provider_sink_tool",
          msg.nblocks >= 3 && msg.blocks[2].type == AGENTC_BLK_TOOLCALL &&
              cstr_eq(msg.blocks[2].tool_id, "call-9") &&
              cstr_eq(msg.blocks[2].tool_args, "{\"a\":1}"));
    check("provider_sink_usage",
          st.usage_input == 11 && st.usage_output == 22 && st.usage_reasoning == 3);
    /* a truncated usage struct (no reasoning field) is ignored whole */
    ev.data = "{\"usage_short\":true}";
    ev.data_len = agentc_strlen(ev.data);
    (void)h->map_sse(&st, &ev);
    check("provider_sink_usage_gated",
          st.usage_input == 11 && st.usage_output == 22 && st.usage_reasoning == 3);
    check("provider_sink_response_id", cstr_eq(st.response_id, "resp-1"));
    ev.data = "[done]";
    ev.data_len = 6;
    (void)h->map_sse(&st, &ev);
    check("provider_stream_finish", h->finish(&st) == 0 && st.stop_reason == AGENTC_STOP_STOP);
    agentc_stream_state_close(&st);
    check("provider_stream_close", g_prov_close_calls == 1);
    agentc_msg_free(&msg);

    /* finish without a stop is a clean protocol error */
    AgcMsg msg2;
    agentc_memset(&msg2, 0, sizeof msg2);
    msg2.role = AGENTC_ROLE_ASSISTANT;
    rc = agentc_stream_state_init(h, &st);
    check("provider_stream_reopen", rc == 0 && g_prov_open_calls == 2);
    st.msg = &msg2;
    check("provider_stream_finish_early",
          h->finish(&st) == -1 && st.stop_reason == AGENTC_STOP_ERROR);
    agentc_stream_state_close(&st);
    check("provider_stream_close2", g_prov_close_calls == 2);
    agentc_msg_free(&msg2);
}

static void test_provider_retire(void) {
    agentc_ext_shutdown();
    size_t baseline = agentc_mem_live();
    const AgcProvider *h = prov_setup();
    check("provider_retire_setup", h != NULL);
    agentc_ext_unload("provfix-ext");
    check("provider_retire_lookup", agentc_provider_by_name("provfix") == NULL);
    check("provider_retire_by_api", agentc_provider_by_api("provfix") == NULL);
    const AgcProviderOps *ops = agentc_provider_ops(h);
    check("provider_retire_handle", ops != NULL && ops->retired && ops->is_ext);
    AgcRequest req;
    agentc_memset(&req, 0, sizeof req);
    req.provider = "provfix";
    req.model = "fx-small";
    AgcBuf out = { 0 };
    check("provider_retire_build", h->build_request(&out, &req, NULL, "/chat") == -38);
    agentc_buf_free(&out);
    /* A held handle must not follow a re-registered name: the retired row keeps
     * the name reserved, so the attempt is dropped and the handle keeps
     * returning the clean -ENOSYS. */
    AgcExtProvider again = g_prov_desc;
    again.name = "provfix";
    agentc_ext_host()->add_provider(&again);
    check("provider_retire_reregister", agentc_provider_by_name("provfix") == NULL);
    AgcBuf out2 = { 0 };
    check("provider_retire_build_reserved",
          h->build_request(&out2, &req, NULL, "/chat") == -38);
    agentc_buf_free(&out2);
    AgcStreamState st;
    check("provider_retire_stream_open", agentc_stream_state_init(h, &st) == -38);
    agentc_ext_shutdown();
    check("provider_retire_mem_baseline", agentc_mem_live() == baseline);
}

static void test_provider_cap(void) {
    agentc_ext_shutdown();
    const AgcExtHost *host = agentc_ext_host();
    char names[64][16];
    AgcExtProvider p;
    for (int i = 0; i < 64; i++) {
        agentc_snprintf(names[i], sizeof names[i], "cap-%d", i);
        p = g_prov_desc;
        p.name = names[i];
        p.label = NULL;
        p.env_keys[0] = NULL;
        p.env_keys[1] = NULL;
        p.env_keys[2] = NULL;
        p.models = NULL;
        p.nmodels = 0;
        host->add_provider(&p);
    }
    check("provider_cap_last", agentc_provider_by_name("cap-63") != NULL);
    p = g_prov_desc;
    p.name = "cap-over";
    p.label = NULL;
    p.env_keys[0] = NULL;
    p.env_keys[1] = NULL;
    p.env_keys[2] = NULL;
    p.models = NULL;
    p.nmodels = 0;
    host->add_provider(&p);
    check("provider_cap_reached", agentc_provider_by_name("cap-over") == NULL);
}

static void test_model_static(void) {
    agentc_ext_shutdown();
    agentc_model_clear_static(NULL);
    agentc_model_clear_dynamic(NULL);
    check("model_base_clean", agentc_model_dynamic_count() == 0);
    agentc_model_register_static("mprov", "m1", "mprov", "https://x", 100, 10, false,
                                 false);
    const AgcModel *m = agentc_model_find("mprov", "m1");
    check("model_static_registered",
          m && agentc_model_is_static(m) && m->ctx_window == 100);
    agentc_model_register_dynamic("mprov", "m1", "mprov", "https://x", 200, 20, true, true);
    m = agentc_model_find("mprov", "m1");
    check("model_static_kept_on_update",
          m && agentc_model_is_static(m) && m->ctx_window == 200 && m->max_tokens == 20 &&
              m->reasoning && m->image);
    agentc_model_clear_dynamic("mprov");
    check("model_static_survives_clear", agentc_model_find("mprov", "m1") != NULL);
    char id[32];
    for (int i = 1; agentc_model_dynamic_count() < 256; i++) {
        agentc_snprintf(id, sizeof id, "cap-%d", i);
        agentc_model_register_static("capm", id, "capm", "", 0, 0, false, false);
    }
    check("model_cap_full", agentc_model_dynamic_count() == 256);
    agentc_model_register_static("capm", "overflow", "capm", "", 0, 0, false, false);
    check("model_cap_overflow_absent", agentc_model_find("capm", "overflow") == NULL);
    agentc_model_clear_static("capm");
    agentc_model_clear_static("mprov");
    check("model_cap_cleared", agentc_model_dynamic_count() == 0);
}

/* ------------------------------------------------------- registry fixtures */

static int g_order_log[4];
static int g_order_n;
static int order_init(int id) {
    g_order_log[g_order_n++] = id;
    return 0;
}
static int order_a_init(const AgcExtHost *host) { (void)host; return order_init(1); }
static int order_b_init(const AgcExtHost *host) { (void)host; return order_init(2); }

static const AgcExt g_order_a = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "order-a", "1", 10, order_a_init, NULL, 0 };
static const AgcExt g_order_b = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "order-b", "1", -10, order_b_init, NULL, 0 };

/* a failed init: register then fail; the registry must roll back and shut down */
static int g_fail_shutdown_called;
static void fail_shutdown(void) { g_fail_shutdown_called = 1; }
static int g_fail_pump_calls;
static int fail_pump(void *ud) {
    (void)ud;
    g_fail_pump_calls++;
    return 0;
}
static void fail_command_run(const AgcExtHost *host, void *ud, const char *args_json,
                             void *out) {
    (void)host; (void)ud; (void)args_json; (void)out;
}
static int fail_section_render(const AgcExtHost *host, void *ud, void *out) {
    (void)ud;
    host->out_write(out, "FAILSEC", 7);
    return 0;
}
static size_t fail_status_provider(void *ud, AgcExtStatusSegment *out, size_t max,
                                   char *arena, size_t arena_cap) {
    (void)ud; (void)arena; (void)arena_cap;
    if (out == NULL || max == 0) return 0;
    agentc_memset(&out[0], 0, sizeof out[0]);
    out[0].struct_size = sizeof out[0];
    out[0].slot = AGENTC_PSEG_SLOT_LEFT;
    out[0].text = "failed-init-status";
    return 1;
}
static int fail_init(const AgcExtHost *host) {
    static const AgcExtTool tool = {
        .struct_size = sizeof(AgcExtTool), .flags = AGENTC_TOOL_READONLY,
        .name = "fixture_bad", .label = "bad", .description = "bad",
        .parameters_json = fx_params, .run = fx_run };
    static const AgcExtCommand cmd = {
        .struct_size = sizeof(AgcExtCommand), .name = "fail-cmd",
        .description = "must roll back", .run = fail_command_run };
    static const AgcExtSection sec = {
        .struct_size = sizeof(AgcExtSection), .key = "fail-sec", .priority = 100,
        .render = fail_section_render };
    host->add_tool(&tool);
    host->add_command(&cmd);
    host->add_section(&sec);
    host->add_status(fail_status_provider, NULL);
    agentc_ext_add_pump(fail_pump, NULL);
    return 1;
}
static const AgcExt g_fail = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "fixture-fail", "0.1", 0, fail_init, fail_shutdown, 0 };

/* an extension whose shutdown() checks that owner removal already happened */
static size_t g_unload_tools_at_shutdown;
static bool g_unload_wants_at_shutdown;
static int unload_order_hook(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = NULL;
    return 0;
}
static int unload_order_init(const AgcExtHost *host) {
    static const AgcExtTool tool = {
        .struct_size = sizeof(AgcExtTool), .flags = AGENTC_TOOL_READONLY,
        .name = "unload_order_tool", .label = "unload order", .description = "d",
        .parameters_json = fx_params, .run = fx_run };
    host->add_tool(&tool);
    host->on("turn_start", AGENTC_HOOK_OBSERVE, 0, unload_order_hook, NULL);
    return 0;
}
static void unload_order_shutdown(void) {
    g_unload_tools_at_shutdown = agentc_ext_tools(NULL, 0);
    g_unload_wants_at_shutdown = agentc_ext_wants("turn_start");
}
static const AgcExt g_unload_order = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "unload-order", "1", 0,
    unload_order_init, unload_order_shutdown, 0 };

/* init() registering another extension: load_all must initialize the newly
 * appended tail too (bounded rounds). chain-0's init appends chain-1, whose
 * init appends chain-2. */
static int g_chain_inits;
static const char *const chain_names[] = { "chain-0", "chain-1", "chain-2" };
static int chain_init(const AgcExtHost *host) {
    (void)host;
    int i = g_chain_inits++;
    if (i + 1 < 3) {
        AgcExt next = {
            AGENTC_EXT_ABI, sizeof(AgcExt), chain_names[i + 1], "1", 0,
            chain_init, NULL, 0 };
        agentc_ext_register(&next);
    }
    return 0;
}
static const AgcExt g_chain_ext = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "chain-0", "1", 0, chain_init, NULL, 0 };

/* A chain longer than the load cap: every init appends another extension, so
 * init calls must stop at AGENTC_EXT_LOAD_ROUNDS (8) with the cap logged. */
static int g_cap_inits;
static int cap_init(const AgcExtHost *host) {
    (void)host;
    g_cap_inits++;
    char name[32];
    agentc_snprintf(name, sizeof name, "cap-%d", g_cap_inits);
    AgcExt next = {
        AGENTC_EXT_ABI, sizeof(AgcExt), name, "1", 0, cap_init, NULL, 0 };
    agentc_ext_register(&next);
    return 0;
}
static const AgcExt g_cap_ext = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "cap-0", "1", 0, cap_init, NULL, 0 };

/* shutdown() registering another extension: the live-record teardown must
 * still clear it (no leak). The appended record is pending, so it has no
 * shutdown() of its own; a later load_all must not see it. */
static int g_shutdown_append_calls;
static void shutdown_append_shutdown(void) {
    g_shutdown_append_calls++;
    static const AgcExt tail = {
        AGENTC_EXT_ABI, sizeof(AgcExt), "shutdown-tail", "1", 0, NULL, NULL, 0 };
    agentc_ext_register(&tail);
}
static const AgcExt g_shutdown_append = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "shutdown-append", "1", 0, NULL,
    shutdown_append_shutdown, 0 };

/* init() calling agentc_ext_shutdown(): while load_all runs the registry must
 * stay intact, so the call is a logged no-op and the extension still loads. */
static int g_loadshutdown_calls;
static int loadshutdown_init(const AgcExtHost *host) {
    (void)host;
    g_loadshutdown_calls++;
    agentc_ext_shutdown();
    return 0;
}
static const AgcExt g_loadshutdown = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "load-shutdown", "1", 0, loadshutdown_init, NULL, 0 };

/* init() unloading its own record: load_all's post-init `!e->used` re-check
 * must skip the freed record instead of dereferencing it. */
static int loadunload_init(const AgcExtHost *host) {
    (void)host;
    agentc_ext_unload("load-unload");
    return 0;
}
static const AgcExt g_loadunload = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "load-unload", "1", 0, loadunload_init, NULL, 0 };

/* apply_config fixtures: both are non-default, so the old is_default-only
 * filter would never have disabled them. */
static int g_cfg_off_init, g_cfg_on_init;
static int cfg_off_init(const AgcExtHost *host) { (void)host; g_cfg_off_init++; return 0; }
static int cfg_on_init(const AgcExtHost *host) { (void)host; g_cfg_on_init++; return 0; }
static const AgcExt g_cfg_off = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "cfg-off", "1", 0, cfg_off_init, NULL, 0 };
static const AgcExt g_cfg_on = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "cfg-on", "1", 0, cfg_on_init, NULL, 0 };

/* a bad-ABI descriptor: adopt must reject it */
static int bad_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    out->abi_version = 99;
    out->struct_size = sizeof *out;
    out->name = "bad-abi";
    out->init = NULL;
    return 0;
}

/* ------------------------------------------------------------ hook fixtures */

static int g_chain2_saw;
static int chain1(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point;
    if (res) *res = g_host->strdup_("{\"text\":\"one\"}");
    return 0;
}
static int chain2(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point;
    g_chain2_saw = g_host->json_get_str(payload, "text", NULL) != NULL &&
                   agentc_streq(g_host->json_get_str(payload, "text", ""), "one");
    if (res) *res = g_host->strdup_("{\"text\":\"two\"}");
    return 0;
}

static int veto_rewrite(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point;
    if (res) *res = g_host->strdup_("{\"input\":{\"x\":2}}");
    return 0;
}
static int veto_block(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point;
    if (g_host->json_get_int(payload, "input.x", -1) == 2) {
        if (res) *res = g_host->strdup_("{\"block\":true}");
        return 1;
    }
    return 0;
}

/* Adapter fixtures: one rewrites the input, one blocks with a reason, two
 * exercise `terminate` (with and without `block`). */
static int veto_adapter_rewrite(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = g_host->strdup_("{\"input\":{\"text\":\"rewritten\"}}");
    return 0;
}
static int veto_adapter_block(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = g_host->strdup_("{\"block\":true,\"reason\":\"nope\"}");
    return 1;
}
static int veto_adapter_terminate(void *ud, const char *point, const char *payload,
                                  char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = g_host->strdup_("{\"block\":true,\"reason\":\"stop\",\"terminate\":true}");
    return 1;
}
static int veto_adapter_terminate_only(void *ud, const char *point, const char *payload,
                                       char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = g_host->strdup_("{\"terminate\":true}");
    return 0;
}

static int g_first_calls;
static int first_win(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    g_first_calls++;
    if (res) *res = g_host->strdup_("{\"trusted\":\"yes\"}");
    return 1;
}
static int first_lose(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    g_first_calls++;
    if (res) *res = g_host->strdup_("{\"trusted\":\"no\"}");
    return 1;
}

static int fail_open_handler(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = NULL;
    return -1;
}

/* MERGE_REPLACE (project_trust): B sees A's result verbatim and B's result is
 * the whole accumulated value, not field-merged with A's. */
static char g_replace_payload[128];
static int replace_a(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = g_host->strdup_("{\"trusted\":\"yes\"}");
    return 0;
}
static int replace_b(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point;
    size_t n = 0;
    while (payload && payload[n] && n < sizeof g_replace_payload - 1) {
        g_replace_payload[n] = payload[n];
        n++;
    }
    g_replace_payload[n] = 0;
    if (res) *res = g_host->strdup_("{\"trusted\":\"no\",\"extra\":1}");
    return 1;
}

/* An observe handler returning 1, with or without a result, is
 * ignored and later observers still run. */
static int g_observe_one, g_observe_result, g_observe_later;
static int observe_one(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = NULL;
    g_observe_one++;
    return 1;
}
static int observe_result_one(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = g_host->strdup_("{\"bogus\":true}");
    g_observe_result++;
    return 1;
}
static int observe_later(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = NULL;
    g_observe_later++;
    return 0;
}

/* MERGE_FIELDS (message_end): a JSON null in a handler result must survive in
 * the accumulator as a delete marker while the effective payload hides it. */
static char g_overlay_payload[128];
static int overlay_a(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = g_host->strdup_("{\"usage\":null}");
    return 0;
}
static int overlay_b(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point;
    size_t n = 0;
    while (payload && payload[n] && n < sizeof g_overlay_payload - 1) {
        g_overlay_payload[n] = payload[n];
        n++;
    }
    g_overlay_payload[n] = 0;
    if (res) *res = g_host->strdup_("{}");
    return 0;
}

/* off() during dispatch must not affect the walk already in flight. */
static u64 g_off_late_handle;
static int g_off_early, g_off_late;
static int off_early(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = NULL;
    g_off_early++;
    g_host->off(g_off_late_handle);
    return 0;
}
static int off_late(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = NULL;
    g_off_late++;
    return 0;
}

static int section_hi(const AgcExtHost *host, void *ud, void *out) {
    (void)ud;
    host->out_write(out, "HI", 2);
    return 0;
}
static int section_lo(const AgcExtHost *host, void *ud, void *out) {
    (void)ud;
    host->out_write(out, "LO", 2);
    return 0;
}
static int sections_init(const AgcExtHost *host) {
    static const AgcExtSection hi = { sizeof(AgcExtSection), "hi", 10, NULL, section_hi };
    static const AgcExtSection lo = { sizeof(AgcExtSection), "lo", 5, NULL, section_lo };
    host->add_section(&hi);
    host->add_section(&lo);
    return 0;
}

/* ------------------------------------------------------------- defer/HTTP */

static int g_defer_calls;
static void defer_cb(void *ud) {
    (void)ud;
    g_defer_calls++;
}

static int g_http_calls;
static void http_cb(void *ud, int status, const char *headers_json,
                    const char *body, uint64_t body_len) {
    (void)ud; (void)status; (void)headers_json; (void)body; (void)body_len;
    g_http_calls++;
}

/* ---------------------------------------------------------------- context */

static int g_entry_calls;
static char g_entry_type[64], g_entry_data[160];
static void entry_sink(void *ud, const char *type, const char *data_json) {
    (void)ud;
    g_entry_calls++;
    size_t n = 0;
    while (type && type[n] && n < sizeof g_entry_type - 1) { g_entry_type[n] = type[n]; n++; }
    g_entry_type[n] = 0;
    n = 0;
    while (data_json && data_json[n] && n < sizeof g_entry_data - 1) { g_entry_data[n] = data_json[n]; n++; }
    g_entry_data[n] = 0;
}

/* --------------------------------------------------------- harden fixtures */

static char *run_ext_tool(const AgcTool *t, const char *call_id, const char *args,
                          bool *is_error);
static const AgcTool *find_tool(const char *name, AgcTool *buf, size_t cap,
                                size_t *n_out);

static int internal_noop_run(const AgcTool *self, const AgcToolCall *call,
                             AgcBuf *out, bool *is_error) {
    (void)self; (void)call; (void)out;
    if (is_error) *is_error = false;
    return 0;
}

/* descriptors registered directly must pass the same ABI/size gate as adopt */
static int g_bad_abi_loaded, g_bad_short_loaded;
static int bad_abi_init(const AgcExtHost *host) {
    (void)host;
    g_bad_abi_loaded = 1;
    return 0;
}
static int bad_short_init(const AgcExtHost *host) {
    (void)host;
    g_bad_short_loaded = 1;
    return 0;
}
static const AgcExt g_bad_register_abi = {
    AGENTC_EXT_ABI + 1, sizeof(AgcExt), "bad-register-abi", "1", 0,
    bad_abi_init, NULL, 0 };
static const AgcExt g_bad_register_short = {
    AGENTC_EXT_ABI, (uint32_t)offsetof(AgcExt, init), "bad-register-short", "1",
    0, bad_short_init, NULL, 0 };

/* malformed override results are handler failures, not chain values */
static int bad_result_text(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = g_host->strdup_("not json");
    return 0;
}
static int bad_result_array(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = g_host->strdup_("[]");
    return 0;
}
static int bad_result_empty(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = g_host->strdup_("");
    return 0;
}
static int g_invalid_later;
static int invalid_later(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = NULL;
    g_invalid_later = 1;
    return 0;
}

/* chain policy: a middle 1 is recorded but must not stop the walk */
static int g_chain_mid_ran, g_chain_last_ran;
static int chain_mid_handled(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    g_chain_mid_ran++;
    if (res) *res = g_host->strdup_("{\"a\":2}");
    return 1;
}
static int chain_last_ran_fn(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    g_chain_last_ran++;
    if (res) *res = g_host->strdup_("{\"b\":3}");
    return 0;
}

/* error/compact have no point-table entry: on() must reject them */
static AgcBuf g_agent_event_payload;
static int agent_event_rec(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point;
    if (res) *res = NULL;
    agentc_buf_clear(&g_agent_event_payload);
    if (payload) agentc_buf_cstr(&g_agent_event_payload, payload);
    return 0;
}

/* owner-tagged async work: unload/shutdown drops it without delivering */
static int g_owner_defer_calls, g_owner_http_calls;
static void owner_defer_cb(void *ud) { (void)ud; g_owner_defer_calls++; }
static void owner_http_cb(void *ud, int status, const char *headers_json,
                          const char *body, uint64_t body_len) {
    (void)ud; (void)status; (void)headers_json; (void)body; (void)body_len;
    g_owner_http_calls++;
}
static int owner_async_init(const AgcExtHost *host) {
    host->defer(host, owner_defer_cb, NULL);
    (void)host->http_request(host, "GET", "http://127.0.0.1:1/never", NULL,
                             NULL, 0, owner_http_cb, NULL);
    return 0;
}
static const AgcExt g_owner_async = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "owner-async", "1", 0, owner_async_init, NULL, 0 };

/* -------------------------------------- fixtures (ownership/overrun) */

/* One extension whose hook, tool and command each call set_status + defer; the
 * records must carry this owner and unload must drop all three kinds. */
static const AgcExtHost *g_owner_attr_host;
static int g_owner_attr_hook_calls;
static int g_owner_attr_defer_calls;
static void owner_attr_defer_cb(void *ud) { (void)ud; g_owner_attr_defer_calls++; }

static int owner_attr_hook(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = NULL;
    g_owner_attr_hook_calls++;
    g_owner_attr_host->set_status("owner-hook", "hook");
    g_owner_attr_host->defer(g_owner_attr_host, owner_attr_defer_cb, NULL);
    /* host->log is prefixed with [ext:owner-attrib] while this owner runs. */
    g_owner_attr_host->log(0, "owner-attrib: log inside hook");
    return 0;
}

static int owner_attr_tool_run(const AgcExtHost *host, const AgcExtTool *self,
                               const AgcExtToolCall *call, void *out, bool *is_error) {
    (void)self; (void)call; (void)out;
    if (is_error) *is_error = false;
    host->set_status("owner-tool", "tool");
    host->defer(host, owner_attr_defer_cb, NULL);
    host->log(0, "owner-attrib: log inside tool");
    return 0;
}

static void owner_attr_command(const AgcExtHost *host, void *ud, const char *args,
                               void *out) {
    (void)ud; (void)args; (void)out;
    host->set_status("owner-cmd", "cmd");
    host->defer(host, owner_attr_defer_cb, NULL);
    host->log(0, "owner-attrib: log inside command");
}

static int owner_attr_init(const AgcExtHost *host) {
    g_owner_attr_host = host;
    static const AgcExtTool tool = {
        .struct_size = sizeof(AgcExtTool),
        .flags = AGENTC_TOOL_READONLY,
        .name = "owner_attr_tool",
        .label = "Owner attr",
        .description = "ownership fixture",
        .parameters_json = fx_params,
        .run = owner_attr_tool_run,
    };
    static const AgcExtCommand cmd = {
        .struct_size = sizeof(AgcExtCommand),
        .name = "owner-attrib-cmd",
        .description = "ownership fixture command",
        .run = owner_attr_command,
    };
    host->add_tool(&tool);
    host->add_command(&cmd);
    host->on("agent_start", AGENTC_HOOK_OBSERVE, 0, owner_attr_hook, NULL);
    return 0;
}
static const AgcExt g_owner_attr = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "owner-attrib", "1", 0, owner_attr_init, NULL, 0 };

/* Deliberately slow handler on one observe and one override point: the point
 * blocks for the fail-closed one and the handler is disabled after 3 overruns. */
static int g_slow_observe_calls, g_slow_override_calls;
static int slow_busy_handler(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)payload;
    if (res) *res = NULL;
    if (agentc_streq(point, "agent_start")) g_slow_observe_calls++;
    else g_slow_override_calls++;
    i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    while (os_now_ns(OS_CLOCK_MONOTONIC) - t0 < 80000000LL) {
    }
    return 0;
}
static int slow_ext_init(const AgcExtHost *host) {
    host->on("agent_start", AGENTC_HOOK_OBSERVE, 0, slow_busy_handler, NULL);
    host->on("input", AGENTC_HOOK_OVERRIDE, 0, slow_busy_handler, NULL);
    return 0;
}
static const AgcExt g_slow_ext = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "slow-ext", "1", 0, slow_ext_init, NULL, 0 };

/* Recursively re-emits its own point; the depth cap must contain it. */
static int g_recursive_calls;
static int g_recursive_last_blocked;
static int recursive_handler(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)payload;
    if (res) *res = NULL;
    g_recursive_calls++;
    AgcExtResult r;
    agentc_memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    agentc_ext_host()->emit(agentc_ext_host(), point, "{}", &r);
    if (r.blocked) g_recursive_last_blocked = 1;
    agentc_ext_host()->free(r.result_json);
    return 0;
}

/* Two owners using one set_status key must not collide or clear each other. */
static int keyed_a_init(const AgcExtHost *host) {
    host->set_status("shared-key", "from-a");
    return 0;
}
static int keyed_b_init(const AgcExtHost *host) {
    host->set_status("shared-key", "from-b");
    return 0;
}
static const AgcExt g_keyed_a = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "keyed-a", "1", 0, keyed_a_init, NULL, 0 };
static const AgcExt g_keyed_b = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "keyed-b", "1", 0, keyed_b_init, NULL, 0 };

/* Fills the 8-key table, then tries a ninth: owner and key must be logged. */
static int status_full_init(const AgcExtHost *host) {
    char key[16];
    for (int i = 0; i < 8; i++) {
        agentc_snprintf(key, sizeof key, "fill-%d", i);
        host->set_status(key, "x");
    }
    host->set_status("fill-9", "x");
    return 0;
}
static const AgcExt g_status_full = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "status-full", "1", 0, status_full_init, NULL, 0 };

/* ABI capability gate: a descriptor demanding a newer/smaller host is refused. */
static int g_future_host_loaded, g_exact_host_loaded;
static int future_host_init(const AgcExtHost *host) { (void)host; g_future_host_loaded = 1; return 0; }
static int exact_host_init(const AgcExtHost *host) { (void)host; g_exact_host_loaded = 1; return 0; }
static const AgcExt g_future_host = {
    .abi_version = AGENTC_EXT_ABI,
    .struct_size = sizeof(AgcExt),
    .name = "future-host",
    .version = "1",
    .order = 0,
    .init = future_host_init,
    .shutdown = NULL,
    .required_host_size = (uint32_t)sizeof(AgcExtHost) + 64u,
};
static const AgcExt g_exact_host = {
    .abi_version = AGENTC_EXT_ABI,
    .struct_size = sizeof(AgcExt),
    .name = "exact-host",
    .version = "1",
    .order = 0,
    .init = exact_host_init,
    .shutdown = NULL,
    .required_host_size = (uint32_t)sizeof(AgcExtHost),
};

/* registration name validation */
static int g_invalid_name_loaded, g_long_name_loaded;
static int invalid_name_init(const AgcExtHost *host) { (void)host; g_invalid_name_loaded = 1; return 0; }
static int long_name_init(const AgcExtHost *host) { (void)host; g_long_name_loaded = 1; return 0; }
static const AgcExt g_invalid_name = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "Bad Name!", "1", 0, invalid_name_init, NULL, 0 };

/* A deferred callback that calls the pump again must not recurse: the callback
 * it queues behind itself runs after it returns, not inside the nested call. */
static int g_reentry_phase;
static int g_reentry_defer_ran;
static int g_reentry_inner_defer;
static int g_reentry_http_ran;
static int g_reentry_http_defer_ran;
static void reentry_defer_cb(void *ud) {
    (void)ud;
    if (g_reentry_phase == 1) g_reentry_inner_defer = 1;
    g_reentry_defer_ran++;
}
static void reentry_defer_a(void *ud) {
    (void)ud;
    g_reentry_phase = 1;
    agentc_ext_host()->defer(agentc_ext_host(), reentry_defer_cb, NULL);
    agentc_ext_pump();   /* nested: must be a no-op */
    g_reentry_phase = 2;
}
static void reentry_http_defer_cb(void *ud) {
    (void)ud;
    if (g_reentry_phase == 1) g_reentry_inner_defer = 1;
    g_reentry_http_defer_ran++;
}
static void reentry_http_cb(void *ud, int status, const char *headers_json,
                            const char *body, uint64_t body_len) {
    (void)ud; (void)status; (void)headers_json; (void)body; (void)body_len;
    g_reentry_http_ran++;
    agentc_ext_host()->defer(agentc_ext_host(), reentry_http_defer_cb, NULL);
    agentc_ext_pump();   /* nested: must be a no-op */
}

/* short result prefix for host->emit's struct_size honoring */
typedef struct {
    uint32_t struct_size;
    int handled;
    unsigned char tail[64];
} ShortResult;

/* ----------------------------------------------------------- harden tests */

static void test_register_validation(void) {
    g_bad_abi_loaded = g_bad_short_loaded = 0;
    agentc_ext_register(&g_bad_register_abi);
    agentc_ext_register(&g_bad_register_short);
    agentc_ext_load_all();
    check("register_bad_abi_refused", g_bad_abi_loaded == 0);
    check("register_short_struct_refused", g_bad_short_loaded == 0);
}

static void test_tool_stability(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();

    AgcTool before[4];
    const AgcTool *t0 = find_tool("fixture_echo", before, 4, NULL);
    if (!t0) {
        check("tool_stability_capture", false);
        return;
    }
    AgcTool captured = *t0;   /* what agentc_ext_tools hands the agent */

    for (int i = 0; i < 40; i++) {
        char name[24];
        agentc_snprintf(name, sizeof name, "bulk_tool_%02d", i);
        AgcTool it;
        agentc_memset(&it, 0, sizeof it);
        it.name = name;
        it.label = name;
        it.desc = "bulk tool";
        it.params_json = "{\"type\":\"object\"}";
        it.flags = AGENTC_TOOL_READONLY;
        it.run = internal_noop_run;
        if (agentc_ext_add_tool_internal(&it) != 0) {
            check("tool_stability_add", false);
            return;
        }
    }
    check("tool_stability_forced_growth", agentc_ext_tools(NULL, 0) == 41);

    bool err = false;
    char *r = run_ext_tool(&captured, "call-stable", "{\"check_ud\":true}", &err);
    check("tool_stability_exec", !err && cstr_eq(r, "ud-ok"));
    agentc_free(r);

    AgcTool after[64];
    const AgcTool *t1 = find_tool("fixture_echo", after, 64, NULL);
    check("tool_stability_same_ud",
          t1 && captured.ud == t1->ud && captured.run == t1->run);

    /* internal removal: drop one, prove the count, ENOENT on repeat, re-add */
    check("remove_tool_internal", agentc_ext_remove_tool_internal("bulk_tool_00") == 0);
    check("remove_tool_internal_count", agentc_ext_tools(NULL, 0) == 40);
    check("remove_tool_internal_missing",
          agentc_ext_remove_tool_internal("bulk_tool_00") == -2);
    AgcTool re;
    agentc_memset(&re, 0, sizeof re);
    re.name = "bulk_tool_00";
    re.label = "bulk tool";
    re.desc = "bulk tool";
    re.params_json = "{\"type\":\"object\"}";
    re.run = internal_noop_run;
    check("remove_tool_internal_readd",
          agentc_ext_add_tool_internal(&re) == 0 && agentc_ext_tools(NULL, 0) == 41);
}

static bool status_has(const char *text) {
    AgcStatusValue segs[8];
    size_t n = agentc_status_snapshot(segs, 8);
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(segs[i].text, text)) return true;
    return false;
}

static void test_status_staleness(void) {
    const AgcExtHost *h = agentc_ext_host();
    h->set_status("stale-key", "one");
    check("stale_set_visible", status_has("one"));
    u64 v = agentc_status_version();
    h->set_status("stale-key", "two");
    check("stale_update_version", agentc_status_version() > v);
    check("stale_update_text", status_has("two") && !status_has("one"));
    v = agentc_status_version();
    h->set_status("stale-key", NULL);
    check("stale_clear_null_version", agentc_status_version() > v);
    check("stale_clear_null_gone", !status_has("two"));
    h->set_status("stale-key", "three");
    v = agentc_status_version();
    h->set_status("stale-key", "");
    check("stale_clear_empty_version", agentc_status_version() > v);
    check("stale_clear_empty_gone", !status_has("three"));
    /* external reset wipes providers behind the registry's back; the next
     * mutation must re-register rather than trust a cached flag */
    h->set_status("stale-key", "four");
    agentc_status_reset();
    h->set_status("stale-key", "five");
    check("stale_after_reset", status_has("five"));
    v = agentc_status_version();
    h->set_status("stale-key", "");
    check("stale_after_reset_clear",
          agentc_status_version() > v && !status_has("five"));
}

static void test_host_emit_short_struct(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 0, first_win, NULL);

    AgcExtResult full;
    agentc_memset(&full, 0, sizeof full);
    full.struct_size = sizeof full;
    agentc_ext_host()->emit(agentc_ext_host(), "input", "{\"text\":\"x\"}", &full);
    check("emit_full_result", full.handled == 1 && full.result_json != NULL);
    agentc_free(full.result_json);

    ShortResult sr;
    agentc_memset(&sr, 0xAB, sizeof sr);
    sr.struct_size = (uint32_t)offsetof(AgcExtResult, blocked);   /* handled only */
    agentc_ext_host()->emit(agentc_ext_host(), "input", "{\"text\":\"x\"}",
                            (AgcExtResult *)&sr);
    bool tail_ok = true;
    for (size_t i = 0; i < sizeof sr.tail; i++)
        if (sr.tail[i] != 0xAB) tail_ok = false;
    check("emit_short_handled", sr.handled == 1);
    check("emit_short_no_overrun", tail_ok);

    ShortResult tiny;
    agentc_memset(&tiny, 0xCD, sizeof tiny);
    int pat;
    agentc_memset(&pat, 0xCD, sizeof pat);
    tiny.struct_size = (uint32_t)offsetof(AgcExtResult, handled);  /* nothing covered */
    agentc_ext_host()->emit(agentc_ext_host(), "input", "{\"text\":\"x\"}",
                            (AgcExtResult *)&tiny);
    check("emit_tiny_untouched", tiny.handled == pat);
}

static void test_hook_invalid_result(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 0, bad_result_text, NULL);
    AgcExtResult r = agentc_ext_emit("input", "{\"text\":\"x\"}");
    check("invalid_result_text_blocked",
          r.handled == 1 && r.blocked == 1 && r.result_json == NULL);
    agentc_free(r.result_json);

    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 0, bad_result_array, NULL);
    r = agentc_ext_emit("input", "{\"text\":\"x\"}");
    check("invalid_result_array_blocked",
          r.handled == 1 && r.blocked == 1 && r.result_json == NULL);
    agentc_free(r.result_json);

    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 0, bad_result_empty, NULL);
    r = agentc_ext_emit("input", "{\"text\":\"x\"}");
    check("invalid_result_empty_blocked",
          r.handled == 1 && r.blocked == 1 && r.result_json == NULL);
    agentc_free(r.result_json);

    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    g_invalid_later = 0;
    agentc_ext_host()->on("agent_start", AGENTC_HOOK_OBSERVE, 0, bad_result_text, NULL);
    agentc_ext_host()->on("agent_start", AGENTC_HOOK_OBSERVE, 1, invalid_later, NULL);
    r = agentc_ext_emit("agent_start", "{}");
    check("invalid_result_observe_fail_open", r.blocked == 0 && g_invalid_later == 1);
    agentc_free(r.result_json);
}

static void test_hook_chain_handled(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    g_chain_mid_ran = g_chain_last_ran = 0;
    agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 0, chain1, NULL);
    agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 1, chain_mid_handled, NULL);
    agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 2, chain_last_ran_fn, NULL);
    AgcExtResult r = agentc_ext_emit("input", "{\"text\":\"zero\"}");
    check("chain_handled_middle_runs", g_chain_mid_ran == 1 && g_chain_last_ran == 1);
    check("chain_handled_merged",
          r.handled == 1 && r.result_json &&
              agentc_str_str(r.result_json, "\"text\":\"one\"") != NULL &&
              agentc_str_str(r.result_json, "\"a\":2") != NULL &&
              agentc_str_str(r.result_json, "\"b\":3") != NULL);
    agentc_free(r.result_json);
}

static void test_merge_fields(void) {
    char *m = agentc_ext_merge_fields(
        "{\"role\":\"assistant\",\"content\":\"old\",\"usage\":{\"in\":1},\"stop_reason\":\"stop\"}",
        "{\"content\":\"new\",\"usage\":null}");
    check("merge_fields_replace_delete",
          m && cstr_eq(m, "{\"role\":\"assistant\",\"stop_reason\":\"stop\",\"content\":\"new\"}"));
    agentc_free(m);
    m = agentc_ext_merge_fields(NULL, "{\"a\":1}");
    check("merge_fields_null_base", m && cstr_eq(m, "{\"a\":1}"));
    agentc_free(m);
    /* A patch that is not an object leaves the base object untouched (ext.h
     * promises a JSON object, never a bare null/number/string). */
    m = agentc_ext_merge_fields("{\"a\":1}", "null");
    check("merge_fields_non_object_patch", m && cstr_eq(m, "{\"a\":1}"));
    agentc_free(m);
    m = agentc_ext_merge_fields("{\"a\":1}", "not json");
    check("merge_fields_bad_patch", m && cstr_eq(m, "{\"a\":1}"));
    agentc_free(m);
}

static void test_bridge_undefined_events(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    check("bridge_error_point_rejected",
          agentc_ext_host()->on("error", AGENTC_HOOK_OBSERVE, 0,
                                fx_on_event, NULL) == 0);
    check("bridge_compact_point_rejected",
          agentc_ext_host()->on("compact", AGENTC_HOOK_OBSERVE, 0,
                                fx_on_event, NULL) == 0);
    agentc_ext_emit_agent_event(AGENTC_EV_ERROR, "boom");
    agentc_ext_emit_agent_event(AGENTC_EV_COMPACT, NULL);
    check("bridge_error_unmapped", !agentc_ext_wants("error"));
    check("bridge_compact_unmapped", !agentc_ext_wants("compact"));
    /* turn_end is a direct override now: the typed bridge must not fan it out */
    g_typed_events = 0;
    agentc_ext_emit_agent_event(AGENTC_EV_TURN_END, NULL);
    check("bridge_turn_end_unmapped", g_typed_events == 0);
    /* a still-mapped observe point keeps working, wants-guarded */
    uint64_t h =
        agentc_ext_host()->on("turn_start", AGENTC_HOOK_OBSERVE, 0, fx_turn_start, NULL);
    g_turn_start_events = 0;
    agentc_ext_emit_agent_event(AGENTC_EV_TURN_START, NULL);
    check("bridge_turn_start_mapped", g_turn_start_events == 1);
    agentc_ext_host()->off(h);
    /* turn_start / agent_end carry their int data (NULL-safe) */
    uint64_t th = agentc_ext_host()->on("turn_start", AGENTC_HOOK_OBSERVE, 0,
                                        agent_event_rec, NULL);
    int turn = 3;
    agentc_ext_emit_agent_event(AGENTC_EV_TURN_START, &turn);
    check("bridge_turn_start_index",
          g_agent_event_payload.p &&
              agentc_streq((const char *)g_agent_event_payload.p, "{\"turn_index\":3}"));
    agentc_ext_host()->off(th);
    uint64_t eh =
        agentc_ext_host()->on("agent_end", AGENTC_HOOK_OBSERVE, 0, agent_event_rec, NULL);
    int stop = AGENTC_STOP_TOOLUSE;
    agentc_ext_emit_agent_event(AGENTC_EV_AGENT_END, &stop);
    check("bridge_agent_end_stop",
          g_agent_event_payload.p &&
              agentc_streq((const char *)g_agent_event_payload.p,
                           "{\"stop_reason\":\"tool_use\"}"));
    agentc_ext_emit_agent_event(AGENTC_EV_AGENT_END, NULL);
    check("bridge_agent_end_null_safe",
          g_agent_event_payload.p &&
              agentc_streq((const char *)g_agent_event_payload.p, "{}"));
    agentc_ext_host()->off(eh);
    agentc_buf_free(&g_agent_event_payload);
}

static void test_owner_async_cleanup(void) {
    agentc_ext_shutdown();
    g_owner_defer_calls = g_owner_http_calls = 0;
    agentc_ext_register(&g_owner_async);
    agentc_ext_load_all();
    agentc_ext_unload("owner-async");
    agentc_ext_pump();
    check("owner_defer_dropped_on_unload", g_owner_defer_calls == 0);
    check("owner_http_dropped_on_unload", g_owner_http_calls == 0);

    /* shutdown with a queued defer must not hang/crash, nor deliver later */
    agentc_ext_register(&g_owner_async);
    agentc_ext_load_all();
    agentc_ext_shutdown();
    agentc_ext_pump();
    check("owner_defer_dropped_on_shutdown", g_owner_defer_calls == 0);
    check("owner_http_dropped_on_shutdown", g_owner_http_calls == 0);
}

static void test_owner_attribution(void) {
    agentc_ext_shutdown();
    g_owner_attr_hook_calls = g_owner_attr_defer_calls = 0;
    agentc_ext_register(&g_owner_attr);
    agentc_ext_load_all();
    check("owner_hook_registered", agentc_ext_wants("agent_start"));
    AgcExtResult r = agentc_ext_emit("agent_start", "{}");
    agentc_free(r.result_json);
    check("owner_hook_ran", g_owner_attr_hook_calls == 1);
    check("owner_hook_status", status_has("hook"));

    AgcTool tools[8];
    const AgcTool *t = find_tool("owner_attr_tool", tools, 8, NULL);
    bool err = false;
    char *out = t ? run_ext_tool(t, "owner", "{}", &err) : NULL;
    agentc_free(out);
    check("owner_tool_status", status_has("tool"));

    char *cr = agentc_ext_run_command("owner-attrib-cmd", "");
    agentc_free(cr);
    check("owner_cmd_status", status_has("cmd"));

    agentc_ext_pump();
    check("owner_defer_from_all_three", g_owner_attr_defer_calls == 3);

    /* a fourth callback is queued behind the owner; unload drops it unrun */
    r = agentc_ext_emit("agent_start", "{}");
    agentc_free(r.result_json);
    check("owner_defer_queued_not_run", g_owner_attr_defer_calls == 3);
    agentc_ext_unload("owner-attrib");
    agentc_ext_pump();
    check("owner_defer_dropped_by_unload", g_owner_attr_defer_calls == 3);
    check("owner_status_dropped_by_unload",
          !status_has("hook") && !status_has("tool") && !status_has("cmd"));
    check("owner_hooks_dropped_by_unload", !agentc_ext_wants("agent_start"));
    check("owner_tools_dropped_by_unload",
          find_tool("owner_attr_tool", tools, 8, NULL) == NULL);
}

static void test_dirty_on_unload(void) {
    agentc_ext_shutdown();
    agentc_ext_clear_dirty();
    agentc_ext_register(&g_owner_attr);
    agentc_ext_load_all();
    agentc_ext_clear_dirty();
    agentc_ext_unload("owner-attrib");
    check("dirty_owner_tool_removal", agentc_ext_dirty());
    agentc_ext_clear_dirty();
}

static void test_wants_fast_path(void) {
    agentc_ext_shutdown();
    check("wants_zero_hooks", !agentc_ext_wants("turn_start"));
    uint64_t h = agentc_ext_host()->on("turn_start", AGENTC_HOOK_OBSERVE, 0,
                                       fx_turn_start, NULL);
    check("wants_with_hook", agentc_ext_wants("turn_start"));
    agentc_ext_host()->off(h);
    check("wants_after_off", !agentc_ext_wants("turn_start"));
    check("wants_null_point", !agentc_ext_wants(NULL));
}

static void test_unknown_point(void) {
    agentc_ext_shutdown();
    check("unknown_point_rejected",
          agentc_ext_host()->on("no_such_point", AGENTC_HOOK_OBSERVE, 0,
                                fx_on_event, NULL) == 0);
    check("unknown_point_not_wanted", !agentc_ext_wants("no_such_point"));
    AgcExtResult r = agentc_ext_emit("no_such_point", "{}");
    check("unknown_point_emit_empty", r.blocked == 0 && r.result_json == NULL);
    agentc_free(r.result_json);
}

/* `session_before_switch` is an override-first point with a replace
 * result and fail-closed failure mode; OBSERVE subscriptions are rejected and
 * the first handler short-circuits. */
static int g_sbswitch_first_calls, g_sbswitch_second_calls;
static int sbswitch_first(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    g_sbswitch_first_calls++;
    if (res) *res = g_host->strdup_("{\"cancel\":true}");
    return 1;
}
static int sbswitch_second(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    g_sbswitch_second_calls++;
    if (res) *res = NULL;
    return 0;
}

static void test_session_before_switch_point(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    check("sbswitch_observe_rejected",
          agentc_ext_host()->on("session_before_switch", AGENTC_HOOK_OBSERVE, 0,
                                fx_on_event, NULL) == 0);
    g_sbswitch_first_calls = g_sbswitch_second_calls = 0;
    agentc_ext_host()->on("session_before_switch", AGENTC_HOOK_OVERRIDE, 0,
                          sbswitch_first, NULL);
    agentc_ext_host()->on("session_before_switch", AGENTC_HOOK_OVERRIDE, 1,
                          sbswitch_second, NULL);
    check("sbswitch_wanted", agentc_ext_wants("session_before_switch"));
    AgcExtResult r = agentc_ext_emit("session_before_switch", "{\"reason\":\"new\"}");
    check("sbswitch_first_short_circuit",
          g_sbswitch_first_calls == 1 && g_sbswitch_second_calls == 0 &&
              r.handled == 1 && r.result_json && cstr_eq(r.result_json, "{\"cancel\":true}"));
    agentc_free(r.result_json);
}

static void test_overrun_disable(void) {
    agentc_ext_shutdown();
    g_slow_observe_calls = g_slow_override_calls = 0;
    agentc_ext_register(&g_slow_ext);
    agentc_ext_load_all();

    /* observe: each overrun is dropped and only the third disables */
    check("overrun_observe_wants", agentc_ext_wants("agent_start"));
    for (int i = 0; i < 3; i++) {
        AgcExtResult r = agentc_ext_emit("agent_start", "{}");
        check(i == 0 ? "overrun_observe_open1"
                     : (i == 1 ? "overrun_observe_open2" : "overrun_observe_open3"),
              r.blocked == 0);
        agentc_free(r.result_json);
        check(i < 2 ? "overrun_observe_still_wanted" : "overrun_observe_disabled",
              i < 2 ? agentc_ext_wants("agent_start") : !agentc_ext_wants("agent_start"));
    }
    check("overrun_observe_calls", g_slow_observe_calls == 3);
    AgcExtResult r = agentc_ext_emit("agent_start", "{}");
    agentc_free(r.result_json);
    check("overrun_observe_skipped_after_disable", g_slow_observe_calls == 3);

    /* fail-closed: every overrun blocks the occurrence; the third disables */
    check("overrun_override_wants", agentc_ext_wants("input"));
    for (int i = 0; i < 3; i++) {
        r = agentc_ext_emit("input", "{}");
        check(i == 0 ? "overrun_override_blocks1"
                     : (i == 1 ? "overrun_override_blocks2" : "overrun_override_blocks3"),
              r.blocked == 1 && r.handled == 1);
        agentc_free(r.result_json);
    }
    check("overrun_override_disabled",
          !agentc_ext_wants("input") && g_slow_override_calls == 3);
    r = agentc_ext_emit("input", "{}");
    check("overrun_override_unblocked_after_disable", r.blocked == 0);
    agentc_free(r.result_json);
}

static void test_recursion_guard(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();

    /* 32 is AGENTC_EXT_MAX_EMIT_DEPTH (private to registry.c) */
    agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 0, recursive_handler, NULL);
    g_recursive_calls = 0;
    g_recursive_last_blocked = 0;
    AgcExtResult r = agentc_ext_emit("input", "{}");
    agentc_free(r.result_json);
    check("recursion_fail_closed_capped", g_recursive_calls == 32);
    check("recursion_overflow_blocks", g_recursive_last_blocked == 1);

    uint64_t h = agentc_ext_host()->on("agent_start", AGENTC_HOOK_OBSERVE, 0,
                                       recursive_handler, NULL);
    g_recursive_calls = 0;
    g_recursive_last_blocked = 0;
    r = agentc_ext_emit("agent_start", "{}");
    agentc_free(r.result_json);
    check("recursion_observe_capped", g_recursive_calls == 32);
    check("recursion_overflow_empty", g_recursive_last_blocked == 0);
    agentc_ext_host()->off(h);
}

static void test_status_keying(void) {
    agentc_ext_shutdown();
    agentc_ext_register(&g_keyed_a);
    agentc_ext_register(&g_keyed_b);
    agentc_ext_load_all();
    check("status_same_key_two_owners", status_has("from-a") && status_has("from-b"));
    agentc_ext_unload("keyed-a");
    check("status_owner_a_clear_keeps_b", !status_has("from-a") && status_has("from-b"));
    agentc_ext_unload("keyed-b");
    check("status_owner_b_cleared", !status_has("from-b"));

    /* the ninth live key under one owner is rejected with a named log */
    agentc_ext_shutdown();
    agentc_ext_register(&g_status_full);
    agentc_ext_load_all();
    AgcStatusValue segs[16];
    size_t n = agentc_status_snapshot(segs, 16);
    size_t filled = 0;
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(segs[i].text, "x")) filled++;
    check("status_table_full_keeps_8", filled == 8);
    agentc_ext_unload("status-full");
    n = agentc_status_snapshot(segs, 16);
    filled = 0;
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(segs[i].text, "x")) filled++;
    check("status_table_full_cleared_on_unload", filled == 0);
}

static void test_register_gates(void) {
    g_invalid_name_loaded = g_future_host_loaded = g_exact_host_loaded = 0;
    g_long_name_loaded = 0;
    agentc_ext_register(&g_invalid_name);
    agentc_ext_register(&g_future_host);
    agentc_ext_register(&g_exact_host);
    /* an over-long name is refused before it reaches the descriptor table */
    static char long_name[80];
    for (size_t i = 0; i < sizeof long_name - 1; i++) long_name[i] = 'a';
    long_name[sizeof long_name - 1] = 0;
    AgcExt long_ext;
    agentc_memset(&long_ext, 0, sizeof long_ext);
    long_ext.abi_version = AGENTC_EXT_ABI;
    long_ext.struct_size = sizeof long_ext;
    long_ext.name = long_name;
    long_ext.version = "1";
    long_ext.init = long_name_init;
    agentc_ext_register(&long_ext);
    agentc_ext_load_all();
    check("register_invalid_name_refused", g_invalid_name_loaded == 0);
    check("register_long_name_refused", g_long_name_loaded == 0);
    check("register_required_host_too_new_refused", g_future_host_loaded == 0);
    check("register_required_host_exact_ok", g_exact_host_loaded == 1);
    /* keep the fixture loaded for the tests that follow */
    agentc_ext_unload("exact-host");
}

static void test_abi_gates(void) {
    AgcExtTool t;
    agentc_memset(&t, 0, sizeof t);
    t.struct_size = (uint32_t)offsetof(AgcExtTool, run);
    check("field_ok_rejects_short", !AGENTC_EXT_FIELD_OK(&t, AgcExtTool, run));
    t.struct_size = (uint32_t)(offsetof(AgcExtTool, run) + sizeof t.run);
    check("field_ok_accepts_covered",
          AGENTC_EXT_FIELD_OK(&t, AgcExtTool, run) &&
              AGENTC_EXT_FIELD_OK(&t, AgcExtTool, prompt_guidelines));
    check("field_ok_rejects_null",
          !AGENTC_EXT_FIELD_OK((const AgcExtTool *)NULL, AgcExtTool, run));
    const AgcExtHost *h = agentc_ext_host();
    check("host_has_services",
          AGENTC_EXT_HOST_HAS(h, alloc) && AGENTC_EXT_HOST_HAS(h, json_get_str));
    check("host_has_rejects_null",
          !AGENTC_EXT_HOST_HAS((const AgcExtHost *)NULL, alloc));
}

static void test_append_entry_cap(void) {
    const AgcExtHost *h = agentc_ext_host();
    agentc_ext_set_entry_sink(entry_sink, NULL);
    int before = g_entry_calls;
    char *big = h->alloc(9001);
    agentc_memset(big, 'a', 9000);
    big[0] = '{'; big[1] = '"'; big[2] = 'k'; big[3] = '"';
    big[4] = ':'; big[5] = '"';
    big[8998] = '"'; big[8999] = '}'; big[9000] = 0;
    h->append_entry("note", big);
    check("entry_over_cap_rejected", g_entry_calls == before);
    h->free(big);
    h->append_entry("note", "{\"k\":1}");
    check("entry_under_cap_ok", g_entry_calls == before + 1);
}

static void test_http_hygiene(void) {
    const AgcExtHost *h = agentc_ext_host();
    int before = g_http_calls;
    uint64_t id = h->http_request(h, "POST", "not-a-url", NULL, NULL, 5,
                                  http_cb, NULL);
    check("http_null_body_rejected", id == 0);
    id = h->http_request(h, "GET", "not-a-url", "{\"X\":12}", NULL, 0,
                         http_cb, NULL);
    check("http_malformed_headers_kept_request", id != 0);
    h->http_cancel(h, id);
    id = h->http_request(h, "GET", "not-a-url",
                         "{\"Host\":\"evil\",\"Content-Length\":\"9\","
                         "\"Transfer-Encoding\":\"chunked\",\"X-Ok\":\"1\"}",
                         NULL, 0, http_cb, NULL);
    check("http_forbidden_kept_request", id != 0);
    h->http_cancel(h, id);
    id = h->http_request(h, "GET EVIL", "not-a-url", NULL, NULL, 0,
                         http_cb, NULL);
    check("http_nontoken_method_rejected", id == 0);
    id = h->http_request(h, "GET\r\nHost: evil", "not-a-url", NULL, NULL, 0,
                         http_cb, NULL);
    check("http_crlf_method_rejected", id == 0);
    id = h->http_request(h, "M-SEARCH", "not-a-url", NULL, NULL, 0,
                         http_cb, NULL);
    check("http_token_method_kept", id != 0);
    h->http_cancel(h, id);
    agentc_ext_pump();
    check("http_hygiene_no_callbacks", g_http_calls == before);
}

/* ------------------------------------------------- in-flight HTTP cancel */

/* Mock-script writer: queue `n` server->client bytes as `data HEX`. */
static void mock_push_bytes(AgcBuf *s, const void *p, size_t n) {
    static const char hex[] = "0123456789abcdef";
    const u8 *q = p;
    agentc_buf_cstr(s, "data ");
    for (size_t i = 0; i < n; i++) {
        agentc_buf_byte(s, (u8)hex[q[i] >> 4]);
        agentc_buf_byte(s, (u8)hex[q[i] & 0xf]);
    }
    agentc_buf_byte(s, '\n');
}

static int g_cancel_calls;
static int g_cancel_status;
static uint64_t g_cancel_body_len;
static char g_cancel_body[32];
static uint64_t g_cancel_id;
static const AgcExtHost *g_cancel_host;

static void cancel_http_cb(void *ud, int status, const char *headers_json,
                           const char *body, uint64_t body_len) {
    (void)ud; (void)headers_json;
    g_cancel_calls++;
    g_cancel_status = status;
    g_cancel_body_len = body_len;
    size_t n = body_len < sizeof g_cancel_body - 1 ? (size_t)body_len
                                                   : sizeof g_cancel_body - 1;
    if (body && n) agentc_memcpy(g_cancel_body, body, n);
    g_cancel_body[n] = 0;
}

/* Runs inside agentc_http_run's poll wait: the request is already in flight. */
static void cancel_poll_hook(void *ud, int timeout_ms) {
    (void)ud; (void)timeout_ms;
    if (g_cancel_id && g_cancel_host) {
        g_cancel_host->http_cancel(g_cancel_host, g_cancel_id);
        g_cancel_id = 0;
    }
}

/* http_cancel must interrupt a request already inside agentc_http_run.
 * The mock delivers a partial body and then stalls; the poll hook cancels, so
 * the callback sees -ECANCELED with the bytes received before the cancel. */
static void test_http_cancel_inflight(void) {
    const AgcExtHost *h = agentc_ext_host();
    agentc_ext_shutdown();
    agentc_rm_rf(TEST_ROOT);

    AgcBuf script = { 0 };
    agentc_buf_cstr(&script, "dns api.test 203.0.113.5\n");
    static const char resp[] =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: 100\r\n\r\npartial";
    mock_push_bytes(&script, resp, sizeof resp - 1);
    agentc_buf_cstr(&script, "eagain\n");
    check("http_inflight.mock_write",
          agentc_write_file_atomic(TEST_ROOT "/cancel.mock", script.p, script.len, 0644) == 0);
    agentc_buf_free(&script);
    check("http_inflight.mock_load", agentc_mock_load(TEST_ROOT "/cancel.mock") == 0);

    g_cancel_calls = 0;
    g_cancel_status = 0;
    g_cancel_body_len = 0;
    g_cancel_body[0] = 0;
    g_cancel_host = h;
    agentc_http_set_poll_hook(cancel_poll_hook, NULL);
    g_cancel_id = h->http_request(h, "GET", "http://api.test/x", NULL, NULL, 0,
                                  cancel_http_cb, NULL);
    check("http_inflight.queued", g_cancel_id != 0);
    agentc_ext_pump();
    agentc_http_set_poll_hook(NULL, NULL);
    check("http_inflight.cancelled", g_cancel_calls == 1 && g_cancel_status == -125);
    check("http_inflight.partial_body",
          g_cancel_body_len == 7 && cstr_eq(g_cancel_body, "partial"));

    agentc_mock_reset();
    agentc_rm_rf(TEST_ROOT);
}

static void test_pump_reentrancy(void) {
    const AgcExtHost *h = agentc_ext_host();
    agentc_ext_shutdown();
    g_reentry_phase = 0;
    g_reentry_defer_ran = g_reentry_inner_defer = 0;
    g_reentry_http_ran = g_reentry_http_defer_ran = 0;

    /* a deferred callback that pumps again must not drain the queue inside
     * itself: the callback it queues runs after it returns */
    h->defer(h, reentry_defer_a, NULL);
    agentc_ext_pump();
    check("pump_reentry_outer_once", g_reentry_defer_ran == 1);
    check("pump_reentry_inner_dropped", g_reentry_inner_defer == 0);
    check("pump_reentry_phase_after_return", g_reentry_phase == 2);

    /* an HTTP callback that pumps again must not dispatch itself twice or run
     * its defer inline */
    (void)h->http_request(h, "GET", "not-a-url", NULL, NULL, 0, reentry_http_cb, NULL);
    agentc_ext_pump();
    check("pump_reentry_http_once", g_reentry_http_ran == 1);
    check("pump_reentry_http_defer_waits", g_reentry_http_defer_ran == 0);
    agentc_ext_pump();
    check("pump_reentry_http_defer_next_pump", g_reentry_http_defer_ran == 1);
}

/* ------------------------------------------------------------------- main */

static char *run_ext_tool(const AgcTool *t, const char *call_id, const char *args,
                          bool *is_error) {
    AgcToolCall call = { call_id, t->name, args ? args : "{}", NULL };
    AgcBuf out = { 0 };
    bool err = false;
    int rc = t->run(t, &call, &out, &err);
    if (is_error) *is_error = err || rc < 0;
    if (!out.p) agentc_buf_cstr(&out, "");
    return (char *)out.p;
}

static const AgcTool *find_tool(const char *name, AgcTool *buf, size_t cap, size_t *n_out) {
    size_t n = agentc_ext_tools(buf, cap);
    if (n_out) *n_out = n;
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(buf[i].name, name)) return &buf[i];
    return NULL;
}

static void test_host_basics(void) {
    const AgcExtHost *h = agentc_ext_host();
    check("host_abi", h && h->abi_version == AGENTC_EXT_ABI &&
                          h->struct_size == sizeof(AgcExtHost));
    u8 *p = h->alloc(37);
    bool zeroed = p != NULL;
    for (size_t i = 0; i < 37; i++) if (p[i]) zeroed = false;
    check("alloc_zeroed", zeroed);
    h->free(p);
    h->free(NULL);
    char *s = h->strdup_("agentc");
    check("strdup", cstr_eq(s, "agentc"));
    h->free(s);
    char *esc = h->json_escape("a\"b\n");
    check("json_escape", cstr_eq(esc, "a\\\"b\\n"));
    h->free(esc);
    h->log(0, "fixture: host log");
}

static void test_json_helpers(void) {
    const AgcExtHost *h = agentc_ext_host();
    const char *json = "{\"a\":{\"b\":\"hi\"},\"n\":42,\"flag\":true,\"arr\":[10,\"x\"]}";
    check("json_str", cstr_eq(h->json_get_str(json, "a.b", "?"), "hi"));
    check("json_int", h->json_get_int(json, "n", 0) == 42);
    check("json_bool", h->json_get_bool(json, "flag", 0) == 1);
    check("json_path_arr", cstr_eq(h->json_get_str(json, "arr.1", "?"), "x"));
    check("json_path_bad", cstr_eq(h->json_get_str(json, "arr.9", "dflt"), "dflt"));
}

static void test_registration(void) {
    check("adopt_fixture", agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY) == 0);
    check("adopt_duplicate", agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY) == 0);
    check("adopt_bad_abi", agentc_ext_adopt("bad-abi", bad_entry) == -22);
    agentc_ext_load_all();
    AgcTool tools[8];
    size_t n = 0;
    const AgcTool *t = find_tool("fixture_echo", tools, 8, &n);
    check("tool_count", n >= 1);
    check("tool_present", t != NULL);
    check("tool_name", t && cstr_eq(t->name, "fixture_echo"));
    check("tool_label", t && cstr_eq(t->label, "Fixture echo"));
    check("tool_flags", t && (t->flags & AGENTC_TOOL_READONLY) != 0 &&
                            (t->flags & AGENTC_TOOL_SEQUENTIAL) != 0);
    check("tools_max0", agentc_ext_tools(NULL, 0) == n);
}

static void test_tools(void) {
    AgcTool tools[8];
    const AgcTool *t = find_tool("fixture_echo", tools, 8, NULL);
    if (!t) { check("tool_present2", false); return; }
    bool err = true;
    char *r = run_ext_tool(t, "call-1", "{\"text\":\"hi\"}", &err);
    check("exec_text", !err && cstr_eq(r, "hi"));
    agentc_free(r);
    r = run_ext_tool(t, "call-1", "{}", &err);
    check("exec_error", err && cstr_eq(r, "error: text required"));
    agentc_free(r);
    r = run_ext_tool(t, "call-42", "{\"echo_call_id\":true}", &err);
    check("exec_call_id", !err && cstr_eq(r, "call-42"));
    agentc_free(r);
    volatile bool cancel = false;
    AgcToolCall cc = { "c", t->name, "{\"cancel_check\":true}", &cancel };
    AgcBuf cb = { 0 };
    (void)t->run(t, &cc, &cb, &err);
    check("exec_not_cancelled", !err && cstr_eq((char *)cb.p, "running"));
    agentc_buf_free(&cb);
    cancel = true;
    AgcToolCall cc2 = { "c", t->name, "{\"cancel_check\":true}", &cancel };
    AgcBuf cb2 = { 0 };
    (void)t->run(t, &cc2, &cb2, &err);
    check("exec_cancelled", !err && cstr_eq((char *)cb2.p, "cancelled"));
    agentc_buf_free(&cb2);
}

static void test_commands(void) {
    AgcExtCommandInfo cmds[4];
    size_t n = agentc_ext_commands(cmds, 4);
    check("command_count", n == 1);
    check("command_name", n == 1 && cstr_eq(cmds[0].name, "fx-echo"));
    AgcExtCommand small = { .struct_size = 0, .name = "too-small", .run = fx_command };
    agentc_ext_host()->add_command(&small);
    check("command_struct_too_small", agentc_ext_commands(NULL, 0) == n);
    char *res = agentc_ext_run_command("fx-echo", "abc");
    check("command_run", res && agentc_str_str(res, "\"args\":\"abc\"") != NULL);
    agentc_free(res);
    check("command_missing", agentc_ext_run_command("nope", "") == NULL);
}

static void test_status(void) {
    AgcStatusValue segs[8];
    size_t n = agentc_status_snapshot(segs, 8);
    bool found = false, pushed = false;
    for (size_t i = 0; i < n; i++) {
        if (agentc_streq(segs[i].text, "fixture-status")) found = true;
        if (agentc_streq(segs[i].text, "on")) pushed = true;
    }
    check("status_fixture", found);
    check("status_set_status", pushed);
    /* Unload drops the auto-clearing segment with the owner's registrations. */
    agentc_ext_unload("fixture");
    n = agentc_status_snapshot(segs, 8);
    bool gone = true;
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(segs[i].text, "on")) gone = false;
    check("status_set_status_cleared", gone);
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
}

static void test_events(void) {
    check("wants_registered", agentc_ext_wants("tool_call"));
    check("wants_unregistered", !agentc_ext_wants("definitely_not_a_point"));
    AgcExtResult r = agentc_ext_emit("tool_call", "{\"input\":{\"veto\":false}}");
    check("event_pass", r.blocked == 0 && r.handled == 0);
    agentc_free(r.result_json);
    r = agentc_ext_emit("tool_call", "{\"input\":{\"veto\":true}}");
    check("event_veto", r.blocked == 1 && r.handled == 1);
    agentc_free(r.result_json);

    uint64_t h =
        agentc_ext_host()->on("turn_start", AGENTC_HOOK_OBSERVE, 0, fx_turn_start, NULL);
    g_turn_start_events = 0;
    agentc_events_set_quiet(true);
    agentc_events_emit(NULL, AGENTC_EV_TURN_START, NULL);
    bool typed_quiet = g_turn_start_events == 0;
    agentc_events_set_quiet(false);
    agentc_events_emit(NULL, AGENTC_EV_TURN_START, NULL);
    check("ext.event.quiet-suppresses-bridge", typed_quiet && g_turn_start_events == 1);
    agentc_ext_host()->off(h);
}

static void test_context(void) {
    const AgcExtHost *h = agentc_ext_host();
    AgcExtContext ctx = {
        .cwd = "/tmp/agentc-ext-cwd",
        .session_id = "sess-1",
        .session_file = "/tmp/agentc-ext-cwd/session.jsonl",
        .system_prompt = "SYSTEM",
    };
    agentc_ext_set_context(&ctx);
    check("ctx_cwd", cstr_eq(h->cwd(h), "/tmp/agentc-ext-cwd"));
    check("ctx_session_id", cstr_eq(h->session_id(h), "sess-1"));
    check("ctx_session_file", cstr_eq(h->session_file(h), "/tmp/agentc-ext-cwd/session.jsonl"));
    check("ctx_system_prompt", cstr_eq(h->system_prompt(h), "SYSTEM"));
    AgcExtContext clear = { .session_file = "" };
    agentc_ext_set_context(&clear);
    check("ctx_session_file_clear", h->session_file(h) == NULL);
    check("ctx_keeps_other", cstr_eq(h->cwd(h), "/tmp/agentc-ext-cwd"));
}

static void test_append_entry(void) {
    const AgcExtHost *h = agentc_ext_host();
    agentc_ext_set_entry_sink(entry_sink, NULL);
    h->append_entry("note", "{\"k\":1}");
    check("entry_sink", g_entry_calls == 1 && cstr_eq(g_entry_type, "note") &&
                            cstr_eq(g_entry_data, "{\"k\":1}"));
    h->append_entry("note", "[1,2]");
    h->append_entry("note", "\"scalar\"");
    h->append_entry("note", "not json");
    check("entry_reject_non_object", g_entry_calls == 1 && cstr_eq(g_entry_data, "{\"k\":1}"));
    /* the core-facing entry point validates and delivers exactly like the
     * vtable slot the core no longer calls directly */
    agentc_ext_append_entry("core-note", "{\"core\":true}");
    check("entry_core_sink", g_entry_calls == 2 && cstr_eq(g_entry_type, "core-note") &&
                                 cstr_eq(g_entry_data, "{\"core\":true}"));
    agentc_ext_append_entry("bad type!", "{\"k\":1}");
    agentc_ext_append_entry("core-note", "[1]");
    check("entry_core_validation", g_entry_calls == 2);
}

static int g_model_calls;
static int model_sink(void *ud, const char *provider, const char *model) {
    (void)ud; (void)provider; (void)model;
    g_model_calls++;
    return 1;
}
static int g_thinking_calls;
static char g_thinking_level[16];
static void thinking_sink(void *ud, const char *level) {
    (void)ud;
    g_thinking_calls++;
    size_t n = 0;
    while (level && level[n] && n < sizeof g_thinking_level - 1) {
        g_thinking_level[n] = level[n];
        n++;
    }
    g_thinking_level[n] = 0;
}
static void test_model(void) {
    const AgcExtHost *h = agentc_ext_host();
    agentc_ext_set_model_sink(model_sink, NULL);
    check("set_model", h->set_model("anthropic", "claude-sonnet") == 1);
    check("model_sink", g_model_calls == 1);
    agentc_ext_set_model_sink(NULL, NULL);
    check("set_model_unset", h->set_model("anthropic", "claude-sonnet") == -38);
    agentc_ext_set_thinking_sink(thinking_sink, NULL);
    h->set_thinking("high");
    check("set_thinking", g_thinking_calls == 1 && cstr_eq(g_thinking_level, "high"));
    agentc_ext_set_thinking_sink(NULL, NULL);
}

static void test_defer_and_http(void) {
    const AgcExtHost *h = agentc_ext_host();
    h->defer(h, defer_cb, NULL);
    agentc_ext_pump();
    check("defer_ran", g_defer_calls == 1);
    uint64_t id = h->http_request(h, "GET", "http://127.0.0.1:1/never", NULL,
                                  NULL, 0, http_cb, NULL);
    check("http_id", id != 0);
    h->http_cancel(h, id);
    agentc_ext_pump();
    check("http_cancelled", g_http_calls == 0);
}

/* ---- registry behaviour: order, hooks, sections, rollback --------------- */

static void test_order(void) {
    g_order_n = 0;
    agentc_ext_register(&g_order_a);
    agentc_ext_register(&g_order_b);
    agentc_ext_load_all();
    check("registry_order", g_order_n == 2 && g_order_log[0] == 2 && g_order_log[1] == 1);
}

/* init() may register another extension; load_all must initialize the tail. */
static void test_load_all_appended(void) {
    agentc_ext_shutdown();
    g_chain_inits = 0;
    agentc_ext_register(&g_chain_ext);
    agentc_ext_load_all();
    check("load_all_appended_all",
          g_chain_inits == 3 && agentc_ext_is_loaded("chain-0") &&
              agentc_ext_is_loaded("chain-1") && agentc_ext_is_loaded("chain-2"));
}

/* The appended-tail loop is bounded: a chain longer than the cap stops with a
 * log and leaves the rest pending. */
static void test_load_all_cap(void) {
    agentc_ext_shutdown();
    g_cap_inits = 0;
    agentc_ext_register(&g_cap_ext);
    agentc_ext_load_all();
    check("load_all_cap_rounds", g_cap_inits == 8);
    check("load_all_cap_last_loaded", agentc_ext_is_loaded("cap-7"));
    check("load_all_cap_pending_not_loaded", !agentc_ext_is_loaded("cap-8"));
}

/* A shutdown() that appends must not skip or leak the appended record. */
static void test_shutdown_append_live(void) {
    agentc_ext_shutdown();
    size_t before = agentc_mem_live();
    g_shutdown_append_calls = 0;
    agentc_ext_register(&g_shutdown_append);
    agentc_ext_load_all();
    check("shutdown_append_loaded", agentc_ext_is_loaded("shutdown-append"));
    agentc_ext_shutdown();
    check("shutdown_append_ran", g_shutdown_append_calls == 1);
    check("shutdown_append_no_leak", agentc_mem_live() == before);
    agentc_ext_load_all();
    check("shutdown_append_tail_cleared", !agentc_ext_is_loaded("shutdown-tail"));
}

/* An init() that calls agentc_ext_shutdown() must not free the registry under
 * the running load loop: the call is a logged no-op while g_loading > 0, the
 * extension still loads, the registry stays enumerable, and the later normal
 * shutdown tears everything down without leaking. An init() that unloads its
 * own record must be skipped by the post-init `!e->used` re-check. */
static void test_shutdown_during_load(void) {
    agentc_ext_shutdown();
    size_t before = agentc_mem_live();
    g_loadshutdown_calls = 0;

    agentc_ext_register(&g_loadshutdown);
    agentc_ext_register(&g_loadunload);
    agentc_ext_load_all();

    check("loadshutdown_called", g_loadshutdown_calls == 1);
    check("loadshutdown_loaded", agentc_ext_is_loaded("load-shutdown"));
    check("loadunload_absent", !agentc_ext_is_loaded("load-unload"));
    AgcExtInfo info[8];
    check("loadshutdown_registry_intact", agentc_ext_describe(info, 8) == 1);

    agentc_ext_shutdown();
    check("loadshutdown_later_shutdown", agentc_ext_describe(info, 8) == 0);
    check("loadshutdown_mem_baseline", agentc_mem_live() == before);
}

static void test_hook_chain(void) {
    g_chain2_saw = 0;
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 0, chain1, NULL);
    agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 0, chain2, NULL);
    AgcExtResult r = agentc_ext_emit("input", "{\"text\":\"zero\"}");
    check("hook_chain_merge", g_chain2_saw == 1 && r.result_json &&
                              agentc_str_str(r.result_json, "\"text\":\"two\"") != NULL);
    agentc_free(r.result_json);
}

static void test_hook_chain_veto(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    agentc_ext_host()->on("tool_call", AGENTC_HOOK_OVERRIDE, 0, veto_rewrite, NULL);
    agentc_ext_host()->on("tool_call", AGENTC_HOOK_OVERRIDE, 1, veto_block, NULL);
    AgcExtResult r = agentc_ext_emit("tool_call", "{\"input\":{\"x\":1}}");
    check("hook_chain_veto", r.blocked == 1 && r.result_json &&
                             agentc_str_str(r.result_json, "\"block\":true") != NULL);
    agentc_free(r.result_json);
}

/* The agent tool veto adapter consumes the emit result for the core: block +
 * reason, and a rewritten input serialized back to JSON. */
static void test_tool_veto_adapter(void) {
    const AgcExtHost *h = agentc_ext_host();
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    h->on("tool_call", AGENTC_HOOK_OVERRIDE, 0, veto_adapter_rewrite, NULL);
    AgcToolVetoDecision d;
    agentc_memset(&d, 0, sizeof d);
    agentc_ext_tool_veto(NULL, "call-1", "fixture_echo", "{\"text\":\"orig\"}", &d);
    check("veto_adapter_rewrite_no_block", d.block == 0);
    check("veto_adapter_rewrite_args",
          d.args_json && cstr_eq(d.args_json, "{\"text\":\"rewritten\"}"));
    check("veto_adapter_rewrite_reason_none", d.reason == NULL);
    agentc_free(d.args_json);
    agentc_free(d.reason);

    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    h->on("tool_call", AGENTC_HOOK_OVERRIDE, 0, veto_adapter_block, NULL);
    agentc_memset(&d, 0, sizeof d);
    agentc_ext_tool_veto(NULL, "call-2", "fixture_echo", "{}", &d);
    check("veto_adapter_block", d.block == 1);
    check("veto_adapter_block_reason", d.reason && cstr_eq(d.reason, "nope"));
    check("veto_adapter_block_no_args", d.args_json == NULL);
    agentc_free(d.args_json);
    agentc_free(d.reason);

    /* terminate: parsed from the result and honored only alongside block */
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    h->on("tool_call", AGENTC_HOOK_OVERRIDE, 0, veto_adapter_terminate, NULL);
    agentc_memset(&d, 0, sizeof d);
    agentc_ext_tool_veto(NULL, "call-4", "fixture_echo", "{}", &d);
    check("veto_adapter_terminate_block", d.block == 1 && d.terminate == 1);
    check("veto_adapter_terminate_reason", d.reason && cstr_eq(d.reason, "stop"));
    agentc_free(d.args_json);
    agentc_free(d.reason);

    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    h->on("tool_call", AGENTC_HOOK_OVERRIDE, 0, veto_adapter_terminate_only, NULL);
    agentc_memset(&d, 0, sizeof d);
    agentc_ext_tool_veto(NULL, "call-5", "fixture_echo", "{}", &d);
    check("veto_adapter_terminate_gated", d.block == 0 && d.terminate == 0);
    agentc_free(d.args_json);
    agentc_free(d.reason);

    /* Unwatched point: the guard leaves the decision untouched. */
    agentc_ext_shutdown();
    agentc_memset(&d, 0, sizeof d);
    agentc_ext_tool_veto(NULL, "call-3", "fixture_echo", "{}", &d);
    check("veto_adapter_unwatched",
          d.block == 0 && d.reason == NULL && d.args_json == NULL);
    agentc_free(d.args_json);
    agentc_free(d.reason);
}

static char g_tie_order[8];
static int g_tie_n;
static int tie_first(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = NULL;
    if (g_tie_n < (int)sizeof g_tie_order) g_tie_order[g_tie_n] = 'a';
    g_tie_n++;
    return 0;
}
static int tie_second(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = NULL;
    if (g_tie_n < (int)sizeof g_tie_order) g_tie_order[g_tie_n] = 'b';
    g_tie_n++;
    return 0;
}

/* Equal priorities must keep registration order; a lower priority registered
 * later still runs first. */
static void test_hook_priority_ties(void) {
    const AgcExtHost *h = agentc_ext_host();
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    g_tie_n = 0;
    agentc_memset(g_tie_order, 0, sizeof g_tie_order);
    h->on("agent_start", AGENTC_HOOK_OBSERVE, 5, tie_first, NULL);
    h->on("agent_start", AGENTC_HOOK_OBSERVE, 5, tie_second, NULL);
    AgcExtResult r = agentc_ext_emit("agent_start", "{}");
    check("hook_tie_registration_order",
          g_tie_n == 2 && g_tie_order[0] == 'a' && g_tie_order[1] == 'b');
    agentc_free(r.result_json);
    agentc_ext_shutdown();

    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    g_tie_n = 0;
    agentc_memset(g_tie_order, 0, sizeof g_tie_order);
    h->on("agent_start", AGENTC_HOOK_OBSERVE, 9, tie_second, NULL);
    h->on("agent_start", AGENTC_HOOK_OBSERVE, -3, tie_first, NULL);
    r = agentc_ext_emit("agent_start", "{}");
    check("hook_priority_over_seq",
          g_tie_n == 2 && g_tie_order[0] == 'a' && g_tie_order[1] == 'b');
    agentc_free(r.result_json);
    agentc_ext_shutdown();
}

static void test_hook_first(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    g_first_calls = 0;
    agentc_ext_host()->on("project_trust", AGENTC_HOOK_OVERRIDE, 0, first_win, NULL);
    agentc_ext_host()->on("project_trust", AGENTC_HOOK_OVERRIDE, 1, first_lose, NULL);
    AgcExtResult r = agentc_ext_emit("project_trust", "{\"cwd\":\"/x\"}");
    check("hook_first_short_circuit", g_first_calls == 1 && r.handled == 1 &&
                                      r.result_json &&
                                      agentc_str_str(r.result_json, "\"yes\"") != NULL);
    agentc_free(r.result_json);
}

static void test_hook_merge_replace(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    g_replace_payload[0] = 0;
    agentc_ext_host()->on("project_trust", AGENTC_HOOK_OVERRIDE, 0, replace_a, NULL);
    agentc_ext_host()->on("project_trust", AGENTC_HOOK_OVERRIDE, 1, replace_b, NULL);
    AgcExtResult r = agentc_ext_emit("project_trust", "{\"cwd\":\"/x\"}");
    check("hook_merge_replace_payload",
          cstr_eq(g_replace_payload, "{\"trusted\":\"yes\"}"));
    check("hook_merge_replace_result",
          r.handled == 1 && cstr_eq(r.result_json, "{\"trusted\":\"no\",\"extra\":1}"));
    agentc_free(r.result_json);
}

static void test_hook_fields_overlay(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    g_overlay_payload[0] = 0;
    agentc_ext_host()->on("message_end", AGENTC_HOOK_OVERRIDE, 0, overlay_a, NULL);
    agentc_ext_host()->on("message_end", AGENTC_HOOK_OVERRIDE, 1, overlay_b, NULL);
    AgcExtResult r =
        agentc_ext_emit("message_end", "{\"role\":\"assistant\",\"usage\":{\"a\":1}}");
    check("hook_fields_overlay_payload",
          cstr_eq(g_overlay_payload, "{\"role\":\"assistant\"}"));
    check("hook_fields_overlay_result",
          r.handled == 0 && r.result_json && cstr_eq(r.result_json, "{\"usage\":null}"));
    agentc_free(r.result_json);
}

static void test_hook_caps(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    check("hook_caps_mismatch",
          agentc_ext_host()->on("tool_call", AGENTC_HOOK_OBSERVE, 0, fx_on_event, NULL) == 0);
}

static void test_hook_observe_gate(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    g_observe_one = g_observe_result = g_observe_later = 0;
    agentc_ext_host()->on("agent_start", AGENTC_HOOK_OBSERVE, 0, observe_one, NULL);
    agentc_ext_host()->on("agent_start", AGENTC_HOOK_OBSERVE, 1, observe_result_one, NULL);
    agentc_ext_host()->on("agent_start", AGENTC_HOOK_OBSERVE, 2, observe_later, NULL);
    AgcExtResult r = agentc_ext_emit("agent_start", "{}");
    check("hook_observe_return1_ignored",
          r.handled == 0 && g_observe_one == 1 && g_observe_result == 1 &&
          g_observe_later == 1);
    agentc_free(r.result_json);
}

static void test_hook_off_during_dispatch(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    g_off_early = g_off_late = 0;
    g_off_late_handle =
        agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 1, off_late, NULL);
    agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 0, off_early, NULL);
    AgcExtResult r = agentc_ext_emit("input", "{\"text\":\"x\"}");
    agentc_free(r.result_json);
    check("hook_off_during_dispatch", g_off_early == 1 && g_off_late == 1);
    r = agentc_ext_emit("input", "{\"text\":\"x\"}");
    agentc_free(r.result_json);
    check("hook_off_persisted", g_off_early == 2 && g_off_late == 1);
}

static void test_hook_fail_modes(void) {
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    agentc_ext_host()->on("agent_start", AGENTC_HOOK_OBSERVE, 0, fail_open_handler, NULL);
    AgcExtResult o = agentc_ext_emit("agent_start", "{}");
    check("hook_fail_open", o.blocked == 0);
    agentc_free(o.result_json);
    agentc_ext_host()->on("input", AGENTC_HOOK_OVERRIDE, 0, fail_open_handler, NULL);
    AgcExtResult c = agentc_ext_emit("input", "{}");
    check("hook_fail_closed", c.blocked == 1);
    agentc_free(c.result_json);
}

static void test_sections(void) {
    agentc_ext_shutdown();
    static const AgcExt ext = {
        AGENTC_EXT_ABI, sizeof(AgcExt), "sections", "1", 0, sections_init, NULL, 0 };
    agentc_ext_register(&ext);
    agentc_ext_load_all();
    AgcBuf b = { 0 };
    agentc_ext_render_sections(&b);
    check("section_order", b.p && cstr_eq((char *)b.p, "LOHI"));
    agentc_buf_free(&b);
    AgcExtSection small = { .struct_size = 0, .key = "too-small", .render = section_hi };
    agentc_ext_host()->add_section(&small);
    agentc_ext_render_sections(&b);
    check("section_struct_too_small", b.p && cstr_eq((char *)b.p, "LOHI"));
    agentc_buf_free(&b);
}

static void test_failed_init(void) {
    agentc_ext_shutdown();
    agentc_ext_register(&g_fail);
    size_t before = agentc_ext_tools(NULL, 0);
    AgcExtCommandInfo cmds_before[8];
    size_t ncmds_before = agentc_ext_commands(cmds_before, 8);
    AgcStatusValue segs_before[16];
    size_t nsegs_before = agentc_status_snapshot(segs_before, 16);
    AgcBuf sec_before = { 0 };
    agentc_ext_render_sections(&sec_before);
    g_fail_pump_calls = 0;

    agentc_ext_load_all();
    check("failed_init_rollback_tools", agentc_ext_tools(NULL, 0) == before);

    AgcExtCommandInfo cmds_after[8];
    size_t ncmds_after = agentc_ext_commands(cmds_after, 8);
    bool fail_cmd_gone = ncmds_after == ncmds_before;
    for (size_t i = 0; i < ncmds_after && fail_cmd_gone; i++)
        if (agentc_streq(cmds_after[i].name, "fail-cmd")) fail_cmd_gone = false;
    check("failed_init_rollback_commands", fail_cmd_gone);

    AgcStatusValue segs_after[16];
    check("failed_init_rollback_status",
          agentc_status_snapshot(segs_after, 16) == nsegs_before);

    AgcBuf sec_after = { 0 };
    agentc_ext_render_sections(&sec_after);
    check("failed_init_rollback_sections",
          sec_after.len == sec_before.len &&
              (sec_before.len == 0 ||
               agentc_memeq(sec_before.p, sec_after.p, sec_before.len)));

    agentc_ext_pump();
    check("failed_init_rollback_pumps", g_fail_pump_calls == 0);
    check("failed_init_shutdown", g_fail_shutdown_called == 1);
    agentc_buf_free(&sec_before);
    agentc_buf_free(&sec_after);
}

static void test_unload_order(void) {
    agentc_ext_shutdown();
    agentc_ext_register(&g_unload_order);
    agentc_ext_load_all();
    check("unload_order_registered",
          agentc_ext_tools(NULL, 0) == 1 && agentc_ext_wants("turn_start"));
    g_unload_tools_at_shutdown = (size_t)-1;
    g_unload_wants_at_shutdown = true;
    agentc_ext_unload("unload-order");
    check("unload_order_remove_before_shutdown",
          g_unload_tools_at_shutdown == 0 && !g_unload_wants_at_shutdown);
}

/* extensions.disabled is applied after register_linked and before load_all, so
 * it must drop a linked (non-default) extension by name, not only defaults. */
static void test_apply_config(void) {
    agentc_ext_shutdown();
    g_cfg_off_init = g_cfg_on_init = 0;
    AgcConfig cfg;
    agentc_memset(&cfg, 0, sizeof cfg);
    char *disabled[] = { "cfg-off", "builtin-tools", "builtin-context", "no-such-ext" };
    cfg.extensions_disabled = disabled;
    cfg.nextensions_disabled = sizeof disabled / sizeof disabled[0];
    agentc_ext_register(&g_cfg_off);
    agentc_ext_register(&g_cfg_on);
    agentc_ext_register_defaults(true);   /* builtin-tools, builtin-context */
    agentc_ext_apply_config(&cfg);
    agentc_ext_load_all();
    check("apply_config_nondefault_disabled", g_cfg_off_init == 0);
    check("apply_config_other_loaded", g_cfg_on_init == 1);
    check("apply_config_default_disabled", agentc_ext_tools(NULL, 0) == 0);
}

/* --------------------------------------------------- header helpers */

static void test_header_helpers(void) {
    /* to_json: escaping, whitespace trim, malformed line skipped, duplicate
     * names last-wins. */
    char *j = agentc_ext_headers_to_json("X-A: 1\r\n"
                                         "X-B: a\"b\\c\r\n"
                                         "Bogus line without a colon\r\n"
                                         "X-C:   spaced   \r\n"
                                         "X-A: 2\r\n");
    check("hdr_to_json",
          j && cstr_eq(j, "{\"X-B\":\"a\\\"b\\\\c\",\"X-C\":\"spaced\","
                          "\"X-A\":\"2\"}"));
    agentc_free(j);
    j = agentc_ext_headers_to_json("X-A: 1\r\nx-a: 2\r\n");
    check("hdr_to_json_ci_dup", j && cstr_eq(j, "{\"x-a\":\"2\"}"));
    agentc_free(j);
    j = agentc_ext_headers_to_json(NULL);
    check("hdr_to_json_empty", j && cstr_eq(j, "{}"));
    agentc_free(j);

    /* apply_patch: replace in place for every occurrence, order preserved;
     * the forbidden trio is dropped, not appended. */
    char *p1 = agentc_ext_headers_apply_patch("X-Keep: 1\r\n"
                                              "X-Patch: old\r\n"
                                              "X-Keep: 2\r\n",
                                              "{\"X-Patch\":\"new\","
                                              "\"Content-Length\":\"999\","
                                              "\"Host\":\"evil\","
                                              "\"Transfer-Encoding\":\"chunked\"}");
    check("hdr_patch_replace",
          p1 && cstr_eq(p1, "X-Keep: 1\r\nX-Patch: new\r\nX-Keep: 2\r\n"));
    agentc_free(p1);

    /* CR/LF in a value become spaces (the hdr_string sanitizer) */
    char *p2 = agentc_ext_headers_apply_patch("X-P: old\r\n",
                                              "{\"X-P\":\"a\\r\\nInjected: 1\"}");
    check("hdr_patch_crlf", p2 && cstr_eq(p2, "X-P: a  Injected: 1\r\n"));
    agentc_free(p2);

    /* null deletes every occurrence; unmatched keys append in patch order */
    char *p3 = agentc_ext_headers_apply_patch("X-Del: 1\r\nX-Keep: 2\r\nX-Del: 3\r\n",
                                              "{\"X-Del\":null,\"X-New\":\"n\","
                                              "\"X-Also\":\"m\"}");
    check("hdr_patch_delete_append",
          p3 && cstr_eq(p3, "X-Keep: 2\r\nX-New: n\r\nX-Also: m\r\n"));
    agentc_free(p3);

    /* patch keys match case-insensitively; the forbidden trio is rejected in
     * any case (mixed case must not smuggle a Content-Length through) */
    char *pc = agentc_ext_headers_apply_patch("X-Patch: old\r\n",
                                              "{\"x-PATCH\":\"new\"}");
    check("hdr_patch_ci_replace", pc && cstr_eq(pc, "X-Patch: new\r\n"));
    agentc_free(pc);
    char *pf = agentc_ext_headers_apply_patch("X-A: 1\r\n",
                                              "{\"content-LENGTH\":\"9\"}");
    check("hdr_patch_ci_forbidden", pf && cstr_eq(pf, "X-A: 1\r\n"));
    agentc_free(pf);

    /* duplicate patch keys: the last occurrence supplies the value */
    char *p4 = agentc_ext_headers_apply_patch("X-A: old\r\n",
                                              "{\"X-A\":\"1\",\"X-A\":\"2\"}");
    check("hdr_patch_dup_last", p4 && cstr_eq(p4, "X-A: 2\r\n"));
    agentc_free(p4);

    /* forbidden-only and empty patches still rebuild the block */
    char *p5 = agentc_ext_headers_apply_patch("X-A: 1\r\n", "{\"Host\":\"h\"}");
    check("hdr_patch_forbidden_only", p5 && cstr_eq(p5, "X-A: 1\r\n"));
    agentc_free(p5);
    char *p6 = agentc_ext_headers_apply_patch("X-A: 1\r\n", "{}");
    check("hdr_patch_empty", p6 && cstr_eq(p6, "X-A: 1\r\n"));
    agentc_free(p6);

    /* bad patches: a number value, trailing garbage and a bad key all fail
     * closed (NULL), so the caller keeps its original block. */
    check("hdr_patch_bad_number",
          agentc_ext_headers_apply_patch("X-A: 1\r\n", "{\"X-A\":12}") == NULL);
    check("hdr_patch_bad_garbage",
          agentc_ext_headers_apply_patch("X-A: 1\r\n", "{\"X-A\":\"1\"} trailing") == NULL);
    check("hdr_patch_bad_key",
          agentc_ext_headers_apply_patch("X-A: 1\r\n", "{\"bad key\":\"1\"}") == NULL);
}

/* ------------------------------------- late mcp__ recompose selection */

static int late_mcp_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                        bool *is_error) {
    (void)self; (void)call;
    if (is_error) *is_error = false;
    agentc_buf_cstr(out, "late-mcp-ok");
    return 0;
}

static const AgcTool late_mcp_tool = {
    .name = "mcp__late__tool", .label = "Late MCP", .desc = "late mcp",
    .params_json = "{\"type\":\"object\"}", .flags = AGENTC_TOOL_READONLY,
    .run = late_mcp_run,
};

/* Minimal canned end_turn reply for the replay transport. */
static const char recompose_reply_final[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_r\",\"usage\":{"
    "\"input_tokens\":1,\"output_tokens\":0}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
    "\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
    "\"usage\":{\"output_tokens\":1}}\n\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n";

typedef struct {
    const char *reply;
    AgcBuf last_body;
} LateReplay;

static int late_replay_request(void *ud, const char *url, const char *headers,
                               const void *body, size_t body_len,
                               int (*on_chunk)(void *, const void *, size_t), void *u,
                               int timeout_ms, AgcBuf *record) {
    (void)url; (void)headers; (void)timeout_ms; (void)record;
    LateReplay *r = ud;
    agentc_buf_clear(&r->last_body);
    if (body && body_len) agentc_buf_push(&r->last_body, body, body_len);
    size_t len = agentc_strlen(r->reply);
    for (size_t off = 0; off < len; off += 7) {
        size_t n = len - off;
        if (n > 7) n = 7;
        if (on_chunk && on_chunk(u, r->reply + off, n)) return -125;
    }
    return 0;
}

/* The app-level recompose (`agentc_policy_recompose`) must keep the previous
 * table when the only --tools name is an absent deferred `mcp__` name, and
 * select it once the server-side tool appears late in the registry. */
static void test_late_mcp_recompose(void) {
    agentc_ext_shutdown();
    agentc_ext_clear_dirty();

    AgcConfig cfg;
    agentc_memset(&cfg, 0, sizeof cfg);
    ToolPolicy pol = { &cfg, "mcp__late__tool", false };

    LateReplay r;
    agentc_memset(&r, 0, sizeof r);
    r.reply = recompose_reply_final;

    AgcAgent *a = agentc_agent_new(agentc_prov_anthropic(), "claude-sonnet-4-5");
    agentc_agent_set_transport(a, (AgcTransport){ late_replay_request, &r, NULL });
    agentc_agent_set_system(a, "recompose-sys");
    AgcTool seed = { .name = "read", .label = "Read", .desc = "seed",
                     .params_json = "{\"type\":\"object\"}",
                     .flags = AGENTC_TOOL_READONLY, .run = late_mcp_run };
    agentc_agent_set_tools(a, &seed, 1);
    agentc_agent_test_no_backoff(a);

    /* still absent: the deferred mcp__ name must not empty the table */
    agentc_ext_host()->request_recompose(agentc_ext_host());
    agentc_policy_recompose(&pol, a);
    check("late_mcp.absent_submit", agentc_agent_submit(a, "go") == 0);
    check("late_mcp.absent_kept",
          r.last_body.p &&
              agentc_str_str((const char *)r.last_body.p, "\"name\":\"read\"") != NULL &&
              agentc_str_str((const char *)r.last_body.p, "mcp__late__tool") == NULL);

    /* the registry gains the late mcp tool: recompose selects it */
    check("late_mcp.add", agentc_ext_add_tool_internal(&late_mcp_tool) == 0);
    check("late_mcp.dirty", agentc_ext_dirty());
    agentc_policy_recompose(&pol, a);
    check("late_mcp.clean", !agentc_ext_dirty());
    check("late_mcp.present_submit", agentc_agent_submit(a, "again") == 0);
    check("late_mcp.present_selected",
          r.last_body.p &&
              agentc_str_str((const char *)r.last_body.p, "mcp__late__tool") != NULL &&
              agentc_str_str((const char *)r.last_body.p, "\"name\":\"read\"") == NULL);

    check("late_mcp.remove",
          agentc_ext_remove_tool_internal("mcp__late__tool") == 0);
    agentc_buf_free(&r.last_body);
    agentc_agent_free(a);
    agentc_ext_shutdown();
}

static void test_dirty(void) {
    agentc_ext_shutdown();
    agentc_ext_clear_dirty();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    check("dirty_public_add_tool", agentc_ext_dirty());
    agentc_ext_clear_dirty();
    check("dirty_after_clear", !agentc_ext_dirty());
    /* host->request_recompose is the explicit public dirty bit */
    agentc_ext_host()->request_recompose(agentc_ext_host());
    check("dirty_request_recompose", agentc_ext_dirty());
    agentc_ext_clear_dirty();
}

static void test_shutdown(size_t baseline) {
    agentc_ext_shutdown();
    check("shutdown_called", g_shutdown_called == 1);
    check("mem_baseline", agentc_mem_live() == baseline);
}

/* ------------------------------------------ harness async example */

/* The linked async_demo example exposes its per-tool counters through the
 * `async-demo-debug` command (the example must not become a link dependency of
 * the golden fixture binary, which does not link it), so these checks assert
 * the exact stop count and reason without importing any example symbol. */
typedef struct {
    AgcBuf out;
    bool is_error;
    int state;
    bool started, done;
} HxCtx;

static int hx_collect(void *ud, int what, const AgcJob *job) {
    HxCtx *c = ud;
    if (what == AGENTC_JOB_STARTED) {
        c->started = true;
        return 0;
    }
    if (job->out.len) agentc_buf_push(&c->out, job->out.p, job->out.len);
    c->is_error = job->is_error;
    c->state = job->state;
    c->done = true;
    return 0;
}

static void hx_run(const AgcTool *tool, const char *args,
                   const volatile bool *cancel, HxCtx *c) {
    agentc_memset(c, 0, sizeof *c);
    if (!tool) return;
    AgcToolCall call = { "hx-call", tool->name, args ? args : "{}", cancel };
    (void)agentc_tool_jobs_run(&tool, NULL, &call, 1, hx_collect, c);
}

static const char *hx_out(const HxCtx *c) {
    return c->out.p ? (const char *)c->out.p : "";
}

static const AgcTool *hx_find(const char *name, AgcTool *tools, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(tools[i].name, name)) return &tools[i];
    return NULL;
}

static char *hx_debug(const char *args) {
    return agentc_ext_run_command("async-demo-debug", args ? args : "{}");
}

/* `async_demo starts=1 steps=3 stops=1 reason=0` must appear verbatim. */
static bool hx_debug_is(const char *dbg, const char *tool, int starts, int steps,
                        int stops, int reason) {
    if (!dbg) return false;
    char want[128];
    agentc_snprintf(want, sizeof want, "%s starts=%d steps=%d stops=%d reason=%d",
                    tool, starts, steps, stops, reason);
    return agentc_str_str(dbg, want) != NULL;
}

/* The adapter's D-A9 pump runs after every running step; the harness uses it
 * to deliver cancellation through the call's cancel flag -- the same pointer
 * host->is_cancelled resolves for the extension. */
static volatile bool g_hx_cancel;
static bool g_hx_arm_cancel;
static int g_hx_pumps;

static void hx_pump(void *ud, int timeout_ms) {
    (void)ud;
    (void)timeout_ms;
    g_hx_pumps++;
    if (g_hx_arm_cancel) {
        g_hx_arm_cancel = false;
        g_hx_cancel = true;
    }
    agentc_ext_pump();
}

static bool harness_async_checks(void) {
    bool ok = true;
    AgcTool tools[16];
    size_t n = agentc_ext_tools(tools, 16);
    const AgcTool *demo = hx_find("async_demo", tools, n);
    const AgcTool *fatal = hx_find("async_fatal", tools, n);
    const AgcTool *slow = hx_find("async_slow", tools, n);
    const AgcTool *cancel = hx_find("async_cancel", tools, n);
    bool reg = demo && fatal && slow && cancel &&
               demo->run == NULL && demo->start && demo->step &&
               fatal->run == NULL && fatal->start && fatal->step &&
               slow->run == NULL && slow->start && slow->step &&
               cancel->run == NULL && cancel->start && cancel->step &&
               slow->timeout_ms == 1;
    agentc_outf("harness async_tools=%s\n", reg ? "ok" : "missing");
    ok = ok && reg;

    g_hx_cancel = false;
    g_hx_arm_cancel = false;
    g_hx_pumps = 0;
    agentc_pump_install(hx_pump, NULL);
    HxCtx c;

    /* completing run: incremental output, no error, one stop(FINISHED) */
    {
        char *r0 = hx_debug("{\"reset\":true}");
        agentc_free(r0);
        hx_run(demo, "{\"steps\":3}", NULL, &c);
        char *dbg = hx_debug("{}");
        bool pass = c.done && c.started && !c.is_error && c.state == 2 &&
                    agentc_streq(hx_out(&c), "step 0\nstep 1\nstep 2\n") &&
                    hx_debug_is(dbg, "async_demo", 1, 3, 1, AGENTC_EXT_TOOL_FINISHED);
        agentc_free(dbg);
        agentc_buf_free(&c.out);
        agentc_outf("harness async_demo=%s\n", pass ? "ok" : "missing");
        ok = ok && pass;
    }

    /* fail_at: the error is the tool result, delivered as stop(FINISHED) */
    {
        char *r0 = hx_debug("{\"reset\":true}");
        agentc_free(r0);
        hx_run(demo, "{\"steps\":5,\"fail_at\":2}", NULL, &c);
        char *dbg = hx_debug("{}");
        bool pass = c.done && c.started && c.is_error && c.state == 2 &&
                    agentc_streq(hx_out(&c),
                                 "step 0\nstep 1\nerror: demo failed at 2\n") &&
                    hx_debug_is(dbg, "async_demo", 1, 3, 1, AGENTC_EXT_TOOL_FINISHED);
        agentc_free(dbg);
        agentc_buf_free(&c.out);
        agentc_outf("harness async_error=%s\n", pass ? "ok" : "missing");
        ok = ok && pass;
    }

    /* fatal step: the driver replaces the staged output and stops ERROR */
    {
        char *r0 = hx_debug("{\"reset\":true}");
        agentc_free(r0);
        hx_run(fatal, "{}", NULL, &c);
        char *dbg = hx_debug("{}");
        bool pass = c.done && c.started && c.is_error && c.state == 2 &&
                    agentc_streq(hx_out(&c),
                                 "error: tool 'async_fatal' failed to run (-5)\n") &&
                    hx_debug_is(dbg, "async_fatal", 1, 3, 1, AGENTC_EXT_TOOL_ERROR);
        agentc_free(dbg);
        agentc_buf_free(&c.out);
        agentc_outf("harness async_fatal=%s\n", pass ? "ok" : "missing");
        ok = ok && pass;
    }

    /* timeout: one step runs, then the deadline stops TIMEOUT */
    {
        char *r0 = hx_debug("{\"reset\":true}");
        agentc_free(r0);
        hx_run(slow, "{}", NULL, &c);
        char *dbg = hx_debug("{}");
        bool pass = c.done && c.started && c.is_error && c.state == 2 &&
                    agentc_streq(hx_out(&c), "error: tool 'async_slow' timed out\n") &&
                    hx_debug_is(dbg, "async_slow", 1, 1, 1, AGENTC_EXT_TOOL_TIMEOUT);
        agentc_free(dbg);
        agentc_buf_free(&c.out);
        agentc_outf("harness async_timeout=%s\n", pass ? "ok" : "missing");
        ok = ok && pass;
    }

    /* cancel: the pump flips the call's cancel flag after the first step, the
     * driver cancels the running job, one stop(CANCELLED) keeps the partial
     * output */
    {
        char *r0 = hx_debug("{\"reset\":true}");
        agentc_free(r0);
        g_hx_cancel = false;
        g_hx_arm_cancel = true;
        hx_run(cancel, "{}", &g_hx_cancel, &c);
        g_hx_arm_cancel = false;
        char *dbg = hx_debug("{}");
        bool pass = c.done && c.started && c.is_error && c.state == 3 &&
                    agentc_streq(hx_out(&c), "cancel step 0\n") &&
                    hx_debug_is(dbg, "async_cancel", 1, 1, 1,
                                AGENTC_EXT_TOOL_CANCELLED);
        agentc_free(dbg);
        agentc_buf_free(&c.out);
        agentc_outf("harness async_cancel=%s\n", pass ? "ok" : "missing");
        ok = ok && pass;
    }

    /* The job driver checks the cancel flag before every step, so a
     * driver-delivered cancellation always preempts the extension's own poll
     * (above: polled=0). Drive the adapter's start/step directly with the
     * token already cancelled to cover the extension's documented poll branch
     * too: partial output + *is_error, stop(FINISHED). The harness acts as the
     * driver here, so it owns job.priv exactly as jobs_release would. */
    {
        char *r0 = hx_debug("{\"reset\":true}");
        agentc_free(r0);
        bool poll_ok = false;
        if (cancel) {
            AgcJob job;
            agentc_tool_job_init(&job);
            g_hx_cancel = true;   /* the token reads cancelled from step 0 */
            AgcToolCall call = { "hx-poll", "async_cancel", "{}", &g_hx_cancel };
            job.tool = cancel;
            job.call = call;
            job.index = 0;
            int rc = cancel->start(cancel, &call, &job);
            if (rc == 0) rc = cancel->step(cancel, &job);
            poll_ok = rc == 1 && job.is_error && job.out.p != NULL &&
                      agentc_streq((const char *)job.out.p, "cancelled at step 0\n");
            agentc_free(job.priv);   /* the driver owns priv */
            agentc_buf_free(&job.out);
        }
        g_hx_cancel = false;
        char *dbg = hx_debug("{}");
        poll_ok = poll_ok && hx_debug_is(dbg, "async_cancel", 1, 1, 1,
                                         AGENTC_EXT_TOOL_FINISHED) &&
                  agentc_str_str(dbg, "polled=1") != NULL;
        agentc_free(dbg);
        agentc_outf("harness async_cancel_poll=%s\n", poll_ok ? "ok" : "missing");
        ok = ok && poll_ok;
    }

    agentc_pump_install(NULL, NULL);
    if (g_hx_pumps == 0) {
        agentc_outf("harness async_pump=missing\n");
        ok = false;
    }
    return ok;
}

/* ------------------------------------------------------------ harness mode */

static int harness_main(void) {
    agentc_ext_register_linked();
    agentc_ext_load_all();
    AgcTool tools[8];
    size_t n = agentc_ext_tools(tools, 8);
    bool hello_ok = false, rust_ok = false;
    for (size_t i = 0; i < n; i++) {
        bool err = false;
        if (agentc_streq(tools[i].name, "hello_echo")) {
            char *r = run_ext_tool(&tools[i], "c", "{\"text\":\"from harness\"}", &err);
            hello_ok = !err && cstr_eq(r, "from harness");
            agentc_free(r);
        } else if (agentc_streq(tools[i].name, "rust_echo")) {
            char *r = run_ext_tool(&tools[i], "c", "{\"text\":\"from harness\"}", &err);
            rust_ok = !err && r && agentc_str_str(r, "rust_echo: from harness") != NULL;
            agentc_free(r);
        }
    }
    AgcExtResult veto = agentc_ext_emit("tool_call",
        "{\"tool_name\":\"bash\",\"input\":{\"veto\":true}}");
    AgcStatusValue segs[8];
    size_t ns = agentc_status_snapshot(segs, 8);
    bool hello_status = false;
    for (size_t i = 0; i < ns; i++)
        if (agentc_streq(segs[i].text, "hello")) hello_status = true;
    agentc_outf("harness hello_tool=%s\n", hello_ok ? "ok" : "missing");
    agentc_outf("harness hello_veto=%d\n", veto.blocked);
    agentc_outf("harness hello_status=%s\n", hello_status ? "ok" : "missing");
    agentc_outf("harness rust_tool=%s\n", rust_ok ? "ok" : "missing");

    /* the linked fake provider is a live registry row with a static
     * model, a sanitized request and a working canned stream. */
    agentc_test_setenv("FAKE_PROVIDER_API_KEY", "harness");
    bool fake_ok = false;
    const AgcProviderOps *fake_ops = agentc_provider_by_name("fake");
    const AgcModel *fake_model = agentc_model_find("fake", "fake-model-1");
    if (fake_ops && fake_ops->is_ext && fake_model && agentc_model_is_static(fake_model)) {
        const AgcProvider *fh = agentc_provider_handle((AgcProviderOps *)fake_ops);
        AgcRequest rq;
        agentc_memset(&rq, 0, sizeof rq);
        rq.provider = "fake";
        rq.model = "fake-model-1";
        rq.api_key = "harness-key";
        AgcBuf out = { 0 };
        int brc = fh ? fh->build_request(&out, &rq, NULL, "/chat") : -1;
        i64 split = brc == 0 ? agentc_str_find((const char *)out.p, out.len, "\r\n\r\n", 4)
                             : -1;
        if (brc == 0 && split >= 0) {
            char *head = agentc_strdup_len((const char *)out.p, (size_t)split);
            const char *body = (const char *)out.p + split + 4;
            fake_ok = agentc_str_str(head, "content-type: application/json") != NULL &&
                      agentc_str_str(head, "Host:") == NULL &&
                      agentc_str_str(head, "Authorization: Bearer harness-key") != NULL &&
                      agentc_str_str(body, "\"model\":\"fake-model-1\"") != NULL;
            agentc_free(head);
        }
        agentc_buf_free(&out);
        if (fake_ok) {
            fake_ok = false;
            AgcStreamState st;
            if (agentc_stream_state_init(fh, &st) == 0) {
                AgcMsg msg;
                agentc_memset(&msg, 0, sizeof msg);
                msg.role = AGENTC_ROLE_ASSISTANT;
                st.msg = &msg;
                static const char *events[] = {
                    "{\"kind\":\"text\",\"text\":\"hi\"}",
                    "{\"kind\":\"thinking\",\"text\":\"hmm\"}",
                    "{\"kind\":\"tool_call\",\"id\":\"c1\",\"name\":\"read\",\"args\":\"{}\"}",
                    "{\"kind\":\"usage\",\"input\":5,\"output\":6}",
                    "{\"kind\":\"stop\",\"reason\":\"tool_use\"}",
                };
                AgcSseEvent ev;
                agentc_memset(&ev, 0, sizeof ev);
                int mrc = 0;
                for (size_t i = 0; (i < sizeof events / sizeof events[0]) && mrc == 0; i++) {
                    ev.data = events[i];
                    ev.data_len = agentc_strlen(events[i]);
                    mrc = fh->map_sse(&st, &ev);
                }
                bool finished = fh->finish(&st) == 0 && st.stop_reason == AGENTC_STOP_TOOLUSE;
                fake_ok = mrc == 0 && finished && msg.nblocks == 3 &&
                          agentc_streq(msg.blocks[0].text, "hi") &&
                          agentc_streq(msg.blocks[1].text, "hmm") &&
                          agentc_streq(msg.blocks[2].tool_name, "read") &&
                          st.usage_input == 5 && st.usage_output == 6;
                agentc_stream_state_close(&st);
                agentc_msg_free(&msg);
            }
        }
    }
    agentc_outf("harness fake_provider=%s\n", fake_ok ? "ok" : "missing");
    /* The row-driven --list-models enumeration prints the linked provider's
     * static model offline; tests/ext.sh greps the line. */
    (void)agentc_setup_list_models(NULL, "fake", false, true);

    bool async_ok = harness_async_checks();

    agentc_free(veto.result_json);
    agentc_ext_shutdown();
    agentc_outf("harness done=1\n");
    return (hello_ok && veto.blocked == 1 && hello_status && fake_ok && async_ok) ? fails : 1;
}

/* ---------------------------------------------------------- async tools */

enum {
    AF_OK, AF_ERR, AF_FATAL, AF_TIMEOUT, AF_CANCEL,
    AF_UNLOAD, AF_SHUTDOWN, AF_REMOVE,
};

static int af_m_ok = AF_OK;
static int af_m_err = AF_ERR;
static int af_m_fatal = AF_FATAL;
static int af_m_timeout = AF_TIMEOUT;
static int af_m_cancel = AF_CANCEL;
static int af_m_unload = AF_UNLOAD;
static int af_m_shutdown = AF_SHUTDOWN;
static int af_m_remove = AF_REMOVE;

static int g_af_starts, g_af_steps, g_af_stops;
static int g_af_stop_reason[8];
static int g_af_stop_n;
static int g_af_bad_state;
static int g_af_stop_saw_cancel;
static volatile bool g_af_cancel_flag;

/* The extension-owned per-job state, allocated in start and freed in stop. */
typedef struct {
    u32 magic;
    int mode;
    int polls_left;
    bool deferred;
    const char *token;
} AsyncSt;

#define AF_STATE_MAGIC 0xAF00C0DEu

static void af_unload_cb(void *ud) { (void)ud; agentc_ext_unload("asyncfix"); }
static void af_shutdown_cb(void *ud) { (void)ud; agentc_ext_shutdown(); }
static void af_remove_cb(void *ud) {
    (void)ud;
    agentc_ext_remove_tool_internal("asyncfix.remove");
}

static int af_start(const AgcExtHost *host, const AgcExtTool *self,
                    const AgcExtToolCall *call, void *out, bool *is_error,
                    void **state) {
    int mode = *(const int *)self->ud;
    g_af_starts++;
    AsyncSt *st = host->alloc(sizeof *st);
    st->magic = AF_STATE_MAGIC;
    st->mode = mode;
    st->polls_left = 2;
    st->token = call ? call->signal_token : NULL;
    *state = st;
    if (mode == AF_ERR) {
        host->out_write(out, "boom", 4);
        if (is_error) *is_error = true;
        return 1;
    }
    if (is_error) *is_error = false;
    return 0;
}

static int af_step(const AgcExtHost *host, const AgcExtTool *self,
                   const AgcExtToolCall *call, void *state, void *out,
                   bool *is_error) {
    (void)call;
    (void)self;
    AsyncSt *st = state;
    if (!st || st->magic != AF_STATE_MAGIC) {
        g_af_bad_state = 1;
        return -22;
    }
    g_af_steps++;
    switch (st->mode) {
    case AF_FATAL:
        host->out_write(out, "partial", 7);
        return -22;
    case AF_TIMEOUT:
        return 0;
    case AF_CANCEL:
        g_af_cancel_flag = true;
        return 0;
    case AF_UNLOAD:
        if (!st->deferred) { st->deferred = true; host->defer(host, af_unload_cb, NULL); }
        return 0;
    case AF_SHUTDOWN:
        if (!st->deferred) { st->deferred = true; host->defer(host, af_shutdown_cb, NULL); }
        return 0;
    case AF_REMOVE:
        if (!st->deferred) { st->deferred = true; host->defer(host, af_remove_cb, NULL); }
        return 0;
    default:
        if (st->mode == AF_OK && st->polls_left == 2)
            host->log(0, "async step");   /* prefix proves the owner is restored */
        host->out_write(out, "chunk", 5);
        st->polls_left--;
        if (st->polls_left <= 0) {
            if (is_error) *is_error = false;
            return 1;
        }
        return 0;
    }
}

static void af_stop(const AgcExtHost *host, const AgcExtTool *self, void *state, int reason) {
    (void)self;
    AsyncSt *st = state;
    g_af_stops++;
    if (g_af_stop_n < 8) g_af_stop_reason[g_af_stop_n++] = reason;
    if (!st || st->magic != AF_STATE_MAGIC) {
        g_af_bad_state = 1;
        return;
    }
    if (st->mode == AF_OK)
        host->log(0, "async stop");   /* prefix proves the owner is restored */
    /* the signal token must still resolve while stop runs */
    if (st->mode == AF_CANCEL && host->is_cancelled(host, st->token))
        g_af_stop_saw_cancel = 1;
    host->free(st);
}

static int af_sync_run(const AgcExtHost *host, const AgcExtTool *self,
                       const AgcExtToolCall *call, void *out, bool *is_error) {
    (void)self;
    (void)call;
    if (is_error) *is_error = false;
    host->out_write(out, "sync-ran", 8);
    return 0;
}

static const AgcExtTool af_tools[] = {
    { .struct_size = sizeof(AgcExtTool), .name = "asyncfix.ok", .label = "ok",
      .description = "finish normally", .parameters_json = "{}",
      .ud = &af_m_ok, .start = af_start, .step = af_step, .stop = af_stop },
    { .struct_size = sizeof(AgcExtTool), .name = "asyncfix.err", .label = "err",
      .description = "error result", .parameters_json = "{}",
      .ud = &af_m_err, .start = af_start, .step = af_step, .stop = af_stop },
    { .struct_size = sizeof(AgcExtTool), .name = "asyncfix.fatal", .label = "fatal",
      .description = "fatal step", .parameters_json = "{}",
      .ud = &af_m_fatal, .start = af_start, .step = af_step, .stop = af_stop },
    { .struct_size = sizeof(AgcExtTool), .name = "asyncfix.timeout", .label = "timeout",
      .description = "never finishes", .parameters_json = "{}", .timeout_ms = 5,
      .ud = &af_m_timeout, .start = af_start, .step = af_step, .stop = af_stop },
    { .struct_size = sizeof(AgcExtTool), .name = "asyncfix.cancel", .label = "cancel",
      .description = "cancelled from step", .parameters_json = "{}",
      .ud = &af_m_cancel, .start = af_start, .step = af_step, .stop = af_stop },
    { .struct_size = sizeof(AgcExtTool), .name = "asyncfix.unload", .label = "unload",
      .description = "unloads mid-job", .parameters_json = "{}",
      .ud = &af_m_unload, .start = af_start, .step = af_step, .stop = af_stop },
    { .struct_size = sizeof(AgcExtTool), .name = "asyncfix.shutdown", .label = "shutdown",
      .description = "shuts down mid-job", .parameters_json = "{}",
      .ud = &af_m_shutdown, .start = af_start, .step = af_step, .stop = af_stop },
    { .struct_size = sizeof(AgcExtTool), .name = "asyncfix.remove", .label = "remove",
      .description = "removed mid-job", .parameters_json = "{}",
      .ud = &af_m_remove, .start = af_start, .step = af_step, .stop = af_stop },
};

static int asyncfix_init(const AgcExtHost *host) {
    for (size_t i = 0; i < sizeof af_tools / sizeof af_tools[0]; i++)
        host->add_tool(&af_tools[i]);
    return 0;
}

static int asyncfix_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    out->abi_version = AGENTC_EXT_ABI;
    out->struct_size = sizeof *out;
    out->name = "asyncfix";
    out->version = "0.1";
    out->order = 0;
    out->init = asyncfix_init;
    return 0;
}

/* ------------------------------------------------ start-window
 *
 * A ToolRec must never be freed while its own start() is still on the stack.
 * These tools tear the extension (or their own record) down from inside
 * start(), through an emit -> hook callback and through the internal API.
 */

enum { AF_TD_NONE, AF_TD_UNLOAD, AF_TD_SHUTDOWN };
static int g_af_teardown;

static int af_teardown_hook(void *ud, const char *point, const char *payload,
                            char **res) {
    (void)ud;
    (void)point;
    (void)payload;
    if (res) *res = NULL;
    int td = g_af_teardown;
    g_af_teardown = AF_TD_NONE;
    if (td == AF_TD_UNLOAD) agentc_ext_unload("asyncstart");
    else if (td == AF_TD_SHUTDOWN) agentc_ext_shutdown();
    return 0;
}

enum {
    AS_START_UNLOAD = 32, AS_START_SHUTDOWN, AS_START_DELETE, AS_START_FATAL,
    AS_SHUTDOWN_TIMEOUT,
};

static int as_m_unload = AS_START_UNLOAD;
static int as_m_shutdown = AS_START_SHUTDOWN;
static int as_m_delete = AS_START_DELETE;
static int as_m_fatal = AS_START_FATAL;
static int as_m_timeout = AS_SHUTDOWN_TIMEOUT;

static int as_start(const AgcExtHost *host, const AgcExtTool *self,
                    const AgcExtToolCall *call, void *out, bool *is_error,
                    void **state) {
    int mode = *(const int *)self->ud;
    g_af_starts++;
    AsyncSt *st = host->alloc(sizeof *st);
    st->magic = AF_STATE_MAGIC;
    st->mode = mode;
    st->polls_left = 2;
    st->token = call ? call->signal_token : NULL;
    *state = st;
    (void)out;
    if (is_error) *is_error = false;
    switch (mode) {
    case AS_START_DELETE:
        /* internal API, straight from inside start() */
        agentc_ext_remove_tool_internal("asyncstart.start-delete");
        return 1;
    case AS_START_UNLOAD:
    case AS_START_SHUTDOWN:
        /* emit -> hook -> unload/shutdown while this start is on the stack */
        g_af_teardown = mode == AS_START_UNLOAD ? AF_TD_UNLOAD : AF_TD_SHUTDOWN;
        host->emit(host, "agent_start", "{}", NULL);
        g_af_teardown = AF_TD_NONE;
        return 1;
    case AS_START_FATAL:
        /* full shutdown during start, then fail: no stop may run at all */
        g_af_teardown = AF_TD_SHUTDOWN;
        host->emit(host, "agent_start", "{}", NULL);
        g_af_teardown = AF_TD_NONE;
        host->free(st);   /* a failed start does not deliver stop */
        *state = NULL;
        return -22;
    default:
        return 0;   /* AS_SHUTDOWN_TIMEOUT: step drives the teardown */
    }
}

static int as_step(const AgcExtHost *host, const AgcExtTool *self,
                   const AgcExtToolCall *call, void *state, void *out,
                   bool *is_error) {
    (void)self;
    (void)call;
    (void)out;
    AsyncSt *st = state;
    if (!st || st->magic != AF_STATE_MAGIC) {
        g_af_bad_state = 1;
        return -22;
    }
    g_af_steps++;
    if (st->mode == AS_SHUTDOWN_TIMEOUT) {
        if (!st->deferred) {
            st->deferred = true;
            host->defer(host, af_shutdown_cb, NULL);
        }
        os_sleep_ns(5 * 1000000);   /* land the 1 ms deadline */
        return 0;
    }
    if (is_error) *is_error = false;
    return 1;
}

static const AgcExtTool as_tools[] = {
    { .struct_size = sizeof(AgcExtTool), .name = "asyncstart.start-unload",
      .label = "start unload", .description = "unload during start",
      .parameters_json = "{}", .ud = &as_m_unload,
      .start = as_start, .step = as_step, .stop = af_stop },
    { .struct_size = sizeof(AgcExtTool), .name = "asyncstart.start-shutdown",
      .label = "start shutdown", .description = "shutdown during start",
      .parameters_json = "{}", .ud = &as_m_shutdown,
      .start = as_start, .step = as_step, .stop = af_stop },
    { .struct_size = sizeof(AgcExtTool), .name = "asyncstart.start-delete",
      .label = "start delete", .description = "remove self during start",
      .parameters_json = "{}", .ud = &as_m_delete,
      .start = as_start, .step = as_step, .stop = af_stop },
    { .struct_size = sizeof(AgcExtTool), .name = "asyncstart.start-fatal",
      .label = "start fatal", .description = "shutdown then fail start",
      .parameters_json = "{}", .ud = &as_m_fatal,
      .start = as_start, .step = as_step, .stop = af_stop },
    { .struct_size = sizeof(AgcExtTool), .name = "asyncstart.shutdown-timeout",
      .label = "shutdown timeout", .description = "shutdown then time out",
      .parameters_json = "{}", .timeout_ms = 1, .ud = &as_m_timeout,
      .start = as_start, .step = as_step, .stop = af_stop },
};

static int asyncstart_init(const AgcExtHost *host) {
    for (size_t i = 0; i < sizeof as_tools / sizeof as_tools[0]; i++)
        host->add_tool(&as_tools[i]);
    host->on("agent_start", AGENTC_HOOK_OBSERVE, 0, af_teardown_hook, NULL);
    return 0;
}

static int asyncstart_entry(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    out->abi_version = AGENTC_EXT_ABI;
    out->struct_size = sizeof *out;
    out->name = "asyncstart";
    out->version = "0.1";
    out->order = 0;
    out->init = asyncstart_init;
    return 0;
}

static bool as_adopt(void) {
    agentc_ext_shutdown();
    g_af_teardown = AF_TD_NONE;
    bool ok = agentc_ext_adopt("asyncstart", asyncstart_entry) == 0;
    agentc_ext_load_all();
    return ok;
}

/* Registration gate fixtures (no extension owner: freed by shutdown). */
#define BASELINE_TOOL_SIZE \
    ((uint32_t)(offsetof(AgcExtTool, run) + sizeof(((AgcExtTool *)0)->run)))

static const AgcExtTool gate_full = {
    .struct_size = sizeof(AgcExtTool), .name = "gate.full", .label = "full",
    .description = "", .parameters_json = "{}", .ud = &af_m_ok,
    .start = af_start, .step = af_step, .stop = af_stop,
};
static const AgcExtTool gate_partial = {
    .struct_size = sizeof(AgcExtTool), .name = "gate.partial", .label = "partial",
    .description = "", .parameters_json = "{}", .ud = &af_m_ok,
    .start = af_start,
};
static const AgcExtTool gate_run_wins = {
    .struct_size = sizeof(AgcExtTool), .name = "gate.run-wins", .label = "run-wins",
    .description = "", .parameters_json = "{}", .ud = &af_m_ok,
    .run = af_sync_run, .start = af_start, .step = af_step, .stop = af_stop,
};
static const AgcExtTool gate_short_sync = {
    .struct_size = BASELINE_TOOL_SIZE, .name = "gate.short-sync", .label = "short-sync",
    .description = "", .parameters_json = "{}", .run = af_sync_run,
};
static const AgcExtTool gate_short_async = {
    .struct_size = BASELINE_TOOL_SIZE, .name = "gate.short-async", .label = "short-async",
    .description = "", .parameters_json = "{}", .ud = &af_m_ok,
    .start = af_start, .step = af_step, .stop = af_stop,
};
static const AgcExtTool gate_tiny = {
    .struct_size = 8, .name = "gate.tiny", .label = "tiny",
    .description = "", .parameters_json = "{}", .run = af_sync_run,
};
static const AgcExtTool gate_clamp = {
    .struct_size = sizeof(AgcExtTool), .name = "gate.clamp", .label = "clamp",
    .description = "", .parameters_json = "{}", .timeout_ms = 2000000,
    .ud = &af_m_ok, .start = af_start, .step = af_step, .stop = af_stop,
};
static const AgcExtTool gate_neg = {
    .struct_size = sizeof(AgcExtTool), .name = "gate.neg", .label = "neg",
    .description = "", .parameters_json = "{}", .timeout_ms = -7,
    .ud = &af_m_ok, .start = af_start, .step = af_step, .stop = af_stop,
};

/* Collector for one driven job. */
typedef struct {
    AgcBuf out;
    bool is_error;
    int state;
    bool started, done;
} AfCtx;

static int af_collect(void *ud, int what, const AgcJob *job) {
    AfCtx *c = ud;
    if (what == AGENTC_JOB_STARTED) {
        c->started = true;
        return 0;
    }
    if (job->out.len) agentc_buf_push(&c->out, job->out.p, job->out.len);
    c->is_error = job->is_error;
    c->state = job->state;
    c->done = true;
    return 0;
}

static void af_run_tool(const AgcTool *tool, const volatile bool *cancel, AfCtx *c) {
    agentc_memset(c, 0, sizeof *c);
    if (!tool) return;
    AgcToolCall call = { "af-call", tool->name, "{}", cancel };
    (void)agentc_tool_jobs_run(&tool, NULL, &call, 1, af_collect, c);
}

static const char *af_out(const AfCtx *c) {
    return c->out.p ? (const char *)c->out.p : "";
}

static void af_reset(void) {
    g_af_starts = g_af_steps = g_af_stops = 0;
    g_af_stop_n = 0;
    g_af_bad_state = 0;
    g_af_stop_saw_cancel = 0;
    g_af_cancel_flag = false;
}

/* The adapter's D-A9 call is agentc_pump(0), the app pump; a headless test has
 * to install one, exactly like agentc_mode_setup does. */
static int g_af_pumps;
static void af_test_pump(void *ud, int timeout_ms) {
    (void)ud;
    (void)timeout_ms;
    g_af_pumps++;
    agentc_ext_pump();
}

static void test_async_gates(void) {
    const AgcExtHost *h = agentc_ext_host();
    agentc_ext_shutdown();
    size_t base = agentc_ext_tools(NULL, 0);
    AgcTool tools[8];

    check("async_code_values",
          AGENTC_EXT_TOOL_FINISHED == 0 && AGENTC_EXT_TOOL_ERROR == 1 &&
          AGENTC_EXT_TOOL_TIMEOUT == 2 && AGENTC_EXT_TOOL_CANCELLED == 3 &&
          AGENTC_EXT_TOOL_UNLOAD == 4 && AGENTC_EXT_TOOL_TIMEOUT_MAX_MS == 1800000);

    h->add_tool(&gate_full);
    const AgcTool *gt = find_tool("gate.full", tools, 8, NULL);
    check("async_gate_full", gt != NULL);
    check("async_gate_full_async",
          gt != NULL && gt->run == NULL && gt->start != NULL && gt->step != NULL);

    h->add_tool(&gate_partial);
    check("async_gate_partial", find_tool("gate.partial", tools, 8, NULL) == NULL);

    h->add_tool(&gate_short_async);
    check("async_gate_short_async", find_tool("gate.short-async", tools, 8, NULL) == NULL);

    h->add_tool(&gate_run_wins);
    gt = find_tool("gate.run-wins", tools, 8, NULL);
    check("async_gate_run_wins",
          gt != NULL && gt->run != NULL && gt->start == NULL && gt->step == NULL);
    if (gt) {
        bool err = true;
        char *r = run_ext_tool(gt, "g", "{}", &err);
        check("async_gate_run_wins_exec", !err && cstr_eq(r, "sync-ran"));
        agentc_free(r);
    }

    h->add_tool(&gate_short_sync);
    check("async_gate_short_sync", find_tool("gate.short-sync", tools, 8, NULL) != NULL);

    h->add_tool(&gate_tiny);
    check("async_gate_tiny", find_tool("gate.tiny", tools, 8, NULL) == NULL);

    h->add_tool(&gate_clamp);
    gt = find_tool("gate.clamp", tools, 8, NULL);
    check("async_gate_timeout_clamp",
          gt != NULL && gt->timeout_ms == AGENTC_EXT_TOOL_TIMEOUT_MAX_MS);

    h->add_tool(&gate_neg);
    gt = find_tool("gate.neg", tools, 8, NULL);
    check("async_gate_timeout_negative", gt != NULL && gt->timeout_ms == 0);

    check("async_gate_count", agentc_ext_tools(NULL, 0) == base + 5);
    agentc_ext_shutdown();
}

static void test_async_runtime(void) {
    const AgcExtHost *h = agentc_ext_host();
    AgcTool tools[8];
    AfCtx c;

    agentc_ext_shutdown();
    check("async_adopt", agentc_ext_adopt("asyncfix", asyncfix_entry) == 0);
    agentc_ext_load_all();
    check("async_loaded", agentc_ext_tools(NULL, 0) == 8);
    agentc_ext_clear_dirty();

    /* normal run: two incremental steps, FINISHED, is_error clear */
    af_reset();
    const AgcTool *t = find_tool("asyncfix.ok", tools, 8, NULL);
    af_run_tool(t, NULL, &c);
    check("async_ok_output", cstr_eq(af_out(&c), "chunkchunk"));
    check("async_ok_is_error", !c.is_error && c.done && c.state == 2);
    check("async_ok_stop_once",
          g_af_stops == 1 && g_af_stop_reason[0] == AGENTC_EXT_TOOL_FINISHED);
    check("async_ok_state", g_af_bad_state == 0);
    agentc_buf_free(&c.out);

    /* start returning complete with is_error */
    af_reset();
    t = find_tool("asyncfix.err", tools, 8, NULL);
    af_run_tool(t, NULL, &c);
    check("async_err_output", cstr_eq(af_out(&c), "boom"));
    check("async_err_is_error", c.is_error);
    check("async_err_stop_once",
          g_af_stops == 1 && g_af_stop_reason[0] == AGENTC_EXT_TOOL_FINISHED);
    agentc_buf_free(&c.out);

    /* fatal step: the driver replaces the staged output */
    af_reset();
    t = find_tool("asyncfix.fatal", tools, 8, NULL);
    af_run_tool(t, NULL, &c);
    check("async_fatal_output",
          cstr_eq(af_out(&c), "error: tool 'asyncfix.fatal' failed to run (-22)\n"));
    check("async_fatal_is_error", c.is_error);
    check("async_fatal_stop_once",
          g_af_stops == 1 && g_af_stop_reason[0] == AGENTC_EXT_TOOL_ERROR);
    agentc_buf_free(&c.out);

    /* timeout */
    af_reset();
    t = find_tool("asyncfix.timeout", tools, 8, NULL);
    af_run_tool(t, NULL, &c);
    check("async_timeout_output",
          cstr_eq(af_out(&c), "error: tool 'asyncfix.timeout' timed out\n"));
    check("async_timeout_stop_once",
          g_af_stops == 1 && g_af_stop_reason[0] == AGENTC_EXT_TOOL_TIMEOUT);
    agentc_buf_free(&c.out);

    /* cancel from the extension's step; the signal token still resolves */
    af_reset();
    t = find_tool("asyncfix.cancel", tools, 8, NULL);
    af_run_tool(t, &g_af_cancel_flag, &c);
    check("async_cancel_output", cstr_eq(af_out(&c), "error: aborted"));
    check("async_cancel_is_error", c.is_error);
    check("async_cancel_stop_once",
          g_af_stops == 1 && g_af_stop_reason[0] == AGENTC_EXT_TOOL_CANCELLED);
    check("async_cancel_token", g_af_stop_saw_cancel == 1);
    agentc_buf_free(&c.out);

    /* unload mid-job (deferred from the step): exactly one stop(UNLOAD), no
     * later extension call, the record unlinked and its name reusable */
    af_reset();
    t = find_tool("asyncfix.unload", tools, 8, NULL);
    af_run_tool(t, NULL, &c);
    check("async_unload_output", cstr_eq(af_out(&c), "error: extension tool unloaded"));
    check("async_unload_is_error", c.is_error);
    check("async_unload_stop_once",
          g_af_stops == 1 && g_af_stop_reason[0] == AGENTC_EXT_TOOL_UNLOAD);
    check("async_unload_no_extra_step", g_af_steps == 1);
    check("async_unload_count", agentc_ext_tools(NULL, 0) == 0);
    check("async_unload_dirty", agentc_ext_dirty());
    agentc_buf_free(&c.out);

    /* shutdown mid-job (deferred from the step): the retired record survives
     * until the job unlinks, then everything is freed */
    agentc_ext_adopt("asyncfix", asyncfix_entry);
    agentc_ext_load_all();
    af_reset();
    t = find_tool("asyncfix.shutdown", tools, 8, NULL);
    af_run_tool(t, NULL, &c);
    check("async_shutdown_output", cstr_eq(af_out(&c), "error: extension tool unloaded"));
    check("async_shutdown_stop_once",
          g_af_stops == 1 && g_af_stop_reason[0] == AGENTC_EXT_TOOL_UNLOAD);
    check("async_shutdown_no_extra_step", g_af_steps == 1);
    check("async_shutdown_reset", agentc_ext_tools(NULL, 0) == 0);
    agentc_buf_free(&c.out);

    /* remove_tool_internal mid-job retires just that record; its name is
     * reusable while the retired record is still held */
    agentc_ext_adopt("asyncfix", asyncfix_entry);
    agentc_ext_load_all();
    size_t before = agentc_ext_tools(NULL, 0);
    agentc_ext_clear_dirty();
    af_reset();
    t = find_tool("asyncfix.remove", tools, 8, NULL);
    af_run_tool(t, NULL, &c);
    check("async_remove_output", cstr_eq(af_out(&c), "error: extension tool unloaded"));
    check("async_remove_stop_once",
          g_af_stops == 1 && g_af_stop_reason[0] == AGENTC_EXT_TOOL_UNLOAD);
    check("async_remove_no_extra_step", g_af_steps == 1);
    check("async_remove_count", agentc_ext_tools(NULL, 0) == before - 1);
    check("async_remove_dirty", agentc_ext_dirty());
    agentc_buf_free(&c.out);
    h->add_tool(&af_tools[7]);
    check("async_remove_reuse", find_tool("asyncfix.remove", tools, 8, NULL) != NULL);

    agentc_ext_shutdown();
}

static void test_async_start_teardown(void) {
    AgcTool tools[8];
    AfCtx c;

    /* unload from a hook while start() is on the stack */
    check("asyncstart_adopt_unload", as_adopt());
    check("asyncstart_loaded", agentc_ext_tools(NULL, 0) == 5);
    agentc_ext_clear_dirty();
    af_reset();
    const AgcTool *t = find_tool("asyncstart.start-unload", tools, 8, NULL);
    af_run_tool(t, NULL, &c);
    check("asyncstart_unload_output",
          cstr_eq(af_out(&c), "error: extension tool unloaded"));
    check("asyncstart_unload_error", c.is_error && c.done && c.state == 2);
    check("asyncstart_unload_stop_once",
          g_af_stops == 1 && g_af_stop_n == 1 &&
              g_af_stop_reason[0] == AGENTC_EXT_TOOL_UNLOAD);
    check("asyncstart_unload_no_step", g_af_steps == 0);
    check("asyncstart_unload_state", g_af_bad_state == 0);
    check("asyncstart_unload_count", agentc_ext_tools(NULL, 0) == 0);
    check("asyncstart_unload_dirty", agentc_ext_dirty());
    agentc_buf_free(&c.out);

    /* shutdown from a hook while start() is on the stack: the retired record
     * survives until this job unlinks, then is freed */
    check("asyncstart_adopt_shutdown", as_adopt());
    af_reset();
    t = find_tool("asyncstart.start-shutdown", tools, 8, NULL);
    af_run_tool(t, NULL, &c);
    check("asyncstart_shutdown_output",
          cstr_eq(af_out(&c), "error: extension tool unloaded"));
    check("asyncstart_shutdown_stop_once",
          g_af_stops == 1 && g_af_stop_reason[0] == AGENTC_EXT_TOOL_UNLOAD);
    check("asyncstart_shutdown_no_step", g_af_steps == 0);
    check("asyncstart_shutdown_state", g_af_bad_state == 0);
    check("asyncstart_shutdown_count", agentc_ext_tools(NULL, 0) == 0);
    agentc_buf_free(&c.out);

    /* the internal remove API called directly from start() */
    check("asyncstart_adopt_delete", as_adopt());
    agentc_ext_clear_dirty();
    af_reset();
    t = find_tool("asyncstart.start-delete", tools, 8, NULL);
    af_run_tool(t, NULL, &c);
    check("asyncstart_delete_output",
          cstr_eq(af_out(&c), "error: extension tool unloaded"));
    check("asyncstart_delete_stop_once",
          g_af_stops == 1 && g_af_stop_reason[0] == AGENTC_EXT_TOOL_UNLOAD);
    check("asyncstart_delete_no_step", g_af_steps == 0);
    check("asyncstart_delete_count", agentc_ext_tools(NULL, 0) == 4);
    check("asyncstart_delete_dirty", agentc_ext_dirty());
    agentc_buf_free(&c.out);

    /* a start that fails after a full shutdown: no stop at all, and the
     * driver still formats the real name after the record is released */
    check("asyncstart_adopt_fatal", as_adopt());
    af_reset();
    t = find_tool("asyncstart.start-fatal", tools, 8, NULL);
    af_run_tool(t, NULL, &c);
    check("asyncstart_fatal_output",
          cstr_eq(af_out(&c),
                  "error: tool 'asyncstart.start-fatal' failed to run (-22)\n"));
    check("asyncstart_fatal_no_stop", g_af_stops == 0 && g_af_steps == 0);
    check("asyncstart_fatal_error", c.is_error && c.done);
    check("asyncstart_fatal_count", agentc_ext_tools(NULL, 0) == 0);
    agentc_buf_free(&c.out);

    /* a step defers a full shutdown, then the 1 ms deadline lands: the timeout
     * message must carry the real name even though cleanup released the record */
    check("asyncstart_adopt_timeout", as_adopt());
    af_reset();
    t = find_tool("asyncstart.shutdown-timeout", tools, 8, NULL);
    af_run_tool(t, NULL, &c);
    check("asyncstart_timeout_output",
          cstr_eq(af_out(&c),
                  "error: tool 'asyncstart.shutdown-timeout' timed out\n"));
    check("asyncstart_timeout_error", c.is_error && c.done);
    check("asyncstart_timeout_stop_once",
          g_af_stops == 1 && g_af_stop_reason[0] == AGENTC_EXT_TOOL_UNLOAD);
    check("asyncstart_timeout_one_step", g_af_steps == 1);
    check("asyncstart_timeout_state", g_af_bad_state == 0);
    check("asyncstart_timeout_count", agentc_ext_tools(NULL, 0) == 0);
    agentc_buf_free(&c.out);

    agentc_ext_shutdown();
}

static void test_async_tools(void) {
    agentc_ext_shutdown();
    size_t base = agentc_mem_live();
    g_af_pumps = 0;
    agentc_pump_install(af_test_pump, NULL);
    test_async_gates();
    test_async_runtime();
    test_async_start_teardown();
    agentc_pump_install(NULL, NULL);
    agentc_ext_shutdown();
    check("async_pumped", g_af_pumps > 0);
    check("async_mem_baseline", agentc_mem_live() == base);
}

/* ------------------------------------------------ D2/D3 regression fixtures */

/* D2 (unloaded handler UAF): two hook handlers on one point; the earlier one
 * (lower priority) unloads the later one's extension and its heap userdata.
 * The dispatch snapshot must not invoke the stale handler afterwards. */
static int g_d2_hook_x, g_d2_hook_y, g_d2_y_shutdowns;
static int *g_d2_y_ud;

static int d2_hook_x(void *ud, const char *point, const char *payload, char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = NULL;
    g_d2_hook_x++;
    agentc_ext_unload("d2-y");
    return 0;
}
static int d2_hook_y(void *ud, const char *point, const char *payload, char **res) {
    (void)point; (void)payload;
    if (res) *res = NULL;
    g_d2_hook_y += ud ? *(int *)ud : 0;   /* UAF if the guard is missing */
    return 0;
}
static void d2_y_shutdown(void) {
    g_d2_y_shutdowns++;
    agentc_ext_host()->free(g_d2_y_ud);
    g_d2_y_ud = NULL;
}
static int d2_x_init(const AgcExtHost *host) {
    host->on("turn_start", AGENTC_HOOK_OBSERVE, 0, d2_hook_x, NULL);
    return 0;
}
static int d2_y_init(const AgcExtHost *host) {
    g_d2_y_ud = host->alloc(sizeof(int));
    *g_d2_y_ud = 7;
    host->on("turn_start", AGENTC_HOOK_OBSERVE, 10, d2_hook_y, g_d2_y_ud);
    return 0;
}

/* D2 section variant: the earlier render unloads the later section's owner and
 * its heap userdata; the snapshotted render must be skipped. */
static int g_d2_sec_x, g_d2_sec_y, g_d2_ysec_shutdowns;
static int *g_d2_ysec_ud;

static int d2_sec_x(const AgcExtHost *host, void *ud, void *out) {
    (void)host; (void)ud; (void)out;
    g_d2_sec_x++;
    agentc_ext_unload("d2-ysec");
    return 0;
}
static int d2_sec_y(const AgcExtHost *host, void *ud, void *out) {
    g_d2_sec_y += ud ? *(int *)ud : 0;   /* UAF if the guard is missing */
    host->out_write(out, "d2y", 3);
    return 0;
}
static void d2_ysec_shutdown(void) {
    g_d2_ysec_shutdowns++;
    agentc_ext_host()->free(g_d2_ysec_ud);
    g_d2_ysec_ud = NULL;
}
static int d2_xsec_init(const AgcExtHost *host) {
    static const AgcExtSection s = { sizeof(AgcExtSection), "d2x", 0, NULL, d2_sec_x };
    host->add_section(&s);
    return 0;
}
static int d2_ysec_init(const AgcExtHost *host) {
    g_d2_ysec_ud = host->alloc(sizeof(int));
    *g_d2_ysec_ud = 5;
    AgcExtSection s;
    agentc_memset(&s, 0, sizeof s);
    s.struct_size = sizeof s;
    s.key = "d2y";
    s.priority = 10;
    s.ud = g_d2_ysec_ud;
    s.render = d2_sec_y;
    host->add_section(&s);
    return 0;
}

static const AgcExt g_d2_x = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "d2-x", "1", 0, d2_x_init, NULL, 0 };
static const AgcExt g_d2_y = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "d2-y", "1", 0, d2_y_init, d2_y_shutdown, 0 };
static const AgcExt g_d2_xsec = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "d2-xsec", "1", 0, d2_xsec_init, NULL, 0 };
static const AgcExt g_d2_ysec = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "d2-ysec", "1", 0, d2_ysec_init,
    d2_ysec_shutdown, 0 };

static void test_unloaded_handler_skip(void) {
    agentc_ext_shutdown();
    g_d2_hook_x = g_d2_hook_y = g_d2_y_shutdowns = 0;
    g_d2_sec_x = g_d2_sec_y = g_d2_ysec_shutdowns = 0;
    agentc_ext_register(&g_d2_x);
    agentc_ext_register(&g_d2_y);
    agentc_ext_register(&g_d2_xsec);
    agentc_ext_register(&g_d2_ysec);
    agentc_ext_load_all();

    AgcExtResult r = agentc_ext_emit("turn_start", "{}");
    agentc_free(r.result_json);
    check("unloaded_hook_x_ran", g_d2_hook_x == 1);
    check("unloaded_hook_y_skipped", g_d2_hook_y == 0);
    check("unloaded_hook_y_shutdown", g_d2_y_shutdowns == 1);

    AgcBuf b = { 0 };
    agentc_ext_render_sections(&b);
    check("unloaded_section_x_ran", g_d2_sec_x == 1);
    check("unloaded_section_y_skipped", g_d2_sec_y == 0);
    check("unloaded_section_y_shutdown", g_d2_ysec_shutdowns == 1);
    agentc_buf_free(&b);
}

/* D3 (bounded growth): a churned tool name must not grow the tool table, and a
 * freed slot must be reused before the pointer vector is appended to. */
static void add_churn_tool(const char *name) {
    AgcExtTool t;
    agentc_memset(&t, 0, sizeof t);
    t.struct_size = sizeof t;
    t.name = name;
    t.label = name;
    t.description = "churn";
    t.parameters_json = "{\"type\":\"object\"}";
    t.run = fx_run;
    agentc_ext_host()->add_tool(&t);
}

static void test_tool_slot_reuse(void) {
    agentc_ext_shutdown();
    for (int i = 0; i < 200; i++) {
        add_churn_tool("churn");
        if (agentc_ext_remove_tool_internal("churn") != 0) break;
    }
    check("slot_reuse_churn_clean", agentc_ext_tools(NULL, 0) == 0);

    add_churn_tool("reuse_a");
    add_churn_tool("reuse_b");
    add_churn_tool("reuse_c");
    check("slot_reuse_before", agentc_ext_tools(NULL, 0) == 3);
    check("slot_reuse_remove", agentc_ext_remove_tool_internal("reuse_b") == 0);
    add_churn_tool("reuse_d");
    AgcTool got[8];
    size_t n = agentc_ext_tools(got, 8);
    check("slot_reuse_count", n == 3);
    check("slot_reuse_order",
          n == 3 && cstr_eq(got[0].name, "reuse_a") &&
              cstr_eq(got[1].name, "reuse_d") && cstr_eq(got[2].name, "reuse_c"));
    check("slot_reuse_removed_absent", find_tool("reuse_b", got, 8, NULL) == NULL);
}

/* G: pump and status-provider registrations must reuse their slots (like the
 * tool/command/section slots) so repeated register/unload churn does not grow
 * the vectors without bound, while exactly one live record remains. */
static int g_churn_pump_calls;
static int churn_pump_fn(void *ud) {
    (void)ud;
    g_churn_pump_calls++;
    return 0;
}
static size_t churn_status_fn(void *ud, AgcExtStatusSegment *out, size_t max,
                              char *arena, size_t arena_cap) {
    (void)ud; (void)arena; (void)arena_cap;
    if (out == NULL || max == 0) return 0;
    agentc_memset(&out[0], 0, sizeof out[0]);
    out[0].struct_size = sizeof out[0];
    out[0].slot = AGENTC_PSEG_SLOT_LEFT;
    out[0].text = "churn-status";
    return 1;
}
static int churn_init(const AgcExtHost *host) {
    agentc_ext_add_pump(churn_pump_fn, NULL);
    host->add_status(churn_status_fn, NULL);
    return 0;
}
static const AgcExt g_churn_ext = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "pump-churn", "1", 0, churn_init, NULL, 0 };

/* A failed init whose shutdown() still registers a tool: the post-shutdown
 * sweep must remove it (shutdown must run with the owner active, not owner 0). */
static int fis_init(const AgcExtHost *host) {
    (void)host;
    return -1;
}
static void fis_shutdown(void) {
    static const AgcExtTool t = {
        .struct_size = sizeof(AgcExtTool),
        .flags = AGENTC_TOOL_READONLY,
        .name = "initfail-leak",
        .label = "leak",
        .description = "leak",
        .parameters_json = "{\"type\":\"object\"}",
        .run = fx_run,
    };
    agentc_ext_host()->add_tool(&t);
}
static const AgcExt g_fis_ext = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "initfail", "1", 0, fis_init, fis_shutdown, 0 };

static void test_failed_init_shutdown_sweep(void) {
    agentc_ext_shutdown();
    agentc_log_set_level(4);
    agentc_ext_register(&g_fis_ext);
    agentc_ext_load_all();
    agentc_log_set_level(0);
    AgcTool got[4];
    check("failed_init_shutdown_swept",
          find_tool("initfail-leak", got, 4, NULL) == NULL);
    agentc_ext_shutdown();
}

/* A: a handed-out tool whose owning extension is unloaded from a hook must
 * survive as a retired record so ext_tool_run can return a clean error instead
 * of dereferencing freed memory. */
static int g_handout_hook_calls;
static int handout_unload_hook(void *ud, const char *point, const char *payload,
                               char **res) {
    (void)ud; (void)point; (void)payload;
    if (res) *res = NULL;
    g_handout_hook_calls++;
    agentc_ext_unload("handout-ext");
    return 0;
}
static int handout_init(const AgcExtHost *host) {
    static const AgcExtTool tool = {
        .struct_size = sizeof(AgcExtTool), .flags = AGENTC_TOOL_READONLY,
        .name = "handout_tool", .label = "handout", .description = "d",
        .parameters_json = fx_params, .run = fx_run };
    host->add_tool(&tool);
    host->on("tool_call", AGENTC_HOOK_OVERRIDE, 0, handout_unload_hook, NULL);
    return 0;
}
static const AgcExt g_handout = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "handout-ext", "1", 0, handout_init, NULL, 0 };

static void test_handout_unload_then_run(void) {
    agentc_ext_shutdown();
    g_handout_hook_calls = 0;
    agentc_ext_register(&g_handout);
    agentc_ext_load_all();
    AgcTool buf[4];
    const AgcTool *t = find_tool("handout_tool", buf, 4, NULL);
    check("handout.present", t != NULL);
    if (!t) return;
    AgcTool held = *t;   /* what the agent keeps and later executes */
    AgcExtResult r = agentc_ext_emit("tool_call", "{}");
    agentc_free(r.result_json);
    check("handout.hook_ran", g_handout_hook_calls == 1);
    check("handout.unloaded", !agentc_ext_is_loaded("handout-ext"));
    bool err = false;
    char *out = run_ext_tool(&held, "call-handout", "{\"text\":\"late\"}", &err);
    check("handout.clean_error",
          err && cstr_eq(out, "error: extension tool is no longer registered"));
    agentc_free(out);
    agentc_ext_shutdown();
}

/* An internal (builtin/MCP) synchronous tool handed out through the registry
 * must resolve to a clean error once its record is retired, instead of calling
 * the tool's own run() with a ud the owning module may already have freed. */
static int g_intern_calls;
static int intern_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                      bool *is_error) {
    (void)call;
    if (self && self->ud == (void *)(size_t)0x1234) g_intern_calls++;
    agentc_buf_cstr(out, "intern-ok");
    if (is_error) *is_error = false;
    return 0;
}

static void test_internal_tool_trampoline(void) {
    agentc_ext_shutdown();
    AgcTool t;
    agentc_memset(&t, 0, sizeof t);
    t.name = "intern-x";
    t.label = "intern";
    t.desc = "d";
    t.params_json = "{\"type\":\"object\"}";
    t.flags = AGENTC_TOOL_READONLY;
    t.ud = (void *)(size_t)0x1234;
    t.run = intern_run;
    check("intern.add", agentc_ext_add_tool_internal(&t) == 0);

    AgcTool got[2];
    const AgcTool *held = find_tool("intern-x", got, 2, NULL);
    check("intern.held", held != NULL && held->run != NULL &&
                             held->ud != (void *)(size_t)0x1234);

    AgcToolCall call;
    agentc_memset(&call, 0, sizeof call);
    call.call_id = "c1";
    call.name = "intern-x";
    call.args_json = "{}";
    AgcBuf out = { 0 };
    bool err = true;
    g_intern_calls = 0;
    int rc = held ? held->run(held, &call, &out, &err) : -1;
    check("intern.run_wrapped", rc == 0 && !err && g_intern_calls == 1);
    agentc_buf_free(&out);

    check("intern.remove", agentc_ext_remove_tool_internal("intern-x") == 0);
    agentc_memset(&out, 0, sizeof out);
    err = false;
    rc = held ? held->run(held, &call, &out, &err) : -1;
    check("intern.retired_clean",
          rc == 0 && err && !agentc_streq((const char *)out.p, "intern-ok"));
    check("intern.retired_no_call", g_intern_calls == 1);
    agentc_buf_free(&out);
    agentc_ext_shutdown();
}

/* D: the g_exts descriptor store must reuse freed slots instead of appending
 * a new record on every register/unload churn. */
static int desc_slot_init(const AgcExtHost *host) { (void)host; return 0; }
static const AgcExt g_desc_slot = {
    AGENTC_EXT_ABI, sizeof(AgcExt), "desc-slot", "1", 0, desc_slot_init, NULL, 0 };

static void test_ext_descriptor_slot_reuse(void) {
    agentc_ext_shutdown();
    size_t base = agentc_mem_live();
    agentc_log_set_level(4);   /* the churn load/unload logs are not asserted */
    for (int i = 0; i < 200; i++) {
        agentc_ext_register(&g_desc_slot);
        agentc_ext_load_all();
        agentc_ext_unload("desc-slot");
    }
    agentc_log_set_level(0);
    agentc_ext_register(&g_desc_slot);
    agentc_ext_load_all();
    AgcExtInfo info[4];
    check("desc_slot_reuse_ok", agentc_ext_describe(info, 4) == 1 &&
                                    agentc_streq(info[0].name, "desc-slot") &&
                                    info[0].state == AGENTC_EXT_STATE_LOADED);
    agentc_ext_unload("desc-slot");
    /* With slot reuse the store holds one record; append-only would be 200. */
    check("desc_slot_reuse_bounded", agentc_mem_live() <= base + 1024);
    agentc_ext_shutdown();
}

static void test_pump_status_slot_reuse(void) {
    agentc_ext_shutdown();
    /* churn quietly: the load/unload logs are not part of this assertion */
    agentc_log_set_level(4);
    for (int i = 0; i < 200; i++) {
        agentc_ext_register(&g_churn_ext);
        agentc_ext_load_all();
        agentc_ext_unload("pump-churn");
    }
    agentc_ext_register(&g_churn_ext);
    agentc_ext_load_all();
    agentc_log_set_level(0);

    g_churn_pump_calls = 0;
    agentc_ext_pump();
    check("pump_slot_reuse_one_live", g_churn_pump_calls == 1);

    AgcStatusValue segs[24];
    size_t n = agentc_status_snapshot(segs, 24);
    size_t churn = 0;
    for (size_t i = 0; i < n; i++)
        if (cstr_eq(segs[i].text, "churn-status")) churn++;
    check("status_slot_reuse_one_live", churn == 1);
    agentc_ext_unload("pump-churn");
}

int agentc_main(int argc, char **argv) {
    agentc_log_set_level(0);
    if (argc > 1 && agentc_streq(argv[1], "--harness")) return harness_main();

    (void)agentc_json_parse("{\"a\":[1,2]}", 11);
    /* Seed then reset the provider registry so its row storage is at the
     * baseline size before the snapshot. The late-recompose test builds a real
     * agent with the anthropic provider; materialize its process-lifetime
     * handle (cached in the static ops row) before the baseline too. */
    {
        const AgcProviderOps *warm[8];
        (void)agentc_provider_all(warm, 8);
        agentc_provider_registry_reset();
        (void)agentc_prov_anthropic();
    }
    /* The net mock backend keeps its segment vector and capture buffer across
     * resets (they are cleared, never freed). Touch both once before the
     * baseline so the HTTP tests reuse them instead of counting as a leak. */
    {
        AgcBuf warm = { 0 };
        mock_push_bytes(&warm, "x", 1);
        (void)agentc_write_file_atomic(TEST_ROOT "/warm.mock", warm.p, warm.len, 0644);
        agentc_buf_free(&warm);
        (void)agentc_mock_load(TEST_ROOT "/warm.mock");
        (void)agentc_net_send(-1, "x", 1);
        agentc_mock_reset();
        agentc_rm_rf(TEST_ROOT);
    }
    size_t baseline = agentc_mem_live();

    test_host_basics();
    test_abi_gates();
    test_json_helpers();
    test_registration();
    test_register_validation();
    test_register_gates();
    test_tools();
    test_tool_stability();
    test_commands();
    test_status();
    test_status_staleness();
    test_events();
    test_context();
    test_append_entry();
    test_append_entry_cap();
    test_model();
    test_defer_and_http();
    test_http_hygiene();
    test_http_cancel_inflight();
    test_owner_async_cleanup();
    test_order();
    test_load_all_appended();
    test_load_all_cap();
    test_shutdown_append_live();
    test_shutdown_during_load();
    test_hook_chain();
    test_hook_priority_ties();
    test_hook_chain_handled();
    test_hook_chain_veto();
    test_tool_veto_adapter();
    test_hook_first();
    test_hook_merge_replace();
    test_hook_fields_overlay();
    test_merge_fields();
    test_hook_caps();
    test_hook_observe_gate();
    test_hook_off_during_dispatch();
    test_hook_fail_modes();
    test_hook_invalid_result();
    test_wants_fast_path();
    test_unknown_point();
    test_session_before_switch_point();
    test_owner_attribution();
    test_dirty_on_unload();
    test_overrun_disable();
    test_recursion_guard();
    test_status_keying();
    test_pump_reentrancy();
    test_host_emit_short_struct();
    test_sections();
    test_failed_init();
    test_unload_order();
    test_dirty();
    test_late_mcp_recompose();
    test_bridge_undefined_events();
    test_apply_config();
    test_provider_registration();
    test_provider_all();
    test_provider_validation();
    test_provider_request();
    test_provider_stream();
    test_provider_retire();
    test_provider_cap();
    test_model_static();
    test_header_helpers();
    test_async_tools();
    test_unloaded_handler_skip();
    test_handout_unload_then_run();
    test_internal_tool_trampoline();
    test_ext_descriptor_slot_reuse();
    test_tool_slot_reuse();
    test_pump_status_slot_reuse();
    test_failed_init_shutdown_sweep();
    /* re-adopt the fixture so shutdown_called reflects the last fixture load */
    agentc_ext_shutdown();
    agentc_ext_adopt("fixture", AGENTC_EXT_TEST_ENTRY);
    agentc_ext_load_all();
    test_shutdown(baseline);

    return fails;
}
