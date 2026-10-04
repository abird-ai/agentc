/* net_internal.h — private helpers shared by the agentc net/wire implementations. */
#ifndef AGENTC_NET_INTERNAL_H
#define AGENTC_NET_INTERNAL_H

#include "agentc.h"
#include "plat.h"
#include "wire.h"

/* Monotonic milliseconds. */
static inline i64 agentc_net_now_ms(void) { return os_now_ns(OS_CLOCK_MONOTONIC) / 1000000; }

/* ip_be helpers: the wire word whose memory bytes are already network order. */
static inline u32 agentc_ip4_word(const struct agentc_ip4 *ip) {
    u32 w;
    agentc_memcpy(&w, ip->b, 4);
    return w;
}
static inline void agentc_ip4_from_word(struct agentc_ip4 *ip, u32 w) {
    agentc_memcpy(ip->b, &w, 4);
}

/* ---------------------------------------------------------------- backends */
/* TLS poll direction after -EAGAIN: 0 none, 1 wait readable, 2 wait writable.
 * Linux: src/net/linux/tls_shim.c. Mock: src/net/mock.c. */
int agentc_tls_want(const AgcTls *t);

/* UDP socket used by the resolver (src/net/linux/socket.c). */
int agentc_net_udp_socket(void);

/* Scripted replay backend (tests link src/net/mock.c instead of net/linux).
 * The script-file format is documented at the top of src/net/mock.c. */
int agentc_mock_load(const char *path);
void agentc_mock_reset(void);
void agentc_mock_set_dns(const char *host, const struct agentc_ip4 *ip);
const AgcBuf *agentc_mock_sent(void);

#endif /* AGENTC_NET_INTERNAL_H */
