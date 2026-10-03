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
        int longs = 0;
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'j' || *fmt == 'h' || *fmt == 't') {
            if (*fmt == 'l') longs++;
            if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') longs = 2;
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
            if (longs >= 2) v = va_arg(ap, long long);
            else if (longs == 1) v = va_arg(ap, long);
            else v = va_arg(ap, int);
            put_num(&s, v < 0 ? (u64)(-(v + 1)) + 1 : (u64)v, 10, false, v < 0, width, pad, left);
            fmt++;
            break;
        }
        case 'u': case 'x': case 'X': {
            u64 v;
            if (longs >= 2) v = va_arg(ap, unsigned long long);
            else if (longs == 1) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned);
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
