/* term.h — terminal backend interface: real tty or in-memory (tests). */
#ifndef AGENTC_TUI_TERM_H
#define AGENTC_TUI_TERM_H

#include "agentc.h"

typedef struct Terminal Terminal;

Terminal *term_open_tty(void); /* NULL when stdin is not a tty */
Terminal *term_open_memory(int cols, int rows);
void term_close(Terminal *t);
bool term_is_tty(const Terminal *t);
/* True when the terminal cannot be assumed to render Unicode box glyphs:
 * AGENTC_ASCII forces it (any value but "0") and TERM=dumb marks a serial or
 * otherwise limited console. The memory backend is never a tty, so golden
 * tests always see the Unicode shape. */
bool term_ascii_only(const Terminal *t);
int term_cols(Terminal *t);
int term_rows(Terminal *t);
int term_enter(Terminal *t); /* returns 0 or -errno */
/* Raw mode + bracketed paste; the alternate screen only when alt_screen is
 * true. Inline mode leaves the transcript in the terminal's own scrollback.
 * Returns 0 on success and a negative errno when raw mode cannot be entered
 * (the caller must not run half-initialized). */
int term_enter_mode(Terminal *t, bool alt_screen);
void term_leave(Terminal *t);                          /* idempotent shutdown */
int term_read(Terminal *t, u8 *buf, size_t cap);       /* 0 = nothing available */
void term_write(Terminal *t, const void *p, size_t n); /* one write per frame */
/* DSR (\x1b[6n) cursor query, parsed from the ESC[<row>;<col>R reply. One
 * query per call, parsed on the first complete reply (N5: no double query and
 * no sticky death — a miss only means "no reply this resize"). Returns 0 and
 * the 1-based row/col on success; -1 for the memory backend with no scripted
 * reply, a malformed reply, or no reply within the read window. Every byte read
 * that is not part of a complete reply is appended to `typed` in arrival order
 * (the caller clears it first) so the normal input path receives it and no
 * keystroke is lost; extra complete replies are consumed/discarded and a
 * trailing partial reply is held back instead of being handed to the input
 * parser, so a DSR reply byte can never reach the composer. */
int term_cursor(Terminal *t, int *row, int *col, AgcBuf *typed);
/* Test-only memory-backend DSR script (see tui_test.h): `row >= 1` queues a
 * complete reply and arms the memory probe; `row < 1` arms the probe with no
 * reply so a miss can be exercised. Inert for a real tty. */
void term_mem_dsr_reply(Terminal *t, int row, int col);
/* Test-only: queue arbitrary raw bytes for the next memory probe (reply + keys,
 * several replies, a trailing partial reply) and arm the probe. */
void term_mem_dsr_bytes(Terminal *t, const void *bytes, size_t n);
/* True once the test harness has armed the memory-backend DSR probe. */
bool term_mem_dsr_active(const Terminal *t);
/* Push/clear an explicit terminal background (OSC 11 / OSC 111) for a fixed
 * light/dark theme; term_leave resets a pushed background. */
void term_set_bg(Terminal *t, u32 rgb);
void term_reset_bg(Terminal *t);
const AgcBuf *term_output(Terminal *t); /* memory backend transcript */
void term_output_clear(Terminal *t);
/* Returns true and updates out params when the window size changed. */
bool term_check_resize(Terminal *t, int *cols, int *rows);
/* Scripted resize for the memory backend. */
void term_mem_resize(Terminal *t, int cols, int rows);
/* Tests only: queue scripted input bytes for the memory backend. */
void term_mem_feed(Terminal *t, const u8 *p, size_t n);

#endif /* AGENTC_TUI_TERM_H */
