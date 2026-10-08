/* pick.h — standalone modal list picker for the pre-TUI flow (session resume).
 *
 * The resume prompt runs before an agent exists, so it cannot use the running
 * TUI loop; it reuses the same cell grid, input parser, list chrome
 * (comp_command_menu) and keys as the in-TUI model picker. `Terminal` is a
 * parameter so the golden harness can drive it with the in-memory backend.
 */
#ifndef AGENTC_TUI_PICK_H
#define AGENTC_TUI_PICK_H

#include "agentc.h"
#include "term.h"

/* Run the picker over `term` (already in raw/alt mode for a real tty; the memory
 * backend needs no setup). Returns the selected index, or -1 on Escape, an empty
 * list or a gone backend. Typing filters by substring of the name or
 * description, Up/Down (PageUp/PageDown) move, Enter selects. `initial` is
 * clamped to the list. */
int agentc_tui_pick(Terminal *term, const char *title, const char *const *names,
                    const char *const *descs, size_t n, size_t initial);

/* tty convenience wrapper: open the controlling terminal, enter raw/alt mode,
 * run the picker and restore. Returns -1 when there is no tty. */
int agentc_tui_pick_tty(const char *title, const char *const *names,
                        const char *const *descs, size_t n, size_t initial);

#endif /* AGENTC_TUI_PICK_H */
