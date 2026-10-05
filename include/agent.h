/* agent.h — messages, tools, providers and the agent loop.
 *
 * Ownership: everything created by these calls is owned by the transcript or
 * agent and released by the corresponding free function. Strings are NUL-
 * terminated, allocated with agentc_alloc. No libc.
 */
#ifndef AGENTC_AGENT_H
#define AGENTC_AGENT_H

#include "agentc.h"
#include "wire.h"

/* -------------------------------------------------------------- messages */
enum {
    AGENTC_ROLE_SYSTEM = 0,
    AGENTC_ROLE_USER,
    AGENTC_ROLE_ASSISTANT,
    AGENTC_ROLE_TOOL,
};

enum {
    AGENTC_BLK_TEXT = 0,
    AGENTC_BLK_THINK,
    AGENTC_BLK_TOOLCALL,
};

enum {
    AGENTC_STOP_PENDING = 0,
    AGENTC_STOP_STOP,
    AGENTC_STOP_LENGTH,
    AGENTC_STOP_TOOLUSE,
    AGENTC_STOP_ERROR,
    AGENTC_STOP_ABORTED,
};

typedef struct {
    u32 input, output, cache_read, cache_write, reasoning;
    i64 cost_micro; /* micro-USD, integer */
} AgcUsage;

typedef struct {
    int type;   /* AGENTC_BLK_* */
    char *text; /* AGENTC_BLK_TEXT/THINK body (owned) */
    size_t text_len;
    char *tool_id; /* AGENTC_BLK_TOOLCALL (owned) */
    char *tool_name;
    char *tool_args; /* raw JSON text (owned) */
} AgcBlock;

typedef struct {
    int role; /* AGENTC_ROLE_* */
    AgcBlock *blocks;
    size_t nblocks, blocks_cap;
    AgcUsage usage;
    int stop_reason; /* AGENTC_STOP_* */
    char *error;     /* owned or NULL */
    u64 ts_ms;
    /* Appended P6-D: lineage stamped by the child constructor / agent tool so
     * the v1 `message` record can replay the tree (P6-SUBAGENTS §8). 0 on the
     * root and on legacy messages. */
    uint32_t agent_id, parent_id;
} AgcMsg;

typedef struct {
    AgcMsg *msgs;
    size_t n, cap;
    const char *model;    /* borrowed */
    const char *provider; /* borrowed */
    const char *system;   /* owned system prompt snapshot */
} AgcTranscript;

void agentc_transcript_init(AgcTranscript *t);
void agentc_transcript_free(AgcTranscript *t);
void agentc_msg_free(AgcMsg *m);
AgcMsg *agentc_transcript_push(AgcTranscript *t, int role); /* zeroed message */
void agentc_msg_add_text(AgcMsg *m, const char *text, size_t n);
void agentc_msg_add_tool_call(AgcMsg *m, const char *id, const char *name);
void agentc_msg_tool_args_append(AgcMsg *m, const char *p, size_t n);
/* Length-aware tool-result append. `result` need not be NUL terminated and may
 * contain NUL bytes; exactly `len` bytes are stored, and an embedded NUL is
 * preserved to the transcript (the JSON writer escapes it as \u0000). */
void agentc_msg_add_tool_result_n(AgcMsg *m, const char *call_id, const char *name,
                                  const char *result, size_t len);

/* ------------------------------------------------------------ tool calls */
/* A tool executes synchronously and returns a result string (owned by the
 * caller) or NULL with *is_error set. Tools may poll and should return early
 * when the loop sets *cancel. */
typedef struct AgcTool AgcTool;

/* Job slot owned by the driver in src/core/tools/jobs.h; callers only see the
 * opaque handle the start/step callbacks receive. */
typedef struct AgcJob AgcJob;

/* One tool invocation. The strings stay owned by the caller (the transcript)
 * and are valid for the whole batch; `cancel` points at the turn's abort flag. */
typedef struct {
    const char *call_id;
    const char *name;
    const char *args_json;
    const volatile bool *cancel;
    /* Appended P6-C: the effective envelope for the call, borrowed. NULL means
     * unconfined (legacy callers and P6-A goldens stay green). */
    const struct AgcTrustEnv *env;
    /* Appended P6-D: the calling agent actor, so the op-based `agent` tool can
     * resolve its parent without a process-global (P6-SURFACE §7). */
    uint32_t actor_id;
    /* Appended I1 (review-hardening WS1): the resolved tool's AgcTool.flags, so
     * the trust layer classifies capability from the tool that will actually
     * run instead of hardcoded names. Zero/unknown means "mutating" (fail
     * closed). Every caller that resolves a tool must set it from that tool. */
    uint32_t flags;
} AgcToolCall;

struct AgcTool {
    const char *name;
    const char *label;
    const char *desc;
    const char *params_json; /* JSON Schema object, embedded string */
    u32 flags;               /* AGENTC_TOOL_* */
    char *(*exec)(const char *args_json, bool *is_error, const volatile bool *cancel);

    /* Appended v2 tool interface. Every existing 6-field positional initializer
     * above stays valid: C zero-fills the appended fields, and `exec` remains
     * the v1 entry point the job driver wraps when `run`/`start` are NULL. New
     * tools implement `run` (synchronous) or `start`/`step` (async) and leave
     * `exec` NULL. `ud` is the tool-instance identity (NULL for builtins), so a
     * extension or MCP adapter no longer needs one trampoline per registration. A
     * `timeout_ms` of 0 selects AGENTC_TOOL_TIMEOUT_DEF_MS. The job driver
     * (src/core/tools/jobs.h) owns the AgcJob it passes to start/step. */
    void *ud;       /* instance identity; NULL for builtins */
    int timeout_ms; /* 0 = AGENTC_TOOL_TIMEOUT_DEF_MS (120000) */
    int (*run)(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
               bool *is_error); /* 0 or -errno */
    int (*start)(const AgcTool *self, const AgcToolCall *call, AgcJob *job);
    int (*step)(const AgcTool *self, AgcJob *job); /* 0 running, 1 done, <0 fatal */
};

#define AGENTC_TOOL_READONLY 0x1u
#define AGENTC_TOOL_DESTRUCTIVE 0x2u
#define AGENTC_TOOL_SEQUENTIAL 0x4u
/* Registered and executable, but not declared to the model: hidden tools are
 * omitted from the model-facing # Tools prompt section and (from the provider
 * stage on) from request serialization. Tools the app or another tool invokes
 * directly set this bit. */
#define AGENTC_TOOL_HIDDEN 0x8u
/* Appended P6-B (P6-HOOKS §4): opt a tool (or a whole run via tools.hooks=off)
 * out of the hook pipeline; NO_APPROVAL opts out of only the approval branch. */
#define AGENTC_TOOL_NO_HOOKS 0x20u
#define AGENTC_TOOL_NO_APPROVAL 0x40u
/* Core-internal (never crosses the extension ABI): the tool is registered by
 * the core itself (the builtin-tools extension). The app's default selection
 * hides non-core AGENTC_TOOL_DESTRUCTIVE tools unless they are named in
 * --tools/default_tools; core builtins stay eligible. */
#define AGENTC_TOOL_CORE 0x10u
#define AGENTC_TOOL_TIMEOUT_DEF_MS 120000
#define AGENTC_TOOL_JOBS_MAX 32

size_t agentc_tools_builtin(AgcTool *out, size_t max); /* read, bash, edit, write, ls, find, grep */

/* ----------------------------------------------------------- tool helpers */
/* All return owned strings (agentc_free) unless noted. */
char *agentc_tool_read(const char *path, i64 offset, i64 limit, bool *is_error);
char *agentc_tool_write(const char *path, const char *content, bool *is_error);
/* Length-explicit form of agentc_tool_write: `content` need not be NUL
 * terminated and may contain NUL bytes, and exactly `len` bytes are written.
 * agentc_tool_write() is the strlen convenience wrapper. */
char *agentc_tool_write_len(const char *path, const char *content, size_t len, bool *is_error);
char *agentc_tool_edit(const char *path, const char *edits_json, bool *is_error);
char *agentc_tool_bash(const char *command, i64 timeout_ms, const volatile bool *cancel,
                       bool *is_error);

/* -------------------------------------------------------------- providers */
typedef struct {
    const char *provider;            /* e.g. "openai" */
    const char *model;               /* "claude-sonnet-4-5" */
    const char *api_key;             /* borrowed */
    const char *base_url;            /* optional override */
    const char *system;              /* system prompt */
    const AgcTranscript *transcript; /* messages to serialize (borrowed) */
    const AgcTool *tools;
    size_t ntools;
    int thinking_level;     /* 0=off .. 4=high */
    i64 max_tokens;         /* 0 = model default */
    const char *account_id; /* OAuth account id (ChatGPT backend), optional */
} AgcRequest;

/* Stream state filled by the provider while SSE events arrive. */
typedef struct {
    AgcMsg *msg;       /* assistant message being built */
    AgcBuf args;       /* deprecated: adapter args accumulate in `priv` */
    size_t tool_index; /* deprecated: adapter index lives in `priv` */
    u32 usage_input, usage_output, usage_cache_read, usage_cache_write;
    u32 usage_reasoning; /* subset of output, when the provider reports it */
    char response_id[128];
    char error[512];
    int stop_reason;
    bool saw_stop;
    AgcJsonArena *arena; /* per-stream JSON arena; owned by the agent loop */
    void *priv;          /* adapter stream state, via ops->stream_open */
} AgcStreamState;

typedef struct AgcProvider {
    const char *name; /* "openai", "anthropic" */
    const char *api;  /* "anthropic-messages", "openai-chat" */
    const char *default_base_url;
    /* Build the full request into out: provider headers, the blank line, then the
     * JSON body (the loop splits head and body at "\r\n\r\n"). The request path is
     * the provider row's `path` (see prov/provider.h); both host and path args are
     * informational for the Host header. Returns 0 or -errno. */
    int (*build_request)(AgcBuf *out, const AgcRequest *r, const char *url_host,
                         const char *url_path);
    /* Map one SSE event into the stream state; return 0 or -1 on protocol error. */
    int (*map_sse)(AgcStreamState *st, const AgcSseEvent *ev);
    /* Called once after the stream ends cleanly. */
    int (*finish)(AgcStreamState *st);
} AgcProvider;

const AgcProvider *agentc_prov_anthropic(void);
const AgcProvider *agentc_prov_openai(void);
/* ChatGPT subscription backend (Codex Responses API); same provider name, used
 * when the credential is an OAuth one. */
const AgcProvider *agentc_prov_openai_codex(void); /* chat completions */
/* Ollama speaks the OpenAI chat API: local at 127.0.0.1:11434 (no key needed)
 * and Ollama Cloud at ollama.com (Bearer key). */
const AgcProvider *agentc_prov_ollama(void); /* local ollama */
const AgcProvider *agentc_prov_ollama_cloud(void);
/* Native Google Gemini (Generative Language API): generateContent with
 * systemInstruction, functionCall/functionResponse tools and thinking budgets. */
const AgcProvider *agentc_prov_google(void);
/* Any other OpenAI-compatible endpoint (OpenRouter, xAI, Gemini's compat
 * endpoint, a self-hosted gateway, ...). The strings are copied; the returned
 * provider is stable for the process lifetime. */
const AgcProvider *agentc_prov_openai_compatible(const char *name, const char *base_url);
/* A config-declared user gateway (providers.<id>.base_url). Same OpenAI-chat row
 * as agentc_prov_openai_compatible, but it does not require a credential: a
 * local gateway may accept unauthenticated requests. Only ids with a configured
 * base URL are materialized this way. */
const AgcProvider *agentc_prov_openai_compatible_gateway(const char *name, const char *base_url);

/* ----------------------------------------------------------- model catalog */
typedef struct {
    const char *provider;
    const char *id;
    const char *api;
    const char *base_url;
    u32 ctx_window;
    u32 max_tokens;
    bool reasoning;
    bool image;
    /* AGENTC_MODEL_STATIC marks a model contributed by an
     * extension provider. Static rows survive agentc_model_clear_dynamic(); a
     * dynamic re-registration of the same id updates the numeric fields but
     * never clears the bit. */
    u32 flags;
    /* Appended (cost engine): rates in micro-USD per million tokens plus a
     * flag word. AGENTC_MODEL_RATE_KNOWN means the rate fields are meaningful;
     * AGENTC_MODEL_INPUT_INCLUDES_CACHE says usage.input already counts the
     * cache-read tokens (OpenAI prompt_tokens), so cache reads must be
     * subtracted before billing the fresh input. An unknown rate makes
     * agentc_model_cost() return -1. */
    i64 in_rate, out_rate;
    i64 cache_read_rate, cache_write_rate;
    u32 rate_flags;
    /* Appended W4 (virtual models): AGENTC_MODEL_VIRTUAL marks an alias row.
     * `route_provider`/`route_model` are the authoritative concrete target (both
     * owned); `route` is the owned display string "provider/model". NULL for a
     * real catalog/discovered row. The alias itself is `id` and the row is
     * provider-independent (provider == id). */
    const char *route;
    const char *route_provider;
    const char *route_model;
} AgcModel;

#define AGENTC_MODEL_STATIC 0x1u
#define AGENTC_MODEL_RATE_KNOWN 0x1u
#define AGENTC_MODEL_INPUT_INCLUDES_CACHE 0x2u
/* Appended W4: a virtual-model alias row (see agentc_model_register_virtual). */
#define AGENTC_MODEL_VIRTUAL 0x2u

const AgcModel *agentc_model_find(const char *provider, const char *id);
/* Fill up to `max` catalog entries into `out` (which may be NULL). With
 * out != NULL returns the number actually written (<= max); with out == NULL
 * returns the total number of entries, so a caller can size-count first. */
size_t agentc_model_all(const AgcModel **out, size_t max);
/* Runtime catalog entries (from provider model discovery). The strings are
 * owned by the registry; registering the same provider+id replaces it. */
void agentc_model_register_dynamic(const char *provider, const char *id, const char *api,
                                   const char *base_url, u32 ctx_window, u32 max_tokens,
                                   bool reasoning, bool image);
/* Static registration (extension providers): same slot table, but the row is
 * marked AGENTC_MODEL_STATIC so agentc_model_clear_dynamic() keeps it. Updates
 * an existing provider+id row in place (keeping the static bit). */
void agentc_model_register_static(const char *provider, const char *id, const char *api,
                                  const char *base_url, u32 ctx_window, u32 max_tokens,
                                  bool reasoning, bool image);
/* Appended W4: register a virtual-model alias. The alias `id` resolves per
 * model step to the concrete {provider, model} target carried authoritatively
 * in the row's appended `route_provider`/`route_model` fields (marked
 * AGENTC_MODEL_VIRTUAL; the appended `route` is the owned "provider/model"
 * display string only, never parsed). The alias
 * is provider-independent (its row's provider == alias) and must NOT shadow a
 * catalog or non-virtual dynamic model id: if one exists the real model keeps
 * the id and the alias is refused (first-wins). A malformed/empty alias, missing
 * provider/model, or an over-long alias is ignored. The strings are copied; the
 * route is stable for the process lifetime. */
/* Appended W4: drop every virtual-model alias row (used by agentc_model_reset
 * and by agentc_config_load before it republishes the loaded aliases, so a
 * removed `models.<alias>` does not linger for the process lifetime). */
void agentc_model_clear_virtual(void);

void agentc_model_register_virtual(const char *alias, const char *provider, const char *model);
/* The virtual row for `id`, or NULL. */
const AgcModel *agentc_model_find_virtual(const char *id);
void agentc_model_clear_dynamic(const char *provider); /* NULL clears everything */
/* The runtime discovered/static model table capacity (slots). */
size_t agentc_model_capacity(void);
/* Drop the static rows registered for `provider` (NULL clears every static
 * row). Used by the extension registry when provider records are freed; the
 * dynamic clear never touches static rows. */
void agentc_model_clear_static(const char *provider);
/* Full reset for the SDK one-engine contract: drop every discovered row and
 * every pricing override, returning the runtime catalog to its pristine
 * baseline. Static catalog rows are const and stay. */
void agentc_model_reset(void);
size_t agentc_model_dynamic_count(void);
bool agentc_model_is_dynamic(const AgcModel *m);
bool agentc_model_is_static(const AgcModel *m);
/* cost in micro-USD for a usage record, or -1 when the model's rate is unknown */
i64 agentc_model_cost(const AgcModel *m, const AgcUsage *u);

/* Cost-engine plumbing. `agentc_rate_parse_scaled` parses a
 * non-negative decimal scalar and returns value*scale (scale a power of ten),
 * or -1 on malformed input. Discovered pricing is staged by the discovery
 * parser and consumed by agentc_model_register_dynamic(); a local pricing
 * override (pricing.jsonc) is recorded here and consulted before any model
 * field, so a hand-set rate always wins. */
i64 agentc_rate_parse_scaled(const char *text, i64 scale);
void agentc_model_stage_rate(const char *provider, const char *id, i64 in_rate, i64 out_rate,
                             i64 cache_read_rate, i64 cache_write_rate, u32 rate_flags);
void agentc_model_set_rate_override(const char *provider, const char *id, i64 in_rate, i64 out_rate,
                                    i64 cache_read_rate, i64 cache_write_rate, u32 rate_flags);

/* --------------------------------------------------------------- transport */
/* Injected I/O: real runs use the wire layer; tests replay canned bytes.
 * Return: 0 on success, a positive HTTP status for status errors (429/5xx are
 * retried by the loop), or a negative errno / -ECANCELED on transport failure.
 * on_chunk returns 0 to continue, nonzero to abort (surfaced as -ECANCELED). */
typedef int (*AgcTransportFn)(void *ud, const char *url, const char *headers, const void *body,
                              size_t body_len, int (*on_chunk)(void *u, const void *p, size_t n),
                              void *u, int timeout_ms, AgcBuf *record);
typedef struct {
    AgcTransportFn request;
    void *ud;
    /* Appended: borrowed JSON object of the last response's headers, "{}" when
     * the transport has none, or NULL when the transport cannot expose them.
     * The string is owned by the transport and valid until the next call. */
    const char *(*response_headers)(void *ud);
} AgcTransport;

/* Wraps agentc_http_* over net/tls: the default transport for real runs.
 * `insecure` clears certificate verification (--insecure). */
AgcTransport agentc_transport_http(bool insecure);

/* Bounded, sanitised single-line excerpt of the most recent non-2xx response
 * body (empty when the response was 2xx or had no body). Control characters are
 * collapsed to spaces and credential-looking request header values are redacted,
 * so the text is safe to show in an error. Ownership stays with the transport;
 * the pointer is valid until the next request. */
const char *agentc_transport_http_last_body_excerpt(void);

/* ------------------------------------------------------------------ events */
enum {
    AGENTC_EV_AGENT_START = 0,
    /* R-D31: the model cycle is a `step`; the user-driven cycle is a `turn`.
     * The user turn is the AGENT_START..AGENT_END (agent_settled) span. */
    AGENTC_EV_STEP_START,
    AGENTC_EV_MSG_START,
    AGENTC_EV_TEXT_DELTA, /* data: AgcTextDelta* */
    AGENTC_EV_THINK_DELTA,
    AGENTC_EV_TOOL_ARGS_DELTA,
    AGENTC_EV_MSG_END,
    AGENTC_EV_TOOL_EXEC_START, /* data: AgcToolExec* */
    AGENTC_EV_TOOL_EXEC_END,   /* data: AgcToolExec* */
    AGENTC_EV_STEP_END,
    AGENTC_EV_AGENT_END,
    AGENTC_EV_ERROR,   /* data: const char* */
    AGENTC_EV_COMPACT, /* data: AgcCompactInfo* */
    /* A retry is about to re-run the current assistant turn after text or
     * reasoning already streamed. data: NULL. Front ends must discard the
     * buffered/in-progress content for the message opened by the last
     * AGENTC_EV_MSG_START: deltas from the abandoned attempt are being replaced,
     * not appended. Never emitted for a retry that streamed nothing. */
    AGENTC_EV_MSG_RESET,
    /* P6 event layer (append-only; R-D10/R-D16). P6-A emits the ACTOR_* set;
     * the trust/approval/hook slots are reserved in their final order so later
     * waves append no earlier value. */
    AGENTC_EV_TRUST_SET,
    AGENTC_EV_APPROVAL_REQUEST,
    AGENTC_EV_APPROVAL_RESULT,
    AGENTC_EV_SANDBOX_DENIED,
    AGENTC_EV_ACTOR_STARTED,
    AGENTC_EV_ACTOR_PROGRESS,
    AGENTC_EV_ACTOR_STALLED,
    AGENTC_EV_ACTOR_INPUT,
    AGENTC_EV_ACTOR_STEERED,
    AGENTC_EV_ACTOR_QUEUED,
    AGENTC_EV_ACTOR_NOTIFY,
    AGENTC_EV_ACTOR_FINISH,
    AGENTC_EV_ACTOR_FAIL,
    AGENTC_EV_ACTOR_CANCEL_REQUESTED,
    AGENTC_EV_ACTOR_CANCELLED,
    AGENTC_EV_ACTOR_FROZEN,
    AGENTC_EV_ACTOR_RESUMED,
    AGENTC_EV_ACTOR_CHECKPOINTED,
    AGENTC_EV_ACTOR_ATTACHED,
    AGENTC_EV_ACTOR_DETACHED,
    AGENTC_EV_HOOK_START,
    AGENTC_EV_HOOK_END,
    /* Appended P6-B: the queue paused/unpaused state changed. */
    AGENTC_EV_QUEUE_PAUSED,
    /* Appended P6-D: a child agent produced its final result, and the decision
     * seam's request/result/route events (P6-SURFACE §4.1). */
    AGENTC_EV_AGENT_RESULT,
    AGENTC_EV_DECISION_REQUEST,
    AGENTC_EV_DECISION_RESULT,
    AGENTC_EV_ROUTE_DECIDED,
    /* Appended P6-E: an extension setting changed (P6-SETTINGS §5). */
    AGENTC_EV_SETTINGS_CHANGED,
    /* Appended P6-A completion: a TTY process actor's PTY geometry changed
     * (agentc_process_resize). data: AgcActorEvent with cols/rows set. */
    AGENTC_EV_ACTOR_RESIZED,
    /* Appended W4: a virtual model alias resolved to a concrete target for the
     * next request. data: const AgcModelRouteEvent* (borrowed). Observe-only;
     * used by the session layer to append a `custom type:"model"` record. */
    AGENTC_EV_MODEL_ROUTE,
};

typedef struct {
    const char *text;
    size_t len;
} AgcTextDelta;

typedef struct {
    const char *call_id;
    const char *tool_name;
    const char *args_json;
    const char *result; /* NULL on start */
    bool is_error;
    i64 duration_ms;
} AgcToolExec;

/* emitted with AGENTC_EV_COMPACT */
typedef struct {
    u32 tokens_before;
    u32 kept_messages;
    bool automatic;      /* threshold vs manual /compact */
    const char *summary; /* may be NULL */
} AgcCompactInfo;

/* emitted with AGENTC_EV_MODEL_ROUTE (W4): the alias kept as the transcript
 * identity and the concrete target the next request was routed to. All strings
 * are borrowed and valid for the duration of the emit. */
typedef struct {
    const char *virtual_model; /* the alias (a->model) */
    const char *provider;      /* concrete target provider name */
    const char *model;         /* concrete target model id */
    /* Appended P8-D1 (B-M8): the emitting agent's actor id (0 for legacy
     * emitters). The session layer derives the persistence owner from it. This
     * region precedes the actor.h typedef, so it uses the underlying uint32_t
     * (AgcActorId is exactly that). */
    uint32_t actor_id;
} AgcModelRouteEvent;

/* Actor lifecycle event payload (P6-A; P6-SIGNALS §10.1 extends the struct
 * append-only). Field names are stable for RPC/session/hook consumers. */
typedef struct {
    u32 id, parent_id;
    u16 depth;
    int kind;  /* AgcActorKind */
    int state; /* AgcActorState */
    i64 turn;
    int32_t priority;
    const char *at;   /* STEERED: "preempt" | "boundary" */
    const char *text; /* INPUT/NOTIFY text, borrowed */
    const char *json; /* kind-specific, borrowed */
    int exit_code;    /* FINISH */
    i64 duration_ms;
    /* Appended P6-A completion (append-only): the PTY geometry carried by
     * AGENTC_EV_ACTOR_RESIZED. Zero for every other event. */
    int cols, rows;
} AgcActorEvent;

/* Queue mutation event payload (P6-B; dispatched with
 * AGENTC_EV_ACTOR_QUEUED / AGENTC_EV_QUEUE_PAUSED). */
typedef struct {
    uint32_t actor_id;
    int op; /* 0 add, 1 update, 2 delete, 3 reorder, 4 start, 5 clear, 6 pause */
    uint64_t item_id;
    int64_t turn;
    int32_t priority;
    size_t count;
} AgcQueueEvent;

typedef void (*AgcEventFn)(void *ud, int ev, const void *data);

/* Tool results are appended as AGENTC_ROLE_TOOL messages whose first block is a
 * AGENTC_BLK_TEXT carrying the provider call id in `tool_id` (providers read
 * exactly that field when converting transcript to wire messages). */

/* ------------------------------------------------------------------- agent */
typedef struct AgcAgent AgcAgent;

AgcAgent *agentc_agent_new(const AgcProvider *prov, const char *model);
void agentc_agent_free(AgcAgent *a);

void agentc_agent_set_api_key(AgcAgent *a, const char *key);
void agentc_agent_set_base_url(AgcAgent *a, const char *url);
/* Explicit system prompt; NULL means auto. Auto builds the prompt from the live
 * tool table (agentc_prompt_build) before each turn, so a turn-boundary
 * recompose is reflected in the next request. An explicit prompt hard-replaces
 * the build for the whole run; a `before_agent_start` systemPrompt override
 * wins for the rest of the run over both. The effective prompt is published to
 * the extension context before every request. */
void agentc_agent_set_system(AgcAgent *a, const char *text);
void agentc_agent_set_transport(AgcAgent *a, AgcTransport t);
/* The installed transport (borrowed; `request` may be NULL). The app's rebuild
 * path uses this to keep an injected transport (tests/embedders) alive across a
 * model/provider swap, exactly like the observer and event hooks. */
AgcTransport agentc_agent_transport(const AgcAgent *a);
/* Install a tool table. The agent owns a deep copy of the AgcTool array and of
 * every owned string field (name, label, desc, params_json); the caller's array
 * and strings may be freed as soon as this returns. `ud` and the function
 * pointers are kept as-is (opaque/borrowed). NULL/0 clears the table. */
void agentc_agent_set_tools(AgcAgent *a, const AgcTool *tools, size_t n);
void agentc_agent_set_events(AgcAgent *a, AgcEventFn cb, void *ud);
/* Clamps to 0..4. A numeric change emits the observe-only
 * `thinking_level_select` point ({level, previous}). */
void agentc_agent_set_thinking(AgcAgent *a, int level);
/* Configured thinking level as a stable name: "off"/"low"/"medium"/"high". */
const char *agentc_agent_thinking(const AgcAgent *a);
/* Switch the model (same provider) between runs; returns 0 or -errno. An id
 * change emits the observe-only `model_select` point
 * ({provider, model, previous}). */
int agentc_agent_set_model(AgcAgent *a, const char *model);
void agentc_agent_set_max_tokens(AgcAgent *a, i64 max_tokens);
void agentc_agent_set_retry(AgcAgent *a, int max_attempts);
void agentc_agent_set_record(AgcAgent *a, AgcBuf *sink);
void agentc_agent_set_insecure(AgcAgent *a, bool insecure);

/* Run one user turn to completion (all model/tool iterations).
 * Returns 0 on success, negative on failure. */
int agentc_agent_submit(AgcAgent *a, const char *text);

/* Cooperative one-step run (P5 multi-session concurrency). `run_begin` starts a
 * run without blocking and returns 0 or -errno (-EBUSY if one is already in
 * flight); `run_step` advances it by one model step, returning
 * AGENTC_AGENT_RUN_YIELD while more steps remain, 0 on settle, or a negative
 * errno on failure. `run_active` reports whether one is in flight. The front end
 * must rebind its process-global services to the owning session before each
 * `run_step` (see agentc_mode_activate). These never touch a->running, so an
 * agent actor tick cannot steal a slice. */
#define AGENTC_AGENT_RUN_YIELD 1
int agentc_agent_run_begin(AgcAgent *a, const char *text);
int agentc_agent_run_step(AgcAgent *a);
bool agentc_agent_run_active(const AgcAgent *a);

/* Ask the current run to stop (checked between events and inside tool polls). */
void agentc_agent_abort(AgcAgent *a);

/* Interactive pump: installed by a front end (TUI) and called by long-running
 * tools (bash) so the UI can repaint and read input while work proceeds.
 * Same contract as AgcPollHook: return promptly, `timeout_ms` is a hint. */
typedef void (*AgcPumpFn)(void *ud, int timeout_ms);
void agentc_pump_install(AgcPumpFn pump, void *ud);
void agentc_pump(int timeout_ms);

/* Resume support: deep-copy a replayed transcript (from agentc_session_load_messages)
 * into the agent before the first submit. Must be called while idle. Returns 0
 * or -errno. */
int agentc_agent_load(AgcAgent *a, const AgcTranscript *t);

/* Compaction: summarize older messages once the context approaches the model
 * window. Automatic mode runs before a request when the estimated context exceeds
 * ctx_window - reserve_tokens; the tail of keep_recent_tokens is preserved. The
 * summary replaces the compacted prefix as a user message in the transcript. */
typedef struct {
    int block;       /* nonzero: do not run the tool */
    char *reason;    /* owned by the core (agentc_free); NULL = none */
    char *args_json; /* owned by the core (agentc_free); NULL = keep args */
    /* pi's batch-terminate hint. Only honored together with
     * block; when every call in the assistant message was blocked with
     * terminate set, the submission ends after the batch (agent_end stays
     * "tool_use") instead of issuing the next provider turn. */
    int terminate;
} AgcToolVetoDecision;

/* Pre-tool decision hook. out fields are owned by the caller and freed with
 * agentc_free. The callback may set block (with optional reason) and/or a
 * replacement args_json; the core uses the replacement for execution. */
typedef void (*AgcToolVetoFn)(void *ud, const char *tool_call_id, const char *tool_name,
                              const char *args_json, AgcToolVetoDecision *out);
void agentc_agent_set_tool_veto(AgcAgent *a, AgcToolVetoFn cb, void *ud);

/* Recompose hook. The agent calls it at a turn boundary only after the tool job
 * batch has fully returned (never mid-batch, where swapping the table would free
 * live AgcJob.tool pointers), and immediately before a terminal agent_end on an
 * abort/error. The callback re-runs the app's active-tool selection and installs
 * it with agentc_agent_set_tools; it should no-op when nothing changed. */
typedef void (*AgcRecomposeFn)(void *ud, AgcAgent *a);
void agentc_agent_set_recompose(AgcAgent *a, AgcRecomposeFn fn, void *ud);

/* Transcript observer: called after every message is appended or finalized
 * (user message, assistant message_end, tool results). Front ends use it to
 * persist sessions; `m` is the message that just changed. */
typedef void (*AgcMsgObserver)(void *ud, const AgcMsg *m);
void agentc_agent_set_observer(AgcAgent *a, AgcMsgObserver cb, void *ud);

void agentc_agent_set_auto_compact(AgcAgent *a, bool on);
void agentc_agent_set_compact_limits(AgcAgent *a, u32 reserve_tokens, u32 keep_recent_tokens);
int agentc_agent_compact(AgcAgent *a); /* manual; 0 or -errno */
bool agentc_agent_compacted(const AgcAgent *a);
/* Estimated context size: last usage + chars/4 of trailing messages. */
u32 agentc_agent_context_tokens(const AgcAgent *a);

const AgcTranscript *agentc_agent_transcript(const AgcAgent *a);
const char *agentc_agent_last_error(const AgcAgent *a);

/* Actor registry / process actors (P6-A). Included last so existing includers
 * of agent.h keep compiling unchanged. */
#include "actor.h"

/* P6-B bus + hook pipeline (append-only; declared after actor.h so AgcActorId
 * and AgcBuf are visible). */
#include "message.h"
#include "hook.h"

/* P6-C trust envelope, policy and approval contract (append-only). */
#include "trust.h"

typedef struct { /* AGENTC_EV_TRUST_SET */
    AgcActorId actor_id;
    const char *effective_json; /* borrowed */
    const char *reason;         /* narrowing cause, borrowed */
} AgcTrustEvent;

typedef struct { /* AGENTC_EV_APPROVAL_REQUEST/RESULT */
    uint64_t id;
    AgcActorId actor_id;
    const char *tool;
    const char *trigger;
    int decision; /* AgcDecision; 0 on REQUEST */
    const char *reason;
    int scope;
    const char *policy_id;
} AgcApprovalEvent;

/* Root/effective envelope install (P6-C). Borrowed; the app owns it. */
void agentc_agent_set_trust(AgcAgent *a, const AgcTrustEnv *env);
const AgcTrustEnv *agentc_agent_trust(const AgcAgent *a);

/* Tool-batch gate: decide -> policy memo -> ASK/DELEGATE approval dispatch.
 * Returns 0 always; `out` carries the verdict. On DENY the caller writes a
 * tool pre-error and never sends the call to a delegate (P6-TRUST §4.1). */
int agentc_trust_gate(const AgcTrustEnv *env, const AgcToolCall *call, AgcTrustDecision *out);

/* Agent actor binding (P6-B; root-only until P6-D owns the unified child
 * constructor). */
AgcActorId agentc_agent_bind_actor(AgcAgent *a, AgcActorId parent, const char *label);
AgcActorId agentc_agent_actor(const AgcAgent *a);
int64_t agentc_agent_turn_no(const AgcAgent *a);
bool agentc_agent_steerable(const AgcAgent *a, const char **kind);
int agentc_agent_submit_auto(AgcAgent *a, const char *text); /* submit + auto-start */
/* When on, a settled turn with due queue items starts the next turn itself
 * (`trust.queue.auto_start`, SIG §5.5). Off by default; the app enables it. */
void agentc_agent_set_auto_start(AgcAgent *a, bool on);
/* tools.hooks=off (HOK §4): bypass the tool call/result pipeline for the whole
 * run. Per-tool AGENTC_TOOL_NO_HOOKS is honored independently. */
void agentc_agent_set_tools_hooks(AgcAgent *a, bool on);

/* Runtime driver (P6-B; SURF §3.9). `runtime_tick` is the only pump driver:
 * it drains the message bus and ticks the actor registry. */
int agentc_runtime_init(const struct AgcTrustEnv *root);
void agentc_runtime_tick(void);
void agentc_runtime_shutdown(const char *reason);
int agentc_runtime_freeze(uint32_t flags);
int agentc_runtime_resume(void);

/* Runtime continuations (P6-PRIMITIVES §2.1): `runtime_defer` parks a
 * continuation and returns a non-zero token; `runtime_tick` calls each parked
 * `resume(ud, NULL)` once per pump. A continuation that is not yet ready
 * returns without unregistering; a ready one performs its work and calls
 * `agentc_runtime_cancel_defer(token)` (or is cancelled by its owner). The
 * table is bounded and fail-closed: a full table returns 0 and the caller must
 * deny/fail rather than block. */
typedef void (*AgcDeferFn)(void *ud, AgcMessage *m);
uint64_t agentc_runtime_defer(AgcDeferFn resume, void *ud);
void agentc_runtime_cancel_defer(uint64_t token);
size_t agentc_runtime_defer_count(void);

/* =============================================================== P6-D ====
 * Unified agent constructor, the agent tree, and the decision seam. All
 * declarations here are appended (R-D10); decide.h is included last because
 * its helper signatures use AgcMessage/AgcBuf. */
#include "decide.h"

/* Appended P8-D1 (B-M8): route resolution is per-agent. A virtual-model alias
 * resolves its concrete target through seams carried on the agent — installed
 * by the front end that owns the agent, inherited by children — never through a
 * process global, so a background agent of one session can never resolve or
 * persist through another session's context. `provider` maps a provider name to
 * its adapter row (NULL => the provider registry default); `target` maps a
 * concrete target provider to its config-aware endpoint and credential (NULL =>
 * the provider's default base URL + agentc_auth_key). `ud` is opaque to the core;
 * the AgcRouteTarget strings are borrowed for the duration of the call. */
typedef const AgcProvider *(*AgcProviderResolveFn)(void *ud, const char *name);
typedef struct {
    const char *base_url; /* NULL => the provider's default base URL */
    const char *api_key;  /* NULL => none */
} AgcRouteTarget;
typedef bool (*AgcRouteTargetFn)(void *ud, const char *provider, AgcRouteTarget *out);
typedef struct {
    AgcProviderResolveFn provider; /* NULL => registry default */
    void *provider_ud;
    AgcRouteTargetFn target; /* NULL => default base URL + auth store */
    void *target_ud;
} AgcRouteSeam;

/* One agent type, one constructor (R-D1). `parent` selects the tree position;
 * NULL is the root. Every string is borrowed for the duration of the call. */
typedef struct AgcAgentSpec {
    struct AgcAgent *parent;  /* NULL = root; borrowed. */
    const char *task;         /* initial user/task text; NULL = no submit. */
    const char *provider;     /* NULL => inherit parent's current provider. */
    const char *model;        /* NULL => inherit parent's current model. */
    int thinking;             /* -1 => inherit. */
    i64 max_tokens;           /* 0  => inherit. */
    const AgcTrustEnv *trust; /* NULL => inherit; else validated narrow/own. */
    const AgcTool *tools;
    size_t ntools;      /* NULL/0 => parent's effective table. */
    const char *system; /* NULL => auto-build from the live tool table. */
    AgcMsgObserver observer;
    void *observer_ud;
    AgcEventFn events;
    void *events_ud;
    uint32_t flags;    /* AGENTC_AGENT_F_* */
    const char *label; /* UI label; NULL => derived from task. */
    /* Appended P6-D additions. */
    const AgcProvider *provider_handle; /* resolved provider; NULL => resolve `provider`. */
    AgcTransport transport;
    bool transport_set; /* injected/test transport (root rebuild). */
    const char *api_key;
    const char *base_url; /* root config; children inherit. */
    bool insecure;
    int max_attempts;     /* root config; children inherit. */
    const char *env_json; /* reserved: explicit small env map. */
    /* Appended (token budget, P6-SURFACE §18 #23): a cumulative soft cap on
     * this agent's model tokens (usage.input + usage.output + cache reads and
     * writes). 0 => inherit the parent's cap; <0 => explicitly uncapped;
     * >0 => the cap. Reaching it between steps emits `actor_notify` and
     * settles the turn without issuing another provider request. */
    i64 token_budget;
    /* Appended P8-D1 (B-M8): per-agent route resolution seam. A root takes it
     * from the spec; a child whose spec leaves it zeroed inherits the parent's
     * seam. Borrowed for the agent's lifetime. */
    AgcRouteSeam route;
    /* Appended P8-D1 (B-M8): the owning front-end context, opaque to the core.
     * The session-persistence observers (runsession.c) derive the target session
     * of a queue/agent/model-route record from the *emitting agent's* owner, so a
     * background agent of one session can never persist into another. Borrowed;
     * children inherit. */
    void *owner;
} AgcAgentSpec;

#define AGENTC_AGENT_F_INHERIT_CONTEXT 0x1u /* copy bounded parent context. */
#define AGENTC_AGENT_F_EPHEMERAL 0x2u       /* do not persist the child transcript. */
#define AGENTC_AGENT_F_WAIT 0x4u            /* run to completion before returning. */
#define AGENTC_AGENT_F_BACKGROUND 0x8u      /* wait:false: register and return. */

AgcAgent *agentc_agent_create(const AgcAgentSpec *spec); /* NULL on failure. */
const AgcProvider *agentc_agent_provider(const AgcAgent *a);
const char *agentc_agent_model(const AgcAgent *a);

typedef struct { /* AGENTC_EV_SETTINGS_CHANGED */
    const char *ext;
    const char *key;
    int scope; /* 0=cli, 1=project, 2=user, 3=secret */
} AgcSettingsChange;

/* ------------------------------------------------------------- the tree */
typedef struct { /* AGENTC_EV_AGENT_RESULT */
    AgcActorId id, parent_id;
    const char *text; /* final assistant text, borrowed */
    const char *json; /* {usage,agent_id,duration_ms,exit_code}, borrowed */
    int exit_code;
    i64 duration_ms;
} AgcAgentResultEvent;

typedef struct { /* agent tree view (P6-SURFACE §3.8) */
    AgcActorId id, parent_id;
    uint16_t depth;
    int state;                           /* AgcActorState */
    const char *model, *provider, *task; /* borrowed */
    const AgcTrustEnv *trust;            /* borrowed */
    i64 duration_ms;
    int exit_code;
    bool is_subagent;
    i64 turn_no;
    bool steerable;
    const char *steerable_kind; /* NULL | "compact" | "review" */
    /* Appended (token budget): the effective soft cap (0 = none) and the
     * cumulative model tokens spent so far. */
    i64 token_budget;
    uint64_t tokens_spent;
} AgcAgentInfo;

size_t agentc_agent_list(AgcAgentInfo *out, size_t max); /* count-first */
AgcAgent *agentc_agent_find(AgcActorId id);
int agentc_agent_info(AgcActorId id, AgcAgentInfo *out);
AgcActorId agentc_agent_parent(AgcActorId id);
int agentc_agent_wait(AgcActorId id, i64 timeout_ms, AgcBuf *result); /* pumps the bus */
int agentc_agent_send(AgcActorId id, const char *text, i64 turn, int priority, uint32_t flags,
                      const char *client_id);
int agentc_agent_cancel(AgcActorId id, int scope, int stage);
int agentc_agent_result(AgcActorId id, AgcBuf *out);
int agentc_agent_result_set(AgcActorId id, const char *text, int exit_code);
AgcActorId agentc_agent_current(void);
int agentc_agent_depth(const AgcAgent *a);

/* Token budget (P6-SURFACE §18 #23): a cumulative soft cap on the agent's
 * model tokens. When a cap is set and the spent total reaches it between two
 * model steps, the runtime emits `actor_notify` (reason "token_budget") and
 * settles the turn without another provider request. `set` with 0 clears the
 * cap; negative means explicitly uncapped. */
void agentc_agent_set_token_budget(AgcAgent *a, i64 tokens);
int64_t agentc_agent_token_budget(const AgcAgent *a);
uint64_t agentc_agent_tokens_spent(const AgcAgent *a);

/* Appended P8-D1 (B-M8): the per-agent route seam and persistence owner. The
 * seam and owner are borrowed (the front end that owns the agent keeps the
 * userdata alive); children inherit both from their parent when the spec leaves
 * them zeroed. Set/read them across a model rebuild exactly like the injected
 * transport (agentc_agent_transport). Setting a NULL seam clears it. */
void agentc_agent_set_route_seam(AgcAgent *a, const AgcRouteSeam *seam);
AgcRouteSeam agentc_agent_route_seam(const AgcAgent *a);
/* The opaque owning front-end context; NULL = not owned by a front end. */
void agentc_agent_set_owner(AgcAgent *a, void *owner);
void *agentc_agent_owner(const AgcAgent *a);

#endif /* AGENTC_AGENT_H */
