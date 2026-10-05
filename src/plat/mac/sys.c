/* plat/mac/sys.c — Layer 0 over libSystem (macOS arm64).
 *
 * Every entry point returns >= 0 or a negative Linux errno: Darwin errors go
 * through mac_errno(). We use the real Darwin structs internally (struct stat,
 * struct termios/NCCS, struct dirent, struct winsize) and convert at the
 * boundary:
 *
 *   - os_fstat/os_stat emit the portable struct os_stat the core reads
 *     (plat.h) by converting a Darwin struct stat;
 *   - os_getdents emits Linux dirent64 records from readdir();
 *   - os_poll's struct os_pollfd is layout-compatible with Darwin pollfd and
 *     the POLLIN/OUT/ERR/HUP values match POSIX (no translation).
 *
 * Directories are enumerated with fdopendir()/readdir() so the directory
 * cursor lives inside libSystem; os_close() closes the stream instead of the
 * raw fd. This avoids getdirentries()' in/out basep cursor bookkeeping.
 */
#include "agentc.h"
#include "plat.h"
#include "mac_internal.h"

/* ------------------------------------------------------------- errno table */
int mac_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EPERM: return -1;
    case ENOENT: return -2;
    case ESRCH: return -3;
    case EINTR: return -4;
    case EIO: return -5;
    case ENXIO: return -6;
    case E2BIG: return -7;
    case ENOEXEC: return -8;
    case EBADF: return -9;
    case ECHILD: return -10;
    case EDEADLK: return -35;
    case ENOMEM: return -12;
    case EACCES: return -13;
    case EFAULT: return -14;
    case ENOTBLK: return -15;
    case EBUSY: return -16;
    case EEXIST: return -17;
    case EXDEV: return -18;
    case ENODEV: return -19;
    case ENOTDIR: return -20;
    case EISDIR: return -21;
    case EINVAL: return -22;
    case ENFILE: return -23;
    case EMFILE: return -24;
    case ENOTTY: return -25;
    case ETXTBSY: return -26;
    case EFBIG: return -27;
    case ENOSPC: return -28;
    case ESPIPE: return -29;
    case EROFS: return -30;
    case EMLINK: return -31;
    case EPIPE: return -32;
    case EDOM: return -33;
    case ERANGE: return -34;
    case EAGAIN: return -11;
    case EINPROGRESS: return -115;
    case EALREADY: return -114;
    case ENOTSOCK: return -88;
    case EDESTADDRREQ: return -89;
    case EMSGSIZE: return -90;
    case EPROTOTYPE: return -91;
    case ENOPROTOOPT: return -92;
    case EPROTONOSUPPORT: return -93;
    case ESOCKTNOSUPPORT: return -94;
    case ENOTSUP: return -95;
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP: return -95;
#endif
    case EPFNOSUPPORT: return -96;
    case EAFNOSUPPORT: return -97;
    case EADDRINUSE: return -98;
    case EADDRNOTAVAIL: return -99;
    case ENETDOWN: return -100;
    case ENETUNREACH: return -101;
    case ENETRESET: return -102;
    case ECONNABORTED: return -103;
    case ECONNRESET: return -104;
    case ENOBUFS: return -105;
    case EISCONN: return -106;
    case ENOTCONN: return -107;
    case ESHUTDOWN: return -108;
    case ETOOMANYREFS: return -109;
    case ETIMEDOUT: return -110;
    case ECONNREFUSED: return -111;
    case ELOOP: return -40;
    case ENAMETOOLONG: return -36;
    case EHOSTDOWN: return -112;
    case EHOSTUNREACH: return -113;
    case ENOTEMPTY: return -39;
    case EPROCLIM: return -11; /* "too many processes": EAGAIN */
    case EUSERS: return -87;
    case EDQUOT: return -122;
    case ESTALE: return -116;
    case EREMOTE: return -66;
    case ENOLCK: return -37;
    case ENOSYS: return -38;
    case EOVERFLOW: return -75;
    case EBADEXEC:
    case EBADARCH:
    case ESHLIBVERS:
    case EBADMACHO: return -8; /* ENOEXEC */
    case ECANCELED: return -125;
    case EIDRM: return -43;
    case ENOMSG: return -42;
    case EILSEQ: return -84;
    case ENOATTR: return -61; /* ENODATA */
    case EBADMSG: return -74;
    case EMULTIHOP: return -72;
    case ENODATA: return -61;
    case ENOLINK: return -67;
    case ENOSR: return -63;
    case ENOSTR: return -60;
    case EPROTO: return -71;
    case ETIME: return -62;
    case ENOPOLICY: return -95;
    case ENOTRECOVERABLE: return -131;
    case EOWNERDEAD: return -130;
    default: return -5; /* EIO */
    }
}

static int mac_err(void) { return mac_errno(errno); }

/* ---------------------------------------------------------------- files */
static int mac_open_flags(int flags) {
    int f;
    switch (flags & 3) {
    case OS_O_WRONLY: f = O_WRONLY; break;
    case OS_O_RDWR: f = O_RDWR; break;
    default: f = O_RDONLY; break;
    }
    if (flags & OS_O_CREAT) f |= O_CREAT;
    if (flags & OS_O_TRUNC) f |= O_TRUNC;
    if (flags & OS_O_APPEND) f |= O_APPEND;
    if (flags & OS_O_NONBLOCK) f |= O_NONBLOCK;
    if (flags & OS_O_DIRECTORY) f |= O_DIRECTORY;
    if (flags & OS_O_CLOEXEC) f |= O_CLOEXEC;
    return f;
}

int os_open(const char *path, int flags, int mode) {
    int fd = openat(AT_FDCWD, path, mac_open_flags(flags), (mode_t)mode);
    return fd < 0 ? mac_err() : fd;
}

int os_read(int fd, void *buf, size_t n) {
    ssize_t r = read(fd, buf, n);
    return r < 0 ? mac_err() : (int)r;
}

int os_write(int fd, const void *buf, size_t n) {
    ssize_t r = write(fd, buf, n);
    return r < 0 ? mac_err() : (int)r;
}

int os_close(int fd) {
    if (mac_dir_drop(fd)) return 0; /* fdopendir owns the fd; closedir closed it */
    return close(fd) == 0 ? 0 : mac_err();
}

i64 os_lseek(int fd, i64 off, int whence) {
    off_t r = lseek(fd, (off_t)off, whence);
    return r < 0 ? mac_err() : (i64)r;
}

int os_ftruncate(int fd, i64 len) {
    return ftruncate(fd, (off_t)len) == 0 ? 0 : mac_err();
}

static void stat_to_os(struct os_stat *o, const struct stat *st) {
    agentc_memset(o, 0, sizeof *o);
    o->st_dev = (u64)st->st_dev;
    o->st_ino = (u64)st->st_ino;
    o->st_nlink = (u64)st->st_nlink;
    o->st_mode = (u32)st->st_mode;
    o->st_uid = (u32)st->st_uid;
    o->st_gid = (u32)st->st_gid;
    o->st_rdev = (u64)st->st_rdev;
    o->st_size = (i64)st->st_size;
    o->st_blksize = (i64)st->st_blksize;
    o->st_blocks = (i64)st->st_blocks;
    o->st_atime = (i64)st->st_atimespec.tv_sec;
    o->st_atime_nsec = (i64)st->st_atimespec.tv_nsec;
    o->st_mtime = (i64)st->st_mtimespec.tv_sec;
    o->st_mtime_nsec = (i64)st->st_mtimespec.tv_nsec;
    o->st_ctime = (i64)st->st_ctimespec.tv_sec;
    o->st_ctime_nsec = (i64)st->st_ctimespec.tv_nsec;
}

int os_fstat(int fd, void *statbuf) {
    struct stat st;
    if (fstat(fd, &st) != 0) return mac_err();
    stat_to_os(statbuf, &st);
    return 0;
}

int os_stat(const char *path, void *statbuf) {
    struct stat st;
    if (fstatat(AT_FDCWD, path, &st, 0) != 0) return mac_err();
    stat_to_os(statbuf, &st);
    return 0;
}

int os_mkdir(const char *path, int mode) {
    return mkdirat(AT_FDCWD, path, (mode_t)mode) == 0 ? 0 : mac_err();
}

int os_unlink(const char *path) {
    return unlinkat(AT_FDCWD, path, 0) == 0 ? 0 : mac_err();
}

int os_rename(const char *from, const char *to) {
    return renameat(AT_FDCWD, from, AT_FDCWD, to) == 0 ? 0 : mac_err();
}

int os_getcwd(char *buf, size_t len) {
    if (getcwd(buf, len) == NULL) return mac_err();
    return (int)agentc_strlen(buf) + 1; /* linux getcwd returns the length incl. NUL */
}

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

/* ------------------------------------------------------- directory scanning */
#define MAC_DIR_SLOTS 32
#define MAC_NAME_MAX 256

struct mac_dir {
    int fd;            /* -1 when free */
    DIR *d;
    bool pending;
    u64 p_ino;
    i64 p_off;
    u8 p_type;
    u16 p_namlen;
    char p_name[MAC_NAME_MAX];
};

static struct mac_dir g_dirs[MAC_DIR_SLOTS];

/* The table lives in BSS, so every fd would start as 0, and fd 0 is also a
 * legal descriptor: the "free" marker has to be written explicitly before the
 * first lookup. */
static void dir_table_init(void) {
    static bool done;
    if (done) return;
    for (int i = 0; i < MAC_DIR_SLOTS; i++) g_dirs[i].fd = -1;
    done = true;
}

static struct mac_dir *dir_state(int fd, bool create) {
    if (fd < 0) return NULL;
    dir_table_init();
    struct mac_dir *free_slot = NULL;
    for (int i = 0; i < MAC_DIR_SLOTS; i++) {
        if (g_dirs[i].fd == fd) return &g_dirs[i];
        if (free_slot == NULL && g_dirs[i].fd < 0) free_slot = &g_dirs[i];
    }
    if (!create) return NULL;
    if (free_slot == NULL) {
        errno = EMFILE; /* every table slot owns a stream */
        return NULL;
    }
    DIR *d = fdopendir(fd);
    if (d == NULL) return NULL;
    free_slot->fd = fd;
    free_slot->d = d;
    free_slot->pending = false;
    return free_slot;
}

/* os_close helper: close a directory stream if one owns this fd. */
bool mac_dir_drop(int fd) {
    struct mac_dir *s = dir_state(fd, false);
    if (s == NULL) return false;
    closedir(s->d);
    s->fd = -1;
    s->d = NULL;
    s->pending = false;
    return true;
}

struct mac_linux_dirent64 {
    u64 d_ino;
    i64 d_off;
    u16 d_reclen;
    u8 d_type;
    char d_name[];
};

static size_t linux_dirent_len(size_t namlen) {
    return (19 + namlen + 1 + 7) & ~(size_t)7;
}

static void dirent_emit(struct mac_linux_dirent64 *o, u64 ino, i64 off, u8 type,
                        const char *name, size_t namlen) {
    size_t reclen = linux_dirent_len(namlen);
    o->d_ino = ino;
    o->d_off = off;
    o->d_reclen = (u16)reclen;
    o->d_type = type;
    agentc_memcpy(o->d_name, name, namlen);
    o->d_name[namlen] = '\0';
}

int os_getdents(int fd, void *buf, size_t len) {
    struct mac_dir *s = dir_state(fd, true);
    if (s == NULL) return mac_err();
    size_t used = 0;
    for (;;) {
        u64 ino;
        i64 off;
        u8 type;
        const char *name;
        size_t namlen;
        if (s->pending) {
            if (used + linux_dirent_len(s->p_namlen) > len) break;
            dirent_emit((struct mac_linux_dirent64 *)((u8 *)buf + used), s->p_ino,
                        s->p_off, s->p_type, s->p_name, s->p_namlen);
            used += linux_dirent_len(s->p_namlen);
            s->pending = false;
        }
        struct dirent *e = readdir(s->d);
        if (e == NULL) break;
        size_t nl = (size_t)e->d_namlen;
        if (nl >= MAC_NAME_MAX) nl = MAC_NAME_MAX - 1;
        if (used + linux_dirent_len(nl) > len) {
            s->pending = true;
            s->p_ino = (u64)e->d_ino;
            s->p_off = (i64)e->d_seekoff;
            s->p_type = (u8)e->d_type;
            s->p_namlen = (u16)nl;
            agentc_memcpy(s->p_name, e->d_name, nl);
            s->p_name[nl] = '\0';
            break;
        }
        ino = (u64)e->d_ino;
        off = (i64)e->d_seekoff;
        type = (u8)e->d_type;
        name = e->d_name;
        namlen = nl;
        dirent_emit((struct mac_linux_dirent64 *)((u8 *)buf + used), ino, off, type,
                    name, namlen);
        used += linux_dirent_len(namlen);
    }
    if (used == 0 && s->pending) return -22; /* buffer too small for one record */
    return (int)used;
}

/* ---------------------------------------------------------------- memory */
u8 *os_map_try(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}

u8 *os_map(size_t n) {
    u8 *p = os_map_try(n);
    if (p == NULL) agentc_die("out of memory");
    return p;
}

int os_unmap(u8 *p, size_t n) { return munmap(p, n) == 0 ? 0 : mac_err(); }

/* ------------------------------------------------------------- processes */
static int mac_spawn_common(char *const argv[], char *const envp[], const char *cwd,
                            int fd_in, int fd_out, int fd_err, int ctty, bool group) {
    (void)ctty; /* the TUI does not need a controlling tty yet */
    pid_t pid = fork();
    if (pid < 0) return mac_err();
    if (pid == 0) {
        /* New process group before exec, so os_kill(-pid) sees the whole tree.
         * A failure means the child is already a leader; ignore it. */
        if (group) (void)setpgid(0, 0);
        if (fd_in >= 0) dup2(fd_in, 0);
        if (fd_out >= 0) dup2(fd_out, 1);
        if (fd_err >= 0) dup2(fd_err, 2);
        if (cwd != NULL) chdir(cwd);
        execve(argv[0], argv, envp);
        _exit(127);
    }
    return (int)pid;
}

int os_spawn(char *const argv[], char *const envp[], const char *cwd, int fd_in,
             int fd_out, int fd_err, int ctty) {
    return mac_spawn_common(argv, envp, cwd, fd_in, fd_out, fd_err, ctty, false);
}

int os_spawn_group(char *const argv[], char *const envp[], const char *cwd, int fd_in,
                   int fd_out, int fd_err, int ctty) {
    return mac_spawn_common(argv, envp, cwd, fd_in, fd_out, fd_err, ctty, true);
}

int os_wait(int pid, bool nohang) {
    int status = 0;
    pid_t r = wait4(pid, &status, nohang ? WNOHANG : 0, NULL);
    if (r > 0) return status;
    if (r == 0) return -1; /* still running */
    return -2;             /* unknown pid */
}

int os_kill(int pid, int sig) {
    if (kill(pid, sig) == 0) return 0;
    /* Negative pid: kill the process group; fall back to the leader pid when
     * the group no longer exists so a non-group child still dies. */
    if (pid < 0) return kill(-pid, sig) == 0 ? 0 : mac_err();
    return mac_err();
}

int os_pipe(int fds[2]) {
    int f[2];
    if (pipe(f) != 0) return mac_err();
    int fl = fcntl(f[0], F_GETFL, 0);
    if (fl >= 0) fcntl(f[0], F_SETFL, fl | O_NONBLOCK);
    fcntl(f[0], F_SETFD, FD_CLOEXEC);
    fcntl(f[1], F_SETFD, FD_CLOEXEC);
    fds[0] = f[0];
    fds[1] = f[1];
    return 0;
}

/* ------------------------------------------------------------- shell tool */
/* Same contract as the Linux adapter: /bin/sh is the only shell, and a
 * Windows-only kind fails with EINVAL instead of silently falling back. */
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

static int mac_spawn_shell_common(const char *kind, const char *command, int fd_in,
                                  int fd_out, int fd_err, bool group) {
    if (os_shell_kind(kind) == NULL || command == NULL) return -22;
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
    return mac_spawn_shell_common(kind, command, fd_in, fd_out, fd_err, false);
}

int os_spawn_shell_group(const char *kind, const char *command, int fd_in, int fd_out,
                         int fd_err) {
    return mac_spawn_shell_common(kind, command, fd_in, fd_out, fd_err, true);
}

/* ---------------------------------------------------------------- terminal */
static void (*g_winch_handler)(void);

static void mac_winch_trampoline(int sig) {
    (void)sig;
    if (g_winch_handler != NULL) g_winch_handler();
}

int os_tty_raw(int fd, void **saved) {
    struct termios *st = agentc_alloc(128);
    if (tcgetattr(fd, st) != 0) {
        agentc_free(st);
        return mac_err();
    }
    struct termios t = *st;
    t.c_iflag &= (tcflag_t) ~(IXON | ICRNL | BRKINT | PARMRK | ISTRIP);
    t.c_oflag &= (tcflag_t) ~OPOST;
    t.c_lflag &= (tcflag_t) ~(ISIG | ICANON | ECHO | IEXTEN);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        agentc_free(st);
        return mac_err();
    }
    *saved = st;
    return 0;
}

int os_tty_restore(int fd, void *saved) {
    if (saved == NULL) return -22; /* EINVAL */
    int r = tcsetattr(fd, TCSANOW, saved) == 0 ? 0 : mac_err();
    agentc_free(saved);
    return r;
}

int os_tty_size(int fd, int *cols, int *rows) {
    struct winsize ws;
    agentc_memset(&ws, 0, sizeof ws);
    if (ioctl(fd, TIOCGWINSZ, &ws) != 0) return mac_err();
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* Darwin sigaction via libSystem (the SDK struct is available: mac_internal.h
 * includes <signal.h>). */
int os_sig_install(int sig, void (*handler)(int)) {
    struct sigaction sa;
    agentc_memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    return sigaction(sig, &sa, NULL) == 0 ? 0 : mac_err();
}

int os_sig_winch(void (*handler)(void)) {
    if (signal(SIGWINCH, mac_winch_trampoline) == SIG_ERR) return mac_err();
    g_winch_handler = handler;
    return 0;
}

int os_open_url(const char *url) {
    char *argv[3] = { (char *)"/usr/bin/open", (char *)url, NULL };
    int pid = os_spawn(argv, mac_environ(), NULL, -1, -1, -1, -1);
    return pid < 0 ? pid : 0; /* fire and forget */
}

/* ----------------------------------------------------- time, entropy, poll */
i64 os_now_ns(int clock) {
    struct timespec ts;
    clockid_t c = (clock == OS_CLOCK_MONOTONIC) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    if (clock_gettime(c, &ts) != 0) return mac_err();
    return (i64)ts.tv_sec * 1000000000 + (i64)ts.tv_nsec;
}

void os_sleep_ns(i64 ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

int os_random(void *buf, size_t n) {
    arc4random_buf(buf, n);
    return 0;
}

int os_poll(struct os_pollfd *fds, int nfds, int timeout_ms) {
    int r = poll((struct pollfd *)fds, (nfds_t)nfds, timeout_ms);
    return r < 0 ? mac_err() : r;
}
