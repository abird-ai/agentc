/* editor.c — multiline editor: insertion, motions, kill ring, history ring,
 * bracketed-paste collapsing, and @file completion.
 *
 * TODO(m4+): one-level undo (Ctrl+Z), selection extensions, multi-line history
 * entries, and cycling through all @file matches.
 *
 * The buffer is a plain AgcBuf plus a byte cursor/anchor; multi-line history and
 * paste markers are stored as separate owned strings. Undo and true gap-buffer
 * semantics are TODO (the docs call for them; not needed by the M4 tests).
 */
#include "editor.h"
#include "plat.h"

#define PASTE_LINE_LIMIT 10
#define HISTORY_MAX 1000
#define HISTORY_FILE_MAX (16u << 20)
#define COMPLETE_MAX_ENTRIES 512

/* -------------------------------------------------------------- primitives */

static bool is_word_byte(u8 c) {
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c >= 0x80;
}

static size_t utf8_prev(const char *s, size_t pos) {
    while (pos > 0 && ((u8)s[pos - 1] & 0xC0) == 0x80) pos--;
    return pos > 0 ? pos - 1 : 0;
}

static size_t utf8_next(const char *s, size_t len, size_t pos) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((u8)s[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

static void line_bounds(const char *s, size_t len, size_t pos, size_t *ls, size_t *le) {
    size_t a = pos;
    while (a > 0 && s[a - 1] != '\n') a--;
    size_t b = pos;
    while (b < len && s[b] != '\n') b++;
    *ls = a;
    *le = b;
}

static void selection_bounds(const Editor *e, size_t *a, size_t *b) {
    if (e->cur < e->sel) { *a = e->cur; *b = e->sel; }
    else { *a = e->sel; *b = e->cur; }
}

/* Decode one UTF-8 codepoint at byte offset i (< len). Returns the number of
 * bytes consumed (>= 1) and stores the codepoint in *cp. An invalid lead byte,
 * a truncated sequence, or a bad continuation byte yields U+FFFD and consumes
 * one byte, exactly like render.c's utf8_decode()/grid_put(). Every pass that
 * measures or draws the buffer uses this helper, so the visual-row count and
 * the rendered rows can never disagree on malformed input. */
static size_t editor_decode(const char *s, size_t len, size_t i, u32 *cp) {
    u8 b = (u8)s[i];
    if (b < 0x80) { *cp = b; return 1; }
    size_t need;
    u32 v;
    if ((b & 0xE0) == 0xC0) { need = 2; v = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { need = 3; v = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { need = 4; v = b & 0x07; }
    else { *cp = 0xFFFD; return 1; }
    if (i + need > len) { *cp = 0xFFFD; return 1; }
    for (size_t j = 1; j < need; j++) {
        u8 c = (u8)s[i + j];
        if ((c & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (c & 0x3F);
    }
    *cp = v;
    return need;
}

/* Expand tabs to the next 4-column stop, the same stop grid_put() uses. The
 * editor's wrap/caret/clip math treats a tab as zero width (agentc_wcwidth),
 * so a raw tab in the buffer would misalign every column after it. Normalize
 * on entry instead: the editor never holds a tab and grid_put keeps its own
 * tab handling for other grid users. Columns count display width so a tab
 * after a wide glyph lands on the correct stop. */
static void expand_tabs(const char *p, size_t n, AgcBuf *out) {
    if (!p || n == 0) return;
    int col = 0;
    size_t i = 0;
    while (i < n) {
        u8 b = (u8)p[i];
        if (b == '\t') {
            int spaces = 4 - (col % 4);
            for (int k = 0; k < spaces; k++) agentc_buf_byte(out, ' ');
            col += spaces;
            i++;
            continue;
        }
        u32 cp;
        size_t k = editor_decode(p, n, i, &cp);
        for (size_t j = 0; j < k; j++) agentc_buf_byte(out, (u8)p[i + j]);
        i += k;
        if (cp == '\n') col = 0;
        else col += agentc_wcwidth(cp);
    }
}

static void set_text(Editor *e, const char *p, size_t n) {
    agentc_buf_clear(&e->text);
    AgcBuf norm = { 0 };
    expand_tabs(p, n, &norm);
    agentc_buf_push(&e->text, norm.p, norm.len);
    agentc_buf_free(&norm);
    e->cur = e->sel = e->text.len;
    e->goal_col = -1;
}

static void editor_delete_range(Editor *e, size_t a, size_t b) {
    if (a > b) { size_t t = a; a = b; b = t; }
    if (b > e->text.len) b = e->text.len;
    if (a >= b) return;
    agentc_memmove(e->text.p + a, e->text.p + b, e->text.len - b);
    e->text.len -= b - a;
    if (e->text.p) e->text.p[e->text.len] = 0;
    e->cur = e->sel = a;
}

static void insert_bytes(Editor *e, const char *p, size_t n) {
    if (e->sel != e->cur) {
        size_t a, b;
        selection_bounds(e, &a, &b);
        editor_delete_range(e, a, b);
    }
    (void)agentc_buf_reserve(&e->text, n);
    u8 *base = e->text.p;
    agentc_memmove(base + e->cur + n, base + e->cur, e->text.len - e->cur);
    agentc_memcpy(base + e->cur, p, n);
    e->text.len += n;
    e->cur += n;
    e->sel = e->cur;
    e->text.p[e->text.len] = 0;
    e->goal_col = -1;
}

static void insert_cp(Editor *e, u32 cp) {
    u8 tmp[4];
    size_t n = agentc_utf8_encode(cp, tmp);
    insert_bytes(e, (const char *)tmp, n);
}

static void copy_to_kill(Editor *e, size_t a, size_t b) {
    agentc_buf_clear(&e->kill);
    if (b > a) agentc_buf_push(&e->kill, e->text.p + a, b - a);
}

/* ------------------------------------------------------------------ motion */

static void move_left(Editor *e) {
    e->goal_col = -1;
    if (e->sel != e->cur) {
        size_t a, b;
        selection_bounds(e, &a, &b);
        e->cur = e->sel = a;
        return;
    }
    if (e->cur > 0) e->cur = e->sel = utf8_prev((const char *)e->text.p, e->cur);
}

static void move_right(Editor *e) {
    e->goal_col = -1;
    if (e->sel != e->cur) {
        size_t a, b;
        selection_bounds(e, &a, &b);
        e->cur = e->sel = b;
        return;
    }
    if (e->cur < e->text.len)
        e->cur = e->sel = utf8_next((const char *)e->text.p, e->text.len, e->cur);
}

static void word_left(Editor *e) {
    const char *s = (const char *)e->text.p;
    size_t pos = e->cur;
    if (e->sel != e->cur) {
        size_t a, b;
        selection_bounds(e, &a, &b);
        e->cur = e->sel = a;
        return;
    }
    while (pos > 0 && !is_word_byte((u8)s[utf8_prev(s, pos)])) pos = utf8_prev(s, pos);
    while (pos > 0 && is_word_byte((u8)s[utf8_prev(s, pos)])) pos = utf8_prev(s, pos);
    e->cur = e->sel = pos;
    e->goal_col = -1;
}

static void word_right(Editor *e) {
    const char *s = (const char *)e->text.p;
    size_t pos = e->cur;
    if (e->sel != e->cur) {
        size_t a, b;
        selection_bounds(e, &a, &b);
        e->cur = e->sel = b;
        return;
    }
    while (pos < e->text.len && !is_word_byte((u8)s[pos])) pos = utf8_next(s, e->text.len, pos);
    while (pos < e->text.len && is_word_byte((u8)s[pos])) pos = utf8_next(s, e->text.len, pos);
    e->cur = e->sel = pos;
    e->goal_col = -1;
}

static void move_home(Editor *e, bool absolute) {
    if (absolute) e->cur = e->sel = 0;
    else {
        size_t ls, le;
        line_bounds((const char *)e->text.p, e->text.len, e->cur, &ls, &le);
        e->cur = e->sel = ls;
    }
    e->goal_col = -1;
}

static void move_end(Editor *e, bool absolute) {
    if (absolute) e->cur = e->sel = e->text.len;
    else {
        size_t ls, le;
        line_bounds((const char *)e->text.p, e->text.len, e->cur, &ls, &le);
        e->cur = e->sel = le;
    }
    e->goal_col = -1;
}

/* Byte column `col` on the line [ls, le) snapped to a codepoint boundary. The
 * caret is byte-indexed, so a goal column from a shorter ASCII line can land
 * inside a multi-byte codepoint on a CJK line; landing there would hide the
 * caret and let the next insert split the codepoint. Past the line end the
 * position clamps to `le`; inside a sequence it advances over the continuation
 * bytes to the next lead byte (never past `le`), which keeps the caret on a
 * grapheme boundary and matches the display column the goal column meant. */
static size_t line_col_boundary(const char *s, size_t ls, size_t le, int col) {
    if (col < 0) col = 0;
    size_t pos = ls + (size_t)col;
    if (pos > le) pos = le;
    while (pos < le && ((u8)s[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

static void move_vertical(Editor *e, int dir) {
    const char *s = (const char *)e->text.p;
    size_t len = e->text.len;
    size_t ls, le;
    line_bounds(s, len, e->cur, &ls, &le);
    int col = e->goal_col >= 0 ? e->goal_col : (int)(e->cur - ls);
    if (e->goal_col < 0) e->goal_col = col;
    if (dir < 0) {
        if (ls == 0) { e->goal_col = -1; return; }
        size_t pls, ple;
        line_bounds(s, len, ls - 1, &pls, &ple);
        e->cur = e->sel = line_col_boundary(s, pls, ple, col);
    } else {
        if (le >= len) { e->goal_col = -1; return; }
        size_t nls, nle;
        line_bounds(s, len, le + 1, &nls, &nle);
        e->cur = e->sel = line_col_boundary(s, nls, nle, col);
    }
}

/* --------------------------------------------------------------- kill ring */

static void kill_to_end(Editor *e) {
    if (e->sel != e->cur) {
        size_t a, b;
        selection_bounds(e, &a, &b);
        copy_to_kill(e, a, b);
        editor_delete_range(e, a, b);
        return;
    }
    size_t ls, le;
    line_bounds((const char *)e->text.p, e->text.len, e->cur, &ls, &le);
    size_t end = le;
    if (le == e->cur && le < e->text.len) end = le + 1; /* eat the newline too */
    copy_to_kill(e, e->cur, end);
    editor_delete_range(e, e->cur, end);
}

static void kill_to_start(Editor *e) {
    if (e->sel != e->cur) {
        size_t a, b;
        selection_bounds(e, &a, &b);
        copy_to_kill(e, a, b);
        editor_delete_range(e, a, b);
        return;
    }
    size_t ls, le;
    line_bounds((const char *)e->text.p, e->text.len, e->cur, &ls, &le);
    copy_to_kill(e, ls, e->cur);
    editor_delete_range(e, ls, e->cur);
}

/* Kill the word before the caret into the kill buffer (Ctrl+W,
 * Alt+Backspace). Same word rule as word_left/word_right: a word is a run of
 * [A-Za-z0-9_] plus any non-ASCII byte, so punctuation and whitespace delimit.
 */
static void kill_word(Editor *e) {
    if (e->sel != e->cur) {
        size_t a, b;
        selection_bounds(e, &a, &b);
        copy_to_kill(e, a, b);
        editor_delete_range(e, a, b);
        return;
    }
    const char *s = (const char *)e->text.p;
    size_t start = e->cur;
    while (start > 0 && !is_word_byte((u8)s[utf8_prev(s, start)]))
        start = utf8_prev(s, start);
    while (start > 0 && is_word_byte((u8)s[utf8_prev(s, start)]))
        start = utf8_prev(s, start);
    copy_to_kill(e, start, e->cur);
    editor_delete_range(e, start, e->cur);
}

/* Kill the word after the caret into the kill buffer (Alt+D, Ctrl+Delete).
 * Non-word bytes before the word are skipped first, matching readline's
 * kill-word, so deleting between words still removes the next word. */
static void kill_word_forward(Editor *e) {
    if (e->sel != e->cur) {
        size_t a, b;
        selection_bounds(e, &a, &b);
        copy_to_kill(e, a, b);
        editor_delete_range(e, a, b);
        return;
    }
    const char *s = (const char *)e->text.p;
    size_t start = e->cur, end = start;
    while (end < e->text.len && !is_word_byte((u8)s[end]))
        end = utf8_next(s, e->text.len, end);
    while (end < e->text.len && is_word_byte((u8)s[end]))
        end = utf8_next(s, e->text.len, end);
    copy_to_kill(e, start, end);
    editor_delete_range(e, start, end);
}

static void delete_forward(Editor *e) {
    if (e->sel != e->cur) {
        size_t a, b;
        selection_bounds(e, &a, &b);
        editor_delete_range(e, a, b);
    } else if (e->cur < e->text.len) {
        size_t p = utf8_next((const char *)e->text.p, e->text.len, e->cur);
        editor_delete_range(e, e->cur, p);
    }
}

/* Emacs transpose-chars (Ctrl+T): swap the codepoints around the caret and
 * move past the pair; at the end of the input, swap the two before the caret
 * instead. Whole UTF-8 sequences are moved, never bytes. */
static void transpose_chars(Editor *e) {
    if (e->sel != e->cur || e->cur == 0) return;
    const char *s = (const char *)e->text.p;
    size_t len = e->text.len;
    size_t a, b, c;
    if (e->cur == len) {
        c = e->cur;
        b = utf8_prev(s, c);
        if (b == 0) return;   /* only one codepoint: nothing to swap */
        a = utf8_prev(s, b);
    } else {
        a = utf8_prev(s, e->cur);
        b = e->cur;
        c = utf8_next(s, len, e->cur);
    }
    u8 pair[8];
    size_t n1 = b - a, n2 = c - b;
    if (n1 + n2 > sizeof pair) return;
    agentc_memcpy(pair, s + a, n1);
    agentc_memcpy(pair + n1, s + b, n2);
    agentc_memcpy(e->text.p + a, pair + n1, n2);
    agentc_memcpy(e->text.p + a + n2, pair, n1);
    e->cur = e->sel = a + n1 + n2;
    e->goal_col = -1;
}

static void yank(Editor *e) {
    if (e->kill.len) insert_bytes(e, (const char *)e->kill.p, e->kill.len);
}

/* -------------------------------------------------------------- paste ring */

static void paste_insert(Editor *e, const u8 *p, size_t n) {
    /* normalize CRLF/CR to LF while counting lines */
    AgcBuf norm = { 0 };
    size_t lines = 1;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\r') {
            agentc_buf_byte(&norm, '\n');
            lines++;
            if (i + 1 < n && p[i + 1] == '\n') i++;
        } else {
            agentc_buf_byte(&norm, p[i]);
            if (p[i] == '\n') lines++;
        }
    }
    /* Normalize tabs before the text enters the editor (or a stored paste
     * body): the wrap/caret math sees spaces of equal width, matching what
     * grid_put draws. */
    AgcBuf exp = { 0 };
    expand_tabs((const char *)norm.p, norm.len, &exp);
    if (lines > PASTE_LINE_LIMIT) {
        char *body = agentc_strdup_len((const char *)exp.p, exp.len);
        agentc_vec_push(&e->pastes, sizeof(char *));
        ((char **)e->pastes.p)[e->pastes.len - 1] = body;
        e->paste_count++;
        char marker[64];
        int m = agentc_snprintf_used(marker, sizeof marker, "[Pasted text #%d +%llu lines]",
                            e->paste_count, (unsigned long long)lines);
        insert_bytes(e, marker, (size_t)m);
    } else {
        insert_bytes(e, (const char *)exp.p, exp.len);
    }
    agentc_buf_free(&exp);
    agentc_buf_free(&norm);
}

/* Expand `[Pasted text #N +M lines]` markers into the paste bodies. */
static void append_expanded(Editor *e, const char *p, size_t n, AgcBuf *out) {
    size_t i = 0;
    while (i < n) {
        if (p[i] == '[' && n - i > 14 && agentc_memeq(p + i, "[Pasted text #", 14)) {
            size_t j = i + 14;
            int idx = 0;
            while (j < n && p[j] >= '0' && p[j] <= '9') {
                /* Clamp while accumulating: a long digit run must not overflow
                 * the signed int (it stays well past any real paste index). */
                if (idx < (1 << 20)) idx = idx * 10 + (p[j] - '0');
                j++;
            }
            if (j < n && p[j] == ' ' && idx >= 1 && (size_t)idx <= e->pastes.len) {
                while (j < n && p[j] != ']') j++;
                if (j < n) {
                    const char *body = ((char **)e->pastes.p)[idx - 1];
                    agentc_buf_push(out, body, agentc_strlen(body));
                    i = j + 1;
                    continue;
                }
            }
        }
        agentc_buf_byte(out, (u8)p[i++]);
    }
}

/* ---------------------------------------------------------------- history */

static bool has_newline(const Editor *e) {
    return e->text.len && agentc_str_find((const char *)e->text.p, e->text.len, "\n", 1) >= 0;
}

static void history_browse(Editor *e, int dir) {
    if (e->hist.len == 0) return;
    if (!e->hist_browsing) {
        e->hist_browsing = true;
        agentc_buf_clear(&e->live);
        agentc_buf_push(&e->live, e->text.p, e->text.len);
        e->hist_pos = e->hist.len;
    }
    if (dir < 0) {
        if (e->hist_pos == 0) return;
        e->hist_pos--;
    } else {
        if (e->hist_pos + 1 < e->hist.len) e->hist_pos++;
        else {
            /* back to the live buffer */
            set_text(e, (const char *)e->live.p, e->live.len);
            e->hist_browsing = false;
            return;
        }
    }
    const char *h = ((char **)e->hist.p)[e->hist_pos];
    set_text(e, h, agentc_strlen(h));
}

void editor_history_load(Editor *e, const char *path) {
    if (!path) return;
    int fd = os_open(path, OS_O_RDONLY, 0);
    if (fd < 0) return;
    AgcBuf b = { 0 };
    char tmp[4096];
    for (;;) {
        if (b.len > HISTORY_FILE_MAX) {
            os_close(fd);
            agentc_buf_free(&b);
            return;
        }
        int n = os_read(fd, tmp, sizeof tmp);
        if (n <= 0) break;
        agentc_buf_push(&b, tmp, (size_t)n);
    }
    os_close(fd);
    size_t start = 0;
    for (size_t i = 0; i <= b.len; i++) {
        if (i == b.len || b.p[i] == '\n') {
            size_t n = i - start;
            /* A CRLF history file leaves a trailing '\r' on every line; strip
             * one so it cannot survive into the composer or the submission. */
            if (n > 0 && b.p[start + n - 1] == '\r') n--;
            if (n > 0 && e->hist.len < HISTORY_MAX) {
                char *line = agentc_strdup_len((const char *)b.p + start, n);
                /* dedupe consecutive entries */
                if (e->hist.len == 0 ||
                    !agentc_streq(((char **)e->hist.p)[e->hist.len - 1], line)) {
                    agentc_vec_push(&e->hist, sizeof(char *));
                    ((char **)e->hist.p)[e->hist.len - 1] = line;
                } else {
                    agentc_free(line);
                }
            }
            start = i + 1;
        }
    }
    agentc_buf_free(&b);
    e->hist_pos = e->hist.len;
}

void editor_history_append(Editor *e, const char *path, const char *text) {
    if (!text || !text[0]) return;
    /* Multi-line submissions are not persisted (history is line based). */
    if (agentc_str_str(text, "\n")) return;
    if (e->hist.len == 0 || !agentc_streq(((char **)e->hist.p)[e->hist.len - 1], text)) {
        if (e->hist.len >= HISTORY_MAX) {
            agentc_free(((char **)e->hist.p)[0]);
            agentc_memmove(e->hist.p, (char **)e->hist.p + 1,
                       (e->hist.len - 1) * sizeof(char *));
            e->hist.len--;
        }
        char *copy = agentc_strdup(text);
        agentc_vec_push(&e->hist, sizeof(char *));
        ((char **)e->hist.p)[e->hist.len - 1] = copy;
    }
    e->hist_pos = e->hist.len;
    if (!path) return;
    int fd = os_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_APPEND, 0600);
    if (fd < 0) return;
    (void)os_write(fd, text, agentc_strlen(text));
    (void)os_write(fd, "\n", 1);
    os_close(fd);
}

/* ------------------------------------------------------------- completion */

struct agentc_dirent64 {
    u64 d_ino;
    i64 d_off;
    u16 d_reclen;
    u8 d_type;
    char d_name[];
};

static bool subseq_match(const char *cand, const char *needle) {
    size_t j = 0;
    for (size_t i = 0; cand[i]; i++) {
        char c = cand[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (needle[j] == c) {
            if (!needle[++j]) return true;
        }
    }
    return needle[j] == 0;
}

static bool prefix_match(const char *cand, const char *needle, size_t nlen) {
    for (size_t i = 0; i < nlen; i++) {
        char c = cand[i];
        if (!c) return false;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        char d = needle[i];
        if (d >= 'A' && d <= 'Z') d = (char)(d + 32);
        if (c != d) return false;
    }
    return true;
}

void editor_complete(Editor *e, const char *dir) {
    const char *s = (const char *)e->text.p;
    size_t len = e->text.len;
    if (e->cur > len) e->cur = len;
    size_t tok = e->cur;
    while (tok > 0 && s[tok - 1] != ' ' && s[tok - 1] != '\t' && s[tok - 1] != '\n') tok--;
    if (tok >= e->cur || s[tok] != '@') return;
    size_t name_start = tok + 1;
    size_t slash = name_start;
    size_t last_slash = (size_t)-1;
    for (size_t i = name_start; i < e->cur; i++)
        if (s[i] == '/') last_slash = i;
    if (last_slash != (size_t)-1) slash = last_slash + 1;
    char name_part[256];
    size_t nl = e->cur - slash;
    if (nl >= sizeof name_part) nl = sizeof name_part - 1;
    if (nl) agentc_memcpy(name_part, s + slash, nl);
    name_part[nl] = 0;

    char base[4096];
    if (!dir) {
        if (os_getcwd(base, sizeof base) <= 0) return;
        dir = base;
    }
    char full[4096];
    size_t dl = agentc_strlen(dir), pl = slash - name_start;
    if (dl + 1 + pl + 1 >= sizeof full) return;
    agentc_memcpy(full, dir, dl);
    size_t fp = dl;
    if (fp > 0 && full[fp - 1] != '/') full[fp++] = '/';
    if (pl) { agentc_memcpy(full + fp, s + name_start, pl); fp += pl; }
    full[fp] = 0;

    int fd = os_open(full, OS_O_RDONLY | OS_O_DIRECTORY, 0);
    if (fd < 0) return;
    char dbuf[8192];
    char best[512];
    bool best_is_dir = false;
    bool have_best = false;
    int best_score = 3;
    size_t entries = 0;
    for (;;) {
        int n = os_getdents(fd, dbuf, sizeof dbuf);
        if (n <= 0) break;
        for (int off = 0; off < n;) {
            struct agentc_dirent64 *d = (struct agentc_dirent64 *)(dbuf + off);
            if (d->d_reclen == 0) break;
            const char *nm = d->d_name;
            if (!(nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))) {
                int score = -1;
                if (prefix_match(nm, name_part, nl)) score = 0;
                else if (nl && subseq_match(nm, name_part)) score = 1;
                if (score >= 0 && entries < COMPLETE_MAX_ENTRIES) {
                    bool take = false;
                    if (!have_best) take = true;
                    else if (score < best_score) take = true;
                    else if (score == best_score) {
                        size_t bl = agentc_strlen(best), cl = agentc_strlen(nm);
                        if (cl < bl) take = true;
                        else if (cl == bl && agentc_streq(nm, best)) take = false;
                        else if (cl == bl) {
                            /* stable lexicographic choice */
                            for (size_t i = 0;; i++) {
                                if (nm[i] == best[i]) {
                                    if (!nm[i]) break;
                                    continue;
                                }
                                take = (u8)nm[i] < (u8)best[i];
                                break;
                            }
                        }
                    }
                    if (take) {
                        agentc_snprintf(best, sizeof best, "%s", nm);
                        best_is_dir = d->d_type == 4; /* DT_DIR */
                        best_score = score;
                        have_best = true;
                    }
                }
                entries++;
            }
            off += d->d_reclen;
        }
        if (entries >= COMPLETE_MAX_ENTRIES) break;
    }
    os_close(fd);
    if (!have_best) return;

    AgcBuf repl = { 0 };
    agentc_buf_byte(&repl, '@');
    if (pl) agentc_buf_push(&repl, s + name_start, pl);
    agentc_buf_cstr(&repl, best);
    if (best_is_dir) agentc_buf_byte(&repl, '/');

    /* replace [tok, cur) */
    size_t tail = e->cur;
    size_t removed = e->cur - tok;
    (void)agentc_buf_reserve(&e->text, repl.len);
    u8 *buf = e->text.p;
    agentc_memmove(buf + tok + repl.len, buf + tail, e->text.len - tail);
    agentc_memcpy(buf + tok, repl.p, repl.len);
    e->text.len = e->text.len - removed + repl.len;
    e->cur = e->sel = tok + repl.len;
    e->text.p[e->text.len] = 0;
    e->goal_col = -1;
    agentc_buf_free(&repl);
}

/* ------------------------------------------------------------------ public */

void editor_init(Editor *e) {
    agentc_memset(e, 0, sizeof *e);
    e->goal_col = -1;
}

void editor_free(Editor *e) {
    agentc_buf_free(&e->text);
    agentc_buf_free(&e->kill);
    agentc_buf_free(&e->live);
    for (size_t i = 0; i < e->hist.len; i++) agentc_free(((char **)e->hist.p)[i]);
    agentc_vec_free(&e->hist);
    for (size_t i = 0; i < e->pastes.len; i++) agentc_free(((char **)e->pastes.p)[i]);
    agentc_vec_free(&e->pastes);
}

/* Drop every collapsed paste body: once the text is cleared the markers are
 * gone and the bodies are unreachable. Reset the marker counter with the
 * vector, or the next paste would emit an index past the fresh vector and
 * expand to a literal marker instead of its body. */
static void editor_pastes_clear(Editor *e) {
    for (size_t i = 0; i < e->pastes.len; i++) agentc_free(((char **)e->pastes.p)[i]);
    agentc_vec_free(&e->pastes);
    e->paste_count = 0;
}

void editor_clear(Editor *e) {
    agentc_buf_clear(&e->text);
    editor_pastes_clear(e);
    e->cur = e->sel = 0;
    e->goal_col = -1;
    e->hist_browsing = false;
}

void editor_set(Editor *e, const char *s) { set_text(e, s, s ? agentc_strlen(s) : 0); }

const char *editor_text(const Editor *e) {
    return e->text.p ? (const char *)e->text.p : "";
}

bool editor_empty(const Editor *e) { return e->text.len == 0; }

char *editor_take(Editor *e) {
    AgcBuf out = { 0 };
    append_expanded(e, (const char *)e->text.p, e->text.len, &out);
    editor_clear(e);
    if (!out.p) return agentc_strdup("");
    return (char *)out.p;
}

int editor_key(Editor *e, const Key *k) {
    switch (k->code) {
    case K_CHAR:
        /* Readline/emacs control and Alt bindings. Ctrl+A/E are line ends,
         * Ctrl+B/F move by codepoint like the plain arrow keys, and Alt+B/F
         * move by word like Ctrl+arrows do. */
        if (k->mods & MOD_CTRL) {
            switch (k->cp) {
            case 'a': move_home(e, false); return 0;
            case 'b': move_left(e); return 0;
            case 'd': delete_forward(e); return 0;
            case 'e': move_end(e, false); return 0;
            case 'f': move_right(e); return 0;
            case 'k': kill_to_end(e); return 0;
            case 't': transpose_chars(e); return 0;
            case 'u': kill_to_start(e); return 0;
            case 'w': kill_word(e); return 0;
            case 'y': yank(e); return 0;
            default: return 0;
            }
        }
        if (k->mods & MOD_ALT) {
            switch (k->cp) {
            case 'b': word_left(e); return 0;
            case 'd': kill_word_forward(e); return 0;
            case 'f': word_right(e); return 0;
            /* Alt+Backspace as kitty-protocol CSI 127;3u. The legacy ESC DEL
             * form is a K_BACKSPACE with MOD_ALT, handled below. */
            case 8: case 127: kill_word(e); return 0;
            default: break;
            }
        }
        insert_cp(e, k->cp);
        return 0;
    case K_ENTER:
        if (k->mods & MOD_ALT) {
            insert_bytes(e, "\n", 1);
            return 0;
        }
        return 1;
    case K_TAB:
        editor_complete(e, NULL);
        return 0;
    case K_PASTE:
        paste_insert(e, k->paste, k->paste_len);
        return 0;
    case K_BACKSPACE:
        if (k->mods & MOD_ALT) { kill_word(e); return 0; }
        if (e->sel != e->cur) {
            size_t a, b;
            selection_bounds(e, &a, &b);
            editor_delete_range(e, a, b);
        } else if (e->cur > 0) {
            size_t p = utf8_prev((const char *)e->text.p, e->cur);
            editor_delete_range(e, p, e->cur);
        }
        return 0;
    case K_DELETE:
        if (k->mods & MOD_CTRL) { kill_word_forward(e); return 0; }
        delete_forward(e);
        return 0;
    case K_LEFT:
        if (k->mods & (MOD_ALT | MOD_CTRL)) word_left(e);
        else move_left(e);
        return 0;
    case K_RIGHT:
        if (k->mods & (MOD_ALT | MOD_CTRL)) word_right(e);
        else move_right(e);
        return 0;
    case K_UP:
        if (!has_newline(e) && e->hist.len) history_browse(e, -1);
        else move_vertical(e, -1);
        return 0;
    case K_DOWN:
        if (!has_newline(e) && e->hist.len) history_browse(e, 1);
        else move_vertical(e, 1);
        return 0;
    case K_HOME: move_home(e, (k->mods & MOD_CTRL) != 0); return 0;
    case K_END: move_end(e, (k->mods & MOD_CTRL) != 0); return 0;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- renderer */

/* One composer rule row: a subdued full-width horizontal line. Dim + muted
 * keeps it out of the way; no background or reverse video, unlike the old
 * accent-coloured `> ` marker this replaces. The background is left to the
 * terminal (TH_NO_BG), the same as transcript and input rows. */
static void composer_rule(Grid *g, int x, int y, int w, bool ascii) {
    grid_hline(g, x, y, w, A_DIM, TH_MUTED, TH_NO_BG, ascii ? '-' : 0x2500);
}

/* Total rows the composer needs at the given width, rules included. The text
 * area reserves the last column so the cursor after the last character never
 * forces a wrap onto a fresh line. */
int editor_visual_rows(const Editor *e, int width) {
    const char *s = (const char *)e->text.p;
    size_t len = e->text.len;
    int inner = width - 1;
    if (inner < 1) inner = 1;
    int rows = 1;
    int col = 0;
    size_t i = 0;
    while (i < len) {
        u32 cp;
        size_t k = editor_decode(s, len, i, &cp);
        i += k;
        if (cp == '\n') { rows++; col = 0; continue; }
        int cw = agentc_wcwidth(cp);
        if (col + cw > inner) { rows++; col = 0; }
        col += cw;
    }
    return rows + 2;   /* top rule + bottom rule */
}

/* Caret representation: the cell under the caret is drawn reverse-video (a
 * space when the caret is past the last character) and the TUI additionally
 * parks the real terminal cursor on that same cell. Both coordinates come from
 * the single (cursor_x, cursor_y) computed here, so they cannot disagree. The
 * reverse cell is the fallback: the in-memory test backend has no hardware
 * cursor, and a terminal that cannot show one still gets a visible caret. A
 * wide glyph reverses as a whole because the terminal applies the SGR to both
 * of its columns; a zero-width combining mark reverses the cell it attaches
 * to, and a caret on a newline parks at the end of the preceding row. */
void editor_render(Editor *e, Grid *g, const Theme *th, int x, int y, int w, int h,
                   const char *placeholder, int *cursor_x, int *cursor_y) {
    const char *s = (const char *)e->text.p;
    size_t len = e->text.len;
    int inner = w - 1;
    if (inner < 1) inner = 1;
    if (h < 1) h = 1;

    /* The input is bracketed by rules when there is room; on a one/two-row
     * sliver the text row wins and the top rule (then the bottom) is dropped
     * rather than overlapping the input. */
    bool top = h >= 3;
    bool bottom = h >= 2;
    int input_h = h - (top ? 1 : 0) - (bottom ? 1 : 0);
    if (input_h < 1) input_h = 1;
    int input_y = y + (top ? 1 : 0);
    if (top) composer_rule(g, x, y, w, e->ascii);
    if (bottom) composer_rule(g, x, input_y + input_h, w, e->ascii);

    /* Placeholder: decoration on the empty input row. It is drawn before the
     * caret pass so the caret can reverse the first cell without losing the
     * glyph; clipped, never wrapped, so it cannot change the composer's row
     * count or reach the buffer. */
    if (placeholder && placeholder[0] && len == 0)
        grid_put_clip(g, x, input_y, inner, A_DIM, TH_MUTED, TH_NO_BG, placeholder,
                      agentc_strlen(placeholder));

    /* find the visual row of the cursor so we can scroll the viewport. The
     * render pass below applies the wrap of the codepoint AT the cursor before
     * placing the caret, so this pass must process that codepoint too; stopping
     * at e->cur left cur_row one row short when the caret sat on a wrap. */
    int cur_row = 0;
    {
        int col = 0;
        size_t i = 0;
        while (i < len) {
            u32 cp;
            size_t k = editor_decode(s, len, i, &cp);
            bool at_cursor = (i == e->cur);
            i += k;
            if (cp == '\n') {
                /* The render pass parks a caret on a newline at the row it ends,
                 * before the row advance; only advance for earlier codepoints. */
                if (at_cursor) break;
                cur_row++;
                col = 0;
                continue;
            }
            int cw = agentc_wcwidth(cp);
            if (col + cw > inner) { cur_row++; col = 0; }
            col += cw;
            if (at_cursor) break;
        }
    }
    int start = 0;
    if (cur_row >= input_h) start = cur_row - input_h + 1;

    int r = 0, col = 0;
    int cx = x, cy = input_y;
    bool cursor_set = false;
    for (size_t i = 0; i < len;) {
        u32 cp;
        size_t k = editor_decode(s, len, i, &cp);

        /* Wrap first, then place: placing before the wrap test dropped the
         * character that lands exactly on the boundary (and mismatched the
         * visual-row computation used for the cursor and scrolling). */
        int cw = (cp == '\n') ? 0 : agentc_wcwidth(cp);
        if ((size_t)col + (size_t)cw > inner) { r++; col = 0; }

        bool at_cursor = (i == e->cur);
        /* A zero-width combining mark attaches to the cell before it (or is
         * dropped at the start of a line), so the caret sits on that cell. */
        /* A newline has cw == 0 but is not a combining mark: its reverse cell
         * is placed at `col`, so it must not take the zero-width decrement. */
        int caret_col = (at_cursor && cw == 0 && col > 0 && cp != '\n') ? col - 1 : col;
        if (at_cursor && r >= start && r < start + input_h) {
            cx = x + caret_col;
            cy = input_y + (r - start);
            cursor_set = true;
        }
        if (r >= start && r < start + input_h) {
            int yy = input_y + (r - start);
            if (cp != '\n') {
                u16 attrs = at_cursor ? A_REVERSE : 0;
                grid_put(g, x + col, yy, attrs, TH_FG, TH_NO_BG, s + i, k);
                if (at_cursor && cw == 0) {
                    /* grid_put() attaches the mark to the previous cell, which
                     * the caret already points at; make sure it is reversed. */
                    Cell *pc = grid_at(g, x + caret_col, yy);
                    if (pc && pc->cp != CELL_CONT) pc->attrs |= A_REVERSE;
                }
            } else if (at_cursor) {
                /* Caret on the newline itself: park it at the end of the
                 * preceding visual row, as every other editor does. */
                Cell *c = grid_at(g, x + col, yy);
                if (c) {
                    c->attrs = A_REVERSE;
                    if (c->cp == CELL_INVALID || c->cp == CELL_CONT) {
                        c->cp = ' ';
                        c->comb = 0;
                    }
                    c->fg = TH_FG;
                    c->bg = TH_NO_BG;
                }
            }
        }
        i += k;
        if (cp == '\n') { r++; col = 0; continue; }
        col += cw;
    }
    if (e->cur >= len && r >= start && r < start + input_h) {
        cx = x + col;
        cy = input_y + (r - start);
        Cell *c = grid_at(g, cx, cy);
        if (c) {
            if (c->cp == CELL_INVALID || c->cp == CELL_CONT) {
                c->cp = ' ';
                c->comb = 0;
                c->fg = TH_FG;
                c->bg = TH_NO_BG;
                c->attrs = A_REVERSE;
            } else {
                /* The cell already holds a glyph (the empty-state placeholder):
                 * reverse it without resetting its dim/muted styling, so the
                 * hint's first character does not jump to the normal fg. */
                c->attrs |= A_REVERSE;
            }
        }
        cursor_set = true;
    }
    if (cursor_x) *cursor_x = cx;
    if (cursor_y) *cursor_y = cy;
    (void)cursor_set;
    (void)th;
}
