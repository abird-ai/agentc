# P6-ACTORS — Actors, processes, and cancellation

Status: **as-built (P6-A..E + completion passes 2a-2g).** Actors, processes and cancellation.
As-built note: the outcome is folded into `.agents/design/20-core-agent.md`; the decision index is
`.agents/plans/p6-runtime/TODO.md`.

Scope of this file:

- the `AgcActor` registry and lifecycle;
- **process actors**: background/foreground shells, pipe and PTY, stdin,
  output, spill;
- **cancellation**: graceful→force escalation, scope, deadlines, idempotency;
- **liveness**: progress/stalled;
- the Layer-0 additions (`os_pty_*`) and how they fit `plat.h`;
- the `process {op: spawn}` tool's foreground/background behavior (`bash` alias);
- failure modes and the golden test plan for P6-A.

Non-goals: the trust envelope and approval policy (`P6-TRUST.md`), the signal
vocabulary in full (`P6-SIGNALS.md` — this file defines only what processes
need), sub-agent spawning (`P6-SUBAGENTS.md`).

---

## 1. Current state (facts to build on)

- `bash` (`src/core/tools/bash.c`) spawns one process group with
  `os_spawn_shell_group`, reads a pipe to EOF or child exit, and on
  cancel/timeout sends SIGTERM then SIGKILL. The result is one string with a
  `[exit code: N, Xms]` trailer and a spill notice. `bash_start`/`bash_step`
  exist so its child overlaps sibling tools **within one batch**.
- `src/core/tools/jobs.c` owns `g_jobs[AGENTC_TOOL_JOBS_MAX]` (32). One batch at
  a time; the whole batch is released before `agentc_tool_jobs_run` returns —
  **nothing a job started survives the batch**.
- `include/plat.h` already provides `os_pipe` (read end `O_NONBLOCK|O_CLOEXEC`),
  `os_spawn`, `os_spawn_group`, `os_spawn_shell`, `os_spawn_shell_group`,
  `os_kill` (negative pid = group), `os_wait(pid, nohang)`, `os_poll`,
  `os_which`, `os_tty_raw/restore/size/isatty`, `os_now_ns`, `os_random`.
- There is **no** PTY open/resize, no process registry, no detach/reap-owner
  separate from a job, and no stall detection.
- Output caps live in `src/base/limits.h`: `AGENTC_LIMIT_TOOL_BYTES` (50000),
  `AGENTC_LIMIT_TOOL_LINES` (2000). Spill uses a 0600 file in `$TMPDIR`
  (`open_spill` in `bash.c`), written via `agentc_tool_append_output`.

## 2. Actor model

### 2.1 Types (append-only; new in `include/actor.h`)

```c
/* actor.h — the runtime actor registry (internal; see P6-ACTORS/P6-SIGNALS).
 * Owner: the core. One table per process, reset by agentc_actor_init. */
typedef enum {
    AGENTC_ACTOR_AGENT = 0,
    AGENTC_ACTOR_PROCESS = 1,
} AgcActorKind;

typedef enum {
    AGENTC_ACTOR_STARTING = 0,
    AGENTC_ACTOR_RUNNING,
    AGENTC_ACTOR_WAITING,    /* agent waiting on a tool/approval/child */
    AGENTC_ACTOR_STALLED,    /* advisory: no progress within idle_ms */
    AGENTC_ACTOR_SETTLED,    /* finished cleanly */
    AGENTC_ACTOR_FAILED,     /* error */
    AGENTC_ACTOR_CANCELLED,  /* cancelled (graceful or force) */
} AgcActorState;

typedef uint32_t AgcActorId;         /* 0 = invalid */

struct AgcActor {
    AgcActorId    id;
    AgcActorKind  kind;
    AgcActorId    parent;            /* 0 for the root agent */
    uint16_t      depth;
    AgcActorState state;
    AgcTrustEnv  *env;               /* borrowed from the owning agent/registry */
    i64           started_ns;
    i64           last_progress_ns;
    i64           deadline_ns;       /* 0 = none (process: bash cap) */
    i64           grace_deadline_ns; /* set when cancel_graceful is pending */
    int           grace_ms;          /* 0 = AGENTC_CANCEL_GRACE_DEF_MS */
    int           idle_ms;           /* 0 = kind default */
    int           cancel_stage;      /* 0 none, 1 graceful, 2 force */
    bool          cancel_seen;       /* `cancelled` already emitted */
    char         *label;             /* short human label for UI/list */
    AgcProcess  *term;              /* process actors only, else NULL */
    struct AgcAgent *agent;          /* agent actors only, else NULL */
};
```

`AgcActor` is the registry row. It is **not** owned by the caller; the registry
owns `label` and the `AgcProcess`, and reaps them on release. The `agent`
pointer is borrowed (the owning `AgcAgent` frees itself) and must be cleared on
agent free.

### 2.2 Registry API

```c
void        agentc_actor_init(void);           /* reset table, install atexit reap */
AgcActorId  agentc_actor_register(AgcActorKind kind, AgcActorId parent,
                                  const char *label);   /* 0 on table full */
AgcActor   *agentc_actor_get(AgcActorId id);
AgcActorId  agentc_actor_parent(AgcActorId id);
size_t      agentc_actor_list(AgcActor *out, size_t max);   /* count-first */
void        agentc_actor_release(AgcActorId id);            /* recursive for agents */
void        agentc_actor_tick(void);                        /* poll/emit/reap/stall */
const char *agentc_actor_state_name(AgcActorState s);
```

- The table is bounded by `AGENTC_ACTOR_MAX` (proposed 64; see §11). Overflow
  returns `0` and logs; the caller surfaces a typed error.
- `agentc_actor_release(id)` for a process closes fds, kills (graceful→force)
  and reaps if still running, frees the ring/spill/label. For an agent it first
  releases all descendants (depth-first), then clears `actor->agent`.
- `agentc_actor_init` registers the atexit cleanup so a crash/exit cannot orphan
  processes (the Codex "failed to clean background processes" path). It is
  called from `agentc_mode_setup` and from the TUI/RPC entry points; tests call
  it directly.

### 2.3 Lifecycle sequence (identical for both kinds)

```text
register(STARTING)
  → start payload ready           → RUNNING, emit actor_started
  → progress/heartbeat            → refresh last_progress_ns
  → (no progress > idle_ms)       → STALLED, emit actor_stalled   [advisory]
  → clean end                     → SETTLED, emit actor_finish
  → error                         → FAILED,  emit actor_fail
  → cancel_graceful               → keep running, start grace timer, emit actor_cancel_requested
  → grace expired                 → cancel_force
  → confirm stop                  → CANCELLED, emit actor_cancelled (once)
release(id)
```

`STALLED` is a state label, not terminal; a later `progress` returns it to
`RUNNING`. Only `SETTLED`/`FAILED`/`CANCELLED` are terminal.

## 3. Process actors

### 3.1 Process payload

```c
typedef struct AgcProcess {
    int   pid;            /* process-group leader; os_kill(-pid, sig) */
    int   fd_out;         /* read end (pipe) or PTY master */
    int   fd_in;          /* write end: -1 (pipe: stdin=/dev/null) or PTY master */
    bool  tty;            /* PTY-backed */
    bool  stdin_open;     /* false after `input{close_stdin}` or fd_in==-1 */
    int   cols, rows;     /* PTY geometry */
    AgcBuf tail;          /* bounded tail ring for UI/list/attach */
    size_t tail_cap;      /* default AGENTC_PROC_TAIL_BYTES */
    u64   total_bytes;    /* bytes seen (for truncation accounting) */
    u64   total_lines;
    u8    last;           /* last byte; trailing-newline decision */
    int   spill_fd;       /* full-output spill, -1 when none */
    AgcBuf spill_path;
    bool  spill_failed;
    int   exit_code;      /* valid when terminal state is terminal */
    int   wait_status;    /* raw from os_wait, or -99 */
    int   read_err;
    bool  eof;
    char *cmd;            /* owned; for list/UI */
    char *cwd;            /* owned; informational */
} AgcProcess;
```

### 3.2 Spawn

```c
/* Spawn a process actor via the unified factory (`AgcActorSpec.proc`):
 *   - shell != NULL -> os_spawn_shell_group(shell, command, ...)
 *   - else          -> os_spawn_group(argv, envp, cwd, ...)
 * The kind-specific builder `agentc_process_spawn(parent, spec, err)` is private
 * to actor.c; callers use `agentc_actor_spawn(&(AgcActorSpec){.proc=spec}, ...)`.
 * `mode`: AGENTC_PROC_PIPE (stdin=/dev/null, merged pipe) or AGENTC_PROC_PTY.
 * Returns a new actor id, or 0 with *err set (negative errno). */
AgcActorId agentc_actor_spawn(const AgcActorSpec *spec, char *err, size_t cap);

typedef struct AgcProcSpawn {
    const char *command;         /* shell form; NULL if argv set */
    const char *shell;           /* NULL = platform default (os_shell_kind) */
    char *const *argv;           /* NULL if command set */
    const char *cwd;             /* NULL = inherit */
    int   mode;                  /* AGENTC_PROC_PIPE | AGENTC_PROC_PTY */
    int   cols, rows;            /* PTY only */
    i64   deadline_ms;           /* 0 = AGENTC_PROC_DEADLINE_DEF_MS (bash cap) */
    int   idle_ms;               /* 0 = kind default for stalled */
    const char *label;           /* UI label; NULL = derived from command */
} AgcProcSpawn;
```

Rules:

- Pipe mode: `os_pipe` for merged output; `stdin = /dev/null` (matches today's
  bash). stderr is merged (bash semantics); a future `stderr_separate` flag is
  deferred.
- PTY mode: `os_pty_open` yields a master and a slave; spawn with
  `fd_in = fd_out = fd_err = slave`, `ctty = 1`; then close the slave in the
  parent. `os_tty_size` on the master reports geometry; resize via
  `os_pty_resize`.
- Process group: always `os_spawn_*_group` so `os_kill(-pid, sig)` reaches the
  whole tree (Windows job object with `KILL_ON_JOB_CLOSE`).
- The spawn applies the **owning agent's effective sandbox** to the child
  (P6-TRUST §8). Process spawn never happens outside an envelope; a spawn with
  no resolvable envelope is denied.
- `deadline_ms` defaults to the bash cap (`BASH_MAX_MS`, 30 min) and may be
  overridden per spawn. A PTY/background process may set `deadline_ms = 0` only
  if the envelope allows unattended long-running processes (a flag; default caps
  at 30 min to prevent leaks).

### 3.3 Ownership and lifetime

- The process's owner is its parent actor (an agent). A process is **not**
  owned by `AgcJob` and must survive the batch that spawned it.
- Foreground `bash` (existing behavior) is: spawn process actor →
  job/batch polls it until `SETTLED|FAILED|CANCELLED` **or** the caller is
  satisfied → release. Background `bash` is: spawn process actor → return a
  handle immediately → the actor persists across steps.
- On agent settle, background processes are **not** auto-killed (the model may
  still poll them); on agent `release`, on `new_session`, and at process exit
  they are reaped (`agentc_actor_release` depth-first). Document this: a
  long-lived background process is bounded by its `deadline_ms` and by the
  agent lifetime.
- On overflow of `AGENTC_ACTOR_MAX`, spawning is refused; the error names the
  limit and suggests killing an existing process (`process op:list`).

## 4. stdin

```c
/* Write bytes to a process actor's stdin. close_stdin sends EOF after.
 * Returns 0 | -ENOTTY (pipe mode has no stdin) | -EPIPE (already closed)
 * | -errno. A zero-length write with close_stdin=false is a no-op (a poll). */
int agentc_process_write(AgcActorId id, const char *bytes, size_t n, bool close_stdin);
```

- PTY mode writes to the master; pipe mode with `fd_in == -1` returns `-ENOTTY`.
  To get stdin in pipe mode, spawn with `mode = AGENTC_PROC_PIPE_STDIN` (a
  future flag) or PTY. **P6-A decision:** interactive stdin requires PTY. Pipe
  spawns document `stdin: closed`. (Codex's unified exec is PTY for interactive
  sessions and pipe for one-shot.)
- `close_stdin` is idempotent; a second close is `-EPIPE` at the C API but is
  treated as success by the `input` signal handler (matches Codex's
  `closeStdin` semantics).
- Writes are bounded (`AGENTC_PROC_WRITE_MAX`, e.g. 64 KiB per call) and
  non-blocking: a full pipe returns `-EAGAIN` and the caller retries after the
  next poll (the signal handler does this in the pump; never blocks the loop).

## 5. Output

### 5.1 Ring + spill + progress

- Every read appends to `tail` (a bounded ring, `AGENTC_PROC_TAIL_BYTES`,
  proposed 64 KiB) **and** to the spill via the existing
  `agentc_tool_append_output` discipline. `tail` is what the UI/RPC `attach`
  and `process op:list` show; the spill holds the full stream for a late reader.
- Each successful read emits exactly one `progress` signal (§P6-SIGNALS) with
  `{actor_id, stream, bytes, total_bytes}`; the bytes themselves are not copied
  into the signal — a reader calls `agentc_process_read` to drain.
- `AGENTC_LIMIT_TOOL_BYTES` (50000) / `AGENTC_LIMIT_TOOL_LINES` (2000) still
  bound the **displayed** result when the process is consumed as a tool result
  (foreground bash). Background processes are not truncated in place; the reader
  applies the cap.

```c
/* Drain up to `max` bytes of not-yet-read output (bounded, non-blocking).
 * Appends to out. Returns bytes appended (0 if none). */
size_t agentc_process_read(AgcActorId id, AgcBuf *out, size_t max);
/* Read the tail ring (for UI/list); returns bytes appended. */
size_t agentc_process_tail(AgcActorId id, AgcBuf *out, size_t max);
/* Current status snapshot for list/UI. */
int     agentc_process_info(AgcActorId id, AgcProcInfo *out);
```

### 5.2 Exit

- On child exit and EOF, the process composes its exit info:
  `exit_code = (status&0x7f) ? 128+(status&0x7f) : (status>>8)&0xff`, wall time
  from `started_ns`, and (for foreground bash) the existing
  `[exit code: N, Xms]` trailer plus the spill notice, byte-for-byte compatible
  with today's `bash_finish`. **The foreground bash output must remain
  golden-identical**; refactor `bash.c` to call the actor layer but keep the
  string.
- On `eof` while the group is still alive (a backgrounded child holding the
  write end), read drains and stops; the process stays `RUNNING` until the
  deadline or the group exits. Same rule as today's comment in `bash.c`.

## 6. Cancellation

### 6.1 Model

Cancellation is a signal (P6-SIGNALS §7) with:

- `scope`: `turn` | `actor`. /*turn = the user-driven cycle (Codex `turn`)*/
- `stage`: `graceful` (explicit) → `force` (explicit or auto after `grace_ms`).

| scope | process | agent |
| --- | --- | --- |
| `turn` | n/a (processes have no turn scope) | stop the current turn: abort the in-flight request and current step (tool batch); keep the agent and its background processes |
| `actor` | close stdin, SIGTERM group → SIGKILL group | the same, plus force-cancel descendants and release on terminal state |

`grace_ms` default `AGENTC_CANCEL_GRACE_DEF_MS` (proposed 3000), per-actor
override. `cancelled` is emitted exactly once with `{stage, reason}`.

### 6.2 Process escalation

1. `cancel_graceful`: `close_stdin` (if open), then `os_kill(-pid, SIGTERM)`.
   Set `grace_deadline_ns = now + grace_ms`.
2. On each `agentc_actor_tick`, if `grace_deadline_ns != 0 && now >= it` and the
   group has not exited (`os_wait(pid, nohang)` still running), escalate:
   `os_kill(-pid, SIGKILL)`, `cancel_stage = force`.
3. Bounded reap: poll `os_wait` for a bounded window (e.g. 200 × 5 ms, matching
   `bash_finish`) so an uninterruptible child cannot block the loop. If the reap
   never completes, mark `FAILED` with a read error and leave the pid recorded
   for the atexit sweep (Windows job object closes on release).
4. Emit `cancelled{stage}` once; compose the result (`[aborted]` for graceful,
   `[aborted]` for force too; the stage is in the signal, the tool text stays
   stable for goldens).

**This generalizes today's SIGTERM→~100 ms→SIGKILL.** The grace becomes
configurable and the text stays compatible.

### 6.3 Agent escalation

- `cancel_graceful` on an agent sets its cancel flag (the same flag
  `agentc_agent_abort` uses) so the loop exits at the next safe point
  (post-tool, post-request) with `AGENTC_STOP_ABORTED`, then emits
  `actor_cancelled`.
- `cancel_force` additionally abandons the in-flight HTTP/SSE stream immediately
  (the transport must expose a cancel hook; `src/core/transport_http.c` already
  polls cooperatively, so the flag is observed within one poll slice) and
  force-cancels descendants.
- `scope: turn` never releases the agent; `scope: actor` releases on process
  state (depth-first descendants first).

### 6.4 Idempotency

- A second cancel for the same actor is a no-op (or a stage upgrade
  `graceful → force`); it never emits a second `cancelled`.
- Cancel after a terminal state is a no-op returning 0.
- Cancel is checked at safe points in the loop (`agentc_tool_checkpoint` for
  synchronous bodies; the pump for async ones), so no code path can miss it.

## 6b. Freeze and resume

Freeze suspends an actor **at its current state** and resumes it later, without
killing it. It is a first-class actor operation (not a cancel variant).

### 6b.1 State and scopes

Add `AGENTC_ACTOR_FROZEN` to `AgcActorState`. Freeze scopes:

- `AGENTC_FREEZE_ACTOR` — one actor.
- `AGENTC_FREEZE_SUBTREE` — depth-first over an agent's descendants.
- `AGENTC_FREEZE_RUNTIME` — every actor (global pause).

### 6b.2 Process actors (LIVE, default)

- Freeze: `os_process_freeze(pid, /*group*/true)` → `kill(-pid, SIGSTOP)`.
- Resume: `os_process_resume(pid, true)` → `SIGCONT`.
- The process tree stays alive and stopped; it consumes no CPU. Buffered
  output already in the pipe/PTY remains readable; no new output is produced.
- The read loop pauses: the actor stops polling its fd while frozen (the fd is
  not closed). On resume the loop drains whatever accumulated.
- stdin writes while frozen are buffered up to `AGENTC_PROC_FREEZE_IN_MAX`
  (proposed 64 KiB) and flushed on resume; overflow returns `-EAGAIN`.

### 6b.3 Agent actors

Agent freeze is **cooperative at safe points** (the loop cannot be
`sigstop`ed without freezing the whole process): the actor tick stops advancing
a frozen agent and blocks in a pump that drains only control signals
(resume/cancel). An in-flight model request is handled per
`AgcFreezeFlags`:

- `AGENTC_FREEZE_DRAIN` (default): let the request complete, buffer the
  message, and freeze before the next safe point.
- `AGENTC_FREEZE_ABORT`: abandon the in-flight stream now (`MSG_RESET` on
  resume) and freeze immediately.

Pending approvals remain pending and may still be answered while the agent is
frozen; the freeze pauses the loop, not the approver.

### 6b.4 Timers and liveness (important)

A frozen actor must not time out or be reported stalled:

- On freeze, record `frozen_ns = now`. While `frozen_ns != 0`, the actor tick
  skips `deadline_ns`, `grace_deadline_ns` and stall evaluation.
- On resume, shift `deadline_ns` and `grace_deadline_ns` forward by
  `now - frozen_ns`, set `last_progress_ns = now`, clear `frozen_ns`. A long
  freeze therefore does not consume the deadline or trigger `STALLED`.
- `AGENTC_FREEZE_MAX_MS` (proposed 0 = unbounded) optionally auto-resumes; the
  default is unbounded, bounded in practice by the atexit reap.

### 6b.5 Interaction with cancellation

- `cancel_graceful` on a frozen actor: `SIGCONT` first (a stopped process does
  not handle `SIGTERM` until continued), then `SIGTERM`; the grace timer runs
  from the continue.
- `cancel_force`: `SIGCONT` then `SIGKILL` (robust on all platforms).
- `new_session`, `runtime_shutdown` and the atexit reap: resume-then-terminate
  every frozen actor; a stopped child must never be left orphaned.
- Cancel of a frozen actor resolves its queue? No — the queue is preserved until
  `release`; freeze/resume never drops queue items (they are delivered on
  resume).

### 6b.6 “Come back later” — across agentc exit (CHECKPOINT, optional)

LIVE freeze keeps the process only for the lifetime of the `agentc` process; if
`agentc` exits, the reaper terminates frozen children (they cannot outlive it).
To survive a restart, an optional **checkpoint** backend snapshots the process
tree to disk:

- `AGENTC_FREEZE_CHECKPOINT` + `os_process_checkpoint(pid, dir, err, cap)`:
  Linux CRIU (`criu dump`/`criu restore`), opt-in (`trust.freeze.backend =
  live|checkpoint`), `-ENOSYS` where unavailable; never a hard dependency.
- Restore re-creates process actors from the checkpoint and rebinds their
  session records. The **agent's** in-memory step state is not checkpointed;
  resume relies on the existing session replay (transcript + `custom` records).
- `AGENTC_EV_ACTOR_CHECKPOINTED` records the checkpoint path so the session can
  locate it.

This tier is explicitly optional and OS-specific; the default tier is LIVE.

### 6b.7 Surface (fold into the specs)

Signals `SIG_FREEZE`/`SIG_RESUME` (`P6-SIGNALS` §7b), `SIG_FROZEN`/`SIG_RESUMED`;
events `AGENTC_EV_ACTOR_FROZEN`/`_RESUMED`/`_CHECKPOINTED`; hooks
`actor_frozen`/`actor_resumed`/`actor_checkpointed`; C API
`agentc_actor_freeze/resume/frozen`, `agentc_runtime_freeze/resume`; tools
`process op:freeze|resume` and `agent op:freeze|resume`; RPC
`actor_op {op: freeze|resume|checkpoint}` (and the runtime/global form); config `trust.freeze.{max_ms,agent_mode,
backend,stdin_max}`; session `actor{op:freeze|resume|checkpoint}`; limits
`AGENTC_PROC_FREEZE_IN_MAX`, `AGENTC_FREEZE_MAX_MS`.

### 6b.8 Failure modes

| Failure | Behavior |
| --- | --- |
| freeze a released actor | `-ESRCH` |
| freeze a process without group support | per-pid `SIGSTOP` (best effort) |
| stdin write to a frozen process, buffer full | `-EAGAIN` |
| checkpoint backend unavailable | `-ENOSYS`; LIVE freeze still applies if requested |
| resume a non-frozen actor | no-op `0` |
| frozen child at `agentc` exit | reaper resumes then terminates |
| freeze during a model request | per `DRAIN`/`ABORT` mode |

## 7. Liveness: progress and stalled

- `last_progress_ns` is refreshed on: process read, agent model delta, agent
  tool start/end, and explicit heartbeat.
- `idle_ms` default: processes `AGENTC_PROC_IDLE_DEF_MS` (proposed 30000),
  agents `AGENTC_AGENT_IDLE_DEF_MS` (proposed 120000, to tolerate long model
  latency). Per-actor override.
- `agentc_actor_tick` computes `idle = now - last_progress_ns`; on crossing
  `idle_ms` while `RUNNING`, emit `stalled` once (not per tick) and set
  `STALLED`. A later progress clears `STALLED` and re-arms (emit at most once
  per stall episode; a hysteresis of `idle_ms/2` before re-arming).
- `stalled` is advisory. It does **not** cancel. The controller (parent/user)
  decides: `input`, `cancel_graceful`, or wait.
- A hard `deadline_ns` still escalates to force independently of stall.

## 8. Integration with the existing job driver and the agent loop

### 8.1 Universal tool execution (internal, external, MCP)

`P6-HOOKS.md` §4 defines one **runner** API for every tool, and every tool call
traverses the same `tool`/`call` → execute → `tool`/`result` stages:

| runner kind | executes as | actor |
| --- | --- | --- |
| `INTERNAL` | a C function in-process (`read`/`write`/`edit`/`ls`, built-in `grep`/`find`) | none |
| `PROCESS` | an external process (bash, `rg`/`fd`/POSIX `grep`/`find`) | **process actor** |
| `MCP` | an MCP tool call on a long-lived server process | **process actor** (the server) |

- **One process model.** Every external process in the runtime — bash, the
  external engine rungs, and MCP servers — is a **process actor**, so it gets
  the same sandbox, cancellation, freeze, output and list/attach surfaces. There
  is no separate pid/fd owner.
- **The job driver becomes a scheduler over runners.** `src/core/tools/jobs.c`
  keeps batch ordering, concurrency, per-job timeout and source-order results,
  but it no longer owns `pid`/`fd`: a process runner exposes a process actor it
  polls, exactly like foreground bash today (`job->actor`).
- **MCP calls are tool calls.** They use the same `tool` stage, the same envelope
  decision and the same approval branch; the MCP server process is a process
  actor spawned under the actor's envelope. There is no MCP-specific hook point.
- **Internal tools go through hooks by default** (uniform audit/policy/redaction),
  and may opt out with `AGENTC_TOOL_NO_HOOKS` (or `tools.hooks=off`); MCP and
  process runners honor the same flags. See `P6-HOOKS.md` §4.
- **Agent/process/queue ops are tools.** `agent op:spawn`, `process op:*`,
  `queue op:*` are tool calls, so the spawn/permission decisions live in the
  `tool`/`call` hook rather than a dedicated seam.

### 8.2 Foreground/background bash and the tick

- **Foreground `bash`** stays a tool job for scheduling/parallelism: `bash_start`
  spawns a process actor and stores its id in the job (`job->actor`); `bash_step`
  polls the actor and, on terminal state, composes the legacy string and marks
  the job done. `job->cleanup` releases the actor when the job is fatal/timeout/
  cancel — but the default path lets the actor reach terminal state first so the
  exit trailer is exact.
- **Background `bash`** (`background: true` or a `yield_ms` exceeded) starts the
  actor and returns a handle; the job completes immediately with the handle and
  **must not** release the actor. Add `job->owns_actor = false` for this path so
  the driver's release sweep skips it.
- **`agentc_actor_tick` is called from `agentc_pump`** (installed by the mode
  and by `bash_step_once` today) so background processes keep draining while a
  foreground tool runs or while the model streams. It is also called explicitly
  from the agent loop's poll slices.
- The agent loop gains **safe points** where it drains its own signals (P6-B):
  before request build, after `message_end`, and after each tool batch. P6-A
  only needs the tick and the cancel flag; the drain points are P6-B.

## 9. Layer-0 additions (`plat.h`, append-only)

```c
/* PTY. os_pty_open allocates a master/slave pair and optionally sets the
 * initial window size; the caller spawns with fd_in=fd_out=fd_err=slave,
 * ctty=1, then closes slave in the parent. -> 0 | -errno. */
int os_pty_open(int *master, int *slave, int cols, int rows);
int os_pty_resize(int fd, int cols, int rows);   /* -> 0 | -errno */
int os_pty_size(int fd, int *cols, int *rows);   /* -> 0 | -errno */
```

- **Linux:** `/dev/ptmx` + `TIOCSPTLCK`/`TIOCGPTN` + `ptsname`, then open the
  slave; `TIOCSWINSZ`/`TIOCGWINSZ` for size. `O_CLOEXEC` on the master; the
  slave must not be `O_CLOEXEC` so the child inherits it (or reopen in child).
- **macOS:** `posix_openpt`/`grantpt`/`unlockpt`/`ptsname`; same ioctls.
- **Windows:** ConPTY (`CreatePseudoConsole`) already underpins `os_spawn`'s
  terminal path; `os_pty_open` is **not** portable as a master/slave pair.
  **As built:** P6-E shipped without a ConPTY shim, so Windows process actors
  run in pipe mode and `os_pty_open` stays `-ENOSYS` (`src/plat/win/sys.c`); a
  `tty: true` request on Windows fails closed with a clear error, never a
  silent pipe. This is a real platform asymmetry, not a pending wave.
- **headless test backend:** `os_pty_open` returns `-ENOSYS`; tests use pipe
  mode and the scripted process mock.

`os_kill`, `os_wait`, `os_pipe`, `os_spawn_group` are unchanged. The atexit
cleanup hook can reuse `agentc_atexit` from `src/app`/core.

## 10. Failure modes (must be explicit)

| Failure | Behavior |
| --- | --- |
| Actor table full | spawn refused, typed error naming `AGENTC_ACTOR_MAX`; suggest `process op:list` |
| PTY unavailable (`-ENOSYS`) | `tty:true` denied with a clear message; never silent pipe fallback |
| Sandbox apply fails | spawn denied (fail-closed), error names the boundary and platform |
| stdin write on pipe mode | `-ENOTTY`; the tool result tells the model stdin is closed |
| stdin pipe full | `-EAGAIN`; the signal handler retries in the pump, bounded by `deadline_ms` |
| child holds pipe open after exit | drain and stop (today's rule); stay RUNNING until deadline |
| uninterruptible child after SIGKILL | bounded reap; leave pid recorded; atexit/job-object closes it |
| spill open/write fails | latch `spill_failed`; the result says the full output could not be saved (bash parity) |
| output over cap | tail ring drops oldest; spill retains full (if not failed); reader applies display cap |
| deadline exceeded | compose `[timed out after Nms]` (bash parity) and force |
| atexit with live processes | `agentc_proc_reap_all`-equivalent: graceful then force, bounded; never hang exit |

## 11. Limits (propose these names in `src/base/limits.h`)

| Name | Proposed value | Guards |
| --- | --- | --- |
| `AGENTC_ACTOR_MAX` | 64 | actors per process |
| `AGENTC_PROC_TAIL_BYTES` | 65536 | process tail ring |
| `AGENTC_PROC_WRITE_MAX` | 65536 | one stdin write |
| `AGENTC_PROC_IDLE_DEF_MS` | 30000 | process stall |
| `AGENTC_AGENT_IDLE_DEF_MS` | 120000 | agent stall |
| `AGENTC_CANCEL_GRACE_DEF_MS` | 3000 | graceful→force |
| `AGENTC_PROC_DEADLINE_DEF_MS` | 1800000 | bash-compatible hard cap |
| `AGENTC_PROC_READ_MAX` | 262144 | one drain call |

Existing `AGENTC_LIMIT_TOOL_BYTES`/`_LINES` are unchanged and still bound display.

## 12. Acceptance for P6-A

P6-A is done when:

1. `agentc_actor_*` exists with the registry, list, release, tick, and the
   atexit reap; a golden test drives the full state machine (see below).
2. Process actors spawn in pipe mode with merged output and (on Linux/macOS)
   PTY mode with stdin, resize, and `TIOCSWINSZ` honored by a real child.
3. `bash` runs through the actor layer; **its foreground output is byte-for-byte
   unchanged** (existing `tools_test` bash goldens pass without edits).
4. `bash` gains `background:true` and `yield_ms`; an unfinished spawn returns a
   stable actor id and the actor survives the batch and the step.
5. `agentc_process_write` writes stdin (PTY) and `close_stdin` EOF; `-ENOTTY`
   in pipe mode.
6. Cancellation is graceful→force with `grace_ms`, idempotent, emits `cancelled`
   once, and kills the process group (no orphans in a real test).
7. Stall fires once per episode at `idle_ms` and clears on progress.
8. `make check`, `./tests/ext.sh`, `./tests/mcp.sh`, `./tests/e2e.sh`,
   `make windows`/`wine-check`/`dyn-check` green.
9. `design/20-core-agent.md` §4 documents actors/processes and the bash
   foreground/background split in the same change.

### Golden test plan (new `tests/actors_test.c` + `tests/data/actors_test.expected`)

Drive the state machine through a scripted process mock (extend
`src/plat/headless.c` with a deterministic spawn stub: canned output, canned
exit, controllable "still running" state, and a recorded kill log):

- register → started → progress → finish; assert states + events, byte-exact.
- register → started → stalled (advance a fake clock) → progress clears stall.
- cancel_graceful → child ignores → grace expiry → force kill (assert the kill
  log is `SIGTERM` then `SIGKILL`, group pid).
- cancel idempotency: two cancels, one `cancelled`.
- actor table full: the (MAX+1)th spawn is refused with the exact error.
- stdin: PTY write, close_stdin idempotent, pipe-mode `-ENOTTY`.
- tail ring: >tail bytes in, tail shows the last `tail_cap`, spill holds all.
- foreground bash parity: reuse the existing bash goldens (no edits).
- atexit reap: two live processes, process mock records graceful+force.

Determinism: fake clock (`os_now_ns` injectable in the headless backend) and a
recorded syscall log, matching the existing golden discipline.

## 13. Handoff (P6-A)

> Implement `plans/P6-ACTORS.md` only. Read `plans/P6-RUNTIME.md` for the
> decision log. Create `include/actor.h`, `src/core/actor.c`,
> `src/core/process.c`; add `os_pty_*` to `include/plat.h` and the
> linux/mac/headless backends; refactor `src/core/tools/bash.c` to use the actor
> layer while keeping its output byte-identical; add the
> `background`/`yield_ms` args and the actor-backed step loop. Do not touch the
> trust, signal, or sub-agent surfaces beyond what `P6-ACTORS.md` requires.
> Acceptance and tests are §12. Keep every P6-RUNTIME §7 gate green. Append to
> headers; never edit. Record any deviation as a decision in this file before
> committing.

## 14. Audit addenda (authoritative in `P6-SURFACE.md`)

This file predates the surface audit; `P6-SURFACE.md` §3.2/§18 adds:
`agentc_process_resize/kill/spill_path/set_stdin`, `AgcProcSpawn.env`,
`AGENTC_ACTOR_ATTACH_MAX`, the `actor_resized` event/hook, and the
`AGENTC_ACTOR_LABEL_MAX`/`AGENTC_MSG_MAX_TEXT` limits. Fold these into the
P6-A implementation; they do not change any decision.
