/* jobs.h — bounded tool job driver (internal).
 *
 * One batch at a time: the driver keeps a static AgcJob table, so
 * agentc_tool_jobs_run() is not re-entrant. Async tools are started first (in
 * source order) so their children overlap the slow synchronous tools; every
 * step is driven by one os_poll(..., 20) loop, so no call blocks the loop for
 * longer than ~20 ms.
 *
 * Ownership: the driver owns job->out, job->spill_path and job->priv, closes
 * job->fd and reaps job->pid, and frees all of it when the batch returns. The
 * completion callback therefore must copy anything it needs (the agent loop
 * copies job->out into its own slots).
 *
 * `exec` normalization: a tool with run == NULL and exec != NULL is executed
 * through the v1 exec entry point, so every existing tool keeps working
 * unchanged.
 */
#ifndef AGENTC_CORE_TOOLS_JOBS_H
#define AGENTC_CORE_TOOLS_JOBS_H

#include "agent.h"

struct AgcJob {
    const AgcTool *tool;
    AgcToolCall call;      /* by value; the strings stay owned by the caller */
    size_t index;
    int state;                /* 0 free, 1 running, 2 done, 3 cancelled */
    i64 deadline_ns;          /* absolute monotonic ns; 0 = none; start() may shorten */
    AgcBuf out;
    size_t total_bytes;
    bool is_error;
    i64 duration_ms;
    int pid, fd, spill_fd;    /* -1 when unused */
    AgcBuf spill_path;
    bool spill_notified;      /* tool composed its own final text (bash); finalize skips */
    void *priv;               /* tool-private; freed by the driver with agentc_free */
    /* Invoked exactly once by the driver, on fatal, timeout,
     * cancel and release (the pointer is cleared before calling, so it can
     * never run twice). The guarantee is that it has run before `priv` is
     * released; when the child/transport is torn down relative to it depends
     * on the path (fatal/timeout/cancel run it before close/kill, the release
     * sweep closes/kills first). `reason` is one of AGENTC_JOB_STOP_*; while it
     * runs job->priv is still valid. */
    void (*cleanup)(const AgcTool *self, AgcJob *job, int reason);
};

/* AgcJob.cleanup reasons, mirroring the extension tool stop reasons. */
#define AGENTC_JOB_STOP_ERROR     1
#define AGENTC_JOB_STOP_TIMEDOUT  2
#define AGENTC_JOB_STOP_CANCELLED 3
#define AGENTC_JOB_STOP_RELEASE   4

/* Every negative-capable field becomes -1, state free. */
void agentc_tool_job_init(AgcJob *j);

/* Append tool output under a streaming cap: up to `cap` bytes (0 selects
 * AGENTC_LIMIT_TOOL_BYTES) stay in j->out, everything beyond is written to a
 * 0600 spill file created on first overflow. Returns 0 or -errno. */
int agentc_tool_append_output(AgcJob *j, const char *p, size_t n, size_t cap);

/* Close the spill file if any and apply the generic display cap + truncation
 * notice. A tool that set spill_notified (it built its own final text, e.g.
 * bash's status trailer and operator-facing notice) is left untouched: only
 * its spill fd is closed. Returns 1 when anything was truncated. */
int agentc_tool_finalize_output(AgcJob *j);

/* Cooperative checkpoint for synchronous run() bodies: 0 / -ECANCELED /
 * -ETIMEDOUT. Outside a run it always returns 0. */
int agentc_tool_checkpoint(void);

/* what: 1 = started, 2 = done. Called for every job exactly once at completion,
 * and once at start for jobs that actually begin running. Return value ignored. */
typedef int (*AgcJobCb)(void *ud, int what, const AgcJob *job);
#define AGENTC_JOB_STARTED 1
#define AGENTC_JOB_DONE    2

/* Run one batch of at most AGENTC_TOOL_JOBS_MAX calls. pre_errors[i] != NULL
 * marks call i as pre-failed (veto): it is answered with that text without
 * running. tools[i] == NULL answers "error: unknown tool: <name>". Returns 0,
 * or -ERANGE after answering every call when n is over the cap. */
int agentc_tool_jobs_run(const AgcTool *const *tools, const char *const *pre_errors,
                         const AgcToolCall *calls, size_t n,
                         AgcJobCb cb, void *ud);

#endif /* AGENTC_CORE_TOOLS_JOBS_H */
