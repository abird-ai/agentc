/* plat/mac/rt.c — macOS bootstrap and process-wide globals.
 *
 * The Mach-O entry point is the linker symbol `_start` (so that the build's
 * `-Wl,-e,_start` resolves). dyld enters arm64 executables with argc/argv/envp
 * in x0/x1/x2; mac_entry() rebuilds the Linux-style initial stack image the
 * rest of agentc expects (argc, argv..., NULL, envp..., NULL) and hands it to
 * agentc_start(), exactly like src/plat/linux/sys.c does with %rsp.
 *
 * A C function named `_start` would be mangled to `__start` by the Darwin
 * assembler, hence the file-scope asm for the entry symbol.
 */
#include "agentc.h"
#include "plat.h"
#include "mac_internal.h"

int agentc_main(int argc, char **argv);

static u64 g_argc;
static char **g_argv;
static char **g_envp;
static bool g_inited;

/* dyld enters an arm64 image with argc/argv/envp in x0/x1/x2, but an x86-64
 * image with the classic argc-at-top-of-stack layout, so the two stubs differ.
 * The x86-64 stub keeps a pointer to the original image for agentc_start, but
 * must realign %rsp before entering C: dyld does not promise 16-byte alignment
 * at _start, and a C call made from an rsp == 8 (mod 16) entry can fault in any
 * libSystem/CoreFoundation SSE code (crt1.o realigns for the same reason).
 * arm64 is entered with sp already 16-byte aligned and has no call-frame skew. */
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
#  error "unsupported macOS architecture"
#endif

__attribute__((noreturn)) void agentc_start(void *stack) {
    os_init(stack);
    os_exit(agentc_main((int)g_argc, g_argv));
    __builtin_unreachable();
}

/* x86-64: dyld already left the Linux-shaped stack image in place; the stub
 * preserved a pointer to it in %rdi and aligned %rsp for the call. */
__attribute__((noinline, used, noreturn)) void mac_entry_stack(void *sp) {
    agentc_start(sp);
    __builtin_unreachable();
}

__attribute__((noinline, used, noreturn)) void mac_entry(u64 argc, char **argv,
                                                          char **envp) {
    u64 envc = 0;
    if (envp != NULL) {
        while (envp[envc] != NULL) envc++;
    }
    /* argc + argv + NULL + envp + NULL, with headroom for alignment. */
    u64 words = 1 + argc + 1 + envc + 1;
    void **sp = __builtin_alloca((words + 4) * sizeof(void *));
    u64 i = 0;
    sp[i++] = (void *)(uintptr_t)argc;
    for (u64 j = 0; j < argc; j++) sp[i++] = (argv != NULL) ? argv[j] : NULL;
    sp[i++] = NULL;
    for (u64 j = 0; j < envc; j++) sp[i++] = envp[j];
    sp[i++] = NULL;
    agentc_start(sp);
}

const char *os_platform(void) { return "macos"; }

void os_init(void *initial_stack) {
    long *sp = initial_stack;
    g_argc = (u64)sp[0];
    g_argv = (char **)(sp + 1);
    g_envp = g_argv + g_argc + 1;
    g_inited = true;
}

void os_exit(int code) {
    _exit(code);
    __builtin_unreachable();
}

const char *os_getenv(const char *name) {
    if (!g_inited || name == NULL) return NULL;
    size_t n = agentc_strlen(name);
    for (char **e = g_envp; *e != NULL; e++) {
        if (agentc_str_eq(*e, n, name, n) && (*e)[n] == '=') return *e + n + 1;
    }
    return NULL;
}

char **mac_environ(void) { return g_envp; }
