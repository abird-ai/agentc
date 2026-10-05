/* plat/win/rt.c — Windows bootstrap, fd table and Win32 error mapping.
 *
 * The MSVC x86-64 ABI entry is win_start (no CRT, no arguments); the command
 * line and the environment come from GetCommandLineW/CommandLineToArgvW and
 * GetEnvironmentStringsW, both converted to UTF-8. Stdio handles are wrapped in
 * a 1024-entry fd table so the rest of the platform layer and the core can use
 * small int fds exactly as on Linux. All handles we create are non-inheritable
 * (passing NULL SECURITY_ATTRIBUTES); os_spawn() temporarily flips the three
 * stdio handles it hands to CreateProcessW.
 *
 * Prototypes live in win.h: no <windows.h>, no CRT.
 */
#include "agentc.h"
#include "plat.h"
#include "win.h"

/* Console control events (Ctrl+C, close, logoff, shutdown) map onto the signal
 * API; there is no equivalent for hardware faults on Windows. */
static void (*g_ctrl_handler)(int);

static WinBOOL win_ctrl_dispatch(WinDWORD type) {
    if (!g_ctrl_handler) return 0;
    switch (type) {
    case 0: g_ctrl_handler(OS_SIGINT); break;    /* CTRL_C_EVENT */
    case 1: g_ctrl_handler(OS_SIGINT); break;    /* CTRL_BREAK_EVENT */
    case 2: g_ctrl_handler(OS_SIGHUP); break;    /* CTRL_CLOSE_EVENT */
    case 5: g_ctrl_handler(OS_SIGTERM); break;   /* CTRL_LOGOFF_EVENT */
    case 6: g_ctrl_handler(OS_SIGTERM); break;   /* CTRL_SHUTDOWN_EVENT */
    default: return 0;
    }
    return 1;
}

int os_sig_install(int sig, void (*handler)(int)) {
    if (sig != OS_SIGINT && sig != OS_SIGTERM && sig != OS_SIGHUP) return -38; /* ENOSYS */
    if (handler) {
        g_ctrl_handler = handler;
        (void)SetConsoleCtrlHandler(win_ctrl_dispatch, 1);
    } else {
        g_ctrl_handler = NULL;
        (void)SetConsoleCtrlHandler(win_ctrl_dispatch, 0);
    }
    return 0;
}

/* reported to the model in the system prompt (see prompt.c) */
const char *os_platform(void) { return "windows"; }

int agentc_main(int argc, char **argv);

/* Windows stack probe emitted by clang for frames larger than one page; the
 * compiler calls it before subtracting the frame from sp.
 *
 * x86-64: rax holds the allocation size; push rcx/rax preserve both while the
 * loop touches one byte per page walking down from the caller's frame. Must
 * leave rax/rcx intact. Same sequence as compiler-rt's x86_64 chkstk.S.
 *
 * ARM64: x15 holds the allocation size in 16-byte units and must survive the
 * call; the compiler follows with `sub sp, sp, x15, lsl #4`. Walk down from
 * the current sp one 4 KiB page at a time, touching each page so the OS can
 * grow the stack, then touch the (possibly unaligned) target; never probe
 * below the target. x16/x17 are the ABI's intra-procedure-call scratch
 * registers. */
#if defined(__x86_64__)
__attribute__((naked)) void __chkstk(void) {
    __asm__ volatile(
        "pushq %rcx\n\t"
        "pushq %rax\n\t"
        "cmpq $0x1000, %rax\n\t"
        "leaq 24(%rsp), %rcx\n\t"
        "jb 2f\n\t"
        "1:\n\t"
        "subq $0x1000, %rcx\n\t"
        "orq $0, (%rcx)\n\t"
        "subq $0x1000, %rax\n\t"
        "cmpq $0x1000, %rax\n\t"
        "ja 1b\n\t"
        "2:\n\t"
        "subq %rax, %rcx\n\t"
        "orq $0, (%rcx)\n\t"
        "popq %rax\n\t"
        "popq %rcx\n\t"
        "ret\n\t");
}
#elif defined(__aarch64__)
__attribute__((naked)) void __chkstk(void) {
    __asm__ volatile(
        "lsl x16, x15, #4\n\t"     /* allocation size in bytes */
        "sub x16, sp, x16\n\t"     /* x16 = target sp */
        "mov x17, sp\n\t"
        "1:\n\t"
        "sub x17, x17, #4096\n\t"
        "cmp x17, x16\n\t"
        "b.lo 2f\n\t"
        "ldr xzr, [x17]\n\t"
        "b 1b\n\t"
        "2:\n\t"
        "ldr xzr, [x16]\n\t"
        "ret\n\t");
}
#endif

static int g_argc;
static char **g_argv;
static char **g_envp;
static bool g_inited;

static WinFd g_fds[WIN_FD_MAX];

__attribute__((noreturn)) void win_start(void) {
    os_init(NULL);
    os_exit(agentc_main(g_argc, g_argv));
    __builtin_unreachable();
}

/* ------------------------------------------------------------------- UTF-8 */
u16 *win_utf8_to_wide(const char *s) {
    if (s == NULL) return NULL;
    int n = MultiByteToWideChar(65001 /*CP_UTF8*/, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    u16 *w = agentc_alloc((size_t)n * sizeof(u16));
    if (MultiByteToWideChar(65001, 0, s, -1, w, n) <= 0) {
        agentc_free(w);
        return NULL;
    }
    return w;
}

char *win_wide_to_utf8(const u16 *s) {
    if (s == NULL) return NULL;
    int n = WideCharToMultiByte(65001, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *out = agentc_alloc((size_t)n);
    if (WideCharToMultiByte(65001, 0, s, -1, out, n, NULL, NULL) <= 0) {
        agentc_free(out);
        return NULL;
    }
    return out;
}

size_t wide_len(const u16 *s) {
    size_t n = 0;
    while (s[n] != 0) n++;
    return n;
}

u16 *win_path_wide(const char *path) {
    if (path == NULL) return NULL;
    if (agentc_streq(path, "/dev/null") || agentc_streq(path, "/dev/zero"))
        return win_utf8_to_wide("NUL");
    size_t n = agentc_strlen(path);
    /* Normalise separators; Win32 accepts '/', but \\?\ long paths do not. */
    char *norm = agentc_alloc(n + 8);
    for (size_t i = 0; i < n; i++) norm[i] = path[i] == '/' ? '\\' : path[i];
    norm[n] = '\0';
    u16 *w;
    if (n >= 240 && n >= 3 && norm[1] == ':') {
        char *lp = agentc_alloc(n + 8);
        agentc_memcpy(lp, "\\\\?\\", 4);
        agentc_memcpy(lp + 4, norm, n + 1);
        w = win_utf8_to_wide(lp);
        agentc_free(lp);
    } else if (n >= 240 && n >= 3 && norm[0] == '\\' && norm[1] == '\\') {
        char *lp = agentc_alloc(n + 16);
        agentc_memcpy(lp, "\\\\?\\UNC\\", 8);
        agentc_memcpy(lp + 8, norm + 2, n - 1);
        w = win_utf8_to_wide(lp);
        agentc_free(lp);
    } else {
        w = win_utf8_to_wide(norm);
    }
    agentc_free(norm);
    return w;
}

/* -------------------------------------------------------------- error maps */
int win_errno(WinDWORD err) {
    switch (err) {
    case WIN_ERROR_SUCCESS: return 0;
    case WIN_ERROR_FILE_NOT_FOUND:
    case WIN_ERROR_PATH_NOT_FOUND:
    case WIN_ERROR_NO_MORE_FILES:
    case WIN_ERROR_NOT_FOUND:
    case WIN_ERROR_MOD_NOT_FOUND:
    case WIN_ERROR_PROC_NOT_FOUND: return -2; /* ENOENT */
    case WIN_ERROR_BAD_EXE_FORMAT: return -8;  /* ENOEXEC */
    case WIN_ERROR_DLL_INIT_FAILED: return -5; /* EIO */
    case WIN_ERROR_ACCESS_DENIED: return -13;
    case WIN_ERROR_INVALID_HANDLE: return -9;
    case WIN_ERROR_NOT_ENOUGH_MEMORY: return -12;
    case WIN_ERROR_TOO_MANY_OPEN_FILES: return -24;
    case WIN_ERROR_SHARING_VIOLATION:
    case WIN_ERROR_PIPE_BUSY: return -16; /* EBUSY */
    case WIN_ERROR_FILE_EXISTS:
    case WIN_ERROR_ALREADY_EXISTS: return -17;
    case WIN_ERROR_BUFFER_OVERFLOW:
    case WIN_ERROR_FILENAME_EXCED_RANGE: return -36; /* ENAMETOOLONG */
    case WIN_ERROR_INVALID_PARAMETER: return -22;
    case WIN_ERROR_BROKEN_PIPE:
    case WIN_ERROR_NO_DATA:
    case WIN_ERROR_PIPE_NOT_CONNECTED: return -32; /* EPIPE */
    case WIN_ERROR_DISK_FULL: return -28;
    case WIN_ERROR_INSUFFICIENT_BUFFER: return -75; /* EOVERFLOW */
    case WIN_ERROR_DIR_NOT_EMPTY: return -39;
    case WIN_ERROR_DIRECTORY: return -20; /* ENOTDIR */
    case WIN_ERROR_OPERATION_ABORTED: return -125; /* ECANCELED */
    case WIN_ERROR_IO_PENDING:
    case WIN_ERROR_IO_INCOMPLETE: return -11; /* EAGAIN */
    case WIN_WAIT_TIMEOUT_CODE:
    case WIN_ERROR_SEM_TIMEOUT: return -110; /* ETIMEDOUT */
    case WIN_ERROR_NOT_SUPPORTED:
    case WIN_ERROR_CALL_NOT_IMPLEMENTED: return -38; /* ENOSYS */
    case WIN_ERROR_HANDLE_EOF: return 0;            /* EOF, not an error */
    default: return -5;                            /* EIO */
    }
}

int win_wsa_errno(int err) {
    switch (err) {
    case 0: return 0;
    case 10004: return -4;   /* WSAEINTR */
    case 10009: return -9;   /* WSAEBADF */
    case 10013: return -13;  /* WSAEACCES */
    case 10014: return -14;  /* WSAEFAULT */
    case 10022: return -22;  /* WSAEINVAL */
    case 10024: return -24;  /* WSAEMFILE */
    case 10035: return -11;  /* WSAEWOULDBLOCK */
    case 10036: return -115; /* WSAEINPROGRESS */
    case 10037: return -114; /* WSAEALREADY */
    case 10038: return -88;  /* WSAENOTSOCK */
    case 10039: return -89;  /* WSAEDESTADDRREQ */
    case 10040: return -90;  /* WSAEMSGSIZE */
    case 10041: return -91;  /* WSAEPROTOTYPE */
    case 10042: return -92;  /* WSAENOPROTOOPT */
    case 10043: return -93;  /* WSAEPROTONOSUPPORT */
    case 10044: return -94;  /* WSAESOCKTNOSUPPORT */
    case 10045: return -95;  /* WSAEOPNOTSUPP */
    case 10046: return -96;  /* WSAEPFNOSUPPORT */
    case 10047: return -97;  /* WSAEAFNOSUPPORT */
    case 10048: return -98;  /* WSAEADDRINUSE */
    case 10049: return -99;  /* WSAEADDRNOTAVAIL */
    case 10050: return -100; /* WSAENETDOWN */
    case 10051: return -101; /* WSAENETUNREACH */
    case 10052: return -102; /* WSAENETRESET */
    case 10053: return -103; /* WSAECONNABORTED */
    case 10054: return -104; /* WSAECONNRESET */
    case 10055: return -105; /* WSAENOBUFS */
    case 10056: return -106; /* WSAEISCONN */
    case 10057: return -107; /* WSAENOTCONN */
    case 10058: return -108; /* WSAESHUTDOWN */
    case 10059: return -109; /* WSAETOOMANYREFS */
    case 10060: return -110; /* WSAETIMEDOUT */
    case 10061: return -111; /* WSAECONNREFUSED */
    case 10062: return -40;  /* WSAELOOP */
    case 10063: return -36;  /* WSAENAMETOOLONG */
    case 10064: return -112; /* WSAEHOSTDOWN */
    case 10065: return -113; /* WSAEHOSTUNREACH */
    case 10066: return -39;  /* WSAENOTEMPTY */
    case 10068: return -87;  /* WSAEUSERS */
    case 10069: return -122; /* WSAEDQUOT */
    case 10070: return -116; /* WSAESTALE */
    case 10071: return -66;  /* WSAEREMOTE */
    case 10091: return -5;   /* WSASYSNOTREADY */
    case 10092: return -38;  /* WSAVERNOTSUPPORTED */
    case 10101: return -107; /* WSAEDISCON */
    case 11001: return -2;   /* WSAHOST_NOT_FOUND */
    case 11002: return -11;  /* WSATRY_AGAIN */
    case 11003: return -5;   /* WSANO_RECOVERY */
    case 11004: return -2;   /* WSANO_DATA */
    default: return -5;
    }
}

/* ----------------------------------------------------------------- fd table */
WinFd *win_fd(int fd) {
    if (fd < 0 || fd >= WIN_FD_MAX) return NULL;
    if (g_fds[fd].kind == WIN_FD_FREE) return NULL;
    return &g_fds[fd];
}

int win_fd_alloc(WinHandle handle, WinHandle write_handle, int kind, u16 flags) {
    for (int i = 0; i < WIN_FD_MAX; i++) {
        if (g_fds[i].kind != WIN_FD_FREE) continue;
        g_fds[i].handle = handle;
        g_fds[i].write_handle = write_handle;
        g_fds[i].kind = (u8)kind;
        g_fds[i].flags = flags;
        g_fds[i].path = NULL;
        g_fds[i].aux = NULL;
        return i;
    }
    return -24; /* EMFILE */
}

void win_fd_free(int fd) {
    WinFd *f = win_fd(fd);
    if (f == NULL) return;
    if (f->kind == WIN_FD_DIR && f->aux != NULL) {
        WinFindCtx *ctx = f->aux;
        if (ctx->find != NULL && ctx->find != WIN_INVALID_HANDLE_VALUE)
            FindClose(ctx->find);
        agentc_free(ctx);
        f->aux = NULL;
    }
    if (f->handle != NULL && f->handle != WIN_INVALID_HANDLE) CloseHandle(f->handle);
    if (f->write_handle != NULL && f->write_handle != f->handle)
        CloseHandle(f->write_handle);
    if (f->path != NULL) agentc_free(f->path);
    agentc_memset(f, 0, sizeof *f);
    f->kind = WIN_FD_FREE;
}

WinHandle win_fd_handle(int fd) {
    WinFd *f = win_fd(fd);
    return f != NULL ? f->handle : NULL;
}

/* ---------------------------------------------------------------- bootstrap */
static void setup_stdio(void) {
    const WinDWORD ids[3] = { WIN_STD_INPUT_HANDLE, WIN_STD_OUTPUT_HANDLE,
                              WIN_STD_ERROR_HANDLE };
    for (int i = 0; i < 3; i++) {
        WinHandle h = GetStdHandle(ids[i]);
        if (h == NULL || h == WIN_INVALID_HANDLE) continue;
        int kind = WIN_FD_FILE;
        switch (GetFileType(h)) {
        case WIN_FILE_TYPE_CHAR: kind = WIN_FD_CONSOLE; break;
        case WIN_FILE_TYPE_PIPE: kind = WIN_FD_PIPE; break;
        default: kind = WIN_FD_FILE; break;
        }
        u16 flags = WIN_FD_STD;
        if (i == 0) flags |= WIN_FD_READ;
        else flags |= WIN_FD_WRITE;
        win_fd_alloc(h, NULL, kind, flags);
    }
}

void os_init(void *initial_stack) {
    (void)initial_stack;
    agentc_memset(g_fds, 0, sizeof g_fds);
    for (int i = 0; i < WIN_FD_MAX; i++) g_fds[i].kind = WIN_FD_FREE;

    int argc = 0;
    WinWCHAR **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (wargv != NULL && argc > 0) {
        g_argv = agentc_alloc(((size_t)argc + 1) * sizeof(char *));
        g_argc = argc;
        for (int i = 0; i < argc; i++) g_argv[i] = win_wide_to_utf8(wargv[i]);
        g_argv[argc] = NULL;
        LocalFree(wargv);
    } else {
        g_argc = 1;
        g_argv = agentc_alloc(2 * sizeof(char *));
        g_argv[0] = agentc_strdup("agentc");
        g_argv[1] = NULL;
    }

    size_t envc = 0;
    WinWCHAR *env = GetEnvironmentStringsW();
    if (env != NULL) {
        for (WinWCHAR *p = env; *p != 0; p += wide_len(p) + 1) envc++;
        g_envp = agentc_alloc((envc + 1) * sizeof(char *));
        size_t i = 0;
        for (WinWCHAR *p = env; *p != 0; p += wide_len(p) + 1)
            g_envp[i++] = win_wide_to_utf8(p);
        g_envp[envc] = NULL;
        FreeEnvironmentStringsW(env);
    } else {
        g_envp = agentc_alloc(sizeof(char *));
        g_envp[0] = NULL;
    }

    setup_stdio();
    win_console_init();
    g_inited = true;
}

void os_exit(int code) {
    ExitProcess((WinUINT)code);
    __builtin_unreachable();
}

const char *os_getenv(const char *name) {
    if (!g_inited || name == NULL) return NULL;
    size_t n = agentc_strlen(name);
    for (char **e = g_envp; e != NULL && *e != NULL; e++) {
        const char *s = *e;
        size_t i = 0;
        while (i < n && s[i] != '\0' && s[i] != '=') {
            char a = s[i];
            char b = name[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            i++;
        }
        if (i == n && s[i] == '=') return s + n + 1;
    }
    return NULL;
}
