/* net/mac/socket.c — Layer 1 macOS backend: BSD sockets over libSystem.
 *
 * Sockets are created blocking, then switched to O_NONBLOCK + FD_CLOEXEC and
 * SO_NOSIGPIPE (the Darwin stand-in for Linux MSG_NOSIGNAL). Darwin errors are
 * translated through mac_errno(); every entry point returns >= 0 or a negative
 * linux errno, and -EAGAIN / -EINPROGRESS are normal outcomes.
 */
#include "net/net_internal.h"
#include "plat/mac/mac_internal.h"

#include <netinet/in.h>
#include <netinet/tcp.h>

static int mac_sock_err(void) { return mac_errno(errno); }

int agentc_net_init(void) { return 0; }

int agentc_net_set_nodelay(int fd) {
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0 ? 0
                                                                          : mac_sock_err();
}

static int set_nonblock_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return mac_sock_err();
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) return mac_sock_err();
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) return mac_sock_err();
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    return 0;
}

int agentc_net_socket(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return mac_sock_err();
    int r = set_nonblock_cloexec(fd);
    if (r == 0) r = agentc_net_set_nodelay(fd);
    if (r < 0) {
        close(fd);
        return r;
    }
    return fd;
}

int agentc_net_udp_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return mac_sock_err();
    int r = set_nonblock_cloexec(fd);
    if (r < 0) {
        close(fd);
        return r;
    }
    return fd;
}

static u16 port_htons(u16 port) { return (u16)((port << 8) | (port >> 8)); }

int agentc_net_connect(int fd, u32 ip_be, u16 port, int deadline_ms) {
    (void)deadline_ms; /* connect is attempted once; poll and so_error follow */
    struct sockaddr_in sa;
    agentc_memset(&sa, 0, sizeof sa);
    sa.sin_len = (u8)sizeof sa; /* BSD wants the length filled in */
    sa.sin_family = AF_INET;
    sa.sin_port = port_htons(port);
    agentc_memcpy(&sa.sin_addr, &ip_be, 4);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) return mac_sock_err();
    return 0;
}

int agentc_net_send(int fd, const void *p, size_t n) {
    ssize_t r = send(fd, p, n, 0);
    return r < 0 ? mac_sock_err() : (int)r;
}

int agentc_net_recv(int fd, void *p, size_t n) {
    ssize_t r = recv(fd, p, n, 0);
    return r < 0 ? mac_sock_err() : (int)r;
}

void agentc_net_close(int fd) {
    if (fd >= 0) close(fd);
}

int agentc_net_so_error(int fd) {
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return mac_sock_err();
    return err != 0 ? mac_errno(err) : 0;
}

int agentc_net_poll(int fd, short events, int timeout_ms) {
    struct os_pollfd p = { fd, events, 0 };
    return os_poll(&p, 1, timeout_ms);
}
