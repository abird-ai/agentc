/* deadline.h — absolute monotonic deadlines.
 *
 * A deadline is computed once and never re-armed, so a peer that drips one
 * byte per second still hits it: every loop that waits on the network, a child
 * process or a remote RPC takes an AgcDeadline and checks
 * agentc_deadline_expired() instead of counting down its own timeout.
 */
#ifndef AGENTC_BASE_DEADLINE_H
#define AGENTC_BASE_DEADLINE_H

#include "plat.h"

typedef struct {
    i64 at_ns;   /* absolute monotonic ns */
    bool set;    /* false = no deadline; avoids overloading 0 as a sentinel */
} AgcDeadline;

/* A deadline that never expires. */
static inline AgcDeadline agentc_deadline_none(void) {
    AgcDeadline d = { 0, false };
    return d;
}

/* now + ms (0 ms means "expired immediately", not "none"). */
static inline AgcDeadline agentc_deadline_after_ms(i64 ms) {
    AgcDeadline d;

    if (ms < 0) ms = 0;
    /* clamp so the multiply cannot overflow i64 (about 292 years) */
    if (ms > 9223372036854LL) ms = 9223372036854LL;
    d.at_ns = os_now_ns(OS_CLOCK_MONOTONIC) + ms * 1000000;
    d.set = true;
    return d;
}

static inline bool agentc_deadline_set(AgcDeadline d) {
    return d.set;
}

static inline bool agentc_deadline_expired(AgcDeadline d) {
    return d.set && os_now_ns(OS_CLOCK_MONOTONIC) >= d.at_ns;
}

/* Milliseconds left, clamped to [0, cap]; cap when no deadline is set (a poll
 * that must not block forever still gets a bound).  Rounds up, so a sub-
 * millisecond remainder never turns into a 0 ms poll. */
static inline int agentc_deadline_remaining_ms(AgcDeadline d, int cap) {
    i64 now, left_ns, ms;

    if (!d.set) return cap;
    now = os_now_ns(OS_CLOCK_MONOTONIC);
    if (now >= d.at_ns) return 0;
    left_ns = d.at_ns - now;
    ms = (left_ns + 999999) / 1000000;
    return ms > cap ? cap : (int)ms;
}

#endif /* AGENTC_BASE_DEADLINE_H */
