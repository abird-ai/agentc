/* provider.c — provider descriptor registry and shared stream lifecycle
 * (see prov/provider.h).
 *
 * The registry is an append-only table of row pointers, seeded lazily with the
 * five builtin adapters.  Rows are keyed by name (names may share an api); the
 * handle pointer on a row is created once from agentc_alloc so callers may hold
 * an AgcProvider for the process lifetime.
 *
 * The adapter stream hooks are dispatched through a small open-stream table:
 * agentc_stream_state_close() only sees the state pointer, so init records the
 * row that opened the stream and close pops it again.  The table keeps nested
 * and sequential streams correct and is bounded by the number of live streams.
 */
#include "prov/provider.h"

extern AgcProviderOps agentc_anthropic_ops;
extern AgcProviderOps agentc_openai_ops;
extern AgcProviderOps agentc_ollama_ops;
extern AgcProviderOps agentc_ollama_cloud_ops;
extern AgcProviderOps agentc_openai_codex_ops;
extern AgcProviderOps agentc_google_ops;

/* ---------------------------------------------------------------- registry */

static AgcProviderOps **g_reg;
static size_t g_n;
static size_t g_cap;
static bool   g_ready;
static size_t g_seeded;

/* Open-stream table (defined with the lifecycle functions below): lives here so
 * registry_reset can release its storage at the baseline. */
typedef struct {
    AgcStreamState *st;
    const AgcProviderOps *ops;
} OpenStream;

static OpenStream *g_open;
static size_t g_open_n;
static size_t g_open_cap;

static AgcProviderOps *const g_builtins[] = {
    &agentc_anthropic_ops,
    &agentc_openai_ops,
    &agentc_ollama_ops,
    &agentc_ollama_cloud_ops,
    &agentc_openai_codex_ops,
    &agentc_google_ops,
};

#define BUILTIN_N (sizeof g_builtins / sizeof g_builtins[0])

/* OpenAI-compatible presets.  The adapter hooks are copied from the openai row
 * when the preset is first materialized, so the wire protocol has exactly one
 * implementation; the rows are static, so their handles stay process-stable. */
static AgcProviderOps g_presets[] = {
    { .name = "openrouter",
      .api = "openai-chat",
      .path = "/chat/completions",
      .default_base_url = "https://openrouter.ai/api/v1",
      .env_keys = { "OPENROUTER_API_KEY", NULL, NULL },
      .needs_key = 1,
      .discover_style = AGENTC_DISCOVER_DEFAULT,
      .max_tokens_key = "max_tokens" },
    { .name = "xai",
      .api = "openai-chat",
      .path = "/chat/completions",
      .default_base_url = "https://api.x.ai/v1",
      .env_keys = { "XAI_API_KEY", NULL, NULL },
      .needs_key = 1,
      .discover_style = AGENTC_DISCOVER_DEFAULT,
      .max_tokens_key = "max_tokens" },
    { .name = "deepseek",
      .api = "openai-chat",
      .path = "/chat/completions",
      .default_base_url = "https://api.deepseek.com/v1",
      .env_keys = { "DEEPSEEK_API_KEY", NULL, NULL },
      .needs_key = 1,
      .discover_style = AGENTC_DISCOVER_DEFAULT,
      .max_tokens_key = "max_tokens" },
    { .name = "groq",
      .api = "openai-chat",
      .path = "/chat/completions",
      .default_base_url = "https://api.groq.com/openai/v1",
      .env_keys = { "GROQ_API_KEY", NULL, NULL },
      .needs_key = 1,
      .discover_style = AGENTC_DISCOVER_DEFAULT,
      .max_tokens_key = "max_tokens" },
    { .name = "mistral",
      .api = "openai-chat",
      .path = "/chat/completions",
      .default_base_url = "https://api.mistral.ai/v1",
      .env_keys = { "MISTRAL_API_KEY", NULL, NULL },
      .needs_key = 1,
      .discover_style = AGENTC_DISCOVER_DEFAULT,
      .max_tokens_key = "max_tokens" },
    { .name = "together",
      .api = "openai-chat",
      .path = "/chat/completions",
      .default_base_url = "https://api.together.ai/v1",
      .env_keys = { "TOGETHER_API_KEY", NULL, NULL },
      .needs_key = 1,
      .discover_style = AGENTC_DISCOVER_DEFAULT,
      .max_tokens_key = "max_tokens" },
    { .name = "gemini",
      .api = "openai-chat",
      .path = "/chat/completions",
      .default_base_url = "https://generativelanguage.googleapis.com/v1beta/openai",
      .env_keys = { "GEMINI_API_KEY", "GOOGLE_API_KEY", NULL },
      .needs_key = 1,
      .discover_style = AGENTC_DISCOVER_DEFAULT,
      .max_tokens_key = "max_tokens" },
};

#define PRESET_N (sizeof g_presets / sizeof g_presets[0])

static bool is_builtin(const AgcProviderOps *ops) {
    for (size_t i = 0; i < BUILTIN_N; i++)
        if (g_builtins[i] == ops) return true;
    return false;
}

static AgcProviderOps *preset_by_name(const char *name) {
    for (size_t i = 0; i < PRESET_N; i++)
        if (agentc_streq(g_presets[i].name, name)) return &g_presets[i];
    return NULL;
}

/* Register the matching preset on first by-name lookup.  This is what makes a
 * preset resolvable before any setup call (and by auth/discovery, which resolve
 * the provider row themselves). */
static const AgcProviderOps *preset_materialize(const char *name) {
    AgcProviderOps *row = preset_by_name(name);
    if (!row) return NULL;
    row->build_request = agentc_openai_ops.build_request;
    row->map_sse = agentc_openai_ops.map_sse;
    row->finish = agentc_openai_ops.finish;
    row->stream_open = agentc_openai_ops.stream_open;
    row->stream_close = agentc_openai_ops.stream_close;
    if (agentc_provider_register(row) < 0) return NULL;
    return row;
}

static int registry_push(AgcProviderOps *ops) {
    if (g_n == g_cap) {
        size_t cap = g_cap ? g_cap * 2 : 8;
        AgcProviderOps **grown = agentc_alloc_try(cap * sizeof *grown);
        if (!grown) return -12;                       /* ENOMEM */
        if (g_n) agentc_memcpy(grown, g_reg, g_n * sizeof *grown);
        agentc_free(g_reg);
        g_reg = grown;
        g_cap = cap;
    }
    g_reg[g_n] = ops;
    return (int) g_n++;
}

/* Populate the builtins exactly once.  Every public entry point calls this, so
 * callers never need a separate init step. */
static int provider_ensure(void) {
    if (g_ready) return 0;
    while (g_seeded < BUILTIN_N) {
        int r = registry_push(g_builtins[g_seeded]);
        if (r < 0) return r;
        g_seeded++;
    }
    g_ready = true;
    return 0;
}

void agentc_provider_registry_reset(void) {
    size_t w = 0;
    if (provider_ensure() != 0) return;
    for (size_t i = 0; i < g_n; i++)
        if (is_builtin(g_reg[i])) g_reg[w++] = g_reg[i];
    g_n = w;
    /* Return to the pristine baseline so tests can assert memory: drop the row
     * storage (the builtins re-seed lazily on the next lookup) and the
     * open-stream table when no stream is live. Builtin rows and their handles
     * live in static storage and stay stable; dropped dynamic handles are owned
     * by their producer (the extension registry frees its own records).
     *
     * The dynamically named OpenAI-compatible rows built by
     * agentc_prov_openai_compatible() are heap blocks owned by their producer for
     * the process lifetime (see src/prov/openai.c): reset deliberately unlinks
     * them without freeing, so a still-held AgcProvider handle stays valid. */
    if (g_open_n == 0) {
        agentc_free(g_reg);
        g_reg = NULL;
        g_cap = 0;
        g_n = 0;
        g_ready = false;
        g_seeded = 0;
        agentc_free(g_open);
        g_open = NULL;
        g_open_cap = 0;
    }
}

void agentc_provider_retire_ext(void) {
    if (provider_ensure() != 0) return;
    for (size_t i = 0; i < g_n; i++)
        if (g_reg[i]->is_ext) g_reg[i]->retired = true;
}

int agentc_provider_register(AgcProviderOps *ops) {
    int r;
    if (ops == NULL || ops->api == NULL || ops->api[0] == '\0') return -22;  /* EINVAL */
    r = provider_ensure();
    if (r != 0) return r;
    /* Provider identity is the name: several rows share "openai-chat". A
     * retired row keeps its name reserved until the registry reset: a held
     * AgcProvider still resolves to the retired
     * row through agentc_provider_ops(), and the extension adapter resolves
     * its record by name, so letting a new row take the name would silently
     * redirect the old handle to the new extension's callbacks. The old handle
     * therefore stays on the clean -ENOSYS path instead. */
    for (size_t i = 0; i < g_n; i++)
        if (ops->name && g_reg[i]->name && agentc_streq(g_reg[i]->name, ops->name)) {
            if (g_reg[i]->retired)
                agentc_logf(2, "provider: name %s reserved by a retired row", ops->name);
            return -17;                               /* EEXIST */
        }
    return registry_push(ops);
}

const AgcProviderOps *agentc_provider_by_api(const char *api) {
    if (!api || provider_ensure() != 0) return NULL;
    for (size_t i = 0; i < g_n; i++)
        if (!g_reg[i]->retired && g_reg[i]->api && agentc_streq(g_reg[i]->api, api))
            return g_reg[i];
    return NULL;
}

const AgcProviderOps *agentc_provider_by_name(const char *name) {
    if (!name || provider_ensure() != 0) return NULL;
    for (size_t i = 0; i < g_n; i++)
        if (!g_reg[i]->retired && g_reg[i]->name && agentc_streq(g_reg[i]->name, name))
            return g_reg[i];
    return preset_materialize(name);
}

/* Enumerate the live rows, deduplicated by name+api in registration order.
 * Count-first: out == NULL returns the total; otherwise fills min(total, max)
 * and returns the number written. The scan still visits every row when out is
 * provided, so the dedup total stays correct. */
size_t agentc_provider_all(const AgcProviderOps **out, size_t max) {
    if (provider_ensure() != 0) return 0;
    size_t total = 0, written = 0;
    for (size_t i = 0; i < g_n; i++) {
        const AgcProviderOps *row = g_reg[i];
        if (row->retired) continue;
        bool dup = false;
        for (size_t j = 0; j < i && !dup; j++) {
            const AgcProviderOps *prev = g_reg[j];
            if (prev->retired) continue;
            if (row->name && prev->name && row->api && prev->api &&
                agentc_streq(row->name, prev->name) && agentc_streq(row->api, prev->api))
                dup = true;
        }
        if (dup) continue;
        if (out && written < max) out[written++] = row;
        total++;
    }
    return out ? written : total;
}

const AgcProvider *agentc_provider_handle(AgcProviderOps *ops) {
    if (!ops) return NULL;
    if (ops->handle) return ops->handle;
    AgcProvider *h = agentc_alloc(sizeof *h);
    h->name = ops->name;
    h->api = ops->api;
    h->default_base_url = ops->default_base_url;
    h->build_request = ops->build_request;
    h->map_sse = ops->map_sse;
    h->finish = ops->finish;
    ops->handle = h;
    return h;
}

const AgcProviderOps *agentc_provider_ops(const AgcProvider *p) {
    if (!p || provider_ensure() != 0) return NULL;
    for (size_t i = 0; i < g_n; i++)
        if (g_reg[i]->handle == p) return g_reg[i];
    for (size_t i = 0; i < g_n; i++) {
        const AgcProviderOps *row = g_reg[i];
        if (row->name && row->api && p->name && p->api &&
            agentc_streq(row->name, p->name) && agentc_streq(row->api, p->api))
            return row;
    }
    return NULL;
}

/* --------------------------------------------------------- request policy */

const char *agentc_provider_max_tokens_key(const char *provider_name) {
    const AgcProviderOps *ops = agentc_provider_by_name(provider_name);
    if (ops && ops->max_tokens_key && ops->max_tokens_key[0]) return ops->max_tokens_key;
    return "max_completion_tokens";
}

size_t agentc_request_visible_tools(const AgcRequest *r) {
    if (!r || !r->tools) return 0;
    size_t n = 0;
    for (size_t i = 0; i < r->ntools; i++)
        if (!(r->tools[i].flags & AGENTC_TOOL_HIDDEN)) n++;
    return n;
}

bool agentc_provider_msg_serializable(const AgcMsg *m) {
    if (!m) return false;
    if (m->role != AGENTC_ROLE_ASSISTANT) return true;
    return m->stop_reason != AGENTC_STOP_ERROR && m->stop_reason != AGENTC_STOP_ABORTED;
}

/* --------------------------------------------------------- stream lifecycle */

static int open_track(AgcStreamState *st, const AgcProviderOps *ops) {
    if (g_open_n == g_open_cap) {
        size_t cap = g_open_cap ? g_open_cap * 2 : 4;
        OpenStream *grown = agentc_alloc_try(cap * sizeof *grown);
        if (!grown) return -12;
        if (g_open_n) agentc_memcpy(grown, g_open, g_open_n * sizeof *grown);
        agentc_free(g_open);
        g_open = grown;
        g_open_cap = cap;
    }
    g_open[g_open_n].st = st;
    g_open[g_open_n].ops = ops;
    g_open_n++;
    return 0;
}

static const AgcProviderOps *open_untrack(AgcStreamState *st) {
    for (size_t i = g_open_n; i > 0; i--) {
        if (g_open[i - 1].st != st) continue;
        const AgcProviderOps *ops = g_open[i - 1].ops;
        for (size_t j = i; j < g_open_n; j++) g_open[j - 1] = g_open[j];
        g_open_n--;
        return ops;
    }
    return NULL;
}

const AgcProviderOps *agentc_provider_open_ops(const AgcStreamState *st) {
    for (size_t i = g_open_n; i > 0; i--)
        if (g_open[i - 1].st == st) return g_open[i - 1].ops;
    return NULL;
}

static void priv_dispose(AgcStreamState *st, const AgcProviderOps *ops) {
    if (ops && ops->stream_close) {
        ops->stream_close(st);
    } else if (st->priv) {
        agentc_free(st->priv);
    }
    st->priv = NULL;
}

int agentc_stream_state_init(const AgcProvider *p, AgcStreamState *st) {
    const AgcProviderOps *ops;
    if (!st) return -22;                              /* EINVAL */
    ops = agentc_provider_ops(p);
    agentc_memset(st, 0, sizeof *st);
    st->msg = NULL;
    st->stop_reason = AGENTC_STOP_PENDING;
    st->tool_index = (size_t)-1;
    st->arena = agentc_json_arena_new(0);
    if (!st->arena) return -12;                       /* ENOMEM */
    /* Track before stream_open: an adapter whose callbacks do not receive the
     * row (the extension provider) recovers it with agentc_provider_open_ops. */
    if (open_track(st, ops) != 0) {
        agentc_json_arena_free(st->arena);
        st->arena = NULL;
        return -12;
    }
    if (ops && ops->stream_open) {
        int r = ops->stream_open(st);
        if (r != 0) {
            open_untrack(st);
            priv_dispose(st, ops);
            agentc_json_arena_free(st->arena);
            st->arena = NULL;
            return r;
        }
    }
    return 0;
}

void agentc_stream_state_close(AgcStreamState *st) {
    if (!st) return;
    priv_dispose(st, open_untrack(st));
    agentc_buf_free(&st->args);                       /* legacy field, deprecated */
    agentc_json_arena_free(st->arena);
    st->arena = NULL;
}
