# 10 — Platform layer: Layer 0 and Layer 1 contracts

`agentc` takes its design inspiration from pi
(https://github.com/earendil-works/pi): a minimal coding agent whose UX and
extensibility the project targets, realized here as one freestanding static
binary. The platform layer is the boundary that keeps the rest of the tree
portable across Linux, macOS and Windows.

Everything below is a **link-time contract**: the core is compiled against
`include/plat.h` (Layer 0) and `include/net.h` (Layer 1) and never knows which
implementation is linked. The build selects exactly one `plat/` tree and one
`net/` tree over the portable sources (`.agents/design/50-build-and-test.md`).
Tests substitute `src/plat/headless.c` and `src/net/mock.c`. Runtime dispatch
exists only inside the TLS handshake state machine and the mock/headless
backends.

Related documents: `.agents/design/00-architecture.md`,
`.agents/design/20-core-agent.md`, `.agents/design/30-extensibility.md`,
`.agents/design/40-tui.md`, `.agents/design/50-build-and-test.md`,
`.agents/plans/P5-TOOL-ENGINE.md`, `.agents/plans/P5-AXES.md`.

---

## 1. Layer 0 — kernel surface (`include/plat.h`)

All functions return `int` (`i64` for offsets and times) and report failure as a
**negative Linux errno**. Each adapter translates its native errors into that
shared set (Darwin and Winsock both have explicit tables). Two-value returns use
out-parameters. Buffers are caller-owned. There are no hidden allocations. The
header is the single source of truth; the groups below mirror it.

### Bootstrap

```c
void        os_init(void *initial_stack);   /* _start hands us the stack */
const char *os_platform(void);              /* "linux" | "macos" | "windows" */
void        os_exit(int code) __attribute__((noreturn));
const char *os_getenv(const char *name);    /* NULL if unset */
```

### Files

```c
int  os_open(const char *path, int flags, int mode);  /* -> fd | -errno */
int  os_read(int fd, void *buf, size_t n);
int  os_write(int fd, const void *buf, size_t n);
int  os_close(int fd);
i64  os_lseek(int fd, i64 off, int whence);
int  os_ftruncate(int fd, i64 len);                   /* -> 0 | -errno */
int  os_fstat(int fd, void *statbuf);                 /* fills struct os_stat */
int  os_stat(const char *path, void *statbuf);
int  os_mkdir(const char *path, int mode);
int  os_unlink(const char *path);
int  os_rename(const char *from, const char *to);
int  os_getcwd(char *buf, size_t len);
int  os_getdents(int fd, void *buf, size_t len);      /* Linux dirent64 records */
```

### Memory

```c
u8 *os_map(size_t n);        /* zeroed pages, dies on failure */
u8 *os_map_try(size_t n);    /* NULL on failure */
int os_unmap(u8 *p, size_t n);
```

### Processes

```c
int os_spawn(char *const argv[], char *const envp[], const char *cwd,
             int fd_in, int fd_out, int fd_err, int ctty);   /* -> pid | -errno */
int os_wait(int pid, bool nohang);                     /* 128+sig | exit<<8 | -1 | -2 */
int os_kill(int pid, int sig);
int os_pipe(int fds[2]);                               /* read end O_NONBLOCK|O_CLOEXEC */
```

`os_kill` with a negative `pid` targets the process group `-pid` (POSIX
`kill(-pid)`, a Windows job object) and falls back to the leader process when
the group no longer exists; a positive `pid` is a single-process kill.
`os_spawn_group` and `os_spawn_shell_group` make the child a new process-group
leader so `os_kill(-pid, sig)` reaches descendants; on Windows the child is put
in a job object with `KILL_ON_JOB_CLOSE`.

PATH lookup runs without a shell:

```c
int os_which_in(const char *path_env, const char *name, char *out, size_t cap);
int os_which(const char *name, char *out, size_t cap);
```

`os_which_in` searches the explicit `path_env` value (`NULL`/`""` means no
PATH); `os_which` uses `$PATH`. On POSIX a hit must be a regular file with an
execute bit; on Windows `PATHEXT` suffixes are tried when `name` has no
extension. A `name` containing `/` (or `\` on Windows) is checked as given,
relative names against the cwd, and is never searched on PATH. The absolute
path is written NUL-terminated into `out[cap]`; the result is `0`, `-ENOENT`
(not found) or `-ERANGE` (out too small). The tool engine selects between
external and built-in search through this surface
(`.agents/plans/P5-TOOL-ENGINE.md`).

### Shell tool support

```c
const char *os_shell_kind(const char *kind);            /* sh | cmd | powershell | pwsh */
int  os_shell_resolve(const char *kind, char *out, size_t cap);   /* 0 | -ENOENT | -EINVAL */
int  os_spawn_shell(const char *kind, const char *command, int fd_in, int fd_out,
                    int fd_err);
int  os_spawn_shell_group(const char *kind, const char *command, int fd_in, int fd_out,
                          int fd_err);
```

`kind` is the user's `shell` selection: `auto`/`NULL` is the platform default,
plus `sh` on POSIX and `cmd`, `powershell`, `pwsh` on Windows. `os_shell_kind`
maps a kind to its canonical name for display (`NULL` for a kind the platform
does not have), `os_shell_resolve` additionally resolves the executable
(`-ENOENT` absent, `-EINVAL` unknown), and `os_spawn_shell` runs `command`
through it as a single script argument with the inherited cwd and environment.

### Terminal and signals

```c
int  os_tty_raw(int fd, void **saved);                 /* termios / console mode */
int  os_tty_restore(int fd, void *saved);
int  os_tty_size(int fd, int *cols, int *rows);
int  os_sig_winch(void (*handler)(void));              /* SIGWINCH / console resize */
int  os_open_url(const char *url);                     /* browser, for OAuth login */

#define OS_SIGHUP 1
#define OS_SIGINT 2
#define OS_SIGQUIT 3
#define OS_SIGABRT 6
#define OS_SIGBUS 7
#define OS_SIGFPE 8
#define OS_SIGSEGV 11
#define OS_SIGTERM 15
int  os_sig_install(int sig, void (*handler)(int));    /* NULL restores default */
```

`os_sig_install` handlers must be async-signal-safe; the core uses them to
restore the terminal before the process dies. `os_open_url` opens the user's
browser (`xdg-open`/`open`/`ShellExecuteW`) for the OAuth login flow.

### Time, entropy, events

```c
i64  os_now_ns(int clock);                             /* 0=realtime 1=monotonic */
void os_sleep_ns(i64 ns);
int  os_random(void *buf, size_t n);
struct os_pollfd { int fd; short events; short revents; };
int  os_poll(struct os_pollfd *fds, int nfds, int timeout_ms);
```

`struct os_pollfd` matches the POSIX layout so Linux and macOS share it; the
Windows adapter fills `revents` from `WSAPoll`.

### Dynamic extension loader

```c
int  os_ext_open(const char *path, void **handle, char *err, size_t errcap);
int  os_ext_sym(void *handle, const char *name, void **out, char *err, size_t errcap);
int  os_ext_close(void *handle);
bool os_ext_available(void);
const char *os_ext_suffix(void);
```

`os_ext_open/sym/close/available/suffix` load a shared library through the OS
loader. `os_ext_available()` decides whether a scan runs at all and
`os_ext_suffix()` names the platform's shared-library extension; `open` takes an
absolute path and returns an opaque handle, `sym` looks up an ASCII symbol name,
`close` releases the handle. The surface follows the Layer 0 rules: negative
Linux errno, out-params, caller-owned `err` (NUL-terminated on failure;
untouched on success, so callers read it only when the return is negative).
`os_ext_close(NULL)` is a no-op and close is idempotent at this layer; the
registry guarantees exactly-once. The Linux static backend stubs all of it
(`false`/`NULL`/`-ENOSYS`, §1.1); macOS and Windows implement it (§1.2/§1.3). The
scan/adopt/close policy lives on the registry side (`src/ext/dynlib.c`,
`src/ext/registry.c`), not here (`.agents/design/30-extensibility.md`).

### Constants shared by all implementations

`OS_O_*`, `OS_POLLIN/OUT/ERR/HUP` and `OS_CLOCK_*` exist once in `plat.h`, so the
adapters never disagree. Error numbers are the Linux values; each adapter
translates its native errors into them.

```c
#define OS_POLLIN  1
#define OS_POLLOUT 4
#define OS_POLLERR 8
#define OS_POLLHUP 16

#define OS_O_RDONLY    0
#define OS_O_WRONLY    1
#define OS_O_RDWR      2
#define OS_O_CREAT     0x40
#define OS_O_TRUNC     0x200
#define OS_O_APPEND    0x400
#define OS_O_NONBLOCK  0x800
#define OS_O_DIRECTORY 0x10000
#define OS_O_CLOEXEC   0x80000

#define OS_CLOCK_REALTIME  0
#define OS_CLOCK_MONOTONIC 1
```

`os_stat`/`os_fstat` fill `struct os_stat` through their `void *statbuf`
argument. The field order is logical, not any platform's native layout: each
backend converts from its own `struct stat`, so the core never depends on byte
offsets.

```c
struct os_stat {
    u64 st_dev;   u64 st_ino;   u64 st_nlink;
    u32 st_mode;  u32 st_uid;   u32 st_gid;
    u64 st_rdev;  i64 st_size;  i64 st_blksize; i64 st_blocks;
    i64 st_atime; i64 st_atime_nsec;
    i64 st_mtime; i64 st_mtime_nsec;
    i64 st_ctime; i64 st_ctime_nsec;
};
```

---

## 1.1 Linux (`src/plat/linux/*.c`)

Direct syscalls via inline `syscall` wrappers, no vDSO dependency, static ELF
built by `clang -static -nostdlib -Wl,-e,_start`. `openat` is the primitive.
Time uses `clock_gettime` plus `ppoll` when a sub-millisecond timeout is needed,
otherwise `poll`. `os_init` parses the initial stack for argc/argv/envp and
hands control to `main`.

The backend is freestanding on x86-64, aarch64 and riscv64:

- x86-64 has its own syscall numbers and `syscall`; aarch64 (`svc #0`) and
  riscv64 (`ecall`) both use the asm-generic table. The instruction, the
  numbers, and wrappers for the calls the generic ABI spells differently (no
  `dup2`/`poll`/`fork`, only the `*at` file variants, `newfstatat` for `fstat`)
  live in `src/plat/linux/syscall.h`, shared by `plat/linux` and `net/linux`.
- The entry stubs load `argc` from `sp` per architecture (`src/plat/linux/sys.c`).
- Signal handlers need an `rt_sigreturn` trampoline on all three.

Two ABI hazards are handled explicitly in `src/plat/linux/`: arm64 is the one
Linux ABI where `O_DIRECTORY` is `040000` and `O_DIRECT` is `0x10000` (the
portable constant is the x86-64 one), and riscv64 has no `renameat(2)` syscall
at all, so atomic file writes use the available rename path.

The dynamic-extension surface is a fail-closed stub (`src/plat/linux/ext.c`):
`os_ext_available()` is false, `os_ext_suffix()` is `NULL`, open/sym return
`-ENOSYS` with the message `dynamic extension loading requires macOS or
Windows`, and close is a no-op — the freestanding static ELF has no dynamic
loader.

## 1.2 macOS (`src/plat/mac/*.c`)

A clang target (`arm64-apple-macos`, `x86_64-apple-macos`). There is **no
translator**: the same C sources compile for both slices, and `plat/mac/`
implements Layer 0 over libSystem (`open`/`read`/`write`/`fstat`/
`getdirentries`/`fork`/`execve`/`posix_spawn`/`poll`/`tcgetattr`/`tcsetattr`),
with a Darwin→Linux errno table that covers values 1–106 and maps unknown values
to `-EIO`. `MAC_ARCH` picks the slice; on a Mac it defaults to the compiler's
target, and a cross build leaves it to the `--target=` triple. Linking is
`-nostdlib -Wl,-e,_start` but pulls in `libSystem`, `Security` and
`CoreFoundation` explicitly. macOS is the one backend that keeps the SDK headers
instead of `-nostdinc`, because it links a libc anyway.

The entry stub is per slice: dyld enters an arm64 image with argc/argv/envp in
x0/x1/x2, an x86-64 image with the classic argc-at-top-of-stack layout
(`src/plat/mac/rt.c`). `src/plat/mac/ext.c` implements the dynamic-extension
loader over libSystem: `dlopen(path, RTLD_NOW|RTLD_LOCAL)` resolves every symbol
before the library is handed to the registry and keeps its symbols out of the
process-wide namespace, `dlsym`/`dlerror` do lookup and diagnostics, and the
suffix is `.dylib`.

`os_getdents` uses `fdopendir`/`readdir` behind a 32-slot fd→`DIR` table;
`os_close` drops the stream through `mac_dir_drop`. Directory entries are
converted to Linux `dirent64` records in place, which fits because Linux records
are never larger than Darwin records. `os_spawn` uses fork/execve and ignores the
`ctty` argument; `os_open_url` spawns `/usr/bin/open`.

Cross building from Linux needs Apple's SDK, which nixpkgs fetches and unpacks;
the toolchain is then `clang --target=<arch>-apple-darwin -isysroot <sdk>
-fuse-ld=lld`, and `llvm-lipo` combines the slices into a universal binary.

## 1.3 Windows (`src/plat/win/*.c`)

A native clang target: `/entry:win_start`, `/nodefaultlib`, import libraries for
`kernel32`/`shell32`/`bcrypt`/`ws2_32`/`dnsapi`/`secur32`/`crypt32` — no CRT and
no Windows SDK. `WIN_ARCH=x86_64` (the `make windows` default) builds
`x86_64-pc-windows-msvc`; `make windows WIN_ARCH=arm64` builds
`aarch64-pc-windows-msvc`. The variable also selects the `llvm-dlltool -m`
machine (`i386:x86-64`/`arm64`) and lld-link's `/machine:` (`x64`/`arm64`);
`win-tests` follows the same selection and writes ARM64 binaries to
`build/test/arm64/`.

- `win_start` reads `GetCommandLineW` + `CommandLineToArgvW` → UTF-8 argv and
  `GetEnvironmentStringsW` → case-insensitively sorted UTF-8 envp.
- Stdio goes through `GetStdHandle` (works from a console `.exe`).
- A **1024-entry fd table** (`{handle, write_handle, kind, flags, path, aux}`)
  with kinds `FILE/DIR/PIPE/CONSOLE/SOCKET`. Every Layer 0 call goes through it:
  `CreateFileW` for files, `ReadFile`/`WriteFile` for I/O, `FindFirstFileW` for
  `os_getdents`, `CreateProcessW` for `os_spawn`, `WaitForSingleObject` for
  `os_wait`, `GetTickCount64` for time.
- **Pipe write behavior:** a `PIPE` fd write goes through blocking `WriteFile`
  (the handles from `CreatePipe` are synchronous), and `os_poll` has **no
  write-pipe branch** — its pipe case only serves the read side
  (`PeekNamedPipe`, a broken pipe reported as `POLLIN|POLLHUP`), and a
  `POLLOUT` request on a write end never becomes ready. `agentc_mcp_write_all`
  therefore attempts the first `PIPE_BUF` (4096-byte) chunk write-first and only
  falls into its bounded poll path when the probe returns `EAGAIN`/`EINTR`;
  later chunks poll before writing. **Documented residual:** after that first
  chunk a full request pipe can sleep past the write deadline, and a request
  larger than one `PIPE_BUF` can still time out; a complete writability fix
  belongs in `src/plat/win/sys.c`. On macOS the parent write end is blocking too
  (`os_pipe` leaves it so), so the same full-pipe probe can sleep past the
  deadline; the follow-up fix is a non-blocking parent write end. Small requests
  (the Wine golden `mcp_test write.small`) are unaffected.
- **Stack probes:** clang emits `__chkstk` for frames larger than one page;
  `rt.c` has the per-arch stub (rax-based on x86-64, x15-based on ARM64) and
  walks one 4 KiB page at a time so the OS can grow the stack.
- **Console input:** a reader thread with `ReadConsoleInputW` converts key and
  resize events into a byte pipe; the main loop polls that pipe like any other
  fd. A resize event becomes a synthetic SIGWINCH flag checked by the loop. VT
  mode is enabled on startup (`ENABLE_VIRTUAL_TERMINAL_PROCESSING |
  DISABLE_NEWLINE_AUTO_RETURN`), so ANSI rendering is byte-identical to POSIX.
  `os_poll` mixes `WSAPoll` (sockets) with `WaitForSingleObject`/
  `WaitForMultipleObjects` (pipes/console); when both kinds are present the
  socket wait is capped at 16 ms so console input latency stays TUI-friendly.
- **Shell tool:** `os_spawn_shell` resolves the configured shell and passes the
  command as one script argument. `cmd` comes from `%COMSPEC%`, then
  `%SystemRoot%\System32\cmd.exe`; `powershell` prefers PowerShell 7
  (`pwsh.exe` on `PATH`) and falls back to the built-in 5.1 at
  `%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe`; `pwsh` requires
  PowerShell 7. `auto` means `cmd.exe` because it is always present; an
  explicitly requested shell that is missing returns `-ENOENT` and the tool
  prints where it looked — it never silently falls back to another shell. `cmd`
  runs `/d /s /c` with the command quoted verbatim (cmd parses its own command
  line; MSVC-style quote escaping would leave backslashes in the text);
  PowerShell runs `-NoProfile -NonInteractive -ExecutionPolicy Bypass
  -Command`. The child inherits the user's environment and working directory.
  POSIX keeps `/bin/sh -lc` with the minimal PATH/HOME/TMPDIR/TERM environment.
- **Dynamic extension loader:** `LoadLibraryExW` with
  `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32` — the DLL's
  own directory plus System32 only, no `PATH`/current-directory search. A system
  where those flags are unsupported fails with `ERROR_INVALID_PARAMETER` and the
  loader fails closed instead of retrying the default search order.
  `GetProcAddress` looks up the ASCII symbol, `FreeLibrary` releases the handle,
  and `win_errno` maps `ERROR_MOD_NOT_FOUND`/`ERROR_PROC_NOT_FOUND` to
  `-ENOENT`, `ERROR_BAD_EXE_FORMAT` to `-ENOEXEC` and `ERROR_DLL_INIT_FAILED` to
  `-EIO`; the error text carries the Win32 code (e.g. `not a valid Win32
  application (error 193)`).

**Wine runtime coverage.** The Windows x86-64 golden suite runs under Wine in CI
(`make wine-check`, `tests/wine.sh`, Wine 11 from the dev shell). That exercises
the Win32 syscall surface, the CRT-free `win_start`, the fd table and
file/directory paths, the console reader thread, the socket/`WSAPoll`
translation, and real `cmd.exe` execution — output capture, redirection, exit
codes, cancellation, a background child that inherits the pipe, and output
truncation. Wine ships no PowerShell 7, and its `powershell.exe` is an inert
stub, so the suite selects `pwsh` and asserts the clear missing-shell error
instead of pretending a shell ran. Wine is a reimplementation of those APIs, not
Windows: it does not validate SChannel/TLS against a real provider, real
console/ConPTY behaviour, real PowerShell 5.1 or `pwsh` execution, or Windows
ARM64. The `windows-latest` CI job runs the golden suite once per shell
(informational until observed green on a real kernel) and prints which of
`cmd.exe` and PowerShell its runs verified. The dynamic-extension smoke
(`tests/dyn.sh --wine`) runs under Wine in the `linux` CI job (`make dyn-check`)
and on a real Windows kernel in the `windows-latest` job (`tests/dyn.sh
--windows`); `macos-14` runs the `dlopen` branch. Wine does not emulate a CPU,
so an ARM64 PE runs under native aarch64 Wine on an ARM64 CI runner
(non-gating). The alternative, qemu-user-static plus a registered `binfmt_misc`
handler, is a legitimate fallback — the kernel then transparently hands foreign
ELF execs to qemu — but registering the handler needs root in CI and runs Wine
under emulation, so the native runner is preferred.

## 1.4 Headless (`src/plat/headless.c`)

Deterministic clock (`os_now_ns` advances only via `test_advance_time`),
scripted input queue, in-memory terminal buffer, optional in-memory fs. Used by
every test and by `--replay`.

---

## 2. Layer 1 — networking (`include/net.h`)

Sockets diverge enough between Winsock and BSD that they get their own contract,
implemented once per OS:

```c
int  agentc_net_init(void);                              /* WSAStartup on Windows; no-op elsewhere */
int  agentc_net_socket(void);                            /* IPv4 TCP, O_NONBLOCK|O_CLOEXEC semantics */
int  agentc_net_connect(int fd, u32 ip_be, u16 port, int deadline_ms); /* 0 | -EINPROGRESS */
int  agentc_net_send(int fd, const void *p, size_t n);   /* n | -EAGAIN | -errno */
int  agentc_net_recv(int fd, void *p, size_t n);         /* n | 0 eof | -EAGAIN | -errno */
void agentc_net_close(int fd);
int  agentc_net_so_error(int fd);
int  agentc_net_set_nodelay(int fd);                     /* TCP_NODELAY */
int  agentc_net_poll(int fd, short events, int timeout_ms);  /* >0 ready, 0 timeout, -errno */

int  agentc_net_dns(const char *host, struct agentc_ip4 *out, int deadline_ms);
bool agentc_net_is_ip4(const char *host, struct agentc_ip4 *out);  /* dotted-quad fast path */

/* TLS */
typedef struct AgcTls AgcTls;
enum {
    AGENTC_TLS_VERIFY   = 1u << 0,   /* default ON; clearing it is --insecure */
    AGENTC_TLS_NO_SNI   = 1u << 1,   /* caller opt-in: suppress SNI. On stacks
                                     * where the SNI name also drives the
                                     * certificate name check, this skips that
                                     * check; the default path verifies the host
                                     * (including an IP address SAN). */
    AGENTC_TLS_MIN_1_2  = 1u << 2,   /* default */
};
AgcTls *agentc_tls_new(int fd, const char *host, u16 port, unsigned flags);
int    agentc_tls_handshake(AgcTls *t);                    /* 0 | -EAGAIN | -errno */
int    agentc_tls_read(AgcTls *t, void *p, size_t n);      /* n | 0 eof | -EAGAIN | -errno */
int    agentc_tls_write(AgcTls *t, const void *p, size_t n);
void   agentc_tls_close(AgcTls *t);
const char *agentc_tls_error(AgcTls *t);                   /* human-readable, never NULL */
```

**Integration rule:** all sockets are non-blocking. `-EAGAIN` is never an error;
the caller waits for readiness through `agentc_net_poll`, which every backend
implements (the replay backend reports readiness immediately so a replayed
read advances). `agentc_net_poll` returns the `os_poll` result, so a real poll
error (`-EBADF`, `-EINVAL`) reaches the caller instead of being treated as
readiness.
`agentc_net_connect` returning `-EINPROGRESS` means "poll for `OS_POLLOUT`, then
read `agentc_net_so_error`". The TLS handshake is a resumable state machine: the
loop calls `agentc_tls_handshake` whenever the fd is readable or writable and
continues on `-EAGAIN`. That keeps TLS byte-for-byte testable against a scripted
server.

## 2.1 Linux (`src/net/linux/*.c`)

Raw `socket`/`connect`/`send`/`recv`/`setsockopt`/`shutdown`/`close` syscalls.
`TCP_NODELAY=1` is set immediately after connect (agent traffic is small
frames). `dns.c` parses `/etc/hosts`, reads the first `nameserver` from
`/etc/resolv.conf`, builds A/AAAA queries by hand over a non-blocking UDP
socket, retries truncated responses (TC bit) over TCP, follows CNAME chains,
uses a 2 s deadline, and falls back to `1.1.1.1`/`8.8.8.8` only when no
nameserver is configured.

## 2.2 macOS (`src/net/mac/*.c`)

BSD sockets via libSystem and the same hand-rolled UDP resolver against
`/etc/resolv.conf` (present on macOS). `getaddrinfo` is not used — the tree stays
libc-free even here.

## 2.3 Windows (`src/net/win/*.c`)

`WSAStartup(2.2)`, `socket`, non-blocking `connect` + `WSAPoll`, `send`/`recv`,
`closesocket`, `ioctlsocket(FIONBIO)`. DNS prefers
`DnsQuery_A(host, DNS_TYPE_A, DNS_QUERY_STANDARD, ...)` from `dnsapi` and falls
back to the shared UDP resolver; `DnsQuery_A` is synchronous and ignores
`deadline_ms`. WSA error codes map to the shared negative errno set
(`WSAEWOULDBLOCK→-EAGAIN`, `WSAECONNRESET→-ECONNRESET`, ...).

## 2.4 Mock (`src/net/mock.c`) — for tests

Scripted TCP byte streams loaded from disk (`tests/data/wire/*.bin`) with an
optional fault plan (stall, partial reads, RST). `agentc_net_dns` returns fixed
IPs. `agentc_tls_*` can be a passthrough `tls_plain`, so HTTP/SSE logic is tested
without crypto; the real TLS bindings are tested separately against a local TLS
test server or recorded handshakes.

---

## 3. TLS paths

There is exactly one `agentc_tls_*` contract, and each platform backend
implements it:

| Platform | TLS backend | Where |
|---|---|---|
| Linux | vendored freestanding mbedTLS 3.6.2 | `third_party/mbedtls`, `third_party/mbedtls_glue.c`, `src/net/linux/tls_shim.c` |
| macOS | SecureTransport | `src/net/mac/tls.c` |
| Windows | SChannel (+ `crypt32` chain validation) | `src/net/win/tls.c` |
| tests/mock | passthrough or recorded handshakes | `src/net/mock.c` |

**No `dlopen` in the TLS path.** The TLS library is always a link-time
dependency: mbedTLS objects are compiled into the Linux static binary and call
only `agentc_memcpy`/`agentc_memset`/`agentc_memcmp`, `agentc_alloc`,
`os_random`, `os_now_ns`, file helpers and `agentc_net_send`/`recv`; macOS links
`Security`/`CoreFoundation` and Windows links `secur32`/`crypt32` import
libraries. The dynamic-extension loader (`os_ext_*`) uses the OS loader for
extension shared libraries and is unrelated to TLS.

### 3.1 Vendored mbedTLS on Linux

The build compiles a pinned source subset in freestanding mode:

```sh
clang -O2 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
      -DMBEDTLS_CONFIG_FILE='"mbedtls_agentc_config.h"' \
      -c third_party/mbedtls_glue.c $(cat tools/mbedtls-sources.txt)
```

Wiring:

- `third_party/mbedtls_agentc_config.h` — one config for all targets: no
  filesystem I/O, no timing callbacks we do not supply, no `printf`, no `fork`,
  TLS 1.2 + 1.3, X.509 parse, PEM bundle, no DTLS, no self-tests.
- `third_party/mbedtls_glue.c` — the hooks:
  `mbedtls_platform_set_calloc_free` → `agentc_alloc`/`agentc_free`;
  `mbedtls_platform_set_time` → `os_now_ns`; `mbedtls_hardware_poll` (entropy) →
  `os_random`; `mbedtls_platform_set_nv_seed` disabled; and an
  `mbedtls_ssl_set_bio` pair over `agentc_net_send`/`agentc_net_recv`.
- `src/net/linux/tls_shim.c` — `agentc_tls_*` over the mbedTLS context and BIO.
- `tools/mbedtls-sources.txt` — the exact upstream source subset, so the object
  set is identical on every Linux architecture.
- `third_party/cacert.pem` — the Mozilla CA bundle, embedded at build time as C
  data. A system bundle (`/etc/ssl/certs/ca-certificates.crt` or the platform
  equivalent) replaces it when present.
- TLS is verified by default; `agentc_tls_error` maps failures to readable
  strings.

### 3.2 macOS SecureTransport

`src/net/mac/tls.c` builds the client over `SSLCreateContext` and non-blocking
`SSLRead`/`SSLWrite`, checks the peer chain with
`SSLCopyPeerTrust`/`SecTrustEvaluate` against the bundled CA store, and uses
`SSLSetIOFuncs`/`SSLHandshake`. This is the one backend that does not use the
vendored mbedTLS.

### 3.3 Windows SChannel

`src/net/win/tls.c` drives SChannel (`secur32` + `crypt32`) over Winsock with
manual certificate validation. There is no mbedTLS code path on Windows.

### 3.4 Security policy

- Pin an upstream tag (`3.6.2`); record the exact source subset in
  `THIRD_PARTY.md` with its license.
- Watch upstream advisories in CI (a scheduled workflow opens an issue when the
  pinned version is flagged); treat TLS fixes as release blockers.
- `--insecure` never disables verification silently: it must be explicit and is
  reflected in the status bar and the session log.

---

## 4. Entropy, time, and TLS seed

`os_random` is the only entropy source: `getrandom(2)` on Linux,
`getentropy`/`arc4random_buf` on macOS, and
`BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG)` on Windows. mbedTLS consumes
it through the hardware-poll hook on Linux; SecureTransport and SChannel use the
platform source directly. A small ChaCha20 DRBG seeded once at startup covers
session IDs and retry jitter — one syscall per process.

---

## 5. Failure policy

- Core startup failures (no terminal, no memory) → `agentc_die(msg)`, exit 1.
- Network failures → errno, retried by `wire/retry.c` with backoff; surfaced to
  the UI as a red tool/assistant error card; never abort the process.
- TLS verification failure → hard error with the specific reason string
  (`agentc_tls_error`), never silently downgraded. The host name is always
  passed to the TLS layer for identity verification, including IP literals
  (checked against the certificate's `iPAddress` SAN); `AGENTC_TLS_NO_SNI` is a
  caller opt-in that suppresses SNI, and where the stack derives the name check
  from SNI it also skips that check. `--insecure` is opt-in and prints a warning
  banner in the status bar.
- Extension crash → the process crashes, exactly as any in-process extension
  does. Isolation is the user's container's job; documented.
