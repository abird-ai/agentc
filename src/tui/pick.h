/* pick.h — a standalone interactive list picker for the pre-TUI flows
 * (agentc setup / onboarding). It shares the selection mechanic (PickList) and
 * the list chrome (comp_command_menu) with the in-TUI pickers; only the small
 * terminal loop lives here, and it takes `Terminal` as a parameter so the golden
 * harness can drive it with the in-memory backend.
 */
#ifndef AGENTC_TUI_PICK_H
#define AGENTC_TUI_PICK_H

#include "agentc.h"
#include "term.h"

/* Run the picker over `term` (already in raw/alt mode for a real tty; the memory
 * backend needs no setup). Returns the chosen index, or -1 on Escape, an empty
 * list or a gone backend. Typing filters, Up/Down wrap, Enter selects. */
int agentc_tui_pick(Terminal *term, const char *title, const char *const *names,
                    const char *const *descs, size_t n, size_t initial);

/* tty convenience wrapper: open the controlling terminal, install the restore
 * handlers, enter raw/alt mode, run the picker and restore. Returns -1 when
 * there is no tty (the caller falls back to text input). */
int agentc_tui_pick_tty(const char *title, const char *const *names,
                        const char *const *descs, size_t n, size_t initial);

#endif /* AGENTC_TUI_PICK_H */
