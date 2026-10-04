/* mock.c — scripted replay backend for the offline test suite.
 *
 * ./build.sh test links this file instead of the src/net/linux backend, so tests never
 * touch the network. A script is an ASCII text file, one directive per line,
 * with leading/trailing spaces ignored, '#' starting a comment and blank
 * lines ignored. Directives are applied in order:
 *
 *   dns HOST IP4    add a fixed A answer; HOST is matched case-insensitively
 *                   with an optional trailing dot, first match wins
 *   short N         the next recv returns at most N bytes (N > 0); if data
 *                   remains queued afterwards the next recv returns -EAGAIN
 *                   once, so short reads/replay resume paths are exercised
 *   data HEX        queue server->client bytes, HEX is an even number of
 *                   hex digits (may span multiple recv calls)
 *   eagain          queue one -EAGAIN result
 *   rst             queue one -ECONNRESET result; the stream is dead afterwards
 *   eof             queue one EOF (recv returns 0)
 *   reset           clear the capture, the queue and the DNS answers
 *
 * recv drains one queued item per call; when the queue is empty recv returns 0
 * (EOF), so a script that ends without eof still terminates. Every byte the
 * client writes is appended to the capture buffer returned by agentc_mock_sent().
 *
 * TLS is plaintext passthrough: agentc_tls_new/handshake/read/write/close simply
 * wrap the mock socket, so an https URL replays the same script. A single
 * connection at a time is supported (the tests are deterministic and
 * single-threaded); agentc_net_socket() hands out increasing fake descriptors.
 */
#include "net/net_internal.h"

#define MK_ERR_EAGAIN (-11)
#define MK_ERR_ECONNRESET (-104)
#define MK_ERR_EINVAL (-22)
#define MK_ERR_ENOENT (-2)
#define MK_ERR_E2BIG (-7)
#define MK_SCRIPT_MAX (1024 * 1024)

enum {
    MK_DATA = 0,
    MK_SEG_EAGAIN,
    MK_SEG_RST,
    MK_SEG_EOF,
};

typedef struct {
    u8 kind;
    u32 off, len;
    u8 *p;
} MockSeg;

typedef struct {
    char host[256];
    struct agentc_ip4 ip;
} MockDns;

static AgcVec g_segs;
static AgcBuf g_sent;
static MockDns g_dns[16];
static size_t g_ndns;
static size_t g_cur;
static bool g_dead;
static bool g_force_eagain;
static int g_short_cap;
static int g_next_fd = 4242;

static void push_seg(u8 kind) {
    MockSeg *s = agentc_vec_push(&g_segs, sizeof(*s));
    s->kind = kind;
}

static void reset_all(void) {
    for (size_t i = 0; i < g_segs.len; i++) agentc_free(((MockSeg *)g_segs.p)[i].p);
    g_segs.len = 0;
    agentc_buf_clear(&g_sent);
    g_ndns = 0;
    g_cur = 0;
    g_dead = false;
    g_force_eagain = false;
    g_short_cap = 0;
}

void agentc_mock_reset(void) { reset_all(); }

static void set_dns_entry(const char *host, size_t hlen, const struct agentc_ip4 *ip) {
    while (hlen > 0 && host[hlen - 1] == '.') hlen--;
    if (hlen == 0 || hlen >= sizeof(g_dns[0].host)) return;
    for (size_t i = 0; i < g_ndns; i++) {
        size_t l = agentc_strlen(g_dns[i].host);
        if (l == hlen) {
            bool same = true;
            for (size_t j = 0; j < hlen; j++) {
                char a = g_dns[i].host[j], b = host[j];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) same = false;
            }
            if (same) {
                g_dns[i].ip = *ip;
                return;
            }
        }
    }
    if (g_ndns >= 16) return;
    for (size_t j = 0; j < hlen; j++) {
        char c = host[j];
        g_dns[g_ndns].host[j] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    g_dns[g_ndns].host[hlen] = '\0';
    g_dns[g_ndns].ip = *ip;
    g_ndns++;
}

void agentc_mock_set_dns(const char *host, const struct agentc_ip4 *ip) {
    if (host != NULL && ip != NULL) set_dns_entry(host, agentc_strlen(host), ip);
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_u64_dec(const char *p, size_t n, u64 *out) {
    u64 v = 0;
    if (n == 0) return -1;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (u64)(p[i] - '0');
    }
    *out = v;
    return 0;
}

int agentc_mock_load(const char *path) {
    reset_all();
    int fd = os_open(path, OS_O_RDONLY | OS_O_CLOEXEC, 0);
    if (fd < 0) return fd;
    AgcBuf text = { 0 };
    for (;;) {
        if (text.len >= MK_SCRIPT_MAX) {
            os_close(fd);
            agentc_buf_free(&text);
            return MK_ERR_E2BIG;
        }
        u8 *dst = agentc_buf_reserve(&text, 4096);
        int n = os_read(fd, dst, 4096);
        if (n == -4) continue;
        if (n < 0) {
            os_close(fd);
            agentc_buf_free(&text);
            return n;
        }
        if (n == 0) break;
        text.len += (size_t)n;
    }
    os_close(fd);

    size_t i = 0;
    int rc = 0;
    while (i < text.len) {
        size_t ls = i;
        while (i < text.len && text.p[i] != '\n') i++;
        size_t le = i;
        if (i < text.len) i++;
        while (le > ls && (text.p[le - 1] == '\r' || text.p[le - 1] == ' ' || text.p[le - 1] == '\t'))
            le--;
        size_t p = ls;
        while (p < le && (text.p[p] == ' ' || text.p[p] == '\t')) p++;
        if (p >= le || text.p[p] == '#') continue;

        size_t ts = p;
        while (p < le && text.p[p] != ' ' && text.p[p] != '\t') p++;
        const char *word = (const char *)text.p + ts;
        size_t wlen = p - ts;
        while (p < le && (text.p[p] == ' ' || text.p[p] == '\t')) p++;
        size_t as = p;
        while (p < le && text.p[p] != ' ' && text.p[p] != '\t') p++;
        const char *arg = (const char *)text.p + as;
        size_t alen = p - as;

        if (agentc_str_eq(word, wlen, "reset", 5)) {
            reset_all();
        } else if (agentc_str_eq(word, wlen, "eagain", 6)) {
            push_seg(MK_SEG_EAGAIN);
        } else if (agentc_str_eq(word, wlen, "rst", 3)) {
            push_seg(MK_SEG_RST);
        } else if (agentc_str_eq(word, wlen, "eof", 3)) {
            push_seg(MK_SEG_EOF);
        } else if (agentc_str_eq(word, wlen, "short", 5)) {
            u64 n;
            if (parse_u64_dec(arg, alen, &n) != 0 || n == 0 || n > 0x7FFFFFFF) {
                rc = MK_ERR_EINVAL;
                break;
            }
            g_short_cap = (int)n;
        } else if (agentc_str_eq(word, wlen, "dns", 3)) {
            size_t hs = as;
            while (hs < le && text.p[hs] != ' ' && text.p[hs] != '\t') hs++;
            size_t ip_start = hs;
            while (ip_start < le && (text.p[ip_start] == ' ' || text.p[ip_start] == '\t')) ip_start++;
            char ipbuf[24];
            size_t iplen = le - ip_start;
            struct agentc_ip4 ip;
            if (alen == 0 || iplen == 0 || iplen >= sizeof(ipbuf)) {
                rc = MK_ERR_EINVAL;
                break;
            }
            agentc_memcpy(ipbuf, text.p + ip_start, iplen);
            ipbuf[iplen] = '\0';
            if (!agentc_net_is_ip4(ipbuf, &ip)) {
                rc = MK_ERR_EINVAL;
                break;
            }
            set_dns_entry(arg, alen, &ip);
        } else if (agentc_str_eq(word, wlen, "data", 4)) {
            if (alen % 2 != 0 || alen == 0) {
                rc = MK_ERR_EINVAL;
                break;
            }
            size_t blen = alen / 2;
            u8 *bytes = agentc_alloc(blen);
            for (size_t j = 0; j < blen; j++) {
                int hi = hex_val(arg[2 * j]), lo = hex_val(arg[2 * j + 1]);
                if (hi < 0 || lo < 0) {
                    agentc_free(bytes);
                    rc = MK_ERR_EINVAL;
                    break;
                }
                bytes[j] = (u8)((hi << 4) | lo);
            }
            if (rc != 0) break;
            push_seg(MK_DATA);
            MockSeg *s = &((MockSeg *)g_segs.p)[g_segs.len - 1];
            s->p = bytes;
            s->len = (u32)blen;
            s->off = 0;
        } else {
            rc = MK_ERR_EINVAL;
            break;
        }
    }
    agentc_buf_free(&text);
    return rc;
}

/* ------------------------------------------------------------ net backend */
int agentc_net_init(void) { return 0; }

int agentc_net_socket(void) { return g_next_fd++; }

int agentc_net_connect(int fd, u32 ip_be, u16 port, int deadline_ms) {
    (void)fd;
    (void)ip_be;
    (void)port;
    (void)deadline_ms;
    return 0;
}

int agentc_net_send(int fd, const void *p, size_t n) {
    (void)fd;
    if (p == NULL && n > 0) return MK_ERR_EINVAL;
    agentc_buf_push(&g_sent, p, n);
    return (int)n;
}

int agentc_net_recv(int fd, void *p, size_t n) {
    (void)fd;
    if (g_dead) return MK_ERR_ECONNRESET;
    if (g_force_eagain) {
        g_force_eagain = false;
        return MK_ERR_EAGAIN;
    }
    if (g_cur >= g_segs.len) return 0;
    MockSeg *s = &((MockSeg *)g_segs.p)[g_cur];
    switch (s->kind) {
    case MK_SEG_EAGAIN:
        g_cur++;
        return MK_ERR_EAGAIN;
    case MK_SEG_RST:
        g_cur++;
        g_dead = true;
        return MK_ERR_ECONNRESET;
    case MK_SEG_EOF:
        g_cur++;
        return 0;
    default: {
        size_t left = (size_t)s->len - s->off;
        size_t take = left < n ? left : n;
        if (g_short_cap > 0 && take > (size_t)g_short_cap) {
            take = (size_t)g_short_cap;
            g_short_cap = 0;
            if (take < left) g_force_eagain = true;
        }
        if (take > 0) agentc_memcpy(p, s->p + s->off, take);
        s->off += (u32)take;
        if (s->off == s->len) g_cur++;
        return (int)take;
    }
    }
}

void agentc_net_close(int fd) { (void)fd; }

int agentc_net_so_error(int fd) {
    (void)fd;
    return 0;
}

int agentc_net_set_nodelay(int fd) {
    (void)fd;
    return 0;
}

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

int agentc_net_dns(const char *host, struct agentc_ip4 *out, int deadline_ms) {
    (void)deadline_ms;
    if (host == NULL || out == NULL) return MK_ERR_EINVAL;
    for (size_t i = 0; i < g_ndns; i++) {
        size_t l = agentc_strlen(g_dns[i].host);
        if (l != agentc_strlen(host)) continue;
        bool same = true;
        for (size_t j = 0; j < l; j++) {
            char a = g_dns[i].host[j], b = host[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) same = false;
        }
        if (same) {
            *out = g_dns[i].ip;
            return 0;
        }
    }
    return MK_ERR_ENOENT;
}

const AgcBuf *agentc_mock_sent(void) { return &g_sent; }

/* ----------------------------------------------------------- TLS passthrough */
struct AgcTls {
    int fd;
    int want;
};

AgcTls *agentc_tls_new(int fd, const char *host, u16 port, unsigned flags) {
    (void)host;
    (void)port;
    (void)flags;
    AgcTls *t = agentc_alloc(sizeof(*t));
    t->fd = fd;
    t->want = 0;
    return t;
}

int agentc_tls_handshake(AgcTls *t) {
    (void)t;
    return 0;
}

int agentc_tls_read(AgcTls *t, void *p, size_t n) {
    int r = agentc_net_recv(t->fd, p, n);
    t->want = r == MK_ERR_EAGAIN ? 1 : 0;
    return r;
}

int agentc_tls_write(AgcTls *t, const void *p, size_t n) {
    int r = agentc_net_send(t->fd, p, n);
    t->want = r == MK_ERR_EAGAIN ? 2 : 0;
    return r;
}

void agentc_tls_close(AgcTls *t) { agentc_free(t); }

const char *agentc_tls_error(AgcTls *t) {
    (void)t;
    return "";
}

int agentc_tls_want(const AgcTls *t) { return t == NULL ? 0 : t->want; }
