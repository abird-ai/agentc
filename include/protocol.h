/* protocol.h — the framed agentc protocol v1 envelope (P5 Axis C).
 *
 * One CBOR value per frame. The envelope is a map with a text `kind`:
 *
 *   hello        {kind:"hello", version:1, client?:"...", token?:"..."}
 *   hello_ack    {kind:"hello_ack", version:1, server_id:"<uuid>",
 *                 session_id?:"...", features:["cancel","ui","attach","multi_session"]}
 *   hello_error  {kind:"hello_error", code:"...", message:"..."}
 *   request      {kind:"request", id:<uint|text>, method:"...", params:<json>?}
 *   response     {kind:"response", id:<uint|text>, ok:<bool>,
 *                 result:<json>? | error:{code,message}}
 *   cancel       {kind:"cancel", id:<uint|text>}
 *   event        {kind:"event", seq:<uint>, session_id?:"<id>", event:<json>}
 *   ui_request   {kind:"ui_request", id:<uint>, method:"...", ...}   (feature ui)
 *   ui_response  {kind:"ui_response", id:<uint>, ok:<bool>, ...}     (feature ui)
 *   attach       {kind:"attach", id:<uint|text>, session?:"<id>", events?:[...]}
 *   detach       {kind:"detach", id:<uint|text>}
 *
 * `params`/`result`/`event` carry strict-JSON-constrained CBOR (C-D1); the
 * decoder rejects anything else with -ENOTSUP. Unknown properties are rejected
 * at the envelope level (C-D10): a decode with an unexpected key returns
 * -EPROTO, and the connection treats that as terminal (C-D7). The exact version
 * is 1 (C-D6).
 */
#ifndef AGENTC_PROTOCOL_H
#define AGENTC_PROTOCOL_H

#include "cbor.h"

#define AGENTC_PROTO_VERSION 1u
#define AGENTC_PROTO_MAX_ID 64u /* bytes of a text id / method */
/* Per-connection requests awaiting a response. */
#define AGENTC_PROTO_MAX_PENDING 64u
/* Dialog request/response bounds (P5 C2 UI). */
#define AGENTC_PROTO_MAX_UI_OPTIONS 64u  /* `select` choices */
#define AGENTC_PROTO_MAX_UI_PROMPT 4096u /* dialog prompt text */
/* Attach subscription filter (P5 C3): event-type names (C-D4). */
#define AGENTC_PROTO_MAX_ATTACH_EVENTS 32u

enum {
    AGENTC_PROTO_NONE = 0,
    AGENTC_PROTO_HELLO,
    AGENTC_PROTO_HELLO_ACK,
    AGENTC_PROTO_HELLO_ERROR,
    AGENTC_PROTO_REQUEST,
    AGENTC_PROTO_RESPONSE,
    AGENTC_PROTO_CANCEL,
    AGENTC_PROTO_EVENT,
    AGENTC_PROTO_UI_REQUEST,
    AGENTC_PROTO_UI_RESPONSE,
    /* P5 C3 attachment routing (C-D4): additive kinds. */
    AGENTC_PROTO_ATTACH,
    AGENTC_PROTO_DETACH,
};

typedef struct {
    int kind;
    /* hello / hello_ack */
    u32 version;
    const char *client;      /* borrowed */
    const char *token;       /* borrowed */
    const char *server_id;   /* borrowed */
    const char *session_id;  /* borrowed */
    const char *features[8]; /* hello_ack features (borrowed text) */
    size_t nfeatures;
    /* request / response / cancel */
    AgcCbor *id;        /* borrowed; uint or text */
    const char *method; /* borrowed text */
    size_t method_len;
    AgcCbor *params; /* borrowed JSON value or NULL */
    bool ok;
    AgcCbor *result;        /* borrowed JSON value or NULL */
    const char *error_code; /* borrowed */
    size_t error_code_len;
    const char *error_message; /* borrowed */
    size_t error_message_len;
    /* event */
    u64 seq;
    AgcCbor *event; /* borrowed JSON value */
    /* attach (C3): session filter + event-type subscription filter */
    const char *session; /* borrowed text or NULL */
    const char *events[AGENTC_PROTO_MAX_ATTACH_EVENTS];
    size_t nevents;
} AgcProtoMsg;

/* Decode and validate one envelope. -> 0 | -EINVAL not a map |
 * -EPROTO unknown kind / unknown property / missing mandatory property |
 * -ENOTSUP a payload outside the JSON vocabulary. On success `out` borrows
 * pointers from `v` (which must outlive `out`). */
int agentc_proto_decode(const AgcCbor *v, AgcProtoMsg *out);

/* True when `method` is one of the v1 dialog methods
 * (select|confirm|input|editor). */
bool agentc_proto_ui_method_valid(const char *method, size_t n);

/* True when `code` is in the closed v1 error-code set. */
bool agentc_proto_code_valid(const char *code, size_t n);

/* Random RFC 4122 v4 UUID string (36 chars + NUL) into `out`. */
void agentc_proto_uuid(char out[37]);

/* --------------------------------------------------------------- encoders */
void agentc_proto_enc_hello(AgcCborW *w, u32 version, const char *client, const char *token);
void agentc_proto_enc_hello_ack(AgcCborW *w, u32 version, const char *server_id,
                                const char *session_id, const char *const *features,
                                size_t nfeatures);
void agentc_proto_enc_hello_error(AgcCborW *w, const char *code, const char *message);
void agentc_proto_enc_request(AgcCborW *w, const AgcCbor *id, const char *method, size_t method_len,
                              const AgcCbor *params);
void agentc_proto_enc_response(AgcCborW *w, const AgcCbor *id, bool ok, const AgcCbor *result,
                               const char *code, const char *message);
void agentc_proto_enc_cancel(AgcCborW *w, const AgcCbor *id);
void agentc_proto_enc_event(AgcCborW *w, u64 seq, const AgcCbor *event);
/* Additive C3 form: the `event` envelope carries the originating session id.
 * The 3-argument form above stays for ABI stability (single-session callers). */
void agentc_proto_enc_event_sid(AgcCborW *w, u64 seq, const char *session_id, const AgcCbor *event);
void agentc_proto_enc_ui_request(AgcCborW *w, u64 id, const char *method, const AgcCbor *params);
void agentc_proto_enc_ui_response(AgcCborW *w, u64 id, bool ok, const AgcCbor *result);
void agentc_proto_enc_attach(AgcCborW *w, const AgcCbor *id, const char *session,
                             const char *const *events, size_t nevents);
void agentc_proto_enc_detach(AgcCborW *w, const AgcCbor *id);

#endif /* AGENTC_PROTOCOL_H */
