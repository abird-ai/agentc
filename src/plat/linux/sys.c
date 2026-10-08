/* plat/linux/sys.c — Layer 0 on raw x86-64 Linux syscalls (no libc).
 *
 * Errors are returned as negative errno values exactly as the kernel produces
 * them, which is the convention every adapter must follow (see plat.h).
 */
#include "agentc.h"
#include "plat.h"

#include "plat/linux/syscall.h"

#define AT_FDCWD -100
#define PROT_RW 3
#define MAP_PRIV_ANON 0x22
#define O_CLOEXEC 0x80000
#define O_NONBLOCK 0x800
#define F_GETFL 3
#define F_SETFL 4

/* setpgid(0, 0): the child becomes its own process group leader.  The number is
 * arch-dependent and used only here, so it stays local to the spawn path. */
#if defined(LINUX_ARCH_X86_64)
#define SYS_SETPGID 109
#else
#define SYS_SETPGID 154
#endif

static long g_argc;
static char **g_argv;
static char **g_envp;
static bool g_inited;

/* ---------------------------------------------------------------- bootstrap */
int agentc_main(int argc, char **argv);

[[noreturn]] void agentc_start(void *stack) {
    os_init(stack);
    int code = agentc_main((int)g_argc, g_argv);
    os_exit(code);
    __builtin_unreachable();
}

/* The kernel enters every Linux program with `sp` pointing at argc; agentc_start
 * takes that pointer. x86-64 passes it in %rdi, aarch64 in x0, riscv64 in a0. */
__attribute__((naked, noreturn)) void _start(void) {
#if defined(LINUX_ARCH_X86_64)
    __asm__("xorq %rbp, %rbp\n\t"
            "movq %rsp, %rdi\n\t"
            "andq $-16, %rsp\n\t"
            "call agentc_start\n\t"
            "ud2\n\t");
#elif defined(LINUX_ARCH_AARCH64)
    __asm__("mov x0, sp\n\t"
            "and sp, x0, #-16\n\t"
            "bl agentc_start\n\t"
            "brk #1\n\t");
#else
    __asm__("mv a0, sp\n\t"
            "andi sp, sp, -16\n\t"
            "call agentc_start\n\t"
            "ebreak\n\t");
#endif
}

const char *os_platform(void) { return "linux"; }

void os_init(void *stack) {
    long *sp = stack;
    g_argc = sp[0];
    g_argv = (char **)(sp + 1);
    g_envp = g_argv + g_argc + 1;
    g_inited = true;
}

void os_exit(int code) {
    linux_sc1(SYS_exit_group, code);
    __builtin_unreachable();
}

const char *os_getenv(const char *name) {
    size_t n = agentc_strlen(name);
    if (!g_inited) return NULL;
    for (char **e = g_envp; *e; e++) {
        if (agentc_str_eq(*e, n, name, n) && (*e)[n] == '=') return *e + n + 1;
    }
    return NULL;
}

/* ------------------------------------------------------------------ files */
int os_open(const char *path, int flags, int mode) {
#if defined(LINUX_ARCH_AARCH64)
    /* arm64 is the one Linux ABI whose open flags are not the asm-generic ones:
     * O_DIRECTORY is 040000 (0x4000) there while O_DIRECT is 0x10000, the value
     * OS_O_DIRECTORY carries (plat.h follows x86-64/asm-generic). Passing the
     * portable value through opened with O_DIRECT, which fails EINVAL on
     * filesystems that reject it, so every directory read in the core got
     * nothing back. */
    if (flags & OS_O_DIRECTORY) flags = (flags & ~OS_O_DIRECTORY) | 0x4000;
#endif
    return (int)linux_sc4(SYS_openat, AT_FDCWD, (long)path, flags, mode);
}
int os_read(int fd, void *buf, size_t n) { return (int)linux_sc3(SYS_read, fd, (long)buf, (long)n); }
int os_write(int fd, const void *buf, size_t n) { return (int)linux_sc3(SYS_write, fd, (long)buf, (long)n); }
int os_close(int fd) { return (int)linux_sc1(SYS_close, fd); }
i64 os_lseek(int fd, i64 off, int whence) { return linux_sc3(SYS_lseek, fd, off, whence); }
int os_ftruncate(int fd, i64 len) { return (int)linux_sc2(SYS_ftruncate, fd, len); }
/* os_stat/os_fstat fill the portable struct os_stat (plat.h), never the
 * kernel's layout. x86-64's native struct stat happens to be where the project
 * started, but aarch64 and riscv64 use the asm-generic layout (st_mode at 16,
 * int blksize), so the syscall writes linux_stat_native and it is converted
 * here; mac/sys.c and win/sys.c convert from their native structs the same
 * way. */
#if defined(LINUX_ARCH_X86_64)
struct linux_stat_native {
    u64 st_dev;
    u64 st_ino;
    u64 st_nlink;
    u32 st_mode;
    u32 st_uid;
    u32 st_gid;
    u32 __pad0;
    u64 st_rdev;
    i64 st_size;
    i64 st_blksize;
    i64 st_blocks;
    i64 st_atime;
    i64 st_atime_nsec;
    i64 st_mtime;
    i64 st_mtime_nsec;
    i64 st_ctime;
    i64 st_ctime_nsec;
    i64 __unused[3];
};
#else
struct linux_stat_native {
    u64 st_dev;
    u64 st_ino;
    u32 st_mode;
    u32 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u64 st_rdev;
    u64 __pad1;
    i64 st_size;
    int st_blksize;   /* asm-generic spells these `int`, not an i64 */
    int __pad2;
    i64 st_blocks;
    i64 st_atime;
    u64 st_atime_nsec;
    i64 st_mtime;
    u64 st_mtime_nsec;
    i64 st_ctime;
    u64 st_ctime_nsec;
    u32 __unused4;
    u32 __unused5;
};
#endif

static void linux_stat_fixup(void *dst, const void *src) {
    const struct linux_stat_native *g = src;
    struct os_stat *o = dst;
    agentc_memset(o, 0, sizeof *o);
    o->st_dev = g->st_dev;
    o->st_ino = g->st_ino;
    o->st_nlink = g->st_nlink;
    o->st_mode = g->st_mode;
    o->st_uid = g->st_uid;
    o->st_gid = g->st_gid;
    o->st_rdev = g->st_rdev;
    o->st_size = g->st_size;
    o->st_blksize = g->st_blksize;
    o->st_blocks = g->st_blocks;
    o->st_atime = g->st_atime;
    o->st_atime_nsec = (i64)g->st_atime_nsec;
    o->st_mtime = g->st_mtime;
    o->st_mtime_nsec = (i64)g->st_mtime_nsec;
    o->st_ctime = g->st_ctime;
    o->st_ctime_nsec = (i64)g->st_ctime_nsec;
}

int os_fstat(int fd, void *statbuf) {
    struct linux_stat_native g;
#if defined(LINUX_ARCH_X86_64)
    long r = linux_sc2(SYS_fstat, fd, (long)&g);
#else
    /* the generic ABI is happiest with newfstatat + AT_EMPTY_PATH */
    long r = linux_sc4(SYS_newfstatat, fd, (long)"", (long)&g, 0x1000 /*AT_EMPTY_PATH*/);
#endif
    if (r < 0) return (int)r;
    linux_stat_fixup(statbuf, &g);
    return 0;
}
int os_stat(const char *path, void *statbuf) {
    struct linux_stat_native g;
    long r = linux_sc4(SYS_newfstatat, AT_FDCWD, (long)path, (long)&g, 0);
    if (r < 0) return (int)r;
    linux_stat_fixup(statbuf, &g);
    return 0;
}
int os_mkdir(const char *path, int mode) { return (int)linux_sc3(SYS_mkdirat, AT_FDCWD, (long)path, mode); }
int os_unlink(const char *path) { return (int)linux_sc3(SYS_unlinkat, AT_FDCWD, (long)path, 0); }
int os_rename(const char *from, const char *to) {
    /* renameat2(from, to, 0): riscv64 has no renameat(2) syscall at all (see
     * syscall.h); flags=0 is plain rename semantics on every Linux arch. */
    return (int)linux_sc5(SYS_renameat2, AT_FDCWD, (long)from, AT_FDCWD, (long)to, 0);
}
int os_getcwd(char *buf, size_t len) { return (int)linux_sc2(SYS_getcwd, (long)buf, (long)len); }
int os_getdents(int fd, void *buf, size_t len) { return (int)linux_sc3(SYS_getdents64, fd, (long)buf, (long)len); }

/* ------------------------------------------------------------- PATH lookup */
#define WHICH_CWD_MAX 4096

/* A candidate is a hit only when it is a regular file with at least one
 * execute bit. On success the absolute path is copied into out. */
static int which_try(const char *cand, char *out, size_t cap) {
    struct os_stat st;
    if (os_stat(cand, &st) != 0) return -2;              /* ENOENT */
    if ((st.st_mode & 0170000u) != 0100000u) return -2; /* not S_IFREG */
    if (!(st.st_mode & 0111u)) return -2;               /* not executable */
    if (cand[0] == '/') {
        size_t n = agentc_strlen(cand);
        if (n + 1 > cap) return -34; /* ERANGE */
        agentc_memcpy(out, cand, n + 1);
        return 0;
    }
    char cwd[WHICH_CWD_MAX];
    int r = os_getcwd(cwd, sizeof cwd);
    if (r < 0) return r;
    size_t cl = agentc_strlen(cwd);
    size_t nl = agentc_strlen(cand);
    if (cl + 1 + nl + 1 > cap) return -34;
    agentc_memcpy(out, cwd, cl);
    out[cl] = '/';
    agentc_memcpy(out + cl + 1, cand, nl + 1);
    return 0;
}

int os_which_in(const char *path_env, const char *name, char *out, size_t cap) {
    if (name == NULL || name[0] == 0) return -2; /* ENOENT */
    /* A name with a slash is checked as given, relative to the cwd. */
    for (const char *p = name; *p != 0; p++) {
        if (*p == '/') return which_try(name, out, cap);
    }
    if (path_env == NULL || path_env[0] == 0) return -2; /* no PATH search */
    size_t nl = agentc_strlen(name);
    for (const char *p = path_env;;) {
        const char *end = p;
        while (*end != 0 && *end != ':') end++;
        size_t dl = (size_t)(end - p);
        char *cand = agentc_alloc(dl + (dl ? 1 : 0) + nl + 1);
        size_t pos = 0;
        if (dl) {
            agentc_memcpy(cand, p, dl);
            pos = dl;
            cand[pos++] = '/';
        }
        agentc_memcpy(cand + pos, name, nl + 1);
        int r = which_try(cand, out, cap);
        agentc_free(cand);
        if (r == 0 || r == -34) return r; /* hit, or the hit does not fit */
        if (*end == 0) break;
        p = end + 1;
    }
    return -2;
}

int os_which(const char *name, char *out, size_t cap) {
    return os_which_in(os_getenv("PATH"), name, out, cap);
}

/* ---------------------------------------------------------------- memory */
u8 *os_map_try(size_t n) {
    long r = linux_sc6(SYS_mmap, 0, (long)n, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (r < 0 && r > -4096) return NULL;
    return (u8 *)r;
}
u8 *os_map(size_t n) {
    u8 *p = os_map_try(n);
    if (!p) agentc_die("out of memory");
    return p;
}
int os_unmap(u8 *p, size_t n) { return (int)linux_sc2(SYS_munmap, (long)p, (long)n); }

/* ------------------------------------------------------------- processes */
static int linux_spawn_common(char *const argv[], char *const envp[], const char *cwd,
                              int fd_in, int fd_out, int fd_err, int ctty, bool group) {
    (void)ctty; /* controlling-tty setup arrives with the TUI (M4) */
    long pid = linux_fork();
    if (pid < 0) return (int)pid;
    if (pid == 0) {
        /* New process group before exec, so os_kill(-pid) sees the whole tree.
         * A failure means the child is already a leader; ignore it. */
        if (group) (void)linux_sc2(SYS_SETPGID, 0, 0);
        if (fd_in >= 0) linux_dup2(fd_in, 0);
        if (fd_out >= 0) linux_dup2(fd_out, 1);
        if (fd_err >= 0) linux_dup2(fd_err, 2);
        if (cwd) linux_sc1(SYS_chdir, (long)cwd);
        linux_sc3(SYS_execve, (long)argv[0], (long)argv, (long)envp);
        linux_sc1(SYS_exit_group, 127);
        __builtin_unreachable();
    }
    return (int)pid;
}

int os_spawn(char *const argv[], char *const envp[], const char *cwd,
             int fd_in, int fd_out, int fd_err, int ctty) {
    return linux_spawn_common(argv, envp, cwd, fd_in, fd_out, fd_err, ctty, false);
}

int os_spawn_group(char *const argv[], char *const envp[], const char *cwd,
                   int fd_in, int fd_out, int fd_err, int ctty) {
    return linux_spawn_common(argv, envp, cwd, fd_in, fd_out, fd_err, ctty, true);
}

int os_wait(int pid, bool nohang) {
    int status = 0;
    long r = linux_sc4(SYS_wait4, pid, (long)&status, nohang ? 1 : 0, 0);
    if (r > 0) return status;
    if (r == 0) return -1;   /* still running */
    return -2;               /* unknown pid */
}

int os_kill(int pid, int sig) {
    long r = linux_sc2(SYS_kill, pid, sig);
    /* Negative pid: kill the process group; when it no longer exists fall back
     * to the leader pid so a non-group child still dies on the first signal. */
    if (r < 0 && pid < 0) r = linux_sc2(SYS_kill, -pid, sig);
    return (int)r;
}

int os_pipe(int fds[2]) {
    int f[2];
    long r = linux_sc2(SYS_pipe2, (long)f, O_CLOEXEC);
    if (r < 0) return (int)r;
    long fl = linux_sc3(SYS_fcntl, f[0], F_GETFL, 0);
    if (fl >= 0) linux_sc3(SYS_fcntl, f[0], F_SETFL, fl | O_NONBLOCK);
    fds[0] = f[0];
    fds[1] = f[1];
    return 0;
}

/* ------------------------------------------------------------- shell tool */
/* POSIX has exactly one reasonable shell. The Windows kinds are rejected with
 * EINVAL so a config copied from Windows fails loudly instead of silently
 * running sh with cmd syntax. */
const char *os_shell_kind(const char *kind) {
    if (kind == NULL || kind[0] == 0 || agentc_streq(kind, "auto") ||
        agentc_streq(kind, "sh"))
        return "sh";
    return NULL;
}

int os_shell_resolve(const char *kind, char *out, size_t cap) {
    if (os_shell_kind(kind) == NULL) return -22; /* EINVAL */
    if (cap < sizeof "/bin/sh") return -36;     /* ENAMETOOLONG */
    agentc_memcpy(out, "/bin/sh", sizeof "/bin/sh");
    return 0;
}

static int linux_spawn_shell_common(const char *kind, const char *command, int fd_in,
                                    int fd_out, int fd_err, bool group) {
    if (os_shell_kind(kind) == NULL || command == NULL) return -22;
    /* The bash tool historically ran with a minimal environment so /bin/sh
     * also works when the parent passes almost nothing; keep that exact list
     * and ordering (see the Windows adapter for why it inherits instead). */
    const char *keys[] = { "PATH", "HOME", "TMPDIR", "TERM" };
    char *envp[5];
    size_t nenv = 0;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const char *v = os_getenv(keys[i]);
        if (!v) continue;
        size_t klen = agentc_strlen(keys[i]);
        size_t vlen = agentc_strlen(v);
        char *s = agentc_alloc(klen + 1 + vlen + 1);
        agentc_memcpy(s, keys[i], klen);
        s[klen] = '=';
        agentc_memcpy(s + klen + 1, v, vlen + 1);
        envp[nenv++] = s;
    }
    envp[nenv] = NULL;
    char *argv[4];
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-lc";
    argv[2] = (char *)command;
    argv[3] = NULL;
    int pid = group ? os_spawn_group(argv, envp, NULL, fd_in, fd_out, fd_err, 0)
                    : os_spawn(argv, envp, NULL, fd_in, fd_out, fd_err, 0);
    for (size_t i = 0; i < nenv; i++) agentc_free(envp[i]);
    return pid;
}

int os_spawn_shell(const char *kind, const char *command, int fd_in, int fd_out,
                   int fd_err) {
    return linux_spawn_shell_common(kind, command, fd_in, fd_out, fd_err, false);
}

int os_spawn_shell_group(const char *kind, const char *command, int fd_in, int fd_out,
                         int fd_err) {
    return linux_spawn_shell_common(kind, command, fd_in, fd_out, fd_err, true);
}

/* ---------------------------------------------------------------- terminal */
struct agentc_winsize { u16 rows, cols, xpix, ypix; };

int os_tty_size(int fd, int *cols, int *rows) {
    struct agentc_winsize ws = { 0, 0, 0, 0 };
    long r = linux_sc3(SYS_ioctl, fd, 0x5413 /* TIOCGWINSZ */, (long)&ws);
    if (r < 0) return (int)r;
    *cols = ws.cols;
    *rows = ws.rows;
    return 0;
}

/* TCGETS answers only for a tty and never consults stdout, unlike os_tty_size,
 * so it is the honest input-side test. The buffer just has to be writable. */
int os_tty_isatty(int fd) {
    u8 st[64];
    long r = linux_sc3(SYS_ioctl, fd, 0x5401 /* TCGETS */, (long)st);
    return r < 0 ? 0 : 1;
}

/* Kernel termios: 4 flag words, 1 line discipline byte, 19 control chars, then
 * speeds (offsets 0/4/8/12, c_cc at 17; VMIN=6, VTIME=5 for asm-generic). */
int os_tty_raw(int fd, void **saved) {
    u8 *st = agentc_alloc(64);
    long r = linux_sc3(SYS_ioctl, fd, 0x5401 /* TCGETS */, (long)st);
    if (r < 0) { agentc_free(st); return (int)r; }
    /* Keep the untouched settings for os_tty_restore(); `st` is then modified in
     * place. (Saving the modified copy re-applied raw mode on restore, which
     * left the user's shell without echo/canonical mode.) */
    u8 *orig = agentc_alloc(64);
    agentc_memcpy(orig, st, 64);
    u32 *iflag = (u32 *)(st + 0);
    u32 *oflag = (u32 *)(st + 4);
    u32 *lflag = (u32 *)(st + 12);
    u8 *cc = st + 17;
    *lflag &= ~(0x1u /*ISIG*/ | 0x2u /*ICANON*/ | 0x8u /*ECHO*/ | 0x8000u /*IEXTEN*/);
    *iflag &= ~(0x400u /*IXON*/ | 0x100u /*ICRNL*/ | 0xC000u /*BRKINT|PARMRK*/ | 0x20u /*ISTRIP*/);
    *oflag &= ~0x1u /*OPOST*/;
    cc[6] = 1;  /* VMIN */
    cc[5] = 0;  /* VTIME */
    r = linux_sc3(SYS_ioctl, fd, 0x5402 /* TCSETS */, (long)st);
    agentc_free(st);
    if (r < 0) { agentc_free(orig); return (int)r; }
    *saved = orig;
    return 0;
}

int os_tty_restore(int fd, void *saved) {
    if (!saved) return -22 /*EINVAL*/;
    long r = linux_sc3(SYS_ioctl, fd, 0x5402 /* TCSETS */, (long)saved);
    agentc_free(saved);
    return (int)r;
}

int os_sig_install(int sig, void (*handler)(int)) {
    long r = linux_rt_sigaction(sig, (long)handler);
    return r < 0 ? (int)r : 0;
}

int os_sig_winch(void (*handler)(void)) {
    (void)handler;
    return -38; /* ENOSYS: installed with the TUI (M4) */
}

/* os_open_url(url): hand a URL to the desktop's opener.
 *
 * Resolve xdg-open through PATH ourselves instead of spawning a shell: when it
 * is absent (a bare SSH/NixOS box, a container) there is nothing to try, so we
 * report -ENOENT and stay quiet rather than letting the shell spew "command not
 * found" noise. The caller has already printed the URL. Passing the URL as a
 * real argv entry also rules out shell injection.
 *
 * When xdg-open exists but finds no browser backend it prints its own errors to
 * stderr ("www-browser: command not found", "no method available"). Those are
 * not actionable - the URL is already on screen - so the opener's stdout/stderr
 * go to /dev/null and the login flow stays clean either way. */
int os_open_url(const char *url) {
    char opener[4096];
    if (os_which("xdg-open", opener, sizeof opener) != 0) return -2; /* ENOENT */
    int devnull = os_open("/dev/null", OS_O_WRONLY, 0);
    char *argv[3] = { opener, (char *)url, NULL };
    int pid = os_spawn(argv, g_envp, NULL, -1, devnull, devnull, -1);
    if (devnull >= 0) os_close(devnull);
    return pid < 0 ? pid : 0; /* fire and forget */
}

/* ----------------------------------------------------- time, entropy, poll */
i64 os_now_ns(int clock) {
    struct { i64 sec; i64 nsec; } ts = { 0, 0 };
    long r = linux_sc2(SYS_clock_gettime, clock, (long)&ts);
    if (r < 0) return r;
    return ts.sec * 1000000000 + ts.nsec;
}

void os_sleep_ns(i64 ns) {
    struct { i64 sec; i64 nsec; } ts, rem;
    ts.sec = ns / 1000000000;
    ts.nsec = ns % 1000000000;
    /* after EINTR nanosleep leaves the remainder in the second buffer */
    while (linux_sc2(SYS_nanosleep, (long)&ts, (long)&rem) == -4 /*EINTR*/) ts = rem;
}

int os_random(void *buf, size_t n) {
    u8 *p = buf;
    while (n) {
        long r = linux_sc3(SYS_getrandom, (long)p, (long)n, 0);
        if (r < 0) {
            if (r == -4 /*EINTR*/) continue;
            return (int)r;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int os_poll(struct os_pollfd *fds, int nfds, int timeout_ms) {
    return (int)linux_poll((void *)fds, nfds, timeout_ms);
}
