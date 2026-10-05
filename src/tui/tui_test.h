/* tui_test.h — deterministic harness for the TUI (no tty required).
 *
 * Tests script raw terminal bytes into the in-memory backend, synthesize
 * AGENTC_EV_* agent events, and dump the visible grid as plain text. Time is a
 * fake millisecond clock, so spinner/elapsed output is stable.
 */
#ifndef AGENTC_TUI_TEST_H
#define AGENTC_TUI_TEST_H

#include "agentc.h"
#include "agent.h"
#include "tui.h"

typedef struct AgcTuiTest AgcTuiTest;

/* agentc_tui_test_new() builds the fullscreen harness: the interactive TUI's
 * only rendering path (the alternate screen). */
AgcTuiTest *agentc_tui_test_new(int cols, int rows);
void agentc_tui_test_free(AgcTuiTest *t);
/* Raw terminal bytes (escape sequences, UTF-8, bracketed paste). */
void agentc_tui_test_feed(AgcTuiTest *t, const char *bytes, size_t len);
/* Advance the fake clock, flushing a pending lone ESC after 50 ms. */
void agentc_tui_test_tick(AgcTuiTest *t, i64 delta_ms);
/* Deliver one agent event (AgcTextDelta/AgcToolExec/... as documented). */
void agentc_tui_test_event(AgcTuiTest *t, int ev, const void *data);
void agentc_tui_test_submit(AgcTuiTest *t);
/* Pretend a run is in flight. */
void agentc_tui_test_set_running(AgcTuiTest *t, bool on);
/* The composer caret from the last layout: grid coordinates and whether the
 * rendered fallback cell there is reverse-video (the memory backend has no
 * hardware cursor, so this is the observable caret). */
void agentc_tui_test_cursor(AgcTuiTest *t, int *x, int *y, bool *reverse);
void agentc_tui_test_frame(AgcTuiTest *t);
void agentc_tui_test_resize(AgcTuiTest *t, int cols, int rows);
/* The composer's plain text (tests). */
const char *agentc_tui_test_editor_text(AgcTuiTest *t);
/* Plain-text view of what the user currently sees: the fullscreen grid. */
const char *agentc_tui_test_screen(AgcTuiTest *t);
/* Install the compaction callback the harness should exercise, mirroring the
 * fields agentc_tui_run sets on a real Tui. */
void agentc_tui_test_set_compact_hook(AgcTuiTest *t, void (*fn)(void *ud, const AgcCompactInfo *ci),
                                      void *ud);
/* Raw bytes the backend has emitted (alt-screen/frames/...). */
const char *agentc_tui_test_output(AgcTuiTest *t);
void agentc_tui_test_output_clear(AgcTuiTest *t);
bool agentc_tui_test_aborted(AgcTuiTest *t);
bool agentc_tui_test_running(AgcTuiTest *t);
/* True once Ctrl+D on an empty composer (or another quit path) set the quit flag. */
bool agentc_tui_test_quit(AgcTuiTest *t);
void agentc_tui_test_set_history(AgcTuiTest *t, const char *path);
void agentc_tui_test_set_footer(AgcTuiTest *t, const char *model, const char *thinking, u32 tok_in,
                                u32 tok_out, i64 cost_micro);
/* Mirror agentc_tui_run's `show_tools` banner gate. */
void agentc_tui_test_set_show_tools(AgcTuiTest *t, bool on);

/* Approval overlay harness (P6 completion 2g): open the modal from a raw §7.1
 * request JSON, then step the scope/remember selection through agentc_tui_test_feed
 * and inspect it. `build` renders the response the overlay would submit without
 * touching the core pending table. */
void agentc_tui_test_approval_open(AgcTuiTest *t, uint64_t id, const char *request_json);
bool agentc_tui_test_approval_is_open(AgcTuiTest *t);
int agentc_tui_test_approval_scope(AgcTuiTest *t);
bool agentc_tui_test_approval_remember(AgcTuiTest *t);
void agentc_tui_test_approval_build(AgcTuiTest *t, bool allow, AgcBuf *out);

/* /settings single-value editor harness (P6 completion 2h). `open` reports
 * whether the modal is open; `text` returns the current edit buffer. */
bool agentc_tui_test_setting_editor_open(AgcTuiTest *t);
const char *agentc_tui_test_setting_editor_text(AgcTuiTest *t);

/* Attach an agent for tests that exercise the model picker (`/model` with no
 * argument). The caller retains ownership; the harness never frees it. */
void agentc_tui_test_set_agent(AgcTuiTest *t, AgcAgent *a);
/* Attach app services so `/new` can start a session (usually NULL in tests). */
void agentc_tui_test_set_app(AgcTuiTest *t, const AgcTuiApp *app);
/* Attributes of the cell at (x, y) in the grid the last frame composed. */
u16 agentc_tui_test_cell_attrs(AgcTuiTest *t, int x, int y);

#endif /* AGENTC_TUI_TEST_H */
