/* unix.c — Layer 1 Linux backend: framed-protocol listeners (P5 Axis C, C-D3).
 *
 * Unix-domain sockets are primary; loopback TCP is the fallback. Every socket
 * is non-blocking and close-on-exec. A unix listener is chmod'ed 0600 (the
 * caller owns a 0700 directory) and recovers a stale socket file by probing it
 * with a connect before unlinking. Errors are negative linux errno.
 */
#include "net/net_internal.h"

#include "plat/linux/syscall.h"

#define AF_UNIX_ 1
#define AF_INET_ 2
#define SOCK_STREAM_ 1
#define SOCK_NONBLOCK_ 0x800
#define SOCK_CLOEXEC_ 0x80000
#define SOL_SOCKET_ 1
#define SO_REUSEADDR_ 2
#define E_INPROGRESS (-115)
#define E_EAGAIN (-11)
#define E_EADDRINUSE (-98)
#define E_NAMETOOLONG (-36)

struct un_sockaddr_un {
    u16 family;
    char path[108];
} __attribute__((packed));

struct un_sockaddr_in {
    u16 family;
    u16 port_be;
    u32 addr_be;
    u8 zero[8];
} __attribute__((packed));

static u16 un_htons(u16 port) {
    return (u16)((port << 8) | (port >> 8));
}

static int un_fill(struct un_sockaddr_un *sa, const char *path) {
    size_t n = agentc_strlen(path);
    if (n == 0 || n >= sizeof sa->path)
        return E_NAMETOOLONG;
    agentc_memset(sa, 0, sizeof *sa);
    sa->family = AF_UNIX_;
    agentc_memcpy(sa->path, path, n + 1);
    return 0;
}

int agentc_net_unix_connect(const char *path) {
    if (!path || !path[0])
        return E_NAMETOOLONG;
    struct un_sockaddr_un sa;
    int e = un_fill(&sa, path);
    if (e)
        return e;
    long fd = linux_sc3(SYS_socket, AF_UNIX_, SOCK_STREAM_ | SOCK_NONBLOCK_ | SOCK_CLOEXEC_, 0);
    if (fd < 0)
        return (int)fd;
    long r = linux_sc3(SYS_connect, fd, (long)&sa, sizeof sa);
    if (r < 0) {
        int err = (int)r;
        if (err == E_INPROGRESS || err == E_EAGAIN)
            return (int)fd;
        linux_sc1(SYS_close, fd);
        return err;
    }
    return (int)fd;
}

int agentc_net_unix_listen(const char *path, int backlog) {
    if (!path || !path[0])
        return E_NAMETOOLONG;
    struct un_sockaddr_un sa;
    int e = un_fill(&sa, path);
    if (e)
        return e;
    long fd = linux_sc3(SYS_socket, AF_UNIX_, SOCK_STREAM_ | SOCK_NONBLOCK_ | SOCK_CLOEXEC_, 0);
    if (fd < 0)
        return (int)fd;
    long r = linux_sc3(SYS_bind, fd, (long)&sa, sizeof sa);
    if (r < 0 && (int)r == E_EADDRINUSE) {
        /* Probe: a live server answers (0) or is still connecting (-EINPROGRESS). */
        int probe = agentc_net_unix_connect(path);
        bool live = probe >= 0 || probe == E_INPROGRESS;
        if (probe >= 0)
            linux_sc1(SYS_close, probe);
        if (live) {
            linux_sc1(SYS_close, fd);
            return E_EADDRINUSE;
        }
        (void)agentc_net_unlink_socket(path);
        r = linux_sc3(SYS_bind, fd, (long)&sa, sizeof sa);
    }
    if (r < 0) {
        int err = (int)r;
        linux_sc1(SYS_close, fd);
        return err;
    }
    long lr = linux_sc2(SYS_listen, fd, backlog);
    if (lr < 0) {
        linux_sc1(SYS_close, fd);
        return (int)lr;
    }
    (void)os_chmod(path, 0600);
    return (int)fd;
}

int agentc_net_tcp_listen(u32 ip_be, u16 port, int backlog) {
    long fd = linux_sc3(SYS_socket, AF_INET_, SOCK_STREAM_ | SOCK_NONBLOCK_ | SOCK_CLOEXEC_, 0);
    if (fd < 0)
        return (int)fd;
    int one = 1;
    (void)linux_sc5(SYS_setsockopt, fd, SOL_SOCKET_, SO_REUSEADDR_, (long)&one, sizeof one);
    struct un_sockaddr_in sa;
    agentc_memset(&sa, 0, sizeof sa);
    sa.family = AF_INET_;
    sa.port_be = un_htons(port);
    sa.addr_be = ip_be;
    long br = linux_sc3(SYS_bind, fd, (long)&sa, sizeof sa);
    if (br < 0) {
        linux_sc1(SYS_close, fd);
        return (int)br;
    }
    long lr = linux_sc2(SYS_listen, fd, backlog);
    if (lr < 0) {
        linux_sc1(SYS_close, fd);
        return (int)lr;
    }
    return (int)fd;
}

int agentc_net_accept(int lfd) {
    long r = linux_sc4(SYS_accept4, lfd, 0, 0, SOCK_NONBLOCK_ | SOCK_CLOEXEC_);
    return (int)r;
}

int agentc_net_local_addr(int fd, char *out, size_t cap) {
    if (!out || cap == 0)
        return -22;
    u8 buf[128];
    int len = sizeof buf;
    long r = linux_sc3(SYS_getsockname, fd, (long)buf, (long)&len);
    if (r < 0)
        return (int)r;
    u16 family;
    agentc_memcpy(&family, buf, sizeof family);
    if (family == AF_UNIX_) {
        const char *p = (const char *)buf + 2;
        size_t n = 0;
        while (n < (size_t)len - 2 && p[n])
            n++;
        int w = agentc_snprintf(out, cap, "unix:%.*s", (int)n, p);
        return (w < 0 || (size_t)w >= cap) ? -36 : 0;
    }
    if (family == AF_INET_) {
        u16 port_be;
        u32 addr_be;
        agentc_memcpy(&port_be, buf + 2, sizeof port_be);
        agentc_memcpy(&addr_be, buf + 4, sizeof addr_be);
        u8 ip[4];
        agentc_memcpy(ip, &addr_be, 4);
        u16 port = (u16)((port_be >> 8) | (port_be << 8));
        int w = agentc_snprintf(out, cap, "tcp:%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3],
                                (unsigned)port);
        return (w < 0 || (size_t)w >= cap) ? -36 : 0;
    }
    return -22;
}

int agentc_net_unlink_socket(const char *path) {
    if (!path || !path[0])
        return -22;
    return (int)linux_unlink(path);
}
