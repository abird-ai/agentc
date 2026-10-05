/* unix.c — Layer 1 macOS backend: framed-protocol listeners (P5 Axis C).
 * BSD sockets over libSystem; errors are negative linux errno. See
 * src/net/linux/unix.c for the contract. */
#include "net/net_internal.h"
#include "plat/mac/mac_internal.h"

static int mac_sock_err(void) {
    return mac_errno(errno);
}

static int set_nonblock_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return mac_sock_err();
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0)
        return mac_sock_err();
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
        return mac_sock_err();
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    return 0;
}

int agentc_net_unix_connect(const char *path) {
    if (!path || !path[0] || agentc_strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
        return -36;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return mac_sock_err();
    struct sockaddr_un sa;
    agentc_memset(&sa, 0, sizeof sa);
    sa.sun_len = (u8)sizeof sa;
    sa.sun_family = AF_UNIX;
    agentc_snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int e = mac_sock_err();
        close(fd);
        return e;
    }
    int r = set_nonblock_cloexec(fd);
    if (r < 0) {
        close(fd);
        return r;
    }
    return fd;
}

int agentc_net_unix_listen(const char *path, int backlog) {
    if (!path || !path[0] || agentc_strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
        return -36;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return mac_sock_err();
    struct sockaddr_un sa;
    agentc_memset(&sa, 0, sizeof sa);
    sa.sun_len = (u8)sizeof sa;
    sa.sun_family = AF_UNIX;
    agentc_snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int e = mac_sock_err();
        if (e == -98) { /* EADDRINUSE: recover a stale socket */
            int probe = agentc_net_unix_connect(path);
            if (probe >= 0) {
                close(probe);
                close(fd);
                return -98;
            }
            (void)agentc_net_unlink_socket(path);
            if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0)
                e = mac_sock_err();
            else
                e = 0;
        }
        if (e != 0) {
            close(fd);
            return e;
        }
    }
    if (listen(fd, backlog) != 0) {
        int e = mac_sock_err();
        close(fd);
        return e;
    }
    (void)os_chmod(path, 0600);
    int r = set_nonblock_cloexec(fd);
    if (r < 0) {
        close(fd);
        return r;
    }
    return fd;
}

int agentc_net_tcp_listen(u32 ip_be, u16 port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return mac_sock_err();
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    agentc_memset(&sa, 0, sizeof sa);
    sa.sin_len = (u8)sizeof sa;
    sa.sin_family = AF_INET;
    sa.sin_port = (u16)((port << 8) | (port >> 8));
    agentc_memcpy(&sa.sin_addr, &ip_be, 4);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, backlog) != 0) {
        int e = mac_sock_err();
        close(fd);
        return e;
    }
    int r = set_nonblock_cloexec(fd);
    if (r < 0) {
        close(fd);
        return r;
    }
    return fd;
}

int agentc_net_accept(int lfd) {
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0)
        return mac_sock_err();
    int r = set_nonblock_cloexec(fd);
    if (r < 0) {
        close(fd);
        return r;
    }
    return fd;
}

int agentc_net_local_addr(int fd, char *out, size_t cap) {
    if (!out || cap == 0)
        return -22;
    union {
        struct sockaddr sa;
        struct sockaddr_un un;
        struct sockaddr_in in;
    } u;
    socklen_t len = sizeof u;
    if (getsockname(fd, &u.sa, &len) != 0)
        return mac_sock_err();
    if (u.sa.sa_family == AF_UNIX) {
        int w = agentc_snprintf(out, cap, "unix:%s", u.un.sun_path);
        return (w < 0 || (size_t)w >= cap) ? -36 : 0;
    }
    if (u.sa.sa_family == AF_INET) {
        const u8 *b = (const u8 *)&u.in.sin_addr;
        u16 port = (u16)((u.in.sin_port >> 8) | (u.in.sin_port << 8));
        int w =
            agentc_snprintf(out, cap, "tcp:%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], (unsigned)port);
        return (w < 0 || (size_t)w >= cap) ? -36 : 0;
    }
    return -22;
}

int agentc_net_unlink_socket(const char *path) {
    if (!path || !path[0])
        return -22;
    return unlink(path) == 0 ? 0 : mac_sock_err();
}
