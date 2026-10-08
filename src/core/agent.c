/* agent.c — the synchronous turn loop: request, stream, tools, repeat.
 *
 * Transport is injected; the default agentc_transport_http() is wired in main.c
 * (and is an empty stub until src/wire lands). Events are emitted in the
 * order documented in .agents/design/00-architecture.md.
 */
#include "agent.h"
#include "core/events.h"
#include "core/tools/jobs.h"
#include "ext.h"
#include "oauth.h"
#include "plat.h"
#include "prov/provider.h"

/* internal helpers from messages.c / prompt.c / retry.c / transport_http.c */
void agentc_msg_add_tool_result(AgcMsg *m, const char *call_id, const char *name,
                            const char *result);
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
    if (g_pump) g_pump(g_pump_ud, timeout_ms);
}

#define AGENT_DEFAULT_ATTEMPTS 5
#define AGENT_REQUEST_TIMEOUT_MS 600000

#define REQ_BUILD_FAIL (-1000)   /* internal, outside errno range */
#define REQ_MALFORMED  (-1001)
#define REQ_BLOCKED    (-1002)   /* an extension override blocked the request */

struct AgcAgent {
    const AgcProvider *prov;
    char *model;                  /* owned */
    char *api_key;                /* owned */
    char *base_url;               /* owned */
    char *system;                 /* owned explicit override; NULL = auto/build */
    char *system_override;        /* owned per-run before_agent_start override */
    AgcTransport transport;
    AgcTool *tools;               /* owned deep copy (agentc_agent_set_tools) */
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
    bool no_backoff;              /* tests: skip real sleeps */
    int timeout_ms;
    volatile bool cancel;
    bool auto_compact;
    u32 compact_reserve;          /* default 16384 */
    u32 compact_keep;             /* default 20000 */
    bool compact_done;
    int turn_index;               /* pi-style: reset on agent_start, ++ per turn */
    AgcTranscript tr;
    char last_error[1024];
};

/* Tool table ownership: the agent holds a deep copy of the array and of every
 * owned string field. `ud` and the function pointers stay borrowed (opaque), so
 * an MCP/extension registry may free the source strings and array immediately
 * after installing them. Both helpers keep the table consistent on replacement
 * and on free; they are the only writers of a->tools/ntools. */
static void agent_free_tools(AgcAgent *a) {
    if (!a->tools) return;
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
    if (!tools || n == 0) return NULL;
    if (n > (size_t)-1 / sizeof(AgcTool)) return NULL;   /* n * sizeof cannot wrap */
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
    if (!p) return NULL;
    /* Prefer a catalog entry on the provider's own wire, so the `openai` name
     * cannot default a chat model onto the Codex Responses api (or vice versa). */
    const AgcProviderOps *ops = agentc_provider_ops(p);
    const char *api = ops ? ops->api : NULL;
    const AgcModel *all[512];
    size_t n = agentc_model_all(all, 512);
    for (size_t i = 0; i < n; i++) {
        if (!agentc_streq(all[i]->provider, p->name)) continue;
        if (api && all[i]->api && !agentc_streq(all[i]->api, api)) continue;
        return all[i];
    }
    return NULL;
}

AgcAgent *agentc_agent_new(const AgcProvider *prov, const char *model) {
    AgcAgent *a = agentc_alloc(sizeof *a);
    a->prov = prov;
    a->transport.request = NULL;
    a->transport.ud = NULL;
    a->max_attempts = AGENT_DEFAULT_ATTEMPTS;
    a->timeout_ms = AGENT_REQUEST_TIMEOUT_MS;
    a->compact_reserve = 16384;
    a->compact_keep = 20000;
    agentc_transcript_init(&a->tr);
    if (model && model[0]) {
        a->model = agentc_strdup(model);
    } else {
        const AgcModel *m = default_model(prov);
        a->model = agentc_strdup(m ? m->id : "");
    }
    a->tr.model = a->model;
    a->tr.provider = prov ? prov->name : NULL;
    return a;
}

void agentc_agent_free(AgcAgent *a) {
    if (!a) return;
    agentc_transcript_free(&a->tr);
    agent_free_tools(a);
    agentc_free(a->model);
    agentc_free(a->api_key);
    agentc_free(a->base_url);
    agentc_free(a->system);
    agentc_free(a->system_override);
    agentc_free(a);
}

/* An api_key is written into a request header; a CR/LF from a hostile env,
 * config, flag or OAuth token would let the value inject another header. Store
 * a copy with every C0 control byte removed (normal bytes untouched). */
static char *header_value_sanitize(const char *s) {
    if (!s) return NULL;
    size_t n = agentc_strlen(s);
    char *out = agentc_alloc(n + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20) continue;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return out;
}

/* The setters duplicate the new value before freeing the old one, so passing
 * the agent's own current pointer (a caller that read it back first) is safe. */
void agentc_agent_set_api_key(AgcAgent *a, const char *key) {
    if (!a) return;
    char *copy = header_value_sanitize(key);
    agentc_free(a->api_key);
    a->api_key = copy;
}

void agentc_agent_set_base_url(AgcAgent *a, const char *url) {
    if (!a) return;
    char *copy = url ? agentc_strdup(url) : NULL;
    agentc_free(a->base_url);
    a->base_url = copy;
}

/* Explicit system prompt; NULL restores the auto/build mode. The effective
 * prompt is recomputed per turn by refresh_system() below. */
void agentc_agent_set_system(AgcAgent *a, const char *text) {
    if (!a) return;
    char *copy = text ? agentc_strdup(text) : NULL;
    agentc_free(a->system);
    a->system = copy;
}

void agentc_agent_set_transport(AgcAgent *a, AgcTransport t) {
    if (!a) return;
    a->transport = t;
}

AgcTransport agentc_agent_transport(const AgcAgent *a) {
    AgcTransport empty;
    agentc_memset(&empty, 0, sizeof empty);
    return a ? a->transport : empty;
}

void agentc_agent_set_tool_veto(AgcAgent *a, AgcToolVetoFn cb, void *ud) {
    a->veto = cb;
    a->veto_ud = ud;
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
    if (a && a->recompose) a->recompose(a->recompose_ud, a);
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
static bool want_override(const char *p) { return agentc_ext_wants(p); }

/* Observe point with no typed event: quiet still suppresses delivery, exactly
 * like a quiet-gated typed event. */
static bool want_observe(const char *p) {
    return !agentc_events_quiet() && agentc_ext_wants(p);
}

static const char *stop_name(int stop) {
    switch (stop) {
    case AGENTC_STOP_STOP: return "stop";
    case AGENTC_STOP_LENGTH: return "length";
    case AGENTC_STOP_TOOLUSE: return "tool_use";
    case AGENTC_STOP_ERROR: return "error";
    case AGENTC_STOP_ABORTED: return "aborted";
    default: return "pending";
    }
}

/* Numeric thinking level -> event name. The CLI parsers produce only 0/1/3/4;
 * a config-provided numeric 2 normalizes to "low" so every 0..4 value has a
 * stable, printable name. */
static const char *thinking_name(int level) {
    switch (level) {
    case 0: return "off";
    case 1:
    case 2: return "low";
    case 3: return "medium";
    default: return "high";
    }
}

/* Effective result of a `fields` point: the accumulated patch merged over the
 * payload. Returns NULL when the hook returned no patch. */
static char *hook_effective(const AgcBuf *payload, const char *result_json) {
    if (!result_json) return NULL;
    const char *base = (payload && payload->p) ? (const char *)payload->p : "{}";
    return agentc_ext_merge_fields(base, result_json);
}

/* Append the `entries` array of a turn_end / agent_before_settle result through
 * the host entry sink. Returns the result's `continue` flag. Malformed entries
 * are logged and skipped. */
static bool hook_apply_entries(const char *eff_json) {
    if (!eff_json) return false;
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
                agentc_logf(2, "ext: entries[%llu] is malformed, ignored",
                            (unsigned long long)i);
                continue;
            }
            AgcBuf dj = { 0 };
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
static int input_hook(const char *text, char **out) {
    *out = NULL;
    if (!want_override("input")) return 0;
    /* 0 = run, 1 = handled (deliberate quiet no-run), negative = blocked
     * (fail-closed: the caller reports last_error and returns that errno). */
    AgcBuf p = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &p);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "text");
    agentc_jsonw_cstr(&w, text ? text : "");
    agentc_jsonw_key(&w, "source");
    agentc_jsonw_cstr(&w, "user");
    agentc_jsonw_end(&w);
    AgcExtResult r = agentc_ext_emit("input", (const char *)p.p);
    int handled = 0;
    if (r.blocked) {
        agentc_logf(2, "ext: input blocked, no run");
        handled = -5; /* EIO: the fail-closed block is a visible run failure */
    } else if (r.result_json) {
        char *eff = hook_effective(&p, r.result_json);
        if (eff) {
            AgcJsonArena *arena = agentc_json_arena_new(0);
            AgcJson *o = agentc_json_parse_in(arena, eff, agentc_strlen(eff));
            const char *action = agentc_json_get_str(o, "action");
            if (action && agentc_streq(action, "handled")) {
                handled = 1;
            } else if (action && agentc_streq(action, "transform")) {
                const char *nt = agentc_json_get_str(o, "text");
                if (nt) *out = agentc_strdup(nt);
                else agentc_logf(2, "ext: input transform without text, ignored");
            } else if (action && !agentc_streq(action, "continue")) {
                agentc_logf(2, "ext: input action '%s' ignored", action);
            }
            agentc_json_arena_free(arena);
            agentc_free(eff);
        }
    }
    agentc_free(r.result_json);
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
    AgcExtContext ctx = { 0 };
    ctx.system_prompt = effective ? effective : "";
    agentc_ext_set_context(&ctx);
}

/* before_agent_start override: a systemPrompt replaces the built prompt for the
 * rest of the run (stored per-run so the turn refresh cannot clobber it) and is
 * re-published to the extension context; message is logged and ignored. */
static void before_agent_start_hook(AgcAgent *a, const char *prompt) {
    if (!want_override("before_agent_start")) return;
    AgcBuf p = { 0 };
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
                AgcExtContext ctx = { 0 };
                ctx.system_prompt = a->tr.system;
                agentc_ext_set_context(&ctx);
            }
            const char *message = agentc_json_get_str(o, "message");
            if (message && message[0]) agentc_logf(1, "ext: before_agent_start: %s", message);
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
    if (agentc_json_type(content) != AGENTC_JSON_ARR) return false;
    size_t n = agentc_json_len(content);
    for (size_t i = 0; i < n; i++) {
        const AgcJson *b = agentc_json_at(content, i);
        if (agentc_json_type(b) != AGENTC_JSON_OBJ) return false;
        const char *t = agentc_json_get_str(b, "type");
        if (!t) return false;
        if (agentc_streq(t, "text") || agentc_streq(t, "thinking")) {
            const AgcJson *txt = agentc_json_get(b, "text");
            int ty = agentc_json_type(txt);
            if (txt && ty != AGENTC_JSON_STR && ty != AGENTC_JSON_NULL) return false;
        } else if (agentc_streq(t, "tool_call")) {
            const char *id = agentc_json_get_str(b, "id");
            const char *name = agentc_json_get_str(b, "name");
            if (!id || !id[0] || !name || !name[0]) return false;
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
            AgcBlock *blk = agentc_msg_block_new(
                m, agentc_streq(t, "text") ? AGENTC_BLK_TEXT : AGENTC_BLK_THINK);
            if (txt) agentc_msg_block_append(blk, txt, agentc_strlen(txt));
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
            AgcBuf ab = { 0 };
            AgcJsonW aw;
            agentc_jsonw_init(&aw, &ab);
            agentc_json_emit(&aw, args);
            blk->tool_args = agentc_strdup((const char *)ab.p);
            agentc_buf_free(&ab);
        } else {
            blk->tool_args = agentc_strdup("{}");
        }
    }
    if (preserve_terminal) return;
    const AgcJson *sr = agentc_json_get(msg, "stop_reason");
    if (sr) {
        const char *s = agentc_json_get_str(msg, "stop_reason");
        if (s && agentc_streq(s, "stop")) m->stop_reason = AGENTC_STOP_STOP;
        else if (s && agentc_streq(s, "length")) m->stop_reason = AGENTC_STOP_LENGTH;
        else if (s && agentc_streq(s, "tool_use")) m->stop_reason = AGENTC_STOP_TOOLUSE;
        else if (s && agentc_streq(s, "error")) m->stop_reason = AGENTC_STOP_ERROR;
        else if (s && agentc_streq(s, "aborted")) m->stop_reason = AGENTC_STOP_ABORTED;
        else if (s && agentc_streq(s, "pending")) m->stop_reason = AGENTC_STOP_PENDING;
        else agentc_logf(0, "ext: message_end stop_reason ignored");
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
    if (!want_override("message_end")) return;
    AgcBuf p = { 0 };
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
    AgcExtResult r = agentc_ext_emit("message_end", (const char *)p.p);
    if (r.blocked) {
        agentc_logf(2, "ext: message_end blocked, keeping the original message");
    } else if (r.result_json) {
        char *eff = hook_effective(&p, r.result_json);
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
    agentc_free(r.result_json);
    agentc_buf_free(&p);
}

/* tool_result override: dispatched directly before the transcript append and
 * the observer. Returns an owned replacement text (or NULL) and updates the
 * is_error flag. */
static char *tool_result_hook(const char *call_id, const char *tool_name,
                              const char *res, bool *is_error) {
    char *replacement = NULL;
    if (!want_override("tool_result")) return NULL;
    AgcBuf p = { 0 };
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
    AgcExtResult r = agentc_ext_emit("tool_result", (const char *)p.p);
    if (r.blocked) {
        agentc_logf(2, "ext: tool_result blocked, keeping the raw result");
    } else if (r.result_json) {
        /* A `null` in a fields patch deletes the key, so the patch is inspected
         * to distinguish "content": null (empty result) from an absent key. */
        bool content_null = false;
        {
            AgcJsonArena *pa = agentc_json_arena_new(0);
            AgcJson *po = agentc_json_parse_in(pa, r.result_json, agentc_strlen(r.result_json));
            const AgcJson *pc = agentc_json_get(po, "content");
            content_null = pc && agentc_json_type(pc) == AGENTC_JSON_NULL;
            agentc_json_arena_free(pa);
        }
        char *eff = hook_effective(&p, r.result_json);
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
                AgcBuf j = { 0 };
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
                    if (i > 0) agentc_buf_byte(&j, '\n');
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
    agentc_free(r.result_json);
    agentc_buf_free(&p);
    return replacement;
}

/* turn_end override: entries always apply, `continue` only on a non-hard turn.
 * Returns the requested continuation. Increments the pi-style turn index. */
static bool turn_end_hook(AgcAgent *a, int stop_reason, bool hard) {
    bool want_continue = false;
    if (want_override("turn_end")) {
        AgcBuf p = { 0 };
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "turn_index");
        agentc_jsonw_i64(&w, a->turn_index);
        agentc_jsonw_key(&w, "stop_reason");
        agentc_jsonw_cstr(&w, stop_name(stop_reason));
        agentc_jsonw_end(&w);
        AgcExtResult r = agentc_ext_emit("turn_end", (const char *)p.p);
        if (r.blocked) {
            agentc_logf(2, "ext: turn_end blocked, keeping the original turn");
        } else if (r.result_json) {
            char *eff = hook_effective(&p, r.result_json);
            if (eff) {
                want_continue = hook_apply_entries(eff) && !hard;
                agentc_free(eff);
            }
        }
        agentc_free(r.result_json);
        agentc_buf_free(&p);
    }
    a->turn_index++;
    return want_continue;
}

/* agent_before_settle override: entries apply, `continue` is the caller's to
 * honor (rc == 0 and no abort). A blocked result settles normally. */
static bool settle_hook(const char *outcome) {
    bool want_continue = false;
    if (want_override("agent_before_settle")) {
        AgcBuf p = { 0 };
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "outcome");
        agentc_jsonw_cstr(&w, outcome ? outcome : "completed");
        agentc_jsonw_end(&w);
        AgcExtResult r = agentc_ext_emit("agent_before_settle", (const char *)p.p);
        if (r.blocked) {
            agentc_logf(2, "ext: agent_before_settle blocked, settling normally");
        } else if (r.result_json) {
            char *eff = hook_effective(&p, r.result_json);
            if (eff) {
                want_continue = hook_apply_entries(eff);
                agentc_free(eff);
            }
        }
        agentc_free(r.result_json);
        agentc_buf_free(&p);
    }
    return want_continue;
}

int agentc_agent_set_model(AgcAgent *a, const char *model) {
    if (!a || !model || !model[0]) return -22;
    if (a->model && agentc_streq(a->model, model)) return 0;   /* no change, no emit */
    /* The transcript borrows the old model string, so snapshot it for the
     * payload before the free. */
    char *previous = agentc_strdup(a->model ? a->model : "");
    agentc_free(a->model);
    a->model = agentc_strdup(model);
    a->tr.model = a->model;   /* the transcript borrows a->model; keep it live */
    if (want_observe("model_select")) {
        AgcBuf p = { 0 };
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "provider");
        agentc_jsonw_cstr(&w, a->tr.provider ? a->tr.provider
                                             : (a->prov ? a->prov->name : ""));
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
    if (!a) return;
    /* Copy before freeing: the caller may hand back a pointer into the table we
     * are replacing (e.g. a rebuild that re-reads the agent's own tools). */
    AgcTool *copy = agent_copy_tools(tools, n);
    agent_free_tools(a);
    a->tools = copy;
    a->ntools = copy ? n : 0;
}

void agentc_agent_set_events(AgcAgent *a, AgcEventFn cb, void *ud) {
    if (!a) return;
    a->events = cb;
    a->events_ud = ud;
}

void agentc_agent_set_thinking(AgcAgent *a, int level) {
    if (!a) return;
    if (level < 0) level = 0;
    if (level > 4) level = 4;
    if (a->thinking == level) return;   /* numeric no-op: no emit */
    const char *previous = thinking_name(a->thinking);
    a->thinking = level;
    if (!want_observe("thinking_level_select")) return;
    AgcBuf p = { 0 };
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
    if (!a) return;
    a->max_tokens = max_tokens;
}

void agentc_agent_set_retry(AgcAgent *a, int max_attempts) {
    if (!a) return;
    if (max_attempts > 0) a->max_attempts = max_attempts;
}

void agentc_agent_set_record(AgcAgent *a, AgcBuf *sink) {
    if (!a) return;
    a->record = sink;
}

void agentc_agent_set_insecure(AgcAgent *a, bool insecure) {
    if (!a) return;
    a->insecure = insecure;
}

void agentc_agent_set_auto_compact(AgcAgent *a, bool on) {
    if (!a) return;
    a->auto_compact = on;
}

void agentc_agent_set_compact_limits(AgcAgent *a, u32 reserve_tokens, u32 keep_recent_tokens) {
    if (!a) return;
    if (reserve_tokens > 0) a->compact_reserve = reserve_tokens;
    if (keep_recent_tokens > 0) a->compact_keep = keep_recent_tokens;
}

bool agentc_agent_compacted(const AgcAgent *a) { return a ? a->compact_done : false; }

u32 agentc_agent_context_tokens(const AgcAgent *a) {
    return a ? agentc_compact_estimate(&a->tr) : 0;
}

void agentc_agent_abort(AgcAgent *a) {
    if (!a) return;
    a->cancel = true;
}

/* Resume support: deep-copy a replayed transcript into an idle agent. */
int agentc_agent_load(AgcAgent *a, const AgcTranscript *t) {
    if (!a || !t) return -22 /* EINVAL */;
    if (t == &a->tr) return 0;

    agentc_transcript_free(&a->tr);
    agentc_transcript_init(&a->tr);
    a->compact_done = false;
    a->tr.model = a->model;
    a->tr.provider = a->prov ? a->prov->name : NULL;
    if (t->system) a->tr.system = agentc_strdup(t->system);

    for (size_t i = 0; i < t->n; i++) {
        const AgcMsg *src = &t->msgs[i];
        AgcMsg *dst = agentc_transcript_push(&a->tr, src->role);
        dst->stop_reason = src->stop_reason;
        dst->usage = src->usage;
        dst->ts_ms = src->ts_ms;
        if (src->error) dst->error = agentc_strdup(src->error);
        for (size_t b = 0; b < src->nblocks; b++) {
            const AgcBlock *sb = &src->blocks[b];
            AgcBlock *db = agentc_msg_block_new(dst, sb->type);
            if (sb->text) {
                db->text = agentc_strdup_len(sb->text, sb->text_len);
                db->text_len = sb->text_len;
            }
            if (sb->tool_id) db->tool_id = agentc_strdup(sb->tool_id);
            if (sb->tool_name) db->tool_name = agentc_strdup(sb->tool_name);
            if (sb->tool_args) db->tool_args = agentc_strdup(sb->tool_args);
        }
    }
    return 0;
}

const AgcTranscript *agentc_agent_transcript(const AgcAgent *a) { return a ? &a->tr : NULL; }

const char *agentc_agent_last_error(const AgcAgent *a) {
    if (!a || !a->last_error[0]) return NULL;
    return a->last_error;
}

/* test hook: disable real backoff sleeps (declared extern by tests) */
void agentc_agent_test_no_backoff(AgcAgent *a) {
    if (a) a->no_backoff = true;
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
    if (!a || !a->cancel) return false;
    return ev != AGENTC_EV_ERROR && ev != AGENTC_EV_AGENT_END && ev != AGENTC_EV_TURN_END &&
           ev != AGENTC_EV_MSG_END && ev != AGENTC_EV_TOOL_EXEC_END;
}

void agentc_agent_event_own(AgcAgent *a, int ev, const void *data) {
    if (a && a->events) a->events(a->events_ud, ev, data);
}

static void emit(AgcAgent *a, int ev, const void *data) {
    agentc_events_emit(a, ev, data);
}

/* Provider request path, with `{model}` substituted when the row's path carries
 * it (the native Gemini API puts the model in the path). */
static void provider_path(const AgcAgent *a, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    const AgcProviderOps *ops = agentc_provider_ops(a->prov);
    const char *p = (ops && ops->path) ? ops->path : "/";
    const char *m = agentc_str_str(p, "{model}");
    if (!m) {
        agentc_snprintf(out, cap, "%s", p);
        return;
    }
    const char *model = (a->model && a->model[0]) ? a->model : "";
    size_t pre = (size_t)(m - p);
    if (pre > cap - 1) pre = cap - 1;
    agentc_memcpy(out, p, pre);
    out[pre] = 0;
    agentc_snprintf(out + pre, cap - pre, "%s%s", model, m + 7);
}

static void build_url(char *out, size_t cap, const char *base, const char *path) {
    char b[1024];
    size_t bl = base ? agentc_strlen(base) : 0;
    if (bl >= sizeof b) bl = sizeof b - 1;
    if (bl) agentc_memcpy(b, base, bl);
    b[bl] = 0;
    while (bl > 0 && b[bl - 1] == '/') b[--bl] = 0;
    agentc_snprintf(out, cap, "%s%s", b, path);
}

static void provider_url(const AgcAgent *a, char *out, size_t cap) {
    const char *base = (a->base_url && a->base_url[0]) ? a->base_url
                                                       : a->prov->default_base_url;
    char path[1024];
    provider_path(a, path, sizeof path);
    build_url(out, cap, base, path);
}

/* request_stream() result: the map failure and the transport retry hints the
 * caller's retry loop uses. out may be NULL (compaction). */
typedef struct {
    bool map_failed;
    i64 retry_after_ms;
    int last_status;
} AgcStreamResult;

static int request_stream(AgcAgent *a, const AgcRequest *r, bool emit_events,
                          AgcStreamState *st, AgcStreamResult *out);

static const AgcTool *find_tool(const AgcAgent *a, const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < a->ntools; i++)
        if (a->tools[i].name && agentc_streq(a->tools[i].name, name)) return &a->tools[i];
    return NULL;
}

static size_t last_toolcall_args_len(const AgcMsg *m) {
    for (size_t i = m->nblocks; i > 0; i--) {
        const AgcBlock *b = &m->blocks[i - 1];
        if (b->type == AGENTC_BLK_TOOLCALL) return b->tool_args ? agentc_strlen(b->tool_args) : 0;
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
        if (b->type == AGENTC_BLK_TOOLCALL && b->tool_args && b->tool_args[0]) return true;
    }
    return false;
}

/* ------------------------------------------------------------ compaction */

static const char *compact_role_name(int role) {
    switch (role) {
    case AGENTC_ROLE_USER: return "user";
    case AGENTC_ROLE_ASSISTANT: return "assistant";
    case AGENTC_ROLE_TOOL: return "tool";
    default: return "system";
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
        const char *call_id =
            (m->nblocks && m->blocks[0].tool_id) ? m->blocks[0].tool_id : "";
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
    if (!want_observe("session_compact_failed")) return;
    AgcBuf p = { 0 };
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
    if (!want_observe("session_compact")) return;
    AgcBuf p = { 0 };
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
    if (!a->prov || !a->transport.request) {
        compact_failed(a, reason);
        return a->prov ? -107 /* ENOTCONN */ : -22 /* EINVAL */;
    }
    u32 before = agentc_compact_estimate(&a->tr);
    size_t cut = agentc_compact_cut(&a->tr, a->compact_keep);
    if (cut == 0 || cut >= a->tr.n) return 0;
    size_t kept = a->tr.n - cut;

    /* session_before_compact: first-policy override after the cut is known. A
     * blocked/failed result or a handled result without a non-empty summary
     * cancels compaction (no event); a non-empty summary skips the provider
     * request and is spliced verbatim. */
    char *ext_summary = NULL;
    if (want_override("session_before_compact")) {
        AgcBuf p = { 0 };
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "reason");
        agentc_jsonw_cstr(&w, reason);
        agentc_jsonw_key(&w, "first_kept");
        agentc_jsonw_u64(&w, (u64)cut);
        agentc_jsonw_key(&w, "messages");
        agentc_jsonw_arr(&w);
        for (size_t i = 0; i < cut; i++) compact_message_json(&w, &a->tr.msgs[i]);
        agentc_jsonw_end(&w);
        agentc_jsonw_end(&w);
        AgcExtResult r = agentc_ext_emit("session_before_compact", (const char *)p.p);
        bool cancelled = r.blocked;
        if (!cancelled && r.result_json) {
            AgcJsonArena *arena = agentc_json_arena_new(0);
            AgcJson *o =
                agentc_json_parse_in(arena, r.result_json, agentc_strlen(r.result_json));
            const char *s = agentc_json_get_str(o, "summary");
            if (s && s[0]) {
                ext_summary = agentc_strdup(s);
            } else if (agentc_json_get_bool(o, "cancel", false)) {
                cancelled = true;
            } else if (r.handled) {
                cancelled = true;   /* handled without a summary */
            }
            /* No summary and no cancel: the extension made no decision, so the
             * default provider summary runs. */
            agentc_json_arena_free(arena);
        } else if (!cancelled && r.handled) {
            cancelled = true;
        }
        agentc_free(r.result_json);
        agentc_buf_free(&p);
        if (cancelled) return 0;
    }

    AgcTranscript prefix;
    agentc_memset(&prefix, 0, sizeof prefix);
    prefix.msgs = a->tr.msgs;
    prefix.n = cut;
    prefix.cap = cut;
    prefix.model = a->tr.model;
    prefix.provider = a->tr.provider;
    prefix.system = agentc_compact_system_prompt();

    AgcBuf sum = { 0 };
    char *summary = NULL;
    bool from_extension = false;
    int rc = 0;

    if (ext_summary) {
        summary = ext_summary;
        ext_summary = NULL;
        from_extension = true;
    } else {
        const AgcModel *m = agentc_model_find(a->prov->name, a->model);
        i64 max_tokens = (i64)((u64)a->compact_reserve * 8 / 10);
        if (m && max_tokens > (i64)m->max_tokens) max_tokens = (i64)m->max_tokens;
        if (max_tokens < 256) max_tokens = 256;
        if (a->max_tokens > 0 && max_tokens > a->max_tokens) max_tokens = a->max_tokens;

        AgcRequest r;
        agentc_memset(&r, 0, sizeof r);
        r.provider = a->prov->name;
        r.model = a->model;
        r.api_key = a->api_key;
        r.base_url = a->base_url;
        r.system = agentc_compact_system_prompt();
        r.tools = NULL;
        r.ntools = 0;
        r.thinking_level = 0;
        r.account_id = agentc_oauth_account_id(a->prov->name);
        r.max_tokens = max_tokens;
        r.transcript = &prefix;

        /* The summary is an ordinary streamed request; quiet suppresses every
         * front end so only this function consumes the deltas. */
        bool quiet_prev = agentc_events_quiet();
        agentc_events_set_quiet(true);

        AgcStreamState st;
        rc = agentc_stream_state_init(a->prov, &st);
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
            int trc = request_stream(a, &r, false, &st, &res);
            if (trc == REQ_BUILD_FAIL || trc == REQ_MALFORMED || trc == REQ_BLOCKED) {
                set_error(a, trc == REQ_BLOCKED
                                 ? "compaction: request blocked by extension"
                                 : "compaction: cannot build the summarization request");
                rc = -1;
            } else if (trc != 0) {
                set_error(a, "compaction: summarization request failed");
                rc = trc;
            } else {
                int fr = a->prov->finish(&st);
                if (fr != 0 || res.map_failed) {
                    set_error(a, st.error[0] ? st.error
                                             : "compaction: summary response had no text");
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

        for (size_t i = 0; i < cut; i++) agentc_msg_free(&a->tr.msgs[i]);
        if (kept) agentc_memmove(&a->tr.msgs[1], &a->tr.msgs[cut], kept * sizeof(AgcMsg));
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
    if (!a) return -22; /* EINVAL */
    return compact_now(a, false);
}

/* ------------------------------------------------------------- streaming */

typedef struct {
    AgcAgent *a;
    AgcStreamState *st;
    AgcSse sse;
    bool failed;
} SseCtx;

static void emit_text(AgcAgent *a, const AgcBlock *b, const char *p, size_t n) {
    if (!p || !n) return;
    AgcTextDelta d = { p, n };
    emit(a, b->type == AGENTC_BLK_THINK ? AGENTC_EV_THINK_DELTA : AGENTC_EV_TEXT_DELTA, &d);
}

static int on_sse_event(void *ud, const AgcSseEvent *ev) {
    SseCtx *c = ud;
    AgcAgent *a = c->a;
    AgcMsg *m = c->st->msg;
    if (a->cancel) return 1;

    if (want_observe("provider_stream_event") && ev && ev->data) {
        AgcBuf p = { 0 };
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "provider");
        agentc_jsonw_cstr(&w, a->prov->name);
        agentc_jsonw_key(&w, "api");
        agentc_jsonw_cstr(&w, a->prov->api);
        agentc_jsonw_key(&w, "model");
        agentc_jsonw_cstr(&w, a->model ? a->model : "");
        agentc_jsonw_key(&w, "event");
        agentc_jsonw_cstr(&w, ev->event ? ev->event : "");
        agentc_jsonw_key(&w, "data");
        /* The wire layer passes non-JSON payloads through (the OpenAI `[DONE]`
         * sentinel), so keep the hook payload valid JSON: raw when it parses,
         * a quoted string otherwise. */
        {
            AgcJsonArena *ja = agentc_json_arena_new(0);
            AgcJson *jd = agentc_json_parse_in(ja, ev->data ? ev->data : "", ev->data_len);
            if (jd) agentc_jsonw_raw(&w, ev->data ? ev->data : "", ev->data_len);
            else agentc_jsonw_str(&w, ev->data ? ev->data : "", ev->data_len);
            agentc_json_arena_free(ja);
        }
        agentc_jsonw_end(&w);
        AgcExtResult r = agentc_ext_emit("provider_stream_event", (const char *)p.p);
        agentc_free(r.result_json);
        agentc_buf_free(&p);
    }

    size_t nb = m->nblocks;
    size_t last_text = nb ? m->blocks[nb - 1].text_len : 0;
    size_t last_args = last_toolcall_args_len(m);

    if (a->prov->map_sse(c->st, ev) != 0) c->failed = true;

    if (m->nblocks > nb) {
        AgcBlock *b = &m->blocks[m->nblocks - 1];
        if (b->type == AGENTC_BLK_TEXT || b->type == AGENTC_BLK_THINK)
            emit_text(a, b, b->text, b->text_len);
        else if (b->type == AGENTC_BLK_TOOLCALL && b->tool_args)
            emit(a, AGENTC_EV_TOOL_ARGS_DELTA,
                 &(AgcTextDelta){ b->tool_args, agentc_strlen(b->tool_args) });
    } else if (nb > 0) {
        AgcBlock *b = &m->blocks[nb - 1];
        if (b->text_len > last_text && (b->type == AGENTC_BLK_TEXT || b->type == AGENTC_BLK_THINK))
            emit_text(a, b, b->text + last_text, b->text_len - last_text);
        if (b->type == AGENTC_BLK_TOOLCALL && b->tool_args) {
            size_t now = agentc_strlen(b->tool_args);
            if (now > last_args)
                emit(a, AGENTC_EV_TOOL_ARGS_DELTA,
                     &(AgcTextDelta){ b->tool_args + last_args, now - last_args });
        }
    }
    return 0;
}

static int on_sse_chunk(void *ud, const void *p, size_t n) {
    SseCtx *c = ud;
    if (c->a->cancel) return 1;
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
} ToolSlot;

typedef struct {
    AgcAgent *a;
    ToolSlot *slots;
    size_t base;                  /* global index of this chunk's first call */
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
        emit(c->a, AGENTC_EV_TOOL_EXEC_START, &ex);
        s->started = true;
        return 0;
    }
    if (job->out.len > 0) agentc_buf_push(&s->out, job->out.p, job->out.len);
    s->is_error = job->is_error;
    s->duration_ms = job->duration_ms;
    s->ran = true;
    /* A pre-aborted call is answered without a start event, so it must not
     * emit an end event either (the old loop's backfill path). */
    if (s->started) {
        ex.result = s->out.p != NULL ? (const char *)s->out.p : "";
        ex.is_error = s->is_error;
        ex.duration_ms = s->duration_ms;
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
static int request_stream(AgcAgent *a, const AgcRequest *r, bool emit_events,
                          AgcStreamState *st, AgcStreamResult *out) {
    char url[1200];
    provider_url(a, url, sizeof url);

    if (out) {
        out->map_failed = false;
        out->retry_after_ms = 0;
        out->last_status = 0;
    }

    AgcBuf req = { 0 };
    char path[1024];
    provider_path(a, path, sizeof path);
    if (a->prov->build_request(&req, r, NULL, path) != 0) {
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
    AgcBuf body_owned = { 0 };

    /* before_provider_headers: direct override on the assembled header block,
     * after the head/body split and before send. The result is a flat header
     * patch (null deletes); a blocked or failed (fail-closed) result fails the
     * request. Dispatch is not quiet-gated: overrides must decide. */
    if (want_override("before_provider_headers")) {
        char *hjson = agentc_ext_headers_to_json(headers);
        AgcBuf p = { 0 };
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "provider");
        agentc_jsonw_cstr(&w, a->prov->name);
        agentc_jsonw_key(&w, "api");
        agentc_jsonw_cstr(&w, a->prov->api);
        agentc_jsonw_key(&w, "model");
        agentc_jsonw_cstr(&w, a->model ? a->model : "");
        agentc_jsonw_key(&w, "headers");
        agentc_jsonw_raw(&w, hjson, agentc_strlen(hjson));
        agentc_jsonw_end(&w);
        AgcExtResult r = agentc_ext_emit("before_provider_headers", (const char *)p.p);
        if (r.blocked) {
            agentc_free(r.result_json);
            agentc_free(hjson);
            agentc_buf_free(&p);
            agentc_free(headers);
            agentc_buf_free(&req);
            return REQ_BLOCKED;
        }
        if (r.result_json) {
            char *patched = agentc_ext_headers_apply_patch(headers, r.result_json);
            if (patched) {
                agentc_free(headers);
                headers = patched;
            } else {
                agentc_logf(2, "ext: before_provider_headers patch ignored");
            }
        }
        agentc_free(r.result_json);
        agentc_free(hjson);
        agentc_buf_free(&p);
    }

    /* before_provider_request: consuming override on the request object. A
     * returned `payload` object replaces the serialized body; Content-Length
     * and --record reflect the replacement because the transport derives the
     * length from body_len. */
    if (want_override("before_provider_request")) {
        AgcBuf p = { 0 };
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "provider");
        agentc_jsonw_cstr(&w, a->prov->name);
        agentc_jsonw_key(&w, "api");
        agentc_jsonw_cstr(&w, a->prov->api);
        agentc_jsonw_key(&w, "model");
        agentc_jsonw_cstr(&w, a->model ? a->model : "");
        agentc_jsonw_key(&w, "url");
        agentc_jsonw_cstr(&w, url);
        agentc_jsonw_key(&w, "payload");
        if (body && body_len) agentc_jsonw_raw(&w, (const char *)body, body_len);
        else agentc_jsonw_null(&w);
        agentc_jsonw_end(&w);
        AgcExtResult r = agentc_ext_emit("before_provider_request", (const char *)p.p);
        if (r.blocked) {
            agentc_free(r.result_json);
            agentc_buf_free(&p);
            agentc_free(headers);
            agentc_buf_free(&req);
            return REQ_BLOCKED;
        }
        if (r.result_json) {
            char *eff = hook_effective(&p, r.result_json);
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
        agentc_free(r.result_json);
        agentc_buf_free(&p);
    }

    SseCtx ctx;
    agentc_memset(&ctx, 0, sizeof ctx);
    ctx.a = a;
    ctx.st = st;
    agentc_sse_init(&ctx.sse);

    int trc = a->transport.request(a->transport.ud, url, headers, body, body_len,
                                   on_sse_chunk, &ctx, a->timeout_ms, a->record);
    i64 retry_after_ms = agentc_transport_http_retry_after_ms();
    int last_status = agentc_transport_http_last_status();
    if (out) {
        out->retry_after_ms = retry_after_ms;
        out->last_status = last_status;
    }
    if (emit_events && want_observe("after_provider_response")) {
        AgcBuf p = { 0 };
        AgcJsonW w;
        agentc_jsonw_init(&w, &p);
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "provider");
        agentc_jsonw_cstr(&w, a->prov->name);
        agentc_jsonw_key(&w, "status");
        agentc_jsonw_i64(&w, last_status);
        agentc_jsonw_key(&w, "headers");
        const char *rh = a->transport.response_headers
                             ? a->transport.response_headers(a->transport.ud)
                             : NULL;
        if (!rh) rh = "{}";
        agentc_jsonw_raw(&w, rh, agentc_strlen(rh));
        agentc_jsonw_key(&w, "transport_error");
        agentc_jsonw_i64(&w, trc);
        agentc_jsonw_end(&w);
        AgcExtResult r = agentc_ext_emit("after_provider_response", (const char *)p.p);
        agentc_free(r.result_json);
        agentc_buf_free(&p);
    }
    /* A clean transport completion is an SSE EOF: dispatch an event the server
     * terminated without its blank line instead of dropping the final token. A
     * failed transfer stays undispatched (the partial event is not trustworthy). */
    if (trc == 0) agentc_sse_finish(&ctx.sse, on_sse_event, &ctx);
    agentc_sse_free(&ctx.sse);
    agentc_free(headers);
    agentc_buf_free(&body_owned);
    agentc_buf_free(&req);
    if (out) out->map_failed = ctx.failed;
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
        if (!tc) break;
        const char *id = tc->tool_id;
        const char *name = tc->tool_name;
        AgcMsg *tm = agentc_transcript_push(&a->tr, AGENTC_ROLE_TOOL);
        agentc_msg_add_tool_result(tm, id, name, why);
        tm->error = agentc_strdup("error");
        if (a->observer) a->observer(a->observer_ud, tm);
    }
}

/* ---------------------------------------------------------------- submit */

/* Sleep a retry backoff in <=50 ms slices so an abort interrupts it promptly
 * instead of waiting out a long Retry-After or exponential delay. With a front
 * end pump installed the slice runs it, so the UI repaints and can set the cancel
 * flag; otherwise the slice is a plain sleep. `no_backoff` (tests) skips it. */
#define BACKOFF_SLICE_MS 50
static void backoff_sleep(AgcAgent *a, i64 ms) {
    if (ms <= 0 || a->no_backoff) return;
    while (ms > 0 && !a->cancel) {
        i64 slice = ms < BACKOFF_SLICE_MS ? ms : BACKOFF_SLICE_MS;
        if (g_pump) agentc_pump((int)slice);
        else os_sleep_ns(slice * 1000000);
        ms -= slice;
    }
}

int agentc_agent_submit(AgcAgent *a, const char *text) {
    if (!a || !a->prov) return -22; /* EINVAL */
    a->cancel = false;
    a->last_error[0] = 0;

    /* input override: the first decision of a run. A handled/blocked prompt
     * never emits agent_start/agent_end/agent_settled. */
    const char *eff_text = text ? text : "";
    char *input_owned = NULL;
    int input_rc = input_hook(eff_text, &input_owned);
    if (input_rc < 0) {
        /* A fail-closed input handler blocks the run visibly: the front ends
         * report last_error and exit nonzero instead of a silent no-run. */
        agentc_free(input_owned);
        set_error(a, "input blocked by extension");
        emit(a, AGENTC_EV_ERROR, a->last_error);
        return input_rc;
    }
    if (input_rc > 0) {
        agentc_free(input_owned);
        return 0;
    }
    if (input_owned) eff_text = input_owned;

    /* System prompt: reset the per-run override, then build/publish the
     * effective prompt; before_agent_start may override it for the run. */
    agentc_free(a->system_override);
    a->system_override = NULL;
    refresh_system(a);
    a->tr.model = a->model;
    a->tr.provider = a->prov->name;
    before_agent_start_hook(a, eff_text);
    emit(a, AGENTC_EV_AGENT_START, NULL);
    a->turn_index = 0;

    int rc = 0;
    int terminal_stop = AGENTC_STOP_PENDING;
    int continuations = 0;
    bool have_transport = a->transport.request != NULL;

    if (!have_transport) {
        set_error(a, "no transport configured");
        emit(a, AGENTC_EV_ERROR, a->last_error);
        rc = -1;
        terminal_stop = AGENTC_STOP_ERROR;
    } else {
        AgcMsg *um = agentc_transcript_push(&a->tr, AGENTC_ROLE_USER);
        agentc_msg_add_text(um, eff_text, agentc_strlen(eff_text));
        if (a->observer) a->observer(a->observer_ud, um);
    }
    agentc_free(input_owned);
    input_owned = NULL;

    /* The settle loop: an agent_before_settle `continue` re-enters the turn
     * loop for one more turn without a second agent_start. */
    for (;;) {
        if (have_transport) {
            for (;;) {
                /* A recompose after the previous tool batch may have changed
                 * the tool table; refresh auto prompts before each turn. */
                refresh_system(a);
                emit(a, AGENTC_EV_TURN_START, &a->turn_index);
                if (a->auto_compact && a->prov) {
                    const AgcModel *cm = agentc_model_find(a->prov->name, a->model);
                    u32 window = cm ? cm->ctx_window : 0;
                    if (window > a->compact_reserve &&
                        agentc_agent_context_tokens(a) > window - a->compact_reserve) {
                        (void)compact_now(a, true);
                    }
                }
                size_t asst_idx = a->tr.n;
                AgcMsg *asst = agentc_transcript_push(&a->tr, AGENTC_ROLE_ASSISTANT);
                emit(a, AGENTC_EV_MSG_START, asst);
                bool turn_ok = false;

                int attempts = a->max_attempts > 0 ? a->max_attempts : AGENT_DEFAULT_ATTEMPTS;
                for (int attempt = 0; attempt < attempts; attempt++) {
                    if (a->cancel) break;
                    asst = &a->tr.msgs[asst_idx];
                    if (attempt > 0) {
                        agentc_msg_free(asst);
                        asst->role = AGENTC_ROLE_ASSISTANT;
                        asst->stop_reason = AGENTC_STOP_PENDING;
                        asst->ts_ms = (u64)(os_now_ns(OS_CLOCK_REALTIME) / 1000000);
                    }

                    AgcRequest r;
                    agentc_memset(&r, 0, sizeof r);
                    r.provider = a->prov->name;
                    r.model = a->model;
                    r.api_key = a->api_key;
                    r.base_url = a->base_url;
                    r.system = a->tr.system;
                    r.tools = a->tools;
                    r.ntools = a->ntools;
                    r.thinking_level = a->thinking;
                    r.max_tokens = a->max_tokens;
                    r.account_id = agentc_oauth_account_id(a->prov->name);
                    r.transcript = &a->tr;

                    AgcStreamState st;
                    if (agentc_stream_state_init(a->prov, &st) != 0) {
                        asst->stop_reason = AGENTC_STOP_ERROR;
                        set_error(a, "failed to initialize stream state");
                        break;
                    }
                    st.msg = asst;
                    AgcStreamResult res;
                    int trc = request_stream(a, &r, true, &st, &res);
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
                                              agentc_retry_backoff_ms(attempt,
                                                                      res.retry_after_ms));
                                if (a->cancel) goto attempt_done;
                                /* The abandoned attempt already rendered content:
                                 * tell front ends to drop it before the re-run. */
                                if (attempt_streamed(asst)) emit(a, AGENTC_EV_MSG_RESET, NULL);
                                retry = true;
                                goto attempt_done;
                            }
                            asst->stop_reason = AGENTC_STOP_ERROR;
                            set_error(a, "stream ended prematurely");
                            goto attempt_done;
                        }
                        int fr = a->prov->finish(&st);
                        if (st.error[0]) {
                            asst->stop_reason = AGENTC_STOP_ERROR;
                            set_error(a, st.error);
                        } else if (fr != 0) {
                            asst->stop_reason = AGENTC_STOP_ERROR;
                            set_error(a, "provider protocol error");
                        } else {
                            asst->stop_reason = st.stop_reason;
                        }
                        if (asst->stop_reason == AGENTC_STOP_ERROR) goto attempt_done;
                        asst->usage.input = st.usage_input;
                        asst->usage.output = st.usage_output;
                        asst->usage.cache_read = st.usage_cache_read;
                        asst->usage.cache_write = st.usage_cache_write;
                        asst->usage.reasoning = st.usage_reasoning;
                        asst->usage.cost_micro =
                            agentc_model_cost(agentc_model_find(a->prov->name, a->model),
                                              &asst->usage);
                        a->last_error[0] = 0;
                        turn_ok = true;
                        goto attempt_done;
                    }

                    {
                        int code = trc;
                        if (code == 0) code = res.last_status ? res.last_status : -5;
                        asst->stop_reason = AGENTC_STOP_ERROR;
                        if (st.error[0]) {
                            set_error(a, st.error);
                        } else if (code > 0) {
                            /* A non-2xx body is where the provider says what it objected to;
                             * the transport keeps a bounded, redacted excerpt of it. */
                            const char *excerpt = agentc_transport_http_last_body_excerpt();
                            char url[1200];
                            provider_url(a, url, sizeof url);
                            if (excerpt && excerpt[0])
                                set_errorf(a, "http error %d from %s (%s): %s", code,
                                           a->prov->name, url, excerpt);
                            else
                                set_errorf(a, "http error %d from %s (%s)", code, a->prov->name,
                                           url);
                        } else {
                            set_errorf(a, "transport error %d", code);
                        }
                        if (agentc_retry_retryable(code) && attempt + 1 < attempts) {
                            backoff_sleep(a,
                                          agentc_retry_backoff_ms(attempt, res.retry_after_ms));
                            if (!a->cancel) {
                                if (attempt_streamed(asst)) emit(a, AGENTC_EV_MSG_RESET, NULL);
                                retry = true;
                            }
                        }
                    }

                attempt_done:
                    agentc_stream_state_close(&st);
                    if (retry) continue;
                    break;
                }

                asst = &a->tr.msgs[asst_idx];
                if (a->cancel && asst->stop_reason != AGENTC_STOP_ERROR) {
                    asst->stop_reason = AGENTC_STOP_ABORTED;
                    set_error(a, "aborted");
                }
                /* One terminal decision for the turn: an abort or an error must
                 * stay terminal through the message_end hook, so a late override
                 * cannot turn it back into a serializable success. */
                bool terminal = !turn_ok || a->cancel ||
                                asst->stop_reason == AGENTC_STOP_ERROR ||
                                asst->stop_reason == AGENTC_STOP_ABORTED;
                if (!turn_ok) {
                    if (asst->stop_reason != AGENTC_STOP_ERROR &&
                        asst->stop_reason != AGENTC_STOP_ABORTED) {
                        asst->stop_reason = AGENTC_STOP_ERROR;
                        if (!a->last_error[0]) set_error(a, "request failed");
                    }
                    agentc_free(asst->error);
                    asst->error = agentc_strdup(a->last_error);
                    /* A failed/aborted assistant is neither serialized nor persisted,
                     * so its tool calls must stay unanswered: synthesizing results here
                     * would persist a `tool` result with no matching `tool_call`. */
                    asst = &a->tr.msgs[asst_idx];
                    message_end_hook(asst, terminal);
                    emit(a, AGENTC_EV_MSG_END, asst);
                    if (a->observer) a->observer(a->observer_ud, asst);
                    /* hard exit: entries apply, `continue` does not */
                    (void)turn_end_hook(a, asst->stop_reason, true);
                    emit(a, AGENTC_EV_TURN_END, NULL);
                    emit(a, AGENTC_EV_ERROR, a->last_error);
                    rc = -1;
                    terminal_stop = asst->stop_reason;
                    break;
                }

                message_end_hook(asst, terminal);
                emit(a, AGENTC_EV_MSG_END, asst);
                if (a->observer) a->observer(a->observer_ud, asst);

                size_t ntc = agentc_msg_count_tool_calls(asst);
                if (ntc == 0) {
                    bool hard = a->cancel || asst->stop_reason == AGENTC_STOP_ERROR ||
                                asst->stop_reason == AGENTC_STOP_ABORTED;
                    bool cont = turn_end_hook(a, asst->stop_reason, hard);
                    emit(a, AGENTC_EV_TURN_END, NULL);
                    if (a->cancel) {
                        set_error(a, "aborted");
                        emit(a, AGENTC_EV_ERROR, a->last_error);
                        rc = -1;
                        terminal_stop = AGENTC_STOP_ABORTED;
                        break;
                    }
                    if (cont && continuation_take(&continuations)) continue;
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
                        const char *why =
                            asst->stop_reason == AGENTC_STOP_ABORTED
                                ? "error: aborted before execution"
                                : "error: response ended before this call could run";
                        close_tool_calls(a, asst_idx, why);
                        /* close_tool_calls() pushes tool results, which can
                         * realloc a->tr.msgs and invalidate `asst`. */
                        asst = &a->tr.msgs[asst_idx];
                    }
                    bool hard = a->cancel || asst->stop_reason == AGENTC_STOP_ERROR ||
                                asst->stop_reason == AGENTC_STOP_ABORTED;
                    bool cont = turn_end_hook(a, asst->stop_reason, hard);
                    emit(a, AGENTC_EV_TURN_END, NULL);
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
                    if (cont && continuation_take(&continuations)) continue;
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
                    if (batch > AGENTC_TOOL_JOBS_MAX) batch = AGENTC_TOOL_JOBS_MAX;
                    asst = &a->tr.msgs[asst_idx];
                    char *args_owned[AGENTC_TOOL_JOBS_MAX];
                    for (size_t k = 0; k < batch; k++) {
                        AgcBlock *tc = agentc_msg_nth_tool_call(asst, off + k);
                        const char *args =
                            (tc && tc->tool_args && tc->tool_args[0]) ? tc->tool_args : "{}";
                        args_owned[k] = NULL;
                        job_tools[k] = tc ? find_tool(a, tc->tool_name) : NULL;
                        job_calls[k].call_id = tc ? tc->tool_id : NULL;
                        job_calls[k].name = tc ? tc->tool_name : NULL;
                        job_calls[k].args_json = args;
                        job_calls[k].cancel = &a->cancel;
                        pre_errors[k] = NULL;
                        if (tc && job_tools[k] != NULL && a->veto) {
                            AgcToolVetoDecision dec;
                            agentc_memset(&dec, 0, sizeof dec);
                            a->veto(a->veto_ud, tc->tool_id, tc->tool_name, args, &dec);
                            if (dec.block) {
                                if (dec.terminate) n_terminate++;
                                agentc_snprintf(pe_buf[k], sizeof pe_buf[k],
                                                "error: tool blocked%s%s",
                                                dec.reason ? ": " : "",
                                                dec.reason ? dec.reason : "");
                                pre_errors[k] = pe_buf[k];
                                agentc_free(dec.args_json);
                            } else if (dec.args_json) {
                                args_owned[k] = dec.args_json;
                                job_calls[k].args_json = dec.args_json;
                            }
                            agentc_free(dec.reason);
                        }
                    }
                    jctx.base = off;
                    (void)agentc_tool_jobs_run(job_tools, pre_errors, job_calls, batch,
                                               on_tool_job, &jctx);
                    /* The veto's replacement args are core-owned and the driver hands
                     * them to the tools; they must outlive the whole batch. The abort
                     * path still returns here, so this free is unconditional. */
                    for (size_t k = 0; k < batch; k++) agentc_free(args_owned[k]);
                }

                /* Answers go into the transcript in source order; the driver has
                 * already emitted the start/end events (completion order) and freed
                 * its own buffers. The tool_result override runs before both the
                 * append and the observer so its replacement is not dropped. */
                for (size_t t = 0; t < ntc; t++) {
                    const char *res =
                        slots[t].out.p != NULL ? (const char *)slots[t].out.p : "";
                    asst = &a->tr.msgs[asst_idx];
                    AgcBlock *tc = agentc_msg_nth_tool_call(asst, t);
                    if (!tc) break;
                    bool is_error = slots[t].is_error;
                    char *hooked =
                        tool_result_hook(tc->tool_id, tc->tool_name, res, &is_error);
                    const char *eff_res = hooked ? hooked : res;
                    AgcMsg *tm = agentc_transcript_push(&a->tr, AGENTC_ROLE_TOOL);
                    agentc_msg_add_tool_result(tm, tc->tool_id, tc->tool_name, eff_res);
                    /* the session serializer reads error to write is_error, so it must
                     * be set before the observer persists the message */
                    if (is_error) tm->error = agentc_strdup("error");
                    if (a->observer) a->observer(a->observer_ud, tm);
                    agentc_free(hooked);
                }
                for (size_t t = 0; t < ntc; t++) agentc_buf_free(&slots[t].out);
                agentc_free(slots);
                /* The tool batch has fully returned: it is now safe to swap the tool
                 * table (no live AgcJob.tool pointers) for the next provider request. */
                recompose_now(a);
                asst = &a->tr.msgs[asst_idx];
                bool cont = turn_end_hook(a, asst->stop_reason, a->cancel);
                emit(a, AGENTC_EV_TURN_END, NULL);

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
                /* next turn */
            }
        }

        /* ---- one finish tail for every terminal exit: recompose, the typed
         * agent_end (stop_reason enriched), agent_before_settle, and, once and
         * only once per submit, agent_settled. */
        recompose_now(a);
        emit(a, AGENTC_EV_AGENT_END, &terminal_stop);
        const char *outcome = (rc == 0 && !a->cancel) ? "completed"
                              : a->cancel            ? "aborted"
                                                     : "error";
        if (settle_hook(outcome) && rc == 0 && !a->cancel &&
            continuation_take(&continuations)) {
            terminal_stop = AGENTC_STOP_PENDING;
            continue;
        }
        if (want_observe("agent_settled")) {
            AgcExtResult sr = agentc_ext_emit("agent_settled", "{}");
            agentc_free(sr.result_json);
        }
        break;
    }
    return rc;
}
