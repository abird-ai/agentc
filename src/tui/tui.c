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
#include "config.h"
#include "session.h"
#include "settings.h"
#include "picklist.h"
#include "base/limits.h"
#include "core/prompt.h"
#include "core/prompts.h"
#include "core/tools/engine.h"
#include "ext.h"
#include "hook.h"
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

/* Double-press window for Ctrl-C: a second press inside this window quits (or
 * the first press after a clear, which re-arms it). It is also the lifetime of
 * the transient "press again to exit" hint drawn above the composer. */
#define TUI_CTRL_C_WINDOW_MS 1000

/* Slash-command menu limits. The registry is small; the caps keep the popup
 * from ever pushing the inline live region past its row budget. */
#define TUI_MENU_MAX 128
#define TUI_MENU_VISIBLE_MAX 8
/* Model ids and virtual aliases are longer than the 47-byte display name
 * buffer; the picker keeps the full id in this owned row and only borrows the
 * display-sized `pick_name` for labels. Keep it above the alias cap (64) with
 * headroom for discovered ids. */
#define TUI_PICK_ID_MAX 128
/* The one row a filter with no hits still draws, so the pane never vanishes
 * while it owns the keyboard. */
#define TUI_PICK_EMPTY "(no matches)"

/* Inline flushes committed rows to the real scrollback by rendering them at the
 * terminal's current width. Below this width wrapping is meaningless (a one-
 * column terminal would bake one character per scrollback line), so the window
 * keeps the rows in memory until the terminal is usable again; the resize path
 * reflows and repaints them. */
#define TUI_INLINE_FLUSH_MIN_COLS 20

/* Which list the shared menu_* rows describe. The slash-command menu is derived
 * from the composer; the interactive pickers (TUI_MENU_MODEL for `/model`,
 * TUI_MENU_THINKING for `/thinking`, TUI_MENU_SESSION for `/resume`) own the
 * keyboard until a selection or Escape. */
enum {
    TUI_MENU_COMMAND = 0,
    TUI_MENU_MODEL = 1,
    TUI_MENU_THINKING = 2,
    TUI_MENU_SESSION = 3,
    TUI_MENU_SETTING = 4,
    TUI_MENU_TREE = 5, /* P7 /tree: the session fork forest */
};

typedef struct {
    Terminal *term;
    AgcAgent *agent;
    const AgcTuiApp *app; /* app-owned services (/new); may be NULL */
    void (*on_compact)(void *ud, const AgcCompactInfo *ci);
    void *on_compact_ud;
    bool test;
    bool show_tools; /* banner advertises the resolved engines */
    i64 now_ms;
    i64 last_frame_ms;
    int spinner;

    Chat chat;
    Editor ed;
    Theme theme;
    Input in;

    Grid cur, prev;
    int header_blocks; /* N83: leading intro-header blocks (0 or 1), not
                        * counted as transcript for the empty hint */
    u64 geom_gen;      /* bumped on every accepted geometry change */
    bool dirty, quit, running, abort_requested;
    i64 run_started_ms;
    i64 last_ctrl_c_ms;
    bool ctrl_c_hint; /* last poll's hint state, to repaint on lapse */

    char **queue;
    size_t nq, qcap;
    AgcBuf tool_args;
    AgcBuf pend_text; /* streamed deltas coalesced to one markdown append per frame */
    AgcBuf pend_think;
    int pend_order;    /* 0 none, 1 text first, 2 think first */
    size_t msg_mark;   /* chat.n when the current MSG_START opened its message */
    bool msg_mark_set; /* a message is open: MSG_RESET may roll the chat back to msg_mark */

    bool footer_set;
    const char *model;
    const char *thinking; /* transient "on"/"off" from delta events */
    int thinking_level;   /* configured level: 0=off,1=low,3=medium,4=high */
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
    bool cursor_reversed; /* the rendered fallback cell at (cursor_x, cursor_y) is A_REVERSE */

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
    char pick_id[TUI_MENU_MAX][TUI_PICK_ID_MAX]; /* full model id/alias, owned */
    char pick_name[TUI_MENU_MAX][48];
    char pick_desc[TUI_MENU_MAX][48];
    const char *pick_paths[TUI_MENU_MAX];
    size_t pick_paths_n;
    /* P6 completion pass 2g: async approval overlay. A registered `approval`
     * hook parks while this is open; the answer is submitted with
     * `agentc_approval_submit` from the key handler. The modal shows the tool
     * and its input and offers a scope picker (once|step|session|project) plus a
     * `remember` choice that forces the persisted project scope on allow. With
     * no request pending every field stays zero/false, so the layout is
     * byte-identical. */
    bool approval_open;
    uint64_t approval_id;
    char approval_tool[64];
    char approval_trigger[64];
    char approval_input[220];
    int approval_scope;     /* AGENTC_SCOPE_* selection */
    bool approval_remember; /* r: submit the persisted project scope */
    uint64_t approval_hook;
    /* P5 C2 generic UI dialog: a synchronous prompt sink (confirm/select/
     * input/editor). While `dialog_open` is false every field stays
     * zero/false, so the layout and the in-memory test goldens are unchanged. */
    bool dialog_open;
    bool dialog_done;
    bool dialog_ok;
    char dialog_method[16];
    char dialog_prompt[400];
    char dialog_opt[8][64];
    size_t dialog_nopts;
    size_t dialog_sel;
    char dialog_text[512];
    size_t dialog_text_len;
    char dialog_answer[512];
    const char *dialog_deflt; /* borrowed from the sink call */
    /* P6 completion pass 2h: /settings single-value editor modal. The picker
     * opens it for a non-bool/non-enum row, prefilling the resolved value (a
     * SECRET is never prefilled, only its `…last4` preview is shown); Enter
     * validates through agentc_settings_set and an invalid value keeps the
     * modal open with the reason, Escape cancels (fail-closed). With no editor
     * open every field is zero/false, so the layout and every TUI golden stay
     * byte-identical. */
    bool setting_edit_open;
    char setting_edit_ext[64];
    char setting_edit_key[64];
    int setting_edit_type;
    uint32_t setting_edit_flags;
    char setting_edit_cur[80]; /* resolved-value preview (masked for SECRET) */
    char setting_edit_buf[512];
    size_t setting_edit_len;
    size_t setting_edit_cursor; /* caret byte offset into setting_edit_buf */
    char setting_edit_err[96];
} Tui;

/* --------------------------------------------------------------- helpers */

static i64 tui_now_ms(Tui *st) {
    if (st->test)
        return st->now_ms;
    return os_now_ns(OS_CLOCK_MONOTONIC) / 1000000;
}

static void tui_dirty(Tui *st) {
    st->dirty = true;
}

/* True while the Ctrl-C double-press window is armed on an empty composer, so
 * the next Ctrl-C quits. Drives the transient exit hint above the composer; it
 * lapses after TUI_CTRL_C_WINDOW_MS, the same window the key handler uses. */
static bool tui_ctrl_c_hint(Tui *st) {
    return editor_empty(&st->ed) && tui_now_ms(st) - st->last_ctrl_c_ms <= TUI_CTRL_C_WINDOW_MS;
}

/* Startup banner: the intro header is the FIRST transcript block, so it is
 * visible in the fullscreen app and scrolls away with the transcript (and is
 * carried into the exit dump). A second `tools: …` line names the resolved
 * core-tool engines, kept succinct (binary names only) so it rides with the
 * version. */
static void tui_banner(Tui *st) {
    if (!st || !st->term)
        return;
    /* One block with a hard break (two trailing spaces) so the two header lines
     * are adjacent -- two blocks would put a blank line between them. */
    AgcBuf h = {0};
    agentc_buf_printf(&h, "agentc %s  \n", AGENTC_VERSION);
    const char *tools = st->show_tools ? agentc_tool_engine_summary() : NULL;
    if (tools && tools[0])
        agentc_buf_printf(&h, "tools: %s\n", tools);
    chat_append_notice(&st->chat, (const char *)h.p, h.len);
    agentc_buf_free(&h);
    st->header_blocks = 1;
}

/* A fixed light/dark palette owns its background: push TH_BG to the terminal
 * (OSC 11) so a light theme is not dark text on a dark terminal, and clear it
 * for `system`/named themes that follow the terminal's own background. */
static void tui_sync_term_bg(Tui *st) {
    if (!st || !st->term || !term_is_tty(st->term))
        return;
    if (st->theme.force_bg)
        term_set_bg(st->term, st->theme.rgb[TH_BG]);
    else
        term_reset_bg(st->term);
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
    if (!st->nq)
        return NULL;
    char *p = st->queue[0];
    for (size_t i = 1; i < st->nq; i++)
        st->queue[i - 1] = st->queue[i];
    st->nq--;
    return p;
}

static void mkdir_p(const char *path) {
    char tmp[4096];
    size_t n = agentc_strlen(path);
    if (n >= sizeof tmp)
        return;
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
    char dir[4096];
    if (agentc_state_home(dir, sizeof dir) && agentc_path_join(buf, cap, dir, "history"))
        return;
    buf[0] = 0;
}

/* ------------------------------------------------------------ submission */

static void tui_submit(Tui *st, char *text);
static void tui_accept(Tui *st);
static void tui_feed(Tui *st, const u8 *p, size_t n);
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
static bool tui_settings_pick_open(Tui *st);
static bool tui_tree_pick_open(Tui *st);
static const char *tui_setting_type_name(int type);
static void tui_setting_edit_open(Tui *st, const AgcSettingInfo *info);
static void tui_setting_edit_key(Tui *st, const Key *k);
static void tui_after_session_swap(Tui *st);

/* The interactive TUI always runs on the alternate screen, so a geometry change
 * has no owned band to erase and no cursor anchor to preserve: resize the grids
 * and let the next frame repaint the whole surface. Every render path clamps to
 * GRID_MAX_COLS/ROWS; the resize path must too. */
static void tui_apply_resize_fs(Tui *st, int c, int r) {
    if (c > GRID_MAX_COLS)
        c = GRID_MAX_COLS;
    if (r > GRID_MAX_ROWS)
        r = GRID_MAX_ROWS;
    if (c < 1)
        c = 1;
    if (r < 1)
        r = 1;
    grid_resize(&st->cur, c, r);
    grid_resize(&st->prev, c, r);
    grid_invalidate(&st->prev);
    chat_set_width(&st->chat, c);
    tui_dirty(st);
}

/* Fullscreen resize: no DSR probe, no settle window, no band erase and no
 * content anchor -- the alternate screen repaints itself. */
static void tui_check_resize(Tui *st) {
    int c = 0, r = 0;
    if (term_check_resize(st->term, &c, &r)) {
        st->geom_gen++;
        tui_apply_resize_fs(st, c, r);
    }
}

/* --------------------------------------------------------------- drawing */

/* Rows a non-footer chrome region contributes (one value per row). Zero when
 * unused, so the layout math below is byte-identical without them. */
static int tui_region_count(const Tui *st, int region) {
    int n = 0;
    for (size_t i = 0; i < st->status_n; i++)
        if (st->status_segs[i].region == region && st->status_segs[i].text[0])
            n++;
    return n;
}

static void tui_layout(Tui *st) {
    int cols = st->cur.cols;
    int rows = st->cur.rows;
    grid_clear(&st->cur, 0, TH_FG, TH_NO_BG);

    int above_h = tui_region_count(st, AGENTC_REGION_ABOVE_COMPOSER);
    int below_h = tui_region_count(st, AGENTC_REGION_BELOW_COMPOSER);
    int below_status_h = tui_region_count(st, AGENTC_REGION_BELOW_STATUS);

    int status_y = rows - 1 - below_status_h;
    int footer_y = status_y;
    int queue_y = -1;
    if (st->nq) {
        queue_y = footer_y - 1;
        footer_y--;
    }
    /* While the Ctrl-C double-press window is armed, reserve a transient line
     * directly above the composer so the user sees that the next press quits.
     * It comes out of the transcript/menu budget (it is above the composer), not
     * the composer's place above the footer or the queue strip, which sits below. */
    int hint_h = tui_ctrl_c_hint(st) ? 1 : 0;
    /* The approval overlay owns four lines above the composer while a request
     * is pending (tool/input, scope picker, hint); with none open it costs
     * nothing, so the layout is unchanged. */
    int approval_h = st->approval_open ? 4 : 0;
    /* The generic dialog overlay owns three lines above the composer while a
     * prompt is pending; with none open it costs nothing. */
    int dialog_h = st->dialog_open ? 3 : 0;
    /* The /settings single-value editor owns four lines above the composer while
     * it is open; with none open it costs nothing. */
    int setting_h = st->setting_edit_open ? 4 : 0;
    int modal_h = approval_h + dialog_h + setting_h;
    int editor_h = editor_visual_rows(&st->ed, cols);
    if (editor_h > EDITOR_MAX_ROWS)
        editor_h = EDITOR_MAX_ROWS;
    if (editor_h < 1)
        editor_h = 1;
    int avail = footer_y - below_h;
    if (editor_h > avail - 1)
        editor_h = avail > 1 ? avail - 1 : 1;
    int editor_y = footer_y - below_h - editor_h;
    if (editor_y < 1)
        editor_y = 1;

    tui_menu_refresh(st);
    /* The menu stacks directly above the hint (which sits above the composer). */
    int menu_h = tui_menu_height(st, editor_y - hint_h - modal_h);
    int chat_h = editor_y - hint_h - modal_h - menu_h - above_h;
    if (chat_h < 0)
        chat_h = 0;

    const char *placeholder =
        (st->chat.n <= (size_t)st->header_blocks && !st->running) ? TUI_EMPTY_HINT : NULL;
    int spinner = st->spinner;
    /* The last column is a fixed scrollbar gutter: the transcript wraps at
     * cols-1 so the bar never overlaps content nor reflows it. */
    int content_w = cols > 1 ? cols - 1 : cols;
    chat_render(&st->chat, &st->cur, &st->theme, 0, 0, content_w, chat_h, tui_now_ms(st), spinner);
    /* Thin muted scrollbar in the gutter when there is history above the window
     * (PgUp/PgDn scroll). The thumb sits at the bottom at scroll=0, so it reads
     * as "you are at the end". */
    if (chat_h > 0) {
        size_t total_rows = (size_t)chat_total_height(&st->chat);
        if (total_rows > (size_t)chat_h)
            comp_scrollbar(&st->cur, &st->theme, cols - 1, 0, chat_h, total_rows, (size_t)chat_h,
                           (size_t)st->chat.scroll);
    }
    if (menu_h > 0) {
        tui_menu_scroll(st, menu_h);
        comp_command_menu(&st->cur, &st->theme, 0, chat_h, cols, menu_h, st->menu_name,
                          st->menu_desc, st->menu_n, st->menu_top, st->menu_sel,
                          st->menu_kind == TUI_MENU_COMMAND ? "/" : NULL);
    }
    if (approval_h > 0) {
        int ay = editor_y - hint_h - approval_h - dialog_h - setting_h;
        char line[256];
        agentc_snprintf(line, sizeof line, "Approve %s (%s)?  [y] allow  [n] deny",
                        st->approval_tool, st->approval_trigger);
        comp_hint(&st->cur, &st->theme, ay++, cols, line);
        agentc_snprintf(line, sizeof line, "input: %s", st->approval_input);
        comp_hint(&st->cur, &st->theme, ay++, cols, line);
        agentc_snprintf(line, sizeof line,
                        "scope: %s[o]nce  %s[s]tep  %s[e]ssion  %s[p]roject   remember: %s",
                        st->approval_scope == AGENTC_SCOPE_ONCE ? "*" : " ",
                        st->approval_scope == AGENTC_SCOPE_STEP ? "*" : " ",
                        st->approval_scope == AGENTC_SCOPE_SESSION ? "*" : " ",
                        st->approval_scope == AGENTC_SCOPE_PROJECT ? "*" : " ",
                        st->approval_remember ? "on" : "off");
        comp_hint(&st->cur, &st->theme, ay++, cols, line);
        comp_hint(&st->cur, &st->theme, ay, cols,
                  "o/s/e/p scope, r remember, y allow; any other key denies");
    }
    if (dialog_h > 0) {
        int dy = editor_y - hint_h - dialog_h - setting_h;
        char line[512];
        if (st->dialog_nopts > 0) {
            char opts[256];
            size_t o = 0;
            opts[0] = 0;
            for (size_t i = 0; i < st->dialog_nopts && o + 2 < sizeof opts; i++) {
                int w = agentc_snprintf(opts + o, sizeof opts - o, "%s%s", i ? " " : "",
                                        st->dialog_opt[i]);
                if (w < 0)
                    break;
                o += (size_t)w;
            }
            agentc_snprintf(line, sizeof line, "%s: %s  [%s]", st->dialog_method, st->dialog_prompt,
                            opts);
        } else {
            agentc_snprintf(line, sizeof line, "%s: %s", st->dialog_method, st->dialog_prompt);
        }
        comp_hint(&st->cur, &st->theme, dy++, cols, line);
        if (st->dialog_nopts > 0)
            agentc_snprintf(line, sizeof line, "select: %s   (1-%zu, Enter)",
                            st->dialog_opt[st->dialog_sel], st->dialog_nopts);
        else
            agentc_snprintf(line, sizeof line, "input: %s", st->dialog_text);
        comp_hint(&st->cur, &st->theme, dy++, cols, line);
        comp_hint(&st->cur, &st->theme, dy, cols, "Enter answer, Esc cancel (fail-closed)");
    }
    if (setting_h > 0) {
        int sy = editor_y - hint_h - dialog_h - setting_h;
        char line[700];
        agentc_snprintf(line, sizeof line, "edit %s.%s (%s)%s", st->setting_edit_ext,
                        st->setting_edit_key, tui_setting_type_name(st->setting_edit_type),
                        (st->setting_edit_flags & AGENTC_SETTING_SECRET) ? "  secret" : "");
        comp_hint(&st->cur, &st->theme, sy++, cols, line);
        agentc_snprintf(line, sizeof line, "now: %s",
                        st->setting_edit_cur[0] ? st->setting_edit_cur : "(default)");
        comp_hint(&st->cur, &st->theme, sy++, cols, line);
        AgcBuf eb = {0};
        agentc_buf_cstr(&eb, "edit> ");
        agentc_buf_push(&eb, st->setting_edit_buf, st->setting_edit_cursor);
        agentc_buf_byte(&eb, '|');
        agentc_buf_cstr(&eb, st->setting_edit_buf + st->setting_edit_cursor);
        agentc_buf_byte(&eb, 0);
        comp_hint(&st->cur, &st->theme, sy++, cols, eb.p ? (const char *)eb.p : "edit> ");
        agentc_buf_free(&eb);
        if (st->setting_edit_err[0])
            agentc_snprintf(line, sizeof line, "error: %s   Esc cancels (fail-closed)",
                            st->setting_edit_err);
        else
            agentc_snprintf(line, sizeof line, "Enter saves (validated), Esc cancels, <-/-> move");
        comp_hint(&st->cur, &st->theme, sy, cols, line);
    }
    if (hint_h > 0)
        comp_hint(&st->cur, &st->theme, editor_y - 1, cols, "Press Ctrl-C again to exit");
    if (above_h > 0)
        comp_status_rows(&st->cur, &st->theme, editor_y - hint_h - above_h, cols,
                         AGENTC_REGION_ABOVE_COMPOSER, st->status_segs, st->status_n);
    editor_render(&st->ed, &st->cur, &st->theme, 0, editor_y, cols, editor_h, placeholder,
                  &st->cursor_x, &st->cursor_y);
    if (below_h > 0)
        comp_status_rows(&st->cur, &st->theme, editor_y + editor_h, cols,
                         AGENTC_REGION_BELOW_COMPOSER, st->status_segs, st->status_n);
    if (queue_y >= 0)
        comp_queue(&st->cur, &st->theme, queue_y, cols, st->nq, st->queue[0]);
    comp_footer(&st->cur, &st->theme, status_y, cols, st->status_segs, st->status_n);
    if (below_status_h > 0)
        comp_status_rows(&st->cur, &st->theme, status_y + 1, cols, AGENTC_REGION_BELOW_STATUS,
                         st->status_segs, st->status_n);

    if (st->cursor_x >= cols)
        st->cursor_x = cols - 1;
    if (st->cursor_x < 0)
        st->cursor_x = 0;
    if (st->cursor_y >= rows)
        st->cursor_y = rows - 1;
    if (st->cursor_y < 0)
        st->cursor_y = 0;
    Cell *cc = grid_at(&st->cur, st->cursor_x, st->cursor_y);
    st->cursor_reversed = cc && (cc->attrs & A_REVERSE) != 0;
}

/* The configured (not transient) thinking level. The core accessor is the
 * source of truth for a live agent; the level tracked here keeps the status
 * line and /thinking report correct in the no-agent test harness. */
static const char *tui_thinking_name(const Tui *st) {
    if (st->agent) {
        const char *n = agentc_agent_thinking(st->agent);
        if (n)
            return n;
    }
    switch (st->thinking_level) {
    case 1:
    case 2:
        return "low";
    case 3:
        return "medium";
    case 4:
        return "high";
    default:
        return "off";
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
        agentc_snprintf(s.model, sizeof s.model, "%s", st->model && st->model[0] ? st->model : "?");
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
                if (m->usage.cost_micro < 0)
                    cost_known = false;
                else
                    cost += m->usage.cost_micro;
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
            if (!cm || agentc_model_cost(cm, &zero) < 0)
                s.cost_micro = -1;
        }
    }
    s.running = st->running;
    s.spinner = st->spinner;
    s.elapsed_ms = st->running ? tui_now_ms(st) - st->run_started_ms : 0;
    agentc_status_builtin_set(&s);
    if (agentc_status_version() != st->status_version) {
        st->status_n = agentc_status_snapshot(AGENTC_STATUS_ALL_REGIONS, st->status_segs,
                                              AGENTC_STATUS_MAX_SEGMENTS);
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

static void tui_frame(Tui *st) {
    tui_flush_pending(st);
    if (st->cur.cols != st->prev.cols || st->cur.rows != st->prev.rows)
        grid_resize(&st->prev, st->cur.cols, st->cur.rows);
    tui_draw(st);
    AgcBuf out = {0};
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

/* Emit one replayed notice line: status notices already end in a newline; add
 * one when a persisted text does not, so the chat rows stay aligned. */
static void tui_replay_notice(void *ud, const char *text) {
    Tui *st = ud;
    AgcBuf b = {0};
    agentc_buf_cstr(&b, text ? text : "");
    if (b.len == 0 || b.p[b.len - 1] != '\n')
        agentc_buf_byte(&b, '\n');
    chat_append_notice(&st->chat, (const char *)b.p, b.len);
    agentc_buf_free(&b);
}

/* A session swap (new or resume) replaces the agent transcript: clear the view
 * so the next frame repaints from scratch. Without this an equal-height
 * replacement would keep the old state on screen. Persisted `custom notice`
 * rows are replayed into the fresh view so a resumed session keeps its status
 * history (P6 completion pass 2c). */
static void tui_after_session_swap(Tui *st) {
    chat_clear(&st->chat);
    editor_clear(&st->ed);
    st->msg_mark = 0;
    st->msg_mark_set = false;
    agentc_buf_clear(&st->pend_text);
    agentc_buf_clear(&st->pend_think);
    agentc_buf_clear(&st->tool_args);
    st->pend_order = 0;
    st->chat.scroll = 0;
    if (st->app && st->app->replay_notices)
        st->app->replay_notices(st->app->ud, tui_replay_notice, st);
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
    tui_noticef(st, swap ? "new: started a new session\n" : "/new: chat view cleared\n", NULL);
    return true;
}

/* /fork: branch the live conversation into a new session file (transcript
 * kept). Refused while a run is in flight, like /new. */
static bool cmd_fork(Tui *st, const char *args) {
    (void)args;
    if (st->running) {
        tui_noticef(st, "fork: wait for the current run to finish\n", NULL);
        return true;
    }
    if (!st->app || !st->app->fork_session) {
        tui_noticef(st, "fork: session storage is not available\n", NULL);
        return true;
    }
    if (st->app->fork_session(st->app->ud) != 0) {
        tui_noticef(st, "fork: cannot fork the session\n", NULL);
        return true;
    }
    /* The transcript is kept, so only repaint; no chat clear. */
    tui_noticef(st, "fork: forked to a new session\n", NULL);
    tui_dirty(st);
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

/* /tree: open the session fork forest. Sessions are labelled with an indent by
 * lineage depth; selecting one resumes it (the shared session-picker action). */
static bool cmd_tree(Tui *st, const char *args) {
    (void)args;
    if (!st->app || !st->app->resume_session) {
        tui_noticef(st, "tree: session storage is not available\n", NULL);
        return true;
    }
    if (st->running) {
        tui_noticef(st, "tree: wait for the current run to finish\n", NULL);
        return true;
    }
    if (!tui_tree_pick_open(st))
        tui_noticef(st, "tree: no stored sessions\n", NULL);
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
                               "- /model [id|alias], /theme [dark|light]\n"
                               "- /thinking [level]; blank opens a picker\n"
                               "- /settings [ext.key value]\n"
                               "- /compact compact the context\n"
                               "- /resume /continue switch session\n"
                               "- /fork branch into a new session\n"
                               "- /tree browse the session fork tree\n"
                               "- /quit /new /clear /help\n";
    chat_append_notice(&st->chat, help, agentc_strlen(help));
    return true;
}

static bool cmd_model(Tui *st, const char *args) {
    AgcBuf b = {0};
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
        /* A virtual model (alias) is provider-independent: its row's provider is
         * the alias itself, so the provider-scoped lookups below would misread it
         * as another provider's model and point at a restart that cannot work.
         * Set it directly; step routing resolves the concrete target per request
         * (design/40 §6.1). */
        const AgcModel *vm = agentc_model_find_virtual(args);
        const AgcModel *own = vm ? NULL : agentc_model_find(cur, args);
        /* A concrete model id normally exists on one provider. If this one is not
         * in the current provider's catalog but is another provider's, refuse
         * rather than silently point the current endpoint at it (which would just
         * 404). The TUI cannot switch provider in place; point at the restart
         * path. */
        const AgcModel *other = own ? NULL : agentc_model_find(NULL, args);
        if (vm) {
            if (agentc_agent_set_model(st->agent, args) == 0) {
                agentc_buf_printf(&b, "model: %s (alias -> %s/%s)\n", args,
                                  vm->route_provider ? vm->route_provider : "?",
                                  vm->route_model ? vm->route_model : "?");
            } else {
                agentc_buf_printf(&b, "cannot switch to '%s'\n", args);
            }
        } else if (other && other->provider && !agentc_streq(other->provider, cur)) {
            agentc_buf_printf(&b,
                              "model: '%s' belongs to provider '%s', not '%s'\n"
                              "       restart with `agentc --provider %s` or run `agentc setup`\n",
                              args, other->provider, cur[0] ? cur : "?", other->provider);
        } else if (agentc_agent_set_model(st->agent, args) == 0) {
            agentc_buf_printf(&b, "model: %s\n", args);
            if (!own)
                agentc_buf_printf(
                    &b, "note: '%s' is not in the %s catalog; the provider may reject it\n", args,
                    cur[0] ? cur : "current");
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
        grid_invalidate(&st->prev); /* colours changed: repaint everything */
        const char *tnote = st->theme.dark ? "theme: dark\n" : "theme: light\n";
        chat_append_notice(&st->chat, tnote, agentc_strlen(tnote));
        return true;
    }
    char name[AGENTC_THEME_NAME_MAX + 1];
    size_t n = 0;
    while (args[n] && args[n] != ' ' && args[n] != '\t' && args[n] != '\n' && n < sizeof name - 1) {
        name[n] = args[n];
        n++;
    }
    name[n] = 0;
    AgcBuf b = {0};
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
    if (!s)
        return -1;
    if (agentc_streq(s, "off"))
        return 0;
    if (agentc_streq(s, "low"))
        return 1;
    if (agentc_streq(s, "medium"))
        return 3;
    if (agentc_streq(s, "high"))
        return 4;
    return -1;
}

static bool cmd_thinking(Tui *st, const char *args) {
    char word[24];
    size_t n = 0;
    while (args[n] && args[n] != ' ' && args[n] != '\t' && args[n] != '\n' && n < sizeof word - 1) {
        word[n] = args[n];
        n++;
    }
    word[n] = 0;
    AgcBuf b = {0};
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
            if (st->agent)
                agentc_agent_set_thinking(st->agent, level);
            agentc_buf_printf(&b, "thinking: %s\n", tui_thinking_name(st));
        }
    }
    chat_append_notice(&st->chat, (const char *)b.p, b.len);
    agentc_buf_free(&b);
    return true;
}

/* ------------------------------------------------------------- /settings */

static const char *tui_setting_type_name(int type) {
    switch (type) {
    case AGENTC_SET_BOOL:
        return "bool";
    case AGENTC_SET_INT:
        return "int";
    case AGENTC_SET_FLOAT:
        return "float";
    case AGENTC_SET_STRING:
        return "string";
    case AGENTC_SET_ENUM:
        return "enum";
    case AGENTC_SET_PATH:
        return "path";
    case AGENTC_SET_LIST:
        return "list";
    default:
        return "?";
    }
}

/* Secret display rule: never print the value, only U+2026 followed by the last
 * four bytes. `json` is the resolved JSON value (a quoted string for a SECRET). */
static void tui_secret_last4(const char *json, char *out, size_t cap) {
    const char *s = json ? json : "";
    size_t n = agentc_strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s++;
        n -= 2;
    }
    const char *dots = "\xE2\x80\xA6"; /* U+2026 HORIZONTAL ELLIPSIS */
    if (n <= 4) {
        agentc_snprintf(out, cap, "%s", dots);
        return;
    }
    agentc_snprintf(out, cap, "%s", dots);
    size_t used = agentc_strlen(out);
    if (used + 4 < cap) {
        agentc_memcpy(out + used, s + n - 4, 4);
        out[used + 4] = 0;
    }
}

/* Wrap a raw user value as JSON: pass through a JSON literal (bool/null/number/
 * quoted string/array/object), otherwise quote it as a string. The settings
 * store validates strictly, so an enum/secret/string value must carry quotes. */
static void tui_setting_value_json(const char *raw, AgcBuf *out) {
    const char *r = raw ? raw : "";
    char c = r[0];
    bool literal = c == '"' || c == '[' || c == '{' || c == '-' || (c >= '0' && c <= '9') ||
                   agentc_streq(r, "true") || agentc_streq(r, "false") || agentc_streq(r, "null");
    if (literal) {
        agentc_buf_cstr(out, r);
        return;
    }
    agentc_buf_byte(out, '"');
    agentc_json_escape(out, r, agentc_strlen(r));
    agentc_buf_byte(out, '"');
}

/* Cycle an ENUM value to the next entry in its `enum_json` array (wrapping). */
static bool enum_next(const char *enum_json, const char *cur, char *out, size_t cap) {
    const char *p = enum_json;
    char first[64] = "";
    char prev[64] = "";
    bool have = false;
    while (p && *p) {
        const char *q = agentc_str_str(p, "\"");
        if (!q)
            break;
        q++;
        const char *e = q;
        while (*e && *e != '"')
            e++;
        size_t n = (size_t)(e - q);
        if (n >= sizeof prev)
            n = sizeof prev - 1;
        char val[64];
        agentc_memcpy(val, q, n);
        val[n] = 0;
        if (!first[0])
            agentc_snprintf(first, sizeof first, "%s", val);
        if (have) {
            agentc_snprintf(out, cap, "%s", val);
            return true;
        }
        if (cur && agentc_streq(cur, val))
            have = true;
        agentc_snprintf(prev, sizeof prev, "%s", val);
        p = *e ? e + 1 : e;
    }
    (void)prev;
    if (have && first[0]) { /* wrapped past the last entry */
        agentc_snprintf(out, cap, "%s", first);
        return true;
    }
    return false;
}

/* Apply one picker selection: bool toggles, enum cycles, anything else opens
 * the single-value editor modal. */
static void tui_setting_apply(Tui *st, const AgcSettingInfo *info) {
    AgcBuf b = {0};
    if (info->type == AGENTC_SET_BOOL) {
        const char *cur = agentc_settings_get_json(info->ext, info->key);
        const char *next = (cur && agentc_streq(cur, "true")) ? "false" : "true";
        int rc = agentc_settings_set(info->ext, info->key, next);
        agentc_buf_printf(&b, "settings: %s.%s = %s%s\n", info->ext, info->key, next,
                          rc ? " (rejected)" : "");
    } else if (info->type == AGENTC_SET_ENUM) {
        const char *cur = agentc_settings_get_json(info->ext, info->key);
        char next[64];
        if (enum_next(info->enum_json, cur, next, sizeof next)) {
            int rc = agentc_settings_set(info->ext, info->key, next);
            agentc_buf_printf(&b, "settings: %s.%s = %s%s\n", info->ext, info->key, next,
                              rc ? " (rejected)" : "");
        } else {
            agentc_buf_printf(&b, "settings: %s.%s has no options\n", info->ext, info->key);
        }
    } else {
        /* A string/int/float/path/list row opens the single-value editor modal
         * instead of printing the argument-form hint. */
        tui_setting_edit_open(st, info);
        return;
    }
    chat_append_notice(&st->chat, (const char *)b.p, b.len);
    agentc_buf_free(&b);
}

/* Convert the editor buffer to the JSON the settings store validates. A
 * string/path/enum value is always quoted (so a numeric-looking text stays a
 * string); every other type uses the shared literal/quote rule. */
static void tui_setting_edit_json(const Tui *st, AgcBuf *out) {
    if (st->setting_edit_type == AGENTC_SET_STRING || st->setting_edit_type == AGENTC_SET_PATH ||
        st->setting_edit_type == AGENTC_SET_ENUM) {
        agentc_buf_byte(out, '"');
        agentc_json_escape(out, st->setting_edit_buf, st->setting_edit_len);
        agentc_buf_byte(out, '"');
        return;
    }
    tui_setting_value_json(st->setting_edit_buf, out);
}

/* Open the single-value editor for one declared setting. The buffer is prefilled
 * with the resolved value; a SECRET is never prefilled (the preview shows only
 * `…last4`). */
static void tui_setting_edit_open(Tui *st, const AgcSettingInfo *info) {
    agentc_snprintf(st->setting_edit_ext, sizeof st->setting_edit_ext, "%s", info->ext);
    agentc_snprintf(st->setting_edit_key, sizeof st->setting_edit_key, "%s", info->key);
    st->setting_edit_type = info->type;
    st->setting_edit_flags = info->flags;
    st->setting_edit_cur[0] = 0;
    st->setting_edit_buf[0] = 0;
    st->setting_edit_len = 0;
    st->setting_edit_cursor = 0;
    st->setting_edit_err[0] = 0;
    const char *cur = agentc_settings_get_json(info->ext, info->key);
    if (cur) {
        if (info->flags & AGENTC_SETTING_SECRET) {
            tui_secret_last4(cur, st->setting_edit_cur, sizeof st->setting_edit_cur);
        } else {
            agentc_snprintf(st->setting_edit_cur, sizeof st->setting_edit_cur, "%s", cur);
            const char *s = cur;
            size_t n = agentc_strlen(s);
            if ((info->type == AGENTC_SET_STRING || info->type == AGENTC_SET_PATH ||
                 info->type == AGENTC_SET_ENUM) &&
                n >= 2 && s[0] == '"' && s[n - 1] == '"') {
                s++;
                n -= 2;
            }
            if (n >= sizeof st->setting_edit_buf)
                n = sizeof st->setting_edit_buf - 1;
            agentc_memcpy(st->setting_edit_buf, s, n);
            st->setting_edit_buf[n] = 0;
            st->setting_edit_len = n;
            st->setting_edit_cursor = n;
        }
    }
    st->setting_edit_open = true;
    tui_dirty(st);
}

static void tui_setting_edit_close(Tui *st) {
    st->setting_edit_open = false;
    st->setting_edit_ext[0] = 0;
    st->setting_edit_key[0] = 0;
    st->setting_edit_cur[0] = 0;
    st->setting_edit_buf[0] = 0;
    st->setting_edit_len = 0;
    st->setting_edit_cursor = 0;
    st->setting_edit_err[0] = 0;
    tui_dirty(st);
}

/* The editor modal owns the keyboard while open. Enter validates through the
 * settings store and only closes on success; an invalid value stays open with
 * a reason (fail-safe). Escape cancels. */
static void tui_setting_edit_key(Tui *st, const Key *k) {
    if (k->code == K_ESC) {
        tui_setting_edit_close(st);
        return;
    }
    if (k->code == K_ENTER) {
        if (st->setting_edit_len == 0) {
            agentc_snprintf(st->setting_edit_err, sizeof st->setting_edit_err,
                            "a value is required");
            tui_dirty(st);
            return;
        }
        AgcBuf vj = {0};
        tui_setting_edit_json(st, &vj);
        int rc = agentc_settings_set(st->setting_edit_ext, st->setting_edit_key,
                                     vj.p ? (const char *)vj.p : "null");
        agentc_buf_free(&vj);
        if (rc == 0) {
            AgcBuf nb = {0};
            agentc_buf_printf(&nb, "settings: %s.%s updated\n", st->setting_edit_ext,
                              st->setting_edit_key);
            chat_append_notice(&st->chat, (const char *)nb.p, nb.len);
            agentc_buf_free(&nb);
            tui_setting_edit_close(st);
        } else if (rc == -2) {
            agentc_snprintf(st->setting_edit_err, sizeof st->setting_edit_err, "unknown setting");
            tui_dirty(st);
        } else {
            agentc_snprintf(st->setting_edit_err, sizeof st->setting_edit_err,
                            "invalid or rejected value");
            tui_dirty(st);
        }
        return;
    }
    if (k->code == K_LEFT) {
        if (st->setting_edit_cursor)
            st->setting_edit_cursor--;
        tui_dirty(st);
        return;
    }
    if (k->code == K_RIGHT) {
        if (st->setting_edit_cursor < st->setting_edit_len)
            st->setting_edit_cursor++;
        tui_dirty(st);
        return;
    }
    if (k->code == K_HOME) {
        st->setting_edit_cursor = 0;
        tui_dirty(st);
        return;
    }
    if (k->code == K_END) {
        st->setting_edit_cursor = st->setting_edit_len;
        tui_dirty(st);
        return;
    }
    if (k->code == K_BACKSPACE) {
        if (st->setting_edit_cursor) {
            size_t c = st->setting_edit_cursor;
            agentc_memmove(st->setting_edit_buf + c - 1, st->setting_edit_buf + c,
                           st->setting_edit_len - c + 1);
            st->setting_edit_cursor--;
            st->setting_edit_len--;
        }
        tui_dirty(st);
        return;
    }
    if (k->code == K_DELETE) {
        if (st->setting_edit_cursor < st->setting_edit_len) {
            size_t c = st->setting_edit_cursor;
            agentc_memmove(st->setting_edit_buf + c, st->setting_edit_buf + c + 1,
                           st->setting_edit_len - c);
            st->setting_edit_len--;
        }
        tui_dirty(st);
        return;
    }
    if (k->code == K_CHAR && k->cp >= 0x20 && k->cp < 0x7f &&
        st->setting_edit_len + 1 < sizeof st->setting_edit_buf) {
        size_t c = st->setting_edit_cursor;
        agentc_memmove(st->setting_edit_buf + c + 1, st->setting_edit_buf + c,
                       st->setting_edit_len - c + 1);
        st->setting_edit_buf[c] = (char)k->cp;
        st->setting_edit_cursor++;
        st->setting_edit_len++;
        tui_dirty(st);
    }
}

static bool cmd_settings(Tui *st, const char *args) {
    while (*args == ' ' || *args == '\t')
        args++;
    if (!args[0]) {
        if (tui_settings_pick_open(st)) {
            tui_dirty(st);
        } else {
            tui_noticef(st, "settings: none declared\n", NULL);
        }
        return true;
    }
    const char *sp = args;
    while (*sp && *sp != ' ' && *sp != '\t')
        sp++;
    char path[128];
    size_t plen = (size_t)(sp - args);
    if (plen >= sizeof path)
        plen = sizeof path - 1;
    agentc_memcpy(path, args, plen);
    path[plen] = 0;
    char *dot = NULL;
    for (char *p = path; *p; p++)
        if (*p == '.') {
            dot = p;
            break;
        }
    AgcBuf b = {0};
    if (!dot || dot == path || dot[1] == 0) {
        agentc_buf_printf(&b, "settings: expected <ext>.<key>, got '%s'\n", path);
        chat_append_notice(&st->chat, (const char *)b.p, b.len);
        agentc_buf_free(&b);
        return true;
    }
    *dot = 0;
    const char *ext = path, *key = dot + 1;
    while (*sp == ' ' || *sp == '\t')
        sp++;
    if (!*sp) {
        const char *cur = agentc_settings_get_json(ext, key);
        if (!cur) {
            agentc_buf_printf(&b, "settings: unknown %s.%s\n", ext, key);
        } else if (agentc_settings_is_set(ext, key)) {
            char val[48];
            size_t total = agentc_settings_list(NULL, 0);
            AgcSettingInfo *info = agentc_alloc((total ? total : 1) * sizeof *info);
            total = agentc_settings_list(info, total);
            bool secret = false;
            for (size_t i = 0; i < total; i++)
                if (agentc_streq(info[i].ext, ext) && agentc_streq(info[i].key, key))
                    secret = (info[i].flags & AGENTC_SETTING_SECRET) != 0;
            agentc_free(info);
            if (secret)
                tui_secret_last4(cur, val, sizeof val);
            else
                agentc_snprintf(val, sizeof val, "%s", cur);
            agentc_buf_printf(&b, "settings: %s.%s = %s\n", ext, key, val);
        } else {
            agentc_buf_printf(&b, "settings: %s.%s = default\n", ext, key);
        }
    } else {
        AgcBuf vj = {0};
        tui_setting_value_json(sp, &vj);
        int rc = agentc_settings_set(ext, key, vj.p ? (const char *)vj.p : "null");
        if (rc == -2)
            agentc_buf_printf(&b, "settings: unknown %s.%s\n", ext, key);
        else if (rc != 0)
            agentc_buf_printf(&b, "settings: %s.%s rejected\n", ext, key);
        else
            agentc_buf_printf(&b, "settings: %s.%s updated\n", ext, key);
        agentc_buf_free(&vj);
    }
    chat_append_notice(&st->chat, (const char *)b.p, b.len);
    agentc_buf_free(&b);
    return true;
}

static const TuiCommand tui_commands[] = {
    {"clear", "clear the transcript view", cmd_clear},
    {"help", "show key bindings and slash commands", cmd_help},
    {"model", "switch model or alias (picker with no argument)", cmd_model},
    {"new", "start a new session", cmd_new},
    {"quit", "exit agentc", cmd_quit},
    {"settings", "edit extension settings (picker with no argument)", cmd_settings},
    {"theme", "switch theme (dark|light|<name>)", cmd_theme},
    {"thinking", "set reasoning level (picker with no argument)", cmd_thinking},
    {"compact", "compact the conversation context", cmd_compact},
    {"fork", "branch the conversation into a new session", cmd_fork},
    {"tree", "browse the session fork tree", cmd_tree},
    {"resume", "switch to another session", cmd_resume},
    {"continue", "switch to another session", cmd_resume},
};

/* The menu is open while the composer holds an unterminated command word: text
 * starts with '/' and contains no space/newline yet. Returns false (menu
 * closed) in the argument phase and for over-long input. */
static bool tui_menu_word(const Editor *ed, char *out, size_t cap) {
    const char *s = editor_text(ed);
    if (s[0] != '/')
        return false;
    size_t i = 1;
    while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != '\n') {
        if (i >= cap)
            return false;
        out[i - 1] = s[i];
        i++;
    }
    if (s[i])
        return false;
    out[i - 1] = 0;
    return true;
}

static bool tui_menu_prefix(const char *name, const char *filter) {
    for (size_t i = 0; filter[i]; i++)
        if (name[i] != filter[i])
            return false;
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
        if (agentc_streq(tui_commands[i].name, name))
            return true;
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
        if (!tui_menu_prefix(tui_commands[i].name, filter))
            continue;
        if (n < TUI_MENU_MAX) {
            st->menu_name[n] = tui_commands[i].name;
            st->menu_desc[n] = tui_commands[i].description;
        }
        n++;
    }
    AgcPromptInfo prompts[TUI_MENU_MAX];
    size_t np = agentc_prompts_list(prompts, TUI_MENU_MAX);
    for (size_t i = 0; i < np; i++) {
        if (tui_is_builtin(prompts[i].name) || tui_reserved_prefix(prompts[i].name))
            continue;
        if (!tui_menu_prefix(prompts[i].name, filter))
            continue;
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
        if (!tui_menu_prefix(cmds[i].name, filter))
            continue;
        if (n < TUI_MENU_MAX) {
            st->menu_name[n] = cmds[i].name;
            st->menu_desc[n] = cmds[i].description;
        }
        n++;
    }
    if (n > TUI_MENU_MAX)
        n = TUI_MENU_MAX;
    st->menu_n = n;
    if (st->menu_sel >= n)
        st->menu_sel = n ? n - 1 : 0;
    st->menu_open = n > 0 && !st->menu_dismissed;
}

/* Rows the menu wants for an available row budget: never more than the cap,
 * never more than the frame can afford. Zero means the menu is closed or the
 * terminal is too short even for one row, in which case it is dropped. */
static int tui_menu_height(Tui *st, int avail) {
    if (!st->menu_open || st->menu_n == 0 || avail <= 0)
        return 0;
    size_t n = st->menu_n;
    if (n > TUI_MENU_VISIBLE_MAX)
        n = TUI_MENU_VISIBLE_MAX;
    if (n > (size_t)avail)
        n = (size_t)avail;
    return (int)n;
}

/* Keep the selected entry inside the visible window by scrolling the list. */
static void tui_menu_scroll(Tui *st, int menu_h) {
    if (menu_h <= 0)
        return;
    if (st->pick_open) {
        picklist_scroll(&st->pick, menu_h);
        st->menu_sel = st->pick.sel;
        st->menu_top = st->pick.top;
        return;
    }
    if (st->menu_sel < st->menu_top)
        st->menu_top = st->menu_sel;
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
        if (!agentc_streq(cmds[i].name, name))
            continue;
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
    if (!text)
        text = agentc_strdup("");
    if (st->running) {
        queue_push(st, text);
        agentc_free(text);
    } else {
        tui_submit(st, text);
    }
}

static void tui_noticef(Tui *st, const char *fmt, const char *arg) {
    AgcBuf b = {0};
    agentc_buf_printf(&b, fmt, arg);
    chat_append_notice(&st->chat, (const char *)b.p, b.len);
    agentc_buf_free(&b);
}

/* ------------------------------------------------------ model picker */

/* The four configured reasoning levels, in the order `/thinking` lists them.
 * `tui_thinking_descs` is the short description shown next to each row. */
static const char *const tui_thinking_levels[] = {"off", "low", "medium", "high"};
static const char *const tui_thinking_descs[] = {
    "no reasoning output",
    "brief reasoning",
    "balanced reasoning",
    "deep reasoning",
};

/* Publish the picker's visible rows through the command menu's fields so the
 * existing layout/render path draws it unchanged. */
static void tui_pick_mirror(Tui *st) {
    st->menu_sel = st->pick.sel;
    st->menu_top = st->pick.top;
    if (st->pick_open && st->pick.vn == 0) {
        /* A filter that matches nothing keeps one placeholder row: the pane
         * stays visible (and Esc/Backspace still reach the picker) instead of
         * disappearing while it silently swallows every key. */
        st->menu_n = 1;
        st->menu_name[0] = TUI_PICK_EMPTY;
        st->menu_desc[0] = "";
        st->menu_sel = 0;
        st->menu_top = 0;
        st->menu_open = true;
        return;
    }
    st->menu_n = st->pick.vn;
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

/* /model: every virtual-model (alias) row first (an alias is
 * provider-independent, so it is selectable from any agent; the row is
 * labelled with its concrete target), then the runtime catalog for the
 * current provider, restricted to the current model's wire so the two
 * `openai` rows never mix. Alias rows come first so a long provider catalog
 * cannot push them out of the row budget. */
static bool tui_model_pick_open(Tui *st) {
    if (!st->agent)
        return false;
    const AgcTranscript *tr = agentc_agent_transcript(st->agent);
    const char *provider = tr && tr->provider ? tr->provider : NULL;
    if (!provider || !provider[0])
        return false;
    const AgcModel *cm =
        (tr->model && tr->model[0]) ? agentc_model_find(provider, tr->model) : NULL;
    const char *api = cm ? cm->api : NULL;
    size_t n = 0;
    size_t sel = 0;

    /* aliases: the count-first enumeration covers the whole registry */
    size_t total = agentc_model_all(NULL, 0);
    const AgcModel **reg = agentc_alloc((total ? total : 1) * sizeof *reg);
    size_t reg_n = agentc_model_all(reg, total);
    for (size_t i = 0; i < reg_n && n < TUI_MENU_MAX; i++) {
        const AgcModel *vm = reg[i];
        if (!(vm->flags & AGENTC_MODEL_VIRTUAL))
            continue;
        agentc_snprintf(st->pick_id[n], sizeof st->pick_id[n], "%s", vm->id);
        agentc_snprintf(st->pick_desc[n], sizeof st->pick_desc[n], "alias -> %s/%s%s",
                        vm->route_provider ? vm->route_provider : "?",
                        vm->route_model ? vm->route_model : "?",
                        (tr->model && agentc_streq(tr->model, st->pick_id[n])) ? "  (current)" : "");
        st->pick_base_name[n] = st->pick_id[n];
        st->pick_base_desc[n] = st->pick_desc[n];
        if (tr->model && agentc_streq(tr->model, st->pick_id[n]))
            sel = n;
        n++;
    }
    agentc_free(reg);

    /* the provider's own rows fill the rest of the budget */
    const AgcModel *all[TUI_MENU_MAX];
    size_t pn = agentc_model_filter(provider, api, all, TUI_MENU_MAX - n);
    for (size_t i = 0; i < pn; i++) {
        agentc_snprintf(st->pick_id[n], sizeof st->pick_id[n], "%s", all[i]->id);
        agentc_snprintf(st->pick_desc[n], sizeof st->pick_desc[n], "%s%s  ctx=%u%s%s",
                        all[i]->provider ? all[i]->provider : "",
                        (tr->model && agentc_streq(tr->model, all[i]->id)) ? "  (current)" : "",
                        all[i]->ctx_window, all[i]->reasoning ? "  reasoning" : "",
                        all[i]->image ? "  image" : "");
        st->pick_base_name[n] = st->pick_id[n];
        st->pick_base_desc[n] = st->pick_desc[n];
        if (tr->model && agentc_streq(tr->model, st->pick_id[n]))
            sel = n;
        n++;
    }
    if (n == 0)
        return false;
    st->pick_paths_n = 0;
    tui_pick_start(st, TUI_MENU_MODEL, n, sel);
    return true;
}

/* /thinking: the four levels, current marked from the agent (the source of
 * truth, so --thinking at startup is reflected). */
static bool tui_thinking_pick_open(Tui *st) {
    if (!st->agent)
        return false;
    int cur = thinking_level_from_name(agentc_agent_thinking(st->agent));
    if (cur < 0)
        cur = st->thinking_level;
    for (size_t i = 0; i < 4; i++) {
        st->pick_base_name[i] = tui_thinking_levels[i];
        agentc_snprintf(st->pick_desc[i], sizeof st->pick_desc[i], "%s%s", tui_thinking_descs[i],
                        thinking_level_from_name(tui_thinking_levels[i]) == cur ? "  (current)"
                                                                                : "");
        st->pick_base_desc[i] = st->pick_desc[i];
    }
    st->pick_paths_n = 0;
    tui_pick_start(st, TUI_MENU_THINKING, 4, 0);
    return true;
}

/* /resume and /continue: stored sessions, newest first. `pick_paths` is owned
 * and freed on close. Returns false when there is nothing to list. */
static bool tui_session_pick_open(Tui *st) {
    if (!st->app || !st->app->resume_session)
        return false;
    tui_pick_free_paths(st);
    size_t sn = 0;
    char **paths = agentc_session_list(st->app->session_dir, &sn, TUI_MENU_MAX);
    if (!paths || sn == 0) {
        if (paths)
            agentc_sessions_free(paths, sn);
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
            if (stamps[j] > stamps[best])
                best = j;
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

/* /tree: the session fork forest, indented by lineage depth. Selecting a row
 * resumes that session through the same path as /resume. */
static bool tui_tree_pick_open(Tui *st) {
    if (!st->app || !st->app->resume_session)
        return false;
    tui_pick_free_paths(st);
    size_t total = agentc_session_tree(st->app->session_dir, NULL, 0);
    if (total == 0)
        return false;
    if (total > TUI_MENU_MAX)
        total = TUI_MENU_MAX;
    AgcSessionNode *nodes = agentc_alloc(total * sizeof *nodes);
    size_t n = agentc_session_tree(st->app->session_dir, nodes, total);
    i64 now = os_now_ns(OS_CLOCK_REALTIME) / 1000000;
    for (size_t i = 0; i < n; i++) {
        int depth = 0;
        const char *p = nodes[i].parent_id;
        for (int hop = 0; p && p[0] && hop < 16; hop++) {
            depth++;
            const char *next = NULL;
            for (size_t j = 0; j < n; j++)
                if (nodes[j].id && agentc_streq(nodes[j].id, p)) {
                    next = nodes[j].parent_id;
                    break;
                }
            p = next;
        }
        char ind[24];
        size_t w = 0;
        for (int d = 0; d < depth && w + 2 < sizeof ind; d++) {
            ind[w++] = ' ';
            ind[w++] = ' ';
        }
        ind[w] = 0;
        char age[48];
        agentc_session_age_label(nodes[i].timestamp_ms, now, age, sizeof age);
        i64 ts = 0;
        char preview[120];
        (void)agentc_session_summary(nodes[i].path, &ts, preview, sizeof preview);
        agentc_snprintf(st->pick_name[i], sizeof st->pick_name[i], "%s%s", ind,
                        nodes[i].id ? nodes[i].id : "?");
        agentc_snprintf(st->pick_desc[i], sizeof st->pick_desc[i], "%s %s", age,
                        preview[0] ? preview : "(empty session)");
        st->pick_paths[i] = agentc_strdup(nodes[i].path);
        st->pick_base_name[i] = st->pick_name[i];
        st->pick_base_desc[i] = st->pick_desc[i];
    }
    agentc_session_nodes_free(nodes, n);
    st->pick_paths_n = n;
    tui_pick_start(st, TUI_MENU_TREE, n, 0);
    return true;
}

/* /settings: every declared setting, with a masked value for secrets. Selecting
 * a bool toggles it and an enum cycles; the row action is applied in
 * tui_pick_key. */
static bool tui_settings_pick_open(Tui *st) {
    size_t total = agentc_settings_list(NULL, 0);
    if (total == 0)
        return false;
    AgcSettingInfo *info = agentc_alloc(total * sizeof *info);
    size_t n = agentc_settings_list(info, total);
    if (n > TUI_MENU_MAX)
        n = TUI_MENU_MAX;
    for (size_t i = 0; i < n; i++) {
        const char *cur = agentc_settings_get_json(info[i].ext, info[i].key);
        char val[40];
        if (info[i].flags & AGENTC_SETTING_SECRET)
            tui_secret_last4(cur, val, sizeof val);
        else
            agentc_snprintf(val, sizeof val, "%s", cur ? cur : "null");
        agentc_snprintf(st->pick_name[i], sizeof st->pick_name[i], "%s.%s", info[i].ext,
                        info[i].key);
        agentc_snprintf(st->pick_desc[i], sizeof st->pick_desc[i], "%s = %s%s",
                        tui_setting_type_name(info[i].type), val,
                        agentc_settings_is_set(info[i].ext, info[i].key) ? " *" : "");
        st->pick_base_name[i] = st->pick_name[i];
        st->pick_base_desc[i] = st->pick_desc[i];
    }
    agentc_free(info);
    st->pick_paths_n = 0;
    tui_pick_start(st, TUI_MENU_SETTING, n, 0);
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
                if (st->agent)
                    agentc_agent_set_thinking(st->agent, level);
                tui_noticef(st, "thinking: %s\n", tui_thinking_name(st));
            }
        } else if (st->menu_kind == TUI_MENU_SESSION || st->menu_kind == TUI_MENU_TREE) {
            size_t base = st->pick.vidx[st->pick.sel];
            const char *path = base < st->pick_paths_n ? st->pick_paths[base] : NULL;
            if (path && st->app && st->app->resume_session &&
                st->app->resume_session(st->app->ud, path) == 0) {
                tui_after_session_swap(st);
                tui_noticef(st, "resumed %s\n", name ? name : "");
            } else {
                tui_noticef(st, "resume: cannot open the session\n", NULL);
            }
        } else if (st->menu_kind == TUI_MENU_SETTING) {
            size_t total = agentc_settings_list(NULL, 0);
            AgcSettingInfo *info = agentc_alloc((total ? total : 1) * sizeof *info);
            total = agentc_settings_list(info, total);
            size_t idx = st->pick.vidx[st->pick.sel];
            if (idx < total)
                tui_setting_apply(st, &info[idx]);
            agentc_free(info);
        } else if (st->agent && name && agentc_agent_set_model(st->agent, name) == 0) {
            /* a virtual model (alias) reports its concrete route so the
             * selection is self-describing after the picker closes */
            const AgcModel *vm = agentc_model_find_virtual(name);
            if (vm) {
                AgcBuf b = {0};
                agentc_buf_printf(&b, "model: %s (alias -> %s/%s)\n", name,
                                  vm->route_provider ? vm->route_provider : "?",
                                  vm->route_model ? vm->route_model : "?");
                chat_append_notice(&st->chat, (const char *)b.p, b.len);
                agentc_buf_free(&b);
            } else {
                tui_noticef(st, "model: %s\n", name);
            }
        } else {
            tui_noticef(st, "cannot switch to '%s'\n", name ? name : "?");
        }
        tui_pick_close(st);
        tui_dirty(st);
        return true;
    }
    /* navigation or filtering: keep the command-menu view in sync (mirroring
     * also draws the empty-result placeholder). */
    tui_pick_mirror(st);
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
        AgcBuf b = {0};
        agentc_buf_printf(&b, "skill '%s' is too large (>%u KiB); not submitted\n", sn,
                          (unsigned)(AGENTC_SKILL_SUBMIT_MAX / 1024));
        chat_append_notice(&st->chat, (const char *)b.p, b.len);
        agentc_buf_free(&b);
        return true;
    }
    AgcBuf out = {0};
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
    if (!agentc_prompts_has(name))
        return false;
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
    if (text[0] != '/')
        return false;
    const char *name = text + 1;
    size_t nlen = 0;
    while (name[nlen] && name[nlen] != ' ' && name[nlen] != '\t' && name[nlen] != '\n')
        nlen++;
    const char *args = name + nlen;
    while (*args == ' ' || *args == '\t')
        args++;
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
    if (!handled)
        handled = tui_prompt_command(st, token, args);
    if (!handled)
        handled = tui_ext_command(st, token, args);
    if (token != stack)
        agentc_free(token);
    return handled;
}

/* ---------------------------------------------------------------- abort */

static void tui_abort(Tui *st) {
    if (!st->running) {
        editor_clear(&st->ed);
        tui_dirty(st);
        return;
    }
    if (st->agent)
        agentc_agent_abort(st->agent);
    st->abort_requested = true;
    if (st->nq) {
        AgcBuf back = {0};
        for (size_t i = 0; i < st->nq; i++) {
            if (i)
                agentc_buf_byte(&back, '\n');
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
            text = NULL; /* tui_submit took ownership */
        }
    }
    agentc_free(text);
    tui_dirty(st);
}

/* ---------------------------------------------------------------- input */

/* Copy `s` into `dst` as one bounded display line: control bytes collapse to a
 * space. Used to snapshot the borrowed request strings before the JSON arena
 * is freed. */
static void tui_approval_copy(char *dst, size_t cap, const char *s) {
    size_t n = 0;
    if (cap == 0)
        return;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p && n + 1 < cap; p++) {
        unsigned char c = *p;
        if (c < 0x20 || c == 0x7f)
            c = ' ';
        dst[n++] = (char)c;
    }
    dst[n] = 0;
}

/* The `approval` hook opens the modal and parks the request; the key handler
 * answers through `agentc_approval_submit`. A request with no id never opens a
 * modal (there is nothing to answer), so it falls through to the next approver
 * and ultimately the fail-closed deny. */
static int tui_approval_hook(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer) {
    Tui *st = ud;
    *next = m;
    if (!st || m == NULL || m->id == 0)
        return 0;
    const char *tool = "tool";
    const char *trigger = "approval";
    AgcBuf input = {0};
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *o = agentc_json_parse_in(a, m->body ? m->body : "{}", m->body_len);
    const char *t = agentc_json_get_str(o, "tool");
    const char *g = agentc_json_get_str(o, "trigger");
    if (t && t[0])
        tool = t;
    if (g && g[0])
        trigger = g;
    const AgcJson *in = agentc_json_get(o, "input");
    if (in && agentc_json_type(in) != AGENTC_JSON_NULL) {
        AgcJsonW iw;
        agentc_jsonw_init(&iw, &input);
        agentc_json_emit(&iw, in);
    }
    /* Snapshot the borrowed strings before the arena is freed: the Tui fields
     * must outlive this hook call. */
    tui_approval_copy(st->approval_tool, sizeof st->approval_tool, tool);
    tui_approval_copy(st->approval_trigger, sizeof st->approval_trigger, trigger);
    tui_approval_copy(st->approval_input, sizeof st->approval_input,
                      input.p ? (const char *)input.p : "");
    agentc_buf_free(&input);
    agentc_json_arena_free(a);
    st->approval_scope = AGENTC_SCOPE_ONCE;
    st->approval_remember = false;
    st->approval_open = true;
    st->approval_id = m->id;
    st->dirty = true;
    *defer = 1; /* park: the answer arrives out of band */
    return 0;
}

/* Build the §7.1 response for the current selection without clearing the
 * modal. `remember` (or a project scope) submits the persisted project scope;
 * a deny carries no scope (a deny is never memoized). */
static void tui_approval_build(Tui *st, bool allow, AgcBuf *out) {
    agentc_buf_cstr(out, "{\"id\":");
    agentc_buf_u64(out, st->approval_id);
    if (!allow) {
        agentc_buf_cstr(out, ",\"decision\":\"deny\"}");
        return;
    }
    int scope = st->approval_remember ? AGENTC_SCOPE_PROJECT : st->approval_scope;
    const char *name = "once";
    if (scope == AGENTC_SCOPE_STEP)
        name = "step";
    else if (scope == AGENTC_SCOPE_SESSION)
        name = "session";
    else if (scope == AGENTC_SCOPE_PROJECT)
        name = "project";
    agentc_buf_cstr(out, ",\"decision\":\"allow\",\"scope\":\"");
    agentc_buf_cstr(out, name);
    agentc_buf_cstr(out, "\"}");
}

static void tui_approval_answer(Tui *st, bool allow) {
    AgcBuf resp = {0};
    tui_approval_build(st, allow, &resp);
    (void)agentc_approval_submit(resp.p ? (const char *)resp.p : "{}");
    agentc_buf_free(&resp);
    st->approval_open = false;
    st->approval_id = 0;
    st->approval_tool[0] = 0;
    st->approval_trigger[0] = 0;
    st->approval_input[0] = 0;
    st->approval_scope = AGENTC_SCOPE_ONCE;
    st->approval_remember = false;
    tui_dirty(st);
}

static void tui_dialog_finish(Tui *st, bool ok, const char *answer) {
    st->dialog_done = true;
    st->dialog_ok = ok;
    agentc_snprintf(st->dialog_answer, sizeof st->dialog_answer, "%s", answer ? answer : "");
    tui_dirty(st);
}

/* The generic dialog owns the keyboard while open. Confirm answers y/n;
 * select answers with the highlighted option (arrows or 1..9); input/editor
 * are a one-line text field. Escape cancels (fail-closed). */
static void tui_dialog_key(Tui *st, const Key *k) {
    const char *m = st->dialog_method;
    if (k->code == K_ESC) {
        tui_dialog_finish(st, false, NULL);
        return;
    }
    if (agentc_streq(m, "confirm")) {
        if (k->code == K_CHAR && (k->cp == 'y' || k->cp == 'Y'))
            tui_dialog_finish(st, true, "true");
        else if (k->code == K_CHAR && (k->cp == 'n' || k->cp == 'N'))
            tui_dialog_finish(st, true, "false");
        else if (k->code == K_ENTER)
            tui_dialog_finish(st, true, st->dialog_deflt ? st->dialog_deflt : "false");
        else if (k->code == K_CHAR)
            tui_dialog_finish(st, true, "false");
        return;
    }
    if (agentc_streq(m, "select")) {
        if (st->dialog_nopts == 0) {
            tui_dialog_finish(st, false, NULL);
            return;
        }
        if (k->code == K_UP) {
            st->dialog_sel = st->dialog_sel ? st->dialog_sel - 1 : st->dialog_nopts - 1;
            tui_dirty(st);
            return;
        }
        if (k->code == K_DOWN) {
            st->dialog_sel = (st->dialog_sel + 1) % st->dialog_nopts;
            tui_dirty(st);
            return;
        }
        if (k->code == K_CHAR && k->cp >= '1' && k->cp <= '9') {
            size_t i = (size_t)(k->cp - '1');
            if (i < st->dialog_nopts)
                tui_dialog_finish(st, true, st->dialog_opt[i]);
            return;
        }
        if (k->code == K_ENTER)
            tui_dialog_finish(st, true, st->dialog_opt[st->dialog_sel]);
        return;
    }
    if (k->code == K_ENTER) {
        tui_dialog_finish(st, true, st->dialog_text);
        return;
    }
    if (k->code == K_BACKSPACE) {
        if (st->dialog_text_len)
            st->dialog_text[--st->dialog_text_len] = 0;
        tui_dirty(st);
        return;
    }
    if (k->code == K_CHAR && k->cp >= 0x20 && k->cp < 0x7f &&
        st->dialog_text_len + 1 < sizeof st->dialog_text) {
        st->dialog_text[st->dialog_text_len++] = (char)k->cp;
        st->dialog_text[st->dialog_text_len] = 0;
        tui_dirty(st);
    }
}

static void tui_key(void *ud, const Key *k) {
    Tui *st = ud;
    if (st->dialog_open) {
        tui_dialog_key(st, k);
        return;
    }
    if (st->approval_open) {
        /* Modal: y allows (with the selected scope); o/s/e/p pick the scope,
         * r toggles the persisted-decision choice; n, Esc, Enter or any other
         * key denies (fail-closed) and dismisses the overlay. */
        if (k->code == K_CHAR) {
            switch (k->cp) {
            case 'y':
            case 'Y':
                tui_approval_answer(st, true);
                return;
            case 'o':
            case 'O':
                st->approval_scope = AGENTC_SCOPE_ONCE;
                st->approval_remember = false;
                tui_dirty(st);
                return;
            case 's':
            case 'S':
                st->approval_scope = AGENTC_SCOPE_STEP;
                st->approval_remember = false;
                tui_dirty(st);
                return;
            case 'e':
            case 'E':
                st->approval_scope = AGENTC_SCOPE_SESSION;
                st->approval_remember = false;
                tui_dirty(st);
                return;
            case 'p':
            case 'P':
                st->approval_scope = AGENTC_SCOPE_PROJECT;
                st->approval_remember = false;
                tui_dirty(st);
                return;
            case 'r':
            case 'R':
                st->approval_remember = !st->approval_remember;
                tui_dirty(st);
                return;
            default:
                break;
            }
        }
        tui_approval_answer(st, false);
        return;
    }
    if (st->setting_edit_open) {
        tui_setting_edit_key(st, k);
        return;
    }
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
            if (st->menu_n)
                st->menu_sel = st->menu_sel == 0 ? st->menu_n - 1 : st->menu_sel - 1;
            tui_dirty(st);
            return;
        }
        if (k->code == K_DOWN) {
            if (st->menu_n)
                st->menu_sel = st->menu_sel + 1 >= st->menu_n ? 0 : st->menu_sel + 1;
            tui_dirty(st);
            return;
        }
        if (k->code == K_TAB && fits) {
            agentc_snprintf(buf, sizeof buf, "/%s ", name);
            editor_set(&st->ed, buf);
            tui_menu_refresh(st); /* the trailing space closes the menu */
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
                if (now - st->last_ctrl_c_ms <= TUI_CTRL_C_WINDOW_MS)
                    st->quit = true;
                else
                    st->last_ctrl_c_ms = now;
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
            /* Toggle the last tool card. The fullscreen view is redrawn whole
             * each frame, so there is no append-only boundary to invalidate. */
            size_t ti = chat_last_tool_index(&st->chat);
            if (ti < st->chat.n)
                chat_toggle_last_tool(&st->chat);
            tui_dirty(st);
            return;
        }
    }
    if (k->code == K_ESC) {
        tui_abort(st);
        return;
    }
    if (k->code == K_MWHEEL_UP || k->code == K_MWHEEL_DOWN) {
        /* Mouse wheel: three lines per notch, mirroring PgUp/PgDn's direction. */
        chat_scroll(&st->chat, k->code == K_MWHEEL_UP ? 3 : -3);
        tui_dirty(st);
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
    if (!st->term)
        return;
    if (term_is_tty(st->term)) {
        u8 buf[1024];
        for (;;) {
            struct os_pollfd p = {0, OS_POLLIN, 0};
            if (os_poll(&p, 1, 0) <= 0)
                break;
            int n = term_read(st->term, buf, sizeof buf);
            if (n <= 0)
                break;
            tui_feed(st, buf, (size_t)n);
        }
    } else {
        u8 buf[1024];
        int n;
        while ((n = term_read(st->term, buf, sizeof buf)) > 0)
            tui_feed(st, buf, (size_t)n);
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
         * waiting in the pending buffers, so the replacement starts clean. */
        if (st->msg_mark_set) {
            if (st->msg_mark > st->chat.n)
                st->msg_mark = st->chat.n; /* defensively renormalize */
            chat_truncate(&st->chat, st->msg_mark);
            chat_seal(&st->chat); /* the replacement opens its own block */
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
            if (st->pend_order == 0)
                st->pend_order = 1;
            agentc_buf_push(&st->pend_text, d->text, d->len);
        }
        break;
    }
    case AGENTC_EV_THINK_DELTA: {
        /* The configured level gates the view: a reasoning model may stream
         * `reasoning_content` even with thinking off, but the transcript stays
         * answer-only so the footer ("think:off") and the visible output
         * agree. The agent transcript still records the block for replay. */
        if (agentc_streq(tui_thinking_name(st), "off"))
            break;
        const AgcTextDelta *d = data;
        if (d && d->text && d->len) {
            if (st->pend_order == 0)
                st->pend_order = 2;
            agentc_buf_push(&st->pend_think, d->text, d->len);
        }
        st->thinking = "on";
        break;
    }
    case AGENTC_EV_TOOL_ARGS_DELTA: {
        const AgcTextDelta *d = data;
        if (d && d->text && d->len)
            agentc_buf_push(&st->tool_args, d->text, d->len);
        break;
    }
    case AGENTC_EV_TOOL_EXEC_START: {
        tui_flush_pending(st);
        const AgcToolExec *e = data;
        if (!e)
            break;
        const char *args = e->args_json && e->args_json[0]
                               ? e->args_json
                               : (st->tool_args.p ? (const char *)st->tool_args.p : "");
        chat_tool_start(&st->chat, e->tool_name, args, tui_now_ms(st));
        agentc_buf_clear(&st->tool_args);
        break;
    }
    case AGENTC_EV_TOOL_EXEC_END: {
        const AgcToolExec *e = data;
        if (!e)
            break;
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
        if (ci && st->on_compact)
            st->on_compact(st->on_compact_ud, ci);
        break;
    }
    case AGENTC_EV_ERROR: {
        const char *msg = data;
        if (msg && msg[0]) {
            AgcBuf b = {0};
            agentc_buf_cstr(&b, "error: ");
            agentc_buf_cstr(&b, msg);
            agentc_buf_byte(&b, '\n');
            chat_append_error(&st->chat, (const char *)b.p, b.len);
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
    if (st->agent)
        agentc_agent_set_events(st->agent, tui_event, st);
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

struct AgcTuiTest {
    Tui st;
    AgcBuf screen;
};

AgcTuiTest *agentc_tui_test_new(int cols, int rows) {
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;
    Terminal *term = term_open_memory(cols, rows);
    AgcTuiTest *t = agentc_alloc(sizeof *t);
    agentc_memset(t, 0, sizeof *t);
    Tui *st = &t->st;
    agentc_memset(st, 0, sizeof *st);
    st->term = term;
    st->test = true;
    st->now_ms = 0;
    st->last_ctrl_c_ms = -100000;
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
    /* The memory backend cannot fail to enter raw mode. */
    (void)term_enter_mode(term, true);
    return t;
}

void agentc_tui_test_free(AgcTuiTest *t) {
    if (!t)
        return;
    Tui *st = &t->st;
    chat_free(&st->chat);
    editor_free(&st->ed);
    input_free(&st->in);
    grid_free(&st->cur);
    grid_free(&st->prev);
    agentc_buf_free(&st->tool_args);
    agentc_buf_free(&st->pend_text);
    agentc_buf_free(&st->pend_think);
    for (size_t i = 0; i < st->nq; i++)
        agentc_free(st->queue[i]);
    agentc_free(st->queue);
    agentc_buf_free(&t->screen);
    agentc_status_reset(); /* tests share one process: no provider leaks */
    term_close(st->term);
    agentc_free(t);
}

void agentc_tui_test_feed(AgcTuiTest *t, const char *bytes, size_t len) {
    if (!t || !bytes || !len)
        return;
    term_mem_feed(t->st.term, (const u8 *)bytes, len);
    tui_read_input(&t->st);
}

void agentc_tui_test_tick(AgcTuiTest *t, i64 delta_ms) {
    if (!t)
        return;
    t->st.now_ms += delta_ms;
    input_idle(&t->st.in, t->st.now_ms * 1000000, 50, tui_key, &t->st);
}

void agentc_tui_test_event(AgcTuiTest *t, int ev, const void *data) {
    if (t)
        tui_event(&t->st, ev, data);
}

void agentc_tui_test_submit(AgcTuiTest *t) {
    if (!t)
        return;
    char *text = editor_take(&t->st.ed);
    if (text[0])
        tui_submit(&t->st, text);
    else
        agentc_free(text);
}

void agentc_tui_test_set_compact_hook(AgcTuiTest *t, void (*fn)(void *ud, const AgcCompactInfo *ci),
                                      void *ud) {
    if (!t)
        return;
    t->st.on_compact = fn;
    t->st.on_compact_ud = ud;
}

/* The composer caret from the last layout: grid coordinates plus whether the
 * rendered fallback cell there carries A_REVERSE. The memory backend has no
 * hardware cursor, so this reverse cell is the observable caret and the test
 * compares it with the cursor-position sequences in the emitted frame. */
void agentc_tui_test_cursor(AgcTuiTest *t, int *x, int *y, bool *reverse) {
    if (x)
        *x = t ? t->st.cursor_x : 0;
    if (y)
        *y = t ? t->st.cursor_y : 0;
    if (reverse)
        *reverse = t && t->st.cursor_reversed;
}

u16 agentc_tui_test_cell_attrs(AgcTuiTest *t, int x, int y) {
    if (!t)
        return 0;
    Grid *g = &t->st.cur;
    Cell *c = grid_at(g, x, y);
    return c ? c->attrs : 0;
}

void agentc_tui_test_set_running(AgcTuiTest *t, bool on) {
    if (t)
        t->st.running = on;
}

void agentc_tui_test_approval_open(AgcTuiTest *t, uint64_t id, const char *request_json) {
    if (!t)
        return;
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_HOOK;
    m.kind = AGENTC_MSG_INPUT;
    m.id = id;
    m.body = request_json ? request_json : "{}";
    m.body_len = agentc_strlen(m.body);
    uint64_t defer = 0;
    AgcMessage *next = NULL;
    (void)tui_approval_hook(&t->st, &m, &next, &defer);
}

bool agentc_tui_test_approval_is_open(AgcTuiTest *t) {
    return t && t->st.approval_open;
}

int agentc_tui_test_approval_scope(AgcTuiTest *t) {
    return t ? t->st.approval_scope : -1;
}

bool agentc_tui_test_approval_remember(AgcTuiTest *t) {
    return t && t->st.approval_remember;
}

void agentc_tui_test_approval_build(AgcTuiTest *t, bool allow, AgcBuf *out) {
    if (t)
        tui_approval_build(&t->st, allow, out);
}

bool agentc_tui_test_setting_editor_open(AgcTuiTest *t) {
    return t && t->st.setting_edit_open;
}

const char *agentc_tui_test_setting_editor_text(AgcTuiTest *t) {
    return t ? t->st.setting_edit_buf : "";
}

/* Test-only: attach an agent so the model picker has a provider/catalog. The
 * caller retains ownership (the harness never frees it). */
void agentc_tui_test_set_agent(AgcTuiTest *t, AgcAgent *a) {
    if (t)
        t->st.agent = a;
}

void agentc_tui_test_set_app(AgcTuiTest *t, const AgcTuiApp *app) {
    if (t)
        t->st.app = app;
}

void agentc_tui_test_frame(AgcTuiTest *t) {
    if (!t)
        return;
    tui_flush_pending(&t->st);
    tui_frame(&t->st);
}

void agentc_tui_test_resize(AgcTuiTest *t, int cols, int rows) {
    if (!t)
        return;
    term_mem_resize(t->st.term, cols, rows);
    tui_check_resize(&t->st);
}

/* The composer's plain text (tests). */
const char *agentc_tui_test_editor_text(AgcTuiTest *t) {
    return t ? editor_text(&t->st.ed) : "";
}

const char *agentc_tui_test_screen(AgcTuiTest *t) {
    if (!t)
        return "";
    tui_flush_pending(&t->st);
    tui_draw(&t->st);
    agentc_buf_clear(&t->screen);
    render_screen_text(&t->st.cur, &t->screen);
    return t->screen.p ? (const char *)t->screen.p : "";
}

const char *agentc_tui_test_output(AgcTuiTest *t) {
    if (!t)
        return "";
    const AgcBuf *o = term_output(t->st.term);
    return o && o->p ? (const char *)o->p : "";
}

void agentc_tui_test_output_clear(AgcTuiTest *t) {
    if (t)
        term_output_clear(t->st.term);
}

bool agentc_tui_test_aborted(AgcTuiTest *t) {
    return t && t->st.abort_requested;
}

bool agentc_tui_test_running(AgcTuiTest *t) {
    return t && t->st.running;
}

bool agentc_tui_test_quit(AgcTuiTest *t) {
    return t && t->st.quit;
}

void agentc_tui_test_set_history(AgcTuiTest *t, const char *path) {
    if (!t || !path)
        return;
    agentc_snprintf(t->st.hist_path, sizeof t->st.hist_path, "%s", path);
    t->st.hist_set = true;
    editor_history_load(&t->st.ed, path);
}

void agentc_tui_test_set_footer(AgcTuiTest *t, const char *model, const char *thinking, u32 tok_in,
                                u32 tok_out, i64 cost_micro) {
    if (!t)
        return;
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
    if (t)
        t->st.show_tools = on;
}

/* Test-only: run the same startup banner printer agentc_tui_run uses, so a test
 * can assert its bytes precede the first frame without a tty. */
void agentc_tui_test_print_banner(AgcTuiTest *t) {
    if (t)
        tui_banner(&t->st);
}

/* ------------------------------------------------------------- main loop */

/* The poll hook must live at file scope (agentc_http only takes a function ptr). */
static void tui_poll_hook(void *ud, int timeout_ms) {
    agentc_mode_pump(NULL, timeout_ms); /* the shared extension pump duty */
    Tui *st = ud;
    tui_read_input(st);
    if (!st->test)
        st->now_ms = os_now_ns(OS_CLOCK_MONOTONIC) / 1000000;
    input_idle(&st->in, tui_now_ms(st) * 1000000, 50, tui_key, st);
    tui_check_resize(st);
    st->spinner = (int)((tui_now_ms(st) / 100) & 3);
    i64 now = tui_now_ms(st);
    /* Repaint once when the Ctrl-C hint lapses, so it does not linger past its
     * window on an otherwise idle screen. */
    bool hint = tui_ctrl_c_hint(st);
    if (hint != st->ctrl_c_hint) {
        st->ctrl_c_hint = hint;
        tui_dirty(st);
    }
    if (st->dirty || (st->running && now - st->last_frame_ms >= 100))
        tui_frame(st);
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
    if (test)
        theme_set_mode(&st->theme, THEME_TRUE);
    agentc_status_register_builtin();
    grid_init(&st->cur, term_cols(term), term_rows(term));
    grid_init(&st->prev, term_cols(term), term_rows(term));
    history_path(st->hist_path, sizeof st->hist_path);
    if (st->hist_path[0]) {
        if (!test) {
            /* mkdir -p of the parent directory */
            char dir[4096];
            agentc_snprintf(dir, sizeof dir, "%s", st->hist_path);
            char *slash = dir;
            while (*slash)
                slash++;
            while (slash > dir && slash[-1] != '/')
                slash--;
            if (slash > dir) {
                *slash = 0;
                mkdir_p(dir);
            }
        }
        editor_history_load(&st->ed, st->hist_path);
        st->hist_set = true;
    }
}

/* Fatal paths (out-of-memory via agentc_die, for example) must not leave the
 * user's terminal in raw mode on the alternate screen. */
static Terminal *g_term_cleanup;

/* N81: on exit from the alt screen, leave a COMPACT trace in the shell
 * scrollback instead of the whole transcript: the startup banner, then the
 * transcript up to the end of the FIRST human prompt, a blank / "..." / blank
 * separator, and the final non-empty output row. Nothing is emitted while the
 * alt screen is active; this runs after term_leave() restores the normal buffer,
 * and the rows are never redrawn (they are terminal-owned from here). An empty
 * session (no human prompt) leaves just the banner. */
static void tui_exit_dump(Tui *st) {
    if (!st || !st->term || st->test || !term_is_tty(st->term))
        return;
    int cols = term_cols(st->term);
    if (cols < 1)
        cols = 1;
    AgcBuf out = {0};
    theme_emit_sgr(&out, &st->theme, A_DIM, TH_MUTED, TH_NO_BG);
    agentc_buf_printf(&out, "agentc %s\r\n", AGENTC_VERSION);
    {
        const char *tools = st->show_tools ? agentc_tool_engine_summary() : NULL;
        if (tools && tools[0])
            agentc_buf_printf(&out, "tools: %s\r\n", tools);
    }

    /* End row of the first human prompt. With no human prompt the banner above
     * is the whole trace: dumping the transcript would only repeat the intro
     * header and print a dangling "...", so exit clean right here. */
    int total = chat_total_height(&st->chat);
    bool has_user = false;
    int cut = total, acc = 0;
    for (size_t i = 0; i < st->chat.n; i++) {
        int bh = chat_block_height(&st->chat.blocks[i]);
        if (st->chat.blocks[i].kind == CHAT_USER) {
            has_user = true;
            cut = acc + bh;
            break;
        }
        acc += bh;
    }
    if (has_user && total > 0) {
        agentc_buf_cstr(&out, "\x1b[0m\r\n"); /* blank line before the transcript */
        int maxrows = total > GRID_MAX_ROWS ? GRID_MAX_ROWS : total;
        Grid g;
        grid_init(&g, cols, maxrows);
        /* The first header_blocks blocks are the intro header, which the banner
         * above already printed: skip them so the trace has ONE header. */
        int hdr = 0;
        for (size_t i = 0; i < (size_t)st->header_blocks && i < st->chat.n; i++)
            hdr += chat_block_height(&st->chat.blocks[i]);
        for (int row = hdr; row < cut;) {
            int n = cut - row;
            if (n > maxrows)
                n = maxrows;
            grid_clear(&g, 0, TH_FG, TH_NO_BG);
            chat_render_rows(&st->chat, &g, &st->theme, cols, row, n, tui_now_ms(st), st->spinner);
            render_rows_ansi(&g, 0, n, &out, &st->theme, true);
            row += n;
        }
        /* The WHOLE last message (the last block with content), not just its
         * final row: an error/answer must be readable in full in the scrollback.
         * Only when something followed the first prompt; otherwise the prompt
         * would be reprinted under the "..." separator. */
        int last_from = total, acc2 = 0;
        for (size_t i = 0; i < st->chat.n; i++) {
            int bh = chat_block_height(&st->chat.blocks[i]);
            if (bh > 0)
                last_from = acc2;
            acc2 += bh;
        }
        if (last_from >= cut) {
            agentc_buf_cstr(&out, "\r\n...\r\n\r\n");
            for (int row = last_from; row < total;) {
                int n = total - row;
                if (n > maxrows)
                    n = maxrows;
                grid_clear(&g, 0, TH_FG, TH_NO_BG);
                chat_render_rows(&st->chat, &g, &st->theme, cols, row, n, tui_now_ms(st),
                                 st->spinner);
                render_rows_ansi(&g, 0, n, &out, &st->theme, true);
                row += n;
            }
        }
        grid_free(&g);
    }
    term_write(st->term, out.p, out.len);
    agentc_buf_free(&out);
}

static void tui_emergency_restore(void) {
    if (g_term_cleanup) {
        term_leave(g_term_cleanup); /* idempotent */
        g_term_cleanup = NULL;
    }
}

/* Async-signal-safe: only write()/ioctl() through term_leave, no allocation. */
static void tui_signal(int sig) {
    tui_emergency_restore();
    os_exit(128 + sig);
}

static void tui_free(Tui *st) {
    if (st->approval_hook)
        agentc_hook_off(st->approval_hook);
    chat_free(&st->chat);
    editor_free(&st->ed);
    input_free(&st->in);
    grid_free(&st->cur);
    grid_free(&st->prev);
    agentc_buf_free(&st->tool_args);
    agentc_buf_free(&st->pend_text);
    agentc_buf_free(&st->pend_think);
    for (size_t i = 0; i < st->nq; i++)
        agentc_free(st->queue[i]);
    agentc_free(st->queue);
}

/* ----------------------------------------------------- extension UI sink */
/* The extension host routes host->notify/host->set_title here while the TUI
 * owns the screen; print/json/rpc leave the sinks unset and keep the log
 * fallback. The terminal layer has no title operation, so set_title shares the
 * notice path (and a debug log). */
static void tui_ext_notice(Tui *st, const char *prefix, const char *text) {
    if (!st || !text)
        return;
    AgcBuf b = {0};
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
    if (title)
        agentc_logf(0, "ext title: %s", title);
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

/* Startup policy for the session picker and an explicit initial prompt. The
 * prompt runs immediately against whatever session the app already opened, so
 * the picker is skipped when a prompt is present: submitting under the modal
 * would either run the prompt before the picker resolves (the old bug) or hide
 * the modal behind a run that had already started. Without a prompt,
 * --continue/--resume offers the picker before the first frame (Escape keeps
 * the newest session the app opened). */
static void tui_startup(Tui *st, const char *initial_prompt) {
    if (initial_prompt && initial_prompt[0]) {
        char *text = agentc_strdup(initial_prompt);
        tui_submit(st, text);
    } else if (st->app && st->app->pick_session_on_start) {
        (void)tui_session_pick_open(st);
    }
}

/* Test-only: run the startup prompt/picker policy without the poll loop so a
 * test can assert the initial_prompt + pick_session_on_start ordering. */
void agentc_tui_test_startup(AgcTuiTest *t, const char *initial_prompt) {
    if (t)
        tui_startup(&t->st, initial_prompt);
}

/* The generic dialog sink (P5 C2). The core raises a select/confirm/input/editor
 * dialog from the pipeline; this opens the modal and pumps the TUI's own poll
 * hook (bounded) until the key handler answers. The test harness fails closed. */
static int tui_ui_prompt(void *ud, const char *method, const char *prompt,
                         const char *const *options, size_t noptions, const char *deflt, char *out,
                         size_t out_cap) {
    Tui *st = ud;
    if (!st || st->test || !method || !prompt)
        return -38;
    agentc_snprintf(st->dialog_method, sizeof st->dialog_method, "%s", method);
    agentc_snprintf(st->dialog_prompt, sizeof st->dialog_prompt, "%s", prompt);
    st->dialog_nopts = noptions > 8 ? 8 : noptions;
    for (size_t i = 0; i < st->dialog_nopts; i++)
        agentc_snprintf(st->dialog_opt[i], sizeof st->dialog_opt[i], "%s",
                        options && options[i] ? options[i] : "");
    st->dialog_sel = 0;
    st->dialog_text[0] = 0;
    st->dialog_text_len = 0;
    if (deflt && !agentc_streq(method, "confirm")) {
        agentc_snprintf(st->dialog_text, sizeof st->dialog_text, "%s", deflt);
        st->dialog_text_len = agentc_strlen(st->dialog_text);
    }
    st->dialog_deflt = deflt;
    st->dialog_answer[0] = 0;
    st->dialog_done = false;
    st->dialog_ok = false;
    st->dialog_open = true;
    tui_dirty(st);
    i64 deadline = tui_now_ms(st) + 30000;
    while (!st->dialog_done && !st->quit && tui_now_ms(st) < deadline)
        agentc_pump(10);
    int rc = -125; /* ECANCELED: fail-closed */
    if (st->dialog_done && st->dialog_ok) {
        size_t n = agentc_strlen(st->dialog_answer);
        if (out && out_cap) {
            if (n >= out_cap)
                n = out_cap - 1;
            agentc_memcpy(out, st->dialog_answer, n);
            out[n] = 0;
        }
        rc = 0;
    }
    st->dialog_open = false;
    st->dialog_done = false;
    st->dialog_deflt = NULL;
    tui_dirty(st);
    return rc;
}

int agentc_tui_run(AgcAgent *agent, const char *initial_prompt, const char *theme_name,
                   bool trusted, bool show_tools, const AgcTuiApp *app) {
    Terminal *term = term_open_tty();
    if (!term)
        return -1;
    Tui *st = agentc_alloc(sizeof *st);
    tui_init(st, term, agent, false);
    if (agent) {
        int lv = thinking_level_from_name(agentc_agent_thinking(agent));
        if (lv >= 0)
            st->thinking_level = lv;
    }
    st->show_tools = show_tools;
    st->app = app;
    if (app) {
        st->on_compact = app->on_compact;
        st->on_compact_ud = app->ud;
    }
    tui_set_project_theme_root(trusted);
    if (theme_name && theme_name[0])
        (void)theme_apply_named(&st->theme, theme_name);
    tui_sync_term_bg(st);
    AgcExtUiSink ui;
    agentc_memset(&ui, 0, sizeof ui);
    ui.notify = tui_ext_notify;
    ui.set_title = tui_ext_set_title;
    agentc_ext_set_ui_sink(&ui, st);
    AgcExtUiPromptFn prompt_prev = NULL;
    void *prompt_prev_ud = NULL;
    agentc_ext_get_ui_prompt(&prompt_prev, &prompt_prev_ud);
    agentc_ext_set_ui_prompt(tui_ui_prompt, st);
    tui_set_events(st);
    /* Async approval overlay: park the `approval` hook while the modal is
     * open, then answer with agentc_approval_submit. Registered only for the
     * interactive path, so the in-memory test harness goldens are unchanged. */
    st->approval_hook = agentc_hook(AGENTC_HOOK_STAGE_APPROVAL, 0, tui_approval_hook, st);
    /* Install the restore handlers before entering raw/alt mode: a signal that
     * lands between entering and registering would otherwise leave the terminal
     * raw. term_leave() is a no-op until the terminal is actually entered, so the
     * earlier install is safe. */
    g_term_cleanup = term;
    agentc_atexit(tui_emergency_restore);
    os_sig_install(OS_SIGHUP, tui_signal);
    os_sig_install(OS_SIGINT, tui_signal);
    os_sig_install(OS_SIGQUIT, tui_signal);
    os_sig_install(OS_SIGABRT, tui_signal);
    os_sig_install(OS_SIGBUS, tui_signal);
    os_sig_install(OS_SIGFPE, tui_signal);
    os_sig_install(OS_SIGSEGV, tui_signal);
    os_sig_install(OS_SIGTERM, tui_signal);
    int enter_rc = term_enter_mode(term, true);
    if (enter_rc != 0) {
        /* Raw mode failed: undo the process-global registration, drop any
         * background the theme pushed, and report instead of running against a
         * terminal we do not own. */
        g_term_cleanup = NULL;
        agentc_ext_set_ui_sink(NULL, NULL);
        agentc_ext_set_ui_prompt(prompt_prev, prompt_prev_ud);
        if (agent)
            agentc_agent_set_events(agent, NULL, NULL);
        term_reset_bg(term);
        tui_free(st);
        term_close(term);
        agentc_free(st);
        return enter_rc;
    }
    /* The intro header is the first transcript block: appended to the chat so
     * it renders in the alt screen and scrolls with the transcript. */
    tui_banner(st);
    tui_check_resize(st);
    tui_startup(st, initial_prompt);
    tui_frame(st);

    while (!st->quit) {
        struct os_pollfd p = {0, OS_POLLIN, 0};
        int pr = os_poll(&p, 1, 16);
        agentc_mode_pump(NULL, 0); /* idle tick: deferred extension work */
        if (pr > 0) {
            if (p.revents & (OS_POLLHUP | OS_POLLERR)) {
                st->quit = true; /* the terminal went away */
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
            tui_frame(st);
    }

    g_term_cleanup = NULL; /* the normal path already restored the terminal */
    term_leave(term);
    /* The alt screen is gone (and its content discarded); leave the compact
     * trace in the shell scrollback. */
    tui_exit_dump(st);
    agentc_ext_set_ui_sink(NULL, NULL); /* st is freed next; drop the borrowed ud */
    agentc_ext_set_ui_prompt(prompt_prev, prompt_prev_ud);
    tui_free(st);
    term_close(term);
    agentc_free(st);
    return 0;
}
