/*
 * mbedtls_shim.c — libc- and compiler-rt-named shims for the freestanding
 * (Linux, -nostdlib) TLS build.
 *
 * Copyright (c) 2026 agentc contributors
 * SPDX-License-Identifier: MIT
 *
 * These definitions exist only because a -nostdlib link has no libc and no
 * compiler-rt. They are deliberately split out of third_party/mbedtls_glue.c
 * (B-D5) so build/libagentc.a can exclude this object and let an embedding
 * host's libc/compiler-rt own those names: the archive must never *define*
 * memcpy/calloc/exit/snprintf/… (the B2 deny-list gate asserts exactly that).
 * The mbedTLS platform hooks and the mbedTLS-named overrides live in the glue.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "agentc.h"
#include "plat.h"

/* --------------------------------------------------------------- memory */
/* calloc/free are the fallbacks mbedTLS captures at load time; the glue's
 * agentc_platform_setup() installs agentc-backed versions afterwards. */
void *calloc(size_t nmemb, size_t size)
{
    if (size != 0 && nmemb > (size_t) -1 / size) return NULL;
    return agentc_alloc((size_t) (nmemb * size));
}

void free(void *ptr) { agentc_free(ptr); }

__attribute__((noreturn)) void exit(int status)
{
    os_exit(status);
    __builtin_unreachable();
}

/* ----------------------------------------------------------------- time */
time_t time(time_t *timer)
{
    time_t now = (time_t) (os_now_ns(0) / 1000000000L);
    if (timer != NULL) *timer = now;
    return now;
}

/* x509_crt.c parses SAN IP-addresses with inet_pton(); AF_INET only. */
int inet_pton(int af, const char *src, void *dst)
{
    unsigned char *out = (unsigned char *) dst;
    if (af != 2) return 0;              /* AF_INET */
    for (int i = 0; i < 4; i++) {
        int v = 0, digits = 0;
        while (*src >= '0' && *src <= '9') {
            v = v * 10 + (*src - '0');
            if (++digits > 3 || v > 255) return 0;
            src++;
        }
        if (digits == 0) return 0;
        out[i] = (unsigned char) v;
        if (i < 3) {
            if (*src != '.') return 0;
            src++;
        }
    }
    return *src == '\0' ? 1 : 0;
}

/* ----------------------------------------- compiler-rt 128-bit division */
/* bignum.c uses 128-bit division; there is no compiler-rt in a -nostdlib
 * link, so provide the helpers with a plain shift/subtract loop. */
typedef unsigned __int128 agentc_u128;

agentc_u128 __udivti3(agentc_u128 n, agentc_u128 d)
{
    agentc_u128 q = 0, r = 0;
    if (d == 0) return 0;
    for (int i = 127; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (agentc_u128) 1 << i;
        }
    }
    return q;
}

agentc_u128 __umodti3(agentc_u128 n, agentc_u128 d)
{
    return n - __udivti3(n, d) * d;
}

/* ---------------------------------------------------- small C string helpers */
size_t strlen(const char *s)
{
    const char *p = s;
    while (*p != '\0') p++;
    return (size_t) (p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) { a++; b++; }
    return (int) (unsigned char) *a - (int) (unsigned char) *b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n > 0 && *a != '\0' && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int) (unsigned char) *a - (int) (unsigned char) *b;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char) c) return (char *) s;
        if (*s == '\0') return NULL;
    }
}

char *strstr(const char *hay, const char *needle)
{
    size_t nlen = 0;
    if (needle[0] == '\0') return (char *) hay;
    while (needle[nlen] != '\0') nlen++;
    while (*hay != '\0') {
        size_t i = 0;
        while (i < nlen && hay[i] == needle[i]) i++;
        if (i == nlen) return (char *) hay;
        hay++;
    }
    return NULL;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    while (i < n && src[i] != '\0') { dst[i] = src[i]; i++; }
    while (i < n) dst[i++] = '\0';
    return dst;
}

/* ------------------------------------------------------------ formatted output */
/* Minimal freestanding snprintf: %d %i %u %x %X %o %c %s %p %% with
 * '-', '0', '+', ' ', '#', width, precision; length modifiers are parsed and
 * ignored (all integer arguments are fetched as 64-bit, which the x86-64 SysV
 * ABI guarantees for integer varargs). */
typedef struct {
    char *buf;
    size_t size;
    size_t len;                 /* characters that would be written */
} shim_fmt;

static void fmt_put(shim_fmt *o, char c)
{
    if (o->len + 1 < o->size) o->buf[o->len] = c;
    o->len++;
}

static void fmt_pad(shim_fmt *o, int width, char fill, int left)
{
    while (!left && width-- > 0) fmt_put(o, fill);
    while (left && width-- > 0) fmt_put(o, ' ');
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    shim_fmt o = { buf, n, 0 };

    for (; *fmt != '\0'; fmt++) {
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        int width = 0, prec = -1, have_prec = 0;
        int base = 10, upper = 0;
        char tmp[72];
        int tlen = 0;
        char sign = 0;

        if (*fmt != '%') { fmt_put(&o, *fmt); continue; }
        fmt++;

        for (;;) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else break;
            fmt++;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        }
        if (*fmt == '.') {
            fmt++;
            have_prec = 1;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'z' || *fmt == 'j' ||
               *fmt == 't' || *fmt == 'L') fmt++;

        if (*fmt == '\0') break;

        switch (*fmt) {
        case '%':
            fmt_put(&o, '%');
            continue;
        case 'c': {
            char c = (char) va_arg(ap, int);
            if (!left) fmt_pad(&o, width - 1, ' ', 0);
            fmt_put(&o, c);
            if (left) fmt_pad(&o, width - 1, ' ', 1);
            continue;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            int slen;
            if (s == NULL) s = "(null)";
            slen = 0;
            while (s[slen] != '\0' && !(have_prec && slen >= prec)) slen++;
            if (!left) fmt_pad(&o, width - slen, ' ', 0);
            for (int i = 0; i < slen; i++) fmt_put(&o, s[i]);
            if (left) fmt_pad(&o, width - slen, ' ', 1);
            continue;
        }
        case 'd':
        case 'i': {
            long long v = va_arg(ap, long long);
            if (v < 0) {
                sign = '-';
                v = -(v + 1);
                v += 1;         /* avoids UB on LLONG_MIN */
            } else if (plus) sign = '+';
            else if (space) sign = ' ';
            do {
                tmp[tlen++] = (char) ('0' + (int) (v % 10));
                v /= 10;
            } while (v != 0);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            unsigned long long v;
            if (*fmt == 'p') {
                v = (unsigned long long) (uintptr_t) va_arg(ap, void *);
                alt = 1;
                base = 16;
                upper = 0;
            } else {
                v = va_arg(ap, unsigned long long);
                base = (*fmt == 'o') ? 8 : ((*fmt == 'u') ? 10 : 16);
                upper = (*fmt == 'X');
            }
            if (have_prec && prec == 0 && v == 0) break;
            do {
                int digit = (int) (v % (unsigned) base);
                if (digit < 10) tmp[tlen++] = (char) ('0' + digit);
                else tmp[tlen++] = (char) ((upper ? 'A' : 'a') + digit - 10);
                v /= (unsigned) base;
            } while (v != 0);
            if (alt) {
                if (base == 8 && (tlen == 0 || tmp[tlen - 1] != '0')) tmp[tlen++] = '0';
                else if (base == 16) { tmp[tlen++] = upper ? 'X' : 'x'; tmp[tlen++] = '0'; }
            }
            break;
        }
        default:
            fmt_put(&o, '%');
            fmt_put(&o, *fmt);
            continue;
        }

        {
            int digits = tlen;
            int zeros = 0;
            int total;
            if (have_prec && prec > digits) zeros = prec - digits;
            total = digits + zeros + (sign ? 1 : 0);
            if (zero && !left && !have_prec) {
                zeros = width - total;
                if (zeros < 0) zeros = 0;
            }
            total = digits + zeros + (sign ? 1 : 0);
            if (!left) {
                int pad = width - total;
                while (pad-- > 0) fmt_put(&o, ' ');
            }
            if (sign) fmt_put(&o, sign);
            while (zeros-- > 0) fmt_put(&o, '0');
            while (tlen > 0) fmt_put(&o, tmp[--tlen]);
            if (left) {
                int pad = width - total;
                while (pad-- > 0) fmt_put(&o, ' ');
            }
        }
    }

    if (n > 0) {
        size_t end = o.len < n - 1 ? o.len : n - 1;
        buf[end] = '\0';
    }
    return (int) o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return ret;
}
