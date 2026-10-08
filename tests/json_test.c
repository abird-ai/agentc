/* json_test.c — JSONC parser: dialect, strictness, accessors, leaks. */
#include "agentc.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

static const char doc[] =
    "{\n"
    "  // line comment\n"
    "  \"name\": \"agentc\", /* block comment */\n"
    "  \"version\": 2,\n"
    "  \"tags\": [\"a\", \"b\", \"c\",],\n"
    "  \"nested\": {\"n\": 42, \"ok\": true},\n"
    "  \"escaped\": \"q\\\"w\\\\e\\nx\",\n"
    "  \"unicode\": \"\\u00e9\\u4e2d\\ud83d\\ude00\",\n"
    "  \"empty_obj\": {},\n"
    "  \"empty_arr\": [],\n"
    "}\n";

/* ------------------------------------------------------------- arenas */

static const char arena_doc_a[] =
    "{\"name\":\"alpha\",\"list\":[1,2,{\"deep\":\"value\"}],\"nested\":{\"inner\":[3,4]}}";
static const char arena_doc_b[] = "{\"other\":\"beta\",\"arr\":[[1],[2]]}";

static void test_arenas(void) {
    /* Two live documents in two arenas: parsing B must not disturb A's
     * strings or nodes. */
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJsonArena *b = agentc_json_arena_new(64);
    AgcJson *ra = agentc_json_parse_in(a, arena_doc_a, sizeof arena_doc_a - 1);
    const char *name_a = agentc_json_get_str(ra, "name");
    AgcJson *rb = agentc_json_parse_in(b, arena_doc_b, sizeof arena_doc_b - 1);
    AgcJson *list_a = agentc_json_get(ra, "list");
    AgcJson *deep_a = agentc_json_at(list_a, 2);
    check("arena.two-live",
          a && b && ra && rb && name_a && agentc_streq(name_a, "alpha") &&
              agentc_json_len(list_a) == 3 &&
              agentc_json_is(agentc_json_get(deep_a, "deep"), "value") &&
              agentc_json_len(agentc_json_get(agentc_json_get(ra, "nested"), "inner")) == 2 &&
              agentc_json_is(agentc_json_get(rb, "other"), "beta") &&
              agentc_json_len(agentc_json_at(agentc_json_get(rb, "arr"), 1)) == 1);
    agentc_json_arena_free(a);
    agentc_json_arena_free(b);

    /* Re-parsing in one arena recycles it: the new doc reads and nothing grows
     * past the high-water mark reached by the first (larger) parse. */
    AgcJsonArena *c = agentc_json_arena_new(0);
    AgcJson *first = agentc_json_parse_in(c, arena_doc_a, sizeof arena_doc_a - 1);
    size_t high = agentc_mem_live();
    AgcJson *second = agentc_json_parse_in(c, arena_doc_b, sizeof arena_doc_b - 1);
    check("arena.reuse",
          first != NULL && second != NULL &&
              agentc_json_is(agentc_json_get(second, "other"), "beta") &&
              agentc_json_len(agentc_json_at(agentc_json_get(second, "arr"), 1)) == 1 &&
              agentc_mem_live() == high);
    agentc_json_arena_free(c);

    /* Freeing an arena releases the block, the child vector and the struct;
     * NULL handles are no-ops. */
    size_t base = agentc_mem_live();
    AgcJsonArena *d = agentc_json_arena_new(0);
    AgcJson *rd = agentc_json_parse_in(d, arena_doc_a, sizeof arena_doc_a - 1);
    agentc_json_arena_free(d);
    agentc_json_arena_clear(NULL);
    agentc_json_arena_free(NULL);
    check("arena.free",
          rd != NULL && agentc_mem_live() == base &&
              agentc_json_parse_in(NULL, arena_doc_a, sizeof arena_doc_a - 1) == NULL);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    AgcJson *root = agentc_json_parse(doc, sizeof doc - 1);
    check("parsed", root != NULL);

    check("name", agentc_json_is(agentc_json_get(root, "name"), "agentc"));
    check("version", agentc_json_get_int(root, "version", -1) == 2);

    AgcJson *tags = agentc_json_get(root, "tags");
    check("tags_len", agentc_json_len(tags) == 3);
    check("tag1", agentc_json_is(agentc_json_at(tags, 1), "b"));

    AgcJson *nested = agentc_json_get(root, "nested");
    check("n", agentc_json_get_int(nested, "n", 0) == 42);
    check("ok", agentc_json_get_bool(nested, "ok", false));

    size_t elen = 0;
    const char *esc = agentc_json_str(agentc_json_get(root, "escaped"), &elen);
    const char *eexp = "q\"w\\e\nx";
    check("escaped", esc && agentc_str_eq(esc, elen, eexp, agentc_strlen(eexp)));

    size_t ulen = 0;
    const char *uni = agentc_json_str(agentc_json_get(root, "unicode"), &ulen);
    check("unicode_len", ulen == 9);
    check("unicode_bytes",
          uni && (u8)uni[0] == 0xC3 && (u8)uni[1] == 0xA9 && (u8)uni[2] == 0xE4 &&
              (u8)uni[3] == 0xB8 && (u8)uni[4] == 0xAD && (u8)uni[5] == 0xF0 &&
              (u8)uni[6] == 0x9F && (u8)uni[7] == 0x98 && (u8)uni[8] == 0x80);

    check("empty_obj", agentc_json_type(agentc_json_get(root, "empty_obj")) == AGENTC_JSON_OBJ &&
                           agentc_json_len(agentc_json_get(root, "empty_obj")) == 0);
    check("empty_arr", agentc_json_type(agentc_json_get(root, "empty_arr")) == AGENTC_JSON_ARR);

    check("missing", agentc_json_get(root, "nope") == NULL &&
                         agentc_json_type(agentc_json_get(root, "nope")) == -1 &&
                         agentc_json_get_int(root, "nope", 7) == 7);
    check("wrong_type", agentc_json_get_str(nested, "n") == NULL);

    size_t nlen = 0;
    const char *nt = agentc_json_num(agentc_json_get(nested, "n"), &nlen);
    check("num_text", nt && nlen == 2 && nt[0] == '4' && nt[1] == '2');

    check("bad_missing_comma", agentc_json_parse("{\"a\":1 \"b\":2}", 13ul) == NULL);
    check("bad_literal", agentc_json_parse("{\"a\":tru}", 10ul) == NULL);
    check("bad_trailing", agentc_json_parse("{} x", 4ul) == NULL);
    check("bad_unterminated", agentc_json_parse("{\"a\":\"x}", 9ul) == NULL);
    check("bad_comment", agentc_json_parse("{} /*", 5ul) == NULL);
    check("bad_empty", agentc_json_parse("", 0) == NULL);
    check("bad_comma_only", agentc_json_parse("[,]", 3ul) == NULL);

    /* number tokenizer: JSON's grammar, comments/trailing commas are the only
     * JSONC leniency. Malformed runs must not parse as a number. */
    check("num.malformed",
          agentc_json_parse("1e", 2) == NULL && agentc_json_parse("1.2.3", 5) == NULL &&
              agentc_json_parse("1+2", 3) == NULL && agentc_json_parse("--1", 3) == NULL);
    check("num.leading_zero", agentc_json_parse("01", 2) == NULL);
    check("num.jsonc_ok",
          agentc_json_parse("-0.5e+10", 8) != NULL &&
              agentc_json_parse("[1, 2.5, -3e-2, 0, 1.5e3]", 25) != NULL);

    /* the integer accessor accepts only a genuine integer token */
    static const char nums_doc[] = "{\"a\":1.9,\"b\":1e3,\"c\":-7,\"d\":2}";
    AgcJson *nums = agentc_json_parse(nums_doc, sizeof nums_doc - 1);
    check("int.frac_rejected", nums && agentc_json_get_int(nums, "a", 7) == 7);
    check("int.exp_rejected", nums && agentc_json_get_int(nums, "b", 7) == 7);
    check("int.negative", nums && agentc_json_get_int(nums, "c", 0) == -7);
    check("int.plain", nums && agentc_json_get_int(nums, "d", 0) == 2);

    char deep[600];
    for (int i = 0; i < 300; i++) deep[i] = '[';
    for (int i = 0; i < 300; i++) deep[300 + i] = ']';
    check("deep_rejected", agentc_json_parse(deep, 600ul) == NULL);

    /* repeated parses reuse the arena: live block count must stay stable */
    agentc_json_reset();
    size_t live1 = agentc_mem_live();
    (void)agentc_json_parse(doc, sizeof doc - 1);
    agentc_json_reset();
    check("leak", agentc_mem_live() == live1);

    /* unpaired surrogates are not scalar values and must decode to U+FFFD
     * (EF BF BD), never CESU-8. Run after the `root` accesses above: a new
     * default-arena parse recycles it. */
    static const char surr_doc[] =
        "{\"hi\":\"\\ud800\",\"lo\":\"\\udc00\",\"mix\":\"\\ud800\\u0041\"}";
    AgcJson *surr = agentc_json_parse(surr_doc, sizeof surr_doc - 1);
    size_t hlen = 0, llen = 0, mlen = 0;
    const char *hs = agentc_json_str(agentc_json_get(surr, "hi"), &hlen);
    const char *ls = agentc_json_str(agentc_json_get(surr, "lo"), &llen);
    const char *ms = agentc_json_str(agentc_json_get(surr, "mix"), &mlen);
    check("surrogate.high",
          hs && hlen == 3 && (u8)hs[0] == 0xEF && (u8)hs[1] == 0xBF && (u8)hs[2] == 0xBD);
    check("surrogate.low",
          ls && llen == 3 && (u8)ls[0] == 0xEF && (u8)ls[1] == 0xBF && (u8)ls[2] == 0xBD);
    check("surrogate.mix",
          ms && mlen == 4 && (u8)ms[0] == 0xEF && (u8)ms[1] == 0xBF &&
              (u8)ms[2] == 0xBD && ms[3] == 'A');

    /* RFC 8259: a raw C0 control byte (0x00-0x1F) is illegal inside a string;
     * 0x7F is legal, and escaped forms (\n, \u0000) still decode. */
    static const char ctl_nl[] = { '"', 'a', '\n', 'b', '"' };
    static const char ctl_nul[] = { '"', 'a', 0x00, 'b', '"' };
    static const char ctl_del[] = { '"', 'a', 0x7f, 'b', '"' };
    static const char ctl_esc[] = "\"a\\nb\\u0000c\"";
    check("str.raw_control", agentc_json_parse(ctl_nl, sizeof ctl_nl) == NULL &&
                                 agentc_json_parse(ctl_nul, sizeof ctl_nul) == NULL);
    check("str.del_ok", agentc_json_parse(ctl_del, sizeof ctl_del) != NULL);
    check("str.escaped_ok", agentc_json_parse(ctl_esc, sizeof ctl_esc - 1) != NULL);

    test_arenas();

    return fails;
}
