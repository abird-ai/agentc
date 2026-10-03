/* session.h — JSONL session storage for --continue/--resume.
 *
 * One file per session:
 *   $XDG_DATA_HOME/agentc/sessions/--<sanitized-cwd>--/<timestamp>_<id>.jsonl
 * (overridable by options.dir / --session-dir; env AGENTC_SESSION_DIR).
 *
 * The file is append-only. The first line is a header:
 *   {"type":"session","version":1,"id":"…","timestamp":<unix_ms>,"cwd":"…"}
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
    const char *dir;    /* NULL = default location */
    const char *id;     /* NULL = random 8-hex id */
    bool memory_only;   /* --no-session: nothing is written to disk */
} AgcSessionOptions;

AgcSession *agentc_session_new(const AgcSessionOptions *o);
AgcSession *agentc_session_open(const char *path);        /* resume an existing file */
void agentc_session_close(AgcSession *s);

/* Append one transcript message as a JSONL entry (no-op in memory_only mode). */
int agentc_session_append_message(AgcSession *s, const AgcMsg *m);
/* Append a raw JSON object (must be a single line, already valid JSON). */
int agentc_session_append_raw(AgcSession *s, const char *json, size_t n);
/* Append a compaction checkpoint record (written before the transcript splice so
 * a crash cannot lose the checkpoint). */
int agentc_session_append_compaction(AgcSession *s, const AgcCompactInfo *ci);

const char *agentc_session_path(const AgcSession *s);     /* NULL in memory_only mode */
const char *agentc_session_id(const AgcSession *s);

/* Replay the session file into a transcript (used by --continue/--resume). */
int agentc_session_load_messages(const AgcSession *s, AgcTranscript *tr);

/* Sessions for /resume and --continue. Both return owned string arrays that the
 * caller frees with agentc_sessions_free(). Newest first. */
char **agentc_session_list(const char *dir, size_t *count, size_t max);
char *agentc_session_find_latest(const char *dir, const char *cwd);
void agentc_sessions_free(char **paths, size_t count);

#endif /* AGENTC_SESSION_H */
