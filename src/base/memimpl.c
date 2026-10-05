/* memimpl.c — the libc-named memory routines the compiler may emit.
 *
 * The compiler can lower struct zero-initialization and large copies to
 * `memcpy`/`memmove`/`memset`/`memcmp` even in a freestanding build. They are
 * split out of str.c so build/libagentc.a can exclude this one object and let
 * an embedding host's libc own those names (B-D5/B-D1). Everything inside the
 * engine calls the agentc_* spellings; mbedTLS reaches the freestanding build's
 * copies through its own library. Definitions must exist exactly once in a
 * -nostdlib link.
 */
#include "agentc.h"

void *memcpy(void *d, const void *s, size_t n) {
    return agentc_memcpy(d, s, n);
}
void *memmove(void *d, const void *s, size_t n) {
    return agentc_memmove(d, s, n);
}
void *memset(void *d, int c, size_t n) {
    return agentc_memset(d, c, n);
}
int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}
