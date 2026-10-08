/* oauth_test.c — OAuth store, PKCE vectors, callback parsing and refresh
 * against a scripted token endpoint (src/net/mock.c). No network, fixed clock.
 */
#include "oauth.h"
#include "config.h"
#include "plat.h"
#include "net/net_internal.h"

/* internal helpers (not in the frozen headers) */
void agentc_test_setenv(const char *name, const char *value);
void agentc_test_clearenv(void);
char *agentc_read_file_owned(const char *path, size_t *len);
int agentc_write_file_atomic(const char *path, const void *data, size_t len, int mode);
void agentc_rm_rf(const char *path);
void agentc_oauth_test_fail_random_at(int call_index);

#define ROOT "/tmp/agentc-oauth-test"
#define AUTHDIR ROOT "/config/agentc"
#define AUTHPATH AUTHDIR "/auth.jsonc"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

static bool contains(const char *s, const char *needle) {
    return s && agentc_str_str(s, needle) != NULL;
}

static i64 g_now_ms;
static i64 test_now(void) { return g_now_ms; }

static char g_auth_url[2048];
static char g_state[128];

static int cb_hook(void *ud, const char *url, char *out, size_t cap) {
    (void)ud;
    agentc_snprintf(g_auth_url, sizeof g_auth_url, "%s", url ? url : "");
    const char *p = agentc_str_str(url, "state=");
    g_state[0] = 0;
    if (p) {
        p += 6;
        size_t n = 0;
        while (p[n] && p[n] != '&' && n + 1 < sizeof g_state) {
            g_state[n] = p[n];
            n++;
        }
        g_state[n] = 0;
    }
    agentc_snprintf(out, cap, "code=test-code&state=%s", g_state);
    return 0;
}

static bool url_has(const char *needle) {
    return agentc_str_str(g_auth_url, needle) != NULL;
}

static void write_mock(const char *path, const char *response) {
    static const char hexd[] = "0123456789abcdef";
    AgcBuf b = { 0 };
    agentc_buf_cstr(&b, "dns oauth.test 127.0.0.1\n");
    agentc_buf_cstr(&b, "data ");
    for (const u8 *p = (const u8 *)response; *p; p++) {
        agentc_buf_byte(&b, (u8)hexd[*p >> 4]);
        agentc_buf_byte(&b, (u8)hexd[*p & 0xF]);
    }
    agentc_buf_byte(&b, '\n');
    (void)agentc_write_file_atomic(path, b.p, b.len, 0644);
    agentc_buf_free(&b);
}

static void write_json_mock(const char *path, const char *json) {
    AgcBuf r = { 0 };
    agentc_buf_cstr(&r, "HTTP/1.1 200 OK\r\ncontent-type: application/json\r\ncontent-length: ");
    agentc_buf_u64(&r, agentc_strlen(json));
    agentc_buf_cstr(&r, "\r\nconnection: close\r\n\r\n");
    agentc_buf_cstr(&r, json);
    write_mock(path, (const char *)r.p);
    agentc_buf_free(&r);
}

static char *read_file(const char *path) {
    size_t n = 0;
    return agentc_read_file_owned(path, &n);
}

static int file_mode(const char *path) {
    struct os_stat st;
    if (os_stat(path, &st) < 0) return -1;
    return (int)(st.st_mode & 0777u);
}

/* Windows has no POSIX permission bits: os_fstat() reports the platform's
 * fixed creation mode and there is no chmod(2) to force 0600. Compare the auth
 * file with a reference file written through the same API with mode 0600; on
 * POSIX that is exactly the old == 0600 check, on Windows it asserts the auth
 * file is no more permissive than the platform's own private creation path. */
static bool file_mode_is_private(const char *path) {
    char ref[512];
    agentc_snprintf(ref, sizeof ref, "%s.mode-ref", path);
    if (agentc_write_file_atomic(ref, "x", 1, 0600) != 0) return false;
    int want = file_mode(ref);
    (void)os_unlink(ref);
    return want >= 0 && file_mode(path) == want;
}

static bool sent_has(const char *needle) {
    const AgcBuf *s = agentc_mock_sent();
    return s && s->p && agentc_str_find((const char *)s->p, s->len, needle,
                                    agentc_strlen(needle)) >= 0;
}

static void test_pkce(void) {
    /* RFC 7636 appendix B */
    static const char verifier[] = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    static const char expect[] = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
    char out[96];
    check("pkce_rc", agentc_oauth_pkce_challenge(verifier, out, sizeof out) == 0);
    agentc_outf("pkce_vector_got=%s\n", out);
    check("pkce_vector", agentc_streq(out, expect));
    check("pkce_short_cap", agentc_oauth_pkce_challenge(verifier, out, 8) == -22);
}

static void test_callback(void) {
    static const char req[] =
        "GET /callback?code=abc%20123&state=xyz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    AgcBuf resp = { 0 };
    char code[64], state[64];
    int rc = agentc_oauth_callback_handle(req, agentc_strlen(req), &resp, code, sizeof code,
                                      state, sizeof state);
    check("callback_rc", rc == 0);
    check("callback_code", agentc_streq(code, "abc 123"));
    check("callback_state", agentc_streq(state, "xyz"));
    check("callback_200", contains((const char *)resp.p, "200 OK"));
    agentc_buf_free(&resp);

    static const char bad[] = "GET /callback?code=abc HTTP/1.1\r\n\r\n";
    AgcBuf resp2 = { 0 };
    (void)agentc_oauth_callback_handle(bad, agentc_strlen(bad), &resp2, code, sizeof code,
                                   state, sizeof state);
    check("callback_missing_state_400", contains((const char *)resp2.p, "400"));
    agentc_buf_free(&resp2);
}

static void test_callback_path(void) {
    static const char req[] =
        "GET /auth/callback?code=c1&state=s1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    AgcBuf resp = { 0 };
    char code[64], state[64];
    int rc = agentc_oauth_callback_handle_path(req, agentc_strlen(req), "/auth/callback", &resp,
                                               code, sizeof code, state, sizeof state);
    check("callback_path_rc", rc == 0 && agentc_streq(code, "c1") && agentc_streq(state, "s1"));
    check("callback_path_200", contains((const char *)resp.p, "200 OK"));
    agentc_buf_clear(&resp);

    /* the same request on the wrong path is not a callback, and its query must
     * not populate code/state for the accept loop to act on */
    (void)agentc_oauth_callback_handle_path(req, agentc_strlen(req), "/callback", &resp, code,
                                            sizeof code, state, sizeof state);
    check("callback_path_mismatch_400", contains((const char *)resp.p, "400"));
    check("callback_path_mismatch_no_code", code[0] == 0 && state[0] == 0);
    agentc_buf_free(&resp);
}

static void test_paste_parse(void) {
    char code[64], state[64];

    /* full redirect URL */
    const char *url = "http://localhost:1455/auth/callback?code=abc-123&state=st-9";
    check("paste_url",
          agentc_oauth_parse_pasted_code(url, agentc_strlen(url), code, sizeof code, state,
                                         sizeof state) &&
              agentc_streq(code, "abc-123") && agentc_streq(state, "st-9"));

    /* bare query with percent-encoding and surrounding whitespace */
    const char *q = "  code=a%2Fb%20c&state=s1 \n";
    check("paste_query",
          agentc_oauth_parse_pasted_code(q, agentc_strlen(q), code, sizeof code, state,
                                         sizeof state) &&
              agentc_streq(code, "a/b c") && agentc_streq(state, "s1"));

    /* <code>#<state> as the vendor CLI hands it back */
    const char *hash = "code-xyz#state-7";
    check("paste_hash",
          agentc_oauth_parse_pasted_code(hash, agentc_strlen(hash), code, sizeof code, state,
                                         sizeof state) &&
              agentc_streq(code, "code-xyz") && agentc_streq(state, "state-7"));

    /* bare code: nothing to validate the state against */
    check("paste_bare",
          agentc_oauth_parse_pasted_code("bare-code", 9, code, sizeof code, state,
                                         sizeof state) &&
              agentc_streq(code, "bare-code") && state[0] == 0);

    /* whitespace-only input is not a code */
    check("paste_empty", !agentc_oauth_parse_pasted_code("  \n", 3, code, sizeof code, state,
                                                          sizeof state));

    /* a zero-size output buffer must be rejected, not written past */
    check("paste_zero_cap", !agentc_oauth_parse_pasted_code("abc", 3, code, 0, state, 0));
}

static void test_login_rng_failure(void) {
    agentc_oauth_free();
    agentc_oauth_test_set_clock(test_now);
    agentc_oauth_test_set_callback(cb_hook, NULL);
    agentc_oauth_test_set_endpoints("https://auth.test/authorize", "http://oauth.test/token");
    g_now_ms = 1700000000000LL;
    g_auth_url[0] = 0;

    /* RNG failure deriving the PKCE verifier: fail closed, no authorize URL. */
    agentc_oauth_test_fail_random_at(0);
    check("rng_verifier_login_fails", agentc_oauth_login("anthropic") < 0);
    check("rng_verifier_error",
          contains(agentc_oauth_last_error(), "cannot generate secure random data"));
    check("rng_verifier_no_authorize_url", g_auth_url[0] == 0);
    check("rng_verifier_not_logged_in", !agentc_oauth_logged_in("anthropic"));

    /* RNG failure deriving the CSRF state (verifier already built): same. */
    g_auth_url[0] = 0;
    agentc_oauth_test_fail_random_at(1);
    check("rng_state_login_fails", agentc_oauth_login("anthropic") < 0);
    check("rng_state_error",
          contains(agentc_oauth_last_error(), "cannot generate secure random data"));
    check("rng_state_no_authorize_url", g_auth_url[0] == 0);
    check("rng_state_not_logged_in", !agentc_oauth_logged_in("anthropic"));

    agentc_oauth_test_fail_random_at(-1);
    agentc_oauth_test_set_callback(NULL, NULL);
    agentc_oauth_test_set_endpoints(NULL, NULL);
}

static void test_login_refresh_logout(void) {
    agentc_oauth_free();
    agentc_oauth_test_set_clock(test_now);
    agentc_oauth_test_set_callback(cb_hook, NULL);
    agentc_oauth_test_set_endpoints("https://auth.test/authorize", "http://oauth.test/token");
    g_now_ms = 1700000000000LL;             /* 2023-11-14T22:13:20Z */

    (void)agentc_write_file_atomic(AUTHPATH,
                               "{\"anthropic\":{\"api_key\":\"keep-me\"},"
                               "\"openai\":{\"api_key\":\"keep-openai\"}}\n",
                               71, 0600);

    write_json_mock(ROOT "/token1.mock",
                    "{\"access_token\":\"oat-1\",\"refresh_token\":\"ort-1\","
                    "\"expires_in\":3600,\"account\":{\"uuid\":\"acct-1\"}}");
    check("login_mock_load", agentc_mock_load(ROOT "/token1.mock") == 0);
    check("login_rc", agentc_oauth_login("anthropic") == 0);
    check("login_sent_code_verifier", sent_has("code_verifier"));
    check("anthropic_authorize_redirect",
          url_has("redirect_uri=http%3A%2F%2Flocalhost%3A0%2Fcallback"));
    check("anthropic_authorize_extra", url_has("code=true") && url_has("client_id=9d1c250a-"));
    {
        char want[256];
        agentc_snprintf(want, sizeof want, "\"state\":\"%s\"", g_state);
        check("anthropic_token_echoes_state", g_state[0] && sent_has(want));
    }
    check("login_logged_in", agentc_oauth_logged_in("anthropic"));
    check("login_file_mode", file_mode_is_private(AUTHPATH));

    char *txt = read_file(AUTHPATH);
    check("login_file_token", contains(txt, "\"access_token\":\"oat-1\"") &&
                                 contains(txt, "\"refresh_token\":\"ort-1\"") &&
                                 contains(txt, "\"account_id\":\"acct-1\"") &&
                                 contains(txt, "\"expires_at\":1700003600"));
    check("login_file_kept_anthropic_key", contains(txt, "\"api_key\":\"keep-me\""));
    check("login_file_kept_openai_key", contains(txt, "\"api_key\":\"keep-openai\""));
    agentc_free(txt);

    check("key_oauth", agentc_streq(agentc_auth_key("anthropic"), "oat-1"));

    /* oauth owns the provider even when an env key is present */
    agentc_test_setenv("ANTHROPIC_API_KEY", "env-key");
    agentc_auth_free();
    check("key_oauth_beats_env", agentc_streq(agentc_auth_key("anthropic"), "oat-1"));

    /* within 5 minutes of expiry: agentc_auth_key refreshes transparently */
    g_now_ms = (1700003600LL - 200) * 1000;
    write_json_mock(ROOT "/token2.mock",
                    "{\"access_token\":\"oat-2\",\"refresh_token\":\"ort-2\","
                    "\"expires_in\":7200}");
    check("refresh_mock_load", agentc_mock_load(ROOT "/token2.mock") == 0);
    check("refresh_key", agentc_streq(agentc_auth_key("anthropic"), "oat-2"));
    check("refresh_sent_grant", sent_has("\"grant_type\":\"refresh_token\""));
    check("refresh_sent_verifier_absent", !sent_has("code_verifier"));
    txt = read_file(AUTHPATH);
    check("refresh_file_updated", contains(txt, "\"access_token\":\"oat-2\"") &&
                                      contains(txt, "\"expires_at\":1700010600"));
    agentc_free(txt);

    /* failed refresh: error, no env fallback, credential still present */
    g_now_ms = (1700010600LL - 100) * 1000;
    write_mock(ROOT "/token3.mock",
               "HTTP/1.1 401 Unauthorized\r\ncontent-type: application/json\r\n"
               "content-length: 2\r\nconnection: close\r\n\r\n{}");
    check("fail_mock_load", agentc_mock_load(ROOT "/token3.mock") == 0);
    check("fail_refresh_null", agentc_auth_key("anthropic") == NULL);
    check("fail_refresh_error", agentc_oauth_last_error() != NULL);
    check("fail_logged_in_still", agentc_oauth_logged_in("anthropic"));
    agentc_auth_free();
    write_mock(ROOT "/token4.mock",
               "HTTP/1.1 401 Unauthorized\r\ncontent-length: 0\r\nconnection: close\r\n\r\n");
    check("fail_mock2_load", agentc_mock_load(ROOT "/token4.mock") == 0);
    check("fail_no_env_fallback", agentc_auth_key("anthropic") == NULL);
    agentc_test_setenv("ANTHROPIC_API_KEY", NULL);

    /* logout removes oauth but keeps api_key entries */
    check("logout_rc", agentc_oauth_logout("anthropic") == 0);
    check("logout_logged_out", !agentc_oauth_logged_in("anthropic"));
    txt = read_file(AUTHPATH);
    check("logout_keeps_keys", contains(txt, "\"api_key\":\"keep-me\"") &&
                                  contains(txt, "\"api_key\":\"keep-openai\"") &&
                                  !contains(txt, "\"oauth\""));
    agentc_free(txt);

    agentc_oauth_test_set_callback(NULL, NULL);
    agentc_oauth_test_set_endpoints(NULL, NULL);
}

static void test_openai_form(void) {
    agentc_oauth_free();
    agentc_oauth_test_set_clock(test_now);
    agentc_oauth_test_set_callback(cb_hook, NULL);
    agentc_oauth_test_set_endpoints("https://auth.test/authorize", "http://oauth.test/token");
    g_now_ms = 1700000000000LL;

    /* id_token payload: {"https://api.openai.com/auth":{"chatgpt_account_id":"acct-jwt"}} */
    static const char idtok[] =
        "e30."
        "eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdC1qd3QifX0."
        "sig";
    char token_json[512];
    agentc_snprintf(token_json, sizeof token_json,
                    "{\"access_token\":\"gpt-1\",\"refresh_token\":\"grt-1\","
                    "\"expires_in\":3600,\"id_token\":\"%s\"}",
                    idtok);
    write_json_mock(ROOT "/openai1.mock", token_json);
    check("openai_mock_load", agentc_mock_load(ROOT "/openai1.mock") == 0);
    check("openai_login_rc", agentc_oauth_login("openai") == 0);
    check("openai_form_body",
          sent_has("grant_type=authorization_code") &&
              sent_has("code_verifier=") && sent_has("client_id=app_"));
    check("openai_authorize_redirect",
          url_has("redirect_uri=http%3A%2F%2Flocalhost%3A0%2Fauth%2Fcallback") &&
              url_has("originator=codex_cli_rs"));
    check("openai_token_redirect_form",
          sent_has("redirect_uri=http%3A%2F%2Flocalhost%3A0%2Fauth%2Fcallback"));
    check("openai_token_no_state", !sent_has("state="));
    check("openai_key", agentc_streq(agentc_auth_key("openai"), "gpt-1"));
    check("openai_account_id", agentc_streq(agentc_oauth_account_id("openai"), "acct-jwt"));

    g_now_ms = (1700003600LL - 100) * 1000;
    write_json_mock(ROOT "/openai2.mock",
                    "{\"access_token\":\"gpt-2\",\"expires_in\":3600}");
    check("openai_refresh_load", agentc_mock_load(ROOT "/openai2.mock") == 0);
    check("openai_refresh_key", agentc_streq(agentc_auth_key("openai"), "gpt-2"));
    check("openai_refresh_form", sent_has("grant_type=refresh_token"));

    agentc_oauth_test_set_callback(NULL, NULL);
    agentc_oauth_test_set_endpoints(NULL, NULL);
}

/* A token endpoint (or id_token) is untrusted input; a CR/LF in the
 * access_token or account id must never reach a request header. */
static bool has_ctl(const char *s) {
    if (!s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20) return true;
    return false;
}

static void test_header_sanitize(void) {
    agentc_oauth_free();
    agentc_auth_free();
    agentc_oauth_test_set_clock(test_now);
    agentc_oauth_test_set_callback(cb_hook, NULL);
    agentc_oauth_test_set_endpoints("https://auth.test/authorize", "http://oauth.test/token");
    g_now_ms = 1700000000000LL;

    write_json_mock(ROOT "/ctl1.mock",
                    "{\"access_token\":\"tok\\r\\nX-Inject: yes\","
                    "\"refresh_token\":\"ref\",\"expires_in\":3600,"
                    "\"account\":{\"uuid\":\"acct\\r\\nX-Acct: bad\"}}");
    check("ctl_mock_load", agentc_mock_load(ROOT "/ctl1.mock") == 0);
    check("ctl_login_rc", agentc_oauth_login("anthropic") == 0);
    const char *tok = agentc_auth_key("anthropic");
    check("ctl_token_stored", tok && agentc_streq(tok, "tokX-Inject: yes"));
    check("ctl_token_no_ctl", !has_ctl(tok));
    const char *acct = agentc_oauth_account_id("anthropic");
    check("ctl_account_stored", acct && agentc_streq(acct, "acctX-Acct: bad"));
    check("ctl_account_no_ctl", !has_ctl(acct));

    /* the same bytes loaded straight from auth.jsonc on the next run */
    agentc_oauth_free();
    agentc_auth_free();
    static const char disk[] =
        "{\"anthropic\":{\"oauth\":{\"access_token\":\"d\\r\\nA: b\","
        "\"refresh_token\":\"r\",\"expires_at\":9999999999,"
        "\"account_id\":\"id\\r\\nC: d\"}}}\n";
    check("ctl_file_write",
          agentc_write_file_atomic(AUTHPATH, disk, sizeof disk - 1, 0600) == 0);
    agentc_auth_free();
    const char *dtok = agentc_auth_key("anthropic");
    check("ctl_load_token", dtok && agentc_streq(dtok, "dA: b") && !has_ctl(dtok));
    const char *dacct = agentc_oauth_account_id("anthropic");
    check("ctl_load_account", dacct && agentc_streq(dacct, "idC: d") && !has_ctl(dacct));

    agentc_oauth_test_set_callback(NULL, NULL);
    agentc_oauth_test_set_endpoints(NULL, NULL);
}

static void test_unknown_provider(void) {
    agentc_oauth_free();
    check("unknown_login", agentc_oauth_login("nope") == -22);
    check("unknown_logout", agentc_oauth_logout("nope") == -22);
    check("unknown_logged_in", !agentc_oauth_logged_in("nope"));
}

/* The one auth-store rewrite must preserve every top-level pair: a subscription
 * login may only add/replace the target provider's oauth member. */
static void test_auth_merge(void) {
    agentc_oauth_free();
    agentc_auth_free();
    agentc_oauth_test_set_clock(test_now);
    agentc_oauth_test_set_callback(cb_hook, NULL);
    agentc_oauth_test_set_endpoints("https://auth.test/authorize", "http://oauth.test/token");
    g_now_ms = 1700000000000LL;

    static const char initial[] =
        "{\"anthropic\":{\"api_key\":\"merge-anthropic\",\"note\":\"keep\"},"
        "\"openrouter\":{\"api_key\":\"merge-openrouter\"},"
        "\"api_keys\":{\"deepseek\":\"legacy-deepseek\"},"
        "\"x\":{\"y\":1}}\n";
    check("auth.merge_write",
          agentc_write_file_atomic(AUTHPATH, initial, sizeof initial - 1, 0600) == 0);
    agentc_auth_free();

    write_json_mock(ROOT "/merge1.mock",
                    "{\"access_token\":\"merge-oat\",\"refresh_token\":\"merge-ort\","
                    "\"expires_in\":3600,\"account\":{\"uuid\":\"merge-acct\"}}");
    check("auth.merge_login_mock", agentc_mock_load(ROOT "/merge1.mock") == 0);
    check("auth.merge_login_rc", agentc_oauth_login("openai") == 0);

    char *txt = read_file(AUTHPATH);
    check("auth.merge_login_keeps_keys",
          contains(txt, "\"api_key\":\"merge-anthropic\"") &&
              contains(txt, "\"api_key\":\"merge-openrouter\""));
    check("auth.merge_login_oauth_present",
          contains(txt, "\"oauth\":{\"access_token\":\"merge-oat\"") &&
              contains(txt, "\"account_id\":\"merge-acct\""));
    check("auth.merge_login_unknown_keys",
          contains(txt, "\"api_keys\":{\"deepseek\":\"legacy-deepseek\"}") &&
              contains(txt, "\"x\":{\"y\":1}") && contains(txt, "\"note\":\"keep\""));
    agentc_free(txt);

    /* reload from disk: both api keys and the new oauth entry are stored */
    agentc_auth_free();
    check("auth.merge_reload_anthropic",
          agentc_streq(agentc_auth_key("anthropic"), "merge-anthropic"));
    check("auth.merge_reload_oauth", agentc_streq(agentc_auth_key("openai"), "merge-oat"));

    /* remove one provider's key and oauth; the other pairs survive */
    check("auth.merge_set_empty", agentc_auth_set_key("anthropic", "") == 0);
    check("auth.merge_logout", agentc_oauth_logout("openai") == 0);
    txt = read_file(AUTHPATH);
    check("auth.merge_remove_key_gone", !contains(txt, "merge-anthropic"));
    check("auth.merge_remove_oauth_gone", !contains(txt, "\"oauth\""));
    check("auth.merge_remove_keeps_provider",
          contains(txt, "\"api_key\":\"merge-openrouter\""));
    check("auth.merge_remove_keeps_sibling", contains(txt, "\"note\":\"keep\""));
    check("auth.merge_remove_keeps_unknown",
          contains(txt, "\"api_keys\":{\"deepseek\":\"legacy-deepseek\"}") &&
              contains(txt, "\"x\":{\"y\":1}"));
    agentc_free(txt);

    /* a malformed file is replaced by a valid one instead of propagating */
    static const char broken[] = "{ this is not json\n";
    check("auth.merge_broken_write",
          agentc_write_file_atomic(AUTHPATH, broken, sizeof broken - 1, 0600) == 0);
    check("auth.merge_broken_set", agentc_auth_set_key("groq", "g-key") == 0);
    txt = read_file(AUTHPATH);
    AgcJson *root = txt ? agentc_json_parse(txt, agentc_strlen(txt)) : NULL;
    const char *gv = agentc_json_get_str(agentc_json_get(root, "groq"), "api_key");
    check("auth.merge_broken_valid", agentc_streq(gv, "g-key"));
    agentc_free(txt);

    agentc_oauth_test_set_callback(NULL, NULL);
    agentc_oauth_test_set_endpoints(NULL, NULL);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    agentc_test_setenv("XDG_CONFIG_HOME", ROOT "/config");
    agentc_test_setenv("HOME", ROOT "/home");
    agentc_test_clearenv();
    agentc_test_setenv("XDG_CONFIG_HOME", ROOT "/config");
    agentc_test_setenv("HOME", ROOT "/home");
    agentc_test_setenv("ANTHROPIC_API_KEY", NULL);
    agentc_rm_rf(ROOT);

    test_pkce();
    test_callback();
    test_callback_path();
    test_paste_parse();
    test_login_rng_failure();
    test_login_refresh_logout();
    test_openai_form();
    test_header_sanitize();
    test_unknown_provider();
    test_auth_merge();

    agentc_oauth_free();
    agentc_rm_rf(ROOT);
    agentc_test_clearenv();
    return fails;
}
