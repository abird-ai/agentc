/* provider.h — provider descriptor registry and shared stream lifecycle.
 *
 * One AgcProviderOps row per named provider; the row carries everything the
 * agent loop, auth/discovery/setup and compaction need: wire api, request
 * path, base URL, credential env vars, discovery style and the adapter hooks.
 * Providers are data, not branches in agent.c.  Names may share an api string
 * (openai / ollama / ollama-cloud all speak openai-chat), so rows are keyed by
 * name; by_api returns the first registered row for an api.
 *
 * The public AgcProvider stays the v1 vtable (build_request/map_sse/finish);
 * provider.c mirrors those pointers from the row into a stable, lazily created
 * AgcProvider handle so factories keep returning process-stable pointers.
 * Adapter-private stream state lives behind AgcStreamState.priv.
 */
#ifndef AGENTC_PROV_PROVIDER_H
#define AGENTC_PROV_PROVIDER_H

#include "agent.h"

enum {
    AGENTC_DISCOVER_DEFAULT = 0,   /* GET <base>/models, OpenAI-shaped list */
    AGENTC_DISCOVER_ANTHROPIC,     /* GET <base>/v1/models + anthropic-version */
    AGENTC_DISCOVER_OLLAMA,        /* GET <base>/api/tags, fallback /models */
    AGENTC_DISCOVER_GOOGLE,        /* GET <base>/models, {models:[{name,displayName,…}]} + x-goog-api-key */
    AGENTC_DISCOVER_NONE,          /* no listing */
};

typedef struct AgcProviderOps {
    const char *name;              /* "openai" */
    const char *api;               /* wire protocol key, table key */
    const char *path;              /* request path */
    const char *default_base_url;
    const char *env_keys[3];       /* credential env vars, first non-empty wins; NULL slot ends */
    int  needs_key;                /* 0 for local providers (ollama) */
    int  discover_style;           /* AGENTC_DISCOVER_* */
    const char *max_tokens_key;    /* JSON field for the output cap; NULL = "max_completion_tokens" */
    int  (*build_request)(AgcBuf *out, const AgcRequest *r, const char *url_host,
                          const char *url_path);
    int  (*map_sse)(AgcStreamState *st, const AgcSseEvent *ev);
    int  (*finish)(AgcStreamState *st);
    int  (*stream_open)(AgcStreamState *st);   /* alloc st->priv (+adapter scratch) */
    void (*stream_close)(AgcStreamState *st);  /* free st->priv contents */
    const AgcProvider *handle;  /* registry-owned; stable public vtable for this row */

    /* Extension-provider fields. `is_ext` tags a row whose
     * callbacks belong to a loaded extension; `ext_rec` is the extension
     * registry's opaque record. `retired` rows are skipped by
     * agentc_provider_by_name/by_api but still resolve through
     * agentc_provider_ops() so an in-flight agent handle gets a clean adapter
     * error. `auth_headers` appends the row's discovery auth header(s); NULL
     * means the caller's Bearer default (builtin rows). */
    int   is_ext;
    void *ext_rec;
    bool  retired;
    void (*auth_headers)(const struct AgcProviderOps *self, AgcBuf *out,
                         const char *api_key);
} AgcProviderOps;

int agentc_provider_register(AgcProviderOps *ops);          /* index | -EEXIST/-EINVAL/-ENOMEM */
const AgcProviderOps *agentc_provider_by_api(const char *api);
const AgcProviderOps *agentc_provider_by_name(const char *name);
const AgcProviderOps *agentc_provider_ops(const AgcProvider *p);  /* handle -> row */
const AgcProvider *agentc_provider_handle(AgcProviderOps *ops);   /* lazily created, stable */
/* The row that opened `st` (stream_state_init tracks it), or NULL. Adapters
 * whose stream callbacks do not receive the row use this to recover it. */
const AgcProviderOps *agentc_provider_open_ops(const AgcStreamState *st);
/* Count-first enumeration of the live (non-retired) rows, deduplicated by
 * name+api in registration order. With out == NULL returns the total;
 * otherwise fills min(total, max) and returns the count written. Rows that
 * share a name but not an api (openai's two wire protocols) stay distinct.
 * The rows stay owned by the registry and are valid until reset/shutdown. */
size_t agentc_provider_all(const AgcProviderOps **out, size_t max);
/* Drop every non-builtin row, release the registry and open-stream storage
 * (safe only with no live stream), and unseed the builtins so the next lookup
 * re-seeds them lazily. Builtin rows and their handles are static and stay
 * stable. Called by the extension registry at shutdown. */
void agentc_provider_registry_reset(void);
/* Mark every extension-registered row retired. Lookups skip it; an existing
 * handle still resolves for a clean adapter error. The extension registry calls
 * this before dropping the records at shutdown. Builtin rows are untouched. */
void agentc_provider_retire_ext(void);
/* Output-token JSON key for a provider name, resolved through its registry row
 * (defaults to "max_completion_tokens" for an unknown or unset row). */
const char *agentc_provider_max_tokens_key(const char *provider_name);
/* Number of request tools the model may see (skips AGENTC_TOOL_HIDDEN). */
size_t agentc_request_visible_tools(const AgcRequest *r);
/* Request eligibility of one transcript message. A failed (ERROR) or aborted
 * (ABORTED) assistant turn is kept in the transcript for the UI and telemetry
 * but must never reach the wire: it can hold partial tool-call arguments and
 * its tool results are not persisted, so serializing it would send a tool_use
 * with no matching tool_result (or truncated arguments). Every other message is
 * serializable; the adapters still apply their own empty-content filter. */
bool agentc_provider_msg_serializable(const AgcMsg *m);

/* Per-stream lifecycle. init: memset, msg/stop_reason/tool_index, arena,
 * ops->stream_open; close: ops->stream_close, arena + legacy st->args. */
int  agentc_stream_state_init(const AgcProvider *p, AgcStreamState *st); /* 0|-ENOMEM */
void agentc_stream_state_close(AgcStreamState *st);

/* Adapter rows, defined by the adapters and seeded lazily by provider.c. */
extern AgcProviderOps agentc_anthropic_ops;
extern AgcProviderOps agentc_openai_ops;
extern AgcProviderOps agentc_ollama_ops;
extern AgcProviderOps agentc_ollama_cloud_ops;
extern AgcProviderOps agentc_openai_codex_ops;
extern AgcProviderOps agentc_google_ops;

#endif /* AGENTC_PROV_PROVIDER_H */
