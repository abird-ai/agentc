/* oauth.h — subscription logins (Claude Pro/Max, ChatGPT) with PKCE S256.
 *
 * Tokens are stored in ~/.config/agentc/auth.jsonc under
 *   {"<provider>":{"oauth":{access_token,refresh_token,expires_at,account_id}}}
 * written atomically with mode 0600. agentc_auth_key() resolves a stored OAuth
 * credential before any api key and never falls back to the environment when
 * a refresh fails.
 *
 * Only providers listed in the table in oauth.c are supported. The table holds
 * the public client ids used by the vendor CLIs; see the "unofficial client"
 * comment there.
 */
#ifndef AGENTC_OAUTH_H
#define AGENTC_OAUTH_H

#include "agentc.h"

/* Interactive: builds the authorize URL, opens the browser, runs a one-shot
 * loopback server on 127.0.0.1, validates state, exchanges the code and stores
 * the credential. Returns 0 or -errno. */
int agentc_oauth_login(const char *provider);

/* Removes the stored oauth credential; api_key entries are preserved. */
int agentc_oauth_logout(const char *provider);

/* Forces a token refresh. Called by auth resolution when the access token is
 * within 5 minutes of expiry. Returns 0 or -errno. */
int agentc_oauth_refresh(const char *provider);

/* True when auth.jsonc holds an oauth credential (access token) for the
 * provider, whether or not it is currently expired. */
bool agentc_oauth_logged_in(const char *provider);

/* ---- internal (auth.c / tests); not part of the public CLI surface ---- */
/* Resolve the stored credential:
 *    1 -> *token is a valid access token (borrowed, valid until the next
 *         oauth call)
 *    0 -> no stored credential for the provider
 *   -1 -> a credential exists but refresh failed; agentc_oauth_last_error()
 *         describes the failure. Callers must not fall back to env.
 * Never returns a pointer into the credential file. */
int agentc_oauth_token_for(const char *provider, const char **token);

/* Last refresh/exchange error, or NULL. */
const char *agentc_oauth_last_error(void);

/* Releases the in-process credential store (called by agentc_auth_free). */
void agentc_oauth_free(void);

/* ------------------------------------------------------------- test hooks */
/* Fixed clock: now_ms is called instead of os_now_ns(). */
void agentc_oauth_test_set_clock(i64 (*now_ms)(void));
/* Replace the endpoints from the provider table for tests. The strings are
 * borrowed and must outlive the hook. NULL restores the table. */
void agentc_oauth_test_set_endpoints(const char *authorize_url, const char *token_url);
/* Skip the browser + loopback wait. Called with the authorize URL, the hook
 * writes "code=...&state=..." and returns 0. */
typedef int (*AgcOauthCallbackHook)(void *ud, const char *authorize_url,
                                   char *query_out, size_t cap);
void agentc_oauth_test_set_callback(AgcOauthCallbackHook cb, void *ud);

/* PKCE helper exposed for RFC 7636 appendix B vectors. Returns 0 or -errno. */
int agentc_oauth_pkce_challenge(const char *verifier, char *out, size_t cap);

/* Parse one HTTP request, extract the callback query and build the response.
 * No sockets; used by the loopback server and by tests. code_out/state_out are
 * set to "" when the parameter is absent. Returns 0 when a response was built. */
int agentc_oauth_callback_handle(const char *req, size_t n, AgcBuf *response,
                             char *code_out, size_t code_cap,
                             char *state_out, size_t state_cap);

/* Same, but only the given loopback path is accepted as the redirect target
 * (providers register an exact redirect URI, e.g. "/auth/callback"). */
int agentc_oauth_callback_handle_path(const char *req, size_t n, const char *path,
                                      AgcBuf *response, char *code_out,
                                      size_t code_cap, char *state_out, size_t state_cap);

/* ChatGPT account id from the stored OAuth credential (the id_token claim the
 * Codex backend wants in `chatgpt-account-id`), or NULL. Borrowed. */
const char *agentc_oauth_account_id(const char *provider);

#endif /* AGENTC_OAUTH_H */
