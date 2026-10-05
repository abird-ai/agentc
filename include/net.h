/* net.h — Layer 1: sockets, DNS and TLS.
 *
 * Implementations (link-time): src/net/linux/, src/net/mac/,
 * src/net/win/, src/net/mock.c (tests; see the format at the top of mock.c).
 *
 * All sockets are non-blocking; -EAGAIN is a normal outcome. TLS handshakes are
 * resumable: call agentc_tls_handshake() whenever the fd becomes ready and keep
 * going on -EAGAIN.
 */
#ifndef AGENTC_NET_H
#define AGENTC_NET_H

#include "agentc.h"

int agentc_net_init(void);
int agentc_net_socket(void); /* ipv4 tcp, nonblock|cloexec */
int agentc_net_connect(int fd, u32 ip_be, u16 port, int deadline_ms); /* 0 | -EINPROGRESS */
int agentc_net_send(int fd, const void *p, size_t n);                 /* n | -EAGAIN | -errno */
int agentc_net_recv(int fd, void *p, size_t n); /* n | 0 eof | -EAGAIN | -errno */
void agentc_net_close(int fd);
int agentc_net_so_error(int fd);
int agentc_net_set_nodelay(int fd);
/* Wait until `fd` is ready for `events` (the OS_POLLIN/OS_POLLOUT bits from
 * plat.h), or timeout_ms elapses (negative waits forever). Returns the os_poll
 * result: > 0 ready, 0 timeout, -errno. The net layer owns polling its own
 * descriptors: the replay backend (net/mock.c) has no kernel object and reports
 * readiness immediately so the caller retries the read. */
int agentc_net_poll(int fd, short events, int timeout_ms);

/* ---------------------------------------- framed-protocol transport (Axis C) */
/* Unix-domain and loopback-TCP listeners for `agentc serve` (C-D3). All sockets
 * are non-blocking and close-on-exec; -EAGAIN is a normal accept outcome. The
 * unix listener creates the socket 0600 (the caller owns a 0700 directory).
 * A stale socket file is recovered: a bind that fails with -EADDRINUSE is
 * probed with a connect; only when no live server answers is the path unlinked
 * and the bind retried. */
int agentc_net_unix_listen(const char *path, int backlog);   /* -> fd | -errno */
int agentc_net_unix_connect(const char *path);               /* -> fd | -EINPROGRESS | -errno */
int agentc_net_tcp_listen(u32 ip_be, u16 port, int backlog); /* -> fd | -errno */
int agentc_net_accept(int lfd);                              /* -> fd | -EAGAIN | -errno */
/* "unix:<path>" or "tcp:<ip>:<port>"; out is NUL-terminated. -> 0 | -errno. */
int agentc_net_local_addr(int fd, char *out, size_t cap);
int agentc_net_unlink_socket(const char *path); /* -> 0 | -errno */

/* host resolution: dotted-quad fast path, /etc/hosts, resolv.conf UDP */
struct agentc_ip4 {
    u8 b[4];
};
int agentc_net_dns(const char *host, struct agentc_ip4 *out, int deadline_ms);
bool agentc_net_is_ip4(const char *host, struct agentc_ip4 *out);

/* TLS */
typedef struct AgcTls AgcTls;
enum {
    AGENTC_TLS_VERIFY = 1u << 0, /* default ON; clearing it is --insecure */
    /* Suppress SNI only. On stacks where the SNI name also drives the
     * certificate name check (mbedTLS set_hostname, SecureTransport
     * SSLSetPeerDomainName, SChannel pwszServerName) this skips identity
     * verification too, so it is an explicit caller opt-in; the wire layer
     * never sets it for IP literals. */
    AGENTC_TLS_NO_SNI = 1u << 1,
    AGENTC_TLS_MIN_1_2 = 1u << 2, /* default */
};
AgcTls *agentc_tls_new(int fd, const char *host, u16 port, unsigned flags);
int agentc_tls_handshake(AgcTls *t);               /* 0 | -EAGAIN | -errno */
int agentc_tls_read(AgcTls *t, void *p, size_t n); /* n | 0 eof | -EAGAIN | -errno */
int agentc_tls_write(AgcTls *t, const void *p, size_t n);
void agentc_tls_close(AgcTls *t);
const char *agentc_tls_error(AgcTls *t); /* human-readable, never NULL */
/* Direction to poll after agentc_tls_handshake/read/write returned -EAGAIN:
 * 0 none, 1 wait readable, 2 wait writable. Public for the served listener
 * loop (P5 C3). */
int agentc_tls_want(const AgcTls *t);

/* --------------------------------------------------- TLS server (P5 C3) */
/* Create a TLS *server* endpoint over `fd` from in-memory PEM certificate and
 * private-key buffers (no filesystem access; the caller reads the files). The
 * handshake is resumable via agentc_tls_handshake/agentc_tls_want exactly like
 * the client. Returns NULL on failure and, when `err` is non-NULL, writes a
 * bounded diagnostic. Client certificates are not requested (v1). */
AgcTls *agentc_tls_server_new(int fd, const void *cert, size_t cert_len, const void *key,
                              size_t key_len, char *err, size_t errcap);
/* True when the linked backend can serve TLS. On macOS/Windows the vendored
 * mbedTLS server is not built, so `agentc serve --tls` fails closed there. */
bool agentc_tls_server_supported(void);

#endif /* AGENTC_NET_H */
