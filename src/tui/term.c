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
/* Wheel/click reporting (SGR extended coords) so the composer can scroll
 * with the mouse wheel. Clicks are parsed-but-ignored by input.c. */
#define TERM_MOUSE_ON "\x1b[?1000h\x1b[?1006h"
#define TERM_MOUSE_OFF "\x1b[?1006l\x1b[?1000l"
#define TERM_CURSOR_OFF "\x1b[?25l"
#define TERM_CURSOR_ON "\x1b[?25h"
/* Closing the synchronized-update bracket is idempotent and makes the restore
 * path safe even if a signal landed in the middle of a frame write. */
#define TERM_SYNC_OFF "\x1b[?2026l"
/* Longest reply-prefix suffix held for the next probe (see the DSR probe). */
#define PROBE_TAIL_MAX 16

struct Terminal {
    bool tty;
    bool entered;
    bool alt;    /* the alternate screen is active */
    bool bg_set; /* an OSC 11 background is currently pushed */
    int cols, rows;
    void *saved;
    AgcBuf out; /* memory backend frame transcript */
    AgcBuf in;  /* memory backend scripted input */
    size_t in_pos;
    bool test_dsr; /* memory backend: the test harness armed the DSR probe */
    int mem_cols, mem_rows;
    /* A trailing partial DSR reply is per-terminal, never process-global: the
     * memory backend has one Terminal per test harness. */
    u8 probe_tail[PROBE_TAIL_MAX];
    size_t probe_tail_len;
};

static void write_all(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n) {
        int w = os_write(fd, q, n);
        if (w <= 0)
            return;
        q += w;
        n -= (size_t)w;
    }
}

/* Parse the first DEC DSR reply ESC [ <row> ; <col> R in `buf`. On success
 * stores row/col and the reply's byte span [start,end) so the caller can hand
 * back the surrounding type-ahead bytes in order. */
static bool parse_cursor_reply(const u8 *buf, size_t n, int *row, int *col, size_t *start,
                               size_t *end) {
    for (size_t i = 0; i + 2 < n; i++) {
        if (buf[i] != 0x1b || buf[i + 1] != '[')
            continue;
        size_t j = i + 2;
        int r = 0;
        bool dr = false;
        while (j < n && buf[j] >= '0' && buf[j] <= '9') {
            r = r * 10 + (buf[j] - '0');
            dr = true;
            j++;
        }
        if (!dr || j >= n || buf[j] != ';')
            continue;
        j++;
        int c = 0;
        bool dc = false;
        while (j < n && buf[j] >= '0' && buf[j] <= '9') {
            c = c * 10 + (buf[j] - '0');
            dc = true;
            j++;
        }
        if (!dc || j >= n || buf[j] != 'R')
            continue;
        if (row)
            *row = r;
        if (col)
            *col = c;
        if (start)
            *start = i;
        if (end)
            *end = j + 1;
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- DSR probe
 *
 * A DSR reply is a *displacement sensor*, not a law oracle: the app derives
 * the inline region's true top from its own geometry/parked-row state and uses
 * the measured distance only to navigate before the erase (D2 §3). The probe
 * therefore must be exact about what it consumes and lossless about everything
 * else: type-ahead is handed back in arrival order, a partial reply is held for
 * the next probe instead of being fed to the input parser (which would swallow
 * the user's next keystroke as the sequence's final byte), and a terminal that
 * does not answer is simply not asked again *this resize* — never disabled for
 * the process (N5: the old double probe + sticky death turned one transient
 * miss into 98/98 class-G resizes). Width changes are probed too (the reply is
 * measured but the erase class G ignores it): the class is chosen by
 * inline_resize_erase, not by the gate. */
/* Bounded reply window for one probe. A real terminal answers DSR within a few
 * milliseconds, but a resize storm makes it reflow and answer later; the old
 * 60 ms window timed out on genuine resizes (70/71 class=F/G, the composer
 * ghost). 300 ms matches the known-good 6a build, which measured 51/56 resize
 * replies on the same terminal, and still bounds a silent terminal's stall to
 * one window per resize (the N17 accepted residual). */
#define DSR_DEADLINE_MS 300
#define PROBE_CAP (64u * 1024u)

/* Single-threaded probe scratch (bounded against a paste during the probe). */
static u8 probe_buf[PROBE_CAP];

/* True when p[0..n) is a prefix of the reply grammar (ESC, ESC [, ESC [ digits,
 * ESC [ digits ;, ESC [ digits ; digits), with the digit runs bounded. */
static bool is_reply_prefix(const u8 *p, size_t n) {
    if (n < 1 || p[0] != 0x1b)
        return false;
    if (n == 1)
        return true; /* ESC */
    if (p[1] != '[')
        return false;
    if (n == 2)
        return true; /* ESC [ */
    size_t i = 2, d = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
        i++;
        d++;
    }
    if (d == 0 || d > 9)
        return false;
    if (i == n)
        return true; /* ESC [ digits */
    if (p[i] != ';')
        return false;
    i++;
    if (i == n)
        return true; /* ESC [ digits ; */
    d = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
        i++;
        d++;
    }
    if (d == 0 || d > 9)
        return false;
    return i == n; /* ESC [ digits ; digits */
}

/* Length of the longest suffix of buf[0..n) that is a reply prefix. */
static size_t reply_tail_len(const u8 *buf, size_t n) {
    size_t max = n < PROBE_TAIL_MAX ? n : PROBE_TAIL_MAX;
    for (size_t len = max; len > 0; len--)
        if (is_reply_prefix(buf + n - len, len))
            return len;
    return 0;
}

/* Strip EVERY complete ESC[<row>;<col>R reply from buf[0..n) and append the
 * bytes that are not part of one to `typed` in arrival order. Stores the first
 * reply's row/col (the answer). A trailing incomplete reply prefix is copied to
 * hold[0..*hold_len) instead of `typed` so the next read can complete it.
 * Returns true when at least one complete reply was seen. This is the
 * reply/type-ahead separation: a second coalesced reply (the old double probe
 * leaked it into the composer) is consumed here, never handed back. */
static bool take_replies(const u8 *buf, size_t n, int *row, int *col, AgcBuf *typed, u8 *hold,
                         size_t *hold_len) {
    size_t i = 0;
    bool found = false;
    if (hold)
        *hold_len = 0;
    while (i < n) {
        size_t s = 0, e = 0;
        int r = 0, c = 0;
        if (parse_cursor_reply(buf + i, n - i, &r, &c, &s, &e)) {
            if (typed && s)
                agentc_buf_push(typed, buf + i, s);
            if (!found) {
                if (row)
                    *row = r;
                if (col)
                    *col = c;
                found = true;
            }
            i += e;
            continue;
        }
        size_t rem = n - i;
        if (is_reply_prefix(buf + i, rem)) {
            if (hold) {
                agentc_memcpy(hold, buf + i, rem);
                *hold_len = rem;
            }
            return found;
        }
        if (typed)
            agentc_buf_byte(typed, buf[i]);
        i++;
    }
    return found;
}

/* One query/response cycle (one query, one read window). Returns 0 and the
 * parsed row/col on a reply, -1 on timeout/EOF/overflow. Non-reply bytes go to
 * `typed`; a trailing reply prefix is held in t->probe_tail for the next probe.
 *
 * 6a semantics: there is deliberately NO pre-query drain/discard. Bytes already
 * queued are read as part of the one bounded window below, so a complete reply
 * left pending by an earlier query (its answer landed after that probe's
 * deadline) is claimed by this call instead of being thrown away as "stale" --
 * discarding it made every probe in a late-answer resize storm time out on its
 * own query (0 real replies). The read buffer starts from the incomplete reply
 * prefix the previous probe held, if any. */
static int probe_once(Terminal *t, int *row, int *col, AgcBuf *typed) {
    size_t have = 0;
    int rr = 0, cc = 0;

    if (t->probe_tail_len) {
        agentc_memcpy(probe_buf, t->probe_tail, t->probe_tail_len);
        have = t->probe_tail_len;
        t->probe_tail_len = 0;
    }

    write_all(1, "\x1b[6n", 4);
    i64 deadline = os_now_ns(OS_CLOCK_MONOTONIC) + DSR_DEADLINE_MS * 1000000;
    for (;;) {
        i64 now = os_now_ns(OS_CLOCK_MONOTONIC);
        if (now >= deadline)
            break;
        int to = (int)((deadline - now) / 1000000);
        if (to < 1)
            to = 1;
        if (to > 50)
            to = 50;
        struct os_pollfd p = {0, OS_POLLIN, 0};
        int pr = os_poll(&p, 1, to);
        if (pr == -4 /*EINTR*/)
            continue;
        if (pr <= 0)
            continue;
        if (p.revents & (OS_POLLHUP | OS_POLLERR))
            break;
        if (have == sizeof probe_buf)
            break;
        int n = os_read(0, probe_buf + have, sizeof probe_buf - have);
        if (n == -4 /*EINTR*/)
            continue;
        if (n <= 0)
            break;
        have += (size_t)n;
        if (parse_cursor_reply(probe_buf, have, NULL, NULL, NULL, NULL)) {
            size_t hold = 0;
            take_replies(probe_buf, have, &rr, &cc, typed, t->probe_tail, &hold);
            t->probe_tail_len = hold;
            if (row)
                *row = rr;
            if (col)
                *col = cc;
            return 0;
        }
    }

    /* A complete reply is never dropped on the floor, even when the window
     * ended on EOF/overflow: claim it if the buffer holds one. */
    if (parse_cursor_reply(probe_buf, have, NULL, NULL, NULL, NULL)) {
        size_t hold = 0;
        take_replies(probe_buf, have, &rr, &cc, typed, t->probe_tail, &hold);
        t->probe_tail_len = hold;
        if (row)
            *row = rr;
        if (col)
            *col = cc;
        return 0;
    }

    /* No reply: hand everything back except a trailing reply prefix. */
    {
        size_t tl = reply_tail_len(probe_buf, have);
        if (typed && have > tl)
            agentc_buf_push(typed, probe_buf, have - tl);
        if (tl) {
            agentc_memcpy(t->probe_tail, probe_buf + have - tl, tl);
            t->probe_tail_len = tl;
        }
    }
    return -1;
}

int term_cursor(Terminal *t, int *row, int *col, AgcBuf *typed) {
    if (!t)
        return -1;
    if (!t->tty) {
        /* Memory backend: the test harness scripts the reply bytes. Nothing is
         * written to a real fd; the same reply/type-ahead separation runs so the
         * unit tests exercise the production parser. A miss is just -1; the
         * scripted queue is left untouched for the normal input path. */
        if (!t->test_dsr)
            return -1;
        size_t avail = t->in.len > t->in_pos ? t->in.len - t->in_pos : 0;
        size_t have = t->probe_tail_len;
        if (t->probe_tail_len)
            agentc_memcpy(probe_buf, t->probe_tail, t->probe_tail_len);
        if (have + avail > sizeof probe_buf)
            avail = sizeof probe_buf - have;
        if (avail)
            agentc_memcpy(probe_buf + have, t->in.p + t->in_pos, avail);
        have += avail;
        if (!parse_cursor_reply(probe_buf, have, NULL, NULL, NULL, NULL))
            return -1;
        int r = 0, c = 0;
        size_t hold = 0;
        take_replies(probe_buf, have, &r, &c, typed, t->probe_tail, &hold);
        t->probe_tail_len = hold;
        if (row)
            *row = r;
        if (col)
            *col = c;
        agentc_buf_clear(&t->in);
        t->in_pos = 0;
        return 0;
    }
    /* One query per call (N5): no double probe, so the terminal cannot coalesce
     * two replies into one read; a miss never disables later probes. */
    int r = 0, c = 0;
    if (probe_once(t, &r, &c, typed) != 0)
        return -1;
    if (row)
        *row = r;
    if (col)
        *col = c;
    return 0;
}

/* Test-only memory-backend DSR script (see tui_test.h). */
void term_mem_dsr_reply(Terminal *t, int row, int col) {
    if (!t || t->tty)
        return;
    t->test_dsr = true;
    if (row >= 1) {
        char seq[48];
        int n = agentc_snprintf(seq, sizeof seq, "\x1b[%d;%dR", row, col);
        if (n > 0)
            agentc_buf_push(&t->in, seq, (size_t)n);
    }
}

void term_mem_dsr_bytes(Terminal *t, const void *bytes, size_t n) {
    if (!t || t->tty)
        return;
    t->test_dsr = true;
    if (bytes && n)
        agentc_buf_push(&t->in, bytes, n);
}

bool term_mem_dsr_active(const Terminal *t) {
    return t && !t->tty && t->test_dsr;
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
    if (os_tty_size(0, &cols, &rows) != 0)
        return NULL;
    if (cols < 1)
        cols = 80;
    if (rows < 1)
        rows = 24;
    Terminal *t = agentc_alloc(sizeof *t);
    t->tty = true;
    t->cols = cols;
    t->rows = rows;
    return t;
}

bool term_is_tty(const Terminal *t) {
    return t && t->tty;
}

bool term_ascii_only(const Terminal *t) {
    if (!t || !t->tty)
        return false;
    const char *force = os_getenv("AGENTC_ASCII");
    if (force && force[0] && !agentc_streq(force, "0"))
        return true;
    const char *term = os_getenv("TERM");
    return term && agentc_streq(term, "dumb");
}

int term_cols(Terminal *t) {
    return t ? t->cols : 80;
}
int term_rows(Terminal *t) {
    return t ? t->rows : 24;
}

int term_enter_mode(Terminal *t, bool alt_screen) {
    if (!t || t->entered)
        return 0;
    const char *on = alt_screen ? TERM_ALT_ON TERM_PASTE_ON TERM_MOUSE_ON TERM_CURSOR_OFF
                                : TERM_PASTE_ON TERM_MOUSE_ON TERM_CURSOR_OFF;
    if (t->tty) {
        int rc = os_tty_raw(0, &t->saved);
        if (rc != 0)
            return rc; /* -errno: the caller must not run half-initialized */
        t->entered = true;
        t->alt = alt_screen;
        write_all(1, on, agentc_strlen(on));
    } else {
        t->entered = true;
        t->alt = alt_screen;
        agentc_buf_cstr(&t->out, on);
    }
    return 0;
}

int term_enter(Terminal *t) {
    return term_enter_mode(t, true);
}

void term_leave(Terminal *t) {
    if (!t || !t->entered)
        return;
    const char *off = t->alt
                          ? TERM_SYNC_OFF TERM_MOUSE_OFF TERM_CURSOR_ON TERM_PASTE_OFF TERM_ALT_OFF
                          : TERM_SYNC_OFF TERM_MOUSE_OFF TERM_CURSOR_ON TERM_PASTE_OFF;
    if (t->tty) {
        /* Reset the pushed background before the rest of the restore. This is
         * a constant write, so the signal path stays allocation-free. */
        if (t->bg_set)
            write_all(1, "\x1b]111\x1b\\", 7);
        write_all(1, off, agentc_strlen(off));
        os_tty_restore(0, t->saved);
    } else {
        if (t->bg_set)
            agentc_buf_cstr(&t->out, "\x1b]111\x1b\\");
        agentc_buf_cstr(&t->out, off);
    }
    t->bg_set = false;
    t->entered = false;
    t->alt = false;
}

void term_close(Terminal *t) {
    if (!t)
        return;
    term_leave(t);
    agentc_buf_free(&t->out);
    agentc_buf_free(&t->in);
    agentc_free(t);
}

void term_write(Terminal *t, const void *p, size_t n) {
    if (!t || !n)
        return;
    if (t->tty)
        write_all(1, p, n);
    else
        agentc_buf_push(&t->out, p, n);
}

/* Push an explicit background (OSC 11) so a fixed light/dark palette does not
 * draw dark text on a dark terminal. `term_leave` resets it (OSC 111). */
void term_set_bg(Terminal *t, u32 rgb) {
    if (!t)
        return;
    char seq[48];
    int n = agentc_snprintf(seq, sizeof seq, "\x1b]11;rgb:%02x/%02x/%02x\x1b\\",
                            (unsigned)((rgb >> 16) & 0xFF), (unsigned)((rgb >> 8) & 0xFF),
                            (unsigned)(rgb & 0xFF));
    if (n > 0)
        term_write(t, seq, (size_t)n);
    t->bg_set = true;
}

void term_reset_bg(Terminal *t) {
    if (!t || !t->bg_set)
        return;
    static const char seq[] = "\x1b]111\x1b\\";
    term_write(t, seq, sizeof seq - 1);
    t->bg_set = false;
}

int term_read(Terminal *t, u8 *buf, size_t cap) {
    if (!t)
        return 0;
    if (t->tty)
        return os_read(0, buf, cap);
    size_t avail = t->in.len > t->in_pos ? t->in.len - t->in_pos : 0;
    if (!avail)
        return 0;
    size_t n = avail < cap ? avail : cap;
    agentc_memcpy(buf, t->in.p + t->in_pos, n);
    t->in_pos += n;
    if (t->in_pos == t->in.len) {
        agentc_buf_clear(&t->in);
        t->in_pos = 0;
    }
    return (int)n;
}

const AgcBuf *term_output(Terminal *t) {
    return t ? &t->out : NULL;
}

void term_output_clear(Terminal *t) {
    if (t)
        agentc_buf_clear(&t->out);
}

bool term_check_resize(Terminal *t, int *cols, int *rows) {
    if (!t)
        return false;
    if (t->tty) {
        int c = 0, r = 0;
        if (os_tty_size(0, &c, &r) != 0 || c < 1 || r < 1)
            return false;
        if (c == t->cols && r == t->rows)
            return false;
        t->cols = c;
        t->rows = r;
    } else {
        if (t->mem_cols == t->cols && t->mem_rows == t->rows)
            return false;
        t->cols = t->mem_cols;
        t->rows = t->mem_rows;
    }
    if (cols)
        *cols = t->cols;
    if (rows)
        *rows = t->rows;
    return true;
}

/* Tests only: queue scripted input bytes and stage a resize. */
void term_mem_resize(Terminal *t, int cols, int rows) {
    if (!t || t->tty)
        return;
    t->mem_cols = cols;
    t->mem_rows = rows;
}

/* Internal helper used by the test harness to script input directly. */
void term_mem_feed(Terminal *t, const u8 *p, size_t n) {
    if (t && !t->tty)
        agentc_buf_push(&t->in, p, n);
}
