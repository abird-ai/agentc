/* plat/linux/sys.c — Layer 0 on raw x86-64 Linux syscalls (no libc).
 *
 * Errors are returned as negative errno values exactly as the kernel produces
 * them, which is the convention every adapter must follow (see plat.h).
 */
#include "agentc.h"
#include "plat.h"

#include "plat/linux/syscall.h"

#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200
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

/* Process argv/envp, filled by os_init() from the kernel stack image or by
 * os_embed_init() from the host's own argc/argv. The boot stub in boot.c reads
 * these directly (it must own the only reference to agentc_main, which is
 * excluded from the embeddable archive). */
long os_argc;
char **os_argv;
char **os_envp;
bool os_inited;

/* ---------------------------------------------------------------- bootstrap */
const char *os_platform(void) {
    return "linux";
}

void os_init(void *stack) {
    long *sp = stack;
    os_argc = sp[0];
    os_argv = (char **)(sp + 1);
    os_envp = os_argv + os_argc + 1;
    os_inited = true;
}

/* Embedding seam (plat.h): a host that already owns the process (no kernel
 * entry stub) publishes its argv/envp here. The arrays are borrowed and must
 * outlive the engine. */
void os_embed_init(char **argv, char **envp) {
    static char *no_env[] = {NULL};
    long argc = 0;
    if (argv) {
        while (argv[argc])
            argc++;
    } else {
        static char *no_argv[] = {NULL};
        argv = no_argv;
    }
    os_argc = argc;
    os_argv = argv;
    os_envp = envp ? envp : no_env;
    os_inited = true;
}

void os_exit(int code) {
    linux_sc1(SYS_exit_group, code);
    __builtin_unreachable();
}

const char *os_getenv(const char *name) {
    size_t n = agentc_strlen(name);
    if (!os_inited)
        return NULL;
    for (char **e = os_envp; *e; e++) {
        if (agentc_str_eq(*e, n, name, n) && (*e)[n] == '=')
            return *e + n + 1;
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
    if (flags & OS_O_DIRECTORY)
        flags = (flags & ~OS_O_DIRECTORY) | 0x4000;
#endif
    return (int)linux_sc4(SYS_openat, AT_FDCWD, (long)path, flags, mode);
}
int os_read(int fd, void *buf, size_t n) {
    return (int)linux_sc3(SYS_read, fd, (long)buf, (long)n);
}
int os_write(int fd, const void *buf, size_t n) {
    return (int)linux_sc3(SYS_write, fd, (long)buf, (long)n);
}
int os_close(int fd) {
    return (int)linux_sc1(SYS_close, fd);
}
i64 os_lseek(int fd, i64 off, int whence) {
    return linux_sc3(SYS_lseek, fd, off, whence);
}
int os_ftruncate(int fd, i64 len) {
    return (int)linux_sc2(SYS_ftruncate, fd, len);
}
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
    int st_blksize; /* asm-generic spells these `int`, not an i64 */
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
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
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
    if (r < 0)
        return (int)r;
    linux_stat_fixup(statbuf, &g);
    return 0;
}
int os_stat(const char *path, void *statbuf) {
    struct linux_stat_native g;
    long r = linux_sc4(SYS_newfstatat, AT_FDCWD, (long)path, (long)&g, 0);
    if (r < 0)
        return (int)r;
    linux_stat_fixup(statbuf, &g);
    return 0;
}
int os_lstat(const char *path, void *statbuf) {
    struct linux_stat_native g;
    /* AT_SYMLINK_NOFOLLOW (0x100) is the asm-generic value on every arch the
     * syscall table here covers. */
    long r = linux_sc4(SYS_newfstatat, AT_FDCWD, (long)path, (long)&g,
                       0x100 /* AT_SYMLINK_NOFOLLOW */);
    if (r < 0)
        return (int)r;
    linux_stat_fixup(statbuf, &g);
    return 0;
}
int os_mkdir(const char *path, int mode) {
    return (int)linux_sc3(SYS_mkdirat, AT_FDCWD, (long)path, mode);
}
int os_unlink(const char *path) {
    return (int)linux_sc3(SYS_unlinkat, AT_FDCWD, (long)path, 0);
}
int os_rmdir(const char *path) {
    return (int)linux_sc3(SYS_unlinkat, AT_FDCWD, (long)path, AT_REMOVEDIR);
}
int os_rename(const char *from, const char *to) {
    /* renameat2(from, to, 0): riscv64 has no renameat(2) syscall at all (see
     * syscall.h); flags=0 is plain rename semantics on every Linux arch. */
    return (int)linux_sc5(SYS_renameat2, AT_FDCWD, (long)from, AT_FDCWD, (long)to, 0);
}
int os_getcwd(char *buf, size_t len) {
    return (int)linux_sc2(SYS_getcwd, (long)buf, (long)len);
}
int os_chmod(const char *path, int mode) {
    return (int)linux_chmod(path, mode);
}
u32 os_getuid(void) {
    return (u32)linux_sc1(SYS_getuid, 0);
}
int os_getpid(void) {
    return (int)linux_sc1(SYS_getpid, 0);
}
int os_getdents(int fd, void *buf, size_t len) {
    return (int)linux_sc3(SYS_getdents64, fd, (long)buf, (long)len);
}

/* ------------------------------------------------------------- PATH lookup */
#define WHICH_CWD_MAX 4096

/* A candidate is a hit only when it is a regular file with at least one
 * execute bit. On success the absolute path is copied into out. */
static int which_try(const char *cand, char *out, size_t cap) {
    struct os_stat st;
    if (os_stat(cand, &st) != 0)
        return -2; /* ENOENT */
    if ((st.st_mode & 0170000u) != 0100000u)
        return -2; /* not S_IFREG */
    if (!(st.st_mode & 0111u))
        return -2; /* not executable */
    if (cand[0] == '/') {
        size_t n = agentc_strlen(cand);
        if (n + 1 > cap)
            return -34; /* ERANGE */
        agentc_memcpy(out, cand, n + 1);
        return 0;
    }
    char cwd[WHICH_CWD_MAX];
    int r = os_getcwd(cwd, sizeof cwd);
    if (r < 0)
        return r;
    size_t cl = agentc_strlen(cwd);
    size_t nl = agentc_strlen(cand);
    if (cl + 1 + nl + 1 > cap)
        return -34;
    agentc_memcpy(out, cwd, cl);
    out[cl] = '/';
    agentc_memcpy(out + cl + 1, cand, nl + 1);
    return 0;
}

int os_which_in(const char *path_env, const char *name, char *out, size_t cap) {
    if (name == NULL || name[0] == 0)
        return -2; /* ENOENT */
    /* A name with a slash is checked as given, relative to the cwd. */
    for (const char *p = name; *p != 0; p++) {
        if (*p == '/')
            return which_try(name, out, cap);
    }
    if (path_env == NULL || path_env[0] == 0)
        return -2; /* no PATH search */
    size_t nl = agentc_strlen(name);
    for (const char *p = path_env;;) {
        const char *end = p;
        while (*end != 0 && *end != ':')
            end++;
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
        if (r == 0 || r == -34)
            return r; /* hit, or the hit does not fit */
        if (*end == 0)
            break;
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
    if (r < 0 && r > -4096)
        return NULL;
    return (u8 *)r;
}
u8 *os_map(size_t n) {
    u8 *p = os_map_try(n);
    if (!p)
        agentc_die("out of memory");
    return p;
}
int os_unmap(u8 *p, size_t n) {
    return (int)linux_sc2(SYS_munmap, (long)p, (long)n);
}

/* ------------------------------------------------------------- processes */
static int linux_spawn_common(char *const argv[], char *const envp[], const char *cwd, int fd_in,
                              int fd_out, int fd_err, int ctty, bool group) {
    (void)ctty; /* controlling-tty setup arrives with the TUI (M4) */
    long pid = linux_fork();
    if (pid < 0)
        return (int)pid;
    if (pid == 0) {
        /* New process group before exec, so os_kill(-pid) sees the whole tree.
         * A failure means the child is already a leader; ignore it. */
        if (group)
            (void)linux_sc2(SYS_SETPGID, 0, 0);
        if (fd_in >= 0)
            linux_dup2(fd_in, 0);
        if (fd_out >= 0)
            linux_dup2(fd_out, 1);
        if (fd_err >= 0)
            linux_dup2(fd_err, 2);
        if (cwd)
            linux_sc1(SYS_chdir, (long)cwd);
        linux_sc3(SYS_execve, (long)argv[0], (long)argv, (long)envp);
        linux_sc1(SYS_exit_group, 127);
        __builtin_unreachable();
    }
    /* Establish the group in the parent as well as the child: a caller that
     * signals -pid immediately after spawn must not race the child's own
     * setpgid. EACCES (the child already exec'd) and ESRCH (it already exited)
     * are both fine to ignore. */
    if (group)
        (void)linux_sc2(SYS_SETPGID, pid, pid);
    return (int)pid;
}

int os_spawn(char *const argv[], char *const envp[], const char *cwd, int fd_in, int fd_out,
             int fd_err, int ctty) {
    return linux_spawn_common(argv, envp, cwd, fd_in, fd_out, fd_err, ctty, false);
}

int os_spawn_group(char *const argv[], char *const envp[], const char *cwd, int fd_in, int fd_out,
                   int fd_err, int ctty) {
    return linux_spawn_common(argv, envp, cwd, fd_in, fd_out, fd_err, ctty, true);
}

int os_wait(int pid, bool nohang) {
    int status = 0;
    long r = linux_sc4(SYS_wait4, pid, (long)&status, nohang ? 1 : 0, 0);
    if (r > 0)
        return status;
    if (r == 0)
        return -1; /* still running */
    return -2;     /* unknown pid */
}

int os_kill(int pid, int sig) {
    long r = linux_sc2(SYS_kill, pid, sig);
    /* Negative pid: kill the process group; when it no longer exists fall back
     * to the leader pid so a non-group child still dies on the first signal. */
    if (r < 0 && pid < 0)
        r = linux_sc2(SYS_kill, -pid, sig);
    return (int)r;
}

int os_pipe(int fds[2]) {
    int f[2];
    long r = linux_sc2(SYS_pipe2, (long)f, O_CLOEXEC);
    if (r < 0)
        return (int)r;
    // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
    long fl = linux_sc3(SYS_fcntl, f[0], F_GETFL, 0);
    if (fl >= 0)
        linux_sc3(SYS_fcntl, f[0], F_SETFL, fl | O_NONBLOCK);
    fds[0] = f[0];
    fds[1] = f[1];
    return 0;
}

/* Clear or set O_NONBLOCK on the open file description. The same shape the
 * sandbox status pipe uses (above); the flag is shared with dup/fork. */
int os_set_nonblock(int fd, bool on) {
    long fl = linux_sc3(SYS_fcntl, fd, F_GETFL, 0);
    if (fl < 0)
        return (int)fl;
    long want = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    long r = linux_sc3(SYS_fcntl, fd, F_SETFL, want);
    return r < 0 ? (int)r : 0;
}

/* ------------------------------------------------------------- shell tool */
/* POSIX has exactly one reasonable shell. The Windows kinds are rejected with
 * EINVAL so a config copied from Windows fails loudly instead of silently
 * running sh with cmd syntax. */
const char *os_shell_kind(const char *kind) {
    if (kind == NULL || kind[0] == 0 || agentc_streq(kind, "auto") || agentc_streq(kind, "sh"))
        return "sh";
    return NULL;
}

int os_shell_resolve(const char *kind, char *out, size_t cap) {
    if (os_shell_kind(kind) == NULL)
        return -22; /* EINVAL */
    if (cap < sizeof "/bin/sh")
        return -36; /* ENAMETOOLONG */
    agentc_memcpy(out, "/bin/sh", sizeof "/bin/sh");
    return 0;
}

static int linux_spawn_shell_common(const char *kind, const char *command, int fd_in, int fd_out,
                                    int fd_err, bool group) {
    if (os_shell_kind(kind) == NULL || command == NULL)
        return -22;
    /* The bash tool historically ran with a minimal environment so /bin/sh
     * also works when the parent passes almost nothing; keep that exact list
     * and ordering (see the Windows adapter for why it inherits instead). */
    const char *keys[] = {"PATH", "HOME", "TMPDIR", "TERM"};
    char *envp[5];
    size_t nenv = 0;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const char *v = os_getenv(keys[i]);
        if (!v)
            continue;
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
    for (size_t i = 0; i < nenv; i++)
        agentc_free(envp[i]);
    return pid;
}

int os_spawn_shell(const char *kind, const char *command, int fd_in, int fd_out, int fd_err) {
    return linux_spawn_shell_common(kind, command, fd_in, fd_out, fd_err, false);
}

int os_spawn_shell_group(const char *kind, const char *command, int fd_in, int fd_out, int fd_err) {
    return linux_spawn_shell_common(kind, command, fd_in, fd_out, fd_err, true);
}

/* ---------------------------------------------------------------- terminal */
struct agentc_winsize {
    u16 rows, cols, xpix, ypix;
};

int os_tty_size(int fd, int *cols, int *rows) {
    struct agentc_winsize ws = {0, 0, 0, 0};
    long r = linux_sc3(SYS_ioctl, fd, 0x5413 /* TIOCGWINSZ */, (long)&ws);
    if (r < 0)
        return (int)r;
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
    if (r < 0) {
        agentc_free(st);
        return (int)r;
    }
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
    cc[6] = 1; /* VMIN */
    cc[5] = 0; /* VTIME */
    r = linux_sc3(SYS_ioctl, fd, 0x5402 /* TCSETS */, (long)st);
    agentc_free(st);
    if (r < 0) {
        agentc_free(orig);
        return (int)r;
    }
    *saved = orig;
    return 0;
}

int os_tty_restore(int fd, void *saved) {
    if (!saved)
        return -22 /*EINVAL*/;
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
    if (os_which("xdg-open", opener, sizeof opener) != 0)
        return -2; /* ENOENT */
    int devnull = os_open("/dev/null", OS_O_WRONLY, 0);
    char *argv[3] = {opener, (char *)url, NULL};
    int pid = os_spawn(argv, os_envp, NULL, -1, devnull, devnull, -1);
    if (devnull >= 0)
        os_close(devnull);
    return pid < 0 ? pid : 0; /* fire and forget */
}

/* ----------------------------------------------------- time, entropy, poll */
i64 os_now_ns(int clock) {
    struct {
        i64 sec;
        i64 nsec;
    } ts = {0, 0};
    if (clock == OS_CLOCK_CPUTIME)
        clock = 2; /* CLOCK_PROCESS_CPUTIME_ID */
    long r = linux_sc2(SYS_clock_gettime, clock, (long)&ts);
    if (r < 0)
        return r;
    return ts.sec * 1000000000 + ts.nsec;
}

void os_sleep_ns(i64 ns) {
    struct {
        i64 sec;
        i64 nsec;
    } ts, rem;
    ts.sec = ns / 1000000000;
    ts.nsec = ns % 1000000000;
    /* after EINTR nanosleep leaves the remainder in the second buffer */
    while (linux_sc2(SYS_nanosleep, (long)&ts, (long)&rem) == -4 /*EINTR*/)
        ts = rem;
}

int os_random(void *buf, size_t n) {
    u8 *p = buf;
    while (n) {
        long r = linux_sc3(SYS_getrandom, (long)p, (long)n, 0);
        if (r < 0) {
            if (r == -4 /*EINTR*/)
                continue;
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

/* ------------------------------------------------------------- PTY (P6-A) */
/* /dev/ptmx + TIOCSPTLCK/TIOCGPTN + /dev/pts/N, then TIOCSWINSZ/GWINSZ. The
 * ioctl request numbers are the asm-generic encodings (identical on x86-64,
 * aarch64 and riscv64). */
#define PTY_TIOCGPTN 0x80045430u
#define PTY_TIOCSPTLCK 0x40045431u
#define PTY_TIOCSWINSZ 0x5414u
#define PTY_TIOCGWINSZ 0x5413u
#define PTY_O_NOCTTY 0x100

int os_pty_resize(int fd, int cols, int rows) {
    struct agentc_winsize ws;
    ws.rows = (u16)(rows > 0 ? rows : 0);
    ws.cols = (u16)(cols > 0 ? cols : 0);
    ws.xpix = 0;
    ws.ypix = 0;
    long r = linux_sc3(SYS_ioctl, fd, PTY_TIOCSWINSZ, (long)&ws);
    return r < 0 ? (int)r : 0;
}

int os_pty_size(int fd, int *cols, int *rows) {
    struct agentc_winsize ws = {0, 0, 0, 0};
    long r = linux_sc3(SYS_ioctl, fd, PTY_TIOCGWINSZ, (long)&ws);
    if (r < 0)
        return (int)r;
    *cols = ws.cols;
    *rows = ws.rows;
    return 0;
}

int os_pty_open(int *master, int *slave, int cols, int rows) {
    int m = os_open("/dev/ptmx", OS_O_RDWR | PTY_O_NOCTTY | OS_O_CLOEXEC | OS_O_NONBLOCK, 0);
    if (m < 0)
        return m;
    int zero = 0;
    if (linux_sc3(SYS_ioctl, m, PTY_TIOCSPTLCK, (long)&zero) < 0) {
        os_close(m);
        return -5;
    }
    unsigned int ptn = 0;
    if (linux_sc3(SYS_ioctl, m, PTY_TIOCGPTN, (long)&ptn) < 0) {
        os_close(m);
        return -5;
    }
    char path[64];
    int n = agentc_snprintf(path, sizeof path, "/dev/pts/%u", ptn);
    if (n <= 0 || (size_t)n >= sizeof path) {
        os_close(m);
        return -36;
    }
    int s = os_open(path, OS_O_RDWR | PTY_O_NOCTTY, 0);
    if (s < 0) {
        os_close(m);
        return s;
    }
    if (cols > 0 && rows > 0)
        (void)os_pty_resize(m, cols, rows);
    *master = m;
    *slave = s;
    return 0;
}

/* -------------------------------------------------------- freeze (P6-A) */
int os_process_freeze(int pid, bool group) {
    return os_kill(group ? -pid : pid, OS_SIGSTOP);
}

int os_process_resume(int pid, bool group) {
    return os_kill(group ? -pid : pid, OS_SIGCONT);
}

int os_process_checkpoint(int pid, const char *dir, char *err, size_t errcap) {
    (void)pid;
    (void)dir;
    if (err != NULL && errcap > 0)
        agentc_snprintf(err, errcap, "checkpoint backend unavailable (CRIU not built)");
    return -38; /* ENOSYS */
}

/* ------------------------------------------------------- sandbox (P6-C) */
/* os_realpath canonicalizes via the kernel: open the path (O_PATH resolves
 * `.`/`..`/symlinks) and read /proc/self/fd/N. A missing leaf falls back to the
 * canonical parent plus the basename, then to a purely lexical normalizer so a
 * check never silently passes on an unresolvable path. */
#ifndef O_PATH
#define O_PATH 0x200000
#endif

static int lexical_normalize(const char *abs, char *out, size_t cap) {
    if (!abs || abs[0] != '/')
        return -22;
    size_t o = 0;
    if (cap < 2)
        return -36;
    out[o++] = '/';
    const char *p = abs;
    while (*p == '/')
        p++;
    while (*p) {
        const char *e = p;
        while (*e && *e != '/')
            e++;
        size_t n = (size_t)(e - p);
        if (n == 1 && p[0] == '.') {
            /* skip */
        } else if (n == 2 && p[0] == '.' && p[1] == '.') {
            while (o > 1 && out[o - 1] != '/')
                o--;
            if (o > 1)
                o--; /* drop the separator too */
            if (o == 0)
                out[o++] = '/';
        } else if (n > 0) {
            if (o > 1 && out[o - 1] != '/') {
                if (o + 1 >= cap)
                    return -36;
                out[o++] = '/';
            }
            if (o + n + 1 >= cap)
                return -36;
            agentc_memcpy(out + o, p, n);
            o += n;
        }
        while (*e == '/')
            e++;
        p = e;
    }
    if (o == 0)
        out[o++] = '/';
    out[o] = 0;
    return 0;
}

static int proc_realpath(const char *abs, char *out, size_t cap) {
    int fd = os_open(abs, O_PATH | O_CLOEXEC, 0);
    if (fd < 0)
        return fd;
    char link[64];
    int n = agentc_snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    if (n <= 0 || (size_t)n >= sizeof link) {
        os_close(fd);
        return -2;
    }
    long r = linux_sc4(SYS_readlinkat, AT_FDCWD, (long)link, (long)out, (long)(cap - 1));
    os_close(fd);
    if (r <= 0)
        return r == 0 ? -2 : (int)r;
    if ((size_t)r >= cap)
        return -36;
    out[r] = 0;
    return 0;
}

int os_realpath(const char *path, char *out, size_t cap) {
    if (!path || !path[0] || !out || cap == 0)
        return -22;
    char abs[4096];
    if (path[0] == '/') {
        size_t n = agentc_strlen(path);
        if (n + 1 > sizeof abs)
            return -36;
        agentc_memcpy(abs, path, n + 1);
    } else {
        char cwd[4096];
        int r = os_getcwd(cwd, sizeof cwd);
        if (r < 0)
            return r;
        int n = agentc_snprintf(abs, sizeof abs, "%s/%s", cwd, path);
        if (n <= 0 || (size_t)n >= sizeof abs)
            return -36;
    }
    if (proc_realpath(abs, out, cap) == 0)
        return 0;
    /* missing leaf: canonicalize the parent and re-attach the basename */
    size_t al = agentc_strlen(abs);
    while (al > 1 && abs[al - 1] == '/')
        abs[--al] = 0;
    const char *slash = NULL;
    for (size_t i = al; i > 0; i--)
        if (abs[i - 1] == '/') {
            slash = abs + i - 1;
            break;
        }
    if (slash) {
        size_t plen = (size_t)(slash - abs);
        if (plen == 0)
            plen = 1;
        char parent[4096], rp[4096];
        if (plen < sizeof parent) {
            agentc_memcpy(parent, abs, plen);
            parent[plen] = 0;
            if (proc_realpath(parent, rp, sizeof rp) == 0) {
                int n = agentc_snprintf(out, cap, "%s%s", rp, slash);
                if (n > 0 && (size_t)n < cap)
                    return 0;
                return -36;
            }
        }
    }
    return lexical_normalize(abs, out, cap);
}

/* --- Landlock / seccomp constants (freestanding: no linux headers) --- */
#define LL_CREATE_RULESET_VERSION 1u
#define LL_RULE_PATH_BENEATH 1
#define LL_FS_EXECUTE (1ull << 0)
#define LL_FS_WRITE_FILE (1ull << 1)
#define LL_FS_READ_FILE (1ull << 2)
#define LL_FS_READ_DIR (1ull << 3)
#define LL_FS_REMOVE_DIR (1ull << 4)
#define LL_FS_REMOVE_FILE (1ull << 5)
#define LL_FS_MAKE_CHAR (1ull << 6)
#define LL_FS_MAKE_DIR (1ull << 7)
#define LL_FS_MAKE_REG (1ull << 8)
#define LL_FS_MAKE_SOCK (1ull << 9)
#define LL_FS_MAKE_FIFO (1ull << 10)
#define LL_FS_MAKE_BLOCK (1ull << 11)
#define LL_FS_MAKE_SYM (1ull << 12)
#define LL_FS_REFER (1ull << 13)
#define LL_FS_TRUNCATE (1ull << 14)

struct ll_ruleset_attr {
    u64 handled_access_fs;
    u64 handled_access_net;
    u64 scoped;
};
struct ll_path_attr {
    u64 allowed_access;
    int parent_fd;
} __attribute__((packed));

struct sc_filter {
    u16 code;
    u8 jt;
    u8 jf;
    u32 k;
};
struct sc_prog {
    u16 len;
    struct sc_filter *filter;
};
#define SC_LD_W_ABS 0x20u
#define SC_JMP_JEQ_K 0x15u
#define SC_RET_K 0x06u
#define SC_RET_ALLOW 0x7fff0000u
#define SC_RET_ERRNO 0x00050000u

static int g_sandbox_test_backend;
static char g_sandbox_summary[192];

void os_sandbox_set_test_backend(int kind) {
    g_sandbox_test_backend = kind;
}
const char *os_sandbox_last_summary(void) {
    return g_sandbox_summary;
}

static void sandbox_record(const AgcSandboxSpec *spec) {
    agentc_snprintf(g_sandbox_summary, sizeof g_sandbox_summary,
                    "access=%d boundary=%d network=%d roots=%llu hosts=%llu",
                    spec ? spec->access : -1, spec ? spec->boundary : -1, spec ? spec->network : -1,
                    (unsigned long long)(spec ? spec->nroots : 0),
                    (unsigned long long)(spec ? spec->nhosts : 0));
}

#define PR_SET_NO_NEW_PRIVS 38
#define SC_SET_MODE_FILTER 1u

static int sandbox_seccomp_net(void) {
    struct sc_filter f[7];
    f[0].code = SC_LD_W_ABS;
    f[0].jt = 0;
    f[0].jf = 0;
    f[0].k = 0; /* nr */
    f[1].code = SC_JMP_JEQ_K;
    f[1].jt = 0;
    f[1].jf = 4;
    f[1].k = SYS_socket;
    f[2].code = SC_LD_W_ABS;
    f[2].jt = 0;
    f[2].jf = 0;
    f[2].k = 16; /* args[0] */
    f[3].code = SC_JMP_JEQ_K;
    f[3].jt = 1;
    f[3].jf = 0;
    f[3].k = 2; /* AF_INET */
    f[4].code = SC_JMP_JEQ_K;
    f[4].jt = 0;
    f[4].jf = 1;
    f[4].k = 10; /* AF_INET6 */
    f[5].code = SC_RET_K;
    f[5].jt = 0;
    f[5].jf = 0;
    f[5].k = SC_RET_ERRNO | 1;
    f[6].code = SC_RET_K;
    f[6].jt = 0;
    f[6].jf = 0;
    f[6].k = SC_RET_ALLOW;
    struct sc_prog p = {7, f};
    long r = linux_sc5(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    if (r < 0)
        return (int)r;
    r = linux_sc3(SYS_seccomp, SC_SET_MODE_FILTER, 0, (long)&p);
    return r < 0 ? (int)r : 0;
}

static int landlock_probe(void) {
    long r = linux_sc3(SYS_landlock_create_ruleset, 0, 0, LL_CREATE_RULESET_VERSION);
    return r < 0 ? (int)r : (int)r;
}

int os_sandbox_available(const AgcSandboxSpec *spec) {
    if (!spec || spec->boundary == 0 /* AGENTC_BOUND_NONE */)
        return 0;
    if (g_sandbox_test_backend == 1)
        return -38; /* ENOSYS */
    if (spec->boundary == 2 /* strict */)
        return -38;
    int abi = landlock_probe();
    if (abi < 1)
        return -38;
    return 0;
}

int os_sandbox_apply(const AgcSandboxSpec *spec, char *err, size_t errcap) {
    if (err && errcap)
        err[0] = 0;
    if (!spec || spec->boundary == 0)
        return 0; /* none = the only unsandboxed mode */
    if (g_sandbox_test_backend == 1) {
        if (err && errcap)
            agentc_snprintf(err, errcap, "sandbox backend forced unavailable");
        return -38;
    }
    if (g_sandbox_test_backend == 2) {
        sandbox_record(spec);
        return 0;
    }
    if (spec->boundary == 2) {
        if (err && errcap)
            agentc_snprintf(err, errcap, "strict sandbox backend is not available");
        return -38;
    }
    int abi = landlock_probe();
    if (abi < 1) {
        if (err && errcap)
            agentc_snprintf(err, errcap, "Landlock unavailable on this kernel");
        return -38;
    }
    u64 handled = LL_FS_WRITE_FILE | LL_FS_REMOVE_DIR | LL_FS_REMOVE_FILE | LL_FS_MAKE_CHAR |
                  LL_FS_MAKE_DIR | LL_FS_MAKE_REG | LL_FS_MAKE_SOCK | LL_FS_MAKE_FIFO |
                  LL_FS_MAKE_BLOCK | LL_FS_MAKE_SYM;
    if (abi >= 2)
        handled |= LL_FS_REFER;
    if (abi >= 3)
        handled |= LL_FS_TRUNCATE;
    struct ll_ruleset_attr attr;
    agentc_memset(&attr, 0, sizeof attr);
    attr.handled_access_fs = handled;
    int rs = (int)linux_sc3(SYS_landlock_create_ruleset, (long)&attr, (long)sizeof attr, 0);
    if (rs < 0) {
        if (err && errcap)
            agentc_snprintf(err, errcap, "Landlock ruleset failed (%d)", rs);
        return rs;
    }
    int rc = 0;
    if (spec->access != 0 /* RO: no writable paths at all */) {
        for (size_t i = 0; i < spec->nroots && rc == 0; i++) {
            int fd = os_open(spec->writable_roots[i], O_PATH | O_CLOEXEC, 0);
            if (fd < 0)
                continue; /* a missing root grants nothing */
            struct ll_path_attr pa = {handled, fd};
            long r = linux_sc4(SYS_landlock_add_rule, rs, LL_RULE_PATH_BENEATH, (long)&pa, 0);
            os_close(fd);
            if (r < 0) {
                rc = (int)r;
                break;
            }
        }
        if (rc == 0 && !spec->exclude_tmp) {
            const char *tmp = os_getenv("TMPDIR");
            if (!tmp || !tmp[0])
                tmp = "/tmp";
            int fd = os_open(tmp, O_PATH | O_CLOEXEC, 0);
            if (fd >= 0) {
                struct ll_path_attr pa = {handled, fd};
                long r = linux_sc4(SYS_landlock_add_rule, rs, LL_RULE_PATH_BENEATH, (long)&pa, 0);
                os_close(fd);
                if (r < 0)
                    rc = (int)r;
            }
        }
    }
    if (rc == 0) {
        long r = linux_sc5(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        if (r < 0)
            rc = (int)r;
    }
    if (rc == 0) {
        long r = linux_sc2(SYS_landlock_restrict_self, rs, 0);
        if (r < 0)
            rc = (int)r;
    }
    os_close(rs);
    if (rc != 0) {
        if (err && errcap)
            agentc_snprintf(err, errcap, "Landlock apply failed (%d)", rc);
        return rc;
    }
    if (spec->network == 0 /* DENY */) {
        int sr = sandbox_seccomp_net();
        if (sr != 0) {
            if (err && errcap)
                agentc_snprintf(err, errcap, "network sandbox failed (%d)", sr);
            return sr;
        }
    }
    /* network=allowlist: Landlock network rules (ABI 4+) are scoped to TCP
     * bind/connect *ports*, not to hosts, so a per-host allowlist cannot be
     * enforced by the kernel here. It stays a Layer-2 check (only declared
     * network tools run) while network=deny remains kernel-enforced by the
     * seccomp filter above. Documented in .agents/design/10-platform.md. */
    return 0;
}

/* --- sandboxed spawns --- */
static int linux_spawn_sandboxed_common(char *const argv[], char *const envp[], const char *cwd,
                                        int fd_in, int fd_out, int fd_err, int ctty, bool group,
                                        const AgcSandboxSpec *sandbox, char *err, size_t errcap) {
    (void)ctty;
    if (err && errcap)
        err[0] = 0;
    if (sandbox == NULL)
        return group ? os_spawn_group(argv, envp, cwd, fd_in, fd_out, fd_err, ctty)
                     : os_spawn(argv, envp, cwd, fd_in, fd_out, fd_err, ctty);
    int sp[2];
    int pr = os_pipe(sp);
    if (pr < 0)
        return pr;
    long fl = linux_sc3(SYS_fcntl, sp[0], F_GETFL, 0);
    if (fl >= 0)
        linux_sc3(SYS_fcntl, sp[0], F_SETFL, fl & ~O_NONBLOCK);
    long pid = linux_fork();
    if (pid < 0) {
        os_close(sp[0]);
        os_close(sp[1]);
        return (int)pid;
    }
    if (pid == 0) {
        os_close(sp[0]);
        if (group)
            (void)linux_sc2(SYS_SETPGID, 0, 0);
        if (fd_in >= 0)
            linux_dup2(fd_in, 0);
        if (fd_out >= 0)
            linux_dup2(fd_out, 1);
        if (fd_err >= 0)
            linux_dup2(fd_err, 2);
        char serr[128];
        int rc = os_sandbox_apply(sandbox, serr, sizeof serr);
        if (rc != 0) {
            int e = rc;
            (void)linux_sc3(SYS_write, sp[1], (long)&e, sizeof e);
            linux_sc1(SYS_exit_group, 125);
            __builtin_unreachable();
        }
        char z = 0;
        (void)linux_sc3(SYS_write, sp[1], (long)&z, 1);
        os_close(sp[1]);
        if (cwd)
            linux_sc1(SYS_chdir, (long)cwd);
        linux_sc3(SYS_execve, (long)argv[0], (long)argv, (long)envp);
        linux_sc1(SYS_exit_group, 127);
        __builtin_unreachable();
    }
    os_close(sp[1]);
    if (group)
        (void)linux_sc2(SYS_SETPGID, pid, pid);
    int e = 0;
    long n = linux_sc3(SYS_read, sp[0], (long)&e, sizeof e);
    os_close(sp[0]);
    if (n == (long)sizeof e) {
        (void)os_wait((int)pid, false);
        if (err && errcap)
            agentc_snprintf(err, errcap, "sandbox denied the spawn (%d)", e);
        return e;
    }
    if (n < 0) {
        os_kill(-(int)pid, 9);
        return (int)n;
    }
    return (int)pid;
}

int os_spawn_group_sandboxed(char *const argv[], char *const envp[], const char *cwd, int fd_in,
                             int fd_out, int fd_err, int ctty, const AgcSandboxSpec *sandbox,
                             char *err, size_t errcap) {
    return linux_spawn_sandboxed_common(argv, envp, cwd, fd_in, fd_out, fd_err, ctty, true, sandbox,
                                        err, errcap);
}

int os_spawn_shell_group_sandboxed(const char *kind, const char *command, int fd_in, int fd_out,
                                   int fd_err, const AgcSandboxSpec *sandbox, char *err,
                                   size_t errcap) {
    if (os_shell_kind(kind) == NULL || command == NULL)
        return -22;
    const char *keys[] = {"PATH", "HOME", "TMPDIR", "TERM"};
    char *envp[5];
    size_t nenv = 0;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const char *v = os_getenv(keys[i]);
        if (!v)
            continue;
        size_t klen = agentc_strlen(keys[i]);
        size_t vlen = agentc_strlen(v);
        char *s = agentc_alloc(klen + 1 + vlen + 1);
        agentc_memcpy(s, keys[i], klen);
        s[klen] = '=';
        agentc_memcpy(s + klen + 1, v, vlen + 1);
        envp[nenv++] = s;
    }
    envp[nenv] = NULL;
    char *argv[4] = {(char *)"/bin/sh", (char *)"-lc", (char *)command, NULL};
    int pid = linux_spawn_sandboxed_common(argv, envp, NULL, fd_in, fd_out, fd_err, 0, true,
                                           sandbox, err, errcap);
    for (size_t i = 0; i < nenv; i++)
        agentc_free(envp[i]);
    return pid;
}
