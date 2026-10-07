/* trust.c — trust envelopes, static policy, path/host checks, memoization and
 * audit (P6-C; see .agents/plans/P6-TRUST.md).
 *
 * Pure C over the base library plus os_realpath. Every entry point is
 * fail-closed: malformed input, a widening request or an unknown backend is a
 * denial (negative errno / DENY), never an allow. The envelope owns its roots,
 * hosts and policy_id; the current-envelope pointer and every policy pointer
 * the app installs are borrowed.
 */
#include "agent.h"

#include "base/limits.h"
#include "base/glob.h"
#include "config.h"
#include "core/events.h"
#include "plat.h"

#define PATH_MAX_ 4096

/* Project-scope approval memo persistence (C-D11), set by agentc_trust_load. */
static char *g_approvals_path;
static bool g_approvals_persist;

/* ------------------------------------------------------------- helpers */

static void env_clear(AgcTrustEnv *e) {
    if (!e)
        return;
    if (e->roots) {
        for (size_t i = 0; i < e->nroots; i++)
            agentc_free((void *)e->roots[i]);
        agentc_free((void *)e->roots);
    }
    if (e->hosts) {
        for (size_t i = 0; i < e->nhosts; i++)
            agentc_free((void *)e->hosts[i]);
        agentc_free((void *)e->hosts);
    }
    agentc_free((void *)e->policy_id);
    agentc_memset(e, 0, sizeof *e);
}

void agentc_trust_env_free(AgcTrustEnv *e) {
    env_clear(e);
}

static int set_roots(AgcTrustEnv *e, const char *const *roots, size_t n) {
    if (n == 0)
        return 0;
    if (n > AGENTC_TRUST_MAX_ROOTS)
        n = AGENTC_TRUST_MAX_ROOTS;
    e->roots = agentc_alloc(n * sizeof(char *));
    e->nroots = 0;
    for (size_t i = 0; i < n; i++) {
        if (roots[i] == NULL || roots[i][0] == 0)
            continue;
        e->roots[e->nroots++] = agentc_strdup(roots[i]);
    }
    return 0;
}

static int set_hosts(AgcTrustEnv *e, const char *const *hosts, size_t n) {
    if (n == 0)
        return 0;
    if (n > AGENTC_TRUST_MAX_HOSTS)
        n = AGENTC_TRUST_MAX_HOSTS;
    e->hosts = agentc_alloc(n * sizeof(char *));
    e->nhosts = 0;
    for (size_t i = 0; i < n; i++) {
        if (hosts[i] == NULL || hosts[i][0] == 0)
            continue;
        e->hosts[e->nhosts++] = agentc_strdup(hosts[i]);
    }
    return 0;
}

int agentc_trust_env_clone(const AgcTrustEnv *in, AgcTrustEnv *out) {
    if (!in || !out)
        return -22; /* EINVAL */
    agentc_memset(out, 0, sizeof *out);
    *out = *in;
    out->roots = NULL;
    out->nroots = 0;
    out->hosts = NULL;
    out->nhosts = 0;
    out->policy_id = NULL;
    if (in->nroots)
        set_roots(out, in->roots, in->nroots);
    if (in->nhosts)
        set_hosts(out, in->hosts, in->nhosts);
    if (in->policy_id)
        out->policy_id = agentc_strdup(in->policy_id);
    return 0;
}

int agentc_trust_default(AgcTrustEnv *out, const char *cwd) {
    if (!out)
        return -22;
    agentc_memset(out, 0, sizeof *out);
    out->access = AGENTC_ACCESS_RW;
    out->boundary = AGENTC_BOUND_WORKSPACE;
    out->approval = AGENTC_APPROVAL_AUTO;
    out->network = AGENTC_NET_DENY;
    out->flags =
        AGENTC_TRUST_ALLOW_PTY | AGENTC_TRUST_ALLOW_BACKGROUND | AGENTC_TRUST_ALLOW_SUBAGENT;
    if (cwd && cwd[0]) {
        const char *r[1] = {cwd};
        set_roots(out, r, 1);
    }
    return 0;
}

/* ---------------------------------------------------------- widen policy */
static bool g_widen_clamp;

void agentc_trust_set_widen_clamp(bool clamp) {
    g_widen_clamp = clamp;
}
bool agentc_trust_widen_clamp(void) {
    return g_widen_clamp;
}

/* ------------------------------------------------- canonical path checks */
/* Resolve `.`/`..`/symlinks where possible. On a missing leaf, canonicalize the
 * parent directory and re-attach the basename so a create is still checked
 * against its real parent. Returns 0 | -errno. */
static int canon_path(const char *in, char *out, size_t cap) {
    if (!in || !in[0])
        return -22;
    char abs[PATH_MAX_];
    if (in[0] != '/') {
        char cwd[PATH_MAX_];
        if (os_getcwd(cwd, sizeof cwd) < 0)
            return -2;
        int n = agentc_snprintf(abs, sizeof abs, "%s/%s", cwd, in);
        if (n <= 0 || (size_t)n >= sizeof abs)
            return -36;
    } else {
        size_t n = agentc_strlen(in);
        if (n + 1 > sizeof abs)
            return -36;
        agentc_memcpy(abs, in, n + 1);
    }
    if (os_realpath(abs, out, cap) == 0)
        return 0;
    /* leaf missing: realpath the parent, then append the leaf */
    size_t al = agentc_strlen(abs);
    while (al > 1 && abs[al - 1] == '/')
        abs[--al] = 0;
    const char *slash = NULL;
    for (size_t i = al; i > 0; i--)
        if (abs[i - 1] == '/') {
            slash = abs + i - 1;
            break;
        }
    if (!slash)
        return -2;
    size_t plen = (size_t)(slash - abs);
    if (plen == 0)
        plen = 1;
    char parent[PATH_MAX_];
    if (plen >= sizeof parent)
        return -36;
    agentc_memcpy(parent, abs, plen);
    parent[plen] = 0;
    char rp[PATH_MAX_];
    if (os_realpath(parent, rp, sizeof rp) != 0) {
        /* parent also absent: fall back to the lexical path (still bounded) */
        size_t n = al;
        if (n + 1 > cap)
            return -36;
        agentc_memcpy(out, abs, n + 1);
        return 0;
    }
    int n = agentc_snprintf(out, cap, "%s%s", rp, slash);
    if (n <= 0 || (size_t)n >= cap)
        return -36;
    return 0;
}

/* is `child` equal to or under `root`? both must already be canonical. */
static bool under_root(const char *child, const char *root) {
    if (!child || !root || !root[0])
        return false;
    size_t rl = agentc_strlen(root);
    while (rl > 1 && root[rl - 1] == '/')
        rl--;
    size_t cl = agentc_strlen(child);
    if (cl < rl)
        return false;
    if (!agentc_str_eq(child, rl, root, rl))
        return false;
    if (cl == rl)
        return true;
    if (rl == 1 && root[0] == '/')
        return true;
    return child[rl] == '/';
}

static bool path_under_any(const AgcTrustEnv *e, const char *path) {
    for (size_t i = 0; i < e->nroots; i++) {
        char root[PATH_MAX_];
        if (canon_path(e->roots[i], root, sizeof root) != 0) {
            /* an unresolvable root cannot grant access */
            continue;
        }
        if (under_root(path, root))
            return true;
    }
    return false;
}

static bool path_under_tmp(const char *path) {
    const char *tmp = os_getenv("TMPDIR");
    if (!tmp || !tmp[0])
        tmp = "/tmp";
    char rt[PATH_MAX_];
    if (os_realpath(tmp, rt, sizeof rt) != 0) {
        size_t n = agentc_strlen(tmp);
        if (n + 1 > sizeof rt)
            return false;
        agentc_memcpy(rt, tmp, n + 1);
    }
    return under_root(path, rt);
}

bool agentc_trust_path_ok(const AgcTrustEnv *e, const char *path, bool write) {
    if (!e || !path || !path[0])
        return true; /* unconfined / nothing to check */
    if (!write) {
        if (e->boundary != AGENTC_BOUND_STRICT)
            return true;
        char canon[PATH_MAX_];
        if (canon_path(path, canon, sizeof canon) != 0)
            return false;
        return path_under_any(e, canon);
    }
    if (e->access == AGENTC_ACCESS_RO)
        return false;
    if (e->boundary == AGENTC_BOUND_NONE)
        return true;
    char canon[PATH_MAX_];
    if (canon_path(path, canon, sizeof canon) != 0)
        return false;
    if (path_under_any(e, canon))
        return true;
    if (e->boundary == AGENTC_BOUND_WORKSPACE && path_under_tmp(canon))
        return true;
    return false;
}

static bool host_match(const char *pattern, const char *host) {
    if (!pattern || !host)
        return false;
    return agentc_glob_match(pattern, host);
}

bool agentc_trust_host_ok(const AgcTrustEnv *e, const char *host) {
    if (!e)
        return true;
    if (e->network == AGENTC_NET_ALLOW)
        return true;
    if (e->network == AGENTC_NET_DENY)
        return false;
    if (!host || !host[0])
        return false;
    for (size_t i = 0; i < e->nhosts; i++)
        if (host_match(e->hosts[i], host))
            return true;
    return false;
}

/* ---------------------------------------------------------------- decide */

static bool name_is(const char *t, const char *a) {
    return t != NULL && agentc_streq(t, a);
}

/* Fail-closed capability rule (H-D2/S4): a tool is mutating unless it declares
 * AGENTC_TOOL_READONLY. Zero/unknown flags are mutating; the name is never
 * consulted. */
static bool is_mutation_tool(uint32_t flags) {
    return (flags & AGENTC_TOOL_READONLY) == 0;
}

/* Network capability (H-D2): honor a declared bit, else fall back to the
 * conservative built-in name list. The name list is only a fallback for tools
 * that predate AGENTC_TRUST_TOOL_NET. */
static bool is_network_tool(const char *tool, uint32_t flags) {
    if (flags & AGENTC_TRUST_TOOL_NET)
        return true;
    return name_is(tool, "fetch") || name_is(tool, "http_request") || name_is(tool, "web_search") ||
           name_is(tool, "web_fetch") || name_is(tool, "browser") || name_is(tool, "net");
}

/* Extract the first path-ish argument into out. Returns true when present. */
static bool args_path(const char *args_json, char *out, size_t cap) {
    if (!args_json || !args_json[0])
        return false;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *r = agentc_json_parse_in(ja, args_json, agentc_strlen(args_json));
    const char *p = agentc_json_type(r) == AGENTC_JSON_OBJ ? agentc_json_get_str(r, "path") : NULL;
    bool ok = false;
    if (p && p[0]) {
        size_t n = agentc_strlen(p);
        if (n + 1 <= cap) {
            agentc_memcpy(out, p, n + 1);
            ok = true;
        }
    }
    agentc_json_arena_free(ja);
    return ok;
}

static bool args_host(const char *args_json, char *out, size_t cap) {
    if (!args_json || !args_json[0])
        return false;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *r = agentc_json_parse_in(ja, args_json, agentc_strlen(args_json));
    const char *p = NULL;
    if (agentc_json_type(r) == AGENTC_JSON_OBJ)
        p = agentc_json_get_str(r, "host");
    bool ok = false;
    if (p && p[0]) {
        size_t n = agentc_strlen(p);
        if (n + 1 <= cap) {
            agentc_memcpy(out, p, n + 1);
            ok = true;
        }
    }
    agentc_json_arena_free(ja);
    return ok;
}

int agentc_trust_decide_flags(const AgcTrustEnv *e, const char *tool, uint32_t flags,
                              const char *args_json, AgcTrustDecision *out) {
    if (!out)
        return -22;
    agentc_memset(out, 0, sizeof *out);
    out->decision = AGENTC_DEC_ALLOW;
    out->scope = AGENTC_SCOPE_ONCE;
    if (!e || !tool)
        return 0;
    out->policy_id = e->policy_id;

    /* Layer 2a: read-only denies every mutation. Classification is by the
     * resolved tool's declared capability, not its name. */
    if (e->access == AGENTC_ACCESS_RO && is_mutation_tool(flags)) {
        out->decision = AGENTC_DEC_DENY;
        out->reason = "read-only access denies mutating tools";
        return 0;
    }

    /* Layer 2b: path writes must stay inside the boundary. */
    char path[PATH_MAX_];
    bool has_path = args_path(args_json, path, sizeof path);
    if (has_path && is_mutation_tool(flags)) {
        char canon[PATH_MAX_];
        if (canon_path(path, canon, sizeof canon) != 0) {
            out->decision = AGENTC_DEC_DENY;
            out->reason = "cannot resolve path";
            return 0;
        }
        if (!agentc_trust_path_ok(e, canon, true)) {
            if (e->access == AGENTC_ACCESS_RO) {
                out->decision = AGENTC_DEC_DENY;
                out->reason = "read-only access denies writes outside the workspace";
            } else if (e->boundary == AGENTC_BOUND_STRICT) {
                out->decision = AGENTC_DEC_DENY;
                out->reason = "path is outside the strict boundary";
            } else {
                switch (e->approval) {
                case AGENTC_APPROVAL_ASK:
                    out->decision = AGENTC_DEC_ASK;
                    out->reason = "path is outside the workspace";
                    break;
                case AGENTC_APPROVAL_POLICY:
                    out->decision = AGENTC_DEC_DELEGATE;
                    out->reason = "path is outside the workspace";
                    break;
                default:
                    out->decision = AGENTC_DEC_DENY;
                    out->reason = "path is outside the workspace boundary";
                    break;
                }
            }
            return 0;
        }
    } else if (has_path && e->boundary == AGENTC_BOUND_STRICT) {
        /* strict reads must stay inside the roots too */
        char canon[PATH_MAX_];
        if (canon_path(path, canon, sizeof canon) != 0 || !agentc_trust_path_ok(e, canon, false)) {
            out->decision = AGENTC_DEC_DENY;
            out->reason = "read outside the strict boundary";
            return 0;
        }
    }

    /* Layer 2c: network. */
    if (is_network_tool(tool, flags) && e->network != AGENTC_NET_ALLOW) {
        char host[512];
        bool has_host = args_host(args_json, host, sizeof host);
        bool allowed = has_host && agentc_trust_host_ok(e, host);
        if (e->network == AGENTC_NET_DENY && !allowed) {
            if (e->approval == AGENTC_APPROVAL_ASK) {
                out->decision = AGENTC_DEC_ASK;
                out->reason = "network is denied by the envelope";
            } else if (e->approval == AGENTC_APPROVAL_POLICY) {
                out->decision = AGENTC_DEC_DELEGATE;
                out->reason = "network is denied by the envelope";
            } else {
                out->decision = AGENTC_DEC_DENY;
                out->reason = "network is denied by the envelope";
            }
            return 0;
        }
        if (e->network == AGENTC_NET_ALLOWLIST && !allowed) {
            out->decision = AGENTC_DEC_DENY;
            out->reason = "host is not on the network allowlist";
            return 0;
        }
    }
    return 0;
}

int agentc_trust_decide(const AgcTrustEnv *e, const char *tool, const char *args_json,
                        AgcTrustDecision *out) {
    /* Name-only legacy entry point: no capability flags, so every tool is
     * treated as mutating (fail closed). Callers that resolved a tool must use
     * agentc_trust_decide_flags or agentc_trust_gate. */
    return agentc_trust_decide_flags(e, tool, 0, args_json, out);
}

int agentc_trust_filter_tools(const AgcTrustEnv *e, struct AgcTool *tools, size_t *n) {
    if (!e || !tools || !n)
        return 0;
    size_t w = 0;
    for (size_t i = 0; i < *n; i++) {
        if (e->access == AGENTC_ACCESS_RO && is_mutation_tool(tools[i].flags))
            continue;
        if (e->network == AGENTC_NET_DENY && is_network_tool(tools[i].name, tools[i].flags))
            continue;
        tools[w++] = tools[i];
    }
    *n = w;
    return 0;
}

/* ------------------------------------------------------------ JSON codec */

static const char *access_name(int v) {
    return v == AGENTC_ACCESS_RO ? "read-only" : "read-write";
}
static const char *bound_name(int v) {
    switch (v) {
    case AGENTC_BOUND_NONE:
        return "none";
    case AGENTC_BOUND_STRICT:
        return "strict";
    default:
        return "workspace";
    }
}
static const char *approval_name(int v) {
    switch (v) {
    case AGENTC_APPROVAL_ASK:
        return "ask";
    case AGENTC_APPROVAL_POLICY:
        return "policy";
    default:
        return "auto";
    }
}
static const char *network_name(int v) {
    switch (v) {
    case AGENTC_NET_ALLOW:
        return "allow";
    case AGENTC_NET_ALLOWLIST:
        return "allowlist";
    default:
        return "deny";
    }
}

static int parse_access(const char *s) {
    if (!s)
        return -1;
    if (agentc_streq(s, "read-only") || agentc_streq(s, "ro"))
        return AGENTC_ACCESS_RO;
    if (agentc_streq(s, "read-write") || agentc_streq(s, "rw"))
        return AGENTC_ACCESS_RW;
    return -1;
}
static int parse_bound(const char *s) {
    if (!s)
        return -1;
    if (agentc_streq(s, "none"))
        return AGENTC_BOUND_NONE;
    if (agentc_streq(s, "workspace"))
        return AGENTC_BOUND_WORKSPACE;
    if (agentc_streq(s, "strict"))
        return AGENTC_BOUND_STRICT;
    return -1;
}
static int parse_approval(const char *s) {
    if (!s)
        return -1;
    if (agentc_streq(s, "auto"))
        return AGENTC_APPROVAL_AUTO;
    if (agentc_streq(s, "ask"))
        return AGENTC_APPROVAL_ASK;
    if (agentc_streq(s, "policy"))
        return AGENTC_APPROVAL_POLICY;
    return -1;
}
static int parse_network(const char *s) {
    if (!s)
        return -1;
    if (agentc_streq(s, "deny"))
        return AGENTC_NET_DENY;
    if (agentc_streq(s, "allow"))
        return AGENTC_NET_ALLOW;
    if (agentc_streq(s, "allowlist"))
        return AGENTC_NET_ALLOWLIST;
    return -1;
}

static void flags_to_json(AgcBuf *out, uint32_t flags) {
    agentc_buf_byte(out, '[');
    bool first = true;
    struct {
        uint32_t bit;
        const char *name;
    } tbl[] = {
        {AGENTC_TRUST_ALLOW_PTY, "allow_pty"},
        {AGENTC_TRUST_ALLOW_BACKGROUND, "allow_background"},
        {AGENTC_TRUST_ALLOW_SUBAGENT, "allow_subagent"},
        {AGENTC_TRUST_ALLOW_NET_UNATTENDED, "allow_net_unattended"},
        {AGENTC_TRUST_ALLOW_MIC, "allow_mic"},
        {AGENTC_TRUST_ALLOW_SPEAKER, "allow_speaker"},
    };
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
        if (!(flags & tbl[i].bit))
            continue;
        if (!first)
            agentc_buf_byte(out, ',');
        first = false;
        agentc_buf_byte(out, '"');
        agentc_buf_cstr(out, tbl[i].name);
        agentc_buf_byte(out, '"');
    }
    agentc_buf_byte(out, ']');
}

int agentc_trust_to_json(const AgcTrustEnv *e, AgcBuf *out) {
    if (!out)
        return -22;
    AgcJsonW w;
    agentc_jsonw_init(&w, out);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "access");
    agentc_jsonw_cstr(&w, access_name(e ? e->access : AGENTC_ACCESS_RW));
    agentc_jsonw_key(&w, "boundary");
    agentc_jsonw_cstr(&w, bound_name(e ? e->boundary : AGENTC_BOUND_WORKSPACE));
    agentc_jsonw_key(&w, "approval");
    agentc_jsonw_cstr(&w, approval_name(e ? e->approval : AGENTC_APPROVAL_AUTO));
    agentc_jsonw_key(&w, "network");
    agentc_jsonw_cstr(&w, network_name(e ? e->network : AGENTC_NET_DENY));
    agentc_jsonw_key(&w, "roots");
    agentc_jsonw_arr(&w);
    if (e)
        for (size_t i = 0; i < e->nroots; i++)
            agentc_jsonw_cstr(&w, e->roots[i]);
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "hosts");
    agentc_jsonw_arr(&w);
    if (e)
        for (size_t i = 0; i < e->nhosts; i++)
            agentc_jsonw_cstr(&w, e->hosts[i]);
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "flags");
    {
        AgcBuf fb = {0};
        flags_to_json(&fb, e ? e->flags : 0);
        agentc_jsonw_raw(&w, fb.p ? (const char *)fb.p : "[]", fb.len);
        agentc_buf_free(&fb);
    }
    agentc_jsonw_key(&w, "policy_id");
    if (e && e->policy_id)
        agentc_jsonw_cstr(&w, e->policy_id);
    else
        agentc_jsonw_raw(&w, "null", 4);
    agentc_jsonw_key(&w, "agent_id");
    agentc_jsonw_u64(&w, e ? e->self : 0);
    agentc_jsonw_key(&w, "parent_id");
    agentc_jsonw_u64(&w, e ? e->parent : 0);
    agentc_jsonw_key(&w, "depth");
    agentc_jsonw_u64(&w, e ? e->depth : 0);
    agentc_jsonw_end(&w);
    return 0;
}

int agentc_trust_from_json(const char *json, size_t n, AgcTrustEnv *out, char *err, size_t cap) {
    if (!out)
        return -22;
    if (err && cap)
        err[0] = 0;
    agentc_memset(out, 0, sizeof *out);
    agentc_trust_default(out, NULL);
    if (!json || n == 0)
        return 0;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *r = agentc_json_parse_in(ja, json, n);
    if (agentc_json_type(r) != AGENTC_JSON_OBJ) {
        agentc_json_arena_free(ja);
        if (err && cap)
            agentc_snprintf(err, cap, "trust: expected a JSON object");
        agentc_trust_env_free(out);
        return -22;
    }
    const char *s;
    int v;
    if ((s = agentc_json_get_str(r, "access")) != NULL) {
        v = parse_access(s);
        if (v < 0)
            goto bad;
        out->access = v;
    }
    if ((s = agentc_json_get_str(r, "boundary")) != NULL) {
        v = parse_bound(s);
        if (v < 0)
            goto bad;
        out->boundary = v;
    }
    if ((s = agentc_json_get_str(r, "approval")) != NULL) {
        v = parse_approval(s);
        if (v < 0)
            goto bad;
        out->approval = v;
    }
    if ((s = agentc_json_get_str(r, "network")) != NULL) {
        v = parse_network(s);
        if (v < 0)
            goto bad;
        out->network = v;
    }
    if ((s = agentc_json_get_str(r, "policy_id")) != NULL)
        out->policy_id = agentc_strdup(s);
    AgcJson *roots = agentc_json_get(r, "roots");
    if (agentc_json_type(roots) == AGENTC_JSON_ARR) {
        size_t nr = agentc_json_len(roots);
        if (nr > AGENTC_TRUST_MAX_ROOTS)
            nr = AGENTC_TRUST_MAX_ROOTS;
        for (size_t i = 0; i < out->nroots; i++)
            agentc_free((void *)out->roots[i]);
        agentc_free((void *)out->roots);
        out->roots = NULL;
        out->nroots = 0;
        const char **arr = agentc_alloc((nr ? nr : 1) * sizeof(char *));
        size_t k = 0;
        for (size_t i = 0; i < nr; i++) {
            const char *p = agentc_json_str(agentc_json_at(roots, i), NULL);
            if (p && p[0])
                arr[k++] = agentc_strdup(p);
        }
        out->roots = arr;
        out->nroots = k;
    }
    /* S5: trust_to_json emits "hosts" and env_eq compares nhosts; parse it back
     * so a host-bearing envelope does not silently lose its allowlist. */
    AgcJson *hosts = agentc_json_get(r, "hosts");
    if (agentc_json_type(hosts) == AGENTC_JSON_ARR) {
        size_t nh = agentc_json_len(hosts);
        if (nh > AGENTC_TRUST_MAX_HOSTS)
            nh = AGENTC_TRUST_MAX_HOSTS;
        for (size_t i = 0; i < out->nhosts; i++)
            agentc_free((void *)out->hosts[i]);
        agentc_free((void *)out->hosts);
        out->hosts = NULL;
        out->nhosts = 0;
        const char **harr = agentc_alloc((nh ? nh : 1) * sizeof(char *));
        size_t hk = 0;
        for (size_t i = 0; i < nh; i++) {
            const char *p = agentc_json_str(agentc_json_at(hosts, i), NULL);
            if (p && p[0])
                harr[hk++] = agentc_strdup(p);
        }
        out->hosts = harr;
        out->nhosts = hk;
    }
    AgcJson *flags = agentc_json_get(r, "flags");
    if (agentc_json_type(flags) == AGENTC_JSON_ARR) {
        out->flags = 0;
        for (size_t i = 0; i < agentc_json_len(flags); i++) {
            const char *f = agentc_json_str(agentc_json_at(flags, i), NULL);
            if (!f)
                continue;
            if (agentc_streq(f, "allow_pty"))
                out->flags |= AGENTC_TRUST_ALLOW_PTY;
            else if (agentc_streq(f, "allow_background"))
                out->flags |= AGENTC_TRUST_ALLOW_BACKGROUND;
            else if (agentc_streq(f, "allow_subagent"))
                out->flags |= AGENTC_TRUST_ALLOW_SUBAGENT;
            else if (agentc_streq(f, "allow_net_unattended"))
                out->flags |= AGENTC_TRUST_ALLOW_NET_UNATTENDED;
            else if (agentc_streq(f, "allow_mic"))
                out->flags |= AGENTC_TRUST_ALLOW_MIC;
            else if (agentc_streq(f, "allow_speaker"))
                out->flags |= AGENTC_TRUST_ALLOW_SPEAKER;
        }
    }
    out->self = (AgcActorId)agentc_json_get_int(r, "agent_id", out->self);
    out->parent = (AgcActorId)agentc_json_get_int(r, "parent_id", out->parent);
    i64 d = agentc_json_get_int(r, "depth", out->depth);
    if (d < 0)
        d = 0;
    if (d > 65535)
        d = 65535;
    out->depth = (uint16_t)d;
    agentc_json_arena_free(ja);
    return 0;

bad:
    agentc_json_arena_free(ja);
    if (err && cap)
        agentc_snprintf(err, cap, "trust: invalid enum value");
    agentc_trust_env_free(out);
    return -22;
}

int agentc_trust_describe(const AgcTrustEnv *e, AgcBuf *out) {
    if (!out)
        return -22;
    if (!e) {
        agentc_buf_cstr(out, "unconfined");
        return 0;
    }
    agentc_buf_printf(out, "%s/%s/%s/%s roots=%llu", access_name(e->access),
                      bound_name(e->boundary), approval_name(e->approval), network_name(e->network),
                      (unsigned long long)e->nroots);
    return 0;
}

/* ------------------------------------------------------------- narrowing */

static int widen_err(char *err, size_t cap, const char *what) {
    if (err && cap)
        agentc_snprintf(err, cap, "trust: widening denied (%s)", what);
    return -1; /* EPERM */
}

/* Permissiveness rank (S1): ALLOW > ALLOWLIST > DENY. The enum's declaration
 * order (DENY=0 < ALLOW=1 < ALLOWLIST=2) is not the permissiveness order, so
 * narrowing must compare ranks, never the raw values. */
static int net_rank(int n) {
    switch (n) {
    case AGENTC_NET_ALLOW:
        return 2;
    case AGENTC_NET_ALLOWLIST:
        return 1;
    default:
        return 0; /* DENY / unknown */
    }
}

static int narrow_impl(const AgcTrustEnv *parent, const AgcTrustEnv *req, AgcTrustEnv *out,
                       char *err, size_t cap) {
    bool clamp = g_widen_clamp;
    if (err && cap)
        err[0] = 0;
    if (!parent || !req || !out)
        return -22;
    if (agentc_trust_env_clone(parent, out) != 0)
        return -12;

    if (req->access > parent->access) {
        if (!clamp) {
            agentc_trust_env_free(out);
            return widen_err(err, cap, "access");
        }
        /* clamp keeps parent's access */
    } else {
        out->access = req->access;
    }
    if (req->boundary < parent->boundary) {
        if (!clamp) {
            agentc_trust_env_free(out);
            return widen_err(err, cap, "boundary");
        }
    } else {
        out->boundary = req->boundary;
    }
    if (req->approval < parent->approval) {
        if (!clamp) {
            agentc_trust_env_free(out);
            return widen_err(err, cap, "approval");
        }
    } else {
        out->approval = req->approval;
    }
    if (net_rank(req->network) > net_rank(parent->network)) {
        if (!clamp) {
            agentc_trust_env_free(out);
            return widen_err(err, cap, "network");
        }
        /* clamp keeps parent's network */
    } else {
        out->network = req->network;
    }
    uint32_t extra = req->flags & ~parent->flags;
    if (extra) {
        if (!clamp) {
            agentc_trust_env_free(out);
            return widen_err(err, cap, "flags");
        }
    }
    out->flags = clamp ? (parent->flags & req->flags) : req->flags;
    /* lineage comes from the request, not the requested capability set */
    out->self = req->self ? req->self : parent->self;
    out->parent = req->parent ? req->parent : parent->self;
    /* S6: depth is monotone lineage, not a free-form request field. A child is
     * at least the parent's depth and at most one deeper. */
    {
        uint16_t lo = parent->depth;
        /* Finding 5 (VC1b): saturate at UINT16_MAX instead of letting
         * `parent->depth + 1` wrap 65535 -> 0, which would force a child's
         * depth back to zero. */
        uint32_t hi32 = (uint32_t)parent->depth + 1u;
        uint16_t hi = hi32 > 0xFFFFu ? 0xFFFFu : (uint16_t)hi32;
        uint16_t d = req->depth;
        if (d < lo)
            d = lo;
        if (d > hi)
            d = hi;
        out->depth = d;
    }

    /* roots: every requested root must be inside some parent root */
    if (req->nroots) {
        for (size_t i = 0; i < req->nroots; i++) {
            char rc[PATH_MAX_];
            if (canon_path(req->roots[i], rc, sizeof rc) != 0 || !path_under_any(parent, rc)) {
                if (!clamp) {
                    agentc_trust_env_free(out);
                    return widen_err(err, cap, "roots");
                }
                /* clamp: drop the out-of-range root */
                continue;
            }
        }
        /* rebuild the root list to only the inside ones */
        const char **keep = agentc_alloc(req->nroots * sizeof(char *));
        size_t kn = 0;
        for (size_t i = 0; i < req->nroots; i++) {
            char rc[PATH_MAX_];
            if (canon_path(req->roots[i], rc, sizeof rc) != 0)
                continue;
            if (!path_under_any(parent, rc))
                continue;
            keep[kn++] = req->roots[i];
        }
        for (size_t i = 0; i < out->nroots; i++)
            agentc_free((void *)out->roots[i]);
        agentc_free((void *)out->roots);
        out->roots = NULL;
        out->nroots = 0;
        if (kn)
            set_roots(out, keep, kn);
        agentc_free(keep);
    }
    if (req->nhosts) {
        const char **keep = agentc_alloc(req->nhosts * sizeof(char *));
        size_t kn = 0;
        for (size_t i = 0; i < req->nhosts; i++) {
            bool ok;
            if (parent->network == AGENTC_NET_ALLOWLIST) {
                ok = false;
                for (size_t j = 0; j < parent->nhosts && !ok; j++)
                    if (agentc_streq(parent->hosts[j], req->hosts[i]))
                        ok = true;
            } else {
                ok = true; /* parent allow/deny makes the child host set inert */
            }
            if (!ok) {
                if (!clamp) {
                    agentc_trust_env_free(out);
                    return widen_err(err, cap, "hosts");
                }
                continue;
            }
            keep[kn++] = req->hosts[i];
        }
        for (size_t i = 0; i < out->nhosts; i++)
            agentc_free((void *)out->hosts[i]);
        agentc_free((void *)out->hosts);
        out->hosts = NULL;
        out->nhosts = 0;
        if (kn)
            set_hosts(out, keep, kn);
        agentc_free(keep);
    }
    /* S3: policy_id selects the approval backend and is parent-owned. A child
     * may repeat the parent's id but never choose its own; a differing value is
     * a widening attempt. The clone already carries the parent's id. */
    if (req->policy_id &&
        !(parent->policy_id && agentc_streq(parent->policy_id, req->policy_id))) {
        if (!clamp) {
            agentc_trust_env_free(out);
            return widen_err(err, cap, "policy_id");
        }
        /* clamp keeps the parent's id */
    }
    return 0;
}

int agentc_trust_narrow(const AgcTrustEnv *parent, const AgcTrustEnv *req, AgcTrustEnv *out,
                        char *err, size_t cap) {
    return narrow_impl(parent, req, out, err, cap);
}

/* ------------------------------------------------------------------ load */
/* CLI overrides set by main.c before agentc_trust_load. */
static char *g_cli_access;
static char *g_cli_sandbox;
static char *g_cli_approval;
static char *g_cli_policy;
static char *g_cli_network;

static void set_cli_str(char **slot, const char *v) {
    char *copy = (v && v[0]) ? agentc_strdup(v) : NULL;
    agentc_free(*slot);
    *slot = copy;
}

void agentc_trust_set_cli(const char *access, const char *sandbox, const char *approval,
                          const char *policy, const char *network) {
    set_cli_str(&g_cli_access, access);
    set_cli_str(&g_cli_sandbox, sandbox);
    set_cli_str(&g_cli_approval, approval);
    set_cli_str(&g_cli_policy, policy);
    set_cli_str(&g_cli_network, network);
}

static void apply_config_env(AgcTrustEnv *e, const AgcConfig *c) {
    if (!c)
        return;
    int v;
    if ((v = parse_access(c->trust_access)) >= 0)
        e->access = v;
    if ((v = parse_bound(c->trust_sandbox)) >= 0)
        e->boundary = v;
    if ((v = parse_approval(c->trust_approval)) >= 0)
        e->approval = v;
    if ((v = parse_network(c->trust_network)) >= 0)
        e->network = v;
    if (c->trust_policy_id && c->trust_policy_id[0]) {
        agentc_free((void *)e->policy_id);
        e->policy_id = agentc_strdup(c->trust_policy_id);
    }
    if (c->ntrust_roots) {
        for (size_t i = 0; i < e->nroots; i++)
            agentc_free((void *)e->roots[i]);
        agentc_free((void *)e->roots);
        e->roots = NULL;
        e->nroots = 0;
        set_roots(e, (const char *const *)c->trust_roots, c->ntrust_roots);
    }
    if (c->ntrust_hosts) {
        for (size_t i = 0; i < e->nhosts; i++)
            agentc_free((void *)e->hosts[i]);
        agentc_free((void *)e->hosts);
        e->hosts = NULL;
        e->nhosts = 0;
        set_hosts(e, (const char *const *)c->trust_hosts, c->ntrust_hosts);
    }
}

int agentc_trust_load(const char *cwd, bool project_trusted, AgcTrustEnv *out) {
    (void)project_trusted; /* config_load already gates the project block */
    if (!out)
        return -22;
    agentc_trust_default(out, cwd);
    AgcConfig *cfg = agentc_config_load(cwd);
    apply_config_env(out, cfg);
    if (cfg) {
        agentc_trust_set_widen_clamp(cfg->trust_widen && agentc_streq(cfg->trust_widen, "clamp"));
    }
    int v;
    if ((v = parse_access(g_cli_access)) >= 0)
        out->access = v;
    if ((v = parse_bound(g_cli_sandbox)) >= 0)
        out->boundary = v;
    if ((v = parse_approval(g_cli_approval)) >= 0)
        out->approval = v;
    if ((v = parse_network(g_cli_network)) >= 0)
        out->network = v;
    if (g_cli_policy && g_cli_policy[0]) {
        agentc_free((void *)out->policy_id);
        out->policy_id = agentc_strdup(g_cli_policy);
    }
    /* Project-scope approval memos persist in a separate approvals.jsonc
     * (C-D11); load them once per trust load. trust.approval.persist gates the
     * write side; the read always re-validates against the loaded entry. */
    g_approvals_persist = cfg ? agentc_config_trust_persist(cfg) : false;
    {
        char home[PATH_MAX_];
        if (agentc_config_home(home, sizeof home)) {
            char ap[PATH_MAX_ + 64];
            int an = agentc_snprintf(ap, sizeof ap, "%s/approvals.jsonc", home);
            if (an > 0 && (size_t)an < sizeof ap) {
                agentc_free(g_approvals_path);
                g_approvals_path = agentc_strdup(ap);
                (void)agentc_trust_approvals_load(g_approvals_path);
            }
        }
    }
    agentc_config_free(cfg);
    return 0;
}

/* --------------------------------------------------------- current env */
static const AgcTrustEnv *g_current;
static AgcSandboxSpec g_spawn_spec;
static bool g_spawn_spec_valid;

void agentc_trust_set_current(const AgcTrustEnv *e) {
    g_current = e;
    g_spawn_spec_valid = false;
    AgcBuf b = {0};
    (void)agentc_trust_to_json(e, &b);
    AgcTrustEvent ev;
    agentc_memset(&ev, 0, sizeof ev);
    ev.actor_id = e ? e->self : 0;
    ev.effective_json = b.p ? (const char *)b.p : "{}";
    ev.reason = "effective";
    agentc_events_emit(NULL, AGENTC_EV_TRUST_SET, &ev);
    agentc_buf_free(&b);
}
const AgcTrustEnv *agentc_trust_current(void) {
    return g_current;
}

const AgcSandboxSpec *agentc_trust_spawn_spec(void) {
    if (!g_current)
        return NULL;
    if (!g_spawn_spec_valid) {
        agentc_trust_sandbox_spec(g_current, &g_spawn_spec);
        g_spawn_spec_valid = true;
    }
    return &g_spawn_spec;
}

int agentc_trust_sandbox_spec(const AgcTrustEnv *e, AgcSandboxSpec *out) {
    if (!out)
        return -22;
    agentc_memset(out, 0, sizeof *out);
    if (!e)
        return 0;
    out->access = e->access;
    out->boundary = e->boundary;
    out->network = e->network;
    out->writable_roots = e->roots;
    out->nroots = e->nroots;
    out->allowed_hosts = e->hosts;
    out->nhosts = e->nhosts;
    out->exclude_tmp = false;
    return 0;
}

/* --------------------------------------------------------- scope memory */
typedef struct {
    u64 hash;
    int scope;
    int decision;
} ScopeEnt;
static ScopeEnt g_scope[AGENTC_APPROVAL_CACHE_MAX];
static size_t g_scope_n;

static u64 fnv1a(const char *s, u64 h) {
    if (!s)
        return h;
    // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound)
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    return h;
}

static u64 fnv_mix_u64(u64 h, u64 v) {
    for (unsigned i = 0; i < 8; i++) {
        h ^= (u64)(unsigned char)(v >> (i * 8));
        h *= 1099511628211ull;
    }
    return h;
}

/* S2: the memo key covers every envelope dimension that can change whether an
 * allow is valid: access, boundary, network, the ordered roots, the ordered
 * hosts, the flags word and the policy id. A memoized allow under one root (or
 * one policy backend) must never satisfy a call in a different envelope. */
static u64 scope_key(const AgcTrustEnv *e, const char *tool, const char *args) {
    u64 h = 1469598103934665603ull;
    h = fnv1a(tool, h);
    h = fnv1a("\x1f", h);
    h = fnv1a(args, h);
    h = fnv1a("\x1f", h);
    if (e) {
        h = fnv_mix_u64(h, (u64)e->access);
        h = fnv_mix_u64(h, (u64)e->boundary);
        h = fnv_mix_u64(h, (u64)e->network);
        h = fnv_mix_u64(h, (u64)e->nroots);
        for (size_t i = 0; i < e->nroots; i++) {
            h = fnv1a("\x1f", h);
            h = fnv1a(e->roots[i], h);
        }
        h = fnv_mix_u64(h, (u64)e->nhosts);
        for (size_t i = 0; i < e->nhosts; i++) {
            h = fnv1a("\x1f", h);
            h = fnv1a(e->hosts[i], h);
        }
        h = fnv_mix_u64(h, (u64)e->flags);
        h = fnv1a("\x1f", h);
        h = fnv1a(e->policy_id, h);
    }
    return h;
}

int agentc_trust_scope_get(const AgcTrustEnv *e, const char *tool, const char *args_json,
                           int *scope_out) {
    u64 k = scope_key(e, tool, args_json);
    for (size_t i = 0; i < g_scope_n; i++) {
        if (g_scope[i].hash != k)
            continue;
        if (scope_out)
            *scope_out = g_scope[i].scope;
        return g_scope[i].decision;
    }
    return -2; /* ENOENT: not memoized */
}

int agentc_trust_scope_put(const AgcTrustEnv *e, const char *tool, const char *args_json, int scope,
                           int decision) {
    if (scope == AGENTC_SCOPE_ONCE)
        return 0;
    if (decision == AGENTC_DEC_DENY)
        return 0; /* denies are never cached */
    u64 k = scope_key(e, tool, args_json);
    int rc = 0;
    bool inserted = true;
    for (size_t i = 0; i < g_scope_n; i++) {
        if (g_scope[i].hash != k)
            continue;
        g_scope[i].scope = scope;
        g_scope[i].decision = decision;
        inserted = false;
        break;
    }
    if (inserted) {
        if (g_scope_n >= AGENTC_APPROVAL_CACHE_MAX)
            return -28; /* ENOSPC */
        g_scope[g_scope_n].hash = k;
        g_scope[g_scope_n].scope = scope;
        g_scope[g_scope_n].decision = decision;
        g_scope_n++;
    }
    /* Project scope is the only disk-backed memo (C-D11): persist on change so
     * the decision survives the run. A write failure keeps the in-memory memo. */
    if (scope == AGENTC_SCOPE_PROJECT && g_approvals_persist && g_approvals_path)
        (void)agentc_trust_approvals_save(g_approvals_path);
    return rc;
}

void agentc_trust_scope_clear_step(void) {
    size_t w = 0;
    for (size_t i = 0; i < g_scope_n; i++)
        if (g_scope[i].scope != AGENTC_SCOPE_STEP)
            g_scope[w++] = g_scope[i];
    g_scope_n = w;
}

/* A session clear drops the in-memory scopes but not the disk-backed project
 * memos (C-D11); those are only replaced by a later load/save cycle. */
void agentc_trust_scope_clear_session(void) {
    size_t w = 0;
    for (size_t i = 0; i < g_scope_n; i++)
        if (g_scope[i].scope == AGENTC_SCOPE_PROJECT)
            g_scope[w++] = g_scope[i];
    g_scope_n = w;
}

/* ------------------------------------------- approvals.jsonc persistence */
static void scope_insert(u64 hash, int scope, int decision) {
    for (size_t i = 0; i < g_scope_n; i++) {
        if (g_scope[i].hash != hash)
            continue;
        g_scope[i].scope = scope;
        g_scope[i].decision = decision;
        return;
    }
    if (g_scope_n >= AGENTC_APPROVAL_CACHE_MAX)
        return;
    g_scope[g_scope_n].hash = hash;
    g_scope[g_scope_n].scope = scope;
    g_scope[g_scope_n].decision = decision;
    g_scope_n++;
}

int agentc_trust_approvals_load(const char *path) {
    if (!path || !path[0])
        return -22;
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text)
        return 0; /* absent/unreadable: no memos, never an error */
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(ja, text, len);
    agentc_free(text);
    if (agentc_json_type(root) != AGENTC_JSON_OBJ) {
        agentc_json_arena_free(ja);
        return 0; /* malformed: ignored (fail-closed) */
    }
    const AgcJson *projs = agentc_json_get(root, "projects");
    if (agentc_json_type(projs) != AGENTC_JSON_OBJ) {
        agentc_json_arena_free(ja);
        return 0;
    }
    /* Objects have no public length; iterate the key/value pairs until the
     * accessor reports the end, bounded by the memo cap. */
    for (size_t i = 0; i < AGENTC_APPROVAL_CACHE_MAX; i++) {
        const AgcJson *k = agentc_json_key_at(projs, i);
        const AgcJson *v = agentc_json_val_at(projs, i);
        if (!k || !v)
            break;
        const char *ks = k ? agentc_json_str(k, NULL) : NULL;
        if (!ks)
            continue;
        bool ok = false;
        u64 hash = agentc_parse_u64(ks, agentc_strlen(ks), &ok);
        if (!ok)
            continue;
        i64 dec = agentc_json_get_int(v, "decision", AGENTC_DEC_ALLOW);
        if (dec != AGENTC_DEC_ALLOW && dec != AGENTC_DEC_ASK)
            continue; /* no cached deny */
        scope_insert(hash, AGENTC_SCOPE_PROJECT, (int)dec);
    }
    agentc_json_arena_free(ja);
    return 0;
}

int agentc_trust_approvals_save(const char *path) {
    if (!path || !path[0])
        return -22;
    AgcBuf b = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "version");
    agentc_jsonw_u64(&w, 1);
    agentc_jsonw_key(&w, "projects");
    agentc_jsonw_obj(&w);
    for (size_t i = 0; i < g_scope_n; i++) {
        if (g_scope[i].scope != AGENTC_SCOPE_PROJECT)
            continue;
        char key[32];
        agentc_snprintf(key, sizeof key, "%llu", (unsigned long long)g_scope[i].hash);
        agentc_jsonw_key(&w, key);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "decision");
        agentc_jsonw_i64(&w, g_scope[i].decision);
        agentc_jsonw_key(&w, "scope");
        agentc_jsonw_cstr(&w, "project");
        agentc_jsonw_end(&w);
    }
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    int rc = agentc_write_file_atomic(path, b.p, b.len, 0644);
    agentc_buf_free(&b);
    return rc;
}

/* ------------------------------------------------------------- policy */
typedef struct {
    char *tool;
    char *path;
    char *cmd;
    char *host;
    char *field_key;
    char *field_val;
    int decision;
    char *policy_id;
    int scope;
    char *reason;
} AgcRule;

struct AgcPolicy {
    AgcRule *rules;
    size_t n, cap;
};

static void rule_free(AgcRule *r) {
    agentc_free(r->tool);
    agentc_free(r->path);
    agentc_free(r->cmd);
    agentc_free(r->host);
    agentc_free(r->field_key);
    agentc_free(r->field_val);
    agentc_free(r->policy_id);
    agentc_free(r->reason);
}

void agentc_policy_free(AgcPolicy *p) {
    if (!p)
        return;
    for (size_t i = 0; i < p->n; i++)
        rule_free(&p->rules[i]);
    agentc_free(p->rules);
    agentc_free(p);
}

static int rule_parse(AgcPolicy *p, const AgcJson *o) {
    if (agentc_json_type(o) != AGENTC_JSON_OBJ)
        return -22;
    if (p->n >= AGENTC_POLICY_MAX_RULES)
        return -28;
    if (p->n == p->cap) {
        size_t ncap = p->cap ? p->cap * 2 : 8;
        AgcRule *nr = agentc_alloc(ncap * sizeof *nr);
        agentc_memset(nr, 0, ncap * sizeof *nr);
        if (p->rules) {
            agentc_memcpy(nr, p->rules, p->n * sizeof *nr);
            agentc_free(p->rules);
        }
        p->rules = nr;
        p->cap = ncap;
    }
    AgcRule *r = &p->rules[p->n];
    agentc_memset(r, 0, sizeof *r);
    r->decision = AGENTC_DEC_ALLOW;
    const char *s = agentc_json_get_str(o, "tool");
    if (s)
        r->tool = agentc_strdup(s);
    if ((s = agentc_json_get_str(o, "path")))
        r->path = agentc_strdup(s);
    if ((s = agentc_json_get_str(o, "cmd")))
        r->cmd = agentc_strdup(s);
    if ((s = agentc_json_get_str(o, "host")))
        r->host = agentc_strdup(s);
    if ((s = agentc_json_get_str(o, "policy_id")))
        r->policy_id = agentc_strdup(s);
    if ((s = agentc_json_get_str(o, "reason")))
        r->reason = agentc_strdup(s);
    if ((s = agentc_json_get_str(o, "decision"))) {
        if (agentc_streq(s, "allow"))
            r->decision = AGENTC_DEC_ALLOW;
        else if (agentc_streq(s, "deny"))
            r->decision = AGENTC_DEC_DENY;
        else if (agentc_streq(s, "ask"))
            r->decision = AGENTC_DEC_ASK;
        else if (agentc_streq(s, "delegate"))
            r->decision = AGENTC_DEC_DELEGATE;
        else {
            rule_free(r);
            return -22;
        }
    }
    if ((s = agentc_json_get_str(o, "scope"))) {
        if (agentc_streq(s, "once"))
            r->scope = AGENTC_SCOPE_ONCE;
        else if (agentc_streq(s, "step"))
            r->scope = AGENTC_SCOPE_STEP;
        else if (agentc_streq(s, "session"))
            r->scope = AGENTC_SCOPE_SESSION;
        else if (agentc_streq(s, "project"))
            r->scope = AGENTC_SCOPE_PROJECT;
        else {
            rule_free(r);
            return -22;
        }
    }
    AgcJson *field = agentc_json_get(o, "field");
    if (agentc_json_type(field) == AGENTC_JSON_OBJ) {
        const AgcJson *k = agentc_json_key_at(field, 0);
        const AgcJson *v = agentc_json_val_at(field, 0);
        const char *ks = k ? agentc_json_str(k, NULL) : NULL;
        if (ks) {
            r->field_key = agentc_strdup(ks);
            size_t vn = 0;
            const char *vs = agentc_json_str(v, &vn);
            if (vs)
                r->field_val = agentc_strdup_len(vs, vn);
            else if (agentc_json_type(v) == AGENTC_JSON_TRUE)
                r->field_val = agentc_strdup("true");
            else if (agentc_json_type(v) == AGENTC_JSON_FALSE)
                r->field_val = agentc_strdup("false");
        }
    }
    p->n++;
    return 0;
}

int agentc_policy_add_json(AgcPolicy *p, const char *json) {
    if (!p || !json)
        return -22;
    if (agentc_strlen(json) > AGENTC_TRUST_MAX_POLICY_BYTES)
        return -27; /* EFBIG */
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *r = agentc_json_parse_in(ja, json, agentc_strlen(json));
    int rc = -22;
    if (agentc_json_type(r) == AGENTC_JSON_ARR) {
        rc = 0;
        for (size_t i = 0; i < agentc_json_len(r) && rc == 0; i++)
            rc = rule_parse(p, agentc_json_at(r, i));
    } else if (agentc_json_type(r) == AGENTC_JSON_OBJ) {
        AgcJson *rules = agentc_json_get(r, "rules");
        if (agentc_json_type(rules) == AGENTC_JSON_ARR) {
            rc = 0;
            for (size_t i = 0; i < agentc_json_len(rules) && rc == 0; i++)
                rc = rule_parse(p, agentc_json_at(rules, i));
        } else {
            rc = rule_parse(p, r);
        }
    }
    agentc_json_arena_free(ja);
    return rc;
}

AgcPolicy *agentc_policy_load(const char *path, char *err, size_t cap) {
    if (err && cap)
        err[0] = 0;
    if (!path)
        return NULL;
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text) {
        if (err && cap)
            agentc_snprintf(err, cap, "policy: cannot read %s", path);
        return NULL;
    }
    AgcPolicy *p = agentc_alloc(sizeof *p);
    agentc_memset(p, 0, sizeof *p);
    int rc = agentc_policy_add_json(p, text);
    agentc_free(text);
    if (rc != 0) {
        if (err && cap)
            agentc_snprintf(err, cap, "policy: malformed %s", path);
        agentc_policy_free(p);
        return NULL;
    }
    return p;
}

/* Evaluate one rule against (tool, args). Missing predicate = wildcard. */
static bool rule_matches(const AgcRule *r, const char *tool, const char *args_json) {
    if (r->tool && !agentc_streq(r->tool, "*") && !agentc_glob_match(r->tool, tool))
        return false;
    char path[PATH_MAX_];
    bool has_path = args_path(args_json, path, sizeof path);
    if (r->path && (!has_path || !agentc_glob_match(r->path, path)))
        return false;
    if (r->cmd) {
        char cmd[PATH_MAX_];
        cmd[0] = 0;
        if (args_json) {
            AgcJsonArena *ja = agentc_json_arena_new(0);
            AgcJson *o = agentc_json_parse_in(ja, args_json, agentc_strlen(args_json));
            const char *cv =
                agentc_json_type(o) == AGENTC_JSON_OBJ ? agentc_json_get_str(o, "command") : NULL;
            if (cv)
                agentc_snprintf(cmd, sizeof cmd, "%s", cv);
            agentc_json_arena_free(ja);
        }
        if (!agentc_glob_match(r->cmd, cmd))
            return false;
    }
    if (r->host) {
        char host[512];
        if (!args_host(args_json, host, sizeof host))
            return false;
        if (!agentc_glob_match(r->host, host))
            return false;
    }
    if (r->field_key) {
        if (!args_json)
            return false;
        AgcJsonArena *ja = agentc_json_arena_new(0);
        AgcJson *o = agentc_json_parse_in(ja, args_json, agentc_strlen(args_json));
        const AgcJson *v =
            agentc_json_type(o) == AGENTC_JSON_OBJ ? agentc_json_get(o, r->field_key) : NULL;
        bool ok = false;
        if (v) {
            if (r->field_val) {
                size_t vn = 0;
                const char *vs = agentc_json_str(v, &vn);
                if (vs)
                    ok = r->field_val &&
                         agentc_str_eq(vs, vn, r->field_val, agentc_strlen(r->field_val));
            } else {
                ok = true;
            }
        }
        agentc_json_arena_free(ja);
        if (!ok)
            return false;
    }
    return true;
}

int agentc_policy_eval(const AgcPolicy *p, const char *tool, const char *args_json,
                       AgcTrustDecision *out) {
    if (!out)
        return -22;
    agentc_memset(out, 0, sizeof *out);
    out->decision = AGENTC_DEC_ALLOW;
    out->scope = AGENTC_SCOPE_ONCE;
    if (!p || !tool)
        return -2; /* ENOENT: no matching rule */
    for (size_t i = 0; i < p->n; i++) {
        if (!rule_matches(&p->rules[i], tool, args_json))
            continue;
        out->decision = p->rules[i].decision;
        out->reason = p->rules[i].reason;
        out->policy_id = p->rules[i].policy_id;
        out->scope = p->rules[i].scope;
        return 0;
    }
    return -2; /* ENOENT */
}

/* ------------------------------------------------- process policy store */
static AgcPolicy *g_policy;

const AgcPolicy *agentc_trust_policy(void) {
    return g_policy;
}

void agentc_trust_policy_clear(void) {
    agentc_policy_free(g_policy);
    g_policy = NULL;
}

static int policy_ensure(AgcPolicy **out) {
    if (g_policy == NULL) {
        g_policy = agentc_alloc(sizeof *g_policy);
        if (g_policy == NULL)
            return -12;
        agentc_memset(g_policy, 0, sizeof *g_policy);
    }
    *out = g_policy;
    return 0;
}

int agentc_trust_policy_add_json(const char *json) {
    if (!json)
        return -22;
    AgcPolicy *p = NULL;
    int rc = policy_ensure(&p);
    if (rc != 0)
        return rc;
    return agentc_policy_add_json(p, json);
}

int agentc_trust_policy_load(const char *path, char *err, size_t cap) {
    if (err && cap)
        err[0] = 0;
    if (!path || !path[0])
        return -22;
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text)
        return -2; /* absent/unreadable: nothing to load */
    AgcPolicy *p = NULL;
    int rc = policy_ensure(&p);
    if (rc == 0)
        rc = agentc_policy_add_json(p, text);
    agentc_free(text);
    if (rc != 0 && err && cap)
        agentc_snprintf(err, cap, "policy: malformed %s", path);
    return rc;
}

/* ---------------------------------------------------------------- audit */
static AgcTrustAuditFn g_audit_fn;
static void *g_audit_ud;
static bool g_audit_set;

void agentc_trust_set_audit(AgcTrustAuditFn fn, void *ud) {
    g_audit_fn = fn;
    g_audit_ud = ud;
    g_audit_set = true;
}

void agentc_trust_audit(const char *kind, const char *payload_json) {
    if (!g_audit_set || !g_audit_fn)
        return;
    (void)kind;
    if (!payload_json)
        return;
    g_audit_fn(g_audit_ud, payload_json, agentc_strlen(payload_json));
}
