/* mode.h — shared mechanics for the JSON/RPC/print/TUI front ends.
 *
 * Internal app header, not part of the frozen public surface. It owns the
 * agent/session construction (agentc_mode_setup / agentc_mode_teardown), the
 * JSONL session header and event stream used by --mode json and --mode rpc,
 * transcript flushing into the session, and the provider-registry agent rebuild
 * used by `set_model`. Every front end calls the same setup path; only the
 * event/observer callbacks (AgcModeIo) and the session options differ.
 */
#ifndef AGENTC_APP_MODE_H
#define AGENTC_APP_MODE_H

#include "agent.h"
#include "config.h"
#include "session.h"

/* Injected I/O and hooks. read/write may be NULL for setup-only callers.
 * event    NULL -> the shared JSONL event writer (--mode json / --mode rpc).
 * observer NULL -> transcript messages are flushed into the session. */
typedef struct {
    int (*read)(void *ud, void *buf, size_t cap);
    void (*write)(void *ud, const void *buf, size_t n);
    void *in_ud;
    void *out_ud;
    AgcEventFn event;
    void *event_ud;
    AgcMsgObserver observer;
    void *observer_ud;
} AgcModeIo;

/* Everything setup needs. All string/array pointers are borrowed and must
 * outlive the ctx. `session` is the AgcSessionOptions (cwd/dir/memory_only);
 * `session_spec` is a resolved --session PATH|ID and `continue_last` selects
 * --continue/--resume. */
typedef struct {
    const AgcConfig *cfg;      /* loaded config (borrowed) */
    const char *provider;
    const char *model;
    const char *api_key;
    const char *base_url;
    const char *system;         /* explicit system prompt; NULL = auto (agent builds
                                 * from the live tool table each turn) */
    const AgcTool *tools;
    size_t ntools;
    int thinking;
    i64 max_tokens;
    int max_attempts;
    bool insecure;
    bool auto_compact;
    u32 compact_reserve;
    u32 compact_keep;
    AgcSessionOptions session;
    const char *session_spec;
    bool continue_last;
    /* Re-run the active-tool selection after a late contribution (MCP connect).
     * Installed on every agent, including rebuilds; may be NULL. */
    AgcRecomposeFn recompose;
    void *recompose_ud;
} AgcModeConfig;

/* AGENTC_MODE_F_SESSION: create/open the session and wire persistence.
 * AGENTC_MODE_F_ABORT: install the cooperative-abort wiring (extension context and
 * entry sink, tool veto, pump) so a front end can cancel a run. */
#define AGENTC_MODE_F_SESSION (1u << 0)
#define AGENTC_MODE_F_ABORT   (1u << 1)

/* Owns provider/model strings, the agent and the session. `cfg`, `mcfg` and
 * `io` stay borrowed from the caller; a mode-private tail (RPC command state)
 * embeds this struct as its first member so callbacks can cast the userdata.
 * Setup installs the extension model/thinking sinks with this context as
 * userdata; teardown clears them. */
typedef struct AgcModeCtx {
    const AgcConfig *cfg;
    const AgcModeConfig *mcfg;  /* RPC swaps in its mutable copy */
    const AgcModeIo *io;
    unsigned flags;
    char *provider, *model;        /* owned */
    AgcAgent *agent;            /* owned */
    AgcSession *session;        /* owned (may be memory-only) */
    bool session_resumed;       /* an existing session file was opened */
    size_t flushed;                /* transcript entries already persisted */
} AgcModeCtx;

int  agentc_mode_setup(AgcModeCtx *c, const AgcModeConfig *cfg,
                       const AgcModeIo *io, unsigned flags);
void agentc_mode_teardown(AgcModeCtx *c);

/* The one main-loop pump duty (deferred extension work: MCP connect/re-sync).
 * agentc_mode_setup installs it as the agent pump and the wire poll hook for
 * every mode; a front end with its own poll hook chains through this callback
 * rather than installing a second, competing hook. Safe to call directly from
 * an idle loop. `timeout_ms` is a hint; the callback returns promptly. */
void agentc_mode_pump(void *ud, int timeout_ms);

void agentc_mode_write_session_header(AgcModeCtx *c);
void agentc_mode_session_flush(AgcModeCtx *c);
void agentc_mode_append_compaction(AgcSession *s, const AgcCompactInfo *ci);
void agentc_mode_out(AgcModeCtx *c, const void *p, size_t n);
void agentc_mode_out_line(AgcModeCtx *c, AgcBuf *b);
void agentc_mode_event(void *ud, int ev, const void *data);
void agentc_mode_msg_json(AgcBuf *b, const AgcMsg *m);
/* The single persistence policy shared by every observer: a transcript message
 * is written to the JSONL session unless it is the system prompt (it lives in
 * the header) or a failed/aborted assistant turn (providers reject it on
 * replay). Keeping this in one place is what stops the print/TUI and the
 * JSON/RPC flush paths from drifting apart. */
bool agentc_mode_msg_persistable(const AgcMsg *m);
const char *agentc_mode_stop_name(int stop);
AgcAgent *agentc_mode_rebuild_agent(AgcModeCtx *c);

/* Rebind the persistence owner after a session swap (RPC `new_session`):
 * publish the new `session_id`/`session_file` to the extension context (an
 * empty file clears it), reinstall the entry sink on the current session, and
 * reinstall the ctx-bound observer so a flush can never reach a closed old
 * session. The context/entry sink exist only when the mode installed the
 * cooperative-abort wiring; the observer is reinstalled in every mode. Safe
 * with a NULL session. */
void agentc_mode_rebind_session(AgcModeCtx *c);

/* Emit `session_start` in the one shared payload shape
 * `{reason, session_id, session_file, previous_session_file?}`. The startup
 * site and the new-session site both call this so their fields cannot drift;
 * `previous_session_file` may be NULL/empty, in which case the key is omitted.
 * No-op unless an extension subscribes to the point. */
void agentc_mode_emit_session_start(AgcModeCtx *c, const char *reason,
                                    const char *previous_session_file);

/* Persist a compaction checkpoint and re-sync the flush index. The COMPACT
 * event fires before the core splices, so the post-splice transcript length is
 * exactly checkpoint + kept messages; this is the one compaction policy used
 * by the shared event path and by print mode. */
void agentc_mode_note_compaction(AgcModeCtx *c, const AgcCompactInfo *ci);

int agentc_mode_json_run(AgcModeCtx *c, const char *prompt);
int agentc_mode_rpc_run(AgcModeCtx *c);

#endif /* AGENTC_APP_MODE_H */
