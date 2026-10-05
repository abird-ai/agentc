/* mode.h — app-facing mode header.
 *
 * The shared run/session mechanics moved to src/core/runsession.h (Axis B4) so
 * the strict no-app libagentc archive can link them; this header forwards to
 * that core header and declares the app-only front ends: --mode json, --mode
 * rpc and the `agentc serve` subcommand.
 */
#ifndef AGENTC_APP_MODE_H
#define AGENTC_APP_MODE_H

#include "agent.h"
#include "config.h"
#include "session.h"
#include "core/runsession.h"

/* RPC record-layer version. Bumped only for a breaking change to the --mode
 * rpc message vocabulary or command semantics; additive keys/commands/codes do
 * not bump it. The full contract lives in .agents/design/60-rpc.md. */
#define AGENTC_RPC_VERSION 1

/* Replay the resumed session's `custom type:"notice"` rows through `emit`, in
 * file order. The entry sink persists status notices as `custom notice` records
 * (`{region,turn,step,text}`); a resumed TUI re-renders the text. No-op for a
 * memory-only session or when the session has no file. App-only (the TUI is
 * present only in the native build); `emit` may not be NULL. */
void agentc_mode_replay_notices(AgcModeCtx *c, void (*emit)(void *ud, const char *text), void *ud);

int agentc_mode_json_run(AgcModeCtx *c, const char *prompt);
int agentc_mode_rpc_run(AgcModeCtx *c);

/* `agentc serve` (P5 Axis C). A Unix-domain listener is primary; the loopback
 * TCP fallback is selected when `tcp` is set (or when a unix listener is not
 * available). `socket_path` NULL selects
 * $XDG_RUNTIME_DIR/abird/agentc/serve.sock. `token`, when non-NULL, must match
 * the client's hello. Runs until shutdown (SIGINT/SIGTERM, EOF on stdin / a
 * fatal error); a signal performs a graceful stop that unlinks the socket and
 * releases the sessions. Returns 0 or a negative errno. */
typedef struct {
    const char *socket_path;
    const char *tcp;  /* "IP:PORT" (explicit loopback/remote bind) */
    const char *host; /* P5 C3 `--host IP[:PORT]` / `--remote` bind; a
                       * non-loopback bind requires a token. */
    const char *token;
    bool ws;              /* P5 C3: WebSocket upgrade on the TCP listener */
    const char *tls_cert; /* P5 C3: PEM certificate file (server TLS) */
    const char *tls_key;  /* P5 C3: PEM private key file (server TLS) */
} AgcServeOptions;
int agentc_mode_serve_run(AgcModeCtx *c, const AgcServeOptions *opt);

#endif /* AGENTC_APP_MODE_H */
