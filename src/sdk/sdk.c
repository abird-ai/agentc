/* sdk.c — the embeddable libagentc SDK (Axis B).
 *
 * One engine, one agent, security-first defaults. The implementation composes
 * exactly like app/main.c does (config -> extensions -> context -> trust ->
 * tools -> bounded pump) but with the load flags inverted: the host must opt in
 * to user/project config, dynamic extension libraries, MCP and network
 * discovery. Every process-global registry the engine owns is reset on
 * shutdown so a second init/shutdown cycle is clean.
 *
 * AgcSdkAgent embeds its AgcModeCtx as the first member, so the mode context's
 * callbacks (which cast their userdata) keep working unchanged; the mode
 * configuration and I/O records live in the agent for the same reason.
 */
#include "agentc_sdk.h"

#include "app/mode.h"
#include "app/policy.h"
#include "app/project.h"
#include "app/setup.h"
#include "core/events.h"
#include "core/prompt.h"
#include "core/prompts.h"
#include "decide.h"
#include "ext.h"
#include "mcp.h"
#include "net.h"
#include "plat.h"
#include "prov/provider.h"
#include "settings.h"
#include "status.h"
#include "wire.h"

/* Process-global engine state (one engine per process, B-D2). */
static bool g_inited;
static AgcSdkAgent *g_agent;
static AgcConfig *g_owned_cfg;      /* created by init; freed by shutdown */
static const AgcConfig *g_host_cfg; /* borrowed from AgcSdkConfig */
static char g_error[256];

/* ------------------------------------------------------------------ errors */

static void sdk_set_error(const char *msg) {
    size_t i = 0;
    if (!msg)
        msg = "";
    for (; msg[i] && i + 1 < sizeof g_error; i++)
        g_error[i] = msg[i];
    g_error[i] = 0;
}

const char *agentc_sdk_last_error(void) {
    return g_error;
}

/* --------------------------------------------------------------- lifecycle */

static void sdk_startup_pump(unsigned budget_ms) {
    if (budget_ms == 0)
        return;
    i64 start = os_now_ns(OS_CLOCK_MONOTONIC);
    agentc_ext_pump();
    while (agentc_mcp_pending()) {
        if (os_now_ns(OS_CLOCK_MONOTONIC) - start >= (i64)budget_ms * 1000000)
            break;
        agentc_ext_pump();
        os_sleep_ns(1000000);
    }
}

int agentc_sdk_init(const AgcSdkConfig *cfg) {
    AgcSdkConfig local;
    agentc_memset(&local, 0, sizeof local);
    local.struct_size = (uint32_t)sizeof local;
    if (!cfg)
        cfg = &local;
    if (g_inited)
        return -16; /* EBUSY */

    if (cfg->argv || cfg->envp)
        os_embed_init(cfg->argv, cfg->envp);

    char cwd[4096];
    const char *c = cfg->cwd;
    if (!c || !c[0]) {
        if (os_getcwd(cwd, sizeof cwd) < 0)
            cwd[0] = 0;
        c = cwd;
    }

    const AgcConfig *conf = cfg->config;
    if (!conf) {
        if (cfg->load_user_config || cfg->load_project_config)
            g_owned_cfg = agentc_config_load(c);
        else
            g_owned_cfg = agentc_config_defaults();
        if (!g_owned_cfg) {
            sdk_set_error("config allocation failed");
            return -12;
        }
        conf = g_owned_cfg;
    }
    g_host_cfg = cfg->config;

    agentc_config_set_cli_trust(-1);
    if (agentc_settings_init(c, cfg->trusted) != 0)
        agentc_logf(2, "sdk: settings unavailable; extension settings are defaults");
    (void)agentc_pricing_load(NULL, 0);
    (void)agentc_net_init();

    agentc_setup_set_context(conf, NULL, NULL);
    agentc_ext_register_defaults(!cfg->enable_mcp);
    agentc_ext_register_linked();
    if (cfg->load_dynamic_extensions)
        agentc_ext_register_dynamic();
    agentc_ext_apply_config(conf);
    /* No server may start before trust is known. */
    agentc_mcp_set_context(c, false);
    agentc_ext_load_all();

    agentc_project_apply_resources(c, cfg->trusted);
    agentc_project_apply_policy(c, cfg->trusted);
    (void)agentc_prompts_load_file_templates(c, cfg->trusted);
    agentc_builtin_context_set(c, cfg->trusted);

    sdk_startup_pump(cfg->startup_pump_ms);

    if (cfg->log)
        agentc_log_install(cfg->log, cfg->log_ud);
    if (cfg->fatal)
        agentc_fatal_install(cfg->fatal, cfg->fatal_ud);
    if (cfg->log_level >= 0)
        agentc_log_set_level(cfg->log_level);

    g_inited = true;
    g_agent = NULL;
    sdk_set_error("");
    return 0;
}

/* Free the agent (if any) and reset every process-global registry the engine
 * owns. Kept in one function so init -> shutdown -> init is exact. */
static void sdk_reset(void) {
    if (g_agent) {
        agentc_sdk_agent_free(g_agent);
        g_agent = NULL;
    }
    agentc_ext_set_model_sink(NULL, NULL);
    agentc_ext_set_thinking_sink(NULL, NULL);
    agentc_ext_set_entry_sink(NULL, NULL);
    agentc_ext_set_ui_sink(NULL, NULL);
    agentc_ext_set_status_observer(NULL, NULL);
    agentc_http_set_poll_hook(NULL, NULL);
    agentc_pump_install(NULL, NULL);
    agentc_setup_set_context(NULL, NULL, NULL);
    agentc_auth_free(); /* also frees the OAuth store */
    agentc_settings_shutdown();
    agentc_decider_reset();
    agentc_decision_pending_reset();
    agentc_events_reset();
    agentc_prompts_reset();
    agentc_model_reset();
    agentc_ext_shutdown(); /* drops provider rows/status/tools */
    agentc_json_reset();
    agentc_log_install(NULL, NULL);
    agentc_fatal_install(NULL, NULL);
    if (g_owned_cfg) {
        agentc_config_free(g_owned_cfg);
        g_owned_cfg = NULL;
    }
    g_host_cfg = NULL;
}

void agentc_sdk_shutdown(void) {
    if (!g_inited)
        return;
    sdk_reset();
    g_inited = false;
    sdk_set_error("");
}

bool agentc_sdk_ready(void) {
    return g_inited;
}

const char *agentc_sdk_version(void) {
    return AGENTC_VERSION;
}
const char *agentc_sdk_host(void) {
    return os_platform();
}

/* ------------------------------------------------------------------ agents */

struct AgcSdkAgent {
    AgcModeCtx ctx; /* MUST be first: mode callbacks cast to it */
    AgcModeConfig mcfg;
    AgcModeIo io;
    AgcSessionOptions so;
    uint32_t magic;
    char *cwd_owned;      /* session cwd (borrowed by the mode) */
    AgcTool *owned_tools; /* selected by policy; NULL when injected */
    size_t n_owned_tools;
    ToolPolicy policy;
    AgcTransport transport;
    bool transport_set;
    AgcEventFn events;
    void *events_ud;
    AgcMsgObserver observer;
    void *observer_ud;
};

#define SDK_AGENT_MAGIC 0x41474342u /* "AGCB" */

static const char *sdk_pick_model(const AgcConfig *conf, const char *provider,
                                  const AgcProvider *prov) {
    if (conf->default_model && conf->default_model[0])
        return conf->default_model;
    const AgcModel *all[64];
    size_t n = agentc_model_filter(provider, prov ? prov->api : NULL, all, 64);
    return n ? all[0]->id : NULL;
}

AgcSdkAgent *agentc_sdk_agent_new(const AgcSdkAgentOptions *opts) {
    AgcSdkAgentOptions local;
    agentc_memset(&local, 0, sizeof local);
    local.struct_size = (uint32_t)sizeof local;
    local.thinking = -1;
    if (!opts)
        opts = &local;
    if (!g_inited) {
        sdk_set_error("agentc_sdk_init has not run");
        return NULL;
    }
    if (g_agent) {
        sdk_set_error("an SDK agent already exists (one per process)");
        return NULL;
    }

    AgcSdkAgent *a = agentc_alloc(sizeof *a);
    agentc_memset(a, 0, sizeof *a);
    a->magic = SDK_AGENT_MAGIC;

    const AgcConfig *conf = g_owned_cfg ? g_owned_cfg : g_host_cfg;
    if (!conf) {
        sdk_set_error("no engine config");
        agentc_free(a);
        return NULL;
    }

    /* cwd for the session/config context; the mode borrows it for its whole
     * lifetime, so the agent owns a copy. */
    char cwdbuf[4096];
    if (os_getcwd(cwdbuf, sizeof cwdbuf) < 0)
        cwdbuf[0] = 0;
    a->cwd_owned = agentc_strdup(cwdbuf);
    const char *cwd = a->cwd_owned;

    const char *provider = (opts->provider && opts->provider[0]) ? opts->provider
                           : (conf->default_provider && conf->default_provider[0])
                               ? conf->default_provider
                               : "openai";
    const AgcProvider *prov = agentc_setup_provider(provider);
    if (!prov) {
        sdk_set_error("unknown provider");
        agentc_free(a->cwd_owned);
        agentc_free(a);
        return NULL;
    }
    const char *model =
        (opts->model && opts->model[0]) ? opts->model : sdk_pick_model(conf, provider, prov);
    const char *api_key = agentc_setup_resolve_key(conf, provider, opts->api_key);
    const char *base_url = agentc_setup_base_url(conf, provider, opts->base_url);

    /* Tool table: policy-selected unless the host injected one. */
    a->policy.cfg = conf;
    a->policy.tools_flag = NULL;
    a->policy.no_tools = false;
    const AgcTool *tools = NULL;
    size_t ntools = 0;
    if (opts->tools_set && opts->tools) {
        tools = opts->tools;
        ntools = opts->ntools;
    } else {
        size_t navail = agentc_ext_tools(NULL, 0);
        AgcTool *avail = agentc_alloc((navail ? navail : 1) * sizeof *avail);
        navail = agentc_ext_tools(avail, navail);
        if (agentc_policy_apply(avail, navail, &a->policy, false, &a->owned_tools,
                                &a->n_owned_tools) != 0) {
            agentc_free(avail);
            sdk_set_error("tool policy failed");
            agentc_free(a->cwd_owned);
            agentc_free(a);
            return NULL;
        }
        agentc_free(avail);
        tools = a->owned_tools;
        ntools = a->n_owned_tools;
    }
    agentc_ext_clear_dirty();

    /* Session options. */
    a->so.cwd = cwd;
    a->so.dir = opts->session_dir;
    a->so.memory_only = !opts->persist_session;

    /* Mode config. */
    a->mcfg.cfg = conf;
    a->mcfg.provider = provider;
    a->mcfg.model = model;
    a->mcfg.api_key = api_key;
    a->mcfg.base_url = base_url;
    a->mcfg.system = opts->system;
    a->mcfg.tools = tools;
    a->mcfg.ntools = ntools;
    a->mcfg.thinking = opts->thinking >= 0 ? opts->thinking : 3;
    a->mcfg.max_tokens = opts->max_tokens;
    a->mcfg.max_attempts = conf->max_attempts;
    a->mcfg.insecure = opts->insecure;
    a->mcfg.auto_compact = true;
    a->mcfg.compact_reserve = 16384;
    a->mcfg.compact_keep = 20000;
    a->mcfg.session = a->so;
    a->mcfg.session_spec = opts->session_spec;
    a->mcfg.continue_last = opts->continue_last;
    a->mcfg.recompose = agentc_policy_recompose;
    a->mcfg.recompose_ud = &a->policy;

    /* I/O: no JSONL writer, host events/observer. */
    a->io.read = NULL;
    a->io.write = NULL;
    a->io.event = opts->events;
    a->io.event_ud = opts->events_ud;
    a->io.observer = opts->observer;
    a->io.observer_ud = opts->observer_ud;

    a->events = opts->events;
    a->events_ud = opts->events_ud;
    a->observer = opts->observer;
    a->observer_ud = opts->observer_ud;
    if (opts->transport_set) {
        a->transport = opts->transport;
        a->transport_set = true;
    }

    int rc =
        agentc_mode_setup(&a->ctx, &a->mcfg, &a->io, AGENTC_MODE_F_SESSION | AGENTC_MODE_F_ABORT);
    if (rc != 0) {
        sdk_set_error("agent setup failed");
        agentc_free(a->owned_tools);
        agentc_free(a->cwd_owned);
        agentc_free(a);
        return NULL;
    }
    if (a->transport_set)
        agentc_agent_set_transport(a->ctx.agent, a->transport);

    g_agent = a;
    sdk_set_error("");
    return a;
}

void agentc_sdk_agent_free(AgcSdkAgent *a) {
    if (!a || a->magic != SDK_AGENT_MAGIC)
        return;
    if (g_agent == a)
        g_agent = NULL;
    agentc_mode_teardown(&a->ctx);
    agentc_free(a->owned_tools);
    agentc_free(a->cwd_owned);
    a->magic = 0;
    agentc_free(a);
}

AgcAgent *agentc_sdk_agent(AgcSdkAgent *a) {
    return (a && a->magic == SDK_AGENT_MAGIC) ? a->ctx.agent : NULL;
}

AgcSession *agentc_sdk_session(AgcSdkAgent *a) {
    return (a && a->magic == SDK_AGENT_MAGIC) ? a->ctx.session : NULL;
}

int agentc_sdk_agent_set_events(AgcSdkAgent *a, AgcEventFn cb, void *ud) {
    if (!a || a->magic != SDK_AGENT_MAGIC || !a->ctx.agent)
        return -22;
    a->events = cb;
    a->events_ud = ud;
    a->io.event = cb;
    a->io.event_ud = ud;
    agentc_agent_set_events(a->ctx.agent, cb, ud);
    return 0;
}

int agentc_sdk_agent_set_observer(AgcSdkAgent *a, AgcMsgObserver cb, void *ud) {
    if (!a || a->magic != SDK_AGENT_MAGIC || !a->ctx.agent)
        return -22;
    a->observer = cb;
    a->observer_ud = ud;
    a->io.observer = cb;
    a->io.observer_ud = ud;
    agentc_agent_set_observer(a->ctx.agent, cb, ud);
    return 0;
}

int agentc_sdk_agent_set_transport(AgcSdkAgent *a, AgcTransport t) {
    if (!a || a->magic != SDK_AGENT_MAGIC || !a->ctx.agent)
        return -22;
    a->transport = t;
    a->transport_set = t.request != NULL;
    agentc_agent_set_transport(a->ctx.agent, t);
    return 0;
}

int agentc_sdk_agent_set_tools(AgcSdkAgent *a, const AgcTool *tools, size_t ntools) {
    if (!a || a->magic != SDK_AGENT_MAGIC || !a->ctx.agent)
        return -22;
    if (!tools || ntools == 0) {
        agentc_free(a->owned_tools);
        a->owned_tools = NULL;
        a->n_owned_tools = 0;
        a->mcfg.tools = NULL;
        a->mcfg.ntools = 0;
        agentc_agent_set_tools(a->ctx.agent, NULL, 0);
        return 0;
    }
    /* The host keeps ownership; the engine only borrows the table for the
     * agent's lifetime (documented in agentc_sdk.h). */
    agentc_free(a->owned_tools);
    a->owned_tools = NULL;
    a->n_owned_tools = 0;
    a->mcfg.tools = tools;
    a->mcfg.ntools = ntools;
    agentc_agent_set_tools(a->ctx.agent, tools, ntools);
    return 0;
}

int agentc_sdk_agent_submit(AgcSdkAgent *a, const char *text) {
    if (!a || a->magic != SDK_AGENT_MAGIC || !a->ctx.agent)
        return -22;
    /* One mechanism: a synchronous submit is the cooperative run pumped to
     * completion in a tight loop. submit_async/step expose the same loop to a
     * host that interleaves several agents; the transcript and events are
     * identical (proved by tests/sdk_test.c). */
    int rc = agentc_agent_run_begin(a->ctx.agent, text ? text : "");
    if (rc < 0)
        return rc;
    for (;;) {
        rc = agentc_agent_run_step(a->ctx.agent);
        if (rc != AGENTC_AGENT_RUN_YIELD)
            return rc;
    }
}

int agentc_sdk_agent_submit_async(AgcSdkAgent *a, const char *text) {
    if (!a || a->magic != SDK_AGENT_MAGIC || !a->ctx.agent)
        return -22;
    return agentc_agent_run_begin(a->ctx.agent, text ? text : "");
}

int agentc_sdk_agent_step(AgcSdkAgent *a) {
    if (!a || a->magic != SDK_AGENT_MAGIC || !a->ctx.agent)
        return -22;
    return agentc_agent_run_step(a->ctx.agent);
}

bool agentc_sdk_agent_run_active(const AgcSdkAgent *a) {
    if (!a || a->magic != SDK_AGENT_MAGIC || !a->ctx.agent)
        return false;
    return agentc_agent_run_active(a->ctx.agent);
}

void agentc_sdk_agent_abort(AgcSdkAgent *a) {
    if (a && a->magic == SDK_AGENT_MAGIC && a->ctx.agent)
        agentc_agent_abort(a->ctx.agent);
}

void agentc_sdk_pump(AgcSdkAgent *a, int timeout_ms) {
    (void)a;
    if (!g_inited)
        return;
    agentc_mode_pump(NULL, timeout_ms);
}

int agentc_sdk_agent_new_session(AgcSdkAgent *a) {
    if (!a || a->magic != SDK_AGENT_MAGIC)
        return -22;
    return agentc_mode_new_session(&a->ctx);
}

const AgcTranscript *agentc_sdk_agent_transcript(const AgcSdkAgent *a) {
    if (!a || a->magic != SDK_AGENT_MAGIC)
        return NULL;
    return agentc_agent_transcript(a->ctx.agent);
}

const char *agentc_sdk_agent_last_error(const AgcSdkAgent *a) {
    if (!a || a->magic != SDK_AGENT_MAGIC)
        return g_error;
    return agentc_agent_last_error(a->ctx.agent);
}

/* ------------------------------------------------------------- registries */

size_t agentc_sdk_tools(AgcTool *out, size_t max) {
    return agentc_ext_tools(out, max);
}

size_t agentc_sdk_providers(const char **out, size_t max) {
    return agentc_setup_provider_names(out, max);
}

const AgcProvider *agentc_sdk_provider(const char *name) {
    return name ? agentc_setup_provider(name) : NULL;
}

bool agentc_sdk_needs_key(const char *name) {
    if (!name || !name[0])
        return true; /* fail closed */
    if (!agentc_setup_provider(name))
        return true;
    return agentc_setup_needs_key(name);
}
