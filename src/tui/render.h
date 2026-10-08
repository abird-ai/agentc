/* render.h — cell grid and the differential frame writer. */
#ifndef AGENTC_TUI_RENDER_H
#define AGENTC_TUI_RENDER_H

#include "agentc.h"
#include "theme.h"

#define GRID_MAX_COLS 400
#define GRID_MAX_ROWS 200

/* Cell attributes. */
enum {
    A_BOLD = 1u << 0,
    A_DIM = 1u << 1,
    A_ITALIC = 1u << 2,
    A_UNDERLINE = 1u << 3,
    A_REVERSE = 1u << 4,
};

/* cp sentinels: CELL_CONT marks the second column of a wide glyph;
 * CELL_INVALID marks a cell that has never been drawn (forces a redraw). */
#define CELL_CONT 0xFFFFFFFFu
#define CELL_INVALID 0xFFFFFFFEu

typedef struct {
    u32 cp;
    u32 comb;      /* one combining mark attached to this cell (0 = none) */
    u16 attrs;
    u16 fg;        /* theme slot */
    u16 bg;        /* theme slot */
} Cell;

typedef struct {
    Cell *cells;
    int cols, rows;
} Grid;

void grid_init(Grid *g, int cols, int rows);
void grid_free(Grid *g);
/* Resize/reallocate; contents are cleared to CELL_INVALID. */
void grid_resize(Grid *g, int cols, int rows);
/* Clear to blanks with the given default attrs/slots. */
void grid_clear(Grid *g, u16 attrs, u16 fg, u16 bg);
void grid_invalidate(Grid *g);
Cell *grid_at(Grid *g, int x, int y);
void grid_fill(Grid *g, int x, int y, int w, u16 attrs, u16 fg, u16 bg, u32 cp);
void grid_hline(Grid *g, int x, int y, int w, u16 attrs, u16 fg, u16 bg, u32 cp);
/* Put UTF-8 text at (x,y); returns the next x (may pass cols when clipped).
 * Handles wide glyphs and combining marks; does not wrap on line width. */
int grid_put(Grid *g, int x, int y, u16 attrs, u16 fg, u16 bg, const char *s, size_t n);
int grid_put_clip(Grid *g, int x, int y, int maxw, u16 attrs, u16 fg, u16 bg,
                  const char *s, size_t n);
int grid_puts(Grid *g, int x, int y, u16 attrs, u16 fg, u16 bg, const char *s);
int grid_printf(Grid *g, int x, int y, int maxw, u16 attrs, u16 fg, u16 bg,
                const char *fmt, ...);

/* Serialize the difference prev->next into out. Returns the number of rows
 * that changed. Emits at most one cursor movement per row + SGR on attribute
 * changes + an SGR reset and \x1b[K to clear changed row tails. */
int render_diff(const Grid *prev, const Grid *next, AgcBuf *out, const Theme *th);

/* Emit rows [y0,y1) as ANSI lines, clearing each row first: "\r\x1b[2K" then
 * the styled content, rows separated (and optionally terminated) by "\r\n".
 * Used by the inline (scrollback) renderer; trailing blanks are trimmed. */
void render_rows_ansi(const Grid *g, int y0, int y1, AgcBuf *out, const Theme *th,
                      bool newline);
/* Plain text of one row, trailing blanks trimmed (no trailing newline). */
void render_row_text(const Grid *g, int y, AgcBuf *out);

/* Plain-text dump of the whole grid (one line per row, trailing blanks
 * trimmed). Used by tests and the future --dump mode. */
void render_screen_text(const Grid *g, AgcBuf *out);

int agentc_wcwidth(u32 cp);
bool agentc_is_combining(u32 cp);
/* Display width of UTF-8 text (sum of agentc_wcwidth over its codepoints). */
int agentc_text_width(const char *s);

#endif /* AGENTC_TUI_RENDER_H */
