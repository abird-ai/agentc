/* config.c — JSONC user/project config, project trust and shared file helpers.
 *
 * The config file is JSONC (see base/json.c). User values load first, project
 * values (trust-gated) overwrite them key by key. All strings stored in AgcConfig
 * are owned copies; agentc_config_load never returns NULL.
 *
 * trust.jsonc is a JSON array of {"path":…,"trusted":true|false}; resolution
 * walks the closest ancestor of cwd. Writes use temp+rename (atomic).
 *
 * This file also hosts the small path/file helpers that session.c, resources.c
 * and auth.c share; those translation units declare them extern.
 */
#include "agentc.h"
#include "agent.h"
#include "plat.h"
#include "wire.h"
#include "config.h"

typedef int (*AgcDirFn)(void *ud, const char *name, const char *full, bool is_dir);

#define PATH_MAX_ 4096

/* ---------------------------------------------------- internal environment */
/* Tests cannot call setenv (no libc); agentc_test_setenv records overrides that
 * agentc_env_get consults before the process environment. Production reads the
 * process environment unchanged. */
typedef struct {
    char *name;
    char *value;
} EnvOverride;

static EnvOverride env_over[8];

const char *agentc_env_get(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof env_over / sizeof env_over[0]; i++)
        if (env_over[i].name && agentc_streq(env_over[i].name, name)) return env_over[i].value;
    return os_getenv(name);
}

void agentc_test_setenv(const char *name, const char *value) {
    if (!name || !name[0]) return;
    for (size_t i = 0; i < sizeof env_over / sizeof env_over[0]; i++) {
        if (env_over[i].name && agentc_streq(env_over[i].name, name)) {
            agentc_free(env_over[i].value);
            env_over[i].value = value ? agentc_strdup(value) : NULL;
            if (!value) {
                agentc_free(env_over[i].name);
                env_over[i].name = NULL;
            }
            return;
        }
    }
    if (!value) return;
    for (size_t i = 0; i < sizeof env_over / sizeof env_over[0]; i++) {
        if (!env_over[i].name) {
            env_over[i].name = agentc_strdup(name);
            env_over[i].value = agentc_strdup(value);
            return;
        }
    }
}

void agentc_test_clearenv(void) {
    for (size_t i = 0; i < sizeof env_over / sizeof env_over[0]; i++) {
        agentc_free(env_over[i].name);
        agentc_free(env_over[i].value);
        env_over[i].name = env_over[i].value = NULL;
    }
}

/* ------------------------------------------------------------ path helpers */

bool agentc_path_join(char *out, size_t cap, const char *dir, const char *name) {
    if (!out || cap == 0) return false;
    if (!dir) dir = "";
    if (!name) name = "";
    size_t dl = agentc_strlen(dir);
    if (dl >= cap) return false;
    agentc_memcpy(out, dir, dl);
    size_t n = dl;
    if (n > 0 && out[n - 1] != '/') {
        if (n + 1 >= cap) return false;
        out[n++] = '/';
    }
    size_t nl = agentc_strlen(name);
    if (n + nl >= cap) return false;
    agentc_memcpy(out + n, name, nl);
    out[n + nl] = 0;
    return true;
}

/* os_stat/os_fstat fill the named fields declared in plat.h. */
static u32 stat_mode(const struct os_stat *st) { return st->st_mode; }

static bool stat_is(const char *path, u32 ifmt) {
    struct os_stat st;
    if (os_stat(path, &st) < 0) return false;
    return (stat_mode(&st) & 0170000u) == ifmt;
}

bool agentc_path_is_dir(const char *path) { return stat_is(path, 0040000u); }
bool agentc_path_is_file(const char *path) { return stat_is(path, 0100000u); }

int agentc_mkdir_parents(const char *path) {
    char buf[PATH_MAX_];
    size_t n = agentc_strlen(path);
    if (n >= sizeof buf) return -36; /* ENAMETOOLONG */
    agentc_memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        int r = os_mkdir(buf, 0755);
        if (r < 0 && r != -17 /* EEXIST */) return r;
        buf[i] = '/';
    }
    return 0;
}

/* Whole-file reader used by config, sessions, auth, resources, mcp and the
 * read-only tools. Capped: a hostile path (/dev/zero, /proc/kcore, a 20 GB
 * file) must fail, not exhaust the process. */
#define AGENTC_READ_FILE_MAX (16u << 20)

char *agentc_read_file_owned(const char *path, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!path) return NULL;
    int fd = os_open(path, OS_O_RDONLY, 0);
    if (fd < 0) return NULL;
    struct os_stat st;
    if (os_fstat(fd, &st) == 0) {
        i64 size = st.st_size;
        if (size > (i64)AGENTC_READ_FILE_MAX) {
            os_close(fd);
            return NULL;
        }
    }
    AgcBuf b = { 0 };
    char tmp[65536];
    for (;;) {
        if (b.len > AGENTC_READ_FILE_MAX) {
            os_close(fd);
            agentc_buf_free(&b);
            return NULL;
        }
        int n = os_read(fd, tmp, sizeof tmp);
        if (n < 0) {
            os_close(fd);
            agentc_buf_free(&b);
            return NULL;
        }
        if (n == 0) break;
        agentc_buf_push(&b, tmp, (size_t)n);
    }
    os_close(fd);
    if (!b.p) b.p = (u8 *)agentc_strdup_len("", 0);
    if (out_len) *out_len = b.len;
    return (char *)b.p;
}

static int write_all(int fd, const void *p, size_t n) {
    const u8 *q = p;
    while (n) {
        int w = os_write(fd, q, n);
        if (w < 0) {
            if (w == -4 /* EINTR */) continue;
            return w;
        }
        if (w == 0) return -5;
        q += w;
        n -= (size_t)w;
    }
    return 0;
}

int agentc_write_file_atomic(const char *path, const void *data, size_t len, int mode) {
    int mr = agentc_mkdir_parents(path);
    if (mr < 0) return mr;
    u64 rnd = 0;
    (void)os_random(&rnd, sizeof rnd);
    i64 now = os_now_ns(OS_CLOCK_MONOTONIC);
    char tmp[PATH_MAX_ + 64];
    int n = agentc_snprintf(tmp, sizeof tmp, "%s.agentc-%llx%llx.tmp", path,
                        (unsigned long long)now, (unsigned long long)rnd);
    if (n <= 0 || (size_t)n >= sizeof tmp) return -36;
    int fd = os_open(tmp, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, mode);
    if (fd < 0) return fd;
    int w = write_all(fd, data, len);
    int cr = os_close(fd);
    if (w < 0 || cr < 0) {
        os_unlink(tmp);
        return w < 0 ? w : cr;
    }
    int rr = os_rename(tmp, path);
    if (rr < 0) {
        os_unlink(tmp);
        return rr;
    }
    return 0;
}

/* linux dirent64 */
typedef struct {
    u64 ino;
    i64 off;
    u16 reclen;
    u8 type;
    char name[];
} AgcDirent;

#define AGENTC_DT_UNKNOWN 0
#define AGENTC_DT_DIR 4

/* Directory entries are buffered and sorted by name before the callback runs.
 * os_getdents order is filesystem-dependent, and the callers (extension scans,
 * resource loaders and their golden tests) need stable, reproducible output. */
typedef struct {
    char *name;
    bool is_dir;
} AgcDirItem;

static int dir_item_name_cmp(const AgcDirItem *a, const AgcDirItem *b) {
    const u8 *x = (const u8 *)a->name;
    const u8 *y = (const u8 *)b->name;
    while (*x && *x == *y) { x++; y++; }
    return (int)*x - (int)*y;
}

int agentc_dir_scan(const char *dir, AgcDirFn cb, void *ud) {
    int fd = os_open(dir, OS_O_RDONLY | OS_O_DIRECTORY | OS_O_CLOEXEC, 0);
    if (fd < 0) return fd;
    AgcDirItem *items = NULL;
    size_t len = 0, cap = 0;
    u8 buf[16384];
    int n = 0;
    int rc = 0;
    while ((n = os_getdents(fd, buf, sizeof buf)) > 0) {
        bool stop = false;
        for (size_t off = 0; off < (size_t)n;) {
            AgcDirent *d = (AgcDirent *)(buf + off);
            if (d->reclen == 0) break;
            off += d->reclen;
            if (agentc_streq(d->name, ".") || agentc_streq(d->name, "..")) continue;
            char full[PATH_MAX_];
            if (!agentc_path_join(full, sizeof full, dir, d->name)) continue;
            if (len == cap) {
                size_t ncap = cap ? cap * 2 : 32;
                AgcDirItem *grown = agentc_realloc(items, ncap * sizeof *grown);
                if (!grown) { rc = -12; stop = true; break; } /* ENOMEM */
                items = grown;
                cap = ncap;
            }
            items[len].name = agentc_strdup(d->name);
            if (!items[len].name) { rc = -12; stop = true; break; } /* ENOMEM */
            bool is_dir = d->type == AGENTC_DT_DIR;
            if (d->type == AGENTC_DT_UNKNOWN) is_dir = agentc_path_is_dir(full);
            items[len].is_dir = is_dir;
            len++;
        }
        if (stop) break;
    }
    if (n < 0) rc = n;
    if (rc == 0) {
        for (size_t i = 1; i < len; i++) {
            AgcDirItem key = items[i];
            size_t j = i;
            while (j > 0 && dir_item_name_cmp(&items[j - 1], &key) > 0) {
                items[j] = items[j - 1];
                j--;
            }
            items[j] = key;
        }
        for (size_t i = 0; i < len; i++) {
            char full[PATH_MAX_];
            if (!agentc_path_join(full, sizeof full, dir, items[i].name)) continue;
            if (cb(ud, items[i].name, full, items[i].is_dir)) { rc = 1; break; }
        }
    }
    for (size_t i = 0; i < len; i++) agentc_free(items[i].name);
    agentc_free(items);
    os_close(fd);
    return rc;
}

/* An entry may be a symlink to a directory. agentc_dir_scan classifies entries
 * from the dirent type (DT_LNK is not DT_DIR), so `is_dir` is false for a
 * symlink. Carrying that classification into the recursion keeps rm_rf from
 * descending into the link target; re-testing with agentc_path_is_dir() would
 * follow the link and delete the target's contents. */
typedef struct {
    char *path;
    bool is_dir;
} RmEnt;

static int rm_collect_cb(void *ud, const char *name, const char *full, bool is_dir) {
    (void)name;
    AgcVec *v = ud;
    RmEnt *e = agentc_vec_push(v, sizeof *e);
    e->path = agentc_strdup(full);
    e->is_dir = is_dir;
    return 0;
}

static void rm_rf_entry(const char *path, bool is_dir) {
    if (!is_dir) {
        (void)os_unlink(path);
        return;
    }
    AgcVec v = { 0 };
    if (agentc_dir_scan(path, rm_collect_cb, &v) >= 0) {
        RmEnt *ents = (RmEnt *)v.p;
        for (size_t i = 0; i < v.len; i++) rm_rf_entry(ents[i].path, ents[i].is_dir);
        for (size_t i = 0; i < v.len; i++) agentc_free(ents[i].path);
    }
    agentc_vec_free(&v);
    (void)os_unlink(path);
}

void agentc_rm_rf(const char *path) {
    if (!path || !path[0]) return;
    rm_rf_entry(path, agentc_path_is_dir(path));
}

static int str_cmp(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}

/* Insertion sort of owned C strings. */
void agentc_sort_strings(char **a, size_t n, bool descending) {
    for (size_t i = 1; i < n; i++) {
        char *key = a[i];
        size_t j = i;
        while (j > 0) {
            int c = str_cmp(a[j - 1], key);
            bool stop = descending ? c >= 0 : c <= 0;
            if (stop) break;
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

/* Write the first-run defaults (agentc setup / onboarding). The file is ours, so a
 * full rewrite is fine; it is read again by agentc_config_load on the next start. */
int agentc_config_save_setup(const char *provider, const char *model) {
    char path[PATH_MAX_ + 64];
    if (!agentc_config_setup_path(path, sizeof path)) return -2;
    (void)agentc_mkdir_parents(path);
    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "default_provider");
    agentc_jsonw_cstr(&w, provider ? provider : "");
    agentc_jsonw_key(&w, "default_model");
    agentc_jsonw_cstr(&w, model ? model : "");
    agentc_jsonw_end(&w);
    agentc_buf_byte(&b, '\n');
    int rc = agentc_write_file_atomic(path, b.p, b.len, 0644);
    agentc_buf_free(&b);
    return rc;
}

int agentc_config_setup_set_default(const char *provider, const char *model) {
    char base[PATH_MAX_];
    char path[PATH_MAX_ + 64];
    const char *ch = agentc_config_home(base, sizeof base);
    if (!ch) return -2;

    /* config.jsonc is loaded after setup.jsonc and wins, so if it pins
     * default_provider a setup.jsonc write would be invisible. Report that
     * instead of pretending the switch happened (and do not rewrite the user's
     * hand-formatted config.jsonc, which may carry comments). */
    if (agentc_path_join(path, sizeof path, ch, "config.jsonc")) {
        size_t clen = 0;
        char *ctext = agentc_read_file_owned(path, &clen);
        if (ctext && clen) {
            AgcJsonArena *cja = agentc_json_arena_new(0);
            AgcJson *croot = agentc_json_parse_in(cja, ctext, clen);
            bool pinned = agentc_json_type(croot) == AGENTC_JSON_OBJ &&
                          agentc_json_get(croot, "default_provider") != NULL;
            agentc_json_arena_free(cja);
            agentc_free(ctext);
            if (pinned) return 1;
        }
    }

    if (!agentc_config_setup_path(path, sizeof path)) return -2;
    (void)agentc_mkdir_parents(path);

    /* Merge-preserve: setup.jsonc is normally two keys, but a user who edited it
     * must not lose the rest when login updates the default. */
    AgcJsonArena *ja = agentc_json_arena_new(0);
    size_t olen = 0;
    char *old = agentc_read_file_owned(path, &olen);
    AgcJson *root = (old && olen) ? agentc_json_parse_in(ja, old, olen) : NULL;

    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    if (agentc_json_type(root) == AGENTC_JSON_OBJ) {
        for (size_t i = 0; i < (size_t)-1; i++) {
            const AgcJson *k = agentc_json_key_at(root, i);
            if (!k) break;
            const char *key = agentc_json_str(k, NULL);
            if (!key) continue;
            if (agentc_streq(key, "default_provider") || agentc_streq(key, "default_model"))
                continue;
            agentc_jsonw_key(&w, key);
            agentc_json_emit(&w, agentc_json_val_at(root, i));
        }
    }
    agentc_jsonw_key(&w, "default_provider");
    agentc_jsonw_cstr(&w, provider ? provider : "");
    agentc_jsonw_key(&w, "default_model");
    agentc_jsonw_cstr(&w, model ? model : "");
    agentc_jsonw_end(&w);
    agentc_buf_byte(&b, '\n');

    int rc = agentc_write_file_atomic(path, b.p, b.len, 0644);
    agentc_buf_free(&b);
    agentc_free(old);
    agentc_json_arena_free(ja);
    return rc;
}

const char *agentc_config_setup_path(char *buf, size_t cap) {
    char base[PATH_MAX_];
    if (!agentc_config_home(base, sizeof base)) return NULL;
    return agentc_path_join(buf, cap, base, "setup.jsonc") ? buf : NULL;
}

/* ------------------------------------------------------- config locations */
/* agentc_config_home -> $XDG_CONFIG_HOME/agentc or $HOME/.config/agentc
 * agentc_data_home   -> $XDG_DATA_HOME/agentc   or $HOME/.local/share/agentc */
const char *agentc_config_home(char *buf, size_t cap) {
    const char *x = agentc_env_get("XDG_CONFIG_HOME");
    if (x && x[0]) {
        int n = agentc_snprintf(buf, cap, "%s/agentc", x);
        return (n > 0 && (size_t)n < cap) ? buf : NULL;
    }
    const char *h = agentc_env_get("HOME");
    if (!h || !h[0]) return NULL;
    int n = agentc_snprintf(buf, cap, "%s/.config/agentc", h);
    return (n > 0 && (size_t)n < cap) ? buf : NULL;
}

const char *agentc_data_home(char *buf, size_t cap) {
    const char *x = agentc_env_get("XDG_DATA_HOME");
    if (x && x[0]) {
        int n = agentc_snprintf(buf, cap, "%s/agentc", x);
        return (n > 0 && (size_t)n < cap) ? buf : NULL;
    }
    const char *h = agentc_env_get("HOME");
    if (!h || !h[0]) return NULL;
    int n = agentc_snprintf(buf, cap, "%s/.local/share/agentc", h);
    return (n > 0 && (size_t)n < cap) ? buf : NULL;
}

/* ---------------------------------------------------------------- config */

static int g_cli_trust = -1;      /* --approve / --no-approve, set by main.c */
static bool g_default_trusted;    /* config default_trusted, user config only */

void agentc_config_set_cli_trust(int v) { g_cli_trust = v; }
bool agentc_config_default_trusted(void) { return g_default_trusted; }

static char *dup_if(const char *s) { return s ? agentc_strdup(s) : NULL; }

static void set_str(char **dst, const char *v) {
    if (!v) return;
    agentc_free(*dst);
    *dst = agentc_strdup(v);
}

static void set_tools(AgcConfig *c, const AgcJson *arr) {
    if (agentc_json_type(arr) != AGENTC_JSON_ARR) return;
    size_t n = agentc_json_len(arr);
    char **list = agentc_alloc(n * sizeof(char *));
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char *s = agentc_json_str(agentc_json_at(arr, i), NULL);
        if (s && s[0]) list[k++] = agentc_strdup(s);
    }
    for (size_t i = 0; i < c->ndefault_tools; i++) agentc_free(c->default_tools[i]);
    agentc_free(c->default_tools);
    c->default_tools = list;
    c->ndefault_tools = k;
}

static void set_ext_disabled(AgcConfig *c, const AgcJson *arr) {
    if (agentc_json_type(arr) != AGENTC_JSON_ARR) return;
    size_t n = agentc_json_len(arr);
    char **list = agentc_alloc((n ? n : 1) * sizeof(char *));
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char *s = agentc_json_str(agentc_json_at(arr, i), NULL);
        if (s && s[0]) list[k++] = agentc_strdup(s);
    }
    for (size_t i = 0; i < c->nextensions_disabled; i++)
        agentc_free(c->extensions_disabled[i]);
    agentc_free(c->extensions_disabled);
    c->extensions_disabled = list;
    c->nextensions_disabled = k;
}

/* Upsert one generic providers.<id>/api_keys.<id> entry. A later config file
 * (or a later apply_config pass) replaces the value of an existing id; new
 * ids are appended until the cap. An id over the name maximum contributes
 * nothing, matching the named fields' bounded storage. */
static void set_generic_entry(AgcConfigEntry **arr, size_t *n, const char *id,
                              const char *value) {
    if (!id || !id[0] || agentc_strlen(id) > AGENTC_CONFIG_GENERIC_NAME_MAX) return;
    for (size_t i = 0; i < *n; i++) {
        if (!agentc_streq((*arr)[i].id, id)) continue;
        agentc_free((*arr)[i].value);
        (*arr)[i].value = value ? agentc_strdup(value) : NULL;
        return;
    }
    if (*n >= AGENTC_CONFIG_GENERIC_MAX) return;
    *arr = agentc_realloc(*arr, (*n + 1) * sizeof **arr);
    (*arr)[*n].id = agentc_strdup(id);
    (*arr)[*n].value = value ? agentc_strdup(value) : NULL;
    (*n)++;
}

/* Copy every <id>: {base_url: …} entry of a `providers` object. The four named
 * presets are stored in their dedicated fields as well; the getters consult
 * those first, so a generic duplicate is harmless and never changes meaning. */
static void set_generic_providers(AgcConfig *c, const AgcJson *obj) {
    for (size_t i = 0;; i++) {
        const AgcJson *k = agentc_json_key_at(obj, i);
        if (!k) break;
        size_t klen = 0;
        const char *id = agentc_json_str(k, &klen);
        if (!id || !id[0] || klen > AGENTC_CONFIG_GENERIC_NAME_MAX) continue;
        const char *base = agentc_json_get_str(agentc_json_val_at(obj, i), "base_url");
        if (!base) continue;   /* no endpoint contribution */
        set_generic_entry(&c->providers, &c->nproviders, id, base);
    }
}

/* Copy every <id>: "key" entry of an `api_keys` object. */
static void set_generic_api_keys(AgcConfig *c, const AgcJson *obj) {
    for (size_t i = 0;; i++) {
        const AgcJson *k = agentc_json_key_at(obj, i);
        if (!k) break;
        size_t klen = 0;
        const char *id = agentc_json_str(k, &klen);
        if (!id || !id[0] || klen > AGENTC_CONFIG_GENERIC_NAME_MAX) continue;
        const char *key = agentc_json_str(agentc_json_val_at(obj, i), NULL);
        if (!key) continue;   /* nested objects (e.g. oauth) are not keys */
        set_generic_entry(&c->api_keys, &c->napi_keys, id, key);
    }
}

static void apply_config(AgcConfig *c, const AgcJson *root, bool user_scope) {
    if (agentc_json_type(root) != AGENTC_JSON_OBJ) return;

    set_str(&c->default_provider, agentc_json_get_str(root, "default_provider"));
    set_str(&c->default_model, agentc_json_get_str(root, "default_model"));
    set_str(&c->default_thinking, agentc_json_get_str(root, "default_thinking"));
    set_str(&c->theme, agentc_json_get_str(root, "theme"));
    set_str(&c->session_dir, agentc_json_get_str(root, "session_dir"));
    set_str(&c->shell, agentc_json_get_str(root, "shell"));
    set_str(&c->tui_mode, agentc_json_get_str(root, "tui_mode"));
    set_str(&c->openai_client_version, agentc_json_get_str(root, "openai_client_version"));

    AgcJson *tools = agentc_json_get(root, "default_tools");
    if (tools) set_tools(c, tools);

    AgcJson *ext = agentc_json_get(root, "extensions");
    if (agentc_json_type(ext) == AGENTC_JSON_OBJ)
        set_ext_disabled(c, agentc_json_get(ext, "disabled"));

    AgcJson *tobj = agentc_json_get(root, "tools");
    if (agentc_json_type(tobj) == AGENTC_JSON_OBJ)
        set_str(&c->tools_engine, agentc_json_get_str(tobj, "engine"));

    AgcJson *mt = agentc_json_get(root, "max_tokens");
    if (agentc_json_type(mt) == AGENTC_JSON_NUM)
        c->max_tokens = agentc_json_get_int(root, "max_tokens", c->max_tokens);

    i64 attempts = agentc_json_get_int(root, "max_attempts", c->max_attempts);
    AgcJson *retry = agentc_json_get(root, "retry");
    if (agentc_json_type(retry) == AGENTC_JSON_OBJ)
        attempts = agentc_json_get_int(retry, "max_attempts", attempts);
    if (attempts > 0) c->max_attempts = (int)attempts;

    /* SECURITY: endpoint overrides and API keys are user-scope only. A cloned
     * project config that changed base_url could otherwise send the user's
     * subscription token or API key to an attacker-controlled server on the
     * first request. Project files may still set models, tools and paths. */
    if (user_scope) {
        set_str(&c->base_url_anthropic, agentc_json_get_str(root, "base_url_anthropic"));
        set_str(&c->base_url_openai, agentc_json_get_str(root, "base_url_openai"));
        AgcJson *providers = agentc_json_get(root, "providers");
        if (agentc_json_type(providers) == AGENTC_JSON_OBJ) {
            AgcJson *a = agentc_json_get(providers, "anthropic");
            AgcJson *o = agentc_json_get(providers, "openai");
            AgcJson *ol = agentc_json_get(providers, "ollama");
            AgcJson *olc = agentc_json_get(providers, "ollama-cloud");
            set_str(&c->base_url_anthropic, agentc_json_get_str(a, "base_url"));
            set_str(&c->base_url_openai, agentc_json_get_str(o, "base_url"));
            set_str(&c->base_url_ollama, agentc_json_get_str(ol, "base_url"));
            set_str(&c->base_url_ollama_cloud, agentc_json_get_str(olc, "base_url"));
            set_generic_providers(c, providers);
        }

        AgcJson *keys = agentc_json_get(root, "api_keys");
        if (agentc_json_type(keys) == AGENTC_JSON_OBJ) {
            set_str(&c->api_key_anthropic, agentc_json_get_str(keys, "anthropic"));
            set_str(&c->api_key_openai, agentc_json_get_str(keys, "openai"));
            set_generic_api_keys(c, keys);
        }
    } else if (agentc_json_get(root, "providers") ||
               agentc_json_get(root, "base_url_anthropic") ||
               agentc_json_get(root, "base_url_openai") || agentc_json_get(root, "api_keys")) {
        agentc_logf(2, "warning: ignoring endpoint overrides in a project config");
    }

    if (user_scope) {
        AgcJson *dt = agentc_json_get(root, "default_trusted");
        if (agentc_json_type(dt) == AGENTC_JSON_TRUE) g_default_trusted = true;
        else if (agentc_json_type(dt) == AGENTC_JSON_FALSE) g_default_trusted = false;
    }
}

/* Parse `path` (JSONC) in `ja` and hand back the source text to keep alive
 * until the caller has copied what it needs. Returns NULL when absent or
 * malformed. */
static AgcJson *parse_config_file(AgcJsonArena *ja, const char *path, char **text_out) {
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text) return NULL;
    AgcJson *root = agentc_json_parse_in(ja, text, len);
    if (!root) {
        agentc_logf(2, "warning: ignoring malformed config: %s", path);
        agentc_free(text);
        return NULL;
    }
    *text_out = text;
    return root;
}

AgcConfig *agentc_config_load(const char *cwd) {
    AgcConfig *c = agentc_alloc(sizeof *c);
    c->default_provider = dup_if("openai");
    c->default_model = dup_if("gpt-5");
    /* reasoning defaults to medium; config/--thinking override it */
    c->default_thinking = dup_if("medium");
    c->theme = dup_if("system");
    c->max_attempts = 5;
    c->max_tokens = 0;
    c->shell = dup_if("auto");
    c->tui_mode = dup_if("auto");
    c->tools_engine = dup_if("external");
    g_default_trusted = false;

    char base[PATH_MAX_];
    char path[PATH_MAX_ + 64];
    const char *ch = agentc_config_home(base, sizeof base);
    /* setup.jsonc (first-run defaults) loads first so config.jsonc wins */
    if (ch && agentc_path_join(path, sizeof path, ch, "setup.jsonc")) {
        AgcJsonArena *ja = agentc_json_arena_new(0);
        char *text = NULL;
        AgcJson *root = parse_config_file(ja, path, &text);
        if (root) {
            apply_config(c, root, true);
            agentc_free(text);
        }
        agentc_json_arena_free(ja);
    }
    if (ch && agentc_path_join(path, sizeof path, ch, "config.jsonc")) {
        AgcJsonArena *ja = agentc_json_arena_new(0);
        char *text = NULL;
        AgcJson *root = parse_config_file(ja, path, &text);
        if (root) {
            apply_config(c, root, true);
            agentc_free(text);
        }
        agentc_json_arena_free(ja);
    }

    if (cwd && cwd[0] && ch) {
        bool trusted = agentc_trust_resolve(cwd, g_cli_trust, g_default_trusted);
        char proj[PATH_MAX_ + 64];
        if (trusted && agentc_path_join(proj, sizeof proj, cwd, ".agentc/config.jsonc")) {
            AgcJsonArena *ja = agentc_json_arena_new(0);
            char *text = NULL;
            AgcJson *root = parse_config_file(ja, proj, &text);
            if (root) {
                apply_config(c, root, false);
                agentc_free(text);
            }
            agentc_json_arena_free(ja);
        }
    }
    /* The environment override is resolved after the files so a one-off
     * `AGENTC_SHELL=pwsh agentc` does not have to edit config.jsonc. */
    const char *env_shell = agentc_env_get("AGENTC_SHELL");
    agentc_config_set_shell(env_shell && env_shell[0] ? env_shell : c->shell);
    const char *env_engine = agentc_env_get("AGENTC_TOOLS_ENGINE");
    agentc_config_set_tools_engine(env_engine && env_engine[0] ? env_engine : c->tools_engine);
    return c;
}

void agentc_config_free(AgcConfig *c) {
    if (!c) return;
    agentc_free(c->default_provider);
    agentc_free(c->default_model);
    agentc_free(c->default_thinking);
    for (size_t i = 0; i < c->ndefault_tools; i++) agentc_free(c->default_tools[i]);
    agentc_free(c->default_tools);
    agentc_free(c->theme);
    agentc_free(c->base_url_anthropic);
    agentc_free(c->base_url_openai);
    agentc_free(c->base_url_ollama);
    agentc_free(c->base_url_ollama_cloud);
    agentc_free(c->api_key_anthropic);
    agentc_free(c->api_key_openai);
    agentc_free(c->session_dir);
    agentc_free(c->shell);
    agentc_free(c->tui_mode);
    agentc_free(c->tools_engine);
    agentc_free(c->openai_client_version);
    for (size_t i = 0; i < c->nextensions_disabled; i++)
        agentc_free(c->extensions_disabled[i]);
    agentc_free(c->extensions_disabled);
    for (size_t i = 0; i < c->nproviders; i++) {
        agentc_free(c->providers[i].id);
        agentc_free(c->providers[i].value);
    }
    agentc_free(c->providers);
    for (size_t i = 0; i < c->napi_keys; i++) {
        agentc_free(c->api_keys[i].id);
        agentc_free(c->api_keys[i].value);
    }
    agentc_free(c->api_keys);
    agentc_free(c);
}

/* ------------------------------------------------------------- shell kind */
/* The bash tool also runs from contexts that never see the AgcConfig (RPC
 * mode `/bash`, tests), so the effective kind lives in one global published
 * by agentc_config_load. NULL and "auto" mean the platform default. */
static char *g_shell;

const char *agentc_config_shell(void) { return g_shell != NULL ? g_shell : "auto"; }

void agentc_config_set_shell(const char *kind) {
    agentc_free(g_shell);
    g_shell = kind != NULL && kind[0] != 0 ? agentc_strdup(kind) : NULL;
}

/* ----------------------------------------------------------- tools engine */
/* Same shape as the shell kind: the effective engine lives in one process-wide
 * global so the builtin-tools extension (and tests/RPC paths that never hold an
 * AgcConfig) can read it. "external" until a config/env/flag value applies. */
static char *g_tools_engine;

const char *agentc_config_tools_engine(void) {
    return g_tools_engine != NULL ? g_tools_engine : "external";
}

void agentc_config_set_tools_engine(const char *value) {
    /* Copy first: `value` may alias the current g_tools_engine. */
    char *copy = value != NULL && value[0] != 0 ? agentc_strdup(value) : NULL;
    agentc_free(g_tools_engine);
    g_tools_engine = copy;
}

static const char *generic_value(const AgcConfigEntry *arr, size_t n, const char *id) {
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(arr[i].id, id)) return arr[i].value;
    return NULL;
}

const char *agentc_config_base_url(const AgcConfig *c, const char *provider) {
    if (!c || !provider) return NULL;
    if (agentc_streq(provider, "anthropic")) return c->base_url_anthropic;
    if (agentc_streq(provider, "openai")) return c->base_url_openai;
    if (agentc_streq(provider, "ollama")) return c->base_url_ollama;
    if (agentc_streq(provider, "ollama-cloud")) return c->base_url_ollama_cloud;
    return generic_value(c->providers, c->nproviders, provider);
}

const char *agentc_config_api_key(const AgcConfig *c, const char *provider) {
    if (!c || !provider) return NULL;
    if (agentc_streq(provider, "anthropic")) return c->api_key_anthropic;
    if (agentc_streq(provider, "openai")) return c->api_key_openai;
    return generic_value(c->api_keys, c->napi_keys, provider);
}

/* ----------------------------------------------------------------- pricing */

/* Read a JSON scalar (string or number) text into a NUL-terminated buffer. */
static bool pricing_scalar_text(const AgcJson *v, char *buf, size_t cap) {
    if (!v) return false;
    size_t n = 0;
    const char *s = agentc_json_str(v, &n);
    if (!s) s = agentc_json_num(v, &n);
    if (!s || n == 0 || n + 1 > cap) return false;
    agentc_memcpy(buf, s, n);
    buf[n] = 0;
    return true;
}

/* One rate field, defaulting to 0 when absent. Returns -1 when present but
 * malformed. */
static i64 pricing_rate(const AgcJson *v, const char *key) {
    const AgcJson *f = agentc_json_get(v, key);
    if (!f) return 0;
    char buf[64];
    if (!pricing_scalar_text(f, buf, sizeof buf)) return -1;
    return agentc_rate_parse_scaled(buf, 1000000);   /* USD/M -> micro/M */
}

int agentc_pricing_load(char *err, size_t cap) {
    if (err && cap) err[0] = 0;
    char base[PATH_MAX_];
    const char *ch = agentc_config_home(base, sizeof base);
    if (!ch) return 0;
    char path[PATH_MAX_ + 64];
    if (!agentc_path_join(path, sizeof path, ch, "pricing.jsonc")) return -22;
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text) return 0;   /* absent is not an error */

    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(ja, text, len);
    agentc_free(text);
    if (agentc_json_type(root) != AGENTC_JSON_OBJ) {
        agentc_json_arena_free(ja);
        if (err && cap) agentc_snprintf(err, cap, "malformed pricing.jsonc");
        return -22;
    }
    int applied = 0;
    for (size_t i = 0;; i++) {
        const AgcJson *k = agentc_json_key_at(root, i);
        if (!k) break;
        size_t klen = 0;
        const char *key = agentc_json_str(k, &klen);
        if (!key || !key[0] || klen >= 256) continue;
        const AgcJson *v = agentc_json_val_at(root, i);
        if (agentc_json_type(v) != AGENTC_JSON_OBJ) continue;
        if (!agentc_json_get(v, "in")) continue;   /* "in" is required */
        i64 in_rate = pricing_rate(v, "in");
        if (in_rate < 0) continue;
        i64 out_rate = pricing_rate(v, "out");
        i64 cr_rate = pricing_rate(v, "cache_read");
        i64 cw_rate = pricing_rate(v, "cache_write");
        if (out_rate < 0 || cr_rate < 0 || cw_rate < 0) continue;
        u32 flags = AGENTC_MODEL_RATE_KNOWN;
        if (agentc_json_get_bool(v, "input_includes_cache", false))
            flags |= AGENTC_MODEL_INPUT_INCLUDES_CACHE;
        /* provider-qualified key first, then id-only */
        char keybuf[256];
        agentc_memcpy(keybuf, key, klen + 1);
        const char *slash = NULL;
        for (const char *p = keybuf; *p; p++)
            if (*p == '/') { slash = p; break; }
        const char *provider = NULL;
        const char *id = keybuf;
        if (slash) {
            keybuf[slash - keybuf] = 0;
            provider = keybuf;
            id = slash + 1;
        }
        if (!id[0]) continue;
        agentc_model_set_rate_override(provider, id, in_rate, out_rate, cr_rate, cw_rate, flags);
        applied++;
    }
    agentc_json_arena_free(ja);
    return applied;
}

/* ----------------------------------------------------------------- trust */

static const char *trust_path(char *buf, size_t cap) {
    char base[PATH_MAX_];
    const char *ch = agentc_config_home(base, sizeof base);
    if (!ch) return NULL;
    if (!agentc_path_join(buf, cap, ch, "trust.jsonc")) return NULL;
    return buf;
}

static size_t trim_slashes(const char *s) {
    size_t n = agentc_strlen(s);
    while (n > 1 && s[n - 1] == '/') n--;
    return n;
}

/* is `anc` the same path as or an ancestor of `path`? */
static bool path_is_ancestor(const char *anc, const char *path) {
    if (!anc || !anc[0]) return false;   /* "" would otherwise match every path */
    size_t al = trim_slashes(anc);
    size_t pl = trim_slashes(path);
    if (al > pl) return false;
    if (!agentc_str_eq(anc, al, path, al)) return false;
    if (al == pl) return true;
    if (al == 1 && anc[0] == '/') return true;
    return path[al] == '/';
}

bool agentc_trust_resolve(const char *cwd, int cli_verdict, bool default_trusted) {
    if (cli_verdict == 1) return true;
    if (cli_verdict == 0) return false;
    if (!cwd || !cwd[0]) return default_trusted;

    char path[PATH_MAX_ + 64];
    if (!trust_path(path, sizeof path)) return default_trusted;
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text) return default_trusted;

    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(ja, text, len);
    bool result = default_trusted;
    bool found = false;
    size_t best = 0;
    if (agentc_json_type(root) == AGENTC_JSON_ARR) {
        for (size_t i = 0; i < agentc_json_len(root); i++) {
            AgcJson *e = agentc_json_at(root, i);
            const char *p = agentc_json_get_str(e, "path");
            if (!p || !p[0] || !path_is_ancestor(p, cwd)) continue;
            size_t plen = trim_slashes(p);
            if (!found || plen > best) {
                found = true;
                best = plen;
                result = agentc_json_get_bool(e, "trusted", false);
            }
        }
    }
    agentc_free(text);
    agentc_json_arena_free(ja);
    return result;
}

void agentc_trust_save(const char *cwd, bool trusted) {
    if (!cwd || !cwd[0]) return;
    char path[PATH_MAX_ + 64];
    if (!trust_path(path, sizeof path)) return;

    /* Copy existing entries out of the JSON arena before writing. */
    AgcVec paths = { 0 };
    AgcVec verdicts = { 0 };
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (text) {
        AgcJsonArena *ja = agentc_json_arena_new(0);
        AgcJson *root = agentc_json_parse_in(ja, text, len);
        if (agentc_json_type(root) == AGENTC_JSON_ARR) {
            for (size_t i = 0; i < agentc_json_len(root); i++) {
                AgcJson *e = agentc_json_at(root, i);
                const char *p = agentc_json_get_str(e, "path");
                if (!p || !p[0]) continue;
                *(char **)agentc_vec_push(&paths, sizeof(char *)) = agentc_strdup(p);
                *(bool *)agentc_vec_push(&verdicts, sizeof(bool)) =
                    agentc_json_get_bool(e, "trusted", false);
            }
        }
        agentc_free(text);
        agentc_json_arena_free(ja);
    }

    bool replaced = false;
    for (size_t i = 0; i < paths.len; i++) {
        if (agentc_streq(((char **)paths.p)[i], cwd)) {
            ((bool *)verdicts.p)[i] = trusted;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        *(char **)agentc_vec_push(&paths, sizeof(char *)) = agentc_strdup(cwd);
        *(bool *)agentc_vec_push(&verdicts, sizeof(bool)) = trusted;
    }

    AgcBuf out = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &out);
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < paths.len; i++) {
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "path");
        agentc_jsonw_cstr(&w, ((char **)paths.p)[i]);
        agentc_jsonw_key(&w, "trusted");
        agentc_jsonw_bool(&w, ((bool *)verdicts.p)[i]);
        agentc_jsonw_end(&w);
    }
    agentc_jsonw_end(&w);
    agentc_buf_byte(&out, '\n');

    (void)agentc_write_file_atomic(path, out.p, out.len, 0644);

    for (size_t i = 0; i < paths.len; i++) agentc_free(((char **)paths.p)[i]);
    agentc_vec_free(&paths);
    agentc_vec_free(&verdicts);
    agentc_buf_free(&out);
}
