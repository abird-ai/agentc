/* status.h — status-line registry shared by the TUI and the extension registry.
 *
 * Every status line goes through this one table, the built-ins included: there
 * is no privileged path into the footer. A line registers an AgcExtStatusLine
 * (include/agentc_ext.h); this module renders it, validates/copies each output
 * row, sorts by (region, priority, registration order) and hands the TUI a list
 * it alone formats. Text is bounded here and clipped again at draw time, so a
 * chatty line can never wrap the status row.
 *
 * Two natures share the table: chrome regions (PERSISTENT) are polled through
 * agentc_status_snapshot(); event regions (PER_TURN/ONCE) are rendered once at
 * a step/turn boundary through agentc_status_emit().
 */
#ifndef AGENTC_STATUS_H
#define AGENTC_STATUS_H

#include "agentc.h"
#include "agentc_ext.h"

/* Bound for one snapshot; excess rows are dropped with a log line. */
#define AGENTC_STATUS_MAX_SEGMENTS 24
/* Longest text kept per row, NUL included. Longer text is clipped. */
#define AGENTC_STATUS_TEXT_MAX 192
/* Per-region row cap; a line that emits more rows is clipped. */
#define AGENTC_STATUS_MAX_LINES_PER_REGION 3
/* Internal region selector: snapshot every chrome region in enum order. */
#define AGENTC_STATUS_ALL_REGIONS (-1)

/* Owned, validated copy of one rendered status row. */
typedef struct {
    int region; /* AgcStatusRegion */
    int32_t priority;
    uint32_t style;
    char text[AGENTC_STATUS_TEXT_MAX];
    u64 seq; /* stable tiebreak: registration + output order */
} AgcStatusValue;

/* Registers a line. Idempotent for the same (region, render, userdata); a NULL
 * render or an unknown region/lifetime is ignored with an error log.
 * Registration bumps the version. */
void agentc_status_register(const AgcExtStatusLine *line);
/* Removes one line by identity (extension rollback). Safe when absent. */
void agentc_status_remove(int region,
                          int (*render)(const AgcExtHost *, void *, const AgcExtStatusCtx *,
                                        void *),
                          void *userdata);
/* Drops every registration (tests, shutdown) and bumps the version. */
void agentc_status_reset(void);

/* Monotonic counter that changes whenever the registration list or a line's
 * input changes. The TUI rebuilds its snapshot only when this differs from its
 * cached value, never once per frame. */
u64 agentc_status_version(void);
/* Bumps the version without touching the table (line input changed). Used by
 * the built-in state setter. */
void agentc_status_invalidate(void);

/* Renders every PERSISTENT line in `region` (or every chrome region when
 * `region == AGENTC_STATUS_ALL_REGIONS`), validates/copies the rows and sorts
 * them by (region, priority, registration order). Call only when the version
 * changed. Returns the number written (<= max). A line over the time budget is
 * disabled, its output dropped and the version bumped. */
size_t agentc_status_snapshot(int region, AgcStatusValue *out, size_t max);

/* Renders every PER_TURN/ONCE line in `region` with `ctx` and appends the
 * resulting rows (UTF-8, one per line) to `out`. Returns the row count. ONCE
 * lines render only the first time. No-op fast path when the region is empty. */
size_t agentc_status_emit(int region, const AgcExtStatusCtx *ctx, AgcBuf *out);

/* True when at least one enabled line is registered for `region`. Used to skip
 * the render/notice path on the hot boundary when nothing is registered. */
bool agentc_status_has_region(int region);

/* ---- built-in lines (src/core/status_builtin.c) ------------------------ */

typedef struct {
    char model[AGENTC_STATUS_TEXT_MAX];
    char thinking[24];
    u64 tok_in, tok_out;
    i64 cost_micro;
    int spinner; /* 0..3; rendered only while running */
    i64 elapsed_ms;
    bool running;
} AgcStatusBuiltin;

/* Registers the built-in model/thinking/tokens/cost/state lines through
 * agentc_status_register(). Idempotent. */
void agentc_status_register_builtin(void);
/* Replaces the built-in snapshot; bumps the version only when a rendered value
 * actually changed. */
void agentc_status_builtin_set(const AgcStatusBuiltin *state);

#endif /* AGENTC_STATUS_H */
