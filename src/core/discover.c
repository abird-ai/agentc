/* discover.c — provider model discovery over the wire layer. */
#include "discover.h"
#include "agent.h"
#include "config.h"
#include "net.h"
#include "plat.h"
#include "wire.h"
#include "prov/provider.h"

#define DISCOVER_MAX 256
#define CACHE_FILE "models-cache.jsonc"

/* ---------------------------------------------------------------- helpers */
static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) agentc_snprintf(err, cap, "%s", msg);
}

/* Parse one OpenRouter pricing scalar (USD per token, usually a string, but a
 * number is accepted) into micro-USD per million tokens, or 0 when absent or
 * malformed. USD/token * 1e12 == micro-USD/M. */
static i64 pricing_usd_per_token(const AgcJson *pricing, const char *key) {
    const AgcJson *v = agentc_json_get(pricing, key);
    if (!v) return 0;
    size_t n = 0;
    const char *s = agentc_json_str(v, &n);
    if (!s) s = agentc_json_num(v, &n);
    char buf[64];
    if (!s || n == 0 || n + 1 > sizeof buf) return 0;
    agentc_memcpy(buf, s, n);
    buf[n] = 0;
    i64 r = agentc_rate_parse_scaled(buf, 1000000000000LL);
    return r < 0 ? 0 : r;
}

typedef struct {
    AgcBuf *body;
} Sink;

static int on_body(void *ud, const void *p, size_t n) {
    Sink *s = ud;
    if (s->body->len > (4u << 20)) return 1;   /* a model list is never this big */
    agentc_buf_push(s->body, p, n);
    return 0;
}

/* http_get(url, api_key, auth_override, extra_header, body, timeout) -> 0 | negative
 *
 * `api_key` is the Bearer fallback and is passed only for a row without an
 * `auth_headers` hook (the builtins). `auth_override` carries the
 * provider row's own discovery header line(s): when the row owns a hook its
 * override is authoritative even when empty, so AUTH_NONE or a missing key
 * sends no auth line at all instead of falling back to Bearer. */
static int http_get(const char *url, const char *api_key, const AgcBuf *auth_override,
                    const char *extra_header, AgcBuf *body, int timeout_ms, char *err,
                    size_t err_cap) {
    AgcBuf headers = { 0 };
    if (auth_override && auth_override->len) {
        agentc_buf_push(&headers, auth_override->p, auth_override->len);
    } else if (api_key && api_key[0]) {
        agentc_buf_cstr(&headers, "authorization: Bearer ");
        /* A key is untrusted input: a CR/LF in it would forge a header line,
         * so drop every control byte (and DEL) as the request path does. */
        for (const char *k = api_key; *k; k++) {
            u8 c = (u8)*k;
            if (c < 0x20 || c == 0x7f) continue;
            agentc_buf_byte(&headers, c);
        }
        agentc_buf_cstr(&headers, "\r\n");
    }
    if (extra_header) agentc_buf_cstr(&headers, extra_header);
    AgcHttp *h = agentc_http_new("GET", url, (const char *)headers.p, NULL, 0);
    agentc_buf_free(&headers);
    if (!h) {
        set_err(err, err_cap, "cannot parse url");
        return -22;
    }
    Sink sink = { body };
    int rc = agentc_http_run(h, on_body, &sink, timeout_ms);
    int status = agentc_http_status(h);
    if (rc != 0) {
        set_err(err, err_cap, agentc_http_error(h));
        agentc_http_free(h);
        return rc;
    }
    if (status != 200) {
        set_err(err, err_cap, "http status");
        agentc_http_free(h);
        return -71;                            /* EPROTO */
    }
    agentc_http_free(h);
    return 0;
}

/* ------------------------------------------------------------- parsing */

/* true when `arr` is a JSON array containing the string `want`. */
static bool json_array_has_str(const AgcJson *arr, const char *want) {
    if (agentc_json_type(arr) != AGENTC_JSON_ARR || !want) return false;
    size_t total = agentc_json_len(arr);
    for (size_t i = 0; i < total; i++) {
        const char *s = agentc_json_str(agentc_json_at(arr, i), NULL);
        if (s && agentc_streq(s, want)) return true;
    }
    return false;
}

static size_t parse_openai(const char *provider, const AgcJson *root, AgcDiscovered *out,
                          size_t max) {
    const AgcJson *data = agentc_json_get(root, "data");
    if (agentc_json_type(data) != AGENTC_JSON_ARR) data = agentc_json_get(root, "models");
    if (agentc_json_type(data) != AGENTC_JSON_ARR) return 0;
    size_t n = 0;
    size_t total = agentc_json_len(data);
    for (size_t i = 0; i < total && n < max; i++) {
        const AgcJson *m = agentc_json_at(data, i);
        if (agentc_json_type(m) != AGENTC_JSON_OBJ) continue;
        const char *id = agentc_json_get_str(m, "id");
        if (!id) id = agentc_json_get_str(m, "name");
        if (!id || !id[0]) continue;
        AgcDiscovered *d = &out[n];
        agentc_memset(d, 0, sizeof *d);
        d->id = agentc_strdup(id);
        const char *name = agentc_json_get_str(m, "display_name");
        d->name = agentc_strdup(name && name[0] ? name : id);
        /* some compatible gateways expose extras in an OpenAI-ish shape */
        const AgcJson *top = agentc_json_get(m, "top_provider");
        const AgcJson *arch = agentc_json_get(m, "architecture");
        u32 ctx = (u32)agentc_json_get_int(m, "context_window", 0);
        if (!ctx) ctx = (u32)agentc_json_get_int(m, "context_length", 0);
        if (!ctx) ctx = (u32)agentc_json_get_int(top, "context_length", 0);
        u32 mx = (u32)agentc_json_get_int(m, "max_output_tokens", 0);
        if (!mx) mx = (u32)agentc_json_get_int(top, "max_completion_tokens", 0);
        d->ctx_window = ctx;
        d->max_tokens = mx;
        /* Image input: an explicit "vision" wins; otherwise read the gateway's
         * declared input modalities. Only the input half of OpenRouter's
         * "text+image->text" modality string counts: the output half would mark
         * an image *generation* model as image input. */
        d->image = agentc_json_get_bool(m, "vision", false);
        if (agentc_json_get(m, "vision") == NULL) {
            const AgcJson *inmod = agentc_json_get(arch, "input_modalities");
            if (agentc_json_type(inmod) == AGENTC_JSON_ARR) {
                d->image = json_array_has_str(inmod, "image");
            } else {
                const char *modality = agentc_json_get_str(arch, "modality");
                if (modality) {
                    const char *arrow = agentc_str_str(modality, "->");
                    size_t inlen = arrow ? (size_t)(arrow - modality) : agentc_strlen(modality);
                    for (size_t i = 0; i + 5 <= inlen; i++)
                        if (agentc_memeq(modality + i, "image", 5)) {
                            d->image = true;
                            break;
                        }
                }
            }
        }
        /* OpenRouter advertises reasoning-capable models in supported_parameters */
        const AgcJson *params = agentc_json_get(m, "supported_parameters");
        d->reasoning = json_array_has_str(params, "reasoning") ||
                       json_array_has_str(params, "include_reasoning");
        /* OpenRouter publishes a pricing object (USD per token as strings);
         * stage the rates so agentc_model_register_dynamic() installs them.
         * Its usage input excludes cache tokens, so input_includes_cache stays
         * clear. */
        const AgcJson *pricing = agentc_json_get(m, "pricing");
        if (agentc_json_type(pricing) == AGENTC_JSON_OBJ) {
            i64 in = pricing_usd_per_token(pricing, "prompt");
            i64 outr = pricing_usd_per_token(pricing, "completion");
            if (in > 0 || outr > 0) {
                i64 cr = pricing_usd_per_token(pricing, "input_cache_read");
                i64 cw = pricing_usd_per_token(pricing, "input_cache_write");
                agentc_model_stage_rate(provider, id, in, outr, cr, cw,
                                        AGENTC_MODEL_RATE_KNOWN);
            }
        }
        n++;
    }
    return n;
}

/* Ollama native /api/tags: {"models":[{"name":…,"details":{"family":…,
 * "parameter_size":"7.6B"}}]} */
static size_t parse_ollama_tags(const AgcJson *root, AgcDiscovered *out, size_t max) {
    const AgcJson *models = agentc_json_get(root, "models");
    if (agentc_json_type(models) != AGENTC_JSON_ARR) return 0;
    size_t n = 0;
    size_t total = agentc_json_len(models);
    for (size_t i = 0; i < total && n < max; i++) {
        const AgcJson *m = agentc_json_at(models, i);
        if (agentc_json_type(m) != AGENTC_JSON_OBJ) continue;
        const char *name = agentc_json_get_str(m, "name");
        if (!name || !name[0]) name = agentc_json_get_str(m, "model");
        if (!name || !name[0]) continue;
        AgcDiscovered *d = &out[n];
        agentc_memset(d, 0, sizeof *d);
        d->id = agentc_strdup(name);
        d->name = agentc_strdup(name);
        const AgcJson *details = agentc_json_get(m, "details");
        const char *family = agentc_json_get_str(details, "family");
        const char *params = agentc_json_get_str(details, "parameter_size");
        if ((family && family[0]) || (params && params[0])) {
            AgcBuf b = { 0 };
            if (family && family[0]) agentc_buf_cstr(&b, family);
            if (params && params[0]) {
                if (b.len) agentc_buf_cstr(&b, " ");
                agentc_buf_cstr(&b, params);
            }
            d->detail = agentc_strdup((const char *)b.p);
            agentc_buf_free(&b);
        }
        n++;
    }
    return n;
}

/* Gemini native: {"models":[{"name":"models/gemini-2.5-flash","displayName":…,
 * "inputTokenLimit":…,"outputTokenLimit":…,"supportedGenerationMethods":[…]}]}.
 * The name carries a "models/" prefix that is stripped so the id is what the
 * generateContent path takes. */
static size_t parse_google(const char *provider, const AgcJson *root, AgcDiscovered *out,
                           size_t max) {
    (void)provider;
    const AgcJson *models = agentc_json_get(root, "models");
    if (agentc_json_type(models) != AGENTC_JSON_ARR) return 0;
    size_t n = 0;
    size_t total = agentc_json_len(models);
    for (size_t i = 0; i < total && n < max; i++) {
        const AgcJson *m = agentc_json_at(models, i);
        if (agentc_json_type(m) != AGENTC_JSON_OBJ) continue;
        const char *name = agentc_json_get_str(m, "name");
        if (!name || !name[0]) continue;
        const char *id = name;
        if (agentc_str_starts(name, agentc_strlen(name), "models/")) id = name + 7;
        if (!id[0]) continue;
        AgcDiscovered *d = &out[n];
        agentc_memset(d, 0, sizeof *d);
        d->id = agentc_strdup(id);
        const char *display = agentc_json_get_str(m, "displayName");
        d->name = agentc_strdup(display && display[0] ? display : id);
        d->ctx_window = (u32)agentc_json_get_int(m, "inputTokenLimit", 0);
        d->max_tokens = (u32)agentc_json_get_int(m, "outputTokenLimit", 0);
        /* no capability flag in the response; the 2.5/3 generations think */
        d->reasoning = agentc_str_str(id, "gemini-2.5") != NULL ||
                       agentc_str_str(id, "gemini-3") != NULL ||
                       agentc_str_str(id, "thinking") != NULL;
        n++;
    }
    return n;
}

/* ChatGPT Codex backend list, e.g.
 * {"models":[{"slug":"gpt-5.6-sol","display_name":"GPT-5.6-Sol",
 * "default_reasoning_level":"medium","supported_reasoning_levels":[...]}]}.
 * The id is `slug`; a row the backend marks as not API-supported is skipped. */
static size_t parse_codex(const AgcJson *root, AgcDiscovered *out, size_t max) {
    const AgcJson *models = agentc_json_get(root, "models");
    if (agentc_json_type(models) != AGENTC_JSON_ARR) models = agentc_json_get(root, "data");
    if (agentc_json_type(models) != AGENTC_JSON_ARR) return 0;
    size_t n = 0;
    size_t total = agentc_json_len(models);
    for (size_t i = 0; i < total && n < max; i++) {
        const AgcJson *m = agentc_json_at(models, i);
        if (agentc_json_type(m) != AGENTC_JSON_OBJ) continue;
        if (agentc_json_get(m, "supported_in_api") != NULL &&
            !agentc_json_get_bool(m, "supported_in_api", true))
            continue;
        const char *slug = agentc_json_get_str(m, "slug");
        if (!slug || !slug[0]) slug = agentc_json_get_str(m, "id");
        if (!slug || !slug[0]) slug = agentc_json_get_str(m, "name");
        if (!slug || !slug[0]) continue;
        AgcDiscovered *d = &out[n];
        agentc_memset(d, 0, sizeof *d);
        d->id = agentc_strdup(slug);
        const char *disp = agentc_json_get_str(m, "display_name");
        d->name = agentc_strdup(disp && disp[0] ? disp : slug);
        /* the backend reports the reasoning levels it accepts, not a ctx window */
        d->reasoning = agentc_json_get(m, "supported_reasoning_levels") != NULL ||
                       agentc_json_get_str(m, "default_reasoning_level") != NULL;
        n++;
    }
    return n;
}

/* ------------------------------------------------------- endpoints */
static void trim_v1(const char *base, char *out, size_t cap) {
    agentc_snprintf(out, cap, "%s", base ? base : "");
    size_t n = agentc_strlen(out);
    while (n && out[n - 1] == '/') out[--n] = 0;
    if (n >= 3 && agentc_streq(out + n - 3, "/v1")) out[n - 3] = 0;
}

/* The discovery engine, driven by an explicit provider row so the two `openai`
 * wires (chat vs Codex) resolve correctly. */
static size_t discover_impl(const AgcProviderOps *ops, const char *provider,
                            const char *base_url, const char *api_key, AgcDiscovered **out,
                            size_t max, int timeout_ms, char *err, size_t err_cap) {
    if (out) *out = NULL;
    if (!provider || !base_url || !base_url[0]) {
        set_err(err, err_cap, "no base url");
        return 0;
    }
    if (max == 0 || max > DISCOVER_MAX) max = DISCOVER_MAX;
    int style = ops ? ops->discover_style : AGENTC_DISCOVER_DEFAULT;
    if (style == AGENTC_DISCOVER_NONE) {
        set_err(err, err_cap, "no listing");
        return 0;
    }
    /* The row owns its discovery auth style: an extension row may use a
     * non-Bearer header (e.g. X-Api-Key) or none at all. A row with the hook
     * gets no Bearer fallback, so its empty override is authoritative and an
     * AUTH_NONE row never leaks the key to the wire. */
    AgcBuf auth = { 0 };
    const char *fallback = api_key;
    if (ops && ops->auth_headers) {
        ops->auth_headers(ops, &auth, api_key);
        fallback = NULL;
    }
    AgcDiscovered *arr = agentc_alloc(max * sizeof *arr);
    size_t n = 0;
    char url[1200];
    AgcBuf body = { 0 };
    int rc = -1;
    AgcJsonArena *ja = agentc_json_arena_new(0);

    if (style == AGENTC_DISCOVER_OLLAMA) {
        /* native API first: it carries family/parameter size */
        char host[1100];
        trim_v1(base_url, host, sizeof host);
        agentc_snprintf(url, sizeof url, "%s/api/tags", host);
        rc = http_get(url, fallback, &auth, NULL, &body, timeout_ms, err, err_cap);
        if (rc == 0) {
            AgcJson *root = agentc_json_parse_in(ja, (const char *)body.p, body.len);
            n = parse_ollama_tags(root, arr, max);
        }
        if (n == 0) {
            agentc_buf_clear(&body);
            agentc_snprintf(url, sizeof url, "%s/v1/models", host);
            rc = http_get(url, fallback, &auth, NULL, &body, timeout_ms, err, err_cap);
            if (rc == 0) {
                AgcJson *root = agentc_json_parse_in(ja, (const char *)body.p, body.len);
                n = parse_openai(provider, root, arr, max);
            }
        }
    } else if (style == AGENTC_DISCOVER_GOOGLE) {
        agentc_snprintf(url, sizeof url, "%s/models", base_url);
        rc = http_get(url, fallback, &auth, NULL, &body, timeout_ms, err, err_cap);
        if (rc == 0) {
            AgcJson *root = agentc_json_parse_in(ja, (const char *)body.p, body.len);
            n = parse_google(provider, root, arr, max);
        }
    } else if (style == AGENTC_DISCOVER_CODEX) {
        agentc_snprintf(url, sizeof url, "%s/models", base_url);
        rc = http_get(url, fallback, &auth, NULL, &body, timeout_ms, err, err_cap);
        if (rc == 0) {
            AgcJson *root = agentc_json_parse_in(ja, (const char *)body.p, body.len);
            n = parse_codex(root, arr, max);
        }
    } else {
        if (style == AGENTC_DISCOVER_ANTHROPIC) {
            /* the catalog base has no /v1; tolerate a user-supplied one that has */
            char root[1100];
            trim_v1(base_url, root, sizeof root);
            agentc_snprintf(url, sizeof url, "%s/v1/models", root);
            rc = http_get(url, fallback, &auth, "anthropic-version: 2023-06-01\r\n", &body,
                          timeout_ms, err, err_cap);
        } else {
            agentc_snprintf(url, sizeof url, "%s/models", base_url);
            rc = http_get(url, fallback, &auth, NULL, &body, timeout_ms, err, err_cap);
        }
        if (rc == 0) {
            AgcJson *root = agentc_json_parse_in(ja, (const char *)body.p, body.len);
            n = parse_openai(provider, root, arr, max);
        }
    }
    agentc_json_arena_free(ja);
    agentc_buf_free(&body);
    agentc_buf_free(&auth);
    if (n == 0) {
        agentc_discover_free(arr, 0);
        if (err && !err[0]) set_err(err, err_cap, "no models found");
        return 0;
    }
    if (out) {
        *out = arr;
    } else {
        /* count-only call: the caller only wants `n`, so do not leak the array */
        agentc_discover_free(arr, n);
    }
    return n;
}

/* Public entry point: resolve the row by name (unique names are unambiguous). */
size_t agentc_discover_models(const char *provider, const char *base_url, const char *api_key,
                          AgcDiscovered **out, size_t max, int timeout_ms, char *err,
                          size_t err_cap) {
    return discover_impl(agentc_provider_by_name(provider), provider, base_url, api_key, out,
                         max, timeout_ms, err, err_cap);
}

/* Row-driven entry point for callers that already selected a provider (setup),
 * so the `openai` chat/Codex ambiguity is resolved by the chosen row. */
size_t agentc_discover_models_ops(const AgcProviderOps *ops, const char *base_url,
                                  const char *api_key, AgcDiscovered **out, size_t max,
                                  int timeout_ms, char *err, size_t err_cap) {
    return discover_impl(ops, ops ? ops->name : NULL, base_url, api_key, out, max, timeout_ms,
                         err, err_cap);
}

void agentc_discover_free(AgcDiscovered *m, size_t n) {
    if (!m) return;
    for (size_t i = 0; i < n; i++) {
        agentc_free(m[i].id);
        agentc_free(m[i].name);
        agentc_free(m[i].detail);
    }
    agentc_free(m);
}

/* ---------------------------------------------------------- cache */
const char *agentc_discover_cache_path(char *buf, size_t cap) {
    char base[4096];
    if (!agentc_config_home(base, sizeof base)) return NULL;
    int n = agentc_snprintf(buf, cap, "%s/%s", base, CACHE_FILE);
    return (n > 0 && (size_t)n < cap) ? buf : NULL;
}

typedef struct {
    AgcDiscovered *arr;
    size_t n, max;
} LoadCtx;

static void cache_parse_models(const AgcJson *models, LoadCtx *c) {
    if (agentc_json_type(models) != AGENTC_JSON_ARR) return;
    size_t total = agentc_json_len(models);
    for (size_t i = 0; i < total && c->n < c->max; i++) {
        const AgcJson *m = agentc_json_at(models, i);
        if (agentc_json_type(m) != AGENTC_JSON_OBJ) continue;
        const char *id = agentc_json_get_str(m, "id");
        if (!id || !id[0]) continue;
        AgcDiscovered *d = &c->arr[c->n++];
        agentc_memset(d, 0, sizeof *d);
        d->id = agentc_strdup(id);
        const char *name = agentc_json_get_str(m, "name");
        d->name = agentc_strdup(name && name[0] ? name : id);
        const char *detail = agentc_json_get_str(m, "detail");
        if (detail && detail[0]) d->detail = agentc_strdup(detail);
        d->ctx_window = (u32)agentc_json_get_int(m, "ctx", 0);
        d->max_tokens = (u32)agentc_json_get_int(m, "max", 0);
        d->reasoning = agentc_json_get_bool(m, "reasoning", false);
        d->image = agentc_json_get_bool(m, "image", false);
    }
}

size_t agentc_discover_cache_load(const char *provider, const char *base_url, AgcDiscovered **out,
                              size_t max, i64 *fetched_ms) {
    if (out) *out = NULL;
    if (fetched_ms) *fetched_ms = 0;
    if (!provider) return 0;
    char path[4160];
    if (!agentc_discover_cache_path(path, sizeof path)) return 0;
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text) return 0;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(ja, text, len);
    if (max == 0 || max > DISCOVER_MAX) max = DISCOVER_MAX;
    AgcDiscovered *arr = agentc_alloc(max * sizeof *arr);
    LoadCtx c = { arr, 0, max };
    const AgcJson *entry = agentc_json_get(root, provider);
    if (agentc_json_type(entry) == AGENTC_JSON_OBJ) {
        /* The cache is only valid for the endpoint it was fetched from: a base
         * URL change (a different Ollama host, a proxy) must not surface models
         * that may not exist there. An older entry without "base" is accepted. */
        const char *cached_base = agentc_json_get_str(entry, "base");
        bool same_base = !(base_url && base_url[0] && cached_base && cached_base[0]) ||
                         agentc_streq(base_url, cached_base);
        if (same_base) {
            if (fetched_ms) *fetched_ms = agentc_json_get_int(entry, "fetched", 0);
            cache_parse_models(agentc_json_get(entry, "models"), &c);
        }
    }
    agentc_json_arena_free(ja);
    agentc_free(text);
    if (c.n == 0) {
        agentc_discover_free(arr, 0);
        return 0;
    }
    if (out) {
        *out = arr;
    } else {
        agentc_discover_free(arr, c.n);
    }
    return c.n;
}

int agentc_discover_cache_save(const char *provider, const char *base_url, const AgcDiscovered *m,
                           size_t n) {
    if (!provider) return -22;
    char path[4160];
    if (!agentc_discover_cache_path(path, sizeof path)) return -2;
    (void)agentc_mkdir_parents(path);

    /* merge: keep other providers' entries verbatim */
    size_t old_len = 0;
    char *old = agentc_read_file_owned(path, &old_len);
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = old ? agentc_json_parse_in(ja, old, old_len) : NULL;

    AgcBuf out = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &out);
    agentc_jsonw_obj(&w);
    if (agentc_json_type(root) == AGENTC_JSON_OBJ) {
        for (size_t i = 0; i < (size_t)-1; i++) {
            const AgcJson *k = agentc_json_key_at(root, i);
            if (!k) break;
            size_t klen = 0;
            const char *name = agentc_json_str(k, &klen);
            if (name && agentc_streq(name, provider)) continue;   /* replaced below */
            agentc_jsonw_key(&w, name ? name : "");
            agentc_json_emit(&w, agentc_json_val_at(root, i));
        }
    }
    agentc_jsonw_key(&w, provider);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "fetched");
    agentc_jsonw_i64(&w, os_now_ns(OS_CLOCK_REALTIME) / 1000000);
    agentc_jsonw_key(&w, "base");
    agentc_jsonw_cstr(&w, base_url ? base_url : "");
    agentc_jsonw_key(&w, "models");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < n; i++) {
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "id");
        agentc_jsonw_cstr(&w, m[i].id ? m[i].id : "");
        agentc_jsonw_key(&w, "name");
        agentc_jsonw_cstr(&w, m[i].name ? m[i].name : (m[i].id ? m[i].id : ""));
        if (m[i].detail && m[i].detail[0]) {
            agentc_jsonw_key(&w, "detail");
            agentc_jsonw_cstr(&w, m[i].detail);
        }
        agentc_jsonw_key(&w, "ctx");
        agentc_jsonw_u64(&w, m[i].ctx_window);
        agentc_jsonw_key(&w, "max");
        agentc_jsonw_u64(&w, m[i].max_tokens);
        agentc_jsonw_key(&w, "reasoning");
        agentc_jsonw_bool(&w, m[i].reasoning);
        agentc_jsonw_key(&w, "image");
        agentc_jsonw_bool(&w, m[i].image);
        agentc_jsonw_end(&w);
    }
    agentc_jsonw_end(&w);   /* models array */
    agentc_jsonw_end(&w);   /* provider object */
    agentc_jsonw_end(&w);   /* root object */
    agentc_buf_byte(&out, '\n');
    agentc_json_arena_free(ja);
    agentc_free(old);

    int rc = agentc_write_file_atomic(path, out.p, out.len, 0600);
    agentc_buf_free(&out);
    return rc;
}

void agentc_discover_register(const char *provider, const char *api, const char *base_url,
                          const AgcDiscovered *m, size_t n) {
    for (size_t i = 0; i < n; i++) {
        agentc_model_register_dynamic(provider, m[i].id, api, base_url, m[i].ctx_window,
                                  m[i].max_tokens, m[i].reasoning, m[i].image);
    }
}

/* ------------------------------------------------- openrouter pricing sync */

/* Render a micro-USD-per-million rate as a USD-per-million JSON number literal
 * (shortest exact decimal). */
static int fmt_usd_per_million(char *buf, size_t cap, i64 micro) {
    if (micro < 0) micro = 0;
    i64 whole = micro / 1000000;
    i64 frac = micro % 1000000;
    char fs[6];
    for (int i = 5; i >= 0; i--) {
        fs[i] = (char)('0' + (int)(frac % 10));
        frac /= 10;
    }
    int fl = 6;
    while (fl > 0 && fs[fl - 1] == '0') fl--;
    int n = agentc_snprintf(buf, cap, "%lld", (long long)whole);
    if (n < 0 || (size_t)n >= cap) return -1;
    if (fl == 0) return n;
    if ((size_t)n + 1 + (size_t)fl >= cap) return -1;
    buf[n++] = '.';
    for (int i = 0; i < fl; i++) buf[n++] = fs[i];
    buf[n] = 0;
    return n;
}

static void jsonw_usd(AgcJsonW *w, const char *key, i64 micro) {
    char tmp[40];
    int n = fmt_usd_per_million(tmp, sizeof tmp, micro);
    agentc_jsonw_key(w, key);
    if (n < 0) {
        agentc_jsonw_i64(w, 0);
        return;
    }
    agentc_jsonw_raw(w, tmp, (size_t)n);
}

int agentc_pricing_sync_openrouter(char *err, size_t cap) {
    if (err && cap) err[0] = 0;
    AgcBuf body = { 0 };
    int rc = http_get("https://openrouter.ai/api/v1/models", NULL, NULL, NULL, &body, 10000,
                      err, cap);
    if (rc != 0) {
        agentc_buf_free(&body);
        return rc;
    }
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(ja, (const char *)body.p, body.len);
    const AgcJson *data = agentc_json_get(root, "data");
    if (agentc_json_type(data) != AGENTC_JSON_ARR) {
        set_err(err, cap, "malformed models response");
        agentc_json_arena_free(ja);
        agentc_buf_free(&body);
        return -22;
    }
    char base[4096];
    char path[4160];
    const char *ch = agentc_config_home(base, sizeof base);
    if (!ch || !agentc_path_join(path, sizeof path, ch, "pricing.jsonc")) {
        set_err(err, cap, "no config home");
        agentc_json_arena_free(ja);
        agentc_buf_free(&body);
        return -2;
    }
    /* read the existing file and preserve every key this sync does not manage */
    size_t old_len = 0;
    char *old = agentc_read_file_owned(path, &old_len);
    AgcJsonArena *ja_old = agentc_json_arena_new(0);
    AgcJson *oldroot = old ? agentc_json_parse_in(ja_old, old, old_len) : NULL;

    AgcBuf out = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &out);
    agentc_jsonw_obj(&w);
    if (agentc_json_type(oldroot) == AGENTC_JSON_OBJ) {
        for (size_t i = 0;; i++) {
            const AgcJson *k = agentc_json_key_at(oldroot, i);
            if (!k) break;
            size_t klen = 0;
            const char *name = agentc_json_str(k, &klen);
            if (!name) continue;
            if (agentc_str_starts(name, klen, "openrouter/")) continue;   /* replaced */
            agentc_jsonw_key(&w, name);
            agentc_json_emit(&w, agentc_json_val_at(oldroot, i));
        }
    }
    size_t synced = 0;
    size_t total = agentc_json_len(data);
    for (size_t i = 0; i < total; i++) {
        const AgcJson *m = agentc_json_at(data, i);
        if (agentc_json_type(m) != AGENTC_JSON_OBJ) continue;
        const char *id = agentc_json_get_str(m, "id");
        if (!id || !id[0]) continue;
        const AgcJson *pricing = agentc_json_get(m, "pricing");
        if (agentc_json_type(pricing) != AGENTC_JSON_OBJ) continue;
        i64 in = pricing_usd_per_token(pricing, "prompt");
        i64 outr = pricing_usd_per_token(pricing, "completion");
        if (in <= 0 && outr <= 0) continue;
        i64 cr = pricing_usd_per_token(pricing, "input_cache_read");
        i64 cw = pricing_usd_per_token(pricing, "input_cache_write");
        AgcBuf key = { 0 };
        agentc_buf_cstr(&key, "openrouter/");
        agentc_buf_cstr(&key, id);
        agentc_jsonw_key(&w, key.p ? (const char *)key.p : "");
        agentc_jsonw_obj(&w);
        jsonw_usd(&w, "in", in);
        jsonw_usd(&w, "out", outr);
        jsonw_usd(&w, "cache_read", cr);
        jsonw_usd(&w, "cache_write", cw);
        agentc_jsonw_key(&w, "input_includes_cache");
        agentc_jsonw_bool(&w, false);
        agentc_jsonw_end(&w);
        agentc_buf_free(&key);
        synced++;
    }
    agentc_jsonw_end(&w);   /* root object */
    agentc_buf_byte(&out, '\n');
    agentc_json_arena_free(ja_old);
    agentc_free(old);
    agentc_json_arena_free(ja);
    agentc_buf_free(&body);

    (void)agentc_mkdir_parents(path);
    rc = agentc_write_file_atomic(path, out.p, out.len, 0600);
    agentc_buf_free(&out);
    if (rc < 0) return rc;
    (void)agentc_pricing_load(NULL, 0);   /* apply the freshly written rates */
    return (int)synced;
}
