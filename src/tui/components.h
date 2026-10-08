/* components.h — chat viewport, tool cards, diff view, footer, queue strip. */
#ifndef AGENTC_TUI_COMPONENTS_H
#define AGENTC_TUI_COMPONENTS_H

#include "agentc.h"
#include "markdown.h"
#include "render.h"
#include "status.h"
#include "theme.h"

enum {
    CHAT_USER = 0,
    CHAT_ASSISTANT,
    CHAT_THINK,
    CHAT_TOOL,
    CHAT_NOTICE,
};

typedef struct {
    int kind;
    Markdown md;          /* text blocks (user/assistant/think/notice) */
    u16 base_fg;
    u16 base_attrs;
    /* tool card */
    bool running, is_error, expanded;
    char *tool_name;
    char *tool_args;
    i64 started_ms, duration_ms;
    AgcBuf output;
    int tool_seq;
} ChatBlock;

typedef struct {
    ChatBlock *blocks;
    size_t n, cap;
    int width;
    int height;         /* last layout height (for PageUp/Down) */
    int scroll;         /* rows scrolled up from the sticky bottom */
    int last_max;       /* max_scroll from the previous frame (scroll anchoring) */
    bool seal_next;     /* the next append starts a new block (message boundary) */
} Chat;

void chat_init(Chat *c);
void chat_free(Chat *c);
void chat_set_width(Chat *c, int width);
ChatBlock *chat_push(Chat *c, int kind, u16 fg, u16 attrs);
ChatBlock *chat_last(Chat *c);
void chat_clear(Chat *c);
/* Drop blocks [n, c->n) and release everything they own, keeping `blocks` and
 * `cap` allocated. Used to roll back the partially streamed assistant message
 * when a retry replaces it (AGENTC_EV_MSG_RESET). No-op when n >= c->n. */
void chat_truncate(Chat *c, size_t n);
void chat_append_text(Chat *c, const char *p, size_t n);
void chat_append_think(Chat *c, const char *p, size_t n);
void chat_append_user(Chat *c, const char *p, size_t n);
void chat_append_notice(Chat *c, const char *p, size_t n);
/* Force the next append to open a new block even if the last block has the same
 * kind. Used at a message boundary so a retry can roll the whole message back
 * (a continuation turn would otherwise merge into the previous assistant block
 * and be unrecoverable by a block-aligned truncate). */
void chat_seal(Chat *c);
void chat_tool_start(Chat *c, const char *name, const char *args, i64 now_ms);
void chat_tool_end(Chat *c, const char *name, const char *result, bool is_error,
                   i64 duration_ms);
void chat_toggle_last_tool(Chat *c);
/* Index of the last CHAT_TOOL block, or c->n when there is none. */
size_t chat_last_tool_index(const Chat *c);
void chat_scroll(Chat *c, int delta);
void chat_scroll_bottom(Chat *c);
bool chat_has_tool(const Chat *c);
/* Rendered height of one block / the whole transcript at the current width. */
int chat_block_height(const ChatBlock *b);
int chat_total_height(const Chat *c);
/* Render transcript rows [row0, row0+rows) into g (height >= rows, width w).
 * The inline renderer uses it to commit finished rows to scrollback and to
 * compose only the live tail. */
void chat_render_rows(Chat *c, Grid *g, const Theme *th, int w, int row0, int rows,
                      i64 now_ms, int spinner_frame);
void chat_render(Chat *c, Grid *g, const Theme *th, int x, int y, int w, int h,
                 i64 now_ms, int spinner_frame);

/* Slash-command / model list: one row per entry, `prefix` (optional) then the
 * name then the description; the selected row is one full-width reverse band.
 * At most `maxrows` entries are drawn starting at `top`, so the inline renderer
 * can cap the list and scroll it instead of overflowing the frame. Chrome only:
 * terminal background, never a theme band. */
void comp_command_menu(Grid *g, const Theme *th, int x, int y, int w, int maxrows,
                       const char *const *names, const char *const *descs,
                       size_t n, size_t top, size_t sel, const char *prefix);

/* Status line: `segs` is the cached, sorted provider snapshot. comp_footer owns
 * separators, clipping and colour; providers only supply text and style. */
void comp_footer(Grid *g, const Theme *th, int y, int w, const AgcStatusValue *segs,
                 size_t n);
void comp_queue(Grid *g, const Theme *th, int y, int w, size_t nqueued, const char *first);

#endif /* AGENTC_TUI_COMPONENTS_H */
