/* voice.h — the framed realtime-voice helper protocol (P6-VOICE §12).
 *
 * The full WebRTC/audio stack is out of scope for the freestanding core, but the
 * framing the helper and the core agree on is core contract: a process actor is
 * spawned with AGENTC_PROC_PIPE_BIDI and every message is a length-prefixed
 * frame. This header is the one place the wire shape lives, so the extension
 * (`extensions/realtime-voice/`) and the core cannot drift.
 *
 * Frame layout (all integers big-endian):
 *
 *     u32 payload_len | u8 type | payload[payload_len]
 *
 * payload_len is the byte length of the payload only (it excludes the 5-byte
 * header). A frame is at most AGENTC_VOICE_FRAME_MAX_BYTES of payload; a larger
 * declared length is rejected before it is buffered, so a hostile or buggy
 * helper cannot grow the decoder without bound (fail-closed). Unknown types are
 * still carried (the payload is media-agnostic); a peer that does not recognize
 * a type may ignore it.
 *
 * The streaming decoder is single-threaded and bounded: it owns one fixed frame
 * buffer and consumes an arbitrary read split across frames and partial frames.
 * `agentc_voice_decoder_feed` never allocates and never retains a pointer into
 * the caller's input past the call; the frame handed to the callback borrows the
 * decoder's buffer and stays valid only until the next feed.
 */
#ifndef AGENTC_VOICE_H
#define AGENTC_VOICE_H

#include "agentc.h"

/* P6-VOICE §12 frame types. The numeric values are the wire contract. */
typedef enum {
    AGENTC_VOICE_CTRL_JSON = 0x01,  /* control JSON (start/stop/mute/...) */
    AGENTC_VOICE_PCM_IN = 0x02,     /* microphone PCM */
    AGENTC_VOICE_PCM_OUT = 0x03,    /* speaker PCM */
    AGENTC_VOICE_TRANSCRIPT = 0x04, /* speech-to-text / text-to-speech text */
    AGENTC_VOICE_ITEM = 0x05,       /* application/x-realtime-item payload */
    AGENTC_VOICE_TOOLCALL = 0x06,   /* a tool call the helper wants run */
    AGENTC_VOICE_TOOLRESULT = 0x07, /* the tool result returned to it */
    AGENTC_VOICE_ERROR = 0x08,      /* helper-side error text */
} AgcVoiceFrameType;

#define AGENTC_VOICE_HEADER_BYTES 5u
/* Per-frame payload cap. One place to read; the decoder rejects a declared
 * length above it before it reads the payload. */
#define AGENTC_VOICE_FRAME_MAX_BYTES (256u * 1024u)
/* Fixed decoder buffer: one maximum frame plus the header, so a frame is never
 * copied twice. */
#define AGENTC_VOICE_DECODER_BYTES (AGENTC_VOICE_HEADER_BYTES + AGENTC_VOICE_FRAME_MAX_BYTES)

/* One decoded frame. `payload` borrows host memory; it is valid only for the
 * duration of the feed callback and must be copied to be retained. */
typedef struct {
    u8 type;
    const u8 *payload;
    size_t len;
} AgcVoiceFrame;

/* Frame a payload into `out` (appended). Returns 0, -EINVAL for a NULL buffer or
 * a NULL non-empty payload, or -EMSGSIZE when `len` exceeds the cap. */
int agentc_voice_frame_encode(u8 type, const void *payload, size_t len, AgcBuf *out);

/* Stable lowercase name for logs and tests; "unknown" for an unmapped type. */
const char *agentc_voice_frame_type_name(u8 type);

/* Streaming decoder. Zero-initialise or call init before the first feed. The
 * struct is large; callers heap-allocate it rather than placing it on a small
 * stack. */
typedef struct {
    u8 buf[AGENTC_VOICE_DECODER_BYTES];
    size_t have;      /* bytes buffered for the current frame */
    size_t frame_len; /* header + declared payload length once known */
    bool header;      /* the 4-byte length prefix is complete */
    bool dead;        /* a protocol error latched; ignore further input */
} AgcVoiceDecoder;

/* Called once per complete frame, in wire order. Return 0 to continue; any
 * nonzero value stops the feed and is returned to the caller. */
typedef int (*AgcVoiceFrameFn)(void *ud, const AgcVoiceFrame *frame);

void agentc_voice_decoder_init(AgcVoiceDecoder *d);

/* Consume `n` bytes, invoking `cb` for each complete frame. Returns 0 when the
 * input is consumed, a negative errno on a protocol error (-EMSGSIZE for a
 * declared payload over the cap), or the callback's nonzero return. On an error
 * the decoder latches `dead` and ignores further input; a partial frame is held
 * for the next call. */
int agentc_voice_decoder_feed(AgcVoiceDecoder *d, const u8 *in, size_t n, AgcVoiceFrameFn cb,
                              void *ud);

/* Bytes buffered for an incomplete frame (0 when at a frame boundary). */
size_t agentc_voice_decoder_pending(const AgcVoiceDecoder *d);

/* --------------------------------------------------------------- helper I/O */
/* Encode a helper CTRL_JSON frame whose payload is `json` and append it to
 * `out`. A convenience over encode for the control channel. */
int agentc_voice_ctrl_frame(const char *json, AgcBuf *out);

#endif /* AGENTC_VOICE_H */
