/* dynlib_int.h — internal seam between the extension registry and the
 * dynamic-library scan.
 *
 * The registry calls agentc_ext_register_dynamic() (ext.h); this header holds
 * the pieces that unit tests inject: the AgcExtDynLoader vtable (the default
 * wraps the Layer 0 os_ext_* loader) and the scan/path policy. Nothing here is
 * part of the extension ABI.
 */
#ifndef AGENTC_EXT_DYNLIB_INT_H
#define AGENTC_EXT_DYNLIB_INT_H

#include "agentc.h"

/* Maximum dynamic extensions registered from one scan. */
#define AGENTC_EXT_MAX_DYNAMIC 32

/* The platform-loader seam. `open`/`sym`/`close` mirror the os_ext_* contract;
 * `available`/`suffix` decide whether a scan runs at all. The registry never
 * calls a slot it did not check; the default table is never NULL, and
 * agentc_ext_dyn_set_loader() rejects (logs and ignores) any injected table
 * with a missing slot, so the active table is complete by construction. */
typedef struct AgcExtDynLoader {
    int (*open)(const char *path, void **handle, char *err, size_t errcap);
    int (*sym)(void *handle, const char *name, void **out, char *err, size_t errcap);
    int (*close)(void *handle);
    bool (*available)(void);
    const char *(*suffix)(void);
} AgcExtDynLoader;

/* The active loader (platform by default) and the test injection point. NULL
 * restores the platform loader. */
const AgcExtDynLoader *agentc_ext_dyn_loader(void);
void agentc_ext_dyn_set_loader(const AgcExtDynLoader *loader);

/* Scan <config home>/extensions/ for loadable library files and invoke `cb(ud,
 * stem, path)` for each, bytewise-sorted by stem. A file qualifies when its
 * name ends with the loader suffix (ASCII case-insensitive), the stem is a
 * valid extension name ([a-z0-9_.:-]{1,64}), and agentc_ext_dyn_path_ok()
 * accepts it; directories and unknown suffixes are ignored. At most
 * AGENTC_EXT_MAX_DYNAMIC candidates are offered, with one overflow log when
 * the directory holds more. A missing directory yields 0 silently. Returns the
 * number offered. No recursion, no symlink resolution, no `extensions.paths`. */
typedef void (*AgcExtDynScanFn)(void *ud, const char *stem, const char *path);
size_t agentc_ext_dyn_scan(AgcExtDynScanFn cb, void *ud);

/* Path policy: the target must be a regular file, and on POSIX
 * neither the file nor its parent directory may be group- or other-writable.
 * Symlinks are followed (the permission check is on the target). Windows has
 * no group/other mode bits, so the permission half is a no-op there. */
bool agentc_ext_dyn_path_ok(const char *path);

#endif /* AGENTC_EXT_DYNLIB_INT_H */
