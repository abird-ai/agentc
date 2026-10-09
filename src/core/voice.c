/* voice.c — framed realtime-voice helper codec (P6-VOICE §12).
 *
 * See include/voice.h for the wire contract. The codec is small, bounded and
 * allocation-free: encoding appends to a caller buffer, and the streaming
 * decoder owns one fixed frame buffer. No libc. Single-threaded.
 *
 * Owner: the core (the helper transport is app-level because the stable
 * extension ABI has no actor_spawn slot yet).
 */
#include "voice.h"

int agentc_voice_frame_encode(u8 type, const void *payload, size_t len, AgcBuf *out) {
    if (out == NULL)
        return -22; /* EINVAL */
    if (len != 0 && payload == NULL)
        return -22; /* EINVAL */
    if (len > AGENTC_VOICE_FRAME_MAX_BYTES)
        return -90; /* EMSGSIZE */
    u8 hdr[AGENTC_VOICE_HEADER_BYTES];
    hdr[0] = (u8)((len >> 24) & 0xffu);
    hdr[1] = (u8)((len >> 16) & 0xffu);
    hdr[2] = (u8)((len >> 8) & 0xffu);
    hdr[3] = (u8)(len & 0xffu);
    hdr[4] = type;
    agentc_buf_push(out, hdr, sizeof hdr);
    if (len != 0)
        agentc_buf_push(out, payload, len);
    return 0;
}

const char *agentc_voice_frame_type_name(u8 type) {
    switch (type) {
    case AGENTC_VOICE_CTRL_JSON:
        return "ctrl_json";
    case AGENTC_VOICE_PCM_IN:
        return "pcm_in";
    case AGENTC_VOICE_PCM_OUT:
        return "pcm_out";
    case AGENTC_VOICE_TRANSCRIPT:
        return "transcript";
    case AGENTC_VOICE_ITEM:
        return "item";
    case AGENTC_VOICE_TOOLCALL:
        return "toolcall";
    case AGENTC_VOICE_TOOLRESULT:
        return "toolresult";
    case AGENTC_VOICE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

void agentc_voice_decoder_init(AgcVoiceDecoder *d) {
    if (d == NULL)
        return;
    agentc_memset(d, 0, sizeof *d);
}

size_t agentc_voice_decoder_pending(const AgcVoiceDecoder *d) {
    return d ? d->have : 0;
}

/* Decode the 4-byte big-endian length prefix already buffered in d->buf. */
static size_t decode_len(const u8 *p) {
    return ((size_t)p[0] << 24) | ((size_t)p[1] << 16) | ((size_t)p[2] << 8) | (size_t)p[3];
}

int agentc_voice_decoder_feed(AgcVoiceDecoder *d, const u8 *in, size_t n, AgcVoiceFrameFn cb,
                              void *ud) {
    if (d == NULL || (in == NULL && n != 0))
        return -22; /* EINVAL */
    if (d->dead)
        return -22; /* latched error */
    size_t off = 0;
    while (off < n) {
        if (!d->header) {
            size_t need = AGENTC_VOICE_HEADER_BYTES - d->have;
            size_t take = (n - off) < need ? (n - off) : need;
            agentc_memcpy(d->buf + d->have, in + off, take);
            d->have += take;
            off += take;
            if (d->have < AGENTC_VOICE_HEADER_BYTES)
                break;
            size_t len = decode_len(d->buf);
            if (len > AGENTC_VOICE_FRAME_MAX_BYTES) {
                d->dead = true;
                return -90; /* EMSGSIZE */
            }
            d->frame_len = AGENTC_VOICE_HEADER_BYTES + len;
            d->header = true;
        }
        size_t need = d->frame_len - d->have;
        size_t take = (n - off) < need ? (n - off) : need;
        agentc_memcpy(d->buf + d->have, in + off, take);
        d->have += take;
        off += take;
        if (d->have < d->frame_len)
            break;
        AgcVoiceFrame f;
        f.type = d->buf[4];
        f.payload = d->buf + AGENTC_VOICE_HEADER_BYTES;
        f.len = d->frame_len - AGENTC_VOICE_HEADER_BYTES;
        d->have = 0;
        d->frame_len = 0;
        d->header = false;
        int rc = cb ? cb(ud, &f) : 0;
        if (rc != 0)
            return rc;
    }
    return 0;
}

int agentc_voice_ctrl_frame(const char *json, AgcBuf *out) {
    if (json == NULL)
        return -22;
    return agentc_voice_frame_encode(AGENTC_VOICE_CTRL_JSON, json, agentc_strlen(json), out);
}
