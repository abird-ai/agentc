/* setup.h — provider selection, model discovery and first-run onboarding.
 *
 * Kept out of main.c so it is unit-testable (tests link the app sources except
 * main.c) and so the CLI surface stays readable.
 */
#ifndef AGENTC_APP_SETUP_H
#define AGENTC_APP_SETUP_H

#include "agent.h"
#include "config.h"

/* Provider registry: anthropic, openai, ollama, ollama-cloud plus OpenAI-
 * compatible presets (openrouter, xai, deepseek, groq, mistral, together,
 * gemini). NULL for an unknown name. For the `openai` pair the credential kind
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
size_t agentc_model_filter(const char *provider, const char *api, const AgcModel **out,
                           size_t max);

/* A local Ollama server needs no credential. */
bool agentc_setup_needs_key(const char *name);

/* Effective base URL: --base-url flag > config > provider env (OLLAMA_HOST,
 * OLLAMA_CLOUD_BASE_URL, OPENAI_BASE_URL) > NULL (meaning the provider default).
 * The env-derived result is owned by a process-lifetime, one-per-variable cache
 * and stays valid as long as the env value is unchanged (it is process-stable);
 * a later call for the same variable only rewrites the same buffer. Flag/config
 * values are borrowed. */
const char *agentc_setup_base_url(const AgcConfig *cfg, const char *name, const char *flag);

/* Load the cached model list and, when `live` and not `offline`, refresh it from
 * the provider (TTL: 24 h) and register everything in the runtime catalog.
 * Returns the number of models known for the provider afterwards (the cached
 * count when a live refresh fails). */
size_t agentc_setup_discover(const AgcConfig *cfg, const char *name, bool live, bool offline,
                         bool force);

/* `agentc --list-models [filter]`: discover (optionally forced) and print the
 * catalog. Returns a process exit code. */
int agentc_setup_list_models(const AgcConfig *cfg, const char *filter, bool refresh, bool offline);

/* Copy up to `max` live provider names (borrowed, registration order) into
 * `out`; with `out == NULL` return the total. Materializes the lazily
 * registered OpenAI-compatible presets first, so the result covers builtins,
 * presets and extension providers. Used by the unknown-provider diagnostic. */
size_t agentc_setup_provider_names(const char **out, size_t max);

/* First-run onboarding. Writes auth.jsonc and setup.jsonc, reloads *cfg. Only
 * call when stdin is a terminal. Returns 0 on success, 1 when the user quit. */
int agentc_setup_onboard(AgcConfig **cfg, const char *base_url_flag, bool offline);

#endif /* AGENTC_APP_SETUP_H */
