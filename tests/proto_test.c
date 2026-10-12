/* proto_test.c — golden trace for the framed protocol v1 envelopes.
 *
 * Encodes each envelope, decodes it back, asserts the fields, and exercises the
 * unknown-property, unknown-kind and JSON-vocabulary rejections. Golden:
 * tests/data/proto_test.expected.
 */
#include "protocol.h"
#include "cbor_json.h"

static int fails;

/* Build values live in B; decoding clears D, so the two never alias. */
static AgcCborArena *B;
static AgcCborArena *D;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

static int dec(const u8 *wire, size_t n, AgcProtoMsg *msg) {
    int err = 0;
    AgcCbor *v = agentc_cbor_decode(D, wire, n, NULL, &err);
    if (!v)
        return err;
    return agentc_proto_decode(v, msg);
}

static void test_hello(void) {
    AgcBuf b = {0};
    AgcCborW w;
    agentc_cborw_init(&w, &b);
    agentc_proto_enc_hello(&w, 1, "agentc-client/1", "secret");
    check("proto.hello.write", !w.failed);
    AgcProtoMsg m;
    check("proto.hello.decode", dec(b.p, b.len, &m) == 0);
    check("proto.hello.kind", m.kind == AGENTC_PROTO_HELLO);
    check("proto.hello.version", m.version == 1);
    check("proto.hello.client", agentc_streq(m.client, "agentc-client/1"));
    check("proto.hello.token", agentc_streq(m.token, "secret"));
    agentc_buf_free(&b);
}

static void test_hello_ack(void) {
    AgcBuf b = {0};
    AgcCborW w;
    agentc_cborw_init(&w, &b);
    const char *feats[] = {"cancel"};
    agentc_proto_enc_hello_ack(&w, 1, "12345678-1234-4234-8234-123456789abc", "sess-1", feats, 1);
    AgcProtoMsg m;
    check("proto.ack.decode", dec(b.p, b.len, &m) == 0);
    check("proto.ack.kind", m.kind == AGENTC_PROTO_HELLO_ACK);
    check("proto.ack.server", agentc_streq(m.server_id, "12345678-1234-4234-8234-123456789abc"));
    check("proto.ack.session", agentc_streq(m.session_id, "sess-1"));
    check("proto.ack.features", m.nfeatures == 1 && agentc_streq(m.features[0], "cancel"));
    agentc_buf_free(&b);
}

static void test_request(void) {
    int err = 0;
    AgcCbor *params = agentc_cbor_from_json(B, "{\"text\":\"hi\"}", 13, &err);
    AgcCbor *id = agentc_cbor_new_uint(B, 7);
    AgcBuf b = {0};
    AgcCborW w;
    agentc_cborw_init(&w, &b);
    agentc_proto_enc_request(&w, id, "prompt", 6, params);
    AgcProtoMsg m;
    check("proto.req.decode", dec(b.p, b.len, &m) == 0);
    check("proto.req.kind", m.kind == AGENTC_PROTO_REQUEST);
    check("proto.req.id", agentc_cbor_uint(m.id) == 7);
    check("proto.req.method", agentc_str_eq(m.method, m.method_len, "prompt", 6));
    check("proto.req.params", m.params && agentc_cbor_is_json(m.params));
    agentc_buf_free(&b);

    /* string id */
    AgcCbor *sid = agentc_cbor_new_text(B, "abc", 3);
    agentc_cborw_init(&w, &b);
    agentc_proto_enc_request(&w, sid, "get_state", 9, NULL);
    check("proto.req.str.decode", dec(b.p, b.len, &m) == 0);
    check("proto.req.str.id", agentc_cbor_type(m.id) == AGENTC_CBOR_TEXT);
    agentc_buf_free(&b);
}

static void test_rejections(void) {
    int err = 0;
    const char *bad_json;
    AgcProtoMsg m;

    bad_json = "{\"kind\":\"request\",\"id\":1,\"method\":\"x\",\"extra\":1}";
    AgcCbor *bad = agentc_cbor_from_json(B, bad_json, agentc_strlen(bad_json), &err);
    check("proto.rej.unknown_prop", agentc_proto_decode(bad, &m) == -71);

    /* request params outside the JSON vocabulary (bytes) */
    AgcCbor *bytes = agentc_cbor_new_bytes(B, "\x01", 1);
    AgcCbor *r = agentc_cbor_new_map(B, 4);
    agentc_cbor_map_set(r, 0, agentc_cbor_new_text(B, "kind", 4),
                        agentc_cbor_new_text(B, "request", 7));
    agentc_cbor_map_set(r, 1, agentc_cbor_new_text(B, "id", 2), agentc_cbor_new_uint(B, 1));
    agentc_cbor_map_set(r, 2, agentc_cbor_new_text(B, "method", 6),
                        agentc_cbor_new_text(B, "x", 1));
    agentc_cbor_map_set(r, 3, agentc_cbor_new_text(B, "params", 6), bytes);
    check("proto.rej.bytes_params", agentc_proto_decode(r, &m) == -95);

    /* unknown kind is terminal */
    bad_json = "{\"kind\":\"nope\"}";
    bad = agentc_cbor_from_json(B, bad_json, agentc_strlen(bad_json), &err);
    check("proto.rej.unknown_kind", agentc_proto_decode(bad, &m) == -71);

    /* ui_request: the dialog id must be a uint, the method must be in the
     * closed dialog set, and a deny ui_response may not carry a result */
    bad_json = "{\"kind\":\"ui_request\",\"id\":\"x\",\"method\":\"confirm\"}";
    bad = agentc_cbor_from_json(B, bad_json, agentc_strlen(bad_json), &err);
    check("proto.rej.ui_text_id", agentc_proto_decode(bad, &m) == -71);
    bad_json = "{\"kind\":\"ui_request\",\"id\":1,\"method\":\"bogus\"}";
    bad = agentc_cbor_from_json(B, bad_json, agentc_strlen(bad_json), &err);
    check("proto.rej.ui_method", agentc_proto_decode(bad, &m) == -71);
    bad_json = "{\"kind\":\"ui_response\",\"id\":1,\"ok\":false,\"result\":true}";
    bad = agentc_cbor_from_json(B, bad_json, agentc_strlen(bad_json), &err);
    check("proto.rej.ui_deny_result", agentc_proto_decode(bad, &m) == -71);

    /* a bad error code in a response */
    bad_json = "{\"kind\":\"response\",\"id\":1,\"ok\":false,"
               "\"error\":{\"code\":\"bogus\",\"message\":\"x\"}}";
    bad = agentc_cbor_from_json(B, bad_json, agentc_strlen(bad_json), &err);
    check("proto.rej.err_code", agentc_proto_decode(bad, &m) == -71);

    /* a response with ok false but no error object */
    bad_json = "{\"kind\":\"response\",\"id\":1,\"ok\":false}";
    bad = agentc_cbor_from_json(B, bad_json, agentc_strlen(bad_json), &err);
    check("proto.rej.no_error", agentc_proto_decode(bad, &m) == -71);

    /* a hello with no version */
    bad_json = "{\"kind\":\"hello\"}";
    bad = agentc_cbor_from_json(B, bad_json, agentc_strlen(bad_json), &err);
    check("proto.rej.no_version", agentc_proto_decode(bad, &m) == -71);
}

static void test_response_event(void) {
    int err = 0;
    AgcCbor *result = agentc_cbor_from_json(B, "{\"ok\":true}", 11, &err);
    AgcCbor *id = agentc_cbor_new_uint(B, 42);
    AgcBuf b = {0};
    AgcCborW w;
    agentc_cborw_init(&w, &b);
    agentc_proto_enc_response(&w, id, true, result, NULL, NULL);
    AgcProtoMsg m;
    check("proto.resp.decode", dec(b.p, b.len, &m) == 0);
    check("proto.resp.kind", m.kind == AGENTC_PROTO_RESPONSE);
    check("proto.resp.ok", m.ok);
    check("proto.resp.result", m.result && agentc_cbor_is_json(m.result));
    agentc_buf_free(&b);

    agentc_cborw_init(&w, &b);
    agentc_proto_enc_response(&w, id, false, NULL, "busy", "too many");
    check("proto.resp.err.decode", dec(b.p, b.len, &m) == 0);
    check("proto.resp.err.ok", !m.ok);
    check("proto.resp.err.code", agentc_str_eq(m.error_code, m.error_code_len, "busy", 4));
    check("proto.resp.err.msg", agentc_str_eq(m.error_message, m.error_message_len, "too many", 8));
    agentc_buf_free(&b);

    AgcCbor *ev = agentc_cbor_from_json(B, "{\"type\":\"agent_settled\"}", 24, &err);
    agentc_cborw_init(&w, &b);
    agentc_proto_enc_event(&w, 3, ev);
    check("proto.event.decode", dec(b.p, b.len, &m) == 0);
    check("proto.event.kind", m.kind == AGENTC_PROTO_EVENT);
    check("proto.event.seq", m.seq == 3);
    check("proto.event.payload", m.event != NULL);
    agentc_buf_free(&b);

    agentc_cborw_init(&w, &b);
    AgcCbor *cid = agentc_cbor_new_uint(B, 42);
    agentc_proto_enc_cancel(&w, cid);
    check("proto.cancel.decode", dec(b.p, b.len, &m) == 0);
    check("proto.cancel.kind", m.kind == AGENTC_PROTO_CANCEL);
    agentc_buf_free(&b);
}

static void test_ui(void) {
    int err = 0;
    const char *pjson = "{\"prompt\":\"pick\",\"options\":[\"a\",\"b\"]}";
    AgcCbor *params = agentc_cbor_from_json(B, pjson, agentc_strlen(pjson), &err);
    AgcBuf b = {0};
    AgcCborW w;
    agentc_cborw_init(&w, &b);
    agentc_proto_enc_ui_request(&w, 5, "select", params);
    AgcProtoMsg m;
    check("proto.ui_req.decode", dec(b.p, b.len, &m) == 0);
    check("proto.ui_req.kind", m.kind == AGENTC_PROTO_UI_REQUEST);
    check("proto.ui_req.id", agentc_cbor_uint(m.id) == 5);
    check("proto.ui_req.method", agentc_str_eq(m.method, m.method_len, "select", 6));
    check("proto.ui_req.params", m.params && agentc_cbor_is_json(m.params));
    agentc_buf_free(&b);

    AgcCbor *val = agentc_cbor_new_text(B, "b", 1);
    agentc_cborw_init(&w, &b);
    agentc_proto_enc_ui_response(&w, 5, true, val);
    check("proto.ui_resp.decode", dec(b.p, b.len, &m) == 0);
    check("proto.ui_resp.kind", m.kind == AGENTC_PROTO_UI_RESPONSE);
    check("proto.ui_resp.ok", m.ok && m.result && agentc_cbor_is_json(m.result));
    agentc_buf_free(&b);

    agentc_cborw_init(&w, &b);
    agentc_proto_enc_ui_response(&w, 5, false, NULL);
    check("proto.ui_resp.deny", dec(b.p, b.len, &m) == 0 && !m.ok && m.result == NULL);
    agentc_buf_free(&b);
}

static void test_codes_uuid(void) {
    check("proto.code.ok", agentc_proto_code_valid("busy", 4));
    check("proto.code.bad", !agentc_proto_code_valid("nope", 4));
    char u[37];
    agentc_proto_uuid(u);
    check("proto.uuid.len", agentc_strlen(u) == 36);
    check("proto.uuid.dashes", u[8] == '-' && u[13] == '-' && u[18] == '-' && u[23] == '-');
    check("proto.uuid.version", u[14] == '4');
    check("proto.uuid.variant", u[19] == '8' || u[19] == '9' || u[19] == 'a' || u[19] == 'b');
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    B = agentc_cbor_arena_new(0);
    D = agentc_cbor_arena_new(0);
    test_hello();
    test_hello_ack();
    test_request();
    test_rejections();
    test_response_event();
    test_ui();
    test_codes_uuid();
    agentc_cbor_arena_free(B);
    agentc_cbor_arena_free(D);
    return fails;
}
