/* discover.h — provider model discovery (and its on-disk cache).
 *
 * Endpoints, by provider:
 *   ollama        GET {host}/api/tags          (native: name, family, params)
 *                 fallback GET {base}/models
 *   ollama-cloud  GET {base}/models           (OpenAI-compatible, Bearer)
 *   anthropic     GET {base}/v1/models        (x-api-key or Bearer)
 *   openai, others GET {base}/models          (OpenAI-compatible, Bearer)
 *
 * Results are cached in <config>/agentc/models-cache.jsonc, one object per provider:
 *   {"ollama":{"fetched":<unix_ms>,"models":[{"id":…,"name":…,"detail":…,
 *                                              "ctx":0,"max":0,"reasoning":false,"image":false}]}}
 */
#ifndef AGENTC_DISCOVER_H
#define AGENTC_DISCOVER_H

#include "agentc.h"

typedef struct {
    char *id;          /* the id to send to the API */
    char *name;        /* display name (id when the API has none) */
    char *detail;      /* parameter size / family, when reported */
    u32 ctx_window;    /* 0 = unknown */
    u32 max_tokens;    /* 0 = unknown */
    bool reasoning;
    bool image;
} AgcDiscovered;

/* List models. Returns the count (0 on failure; `err` gets a short reason) and
 * stores an owned array in *out to release with agentc_discover_free(). */
size_t agentc_discover_models(const char *provider, const char *base_url, const char *api_key,
                          AgcDiscovered **out, size_t max, int timeout_ms, char *err,
                          size_t err_cap);
void agentc_discover_free(AgcDiscovered *m, size_t n);

/* Cache read/write (atomic, per provider; other providers are preserved).
 * `base_url` is stored and, on load, a non-empty cached base that differs from
 * the requested one is a miss (a changed endpoint must not surface stale
 * models); an entry written before "base" existed is accepted. */
int agentc_discover_cache_save(const char *provider, const char *base_url,
                           const AgcDiscovered *m, size_t n);
size_t agentc_discover_cache_load(const char *provider, const char *base_url,
                              AgcDiscovered **out, size_t max, i64 *fetched_ms);
const char *agentc_discover_cache_path(char *buf, size_t cap);

/* Register discovered models in the runtime catalog so the agent can use them. */
void agentc_discover_register(const char *provider, const char *api, const char *base_url,
                          const AgcDiscovered *m, size_t n);

#endif /* AGENTC_DISCOVER_H */
