# P6-SIGNALS — The message bus and the unified queue

Status: **as-built (P6-A..E + completion passes 2a-2g).** The message bus and unified queue.
As-built note: the outcome is folded into `.agents/design/20-core-agent.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

As-built deviations (see `.agents/plans/p6-runtime/TODO.md`
and the checkpoint `5e99ca9` record): (1) queue persistence (SIG §5.6 / C-D9) — the
`AGENTC_EV_ACTOR_QUEUED` observer and event exist, but the session-side
`custom type:"queue"` append/restore is not yet wired; (2) preempt
`AGENTC_Q_F_PREEMPT_TOOLS` does not yet cancel a running tool-process actor
graceful→force (stream preempt and the non-steerable hold are implemented);
(3) the `agent`/`process` `actor_op` RPC is a thin tool forwarder, not the full
op matrix. Area spec for **P6-B**. Read `P6-RUNTIME.md`
(decision log R-D7 graceful/force, R-D8 queue, R-D9 controller authority, R-D12
determinism, R-D13 bounds) and `P6-ACTORS.md` (actors, cancellation, liveness)
first.

> **Primitive framing (R-D22):** `AgcMessage` and the queue are the **CONTROL**
> and **INPUT** classes of the one **Message** primitive
> (`P6-PRIMITIVES.md` §3). This file uses the historical `signal`/`queue` names
> for the semantics; the implementation is a single message envelope with a
> `class` and a `(turn, priority)` ordering key. `agentc_message_send` and
> `agentc_queue_add` are conveniences over `agentc_message_send`.

Scope: the `AgcMessage` type and bus; **one turn-targeted, priority-ordered
queue** that replaces the three separate delivery modes; `notify` between parent
and child; cancel scopes; mailbox bounds; the control-layer events and hooks.
Processes consume only `input` (raw stdin bytes), `cancel` and `notify`; agents
consume the queue.

Non-goals: trust and approvals (`P6-TRUST.md`), agent spawning
(`P6-SUBAGENTS.md`).

---

## 0. Revision note (why one queue, not three modes)

An earlier revision of this file had three delivery modes:
`queue`, `steer_boundary`, `steer_now`. The chair replaced that with a **single
queue** whose items carry a **turn number** and a **signed priority**, in the
spirit of Linux nice: the turn selects *which turn* the input belongs to, and
the priority sign selects *whether it interrupts now or lands at the next safe
boundary*, with the magnitude giving deterministic ordering. This removes a
mode enum, a `preempt` bitmask, and the ambiguity of "queue vs steer"; the queue
is the one mechanism.

Local decisions recorded for this area:

- **S-D1** — One queue; no `delivery` enum, no `preempt` bitmask on the signal.
  Position in the queue is `(run, priority, enqueue order)`.
- **S-D2** — Priority is a signed `nice`-like scalar in `[-20, 19]` with
  `0` default. `< 0` = preempt phase (immediate interrupt); `>= 0` = boundary
  phase (next safe point); ascending value = earlier within a phase.
- **S-D3** — `turn == 0` means auto (current turn if one is active, else the
  next). `turn > 0` is an absolute turn number. An absolute `run` that has already
  passed lands at the earliest remaining safe point (or the next turn if the
  run settled).
- **S-D4** — Same-step items that preempt default to also preempting the running
  tool batch (`AGENTC_Q_F_PREEMPT_TOOLS`, on by default) — the "not waiting for
  the tool call" case. Cleared only by an explicit flag.
- **S-D5** — A preempt flushes the **whole due set** for that step in priority
  order (preempt block then boundary block), so a second preemption is never
  needed and the result is deterministic.
- **S-D6** — Coalescing is per phase by default (one user message per phase);
  `AGENTC_Q_F_NO_COALESCE` delivers one message per item.

## 1. Current state (facts to build on)

- The agent loop (`src/core/agent.c`) is a synchronous nested loop inside
  `agentc_agent_submit`: a retry loop around each request + an outer settle loop.
  Safe boundaries already exist: **before request build**, **after
  `message_end`**, and **after each tool batch**. `AGENT_CONTINUATION_MAX` (32)
  bounds `step_end.continue` + `agent_before_settle.continue`.
- `agentc_agent_abort()` sets one cancel flag checked between events and inside
  tool polls. There is no queue, no steer, and cancel has no scope/stage.
- The typed event hub is `src/core/events.{c,h}`: slot 0 primary + slots 1..7
  subscribers + a global quiet gate that also silences the extension bridge.
  The enum ends at `AGENTC_EV_MSG_RESET` (`include/agent.h`). `MSG_RESET` is the
  existing "discard the in-progress message and replace it" signal used by retry;
  the queue's preempt phase reuses exactly this mechanism.
- Extension hook points are string constants (`include/agentc_ext.h` ~599–627),
  including the `input` override and the `tool_call` chain-veto. The host has
  `defer` + `is_cancelled` for asynchronous work.
- `--mode rpc` commands (`src/app/mode_rpc.c`): `prompt`, `prompt_template`,
  `abort`, `get_state`, `set_model`, `set_thinking_level`,
  `get_available_models`, `compact`, `bash`, `new_session`, `get_messages`.

## 2. Message type (CONTROL + INPUT + NOTIFY + HOOK)

```c
/* message.h — the runtime control/notify bus (internal). Owner: the core.
 * One bounded mailbox per actor; drain is deterministic (per-sender seq +
 * global arrival order). See P6-SIGNALS. */
typedef enum {
    AGENTC_MSG_STARTED = 0,
    AGENTC_MSG_PROGRESS,        /* heartbeat, no payload bytes */
    AGENTC_MSG_STALLED,         /* advisory */
    AGENTC_MSG_INPUT,           /* agent: enqueue (run+priority); process: stdin bytes */
    AGENTC_MSG_NOTIFY,          /* actor -> controller: status/question */
    AGENTC_MSG_FINISH,          /* terminal state: result / exit code */
    AGENTC_MSG_FAIL,            /* terminal state: error */
    AGENTC_MSG_CANCEL,          /* scope: turn | actor; stage: graceful | force */
    AGENTC_MSG_CANCELLED,       /* confirmation, emitted once */
    AGENTC_MSG_FREEZE,          /* scope: actor | subtree | runtime; flags */
    AGENTC_MSG_RESUME,          /* scope as FREEZE */
    AGENTC_MSG_FROZEN,          /* confirmation, emitted once */
    AGENTC_MSG_RESUMED,         /* confirmation, emitted once */
    AGENTC_MSG_ATTACH,          /* UI subscribes to a stream */
    AGENTC_MSG_DETACH,
    AGENTC_MSG_DECIDE,          /* decision request to a decider (P6-DECIDERS) */
    AGENTC_MSG_DECIDED,         /* decision response */
    AGENTC_MSG_SET_MODEL,       /* CONTROL: set this actor's next model */
    AGENTC_MSG_SET_THINKING,    /* CONTROL: set thinking level */
    AGENTC_MSG_SETTLE,          /* CONTROL: finish the turn (stop), keep the actor */
} AgcMessageKind;

typedef enum { AGENTC_MSG_SCOPE_NONE = 0, AGENTC_MSG_SCOPE_TURN, AGENTC_MSG_SCOPE_ACTOR,
               AGENTC_MSG_SCOPE_SUBTREE, AGENTC_MSG_SCOPE_RUNTIME } AgcMsgScope;
typedef enum { AGENTC_MSG_STAGE_NONE = 0, AGENTC_MSG_STAGE_GRACEFUL, AGENTC_MSG_STAGE_FORCE } AgcMsgStage;
```

The wire type is one struct, **`AgcMessage`** (`P6-PRIMITIVES.md` §3) — there is
no separate signal type. The CONTROL-relevant fields are `kind`, `scope` (CANCEL/
FREEZE), `stage` (CANCEL) and `flags`; the INPUT-relevant fields are `turn`,
`priority`, `flags` and the body (`text`). Every message is bounded and addressed
(`from`/`to`/`id`); the mailbox and ordering rules in
§3 apply.

Queue flags:

```c
#define AGENTC_Q_F_PREEMPT_TOOLS   0x1u  /* preempt phase also cancels the running tool batch (default on) */
#define AGENTC_Q_F_NO_COALESCE     0x2u  /* deliver one message per item rather than per phase */
#define AGENTC_Q_F_CANCEL_NONSTEERABLE 0x4u /* if the active step is non-steerable, cancel it and resubmit */
#define AGENTC_Q_F_DEDUPE          0x8u  /* P7: drop an enqueue whose client_id is already queued (returns the existing id) */
```

Priority constants:

```c
#define AGENTC_PRIO_MIN      (-20)
#define AGENTC_PRIO_MAX      (19)
#define AGENTC_PRIO_PREEMPT  (-1)   /* default for an immediate item */
#define AGENTC_PRIO_BOUNDARY (0)    /* default */
```

## 3. Bus API

```c
void        agentc_message_init(void);            /* reset mailboxes */
int         agentc_message_send(const AgcMessage *s);   /* enqueue; 0 | -ENOSPC | -ESRCH */
/* Drain up to `max` signals destined to `id` (0 = all actors) in order.
 * Calls the actor's handler; returns the number delivered. */
size_t      agentc_message_drain(AgcActorId id, size_t max);
size_t      agentc_message_pending(AgcActorId id);     /* mailbox depth */
void        agentc_message_clear(AgcActorId id);       /* on release */
```

- `agentc_message_send` is the only entry. It validates `to`, clamps `priority`
  to `[MIN,MAX]`, bounds the mailbox, stamps `seq`/`ts`, and enqueues. A signal
  to a released actor returns `-ESRCH` and is dropped with a log (never
  resurrect an actor).
- `agentc_message_drain` is called from the pump (`agentc_pump`) and at the
  agent loop's safe points. It is the **only** place handlers run, which keeps
  execution single-threaded and deterministic (R-D12).
- Broadcast (`to == 0`) is only valid for `CANCEL` (a controller cancelling its
  whole subtree) and is depth-first, source order.

## 4. Signal kinds — normative semantics

| kind | from → to | effect |
| --- | --- | --- |
| `STARTED` | actor → controller | actor is RUNNING; carries `{kind,label,model?,cmd?}` |
| `PROGRESS` | actor → controller | refresh liveness; carries counters, not bytes |
| `STALLED` | actor → controller | advisory; label change only |
| `INPUT` | controller → agent | **enqueue** a queue item (§5) |
| `INPUT` | controller → process | write stdin bytes / `close_stdin` (P6-ACTORS §4) |
| `NOTIFY` | actor → controller | status/question; controller answers with `INPUT` |
| `FINISH` | actor → controller | process; carries result/exit/usage |
| `FAIL` | actor → controller | process; carries error |
| `CANCEL` | controller → actor | scope/stage cancellation (§7) |
| `CANCELLED` | actor → controller | exactly once |
| `FREEZE` | controller → actor | suspend at the current state (§7b) |
| `RESUME` | controller → actor | continue a frozen actor |
| `DECIDE` | core → decider | a typed decision request at a seam (`P6-DECIDERS`) |
| `DECIDED` | decider → core | the decision response |
| `SET_MODEL` | controller → actor | set the actor's next-step model (`P6-DECIDERS` §3) |
| `SET_THINKING` | controller → actor | set the thinking level |
| `SETTLE` | controller → actor | finish the turn (`agent_end`, reason `stop`); actor stays alive |
| `FROZEN`/`RESUMED` | actor → controller | confirmations |
| `ATTACH`/`DETACH` | UI → actor | subscribe/unsubscribe to streamed progress |

A signal handler may synthesize at most one downstream signal (e.g. `CANCEL` →
`CANCELLED`), enqueued for a later drain — no recursion into the handler stack.

## 5. The unified queue

One queue per **agent** actor. Processes have no run and therefore no queue
(their `INPUT` is raw stdin). The queue is the single mechanism behind every
"send/steer/queue" operation.

### 5.1 Item

```c
typedef struct {
    uint64_t    id;          /* stable; unique per agent */
    int64_t     turn;       /* 0 = auto; > 0 absolute turn number */
    int32_t     priority;    /* -20..19; < 0 preempts, >= 0 boundary */
    uint32_t    flags;       /* AGENTC_Q_F_* */
    char       *client_id;   /* optional caller id */
    char       *text;        /* owned */
    i64         enqueued_ms;
} AgcQueueItem;
```

Core API (all tools and RPC go through it):

```c
uint64_t agentc_queue_add(AgcActorId id, const char *text, int64_t run,
                          int32_t priority, uint32_t flags, const char *client_id);
int      agentc_queue_list(AgcActorId id, AgcQueueItem *out, size_t max);  /* count-first */
int      agentc_queue_update(AgcActorId id, uint64_t item, const char *text,
                             int64_t run, int32_t priority);
int      agentc_queue_delete(AgcActorId id, uint64_t item);
int      agentc_queue_reorder(AgcActorId id, const uint64_t *ids, size_t n);
int      agentc_queue_start(AgcActorId id, uint64_t item);   /* make due now */
```

### 5.2 Run numbering

- `turn_seq` is the agent's **turn counter**: `0` when idle, incremented to `S`
  when turn `S` begins. A **turn** is one `agentc_agent_submit` (its steps + tool
  batches). Exposed through `get_state` and `AgcAgentInfo`.
- `turn == 0` (auto): target = the active turn `turn_seq` if one is active, else
  the next turn `turn_seq + 1`.
- `turn == N > 0`: absolute. Due when `turn_seq >= N`.
- An absolute `turn` that has already passed (the turn settled before the item
  arrived) lands at the earliest remaining safe point, or starts the next turn
  if nothing is active. It never resurrects a settled turn.

### 5.3 Priority and phases

Priority is a signed `nice`-like scalar. The **sign selects the phase**, the
**value selects the order**:

| priority | phase | landing |
| --- | --- | --- |
| `< 0` | `PREEMPT` | interrupt now: abandon the in-flight stream; unless `AGENTC_Q_F_PREEMPT_TOOLS` is cleared, gracefully cancel the running tool batch (→ force after `grace_ms`) |
| `0` | `BOUNDARY` | at the next safe point (post-tool batch, pre-request) |
| `> 0` | `BOUNDARY` | same as `0`, ordered after lower values |

Ordering within a phase: `(priority asc, enqueued_ms asc, id asc)`.

`turn` decides *which turn*; `priority` decides *now vs boundary* and *order*.
The two are orthogonal: an item for turn `T+1` with priority `-1` is simply the
first content of turn `T+1` (nothing to preempt at a fresh turn start); the same
item for the active turn `T` preempts the current step.

### 5.4 Due-set and flush rule (S-D5)

At each evaluation point, the **due set** is every pending item whose target
run is `<=` the active turn (or `<=` the turn being started). The due set is
flushed in one of two ways:

- **Preempt flush** — if any due item has `priority < 0` and the active step is
  steerable: preempt the stream and (unless cleared) the tool batch, then deliver
  the whole due set: the `PREEMPT` block first, then the `BOUNDARY` block, both
  in priority order. This avoids a second preemption and is deterministic.
- **Boundary flush** — otherwise, at the post-tool/pre-request safe point,
  deliver all due items with `priority >= 0` in priority order.

Coalescing (S-D6): each phase block becomes **one** user message whose items
appear in order; `AGENTC_Q_F_NO_COALESCE` emits one message per item. Empty
blocks emit nothing.

### 5.5 Drain and auto-start

- **Idle with auto-start** (`trust.queue.auto_start`, default true): when the
  agent settles, pending items with target `<= turn_seq + 1` are coalesced into
  the initial user message of the next submission, which then starts
  automatically. Items with a later `step` wait.
- **Idle with auto-start off:** items wait until `queue_start` or an explicit
  `prompt`.
- **Arrival when idle:** `step == 0` becomes the next submission immediately if
  auto-start is on, else waits.
- **`queue_start{item}`:** sets the item's target to "now" (equivalent to
  `step == 0`, `priority = min(priority, 0)`), triggering a preempt flush if a
  step is active.
- **Drain bound:** the queue is bounded (`AGENTC_QUEUE_MAX`, proposed 32).
  Overflow rejects the add with a typed error; it never silently drops (R-D13).

### 5.6 Persistence

Each add/update/delete/reorder appends a `custom` session record
(`type:"queue"`, the whole item list snapshot including `step`/`priority`) via
`agentc_session_append_raw`. On resume the queue is restored, then re-validated
against the envelope; items that no longer pass are dropped with a `NOTIFY`
(R-D11).

### 5.7 Authority

Only the actor's controller (the root user, or the agent's parent) may enqueue
into it. A child may not enqueue into its parent (R-D9); a child `notify`s
upward.

## 6. How each old mode maps (compatibility of intent)

| old | new equivalent |
| --- | --- |
| `queue` (next step) | `{turn: 0, priority: 0}` when idle, or `{turn: turn_seq+1, priority: 0}` |
| `steer_boundary` (after current tool) | `{turn: 0, priority: 0}` while a step is active |
| `steer_now` stream-only | `{turn: 0, priority: -1, flags: 0}` (clear `PREEMPT_TOOLS`) |
| `steer_now` tools+both | `{turn: 0, priority: -1}` (default `PREEMPT_TOOLS`) |

The `queue`/`agent` tools and the `actor_op`/`queue`/`actor_query` RPC commands
are op-based adapters over `agentc_queue_add` (§11). No separate delivery path
remains.

## 7. Cancellation scopes

`CANCEL` carries `scope` and `stage` (P6-ACTORS §6 defines escalation):

- `scope = step`: stop the active step; keep the agent and its background
  processes. For an agent with no active step this is a no-op. Maps to Codex
  `turn/interrupt`.
- `scope = actor`: cancel the actor (and, for an agent, its descendants
  depth-first). Maps to `command/exec/terminate` / killing a process.
- `stage = graceful`: cooperative (close stdin + SIGTERM / set cancel flag).
- `stage = force`: hard (SIGKILL group / abort stream now). Also applied
  automatically when `grace_deadline_ns` passes.
- `CANCELLED{stage,reason}` is emitted once. A second `CANCEL` upgrades stage or
  is a no-op.

Cancelling an actor resolves any pending approval as deny (P6-TRUST §7.3) and
leaves the queue intact unless the actor is released.

## 7b. Freeze and resume

`FREEZE`/`RESUME` reuse `scope` with two additional values beyond cancel's
`step|actor`:

- `AGENTC_MSG_SCOPE_ACTOR` — one actor.
- `AGENTC_MSG_SCOPE_SUBTREE` — **[NEW]** depth-first over an agent's
  descendants.
- `AGENTC_MSG_SCOPE_TURNTIME` — **[NEW]** every actor (global pause).

Flags on `FREEZE` (new `flags` bits, distinct from the queue `AGENTC_Q_F_*`):

```c
#define AGENTC_FREEZE_DRAIN       0x1u  /* agent: finish the in-flight request first (default) */
#define AGENTC_FREEZE_ABORT       0x2u  /* agent: abandon the in-flight stream now */
#define AGENTC_FREEZE_CHECKPOINT  0x4u  /* additionally snapshot the process tree (optional backend) */
```

Semantics (detail in `P6-ACTORS.md` §6b):

- Freezing a process is `SIGSTOP` on the group; resuming is `SIGCONT`. The
  process stays alive and stopped.
- Freezing an agent is cooperative at a safe point; pending approvals and the
  queue are preserved and delivered on resume.
- Timers (`deadline_ns`, `grace_deadline_ns`, stall) are suspended while frozen
  and shifted forward on resume; a long freeze cannot trigger a timeout.
- `FROZEN`/`RESUMED` are emitted once; a repeat freeze/resume is idempotent.
- Cancel of a frozen actor resumes (`SIGCONT`) before `SIGTERM`, or force-kills.
- Freeze never drops queue items; `release` clears them.

`runtime_freeze`/`runtime_resume` (scope `RUNTIME`) are the global form used by
`agentc_runtime_freeze/resume` and the RPC commands of the same name.

## 8. Mailbox, ordering, backpressure

- One bounded mailbox per actor (`AGENTC_MSG_MAILBOX_MAX`, proposed 64).
- Overflow drop policy, highest priority kept: `FINISH`/`FAIL`/`CANCELLED` >
  `CANCEL` > `INPUT`/`NOTIFY` > `PROGRESS` > `STALLED`. `PROGRESS`/`STALLED`
  coalesce to one when full; `FINISH`/`FAIL`/`CANCELLED` are never dropped.
  Overflow is counted and logged once per actor (R-D13).
- Queue overflow is separate: `agentc_queue_add` returns `-ENOSPC`; the mailbox
  is where the *signal* lands, the queue is where the *item* lands.
- Per-sender `seq` is monotonic; global delivery order is arrival order into the
  actor's mailbox. This is the determinism contract for the golden tests.
- `agentc_message_drain` processes at most `AGENTC_MSG_DRAIN_MAX` (proposed 64)
  per call so a flood cannot starve the loop; the remainder stays queued.

## 9. Notify: parent ↔ child

- **Upward** (`child → parent`): `NOTIFY{kind,...}`; reserved kinds `started`,
  `progress`, `stalled`, `needs_input`, `blocked_on_approval`, `finished`,
  `failed`. Advisory/state; the authoritative process is `FINISH`/`FAIL`.
- **Downward** (`parent → child`): `INPUT` (enqueue with `step`/`priority`) and
  `CANCEL`.
- A `needs_input` notify is answered with an `INPUT` (enqueue); the child does
  not block waiting unless it entered a `WAITING` state, which is visible in
  `actor_list`.
- Every notify/finish/fail carries lineage (`from`, plus `parent_id`/`depth` in
  the JSON payload) so a UI can render the tree without subscribing to the child
  transcript.

## 10. Events and hooks

### 10.1 Events (append after `AGENTC_EV_MSG_RESET`)

```text
AGENTC_EV_ACTOR_STARTED
AGENTC_EV_ACTOR_PROGRESS
AGENTC_EV_ACTOR_STALLED
AGENTC_EV_ACTOR_INPUT        /* enqueued; carries step + priority */
AGENTC_EV_ACTOR_STEERED      /* applied; carries at: preempt|boundary */
AGENTC_EV_ACTOR_QUEUED       /* add/update/delete/reorder/start */
AGENTC_EV_ACTOR_NOTIFY
AGENTC_EV_ACTOR_FINISH
AGENTC_EV_ACTOR_FAIL
AGENTC_EV_ACTOR_CANCEL_REQUESTED
AGENTC_EV_ACTOR_CANCELLED
AGENTC_EV_ACTOR_FROZEN        /* freeze confirmed */
AGENTC_EV_ACTOR_RESUMED
AGENTC_EV_ACTOR_CHECKPOINTED  /* optional checkpoint backend; carries path */
AGENTC_EV_ACTOR_ATTACHED
AGENTC_EV_ACTOR_DETACHED
```

Payload struct `AgcActorEvent` (append-defined):

```c
typedef struct {
    uint32_t id, parent_id;
    uint16_t depth;
    int      kind;        /* AgcActorKind */
    int      state;       /* AgcActorState */
    int64_t  turn;       /* INPUT/STEERED */
    int32_t  priority;    /* INPUT */
    const char *at;       /* STEERED: "preempt" | "boundary" */
    const char *text;     /* INPUT/NOTIFY text, borrowed */
    const char *json;     /* kind-specific, borrowed */
    int      exit_code;   /* FINISH */
    i64      duration_ms;
} AgcActorEvent;
```

These dispatch through the same hub (slot 0 + subscribers + quiet gate) and the
`AgcEventFn(ud, ev, data)` shape. Content events are unchanged (R-D16).

### 10.2 Hook points (append to `include/agentc_ext.h`)

```text
AGENTC_HEV_ACTOR_STARTED          "actor_started"
AGENTC_HEV_ACTOR_PROGRESS         "actor_progress"
AGENTC_HEV_ACTOR_STALLED          "actor_stalled"
AGENTC_HEV_ACTOR_INPUT            "actor_input"
AGENTC_HEV_ACTOR_STEERED          "actor_steered"
AGENTC_HEV_ACTOR_NOTIFY           "actor_notify"
AGENTC_HEV_ACTOR_FINISH           "actor_finish"
AGENTC_HEV_ACTOR_FAIL             "actor_fail"
AGENTC_HEV_ACTOR_CANCEL_REQUESTED "actor_cancel_requested"
AGENTC_HEV_ACTOR_CANCELLED        "actor_cancelled"
AGENTC_HEV_ACTOR_FROZEN           "actor_frozen"
AGENTC_HEV_ACTOR_RESUMED          "actor_resumed"
AGENTC_HEV_ACTOR_CHECKPOINTED     "actor_checkpointed"
```

All are observe-only except `actor_input`, which may transform/block the text
like the existing `input` override. Payloads carry the same fields as
`AgcActorEvent` plus `lineage`. `input` keeps its top-level meaning;
`actor_input` covers all enqueues and process stdin.

## 11. Tools, RPC, TUI (finalized: primitive-first, op-based)

Per R-D26 the model-facing surface is **three tools** — one per actor kind
(`process`, `agent`) plus `queue` — that are thin adapters over the primitives.
There are no per-feature tools; `bash` is a legacy alias for
`process {op: spawn}`.

### 11.1 Tools (3; flat, hidden until enabled; registered by `builtin-tools`)

| tool | args | primitive |
| --- | --- | --- |
| `process` | `{op: spawn\|write\|read\|resize\|kill\|freeze\|resume\|list\|attach\|detach, actor_id?, command?, shell?, tty?, background?, yield_ms?, data?, close_stdin?, cols?, rows?, stage?, scope?}` | `actor_spawn` (proc) + `message_send(INPUT/RESIZE/CANCEL/FREEZE/RESUME)` + `actor_read` + `actor_list` |
| `agent` | `{op: spawn\|wait\|send\|interrupt\|freeze\|resume\|result\|list, agent_id?, task?, model?, trust?, tools?, wait?, timeout_ms?, text?, turn?, priority?, flags?, scope?, stage?}` | `actor_spawn` (agent) + `actor_await` + `queue_add` + `message_send(CANCEL/FREEZE/RESUME)` + `actor_read` + `actor_list` |
| `queue` | `{op: add\|list\|update\|delete\|reorder\|start\|pause\|resume\|clear, actor_id?, text?, id?, turn?, priority?, ids?, paused?}` | queue primitive |

- `process spawn` returns `{actor_id}` (background) or the foreground result;
  `process read` drains output and returns `{output, state, exit_code?, eof}`.
- `agent wait` is `actor_await` + `actor_read`; `agent spawn {wait:true}` (the
  default) awaits and returns the child result.
- `get_trust` is **not** a tool — the effective envelope is rendered into the
  system prompt (declaration, `P6-TRUST` §4 layer 1) and available via the
  `actor_query` RPC/host call.

Named aliases (`spawn_agent`, `wait_agent`, `bash_input`, …) exist only as
optional sugar for front ends if needed; they are not first-class and add no
primitive.

### 11.2 RPC (`--mode rpc`, additive)

Canonical (op-based, mirrors the tools):

```text
actor_op    {actor_id?, op, args}   # op = spawn_process|spawn_agent|write|read|resize|
                                    #      cancel|freeze|resume|attach|detach|enqueue|label|release
actor_query {actor_id?, what}       # what = state|info|queue|trust|result|list|runtime
queue       {op, ...}
runtime     {op: tick|shutdown|freeze|resume}
approve     {id, decision, scope, args?}
deny        {id, reason}
set_trust   {actor_id, patch}
set_policy  {id|file}
```

Extend `get_state` with `turn_no`, `steerable`, `pending_approvals`,
`background_actors`. Plus the `AGENTC_EV_*` events on the existing stream.
Named aliases (`steer`, `interrupt`, `queue_add`, `actor_attach`, …) are
optional sugar over `actor_op`/`queue`; the canonical form is the op-based one.

### 11.3 TUI

- Active step: submit enqueues with `{turn:0, priority:trust.queue.submit}`;
  Alt+submit enqueues to the next step; an "urgent" binding enqueues
  `{priority:trust.queue.urgent}` (default `-1`, preempt). Ctrl-C =
  `actor_op cancel scope:turn`.
- A queue pane lists pending items with their `step`/`priority`, editable and
  reorderable (drag/keys adjust priority), mirroring Codex's queue.
- `/ps` lists actors; `k` = graceful cancel, `K` = force, `f` = freeze, `u` =
  resume, `enter` = attach a process, `esc` = detach.
- Non-steerable turns: a same-turn item is held for the next turn and
  `active_turn_not_steerable{kind}` is shown; `AGENTC_Q_F_CANCEL_NONSTEERABLE`
  cancels and resubmits.

## 12. Failure modes

| Failure | Behavior |
| --- | --- |
| signal to released actor | `-ESRCH`, dropped+logged, never resurrected |
| mailbox full | priority drop (§8); lifecycle never dropped; counted+logged once |
| queue full | add rejected `-ENOSPC`; existing items intact |
| item targets a non-steerable active step | held for the next step + `active_turn_not_steerable{kind}`; or cancel+resubmit with the flag |
| preempt arrives mid-stream | `MSG_RESET`; partial step dropped/aborted per `trust.queue.drop_partial`; re-request with the flush |
| preempt arrives mid-tool | graceful cancel of tool actors → force after `grace_ms`; then flush |
| priority out of range | clamped to `[MIN,MAX]` with a log |
| absolute step already passed | earliest remaining safe point, or next step if settled |
| queue item invalid on resume | dropped with a `NOTIFY` (never submitted blind) |
| child enqueues into parent | denied (authority) |
| drain flood | capped per call; remainder queued (no starvation) |

## 13. Golden test plan (new `tests/signals_test.c` + `.expected`)

Deterministic harness: fake clock, scripted actor handlers, recorded signal log.

- queue CRUD: add/list/update/delete/reorder/start; FIFO by priority then time.
- turn targeting: `{turn:0}` vs `{turn:turn_seq+1}` vs `{turn:turn_seq+2}`;
  auto-start on settle; a settled absolute step lands at the next step.
- preempt: `{priority:-1}` mid-stream → `MSG_RESET`, whole due set flushed,
  re-request built; assert exact event order.
- preempt tools: running process actor cancelled graceful→force (mock ignores
  TERM), then flush.
- boundary: `{priority:0}` lands at the post-tool/pre-request point; `+` items
  ordered after `0`.
- ordering: mixed `[-2,-1,0,1,5]` → preempt block `[-2,-1]` then boundary block
  `[0,1,5]`, one message each.
- no-coalesce flag: one message per item.
- cancel scope: `step` keeps the actor and background processes; `actor`
  releases.
- non-steerable: compaction step holds the item and emits
  `active_turn_not_steerable{compact}`; with `CANCEL_NONSTEERABLE`, cancel +
  resubmit.
- authority: child→parent enqueue denied; child→parent notify allowed.
- queue overflow: `-ENOSPC`, existing items intact.
- determinism: run twice, byte-identical log.

## 14. Acceptance for P6-B

1. `AgcMessage` + bus + bounded mailboxes exist and are drained from the pump and
   at the three loop safe points.
2. The **single queue** implements `(turn, priority)` semantics in §5; there is
   no `delivery` enum and no `preempt` bitmask on the signal (S-D1).
3. The three op-based tools (`process`/`agent`/`queue`) and the
   `actor_op`/`actor_query`/`queue`/`approve`/`deny` RPC commands exist and are
   documented as adapters over the primitives (R-D19).
4. Non-steerable gating returns the typed error with the reserved `kind`.
5. `AGENTC_EV_ACTOR_*` events and `actor_*` hook points are wired; content events
   unchanged.
6. Cancellation scope/stage works for agent and process actors.
7. Every §13 golden passes; `P6-RUNTIME` §7 gates green.
8. `design/20-core-agent.md` §2/§6 and `design/30-extensibility.md` hook list
   updated in the same change.

## 15. Handoff (P6-B)

> Implement `plans/P6-SIGNALS.md` only, on top of a green P6-A. Create
> `include/message.h`, `src/core/signal.c`, `src/core/queue.c`. Add the event
> constants and `AgcActorEvent` to `include/agent.h` after `AGENTC_EV_MSG_RESET`;
> add the `actor_*` hook names to `include/agentc_ext.h`; wire the drain into
> `agentc_pump` and the loop safe points in `src/core/agent.c`; add the RPC
> commands to `src/app/mode_rpc.c` and the tools to the builtin-tools registry.
> The queue is one mechanism: `(turn, priority)` per §5; register the four
> op-based tools (`bash`/`process`/`agent`/`queue`, §11) and the op-based RPC
> commands, not per-feature names. Reuse `AGENTC_EV_MSG_RESET` for the preempt
> phase. Do not implement trust or sub-agents. Acceptance and tests are
> §14/§13. Append to headers; never edit. Keep every gate green.

## 16. Audit addenda (authoritative in `P6-SURFACE.md`)

This file predates the surface audit; `P6-SURFACE.md` §3.3/§3.4/§18 adds:
`agentc_message_to_json/from_json/cancel_subtree`; `agentc_queue_get/count/
clear/pause/is_paused/run_of`; the `queue_paused` event/hook; the
`queue_pause`/`queue_resume`/`queue_clear` RPC commands; and the typed error
codes in §14. **R-D17 applies here:** every wait on a step/child must run
`runtime_tick` (actor tick + signal drain) — a wait that skips the bus can
deadlock a child's notify/approval. Fold these in; they do not change any
decision (S-D1…S-D6).
