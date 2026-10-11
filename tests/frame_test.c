/* frame_test.c — golden trace for the framed-CBOR transport frame.
 *
 * Covers minimal encoding, fragmentation, coalescing, the 16 MiB cap
 * (validate-before-copy), sticky failure and the callback-stop contract.
 * Golden: tests/data/frame_test.expected.
 */
#include "frame.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

static void dump_hex(AgcBuf *b) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < b->len; i++) {
        agentc_outf("%c%c", hex[b->p[i] >> 4], hex[b->p[i] & 0xf]);
    }
    agentc_out_nl();
}

/* A sink that appends each payload to a trace with a separator. */
typedef struct {
    AgcBuf frames;
    AgcBuf lens;
    size_t count;
    size_t stop_after; /* 0 = never stop */
} Sink;

static int sink_cb(void *ud, const AgcFrame *f) {
    Sink *s = ud;
    agentc_buf_push(&s->frames, f->payload, f->len);
    agentc_buf_byte(&s->frames, '|');
    agentc_buf_u64(&s->lens, f->len);
    agentc_buf_byte(&s->lens, ',');
    s->count++;
    if (s->stop_after && s->count >= s->stop_after)
        return -125;
    return 0;
}

static void test_encode(void) {
    AgcBuf b = {0};
    check("frame.enc.empty", agentc_frame_encode(&b, NULL, 0) == 0);
    dump_hex(&b); /* 00000000 */
    b.len = 0;
    check("frame.enc.abc", agentc_frame_encode(&b, "abc", 3) == 0);
    dump_hex(&b); /* 00000003616263 */
    b.len = 0;
    check("frame.enc.oversize", agentc_frame_encode(&b, "x", AGENTC_FRAME_MAX_BYTES + 1) == -90);
    check("frame.enc.oversize.empty", b.len == 0);
    agentc_buf_free(&b);
}

static void test_stream(void) {
    AgcBuf wire = {0};
    (void)agentc_frame_encode(&wire, "hello", 5);
    (void)agentc_frame_encode(&wire, "", 0);
    (void)agentc_frame_encode(&wire, "world!", 6);
    /* 4 + 5 + 4 + 4 + 6 = 23 bytes */

    AgcFrameDecoder d;
    agentc_frame_decoder_init(&d);
    Sink s = {0};
    int rc = 0;
    for (size_t i = 0; i < wire.len && rc == 0; i++)
        rc = agentc_frame_decoder_feed(&d, wire.p + i, 1, sink_cb, &s);
    check("frame.frag.rc", rc == 0);
    check("frame.frag.count", s.count == 3);
    check("frame.frag.payloads",
          agentc_str_eq_cstr((const char *)s.frames.p, s.frames.len, "hello||world!|"));
    check("frame.frag.pending", agentc_frame_decoder_pending(&d) == 0);

    /* coalesced: the same three frames in one feed */
    Sink s2 = {0};
    rc = agentc_frame_decoder_feed(&d, wire.p, wire.len, sink_cb, &s2);
    check("frame.coal.rc", rc == 0);
    check("frame.coal.count", s2.count == 3);
    check("frame.coal.payloads",
          agentc_str_eq_cstr((const char *)s2.frames.p, s2.frames.len, "hello||world!|"));

    /* callback stop: one frame is consumed, the rest stays pending */
    Sink s3 = {0};
    s3.stop_after = 1;
    rc = agentc_frame_decoder_feed(&d, wire.p, wire.len, sink_cb, &s3);
    check("frame.stop.rc", rc == -125);
    check("frame.stop.count", s3.count == 1);
    check("frame.stop.pending", agentc_frame_decoder_pending(&d) == wire.len - 9);
    agentc_frame_decoder_free(&d);

    agentc_buf_free(&wire);
    agentc_buf_free(&s.frames);
    agentc_buf_free(&s.lens);
    agentc_buf_free(&s2.frames);
    agentc_buf_free(&s2.lens);
    agentc_buf_free(&s3.frames);
    agentc_buf_free(&s3.lens);
}

static void test_cap(void) {
    AgcFrameDecoder d;
    agentc_frame_decoder_init(&d);
    /* A header that declares one byte over the cap: rejected before any
       payload is buffered, and the failure is sticky. */
    u8 hdr[4] = {0x01, 0x00, 0x00, 0x01}; /* 0x01000001 = 16 MiB + 1 */
    Sink s = {0};
    int rc = agentc_frame_decoder_feed(&d, hdr, sizeof hdr, sink_cb, &s);
    check("frame.cap.reject", rc == -90);
    check("frame.cap.nocopy", agentc_frame_decoder_pending(&d) == 4);
    check("frame.cap.sticky", agentc_frame_decoder_feed(&d, "abc", 3, sink_cb, &s) == -90);
    check("frame.cap.nofire", s.count == 0);
    /* A maximum-sized declaration that never completes still buffers only what
       arrived. */
    AgcFrameDecoder d2;
    agentc_frame_decoder_init(&d2);
    u8 maxhdr[4] = {0x01, 0x00, 0x00, 0x00}; /* exactly 16 MiB */
    rc = agentc_frame_decoder_feed(&d2, maxhdr, sizeof maxhdr, sink_cb, &s);
    check("frame.cap.max.ok", rc == 0);
    check("frame.cap.max.pending", agentc_frame_decoder_pending(&d2) == 4);
    agentc_frame_decoder_free(&d);
    agentc_frame_decoder_free(&d2);
    agentc_buf_free(&s.frames);
    agentc_buf_free(&s.lens);
}

static void test_peek(void) {
    AgcFrameDecoder d;
    agentc_frame_decoder_init(&d);
    u8 part[3] = {0, 0, 0};
    (void)agentc_frame_decoder_feed(&d, part, sizeof part, NULL, NULL);
    check("frame.peek.partial", agentc_frame_peek_len(&d) == 0);
    u8 more[1] = {9};
    (void)agentc_frame_decoder_feed(&d, more, 1, NULL, NULL);
    check("frame.peek.full", agentc_frame_peek_len(&d) == 9);
    agentc_frame_decoder_free(&d);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    test_encode();
    test_stream();
    test_cap();
    test_peek();
    return fails;
}
