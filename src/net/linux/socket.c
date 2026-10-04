/* socket.c — Layer 1 Linux backend: raw IPv4 TCP/UDP sockets.
 *
 * Direct x86-64 syscalls (the same convention as src/plat/linux/sys.c): every
 * entry point returns >= 0 or a negative linux errno. Sockets are created
 * non-blocking and close-on-exec; -EAGAIN is a normal outcome, not an error.
 *
 * agentc_net_connect() performs exactly one connect(2): it returns 0 when the
 * connection completed immediately and -EINPROGRESS when the caller must poll
 * for POLLOUT and then check agentc_net_so_error(). deadline_ms is informational
 * and never used for an internal wait.
 *
 * Send/recv use sendto(2)/recvfrom(2) with MSG_NOSIGNAL so a peer reset never
 * raises SIGPIPE, which matters in a process without libc signal handling.
 */
#include "net/net_internal.h"

#include "plat/linux/syscall.h"

/* ---------------------------------------------------------------- constants */
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000
#define IPPROTO_TCP 6
#define SOL_SOCKET 1
#define SO_ERROR 4
#define TCP_NODELAY 1
#define MSG_NOSIGNAL 0x4000

struct agentc_sockaddr_in {
    u16 family;
    u16 port_be;
    u32 addr_be;
    u8 zero[8];
} __attribute__((packed));

/* Byte-swap a host-order port into the sockaddr network order. */
static inline u16 port_htons(u16 port) { return (u16)((port << 8) | (port >> 8)); }

/* ------------------------------------------------------------------- API */
int agentc_net_init(void) { return 0; }

int agentc_net_set_nodelay(int fd) {
    int one = 1;
    long r = linux_sc5(SYS_setsockopt, fd, IPPROTO_TCP, TCP_NODELAY, (long)&one, sizeof(one));
    return r < 0 ? (int)r : 0;
}

int agentc_net_socket(void) {
    long fd = linux_sc3(SYS_socket, AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return (int)fd;
    int r = agentc_net_set_nodelay((int)fd);
    if (r < 0) {
        linux_sc1(SYS_close, fd);
        return r;
    }
    return (int)fd;
}

int agentc_net_udp_socket(void) {
    long fd = linux_sc3(SYS_socket, AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    return (int)fd;
}

int agentc_net_connect(int fd, u32 ip_be, u16 port, int deadline_ms) {
    (void)deadline_ms; /* connect is attempted once; poll and so_error follow */
    struct agentc_sockaddr_in sa;
    agentc_memset(&sa, 0, sizeof sa);
    sa.family = AF_INET;
    sa.port_be = port_htons(port);
    sa.addr_be = ip_be;
    long r = linux_sc3(SYS_connect, fd, (long)&sa, sizeof sa);
    return r < 0 ? (int)r : 0;
}

int agentc_net_send(int fd, const void *p, size_t n) {
    long r = linux_sc6(SYS_sendto, fd, (long)p, (long)n, MSG_NOSIGNAL, 0, 0);
    return (int)r;
}

int agentc_net_recv(int fd, void *p, size_t n) {
    long r = linux_sc6(SYS_recvfrom, fd, (long)p, (long)n, 0, 0, 0);
    return (int)r;
}

void agentc_net_close(int fd) {
    if (fd >= 0) linux_sc1(SYS_close, fd);
}

int agentc_net_so_error(int fd) {
    int err = 0;
    int len = sizeof(err);
    long r = linux_sc5(SYS_getsockopt, fd, SOL_SOCKET, SO_ERROR, (long)&err, (long)&len);
    if (r < 0) return (int)r;
    return err ? -err : 0;
}
