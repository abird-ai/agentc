/* picklist.h — the shared selection list behind the modal pickers.
 *
 * One implementation of filtering, scrolling, wrap-around and key handling,
 * used by the in-TUI model/thinking/session pickers (and available to any
 * future picker). Rows are name + description; the caller owns their storage,
 * which must stay valid while the list is in use. The command completion menu is
 * a separate, editor-driven UI and does not use this.
 */
#ifndef AGENTC_TUI_PICKLIST_H
#define AGENTC_TUI_PICKLIST_H

#include "agentc.h"
#include "input.h"

#define PICKLIST_MAX 128

typedef struct {
    const char *name[PICKLIST_MAX];   /* base rows, borrowed */
    const char *desc[PICKLIST_MAX];
    size_t n;                          /* base row count */
    size_t sel, top;                   /* selection and visible-window top */
    char filter[64];
    const char *vname[PICKLIST_MAX];   /* filtered view, rebuilt by refresh */
    const char *vdesc[PICKLIST_MAX];
    size_t vidx[PICKLIST_MAX];         /* view row -> base row */
    size_t vn;
} PickList;

enum { PICKLIST_NONE = 0, PICKLIST_ACCEPT, PICKLIST_CANCEL };

void picklist_init(PickList *l);
/* Set the base rows (borrowed) and reset the filter; `sel` is clamped. */
void picklist_set(PickList *l, const char *const *names, const char *const *descs,
                  size_t n, size_t sel);
void picklist_refresh(PickList *l);
void picklist_scroll(PickList *l, int rows);
/* Handle one modal key. Up/Down wrap around the filtered rows; PageUp/PageDown
 * page; printable bytes and Backspace edit the filter; Enter accepts; Escape or
 * Ctrl-C cancels. Returns an action once; PICKLIST_NONE otherwise. */
int picklist_key(PickList *l, const Key *k, int rows);

#endif /* AGENTC_TUI_PICKLIST_H */
