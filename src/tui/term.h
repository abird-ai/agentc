/* term.h — terminal backend interface: real tty or in-memory (tests). */
#ifndef AGENTC_TUI_TERM_H
#define AGENTC_TUI_TERM_H

#include "agentc.h"

typedef struct Terminal Terminal;

Terminal *term_open_tty(void);                       /* NULL when stdin is not a tty */
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
void term_enter(Terminal *t);                       /* raw mode + alt screen + paste */
/* Raw mode + bracketed paste; the alternate screen only when alt_screen is
 * true. Inline mode leaves the transcript in the terminal's own scrollback. */
void term_enter_mode(Terminal *t, bool alt_screen);
void term_leave(Terminal *t);                       /* idempotent shutdown */
int term_read(Terminal *t, u8 *buf, size_t cap);     /* 0 = nothing available */
void term_write(Terminal *t, const void *p, size_t n);   /* one write per frame */
/* Push/clear an explicit terminal background (OSC 11 / OSC 111) for a fixed
 * light/dark theme; term_leave resets a pushed background. */
void term_set_bg(Terminal *t, u32 rgb);
void term_reset_bg(Terminal *t);
const AgcBuf *term_output(Terminal *t);              /* memory backend transcript */
void term_output_clear(Terminal *t);
/* Returns true and updates out params when the window size changed. */
bool term_check_resize(Terminal *t, int *cols, int *rows);
/* Scripted resize for the memory backend. */
void term_mem_resize(Terminal *t, int cols, int rows);
/* Tests only: queue scripted input bytes for the memory backend. */
void term_mem_feed(Terminal *t, const u8 *p, size_t n);

#endif /* AGENTC_TUI_TERM_H */
