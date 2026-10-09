/* proto.c — the framed agentc protocol v1 envelope (P5 Axis C).
 *
 * See include/protocol.h for the schema. Decoding validates structure, rejects
 * unknown properties and requires the JSON vocabulary for payloads. Encoding
 * writes canonical-key-order envelopes.
 */
#include "protocol.h"
#include "plat.h"

#define P_EINVAL (-22)
#define P_EPROTO (-71)
#define P_ENOTSUP (-95)

/* ------------------------------------------------------------- error codes */
/* The closed v1 set: the RPC machine codes plus the protocol-level ones. */
static const char *const proto_codes[] = {
    "unknown_command",
    "invalid_json",
    "missing_command",
    "missing_text",
    "missing_name",
    "unknown_template",
    "invalid_level",
    "missing_field",
    "unknown_provider",
    "busy",
    "out_of_memory",
    "no_session",
    "cancelled",
    "session_failed",
    "adapter_unavailable",
    "actor_not_found",
    "no_result",
    "failed",
    "denied",
    "invalid_response",
    "unsupported",
    "protocol",
    "unauthorized",
    "version_mismatch",
    "too_large",
    "not_found",
    "error",
    "internal",
};

bool agentc_proto_code_valid(const char *code, size_t n) {
    if (!code)
        return false;
    for (size_t i = 0; i < sizeof proto_codes / sizeof proto_codes[0]; i++)
        if (agentc_str_eq(code, n, proto_codes[i], agentc_strlen(proto_codes[i])))
            return true;
    return false;
}

/* ------------------------------------------------------------------- UUID */
static void put_hex(char *out, u8 v) {
    static const char h[] = "0123456789abcdef";
    out[0] = h[v >> 4];
    out[1] = h[v & 0xf];
}

void agentc_proto_uuid(char out[37]) {
    u8 b[16];
    int r = os_random(b, sizeof b);
    if (r < 0)
        agentc_memset(b, 0, sizeof b);
    b[6] = (u8)((b[6] & 0x0fu) | 0x40u); /* version 4 */
    b[8] = (u8)((b[8] & 0x3fu) | 0x80u); /* variant 10 */
    size_t o = 0;
    for (size_t i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[o++] = '-';
        put_hex(out + o, b[i]);
        o += 2;
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------- decoding */
/* Match a text key against a NULL-terminated list; returns the 1-based index or
 * 0. */
static size_t match_key(const AgcCbor *k, const char *const *names) {
    if (!k || agentc_cbor_type(k) != AGENTC_CBOR_TEXT)
        return 0;
    size_t kn = agentc_cbor_len(k);
    if (kn == 0 || kn > AGENTC_PROTO_MAX_ID)
        return 0;
    for (size_t i = 0; names[i]; i++) {
        size_t n = agentc_strlen(names[i]);
        if (n == kn && agentc_memeq(agentc_cbor_text(k, NULL), names[i], n))
            return i + 1;
    }
    return 0;
}

/* Scan the map, rejecting unknown keys and marking seen ones. */
static int scan_fields(const AgcCbor *m, const char *const *names, bool *seen, size_t nseen) {
    for (size_t i = 0; i < agentc_cbor_len(m); i++) {
        size_t idx = match_key(agentc_cbor_key(m, i), names);
        if (idx == 0 || idx > nseen)
            return P_EPROTO;
        if (seen[idx - 1])
            return P_EPROTO;
        seen[idx - 1] = true;
    }
    return 0;
}

static bool is_json_or_absent(const AgcCbor *v) {
    return v == NULL || agentc_cbor_is_json(v);
}

static int check_id(const AgcCbor *v) {
    if (!v)
        return P_EPROTO;
    int t = agentc_cbor_type(v);
    if (t == AGENTC_CBOR_UINT)
        return 0;
    if (t == AGENTC_CBOR_TEXT) {
        size_t n = agentc_cbor_len(v);
        if (n == 0 || n > AGENTC_PROTO_MAX_ID)
            return P_EPROTO;
        return 0;
    }
    return P_EPROTO;
}

static const char *text_or_null(const AgcCbor *v, size_t *len) {
    if (!v || agentc_cbor_type(v) != AGENTC_CBOR_TEXT) {
        if (len)
            *len = 0;
        return NULL;
    }
    return (const char *)agentc_cbor_text(v, len);
}

static bool valid_errobj(const AgcCbor *e) {
    if (!e || agentc_cbor_type(e) != AGENTC_CBOR_MAP)
        return false;
    AgcCbor *code = agentc_cbor_get(e, "code");
    AgcCbor *msg = agentc_cbor_get(e, "message");
    size_t n = 0;
    const char *c = text_or_null(code, &n);
    return c != NULL && n != 0 && n <= AGENTC_PROTO_MAX_ID && agentc_proto_code_valid(c, n) &&
           msg != NULL && agentc_cbor_type(msg) == AGENTC_CBOR_TEXT && agentc_cbor_len(e) == 2;
}

static int decode_hello(const AgcCbor *m, AgcProtoMsg *o) {
    const char *const names[] = {"kind", "version", "client", "token", NULL};
    bool seen[4] = {false};
    if (scan_fields(m, names, seen, 4) != 0)
        return P_EPROTO;
    if (!seen[1])
        return P_EPROTO;
    AgcCbor *v = agentc_cbor_get(m, "version");
    if (!v || agentc_cbor_type(v) != AGENTC_CBOR_UINT)
        return P_EPROTO;
    o->kind = AGENTC_PROTO_HELLO;
    o->version = (u32)agentc_cbor_uint(v);
    o->client = text_or_null(agentc_cbor_get(m, "client"), NULL);
    o->token = text_or_null(agentc_cbor_get(m, "token"), NULL);
    return 0;
}

static int decode_hello_ack(const AgcCbor *m, AgcProtoMsg *o) {
    const char *const names[] = {"kind", "version", "server_id", "session_id", "features", NULL};
    bool seen[5] = {false};
    if (scan_fields(m, names, seen, 5) != 0)
        return P_EPROTO;
    if (!seen[1] || !seen[2])
        return P_EPROTO;
    AgcCbor *v = agentc_cbor_get(m, "version");
    if (!v || agentc_cbor_type(v) != AGENTC_CBOR_UINT)
        return P_EPROTO;
    AgcCbor *sid = agentc_cbor_get(m, "server_id");
    if (!sid || agentc_cbor_type(sid) != AGENTC_CBOR_TEXT)
        return P_EPROTO;
    AgcCbor *feat = agentc_cbor_get(m, "features");
    if (feat && agentc_cbor_type(feat) != AGENTC_CBOR_ARRAY)
        return P_EPROTO;
    if (feat) {
        for (size_t i = 0; i < agentc_cbor_len(feat); i++)
            if (agentc_cbor_type(agentc_cbor_at(feat, i)) != AGENTC_CBOR_TEXT)
                return P_EPROTO;
    }
    o->kind = AGENTC_PROTO_HELLO_ACK;
    o->version = (u32)agentc_cbor_uint(v);
    o->server_id = (const char *)agentc_cbor_text(sid, NULL);
    o->session_id = text_or_null(agentc_cbor_get(m, "session_id"), NULL);
    o->nfeatures = 0;
    if (feat) {
        for (size_t i = 0; i < agentc_cbor_len(feat) && o->nfeatures < 8; i++) {
            const u8 *fp = agentc_cbor_text(agentc_cbor_at(feat, i), NULL);
            if (fp)
                o->features[o->nfeatures++] = (const char *)fp;
        }
    }
    return 0;
}

static int decode_hello_error(const AgcCbor *m, AgcProtoMsg *o) {
    const char *const names[] = {"kind", "code", "message", NULL};
    bool seen[3] = {false};
    if (scan_fields(m, names, seen, 3) != 0)
        return P_EPROTO;
    if (!seen[1] || !seen[2])
        return P_EPROTO;
    AgcCbor *code = agentc_cbor_get(m, "code");
    AgcCbor *msg = agentc_cbor_get(m, "message");
    size_t cn = 0;
    const char *c = text_or_null(code, &cn);
    if (!c || cn == 0 || cn > AGENTC_PROTO_MAX_ID || !agentc_proto_code_valid(c, cn))
        return P_EPROTO;
    if (!msg || agentc_cbor_type(msg) != AGENTC_CBOR_TEXT)
        return P_EPROTO;
    o->kind = AGENTC_PROTO_HELLO_ERROR;
    o->error_code = c;
    o->error_code_len = cn;
    o->error_message = (const char *)agentc_cbor_text(msg, &o->error_message_len);
    return 0;
}

static int decode_request(const AgcCbor *m, AgcProtoMsg *o) {
    const char *const names[] = {"kind", "id", "method", "params", NULL};
    bool seen[4] = {false};
    if (scan_fields(m, names, seen, 4) != 0)
        return P_EPROTO;
    if (!seen[1] || !seen[2])
        return P_EPROTO;
    AgcCbor *id = agentc_cbor_get(m, "id");
    if (check_id(id) != 0)
        return P_EPROTO;
    AgcCbor *method = agentc_cbor_get(m, "method");
    size_t mn = 0;
    const char *mp = text_or_null(method, &mn);
    if (!mp || mn == 0 || mn > AGENTC_PROTO_MAX_ID)
        return P_EPROTO;
    AgcCbor *params = agentc_cbor_get(m, "params");
    if (!is_json_or_absent(params))
        return P_ENOTSUP;
    o->kind = AGENTC_PROTO_REQUEST;
    o->id = id;
    o->method = mp;
    o->method_len = mn;
    o->params = params;
    return 0;
}

static int decode_response(const AgcCbor *m, AgcProtoMsg *o) {
    const char *const names[] = {"kind", "id", "ok", "result", "error", NULL};
    bool seen[5] = {false};
    if (scan_fields(m, names, seen, 5) != 0)
        return P_EPROTO;
    if (!seen[1] || !seen[2])
        return P_EPROTO;
    AgcCbor *id = agentc_cbor_get(m, "id");
    if (check_id(id) != 0)
        return P_EPROTO;
    AgcCbor *okv = agentc_cbor_get(m, "ok");
    if (!okv || agentc_cbor_type(okv) != AGENTC_CBOR_BOOL)
        return P_EPROTO;
    bool ok = agentc_cbor_bool(okv);
    AgcCbor *result = agentc_cbor_get(m, "result");
    AgcCbor *error = agentc_cbor_get(m, "error");
    if (ok) {
        if (!is_json_or_absent(result))
            return P_ENOTSUP;
    } else if (!valid_errobj(error)) {
        return P_EPROTO;
    }
    o->kind = AGENTC_PROTO_RESPONSE;
    o->id = id;
    o->ok = ok;
    o->result = result;
    if (!ok) {
        o->error_code =
            (const char *)agentc_cbor_text(agentc_cbor_get(error, "code"), &o->error_code_len);
        o->error_message = (const char *)agentc_cbor_text(agentc_cbor_get(error, "message"),
                                                          &o->error_message_len);
    }
    return 0;
}

static int decode_cancel(const AgcCbor *m, AgcProtoMsg *o) {
    const char *const names[] = {"kind", "id", NULL};
    bool seen[2] = {false};
    if (scan_fields(m, names, seen, 2) != 0)
        return P_EPROTO;
    if (!seen[1])
        return P_EPROTO;
    AgcCbor *id = agentc_cbor_get(m, "id");
    if (check_id(id) != 0)
        return P_EPROTO;
    o->kind = AGENTC_PROTO_CANCEL;
    o->id = id;
    return 0;
}

static int decode_event(const AgcCbor *m, AgcProtoMsg *o) {
    const char *const names[] = {"kind", "seq", "session_id", "event", NULL};
    bool seen[4] = {false};
    if (scan_fields(m, names, seen, 4) != 0)
        return P_EPROTO;
    if (!seen[1] || !seen[3])
        return P_EPROTO;
    AgcCbor *seq = agentc_cbor_get(m, "seq");
    if (!seq || agentc_cbor_type(seq) != AGENTC_CBOR_UINT)
        return P_EPROTO;
    AgcCbor *sid = agentc_cbor_get(m, "session_id");
    if (sid && agentc_cbor_type(sid) != AGENTC_CBOR_TEXT)
        return P_EPROTO;
    AgcCbor *ev = agentc_cbor_get(m, "event");
    if (!is_json_or_absent(ev))
        return P_ENOTSUP;
    o->kind = AGENTC_PROTO_EVENT;
    o->seq = agentc_cbor_uint(seq);
    o->session_id = text_or_null(sid, NULL);
    o->event = ev;
    return 0;
}

/* Dialog method names (P5 C2 UI). */
static const char *const ui_methods[] = {"select", "confirm", "input", "editor"};
bool agentc_proto_ui_method_valid(const char *method, size_t n) {
    if (!method || n == 0 || n > AGENTC_PROTO_MAX_ID)
        return false;
    for (size_t i = 0; i < sizeof ui_methods / sizeof ui_methods[0]; i++)
        if (agentc_str_eq(method, n, ui_methods[i], agentc_strlen(ui_methods[i])))
            return true;
    return false;
}

/* ui_request: {kind, id:<uint>, method:<dialog>, params?:<json>}. The dialog id
 * is a bounded server sequence, so text ids are rejected. */
static int decode_ui_request(const AgcCbor *m, AgcProtoMsg *o) {
    const char *const names[] = {"kind", "id", "method", "params", NULL};
    bool seen[4] = {false};
    if (scan_fields(m, names, seen, 4) != 0)
        return P_EPROTO;
    if (!seen[1] || !seen[2])
        return P_EPROTO;
    AgcCbor *id = agentc_cbor_get(m, "id");
    if (!id || agentc_cbor_type(id) != AGENTC_CBOR_UINT)
        return P_EPROTO;
    AgcCbor *method = agentc_cbor_get(m, "method");
    size_t mn = 0;
    const char *mp = text_or_null(method, &mn);
    if (!agentc_proto_ui_method_valid(mp, mn))
        return P_EPROTO;
    AgcCbor *params = agentc_cbor_get(m, "params");
    if (params && (!is_json_or_absent(params) || agentc_cbor_type(params) != AGENTC_CBOR_MAP))
        return P_ENOTSUP;
    o->kind = AGENTC_PROTO_UI_REQUEST;
    o->id = id;
    o->method = mp;
    o->method_len = mn;
    o->params = params;
    return 0;
}

/* ui_response: {kind, id:<uint>, ok:<bool>, result?:<json>}. A denied/absent
 * answer (ok false) carries no result. */
static int decode_ui_response(const AgcCbor *m, AgcProtoMsg *o) {
    const char *const names[] = {"kind", "id", "ok", "result", NULL};
    bool seen[4] = {false};
    if (scan_fields(m, names, seen, 4) != 0)
        return P_EPROTO;
    if (!seen[1] || !seen[2])
        return P_EPROTO;
    AgcCbor *id = agentc_cbor_get(m, "id");
    if (!id || agentc_cbor_type(id) != AGENTC_CBOR_UINT)
        return P_EPROTO;
    AgcCbor *okv = agentc_cbor_get(m, "ok");
    if (!okv || agentc_cbor_type(okv) != AGENTC_CBOR_BOOL)
        return P_EPROTO;
    bool ok = agentc_cbor_bool(okv);
    AgcCbor *result = agentc_cbor_get(m, "result");
    if (ok) {
        if (!is_json_or_absent(result))
            return P_ENOTSUP;
    } else if (result != NULL) {
        return P_EPROTO; /* a deny carries no result */
    }
    o->kind = AGENTC_PROTO_UI_RESPONSE;
    o->id = id;
    o->ok = ok;
    o->result = result;
    return 0;
}

/* attach: {kind, id:<uint|text>, session?:<text>, events?:[<text>...]} (C3). */
static int decode_attach(const AgcCbor *m, AgcProtoMsg *o) {
    const char *const names[] = {"kind", "id", "session", "events", NULL};
    bool seen[4] = {false};
    if (scan_fields(m, names, seen, 4) != 0)
        return P_EPROTO;
    if (!seen[1])
        return P_EPROTO;
    AgcCbor *id = agentc_cbor_get(m, "id");
    if (check_id(id) != 0)
        return P_EPROTO;
    AgcCbor *sess = agentc_cbor_get(m, "session");
    if (sess) {
        if (agentc_cbor_type(sess) != AGENTC_CBOR_TEXT)
            return P_EPROTO;
        size_t n = agentc_cbor_len(sess);
        if (n == 0 || n > AGENTC_PROTO_MAX_ID)
            return P_EPROTO;
    }
    AgcCbor *evs = agentc_cbor_get(m, "events");
    if (evs) {
        if (agentc_cbor_type(evs) != AGENTC_CBOR_ARRAY)
            return P_EPROTO;
        if (agentc_cbor_len(evs) > AGENTC_PROTO_MAX_ATTACH_EVENTS)
            return P_EPROTO;
        for (size_t i = 0; i < agentc_cbor_len(evs); i++) {
            AgcCbor *e = agentc_cbor_at(evs, i);
            if (agentc_cbor_type(e) != AGENTC_CBOR_TEXT)
                return P_EPROTO;
            size_t n = agentc_cbor_len(e);
            if (n == 0 || n > AGENTC_PROTO_MAX_ID)
                return P_EPROTO;
        }
    }
    o->kind = AGENTC_PROTO_ATTACH;
    o->id = id;
    o->session = sess ? (const char *)agentc_cbor_text(sess, NULL) : NULL;
    o->nevents = 0;
    if (evs)
        for (size_t i = 0; i < agentc_cbor_len(evs); i++)
            o->events[o->nevents++] = (const char *)agentc_cbor_text(agentc_cbor_at(evs, i), NULL);
    return 0;
}

/* detach: {kind, id:<uint|text>} (C3). */
static int decode_detach(const AgcCbor *m, AgcProtoMsg *o) {
    const char *const names[] = {"kind", "id", NULL};
    bool seen[2] = {false};
    if (scan_fields(m, names, seen, 2) != 0)
        return P_EPROTO;
    if (!seen[1])
        return P_EPROTO;
    AgcCbor *id = agentc_cbor_get(m, "id");
    if (check_id(id) != 0)
        return P_EPROTO;
    o->kind = AGENTC_PROTO_DETACH;
    o->id = id;
    return 0;
}

int agentc_proto_decode(const AgcCbor *v, AgcProtoMsg *out) {
    if (!out)
        return P_EINVAL;
    agentc_memset(out, 0, sizeof *out);
    if (!v || agentc_cbor_type(v) != AGENTC_CBOR_MAP)
        return P_EINVAL;
    AgcCbor *kind = agentc_cbor_get(v, "kind");
    if (!kind || agentc_cbor_type(kind) != AGENTC_CBOR_TEXT)
        return P_EPROTO;
    size_t kn = 0;
    const char *k = (const char *)agentc_cbor_text(kind, &kn);
    struct {
        const char *name;
        int kind;
        int (*fn)(const AgcCbor *, AgcProtoMsg *);
    } table[] = {
        {"hello", AGENTC_PROTO_HELLO, decode_hello},
        {"hello_ack", AGENTC_PROTO_HELLO_ACK, decode_hello_ack},
        {"hello_error", AGENTC_PROTO_HELLO_ERROR, decode_hello_error},
        {"request", AGENTC_PROTO_REQUEST, decode_request},
        {"response", AGENTC_PROTO_RESPONSE, decode_response},
        {"cancel", AGENTC_PROTO_CANCEL, decode_cancel},
        {"event", AGENTC_PROTO_EVENT, decode_event},
        {"ui_request", AGENTC_PROTO_UI_REQUEST, decode_ui_request},
        {"ui_response", AGENTC_PROTO_UI_RESPONSE, decode_ui_response},
        {"attach", AGENTC_PROTO_ATTACH, decode_attach},
        {"detach", AGENTC_PROTO_DETACH, decode_detach},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (agentc_str_eq(k, kn, table[i].name, agentc_strlen(table[i].name)))
            return table[i].fn(v, out);
    return P_EPROTO; /* unknown kind: terminal */
}

/* ---------------------------------------------------------------- encoding */
static void ekey(AgcCborW *w, const char *s) {
    agentc_cborw_text(w, s, agentc_strlen(s));
}

void agentc_proto_enc_hello(AgcCborW *w, u32 version, const char *client, const char *token) {
    size_t n = 2 + (client ? 1 : 0) + (token ? 1 : 0);
    agentc_cborw_map(w, n);
    ekey(w, "kind");
    agentc_cborw_text(w, "hello", 5);
    ekey(w, "version");
    agentc_cborw_uint(w, version);
    if (client) {
        ekey(w, "client");
        agentc_cborw_text(w, client, agentc_strlen(client));
    }
    if (token) {
        ekey(w, "token");
        agentc_cborw_text(w, token, agentc_strlen(token));
    }
}

void agentc_proto_enc_hello_ack(AgcCborW *w, u32 version, const char *server_id,
                                const char *session_id, const char *const *features,
                                size_t nfeatures) {
    size_t n = 4 + (session_id ? 1 : 0);
    agentc_cborw_map(w, n);
    ekey(w, "kind");
    agentc_cborw_text(w, "hello_ack", 9);
    ekey(w, "version");
    agentc_cborw_uint(w, version);
    ekey(w, "server_id");
    agentc_cborw_text(w, server_id, agentc_strlen(server_id));
    if (session_id) {
        ekey(w, "session_id");
        agentc_cborw_text(w, session_id, agentc_strlen(session_id));
    }
    ekey(w, "features");
    agentc_cborw_array(w, nfeatures);
    for (size_t i = 0; i < nfeatures; i++)
        agentc_cborw_text(w, features[i], agentc_strlen(features[i]));
}

void agentc_proto_enc_hello_error(AgcCborW *w, const char *code, const char *message) {
    agentc_cborw_map(w, 3);
    ekey(w, "kind");
    agentc_cborw_text(w, "hello_error", 11);
    ekey(w, "code");
    agentc_cborw_text(w, code, agentc_strlen(code));
    ekey(w, "message");
    agentc_cborw_text(w, message, agentc_strlen(message));
}

void agentc_proto_enc_request(AgcCborW *w, const AgcCbor *id, const char *method, size_t method_len,
                              const AgcCbor *params) {
    agentc_cborw_map(w, params ? 4 : 3);
    ekey(w, "kind");
    agentc_cborw_text(w, "request", 7);
    ekey(w, "id");
    agentc_cborw_value(w, id);
    ekey(w, "method");
    agentc_cborw_text(w, method, method_len);
    if (params) {
        ekey(w, "params");
        agentc_cborw_value(w, params);
    }
}

void agentc_proto_enc_response(AgcCborW *w, const AgcCbor *id, bool ok, const AgcCbor *result,
                               const char *code, const char *message) {
    agentc_cborw_map(w, 4);
    ekey(w, "kind");
    agentc_cborw_text(w, "response", 8);
    ekey(w, "id");
    agentc_cborw_value(w, id);
    ekey(w, "ok");
    agentc_cborw_bool(w, ok);
    if (ok) {
        ekey(w, "result");
        if (result)
            agentc_cborw_value(w, result);
        else
            agentc_cborw_null(w);
    } else {
        ekey(w, "error");
        agentc_cborw_map(w, 2);
        ekey(w, "code");
        agentc_cborw_text(w, code ? code : "error", agentc_strlen(code ? code : "error"));
        ekey(w, "message");
        agentc_cborw_text(w, message ? message : "", agentc_strlen(message ? message : ""));
    }
}

void agentc_proto_enc_cancel(AgcCborW *w, const AgcCbor *id) {
    agentc_cborw_map(w, 2);
    ekey(w, "kind");
    agentc_cborw_text(w, "cancel", 6);
    ekey(w, "id");
    agentc_cborw_value(w, id);
}

void agentc_proto_enc_event(AgcCborW *w, u64 seq, const AgcCbor *event) {
    agentc_proto_enc_event_sid(w, seq, NULL, event);
}

void agentc_proto_enc_event_sid(AgcCborW *w, u64 seq, const char *session_id,
                                const AgcCbor *event) {
    size_t n = 3 + (session_id ? 1 : 0);
    agentc_cborw_map(w, n);
    ekey(w, "kind");
    agentc_cborw_text(w, "event", 5);
    ekey(w, "seq");
    agentc_cborw_uint(w, seq);
    if (session_id) {
        ekey(w, "session_id");
        agentc_cborw_text(w, session_id, agentc_strlen(session_id));
    }
    ekey(w, "event");
    agentc_cborw_value(w, event);
}

void agentc_proto_enc_ui_request(AgcCborW *w, u64 id, const char *method, const AgcCbor *params) {
    agentc_cborw_map(w, params ? 4 : 3);
    ekey(w, "kind");
    agentc_cborw_text(w, "ui_request", 10);
    ekey(w, "id");
    agentc_cborw_uint(w, id);
    ekey(w, "method");
    agentc_cborw_text(w, method, agentc_strlen(method));
    if (params) {
        ekey(w, "params");
        agentc_cborw_value(w, params);
    }
}

void agentc_proto_enc_ui_response(AgcCborW *w, u64 id, bool ok, const AgcCbor *result) {
    size_t n = 3 + (result ? 1 : 0);
    agentc_cborw_map(w, n);
    ekey(w, "kind");
    agentc_cborw_text(w, "ui_response", 11);
    ekey(w, "id");
    agentc_cborw_uint(w, id);
    ekey(w, "ok");
    agentc_cborw_bool(w, ok);
    if (result) {
        ekey(w, "result");
        agentc_cborw_value(w, result);
    }
}

void agentc_proto_enc_attach(AgcCborW *w, const AgcCbor *id, const char *session,
                             const char *const *events, size_t nevents) {
    size_t n = 2 + (session ? 1 : 0) + (nevents ? 1 : 0);
    agentc_cborw_map(w, n);
    ekey(w, "kind");
    agentc_cborw_text(w, "attach", 6);
    ekey(w, "id");
    agentc_cborw_value(w, id);
    if (session) {
        ekey(w, "session");
        agentc_cborw_text(w, session, agentc_strlen(session));
    }
    if (nevents) {
        ekey(w, "events");
        agentc_cborw_array(w, nevents);
        for (size_t i = 0; i < nevents; i++)
            agentc_cborw_text(w, events[i], agentc_strlen(events[i]));
    }
}

void agentc_proto_enc_detach(AgcCborW *w, const AgcCbor *id) {
    agentc_cborw_map(w, 2);
    ekey(w, "kind");
    agentc_cborw_text(w, "detach", 6);
    ekey(w, "id");
    agentc_cborw_value(w, id);
}
