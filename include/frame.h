/* frame.h — the framed-CBOR transport frame (P5 Axis C, C-D12).
 *
 * Wire shape (one frame):
 *
 *     u32 payload_len (big-endian) | payload[payload_len]
 *
 * payload_len counts the payload only; the 4-byte prefix is not included.
 * A declared length above AGENTC_FRAME_MAX_BYTES is rejected before any of the
 * payload is buffered, so a hostile peer cannot grow the decoder without bound
 * (validate-before-copy, fail-closed). A protocol error latches the decoder
 * into a sticky failure state: every later feed returns the same error and no
 * callback fires. The cap is 16 MiB (C-D12).
 *
 * The streaming decoder coalesces arbitrarily split and coalesced reads. It is
 * single-threaded and allocation-backed (the pending buffer can reach one
 * maximum frame plus a partial frame); no pointer into the caller's input is
 * retained past the call.
 */
#ifndef AGENTC_FRAME_H
#define AGENTC_FRAME_H

#include "agentc.h"

#define AGENTC_FRAME_HEADER_BYTES 4u
/* One frame payload cap (C-D12). One place to read. */
#define AGENTC_FRAME_MAX_BYTES (16u * 1024u * 1024u)

/* One decoded frame. `payload` is borrowed from the decoder's pending buffer
 * and stays valid only for the duration of the feed callback; copy it to
 * retain. */
typedef struct {
    const u8 *payload;
    size_t len;
} AgcFrame;

/* Append `4-byte length | payload` to `out`. -> 0 | -EINVAL |
 * -EMSGSIZE (len over the cap). */
int agentc_frame_encode(AgcBuf *out, const void *payload, size_t len);

typedef struct AgcFrameDecoder {
    AgcBuf buf; /* pending header+payload bytes (compacted) */
    bool dead;  /* a protocol error latched */
    int error;  /* the latched negative errno */
} AgcFrameDecoder;

/* The decoded 32-bit big-endian length prefix of the next frame, or 0 when a
 * full header has not arrived. Exposed for tests. */
u32 agentc_frame_peek_len(const AgcFrameDecoder *d);

void agentc_frame_decoder_init(AgcFrameDecoder *d);
void agentc_frame_decoder_free(AgcFrameDecoder *d);

/* Consume `n` bytes, invoking `cb` once per complete frame in wire order.
 * Returns 0 when the input is consumed, the callback's nonzero return (the
 * frame is consumed first; any following bytes stay buffered for the next feed,
 * so a caller may stop mid-stream and resume), or a negative errno on a protocol
 * error (-EMSGSIZE for a declared payload over the cap). A protocol error is
 * sticky: every later feed returns the same error and no callback fires. */
int agentc_frame_decoder_feed(AgcFrameDecoder *d, const void *in, size_t n,
                              int (*cb)(void *ud, const AgcFrame *frame), void *ud);

/* Bytes buffered for an incomplete frame (0 at a frame boundary). */
size_t agentc_frame_decoder_pending(const AgcFrameDecoder *d);

#endif /* AGENTC_FRAME_H */
