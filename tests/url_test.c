/* url_test.c — agentc_url_parse cases: schemes, ports, paths, queries, errors. */
#include "net/net_internal.h"

static int fails;

static void ok_url(const char *url, const char *scheme, const char *host, u16 port,
                   const char *path) {
    AgcUrl u;
    int rc = agentc_url_parse(url, &u);
    bool good = rc == 0 && agentc_streq(u.https ? "https" : "http", scheme) &&
                agentc_streq(u.host, host) && u.port == port && agentc_streq(u.path, path);
    agentc_outf("%s=%d\n", url, good ? 1 : 0);
    if (!good) {
        fails = 1;
        if (rc == 0) agentc_outf("  got %s %s %u %s\n", u.https ? "https" : "http", u.host,
                             (unsigned)u.port, u.path);
        else agentc_outf("  rc=%d\n", rc);
    }
}

static void bad_url(const char *url) {
    AgcUrl u;
    int rc = agentc_url_parse(url, &u);
    agentc_outf("bad:%s=%d\n", url, rc);
    if (rc == 0) fails = 1;
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    ok_url("http://example.com", "http", "example.com", 80, "/");
    ok_url("https://example.com/", "https", "example.com", 443, "/");
    ok_url("HTTPS://Example.COM:8443/a/b?x=1#frag", "https", "Example.COM", 8443,
           "/a/b?x=1");
    ok_url("http://127.0.0.1:8080/hello", "http", "127.0.0.1", 8080, "/hello");
    ok_url("http://user:pw@host.test:81/p?q=2", "http", "host.test", 81, "/p?q=2");
    ok_url("http://[::1]:9000/x", "http", "::1", 9000, "/x");
    ok_url("http://h?k=v", "http", "h", 80, "/?k=v");
    ok_url("HTTP://UPPER.example", "http", "UPPER.example", 80, "/");

    bad_url("ftp://example.com/");
    bad_url("example.com/path");
    bad_url("http://");
    bad_url("http://:80/");
    bad_url("http://host:0/");
    bad_url("http://host:70000/");
    bad_url("http://host:abc/");
    bad_url("http://host:/");
    bad_url("http://[::1/");

    agentc_outf("fails=%d\n", fails);
    return fails;
}
