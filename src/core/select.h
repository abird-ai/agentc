/* core/select.h — provider selection, credentials and model filtering.
 *
 * Core services shared by the app (main.c, onboarding, discovery) and the
 * strict no-app libagentc archive (Axis B4). The app-facing forwarding header
 * is src/app/setup.h; the interactive first-run onboarding and live discovery
 * orchestration stay in src/app/setup.c.
 *
 * Internal header, not part of the frozen public surface.
 */
#ifndef AGENTC_CORE_SELECT_H
#define AGENTC_CORE_SELECT_H

#include "agent.h"
#include "config.h"

/* Provider registry: anthropic, openai, ollama, ollama-cloud plus the
 * OpenAI-compatible presets owned by the registry (agentc_provider_presets in
 * prov/provider.h; it is the one source of truth for the names). NULL for an
 * unknown name. For the `openai` pair the credential kind
 * decides the wire: an explicit API key selects Chat Completions, a stored OAuth
 * credential selects the Codex backend. Call agentc_setup_set_context() once
 * after loading the config so this choice (and the CLI --api-key) is visible to
 * every later call, including the front-end agent rebuild. */
const AgcProvider *agentc_setup_provider(const char *name);

/* Pure credential-kind choice; does not consult the published context. `flag`
 * is the raw --api-key value. */
const AgcProvider *agentc_setup_provider_for(const AgcConfig *cfg, const char *name,
                                             const char *flag);

/* Explicit (non-OAuth) API key precedence: --api-key > provider env variable >
 * config api_keys. NULL when none is present. Borrowed. */
const char *agentc_setup_explicit_key(const AgcConfig *cfg, const char *name, const char *flag);

/* Documented credential precedence for a request: --api-key flag > stored OAuth
 * credential > provider env variable > auth.jsonc API key > config api_keys. A
 * stored OAuth credential whose refresh failed stays fail-closed (NULL, no env
 * or config fallback). Borrowed. */
const char *agentc_setup_resolve_key(const AgcConfig *cfg, const char *name, const char *flag);

/* Publish the resolution context (loaded config + CLI key + CLI base URL) used
 * by agentc_setup_provider() and by discovery, so a `--base-url` override keys
 * the discovery cache and the requests to the same endpoint. `cfg` may be NULL. */
void agentc_setup_set_context(const AgcConfig *cfg, const char *cli_key, const char *cli_base);
/* The published CLI key/base URL (for callers that resolve a provider/endpoint
 * themselves). */
const char *agentc_setup_cli_key(void);
const char *agentc_setup_cli_base_url(void);

/* Catalog entries matching `provider` and, when non-NULL, the provider's wire
 * `api` (so the `openai` name no longer mixes Chat Completions and Codex
 * models). With out == NULL returns the total match count; otherwise writes up
 * to `max`. */
size_t agentc_model_filter(const char *provider, const char *api, const AgcModel **out, size_t max);

/* A local Ollama server needs no credential. */
bool agentc_setup_needs_key(const char *name);

/* Effective base URL: --base-url flag > config > provider env (OLLAMA_HOST,
 * OLLAMA_CLOUD_BASE_URL, OPENAI_BASE_URL) > NULL (meaning the provider default).
 * The env-derived result is owned by a process-lifetime, one-per-variable cache
 * and stays valid as long as the env value is unchanged (it is process-stable);
 * a later call for the same variable only rewrites the same buffer. Flag/config
 * values are borrowed. */
const char *agentc_setup_base_url(const AgcConfig *cfg, const char *name, const char *flag);

/* Copy up to `max` live provider names (borrowed, registration order) into
 * `out`; with `out == NULL` return the total. Materializes the lazily
 * registered OpenAI-compatible presets first, so the result covers builtins,
 * presets and extension providers. Used by the unknown-provider diagnostic. */
size_t agentc_setup_provider_names(const char **out, size_t max);

/* Materialize the lazily registered OpenAI-compatible preset rows (and any
 * config providers.<id> rows) so a later agentc_provider_all() walk sees them.
 * `flag` is the raw --api-key value. */
void agentc_setup_seed_presets(const AgcConfig *cfg, const char *flag);

#endif /* AGENTC_CORE_SELECT_H */
