/* mac_internal.h — private declarations shared by the macOS Layer 0/1 code.
 *
 * The macOS port links libSystem (see tools/build-mac.sh); platform files are
 * the only ones allowed to include SDK headers, and they do so for the real
 * Darwin struct layouts and constants (struct stat, termios/NCCS, dirent,
 * winsize, O_*, FD_*, CLOCK_*). The core stays header-free.
 */
#ifndef AGENTC_PLAT_MAC_INTERNAL_H
#define AGENTC_PLAT_MAC_INTERNAL_H

#include "agentc.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Not declared by the headers we include. */
void arc4random_buf(void *buf, size_t n);

/* Translate a Darwin errno value into the negative Linux errno the whole
 * codebase speaks (include/plat.h). Never returns 0. */
int mac_errno(int e);

/* Environment array captured by os_init() (mac/rt.c). */
char **mac_environ(void);

/* os_close() hands directory fds to this hook (mac/sys.c); returns true when a
 * fdopendir() stream owned the fd and was closed. */
bool mac_dir_drop(int fd);

/* Darwin's <sys/stat.h> defines st_atime/st_mtime/st_ctime as macros over
 * st_*timespec. struct os_stat (plat.h) has plain members with those names and
 * mac/sys.c assigns them, so drop the macros (nothing here reads the Darwin
 * spellings). */
#undef st_atime
#undef st_mtime
#undef st_ctime

#endif /* AGENTC_PLAT_MAC_INTERNAL_H */
