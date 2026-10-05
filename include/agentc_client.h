/* agentc_client.h — synchronous client for the framed agentc protocol (P5 C1).
 *
 * Connect, handshake, send `request` envelopes and pump events. The client is
 * single-threaded and synchronous (C-D8): `request` blocks until its response
 * arrives, dispatching any interleaved events to the callback. There is no
 * automatic replay on reconnect; resync with `get_messages`.
 */
#ifndef AGENTC_CLIENT_H
#define AGENTC_CLIENT_H

#include "agentc.h"
#include "cbor.h"
#include "protocol.h"

typedef struct AgcClient AgcClient;

/* Event callback: return 0 to continue, nonzero to abort the containing
 * request/pump. `ev` is a decoded event envelope or a server `ui_request`
 * dialog (answer it with agentc_client_ui_response). */
typedef int (*AgcClientEventFn)(void *ud, const AgcProtoMsg *ev);

/* Connect to a unix socket path, or "tcp:<ip>:<port>" for the loopback fallback,
 * and perform the hello handshake. `token` is optional. Returns NULL on
 * failure; agentc_client_error() is not available then (no client). */
AgcClient *agentc_client_connect(const char *path, const char *token);
void agentc_client_close(AgcClient *c);

const char *agentc_client_server_id(const AgcClient *c);
const char *agentc_client_session_id(const AgcClient *c);
bool agentc_client_has_feature(const AgcClient *c, const char *name);
const char *agentc_client_error(const AgcClient *c); /* "" when none */

/* Send {id,method,params} and pump until the matching response. On success
 * returns the response `result` (borrowed from the client arena, valid until the
 * next call) or NULL on transport/protocol failure; an error response is
 * reported through agentc_client_error() and returns NULL. */
AgcCbor *agentc_client_request(AgcClient *c, const char *method, const AgcCbor *params,
                               AgcClientEventFn on_event, void *ud);
/* Send an out-of-band cancel envelope for `id`. -> 0 | -errno. */
int agentc_client_cancel(AgcClient *c, const AgcCbor *id);
/* Answer a server `ui_request` dialog (id is its uint dialog id). `value` is
 * the answer JSON (NULL for a deny). -> 0 | -errno. */
int agentc_client_ui_response(AgcClient *c, u64 id, bool ok, const AgcCbor *value);
/* Dispatch any already-buffered/available events. -> 0 | -errno. */
int agentc_client_pump(AgcClient *c, int timeout_ms, AgcClientEventFn on_event, void *ud);
/* Explicit reconnect + handshake (no replay). -> 0 | -errno. */
int agentc_client_reconnect(AgcClient *c);

/* The response `ok` flag and error text/code of the last request. */
bool agentc_client_last_ok(const AgcClient *c);
const char *agentc_client_last_code(const AgcClient *c);
const char *agentc_client_last_error(const AgcClient *c);

/* P5 C3 session selection. `agentc_client_new_session` asks the server to
 * create a session (`spec` NULL) or select an existing one (`spec` = an id) and
 * adopts the returned id as the tracked session; `agentc_client_attach` binds
 * this connection to `session` (NULL = the tracked session) with an optional
 * event-type filter. Both return 0 or -errno. */
int agentc_client_new_session(AgcClient *c, const char *spec);
int agentc_client_attach(AgcClient *c, const char *session, const char *const *events,
                         size_t nevents);
/* The originating session id of an `event` envelope (borrowed; "" when untagged). */
const char *agentc_client_event_session(const AgcProtoMsg *ev);

#endif /* AGENTC_CLIENT_H */
