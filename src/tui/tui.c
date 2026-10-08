/* tui.c — layout, input routing, slash commands and the main loop.
 *
 * The agent submit is synchronous; while a request is in flight agentc_http calls
 * our poll hook, which reads pending input, processes keys and redraws without
 * blocking. Tests drive the same state machine through the in-memory backend
 * (agentc_tui_test_* below).
 */
#include "tui.h"
#include "app/mode.h"
#include "app/setup.h"
#include "session.h"
#include "picklist.h"
#include "base/limits.h"
#include "core/prompt.h"
#include "core/prompts.h"
#include "core/tools/engine.h"
#include "ext.h"
#include "tui_test.h"
#include "components.h"
#include "editor.h"
#include "input.h"
#include "plat.h"
#include "render.h"
#include "term.h"
#include "theme.h"

/* Empty-composer placeholder. It keeps the hint's original trigger (see
 * tui_layout): shown only while the transcript is empty and no run is in
 * flight, because the sentence is about Ctrl-C quitting, which is only true
 * before anything has happened. Typing hides it because the renderer only draws
 * it while the buffer is empty. */
#define TUI_EMPTY_HINT "Ready. Press Ctrl-C once to clear, twice to exit"

/* Slash-command menu limits. The registry is small; the caps keep the popup
 * from ever pushing the inline live region past its row budget. */
#define TUI_MENU_MAX 128
#define TUI_MENU_VISIBLE_MAX 8

/* Which list the shared menu_* rows describe. The slash-command menu is derived
 * from the composer; the interactive pickers (TUI_MENU_MODEL for `/model`,
 * TUI_MENU_THINKING for `/thinking`, TUI_MENU_SESSION for `/resume`) own the
 * keyboard until a selection or Escape. */
enum {
    TUI_MENU_COMMAND = 0,
    TUI_MENU_MODEL = 1,
    TUI_MENU_THINKING = 2,
    TUI_MENU_SESSION = 3,
};

typedef struct {
    Terminal *term;
    AgcAgent *agent;
    const AgcTuiApp *app;     /* app-owned services (/new); may be NULL */
    void (*on_compact)(void *ud, const AgcCompactInfo *ci);
    void *on_compact_ud;
    bool test;
    bool show_tools;          /* banner advertises the resolved engines */
    i64 now_ms;
    i64 last_frame_ms;
    int spinner;

    Chat chat;
    Editor ed;
    Theme theme;
    Input in;

    Grid cur, prev;
    int mode;                 /* AGENTC_TUI_SCROLLBACK | INLINE | FULLSCREEN */
    Grid live;                /* composed live region (scrollback + inline) */
    Grid commit;              /* scratch for committing rows */
    size_t committed_rows;    /* transcript rows already printed to scrollback */
    size_t committed_blocks;  /* committed boundary: leading whole blocks ... */
    size_t committed_partial; /* ... plus this many rows of the next block */
    int live_rows;            /* owned region height (inline) / drawn rows (scrollback) */
    int live_cursor_off;      /* scrollback: caret distance from the region top */
    int live_cursor_y;        /* inline: kept as 0; the parked anchor is the region top */
    int live_w[GRID_MAX_ROWS];/* inline: content width of each owned row last frame */
    bool in_frame;            /* a frame is composing; a resize must not erase under it */
    bool resize_pending;      /* a resize landed mid-frame: discard and retry */
    int pending_cols, pending_rows;
    u64 geom_gen;             /* bumped on every accepted geometry change */
    AgcBuf scrollback;         /* plain mirror of committed lines (tests) */
    AgcBuf live_text;          /* plain dump of the live region (tests) */
    bool dirty, quit, running, abort_requested;
    i64 run_started_ms;
    i64 last_ctrl_c_ms;

    char **queue;
    size_t nq, qcap;
    AgcBuf tool_args;
    AgcBuf pend_text;      /* streamed deltas coalesced to one markdown append per frame */
    AgcBuf pend_think;
    int pend_order;       /* 0 none, 1 text first, 2 think first */
    size_t msg_mark;      /* chat.n when the current MSG_START opened its message */
    bool msg_mark_set;    /* a message is open: MSG_RESET may roll the chat back to msg_mark */

    bool footer_set;
    const char *model;
    const char *thinking;      /* transient "on"/"off" from delta events */
    int thinking_level;        /* configured level: 0=off,1=low,3=medium,4=high */
    u32 tok_in, tok_out;
    i64 cost_micro;

    /* Status snapshot cache: providers are re-queried only when the registry
     * version changes, never once per rendered frame. */
    u64 status_version;
    size_t status_n;
    AgcStatusValue status_segs[AGENTC_STATUS_MAX_SEGMENTS];

    char hist_path[4096];
    bool hist_set;

    int cursor_x, cursor_y;
    bool cursor_reversed;      /* the rendered fallback cell at (cursor_x, cursor_y) is A_REVERSE */

    /* Slash-command menu (see tui_menu_refresh). The entries are borrowed from
     * the built-in command table and the extension host registry, so only the
     * pointers live here. `menu_filter` detects when the typed word changed so
     * the selection resets and an Escape dismissal lapses. */
    bool menu_open, menu_dismissed;
    size_t menu_n, menu_sel, menu_top;
    char menu_filter[64];
    const char *menu_name[TUI_MENU_MAX];
    const char *menu_desc[TUI_MENU_MAX];

    /* Interactive pickers (`/model`, `/thinking`, `/resume`). `menu_kind`
     * selects whether the menu_* rows hold slash commands or picker rows; the
     * picker owns the keyboard while `pick_open` and `pick` is the one shared
     * selection list. Row names/descs/paths are owned here so no catalog or
     * session pointer can dangle. `switch_pending` defers a mid-run session
     * switch until the submit has unwound (tools and requests already
     * cancelled). */
    int menu_kind;
    bool pick_open;
    bool switch_pending;
    PickList pick;
    const char *pick_base_name[TUI_MENU_MAX];
    const char *pick_base_desc[TUI_MENU_MAX];
    char pick_name[TUI_MENU_MAX][48];
    char pick_desc[TUI_MENU_MAX][48];
    const char *pick_paths[TUI_MENU_MAX];
    size_t pick_paths_n;
} Tui;

/* --------------------------------------------------------------- helpers */

static i64 tui_now_ms(Tui *st) {
    if (st->test) return st->now_ms;
    return os_now_ns(OS_CLOCK_MONOTONIC) / 1000000;
}

static void tui_dirty(Tui *st) { st->dirty = true; }

/* Startup banner: dim+muted lines printed above the owned region so they land
 * in the terminal's scrollback like shell history and never take part in a
 * redraw. A second `tools: …` line names the resolved core-tool engines,
 * kept succinct (binary names only) so it rides with the version. */
static void tui_banner(Tui *st) {
    if (!st || !st->term) return;
    AgcBuf out = { 0 };
    theme_emit_sgr(&out, &st->theme, A_DIM, TH_MUTED, TH_NO_BG);
    agentc_buf_printf(&out, "agentc %s", AGENTC_VERSION);
    const char *tools = st->show_tools ? agentc_tool_engine_summary() : NULL;
    if (tools && tools[0]) agentc_buf_printf(&out, "\r\ntools: %s", tools);
    agentc_buf_cstr(&out, "\x1b[0m\r\n");
    term_write(st->term, out.p, out.len);
    agentc_buf_free(&out);
}

/* A fixed light/dark palette owns its background: push TH_BG to the terminal
 * (OSC 11) so a light theme is not dark text on a dark terminal, and clear it
 * for `system`/named themes that follow the terminal's own background. */
static void tui_sync_term_bg(Tui *st) {
    if (!st || !st->term || !term_is_tty(st->term)) return;
    if (st->theme.force_bg) term_set_bg(st->term, st->theme.rgb[TH_BG]);
    else term_reset_bg(st->term);
}

static void queue_push(Tui *st, const char *text) {
    if (st->nq == st->qcap) {
        size_t cap = st->qcap ? st->qcap * 2 : 4;
        st->queue = agentc_realloc(st->queue, cap * sizeof(char *));
        st->qcap = cap;
    }
    st->queue[st->nq++] = agentc_strdup(text ? text : "");
}

static char *queue_pop(Tui *st) {
    if (!st->nq) return NULL;
    char *p = st->queue[0];
    for (size_t i = 1; i < st->nq; i++) st->queue[i - 1] = st->queue[i];
    st->nq--;
    return p;
}

static void mkdir_p(const char *path) {
    char tmp[4096];
    size_t n = agentc_strlen(path);
    if (n >= sizeof tmp) return;
    agentc_memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            (void)os_mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
}

static void history_path(char *buf, size_t cap) {
    const char *xdg = os_getenv("XDG_STATE_HOME");
    if (xdg && xdg[0]) { agentc_snprintf(buf, cap, "%s/agentc/history", xdg); return; }
    const char *home = os_getenv("HOME");
    if (home && home[0]) {
        agentc_snprintf(buf, cap, "%s/.local/state/agentc/history", home);
        return;
    }
    buf[0] = 0;
}

/* ------------------------------------------------------------ submission */

static void tui_submit(Tui *st, char *text);
static void tui_accept(Tui *st);
static void scrollback_erase_live(Tui *st);
static void inline_erase_owned(Tui *st, int new_cols);
static void tui_menu_refresh(Tui *st);
static int tui_menu_height(Tui *st, int avail);
static void tui_menu_scroll(Tui *st, int menu_h);
static void tui_noticef(Tui *st, const char *fmt, const char *arg);
static void tui_pick_refresh(Tui *st);
static void tui_pick_close(Tui *st);
static bool tui_pick_key(Tui *st, const Key *k);
static bool tui_model_pick_open(Tui *st);
static bool tui_thinking_pick_open(Tui *st);
static bool tui_session_pick_open(Tui *st);
static void tui_after_session_swap(Tui *st);

/* Apply a geometry change: erase what the old geometry owned, then rebuild the
grids. Shared by the poll-time path and the mid-frame discard path so the two
cannot drift. */
/* Rows the committed boundary covers at the chat's current width. The terminal
 * reflows printed rows itself, so the count is recomputed from the block
 * boundary after a width change instead of keeping a row count from the old
 * layout (which would make the app reprint already-committed content). Both
 * inline and scrollback use this: scrollback commits whole rows (the live-tail
 * overflow), and the block decomposition keeps the boundary meaningful across a
 * resize. */
static size_t committed_rows_at(const Tui *st) {
    size_t rows = 0;
    for (size_t i = 0; i < st->committed_blocks && i < st->chat.n; i++)
        rows += (size_t)chat_block_height(&st->chat.blocks[i]);
    return rows + st->committed_partial;
}

/* Move the committed boundary to `rows` (a row index at the chat's current
 * width), decomposing it into whole blocks plus a partial next block. */
static void committed_set_rows(Tui *st, size_t rows) {
    size_t used = 0;
    size_t i = 0;
    while (i < st->chat.n) {
        int bh = chat_block_height(&st->chat.blocks[i]);
        if (used + (size_t)bh > rows) break;
        used += (size_t)bh;
        i++;
    }
    st->committed_blocks = i;
    if (i < st->chat.n) {
        int bh = chat_block_height(&st->chat.blocks[i]);
        size_t part = rows > used ? rows - used : 0;
        st->committed_partial = part > (size_t)bh ? (size_t)bh : part;
    } else {
        st->committed_partial = 0;
    }
    st->committed_rows = rows;
}

/* A transcript block is live (kept out of the permanent scrollback) while it can
 * still change: a running tool card, or a streaming assistant/thinking block of
 * an active run. Every block of a message whose MSG_END has not arrived is also
 * live, so a retry can roll the whole attempt back with chat_truncate() instead
 * of leaving an abandoned draft committed to the terminal. */
static bool block_is_live(const Tui *st, size_t i, const ChatBlock *b) {
    if (st->msg_mark_set && st->msg_mark <= st->chat.n && i >= st->msg_mark) return true;
    if (b->kind == CHAT_TOOL) return b->running;
    return (i + 1 == st->chat.n) && st->running &&
           (b->kind == CHAT_ASSISTANT || b->kind == CHAT_THINK);
}

static void tui_apply_resize(Tui *st, int c, int r) {
    /* Every render path clamps to GRID_MAX_COLS/ROWS; the resize path must too,
     * or the committed boundary, the grid and the live-region bookkeeping end up
     * sized past the cap and disagree. */
    if (c > GRID_MAX_COLS) c = GRID_MAX_COLS;
    if (r > GRID_MAX_ROWS) r = GRID_MAX_ROWS;
    if (st->mode == AGENTC_TUI_INLINE) {
        inline_erase_owned(st, c);
    } else if (st->mode == AGENTC_TUI_SCROLLBACK) {
        scrollback_erase_live(st);
    }
    grid_resize(&st->cur, c, r);
    grid_resize(&st->prev, c, r);
    grid_invalidate(&st->prev);
    chat_set_width(&st->chat, c);
    /* The block boundary is width-independent, so recompute the row count for
     * both modes after a width change: a stale scrollback count would skip or
     * reprint the reflowed rows. */
    if (st->mode == AGENTC_TUI_INLINE || st->mode == AGENTC_TUI_SCROLLBACK)
        st->committed_rows = committed_rows_at(st);
    st->live_rows = 0;
    st->live_cursor_off = 0;
    st->live_cursor_y = 0;
    tui_dirty(st);
}

static void tui_check_resize(Tui *st) {
    int c = 0, r = 0;
    if (!term_check_resize(st->term, &c, &r)) return;
    st->geom_gen++;
    if (st->in_frame) {
        /* The resize landed while a frame was being composed; its coordinates
         * are already stale. Remember the size, throw the frame away, and
         * apply the erase + re-anchor from the frame's discard path. */
        st->pending_cols = c;
        st->pending_rows = r;
        st->resize_pending = true;
        return;
    }
    tui_apply_resize(st, c, r);
}

/* Row budget shared by the inline and scrollback frames. The footer is one
 * mandatory row; the queue strip is one row when present; the composer takes
 * its visual rows; the command menu yields first and gets only what is left;
 * the transcript tail takes the remainder (zero is fine). `rows` has already
 * been clamped by the caller so this budget, the grid and the cursor moves all
 * agree. Keeping the computation in one place stops the two frames from
 * drifting (the scrollback copy used to reserve one row too many). */
typedef struct {
    int qrows;    /* queue strip rows */
    int eh;       /* composer rows, rules included */
    int menu_h;   /* command-menu rows (0 when closed) */
    int budget;   /* transcript rows the live region may use */
} TuiFrameBudget;

static TuiFrameBudget tui_frame_budget(Tui *st, int rows, int cols) {
    TuiFrameBudget b;
    b.qrows = st->nq ? 1 : 0;
    b.eh = editor_visual_rows(&st->ed, cols);
    if (b.eh > EDITOR_MAX_ROWS) b.eh = EDITOR_MAX_ROWS;
    if (b.eh < 1) b.eh = 1;
    if (rows <= 1) {
        b.eh = 0;
        b.qrows = 0;
    } else if (b.eh > rows - 1 - b.qrows) {
        b.eh = rows - 1 - b.qrows;
        if (b.eh < 1) b.eh = 1;
    }
    int menu_avail = rows - 1 - b.qrows - b.eh;
    if (menu_avail < 0) menu_avail = 0;
    b.menu_h = tui_menu_height(st, menu_avail);
    b.budget = rows - 1 - b.qrows - b.eh - b.menu_h;
    if (b.budget < 0) b.budget = 0;
    return b;
}

/* --------------------------------------------------------------- drawing */

static void tui_layout(Tui *st) {
    int cols = st->cur.cols;
    int rows = st->cur.rows;
    grid_clear(&st->cur, 0, TH_FG, TH_NO_BG);

    int footer_y = rows - 1;
    int queue_y = -1;
    if (st->nq) { queue_y = footer_y - 1; footer_y--; }
    int editor_h = editor_visual_rows(&st->ed, cols);
    if (editor_h > EDITOR_MAX_ROWS) editor_h = EDITOR_MAX_ROWS;
    if (editor_h < 1) editor_h = 1;
    int avail = rows - 1 - (st->nq ? 1 : 0);
    if (editor_h > avail - 1) editor_h = avail > 1 ? avail - 1 : 1;
    int editor_y = rows - 1 - (st->nq ? 1 : 0) - editor_h;
    if (editor_y < 1) editor_y = 1;

    tui_menu_refresh(st);
    int menu_h = tui_menu_height(st, editor_y);
    int chat_h = editor_y - menu_h;
    if (chat_h < 0) chat_h = 0;

    const char *placeholder = (st->chat.n == 0 && !st->running) ? TUI_EMPTY_HINT : NULL;
    int spinner = st->spinner;
    chat_render(&st->chat, &st->cur, &st->theme, 0, 0, cols, chat_h, tui_now_ms(st),
                spinner);
    if (menu_h > 0) {
        tui_menu_scroll(st, menu_h);
        comp_command_menu(&st->cur, &st->theme, 0, chat_h, cols, menu_h, st->menu_name,
                          st->menu_desc, st->menu_n, st->menu_top, st->menu_sel, st->menu_kind == TUI_MENU_COMMAND ? "/" : NULL);
    }
    editor_render(&st->ed, &st->cur, &st->theme, 0, editor_y, cols, editor_h,
                  placeholder, &st->cursor_x, &st->cursor_y);
    if (queue_y >= 0)
        comp_queue(&st->cur, &st->theme, queue_y, cols, st->nq, st->queue[0]);
    comp_footer(&st->cur, &st->theme, rows - 1, cols, st->status_segs, st->status_n);

    if (st->cursor_x >= cols) st->cursor_x = cols - 1;
    if (st->cursor_x < 0) st->cursor_x = 0;
    if (st->cursor_y >= rows) st->cursor_y = rows - 1;
    if (st->cursor_y < 0) st->cursor_y = 0;
    Cell *cc = grid_at(&st->cur, st->cursor_x, st->cursor_y);
    st->cursor_reversed = cc && (cc->attrs & A_REVERSE) != 0;
}

/* The configured (not transient) thinking level. The core accessor is the
 * source of truth for a live agent; the level tracked here keeps the status
 * line and /thinking report correct in the no-agent test harness. */
static const char *tui_thinking_name(const Tui *st) {
    if (st->agent) {
        const char *n = agentc_agent_thinking(st->agent);
        if (n) return n;
    }
    switch (st->thinking_level) {
    case 1:
    case 2: return "low";
    case 3: return "medium";
    case 4: return "high";
    default: return "off";
    }
}

/* Copies the TUI's footer state into the built-in provider's snapshot, then
 * rebuilds the segment cache only when the registry version changed. The
 * built-in state setter is a no-op when nothing rendered changed, so an idle
 * footer never re-runs a extension provider. */
static void tui_status_sync(Tui *st) {
    AgcStatusBuiltin s;
    agentc_memset(&s, 0, sizeof s);
    if (st->footer_set) {
        agentc_snprintf(s.model, sizeof s.model, "%s",
                        st->model && st->model[0] ? st->model : "?");
        agentc_snprintf(s.thinking, sizeof s.thinking, "%s", tui_thinking_name(st));
        s.tok_in = st->tok_in;
        s.tok_out = st->tok_out;
        s.cost_micro = st->cost_micro;
    } else {
        /* TODO(session): a session-name getter does not exist on AgcAgent; show
         * it here once one lands (only agentc_agent_transcript/last_error exist). */
        const AgcTranscript *tr = st->agent ? agentc_agent_transcript(st->agent) : NULL;
        agentc_snprintf(s.model, sizeof s.model, "%s",
                        tr && tr->model && tr->model[0] ? tr->model : "?");
        agentc_snprintf(s.thinking, sizeof s.thinking, "%s", tui_thinking_name(st));
        bool cost_known = true;
        i64 cost = 0;
        if (tr) {
            for (size_t i = 0; i < tr->n; i++) {
                const AgcMsg *m = &tr->msgs[i];
                s.tok_in += m->usage.input;
                s.tok_out += m->usage.output;
                if (m->usage.cost_micro < 0) cost_known = false;
                else cost += m->usage.cost_micro;
            }
        }
        s.cost_micro = cost_known ? cost : -1;
        /* No rate for the current model (an unrated/local one) means the
         * aggregate is unknown, not free: ask the pricing helper with a zero
         * usage and pass the -1 sentinel through so the segment is omitted. */
        if (s.cost_micro >= 0 && tr && tr->provider && tr->model) {
            const AgcModel *cm = agentc_model_find(tr->provider, tr->model);
            AgcUsage zero;
            agentc_memset(&zero, 0, sizeof zero);
            if (!cm || agentc_model_cost(cm, &zero) < 0) s.cost_micro = -1;
        }
    }
    s.running = st->running;
    s.spinner = st->spinner;
    s.elapsed_ms = st->running ? tui_now_ms(st) - st->run_started_ms : 0;
    agentc_status_builtin_set(&s);
    if (agentc_status_version() != st->status_version) {
        st->status_n = agentc_status_snapshot(st->status_segs, AGENTC_STATUS_MAX_SEGMENTS);
        st->status_version = agentc_status_version();
    }
}

static void tui_draw(Tui *st) {
    tui_status_sync(st);
    tui_layout(st);
}

static void tui_flush_pending(Tui *st) {
    /* arrival order matters: thinking normally precedes the answer */
    if (st->pend_order == 2) {
        if (st->pend_think.len) {
            chat_append_think(&st->chat, (const char *)st->pend_think.p, st->pend_think.len);
            agentc_buf_clear(&st->pend_think);
        }
        if (st->pend_text.len) {
            chat_append_text(&st->chat, (const char *)st->pend_text.p, st->pend_text.len);
            agentc_buf_clear(&st->pend_text);
        }
    } else {
        if (st->pend_text.len) {
            chat_append_text(&st->chat, (const char *)st->pend_text.p, st->pend_text.len);
            agentc_buf_clear(&st->pend_text);
        }
        if (st->pend_think.len) {
            chat_append_think(&st->chat, (const char *)st->pend_think.p, st->pend_think.len);
            agentc_buf_clear(&st->pend_think);
        }
    }
    st->pend_order = 0;
}

/* ------------------------------------------------------- scrollback mode
 *
 * Today's append-style renderer, kept as the explicit "scrollback" mode. The
 * transcript has a total rendered height T (at the current width). Rows
 * [0, committed_rows) have already been printed to the terminal's scrollback and
 * are never touched again; rows [committed_rows, T) are the live region, drawn
 * in place together with the queue strip, the editor and the footer.
 *
 * Every frame: finished blocks (all but the last) and any overflow beyond the
 * live budget are committed by printing them once; then the live region is
 * redrawn by moving the cursor up over the previous frame, clearing each row and
 * re-emitting it. The region is capped at rows-1 so a redraw can never scroll.
 */

/* Clear the live region and leave the cursor at its top-left. */
static void scrollback_erase_live(Tui *st) {
    if (st->live_rows <= 0) return;
    AgcBuf out = { 0 };
    agentc_buf_cstr(&out, "\x1b[?7l");
    if (st->live_cursor_off > 0) agentc_buf_printf(&out, "\x1b[%dA", st->live_cursor_off);
    for (int i = 0; i < st->live_rows; i++) {
        agentc_buf_cstr(&out, "\r\x1b[2K");
        if (i + 1 < st->live_rows) agentc_buf_cstr(&out, "\x1b[1B");
    }
    if (st->live_rows > 1) agentc_buf_printf(&out, "\x1b[%dA", st->live_rows - 1);
    agentc_buf_cstr(&out, "\x1b[?7h");
    term_write(st->term, out.p, out.len);
    agentc_buf_free(&out);
    st->live_rows = 0;
    st->live_cursor_off = 0;
}

/* Print transcript rows [from, upto) once, advancing the committed boundary as
 * each chunk lands. grid_resize() clamps the scratch grid to GRID_MAX_ROWS, so
 * a longer range must be split into bounded chunks; otherwise rows past the cap
 * would be rendered out of bounds and then marked committed without ever being
 * printed. The committed index moves only over rows actually emitted, so no row
 * is skipped and none is reprinted. */
static void scrollback_print_rows(Tui *st, size_t from, size_t upto) {
    if (upto <= from) return;
    int cols = term_cols(st->term);
    if (cols > GRID_MAX_COLS) cols = GRID_MAX_COLS;
    while (from < upto) {
        size_t chunk = upto - from;
        if (chunk > GRID_MAX_ROWS) chunk = GRID_MAX_ROWS;
        int n = (int)chunk;
        if (st->commit.cols != cols || st->commit.rows < n) grid_resize(&st->commit, cols, n);
        grid_clear(&st->commit, 0, TH_FG, TH_NO_BG);
        chat_render_rows(&st->chat, &st->commit, &st->theme, cols, (int)from, n,
                         tui_now_ms(st), st->spinner);
        AgcBuf out = { 0 };
        render_rows_ansi(&st->commit, 0, n, &out, &st->theme, true);
        term_write(st->term, out.p, out.len);
        agentc_buf_free(&out);
        if (st->test) {
            for (int y = 0; y < n; y++) {
                render_row_text(&st->commit, y, &st->scrollback);
                agentc_buf_byte(&st->scrollback, '\n');
            }
        }
        from += chunk;
        committed_set_rows(st, from);
    }
}

static void tui_scrollback_frame(Tui *st) {
    tui_flush_pending(st);
    int cols = term_cols(st->term);
    int rows = term_rows(st->term);
    if (cols < 10) cols = 10;
    if (cols > GRID_MAX_COLS) cols = GRID_MAX_COLS;
    if (rows < 4) rows = 4;
    if (rows > GRID_MAX_ROWS) rows = GRID_MAX_ROWS;
    /* block heights depend on the wrap width, so set it before measuring */
    if (st->chat.width != cols) chat_set_width(&st->chat, cols);
    tui_menu_refresh(st);

    /* The composer and footer are mandatory; the menu is chrome that yields
     * first. Measure the mandatory rows without a transcript, give the menu
     * only what is left, and let the chat tail take what remains (zero is
     * fine). A menu that cannot get even one row is dropped: a shorter list
     * beats a frame that scrolls. Scrollback draws in the normal buffer, so it
     * keeps one row in reserve (rows - 1) to be sure a redraw can never scroll. */
    TuiFrameBudget b = tui_frame_budget(st, rows - 1, cols);
    int budget = b.budget;

    size_t total = (size_t)chat_total_height(&st->chat);
    if (total < st->committed_rows) {
        /* the transcript was cleared or replaced: the old scrollback mirror is
         * no longer ours to manage */
        scrollback_erase_live(st);
        st->committed_rows = 0;
        st->committed_blocks = 0;
        st->committed_partial = 0;
        st->scrollback.len = 0;
    }

    /* A block is live only while it can still change: a running tool card, or a
     * streaming assistant/thinking block of an active run. Everything else is
     * printed to scrollback for good. */
    size_t keep_from = 0;
    for (size_t i = 0; i < st->chat.n; i++) {
        const ChatBlock *b = &st->chat.blocks[i];
        if (block_is_live(st, i, b)) break;
        keep_from += (size_t)chat_block_height(b);
    }
    /* Commit whole blocks only. A partially committed block would tie the
     * boundary to one width's row layout, and a resize would then reprint or
     * skip reflowed rows; whole blocks map exactly through chat_block_height()
     * at any width. The live tail beyond the boundary is displayed clipped to
     * the budget by chat_win. */
    size_t target = keep_from;
    if (target > st->committed_rows) {
        scrollback_erase_live(st);
        scrollback_print_rows(st, st->committed_rows, target);
    }

    /* compose the live region: chat tail + queue + editor + menu + footer */
    int chat_h = (int)(total - st->committed_rows);
    if (chat_h < 0) chat_h = 0;
    /* Record the viewport height for PageUp/PageDown and apply the same sticky
     * bottom + scroll offset as inline. The committed boundary is the floor, so
     * a fully committed transcript has nothing left to page over (the terminal
     * owns that history); an uncommitted tail can still be scrolled. */
    st->chat.height = chat_h;
    int max_scroll = chat_h > budget ? chat_h - budget : 0;
    /* Keep the reading position stable while streaming, mirroring chat_render():
     * when the user has scrolled up, grow `scroll` by the growth of the max
     * scrollable amount instead of letting the window top slide down. */
    if (st->chat.scroll > 0 && max_scroll > st->chat.last_max)
        st->chat.scroll += max_scroll - st->chat.last_max;
    st->chat.last_max = max_scroll;
    if (st->chat.scroll > max_scroll) st->chat.scroll = max_scroll;
    if (st->chat.scroll < 0) st->chat.scroll = 0;
    int chat_win = chat_h > budget ? budget : chat_h;
    int chat_off = (int)st->committed_rows;
    if (chat_h > budget) chat_off += chat_h - budget - st->chat.scroll;
    int h = chat_win + b.qrows + b.eh + 1 + b.menu_h;
    if (h < st->live_rows) scrollback_erase_live(st);   /* shrink: clear the old frame first */
    if (st->live.cols != cols || st->live.rows < h) grid_resize(&st->live, cols, h);
    grid_clear(&st->live, 0, TH_FG, TH_NO_BG);
    if (chat_win) {
        chat_render_rows(&st->chat, &st->live, &st->theme, cols,
                         chat_off, chat_win, tui_now_ms(st), st->spinner);
    }
    int y = chat_win;
    if (b.qrows) {
        comp_queue(&st->live, &st->theme, y, cols, st->nq, st->queue[0]);
        y++;
    }
    st->cursor_x = 0;
    st->cursor_y = y;
    const char *placeholder = (st->chat.n == 0 && !st->running) ? TUI_EMPTY_HINT : NULL;
    editor_render(&st->ed, &st->live, &st->theme, 0, y, cols, b.eh, placeholder,
                  &st->cursor_x, &st->cursor_y);
    y += b.eh;
    if (b.menu_h) {
        tui_menu_scroll(st, b.menu_h);
        comp_command_menu(&st->live, &st->theme, 0, y, cols, b.menu_h, st->menu_name,
                          st->menu_desc, st->menu_n, st->menu_top, st->menu_sel, st->menu_kind == TUI_MENU_COMMAND ? "/" : NULL);
        y += b.menu_h;
    }
    tui_status_sync(st);
    comp_footer(&st->live, &st->theme, y, cols, st->status_segs, st->status_n);
    if (st->cursor_y >= h) st->cursor_y = h - 1;
    if (st->cursor_y < 0) st->cursor_y = 0;
    if (st->cursor_x >= cols) st->cursor_x = cols - 1;
    if (st->cursor_x < 0) st->cursor_x = 0;
    /* Remember the rendered fallback so the test API can check that the
     * hardware cursor and the reverse cell agree on one position. */
    {
        Cell *cc = grid_at(&st->live, st->cursor_x, st->cursor_y);
        st->cursor_reversed = cc && (cc->attrs & A_REVERSE) != 0;
    }

    AgcBuf out = { 0 };
    /* Autowrap off: writing the last column of the last row would otherwise
     * leave the terminal in a pending-wrap state and the cursor math below
     * would land one row too low. The hardware cursor is hidden for the whole
     * repaint and parked on the caret at the end, in this same atomic write. */
    agentc_buf_cstr(&out, "\x1b[?25l\x1b[?7l");
    if (st->live_rows > 0 && st->live_cursor_off > 0)
        agentc_buf_printf(&out, "\x1b[%dA", st->live_cursor_off);
    render_rows_ansi(&st->live, 0, h, &out, &st->theme, false);
    /* the cursor is on the last row; move it up to the editor cursor row */
    int up = (h - 1) - st->cursor_y;
    if (up > 0) agentc_buf_printf(&out, "\x1b[%dA", up);
    agentc_buf_printf(&out, "\x1b[%dG", st->cursor_x + 1);
    agentc_buf_cstr(&out, "\x1b[?25h\x1b[?7h");
    term_write(st->term, out.p, out.len);
    agentc_buf_free(&out);
    st->live_rows = h;
    /* distance from the cursor row up to the top of the live region */
    st->live_cursor_off = st->cursor_y;
    if (st->live_cursor_off >= st->live_rows) st->live_cursor_off = st->live_rows - 1;
    if (st->live_cursor_off < 0) st->live_cursor_off = 0;
    if (st->test) {
        st->live_text.len = 0;
        if (st->live_text.p) st->live_text.p[0] = 0;
        for (int i = 0; i < h; i++) {
            render_row_text(&st->live, i, &st->live_text);
            agentc_buf_byte(&st->live_text, '\n');
        }
    }
}

/* Leaving scrollback mode: drop the live region, print whatever is left of the
 * conversation so the transcript stays in scrollback, and end on a fresh line. */
static void tui_scrollback_shutdown(Tui *st) {
    size_t total = (size_t)chat_total_height(&st->chat);
    scrollback_erase_live(st);
    if (total > st->committed_rows) scrollback_print_rows(st, st->committed_rows, total);
    term_write(st->term, "\r\n", 2);
}

/* ---------------------------------------------------------- inline mode
 *
 * The app owns a rectangular region that flows with the transcript instead of
 * being pinned to the terminal bottom. The hidden hardware cursor is parked on
 * the region's top-left between frames (live_cursor_off == 0), so a frame starts
 * at the region top. Finished transcript rows are printed there once with
 * newline mode, which pushes the region down; the region (live tail + queue +
 * editor + menu + footer) is then redrawn immediately below the printed rows
 * and the hidden cursor is parked back on the new region top. Every frame is a
 * single relative-anchored write, so while the region is on screen it grows
 * downward from just below the shell prompt; once it reaches the terminal bottom
 * the normal scroll takes over and rows leaving the top go into the terminal's
 * real scrollback. Nothing is ever printed with an absolute cursor move.
 *
 * Resize protocol. The parked region top is the one anchor a terminal keeps: on
 * a height shrink the owned rows sit below it, so the terminal discards or
 * scrolls them instead of pushing the app's chrome into the user's scrollback.
 * The app records each owned row's content width. On SIGWINCH the terminal has
 * already reflowed, so:
 *   - inline_erase_owned() clears downward from the parked region top over the
 *     rows the region can occupy after re-wrapping every owned row at the new
 *     width (all movement relative to the cursor, no row recomputed from the old
 *     width). The clear saves the cursor with DECSC and restores it with DECRC
 *     instead of counting back up, so a down-move that clamps at the bottom
 *     cannot make the restore overshoot above the region top;
 *   - the next frame recomputes the region height, reprints it below the
 *     committed rows and parks the cursor again. A taller region simply writes
 *     over the rows below it; a shorter one erases the rows it no longer owns.
 * If a resize lands while a frame is composing, the frame is discarded and the
 * erase + re-anchor runs before the next one starts.
 */

/* Last column of a region row that carries anything the terminal may reflow:
 * text, a themed background band, or the reverse-video caret. Mirrors the trim
 * rule in render_rows_ansi(). */
static int inline_row_width(const Grid *g, int y) {
    if (!g->cells || y < 0 || y >= g->rows) return 0;
    const Cell *na = g->cells + (size_t)y * g->cols;
    for (int x = g->cols - 1; x >= 0; x--) {
        u32 cp = na[x].cp;
        bool content = cp != CELL_CONT && cp != CELL_INVALID && cp != 0 && cp != ' ';
        bool band = cp != CELL_INVALID && na[x].bg != TH_NO_BG;
        if (cp != CELL_CONT && (content || band || (na[x].attrs & A_REVERSE))) {
            /* A wide glyph is marked by CELL_CONT in the next column; its last
             * occupied column is x+1, not x. Undercounting here would make the
             * reflow estimate too small and leave a fragment after a resize. */
            int wide = (x + 1 < g->cols && na[x + 1].cp == CELL_CONT) ? 2 : 1;
            return x + wide;
        }
    }
    return 0;
}

/* Rows one owned row occupies after re-wrapping at new_cols. */
static int inline_rows_for(int width, int new_cols) {
    if (new_cols < 1) new_cols = 1;
    return width > 0 ? (width + new_cols - 1) / new_cols : 1;
}

/* Rows the owned region can occupy after the terminal re-wraps its rows at
 * new_cols. Treating every row as its own hard line is exactly what tmux does
 * with the app's rows and an upper bound for a terminal that only joins
 * soft-wrapped lines, so clearing this many rows cannot leave a fragment. */
static int inline_reflow_rows(const Tui *st, int new_cols) {
    int total = 0;
    for (int y = 0; y < st->live_rows && y < GRID_MAX_ROWS; y++)
        total += inline_rows_for(st->live_w[y], new_cols);
    if (total < st->live_rows) total = st->live_rows;
    return total;
}

/* Rows the erase for `new_cols` must cover, capped to the terminal height. */
static int inline_erase_rows(const Tui *st, int new_cols) {
    int rows = term_rows(st->term);
    int e = inline_reflow_rows(st, new_cols);
    if (e > rows) e = rows;
    if (e < 1) e = 1;
    return e;
}

/* Append the owned-region erase to `out`. The cursor must be parked at the
 * region's top-left; the region's reflowed rows all extend downward from there,
 * so the erase clears down and relies on DECSC/DECRC to return to the saved
 * point. A counted up-move cannot be used: when the region top is not row 0 and
 * the reflowed rows do not fit below it, the down-moves clamp at the terminal
 * bottom and the up-move would overshoot above the region top (overwriting the
 * shell marker/banner). This is a pure byte builder: a frame that is later
 * discarded for a resize must not have mutated the tracked region state. */
static void inline_erase_owned_buf(const Tui *st, int new_cols, AgcBuf *out) {
    if (st->live_rows <= 0) return;
    int e = inline_erase_rows(st, new_cols);
    agentc_buf_cstr(out, "\x1b" "7");   /* DECSC: save the region-top cursor */
    for (int i = 0; i < e; i++) {
        agentc_buf_cstr(out, "\r\x1b[2K");
        if (i + 1 < e) agentc_buf_cstr(out, "\x1b[1B");
    }
    agentc_buf_cstr(out, "\x1b" "8");   /* DECRC: restore exactly, even if clamped */
}

/* Resize path: erase is its own atomic write (the re-anchor + repaint follows
 * in the next frame). The parked cursor is already the region top (off == 0),
 * so the erase saves and restores that point around the downward clears. */
static void inline_erase_owned(Tui *st, int new_cols) {
    AgcBuf out = { 0 };
    agentc_buf_cstr(&out, "\x1b[?7l");
    if (st->live_rows > 0 && st->live_cursor_off > 0)
        agentc_buf_printf(&out, "\x1b[%dA", st->live_cursor_off);
    inline_erase_owned_buf(st, new_cols, &out);
    agentc_buf_cstr(&out, "\x1b[?7h");
    term_write(st->term, out.p, out.len);
    agentc_buf_free(&out);
    st->live_rows = 0;
    st->live_cursor_off = 0;
    st->live_cursor_y = 0;
}

/* Append the commit of transcript rows [from, upto): render them once and print
 * them at the current cursor (the region top) with newline mode, so each row
 * flows above the region and pushes it down. This is the only path that can
 * scroll, and it scrolls only once the terminal is already full.
 * grid_resize() clamps the scratch grid to GRID_MAX_ROWS, so ranges longer than
 * that are committed in bounded chunks; the committed index advances per chunk,
 * over rows actually emitted, so nothing is skipped or reprinted. */
static void inline_commit_rows_buf(Tui *st, size_t from, size_t upto, AgcBuf *out) {
    int cols = term_cols(st->term);
    if (cols > GRID_MAX_COLS) cols = GRID_MAX_COLS;
    while (from < upto) {
        size_t chunk = upto - from;
        if (chunk > GRID_MAX_ROWS) chunk = GRID_MAX_ROWS;
        int n = (int)chunk;
        if (st->commit.cols != cols || st->commit.rows < n) grid_resize(&st->commit, cols, n);
        grid_clear(&st->commit, 0, TH_FG, TH_NO_BG);
        chat_render_rows(&st->chat, &st->commit, &st->theme, cols, (int)from, n,
                         tui_now_ms(st), st->spinner);
        /* Print the finished rows at the cursor (the region top); the trailing
         * newline of each row is what pushes the live region down. */
        render_rows_ansi(&st->commit, 0, n, out, &st->theme, true);
        if (st->test) {
            for (int y = 0; y < n; y++) {
                render_row_text(&st->commit, y, &st->scrollback);
                agentc_buf_byte(&st->scrollback, '\n');
            }
        }
        from += chunk;
        committed_set_rows(st, from);
    }
}

static void tui_inline_frame(Tui *st) {
    u64 gen = st->geom_gen;
    st->in_frame = true;
    tui_flush_pending(st);
    int cols = term_cols(st->term);
    int rows = term_rows(st->term);
    if (cols < 1) cols = 1;
    if (cols > GRID_MAX_COLS) cols = GRID_MAX_COLS;
    if (rows < 1) rows = 1;
    if (rows > GRID_MAX_ROWS) rows = GRID_MAX_ROWS;
    /* block heights depend on the wrap width, so set it before measuring */
    if (st->chat.width != cols) chat_set_width(&st->chat, cols);
    tui_menu_refresh(st);

    /* The footer is mandatory, then the editor and the queue strip; the menu
     * and the transcript tail share what is left, the tail yielding first so a
     * short terminal still shows a usable composer and status line. */
    TuiFrameBudget b = tui_frame_budget(st, rows, cols);
    int budget = b.budget;

    size_t total = (size_t)chat_total_height(&st->chat);
    if (total < st->committed_rows) {
        /* the transcript was cleared or replaced: the old scrollback mirror is
         * no longer ours to manage */
        inline_erase_owned(st, cols);
        st->committed_rows = 0;
        st->committed_blocks = 0;
        st->committed_partial = 0;
        st->scrollback.len = 0;
    }

    /* A frame can be discarded by a mid-frame resize after it has already
     * advanced the committed boundary and appended to the test mirror; snapshot
     * them so the discard path can roll the bookkeeping back. */
    size_t saved_committed_rows = st->committed_rows;
    size_t saved_committed_blocks = st->committed_blocks;
    size_t saved_committed_partial = st->committed_partial;
    size_t saved_scrollback_len = st->scrollback.len;

    /* A block is live only while it can still change: a running tool card, or a
     * streaming assistant/thinking block of an active run. Everything else is
     * printed to scrollback for good. */
    size_t keep_from = 0;
    for (size_t i = 0; i < st->chat.n; i++) {
        const ChatBlock *b = &st->chat.blocks[i];
        if (block_is_live(st, i, b)) break;
        keep_from += (size_t)chat_block_height(b);
    }
    /* Commit whole blocks only. A partially committed block would tie the
     * boundary to one width's row layout, and a resize would then reprint or
     * skip reflowed rows; whole blocks map exactly through chat_block_height()
     * at any width. A live block taller than the region is shown from its tail
     * (sticky bottom) until it finishes and is committed in one piece. */
    size_t target = keep_from;
    size_t committed_after = target > st->committed_rows ? target : st->committed_rows;
    int ncommit = (int)(committed_after - st->committed_rows);

    /* compose the live region: chat tail + queue + editor + menu + footer */
    int chat_h = (int)(total - committed_after);
    if (chat_h < 0) chat_h = 0;
    if (chat_h > budget) chat_h = budget;
    /* Record the transcript viewport height so PageUp/PageDown page by a screen
     * minus one row (chat_render sets it for fullscreen; the chat_render_rows
     * callers must do it too or the page size collapses to one). chat.scroll is
     * the distance scrolled up from the sticky bottom: clamp it to the
     * uncommitted transcript, then move the window top up by that many rows. */
    st->chat.height = chat_h;
    int max_scroll = (int)(total - committed_after) - chat_h;
    if (max_scroll < 0) max_scroll = 0;
    /* Keep the reading position stable while streaming, mirroring chat_render():
     * when the user has scrolled up, grow `scroll` by the growth of the max
     * scrollable amount instead of letting the window top slide down. */
    if (st->chat.scroll > 0 && max_scroll > st->chat.last_max)
        st->chat.scroll += max_scroll - st->chat.last_max;
    st->chat.last_max = max_scroll;
    if (st->chat.scroll > max_scroll) st->chat.scroll = max_scroll;
    if (st->chat.scroll < 0) st->chat.scroll = 0;
    int window_top = (int)total - chat_h - st->chat.scroll;
    if (window_top < (int)committed_after) window_top = (int)committed_after;
    size_t live_from = (size_t)window_top;   /* sticky bottom while streaming */
    int h = chat_h + b.qrows + b.eh + 1 + b.menu_h;
    if (h > rows) h = rows;
    if (h < 1) h = 1;
    if (st->live.cols != cols || st->live.rows < h) grid_resize(&st->live, cols, h);
    grid_clear(&st->live, 0, TH_FG, TH_NO_BG);
    if (chat_h) {
        chat_render_rows(&st->chat, &st->live, &st->theme, cols,
                         (int)live_from, chat_h, tui_now_ms(st), st->spinner);
    }
    int y = chat_h;
    if (b.qrows) {
        comp_queue(&st->live, &st->theme, y, cols, st->nq, st->queue[0]);
        y++;
    }
    st->cursor_x = 0;
    st->cursor_y = y;
    const char *placeholder = (st->chat.n == 0 && !st->running) ? TUI_EMPTY_HINT : NULL;
    editor_render(&st->ed, &st->live, &st->theme, 0, y, cols, b.eh, placeholder,
                  &st->cursor_x, &st->cursor_y);
    y += b.eh;
    if (b.menu_h) {
        tui_menu_scroll(st, b.menu_h);
        comp_command_menu(&st->live, &st->theme, 0, y, cols, b.menu_h, st->menu_name,
                          st->menu_desc, st->menu_n, st->menu_top, st->menu_sel, st->menu_kind == TUI_MENU_COMMAND ? "/" : NULL);
        y += b.menu_h;
    }
    tui_status_sync(st);
    comp_footer(&st->live, &st->theme, y, cols, st->status_segs, st->status_n);
    if (st->cursor_y >= h) st->cursor_y = h - 1;
    if (st->cursor_y < 0) st->cursor_y = 0;
    if (st->cursor_x >= cols) st->cursor_x = cols - 1;
    if (st->cursor_x < 0) st->cursor_x = 0;
    /* Remember the rendered fallback so the test API can check that the
     * hardware cursor and the reverse cell agree on one position. */
    {
        Cell *cc = grid_at(&st->live, st->cursor_x, st->cursor_y);
        st->cursor_reversed = cc && (cc->attrs & A_REVERSE) != 0;
    }
    for (int i = 0; i < h && i < GRID_MAX_ROWS; i++)
        st->live_w[i] = inline_row_width(&st->live, i);

    /* One write per frame: hide the cursor, suppress autowrap, return to the
     * region top from the parked cursor, clear the old rows when the region
     * shrank, commit new transcript rows at the cursor with newline mode so they
     * flow above and push the region down, then repaint the whole region and
     * park the hidden cursor back on its top-left. Everything is
     * cursor-relative, so the region follows the transcript instead of being
     * pinned to the terminal bottom. The editor's reverse-video cell is the only
     * visible caret. */
    AgcBuf out = { 0 };
    agentc_buf_cstr(&out, "\x1b[?25l\x1b[?7l");
    /* Return to the region top from the parked cursor. Inline parks the hidden
     * cursor on the region top-left (off == 0), so this is normally a no-op;
     * keep the step explicit for parity with the scrollback frame. */
    if (st->live_rows > 0 && st->live_cursor_off > 0)
        agentc_buf_printf(&out, "\x1b[%dA", st->live_cursor_off);
    /* Shrink: clear the old region before repainting so no stale rows survive
     * below the new, shorter region. The save/restore erase is position-exact. */
    if (h < st->live_rows) inline_erase_owned_buf(st, cols, &out);
    /* Commit finished rows at the cursor; newline mode flows them above the
     * region and pushes it down. Then repaint the region at the new cursor. */
    if (ncommit > 0)
        inline_commit_rows_buf(st, st->committed_rows, committed_after, &out);
    render_rows_ansi(&st->live, 0, h, &out, &st->theme, false);
    /* Park the hidden hardware cursor at the region top-left: up over the drawn
     * rows, column 1, hidden. off == 0 is the distance back to the region top. */
    if (h > 1) agentc_buf_printf(&out, "\x1b[%dA", h - 1);
    agentc_buf_cstr(&out, "\x1b[1G\x1b[?25l\x1b[?7h");

    /* A resize that landed while composing wins: discard the frame and apply
     * the erase + re-anchor before the next attempt. */
    st->in_frame = false;
    if (st->resize_pending || st->geom_gen != gen) {
        st->resize_pending = false;
        agentc_buf_free(&out);
        /* The frame bytes are thrown away: un-mark the rows the commit advanced
         * and drop the mirror text it appended. */
        st->committed_rows = saved_committed_rows;
        st->committed_blocks = saved_committed_blocks;
        st->committed_partial = saved_committed_partial;
        st->scrollback.len = saved_scrollback_len;
        if (st->scrollback.p) st->scrollback.p[saved_scrollback_len] = 0;
        tui_apply_resize(st, st->pending_cols, st->pending_rows);
        return;
    }
    term_write(st->term, out.p, out.len);
    agentc_buf_free(&out);
    st->live_rows = h;
    st->live_cursor_off = 0;   /* cursor parked (hidden) at the region top-left */
    st->live_cursor_y = 0;
    st->dirty = false;
    st->last_frame_ms = tui_now_ms(st);
    if (st->test) {
        st->live_text.len = 0;
        if (st->live_text.p) st->live_text.p[0] = 0;
        for (int i = 0; i < h; i++) {
            render_row_text(&st->live, i, &st->live_text);
            agentc_buf_byte(&st->live_text, '\n');
        }
    }
}

/* Leaving inline mode: clear the owned region, print whatever is left of the
 * conversation once so the transcript stays in scrollback, and end on a fresh
 * line. */
static void tui_inline_shutdown(Tui *st) {
    size_t total = (size_t)chat_total_height(&st->chat);
    AgcBuf out = { 0 };
    agentc_buf_cstr(&out, "\x1b[?7l");
    if (st->live_rows > 0 && st->live_cursor_off > 0)
        agentc_buf_printf(&out, "\x1b[%dA", st->live_cursor_off);
    inline_erase_owned_buf(st, term_cols(st->term), &out);
    agentc_buf_cstr(&out, "\x1b[?7h");
    term_write(st->term, out.p, out.len);
    agentc_buf_free(&out);
    st->live_rows = 0;
    st->live_cursor_off = 0;
    st->live_cursor_y = 0;
    if (total > st->committed_rows) scrollback_print_rows(st, st->committed_rows, total);
    term_write(st->term, "\r\n", 2);
}

static void tui_frame(Tui *st);

static void tui_render(Tui *st) {
    if (st->mode == AGENTC_TUI_FULLSCREEN) tui_frame(st);
    else if (st->mode == AGENTC_TUI_SCROLLBACK) tui_scrollback_frame(st);
    else tui_inline_frame(st);
}

static void tui_frame(Tui *st) {
    tui_flush_pending(st);
    if (st->cur.cols != st->prev.cols || st->cur.rows != st->prev.rows)
        grid_resize(&st->prev, st->cur.cols, st->cur.rows);
    tui_draw(st);
    AgcBuf out = { 0 };
    /* One atomic write per frame: hide the hardware cursor while the diff is
     * painted, then park it on the caret and show it again. The rendered
     * reverse cell under the caret is the fallback, so a terminal that ignores
     * ?25 still shows a caret. */
    agentc_buf_cstr(&out, "\x1b[?25l\x1b[?2026h");
    render_diff(&st->prev, &st->cur, &out, &st->theme);
    agentc_buf_cstr(&out, "\x1b[0m");
    agentc_buf_printf(&out, "\x1b[%d;%dH", st->cursor_y + 1, st->cursor_x + 1);
    agentc_buf_cstr(&out, "\x1b[?25h\x1b[?2026l");
    term_write(st->term, out.p, out.len);
    agentc_buf_free(&out);
    Grid tmp = st->prev;
    st->prev = st->cur;
    st->cur = tmp;
    st->dirty = false;
    st->last_frame_ms = tui_now_ms(st);
}

/* -------------------------------------------------------------- commands */

/* Built-in slash commands live in one table that feeds both dispatch and the
 * menu, so the two cannot drift. Names are without the leading slash; the menu
 * adds it. Extension commands are not listed here: they come from the extension
 * registry (agentc_ext_commands) with the descriptions their extensions
 * registered. */
typedef struct {
    const char *name;
    const char *description;
    bool (*run)(Tui *st, const char *args);
} TuiCommand;

static bool cmd_quit(Tui *st, const char *args) {
    (void)args;
    st->quit = true;
    return true;
}

static bool cmd_clear(Tui *st, const char *args) {
    (void)args;
    chat_clear(&st->chat);
    st->msg_mark = 0;
    st->msg_mark_set = false;
    return true;
}

/* A session swap (new or resume) replaces the agent transcript: clear the view
 * and the commit boundary so the next frame repaints from scratch. Without this
 * an equal-height replacement would keep the old state on screen. */
static void tui_after_session_swap(Tui *st) {
    chat_clear(&st->chat);
    editor_clear(&st->ed);
    st->msg_mark = 0;
    st->msg_mark_set = false;
    agentc_buf_clear(&st->pend_text);
    agentc_buf_clear(&st->pend_think);
    agentc_buf_clear(&st->tool_args);
    st->pend_order = 0;
    st->committed_rows = 0;
    st->committed_blocks = 0;
    st->committed_partial = 0;
    agentc_buf_clear(&st->scrollback);
    st->chat.scroll = 0;
}

static bool cmd_new(Tui *st, const char *args) {
    (void)args;
    if (st->running) {
        tui_noticef(st, "new: wait for the current run to finish\n", NULL);
        return true;
    }
    bool swap = st->app && st->app->new_session && st->agent;
    if (swap && st->app->new_session(st->app->ud) != 0) {
        /* a vetoed or failed swap must leave the current session and view
         * intact, or the user loses a transcript the swap never replaced */
        tui_noticef(st, "new: cannot start a new session\n", NULL);
        return true;
    }
    tui_after_session_swap(st);
    tui_noticef(st, swap ? "new: started a new session\n" : "/new: chat view cleared\n",
                NULL);
    return true;
}

/* /resume and /continue: pick another stored session. While a run is in flight
 * the switch is deferred: abort now (tools, jobs and the in-flight request
 * cancel with the turn) and open the picker once the submit has unwound, so the
 * swap can never race an executing tool. */
static bool cmd_resume(Tui *st, const char *args) {
    (void)args;
    if (!st->app || !st->app->resume_session) {
        tui_noticef(st, "resume: session storage is not available\n", NULL);
        return true;
    }
    if (st->running) {
        if (!st->switch_pending && st->agent) {
            st->switch_pending = true;
            agentc_agent_abort(st->agent);
            tui_noticef(st, "resume: cancelling the current run...\n", NULL);
        }
        tui_dirty(st);
        return true;
    }
    if (!tui_session_pick_open(st))
        tui_noticef(st, "resume: no stored sessions\n", NULL);
    tui_dirty(st);
    return true;
}

static bool cmd_compact(Tui *st, const char *args) {
    (void)args;
    if (st->running) {
        tui_noticef(st, "compact: wait for the current run to finish\n", NULL);
    } else if (!st->agent) {
        tui_noticef(st, "compact: no agent\n", NULL);
    } else {
        int rc = agentc_agent_compact(st->agent);
        if (rc != 0) {
            const char *err = agentc_agent_last_error(st->agent);
            tui_noticef(st, "compact: %s\n", err ? err : "?");
        } else if (agentc_agent_compacted(st->agent)) {
            tui_noticef(st, "compacted\n", NULL);
        } else {
            tui_noticef(st, "compact: nothing to compact\n", NULL);
        }
    }
    return true;
}

static bool cmd_help(Tui *st, const char *args) {
    (void)args;
    static const char help[] = "# agentc\n"
                               "- Enter submit, Alt+Enter newline\n"
                               "- Esc abort run, Ctrl+C clear, Ctrl+D delete/quit\n"
                               "- Ctrl+A/E line ends, Ctrl+B/F chars, Alt+B/F words\n"
                               "- Ctrl+W/Alt+Backspace kill word, Ctrl+K/U/Y kill/yank\n"
                               "- PgUp/PgDn scroll, Ctrl+O expand tool output\n"
                               "- /model [id], /theme [dark|light]\n"
                               "- /thinking [level]; blank opens a picker\n"
                               "- /compact compact the context\n"
                               "- /resume /continue switch session\n"
                               "- /quit /new /clear /help\n";
    chat_append_notice(&st->chat, help, agentc_strlen(help));
    return true;
}

static bool cmd_model(Tui *st, const char *args) {
    AgcBuf b = { 0 };
    const AgcTranscript *tr = st->agent ? agentc_agent_transcript(st->agent) : NULL;
    const char *cur = tr && tr->provider ? tr->provider : "";
    if (st->running) {
        agentc_buf_cstr(&b, "model: wait for the current run to finish\n");
    } else if (!args[0] && st->agent && tui_model_pick_open(st)) {
        /* the interactive picker took over; keys drive it until a selection */
        agentc_buf_free(&b);
        tui_dirty(st);
        return true;
    } else if (!args[0]) {
        agentc_buf_printf(&b, "model: %s (%s)\n", tr && tr->model ? tr->model : "?",
                      cur[0] ? cur : "?");
    } else if (!st->agent) {
        agentc_buf_cstr(&b, "model: no agent\n");
    } else {
        /* A model id normally exists on one provider. If this one is not in the
         * current provider's catalog but is another provider's, refuse rather
         * than silently point the current endpoint at it (which would just 404).
         * The TUI cannot switch provider in place; point at the restart path. */
        const AgcModel *own = agentc_model_find(cur, args);
        const AgcModel *other = own ? NULL : agentc_model_find(NULL, args);
        if (other && other->provider && !agentc_streq(other->provider, cur)) {
            agentc_buf_printf(&b, "model: '%s' belongs to provider '%s', not '%s'\n"
                              "       restart with `agentc --provider %s` or run `agentc setup`\n",
                          args, other->provider, cur[0] ? cur : "?", other->provider);
        } else if (agentc_agent_set_model(st->agent, args) == 0) {
            agentc_buf_printf(&b, "model: %s\n", args);
            if (!own)
                agentc_buf_printf(&b,
                              "note: '%s' is not in the %s catalog; the provider may reject it\n",
                              args, cur[0] ? cur : "current");
        } else {
            agentc_buf_printf(&b, "cannot switch to '%s'\n", args);
        }
    }
    chat_append_notice(&st->chat, (const char *)b.p, b.len);
    agentc_buf_free(&b);
    return true;
}

static bool cmd_theme(Tui *st, const char *args) {
    if (!args[0]) {
        /* The toggle is the same fixed-palette path as `/theme light|dark`, so
         * it also pushes the palette's background (and reports the new name). */
        const char *next = st->theme.dark ? "light" : "dark";
        (void)theme_apply_named(&st->theme, next);
        tui_sync_term_bg(st);
        grid_invalidate(&st->prev);   /* colours changed: repaint everything */
        const char *tnote = st->theme.dark ? "theme: dark\n" : "theme: light\n";
        chat_append_notice(&st->chat, tnote, agentc_strlen(tnote));
        return true;
    }
    char name[AGENTC_THEME_NAME_MAX + 1];
    size_t n = 0;
    while (args[n] && args[n] != ' ' && args[n] != '\t' && args[n] != '\n' &&
           n < sizeof name - 1) {
        name[n] = args[n];
        n++;
    }
    name[n] = 0;
    AgcBuf b = { 0 };
    if (theme_apply_named(&st->theme, name)) {
        tui_sync_term_bg(st);
        grid_invalidate(&st->prev);
        agentc_buf_printf(&b, "theme: %s\n", name);
    } else {
        agentc_buf_printf(&b, "theme: unknown '%s'\n", name);
    }
    chat_append_notice(&st->chat, (const char *)b.p, b.len);
    agentc_buf_free(&b);
    return true;
}

static int thinking_level_from_name(const char *s) {
    if (!s) return -1;
    if (agentc_streq(s, "off")) return 0;
    if (agentc_streq(s, "low")) return 1;
    if (agentc_streq(s, "medium")) return 3;
    if (agentc_streq(s, "high")) return 4;
    return -1;
}

static bool cmd_thinking(Tui *st, const char *args) {
    char word[24];
    size_t n = 0;
    while (args[n] && args[n] != ' ' && args[n] != '\t' && args[n] != '\n' &&
           n < sizeof word - 1) {
        word[n] = args[n];
        n++;
    }
    word[n] = 0;
    AgcBuf b = { 0 };
    if (!word[0]) {
        if (st->agent && !st->running && tui_thinking_pick_open(st)) {
            agentc_buf_free(&b);
            tui_dirty(st);
            return true;
        }
        agentc_buf_printf(&b, "thinking: %s\n", tui_thinking_name(st));
    } else {
        int level = thinking_level_from_name(word);
        if (level < 0) {
            agentc_buf_printf(&b, "thinking: unknown '%s' (off|low|medium|high)\n", word);
        } else {
            st->thinking_level = level;
            if (st->agent) agentc_agent_set_thinking(st->agent, level);
            agentc_buf_printf(&b, "thinking: %s\n", tui_thinking_name(st));
        }
    }
    chat_append_notice(&st->chat, (const char *)b.p, b.len);
    agentc_buf_free(&b);
    return true;
}

static const TuiCommand tui_commands[] = {
    { "clear", "clear the transcript view", cmd_clear },
    { "help", "show key bindings and slash commands", cmd_help },
    { "model", "switch model (picker with no argument)", cmd_model },
    { "new", "start a new session", cmd_new },
    { "quit", "exit agentc", cmd_quit },
    { "theme", "switch theme (dark|light|<name>)", cmd_theme },
    { "thinking", "set reasoning level (picker with no argument)", cmd_thinking },
    { "compact", "compact the conversation context", cmd_compact },
    { "resume", "switch to another session", cmd_resume },
    { "continue", "switch to another session", cmd_resume },
};

/* The menu is open while the composer holds an unterminated command word: text
 * starts with '/' and contains no space/newline yet. Returns false (menu
 * closed) in the argument phase and for over-long input. */
static bool tui_menu_word(const Editor *ed, char *out, size_t cap) {
    const char *s = editor_text(ed);
    if (s[0] != '/') return false;
    size_t i = 1;
    while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != '\n') {
        if (i >= cap) return false;
        out[i - 1] = s[i];
        i++;
    }
    if (s[i]) return false;
    out[i - 1] = 0;
    return true;
}

static bool tui_menu_prefix(const char *name, const char *filter) {
    for (size_t i = 0; filter[i]; i++)
        if (name[i] != filter[i]) return false;
    return true;
}

/* Dispatch precedence: a built-in command always shadows a prompt or extension
 * command with the same name, and a live prompt shadows an extension command.
 * The menu applies the same rule so it cannot advertise an unreachable entry.
 * The `skill:` prefix is reserved for `/skill:<name>`: dispatch routes it to
 * the skill handler before any registry lookup, so a registered prompt named
 * `skill:...` is unreachable and must not be advertised. */
static bool tui_is_builtin(const char *name) {
    for (size_t i = 0; i < sizeof tui_commands / sizeof tui_commands[0]; i++)
        if (agentc_streq(tui_commands[i].name, name)) return true;
    return false;
}

static bool tui_reserved_prefix(const char *name) {
    return agentc_strlen(name) >= 6 && agentc_memeq(name, "skill:", 6);
}

/* Rebuild the filtered entry list. The selection and any Escape dismissal reset
 * only when the typed word changed, so cursor motion and repeated frames leave
 * the selection alone. */
static void tui_menu_refresh(Tui *st) {
    if (st->pick_open) {
        tui_pick_refresh(st);
        return;
    }
    char filter[64];
    if (!tui_menu_word(&st->ed, filter, sizeof filter)) {
        st->menu_open = false;
        st->menu_filter[0] = 0;
        return;
    }
    if (!agentc_streq(st->menu_filter, filter)) {
        agentc_snprintf(st->menu_filter, sizeof st->menu_filter, "%s", filter);
        st->menu_sel = 0;
        st->menu_top = 0;
        st->menu_dismissed = false;
    }
    size_t n = 0;
    for (size_t i = 0; i < sizeof tui_commands / sizeof tui_commands[0]; i++) {
        if (!tui_menu_prefix(tui_commands[i].name, filter)) continue;
        if (n < TUI_MENU_MAX) {
            st->menu_name[n] = tui_commands[i].name;
            st->menu_desc[n] = tui_commands[i].description;
        }
        n++;
    }
    AgcPromptInfo prompts[TUI_MENU_MAX];
    size_t np = agentc_prompts_list(prompts, TUI_MENU_MAX);
    for (size_t i = 0; i < np; i++) {
        if (tui_is_builtin(prompts[i].name) || tui_reserved_prefix(prompts[i].name)) continue;
        if (!tui_menu_prefix(prompts[i].name, filter)) continue;
        if (n < TUI_MENU_MAX) {
            st->menu_name[n] = prompts[i].name;
            st->menu_desc[n] = prompts[i].description;
        }
        n++;
    }
    AgcExtCommandInfo cmds[TUI_MENU_MAX];
    size_t nc = agentc_ext_commands(cmds, TUI_MENU_MAX);
    for (size_t i = 0; i < nc && i < TUI_MENU_MAX; i++) {
        if (tui_is_builtin(cmds[i].name) || tui_reserved_prefix(cmds[i].name) ||
            agentc_prompts_has(cmds[i].name))
            continue;
        if (!tui_menu_prefix(cmds[i].name, filter)) continue;
        if (n < TUI_MENU_MAX) {
            st->menu_name[n] = cmds[i].name;
            st->menu_desc[n] = cmds[i].description;
        }
        n++;
    }
    if (n > TUI_MENU_MAX) n = TUI_MENU_MAX;
    st->menu_n = n;
    if (st->menu_sel >= n) st->menu_sel = n ? n - 1 : 0;
    st->menu_open = n > 0 && !st->menu_dismissed;
}

/* Rows the menu wants for an available row budget: never more than the cap,
 * never more than the frame can afford. Zero means the menu is closed or the
 * terminal is too short even for one row, in which case it is dropped. */
static int tui_menu_height(Tui *st, int avail) {
    if (!st->menu_open || st->menu_n == 0 || avail <= 0) return 0;
    size_t n = st->menu_n;
    if (n > TUI_MENU_VISIBLE_MAX) n = TUI_MENU_VISIBLE_MAX;
    if (n > (size_t)avail) n = (size_t)avail;
    return (int)n;
}

/* Keep the selected entry inside the visible window by scrolling the list. */
static void tui_menu_scroll(Tui *st, int menu_h) {
    if (menu_h <= 0) return;
    if (st->pick_open) {
        picklist_scroll(&st->pick, menu_h);
        st->menu_sel = st->pick.sel;
        st->menu_top = st->pick.top;
        return;
    }
    if (st->menu_sel < st->menu_top) st->menu_top = st->menu_sel;
    else if (st->menu_sel >= st->menu_top + (size_t)menu_h)
        st->menu_top = st->menu_sel - (size_t)menu_h + 1;
}

/* Extension commands live in the extension registry. Run one by exact name and
 * show the text it returned; a known command handled silently returns true, an
 * unknown name returns false so it still falls through to the model, which is
 * the existing behaviour for unknown slash input. */
static bool tui_ext_command(Tui *st, const char *name, const char *args) {
    AgcExtCommandInfo cmds[TUI_MENU_MAX];
    size_t n = agentc_ext_commands(cmds, TUI_MENU_MAX);
    for (size_t i = 0; i < n && i < TUI_MENU_MAX; i++) {
        if (!agentc_streq(cmds[i].name, name)) continue;
        char *res = agentc_ext_run_command(name, args);
        if (res) {
            chat_append_notice(&st->chat, res, agentc_strlen(res));
            if (res[0] && res[agentc_strlen(res) - 1] != '\n')
                chat_append_notice(&st->chat, "\n", 1);
            agentc_free(res);
        }
        return true;
    }
    return false;
}

/* Submit text produced by a slash command through the one submit path, so it
 * is recorded in the transcript exactly once. While a run is in flight the
 * expanded text queues like any other composer submit. `text` is consumed. */
static void tui_submit_command(Tui *st, char *text) {
    if (!text) text = agentc_strdup("");
    if (st->running) {
        queue_push(st, text);
        agentc_free(text);
    } else {
        tui_submit(st, text);
    }
}

static void tui_noticef(Tui *st, const char *fmt, const char *arg) {
    AgcBuf b = { 0 };
    agentc_buf_printf(&b, fmt, arg);
    chat_append_notice(&st->chat, (const char *)b.p, b.len);
    agentc_buf_free(&b);
}

/* ------------------------------------------------------ model picker */

/* The four configured reasoning levels, in the order `/thinking` lists them.
 * `tui_thinking_descs` is the short description shown next to each row. */
static const char *const tui_thinking_levels[] = { "off", "low", "medium", "high" };
static const char *const tui_thinking_descs[] = {
    "no reasoning output",
    "brief reasoning",
    "balanced reasoning",
    "deep reasoning",
};

/* Publish the picker's visible rows through the command menu's fields so the
 * existing layout/render path draws it unchanged. */
static void tui_pick_mirror(Tui *st) {
    st->menu_n = st->pick.vn;
    st->menu_sel = st->pick.sel;
    st->menu_top = st->pick.top;
    for (size_t i = 0; i < st->pick.vn && i < TUI_MENU_MAX; i++) {
        st->menu_name[i] = st->pick.vname[i];
        st->menu_desc[i] = st->pick.vdesc[i];
    }
    st->menu_open = st->pick_open && st->pick.vn > 0;
}

/* Re-filter the picker rows (the base rows are set by the open helpers). */
static void tui_pick_refresh(Tui *st) {
    picklist_refresh(&st->pick);
    tui_pick_mirror(st);
}

static void tui_pick_free_paths(Tui *st) {
    for (size_t i = 0; i < st->pick_paths_n; i++)
        agentc_free((void *)st->pick_paths[i]);
    st->pick_paths_n = 0;
}

static void tui_pick_close(Tui *st) {
    tui_pick_free_paths(st);
    st->pick_open = false;
    st->menu_kind = TUI_MENU_COMMAND;
    st->menu_open = false;
    st->menu_n = 0;
    st->menu_sel = 0;
    st->menu_top = 0;
    st->menu_filter[0] = 0;
    picklist_init(&st->pick);
}

/* Begin a picker over the base rows already prepared in pick_name/pick_desc. */
static void tui_pick_start(Tui *st, int kind, size_t n, size_t sel) {
    st->menu_kind = kind;
    st->pick_open = true;
    picklist_set(&st->pick, st->pick_base_name, st->pick_base_desc, n, sel);
    tui_pick_refresh(st);
}

/* /model: the runtime catalog for the current provider, restricted to the
 * current model's wire so the two `openai` rows never mix. */
static bool tui_model_pick_open(Tui *st) {
    if (!st->agent) return false;
    const AgcTranscript *tr = agentc_agent_transcript(st->agent);
    const char *provider = tr && tr->provider ? tr->provider : NULL;
    if (!provider || !provider[0]) return false;
    const AgcModel *cm =
        (tr->model && tr->model[0]) ? agentc_model_find(provider, tr->model) : NULL;
    const char *api = cm ? cm->api : NULL;
    const AgcModel *all[TUI_MENU_MAX];
    size_t n = agentc_model_filter(provider, api, all, TUI_MENU_MAX);
    if (n == 0) return false;
    size_t sel = 0;
    for (size_t i = 0; i < n; i++) {
        agentc_snprintf(st->pick_name[i], sizeof st->pick_name[i], "%s", all[i]->id);
        agentc_snprintf(st->pick_desc[i], sizeof st->pick_desc[i], "%s%s  ctx=%u%s%s",
                        all[i]->provider ? all[i]->provider : "",
                        (tr->model && agentc_streq(tr->model, all[i]->id))
                            ? "  (current)" : "",
                        all[i]->ctx_window, all[i]->reasoning ? "  reasoning" : "",
                        all[i]->image ? "  image" : "");
        st->pick_base_name[i] = st->pick_name[i];
        st->pick_base_desc[i] = st->pick_desc[i];
        if (tr->model && agentc_streq(tr->model, st->pick_name[i])) sel = i;
    }
    st->pick_paths_n = 0;
    tui_pick_start(st, TUI_MENU_MODEL, n, sel);
    return true;
}

/* /thinking: the four levels, current marked from the agent (the source of
 * truth, so --thinking at startup is reflected). */
static bool tui_thinking_pick_open(Tui *st) {
    if (!st->agent) return false;
    int cur = thinking_level_from_name(agentc_agent_thinking(st->agent));
    if (cur < 0) cur = st->thinking_level;
    for (size_t i = 0; i < 4; i++) {
        st->pick_base_name[i] = tui_thinking_levels[i];
        agentc_snprintf(st->pick_desc[i], sizeof st->pick_desc[i], "%s%s",
                        tui_thinking_descs[i],
                        thinking_level_from_name(tui_thinking_levels[i]) == cur
                            ? "  (current)" : "");
        st->pick_base_desc[i] = st->pick_desc[i];
    }
    st->pick_paths_n = 0;
    tui_pick_start(st, TUI_MENU_THINKING, 4, 0);
    return true;
}

/* /resume and /continue: stored sessions, newest first. `pick_paths` is owned
 * and freed on close. Returns false when there is nothing to list. */
static bool tui_session_pick_open(Tui *st) {
    if (!st->app || !st->app->resume_session) return false;
    tui_pick_free_paths(st);
    size_t sn = 0;
    char **paths = agentc_session_list(st->app->session_dir, &sn, TUI_MENU_MAX);
    if (!paths || sn == 0) {
        if (paths) agentc_sessions_free(paths, sn);
        return false;
    }
    i64 now = os_now_ns(OS_CLOCK_REALTIME) / 1000000;
    i64 stamps[TUI_MENU_MAX];
    size_t n = 0;
    for (size_t i = 0; i < sn && n < TUI_MENU_MAX; i++) {
        char preview[160];
        (void)agentc_session_summary(paths[i], &stamps[n], preview, sizeof preview);
        agentc_session_age_label(stamps[n], now, st->pick_name[n], sizeof st->pick_name[n]);
        agentc_snprintf(st->pick_desc[n], sizeof st->pick_desc[n], "%s",
                        preview[0] ? preview : "(empty session)");
        st->pick_paths[n] = agentc_strdup(paths[i]);
        n++;
    }
    agentc_sessions_free(paths, sn);
    /* newest first: the list is filename-sorted, the header stamp is authoritative */
    for (size_t i = 0; i + 1 < n; i++) {
        size_t best = i;
        for (size_t j = i + 1; j < n; j++)
            if (stamps[j] > stamps[best]) best = j;
        if (best != i) {
            char tmp[48];
            agentc_memcpy(tmp, st->pick_name[i], 48);
            agentc_memcpy(st->pick_name[i], st->pick_name[best], 48);
            agentc_memcpy(st->pick_name[best], tmp, 48);
            agentc_memcpy(tmp, st->pick_desc[i], 48);
            agentc_memcpy(st->pick_desc[i], st->pick_desc[best], 48);
            agentc_memcpy(st->pick_desc[best], tmp, 48);
            const char *tp = st->pick_paths[i];
            st->pick_paths[i] = st->pick_paths[best];
            st->pick_paths[best] = tp;
            i64 ts = stamps[i];
            stamps[i] = stamps[best];
            stamps[best] = ts;
        }
    }
    for (size_t i = 0; i < n; i++) {
        st->pick_base_name[i] = st->pick_name[i];
        st->pick_base_desc[i] = st->pick_desc[i];
    }
    st->pick_paths_n = n;
    tui_pick_start(st, TUI_MENU_SESSION, n, 0);
    return true;
}

/* Modal keyboard handling for every picker: the shared list owns filtering,
 * wrap-around Up/Down, paging, Enter and Escape; this maps the decision to the
 * picker's action. Every key is swallowed so nothing reaches the editor. */
static bool tui_pick_key(Tui *st, const Key *k) {
    int action = picklist_key(&st->pick, k, TUI_MENU_VISIBLE_MAX);
    if (action == PICKLIST_CANCEL) {
        tui_pick_close(st);
        tui_dirty(st);
        return true;
    }
    if (action == PICKLIST_ACCEPT) {
        const char *name = st->pick.vname[st->pick.sel];
        if (st->menu_kind == TUI_MENU_THINKING) {
            int level = name ? thinking_level_from_name(name) : -1;
            if (level >= 0) {
                st->thinking_level = level;
                if (st->agent) agentc_agent_set_thinking(st->agent, level);
                tui_noticef(st, "thinking: %s\n", tui_thinking_name(st));
            }
        } else if (st->menu_kind == TUI_MENU_SESSION) {
            size_t base = st->pick.vidx[st->pick.sel];
            const char *path = base < st->pick_paths_n ? st->pick_paths[base] : NULL;
            if (path && st->app && st->app->resume_session &&
                st->app->resume_session(st->app->ud, path) == 0) {
                tui_after_session_swap(st);
                tui_noticef(st, "resumed %s\n", name ? name : "");
            } else {
                tui_noticef(st, "resume: cannot open the session\n", NULL);
            }
        } else if (st->agent && name && agentc_agent_set_model(st->agent, name) == 0) {
            tui_noticef(st, "model: %s\n", name);
        } else {
            tui_noticef(st, "cannot switch to '%s'\n", name ? name : "?");
        }
        tui_pick_close(st);
        tui_dirty(st);
        return true;
    }
    /* navigation or filtering: keep the command-menu view in sync */
    st->menu_n = st->pick.vn;
    st->menu_sel = st->pick.sel;
    st->menu_top = st->pick.top;
    st->menu_open = st->pick.vn > 0;
    tui_dirty(st);
    return true;
}

/* `/skill:<name>`: read the SKILL.md body (frontmatter stripped), cap it, and
 * submit it through the normal path. Unknown and oversized skills get a notice
 * instead of falling through to the model. */
static bool tui_skill_command(Tui *st, const char *name, const char *args) {
    const char *sn = name + 6;
    if (!sn[0]) {
        tui_noticef(st, "skill: missing skill name (try /skill:%s)\n", "<name>");
        return true;
    }
    size_t blen = 0;
    char *body = agentc_builtin_context_skill_body(sn, &blen);
    if (!body) {
        tui_noticef(st, "skill: unknown skill '%s'\n", sn);
        return true;
    }
    if (blen > AGENTC_SKILL_SUBMIT_MAX) {
        agentc_free(body);
        AgcBuf b = { 0 };
        agentc_buf_printf(&b, "skill '%s' is too large (>%u KiB); not submitted\n", sn,
                          (unsigned)(AGENTC_SKILL_SUBMIT_MAX / 1024));
        chat_append_notice(&st->chat, (const char *)b.p, b.len);
        agentc_buf_free(&b);
        return true;
    }
    AgcBuf out = { 0 };
    agentc_buf_cstr(&out, body);
    agentc_free(body);
    if (args[0]) {
        agentc_buf_cstr(&out, "\n\n");
        agentc_buf_cstr(&out, args);
    }
    tui_submit_command(st, (char *)out.p);
    return true;
}

/* Registered prompt templates (file/extension/MCP): expand and submit. */
static bool tui_prompt_command(Tui *st, const char *name, const char *args) {
    if (!agentc_prompts_has(name)) return false;
    char *expanded = agentc_prompts_expand(name, args);
    if (!expanded) {
        tui_noticef(st, "prompt: cannot expand '%s'\n", name);
        return true;
    }
    tui_submit_command(st, expanded);
    return true;
}

static bool tui_command(Tui *st, const char *text) {
    tui_flush_pending(st);
    if (text[0] != '/') return false;
    const char *name = text + 1;
    size_t nlen = 0;
    while (name[nlen] && name[nlen] != ' ' && name[nlen] != '\t' && name[nlen] != '\n') nlen++;
    const char *args = name + nlen;
    while (*args == ' ' || *args == '\t') args++;
    /* NUL-terminate the command word: the registries compare full names, so
     * the raw slice must not run into the argument text. */
    char stack[72];
    char *token = stack;
    if (nlen >= sizeof stack) {
        token = agentc_strdup_len(name, nlen);
    } else {
        agentc_memcpy(stack, name, nlen);
        stack[nlen] = 0;
    }
    bool handled = false;
    for (size_t i = 0; i < sizeof tui_commands / sizeof tui_commands[0]; i++) {
        const TuiCommand *c = &tui_commands[i];
        if (agentc_strlen(c->name) == nlen && agentc_memeq(c->name, name, nlen)) {
            c->run(st, args);
            handled = true;
            break;
        }
    }
    if (!handled && nlen >= 6 && agentc_memeq(token, "skill:", 6))
        handled = tui_skill_command(st, token, args);
    if (!handled) handled = tui_prompt_command(st, token, args);
    if (!handled) handled = tui_ext_command(st, token, args);
    if (token != stack) agentc_free(token);
    return handled;
}

/* ---------------------------------------------------------------- abort */

static void tui_abort(Tui *st) {
    if (!st->running) {
        editor_clear(&st->ed);
        tui_dirty(st);
        return;
    }
    if (st->agent) agentc_agent_abort(st->agent);
    st->abort_requested = true;
    if (st->nq) {
        AgcBuf back = { 0 };
        for (size_t i = 0; i < st->nq; i++) {
            if (i) agentc_buf_byte(&back, '\n');
            agentc_buf_cstr(&back, st->queue[i]);
            agentc_free(st->queue[i]);
        }
        st->nq = 0;
        editor_set(&st->ed, back.p ? (const char *)back.p : "");
        agentc_buf_free(&back);
    }
    chat_scroll_bottom(&st->chat);
    tui_dirty(st);
}

/* -------------------------------------------------------------- accept */

/* One submit path for the editor and the menu's Enter: expand the buffer, run it
 * as a command when it is one, otherwise queue or submit it. Keeping this in one
 * place is why the menu's Enter is not a second command path. */
static void tui_accept(Tui *st) {
    char *text = editor_take(&st->ed);
    if (text[0] && !tui_command(st, text)) {
        if (st->running) {
            queue_push(st, text);
        } else {
            tui_submit(st, text);
            text = NULL;   /* tui_submit took ownership */
        }
    }
    agentc_free(text);
    tui_dirty(st);
}

/* ---------------------------------------------------------------- input */

static void tui_key(void *ud, const Key *k) {
    Tui *st = ud;
    tui_menu_refresh(st);
    if (st->pick_open) {
        tui_pick_key(st, k);
        return;
    }
    if (st->menu_open) {
        /* Menu precedence: while it is open it owns exactly Up/Down/Tab/Enter/
         * Escape. Every other key, the whole readline keymap included, falls
         * through to the editor below exactly as if the menu were closed.
         * Escape only dismisses the menu; the text and any in-flight run are
         * untouched. A name too long for the completion buffer is ignored
         * rather than silently truncated. */
        char buf[128];
        const char *name = st->menu_name[st->menu_sel];
        bool fits = agentc_strlen(name) + 2 <= sizeof buf;
        if (k->code == K_UP) {
            if (st->menu_sel > 0) st->menu_sel--;
            tui_dirty(st);
            return;
        }
        if (k->code == K_DOWN) {
            if (st->menu_sel + 1 < st->menu_n) st->menu_sel++;
            tui_dirty(st);
            return;
        }
        if (k->code == K_TAB && fits) {
            agentc_snprintf(buf, sizeof buf, "/%s ", name);
            editor_set(&st->ed, buf);
            tui_menu_refresh(st);   /* the trailing space closes the menu */
            tui_dirty(st);
            return;
        }
        if (k->code == K_ENTER && !(k->mods & MOD_ALT) && fits) {
            agentc_snprintf(buf, sizeof buf, "/%s", name);
            editor_set(&st->ed, buf);
            tui_accept(st);
            return;
        }
        if (k->code == K_ESC) {
            st->menu_dismissed = true;
            st->menu_open = false;
            tui_dirty(st);
            return;
        }
    }
    if (k->code == K_CHAR && (k->mods & MOD_CTRL)) {
        if (k->cp == 'c') {
            i64 now = tui_now_ms(st);
            if (editor_empty(&st->ed)) {
                if (now - st->last_ctrl_c_ms <= 1000) st->quit = true;
                else st->last_ctrl_c_ms = now;
            } else {
                editor_clear(&st->ed);
                /* The empty-state hint promises "once to clear, twice to
                 * exit": the press that follows a clear must quit, so arm the
                 * double-press window here too instead of only on an already
                 * empty composer. */
                st->last_ctrl_c_ms = now;
            }
            tui_dirty(st);
            return;
        }
        if (k->cp == 'd') {
            /* Readline EOF: an empty line quits; otherwise fall through so the
             * editor deletes the character under the caret. */
            if (editor_empty(&st->ed)) {
                st->quit = true;
                return;
            }
        }
        if (k->cp == 'o') {
            /* Expanding an already-committed card would change its height
             * under the row-index boundary and make the next frame append only
             * the delta, so both the collapsed and expanded forms would appear
             * in scrollback. Only toggle a card that is still live. */
            size_t ti = chat_last_tool_index(&st->chat);
            if (ti < st->chat.n && ti >= st->committed_blocks)
                chat_toggle_last_tool(&st->chat);
            tui_dirty(st);
            return;
        }
    }
    if (k->code == K_ESC) {
        tui_abort(st);
        return;
    }
    if (k->code == K_PGUP || k->code == K_PGDN) {
        int page = st->chat.height > 1 ? st->chat.height - 1 : 1;
        chat_scroll(&st->chat, k->code == K_PGUP ? page : -page);
        tui_dirty(st);
        return;
    }
    if (editor_key(&st->ed, k) == 1) {
        tui_accept(st);
        return;
    }
    tui_dirty(st);
}

static void tui_feed(Tui *st, const u8 *p, size_t n) {
    input_feed(&st->in, p, n, tui_now_ms(st) * 1000000, tui_key, st);
}

static void tui_read_input(Tui *st) {
    if (!st->term) return;
    if (term_is_tty(st->term)) {
        u8 buf[1024];
        for (;;) {
            struct os_pollfd p = { 0, OS_POLLIN, 0 };
            if (os_poll(&p, 1, 0) <= 0) break;
            int n = term_read(st->term, buf, sizeof buf);
            if (n <= 0) break;
            tui_feed(st, buf, (size_t)n);
        }
    } else {
        u8 buf[1024];
        int n;
        while ((n = term_read(st->term, buf, sizeof buf)) > 0) tui_feed(st, buf, (size_t)n);
    }
}

/* --------------------------------------------------------------- events */

static void tui_flush_pending(Tui *st);

static void tui_event(void *ud, int ev, const void *data) {
    Tui *st = ud;
    switch (ev) {
    case AGENTC_EV_AGENT_START:
        chat_scroll_bottom(&st->chat);
        break;
    case AGENTC_EV_MSG_START:
        /* Remember where this message begins so a retry can roll back
         * everything streamed for it (see AGENTC_EV_MSG_RESET). Seal the chat so
         * the first delta opens a fresh block even when the previous message
         * ended in a block of the same kind (a continuation turn): otherwise the
         * new text would merge into that block and a block-aligned rollback
         * could not remove it. */
        st->msg_mark = st->chat.n;
        st->msg_mark_set = true;
        chat_seal(&st->chat);
        break;
    case AGENTC_EV_MSG_END:
        st->msg_mark_set = false;
        tui_flush_pending(st);
        st->thinking = "off";
        agentc_buf_clear(&st->tool_args);
        break;
    case AGENTC_EV_MSG_RESET:
        /* A retry re-runs the assistant turn after content already streamed:
         * drop what a frame already committed to the chat and the deltas still
         * waiting in the pending buffers, so the replacement starts clean. The
         * commit loop keeps the whole in-progress message live, so the boundary
         * should never sit past `msg_mark`; clamp defensively anyway so a shrink
         * can never be mistaken for a full reset that reprints the transcript. */
        if (st->msg_mark_set) {
            if (st->msg_mark > st->chat.n) st->msg_mark = st->chat.n;   /* defensively renormalize */
            chat_truncate(&st->chat, st->msg_mark);
            chat_seal(&st->chat);   /* the replacement opens its own block */
            if (st->committed_blocks > st->chat.n) {
                st->committed_blocks = st->chat.n;
                st->committed_partial = 0;
                st->committed_rows = committed_rows_at(st);
            }
        }
        agentc_buf_clear(&st->pend_text);
        agentc_buf_clear(&st->pend_think);
        agentc_buf_clear(&st->tool_args);
        st->pend_order = 0;
        st->thinking = "off";
        break;
    case AGENTC_EV_TEXT_DELTA: {
        const AgcTextDelta *d = data;
        if (d && d->text && d->len) {
            if (st->pend_order == 0) st->pend_order = 1;
            agentc_buf_push(&st->pend_text, d->text, d->len);
        }
        break;
    }
    case AGENTC_EV_THINK_DELTA: {
        /* The configured level gates the view: a reasoning model may stream
         * `reasoning_content` even with thinking off, but the transcript stays
         * answer-only so the footer ("think:off") and the visible output
         * agree. The agent transcript still records the block for replay. */
        if (agentc_streq(tui_thinking_name(st), "off")) break;
        const AgcTextDelta *d = data;
        if (d && d->text && d->len) {
            if (st->pend_order == 0) st->pend_order = 2;
            agentc_buf_push(&st->pend_think, d->text, d->len);
        }
        st->thinking = "on";
        break;
    }
    case AGENTC_EV_TOOL_ARGS_DELTA: {
        const AgcTextDelta *d = data;
        if (d && d->text && d->len) agentc_buf_push(&st->tool_args, d->text, d->len);
        break;
    }
    case AGENTC_EV_TOOL_EXEC_START: {
        tui_flush_pending(st);
        const AgcToolExec *e = data;
        if (!e) break;
        const char *args = e->args_json && e->args_json[0]
                               ? e->args_json
                               : (st->tool_args.p ? (const char *)st->tool_args.p : "");
        chat_tool_start(&st->chat, e->tool_name, args, tui_now_ms(st));
        agentc_buf_clear(&st->tool_args);
        break;
    }
    case AGENTC_EV_TOOL_EXEC_END: {
        const AgcToolExec *e = data;
        if (!e) break;
        chat_tool_end(&st->chat, e->tool_name, e->result, e->is_error, e->duration_ms);
        break;
    }
    case AGENTC_EV_AGENT_END:
        tui_flush_pending(st);
        st->thinking = "off";
        agentc_buf_clear(&st->tool_args);
        st->running = false;
        break;
    case AGENTC_EV_COMPACT: {
        const AgcCompactInfo *ci = data;
        if (ci && st->on_compact) st->on_compact(st->on_compact_ud, ci);
        break;
    }
    case AGENTC_EV_ERROR: {
        const char *msg = data;
        if (msg && msg[0]) {
            AgcBuf b = { 0 };
            agentc_buf_cstr(&b, "error: ");
            agentc_buf_cstr(&b, msg);
            agentc_buf_byte(&b, '\n');
            chat_append_notice(&st->chat, (const char *)b.p, b.len);
            agentc_buf_free(&b);
        }
        break;
    }
    default:
        break;
    }
    tui_dirty(st);
}

static void tui_set_events(Tui *st) {
    if (st->agent) agentc_agent_set_events(st->agent, tui_event, st);
}

/* --------------------------------------------------------------- submit */

/* The poll hook must live at file scope (agentc_http only takes a function ptr). */
static void tui_poll_hook(void *ud, int timeout_ms);

static void tui_submit(Tui *st, char *text) {
    size_t n = agentc_strlen(text);
    chat_append_user(&st->chat, text, n ? n : 0);
    chat_scroll_bottom(&st->chat);
    st->running = true;
    st->abort_requested = false;
    st->run_started_ms = tui_now_ms(st);
    if (st->hist_set)
        editor_history_append(&st->ed, st->hist_path[0] ? st->hist_path : NULL, text);
    tui_dirty(st);
    if (st->test || !st->agent) {
        agentc_free(text);
        return;
    }
    agentc_http_set_poll_hook(tui_poll_hook, st);
    agentc_pump_install(tui_poll_hook, st);
    agentc_agent_submit(st->agent, text);
    /* Restore the shared mode pump: the TUI hook chained through it, and a
     * later idle tick or tool run must keep pumping the same duty. */
    agentc_pump_install(agentc_mode_pump, NULL);
    agentc_http_set_poll_hook(agentc_mode_pump, NULL);
    agentc_free(text);
    st->running = false;
    if (st->abort_requested) {
        chat_append_notice(&st->chat, "[aborted]\n", 10);
        st->abort_requested = false;
    }
    tui_dirty(st);
}

/* -------------------------------------------------------------- test API */

int agentc_tui_mode_parse(const char *value, int *out) {
    if (!value || !value[0]) return -1;
    int mode;
    if (agentc_streq(value, "scrollback")) mode = AGENTC_TUI_SCROLLBACK;
    else if (agentc_streq(value, "inline")) mode = AGENTC_TUI_INLINE;
    else if (agentc_streq(value, "fullscreen")) mode = AGENTC_TUI_FULLSCREEN;
    else if (agentc_streq(value, "auto")) mode = AGENTC_TUI_INLINE;
    else return -1;
    if (out) *out = mode;
    return 0;
}

const char *agentc_tui_mode_name(int mode) {
    switch (mode) {
    case AGENTC_TUI_SCROLLBACK: return "scrollback";
    case AGENTC_TUI_FULLSCREEN: return "fullscreen";
    default: return "inline";
    }
}

struct AgcTuiTest {
    Tui st;
    AgcBuf screen;
};

AgcTuiTest *agentc_tui_test_new_mode(int cols, int rows, int mode) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    Terminal *term = term_open_memory(cols, rows);
    AgcTuiTest *t = agentc_alloc(sizeof *t);
    agentc_memset(t, 0, sizeof *t);
    Tui *st = &t->st;
    agentc_memset(st, 0, sizeof *st);
    st->term = term;
    st->test = true;
    st->now_ms = 0;
    st->last_ctrl_c_ms = -100000;
    if (mode == AGENTC_TUI_FULLSCREEN) st->mode = AGENTC_TUI_FULLSCREEN;
    else if (mode == AGENTC_TUI_SCROLLBACK) st->mode = AGENTC_TUI_SCROLLBACK;
    else st->mode = AGENTC_TUI_INLINE;
    chat_init(&st->chat);
    editor_init(&st->ed);
    input_init(&st->in);
    theme_init(&st->theme, 1, false);
    theme_set_mode(&st->theme, THEME_TRUE);
    /* The built-ins are an ordinary provider; register them before the first
     * snapshot so a extension and the built-ins share one path. */
    agentc_status_register_builtin();
    grid_init(&st->cur, cols, rows);
    grid_init(&st->prev, cols, rows);
    grid_init(&st->live, cols, rows);
    grid_init(&st->commit, cols, rows);
    /* The memory backend cannot fail to enter raw mode. */
    (void)term_enter_mode(term, st->mode == AGENTC_TUI_FULLSCREEN);
    return t;
}

/* the classic harness: fullscreen, so every pre-inline golden keeps its meaning */
AgcTuiTest *agentc_tui_test_new(int cols, int rows) {
    return agentc_tui_test_new_mode(cols, rows, AGENTC_TUI_FULLSCREEN);
}

void agentc_tui_test_free(AgcTuiTest *t) {
    if (!t) return;
    Tui *st = &t->st;
    chat_free(&st->chat);
    editor_free(&st->ed);
    input_free(&st->in);
    grid_free(&st->cur);
    grid_free(&st->prev);
    grid_free(&st->live);
    grid_free(&st->commit);
    agentc_buf_free(&st->scrollback);
    agentc_buf_free(&st->live_text);
    agentc_buf_free(&st->tool_args);
    agentc_buf_free(&st->pend_text);
    agentc_buf_free(&st->pend_think);
    for (size_t i = 0; i < st->nq; i++) agentc_free(st->queue[i]);
    agentc_free(st->queue);
    agentc_buf_free(&t->screen);
    agentc_status_reset();   /* tests share one process: no provider leaks */
    term_close(st->term);
    agentc_free(t);
}

void agentc_tui_test_feed(AgcTuiTest *t, const char *bytes, size_t len) {
    if (!t || !bytes || !len) return;
    term_mem_feed(t->st.term, (const u8 *)bytes, len);
    tui_read_input(&t->st);
}

void agentc_tui_test_tick(AgcTuiTest *t, i64 delta_ms) {
    if (!t) return;
    t->st.now_ms += delta_ms;
    input_idle(&t->st.in, t->st.now_ms * 1000000, 50, tui_key, &t->st);
}

void agentc_tui_test_event(AgcTuiTest *t, int ev, const void *data) {
    if (t) tui_event(&t->st, ev, data);
}

void agentc_tui_test_submit(AgcTuiTest *t) {
    if (!t) return;
    char *text = editor_take(&t->st.ed);
    if (text[0])
        tui_submit(&t->st, text);
    else
        agentc_free(text);
}

void agentc_tui_test_live_metrics(AgcTuiTest *t, int *rows, int *cursor_y, int *cursor_off) {
    if (rows) *rows = t ? t->st.live_rows : 0;
    if (cursor_y) *cursor_y = t ? t->st.cursor_y : 0;
    if (cursor_off) *cursor_off = t ? t->st.live_cursor_off : 0;
}

int agentc_tui_test_mode(AgcTuiTest *t) { return t ? t->st.mode : AGENTC_TUI_INLINE; }

void agentc_tui_test_set_compact_hook(AgcTuiTest *t,
                                      void (*fn)(void *ud, const AgcCompactInfo *ci),
                                      void *ud) {
    if (!t) return;
    t->st.on_compact = fn;
    t->st.on_compact_ud = ud;
}

/* Inline ownership probe: the absolute row the region starts at (its top), the
 * caret's row inside it, and how many rows below the committed content the app
 * currently owns. -1 for the region top in the other modes. */
int agentc_tui_test_region_top(AgcTuiTest *t) {
    if (!t || t->st.mode != AGENTC_TUI_INLINE) return -1;
    int rows = term_rows(t->st.term);
    return rows - t->st.live_rows;
}

int agentc_tui_test_region_cleared(AgcTuiTest *t) {
    /* The whole live region sits below the committed transcript, so its height
     * is the number of rows the app currently owns/blank. */
    return t ? t->st.live_rows : 0;
}

int agentc_tui_test_region_row_width(AgcTuiTest *t, int y) {
    if (!t || y < 0 || y >= GRID_MAX_ROWS) return 0;
    return t->st.live_w[y];
}

/* The composer caret from the last layout: grid coordinates plus whether the
 * rendered fallback cell there carries A_REVERSE. The memory backend has no
 * hardware cursor, so this reverse cell is the observable caret and the test
 * compares it with the cursor-position sequences in the emitted frame. */
void agentc_tui_test_cursor(AgcTuiTest *t, int *x, int *y, bool *reverse) {
    if (x) *x = t ? t->st.cursor_x : 0;
    if (y) *y = t ? t->st.cursor_y : 0;
    if (reverse) *reverse = t && t->st.cursor_reversed;
}

u16 agentc_tui_test_cell_attrs(AgcTuiTest *t, int x, int y) {
    if (!t) return 0;
    Grid *g = t->st.mode == AGENTC_TUI_FULLSCREEN ? &t->st.cur : &t->st.live;
    Cell *c = grid_at(g, x, y);
    return c ? c->attrs : 0;
}

void agentc_tui_test_set_running(AgcTuiTest *t, bool on) {
    if (t) t->st.running = on;
}

/* Test-only: attach an agent so the model picker has a provider/catalog. The
 * caller retains ownership (the harness never frees it). */
void agentc_tui_test_set_agent(AgcTuiTest *t, AgcAgent *a) {
    if (t) t->st.agent = a;
}

void agentc_tui_test_set_app(AgcTuiTest *t, const AgcTuiApp *app) {
    if (t) t->st.app = app;
}

void agentc_tui_test_frame(AgcTuiTest *t) {
    if (!t) return;
    tui_flush_pending(&t->st);
    tui_render(&t->st);
}

void agentc_tui_test_resize(AgcTuiTest *t, int cols, int rows) {
    if (!t) return;
    term_mem_resize(t->st.term, cols, rows);
    tui_check_resize(&t->st);
}

void agentc_tui_test_resize_mid_frame(AgcTuiTest *t, int cols, int rows) {
    if (!t) return;
    /* Stage the race the single-threaded loop cannot interleave by itself: a
     * frame is marked in progress, the geometry changes, and the resize check
     * must defer the erase until that frame discards itself. */
    t->st.in_frame = true;
    term_mem_resize(t->st.term, cols, rows);
    tui_check_resize(&t->st);
    t->st.in_frame = false;
}

const char *agentc_tui_test_screen(AgcTuiTest *t) {
    if (!t) return "";
    tui_flush_pending(&t->st);
    if (t->st.mode == AGENTC_TUI_FULLSCREEN) {
        tui_draw(&t->st);
        agentc_buf_clear(&t->screen);
        render_screen_text(&t->st.cur, &t->screen);
        return t->screen.p ? (const char *)t->screen.p : "";
    }
    if (t->st.mode == AGENTC_TUI_SCROLLBACK) tui_scrollback_frame(&t->st);
    else tui_inline_frame(&t->st);   /* also commits anything the frame finished */
    return t->st.live_text.p ? (const char *)t->st.live_text.p : "";
}

const char *agentc_tui_test_scrollback(AgcTuiTest *t) {
    if (!t) return "";
    return t->st.scrollback.p ? (const char *)t->st.scrollback.p : "";
}

const char *agentc_tui_test_output(AgcTuiTest *t) {
    if (!t) return "";
    const AgcBuf *o = term_output(t->st.term);
    return o && o->p ? (const char *)o->p : "";
}

void agentc_tui_test_output_clear(AgcTuiTest *t) {
    if (t) term_output_clear(t->st.term);
}

bool agentc_tui_test_aborted(AgcTuiTest *t) { return t && t->st.abort_requested; }

bool agentc_tui_test_running(AgcTuiTest *t) { return t && t->st.running; }

bool agentc_tui_test_quit(AgcTuiTest *t) { return t && t->st.quit; }

void agentc_tui_test_set_history(AgcTuiTest *t, const char *path) {
    if (!t || !path) return;
    agentc_snprintf(t->st.hist_path, sizeof t->st.hist_path, "%s", path);
    t->st.hist_set = true;
    editor_history_load(&t->st.ed, path);
}

void agentc_tui_test_set_footer(AgcTuiTest *t, const char *model, const char *thinking,
                            u32 tok_in, u32 tok_out, i64 cost_micro) {
    if (!t) return;
    t->st.footer_set = true;
    t->st.model = model;
    t->st.thinking = thinking;
    int lv = thinking_level_from_name(thinking);
    t->st.thinking_level = lv < 0 ? 0 : lv;
    t->st.tok_in = tok_in;
    t->st.tok_out = tok_out;
    t->st.cost_micro = cost_micro;
}

/* Test-only: mirror agentc_tui_run's `show_tools` banner gate. */
void agentc_tui_test_set_show_tools(AgcTuiTest *t, bool on) {
    if (t) t->st.show_tools = on;
}

/* Test-only: run the same startup banner printer agentc_tui_run uses, so a test
 * can assert its bytes precede the first frame without a tty. */
void agentc_tui_test_print_banner(AgcTuiTest *t) {
    if (t) tui_banner(&t->st);
}

/* ------------------------------------------------------------- main loop */

/* The poll hook must live at file scope (agentc_http only takes a function ptr). */
static void tui_poll_hook(void *ud, int timeout_ms) {
    agentc_mode_pump(NULL, timeout_ms);   /* the shared extension pump duty */
    Tui *st = ud;
    tui_read_input(st);
    if (!st->test) st->now_ms = os_now_ns(OS_CLOCK_MONOTONIC) / 1000000;
    input_idle(&st->in, tui_now_ms(st) * 1000000, 50, tui_key, st);
    tui_check_resize(st);
    st->spinner = (int)((tui_now_ms(st) / 100) & 3);
    i64 now = tui_now_ms(st);
    if (st->dirty || (st->running && now - st->last_frame_ms >= 100)) tui_render(st);
}

static void tui_init(Tui *st, Terminal *term, AgcAgent *agent, bool test) {
    agentc_memset(st, 0, sizeof *st);
    st->term = term;
    st->agent = agent;
    st->test = test;
    st->last_ctrl_c_ms = -100000;
    chat_init(&st->chat);
    editor_init(&st->ed);
    st->ed.ascii = term_ascii_only(term);
    input_init(&st->in);
    theme_init(&st->theme, 1, !test);
    if (test) theme_set_mode(&st->theme, THEME_TRUE);
    agentc_status_register_builtin();
    grid_init(&st->cur, term_cols(term), term_rows(term));
    grid_init(&st->prev, term_cols(term), term_rows(term));
    st->mode = AGENTC_TUI_INLINE;
    grid_init(&st->live, term_cols(term), 4);
    grid_init(&st->commit, term_cols(term), 4);
    history_path(st->hist_path, sizeof st->hist_path);
    if (st->hist_path[0]) {
        if (!test) {
            /* mkdir -p of the parent directory */
            char dir[4096];
            agentc_snprintf(dir, sizeof dir, "%s", st->hist_path);
            char *slash = dir;
            while (*slash) slash++;
            while (slash > dir && slash[-1] != '/') slash--;
            if (slash > dir) { *slash = 0; mkdir_p(dir); }
        }
        editor_history_load(&st->ed, st->hist_path);
        st->hist_set = true;
    }
}

/* Fatal paths (out-of-memory via agentc_die, for example) must not leave the
 * user's terminal in raw mode on the alternate screen. */
static Terminal *g_term_cleanup;

static void tui_emergency_restore(void) {
    if (g_term_cleanup) {
        term_leave(g_term_cleanup);   /* idempotent */
        g_term_cleanup = NULL;
    }
}

/* Async-signal-safe: only write()/ioctl() through term_leave, no allocation. */
static void tui_signal(int sig) {
    tui_emergency_restore();
    os_exit(128 + sig);
}

static void tui_inline_shutdown(Tui *st);

static void tui_free(Tui *st) {
    chat_free(&st->chat);
    editor_free(&st->ed);
    input_free(&st->in);
    grid_free(&st->cur);
    grid_free(&st->prev);
    grid_free(&st->live);
    grid_free(&st->commit);
    agentc_buf_free(&st->scrollback);
    agentc_buf_free(&st->live_text);
    agentc_buf_free(&st->tool_args);
    agentc_buf_free(&st->pend_text);
    agentc_buf_free(&st->pend_think);
    for (size_t i = 0; i < st->nq; i++) agentc_free(st->queue[i]);
    agentc_free(st->queue);
}

/* ----------------------------------------------------- extension UI sink */
/* The extension host routes host->notify/host->set_title here while the TUI
 * owns the screen; print/json/rpc leave the sinks unset and keep the log
 * fallback. The terminal layer has no title operation, so set_title shares the
 * notice path (and a debug log). */
static void tui_ext_notice(Tui *st, const char *prefix, const char *text) {
    if (!st || !text) return;
    AgcBuf b = { 0 };
    agentc_buf_cstr(&b, prefix);
    agentc_buf_cstr(&b, text);
    agentc_buf_byte(&b, '\n');
    chat_append_notice(&st->chat, (const char *)b.p, b.len);
    agentc_buf_free(&b);
    tui_dirty(st);
}

static void tui_ext_notify(void *ud, const char *message, int level) {
    (void)level;
    tui_ext_notice(ud, "ext: ", message);
}

static void tui_ext_set_title(void *ud, const char *title) {
    if (title) agentc_logf(0, "ext title: %s", title);
    tui_ext_notice(ud, "ext title: ", title);
}

/* A named `/theme` resolves project themes only for a trusted project. The
 * app's resolved verdict (CLI > project_trust hook > saved verdict) is threaded
 * in as `trusted`: the TUI must not re-derive it, or a project trusted only
 * via --approve or a non-remembering hook could not resolve its themes. */
static void tui_set_project_theme_root(bool trusted) {
    const AgcExtHost *host = agentc_ext_host();
    const char *cwd = host && host->cwd ? host->cwd(host) : NULL;
    theme_set_project_root(cwd, trusted);
}

int agentc_tui_run(AgcAgent *agent, const char *initial_prompt, int mode,
                   const char *theme_name, bool trusted, bool show_tools,
                   const AgcTuiApp *app) {
    Terminal *term = term_open_tty();
    if (!term) return -1;
    if (mode != AGENTC_TUI_SCROLLBACK && mode != AGENTC_TUI_FULLSCREEN)
        mode = AGENTC_TUI_INLINE;   /* unknown values and "auto" resolve here */
    Tui *st = agentc_alloc(sizeof *st);
    tui_init(st, term, agent, false);
    if (agent) {
        int lv = thinking_level_from_name(agentc_agent_thinking(agent));
        if (lv >= 0) st->thinking_level = lv;
    }
    st->mode = mode;
    st->show_tools = show_tools;
    st->app = app;
    if (app) {
        st->on_compact = app->on_compact;
        st->on_compact_ud = app->ud;
    }
    tui_set_project_theme_root(trusted);
    if (theme_name && theme_name[0]) (void)theme_apply_named(&st->theme, theme_name);
    tui_sync_term_bg(st);
    AgcExtUiSink ui;
    agentc_memset(&ui, 0, sizeof ui);
    ui.notify = tui_ext_notify;
    ui.set_title = tui_ext_set_title;
    agentc_ext_set_ui_sink(&ui, st);
    tui_set_events(st);
    /* One banner line, printed before the owned region exists so it lands in the
     * terminal's scrollback and is never redrawn. */
    tui_banner(st);
    int enter_rc = term_enter_mode(term, mode == AGENTC_TUI_FULLSCREEN);
    if (enter_rc != 0) {
        /* Raw mode failed: undo the process-global registration, drop any
         * background the theme pushed, and report instead of running against a
         * terminal we do not own. */
        agentc_ext_set_ui_sink(NULL, NULL);
        if (agent) agentc_agent_set_events(agent, NULL, NULL);
        term_reset_bg(term);
        tui_free(st);
        term_close(term);
        agentc_free(st);
        return enter_rc;
    }
    /* from here on a fatal exit (agentc_die, e.g. out of memory) must restore the
     * terminal before the process goes away */
    g_term_cleanup = term;
    agentc_atexit(tui_emergency_restore);
    /* Restore the terminal when the process is killed or dies from a fault;
     * Ctrl+C never reaches us in raw mode, but Ctrl+C sent by another process,
     * a closing terminal window and crashes all do. */
    os_sig_install(OS_SIGHUP, tui_signal);
    os_sig_install(OS_SIGINT, tui_signal);
    os_sig_install(OS_SIGQUIT, tui_signal);
    os_sig_install(OS_SIGABRT, tui_signal);
    os_sig_install(OS_SIGBUS, tui_signal);
    os_sig_install(OS_SIGFPE, tui_signal);
    os_sig_install(OS_SIGSEGV, tui_signal);
    os_sig_install(OS_SIGTERM, tui_signal);
    tui_check_resize(st);

    if (initial_prompt && initial_prompt[0]) {
        char *text = agentc_strdup(initial_prompt);
        tui_submit(st, text);
    }
    /* --continue/--resume on a terminal: offer the session picker before the
     * first prompt (Escape keeps the session the mode already opened). */
    if (app && app->pick_session_on_start) (void)tui_session_pick_open(st);
    tui_render(st);

    while (!st->quit) {
        struct os_pollfd p = { 0, OS_POLLIN, 0 };
        int pr = os_poll(&p, 1, 16);
        agentc_mode_pump(NULL, 0);   /* idle tick: deferred extension work */
        if (pr > 0) {
            if (p.revents & (OS_POLLHUP | OS_POLLERR)) {
                st->quit = true;   /* the terminal went away */
                break;
            }
            tui_read_input(st);
        }
        st->now_ms = os_now_ns(OS_CLOCK_MONOTONIC) / 1000000;
        input_idle(&st->in, st->now_ms * 1000000, 50, tui_key, st);
        tui_check_resize(st);
        if (!st->running && st->switch_pending) {
            /* a mid-run /resume asked to switch: the run has unwound, so no
             * tool or request is live; open the picker now */
            st->switch_pending = false;
            if (!tui_session_pick_open(st))
                tui_noticef(st, "resume: no stored sessions\n", NULL);
            tui_dirty(st);
        } else if (!st->running && st->nq) {
            char *text = queue_pop(st);
            tui_submit(st, text);
        }
        st->spinner = (int)((st->now_ms / 100) & 3);
        if (st->dirty || (st->running && st->now_ms - st->last_frame_ms >= 100))
            tui_render(st);
    }

    g_term_cleanup = NULL;   /* the normal path already restored the terminal */
    if (st->mode == AGENTC_TUI_INLINE) tui_inline_shutdown(st);
    else if (st->mode == AGENTC_TUI_SCROLLBACK) tui_scrollback_shutdown(st);
    term_leave(term);
    agentc_ext_set_ui_sink(NULL, NULL);   /* st is freed next; drop the borrowed ud */
    tui_free(st);
    term_close(term);
    agentc_free(st);
    return 0;
}
