/* fmt.c — a small formatted-output core (no libc printf).
 * Supports: %s %c %d %i %u %x %X %p %%, with l/ll/z/j/h length modifiers.
 * The _used variants report what was actually written, for callers that hand
 * the length to a writer. */
#include "agentc.h"

typedef struct {
    char *out;
    size_t cap;
    size_t pos;
} Sink;

static void put(Sink *s, char c) {
    if (s->pos + 1 < s->cap) s->out[s->pos] = c;
    s->pos++;
}

static void puts_(Sink *s, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) put(s, p[i]);
}

static void put_num(Sink *s, u64 v, unsigned base, bool upper, bool neg, int width, char pad,
                     bool left) {
    char tmp[32];
    size_t n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    int total = (int)n + (neg ? 1 : 0);
    if (neg && !left && pad == '0') put(s, '-');      /* sign before zero padding */
    if (!left)
        for (int i = total; i < width; i++) put(s, pad);
    if (neg && (left || pad != '0')) put(s, '-');     /* sign immediately before digits */
    for (size_t i = 0; i < n; i++) put(s, tmp[n - 1 - i]);
    if (left)
        for (int i = total; i < width; i++) put(s, ' ');
}

int agentc_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
    Sink s = { out, cap, 0 };
    while (*fmt) {
        if (*fmt != '%') { put(&s, *fmt++); continue; }
        fmt++;
        int width = 0;
        bool left = false;
        char pad = ' ';
        if (*fmt == '-') { left = true; fmt++; }
        else if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        /* Length modifier. hh/h truncate to signed char/short, l/ll select
         * long/long long, z/t use size_t/ptrdiff_t (their signed/unsigned
         * counterparts), j selects intmax_t. */
        enum { LM_NONE, LM_HH, LM_H, LM_L, LM_LL, LM_Z, LM_T, LM_J } lm = LM_NONE;
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'j' || *fmt == 'h' || *fmt == 't') {
            switch (*fmt) {
            case 'l': lm = (lm == LM_L) ? LM_LL : LM_L; break;
            case 'h': lm = (lm == LM_H) ? LM_HH : LM_H; break;
            case 'z': lm = LM_Z; break;
            case 't': lm = LM_T; break;
            case 'j': lm = LM_J; break;
            default: break;
            }
            fmt++;
        }
        switch (*fmt) {
        case 0: goto done;
        case '%': put(&s, '%'); fmt++; break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left)
                for (int i = 1; i < width; i++) put(&s, pad);
            put(&s, c);
            if (left)
                for (int i = 1; i < width; i++) put(&s, ' ');
            fmt++;
            break;
        }
        case 's': {
            const char *p = va_arg(ap, const char *);
            if (!p) p = "(null)";
            size_t len = agentc_strlen(p);
            if (!left)
                for (size_t i = len; i < (size_t)width; i++) put(&s, pad);
            puts_(&s, p, len);
            if (left)
                for (size_t i = len; i < (size_t)width; i++) put(&s, ' ');
            fmt++;
            break;
        }
        case 'd': case 'i': {
            i64 v;
            switch (lm) {
            case LM_HH: v = (signed char)va_arg(ap, int); break;
            case LM_H:  v = (short)va_arg(ap, int); break;
            case LM_L:  v = va_arg(ap, long); break;
            case LM_LL: case LM_J: v = va_arg(ap, long long); break;
            case LM_Z:  v = (i64)va_arg(ap, ptrdiff_t); break;   /* signed size_t */
            case LM_T:  v = (i64)va_arg(ap, ptrdiff_t); break;
            default:    v = va_arg(ap, int); break;
            }
            put_num(&s, v < 0 ? (u64)(-(v + 1)) + 1 : (u64)v, 10, false, v < 0, width, pad, left);
            fmt++;
            break;
        }
        case 'u': case 'x': case 'X': {
            u64 v;
            switch (lm) {
            case LM_HH: v = (unsigned char)va_arg(ap, int); break;
            case LM_H:  v = (unsigned short)va_arg(ap, int); break;
            case LM_L:  v = va_arg(ap, unsigned long); break;
            case LM_LL: case LM_J: v = va_arg(ap, unsigned long long); break;
            case LM_Z:  v = va_arg(ap, size_t); break;
            case LM_T:  v = (u64)va_arg(ap, size_t); break;        /* unsigned ptrdiff_t */
            default:    v = va_arg(ap, unsigned); break;
            }
            unsigned base = (*fmt == 'u') ? 10 : 16;
            put_num(&s, v, base, *fmt == 'X', false, width, pad, left);
            fmt++;
            break;
        }
        case 'p': {
            u64 v = (u64)(uintptr_t)va_arg(ap, void *);
            put(&s, '0'); put(&s, 'x');
            put_num(&s, v, 16, false, false, 0, ' ', left);
            fmt++;
            break;
        }
        default:
            put(&s, '%');
            break;
        }
    }
done:
    if (cap) s.out[s.pos < cap ? s.pos : cap - 1] = 0;
    return (int)s.pos;
}

int agentc_snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = agentc_vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

int agentc_vsnprintf_used(char *out, size_t cap, const char *fmt, va_list ap) {
    int n = agentc_vsnprintf(out, cap, fmt, ap);
    if (n <= 0 || cap == 0) return 0;
    return (int)((size_t)n < cap ? (size_t)n : cap - 1);
}

int agentc_snprintf_used(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = agentc_vsnprintf_used(out, cap, fmt, ap);
    va_end(ap);
    return n;
}
