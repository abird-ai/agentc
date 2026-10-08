/* mode.c — app-facing mode helpers (Axis B4).
 *
 * The shared run/session mechanics moved to src/core/runsession.c so the
 * strict no-app libagentc archive can link them. What stays here is the
 * app-only notice replay used by the TUI (the archive excludes src/tui).
 */
#include "app/mode.h"

#include "core/runsession.h"

typedef struct {
    void (*emit)(void *ud, const char *text);
    void *ud;
} ModeNoticeReplay;

static int mode_notice_replay_cb(void *ud, const char *data_json) {
    ModeNoticeReplay *nr = ud;
    if (!nr->emit || !data_json)
        return 0;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *o = agentc_json_parse_in(ja, data_json, agentc_strlen(data_json));
    const char *text = agentc_json_get_str(o, "text");
    if (text && text[0])
        nr->emit(nr->ud, text);
    agentc_json_arena_free(ja);
    return 0;
}

void agentc_mode_replay_notices(AgcModeCtx *c, void (*emit)(void *ud, const char *text), void *ud) {
    if (!c || !c->session || !emit)
        return;
    if (agentc_session_path(c->session) == NULL)
        return;
    ModeNoticeReplay nr = {emit, ud};
    (void)agentc_session_load_custom(c->session, "notice", mode_notice_replay_cb, &nr);
}
