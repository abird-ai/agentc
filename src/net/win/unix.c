/* unix.c — Layer 1 Windows backend: framed-protocol listeners (P5 Axis C).
 *
 * Windows has no AF_UNIX in this freestanding ws2_32 layer, so the unix
 * entry points report -ENOSYS and `agentc serve` uses the loopback-TCP
 * fallback (C-D3). TCP listener/accept/local_addr use ws2_32 and the shared
 * Win32 fd table. AF_UNIX support under Wine is version-dependent:
 * tests/serve.sh --wine is a documented skip unless a Wine build exposes it, and
 * the loopback-TCP fallback is the safety net (C2; 61-protocol.md §3). */
#include "net/net_internal.h"
#include "plat/win/win.h"

#define WSA_AF_INET 2
#define WSA_SOCK_STREAM 1
#define WSA_IPPROTO_TCP 6
#define WSA_SOL_SOCKET 0xFFFF
#define WSA_SO_REUSEADDR 0x0004
#define WSA_FIONBIO 0x8004667Eu

__declspec(dllimport) WinSOCKET socket(int, int, int);
__declspec(dllimport) int bind(WinSOCKET, const WinSockaddrIn *, int);
__declspec(dllimport) int listen(WinSOCKET, int);
__declspec(dllimport) WinSOCKET accept(WinSOCKET, void *, int *);
__declspec(dllimport) int getsockname(WinSOCKET, WinSockaddrIn *, int *);
__declspec(dllimport) int setsockopt(WinSOCKET, int, int, const char *, int);
__declspec(dllimport) int ioctlsocket(WinSOCKET, long, WinULONG *);
__declspec(dllimport) int closesocket(WinSOCKET);
__declspec(dllimport) int WSAGetLastError(void);

static WinSOCKET un_sock_of(int fd) {
    WinFd *f = win_fd(fd);
    if (f == NULL || f->kind != WIN_FD_SOCKET)
        return WIN_INVALID_SOCKET;
    return (WinSOCKET)(uintptr_t)f->handle;
}

static int un_register(WinSOCKET s) {
    WinULONG nb = 1;
    ioctlsocket(s, (long)WSA_FIONBIO, &nb);
    int fd = win_fd_alloc((WinHandle)(uintptr_t)s, NULL, WIN_FD_SOCKET, WIN_FD_READ | WIN_FD_WRITE);
    if (fd < 0)
        closesocket(s);
    return fd;
}

int agentc_net_unix_listen(const char *path, int backlog) {
    (void)path;
    (void)backlog;
    return -38; /* ENOSYS: use the TCP fallback on Windows (C2) */
}
int agentc_net_unix_connect(const char *path) {
    (void)path;
    return -38;
}

int agentc_net_tcp_listen(u32 ip_be, u16 port, int backlog) {
    if (win_ws_init() != 0)
        return -5;
    WinSOCKET s = socket(WSA_AF_INET, WSA_SOCK_STREAM, WSA_IPPROTO_TCP);
    if (s == WIN_INVALID_SOCKET)
        return win_wsa_errno(WSAGetLastError());
    int one = 1;
    (void)setsockopt(s, WSA_SOL_SOCKET, WSA_SO_REUSEADDR, (const char *)&one, (int)sizeof one);
    WinSockaddrIn sa;
    agentc_memset(&sa, 0, sizeof sa);
    sa.sin_family = WSA_AF_INET;
    sa.sin_port = (u16)((port >> 8) | (port << 8));
    sa.sin_addr = ip_be;
    if (bind(s, &sa, (int)sizeof sa) == WIN_SOCKET_ERROR) {
        int e = win_wsa_errno(WSAGetLastError());
        closesocket(s);
        return e;
    }
    if (listen(s, backlog) == WIN_SOCKET_ERROR) {
        int e = win_wsa_errno(WSAGetLastError());
        closesocket(s);
        return e;
    }
    return un_register(s);
}

int agentc_net_accept(int lfd) {
    WinSOCKET s = un_sock_of(lfd);
    if (s == WIN_INVALID_SOCKET)
        return -88;
    WinSOCKET c = accept(s, 0, 0);
    if (c == WIN_INVALID_SOCKET) {
        int e = WSAGetLastError();
        if (e == 10035 || e == 10036)
            return -11; /* WSAEWOULDBLOCK/INPROGRESS */
        return win_wsa_errno(e);
    }
    return un_register(c);
}

int agentc_net_local_addr(int fd, char *out, size_t cap) {
    if (!out || cap == 0)
        return -22;
    WinSOCKET s = un_sock_of(fd);
    if (s == WIN_INVALID_SOCKET)
        return -88;
    WinSockaddrIn sa;
    int len = (int)sizeof sa;
    if (getsockname(s, &sa, &len) == WIN_SOCKET_ERROR)
        return win_wsa_errno(WSAGetLastError());
    u16 port = (u16)((sa.sin_port >> 8) | (sa.sin_port << 8));
    const u8 *b = (const u8 *)&sa.sin_addr;
    int w = agentc_snprintf(out, cap, "tcp:%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], (unsigned)port);
    return (w < 0 || (size_t)w >= cap) ? -36 : 0;
}

int agentc_net_unlink_socket(const char *path) {
    (void)path;
    return -38;
}
