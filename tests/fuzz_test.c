/* fuzz_test.c — deterministic mutation fuzzing of the parsers.
 *
 * Seeds are valid JSONC/SSE/URL documents and valid CBOR/frame byte strings;
 * each iteration applies a handful of random mutations (flip, truncate, insert,
 * swap) and feeds the result to the JSONC parser (walking the result through the
 * getters), the SSE parser, the URL parser, the CBOR decoder and the frame
 * decoder. The PRNG is a fixed-seed xorshift, so the run is reproducible and the
 * golden file is stable. A crash, hang, OOB read, unbounded allocation or leak
 * fails the run.
 */
#include "agentc.h"
#include "cbor.h"
#include "frame.h"
#include "plat.h"
#include "wire.h"

#define ITERATIONS 30000
#define CBOR_ITERATIONS 20000

static u64 rng = 0x243f6a8885a308d3ull;

static u32 rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (u32)(rng >> 32);
}

static const char *seeds[] = {
    "{\"a\":[1,2,{\"b\":\"\\ud83d\\ude00\"}],}",
    "// c\n{\"x\":-1.5e+3,/*k*/\"y\":true,\"z\":null}",
    "[{\"tool\":\"bash\",\"args\":{\"command\":\"ls -la\",\"timeout\":30000}}]",
    "{\"s\":\"\\u0000\\u00ff\\ud800\\udc00\\\\\\\"\"}",
    "event: message\ndata: {\"x\":1}\n\ndata: [DONE]\n\n",
    "data: a\r\ndata: b\r\rdata:\n",
    "http://example.com:8080/a/b?c=d#e",
    "https://user@host:8443/x",
};

/* Valid CBOR/frame byte strings. The decoder must never read out of bounds and
 * every rejected input must leave the arena/decoder bounded. */
static const u8 cbor_array[] = {0x83, 0x01, 0x02, 0x03};
static const u8 cbor_map[] = {0xa2, 0x61, 'a', 0x01, 0x61, 'b', 0xf5};
static const u8 cbor_text[] = {0x63, 'a', 'b', 'c'};
static const u8 cbor_nested[] = {0x82, 0x81, 0x18, 0x64, 0xa1, 0x61, 'k', 0xf6};
/* A valid value plus a trailing byte: the whole-buffer decoder must reject it
 * (and stay bounded when mutation turns the pair into anything else). */
static const u8 cbor_trailing[] = {0x01, 0x02};
static const u8 frame_ok[] = {0x00, 0x00, 0x00, 0x03, 'a', 'b', 'c'};
static const u8 frame_cap[] = {0x01, 0x00, 0x00, 0x01}; /* 16 MiB + 1 */
static const u8 frame_big[] = {0x00, 0x00, 0x10, 0x00}; /* declares 4 KiB */

static const struct {
    const u8 *p;
    size_t n;
} byte_seeds[] = {
    {cbor_array, sizeof cbor_array},       {cbor_map, sizeof cbor_map},
    {cbor_text, sizeof cbor_text},         {cbor_nested, sizeof cbor_nested},
    {cbor_trailing, sizeof cbor_trailing}, {frame_ok, sizeof frame_ok},
    {frame_cap, sizeof frame_cap},         {frame_big, sizeof frame_big},
};

static int frame_sink(void *ud, const AgcFrame *f) {
    (void)ud;
    (void)f;
    return 1; /* stop after the first complete frame */
}

static void exercise(const char *buf, size_t n) {
    AgcJson *v = agentc_json_parse(buf, n);
    if (v) {
        AgcJson *nested = agentc_json_get(v, "a");
        (void)agentc_json_get_int(v, "n", 0);
        (void)agentc_json_get_bool(v, "ok", false);
        (void)agentc_json_get_str(v, "s");
        size_t len = agentc_json_len(nested);
        for (size_t i = 0; i < len && i < 64; i++)
            (void)agentc_json_type(agentc_json_at(nested, i));
    }
    AgcSse sse;
    agentc_sse_init(&sse);
    (void)agentc_sse_feed(&sse, buf, n, NULL, NULL);
    agentc_sse_free(&sse);
    AgcUrl u;
    (void)agentc_url_parse(buf, &u);

    /* One reusable arena: every decode clears it first, so the resident set is
     * bounded by the largest mutated value (<= the fuzz buffer) and stable. */
    static AgcCborArena *ca;
    if (!ca)
        ca = agentc_cbor_arena_new(0);
    int err = 0;
    AgcCbor *cv = agentc_cbor_decode(ca, buf, n, NULL, &err);
    if (cv) {
        (void)agentc_cbor_is_json(cv);
        size_t l = agentc_cbor_len(cv);
        for (size_t i = 0; i < l && i < 64; i++) {
            (void)agentc_cbor_type(agentc_cbor_at(cv, i));
            (void)agentc_cbor_key(cv, i);
            (void)agentc_cbor_val(cv, i);
        }
    }
    /* A fresh decoder per input: pending bytes can never accumulate across
     * iterations, and a sticky failure is exercised on the next feed. */
    AgcFrameDecoder fd;
    agentc_frame_decoder_init(&fd);
    (void)agentc_frame_decoder_feed(&fd, buf, n, frame_sink, NULL);
    (void)agentc_frame_decoder_pending(&fd);
    agentc_frame_decoder_free(&fd);
}

/* Apply 1..6 random mutations to `n` bytes in `buf` in place, returning the new
 * length. Shared by the text and byte seed loops. */
static size_t mutate(char *buf, size_t n) {
    u32 muts = 1 + rnd() % 6;
    for (u32 m = 0; m < muts; m++) {
        u32 op = rnd() % 4;
        if (op == 0 && n) {
            buf[rnd() % n] = (char)(rnd() & 0xff);
        } else if (op == 1 && n) {
            n = rnd() % n;
        } else if (op == 2 && n < 4096 - 1) {
            size_t at = rnd() % (n + 1);
            agentc_memmove(buf + at + 1, buf + at, n - at);
            buf[at] = (char)(rnd() & 0xff);
            n++;
        } else if (n >= 2) {
            size_t a = rnd() % n, b = rnd() % n;
            char t = buf[a];
            buf[a] = buf[b];
            buf[b] = t;
        }
    }
    return n;
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char buf[4096];
    size_t baseline = 0;
    for (size_t it = 0; it < ITERATIONS; it++) {
        const char *seed = seeds[rnd() % (sizeof seeds / sizeof seeds[0])];
        size_t n = agentc_strlen(seed);
        if (n > sizeof buf)
            n = sizeof buf;
        agentc_memcpy(buf, seed, n);
        n = mutate(buf, n);
        exercise(buf, n);
        /* after the first quarter the arena block and the parser's child vector
         * have reached their high-water mark: the rest must not grow anything */
        if (it + 1 == ITERATIONS / 4)
            baseline = agentc_mem_live();
    }
    /* CBOR/frame seeds, mutated the same way. */
    for (size_t it = 0; it < CBOR_ITERATIONS; it++) {
        size_t si = rnd() % (sizeof byte_seeds / sizeof byte_seeds[0]);
        size_t n = byte_seeds[si].n;
        if (n > sizeof buf)
            n = sizeof buf;
        agentc_memcpy(buf, byte_seeds[si].p, n);
        n = mutate(buf, n);
        exercise(buf, n);
    }
    /* pure random bytes as well */
    for (size_t it = 0; it < ITERATIONS / 3; it++) {
        size_t n = rnd() % sizeof buf;
        for (size_t i = 0; i < n; i++)
            buf[i] = (char)(rnd() & 0xff);
        exercise(buf, n);
    }
    bool stable = agentc_mem_live() == baseline;
    agentc_outf("fuzz=%d\n", (int)(ITERATIONS + CBOR_ITERATIONS + ITERATIONS / 3));
    agentc_outf("mem_stable=%d\n", stable ? 1 : 0);
    return stable ? 0 : 1;
}
