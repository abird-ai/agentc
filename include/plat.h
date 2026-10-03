/* plat.h — Layer 0: the portable kernel surface.
 *
 * Exactly one implementation is linked:
 *   src/plat/linux/    raw linux syscalls
 *   src/plat/mac/      libSystem / Mach (later)
 *   src/plat/win/      Win32 (later)
 *   src/plat/headless.c  deterministic backend for tests (later)
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
const char *os_getenv(const char *name);        /* NULL if unset */

/* files */
int os_open(const char *path, int flags, int mode);   /* -> fd | -errno */
int os_read(int fd, void *buf, size_t n);
int os_write(int fd, const void *buf, size_t n);
int os_close(int fd);
i64 os_lseek(int fd, i64 off, int whence);
int os_ftruncate(int fd, i64 len);                        /* -> 0 | -errno */
int os_fstat(int fd, void *statbuf);                   /* fills struct os_stat */
int os_stat(const char *path, void *statbuf);
int os_mkdir(const char *path, int mode);
int os_unlink(const char *path);
int os_rename(const char *from, const char *to);
int os_getcwd(char *buf, size_t len);
int os_getdents(int fd, void *buf, size_t len);        /* linux dirent64 records */

/* memory */
u8 *os_map(size_t n);                                  /* zeroed pages, dies on failure */
u8 *os_map_try(size_t n);                              /* NULL on failure */
int os_unmap(u8 *p, size_t n);

/* processes */
int os_spawn(char *const argv[], char *const envp[], const char *cwd,
             int fd_in, int fd_out, int fd_err, int ctty);   /* -> pid | -errno */
int os_wait(int pid, bool nohang);                     /* 128+sig | exit<<8 | -1 | -2 */
/* os_kill(pid, sig): a negative pid targets the process group -pid (POSIX
 * kill(-pid) / Windows job object) and falls back to the leader process when
 * the group no longer exists; a positive pid is the plain single-process kill. */
int os_kill(int pid, int sig);
int os_pipe(int fds[2]);                               /* read end O_NONBLOCK|O_CLOEXEC */

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
int os_spawn_shell(const char *kind, const char *command, int fd_in, int fd_out,
                   int fd_err);

/* Like os_spawn/os_spawn_shell but the child becomes a new process group
 * leader, so os_kill(-pid, sig) reaches every descendant.  On Windows the
 * child is put in a job object with KILL_ON_JOB_CLOSE instead. */
int os_spawn_group(char *const argv[], char *const envp[], const char *cwd,
                   int fd_in, int fd_out, int fd_err, int ctty);
int os_spawn_shell_group(const char *kind, const char *command, int fd_in, int fd_out,
                         int fd_err);

/* terminal */
int os_tty_raw(int fd, void **saved);
int os_tty_restore(int fd, void *saved);
int os_tty_size(int fd, int *cols, int *rows);
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
i64 os_now_ns(int clock);                              /* 0=realtime 1=monotonic */
void os_sleep_ns(i64 ns);
int os_random(void *buf, size_t n);
struct os_pollfd { int fd; short events; short revents; };
int os_poll(struct os_pollfd *fds, int nfds, int timeout_ms);

/* constants (linux values; adapters translate) */
#define OS_POLLIN  1
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

#define OS_CLOCK_REALTIME  0
#define OS_CLOCK_MONOTONIC 1

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

#endif /* AGENTC_PLAT_H */
