/*
 * agentc platform glue for the vendored mbedTLS.
 *
 * Copyright (c) 2026 agentc contributors
 * SPDX-License-Identifier: MIT
 *
 * This is the only place where mbedTLS is allowed to touch the host:
 *   - memory  -> agentc_alloc/agentc_free (allocations are zeroed);
 *   - time    -> os_now_ns(CLOCK_REALTIME);
 *   - entropy -> os_random (getrandom);
 *   - formatting -> a tiny freestanding vsnprintf used by X.509/PEM error
 *     strings, no libc;
 *   - a handful of C string helpers mbedTLS expects (memcmp, strcmp, ...).
 *
 * There is no libc in the link. Everything below is freestanding-safe.
 *
 * The mbedTLS sources themselves stay upstream and unmodified; see
 * THIRD_PARTY.md for the Apache-2.0 notice.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "mbedtls/build_info.h"
#include "mbedtls/entropy.h"
#include "mbedtls/platform.h"
#include "mbedtls/platform_util.h"

/* Declared by library/entropy_poll.h; repeated here so the glue does not need
 * the mbedTLS private include directory. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len,
                          size_t *olen);

/* ------------------------------------------------------------------ layer 0 */
/* Exported by the agentc base/platform layers (src/plat/linux/sys.c,
 * src/base/mem.c).  Declared by hand: no libc headers are used for these. */
extern long os_now_ns(long clock);
extern long os_random(void *buf, unsigned long len);
extern void *agentc_alloc(size_t size);
extern void agentc_free(void *ptr);
extern void os_exit(int code);

/* ------------------------------------------------------------ memory hooks */
/* Internal names are glue_-prefixed: agentc_alloc/agentc_free already exist in the agentc
 * base library and this file must not define a second copy. */
static void *glue_calloc(size_t nmemb, size_t size)
{
    if (size != 0 && nmemb > (size_t) -1 / size) {
        return NULL;
    }
    return agentc_alloc((size_t) (nmemb * size));
}

static void glue_free(void *ptr)
{
    agentc_free(ptr);
}

/* mbedTLS initialises its function pointers to the libc symbols at load time
 * (before our setup hook runs).  These definitions satisfy those relocations
 * without pulling in libc; agentc_platform_setup() then installs the real
 * hooks. */
void *calloc(size_t nmemb, size_t size)
{
    return glue_calloc(nmemb, size);
}

void free(void *ptr)
{
    glue_free(ptr);
}

__attribute__((noreturn)) void exit(int status)
{
    os_exit(status);
    __builtin_unreachable();
}

/* --------------------------------------------------------------- time hook */
static mbedtls_time_t glue_time(mbedtls_time_t *timer)
{
    mbedtls_time_t now = (mbedtls_time_t) (os_now_ns(0) / 1000000000L);
    if (timer != NULL) {
        *timer = now;
    }
    return now;
}

/* platform_time.h initialises mbedtls_time to libc time() unless we provide
 * the symbol.  Keep a matching time() too, then override via set_time(). */
time_t time(time_t *timer)
{
    time_t now = (time_t) (os_now_ns(0) / 1000000000L);
    if (timer != NULL) {
        *timer = now;
    }
    return now;
}

/* mbedtls_platform_gmtime_r(): civil-from-days, no libc.  Only the standard
 * struct tm fields are consumed by X.509. */
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf)
{
    int64_t t = (int64_t) *tt;
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    int64_t z, era, y;
    uint32_t doe, yoe, doy, mp, d, m;

    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }

    z = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (uint32_t) (z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int64_t) yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);

    tm_buf->tm_year = (int) (y - 1900);
    tm_buf->tm_mon = (int) (m - 1);
    tm_buf->tm_mday = (int) d;
    tm_buf->tm_hour = (int) (rem / 3600);
    tm_buf->tm_min = (int) ((rem % 3600) / 60);
    tm_buf->tm_sec = (int) (rem % 60);
    tm_buf->tm_wday = (int) (((days + 4) % 7 + 7) % 7);
    tm_buf->tm_yday = (int) (doy - (153 * mp + 2) / 5 + mp / 10 - 1);
    tm_buf->tm_isdst = 0;
#if defined(__GLIBC__) || defined(__linux__)
    tm_buf->tm_gmtoff = 0;
    tm_buf->tm_zone = "UTC";
#endif
    return tm_buf;
}

/* ------------------------------------------------------------ entropy hook */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len,
                          size_t *olen)
{
    (void) data;
    if (os_random(output, (unsigned long) len) != 0) {
        *olen = 0;
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    *olen = len;
    return 0;
}

/* ------------------------------------------------- platform_util fallbacks */
/* MBEDTLS_PLATFORM_ZEROIZE_ALT: wipe without libc and without the compiler
 * dropping the stores. */
void mbedtls_platform_zeroize(void *buf, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *) buf;
    while (len-- > 0) {
        *p++ = 0;
    }
}

/* MBEDTLS_PLATFORM_MS_TIME_ALT: monotonic milliseconds for mbedTLS internals. */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t) (os_now_ns(1) / 1000000L);
}

/* ------------------------------------------- compiler-rt / libc leftovers */
/* bignum.c uses 128-bit division; there is no compiler-rt in a -nostdlib
 * link, so provide the helper with a plain shift/subtract loop. */
typedef unsigned __int128 agentc_u128;

agentc_u128 __udivti3(agentc_u128 n, agentc_u128 d)
{
    agentc_u128 q = 0, r = 0;
    int i;
    if (d == 0) {
        return 0;
    }
    for (i = 127; i >= 0; i--) {
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

/* x509_crt.c parses SAN IP-addresses with inet_pton(); AF_INET only. */
int inet_pton(int af, const char *src, void *dst)
{
    unsigned char *out = (unsigned char *) dst;
    int i;

    if (af != 2) {              /* AF_INET */
        return 0;
    }
    for (i = 0; i < 4; i++) {
        int v = 0, digits = 0;
        while (*src >= '0' && *src <= '9') {
            v = v * 10 + (*src - '0');
            if (++digits > 3 || v > 255) {
                return 0;
            }
            src++;
        }
        if (digits == 0) {
            return 0;
        }
        out[i] = (unsigned char) v;
        if (i < 3) {
            if (*src != '.') {
                return 0;
            }
            src++;
        }
    }
    return *src == '\0' ? 1 : 0;
}

/* ------------------------------------------------------- formatted output */
/* Minimal freestanding snprintf: %d %i %u %x %X %o %c %s %p %% with
 * '-', '0', '+', ' ', '#', width, precision; length modifiers are parsed and
 * ignored (all integer arguments are fetched as 64-bit, which is what the
 * x86-64 SysV ABI guarantees for integer varargs). */
typedef struct {
    char *buf;
    size_t size;
    size_t len;                 /* number of characters that would be written */
} glue_fmt;

static void fmt_put(glue_fmt *o, char c)
{
    if (o->len + 1 < o->size) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void fmt_pad(glue_fmt *o, int width, char fill, int left)
{
    while (!left && width-- > 0) {
        fmt_put(o, fill);
    }
    while (left && width-- > 0) {
        fmt_put(o, ' ');
    }
}

static int glue_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    glue_fmt o;
    o.buf = buf;
    o.size = n;
    o.len = 0;

    for (; *fmt != '\0'; fmt++) {
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        int width = 0, prec = -1, have_prec = 0;
        int base = 10, upper = 0, neg = 0;
        char tmp[72];
        int tlen = 0;
        char sign = 0;

        if (*fmt != '%') {
            fmt_put(&o, *fmt);
            continue;
        }
        fmt++;

        /* flags */
        for (;;) {
            if (*fmt == '-') {
                left = 1;
            } else if (*fmt == '0') {
                zero = 1;
            } else if (*fmt == '+') {
                plus = 1;
            } else if (*fmt == ' ') {
                space = 1;
            } else if (*fmt == '#') {
                alt = 1;
            } else {
                break;
            }
            fmt++;
        }
        /* width */
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }
        /* precision */
        if (*fmt == '.') {
            fmt++;
            have_prec = 1;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }
        /* length modifiers (ignored) */
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'z' || *fmt == 'j' ||
               *fmt == 't' || *fmt == 'L') {
            fmt++;
        }

        if (*fmt == '\0') {
            break;
        }

        switch (*fmt) {
        case '%':
            fmt_put(&o, '%');
            continue;
        case 'c': {
            char c = (char) va_arg(ap, int);
            if (!left) {
                fmt_pad(&o, width - 1, ' ', 0);
            }
            fmt_put(&o, c);
            if (left) {
                fmt_pad(&o, width - 1, ' ', 1);
            }
            continue;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            int slen;
            if (s == NULL) {
                s = "(null)";
            }
            slen = 0;
            while (s[slen] != '\0' && !(have_prec && slen >= prec)) {
                slen++;
            }
            if (!left) {
                fmt_pad(&o, width - slen, ' ', 0);
            }
            for (int i = 0; i < slen; i++) {
                fmt_put(&o, s[i]);
            }
            if (left) {
                fmt_pad(&o, width - slen, ' ', 1);
            }
            continue;
        }
        case 'd':
        case 'i': {
            long long v = va_arg(ap, long long);
            if (v < 0) {
                neg = 1;
                sign = '-';
                v = -(v + 1);
                v += 1;         /* avoids UB on LLONG_MIN */
            } else if (plus) {
                sign = '+';
            } else if (space) {
                sign = ' ';
            }
            do {
                int digit = (int) (v % 10);
                tmp[tlen++] = (char) ('0' + digit);
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
            if (have_prec && prec == 0 && v == 0) {
                break;          /* nothing, no padding zeros below */
            }
            do {
                int digit = (int) (v % (unsigned) base);
                if (digit < 10) {
                    tmp[tlen++] = (char) ('0' + digit);
                } else {
                    tmp[tlen++] = (char) ((upper ? 'A' : 'a') + digit - 10);
                }
                v /= (unsigned) base;
            } while (v != 0);
            if (alt) {
                if (base == 8 && (tlen == 0 || tmp[tlen - 1] != '0')) {
                    tmp[tlen++] = '0';
                } else if (base == 16) {
                    tmp[tlen++] = upper ? 'X' : 'x';
                    tmp[tlen++] = '0';
                }
            }
            break;
        }
        default:
            fmt_put(&o, '%');
            fmt_put(&o, *fmt);
            continue;
        }

        /* integer: tmp holds digits little-endian (prefix appended for #) */
        {
            int digits = tlen;
            int zeros = 0;
            int total;
            if (have_prec && prec > digits) {
                zeros = prec - digits;
            }
            total = digits + zeros + (sign ? 1 : 0);
            if (zero && !left && !have_prec) {
                zeros = width - total;
                if (zeros < 0) {
                    zeros = 0;
                }
            }
            total = digits + zeros + (sign ? 1 : 0);
            if (!left) {
                int pad = width - total;
                while (pad-- > 0) {
                    fmt_put(&o, ' ');
                }
            }
            if (sign) {
                fmt_put(&o, sign);
            }
            while (zeros-- > 0) {
                fmt_put(&o, '0');
            }
            while (tlen > 0) {
                fmt_put(&o, tmp[--tlen]);
            }
            (void) neg;
            if (left) {
                int pad = width - total;
                while (pad-- > 0) {
                    fmt_put(&o, ' ');
                }
            }
        }
        continue;
    }

    if (n > 0) {
        size_t end = o.len < n - 1 ? o.len : n - 1;
        buf[end] = '\0';
    }
    return (int) o.len;
}

static int glue_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = glue_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return ret;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    return glue_vsnprintf(buf, n, fmt, ap);
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = glue_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return ret;
}

/* --------------------------------------------------------------- setup hook */
int agentc_platform_setup(void)
{
    mbedtls_platform_set_calloc_free(glue_calloc, glue_free);
    mbedtls_platform_set_time(glue_time);
    mbedtls_platform_set_snprintf(glue_snprintf);
    mbedtls_platform_set_vsnprintf(glue_vsnprintf);
    return 0;
}

/* ------------------------------------------------- small C string helpers */
/* memcmp/memcpy/memmove/memset are already provided by src/base/str.c, so
 * only the helpers mbedTLS needs beyond those live here. */
size_t strlen(const char *s)
{
    const char *p = s;
    while (*p != '\0') {
        p++;
    }
    return (size_t) (p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int) (unsigned char) *a - (int) (unsigned char) *b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n > 0 && *a != '\0' && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int) (unsigned char) *a - (int) (unsigned char) *b;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char) c) {
            return (char *) s;
        }
        if (*s == '\0') {
            return NULL;
        }
    }
}

char *strstr(const char *hay, const char *needle)
{
    size_t nlen = 0;
    if (needle[0] == '\0') {
        return (char *) hay;
    }
    while (needle[nlen] != '\0') {
        nlen++;
    }
    while (*hay != '\0') {
        size_t i = 0;
        while (i < nlen && hay[i] == needle[i]) {
            i++;
        }
        if (i == nlen) {
            return (char *) hay;
        }
        hay++;
    }
    return NULL;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    while (i < n && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    while (i < n) {
        dst[i++] = '\0';
    }
    return dst;
}
