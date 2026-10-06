/* fuzz_test.c — deterministic mutation fuzzing of the parsers.
 *
 * Seeds are valid JSONC/SSE/URL documents; each iteration applies a handful of
 * random mutations (flip, truncate, insert, swap) and feeds the result to the
 * JSONC parser (walking the result through the getters), the SSE parser and the
 * URL parser. The PRNG is a fixed-seed xorshift, so the run is reproducible and
 * the golden file is stable. A crash, hang or leak fails the run.
 */
#include "agentc.h"
#include "plat.h"
#include "wire.h"

#define ITERATIONS 30000

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

static void exercise(const char *buf, size_t n) {
    AgcJson *v = agentc_json_parse(buf, n);
    if (v) {
        AgcJson *nested = agentc_json_get(v, "a");
        (void)agentc_json_get_int(v, "n", 0);
        (void)agentc_json_get_bool(v, "ok", false);
        (void)agentc_json_get_str(v, "s");
        size_t len = agentc_json_len(nested);
        for (size_t i = 0; i < len && i < 64; i++) (void)agentc_json_type(agentc_json_at(nested, i));
    }
    AgcSse sse;
    agentc_sse_init(&sse);
    (void)agentc_sse_feed(&sse, buf, n, NULL, NULL);
    agentc_sse_free(&sse);
    AgcUrl u;
    (void)agentc_url_parse(buf, &u);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char buf[4096];
    size_t baseline = 0;
    for (size_t it = 0; it < ITERATIONS; it++) {
        const char *seed = seeds[rnd() % (sizeof seeds / sizeof seeds[0])];
        size_t n = agentc_strlen(seed);
        if (n > sizeof buf) n = sizeof buf;
        agentc_memcpy(buf, seed, n);
        u32 muts = 1 + rnd() % 6;
        for (u32 m = 0; m < muts; m++) {
            u32 op = rnd() % 4;
            if (op == 0 && n) {
                buf[rnd() % n] = (char)(rnd() & 0xff);
            } else if (op == 1 && n) {
                n = rnd() % n;
            } else if (op == 2 && n < sizeof buf - 1) {
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
        exercise(buf, n);
        /* after the first quarter the arena block and the parser's child vector
         * have reached their high-water mark: the rest must not grow anything */
        if (it + 1 == ITERATIONS / 4) baseline = agentc_mem_live();
    }
    /* pure random bytes as well */
    for (size_t it = 0; it < ITERATIONS / 3; it++) {
        size_t n = rnd() % sizeof buf;
        for (size_t i = 0; i < n; i++) buf[i] = (char)(rnd() & 0xff);
        exercise(buf, n);
    }
    bool stable = agentc_mem_live() == baseline;
    agentc_outf("fuzz=%d\n", (int)(ITERATIONS + ITERATIONS / 3));
    agentc_outf("mem_stable=%d\n", stable ? 1 : 0);
    return stable ? 0 : 1;
}
