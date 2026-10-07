/* settings.c — extension settings store (P6-SETTINGS §2-5 / R-D29).
 *
 * A small registry of declared settings plus a flat value table keyed by
 * (ext,key,scope). Values are loaded from three JSONC files once at startup
 * (user config, trusted project config, auth.jsonc for secrets) and mutated by
 * set/reset, which validate first and then merge-write the owning file so every
 * sibling key survives. Resolution is lazy and fail-safe: the highest allowed
 * scope wins; an unparseable or out-of-range value falls back to the declared
 * default with a single warning.
 */
#include "settings.h"
#include "agent.h"
#include "config.h"
#include "ext.h"
#include "wire.h"
#include "plat.h"
#include "core/events.h"

#if defined(_WIN32) || defined(__MINGW32__)
/* MSVC's floating-point marker. Nothing here calls a float runtime library; a
 * plain FLOAT setting only needs SSE-level arithmetic, so the bare symbol is
 * enough to satisfy the freestanding Windows link (see Makefile). */
int _fltused = 0;
#endif

#define SETTINGS_MAX_DECLS 128
#define SETTINGS_MAX_VALUES 256

typedef struct {
    bool used;
    char *ext;
    char *key;
    char *label;
    char *description;
    char *dflt;
    char *enum_json;
    char *min_json;
    char *max_json;
    char *unit;
    int type;
    uint32_t flags;
    u64 owner;
    bool warned;
} SettingDecl;

typedef struct {
    bool used;
    char *ext;
    char *key;
    char *value; /* JSON text */
    int scope;
} SettingValue;

static SettingDecl g_decls[SETTINGS_MAX_DECLS];
static SettingValue g_vals[SETTINGS_MAX_VALUES];
static char g_cwd[4096];
static bool g_trusted;
static bool g_init;

static const char *scope_name(int scope) {
    switch (scope) {
    case AGENTC_SETTINGS_SCOPE_CLI:
        return "cli";
    case AGENTC_SETTINGS_SCOPE_PROJECT:
        return "project";
    case AGENTC_SETTINGS_SCOPE_USER:
        return "user";
    case AGENTC_SETTINGS_SCOPE_SECRET:
        return "secret";
    default:
        return "?";
    }
}

/* ------------------------------------------------------------- json utils */

static char *json_value_dup(const AgcJson *v) {
    if (!v)
        return agentc_strdup("null");
    AgcBuf b = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_json_emit(&w, v);
    if (!b.p)
        return agentc_strdup("null");
    return (char *)b.p;
}

static bool json_is_int(const AgcJson *v) {
    size_t n = 0;
    const char *s = agentc_json_num(v, &n);
    if (!s)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '.' || s[i] == 'e' || s[i] == 'E')
            return false;
    return true;
}

/* --------------------------------------------------------------- storage */

static SettingDecl *decl_find(const char *ext, const char *key) {
    if (!ext || !key)
        return NULL;
    for (size_t i = 0; i < SETTINGS_MAX_DECLS; i++) {
        SettingDecl *d = &g_decls[i];
        if (d->used && agentc_streq(d->ext, ext) && agentc_streq(d->key, key))
            return d;
    }
    return NULL;
}

static SettingValue *value_find(const char *ext, const char *key, int scope) {
    for (size_t i = 0; i < SETTINGS_MAX_VALUES; i++) {
        SettingValue *v = &g_vals[i];
        if (v->used && v->scope == scope && agentc_streq(v->ext, ext) && agentc_streq(v->key, key))
            return v;
    }
    return NULL;
}

static SettingValue *value_put(const char *ext, const char *key, int scope,
                               const char *value_json) {
    SettingValue *v = value_find(ext, key, scope);
    if (!v) {
        for (size_t i = 0; i < SETTINGS_MAX_VALUES; i++)
            if (!g_vals[i].used) {
                v = &g_vals[i];
                break;
            }
    }
    if (!v) {
        agentc_logf(2, "settings: value table full, dropping %s.%s", ext, key);
        return NULL;
    }
    if (!v->used) {
        v->used = true;
        v->ext = agentc_strdup(ext);
        v->key = agentc_strdup(key);
    }
    agentc_free(v->value);
    v->value = agentc_strdup(value_json);
    v->scope = scope;
    return v;
}

static void value_del(const char *ext, const char *key, int scope) {
    SettingValue *v = value_find(ext, key, scope);
    if (!v)
        return;
    agentc_free(v->ext);
    agentc_free(v->key);
    agentc_free(v->value);
    agentc_memset(v, 0, sizeof *v);
}

/* ------------------------------------------------------------ validation */

/* True when `value_json` is a valid value for `d`. On failure `err` receives a
 * short reason. */
static bool value_valid(const SettingDecl *d, const char *value_json, const char **err) {
    static const char bad[] = "invalid value";
    *err = bad;
    if (!value_json)
        return false;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *v = agentc_json_parse_in(ja, value_json, agentc_strlen(value_json));
    bool ok = false;
    switch (d->type) {
    case AGENTC_SET_BOOL:
        ok = agentc_json_type(v) == AGENTC_JSON_TRUE || agentc_json_type(v) == AGENTC_JSON_FALSE;
        break;
    case AGENTC_SET_INT:
        ok = agentc_json_type(v) == AGENTC_JSON_NUM && json_is_int(v);
        break;
    case AGENTC_SET_FLOAT:
        ok = agentc_json_type(v) == AGENTC_JSON_NUM;
        break;
    case AGENTC_SET_STRING:
    case AGENTC_SET_PATH:
        ok = agentc_json_type(v) == AGENTC_JSON_STR;
        break;
    case AGENTC_SET_ENUM: {
        if (agentc_json_type(v) != AGENTC_JSON_STR)
            break;
        size_t vlen = 0;
        const char *vs = agentc_json_str(v, &vlen);
        ok = false;
        if (d->enum_json) {
            AgcJsonArena *ea = agentc_json_arena_new(0);
            AgcJson *e = agentc_json_parse_in(ea, d->enum_json, agentc_strlen(d->enum_json));
            if (agentc_json_type(e) == AGENTC_JSON_ARR) {
                for (size_t i = 0; i < agentc_json_len(e); i++) {
                    size_t elen = 0;
                    const char *es = agentc_json_str(agentc_json_at(e, i), &elen);
                    if (es && elen == vlen && agentc_memeq(es, vs, vlen)) {
                        ok = true;
                        break;
                    }
                }
            }
            agentc_json_arena_free(ea);
        }
        break;
    }
    case AGENTC_SET_LIST: {
        if (agentc_json_type(v) != AGENTC_JSON_ARR)
            break;
        ok = true;
        for (size_t i = 0; i < agentc_json_len(v); i++)
            if (agentc_json_type(agentc_json_at(v, i)) != AGENTC_JSON_STR) {
                ok = false;
                break;
            }
        break;
    }
    default:
        break;
    }
    if (ok && (d->type == AGENTC_SET_INT || d->type == AGENTC_SET_FLOAT)) {
        size_t n = 0;
        const char *num = agentc_json_num(v, &n);
        double x = 0;
        bool neg = false;
        for (size_t i = 0; i < n; i++) {
            char c = num[i];
            if (c == '-') {
                neg = true;
                continue;
            }
            if (c >= '0' && c <= '9')
                x = x * 10 + (c - '0');
            else if (c == '.') { /* fractional part ignored for bound checks on ints */
            }
        }
        if (neg)
            x = -x;
        const char *mm;
        if (d->min_json && (mm = d->min_json) && mm[0]) {
            double lo = 0;
            bool lneg = false;
            for (const char *p = mm; *p; p++) {
                if (*p == '-')
                    lneg = true;
                else if (*p >= '0' && *p <= '9')
                    lo = lo * 10 + (*p - '0');
            }
            if (lneg)
                lo = -lo;
            if (x < lo) {
                *err = "below minimum";
                ok = false;
            }
        }
        if (ok && d->max_json && d->max_json[0]) {
            double hi = 0;
            bool hneg = false;
            for (const char *p = d->max_json; *p; p++) {
                if (*p == '-')
                    hneg = true;
                else if (*p >= '0' && *p <= '9')
                    hi = hi * 10 + (*p - '0');
            }
            if (hneg)
                hi = -hi;
            if (x > hi) {
                *err = "above maximum";
                ok = false;
            }
        }
    }
    agentc_json_arena_free(ja);
    return ok;
}

/* --------------------------------------------------------------- loading */

static void load_settings_subtree(const AgcJson *root, int scope) {
    const AgcJson *set = agentc_json_get(root, "settings");
    if (agentc_json_type(set) != AGENTC_JSON_OBJ)
        return;
    for (size_t i = 0; i < (size_t)-1; i++) {
        const AgcJson *ek = agentc_json_key_at(set, i);
        if (!ek)
            break;
        size_t elen = 0;
        const char *ext = agentc_json_str(ek, &elen);
        const AgcJson *eo = agentc_json_val_at(set, i);
        if (!ext || agentc_json_type(eo) != AGENTC_JSON_OBJ)
            continue;
        for (size_t j = 0; j < (size_t)-1; j++) {
            const AgcJson *kk = agentc_json_key_at(eo, j);
            if (!kk)
                break;
            size_t klen = 0;
            const char *key = agentc_json_str(kk, &klen);
            if (!key)
                continue;
            char *val = json_value_dup(agentc_json_val_at(eo, j));
            value_put(ext, key, scope, val);
            agentc_free(val);
        }
    }
}

static void load_scope_file(const char *path, int scope) {
    if (!path)
        return;
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text)
        return;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(ja, text, len);
    if (agentc_json_type(root) == AGENTC_JSON_OBJ)
        load_settings_subtree(root, scope);
    agentc_json_arena_free(ja);
    agentc_free(text);
}

int agentc_settings_init(const char *cwd, bool trusted) {
    agentc_settings_shutdown();
    g_init = true;
    g_trusted = trusted;
    g_cwd[0] = 0;
    if (cwd)
        agentc_snprintf(g_cwd, sizeof g_cwd, "%s", cwd);

    char base[4096];
    char path[4160];
    if (agentc_config_home(base, sizeof base)) {
        if (agentc_path_join(path, sizeof path, base, "config.jsonc"))
            load_scope_file(path, AGENTC_SETTINGS_SCOPE_USER);
        if (agentc_path_join(path, sizeof path, base, "auth.jsonc"))
            load_scope_file(path, AGENTC_SETTINGS_SCOPE_SECRET);
    }
    if (trusted && g_cwd[0]) {
        char proj[4096];
        if (agentc_path_join(proj, sizeof proj, g_cwd, ".agentc/config.jsonc"))
            load_scope_file(proj, AGENTC_SETTINGS_SCOPE_PROJECT);
    }
    return 0;
}

void agentc_settings_shutdown(void) {
    for (size_t i = 0; i < SETTINGS_MAX_DECLS; i++) {
        SettingDecl *d = &g_decls[i];
        if (!d->used)
            continue;
        agentc_free(d->ext);
        agentc_free(d->key);
        agentc_free(d->label);
        agentc_free(d->description);
        agentc_free(d->dflt);
        agentc_free(d->enum_json);
        agentc_free(d->min_json);
        agentc_free(d->max_json);
        agentc_free(d->unit);
        agentc_memset(d, 0, sizeof *d);
    }
    for (size_t i = 0; i < SETTINGS_MAX_VALUES; i++) {
        SettingValue *v = &g_vals[i];
        if (!v->used)
            continue;
        agentc_free(v->ext);
        agentc_free(v->key);
        agentc_free(v->value);
        agentc_memset(v, 0, sizeof *v);
    }
    g_init = false;
    g_trusted = false;
    g_cwd[0] = 0;
}

void agentc_settings_declare(const char *ext, const AgcExtSetting *s, u64 owner) {
    if (!ext || !ext[0] || !s)
        return;
    if (!s->key || !s->key[0] || !s->default_json) {
        agentc_logf(2, "settings: %s dropped a setting without key/default", ext);
        return;
    }
    if (s->struct_size < (uint32_t)(offsetof(AgcExtSetting, flags) + sizeof s->flags)) {
        agentc_logf(2, "settings: %s dropped a short setting struct", ext);
        return;
    }
    SettingDecl *d = decl_find(ext, s->key);
    if (!d) {
        for (size_t i = 0; i < SETTINGS_MAX_DECLS; i++)
            if (!g_decls[i].used) {
                d = &g_decls[i];
                break;
            }
    }
    if (!d) {
        agentc_logf(2, "settings: declaration table full, dropping %s.%s", ext, s->key);
        return;
    }
    if (d->used) {
        agentc_free(d->label);
        agentc_free(d->description);
        agentc_free(d->dflt);
        agentc_free(d->enum_json);
        agentc_free(d->min_json);
        agentc_free(d->max_json);
        agentc_free(d->unit);
    } else {
        agentc_memset(d, 0, sizeof *d);
        d->used = true;
        d->ext = agentc_strdup(ext);
        d->key = agentc_strdup(s->key);
    }
    d->label = agentc_strdup(s->label ? s->label : s->key);
    d->description = agentc_strdup(s->description ? s->description : "");
    d->dflt = agentc_strdup(s->default_json);
    d->enum_json = s->enum_json ? agentc_strdup(s->enum_json) : NULL;
    d->min_json = s->min_json ? agentc_strdup(s->min_json) : NULL;
    d->max_json = s->max_json ? agentc_strdup(s->max_json) : NULL;
    d->unit = s->unit ? agentc_strdup(s->unit) : NULL;
    d->type = s->type;
    d->flags = s->flags;
    d->owner = owner;
    d->warned = false;
}

void agentc_settings_owner_remove(u64 owner) {
    for (size_t i = 0; i < SETTINGS_MAX_DECLS; i++) {
        SettingDecl *d = &g_decls[i];
        if (!d->used || d->owner != owner)
            continue;
        agentc_free(d->ext);
        agentc_free(d->key);
        agentc_free(d->label);
        agentc_free(d->description);
        agentc_free(d->dflt);
        agentc_free(d->enum_json);
        agentc_free(d->min_json);
        agentc_free(d->max_json);
        agentc_free(d->unit);
        agentc_memset(d, 0, sizeof *d);
    }
}

/* -------------------------------------------------------------- resolve */

static bool scope_allowed(const SettingDecl *d, int scope) {
    if (scope == AGENTC_SETTINGS_SCOPE_CLI)
        return true;
    if (d->flags & AGENTC_SETTING_SECRET)
        return scope == AGENTC_SETTINGS_SCOPE_SECRET;
    if (scope == AGENTC_SETTINGS_SCOPE_PROJECT)
        return (d->flags & AGENTC_SETTING_PROJECT) && g_trusted;
    return scope == AGENTC_SETTINGS_SCOPE_USER;
}

/* Finds the winning value for a declaration, in scope order. */
static const SettingValue *resolve_value(const SettingDecl *d) {
    static const int order[] = {
        AGENTC_SETTINGS_SCOPE_CLI,
        AGENTC_SETTINGS_SCOPE_PROJECT,
        AGENTC_SETTINGS_SCOPE_USER,
        AGENTC_SETTINGS_SCOPE_SECRET,
    };
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        if (!scope_allowed(d, order[i]))
            continue;
        SettingValue *v = value_find(d->ext, d->key, order[i]);
        if (v)
            return v;
    }
    return NULL;
}

const char *agentc_settings_get_json(const char *ext, const char *key) {
    SettingDecl *d = decl_find(ext, key);
    if (!d)
        return NULL;
    const SettingValue *v = resolve_value(d);
    if (!v)
        return d->dflt;
    const char *err = NULL;
    if (!value_valid(d, v->value, &err)) {
        if (!d->warned) {
            d->warned = true;
            agentc_logf(2, "settings: %s.%s from %s %s; using default", ext, key,
                        scope_name(v->scope), err ? err : "invalid");
        }
        return d->dflt;
    }
    return v->value;
}

int agentc_settings_get_bool(const char *ext, const char *key, int dflt) {
    const char *v = agentc_settings_get_json(ext, key);
    if (!v)
        return dflt;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *j = agentc_json_parse_in(ja, v, agentc_strlen(v));
    int out = dflt;
    if (agentc_json_type(j) == AGENTC_JSON_TRUE)
        out = 1;
    else if (agentc_json_type(j) == AGENTC_JSON_FALSE)
        out = 0;
    agentc_json_arena_free(ja);
    return out;
}

long long agentc_settings_get_int(const char *ext, const char *key, long long dflt) {
    const char *v = agentc_settings_get_json(ext, key);
    if (!v)
        return dflt;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *j = agentc_json_parse_in(ja, v, agentc_strlen(v));
    long long out = dflt;
    if (agentc_json_type(j) == AGENTC_JSON_NUM) {
        size_t n = 0;
        const char *s = agentc_json_num(j, &n);
        long long x = 0;
        bool neg = false;
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '-')
                neg = true;
            else if (s[i] >= '0' && s[i] <= '9')
                x = x * 10 + (s[i] - '0');
        }
        out = neg ? -x : x;
    }
    agentc_json_arena_free(ja);
    return out;
}

double agentc_settings_get_float(const char *ext, const char *key, double dflt) {
    const char *v = agentc_settings_get_json(ext, key);
    if (!v)
        return dflt;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *j = agentc_json_parse_in(ja, v, agentc_strlen(v));
    double out = dflt;
    if (agentc_json_type(j) == AGENTC_JSON_NUM) {
        size_t n = 0;
        const char *s = agentc_json_num(j, &n);
        double x = 0, frac = 0.1;
        bool neg = false, dot = false;
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '-') {
                neg = true;
                continue;
            }
            if (s[i] == '.') {
                dot = true;
                continue;
            }
            if (s[i] < '0' || s[i] > '9')
                continue;
            if (dot) {
                x += (s[i] - '0') * frac;
                frac *= 0.1;
            } else
                x = x * 10 + (s[i] - '0');
        }
        out = neg ? -x : x;
    }
    agentc_json_arena_free(ja);
    return out;
}

int agentc_settings_scope_of(const char *ext, const char *key) {
    SettingDecl *d = decl_find(ext, key);
    if (!d)
        return -1;
    const SettingValue *v = resolve_value(d);
    return v ? v->scope : -1;
}

bool agentc_settings_is_set(const char *ext, const char *key) {
    SettingDecl *d = decl_find(ext, key);
    return d && resolve_value(d) != NULL;
}

size_t agentc_settings_list(AgcSettingInfo *out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < SETTINGS_MAX_DECLS; i++) {
        SettingDecl *d = &g_decls[i];
        if (!d->used)
            continue;
        if (out && n < max) {
            out[n].ext = d->ext;
            out[n].key = d->key;
            out[n].label = d->label;
            out[n].description = d->description;
            out[n].default_json = d->dflt;
            out[n].enum_json = d->enum_json;
            out[n].min_json = d->min_json;
            out[n].max_json = d->max_json;
            out[n].unit = d->unit;
            out[n].type = d->type;
            out[n].flags = d->flags;
        }
        n++;
    }
    return out ? (n < max ? n : max) : n;
}

/* ------------------------------------------------------- merge-write path */

static void emit_settings_ext(AgcJsonW *w, const AgcJson *eo, const char *key, const char *raw,
                              bool remove) {
    agentc_jsonw_obj(w);
    bool saw = false;
    if (agentc_json_type(eo) == AGENTC_JSON_OBJ) {
        for (size_t i = 0; i < (size_t)-1; i++) {
            const AgcJson *kk = agentc_json_key_at(eo, i);
            if (!kk)
                break;
            size_t klen = 0;
            const char *name = agentc_json_str(kk, &klen);
            if (name && agentc_streq(name, key)) {
                saw = true;
                if (!remove) {
                    agentc_jsonw_key(w, name);
                    agentc_jsonw_raw(w, raw, agentc_strlen(raw));
                }
                continue;
            }
            agentc_jsonw_key(w, name ? name : "");
            agentc_json_emit(w, agentc_json_val_at(eo, i));
        }
    }
    if (!saw && !remove && raw) {
        agentc_jsonw_key(w, key);
        agentc_jsonw_raw(w, raw, agentc_strlen(raw));
    }
    agentc_jsonw_end(w);
}

static void emit_settings_obj(AgcJsonW *w, const AgcJson *set, const char *ext, const char *key,
                              const char *raw, bool remove) {
    agentc_jsonw_obj(w);
    bool saw = false;
    if (agentc_json_type(set) == AGENTC_JSON_OBJ) {
        for (size_t i = 0; i < (size_t)-1; i++) {
            const AgcJson *ek = agentc_json_key_at(set, i);
            if (!ek)
                break;
            size_t elen = 0;
            const char *name = agentc_json_str(ek, &elen);
            if (name && agentc_streq(name, ext)) {
                saw = true;
                agentc_jsonw_key(w, name);
                emit_settings_ext(w, agentc_json_val_at(set, i), key, raw, remove);
                continue;
            }
            agentc_jsonw_key(w, name ? name : "");
            agentc_json_emit(w, agentc_json_val_at(set, i));
        }
    }
    if (!saw && !remove && raw) {
        agentc_jsonw_key(w, ext);
        emit_settings_ext(w, NULL, key, raw, false);
    }
    agentc_jsonw_end(w);
}

static int write_scope_value(const char *path, const char *ext, const char *key, const char *raw,
                             bool remove) {
    if (!path)
        return -2;
    size_t len = 0;
    char *old = agentc_read_file_owned(path, &len);
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = old ? agentc_json_parse_in(ja, old, len) : NULL;
    agentc_free(old);
    if (agentc_json_type(root) != AGENTC_JSON_OBJ)
        root = NULL;

    AgcBuf out = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &out);
    agentc_jsonw_obj(&w);
    bool saw_settings = false;
    for (size_t i = 0; root && i < (size_t)-1; i++) {
        const AgcJson *k = agentc_json_key_at(root, i);
        if (!k)
            break;
        size_t klen = 0;
        const char *name = agentc_json_str(k, &klen);
        if (name && agentc_streq(name, "settings")) {
            saw_settings = true;
            agentc_jsonw_key(&w, "settings");
            emit_settings_obj(&w, agentc_json_val_at(root, i), ext, key, raw, remove);
            continue;
        }
        agentc_jsonw_key(&w, name ? name : "");
        agentc_json_emit(&w, agentc_json_val_at(root, i));
    }
    if (!saw_settings) {
        agentc_jsonw_key(&w, "settings");
        emit_settings_obj(&w, NULL, ext, key, raw, remove);
    }
    agentc_jsonw_end(&w);
    agentc_buf_byte(&out, '\n');
    agentc_json_arena_free(ja);

    int rc = agentc_write_file_atomic(path, out.p, out.len, remove ? 0600 : 0600);
    agentc_buf_free(&out);
    return rc;
}

static int settings_file_for(int scope, char *buf, size_t cap) {
    char base[4096];
    if (scope == AGENTC_SETTINGS_SCOPE_PROJECT) {
        if (!g_cwd[0])
            return -2;
        return agentc_path_join(buf, cap, g_cwd, ".agentc/config.jsonc") ? 0 : -2;
    }
    if (!agentc_config_home(base, sizeof base))
        return -2;
    const char *name = scope == AGENTC_SETTINGS_SCOPE_SECRET ? "auth.jsonc" : "config.jsonc";
    return agentc_path_join(buf, cap, base, name) ? 0 : -2;
}

static int setting_scope_for(const SettingDecl *d) {
    if (d->flags & AGENTC_SETTING_SECRET)
        return AGENTC_SETTINGS_SCOPE_SECRET;
    if ((d->flags & AGENTC_SETTING_PROJECT) && g_trusted && g_cwd[0])
        return AGENTC_SETTINGS_SCOPE_PROJECT;
    return AGENTC_SETTINGS_SCOPE_USER;
}

int agentc_settings_set(const char *ext, const char *key, const char *value_json) {
    SettingDecl *d = decl_find(ext, key);
    if (!d)
        return -2; /* ENOENT: never validate an undeclared key */
    if (!value_json)
        return -22;
    const char *err = NULL;
    if (!value_valid(d, value_json, &err)) {
        agentc_logf(2, "settings: %s.%s rejected: %s", ext, key, err ? err : "invalid");
        return -22;
    }
    /* The settings_change hook may reject a write (fail-closed). */
    if (agentc_ext_wants(AGENTC_HEV_SETTINGS_CHANGE)) {
        AgcBuf p = {0};
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "ext");
        agentc_jsonw_cstr(&w, ext);
        agentc_jsonw_key(&w, "key");
        agentc_jsonw_cstr(&w, key);
        agentc_jsonw_key(&w, "value");
        agentc_jsonw_raw(&w, value_json, agentc_strlen(value_json));
        agentc_jsonw_end(&w);
        AgcExtResult r = agentc_ext_emit(AGENTC_HEV_SETTINGS_CHANGE, (char *)p.p);
        agentc_free(r.result_json);
        agentc_buf_free(&p);
        if (r.blocked) {
            agentc_logf(2, "settings: %s.%s rejected by settings_change", ext, key);
            return -1; /* EPERM */
        }
    }

    int scope = setting_scope_for(d);
    char path[4160];
    if (settings_file_for(scope, path, sizeof path) != 0)
        return -2;
    int rc = write_scope_value(path, ext, key, value_json, false);
    if (rc != 0)
        return rc;
    value_put(ext, key, scope, value_json);

    AgcSettingsChange ev = {ext, key, scope};
    agentc_events_emit(NULL, AGENTC_EV_SETTINGS_CHANGED, &ev);
    return 0;
}

int agentc_settings_reset(const char *ext, const char *key) {
    SettingDecl *d = decl_find(ext, key);
    if (!d)
        return -2;
    /* Remove from every scope we ever write (CLI overrides are memory-only and
     * removed by replacing the CLI table). */
    int scope = setting_scope_for(d);
    char path[4160];
    if (settings_file_for(scope, path, sizeof path) != 0)
        return -2;
    int rc = write_scope_value(path, ext, key, NULL, true);
    if (rc != 0)
        return rc;
    value_del(ext, key, scope);
    AgcSettingsChange ev = {ext, key, scope};
    agentc_events_emit(NULL, AGENTC_EV_SETTINGS_CHANGED, &ev);
    return 0;
}

/* ------------------------------------------------------------------ CLI */

/* Parses `ext.key=<json>`; a non-JSON right side becomes a JSON string. */
void agentc_settings_add_cli(const char *assignment) {
    if (!assignment || !assignment[0])
        return;
    const char *eq = agentc_str_str(assignment, "=");
    if (!eq) {
        agentc_logf(2, "settings: --set expects ext.key=value");
        return;
    }
    size_t left = (size_t)(eq - assignment);
    char ext[128], key[128];
    const char *dot = NULL;
    for (size_t i = 0; i < left; i++)
        if (assignment[i] == '.')
            dot = assignment + i;
    if (!dot) {
        agentc_logf(2, "settings: --set expects ext.key=value");
        return;
    }
    size_t elen = (size_t)(dot - assignment);
    size_t klen = left - elen - 1;
    if (elen == 0 || elen >= sizeof ext || klen == 0 || klen >= sizeof key) {
        agentc_logf(2, "settings: --set key too long");
        return;
    }
    agentc_memcpy(ext, assignment, elen);
    ext[elen] = 0;
    agentc_memcpy(key, dot + 1, klen);
    key[klen] = 0;

    const char *value = eq + 1;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *j = agentc_json_parse_in(ja, value, agentc_strlen(value));
    char *raw = NULL;
    if (agentc_json_type(j) == -1) {
        AgcBuf b = {0};
        AgcJsonW w;
        agentc_jsonw_init(&w, &b);
        agentc_jsonw_cstr(&w, value);
        raw = (char *)b.p;
    } else {
        raw = json_value_dup(j);
    }
    agentc_json_arena_free(ja);
    if (raw) {
        value_put(ext, key, AGENTC_SETTINGS_SCOPE_CLI, raw);
        agentc_free(raw);
    }
}
