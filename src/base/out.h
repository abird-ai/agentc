/* out.h — the terminal-safe output boundary.
 *
 * Every byte a terminal (or a pipe a terminal will read) can see goes through
 * this writer: model text, tool output, file contents and remote strings are
 * sanitized here, so "no OSC 52 / cursor / title injection" is a property of
 * the writer rather than of every caller.  \n, \t and \r stay; ESC, every
 * other C0 byte, DEL and the C1 range become U+FFFD.
 *
 * agentc_out_raw() is the only writer that does not sanitize.  It exists for
 * trusted, caller-controlled interactive echo (setup.c) and must never be fed
 * model, tool or file content.
 */
#ifndef AGENTC_BASE_OUT_H
#define AGENTC_BASE_OUT_H

#include "agentc.h"

/* May cp appear in terminal-bound output? */
bool agentc_out_cp_safe(u32 cp);
/* cp, or U+FFFD when it is not safe. */
u32 agentc_out_cp_filter(u32 cp);

/* Write all of p to fd with unsafe codepoints replaced by U+FFFD.  Short
 * writes are handled internally, so callers never have to map an output
 * offset back to an input offset.  Returns bytes written or -errno. */
i64 agentc_out_safe_write(int fd, const void *p, size_t n);

/* Sanitized append to b (for callers that assemble a line before writing). */
void agentc_out_safe_buf(AgcBuf *b, const void *p, size_t n);

/* Raw, unsanitized stdout write.  Trusted interactive echo only. */
void agentc_out_raw(const void *p, size_t n);

#endif /* AGENTC_BASE_OUT_H */
