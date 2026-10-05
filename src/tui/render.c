/* render.c — cell grid, width table, and the differential writer.
 *
 * The render path never allocates: grids are sized on resize, the diff writer
 * appends into a caller-owned AgcBuf. A frame is one contiguous byte sequence
 * the terminal backend writes with a single os_write.
 */
#include "render.h"

/* ------------------------------------------------------------ width table */

/* Hand-maintained Unicode width table. It is not generated from the Unicode
 * data files: it covers the combining-mark blocks this TUI actually meets and
 * the East Asian Wide/Fullwidth ranges most terminals render double-width.
 * `agentc_is_combining` also drives grid_put's decision to attach a zero-width
 * mark to the preceding cell. Anything outside the ranges falls back to width
 * 1, which is the conservative choice: a miscounted single-width glyph can
 * shift the caret by one column, while treating a narrow glyph as wide would
 * leave a visible hole. */
bool agentc_is_combining(u32 cp) {
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) ||
           (cp >= 0x0591 && cp <= 0x05BD) || (cp >= 0x0610 && cp <= 0x061A) ||
           (cp >= 0x064B && cp <= 0x065F) || (cp >= 0x0670 && cp <= 0x0670) ||
           (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x06DF && cp <= 0x06E4) ||
           (cp >= 0x06E7 && cp <= 0x06E8) || (cp >= 0x06EA && cp <= 0x06ED) ||
           (cp >= 0x0711 && cp <= 0x0711) || (cp >= 0x0730 && cp <= 0x074A) ||
           (cp >= 0x07A6 && cp <= 0x07B0) || (cp >= 0x0900 && cp <= 0x0902) ||
           (cp >= 0x093C && cp <= 0x093C) || (cp >= 0x0941 && cp <= 0x0948) ||
           (cp >= 0x094D && cp <= 0x094D) || (cp >= 0x0951 && cp <= 0x0957) ||
           (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF);
}

/* Zero-width formatting and joiner codepoints (Unicode Cf plus the soft hyphen
 * and the deprecated Mongolian vowel separator). They must not spend a column,
 * or a ZWJ emoji sequence and a BOM would shift the cursor and wrap math. The
 * set also covers the bidi embedding/override controls and the word-joiner
 * shaped operators, so none of them is emitted as a visible glyph. */
static bool is_zero_width(u32 cp) {
    return cp == 0x00AD || cp == 0x180E || cp == 0x200B || cp == 0x200C ||
           cp == 0x200D || cp == 0x200E || cp == 0x200F ||
           (cp >= 0x202A && cp <= 0x202E) || cp == 0x2060 ||
           (cp >= 0x2061 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x206F) ||
           cp == 0xFEFF;
}

int agentc_wcwidth(u32 cp) {
    if (cp == 0) return 0;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if (agentc_is_combining(cp) || is_zero_width(cp)) return 0;
    static const struct { u32 lo, hi; } wide[] = {
        { 0x1100, 0x115F },  { 0x2329, 0x232A },  { 0x2E80, 0x303E },
        { 0x3041, 0x33FF },  { 0x3400, 0x4DBF },  { 0x4E00, 0x9FFF },
        { 0xA000, 0xA4CF },  { 0xA960, 0xA97F },  { 0xAC00, 0xD7A3 },
        { 0xF900, 0xFAFF },  { 0xFE10, 0xFE19 },  { 0xFE30, 0xFE6F },
        { 0xFF00, 0xFF60 },  { 0xFFE0, 0xFFE6 },  { 0x1F000, 0x1F2FF },
        { 0x1F300, 0x1F64F },
        { 0x1F680, 0x1F6FF }, { 0x1F7E0, 0x1F7EB }, { 0x1F7F0, 0x1F7F0 },
        { 0x1F900, 0x1F9FF }, { 0x1FA70, 0x1FAFF }, { 0x20000, 0x2FFFD },
        { 0x30000, 0x3FFFD },
    };
    for (size_t i = 0; i < sizeof wide / sizeof wide[0]; i++)
        if (cp >= wide[i].lo && cp <= wide[i].hi) return 2;
    return 1;
}

/* Decode one UTF-8 codepoint; invalid bytes yield U+FFFD and consume one byte. */
static u32 utf8_decode(const u8 *p, size_t n, size_t *used) {
    u8 b = p[0];
    if (b < 0x80) { *used = 1; return b; }
    int need;
    u32 cp;
    if ((b & 0xE0) == 0xC0) { need = 2; cp = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { need = 3; cp = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { need = 4; cp = b & 0x07; }
    else { *used = 1; return 0xFFFD; }
    if ((size_t)need > n) { *used = 1; return 0xFFFD; }
    for (int i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) { *used = 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *used = (size_t)need;
    return cp;
}

/* Display width of UTF-8 text: the sum of agentc_wcwidth() over its codepoints.
 * Invalid bytes count as U+FFFD (width 1), matching how they render. Used by
 * the footer to place the right slot by display columns rather than byte count. */
int agentc_text_width(const char *s) {
    if (!s) return 0;
    int w = 0;
    size_t n = agentc_strlen(s);
    for (size_t i = 0; i < n;) {
        size_t k;
        u32 cp = utf8_decode((const u8 *)s + i, n - i, &k);
        w += agentc_wcwidth(cp);
        i += k;
    }
    return w;
}

/* -------------------------------------------------------------- grid core */

void grid_init(Grid *g, int cols, int rows) {
    g->cells = NULL;
    g->cols = g->rows = 0;
    grid_resize(g, cols, rows);
}

void grid_free(Grid *g) {
    agentc_free(g->cells);
    g->cells = NULL;
    g->cols = g->rows = 0;
}

void grid_resize(Grid *g, int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > GRID_MAX_COLS) cols = GRID_MAX_COLS;
    if (rows > GRID_MAX_ROWS) rows = GRID_MAX_ROWS;
    if (g->cells && g->cols == cols && g->rows == rows) return;
    agentc_free(g->cells);
    g->cells = agentc_alloc((size_t)cols * (size_t)rows * sizeof(Cell));
    g->cols = cols;
    g->rows = rows;
    grid_invalidate(g);
}

void grid_clear(Grid *g, u16 attrs, u16 fg, u16 bg) {
    Cell blank = { ' ', 0, attrs, fg, bg };
    for (int i = 0; i < g->cols * g->rows; i++) g->cells[i] = blank;
}

void grid_invalidate(Grid *g) {
    for (int i = 0; i < g->cols * g->rows; i++) {
        g->cells[i].cp = CELL_INVALID;
        g->cells[i].comb = 0;
        g->cells[i].attrs = 0;
        g->cells[i].fg = 0;
        g->cells[i].bg = 0;
    }
}

Cell *grid_at(Grid *g, int x, int y) {
    if (!g->cells || x < 0 || y < 0 || x >= g->cols || y >= g->rows) return NULL;
    return &g->cells[(size_t)y * (size_t)g->cols + (size_t)x];
}

void grid_fill(Grid *g, int x, int y, int w, u16 attrs, u16 fg, u16 bg, u32 cp) {
    if (y < 0 || y >= g->rows) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > g->cols) w = g->cols - x;
    for (int i = 0; i < w; i++) {
        Cell *c = grid_at(g, x + i, y);
        c->cp = cp;
        c->comb = 0;
        c->attrs = attrs;
        c->fg = fg;
        c->bg = bg;
    }
}

void grid_hline(Grid *g, int x, int y, int w, u16 attrs, u16 fg, u16 bg, u32 cp) {
    grid_fill(g, x, y, w, attrs, fg, bg, cp);
}

/* Bytes of s that fit in `maxw` display columns starting at column x. */
static size_t clip_bytes(const Grid *g, int x, int maxw, const char *s, size_t n) {
    size_t i = 0;
    int used = 0;
    while (i < n) {
        size_t k;
        u32 cp = utf8_decode((const u8 *)s + i, n - i, &k);
        if (cp == '\n' || cp == '\r') break;
        int w = agentc_wcwidth(cp);
        if (used + w > maxw || x + used + w > g->cols) break;
        used += w;
        i += k;
    }
    return i;
}

int grid_put(Grid *g, int x, int y, u16 attrs, u16 fg, u16 bg, const char *s, size_t n) {
    if (!g || !g->cells || !s) return x;
    size_t i = 0;
    while (i < n) {
        size_t k;
        u32 cp = utf8_decode((const u8 *)s + i, n - i, &k);
        i += k;
        if (cp == '\r') continue;
        if (cp == '\n') { x = 0; y++; if (y >= g->rows) break; continue; }
        if (cp == '\t') {
            int spaces = 4 - (x < 0 ? 0 : x % 4);
            for (int t = 0; t < spaces; t++, x++) {
                Cell *c = grid_at(g, x, y);
                if (c) { c->cp = ' '; c->comb = 0; c->attrs = attrs; c->fg = fg; c->bg = bg; }
            }
            continue;
        }
        int w = agentc_wcwidth(cp);
        if (w == 0) {
            /* Only genuine combining marks may attach to the previous cell. A
             * stray C0 control or DEL (possible in model or tool output) must
             * never be written to the terminal: it would be an escape injection. */
            if (!agentc_is_combining(cp)) {
                if (cp < 0x20 || cp == 0x7f) cp = ' ';
                else continue;
            } else {
                /* A combining mark after a wide glyph attaches to the base
                 * cell, not the CELL_CONT continuation cell (whose glyph is
                 * skipped when rendering), or the mark would never be emitted. */
                int bx = x - 1;
                Cell *base = grid_at(g, bx, y);
                while (bx > 0 && base && base->cp == CELL_CONT) {
                    bx--;
                    base = grid_at(g, bx, y);
                }
                if (base && base->cp != CELL_INVALID && base->cp != ' ' &&
                    base->cp != CELL_CONT && !base->comb)
                    base->comb = cp;
                continue;
            }
        }
        if (x < 0) { x += w; continue; }
        if (x + w > g->cols) { /* clip the glyph (wide char at the right edge) */
            Cell *c = grid_at(g, x, y);
            if (c) { c->cp = ' '; c->comb = 0; c->attrs = attrs; c->fg = fg; c->bg = bg; }
            break;
        }
        Cell *c = grid_at(g, x, y);
        if (!c) { x += w; continue; }
        c->cp = cp;
        c->comb = 0;
        c->attrs = attrs;
        c->fg = fg;
        c->bg = bg;
        if (w == 2) {
            Cell *d = grid_at(g, x + 1, y);
            d->cp = CELL_CONT;
            d->comb = 0;
            d->attrs = attrs;
            d->fg = fg;
            d->bg = bg;
        }
        x += w;
    }
    return x;
}

int grid_put_clip(Grid *g, int x, int y, int maxw, u16 attrs, u16 fg, u16 bg,
                  const char *s, size_t n) {
    if (!g || !g->cells || !s) return x;
    size_t fit = clip_bytes(g, x, maxw, s, n);
    return grid_put(g, x, y, attrs, fg, bg, s, fit);
}

int grid_puts(Grid *g, int x, int y, u16 attrs, u16 fg, u16 bg, const char *s) {
    return grid_put(g, x, y, attrs, fg, bg, s, s ? agentc_strlen(s) : 0);
}

int grid_printf(Grid *g, int x, int y, int maxw, u16 attrs, u16 fg, u16 bg,
                const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    agentc_vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    size_t n = agentc_strlen(tmp);
    if (maxw >= 0) n = clip_bytes(g, x, maxw, tmp, n);
    return grid_put(g, x, y, attrs, fg, bg, tmp, n);
}

/* ------------------------------------------------------- differential write */

/* First changed / last changed column in a row; -1 when equal. */
static int row_changed(const Cell *a, const Cell *b, int cols, int *last) {
    int first = -1;
    *last = -1;
    for (int i = 0; i < cols; i++) {
        if (a[i].cp != b[i].cp || a[i].comb != b[i].comb || a[i].attrs != b[i].attrs ||
            a[i].fg != b[i].fg || a[i].bg != b[i].bg) {
            if (first < 0) first = i;
            *last = i;
        }
    }
    return first;
}

static void emit_cell_cp(AgcBuf *out, const Cell *c) {
    u8 tmp[8];
    if (c->cp == CELL_CONT || c->cp == 0 || c->cp > 0x10FFFF) return;
    if (c->cp == CELL_INVALID) {
        agentc_buf_byte(out, ' ');          /* keep the row width honest */
        return;
    }
    /* defence in depth: never emit a raw control byte to the terminal */
    u32 cp = (c->cp < 0x20 || c->cp == 0x7f) ? ' ' : c->cp;
    size_t n = agentc_utf8_encode(cp, tmp);
    agentc_buf_push(out, tmp, n);
    if (c->comb && c->comb >= 0x20 && c->comb != 0x7f) {
        size_t m = agentc_utf8_encode(c->comb, tmp);
        agentc_buf_push(out, tmp, m);
    }
}

int render_diff(const Grid *prev, const Grid *next, AgcBuf *out, const Theme *th) {
    int changed = 0;
    for (int y = 0; y < next->rows; y++) {
        const Cell *pa = prev && prev->cells && y < prev->rows && prev->cols == next->cols
                             ? prev->cells + (size_t)y * next->cols
                             : NULL;
        const Cell *na = next->cells + (size_t)y * next->cols;
        int last;
        int first;
        if (pa) {
            first = row_changed(pa, na, next->cols, &last);
        } else {
            first = 0;
            last = next->cols - 1;
        }
        if (first < 0) continue;
        changed++;
        agentc_buf_printf(out, "\x1b[%d;1H", y + 1);
        u16 ca = 0xFFFF, cfg = 0xFFFF, cbg = 0xFFFF;
        for (int x = 0; x <= last && x < next->cols; x++) {
            const Cell *c = &na[x];
            if (c->cp == CELL_CONT) continue;
            if (c->attrs != ca || c->fg != cfg || c->bg != cbg) {
                theme_emit_sgr(out, th, c->attrs, c->fg, c->bg);
                ca = c->attrs;
                cfg = c->fg;
                cbg = c->bg;
            }
            emit_cell_cp(out, c);
        }
        if (last < next->cols - 1) agentc_buf_cstr(out, "\x1b[K");
    }
    return changed;
}

void render_row_text(const Grid *g, int y, AgcBuf *out) {
    if (!g || !g->cells || y < 0 || y >= g->rows) return;
    const Cell *na = g->cells + (size_t)y * g->cols;
    size_t start = out->len;
    for (int x = 0; x < g->cols; x++) {
        const Cell *c = &na[x];
        if (c->cp == CELL_CONT) continue;
        if (c->cp == CELL_INVALID || c->cp == 0) {
            agentc_buf_byte(out, ' ');
            continue;
        }
        emit_cell_cp(out, c);
    }
    while (out->len > start && out->p[out->len - 1] == ' ') out->len--;
    if (out->p) out->p[out->len] = 0;
}

void render_rows_ansi(const Grid *g, int y0, int y1, AgcBuf *out, const Theme *th,
                      bool newline) {
    if (!g || !g->cells) return;
    if (y0 < 0) y0 = 0;
    if (y1 > g->rows) y1 = g->rows;
    for (int y = y0; y < y1; y++) {
        const Cell *na = g->cells + (size_t)y * g->cols;
        int last = -1;
        for (int x = g->cols - 1; x >= 0; x--) {
            u32 cp = na[x].cp;
            /* A reverse-video cell is the caret fallback even when it holds a
             * space, so it must not be trimmed as a trailing blank. A themed
             * background (e.g. a tool card band or the status line) must reach
             * the row's right edge for the same reason. */
            bool content = cp != CELL_CONT && cp != CELL_INVALID && cp != 0 && cp != ' ';
            bool band = cp != CELL_INVALID && na[x].bg != TH_NO_BG;
            if (cp != CELL_CONT && (content || band || (na[x].attrs & A_REVERSE))) {
                last = x;
                break;
            }
        }
        agentc_buf_cstr(out, "\r\x1b[2K");
        u16 ca = 0xFFFF, cfg = 0xFFFF, cbg = 0xFFFF;
        for (int x = 0; x <= last; x++) {
            const Cell *c = &na[x];
            if (c->cp == CELL_CONT) continue;
            if (c->attrs != ca || c->fg != cfg || c->bg != cbg) {
                theme_emit_sgr(out, th, c->attrs, c->fg, c->bg);
                ca = c->attrs;
                cfg = c->fg;
                cbg = c->bg;
            }
            emit_cell_cp(out, c);
        }
        if (last >= 0) agentc_buf_cstr(out, "\x1b[0m");
        if (newline || y + 1 < y1) agentc_buf_cstr(out, "\r\n");
    }
}

void render_screen_text(const Grid *g, AgcBuf *out) {
    if (!g || !g->cells) return;
    for (int y = 0; y < g->rows; y++) {
        size_t start = out->len;
        for (int x = 0; x < g->cols; x++) {
            const Cell *c = &g->cells[(size_t)y * g->cols + x];
            if (c->cp == CELL_CONT) continue;
            if (c->cp == CELL_INVALID || c->cp == 0) {
                agentc_buf_byte(out, ' ');
                continue;
            }
            emit_cell_cp(out, c);
        }
        /* trim trailing blanks, including any combining marks left behind */
        while (out->len > start) {
            u8 last = out->p[out->len - 1];
            if (last == ' ') { out->len--; continue; }
            break;
        }
        if (out->p) out->p[out->len] = 0;
        agentc_buf_byte(out, '\n');
    }
}
