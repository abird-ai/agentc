/* pick.c — see pick.h: the standalone picker for setup/onboarding. */
#include "pick.h"
#include "components.h"
#include "input.h"
#include "picklist.h"
#include "plat.h"
#include "render.h"
#include "theme.h"

#define PICK_VISIBLE_MAX 10

typedef struct {
    PickList list;
    bool done;
    int result;
} PickState;

static void pick_key(void *ud, const Key *k) {
    PickState *p = ud;
    if (p->done) return;
    int action = picklist_key(&p->list, k, PICK_VISIBLE_MAX);
    if (action == PICKLIST_ACCEPT) {
        p->result = (int)p->list.vidx[p->list.sel];
        p->done = true;
    } else if (action == PICKLIST_CANCEL) {
        p->result = -1;
        p->done = true;
    }
}

int agentc_tui_pick(Terminal *term, const char *title, const char *const *names,
                    const char *const *descs, size_t n, size_t initial) {
    if (!term || !names || n == 0) return -1;
    PickState p;
    agentc_memset(&p, 0, sizeof p);
    picklist_set(&p.list, names, descs, n, initial);

    Theme th;
    theme_init(&th, 1, term_is_tty(term));
    int cols = term_cols(term), rows = term_rows(term);
    if (cols < 24) cols = 24;
    if (cols > GRID_MAX_COLS) cols = GRID_MAX_COLS;
    if (rows < 6) rows = 6;
    if (rows > GRID_MAX_ROWS) rows = GRID_MAX_ROWS;
    Grid g;
    grid_init(&g, cols, rows);
    Input in;
    input_init(&in);

    const char *hint = "Up/Down move (wrap) | Enter select | Esc cancel | type to filter";
    bool dirty = true;
    while (!p.done) {
        int nc = 0, nr = 0;
        if (term_check_resize(term, &nc, &nr)) {
            if (nc > GRID_MAX_COLS) nc = GRID_MAX_COLS;
            if (nr > GRID_MAX_ROWS) nr = GRID_MAX_ROWS;
            if (nc >= 24 && nr >= 6) {
                cols = nc;
                rows = nr;
                grid_resize(&g, cols, rows);
            }
            dirty = true;
        }
        if (dirty) {
            grid_clear(&g, 0, TH_FG, TH_NO_BG);
            if (title && title[0])
                grid_put_clip(&g, 0, 0, cols, A_BOLD, TH_FG, TH_NO_BG, title,
                              agentc_strlen(title));
            grid_put_clip(&g, 0, 1, cols, A_DIM, TH_MUTED, TH_NO_BG, hint,
                          agentc_strlen(hint));
            int listrows = rows - 3;
            if (listrows > PICK_VISIBLE_MAX) listrows = PICK_VISIBLE_MAX;
            if (listrows < 1) listrows = 1;
            picklist_scroll(&p.list, listrows);
            comp_command_menu(&g, &th, 0, 3, cols, listrows, p.list.vname, p.list.vdesc,
                              p.list.vn, p.list.top, p.list.sel, NULL);
            AgcBuf out = { 0 };
            agentc_buf_cstr(&out, "\x1b[?25l");
            render_rows_ansi(&g, 0, rows, &out, &th, false);
            term_write(term, out.p, out.len);
            agentc_buf_free(&out);
            dirty = false;
        }
        /* On a real tty term_read() blocks, so poll first and run the idle ESC
         * flush each tick; the memory backend has no fd and is drained directly. */
        if (term_is_tty(term)) {
            struct os_pollfd pf = { 0, OS_POLLIN, 0 };
            int pr = os_poll(&pf, 1, 50);
            if (pr < 0 && pr != -4) break;
            if (pr > 0 && (pf.revents & (OS_POLLHUP | OS_POLLERR))) break;
            input_idle(&in, os_now_ns(OS_CLOCK_MONOTONIC), 50, pick_key, &p);
            if (p.done) break;
            if (pr <= 0) continue;
        }
        u8 buf[256];
        int r = term_read(term, buf, sizeof buf);
        if (r > 0) {
            input_feed(&in, buf, (size_t)r, os_now_ns(OS_CLOCK_MONOTONIC), pick_key, &p);
            dirty = true;
            continue;
        }
        if (r < 0 && r != -11 && r != -4) break;
        input_idle(&in, os_now_ns(OS_CLOCK_MONOTONIC), 50, pick_key, &p);
        if (p.done) break;
        if (!term_is_tty(term)) break;   /* scripted backend drained */
    }
    input_free(&in);
    grid_free(&g);
    return p.done ? p.result : -1;
}

static Terminal *g_pick_term;

static void pick_restore(void) {
    if (g_pick_term) {
        term_leave(g_pick_term);
        g_pick_term = NULL;
    }
}

static void pick_signal(int sig) {
    pick_restore();
    os_exit(128 + sig);
}

int agentc_tui_pick_tty(const char *title, const char *const *names,
                        const char *const *descs, size_t n, size_t initial) {
    Terminal *term = term_open_tty();
    if (!term) return -1;
    g_pick_term = term;
    agentc_atexit(pick_restore);
    os_sig_install(OS_SIGHUP, pick_signal);
    os_sig_install(OS_SIGINT, pick_signal);
    os_sig_install(OS_SIGQUIT, pick_signal);
    os_sig_install(OS_SIGABRT, pick_signal);
    os_sig_install(OS_SIGBUS, pick_signal);
    os_sig_install(OS_SIGFPE, pick_signal);
    os_sig_install(OS_SIGSEGV, pick_signal);
    os_sig_install(OS_SIGTERM, pick_signal);
    if (term_enter_mode(term, true) != 0) {
        g_pick_term = NULL;
        term_close(term);
        return -1;
    }
    int r = agentc_tui_pick(term, title, names, descs, n, initial);
    g_pick_term = NULL;
    term_leave(term);
    term_close(term);
    return r;
}
