/* http.c — HTTP/1.1 client: request builder, response head parser, body
 * decoder (Content-Length, chunked with trailers, EOF-delimited) and the
 * internal poll loop that drives both plain TCP and AgcTls.
 *
 * Behaviour decisions (contract in include/wire.h):
 *   - headers are sent as given, normalised to CRLF line endings;
 *   - "Connection: close" is always requested; the client does not implement
 *     keep-alive and never follows redirects (the 3xx status is returned);
 *   - on_body is invoked once per body chunk as it is decoded; returning
 *     nonzero aborts the request and agentc_http_run() returns -ECANCELED;
 *   - agentc_http_set_record() appends every byte received after the connection
 *     is established (for TLS that is the decrypted byte stream), suitable
 *     for the replay backend;
 *   - timeout_ms is an overall wall-clock budget for connect + send + receive;
 *     <= 0 means no deadline.
 *
 * Plain sockets and TLS are non-blocking; -EAGAIN polls using agentc_tls_want()
 * for TLS and POLLOUT/POLLIN for plain sockets.
 */
#include "net/net_internal.h"
#include "base/deadline.h"
#include "base/limits.h"

static bool g_insecure;
void agentc_http_set_insecure(bool on) { g_insecure = on; }

/* Optional UI pump: called before each internal poll wait so an interactive
 * front end can read input and redraw while a request is in flight. The hook
 * must return promptly; `timeout_ms` is the wait budget about to be used
 * (-1 = indefinite) and is only a hint. */
static AgcPollHook g_poll_hook;
static void *g_poll_ud;
void agentc_http_set_poll_hook(AgcPollHook hook, void *ud) {
    g_poll_hook = hook;
    g_poll_ud = ud;
}

#define H_EAGAIN (-11)
#define H_EINVAL (-22)
#define H_ECANCELED (-125)
#define H_ECONNRESET (-104)
#define H_E2BIG (-7)
#define H_EINPROGRESS (-115)
#define H_EIO (-5)
#define H_EPROTO (-71)
#define H_ETIMEDOUT (-110)

#define H_READ_CHUNK 16384

enum {
    HS_HEAD = 0,
    HS_LEN,
    HS_CHUNK_SIZE,
    HS_CHUNK_DATA,
    HS_CHUNK_CRLF,
    HS_TRAILERS,
    HS_EOF,
    HS_DONE,
};

typedef struct {
    u32 name_off, name_len, val_off, val_len;
} Hdr;

struct AgcHttp {
    char *method;
    AgcBuf req;
    AgcUrl url;
    char err[256];

    int fd;
    AgcTls *tls;
    AgcBuf *record;

    int status;
    AgcBuf hdr_raw;
    AgcVec hdrs;
    AgcBuf hdr_json;         /* agentc_http_headers_json() scratch */

    AgcBuf in;
    size_t in_pos;

    int state;
    u64 body_left;
    u64 chunk_left;
    bool recv_eof;
    bool done;
    bool aborted;
    bool started;
    size_t sent;
    const volatile bool *cancel;   /* borrowed; NULL = no cancellation */
};

void agentc_http_set_cancel(AgcHttp *h, const volatile bool *cancel) {
    if (h != NULL) h->cancel = cancel;
}

/* ------------------------------------------------------------------ errors */
static int http_err(AgcHttp *h, int rc, const char *fmt, const char *a) {
    if (h != NULL) agentc_snprintf(h->err, sizeof(h->err), fmt, a);
    return rc;
}

/* --------------------------------------------------------------- utilities */
static bool ci_eq(const char *a, size_t alen, const char *b) {
    for (size_t i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (y == '\0') return false;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
    }
    return b[alen] == '\0';
}


static const char *hdr_get(const AgcHttp *h, const char *name) {
    if (h == NULL || h->hdr_raw.p == NULL) return NULL;
    for (size_t i = 0; i < h->hdrs.len; i++) {
        const Hdr *e = &((const Hdr *)h->hdrs.p)[i];
        if (ci_eq((const char *)h->hdr_raw.p + e->name_off, e->name_len, name)) {
            return (const char *)h->hdr_raw.p + e->val_off;
        }
    }
    return NULL;
}

static size_t hdr_count(const AgcHttp *h, const char *name) {
    size_t n = 0;
    if (h == NULL || h->hdr_raw.p == NULL) return 0;
    for (size_t i = 0; i < h->hdrs.len; i++) {
        const Hdr *e = &((const Hdr *)h->hdrs.p)[i];
        if (ci_eq((const char *)h->hdr_raw.p + e->name_off, e->name_len, name)) n++;
    }
    return n;
}

/* Wait until fd is ready or the absolute AgcDeadline passes. Returns 0
 * ready, -ETIMEDOUT, or -ECANCELED when the cancel predicate turned true. */
static int wait_fd(AgcHttp *h, short events, AgcDeadline d) {
    struct os_pollfd p = { h->fd, events, 0 };
    for (;;) {
        if (h->cancel && *h->cancel) return H_ECANCELED;
        int to = -1;
        if (agentc_deadline_set(d)) {
            if (agentc_deadline_expired(d)) return H_ETIMEDOUT;
            to = agentc_deadline_remaining_ms(d, 1000);
        }
        /* A front end needs to read input and redraw while we wait: cap the
         * blocking slice so keystrokes are never delayed by a silent server. */
        if (g_poll_hook && (to < 0 || to > 30)) to = 30;
        /* Likewise a cancel predicate must stay responsive without a poll
         * hook; a short slice bounds how long a silent server can hold us. */
        if (h->cancel && (to < 0 || to > 50)) to = 50;
        if (g_poll_hook) g_poll_hook(g_poll_ud, to);
        int r = os_poll(&p, 1, to);
        if (r == -4 /* EINTR */) continue;
        if (r < 0) return 0; /* mock/fake fds: let the caller retry */
        return 0;
    }
}

/* wait_fd error reporting: a cancelled wait keeps the explicit "cancelled"
 * diagnostic; every other failure keeps the call-site message. */
static int wait_err(AgcHttp *h, int rc, const char *what) {
    if (rc == H_ECANCELED) return http_err(h, rc, "cancelled%s", "");
    return http_err(h, rc, what, "");
}

/* -------------------------------------------------------- request building */
static void buf_crlf(AgcBuf *b) { agentc_buf_cstr(b, "\r\n"); }

static void append_user_headers(AgcBuf *b, const char *headers) {
    const char *p = headers;
    while (*p != '\0') {
        const char *e = p;
        while (*e != '\0' && *e != '\n') e++;
        size_t len = (size_t)(e - p);
        if (len > 0 && p[len - 1] == '\r') len--;
        if (len > 0) {
            agentc_buf_push(b, p, len);
            buf_crlf(b);
        }
        p = (*e == '\n') ? e + 1 : e;
    }
}

AgcHttp *agentc_http_new(const char *method, const char *url,
                            const char *headers, const void *body,
                            size_t body_len) {
    if (method == NULL || method[0] == '\0' || url == NULL) return NULL;
    (void)agentc_net_init();

    AgcHttp *h = agentc_alloc(sizeof(*h));
    h->fd = -1;
    h->method = agentc_strdup(method);
    if (agentc_url_parse(url, &h->url) != 0) {
        agentc_free(h->method);
        agentc_free(h);
        return NULL;
    }

    AgcBuf *b = &h->req;
    agentc_buf_cstr(b, method);
    agentc_buf_byte(b, ' ');
    agentc_buf_cstr(b, h->url.path);
    agentc_buf_cstr(b, " HTTP/1.1\r\nHost: ");
    bool v6 = agentc_str_str(h->url.host, ":") != NULL;
    if (v6) agentc_buf_byte(b, '[');
    agentc_buf_cstr(b, h->url.host);
    if (v6) agentc_buf_byte(b, ']');
    bool default_port = h->url.https ? h->url.port == 443 : h->url.port == 80;
    if (!default_port) {
        agentc_buf_byte(b, ':');
        agentc_buf_u64(b, h->url.port);
    }
    buf_crlf(b);
    if (headers != NULL) append_user_headers(b, headers);
    agentc_buf_cstr(b, "Connection: close\r\n");
    if (body != NULL || body_len > 0) {
        agentc_buf_cstr(b, "Content-Length: ");
        agentc_buf_u64(b, body_len);
        buf_crlf(b);
    }
    buf_crlf(b);
    if (body_len > 0 && body != NULL) agentc_buf_push(b, body, body_len);
    return h;
}

/* ------------------------------------------------------------ status/head */
static int parse_status_line(const char *p, size_t n, int *status) {
    if (n < 12) return H_EPROTO;
    if (!(p[0] == 'H' && p[1] == 'T' && p[2] == 'T' && p[3] == 'P' && p[4] == '/'))
        return H_EPROTO;
    size_t i = 5;
    while (i < n && p[i] != ' ') i++;
    if (i >= n || i + 2 > n) return H_EPROTO;
    i++;
    int st = 0, digits = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
        st = st * 10 + (p[i] - '0');
        i++;
        digits++;
    }
    if (digits != 3) return H_EPROTO;
    *status = st;
    return 0;
}

/* Tokenize CRLF header lines in hdr_raw[start,end) into h->hdrs; the value
 * of every line is NUL-terminated in place. A blank line ends the block. */
static int tokenize_headers(AgcHttp *h, size_t start, size_t end) {
    u8 *p = h->hdr_raw.p;
    size_t o = start;
    while (o < end) {
        size_t le = o;
        while (le + 1 < end && !(p[le] == '\r' && p[le + 1] == '\n')) le++;
        if (le + 1 >= end) break;
        if (le == o) break; /* blank line */
        if (p[o] == ' ' || p[o] == '\t') {
            /* a folded continuation line (leading SP/HTAB) is a
             * request-smuggling vector: reject it */
            return H_EPROTO;
        }
        size_t colon = o;
        while (colon < le && p[colon] != ':') colon++;
        if (colon >= le) {
            /* a line without a colon is malformed */
            return H_EPROTO;
        }
        size_t ns = o, ne = colon;
        while (ne > ns && (p[ne - 1] == ' ' || p[ne - 1] == '\t')) ne--;
        size_t vs = colon + 1, ve = le;
        while (vs < ve && (p[vs] == ' ' || p[vs] == '\t')) vs++;
        while (ve > vs && (p[ve - 1] == ' ' || p[ve - 1] == '\t')) ve--;
        p[ve] = '\0';
        if (h->hdrs.len >= AGENTC_LIMIT_HTTP_HEADER_LINES) return H_E2BIG;
        if (ne > ns) {
            Hdr *e = agentc_vec_push(&h->hdrs, sizeof(*e));
            e->name_off = (u32)ns;
            e->name_len = (u32)(ne - ns);
            e->val_off = (u32)vs;
            e->val_len = (u32)(ve - vs);
        }
        o = le + 2;
    }
    return 0;
}

static bool method_is_head(const AgcHttp *h) {
    return ci_eq(h->method, agentc_strlen(h->method), "HEAD");
}

/* Parse the head already copied to the start of the receive buffer. */
static int parse_head(AgcHttp *h, size_t off, size_t len) {
    const char *p = (const char *)h->in.p + off;
    i64 eol = agentc_str_find(p, len, "\r\n", 2);
    if (eol < 0) return H_EPROTO;
    int st = 0;
    if (parse_status_line(p, (size_t)eol, &st) != 0) return H_EPROTO;
    h->status = st;

    agentc_buf_clear(&h->hdr_raw);
    h->hdrs.len = 0;
    agentc_buf_push(&h->hdr_raw, p, len);
    int rc = tokenize_headers(h, (size_t)eol + 2, len);
    if (rc != 0) return rc;

    /* A message may carry at most one Content-Length and one
     * Transfer-Encoding. Duplicates are a request-smuggling vector: different
     * parsers pick different values, so reject rather than choose the first. */
    if (hdr_count(h, "Content-Length") > 1) return H_EPROTO;
    if (hdr_count(h, "Transfer-Encoding") > 1) return H_EPROTO;

    h->body_left = 0;
    h->chunk_left = 0;
    if (method_is_head(h) || st == 204 || st == 304 || (st >= 100 && st < 200)) {
        h->state = HS_DONE;
        return 0;
    }
    const char *te = hdr_get(h, "Transfer-Encoding");
    bool chunked = false;
    if (te != NULL) {
        /* tokenize the comma list; a substring test would match "notchunked" */
        const char *q = te;
        while (*q) {
            while (*q == ' ' || *q == '\t' || *q == ',') q++;
            const char *tok = q;
            while (*q && *q != ',' && *q != ';') q++;
            size_t tl = (size_t)(q - tok);
            while (tl && (tok[tl - 1] == ' ' || tok[tl - 1] == '\t')) tl--;
            if (tl == 7 && agentc_str_ieq(tok, tl, "chunked", 7)) { chunked = true; break; }
            while (*q && *q != ',') q++;
        }
    }
    const char *cl = hdr_get(h, "Content-Length");
    if (chunked) {
        if (cl != NULL) return H_EPROTO;         /* request smuggling vector */
        h->state = HS_CHUNK_SIZE;
        return 0;
    }
    if (cl != NULL) {
        /* digits only, no sign, no spaces, no overflow */
        u64 v = 0;
        bool ok = *cl != 0;
        for (const char *d = cl; *d; d++) {
            if (*d < '0' || *d > '9') { ok = false; break; }
            u64 nv = v * 10 + (u64)(*d - '0');
            if (v > (0x7fffffffffffffffull - (u64)(*d - '0')) / 10) { ok = false; break; }
            v = nv;
        }
        if (!ok) return H_EPROTO;
        h->body_left = v;
        h->state = v > 0 ? HS_LEN : HS_DONE;
        return 0;
    }
    h->state = HS_EOF; /* HTTP/1.0-style body terminated by connection close */
    return 0;
}

static size_t find_crlf(const u8 *p, size_t n) {
    for (size_t i = 0; i + 1 < n; i++) {
        if (p[i] == '\r' && p[i + 1] == '\n') return i;
    }
    return (size_t)-1;
}

static bool parse_hex_u64(const char *p, size_t n, u64 *out) {
    u64 v = 0;
    size_t i = 0;
    if (n == 0) return false;
    while (i < n && p[i] != ';') {
        char c = p[i];
        u32 d;
        if (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
        else return false;
        if (v > (~(u64)0 - d) / 16) return false;
        v = v * 16 + d;
        i++;
    }
    if (i == 0) return false;
    *out = v;
    return true;
}

/* Consume buffered input; invokes on_body per decoded body slice. */
static int http_process(AgcHttp *h, int (*on_body)(void *, const void *, size_t), void *ud) {
    for (;;) {
        size_t avail = h->in.len - h->in_pos;
        const u8 *p = h->in.p + h->in_pos;
        switch (h->state) {
        case HS_HEAD: {
            i64 idx = agentc_str_find((const char *)p, avail, "\r\n\r\n", 4);
            if (idx < 0) {
                if (avail > AGENTC_LIMIT_HTTP_HEADER_BYTES) return http_err(h, H_E2BIG, "head too large%s", "");
                goto done;
            }
            size_t head_len = (size_t)idx + 4;
            /* The cap applies to the whole head, not just to the unframed
             * prefix: a terminator found in the same read must not bypass it. */
            if (head_len > AGENTC_LIMIT_HTTP_HEADER_BYTES)
                return http_err(h, H_E2BIG, "head too large%s", "");
            int rc = parse_head(h, h->in_pos, head_len);
            h->in_pos += head_len;
            if (rc != 0) return http_err(h, rc, "bad response head%s", "");
            if (h->status >= 100 && h->status < 200 && h->status != 101) {
                h->state = HS_HEAD; /* interim response: read the next head */
            }
            continue;
        }
        case HS_LEN: {
            if (avail == 0) goto done;
            size_t take = avail;
            if ((u64)take > h->body_left) take = (size_t)h->body_left;
            if (on_body != NULL && take > 0 && on_body(ud, p, take) != 0) {
                h->aborted = true;
                return http_err(h, H_ECANCELED, "cancelled%s", "");
            }
            h->in_pos += take;
            h->body_left -= take;
            if (h->body_left == 0) h->state = HS_DONE;
            continue;
        }
        case HS_CHUNK_SIZE: {
            size_t le = find_crlf(p, avail);
            if (le == (size_t)-1) {
                if (avail > AGENTC_LIMIT_HTTP_CHUNK_LINE_BYTES) return http_err(h, H_EPROTO, "chunk line too long%s", "");
                goto done;
            }
            u64 sz = 0;
            if (!parse_hex_u64((const char *)p, le, &sz))
                return http_err(h, H_EPROTO, "bad chunk size%s", "");
            h->in_pos += le + 2;
            h->chunk_left = sz;
            h->state = sz > 0 ? HS_CHUNK_DATA : HS_TRAILERS;
            continue;
        }
        case HS_CHUNK_DATA: {
            if (avail == 0) goto done;
            size_t take = avail;
            if ((u64)take > h->chunk_left) take = (size_t)h->chunk_left;
            if (on_body != NULL && take > 0 && on_body(ud, p, take) != 0) {
                h->aborted = true;
                return http_err(h, H_ECANCELED, "cancelled%s", "");
            }
            h->in_pos += take;
            h->chunk_left -= take;
            if (h->chunk_left == 0) h->state = HS_CHUNK_CRLF;
            continue;
        }
        case HS_CHUNK_CRLF: {
            if (avail == 0) goto done;
            if (p[0] == '\r') {
                if (avail < 2) goto done;
                if (p[1] != '\n') return http_err(h, H_EPROTO, "bad chunk terminator%s", "");
                h->in_pos += 2;
            } else if (p[0] == '\n') {
                h->in_pos += 1;
            } else {
                return http_err(h, H_EPROTO, "bad chunk terminator%s", "");
            }
            h->state = HS_CHUNK_SIZE;
            continue;
        }
        case HS_TRAILERS: {
            size_t le = find_crlf(p, avail);
            if (le == (size_t)-1) {
                if (avail > AGENTC_LIMIT_HTTP_CHUNK_LINE_BYTES) return http_err(h, H_EPROTO, "trailer line too long%s", "");
                goto done;
            }
            if (le == 0) {
                h->in_pos += 2;
                h->state = HS_DONE;
                continue;
            }
            size_t start = h->hdr_raw.len;
            if (start + le + 2 > AGENTC_LIMIT_HTTP_HEADER_BYTES) return http_err(h, H_E2BIG, "trailers too large%s", "");
            agentc_buf_push(&h->hdr_raw, p, le + 2);
            int rc = tokenize_headers(h, start, h->hdr_raw.len);
            if (rc != 0) return http_err(h, rc, "bad trailer%s", "");
            h->in_pos += le + 2;
            continue;
        }
        case HS_EOF: {
            if (avail == 0) goto done;
            if (on_body != NULL && on_body(ud, p, avail) != 0) {
                h->aborted = true;
                return http_err(h, H_ECANCELED, "cancelled%s", "");
            }
            h->in_pos += avail;
            continue;
        }
        case HS_DONE:
        default:
            goto done;
        }
    }
done:
    h->done = h->state == HS_DONE;
    if (h->in_pos == h->in.len) {
        agentc_buf_clear(&h->in);
        h->in_pos = 0;
    } else if (h->in_pos > 0) {
        size_t left = h->in.len - h->in_pos;
        agentc_memmove(h->in.p, h->in.p + h->in_pos, left);
        h->in.len = left;
        h->in_pos = 0;
    }
    return 0;
}

/* ------------------------------------------------------------ connection */
static int http_connect(AgcHttp *h, AgcDeadline d) {
    if (h->started) return http_err(h, H_EPROTO, "request already used%s", "");
    h->started = true;
    struct agentc_ip4 ip;
    int rc;
    if (agentc_net_is_ip4(h->url.host, &ip)) {
        rc = 0;
    } else {
        int dur = 2000;
        if (agentc_deadline_set(d)) {
            i64 rem = agentc_deadline_remaining_ms(d, dur);
            if (rem <= 0) return http_err(h, H_ETIMEDOUT, "timeout%s", "");
            dur = (int)rem;
        }
        rc = agentc_net_dns(h->url.host, &ip, dur);
        if (rc != 0) return http_err(h, rc, "dns: %s", h->url.host);
    }
    h->fd = agentc_net_socket();
    if (h->fd < 0) return http_err(h, h->fd, "socket%s", "");
    if ((rc = agentc_net_connect(h->fd, agentc_ip4_word(&ip), h->url.port, 0)) == H_EINPROGRESS) {
        rc = wait_fd(h, OS_POLLOUT, d);
        if (rc == 0) rc = agentc_net_so_error(h->fd);
    }
    if (rc != 0) return wait_err(h, rc, "connect%s");

    if (h->url.https) {
        unsigned flags = AGENTC_TLS_VERIFY | AGENTC_TLS_MIN_1_2;
        /* Verification is ON by default. Only an explicit falsy value turns it
         * off: AGENTC_TLS_VERIFY=0|false|no|off (case-insensitive). Anything else
         * (including "true", "" or junk) keeps certificates verified. */
        const char *v = os_getenv("AGENTC_TLS_VERIFY");
        bool env_off = false, env_set = false;
        if (v != NULL) {
            static const char *off[] = { "0", "false", "no", "off" };
            for (size_t i = 0; i < sizeof off / sizeof off[0]; i++) {
                size_t n = agentc_strlen(off[i]);
                if (agentc_str_ieq(v, agentc_strlen(v), off[i], n)) { env_off = true; env_set = true; break; }
            }
            if (!env_off) env_set = true;   /* explicit truthy/unknown value: stay on */
        }
        if ((env_set && env_off) || (!env_set && g_insecure)) flags &= ~AGENTC_TLS_VERIFY;
        /* an IP literal has no name to put in SNI (RFC 6066) */
        struct agentc_ip4 dummy;
        if (agentc_net_is_ip4(h->url.host, &dummy) ||
            agentc_str_str(h->url.host, ":") != NULL)
            flags |= AGENTC_TLS_NO_SNI;
        h->tls = agentc_tls_new(h->fd, h->url.host, h->url.port, flags);
        if (h->tls == NULL) return http_err(h, H_EIO, "tls init: %s", agentc_tls_error(NULL));
        for (;;) {
            rc = agentc_tls_handshake(h->tls);
            if (rc == 0) break;
            if (rc == H_EAGAIN) {
                int w = agentc_tls_want(h->tls);
                short ev = w == 2 ? OS_POLLOUT : (w == 1 ? OS_POLLIN : (OS_POLLIN | OS_POLLOUT));
                rc = wait_fd(h, ev, d);
                if (rc != 0) return wait_err(h, rc, "tls wait%s");
                continue;
            }
            return http_err(h, rc, "tls: %s", agentc_tls_error(h->tls));
        }
    }
    return 0;
}

static int http_send_request(AgcHttp *h, AgcDeadline d) {
    while (h->sent < h->req.len) {
        if (h->cancel && *h->cancel) return wait_err(h, H_ECANCELED, "send wait%s");
        size_t left = h->req.len - h->sent;
        int n;
        if (h->tls != NULL) {
            n = agentc_tls_write(h->tls, h->req.p + h->sent, left);
        } else {
            n = agentc_net_send(h->fd, h->req.p + h->sent, left);
        }
        if (n == H_EAGAIN) {
            short ev = OS_POLLOUT;
            if (h->tls != NULL) {
                int w = agentc_tls_want(h->tls);
                ev = w == 1 ? OS_POLLIN : OS_POLLOUT;
            }
            int rc = wait_fd(h, ev, d);
            if (rc != 0) return wait_err(h, rc, "send wait%s");
            continue;
        }
        if (n < 0) return http_err(h, n, "send%s", "");
        if (n == 0) return http_err(h, H_EIO, "send eof%s", "");
        if (h->record != NULL) agentc_buf_push(h->record, h->req.p + h->sent, (size_t)n);
        h->sent += (size_t)n;
    }
    return 0;
}

static int http_receive(AgcHttp *h, int (*on_body)(void *, const void *, size_t),
                         void *ud, AgcDeadline d) {
    u8 *buf = agentc_alloc(H_READ_CHUNK);
    int rc = 0;
    while (!h->done && !h->aborted) {
        if (h->cancel && *h->cancel) {
            rc = wait_err(h, H_ECANCELED, "recv wait%s");
            break;
        }
        if (agentc_deadline_expired(d)) {
            rc = http_err(h, H_ETIMEDOUT, "timeout%s", "");
            break;
        }
        int n;
        if (h->tls != NULL) {
            n = agentc_tls_read(h->tls, buf, H_READ_CHUNK);
        } else {
            n = agentc_net_recv(h->fd, buf, H_READ_CHUNK);
        }
        if (n == H_EAGAIN) {
            short ev = OS_POLLIN;
            if (h->tls != NULL) {
                int w = agentc_tls_want(h->tls);
                ev = w == 2 ? OS_POLLOUT : (w == 1 ? OS_POLLIN : (OS_POLLIN | OS_POLLOUT));
            }
            rc = wait_fd(h, ev, d);
            if (rc != 0) {
                wait_err(h, rc, "recv wait%s");
                break;
            }
            continue;
        }
        if (n == -4 /* EINTR */) continue;
        if (n < 0) {
            rc = http_err(h, n, "recv%s", "");
            break;
        }
        if (n == 0) {
            h->recv_eof = true;
            if (h->state == HS_EOF) {
                rc = http_process(h, on_body, ud);
                if (rc == 0 && h->state == HS_EOF) {
                    h->state = HS_DONE;
                    h->done = true;
                }
            }
            if (rc == 0 && !h->done) {
                rc = http_err(h, H_ECONNRESET, "premature eof%s", "");
            }
            break;
        }
        if (h->record != NULL) agentc_buf_push(h->record, buf, (size_t)n);
        agentc_buf_push(&h->in, buf, (size_t)n);
        rc = http_process(h, on_body, ud);
        if (rc != 0) break;
    }
    agentc_free(buf);
    if (rc != 0) return rc;
    if (h->aborted) return H_ECANCELED;
    if (!h->done) return http_err(h, H_ETIMEDOUT, "timeout%s", "");
    return 0;
}

int agentc_http_run(AgcHttp *h, int (*on_body)(void *, const void *, size_t),
                        void *ud, int timeout_ms) {
    if (h == NULL) return H_EINVAL;
    if (h->done) return 0;
    if (h->aborted) return H_ECANCELED;

    AgcDeadline d = agentc_deadline_none();
    if (timeout_ms > 0) d = agentc_deadline_after_ms(timeout_ms);
    h->err[0] = '\0';
    if (h->cancel && *h->cancel) return wait_err(h, H_ECANCELED, "cancelled%s");

    int rc = http_connect(h, d);
    if (rc != 0) return rc;
    rc = http_send_request(h, d);
    if (rc != 0) return rc;
    return http_receive(h, on_body, ud, d);
}

int agentc_http_status(const AgcHttp *h) { return h == NULL ? 0 : h->status; }

const char *agentc_http_header(const AgcHttp *h, const char *name) {
    return hdr_get(h, name);
}

const char *agentc_http_headers_json(const AgcHttp *h) {
    if (h == NULL || h->hdrs.len == 0) return "{}";
    /* The function is logically const; the JSON is a per-response cache owned
     * by the http object. */
    AgcHttp *m = (AgcHttp *)h;
    agentc_buf_clear(&m->hdr_json);
    AgcJsonW w;
    agentc_jsonw_init(&w, &m->hdr_json);
    /* Header bytes are attacker-controlled and need not be valid UTF-8: force
     * \uXXXX escaping so every byte yields a valid JSON string. */
    agentc_jsonw_set_ascii(&w, true);
    agentc_jsonw_obj(&w);
    for (size_t i = 0; i < h->hdrs.len; i++) {
        const Hdr *e = &((const Hdr *)h->hdrs.p)[i];
        const char *name = (const char *)h->hdr_raw.p + e->name_off;
        /* duplicate names: the last occurrence wins, so skip every occurrence
         * that is not the last one */
        bool later = false;
        for (size_t j = i + 1; j < h->hdrs.len && !later; j++) {
            const Hdr *e2 = &((const Hdr *)h->hdrs.p)[j];
            later = agentc_str_ieq(name, e->name_len,
                                   (const char *)h->hdr_raw.p + e2->name_off, e2->name_len);
        }
        if (later) continue;
        AgcBuf k = { 0 };
        agentc_buf_push(&k, name, e->name_len);   /* NUL-terminated copy */
        agentc_jsonw_key(&w, (const char *)k.p);
        agentc_buf_free(&k);
        agentc_jsonw_str(&w, (const char *)h->hdr_raw.p + e->val_off, e->val_len);
    }
    agentc_jsonw_end(&w);
    return (const char *)m->hdr_json.p;
}

const char *agentc_http_error(const AgcHttp *h) {
    return (h == NULL || h->err[0] == '\0') ? "" : h->err;
}

void agentc_http_set_record(AgcHttp *h, AgcBuf *sink) {
    if (h != NULL) h->record = sink;
}

void agentc_http_free(AgcHttp *h) {
    if (h == NULL) return;
    if (h->tls != NULL) agentc_tls_close(h->tls);
    if (h->fd >= 0) agentc_net_close(h->fd);
    agentc_free(h->method);
    agentc_buf_free(&h->req);
    agentc_buf_free(&h->hdr_raw);
    agentc_buf_free(&h->hdr_json);
    agentc_vec_free(&h->hdrs);
    agentc_buf_free(&h->in);
    agentc_free(h);
}
