/* ws.c — WebSocket (RFC 6455) transport shim for the framed protocol (P5 C3).
 *
 * Self-contained: no libc, no mbedTLS. SHA-1 and base64 are implemented here
 * because the freestanding build does not link a general crypto library into
 * the wire layer; the handshake needs only SHA-1(key ++ GUID).
 *
 * See include/ws.h for the policy. The decoder validates the frame header
 * before copying or buffering the declared payload, so a declared length over
 * the cap fails closed immediately. All string scanning is bounded by the
 * caller's `\r\n\r\n` header block / frame lengths; no NUL terminator is
 * assumed.
 */
#include "ws.h"

#include "plat.h"
#include "wire.h"

/* ------------------------------------------------------------------- SHA-1 */
typedef struct {
    u32 h[5];
    u64 len;
    u8 buf[64];
    size_t n;
} Sha1;

static u32 rol32(u32 v, int b) {
    return (v << b) | (v >> (32 - b));
}

static void sha1_block(Sha1 *s, const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) | ((u32)p[i * 4 + 2] << 8) |
               (u32)p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        u32 t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

static void sha1_update(Sha1 *s, const u8 *p, size_t n) {
    s->len += n;
    while (n) {
        size_t take = 64 - s->n;
        if (take > n)
            take = n;
        agentc_memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        n -= take;
        if (s->n == 64) {
            sha1_block(s, s->buf);
            s->n = 0;
        }
    }
}

void agentc_ws_sha1(const void *data, size_t n, u8 out[20]) {
    Sha1 s = {{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u}, 0, {0}, 0};
    sha1_update(&s, data, n);
    u64 bits = s.len * 8;
    u8 pad = 0x80;
    sha1_update(&s, &pad, 1);
    u8 zero = 0;
    while (s.n != 56)
        sha1_update(&s, &zero, 1);
    u8 lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (u8)(bits >> (56 - 8 * i));
    sha1_update(&s, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (u8)(s.h[i] >> 24);
        out[i * 4 + 1] = (u8)(s.h[i] >> 16);
        out[i * 4 + 2] = (u8)(s.h[i] >> 8);
        out[i * 4 + 3] = (u8)s.h[i];
    }
}

/* ----------------------------------------------------------------- base64 */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t agentc_ws_base64(const void *data, size_t n, char *out, size_t cap) {
    const u8 *p = data;
    size_t need = ((n + 2) / 3) * 4;
    if (need + 1 > cap)
        return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        u32 v = (u32)p[i] << 16;
        if (i + 1 < n)
            v |= (u32)p[i + 1] << 8;
        if (i + 2 < n)
            v |= (u32)p[i + 2];
        out[o++] = B64[(v >> 18) & 0x3f];
        out[o++] = B64[(v >> 12) & 0x3f];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 0x3f] : '=';
        out[o++] = i + 2 < n ? B64[v & 0x3f] : '=';
    }
    out[o] = 0;
    return o;
}

int agentc_ws_accept(const char *key, char *out, size_t cap) {
    if (!key || !out)
        return -22;
    AgcBuf b = {0};
    agentc_buf_cstr(&b, key);
    agentc_buf_cstr(&b, AGENTC_WS_GUID);
    u8 digest[20];
    agentc_ws_sha1(b.p, b.len, digest);
    agentc_buf_free(&b);
    return agentc_ws_base64(digest, sizeof digest, out, cap) ? 0 : -34;
}

/* -------------------------------------------------------- header parsing */
static bool token_has(const char *val, size_t n, const char *tok) {
    size_t tl = agentc_strlen(tok);
    size_t i = 0;
    while (i < n) {
        while (i < n && (val[i] == ' ' || val[i] == '\t' || val[i] == ','))
            i++;
        size_t start = i;
        while (i < n && val[i] != ',')
            i++;
        size_t end = i;
        while (end > start && (val[end - 1] == ' ' || val[end - 1] == '\t'))
            end--;
        if (end - start == tl && agentc_str_ieq(val + start, end - start, tok, tl))
            return true;
    }
    return false;
}

/* Offset just past the "\r\n\r\n" header terminator, or (size_t)-1 when it has
 * not arrived. Shared by the policy helpers below. */
static size_t ws_head_end(const AgcBuf *in) {
    if (in == NULL || in->p == NULL)
        return (size_t)-1;
    for (size_t i = 0; i + 3 < in->len; i++) {
        if (in->p[i] == '\r' && in->p[i + 1] == '\n' && in->p[i + 2] == '\r' &&
            in->p[i + 3] == '\n')
            return i + 4;
    }
    return (size_t)-1;
}

static void ws_reject(AgcBuf *out, const char *status) {
    agentc_buf_cstr(out, "HTTP/1.1 ");
    agentc_buf_cstr(out, status);
    agentc_buf_cstr(out, "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

int agentc_ws_server_handshake(AgcBuf *in, AgcBuf *out) {
    if (!in || !out)
        return -22;
    /* find the header terminator */
    i64 end = -1;
    for (size_t i = 0; i + 3 < in->len; i++) {
        if (in->p[i] == '\r' && in->p[i + 1] == '\n' && in->p[i + 2] == '\r' &&
            in->p[i + 3] == '\n') {
            end = (i64)i;
            break;
        }
    }
    if (end < 0) {
        if (in->len > AGENTC_WS_MAX_HEADER) {
            ws_reject(out, "431 Request Header Fields Too Large");
            return -90;
        }
        return 0;
    }
    size_t hdr_len = (size_t)end + 4;
    /* The cap must also cover a block that *did* complete in this read: a
     * pipelined `\r\n\r\n` at the end of an oversized request must not let it
     * through the incomplete-prefix check above. */
    if (hdr_len > AGENTC_WS_MAX_HEADER) {
        ws_reject(out, "431 Request Header Fields Too Large");
        return -90;
    }
    const char *p = (const char *)in->p;

    /* request line: GET <path> HTTP/1.1 */
    size_t rl = 0;
    while (rl < hdr_len && p[rl] != '\r')
        rl++;
    bool ok =
        agentc_str_starts(p, rl, "GET ") && rl >= 8 && agentc_str_eq(p + rl - 8, 8, "HTTP/1.1", 8);

    bool up = false, conn = false, ver13 = false, proto = false;
    const char *key = NULL;
    size_t key_len = 0;

    size_t pos = rl;
    if (pos + 2 <= hdr_len && p[pos] == '\r' && p[pos + 1] == '\n')
        pos += 2;
    while (ok && pos < hdr_len) {
        size_t ls = pos;
        while (pos < hdr_len && p[pos] != '\r')
            pos++;
        size_t le = pos;
        if (pos + 1 >= hdr_len || p[pos + 1] != '\n')
            break;
        pos += 2;
        if (le == ls)
            break; /* empty line: end of headers */
        const char *line = p + ls;
        size_t n = le - ls;
        size_t c = 0;
        while (c < n && line[c] != ':')
            c++;
        if (c == n) {
            ok = false;
            break;
        }
        const char *name = line;
        size_t nn = c;
        const char *val = line + c + 1;
        size_t vn = n - c - 1;
        while (vn && (*val == ' ' || *val == '\t')) {
            val++;
            vn--;
        }
        while (vn && (val[vn - 1] == ' ' || val[vn - 1] == '\t'))
            vn--;
        if (nn == 7 && agentc_str_ieq(name, nn, "Upgrade", 7)) {
            up = token_has(val, vn, "websocket");
        } else if (nn == 10 && agentc_str_ieq(name, nn, "Connection", 10)) {
            conn = token_has(val, vn, "upgrade");
        } else if (nn == 17 && agentc_str_ieq(name, nn, "Sec-WebSocket-Key", 17)) {
            key = val;
            key_len = vn;
        } else if (nn == 21 && agentc_str_ieq(name, nn, "Sec-WebSocket-Version", 21)) {
            ver13 = (vn == 2 && val[0] == '1' && val[1] == '3');
        } else if (nn == 22 && agentc_str_ieq(name, nn, "Sec-WebSocket-Protocol", 22)) {
            proto = token_has(val, vn, AGENTC_WS_SUBPROTOCOL);
        }
    }

    ok = ok && up && conn && ver13 && proto && key != NULL && key_len > 0 && key_len <= 64;
    if (!ok) {
        ws_reject(out, "400 Bad Request");
        /* consume the request either way; the connection closes */
        size_t left = in->len - hdr_len;
        agentc_memmove(in->p, in->p + hdr_len, left);
        in->len = left;
        return -71;
    }

    char acc[64];
    if (key_len >= 64) {
        ws_reject(out, "400 Bad Request");
        size_t left = in->len - hdr_len;
        agentc_memmove(in->p, in->p + hdr_len, left);
        in->len = left;
        return -22;
    }
    char keybuf[64];
    agentc_memcpy(keybuf, key, key_len);
    keybuf[key_len] = 0;
    if (agentc_ws_accept(keybuf, acc, sizeof acc) != 0) {
        ws_reject(out, "400 Bad Request");
        size_t left = in->len - hdr_len;
        agentc_memmove(in->p, in->p + hdr_len, left);
        in->len = left;
        return -22;
    }
    agentc_buf_cstr(out, "HTTP/1.1 101 Switching Protocols\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Accept: ");
    agentc_buf_cstr(out, acc);
    agentc_buf_cstr(out, "\r\nSec-WebSocket-Protocol: " AGENTC_WS_SUBPROTOCOL "\r\n\r\n");

    size_t left = in->len - hdr_len;
    agentc_memmove(in->p, in->p + hdr_len, left);
    in->len = left;
    return 1;
}

/* --------------------------------------------- Host/Origin policy (I2) */
int agentc_ws_parse_request(const AgcBuf *in, AgcWsRequest *out) {
    if (in == NULL || out == NULL)
        return -22;
    out->host[0] = 0;
    out->origin[0] = 0;
    out->has_host = false;
    out->has_origin = false;
    size_t hdr_len = ws_head_end(in);
    if (hdr_len == (size_t)-1)
        return 1; /* header block not complete yet */
    const char *p = (const char *)in->p;
    size_t pos = 0;
    while (pos < hdr_len && p[pos] != '\r')
        pos++;
    if (pos + 1 < hdr_len && p[pos] == '\r' && p[pos + 1] == '\n')
        pos += 2;
    while (pos < hdr_len) {
        size_t ls = pos;
        while (pos < hdr_len && p[pos] != '\r')
            pos++;
        size_t le = pos;
        if (pos + 1 >= hdr_len || p[pos + 1] != '\n')
            break;
        pos += 2;
        if (le == ls)
            break; /* empty line: end of headers */
        const char *line = p + ls;
        size_t n = le - ls;
        size_t c = 0;
        while (c < n && line[c] != ':')
            c++;
        if (c == n)
            continue; /* no colon: the base handshake rejects it */
        const char *name = line;
        size_t nn = c;
        const char *val = line + c + 1;
        size_t vn = n - c - 1;
        while (vn && (*val == ' ' || *val == '\t')) {
            val++;
            vn--;
        }
        while (vn && (val[vn - 1] == ' ' || val[vn - 1] == '\t'))
            vn--;
        if (nn == 4 && agentc_str_ieq(name, nn, "Host", 4)) {
            if (vn >= sizeof out->host)
                return -90;
            agentc_memcpy(out->host, val, vn);
            out->host[vn] = 0;
            out->has_host = true;
        } else if (nn == 6 && agentc_str_ieq(name, nn, "Origin", 6)) {
            if (vn >= sizeof out->origin)
                return -90;
            agentc_memcpy(out->origin, val, vn);
            out->origin[vn] = 0;
            out->has_origin = true;
        }
    }
    return 0;
}

int agentc_ws_server_handshake_with(AgcBuf *in, AgcBuf *out, AgcWsAllowFn allow, void *ud) {
    if (in == NULL || out == NULL)
        return -22;
    if (allow != NULL) {
        AgcWsRequest req;
        int pr = agentc_ws_parse_request(in, &req);
        if (pr == 1) {
            /* F1: mirror the base handshake's cap on the incomplete path. A
             * block already past the cap can never become a valid request, so
             * rejecting now keeps the pre-token surface bounded instead of
             * buffering an unterminated header forever. */
            if (in->len > AGENTC_WS_MAX_HEADER) {
                ws_reject(out, "431 Request Header Fields Too Large");
                return -90;
            }
            return 0; /* incomplete: emit nothing and wait for more bytes */
        }
        if (pr != 0) {
            ws_reject(out, "400 Bad Request");
            size_t hl = ws_head_end(in);
            if (hl != (size_t)-1) {
                size_t left = in->len - hl;
                agentc_memmove(in->p, in->p + hl, left);
                in->len = left;
            }
            return pr;
        }
        if (!allow(ud, &req)) {
            ws_reject(out, "403 Forbidden");
            size_t hl = ws_head_end(in);
            size_t left = in->len - hl;
            agentc_memmove(in->p, in->p + hl, left);
            in->len = left;
            return -13; /* EACCES */
        }
    }
    return agentc_ws_server_handshake(in, out);
}

/* --------------------------------------------------------------- decoder */
void agentc_ws_decoder_init(AgcWsDecoder *d) {
    agentc_memset(d, 0, sizeof *d);
}

void agentc_ws_decoder_free(AgcWsDecoder *d) {
    if (!d)
        return;
    agentc_buf_free(&d->buf);
    agentc_buf_free(&d->msg);
    agentc_memset(d, 0, sizeof *d);
}

static void ws_fail(AgcWsDecoder *d, int err) {
    d->dead = true;
    d->error = err;
}

int agentc_ws_decoder_feed(AgcWsDecoder *d, const void *in, size_t n, AgcWsFeedFn cb, void *ud) {
    if (!d)
        return -22;
    if (in == NULL && n != 0)
        return -22;
    if (d->dead)
        return d->error;
    if (n)
        agentc_buf_push(&d->buf, in, n);
    size_t off = 0;
    int rc = 0;
    while (off + 2 <= d->buf.len) {
        const u8 *b = d->buf.p + off;
        size_t avail = d->buf.len - off;
        u8 b0 = b[0], b1 = b[1];
        bool fin = (b0 >> 7) & 1;
        int rsv = (b0 >> 4) & 7;
        int opcode = b0 & 0xf;
        bool mask = (b1 >> 7) & 1;
        u64 len = b1 & 0x7f;
        size_t hdr = 2;
        if (rsv != 0 || !mask) {
            rc = -71;
            break;
        } /* RSV must be 0; client must mask */
        if (len == 126) {
            if (avail < 4)
                break;
            len = ((u64)b[2] << 8) | (u64)b[3];
            hdr = 4;
        } else if (len == 127) {
            if (avail < 10)
                break;
            len = 0;
            for (int i = 0; i < 8; i++)
                len = (len << 8) | b[2 + i];
            hdr = 10;
            if (len >> 63) {
                rc = -71;
                break;
            } /* MSB must be 0 */
        }
        /* validate the declared length before buffering the payload */
        if (len > AGENTC_WS_MAX_MESSAGE) {
            rc = -90;
            break;
        }
        bool control = opcode >= 8;
        if (control && (!fin || len > 125)) {
            rc = -71;
            break;
        }
        hdr += 4; /* masking key */
        if (avail < hdr + (size_t)len)
            break; /* wait for the payload */
        u8 *payload = d->buf.p + off + hdr;
        const u8 *key = d->buf.p + off + hdr - 4;
        for (u64 i = 0; i < len; i++)
            payload[i] ^= key[i & 3];
        int cr = 0;
        if (control) {
            if (opcode == AGENTC_WS_CLOSE) {
                if (cb)
                    cr = cb(ud, AGENTC_WS_CLOSE, payload, (size_t)len);
                off += hdr + (size_t)len;
                rc = cr;
                ws_fail(d, rc);
                d->closed = true;
                break;
            }
            if (opcode == AGENTC_WS_PING) {
                if (cb)
                    cr = cb(ud, AGENTC_WS_PING, payload, (size_t)len);
            } else if (opcode == AGENTC_WS_PONG) {
                if (cb)
                    cr = cb(ud, AGENTC_WS_PONG, payload, (size_t)len);
            } else {
                rc = -71;
                break;
            }
        } else if (opcode == AGENTC_WS_BINARY || opcode == 0) {
            if (opcode == AGENTC_WS_BINARY) {
                if (d->opcode != 0) {
                    rc = -71;
                    break;
                } /* data before finishing */
                if (fin) {
                    if (cb)
                        cr = cb(ud, AGENTC_WS_BINARY, payload, (size_t)len);
                } else {
                    d->opcode = AGENTC_WS_BINARY;
                    d->msg.len = 0;
                    agentc_buf_push(&d->msg, payload, (size_t)len);
                }
            } else { /* continuation */
                if (d->opcode == 0) {
                    rc = -71;
                    break;
                }
                if (d->msg.len > AGENTC_WS_MAX_MESSAGE - (size_t)len) {
                    rc = -90;
                    break;
                }
                agentc_buf_push(&d->msg, payload, (size_t)len);
                if (fin) {
                    if (cb)
                        cr = cb(ud, AGENTC_WS_BINARY, d->msg.p, d->msg.len);
                    d->opcode = 0;
                    d->msg.len = 0;
                }
            }
        } else {
            rc = -95; /* text or reserved data opcode: unsupported */
        }
        off += hdr + (size_t)len;
        if (cr) {
            rc = cr;
            break;
        }
    }
    if (off) {
        size_t left = d->buf.len - off;
        agentc_memmove(d->buf.p, d->buf.p + off, left);
        d->buf.len = left;
        if (d->buf.p && left < d->buf.cap)
            d->buf.p[left] = 0;
    }
    if (rc)
        ws_fail(d, rc);
    return rc;
}

/* --------------------------------------------------------------- encoder */
int agentc_ws_encode_frame(AgcBuf *out, int opcode, const void *payload, size_t len) {
    if (!out)
        return -22;
    if (len > AGENTC_WS_MAX_MESSAGE)
        return -90;
    agentc_buf_byte(out, (u8)(0x80u | (unsigned)(opcode & 0xf)));
    if (len < 126) {
        agentc_buf_byte(out, (u8)len);
    } else if (len <= 0xFFFF) {
        agentc_buf_byte(out, 126);
        agentc_buf_byte(out, (u8)(len >> 8));
        agentc_buf_byte(out, (u8)len);
    } else {
        agentc_buf_byte(out, 127);
        for (int i = 7; i >= 0; i--)
            agentc_buf_byte(out, (u8)((u64)len >> (8 * i)));
    }
    if (len)
        agentc_buf_push(out, payload, len);
    return 0;
}

int agentc_ws_encode(AgcBuf *out, const void *payload, size_t len) {
    return agentc_ws_encode_frame(out, AGENTC_WS_BINARY, payload, len);
}

int agentc_ws_encode_pong(AgcBuf *out, const void *payload, size_t len) {
    if (len > 125)
        return -90;
    return agentc_ws_encode_frame(out, AGENTC_WS_PONG, payload, len);
}

int agentc_ws_encode_close(AgcBuf *out, u16 code) {
    u8 b[2] = {(u8)(code >> 8), (u8)code};
    return agentc_ws_encode_frame(out, AGENTC_WS_CLOSE, b, sizeof b);
}
