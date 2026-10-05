/* trust.h — trust envelopes, static policy, approvals and sandbox specs (P6-C).
 *
 * The envelope is the per-agent boundary: access (read-only / read-write),
 * boundary (none / workspace / strict), approval (auto / ask / policy),
 * network (deny / allow / allowlist), workspace roots, network hosts and a
 * flags word. An envelope only ever narrows: a child is an intersection of its
 * parent, and the root is the immutable ceiling (P6-TRUST §3).
 *
 * Everything here is pure C over the base library: the only Layer-0 calls are
 * os_realpath (path checks) and the sandbox hooks. Fail-closed: a malformed
 * request, a widening request, a missing approver or a sandbox failure is a
 * denial, never an allow.
 *
 * Append-only (R-D10): this is a new header, included by agent.h at the end.
 */
#ifndef AGENTC_TRUST_H
#define AGENTC_TRUST_H

#include "agentc.h"
#include "actor.h"
#include "plat.h"

/* ------------------------------------------------------------------ enums */
typedef enum {
    AGENTC_ACCESS_RO = 0, /* read-only */
    AGENTC_ACCESS_RW = 1, /* read-write (default) */
} AgcAccess;

typedef enum {
    AGENTC_BOUND_NONE = 0,      /* no kernel confinement */
    AGENTC_BOUND_WORKSPACE = 1, /* fs writes limited to roots + tmp (default) */
    AGENTC_BOUND_STRICT = 2,    /* read-only fs unless granted; net denied */
} AgcBoundary;

typedef enum {
    AGENTC_APPROVAL_AUTO = 0,   /* trust the model inside the boundary (default) */
    AGENTC_APPROVAL_ASK = 1,    /* interactive prompt on cross-boundary calls */
    AGENTC_APPROVAL_POLICY = 2, /* delegate to a policy program/model */
} AgcApproval;

typedef enum {
    AGENTC_NET_DENY = 0,
    AGENTC_NET_ALLOW = 1,
    AGENTC_NET_ALLOWLIST = 2,
} AgcNetwork;

#define AGENTC_TRUST_ALLOW_PTY 0x1u
#define AGENTC_TRUST_ALLOW_BACKGROUND 0x2u
#define AGENTC_TRUST_ALLOW_SUBAGENT 0x4u
#define AGENTC_TRUST_ALLOW_NET_UNATTENDED 0x8u
/* P6-E voice capabilities (R-D27): denied by default. */
#define AGENTC_TRUST_ALLOW_MIC 0x10u
#define AGENTC_TRUST_ALLOW_SPEAKER 0x20u

/* ------------------------------------------------------------------ env */
typedef struct AgcTrustEnv {
    int access;   /* AgcAccess */
    int boundary; /* AgcBoundary */
    int approval; /* AgcApproval */
    int network;  /* AgcNetwork */
    const char **roots;
    size_t nroots; /* owned by the envelope */
    const char **hosts;
    size_t nhosts;         /* owned by the envelope */
    const char *policy_id; /* owned by the envelope */
    uint32_t flags;        /* AGENTC_TRUST_* */
    /* lineage; filled by the registry, never requested by a child */
    AgcActorId self, parent;
    uint16_t depth;
} AgcTrustEnv;

/* ------------------------------------------------------------- decisions */
typedef enum {
    AGENTC_DEC_ALLOW = 0,
    AGENTC_DEC_DENY,
    AGENTC_DEC_ASK,
    AGENTC_DEC_DELEGATE,
} AgcDecision;

typedef enum {
    AGENTC_SCOPE_ONCE = 0,
    AGENTC_SCOPE_STEP,
    AGENTC_SCOPE_SESSION,
    AGENTC_SCOPE_PROJECT,
} AgcScope;

typedef struct {
    int decision;          /* AgcDecision */
    const char *reason;    /* borrowed for a static verdict, owned for a hook verdict */
    const char *args_json; /* optional narrowed args (owned when set by hook/approver) */
    int scope;             /* AgcScope */
    const char *policy_id; /* when DELEGATE */
} AgcTrustDecision;

/* ------------------------------------------------------------ sandbox spec */
/* AgcSandboxSpec is declared in plat.h (Layer 0 needs it); this alias keeps the
 * trust API readable. The roots/hosts pointers are borrowed from the envelope. */

/* ---------------------------------------------------------------- policy */
typedef struct AgcPolicy AgcPolicy; /* opaque compiled rule set */

/* ------------------------------------------------------------- approvals */
typedef struct {
    uint64_t id;
    AgcActorId actor_id, parent_agent_id;
    uint16_t depth;
    const char *tool;
    const char *input_json; /* borrowed; hashed for audit, capped */
    const char *trigger;    /* destructive|outside-roots|network|rule:<id> */
    const AgcTrustEnv *env; /* effective, borrowed */
    i64 deadline_ns;
} AgcApprovalRequest;

typedef struct {
    uint64_t id;
    int decision; /* AgcDecision */
    const char *reason;
    const char *args_json;
    int scope;
    const char *add_rule_json; /* optional persisted rule */
} AgcApprovalResult;

/* interactive approval UI sink (front end); returns 0 if answered, -ENOSYS none */
typedef int (*AgcApprovalAskFn)(void *ud, const AgcApprovalRequest *req, AgcApprovalResult *out);
/* audit sink: append one complete custom JSON line to the session */
typedef void (*AgcTrustAuditFn)(void *ud, const char *json, size_t n);

/* ------------------------------------------------------------ envelope API */
int agentc_trust_default(AgcTrustEnv *out, const char *cwd);
int agentc_trust_load(const char *cwd, bool project_trusted, AgcTrustEnv *out);
int agentc_trust_narrow(const AgcTrustEnv *parent, const AgcTrustEnv *req, AgcTrustEnv *out,
                        char *err, size_t cap);
int agentc_trust_env_clone(const AgcTrustEnv *in, AgcTrustEnv *out);
void agentc_trust_env_free(AgcTrustEnv *e);
int agentc_trust_from_json(const char *json, size_t n, AgcTrustEnv *out, char *err, size_t cap);
int agentc_trust_to_json(const AgcTrustEnv *e, AgcBuf *out);
int agentc_trust_describe(const AgcTrustEnv *e, AgcBuf *out);
int agentc_trust_sandbox_spec(const AgcTrustEnv *e, AgcSandboxSpec *out);
bool agentc_trust_path_ok(const AgcTrustEnv *e, const char *path, bool write);
bool agentc_trust_host_ok(const AgcTrustEnv *e, const char *host);
int agentc_trust_decide(const AgcTrustEnv *e, const char *tool, const char *args_json,
                        AgcTrustDecision *out);
int agentc_trust_filter_tools(const AgcTrustEnv *e, struct AgcTool *tools, size_t *n);

/* widen policy (trust.widen). Default deny; clamp intersects instead. */
void agentc_trust_set_widen_clamp(bool clamp);
bool agentc_trust_widen_clamp(void);

/* CLI overrides for agentc_trust_load; NULL/-1 = unset. */
void agentc_trust_set_cli(const char *access, const char *sandbox, const char *approval,
                          const char *policy, const char *network);

/* process-global current envelope (set by the app) and the spawn-spec helper. */
void agentc_trust_set_current(const AgcTrustEnv *e);
const AgcTrustEnv *agentc_trust_current(void);
const AgcSandboxSpec *agentc_trust_spawn_spec(void); /* NULL = unconfined */

/* --------------------------------------------------------------- policy */
AgcPolicy *agentc_policy_load(const char *path, char *err, size_t cap);
int agentc_policy_add_json(AgcPolicy *p, const char *json);
void agentc_policy_free(AgcPolicy *p);
int agentc_policy_eval(const AgcPolicy *p, const char *tool, const char *args_json,
                       AgcTrustDecision *out);

/* Process policy store (P6 completion). Rules are accumulated at startup from
 * the user policy file, a trusted project policy file and the `policy_rules`
 * hook contribution; the store owns the merged rule set and lives for the
 * process. `agentc_trust_gate` evaluates it when no hard layer-2 deny applies. */
int agentc_trust_policy_load(const char *path, char *err, size_t cap);
int agentc_trust_policy_add_json(const char *json);
const AgcPolicy *agentc_trust_policy(void);
void agentc_trust_policy_clear(void);

/* ------------------------------------- memoization (tool, args, path, host) */
int agentc_trust_scope_get(const AgcTrustEnv *e, const char *tool, const char *args_json,
                           int *scope_out);
int agentc_trust_scope_put(const AgcTrustEnv *e, const char *tool, const char *args_json, int scope,
                           int decision);
void agentc_trust_scope_clear_step(void);
void agentc_trust_scope_clear_session(void);

/* Project-scope approval memo persistence (P6 completion, C-D11): a separate
 * `approvals.jsonc` holding only scope==project entries, read at startup and
 * rewritten on each project-scope put (when trust.approval.persist is on).
 * once/step/session stay in memory. An absent/unreadable file loads nothing;
 * a malformed file is ignored (never trusted). */
int agentc_trust_approvals_load(const char *path);
int agentc_trust_approvals_save(const char *path);

/* -------------------------------------------------------------- approval */
int agentc_approval_request(const AgcApprovalRequest *req, AgcApprovalResult *out);
int agentc_approval_submit(const char *response_json);
int agentc_approval_pending(const AgcApprovalRequest **out, size_t max);
int agentc_approval_cancel(uint64_t id, const char *reason);
bool agentc_approval_is_pending(uint64_t id);
void agentc_approval_set_ui(AgcApprovalAskFn fn, void *ud);
int agentc_approval_register_policy(const char *id, const char *kind, const char *command);
void agentc_approval_shutdown(void);
void agentc_trust_set_audit(AgcTrustAuditFn fn, void *ud);
void agentc_trust_audit(const char *kind, const char *payload_json);
/* Build the §7.1 request JSON into `out` (bounded). */
int agentc_approval_request_json(const AgcApprovalRequest *req, AgcBuf *out);
/* Parse a §7.1 response; malformed = deny (returns 0 with decision DENY). */
int agentc_approval_parse_response(const char *json, size_t n, AgcApprovalResult *out);
uint64_t agentc_approval_next_id(void);

/* Gate used by the agent tool batch: decide, policy, then ASK/DELEGATE. */
/* Declared in agent.h because it takes AgcToolCall. */

/* Appended WS2 (review-hardening, S4/D2): the tool capability bit the trust
 * classifier honors for tools that reach the network. It shares the
 * AgcTool.flags namespace with AGENTC_TOOL_*; a tool that declares it is
 * classified as a network tool even when its name is unknown. Tools that
 * predate the bit keep the conservative built-in name-list fallback. */
#define AGENTC_TRUST_TOOL_NET 0x80u

/* Appended WS2 (review-hardening, S4/D2): flag-aware verdict. `flags` is the
 * resolved tool's AgcTool.flags, so capability (not the tool name) drives the
 * classification. Zero/unknown flags mean "mutating" (fail closed). The
 * legacy name-only agentc_trust_decide() forwards flags=0 and is therefore
 * conservative. */
int agentc_trust_decide_flags(const AgcTrustEnv *e, const char *tool, uint32_t flags,
                              const char *args_json, AgcTrustDecision *out);

/* Appended WS-G1 (review-hardening, finding 6): the gate for a tool that
 * declares AGENTC_TOOL_NO_APPROVAL. Runs layer-2 decide_flags and the process
 * policy exactly like agentc_trust_gate, then collapses a non-terminal
 * ASK/DELEGATE to ALLOW instead of dispatching an approver. A hard DENY is
 * never relaxed. `out->reason` is always an owned copy. */
int agentc_trust_decide_skip_approval(const AgcTrustEnv *e, const char *tool, uint32_t flags,
                                      const char *args_json, AgcTrustDecision *out);

#endif /* AGENTC_TRUST_H */
