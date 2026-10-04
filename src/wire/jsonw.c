/* jsonw.c — streaming JSON writer over AgcBuf (RFC 8259, no whitespace).
 *
 * Strings escape '"', '\\' and the control characters (\\b \\f \\n \\r \\t,
 * otherwise \\u00XX with lowercase hex). With w->ascii off (the default) bytes
 * >= 0x80 pass through as UTF-8; with it on they are emitted as \\uXXXX
 * (a surrogate pair for non-BMP code points), mirroring base/out.c's lenient
 * UTF-8 decode for invalid bytes.
 * agentc_jsonw_key() emits the separator, so a value following a key never emits
 * a second comma.
 *
 * The writer state is self-contained in AgcJsonW: the depth counter, the
 * per-level "comma pending" bits and the per-level container type stack. No
 * file-static state remains, so any number of writers may be used interleaved.
 */
#include "net/net_internal.h"
#include "base/limits.h"

/* Lenient UTF-8 decode mirroring base/out.c: an invalid lead byte is returned
 * as-is with used = 1, so escaping never drops or re-frames bytes it does not
 * understand. */
static u32 json_utf8_next(const u8 *p, size_t n, size_t *used) {
    u8 c = p[0];

    if (c < 0x80u || n == 1) {
        *used = 1;
        return c;
    }
    if ((c & 0xe0u) == 0xc0u && n >= 2 && (p[1] & 0xc0u) == 0x80u) {
        *used = 2;
        return ((u32)(c & 0x1fu) << 6) | (u32)(p[1] & 0x3fu);
    }
    if ((c & 0xf0u) == 0xe0u && n >= 3 && (p[1] & 0xc0u) == 0x80u &&
        (p[2] & 0xc0u) == 0x80u) {
        *used = 3;
        return ((u32)(c & 0x0fu) << 12) | ((u32)(p[1] & 0x3fu) << 6) |
               (u32)(p[2] & 0x3fu);
    }
    if ((c & 0xf8u) == 0xf0u && n >= 4 && (p[1] & 0xc0u) == 0x80u &&
        (p[2] & 0xc0u) == 0x80u && (p[3] & 0xc0u) == 0x80u) {
        *used = 4;
        return ((u32)(c & 0x07u) << 18) | ((u32)(p[1] & 0x3fu) << 12) |
               ((u32)(p[2] & 0x3fu) << 6) | (u32)(p[3] & 0x3fu);
    }
    *used = 1;
    return c;
}

/* \\uXXXX for BMP, a surrogate pair for non-BMP (lowercase hex). */
static void json_escape_cp(AgcBuf *b, u32 cp) {
    static const char hex[] = "0123456789abcdef";
    char tmp[12];

    if (cp > 0xffffu) {
        u32 v = cp - 0x10000u;
        u32 hi = 0xd800u + (v >> 10), lo = 0xdc00u + (v & 0x3ffu);
        tmp[0] = '\\'; tmp[1] = 'u';
        tmp[2] = hex[(hi >> 12) & 0xf]; tmp[3] = hex[(hi >> 8) & 0xf];
        tmp[4] = hex[(hi >> 4) & 0xf]; tmp[5] = hex[hi & 0xf];
        tmp[6] = '\\'; tmp[7] = 'u';
        tmp[8] = hex[(lo >> 12) & 0xf]; tmp[9] = hex[(lo >> 8) & 0xf];
        tmp[10] = hex[(lo >> 4) & 0xf]; tmp[11] = hex[lo & 0xf];
        agentc_buf_push(b, tmp, sizeof tmp);
        return;
    }
    tmp[0] = '\\'; tmp[1] = 'u';
    tmp[2] = hex[(cp >> 12) & 0xf]; tmp[3] = hex[(cp >> 8) & 0xf];
    tmp[4] = hex[(cp >> 4) & 0xf]; tmp[5] = hex[cp & 0xf];
    agentc_buf_push(b, tmp, 6);
}

static void json_escape_into(AgcBuf *b, const char *s, size_t n, bool ascii) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        u8 c = (u8)s[i];
        switch (c) {
        case '"':
            agentc_buf_cstr(b, "\\\"");
            break;
        case '\\':
            agentc_buf_cstr(b, "\\\\");
            break;
        case '\b':
            agentc_buf_cstr(b, "\\b");
            break;
        case '\f':
            agentc_buf_cstr(b, "\\f");
            break;
        case '\n':
            agentc_buf_cstr(b, "\\n");
            break;
        case '\r':
            agentc_buf_cstr(b, "\\r");
            break;
        case '\t':
            agentc_buf_cstr(b, "\\t");
            break;
        default:
            if (c < 0x20) {
                char tmp[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 0xF] };
                agentc_buf_push(b, tmp, sizeof(tmp));
            } else if (ascii && c >= 0x7fu) {
                size_t used = 0;
                u32 cp = json_utf8_next((const u8 *)s + i, n - i, &used);
                /* CESU-8/overlong bytes can decode to a surrogate half, which
                 * is not a Unicode scalar and must not become a lone \uXXXX
                 * escape; substitute U+FFFD as base/json.c does. */
                if (cp >= 0xD800u && cp <= 0xDFFFu) cp = 0xFFFDu;
                if (cp > 0x10ffffu) {
                    /* not a Unicode scalar: fall back to the lead byte's own
                     * value and let the continuation bytes stand alone */
                    json_escape_cp(b, c);
                    used = 1;
                } else {
                    json_escape_cp(b, cp);
                }
                i += used - 1;
            } else {
                agentc_buf_byte(b, c);
            }
        }
    }
}

static void emit_value(AgcJsonW *w, const AgcJson *v) {
    switch (agentc_json_type(v)) {
    case AGENTC_JSON_TRUE:
        agentc_jsonw_bool(w, true);
        break;
    case AGENTC_JSON_FALSE:
        agentc_jsonw_bool(w, false);
        break;
    case AGENTC_JSON_NUM: {
        size_t n = 0;
        const char *p = agentc_json_num(v, &n);
        agentc_jsonw_raw(w, p ? p : "0", p ? n : 1);
        break;
    }
    case AGENTC_JSON_STR: {
        size_t n = 0;
        const char *p = agentc_json_str(v, &n);
        agentc_jsonw_str(w, p ? p : "", p ? n : 0);
        break;
    }
    case AGENTC_JSON_ARR: {
        agentc_jsonw_arr(w);
        size_t n = agentc_json_len(v);
        for (size_t i = 0; i < n; i++) emit_value(w, agentc_json_at(v, i));
        agentc_jsonw_end(w);
        break;
    }
    case AGENTC_JSON_OBJ: {
        agentc_jsonw_obj(w);
        for (size_t i = 0; i < (size_t)-1; i++) {
            const AgcJson *k = agentc_json_key_at(v, i);
            if (!k) break;
            size_t n = 0;
            const char *key = agentc_json_str(k, &n);
            agentc_jsonw_key(w, key ? key : "");
            emit_value(w, agentc_json_val_at(v, i));
        }
        agentc_jsonw_end(w);
        break;
    }
    default:
        agentc_jsonw_null(w);
        break;
    }
}

void agentc_json_emit(AgcJsonW *w, const AgcJson *v) { emit_value(w, v); }

void agentc_json_escape(AgcBuf *b, const char *s, size_t n) {
    if (b == NULL || s == NULL) return;
    json_escape_into(b, s, n, false);
}

void agentc_jsonw_init(AgcJsonW *w, AgcBuf *b) {
    if (w == NULL) return;
    w->b = b;
    w->depth = 0;
    for (size_t i = 0; i < sizeof(w->need_comma); i++) w->need_comma[i] = false;
    for (size_t i = 0; i < sizeof(w->ctype); i++) w->ctype[i] = 0;
    w->ascii = false;
}

void agentc_jsonw_set_ascii(AgcJsonW *w, bool on) {
    if (w == NULL) return;
    w->ascii = on;
}

/* Emit a separating comma before a value at the current nesting level. */
static void json_before_value(AgcJsonW *w) {
    if (w->b == NULL || w->depth == 0 || w->depth > AGENTC_JSON_MAX_DEPTH) return;
    u32 d = w->depth - 1;
    if (w->need_comma[d]) agentc_buf_byte(w->b, ',');
    w->need_comma[d] = true;
}

static void json_open(AgcJsonW *w, u8 type, char open) {
    if (w->b == NULL || w->depth >= AGENTC_JSON_MAX_DEPTH) return;
    json_before_value(w);
    agentc_buf_byte(w->b, (u8)open);
    w->ctype[w->depth] = type;
    w->depth++;
    w->need_comma[w->depth - 1] = false;
}

void agentc_jsonw_obj(AgcJsonW *w) { json_open(w, 0, '{'); }
void agentc_jsonw_arr(AgcJsonW *w) { json_open(w, 1, '['); }

void agentc_jsonw_end(AgcJsonW *w) {
    if (w == NULL || w->b == NULL || w->depth == 0) return;
    w->depth--;
    agentc_buf_byte(w->b, w->ctype[w->depth] ? ']' : '}');
}

void agentc_jsonw_key(AgcJsonW *w, const char *key) {
    if (w == NULL || w->b == NULL || w->depth == 0 || key == NULL) return;
    u32 d = w->depth - 1;
    if (w->need_comma[d]) agentc_buf_byte(w->b, ',');
    w->need_comma[d] = false;
    agentc_buf_byte(w->b, '"');
    json_escape_into(w->b, key, agentc_strlen(key), w->ascii);
    agentc_buf_byte(w->b, '"');
    agentc_buf_byte(w->b, ':');
}

void agentc_jsonw_str(AgcJsonW *w, const char *s, size_t n) {
    if (w == NULL || w->b == NULL || s == NULL) return;
    json_before_value(w);
    agentc_buf_byte(w->b, '"');
    json_escape_into(w->b, s, n, w->ascii);
    agentc_buf_byte(w->b, '"');
}

void agentc_jsonw_cstr(AgcJsonW *w, const char *s) {
    if (s == NULL) return;
    agentc_jsonw_str(w, s, agentc_strlen(s));
}

void agentc_jsonw_u64(AgcJsonW *w, u64 v) {
    if (w == NULL || w->b == NULL) return;
    json_before_value(w);
    agentc_buf_u64(w->b, v);
}

void agentc_jsonw_i64(AgcJsonW *w, i64 v) {
    if (w == NULL || w->b == NULL) return;
    json_before_value(w);
    agentc_buf_i64(w->b, v);
}

void agentc_jsonw_bool(AgcJsonW *w, bool v) {
    if (w == NULL || w->b == NULL) return;
    json_before_value(w);
    agentc_buf_cstr(w->b, v ? "true" : "false");
}

void agentc_jsonw_null(AgcJsonW *w) {
    if (w == NULL || w->b == NULL) return;
    json_before_value(w);
    agentc_buf_cstr(w->b, "null");
}

void agentc_jsonw_raw(AgcJsonW *w, const char *raw, size_t n) {
    if (w == NULL || w->b == NULL || raw == NULL) return;
    json_before_value(w);
    agentc_buf_push(w->b, raw, n);
}
