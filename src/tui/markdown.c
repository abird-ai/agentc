/* markdown.c — incremental markdown block renderer.
 *
 * Blocks are delimited by blank lines and fences. Appending text drops and
 * re-renders only the trailing incomplete block; completed blocks keep their
 * wrapped rows. Rendering happens at a fixed width, so resize re-renders all.
 */
#include "markdown.h"

/* ------------------------------------------------------------- block store */

static MdRun *row_open_run(MdRow *r, u16 attrs, u16 fg) {
    if (r->nruns == r->runs_cap) {
        size_t cap = r->runs_cap ? r->runs_cap * 2 : 4;
        r->runs = agentc_realloc(r->runs, cap * sizeof(MdRun));
        r->runs_cap = cap;
    }
    MdRun *run = &r->runs[r->nruns++];
    run->start = (u16)r->line.len;
    run->len = 0;
    run->attrs = attrs;
    run->fg = fg;
    return run;
}

static MdRow *block_new_row(MdBlock *b) {
    if (b->nrows == b->rows_cap) {
        size_t cap = b->rows_cap ? b->rows_cap * 2 : 4;
        b->rows = agentc_realloc(b->rows, cap * sizeof(MdRow));
        b->rows_cap = cap;
    }
    MdRow *r = &b->rows[b->nrows++];
    agentc_memset(r, 0, sizeof *r);
    return r;
}

static void row_add_run(MdRow *r, const char *p, size_t n, u16 attrs, u16 fg) {
    if (!n) return;
    MdRun *run = row_open_run(r, attrs, fg);
    run->len = (u16)n;
    agentc_buf_push(&r->line, p, n);
}

static void block_free_rows(MdBlock *b) {
    for (size_t i = 0; i < b->nrows; i++) {
        agentc_buf_free(&b->rows[i].line);
        agentc_free(b->rows[i].runs);
    }
    agentc_free(b->rows);
    b->rows = NULL;
    b->nrows = b->rows_cap = 0;
}

static MdBlock *markdown_push_block(Markdown *m, int kind, size_t start, size_t end,
                                    bool complete) {
    if (m->nblocks == m->blocks_cap) {
        size_t cap = m->blocks_cap ? m->blocks_cap * 2 : 4;
        m->blocks = agentc_realloc(m->blocks, cap * sizeof(MdBlock));
        m->blocks_cap = cap;
    }
    MdBlock *b = &m->blocks[m->nblocks++];
    agentc_memset(b, 0, sizeof *b);
    b->kind = kind;
    b->start = start;
    b->end = end;
    b->complete = complete;
    return b;
}

/* -------------------------------------------------------------- wrapping */

typedef struct {
    MdBlock *b;
    int width;
    int col;
    int indent;
    MdRow *row;
    u16 run_attrs;
    u16 run_fg;
    bool run_open;
    bool pending_space;
} Wrap;

static void wrap_new_row(Wrap *w) {
    w->row = block_new_row(w->b);
    w->run_open = false;
    w->col = 0;
    if (w->indent > 0) {
        for (int i = 0; i < w->indent; i++) row_add_run(w->row, " ", 1, 0, TH_MUTED);
        w->col = w->indent;
    }
}

static void wrap_put(Wrap *w, const char *p, size_t n, int cw, u16 attrs, u16 fg,
                     bool clip_only) {
    if (cw > 0 && w->col + cw > w->width) {
        if (clip_only) return;
        wrap_new_row(w);
        if (p[0] == ' ') return; /* a wrapped space disappears */
    }
    if (!w->row) wrap_new_row(w);
    if (p[0] == ' ' && w->col == 0) return;
    if (!w->run_open || w->run_attrs != attrs || w->run_fg != fg) {
        row_open_run(w->row, attrs, fg);
        w->run_open = true;
        w->run_attrs = attrs;
        w->run_fg = fg;
    }
    MdRun *run = &w->row->runs[w->row->nruns - 1];
    run->len = (u16)(run->len + n);
    agentc_buf_push(&w->row->line, p, n);
    w->col += cw;
}

/* Decode one UTF-8 codepoint, validating shape and range exactly as the grid
 * renderer does: overlong forms, surrogates and scalars above U+10FFFF yield
 * U+FFFD and consume one byte. Wrapping must agree with the renderer or a
 * malformed byte would shift the columns it draws into. */
static u32 utf8_cp_at(const char *s, size_t n, size_t *used) {
    u8 b = (u8)s[0];
    if (b < 0x80) { *used = 1; return b; }
    int need;
    u32 cp, min;
    if ((b & 0xE0) == 0xC0) { need = 2; cp = b & 0x1F; min = 0x80; }
    else if ((b & 0xF0) == 0xE0) { need = 3; cp = b & 0x0F; min = 0x800; }
    else if ((b & 0xF8) == 0xF0) { need = 4; cp = b & 0x07; min = 0x10000; }
    else { *used = 1; return 0xFFFD; }
    if ((size_t)need > n) { *used = 1; return 0xFFFD; }
    for (int i = 1; i < need; i++) {
        if (((u8)s[i] & 0xC0) != 0x80) { *used = 1; return 0xFFFD; }
        cp = (cp << 6) | ((u8)s[i] & 0x3F);
    }
    if (cp < min || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) { *used = 1; return 0xFFFD; }
    *used = (size_t)need;
    return cp;
}

/* Feed a styled text slice through the wrapper. Words move to the next row as
 * a unit; only words longer than the full width are split per codepoint. */
static void wrap_text(Wrap *w, const char *p, size_t n, u16 attrs, u16 fg, bool clip_only) {
    size_t i = 0;
    while (i < n) {
        u8 c = (u8)p[i];
        if (c == '\n' || c == '\r') { i++; continue; }
        if (c == ' ') {
            if (w->col > 0) w->pending_space = true;
            i++;
            continue;
        }
        /* measure the next word (combining marks add no width) */
        size_t j = i;
        int wlen = 0;
        while (j < n && p[j] != ' ' && p[j] != '\n' && p[j] != '\r') {
            size_t k;
            u32 cp = utf8_cp_at(p + j, n - j, &k);
            wlen += agentc_wcwidth(cp);
            j += k;
        }
        int space = w->pending_space ? 1 : 0;
        if (!clip_only && w->col > 0 && w->col + space + wlen > w->width) {
            wrap_new_row(w);
            w->pending_space = false;
        } else if (w->pending_space) {
            wrap_put(w, " ", 1, 1, attrs, fg, clip_only);
            w->pending_space = false;
        }
        size_t word_end = j;
        while (i < word_end) {
            size_t k;
            u32 cp = utf8_cp_at(p + i, word_end - i, &k);
            int cw = agentc_wcwidth(cp);
            if (cw == 0) {
                if (w->row && w->row->line.len) {
                    agentc_buf_push(&w->row->line, p + i, k);
                    if (w->row->nruns) w->row->runs[w->row->nruns - 1].len += (u16)k;
                }
            } else {
                wrap_put(w, p + i, k, cw, attrs, fg, clip_only);
            }
            i += k;
        }
    }
}

/* ------------------------------------------------------------ inline parse */

typedef struct {
    const char *p;
    size_t n;
    u16 attrs;
    u16 fg;
} Seg;

static int parse_inline(const char *s, size_t n, Seg *out, int max) {
    int ns = 0;
    size_t i = 0;
    /* Keep one slot free for the unstyled remainder: a line with more than
     * `max - 1` styled runs must emit its tail as plain text instead of
     * dropping it (the renderer passes a fixed `Seg[64]`). */
    int limit = max > 0 ? max - 1 : 0;
    while (i < n && ns < limit) {
        if (s[i] == '*' && i + 1 < n && s[i + 1] == '*') {
            size_t e = i + 2;
            while (e + 1 < n && !(s[e] == '*' && s[e + 1] == '*')) e++;
            if (e + 1 < n) {
                out[ns].p = s + i + 2;
                out[ns].n = e - (i + 2);
                out[ns].attrs = A_BOLD;
                out[ns].fg = 0xFFFF;
                ns++;
                i = e + 2;
                continue;
            }
        } else if (s[i] == '`') {
            size_t e = i + 1;
            while (e < n && s[e] != '`') e++;
            if (e < n) {
                out[ns].p = s + i + 1;
                out[ns].n = e - (i + 1);
                out[ns].attrs = 0;
                out[ns].fg = TH_CODE;
                ns++;
                i = e + 1;
                continue;
            }
        } else if ((s[i] == '*' || s[i] == '_') && i + 1 < n) {
            char mk = s[i];
            size_t e = i + 1;
            while (e < n && s[e] != mk) e++;
            if (e < n && e > i + 1) {
                out[ns].p = s + i + 1;
                out[ns].n = e - (i + 1);
                out[ns].attrs = A_ITALIC;
                out[ns].fg = 0xFFFF;
                ns++;
                i = e + 1;
                continue;
            }
        }
        /* literal run up to the next potential marker */
        size_t j = i;
        while (j < n) {
            if (s[j] == '`') break;
            if ((s[j] == '*' || s[j] == '_') && j + 1 < n) break;
            j++;
        }
        if (j == i) j = i + 1; /* unterminated marker stays literal */
        out[ns].p = s + i;
        out[ns].n = j - i;
        out[ns].attrs = 0;
        out[ns].fg = 0xFFFF;
        ns++;
        i = j;
    }
    if (i < n && ns < max) {
        out[ns].p = s + i;
        out[ns].n = n - i;
        out[ns].attrs = 0;
        out[ns].fg = 0xFFFF;
        ns++;
    }
    return ns;
}

/* --------------------------------------------------------- block rendering */

static bool line_is_blank(const char *s, size_t len, size_t i) {
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
    /* End-of-buffer is not a blank line: a streamed delta can end in '\n'
     * with more text still to come, and treating the buffer end as a blank
     * would complete the trailing paragraph and start a new block on the next
     * append. Only a real newline (or a block start) completes a paragraph;
     * the block is re-parsed on the next append. */
    return i < len && s[i] == '\n';
}

static bool line_starts_block(const char *s, size_t len, size_t i) {
    if (i >= len) return false;
    if (s[i] == '#') return true;
    if (i + 2 < len && (s[i] == '`' || s[i] == '~') && s[i + 1] == s[i] && s[i + 2] == s[i])
        return true;
    if ((s[i] == '-' || s[i] == '*' || s[i] == '+') && i + 1 < len && s[i + 1] == ' ')
        return true;
    return false;
}

static size_t line_end(const char *s, size_t len, size_t i) {
    while (i < len && s[i] != '\n') i++;
    return i;
}

static void render_flow(Markdown *m, MdBlock *b, bool bullet) {
    const char *s = (const char *)m->text.p + b->start;
    size_t len = b->end - b->start;
    Wrap w = { b, m->width, 0, 0, NULL, 0, 0, false, false };
    size_t i = 0;
    bool first = true;
    while (i < len) {
        size_t le = line_end(s, len, i);
        size_t ls = i;
        int indent = 0;
        if (bullet) {
            size_t k = ls;
            while (k < le && (s[k] == ' ' || s[k] == '\t')) k++;
            if (k + 1 < le && (s[k] == '-' || s[k] == '*' || s[k] == '+') && s[k + 1] == ' ')
                ls = k + 2;
            indent = 2;
        }
        if (bullet) {
            wrap_new_row(&w);
            row_add_run(w.row, "- ", 2, 0, TH_ACCENT);
            w.col = 2;
            w.indent = indent;
        } else if (!first && w.col > 0) {
            wrap_text(&w, " ", 1, 0, 0xFFFF, false);
        }
        Seg segs[64];
        int ns = parse_inline(s + ls, le - ls, segs, 64);
        for (int k = 0; k < ns; k++)
            wrap_text(&w, segs[k].p, segs[k].n, segs[k].attrs, segs[k].fg, false);
        i = le < len ? le + 1 : len;
        first = false;
    }
    if (!b->nrows) block_new_row(b);
}

static void render_heading(Markdown *m, MdBlock *b) {
    const char *s = (const char *)m->text.p + b->start;
    size_t len = b->end - b->start;
    size_t i = 0;
    int level = 0;
    while (i < len && s[i] == '#' && level < 6) { i++; level++; }
    while (i < len && s[i] == ' ') i++;
    size_t le = line_end(s, len, i);
    Wrap w = { b, m->width, 0, 0, NULL, 0, 0, false, false };
    wrap_new_row(&w);
    u16 attrs = A_BOLD;
    u16 fg = level <= 1 ? TH_ACCENT : 0xFFFF;
    if (level >= 3) attrs |= A_DIM;
    Seg segs[64];
    int ns = parse_inline(s + i, le - i, segs, 64);
    for (int k = 0; k < ns; k++) {
        u16 sf = segs[k].fg == 0xFFFF ? fg : segs[k].fg;
        wrap_text(&w, segs[k].p, segs[k].n, attrs | segs[k].attrs, sf, false);
    }
    if (!b->nrows) block_new_row(b);
}

static void render_code(Markdown *m, MdBlock *b) {
    const char *s = (const char *)m->text.p + b->start;
    size_t len = b->end - b->start;
    char fence_ch = s[0];   /* opening fence char: the block starts at that line */
    size_t i = 0;
    bool first_line = true;
    while (i < len) {
        size_t le = line_end(s, len, i);
        size_t ls = i;
        if (first_line) {
            first_line = false;
            i = le < len ? le + 1 : len;
            continue;
        }
        if (le - ls >= 3 && s[ls] == fence_ch && s[ls + 1] == fence_ch && s[ls + 2] == fence_ch)
            break; /* closing fence (same char as the opening) */
        Wrap w = { b, m->width, 0, 0, NULL, 0, 0, false, false };
        w.width = m->width;
        wrap_new_row(&w);
        wrap_text(&w, s + ls, le - ls, A_DIM, TH_THINKING, true);
        i = le < len ? le + 1 : len;
    }
    if (!b->nrows) block_new_row(b);
}

static void md_render_block(Markdown *m, MdBlock *b) {
    block_free_rows(b);
    switch (b->kind) {
    case MD_HEAD: render_heading(m, b); break;
    case MD_CODE: render_code(m, b); break;
    case MD_BULLET: render_flow(m, b, true); break;
    default: render_flow(m, b, false); break;
    }
}

/* --------------------------------------------------------------- parsing */

static bool fence_start(const char *s, size_t len, size_t i) {
    return i + 2 < len && (s[i] == '`' || s[i] == '~') && s[i + 1] == s[i] && s[i + 2] == s[i];
}

static bool fence_close(const char *s, size_t len, size_t i, char c) {
    return i + 2 < len && s[i] == c && s[i + 1] == c && s[i + 2] == c;
}

static void md_parse(Markdown *m, size_t from) {
    const char *s = (const char *)m->text.p;
    size_t len = m->text.len;
    size_t i = from;
    while (i < len) {
        while (i < len && line_is_blank(s, len, i)) {
            size_t le = line_end(s, len, i);
            i = le < len ? le + 1 : len;
        }
        if (i >= len) break;

        if (fence_start(s, len, i)) {
            char fence_ch = s[i];
            size_t q = line_end(s, len, i);
            q = q < len ? q + 1 : len;
            size_t end = len;
            bool complete = false;
            while (q <= len) {
                size_t qe = line_end(s, len, q);
                if (fence_close(s, len, q, fence_ch)) {
                    end = qe < len ? qe + 1 : qe;
                    complete = true;
                    break;
                }
                if (qe >= len) break;
                q = qe + 1;
            }
            MdBlock *b = markdown_push_block(m, MD_CODE, i, end, complete);
            md_render_block(m, b);
            i = end;
            continue;
        }

        if (s[i] == '#') {
            size_t le = line_end(s, len, i);
            size_t end = le < len ? le + 1 : le;
            MdBlock *b = markdown_push_block(m, MD_HEAD, i, end, true);
            md_render_block(m, b);
            i = end;
            continue;
        }

        bool bullet = false;
        {
            size_t k = i;
            while (k < len && (s[k] == ' ' || s[k] == '\t')) k++;
            if (k + 1 < len && (s[k] == '-' || s[k] == '*' || s[k] == '+') && s[k + 1] == ' ')
                bullet = true;
        }

        size_t q = i;
        size_t end = i;
        bool complete = false;
        while (q < len) {
            size_t qe = line_end(s, len, q);
            end = qe < len ? qe + 1 : qe;
            if (qe >= len) break;
            size_t nq = qe + 1;
            if (line_is_blank(s, len, nq)) { complete = true; break; }
            if (line_starts_block(s, len, nq)) { complete = true; break; }
            q = nq;
        }
        MdBlock *b = markdown_push_block(m, bullet ? MD_BULLET : MD_PARA, i, end, complete);
        md_render_block(m, b);
        i = end;
    }
}

void md_init(Markdown *m, u16 base_fg, u16 base_attrs) {
    agentc_memset(m, 0, sizeof *m);
    m->width = 80;
    m->base_fg = base_fg;
    m->base_attrs = base_attrs;
}

void md_free(Markdown *m) {
    for (size_t i = 0; i < m->nblocks; i++) block_free_rows(&m->blocks[i]);
    agentc_free(m->blocks);
    agentc_buf_free(&m->text);
    m->blocks = NULL;
    m->nblocks = m->blocks_cap = 0;
}

void md_reset(Markdown *m) {
    md_free(m);
    m->width = 80;
}

void md_set_width(Markdown *m, int width) {
    if (width < 1) width = 1;
    if (width == m->width) return;
    m->width = width;
    /* full re-render once per resize */
    for (size_t i = 0; i < m->nblocks; i++) md_render_block(m, &m->blocks[i]);
}

void md_append(Markdown *m, const char *p, size_t n) {
    if (!p || !n) return;
    agentc_buf_push(&m->text, p, n);
    size_t from;
    if (m->nblocks && !m->blocks[m->nblocks - 1].complete) {
        from = m->blocks[m->nblocks - 1].start;
        block_free_rows(&m->blocks[m->nblocks - 1]);
        m->nblocks--;
    } else if (m->nblocks) {
        from = m->blocks[m->nblocks - 1].end;
    } else {
        from = 0;
    }
    md_parse(m, from);
}

size_t md_height(const Markdown *m) {
    size_t n = 0;
    for (size_t i = 0; i < m->nblocks; i++) n += m->blocks[i].nrows;
    return n;
}

const MdBlock *md_blocks(const Markdown *m, size_t *nblocks) {
    if (nblocks) *nblocks = m->nblocks;
    return m->blocks;
}
