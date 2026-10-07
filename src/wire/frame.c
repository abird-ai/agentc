/* frame.c — the framed-CBOR transport frame (P5 Axis C, C-D12).
 *
 * See include/frame.h for the wire contract. The decoder buffers only the
 * bytes it needs, validates the declared length before copying a payload and
 * latches a protocol error (sticky failure). No libc; single-threaded.
 */
#include "frame.h"

#define FRAME_EINVAL (-22)
#define FRAME_EMSGSIZE (-90)

static u32 frame_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

int agentc_frame_encode(AgcBuf *out, const void *payload, size_t len) {
    if (out == NULL)
        return FRAME_EINVAL;
    if (len != 0 && payload == NULL)
        return FRAME_EINVAL;
    if (len > AGENTC_FRAME_MAX_BYTES)
        return FRAME_EMSGSIZE;
    u8 hdr[AGENTC_FRAME_HEADER_BYTES];
    hdr[0] = (u8)((len >> 24) & 0xffu);
    hdr[1] = (u8)((len >> 16) & 0xffu);
    hdr[2] = (u8)((len >> 8) & 0xffu);
    hdr[3] = (u8)(len & 0xffu);
    agentc_buf_push(out, hdr, sizeof hdr);
    if (len != 0)
        agentc_buf_push(out, payload, len);
    return 0;
}

void agentc_frame_decoder_init(AgcFrameDecoder *d) {
    if (d == NULL)
        return;
    agentc_memset(d, 0, sizeof *d);
}

void agentc_frame_decoder_free(AgcFrameDecoder *d) {
    if (d == NULL)
        return;
    agentc_buf_free(&d->buf);
    d->dead = false;
    d->error = 0;
}

u32 agentc_frame_peek_len(const AgcFrameDecoder *d) {
    if (d == NULL || d->buf.len < AGENTC_FRAME_HEADER_BYTES)
        return 0;
    return frame_be32(d->buf.p);
}

/* Compact consumed bytes (off) to the front of the pending buffer. */
static void frame_compact(AgcFrameDecoder *d, size_t off) {
    if (off == 0)
        return;
    if (off >= d->buf.len) {
        agentc_buf_clear(&d->buf);
        return;
    }
    size_t left = d->buf.len - off;
    agentc_memmove(d->buf.p, d->buf.p + off, left);
    d->buf.len = left;
    d->buf.p[left] = 0;
}

int agentc_frame_decoder_feed(AgcFrameDecoder *d, const void *in, size_t n,
                              int (*cb)(void *ud, const AgcFrame *frame), void *ud) {
    if (d == NULL)
        return FRAME_EINVAL;
    if (d->dead)
        return d->error ? d->error : FRAME_EINVAL;
    if (n != 0 && in == NULL) {
        d->dead = true;
        d->error = FRAME_EINVAL;
        return d->error;
    }
    if (n != 0)
        agentc_buf_push(&d->buf, in, n);

    size_t off = 0;
    int stopped = 0;
    while (!d->dead) {
        if (d->buf.len - off < AGENTC_FRAME_HEADER_BYTES)
            break;
        u32 len = frame_be32(d->buf.p + off);
        if (len > AGENTC_FRAME_MAX_BYTES) {
            d->dead = true;
            d->error = FRAME_EMSGSIZE;
            break;
        }
        if (d->buf.len - off - AGENTC_FRAME_HEADER_BYTES < len)
            break;
        AgcFrame f;
        f.payload = d->buf.p + off + AGENTC_FRAME_HEADER_BYTES;
        f.len = len;
        stopped = cb ? cb(ud, &f) : 0;
        off += AGENTC_FRAME_HEADER_BYTES + len;
        if (stopped)
            break;
    }
    frame_compact(d, off);
    if (d->dead)
        return d->error;
    return stopped;
}

size_t agentc_frame_decoder_pending(const AgcFrameDecoder *d) {
    return d ? d->buf.len : 0;
}
