/* assert.h — release semantics for a freestanding build: nothing to report to.
 * mbedTLS includes it from library/common.h; there is no libc assert here. */
#ifndef AGENTC_FREESTANDING_ASSERT_H
#define AGENTC_FREESTANDING_ASSERT_H

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
/* A failing assertion in a program with no stdio is still fatal, and the
 * platform layer is the only thing that can end the process. */
void os_exit(int code);
#define assert(e) ((e) ? (void)0 : (os_exit(134), (void)0))
#endif

#endif
