/* pick.c — see pick.h: the pre-TUI list picker. */
#include "pick.h"
#include "components.h"
#include "input.h"
#include "plat.h"
#include "render.h"
#include "theme.h"

#define PICK_MAX 256
#define PICK_VISIBLE_MAX 12

typedef struct {
    const char *const *names;
    const char *const *descs;
    size_t n, sel, top;
    char filter[64];
    const char *vname[PICK_MAX];
    const char *vdesc[PICK_MAX];
    size_t vidx[PICK_MAX];
    size_t vn;
    bool done;
    int result;
} PickState;

static void pick_refresh(PickState *p) {
    size_t n = 0;
    for (size_t i = 0; i < p->n && n < PICK_MAX; i++) {
        if (p->filter[0]) {
            bool hit = (p->names[i] && agentc_str_str(p->names[i], p->filter)) ||
                       (p->descs && p->descs[i] && agentc_str_str(p->descs[i], p->filter));
            if (!hit) continue;
        }
        p->vname[n] = p->names[i];
        p->vdesc[n] = p->descs ? p->descs[i] : NULL;
        p->vidx[n] = i;
        n++;
    }
    p->vn = n;
    if (n == 0) p->sel = p->top = 0;
    else if (p->sel >= n) p->sel = n - 1;
}

static void pick_scroll(PickState *p, int rows) {
    if (rows <= 0) return;
    if (p->sel < p->top) p->top = p->sel;
    else if (p->sel >= p->top + (size_t)rows) p->top = p->sel - (size_t)rows + 1;
}

static void pick_key(void *ud, const Key *k) {
    PickState *p = ud;
    if (k->code == K_ESC ||
        (k->code == K_CHAR && (k->mods & MOD_CTRL) && k->cp == 'c')) {
        p->result = -1;
        p->done = true;
        return;
    }
    if (k->code == K_UP || k->code == K_PGUP) {
        if (p->sel > 0) p->sel--;
        return;
    }
    if (k->code == K_DOWN || k->code == K_PGDN) {
        if (p->sel + 1 < p->vn) p->sel++;
        return;
    }
    if (k->code == K_ENTER && !(k->mods & MOD_ALT)) {
        p->result = p->vn ? (int)p->vidx[p->sel] : -1;
        p->done = true;
        return;
    }
    if (k->code == K_BACKSPACE) {
        size_t n = agentc_strlen(p->filter);
        if (n) p->filter[n - 1] = 0;
        pick_refresh(p);
        return;
    }
    if (k->code == K_CHAR && !(k->mods & (MOD_CTRL | MOD_ALT)) && k->cp >= 0x20 &&
        k->cp < 0x7f) {
        size_t n = agentc_strlen(p->filter);
        if (n + 1 < sizeof p->filter) {
            p->filter[n] = (char)k->cp;
            p->filter[n + 1] = 0;
        }
        pick_refresh(p);
        return;
    }
}

int agentc_tui_pick(Terminal *term, const char *title, const char *const *names,
                    const char *const *descs, size_t n, size_t initial) {
    if (!term || !names || n == 0) return -1;
    PickState p;
    agentc_memset(&p, 0, sizeof p);
    p.names = names;
    p.descs = descs;
    p.n = n;
    p.sel = initial < n ? initial : 0;
    pick_refresh(&p);

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

    const char *hint = "Up/Down move · Enter select · Esc start new · type to filter";
    bool dirty = true;
    while (!p.done) {
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
            pick_scroll(&p, listrows);
            comp_command_menu(&g, &th, 0, 3, cols, listrows, p.vname, p.vdesc, p.vn,
                              p.top, p.sel, NULL);
            AgcBuf out = { 0 };
            agentc_buf_cstr(&out, "\x1b[?25l");
            render_rows_ansi(&g, 0, rows, &out, &th, false);
            term_write(term, out.p, out.len);
            agentc_buf_free(&out);
            dirty = false;
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
        if (!term_is_tty(term)) break;   /* scripted backend exhausted */
        struct os_pollfd pf = { 0, OS_POLLIN, 0 };
        int pr = os_poll(&pf, 1, 50);
        if (pr > 0 && (pf.revents & (OS_POLLHUP | OS_POLLERR))) break;
    }
    input_free(&in);
    grid_free(&g);
    return p.done ? p.result : -1;
}

int agentc_tui_pick_tty(const char *title, const char *const *names,
                        const char *const *descs, size_t n, size_t initial) {
    Terminal *term = term_open_tty();
    if (!term) return -1;
    if (term_enter_mode(term, true) != 0) {
        term_close(term);
        return -1;
    }
    int r = agentc_tui_pick(term, title, names, descs, n, initial);
    term_leave(term);
    term_close(term);
    return r;
}
