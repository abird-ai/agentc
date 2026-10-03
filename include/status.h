/* status.h — status-line segment registry shared by the TUI and the extension
 * registry.
 *
 * Every provider goes through this one table, the built-in segments included:
 * there is no privileged path into the footer. Providers write
 * AgcExtStatusSegment values (include/agentc_ext.h); this module validates
 * and copies them, sorts by (slot, priority, registration order) and hands the
 * TUI a list it alone formats. Text is bounded here and clipped again at draw
 * time, so a chatty provider can never wrap the status row.
 */
#ifndef AGENTC_STATUS_H
#define AGENTC_STATUS_H

#include "agentc.h"
#include "agentc_ext.h"

/* Bound for one snapshot; excess segments are dropped with a log line. */
#define AGENTC_STATUS_MAX_SEGMENTS 24
/* Room for one provider's whole answer before the global cap applies. */
#define AGENTC_STATUS_MAX_PROVIDER_SEGMENTS 8
/* Text arena handed to one provider call; text is copied out immediately. */
#define AGENTC_STATUS_ARENA_CAP 1024
/* Longest text kept per segment, NUL included. Longer text is clipped. */
#define AGENTC_STATUS_TEXT_MAX 192

/* Owned, validated copy of one provider segment. */
typedef struct {
    uint32_t slot;
    int32_t priority;
    uint32_t style;
    char text[AGENTC_STATUS_TEXT_MAX];
    u64 seq;                  /* stable tiebreak: provider + output order */
} AgcStatusValue;

/* Registers a provider. Idempotent for the same (provider, userdata); a NULL
 * provider is ignored with an error log. Registration bumps the version. */
void agentc_status_register(AgcExtStatusProvider provider, void *userdata);
/* Removes one provider by identity (extension rollback). Safe when absent. */
void agentc_status_remove(AgcExtStatusProvider provider, void *userdata);
/* Drops every provider (tests, shutdown) and bumps the version. */
void agentc_status_reset(void);

/* Monotonic counter that changes whenever the provider list or a provider's
 * input changes. The TUI rebuilds its snapshot only when this differs from its
 * cached value, never once per frame. */
u64 agentc_status_version(void);
/* Bumps the version without touching the provider table (provider input
 * changed). Used by the built-in provider's state setter. */
void agentc_status_invalidate(void);

/* Calls every live provider, validates/copies the segments and sorts them by
 * (slot, priority, registration order). Call only when the version changed.
 * Returns the number written (<= max). A provider over the time budget is
 * disabled, its output dropped and the version bumped. */
size_t agentc_status_snapshot(AgcStatusValue *out, size_t max);

/* ---- built-in provider (src/core/status_builtin.c) --------------------- */

typedef struct {
    char model[AGENTC_STATUS_TEXT_MAX];
    char thinking[24];
    u64 tok_in, tok_out;
    i64 cost_micro;
    int spinner;              /* 0..3; rendered only while running */
    i64 elapsed_ms;
    bool running;
} AgcStatusBuiltin;

/* Registers the built-in model/thinking/tokens/cost/state provider through
 * agentc_status_register(). Idempotent. */
void agentc_status_register_builtin(void);
/* Replaces the built-in snapshot; bumps the version only when a rendered value
 * actually changed. */
void agentc_status_builtin_set(const AgcStatusBuiltin *state);

#endif /* AGENTC_STATUS_H */
