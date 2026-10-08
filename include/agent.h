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
    i64 cost_micro;               /* micro-USD, integer */
} AgcUsage;

typedef struct {
    int type;                     /* AGENTC_BLK_* */
    char *text;                   /* AGENTC_BLK_TEXT/THINK body (owned) */
    size_t text_len;
    char *tool_id;                /* AGENTC_BLK_TOOLCALL (owned) */
    char *tool_name;
    char *tool_args;              /* raw JSON text (owned) */
} AgcBlock;

typedef struct {
    int role;                     /* AGENTC_ROLE_* */
    AgcBlock *blocks;
    size_t nblocks, blocks_cap;
    AgcUsage usage;
    int stop_reason;              /* AGENTC_STOP_* */
    char *error;                  /* owned or NULL */
    u64 ts_ms;
} AgcMsg;

typedef struct {
    AgcMsg *msgs;
    size_t n, cap;
    const char *model;            /* borrowed */
    const char *provider;         /* borrowed */
    const char *system;           /* owned system prompt snapshot */
} AgcTranscript;

void agentc_transcript_init(AgcTranscript *t);
void agentc_transcript_free(AgcTranscript *t);
void agentc_msg_free(AgcMsg *m);
AgcMsg *agentc_transcript_push(AgcTranscript *t, int role);      /* zeroed message */
void agentc_msg_add_text(AgcMsg *m, const char *text, size_t n);
void agentc_msg_add_tool_call(AgcMsg *m, const char *id, const char *name);
void agentc_msg_tool_args_append(AgcMsg *m, const char *p, size_t n);

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
} AgcToolCall;

struct AgcTool {
    const char *name;
    const char *label;
    const char *desc;
    const char *params_json;      /* JSON Schema object, embedded string */
    u32 flags;                    /* AGENTC_TOOL_* */
    char *(*exec)(const char *args_json, bool *is_error, const volatile bool *cancel);

    /* Appended v2 tool interface. Every existing 6-field positional initializer
     * above stays valid: C zero-fills the appended fields, and `exec` remains
     * the v1 entry point the job driver wraps when `run`/`start` are NULL. New
     * tools implement `run` (synchronous) or `start`/`step` (async) and leave
     * `exec` NULL. `ud` is the tool-instance identity (NULL for builtins), so a
     * extension or MCP adapter no longer needs one trampoline per registration. A
     * `timeout_ms` of 0 selects AGENTC_TOOL_TIMEOUT_DEF_MS. The job driver
     * (src/core/tools/jobs.h) owns the AgcJob it passes to start/step. */
    void  *ud;                 /* instance identity; NULL for builtins */
    int    timeout_ms;         /* 0 = AGENTC_TOOL_TIMEOUT_DEF_MS (120000) */
    int  (*run)(const AgcTool *self, const AgcToolCall *call,
                AgcBuf *out, bool *is_error);          /* 0 or -errno */
    int  (*start)(const AgcTool *self, const AgcToolCall *call, AgcJob *job);
    int  (*step)(const AgcTool *self, AgcJob *job); /* 0 running, 1 done, <0 fatal */
};

#define AGENTC_TOOL_READONLY   0x1u
#define AGENTC_TOOL_DESTRUCTIVE 0x2u
#define AGENTC_TOOL_SEQUENTIAL 0x4u
/* Registered and executable, but not declared to the model: hidden tools are
 * omitted from the model-facing # Tools prompt section and (from the provider
 * stage on) from request serialization. Tools the app or another tool invokes
 * directly set this bit. */
#define AGENTC_TOOL_HIDDEN     0x8u
/* Core-internal (never crosses the extension ABI): the tool is registered by
 * the core itself (the builtin-tools extension). The app's default selection
 * hides non-core AGENTC_TOOL_DESTRUCTIVE tools unless they are named in
 * --tools/default_tools; core builtins stay eligible. */
#define AGENTC_TOOL_CORE       0x10u
#define AGENTC_TOOL_TIMEOUT_DEF_MS 120000
#define AGENTC_TOOL_JOBS_MAX 32

size_t agentc_tools_builtin(AgcTool *out, size_t max);   /* read, bash, edit, write, ls, find, grep */

/* ----------------------------------------------------------- tool helpers */
/* All return owned strings (agentc_free) unless noted. */
char *agentc_tool_read(const char *path, i64 offset, i64 limit, bool *is_error);
char *agentc_tool_write(const char *path, const char *content, bool *is_error);
/* Length-explicit form of agentc_tool_write: `content` need not be NUL
 * terminated and may contain NUL bytes, and exactly `len` bytes are written.
 * agentc_tool_write() is the strlen convenience wrapper. */
char *agentc_tool_write_len(const char *path, const char *content, size_t len,
                        bool *is_error);
char *agentc_tool_edit(const char *path, const char *edits_json, bool *is_error);
char *agentc_tool_bash(const char *command, i64 timeout_ms, const volatile bool *cancel,
                   bool *is_error);

/* -------------------------------------------------------------- providers */
typedef struct {
    const char *provider;         /* e.g. "openai" */
    const char *model;            /* "claude-sonnet-4-5" */
    const char *api_key;          /* borrowed */
    const char *base_url;         /* optional override */
    const char *system;           /* system prompt */
    const AgcTranscript *transcript; /* messages to serialize (borrowed) */
    const AgcTool *tools;
    size_t ntools;
    int thinking_level;           /* 0=off .. 4=high */
    i64 max_tokens;               /* 0 = model default */
    const char *account_id;       /* OAuth account id (ChatGPT backend), optional */
} AgcRequest;

/* Stream state filled by the provider while SSE events arrive. */
typedef struct {
    AgcMsg *msg;                   /* assistant message being built */
    AgcBuf args;                   /* deprecated: adapter args accumulate in `priv` */
    size_t tool_index;            /* deprecated: adapter index lives in `priv` */
    u32 usage_input, usage_output, usage_cache_read, usage_cache_write;
    u32 usage_reasoning;          /* subset of output, when the provider reports it */
    char response_id[128];
    char error[512];
    int stop_reason;
    bool saw_stop;
    AgcJsonArena *arena;   /* per-stream JSON arena; owned by the agent loop */
    void *priv;               /* adapter stream state, via ops->stream_open */
} AgcStreamState;

typedef struct AgcProvider {
    const char *name;             /* "openai", "anthropic" */
    const char *api;              /* "anthropic-messages", "openai-chat" */
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
const AgcProvider *agentc_prov_openai_codex(void);       /* chat completions */
/* Ollama speaks the OpenAI chat API: local at 127.0.0.1:11434 (no key needed)
 * and Ollama Cloud at ollama.com (Bearer key). */
const AgcProvider *agentc_prov_ollama(void);       /* local ollama */
const AgcProvider *agentc_prov_ollama_cloud(void);
/* Native Google Gemini (Generative Language API): generateContent with
 * systemInstruction, functionCall/functionResponse tools and thinking budgets. */
const AgcProvider *agentc_prov_google(void);
/* Any other OpenAI-compatible endpoint (OpenRouter, xAI, Gemini's compat
 * endpoint, a self-hosted gateway, ...). The strings are copied; the returned
 * provider is stable for the process lifetime (up to 8 distinct names). */
const AgcProvider *agentc_prov_openai_compatible(const char *name, const char *base_url);

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
} AgcModel;

#define AGENTC_MODEL_STATIC 0x1u
#define AGENTC_MODEL_RATE_KNOWN 0x1u
#define AGENTC_MODEL_INPUT_INCLUDES_CACHE 0x2u

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
void agentc_model_clear_dynamic(const char *provider);   /* NULL clears everything */
/* Drop the static rows registered for `provider` (NULL clears every static
 * row). Used by the extension registry when provider records are freed; the
 * dynamic clear never touches static rows. */
void agentc_model_clear_static(const char *provider);
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
void agentc_model_set_rate_override(const char *provider, const char *id, i64 in_rate,
                                    i64 out_rate, i64 cache_read_rate, i64 cache_write_rate,
                                    u32 rate_flags);

/* --------------------------------------------------------------- transport */
/* Injected I/O: real runs use the wire layer; tests replay canned bytes.
 * Return: 0 on success, a positive HTTP status for status errors (429/5xx are
 * retried by the loop), or a negative errno / -ECANCELED on transport failure.
 * on_chunk returns 0 to continue, nonzero to abort (surfaced as -ECANCELED). */
typedef int (*AgcTransportFn)(void *ud, const char *url, const char *headers,
                             const void *body, size_t body_len,
                             int (*on_chunk)(void *u, const void *p, size_t n),
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
    AGENTC_EV_TURN_START,
    AGENTC_EV_MSG_START,
    AGENTC_EV_TEXT_DELTA,             /* data: AgcTextDelta* */
    AGENTC_EV_THINK_DELTA,
    AGENTC_EV_TOOL_ARGS_DELTA,
    AGENTC_EV_MSG_END,
    AGENTC_EV_TOOL_EXEC_START,        /* data: AgcToolExec* */
    AGENTC_EV_TOOL_EXEC_END,          /* data: AgcToolExec* */
    AGENTC_EV_TURN_END,
    AGENTC_EV_AGENT_END,
    AGENTC_EV_ERROR,                  /* data: const char* */
    AGENTC_EV_COMPACT,                /* data: AgcCompactInfo* */
    /* A retry is about to re-run the current assistant turn after text or
     * reasoning already streamed. data: NULL. Front ends must discard the
     * buffered/in-progress content for the message opened by the last
     * AGENTC_EV_MSG_START: deltas from the abandoned attempt are being replaced,
     * not appended. Never emitted for a retry that streamed nothing. */
    AGENTC_EV_MSG_RESET,
};

typedef struct {
    const char *text;
    size_t len;
} AgcTextDelta;

typedef struct {
    const char *call_id;
    const char *tool_name;
    const char *args_json;
    const char *result;           /* NULL on start */
    bool is_error;
    i64 duration_ms;
} AgcToolExec;

/* emitted with AGENTC_EV_COMPACT */
typedef struct {
    u32 tokens_before;
    u32 kept_messages;
    bool automatic;               /* threshold vs manual /compact */
    const char *summary;          /* may be NULL */
} AgcCompactInfo;

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
    int   block;      /* nonzero: do not run the tool */
    char *reason;     /* owned by the core (agentc_free); NULL = none */
    char *args_json;  /* owned by the core (agentc_free); NULL = keep args */
    /* pi's batch-terminate hint. Only honored together with
     * block; when every call in the assistant message was blocked with
     * terminate set, the submission ends after the batch (agent_end stays
     * "tool_use") instead of issuing the next provider turn. */
    int   terminate;
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
int agentc_agent_compact(AgcAgent *a);           /* manual; 0 or -errno */
bool agentc_agent_compacted(const AgcAgent *a);
/* Estimated context size: last usage + chars/4 of trailing messages. */
u32 agentc_agent_context_tokens(const AgcAgent *a);

const AgcTranscript *agentc_agent_transcript(const AgcAgent *a);
const char *agentc_agent_last_error(const AgcAgent *a);

#endif /* AGENTC_AGENT_H */
