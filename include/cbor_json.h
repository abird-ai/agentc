/* cbor_json.h — JSON text <-> AgcCbor bridge for the framed protocol.
 *
 * The dispatcher is AgcJson-based; the framed protocol speaks strict-JSON-
 * constrained CBOR (C-D1). This bridge converts between the two in the one
 * vocabulary the agentc JSON layer supports: integers, strings, booleans,
 * null, arrays and objects. It is integer-only on the wire path: a JSON float
 * and a CBOR f64 are rejected (-ENOTSUP), documented as a pi divergence.
 */
#ifndef AGENTC_CBOR_JSON_H
#define AGENTC_CBOR_JSON_H

#include "cbor.h"

/* Parse strict JSON into a CBOR value owned by `a`. -> value | NULL.
 * *err (optional): -EINVAL malformed JSON, -EPROTO invalid UTF-8 / duplicate
 * key / non-text key, -ENOTSUP a float/exponent number or a f64 value. */
AgcCbor *agentc_cbor_from_json(AgcCborArena *a, const char *p, size_t n, int *err);

/* Emit `v` as compact JSON text appended to `out`. -> 0 | -EINVAL |
 * -ENOTSUP (bytes/f64 or a non-JSON value). */
int agentc_cbor_to_json(const AgcCbor *v, AgcBuf *out);

#endif /* AGENTC_CBOR_JSON_H */
