/* http_replay_test.c — deterministic HTTP/1.1 tests through src/net/mock.c:
 * Content-Length with short-read resume, chunked + trailers, EOF-delimited
 * bodies, aborting callbacks, resets, redirects (not followed), DNS and the
 * record buffer. Scripts live in tests/data/ (mock format in src/net/mock.c).
 */
#include "net/net_internal.h"

static int fails;
static AgcHttp *g_h;
static int g_rc;

static void want(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

static int collect(void *ud, const void *p, size_t n) {
    AgcBuf *b = ud;
    agentc_buf_push(b, p, n);
    return 0;
}

static int abort_cb(void *ud, const void *p, size_t n) {
    AgcBuf *b = ud;
    agentc_buf_push(b, p, n);
    return 1;
}

static void print_escaped(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\n') agentc_outs("\\n");
        else if (c == '\r') agentc_outs("\\r");
        else agentc_out(&c, 1);
    }
}

static void run(const char *label, const char *script, const char *method, const char *url,
                const char *headers, const void *data, size_t dlen, bool aborting,
                AgcBuf *body, AgcBuf *rec) {
    int lrc = agentc_mock_load(script);
    want("load", lrc == 0);
    g_h = agentc_http_new(method, url, headers, data, dlen);
    if (g_h == NULL) {
        want("new", false);
        g_rc = 0;
        return;
    }
    if (rec != NULL) agentc_http_set_record(g_h, rec);
    g_rc = agentc_http_run(g_h, aborting ? abort_cb : collect, body, 5000);
    agentc_outf("%s rc=%d status=%d err=", label, g_rc, agentc_http_status(g_h));
    print_escaped(agentc_http_error(g_h), agentc_strlen(agentc_http_error(g_h)));
    agentc_out_nl();
    agentc_outf("%s body=", label);
    print_escaped((const char *)body->p, body->len);
    agentc_out_nl();
}

static bool buf_is(const AgcBuf *b, const char *p, size_t n) {
    return b->len == n && (n == 0 || agentc_memeq(b->p, p, n));
}

static bool sent_has(const char *needle) {
    const AgcBuf *s = agentc_mock_sent();
    return s != NULL && s->p != NULL &&
           agentc_str_find((const char *)s->p, s->len, needle, agentc_strlen(needle)) >= 0;
}

static int sent_count(const char *needle) {
    const AgcBuf *s = agentc_mock_sent();
    if (s == NULL || s->p == NULL) return 0;
    int n = 0;
    size_t nl = agentc_strlen(needle);
    for (size_t i = 0; i + nl <= s->len;) {
        if (agentc_memeq(s->p + i, needle, nl)) {
            n++;
            i += nl;
        } else {
            i++;
        }
    }
    return n;
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* ---------------------------------------------- Content-Length + short */
    {
        AgcBuf body = { 0 }, rec = { 0 };
        run("content", "tests/data/http_content_length.mock", "GET", "http://api.test/hello",
            NULL, NULL, 0, false, &body, &rec);
        const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                           "Content-Length: 12\r\nX-Trace: a, b\r\n\r\nhello world\n";
        want("content.rc", g_rc == 0);
        want("content.body", buf_is(&body, "hello world\n", 12));
        want("content.trace", agentc_streq(agentc_http_header(g_h, "X-Trace"), "a, b"));
        want("content.req.host", sent_has("Host: api.test\r\n"));
        want("content.req.close", sent_has("Connection: close\r\n"));
        /* the record sink captures raw wire bytes: request head + response */
        AgcBuf want_rec = { 0 };
        agentc_buf_cstr(&want_rec, "GET /hello HTTP/1.1\r\nHost: api.test\r\n"
                                  "Connection: close\r\n\r\n");
        agentc_buf_cstr(&want_rec, resp);
        want("content.record", buf_is(&rec, (const char *)want_rec.p, want_rec.len));
        agentc_buf_free(&want_rec);
        agentc_http_free(g_h);
        agentc_buf_free(&body);
        agentc_buf_free(&rec);
    }

    /* ---------------------------------------------------- chunked + trailer */
    {
        AgcBuf body = { 0 };
        run("chunked", "tests/data/http_chunked.mock", "GET", "http://chunk.test/x", NULL,
            NULL, 0, false, &body, NULL);
        want("chunked.rc", g_rc == 0);
        want("chunked.body", buf_is(&body, "hello world\n", 12));
        want("chunked.te", agentc_streq(agentc_http_header(g_h, "Transfer-Encoding"), "chunked"));
        want("chunked.trailer", agentc_streq(agentc_http_header(g_h, "X-Trailer"), "yes"));
        agentc_http_free(g_h);
        agentc_buf_free(&body);
    }

    /* --------------------------------------------------------- EOF-delimited */
    {
        AgcBuf body = { 0 };
        run("eof", "tests/data/http_eof.mock", "GET", "http://eof.test/", NULL, NULL, 0,
            false, &body, NULL);
        want("eof.rc", g_rc == 0);
        want("eof.body", buf_is(&body, "eof-delimited\n", 14));
        agentc_http_free(g_h);
        agentc_buf_free(&body);
    }

    /* ----------------------------------------------------------- abort clean */
    {
        AgcBuf body = { 0 };
        run("abort", "tests/data/http_abort.mock", "GET", "http://abort.test/", NULL, NULL,
            0, true, &body, NULL);
        want("abort.rc", g_rc == -125);
        want("abort.partial", body.len > 0);
        agentc_http_free(g_h);
        agentc_buf_free(&body);
    }

    /* --------------------------------------------------------------- reset */
    {
        AgcBuf body = { 0 };
        run("rst", "tests/data/http_rst.mock", "GET", "http://rst.test/", NULL, NULL, 0,
            false, &body, NULL);
        want("rst.rc", g_rc == -104);
        want("rst.status", agentc_http_status(g_h) == 200);
        want("rst.err", agentc_streq(agentc_http_error(g_h), "recv"));
        agentc_http_free(g_h);
        agentc_buf_free(&body);
    }

    /* ------------------------------------------------------------ redirect */
    {
        AgcBuf body = { 0 };
        run("redirect", "tests/data/http_redirect.mock", "GET", "http://redir.test/", NULL,
            NULL, 0, false, &body, NULL);
        want("redirect.rc", g_rc == 0);
        want("redirect.status", agentc_http_status(g_h) == 302);
        want("redirect.nofollow", sent_count("GET ") == 1);
        agentc_http_free(g_h);
        agentc_buf_free(&body);
    }

    /* ----------------------------------------------------------------- DNS */
    {
        AgcBuf body = { 0 };
        run("dns", "tests/data/http_dns.mock", "GET", "http://api.example/", NULL, NULL, 0,
            false, &body, NULL);
        want("dns.rc", g_rc == 0);
        want("dns.status", agentc_http_status(g_h) == 204);
        want("dns.host", sent_has("Host: api.example\r\n"));
        agentc_http_free(g_h);
        agentc_buf_free(&body);
    }

    /* ------------------------------------------------- request head + POST */
    {
        AgcBuf body = { 0 };
        const char *payload = "{\"a\":1}";
        run("post", "tests/data/http_post.mock", "POST", "http://post.test:8080/echo",
            "X-Test: yes\r\n", payload, agentc_strlen(payload), false, &body, NULL);
        want("post.rc", g_rc == 0);
        want("post.body", buf_is(&body, "ok", 2));
        want("post.line", sent_has("POST /echo HTTP/1.1\r\n"));
        want("post.port", sent_has("Host: post.test:8080\r\n"));
        want("post.custom", sent_has("X-Test: yes\r\n"));
        want("post.len", sent_has("Content-Length: 7\r\n"));
        const AgcBuf *s = agentc_mock_sent();
        want("post.payload", s != NULL && s->len >= 7 && agentc_memeq(s->p + s->len - 7, payload, 7));
        agentc_http_free(g_h);
        agentc_buf_free(&body);
    }

    /* ------------------------------------ duplicate Content-Length rejected */
    {
        AgcBuf body = { 0 };
        run("dupcl", "tests/data/http_dup_cl.mock", "GET", "http://dup.test/", NULL, NULL,
            0, false, &body, NULL);
        want("dupcl.rc", g_rc == -71);
        want("dupcl.empty", body.len == 0);
        want("dupcl.err", agentc_streq(agentc_http_error(g_h), "bad response head"));
        agentc_http_free(g_h);
        agentc_buf_free(&body);
    }

    /* --------------------------------- header cap enforced on a complete head */
    {
        AgcBuf body = { 0 };
        run("bighead", "tests/data/http_bighead.mock", "GET", "http://big.test/", NULL, NULL,
            0, false, &body, NULL);
        want("bighead.rc", g_rc == -7);
        want("bighead.err", agentc_streq(agentc_http_error(g_h), "head too large"));
        agentc_http_free(g_h);
        agentc_buf_free(&body);
    }

    /* ------------------------------------ obs-fold continuation rejected */
    {
        AgcBuf body = { 0 };
        run("obsfold", "tests/data/http_obsfold.mock", "GET", "http://obsfold.test/", NULL,
            NULL, 0, false, &body, NULL);
        want("obsfold.rc", g_rc == -71);
        want("obsfold.empty", body.len == 0);
        want("obsfold.err", agentc_streq(agentc_http_error(g_h), "bad response head"));
        agentc_http_free(g_h);
        agentc_buf_free(&body);
    }

    /* ------------------------------------ whitespace before ':' rejected */
    {
        AgcBuf body = { 0 };
        run("wscolon", "tests/data/http_wscolon.mock", "GET", "http://wscolon.test/", NULL,
            NULL, 0, false, &body, NULL);
        want("wscolon.rc", g_rc == -71);
        want("wscolon.empty", body.len == 0);
        want("wscolon.err", agentc_streq(agentc_http_error(g_h), "bad response head"));
        agentc_http_free(g_h);
        agentc_buf_free(&body);
    }

    agentc_outf("fails=%d\n", fails);
    return fails;
}
