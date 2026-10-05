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

/* Process argv/envp, filled by os_init() from the dyld stack image or by
 * os_embed_init() from the host's own argc/argv. The boot stub in boot.c reads
 * these directly (it must own the only reference to agentc_main, which is
 * excluded from the embeddable archive). */
u64 os_argc;
char **os_argv;
char **os_envp;
bool os_inited;

const char *os_platform(void) {
    return "macos";
}

void os_init(void *initial_stack) {
    long *sp = initial_stack;
    os_argc = (u64)sp[0];
    os_argv = (char **)(sp + 1);
    os_envp = os_argv + os_argc + 1;
    os_inited = true;
}

/* Embedding seam (plat.h): a host that already owns the process (no dyld
 * entry stub) publishes its argv/envp here. The arrays are borrowed and must
 * outlive the engine. */
void os_embed_init(char **argv, char **envp) {
    static char *no_env[] = {NULL};
    u64 argc = 0;
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
    _exit(code);
    __builtin_unreachable();
}

const char *os_getenv(const char *name) {
    if (!os_inited || name == NULL)
        return NULL;
    size_t n = agentc_strlen(name);
    for (char **e = os_envp; *e != NULL; e++) {
        if (agentc_str_eq(*e, n, name, n) && (*e)[n] == '=')
            return *e + n + 1;
    }
    return NULL;
}

char **mac_environ(void) {
    return os_envp;
}
