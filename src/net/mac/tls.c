/* tls.c — macOS AgcTls backend on Security.framework SecureTransport.
 *
 * No mbedTLS on macOS: SSLContextRef is a synchronous state machine, so it
 * maps onto the resumable agentc_tls_* contract cleanly. The IO callbacks sit on
 * top of the non-blocking agentc_net_recv/agentc_net_send and report errSSLWouldBlock,
 * which becomes -EAGAIN with agentc_tls_want() telling the caller which direction
 * to poll.
 *
 * Verification is opt-out via AGENTC_TLS_VERIFY, exactly as on Linux:
 *   - default: kSSLSessionOptionBreakOnServerAuth pauses the handshake at the
 *     server certificate; we evaluate SSLCopyPeerTrust() with
 *     SecTrustEvaluateWithError() and only then continue. SSLSetPeerDomainName
 *     gives the trust an SSL policy with the hostname, so evaluation also
 *     covers the name; AGENTC_TLS_NO_SNI skips the name (chain-only), matching
 *     mbedTLS without a hostname.
 *   - cleared: no break option, and SecureTransport performs no chain check.
 * Minimum protocol version is always TLS 1.2. ALPN is not used.
 *
 * Ownership mirrors Linux: agentc_tls_close() frees the context but not the fd.
 */
#include "net/net_internal.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecureTransport.h>
#include <Security/SecTrust.h>

#define TLS_EAGAIN 11
#define TLS_EACCES 13
#define TLS_EIO 5
#define TLS_EINVAL 22
#define TLS_EPIPE 32
#define TLS_ECONNRESET 104

struct AgcTls {
    SSLContextRef ctx;
    int fd;
    int want;      /* 0 none, 1 read, 2 write */
    int net_errno; /* last negative errno from agentc_net_send/recv */
    bool verify;
    char err[256];
};

static char g_last_error[256];

static void tls_set_err(AgcTls *t, const char *msg) {
    agentc_snprintf(t->err, sizeof t->err, "%s", msg);
}

static void tls_set_status(AgcTls *t, OSStatus st) {
    agentc_snprintf(t->err, sizeof t->err, "TLS: SecureTransport error %d", (int)st);
}

static void tls_set_trust_error(AgcTls *t, CFErrorRef err) {
    char desc[192];
    desc[0] = '\0';
    if (err != NULL) {
        CFStringRef s = CFErrorCopyDescription(err);
        if (s != NULL) {
            if (!CFStringGetCString(s, desc, sizeof desc, kCFStringEncodingUTF8))
                desc[0] = '\0';
            CFRelease(s);
        }
    }
    if (desc[0] != '\0')
        agentc_snprintf(t->err, sizeof t->err, "TLS: %s", desc);
    else
        tls_set_err(t, "TLS: certificate verification failed");
}

/* ------------------------------------------------------------- IO callbacks */
static OSStatus tls_read_cb(SSLConnectionRef conn, void *data, size_t *len) {
    AgcTls *t = (AgcTls *)conn;
    int n = agentc_net_recv(t->fd, data, *len);
    if (n > 0) {
        *len = (size_t)n;
        return errSecSuccess;
    }
    if (n == 0) {
        *len = 0;
        return errSSLClosedGraceful;
    }
    if (n == -TLS_EAGAIN) {
        t->want = 1;
        *len = 0;
        return errSSLWouldBlock;
    }
    t->net_errno = n;
    *len = 0;
    return errSSLClosedAbort;
}

static OSStatus tls_write_cb(SSLConnectionRef conn, const void *data, size_t *len) {
    AgcTls *t = (AgcTls *)conn;
    int n = agentc_net_send(t->fd, data, *len);
    if (n >= 0) {
        *len = (size_t)n;
        return errSecSuccess;
    }
    if (n == -TLS_EAGAIN) {
        t->want = 2;
        *len = 0;
        return errSSLWouldBlock;
    }
    t->net_errno = n;
    *len = 0;
    return errSSLClosedAbort;
}

/* --------------------------------------------------------------- verification */
static bool tls_check_trust(AgcTls *t) {
    SecTrustRef trust = NULL;
    if (SSLCopyPeerTrust(t->ctx, &trust) != errSecSuccess || trust == NULL) {
        tls_set_err(t, "TLS: peer certificate unavailable");
        return false;
    }
    CFErrorRef err = NULL;
    bool ok = SecTrustEvaluateWithError(trust, &err);
    if (!ok) tls_set_trust_error(t, err);
    if (err != NULL) CFRelease(err);
    CFRelease(trust);
    return ok;
}

/* ---------------------------------------------------------------- public API */
AgcTls *agentc_tls_new(int fd, const char *host, u16 port, unsigned flags) {
    (void)port; /* SNI uses the hostname */
    if (fd < 0) return NULL;
    g_last_error[0] = '\0';

    AgcTls *t = agentc_alloc(sizeof *t);
    t->fd = fd;
    t->verify = (flags & AGENTC_TLS_VERIFY) != 0;
    t->ctx = SSLCreateContext(kCFAllocatorDefault, kSSLClientSide, kSSLStreamType);
    if (t->ctx == NULL) {
        agentc_snprintf(g_last_error, sizeof g_last_error, "TLS: SSLCreateContext failed");
        agentc_free(t);
        return NULL;
    }

    OSStatus r = SSLSetIOFuncs(t->ctx, tls_read_cb, tls_write_cb);
    if (r == errSecSuccess) r = SSLSetConnection(t->ctx, t);
    if (r == errSecSuccess) r = SSLSetProtocolVersionMin(t->ctx, kTLSProtocol12);
    if (r == errSecSuccess && host != NULL && host[0] != '\0' && !(flags & AGENTC_TLS_NO_SNI))
        r = SSLSetPeerDomainName(t->ctx, host, agentc_strlen(host));
    if (r == errSecSuccess && t->verify)
        r = SSLSetSessionOption(t->ctx, kSSLSessionOptionBreakOnServerAuth, true);
    if (r != errSecSuccess) {
        agentc_snprintf(g_last_error, sizeof g_last_error,
                    "TLS: SecureTransport setup error %d", (int)r);
        CFRelease(t->ctx);
        agentc_free(t);
        return NULL;
    }
    return t;
}

int agentc_tls_handshake(AgcTls *t) {
    if (t == NULL || t->ctx == NULL) return -TLS_EINVAL;
    for (;;) {
        t->want = 0;
        t->net_errno = 0;
        OSStatus r = SSLHandshake(t->ctx);
        if (r == errSecSuccess) return 0;
        if (r == errSSLWouldBlock) return -TLS_EAGAIN;
        if (r == errSSLServerAuthCompleted) {
            if (t->verify && !tls_check_trust(t)) return -TLS_EACCES;
            continue; /* certificate accepted: keep handshaking */
        }
        if (r == errSSLClosedGraceful || r == errSSLClosedNoNotify) {
            tls_set_err(t, "TLS: peer closed during handshake");
            return t->net_errno != 0 ? t->net_errno : -TLS_ECONNRESET;
        }
        if (r == errSSLClosedAbort && t->net_errno != 0) {
            tls_set_status(t, r);
            return t->net_errno;
        }
        tls_set_status(t, r);
        return -TLS_EIO;
    }
}

int agentc_tls_read(AgcTls *t, void *p, size_t n) {
    if (t == NULL || t->ctx == NULL) return -TLS_EINVAL;
    t->want = 0;
    t->net_errno = 0;
    size_t got = 0;
    OSStatus r = SSLRead(t->ctx, p, n, &got);
    if (r == errSecSuccess) return got > 0 ? (int)got : -TLS_EAGAIN;
    if (r == errSSLWouldBlock) return got > 0 ? (int)got : -TLS_EAGAIN;
    if (r == errSSLClosedGraceful || r == errSSLClosedNoNotify) return 0;
    if (r == errSSLClosedAbort) {
        tls_set_status(t, r);
        return t->net_errno != 0 ? t->net_errno : -TLS_ECONNRESET;
    }
    tls_set_status(t, r);
    return -TLS_EIO;
}

int agentc_tls_write(AgcTls *t, const void *p, size_t n) {
    if (t == NULL || t->ctx == NULL) return -TLS_EINVAL;
    t->want = 0;
    t->net_errno = 0;
    size_t wrote = 0;
    OSStatus r = SSLWrite(t->ctx, p, n, &wrote);
    if (r == errSecSuccess) return (int)wrote;
    if (r == errSSLWouldBlock) return wrote > 0 ? (int)wrote : -TLS_EAGAIN;
    if (r == errSSLClosedAbort) {
        tls_set_status(t, r);
        return t->net_errno != 0 ? t->net_errno : -TLS_EPIPE;
    }
    tls_set_status(t, r);
    return -TLS_EIO;
}

void agentc_tls_close(AgcTls *t) {
    if (t == NULL) return;
    if (t->ctx != NULL) {
        (void)SSLClose(t->ctx); /* best effort; the fd stays the caller's */
        CFRelease(t->ctx);
    }
    agentc_free(t);
}

const char *agentc_tls_error(AgcTls *t) {
    if (t != NULL && t->err[0] != '\0') return t->err;
    if (g_last_error[0] != '\0') return g_last_error;
    return "";
}

int agentc_tls_want(const AgcTls *t) { return t == NULL ? 0 : t->want; }
