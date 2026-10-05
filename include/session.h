/* session.h — JSONL session storage for --continue/--resume.
 *
 * One file per session:
 *   $XDG_DATA_HOME/abird/agentc/sessions/--<sanitized-cwd>--/<timestamp>_<id>.jsonl
 * (overridable by options.dir / --session-dir; env AGENTC_SESSION_DIR).
 *
 * The file is append-only. The first line is a header:
 *   {"type":"session","version":1,"id":"…","timestamp":<unix_ms>,"cwd":"…"}
 * A forked session adds an optional `"parent_session":"<id>"` key (P7); an
 * absent key is a root session and a v1 reader ignores the unknown key.
 * Messages follow as serde-style entries with snake_case fields:
 *   {"type":"message","role":"user","ts":…,"content":[{"type":"text","text":"…"}],
 *    "tool_call_id":"…","is_error":false}
 * plus generic custom entries appended with agentc_session_append_raw().
 */
#ifndef AGENTC_SESSION_H
#define AGENTC_SESSION_H

#include "agentc.h"
#include "agent.h"

typedef struct AgcSession AgcSession;

typedef struct {
    const char *cwd;
    const char *dir;  /* NULL = default location */
    const char *id;   /* NULL = random 8-hex id */
    bool memory_only; /* --no-session: nothing is written to disk */
    /* Appended P7: fork lineage. The parent session's id is written as an
     * optional header key; NULL/"" = a root session. */
    const char *parent_session;
} AgcSessionOptions;

AgcSession *agentc_session_new(const AgcSessionOptions *o);
AgcSession *agentc_session_open(const char *path); /* resume an existing file */
void agentc_session_close(AgcSession *s);

/* Append one transcript message as a JSONL entry (no-op in memory_only mode). */
int agentc_session_append_message(AgcSession *s, const AgcMsg *m);
/* Append a raw JSON object (must be a single line, already valid JSON). */
int agentc_session_append_raw(AgcSession *s, const char *json, size_t n);
/* Append a compaction checkpoint record (written before the transcript splice so
 * a crash cannot lose the checkpoint). */
int agentc_session_append_compaction(AgcSession *s, const AgcCompactInfo *ci);

const char *agentc_session_path(const AgcSession *s); /* NULL in memory_only mode */
const char *agentc_session_id(const AgcSession *s);
/* Appended P7: the fork-lineage parent id recorded in the header, or NULL. */
const char *agentc_session_parent(const AgcSession *s);

/* Replay the session file into a transcript (used by --continue/--resume). */
int agentc_session_load_messages(const AgcSession *s, AgcTranscript *tr);

/* Invoke cb(ud, data_json) for every `custom` record whose custom_type matches,
 * in file order (data_json is the record's `data` value, JSON text). cb returns
 * 0 to continue or non-zero to stop and propagate through the return. Returns 0
 * or -errno. No-op (0) in memory_only mode. */
int agentc_session_load_custom(const AgcSession *s, const char *custom_type,
                               int (*cb)(void *ud, const char *data_json), void *ud);

/* Appended P7: invoke cb(ud, custom_type, data_json) for every `custom` record
 * in file order (used to copy a session's custom records into a fork). cb returns
 * 0 to continue or non-zero to stop. Returns 0 or -errno. No-op in memory-only
 * mode. */
int agentc_session_load_custom_all(const AgcSession *s,
                                   int (*cb)(void *ud, const char *custom_type,
                                             const char *data_json),
                                   void *ud);

/* P6-D: reconstruct the persisted agent tree from `custom type:"agent"`
 * records, read-only (no child is re-run). Count-first: `out == NULL` returns
 * the record count; otherwise up to `max` records are filled in file order.
 * Strings are owned by the caller-visible rows and freed with
 * agentc_session_agents_free. */
typedef struct {
    AgcActorId id, parent_id;
    uint16_t depth;
    int exit_code;
    char *op, *provider, *model;
} AgcSessionAgent;
size_t agentc_session_load_agents(const AgcSession *s, AgcSessionAgent *out, size_t max);
void agentc_session_agents_free(AgcSessionAgent *a, size_t n);

/* Sessions for /resume and --continue. Both return owned string arrays that the
 * caller frees with agentc_sessions_free(). Newest first. */
char **agentc_session_list(const char *dir, size_t *count, size_t max);
char *agentc_session_find_latest(const char *dir, const char *cwd);

/* Metadata for the resume picker: the header timestamp (unix ms; 0 when the
 * file has no header) and a single-line preview of the first user message
 * (empty when there is none). `preview` is NUL-terminated when cap > 0, never
 * contains a newline, and reads a bounded prefix of the file. Returns 0 or
 * -errno. */
int agentc_session_summary(const char *path, i64 *timestamp_ms, char *preview, size_t cap);
/* Compact locale-free age label ("now"/"5m"/"2h"/"3d") for a session
 * timestamp, for the pickers. `out` is always NUL-terminated. */
void agentc_session_age_label(i64 ts_ms, i64 now_ms, char *out, size_t cap);
void agentc_sessions_free(char **paths, size_t count);

/* Appended P7 — the /tree fork forest. Count-first: `out == NULL` returns the
 * number of sessions; otherwise up to `max` rows are filled, newest first, with
 * the header's id/parent_session/timestamp. Owned strings on each row; free the
 * array with agentc_session_nodes_free(). */
typedef struct {
    char *path;      /* owned */
    char *id;        /* owned */
    char *parent_id; /* owned; NULL = root */
    i64 timestamp_ms;
} AgcSessionNode;
size_t agentc_session_tree(const char *dir, AgcSessionNode *out, size_t max);
void agentc_session_nodes_free(AgcSessionNode *nodes, size_t n);

#endif /* AGENTC_SESSION_H */
