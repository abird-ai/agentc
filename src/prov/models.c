/* models.c — static model catalog and cost accounting.
 *
 * Small hand-maintained subset: api, base URL, context window, max output
 * tokens and capability flags, plus per-MTok rates for agentc_model_cost().
 */
#include "agent.h"

/* Rates are micro-USD per million tokens (USD/M * 1e6). Anthropic input
 * excludes cache-read/creation tokens; OpenAI prompt_tokens includes cached
 * tokens, so AGENTC_MODEL_INPUT_INCLUDES_CACHE subtracts them before billing.
 * Rows without rates keep AGENTC_MODEL_RATE_KNOWN clear and cost -1. */
static AgcModel catalog[] = {
    { "anthropic", "claude-sonnet-4-5", "anthropic-messages", "https://api.anthropic.com",
      200000, 64000, true, true, 0, 3000000, 15000000, 300000, 3750000,
      AGENTC_MODEL_RATE_KNOWN },
    { "anthropic", "claude-haiku-4-5", "anthropic-messages", "https://api.anthropic.com",
      200000, 64000, true, true, 0, 1000000, 5000000, 100000, 1250000,
      AGENTC_MODEL_RATE_KNOWN },
    { "anthropic", "claude-opus-4-1", "anthropic-messages", "https://api.anthropic.com",
      200000, 32000, true, true, 0, 15000000, 75000000, 1500000, 18750000,
      AGENTC_MODEL_RATE_KNOWN },
    { "openai", "gpt-5", "openai-chat", "https://api.openai.com/v1", 400000, 128000, true,
      true, 0, 1250000, 10000000, 625000, 0,
      AGENTC_MODEL_RATE_KNOWN | AGENTC_MODEL_INPUT_INCLUDES_CACHE },
    { "openai", "gpt-5-mini", "openai-chat", "https://api.openai.com/v1", 400000, 128000,
      true, true, 0, 250000, 2000000, 125000, 0,
      AGENTC_MODEL_RATE_KNOWN | AGENTC_MODEL_INPUT_INCLUDES_CACHE },
    { "openai", "gpt-4.1", "openai-chat", "https://api.openai.com/v1", 1000000, 32768,
      false, true, 0, 2000000, 8000000, 1000000, 0,
      AGENTC_MODEL_RATE_KNOWN | AGENTC_MODEL_INPUT_INCLUDES_CACHE },
    /* ChatGPT subscription (Codex backend, Responses API): no published rate */
    { "openai", "gpt-5-codex", "openai-codex-responses",
      "https://chatgpt.com/backend-api/codex", 400000, 128000, true, false, 0, 0, 0, 0, 0, 0 },
    { "openai", "codex-mini-latest", "openai-codex-responses",
      "https://chatgpt.com/backend-api/codex", 200000, 100000, true, false, 0, 0, 0, 0, 0, 0 },
    /* Native Gemini API (generateContent). Rates are the published 2.5 list
     * price in micro-USD per million tokens; promptTokenCount includes cached
     * tokens, hence AGENTC_MODEL_INPUT_INCLUDES_CACHE. */
    { "google", "gemini-2.5-pro", "google-generativeai",
      "https://generativelanguage.googleapis.com/v1beta", 1048576, 65536, true, true, 0,
      1250000, 10000000, 312500, 0,
      AGENTC_MODEL_RATE_KNOWN | AGENTC_MODEL_INPUT_INCLUDES_CACHE },
    { "google", "gemini-2.5-flash", "google-generativeai",
      "https://generativelanguage.googleapis.com/v1beta", 1048576, 65536, true, true, 0,
      300000, 2500000, 75000, 0,
      AGENTC_MODEL_RATE_KNOWN | AGENTC_MODEL_INPUT_INCLUDES_CACHE },
    { "google", "gemini-2.5-flash-lite", "google-generativeai",
      "https://generativelanguage.googleapis.com/v1beta", 1048576, 65536, true, true, 0,
      100000, 400000, 25000, 0,
      AGENTC_MODEL_RATE_KNOWN | AGENTC_MODEL_INPUT_INCLUDES_CACHE },
};

#define CATALOG_N (sizeof catalog / sizeof catalog[0])

/* Discovered models live here: the table is owned by the registry so the
 * AgcModel pointers handed out stay valid until clear. Static extension models
 * share the same 256 slots; the cap is logged once when the table fills. */
#define DYN_MAX 256
static AgcModel dyn[DYN_MAX];
static size_t dyn_n;
static bool dyn_full_logged;

/* ------------------------------------------------------------- rate plumbing */

/* Parse a non-negative decimal scalar and return value*scale (scale a power of
 * ten), or -1 on malformed input. Extra fractional digits past the scale are
 * truncated. Used for USD-per-million (scale 1e6) and USD-per-token (1e12). */
i64 agentc_rate_parse_scaled(const char *s, i64 scale) {
    if (!s || scale <= 0) return -1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '+') s++;
    if (*s == '-' || *s == 0) return -1;
    bool any = false;
    i64 ip = 0;
    while (*s >= '0' && *s <= '9') {
        if (ip > 1000000000LL) return -1;   /* absurd rate: refuse the overflow */
        ip = ip * 10 + (*s - '0');
        s++;
        any = true;
    }
    i64 frac = 0, fscale = 1;
    if (*s == '.') {
        s++;
        i64 sc = scale;
        int maxfd = 0;
        while (sc > 1 && maxfd < 18) { sc /= 10; maxfd++; }
        int fd = 0;
        while (*s >= '0' && *s <= '9') {
            if (fd < maxfd) {
                frac = frac * 10 + (*s - '0');
                fscale *= 10;
                fd++;
            }
            s++;
            any = true;
        }
    }
    if (!any) return -1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s != 0) return -1;
    /* ip*scale must not overflow (a hostile file could carry a huge integer) */
    if (ip > (i64)9000000000000000000LL / scale) return -1;
    return ip * scale + frac * (scale / fscale);
}

/* Rates parsed from a provider /models response, keyed provider+id and consumed
 * when agentc_model_register_dynamic() installs the matching row. */
typedef struct {
    char *provider, *id;
    i64 in_rate, out_rate, cache_read_rate, cache_write_rate;
    u32 rate_flags;
} StagedRate;
#define RATE_STAGE_MAX 256
static StagedRate staged[RATE_STAGE_MAX];
static size_t staged_n;

static StagedRate *stage_find(const char *provider, const char *id) {
    for (size_t i = 0; i < staged_n; i++)
        if (agentc_streq(staged[i].provider, provider) && agentc_streq(staged[i].id, id))
            return &staged[i];
    return NULL;
}

void agentc_model_stage_rate(const char *provider, const char *id, i64 in_rate, i64 out_rate,
                             i64 cache_read_rate, i64 cache_write_rate, u32 rate_flags) {
    if (!provider || !id || !id[0]) return;
    StagedRate *s = stage_find(provider, id);
    if (!s) {
        if (staged_n >= RATE_STAGE_MAX) return;
        s = &staged[staged_n++];
        s->provider = agentc_strdup(provider);
        s->id = agentc_strdup(id);
    }
    s->in_rate = in_rate;
    s->out_rate = out_rate;
    s->cache_read_rate = cache_read_rate;
    s->cache_write_rate = cache_write_rate;
    s->rate_flags = rate_flags;
}

/* Local pricing.jsonc overrides: the highest-precedence source, consulted by
 * agentc_model_cost() before any model field. An id-only key (provider NULL)
 * matches any provider. */
typedef struct {
    char *provider;   /* NULL = id-only key */
    char *id;
    i64 in_rate, out_rate, cache_read_rate, cache_write_rate;
    u32 rate_flags;
} RateOverride;
#define RATE_OVERRIDE_MAX 256
static RateOverride overrides[RATE_OVERRIDE_MAX];
static size_t overrides_n;

static bool override_matches(const RateOverride *o, const char *provider, const char *id) {
    if (!agentc_streq(o->id, id)) return false;
    return o->provider == NULL || (provider && agentc_streq(o->provider, provider));
}

static const RateOverride *override_find(const char *provider, const char *id) {
    if (!id) return NULL;
    /* a provider-qualified key wins over an id-only key */
    for (size_t i = 0; i < overrides_n; i++)
        if (overrides[i].provider && override_matches(&overrides[i], provider, id))
            return &overrides[i];
    for (size_t i = 0; i < overrides_n; i++)
        if (!overrides[i].provider && override_matches(&overrides[i], provider, id))
            return &overrides[i];
    return NULL;
}

static bool rate_model_matches(const AgcModel *m, const char *provider, const char *id) {
    if (!agentc_streq(m->id, id)) return false;
    return provider == NULL || agentc_streq(m->provider, provider);
}

static void rate_apply(AgcModel *m, i64 in_rate, i64 out_rate, i64 cache_read_rate,
                       i64 cache_write_rate, u32 rate_flags) {
    m->in_rate = in_rate;
    m->out_rate = out_rate;
    m->cache_read_rate = cache_read_rate;
    m->cache_write_rate = cache_write_rate;
    m->rate_flags = rate_flags;
}

void agentc_model_set_rate_override(const char *provider, const char *id, i64 in_rate,
                                    i64 out_rate, i64 cache_read_rate, i64 cache_write_rate,
                                    u32 rate_flags) {
    if (!id || !id[0]) return;
    RateOverride *o = NULL;
    for (size_t i = 0; i < overrides_n; i++) {
        bool prov_eq = (overrides[i].provider == NULL && provider == NULL) ||
                       (overrides[i].provider && provider &&
                        agentc_streq(overrides[i].provider, provider));
        if (prov_eq && agentc_streq(overrides[i].id, id)) { o = &overrides[i]; break; }
    }
    if (!o) {
        if (overrides_n >= RATE_OVERRIDE_MAX) return;
        o = &overrides[overrides_n++];
        o->provider = provider ? agentc_strdup(provider) : NULL;
        o->id = agentc_strdup(id);
    }
    o->in_rate = in_rate;
    o->out_rate = out_rate;
    o->cache_read_rate = cache_read_rate;
    o->cache_write_rate = cache_write_rate;
    o->rate_flags = rate_flags;
    /* mirror onto any registered row so a caller reading AgcModel sees the
     * same effective rate agentc_model_cost() bills */
    for (size_t i = 0; i < CATALOG_N; i++)
        if (rate_model_matches(&catalog[i], provider, id))
            rate_apply(&catalog[i], in_rate, out_rate, cache_read_rate, cache_write_rate,
                       rate_flags);
    for (size_t i = 0; i < dyn_n; i++)
        if (rate_model_matches(&dyn[i], provider, id))
            rate_apply(&dyn[i], in_rate, out_rate, cache_read_rate, cache_write_rate,
                       rate_flags);
}

/* Effective rate for a newly registered row: a local override wins, otherwise
 * consume any staged discovered rate. */
static void model_apply_rate(AgcModel *m) {
    const RateOverride *o = override_find(m->provider, m->id);
    if (o) {
        rate_apply(m, o->in_rate, o->out_rate, o->cache_read_rate, o->cache_write_rate,
                   o->rate_flags);
        return;
    }
    StagedRate *s = stage_find(m->provider, m->id);
    if (!s) return;
    rate_apply(m, s->in_rate, s->out_rate, s->cache_read_rate, s->cache_write_rate,
               s->rate_flags);
    agentc_free(s->provider);
    agentc_free(s->id);
    *s = staged[--staged_n];
}

/* Shared insert/update behind the dynamic and static entry points. The flags
 * argument carries AGENTC_MODEL_STATIC for static registrations; an existing
 * row keeps its bit (a dynamic update never un-statics an extension model). */
static void model_register(const char *provider, const char *id, const char *api,
                           const char *base_url, u32 ctx_window, u32 max_tokens,
                           bool reasoning, bool image, u32 flags) {
    if (!provider || !id || !id[0]) return;
    for (size_t i = 0; i < dyn_n; i++) {
        if (agentc_streq(dyn[i].provider, provider) && agentc_streq(dyn[i].id, id)) {
            dyn[i].ctx_window = ctx_window;
            dyn[i].max_tokens = max_tokens;
            dyn[i].reasoning = reasoning;
            dyn[i].image = image;
            dyn[i].flags |= flags;
            /* api and base_url are part of the identity a caller may route on,
             * so a re-register (e.g. after a base-URL change) refreshes them. */
            if (api && !agentc_streq(dyn[i].api, api)) {
                agentc_free((void *)dyn[i].api);
                dyn[i].api = agentc_strdup(api);
            }
            const char *bu = base_url ? base_url : "";
            if (!agentc_streq(dyn[i].base_url, bu)) {
                agentc_free((void *)dyn[i].base_url);
                dyn[i].base_url = agentc_strdup(bu);
            }
            model_apply_rate(&dyn[i]);
            return;
        }
    }
    if (dyn_n >= DYN_MAX) {
        if (!dyn_full_logged) {
            agentc_logf(2, "model: registry full");
            dyn_full_logged = true;
        }
        return;
    }
    AgcModel *m = &dyn[dyn_n];
    m->provider = agentc_strdup(provider);
    m->id = agentc_strdup(id);
    m->api = agentc_strdup(api ? api : "openai-chat");
    m->base_url = agentc_strdup(base_url ? base_url : "");
    m->ctx_window = ctx_window;
    m->max_tokens = max_tokens;
    m->reasoning = reasoning;
    m->image = image;
    m->flags = flags;
    m->in_rate = m->out_rate = m->cache_read_rate = m->cache_write_rate = 0;
    m->rate_flags = 0;
    dyn_n++;
    model_apply_rate(m);
}

void agentc_model_register_dynamic(const char *provider, const char *id, const char *api,
                               const char *base_url, u32 ctx_window, u32 max_tokens,
                               bool reasoning, bool image) {
    model_register(provider, id, api, base_url, ctx_window, max_tokens, reasoning,
                   image, 0);
}

void agentc_model_register_static(const char *provider, const char *id, const char *api,
                               const char *base_url, u32 ctx_window, u32 max_tokens,
                               bool reasoning, bool image) {
    model_register(provider, id, api, base_url, ctx_window, max_tokens, reasoning,
                   image, AGENTC_MODEL_STATIC);
}

void agentc_model_clear_dynamic(const char *provider) {
    size_t keep = 0;
    for (size_t i = 0; i < dyn_n; i++) {
        bool match = !provider || agentc_streq(dyn[i].provider, provider);
        /* Static rows are owned by their extension provider and survive a
         * discovery refresh; agentc_model_clear_static() drops them. */
        if (match && !(dyn[i].flags & AGENTC_MODEL_STATIC)) {
            agentc_free((void *)dyn[i].provider);
            agentc_free((void *)dyn[i].id);
            agentc_free((void *)dyn[i].api);
            agentc_free((void *)dyn[i].base_url);
            continue;
        }
        if (keep != i) dyn[keep] = dyn[i];
        keep++;
    }
    dyn_n = keep;
}

void agentc_model_clear_static(const char *provider) {
    size_t keep = 0;
    for (size_t i = 0; i < dyn_n; i++) {
        bool match = (!provider || agentc_streq(dyn[i].provider, provider)) &&
                     (dyn[i].flags & AGENTC_MODEL_STATIC);
        if (match) {
            agentc_free((void *)dyn[i].provider);
            agentc_free((void *)dyn[i].id);
            agentc_free((void *)dyn[i].api);
            agentc_free((void *)dyn[i].base_url);
            continue;
        }
        if (keep != i) dyn[keep] = dyn[i];
        keep++;
    }
    dyn_n = keep;
}

size_t agentc_model_dynamic_count(void) { return dyn_n; }

bool agentc_model_is_dynamic(const AgcModel *m) {
    return m >= dyn && m < dyn + dyn_n;
}

bool agentc_model_is_static(const AgcModel *m) {
    return m && agentc_model_is_dynamic(m) && (m->flags & AGENTC_MODEL_STATIC);
}

const AgcModel *agentc_model_find(const char *provider, const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < CATALOG_N; i++) {
        if (provider && !agentc_streq(provider, catalog[i].provider)) continue;
        if (agentc_streq(id, catalog[i].id)) return &catalog[i];
    }
    for (size_t i = 0; i < dyn_n; i++) {
        if (provider && !agentc_streq(provider, dyn[i].provider)) continue;
        if (agentc_streq(id, dyn[i].id)) return &dyn[i];
    }
    return NULL;
}

size_t agentc_model_all(const AgcModel **out, size_t max) {
    size_t total = CATALOG_N + dyn_n;
    if (out == NULL) return total;   /* count-query form, like agentc_tools_builtin */
    size_t n = 0;
    for (size_t i = 0; i < CATALOG_N && n < max; i++) out[n++] = &catalog[i];
    for (size_t i = 0; i < dyn_n && n < max; i++) out[n++] = &dyn[i];
    return n;
}

/* Billing: billable input is the fresh input only. For a provider whose
 * usage.input already counts cache reads (OpenAI prompt_tokens includes
 * cached_tokens) subtract them so they are billed once, at the cache rate;
 * Anthropic input already excludes cache read/creation. Returns the cost in
 * micro-USD, or -1 when the model's rate is unknown (no rate, no override). */
i64 agentc_model_cost(const AgcModel *m, const AgcUsage *u) {
    if (!m || !u) return -1;
    i64 in_rate, out_rate, cache_read_rate, cache_write_rate;
    u32 flags;
    const RateOverride *o = override_find(m->provider, m->id);
    if (o) {
        in_rate = o->in_rate;
        out_rate = o->out_rate;
        cache_read_rate = o->cache_read_rate;
        cache_write_rate = o->cache_write_rate;
        flags = o->rate_flags;
    } else if (m->rate_flags & AGENTC_MODEL_RATE_KNOWN) {
        in_rate = m->in_rate;
        out_rate = m->out_rate;
        cache_read_rate = m->cache_read_rate;
        cache_write_rate = m->cache_write_rate;
        flags = m->rate_flags;
    } else {
        return -1;
    }
    i64 billable_input = (i64)u->input;
    if (flags & AGENTC_MODEL_INPUT_INCLUDES_CACHE)
        billable_input = (i64)u->input > (i64)u->cache_read
                             ? (i64)u->input - (i64)u->cache_read
                             : 0;
    i64 micro = billable_input * in_rate + (i64)u->output * out_rate +
                (i64)u->cache_read * cache_read_rate +
                (i64)u->cache_write * cache_write_rate;
    return micro / 1000000;
}
