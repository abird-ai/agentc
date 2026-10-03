/* out.c — terminal-safe writer; see base/out.h for the contract. */
#include "base/out.h"

#include "plat.h"

#define OUT_REPLACEMENT 0xfffdu
#define OUT_EIO (-5)

bool agentc_out_cp_safe(u32 cp) {
    if (cp < 0x20u) {
        /* \n and \t are ordinary text whitespace; \r is allowed because it
         * cannot start a control sequence on its own and byte-exact HTTP dumps
         * need it.  ESC is the only C0 byte that can introduce one. */
        return cp == '\n' || cp == '\t' || cp == '\r';
    }
    if (cp == 0x7fu) return false;           /* DEL */
    if (cp >= 0x80u && cp <= 0x9fu) return false;  /* C1: 8-bit CSI/OSC */
    return true;
}

u32 agentc_out_cp_filter(u32 cp) {
    return agentc_out_cp_safe(cp) ? cp : OUT_REPLACEMENT;
}

/* Lenient UTF-8 decode: an invalid lead byte is returned as-is with used = 1,
 * so sanitizing never drops or re-frames bytes it does not understand. */
static u32 out_next(const u8 *p, size_t n, size_t *used) {
    u8 c = p[0];

    if (c < 0x80u || n == 1) {
        *used = 1;
        return c;
    }
    if ((c & 0xe0u) == 0xc0u && n >= 2 && (p[1] & 0xc0u) == 0x80u) {
        *used = 2;
        return ((u32)(c & 0x1fu) << 6) | (u32)(p[1] & 0x3fu);
    }
    if ((c & 0xf0u) == 0xe0u && n >= 3 && (p[1] & 0xc0u) == 0x80u &&
        (p[2] & 0xc0u) == 0x80u) {
        *used = 3;
        return ((u32)(c & 0x0fu) << 12) | ((u32)(p[1] & 0x3fu) << 6) |
               (u32)(p[2] & 0x3fu);
    }
    if ((c & 0xf8u) == 0xf0u && n >= 4 && (p[1] & 0xc0u) == 0x80u &&
        (p[2] & 0xc0u) == 0x80u && (p[3] & 0xc0u) == 0x80u) {
        *used = 4;
        return ((u32)(c & 0x07u) << 18) | ((u32)(p[1] & 0x3fu) << 12) |
               ((u32)(p[2] & 0x3fu) << 6) | (u32)(p[3] & 0x3fu);
    }
    *used = 1;
    return c;
}

/* All bytes or the first error; 0 progress is an error, not a spin. */
static i64 out_write_full(int fd, const u8 *p, size_t n) {
    size_t off = 0;

    while (off < n) {
        int w = os_write(fd, p + off, n - off);
        if (w < 0) return w;
        if (w == 0) return OUT_EIO;
        off += (size_t)w;
    }
    return (i64)n;
}

i64 agentc_out_safe_write(int fd, const void *p, size_t n) {
    static const u8 repl[3] = { 0xef, 0xbf, 0xbd };
    const u8 *q = p;
    u8 tmp[4096];
    size_t i = 0, o = 0;
    i64 total = 0;

    while (i < n) {
        size_t used = 0, take;
        u32 cp = out_next(q + i, n - i, &used);

        if ((cp == '\n' || cp == '\t') && used == 1) {
            take = 1;
            tmp[o] = q[i];
        } else if (agentc_out_cp_safe(cp)) {
            take = used;
            agentc_memcpy(tmp + o, q + i, used);
        } else {
            take = sizeof repl;
            agentc_memcpy(tmp + o, repl, sizeof repl);
        }
        i += used;
        o += take;
        if (o + 4 > sizeof tmp) {
            i64 w = out_write_full(fd, tmp, o);
            if (w < 0) return total > 0 ? total : w;
            total += w;
            o = 0;
        }
    }
    if (o > 0) {
        i64 w = out_write_full(fd, tmp, o);
        if (w < 0) return total > 0 ? total : w;
        total += w;
    }
    return total;
}

void agentc_out_safe_buf(AgcBuf *b, const void *p, size_t n) {
    static const u8 repl[3] = { 0xef, 0xbf, 0xbd };
    const u8 *q = p;
    size_t i = 0;

    if (b == NULL || (p == NULL && n > 0)) return;
    while (i < n) {
        size_t used = 0;
        u32 cp = out_next(q + i, n - i, &used);

        if (agentc_out_cp_safe(cp)) agentc_buf_push(b, q + i, used);
        else agentc_buf_push(b, repl, sizeof repl);
        i += used;
    }
}

void agentc_out_raw(const void *p, size_t n) {
    (void)out_write_full(1, p, n);
}
