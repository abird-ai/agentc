/* ws_test.c — golden trace for the RFC 6455 WebSocket transport shim (P5 C3).
 *
 * Covers the handshake accept computation, header parsing/limits, masked
 * client framing, fragmentation + interleaved control frames, the cap
 * (validate-before-copy), unmasked/RSV/text/control violations, close, and
 * server-frame encoding. Golden: tests/data/ws_test.expected.
 */
#include "ws.h"
#include "wire.h" /* appended I2 handshake policy API */

static int fails;

/* Zeroed scratch for encoder length checks (never NULL with n > 0). */
static u8 zeros[70000];

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

static void dump_hex(const void *p, size_t n) {
    static const char hex[] = "0123456789abcdef";
    const u8 *b = p;
    for (size_t i = 0; i < n; i++)
        agentc_outf("%c%c", hex[b[i] >> 4], hex[b[i] & 0xf]);
    agentc_out_nl();
}

/* A client frame with a fixed masking key (0x11223344). */
static void put_client(AgcBuf *b, int opcode, bool fin, const void *payload, size_t len) {
    agentc_buf_byte(b, (u8)((fin ? 0x80 : 0x00) | (unsigned)(opcode & 0xf)));
    if (len < 126) {
        agentc_buf_byte(b, (u8)(0x80 | len));
    } else if (len <= 0xFFFF) {
        agentc_buf_byte(b, 0x80 | 126);
        agentc_buf_byte(b, (u8)(len >> 8));
        agentc_buf_byte(b, (u8)len);
    } else {
        agentc_buf_byte(b, 0x80 | 127);
        for (int i = 7; i >= 0; i--)
            agentc_buf_byte(b, (u8)((u64)len >> (8 * i)));
    }
    const u8 key[4] = {0x11, 0x22, 0x33, 0x44};
    agentc_buf_push(b, key, 4);
    const u8 *p = payload;
    for (size_t i = 0; i < len; i++)
        agentc_buf_byte(b, (u8)(p[i] ^ key[i & 3]));
}

typedef struct {
    AgcBuf log;
    size_t count;
} Sink;

static int sink(void *ud, int opcode, const u8 *p, size_t n) {
    Sink *s = ud;
    const char *tag = opcode == AGENTC_WS_BINARY ? "bin:"
                      : opcode == AGENTC_WS_PING ? "ping:"
                      : opcode == AGENTC_WS_PONG ? "pong:"
                                                 : "close:";
    agentc_buf_cstr(&s->log, tag);
    if (n)
        agentc_buf_push(&s->log, p, n);
    agentc_buf_byte(&s->log, '|');
    s->count++;
    return 0;
}

/* Host/Origin policy predicate for the appended handshake API (I2): loopback
 * Host plus a matching loopback Origin (or no Origin at all, for non-browser
 * clients). */
static bool allow_loopback(void *ud, const AgcWsRequest *req) {
    (void)ud;
    if (!req->has_host || !agentc_streq(req->host, "127.0.0.1:8790"))
        return false;
    if (!req->has_origin)
        return true;
    return agentc_streq(req->origin, "http://127.0.0.1:8790");
}

static bool allow_any(void *ud, const AgcWsRequest *req) {
    (void)ud;
    (void)req;
    return true;
}

static void test_sha1_base64(void) {
    u8 d[20];
    agentc_ws_sha1("abc", 3, d);
    check("ws.sha1.abc", true);
    dump_hex(d, sizeof d); /* a9993e364706816aba3e25717850c26c9cd0d89d */
    agentc_ws_sha1("", 0, d);
    check("ws.sha1.empty", true);
    dump_hex(d, sizeof d); /* da39a3ee5e6b4b0d3255bfef95601890afd80709 */

    char b64[64];
    size_t n = agentc_ws_base64("abc", 3, b64, sizeof b64);
    check("ws.b64.abc", n == 4 && agentc_streq(b64, "YWJj"));
    const char *phrase = "any carnal pleasure.";
    n = agentc_ws_base64(phrase, agentc_strlen(phrase), b64, sizeof b64);
    check("ws.b64.phrase", agentc_streq(b64, "YW55IGNhcm5hbCBwbGVhc3VyZS4="));
    check("ws.b64.short", agentc_ws_base64("abc", 3, b64, 4) == 0);

    char acc[64];
    check("ws.accept.rfc", agentc_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", acc, sizeof acc) == 0);
    check("ws.accept.value", agentc_streq(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

static void test_handshake(void) {
    static const char REQ[] = "GET /agentc HTTP/1.1\r\n"
                              "Host: 127.0.0.1:8790\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: keep-alive, Upgrade\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                              "Sec-WebSocket-Version: 13\r\n"
                              "Sec-WebSocket-Protocol: agentc.v1\r\n"
                              "\r\n";

    /* incomplete: no reply */
    AgcBuf in = {0};
    AgcBuf out = {0};
    agentc_buf_push(&in, REQ, sizeof REQ - 1 - 8);
    check("ws.hs.incomplete", agentc_ws_server_handshake(&in, &out) == 0 && out.len == 0);

    /* split byte by byte up to completion; keep 3 bytes of a pipelined frame */
    AgcBuf pipelined = {0};
    put_client(&pipelined, AGENTC_WS_BINARY, true, "hi", 2);
    int r = 0;
    for (size_t i = sizeof REQ - 1 - 8; i < sizeof REQ - 1 && r == 0; i++) {
        agentc_buf_byte(&in, (u8)REQ[i]);
        r = agentc_ws_server_handshake(&in, &out);
    }
    agentc_buf_push(&in, pipelined.p, pipelined.len);
    check("ws.hs.accept", r == 1);
    check("ws.hs.leftover", in.len == pipelined.len);
    agentc_buf_byte(&out, 0);
    check("ws.hs.has_101", agentc_str_str((const char *)out.p, "101 Switching Protocols") != NULL);
    check("ws.hs.has_accept",
          agentc_str_str((const char *)out.p, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
    check("ws.hs.has_proto", agentc_str_str((const char *)out.p, "agentc.v1") != NULL);
    agentc_buf_free(&in);
    agentc_buf_free(&out);
    agentc_buf_free(&pipelined);

    /* the pipelined frame decodes with the accepted handshake's leftover */
    in = (AgcBuf){0};
    AgcBuf frame = {0};
    put_client(&frame, AGENTC_WS_BINARY, true, "left", 4);
    agentc_buf_push(&in, frame.p, frame.len);
    AgcWsDecoder dec;
    agentc_ws_decoder_init(&dec);
    Sink s = {0};
    int fr = agentc_ws_decoder_feed(&dec, in.p, in.len, sink, &s);
    check("ws.hs.pipeline.rc", fr == 0);
    check("ws.hs.pipeline.payload",
          agentc_str_eq_cstr((const char *)s.log.p, s.log.len, "bin:left|"));
    agentc_buf_free(&s.log);
    agentc_ws_decoder_free(&dec);
    agentc_buf_free(&in);
    agentc_buf_free(&frame);

    /* missing subprotocol: rejected 400 */
    in = (AgcBuf){0};
    out = (AgcBuf){0};
    const char *noproto = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                          "Sec-WebSocket-Version: 13\r\n\r\n";
    agentc_buf_push(&in, noproto, agentc_strlen(noproto));
    int nr = agentc_ws_server_handshake(&in, &out);
    check("ws.hs.noproto.reject", nr < 0);
    agentc_buf_byte(&out, 0);
    check("ws.hs.noproto.400", agentc_str_str((const char *)out.p, "400 Bad Request") != NULL);
    agentc_buf_free(&in);
    agentc_buf_free(&out);

    /* oversized header block: rejected before buffering without bound */
    in = (AgcBuf){0};
    out = (AgcBuf){0};
    for (size_t i = 0; i < AGENTC_WS_MAX_HEADER + 1; i++)
        agentc_buf_byte(&in, 'x');
    check("ws.hs.oversize", agentc_ws_server_handshake(&in, &out) == -90);
    agentc_buf_free(&in);
    agentc_buf_free(&out);

    /* the same block that *completes* in one read must not bypass the cap */
    in = (AgcBuf){0};
    out = (AgcBuf){0};
    for (size_t i = 0; i < AGENTC_WS_MAX_HEADER + 1; i++)
        agentc_buf_byte(&in, 'x');
    agentc_buf_cstr(&in, "\r\n\r\n");
    check("ws.hs.oversize.complete", agentc_ws_server_handshake(&in, &out) == -90);
    agentc_buf_free(&in);
    agentc_buf_free(&out);
}

static void feed_all(AgcWsDecoder *d, Sink *s, const AgcBuf *wire, size_t chunk) {
    int rc = 0;
    for (size_t i = 0; i < wire->len && rc == 0; i += chunk)
        rc = agentc_ws_decoder_feed(d, wire->p + i, chunk < wire->len - i ? chunk : wire->len - i,
                                    sink, s);
    check("ws.feed.rc", rc == 0);
}

static void test_decode(void) {
    /* single masked binary frame */
    AgcBuf w = {0};
    put_client(&w, AGENTC_WS_BINARY, true, "hello", 5);
    AgcWsDecoder d;
    agentc_ws_decoder_init(&d);
    Sink s = {0};
    feed_all(&d, &s, &w, 1);
    check("ws.dec.masked", agentc_str_eq_cstr((const char *)s.log.p, s.log.len, "bin:hello|"));
    agentc_buf_free(&s.log);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&w);

    /* fragmentation: binary(FIN=0) + ping + continuation(FIN=1) */
    w = (AgcBuf){0};
    put_client(&w, AGENTC_WS_BINARY, false, "hel", 3);
    put_client(&w, AGENTC_WS_PING, true, "p", 1);
    put_client(&w, 0, true, "lo", 2);
    agentc_ws_decoder_init(&d);
    s = (Sink){0};
    feed_all(&d, &s, &w, 2);
    check("ws.dec.frag", agentc_str_eq_cstr((const char *)s.log.p, s.log.len, "ping:p|bin:hello|"));
    agentc_buf_free(&s.log);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&w);

    /* 16-bit and 64-bit extended lengths */
    w = (AgcBuf){0};
    AgcBuf big = {0};
    for (size_t i = 0; i < 300; i++)
        agentc_buf_byte(&big, (u8)('a' + (i % 26)));
    put_client(&w, AGENTC_WS_BINARY, true, big.p, big.len);
    agentc_ws_decoder_init(&d);
    s = (Sink){0};
    feed_all(&d, &s, &w, 7);
    check("ws.dec.len16", s.count == 1 && s.log.len == 4 + big.len + 1);
    check("ws.dec.len16.head", agentc_str_eq_cstr((const char *)s.log.p, 4, "bin:"));
    agentc_buf_free(&s.log);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&big);
    agentc_buf_free(&w);

    w = (AgcBuf){0};
    big = (AgcBuf){0};
    for (size_t i = 0; i < 70000; i++)
        agentc_buf_byte(&big, (u8)(i & 0xff));
    put_client(&w, AGENTC_WS_BINARY, true, big.p, big.len);
    agentc_ws_decoder_init(&d);
    s = (Sink){0};
    feed_all(&d, &s, &w, 997);
    check("ws.dec.len64", s.count == 1);
    agentc_buf_free(&s.log);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&big);
    agentc_buf_free(&w);
}

static void test_violations(void) {
    AgcWsDecoder d;
    AgcBuf w = {0};
    Sink s = {0};

    /* unmasked client frame */
    agentc_buf_byte(&w, 0x82);
    agentc_buf_byte(&w, 2);
    agentc_buf_push(&w, "hi", 2);
    agentc_ws_decoder_init(&d);
    s = (Sink){0};
    check("ws.viol.unmasked", agentc_ws_decoder_feed(&d, w.p, w.len, sink, &s) == -71);
    check("ws.viol.unmasked.sticky", agentc_ws_decoder_feed(&d, w.p, w.len, sink, &s) == -71);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&w);

    /* RSV bit set */
    w = (AgcBuf){0};
    agentc_buf_byte(&w, 0xc2); /* FIN|RSV1|binary */
    agentc_buf_byte(&w, 0x80);
    agentc_buf_push(&w, "\0\0\0\0", 4);
    agentc_ws_decoder_init(&d);
    check("ws.viol.rsv", agentc_ws_decoder_feed(&d, w.p, w.len, sink, &s) == -71);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&w);

    /* text data frame: unsupported (close 1003) */
    w = (AgcBuf){0};
    put_client(&w, 0x1, true, "t", 1);
    agentc_ws_decoder_init(&d);
    check("ws.viol.text", agentc_ws_decoder_feed(&d, w.p, w.len, sink, &s) == -95);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&w);

    /* fragmented control frame */
    w = (AgcBuf){0};
    put_client(&w, AGENTC_WS_PING, false, "p", 1);
    agentc_ws_decoder_init(&d);
    check("ws.viol.ctrl_frag", agentc_ws_decoder_feed(&d, w.p, w.len, sink, &s) == -71);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&w);

    /* control frame over 125 bytes */
    w = (AgcBuf){0};
    AgcBuf p126 = {0};
    for (size_t i = 0; i < 126; i++)
        agentc_buf_byte(&p126, 'x');
    put_client(&w, AGENTC_WS_PING, true, p126.p, p126.len);
    agentc_ws_decoder_init(&d);
    check("ws.viol.ctrl_big", agentc_ws_decoder_feed(&d, w.p, w.len, sink, &s) == -71);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&p126);
    agentc_buf_free(&w);

    /* oversized declared frame: rejected at the header, before any payload */
    w = (AgcBuf){0};
    agentc_buf_byte(&w, 0x82);
    agentc_buf_byte(&w, 0x80 | 127);
    u64 over = (u64)AGENTC_WS_MAX_MESSAGE + 1;
    for (int i = 7; i >= 0; i--)
        agentc_buf_byte(&w, (u8)(over >> (8 * i)));
    agentc_buf_push(&w, "\0\0\0\0", 4);
    agentc_ws_decoder_init(&d);
    s = (Sink){0};
    check("ws.viol.oversize", agentc_ws_decoder_feed(&d, w.p, w.len, sink, &s) == -90);
    check("ws.viol.oversize.nofire", s.count == 0);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&w);

    /* oversized reassembled message */
    w = (AgcBuf){0};
    AgcBuf big = {0};
    for (size_t i = 0; i < AGENTC_WS_MAX_MESSAGE; i++)
        agentc_buf_byte(&big, 'z');
    put_client(&w, AGENTC_WS_BINARY, false, big.p, big.len);
    put_client(&w, 0, true, "tail", 4);
    agentc_ws_decoder_init(&d);
    s = (Sink){0};
    check("ws.viol.frag_oversize", agentc_ws_decoder_feed(&d, w.p, w.len, sink, &s) == -90);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&big);
    agentc_buf_free(&w);

    /* continuation without a start */
    w = (AgcBuf){0};
    put_client(&w, 0, true, "x", 1);
    agentc_ws_decoder_init(&d);
    check("ws.viol.orphan_cont", agentc_ws_decoder_feed(&d, w.p, w.len, sink, &s) == -71);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&w);

    /* close frame latches cleanly */
    w = (AgcBuf){0};
    put_client(&w, AGENTC_WS_CLOSE, true, "\x03\xe8", 2);
    agentc_ws_decoder_init(&d);
    s = (Sink){0};
    int cr = agentc_ws_decoder_feed(&d, w.p, w.len, sink, &s);
    check("ws.close.rc", cr == 0 && d.closed && d.dead);
    check("ws.close.log", agentc_str_eq_cstr((const char *)s.log.p, s.log.len, "close:\x03\xe8|"));
    agentc_buf_free(&s.log);
    agentc_ws_decoder_free(&d);
    agentc_buf_free(&w);

    /* NULL input with a non-zero length is rejected, NULL + 0 is a no-op */
    agentc_ws_decoder_init(&d);
    check("ws.dec.null", agentc_ws_decoder_feed(&d, NULL, 3, sink, &s) == -22);
    check("ws.dec.null0", agentc_ws_decoder_feed(&d, NULL, 0, sink, &s) == 0);
    agentc_ws_decoder_free(&d);
}

static void test_encode(void) {
    AgcBuf b = {0};
    check("ws.enc.binary", agentc_ws_encode(&b, "hello", 5) == 0);
    dump_hex(b.p, b.len); /* 820568656c6c6f */
    b.len = 0;
    check("ws.enc.len126", agentc_ws_encode(&b, zeros, 126) == 0);
    check("ws.enc.len126.head",
          b.p[0] == 0x82 && b.p[1] == 126 && b.p[2] == 0 && b.p[3] == 126 && b.len == 4 + 126);
    b.len = 0;
    check("ws.enc.len64k", agentc_ws_encode(&b, zeros, 70000) == 0);
    check("ws.enc.len64k.head", b.len == 10 + 70000 && b.p[0] == 0x82 && b.p[1] == 127);
    b.len = 0;
    check("ws.enc.pong", agentc_ws_encode_pong(&b, "p", 1) == 0);
    dump_hex(b.p, b.len); /* 8a0170 */
    b.len = 0;
    check("ws.enc.pong.big", agentc_ws_encode_pong(&b, zeros, 126) == -90);
    b.len = 0;
    check("ws.enc.close", agentc_ws_encode_close(&b, 1009) == 0);
    dump_hex(b.p, b.len); /* 880203f1 */
    agentc_buf_free(&b);
}

static void test_policy(void) {
    static const char REQ[] = "GET /agentc HTTP/1.1\r\n"
                              "Host: 127.0.0.1:8790\r\n"
                              "Origin: http://evil.example\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                              "Sec-WebSocket-Version: 13\r\n"
                              "Sec-WebSocket-Protocol: agentc.v1\r\n\r\n";
    AgcBuf in = {0};
    AgcBuf out = {0};
    AgcWsRequest req;

    /* incomplete: parse says "keep reading"; the policy handshake emits nothing */
    agentc_buf_push(&in, REQ, 10);
    check("ws.pol.parse.incomplete", agentc_ws_parse_request(&in, &req) == 1);
    check("ws.pol.hs.incomplete",
          agentc_ws_server_handshake_with(&in, &out, allow_loopback, NULL) == 0 && out.len == 0);

    /* complete: Host/Origin are exposed and the caller policy sees them */
    agentc_buf_push(&in, REQ + 10, sizeof REQ - 1 - 10);
    check("ws.pol.parse.complete", agentc_ws_parse_request(&in, &req) == 0);
    check("ws.pol.host", req.has_host && agentc_streq(req.host, "127.0.0.1:8790"));
    check("ws.pol.origin", req.has_origin && agentc_streq(req.origin, "http://evil.example"));
    int rc = agentc_ws_server_handshake_with(&in, &out, allow_loopback, NULL);
    check("ws.pol.reject", rc == -13);
    agentc_buf_byte(&out, 0);
    check("ws.pol.reject.403", agentc_str_str((const char *)out.p, "403 Forbidden") != NULL);
    agentc_buf_free(&in);
    agentc_buf_free(&out);

    /* an accepting predicate runs the base handshake */
    in = (AgcBuf){0};
    out = (AgcBuf){0};
    agentc_buf_push(&in, REQ, sizeof REQ - 1);
    rc = agentc_ws_server_handshake_with(&in, &out, allow_any, NULL);
    check("ws.pol.accept", rc == 1);
    agentc_buf_byte(&out, 0);
    check("ws.pol.accept.101",
          agentc_str_str((const char *)out.p, "101 Switching Protocols") != NULL);
    agentc_buf_free(&in);
    agentc_buf_free(&out);

    /* a NULL predicate is exactly the base handshake */
    in = (AgcBuf){0};
    out = (AgcBuf){0};
    agentc_buf_push(&in, REQ, sizeof REQ - 1);
    check("ws.pol.null_allow", agentc_ws_server_handshake_with(&in, &out, NULL, NULL) == 1);
    agentc_buf_free(&in);
    agentc_buf_free(&out);

    /* an over-long Host is refused before a policy verdict */
    {
        AgcBuf hi = {0};
        agentc_buf_cstr(&hi, "GET / HTTP/1.1\r\nHost: ");
        for (int i = 0; i < 300; i++)
            agentc_buf_byte(&hi, 'a');
        agentc_buf_cstr(&hi, "\r\n\r\n");
        AgcWsRequest r2;
        check("ws.pol.host_toolong", agentc_ws_parse_request(&hi, &r2) == -90);
        agentc_buf_free(&hi);
    }

    /* F1: the policy handshake applies the base incomplete-block cap. An
     * oversized block with no `\r\n\r\n` must be rejected with 431 (like the
     * base handshake), not buffered without bound. */
    {
        AgcBuf oi = {0};
        AgcBuf oo = {0};
        for (size_t i = 0; i < AGENTC_WS_MAX_HEADER + 1; i++)
            agentc_buf_byte(&oi, 'x');
        int orc = agentc_ws_server_handshake_with(&oi, &oo, allow_any, NULL);
        check("ws.pol.oversize.incomplete", orc == -90);
        agentc_buf_byte(&oo, 0);
        check("ws.pol.oversize.431",
              agentc_str_str((const char *)oo.p, "431 Request Header Fields Too Large") != NULL);
        agentc_buf_free(&oi);
        agentc_buf_free(&oo);
    }
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    test_sha1_base64();
    test_handshake();
    test_policy();
    test_decode();
    test_violations();
    test_encode();
    return fails ? 1 : 0;
}
