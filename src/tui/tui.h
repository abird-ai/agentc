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

/* App services the TUI cannot own. Passed to agentc_tui_run; may be NULL (tests
 * and library use), in which case /new only clears the view and /resume is
 * disabled. Extend this table rather than widening agentc_tui_run. */
typedef struct {
    void *ud;
    /* Start a fresh session (close the file, create a new one, rebind
     * persistence, clear the agent transcript). Returns 0 or -errno. */
    int (*new_session)(void *ud);
    /* Switch to an existing session file. Returns 0 or -errno. */
    int (*resume_session)(void *ud, const char *path);
    /* NULL = default session location; used by the in-TUI /resume picker and by
     * the startup picker. */
    const char *session_dir;
    /* --continue/--resume on a terminal: open the session picker before the
     * first frame (Escape keeps the mode's newest session). */
    bool pick_session_on_start;
    /* Receives AGENTC_EV_COMPACT so a session-owning front end persists the
     * checkpoint; may be NULL. */
    void (*on_compact)(void *ud, const AgcCompactInfo *ci);
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
 * perform itself: starting or resuming a session, the session directory for the
 * picker, and the compaction checkpoint hook. Without it `/new` and `/resume`
 * are view-only / disabled.
 *
 * `on_compact` used to be a separate argument; it now lives in `AgcTuiApp` so
 * the run signature stays stable as services are added.
 */
int agentc_tui_run(AgcAgent *agent, const char *initial_prompt, int mode,
                   const char *theme_name, bool trusted, bool show_tools,
                   const AgcTuiApp *app);

#endif /* AGENTC_TUI_H */
