# P6-TRUST — Trust envelopes, sandbox, approvals, delegation

Status: **as-built (P6-A..E + completion passes 2a-2g).** Trust envelopes, sandbox, approvals and delegation.
As-built note: the outcome is folded into `.agents/design/10-platform.md` and `.agents/design/20-core-agent.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

Scope: `AgcTrustEnv`; inherit vs own; nesting and monotonicity; read-only and
read-write enforcement at four layers; fine-grained per-call rules; the approval
contract and its three delivery forms (interactive, external program, reviewer
model such as `jev`); per-platform sandbox application; audit; config/CLI/RPC;
tests.

Non-goals: actor/process mechanics (`P6-ACTORS.md`); queue/steer
(`P6-SIGNALS.md`); the agent constructor (`P6-SUBAGENTS.md`).

---

## 1. Current state (facts to build on)

- **Project trust** exists: `include/config.h` `agentc_trust_resolve`,
  `agentc_trust_save`, `~/.config/abird/agentc/trust.jsonc`, the `project_trust`
  replace-override hook (`AGENTC_HEV_PROJECT_TRUST`), and CLI
  `--approve`/`--no-approve`. It gates whether project `resources`, `config`,
  `prompts`, `themes`, `MCP` load. It is a *loading* decision, not a runtime
  permission.
- **Tool veto** exists: `AgcToolVetoFn` / `AgcToolVetoDecision` (`include/agent.h`)
  with `block`, `reason`, `args_json` (replacement), `terminate`. It is a
  synchronous chain-veto, no async, no scope/caching, no prompt. The extension
  bridge implements it via the `tool_call` hook (`src/ext/registry.c`).
- **Tool flags**: `AGENTC_TOOL_READONLY`, `AGENTC_TOOL_DESTRUCTIVE`,
  `AGENTC_TOOL_SEQUENTIAL`, `AGENTC_TOOL_HIDDEN`, `AGENTC_TOOL_CORE`.
- There is **no** sandbox, no read-only runtime mode, no approval dialog, and no
  delegation. `os_spawn_group` runs children unrestricted.
- `src/core/policy.c` selects the active tool table (`--tools` > `default_tools`
  > all) and hides non-core destructive tools unless named.

## 2. Trust envelope

```c
/* include/agent.h (append-only block, or a new include/trust.h included by it) */
typedef enum {
    AGENTC_ACCESS_RO = 0,     /* read-only */
    AGENTC_ACCESS_RW = 1,     /* read-write (default) */
} AgcAccess;

typedef enum {
    AGENTC_BOUND_NONE = 0,    /* no confinement */
    AGENTC_BOUND_WORKSPACE = 1, /* fs writes limited to roots + tmp; net per policy (default) */
    AGENTC_BOUND_STRICT = 2,  /* RO fs unless granted; net denied unless allowed */
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

typedef struct AgcTrustEnv {
    int          access;
    int          boundary;
    int          approval;
    int          network;
    const char **roots;  size_t nroots;   /* owned by the registry; workspace roots */
    const char **hosts;  size_t nhosts;   /* owned; allowlist when network==ALLOWLIST */
    const char  *policy_id;               /* named policy or inline JSONC; owned */
    uint32_t     flags;                    /* AGENTC_TRUST_* below */
    /* lineage (filled by the registry; not part of the requested envelope) */
    AgcActorId   self, parent;
    uint16_t     depth;
} AgcTrustEnv;

#define AGENTC_TRUST_ALLOW_PTY        0x1u
#define AGENTC_TRUST_ALLOW_BACKGROUND 0x2u
#define AGENTC_TRUST_ALLOW_SUBAGENT   0x4u
#define AGENTC_TRUST_ALLOW_NET_UNATTENDED 0x8u
```

Rules:

- The envelope is **per agent actor**, not process-global. Multiple envelopes
  coexist (single process, single thread).
- `access=RO` is the coarse read-only switch; `access=RW` is the default.
- `boundary` is the sandbox scope. `none` means no kernel confinement (only the
  static policy layer applies); `workspace` confines writes to `roots` (+
  `$TMPDIR`, unless `exclude_tmp`); `strict` is read-only filesystem plus
  network denied unless explicitly allowed.
- `approval=auto` is the "trust the model" default: inside the boundary nothing
  prompts. `ask`/`policy` only affect calls that would cross the boundary or
  match a rule that requests review.

### 2.1 Resolution and defaults

Effective envelope = merge, highest wins, then validate:

```text
CLI flags  >  project policy (trusted only)  >  user policy  >  built-in default
```

Built-in default (R-D4): `access=RW, boundary=WORKSPACE, approval=AUTO,
network=DENY, roots=[cwd], flags=ALLOW_PTY|ALLOW_BACKGROUND|ALLOW_SUBAGENT`.

- An **untrusted** project may only *tighten* the effective envelope; a
  tightening contributes, a loosening is ignored with a log. This reuses the
  existing trust gate.
- `roots` default to the session cwd. `$HOME` is not a root by default; reads
  outside roots are permitted (they are not writes) unless `boundary=strict`.

## 3. Inheritance, own, nesting, monotonicity

At agent creation (`P6-SUBAGENTS.md`), the spec chooses:

- **inherit** (default): the child's effective envelope equals the parent's,
  with `self/parent/depth` updated.
- **own**: the child supplies a requested envelope. It is validated as an
  intersection of the parent's:

```text
requested.access   <= parent.access          (RO cannot become RW)
requested.boundary >= parent.boundary        (cannot loosen the sandbox)
requested.network  <= parent.network         (ALLOW cannot become ALLOW if parent DENY)
requested.roots    subset-of parent.roots
requested.flags    subset-of parent.flags
requested.approval >= parent.approval        (auto < ask < policy on scrutiny)
```

A widening request is denied by default (`trust.widen` config: `deny|clamp`).
Escaping the **root ceiling** (the root envelope) requires an explicit approval
at the spawn call; the grant is recorded and audited. Nested boundaries are
ordinary: a grandchild narrows a child.

The root envelope is computed once at startup and is immutable for the session.
`AGENTC_EV_TRUST_SET` reports every effective envelope change (only narrowing is
possible after startup).

## 4. The four enforcement layers (defense in depth)

Every tool call flows through all four; each can only tighten:

1. **Declaration.** The envelope is rendered into the system prompt (extend the
   `tools`/`rules` sections). In `access=RO`, `write`/`edit` are hidden and the
   `bash` description states writes are denied. In `boundary=strict`, network
   tools are hidden/denied. *Advisory only.*
2. **Static policy (pure C).** `agentc_trust_decide(env, call) → decision` — the
   authoritative in-process gate. Deterministic, no I/O, golden-testable. It:
   - enforces RO against mutation tools (`write`, `edit`) and against any
     mutation-flagged call;
   - resolves path-like args and checks them against `roots` for writes;
   - checks `AGENTC_TOOL_DESTRUCTIVE`; in `auto` it **allows** inside the
     boundary and flags cross-boundary calls;
   - checks network hosts against `network`/`hosts`;
   - checks `ALLOW_PTY`/`ALLOW_BACKGROUND`.
3. **Hook / delegation.** `tool_approval` (new; §7) is reached only when layer 2
   returns `ask`/`delegate` or a rule demands it. It may be interactive, an
   external program, or a model.
4. **Enforcement (kernel).** Every external process spawned for the actor gets
   the effective sandbox applied at fork (`os_sandbox_apply`, §8). In-process
   fs ops (`read`/`write`/`edit`/`find`/`grep`) re-check `roots` after realpath.
   This is the load-bearing layer: a lying model or a buggy hook cannot escape.

### 4.1 Decision type and precedence

```c
typedef enum {
    AGENTC_DEC_ALLOW = 0,
    AGENTC_DEC_DENY,
    AGENTC_DEC_ASK,
    AGENTC_DEC_DELEGATE,
} AgcDecision;

typedef struct {
    int         decision;
    const char *reason;    /* owned by the caller when from a hook */
    const char *args_json; /* optional narrowed args */
    int         scope;     /* once|step|session|project */
    const char *policy_id; /* when DELEGATE */
} AgcTrustDecision;
```

Precedence: hard deny (config/root/rules with `deny`) > explicit user deny >
explicit allow rule > delegated verdict > interactive ask > mode default
(auto-allow inside boundary). A deny is absolute; a delegate can only speak
within what layer 2 permits (a `DENY` from layer 2 is never sent to a delegate).

## 5. Read-only and read-write

- **RW (default):** mutation allowed inside the boundary; no prompt.
- **RO:** layer 1 hides write tools and layer 2 denies any mutation; layer 4
  denies fs writes at the kernel for external processes. Reads are unlimited
  within what the sandbox permits. A `bash` command that writes fails with a
  clear error naming the access mode. This is both a declaration and an
  enforcement concept — RO is not merely a hidden tool.
- **RO + subagent:** a child of an RW parent may be RO (narrowing). An RW child
  of an RO parent is impossible (monotonicity).

## 6. Fine-grained per-call rules

A rule is an ordered predicate:

```jsonc
{
  "tool": "bash",            // name or "*"
  "path": "src/**",          // resolved-path glob (writes)
  "cmd":  "rm\\s+-rf",       // regex/glob over bash.command
  "host": "*.example.com",   // network hosts
  "field": { "recursive": true },   // dotted arg equality
  "decision": "allow|deny|ask|delegate",
  "policy_id": "jev",        // when delegate
  "scope": "once|step|session|project",
  "reason": "…"
}
```

- Evaluation is first-match-wins in declaration order; a rule may be
  contributed by an extension through a new `policyPaths` field on
  `AgcExtResult` (mirroring `resources_discover`) or through the config.
- Sources: `~/.config/abird/agentc/policy.jsonc`, trusted `.agentc/policy.jsonc`
  (tighten-only if untrusted project), `--policy <file|id>`, and inline
  `trust.policy_id`.
- Rules compose per agent: parent rules apply to children; children add
  narrowing rules only.
- **Memoization:** a decision with scope > `once` is cached for the session by a
  key of `(tool, normalized args, path class, host)` so identical calls do not
  re-prompt. `project` scope persists to `trust.jsonc` under the project hash.

## 7. Approvals and delegation

### 7.1 The contract (language-agnostic, stable)

**Request** (core → approver):

```json
{
  "type": "approval_request",
  "id": "apr_…",
  "session_id": "…",
  "agent_id": 12, "parent_agent_id": 1, "depth": 1,
  "tool": "bash",
  "input": { "command": "rm -rf build" },
  "effective_trust": {
    "access": "read-write", "boundary": "workspace",
    "approval": "policy", "roots": ["/repo"], "network": "deny",
    "flags": ["allow_pty", "allow_background", "allow_subagent"]
  },
  "trigger": "destructive|outside-roots|network|rule:<policy_id>",
  "options": ["allow", "deny", "ask"],
  "reason_required": true
}
```

**Response** (approver → core):

```json
{ "id": "apr_…", "decision": "allow|deny|ask",
  "reason": "…", "scope": "once|step|session|project",
  "args": { "command": "rm -rf ./build" } }
```

- `args` narrows the executed call (reuses the existing `args_json`
  replacement).
- Unknown fields are ignored; a malformed response is a **deny** (fail-closed).
- Timeout (`trust.approval_timeout_ms`, proposed 120000) → deny with a reason.

### 7.2 Delivery forms (one hook, three backends)

The core emits `tool_approval` and does not know which backend answers.

1. **Interactive** (`approval=ask`, or a delegate returning `ask`): the front
   end (TUI/RPC) renders a dialog; the core blocks in a pump-driven wait
   (`agentc_pump(20)` slices) so repaint/input/abort stay live, exactly like
   `bash_step_once`.
2. **External program** (`approval=policy` with a command policy): spawn via
   `os_spawn_group` under the actor's sandbox, write the request JSON to stdin,
   read one JSON line from stdout. Exit code: `0` = use the response JSON,
   non-zero = no opinion → escalates to `ask` (or deny if non-interactive). This
   is "passed into other programs", including a repo's own
   `scripts/approve` or an enterprise policy engine. A one-shot CLI form
   (`agentc policy-check < request.json`) exposes the same contract to
   non-agent tooling.
3. **Reviewer model (e.g. `jev`)** (`approval=policy` with `policy_id:"jev"`):
   an extension maps the request to a provider/model call (using the existing
   extension `http_request` + `defer`/`is_cancelled` host services) with a
   reviewer system prompt and a strict parser, then submits the response JSON.
   Because it is an extension, `jev` can be a linked Rust extension, an
   MCP-connected policy server, or a separate process. No core change.

`tool_approval` is asynchronous: the hook returns "pending" and the actor enters
`WAITING`; the answer arrives as a signal that resumes it. The existing
`defer`/`is_cancelled` services are the mechanism.

### 7.3 Routing

- An approval for a child is routed to the approver resolved for **that child's
  envelope**, falling back up the parent chain to the root.
- A child under `approval=auto` never prompts, even if the root is `ask`
  (narrowing only makes scrutiny higher, never lower — but `auto` is the lowest
  and a child cannot be lower than its parent; a root `ask` therefore forces
  children to `ask` or `policy`).
- Pending approvals are cancelled when the actor is cancelled (`CANCELLED`
  resolves any pending approval as deny).

**Generalization:** `approve` is one *decision point* of the general decision
seam (`P6-DECIDERS.md`). The request/response JSON in §7.1 is the `approve`
form of the shared decision contract; the classified router is the same
mechanism at the `route` point, and jev is a `model` decider backend. A decision
may only **narrow** the envelope; widening is denied or escalated. Nothing here
adds a primitive — deciders are configured backends over the envelope/approval
primitive plus the `DECIDE`/`DECIDED` signal kinds. Approval is a handler on the
`tool`/`call` stage of the `P6-HOOKS` pipeline (`deciders`/jev are branch
handlers).

## 8. Sandbox enforcement per platform

Layer-0 addition:

```c
/* Apply the effective sandbox to the calling thread/process before execve, or
 * to a spawned child. `spec` is the resolved envelope in a flat form.
 * -> 0 | -errno. Failure is fatal for `strict`, denial for `workspace`. */
typedef struct AgcSandboxSpec {
    int access;                  /* AgcAccess */
    int boundary;                /* AgcBoundary */
    int network;                 /* AgcNetwork */
    const char **writable_roots; size_t nroots;
    const char **allowed_hosts;  size_t nhosts;
    bool exclude_tmp;
} AgcSandboxSpec;
int os_sandbox_apply(const AgcSandboxSpec *spec, char *err, size_t errcap);
```

- **Linux:** Landlock rulesets for the fs (write to `writable_roots`, read
  allowed per boundary) plus a seccomp-bpf denylist; network denial via seccomp
  or a network namespace. Apply between `fork` and `execve` using the existing
  `os_spawn_group` seam. (bwrap is an alternative backend behind the same call;
  choose the minimum that honors RO/workspace — see P6-RUNTIME §9 open item.)
- **macOS:** `sandbox_init`/seatbelt profile generated from `spec`.
- **Windows:** restricted token + job object (job objects already exist for
  `os_spawn_group`), optional low integrity; network via Windows Firewall policy
  or a restricted token capability set.
- **In-process tools** enforce `roots` after realpath (defeat `..`/symlinks) for
  `read`/`write`/`edit`/`find`/`grep`.
- Failure to apply: `strict` → fatal (spawn denied); `workspace` → spawn denied
  with a clear error; never a silent unsandboxed run. A platform without a
  backend returns `-ENOSYS` and the app refuses `boundary != none` with an
  actionable message (or runs `none` only if the user explicitly asked).

## 9. Bounds and fail-closed

- `roots`/`hosts` arrays bounded (`AGENTC_TRUST_MAX_ROOTS` 16,
  `AGENTC_TRUST_MAX_HOSTS` 32); overflow truncates with a log.
- `policy.jsonc` and rules bounded (`AGENTC_POLICY_MAX_RULES` 256).
- Approval request/response size bounded (`AGENTC_APPROVAL_MAX_BYTES` 64 KiB).
- Missing/malformed approver, timeout, sandbox failure, rule parse error →
  **deny** (fail-closed). Never allow on error.
- Denials are not fatal: they surface to the model as a tool error with the
  reason, so the model can adjust.

## 10. Audit, events, session

- Events (append after the actor events): `AGENTC_EV_TRUST_SET`,
  `AGENTC_EV_APPROVAL_REQUEST`, `AGENTC_EV_APPROVAL_RESULT`,
  `AGENTC_EV_SANDBOX_DENIED`.
- Hook points: `AGENTC_HEV_TRUST_SELECT` (observe/narrow at child creation),
  `AGENTC_HEV_TOOL_APPROVAL` (the contract), `AGENTC_HEV_SANDBOX_DENIED`
  (observe), `AGENTC_HEV_POLICY_RULES` (contribute rules at load).
- Every approve/deny/violation is appended as a `custom` session record
  (`type:"approval"`) with `{agent_id, tool, input_hash, decision, reason,
  scope, policy_id}`. No raw secrets; `input` is hashed and capped.
- `trust.jsonc` gains a `projects.<hash>` block for `project`-scope approvals
  and the saved default envelope. The existing project-trust verdict is
  unchanged and still gates config/resource loading.

## 11. Config, CLI, RPC

Config (`config.jsonc`, user + trusted project):

```jsonc
"trust": {
  "access": "read-write",
  "sandbox": "workspace",
  "approval": "auto",
  "network": "deny",
  "widen": "deny",
  "roots": ["."],
  "approval_timeout_ms": 120000,
  "subagent": { "inherit": true, "max_depth": 3 }
},
"policies": {
  "jev": { "kind": "model", "provider": "openai", "model": "gpt-5-codex" },
  "corp": { "kind": "program", "command": "/usr/local/bin/corp-approve" }
}
```

CLI: `--access read-only|rw`, `--sandbox none|workspace|strict`,
`--approval auto|ask|policy`, `--policy <id|file>`, `--network deny|allow`.
`--approve`/`--no-approve` keep their project-trust meaning and also seed the
project default envelope.

RPC: `get_trust`, `set_trust`, `approve {id, decision, scope}`, `deny {id,
reason}`, `set_policy`; approval request/result ride the event stream so a host
UI can answer.

## 12. Failure modes

| Failure | Behavior |
| --- | --- |
| widening request | denied (or clamped per config); logged + audited |
| untrusted project policy tries to loosen | ignored (tighten-only) |
| RO mutation | layer 1 hides, layer 2 denies, layer 4 blocks at kernel; tool error |
| path escapes roots | layer 2 denies after realpath; denial is a tool error |
| sandbox apply fails | spawn denied; `strict` fatal; never silent |
| approver missing/timeout/malformed | deny (fail-closed) |
| delegate returns out-of-range decision | clamped to deny; logged |
| approval for a cancelled actor | resolved as deny; no leak |
| rule parse error | rule dropped + warning; if it was the only rule, default applies, not allow-on-error |
| network not allowlisted | denied with host+policy in the reason |

## 13. Golden test plan (new `tests/trust_test.c` + `.expected`)

- `agentc_trust_decide` truth table: `{RO,RW} × {none,workspace,strict} ×
  {auto,ask,policy}` × {path inside/outside/`..`/symlink, cmd destructive/benign,
  host allowed/denied}.
- Monotonicity matrix for `own`: every widening combination → deny/clamp as
  configured; every narrowing combination accepted.
- Rule evaluation: first-match-wins, field predicates, path globs, host globs,
  malformed rule dropped.
- Approval contract: interactive allow/deny; program allow/deny/narrow; malformed
  and timeout → deny; scope memoization (second identical call does not ask).
- Sandbox: a mock backend records the applied spec; a write outside roots fails
  and emits `sandbox_denied` + a tool error.
- Audit: approve/deny append the expected `custom` records (byte-exact).
- Fail-closed: no approver configured + `policy` → deny.

## 14. Acceptance for P6-C

1. `AgcTrustEnv`, resolution/merge, inherit/own with monotonicity, and the
   `trust.widen` policy exist.
2. RO/RW enforced at all four layers; a real RO session cannot write via any
   tool.
3. `os_sandbox_apply` implemented per platform as far as §8 requires; failure is
   fail-closed; `boundary=none` documented as the only unsandboxed mode.
4. `tool_approval` + the JSON contract work through all three delivery forms;
   scopes memoize; malformed/timeout deny.
5. `AGENTC_EV_TRUST_SET`/`APPROVAL_*`/`SANDBOX_DENIED` and the `trust_*`/
   `tool_approval`/`sandbox_denied`/`policy_rules` hooks are wired.
6. Config/CLI/RPC surfaces exist and are documented.
7. Every §13 golden passes; P6-RUNTIME §7 gates green.
8. `design/20-core-agent.md` (new §on trust/security) and
   `design/30-extensibility.md` (hook list) updated in the same change.

## 15. Handoff (P6-C)

> Implement `plans/P6-TRUST.md` only, on top of green P6-A/P6-B. Add
> `AgcTrustEnv`/`AgcTrustDecision`/`AgcSandboxSpec` to `include/agent.h`
> (append-only) or a new `include/trust.h`; add `os_sandbox_apply` to
> `include/plat.h` and the linux/mac/win/headless backends; add
> `agentc_trust_decide` + the rule engine in `src/core/trust.c`; add the approval
> dispatch in `src/core/approval.c` reusing `defer`/`is_cancelled`; wire
> `tool_approval` into the `tool_call` veto path without breaking existing veto
> semantics; add the `policy-check` CLI. Do not implement sub-agents. Acceptance
> and tests are §14/§13. Append to headers; never edit. Keep every gate green.

## 16. Audit addenda (authoritative in `P6-SURFACE.md`)

This file predates the surface audit; `P6-SURFACE.md` §3.5/§3.6/§3.7/§18 adds
the explicit C APIs: `agentc_trust_env_free/clone/default/narrow/from_json/
to_json/describe/sandbox_spec/path_ok/host_ok/load`, `agentc_policy_load/
add_json/free/eval`, `agentc_approval_request/submit/pending/cancel/
scope_remember/is_pending`, plus `AgcTrustEvent`/`AgcApprovalEvent`, the
`add_rule` approval-response field, config `trust.approval.persist` and
`trust.sandbox.backend`, and the approval error codes. The interactive/
delegated approval wait is a blocking wait and therefore must run `runtime_tick`
(R-D17). Fold these in; no decision (R-D3/R-D4/R-D5) changes.
