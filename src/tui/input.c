/* input.c — incremental escape-sequence parser.
 *
 * States: ground -> ESC -> CSI/SS3, plus bracketed paste accumulation. A lone
 * ESC never blocks: the caller runs input_idle() from its idle/tick path and
 * supplies the clock, so a 50 ms timeout fires deterministically in tests.
 * That timeout is also the Alt disambiguation rule: while the parser sits in
 * IN_ESC no key is emitted, `ESC b` becomes Alt+b on the next byte, and only an
 * ESC with no following byte within the window is reported as K_ESC.
 */
#include "input.h"

enum {
    IN_GROUND = 0,
    IN_ESC,
    IN_CSI,
    IN_SS3,
    IN_PASTE,
};

void input_init(Input *in) {
    agentc_memset(in, 0, sizeof *in);
}

void input_free(Input *in) {
    agentc_buf_free(&in->paste);
}

bool input_busy(const Input *in) {
    return in->state != IN_GROUND || in->utf8_need > 0;
}

static void emit_key(KeyFn cb, void *ud, int code, int mods, u32 cp) {
    if (!cb) return;
    Key k = { code, mods, cp, NULL, 0 };
    cb(ud, &k);
}

static void emit_paste(KeyFn cb, void *ud, const u8 *p, size_t n) {
    if (!cb) return;
    Key k = { K_PASTE, 0, 0, p, n };
    cb(ud, &k);
}

static int mods_from_param(int p) {
    if (p <= 0) return 0;
    int m = p - 1;
    int mods = 0;
    if (m & 1) mods |= MOD_SHIFT;
    if (m & 2) mods |= MOD_ALT;
    if (m & 4) mods |= MOD_CTRL;
    return mods;
}

/* idx-th numeric parameter of a CSI sequence, -1 when absent. */
static int csi_param(const u8 *s, size_t n, int idx) {
    int cur = -1, i = 0;
    size_t pos = 0;
    /* skip private markers */
    while (pos < n && (s[pos] == '?' || s[pos] == '<' || s[pos] == '=' || s[pos] == '>')) pos++;
    for (size_t p = pos; p < n; p++) {
        if (s[p] == ';') {
            if (i == idx) return cur < 0 ? 0 : cur;
            i++;
            cur = -1;
        } else if (s[p] >= '0' && s[p] <= '9') {
            if (cur < 0) cur = 0;
            /* Clamp while accumulating: a long digit run must not overflow the
             * signed int, and no real parameter is anywhere near this large. */
            if (cur < (1 << 20)) {
                cur = cur * 10 + (s[p] - '0');
                if (cur > (1 << 20)) cur = (1 << 20);
            }
        }
    }
    if (i == idx) return cur < 0 ? 0 : cur;
    return -1;
}

static void csi_dispatch(Input *in, KeyFn cb, void *ud) {
    const u8 *s = in->seq;
    size_t n = in->seqlen;
    if (n == 0) return;
    u8 final = s[n - 1];
    const u8 *body = s;
    size_t blen = n - 1;
    /* paste markers */
    if (final == '~' && csi_param(body, blen, 0) == 200) {
        agentc_buf_clear(&in->paste);
        in->state = IN_PASTE;
        return;
    }
    int code = K_NONE;
    switch (final) {
    case 'A': code = K_UP; break;
    case 'B': code = K_DOWN; break;
    case 'C': code = K_RIGHT; break;
    case 'D': code = K_LEFT; break;
    case 'H': code = K_HOME; break;
    case 'F': code = K_END; break;
    case 'Z': emit_key(cb, ud, K_TAB, MOD_SHIFT, 0); return;
    case '~': {
        int p1 = csi_param(body, blen, 0);
        if (p1 == 1 || p1 == 7) code = K_HOME;
        else if (p1 == 2) code = K_INSERT;
        else if (p1 == 3) code = K_DELETE;
        else if (p1 == 4 || p1 == 8) code = K_END;
        else if (p1 == 5) code = K_PGUP;
        else if (p1 == 6) code = K_PGDN;
        else if (p1 == 11) code = K_F1;
        else if (p1 == 12) code = K_F2;
        else if (p1 == 13) code = K_F3;
        else if (p1 == 14) code = K_F4;
        break;
    }
    case 'u': { /* kitty keyboard protocol: cp;mods;event */
        int cp = csi_param(body, blen, 0);
        int mods = mods_from_param(csi_param(body, blen, 1));
        if (cp == 8 || cp == 127) emit_key(cb, ud, K_BACKSPACE, mods, 0);
        else if (cp == 13) emit_key(cb, ud, K_ENTER, mods, 0);
        else if (cp == 9) emit_key(cb, ud, K_TAB, mods, 0);
        else if (cp == 27) emit_key(cb, ud, K_ESC, mods, 0);
        else if (cp > 0) emit_key(cb, ud, K_CHAR, mods, (u32)cp);
        return;
    }
    default:
        return;
    }
    if (code == K_NONE) return;
    int p2 = csi_param(body, blen, 1);
    int mods = mods_from_param(p2);
    /* Some terminals send the legacy single-parameter modifier form instead of
     * `1;mod`: CSI 5D is Ctrl+Left and CSI 3A is Alt+Up. For `~` the first
     * parameter is the key code, so the legacy form only applies to the
     * A/B/C/D/H/F family. */
    if (p2 < 0 && final != '~') {
        int p1 = csi_param(body, blen, 0);
        if (p1 >= 2 && p1 <= 8) mods = mods_from_param(p1);
    }
    emit_key(cb, ud, code, mods, 0);
}

static void ss3_dispatch(Input *in, KeyFn cb, void *ud) {
    if (in->seqlen != 1) return;
    int code = K_NONE;
    switch (in->seq[0]) {
    case 'A': code = K_UP; break;
    case 'B': code = K_DOWN; break;
    case 'C': code = K_RIGHT; break;
    case 'D': code = K_LEFT; break;
    case 'H': code = K_HOME; break;
    case 'F': code = K_END; break;
    case 'P': emit_key(cb, ud, K_F1, 0, 0); return;
    case 'Q': emit_key(cb, ud, K_F2, 0, 0); return;
    case 'R': emit_key(cb, ud, K_F3, 0, 0); return;
    case 'S': emit_key(cb, ud, K_F4, 0, 0); return;
    default: return;
    }
    emit_key(cb, ud, code, 0, 0);
}

static void utf8_start(Input *in, u8 b, int mods) {
    if (b >= 0xC2 && b <= 0xDF) { in->utf8_need = 1; in->utf8_cp = b & 0x1F; in->utf8_min = 0x80; }
    else if (b >= 0xE0 && b <= 0xEF) { in->utf8_need = 2; in->utf8_cp = b & 0x0F; in->utf8_min = 0x800; }
    else if (b >= 0xF0 && b <= 0xF4) { in->utf8_need = 3; in->utf8_cp = b & 0x07; in->utf8_min = 0x10000; }
    else { in->utf8_need = 0; }
    in->utf8_mods = mods;
}

static bool utf8_feed(Input *in, u8 b, KeyFn cb, void *ud) {
    if (in->utf8_need <= 0) return false;
    if ((b & 0xC0) != 0x80) {
        /* malformed: emit replacement and re-process this byte in the caller */
        emit_key(cb, ud, K_CHAR, in->utf8_mods, 0xFFFD);
        in->utf8_need = 0;
        return false;
    }
    in->utf8_cp = (in->utf8_cp << 6) | (b & 0x3F);
    if (--in->utf8_need == 0) {
        u32 cp = in->utf8_cp;
        if (cp < in->utf8_min || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
        emit_key(cb, ud, K_CHAR, in->utf8_mods, cp);
    }
    return true;
}

static void ground_byte(Input *in, u8 b, i64 now, KeyFn cb, void *ud) {
    if (in->utf8_need > 0) {
        if (utf8_feed(in, b, cb, ud)) return;
        /* fallthrough: malformed continuation, reinterpret in ground */
    }
    if (in->last_cr && b == '\n') { in->last_cr = false; return; }
    in->last_cr = false;
    if (b == 0x1B) {
        in->state = IN_ESC;
        in->esc_at_ns = now;
        in->seqlen = 0;
        return;
    }
    if (b == '\r') { in->last_cr = true; in->cr_at_ns = now; emit_key(cb, ud, K_ENTER, 0, 0); return; }
    if (b == '\n') { emit_key(cb, ud, K_ENTER, 0, 0); return; }
    if (b == 0x7F || b == 0x08) { emit_key(cb, ud, K_BACKSPACE, 0, 0); return; }
    if (b == 0x09) { emit_key(cb, ud, K_TAB, 0, 0); return; }
    if (b < 0x20) {
        /* control bytes: ctrl+a .. ctrl+z, ctrl+space */
        u32 cp = b == 0 ? ' ' : (u32)('a' + b - 1);
        if (b >= 27) cp = (u32)(b + 0x40);
        emit_key(cb, ud, K_CHAR, MOD_CTRL, cp);
        return;
    }
    if (b < 0x80) { emit_key(cb, ud, K_CHAR, 0, b); return; }
    utf8_start(in, b, 0);
}

void input_feed(Input *in, const u8 *p, size_t n, i64 now_ns, KeyFn cb, void *ud) {
    static const char endmark[] = "\x1b[201~";
    size_t pm = 0;
    if (in->state == IN_PASTE) {
        /* recompute the partial marker length from what we already buffered is
         * not possible; track it via seqlen (reused as marker position). */
        pm = in->seqlen;
    }
    for (size_t i = 0; i < n; i++) {
        u8 b = p[i];
        switch (in->state) {
        case IN_GROUND:
            ground_byte(in, b, now_ns, cb, ud);
            break;
        case IN_ESC:
            in->last_cr = false;
            if (b == '[') { in->state = IN_CSI; in->seqlen = 0; }
            else if (b == 'O') { in->state = IN_SS3; in->seqlen = 0; }
            else if (b == 0x1B) { emit_key(cb, ud, K_ESC, 0, 0); in->esc_at_ns = now_ns; }
            else if (b == '\r' || b == '\n') { emit_key(cb, ud, K_ENTER, MOD_ALT, 0); in->state = IN_GROUND; }
            else if (b == 0x7F || b == 0x08) { emit_key(cb, ud, K_BACKSPACE, MOD_ALT, 0); in->state = IN_GROUND; }
            else if (b == 0x09) { emit_key(cb, ud, K_TAB, MOD_ALT, 0); in->state = IN_GROUND; }
            else if (b < 0x20) { emit_key(cb, ud, K_CHAR, MOD_CTRL | MOD_ALT, (u32)('a' + b - 1)); in->state = IN_GROUND; }
            else if (b < 0x80) { emit_key(cb, ud, K_CHAR, MOD_ALT, b); in->state = IN_GROUND; }
            else { in->state = IN_GROUND; utf8_start(in, b, MOD_ALT); }
            break;
        case IN_CSI:
            if (b == 0x1B) { in->state = IN_ESC; in->esc_at_ns = now_ns; in->seqlen = 0; break; }
            if (in->seqlen < sizeof in->seq - 1) in->seq[in->seqlen++] = b;
            if (b >= 0x40 && b <= 0x7E) {
                csi_dispatch(in, cb, ud);
                if (in->state != IN_PASTE) in->state = IN_GROUND;
                in->seqlen = 0;
            }
            break;
        case IN_SS3:
            if (b == 0x1B) { in->state = IN_ESC; in->esc_at_ns = now_ns; in->seqlen = 0; break; }
            if (in->seqlen < sizeof in->seq - 1) in->seq[in->seqlen++] = b;
            if (b >= 0x40 && b <= 0x7E) {
                ss3_dispatch(in, cb, ud);
                in->state = IN_GROUND;
                in->seqlen = 0;
            }
            break;
        case IN_PASTE: {
            if (b == (u8)endmark[pm]) {
                pm++;
                if (pm == sizeof endmark - 1) {
                    emit_paste(cb, ud, in->paste.p, in->paste.len);
                    agentc_buf_clear(&in->paste);
                    in->state = IN_GROUND;
                    in->seqlen = 0;
                    pm = 0;
                }
            } else {
                for (size_t k = 0; k < pm; k++) agentc_buf_byte(&in->paste, (u8)endmark[k]);
                pm = 0;
                if (b == 0x1B) {
                    pm = 1;
                } else {
                    agentc_buf_byte(&in->paste, b);
                }
            }
            break;
        }
        default:
            break;
        }
    }
    in->seqlen = in->state == IN_PASTE ? pm : in->seqlen;
}

void input_idle(Input *in, i64 now_ns, int esc_timeout_ms, KeyFn cb, void *ud) {
    /* The CRLF fold is bounded by the idle window: a CR and its LF that arrive
     * in separate reads are still folded, but a lone LF (Ctrl+J) after the
     * window starts its own Enter instead of being swallowed forever. Also drop
     * any pending ESC state below. */
    if (in->last_cr && now_ns - in->cr_at_ns >= (i64)esc_timeout_ms * 1000000)
        in->last_cr = false;
    if (in->state == IN_ESC && now_ns - in->esc_at_ns >= (i64)esc_timeout_ms * 1000000) {
        in->state = IN_GROUND;
        emit_key(cb, ud, K_ESC, 0, 0);
    } else if ((in->state == IN_CSI || in->state == IN_SS3) &&
               now_ns - in->esc_at_ns >= (i64)esc_timeout_ms * 1000000) {
        /* incomplete sequence: drop it rather than confusing the editor */
        in->state = IN_GROUND;
        in->seqlen = 0;
    }
}
