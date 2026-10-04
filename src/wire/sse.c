/* sse.c — incremental Server-Sent Events parser.
 *
 * Handles arbitrary chunk boundaries, LF / CRLF / CR line endings, multiple
 * data: lines joined with '\n', comment lines starting with ':' and the
 * [DONE] sentinel (passed through as ordinary data). Each event dispatches
 * through the callback with a NUL-terminated data buffer valid only for the
 * duration of the call.
 *
 * An event is dispatched when at least one data field has been seen, even
 * when every value is empty (a bare `data:` line dispatches empty data),
 * matching the WHATWG event-stream rules. A 1 MB cap per line and per event
 * guards against unbounded input; exceeding it returns -E2BIG.
 *
 * Internals: AgcSse.buf stores the current line with a one-byte flag prefix
 * (bit 0 = last line ended with CR, to fold CRLF into one terminator), because
 * the frozen public struct has no room for a separate flag. The event name is
 * owned by the parser and freed by agentc_sse_free().
 */
#include "net/net_internal.h"
#include "base/limits.h"

#define SSE_E2BIG (-7)
#define SSE_CR_FLAG 1u

static void sse_reset_event(AgcSse *s) {
    s->saw_data = false;
    if (s->event != NULL) {
        agentc_free((void *)s->event);
        s->event = NULL;
    }
    agentc_buf_clear(&s->data);
}

/* The current line (without terminator) lives at buf.p + 1. */
static int sse_line(AgcSse *s, int (*cb)(void *, const AgcSseEvent *), void *ud) {
    const char *line = (const char *)s->buf.p + 1;
    size_t len = s->buf.len > 0 ? s->buf.len - 1 : 0;

    if (len == 0) {
        int rc = 0;
        if (s->saw_data) {
            u8 *z = agentc_buf_reserve(&s->data, 1);
            *z = '\0';
            AgcSseEvent ev;
            ev.event = s->event != NULL ? s->event : "";
            ev.data = (const char *)s->data.p;
            ev.data_len = s->data.len;
            if (cb != NULL) rc = cb(ud, &ev);
        }
        sse_reset_event(s);
        s->buf.len = 1; /* keep the CR flag byte */
        return rc;
    }

    s->buf.len = 1;
    if (line[0] == ':') return 0; /* comment */

    size_t colon = 0;
    while (colon < len && line[colon] != ':') colon++;
    size_t name_len = colon;
    const char *value = line;
    size_t vlen = 0;
    if (colon < len) {
        value = line + colon + 1;
        vlen = len - colon - 1;
        if (vlen > 0 && value[0] == ' ') { value++; vlen--; }  /* exactly one space */
    }

    if (name_len == 5 && agentc_str_eq(line, 5, "event", 5)) {
        if (s->event != NULL) agentc_free((void *)s->event);
        s->event = agentc_strdup_len(value, vlen);
        return 0;
    }
    if (name_len == 4 && agentc_str_eq(line, 4, "data", 4)) {
        if (s->data.len + vlen + 1 > AGENTC_LIMIT_SSE_EVENT_BYTES) return SSE_E2BIG;
        if (s->saw_data) agentc_buf_byte(&s->data, '\n');
        agentc_buf_push(&s->data, value, vlen);
        s->saw_data = true;
        return 0;
    }
    return 0; /* id:, retry:, unknown fields are ignored */
}

void agentc_sse_init(AgcSse *s) {
    if (s == NULL) return;
    agentc_memset(s, 0, sizeof(*s));
    agentc_buf_byte(&s->buf, 0); /* CR flag placeholder */
}

int agentc_sse_feed(AgcSse *s, const void *p, size_t n,
                        int (*cb)(void *, const AgcSseEvent *), void *ud) {
    if (s == NULL || (p == NULL && n > 0)) return -22;
    const u8 *d = p;
    for (size_t i = 0; i < n; i++) {
        u8 c = d[i];
        u8 flags = s->buf.p[0];
        if (c == '\r') {
            int rc = sse_line(s, cb, ud);
            if (rc != 0) return rc;
            s->buf.p[0] = flags | SSE_CR_FLAG;
        } else if (c == '\n') {
            if (!(flags & SSE_CR_FLAG)) {
                int rc = sse_line(s, cb, ud);
                if (rc != 0) return rc;
            }
            s->buf.p[0] = flags & (u8)~SSE_CR_FLAG;
        } else {
            s->buf.p[0] = flags & (u8)~SSE_CR_FLAG;
            if (s->buf.len - 1 >= AGENTC_LIMIT_SSE_EVENT_BYTES) return SSE_E2BIG;
            agentc_buf_byte(&s->buf, c);
        }
    }
    return 0;
}

void agentc_sse_free(AgcSse *s) {
    if (s == NULL) return;
    if (s->event != NULL) agentc_free((void *)s->event);
    s->event = NULL;
    agentc_buf_free(&s->buf);
    agentc_buf_free(&s->data);
}
