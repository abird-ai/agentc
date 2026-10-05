/* plat/mac/boot.c — the Mach-O process entry stub.
 *
 * This is the only object that references `agentc_main` and the only one that
 * defines `_start`; both stay out of build/libagentc.a (B-D1). dyld enters an
 * arm64 image with argc/argv/envp in x0/x1/x2, but an x86-64 image with the
 * classic argc-at-top-of-stack layout, so the two stubs differ.
 *
 * x86-64 must realign %rsp before entering C: dyld does not promise 16-byte
 * alignment at _start, and a call made from an rsp == 8 (mod 16) entry can
 * fault in libSystem/CoreFoundation SSE code (crt1.o realigns for the same
 * reason). arm64 is entered with sp already aligned.
 */
#include "agentc.h"
#include "plat.h"

int agentc_main(int argc, char **argv);

#if defined(__aarch64__) || defined(__arm64__)
__asm__(".text\n"
        ".globl _start\n"
        ".p2align 2\n"
        "_start:\n"
        "  b _mac_entry\n");
#elif defined(__x86_64__)
__asm__(".text\n"
        ".globl _start\n"
        ".p2align 2\n"
        "_start:\n"
        "  movq %rsp, %rdi\n"
        "  andq $-16, %rsp\n"
        "  callq _mac_entry_stack\n"
        "  ud2\n");
#else
#error "unsupported macOS architecture"
#endif

/* x86-64: dyld already left the Linux-shaped stack image in place; the stub
 * preserved a pointer to it in %rdi and aligned %rsp for the call. */
__attribute__((noinline, used, noreturn)) void mac_entry_stack(void *sp) {
    long *st = sp;
    u64 argc = (u64)st[0];
    char **argv = (char **)(st + 1);
    char **envp = argv + argc + 1;
    os_embed_init(argv, envp);
    os_exit(agentc_main((int)argc, argv));
    __builtin_unreachable();
}

/* arm64: dyld hands argc/argv/envp in registers; publish them and run. */
__attribute__((noinline, used, noreturn)) void mac_entry(u64 argc, char **argv, char **envp) {
    os_embed_init(argv, envp);
    os_exit(agentc_main((int)argc, argv));
    __builtin_unreachable();
}
