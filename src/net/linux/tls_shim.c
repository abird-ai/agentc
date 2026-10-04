/* tls_shim.c — Linux AgcTls backend on vendored mbedTLS 3.6 (MIT glue).
 *
 * Shape: one process-wide CA chain + CTR-DRBG, a BIO over
 * agentc_net_send/agentc_net_recv and map_result() translating mbedTLS codes to
 * the 0 | n | -EAGAIN | -errno contract. Notes on the interface choices:
 *
 *   - agentc_net_send/agentc_net_recv return int, so a partial write is a
 *     positive count and only -EAGAIN means "retry";
 *   - the fd and host arrive together in agentc_tls_new(fd, host, port, flags);
 *   - entropy is os_random() from include/plat.h (mbedtls_hardware_poll is
 *     wired in third_party/mbedtls_glue.c);
 *   - the Mozilla CA bundle is embedded with C23 #embed, so the binary is
 *     self-contained and verification needs no files at runtime;
 *   - the CA/DRBG are initialised once, then reused by every connection.
 *
 * Ownership: agentc_tls_close() frees the TLS state but never closes the fd; the
 * caller (agentc_http) owns the socket. The handshake is resumable: every -EAGAIN
 * is coupled with agentc_tls_want() so the caller can poll the right direction.
 *
 * CA verification is ON when AGENTC_TLS_VERIFY is set; clearing the flag is the
 * --insecure mode. mbedTLS errors are reported through agentc_tls_error().
 */
#include "net/net_internal.h"

#define MBEDTLS_CONFIG_FILE "mbedtls_agentc_config.h"
#include "mbedtls/build_info.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

/* Provided by third_party/mbedtls_glue.c (compiled with the mbedTLS flags). */
int agentc_platform_setup(void);

/* Vendored Mozilla CA bundle, embedded at compile time (C23 #embed). The
 * explicit NUL matters: mbedtls_x509_crt_parse() only treats the input as PEM
 * when the buffer is NUL-terminated, and sizeof() then includes it. */
static const unsigned char k_ca_bundle[] = {
#embed "../../../third_party/cacert.pem"
, 0
};

#define TLS_EAGAIN 11
#define TLS_EACCES 13
#define TLS_EIO 5
#define TLS_EINVAL 22

struct AgcTls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    int fd;
    bool handshake_done;
    int want;       /* 0 none, 1 read, 2 write */
    int net_errno;  /* last negative errno from agentc_net_send/recv */
    char err[512];
};

/* Process-wide state; agentc is single-threaded. */
static mbedtls_x509_crt g_ca;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static int g_ready;
static char g_init_error[256];

static void set_error_mbedtls(AgcTls *c, int ret) {
    static const char prefix[] = "TLS: ";
    if (c == NULL) return;
    size_t i = 0;
    while (prefix[i] != '\0' && i + 1 < sizeof(c->err)) {
        c->err[i] = prefix[i];
        i++;
    }
    char tmp[192];
    mbedtls_strerror(ret, tmp, sizeof(tmp));
    for (size_t j = 0; tmp[j] != '\0' && i + 1 < sizeof(c->err); j++) c->err[i++] = tmp[j];
    c->err[i] = '\0';
}

static int global_init(void) {
    static const unsigned char pers[] = "agentc-mbedtls";
    if (g_ready) return 0;

    agentc_platform_setup();
    mbedtls_x509_crt_init(&g_ca);
    int ret = mbedtls_x509_crt_parse(&g_ca, k_ca_bundle, sizeof(k_ca_bundle));
    if (ret < 0) {
        mbedtls_strerror(ret, g_init_error, sizeof(g_init_error));
        mbedtls_x509_crt_free(&g_ca);
        return -1;
    }

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    ret = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, pers,
                                sizeof(pers) - 1);
    if (ret != 0) {
        mbedtls_strerror(ret, g_init_error, sizeof(g_init_error));
        mbedtls_entropy_free(&g_entropy);
        mbedtls_ctr_drbg_free(&g_drbg);
        return -1;
    }

    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS) {
        agentc_snprintf(g_init_error, sizeof(g_init_error), "TLS: psa_crypto_init %ld",
                    (long)ps);
        mbedtls_entropy_free(&g_entropy);
        mbedtls_ctr_drbg_free(&g_drbg);
        mbedtls_x509_crt_free(&g_ca);
        return -1;
    }

    g_ready = 1;
    return 0;
}

/* -------------------------------------------------------------- BIO hooks */
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    AgcTls *c = ctx;
    int n = agentc_net_send(c->fd, buf, len);
    if (n >= 0) return n;
    if (n == -TLS_EAGAIN) return MBEDTLS_ERR_SSL_WANT_WRITE;
    c->net_errno = n;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    AgcTls *c = ctx;
    int n = agentc_net_recv(c->fd, buf, len);
    if (n > 0) return n;
    if (n == 0) return 0; /* EOF; mbedTLS turns it into CONN_EOF */
    if (n == -TLS_EAGAIN) return MBEDTLS_ERR_SSL_WANT_READ;
    c->net_errno = n;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

enum { TLS_OP_HANDSHAKE, TLS_OP_READ, TLS_OP_WRITE };

static int map_result_op(AgcTls *c, int ret, int op) {
    if (ret >= 0) return ret;
    switch (ret) {
    case MBEDTLS_ERR_SSL_WANT_READ:
        c->want = 1;
        return -TLS_EAGAIN;
    case MBEDTLS_ERR_SSL_WANT_WRITE:
        c->want = 2;
        return -TLS_EAGAIN;
    case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
    case MBEDTLS_ERR_SSL_CONN_EOF:
        /* Only a read may treat EOF as a clean end of stream; a handshake or a
         * write that ends here is a connection reset, not success. */
        if (op == TLS_OP_READ) return 0;
        agentc_snprintf(c->err, sizeof c->err, "%s",
                    op == TLS_OP_HANDSHAKE ? "peer closed during the handshake"
                                           : "peer closed the connection");
        return -104;                             /* ECONNRESET */
    case MBEDTLS_ERR_NET_SEND_FAILED:
    case MBEDTLS_ERR_NET_RECV_FAILED:
    case MBEDTLS_ERR_NET_CONN_RESET:
        return c->net_errno != 0 ? c->net_errno : -TLS_EIO;
    default:
        set_error_mbedtls(c, ret);
        return -TLS_EIO;
    }
}

static int map_result(AgcTls *c, int ret) { return map_result_op(c, ret, TLS_OP_READ); }

/* ------------------------------------------------------------- public API */
AgcTls *agentc_tls_new(int fd, const char *host, u16 port, unsigned flags) {
    (void)port; /* SNI uses the hostname */
    if (fd < 0) return NULL;
    if (global_init() != 0) return NULL;

    AgcTls *c = agentc_alloc(sizeof(*c));
    c->fd = fd;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);

    int ret = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        set_error_mbedtls(c, ret);
        goto fail;
    }

    mbedtls_ssl_conf_authmode(&c->conf, (flags & AGENTC_TLS_VERIFY)
                                           ? MBEDTLS_SSL_VERIFY_REQUIRED
                                           : MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_ca_chain(&c->conf, &g_ca, NULL);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &g_drbg);
    /* the documented minimum is TLS 1.2 unless the caller says otherwise */
    if (flags & AGENTC_TLS_MIN_1_2)
        mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);

    ret = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (ret != 0) {
        set_error_mbedtls(c, ret);
        goto fail;
    }

    if (host != NULL && host[0] != '\0' && !(flags & AGENTC_TLS_NO_SNI)) {
        ret = mbedtls_ssl_set_hostname(&c->ssl, host);
        if (ret != 0) {
            set_error_mbedtls(c, ret);
            goto fail;
        }
    }

    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    return c;

fail:
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    agentc_free(c);
    return NULL;
}

int agentc_tls_handshake(AgcTls *t) {
    if (t == NULL || t->fd < 0) return -TLS_EINVAL;
    t->want = 0;
    t->net_errno = 0;

    int ret = mbedtls_ssl_handshake(&t->ssl);
    if (ret == 0) {
        t->handshake_done = true;
        uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
        if (flags != 0) {
            mbedtls_x509_crt_verify_info(t->err, sizeof(t->err), "TLS: ", flags);
            return -TLS_EACCES;
        }
        return 0;
    }
    if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
        mbedtls_x509_crt_verify_info(t->err, sizeof(t->err), "TLS: ", flags);
        t->want = 0;
        return -TLS_EACCES;
    }
    return map_result_op(t, ret, TLS_OP_HANDSHAKE);
}

int agentc_tls_read(AgcTls *t, void *p, size_t n) {
    if (t == NULL || t->fd < 0) return -TLS_EINVAL;
    t->want = 0;
    t->net_errno = 0;
    return map_result(t, mbedtls_ssl_read(&t->ssl, p, n));
}

int agentc_tls_write(AgcTls *t, const void *p, size_t n) {
    if (t == NULL || t->fd < 0) return -TLS_EINVAL;
    t->want = 0;
    t->net_errno = 0;
    return map_result_op(t, mbedtls_ssl_write(&t->ssl, p, n), TLS_OP_WRITE);
}

void agentc_tls_close(AgcTls *t) {
    if (t == NULL) return;
    if (t->handshake_done) {
        /* best effort: tell the peer we are done; never blocks, never fails */
        (void)mbedtls_ssl_close_notify(&t->ssl);
    }
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    agentc_free(t);
}

const char *agentc_tls_error(AgcTls *t) {
    if (t != NULL && t->err[0] != '\0') return t->err;
    if (!g_ready && g_init_error[0] != '\0') return g_init_error;
    return "";
}

int agentc_tls_want(const AgcTls *t) { return t == NULL ? 0 : t->want; }
