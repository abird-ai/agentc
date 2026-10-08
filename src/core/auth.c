/* auth.c — credential lookup: stored OAuth token, then provider env vars,
 * then $XDG_CONFIG_HOME/agentc/auth.jsonc api_key entries.
 *
 * auth.jsonc shape:
 *   {"anthropic":{"api_key":"…"},"openai":{"api_key":"…"},"…":{"oauth":{…}}}
 * The legacy {"api_keys":{"anthropic":"…"}} shape is also accepted. The file is
 * read once and cached in a process-global store; agentc_auth_free() releases it.
 * Mode is inspected on read: a warning is logged when it is looser than 0600.
 *
 * Resolution order: stored OAuth credential (refreshed when near expiry, and a
 * failed refresh is an error — never a silent env/api-key fallback) > provider
 * env var > auth.jsonc api_key. Config api_keys are handled by main.c. */
#include "agentc.h"
#include "plat.h"
#include "wire.h"
#include "config.h"
#include "oauth.h"
#include "prov/provider.h"

extern const char *agentc_env_get(const char *name);
extern char *agentc_read_file_owned(const char *path, size_t *len);
extern const char *agentc_config_home(char *buf, size_t cap);
extern bool agentc_path_join(char *out, size_t cap, const char *dir, const char *name);

#define AUTH_MAX 32

typedef struct {
    char *provider;
    char *key;
} AuthKey;

static AuthKey g_keys[AUTH_MAX];
static size_t g_nkeys;
static bool g_loaded;
static char *g_auth_path;      /* owned; the file the store was loaded from */

void agentc_auth_free(void) {
    for (size_t i = 0; i < g_nkeys; i++) {
        agentc_free(g_keys[i].provider);
        agentc_free(g_keys[i].key);
    }
    g_nkeys = 0;
    agentc_free(g_auth_path);
    g_auth_path = NULL;
    g_loaded = false;
    agentc_oauth_free();
}

static void store_key(const char *provider, const char *key) {
    if (!provider || !provider[0] || !key || !key[0] || g_nkeys >= AUTH_MAX) return;
    for (size_t i = 0; i < g_nkeys; i++) {
        if (agentc_streq(g_keys[i].provider, provider)) {
            agentc_free(g_keys[i].key);
            g_keys[i].key = agentc_strdup(key);
            return;
        }
    }
    g_keys[g_nkeys].provider = agentc_strdup(provider);
    g_keys[g_nkeys].key = agentc_strdup(key);
    g_nkeys++;
}

static void auth_load(void) {
    g_loaded = true;
    char base[4096];
    char path[4160];
    if (!agentc_config_home(base, sizeof base)) return;
    if (!agentc_path_join(path, sizeof path, base, "auth.jsonc")) return;

    int fd = os_open(path, OS_O_RDONLY, 0);
    if (fd >= 0) {
        /* POSIX permission bits are not meaningful on Windows (Wine reports the
         * file as 0644 no matter what mode the writer asked for), so the
         * advisory mode check only runs on the POSIX ports. */
        if (!agentc_streq(os_platform(), "windows")) {
            struct os_stat st;
            if (os_fstat(fd, &st) == 0) {
                u32 mode = st.st_mode & 0777u;
                if (mode & 0077u)
                    agentc_logf(2, "warning: %s is mode 0%03x (expected 0600)", path, mode);
            }
        }
        os_close(fd);
    }

    g_auth_path = agentc_strdup(path);
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text) return;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(ja, text, len);
    if (agentc_json_type(root) == AGENTC_JSON_OBJ) {
        /* every top-level provider with an api_key is kept; an "oauth" object
         * inside is left for oauth.c */
        for (size_t i = 0; i < (size_t)-1; i++) {
            const AgcJson *k = agentc_json_key_at(root, i);
            if (!k) break;
            size_t klen = 0;
            const char *provider = agentc_json_str(k, &klen);
            const AgcJson *pv = agentc_json_val_at(root, i);
            if (provider && agentc_json_type(pv) == AGENTC_JSON_OBJ)
                store_key(provider, agentc_json_get_str(pv, "api_key"));
        }
        /* legacy {"api_keys":{...}} shape */
        const AgcJson *legacy = agentc_json_get(root, "api_keys");
        if (agentc_json_type(legacy) == AGENTC_JSON_OBJ) {
            for (size_t i = 0; i < (size_t)-1; i++) {
                const AgcJson *k = agentc_json_key_at(legacy, i);
                if (!k) break;
                size_t klen = 0;
                const char *provider = agentc_json_str(k, &klen);
                const AgcJson *kv = agentc_json_val_at(legacy, i);
                if (provider) store_key(provider, agentc_json_str(kv, &(size_t){ 0 }));
            }
        }
    }
    agentc_free(text);
    agentc_json_arena_free(ja);
}

const char *agentc_auth_key(const char *provider) {
    if (!provider) return NULL;
    /* A stored OAuth credential owns the provider: refresh near expiry, and on
     * failure return NULL without consulting env or auth.jsonc api keys. */
    const char *token = NULL;
    int oauth = agentc_oauth_token_for(provider, &token);
    if (oauth == 1) return token;
    if (oauth < 0) return NULL;
    const AgcProviderOps *ops = agentc_provider_by_name(provider);
    if (ops) {
        for (size_t j = 0; j < 3 && ops->env_keys[j]; j++) {
            const char *v = agentc_env_get(ops->env_keys[j]);
            if (v && v[0]) return v;
        }
    }
    /* A stored auth.jsonc key is provider-agnostic: consult it even when the
     * provider has no env-key row (an extension provider). */
    if (!g_loaded) auth_load();
    for (size_t i = 0; i < g_nkeys; i++)
        if (agentc_streq(g_keys[i].provider, provider)) return g_keys[i].key;
    return NULL;
}

/* ------------------------------------------------------------ credential store
 *
 * agentc_auth_store_rewrite() is the only writer: one read-merge-rewrite of
 * auth.jsonc that preserves every top-level pair (including "api_keys" and
 * unknown keys) and changes only `provider`'s object. api_key and oauth are each
 * AUTH_KEEP (leave the member as found), AUTH_SET (write the value; NULL/empty
 * removes the member) or AUTH_CLEAR (remove). agentc_auth_set_key() and oauth.c's
 * store_save() both go through it, so a subscription login cannot silently drop
 * stored api keys or fields owned by other tools.
 */

enum { AUTH_KEEP = 0, AUTH_SET = 1, AUTH_CLEAR = 2 };

/* Append `provider`'s object, applying the two member operations. Unknown
 * members are re-emitted in place; a missing member is appended when set. */
static void auth_emit_provider(AgcJsonW *w, const AgcJson *pv, const char *provider,
                               int api_key_op, const char *api_key, int oauth_op,
                               const char *oauth_json) {
    bool want_key = api_key_op == AUTH_SET && api_key && api_key[0];
    bool want_oauth = oauth_op == AUTH_SET && oauth_json && oauth_json[0];
    bool saw_key = false, saw_oauth = false;

    agentc_jsonw_key(w, provider);
    agentc_jsonw_obj(w);
    if (agentc_json_type(pv) == AGENTC_JSON_OBJ) {
        for (size_t i = 0; i < (size_t)-1; i++) {
            const AgcJson *pk = agentc_json_key_at(pv, i);
            if (!pk) break;
            size_t plen = 0;
            const char *pname = agentc_json_str(pk, &plen);
            if (pname && agentc_streq(pname, "api_key")) {
                saw_key = true;
                if (api_key_op == AUTH_KEEP) {
                    agentc_jsonw_key(w, "api_key");
                    agentc_json_emit(w, agentc_json_val_at(pv, i));
                } else if (want_key) {
                    agentc_jsonw_key(w, "api_key");
                    agentc_jsonw_cstr(w, api_key);
                }
                continue;
            }
            if (pname && agentc_streq(pname, "oauth")) {
                saw_oauth = true;
                if (oauth_op == AUTH_KEEP) {
                    agentc_jsonw_key(w, "oauth");
                    agentc_json_emit(w, agentc_json_val_at(pv, i));
                } else if (want_oauth) {
                    agentc_jsonw_key(w, "oauth");
                    agentc_jsonw_raw(w, oauth_json, agentc_strlen(oauth_json));
                }
                continue;
            }
            agentc_jsonw_key(w, pname ? pname : "");
            agentc_json_emit(w, agentc_json_val_at(pv, i));
        }
    }
    if (!saw_key && want_key) {
        agentc_jsonw_key(w, "api_key");
        agentc_jsonw_cstr(w, api_key);
    }
    if (!saw_oauth && want_oauth) {
        agentc_jsonw_key(w, "oauth");
        agentc_jsonw_raw(w, oauth_json, agentc_strlen(oauth_json));
    }
    agentc_jsonw_end(w);
}

int agentc_auth_store_rewrite(const char *provider, int api_key_op, const char *api_key,
                              int oauth_op, const char *oauth_json) {
    if (!provider || !provider[0]) return -22;
    if (!g_loaded) auth_load();

    char *old_text = NULL;
    size_t old_len = 0;
    if (g_auth_path) old_text = agentc_read_file_owned(g_auth_path, &old_len);
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = old_text ? agentc_json_parse_in(ja, old_text, old_len) : NULL;
    agentc_free(old_text);
    if (agentc_json_type(root) != AGENTC_JSON_OBJ) root = NULL;   /* malformed -> {} */

    bool want_key = api_key_op == AUTH_SET && api_key && api_key[0];
    bool want_oauth = oauth_op == AUTH_SET && oauth_json && oauth_json[0];

    AgcBuf out = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &out);
    agentc_jsonw_obj(&w);
    bool replaced = false;
    for (size_t i = 0; root && i < (size_t)-1; i++) {
        const AgcJson *k = agentc_json_key_at(root, i);
        if (!k) break;
        size_t klen = 0;
        const char *kname = agentc_json_str(k, &klen);
        const AgcJson *pv = agentc_json_val_at(root, i);
        if (kname && agentc_streq(kname, provider)) {
            /* a non-object provider is only replaced when there is a set op */
            if (agentc_json_type(pv) == AGENTC_JSON_OBJ || want_key || want_oauth) {
                auth_emit_provider(&w, pv, provider, api_key_op, api_key, oauth_op,
                                   oauth_json);
            } else {
                agentc_jsonw_key(&w, kname);
                agentc_json_emit(&w, pv);
            }
            replaced = true;
        } else {
            agentc_jsonw_key(&w, kname ? kname : "");
            agentc_json_emit(&w, pv);
        }
    }
    if (!replaced && (want_key || want_oauth))
        auth_emit_provider(&w, NULL, provider, api_key_op, api_key, oauth_op, oauth_json);
    agentc_jsonw_end(&w);
    agentc_buf_byte(&out, '\n');
    agentc_json_arena_free(ja);

    const char *path = g_auth_path;
    char pathbuf[4160];
    if (!path) {
        char base[4096];
        if (!agentc_config_home(base, sizeof base)) { agentc_buf_free(&out); return -2; }
        if (!agentc_path_join(pathbuf, sizeof pathbuf, base, "auth.jsonc")) {
            agentc_buf_free(&out);
            return -2;
        }
        path = pathbuf;
    }
    /* creates the config dir on a fresh install, writes atomically, 0600 */
    int rc = agentc_write_file_atomic(path, out.p, out.len, 0600);
    agentc_buf_free(&out);
    return rc;
}

int agentc_auth_set_key(const char *provider, const char *key) {
    if (!provider || !provider[0]) return -22;
    int rc = agentc_auth_store_rewrite(provider, AUTH_SET, key, AUTH_KEEP, NULL);
    if (rc != 0) return rc;

    /* refresh the in-memory store to match what was just written */
    if (key && key[0]) {
        for (size_t i = 0; i < g_nkeys; i++) {
            if (agentc_streq(g_keys[i].provider, provider)) {
                agentc_free(g_keys[i].key);
                g_keys[i].key = agentc_strdup(key);
                return 0;
            }
        }
        if (g_nkeys < AUTH_MAX) {
            g_keys[g_nkeys].provider = agentc_strdup(provider);
            g_keys[g_nkeys].key = agentc_strdup(key);
            g_nkeys++;
        }
    } else {
        for (size_t i = 0; i < g_nkeys; i++) {
            if (agentc_streq(g_keys[i].provider, provider)) {
                agentc_free(g_keys[i].provider);
                agentc_free(g_keys[i].key);
                for (size_t j = i + 1; j < g_nkeys; j++) g_keys[j - 1] = g_keys[j];
                g_nkeys--;
                break;
            }
        }
    }
    return 0;
}
