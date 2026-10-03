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
int agentc_net_socket(void);                               /* ipv4 tcp, nonblock|cloexec */
int agentc_net_connect(int fd, u32 ip_be, u16 port, int deadline_ms);  /* 0 | -EINPROGRESS */
int agentc_net_send(int fd, const void *p, size_t n);      /* n | -EAGAIN | -errno */
int agentc_net_recv(int fd, void *p, size_t n);            /* n | 0 eof | -EAGAIN | -errno */
void agentc_net_close(int fd);
int agentc_net_so_error(int fd);
int agentc_net_set_nodelay(int fd);

/* host resolution: dotted-quad fast path, /etc/hosts, resolv.conf UDP */
struct agentc_ip4 { u8 b[4]; };
int agentc_net_dns(const char *host, struct agentc_ip4 *out, int deadline_ms);
bool agentc_net_is_ip4(const char *host, struct agentc_ip4 *out);

/* TLS */
typedef struct AgcTls AgcTls;
enum {
    AGENTC_TLS_VERIFY   = 1u << 0,   /* default ON; clearing it is --insecure */
    AGENTC_TLS_NO_SNI   = 1u << 1,
    AGENTC_TLS_MIN_1_2  = 1u << 2,   /* default */
};
AgcTls *agentc_tls_new(int fd, const char *host, u16 port, unsigned flags);
int agentc_tls_handshake(AgcTls *t);                        /* 0 | -EAGAIN | -errno */
int agentc_tls_read(AgcTls *t, void *p, size_t n);         /* n | 0 eof | -EAGAIN | -errno */
int agentc_tls_write(AgcTls *t, const void *p, size_t n);
void agentc_tls_close(AgcTls *t);
const char *agentc_tls_error(AgcTls *t);                   /* human-readable, never NULL */

#endif /* AGENTC_NET_H */
