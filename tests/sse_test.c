/* sse_test.c — incremental SSE parsing: line endings, split boundaries,
 * multi-line data, comments, [DONE] and the size guard. */
#include "net/net_internal.h"

static int fails;

static void print_escaped(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\n') agentc_outs("\\n");
        else if (c == '\r') agentc_outs("\\r");
        else if (c == '\\') agentc_outs("\\\\");
        else agentc_out(&c, 1);
    }
}

static int on_ev(void *ud, const AgcSseEvent *ev) {
    (void)ud;
    agentc_outf("E|%s|%d|", ev->event, (int)ev->data_len);
    print_escaped(ev->data, ev->data_len);
    agentc_out_nl();
    return 0;
}

static int abort_ev(void *ud, const AgcSseEvent *ev) {
    (void)ud;
    (void)ev;
    return 7;
}

static int feed(const char *s, size_t step, int (*cb)(void *, const AgcSseEvent *)) {
    AgcSse ss;
    agentc_sse_init(&ss);
    size_t n = agentc_strlen(s);
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0;) {
        size_t chunk = (step == 0 || i + step > n) ? n - i : step;
        rc = agentc_sse_feed(&ss, s + i, chunk, cb, NULL);
        i += chunk;
    }
    agentc_sse_free(&ss);
    return rc;
}

static void case_run(const char *label, const char *s) {
    agentc_outf("%s whole rc=%d\n", label, feed(s, 0, on_ev));
    agentc_outf("%s split rc=%d\n", label, feed(s, 1, on_ev));
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    case_run("lf", "event: delta\ndata: he\ndata: llo\nid: 3\n\n");
    case_run("crlf", "event: delta\r\ndata: he\r\ndata: llo\r\n\r\n");
    case_run("cr", "event: delta\rdata: he\rdata: llo\r\r");
    case_run("mix", "data: a\n\rdata: b\r\n\ndata: c\n\n");
    case_run("comment", ": keep-alive\ndata: x\n: trailing\n\n");
    case_run("done", "data: [DONE]\n\n");
    case_run("nospace", "event:gzip\ndata:no-space\n\n");
    case_run("onespace", "data:  leading\n\n");
    case_run("event-only", "event: idle\n\n");
    case_run("empty-data", "data:\n\n");
    case_run("empty-then-data", "data:\ndata:foo\n\n");
    case_run("no-cr", "event: a\ndata: 1\n");

    agentc_outf("abort rc=%d\n", feed("data: x\n\n", 0, abort_ev));

    /* 1 MB guard: a single data line of 1 MiB + 8 bytes must be rejected. */
    AgcBuf big = { 0 };
    agentc_buf_cstr(&big, "data: ");
    for (size_t i = 0; i < 1024 * 1024; i++) agentc_buf_byte(&big, 'a');
    agentc_buf_cstr(&big, "\n");
    AgcSse ss;
    agentc_sse_init(&ss);
    int rc = agentc_sse_feed(&ss, big.p, big.len, on_ev, NULL);
    agentc_outf("big rc=%d\n", rc);
    agentc_sse_free(&ss);
    agentc_buf_free(&big);
    if (rc != -7) fails = 1;

    agentc_outf("fails=%d\n", fails);
    return fails;
}
