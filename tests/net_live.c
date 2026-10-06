/* net_live.c — tiny live client used by tests/net.sh against tests/httpd.py.
 *
 * It is also compiled (with the mock backend) by ./build.sh test and run with
 * no arguments, in which case it prints an offline notice so tests/run.sh
 * stays deterministic.
 *
 * usage: net_live [--data BODY] [--header "Name: value"] [--sse] URL
 *   prints "status N", the response body (or streamed [event] data lines for
 *   --sse) and exits 0 for 2xx/3xx, 1 for other statuses, 2 for transport
 *   errors. https verification follows AGENTC_TLS_VERIFY (default on; set
 *   AGENTC_TLS_VERIFY=0 for a self-signed test server).
 */
#include "net/net_internal.h"

static int body_cb(void *ud, const void *p, size_t n) {
    AgcBuf *b = ud;
    agentc_buf_push(b, p, n);
    return 0;
}

static int event_cb(void *ud, const AgcSseEvent *ev) {
    (void)ud;
    agentc_outf("[%s] ", ev->event[0] != '\0' ? ev->event : "message");
    agentc_out(ev->data, ev->data_len);
    agentc_out_nl();
    return 0;
}

static int sse_cb(void *ud, const void *p, size_t n) {
    return agentc_sse_feed((AgcSse *)ud, p, n, event_cb, NULL);
}

int agentc_main(int argc, char **argv) {
    if (argc < 2) {
        agentc_outs("net_live: offline (live tests are run by tests/net.sh)\n");
        return 0;
    }

    const char *url = NULL, *data = NULL, *hdr = NULL;
    bool sse = false;
    for (int i = 1; i < argc; i++) {
        if (agentc_streq(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (agentc_streq(argv[i], "--header") && i + 1 < argc) hdr = argv[++i];
        else if (agentc_streq(argv[i], "--sse")) sse = true;
        else url = argv[i];
    }
    if (url == NULL) {
        agentc_outs("usage: net_live [--data BODY] [--header \"Name: value\"] [--sse] URL\n");
        return 2;
    }

    AgcHttp *h = agentc_http_new(data != NULL ? "POST" : "GET", url, hdr, data,
                            data != NULL ? agentc_strlen(data) : 0);
    if (h == NULL) {
        agentc_outs("error: bad URL\n");
        return 2;
    }

    int rc;
    if (sse) {
        AgcSse s;
        agentc_sse_init(&s);
        rc = agentc_http_run(h, sse_cb, &s, 10000);
        agentc_sse_free(&s);
    } else {
        AgcBuf body = { 0 };
        rc = agentc_http_run(h, body_cb, &body, 10000);
        if (rc == 0) {
            agentc_outf("status %d\n", agentc_http_status(h));
            agentc_out(body.p, body.len);
            if (body.len == 0 || ((const char *)body.p)[body.len - 1] != '\n') agentc_out_nl();
        }
        agentc_buf_free(&body);
    }
    if (rc != 0) {
        agentc_outf("error rc=%d %s\n", rc, agentc_http_error(h));
        agentc_http_free(h);
        return 2;
    }
    if (sse) agentc_outf("status %d\n", agentc_http_status(h));
    int st = agentc_http_status(h);
    agentc_http_free(h);
    if (st >= 500) return 1;
    return (st >= 200 && st < 400) ? 0 : 1;
}
