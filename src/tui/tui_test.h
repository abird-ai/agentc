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

typedef struct AgcTuiTest AgcTuiTest;

/* agentc_tui_test_new() keeps the classic fullscreen harness (existing goldens);
 * agentc_tui_test_new_mode lets a test pick the inline renderer. */
AgcTuiTest *agentc_tui_test_new(int cols, int rows);
AgcTuiTest *agentc_tui_test_new_mode(int cols, int rows, int mode);
void agentc_tui_test_free(AgcTuiTest *t);
/* Raw terminal bytes (escape sequences, UTF-8, bracketed paste). */
void agentc_tui_test_feed(AgcTuiTest *t, const char *bytes, size_t len);
/* Advance the fake clock, flushing a pending lone ESC after 50 ms. */
void agentc_tui_test_tick(AgcTuiTest *t, i64 delta_ms);
/* Deliver one agent event (AgcTextDelta/AgcToolExec/... as documented). */
void agentc_tui_test_event(AgcTuiTest *t, int ev, const void *data);
void agentc_tui_test_submit(AgcTuiTest *t);
/* Pretend a run is in flight (inline mode keeps streaming content live only
 * while a run is active). */
void agentc_tui_test_set_running(AgcTuiTest *t, bool on);
/* Inline renderer bookkeeping: rows drawn last frame, the editor cursor row
 * inside the region and the cursor's distance from the region top. */
void agentc_tui_test_live_metrics(AgcTuiTest *t, int *rows, int *cursor_y, int *cursor_off);
/* The presentation mode the harness was created with. */
int agentc_tui_test_mode(AgcTuiTest *t);
/* Inline ownership probes: the absolute top row of the owned region (-1 when the
 * mode does not own one), the number of rows currently blank below the committed
 * content, and the recorded content width of an owned row (the reflow input). */
int agentc_tui_test_region_top(AgcTuiTest *t);
int agentc_tui_test_region_cleared(AgcTuiTest *t);
int agentc_tui_test_region_row_width(AgcTuiTest *t, int y);
/* The composer caret from the last layout: grid coordinates and whether the
 * rendered fallback cell there is reverse-video (the memory backend has no
 * hardware cursor, so this is the observable caret). */
void agentc_tui_test_cursor(AgcTuiTest *t, int *x, int *y, bool *reverse);
void agentc_tui_test_frame(AgcTuiTest *t);
void agentc_tui_test_resize(AgcTuiTest *t, int cols, int rows);
/* Test-only: stage the resize race the single-threaded loop cannot interleave
 * (a frame in progress when the geometry changes); the next frame must discard
 * and re-anchor instead of painting with stale coordinates. */
void agentc_tui_test_resize_mid_frame(AgcTuiTest *t, int cols, int rows);
/* Plain-text view of what the user currently sees: the grid for fullscreen,
 * the composed live region for inline. */
const char *agentc_tui_test_screen(AgcTuiTest *t);
/* Inline mode only: plain text of the lines already committed to scrollback. */
const char *agentc_tui_test_scrollback(AgcTuiTest *t);
/* Install the compaction callback the harness should exercise, mirroring the
 * fields agentc_tui_run sets on a real Tui. */
void agentc_tui_test_set_compact_hook(AgcTuiTest *t,
                                      void (*fn)(void *ud, const AgcCompactInfo *ci),
                                      void *ud);
/* Raw bytes the backend has emitted (alt-screen/frames/...). */
const char *agentc_tui_test_output(AgcTuiTest *t);
void agentc_tui_test_output_clear(AgcTuiTest *t);
bool agentc_tui_test_aborted(AgcTuiTest *t);
bool agentc_tui_test_running(AgcTuiTest *t);
/* True once Ctrl+D on an empty composer (or another quit path) set the quit flag. */
bool agentc_tui_test_quit(AgcTuiTest *t);
void agentc_tui_test_set_history(AgcTuiTest *t, const char *path);
void agentc_tui_test_set_footer(AgcTuiTest *t, const char *model, const char *thinking,
                            u32 tok_in, u32 tok_out, i64 cost_micro);
/* Mirror agentc_tui_run's `show_tools` banner gate. */
void agentc_tui_test_set_show_tools(AgcTuiTest *t, bool on);

#endif /* AGENTC_TUI_TEST_H */
