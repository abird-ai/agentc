/* url.c — minimal absolute-URL parser for http:// and https://.
 *
 * Supported:  [scheme://][userinfo@]host[:port][/path][?query][#fragment]
 *   - scheme is matched case-insensitively, only http/https;
 *   - host is copied verbatim (brackets stripped for IPv6 literals);
 *   - path starts with '/' and carries the query string, the fragment is
 *     dropped; a missing path becomes "/";
 *   - default ports are 80 (http) and 443 (https).
 *
 * Errors: -EINVAL malformed, -ENAMETOOLONG host/path over the fixed AgcUrl
 * limits. The parser allocates nothing and never reads past the NUL.
 */
#include "net/net_internal.h"

#define U_EINVAL (-22)
#define U_ENAMETOOLONG (-36)

static bool ci_prefix(const char *p, const char *s) {
    for (; *s != '\0'; s++, p++) {
        char a = *p, b = *s;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return true;
}

static int parse_port(const char *p, const char *end, u16 *out) {
    if (p >= end) return U_EINVAL;
    u32 v = 0;
    for (; p < end; p++) {
        if (*p < '0' || *p > '9') return U_EINVAL;
        v = v * 10 + (u32)(*p - '0');
        if (v > 65535) return U_EINVAL;
    }
    if (v == 0) return U_EINVAL;
    *out = (u16)v;
    return 0;
}

int agentc_url_parse(const char *url, AgcUrl *out) {
    if (url == NULL || out == NULL) return U_EINVAL;
    agentc_memset(out, 0, sizeof(*out));

    const char *p = url;
    if (ci_prefix(p, "https://")) {
        out->https = true;
        out->port = 443;
        p += 8;
    } else if (ci_prefix(p, "http://")) {
        out->https = false;
        out->port = 80;
        p += 7;
    } else {
        return U_EINVAL;
    }

    const char *auth_end = p;
    while (*auth_end != '\0' && *auth_end != '/' && *auth_end != '?' && *auth_end != '#') auth_end++;

    /* userinfo is dropped from the host, as browsers do. */
    const char *host = p;
    for (const char *q = p; q < auth_end; q++) {
        if (*q == '@') host = q + 1;
    }

    const char *host_end;
    if (host < auth_end && *host == '[') {
        const char *close = host + 1;
        while (close < auth_end && *close != ']') close++;
        if (close >= auth_end) return U_EINVAL;
        host++;
        host_end = close;
        if (close + 1 < auth_end) {
            if (close[1] != ':') return U_EINVAL;
            int rc = parse_port(close + 2, auth_end, &out->port);
            if (rc != 0) return rc;
        }
    } else {
        host_end = host;
        while (host_end < auth_end && *host_end != ':') host_end++;
        if (host_end < auth_end) {
            int rc = parse_port(host_end + 1, auth_end, &out->port);
            if (rc != 0) return rc;
        }
    }

    size_t hlen = (size_t)(host_end - host);
    if (hlen == 0) return U_EINVAL;
    if (hlen >= sizeof(out->host)) return U_ENAMETOOLONG;
    agentc_memcpy(out->host, host, hlen);
    out->host[hlen] = '\0';
    /* request-line/header injection guard: no CTLs, spaces or DEL in the host */
    for (size_t i = 0; i < hlen; i++) {
        u8 c = (u8)out->host[i];
        if (c <= 0x20 || c == 0x7f) return -22;
    }

    /* Origin-form target: /path[?query], fragment removed. */
    const char *rest = auth_end;
    const char *frag = rest;
    while (*frag != '\0' && *frag != '#') frag++;
    size_t rlen = (size_t)(frag - rest);
    size_t need = (rlen > 0 && *rest == '?') ? rlen + 1 : (rlen > 0 ? rlen : 1);
    if (need >= sizeof(out->path)) return U_ENAMETOOLONG;
    size_t o = 0;
    if (rlen == 0 || *rest != '/') out->path[o++] = '/';
    if (rlen > 0) {
        agentc_memcpy(out->path + o, rest, rlen);
        o += rlen;
    }
    out->path[o] = '\0';
    for (size_t i = 0; i < o; i++) {
        u8 c = (u8)out->path[i];
        if (c < 0x21 || c == 0x7f) return -22;   /* CTLs/space would forge headers */
    }
    return 0;
}
