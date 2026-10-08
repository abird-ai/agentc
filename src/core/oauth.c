/* oauth.c — OAuth 2.0 authorization-code + PKCE (S256) subscription logins.
 *
 * Providers: Claude Pro/Max ("anthropic") and ChatGPT ("openai"). The flow is
 * the CLI flow used by the vendor tools:
 *   1. generate a PKCE verifier/challenge and a random state;
 *   2. open the provider authorize URL in the browser;
 *   3. run a one-shot loopback HTTP server on 127.0.0.1 (5 minute timeout),
 *      accept the redirect, validate state and pull out the code;
 *   4. exchange the code at the provider token endpoint (wire layer);
 *   5. store access/refresh tokens in ~/.config/agentc/auth.jsonc (0600, atomic).
 *
 * The credential store is a process-global cache owned by agentc_oauth_free(),
 * called from agentc_auth_free(). auth.c resolves a stored credential before any
 * environment/api-key fallback; a failed refresh is an error (never a silent
 * env fallback).
 *
 * Test hooks (fixed clock, endpoint overrides, callback injection) keep the
 * offline suite deterministic; see include/oauth.h.
 */
#include "agentc.h"
#include "plat.h"
#include "net.h"
#include "wire.h"
#include "oauth.h"

/* file helpers from config.c (not part of a frozen header) */
char *agentc_read_file_owned(const char *path, size_t *len);
int agentc_write_file_atomic(const char *path, const void *data, size_t len, int mode);
const char *agentc_config_home(char *buf, size_t cap);
bool agentc_path_join(char *out, size_t cap, const char *dir, const char *name);

/* auth.c's one read-merge-rewrite of auth.jsonc (internal cross-file helper) */
int agentc_auth_store_rewrite(const char *provider, int api_key_op, const char *api_key,
                              int oauth_op, const char *oauth_json);
enum { AUTH_KEEP = 0, AUTH_SET = 1, AUTH_CLEAR = 2 };

/* ------------------------------------------------ unofficial client table */
/* These are the OAuth client identifiers that the vendor CLIs use. They are
 * intentionally public for CLI interoperability but are NOT a published API:
 * the vendor can rotate them, change endpoints or reject the flow at any time.
 * Treat a 4xx/5xx here as "the unofficial client needs updating", not as an agentc
 * bug. Endpoint overrides in tests replace these values. */
typedef struct {
    const char *name;
    const char *client_id;
    const char *authorize_url;
    const char *token_url;
    const char *scope;
    const char *extra_query;    /* appended verbatim to the authorize query */
    bool token_json;            /* token endpoint body is JSON (vs form) */
    u16 fixed_port;             /* preferred loopback port; 0 = random */
    /* Appended: the redirect URI is registered per client, so the host, port and
     * path have to match it exactly. */
    const char *redirect_path;  /* loopback path the provider registered */
    bool token_state;           /* echo `state` back in the code exchange */
    bool strict_port;           /* port must be fixed_port verbatim (no random fallback) */
} OauthProvider;

static const OauthProvider providers[] = {
    { "anthropic",
      "9d1c250a-e61b-44d9-88ed-5944d1962f5e",
      "https://claude.ai/oauth/authorize",
      "https://console.anthropic.com/v1/oauth/token",
      "org:create_api_key user:profile user:inference user:sessions:claude_code "
      "user:mcp_servers user:file_upload",
      "code=true",
      true,
      54545,
      "/callback", true, false },
    { "openai",
      "app_EMoamEEZ73f0CkXaXp7hrann",
      "https://auth.openai.com/oauth/authorize",
      "https://auth.openai.com/oauth/token",
      "openid profile email offline_access",
      "id_token_add_organizations=true&codex_cli_simplified_flow=true&originator=codex_cli_rs",
      false,
      1455,
      "/auth/callback", false, true },
};
#define NPROVIDERS (sizeof providers / sizeof providers[0])
#define OAUTH_REFRESH_SKEW_S 300        /* refresh 5 minutes before expiry */
#define OAUTH_CALLBACK_TIMEOUT_MS 300000 /* 5 minutes */
#define OAUTH_HTTP_TIMEOUT_MS 30000

static int provider_index(const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < NPROVIDERS; i++)
        if (agentc_streq(providers[i].name, name)) return (int)i;
    return -1;
}

/* --------------------------------------------------------------- test hooks */
static i64 (*g_now_hook)(void);
static const char *g_authorize_override;
static const char *g_token_override;
static AgcOauthCallbackHook g_cb_hook;
static void *g_cb_ud;

void agentc_oauth_test_set_clock(i64 (*now_ms)(void)) { g_now_hook = now_ms; }
void agentc_oauth_test_set_endpoints(const char *authorize_url, const char *token_url) {
    g_authorize_override = authorize_url;
    g_token_override = token_url;
}
void agentc_oauth_test_set_callback(AgcOauthCallbackHook cb, void *ud) {
    g_cb_hook = cb;
    g_cb_ud = ud;
}

/* Internal test hook (declared ad hoc by tests/oauth_test.c): the call_index-th
 * (0-based) os_random() request inside agentc_oauth_login() fails as if the
 * kernel RNG were unavailable (-EIO), so the fail-closed path is observable.
 * A negative call_index disables the hook. */
static int g_random_fail_at = -1;
static int g_random_calls;

void agentc_oauth_test_fail_random_at(int call_index) {
    g_random_fail_at = call_index;
    g_random_calls = 0;
}

static int oauth_random(void *buf, size_t n) {
    int call = g_random_calls++;
    if (g_random_fail_at >= 0 && call == g_random_fail_at) return -5; /* EIO */
    return os_random(buf, n);
}

static i64 oauth_now_ms(void) {
    if (g_now_hook) return g_now_hook();
    i64 ns = os_now_ns(OS_CLOCK_REALTIME);
    return ns / 1000000;
}

static i64 oauth_now_s(void) { return oauth_now_ms() / 1000; }

static const char *provider_authorize(const OauthProvider *p) {
    return (g_authorize_override && g_authorize_override[0]) ? g_authorize_override
                                                              : p->authorize_url;
}
static const char *provider_token(const OauthProvider *p) {
    return (g_token_override && g_token_override[0]) ? g_token_override : p->token_url;
}

/* ---------------------------------------------------------------- sha-256 */
/* Small freestanding SHA-256 (PKCE S256; no mbedTLS in the test link). */
typedef struct {
    u32 h[8];
    u64 len;
    u8 buf[64];
    size_t n;
} Sha256;

static const u32 sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define SHA_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(Sha256 *s, const u8 *p) {
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) |
               ((u32)p[4 * i + 2] << 8) | (u32)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = SHA_ROTR(w[i - 15], 7) ^ SHA_ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = SHA_ROTR(w[i - 2], 17) ^ SHA_ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    u32 e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        u32 S1 = SHA_ROTR(e, 6) ^ SHA_ROTR(e, 11) ^ SHA_ROTR(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + S1 + ch + sha_k[i] + w[i];
        u32 S0 = SHA_ROTR(a, 2) ^ SHA_ROTR(a, 13) ^ SHA_ROTR(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(Sha256 *s) {
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85; s->h[2] = 0x3c6ef372;
    s->h[3] = 0xa54ff53a; s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c;
    s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->len = 0;
    s->n = 0;
}

static void sha256_update(Sha256 *s, const u8 *p, size_t n) {
    s->len += n;
    while (n) {
        if (s->n == 64) {
            sha256_block(s, s->buf);
            s->n = 0;
        }
        size_t take = 64 - s->n;
        if (take > n) take = n;
        agentc_memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        n -= take;
    }
}

static void sha256_final(Sha256 *s, u8 out[32]) {
    u64 bits = s->len * 8;
    u8 one = 0x80;
    sha256_update(s, &one, 1);
    u8 zero = 0;
    while (s->n != 56) sha256_update(s, &zero, 1);
    u8 lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (u8)(bits >> (56 - 8 * i));
    sha256_update(s, lenb, 8);
    sha256_block(s, s->buf);                /* flush the length block */
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (u8)(s->h[i] >> 24);
        out[4 * i + 1] = (u8)(s->h[i] >> 16);
        out[4 * i + 2] = (u8)(s->h[i] >> 8);
        out[4 * i + 3] = (u8)s->h[i];
    }
}

/* base64url without padding; returns chars written or 0 when cap is short */
static size_t b64url(const u8 *in, size_t n, char *out, size_t cap) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t padded = (n + 2) / 3 * 4;
    size_t pad = n % 3 == 1 ? 2 : (n % 3 == 2 ? 1 : 0);
    size_t need = padded - pad;
    if (need + 1 > cap) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        u32 v = (u32)in[i] << 16;
        if (i + 1 < n) v |= (u32)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = t[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = t[v & 63];
    }
    out[o] = 0;
    return o;
}

int agentc_oauth_pkce_challenge(const char *verifier, char *out, size_t cap) {
    if (!verifier || !out || cap < 44) return -22;
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, (const u8 *)verifier, agentc_strlen(verifier));
    u8 d[32];
    sha256_final(&s, d);
    return b64url(d, sizeof d, out, cap) ? 0 : -28;
}

/* ----------------------------------------------------------- credential store */

typedef struct {
    bool present;
    char *access_token;
    char *refresh_token;
    char *account_id;
    u64 expires_at;             /* unix seconds */
} OauthCred;

static OauthCred g_cred[NPROVIDERS];
static bool g_store_loaded;
static char g_err[256];

/* A token or account id ends up in an Authorization/x-account header, so a
 * CR/LF (or any other C0 byte) from a hostile token endpoint or id_token
 * would inject a request header. Strip C0 bytes; normal bytes untouched. */
static char *oauth_header_sanitize(const char *s) {
    if (!s) return NULL;
    size_t n = agentc_strlen(s);
    char *out = agentc_alloc(n + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20) continue;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return out;
}

const char *agentc_oauth_last_error(void) { return g_err[0] ? g_err : NULL; }

static void set_err(const char *msg) {
    agentc_snprintf(g_err, sizeof g_err, "%s", msg ? msg : "oauth error");
}

static void set_errf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    agentc_vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
}

void agentc_oauth_free(void) {
    for (size_t i = 0; i < NPROVIDERS; i++) {
        agentc_free(g_cred[i].access_token);
        agentc_free(g_cred[i].refresh_token);
        agentc_free(g_cred[i].account_id);
        agentc_memset(&g_cred[i], 0, sizeof g_cred[i]);
    }
    g_store_loaded = false;
    g_err[0] = 0;
}

static bool store_path(char *out, size_t cap) {
    char base[4096];
    if (!agentc_config_home(base, sizeof base)) return false;
    return agentc_path_join(out, cap, base, "auth.jsonc");
}

/* Persist one provider's changed OAuth credential. auth.c's merge helper
 * preserves every other top-level pair (other providers, "api_keys", unknown
 * keys), so store_save no longer rebuilds the whole file from the provider
 * table. */
static int store_save(int pidx) {
    if (pidx < 0 || (size_t)pidx >= NPROVIDERS) return -22;
    AgcBuf ob = { 0 };
    if (g_cred[pidx].present) {
        AgcJsonW w;
        agentc_jsonw_init(&w, &ob);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "access_token");
        agentc_jsonw_cstr(&w, g_cred[pidx].access_token ? g_cred[pidx].access_token : "");
        agentc_jsonw_key(&w, "refresh_token");
        agentc_jsonw_cstr(&w, g_cred[pidx].refresh_token ? g_cred[pidx].refresh_token : "");
        agentc_jsonw_key(&w, "expires_at");
        agentc_jsonw_u64(&w, g_cred[pidx].expires_at);
        agentc_jsonw_key(&w, "account_id");
        agentc_jsonw_cstr(&w, g_cred[pidx].account_id ? g_cred[pidx].account_id : "");
        agentc_jsonw_end(&w);
        agentc_buf_byte(&ob, 0);          /* oauth_json is a C string */
    }
    int rc = agentc_auth_store_rewrite(providers[pidx].name, AUTH_KEEP, NULL,
                                       g_cred[pidx].present ? AUTH_SET : AUTH_CLEAR,
                                       ob.p ? (const char *)ob.p : NULL);
    agentc_buf_free(&ob);
    return rc;
}

static void store_load(void) {
    for (size_t i = 0; i < NPROVIDERS; i++) {
        agentc_free(g_cred[i].access_token);
        agentc_free(g_cred[i].refresh_token);
        agentc_free(g_cred[i].account_id);
        agentc_memset(&g_cred[i], 0, sizeof g_cred[i]);
    }
    g_store_loaded = true;
    char path[4224];
    if (!store_path(path, sizeof path)) return;
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text) return;
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(a, text, len);
    if (agentc_json_type(root) == AGENTC_JSON_OBJ) {
        for (size_t i = 0; i < NPROVIDERS; i++) {
            AgcJson *p = agentc_json_get(root, providers[i].name);
            AgcJson *o = agentc_json_get(p, "oauth");
            if (agentc_json_type(o) != AGENTC_JSON_OBJ) continue;
            const char *at = agentc_json_get_str(o, "access_token");
            if (!at || !at[0]) continue;
            const char *rt = agentc_json_get_str(o, "refresh_token");
            const char *ac = agentc_json_get_str(o, "account_id");
            g_cred[i].access_token = oauth_header_sanitize(at);
            g_cred[i].refresh_token = rt ? oauth_header_sanitize(rt) : NULL;
            g_cred[i].account_id = ac ? oauth_header_sanitize(ac) : NULL;
            g_cred[i].expires_at = (u64)agentc_json_get_int(o, "expires_at", 0);
            g_cred[i].present = true;
        }
    }
    agentc_free(text);
    agentc_json_arena_free(a);
}

static void store_ensure(void) {
    if (!g_store_loaded) store_load();
}

/* ------------------------------------------------------------- HTTP helper */

typedef struct {
    AgcBuf *body;
} BodySink;

static int body_sink(void *ud, const void *p, size_t n) {
    BodySink *s = ud;
    agentc_buf_push(s->body, p, n);
    return 0;
}

static const char *http_error_for(int rc, int status) {
    (void)rc;
    if (status == 400) return "token endpoint rejected the request (400)";
    if (status == 401) return "token endpoint rejected the credentials (401)";
    if (status == 403) return "token endpoint denied access (403)";
    if (status >= 400) return "token endpoint error";
    return "token request failed";
}

static int http_post(const char *url, const char *content_type, const void *body,
                     size_t body_len, AgcBuf *resp, int *status_out) {
    AgcBuf hdrs = { 0 };
    agentc_buf_printf(&hdrs, "content-type: %s\r\naccept: application/json\r\n",
                  content_type);
    AgcHttp *h = agentc_http_new("POST", url, (const char *)hdrs.p, body, body_len);
    agentc_buf_free(&hdrs);
    if (!h) {
        set_err("cannot start token request");
        return -12;
    }
    BodySink sink = { resp };
    int rc = agentc_http_run(h, body_sink, &sink, OAUTH_HTTP_TIMEOUT_MS);
    int status = agentc_http_status(h);
    if (status_out) *status_out = status;
    if (rc != 0) {
        const char *e = agentc_http_error(h);
        set_errf("token request failed: %s", e && e[0] ? e : "transport error");
    } else if (status < 200 || status > 299) {
        set_err(http_error_for(rc, status));
    } else if (resp->len == 0) {
        set_err("token endpoint returned an empty body");
        rc = -1;
    }
    agentc_http_free(h);
    return rc == 0 && status >= 200 && status <= 299 && resp->len > 0 ? 0 : -1;
}

static void form_pair(AgcBuf *b, const char *key, const char *val) {
    static const char hex[] = "0123456789ABCDEF";
    agentc_buf_cstr(b, key);
    agentc_buf_byte(b, '=');
    for (const char *p = val; p && *p; p++) {
        u8 c = (u8)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            agentc_buf_byte(b, c);
        } else {
            agentc_buf_byte(b, '%');
            agentc_buf_byte(b, (u8)hex[c >> 4]);
            agentc_buf_byte(b, (u8)hex[c & 0xF]);
        }
    }
}

static void form_add(AgcBuf *b, const char *key, const char *val) {
    if (b->len) agentc_buf_byte(b, '&');
    form_pair(b, key, val);
}

/* token request bodies ---------------------------------------------------- */

static void json_add_str(AgcJsonW *w, const char *key, const char *val) {
    agentc_jsonw_key(w, key);
    agentc_jsonw_cstr(w, val ? val : "");
}

static AgcBuf build_code_exchange(const OauthProvider *p, const char *code,
                                 const char *redirect_uri, const char *verifier,
                                 const char *state) {
    AgcBuf b = { 0 };
    if (p->token_json) {
        AgcJsonW w;
        agentc_jsonw_init(&w, &b);
        agentc_jsonw_obj(&w);
        json_add_str(&w, "grant_type", "authorization_code");
        json_add_str(&w, "code", code);
        if (p->token_state) json_add_str(&w, "state", state);
        json_add_str(&w, "redirect_uri", redirect_uri);
        json_add_str(&w, "client_id", p->client_id);
        json_add_str(&w, "code_verifier", verifier);
        agentc_jsonw_end(&w);
    } else {
        form_add(&b, "grant_type", "authorization_code");
        form_add(&b, "code", code);
        if (p->token_state) form_add(&b, "state", state);
        form_add(&b, "redirect_uri", redirect_uri);
        form_add(&b, "client_id", p->client_id);
        form_add(&b, "code_verifier", verifier);
    }
    return b;
}

static AgcBuf build_refresh(const OauthProvider *p, const char *refresh_token) {
    AgcBuf b = { 0 };
    if (p->token_json) {
        AgcJsonW w;
        agentc_jsonw_init(&w, &b);
        agentc_jsonw_obj(&w);
        json_add_str(&w, "grant_type", "refresh_token");
        json_add_str(&w, "refresh_token", refresh_token);
        json_add_str(&w, "client_id", p->client_id);
        agentc_jsonw_end(&w);
    } else {
        form_add(&b, "grant_type", "refresh_token");
        form_add(&b, "refresh_token", refresh_token);
        form_add(&b, "client_id", p->client_id);
    }
    return b;
}

/* ------------------------------------------------- base64url / id_token */

static int b64url_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

/* Decode unpadded base64url into an owned NUL-terminated string. */
static char *b64url_decode(const char *p, size_t n) {
    char *out = agentc_alloc(n / 4 * 3 + 4);
    size_t o = 0;
    u32 acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n && p[i] != '='; i++) {
        int v = b64url_val((unsigned char)p[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (u32)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (char)((acc >> (unsigned)bits) & 0xFFu);
        }
    }
    out[o] = 0;
    return out;
}

/* The ChatGPT account id lives in the id_token JWT, claim
 * "https://api.openai.com/auth" -> "chatgpt_account_id". The payload gets its
 * own arena so the caller's live token document is never cleared. */
static char *account_from_id_token(const char *jwt) {
    size_t n1 = 0;
    while (jwt[n1] && jwt[n1] != '.') n1++;
    if (!jwt[n1]) return NULL;
    const char *pay = jwt + n1 + 1;
    size_t n2 = 0;
    while (pay[n2] && pay[n2] != '.') n2++;
    if (n2 == 0) return NULL;
    char *json = b64url_decode(pay, n2);
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(a, json, agentc_strlen(json));
    const char *ac = NULL;
    if (agentc_json_type(root) == AGENTC_JSON_OBJ) {
        AgcJson *auth = agentc_json_get(root, "https://api.openai.com/auth");
        if (agentc_json_type(auth) == AGENTC_JSON_OBJ)
            ac = agentc_json_get_str(auth, "chatgpt_account_id");
        if (!ac) ac = agentc_json_get_str(root, "chatgpt_account_id");
    }
    char *out = (ac && ac[0]) ? oauth_header_sanitize(ac) : NULL;
    agentc_json_arena_free(a);
    agentc_free(json);
    return out;
}

/* Parse a token response into cred. Returns 0 or -1. */
static int parse_token_response(const char *body, size_t len, OauthCred *cred) {
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(a, body, len);
    if (agentc_json_type(root) != AGENTC_JSON_OBJ) {
        set_err("token endpoint returned invalid JSON");
        agentc_json_arena_free(a);
        return -1;
    }
    const char *at = agentc_json_get_str(root, "access_token");
    if (!at || !at[0]) {
        set_err("token response has no access_token");
        agentc_json_arena_free(a);
        return -1;
    }
    const char *rt = agentc_json_get_str(root, "refresh_token");
    const char *ac = agentc_json_get_str(root, "account_id");
    if (!ac) {
        AgcJson *a = agentc_json_get(root, "account");
        if (agentc_json_type(a) == AGENTC_JSON_OBJ) {
            ac = agentc_json_get_str(a, "uuid");
            if (!ac) ac = agentc_json_get_str(a, "id");
            if (!ac) ac = agentc_json_get_str(a, "account_id");
        }
    }
    i64 expires_in = agentc_json_get_int(root, "expires_in", 3600);
    if (expires_in <= 0) expires_in = 3600;
    /* Clamp so a hostile/huge expires_in cannot overflow oauth_now_s()+expires_in. */
    if (expires_in > 2592000) expires_in = 2592000;   /* 30 days */

    /* copy the strings we keep while the token document is still live;
     * account_from_id_token owns a separate arena. C0 control bytes are
     * stripped so a token can never inject an HTTP header. */
    char *atc = oauth_header_sanitize(at);
    char *rtc = rt ? oauth_header_sanitize(rt) : NULL;
    char *acc = ac ? oauth_header_sanitize(ac) : NULL;
    if (!acc) {
        size_t idl = 0;
        const char *idt = agentc_json_str(agentc_json_get(root, "id_token"), &idl);
        if (idt && idl > 0 && idl < 4096) {
            char tmp[4096];
            agentc_memcpy(tmp, idt, idl);
            tmp[idl] = 0;
            acc = account_from_id_token(tmp);   /* separate arena */
        }
    }
    agentc_json_arena_free(a);

    agentc_free(cred->access_token);
    cred->access_token = atc;
    if (rtc) {
        agentc_free(cred->refresh_token);
        cred->refresh_token = rtc;
    }
    if (acc) {
        agentc_free(cred->account_id);
        cred->account_id = acc;
    }
    cred->expires_at = (u64)(oauth_now_s() + expires_in);
    cred->present = true;
    return 0;
}

/* ------------------------------------------------------------- token flow */

static int exchange_code(int pidx, const char *code, const char *redirect_uri,
                         const char *verifier, const char *state) {
    const OauthProvider *p = &providers[pidx];
    AgcBuf body = build_code_exchange(p, code, redirect_uri, verifier, state);
    AgcBuf resp = { 0 };
    const char *ct = p->token_json ? "application/json"
                                   : "application/x-www-form-urlencoded";
    int rc = http_post(provider_token(p), ct, body.p, body.len, &resp, NULL);
    agentc_buf_free(&body);
    if (rc != 0) {
        agentc_buf_free(&resp);
        return -1;
    }
    rc = parse_token_response((const char *)resp.p, resp.len, &g_cred[pidx]);
    agentc_buf_free(&resp);
    return rc;
}

int agentc_oauth_refresh(const char *provider) {
    int pidx = provider_index(provider);
    if (pidx < 0) return -22;
    store_ensure();
    OauthCred *cred = &g_cred[pidx];
    if (!cred->present || !cred->refresh_token || !cred->refresh_token[0]) {
        set_errf("no stored refresh token for %s", providers[pidx].name);
        return -2;
    }
    const OauthProvider *p = &providers[pidx];
    AgcBuf body = build_refresh(p, cred->refresh_token);
    AgcBuf resp = { 0 };
    const char *ct = p->token_json ? "application/json"
                                   : "application/x-www-form-urlencoded";
    int rc = http_post(provider_token(p), ct, body.p, body.len, &resp, NULL);
    agentc_buf_free(&body);
    if (rc != 0) {
        agentc_buf_free(&resp);
        return -1;
    }
    rc = parse_token_response((const char *)resp.p, resp.len, cred);
    agentc_buf_free(&resp);
    if (rc != 0) return rc;
    int wrc = store_save(pidx);
    if (wrc < 0) {
        set_errf("cannot save refreshed token (errno %d)", wrc);
        return wrc;
    }
    return 0;
}

bool agentc_oauth_logged_in(const char *provider) {
    int pidx = provider_index(provider);
    if (pidx < 0) return false;
    store_ensure();
    return g_cred[pidx].present;
}

int agentc_oauth_token_for(const char *provider, const char **token) {
    if (token) *token = NULL;
    int pidx = provider_index(provider);
    if (pidx < 0) return 0;
    store_ensure();
    OauthCred *cred = &g_cred[pidx];
    if (!cred->present) return 0;
    i64 now = oauth_now_s();
    if (cred->expires_at > 0 && now >= (i64)cred->expires_at - OAUTH_REFRESH_SKEW_S) {
        if (agentc_oauth_refresh(provider) != 0) {
            if (!g_err[0]) set_errf("cannot refresh %s token", provider);
            return -1;
        }
    }
    if (token) *token = cred->access_token;
    return 1;
}

int agentc_oauth_logout(const char *provider) {
    int pidx = provider_index(provider);
    if (pidx < 0) return -22;
    store_ensure();
    if (g_cred[pidx].present) {
        agentc_free(g_cred[pidx].access_token);
        agentc_free(g_cred[pidx].refresh_token);
        agentc_free(g_cred[pidx].account_id);
        agentc_memset(&g_cred[pidx], 0, sizeof g_cred[pidx]);
    }
    return store_save(pidx);
}

/* --------------------------------------------------------- authorize URL */

static void url_add(AgcBuf *b, const char *key, const char *val) {
    char last = b->len ? (char)b->p[b->len - 1] : 0;
    if (last == '?' || last == '&') {
        /* no separator needed after '?' or after an extra_query fragment */
    } else if (b->len) {
        agentc_buf_byte(b, '&');
    }
    form_pair(b, key ? key : "", val ? val : "");
}

static char *build_authorize_url(const OauthProvider *p, const char *redirect_uri,
                                 const char *challenge, const char *state) {
    AgcBuf b = { 0 };
    agentc_buf_cstr(&b, provider_authorize(p));
    agentc_buf_byte(&b, '?');
    url_add(&b, "client_id", p->client_id);
    url_add(&b, "response_type", "code");
    url_add(&b, "redirect_uri", redirect_uri);
    url_add(&b, "scope", p->scope);
    url_add(&b, "code_challenge", challenge);
    url_add(&b, "code_challenge_method", "S256");
    url_add(&b, "state", state);
    if (p->extra_query && p->extra_query[0]) {
        agentc_buf_byte(&b, '&');
        agentc_buf_cstr(&b, p->extra_query);
    }
    agentc_buf_byte(&b, 0);
    b.len--;                                  /* keep NUL out of len */
    return (char *)b.p;
}

/* ------------------------------------------------------- callback parsing */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *p, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (p[i] == '%' && i + 2 < n) {
            int hi = hexval((u8)p[i + 1]), lo = hexval((u8)p[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (p[i] == '+') {
            out[o++] = ' ';
            continue;
        }
        out[o++] = p[i];
    }
    out[o] = 0;
}

/* Extract a query parameter value from a query string (no leading '?'). */
static bool query_param(const char *q, size_t qn, const char *key, char *out,
                        size_t cap) {
    size_t klen = agentc_strlen(key);
    size_t i = 0;
    while (i < qn) {
        size_t ks = i;
        while (i < qn && q[i] != '=' && q[i] != '&') i++;
        size_t ke = i;
        const char *vs = "";
        size_t vn = 0;
        if (i < qn && q[i] == '=') {
            i++;
            vs = q + i;
            while (i < qn && q[i] != '&') i++;
            vn = (size_t)(q + i - vs);
        }
        if (ke - ks == klen && agentc_memeq(q + ks, key, klen)) {
            url_decode(vs, vn, out, cap);
            return true;
        }
        if (i < qn && q[i] == '&') i++;
    }
    return false;
}

int agentc_oauth_callback_handle(const char *req, size_t n, AgcBuf *response,
                             char *code_out, size_t code_cap,
                             char *state_out, size_t state_cap) {
    return agentc_oauth_callback_handle_path(req, n, "/callback", response, code_out, code_cap,
                                              state_out, state_cap);
}

const char *agentc_oauth_account_id(const char *provider) {
    int pidx = provider_index(provider);
    if (pidx < 0) return NULL;
    store_ensure();
    return g_cred[pidx].present ? g_cred[pidx].account_id : NULL;
}

static const char *http_status_line(bool ok) {
    return ok ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 400 Bad Request\r\n";
}

int agentc_oauth_callback_handle_path(const char *req, size_t n, const char *path,
                                      AgcBuf *response, char *code_out,
                                      size_t code_cap, char *state_out, size_t state_cap) {
    if (code_out && code_cap) code_out[0] = 0;
    if (state_out && state_cap) state_out[0] = 0;
    if (!req || !response) return -22;

    size_t line_end = 0;
    while (line_end < n && req[line_end] != '\n') line_end++;
    size_t target_s = 0, target_e = 0;
    {
        size_t i = 0;
        while (i < line_end && req[i] != ' ') i++;
        while (i < line_end && req[i] == ' ') i++;
        target_s = i;
        while (i < line_end && req[i] != ' ' && req[i] != '?') i++;
        target_e = i;
    }
    const char *q = NULL;
    size_t qn = 0;
    {
        size_t i = target_e;
        if (i < line_end && req[i] == '?') {
            q = req + i + 1;
            size_t j = i + 1;
            while (j < line_end && req[j] != ' ') j++;
            qn = j - i - 1;
        }
    }
    size_t plen = path && path[0] ? agentc_strlen(path) : 0;
    bool is_callback = plen > 0 && target_e - target_s == plen &&
                       agentc_memeq(req + target_s, path, plen);
    if (is_callback && q) {
        if (code_out && code_cap) (void)query_param(q, qn, "code", code_out, code_cap);
        if (state_out && state_cap)
            (void)query_param(q, qn, "state", state_out, state_cap);
    }

    bool ok = is_callback && code_out && code_out[0] && state_out && state_out[0];
    AgcBuf b = { 0 };
    agentc_buf_cstr(&b, http_status_line(ok));
    if (ok) {
        static const char page[] =
            "<!doctype html><html><body><h3>agentc: login complete</h3>"
            "<p>You can close this tab and return to the terminal.</p></body></html>";
        agentc_buf_cstr(&b, "content-type: text/html; charset=utf-8\r\ncontent-length: ");
        agentc_buf_u64(&b, agentc_strlen(page));
        agentc_buf_cstr(&b, "\r\nconnection: close\r\n\r\n");
        agentc_buf_cstr(&b, page);
    } else {
        static const char page[] =
            "<!doctype html><html><body><h3>agentc: waiting for the OAuth callback</h3>"
            "</body></html>";
        agentc_buf_cstr(&b, "content-type: text/html; charset=utf-8\r\ncontent-length: ");
        agentc_buf_u64(&b, agentc_strlen(page));
        agentc_buf_cstr(&b, "\r\nconnection: close\r\n\r\n");
        agentc_buf_cstr(&b, page);
    }
    agentc_buf_push(response, b.p, b.len);
    agentc_buf_free(&b);
    return 0;
}

/* -------------------------------------------------------- loopback server */
/* net.h exposes client sockets only (no bind/listen/accept), so the listener
 * lives here as a Linux-x86-64 syscall block. It is the only platform-specific
 * code in src/core; other targets get -ENOSYS and the code-paste flow only.
 * Contract note for the M5 report: plat/net should grow a server API. */
#if defined(__linux__) && defined(__x86_64__)
#define OA_SYS_socket 41
#define OA_SYS_accept4 288
#define OA_SYS_bind 49
#define OA_SYS_listen 50
#define OA_SYS_getsockname 51
#define OA_SYS_setsockopt 54
#define OA_AF_INET 2
#define OA_SOCK_STREAM 1
#define OA_SOCK_NONBLOCK 0x800
#define OA_SOCK_CLOEXEC 0x80000
#define OA_SOL_SOCKET 1
#define OA_SO_REUSEADDR 2

struct oa_sockaddr_in {
    u16 family;
    u16 port_be;
    u32 addr_be;
    u8 zero[8];
} __attribute__((packed));

static inline long oauth_sys(long n, long a, long b, long c, long d, long e) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}

typedef struct {
    int fd;
    u16 port;
} LoopServer;

static int loop_start(u16 want_port, bool strict, LoopServer *s) {
    for (int attempt = 0; attempt < (strict ? 1 : 2); attempt++) {
        u16 port = attempt == 0 ? want_port : 0;
        long fd = oauth_sys(OA_SYS_socket, OA_AF_INET,
                            OA_SOCK_STREAM | OA_SOCK_NONBLOCK | OA_SOCK_CLOEXEC, 0, 0, 0);
        if (fd < 0) return (int)fd;
        int one = 1;
        (void)oauth_sys(OA_SYS_setsockopt, fd, OA_SOL_SOCKET, OA_SO_REUSEADDR,
                        (long)&one, 4);
        struct oa_sockaddr_in sa;
        agentc_memset(&sa, 0, sizeof sa);
        sa.family = OA_AF_INET;
        sa.port_be = (u16)((port << 8) | (port >> 8));
        sa.addr_be = 0x0100007Fu;             /* memory bytes 127.0.0.1 */
        long r = oauth_sys(OA_SYS_bind, fd, (long)&sa, sizeof sa, 0, 0);
        if (r < 0 && attempt == 0) {          /* busy fixed port: fall back to random */
            oauth_sys(3 /* close */, fd, 0, 0, 0, 0);
            continue;
        }
        if (r < 0) {
            oauth_sys(3, fd, 0, 0, 0, 0);
            return (int)r;
        }
        r = oauth_sys(OA_SYS_listen, fd, 1, 0, 0, 0);
        if (r < 0) {
            oauth_sys(3, fd, 0, 0, 0, 0);
            return (int)r;
        }
        if (port == 0) {
            struct oa_sockaddr_in got = { 0 };
            int glen = (int)sizeof got;
            r = oauth_sys(OA_SYS_getsockname, fd, (long)&got, (long)&glen, 0, 0);
            if (r < 0) {
                oauth_sys(3, fd, 0, 0, 0, 0);
                return (int)r;
            }
            port = (u16)((got.port_be << 8) | (got.port_be >> 8));
        }
        s->fd = (int)fd;
        s->port = port;
        return 0;
    }
    return -98;                               /* EADDRINUSE */
}

static int loop_accept(int lfd, int timeout_ms) {
    struct os_pollfd p = { lfd, OS_POLLIN, 0 };
    int r;
    do {
        r = os_poll(&p, 1, timeout_ms);
    } while (r == -4);                /* EINTR: retry the wait */
    if (r < 0) return r;
    if (r == 0) return -110;                  /* ETIMEDOUT */
    long cfd = oauth_sys(OA_SYS_accept4, lfd, 0, 0, OA_SOCK_CLOEXEC, 0);
    return cfd < 0 ? (int)cfd : (int)cfd;
}

static void loop_close(LoopServer *s) {
    if (s->fd >= 0) oauth_sys(3, s->fd, 0, 0, 0, 0);
    s->fd = -1;
}

#else /* !linux x86-64 */
typedef struct {
    int fd;
    u16 port;
} LoopServer;

static int loop_start(u16 want_port, bool strict, LoopServer *s) {
    (void)want_port;
    (void)strict;
    (void)s;
    return -38;                               /* ENOSYS */
}
static int loop_accept(int lfd, int timeout_ms) {
    (void)lfd;
    (void)timeout_ms;
    return -38;
}
static void loop_close(LoopServer *s) { s->fd = -1; }
#endif

static int write_all_fd(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n) {
        int w = os_write(fd, q, n);
        if (w < 0) {
            if (w == -4) continue;
            return w;
        }
        if (w == 0) return -5;
        q += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Read a request head (up to 8 KB) from the accepted socket. */
static int read_request(int fd, i64 deadline, AgcBuf *out) {
    for (;;) {
        if (out->len >= 8192) return 0;
        size_t end = out->len;
        if (end >= 4 && agentc_memeq(out->p + end - 4, "\r\n\r\n", 4)) return 0;
        i64 left = deadline - oauth_now_ms();
        if (left <= 0) return -110;
        struct os_pollfd p = { fd, OS_POLLIN, 0 };
        int r = os_poll(&p, 1, (int)(left > 1000 ? 1000 : left));
        if (r < 0) {
            if (r == -4) continue;
            return r;
        }
        if (r == 0) continue;
        u8 tmp[2048];
        int n = os_read(fd, tmp, sizeof tmp);
        if (n > 0) agentc_buf_push(out, tmp, (size_t)n);
        else if (n == 0) return 0;
        else if (n == -4) continue;             /* EINTR */
        else if (n != -11) return n;
    }
}

/* ------------------------------------------------------------- paste flow */
/* Remote/headless logins cannot complete the loopback redirect: the browser
 * runs on another machine, so http://localhost:<port>/... never reaches us.
 * The user copies the redirect URL from the address bar (or the code the
 * provider shows) and pastes it back here. */

static bool has_char(const char *p, size_t n, char c) {
    for (size_t i = 0; i < n; i++)
        if (p[i] == c) return true;
    return false;
}

/* Accepts any of: the full redirect URL, a bare query ("code=..&state=.."),
 * "code#state" (what the vendor CLI hands back for Claude), or a bare code. */
bool agentc_oauth_parse_pasted_code(const char *in, size_t n, char *code, size_t ccap,
                                    char *state, size_t scap) {
    if (!in || !code || !state || ccap == 0 || scap == 0) return false;
    code[0] = 0;
    state[0] = 0;
    while (n && (in[0] == ' ' || in[0] == '\t' || in[0] == '\r' || in[0] == '\n')) {
        in++;
        n--;
    }
    while (n && (in[n - 1] == ' ' || in[n - 1] == '\t' || in[n - 1] == '\r' ||
                 in[n - 1] == '\n'))
        n--;
    if (!n) return false;

    const char *q = NULL;
    size_t qn = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '?') {
            q = in + i + 1;
            qn = n - i - 1;
            break;
        }
    }
    if (!q && n >= 5 && agentc_memeq(in, "code=", 5)) {
        q = in;
        qn = n;
    }
    if (q) {
        if (!query_param(q, qn, "code", code, ccap)) return false;
        (void)query_param(q, qn, "state", state, scap);
        return code[0] != 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '#') {              /* <code>#<state> */
            size_t cl = i, sl = n - i - 1;
            if (cl >= ccap) cl = ccap - 1;
            agentc_memcpy(code, in, cl);
            code[cl] = 0;
            if (sl >= scap) sl = scap - 1;
            agentc_memcpy(state, in + i + 1, sl);
            state[sl] = 0;
            return code[0] != 0;
        }
    }
    if (n >= ccap) n = ccap - 1;
    agentc_memcpy(code, in, n);
    code[n] = 0;
    return code[0] != 0;
}

/* Drain one chunk of stdin and, once a whole line (or EOF) has arrived, parse
 * it. Returns true when a usable code was found. */
static bool stdin_take_code(AgcBuf *buf, bool *eof, char *code, size_t ccap,
                            char *state, size_t scap) {
    u8 tmp[1024];
    int n = os_read(0, tmp, sizeof tmp);
    if (n == 0) *eof = true;
    else if (n > 0) agentc_buf_push(buf, tmp, (size_t)n);
    else if (n != -11 && n != -4) *eof = true;   /* EAGAIN/EINTR: keep waiting */

    /* A line far longer than any code/state is garbage (or a runaway pipe):
     * drop it instead of buffering without bound until the deadline. */
    if (buf->len > 16384) {
        agentc_buf_clear(buf);
        return false;
    }
    if (buf->len == 0) return false;
    if (!*eof && !has_char((const char *)buf->p, buf->len, '\n')) return false;
    bool ok = agentc_oauth_parse_pasted_code((const char *)buf->p, buf->len, code, ccap,
                                             state, scap);
    agentc_buf_clear(buf);
    return ok;
}

/* True when the loopback redirect cannot come back to this process: a remote
 * SSH session, or a Linux box with no display server. Both fall back to the
 * copy-the-code flow without the user having to ask. */
static bool oauth_prefer_paste(void) {
    if (os_getenv("SSH_CONNECTION") || os_getenv("SSH_CLIENT") || os_getenv("SSH_TTY"))
        return true;
#if defined(__linux__)
    if (!os_getenv("DISPLAY") && !os_getenv("WAYLAND_DISPLAY")) return true;
#endif
    return false;
}

/* The redirect URI the provider has registered for this client, without a
 * listener: used by the paste flow and by the loopback-failure fallback. */
static void fixed_redirect(const OauthProvider *p, const char *rpath, char *out,
                           size_t cap) {
    if (p->fixed_port)
        agentc_snprintf(out, cap, "http://localhost:%u%s", (unsigned)p->fixed_port, rpath);
    else
        agentc_snprintf(out, cap, "http://localhost%s", rpath);
}

/* ---------------------------------------------------------------- login */

static int oauth_login_impl(const char *provider, bool manual) {
    g_err[0] = 0;
    int pidx = provider_index(provider);
    if (pidx < 0) {
        set_errf("unknown oauth provider: %s", provider ? provider : "");
        return -22;
    }
    const OauthProvider *p = &providers[pidx];
    (void)agentc_net_init();

    /* PKCE verifier (43 chars from 32 random bytes) + state (22 chars).
     * Both secrets must come from the CSPRNG: if it fails we fail closed
     * instead of zero-filling and emitting a predictable verifier/state. */
    char verifier[96], challenge[96], state[64];
    u8 vr[32], sr[16];
    int rr = oauth_random(vr, sizeof vr);
    if (rr < 0) {
        set_err("cannot generate secure random data");
        agentc_memset(vr, 0, sizeof vr);
        return rr;
    }
    if (!b64url(vr, sizeof vr, verifier, sizeof verifier)) {
        set_err("cannot generate PKCE verifier");
        agentc_memset(vr, 0, sizeof vr);
        agentc_memset(verifier, 0, sizeof verifier);
        return -1;
    }
    if (agentc_oauth_pkce_challenge(verifier, challenge, sizeof challenge) != 0) {
        set_err("cannot compute PKCE challenge");
        agentc_memset(vr, 0, sizeof vr);
        agentc_memset(verifier, 0, sizeof verifier);
        agentc_memset(challenge, 0, sizeof challenge);
        return -1;
    }
    rr = oauth_random(sr, sizeof sr);
    if (rr < 0) {
        set_err("cannot generate secure random data");
        agentc_memset(vr, 0, sizeof vr);
        agentc_memset(sr, 0, sizeof sr);
        agentc_memset(verifier, 0, sizeof verifier);
        agentc_memset(challenge, 0, sizeof challenge);
        return rr;
    }
    if (!b64url(sr, sizeof sr, state, sizeof state)) {
        set_err("cannot generate oauth state");
        agentc_memset(vr, 0, sizeof vr);
        agentc_memset(sr, 0, sizeof sr);
        agentc_memset(verifier, 0, sizeof verifier);
        agentc_memset(challenge, 0, sizeof challenge);
        agentc_memset(state, 0, sizeof state);
        return -1;
    }

    char code[512] = { 0 };
    char got_state[128] = { 0 };
    char redirect[128];
    char *url = NULL;
    LoopServer srv = { -1, 0 };
    int rc = 0;
    bool paste = manual;

    const char *rpath = (p->redirect_path && p->redirect_path[0]) ? p->redirect_path
                                                                  : "/callback";
    if (g_cb_hook) {
        agentc_snprintf(redirect, sizeof redirect, "http://localhost:0%s", rpath);
    } else if (manual) {
        /* Nothing listens in the paste flow, but the redirect URI is still the
         * one registered for this client, so use the fixed port. */
        fixed_redirect(p, rpath, redirect, sizeof redirect);
    } else {
        rc = loop_start(p->fixed_port, p->strict_port, &srv);
        if (rc < 0) {
            /* A busy registered port, or a platform without a loopback server,
             * is not fatal: offer the paste flow instead. */
            fixed_redirect(p, rpath, redirect, sizeof redirect);
            paste = true;
            rc = 0;
        } else {
            agentc_snprintf(redirect, sizeof redirect, "http://localhost:%u%s",
                            (unsigned)srv.port, rpath);
        }
    }
    url = build_authorize_url(p, redirect, challenge, state);

    agentc_outf("Open this URL to sign in to %s:\n", provider);
    if (g_cb_hook)
        agentc_outf("(browser suppressed by test hook; redirect %s)\n", redirect);
    else
        agentc_outf("%s\n", url);
    if (!g_cb_hook && !paste) {
        int orc = os_open_url(url);
        /* -ENOENT means no desktop opener is installed (bare SSH/NixOS shell,
         * container): the URL above is all we can offer, so stay quiet. Any
         * other failure means an opener was found but would not launch, which
         * is worth a warning. */
        if (orc < 0 && orc != -2)
            agentc_logf(2, "could not open a browser; open the URL above manually");
    }
    if (paste && !g_cb_hook)
        agentc_outs("Paste the full redirect URL from the browser's address bar "
                    "(or just the code) and press Enter:\n");

    if (g_cb_hook) {
        char q[512] = { 0 };
        if (g_cb_hook(g_cb_ud, url, q, sizeof q) == 0) {
            size_t qn = agentc_strlen(q);
            (void)query_param(q, qn, "code", code, sizeof code);
            (void)query_param(q, qn, "state", got_state, sizeof got_state);
        }
    } else {
        i64 deadline = oauth_now_ms() + OAUTH_CALLBACK_TIMEOUT_MS;
        AgcBuf paste_buf = { 0 };
        bool stdin_eof = false;
        int server_tries = 0;
        while (!code[0]) {
            i64 left = deadline - oauth_now_ms();
            if (left <= 0) {
                rc = -110;
                set_err("timed out waiting for the oauth callback");
                break;
            }
            /* Wait on the loopback socket (when there is one) and, in the paste
             * flow, stdin as well. Reading stdin only when a paste is expected
             * keeps terminal typeahead out of the normal browser flow. */
            struct os_pollfd pfds[2];
            int nf = 0, srv_idx = -1, in_idx = -1;
            if (srv.fd >= 0) {
                srv_idx = nf;
                pfds[nf].fd = srv.fd;
                pfds[nf].events = OS_POLLIN;
                pfds[nf].revents = 0;
                nf++;
            }
            if (paste && !stdin_eof) {
                in_idx = nf;
                pfds[nf].fd = 0;
                pfds[nf].events = OS_POLLIN;
                pfds[nf].revents = 0;
                nf++;
            }
            if (nf == 0) {
                rc = -38;
                set_err("no way to receive the oauth code on this platform");
                break;
            }
            int pr = os_poll(pfds, nf, (int)(left > 1000 ? 1000 : left));
            if (pr < 0) {
                if (pr == -4) continue;
                rc = pr;
                break;
            }
            if (pr == 0) continue;

            if (srv_idx >= 0 &&
                (pfds[srv_idx].revents & (OS_POLLIN | OS_POLLERR | OS_POLLHUP))) {
                if (++server_tries > 8) {   /* favicon/stale floods: give up */
                    rc = -110;
                    set_err("timed out waiting for the oauth callback");
                    break;
                }
                int cfd = loop_accept(srv.fd, 0);
                if (cfd >= 0) {
                    AgcBuf req = { 0 };
                    AgcBuf resp = { 0 };
                    (void)read_request(cfd, deadline, &req);
                    (void)agentc_oauth_callback_handle_path((const char *)req.p, req.len,
                                                            rpath, &resp, code, sizeof code,
                                                            got_state, sizeof got_state);
                    (void)write_all_fd(cfd, resp.p, resp.len);
                    agentc_buf_free(&req);
                    agentc_buf_free(&resp);
                    os_close(cfd);
                    if (code[0] && got_state[0] && agentc_streq(got_state, state)) break;
                    code[0] = 0;        /* favicon/stale request: keep waiting */
                    got_state[0] = 0;
                } else if (cfd != -11 && cfd != -110) {
                    rc = cfd;
                    break;
                }
            }
            if (in_idx >= 0 && (pfds[in_idx].revents & (OS_POLLIN | OS_POLLHUP))) {
                if (stdin_take_code(&paste_buf, &stdin_eof, code, sizeof code, got_state,
                                    sizeof got_state)) {
                    if (!got_state[0] || agentc_streq(got_state, state)) break;
                    rc = -1;
                    set_err("oauth state mismatch");
                    break;
                }
            }
        }
        agentc_buf_free(&paste_buf);
    }
    loop_close(&srv);

    if (rc == 0 && !code[0]) {
        rc = -110;
        set_err("timed out waiting for the oauth callback");
    }
    /* A code pasted as a bare value has no state to check; the provider still
     * binds it to our PKCE verifier. When state is present it must match. */
    if (rc == 0 && got_state[0] && !agentc_streq(got_state, state)) {
        rc = -1;
        set_err("oauth state mismatch");
    }
    if (rc == 0 && !got_state[0] && p->token_state) {
        rc = -1;
        set_err("the pasted code is missing its state; paste the full redirect URL");
    }
    if (rc == 0) {
        rc = exchange_code(pidx, code, redirect, verifier, state);
        if (rc == 0) rc = store_save(pidx);
    }
    agentc_free(url);
    if (rc != 0) {
        if (!g_err[0]) set_errf("oauth login failed (errno %d)", rc);
        return rc;
    }
    agentc_outf("Logged in to %s.\n", provider);
    return 0;
}

int agentc_oauth_login(const char *provider) {
    return oauth_login_impl(provider, oauth_prefer_paste());
}

int agentc_oauth_login_manual(const char *provider) {
    return oauth_login_impl(provider, true);
}
