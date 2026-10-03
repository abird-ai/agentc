/* plat/linux/ext.c — dynamic extension loader stub for the freestanding
 * static Linux build.
 *
 * The Linux binary links no libc and cannot dlopen a shared object, so the
 * whole surface fails closed: os_ext_available() is false, os_ext_suffix()
 * NULL, and open/sym return -ENOSYS with one clear message. The registry never
 * calls open when available() is false, so the message is only a diagnostic for
 * a direct caller. */
#include "agentc.h"
#include "plat.h"

static int ext_unsupported(const char *what, char *err, size_t errcap) {
    (void)what;
    if (err && errcap > 0) {
        const char *msg = "dynamic extension loading requires macOS or Windows";
        size_t n = agentc_strlen(msg);
        if (n >= errcap) n = errcap - 1;
        agentc_memcpy(err, msg, n);
        err[n] = 0;
    }
    return -38; /* ENOSYS */
}

int os_ext_open(const char *path, void **handle, char *err, size_t errcap) {
    (void)path;
    if (handle) *handle = NULL;
    return ext_unsupported("open", err, errcap);
}

int os_ext_sym(void *handle, const char *name, void **out, char *err, size_t errcap) {
    (void)handle;
    (void)name;
    if (out) *out = NULL;
    return ext_unsupported("sym", err, errcap);
}

int os_ext_close(void *handle) {
    (void)handle;   /* idempotent NULL; nothing is ever open on this platform */
    return 0;
}

bool os_ext_available(void) { return false; }

const char *os_ext_suffix(void) { return NULL; }
