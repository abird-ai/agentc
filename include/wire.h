/* wire.h — HTTP/1.1 and SSE.
 *
 * agentc_http is a small state machine driven by agentc_http_run(): it owns the socket
 * and TLS connection, parses the response head, decodes chunked bodies and
 * invokes on_body() for each body chunk as it arrives. It polls internally for
 * up to timeout_ms total, so the caller stays single-threaded and can abort by
 * returning nonzero from the callback.
 */
#ifndef AGENTC_WIRE_H
#define AGENTC_WIRE_H

#include "agentc.h"
#include "net.h"
#include "base/limits.h"

typedef struct AgcHttp AgcHttp;

/* headers: "Name: value\r\n" lines, may be NULL. body may be NULL. */
AgcHttp *agentc_http_new(const char *method, const char *url,
                    const char *headers, const void *body, size_t body_len);
/* on_body returns 0 to continue, nonzero to abort the request. */
int agentc_http_run(AgcHttp *h, int (*on_body)(void *ud, const void *p, size_t n),
                void *ud, int timeout_ms);            /* 0 | -errno */
int agentc_http_status(const AgcHttp *h);
const char *agentc_http_header(const AgcHttp *h, const char *name);
/* Response headers as a JSON object (borrowed; owned by `h`, rebuilt per
 * response). Duplicate names: the last occurrence wins. "{}" when the
 * response carried no headers (or h is NULL). */
const char *agentc_http_headers_json(const AgcHttp *h);
void agentc_http_free(AgcHttp *h);
const char *agentc_http_error(const AgcHttp *h);
/* raw wire bytes for --record / debugging */
void agentc_http_set_record(AgcHttp *h, AgcBuf *sink);
/* Certificate verification for all subsequent requests (--insecure). An explicit
 * AGENTC_TLS_VERIFY environment setting still wins over this. */
void agentc_http_set_insecure(bool on);

/* Called before each internal poll wait, so an interactive front end (TUI) can
 * read input and redraw while a request is in flight. `timeout_ms` is the wait
 * budget about to be used (-1 = indefinite); the hook must return promptly. */
typedef void (*AgcPollHook)(void *ud, int timeout_ms);
void agentc_http_set_poll_hook(AgcPollHook hook, void *ud);

/* Optional cooperative cancellation for a request. `cancel` is borrowed (may be
 * NULL) and polled before connect, on every poll slice and between body chunks;
 * while it is true agentc_http_run() returns -ECANCELED (-125) and
 * agentc_http_error() reports "cancelled". The code is distinct from a plain
 * timeout and is not retried (agentc_retry_retryable excludes -ECANCELED), so an
 * MCP tools/call or a provider stream can be abandoned without waiting out the
 * request timeout. Call after agentc_http_new() and before agentc_http_run(). */
void agentc_http_set_cancel(AgcHttp *h, const volatile bool *cancel);

/* ------------------------------------------------------------------- SSE */
typedef struct {
    AgcBuf buf;              /* partial line */
    const char *event;      /* owned; NULL when the event field was absent */
    AgcBuf data;
    bool saw_data;          /* a data field was seen, even if empty */
} AgcSse;

typedef struct {
    const char *event;      /* "" when the event: field was absent */
    const char *data;       /* NUL-terminated, may contain \n */
    size_t data_len;
} AgcSseEvent;

/* Feed bytes; cb returns 0 to continue, nonzero to abort. */
void agentc_sse_init(AgcSse *s);
int agentc_sse_feed(AgcSse *s, const void *p, size_t n,
                int (*cb)(void *ud, const AgcSseEvent *ev), void *ud);
typedef int (*AgcSseCb)(void *ud, const AgcSseEvent *ev);
/* Clean end of stream: dispatch an event whose fields were all received but
 * whose terminating blank line never arrived. A final, unterminated field line
 * is a truncated event and is discarded (WHATWG event-stream). Safe to call more
 * than once; agentc_sse_free() remains allocation-only. */
void agentc_sse_finish(AgcSse *s, AgcSseCb cb, void *ud);
void agentc_sse_free(AgcSse *s);

/* ------------------------------------------------------------------ URL */
typedef struct {
    bool https;
    char host[256];
    u16 port;
    char path[1024];        /* starts with '/' */
} AgcUrl;

int agentc_url_parse(const char *url, AgcUrl *out);

/* Streaming JSON writer (requests, session lines). The struct is the whole
 * writer state, so any number of writers may be used interleaved. */
typedef struct {
    AgcBuf *b;
    u32 depth;
    bool need_comma[AGENTC_JSON_MAX_DEPTH];
    u8 ctype[AGENTC_JSON_MAX_DEPTH];    /* container type per level: 0 = object, 1 = array */
    bool ascii;      /* when true, bytes >= 0x7f are emitted as \uXXXX */
} AgcJsonW;

void agentc_jsonw_init(AgcJsonW *w, AgcBuf *b);
void agentc_jsonw_set_ascii(AgcJsonW *w, bool on);
void agentc_jsonw_obj(AgcJsonW *w);         /* { */
void agentc_jsonw_arr(AgcJsonW *w);         /* [ */
void agentc_jsonw_end(AgcJsonW *w);         /* } or ] */
void agentc_jsonw_key(AgcJsonW *w, const char *key);
void agentc_jsonw_str(AgcJsonW *w, const char *s, size_t n);
void agentc_jsonw_cstr(AgcJsonW *w, const char *s);
void agentc_jsonw_u64(AgcJsonW *w, u64 v);
void agentc_jsonw_i64(AgcJsonW *w, i64 v);
void agentc_jsonw_bool(AgcJsonW *w, bool v);
void agentc_jsonw_raw(AgcJsonW *w, const char *raw, size_t n);   /* pre-encoded value */
void agentc_jsonw_null(AgcJsonW *w);

/* JSON string escaping into a buffer (no quotes). */
void agentc_json_escape(AgcBuf *b, const char *s, size_t n);

/* Re-emit a parsed document compactly through the writer (object key order and
 * number text preserved). Used to merge config/credential files in place. */
void agentc_json_emit(AgcJsonW *w, const AgcJson *v);

#endif /* AGENTC_WIRE_H */
