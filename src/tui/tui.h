/* tui.h — the interactive terminal front end.
 *
 * The agent must already be configured (provider, model, key, tools, transport).
 * agentc_tui_run installs the event callback and the HTTP poll hook, then returns
 * when the user quits.
 */
#ifndef AGENTC_TUI_H
#define AGENTC_TUI_H

#include "agent.h"

/* Three presentation modes share the same cell grid:
 *
 *   scrollback  append-only: finished content is printed once and never
 *               redrawn; the live area is only the uncommitted tail plus the
 *               composer and footer, and nothing about it is owned for repaint.
 *   inline      (default) the app owns a fixed rectangular region at the
 *               bottom and behaves like fullscreen inside it (absolute cursor
 *               positioning, full repaint every frame). It never enters the
 *               alternate screen, so shell history above the region is kept;
 *               finished transcript rows are pushed into real scrollback
 *               deliberately and are not redrawn afterwards.
 *   fullscreen  the alternate screen, as before.
 *
 * `inline` is the default. The spellings accepted by --tui-mode and the
 * tui_mode config key are "scrollback", "inline", "fullscreen" and "auto"
 * (an alias for inline); agentc_tui_mode_parse owns that mapping. */
enum {
    AGENTC_TUI_SCROLLBACK = 0,
    AGENTC_TUI_INLINE = 1,
    AGENTC_TUI_FULLSCREEN = 2,
};

/* Map a --tui-mode/config spelling onto the enum. Returns 0 on success and
 * sets *out; returns -1 for an unknown spelling (the caller reports it).
 * "auto" resolves to inline, today's default everywhere. */
int agentc_tui_mode_parse(const char *value, int *out);
/* Human-readable value for logs and tests ("scrollback", "inline",
 * "fullscreen"). Never NULL. */
const char *agentc_tui_mode_name(int mode);

/* App services the TUI cannot own. Passed to agentc_tui_run; may be NULL
 * (tests and library use), in which case /new only clears the view. */
typedef struct {
    void *ud;
    /* Start a fresh session (close the file, create a new one, rebind
     * persistence, clear the agent transcript). Returns 0 or -errno. */
    int (*new_session)(void *ud);
} AgcTuiApp;

/* `trusted` is the app's already-resolved project-trust verdict. The TUI must
 * not re-derive it: a project trusted only via --approve or a one-shot
 * project_trust hook would read untrusted and project themes would stay
 * unreachable by name.
 *
 * `show_tools` mirrors the effective tool table: the banner advertises the
 * resolved core-tool engines only when the model actually has tools (false for
 * --no-tools or an otherwise empty selection).
 *
 * `app` (optional, may be NULL) carries the app-owned services the TUI cannot
 * perform itself; today that is only starting a fresh session for `/new`.
 * Without it `/new` clears the view and says so instead of replacing the
 * session.
 *
 * `on_compact` (optional, may be NULL) receives AGENTC_EV_COMPACT with
 * `on_compact_ud`. A front end that owns a session must pass it so the
 * compaction checkpoint is persisted and the session's flush index stays in
 * sync; without it the first post-compaction message is dropped from the
 * session file. */
int agentc_tui_run(AgcAgent *agent, const char *initial_prompt, int mode,
                   const char *theme_name, bool trusted, bool show_tools,
                   const AgcTuiApp *app,
                   void (*on_compact)(void *ud, const AgcCompactInfo *ci),
                   void *on_compact_ud);

#endif /* AGENTC_TUI_H */
