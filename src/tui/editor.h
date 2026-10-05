/* editor.h — multiline prompt editor over an AgcBuf. */
#ifndef AGENTC_TUI_EDITOR_H
#define AGENTC_TUI_EDITOR_H

#include "agentc.h"
#include "input.h"
#include "render.h"
#include "theme.h"

/* Composer rows including its top and bottom rules (4 input rows max + 2). */
#define EDITOR_MAX_ROWS 6

typedef struct {
    AgcBuf text;
    size_t cur, sel;      /* sel == cur: no selection */
    AgcBuf kill;
    AgcVec hist;           /* char* entries, owned */
    size_t hist_pos;
    bool hist_browsing;
    AgcBuf live;           /* text saved when history browsing starts */
    int goal_col;
    AgcVec pastes;         /* char* collapsed paste bodies, owned */
    int paste_count;
    bool ascii;               /* draw the composer rules as '-' (term_ascii_only) */
} Editor;

void editor_init(Editor *e);
void editor_free(Editor *e);
void editor_clear(Editor *e);
void editor_set(Editor *e, const char *s);
const char *editor_text(const Editor *e);
bool editor_empty(const Editor *e);
/* Owned UTF-8 with paste markers expanded; clears the editor. */
char *editor_take(Editor *e);
/* Returns 1 when the user asked to submit (Enter), else 0. */
int editor_key(Editor *e, const Key *k);
void editor_complete(Editor *e, const char *dir);
void editor_history_load(Editor *e, const char *path);
void editor_history_append(Editor *e, const char *path, const char *text);
/* Redraw the composer. `placeholder` (NULL or empty for none) is decoration: it
 * is drawn, dim + muted on the terminal background, on the input row while the
 * buffer is empty. It never enters the buffer, the kill ring or the cursor
 * math, and it clips at the input area's right edge instead of wrapping, so it
 * can never add a row. */
void editor_render(Editor *e, Grid *g, const Theme *th, int x, int y, int w, int h,
                   const char *placeholder, int *cursor_x, int *cursor_y);
/* Total rows the composer occupies at the given width, rules included. */
int editor_visual_rows(const Editor *e, int width);

#endif /* AGENTC_TUI_EDITOR_H */
