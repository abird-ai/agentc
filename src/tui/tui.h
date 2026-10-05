/* tui.h — the interactive terminal front end.
 *
 * The agent must already be configured (provider, model, key, tools, transport).
 * agentc_tui_run installs the event callback and the HTTP poll hook, then returns
 * when the user quits.
 */
#ifndef AGENTC_TUI_H
#define AGENTC_TUI_H

#include "agent.h"

/* The interactive TUI always runs on the alternate screen: the whole view is
 * redrawn in place each frame and the shell's scrollback is left untouched. */

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
     * first frame (Escape keeps the app's newest session). */
    bool pick_session_on_start;
    /* Receives AGENTC_EV_COMPACT so a session-owning front end persists the
     * checkpoint; may be NULL. */
    void (*on_compact)(void *ud, const AgcCompactInfo *ci);
    /* Replay persisted `custom type:"notice"` rows into the TUI after a session
     * swap. `emit` appends one notice line (the TUI supplies it); may be NULL.
     * Appended in P6 completion pass 2c. */
    void (*replay_notices)(void *ud, void (*emit)(void *eud, const char *text), void *eud);
    /* Fork the current session into a new file, keeping the transcript.
     * Appended P7. Returns 0 or -errno. */
    int (*fork_session)(void *ud);
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
int agentc_tui_run(AgcAgent *agent, const char *initial_prompt, const char *theme_name,
                   bool trusted, bool show_tools, const AgcTuiApp *app);

#endif /* AGENTC_TUI_H */
