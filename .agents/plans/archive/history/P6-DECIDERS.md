# P6-DECIDERS — Decisions as hooks (jev, the classified router)

Status: **as-built (P6-A..E + completion passes 2a-2g).** The decision seam and decider backends.
As-built note: the outcome is folded into `.agents/design/20-core-agent.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

A **decision** is not a separate mechanism or stage. It is an ordinary **hook
handler** on `input`, `tool` or `step` that `agentc_hook_branch`es to a decider
(a local rule set, external program, MCP server, or a review **model such as
jev**) and returns the (possibly routed/edited/consumed) message. Per R-D23 the
old `decision`/`route` stages are removed: decisions live on the data stages.

---

## 1. What a decider is

- A **handler** on a stage, exactly like any other hook (two verbs: return the
  next message, or `defer`).
- A **backend** that answers the decision: `local` rules, `program`, `mcp`, or
  `model` (jev/classifier).
- The **classified router** is a decider on `input`; **jev** is a decider on
  `tool`/`call` (approval) and optionally `step`.
- Decisions never sit in the data path and never execute anything; they only
  return a verdict the core validates and applies via the primitives.

## 2. Where decisions live (stages, not points)

| decision | stage / kind | handler returns |
| --- | --- | --- |
| classify & route an input | `input` | a message whose meta is the route (target/model/tools/envelope/queue/budget); or unchanged |
| approve / deny / narrow a tool call | `tool` / `call` | `NULL` (deny), edited `m` (narrow/allow), or a branch to jev |
| allow a sub-agent spawn | `tool` / `call` (tool `agent`, op `spawn`) | `NULL` or an edited child spec (clamp) |
| choose model / thinking | `step` / `model_select` | edited step context, or a branch |
| compaction | `step` / `compact` | edited context, or `NULL` to skip |
| steer policy | `input` / `queue` | edited delivery, or `NULL` to hold |
| escalate | any stage | a handler chains to another via `hook_branch` |

`P6-TRUST.md` approval is the `tool`/`call` row; the router is the `input` row;
jev is the backend used by either. The earlier "decision point" names
(`route`, `approve`, `model_select`, `spawn`, `compact`, `steer_policy`,
`escalate`) survive only as an **intent tag** on the branch spec, not as core
machinery.

## 2b. Controller decisions (multi-effect)

The most powerful decider is a **controller**: one handler on `tool`/`call` (and
optionally `step`) that branches to jev for **every** tool call and then applies a
**multi-effect** decision record. No new mechanism is needed — each effect is
already a primitive call, and the handler is just code.

### 2b.1 The decision record (decider-defined JSON)

```json
{
  "verdict": "allow|deny|narrow|replace",
  "args":   { /* narrowed tool args when verdict=narrow */ },
  "replace":{ "tool":"agent", "op":"spawn", "task":"…" },  /* verdict=replace */
  "approval": { "required": true, "reason": "…" },
  "model":  { "provider": "…", "model": "…", "thinking": "high" },
  "done":   true,
  "queue":  { "text": "…", "turn": 0, "priority": 0 },
  "reason": "…"
}
```

### 2b.2 Each effect maps to an existing primitive

| effect | handler does | primitive |
| --- | --- | --- |
| allow | return the tool-call message unchanged | (the two verbs) |
| narrow | return the message with edited args | the two verbs |
| deny | return `NULL` (consume) → tool error | the two verbs |
| **spin a subagent** | return a **replacement** tool call `agent op:spawn`, or `actor_spawn` directly | `actor_spawn` / the `agent` tool |
| **ask user approval** | `hook_branch(human_approver,…)` and `defer`; then allow/deny from the answer | `P6-TRUST` §7.2 interactive |
| **next-step model** | `message_send` CONTROL `SET_MODEL` (`SET_THINKING`) | control message |
| **be done** | `message_send` CONTROL `SETTLE` (or edit the `step` context `continue=false`) | control message |
| stop this step | `message_send` CONTROL `CANCEL{scope:turn}` | control message |
| queue / steer | `message_send` INPUT `(turn, priority)` | input message |

So a composite decision is **one handler fanning out to a few primitive calls**.
The core interprets nothing but the returned message; `SET_MODEL`/`SETTLE` are
ordinary CONTROL kinds (`P6-SIGNALS.md` §4).

### 2b.3 Mechanics and ordering

- The handler `hook_branch`es to jev (async), then on resume applies the record:
  call-side verdict = the returned message; side effects = `message_send`/
  `actor_spawn`. It may `defer` again for approval.
- **Order:** validate the record, apply side effects, then return the call
  message (or `NULL`). Apply model/done effects only for the taken verdict.
- **done vs cancel:** `SETTLE` ends the turn (`agent_end`, `stop`);
  `CANCEL{scope:turn}` stops the step only. Both are messages.
- **Subagent elegance:** "do it here / in a child / not at all" is literally
  *which tool-call message you return* (`allow` / `replace` with `agent op:spawn`
  / `NULL`).
- **Next-step model:** model/thinking are actor state; jev emits `SET_MODEL` now
  and the next `step`/`model_select` observes/narrows it.

### 2b.4 Cost and determinism

- Per-call jev is a model call per tool call. Short-circuit `local` rules first,
  memoize by `(tool, args, envelope, config_hash)`, or run jev **once per batch**
  (register on `step`, inspect the batch, reuse for the calls).
- The decision is recorded once (session `decision` record) and replayed on
  resume; re-classification never changes a replayed verdict.

### 2b.5 This is Codex's guardian/auto-review

Codex's `guardian`, `guardian_v2`, `auto_review`, `approvals_reviewer` and
`misalignment_policy_violation` are this pattern. In P6 it needs no dedicated
subsystem: a controller is a decider handler on `tool`/`step`.

### 2b.6 Example

```text
[tool/call bash "rm -rf build"]
  → jev handler hook_branch → jev says {verdict:narrow, args:{command:"rm -rf ./build"},
                                         model:{model:"gpt-5-mini"}, approval:{required:true}}
  → handler hook_branch(painter, …) → human approves (session scope)
  → handler message_send SET_MODEL {gpt-5-mini}
  → handler returns the narrowed tool-call message   → tool executes
[next step] step/model_select sees the mini model; a done later returns SETTLE
```

## 3. The decision body schema

A decider handler (or a `hook_branch` target) speaks one JSON shape; it is the
body of the hook message, not a new protocol.

**Request (core → decider), as the branch task:**

```json
{
  "intent": "route|approve|spawn|model_select|compact|steer|escalate",
  "actor_id": 1, "parent_agent_id": 0, "depth": 0, "session_id": "…",
  "input": { /* the stage-specific body: tool+args, user text, step context */ },
  "context": {
    "effective_trust": {...}, "model": {...}, "tools": [...],
    "budget": { "tokens_remaining": 12345, "deadline_ms": 60000 },
    "lineage": [{ "agent_id": 1, "task": "…" }]
  },
  "constraints": { "may_narrow_trust": true, "may_widen_trust": false,
                   "reason_required": true, "max_hops": 2 }
}
```

**Response (decider → core), parsed by the handler and applied as a message:**

```json
{ "verdict": "allow|deny|ask|route|default",
  "reason": "…", "scope": "once|step|session|project",
  "route": { /* target/model/tools/trust(narrow)/queue/budget, §4 */ },
  "args": { /* narrowed tool args */ },
  "add_rule": { /* optional persistent policy rule */ },
  "delegate": { "decider": "jev", "intent": "approve" } }
```

Validation before applying: a `route`'s envelope is an **intersection** of the
effective envelope (else deny/ask); model/provider known; tools a subset; budget
within caps; `add_rule` only when persistence allows. Malformed/timeout/unknown →
**fail-closed** for `approve`/`spawn`, **default** for `route`.

## 4. The classified router

A decider on `input`. It classifies and returns a **route** in the message meta:

```json
{ "verdict": "route",
  "route": {
    "class": { "intent": "coding", "complexity": "deep", "risk": "mutating", "confidence": 0.86 },
    "target": { "kind": "self|spawn|existing|process", "agent_id": 12, "task": "…", "wait": true },
    "model":  { "provider": "anthropic", "model": "claude-sonnet-4-5", "thinking": "high" },
    "tools":  { "allow": ["read","grep","edit","bash"], "deny": ["write"] },
    "trust":  { "access": "read-write", "boundary": "workspace", "roots": ["src"] },
    "queue":  { "turn": 0, "priority": 0 },
    "budget": { "max_tokens": 200000, "deadline_ms": 600000 } } }
```

The core applies it via the primitives: `self` → adjust model/tools/envelope
(narrow) and continue; `spawn` → the `agent` tool (`tool`/`call`); `existing` →
`queue`/`message_send`; `process` → the `bash` tool. Classification axes:
intent/skill, complexity, risk, domain, fan-out. Classifier: `local` rules, a
small `model`, or jev.

Determinism: memoize a route by `(input_hash, effective_envelope, config_hash)`;
record the decision (class, confidence, decider, model) in the session so replay
never re-classifies differently.

## 5. jev

A **`model` decider backend**, not special-cased.

- On `tool`/`call`: the reviewer prompt gets the request (tool, args, envelope,
  trigger, lineage); returns allow/deny/ask + reason/scope/narrowed-args/
  add-rule. This *is* the `P6-TRUST.md` §7 reviewer model.
- On `input`: classify/route (or delegate to a smaller `classify` model).
- On escalate: a `local` handler may `delegate` to jev, jev to `human`.
- Fixed system prompt defines the schema and the rule that it may only **narrow**
  the envelope; off-schema = fail-closed.
- Cost: runs on the shared transport with capped tokens/reasoning; usage is
  attributed to the session (`decision` record) and counts against the budget.

Config:

```jsonc
"deciders": {
  "jev":      { "kind": "model", "provider": "openai", "model": "gpt-5-codex", "max_tokens": 512 },
  "classify": { "kind": "model", "provider": "openai", "model": "gpt-5-mini", "max_tokens": 64 },
  "corp":     { "kind": "program", "command": "/usr/local/bin/corp-approve" }
},
"hooks": {
  "input": [ { "order": 100, "decider": "classify" } ],
  "tool":  [ { "order": 100, "decider": "jev", "on_error": "deny" } ]
}
```

Precedence: a `local` rule with a definitive verdict short-circuits a model
decider. Chaining is `delegate` with `max_hops` (2) + cycle detection.

## 6. Async and audit

- A program/model/MCP decider is internally a `defer`: the actor enters
  `WAITING`; `runtime_step` resumes. No primitive blocks (R-D17/R-D22).
- Audit: `AGENTC_EV_DECISION_REQUEST` / `_RESULT`, `AGENTC_EV_ROUTE_DECIDED`, and
  the session `decision` record
  `{intent, stage, actor_id, decider, model?, input_hash, verdict, scope, reason,
  cost_micro}`. The route/verdict is part of the transcript metadata so replay
  is stable.

## 7. Failure modes

| failure | behavior |
| --- | --- |
| decider timeout | `approve`/`spawn`: fallback (`ask`/`deny`); `route`: `default` |
| malformed/unknown response | fail-closed (deny) for gates; `default` route |
| delegation cycle / hops exceeded | fail-closed (deny) |
| widening trust in a route | denied, or escalated to `approve` |
| unknown model/provider or tool beyond table | rejected; `default` route |
| budget exceeded by decider | abort; fallback |
| resumed session | replay the recorded decision |

## 8. Surfaces (fold into `P6-SURFACE.md`)

No new stage, no new primitive. Surfaces are the hook surfaces plus:
`AGENTC_MSG_DECIDE`/`_DECIDED` messages (the branch round-trip), config
`deciders.<id>` and a per-stage `decider` handler kind, events
`AGENTC_EV_DECISION_REQUEST`/`_RESULT`/`ROUTE_DECIDED`, the `decision` session
record, and errors `decision_denied`, `decision_timeout`, `decision_cycle`,
`decision_hops`, `route_invalid`, `route_widen_denied`. C helpers:
`agentc_decide(spec, m, &defer)` (sugar over `hook_branch` with the schema) and
`agentc_decision_submit(response_json)` for interactive/out-of-band answers.

## 9. Worked examples

1. **Route deep coding to a child with a stronger model.** `input` router
   classifies `coding/deep/mutating`; returns a route `target=spawn`; the `agent`
   tool spawns the child under the clamped envelope.
2. **jev denies a dangerous command.** `tool`/`call` for `bash rm -rf /repo`:
   local rules pass, jev returns deny → handler `NULL` → tool result
   `error: denied: …`.
3. **Router escalates a risky route.** `input` router `delegate`s to jev at the
   tool stage; jev asks; human approves `session` scope; the route is applied and
   memoized.
4. **Per-step model switch.** `step`/`model_select` edits the model; no branch.

## 10. Minimality

This adds **no primitive, no stage, and no disposition**. A decider is a hook
handler (`P6-HOOKS.md`) that branches to a backend. `route`, `approve`, `spawn`,
`model_select`, `compact`, `steer_policy`, `escalate` are intents, not seams.
Disable all deciders and behavior is the R-D4 default (trust the model inside the
boundary).

## 11. Handoff

> Land after P6-HOOKS. Implement the decision schema as a `hook_branch` helper
> (`agentc_decide`) and the `decider` handler kind + `deciders.<id>` config;
> attach jev to `tool`/`call` and the router to `input`. No stage, no primitive,
> no model tool. Events/session/errors per §8. Tests: `tests/deciders_test.c`
> (schema, validation, fallback, delegation/hops/cycle, route application,
> memoization, replay); reuse `tests/hooks_test.c` for the pipeline.
