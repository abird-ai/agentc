/* agent.c — the synchronous turn loop: request, stream, tools, repeat.
 *
 * Transport is injected; the default agentc_transport_http() is wired in main.c
 * (and is an empty stub until src/wire lands). Events are emitted in the
 * order documented in .agents/design/00-architecture.md.
 */
#include "agent.h"
#include "agent_int.h"
#include "base/limits.h"
#include "config.h"
#include "core/actor_int.h"
#include "core/events.h"
#include "core/tools/jobs.h"
#include "ext.h"
#include "oauth.h"
#include "plat.h"
#include "status.h"
#include "prov/provider.h"

/* internal helpers from messages.c / prompt.c / retry.c / transport_http.c */
void agentc_msg_add_tool_result(AgcMsg *m, const char *call_id, const char *name,
                                const char *result);
void agentc_msg_add_tool_result_n(AgcMsg *m, const char *call_id, const char *name,
                                  const char *result, size_t len);
AgcBlock *agentc_msg_block_new(AgcMsg *m, int type);
void agentc_msg_block_append(AgcBlock *b, const char *p, size_t n);
void agentc_msg_clear_blocks(AgcMsg *m);
size_t agentc_msg_count_tool_calls(const AgcMsg *m);
AgcBlock *agentc_msg_nth_tool_call(AgcMsg *m, size_t n);
char *agentc_prompt_build(const AgcTool *tools, size_t ntools);
i64 agentc_retry_backoff_ms(int attempt, i64 retry_after_ms);
bool agentc_retry_retryable(int rc);
int agentc_transport_http_last_status(void);
i64 agentc_transport_http_retry_after_ms(void);
const char *agentc_transport_http_last_body_excerpt(void);
/* P6 completion pass 2f: install the current agent's cancel predicate on the
 * HTTP transport so an in-flight SSE read observes an abort mid-body. */
void agentc_transport_http_set_cancel(const volatile bool *cancel);

/* compact.c helpers (not part of the frozen header) */
u32 agentc_compact_estimate(const AgcTranscript *tr);
size_t agentc_compact_cut(const AgcTranscript *tr, u32 keep_recent_tokens);
const char *agentc_compact_system_prompt(void);

/* interactive pump (installed by the TUI, called by tools and the loop) */
static AgcPumpFn g_pump;
static void *g_pump_ud;
void agentc_pump_install(AgcPumpFn pump, void *ud) {
    g_pump = pump;
    g_pump_ud = ud;
}
void agentc_pump(int timeout_ms) {
    if (g_pump)
        g_pump(g_pump_ud, timeout_ms);
    /* P6-B: the single runtime driver drains the bus and advances actors, so a
     * child's notify/input can never deadlock a foreground wait (R-D17). */
    agentc_runtime_tick();
}

#define AGENT_DEFAULT_ATTEMPTS 5
#define AGENT_REQUEST_TIMEOUT_MS 600000

#define REQ_BUILD_FAIL (-1000) /* internal, outside errno range */
#define REQ_MALFORMED (-1001)
#define REQ_BLOCKED (-1002) /* an extension override blocked the request */

/* Resumable scheduler state (P6 completion pass 2e). The turn loop advances one
 * model step per call from this object: the foreground `agentc_agent_submit`
 * drives it to completion synchronously (never yielding), while a background
 * agent actor's `agentc_actor_tick` advances exactly one step and returns
 * `AGENTC_RUN_YIELD` at the post-tool safe point (after the whole tool batch,
 * before the next provider request). Because the yield is after the batch has
 * returned, no job-table pointer (`job_tools`/`slots`/`JobCtx`) is live across a
 * slice; the only cross-slice locals are the run counters below. */
typedef struct {
    bool started;      /* setup (input/system/agent_start/user msg) done */
    bool resume_inner; /* resume at the top of the inner step loop */
    bool have_transport;
    bool coop; /* P5: a cooperative run is in flight; step it */
    int rc;    /* terminal rc, carried across a slice */
    int terminal_stop;
    int continuations;
} AgcRunState;

struct AgcAgent {
    const AgcProvider *prov;
    char *model;           /* owned */
    char *api_key;         /* owned */
    char *base_url;        /* owned */
    char *system;          /* owned explicit override; NULL = auto/build */
    char *system_override; /* owned per-run before_agent_start override */
    AgcTransport transport;
    AgcTool *tools; /* owned deep copy (agentc_agent_set_tools) */
    size_t ntools;
    AgcEventFn events;
    void *events_ud;
    AgcToolVetoFn veto;
    void *veto_ud;
    AgcMsgObserver observer;
    void *observer_ud;
    AgcRecomposeFn recompose;
    void *recompose_ud;
    int thinking;
    i64 max_tokens;
    int max_attempts;
    AgcBuf *record;
    bool insecure;
    bool no_backoff; /* tests: skip real sleeps */
    int timeout_ms;
    volatile bool cancel;
    bool auto_compact;
    u32 compact_reserve; /* default 16384 */
    u32 compact_keep;    /* default 20000 */
    bool compact_done;
    int step_index; /* pi-style: reset on agent_start, ++ per turn */
    AgcTranscript tr;
    char last_error[1024];

    /* Appended P6-B: agent actor binding and steer/cancel state. */
    AgcActorId actor_id;      /* 0 until agentc_agent_bind_actor */
    bool steer_preempt;       /* a preempt-priority item is due now */
    bool steer_preempt_tools; /* that preempt also cancels tool processes */
    bool steer_boundary;      /* a boundary-priority item is due */
    bool nonsteerable;        /* true during a non-steerable step (compact) */
    bool auto_start;          /* submit+auto-start enabled */
    bool tools_hooks;         /* run tool call/result through the pipeline */
    bool settle;              /* a CONTROL SETTLE requested the turn end */
    /* Appended P6-C: the effective trust envelope (borrowed; NULL = unconfined). */
    const AgcTrustEnv *trust;

    /* Appended P6-D: the agent tree / result state. */
    AgcActorId parent_id; /* 0 for the root */
    uint16_t depth;
    bool is_subagent;
    bool owned_by_actor;    /* child: the registry row frees it */
    bool ephemeral;         /* do not persist/emit lifecycle records */
    bool background;        /* wait:false; advanced by the actor tick */
    bool running;           /* a background turn is in flight */
    AgcTrustEnv *owned_env; /* child explicit envelope; freed on destroy */
    char *task;             /* owned initial task text */
    char *result;           /* owned final text (result_set) */
    bool result_set;
    int result_exit;
    const char *steerable_kind; /* NULL = steerable; "compact"/"review" reserved */
    char *pending_input;        /* owned; background run input */
    uint64_t step_no;           /* steps in the current turn (status ctx) */

    /* Appended P6 completion pass 2e: resumable scheduler state. */
    AgcRunState run;

    /* Appended (token budget, P6-SURFACE §18 #23): a soft cap on the agent's
     * model tokens *per submitted turn*. 0 = none; a child inherits the parent's
     * value when its spec leaves it 0. `tokens_spent` sums usage.input + output +
     * cache reads/writes for the current turn and resets at each turn start, so
     * reaching the cap stops the turn rather than bricking the agent. */
    i64 token_budget;
    uint64_t tokens_spent;
    bool budget_notified; /* one actor_notify per turn */
    bool budget_hit;      /* this run ended on the cap: do not auto-continue */

    /* Appended W4 (virtual models): the last virtual route published on
     * AGENTC_EV_MODEL_ROUTE as "alias|provider|model", so a multi-step alias use
     * appends one session record, not one per step. Owned. */
    char *last_route;

    /* Appended P8-D1 (B-M8): the per-agent route seam and persistence owner,
     * borrowed (installed by the mode layer, inherited by children). */
    AgcRouteSeam route;
    void *owner;
    /* Appended P8-D2 (B-M9): the agent's model is a virtual alias. Set when the
     * model is chosen; route_resolve fails closed if the alias row is later
     * cleared, so the literal alias is never sent to the host provider. */
    bool model_is_alias;
};

/* The agent currently inside agentc_agent_submit; used by agentc_decide and
 * agentc_hook_branch to inherit the shared provider/transport (R-D2). */
static AgcAgent *g_current_agent;
static u64 g_tool_hook_seq; /* synthetic tool-stage message ids */

/* Resumable turn loop (defined below): foreground drives to completion, a
 * background actor tick runs one model step and may return AGENTC_RUN_YIELD. */
static int agent_run_turn(AgcAgent *a, const char *text, bool background);

/* Tool table ownership: the agent holds a deep copy of the array and of every
 * owned string field. `ud` and the function pointers stay borrowed (opaque), so
 * an MCP/extension registry may free the source strings and array immediately
 * after installing them. Both helpers keep the table consistent on replacement
 * and on free; they are the only writers of a->tools/ntools. */
static void agent_free_tools(AgcAgent *a) {
    if (!a->tools)
        return;
    for (size_t i = 0; i < a->ntools; i++) {
        agentc_free((char *)a->tools[i].name);
        agentc_free((char *)a->tools[i].label);
        agentc_free((char *)a->tools[i].desc);
        agentc_free((char *)a->tools[i].params_json);
    }
    agentc_free(a->tools);
    a->tools = NULL;
    a->ntools = 0;
}

static AgcTool *agent_copy_tools(const AgcTool *tools, size_t n) {
    if (!tools || n == 0)
        return NULL;
    if (n > (size_t)-1 / sizeof(AgcTool))
        return NULL; /* n * sizeof cannot wrap */
    AgcTool *copy = agentc_alloc(n * sizeof *copy);
    for (size_t i = 0; i < n; i++) {
        copy[i] = tools[i];
        copy[i].name = tools[i].name ? agentc_strdup(tools[i].name) : NULL;
        copy[i].label = tools[i].label ? agentc_strdup(tools[i].label) : NULL;
        copy[i].desc = tools[i].desc ? agentc_strdup(tools[i].desc) : NULL;
        copy[i].params_json = tools[i].params_json ? agentc_strdup(tools[i].params_json) : NULL;
    }
    return copy;
}

static const AgcModel *default_model(const AgcProvider *p) {
    if (!p)
        return NULL;
    /* Prefer a catalog entry on the provider's own wire, so the `openai` name
     * cannot default a chat model onto the Codex Responses api (or vice versa). */
    const AgcProviderOps *ops = agentc_provider_ops(p);
    const char *api = ops ? ops->api : NULL;
    const AgcModel *all[512];
    size_t n = agentc_model_all(all, 512);
    for (size_t i = 0; i < n; i++) {
        if (!agentc_streq(all[i]->provider, p->name))
            continue;
        if (api && all[i]->api && !agentc_streq(all[i]->api, api))
            continue;
        return all[i];
    }
    return NULL;
}

/* The base allocation/initialization body shared by the legacy wrapper and the
 * unified constructor (R-D1). */
static AgcAgent *agent_new_base(const AgcProvider *prov, const char *model) {
    AgcAgent *a = agentc_alloc(sizeof *a);
    a->prov = prov;
    a->transport.request = NULL;
    a->transport.ud = NULL;
    a->max_attempts = AGENT_DEFAULT_ATTEMPTS;
    a->timeout_ms = AGENT_REQUEST_TIMEOUT_MS;
    a->compact_reserve = 16384;
    a->compact_keep = 20000;
    a->tools_hooks = true; /* tools.hooks default on (HOK §4) */
    agentc_transcript_init(&a->tr);
    if (model && model[0]) {
        a->model = agentc_strdup(model);
    } else {
        const AgcModel *m = default_model(prov);
        a->model = agentc_strdup(m ? m->id : "");
    }
    a->tr.model = a->model;
    a->tr.provider = prov ? prov->name : NULL;
    /* P8-D2 (B-M9): an alias is a fact about the agent, not an inference made
     * per step. Record it when the model is chosen; route_resolve re-checks the
     * registry and fails closed if the row is later cleared. */
    a->model_is_alias = agentc_model_find_virtual(a->model) != NULL;
    return a;
}

/* Free the owned fields and the agent itself; actor wiring is handled by the
 * caller (agentc_agent_free / agent_actor_release). */
static void agent_destroy(AgcAgent *a) {
    if (!a)
        return;
    agentc_transcript_free(&a->tr);
    agent_free_tools(a);
    agentc_free(a->model);
    agentc_free(a->api_key);
    agentc_free(a->base_url);
    agentc_free(a->system);
    agentc_free(a->system_override);
    agentc_free(a->task);
    agentc_free(a->result);
    agentc_free(a->pending_input);
    agentc_free(a->last_route);
    if (a->owned_env) {
        agentc_trust_env_free(a->owned_env);
        a->owned_env = NULL;
    }
    agentc_free(a);
}

/* Provider resolution default (P8-D1): the static provider table. A per-agent
 * seam (AgcRouteSeam.provider) overrides it; there is no process-global slot, so
 * one session's teardown can never strip another's resolution. */
static const AgcProvider *resolve_provider_default(void *ud, const char *name) {
    (void)ud;
    if (!name || !name[0])
        return NULL;
    const AgcProviderOps *ops = agentc_provider_by_name(name);
    return ops ? agentc_provider_handle((AgcProviderOps *)ops) : NULL;
}

/* Filter a deep-copied tool table by the effective envelope, freeing the
 * dropped rows' owned strings so no allocation leaks. */
static void agent_filter_tools(AgcAgent *a, const AgcTrustEnv *env) {
    if (!a || !env || !a->tools || a->ntools == 0)
        return;
    size_t before = a->ntools;
    agentc_trust_filter_tools(env, a->tools, &a->ntools);
    for (size_t i = a->ntools; i < before; i++) {
        agentc_free((char *)a->tools[i].name);
        agentc_free((char *)a->tools[i].label);
        agentc_free((char *)a->tools[i].desc);
        agentc_free((char *)a->tools[i].params_json);
    }
}

/* At the depth ceiling the `agent` tool is hidden from the model (declaration);
 * agent_run() still returns depth_limit for any direct call (enforcement). */
static void agent_depth_filter(AgcAgent *a) {
    if (!a)
        return;
    if ((int)a->depth + 1 < agentc_config_subagent_max_depth())
        return;
    for (size_t i = 0; i < a->ntools; i++)
        if (a->tools[i].name && agentc_streq(a->tools[i].name, "agent"))
            a->tools[i].flags |= AGENTC_TOOL_HIDDEN;
}

/* Derive a bounded UI label from a task or provider/model. */
static char *agent_derive_label(const AgcAgentSpec *spec, const char *model) {
    const char *src = spec->label                     ? spec->label
                      : (spec->task && spec->task[0]) ? spec->task
                      : (model && model[0])           ? model
                                                      : "agent";
    size_t n = agentc_strlen(src);
    if (n > AGENTC_ACTOR_LABEL_MAX)
        n = AGENTC_ACTOR_LABEL_MAX;
    char *out = agentc_alloc(n + 1);
    agentc_memcpy(out, src, n);
    out[n] = 0;
    return out;
}

AgcAgent *agentc_agent_create(const AgcAgentSpec *spec) {
    if (!spec)
        return NULL;
    AgcAgent *parent = spec->parent;

    /* ---- per-agent route seam + owner (P8-D1/B-M8) -------------------- */
    /* A root takes them from the spec; a child whose spec leaves the seam zeroed
     * inherits the parent's. Resolved before any use so route_resolve and the
     * ACTOR_STARTED persistence observer below see the binding. */
    AgcRouteSeam seam = spec->route;
    void *owner = spec->owner;
    if (parent != NULL) {
        if (seam.provider == NULL && seam.target == NULL)
            seam = parent->route;
        if (owner == NULL)
            owner = parent->owner;
    }

    /* ---- resolve provider/model/thinking/transport (R-D2) ------------- */
    /* An explicit spec->provider selects that provider and wins over the parent
     * (spec contract: NULL => inherit); the per-agent seam resolves it. */
    const AgcProvider *prov = spec->provider_handle;
    if (!prov && spec->provider)
        prov = seam.provider ? seam.provider(seam.provider_ud, spec->provider)
                             : resolve_provider_default(NULL, spec->provider);
    if (!prov && spec->provider)
        return NULL; /* unknown provider: no partial agent */
    if (!prov && parent)
        prov = parent->prov;

    const char *model = (spec->model && spec->model[0]) ? spec->model
                        : parent                        ? parent->model
                                                        : NULL;
    AgcAgent *a = agent_new_base(prov, model);
    a->route = seam;
    a->owner = owner;

    a->parent_id = parent ? parent->actor_id : 0;
    a->depth = parent ? (uint16_t)(parent->depth + 1) : 0;
    a->is_subagent = parent != NULL;
    a->owned_by_actor = parent != NULL;
    a->ephemeral = (spec->flags & AGENTC_AGENT_F_EPHEMERAL) != 0;
    a->background = (spec->flags & AGENTC_AGENT_F_BACKGROUND) != 0;

    /* thinking/max_tokens/credentials/transport: spec else parent else default */
    agentc_agent_set_thinking(a, spec->thinking >= 0 ? spec->thinking
                                 : parent            ? parent->thinking
                                                     : 0);
    agentc_agent_set_max_tokens(a, spec->max_tokens != 0 ? spec->max_tokens
                                   : parent              ? parent->max_tokens
                                                         : 0);
    a->token_budget = spec->token_budget != 0 ? spec->token_budget
                          : parent             ? parent->token_budget
                                               : 0;
    /* P8-D3 (B-M7): credentials inherit only on the same provider; a
     * cross-provider child resolves its own through the inherited route seam or
     * the spawn is refused (no partial agent, no parent-key leak). */
    bool same_prov = parent == NULL || prov == NULL || prov == parent->prov;
    if (spec->api_key)
        agentc_agent_set_api_key(a, spec->api_key);
    else if (same_prov && parent && parent->api_key)
        agentc_agent_set_api_key(a, parent->api_key);
    else if (!same_prov && prov != NULL) {
        AgcRouteTarget t;
        agentc_memset(&t, 0, sizeof t);
        bool got = a->route.target && a->route.target(a->route.target_ud, prov->name, &t);
        if (got && t.api_key && t.api_key[0])
            agentc_agent_set_api_key(a, t.api_key);
        if (got && t.base_url && t.base_url[0])
            agentc_agent_set_base_url(a, t.base_url);
        const AgcProviderOps *ops = agentc_provider_ops(prov);
        if ((!got || !t.api_key || !t.api_key[0]) && ops && ops->needs_key &&
            agentc_oauth_account_id(prov->name) == NULL) {
            agentc_logf(3, "agent: no credential for provider %s; spawn refused", prov->name);
            agent_destroy(a);
            return NULL;
        }
    }
    if (spec->base_url)
        agentc_agent_set_base_url(a, spec->base_url);
    else if (same_prov && parent && parent->base_url)
        agentc_agent_set_base_url(a, parent->base_url);
    if (spec->max_attempts > 0)
        agentc_agent_set_retry(a, spec->max_attempts);
    else if (parent)
        agentc_agent_set_retry(a, parent->max_attempts);
    bool insecure = spec->insecure || (parent && parent->insecure);
    agentc_agent_set_insecure(a, insecure);
    if (spec->transport_set)
        agentc_agent_set_transport(a, spec->transport);
    else if (parent)
        agentc_agent_set_transport(a, parent->transport);
    /* Root without an injected transport stays NULL: the app wires the HTTP
     * transport (mode_configure_agent), preserving the legacy agentc_agent_new
     * behavior and every existing golden. */

    /* ---- envelope: inherit or narrow (P6-TRUST §3) -------------------- */
    const AgcTrustEnv *env = NULL;
    if (spec->trust) {
        if (parent && parent->trust) {
            char err[160] = {0};
            AgcTrustEnv narrow;
            agentc_memset(&narrow, 0, sizeof narrow);
            if (agentc_trust_narrow(parent->trust, spec->trust, &narrow, err, sizeof err) != 0) {
                /* widening/denied: fail closed before any actor exists */
                agent_destroy(a);
                return NULL;
            }
            a->owned_env = agentc_alloc(sizeof *a->owned_env);
            *a->owned_env = narrow;
            env = a->owned_env;
        } else {
            /* root explicit, or a child of an unconfined parent: clone it so
             * the agent owns its envelope's roots/hosts. */
            a->owned_env = agentc_alloc(sizeof *a->owned_env);
            if (agentc_trust_env_clone(spec->trust, a->owned_env) != 0) {
                agent_destroy(a);
                return NULL;
            }
            env = a->owned_env;
        }
    } else if (parent) {
        env = parent->trust; /* borrow the parent's immutable envelope */
    }
    a->trust = env;

    /* ---- tools: explicit, else the parent's effective table ----------- */
    if (spec->tools && spec->ntools > 0) {
        agentc_agent_set_tools(a, spec->tools, spec->ntools);
    } else if (parent && parent->tools && parent->ntools > 0) {
        agentc_agent_set_tools(a, parent->tools, parent->ntools);
    }
    agent_filter_tools(a, env);
    agent_depth_filter(a);

    /* ---- system/observer/events --------------------------------------- */
    if (spec->system)
        agentc_agent_set_system(a, spec->system);
    if (spec->observer)
        agentc_agent_set_observer(a, spec->observer, spec->observer_ud);
    else if (parent)
        agentc_agent_set_observer(a, parent->observer, parent->observer_ud);
    if (spec->events)
        agentc_agent_set_events(a, spec->events, spec->events_ud);
    else if (parent)
        agentc_agent_set_events(a, parent->events, parent->events_ud);

    /* ---- bounded parent-context inheritance (P6-SUBAGENTS §7) --------- */
    if ((spec->flags & AGENTC_AGENT_F_INHERIT_CONTEXT) && parent) {
        const AgcTranscript *pt = &parent->tr;
        size_t start = pt->n > AGENTC_AGENT_CTX_KEEP ? pt->n - AGENTC_AGENT_CTX_KEEP : 0;
        AgcTranscript tail;
        agentc_transcript_init(&tail);
        tail.msgs = pt->msgs + start;
        tail.n = pt->n - start;
        tail.model = pt->model;
        tail.provider = pt->provider;
        (void)agentc_agent_load(a, &tail);
    }

    /* ---- register the actor row --------------------------------------- */
    char *label = agent_derive_label(spec, a->model);
    AgcActorId id = agentc_actor_register(AGENTC_ACTOR_AGENT, parent ? parent->actor_id : 0, label);
    agentc_free(label);
    if (id == 0) {
        agent_destroy(a);
        return NULL;
    }
    a->actor_id = id;
    AgcActor *row = agentc_actor_get(id);
    if (row != NULL) {
        row->agent = a;
        row->env = (struct AgcTrustEnv *)env;
        row->state = AGENTC_ACTOR_RUNNING;
        row->steerable = true;
        row->idle_ms = AGENTC_AGENT_IDLE_DEF_MS;
    }

    /* ---- started/trust audit ------------------------------------------ */
    if (row != NULL) {
        AgcBuf j = {0};
        bool override = (spec->model && spec->model[0]) || spec->provider || spec->thinking >= 0 ||
                        spec->max_tokens != 0;
        agentc_buf_printf(&j,
                          "{\"agent_id\":%u,\"parent_id\":%u,\"depth\":%u,"
                          "\"provider\":\"%s\",\"model\":\"%s\","
                          "\"is_subagent\":%s,\"override\":%s}",
                          (unsigned)id, (unsigned)a->parent_id, (unsigned)a->depth,
                          a->prov ? a->prov->name : "", a->model ? a->model : "",
                          a->is_subagent ? "true" : "false", override ? "true" : "false");
        actor_event(row, AGENTC_EV_ACTOR_STARTED, spec->task, (const char *)j.p, 0);
        agentc_buf_free(&j);
    }
    if (env != NULL) {
        AgcBuf tj = {0};
        (void)agentc_trust_to_json(env, &tj);
        AgcTrustEvent te;
        agentc_memset(&te, 0, sizeof te);
        te.actor_id = id;
        te.effective_json = tj.p ? (const char *)tj.p : "{}";
        te.reason = spec->trust ? "explicit" : "inherit";
        agentc_events_emit(NULL, AGENTC_EV_TRUST_SET, &te);
        agentc_buf_free(&tj);
    }

    /* ---- initial task -------------------------------------------------- */
    if (spec->task && spec->task[0]) {
        size_t tn = agentc_strlen(spec->task);
        if (tn > AGENTC_AGENT_TASK_MAX)
            tn = AGENTC_AGENT_TASK_MAX;
        a->task = agentc_alloc(tn + 1);
        agentc_memcpy(a->task, spec->task, tn);
        a->task[tn] = 0;
        if (a->background) {
            a->running = true;
            a->pending_input = agentc_strdup(a->task);
        } else {
            int rc = agentc_agent_submit(a, a->task);
            (void)agent_priv_capture_result(a, rc == 0 ? 0 : 1);
            if (row != NULL)
                actor_finish_state(row, rc == 0 ? AGENTC_ACTOR_SETTLED : AGENTC_ACTOR_FAILED, NULL);
        }
    }
    return a;
}

AgcAgent *agentc_agent_new(const AgcProvider *prov, const char *model) {
    AgcAgentSpec s;
    agentc_memset(&s, 0, sizeof s);
    s.provider_handle = prov;
    s.provider = prov ? prov->name : NULL;
    s.model = model;
    s.thinking = -1;
    s.flags = AGENTC_AGENT_F_WAIT;
    return agentc_agent_create(&s);
}

void agentc_agent_free(AgcAgent *a) {
    if (!a)
        return;
    bool owned = a->owned_by_actor;
    if (a->actor_id != 0) {
        /* Releasing the actor clears its queue, which emits queue events. The
         * agent's own event callback may close over a front-end frame that is
         * already gone during teardown, so detach it first. A child (owned by
         * its row) is freed by agent_actor_release during the recursion, so it
         * must not be destroyed twice here. */
        a->events = NULL;
        a->events_ud = NULL;
        AgcActorId id = a->actor_id;
        a->actor_id = 0;
        agentc_actor_release(id);
    }
    if (!owned)
        agent_destroy(a);
}

const AgcProvider *agentc_agent_provider(const AgcAgent *a) {
    return a ? a->prov : NULL;
}
const char *agentc_agent_model(const AgcAgent *a) {
    return a ? a->model : NULL;
}
int agentc_agent_depth(const AgcAgent *a) {
    return a ? (int)a->depth : 0;
}
AgcActorId agentc_agent_current(void) {
    return g_current_agent ? g_current_agent->actor_id : 0;
}

const char *agent_priv_model(const AgcAgent *a) {
    return a ? a->model : NULL;
}
const char *agent_priv_provider(const AgcAgent *a) {
    return (a && a->prov) ? a->prov->name : NULL;
}
const char *agent_priv_task(const AgcAgent *a) {
    return a ? a->task : NULL;
}
const char *agent_priv_result(const AgcAgent *a) {
    return a ? a->result : NULL;
}
bool agent_priv_is_subagent(const AgcAgent *a) {
    return a ? a->is_subagent : false;
}
uint16_t agent_priv_depth(const AgcAgent *a) {
    return a ? a->depth : 0;
}
bool agent_priv_ephemeral(const AgcAgent *a) {
    return a ? a->ephemeral : false;
}
int64_t agent_priv_turn_no(const AgcAgent *a) {
    return agentc_agent_turn_no(a);
}
bool agent_priv_steerable(const AgcAgent *a, const char **kind) {
    return agentc_agent_steerable(a, kind);
}

/* An api_key is written into a request header; a CR/LF from a hostile env,
 * config, flag or OAuth token would let the value inject another header. Store
 * a copy with every C0 control byte removed (normal bytes untouched). */
static char *header_value_sanitize(const char *s) {
    if (!s)
        return NULL;
    size_t n = agentc_strlen(s);
    char *out = agentc_alloc(n + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20)
            continue;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return out;
}

/* The setters duplicate the new value before freeing the old one, so passing
 * the agent's own current pointer (a caller that read it back first) is safe. */
void agentc_agent_set_api_key(AgcAgent *a, const char *key) {
    if (!a)
        return;
    char *copy = header_value_sanitize(key);
    agentc_free(a->api_key);
    a->api_key = copy;
}

void agentc_agent_set_base_url(AgcAgent *a, const char *url) {
    if (!a)
        return;
    char *copy = url ? agentc_strdup(url) : NULL;
    agentc_free(a->base_url);
    a->base_url = copy;
}

/* Explicit system prompt; NULL restores the auto/build mode. The effective
 * prompt is recomputed per turn by refresh_system() below. */
void agentc_agent_set_system(AgcAgent *a, const char *text) {
    if (!a)
        return;
    char *copy = text ? agentc_strdup(text) : NULL;
    agentc_free(a->system);
    a->system = copy;
}

void agentc_agent_set_transport(AgcAgent *a, AgcTransport t) {
    if (!a)
        return;
    a->transport = t;
}

AgcTransport agentc_agent_transport(const AgcAgent *a) {
    AgcTransport empty;
    agentc_memset(&empty, 0, sizeof empty);
    return a ? a->transport : empty;
}

/* P8-D1 (B-M8): the per-agent route seam and persistence owner. Both are
 * borrowed; the mode layer installs them on the root before create and the
 * constructor inherits them into children. */
void agentc_agent_set_route_seam(AgcAgent *a, const AgcRouteSeam *seam) {
    if (!a)
        return;
    if (seam)
        a->route = *seam;
    else
        agentc_memset(&a->route, 0, sizeof a->route);
}

AgcRouteSeam agentc_agent_route_seam(const AgcAgent *a) {
    AgcRouteSeam empty;
    agentc_memset(&empty, 0, sizeof empty);
    return a ? a->route : empty;
}

void agentc_agent_set_owner(AgcAgent *a, void *owner) {
    if (!a)
        return;
    a->owner = owner;
}

void *agentc_agent_owner(const AgcAgent *a) {
    return a ? a->owner : NULL;
}

void agentc_agent_set_tool_veto(AgcAgent *a, AgcToolVetoFn cb, void *ud) {
    a->veto = cb;
    a->veto_ud = ud;
}

void agentc_agent_set_trust(AgcAgent *a, const AgcTrustEnv *env) {
    if (!a)
        return;
    a->trust = env;
    AgcActor *row = a->actor_id != 0 ? agentc_actor_get(a->actor_id) : NULL;
    if (row != NULL)
        row->env = (struct AgcTrustEnv *)env;
}

const AgcTrustEnv *agentc_agent_trust(const AgcAgent *a) {
    return a ? a->trust : NULL;
}

void agentc_agent_set_observer(AgcAgent *a, AgcMsgObserver cb, void *ud) {
    a->observer = cb;
    a->observer_ud = ud;
}

void agentc_agent_set_recompose(AgcAgent *a, AgcRecomposeFn fn, void *ud) {
    a->recompose = fn;
    a->recompose_ud = ud;
}

/* Applied at safe points only: after a tool batch has returned, or just before a
 * terminal agent_end. The callback re-reads the registry and no-ops when clean. */
static void recompose_now(AgcAgent *a) {
    if (a && a->recompose)
        a->recompose(a->recompose_ud, a);
}

/* ------------------------------------------------ direct hook dispatch
 *
 * The registry is the extension bus and result-consuming points are
 * dispatched from core directly with agentc_ext_emit. Observe points that have
 * a typed front-end event flow through agentc_events_emit to the registry's
 * subscriber (agentc_ext_emit_agent_event), so they stay quiet-gated. Observe
 * points with no typed event (provider_stream_event, after_provider_response)
 * build their payload here and emit directly behind want_observe(). Direct
 * overrides are never gated by agentc_events_quiet. */

/* Never quiet-gated: an override point must dispatch even when a front end
 * suppressed its typed stream. */
static bool want_override(const char *p) {
    return agentc_ext_wants(p);
}

/* Observe point with no typed event: quiet still suppresses delivery, exactly
 * like a quiet-gated typed event. */
static bool want_observe(const char *p) {
    return !agentc_events_quiet() && agentc_ext_wants(p);
}

/* B8 fold for the four provider interception points (completion pass 2g).
 * `observe` mirrors the old want_observe() gate: a quiet front end suppresses
 * the legacy observe dispatch (a new core `provider` handler still runs). An
 * override sub-kind is never quiet-gated. The transferred result lands on *m
 * and is freed by the caller with agentc_hook_result_free. */
static bool provider_watched(const char *point, bool observe) {
    if (agentc_hook_wants(AGENTC_HOOK_STAGE_PROVIDER))
        return true;
    return observe ? want_observe(point) : want_override(point);
}

static int provider_hook(AgcAgent *a, const char *point, const char *sub, const AgcBuf *payload,
                         bool observe, AgcMessage *m) {
    agentc_memset(m, 0, sizeof *m);
    m->class = AGENTC_MSG_CLASS_HOOK;
    m->kind = AGENTC_MSG_INPUT;
    m->actor_id = a ? a->actor_id : 0;
    m->meta = sub;
    m->body = payload && payload->p ? (const char *)payload->p : "{}";
    m->body_len = payload && payload->p ? payload->len : 2;
    m->id = agentc_hook_next_id();
    const char *lpoint = (observe && agentc_events_quiet()) ? NULL : point;
    AgcMessage *out = NULL;
    return agentc_hook_run_legacy(AGENTC_HOOK_STAGE_PROVIDER, lpoint, m, m->body, &out);
}

static const char *stop_name(int stop) {
    switch (stop) {
    case AGENTC_STOP_STOP:
        return "stop";
    case AGENTC_STOP_LENGTH:
        return "length";
    case AGENTC_STOP_TOOLUSE:
        return "tool_use";
    case AGENTC_STOP_ERROR:
        return "error";
    case AGENTC_STOP_ABORTED:
        return "aborted";
    default:
        return "pending";
    }
}

/* Numeric thinking level -> event name. The CLI parsers produce only 0/1/3/4;
 * a config-provided numeric 2 normalizes to "low" so every 0..4 value has a
 * stable, printable name. */
static const char *thinking_name(int level) {
    switch (level) {
    case 0:
        return "off";
    case 1:
    case 2:
        return "low";
    case 3:
        return "medium";
    default:
        return "high";
    }
}

/* Effective result of a `fields` point: the accumulated patch merged over the
 * payload. Returns NULL when the hook returned no patch. */
static char *hook_effective(const AgcBuf *payload, const char *result_json) {
    if (!result_json)
        return NULL;
    const char *base = (payload && payload->p) ? (const char *)payload->p : "{}";
    return agentc_ext_merge_fields(base, result_json);
}

/* Append the `entries` array of a turn_end / agent_before_settle result through
 * the host entry sink. Returns the result's `continue` flag. Malformed entries
 * are logged and skipped. */
static bool hook_apply_entries(const char *eff_json) {
    if (!eff_json)
        return false;
    AgcJsonArena *arena = agentc_json_arena_new(0);
    AgcJson *o = agentc_json_parse_in(arena, eff_json, agentc_strlen(eff_json));
    const AgcJson *entries = agentc_json_get(o, "entries");
    if (agentc_json_type(entries) == AGENTC_JSON_ARR) {
        size_t n = agentc_json_len(entries);
        for (size_t i = 0; i < n; i++) {
            const AgcJson *e = agentc_json_at(entries, i);
            const char *type = agentc_json_get_str(e, "custom_type");
            const AgcJson *data = agentc_json_get(e, "data");
            if (!type || agentc_json_type(data) != AGENTC_JSON_OBJ) {
                agentc_logf(2, "ext: entries[%llu] is malformed, ignored", (unsigned long long)i);
                continue;
            }
            AgcBuf dj = {0};
            AgcJsonW w;
            agentc_jsonw_init(&w, &dj);
            agentc_json_emit(&w, data);
            agentc_ext_append_entry(type, (const char *)dj.p);
            agentc_buf_free(&dj);
        }
    } else if (entries) {
        agentc_logf(2, "ext: entries is not an array, ignored");
    }
    bool cont = agentc_json_get_bool(o, "continue", false);
    agentc_json_arena_free(arena);
    return cont;
}

#define AGENT_CONTINUATION_MAX 32

/* One shared cap for turn_end.continue and agent_before_settle.continue. */
static bool continuation_take(int *count) {
    if (*count >= AGENT_CONTINUATION_MAX) {
        agentc_logf(2, "continuation limit reached");
        return false;
    }
    (*count)++;
    return true;
}

/* input override: returns 1 when the prompt was handled (no run), 0 to run.
 * A transform is stored in *out (owned). */
static int input_hook(AgcAgent *a, const char *text, char **out) {
    *out = NULL;
    if (!want_override("input") && !agentc_hook_wants(AGENTC_HOOK_STAGE_INPUT))
        return 0;
    /* 0 = run, 1 = handled (deliberate quiet no-run), negative = blocked
     * (fail-closed: the caller reports last_error and returns that errno). */
    AgcBuf p = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &p);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "text");
    agentc_jsonw_cstr(&w, text ? text : "");
    agentc_jsonw_key(&w, "source");
    agentc_jsonw_cstr(&w, "user");
    agentc_jsonw_end(&w);
    /* B8: one dispatch runs the `input` stage and, unless a core handler
     * consumed it, the mapped legacy `input` point exactly once. The legacy
     * result is interpreted below exactly as the former direct emit. */
    AgcMessage m;
    agentc_memset(&m, 0, sizeof m);
    m.class = AGENTC_MSG_CLASS_HOOK;
    m.kind = AGENTC_MSG_INPUT;
    m.actor_id = a ? a->actor_id : 0;
    m.meta = "user";
    m.body = text ? text : "";
    m.body_len = text ? agentc_strlen(text) : 0;
    m.id = agentc_hook_next_id();
    AgcMessage *hout = NULL;
    int hrc =
        agentc_hook_run_legacy(AGENTC_HOOK_STAGE_INPUT, "input", &m, (const char *)p.p, &hout);
    (void)hout;
    int handled = 0;
    if (m.consumed) {
        handled = 1; /* a core handler consumed the prompt: quiet no-run */
    } else if (hrc < 0) {
        handled = -5; /* EIO: the fail-closed stage block is a visible failure */
    } else if (m.blocked) {
        agentc_logf(2, "ext: input blocked, no run");
        handled = -5;
    } else if (m.result_json) {
        char *eff = hook_effective(&p, m.result_json);
        if (eff) {
            AgcJsonArena *arena = agentc_json_arena_new(0);
            AgcJson *o = agentc_json_parse_in(arena, eff, agentc_strlen(eff));
            const char *action = agentc_json_get_str(o, "action");
            if (action && agentc_streq(action, "handled")) {
                handled = 1;
            } else if (action && agentc_streq(action, "transform")) {
                const char *nt = agentc_json_get_str(o, "text");
                if (nt)
                    *out = agentc_strdup(nt);
                else
                    agentc_logf(2, "ext: input transform without text, ignored");
            } else if (action && !agentc_streq(action, "continue")) {
                agentc_logf(2, "ext: input action '%s' ignored", action);
            }
            agentc_json_arena_free(arena);
            agentc_free(eff);
        }
    }
    agentc_hook_result_free(&m);
    agentc_buf_free(&p);
    return handled;
}

/* Effective system prompt for the next request: per-run before_agent_start
 * override > explicit --system > live built prompt. The built prompt is rebuilt
 * from the current tool table (auto mode), so a turn-boundary recompose is
 * reflected in the next provider request; the override and the explicit prompt
 * stay stable for the whole run. The effective prompt is published to the
 * extension context so host->system_prompt() agrees with the next request. */
static void refresh_system(AgcAgent *a) {
    const char *effective;
    if (a->system_override) {
        if (!a->tr.system || !agentc_streq(a->tr.system, a->system_override)) {
            agentc_free((void *)a->tr.system);
            a->tr.system = agentc_strdup(a->system_override);
        }
        effective = a->tr.system;
    } else if (a->system && a->system[0]) {
        if (!a->tr.system || !agentc_streq(a->tr.system, a->system)) {
            agentc_free((void *)a->tr.system);
            a->tr.system = agentc_strdup(a->system);
        }
        effective = a->tr.system;
    } else {
        /* Auto: the tool table may have changed at the last turn boundary. */
        agentc_free((void *)a->tr.system);
        a->tr.system = agentc_prompt_build(a->tools, a->ntools);
        effective = a->tr.system;
    }
    AgcExtContext ctx = {0};
    ctx.system_prompt = effective ? effective : "";
    agentc_ext_set_context(&ctx);
    agentc_hook_stage_emit("prompt", "system", effective, a->actor_id);
}

/* before_agent_start override: a systemPrompt replaces the built prompt for the
 * rest of the run (stored per-run so the turn refresh cannot clobber it) and is
 * re-published to the extension context; message is logged and ignored. */
static void before_agent_start_hook(AgcAgent *a, const char *prompt) {
    if (!want_override("before_agent_start"))
        return;
    AgcBuf p = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &p);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "prompt");
    agentc_jsonw_cstr(&w, prompt ? prompt : "");
    agentc_jsonw_key(&w, "system_prompt");
    agentc_jsonw_cstr(&w, a->tr.system ? a->tr.system : "");
    agentc_jsonw_end(&w);
    AgcExtResult r = agentc_ext_emit("before_agent_start", (const char *)p.p);
    if (r.blocked) {
        agentc_logf(2, "ext: before_agent_start blocked, keeping the built prompt");
    } else if (r.result_json) {
        char *eff = hook_effective(&p, r.result_json);
        if (eff) {
            AgcJsonArena *arena = agentc_json_arena_new(0);
            AgcJson *o = agentc_json_parse_in(arena, eff, agentc_strlen(eff));
            const char *sp = agentc_json_get_str(o, "systemPrompt");
            if (sp) {
                agentc_free(a->system_override);
                a->system_override = agentc_strdup(sp);
                agentc_free((void *)a->tr.system);
                a->tr.system = agentc_strdup(sp);
                AgcExtContext ctx = {0};
                ctx.system_prompt = a->tr.system;
                agentc_ext_set_context(&ctx);
            }
            const char *message = agentc_json_get_str(o, "message");
            if (message && message[0])
                agentc_logf(1, "ext: before_agent_start: %s", message);
            agentc_json_arena_free(arena);
            agentc_free(eff);
        }
    }
    agentc_free(r.result_json);
    agentc_buf_free(&p);
}

/* Validate the restricted message_end `content` array before mutating the
 * message: an invalid shape must leave the original blocks untouched. A
 * tool_call needs a non-empty string id and name; `arguments` is optional but
 * must be a string, object or null (never a scalar). */
static bool message_content_valid(const AgcJson *content) {
    if (agentc_json_type(content) != AGENTC_JSON_ARR)
        return false;
    size_t n = agentc_json_len(content);
    for (size_t i = 0; i < n; i++) {
        const AgcJson *b = agentc_json_at(content, i);
        if (agentc_json_type(b) != AGENTC_JSON_OBJ)
            return false;
        const char *t = agentc_json_get_str(b, "type");
        if (!t)
            return false;
        if (agentc_streq(t, "text") || agentc_streq(t, "thinking")) {
            const AgcJson *txt = agentc_json_get(b, "text");
            int ty = agentc_json_type(txt);
            if (txt && ty != AGENTC_JSON_STR && ty != AGENTC_JSON_NULL)
                return false;
        } else if (agentc_streq(t, "tool_call")) {
            const char *id = agentc_json_get_str(b, "id");
            const char *name = agentc_json_get_str(b, "name");
            if (!id || !id[0] || !name || !name[0])
                return false;
            const AgcJson *args = agentc_json_get(b, "arguments");
            if (args) {
                int at = agentc_json_type(args);
                if (at != AGENTC_JSON_STR && at != AGENTC_JSON_OBJ && at != AGENTC_JSON_NULL)
                    return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

/* Rebuild the assistant message from a restricted message_end object. role must
 * be absent or assistant; content is required. usage/ts are never replaceable.
 * With preserve_terminal (the failed/aborted path) content/blocks may still be
 * replaced but stop_reason/error keep the terminal failure: a hook must not be
 * able to flip a failed turn into a serializable success. */
static void apply_message_patch(AgcMsg *m, const AgcJson *msg, bool preserve_terminal) {
    const char *role = agentc_json_get_str(msg, "role");
    if (role && !agentc_streq(role, "assistant")) {
        agentc_logf(2, "ext: message_end role '%s' does not match assistant, ignored", role);
        return;
    }
    const AgcJson *content = agentc_json_get(msg, "content");
    if (!message_content_valid(content)) {
        agentc_logf(2, "ext: message_end content is not a valid block array, ignored");
        return;
    }
    agentc_msg_clear_blocks(m);
    size_t n = agentc_json_len(content);
    for (size_t i = 0; i < n; i++) {
        const AgcJson *b = agentc_json_at(content, i);
        const char *t = agentc_json_get_str(b, "type");
        if (agentc_streq(t, "text") || agentc_streq(t, "thinking")) {
            const char *txt = agentc_json_get_str(b, "text");
            AgcBlock *blk = agentc_msg_block_new(m, agentc_streq(t, "text") ? AGENTC_BLK_TEXT
                                                                            : AGENTC_BLK_THINK);
            if (txt)
                agentc_msg_block_append(blk, txt, agentc_strlen(txt));
            continue;
        }
        const char *id = agentc_json_get_str(b, "id");
        const char *name = agentc_json_get_str(b, "name");
        AgcBlock *blk = agentc_msg_block_new(m, AGENTC_BLK_TOOLCALL);
        blk->tool_id = agentc_strdup(id ? id : "");
        blk->tool_name = agentc_strdup(name ? name : "");
        const AgcJson *args = agentc_json_get(b, "arguments");
        if (agentc_json_type(args) == AGENTC_JSON_STR) {
            size_t alen = 0;
            const char *as = agentc_json_str(args, &alen);
            blk->tool_args = agentc_strdup_len(as ? as : "", as ? alen : 0);
        } else if (args && agentc_json_type(args) != AGENTC_JSON_NULL) {
            AgcBuf ab = {0};
            AgcJsonW aw;
            agentc_jsonw_init(&aw, &ab);
            agentc_json_emit(&aw, args);
            blk->tool_args = agentc_strdup((const char *)ab.p);
            agentc_buf_free(&ab);
        } else {
            blk->tool_args = agentc_strdup("{}");
        }
    }
    if (preserve_terminal)
        return;
    const AgcJson *sr = agentc_json_get(msg, "stop_reason");
    if (sr) {
        const char *s = agentc_json_get_str(msg, "stop_reason");
        if (s && agentc_streq(s, "stop"))
            m->stop_reason = AGENTC_STOP_STOP;
        else if (s && agentc_streq(s, "length"))
            m->stop_reason = AGENTC_STOP_LENGTH;
        else if (s && agentc_streq(s, "tool_use"))
            m->stop_reason = AGENTC_STOP_TOOLUSE;
        else if (s && agentc_streq(s, "error"))
            m->stop_reason = AGENTC_STOP_ERROR;
        else if (s && agentc_streq(s, "aborted"))
            m->stop_reason = AGENTC_STOP_ABORTED;
        else if (s && agentc_streq(s, "pending"))
            m->stop_reason = AGENTC_STOP_PENDING;
        else
            agentc_logf(0, "ext: message_end stop_reason ignored");
    }
    const AgcJson *er = agentc_json_get(msg, "error");
    if (er) {
        int ty = agentc_json_type(er);
        if (ty == AGENTC_JSON_NULL) {
            agentc_free(m->error);
            m->error = NULL;
        } else if (ty == AGENTC_JSON_STR) {
            const char *es = agentc_json_get_str(msg, "error");
            agentc_free(m->error);
            m->error = agentc_strdup(es ? es : "");
        } else {
            agentc_logf(0, "ext: message_end error is not a string/null, ignored");
        }
    }
}

/* message_end override: dispatched directly before the typed MSG_END and the
 * observer so a replacement is not silently dropped. preserve_terminal is set
 * for a failed/aborted turn (see apply_message_patch). */
static void message_end_hook(AgcMsg *m, bool preserve_terminal) {
    if (!want_override("message_end") && !agentc_hook_wants(AGENTC_HOOK_STAGE_MESSAGE))
        return;
    AgcBuf p = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &p);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "role");
    agentc_jsonw_cstr(&w, "assistant");
    agentc_jsonw_key(&w, "stop_reason");
    agentc_jsonw_cstr(&w, stop_name(m->stop_reason));
    agentc_jsonw_key(&w, "usage");
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "input");
    agentc_jsonw_u64(&w, m->usage.input);
    agentc_jsonw_key(&w, "output");
    agentc_jsonw_u64(&w, m->usage.output);
    agentc_jsonw_key(&w, "cache_read");
    agentc_jsonw_u64(&w, m->usage.cache_read);
    agentc_jsonw_key(&w, "cache_write");
    agentc_jsonw_u64(&w, m->usage.cache_write);
    agentc_jsonw_key(&w, "reasoning");
    agentc_jsonw_u64(&w, m->usage.reasoning);
    agentc_jsonw_key(&w, "cost_micro");
    agentc_jsonw_i64(&w, m->usage.cost_micro);
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    /* B8: the `message` stage and the mapped legacy `message_end` point share
     * one dispatch; the legacy result drives the patch exactly as before. */
    AgcMessage hm;
    agentc_memset(&hm, 0, sizeof hm);
    hm.class = AGENTC_MSG_CLASS_HOOK;
    hm.kind = AGENTC_MSG_INPUT;
    hm.actor_id = g_current_agent ? g_current_agent->actor_id : 0;
    hm.meta = "assistant";
    hm.body = (const char *)p.p;
    hm.body_len = p.len;
    hm.id = agentc_hook_next_id();
    AgcMessage *hout = NULL;
    int hrc = agentc_hook_run_legacy(AGENTC_HOOK_STAGE_MESSAGE, "message_end", &hm,
                                     (const char *)p.p, &hout);
    (void)hout;
    if (hrc < 0) {
        agentc_logf(2, "ext: message_end blocked, keeping the original message");
    } else if (hm.blocked) {
        agentc_logf(2, "ext: message_end blocked, keeping the original message");
    } else if (hm.result_json) {
        char *eff = hook_effective(&p, hm.result_json);
        if (eff) {
            AgcJsonArena *arena = agentc_json_arena_new(0);
            AgcJson *o = agentc_json_parse_in(arena, eff, agentc_strlen(eff));
            const AgcJson *msg = agentc_json_get(o, "message");
            if (agentc_json_type(msg) == AGENTC_JSON_OBJ)
                apply_message_patch(m, msg, preserve_terminal);
            agentc_json_arena_free(arena);
            agentc_free(eff);
        }
    }
    agentc_hook_result_free(&hm);
    agentc_buf_free(&p);
}

/* tool_result override: dispatched directly before the transcript append and
 * the observer. Returns an owned replacement text (or NULL) and updates the
 * is_error flag. */
static char *tool_result_hook(const char *call_id, const char *tool_name, const char *res,
                              bool *is_error) {
    char *replacement = NULL;
    if (!want_override("tool_result") && !agentc_hook_wants(AGENTC_HOOK_STAGE_TOOL))
        return NULL;
    AgcBuf p = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &p);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "tool_call_id");
    agentc_jsonw_cstr(&w, call_id ? call_id : "");
    agentc_jsonw_key(&w, "tool_name");
    agentc_jsonw_cstr(&w, tool_name ? tool_name : "");
    agentc_jsonw_key(&w, "content");
    agentc_jsonw_arr(&w);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "type");
    agentc_jsonw_cstr(&w, "text");
    agentc_jsonw_key(&w, "text");
    agentc_jsonw_str(&w, res ? res : "", res ? agentc_strlen(res) : 0);
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "is_error");
    agentc_jsonw_bool(&w, *is_error);
    agentc_jsonw_end(&w);
    /* B8: the `tool` result stage and the legacy `tool_result` point share one
     * dispatch; only the legacy-shaped patch is interpreted below. */
    AgcMessage hm;
    agentc_memset(&hm, 0, sizeof hm);
    hm.class = AGENTC_MSG_CLASS_HOOK;
    hm.kind = AGENTC_MSG_INPUT;
    hm.actor_id = g_current_agent ? g_current_agent->actor_id : 0;
    hm.meta = "result";
    hm.body = (const char *)p.p;
    hm.body_len = p.len;
    hm.id = agentc_hook_next_id();
    AgcMessage *hout = NULL;
    int hrc = agentc_hook_run_legacy(AGENTC_HOOK_STAGE_TOOL, "tool_result", &hm, (const char *)p.p,
                                     &hout);
    (void)hout;
    if (hrc < 0) {
        agentc_logf(2, "ext: tool_result blocked, keeping the raw result");
    } else if (hm.blocked) {
        agentc_logf(2, "ext: tool_result blocked, keeping the raw result");
    } else if (hm.result_json) {
        const char *result_json = hm.result_json;
        /* A `null` in a fields patch deletes the key, so the patch is inspected
         * to distinguish "content": null (empty result) from an absent key. */
        bool content_null = false;
        {
            AgcJsonArena *pa = agentc_json_arena_new(0);
            AgcJson *po = agentc_json_parse_in(pa, result_json, agentc_strlen(result_json));
            const AgcJson *pc = agentc_json_get(po, "content");
            content_null = pc && agentc_json_type(pc) == AGENTC_JSON_NULL;
            agentc_json_arena_free(pa);
        }
        char *eff = hook_effective(&p, result_json);
        if (eff) {
            AgcJsonArena *arena = agentc_json_arena_new(0);
            AgcJson *o = agentc_json_parse_in(arena, eff, agentc_strlen(eff));
            const AgcJson *c = agentc_json_get(o, "content");
            int ty = agentc_json_type(c);
            if (content_null) {
                replacement = agentc_strdup("");
            } else if (ty == AGENTC_JSON_STR) {
                size_t n = 0;
                const char *s = agentc_json_str(c, &n);
                replacement = agentc_strdup_len(s ? s : "", s ? n : 0);
            } else if (ty == AGENTC_JSON_NULL) {
                replacement = agentc_strdup("");
            } else if (ty == AGENTC_JSON_ARR) {
                AgcBuf j = {0};
                bool ok = true;
                size_t n = agentc_json_len(c);
                for (size_t i = 0; i < n; i++) {
                    const AgcJson *it = agentc_json_at(c, i);
                    const char *it_type = agentc_json_get_str(it, "type");
                    const char *it_text = agentc_json_get_str(it, "text");
                    if (!it_type || !agentc_streq(it_type, "text") || !it_text) {
                        ok = false;
                        break;
                    }
                    if (i > 0)
                        agentc_buf_byte(&j, '\n');
                    agentc_buf_push(&j, it_text, agentc_strlen(it_text));
                }
                if (ok) {
                    agentc_buf_byte(&j, 0);
                    replacement = (char *)j.p;
                    j.p = NULL;
                    agentc_buf_free(&j);
                } else {
                    agentc_buf_free(&j);
                    agentc_logf(2, "ext: tool_result content array malformed, ignored");
                }
            } else if (c) {
                agentc_logf(2, "ext: tool_result content shape ignored");
            }
            if (agentc_json_get(o, "is_error"))
                *is_error = agentc_json_get_bool(o, "is_error", *is_error);
            agentc_json_arena_free(arena);
            agentc_free(eff);
        }
    }
    agentc_hook_result_free(&hm);
    agentc_buf_free(&p);
    return replacement;
}

/* step_end override: entries always apply, `continue` only on a non-hard step.
 * Returns the requested continuation. Increments the pi-style step index. */
static bool step_end_hook(AgcAgent *a, int stop_reason, bool hard) {
    bool want_continue = false;
    if (want_override("step_end") || agentc_hook_wants(AGENTC_HOOK_STAGE_STEP)) {
        AgcBuf p = {0};
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "step_index");
        agentc_jsonw_i64(&w, a->step_index);
        agentc_jsonw_key(&w, "stop_reason");
        agentc_jsonw_cstr(&w, stop_name(stop_reason));
        agentc_jsonw_end(&w);
        /* B8: the `step` end stage and the legacy `step_end` point share one
         * dispatch; only a non-hard continuation is honored. */
        AgcMessage hm;
        agentc_memset(&hm, 0, sizeof hm);
        hm.class = AGENTC_MSG_CLASS_HOOK;
        hm.kind = AGENTC_MSG_INPUT;
        hm.actor_id = a->actor_id;
        hm.meta = "end";
        hm.body = (const char *)p.p;
        hm.body_len = p.len;
        hm.id = agentc_hook_next_id();
        AgcMessage *hout = NULL;
        int hrc = agentc_hook_run_legacy(AGENTC_HOOK_STAGE_STEP, "step_end", &hm, (const char *)p.p,
                                         &hout);
        (void)hout;
        if (hrc < 0) {
            agentc_logf(2, "ext: step_end blocked, keeping the original step");
        } else if (hm.blocked) {
            agentc_logf(2, "ext: step_end blocked, keeping the original step");
        } else if (hm.result_json) {
            char *eff = hook_effective(&p, hm.result_json);
            if (eff) {
                want_continue = hook_apply_entries(eff) && !hard;
                agentc_free(eff);
            }
        }
        agentc_hook_result_free(&hm);
        agentc_buf_free(&p);
    }
    a->step_index++;
    return want_continue;
}

/* agent_before_settle override: entries apply, `continue` is the caller's to
 * honor (rc == 0 and no abort). A blocked result settles normally. */
static bool settle_hook(const char *outcome) {
    bool want_continue = false;
    if (want_override("agent_before_settle") || agentc_hook_wants(AGENTC_HOOK_STAGE_SETTLE)) {
        AgcBuf p = {0};
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "outcome");
        agentc_jsonw_cstr(&w, outcome ? outcome : "completed");
        agentc_jsonw_end(&w);
        /* B8: the `settle` stage and the legacy `agent_before_settle` point
         * share one dispatch; entries apply and a blocked result settles
         * normally. */
        AgcMessage hm;
        agentc_memset(&hm, 0, sizeof hm);
        hm.class = AGENTC_MSG_CLASS_HOOK;
        hm.kind = AGENTC_MSG_INPUT;
        hm.meta = "settle";
        hm.body = (const char *)p.p;
        hm.body_len = p.len;
        hm.id = agentc_hook_next_id();
        AgcMessage *hout = NULL;
        int hrc = agentc_hook_run_legacy(AGENTC_HOOK_STAGE_SETTLE, "agent_before_settle", &hm,
                                         (const char *)p.p, &hout);
        (void)hout;
        if (hrc < 0 || hm.blocked) {
            agentc_logf(2, "ext: agent_before_settle blocked, settling normally");
        } else if (hm.result_json) {
            char *eff = hook_effective(&p, hm.result_json);
            if (eff) {
                want_continue = hook_apply_entries(eff);
                agentc_free(eff);
            }
        }
        agentc_hook_result_free(&hm);
        agentc_buf_free(&p);
    }
    return want_continue;
}

int agentc_agent_set_model(AgcAgent *a, const char *model) {
    if (!a || !model || !model[0])
        return -22;
    if (a->model && agentc_streq(a->model, model))
        return 0; /* no change, no emit */
    /* The transcript borrows the old model string, so snapshot it for the
     * payload before the free. */
    char *previous = agentc_strdup(a->model ? a->model : "");
    agentc_free(a->model);
    a->model = agentc_strdup(model);
    a->tr.model = a->model; /* the transcript borrows a->model; keep it live */
    /* P8-D2 (B-M9): re-evaluate the alias fact on every selection. */
    a->model_is_alias = agentc_model_find_virtual(a->model) != NULL;
    if (want_observe("model_select")) {
        AgcBuf p = {0};
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "provider");
        agentc_jsonw_cstr(&w, a->tr.provider ? a->tr.provider : (a->prov ? a->prov->name : ""));
        agentc_jsonw_key(&w, "model");
        agentc_jsonw_cstr(&w, model);
        agentc_jsonw_key(&w, "previous");
        agentc_jsonw_cstr(&w, previous ? previous : "");
        agentc_jsonw_end(&w);
        AgcExtResult r = agentc_ext_emit("model_select", (const char *)p.p);
        agentc_free(r.result_json);
        agentc_buf_free(&p);
    }
    agentc_free(previous);
    return 0;
}

void agentc_agent_set_tools(AgcAgent *a, const AgcTool *tools, size_t n) {
    if (!a)
        return;
    /* Copy before freeing: the caller may hand back a pointer into the table we
     * are replacing (e.g. a rebuild that re-reads the agent's own tools). */
    AgcTool *copy = agent_copy_tools(tools, n);
    agent_free_tools(a);
    a->tools = copy;
    a->ntools = copy ? n : 0;
}

void agentc_agent_set_events(AgcAgent *a, AgcEventFn cb, void *ud) {
    if (!a)
        return;
    a->events = cb;
    a->events_ud = ud;
}

void agentc_agent_set_thinking(AgcAgent *a, int level) {
    if (!a)
        return;
    if (level < 0)
        level = 0;
    if (level > 4)
        level = 4;
    if (a->thinking == level)
        return; /* numeric no-op: no emit */
    const char *previous = thinking_name(a->thinking);
    a->thinking = level;
    if (!want_observe("thinking_level_select"))
        return;
    AgcBuf p = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &p);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "level");
    agentc_jsonw_cstr(&w, thinking_name(level));
    agentc_jsonw_key(&w, "previous");
    agentc_jsonw_cstr(&w, previous);
    agentc_jsonw_end(&w);
    AgcExtResult r = agentc_ext_emit("thinking_level_select", (const char *)p.p);
    agentc_free(r.result_json);
    agentc_buf_free(&p);
}

const char *agentc_agent_thinking(const AgcAgent *a) {
    return thinking_name(a ? a->thinking : 0);
}

void agentc_agent_set_max_tokens(AgcAgent *a, i64 max_tokens) {
    if (!a)
        return;
    a->max_tokens = max_tokens;
}

void agentc_agent_set_retry(AgcAgent *a, int max_attempts) {
    if (!a)
        return;
    if (max_attempts > 0)
        a->max_attempts = max_attempts;
}

void agentc_agent_set_token_budget(AgcAgent *a, i64 tokens) {
    if (a)
        a->token_budget = tokens;
}

int64_t agentc_agent_token_budget(const AgcAgent *a) {
    return a ? a->token_budget : 0;
}

uint64_t agentc_agent_tokens_spent(const AgcAgent *a) {
    return a ? a->tokens_spent : 0;
}

void agentc_agent_set_record(AgcAgent *a, AgcBuf *sink) {
    if (!a)
        return;
    a->record = sink;
}

void agentc_agent_set_insecure(AgcAgent *a, bool insecure) {
    if (!a)
        return;
    a->insecure = insecure;
}

void agentc_agent_set_auto_compact(AgcAgent *a, bool on) {
    if (!a)
        return;
    a->auto_compact = on;
}

void agentc_agent_set_compact_limits(AgcAgent *a, u32 reserve_tokens, u32 keep_recent_tokens) {
    if (!a)
        return;
    if (reserve_tokens > 0)
        a->compact_reserve = reserve_tokens;
    if (keep_recent_tokens > 0)
        a->compact_keep = keep_recent_tokens;
}

bool agentc_agent_compacted(const AgcAgent *a) {
    return a ? a->compact_done : false;
}

u32 agentc_agent_context_tokens(const AgcAgent *a) {
    return a ? agentc_compact_estimate(&a->tr) : 0;
}

void agentc_agent_abort(AgcAgent *a) {
    if (!a)
        return;
    a->cancel = true;
}

/* Resume support: deep-copy a replayed transcript into an idle agent. */
int agentc_agent_load(AgcAgent *a, const AgcTranscript *t) {
    if (!a || !t)
        return -22 /* EINVAL */;
    if (t == &a->tr)
        return 0;

    agentc_transcript_free(&a->tr);
    agentc_transcript_init(&a->tr);
    a->compact_done = false;
    a->tr.model = a->model;
    a->tr.provider = a->prov ? a->prov->name : NULL;
    if (t->system)
        a->tr.system = agentc_strdup(t->system);

    for (size_t i = 0; i < t->n; i++) {
        const AgcMsg *src = &t->msgs[i];
        AgcMsg *dst = agentc_transcript_push(&a->tr, src->role);
        dst->stop_reason = src->stop_reason;
        dst->usage = src->usage;
        dst->ts_ms = src->ts_ms;
        if (src->error)
            dst->error = agentc_strdup(src->error);
        for (size_t b = 0; b < src->nblocks; b++) {
            const AgcBlock *sb = &src->blocks[b];
            AgcBlock *db = agentc_msg_block_new(dst, sb->type);
            if (sb->text) {
                db->text = agentc_strdup_len(sb->text, sb->text_len);
                db->text_len = sb->text_len;
            }
            if (sb->tool_id)
                db->tool_id = agentc_strdup(sb->tool_id);
            if (sb->tool_name)
                db->tool_name = agentc_strdup(sb->tool_name);
            if (sb->tool_args)
                db->tool_args = agentc_strdup(sb->tool_args);
        }
    }
    return 0;
}

const AgcTranscript *agentc_agent_transcript(const AgcAgent *a) {
    return a ? &a->tr : NULL;
}

/* --------------------------------------------------- agent actor (P6-B) */

AgcActorId agentc_agent_bind_actor(AgcAgent *a, AgcActorId parent, const char *label) {
    if (!a)
        return 0;
    if (a->actor_id != 0 && agentc_actor_exists(a->actor_id))
        return a->actor_id;
    AgcActorId id = agentc_actor_register(AGENTC_ACTOR_AGENT, parent, label);
    if (id == 0)
        return 0;
    AgcActor *row = agentc_actor_get(id);
    if (row != NULL) {
        row->agent = a;
        row->env = (struct AgcTrustEnv *)a->trust;
        row->state = AGENTC_ACTOR_RUNNING;
        row->steerable = true;
        row->idle_ms = AGENTC_AGENT_IDLE_DEF_MS;
        actor_event(row, AGENTC_EV_ACTOR_STARTED, NULL, NULL, 0);
    }
    a->actor_id = id;
    return id;
}

AgcActorId agentc_agent_actor(const AgcAgent *a) {
    return a ? a->actor_id : 0;
}

int64_t agentc_agent_turn_no(const AgcAgent *a) {
    return (a != NULL && a->actor_id != 0) ? agentc_actor_turn(a->actor_id) : 0;
}

bool agentc_agent_steerable(const AgcAgent *a, const char **kind) {
    if (kind != NULL)
        *kind = NULL;
    if (a == NULL)
        return true;
    if (a->nonsteerable) {
        if (kind != NULL)
            *kind = "compact";
        return false;
    }
    return true;
}

void agentc_agent_set_auto_start(AgcAgent *a, bool on) {
    if (a)
        a->auto_start = on;
}

void agentc_agent_set_tools_hooks(AgcAgent *a, bool on) {
    if (a)
        a->tools_hooks = on;
}

/* A tool opts out of the tool pipeline per-tool (NO_HOOKS) or per-run
 * (tools.hooks=off); both the `tool` stages and the legacy tool points honor
 * it (HOK §4). */
static bool tool_hooks_enabled(const AgcAgent *a, const AgcTool *tool) {
    if (a == NULL || !a->tools_hooks)
        return false;
    if (tool != NULL && (tool->flags & AGENTC_TOOL_NO_HOOKS))
        return false;
    return true;
}

/* ------------------------------------------------- the agent tree (P6-D) */

AgcAgent *agentc_agent_find(AgcActorId id) {
    AgcActor *row = agentc_actor_get(id);
    return (row != NULL && row->kind == AGENTC_ACTOR_AGENT) ? row->agent : NULL;
}

AgcActorId agentc_agent_parent(AgcActorId id) {
    return agentc_actor_parent(id);
}

size_t agentc_agent_list(AgcAgentInfo *out, size_t max) {
    AgcActor rows[AGENTC_ACTOR_MAX];
    size_t n = agentc_actor_list(rows, AGENTC_ACTOR_MAX);
    size_t total = 0, k = 0;
    for (size_t i = 0; i < n; i++) {
        if (rows[i].kind != AGENTC_ACTOR_AGENT)
            continue;
        if (out != NULL && k < max) {
            AgcAgentInfo *info = &out[k];
            agentc_memset(info, 0, sizeof *info);
            AgcAgent *a = rows[i].agent;
            info->id = rows[i].id;
            info->parent_id = rows[i].parent;
            info->depth = rows[i].depth;
            info->state = rows[i].state;
            info->is_subagent = rows[i].parent != 0;
            info->turn_no = rows[i].turn_seq;
            info->steerable = rows[i].steerable && (a == NULL || !a->nonsteerable);
            info->steerable_kind = (a != NULL && a->nonsteerable) ? "compact" : NULL;
            if (a != NULL) {
                info->model = a->model;
                info->provider = a->prov ? a->prov->name : NULL;
                info->task = a->task;
                info->exit_code = a->result_set ? a->result_exit : 0;
                info->token_budget = a->token_budget;
                info->tokens_spent = a->tokens_spent;
            }
            info->trust = rows[i].env;
            info->duration_ms =
                rows[i].started_ns != 0 ? (actor_now() - rows[i].started_ns) / 1000000 : 0;
            k++;
        }
        total++;
    }
    return out != NULL ? k : total;
}

int agentc_agent_info(AgcActorId id, AgcAgentInfo *out) {
    if (out == NULL)
        return -22;
    AgcActor *row = agentc_actor_get(id);
    if (row == NULL || row->kind != AGENTC_ACTOR_AGENT)
        return -3; /* ESRCH */
    agentc_memset(out, 0, sizeof *out);
    AgcAgent *a = row->agent;
    out->id = row->id;
    out->parent_id = row->parent;
    out->depth = row->depth;
    out->state = row->state;
    out->is_subagent = row->parent != 0;
    out->turn_no = row->turn_seq;
    out->steerable = row->steerable && (a == NULL || !a->nonsteerable);
    out->steerable_kind = (a != NULL && a->nonsteerable) ? "compact" : NULL;
    if (a != NULL) {
        out->model = a->model;
        out->provider = a->prov ? a->prov->name : NULL;
        out->task = a->task;
        out->exit_code = a->result_set ? a->result_exit : 0;
        out->token_budget = a->token_budget;
        out->tokens_spent = a->tokens_spent;
    }
    out->trust = row->env;
    out->duration_ms = row->started_ns != 0 ? (actor_now() - row->started_ns) / 1000000 : 0;
    return 0;
}

int agentc_agent_send(AgcActorId id, const char *text, i64 turn, int priority, uint32_t flags,
                      const char *client_id) {
    AgcActor *row = agentc_actor_get(id);
    if (row == NULL)
        return -3;
    if (row->kind != AGENTC_ACTOR_AGENT)
        return -22;
    /* R-D9: only the controller (root, or the actor's parent) may steer. */
    if (g_current_agent != NULL && g_current_agent->actor_id != 0) {
        bool caller_is_root = g_current_agent->parent_id == 0;
        if (!caller_is_root && g_current_agent->actor_id != row->parent)
            return -1; /* EPERM */
    }
    bool dup = client_id && client_id[0] && (flags & AGENTC_Q_F_DEDUPE) &&
               agentc_queue_has_client(id, client_id);
    uint64_t iid = agentc_queue_add(id, text ? text : "", turn, priority, flags, client_id);
    if (iid == 0)
        return -28; /* ENOSPC: queue_full */
    /* P7: a client_id dedupe drop is an idempotent success with no new input. */
    if (!dup)
        actor_event(row, AGENTC_EV_ACTOR_INPUT, text, client_id, 0);
    return 0;
}

int agentc_agent_cancel(AgcActorId id, int scope, int stage) {
    AgcActor *row = agentc_actor_get(id);
    if (row == NULL)
        return -3;
    if (scope == AGENTC_MSG_SCOPE_TURN) {
        AgcMessage m;
        agentc_memset(&m, 0, sizeof m);
        m.class = AGENTC_MSG_CLASS_CONTROL;
        m.kind = AGENTC_MSG_CANCEL;
        m.to = id;
        m.scope = AGENTC_MSG_SCOPE_TURN;
        m.stage = stage == 2 ? AGENTC_MSG_STAGE_FORCE : AGENTC_MSG_STAGE_GRACEFUL;
        return agentc_message_send(&m);
    }
    int fs = (scope == AGENTC_MSG_SCOPE_SUBTREE) ? AGENTC_FREEZE_SUBTREE : AGENTC_FREEZE_ACTOR;
    return agentc_actor_cancel(id, fs, stage);
}

int agentc_agent_wait(AgcActorId id, i64 timeout_ms, AgcBuf *result) {
    AgcActor *row = agentc_actor_get(id);
    if (row == NULL || row->kind != AGENTC_ACTOR_AGENT)
        return -3;
    i64 deadline = timeout_ms > 0 ? actor_now() + timeout_ms * 1000000 : 0;
    /* R-D17: every blocking wait pumps the one runtime driver, so a child's
     * notify/approval/input is serviced and the tree cannot deadlock. */
    while (!agentc_actor_get(id) || !(agentc_actor_get(id)->state == AGENTC_ACTOR_SETTLED ||
                                      agentc_actor_get(id)->state == AGENTC_ACTOR_FAILED ||
                                      agentc_actor_get(id)->state == AGENTC_ACTOR_CANCELLED)) {
        if (g_pump)
            agentc_pump(1);
        else
            agentc_runtime_tick();
        if (deadline != 0 && actor_now() >= deadline)
            return -110; /* ETIMEDOUT */
        if (!g_pump)
            os_sleep_ns(1000000);
    }
    if (result != NULL)
        return agentc_agent_result(id, result);
    return 0;
}

int agentc_agent_result(AgcActorId id, AgcBuf *out) {
    AgcAgent *a = agentc_agent_find(id);
    if (a == NULL || out == NULL)
        return -3;
    if (!a->result_set)
        return -61; /* ENODATA */
    agentc_buf_cstr(out, a->result ? a->result : "");
    return 0;
}

int agentc_agent_result_set(AgcActorId id, const char *text, int exit_code) {
    AgcAgent *a = agentc_agent_find(id);
    if (a == NULL)
        return -3;
    if (a->result_set)
        return -17; /* EEXIST: fail-closed on a second set */
    a->result = agentc_strdup(text ? text : "");
    a->result_exit = exit_code;
    a->result_set = true;
    AgcActor *row = agentc_actor_get(id);
    AgcAgentResultEvent e;
    agentc_memset(&e, 0, sizeof e);
    e.id = id;
    e.parent_id = a->parent_id;
    e.text = a->result;
    e.exit_code = exit_code;
    e.duration_ms = (row && row->started_ns != 0) ? (actor_now() - row->started_ns) / 1000000 : 0;
    AgcBuf j = {0};
    agentc_buf_printf(&j,
                      "{\"agent_id\":%u,\"parent_id\":%u,\"exit_code\":%d,"
                      "\"duration_ms\":%lld}",
                      (unsigned)id, (unsigned)a->parent_id, exit_code, (long long)e.duration_ms);
    e.json = (const char *)j.p;
    agentc_events_emit(NULL, AGENTC_EV_AGENT_RESULT, &e);
    agentc_buf_free(&j);
    return 0;
}

int agent_priv_capture_result(AgcAgent *a, int exit_code) {
    if (!a || a->result_set)
        return -1;
    const char *text = "";
    for (size_t i = a->tr.n; i > 0; i--) {
        AgcMsg *m = &a->tr.msgs[i - 1];
        if (m->role != AGENTC_ROLE_ASSISTANT)
            continue;
        for (size_t b = 0; b < m->nblocks; b++) {
            if (m->blocks[b].type == AGENTC_BLK_TEXT && m->blocks[b].text) {
                text = m->blocks[b].text;
                break;
            }
        }
        break;
    }
    return agentc_agent_result_set(a->actor_id, text, exit_code);
}

/* actor.c delegates agent actor lifecycle here. */
void agent_actor_release(AgcActor *row) {
    if (row == NULL)
        return;
    AgcAgent *a = row->agent;
    row->agent = NULL;
    if (a != NULL && a->owned_by_actor) {
        a->actor_id = 0;
        agent_destroy(a);
    }
}

void agent_actor_step(AgcActor *row) {
    if (row == NULL || row->kind != AGENTC_ACTOR_AGENT)
        return;
    AgcAgent *a = row->agent;
    if (a == NULL || !a->running)
        return;
    /* The first slice consumes the queued task; a resumed slice passes NULL and
     * continues the saved step. One model step per actor tick (R-D12). */
    char *text = a->pending_input;
    a->pending_input = NULL;
    AgcAgent *prev = g_current_agent;
    g_current_agent = a;
    int rc = agent_run_turn(a, text, true);
    g_current_agent = prev;
    agentc_free(text);
    if (rc == AGENTC_RUN_YIELD)
        return; /* still running; next tick resumes */
    a->running = false;
    (void)agent_priv_capture_result(a, rc == 0 ? 0 : 1);
    actor_finish_state(row, rc == 0 ? AGENTC_ACTOR_SETTLED : AGENTC_ACTOR_FAILED, NULL);
}

int agentc_agent_submit_auto(AgcAgent *a, const char *text) {
    if (!a)
        return -22;
    if (a->actor_id == 0)
        return agentc_agent_submit(a, text);

    /* Coalesce every due item (turn <= the next turn) with the explicit text
     * into the next submission (SIG §5.5). */
    AgcBuf combined = {0};
    if (text && text[0])
        agentc_buf_cstr(&combined, text);
    int64_t next = agentc_actor_turn(a->actor_id) + 1;
    for (;;) {
        AgcBuf q = {0};
        uint32_t fl = 0;
        AgcDuePhase ph = agentc_queue_flush(a->actor_id, next, &q, &fl);
        if (ph == AGENTC_DUE_NONE || q.len == 0) {
            agentc_buf_free(&q);
            break;
        }
        if (combined.len)
            agentc_buf_byte(&combined, '\n');
        agentc_buf_push(&combined, q.p, q.len);
        agentc_buf_free(&q);
    }
    if (combined.p == NULL)
        agentc_buf_byte(&combined, 0);
    int rc = agentc_agent_submit(a, (const char *)combined.p);
    agentc_buf_free(&combined);
    return rc;
}

/* Default dispatch for agent-actor messages (P6-SIGNALS §4/§7). */
int agent_handle_message(AgcActor *row, const AgcMessage *m) {
    if (row == NULL || m == NULL || row->kind != AGENTC_ACTOR_AGENT)
        return -3;
    AgcAgent *a = row->agent;
    if (a == NULL)
        return 0;
    switch (m->kind) {
    case AGENTC_MSG_INPUT: {
        /* Authority: only the controller (from 0) or the actor's parent may
         * enqueue into it (SIG §5.7, R-D9). */
        if (m->from != 0 && m->from != row->parent)
            return -1; /* EPERM */
        bool dup = m->client_id && m->client_id[0] && (m->flags & AGENTC_Q_F_DEDUPE) &&
                   agentc_queue_has_client(row->id, m->client_id);
        uint64_t id =
            agentc_queue_add(row->id, m->body, m->turn, m->priority, m->flags, m->client_id);
        if (id == 0)
            return -28; /* queue full */
        /* P7: a duplicate client_id enqueue is idempotent: no input event and no
         * steer request. */
        if (dup)
            return 0;
        actor_event(row, AGENTC_EV_ACTOR_INPUT, m->body, m->client_id, 0);
        if (m->priority < 0)
            a->steer_preempt = true;
        else if (agentc_actor_turn(row->id) > 0)
            a->steer_boundary = true;
        if (m->priority < 0)
            a->steer_preempt_tools = (m->flags & AGENTC_Q_F_PREEMPT_TOOLS) != 0;
        return 0;
    }
    case AGENTC_MSG_NOTIFY:
        actor_event(row, AGENTC_EV_ACTOR_NOTIFY, m->body, m->meta, 0);
        return 0;
    case AGENTC_MSG_CANCEL: {
        int stage = m->stage == AGENTC_MSG_STAGE_FORCE ? 2 : 1;
        if (m->scope == AGENTC_MSG_SCOPE_TURN) {
            a->cancel = true; /* stop the active turn; the actor stays */
            return 0;
        }
        (void)agentc_actor_cancel(row->id, AGENTC_FREEZE_SUBTREE, stage);
        a->cancel = true;
        a->steer_preempt = false;
        a->steer_boundary = false;
        actor_finish_state(row, AGENTC_ACTOR_CANCELLED, NULL);
        return 0;
    }
    case AGENTC_MSG_FREEZE: {
        int fs = m->scope == AGENTC_MSG_SCOPE_SUBTREE   ? AGENTC_FREEZE_SUBTREE
                 : m->scope == AGENTC_MSG_SCOPE_RUNTIME ? AGENTC_FREEZE_RUNTIME
                                                        : AGENTC_FREEZE_ACTOR;
        return agentc_actor_freeze(row->id, fs, m->flags);
    }
    case AGENTC_MSG_RESUME: {
        int fs = m->scope == AGENTC_MSG_SCOPE_SUBTREE   ? AGENTC_FREEZE_SUBTREE
                 : m->scope == AGENTC_MSG_SCOPE_RUNTIME ? AGENTC_FREEZE_RUNTIME
                                                        : AGENTC_FREEZE_ACTOR;
        return agentc_actor_resume(row->id, fs);
    }
    case AGENTC_MSG_SETTLE:
        a->settle = true;
        return 0;
    case AGENTC_MSG_SET_MODEL:
        return agentc_agent_set_model(a, m->body);
    case AGENTC_MSG_SET_THINKING: {
        bool ok = false;
        i64 lvl =
            agentc_parse_i64(m->body ? m->body : "0", m->body ? agentc_strlen(m->body) : 1, &ok);
        agentc_agent_set_thinking(a, (int)(ok ? lvl : 0));
        return 0;
    }
    case AGENTC_MSG_ATTACH:
        row->attached = true;
        actor_event(row, AGENTC_EV_ACTOR_ATTACHED, NULL, NULL, 0);
        return 0;
    case AGENTC_MSG_DETACH:
        row->attached = false;
        actor_event(row, AGENTC_EV_ACTOR_DETACHED, NULL, NULL, 0);
        return 0;
    default:
        return 0;
    }
}

const char *agentc_agent_last_error(const AgcAgent *a) {
    if (!a || !a->last_error[0])
        return NULL;
    return a->last_error;
}

/* test hook: disable real backoff sleeps (declared extern by tests) */
void agentc_agent_test_no_backoff(AgcAgent *a) {
    if (a)
        a->no_backoff = true;
}

/* --------------------------------------------------------------- helpers */

static void set_error(AgcAgent *a, const char *msg) {
    agentc_snprintf(a->last_error, sizeof a->last_error, "%s", msg ? msg : "error");
}

static void set_errorf(AgcAgent *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    agentc_vsnprintf(a->last_error, sizeof a->last_error, fmt, ap);
    va_end(ap);
}

/* Internal hub bridge (core/events.h): the hub owns the dispatch and quiet
 * policy; these two hooks keep struct AgcAgent private to this file. */
bool agentc_agent_event_muted(const AgcAgent *a, int ev) {
    if (!a || !a->cancel)
        return false;
    return ev != AGENTC_EV_ERROR && ev != AGENTC_EV_AGENT_END && ev != AGENTC_EV_STEP_END &&
           ev != AGENTC_EV_MSG_END && ev != AGENTC_EV_TOOL_EXEC_END;
}

void agentc_agent_event_own(AgcAgent *a, int ev, const void *data) {
    if (a && a->events)
        a->events(a->events_ud, ev, data);
}

/* P6-E status event lines: render the region's PER_TURN/ONCE lines, append them
 * to the transcript as a `custom notice` entry and (for chrome lifetimes) let
 * the TUI poll them. No-op when no line is registered for the region. */
static void status_notice(AgcAgent *a, int region, const AgcExtStatusCtx *ctx) {
    if (!a || !agentc_status_has_region(region))
        return;
    AgcBuf out = {0};
    if (agentc_status_emit(region, ctx, &out) == 0 || !out.p) {
        agentc_buf_free(&out);
        return;
    }
    AgcBuf j = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &j);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "region");
    agentc_jsonw_i64(&w, region);
    agentc_jsonw_key(&w, "turn");
    agentc_jsonw_u64(&w, ctx ? ctx->turn : 0);
    agentc_jsonw_key(&w, "step");
    agentc_jsonw_u64(&w, ctx ? ctx->step : 0);
    agentc_jsonw_key(&w, "text");
    agentc_jsonw_cstr(&w, out.p ? (char *)out.p : "");
    agentc_jsonw_end(&w);
    agentc_ext_append_entry("notice", j.p ? (char *)j.p : "{}");
    agentc_buf_free(&j);
    agentc_buf_free(&out);
}

static void emit(AgcAgent *a, int ev, const void *data) {
    /* P6-B universal pipeline: observe/transform the turn/step/message/tool
     * stages at the typed-event sites (no-op until a handler registers). */
    uint32_t actor = a != NULL ? a->actor_id : 0;
    switch (ev) {
    case AGENTC_EV_AGENT_START: {
        /* R-D31: `turn` is the user-driven cycle; it spans agent_start..
         * agent_settled. The inner model cycle is the `step` stage. */
        agentc_hook_stage_emit(AGENTC_HOOK_STAGE_TURN, "start", NULL, actor);
        break;
    }
    case AGENTC_EV_STEP_START:
        agentc_hook_stage_emit(AGENTC_HOOK_STAGE_STEP, "start", NULL, actor);
        if (a)
            a->step_no = 0;
        break;
    case AGENTC_EV_STEP_END: {
        /* The `step` end stage runs in step_end_hook (B8), paired with the
         * mapped legacy `step_end` point. */
        AgcExtStatusCtx ctx;
        agentc_memset(&ctx, 0, sizeof ctx);
        ctx.struct_size = sizeof ctx;
        ctx.region = AGENTC_REGION_TURN_END;
        ctx.turn = (uint64_t)agentc_agent_turn_no(a);
        ctx.step = a ? a->step_no : 0;
        status_notice(a, AGENTC_REGION_TURN_END, &ctx);
        break;
    }
    case AGENTC_EV_MSG_START:
        agentc_hook_stage_emit("message", "assistant", NULL, actor);
        break;
    case AGENTC_EV_MSG_END:
        break; /* the `message` stage runs in message_end_hook (B8) */
    case AGENTC_EV_AGENT_END: {
        agentc_hook_stage_emit(AGENTC_HOOK_STAGE_TURN, "end", NULL, actor);
        AgcExtStatusCtx ctx;
        agentc_memset(&ctx, 0, sizeof ctx);
        ctx.struct_size = sizeof ctx;
        ctx.region = AGENTC_REGION_FINAL_OUTPUT;
        ctx.turn = (uint64_t)agentc_agent_turn_no(a);
        ctx.step = a ? a->step_no : 0;
        ctx.final = 1;
        ctx.text = NULL;
        status_notice(a, AGENTC_REGION_FINAL_OUTPUT, &ctx);
        break;
    }
    default:
        break;
    }
    agentc_events_emit(a, ev, data);
}

/* Stamp lineage on every persisted message, then notify the (possibly shared)
 * observer (P6-SUBAGENTS §8). A child inherits the parent's observer so the
 * session sees its transcript with agent_id/parent_id. */
static void notify_observer(AgcAgent *a, AgcMsg *m) {
    if (a == NULL || m == NULL)
        return;
    if (a->actor_id != 0) {
        m->agent_id = a->actor_id;
        m->parent_id = a->parent_id;
    }
    if (a->observer)
        a->observer(a->observer_ud, m);
}

/* Provider request path, with `{model}` substituted when the row's path carries
 * it (the native Gemini API puts the model in the path). `prov`/`model` are the
 * effective (possibly routed) target, not necessarily a->prov/a->model. */
static void provider_path(const AgcProvider *prov, const char *model, char *out, size_t cap) {
    if (!out || cap == 0)
        return;
    out[0] = 0;
    const AgcProviderOps *ops = prov ? agentc_provider_ops(prov) : NULL;
    const char *p = (ops && ops->path) ? ops->path : "/";
    const char *m = agentc_str_str(p, "{model}");
    if (!m) {
        agentc_snprintf(out, cap, "%s", p);
        return;
    }
    const char *eff = (model && model[0]) ? model : "";
    size_t pre = (size_t)(m - p);
    if (pre > cap - 1)
        pre = cap - 1;
    agentc_memcpy(out, p, pre);
    out[pre] = 0;
    agentc_snprintf(out + pre, cap - pre, "%s%s", eff, m + 7);
}

static void build_url(char *out, size_t cap, const char *base, const char *path) {
    char b[1024];
    size_t bl = base ? agentc_strlen(base) : 0;
    if (bl >= sizeof b)
        bl = sizeof b - 1;
    if (bl)
        agentc_memcpy(b, base, bl);
    b[bl] = 0;
    while (bl > 0 && b[bl - 1] == '/')
        b[--bl] = 0;
    agentc_snprintf(out, cap, "%s%s", b, path);
}

/* A base-URL override is credential-adjacent, so it arrives through the route
 * target resolver (config-aware). A missing base falls back to the provider's
 * own default endpoint. */
static void provider_url(const char *base, const AgcProvider *prov, const char *model, char *out,
                         size_t cap) {
    char path[1024];
    provider_path(prov, model, path, sizeof path);
    build_url(out, cap, (base && base[0]) ? base : prov->default_base_url, path);
}

/* W4: the effective target for one model step. `alias` is non-NULL when a
 * virtual model row was applied (the alias stays the transcript identity);
 * `target` is the concrete model's catalog/discovered row for cost accounting,
 * or NULL when the target id is unknown (cost then -1). `base_url`/`api_key` are
 * resolved once here so a routed target uses the app's config-aware endpoint and
 * credential (never the agent's own). */
typedef struct {
    const AgcProvider *prov;
    const char *model;
    const char *alias;
    const AgcModel *target;
    const char *base_url; /* borrowed */
    const char *api_key;  /* borrowed */
    char model_name[256]; /* owned copy: a mid-request re-register cannot dangle it */
} AgcRoute;

/* Resolve a->model to the concrete route for the next request. A non-virtual
 * model yields the agent's own provider/model/endpoint/credential. A virtual row
 * resolves its separately-stored target through the per-agent seam; an unknown
 * target provider fails closed (returns -errno and sets the step error). Never
 * reads a process-global resolver (P8-D1). Never mutates a->prov/a->model. */
static int route_resolve(AgcAgent *a, AgcRoute *out) {
    agentc_memset(out, 0, sizeof *out);
    out->prov = a->prov;
    out->model = a->model;
    out->base_url = a->base_url;
    out->api_key = a->api_key;
    const AgcModel *vm = agentc_model_find_virtual(a->model);
    if (vm == NULL) {
        if (a->model_is_alias) {
            /* P8-D2 (B-M9): a live alias whose row was cleared is never sent as
             * a literal id to the agent's own provider/credential. */
            set_errorf(a, "virtual model %s is no longer configured; "
                          "re-select the model with /model or --model",
                      (a->model && a->model[0]) ? a->model : "?");
            return -2; /* ENOENT */
        }
        return 0;
    }
    const char *rp = vm->route_provider;
    const char *rm = vm->route_model;
    if (rp == NULL || rp[0] == 0 || rm == NULL || rm[0] == 0) {
        set_errorf(a, "virtual model %s has no route", (a->model && a->model[0]) ? a->model : "?");
        return -22; /* EINVAL */
    }
    const AgcProvider *prov = a->route.provider ? a->route.provider(a->route.provider_ud, rp)
                                                : resolve_provider_default(NULL, rp);
    if (prov == NULL) {
        set_errorf(a, "virtual model %s: unknown provider %s", a->model, rp);
        return -2; /* ENOENT: fail closed, never silently fall back */
    }
    size_t ml = agentc_strlen(rm);
    if (ml >= sizeof out->model_name) {
        set_errorf(a, "virtual model %s has an over-long target model", a->model);
        return -22;
    }
    agentc_memcpy(out->model_name, rm, ml + 1);
    out->prov = prov;
    out->model = out->model_name;
    out->alias = a->model;
    out->target = agentc_model_find(prov->name, out->model);
    /* P8-D3 (B-M4): a route back to the agent's own provider keeps the agent's
     * effective endpoint and credential (--base-url/--api-key/env/OAuth),
     * exactly like a direct model step; only a different provider goes through
     * the seam's target resolver. */
    if (prov == a->prov) {
        out->base_url = a->base_url;
        out->api_key = a->api_key;
    } else {
        out->base_url = prov->default_base_url;
        out->api_key = NULL;
        if (a->route.target) {
            AgcRouteTarget t;
            agentc_memset(&t, 0, sizeof t);
            if (a->route.target(a->route.target_ud, prov->name, &t)) {
                if (t.base_url && t.base_url[0])
                    out->base_url = t.base_url;
                out->api_key = t.api_key;
            }
        }
        if (out->api_key == NULL)
            out->api_key = agentc_auth_key(prov->name);
    }
    /* P8-D3 (B-M6): a routed step fails closed before any request when the
     * effective provider needs a credential and none resolved. A stored OAuth
     * credential counts (the adapter attaches it via account_id). */
    {
        const AgcProviderOps *ops = agentc_provider_ops(out->prov);
        if (ops != NULL && ops->needs_key &&
            (out->api_key == NULL || out->api_key[0] == 0) &&
            agentc_oauth_account_id(out->prov->name) == NULL) {
            set_errorf(a, "no API key for %s: pass --api-key, set the provider "
                          "env var, or run `agentc setup` / `agentc login %s`",
                       out->prov->name, out->prov->name);
            return -13; /* EACCES */
        }
    }
    return 0;
}

/* W4: publish the applied route on the typed event stream (deduped per agent).
 * The session layer subscribes (runsession.c) and appends a
 * `custom type:"model"` record; emitting here keeps the route in the ONE event
 * stream rather than inventing a parallel persistence path. */
static void emit_model_route(AgcAgent *a, const AgcRoute *route) {
    if (route->alias == NULL)
        return;
    AgcBuf key = {0};
    agentc_buf_cstr(&key, route->alias);
    agentc_buf_byte(&key, '|');
    agentc_buf_cstr(&key, route->prov->name);
    agentc_buf_byte(&key, '|');
    agentc_buf_cstr(&key, route->model);
    if (a->last_route != NULL && agentc_streq(a->last_route, (const char *)key.p)) {
        agentc_buf_free(&key);
        return; /* the same alias resolved to the same target: one record */
    }
    agentc_free(a->last_route);
    a->last_route = agentc_strdup((const char *)key.p);
    agentc_buf_free(&key);
    AgcModelRouteEvent e;
    e.virtual_model = route->alias;
    e.provider = route->prov->name;
    e.model = route->model;
    e.actor_id = a->actor_id; /* P8-D1: the persistence owner derives from this */
    emit(a, AGENTC_EV_MODEL_ROUTE, &e);
}

/* request_stream() result: the map failure and the transport retry hints the
 * caller's retry loop uses. out may be NULL (compaction). */
typedef struct {
    bool map_failed;
    i64 retry_after_ms;
    int last_status;
} AgcStreamResult;

static int request_stream(AgcAgent *a, const AgcProvider *prov, const char *model,
                          const AgcRequest *r, bool emit_events, AgcStreamState *st,
                          AgcStreamResult *out);

static const AgcTool *find_tool(const AgcAgent *a, const char *name) {
    if (!name)
        return NULL;
    for (size_t i = 0; i < a->ntools; i++)
        if (a->tools[i].name && agentc_streq(a->tools[i].name, name))
            return &a->tools[i];
    return NULL;
}

static size_t last_toolcall_args_len(const AgcMsg *m) {
    for (size_t i = m->nblocks; i > 0; i--) {
        const AgcBlock *b = &m->blocks[i - 1];
        if (b->type == AGENTC_BLK_TOOLCALL)
            return b->tool_args ? agentc_strlen(b->tool_args) : 0;
    }
    return 0;
}

/* True when the attempt wrote anything a front end has already rendered: text,
 * reasoning or tool-call arguments. A retry after that must emit
 * AGENTC_EV_MSG_RESET so those deltas are discarded, not appended to. */
static bool attempt_streamed(const AgcMsg *m) {
    for (size_t i = 0; i < m->nblocks; i++) {
        const AgcBlock *b = &m->blocks[i];
        if ((b->type == AGENTC_BLK_TEXT || b->type == AGENTC_BLK_THINK) && b->text_len > 0)
            return true;
        if (b->type == AGENTC_BLK_TOOLCALL && b->tool_args && b->tool_args[0])
            return true;
    }
    return false;
}

/* ------------------------------------------------------------ compaction */

static const char *compact_role_name(int role) {
    switch (role) {
    case AGENTC_ROLE_USER:
        return "user";
    case AGENTC_ROLE_ASSISTANT:
        return "assistant";
    case AGENTC_ROLE_TOOL:
        return "tool";
    default:
        return "system";
    }
}

/* One message object for the session_before_compact payload `messages` array,
 * byte-compatible with the session serializer's message shape. */
static void compact_message_json(AgcJsonW *w, const AgcMsg *m) {
    agentc_jsonw_obj(w);
    agentc_jsonw_key(w, "role");
    agentc_jsonw_cstr(w, compact_role_name(m->role));
    agentc_jsonw_key(w, "ts");
    agentc_jsonw_u64(w, m->ts_ms);
    agentc_jsonw_key(w, "content");
    agentc_jsonw_arr(w);
    for (size_t i = 0; i < m->nblocks; i++) {
        const AgcBlock *blk = &m->blocks[i];
        if (blk->type == AGENTC_BLK_TEXT) {
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "type");
            agentc_jsonw_cstr(w, "text");
            agentc_jsonw_key(w, "text");
            agentc_jsonw_str(w, blk->text ? blk->text : "", blk->text_len);
            agentc_jsonw_end(w);
        } else if (blk->type == AGENTC_BLK_THINK) {
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "type");
            agentc_jsonw_cstr(w, "thinking");
            agentc_jsonw_key(w, "thinking");
            agentc_jsonw_str(w, blk->text ? blk->text : "", blk->text_len);
            agentc_jsonw_end(w);
        } else if (blk->type == AGENTC_BLK_TOOLCALL) {
            agentc_jsonw_obj(w);
            agentc_jsonw_key(w, "type");
            agentc_jsonw_cstr(w, "tool_call");
            agentc_jsonw_key(w, "id");
            agentc_jsonw_cstr(w, blk->tool_id ? blk->tool_id : "");
            agentc_jsonw_key(w, "name");
            agentc_jsonw_cstr(w, blk->tool_name ? blk->tool_name : "");
            agentc_jsonw_key(w, "arguments");
            if (blk->tool_args && blk->tool_args[0])
                agentc_jsonw_str(w, blk->tool_args, agentc_strlen(blk->tool_args));
            else
                agentc_jsonw_str(w, "{}", 2);
            agentc_jsonw_end(w);
        }
    }
    agentc_jsonw_end(w);
    if (m->role == AGENTC_ROLE_TOOL) {
        const char *call_id = (m->nblocks && m->blocks[0].tool_id) ? m->blocks[0].tool_id : "";
        agentc_jsonw_key(w, "tool_call_id");
        agentc_jsonw_cstr(w, call_id);
        agentc_jsonw_key(w, "is_error");
        agentc_jsonw_bool(w, m->error != NULL);
    }
    agentc_jsonw_end(w);
}

/* session_compact_failed: observe a real failure only (`!prov`/`!transport` or
 * a request/summary failure), never "nothing to do" or an extension cancel. */
static void compact_failed(AgcAgent *a, const char *reason) {
    if (!want_observe("session_compact_failed"))
        return;
    AgcBuf p = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &p);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "reason");
    agentc_jsonw_cstr(&w, reason);
    agentc_jsonw_key(&w, "aborted");
    agentc_jsonw_bool(&w, a->cancel);
    agentc_jsonw_key(&w, "error");
    agentc_jsonw_cstr(&w, a->last_error);
    agentc_jsonw_end(&w);
    AgcExtResult r = agentc_ext_emit("session_compact_failed", (const char *)p.p);
    agentc_free(r.result_json);
    agentc_buf_free(&p);
}

/* session_compact: observe after a successful splice. */
static void compact_completed(const char *reason, bool from_extension) {
    if (!want_observe("session_compact"))
        return;
    AgcBuf p = {0};
    AgcJsonW w;
    agentc_jsonw_init(&w, &p);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "reason");
    agentc_jsonw_cstr(&w, reason);
    agentc_jsonw_key(&w, "from_extension");
    agentc_jsonw_bool(&w, from_extension);
    agentc_jsonw_end(&w);
    AgcExtResult r = agentc_ext_emit("session_compact", (const char *)p.p);
    agentc_free(r.result_json);
    agentc_buf_free(&p);
}

/* Summarize the transcript prefix that agentc_compact_cut() selected, replace it
 * with the checkpoint user message and emit AGENTC_EV_COMPACT. The summary runs
 * through the normal stream path with the event hub quiet, so it is silent and
 * provider-neutral; an extension may instead supply the summary through
 * session_before_compact (the provider request is skipped). Returns 0 (also on
 * "nothing to do" and on an extension cancel) or a negative errno. */
static int compact_now(AgcAgent *a, bool automatic) {
    const char *reason = automatic ? "threshold" : "manual";
    /* `agentc_agent_compacted()` reports the LAST call, so clear it here: a
     * no-op (nothing to cut) or a failure must not keep reporting the previous
     * success. */
    a->compact_done = false;
    if (!a->prov || !a->transport.request) {
        compact_failed(a, reason);
        return a->prov ? -107 /* ENOTCONN */ : -22 /* EINVAL */;
    }
    /* W4: a compaction request follows the same virtual-model resolution as a
     * model step. The alias stays the transcript identity; a failed route fails
     * the compaction closed (no silent fall back). */
    AgcRoute route;
    int rrc = route_resolve(a, &route);
    if (rrc != 0) {
        compact_failed(a, reason);
        return rrc;
    }
    u32 before = agentc_compact_estimate(&a->tr);
    size_t cut = agentc_compact_cut(&a->tr, a->compact_keep);
    if (cut == 0 || cut >= a->tr.n)
        return 0;
    size_t kept = a->tr.n - cut;

    /* session_before_compact: first-policy override after the cut is known. A
     * blocked/failed result or a handled result without a non-empty summary
     * cancels compaction (no event); a non-empty summary skips the provider
     * request and is spliced verbatim. */
    char *ext_summary = NULL;
    if (want_override("session_before_compact") || agentc_hook_wants(AGENTC_HOOK_STAGE_COMPACT)) {
        AgcBuf p = {0};
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "reason");
        agentc_jsonw_cstr(&w, reason);
        agentc_jsonw_key(&w, "first_kept");
        agentc_jsonw_u64(&w, (u64)cut);
        agentc_jsonw_key(&w, "messages");
        agentc_jsonw_arr(&w);
        for (size_t i = 0; i < cut; i++)
            compact_message_json(&w, &a->tr.msgs[i]);
        agentc_jsonw_end(&w);
        agentc_jsonw_end(&w);
        /* B8: the `compact` stage and the legacy `session_before_compact`
         * point share one first-decision dispatch (blocked/handled without a
         * summary cancels; a non-empty summary is spliced verbatim). */
        AgcMessage hm;
        agentc_memset(&hm, 0, sizeof hm);
        hm.class = AGENTC_MSG_CLASS_HOOK;
        hm.kind = AGENTC_MSG_INPUT;
        hm.meta = "compact";
        hm.body = (const char *)p.p;
        hm.body_len = p.len;
        hm.id = agentc_hook_next_id();
        AgcMessage *hout = NULL;
        int hrc = agentc_hook_run_legacy(AGENTC_HOOK_STAGE_COMPACT, "session_before_compact", &hm,
                                         (const char *)p.p, &hout);
        (void)hout;
        bool cancelled = hrc < 0 || hm.blocked;
        if (!cancelled && hm.result_json) {
            AgcJsonArena *arena = agentc_json_arena_new(0);
            AgcJson *o = agentc_json_parse_in(arena, hm.result_json, agentc_strlen(hm.result_json));
            const char *s = agentc_json_get_str(o, "summary");
            if (s && s[0]) {
                ext_summary = agentc_strdup(s);
            } else if (agentc_json_get_bool(o, "cancel", false)) {
                cancelled = true;
            } else if (hm.handled) {
                cancelled = true; /* handled without a summary */
            }
            /* No summary and no cancel: the extension made no decision, so the
             * default provider summary runs. */
            agentc_json_arena_free(arena);
        } else if (!cancelled && hm.handled) {
            cancelled = true;
        }
        agentc_hook_result_free(&hm);
        agentc_buf_free(&p);
        if (cancelled)
            return 0;
    }

    AgcTranscript prefix;
    agentc_memset(&prefix, 0, sizeof prefix);
    prefix.msgs = a->tr.msgs;
    prefix.n = cut;
    prefix.cap = cut;
    prefix.model = a->tr.model;
    prefix.provider = a->tr.provider;
    prefix.system = agentc_compact_system_prompt();

    AgcBuf sum = {0};
    char *summary = NULL;
    bool from_extension = false;
    int rc = 0;

    if (ext_summary) {
        summary = ext_summary;
        ext_summary = NULL;
        from_extension = true;
    } else {
        const AgcModel *m =
            route.target ? route.target : agentc_model_find(route.prov->name, route.model);
        i64 max_tokens = (i64)((u64)a->compact_reserve * 8 / 10);
        if (m && max_tokens > (i64)m->max_tokens)
            max_tokens = (i64)m->max_tokens;
        if (max_tokens < 256)
            max_tokens = 256;
        if (a->max_tokens > 0 && max_tokens > a->max_tokens)
            max_tokens = a->max_tokens;

        AgcRequest r;
        agentc_memset(&r, 0, sizeof r);
        r.provider = route.prov->name;
        r.model = route.model;
        r.api_key = route.api_key;
        r.base_url = route.base_url;
        r.system = agentc_compact_system_prompt();
        r.tools = NULL;
        r.ntools = 0;
        r.thinking_level = 0;
        r.account_id = agentc_oauth_account_id(route.prov->name);
        r.max_tokens = max_tokens;
        r.transcript = &prefix;

        /* The summary is an ordinary streamed request; quiet suppresses every
         * front end so only this function consumes the deltas. */
        bool quiet_prev = agentc_events_quiet();
        agentc_events_set_quiet(true);

        AgcStreamState st;
        rc = agentc_stream_state_init(route.prov, &st);
        AgcMsg sm;
        agentc_memset(&sm, 0, sizeof sm);
        sm.role = AGENTC_ROLE_USER;
        sm.ts_ms = (u64)(os_now_ns(OS_CLOCK_REALTIME) / 1000000);

        if (rc != 0) {
            set_error(a, "compaction: cannot start the summarization request");
            rc = -1;
        } else {
            st.msg = &sm;
            AgcStreamResult res;
            int trc = request_stream(a, route.prov, route.model, &r, false, &st, &res);
            if (trc == REQ_BUILD_FAIL || trc == REQ_MALFORMED || trc == REQ_BLOCKED) {
                set_error(a, trc == REQ_BLOCKED
                                 ? "compaction: request blocked by extension"
                                 : "compaction: cannot build the summarization request");
                rc = -1;
            } else if (trc != 0) {
                set_error(a, "compaction: summarization request failed");
                rc = trc;
            } else {
                int fr = route.prov->finish(&st);
                if (fr != 0 || res.map_failed) {
                    set_error(a,
                              st.error[0] ? st.error : "compaction: summary response had no text");
                    rc = -1;
                } else {
                    for (size_t i = 0; i < sm.nblocks; i++) {
                        const AgcBlock *b = &sm.blocks[i];
                        if (b->type == AGENTC_BLK_TEXT && b->text && b->text_len)
                            agentc_buf_push(&sum, b->text, b->text_len);
                    }
                    if (sum.len == 0) {
                        set_error(a, "compaction: summary response had no text");
                        rc = -1;
                    }
                }
            }
        }

        /* Every exit restores the caller's quiet state before anything is
         * emitted, so the observe points below still dispatch. */
        agentc_events_set_quiet(quiet_prev);
        agentc_msg_free(&sm);
        agentc_stream_state_close(&st);

        if (rc != 0) {
            compact_failed(a, reason);
        } else {
            agentc_buf_byte(&sum, 0);
            summary = (char *)sum.p;
            sum.p = NULL;
            sum.len = sum.cap = 0;
        }
    }

    if (rc == 0) {
        size_t summary_len = agentc_strlen(summary);

        AgcCompactInfo ci;
        agentc_memset(&ci, 0, sizeof ci);
        ci.tokens_before = before;
        ci.kept_messages = (u32)kept;
        ci.automatic = automatic;
        ci.summary = summary;
        /* Emit before the splice: the front end persists the checkpoint
         * synchronously (agentc_session_append_compaction), so a crash between
         * the record and the splice cannot lose the checkpoint. Core has no
         * session pointer, so a failed append degrades the session but the
         * in-memory splice still proceeds; the flush index re-syncs on the next
         * observer call. */
        emit(a, AGENTC_EV_COMPACT, &ci);

        for (size_t i = 0; i < cut; i++)
            agentc_msg_free(&a->tr.msgs[i]);
        if (kept)
            agentc_memmove(&a->tr.msgs[1], &a->tr.msgs[cut], kept * sizeof(AgcMsg));
        AgcMsg cp;
        agentc_memset(&cp, 0, sizeof cp);
        cp.role = AGENTC_ROLE_USER;
        cp.ts_ms = (u64)(os_now_ns(OS_CLOCK_REALTIME) / 1000000);
        agentc_msg_add_text(&cp, summary, summary_len);
        a->tr.msgs[0] = cp;
        a->tr.n = kept + 1;
        a->compact_done = true;
        compact_completed(reason, from_extension);
    }
    agentc_free(summary);
    return rc;
}

int agentc_agent_compact(AgcAgent *a) {
    if (!a)
        return -22; /* EINVAL */
    return compact_now(a, false);
}

/* ------------------------------------------------------------- streaming */

typedef struct {
    AgcAgent *a;
    const AgcProvider *prov; /* effective (possibly routed) provider */
    const char *model;       /* effective model id */
    AgcStreamState *st;
    AgcSse sse;
    bool failed;
} SseCtx;

static void emit_text(AgcAgent *a, const AgcBlock *b, const char *p, size_t n) {
    if (!p || !n)
        return;
    agentc_hook_stage_emit("output", b->type == AGENTC_BLK_THINK ? "think" : "text", "delta",
                           a->actor_id);
    AgcTextDelta d = {p, n};
    emit(a, b->type == AGENTC_BLK_THINK ? AGENTC_EV_THINK_DELTA : AGENTC_EV_TEXT_DELTA, &d);
}

static int on_sse_event(void *ud, const AgcSseEvent *ev) {
    SseCtx *c = ud;
    AgcAgent *a = c->a;
    AgcMsg *m = c->st->msg;
    if (a->cancel || a->steer_preempt)
        return 1;

    if (provider_watched("provider_stream_event", true) && ev && ev->data) {
        AgcBuf p = {0};
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "provider");
        agentc_jsonw_cstr(&w, c->prov->name);
        agentc_jsonw_key(&w, "api");
        agentc_jsonw_cstr(&w, c->prov->api);
        agentc_jsonw_key(&w, "model");
        agentc_jsonw_cstr(&w, c->model ? c->model : "");
        agentc_jsonw_key(&w, "event");
        agentc_jsonw_cstr(&w, ev->event ? ev->event : "");
        agentc_jsonw_key(&w, "data");
        /* The wire layer passes non-JSON payloads through (the OpenAI `[DONE]`
         * sentinel), so keep the hook payload valid JSON: raw when it parses,
         * a quoted string otherwise. */
        {
            AgcJsonArena *ja = agentc_json_arena_new(0);
            AgcJson *jd = agentc_json_parse_in(ja, ev->data ? ev->data : "", ev->data_len);
            if (jd)
                agentc_jsonw_raw(&w, ev->data ? ev->data : "", ev->data_len);
            else
                agentc_jsonw_str(&w, ev->data ? ev->data : "", ev->data_len);
            agentc_json_arena_free(ja);
        }
        agentc_jsonw_end(&w);
        AgcMessage hm;
        (void)provider_hook(a, "provider_stream_event", "stream_event", &p, true, &hm);
        agentc_hook_result_free(&hm);
        agentc_buf_free(&p);
    }

    size_t nb = m->nblocks;
    size_t last_text = nb ? m->blocks[nb - 1].text_len : 0;
    size_t last_args = last_toolcall_args_len(m);

    if (c->prov->map_sse(c->st, ev) != 0)
        c->failed = true;

    if (m->nblocks > nb) {
        AgcBlock *b = &m->blocks[m->nblocks - 1];
        if (b->type == AGENTC_BLK_TEXT || b->type == AGENTC_BLK_THINK)
            emit_text(a, b, b->text, b->text_len);
        else if (b->type == AGENTC_BLK_TOOLCALL && b->tool_args)
            emit(a, AGENTC_EV_TOOL_ARGS_DELTA,
                 &(AgcTextDelta){b->tool_args, agentc_strlen(b->tool_args)});
    } else if (nb > 0) {
        AgcBlock *b = &m->blocks[nb - 1];
        if (b->text_len > last_text && (b->type == AGENTC_BLK_TEXT || b->type == AGENTC_BLK_THINK))
            emit_text(a, b, b->text + last_text, b->text_len - last_text);
        if (b->type == AGENTC_BLK_TOOLCALL && b->tool_args) {
            size_t now = agentc_strlen(b->tool_args);
            if (now > last_args)
                emit(a, AGENTC_EV_TOOL_ARGS_DELTA,
                     &(AgcTextDelta){b->tool_args + last_args, now - last_args});
        }
    }
    return 0;
}

static int on_sse_chunk(void *ud, const void *p, size_t n) {
    SseCtx *c = ud;
    /* P6-B: let a preempt/cancel land mid-stream (R-D17: the bus is serviced
     * during a streaming wait, never skipped). */
    if (c->a->actor_id != 0)
        (void)agentc_message_drain(c->a->actor_id, 0);
    if (c->a->cancel || c->a->steer_preempt)
        return 1;
    return agentc_sse_feed(&c->sse, p, n, on_sse_event, c);
}

/* -------------------------------------------------------- tool job table */
/* Per-call result storage: the job driver frees its buffers when the batch
 * returns, so the DONE callback copies the text here for the source-ordered
 * transcript append that follows. */
typedef struct {
    AgcBuf out;
    bool is_error;
    i64 duration_ms;
    bool started;
    bool ran;
    /* The tool that actually ran this call, captured per slot so the
     * source-ordered result loop and the result hook never index the
     * 32-entry `job_tools` batch array with a global index. */
    const AgcTool *tool;
} ToolSlot;

typedef struct {
    AgcAgent *a;
    ToolSlot *slots;
    size_t base; /* global index of this chunk's first call */
} JobCtx;

static int on_tool_job(void *ud, int what, const AgcJob *job) {
    JobCtx *c = ud;
    ToolSlot *s = &c->slots[c->base + job->index];
    AgcToolExec ex;
    agentc_memset(&ex, 0, sizeof ex);
    ex.call_id = job->call.call_id;
    ex.tool_name = job->call.name;
    ex.args_json = job->call.args_json;

    if (what == AGENTC_JOB_STARTED) {
        /* The `tool` call stage is run explicitly in the batch loop (P6-D) so a
         * decision can narrow/replace/deny the call; do not double-fire it
         * here. Only the observe result stage is emitted from the job driver. */
        emit(c->a, AGENTC_EV_TOOL_EXEC_START, &ex);
        s->started = true;
        return 0;
    }
    if (job->out.len > 0)
        agentc_buf_push(&s->out, job->out.p, job->out.len);
    s->is_error = job->is_error;
    s->duration_ms = job->duration_ms;
    s->ran = true;
    /* A pre-aborted call is answered without a start event, so it must not
     * emit an end event either (the old loop's backfill path). */
    if (s->started) {
        ex.result = s->out.p != NULL ? (const char *)s->out.p : "";
        ex.is_error = s->is_error;
        ex.duration_ms = s->duration_ms;
        /* The `tool` result stage runs in tool_result_hook (B8), paired with the
         * legacy `tool_result` point; the typed event still fires here. */
        emit(c->a, AGENTC_EV_TOOL_EXEC_END, &ex);
    }
    return 0;
}

/* --------------------------------------------------------------- request */

/* Build `r`, POST it to the provider path, feed the SSE response through the
 * provider adapter into `st`. Emits the provider_stream_event and
 * after_provider_response observe points only when emit_events is set. Returns
 * the transport code (positive HTTP status or negative errno) or REQ_BUILD_FAIL
 * / REQ_MALFORMED. */
static int request_stream(AgcAgent *a, const AgcProvider *prov, const char *model,
                          const AgcRequest *r, bool emit_events, AgcStreamState *st,
                          AgcStreamResult *out) {
    char url[1200];
    provider_url(r->base_url, prov, model, url, sizeof url);

    if (out) {
        out->map_failed = false;
        out->retry_after_ms = 0;
        out->last_status = 0;
    }

    AgcBuf req = {0};
    char path[1024];
    provider_path(prov, model, path, sizeof path);
    if (prov->build_request(&req, r, NULL, path) != 0) {
        agentc_buf_free(&req);
        return REQ_BUILD_FAIL;
    }
    i64 k = agentc_str_find((const char *)req.p, req.len, "\r\n\r\n", 4);
    if (k < 0) {
        agentc_buf_free(&req);
        return REQ_MALFORMED;
    }
    char *headers = agentc_strdup_len((const char *)req.p, (size_t)k);
    const u8 *body = req.p + k + 4;
    size_t body_len = req.len - (size_t)k - 4;
    AgcBuf body_owned = {0};

    /* before_provider_headers: folded through the `provider` stage (sub-kind
     * `headers`), the flat header patch (null deletes) is applied after the
     * head/body split and before send; a blocked or failed (fail-closed)
     * result fails the request. Overrides are not quiet-gated. */
    if (provider_watched("before_provider_headers", false)) {
        char *hjson = agentc_ext_headers_to_json(headers);
        AgcBuf p = {0};
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "provider");
        agentc_jsonw_cstr(&w, prov->name);
        agentc_jsonw_key(&w, "api");
        agentc_jsonw_cstr(&w, prov->api);
        agentc_jsonw_key(&w, "model");
        agentc_jsonw_cstr(&w, model ? model : "");
        agentc_jsonw_key(&w, "headers");
        agentc_jsonw_raw(&w, hjson, agentc_strlen(hjson));
        agentc_jsonw_end(&w);
        AgcMessage hm;
        int hrc = provider_hook(a, "before_provider_headers", "headers", &p, false, &hm);
        if (hrc < 0 || hm.blocked) {
            agentc_hook_result_free(&hm);
            agentc_free(hjson);
            agentc_buf_free(&p);
            agentc_free(headers);
            agentc_buf_free(&req);
            return REQ_BLOCKED;
        }
        if (hm.result_json) {
            char *patched = agentc_ext_headers_apply_patch(headers, hm.result_json);
            if (patched) {
                agentc_free(headers);
                headers = patched;
            } else {
                agentc_logf(2, "ext: before_provider_headers patch ignored");
            }
        }
        agentc_hook_result_free(&hm);
        agentc_free(hjson);
        agentc_buf_free(&p);
    }

    /* before_provider_request: folded through the `provider` stage (sub-kind
     * `request`). A returned `payload` object replaces the serialized body;
     * Content-Length and --record reflect the replacement because the transport
     * derives the length from body_len. Overrides are not quiet-gated. */
    if (provider_watched("before_provider_request", false)) {
        AgcBuf p = {0};
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "provider");
        agentc_jsonw_cstr(&w, prov->name);
        agentc_jsonw_key(&w, "api");
        agentc_jsonw_cstr(&w, prov->api);
        agentc_jsonw_key(&w, "model");
        agentc_jsonw_cstr(&w, model ? model : "");
        agentc_jsonw_key(&w, "url");
        agentc_jsonw_cstr(&w, url);
        agentc_jsonw_key(&w, "payload");
        if (body && body_len)
            agentc_jsonw_raw(&w, (const char *)body, body_len);
        else
            agentc_jsonw_null(&w);
        agentc_jsonw_end(&w);
        AgcMessage hm;
        int hrc = provider_hook(a, "before_provider_request", "request", &p, false, &hm);
        if (hrc < 0 || hm.blocked) {
            agentc_hook_result_free(&hm);
            agentc_buf_free(&p);
            agentc_free(headers);
            agentc_buf_free(&req);
            return REQ_BLOCKED;
        }
        if (hm.result_json) {
            char *eff = hook_effective(&p, hm.result_json);
            if (eff) {
                AgcJsonArena *arena = agentc_json_arena_new(0);
                AgcJson *o = agentc_json_parse_in(arena, eff, agentc_strlen(eff));
                const AgcJson *pl = agentc_json_get(o, "payload");
                if (agentc_json_type(pl) == AGENTC_JSON_OBJ) {
                    AgcJsonW bw;
                    agentc_jsonw_init(&bw, &body_owned);
                    agentc_json_emit(&bw, pl);
                    body = body_owned.p;
                    body_len = body_owned.len;
                } else {
                    agentc_logf(2, "ext: before_provider_request payload not an object, "
                                   "keeping the original");
                }
                agentc_json_arena_free(arena);
                agentc_free(eff);
            }
        }
        agentc_hook_result_free(&hm);
        agentc_buf_free(&p);
    }

    SseCtx ctx;
    agentc_memset(&ctx, 0, sizeof ctx);
    ctx.a = a;
    ctx.prov = prov;
    ctx.model = model;
    ctx.st = st;
    agentc_sse_init(&ctx.sse);

    /* The HTTP transport observes the current agent's cancel flag: a front end
     * that aborts (Esc) while the body is blocked between SSE chunks now ends
     * the read at the next 50 ms wait slice. Custom/replay transports ignore
     * the predicate, so their behaviour is unchanged. */
    agentc_transport_http_set_cancel(&a->cancel);
    int trc = a->transport.request(a->transport.ud, url, headers, body, body_len, on_sse_chunk,
                                   &ctx, a->timeout_ms, a->record);
    i64 retry_after_ms = agentc_transport_http_retry_after_ms();
    int last_status = agentc_transport_http_last_status();
    if (out) {
        out->retry_after_ms = retry_after_ms;
        out->last_status = last_status;
    }
    if (emit_events && provider_watched("after_provider_response", true)) {
        AgcBuf p = {0};
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "provider");
        agentc_jsonw_cstr(&w, prov->name);
        agentc_jsonw_key(&w, "status");
        agentc_jsonw_i64(&w, last_status);
        agentc_jsonw_key(&w, "headers");
        const char *rh =
            a->transport.response_headers ? a->transport.response_headers(a->transport.ud) : NULL;
        if (!rh)
            rh = "{}";
        agentc_jsonw_raw(&w, rh, agentc_strlen(rh));
        agentc_jsonw_key(&w, "transport_error");
        agentc_jsonw_i64(&w, trc);
        agentc_jsonw_end(&w);
        AgcMessage hm;
        (void)provider_hook(a, "after_provider_response", "response", &p, true, &hm);
        agentc_hook_result_free(&hm);
        agentc_buf_free(&p);
    }
    /* A clean transport completion is an SSE EOF: dispatch an event the server
     * terminated without its blank line instead of dropping the final token. A
     * failed transfer stays undispatched (the partial event is not trustworthy). */
    if (trc == 0)
        agentc_sse_finish(&ctx.sse, on_sse_event, &ctx);
    agentc_sse_free(&ctx.sse);
    agentc_free(headers);
    agentc_buf_free(&body_owned);
    agentc_buf_free(&req);
    if (out)
        out->map_failed = ctx.failed;
    if (a)
        a->step_no++;
    {
        AgcExtStatusCtx sctx;
        agentc_memset(&sctx, 0, sizeof sctx);
        sctx.struct_size = sizeof sctx;
        sctx.region = AGENTC_REGION_STEP_END;
        sctx.turn = (uint64_t)agentc_agent_turn_no(a);
        sctx.step = a ? a->step_no : 0;
        sctx.stop_reason = out ? out->last_status : 0;
        status_notice(a, AGENTC_REGION_STEP_END, &sctx);
    }
    return trc;
}

/* Answer every tool call on transcript message `idx` exactly once. Used only
 * for an assistant turn that IS serialized/persisted (turn_ok with stop_reason
 * STOP/LENGTH) but whose calls could not run: a call left unanswered poisons
 * every later request (providers reject a tool_use without a matching
 * tool_result, and --continue replays the poison).
 *
 * Invariant: the serialized/persisted transcript never contains a `tool` result
 * whose call id has no serialized assistant `tool_call`, and never a serialized
 * `tool_call` without a matching result. The persistence layer skips failed
 * (ERROR/ABORTED) assistant turns, so their calls must NOT be answered here;
 * doing so on the !turn_ok path would persist an orphan `tool` result. */
static void close_tool_calls(AgcAgent *a, size_t idx, const char *why) {
    size_t n = agentc_msg_count_tool_calls(&a->tr.msgs[idx]);
    for (size_t i = 0; i < n; i++) {
        /* agentc_transcript_push() may realloc the message array. */
        AgcMsg *asst = &a->tr.msgs[idx];
        AgcBlock *tc = agentc_msg_nth_tool_call(asst, i);
        if (!tc)
            break;
        const char *id = tc->tool_id;
        const char *name = tc->tool_name;
        AgcMsg *tm = agentc_transcript_push(&a->tr, AGENTC_ROLE_TOOL);
        agentc_msg_add_tool_result(tm, id, name, why);
        tm->error = agentc_strdup("error");
        notify_observer(a, tm);
    }
}

/* ---------------------------------------------------------------- submit */

/* Sleep a retry backoff in <=50 ms slices so an abort interrupts it promptly
 * instead of waiting out a long Retry-After or exponential delay. With a front
 * end pump installed the slice runs it, so the UI repaints and can set the cancel
 * flag; otherwise the slice is a plain sleep. `no_backoff` (tests) skips it. */
#define BACKOFF_SLICE_MS 50
static void backoff_sleep(AgcAgent *a, i64 ms) {
    if (ms <= 0 || a->no_backoff)
        return;
    while (ms > 0 && !a->cancel) {
        i64 slice = ms < BACKOFF_SLICE_MS ? ms : BACKOFF_SLICE_MS;
        if (g_pump)
            agentc_pump((int)slice);
        else
            os_sleep_ns(slice * 1000000);
        ms -= slice;
    }
}

/* P6-B loop safe point: drain this agent's control/queue messages and apply the
 * due BOUNDARY set as a new user message (P6-SIGNALS §5.3/§5.4). Returns true
 * when a queued item was delivered, so the caller can re-issue the request
 * within the same turn. Delivery of the PREEMPT set is refined in B6. */
static void agent_emit_steered(AgcAgent *a, const char *at) {
    AgcActor *row = agentc_actor_get(a->actor_id);
    if (row == NULL)
        return;
    AgcActorEvent e;
    agentc_memset(&e, 0, sizeof e);
    e.id = row->id;
    e.parent_id = row->parent;
    e.depth = row->depth;
    e.kind = row->kind;
    e.state = row->state;
    e.turn = row->turn_seq;
    e.at = at;
    agentc_events_emit(NULL, AGENTC_EV_ACTOR_STEERED, &e);
}

static bool loop_safe_point(AgcAgent *a, bool apply_queue) {
    if (a->actor_id == 0)
        return false;
    (void)agentc_message_drain(a->actor_id, 0);
    if (!apply_queue)
        return false;
    if (a->nonsteerable) {
        /* A non-steerable step (compact) holds the due set; a CANCEL_NONSTEERABLE
         * item aborts the turn so it is resubmitted (SIG §5/§12). */
        AgcQueueItem items[AGENTC_QUEUE_MAX];
        int n = agentc_queue_list(a->actor_id, items, AGENTC_QUEUE_MAX);
        for (int i = 0; i < n; i++) {
            if (items[i].priority < 0 && (items[i].flags & AGENTC_Q_F_CANCEL_NONSTEERABLE)) {
                a->cancel = true;
                break;
            }
        }
        return false;
    }
    bool applied = false;
    for (;;) {
        AgcBuf text = {0};
        uint32_t flags = 0;
        int64_t active = agentc_actor_turn(a->actor_id);
        AgcDuePhase ph = agentc_queue_flush(a->actor_id, active, &text, &flags);
        if (ph == AGENTC_DUE_NONE || text.len == 0) {
            agentc_buf_free(&text);
            break;
        }
        AgcMsg *um = agentc_transcript_push(&a->tr, AGENTC_ROLE_USER);
        agentc_msg_add_text(um, (const char *)text.p, text.len);
        notify_observer(a, um);
        agent_emit_steered(a, ph == AGENTC_DUE_PREEMPT ? "preempt" : "boundary");
        agentc_buf_free(&text);
        applied = true;
    }
    return applied;
}

/* SIG §5.3: a preempt with AGENTC_Q_F_PREEMPT_TOOLS cancels the agent's
 * running tool-process actors graceful first; the actor tick escalates to force
 * once grace_ms passes. agentc_actor_cancel is idempotent at the same stage, so
 * a repeated preempt cannot re-emit ("once"). */
static void agent_cancel_child_tools(AgcAgent *a) {
    if (a->actor_id == 0)
        return;
    AgcActor rows[AGENTC_ACTOR_MAX];
    size_t n = agentc_actor_list(rows, AGENTC_ACTOR_MAX);
    for (size_t i = 0; i < n; i++) {
        if (rows[i].parent != a->actor_id || rows[i].kind != AGENTC_ACTOR_PROCESS)
            continue;
        (void)agentc_actor_cancel(rows[i].id, AGENTC_FREEZE_ACTOR, 1);
    }
}

/* Auto-start on settle (SIG §5.5): coalesce every due item (turn <= the next
 * turn) into the next turn's initial user message. Returns true when a new
 * turn should start. Called only from the settle tail when auto_start is on. */
static bool agent_autostart_due(AgcAgent *a) {
    if (a->actor_id == 0)
        return false;
    AgcBuf text = {0};
    int64_t next = agentc_actor_turn(a->actor_id) + 1;
    for (;;) {
        AgcBuf q = {0};
        uint32_t fl = 0;
        AgcDuePhase ph = agentc_queue_flush(a->actor_id, next, &q, &fl);
        if (ph == AGENTC_DUE_NONE || q.len == 0) {
            agentc_buf_free(&q);
            break;
        }
        if (text.len)
            agentc_buf_byte(&text, '\n');
        agentc_buf_push(&text, q.p, q.len);
        agentc_buf_free(&q);
    }
    if (text.len == 0) {
        agentc_buf_free(&text);
        return false;
    }
    AgcMsg *um = agentc_transcript_push(&a->tr, AGENTC_ROLE_USER);
    agentc_msg_add_text(um, (const char *)text.p, text.len);
    notify_observer(a, um);
    agentc_buf_free(&text);
    return true;
}

/* Current-agent seam: agentc_decide/hook_branch and the `agent` tool read the
 * agent inside the active submit so they inherit its provider/transport (R-D2).
 * The wrapper saves/restores around the loop so nested child submits
 * (a parent spawning a child) restore the parent on return. */
int agentc_agent_submit(AgcAgent *a, const char *text) {
    if (!a)
        return -22;
    AgcAgent *prev = g_current_agent;
    g_current_agent = a;
    int rc = agent_run_turn(a, text, false); /* foreground: never yields */
    g_current_agent = prev;
    return rc;
}

/* Cooperative one-step run (P5 multi-session concurrency). A front end that
 * owns several engines can start a run without blocking and advance each one by
 * one model step per loop iteration; the shared resumable scheduler yields at
 * the post-tool safe point exactly like a background agent actor. The run does
 * NOT set a->running, so agentc_actor_tick never touches it: the front end is
 * the only stepper and can rebind the process globals to the owning session
 * immediately before each slice. Foreground --mode rpc and the TUI stay on
 * agentc_agent_submit and are unchanged. */
int agentc_agent_run_begin(AgcAgent *a, const char *text) {
    if (!a || !a->prov)
        return -22; /* EINVAL */
    if (a->run.coop)
        return -16; /* EBUSY: already in flight */
    a->pending_input = agentc_strdup(text ? text : "");
    if (a->pending_input == NULL)
        return -12; /* ENOMEM */
    a->run.coop = true;
    a->run.started = false;
    a->run.resume_inner = false;
    a->cancel = false; /* clear any stale cancel; a pre-start abort sets it */
    return 0;
}

bool agentc_agent_run_active(const AgcAgent *a) {
    return a != NULL && a->run.coop;
}

int agentc_agent_run_step(AgcAgent *a) {
    if (a == NULL || !a->run.coop)
        return 0; /* nothing in flight */
    /* The first slice consumes the queued text; a resumed slice passes NULL and
     * continues the saved step (one model step per call). */
    char *text = a->pending_input;
    a->pending_input = NULL;
    AgcAgent *prev = g_current_agent;
    g_current_agent = a;
    int rc = agent_run_turn(a, text, true);
    g_current_agent = prev;
    agentc_free(text);
    if (rc == AGENTC_RUN_YIELD)
        return AGENTC_AGENT_RUN_YIELD;
    a->run.coop = false;
    return rc;
}

static int agent_run_turn(AgcAgent *a, const char *text, bool background) {
    if (!a || !a->prov)
        return -22; /* EINVAL */
    AgcRunState *rs = &a->run;
    int rc = 0;
    int terminal_stop = AGENTC_STOP_PENDING;
    int continuations = 0;
    bool have_transport = false;

    if (rs->started) {
        /* Resume a yielded background turn at the top of the inner step loop.
         * The shared turn setup already ran; a cancel/last_error delivered
         * between slices is left intact for this step to observe. */
        rc = rs->rc;
        terminal_stop = rs->terminal_stop;
        continuations = rs->continuations;
        have_transport = rs->have_transport;
        rs->resume_inner = true;
    } else {
        rs->started = true;
        /* A cooperative run cleared cancel in agentc_agent_run_begin; preserving
         * a pre-start abort here is what lets `cancel` win the tiny window
         * between acceptance and the first slice. Foreground submits keep the
         * historical reset. */
        if (!rs->coop)
            a->cancel = false;
        a->last_error[0] = 0;
        /* The token budget is per submitted turn: a new turn gets a fresh
         * allowance, so reaching the cap stops this turn instead of disabling
         * the agent for every later submit. */
        a->tokens_spent = 0;
        a->budget_notified = false;
        a->budget_hit = false;

        /* input override: the first decision of a run. A handled/blocked prompt
         * never emits agent_start/agent_end/agent_settled. The `input` stage
         * and the legacy `input` point share one dispatch (B8). */
        const char *eff_text = text ? text : "";
        char *input_owned = NULL;
        int input_rc = input_hook(a, eff_text, &input_owned);
        if (input_rc < 0) {
            /* A fail-closed input handler blocks the run visibly: the front ends
             * report last_error and exit nonzero instead of a silent no-run. */
            agentc_free(input_owned);
            set_error(a, "input blocked by extension");
            emit(a, AGENTC_EV_ERROR, a->last_error);
            rs->started = false;
            return input_rc;
        }
        if (input_rc > 0) {
            agentc_free(input_owned);
            rs->started = false;
            return 0;
        }
        if (input_owned)
            eff_text = input_owned;

        /* System prompt: reset the per-run override, then build/publish the
         * effective prompt; before_agent_start may override it for the run. */
        agentc_free(a->system_override);
        a->system_override = NULL;
        refresh_system(a);
        a->tr.model = a->model;
        a->tr.provider = a->prov->name;
        before_agent_start_hook(a, eff_text);
        emit(a, AGENTC_EV_AGENT_START, NULL);
        a->step_index = 0;

        have_transport = a->transport.request != NULL;

        if (!have_transport) {
            set_error(a, "no transport configured");
            emit(a, AGENTC_EV_ERROR, a->last_error);
            rc = -1;
            terminal_stop = AGENTC_STOP_ERROR;
        } else {
            AgcMsg *um = agentc_transcript_push(&a->tr, AGENTC_ROLE_USER);
            agentc_msg_add_text(um, eff_text, agentc_strlen(eff_text));
            notify_observer(a, um);
        }
        agentc_free(input_owned);
        input_owned = NULL;
    }

    /* The settle loop: an agent_before_settle `continue` re-enters the turn
     * loop for one more turn without a second agent_start. */
    for (;;) {
        /* On a resume the per-turn setup already ran; skip it and drop straight
         * into the next model step. */
        if (!rs->resume_inner && a->actor_id != 0) {
            agentc_queue_set_turn(a->actor_id, agentc_actor_turn(a->actor_id) + 1);
            a->settle = false;
            a->steer_preempt = false;
            a->steer_preempt_tools = false;
            a->steer_boundary = false;
        }
        rs->resume_inner = false;
        if (have_transport) {
            for (;;) {
                /* Token-budget soft cap (SURF §18 #23): enforce between model
                 * steps, after every tool call from the previous step has its
                 * answer, so no orphan `tool_call` is left. Emit `actor_notify`
                 * once per turn and settle; the last assistant is the output. */
                if (a->token_budget > 0 && a->tokens_spent >= (u64)a->token_budget) {
                    if (!a->budget_notified) {
                        a->budget_notified = true;
                        AgcActor *brow = a->actor_id ? agentc_actor_get(a->actor_id) : NULL;
                        if (brow != NULL) {
                            AgcBuf meta = {0};
                            agentc_buf_printf(&meta,
                                              "{\"reason\":\"token_budget\",\"budget\":%lld,"
                                              "\"spent\":%llu}",
                                              (long long)a->token_budget,
                                              (unsigned long long)a->tokens_spent);
                            actor_event(brow, AGENTC_EV_ACTOR_NOTIFY, "token budget reached",
                                        (const char *)meta.p, 0);
                            agentc_buf_free(&meta);
                        }
                    }
                    a->budget_hit = true;
                    rc = 0;
                    terminal_stop = AGENTC_STOP_STOP;
                    for (size_t bi = a->tr.n; bi > 0; bi--)
                        if (a->tr.msgs[bi - 1].role == AGENTC_ROLE_ASSISTANT) {
                            terminal_stop = a->tr.msgs[bi - 1].stop_reason;
                            break;
                        }
                    break;
                }
                refresh_system(a);
                /* W4: resolve the effective route for THIS model step right after
                 * refresh_system and before STEP_START. The alias stays on
                 * a->model (transcript identity); a failed resolution fails the step
                 * closed below, never silently falling back. */
                AgcRoute route;
                int route_rc = route_resolve(a, &route);
                if (route_rc == 0)
                    emit_model_route(a, &route);
                emit(a, AGENTC_EV_STEP_START, &a->step_index);
                if (route_rc == 0 && a->auto_compact && a->prov) {
                    const AgcModel *cm =
                        route.target ? route.target
                                     : agentc_model_find(route.prov->name, route.model);
                    u32 window = cm ? cm->ctx_window : 0;
                    if (window > a->compact_reserve &&
                        agentc_agent_context_tokens(a) > window - a->compact_reserve) {
                        a->nonsteerable = true;
                        (void)compact_now(a, true);
                        a->nonsteerable = false;
                    }
                }
                loop_safe_point(a, true);
                size_t asst_idx = a->tr.n;
                AgcMsg *asst = agentc_transcript_push(&a->tr, AGENTC_ROLE_ASSISTANT);
                emit(a, AGENTC_EV_MSG_START, asst);
                bool turn_ok = false;

                int attempts = a->max_attempts > 0 ? a->max_attempts : AGENT_DEFAULT_ATTEMPTS;
                for (int attempt = 0; attempt < attempts; attempt++) {
                    if (a->cancel)
                        break;
                    asst = &a->tr.msgs[asst_idx];
                    if (route_rc != 0) {
                        /* Fail closed before any request: a broken virtual route is
                         * a step error, not a silent fall back to a->prov/a->model. */
                        asst->stop_reason = AGENTC_STOP_ERROR;
                        break;
                    }
                    if (attempt > 0) {
                        agentc_msg_free(asst);
                        asst->role = AGENTC_ROLE_ASSISTANT;
                        asst->stop_reason = AGENTC_STOP_PENDING;
                        asst->ts_ms = (u64)(os_now_ns(OS_CLOCK_REALTIME) / 1000000);
                    }

                    AgcRequest r;
                    agentc_memset(&r, 0, sizeof r);
                    r.provider = route.prov->name;
                    r.model = route.model;
                    r.api_key = route.api_key;
                    r.base_url = route.base_url;
                    r.system = a->tr.system;
                    r.tools = a->tools;
                    r.ntools = a->ntools;
                    r.thinking_level = a->thinking;
                    r.max_tokens = a->max_tokens;
                    r.account_id = agentc_oauth_account_id(route.prov->name);
                    r.transcript = &a->tr;

                    AgcStreamState st;
                    if (agentc_stream_state_init(route.prov, &st) != 0) {
                        asst->stop_reason = AGENTC_STOP_ERROR;
                        set_error(a, "failed to initialize stream state");
                        break;
                    }
                    st.msg = asst;
                    AgcStreamResult res;
                    int trc = request_stream(a, route.prov, route.model, &r, true, &st, &res);
                    bool retry = false;

                    if (trc == REQ_BUILD_FAIL) {
                        asst->stop_reason = AGENTC_STOP_ERROR;
                        set_error(a, "failed to build request");
                        goto attempt_done;
                    }
                    if (trc == REQ_MALFORMED) {
                        asst->stop_reason = AGENTC_STOP_ERROR;
                        set_error(a, "malformed request from provider");
                        goto attempt_done;
                    }
                    if (trc == REQ_BLOCKED) {
                        /* An extension override blocked/failed the request:
                         * terminal, no retry and no tools. */
                        asst->stop_reason = AGENTC_STOP_ERROR;
                        set_error(a, "request blocked by extension");
                        goto attempt_done;
                    }

                    if (a->cancel) {
                        asst->stop_reason = AGENTC_STOP_ABORTED;
                        set_error(a, "aborted");
                        goto attempt_done;
                    }

                    if (res.map_failed && trc == 0) {
                        asst->stop_reason = AGENTC_STOP_ERROR;
                        set_error(a, st.error[0] ? st.error : "provider protocol error");
                        goto attempt_done;
                    }

                    if (trc == 0) {
                        bool pending = st.stop_reason == AGENTC_STOP_PENDING;
                        if (pending && !st.saw_stop) {
                            if (attempt + 1 < attempts) {
                                /* Wait first, then decide: an abort during the wait
                                 * settles the turn, so the abandoned draft stays on
                                 * screen instead of being discarded with no
                                 * replacement. */
                                backoff_sleep(a,
                                              agentc_retry_backoff_ms(attempt, res.retry_after_ms));
                                if (a->cancel)
                                    goto attempt_done;
                                /* The abandoned attempt already rendered content:
                                 * tell front ends to drop it before the re-run. */
                                if (attempt_streamed(asst))
                                    emit(a, AGENTC_EV_MSG_RESET, NULL);
                                retry = true;
                                goto attempt_done;
                            }
                            asst->stop_reason = AGENTC_STOP_ERROR;
                            set_error(a, "stream ended prematurely");
                            goto attempt_done;
                        }
                        int fr = route.prov->finish(&st);
                        if (st.error[0]) {
                            asst->stop_reason = AGENTC_STOP_ERROR;
                            set_error(a, st.error);
                        } else if (fr != 0) {
                            asst->stop_reason = AGENTC_STOP_ERROR;
                            set_error(a, "provider protocol error");
                        } else {
                            asst->stop_reason = st.stop_reason;
                        }
                        if (asst->stop_reason == AGENTC_STOP_ERROR)
                            goto attempt_done;
                        asst->usage.input = st.usage_input;
                        asst->usage.output = st.usage_output;
                        asst->usage.cache_read = st.usage_cache_read;
                        asst->usage.cache_write = st.usage_cache_write;
                        asst->usage.reasoning = st.usage_reasoning;
                        asst->usage.cost_micro = agentc_model_cost(
                            route.target ? route.target
                                         : agentc_model_find(route.prov->name, route.model),
                            &asst->usage);
                        /* Token budget (SURF §18 #23): accumulate the step's
                         * model tokens; the cap is enforced between steps. */
                        a->tokens_spent += (uint64_t)asst->usage.input + asst->usage.output +
                                           asst->usage.cache_read + asst->usage.cache_write;
                        a->last_error[0] = 0;
                        turn_ok = true;
                        goto attempt_done;
                    }

                    {
                        int code = trc;
                        if (code == 0)
                            code = res.last_status ? res.last_status : -5;
                        asst->stop_reason = AGENTC_STOP_ERROR;
                        if (st.error[0]) {
                            set_error(a, st.error);
                        } else if (code > 0) {
                            /* A non-2xx body is where the provider says what it objected to;
                             * the transport keeps a bounded, redacted excerpt of it. */
                            const char *excerpt = agentc_transport_http_last_body_excerpt();
                            char url[1200];
                            provider_url(route.base_url, route.prov, route.model, url, sizeof url);
                            if (excerpt && excerpt[0])
                                set_errorf(a, "http error %d from %s (%s): %s", code,
                                           route.prov->name, url, excerpt);
                            else
                                set_errorf(a, "http error %d from %s (%s)", code, route.prov->name,
                                           url);
                        } else {
                            set_errorf(a, "transport error %d", code);
                        }
                        if (agentc_retry_retryable(code) && attempt + 1 < attempts) {
                            backoff_sleep(a, agentc_retry_backoff_ms(attempt, res.retry_after_ms));
                            if (!a->cancel) {
                                if (attempt_streamed(asst))
                                    emit(a, AGENTC_EV_MSG_RESET, NULL);
                                retry = true;
                            }
                        }
                    }

                attempt_done:
                    agentc_stream_state_close(&st);
                    if (retry)
                        continue;
                    break;
                }

                asst = &a->tr.msgs[asst_idx];
                if (a->steer_preempt && !a->cancel) {
                    /* Preempt (SIG §5.4): abandon the in-flight step, drop the
                     * partial assistant (MSG_RESET already told front ends) and
                     * re-issue with the whole due set. */
                    if (a->steer_preempt_tools)
                        agent_cancel_child_tools(a);
                    a->steer_preempt_tools = false;
                    agentc_msg_free(asst);
                    a->tr.n = asst_idx;
                    a->steer_preempt = false;
                    continue;
                }
                if (a->cancel && asst->stop_reason != AGENTC_STOP_ERROR) {
                    asst->stop_reason = AGENTC_STOP_ABORTED;
                    set_error(a, "aborted");
                }
                /* One terminal decision for the turn: an abort or an error must
                 * stay terminal through the message_end hook, so a late override
                 * cannot turn it back into a serializable success. */
                bool terminal = !turn_ok || a->cancel || asst->stop_reason == AGENTC_STOP_ERROR ||
                                asst->stop_reason == AGENTC_STOP_ABORTED;
                if (!turn_ok) {
                    if (asst->stop_reason != AGENTC_STOP_ERROR &&
                        asst->stop_reason != AGENTC_STOP_ABORTED) {
                        asst->stop_reason = AGENTC_STOP_ERROR;
                        if (!a->last_error[0])
                            set_error(a, "request failed");
                    }
                    agentc_free(asst->error);
                    asst->error = agentc_strdup(a->last_error);
                    /* A failed/aborted assistant is neither serialized nor persisted,
                     * so its tool calls must stay unanswered: synthesizing results here
                     * would persist a `tool` result with no matching `tool_call`. */
                    asst = &a->tr.msgs[asst_idx];
                    message_end_hook(asst, terminal);
                    emit(a, AGENTC_EV_MSG_END, asst);
                    notify_observer(a, asst);
                    /* hard exit: entries apply, `continue` does not */
                    (void)step_end_hook(a, asst->stop_reason, true);
                    emit(a, AGENTC_EV_STEP_END, NULL);
                    emit(a, AGENTC_EV_ERROR, a->last_error);
                    rc = -1;
                    terminal_stop = asst->stop_reason;
                    break;
                }

                message_end_hook(asst, terminal);
                emit(a, AGENTC_EV_MSG_END, asst);
                notify_observer(a, asst);
                (void)loop_safe_point(a, false);
                if (a->settle && !a->cancel) {
                    asst = &a->tr.msgs[asst_idx];
                    bool hard = true;
                    (void)step_end_hook(a, asst->stop_reason, hard);
                    emit(a, AGENTC_EV_STEP_END, NULL);
                    rc = 0;
                    terminal_stop = asst->stop_reason;
                    break;
                }

                size_t ntc = agentc_msg_count_tool_calls(asst);
                if (ntc == 0) {
                    bool hard = a->cancel || asst->stop_reason == AGENTC_STOP_ERROR ||
                                asst->stop_reason == AGENTC_STOP_ABORTED;
                    bool cont = step_end_hook(a, asst->stop_reason, hard);
                    emit(a, AGENTC_EV_STEP_END, NULL);
                    if (a->cancel) {
                        set_error(a, "aborted");
                        emit(a, AGENTC_EV_ERROR, a->last_error);
                        rc = -1;
                        terminal_stop = AGENTC_STOP_ABORTED;
                        break;
                    }
                    if (cont && continuation_take(&continuations)) {
                        /* A background run yields one step per actor tick even
                         * when the continuation carries no tools: a no-tool
                         * turn_end.continue must not coalesce a whole extra
                         * model step into the same slice. A foreground submit
                         * never yields, so its transcript sequence (and every
                         * byte-exact golden) is unchanged. */
                        if (background) {
                            rs->rc = rc;
                            rs->terminal_stop = terminal_stop;
                            rs->continuations = continuations;
                            rs->have_transport = have_transport;
                            rs->resume_inner = true;
                            return AGENTC_RUN_YIELD;
                        }
                        continue;
                    }
                    rc = 0;
                    terminal_stop = asst->stop_reason;
                    break;
                }

                bool wants_tools = turn_ok && asst->stop_reason == AGENTC_STOP_TOOLUSE;
                if (!wants_tools) {
                    /* Only answer the calls when this assistant will be serialized and
                     * persisted. If an abort landed after the stream completed, the
                     * turn is ABORTED and skipped, so answering it would persist an
                     * orphan tool result. */
                    if (agentc_provider_msg_serializable(asst)) {
                        const char *why = asst->stop_reason == AGENTC_STOP_ABORTED
                                              ? "error: aborted before execution"
                                              : "error: response ended before this call could run";
                        close_tool_calls(a, asst_idx, why);
                        /* close_tool_calls() pushes tool results, which can
                         * realloc a->tr.msgs and invalidate `asst`. */
                        asst = &a->tr.msgs[asst_idx];
                    }
                    bool hard = a->cancel || asst->stop_reason == AGENTC_STOP_ERROR ||
                                asst->stop_reason == AGENTC_STOP_ABORTED;
                    bool cont = step_end_hook(a, asst->stop_reason, hard);
                    emit(a, AGENTC_EV_STEP_END, NULL);
                    if (a->cancel) {
                        set_error(a, "aborted");
                        emit(a, AGENTC_EV_ERROR, a->last_error);
                        rc = -1;
                        terminal_stop = AGENTC_STOP_ABORTED;
                        break;
                    }
                    /* A non-hard turn (e.g. a max_tokens truncation that still
                     * carries a tool_use block) honors `continue` through the
                     * same shared cap as the no-tool-calls path; otherwise the
                     * run falls through to settle as before. */
                    if (cont && continuation_take(&continuations))
                        continue;
                    rc = 0;
                    terminal_stop = asst->stop_reason;
                    break;
                }

                const AgcTool *job_tools[AGENTC_TOOL_JOBS_MAX];
                AgcToolCall job_calls[AGENTC_TOOL_JOBS_MAX];
                const char *pre_errors[AGENTC_TOOL_JOBS_MAX];
                char pe_buf[AGENTC_TOOL_JOBS_MAX][256];
                ToolSlot *slots = agentc_alloc(ntc * sizeof *slots);
                JobCtx jctx;
                agentc_memset(&jctx, 0, sizeof jctx);
                jctx.a = a;
                jctx.slots = slots;

                /* pi-exact terminate: count blocked calls that asked
                 * to end the submission. Only a batch whose every call was
                 * blocked with terminate ends it; any normal/unknown/rewritten
                 * call keeps the loop going. */
                size_t n_terminate = 0;
                for (size_t off = 0; off < ntc; off += AGENTC_TOOL_JOBS_MAX) {
                    size_t batch = ntc - off;
                    if (batch > AGENTC_TOOL_JOBS_MAX)
                        batch = AGENTC_TOOL_JOBS_MAX;
                    asst = &a->tr.msgs[asst_idx];
                    char *args_owned[AGENTC_TOOL_JOBS_MAX];
                    for (size_t k = 0; k < batch; k++) {
                        AgcBlock *tc = agentc_msg_nth_tool_call(asst, off + k);
                        const char *args =
                            (tc && tc->tool_args && tc->tool_args[0]) ? tc->tool_args : "{}";
                        const char *eff_name = tc ? tc->tool_name : NULL;
                        args_owned[k] = NULL;
                        pre_errors[k] = NULL;
                        job_tools[k] = tc ? find_tool(a, eff_name) : NULL;
                        /* P6-D: the tool `call` stage is the decision seam. A
                         * handler may consume the call (deny), narrow its args,
                         * or replace the tool (verdict=replace). The returned
                         * message's meta is the effective tool name and its body
                         * the effective args. */
                        if (tc != NULL && job_tools[k] != NULL &&
                            tool_hooks_enabled(a, job_tools[k]) &&
                            agentc_hook_wants(AGENTC_HOOK_STAGE_TOOL)) {
                            AgcMessage hm;
                            agentc_memset(&hm, 0, sizeof hm);
                            hm.class = AGENTC_MSG_CLASS_HOOK;
                            hm.kind = AGENTC_MSG_INPUT;
                            hm.actor_id = a->actor_id;
                            hm.meta = "call";
                            hm.body = args;
                            hm.body_len = agentc_strlen(args);
                            hm.id = ++g_tool_hook_seq;
                            AgcMessage *hnext = NULL;
                            int hrc = agentc_hook_run(AGENTC_HOOK_STAGE_TOOL, &hm, &hnext);
                            if (hrc < 0 || hnext == NULL) {
                                agentc_snprintf(pe_buf[k], sizeof pe_buf[k],
                                                "error: tool blocked: decision_denied");
                                pre_errors[k] = pe_buf[k];
                            } else {
                                if (hnext->body != NULL && hnext->body != args) {
                                    agentc_free(args_owned[k]);
                                    args_owned[k] = agentc_strdup(hnext->body);
                                    args = args_owned[k];
                                }
                                if (hnext->meta != NULL)
                                    eff_name = hnext->meta;
                                job_tools[k] = find_tool(a, eff_name);
                                if (job_tools[k] == NULL) {
                                    agentc_snprintf(pe_buf[k], sizeof pe_buf[k],
                                                    "error: tool blocked: unknown tool");
                                    pre_errors[k] = pe_buf[k];
                                }
                            }
                        }
                        job_calls[k].call_id = tc ? tc->tool_id : NULL;
                        job_calls[k].name = eff_name;
                        job_calls[k].args_json = args;
                        job_calls[k].cancel = &a->cancel;
                        job_calls[k].env = a->trust;
                        job_calls[k].actor_id = a->actor_id;
                        /* I1: carry the resolved tool's capability flags so the
                         * trust layer classifies from the real tool (0 = fail
                         * closed mutating). */
                        job_calls[k].flags = job_tools[k] != NULL ? job_tools[k]->flags : 0;
                        /* P6-C Layer 2: the trust envelope is authoritative and
                         * runs before the legacy veto. A DENY never reaches a
                         * delegate; an ALLOW with narrowed args replaces the call
                         * exactly like the legacy veto. */
                        if (tc && job_tools[k] != NULL && pre_errors[k] == NULL) {
                            AgcTrustDecision tdec;
                            agentc_memset(&tdec, 0, sizeof tdec);
                            bool tdec_owned = true;
                            if (job_tools[k]->flags & AGENTC_TOOL_NO_APPROVAL) {
                                /* Finding 6 (VC1b): the flag opts out of only
                                 * the approval dispatch; the envelope and the
                                 * process policy still run. */
                                (void)agentc_trust_decide_skip_approval(
                                    a->trust, eff_name, job_tools[k]->flags, args, &tdec);
                            } else {
                                (void)agentc_trust_gate(a->trust, &job_calls[k], &tdec);
                            }
                            /* VCF1-2: only an ALLOW dispatches the tool. The
                             * approval contract normalizes a non-terminal
                             * approver verdict to DENY, and the gate can still
                             * surface a memoized ASK; blocking anything that is
                             * not ALLOW is defense-in-depth so no non-terminal
                             * verdict ever falls through to the tool. */
                            if (tdec.decision != AGENTC_DEC_ALLOW) {
                                agentc_snprintf(pe_buf[k], sizeof pe_buf[k],
                                                "error: tool blocked: trust_denied: %s",
                                                tdec.reason ? tdec.reason : "denied");
                                pre_errors[k] = pe_buf[k];
                                /* a blocked verdict may still own narrowed args
                                 * (an approver deny/ask carrying args); free it */
                                agentc_free((void *)tdec.args_json);
                            } else if (tdec.args_json) {
                                agentc_free(args_owned[k]);
                                args_owned[k] = (char *)tdec.args_json;
                                job_calls[k].args_json = tdec.args_json;
                            }
                            /* `decide` borrows its reason; only the gate owns it. */
                            if (tdec_owned)
                                agentc_free((void *)tdec.reason);
                        }
                        if (tc && job_tools[k] != NULL && pre_errors[k] == NULL && a->veto &&
                            tool_hooks_enabled(a, job_tools[k])) {
                            AgcToolVetoDecision dec;
                            agentc_memset(&dec, 0, sizeof dec);
                            a->veto(a->veto_ud, tc->tool_id, eff_name, args, &dec);
                            if (dec.block) {
                                if (dec.terminate)
                                    n_terminate++;
                                agentc_snprintf(pe_buf[k], sizeof pe_buf[k],
                                                "error: tool blocked%s%s", dec.reason ? ": " : "",
                                                dec.reason ? dec.reason : "");
                                pre_errors[k] = pe_buf[k];
                                agentc_free(dec.args_json);
                            } else if (dec.args_json) {
                                agentc_free(args_owned[k]);
                                args_owned[k] = dec.args_json;
                                job_calls[k].args_json = dec.args_json;
                            }
                            agentc_free(dec.reason);
                        }
                        /* C2: remember the final resolved tool on the slot so
                         * the source-ordered loop below reads it instead of the
                         * per-chunk `job_tools` array. */
                        slots[off + k].tool = job_tools[k];
                    }
                    jctx.base = off;
                    (void)agentc_tool_jobs_run(job_tools, pre_errors, job_calls, batch, on_tool_job,
                                               &jctx);
                    /* The veto's replacement args are core-owned and the driver hands
                     * them to the tools; they must outlive the whole batch. The abort
                     * path still returns here, so this free is unconditional. */
                    for (size_t k = 0; k < batch; k++)
                        agentc_free(args_owned[k]);
                }

                /* Answers go into the transcript in source order; the driver has
                 * already emitted the start/end events (completion order) and freed
                 * its own buffers. The tool_result override runs before both the
                 * append and the observer so its replacement is not dropped. */
                for (size_t t = 0; t < ntc; t++) {
                    const char *res = slots[t].out.p != NULL ? (const char *)slots[t].out.p : "";
                    size_t res_len = slots[t].out.len;
                    asst = &a->tr.msgs[asst_idx];
                    AgcBlock *tc = agentc_msg_nth_tool_call(asst, t);
                    if (!tc)
                        break;
                    bool is_error = slots[t].is_error;
                    char *hooked =
                        tool_hooks_enabled(a, slots[t].tool)
                            ? tool_result_hook(tc->tool_id, tc->tool_name, res, &is_error)
                            : NULL;
                    const char *eff_res = hooked ? hooked : res;
                    /* The hook's replacement is a C string; only the raw tool
                     * result can carry embedded NUL bytes, so it keeps the
                     * real buffer length instead of strlen's first-NUL cut. */
                    size_t eff_len = hooked ? agentc_strlen(hooked) : res_len;
                    AgcMsg *tm = agentc_transcript_push(&a->tr, AGENTC_ROLE_TOOL);
                    agentc_msg_add_tool_result_n(tm, tc->tool_id, tc->tool_name, eff_res, eff_len);
                    /* the session serializer reads error to write is_error, so it must
                     * be set before the observer persists the message */
                    if (is_error)
                        tm->error = agentc_strdup("error");
                    notify_observer(a, tm);
                    agentc_free(hooked);
                }
                for (size_t t = 0; t < ntc; t++)
                    agentc_buf_free(&slots[t].out);
                agentc_free(slots);
                loop_safe_point(a, true);
                if (a->settle && !a->cancel) {
                    recompose_now(a);
                    asst = &a->tr.msgs[asst_idx];
                    (void)step_end_hook(a, asst->stop_reason, true);
                    emit(a, AGENTC_EV_STEP_END, NULL);
                    rc = 0;
                    terminal_stop = asst->stop_reason;
                    break;
                }
                /* The tool batch has fully returned: it is now safe to swap the tool
                 * table (no live AgcJob.tool pointers) for the next provider request. */
                recompose_now(a);
                asst = &a->tr.msgs[asst_idx];
                bool cont = step_end_hook(a, asst->stop_reason, a->cancel);
                emit(a, AGENTC_EV_STEP_END, NULL);

                if (a->cancel) {
                    set_error(a, "aborted");
                    emit(a, AGENTC_EV_ERROR, a->last_error);
                    rc = -1;
                    terminal_stop = AGENTC_STOP_ABORTED;
                    break;
                }
                /* An all-terminate batch ends the submission here (settle
                 * tail; agent_end keeps stop_reason "tool_use"). A
                 * turn_end.continue still re-enters the loop once under the
                 * shared continuation cap. */
                if (n_terminate == ntc && !(cont && continuation_take(&continuations))) {
                    rc = 0;
                    terminal_stop = asst->stop_reason;
                    break;
                }
                /* Slice boundary (R-D12 / d-plan §3.4): a background run yields
                 * here, after the whole tool batch has returned and before the
                 * next provider request, so the runtime pump can advance
                 * siblings. Foreground submits never take this branch. */
                if (background) {
                    rs->rc = rc;
                    rs->terminal_stop = terminal_stop;
                    rs->continuations = continuations;
                    rs->have_transport = have_transport;
                    rs->resume_inner = true;
                    return AGENTC_RUN_YIELD;
                }
                /* next turn */
            }
        }

        /* ---- one finish tail for every terminal exit: recompose, the typed
         * agent_end (stop_reason enriched), agent_before_settle, and, once and
         * only once per submit, agent_settled. */
        recompose_now(a);
        emit(a, AGENTC_EV_AGENT_END, &terminal_stop);
        const char *outcome = (rc == 0 && !a->cancel) ? "completed"
                              : a->cancel             ? "aborted"
                                                      : "error";
        if (!a->budget_hit && settle_hook(outcome) && rc == 0 && !a->cancel &&
            continuation_take(&continuations)) {
            terminal_stop = AGENTC_STOP_PENDING;
            continue;
        }
        /* Auto-start: a settled turn with due queue items starts the next turn
         * (SIG §5.5). Gated on the app-configured auto_start, so an unwired
         * agent stays byte-identical. A budget-hit run does not auto-continue:
         * the next turn (if any) starts fresh with the next submit. */
        if (!a->budget_hit && a->auto_start && rc == 0 && !a->cancel &&
            continuation_take(&continuations) && agent_autostart_due(a)) {
            terminal_stop = AGENTC_STOP_PENDING;
            continue;
        }
        if (want_observe("agent_settled")) {
            AgcExtResult sr = agentc_ext_emit("agent_settled", "{}");
            agentc_free(sr.result_json);
        }
        break;
    }
    rs->started = false;
    rs->resume_inner = false;
    return rc;
}
