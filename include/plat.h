/* plat.h — Layer 0: the portable kernel surface.
 *
 * Exactly one implementation is linked:
 *   src/plat/linux/    raw linux syscalls
 *   src/plat/mac/      libSystem / Mach (later)
 *   src/plat/win/      Win32 (later)
 *
 * Rules: all calls return negative errno on failure (linux numbers, translated
 * by the adapter), two-value returns use out-params, buffers are caller-owned.
 */
#ifndef AGENTC_PLAT_H
#define AGENTC_PLAT_H

#include "agentc.h"

/* bootstrap */
void os_init(void *initial_stack);
/* "linux" | "macos" | "windows": reported to the model in the system prompt */
const char *os_platform(void);
void os_exit(int code) __attribute__((noreturn));
const char *os_getenv(const char *name); /* NULL if unset */

/* files */
int os_open(const char *path, int flags, int mode); /* -> fd | -errno */
int os_read(int fd, void *buf, size_t n);
int os_write(int fd, const void *buf, size_t n);
int os_close(int fd);
i64 os_lseek(int fd, i64 off, int whence);
int os_ftruncate(int fd, i64 len);   /* -> 0 | -errno */
int os_fstat(int fd, void *statbuf); /* fills struct os_stat */
int os_stat(const char *path, void *statbuf);
int os_mkdir(const char *path, int mode);
int os_unlink(const char *path);
int os_rename(const char *from, const char *to);
/* Permission bits (POSIX mode) / the real uid; the transport uses these to
 * chmod a unix socket 0600. Windows maps chmod to the read-only attribute and
 * reports uid 0. */
int os_chmod(const char *path, int mode);
u32 os_getuid(void);
int os_getpid(void);
int os_getcwd(char *buf, size_t len);
int os_getdents(int fd, void *buf, size_t len); /* linux dirent64 records */

/* memory */
u8 *os_map(size_t n);     /* zeroed pages, dies on failure */
u8 *os_map_try(size_t n); /* NULL on failure */
int os_unmap(u8 *p, size_t n);

/* processes */
int os_spawn(char *const argv[], char *const envp[], const char *cwd, int fd_in, int fd_out,
             int fd_err, int ctty); /* -> pid | -errno */
int os_wait(int pid, bool nohang);  /* 128+sig | exit<<8 | -1 | -2 */
/* os_kill(pid, sig): a negative pid targets the process group -pid (POSIX
 * kill(-pid) / Windows job object) and falls back to the leader process when
 * the group no longer exists; a positive pid is the plain single-process kill. */
int os_kill(int pid, int sig);
int os_pipe(int fds[2]); /* read end O_NONBLOCK|O_CLOEXEC */
/* Set or clear O_NONBLOCK on an open descriptor (POSIX: fcntl F_SETFL; the flag
 * lives on the open file description and is shared with dup/fork, which the
 * pipe transport rule relies on). -> 0 | -errno.
 * Windows: a pipe READ end is always effectively non-blocking (os_read peeks
 * and returns -EAGAIN when empty) and this succeeds as a no-op; a pipe WRITE
 * end cannot be made non-blocking portably - PIPE_NOWAIT is deprecated and
 * would misreport a full pipe as ERROR_NO_DATA, which os_write maps to -EPIPE
 * (a healthy child would be diagnosed as dead) - so it returns -ENOSYS there
 * and the caller keeps a blocking write end as the documented fail-safe.
 * Sockets are Layer 1's business; os_set_nonblock on a socket fd returns
 * -ENOSYS on Windows and is unspecified on POSIX. */
int os_set_nonblock(int fd, bool on);

/* PATH lookup (no shell). Resolve `name` to an absolute path: on POSIX a hit
 * must be a regular file with an execute bit; on Windows the PATHEXT suffixes
 * are tried when `name` has no extension. A `name` containing '/' (or '\\' on
 * Windows) is checked as given, relative names against the cwd, and is never
 * searched on PATH. Writes the absolute path NUL-terminated into out[cap].
 * os_which_in searches the explicit `path_env` value (NULL/"" = no PATH);
 * os_which uses $PATH. -> 0 | -ENOENT (not found) | -ERANGE (out too small). */
int os_which_in(const char *path_env, const char *name, char *out, size_t cap);
int os_which(const char *name, char *out, size_t cap);

/* Shell tool support (the bash tool). `kind` is the user's selection from the
 * `shell` config key: "auto"/NULL is the platform default, plus "sh" on POSIX
 * and "cmd", "powershell", "pwsh" on Windows. os_shell_kind() maps a kind to
 * its canonical name for display (NULL for a kind this platform does not have),
 * os_shell_resolve() additionally resolves the executable (0 | -ENOENT absent |
 * -EINVAL unknown), and os_spawn_shell() runs `command` through it as a single
 * script argument with inherited cwd/env. All return negative Linux errno. */
const char *os_shell_kind(const char *kind);
int os_shell_resolve(const char *kind, char *out, size_t cap);
int os_spawn_shell(const char *kind, const char *command, int fd_in, int fd_out, int fd_err);

/* Like os_spawn/os_spawn_shell but the child becomes a new process group
 * leader, so os_kill(-pid, sig) reaches every descendant.  On Windows the
 * child is put in a job object with KILL_ON_JOB_CLOSE instead. */
int os_spawn_group(char *const argv[], char *const envp[], const char *cwd, int fd_in, int fd_out,
                   int fd_err, int ctty);
int os_spawn_shell_group(const char *kind, const char *command, int fd_in, int fd_out, int fd_err);

/* terminal */
int os_tty_raw(int fd, void **saved);
int os_tty_restore(int fd, void *saved);
int os_tty_size(int fd, int *cols, int *rows);
/* 1 when fd is a terminal/console on its input side, 0 otherwise. Callers need
 * this to know whether the byte contract can change: a console's cooked line
 * discipline translates CR to NL, while a pipe/file delivers its bytes as-is.
 * os_tty_raw/size are not enough - they may succeed or fall back to stdout
 * when fd itself is redirected. */
int os_tty_isatty(int fd);
int os_sig_winch(void (*handler)(void));

/* open a URL in the user's browser (xdg-open/open/ShellExecuteW); used by the
 * OAuth login flow. Returns 0 or -errno. */
int os_open_url(const char *url);

/* signals / console control events: install a handler (NULL restores the
 * default). The handler must be async-signal-safe; it is used to restore the
 * terminal before the process dies. */
#define OS_SIGHUP 1
#define OS_SIGINT 2
#define OS_SIGQUIT 3
#define OS_SIGABRT 6
#define OS_SIGBUS 7
#define OS_SIGFPE 8
#define OS_SIGSEGV 11
#define OS_SIGTERM 15
int os_sig_install(int sig, void (*handler)(int));

/* time, entropy, events */
i64 os_now_ns(int clock); /* 0=realtime 1=monotonic */
void os_sleep_ns(i64 ns);
int os_random(void *buf, size_t n);
struct os_pollfd {
    int fd;
    short events;
    short revents;
};
int os_poll(struct os_pollfd *fds, int nfds, int timeout_ms);

/* constants (linux values; adapters translate) */
#define OS_POLLIN 1
#define OS_POLLOUT 4
#define OS_POLLERR 8
#define OS_POLLHUP 16

#define OS_O_RDONLY 0
#define OS_O_WRONLY 1
#define OS_O_RDWR 2
#define OS_O_CREAT 0x40
#define OS_O_TRUNC 0x200
#define OS_O_APPEND 0x400
#define OS_O_NONBLOCK 0x800
#define OS_O_DIRECTORY 0x10000
#define OS_O_CLOEXEC 0x80000
/* Appended B-L10 (P8-D22): fail the open if the path already exists. Used for
 * the atomic-writer temp file so a pre-planted name or symlink cannot be
 * hijacked. Linux passes the value straight to openat; mac/win map it below. */
#define OS_O_EXCL 0x80

#define OS_CLOCK_REALTIME 0
#define OS_CLOCK_MONOTONIC 1
/* 2 = CPU time consumed by this process. Used where a wall-clock deadline would
 * mistake host preemption for the caller's own work (status render budget). */
#define OS_CLOCK_CPUTIME 2
/* 3 = CPU time consumed by the calling thread. Finer-grained than the process
 * clock on some hosts and unaffected by any other thread; the status render
 * budget measures with it. */
#define OS_CLOCK_THREAD_CPUTIME 3

/* os_stat/os_fstat fill this through their void *statbuf argument. The field
 * order is logical, not any platform's native layout: each backend converts
 * from its own struct stat, so the core never depends on byte offsets (the
 * linux x86-64 kernel writes the native layout into a local and converts too). */
struct os_stat {
    u64 st_dev;
    u64 st_ino;
    u64 st_nlink;
    u32 st_mode;
    u32 st_uid;
    u32 st_gid;
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
};

/* Dynamic extension loader. `path` is an absolute library path; a
 * successful open returns the opaque handle through *handle. `err` (when
 * non-NULL and errcap > 0) receives a NUL-terminated diagnostic on failure;
 * the backends leave it untouched on success, so callers read it only when the
 * return value is negative. Symbol names are ASCII. All calls return 0 or a
 * negative linux errno; os_ext_close(NULL) is a no-op and close is idempotent
 * at this layer (the registry guarantees exactly-once). os_ext_available() is false and
 * os_ext_suffix() NULL on a platform without a loader; the Linux static build
 * returns -ENOSYS with a clear message. */
int os_ext_open(const char *path, void **handle, char *err, size_t errcap);
int os_ext_sym(void *handle, const char *name, void **out, char *err, size_t errcap);
int os_ext_close(void *handle);
bool os_ext_available(void);
const char *os_ext_suffix(void);

/* --------------------------------------------------------- PTY (P6-A) */
/* os_pty_open allocates a master/slave pair and optionally sets the initial
 * window size; the caller spawns with fd_in=fd_out=fd_err=slave and closes the
 * slave in the parent. On Windows a real master/slave pair does not exist, so
 * both return -ENOSYS there (a `tty:true` request must error, never silently
 * fall back to a pipe). -> 0 | -errno. */
int os_pty_open(int *master, int *slave, int cols, int rows);
int os_pty_resize(int fd, int cols, int rows);
int os_pty_size(int fd, int *cols, int *rows);

/* -------------------------------------------------- freeze/resume (P6-A) */
/* POSIX: SIGSTOP/SIGCONT on the group (SIGSTOP if `group` is false). Windows
 * has no portable process suspend here, so both return -ENOSYS; the core keeps
 * the actor's frozen state and skips timers regardless. */
#define OS_SIGSTOP 19
#define OS_SIGCONT 18
int os_process_freeze(int pid, bool group);
int os_process_resume(int pid, bool group);
/* Optional checkpoint backend (Linux CRIU); -ENOSYS where unavailable. */
int os_process_checkpoint(int pid, const char *dir, char *err, size_t errcap);

/* ------------------------------------------------------- sandbox (P6-C) */
/* Flat form of the effective envelope handed to Layer 0. The arrays are
 * borrowed from the AgcTrustEnv that produced the spec and stay valid for the
 * duration of the spawn. `access`/`boundary`/`network` are the AgcAccess /
 * AgcBoundary / AgcNetwork values; `exclude_tmp` drops the implicit $TMPDIR
 * write allowance for boundary=workspace. */
typedef struct AgcSandboxSpec {
    int access;
    int boundary;
    int network;
    const char **writable_roots;
    size_t nroots;
    const char **allowed_hosts;
    size_t nhosts;
    bool exclude_tmp;
} AgcSandboxSpec;

/* Apply the effective sandbox to the calling thread/process. -> 0 | -errno.
 * Fail-closed: any negative return must be treated as a denial. `err`
 * (optional) receives a short actionable diagnostic. `boundary=none` returns 0
 * immediately (the only unsandboxed mode). */
int os_sandbox_apply(const AgcSandboxSpec *spec, char *err, size_t errcap);
/* Pre-flight: 0 = a backend can honor this spec | -ENOSYS (none) | -EINVAL. */
int os_sandbox_available(const AgcSandboxSpec *spec);

/* Appended spawn variants: identical to os_spawn_group/os_spawn_shell_group but
 * the child calls os_sandbox_apply between fork and execve over a CLOEXEC
 * status pipe; the parent waits for the one-byte status and returns the child's
 * negative errno on failure (never a silent unsandboxed run). sandbox == NULL is
 * byte-identical to the existing call. */
int os_spawn_group_sandboxed(char *const argv[], char *const envp[], const char *cwd, int fd_in,
                             int fd_out, int fd_err, int ctty, const AgcSandboxSpec *sandbox,
                             char *err, size_t errcap);
int os_spawn_shell_group_sandboxed(const char *kind, const char *command, int fd_in, int fd_out,
                                   int fd_err, const AgcSandboxSpec *sandbox, char *err,
                                   size_t errcap);

/* Canonicalize an existing path (resolve `.`/`..` and symlinks). -> 0 | -errno.
 * Required by Layer-4 root checks so `..` and symlinks cannot escape. */
int os_realpath(const char *path, char *out, size_t cap);

/* Test seam (P6-C): override the sandbox backend decision so a golden can
 * assert fail-closed refusal deterministically. 0=auto (real), 1=force
 * -ENOSYS, 2=record-and-allow (mock). The mock records a bounded description
 * of the last applied spec, readable with os_sandbox_last_summary(). */
void os_sandbox_set_test_backend(int kind);
const char *os_sandbox_last_summary(void);

/* Embedding seam (appended for the SDK, Axis B): a host that embeds libagentc
 * already owns the process and its argv/envp, so it publishes them here instead
 * of relying on the platform entry stub. The arrays are borrowed and must stay
 * valid for the engine's lifetime. Calling os_embed_init() after os_init() is
 * allowed (the host wins); it installs no signal handlers, stdio ownership or
 * terminal state. */
void os_embed_init(char **argv, char **envp);

/* Remove one empty directory. `os_unlink` removes files and symlinks;
 * `os_rmdir` removes a directory (after its contents are gone), so a recursive
 * tree removal never relies on one primitive doing both. -> 0 | -errno. */
int os_rmdir(const char *path);

/* Like os_stat but never follows a final symlink (POSIX lstat). A symlink is
 * reported with S_IFLNK (0120000) in st_mode and the link's own metadata;
 * Windows reports a reparse point the same way. Appended for the serve socket
 * directory check (S6): a pre-created symlink must be distinguishable from a
 * real directory the caller owns. -> 0 | -errno. */
int os_lstat(const char *path, void *statbuf);

#endif /* AGENTC_PLAT_H */
