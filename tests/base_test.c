/* base_test.c — allocator, buffer, vector, formatting and string primitives. */
#include "agentc.h"
#include "agent.h"
#include "base/glob.h"



static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* The counter is process-wide and the Windows bootstrap has already
     * allocated argv/envp by the time main runs, so assert a delta from a
     * baseline instead of an absolute zero: the check keeps the leak-detection
     * meaning (3 allocations live, then none) on every platform. */
    size_t base = agentc_mem_live();

    u8 *p = agentc_alloc(100);
    bool zeroed = true;
    for (int i = 0; i < 100; i++)
        if (p[i]) zeroed = false;
    check("zeroed", zeroed);

    AgcBuf b = { 0 };
    agentc_buf_cstr(&b, "hello");
    agentc_buf_byte(&b, '!');
    check("buf", agentc_str_eq((char *)b.p, b.len, "hello!", 6));

    agentc_buf_clear(&b);
    agentc_buf_printf(&b, "%s %d %u %x %c %%", "x", -12, 42u, 255u, 'z');
    check("printf", agentc_streq((char *)b.p, "x -12 42 ff z %"));

    AgcVec v = { 0 };
    for (u64 i = 0; i < 3; i++)
        *(u64 *)agentc_vec_push(&v, sizeof(u64)) = (i + 1) * 10;
    u64 sum = 0;
    for (size_t i = 0; i < v.len; i++) sum += ((u64 *)v.p)[i];
    check("vec", sum == 60 && v.len == 3);

    check("live", agentc_mem_live() == base + 3);

    agentc_free(p);
    agentc_buf_free(&b);
    agentc_vec_free(&v);
    check("freed", agentc_mem_live() == base);

    check("find", agentc_str_find("hello world", 11, "world", 5) == 6);
    check("parse", agentc_parse_i64("-42", 3, NULL) == -42);

    /* integer parsing: the whole span must be consumed and overflow must be
     * reported instead of wrapping; INT64_MIN is representable. */
    bool ok = false;
    check("parse.i64.min",
          agentc_parse_i64("-9223372036854775808", 20, &ok) == INT64_MIN && ok);
    ok = true;
    check("parse.i64.over", agentc_parse_i64("9223372036854775808", 19, &ok) == 0 && !ok);
    ok = true;
    check("parse.i64.under", agentc_parse_i64("-9223372036854775809", 20, &ok) == 0 && !ok);
    ok = true;
    check("parse.u64.max", agentc_parse_u64("18446744073709551615", 20, &ok) == ~(u64)0 && ok);
    ok = true;
    check("parse.u64.over", agentc_parse_u64("18446744073709551616", 20, &ok) == 0 && !ok);
    ok = true;
    check("parse.plus", agentc_parse_i64("+5", 2, &ok) == 5 && ok);
    ok = true;
    check("parse.frac", agentc_parse_i64("1.9", 3, &ok) == 0 && !ok);
    ok = true;
    check("parse.exp", agentc_parse_i64("1e3", 3, &ok) == 0 && !ok);
    ok = true;
    check("parse.trail", agentc_parse_i64("5 ", 2, &ok) == 0 && !ok);
    ok = true;
    check("parse.empty", agentc_parse_i64("", 0, &ok) == 0 && !ok);
    ok = true;
    check("parse.u.empty", agentc_parse_u64("", 0, &ok) == 0 && !ok);

    /* left-justified numbers keep their sign before the digits and pad right */
    char fb[32];
    agentc_snprintf(fb, sizeof fb, "[%-5d]", -42);
    check("fmt.left.d", agentc_streq(fb, "[-42  ]"));
    agentc_snprintf(fb, sizeof fb, "[%-5i]", -7);
    check("fmt.left.i", agentc_streq(fb, "[-7   ]"));
    agentc_snprintf(fb, sizeof fb, "[%-5d]", 42);
    check("fmt.left.pos", agentc_streq(fb, "[42   ]"));

    /* model catalog: NULL counts, non-NULL returns what was written (<= max) */
    const AgcModel *mall[2];
    size_t mtotal = agentc_model_all(NULL, 0);
    size_t mgot = agentc_model_all(mall, 2);
    size_t mzero = agentc_model_all(mall, 0);
    check("model_all",
          mtotal >= 2 && mgot == 2 && mzero == 0 && mall[0] != NULL && mall[1] != NULL);

    /* a full-length (256-byte) literal pattern must compile: the slot bound is
     * per-atom, not a flat +4 reservation. */
    char gpat[257], gtext[257];
    for (int i = 0; i < 256; i++) {
        gpat[i] = 'a';
        gtext[i] = 'a';
    }
    gpat[256] = '\0';
    gtext[256] = '\0';
    check("glob.maxlen", agentc_glob_match(gpat, gtext));
    return fails;
}
