/* components.c — immediate-mode-ish chat viewport, tool cards, footer, queue.
 *
 * TODO(m4+): dialog/overlay widgets, hunk folding in the diff view, and
 * per-session git branch in the footer.
 *
 * Text blocks cache their wrapped rows inside Markdown; tool cards render the
 * tail of the output AgcBuf directly (no per-line allocation). The viewport
 * sticks to the bottom unless the user scrolled up.
 */
#include "components.h"

/* Slash-command menu. Every row is chrome on the terminal background; the
 * selected row is reverse-video (no theme background band, so it cannot be
 * confused with the status line). Names are laid out in one column followed by
 * a two-space gap and the description; both clip, neither wraps. */
void comp_command_menu(Grid *g, const Theme *th, int x, int y, int w, int maxrows,
                       const char *const *names, const char *const *descs,
                       size_t n, size_t top, size_t sel) {
    (void)th;
    if (!g || !names || w <= 0 || maxrows <= 0 || !n) return;
    if (sel >= n) sel = n - 1;
    size_t namew = 0;
    for (size_t i = 0; i < n; i++) {
        size_t l = agentc_strlen(names[i]);
        if (l > namew) namew = l;
    }
    if (namew > (size_t)w - 1) namew = (size_t)w - 1;
    for (int r = 0; r < maxrows && top + (size_t)r < n; r++) {
        size_t i = top + (size_t)r;
        int yy = y + r;
        u16 attrs = i == sel ? A_REVERSE : 0;
        int cx = grid_put_clip(g, x, yy, w, attrs, TH_FG, TH_NO_BG, "/", 1);
        cx = grid_put_clip(g, cx, yy, w - (cx - x), attrs, TH_FG, TH_NO_BG,
                           names[i], agentc_strlen(names[i]));
        const char *d = descs ? descs[i] : NULL;
        if (d && d[0]) {
            int col = x + (int)namew + 2;
            if (col < x + w)
                grid_put_clip(g, col, yy, x + w - col, attrs | A_DIM,
                              i == sel ? TH_FG : TH_MUTED, TH_NO_BG, d,
                              agentc_strlen(d));
        }
    }
}

/* ------------------------------------------------------------------- chat */

static ChatBlock *chat_push_impl(Chat *c, int kind, u16 fg, u16 attrs) {
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 8;
        c->blocks = agentc_realloc(c->blocks, cap * sizeof(ChatBlock));
        c->cap = cap;
    }
    ChatBlock *b = &c->blocks[c->n++];
    agentc_memset(b, 0, sizeof *b);
    b->kind = kind;
    b->base_fg = fg;
    b->base_attrs = attrs;
    md_init(&b->md, fg, attrs);
    int mw = (kind == CHAT_USER || kind == CHAT_THINK) ? c->width - 2 : c->width;
    md_set_width(&b->md, mw > 0 ? mw : 1);
    return b;
}

void chat_init(Chat *c) {
    agentc_memset(c, 0, sizeof *c);
    c->width = 80;
}

void chat_free(Chat *c) {
    for (size_t i = 0; i < c->n; i++) {
        ChatBlock *b = &c->blocks[i];
        md_free(&b->md);
        agentc_free(b->tool_name);
        agentc_free(b->tool_args);
        agentc_buf_free(&b->output);
    }
    agentc_free(c->blocks);
    c->blocks = NULL;
    c->n = c->cap = 0;
}

ChatBlock *chat_push(Chat *c, int kind, u16 fg, u16 attrs) {
    return chat_push_impl(c, kind, fg, attrs);
}

ChatBlock *chat_last(Chat *c) { return c->n ? &c->blocks[c->n - 1] : NULL; }

void chat_clear(Chat *c) {
    for (size_t i = 0; i < c->n; i++) {
        ChatBlock *b = &c->blocks[i];
        md_free(&b->md);
        agentc_free(b->tool_name);
        agentc_free(b->tool_args);
        agentc_buf_free(&b->output);
    }
    c->n = 0;
    c->scroll = 0;
}

void chat_set_width(Chat *c, int width) {
    if (width < 1) width = 1;
    if (width == c->width) return;
    c->width = width;
    for (size_t i = 0; i < c->n; i++) {
        int mw = (c->blocks[i].kind == CHAT_USER || c->blocks[i].kind == CHAT_THINK)
                     ? width - 2
                     : width;
        md_set_width(&c->blocks[i].md, mw > 0 ? mw : 1);
    }
}

static void append_to(Chat *c, int kind, u16 fg, u16 attrs, const char *p, size_t n) {
    ChatBlock *b = (c->n && c->blocks[c->n - 1].kind == kind) ? &c->blocks[c->n - 1]
                                                               : chat_push_impl(c, kind, fg, attrs);
    md_append(&b->md, p, n);
}

void chat_append_text(Chat *c, const char *p, size_t n) {
    append_to(c, CHAT_ASSISTANT, TH_ASSISTANT, 0, p, n);
}

void chat_append_think(Chat *c, const char *p, size_t n) {
    append_to(c, CHAT_THINK, TH_THINKING, A_DIM | A_ITALIC, p, n);
}

void chat_append_user(Chat *c, const char *p, size_t n) {
    /* Preserve user line breaks as paragraph breaks. */
    AgcBuf b = { 0 };
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n')
            agentc_buf_cstr(&b, "\n\n");
        else
            agentc_buf_byte(&b, (u8)p[i]);
    }
    append_to(c, CHAT_USER, TH_USER, A_BOLD, (const char *)b.p, b.len);
    agentc_buf_free(&b);
}

void chat_append_notice(Chat *c, const char *p, size_t n) {
    ChatBlock *b = chat_push_impl(c, CHAT_NOTICE, TH_MUTED, 0);
    md_append(&b->md, p, n);
}

void chat_tool_start(Chat *c, const char *name, const char *args, i64 now_ms) {
    ChatBlock *b = chat_push_impl(c, CHAT_TOOL, TH_TOOL, 0);
    b->running = true;
    b->tool_name = agentc_strdup(name ? name : "?");
    b->tool_args = agentc_strdup(args ? args : "");
    b->started_ms = now_ms;
    b->tool_seq = (int)c->n;
}

void chat_tool_end(Chat *c, const char *name, const char *result, bool is_error,
                   i64 duration_ms) {
    ChatBlock *found = NULL;
    for (size_t i = c->n; i > 0; i--) {
        ChatBlock *b = &c->blocks[i - 1];
        if (b->kind != CHAT_TOOL || !b->running) continue;
        if (name && b->tool_name && !agentc_streq(b->tool_name, name)) continue;
        found = b;
        break;
    }
    if (!found) {
        chat_tool_start(c, name, "", 0);
        found = chat_last(c);
    }
    found->running = false;
    found->is_error = is_error;
    found->duration_ms = duration_ms;
    if (result) agentc_buf_push(&found->output, result, agentc_strlen(result));
}

void chat_toggle_last_tool(Chat *c) {
    for (size_t i = c->n; i > 0; i--) {
        ChatBlock *b = &c->blocks[i - 1];
        if (b->kind == CHAT_TOOL) {
            b->expanded = !b->expanded;
            return;
        }
    }
}

size_t chat_last_tool_index(const Chat *c) {
    for (size_t i = c->n; i > 0; i--)
        if (c->blocks[i - 1].kind == CHAT_TOOL) return i - 1;
    return c->n;
}

void chat_scroll(Chat *c, int delta) {
    c->scroll += delta;
    if (c->scroll < 0) c->scroll = 0;
}

void chat_scroll_bottom(Chat *c) { c->scroll = 0; }

bool chat_has_tool(const Chat *c) {
    for (size_t i = 0; i < c->n; i++)
        if (c->blocks[i].kind == CHAT_TOOL) return true;
    return false;
}

/* -------------------------------------------------------------- geometry */

static int count_lines(const char *s, size_t len) {
    if (len == 0) return 0;
    int n = 0;
    for (size_t i = 0; i < len; i++)
        if (s[i] == '\n') n++;
    if (s[len - 1] != '\n') n++;
    return n;
}

static int tool_shown_lines(const ChatBlock *b) {
    int total = count_lines(b->output.p ? (const char *)b->output.p : "", b->output.len);
    int max = b->expanded ? 10 : 3;
    return total < max ? total : max;
}

static int block_height(const ChatBlock *b) {
    if (b->kind == CHAT_TOOL) {
        int total = count_lines(b->output.p ? (const char *)b->output.p : "", b->output.len);
        int shown = tool_shown_lines(b);
        int h = 1 + shown;
        if (!b->expanded && total > shown) h++; /* "... +N more lines" */
        return h + 1; /* gap */
    }
    return (int)md_height(&b->md) + 1;
}

/* Copy the last `max` lines of s into out (forward order); returns the line
 * count filled; *total receives the total number of lines. */
typedef struct { const char *p; size_t n; } Slice;

static int tail_lines(const char *s, size_t len, Slice *out, int max, int *total) {
    int n = count_lines(s, len);
    *total = n;
    int skip = n > max ? n - max : 0;
    if (len && s[len - 1] == '\n') len--; /* ignore the trailing empty line */
    int line = 0, filled = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || s[i] == '\n') {
            if (line >= skip && filled < max) {
                out[filled].p = s + start;
                out[filled].n = i - start;
                filled++;
            }
            line++;
            start = i + 1;
        }
    }
    return filled;
}

static u16 diff_color(const ChatBlock *b, const char *p, size_t n) {
    if (!b->tool_name || !agentc_streq(b->tool_name, "edit")) return TH_FG;
    if (n == 0) return TH_FG;
    if (p[0] == '+') return TH_DIFF_ADD;
    if (p[0] == '-') return TH_DIFF_DEL;
    if (p[0] == '@') return TH_ACCENT;
    return TH_FG;
}

/* Outcome background for a tool card. A card is pending until its end event
 * lands (a running card, or one whose end never arrived, e.g. after an abort);
 * a finished card is success unless the tool reported an error. */
static u16 tool_bg_slot(const ChatBlock *b) {
    if (b->running) return TH_TOOL_BG;
    return b->is_error ? TH_TOOL_ERR_BG : TH_TOOL_OK_BG;
}

static void render_md_block(const ChatBlock *b, Grid *g, const Theme *th, int x, int y,
                            int w, int skip, int maxrows, u16 prefix_fg, const char *prefix) {
    (void)th;
    const MdBlock *blocks;
    size_t nb;
    blocks = md_blocks(&b->md, &nb);
    int row = 0;
    int drawn = 0;
    for (size_t bi = 0; bi < nb && drawn < maxrows; bi++) {
        const MdBlock *blk = &blocks[bi];
        for (size_t ri = 0; ri < blk->nrows && drawn < maxrows; ri++, row++) {
            if (row < skip) continue;
            const MdRow *r = &blk->rows[ri];
            int yy = y + drawn;
            int cx = x;
            if (prefix) {
                const char *pfx = row == 0 ? prefix : "  ";
                grid_puts(g, x, yy, b->base_attrs | A_BOLD, row == 0 ? prefix_fg : TH_MUTED,
                          TH_NO_BG, pfx);
                cx += (int)agentc_strlen(pfx);
            }
            for (size_t k = 0; k < r->nruns; k++) {
                const MdRun *run = &r->runs[k];
                u16 fg = run->fg == 0xFFFF ? b->base_fg : run->fg;
                cx = grid_put_clip(g, cx, yy, w - (cx - x), b->base_attrs | run->attrs, fg,
                                   TH_NO_BG, (const char *)r->line.p + run->start, run->len);
            }
            drawn++;
        }
    }
}

static void render_tool_block(const ChatBlock *b, Grid *g, const Theme *th, int x, int y,
                              int w, int skip, int maxrows, i64 now_ms, int spinner_frame) {
    (void)th;
    if (maxrows <= 0) return;
    u16 bg = tool_bg_slot(b);
    /* Paint the card's rows as a full-width band first. Text, including the
     * diff and status foreground tints, is drawn on top, so the band is the
     * base layer and no foreground can lose it. The trailing gap row is not
     * part of the card and stays on the terminal background (TH_NO_BG), which
     * also keeps two adjacent cards from merging into one band. */
    int content_rows = block_height(b) - 1;
    for (int r = skip; r < skip + maxrows && r < content_rows; r++)
        grid_fill(g, x, y + (r - skip), w, 0, TH_MUTED, bg, ' ');
    if (skip <= 0) {
        /* header */
        char head[512];
        const char *args = b->tool_args ? b->tool_args : "";
        size_t alen = agentc_strlen(args);
        if (alen > 48) alen = 48;
        /* agentc_snprintf returns the C99 would-be length, which can exceed the
         * buffer; agentc_snprintf_used returns the bytes actually written, so
         * `hn` stays within `head` for any tool name. */
        int n = agentc_snprintf_used(head, sizeof head, "[%s] ", b->tool_name ? b->tool_name : "?");
        size_t hn = (size_t)n;
        for (size_t i = 0; i < alen && hn + 1 < sizeof head; i++) {
            char c = args[i];
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            head[hn++] = c;
        }
        head[hn] = 0;
        u16 st_fg = b->is_error ? TH_ERR : TH_OK;
        char status[64];
        if (b->running) {
            static const char spin[] = "|/-\\";
            agentc_snprintf(status, sizeof status, "%c %lldms", spin[spinner_frame & 3],
                        (long long)(now_ms - b->started_ms));
            st_fg = TH_WARN;
        } else if (b->is_error) {
            agentc_snprintf(status, sizeof status, "err %lldms", (long long)b->duration_ms);
        } else {
            agentc_snprintf(status, sizeof status, "ok %lldms", (long long)b->duration_ms);
        }
        int sl = (int)agentc_strlen(status);
        int avail = w - 1;
        grid_put_clip(g, x, y, avail, 0, TH_TOOL, bg, head, hn);
        int hlen = (int)hn;
        if (hlen + 1 + sl <= avail)
            grid_put_clip(g, x + avail - sl, y, sl, 0, st_fg, bg, status, (size_t)sl);
        else if (hlen + 1 + sl <= w)
            grid_put_clip(g, x + hlen + 1, y, sl, 0, st_fg, bg, status, (size_t)sl);
    }
    int row = 1; /* header occupied row 0 */
    if (maxrows <= 1) return;
    const char *out = b->output.p ? (const char *)b->output.p : "";
    int total;
    Slice lines[10];
    int shown = tail_lines(out, b->output.len, lines, 10, &total);
    int collapsed = !b->expanded;
    int display = collapsed ? (shown > 3 ? 3 : shown) : shown;
    int extra = total - display;
    bool marker = collapsed && extra > 0;
    for (int i = 0; i < display; i++) {
        int li = shown - display + i;
        int block_row = row + (marker ? 1 : 0) + i;
        if (block_row < skip) continue;
        int yy = y + (block_row - skip);
        if (yy >= y + maxrows) break;
        const char *p = lines[li].p;
        size_t n = lines[li].n;
        u16 fg = diff_color(b, p, n);
        grid_put_clip(g, x + 1, yy, w - 1, 0, fg, bg, p, n);
    }
    if (marker) {
        int block_row = row;
        if (block_row >= skip) {
            int yy = y + (block_row - skip);
            if (yy < y + maxrows) {
                char m[64];
                agentc_snprintf(m, sizeof m, "... (+%d lines)", extra);
                grid_put_clip(g, x + 1, yy, w - 1, A_DIM, TH_MUTED, bg, m, agentc_strlen(m));
            }
        }
    }
}

int chat_block_height(const ChatBlock *b) { return block_height(b); }

int chat_total_height(const Chat *c) {
    int total = 0;
    for (size_t i = 0; i < c->n; i++) total += block_height(&c->blocks[i]);
    return total;
}

void chat_render_rows(Chat *c, Grid *g, const Theme *th, int w, int row0, int rows,
                      i64 now_ms, int spinner_frame) {
    if (w < 2) w = 2;
    if (c->width != w) chat_set_width(c, w);
    int row = 0;
    for (size_t i = 0; i < c->n; i++) {
        ChatBlock *b = &c->blocks[i];
        int bh = block_height(b);
        if (row + bh <= row0) {
            row += bh;
            continue;
        }
        if (row >= row0 + rows) break;
        int skip = row < row0 ? row0 - row : 0;
        int by = row < row0 ? 0 : row - row0;
        int maxrows = rows - by;
        switch (b->kind) {
        case CHAT_TOOL:
            render_tool_block(b, g, th, 0, by, w, skip, maxrows, now_ms, spinner_frame);
            break;
        case CHAT_USER:
            render_md_block(b, g, th, 0, by, w, skip, maxrows, TH_USER, "> ");
            break;
        case CHAT_THINK:
            render_md_block(b, g, th, 0, by, w, skip, maxrows, TH_THINKING, "~ ");
            break;
        default:
            render_md_block(b, g, th, 0, by, w, skip, maxrows, 0, NULL);
            break;
        }
        row += bh;
    }
}

void chat_render(Chat *c, Grid *g, const Theme *th, int x, int y, int w, int h,
                 i64 now_ms, int spinner_frame) {
    if (w < 2) w = 2;
    c->height = h;
    if (c->width != w) chat_set_width(c, w);
    int total = 0;
    for (size_t i = 0; i < c->n; i++) total += block_height(&c->blocks[i]);
    int max_scroll = total > h ? total - h : 0;
    /* Keep the reading position stable while new content arrives: when the user
     * has scrolled up, grow `scroll` by however much the transcript grew. */
    if (c->scroll > 0 && max_scroll > c->last_max)
        c->scroll += max_scroll - c->last_max;
    c->last_max = max_scroll;
    if (c->scroll > max_scroll) c->scroll = max_scroll;
    if (c->scroll < 0) c->scroll = 0;
    int start = max_scroll - c->scroll;
    int row = 0;
    for (size_t i = 0; i < c->n && row < start + h; i++) {
        ChatBlock *b = &c->blocks[i];
        int bh = block_height(b);
        if (row + bh <= start) { row += bh; continue; }
        int skip = row > start ? 0 : start - row;
        int maxrows = h - (row > start ? row - start : 0);
        int by = row >= start ? y + (row - start) : y;
        switch (b->kind) {
        case CHAT_TOOL:
            render_tool_block(b, g, th, x, by, w, skip, maxrows, now_ms, spinner_frame);
            break;
        case CHAT_USER:
            render_md_block(b, g, th, x, by, w, skip, maxrows, TH_USER, "> ");
            break;
        case CHAT_THINK:
            render_md_block(b, g, th, x, by, w, skip, maxrows, TH_THINKING, "~ ");
            break;
        default:
            render_md_block(b, g, th, x, by, w, skip, maxrows, 0, NULL);
            break;
        }
        row += bh;
    }
}

/* ------------------------------------------------- footer / queue strip */

/* Map a provider style to the theme slot/attributes. Unknown bits never reach
 * here: the registry drops them. Style 0 is the muted status text. */
static u16 seg_fg(uint32_t style) {
    if (style & AGENTC_PSEG_STYLE_ACCENT) return TH_ACCENT;
    if (style & AGENTC_PSEG_STYLE_ERROR) return TH_ERR;
    if (style & AGENTC_PSEG_STYLE_WARN) return TH_WARN;
    if (style & AGENTC_PSEG_STYLE_OK) return TH_OK;
    return TH_MUTED;
}

static u16 seg_attrs(uint32_t style) {
    u16 attrs = 0;
    if (style & AGENTC_PSEG_STYLE_BOLD) attrs |= A_BOLD;
    if (style & AGENTC_PSEG_STYLE_DIM) attrs |= A_DIM;
    return attrs;
}

/* Columns the joined slot occupies, separators included. Display width, not
 * byte length: the right slot is positioned from the terminal's right edge, so
 * a non-ASCII segment would be misplaced by a byte count. */
static int slot_len(const AgcStatusValue *segs, size_t n, uint32_t slot) {
    int len = 0;
    bool first = true;
    for (size_t i = 0; i < n; i++) {
        if (segs[i].slot != slot || !segs[i].text[0]) continue;
        if (!first) len += 3;   /* " | " */
        len += agentc_text_width(segs[i].text);
        first = false;
    }
    return len;
}

/* Draws one slot's segments starting at x and clipped to [x, x+limit). */
static void draw_slot(Grid *g, int y, const AgcStatusValue *segs, size_t n,
                      uint32_t slot, int x, int limit) {
    int end = x + limit;
    bool first = true;
    for (size_t i = 0; i < n && x < end; i++) {
        if (segs[i].slot != slot || !segs[i].text[0]) continue;
        if (!first) {
            x = grid_put_clip(g, x, y, end - x, 0, TH_MUTED, TH_BG, " | ", 3);
        }
        x = grid_put_clip(g, x, y, end - x, seg_attrs(segs[i].style),
                          seg_fg(segs[i].style), TH_BG, segs[i].text,
                          agentc_strlen(segs[i].text));
        first = false;
    }
}

void comp_footer(Grid *g, const Theme *th, int y, int w, const AgcStatusValue *segs,
                 size_t n) {
    (void)th;
    /* The status line is one of two surfaces that own a theme-background band
     * (tool cards are the other); fill it explicitly instead of relying on the
     * grid's default canvas. */
    grid_fill(g, 0, y, w, 0, TH_MUTED, TH_BG, ' ');
    if (w <= 0) return;
    int rl = slot_len(segs, n, AGENTC_PSEG_SLOT_RIGHT);
    if (rl >= w) {
        /* Right slot alone owns the row; clip it so a chatty provider can never
         * wrap the line or move the status row. */
        draw_slot(g, y, segs, n, AGENTC_PSEG_SLOT_RIGHT, 0, w);
        return;
    }
    int left_limit = w - rl - 1;
    if (left_limit > 0)
        draw_slot(g, y, segs, n, AGENTC_PSEG_SLOT_LEFT, 0, left_limit);
    draw_slot(g, y, segs, n, AGENTC_PSEG_SLOT_RIGHT, w - rl, rl);
}

void comp_queue(Grid *g, const Theme *th, int y, int w, size_t nqueued, const char *first) {
    (void)th;
    char buf[512];
    if (first && first[0]) {
        char tmp[128];
        size_t n = agentc_strlen(first);
        if (n > 60) n = 60;
        agentc_memcpy(tmp, first, n);
        tmp[n] = 0;
        agentc_snprintf(buf, sizeof buf, "queued %llu: \"%s\" (esc aborts)", (unsigned long long)nqueued, tmp);
    } else {
        agentc_snprintf(buf, sizeof buf, "queued %llu (esc aborts)", (unsigned long long)nqueued);
    }
    grid_put_clip(g, 0, y, w - 1, 0, TH_WARN, TH_NO_BG, buf, agentc_strlen(buf));
}
