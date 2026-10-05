/* ws.h — WebSocket (RFC 6455) transport shim for the framed protocol (P5 C3).
 *
 * `agentc serve --ws` upgrades the TCP listener to a WebSocket endpoint: the
 * client sends an HTTP/1.1 Upgrade request with the `agentc.v1` subprotocol,
 * and every framed-protocol message (the same `u32 length | CBOR payload`
 * bytes `--tcp` carries) rides as a WebSocket *binary* message payload.
 *
 * Policy (bounded + fail-closed, matching frame.c):
 *   - client frames MUST be masked; server frames are never masked;
 *   - only binary data messages are accepted (text is closed 1003);
 *   - reserved bits, unmasked/oversized/control-fragmented frames and a
 *     declared length over AGENTC_WS_MAX_MESSAGE are protocol errors;
 *   - a fragmented data message is reassembled up to the same 16 MiB cap;
 *   - a violation latches the decoder (no further frames).
 */
#ifndef AGENTC_WS_H
#define AGENTC_WS_H

#include "agentc.h"
#include "frame.h"

/* Same cap as the frame transport (C-D12). */
#define AGENTC_WS_MAX_MESSAGE AGENTC_FRAME_MAX_BYTES
/* RFC 6455 handshake. */
#define AGENTC_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define AGENTC_WS_SUBPROTOCOL "agentc.v1"
/* Bounded request-header block. */
#define AGENTC_WS_MAX_HEADER 8192u

/* SHA-1 (RFC 3174) and standard base64 (RFC 4648) over a bounded buffer.
 * `out` is NUL-terminated. agentc_ws_base64 returns the length written (no NUL)
 * or 0 when `cap` is too small. */
void agentc_ws_sha1(const void *data, size_t n, u8 out[20]);
size_t agentc_ws_base64(const void *data, size_t n, char *out, size_t cap);
/* Sec-WebSocket-Accept for a client key: base64(SHA1(key ++ GUID)). */
int agentc_ws_accept(const char *key, char *out, size_t cap);

/* Server handshake over a byte stream. `in` accumulates the client's request.
 * Returns 1 when the request is accepted (the 101 response is appended to
 * `out`; `in` keeps only the bytes after the header block), a negative errno
 * when it is rejected (a 4xx response is appended to `out`), or 0 when the
 * header block is not complete yet. A header block over AGENTC_WS_MAX_HEADER is
 * rejected -EMSGSIZE. `out` is never cleared by this call. */
int agentc_ws_server_handshake(AgcBuf *in, AgcBuf *out);

/* -------------------------------------------------------------- decoding */
/* Opcodes surfaced to the feed callback. */
enum {
    AGENTC_WS_BINARY = 2,
    AGENTC_WS_CLOSE = 8,
    AGENTC_WS_PING = 9,
    AGENTC_WS_PONG = 10,
};

typedef struct AgcWsDecoder {
    AgcBuf buf;  /* pending raw bytes */
    AgcBuf msg;  /* reassembling data message */
    int opcode;  /* current fragmented-message opcode, 0 when idle */
    bool dead;   /* a protocol error (or a close) latched */
    bool closed; /* a Close frame was seen (dead, no error) */
    int error;   /* latched negative errno */
} AgcWsDecoder;

void agentc_ws_decoder_init(AgcWsDecoder *d);
void agentc_ws_decoder_free(AgcWsDecoder *d);

/* Consume raw bytes, invoking `cb` for each complete binary data message
 * (AGENTC_WS_BINARY) and each control frame (CLOSE/PING/PONG; the CLOSE payload
 * is the raw close payload). The payload is borrowed from the decoder and is
 * valid only for the call. Returns 0, the callback's nonzero return, or a
 * negative errno on a protocol violation (sticky). */
typedef int (*AgcWsFeedFn)(void *ud, int opcode, const u8 *payload, size_t len);
int agentc_ws_decoder_feed(AgcWsDecoder *d, const void *in, size_t n, AgcWsFeedFn cb, void *ud);

/* -------------------------------------------------------------- encoding */
/* Append one unmasked server frame. `opcode` is the low nibble. -> 0 | -EMSGSIZE. */
int agentc_ws_encode_frame(AgcBuf *out, int opcode, const void *payload, size_t len);
/* Append a binary data message (FIN=1, opcode 2). */
int agentc_ws_encode(AgcBuf *out, const void *payload, size_t len);
/* Append a PONG echoing `payload`. */
int agentc_ws_encode_pong(AgcBuf *out, const void *payload, size_t len);
/* Append a Close frame carrying a big-endian u16 status code. */
int agentc_ws_encode_close(AgcBuf *out, u16 code);

#endif /* AGENTC_WS_H */
