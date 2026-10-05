/* plat/linux/boot.c — the Linux process entry stub.
 *
 * This is the only object that may reference `agentc_main` and the only one
 * that defines `_start`; both are deliberately kept out of build/libagentc.a
 * (B-D1) so a host that embeds the engine can provide its own main and never
 * collide with the freestanding entry.
 *
 * The kernel enters every Linux program with `sp` pointing at argc. x86-64
 * passes it in %rdi, aarch64 in x0, riscv64 in a0. os_init() decodes that image
 * and publishes argv/envp to the platform layer.
 */
#include "agentc.h"
#include "plat.h"

#include "plat/linux/syscall.h"

int agentc_main(int argc, char **argv);

/* os_init() lives in sys.c and writes these; declared here so the entry can
 * hand the decoded argc/argv to agentc_main without sys.o referencing it. */
extern long os_argc;
extern char **os_argv;

[[noreturn]] void agentc_start(void *stack) {
    os_init(stack);
    os_exit(agentc_main((int)os_argc, os_argv));
    __builtin_unreachable();
}

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
