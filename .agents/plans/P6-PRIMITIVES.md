# P6-PRIMITIVES — Four primitives and two hook verbs

Status: **as-built (P6-A..E + completion passes 2a-2g).** The four primitives and two hook verbs.
As-built note: the outcome is folded into `.agents/design/20-core-agent.md` and `.agents/design/30-extensibility.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

The whole runtime reduces to **four primitives** and **two hook verbs**.
Everything else — queue, signals, hooks/dispositions, deciders, router, jev,
approval, freeze, steer, attach, spawn, branch — is *derived*.

---

## 1. The four primitives

| # | Primitive | What it is | Why it is irreducible |
| --- | --- | --- | --- |
| 1 | **Runtime** | the single-threaded scheduler + reactor: fds, clock, drain, **continuations** | there must be one place that advances time and resumes parked work |
| 2 | **Actor** | an addressable unit of work (`agent` \| `process`) with an inbox, state, I/O and an envelope | there must be something that runs and is addressable |
| 3 | **Message** | the one unit of communication: control signals **and** ordered input, one envelope, one inbox | actors must communicate; control and work are the same thing with different classes |
| 4 | **Envelope** | the trust/sandbox boundary an actor carries | what an actor may do must be bounded and enforceable |

There is no fifth. The earlier "Queue", "Signal" and "Pipeline" primitives are
**derivations** (§4–§5): queue = the ordered input subset of an inbox; signal =
a message class; pipeline = an ordered transform chain over the message path.

**Continuation** is a runtime facility, not a fifth primitive: a parked
`(actor, message, resume_fn)` the runtime resumes. It is how `defer`, `await`,
`branch` and blocking waits are all built.

## 2. Minimal C API

### 2.1 Runtime (scheduler + continuations)

```c
int  agentc_runtime_init(const AgcTrustEnv *root);
int  agentc_runtime_step(int timeout_ms);                 /* poll + service + drain + resume */
size_t agentc_runtime_pollfds(struct os_pollfd *out, size_t max); /* embed in a host loop */
uint64_t agentc_runtime_defer(void (*resume)(void *ud, AgcMessage *), void *ud); /* park */
void agentc_runtime_cancel_defer(uint64_t token);
void agentc_runtime_shutdown(const char *reason);
```

### 2.2 Actor (one factory + I/O surface)

```c
uint32_t agentc_actor_spawn(const AgcActorSpec *, char *err, size_t);
int      agentc_actor_query(uint32_t, AgcBuf *out_json);          /* state|info|queue|trust|result */
size_t   agentc_actor_read(uint32_t, AgcBuf *out, size_t max);    /* drain output/result */
int      agentc_actor_pollfds(uint32_t, struct os_pollfd *out, size_t max);
void     agentc_actor_release(uint32_t);
size_t   agentc_actor_list(AgcActor *, size_t max);
```

`AgcActorSpec` is a tagged spec — `{ const AgcProcSpawn *proc; const AgcAgentSpec
*agent; }` with exactly one pointer set. The two kinds are `agent` (an
in-process model loop) and `process` (a running OS process; `tty` is a flag, not
a kind). One factory; the kind-specific builders are private. Everything after
spawn is uniform.

### 2.3 Message (the one communication verb)

```c
int  agentc_message_send(const AgcMessage *);             /* enqueue into an actor's inbox */
int  agentc_message_to_json(const AgcMessage *, AgcBuf *);
int  agentc_message_from_json(const char *, size_t, AgcMessage *);
```

### 2.4 Envelope (policy; type `AgcTrustEnv`)

```c
int agentc_trust_default(AgcTrustEnv *, const char *cwd);
int agentc_trust_resolve(const char *cwd, bool project_trusted, int cli_verdict, AgcTrustEnv *out);
int agentc_trust_narrow(const AgcTrustEnv *parent, const AgcTrustEnv *requested, AgcTrustEnv *out,
                        char *err, size_t cap);
int agentc_trust_decide(const AgcTrustEnv *, const AgcMessage *call, AgcTrustDecision *out);
int agentc_trust_to_json/from_json/describe/free/clone(...);
```

That is the primitive surface. `agentc_hook_*`, `agentc_queue_*`, actor control
(cancel/freeze/resume/write/resize), deciders and approval are services/adapters
built on these four (§4–§5, and `P6-SURFACE.md`).

## 3. The message — control and work unified

One envelope for everything an actor sends or receives. This removes the
Signal/Queue split: a control signal and a queued input differ only by `class`
and ordering key.

```c
typedef struct AgcMessage {
    uint64_t  seq;           /* per-sender, monotonic */
    uint32_t  from, to;      /* actor ids; to=0 broadcast (control only) */
    uint8_t   class;         /* CONTROL | INPUT | NOTIFY | HOOK */
    uint16_t  kind;          /* the opcode (see below) */
    int64_t   turn;         /* INPUT: 0 auto, >0 absolute target turn */
    int32_t   priority;      /* INPUT: signed nice -20..19 */
    uint32_t  flags;         /* class-specific (preempt tools, close stdin, …) */
    int       scope;         /* CANCEL/FREEZE: turn | actor | subtree | runtime */
    int       stage;         /* CANCEL: graceful | force */
    const char *client_id;   /* optional caller correlation id */
    uint32_t  actor_id;      /* subject (hook/tool/decision items) */
    uint16_t  depth;         /* branch/reentry depth */
    uint64_t  id;            /* stable id for correlation/loops */
    uint8_t   direction;     /* IN | OUT (hook path) */
    bool      final;         /* last chunk of a stream */
    const char *body;        /* bounded payload (JSON or bytes) */
    size_t     body_len;
    const char *content_type;/* "text/plain" | "application/json" |
                                "audio/pcm;rate=24000;ch=1" |
                                "application/x-realtime-item" | … (opaque to core) */
    const char *meta;        /* bounded context (envelope/model/step/lineage) */
} AgcMessage;
```

`content_type` makes the pipeline **media-agnostic**: audio, transcripts and
realtime items ride the same stages without the core knowing what they are
(`P6-VOICE.md` §3).

- **`class = CONTROL`** → delivered immediately (cancel, freeze, resume, attach,
  detach, write/resize, the `DECIDE`/`DECIDED` round-trip). These are the old
  "signals".
- **`class = INPUT`** → scheduled input, ordered by `(step, priority, seq)`. This
  *is* the queue (`P6-SIGNALS` §5); `agentc_queue_add` is a convenience that
  sends an INPUT message. Preempt (`priority < 0`) and boundary (`>= 0`)
  semantics are unchanged.
- **`class = NOTIFY`** → upward lifecycle/question messages.
- **`class = HOOK`** → a message on the interception path (§5).

### 3.1 Kinds (the opcode is the message kind)

Adding a capability is a new `kind`, not a new function. Control kinds:
`STARTED/PROGRESS/STALLED/FINISH/FAIL/CANCEL/CANCELLED/FREEZE/RESUME/FROZEN/
RESUMED/ATTACH/DETACH/WRITE/RESIZE/DECIDE/DECIDED/SET_MODEL/SET_THINKING/
SETTLE`. Input kinds: `ENQUEUE`.
Notify kinds: `NEEDS_INPUT/BLOCKED/FINISHED/FAILED/…`. Hook kinds are the stage
names (§5).

## 4. The pipeline is a service, not a primitive

Interception is an ordered chain of **transforms** applied to messages at an
actor's boundary. It is built on Message + Runtime continuations, so it adds no
primitive.

**The whole hook contract — two verbs:**

```c
/* Return the message to continue with (usually `m`, possibly modified), or NULL
 * to consume it; set *defer to park and resume later. 0 | -errno. */
typedef int (*AgcHookFn)(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer);
uint64_t agentc_hook(const char *stage, int order, AgcHookFn fn, void *ud);
void     agentc_hook_off(uint64_t handle);
int      agentc_hook_run(const char *stage, AgcMessage *m, AgcMessage **out);
bool     agentc_hook_wants(const char *stage);   /* hot-path guard */
```

- **continue / edit / replace / observe** → return `m` (mutated) or a new message.
  These are not separate verbs; they are *which message you return*.
- **stop / consume** → return `*next = NULL` (optionally with a terminal body).
- **defer** → set `*defer` (a runtime continuation); the chain pauses and resumes
  when the continuation fires with the (possibly replaced) message.
- **branch / emit / fork** → a handler calls the relevant *primitive* itself
  (`agentc_actor_spawn` + `agentc_runtime_defer` for branch; `agentc_message_send`
  for emit/fork) and either continues or defers. A one-line helper
  `agentc_hook_branch(spec, m, &defer)` is sugar, not a disposition.

The earlier eight verbs (`pass/edit/replace/stop/defer/branch/emit/fork`) collapse
to **`next` + `defer`**. Nothing is lost; the combinators are now explicit
one-liners instead of a vocabulary the core must interpret.

- **Handlers chain** by `order`; each sees the current message; the next handler
  sees `next`. `*next = NULL` ends the chain (consume).
- **Stages** are message kinds on the hook path, named after the data:
  `turn`, `step`, `input`, `prompt`, `message`, `output`, `tool`, `signal`,
  `event`, `lifecycle`. (`turn` = user cycle start/end/steer; `step` = model
  cycle start/end/model_select/compact; `tool` covers call/result for
  internal/external/MCP; decisions are handlers, not stages — R-D23/R-D31.)
- **Handler kinds** all satisfy the same contract: C function, program
  (JSON stdio), MCP, model, or **branch-to-agent** (a C helper that spawns an
  actor and defers on its result).
- **Async** is just `defer`: the actor enters `WAITING`; `runtime_step` resumes.
  No primitive blocks.
- **Safety**: item `depth` + `AGENTC_HOOK_MAX_DEPTH` bound recursion; a per-item
  hook set blocks accidental reentry; per-stage `on_error` chooses `pass`
  (default for transform stages) or `deny` (default for gate stages
  `tool_call`/`decision`/`route`).

## 5. Derivation map (everything → four primitives)

| capability | built from |
| --- | --- |
| queue, `queue_add/list/...` | an INPUT message + inbox ordering (a service) |
| signals (cancel/freeze/resume/attach/write/resize) | a CONTROL message kind |
| request/response (`DECIDE`/`DECIDED`, approval) | two messages + a continuation |
| hooks / interception | pipeline service over Message + `defer` |
| universal tool execution | one `AgcToolRunner` (internal/process/mcp) through the `tool` stage; external/MCP procs are process actors (`P6-HOOKS` §4) |
| per-step interception | the `step` stage (start/end/model_select/compact) |
| per-turn interception | the `turn` stage (start/end/steer) |
| branch to another agent | `actor_spawn` + `runtime_defer` (helper `hook_branch`) |
| emit / fork | `message_send` |
| stop / edit / replace / observe | which message the transform returns |
| deciders / router / jev | hook handlers on `decision`/`route` using `hook_branch` |
| approval | a hook handler on `tool_call` using `hook_branch` |
| await / blocking waits | runtime continuation (blocking helper pumps `step`) |
| freeze/resume/cancel | CONTROL messages + actor state |
| session records, RPC, tools, events | adapters over the four |

## 6. Async invariants

1. One thread; `runtime_step` is the only driver; no primitive blocks.
2. `defer` is the only async escape; it registers a continuation and parks.
3. Every actor fd is visible via `runtime_pollfds`; one poll set.
4. Reads/writes never block (`0`/`-EAGAIN`).
5. Timers are absolute and freeze-suspendable.
6. New capabilities are new message kinds; the function set is closed.

## 7. Coverage — four primitives express everything

| capability | composition |
| --- | --- |
| shell / server / PTY | `actor_spawn(process)` + `runtime_step` + `actor_read` + CONTROL messages |
| sub-agent / parallel children | `actor_spawn(agent)` × N + one poll set + continuations |
| steer / queue / interrupt | INPUT message `(turn, priority)` + CONTROL `CANCEL` |
| approval / router / jev | `tool_call`/`route` hook + `hook_branch` + `defer` |
| sandbox / read-only | `envelope_decide` + `os_sandbox_apply` |
| redaction / translation / audit | `output`/`message`/`tool` hook transforms |
| freeze / resume | CONTROL messages + actor state + timer shift |
| replay / audit | session records written from messages/events |

## 8. Why four

- **Runtime** and **Actor** are structural (time + work).
- **Message** unifies what were Signal and Queue: both are "send something to an
  actor"; the difference is a class and an ordering key, not a mechanism.
- **Envelope** is the only non-structural concept (policy), and cannot be folded
  into any other without losing enforcement.
- **Pipeline**, **Queue**, **Signal kinds**, and every disposition are
  *services/combinators* over these four — so they are not primitives.

This keeps the primitive set closed and small while still expressing the full
P6 capability (hooks, deciders, router, jev, approval, freeze, steer, spawn).

## 9. Non-goals

No daemon/app-server, no threads/coroutines runtime, no per-feature primitives,
no rich disposition vocabulary, no blocking primitive.
