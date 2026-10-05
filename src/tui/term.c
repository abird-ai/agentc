/* term.c — terminal lifecycle and the two backends.
 *
 * Real tty: os_tty_raw/os_tty_restore + os_tty_size; every frame is a single
 * os_write(1). Memory backend: frame bytes append to an AgcBuf and input comes
 * from a scripted queue; used by the golden tests (no tty required).
 */
#include "term.h"
#include "plat.h"

#define TERM_ALT_ON "\x1b[?1049h"
#define TERM_ALT_OFF "\x1b[?1049l"
#define TERM_PASTE_ON "\x1b[?2004h"
#define TERM_PASTE_OFF "\x1b[?2004l"
#define TERM_CURSOR_OFF "\x1b[?25l"
#define TERM_CURSOR_ON "\x1b[?25h"
/* Closing the synchronized-update bracket is idempotent and makes the restore
 * path safe even if a signal landed in the middle of a frame write. */
#define TERM_SYNC_OFF "\x1b[?2026l"

struct Terminal {
    bool tty;
    bool entered;
    bool alt;           /* the alternate screen is active */
    bool bg_set;        /* an OSC 11 background is currently pushed */
    int cols, rows;
    void *saved;
    AgcBuf out;          /* memory backend frame transcript */
    AgcBuf in;           /* memory backend scripted input */
    size_t in_pos;
    int mem_cols, mem_rows;
};

static void write_all(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n) {
        int w = os_write(fd, q, n);
        if (w <= 0) return;
        q += w;
        n -= (size_t)w;
    }
}

Terminal *term_open_memory(int cols, int rows) {
    Terminal *t = agentc_alloc(sizeof *t);
    t->tty = false;
    t->cols = t->mem_cols = cols;
    t->rows = t->mem_rows = rows;
    return t;
}

Terminal *term_open_tty(void) {
    int cols = 0, rows = 0;
    if (os_tty_size(0, &cols, &rows) != 0) return NULL;
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 24;
    Terminal *t = agentc_alloc(sizeof *t);
    t->tty = true;
    t->cols = cols;
    t->rows = rows;
    return t;
}

bool term_is_tty(const Terminal *t) { return t && t->tty; }

bool term_ascii_only(const Terminal *t) {
    if (!t || !t->tty) return false;
    const char *force = os_getenv("AGENTC_ASCII");
    if (force && force[0] && !agentc_streq(force, "0")) return true;
    const char *term = os_getenv("TERM");
    return term && agentc_streq(term, "dumb");
}

int term_cols(Terminal *t) { return t ? t->cols : 80; }
int term_rows(Terminal *t) { return t ? t->rows : 24; }

void term_enter_mode(Terminal *t, bool alt_screen) {
    if (!t || t->entered) return;
    const char *on = alt_screen ? TERM_ALT_ON TERM_PASTE_ON TERM_CURSOR_OFF
                                : TERM_PASTE_ON TERM_CURSOR_OFF;
    if (t->tty) {
        if (os_tty_raw(0, &t->saved) != 0) return;
        t->entered = true;
        t->alt = alt_screen;
        write_all(1, on, agentc_strlen(on));
    } else {
        t->entered = true;
        t->alt = alt_screen;
        agentc_buf_cstr(&t->out, on);
    }
}

void term_enter(Terminal *t) { term_enter_mode(t, true); }

void term_leave(Terminal *t) {
    if (!t || !t->entered) return;
    const char *off = t->alt ? TERM_SYNC_OFF TERM_CURSOR_ON TERM_PASTE_OFF TERM_ALT_OFF
                             : TERM_SYNC_OFF TERM_CURSOR_ON TERM_PASTE_OFF;
    if (t->tty) {
        /* Reset the pushed background before the rest of the restore. This is
         * a constant write, so the signal path stays allocation-free. */
        if (t->bg_set) write_all(1, "\x1b]111\x1b\\", 7);
        write_all(1, off, agentc_strlen(off));
        os_tty_restore(0, t->saved);
    } else {
        if (t->bg_set) agentc_buf_cstr(&t->out, "\x1b]111\x1b\\");
        agentc_buf_cstr(&t->out, off);
    }
    t->bg_set = false;
    t->entered = false;
    t->alt = false;
}

void term_close(Terminal *t) {
    if (!t) return;
    term_leave(t);
    agentc_buf_free(&t->out);
    agentc_buf_free(&t->in);
    agentc_free(t);
}

void term_write(Terminal *t, const void *p, size_t n) {
    if (!t || !n) return;
    if (t->tty) write_all(1, p, n);
    else agentc_buf_push(&t->out, p, n);
}

/* Push an explicit background (OSC 11) so a fixed light/dark palette does not
 * draw dark text on a dark terminal. `term_leave` resets it (OSC 111). */
void term_set_bg(Terminal *t, u32 rgb) {
    if (!t) return;
    char seq[48];
    int n = agentc_snprintf(seq, sizeof seq, "\x1b]11;rgb:%02x/%02x/%02x\x1b\\",
                            (unsigned)((rgb >> 16) & 0xFF),
                            (unsigned)((rgb >> 8) & 0xFF),
                            (unsigned)(rgb & 0xFF));
    if (n > 0) term_write(t, seq, (size_t)n);
    t->bg_set = true;
}

void term_reset_bg(Terminal *t) {
    if (!t || !t->bg_set) return;
    static const char seq[] = "\x1b]111\x1b\\";
    term_write(t, seq, sizeof seq - 1);
    t->bg_set = false;
}

int term_read(Terminal *t, u8 *buf, size_t cap) {
    if (!t) return 0;
    if (t->tty) return os_read(0, buf, cap);
    size_t avail = t->in.len > t->in_pos ? t->in.len - t->in_pos : 0;
    if (!avail) return 0;
    size_t n = avail < cap ? avail : cap;
    agentc_memcpy(buf, t->in.p + t->in_pos, n);
    t->in_pos += n;
    if (t->in_pos == t->in.len) {
        agentc_buf_clear(&t->in);
        t->in_pos = 0;
    }
    return (int)n;
}

const AgcBuf *term_output(Terminal *t) { return t ? &t->out : NULL; }

void term_output_clear(Terminal *t) {
    if (t) agentc_buf_clear(&t->out);
}

bool term_check_resize(Terminal *t, int *cols, int *rows) {
    if (!t) return false;
    if (t->tty) {
        int c = 0, r = 0;
        if (os_tty_size(0, &c, &r) != 0 || c < 1 || r < 1) return false;
        if (c == t->cols && r == t->rows) return false;
        t->cols = c;
        t->rows = r;
    } else {
        if (t->mem_cols == t->cols && t->mem_rows == t->rows) return false;
        t->cols = t->mem_cols;
        t->rows = t->mem_rows;
    }
    if (cols) *cols = t->cols;
    if (rows) *rows = t->rows;
    return true;
}

/* Tests only: queue scripted input bytes and stage a resize. */
void term_mem_resize(Terminal *t, int cols, int rows) {
    if (!t || t->tty) return;
    t->mem_cols = cols;
    t->mem_rows = rows;
}

/* Internal helper used by the test harness to script input directly. */
void term_mem_feed(Terminal *t, const u8 *p, size_t n) {
    if (t && !t->tty) agentc_buf_push(&t->in, p, n);
}
