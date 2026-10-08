/* net/win/socket.c — Layer 1 Windows backend on ws2_32.
 *
 * Sockets are created blocking and switched to non-blocking with
 * ioctlsocket(FIONBIO); the handle is registered in the shared Win32 fd table
 * so os_poll() can hand it to WSAPoll. -EAGAIN / -EINPROGRESS are normal
 * outcomes; WSA errors are translated to negative Linux errno.
 */
#include "net/net_internal.h"
#include "plat/win/win.h"

#define WSA_AF_INET 2
#define WSA_SOCK_STREAM 1
#define WSA_SOCK_DGRAM 2
#define WSA_IPPROTO_TCP 6
#define WSA_SOL_SOCKET 0xFFFF
#define WSA_SO_ERROR 0x1007
#define WSA_TCP_NODELAY 1
#define WSA_FIONBIO 0x8004667Eu

__declspec(dllimport) WinSOCKET socket(int, int, int);
__declspec(dllimport) int connect(WinSOCKET, const WinSockaddrIn *, int);
__declspec(dllimport) int send(WinSOCKET, const char *, int, int);
__declspec(dllimport) int recv(WinSOCKET, char *, int, int);
__declspec(dllimport) int closesocket(WinSOCKET);
__declspec(dllimport) int getsockopt(WinSOCKET, int, int, char *, int *);
__declspec(dllimport) int setsockopt(WinSOCKET, int, int, const char *, int);
__declspec(dllimport) int ioctlsocket(WinSOCKET, long, WinULONG *);
__declspec(dllimport) int WSAGetLastError(void);

static WinSOCKET sock_of(int fd) {
    WinFd *f = win_fd(fd);
    if (f == NULL || f->kind != WIN_FD_SOCKET) return WIN_INVALID_SOCKET;
    return (WinSOCKET)(uintptr_t)f->handle;
}

static int register_socket(WinSOCKET s) {
    WinULONG nb = 1;
    ioctlsocket(s, (long)WSA_FIONBIO, &nb);
    int fd = win_fd_alloc((WinHandle)(uintptr_t)s, NULL, WIN_FD_SOCKET,
                          WIN_FD_READ | WIN_FD_WRITE);
    if (fd < 0) closesocket(s);
    return fd;
}

int agentc_net_init(void) { return win_ws_init(); }

int agentc_net_set_nodelay(int fd) {
    WinSOCKET s = sock_of(fd);
    if (s == WIN_INVALID_SOCKET) return -88;
    int one = 1;
    if (setsockopt(s, WSA_IPPROTO_TCP, WSA_TCP_NODELAY, (const char *)&one,
                   (int)sizeof one) == WIN_SOCKET_ERROR)
        return win_wsa_errno(WSAGetLastError());
    return 0;
}

int agentc_net_socket(void) {
    if (win_ws_init() != 0) return -5;
    WinSOCKET s = socket(WSA_AF_INET, WSA_SOCK_STREAM, WSA_IPPROTO_TCP);
    if (s == WIN_INVALID_SOCKET) return win_wsa_errno(WSAGetLastError());
    int fd = register_socket(s);
    if (fd < 0) return fd;
    int r = agentc_net_set_nodelay(fd);
    if (r < 0) {
        agentc_net_close(fd);
        return r;
    }
    return fd;
}

int agentc_net_udp_socket(void) {
    if (win_ws_init() != 0) return -5;
    WinSOCKET s = socket(WSA_AF_INET, WSA_SOCK_DGRAM, 0);
    if (s == WIN_INVALID_SOCKET) return win_wsa_errno(WSAGetLastError());
    return register_socket(s);
}

static u16 port_htons(u16 port) { return (u16)((port << 8) | (port >> 8)); }

int agentc_net_connect(int fd, u32 ip_be, u16 port, int deadline_ms) {
    (void)deadline_ms; /* one connect; poll for POLLOUT then so_error */
    WinSOCKET s = sock_of(fd);
    if (s == WIN_INVALID_SOCKET) return -88;
    WinSockaddrIn sa;
    agentc_memset(&sa, 0, sizeof sa);
    sa.sin_family = WSA_AF_INET;
    sa.sin_port = port_htons(port);
    agentc_memcpy(&sa.sin_addr, &ip_be, 4);
    if (connect(s, &sa, (int)sizeof sa) == WIN_SOCKET_ERROR) {
        int e = WSAGetLastError();
        /* Winsock reports a pending non-blocking connect as WSAEWOULDBLOCK;
         * the core waits for POLLOUT on -EINPROGRESS specifically. */
        if (e == 10035 || e == 10036) return -115; /* WSAEWOULDBLOCK/INPROGRESS */
        return win_wsa_errno(e);
    }
    return 0;
}

int agentc_net_send(int fd, const void *p, size_t n) {
    WinSOCKET s = sock_of(fd);
    if (s == WIN_INVALID_SOCKET) return -88;
    if (n == 0) return 0;
    int len = n > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)n;
    int r = send(s, (const char *)p, len, 0);
    if (r == WIN_SOCKET_ERROR) return win_wsa_errno(WSAGetLastError());
    return r;
}

int agentc_net_recv(int fd, void *p, size_t n) {
    WinSOCKET s = sock_of(fd);
    if (s == WIN_INVALID_SOCKET) return -88;
    if (n == 0) return 0;
    int len = n > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)n;
    int r = recv(s, (char *)p, len, 0);
    if (r == WIN_SOCKET_ERROR) return win_wsa_errno(WSAGetLastError());
    return r;
}

void agentc_net_close(int fd) {
    WinFd *f = win_fd(fd);
    if (f == NULL || f->kind != WIN_FD_SOCKET) return;
    closesocket((WinSOCKET)(uintptr_t)f->handle);
    win_fd_free(fd);
}

int agentc_net_so_error(int fd) {
    WinSOCKET s = sock_of(fd);
    if (s == WIN_INVALID_SOCKET) return -88;
    int err = 0;
    int len = (int)sizeof err;
    if (getsockopt(s, WSA_SOL_SOCKET, WSA_SO_ERROR, (char *)&err, &len) ==
        WIN_SOCKET_ERROR)
        return win_wsa_errno(WSAGetLastError());
    return err != 0 ? win_wsa_errno(err) : 0;
}

int agentc_net_poll(int fd, short events, int timeout_ms) {
    struct os_pollfd p = { fd, events, 0 };
    return os_poll(&p, 1, timeout_ms);
}
