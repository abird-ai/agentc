/* cbor.h — the agentc CBOR codec (P5 Axis C, RFC 8949 subset).
 *
 * A small, freestanding CBOR DOM (`AgcCbor`) with a block arena, a strict-UTF-8
 * validator, a definite-length-only decoder and a streaming writer. The full
 * decisions live in .agents/plans/P5-AXES.md §2.3; the important ones:
 *
 *   - definite lengths only: no tags (major 6), no indefinite lengths, no
 *     f16/f32, no simple values beyond false/true/null;
 *   - byte strings decode into the codec but the protocol profile rejects them
 *     (`agentc_cbor_is_json` is false);
 *   - duplicate text keys are rejected at decode;
 *   - f64 is accepted and must be finite; NaN/Inf are rejected;
 *   - integers are limited to the pi-safe IEEE-754 range by default
 *     (|v| <= 2^53-1) unless the caller opts into the full 64-bit range;
 *   - depth 64, 2^20 items, 16 MiB input.
 *
 * The decoder never retains a pointer into the caller's input, and every limit
 * is enforced before the corresponding allocation or copy (fail-closed).
 */
#ifndef AGENTC_CBOR_H
#define AGENTC_CBOR_H

#include "agentc.h"
#include "base/limits.h"

/* ------------------------------------------------------------------ limits */
#define AGENTC_CBOR_MAX_DEPTH 64u
#define AGENTC_CBOR_MAX_ITEMS (1u << 20)
#define AGENTC_CBOR_MAX_BYTES (16u * 1024u * 1024u)
/* Largest integer representable exactly as a JavaScript/pi Number. */
#define AGENTC_CBOR_MAX_SAFE 9007199254740991ull

/* -------------------------------------------------------------------- DOM */
typedef struct AgcCbor AgcCbor;
typedef struct AgcCborArena AgcCborArena;

enum {
    AGENTC_CBOR_UINT = 0,
    AGENTC_CBOR_NEG, /* value = -1 - n */
    AGENTC_CBOR_BYTES,
    AGENTC_CBOR_TEXT,
    AGENTC_CBOR_ARRAY,
    AGENTC_CBOR_MAP,
    AGENTC_CBOR_BOOL,
    AGENTC_CBOR_NULL,
    AGENTC_CBOR_F64,
};

/* Decode options. A zero struct is the strict default profile. */
typedef struct {
    bool allow_full_int; /* permit integers outside the pi-safe range */
    size_t max_depth;    /* 0 -> AGENTC_CBOR_MAX_DEPTH */
    size_t max_items;    /* 0 -> AGENTC_CBOR_MAX_ITEMS */
} AgcCborDecOpts;

AgcCborArena *agentc_cbor_arena_new(size_t initial_bytes); /* NULL on OOM */
void agentc_cbor_arena_free(AgcCborArena *a);
void agentc_cbor_arena_clear(AgcCborArena *a);

/* Decode exactly one definite-length CBOR value from the whole buffer
 * `[p, p+n)`. The value must consume every byte: trailing data after a valid
 * value is a protocol error, so a frame payload carrying a valid value plus
 * garbage is rejected here in one place (fail-closed). On failure returns NULL
 * and stores a negative errno in *err (optional): -EINVAL malformed,
 * -EPROTO invalid UTF-8 / duplicate map key / non-text map key / trailing
 * bytes, -ENOTSUP tags/indefinite/f16/f32/simple, -ERANGE unsafe integer,
 * -EMSGSIZE depth/item/input cap, -ENOMEM arena. */
AgcCbor *agentc_cbor_decode(AgcCborArena *a, const void *p, size_t n, const AgcCborDecOpts *opts,
                            int *err);

/* Strict UTF-8 (RFC 3629): rejects overlong forms, surrogates and > U+10FFFF. */
bool agentc_cbor_utf8_valid(const u8 *p, size_t n);

/* --------------------------------------------------------------- accessors */
int agentc_cbor_type(const AgcCbor *v);   /* AGENTC_CBOR_* or -1 */
size_t agentc_cbor_len(const AgcCbor *v); /* array/map/text/bytes */
u64 agentc_cbor_uint(const AgcCbor *v);   /* UINT value, else 0 */
i64 agentc_cbor_int(const AgcCbor *v);    /* UINT/NEG value */
bool agentc_cbor_bool(const AgcCbor *v);
u64 agentc_cbor_f64_bits(const AgcCbor *v); /* IEEE-754 bits */
const u8 *agentc_cbor_bytes(const AgcCbor *v, size_t *len);
const u8 *agentc_cbor_text(const AgcCbor *v, size_t *len); /* NUL-terminated */
AgcCbor *agentc_cbor_at(const AgcCbor *arr, size_t i);
AgcCbor *agentc_cbor_key(const AgcCbor *map, size_t i);
AgcCbor *agentc_cbor_val(const AgcCbor *map, size_t i);
/* Map lookup by exact text key. Returns NULL when absent or `map` is not a map. */
AgcCbor *agentc_cbor_get(const AgcCbor *map, const char *key);

/* True when `v` uses only the strict-JSON vocabulary: uint/neg integers inside
 * the pi-safe range, valid UTF-8 text, bool, null, arrays and maps with text
 * keys. Bytes and f64 are not JSON. NULL is not JSON. */
bool agentc_cbor_is_json(const AgcCbor *v);

/* ------------------------------------------------------------------ builder */
AgcCbor *agentc_cbor_new_uint(AgcCborArena *a, u64 v);
AgcCbor *agentc_cbor_new_int(AgcCborArena *a, i64 v);
AgcCbor *agentc_cbor_new_bool(AgcCborArena *a, bool v);
AgcCbor *agentc_cbor_new_null(AgcCborArena *a);
AgcCbor *agentc_cbor_new_f64(AgcCborArena *a, double v);
AgcCbor *agentc_cbor_new_text(AgcCborArena *a, const char *s, size_t n);
AgcCbor *agentc_cbor_new_bytes(AgcCborArena *a, const void *p, size_t n);
AgcCbor *agentc_cbor_new_array(AgcCborArena *a, size_t n); /* slots zeroed (NULL) */
AgcCbor *agentc_cbor_new_map(AgcCborArena *a, size_t n);
void agentc_cbor_array_set(AgcCbor *arr, size_t i, AgcCbor *v);
void agentc_cbor_map_set(AgcCbor *map, size_t i, AgcCbor *key, AgcCbor *val);

/* ------------------------------------------------------------------- writer */
typedef struct {
    AgcBuf *b;
    size_t frame_at; /* offset of the frame prefix, (size_t)-1 when absent */
    bool failed;
    int error;
} AgcCborW;

void agentc_cborw_init(AgcCborW *w, AgcBuf *b);
/* Reserve a 4-byte big-endian length prefix at the current position; a matching
 * agentc_cborw_finish() patches it. Nested framing is not supported. */
int agentc_cborw_framed_init(AgcCborW *w, AgcBuf *b);
/* Patch a framed writer's prefix (over the payload written so far). -> 0 |
 * -EMSGSIZE (payload over AGENTC_FRAME_MAX_BYTES) | a latched writer error. */
int agentc_cborw_finish(AgcCborW *w);

void agentc_cborw_uint(AgcCborW *w, u64 v);
void agentc_cborw_int(AgcCborW *w, i64 v);
void agentc_cborw_bool(AgcCborW *w, bool v);
void agentc_cborw_null(AgcCborW *w);
void agentc_cborw_f64(AgcCborW *w, double v);
void agentc_cborw_text(AgcCborW *w, const char *s, size_t n);
void agentc_cborw_bytes(AgcCborW *w, const void *p, size_t n);
void agentc_cborw_array(AgcCborW *w, size_t n);
void agentc_cborw_map(AgcCborW *w, size_t n);
void agentc_cborw_value(AgcCborW *w, const AgcCbor *v); /* re-emit a DOM value */

#endif /* AGENTC_CBOR_H */
