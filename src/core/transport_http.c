/* transport_http.c — AgcTransport over agentc_http_* (wire layer). */
#include "agent.h"
#include "plat.h"
#include "wire.h"

static bool g_insecure;
static int g_http_status;
static i64 g_retry_after_ms;
/* Owned JSON object of the last response's headers; "{}" when none. Replaced on
 * every request so response_headers() can outlive the AgcHttp object. */
static char *g_response_headers;

/* Error bodies are only useful as a short, one-line breadcrumb. Capture a bit
 * more than the final 512 bytes so a credential near the cut is redacted before
 * the excerpt is truncated; nothing beyond ERR_CAPTURE_MAX is kept. */
#define ERR_EXCERPT_MAX 512
#define ERR_CAPTURE_MAX (ERR_EXCERPT_MAX + 512)
static char g_err_excerpt[ERR_CAPTURE_MAX + 1];

int agentc_transport_http_last_status(void) { return g_http_status; }
i64 agentc_transport_http_retry_after_ms(void) { return g_retry_after_ms; }
const char *agentc_transport_http_last_body_excerpt(void) { return g_err_excerpt; }

/* Days from 1970-01-01 to y-m-d in the proleptic Gregorian calendar
 * (Howard Hinnant's algorithm), used for the HTTP-date Retry-After form. */
static i64 days_from_civil(i64 y, int m, int d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int month_from_name(const char *p) {
    static const char *const names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    for (int i = 0; i < 12; i++)
        if (p[0] == names[i][0] && p[1] == names[i][1] && p[2] == names[i][2]) return i + 1;
    return 0;
}

/* Parse an IMF-fixdate ("Wed, 21 Oct 2015 07:28:00 GMT") into unix seconds.
 * A tolerant scanner: weekday and trailing zone are optional, ranges are
 * validated. Returns 0 when the input is not a valid date. */
static i64 http_date_to_unix(const char *v, size_t n) {
    size_t i = 0;
    while (i < n && ((v[i] >= 'A' && v[i] <= 'Z') || (v[i] >= 'a' && v[i] <= 'z'))) i++;
    if (i > 0 && i < n && v[i] == ',') i++;
    while (i < n && v[i] == ' ') i++;
    if (i >= n || v[i] < '0' || v[i] > '9') return 0;
    int day = 0, dlen = 0;
    while (i < n && v[i] >= '0' && v[i] <= '9') {
        if (dlen >= 4) return 0;   /* bound the accumulate against overflow */
        day = day * 10 + (v[i++] - '0');
        dlen++;
    }
    if (i >= n || v[i] != ' ') return 0;
    i++;
    if (i + 3 > n) return 0;
    int mon = month_from_name(v + i);
    if (!mon) return 0;
    i += 3;
    if (i >= n || v[i] != ' ') return 0;
    i++;
    int year = 0, ylen = 0;
    while (i < n && v[i] >= '0' && v[i] <= '9') {
        if (ylen >= 4) return 0;   /* a year is exactly four digits */
        year = year * 10 + (v[i++] - '0');
        ylen++;
    }
    if (ylen != 4 || i >= n || v[i] != ' ') return 0;
    i++;
    int hh = 0, mm = 0, ss = 0;
    if (i + 8 > n || v[i + 2] != ':' || v[i + 5] != ':') return 0;
    for (int k = 0; k < 2; k++) {
        if (v[i + k] < '0' || v[i + k] > '9') return 0;
        hh = hh * 10 + (v[i + k] - '0');
    }
    for (int k = 3; k < 5; k++) {
        if (v[i + k] < '0' || v[i + k] > '9') return 0;
        mm = mm * 10 + (v[i + k] - '0');
    }
    for (int k = 6; k < 8; k++) {
        if (v[i + k] < '0' || v[i + k] > '9') return 0;
        ss = ss * 10 + (v[i + k] - '0');
    }
    if (day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 59) return 0;
    return days_from_civil(year, mon, day) * 86400 + hh * 3600 + mm * 60 + ss;
}

/* Parse Retry-After: an integer number of seconds, or an HTTP-date
 * (IMF-fixdate). Returns a non-negative delay in ms, 0 when absent/unparseable/
 * in the past, and clamps to 1 h. Non-static so the parser is unit-testable. */
i64 agentc_transport_parse_retry_after(const char *v) {
    if (!v) return 0;
    /* header values may carry surrounding whitespace; parse the trimmed token */
    size_t n = agentc_strlen(v);
    while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t' || v[n - 1] == '\r')) n--;
    while (n > 0 && (*v == ' ' || *v == '\t')) {
        v++;
        n--;
    }
    bool ok = false;
    i64 secs = agentc_parse_i64(v, n, &ok);
    if (ok) {
        if (secs <= 0) return 0;
        if (secs > 3600) secs = 3600;
        return secs * 1000;
    }
    i64 date = http_date_to_unix(v, n);
    if (date <= 0) return 0;
    i64 now = os_now_ns(OS_CLOCK_REALTIME) / 1000000000;
    i64 delay = date - now;
    if (delay <= 0) return 0;
    if (delay > 3600) delay = 3600;
    return delay * 1000;
}

/* Case-insensitive substring search over a byte range. */
static bool ci_has(const char *s, size_t n, const char *needle) {
    size_t nl = agentc_strlen(needle);
    if (nl == 0 || nl > n) return false;
    for (size_t i = 0; i + nl <= n; i++)
        if (agentc_str_ieq(s + i, nl, needle, nl)) return true;
    return false;
}

/* Overwrite every occurrence of a request header value that looks like a
 * credential. The response body may echo request context, and the excerpt ends
 * up in an error message a user will paste around. */
static void redact_header_secrets(char *dst, const char *headers) {
    if (!headers) return;
    const char *p = headers;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        size_t len = (size_t)(e - p);
        if (len > 0 && p[len - 1] == '\r') len--;
        const char *colon = NULL;
        for (size_t i = 0; i < len; i++)
            if (p[i] == ':') { colon = p + i; break; }
        if (colon) {
            size_t nlen = (size_t)(colon - p);
            const char *val = colon + 1;
            size_t vlen = len - nlen - 1;
            while (vlen > 0 && (val[0] == ' ' || val[0] == '\t')) { val++; vlen--; }
            while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) vlen--;
            if (ci_has(p, nlen, "authorization") || ci_has(p, nlen, "api-key") ||
                ci_has(p, nlen, "token")) {
                if (vlen > 7 && agentc_str_ieq(val, 7, "bearer ", 7)) { val += 7; vlen -= 7; }
                if (vlen > 0) {
                    char *secret = agentc_alloc(vlen + 1);
                    agentc_memcpy(secret, val, vlen);
                    secret[vlen] = 0;
                    char *hit = dst;
                    while ((hit = (char *)agentc_str_str(hit, secret)) != NULL) {
                        for (size_t i = 0; i < vlen; i++) hit[i] = '*';
                        hit += vlen;
                    }
                    agentc_free(secret);
                }
            }
        }
        p = (*e == '\n') ? e + 1 : e;
    }
}

/* Copy the body into g_err_excerpt as one printable line: control bytes become a
 * single collapsed space, then request credentials are redacted and the result
 * is cut to ERR_EXCERPT_MAX bytes. */
static void set_err_excerpt(const char *body, size_t len, const char *headers) {
    size_t out = 0;
    bool pending_space = false;
    for (size_t i = 0; i < len && out < ERR_CAPTURE_MAX; i++) {
        unsigned char c = (unsigned char)body[i];
        if (c < 0x20 || c == 0x7f) {
            pending_space = out > 0;
            continue;
        }
        if (pending_space) {
            g_err_excerpt[out++] = ' ';
            pending_space = false;
        }
        if (out < ERR_CAPTURE_MAX) g_err_excerpt[out++] = (char)c;
    }
    g_err_excerpt[out] = 0;
    redact_header_secrets(g_err_excerpt, headers);
    if (agentc_strlen(g_err_excerpt) > ERR_EXCERPT_MAX) g_err_excerpt[ERR_EXCERPT_MAX] = 0;
}

typedef struct {
    int (*on_chunk)(void *u, const void *p, size_t n);
    void *u;
    char capture[ERR_CAPTURE_MAX];
    size_t capture_len;
} HttpCtx;

static int http_on_body(void *ud, const void *p, size_t n) {
    HttpCtx *c = ud;
    if (c->capture_len < ERR_CAPTURE_MAX) {
        size_t room = ERR_CAPTURE_MAX - c->capture_len;
        size_t take = n < room ? n : room;
        agentc_memcpy(c->capture + c->capture_len, p, take);
        c->capture_len += take;
    }
    return c->on_chunk(c->u, p, n);
}

static int http_request(void *ud, const char *url, const char *headers, const void *body,
                        size_t body_len, int (*on_chunk)(void *u, const void *p, size_t n),
                        void *u, int timeout_ms, AgcBuf *record) {
    (void)ud;
    g_retry_after_ms = 0;
    g_err_excerpt[0] = 0;
    agentc_http_set_insecure(g_insecure);
    /* agentc_http_new() collapses URL-parse failure and OOM into NULL; check
     * the URL first so a malformed URL is not reported as -ENOMEM. */
    AgcUrl parsed;
    int perr = agentc_url_parse(url, &parsed);
    if (perr != 0) {
        g_http_status = 0;
        agentc_free(g_response_headers);
        g_response_headers = NULL;
        return perr;
    }
    AgcHttp *h = agentc_http_new("POST", url, headers, body, body_len);
    if (!h) {
        g_http_status = 0;
        agentc_free(g_response_headers);
        g_response_headers = NULL;
        return -12; /* ENOMEM */
    }
    if (record) agentc_http_set_record(h, record);
    HttpCtx ctx = { on_chunk, u, { 0 }, 0 };
    int rc = agentc_http_run(h, http_on_body, &ctx, timeout_ms);
    int status = agentc_http_status(h);
    if (rc == 0) g_retry_after_ms = agentc_transport_parse_retry_after(agentc_http_header(h, "retry-after"));
    g_http_status = rc == 0 ? status : 0;
    if (rc == 0 && status >= 400 && ctx.capture_len > 0)
        set_err_excerpt(ctx.capture, ctx.capture_len, headers);
    agentc_free(g_response_headers);
    g_response_headers = agentc_strdup(agentc_http_headers_json(h));
    agentc_http_free(h);
    if (rc != 0) return rc;
    if (status >= 400) return status;
    if (status < 200) return -5; /* EIO */
    return 0;
}

static const char *http_response_headers(void *ud) {
    (void)ud;
    return g_response_headers ? g_response_headers : "{}";
}

AgcTransport agentc_transport_http(bool insecure) {
    g_insecure = insecure;
    AgcTransport t = { http_request, NULL, http_response_headers };
    return t;
}
