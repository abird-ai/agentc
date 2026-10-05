/* plat/mac/ext.c — macOS dynamic extension loader (dlopen/dlsym/dlclose).
 *
 * RTLD_NOW resolves every symbol before the library is handed to the registry,
 * so a partially resolvable extension fails at open, and RTLD_LOCAL keeps its
 * symbols out of the process-wide namespace (two extensions cannot collide).
 * Error text comes from dlerror(); the registry copies it into its log, so the
 * text must be NUL-terminated by us. Return values are negative linux errno;
 * the concrete loader failure is in the message, not the code. */
#include "agentc.h"
#include "plat.h"
#include "mac_internal.h"

static void ext_error(char *err, size_t cap, const char *msg) {
    if (!err || cap == 0) return;
    if (!msg || !msg[0]) msg = "unknown dynamic loader error";
    size_t n = agentc_strlen(msg);
    if (n >= cap) n = cap - 1;
    agentc_memcpy(err, msg, n);
    err[n] = 0;
}

int os_ext_open(const char *path, void **handle, char *err, size_t errcap) {
    if (handle) *handle = NULL;
    if (!path || !path[0]) {
        ext_error(err, errcap, "no library path");
        return -22; /* EINVAL */
    }
    (void)dlerror();   /* clear any stale message */
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        ext_error(err, errcap, e ? e : "dlopen failed");
        return -5; /* EIO */
    }
    if (handle) *handle = h;
    return 0;
}

int os_ext_sym(void *handle, const char *name, void **out, char *err, size_t errcap) {
    if (out) *out = NULL;
    if (!handle || !name || !name[0]) {
        ext_error(err, errcap, "no library handle or symbol name");
        return -22; /* EINVAL */
    }
    (void)dlerror();
    void *sym = dlsym(handle, name);
    if (!sym) {
        const char *e = dlerror();
        ext_error(err, errcap, e ? e : "symbol not found");
        return -5; /* EIO */
    }
    if (out) *out = sym;
    return 0;
}

int os_ext_close(void *handle) {
    if (!handle) return 0;   /* idempotent NULL */
    return dlclose(handle) == 0 ? 0 : -5; /* EIO */
}

bool os_ext_available(void) { return true; }

const char *os_ext_suffix(void) { return ".dylib"; }
