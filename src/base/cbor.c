/* cbor.c — the agentc CBOR codec (P5 Axis C, RFC 8949 subset).
 *
 * See include/cbor.h for the contract. Decoder and writer are bounded,
 * allocation-backed and single-threaded; the strict-UTF-8 validator and the
 * pi-safe integer rule are the two protocol-critical validations.
 */
#include "cbor.h"
#include "frame.h"

#define CBOR_EINVAL (-22)
#define CBOR_ERANGE (-34)
#define CBOR_EPROTO (-71)
#define CBOR_EMSGSIZE (-90)
#define CBOR_ENOTSUP (-95)

/* ------------------------------------------------------------------ arena */
struct AgcCborArena {
    u8 *arena; /* newest block; first 8 bytes link the previous */
    size_t arena_cap;
    size_t arena_top;
    AgcVec stack; /* pending children during a container parse */
};

#define CBOR_ARENA_BLOCK (1u << 18)

static void arena_grow(AgcCborArena *a, size_t need) {
    size_t cap = a->arena_cap ? a->arena_cap * 2 : CBOR_ARENA_BLOCK;
    if (cap < need + 8)
        cap = need + 8;
    u8 *p = agentc_alloc(cap);
    *(u8 **)p = a->arena;
    a->arena = p;
    a->arena_cap = cap;
    a->arena_top = 8;
}

static void *cbor_arena_alloc(AgcCborArena *a, size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (a->arena_top + n > a->arena_cap)
        arena_grow(a, n);
    void *p = a->arena + a->arena_top;
    a->arena_top += n;
    agentc_memset(p, 0, n);
    return p;
}

AgcCborArena *agentc_cbor_arena_new(size_t initial_bytes) {
    AgcCborArena *a = agentc_alloc(sizeof *a);
    size_t cap = initial_bytes ? initial_bytes : CBOR_ARENA_BLOCK;
    if (cap < 8)
        cap = 8; /* the first 8 bytes link the previous block */
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

void agentc_cbor_arena_clear(AgcCborArena *a) {
    if (!a || !a->arena)
        return;
    u8 *blk = *(u8 **)a->arena;
    *(u8 **)a->arena = NULL;
    while (blk) {
        u8 *prev = *(u8 **)blk;
        agentc_free(blk);
        blk = prev;
    }
    a->arena_top = 8;
    a->stack.len = 0;
}

void agentc_cbor_arena_free(AgcCborArena *a) {
    if (!a)
        return;
    agentc_cbor_arena_clear(a);
    agentc_free(a->arena);
    agentc_free(a->stack.p);
    agentc_free(a);
}

/* --------------------------------------------------------------- DOM node */
struct AgcCbor {
    u8 type;
    u8 bval;
    size_t n;
    u64 u;
    void *ptr;
};

static AgcCbor *cnew(AgcCborArena *a, u8 type, size_t n) {
    AgcCbor *v = cbor_arena_alloc(a, sizeof *v);
    v->type = type;
    v->n = n;
    return v;
}

/* --------------------------------------------------------------- accessors */
int agentc_cbor_type(const AgcCbor *v) {
    return v ? v->type : -1;
}
size_t agentc_cbor_len(const AgcCbor *v) {
    return v ? v->n : 0;
}

u64 agentc_cbor_uint(const AgcCbor *v) {
    return (v && v->type == AGENTC_CBOR_UINT) ? v->u : 0;
}

i64 agentc_cbor_int(const AgcCbor *v) {
    if (!v)
        return 0;
    if (v->type == AGENTC_CBOR_UINT)
        return v->u <= (u64)INT64_MAX ? (i64)v->u : INT64_MAX;
    if (v->type == AGENTC_CBOR_NEG) {
        u64 m;
        if (v->u >= (u64)INT64_MAX)
            return INT64_MIN;
        m = v->u + 1;
        return -(i64)m;
    }
    return 0;
}

bool agentc_cbor_bool(const AgcCbor *v) {
    return v && v->type == AGENTC_CBOR_BOOL && v->bval;
}

u64 agentc_cbor_f64_bits(const AgcCbor *v) {
    return (v && v->type == AGENTC_CBOR_F64) ? v->u : 0;
}

const u8 *agentc_cbor_bytes(const AgcCbor *v, size_t *len) {
    if (!v || (v->type != AGENTC_CBOR_BYTES && v->type != AGENTC_CBOR_TEXT)) {
        if (len)
            *len = 0;
        return NULL;
    }
    if (len)
        *len = v->n;
    return v->ptr;
}

const u8 *agentc_cbor_text(const AgcCbor *v, size_t *len) {
    if (!v || v->type != AGENTC_CBOR_TEXT) {
        if (len)
            *len = 0;
        return NULL;
    }
    if (len)
        *len = v->n;
    return v->ptr;
}

AgcCbor *agentc_cbor_at(const AgcCbor *arr, size_t i) {
    if (!arr || arr->type != AGENTC_CBOR_ARRAY || i >= arr->n)
        return NULL;
    return ((AgcCbor **)arr->ptr)[i];
}

AgcCbor *agentc_cbor_key(const AgcCbor *map, size_t i) {
    if (!map || map->type != AGENTC_CBOR_MAP || i >= map->n)
        return NULL;
    return ((AgcCbor **)map->ptr)[2 * i];
}

AgcCbor *agentc_cbor_val(const AgcCbor *map, size_t i) {
    if (!map || map->type != AGENTC_CBOR_MAP || i >= map->n)
        return NULL;
    return ((AgcCbor **)map->ptr)[2 * i + 1];
}

AgcCbor *agentc_cbor_get(const AgcCbor *map, const char *key) {
    if (!map || map->type != AGENTC_CBOR_MAP || !key)
        return NULL;
    size_t klen = agentc_strlen(key);
    AgcCbor **pairs = map->ptr;
    for (size_t i = 0; i < map->n; i++) {
        AgcCbor *k = pairs[2 * i];
        if (k->type == AGENTC_CBOR_TEXT && k->n == klen && agentc_memeq(k->ptr, key, klen))
            return pairs[2 * i + 1];
    }
    return NULL;
}

/* ------------------------------------------------------------------ UTF-8 */
bool agentc_cbor_utf8_valid(const u8 *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        u8 c = p[i];
        if (c < 0x80u) {
            i++;
            continue;
        }
        if (c < 0xc2u || c > 0xf4u)
            return false; /* continuation / overlong lead */
        if ((c & 0xe0u) == 0xc0u) {
            if (i + 1 >= n || (p[i + 1] & 0xc0u) != 0x80u)
                return false;
            i += 2;
        } else if ((c & 0xf0u) == 0xe0u) {
            if (i + 2 >= n || (p[i + 1] & 0xc0u) != 0x80u || (p[i + 2] & 0xc0u) != 0x80u)
                return false;
            u32 cp =
                ((u32)(c & 0x0fu) << 12) | ((u32)(p[i + 1] & 0x3fu) << 6) | (u32)(p[i + 2] & 0x3fu);
            if (cp < 0x800u || (cp >= 0xd800u && cp <= 0xdfffu))
                return false;
            i += 3;
        } else {
            if (i + 3 >= n || (p[i + 1] & 0xc0u) != 0x80u || (p[i + 2] & 0xc0u) != 0x80u ||
                (p[i + 3] & 0xc0u) != 0x80u)
                return false;
            u32 cp = ((u32)(c & 0x07u) << 18) | ((u32)(p[i + 1] & 0x3fu) << 12) |
                     ((u32)(p[i + 2] & 0x3fu) << 6) | (u32)(p[i + 3] & 0x3fu);
            if (cp < 0x10000u || cp > 0x10ffffu)
                return false;
            i += 4;
        }
    }
    return true;
}

/* ----------------------------------------------------------------- decoder */
typedef struct {
    const u8 *p, *end;
    AgcCborArena *a;
    size_t depth, items, max_depth, max_items;
    bool full_int;
    int error;
} CborDec;

static AgcCbor *cbor_value(CborDec *d);

static AgcCbor *cfail(CborDec *d, int code) {
    if (d->error == 0)
        d->error = code;
    return NULL;
}

/* Read the argument for an additional-info byte 0..27. Returns 0 on success,
 * a negative errno otherwise. ai 28..30 is malformed, ai 31 is indefinite. */
static int cbor_arg(CborDec *d, u8 ai, u64 *out) {
    if (ai < 24) {
        *out = ai;
        return 0;
    }
    size_t need;
    switch (ai) {
    case 24:
        need = 1;
        break;
    case 25:
        need = 2;
        break;
    case 26:
        need = 4;
        break;
    case 27:
        need = 8;
        break;
    case 31:
        return CBOR_ENOTSUP;
    default:
        return CBOR_EINVAL;
    }
    if ((size_t)(d->end - d->p) < need)
        return CBOR_EINVAL;
    u64 v = 0;
    for (size_t i = 0; i < need; i++)
        v = (v << 8) | d->p[i];
    d->p += need;
    *out = v;
    return 0;
}

static AgcCbor *cbor_scalar_u64(CborDec *d, u8 major, u64 v) {
    if (!d->full_int) {
        if (major == 0 && v > AGENTC_CBOR_MAX_SAFE)
            return cfail(d, CBOR_ERANGE);
        if (major == 1 && v > AGENTC_CBOR_MAX_SAFE - 1)
            return cfail(d, CBOR_ERANGE);
    }
    AgcCbor *n = cnew(d->a, major == 0 ? AGENTC_CBOR_UINT : AGENTC_CBOR_NEG, 0);
    n->u = v;
    return n;
}

static AgcCbor *cbor_bytes(CborDec *d, bool text, u64 len) {
    if (len > (u64)(d->end - d->p))
        return cfail(d, CBOR_EINVAL);
    const u8 *src = d->p;
    if (text && !agentc_cbor_utf8_valid(src, (size_t)len))
        return cfail(d, CBOR_EPROTO);
    AgcCbor *n = cnew(d->a, text ? AGENTC_CBOR_TEXT : AGENTC_CBOR_BYTES, (size_t)len);
    /* Always allocate, even for len 0: an empty text/bytes still owns a NUL
     * byte so agentc_cbor_text()/agentc_cbor_bytes() never return NULL for a
     * valid value (the JSON bridge writes "" from it) and the documented
     * NUL-terminated contract holds. The arena rounds the 1-byte payload to 8. */
    u8 *copy = cbor_arena_alloc(d->a, (size_t)len + 1);
    if (len)
        agentc_memcpy(copy, src, (size_t)len);
    copy[len] = 0;
    n->ptr = copy;
    d->p += len;
    return n;
}

/* Reject duplicate text keys. Open-addressing set; 0 = empty.
 * Returns 0 or a negative errno. */
static int cbor_map_dup(CborDec *d, AgcCbor **pairs, size_t n) {
    if (n < 2)
        return 0;
    size_t slots = 8;
    while (slots < n * 2)
        slots <<= 1;
    size_t *tab = cbor_arena_alloc(d->a, slots * sizeof *tab);
    for (size_t i = 0; i < n; i++) {
        AgcCbor *k = pairs[2 * i];
        const u8 *kb = k->ptr;
        size_t kn = k->n;
        u64 h = 1469598103934665603ull;
        for (size_t j = 0; j < kn; j++) {
            h ^= kb[j];
            h *= 1099511628211ull;
        }
        size_t idx = (size_t)h & (slots - 1);
        while (tab[idx]) {
            AgcCbor *prev = pairs[2 * (tab[idx] - 1)];
            if (prev->n == kn && agentc_memeq(prev->ptr, kb, kn))
                return CBOR_EPROTO;
            idx = (idx + 1) & (slots - 1);
        }
        tab[idx] = i + 1;
    }
    return 0;
}

static AgcCbor *cbor_array(CborDec *d, u64 n) {
    /* cbor_value() already counted this container; reserve only its declared
     * children so a huge declared count fails before the slot allocation, and
     * the effective cap stays the documented AGENTC_CBOR_MAX_ITEMS. */
    if (d->items > d->max_items || n > d->max_items - d->items)
        return cfail(d, CBOR_EMSGSIZE);
    AgcCbor *v = cnew(d->a, AGENTC_CBOR_ARRAY, (size_t)n);
    AgcCbor **arr = n ? cbor_arena_alloc(d->a, (size_t)n * sizeof *arr) : NULL;
    for (u64 i = 0; i < n; i++) {
        if (++d->depth > d->max_depth)
            return cfail(d, CBOR_EMSGSIZE);
        AgcCbor *item = cbor_value(d);
        d->depth--;
        if (!item)
            return NULL;
        arr[i] = item;
    }
    v->ptr = arr;
    return v;
}

static AgcCbor *cbor_map(CborDec *d, u64 n) {
    /* As cbor_array(): each map entry is two values (key + value), both counted
     * by cbor_value() as they decode. Division avoids overflow on a hostile
     * 64-bit declared count. */
    if (d->items > d->max_items || n > (d->max_items - d->items) / 2)
        return cfail(d, CBOR_EMSGSIZE);
    AgcCbor *v = cnew(d->a, AGENTC_CBOR_MAP, (size_t)n);
    AgcCbor **pairs = n ? cbor_arena_alloc(d->a, (size_t)n * 2 * sizeof *pairs) : NULL;
    for (u64 i = 0; i < n; i++) {
        if (++d->depth > d->max_depth)
            return cfail(d, CBOR_EMSGSIZE);
        AgcCbor *k = cbor_value(d);
        d->depth--;
        if (!k)
            return NULL;
        if (k->type != AGENTC_CBOR_TEXT)
            return cfail(d, CBOR_EPROTO);
        if (++d->depth > d->max_depth)
            return cfail(d, CBOR_EMSGSIZE);
        AgcCbor *val = cbor_value(d);
        d->depth--;
        if (!val)
            return NULL;
        pairs[2 * i] = k;
        pairs[2 * i + 1] = val;
    }
    v->ptr = pairs;
    int de = cbor_map_dup(d, pairs, (size_t)n);
    if (de)
        return cfail(d, de);
    return v;
}

static AgcCbor *cbor_value(CborDec *d) {
    if (d->p >= d->end)
        return cfail(d, CBOR_EINVAL);
    /* One item per decoded value, counted here and nowhere else: containers no
     * longer pre-charge their children, so AGENTC_CBOR_MAX_ITEMS bounds the real
     * node count instead of roughly half of it. */
    if (d->items >= d->max_items)
        return cfail(d, CBOR_EMSGSIZE);
    d->items++;
    u8 ib = *d->p++;
    u8 major = (u8)(ib >> 5);
    u8 ai = (u8)(ib & 0x1fu);

    if (major == 7) {
        if (ai == 20) {
            AgcCbor *n = cnew(d->a, AGENTC_CBOR_BOOL, 0);
            n->bval = 0;
            return n;
        }
        if (ai == 21) {
            AgcCbor *n = cnew(d->a, AGENTC_CBOR_BOOL, 0);
            n->bval = 1;
            return n;
        }
        if (ai == 22) {
            return cnew(d->a, AGENTC_CBOR_NULL, 0);
        }
        if (ai == 25 || ai == 26)
            return cfail(d, CBOR_ENOTSUP); /* f16/f32 */
        if (ai == 27) {
            if ((size_t)(d->end - d->p) < 8)
                return cfail(d, CBOR_EINVAL);
            u64 bits = 0;
            for (int i = 0; i < 8; i++)
                bits = (bits << 8) | d->p[i];
            d->p += 8;
            if ((bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull)
                return cfail(d, CBOR_EPROTO); /* NaN or Inf */
            AgcCbor *n = cnew(d->a, AGENTC_CBOR_F64, 0);
            n->u = bits;
            return n;
        }
        return cfail(d, CBOR_ENOTSUP); /* tags/simple/undefined */
    }
    if (major == 6)
        return cfail(d, CBOR_ENOTSUP); /* tags not supported */

    u64 arg = 0;
    int e = cbor_arg(d, ai, &arg);
    if (e)
        return cfail(d, e);
    switch (major) {
    case 0:
    case 1:
        return cbor_scalar_u64(d, major, arg);
    case 2:
        return cbor_bytes(d, false, arg);
    case 3:
        return cbor_bytes(d, true, arg);
    case 4:
        return cbor_array(d, arg);
    case 5:
        return cbor_map(d, arg);
    default:
        return cfail(d, CBOR_EINVAL);
    }
}

AgcCbor *agentc_cbor_decode(AgcCborArena *a, const void *p, size_t n, const AgcCborDecOpts *opts,
                            int *err) {
    if (err)
        *err = 0;
    if (!a || (n != 0 && !p)) {
        if (err)
            *err = CBOR_EINVAL;
        return NULL;
    }
    if (n > AGENTC_CBOR_MAX_BYTES) {
        if (err)
            *err = CBOR_EMSGSIZE;
        return NULL;
    }
    agentc_cbor_arena_clear(a);
    CborDec d;
    agentc_memset(&d, 0, sizeof d);
    d.p = p;
    d.end = (const u8 *)p + n;
    d.a = a;
    d.full_int = opts ? opts->allow_full_int : false;
    d.max_depth = (opts && opts->max_depth) ? opts->max_depth : AGENTC_CBOR_MAX_DEPTH;
    d.max_items = (opts && opts->max_items) ? opts->max_items : AGENTC_CBOR_MAX_ITEMS;
    AgcCbor *v = cbor_value(&d);
    if (!v) {
        if (err)
            *err = d.error ? d.error : CBOR_EINVAL;
        agentc_cbor_arena_clear(a);
        return NULL;
    }
    /* The contract is one value covering the whole buffer: any trailing byte
     * is a protocol error, not a second value to be ignored. */
    if (d.p != d.end) {
        if (err)
            *err = CBOR_EPROTO;
        agentc_cbor_arena_clear(a);
        return NULL;
    }
    return v;
}

/* -------------------------------------------------------------- JSON check */
bool agentc_cbor_is_json(const AgcCbor *v) {
    if (!v)
        return false;
    switch (v->type) {
    case AGENTC_CBOR_UINT:
        return v->u <= AGENTC_CBOR_MAX_SAFE;
    case AGENTC_CBOR_NEG:
        return v->u <= AGENTC_CBOR_MAX_SAFE - 1;
    case AGENTC_CBOR_TEXT:
        return true;
    case AGENTC_CBOR_BOOL:
    case AGENTC_CBOR_NULL:
        return true;
    case AGENTC_CBOR_ARRAY:
        for (size_t i = 0; i < v->n; i++)
            if (!agentc_cbor_is_json(agentc_cbor_at(v, i)))
                return false;
        return true;
    case AGENTC_CBOR_MAP:
        for (size_t i = 0; i < v->n; i++) {
            AgcCbor *k = agentc_cbor_key(v, i);
            if (!k || k->type != AGENTC_CBOR_TEXT)
                return false;
            if (!agentc_cbor_is_json(agentc_cbor_val(v, i)))
                return false;
        }
        return true;
    default:
        return false; /* bytes and f64 are not JSON */
    }
}

/* ----------------------------------------------------------------- builder */
AgcCbor *agentc_cbor_new_uint(AgcCborArena *a, u64 v) {
    AgcCbor *n = cnew(a, AGENTC_CBOR_UINT, 0);
    n->u = v;
    return n;
}

AgcCbor *agentc_cbor_new_int(AgcCborArena *a, i64 v) {
    if (v >= 0)
        return agentc_cbor_new_uint(a, (u64)v);
    AgcCbor *n = cnew(a, AGENTC_CBOR_NEG, 0);
    n->u = (u64)(-(v + 1));
    return n;
}

AgcCbor *agentc_cbor_new_bool(AgcCborArena *a, bool v) {
    AgcCbor *n = cnew(a, AGENTC_CBOR_BOOL, 0);
    n->bval = v ? 1 : 0;
    return n;
}

AgcCbor *agentc_cbor_new_null(AgcCborArena *a) {
    return cnew(a, AGENTC_CBOR_NULL, 0);
}

AgcCbor *agentc_cbor_new_f64(AgcCborArena *a, double v) {
    AgcCbor *n = cnew(a, AGENTC_CBOR_F64, 0);
    agentc_memcpy(&n->u, &v, sizeof n->u);
    return n;
}

AgcCbor *agentc_cbor_new_text(AgcCborArena *a, const char *s, size_t n) {
    AgcCbor *v = cnew(a, AGENTC_CBOR_TEXT, n);
    /* Unconditional allocation: an empty text still owns a NUL byte, so
     * agentc_cbor_text() is NUL-terminated for every n (see include/cbor.h). */
    u8 *copy = cbor_arena_alloc(a, n + 1);
    if (n)
        agentc_memcpy(copy, s, n);
    copy[n] = 0;
    v->ptr = copy;
    return v;
}

AgcCbor *agentc_cbor_new_bytes(AgcCborArena *a, const void *p, size_t n) {
    AgcCbor *v = cnew(a, AGENTC_CBOR_BYTES, n);
    u8 *copy = cbor_arena_alloc(a, n + 1);
    if (n)
        agentc_memcpy(copy, p, n);
    copy[n] = 0;
    v->ptr = copy;
    return v;
}

AgcCbor *agentc_cbor_new_array(AgcCborArena *a, size_t n) {
    AgcCbor *v = cnew(a, AGENTC_CBOR_ARRAY, n);
    v->ptr = n ? cbor_arena_alloc(a, n * sizeof(AgcCbor *)) : NULL;
    return v;
}

AgcCbor *agentc_cbor_new_map(AgcCborArena *a, size_t n) {
    AgcCbor *v = cnew(a, AGENTC_CBOR_MAP, n);
    v->ptr = n ? cbor_arena_alloc(a, n * 2 * sizeof(AgcCbor *)) : NULL;
    return v;
}

void agentc_cbor_array_set(AgcCbor *arr, size_t i, AgcCbor *v) {
    if (!arr || arr->type != AGENTC_CBOR_ARRAY || i >= arr->n)
        return;
    ((AgcCbor **)arr->ptr)[i] = v;
}

void agentc_cbor_map_set(AgcCbor *map, size_t i, AgcCbor *key, AgcCbor *val) {
    if (!map || map->type != AGENTC_CBOR_MAP || i >= map->n)
        return;
    ((AgcCbor **)map->ptr)[2 * i] = key;
    ((AgcCbor **)map->ptr)[2 * i + 1] = val;
}

/* ------------------------------------------------------------------ writer */
#define CBOR_NO_FRAME ((size_t)-1)

static void cborw_fail(AgcCborW *w, int err) {
    if (!w->failed) {
        w->failed = true;
        w->error = err;
    }
}

static void cborw_raw(AgcCborW *w, const void *p, size_t n) {
    if (w->failed || w->b == NULL)
        return;
    agentc_buf_push(w->b, p, n);
}

static void cborw_byte(AgcCborW *w, u8 c) {
    if (w->failed || w->b == NULL)
        return;
    agentc_buf_byte(w->b, c);
}

void agentc_cborw_init(AgcCborW *w, AgcBuf *b) {
    if (!w)
        return;
    w->b = b;
    w->frame_at = CBOR_NO_FRAME;
    w->failed = false;
    w->error = 0;
}

int agentc_cborw_framed_init(AgcCborW *w, AgcBuf *b) {
    if (!w || !b)
        return CBOR_EINVAL;
    agentc_cborw_init(w, b);
    w->frame_at = b->len;
    u8 z[4] = {0, 0, 0, 0};
    agentc_buf_push(b, z, sizeof z);
    return 0;
}

int agentc_cborw_finish(AgcCborW *w) {
    if (!w)
        return CBOR_EINVAL;
    if (w->failed)
        return w->error;
    if (w->frame_at == CBOR_NO_FRAME)
        return 0;
    size_t n = w->b->len - w->frame_at - 4;
    if (n > AGENTC_FRAME_MAX_BYTES) {
        cborw_fail(w, CBOR_EMSGSIZE);
        return w->error;
    }
    u8 *p = w->b->p + w->frame_at;
    p[0] = (u8)((n >> 24) & 0xffu);
    p[1] = (u8)((n >> 16) & 0xffu);
    p[2] = (u8)((n >> 8) & 0xffu);
    p[3] = (u8)(n & 0xffu);
    w->frame_at = CBOR_NO_FRAME;
    return 0;
}

/* Minimal-width argument for major `m` (0..7). */
static void cborw_head(AgcCborW *w, u8 m, u64 v) {
    u8 mb = (u8)(m << 5);
    if (v < 24) {
        cborw_byte(w, (u8)(mb | v));
    } else if (v <= 0xffu) {
        cborw_byte(w, (u8)(mb | 24));
        cborw_byte(w, (u8)v);
    } else if (v <= 0xffffu) {
        cborw_byte(w, (u8)(mb | 25));
        cborw_byte(w, (u8)(v >> 8));
        cborw_byte(w, (u8)v);
    } else if (v <= 0xffffffffu) {
        cborw_byte(w, (u8)(mb | 26));
        cborw_byte(w, (u8)(v >> 24));
        cborw_byte(w, (u8)(v >> 16));
        cborw_byte(w, (u8)(v >> 8));
        cborw_byte(w, (u8)v);
    } else {
        cborw_byte(w, (u8)(mb | 27));
        for (int i = 56; i >= 0; i -= 8)
            cborw_byte(w, (u8)(v >> i));
    }
}

void agentc_cborw_uint(AgcCborW *w, u64 v) {
    cborw_head(w, 0, v);
}

void agentc_cborw_int(AgcCborW *w, i64 v) {
    if (v >= 0)
        cborw_head(w, 0, (u64)v);
    else
        cborw_head(w, 1, (u64)(-(v + 1)));
}

void agentc_cborw_bool(AgcCborW *w, bool v) {
    cborw_byte(w, v ? 0xf5 : 0xf4);
}
void agentc_cborw_null(AgcCborW *w) {
    cborw_byte(w, 0xf6);
}

void agentc_cborw_f64(AgcCborW *w, double v) {
    u64 bits;
    agentc_memcpy(&bits, &v, sizeof bits);
    if ((bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull) {
        cborw_fail(w, CBOR_EPROTO);
        return;
    }
    cborw_byte(w, 0xfb);
    for (int i = 56; i >= 0; i -= 8)
        cborw_byte(w, (u8)(bits >> i));
}

void agentc_cborw_text(AgcCborW *w, const char *s, size_t n) {
    if (n != 0 && s == NULL) {
        cborw_fail(w, CBOR_EINVAL);
        return;
    }
    if (!agentc_cbor_utf8_valid((const u8 *)s, n)) {
        cborw_fail(w, CBOR_EPROTO);
        return;
    }
    cborw_head(w, 3, n);
    cborw_raw(w, s, n);
}

void agentc_cborw_bytes(AgcCborW *w, const void *p, size_t n) {
    if (n != 0 && p == NULL) {
        cborw_fail(w, CBOR_EINVAL);
        return;
    }
    cborw_head(w, 2, n);
    cborw_raw(w, p, n);
}

void agentc_cborw_array(AgcCborW *w, size_t n) {
    cborw_head(w, 4, n);
}
void agentc_cborw_map(AgcCborW *w, size_t n) {
    cborw_head(w, 5, n);
}

void agentc_cborw_value(AgcCborW *w, const AgcCbor *v) {
    if (!w || w->failed)
        return;
    if (!v) {
        cborw_fail(w, CBOR_EINVAL);
        return;
    }
    switch (v->type) {
    case AGENTC_CBOR_UINT:
        agentc_cborw_uint(w, v->u);
        break;
    case AGENTC_CBOR_NEG:
        cborw_head(w, 1, v->u);
        break;
    case AGENTC_CBOR_BOOL:
        agentc_cborw_bool(w, v->bval != 0);
        break;
    case AGENTC_CBOR_NULL:
        agentc_cborw_null(w);
        break;
    case AGENTC_CBOR_F64: {
        cborw_byte(w, 0xfb);
        for (int i = 56; i >= 0; i -= 8)
            cborw_byte(w, (u8)(v->u >> i));
        break;
    }
    case AGENTC_CBOR_TEXT:
        cborw_head(w, 3, v->n);
        cborw_raw(w, v->ptr, v->n);
        break;
    case AGENTC_CBOR_BYTES:
        cborw_head(w, 2, v->n);
        cborw_raw(w, v->ptr, v->n);
        break;
    case AGENTC_CBOR_ARRAY:
        cborw_head(w, 4, v->n);
        for (size_t i = 0; i < v->n; i++)
            agentc_cborw_value(w, ((AgcCbor **)v->ptr)[i]);
        break;
    case AGENTC_CBOR_MAP:
        cborw_head(w, 5, v->n);
        for (size_t i = 0; i < v->n; i++) {
            agentc_cborw_value(w, ((AgcCbor **)v->ptr)[2 * i]);
            agentc_cborw_value(w, ((AgcCbor **)v->ptr)[2 * i + 1]);
        }
        break;
    default:
        cborw_fail(w, CBOR_EINVAL);
        break;
    }
}
