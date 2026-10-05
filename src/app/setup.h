/* setup.h — app-side discovery orchestration and first-run onboarding.
 *
 * The provider registry, credential resolution and model filtering live in
 * src/core/select.h (they are shared with libagentc); this header declares the
 * interactive/live orchestration that stays in the app. Kept out of main.c so
 * it is unit-testable (tests link the app sources except main.c) and so the CLI
 * surface stays readable.
 */
#ifndef AGENTC_APP_SETUP_H
#define AGENTC_APP_SETUP_H

#include "agent.h"
#include "config.h"
#include "core/select.h"

/* Load the cached model list and, when `live` and not `offline`, refresh it from
 * the provider (TTL: 24 h) and register everything in the runtime catalog.
 * Returns the number of models known for the provider afterwards (the cached
 * count when a live refresh fails). */
size_t agentc_setup_discover(const AgcConfig *cfg, const char *name, bool live, bool offline,
                             bool force);

/* `agentc --list-models [filter]`: discover (optionally forced) and print the
 * catalog. Returns a process exit code. */
int agentc_setup_list_models(const AgcConfig *cfg, const char *filter, bool refresh, bool offline);

/* First-run onboarding. Writes auth.jsonc and setup.jsonc, reloads *cfg. Only
 * call when stdin is a terminal. Returns 0 on success, 1 when the user quit. */
int agentc_setup_onboard(AgcConfig **cfg, const char *base_url_flag, bool offline);

#endif /* AGENTC_APP_SETUP_H */
