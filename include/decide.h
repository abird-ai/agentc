/* decide.h — the decision seam: deciders are hook handlers, not stages (P6-D).
 *
 * Per R-D20/R-D23 there is no `decision`/`route` stage. A decider is an
 * ordinary handler on `input`/`tool`/`step` that `agentc_hook_branch`es to a
 * backend (local rules, program, mcp, model) and returns a message. The core
 * interprets only the returned message; every side effect is an ordinary
 * primitive call (`message_send`, `hook_branch`). P6-DECIDERS is the normative
 * spec. Append-only; included from agent.h last.
 */
#ifndef AGENTC_DECIDE_H
#define AGENTC_DECIDE_H

#include "agentc.h"
#include "message.h"

/* A decider backend kind. */
typedef enum {
    AGENTC_DECIDER_LOCAL = 0,
    AGENTC_DECIDER_PROGRAM,
    AGENTC_DECIDER_MCP,
    AGENTC_DECIDER_MODEL,
} AgcDeciderKind;

/* A decision outcome. */
typedef enum {
    AGENTC_VERDICT_ALLOW = 0,
    AGENTC_VERDICT_DENY,
    AGENTC_VERDICT_NARROW,
    AGENTC_VERDICT_REPLACE,
} AgcVerdict;

typedef struct AgcDeciderSpec {
    uint32_t struct_size;
    const char *id;     /* deciders.<id> */
    const char *intent; /* route|approve|spawn|model_select|compact|steer|escalate */
    int kind;           /* AgcDeciderKind */
    const char *provider, *model;
    i64 max_tokens;      /* MODEL */
    const char *command; /* PROGRAM */
    const char *system;  /* MODEL reviewer prompt */
    uint32_t flags;      /* AGENTC_DECIDER_FAIL_CLOSED | AGENTC_DECIDER_DEFAULT */
    uint32_t max_hops;   /* 2 by default (P6-DECIDERS §5) */
} AgcDeciderSpec;

#define AGENTC_DECIDER_FAIL_CLOSED 0x1u
#define AGENTC_DECIDER_DEFAULT 0x2u

/* The multi-effect record (P6-DECIDERS §2b.1). Strings are owned unless noted. */
typedef struct AgcDecisionRecord {
    int verdict;        /* AgcVerdict */
    char *args_json;    /* NARROW */
    char *replace_json; /* REPLACE: an `agent op:spawn` call */
    bool approval_required;
    char *approval_reason;
    char *provider, *model, *thinking; /* side effects */
    bool done;
    char *queue_text;
    i64 queue_turn;
    int32_t queue_priority;
    char *reason;
    uint64_t id; /* appended: out-of-band correlation id */
} AgcDecisionRecord;

int agentc_decider_register(const AgcDeciderSpec *spec);
int agentc_decide(const AgcDeciderSpec *spec, AgcMessage *req, uint64_t *defer);
/* Async variant used by the controller handler: a MODEL decider parks on a
 * self-resuming branch and registers an out-of-band pending row, so
 * agentc_decision_submit can answer it from the pump. LOCAL/PROGRAM/MCP still
 * answer synchronously. */
int agentc_decide_async(const AgcDeciderSpec *spec, AgcMessage *req, uint64_t *defer);
int agentc_decision_submit(const char *response_json);
int agentc_decision_parse(const char *json, AgcDecisionRecord *out, char *err, size_t cap);
void agentc_decision_record_free(AgcDecisionRecord *rec);
int agentc_controller_apply(AgcMessage *m, const AgcDecisionRecord *rec, AgcMessage **next,
                            uint64_t *defer, AgcBuf *err);
/* test/introspection seams (core-internal). */
void agentc_decider_reset(void);
size_t agentc_decider_count(void);
int agentc_decision_memo_count(void);
void agentc_decision_pending_reset(void);
int agentc_decision_pending_count(void);
void agentc_decision_set_parent(struct AgcAgent *parent);
const char *agentc_decider_verdict_name(int verdict);

/* Default controller/router handlers (P6-DECIDERS §2b). A controller branches
 * to its decider and applies the multi-effect record; the router is the same on
 * `input`. Register with `agentc_hook(stage, order, agentc_controller_handler,
 * ctx)`. */
typedef struct AgcControllerCtx AgcControllerCtx;
AgcControllerCtx *agentc_controller_new(const AgcDeciderSpec *spec);
void agentc_controller_free(AgcControllerCtx *ctx);
int agentc_controller_handler(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer);
int agentc_router_handler(void *ud, AgcMessage *m, AgcMessage **next, uint64_t *defer);

#endif /* AGENTC_DECIDE_H */
