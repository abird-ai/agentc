/* settings.h — extension settings grouped by extension (P6-SETTINGS / R-D29).
 *
 * Extensions declare typed settings through host->add_setting; the group key is
 * the registering extension's name. Values resolve CLI --set > trusted project
 * config > user config > declared default, with SECRET values read from and
 * written to auth.jsonc instead. Unknown keys are preserved by the JSONC
 * read/merge/write path. Validation is fail-safe: an invalid value resolves to
 * the declared default with one warning.
 */
#ifndef AGENTC_SETTINGS_H
#define AGENTC_SETTINGS_H

#include "agentc.h"
#include "agentc_ext.h"

#define AGENTC_SETTINGS_SCOPE_CLI 0
#define AGENTC_SETTINGS_SCOPE_PROJECT 1
#define AGENTC_SETTINGS_SCOPE_USER 2
#define AGENTC_SETTINGS_SCOPE_SECRET 3

/* One declared setting, as the /settings and RPC surfaces need it. Strings are
 * borrowed from the declaration table. */
typedef struct {
    const char *ext;
    const char *key;
    const char *label;
    const char *description;
    const char *default_json;
    const char *enum_json;
    const char *min_json;
    const char *max_json;
    const char *unit;
    int type;       /* AgcSettingType */
    uint32_t flags; /* AGENTC_SETTING_* */
} AgcSettingInfo;

/* Load the user/project config and auth settings subtrees. Call once at startup
 * before extension init; safe to call again (resets). */
int agentc_settings_init(const char *cwd, bool trusted);
/* Clears declarations and values (tests, shutdown). */
void agentc_settings_shutdown(void);

/* Record a CLI `--set <ext>.<key>=<value>` (highest priority). The value is
 * parsed as JSON, or wrapped as a string when it is not valid JSON. */
void agentc_settings_add_cli(const char *assignment);

/* Declare a setting for `ext` (owned copy). Re-declaring the same (ext,key)
 * replaces the prior declaration. */
void agentc_settings_declare(const char *ext, const AgcExtSetting *setting, u64 owner);
void agentc_settings_owner_remove(u64 owner);

/* Resolved value as JSON (borrowed; stable until the next set/reset). NULL when
 * the (ext,key) is not declared. */
const char *agentc_settings_get_json(const char *ext, const char *key);
int agentc_settings_get_bool(const char *ext, const char *key, int dflt);
long long agentc_settings_get_int(const char *ext, const char *key, long long dflt);
double agentc_settings_get_float(const char *ext, const char *key, double dflt);

/* Validate and persist a value in the owning scope. Returns 0 | -errno. */
int agentc_settings_set(const char *ext, const char *key, const char *value_json);
/* Remove the value from the owning scope (reverts to a lower scope/default). */
int agentc_settings_reset(const char *ext, const char *key);

/* Count-first list of declared settings, in declaration order. */
size_t agentc_settings_list(AgcSettingInfo *out, size_t max);

/* Effective scope of the resolved value, or -1 when undeclared. */
int agentc_settings_scope_of(const char *ext, const char *key);
/* True when the resolved value came from a scope other than the default. */
bool agentc_settings_is_set(const char *ext, const char *key);

#endif /* AGENTC_SETTINGS_H */
