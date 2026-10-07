/* project.c — trust resolution and resources_discover consumption.
 *
 * Trust precedence: an explicit --approve/--no-approve wins; otherwise the
 * first project_trust hook decision is consumed ("yes"/"no" decides,
 * "undecided" falls back) and remember:true is persisted; otherwise the
 * saved/configured agentc_trust_resolve result applies. project_trust is
 * fail-closed: a failed or malformed handler leaves the project untrusted.
 *
 * resources_discover can only contribute paths inside the config dir or
 * inside the project directory (the latter only when trusted). Every path is
 * re-validated here (absolute, no ".." segment, under an allowed root); the
 * hook is never trusted to stay in bounds.
 *
 * Both points are guarded by agentc_ext_wants() so a session without
 * subscribers pays neither the payload build nor the emit.
 */
#include "agentc.h"
#include "config.h"
#include "ext.h"
#include "trust.h"
#include "wire.h"
#include "base/limits.h"
#include "core/project.h"
#include "core/prompt.h"

/* --------------------------------------------------------------- paths */

static bool is_sep(char c) {
    return c == '/' || c == '\\';
}

static bool path_absolute(const char *p) {
    if (p[0] == '/')
        return true;
    if (p[0] == '\\' && p[1] == '\\')
        return true; /* UNC */
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
        (p[2] == '/' || p[2] == '\\'))
        return true; /* drive-letter */
    return false;
}

/* A ".." path segment anywhere rejects the path, even if it lexically cancels
 * out: validation must not depend on normalizing hostile input. */
static bool has_dotdot(const char *p) {
    const char *s = p;
    while (*s) {
        while (is_sep(*s))
            s++;
        const char *e = s;
        while (*e && !is_sep(*e))
            e++;
        if (e - s == 2 && s[0] == '.' && s[1] == '.')
            return true;
        s = e;
    }
    return false;
}

/* Is `path` the same as or below `root`? Separators are treated alike so a
 * Windows-style root still matches; the boundary must be a separator. */
static bool path_under(const char *root, const char *path) {
    if (!root || !root[0] || !path || !path[0])
        return false;
    size_t rl = agentc_strlen(root);
    while (rl > 1 && is_sep(root[rl - 1]))
        rl--;
    if (rl == 1 && is_sep(root[0]))
        return path_absolute(path); /* "/" */
    for (size_t i = 0; i < rl; i++)
        if (root[i] != path[i] && !(is_sep(root[i]) && is_sep(path[i])))
            return false;
    if (path[rl] == 0)
        return true;
    if (is_sep(root[rl - 1]))
        return true;
    return is_sep(path[rl]);
}

int agentc_project_validate_path(const char *cwd, bool trusted, const char *path) {
    if (!path || !path[0])
        return -22;
    if (!path_absolute(path))
        return -22;
    if (has_dotdot(path))
        return -22;

    char home[4096];
    if (agentc_config_home(home, sizeof home) && path_under(home, path))
        return 0;
    if (trusted && cwd && cwd[0] && path_under(cwd, path))
        return 0;
    return -22;
}

/* --------------------------------------------------------------- trust */

bool agentc_project_resolve_trust(const char *cwd, int cli_verdict) {
    if (!cwd)
        cwd = "";
    /* What the already-completed config load used for the project file: any
     * trust granted only now cannot retroactively apply config.jsonc. */
    const bool trusted_at_load =
        agentc_trust_resolve(cwd, cli_verdict, agentc_config_default_trusted());
    bool trusted = trusted_at_load;

    if (cli_verdict == 1) {
        trusted = true;
    } else if (cli_verdict == 0) {
        trusted = false;
    } else {
        bool decided = false;
        bool remember = false;
        if (cwd[0] && agentc_ext_wants("project_trust")) {
            AgcBuf p = {0};
            AgcJsonW w;
            agentc_jsonw_init(&w, &p);
            agentc_jsonw_obj(&w);
            agentc_jsonw_key(&w, "cwd");
            agentc_jsonw_cstr(&w, cwd);
            agentc_jsonw_end(&w);
            AgcExtResult r = agentc_ext_emit("project_trust", (const char *)p.p);

            if (r.blocked || (r.handled && !r.result_json)) {
                /* fail-closed: a failed/blocked override point denies trust */
                agentc_logf(2, "project_trust: extension failed or blocked; project untrusted");
                agentc_free(r.result_json);
                agentc_buf_free(&p);
                return false;
            }
            if (r.result_json) {
                AgcJson *root = agentc_json_parse(r.result_json, agentc_strlen(r.result_json));
                const char *verdict = agentc_json_get_str(root, "trusted");
                remember = agentc_json_get_bool(root, "remember", false);
                if (verdict && agentc_streq(verdict, "yes")) {
                    trusted = true;
                    decided = true;
                } else if (verdict && agentc_streq(verdict, "no")) {
                    trusted = false;
                    decided = true;
                } else if (verdict && agentc_streq(verdict, "undecided")) {
                    decided = false;
                } else {
                    /* malformed decision object: fail-closed */
                    agentc_logf(2, "project_trust: malformed decision; project untrusted");
                    decided = true;
                    trusted = false;
                }
            }
            agentc_free(r.result_json);
            agentc_buf_free(&p);
        }
        if (!decided) {
            trusted = agentc_trust_resolve(cwd, cli_verdict, agentc_config_default_trusted());
        } else if (remember) {
            agentc_trust_save(cwd, trusted);
        }
    }

    if (trusted && !trusted_at_load && cwd[0])
        agentc_logf(2,
                    "warning: project became trusted after config was loaded; "
                    "%s/.agentc/config.jsonc is not applied this run (restart to load it)",
                    cwd);
    return trusted;
}

/* ----------------------------------------------------------- resources */

static void apply_path_list(const AgcJson *root, const char *key, const char *cwd, bool trusted,
                            void (*consume)(const char *)) {
    const AgcJson *arr = agentc_json_get(root, key);
    if (agentc_json_type(arr) != AGENTC_JSON_ARR)
        return;
    size_t examined = 0;
    for (size_t i = 0; i < agentc_json_len(arr); i++) {
        /* The cap counts list entries, checked before validation: a hostile
         * hook cannot burn O(n) validation or spam one warning per element
         * with an over-long list of invalid paths. */
        if (examined >= AGENTC_RESOURCE_PATHS_MAX) {
            agentc_logf(1, "resources_discover: %s has more than %d paths; ignoring the rest", key,
                        AGENTC_RESOURCE_PATHS_MAX);
            break;
        }
        examined++;
        const char *path = agentc_json_str(agentc_json_at(arr, i), NULL);
        if (!path) {
            agentc_logf(2, "resources_discover: ignoring non-string %s entry", key);
            continue;
        }
        if (!path[0]) {
            agentc_logf(2, "resources_discover: ignoring empty %s entry", key);
            continue;
        }
        if (agentc_project_validate_path(cwd, trusted, path) != 0) {
            agentc_logf(2, "resources_discover: rejecting out-of-scope %s path: %s", key, path);
            continue;
        }
        consume(path);
    }
}

/* ----------------------------------------------------------- policy */
/* Consume one validated list of policy file paths. A path is only accepted if
 * it stays inside the config dir or (when trusted) the project dir, then the
 * rules are merged into the process policy store. */
static void apply_policy_paths(const AgcJson *root, const char *key, const char *cwd,
                               bool trusted) {
    const AgcJson *arr = agentc_json_get(root, key);
    if (agentc_json_type(arr) != AGENTC_JSON_ARR)
        return;
    size_t examined = 0;
    for (size_t i = 0; i < agentc_json_len(arr); i++) {
        if (examined >= AGENTC_POLICY_PATHS_MAX) {
            agentc_logf(1, "policy_rules: %s has more than %d paths; ignoring the rest", key,
                        AGENTC_POLICY_PATHS_MAX);
            break;
        }
        examined++;
        const char *path = agentc_json_str(agentc_json_at(arr, i), NULL);
        if (!path || !path[0]) {
            agentc_logf(2, "policy_rules: ignoring empty/non-string %s entry", key);
            continue;
        }
        if (agentc_project_validate_path(cwd, trusted, path) != 0) {
            agentc_logf(2, "policy_rules: rejecting out-of-scope %s path: %s", key, path);
            continue;
        }
        char err[128] = "";
        int rc = agentc_trust_policy_load(path, err, sizeof err);
        if (rc != 0)
            agentc_logf(2, "policy_rules: %s", err[0] ? err : "cannot load policy file");
    }
}

void agentc_project_apply_policy(const char *cwd, bool trusted) {
    if (!cwd)
        cwd = "";
    /* Auto-load the user policy, then a trusted project policy. Absent files
     * are a no-op; a malformed file is logged and skipped (never fatal). */
    char home[4096];
    if (agentc_config_home(home, sizeof home)) {
        char p[4200];
        int n = agentc_snprintf(p, sizeof p, "%s/policy.jsonc", home);
        if (n > 0 && (size_t)n < sizeof p) {
            char err[128] = "";
            (void)agentc_trust_policy_load(p, err, sizeof err);
            if (err[0])
                agentc_logf(2, "policy: %s", err);
        }
    }
    if (trusted && cwd[0]) {
        char p[4200];
        int n = agentc_snprintf(p, sizeof p, "%s/.agentc/policy.jsonc", cwd);
        if (n > 0 && (size_t)n < sizeof p) {
            char err[128] = "";
            (void)agentc_trust_policy_load(p, err, sizeof err);
            if (err[0])
                agentc_logf(2, "policy: %s", err);
        }
    }

    if (!agentc_ext_wants("policy_rules"))
        return;

    AgcBuf p = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &p);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "cwd");
    agentc_jsonw_cstr(&w, cwd);
    agentc_jsonw_key(&w, "reason");
    agentc_jsonw_cstr(&w, "startup");
    agentc_jsonw_end(&w);

    AgcExtResult r = agentc_ext_emit("policy_rules", (const char *)p.p);
    if (r.blocked) {
        /* Fail-closed: a failed/blocked handler rejects the whole batch. */
        agentc_logf(2, "policy_rules: extension failed or blocked; no paths consumed");
    } else if (r.result_json) {
        char *eff = agentc_ext_merge_fields((const char *)p.p, r.result_json);
        AgcJson *root = agentc_json_parse(eff, agentc_strlen(eff));
        apply_policy_paths(root, "policyPaths", cwd, trusted);
        agentc_free(eff);
    }
    agentc_free(r.result_json);
    agentc_buf_free(&p);
}

void agentc_project_apply_resources(const char *cwd, bool trusted) {
    if (!cwd)
        cwd = "";
    if (!agentc_ext_wants("resources_discover"))
        return;

    AgcBuf p = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &p);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "cwd");
    agentc_jsonw_cstr(&w, cwd);
    agentc_jsonw_key(&w, "reason");
    agentc_jsonw_cstr(&w, "startup");
    agentc_jsonw_end(&w);

    AgcExtResult r = agentc_ext_emit("resources_discover", (const char *)p.p);
    if (r.blocked) {
        /* Fail-closed: a failed/blocked handler rejects the whole batch. The
         * accumulator may still hold earlier handlers' paths, so nothing may
         * be consumed from it. */
        agentc_logf(2, "resources_discover: extension failed or blocked; no paths consumed");
    } else if (r.result_json) {
        /* fields-merge point: the effective payload is base patched by the
         * accumulated result. */
        char *eff = agentc_ext_merge_fields((const char *)p.p, r.result_json);
        AgcJson *root = agentc_json_parse(eff, agentc_strlen(eff));
        apply_path_list(root, "skillPaths", cwd, trusted, agentc_resource_add_skill_root);
        apply_path_list(root, "promptPaths", cwd, trusted, agentc_resource_add_prompt_root);
        apply_path_list(root, "themePaths", cwd, trusted, agentc_resource_add_theme_root);
        agentc_free(eff);
    }
    agentc_free(r.result_json);
    agentc_buf_free(&p);
}
