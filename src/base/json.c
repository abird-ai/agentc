/* json.c — JSONC parser (comments + trailing commas) for agentc.
 *
 * Each AgcJsonArena owns one document: nodes and decoded strings live in a
 * chained block arena, and the parse cursor/scratch state is part of the arena
 * struct, so independent parses no longer invalidate each other.
 * agentc_json_parse() uses a process default arena and agentc_json_reset()
 * recycles it; agentc_json_parse_in() is the isolated form.
 */
#include "agentc.h"
#include "base/limits.h"

struct AgcJson {
    int type;      /* AGENTC_JSON_* */
    size_t n;      /* elements (arr), pairs (obj), bytes (str/num) */
    void *ptr;     /* AGENTC_JSON_ARR: AgcJson**; OBJ: AgcJson** key,value pairs;
                      STR: NUL-terminated bytes; NUM: NUL-terminated text */
};

struct AgcJsonArena {
    u8 *arena;               /* newest block; first 8 bytes link to the previous */
    size_t arena_cap;
    size_t arena_top;
    AgcVec children;      /* pending AgcJson* during container parse */
    const char *cur, *end;   /* parse cursor */
    int fail;
    int depth;
};

#define ARENA_BLOCK (1u << 20)

static AgcJsonArena g_default;   /* zero-initialized; backs agentc_json_parse */

static void arena_grow(AgcJsonArena *a, size_t need) {
    size_t cap = a->arena_cap ? a->arena_cap * 2 : ARENA_BLOCK;
    if (cap < need + 8) cap = need + 8;
    u8 *p = agentc_alloc(cap);
    *(u8 **)p = a->arena;
    a->arena = p;
    a->arena_cap = cap;
    a->arena_top = 8;
}

static void *anew(AgcJsonArena *a, size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (a->arena_top + n > a->arena_cap) arena_grow(a, n);
    void *p = a->arena + a->arena_top;
    a->arena_top += n;
    return p;
}

static AgcJson *node(AgcJsonArena *a, int type) {
    AgcJson *v = anew(a, sizeof *v);
    v->type = type;
    return v;
}

AgcJsonArena *agentc_json_arena_new(size_t initial_bytes) {
    AgcJsonArena *a = agentc_alloc(sizeof *a);   /* zeroed */
    size_t cap = initial_bytes ? initial_bytes : ARENA_BLOCK;
    u8 *p = agentc_alloc_try(cap);
    if (!p) {
        agentc_free(a);
        return NULL;
    }
    *(u8 **)p = NULL;
    a->arena = p;
    a->arena_cap = cap;
    a->arena_top = 8;
    return a;
}

void agentc_json_arena_clear(AgcJsonArena *a) {
    if (!a || !a->arena) return;
    u8 *blk = *(u8 **)a->arena;
    *(u8 **)a->arena = NULL;
    while (blk) {
        u8 *prev = *(u8 **)blk;
        agentc_free(blk);
        blk = prev;
    }
    a->arena_top = 8;
    a->children.len = 0;
}

void agentc_json_arena_free(AgcJsonArena *a) {
    if (!a) return;
    agentc_json_arena_clear(a);
    agentc_free(a->arena);
    agentc_free(a->children.p);
    agentc_free(a);
}

void agentc_json_reset(void) {
    agentc_json_arena_clear(&g_default);
}

static void skip_ws(AgcJsonArena *a) {
    for (;;) {
        while (a->cur < a->end && (*a->cur == ' ' || *a->cur == '\t' || *a->cur == '\n' || *a->cur == '\r')) a->cur++;
        if (a->cur + 1 < a->end && a->cur[0] == '/' && a->cur[1] == '/') {
            a->cur += 2;
            while (a->cur < a->end && *a->cur != '\n') a->cur++;
            continue;
        }
        if (a->cur + 1 < a->end && a->cur[0] == '/' && a->cur[1] == '*') {
            a->cur += 2;
            while (a->cur + 1 < a->end && !(a->cur[0] == '*' && a->cur[1] == '/')) a->cur++;
            if (a->cur + 1 >= a->end) { a->fail = 1; return; }
            a->cur += 2;
            continue;
        }
        return;
    }
}

static bool lit(AgcJsonArena *a, const char *s) {
    size_t n = agentc_strlen(s);
    if ((size_t)(a->end - a->cur) < n || !agentc_memeq(a->cur, s, n)) return false;
    a->cur += n;
    return true;
}

static u32 hexval(AgcJsonArena *a, char c) {
    if (c >= '0' && c <= '9') return (u32)(c - '0');
    if (c >= 'a' && c <= 'f') return (u32)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (u32)(c - 'A' + 10);
    a->fail = 1;
    return 0;
}

static AgcJson *parse_value(AgcJsonArena *a);

static AgcJson *parse_string_node(AgcJsonArena *a) {
    const char *p = a->cur + 1;
    const char *q = p;
    while (q < a->end && *q != '"') {
        if (*q == '\\') {
            q++;
            if (q >= a->end) { a->fail = 1; return NULL; }
        } else if ((unsigned char)*q < 0x20) {
            /* RFC 8259: an unescaped control byte (0x00-0x1F) is not allowed in
             * a string. 0x7F is legal and left alone. */
            a->fail = 1;
            return NULL;
        }
        q++;
    }
    if (q >= a->end) { a->fail = 1; return NULL; }
    char *out = anew(a, (size_t)(q - p) + 1);
    size_t o = 0;
    while (p < q) {
        if (*p != '\\') { out[o++] = *p++; continue; }
        p++;
        char e = *p++;
        switch (e) {
        case '"': out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; break;
        case '/': out[o++] = '/'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u': {
            if (q - p < 4) { a->fail = 1; return NULL; }
            u32 cp = 0;
            for (int i = 0; i < 4; i++) cp = (cp << 4) | hexval(a, p[i]);
            p += 4;
            if (a->fail) return NULL;
            if (cp >= 0xD800 && cp <= 0xDBFF && q - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                u32 lo = 0;
                for (int i = 0; i < 4; i++) lo = (lo << 4) | hexval(a, p[2 + i]);
                if (a->fail) return NULL;
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            /* A high surrogate without its low half, or a lone low surrogate,
             * is not a scalar value: substitute U+FFFD rather than emitting
             * CESU-8 (invalid UTF-8). */
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
            o += agentc_utf8_encode(cp, (u8 *)out + o);
            break;
        }
        default:
            a->fail = 1;
            return NULL;
        }
    }
    out[o] = 0;
    a->cur = q + 1;
    AgcJson *v = node(a, AGENTC_JSON_STR);
    v->n = o;
    v->ptr = out;
    return v;
}

static AgcJson *parse_number(AgcJsonArena *a) {
    const char *start = a->cur;
    const char *p = a->cur;
    /* JSONC keeps JSON's number grammar (comments/trailing commas are the only
     * leniency): optional '-', integer part 0 or [1-9][0-9]*, optional fraction
     * and optional exponent. Any other run is rejected, so a malformed token
     * can never be re-emitted verbatim as invalid JSON. */
    if (p < a->end && *p == '-') p++;
    if (p >= a->end) { a->fail = 1; return NULL; }
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (p < a->end && *p >= '0' && *p <= '9') p++;
    } else {
        a->fail = 1;
        return NULL;
    }
    if (p < a->end && *p == '.') {
        p++;
        if (p >= a->end || *p < '0' || *p > '9') { a->fail = 1; return NULL; }
        while (p < a->end && *p >= '0' && *p <= '9') p++;
    }
    if (p < a->end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < a->end && (*p == '+' || *p == '-')) p++;
        if (p >= a->end || *p < '0' || *p > '9') { a->fail = 1; return NULL; }
        while (p < a->end && *p >= '0' && *p <= '9') p++;
    }
    size_t n = (size_t)(p - start);
    a->cur = p;
    char *copy = anew(a, n + 1);
    agentc_memcpy(copy, start, n);
    copy[n] = 0;
    AgcJson *v = node(a, AGENTC_JSON_NUM);
    v->n = n;
    v->ptr = copy;
    return v;
}

static AgcJson *parse_array(AgcJsonArena *a) {
    AgcJson *v = node(a, AGENTC_JSON_ARR);
    a->cur++;                               /* '[' */
    size_t base = a->children.len;
    for (;;) {
        skip_ws(a);
        if (a->fail) return NULL;
        if (a->cur < a->end && *a->cur == ']') { a->cur++; break; }
        AgcJson *item = parse_value(a);
        if (!item) return NULL;
        *(AgcJson **)agentc_vec_push(&a->children, sizeof(AgcJson *)) = item;
        skip_ws(a);
        if (a->fail) return NULL;
        if (a->cur < a->end && *a->cur == ',') { a->cur++; continue; }   /* trailing comma ok */
        if (a->cur < a->end && *a->cur == ']') { a->cur++; break; }
        a->fail = 1;
        return NULL;
    }
    size_t n = a->children.len - base;
    AgcJson **arr = anew(a, n * sizeof(AgcJson *));
    agentc_memcpy(arr, (AgcJson **)a->children.p + base, n * sizeof(AgcJson *));
    a->children.len = base;
    v->n = n;
    v->ptr = arr;
    return v;
}

static AgcJson *parse_object(AgcJsonArena *a) {
    AgcJson *v = node(a, AGENTC_JSON_OBJ);
    a->cur++;                               /* '{' */
    size_t base = a->children.len;
    for (;;) {
        skip_ws(a);
        if (a->fail) return NULL;
        if (a->cur < a->end && *a->cur == '}') { a->cur++; break; }
        if (a->cur >= a->end || *a->cur != '"') { a->fail = 1; return NULL; }
        AgcJson *key = parse_string_node(a);
        if (!key) return NULL;
        *(AgcJson **)agentc_vec_push(&a->children, sizeof(AgcJson *)) = key;
        skip_ws(a);
        if (a->fail) return NULL;
        if (a->cur >= a->end || *a->cur != ':') { a->fail = 1; return NULL; }
        a->cur++;
        AgcJson *val = parse_value(a);
        if (!val) return NULL;
        *(AgcJson **)agentc_vec_push(&a->children, sizeof(AgcJson *)) = val;
        skip_ws(a);
        if (a->fail) return NULL;
        if (a->cur < a->end && *a->cur == ',') { a->cur++; continue; }   /* trailing comma ok */
        if (a->cur < a->end && *a->cur == '}') { a->cur++; break; }
        a->fail = 1;
        return NULL;
    }
    size_t n = a->children.len - base;
    AgcJson **pairs = anew(a, n * sizeof(AgcJson *));
    agentc_memcpy(pairs, (AgcJson **)a->children.p + base, n * sizeof(AgcJson *));
    a->children.len = base;
    v->n = n / 2;
    v->ptr = pairs;
    return v;
}

static AgcJson *parse_value(AgcJsonArena *a) {
    skip_ws(a);
    if (a->fail) return NULL;
    if (a->cur >= a->end) { a->fail = 1; return NULL; }
    char c = *a->cur;
    if (c == '{' || c == '[') {
        if (++a->depth > AGENTC_JSON_MAX_DEPTH) { a->fail = 1; return NULL; }
        AgcJson *v = (c == '{') ? parse_object(a) : parse_array(a);
        a->depth--;
        return v;
    }
    if (c == '"') return parse_string_node(a);
    if (c == 't') { if (lit(a, "true")) return node(a, AGENTC_JSON_TRUE); a->fail = 1; return NULL; }
    if (c == 'f') { if (lit(a, "false")) return node(a, AGENTC_JSON_FALSE); a->fail = 1; return NULL; }
    if (c == 'n') { if (lit(a, "null")) return node(a, AGENTC_JSON_NULL); a->fail = 1; return NULL; }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(a);
    a->fail = 1;
    return NULL;
}

AgcJson *agentc_json_parse_in(AgcJsonArena *a, const char *p, size_t n) {
    if (!a) return NULL;
    agentc_json_arena_clear(a);
    a->cur = p;
    a->end = p + n;
    a->fail = 0;
    a->depth = 0;
    AgcJson *v = parse_value(a);
    if (v) {
        skip_ws(a);
        if (a->fail || a->cur != a->end) v = NULL;
    }
    if (!v) {
        agentc_json_arena_clear(a);
        return NULL;
    }
    return v;
}

AgcJson *agentc_json_parse(const char *p, size_t n) {
    if (!g_default.arena) arena_grow(&g_default, 0);
    return agentc_json_parse_in(&g_default, p, n);
}

/* ---------------------------------------------------------------- accessors */
int agentc_json_type(const AgcJson *v) { return v ? v->type : -1; }

AgcJson *agentc_json_get(const AgcJson *o, const char *key) {
    if (!o || o->type != AGENTC_JSON_OBJ) return NULL;
    AgcJson **pairs = o->ptr;
    size_t klen = agentc_strlen(key);
    for (size_t i = 0; i < o->n; i++) {
        AgcJson *k = pairs[2 * i];
        if (k->n == klen && agentc_memeq(k->ptr, key, klen)) return pairs[2 * i + 1];
    }
    return NULL;
}

const char *agentc_json_get_str(const AgcJson *o, const char *key) {
    AgcJson *v = agentc_json_get(o, key);
    return (v && v->type == AGENTC_JSON_STR) ? (const char *)v->ptr : NULL;
}

i64 agentc_json_get_int(const AgcJson *o, const char *key, i64 dflt) {
    AgcJson *v = agentc_json_get(o, key);
    if (!v || v->type != AGENTC_JSON_NUM || v->n == 0) return dflt;
    const char *p = (const char *)v->ptr;
    size_t n = v->n;
    /* A JSON integer is digits with an optional leading '-': reject fractions,
     * exponents, a leading '+' and any other byte. The number tokenizer already
     * enforces this, but the accessor must not depend on it. */
    size_t i = 0;
    if (p[i] == '-') i++;
    if (i == n) return dflt;
    for (size_t j = i; j < n; j++)
        if (p[j] < '0' || p[j] > '9') return dflt;
    bool ok = false;
    i64 r = agentc_parse_i64(p, n, &ok);
    return ok ? r : dflt;
}

bool agentc_json_get_bool(const AgcJson *o, const char *key, bool dflt) {
    AgcJson *v = agentc_json_get(o, key);
    if (!v) return dflt;
    if (v->type == AGENTC_JSON_TRUE) return true;
    if (v->type == AGENTC_JSON_FALSE) return false;
    return dflt;
}

size_t agentc_json_len(const AgcJson *arr) {
    return (arr && arr->type == AGENTC_JSON_ARR) ? arr->n : 0;
}

AgcJson *agentc_json_at(const AgcJson *arr, size_t i) {
    if (!arr || arr->type != AGENTC_JSON_ARR || i >= arr->n) return NULL;
    return ((AgcJson **)arr->ptr)[i];
}

const char *agentc_json_str(const AgcJson *v, size_t *len) {
    if (!v || v->type != AGENTC_JSON_STR) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = v->n;
    return v->ptr;
}

const char *agentc_json_num(const AgcJson *v, size_t *len) {
    if (!v || v->type != AGENTC_JSON_NUM) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = v->n;
    return v->ptr;
}

const AgcJson *agentc_json_key_at(const AgcJson *o, size_t i) {
    if (!o || o->type != AGENTC_JSON_OBJ || i >= o->n) return NULL;
    return ((AgcJson **)o->ptr)[2 * i];
}

const AgcJson *agentc_json_val_at(const AgcJson *o, size_t i) {
    if (!o || o->type != AGENTC_JSON_OBJ || i >= o->n) return NULL;
    return ((AgcJson **)o->ptr)[2 * i + 1];
}

bool agentc_json_is(const AgcJson *v, const char *s) {
    return v && v->type == AGENTC_JSON_STR && agentc_str_eq_cstr((const char *)v->ptr, v->n, s);
}
