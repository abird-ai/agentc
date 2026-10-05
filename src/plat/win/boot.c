/* plat/win/boot.c — the Win32 process entry point.
 *
 * This is the only object that references `agentc_main`; the entry symbol
 * `win_start` is named by the link line (/entry:win_start) and stays out of
 * build/libagentc.a (B-D1). os_init(NULL) parses GetCommandLineW and publishes
 * argv/envp to the platform layer.
 */
#include "agentc.h"
#include "plat.h"

int agentc_main(int argc, char **argv);

extern int os_argc;
extern char **os_argv;

__attribute__((noreturn)) void win_start(void) {
    os_init(NULL);
    os_exit(agentc_main(os_argc, os_argv));
    __builtin_unreachable();
}
