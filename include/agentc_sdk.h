/*
 * agentc_sdk.h — stable embedding ABI for libagentc (Axis B).
 *
 * One engine per process. A host links build/libagentc.a, calls
 * agentc_sdk_init() once, creates at most one AgcSdkAgent, drives it and then
 * calls agentc_sdk_shutdown(). Every string crossing this boundary is
 * NUL-terminated UTF-8 unless stated otherwise; every struct is append-only and
 * begins with a `struct_size` the host must set to sizeof(its own struct), so
 * future versions can append fields without breaking older callers.
 *
 * Security-first defaults: the SDK never reads the user or project config
 * (load_user_config/load_project_config false), never loads dynamic extension
 * libraries, never connects an MCP server (no_mcp true) and never makes network
 * discovery requests (offline true) unless the host opts in. The host owns
 * signals, stdio and the terminal; the engine installs no signal handlers and
 * never restores terminal state.
 *
 * Threading: the engine is single-threaded and pump-driven. All SDK calls must
 * be made from the thread that called agentc_sdk_init(); deferred extension work
 * is serviced by agentc_sdk_pump() (or by the agent's installed poll hook while
 * a run is blocked).
 */
#ifndef AGENTC_SDK_H
#define AGENTC_SDK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "agentc.h"
#include "config.h"
#include "agent.h"
#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped only for an incompatible change to this header. Additive fields and
 * functions do not bump it. */
#define AGENTC_SDK_ABI 1

/* ------------------------------------------------------------- lifecycle */

typedef struct {
    uint32_t struct_size; /* = sizeof(AgcSdkConfig) */
    const char *cwd;      /* borrowed; NULL = current directory */
    /* When non-NULL, published to the platform layer with os_embed_init()
     * before anything else. The arrays are borrowed and must outlive the
     * engine. */
    char **argv;
    char **envp;
    /* Optional pristine config; NULL = built-in defaults (agentc_config_defaults)
     * unless load_user_config/load_project_config request file I/O. Borrowed;
     * the engine does not free a host-provided config. */
    const AgcConfig *config;
    bool load_user_config;        /* default false */
    bool load_project_config;     /* default false */
    bool allow_network;           /* default false: no discovery requests */
    bool enable_mcp;              /* default false: no MCP server */
    bool load_dynamic_extensions; /* default false */
    bool trusted;                 /* default false: project resources ignored */
    int log_level;                /* -1 = leave the default; else 0..4 */
    AgcLogFn log;
    void *log_ud; /* optional log sink (agentc_log_install) */
    AgcFatalFn fatal;
    void *fatal_ud;           /* optional fatal sink (agentc_fatal_install) */
    unsigned startup_pump_ms; /* bounded MCP/extension settle budget; 0 = none */
} AgcSdkConfig;

/* Initialize the one engine. Returns 0, -EBUSY when already initialized, or a
 * negative errno (e.g. -ENOMEM). The config may be NULL for the safe defaults. */
int agentc_sdk_init(const AgcSdkConfig *config);

/* Tear the engine down and reset every process-global registry it owns
 * (extensions, provider/model rows, prompts, status, auth/OAuth, events, pump
 * hooks, JSON arena, settings, deciders). Freeing the agent is implicit. Safe
 * when not initialized; a later agentc_sdk_init() starts clean. */
void agentc_sdk_shutdown(void);

/* The engine version string ("x.y.z") and the host platform name
 * ("linux"/"macos"/"windows"), both process-stable. */
const char *agentc_sdk_version(void);
const char *agentc_sdk_host(void);

/* True between a successful init and the matching shutdown. */
bool agentc_sdk_ready(void);

/* --------------------------------------------------------------- agents */

typedef struct {
    uint32_t struct_size;     /* = sizeof(AgcSdkAgentOptions) */
    const char *provider;     /* NULL = config default */
    const char *model;        /* NULL = config default; may be auto-picked */
    const char *api_key;      /* NULL = resolve from env/auth/config */
    const char *base_url;     /* NULL = provider/default */
    const char *system;       /* NULL = auto-build per turn */
    int thinking;             /* -1 = config default (medium), else 0..4 */
    i64 max_tokens;           /* 0 = model default */
    bool persist_session;     /* default false: memory-only session */
    const char *session_dir;  /* borrowed; used when persist_session is true */
    const char *session_spec; /* borrowed; path or id to open */
    bool continue_last;       /* resume the newest session */
    bool insecure;            /* skip TLS verification (tests only) */
    /* Transport injection. When transport.request is non-NULL it replaces the
     * built-in HTTP transport for the whole agent lifetime (including a
     * provider/model rebuild). */
    AgcTransport transport;
    bool transport_set;
    /* Event and transcript observers. NULL event = no JSONL output; NULL
     * observer = the session persistence observer. */
    AgcEventFn events;
    void *events_ud;
    AgcMsgObserver observer;
    void *observer_ud;
    /* Explicit tool table. When non-NULL/`ntools` it replaces the policy-selected
     * table; the array and its strings must stay valid for the agent's lifetime.
     * Otherwise the engine selects through the app policy (default_tools). */
    const AgcTool *tools;
    size_t ntools;
    bool tools_set;
} AgcSdkAgentOptions;

typedef struct AgcSdkAgent AgcSdkAgent;

/* Create the one SDK agent (fails with NULL and a -EBUSY last_error when one
 * already exists). Returns NULL on failure; agentc_sdk_agent_last_error()
 * carries the reason on the active agent only, so a failed constructor records
 * it in the process-global last-error slot readable with
 * agentc_sdk_last_error(). */
AgcSdkAgent *agentc_sdk_agent_new(const AgcSdkAgentOptions *opts);
void agentc_sdk_agent_free(AgcSdkAgent *a);

/* Borrowed engine handles; NULL when `a` is NULL. */
AgcAgent *agentc_sdk_agent(AgcSdkAgent *a);
AgcSession *agentc_sdk_session(AgcSdkAgent *a);

/* The process-global constructor last error (NUL-terminated, never NULL). */
const char *agentc_sdk_last_error(void);

/* Replace the event/observer/transport hooks on the live agent. Returns 0 or
 * -EINVAL. */
int agentc_sdk_agent_set_events(AgcSdkAgent *a, AgcEventFn cb, void *ud);
int agentc_sdk_agent_set_observer(AgcSdkAgent *a, AgcMsgObserver cb, void *ud);
int agentc_sdk_agent_set_transport(AgcSdkAgent *a, AgcTransport t);
int agentc_sdk_agent_set_tools(AgcSdkAgent *a, const AgcTool *tools, size_t ntools);

/* Run one user turn to completion (all model/tool iterations). Returns 0 or a
 * negative errno; the detail is in agentc_sdk_agent_last_error(). Re-entrant
 * submits return -EBUSY. This is the synchronous wrapper around
 * agentc_sdk_agent_submit_async()/step() below: it drives the same cooperative
 * run to settle before returning, so its transcript and result are identical. */
int agentc_sdk_agent_submit(AgcSdkAgent *a, const char *text);

/* Pump-driven asynchronous run (P5/P6 SDK gap). `submit_async` starts a user
 * turn without blocking (0, or -EBUSY when one is already in flight) and
 * `step` advances it by one model step. `step` returns AGENTC_AGENT_RUN_YIELD
 * (1) while more steps remain, 0 once the run has settled (the transcript and
 * last_error are then final), or a negative errno on failure. A host that owns
 * its own event loop drives it with:
 *
 *     if (agentc_sdk_agent_submit_async(a, "hello") == 0)
 *         while (agentc_sdk_agent_step(a) == AGENTC_AGENT_RUN_YIELD)
 *             agentc_sdk_pump(a, 0);
 *
 * The run is single-threaded and cooperative: between steps the host may call
 * agentc_sdk_pump() (deferred/extension/actor work) and
 * agentc_sdk_agent_abort(), which is observed at the next step boundary. These
 * are process-global-safe because the SDK owns the one engine/agent (B-D2); the
 * multi-session `serve` front end uses the same core API with per-session
 * rebinding. */
int agentc_sdk_agent_submit_async(AgcSdkAgent *a, const char *text);
int agentc_sdk_agent_step(AgcSdkAgent *a);
bool agentc_sdk_agent_run_active(const AgcSdkAgent *a);

/* Ask the running turn to stop. Only meaningful from a callback the engine
 * invokes (a pump/tool/event); see AgcSdkConfig and agentc_sdk_pump(). */
void agentc_sdk_agent_abort(AgcSdkAgent *a);

/* Service deferred extension work (MCP connect/re-sync, bus/actor ticks). The
 * agent installs this as its poll hook; hosts may also call it from their own
 * event loop. `timeout_ms` is a hint. Safe when no agent exists (pumps the
 * extension registry only). */
void agentc_sdk_pump(AgcSdkAgent *a, int timeout_ms);

/* Start a fresh session on the live agent (memory-only agents no-op with 0).
 * Returns 0 or -errno. */
int agentc_sdk_agent_new_session(AgcSdkAgent *a);

/* Borrowed transcript and last error of the agent. NULL when `a` is NULL. */
const AgcTranscript *agentc_sdk_agent_transcript(const AgcSdkAgent *a);
const char *agentc_sdk_agent_last_error(const AgcSdkAgent *a);

/* ----------------------------------------------------------- registries */

/* Count-first tool table (the engine's current active table): with out == NULL
 * returns the total; otherwise fills min(total, max) and returns the count. */
size_t agentc_sdk_tools(AgcTool *out, size_t max);

/* Count-first provider names (builtins, presets and loaded extension
 * providers). With out == NULL returns the total; otherwise fills
 * min(total, max) borrowed names and returns the count. */
size_t agentc_sdk_providers(const char **out, size_t max);

/* Resolve one provider descriptor by name (NULL when unknown). */
const AgcProvider *agentc_sdk_provider(const char *name);

/* Whether the provider needs a credential to run (false for local Ollama and
 * unauthenticated gateways). Unknown providers return true (fail closed). */
bool agentc_sdk_needs_key(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AGENTC_SDK_H */
