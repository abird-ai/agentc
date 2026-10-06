/* jsonw_test.c — streaming JSON writer and agentc_json_escape golden cases. */
#include "net/net_internal.h"

static void dump(const char *label, const AgcBuf *b) {
    agentc_outf("%s len=%d ", label, (int)b->len);
    agentc_out(b->p, b->len);
    agentc_out_nl();
}

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* os_init has already allocated on Windows (argv/envp), so `live` is a
     * delta: everything the writer allocates must be freed by the end. */
    size_t base = agentc_mem_live();

    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "a");
    agentc_jsonw_u64(&w, 1);
    agentc_jsonw_key(&w, "b");
    agentc_jsonw_arr(&w);
    agentc_jsonw_bool(&w, true);
    agentc_jsonw_null(&w);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "c");
    agentc_jsonw_str(&w, "x\ny", 3);
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "d");
    agentc_jsonw_str(&w, "q\"\\\b\f\n\r\t\x01", 9);
    agentc_jsonw_key(&w, "neg");
    agentc_jsonw_i64(&w, -42);
    agentc_jsonw_key(&w, "big");
    agentc_jsonw_u64(&w, 18446744073709551615ull);
    agentc_jsonw_key(&w, "raw");
    agentc_jsonw_raw(&w, "1.5e3", 5);
    agentc_jsonw_key(&w, "utf8");
    agentc_jsonw_cstr(&w, "\xc3\xa9");
    agentc_jsonw_end(&w);
    dump("doc", &b);

    agentc_buf_clear(&b);
    agentc_json_escape(&b, "a\"b\\c\x1f\x7f", 7);
    dump("escape", &b);

    agentc_buf_clear(&b);
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_arr(&w);
    agentc_jsonw_str(&w, "", 0);
    agentc_jsonw_end(&w);
    dump("empty-str", &b);

    /* Two live writers with opposite container types, calls interleaved: the
     * type stack must live in AgcJsonW or w1's close reads w2's array. */
    AgcBuf b1 = { 0 }, b2 = { 0 };
    AgcJsonW w1, w2;
    agentc_jsonw_init(&w1, &b1);
    agentc_jsonw_init(&w2, &b2);
    agentc_jsonw_obj(&w1);
    agentc_jsonw_arr(&w2);
    agentc_jsonw_key(&w1, "k");
    agentc_jsonw_obj(&w2);
    agentc_jsonw_u64(&w1, 1);
    agentc_jsonw_key(&w2, "z");
    agentc_jsonw_bool(&w2, true);
    agentc_jsonw_end(&w2);
    agentc_jsonw_end(&w1);
    agentc_jsonw_end(&w2);
    dump("two-writers-w1", &b1);
    dump("two-writers-w2", &b2);

    /* Reuse: init on an already-used, non-zeroed struct must yield a fresh doc
     * instead of depending on zeroed storage. */
    agentc_buf_clear(&b1);
    agentc_jsonw_init(&w1, &b1);
    agentc_jsonw_obj(&w1);
    agentc_jsonw_key(&w1, "r");
    agentc_jsonw_arr(&w1);
    agentc_jsonw_u64(&w1, 7);
    agentc_jsonw_end(&w1);
    agentc_jsonw_end(&w1);
    dump("two-writers-reuse", &b1);
    agentc_buf_free(&b1);
    agentc_buf_free(&b2);

    /* ascii off (the default): UTF-8 passes through byte-for-byte */
    agentc_buf_clear(&b);
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_str(&w, "\xc3\xa9", 2);
    check("jsonw.ascii.off", b.len == 4 && agentc_memeq(b.p, "\"\xc3\xa9\"", 4));

    /* ascii on: every byte < 0x7f and the escaped form parses back to the
     * original UTF-8 bytes (BMP + surrogate pair + C1) */
    {
        const char *s = "caf\xc3\xa9 \xf0\x9f\x98\x80 \xc2\x9b";
        size_t sn = agentc_strlen(s);
        AgcBuf esc = { 0 };
        AgcJsonW aw;
        agentc_jsonw_init(&aw, &esc);
        agentc_jsonw_set_ascii(&aw, true);
        agentc_jsonw_str(&aw, s, sn);
        bool ascii_only = true;
        for (size_t i = 0; i < esc.len; i++)
            if (esc.p[i] >= 0x7fu) ascii_only = false;
        AgcJsonArena *a = agentc_json_arena_new(0);
        AgcJson *v = a ? agentc_json_parse_in(a, (const char *)esc.p, esc.len) : NULL;
        size_t rn = 0;
        const char *rs = v ? agentc_json_str(v, &rn) : NULL;
        check("jsonw.ascii.roundtrip",
              ascii_only && rs != NULL && rn == sn && agentc_memeq(rs, s, sn));
        agentc_json_arena_free(a);
        agentc_buf_free(&esc);
    }

    /* ascii on: CESU-8/overlong bytes decode to a surrogate half, which is not
     * a Unicode scalar. The escape must not emit a lone \uD8xx (invalid JSON);
     * substitute U+FFFD, matching base/json.c. */
    {
        const char *bad = "\xed\xa0\xbd\xed\xb8\x80"; /* U+D83D U+DE00 as CESU-8 */
        AgcBuf sb = { 0 };
        AgcJsonW sw;
        agentc_jsonw_init(&sw, &sb);
        agentc_jsonw_set_ascii(&sw, true);
        agentc_jsonw_str(&sw, bad, 6);
        check("jsonw.ascii.surrogate",
              sb.len == 14 && agentc_memeq(sb.p, "\"\\ufffd\\ufffd\"", 14));
        agentc_buf_free(&sb);
    }

    /* The writer's depth stack shares AGENTC_JSON_MAX_DEPTH with the parser (it
     * used to stop silently at 64): a document deeper than 64 must emit valid
     * JSON and parse back with every level intact. */
    {
        enum { DEEP = 120 };
        AgcBuf db = { 0 };
        AgcJsonW dw;
        agentc_jsonw_init(&dw, &db);
        for (int i = 0; i < DEEP; i++) agentc_jsonw_arr(&dw);
        agentc_jsonw_u64(&dw, 1);
        for (int i = 0; i < DEEP; i++) agentc_jsonw_end(&dw);
        AgcJsonArena *da = agentc_json_arena_new(0);
        const AgcJson *v = da ? agentc_json_parse_in(da, (const char *)db.p, db.len) : NULL;
        int depth = 0;
        const AgcJson *n = v;
        while (agentc_json_type(n) == AGENTC_JSON_ARR) {
            depth++;
            n = agentc_json_at(n, 0);
        }
        size_t ln = 0;
        const char *ltext = agentc_json_num(n, &ln);
        check("jsonw.deep",
              v != NULL && depth == DEEP && ltext != NULL && ln == 1 && ltext[0] == '1');
        agentc_json_arena_free(da);
        agentc_buf_free(&db);
    }

    agentc_buf_free(&b);
    agentc_outf("live=%d\n", (int)(agentc_mem_live() - base));
    return fails;
}
