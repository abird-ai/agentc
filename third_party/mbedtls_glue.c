/*
 * agentc platform glue for the vendored mbedTLS.
 *
 * Copyright (c) 2026 agentc contributors
 * SPDX-License-Identifier: MIT
 *
 * This is the only place where mbedTLS is allowed to touch the host:
 *   - memory     -> agentc_alloc/agentc_free (allocations are zeroed);
 *   - time       -> os_now_ns(CLOCK_REALTIME);
 *   - entropy    -> os_random (getrandom);
 *   - formatting -> the freestanding vsnprintf (in the native build) or the
 *     embedding host's libc vsnprintf (in libagentc.a), installed at setup;
 *   - string helpers -> third_party/mbedtls_shim.c (freestanding) or the host
 *     libc.
 *
 * The libc- and compiler-rt-named shims (calloc, free, exit, time, snprintf,
 * str* helpers, __udivti3, inet_pton) live in third_party/mbedtls_shim.c, which
 * is excluded from build/libagentc.a: the archive must never define a libc name
 * (B-D5).
 *
 * The mbedTLS sources themselves stay upstream and unmodified; see
 * THIRD_PARTY.md for the Apache-2.0 notice.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

/* --------------------------------------------------------------- time hook */
static mbedtls_time_t glue_time(mbedtls_time_t *timer)
{
    mbedtls_time_t now = (mbedtls_time_t) (os_now_ns(0) / 1000000000L);
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

/* --------------------------------------------------------------- setup hook */
int agentc_platform_setup(void)
{
    mbedtls_platform_set_calloc_free(glue_calloc, glue_free);
    mbedtls_platform_set_time(glue_time);
    /* The freestanding build defines these in third_party/mbedtls_shim.c; an
     * embedding host provides libc's. Installing them keeps X.509/PEM error
     * formatting on our bounded, control-safe formatter in both worlds. */
    mbedtls_platform_set_snprintf(snprintf);
    mbedtls_platform_set_vsnprintf(vsnprintf);
    return 0;
}
