/* cbor_json.c — JSON text <-> AgcCbor bridge (P5 Axis C, C-D1).
 *
 * See include/cbor_json.h. JSON is parsed with the shared AgcJson parser (in an
 * isolated arena) and written with the shared AgcJsonW streaming writer, so the
 * two framings cannot drift. Integer-only: float/exponent JSON and CBOR f64
 * are `unsupported` in v1.
 */
#include "cbor_json.h"
#include "wire.h"

#define BR_EINVAL (-22)
#define BR_EPROTO (-71)
#define BR_ENOTSUP (-95)

/* ------------------------------------------------------------------ to CBOR */
static AgcCbor *json_to_cbor(AgcCborArena *a, const AgcJson *v, int *err) {
    switch (agentc_json_type(v)) {
    case AGENTC_JSON_NULL:
        return agentc_cbor_new_null(a);
    case AGENTC_JSON_TRUE:
        return agentc_cbor_new_bool(a, true);
    case AGENTC_JSON_FALSE:
        return agentc_cbor_new_bool(a, false);
    case AGENTC_JSON_NUM: {
        size_t n = 0;
        const char *p = agentc_json_num(v, &n);
        /* The parser already enforced JSON number grammar. An integer has no
         * '.', 'e' or 'E'; anything else is a float the wire path cannot carry. */
        for (size_t i = 0; i < n; i++) {
            if (p[i] == '.' || p[i] == 'e' || p[i] == 'E') {
                if (err)
                    *err = BR_ENOTSUP;
                return NULL;
            }
        }
        bool ok = false;
        i64 iv = agentc_parse_i64(p, n, &ok);
        if (!ok) {
            if (err)
                *err = BR_EPROTO;
            return NULL;
        }
        u64 mag = iv < 0 ? (u64)(-(iv + 1)) + 1 : (u64)iv;
        if (mag > AGENTC_CBOR_MAX_SAFE) {
            if (err)
                *err = BR_EPROTO; /* pi-safe range */
            return NULL;
        }
        return agentc_cbor_new_int(a, iv);
    }
    case AGENTC_JSON_STR: {
        size_t n = 0;
        const char *p = agentc_json_str(v, &n);
        if (!agentc_cbor_utf8_valid((const u8 *)p, n)) {
            if (err)
                *err = BR_EPROTO;
            return NULL;
        }
        return agentc_cbor_new_text(a, p, n);
    }
    case AGENTC_JSON_ARR: {
        size_t n = agentc_json_len(v);
        AgcCbor *arr = agentc_cbor_new_array(a, n);
        for (size_t i = 0; i < n; i++) {
            AgcCbor *item = json_to_cbor(a, agentc_json_at(v, i), err);
            if (!item)
                return NULL;
            agentc_cbor_array_set(arr, i, item);
        }
        return arr;
    }
    case AGENTC_JSON_OBJ: {
        size_t n = (size_t)-1;
        size_t count = 0;
        while (agentc_json_key_at(v, count))
            count++;
        (void)n;
        AgcCbor *map = agentc_cbor_new_map(a, count);
        for (size_t i = 0; i < count; i++) {
            const AgcJson *k = agentc_json_key_at(v, i);
            size_t kn = 0;
            const char *kp = agentc_json_str(k, &kn);
            if (!kp || !agentc_cbor_utf8_valid((const u8 *)kp, kn)) {
                if (err)
                    *err = BR_EPROTO;
                return NULL;
            }
            /* Reject duplicate keys (the codec already rejects them on decode). */
            for (size_t j = 0; j < i; j++) {
                size_t jn = 0;
                const char *jp = agentc_json_str(agentc_json_key_at(v, j), &jn);
                if (jp && jn == kn && agentc_memeq(jp, kp, kn)) {
                    if (err)
                        *err = BR_EPROTO;
                    return NULL;
                }
            }
            AgcCbor *key = agentc_cbor_new_text(a, kp, kn);
            AgcCbor *val = json_to_cbor(a, agentc_json_val_at(v, i), err);
            if (!val)
                return NULL;
            agentc_cbor_map_set(map, i, key, val);
        }
        return map;
    }
    default:
        if (err)
            *err = BR_EINVAL;
        return NULL;
    }
}

AgcCbor *agentc_cbor_from_json(AgcCborArena *a, const char *p, size_t n, int *err) {
    if (err)
        *err = 0;
    if (!a || (n != 0 && !p)) {
        if (err)
            *err = BR_EINVAL;
        return NULL;
    }
    AgcJsonArena *ja = agentc_json_arena_new(0);
    if (!ja) {
        if (err)
            *err = -12;
        return NULL;
    }
    AgcJson *j = agentc_json_parse_in(ja, p, n);
    if (!j) {
        agentc_json_arena_free(ja);
        if (err)
            *err = BR_EPROTO;
        return NULL;
    }
    agentc_cbor_arena_clear(a);
    AgcCbor *v = json_to_cbor(a, j, err);
    if (!v)
        agentc_cbor_arena_clear(a);
    agentc_json_arena_free(ja);
    return v;
}

/* ------------------------------------------------------------------ to JSON */
static int cbor_to_json(const AgcCbor *v, AgcJsonW *w) {
    switch (agentc_cbor_type(v)) {
    case AGENTC_CBOR_UINT:
    case AGENTC_CBOR_NEG:
        agentc_jsonw_i64(w, agentc_cbor_int(v));
        return 0;
    case AGENTC_CBOR_TEXT: {
        size_t n = 0;
        const u8 *p = agentc_cbor_text(v, &n);
        /* cbor_text() always owns a NUL byte, but stay defensive: a missing
         * payload with n == 0 must still emit a valid "" not nothing. */
        agentc_jsonw_str(w, (const char *)(p ? p : (const u8 *)""), n);
        return 0;
    }
    case AGENTC_CBOR_BOOL:
        agentc_jsonw_bool(w, agentc_cbor_bool(v));
        return 0;
    case AGENTC_CBOR_NULL:
        agentc_jsonw_null(w);
        return 0;
    case AGENTC_CBOR_ARRAY: {
        agentc_jsonw_arr(w);
        for (size_t i = 0; i < agentc_cbor_len(v); i++) {
            if (cbor_to_json(agentc_cbor_at(v, i), w) != 0)
                return BR_ENOTSUP;
        }
        agentc_jsonw_end(w);
        return 0;
    }
    case AGENTC_CBOR_MAP: {
        agentc_jsonw_obj(w);
        for (size_t i = 0; i < agentc_cbor_len(v); i++) {
            AgcCbor *k = agentc_cbor_key(v, i);
            if (!k || agentc_cbor_type(k) != AGENTC_CBOR_TEXT)
                return BR_ENOTSUP;
            size_t kn = 0;
            const u8 *kp = agentc_cbor_text(k, &kn);
            /* Length-taking key writer: a text key with an embedded NUL is
             * emitted as \u0000 instead of truncating at the NUL (which would
             * collapse distinct keys and drop bytes). */
            agentc_jsonw_keyn(w, (const char *)(kp ? kp : (const u8 *)""), kn);
            if (cbor_to_json(agentc_cbor_val(v, i), w) != 0)
                return BR_ENOTSUP;
        }
        agentc_jsonw_end(w);
        return 0;
    }
    default:
        return BR_ENOTSUP; /* bytes / f64 have no JSON form in v1 */
    }
}

int agentc_cbor_to_json(const AgcCbor *v, AgcBuf *out) {
    if (!v || !out)
        return BR_EINVAL;
    if (!agentc_cbor_is_json(v))
        return BR_ENOTSUP;
    AgcJsonW w;
    agentc_jsonw_init(&w, out);
    int rc = cbor_to_json(v, &w);
    if (rc != 0)
        agentc_buf_clear(out);
    return rc;
}
