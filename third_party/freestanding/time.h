/* time.h — the three things the vendored TLS needs from <time.h>, declared
 * freestanding. The build puts this directory on the include path ahead of any
 * system headers, so a host <time.h> can never leak in (see ../../THIRD_PARTY.md).
 * The layout is the standard one; agentc provides time()/gmtime_r() itself. */
#ifndef AGENTC_FREESTANDING_TIME_H
#define AGENTC_FREESTANDING_TIME_H
#include <stddef.h>

typedef long long time_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

time_t time(time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);

#endif
