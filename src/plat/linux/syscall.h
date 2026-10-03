/* syscall.h — the Linux syscall shim shared by Layer 0 and the linux Layer 1
 * backend.
 *
 * One header so both files agree on the instruction, the numbering and the
 * places where the ABI forces a different syscall:
 *
 *   x86-64   its own numbers, `syscall`, SA_RESTORER, dup2/poll/fork/mkdir/…
 *   aarch64  the asm-generic numbers, `svc #0`
 *   riscv64  the asm-generic numbers, `ecall`
 *
 * The asm-generic ABI has no dup2/poll/fork and only the *at variants of
 * mkdir/unlink/rename, so linux_* wrappers below paper over that; call sites
 * use the wrappers and never a raw number for those.
 *
 * Layering note: this header is deliberately below both layers (it contains no
 * OS policy, only the instruction and the numbers). The code under src/net/linux
 * is Layer 1
 * but is still linux-only code, so sharing this with `src/plat/linux/sys.c` is
 * the same-OS case, not core-including-platform.
 */
#ifndef AGENTC_LINUX_SYSCALL_H
#define AGENTC_LINUX_SYSCALL_H

#if defined(__x86_64__)

#define LINUX_ARCH_X86_64 1

static inline long linux_sc1(long n, long a) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
    return r;
}
static inline long linux_sc2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}
static inline long linux_sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static inline long linux_sc4(long n, long a, long b, long c, long d) {
    register long r10 __asm__("r10") = d;
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}
static inline long linux_sc5(long n, long a, long b, long c, long d, long e) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}
static inline long linux_sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_fstat 5
#define SYS_poll 7
#define SYS_ftruncate 77
#define SYS_lseek 8
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_rt_sigaction 13
#define SYS_rt_sigreturn 15
#define SYS_ioctl 16
#define SYS_dup2 33
#define SYS_nanosleep 35
#define SYS_fork 57
#define SYS_execve 59
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_fcntl 72
#define SYS_getcwd 79
#define SYS_chdir 80
#define SYS_rename 82
#define SYS_renameat2 316
#define SYS_mkdir 83
#define SYS_unlink 87
#define SYS_getdents64 217
#define SYS_clock_gettime 228
#define SYS_exit_group 231
#define SYS_openat 257
#define SYS_mkdirat 258
#define SYS_newfstatat 262
#define SYS_unlinkat 263
#define SYS_renameat 264
#define SYS_pipe2 293
#define SYS_getrandom 318
#define SYS_socket 41
#define SYS_connect 42
#define SYS_sendto 44
#define SYS_recvfrom 45
#define SYS_setsockopt 54
#define SYS_getsockopt 55

#elif defined(__aarch64__) || (defined(__riscv) && __riscv_xlen == 64)

#if defined(__aarch64__)
#define LINUX_ARCH_AARCH64 1
#define LINUX_SVC "svc #0"
#else
#define LINUX_ARCH_RISCV64 1
#define LINUX_SVC "ecall"
#endif

/* aarch64: x8 = number, x0..x5 = args, result in x0.
 * riscv64: a7 = number, a0..a5 = args, result in a0. */
#if defined(__aarch64__)
#define LINUX_SC_REG(n) register long linux_nr __asm__("x8") = (n)
#define LINUX_ARG_REG(i) register long **linux_args __asm__(0)
static inline long linux_sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory", "cc");
    return x0;
}
#else
static inline long linux_sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
                     : "memory");
    return a0;
}
#endif

static inline long linux_sc1(long n, long a) { return linux_sc6(n, a, 0, 0, 0, 0, 0); }
static inline long linux_sc2(long n, long a, long b) { return linux_sc6(n, a, b, 0, 0, 0, 0); }
static inline long linux_sc3(long n, long a, long b, long c) {
    return linux_sc6(n, a, b, c, 0, 0, 0);
}
static inline long linux_sc4(long n, long a, long b, long c, long d) {
    return linux_sc6(n, a, b, c, d, 0, 0);
}
static inline long linux_sc5(long n, long a, long b, long c, long d, long e) {
    return linux_sc6(n, a, b, c, d, e, 0);
}

/* asm-generic numbering */
#define SYS_read 63
#define SYS_write 64
#define SYS_close 57
#define SYS_fstat 80
#define SYS_ftruncate 46
#define SYS_ppoll 73
#define SYS_lseek 62
#define SYS_mmap 222
#define SYS_munmap 215
#define SYS_rt_sigaction 134
#define SYS_rt_sigreturn 139
#define SYS_ioctl 29
#define SYS_dup3 24
#define SYS_nanosleep 101
#define SYS_clone 220
#define SYS_execve 221
#define SYS_wait4 260
#define SYS_kill 129
#define SYS_fcntl 25
#define SYS_getcwd 17
#define SYS_chdir 49
#define SYS_getdents64 61
#define SYS_clock_gettime 113
#define SYS_exit_group 94
#define SYS_openat 56
#define SYS_mkdirat 34
#define SYS_newfstatat 79
#define SYS_unlinkat 35
#define SYS_renameat 38
#define SYS_renameat2 276
#define SYS_pipe2 59
#define SYS_getrandom 278
#define SYS_socket 198
#define SYS_connect 203
#define SYS_sendto 206
#define SYS_recvfrom 207
#define SYS_setsockopt 208
#define SYS_getsockopt 209

#else
#error "agentc: unsupported architecture for the linux backend (see src/plat/linux/syscall.h)"
#endif

/* ------------------------------------------------- portable syscall wrappers */

#define LINUX_AT_FDCWD (-100)

/* dup2(2) is x86-only; the generic ABI has dup3. */
static inline long linux_dup2(int oldfd, int newfd) {
#if defined(LINUX_ARCH_X86_64)
    return linux_sc2(SYS_dup2, oldfd, newfd);
#else
    return linux_sc3(SYS_dup3, oldfd, newfd, 0);
#endif
}

/* poll(2) is x86-only; the generic ABI has ppoll with a timespec. */
static inline long linux_poll(void *fds, unsigned long nfds, int timeout_ms) {
#if defined(LINUX_ARCH_X86_64)
    return linux_sc3(SYS_poll, (long)fds, (long)nfds, timeout_ms);
#else
    struct {
        i64 sec;
        i64 nsec;
    } ts, *pts = 0;
    if (timeout_ms >= 0) {
        ts.sec = timeout_ms / 1000;
        ts.nsec = (i64)(timeout_ms % 1000) * 1000000;
        pts = &ts;
    }
    /* ppoll(fds, nfds, timeout, sigmask, sigsetsize) */
    return linux_sc5(SYS_ppoll, (long)fds, (long)nfds, (long)pts, 0, 8);
#endif
}

/* fork(2) is aarch64/riscv has clone(SIGCHLD) instead; both produce a child
 * that shares the fd table, which is all os_spawn() needs. */
static inline long linux_fork(void) {
#if defined(LINUX_ARCH_X86_64)
    return linux_sc1(SYS_fork, 0);
#else
    /* clone(flags, stack, parent_tid, child_tid, tls); SIGCHLD = 17 */
    return linux_sc5(SYS_clone, 17, 0, 0, 0, 0);
#endif
}

/* mkdir/unlink/rename exist only in their *at spelling on the generic ABI. */
static inline long linux_mkdir(const char *path, int mode) {
#if defined(LINUX_ARCH_X86_64)
    return linux_sc2(SYS_mkdir, (long)path, mode);
#else
    return linux_sc3(SYS_mkdirat, LINUX_AT_FDCWD, (long)path, mode);
#endif
}

static inline long linux_unlink(const char *path) {
#if defined(LINUX_ARCH_X86_64)
    return linux_sc1(SYS_unlink, (long)path);
#else
    return linux_sc2(SYS_unlinkat, LINUX_AT_FDCWD, (long)path);
#endif
}

/* rename(2) is x86-only and riscv64 does not even provide renameat(2): asm-generic
 * only defines __NR_renameat when the arch sets __ARCH_WANT_RENAMEAT (aarch64 does,
 * riscv64 does not). renameat2(..., 0) is present everywhere and is the kernel's
 * own fallback for both. */
static inline long linux_rename(const char *from, const char *to) {
    return linux_sc5(SYS_renameat2, LINUX_AT_FDCWD, (long)from, LINUX_AT_FDCWD, (long)to, 0);
}

/* Signal trampoline. x86-64 needs SA_RESTORER; arm64 uses sa_restorer as the
 * handler's return address, so both get a real stub, riscv64 ignores it. */
__attribute__((naked, unused)) static void linux_sig_restorer(void) {
#if defined(LINUX_ARCH_X86_64)
    __asm__("movq $15, %rax\n\tsyscall");
#elif defined(LINUX_ARCH_AARCH64)
    __asm__("mov x8, #139\n\tsvc #0");
#else
    __asm__("li a7, 139\n\tecall");
#endif
}

static inline long linux_rt_sigaction(long sig, long handler) {
    struct {
        void (*h)(int);
        unsigned long flags;
        void (*restorer)(void);
        unsigned long mask;
    } sa;
    sa.h = (void (*)(int))handler;
    sa.flags = 0;
    sa.mask = 0;
#if defined(LINUX_ARCH_X86_64)
    sa.restorer = linux_sig_restorer;
    sa.flags = handler ? 0x04000000UL /* SA_RESTORER */ : 0UL;
#else
    sa.restorer = handler ? linux_sig_restorer : 0;
#endif
    return linux_sc4(SYS_rt_sigaction, sig, (long)&sa, 0, 8);
}

#endif /* AGENTC_LINUX_SYSCALL_H */
