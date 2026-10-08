/* picklist.c — see picklist.h. */
#include "picklist.h"

void picklist_init(PickList *l) {
    if (l) agentc_memset(l, 0, sizeof *l);
}

void picklist_refresh(PickList *l) {
    if (!l) return;
    size_t n = 0;
    for (size_t i = 0; i < l->n && n < PICKLIST_MAX; i++) {
        if (l->filter[0]) {
            bool hit = (l->name[i] && agentc_str_str(l->name[i], l->filter)) ||
                       (l->desc[i] && agentc_str_str(l->desc[i], l->filter));
            if (!hit) continue;
        }
        l->vname[n] = l->name[i];
        l->vdesc[n] = l->desc[i];
        l->vidx[n] = i;
        n++;
    }
    l->vn = n;
    if (n == 0) l->sel = l->top = 0;
    else if (l->sel >= n) l->sel = n - 1;
}

void picklist_set(PickList *l, const char *const *names, const char *const *descs,
                  size_t n, size_t sel) {
    if (!l) return;
    if (n > PICKLIST_MAX) n = PICKLIST_MAX;
    l->n = n;
    for (size_t i = 0; i < n; i++) {
        l->name[i] = (names && names[i]) ? names[i] : "";
        l->desc[i] = descs ? descs[i] : NULL;
    }
    l->filter[0] = 0;
    l->top = 0;
    l->sel = sel < n ? sel : 0;
    picklist_refresh(l);
}

void picklist_scroll(PickList *l, int rows) {
    if (!l || rows <= 0) return;
    if (l->vn == 0) {
        l->top = 0;
        return;
    }
    if (l->sel < l->top) l->top = l->sel;
    else if (l->sel >= l->top + (size_t)rows) l->top = l->sel - (size_t)rows + 1;
}

static void filter_push(PickList *l, char c) {
    size_t n = agentc_strlen(l->filter);
    if (n + 1 < sizeof l->filter) {
        l->filter[n] = c;
        l->filter[n + 1] = 0;
    }
    picklist_refresh(l);
}

int picklist_key(PickList *l, const Key *k, int rows) {
    if (!l || !k) return PICKLIST_NONE;
    if (k->code == K_ESC ||
        (k->code == K_CHAR && (k->mods & MOD_CTRL) && k->cp == 'c'))
        return PICKLIST_CANCEL;
    if (k->code == K_UP) {
        if (l->vn) l->sel = l->sel == 0 ? l->vn - 1 : l->sel - 1;
        return PICKLIST_NONE;
    }
    if (k->code == K_DOWN) {
        if (l->vn) l->sel = l->sel + 1 >= l->vn ? 0 : l->sel + 1;
        return PICKLIST_NONE;
    }
    if (k->code == K_PGUP || k->code == K_PGDN) {
        size_t step = rows > 1 ? (size_t)rows - 1 : 1;
        if (!l->vn) return PICKLIST_NONE;
        if (k->code == K_PGUP) l->sel = l->sel > step ? l->sel - step : 0;
        else l->sel = l->sel + step < l->vn ? l->sel + step : l->vn - 1;
        return PICKLIST_NONE;
    }
    if (k->code == K_ENTER && !(k->mods & MOD_ALT))
        return l->vn ? PICKLIST_ACCEPT : PICKLIST_NONE;
    if (k->code == K_BACKSPACE) {
        size_t n = agentc_strlen(l->filter);
        if (n) l->filter[n - 1] = 0;
        picklist_refresh(l);
        return PICKLIST_NONE;
    }
    if (k->code == K_CHAR && !(k->mods & (MOD_CTRL | MOD_ALT)) && k->cp >= 0x20 &&
        k->cp < 0x7f)
        filter_push(l, (char)k->cp);
    return PICKLIST_NONE;
}
