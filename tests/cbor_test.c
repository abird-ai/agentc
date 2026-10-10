/* cbor_test.c — golden trace for the agentc CBOR codec and the JSON bridge.
 *
 * RFC 8949 Appendix A vectors, pi-safety, rejections (UTF-8, duplicate keys,
 * tags, indefinite lengths, f16/f32, NaN/Inf), minimal-width writing, framed
 * writing, the JSON bridge and an allocator baseline. Golden:
 * tests/data/cbor_test.expected.
 */
#include "cbor.h"
#include "cbor_json.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static size_t unhex(const char *hex, u8 *out, size_t cap) {
    size_t n = agentc_strlen(hex), o = 0;
    for (size_t i = 0; i + 1 < n && o < cap; i += 2)
        out[o++] = (u8)((hexval(hex[i]) << 4) | hexval(hex[i + 1]));
    return o;
}

static void print_hex(const u8 *p, size_t n) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++)
        agentc_outf("%c%c", h[p[i] >> 4], h[p[i] & 0xf]);
    agentc_out_nl();
}

/* Decode one canonical vector and re-encode it; the bytes must match. */
static void vector(AgcCborArena *a, const char *label, const char *hex) {
    u8 buf[64];
    size_t n = unhex(hex, buf, sizeof buf);
    int err = 0;
    AgcCbor *v = agentc_cbor_decode(a, buf, n, NULL, &err);
    bool ok = v != NULL;
    AgcBuf out = {0};
    if (ok) {
        AgcCborW w;
        agentc_cborw_init(&w, &out);
        agentc_cborw_value(&w, v);
        ok = !w.failed && out.len == n && agentc_memeq(out.p, buf, n);
        if (!ok) {
            agentc_outf("  %s got ", label);
            print_hex(out.p, out.len);
        }
    } else {
        agentc_outf("  %s err=%d\n", label, err);
    }
    check(label, ok);
    agentc_buf_free(&out);
}

static void reject(AgcCborArena *a, const char *label, const char *hex, int want_err) {
    u8 buf[64];
    size_t n = unhex(hex, buf, sizeof buf);
    int err = 0;
    AgcCbor *v = agentc_cbor_decode(a, buf, n, NULL, &err);
    check(label, v == NULL && err == want_err);
    if (v != NULL || err != want_err)
        agentc_outf("  %s want=%d got=%d\n", label, want_err, err);
}

static void test_vectors(AgcCborArena *a) {
    vector(a, "cbor.uint0", "00");
    vector(a, "cbor.uint1", "01");
    vector(a, "cbor.uint10", "0a");
    vector(a, "cbor.uint23", "17");
    vector(a, "cbor.uint24", "1818");
    vector(a, "cbor.uint25", "1819");
    vector(a, "cbor.uint100", "1864");
    vector(a, "cbor.uint1000", "1903e8");
    vector(a, "cbor.uint1e6", "1a000f4240");
    vector(a, "cbor.uint1e12", "1b000000e8d4a51000");
    vector(a, "cbor.neg1", "20");
    vector(a, "cbor.neg10", "29");
    vector(a, "cbor.neg100", "3863");
    vector(a, "cbor.neg1000", "3903e7");
    vector(a, "cbor.f64_1_1", "fb3ff199999999999a");
    vector(a, "cbor.f64_1_5", "fb3ff8000000000000");
    vector(a, "cbor.f64_1e5", "fb40f86a0000000000");
    vector(a, "cbor.false", "f4");
    vector(a, "cbor.true", "f5");
    vector(a, "cbor.null", "f6");
    vector(a, "cbor.text_empty", "60");
    vector(a, "cbor.text_a", "6161");
    vector(a, "cbor.text_ietf", "6449455446");
    vector(a, "cbor.bytes_empty", "40");
    vector(a, "cbor.bytes_4", "4401020304");
    vector(a, "cbor.arr_empty", "80");
    vector(a, "cbor.arr_123", "83010203");
    vector(a, "cbor.arr_nested", "8301820203820405");
    vector(a, "cbor.map_empty", "a0");
    vector(a, "cbor.map_ab", "a26161016162820203");

    reject(a, "cbor.rej.dupkey", "a2616101616102", -71);
    reject(a, "cbor.rej.utf8", "62c328", -71);
    reject(a, "cbor.rej.overlong", "62c0a0", -71);
    reject(a, "cbor.rej.surrogate", "63eda080", -71);
    reject(a, "cbor.rej.f16", "f93c00", -95);
    reject(a, "cbor.rej.f32", "fa47c35000", -95);
    reject(a, "cbor.rej.simple", "f7", -95);
    reject(a, "cbor.rej.undef", "f8ff", -95);
    reject(a, "cbor.rej.indef_arr", "9f01ff", -95);
    reject(a, "cbor.rej.indef_map", "bf616101ff", -95);
    reject(a, "cbor.rej.tag", "c074323031332d30332d32315432303a30343a30305a", -95);
    reject(a, "cbor.rej.nan", "fb7ff8000000000000", -71);
    reject(a, "cbor.rej.inf", "fb7ff0000000000000", -71);
    reject(a, "cbor.rej.nontextkey", "a10102", -71);
    reject(a, "cbor.rej.truncated", "19", -22);
    reject(a, "cbor.rej.unsafe", "1b0020000000000000", -34); /* 2^53 */
    reject(a, "cbor.rej.unsafe_neg", "3b001fffffffffffff", -34);
    reject(a, "cbor.rej.trailing", "0102", -71); /* uint 1 + trailing byte */
}

static void test_full_int(AgcCborArena *a) {
    u8 buf[16];
    size_t n = unhex("1b0020000000000000", buf, sizeof buf); /* 2^53 */
    AgcCborDecOpts o = {.allow_full_int = true};
    int err = 0;
    AgcCbor *v = agentc_cbor_decode(a, buf, n, &o, &err);
    check("cbor.full_int", v != NULL && agentc_cbor_uint(v) == (1ull << 53));
    check("cbor.full_int.not_json", !agentc_cbor_is_json(v));
}

static void test_limits(AgcCborArena *a) {
    /* depth: 65 nested arrays */
    u8 buf[128];
    size_t n = 0;
    for (int i = 0; i < 65; i++)
        buf[n++] = 0x81;
    buf[n++] = 0x00;
    int err = 0;
    AgcCbor *v = agentc_cbor_decode(a, buf, n, NULL, &err);
    check("cbor.limit.depth", v == NULL && err == -90);

    /* items: a 2-element array with max_items = 2 */
    size_t m = unhex("83010203", buf, sizeof buf);
    AgcCborDecOpts o = {.max_items = 2};
    v = agentc_cbor_decode(a, buf, m, &o, &err);
    check("cbor.limit.items", v == NULL && err == -90);

    /* input cap: one byte over 16 MiB is refused before parsing */
    check("cbor.limit.input",
          agentc_cbor_decode(a, buf, AGENTC_CBOR_MAX_BYTES + 1, NULL, &err) == NULL && err == -90);
}

static void test_writer(AgcCborArena *a) {
    AgcBuf out = {0};
    AgcCborW w;
    agentc_cborw_init(&w, &out);
    agentc_cborw_map(&w, 3);
    agentc_cborw_text(&w, "a", 1);
    agentc_cborw_uint(&w, 1);
    agentc_cborw_text(&w, "b", 1);
    agentc_cborw_array(&w, 2);
    agentc_cborw_uint(&w, 2);
    agentc_cborw_uint(&w, 3);
    agentc_cborw_text(&w, "n", 1);
    agentc_cborw_int(&w, -1000);
    check("cbor.write.minimal", !w.failed);
    u8 want[64];
    size_t wn = unhex("a36161016162820203616e3903e7", want, sizeof want);
    check("cbor.write.bytes", out.len == wn && agentc_memeq(out.p, want, wn));
    agentc_buf_free(&out);

    /* framed writer patches the length prefix in place */
    AgcBuf framed = {0};
    check("cbor.frame.init", agentc_cborw_framed_init(&w, &framed) == 0);
    agentc_cborw_uint(&w, 24);
    check("cbor.frame.finish", agentc_cborw_finish(&w) == 0);
    /* 00 00 00 02 | 18 18 */
    check("cbor.frame.bytes",
          framed.len == 6 && framed.p[3] == 2 && framed.p[4] == 0x18 && framed.p[5] == 0x18);
    agentc_buf_free(&framed);
    (void)a;
}

static void test_is_json(AgcCborArena *a) {
    int err = 0;
    u8 buf[16];
    size_t n = unhex("4401020304", buf, sizeof buf);
    AgcCbor *bytes = agentc_cbor_decode(a, buf, n, NULL, &err);
    check("cbor.isjson.bytes", !agentc_cbor_is_json(bytes));
    n = unhex("fb3ff8000000000000", buf, sizeof buf);
    AgcCbor *f = agentc_cbor_decode(a, buf, n, NULL, &err);
    check("cbor.isjson.f64", !agentc_cbor_is_json(f));
    AgcCbor *mixed = agentc_cbor_new_array(a, 2);
    agentc_cbor_array_set(mixed, 0, agentc_cbor_new_uint(a, 1));
    agentc_cbor_array_set(mixed, 1, agentc_cbor_new_text(a, "x", 1));
    check("cbor.isjson.ok", agentc_cbor_is_json(mixed));
}

/* C3: empty text/bytes must keep a NUL-terminated payload so the JSON bridge
 * emits "" instead of dropping the value (which produced invalid JSON). */
static void test_empty(AgcCborArena *a) {
    int err = 0;
    u8 buf[32];
    size_t n = unhex("60", buf, sizeof buf);
    AgcCbor *v = agentc_cbor_decode(a, buf, n, NULL, &err);
    size_t len = 99;
    const u8 *p = agentc_cbor_text(v, &len);
    check("cbor.empty.text", v != NULL && len == 0 && p != NULL && p[0] == 0);
    AgcBuf out = {0};
    check("cbor.empty.text.json",
          agentc_cbor_to_json(v, &out) == 0 && out.len == 2 && agentc_memeq(out.p, "\"\"", 2));
    agentc_buf_free(&out);

    n = unhex("40", buf, sizeof buf);
    v = agentc_cbor_decode(a, buf, n, NULL, &err);
    len = 99;
    p = agentc_cbor_bytes(v, &len);
    check("cbor.empty.bytes", v != NULL && len == 0 && p != NULL && p[0] == 0);

    /* decoded {"a":""} and {"a":["","x"]} must survive the bridge */
    n = unhex("a1616160", buf, sizeof buf);
    v = agentc_cbor_decode(a, buf, n, NULL, &err);
    out = (AgcBuf){0};
    const char *want_map = "{\"a\":\"\"}";
    check("cbor.empty.map", v != NULL && agentc_cbor_to_json(v, &out) == 0 &&
                               out.len == agentc_strlen(want_map) &&
                               agentc_memeq(out.p, want_map, out.len));
    agentc_buf_free(&out);

    n = unhex("a1616182606178", buf, sizeof buf);
    v = agentc_cbor_decode(a, buf, n, NULL, &err);
    out = (AgcBuf){0};
    const char *want_arr = "{\"a\":[\"\",\"x\"]}";
    check("cbor.empty.arr", v != NULL && agentc_cbor_to_json(v, &out) == 0 &&
                               out.len == agentc_strlen(want_arr) &&
                               agentc_memeq(out.p, want_arr, out.len));
    agentc_buf_free(&out);

    /* builder path: an empty text built directly is non-NULL and JSON-safe */
    out = (AgcBuf){0};
    check("cbor.empty.builder", agentc_cbor_to_json(agentc_cbor_new_text(a, "", 0), &out) == 0 &&
                                    out.len == 2 && agentc_memeq(out.p, "\"\"", 2));
    agentc_buf_free(&out);
}

/* CBOR map keys with an embedded NUL must not collapse: the bridge escapes the
 * NUL as \u0000 and keeps the length. */
static void test_nul_keys(AgcCborArena *a) {
    AgcCbor *m = agentc_cbor_new_map(a, 2);
    agentc_cbor_map_set(m, 0, agentc_cbor_new_text(a, "a\0b", 3), agentc_cbor_new_uint(a, 1));
    agentc_cbor_map_set(m, 1, agentc_cbor_new_text(a, "a\0c", 3), agentc_cbor_new_uint(a, 2));
    AgcBuf out = {0};
    const char *want = "{\"a\\u0000b\":1,\"a\\u0000c\":2}";
    check("cbor.nulkey.json", agentc_cbor_to_json(m, &out) == 0 && out.len == agentc_strlen(want) &&
                                  agentc_memeq(out.p, want, out.len));
    agentc_buf_free(&out);
}

/* The item cap counts each decoded value exactly once (the old pre-charge
 * made the effective cap roughly half). */
static void test_item_budget(AgcCborArena *a) {
    int err = 0;
    u8 buf[32];
    /* array(5 scalars) = 6 items: fits a cap of 6 */
    buf[0] = (u8)(0x80 | 5);
    for (int i = 1; i <= 5; i++)
        buf[i] = 0x00;
    AgcCborDecOpts o6 = {.max_items = 6};
    AgcCbor *v = agentc_cbor_decode(a, buf, 6, &o6, &err);
    check("cbor.items.single", v != NULL && agentc_cbor_len(v) == 5);
    /* array(6 scalars) = 7 items: over the cap */
    buf[0] = (u8)(0x80 | 6);
    for (int i = 1; i <= 6; i++)
        buf[i] = 0x00;
    v = agentc_cbor_decode(a, buf, 7, &o6, &err);
    check("cbor.items.cap", v == NULL && err == -90);
    /* map(1 pair) = 3 items: fits a cap of 3 */
    size_t m = unhex("a1616101", buf, sizeof buf);
    AgcCborDecOpts o3 = {.max_items = 3};
    v = agentc_cbor_decode(a, buf, m, &o3, &err);
    check("cbor.items.map_single", v != NULL && agentc_cbor_len(v) == 1);
}

static void test_bridge(AgcCborArena *a) {
    int err = 0;
    const char *json =
        "{\"name\":\"agentc\",\"n\":-3,\"ok\":true,\"list\":[1,null,\"x\"],\"e\":{}}";
    AgcCbor *v = agentc_cbor_from_json(a, json, agentc_strlen(json), &err);
    check("cbor.bridge.from", v != NULL && agentc_cbor_is_json(v));
    AgcBuf out = {0};
    check("cbor.bridge.to", agentc_cbor_to_json(v, &out) == 0);
    check("cbor.bridge.roundtrip",
          out.len == agentc_strlen(json) && agentc_memeq(out.p, json, out.len));
    agentc_buf_free(&out);

    /* floats on either side are unsupported */
    v = agentc_cbor_from_json(a, "1.5", 3, &err);
    check("cbor.bridge.json_float", v == NULL && err == -95);
    u8 buf[16];
    size_t n = unhex("fb3ff8000000000000", buf, sizeof buf);
    v = agentc_cbor_decode(a, buf, n, NULL, &err);
    out.len = 0;
    check("cbor.bridge.to_f64", agentc_cbor_to_json(v, &out) == -95);
    agentc_buf_free(&out);

    /* duplicate JSON keys are rejected */
    v = agentc_cbor_from_json(a, "{\"a\":1,\"a\":2}", 13, &err);
    check("cbor.bridge.json_dup", v == NULL && err == -71);

    /* invalid UTF-8 in a JSON string is rejected */
    v = agentc_cbor_from_json(a, "\"\xc3\x28\"", 4, &err);
    check("cbor.bridge.json_badutf8", v == NULL && err == -71);

    /* C3: empty strings on both sides of the bridge */
    const char *empty_json = "{\"a\":\"\",\"b\":[\"\",\"x\"]}";
    v = agentc_cbor_from_json(a, empty_json, agentc_strlen(empty_json), &err);
    check("cbor.bridge.empty_from", v != NULL && agentc_cbor_is_json(v));
    out = (AgcBuf){0};
    check("cbor.bridge.empty_to", agentc_cbor_to_json(v, &out) == 0);
    check("cbor.bridge.empty_roundtrip",
          out.len == agentc_strlen(empty_json) && agentc_memeq(out.p, empty_json, out.len));
    agentc_buf_free(&out);
}

static void test_mem(void) {
    size_t before = agentc_mem_live();
    {
        AgcCborArena *a = agentc_cbor_arena_new(0);
        u8 buf[16];
        size_t n = unhex("a26161016162820203", buf, sizeof buf);
        int err = 0;
        AgcCbor *v = agentc_cbor_decode(a, buf, n, NULL, &err);
        check("cbor.mem.decode", v != NULL);
        AgcBuf out = {0};
        (void)agentc_cbor_to_json(v, &out);
        agentc_buf_free(&out);
        AgcCbor *j = agentc_cbor_from_json(a, "{\"x\":[1,2]}", 11, &err);
        check("cbor.mem.bridge", j != NULL);
        agentc_cbor_arena_free(a);
    }
    check("cbor.mem.baseline", agentc_mem_live() == before);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    AgcCborArena *a = agentc_cbor_arena_new(0);
    test_vectors(a);
    test_full_int(a);
    test_limits(a);
    test_writer(a);
    test_is_json(a);
    test_empty(a);
    test_nul_keys(a);
    test_item_budget(a);
    test_bridge(a);
    agentc_cbor_arena_free(a);
    test_mem();
    return fails;
}
