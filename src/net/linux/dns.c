/* dns.c — Layer 1 Linux backend: hand-rolled DNS A resolver.
 *
 * No getaddrinfo. Resolution order:
 *   1. dotted-quad fast path (agentc_net_is_ip4);
 *   2. /etc/hosts (first IPv4 token of a line whose alias list matches);
 *   3. the nameservers in /etc/resolv.conf (up to 3), queried over
 *      non-blocking UDP with a per-attempt 1 s budget and a CNAME chain limit
 *      of 8; a truncated (TC) reply is retried over TCP on port 53;
 *      when no nameserver is configured, 1.1.1.1 and 8.8.8.8 are used.
 *
 * deadline_ms is a duration, not an absolute time. Errors are negative linux
 * errno: -EINVAL bad host, -ENOENT no A record, -ETIMEDOUT deadline.
 */
#include "net/net_internal.h"

#define DNS_PORT 53
#define DNS_ATTEMPT_MS 1000
#define DNS_RESP_MAX 2048
#define DNS_TCP_RESP_MAX 8192
#define DNS_FILE_MAX 16384
#define DNS_MAX_SERVERS 3
#define DNS_MAX_HOPS 8

#define DNS_EAGAIN (-11)
#define DNS_EINVAL (-22)
#define DNS_EINPROGRESS (-115)
#define DNS_EINTR (-4)
#define DNS_ENOENT (-2)
#define DNS_EIO (-5)
#define DNS_EPROTO (-71)
#define DNS_ETIMEDOUT (-110)

/* DNS_TC: reply was truncated, retry over TCP. */
#define DNS_TC 2

/* ------------------------------------------------------------------ utils */
static u16 rd16(const u8 *p) { return (u16)(((u16)p[0] << 8) | (u16)p[1]); }

static bool deadline_passed(i64 end_ns) {
    return end_ns != 0 && os_now_ns(OS_CLOCK_MONOTONIC) >= end_ns;
}

static int budget_ms(i64 end_ns, int cap_ms) {
    if (end_ns == 0) return cap_ms;
    i64 rem = (end_ns - os_now_ns(OS_CLOCK_MONOTONIC)) / 1000000;
    if (rem <= 0) return 0;
    return rem < cap_ms ? (int)rem : cap_ms;
}

/* Wait for events, bounded by both cap_ms and the absolute deadline. */
static int wait_ready(int fd, short events, i64 end_ns, int cap_ms) {
    struct os_pollfd p = { fd, events, 0 };
    for (;;) {
        int to = budget_ms(end_ns, cap_ms);
        if (to <= 0) return DNS_ETIMEDOUT;
        int r = os_poll(&p, 1, to);
        if (r == DNS_EINTR) continue;
        if (r < 0) return r;
        if (r == 0) return DNS_ETIMEDOUT;   /* nothing became ready */
        return 0;
    }
}

static int read_file(const char *path, char *buf, size_t cap) {
    int fd = os_open(path, OS_O_RDONLY | OS_O_CLOEXEC, 0);
    if (fd < 0) return fd;
    size_t n = 0;
    while (n + 1 < cap) {
        int r = os_read(fd, buf + n, cap - n - 1);
        if (r == DNS_EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    os_close(fd);
    buf[n] = '\0';
    return (int)n;
}

/* ------------------------------------------------------------ IPv4 literal */
bool agentc_net_is_ip4(const char *host, struct agentc_ip4 *out) {
    if (host == NULL) return false;
    u8 o[4];
    int part = 0, val = 0, digits = 0;
    for (;;) {
        char c = *host;
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            if (++digits > 3 || val > 255) return false;
            host++;
        } else if (c == '.') {
            if (digits == 0 || part == 3) return false;
            o[part++] = (u8)val;
            val = 0;
            digits = 0;
            host++;
        } else if (c == '\0') {
            if (digits == 0 || part != 3) return false;
            o[3] = (u8)val;
            if (out != NULL) agentc_memcpy(out->b, o, 4);
            return true;
        } else {
            return false;
        }
    }
}

/* ----------------------------------------------------------- name handling */
/* Lowercase, one optional trailing dot removed; DNS_EINVAL when malformed. */
static int name_normalize(const char *host, char *out, size_t cap) {
    size_t n = agentc_strlen(host);
    if (n == 0 || n > 254) return DNS_EINVAL;
    if (host[n - 1] == '.') n--;
    if (n == 0 || n + 1 > cap) return DNS_EINVAL;
    size_t label = 0, total = 0;
    for (size_t i = 0; i < n; i++) {
        u8 c = (u8)host[i];
        if (c == '.') {
            if (label == 0 || label > 63) return DNS_EINVAL;
            label = 0;
            total++;
        } else {
            if (c <= ' ' || c == 127) return DNS_EINVAL;
            label++;
        }
    }
    if (label == 0 || label > 63 || total > 253) return DNS_EINVAL;
    for (size_t i = 0; i < n; i++) {
        char c = host[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    out[n] = '\0';
    return (int)n;
}

static bool name_eq(const char *a, size_t alen, const char *b, size_t blen) {
    while (alen > 0 && a[alen - 1] == '.') alen--;
    while (blen > 0 && b[blen - 1] == '.') blen--;
    if (alen != blen) return false;
    for (size_t i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
    }
    return true;
}

/* --------------------------------------------------------------- /etc/hosts */
static bool hosts_lookup(const char *text, size_t len, const char *host,
                         size_t hlen, struct agentc_ip4 *out) {
    size_t i = 0;
    while (i < len) {
        size_t ls = i;
        while (i < len && text[i] != '\n') i++;
        size_t le = i;
        if (i < len) i++;
        while (le > ls && (text[le - 1] == '\r' || text[le - 1] == ' ' || text[le - 1] == '\t'))
            le--;
        size_t p = ls;
        while (p < le && (text[p] == ' ' || text[p] == '\t')) p++;
        if (p >= le || text[p] == '#') continue;
        size_t ts = p;
        while (p < le && text[p] != ' ' && text[p] != '\t') p++;
        char ipbuf[24];
        size_t tl = p - ts;
        if (tl == 0 || tl >= sizeof(ipbuf)) continue;
        agentc_memcpy(ipbuf, text + ts, tl);
        ipbuf[tl] = '\0';
        struct agentc_ip4 ip;
        if (!agentc_net_is_ip4(ipbuf, &ip)) continue;
        while (p < le && text[p] != '#') {
            while (p < le && (text[p] == ' ' || text[p] == '\t')) p++;
            if (p >= le || text[p] == '#') break;
            size_t hs = p;
            while (p < le && text[p] != ' ' && text[p] != '\t' && text[p] != '#') p++;
            if (name_eq(text + hs, p - hs, host, hlen)) {
                *out = ip;
                return true;
            }
        }
    }
    return false;
}

/* ----------------------------------------------------------- /etc/resolv.conf */
static size_t resolv_scan(const char *text, size_t len, u32 out[DNS_MAX_SERVERS]) {
    size_t count = 0;
    size_t i = 0;
    while (i < len && count < DNS_MAX_SERVERS) {
        size_t ls = i;
        while (i < len && text[i] != '\n') i++;
        size_t le = i;
        if (i < len) i++;
        while (le > ls && (text[le - 1] == '\r' || text[le - 1] == ' ' || text[le - 1] == '\t'))
            le--;
        size_t p = ls;
        while (p < le && (text[p] == ' ' || text[p] == '\t')) p++;
        static const char kw[] = "nameserver";
        size_t k = 0;
        while (k < sizeof(kw) - 1 && p < le && text[p] == kw[k]) {
            p++;
            k++;
        }
        if (k != sizeof(kw) - 1) continue;
        while (p < le && (text[p] == ' ' || text[p] == '\t')) p++;
        size_t ts = p;
        while (p < le && text[p] != ' ' && text[p] != '\t') p++;
        if (p == ts || p - ts >= 24) continue;
        char ipbuf[24];
        agentc_memcpy(ipbuf, text + ts, p - ts);
        ipbuf[p - ts] = '\0';
        struct agentc_ip4 ip;
        if (!agentc_net_is_ip4(ipbuf, &ip)) continue; /* IPv6 nameservers are skipped */
        out[count++] = agentc_ip4_word(&ip);
    }
    return count;
}

/* ------------------------------------------------------------- DNS wire I/O */
/* Offset just past the (possibly compressed) name at off, or -1. */
static i64 skip_name(const u8 *b, size_t n, size_t off) {
    while (off < n) {
        u8 c = b[off];
        if ((c & 0xC0) == 0xC0) return off + 2 <= n ? (i64)off + 2 : -1;
        if (c & 0xC0) return -1;
        if (c == 0) return (i64)off + 1;
        off += 1 + c;
        if (off > n) return -1;
    }
    return -1;
}

/* Decode a (possibly compressed) name into lowercase dotted form; returns the
 * offset just past the name at the cursor, or -1. */
static i64 read_name(const u8 *b, size_t n, size_t off, char *out, size_t cap) {
    size_t o = 0, pos = off, ret = 0;
    int hops = 0;
    bool jumped = false;
    for (;;) {
        if (pos >= n || hops > 32) return -1;
        u8 c = b[pos];
        if ((c & 0xC0) == 0xC0) {
            if (pos + 1 >= n) return -1;
            if (!jumped) ret = pos + 2;
            pos = (size_t)((c & 0x3F) << 8) | b[pos + 1];
            jumped = true;
            hops++;
            continue;
        }
        if (c & 0xC0) return -1;
        if (c == 0) {
            if (!jumped) ret = pos + 1;
            break;
        }
        if (c > 63 || pos + 1 + c > n || o + c + 2 > cap) return -1;
        if (o) out[o++] = '.';
        for (size_t i = 0; i < c; i++) {
            char ch = (char)b[pos + 1 + i];
            out[o++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
        }
        pos += 1 + c;
    }
    out[o] = '\0';
    return (i64)ret;
}

static bool name_equals_off(const u8 *b, size_t n, size_t off, const char *name) {
    char tmp[256];
    if (read_name(b, n, off, tmp, sizeof(tmp)) < 0) return false;
    return name_eq(tmp, agentc_strlen(tmp), name, agentc_strlen(name));
}

static int build_query(const char *name, u16 id, u8 *buf, size_t cap) {
    if (cap < 12) return DNS_EINVAL;
    agentc_memset(buf, 0, 12);
    buf[0] = (u8)(id >> 8);
    buf[1] = (u8)id;
    buf[2] = 0x01; /* RD */
    buf[5] = 1;    /* QDCOUNT */
    size_t o = 12;
    const char *p = name;
    while (*p) {
        size_t l = 0;
        while (p[l] != '\0' && p[l] != '.') l++;
        if (l == 0 || l > 63 || o + 1 + l + 5 > cap) return DNS_EINVAL;
        buf[o++] = (u8)l;
        agentc_memcpy(buf + o, p, l);
        o += l;
        p += l;
        if (*p == '.') p++;
    }
    buf[o++] = 0;
    buf[o++] = 0; buf[o++] = 1; /* A */
    buf[o++] = 0; buf[o++] = 1; /* IN */
    return (int)o;
}

/* Parse an answer. Returns 0 A found, 1 CNAME target in cname, DNS_EAGAIN ignore
 * packet, DNS_ENOENT valid negative answer, DNS_EPROTO malformed, DNS_TC truncated. */
static int parse_response(const u8 *b, size_t n, u16 id, const char *qname,
                          struct agentc_ip4 *out, char *cname, size_t cname_cap,
                          bool *tc) {
    if (n < 12) return DNS_EPROTO;
    if (rd16(b) != id) return DNS_EAGAIN;
    u16 flags = rd16(b + 2);
    if (!(flags & 0x8000)) return DNS_EPROTO;
    if (flags & 0x0200) {
        if (tc) *tc = true;
        return DNS_TC;
    }
    if ((flags & 0x000F) != 0) return DNS_ENOENT;
    u16 qd = rd16(b + 4), an = rd16(b + 6);
    size_t off = 12;
    for (u16 i = 0; i < qd; i++) {
        size_t qstart = off;
        i64 e = skip_name(b, n, off);
        if (e < 0 || (size_t)e + 4 > n) return DNS_EPROTO;
        if (i == 0) {
            if (!name_equals_off(b, n, qstart, qname)) return DNS_EAGAIN;
            if (rd16(b + e) != 1 || rd16(b + e + 2) != 1) return DNS_EPROTO;
        }
        off = (size_t)e + 4;
    }
    char current[256];
    agentc_memcpy(current, qname, agentc_strlen(qname) + 1);
    bool have_cname = false;
    for (u16 i = 0; i < an; i++) {
        size_t owner = off;
        i64 e = skip_name(b, n, off);
        if (e < 0 || (size_t)e + 10 > n) return DNS_EPROTO;
        off = (size_t)e;
        u16 type = rd16(b + off), cls = rd16(b + off + 2);
        u16 rdlen = rd16(b + off + 8);
        size_t rd = off + 10;
        if (rd + rdlen > n) return DNS_EPROTO;
        if (cls == 1 && type == 5) {
            char nm[256];
            if (read_name(b, n, rd, nm, sizeof(nm)) < 0) return DNS_EPROTO;
            if (name_equals_off(b, n, owner, current)) {
                agentc_memcpy(current, nm, agentc_strlen(nm) + 1);
                have_cname = true;
            }
        } else if (cls == 1 && type == 1 && rdlen == 4) {
            if (name_equals_off(b, n, owner, current)) {
                agentc_memcpy(out->b, b + rd, 4);
                return 0;
            }
        }
        off = rd + rdlen;
    }
    if (have_cname) {
        size_t l = agentc_strlen(current);
        if (l + 1 > cname_cap) return DNS_EPROTO;
        agentc_memcpy(cname, current, l + 1);
        return 1;
    }
    return DNS_ENOENT;
}

/* ------------------------------------------------------------- UDP exchange */
static int udp_exchange(u32 server, const u8 *query, size_t qlen, u16 id,
                        const char *qname, struct agentc_ip4 *out, char *cname,
                        size_t cname_cap, i64 end_ns) {
    int fd = agentc_net_udp_socket();
    if (fd < 0) return fd;
    int rc = agentc_net_connect(fd, server, DNS_PORT, 0);
    if (rc == DNS_EINPROGRESS) {
        rc = wait_ready(fd, OS_POLLOUT, end_ns, DNS_ATTEMPT_MS);
        if (rc == 0) rc = agentc_net_so_error(fd);
    }
    if (rc < 0) {
        os_close(fd);
        return rc;
    }
    size_t sent = 0;
    while (sent < qlen) {
        if (deadline_passed(end_ns)) { os_close(fd); return DNS_ETIMEDOUT; }
        int n = agentc_net_send(fd, query + sent, qlen - sent);
        if (n == DNS_EAGAIN) {
            rc = wait_ready(fd, OS_POLLOUT, end_ns, DNS_ATTEMPT_MS);
            if (rc < 0) { os_close(fd); return rc; }
            continue;
        }
        if (n < 0) { os_close(fd); return n; }
        sent += (size_t)n;
    }
    u8 rbuf[DNS_RESP_MAX];
    int result = DNS_ENOENT;
    for (;;) {
        if (deadline_passed(end_ns)) { result = DNS_ETIMEDOUT; break; }
        rc = wait_ready(fd, OS_POLLIN, end_ns, DNS_ATTEMPT_MS);
        if (rc < 0) { result = rc; break; }
        if (deadline_passed(end_ns)) { result = DNS_ETIMEDOUT; break; }
        int n = agentc_net_recv(fd, rbuf, sizeof(rbuf));
        if (n == DNS_EAGAIN) continue;
        if (n < 0) { result = n; break; }
        if (n == 0) continue;
        bool tc = false;
        result = parse_response(rbuf, (size_t)n, id, qname, out, cname, cname_cap, &tc);
        if (result == DNS_EAGAIN) continue;
        break;
    }
    os_close(fd);
    return result;
}

/* ------------------------------------------------------------- TCP fallback */
static int tcp_send_all(int fd, const u8 *p, size_t n, i64 end_ns) {
    size_t sent = 0;
    while (sent < n) {
        if (deadline_passed(end_ns)) return DNS_ETIMEDOUT;
        int r = agentc_net_send(fd, p + sent, n - sent);
        if (r == DNS_EAGAIN) {
            int w = wait_ready(fd, OS_POLLOUT, end_ns, DNS_ATTEMPT_MS);
            if (w < 0) return w;
            continue;
        }
        if (r < 0) return r;
        sent += (size_t)r;
    }
    return 0;
}

static int tcp_recv_all(int fd, u8 *p, size_t n, i64 end_ns) {
    size_t got = 0;
    while (got < n) {
        if (deadline_passed(end_ns)) return DNS_ETIMEDOUT;
        int r = agentc_net_recv(fd, p + got, n - got);
        if (r == DNS_EAGAIN) {
            int w = wait_ready(fd, OS_POLLIN, end_ns, DNS_ATTEMPT_MS);
            if (w < 0) return w;
            continue;
        }
        if (r < 0) return r;
        if (r == 0) return DNS_EIO;
        got += (size_t)r;
    }
    return 0;
}

static int tcp_exchange(u32 server, const u8 *query, size_t qlen, u16 id,
                        const char *qname, struct agentc_ip4 *out, char *cname,
                        size_t cname_cap, i64 end_ns) {
    int fd = agentc_net_socket();
    if (fd < 0) return fd;
    int rc = agentc_net_connect(fd, server, DNS_PORT, 0);
    if (rc == DNS_EINPROGRESS) {
        rc = wait_ready(fd, OS_POLLOUT, end_ns, DNS_ATTEMPT_MS);
        if (rc == 0) rc = agentc_net_so_error(fd);
    }
    if (rc < 0) {
        agentc_net_close(fd);
        return rc;
    }
    u8 framed[512 + 2];
    if (qlen > 512) {
        agentc_net_close(fd);
        return DNS_EINVAL;
    }
    framed[0] = (u8)(qlen >> 8);
    framed[1] = (u8)qlen;
    agentc_memcpy(framed + 2, query, qlen);
    rc = tcp_send_all(fd, framed, qlen + 2, end_ns);
    if (rc < 0) {
        agentc_net_close(fd);
        return rc;
    }
    u8 lenbuf[2];
    rc = tcp_recv_all(fd, lenbuf, 2, end_ns);
    if (rc < 0) {
        agentc_net_close(fd);
        return rc;
    }
    size_t rlen = rd16(lenbuf);
    if (rlen < 12 || rlen > DNS_TCP_RESP_MAX) {
        agentc_net_close(fd);
        return DNS_EPROTO;
    }
    u8 rbuf[DNS_TCP_RESP_MAX];
    rc = tcp_recv_all(fd, rbuf, rlen, end_ns);
    if (rc < 0) {
        agentc_net_close(fd);
        return rc;
    }
    agentc_net_close(fd);
    bool tc = false;
    return parse_response(rbuf, rlen, id, qname, out, cname, cname_cap, &tc);
}

/* ------------------------------------------------------------------- public */
int agentc_net_dns(const char *host, struct agentc_ip4 *out, int deadline_ms) {
    if (host == NULL || out == NULL || host[0] == '\0') return DNS_EINVAL;

    struct agentc_ip4 ip;
    if (agentc_net_is_ip4(host, &ip)) {
        *out = ip;
        return 0;
    }
    char name[256];
    int nl = name_normalize(host, name, sizeof(name));
    if (nl < 0) return nl;

    i64 end_ns = 0;
    if (deadline_ms > 0) end_ns = os_now_ns(OS_CLOCK_MONOTONIC) + (i64)deadline_ms * 1000000;

    char filebuf[DNS_FILE_MAX];
    int fl = read_file("/etc/hosts", filebuf, sizeof(filebuf));
    if (fl > 0 && hosts_lookup(filebuf, (size_t)fl, name, (size_t)nl, &ip)) {
        *out = ip;
        return 0;
    }

    u32 servers[DNS_MAX_SERVERS];
    size_t nservers = 0;
    fl = read_file("/etc/resolv.conf", filebuf, sizeof(filebuf));
    if (fl > 0) nservers = resolv_scan(filebuf, (size_t)fl, servers);
    if (nservers == 0) {
        servers[nservers++] = 0x01010101; /* 1.1.1.1 */
        servers[nservers++] = 0x08080808; /* 8.8.8.8 */
    }

    char current[256];
    agentc_memcpy(current, name, (size_t)nl + 1);
    for (int hop = 0; hop < DNS_MAX_HOPS; hop++) {
        if (deadline_passed(end_ns)) return DNS_ETIMEDOUT;
        u8 idbuf[2];
        if (os_random(idbuf, sizeof(idbuf)) != 0) {
            idbuf[0] = 0x5A;
            idbuf[1] = 0x42;
        }
        u16 id = rd16(idbuf);
        u8 query[512];
        int qlen = build_query(current, id, query, sizeof(query));
        if (qlen < 0) return DNS_EINVAL;

        char cname[256];
        bool have_cname = false;
        int last = DNS_ENOENT;
        for (size_t s = 0; s < nservers; s++) {
            if (deadline_passed(end_ns)) return DNS_ETIMEDOUT;
            int r = udp_exchange(servers[s], query, (size_t)qlen, id, current, &ip,
                                 cname, sizeof(cname), end_ns);
            if (r == DNS_TC) {
                r = tcp_exchange(servers[s], query, (size_t)qlen, id, current, &ip, cname,
                                 sizeof(cname), end_ns);
            }
            if (r == 0) {
                *out = ip;
                return 0;
            }
            if (r == 1) {
                size_t l = agentc_strlen(cname);
                if (l + 1 > sizeof(current)) return DNS_EPROTO;
                agentc_memcpy(current, cname, l + 1);
                have_cname = true;
                break;
            }
            if (r != DNS_ENOENT) last = r;
        }
        if (!have_cname) {
            if (last == DNS_ETIMEDOUT || deadline_passed(end_ns)) return DNS_ETIMEDOUT;
            return DNS_ENOENT;
        }
    }
    return DNS_ENOENT; /* CNAME chain too long */
}
