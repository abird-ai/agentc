/* actor.h — the runtime actor registry and process actors (P6-A).
 *
 * An actor is an addressable unit of work: `agent` (an in-process model loop)
 * or `process` (a running OS process; a terminal is a process actor with
 * `tty = true`). One registry per process, reset by agentc_actor_init(). The
 * registry owns every actor's label and, for process actors, the AgcProcess.
 * The `agent` pointer is borrowed (the owning AgcAgent frees itself and the
 * core clears it on agent free).
 *
 * Owners: src/core/actor.c (registry, lifecycle, tick, release) and
 * src/core/process.c (spawn, I/O, ring/spill, cancel, freeze). See
 * .agents/plans/P6-ACTORS.md for the normative semantics.
 */
#ifndef AGENTC_ACTOR_H
#define AGENTC_ACTOR_H

#include "agentc.h"

/* Forward declarations. `AgcTrustEnv` (P6-C trust.h) and `AgcAgentSpec`
 * (P6-D) are defined by later waves; P6-A never dereferences the envelope and
 * returns -ENOTSUP for an `agent` spec until P6-D lands. */
struct AgcTrustEnv;
struct AgcAgentSpec;
struct AgcAgent;

typedef uint32_t AgcActorId; /* 0 = invalid */

typedef enum {
    AGENTC_ACTOR_AGENT = 0,
    AGENTC_ACTOR_PROCESS = 1,
} AgcActorKind;

typedef enum {
    AGENTC_ACTOR_STARTING = 0,
    AGENTC_ACTOR_RUNNING,
    AGENTC_ACTOR_WAITING,   /* agent waiting on a tool/approval/child */
    AGENTC_ACTOR_STALLED,   /* advisory: no progress within idle_ms */
    AGENTC_ACTOR_FROZEN,    /* suspended at its current state (R-D18) */
    AGENTC_ACTOR_SETTLED,   /* finished cleanly */
    AGENTC_ACTOR_FAILED,    /* error */
    AGENTC_ACTOR_CANCELLED, /* cancelled (graceful or force) */
} AgcActorState;

/* Process actor payload. All buffers are owned by the registry; fds are -1
 * when unused. `pending` holds bytes read but not yet drained by a reader
 * (bounded by AGENTC_PROC_READ_MAX); `tail` is the last AGENTC_PROC_TAIL_BYTES
 * for UI/list; the spill file holds the full stream once it exceeds the tail. */
typedef struct AgcProcess {
    int pid;         /* process-group leader; os_kill(-pid, sig) */
    int fd_out;      /* read end (pipe) or PTY master */
    int fd_in;       /* write end: -1 (pipe: no stdin) or PTY master */
    int fd_err;      /* merged with fd_out in P6-A */
    bool tty;        /* PTY-backed */
    bool stdin_open; /* false after `close_stdin` or fd_in == -1 */
    int cols, rows;  /* PTY geometry */
    AgcBuf pending;  /* drained by agentc_process_read */
    AgcBuf tail;     /* bounded tail ring for UI/list */
    size_t tail_cap; /* default AGENTC_PROC_TAIL_BYTES */
    u64 total_bytes;
    u64 total_lines;
    u8 last;      /* last byte; trailing-newline decision */
    int spill_fd; /* full-output spill, -1 when none */
    AgcBuf spill_path;
    bool spill_failed;
    bool no_spill;   /* foreground consumer spills itself */
    int exit_code;   /* valid once reaped */
    int wait_status; /* raw from os_wait, or -99 */
    int read_err;
    bool eof;         /* read end at EOF */
    bool reaped;      /* os_wait returned a status */
    char *cmd;        /* owned; for list/UI */
    char *cwd;        /* owned; informational */
    AgcBuf stdin_buf; /* writes buffered while frozen */
} AgcProcess;

/* The registry row. Not owned by the caller. */
typedef struct AgcActor {
    AgcActorId id;
    AgcActorKind kind;
    AgcActorId parent; /* 0 for the root agent */
    uint16_t depth;
    AgcActorState state;
    struct AgcTrustEnv *env; /* borrowed; P6-C fills it */
    i64 started_ns;
    i64 last_progress_ns;
    i64 deadline_ns;        /* 0 = none */
    i64 grace_deadline_ns;  /* set when cancel_graceful is pending */
    int grace_ms;           /* 0 = AGENTC_CANCEL_GRACE_DEF_MS */
    int idle_ms;            /* 0 = kind default */
    int cancel_stage;       /* 0 none, 1 graceful, 2 force */
    bool cancel_seen;       /* `cancelled` already emitted */
    bool stalled_seen;      /* `stalled` emitted for this episode */
    i64 frozen_ns;          /* 0 = not frozen; else freeze instant */
    char *label;            /* short human label for UI/list */
    AgcProcess *proc;       /* process actors only, else NULL */
    struct AgcAgent *agent; /* agent actors only, else NULL */
    bool attached;          /* a UI is subscribed to the stream */
    /* Appended P6-B: the agent turn counter (0 = idle; incremented when turn S
     * begins) and whether the active step is steerable (false during compact). */
    int64_t turn_seq;
    bool steerable;
} AgcActor;

/* Process spawn modes and flags. */
#define AGENTC_PROC_PIPE 0            /* stdin=/dev/null, merged output pipe */
#define AGENTC_PROC_PTY 1             /* master/slave pair; writable stdin */
#define AGENTC_PROC_PIPE_BIDI 2       /* merged output pipe + writable stdin pipe */
#define AGENTC_PROC_F_NO_SPILL 0x1u   /* foreground consumer spills itself */
#define AGENTC_PROC_F_STDIN_PIPE 0x2u /* internal: bidi pipe (P6-E voice/MCP) */

typedef struct AgcProcSpawn {
    const char *command;  /* shell form; NULL if argv set */
    const char *shell;    /* NULL = platform default */
    char *const *argv;    /* NULL if command set */
    const char *cwd;      /* NULL = inherit */
    int mode;             /* AGENTC_PROC_PIPE | AGENTC_PROC_PTY */
    int cols, rows;       /* PTY only */
    i64 deadline_ms;      /* 0 = AGENTC_PROC_DEADLINE_DEF_MS */
    int idle_ms;          /* 0 = kind default for stalled */
    const char *label;    /* UI label; NULL = derived from command */
    uint32_t flags;       /* AGENTC_PROC_F_* */
    const char *env_json; /* reserved: explicit small env map */
    /* Appended P6-C: the effective envelope for this spawn (borrowed; NULL =
     * unconfined). process.c resolves spec->env ?: agentc_trust_current(). */
    const struct AgcTrustEnv *env;
} AgcProcSpawn;

typedef struct AgcActorSpec {
    const AgcProcSpawn *proc; /* exactly one of proc/agent is set */
    const struct AgcAgentSpec *agent;
} AgcActorSpec;

/* Snapshot for list/UI (strings borrowed from the actor). */
typedef struct AgcProcInfo {
    AgcActorId id;
    int state; /* AgcActorState */
    int pid;
    bool tty;
    bool stdin_open;
    int cols, rows;
    int exit_code;
    u64 total_bytes;
    u64 total_lines;
    i64 duration_ms;
    bool eof;
    bool reaped;
    const char *cmd;        /* borrowed */
    const char *spill_path; /* borrowed; "" when none */
} AgcProcInfo;

/* Freeze scopes and flags (P6-ACTORS §6b). */
#define AGENTC_FREEZE_ACTOR 0
#define AGENTC_FREEZE_SUBTREE 1
#define AGENTC_FREEZE_RUNTIME 2

#define AGENTC_FREEZE_DRAIN 0x1u
#define AGENTC_FREEZE_ABORT 0x2u
#define AGENTC_FREEZE_CHECKPOINT 0x4u

/* ------------------------------------------------------------- registry */
void agentc_actor_init(void); /* reset table, install atexit reap */
AgcActorId agentc_actor_register(AgcActorKind kind, AgcActorId parent,
                                 const char *label); /* 0 on table full */
AgcActor *agentc_actor_get(AgcActorId id);
AgcActorId agentc_actor_parent(AgcActorId id);
size_t agentc_actor_list(AgcActor *out, size_t max); /* count-first */
void agentc_actor_release(AgcActorId id);            /* recursive for agents */
void agentc_actor_release_all(void);
void agentc_actor_tick(void); /* poll/emit/reap/stall */
const char *agentc_actor_state_name(AgcActorState s);

bool agentc_actor_exists(AgcActorId id);
int agentc_actor_info(AgcActorId id, AgcActor *out); /* copy snapshot */
int agentc_actor_set_label(AgcActorId id, const char *label);
size_t agentc_actor_count(AgcActorKind kind_or_any); /* -1 = any */
AgcActorId agentc_actor_root(void);
int agentc_actor_query(AgcActorId id, AgcBuf *out_json);
/* Appended P6-B: the agent actor's turn counter (0 = idle). */
int64_t agentc_actor_turn(AgcActorId id);

/* One actor factory (R-D25). Returns the id or 0 with `err` set (negative
 * errno text). The kind-specific builder is private. */
AgcActorId agentc_actor_spawn(const AgcActorSpec *spec, char *err, size_t cap);

/* Actor control (CONTROL-message sugar). Cancel is graceful→force and
 * idempotent; emits `actor_cancel_requested` then `actor_cancelled` once. */
int agentc_actor_cancel(AgcActorId id, int scope, int stage);
int agentc_actor_freeze(AgcActorId id, int scope, uint32_t flags);
int agentc_actor_resume(AgcActorId id, int scope);
bool agentc_actor_frozen(AgcActorId id);
int agentc_actor_checkpoint(AgcActorId id, const char *dir, char *err, size_t cap);

/* ------------------------------------------------------------- process */
int agentc_process_write(AgcActorId id, const char *bytes, size_t n, bool close_stdin);
size_t agentc_process_read(AgcActorId id, AgcBuf *out, size_t max);
size_t agentc_process_tail(AgcActorId id, AgcBuf *out, size_t max);
int agentc_process_info(AgcActorId id, AgcProcInfo *out);
int agentc_process_resize(AgcActorId id, int cols, int rows);
int agentc_process_kill(AgcActorId id, int stage);
int agentc_process_spill_path(AgcActorId id, char *out, size_t cap);
int agentc_process_set_stdin(AgcActorId id, bool open);

/* Internal: advance one process actor (read fd, reap, timers). Called by
 * agentc_actor_tick; exposed so a foreground consumer can pump one actor. */
int agentc_process_poll(AgcActorId id);

#endif /* AGENTC_ACTOR_H */
