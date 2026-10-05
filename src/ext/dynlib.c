/* dynlib.c — dynamic-extension scan and the injectable platform-loader seam.
 *
 * One fixed directory, <config home>/extensions/, is scanned once per
 * composition: the loader's suffix selects candidates (ASCII
 * case-insensitive), the file stem must be a valid extension name
 * [a-z0-9_.:-]{1,64}, and agentc_ext_dyn_path_ok() decides whether the file is
 * safe to load. The result is bytewise-sorted and capped at
 * AGENTC_EXT_MAX_DYNAMIC with one overflow log. There is no recursive walk, no
 * project directory, no `extensions.paths` and no symlink resolution (a
 * symlink is followed by os_stat, so the permission check applies to the
 * target; the residual TOCTOU window is documented).
 *
 * The registry loads through agentc_ext_dyn_loader(); tests replace it with an
 * in-process fake so the whole lifecycle runs without producing real dylibs.
 *
 * config.c helpers are declared here the way resources.c/session.c declare
 * them (they are internal shared helpers, not part of config.h).
 */
#include "agentc.h"
#include "plat.h"
#include "config.h"
#include "ext/dynlib_int.h"

typedef int (*AgcDirFn)(void *ud, const char *name, const char *full, bool is_dir);
extern int agentc_dir_scan(const char *dir, AgcDirFn cb, void *ud);
extern bool agentc_path_join(char *out, size_t cap, const char *dir, const char *name);

#define DYN_PATH_MAX 4096

/* ------------------------------------------------------- platform seam */

static int platform_open(const char *path, void **handle, char *err, size_t errcap) {
    return os_ext_open(path, handle, err, errcap);
}
static int platform_sym(void *handle, const char *name, void **out, char *err,
                        size_t errcap) {
    return os_ext_sym(handle, name, out, err, errcap);
}
static int platform_close(void *handle) { return os_ext_close(handle); }
static bool platform_available(void) { return os_ext_available(); }
static const char *platform_suffix(void) { return os_ext_suffix(); }

static const AgcExtDynLoader g_platform_loader = {
    .open = platform_open,
    .sym = platform_sym,
    .close = platform_close,
    .available = platform_available,
    .suffix = platform_suffix,
};

static const AgcExtDynLoader *g_loader = &g_platform_loader;

const AgcExtDynLoader *agentc_ext_dyn_loader(void) { return g_loader; }

void agentc_ext_dyn_set_loader(const AgcExtDynLoader *loader) {
    if (!loader) {
        g_loader = &g_platform_loader;
        return;
    }
    /* The registry calls every one of these slots after checking that the
     * active table is complete (open/sym/close in dyn_register_cb, the probe
     * pair in dyn_scan_cb). An incomplete injection must never become active:
     * an unchecked close/open/sym/suffix slot would crash the scan. */
    if (!loader->open || !loader->sym || !loader->close ||
        !loader->available || !loader->suffix) {
        agentc_logf(3, "ext: dynamic loader table incomplete, ignored");
        return;
    }
    g_loader = loader;
}

/* ------------------------------------------------------------- path policy */

/* POSIX file-type and permission bits (st_mode); Windows reports the same
 * S_IFMT values from GetFileAttributes (see win/sys.c fill_stat), so the
 * regular-file check is portable and only the permission half is POSIX-only. */
#define DYN_S_IFMT  0170000u
#define DYN_S_IFREG 0100000u
#define DYN_S_IFDIR 0040000u
#define DYN_WRITE_GROUP_OTHER 0022u

static bool posix_host(void) {
    return !agentc_streq(os_platform(), "windows");
}

/* Parent directory of `path` into `dir` (path must contain a separator that is
 * not the first character). */
static bool parent_dir(const char *path, char *dir, size_t cap) {
    size_t n = agentc_strlen(path);
    if (n == 0 || n >= cap) return false;
    agentc_memcpy(dir, path, n + 1);
    char *slash = NULL;
    for (size_t i = 0; i < n; i++)
        if (dir[i] == '/') slash = dir + i;
    if (!slash || slash == dir) return false;
    *slash = 0;
    return true;
}

bool agentc_ext_dyn_path_ok(const char *path) {
    if (!path || !path[0]) return false;
    struct os_stat st;
    if (os_stat(path, &st) != 0) return false;
    if ((st.st_mode & DYN_S_IFMT) != DYN_S_IFREG) return false;
    if (!posix_host()) return true;   /* no group/other bits to check */
    if (st.st_mode & DYN_WRITE_GROUP_OTHER) return false;
    char dir[DYN_PATH_MAX];
    if (!parent_dir(path, dir, sizeof dir)) return false;
    struct os_stat ds;
    if (os_stat(dir, &ds) != 0) return false;
    if ((ds.st_mode & DYN_S_IFMT) != DYN_S_IFDIR) return false;
    if (ds.st_mode & DYN_WRITE_GROUP_OTHER) return false;
    return true;
}

/* -------------------------------------------------------------- file scan */

/* The registry's name rule (registry.c ext_valid_name), duplicated so the scan
 * never hands a candidate that registration would refuse. */
static bool dyn_stem_valid(const char *stem, size_t len) {
    if (len == 0 || len > 64) return false;
    for (size_t i = 0; i < len; i++) {
        char c = stem[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == ':' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static char dyn_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool suffix_matches(const char *name, size_t nlen, const char *suffix) {
    size_t slen = agentc_strlen(suffix);
    if (slen == 0 || nlen <= slen) return false;
    for (size_t i = 0; i < slen; i++) {
        if (dyn_lower(name[nlen - slen + i]) != dyn_lower(suffix[i])) return false;
    }
    return true;
}

/* The scan directory must hang off an absolute config home: a relative
 * XDG_CONFIG_HOME would resolve against the process CWD, turning the optional
 * scan into "load libraries from wherever agentc happens to run".
 * POSIX has one absolute form (/x); Windows also accepts drive roots (C:\x,
 * C:/x) and UNC (\\server). A bare `\x` is CWD-relative on POSIX. */
static bool dyn_home_absolute(const char *p) {
    if (!p || !p[0]) return false;
    if (p[0] == '/') return true;
    if (posix_host()) return false;
    if (p[0] == '\\') return true;
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
           p[1] == ':' && (p[2] == '/' || p[2] == '\\');
}

typedef struct {
    char *stem;
    char *path;
} DynCandidate;

static int dyn_scan_cb(void *ud, const char *name, const char *full, bool is_dir) {
    AgcVec *v = ud;
    if (is_dir) return 0;
    const char *suffix = g_loader->suffix ? g_loader->suffix() : NULL;
    if (!suffix || !suffix[0]) return 0;
    size_t nlen = agentc_strlen(name);
    if (!suffix_matches(name, nlen, suffix)) return 0;
    size_t stem_len = nlen - agentc_strlen(suffix);
    if (!dyn_stem_valid(name, stem_len)) return 0;
    if (!agentc_ext_dyn_path_ok(full)) return 0;
    DynCandidate *c = agentc_vec_push(v, sizeof *c);
    c->stem = agentc_strdup_len(name, stem_len);
    c->path = agentc_strdup(full);
    return 0;
}

static void dyn_candidates_free(AgcVec *v) {
    for (size_t i = 0; i < v->len; i++) {
        DynCandidate *c = &((DynCandidate *)v->p)[i];
        agentc_free(c->stem);
        agentc_free(c->path);
    }
    agentc_vec_free(v);
}

static void dyn_candidates_sort(DynCandidate *a, size_t n) {
    for (size_t i = 1; i < n; i++) {
        DynCandidate key = a[i];
        size_t j = i;
        while (j > 0) {
            size_t k = 0;
            while (a[j - 1].stem[k] && a[j - 1].stem[k] == key.stem[k]) k++;
            if ((u8)a[j - 1].stem[k] <= (u8)key.stem[k]) break;
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

size_t agentc_ext_dyn_scan(AgcExtDynScanFn cb, void *ud) {
    if (!cb || !g_loader || !g_loader->available || !g_loader->available())
        return 0;
    if (!g_loader->suffix || !g_loader->suffix() || !g_loader->suffix()[0])
        return 0;
    char home[DYN_PATH_MAX];
    if (!agentc_config_home(home, sizeof home)) return 0;
    if (!dyn_home_absolute(home)) {
        agentc_logf(2, "ext: dynamic scan skipped: config home is not absolute");
        return 0;
    }
    char dir[DYN_PATH_MAX];
    if (!agentc_path_join(dir, sizeof dir, home, "extensions")) return 0;

    AgcVec v = { 0 };
    int rc = agentc_dir_scan(dir, dyn_scan_cb, &v);
    if (rc < 0) {
        /* Missing directory (ENOENT/ENOTDIR) and permission refusals are fine:
         * dynamic loading is optional. */
        dyn_candidates_free(&v);
        return 0;
    }
    size_t total = v.len;
    if (total > AGENTC_EXT_MAX_DYNAMIC) {
        agentc_logf(2, "ext: dynamic extension cap reached, keeping %d of %llu candidate(s)",
                    AGENTC_EXT_MAX_DYNAMIC, (unsigned long long)total);
    }
    dyn_candidates_sort(v.p, v.len);
    size_t n = total > AGENTC_EXT_MAX_DYNAMIC ? AGENTC_EXT_MAX_DYNAMIC : total;
    for (size_t i = 0; i < n; i++) {
        DynCandidate *c = &((DynCandidate *)v.p)[i];
        cb(ud, c->stem, c->path);
    }
    dyn_candidates_free(&v);
    return n;
}
