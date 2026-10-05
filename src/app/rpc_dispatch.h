/* rpc_dispatch.h — the one RPC command vocabulary shared by --mode rpc (JSONL)
 * and `agentc serve` (framed CBOR). C-D2: the record layer is not duplicated;
 * the dispatcher emits one JSON record per command through a framing-neutral
 * sink, and the front end owns the transport.
 *
 * The AgcModeCtx base is the first member so agentc_mode_event() and the
 * rebuild path can still cast callback userdata to AgcModeCtx.
 */
#ifndef AGENTC_APP_RPC_DISPATCH_H
#define AGENTC_APP_RPC_DISPATCH_H

#include "app/mode.h"
#include "agent.h"
#include "ext.h"

/* One emitted JSON record (includes its trailing newline, as the JSONL front
 * end requires). */
typedef void (*AgcRpcSink)(void *ud, const char *line, size_t n);

typedef struct AgcRpc {
    AgcModeCtx base;   /* first member */
    AgcModeConfig cfg; /* mutable copy (set_model/thinking) */
    AgcRpcSink sink;
    void *sink_ud;
    AgcEventFn event; /* reinstalled after a set_model rebuild */
    void *event_ud;
    const char *cur_cmd; /* command being dispatched (response echo) */
    u64 ui_seq;          /* ui_request correlation counter */
    bool running;
    /* P5 concurrency: when true a prompt starts a resumable one-step run
     * (agentc_agent_run_begin) instead of driving it to completion; the front
     * end advances it with agentc_rpc_run_step() and `agent_settled` is emitted
     * from there. Default false keeps --mode rpc fully synchronous. */
    bool defer_run;
    size_t pending_count; /* the front end's queued-command count */
    /* Appended P5 C3: an optional front-end session registry hook. When set,
     * `new_session` asks the front end to create or select a session (bounded)
     * instead of switching the one engine in place; `spec` is the optional
     * `session` request key (an existing id) and `out_id` receives the selected
     * id. The default (NULL) preserves the --mode rpc switch-in-place behavior. */
    int (*session_new)(void *ud, const char *spec, char out_id[64]);
    void *session_new_ud;
} AgcRpc;

void agentc_rpc_set_sink(AgcRpc *r, AgcRpcSink sink, void *ud);
bool agentc_rpc_running(const AgcRpc *r);
size_t agentc_rpc_pending(const AgcRpc *r);

/* The command name for a parsed request (A-D1 dual discriminator), or NULL. */
const char *agentc_rpc_command_name(const AgcJson *req);

/* Parse and dispatch one JSON command record. `acked` suppresses the
 * acceptance response for a prompt already answered `queued`. -> true when the
 * record was handled (always true). */
bool agentc_rpc_dispatch(AgcRpc *r, const char *line, size_t n, bool acked);

/* Advance one deferred run by one model step. Returns true while it is still
 * running, false once it settled (agent_settled has already been emitted) or
 * when no run is in flight. Only meaningful with `defer_run`. */
bool agentc_rpc_run_step(AgcRpc *r);

/* Emit a response for a command the front end handled itself (queued ack, busy
 * refusal). `cmd` becomes the response echo. */
void agentc_rpc_respond(AgcRpc *r, const AgcJson *req, const char *cmd, bool ok, const char *data,
                        size_t data_len, const char *error);

/* The stable machine code for a failure text (A-D4). */
const char *agentc_rpc_code_for(const char *error);

/* UI sink/status observer install and restore. Save the previous owners in
 * `saved` and restore them when the run ends. */
typedef struct {
    AgcExtUiSink ui;
    void *ui_ud;
    AgcExtStatusObserver so;
    void *so_ud;
} AgcRpcUiSaved;

void agentc_rpc_ui_install(AgcRpc *r, AgcRpcUiSaved *saved);
void agentc_rpc_ui_restore(AgcRpcUiSaved *saved);

#endif /* AGENTC_APP_RPC_DISPATCH_H */
