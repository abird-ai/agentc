/* str.c — memory and string primitives. */
#include "agentc.h"

void *agentc_memcpy(void *d, const void *s, size_t n) {
    u8 *dd = d;
    const u8 *ss = s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}

void *agentc_memmove(void *d, const void *s, size_t n) {
    u8 *dd = d;
    const u8 *ss = s;
    if (dd < ss) {
        for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    } else if (dd > ss) {
        for (size_t i = n; i > 0; i--) dd[i - 1] = ss[i - 1];
    }
    return d;
}

void *agentc_memset(void *d, int c, size_t n) {
    u8 *dd = d;
    for (size_t i = 0; i < n; i++) dd[i] = (u8)c;
    return d;
}

bool agentc_memeq(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return false;
    return true;
}

size_t agentc_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

bool agentc_str_eq(const char *a, size_t alen, const char *b, size_t blen) {
    return alen == blen && agentc_memeq(a, b, alen);
}

bool agentc_str_eq_cstr(const char *a, size_t alen, const char *cstr) {
    return agentc_str_eq(a, alen, cstr, agentc_strlen(cstr));
}

bool agentc_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

bool agentc_str_starts(const char *a, size_t alen, const char *prefix) {
    size_t n = agentc_strlen(prefix);
    return alen >= n && agentc_memeq(a, prefix, n);
}

static u8 lower(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c + 32) : c; }

bool agentc_str_ieq(const char *a, size_t alen, const char *b, size_t blen) {
    if (alen != blen) return false;
    for (size_t i = 0; i < alen; i++)
        if (lower((u8)a[i]) != lower((u8)b[i])) return false;
    return true;
}

char *agentc_strdup_len(const char *s, size_t n) {
    char *p = agentc_alloc(n + 1);
    agentc_memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *agentc_strdup(const char *s) { return agentc_strdup_len(s, agentc_strlen(s)); }

const char *agentc_str_str(const char *hay, const char *needle) {
    i64 i = agentc_str_find(hay, agentc_strlen(hay), needle, agentc_strlen(needle));
    return i < 0 ? NULL : hay + i;
}

i64 agentc_str_find(const char *hay, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0) return 0;
    if (nlen > hlen) return -1;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (agentc_memeq(hay + i, needle, nlen)) return (i64)i;
    return -1;
}

u64 agentc_parse_u64(const char *p, size_t n, bool *ok) {
    u64 v = 0;
    if (n == 0) {
        if (ok) *ok = false;
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            if (ok) *ok = false;
            return 0;
        }
        u64 d = (u64)(p[i] - '0');
        if (v > (~(u64)0 - d) / 10) {          /* would overflow u64 */
            if (ok) *ok = false;
            return 0;
        }
        v = v * 10 + d;
    }
    if (ok) *ok = true;
    return v;
}

i64 agentc_parse_i64(const char *p, size_t n, bool *ok) {
    size_t i = 0;
    bool neg = false;
    if (i < n && (p[i] == '-' || p[i] == '+')) neg = p[i++] == '-';
    if (i == n) {
        if (ok) *ok = false;
        return 0;
    }
    /* Parse the magnitude against the signed limit so INT64_MIN is reachable
     * without ever negating an out-of-range value (which would be UB). */
    u64 limit = neg ? (u64)INT64_MAX + 1u : (u64)INT64_MAX;
    u64 v = 0;
    for (; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            if (ok) *ok = false;
            return 0;
        }
        u64 d = (u64)(p[i] - '0');
        if (v > (limit - d) / 10) {
            if (ok) *ok = false;
            return 0;
        }
        v = v * 10 + d;
    }
    if (ok) *ok = true;
    if (!neg) return (i64)v;
    return v == (u64)INT64_MAX + 1u ? INT64_MIN : -(i64)v;
}

size_t agentc_fmt_u64(char *out, u64 v) {
    char tmp[24];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

size_t agentc_fmt_i64(char *out, i64 v) {
    if (v < 0) {
        out[0] = '-';
        return 1 + agentc_fmt_u64(out + 1, (u64)(-(v + 1)) + 1);
    }
    return agentc_fmt_u64(out, (u64)v);
}

size_t agentc_utf8_encode(u32 cp, u8 *out) {
    if (cp < 0x80) { out[0] = (u8)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (u8)(0xC0 | (cp >> 6));
        out[1] = (u8)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (u8)(0xE0 | (cp >> 12));
        out[1] = (u8)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (u8)(0xF0 | (cp >> 18));
    out[1] = (u8)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (u8)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (u8)(0x80 | (cp & 0x3F));
    return 4;
}

/* The compiler may emit calls to these four even in a freestanding build
 * (struct zero-initialization, loops it decides to lower). They are part of
 * the freestanding runtime and must exist exactly once. */
void *memcpy(void *d, const void *s, size_t n) { return agentc_memcpy(d, s, n); }
void *memmove(void *d, const void *s, size_t n) { return agentc_memmove(d, s, n); }
void *memset(void *d, int c, size_t n) { return agentc_memset(d, c, n); }
int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
