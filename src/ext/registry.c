/* registry.c — the extension registry, host vtable and hook bus.
 *
 * One model: built-in tools, MCP and statically-linked extensions all register
 * through the AgcExtHost surface (include/agentc_ext.h). This file owns
 *   - the extension descriptor table (register / load_all / unload),
 *   - the tool/command/section registries and their owner-tagged rollback,
 *   - the hook bus (capability validation, priority, policies, merge algebra,
 *     fail-open/closed, budgets, the wants fast path),
 *   - the deferred queue and the single pending extension HTTP request,
 *   - the process context and UI/model sinks.
 *
 * Default extensions (builtin-tools, builtin-context, mcp) are compiled into
 * the binary. Their only privilege is that they may include
 * src/ext/registry_int.h and call the internal bus directly; they register
 * through the same host as a statically-linked extension.
 */
#include "agentc.h"
#include "wire.h"
#include "plat.h"
#include "status.h"
#include "ext.h"
#include "ext/registry_int.h"
#include "ext/dynlib_int.h"
#include "core/events.h"
#include "core/tools/jobs.h"
#include "prov/provider.h"

/* Internal transcript helpers (src/core/messages.c); the builtin adapters
 * declare them the same way. Declared here for the provider stream sink. */
AgcBlock *agentc_msg_block_new(AgcMsg *m, int type);
void agentc_msg_block_append(AgcBlock *b, const char *p, size_t n);
size_t agentc_msg_count_tool_calls(const AgcMsg *m);

#define AGENTC_EXT_MAX_DEFER     256
#define AGENTC_EXT_MAX_HTTP      8
#define AGENTC_EXT_HTTP_TIMEOUT  10000
#define AGENTC_EXT_MAX_HOOKS     128
#define AGENTC_EXT_BUDGET_NS     50000000LL
#define AGENTC_EXT_MAX_OVERRUNS  3
#define AGENTC_EXT_MAX_EMIT_DEPTH 32
#define AGENTC_EXT_APPEND_ENTRY_MAX 8192
#define AGENTC_EXT_LOAD_ROUNDS     8   /* init()-registered extensions per load_all */
#define AGENTC_EXT_SHUTDOWN_ROUNDS 8   /* shutdown()-appended records per shutdown */

/* ---------------------------------------------------------------- helpers */

static void jsonw_out(AgcJsonW *w, AgcBuf *b) {
    agentc_jsonw_init(w, b);
    agentc_jsonw_set_ascii(w, true);
}

static char *ext_dup(const char *s) { return s ? agentc_strdup(s) : NULL; }

static bool ext_valid_name(const char *name) {
    if (!name || !name[0]) return false;
    size_t len = 0;
    for (; name[len]; len++) {
        char c = name[len];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == ':' || c == '.' || c == '-';
        if (!ok || len >= 64) return false;
    }
    return true;
}

static bool ext_valid_charset(const char *s) {
    if (!s || !s[0]) return false;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
                  c == ':';
        if (!ok) return false;
        if (i >= 64) return false;
    }
    return true;
}

/* ------------------------------------------------------------- JSON merge */

static void json_emit_value(AgcJsonW *w, const AgcJson *v) {
    switch (agentc_json_type(v)) {
    case AGENTC_JSON_NULL:  agentc_jsonw_null(w); break;
    case AGENTC_JSON_TRUE:  agentc_jsonw_bool(w, true); break;
    case AGENTC_JSON_FALSE: agentc_jsonw_bool(w, false); break;
    case AGENTC_JSON_NUM: {
        size_t n = 0;
        const char *p = agentc_json_num(v, &n);
        agentc_jsonw_raw(w, p ? p : "0", p ? n : 1);
        break;
    }
    case AGENTC_JSON_STR: {
        size_t n = 0;
        const char *p = agentc_json_str(v, &n);
        agentc_jsonw_str(w, p ? p : "", n);
        break;
    }
    case AGENTC_JSON_ARR: {
        agentc_jsonw_arr(w);
        size_t n = agentc_json_len(v);
        for (size_t i = 0; i < n; i++) json_emit_value(w, agentc_json_at(v, i));
        agentc_jsonw_end(w);
        break;
    }
    case AGENTC_JSON_OBJ: {
        agentc_jsonw_obj(w);
        for (size_t i = 0;; i++) {
            const AgcJson *k = agentc_json_key_at(v, i);
            if (!k) break;
            size_t kn = 0;
            const char *ks = agentc_json_str(k, &kn);
            agentc_jsonw_key(w, ks ? ks : "");
            json_emit_value(w, agentc_json_val_at(v, i));
        }
        agentc_jsonw_end(w);
        break;
    }
    default: agentc_jsonw_null(w); break;
    }
}

/* fields merge: keys in `over` replace the matching key in `base`; a key whose
 * value is JSON null deletes the base key when keep_nulls is false, and stays
 * as a delete marker when keep_nulls is true (the bus accumulator keeps the
 * markers while the payload handed to the next handler is already applied).
 * Absent keys are unchanged. */
static char *json_fields_apply(const char *base, const char *over, bool keep_nulls) {
    if (!base) base = "{}";
    if (!over) return agentc_strdup(base);
    AgcJsonArena *a1 = agentc_json_arena_new(0);
    AgcJsonArena *a2 = agentc_json_arena_new(0);
    AgcJson *bo = agentc_json_parse_in(a1, base, agentc_strlen(base));
    AgcJson *oo = agentc_json_parse_in(a2, over, agentc_strlen(over));
    if (agentc_json_type(oo) != AGENTC_JSON_OBJ) {
        /* ext.h promises a JSON object; a non-object patch (including a parse
         * failure) leaves the base untouched instead of emitting the patch. */
        bool base_obj = agentc_json_type(bo) == AGENTC_JSON_OBJ;
        char *keep = agentc_strdup(base_obj ? base : "{}");
        agentc_json_arena_free(a1);
        agentc_json_arena_free(a2);
        return keep;
    }
    AgcBuf out = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &out);
    agentc_jsonw_obj(&w);
    if (agentc_json_type(bo) == AGENTC_JSON_OBJ) {
        for (size_t i = 0;; i++) {
            const AgcJson *k = agentc_json_key_at(bo, i);
            if (!k) break;
            size_t kn = 0;
            const char *ks = agentc_json_str(k, &kn);
            if (!ks) continue;
            if (agentc_json_get(oo, ks)) continue;   /* over wins/deletes */
            agentc_jsonw_key(&w, ks);
            json_emit_value(&w, agentc_json_val_at(bo, i));
        }
    }
    for (size_t i = 0;; i++) {
        const AgcJson *k = agentc_json_key_at(oo, i);
        if (!k) break;
        const AgcJson *v = agentc_json_val_at(oo, i);
        if (!keep_nulls && agentc_json_type(v) == AGENTC_JSON_NULL) continue;
        size_t kn = 0;
        const char *ks = agentc_json_str(k, &kn);
        agentc_jsonw_key(&w, ks ? ks : "");
        json_emit_value(&w, v);
    }
    agentc_jsonw_end(&w);
    agentc_json_arena_free(a1);
    agentc_json_arena_free(a2);
    if (!out.p) {
        out.p = agentc_alloc(1);
        out.p[0] = 0;
        out.cap = 1;
    }
    return (char *)out.p;
}

/* Public consumer of a `fields`-merge hook result: the host
 * applies the same merge the bus applies internally. */
char *agentc_ext_merge_fields(const char *base_json, const char *patch_json) {
    return json_fields_apply(base_json, patch_json, false);
}

/* ------------------------------------------------------------ hook points */

enum { POLICY_FIRST, POLICY_CHAIN, POLICY_CHAIN_VETO };
enum { MERGE_REPLACE, MERGE_FIELDS };
enum { FAIL_OPEN, FAIL_CLOSED };

typedef struct {
    const char *point;
    uint32_t caps;
    int policy;
    int merge;
    int fail;
} PointInfo;

static const PointInfo g_points[] = {
    { "project_trust",            AGENTC_HOOK_OVERRIDE, POLICY_FIRST,      MERGE_REPLACE, FAIL_CLOSED },
    { "session_start",            AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "session_shutdown",         AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "resources_discover",       AGENTC_HOOK_OVERRIDE, POLICY_CHAIN,      MERGE_FIELDS,  FAIL_CLOSED },
    { "input",                    AGENTC_HOOK_OVERRIDE, POLICY_CHAIN,      MERGE_FIELDS,  FAIL_CLOSED },
    { "before_agent_start",       AGENTC_HOOK_OVERRIDE, POLICY_CHAIN,      MERGE_FIELDS,  FAIL_CLOSED },
    { "agent_start",              AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "agent_end",                AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "agent_before_settle",      AGENTC_HOOK_OVERRIDE, POLICY_CHAIN,      MERGE_FIELDS,  FAIL_CLOSED },
    { "agent_settled",            AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "turn_start",               AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "turn_end",                 AGENTC_HOOK_OVERRIDE, POLICY_CHAIN,      MERGE_FIELDS,  FAIL_CLOSED },
    { "message_start",            AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "message_update",           AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "message_end",              AGENTC_HOOK_OVERRIDE, POLICY_CHAIN,      MERGE_FIELDS,  FAIL_CLOSED },
    { "tool_call",                AGENTC_HOOK_OVERRIDE, POLICY_CHAIN_VETO, MERGE_FIELDS,  FAIL_CLOSED },
    { "tool_result",              AGENTC_HOOK_OVERRIDE, POLICY_CHAIN,      MERGE_FIELDS,  FAIL_CLOSED },
    { "tool_execution_start",     AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "tool_execution_end",       AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "before_provider_headers",  AGENTC_HOOK_OVERRIDE, POLICY_CHAIN,      MERGE_FIELDS,  FAIL_CLOSED },
    { "before_provider_request",  AGENTC_HOOK_OVERRIDE, POLICY_CHAIN,      MERGE_FIELDS,  FAIL_CLOSED },
    { "after_provider_response",  AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "provider_stream_event",    AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "session_before_compact",   AGENTC_HOOK_OVERRIDE, POLICY_FIRST,      MERGE_REPLACE, FAIL_CLOSED },
    { "session_before_switch",    AGENTC_HOOK_OVERRIDE, POLICY_FIRST,      MERGE_REPLACE, FAIL_CLOSED },
    { "session_compact",          AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "session_compact_failed",   AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "mcp_servers_change",       AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "model_select",             AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
    { "thinking_level_select",    AGENTC_HOOK_OBSERVE,  POLICY_CHAIN,      MERGE_REPLACE, FAIL_OPEN },
};

static const PointInfo *point_info(const char *point) {
    for (size_t i = 0; i < sizeof g_points / sizeof g_points[0]; i++)
        if (agentc_streq(g_points[i].point, point)) return &g_points[i];
    return NULL;
}

/* -------------------------------------------------------- descriptor table */

typedef struct {
    int used;
    int state;                 /* 0 pending, 1 loaded, 2 failed, 3 skipped/disabled */
    bool disabled;             /* excluded by extensions.disabled */
    bool known;                /* false only for an unmatched disabled name */
    char *name;
    char *version;
    int32_t order;
    int (*init)(const AgcExtHost *);
    void (*shutdown)(void);
    u64 owner;
    bool dynamic;              /* loaded from <config>/extensions/ */
} ExtRec;

static AgcVec g_exts;          /* ExtRec */
static u64 g_ext_seq;
static u64 g_owner;            /* extension currently running (0 = core) */
static bool g_dirty;           /* a contribution changed; recompose pending */
static int g_loading;          /* load_all depth: >0 while any init() is on the stack */

/* g_owner is written around every extension callback and read by host_defer on
 * worker threads, so every access goes through relaxed atomics. Owner ids are
 * monotonic and never reused; owner_name() is therefore safe, because a stale
 * id simply stops matching a live record. */
static void owner_set(u64 owner) {
    __atomic_store_n(&g_owner, owner, __ATOMIC_RELAXED);
}
static u64 owner_get(void) {
    return __atomic_load_n(&g_owner, __ATOMIC_RELAXED);
}

/* Forward declarations for the dynamic-loader lifecycle: the close
 * helpers live with the DynRec table after the tool storage, but the async
 * job unlink (above it) must notify them. owner_remove is called by adopt
 * (which precedes its definition) and by load_all. */
static void dyn_notify_job_unlinked(u64 owner);
static void owner_remove(u64 owner);

static ExtRec *ext_find(const char *name) {
    for (size_t i = 0; i < g_exts.len; i++) {
        ExtRec *e = &((ExtRec *)g_exts.p)[i];
        if (e->used && agentc_streq(e->name, name)) return e;
    }
    return NULL;
}

/* Safe owner -> extension-name lookup. Never dereferences a freed record: an
 * unloaded owner's ExtRec is cleared, so the id stops matching and NULL is
 * returned. Used for diagnostics and the host->log prefix. */
static const char *owner_name(u64 owner) {
    if (!owner) return NULL;
    for (size_t i = 0; i < g_exts.len; i++) {
        ExtRec *e = &((ExtRec *)g_exts.p)[i];
        if (e->used && e->owner == owner) return e->name;
    }
    return NULL;
}

/* Copy a contributed ABI struct without ever reading past the caller's
 * declared struct_size (D1). The destination is zeroed first so an older,
 * shorter contribution leaves the appended fields at their defined zero value
 * instead of inheriting whatever the host's full struct happened to hold. */
static void abi_copy(void *dst, const void *src, size_t dst_size, uint32_t src_size) {
    size_t n = src_size < dst_size ? src_size : dst_size;
    agentc_memset(dst, 0, dst_size);
    agentc_memcpy(dst, src, n);
}

/* Shared descriptor validation for the direct and adopted paths: one ABI and
 * enough of the struct to reach init(). False means refused, always with a
 * clear log. */
static bool ext_desc_valid(const AgcExt *ext, const char *name) {
    const char *who = name && name[0] ? name : "?";
    if (!ext || ext->abi_version != AGENTC_EXT_ABI) {
        agentc_logf(3, "ext: %s ABI mismatch", who);
        return false;
    }
    if (ext->struct_size <
        (uint32_t)(offsetof(AgcExt, init) + sizeof ext->init)) {
        agentc_logf(3, "ext: %s struct too small", who);
        return false;
    }
    uint32_t required = 0;
    if (AGENTC_EXT_FIELD_OK(ext, AgcExt, required_host_size))
        required = ext->required_host_size;
    if (required > sizeof(AgcExtHost)) {
        /* No sizes in the message: the golden suite runs on several pointer
         * widths and must be byte-identical across them. */
        agentc_logf(3, "ext: %s requires a newer host ABI", who);
        return false;
    }
    return true;
}

/* Shared insertion used by register, adopt and the dynamic loader. The owner
 * id is allocated by the caller so an entry phase can be owner-tagged before
 * its descriptor exists; `dynamic` marks a runtime-loaded library. A name
 * that apply_config recorded as unknown-disabled is a placeholder: a real
 * registration of that name replaces it (the disable no longer applies,
 * matching the pre-placeholder behaviour). False means refused, always with a
 * clear log. */
static bool ext_register_impl(const AgcExt *ext, const char *name, u64 owner,
                              bool dynamic) {
    if (!ext || !name || !name[0]) {
        agentc_logf(3, "ext: register: missing name");
        return false;
    }
    if (!ext_valid_name(name)) {
        agentc_logf(3, "ext: register: invalid extension name %s", name);
        return false;
    }
    if (!ext_desc_valid(ext, name)) return false;
    ExtRec *existing = ext_find(name);
    if (existing && !(existing->disabled && !existing->known)) {
        agentc_logf(1, "ext: duplicate extension %s ignored", name);
        return false;
    }
    /* Normalize fields past the caller's struct_size the same way adopt does. */
    int32_t order = 0;
    if (ext->struct_size >=
        (uint32_t)(offsetof(AgcExt, order) + sizeof ext->order))
        order = ext->order;
    void (*shutdown)(void) = NULL;
    if (ext->struct_size >=
        (uint32_t)(offsetof(AgcExt, shutdown) + sizeof ext->shutdown))
        shutdown = ext->shutdown;
    /* D: reuse a freed (unused) descriptor slot before growing the vector, so
     * register/unload churn cannot grow g_exts without bound. ext_find,
     * describe and shutdown already skip unused records. */
    ExtRec *e = existing;
    if (!e) {
        for (size_t i = 0; i < g_exts.len; i++) {
            ExtRec *cand = &((ExtRec *)g_exts.p)[i];
            if (!cand->used) { e = cand; break; }
        }
    }
    if (!e) e = agentc_vec_push(&g_exts, sizeof *e);
    if (existing) agentc_free(existing->name);   /* unknown-disabled placeholder */
    agentc_memset(e, 0, sizeof *e);
    e->used = 1;
    e->state = 0;
    e->known = true;
    e->name = agentc_strdup(name);
    e->version = ext_dup(ext->version);
    e->order = order;
    e->init = ext->init;
    e->shutdown = shutdown;
    e->owner = owner;
    e->dynamic = dynamic;
    return true;
}

void agentc_ext_register(const AgcExt *ext) {
    if (!ext) {
        agentc_logf(3, "ext: register: missing name");
        return;
    }
    (void)ext_register_impl(ext, ext->name, ++g_ext_seq, false);
}

int agentc_ext_add_tool_internal(const AgcTool *tool);

/* ------------------------------------------------------------ tool storage */

/* One live asynchronous extension job, owned by the driver through job->priv
 * (the driver frees it with agentc_free). It holds a copy of the published
 * call so the same view is passed to start/step/stop, the 32-byte signal token
 * whose validity spans start..stop, the extension's own state pointer (never
 * freed here), and the LIFO link into the owning ToolRec's live-job list. */
typedef struct ExtAsync ExtAsync;

typedef struct {
    AgcTool tool;              /* exposed view; tool.ud == this */
    char *name, *label, *desc, *params;
    int is_ext;
    AgcExtTool ext;            /* extension tools only */
    u64 owner;
    /* Retire discipline: a record with live async jobs is unlinked from
     * g_tools but stays allocated (retired) until agentc_ext_shutdown, because
     * a live job still points at it through tool.ud. `free_pending` is set by
     * shutdown for a retired record that still has live jobs; the last job to
     * unlink then releases it. */
    bool retired;
    bool free_pending;
    /* A: an AgcTool copy left the module through agentc_ext_tools(); the
     * agent may still hold it (and call tool->run) after the owning extension
     * is unloaded, so the record must be retired instead of freed. */
    bool handed_out;
    /* Internal (builtin/MCP) synchronous tools are exposed through
     * internal_tool_run instead of the caller's run, so a held copy resolves to
     * a clean error once the record is retired (the owning module may then free
     * the ud). `orig_run`/`orig_ud` are the caller's originals. */
    int (*orig_run)(const AgcTool *, const AgcToolCall *, AgcBuf *, bool *);
    void *orig_ud;
    ExtAsync *jobs;            /* LIFO list of live async jobs */
} ToolRec;

struct ExtAsync {
    ToolRec *rec;              /* may be NULL after a pending free */
    AgcExtToolCall call;       /* strings borrowed for the batch */
    const volatile bool *cancel;
    char token[32];
    void *state;               /* extension-owned */
    /* The ExtAsync is linked into rec->jobs before start() runs, so a
     * teardown reachable from inside start() (emit -> hook -> unload/shutdown,
     * or the internal API) can never free the record under the trampoline.
     * `started` gates stop(): a job whose start() has not yet returned is
     * in-flight but not startable/stopable, so teardown must not call stop. */
    bool started;              /* start() returned >= 0; stop may run */
    bool stopped;              /* stop ran exactly once */
    bool on_list;
    ExtAsync *next, *prev;
};

/* g_tools owns pointers, not records: an AgcTool handed out by
 * agentc_ext_tools copies `ud == ToolRec*`, so the record must not move when a
 * later add grows (reallocs) the vector. */
static AgcVec g_tools;

/* Records unlinked while an async job still referenced them. Freed by
 * agentc_ext_shutdown, or by the last job's unlink when shutdown already ran. */
static AgcVec g_retired;

static ToolRec *tool_at(size_t i) { return ((ToolRec **)g_tools.p)[i]; }

/* Reuse a freed pointer slot before growing the vector (D3). tool_remove_at
 * always NULLs the slot, so a NULL pointer is the free marker; reusing the
 * vector never moves an already handed-out ToolRec. */
static ToolRec **tool_slot(void) {
    for (size_t i = 0; i < g_tools.len; i++)
        if (((ToolRec **)g_tools.p)[i] == NULL) return &((ToolRec **)g_tools.p)[i];
    return agentc_vec_push(&g_tools, sizeof(ToolRec *));
}

static void tool_release(ToolRec *t) {
    if (!t) return;
    agentc_free(t->name);
    agentc_free(t->label);
    agentc_free(t->desc);
    agentc_free(t->params);
    agentc_free((void *)t->ext.prompt_snippet);
    agentc_free((void *)t->ext.prompt_guidelines);
    agentc_free(t);
}

static void ext_tool_retire(ToolRec *t);

static void tool_remove_at(size_t i) {
    ToolRec *t = tool_at(i);
    if (t && (t->jobs != NULL || t->handed_out)) {
        /* A live job holds tool.ud == t, and a handed-out AgcTool copy holds
         * ud == t too (the agent may execute it after this unload). Unlink
         * now, stop(UNLOAD) each job, and keep the record until shutdown (or
         * the last job's unlink); ext_tool_run refuses a retired record. */
        ext_tool_retire(t);
    } else {
        tool_release(t);
    }
    ((ToolRec **)g_tools.p)[i] = NULL;
}

static bool tool_name_taken(const char *name) {
    for (size_t i = 0; i < g_tools.len; i++) {
        ToolRec *t = tool_at(i);
        if (t && t->tool.name && agentc_streq(t->tool.name, name)) return true;
    }
    return false;
}

static char *g_signal;
static const volatile bool *g_signal_cancel;
static u64 g_signal_seq;

static int ext_tool_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                        bool *is_error) {
    ToolRec *t = (self && self->run == ext_tool_run) ? (ToolRec *)self->ud : NULL;
    if (!t || t->retired) {
        /* The owning extension was unloaded while this AgcTool copy was still
         * held: the record is retired (not freed), so the run must resolve to
         * the same clean error the missing-record path returns (A). */
        if (is_error) *is_error = true;
        agentc_buf_cstr(out, "error: extension tool is no longer registered");
        return 0;
    }
    char token[24];
    agentc_snprintf(token, sizeof token, "extcall-%llu",
                    (unsigned long long)++g_signal_seq);
    g_signal = token;
    g_signal_cancel = call ? call->cancel : NULL;
    AgcExtToolCall ec;
    agentc_memset(&ec, 0, sizeof ec);
    ec.struct_size = sizeof ec;
    ec.call_id = call ? call->call_id : NULL;
    ec.name = call ? call->name : NULL;
    ec.args_json = call ? call->args_json : NULL;
    ec.signal_token = token;
    bool err = false;
    u64 saved_owner = owner_get();
    owner_set(t->owner);
    int rc = t->ext.run ? t->ext.run(agentc_ext_host(), &t->ext, &ec, out, &err) : -38;
    owner_set(saved_owner);
    g_signal = NULL;
    g_signal_cancel = NULL;
    if (is_error) *is_error = err || rc < 0;
    return rc;
}

/* Internal (builtin/MCP) synchronous tools are exposed through this wrapper so
 * a copy held by the agent resolves to a clean error once the record is retired
 * (agentc_ext_remove_tool_internal), instead of calling the tool's original
 * run() with a ud the owning module may already have freed. The wrapper passes
 * the original view (run/ud restored) so the tool's own self-checks still
 * hold. */
static int internal_tool_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                             bool *is_error) {
    ToolRec *t = (self && self->run == internal_tool_run) ? (ToolRec *)self->ud : NULL;
    if (!t || t->retired || !t->orig_run) {
        if (is_error) *is_error = true;
        agentc_buf_cstr(out, "error: tool is no longer available");
        return 0;
    }
    AgcTool view = t->tool;
    view.run = t->orig_run;
    view.ud = t->orig_ud;
    return t->orig_run(&view, call, out, is_error);
}

/* ------------------------------------------------- async extension tools */

/* Every extension callback runs with its owner restored (so host->log and
 * owner-tagged contributions resolve) and the signal window set, so
 * host->is_cancelled(call->signal_token) is valid from start through stop. */
static u64 async_enter(ToolRec *t, ExtAsync *r) {
    u64 saved = owner_get();
    owner_set(t->owner);
    g_signal = r->token;
    g_signal_cancel = r->cancel;
    return saved;
}

static void async_leave(u64 saved_owner) {
    g_signal = NULL;
    g_signal_cancel = NULL;
    owner_set(saved_owner);
}

static void ext_async_link(ExtAsync *r) {
    r->prev = NULL;
    r->next = r->rec->jobs;
    if (r->rec->jobs) r->rec->jobs->prev = r;
    r->rec->jobs = r;
    r->on_list = true;
}

/* Unlink and, for a retired record whose shutdown already ran, release it once
 * the last live job is gone. */
static void ext_async_unlink(ExtAsync *r) {
    if (r->on_list) {
        if (r->prev) r->prev->next = r->next;
        else if (r->rec) r->rec->jobs = r->next;
        if (r->next) r->next->prev = r->prev;
        r->next = NULL;
        r->prev = NULL;
        r->on_list = false;
    }
    ToolRec *t = r->rec;
    r->rec = NULL;
    u64 owner = t ? t->owner : 0;
    /* The last live job is gone; a requested dynamic-library close
     * can now run. This happens before tool_release(), because a released
     * record is not cleared from g_retired and dyn_owner_busy() scans it. */
    if (owner) dyn_notify_job_unlinked(owner);
    if (t && t->retired && t->free_pending && t->jobs == NULL) tool_release(t);
}

static void ext_async_stop(ExtAsync *r, int reason) {
    if (!r || r->stopped) return;
    ToolRec *t = r->rec;
    r->stopped = true;   /* exactly once, even if stop re-enters this job */
    if (!t || !t->ext.stop) return;
    u64 saved = async_enter(t, r);
    t->ext.stop(agentc_ext_host(), &t->ext, r->state, reason);
    async_leave(saved);
}

static void ext_async_finish(ExtAsync *r, int reason) {
    ext_async_stop(r, reason);
    ext_async_unlink(r);
}

static void ext_tool_cleanup(const AgcTool *self, AgcJob *job, int reason);

/* D-A6: a record with live async jobs is unlinked but not freed. Each live job
 * that has finished starting gets stop(UNLOAD) now; an in-flight job whose
 * start() is still on the stack is skipped (a failed start is never
 * followed by stop, and its trampoline delivers stop after start returns).
 * The record survives until shutdown (or, if shutdown already ran, until the
 * last job unlinks). */
static void ext_tool_retire(ToolRec *t) {
    if (!t || t->retired) return;
    t->retired = true;
    ToolRec **slot = agentc_vec_push(&g_retired, sizeof *slot);
    *slot = t;
    for (ExtAsync *r = t->jobs; r != NULL; ) {
        ExtAsync *next = r->next;
        if (r->started) ext_async_stop(r, AGENTC_EXT_TOOL_UNLOAD);
        r = next;
    }
}

/* Cleanup for a start that failed after a teardown unlinked its record: the
 * job still owns the ExtAsync, and releasing it here (after the driver built
 * its fatal-error text from tool->name) also releases a retired record whose
 * shutdown already ran. A failed start never delivers stop. */
static void ext_tool_start_cleanup(const AgcTool *self, AgcJob *job, int reason) {
    (void)self;
    (void)reason;
    ExtAsync *r = job ? (ExtAsync *)job->priv : NULL;
    if (r) ext_async_unlink(r);
}

/* The internal AgcTool.start for an extension tool: publishes the call, runs
 * the extension start under the signal window, and maps its return code. */
static int ext_tool_start(const AgcTool *self, const AgcToolCall *call, AgcJob *job) {
    ToolRec *t = (self && self->start == ext_tool_start) ? (ToolRec *)self->ud : NULL;
    if (!t) {
        agentc_buf_cstr(&job->out, "error: extension tool is no longer registered");
        job->is_error = true;
        return 1;
    }
    if (t->retired) {
        agentc_buf_cstr(&job->out, "error: extension tool unloaded");
        job->is_error = true;
        return 1;
    }
    ExtAsync *r = agentc_alloc(sizeof *r);
    r->rec = t;
    r->call.struct_size = sizeof r->call;
    r->call.call_id = call ? call->call_id : NULL;
    r->call.name = call ? call->name : NULL;
    r->call.args_json = call ? call->args_json : NULL;
    agentc_snprintf(r->token, sizeof r->token, "extasync-%llu",
                    (unsigned long long)++g_signal_seq);
    r->call.signal_token = r->token;
    r->cancel = call ? call->cancel : NULL;
    job->priv = r;
    /* Link before start() so a teardown reachable from inside start sees
     * this job as live and retires the record instead of freeing it. */
    ext_async_link(r);

    AgcBuf stage = { 0 };
    bool err = false;
    void *state = NULL;
    u64 saved = async_enter(t, r);
    int rc = t->ext.start ? t->ext.start(agentc_ext_host(), &t->ext, &r->call,
                                         &stage, &err, &state) : -38;
    async_leave(saved);
    r->state = state;
    if (stage.len)
        (void)agentc_tool_append_output(job, (const char *)stage.p, stage.len, 0);
    agentc_buf_free(&stage);
    if (rc < 0) {
        /* A failed start is never followed by stop. If the record was torn
         * down while start was on the stack, keep the job (and the record it
         * references) until the driver's cleanup, because job_fatal formats
         * the tool name after this trampoline returns. */
        if (t->retired) {
            job->cleanup = ext_tool_start_cleanup;
            return rc;
        }
        ext_async_unlink(r);
        agentc_free(r);
        job->priv = NULL;
        return rc;
    }
    r->started = true;
    if (t->retired) {
        /* The extension tore this record down while start was on the stack:
         * deliver exactly one stop(UNLOAD) and unlink (releasing the record
         * when shutdown already ran). A step must never see this job. */
        job->is_error = true;
        job->cleanup = NULL;
        (void)agentc_tool_append_output(job, "error: extension tool unloaded",
                                        sizeof "error: extension tool unloaded" - 1, 0);
        ext_async_finish(r, AGENTC_EXT_TOOL_UNLOAD);
        return 1;
    }
    if (rc == 1) {
        job->is_error = err;
        ext_async_finish(r, AGENTC_EXT_TOOL_FINISHED);
        return 1;
    }
    job->cleanup = ext_tool_cleanup;
    return 0;
}

/* The internal AgcTool.step for an extension tool. */
static int ext_tool_step(const AgcTool *self, AgcJob *job) {
    ToolRec *t = (self && self->step == ext_tool_step) ? (ToolRec *)self->ud : NULL;
    ExtAsync *r = job ? (ExtAsync *)job->priv : NULL;
    if (!r) {
        agentc_buf_cstr(&job->out, "error: extension tool is no longer registered");
        job->is_error = true;
        return 1;
    }
    if (!t || t->retired || r->rec == NULL) {
        /* The tool unloaded while the job was live: the retire path already
         * ran stop(UNLOAD); synthesize the result with no extension call. */
        job->cleanup = NULL;
        (void)agentc_tool_append_output(job, "error: extension tool unloaded",
                                        sizeof "error: extension tool unloaded" - 1, 0);
        job->is_error = true;
        ext_async_finish(r, AGENTC_EXT_TOOL_UNLOAD);
        return 1;
    }
    AgcBuf stage = { 0 };
    bool err = false;
    u64 saved = async_enter(t, r);
    int rc = t->ext.step ? t->ext.step(agentc_ext_host(), &t->ext, &r->call,
                                       r->state, &stage, &err) : -38;
    async_leave(saved);
    if (stage.len)
        (void)agentc_tool_append_output(job, (const char *)stage.p, stage.len, 0);
    agentc_buf_free(&stage);
    if (rc == 1) {
        job->is_error = err;
        job->cleanup = NULL;
        ext_async_finish(r, AGENTC_EXT_TOOL_FINISHED);
        return 1;
    }
    if (rc < 0) return rc;   /* the driver's cleanup maps it to stop(ERROR) */
    /* D-A9: run queued defer/http work with the extension off the stack. */
    agentc_pump(0);
    return 0;
}

/* The driver's AgcJob.cleanup hook: deliver the matching stop reason. */
static void ext_tool_cleanup(const AgcTool *self, AgcJob *job, int reason) {
    (void)self;
    ExtAsync *r = job ? (ExtAsync *)job->priv : NULL;
    if (!r) return;
    int ext_reason;
    switch (reason) {
    case AGENTC_JOB_STOP_TIMEDOUT:  ext_reason = AGENTC_EXT_TOOL_TIMEOUT; break;
    case AGENTC_JOB_STOP_CANCELLED: ext_reason = AGENTC_EXT_TOOL_CANCELLED; break;
    case AGENTC_JOB_STOP_RELEASE:   ext_reason = AGENTC_EXT_TOOL_UNLOAD; break;
    default:                        ext_reason = AGENTC_EXT_TOOL_ERROR; break;
    }
    /* A step that self-aborted by returning <0 still delivers CANCELLED. */
    if (ext_reason == AGENTC_EXT_TOOL_ERROR && r->cancel && *r->cancel)
        ext_reason = AGENTC_EXT_TOOL_CANCELLED;
    ext_async_finish(r, ext_reason);
}

static void host_add_tool(const AgcExtTool *tool) {
    if (!tool || tool->struct_size < (uint32_t)(offsetof(AgcExtTool, run) + sizeof tool->run)) {
        agentc_logf(3, "ext: add_tool: struct too small");
        return;
    }
    if (!ext_valid_name(tool->name)) {
        agentc_logf(3, "ext: add_tool: invalid tool name");
        return;
    }
    /* D-A1/D-A7: run != NULL is synchronous and wins; otherwise the complete
     * async trio is required. The trio is read only behind FIELD_OK so an
     * older, shorter contribution cannot be misread as async. */
    bool run_set = tool->run != NULL;
    bool start_set = AGENTC_EXT_FIELD_OK(tool, AgcExtTool, start) && tool->start != NULL;
    bool step_set = AGENTC_EXT_FIELD_OK(tool, AgcExtTool, step) && tool->step != NULL;
    bool stop_set = AGENTC_EXT_FIELD_OK(tool, AgcExtTool, stop) && tool->stop != NULL;
    bool async = false;
    if (!run_set) {
        if (!start_set || !step_set || !stop_set) {
            agentc_logf(3, "ext: add_tool: %s has incomplete async trio", tool->name);
            return;
        }
        async = true;
    } else if (start_set || step_set || stop_set) {
        agentc_logf(1, "ext: add_tool: %s declares both run and async; run wins",
                    tool->name);
    }
    if (tool_name_taken(tool->name)) {
        agentc_logf(2, "ext: duplicate tool %s ignored", tool->name);
        return;
    }
    ToolRec **slot = tool_slot();
    ToolRec *t = agentc_alloc(sizeof *t);
    *slot = t;
    t->is_ext = 1;
    t->owner = owner_get();
    t->name = agentc_strdup(tool->name);
    t->label = ext_dup(tool->label);
    t->desc = ext_dup(tool->description);
    t->params = ext_dup(tool->parameters_json);
    abi_copy(&t->ext, tool, sizeof t->ext, tool->struct_size);
    t->ext.name = t->name;
    t->ext.label = t->label;
    t->ext.description = t->desc;
    t->ext.parameters_json = t->params;
    if (tool->prompt_snippet) t->ext.prompt_snippet = agentc_strdup(tool->prompt_snippet);
    if (tool->prompt_guidelines) t->ext.prompt_guidelines = agentc_strdup(tool->prompt_guidelines);
    t->tool.name = t->name;
    t->tool.label = t->label ? t->label : t->name;
    t->tool.desc = t->desc ? t->desc : "";
    t->tool.params_json = t->params ? t->params : "{\"type\":\"object\"}";
    t->tool.flags = tool->flags &
        (AGENTC_TOOL_READONLY | AGENTC_TOOL_DESTRUCTIVE |
         AGENTC_TOOL_SEQUENTIAL | AGENTC_TOOL_HIDDEN);
    t->tool.timeout_ms = tool->timeout_ms;
    t->tool.ud = t;
    if (async) {
        /* OD-3: async timeouts clamp to the ABI maximum; a negative value is
         * normalized to 0 (the driver's default). Sync tools are unchanged. */
        if (t->tool.timeout_ms > AGENTC_EXT_TOOL_TIMEOUT_MAX_MS) {
            agentc_logf(2, "ext: add_tool: %s timeout_ms clamped to %d",
                        tool->name, AGENTC_EXT_TOOL_TIMEOUT_MAX_MS);
            t->tool.timeout_ms = AGENTC_EXT_TOOL_TIMEOUT_MAX_MS;
        } else if (t->tool.timeout_ms < 0) {
            t->tool.timeout_ms = 0;
        }
        t->tool.run = NULL;
        t->tool.start = ext_tool_start;
        t->tool.step = ext_tool_step;
    } else {
        t->tool.run = ext_tool_run;
    }
    g_dirty = true;   /* a late public add reaches the next turn boundary */
}

int agentc_ext_add_tool_internal(const AgcTool *tool) {
    if (!tool || !tool->name || !ext_valid_name(tool->name)) return -22;
    if (!tool->run && !tool->start) return -22;
    if (tool_name_taken(tool->name)) {
        agentc_logf(2, "ext: duplicate internal tool %s ignored", tool->name);
        return -17;
    }
    ToolRec **slot = tool_slot();
    ToolRec *t = agentc_alloc(sizeof *t);
    *slot = t;
    t->is_ext = 0;
    t->owner = owner_get();
    t->name = agentc_strdup(tool->name);
    t->label = ext_dup(tool->label);
    t->desc = ext_dup(tool->desc);
    t->params = ext_dup(tool->params_json);
    t->tool = *tool;
    t->tool.name = t->name;
    t->tool.label = t->label ? t->label : t->name;
    t->tool.desc = t->desc ? t->desc : "";
    t->tool.params_json = t->params ? t->params : "{\"type\":\"object\"}";
    if (tool->run) {
        /* synchronous internal tool: wrap so a held copy is retire-safe */
        t->orig_run = tool->run;
        t->orig_ud = tool->ud;
        t->tool.ud = t;
        t->tool.run = internal_tool_run;
    } else {
        t->tool.ud = tool->ud;   /* async internal tool: start/step are used */
    }
    g_dirty = true;   /* a late add is applied at the next turn boundary */
    return 0;
}

int agentc_ext_remove_tool_internal(const char *name) {
    if (!name) return -2;   /* ENOENT */
    for (size_t i = 0; i < g_tools.len; i++) {
        ToolRec *t = tool_at(i);
        if (!t || !t->tool.name || !agentc_streq(t->tool.name, name)) continue;
        /* Unlink the record. `tool.ud` belongs to the caller and is
         * deliberately not freed: a transcript may still hold the tool. With
         * live async jobs the record is retired instead of freed, so a
         * later step resolves to a clean "unloaded" result. */
        tool_remove_at(i);
        g_dirty = true;
        return 0;
    }
    return -2;   /* ENOENT */
}

/* -------------------------------------------------------- pump callbacks */

typedef struct {
    int (*fn)(void *ud);
    void *ud;
    u64 owner;
} PumpRec;

static AgcVec g_pumps;

/* Reuse a freed pump slot before growing the vector (owner_remove zeroes it,
 * matching the fn == NULL liveness test in the pump loop). */
static PumpRec *pump_slot(void) {
    for (size_t i = 0; i < g_pumps.len; i++) {
        PumpRec *p = &((PumpRec *)g_pumps.p)[i];
        if (!p->fn) return p;
    }
    return agentc_vec_push(&g_pumps, sizeof(PumpRec));
}

void agentc_ext_add_pump(int (*fn)(void *ud), void *ud) {
    if (!fn) return;
    PumpRec *p = pump_slot();
    p->fn = fn;
    p->ud = ud;
    p->owner = owner_get();
}

size_t agentc_ext_tools(AgcTool *out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < g_tools.len; i++) {
        ToolRec *t = tool_at(i);
        if (!t || !t->tool.name) continue;
        if (out && n < max) {
            out[n] = t->tool;
            t->handed_out = true;   /* the caller may hold it across unload (A) */
        }
        n++;
    }
    return out ? (n < max ? n : max) : n;
}

/* --------------------------------------------------------- command storage */

typedef struct {
    AgcExtCommand cmd;
    char *name, *desc;
    void *ud;
    u64 owner;
} CmdRec;

static AgcVec g_cmds;

/* Reuse a freed command slot before growing the vector (D3); a zeroed record
 * (owner_remove) has name == NULL, matching the liveness test used elsewhere. */
static CmdRec *cmd_slot(void) {
    for (size_t i = 0; i < g_cmds.len; i++) {
        CmdRec *c = &((CmdRec *)g_cmds.p)[i];
        if (!c->name) return c;
    }
    return agentc_vec_push(&g_cmds, sizeof(CmdRec));
}

static void host_add_command(const AgcExtCommand *cmd) {
    if (!cmd || cmd->struct_size <
                      (uint32_t)(offsetof(AgcExtCommand, run) + sizeof cmd->run)) {
        agentc_logf(3, "ext: add_command: struct too small");
        return;
    }
    if (!cmd->name || !cmd->name[0]) {
        agentc_logf(3, "ext: add_command: name required");
        return;
    }
    for (size_t i = 0; i < g_cmds.len; i++) {
        CmdRec *c = &((CmdRec *)g_cmds.p)[i];
        if (!c->name) continue;
        if (agentc_streq(c->name, cmd->name)) {
            agentc_logf(2, "ext: duplicate command %s ignored", cmd->name);
            return;
        }
    }
    CmdRec *c = cmd_slot();
    c->name = agentc_strdup(cmd->name);
    c->desc = ext_dup(cmd->description);
    c->ud = cmd->ud;
    abi_copy(&c->cmd, cmd, sizeof c->cmd, cmd->struct_size);
    c->cmd.name = c->name;
    c->cmd.description = c->desc;
    c->owner = owner_get();
}

size_t agentc_ext_commands(AgcExtCommandInfo *out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < g_cmds.len; i++) {
        CmdRec *c = &((CmdRec *)g_cmds.p)[i];
        if (!c->name) continue;
        if (out && n < max) {
            out[n].name = c->name;
            out[n].description = c->desc ? c->desc : "";
        }
        n++;
    }
    return out ? (n < max ? n : max) : n;
}

char *agentc_ext_run_command(const char *name, const char *args) {
    if (!name) return NULL;
    for (size_t i = 0; i < g_cmds.len; i++) {
        CmdRec *c = &((CmdRec *)g_cmds.p)[i];
        if (!c->name || !agentc_streq(c->name, name)) continue;
        if (!c->cmd.run) return NULL;
        AgcBuf out = { 0 };
        u64 saved_owner = owner_get();
        owner_set(c->owner);
        c->cmd.run(agentc_ext_host(), c->ud, args ? args : "", &out);
        owner_set(saved_owner);
        if (!out.p) agentc_buf_cstr(&out, "");
        return (char *)out.p;
    }
    return NULL;
}

/* --------------------------------------------------------- section storage */

typedef struct {
    AgcExtSection sec;
    char *key;
    void *ud;
    u64 owner;
    u64 seq;
} SecRec;

static AgcVec g_sections;
static u64 g_sec_seq;

/* Reuse a freed section slot before growing the vector (D3); a zeroed record
 * (owner_remove) has sec.render == NULL, the liveness test used elsewhere. */
static SecRec *section_slot(void) {
    for (size_t i = 0; i < g_sections.len; i++) {
        SecRec *s = &((SecRec *)g_sections.p)[i];
        if (!s->sec.render) return s;
    }
    return agentc_vec_push(&g_sections, sizeof(SecRec));
}

static void host_add_section(const AgcExtSection *section) {
    if (!section || section->struct_size <
                        (uint32_t)(offsetof(AgcExtSection, render) + sizeof section->render)) {
        agentc_logf(3, "ext: add_section: struct too small");
        return;
    }
    if (!section->render) {
        agentc_logf(3, "ext: add_section: render required");
        return;
    }
    SecRec *s = section_slot();
    s->key = ext_dup(section->key);
    s->ud = section->ud;
    abi_copy(&s->sec, section, sizeof s->sec, section->struct_size);
    s->sec.key = s->key;
    s->owner = owner_get();
    s->seq = ++g_sec_seq;
}

void agentc_ext_render_sections(AgcBuf *out) {
    /* Snapshot the live records by value before calling any render callback:
     * render is extension code and may add/unload sections, which reallocates
     * (or frees) the live vector. Then insertion-sort the snapshot by
     * (priority, seq); the table is tiny and the sort stays dependency-free. */
    size_t live = 0;
    for (size_t i = 0; i < g_sections.len; i++)
        if (((SecRec *)g_sections.p)[i].sec.render) live++;
    SecRec *list = agentc_alloc((live ? live : 1) * sizeof *list);
    size_t n = 0;
    for (size_t i = 0; i < g_sections.len; i++) {
        SecRec *s = &((SecRec *)g_sections.p)[i];
        if (s->sec.render) list[n++] = *s;
    }
    for (size_t i = 1; i < n; i++) {
        SecRec key = list[i];
        size_t j = i;
        while (j > 0) {
            SecRec *a = &list[j - 1];
            bool before = a->sec.priority < key.sec.priority ||
                          (a->sec.priority == key.sec.priority && a->seq < key.seq);
            if (before) break;
            list[j] = list[j - 1];
            j--;
        }
        list[j] = key;
    }
    for (size_t i = 0; i < n; i++) {
        /* An earlier render may have unloaded this section's extension; its
         * ExtRec is cleared, so skip the stale snapshot rather than calling a
         * render whose userdata the unload may already have freed (D2). */
        if (list[i].owner != 0 && owner_name(list[i].owner) == NULL) continue;
        u64 saved_owner = owner_get();
        owner_set(list[i].owner);
        list[i].sec.render(agentc_ext_host(), list[i].ud, out);
        owner_set(saved_owner);
    }
    agentc_free(list);
}

/* --------------------------------------------------------- status tracking */

/* One host trampoline per registered extension status provider:
 * the status module sees only host_status_call, whose userdata is this record,
 * so the extension provider always runs with its owner id active. Without the
 * trampoline an extension provider that unloads its own library mid-snapshot
 * would look like owner-less core code and the dynamic handle would be closed
 * while its code is still on the stack. Records are heap-allocated and tracked
 * by pointer because g_status_regs may realloc and agentc_status_remove()
 * matches on the (trampoline, record) identity. */
typedef struct {
    AgcExtStatusProvider provider;
    void *ud;
    u64 owner;
} StatusReg;

static AgcVec g_status_regs;   /* StatusReg* */

static size_t host_status_call(void *ud, AgcExtStatusSegment *out, size_t max,
                               char *arena, size_t arena_cap) {
    /* Copy the identity first: the provider may remove (and free) its own
     * record by unloading its extension while the call is on the stack. */
    StatusReg *r = ud;
    AgcExtStatusProvider provider = r->provider;
    void *pud = r->ud;
    u64 owner = r->owner;
    u64 saved = owner_get();
    owner_set(owner);
    size_t n = provider(pud, out, max, arena, arena_cap);
    owner_set(saved);
    return n;
}

static void host_add_status(AgcExtStatusProvider provider, void *ud) {
    if (!provider) {
        agentc_logf(3, "ext: add_status: NULL provider");
        return;
    }
    u64 owner = owner_get();
    /* Keep the status module's (provider, userdata) dedup semantics for a
     * repeated registration by the same owner. */
    for (size_t i = 0; i < g_status_regs.len; i++) {
        StatusReg *r = ((StatusReg **)g_status_regs.p)[i];
        if (r && r->provider == provider && r->ud == ud && r->owner == owner)
            return;
    }
    StatusReg *r = agentc_alloc(sizeof *r);
    r->provider = provider;
    r->ud = ud;
    r->owner = owner;
    agentc_status_register(host_status_call, r);
    /* Reuse a freed pointer slot before growing the vector; owner_remove NULLs
     * the slot, and existing records never move (the vector stores pointers). */
    StatusReg **slot = NULL;
    for (size_t i = 0; i < g_status_regs.len; i++) {
        StatusReg **cand = &((StatusReg **)g_status_regs.p)[i];
        if (!*cand) { slot = cand; break; }
    }
    if (!slot) slot = agentc_vec_push(&g_status_regs, sizeof *slot);
    *slot = r;
}

/* set_status: keyed, auto-clearing segments owned by the registry.
 * One live (owner, key) pair contributes one left slot segment; the provider is
 * registered idempotently on every mutation and removed when the last key
 * clears, so the table never caches a registration that agentc_status_reset()
 * could clear behind its back. Entries are owner-tagged and drop with their
 * owner. Two extensions may use the same key without colliding: lookup is by
 * (owner, key), so one owner's clear can never remove the other's text. */
#define AGENTC_EXT_SET_STATUS_KEYS 8

typedef struct {
    bool used;
    char *key;
    char text[AGENTC_STATUS_TEXT_MAX];
    u64 owner;
} SetStatusEntry;

static SetStatusEntry g_set_status[AGENTC_EXT_SET_STATUS_KEYS];

static size_t set_status_provider(void *ud, AgcExtStatusSegment *out, size_t max,
                                  char *arena, size_t arena_cap) {
    (void)ud; (void)arena; (void)arena_cap;
    size_t n = 0;
    for (size_t i = 0; i < AGENTC_EXT_SET_STATUS_KEYS && n < max; i++) {
        SetStatusEntry *e = &g_set_status[i];
        if (!e->used) continue;
        out[n].struct_size = sizeof out[n];
        out[n].slot = AGENTC_PSEG_SLOT_LEFT;
        out[n].priority = 100;                  /* after the built-ins */
        out[n].style = AGENTC_PSEG_STYLE_DIM;
        out[n].text = e->text;
        n++;
    }
    return n;
}

static bool set_status_any(void) {
    for (size_t i = 0; i < AGENTC_EXT_SET_STATUS_KEYS; i++)
        if (g_set_status[i].used) return true;
    return false;
}

static void set_status_sync_provider(void) {
    /* Idempotent, cheap (one small table scan). Deliberately uncached: an
     * external agentc_status_reset() must not desync a cached flag. */
    if (set_status_any()) agentc_status_register(set_status_provider, NULL);
    else agentc_status_remove(set_status_provider, NULL);
}

static SetStatusEntry *set_status_find(const char *key, u64 owner) {
    for (size_t i = 0; i < AGENTC_EXT_SET_STATUS_KEYS; i++)
        if (g_set_status[i].used && g_set_status[i].owner == owner &&
            agentc_streq(g_set_status[i].key, key))
            return &g_set_status[i];
    return NULL;
}

static void set_status_clear(SetStatusEntry *e) {
    agentc_free(e->key);
    agentc_memset(e, 0, sizeof *e);
}

static void set_status_set(const char *key, const char *text) {
    if (!ext_valid_charset(key)) {
        agentc_logf(3, "ext: set_status: invalid key");
        return;
    }
    u64 owner = owner_get();
    SetStatusEntry *e = set_status_find(key, owner);
    if (!text || !text[0]) {           /* empty or NULL text clears the key */
        if (e) {
            set_status_clear(e);
            set_status_sync_provider();
            agentc_status_invalidate();
        }
        return;
    }
    if (!e) {
        for (size_t i = 0; i < AGENTC_EXT_SET_STATUS_KEYS; i++)
            if (!g_set_status[i].used) { e = &g_set_status[i]; break; }
    }
    if (!e) {
        const char *who = owner_name(owner);
        agentc_logf(2, "ext: set_status: key table full for %s key %s",
                    who ? who : "?", key);
        return;
    }
    bool changed = !e->used || !agentc_streq(e->text, text);
    if (!e->used) {
        e->used = true;
        e->key = agentc_strdup(key);
    }
    e->owner = owner;
    agentc_snprintf(e->text, sizeof e->text, "%s", text);
    set_status_sync_provider();
    if (changed) agentc_status_invalidate();
}

static void set_status_owner_remove(u64 owner) {
    bool cleared = false;
    for (size_t i = 0; i < AGENTC_EXT_SET_STATUS_KEYS; i++) {
        if (!g_set_status[i].used || g_set_status[i].owner != owner) continue;
        set_status_clear(&g_set_status[i]);
        cleared = true;
    }
    if (cleared) {
        set_status_sync_provider();
        agentc_status_invalidate();
    }
}

/* ------------------------------------------------------------------ hooks */

typedef struct {
    int used;
    char *point;
    uint32_t caps;
    int priority;
    AgcExtHookFn fn;
    void *ud;
    u64 owner;
    u64 seq;
    bool disabled;
    uint8_t overruns;          /* consecutive budget overruns (3 => disabled) */
} HookRec;

static HookRec g_hooks[AGENTC_EXT_MAX_HOOKS];
static u64 g_hook_seq;
static u32 g_hooks_live;       /* live registrations; zero => wants() fast path */

static uint64_t host_on(const char *point, uint32_t caps, int priority,
                        AgcExtHookFn fn, void *ud) {
    if (!point || !point[0] || !fn) {
        agentc_logf(3, "ext: on: invalid registration");
        return 0;
    }
    const PointInfo *pi = point_info(point);
    if (!pi) {
        /* Only known hook points exist. An unknown name used to become an
         * OBSERVE subscription and silently never fire. */
        agentc_logf(3, "ext: on: unknown hook point %s", point);
        return 0;
    }
    if (caps != pi->caps) {
        agentc_logf(3, "ext: on: %s requires caps 0x%x, got 0x%x", point, pi->caps, caps);
        return 0;
    }
    for (size_t i = 0; i < AGENTC_EXT_MAX_HOOKS; i++) {
        if (g_hooks[i].used) continue;
        g_hooks[i].used = 1;
        g_hooks[i].point = agentc_strdup(point);
        g_hooks[i].caps = caps;
        g_hooks[i].priority = priority;
        g_hooks[i].fn = fn;
        g_hooks[i].ud = ud;
        g_hooks[i].owner = owner_get();
        g_hooks[i].seq = ++g_hook_seq;
        g_hooks_live++;
        return g_hooks[i].seq;
    }
    agentc_logf(3, "ext: too many hook handlers");
    return 0;
}

static void host_off(uint64_t handle) {
    if (!handle) return;
    for (size_t i = 0; i < AGENTC_EXT_MAX_HOOKS; i++) {
        if (g_hooks[i].used && g_hooks[i].seq == handle) {
            agentc_free(g_hooks[i].point);
            agentc_memset(&g_hooks[i], 0, sizeof g_hooks[i]);
            if (g_hooks_live) g_hooks_live--;
            return;
        }
    }
}

bool agentc_ext_wants(const char *point) {
    if (!point || g_hooks_live == 0) return false;
    for (size_t i = 0; i < AGENTC_EXT_MAX_HOOKS; i++) {
        HookRec *h = &g_hooks[i];
        /* A disabled observe handler (overrun) is no longer a subscriber, and
         * a record without a callback is not one either. */
        if (h->used && !h->disabled && h->fn && agentc_streq(h->point, point))
            return true;
    }
    return false;
}

/* Snapshot of the handlers participating in one dispatch. `off()` during the
 * walk clears the live record but cannot affect this walk, and a registration
 * during dispatch stays invisible until the next emit. An overrun disable is
 * persisted by seq because the live record may be gone (or its slot reused) by
 * the time the handler returns. `owner` is restored around every callback so
 * set_status/defer/host->log are attributed to the right extension. */
typedef struct {
    AgcExtHookFn fn;
    void *ud;
    int priority;
    u64 seq;
    u64 owner;
} HookSnap;

/* Count one consecutive overrun against the handler's live record. Returns true
 * when this overrun trips the 3-strike disable. */
static bool hook_overrun_seq(u64 seq) {
    for (size_t i = 0; i < AGENTC_EXT_MAX_HOOKS; i++) {
        HookRec *h = &g_hooks[i];
        if (!h->used || h->seq != seq) continue;
        h->overruns++;
        if (h->overruns >= AGENTC_EXT_MAX_OVERRUNS) {
            h->disabled = true;
            return true;
        }
        return false;
    }
    return false;
}

static void hook_note_ok_seq(u64 seq) {
    for (size_t i = 0; i < AGENTC_EXT_MAX_HOOKS; i++)
        if (g_hooks[i].used && g_hooks[i].seq == seq) {
            g_hooks[i].overruns = 0;
            return;
        }
}

/* true when `json` parses as a JSON object. Invalid JSON and every non-object
 * value (including null and arrays) are rejected. */
static bool json_object_ok(const char *json) {
    if (!json || !json[0]) return false;
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *o = agentc_json_parse_in(a, json, agentc_strlen(json));
    bool ok = agentc_json_type(o) == AGENTC_JSON_OBJ;
    agentc_json_arena_free(a);
    return ok;
}

/* true when `json` is an object with key == true */
static bool json_true_field(const char *json, const char *key) {
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *o = agentc_json_parse_in(a, json, agentc_strlen(json));
    bool v = agentc_json_get_bool(o, key, false);
    agentc_json_arena_free(a);
    return v;
}

static AgcExtResult emit_walk(const char *point, const char *payload_json) {
    AgcExtResult r;
    agentc_memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    if (!point) return r;
    const char *base_payload = payload_json ? payload_json : "{}";
    const PointInfo *pi = point_info(point);
    uint32_t caps = pi ? pi->caps : AGENTC_HOOK_OBSERVE;
    int policy = pi ? pi->policy : POLICY_CHAIN;
    int merge = pi ? pi->merge : MERGE_FIELDS;
    int fail = pi ? pi->fail : FAIL_OPEN;

    HookSnap list[AGENTC_EXT_MAX_HOOKS];
    size_t n = 0;
    for (size_t i = 0; i < AGENTC_EXT_MAX_HOOKS; i++) {
        if (g_hooks[i].used && !g_hooks[i].disabled &&
            agentc_streq(g_hooks[i].point, point)) {
            list[n].fn = g_hooks[i].fn;
            list[n].ud = g_hooks[i].ud;
            list[n].priority = g_hooks[i].priority;
            list[n].seq = g_hooks[i].seq;
            list[n].owner = g_hooks[i].owner;
            n++;
        }
    }
    if (n == 0) return r;
    /* stable insertion sort by (priority, seq) */
    for (size_t i = 1; i < n; i++) {
        HookSnap key = list[i];
        size_t j = i;
        while (j > 0) {
            HookSnap *a = &list[j - 1];
            bool before = a->priority < key.priority ||
                          (a->priority == key.priority && a->seq < key.seq);
            if (before) break;
            list[j] = list[j - 1];
            j--;
        }
        list[j] = key;
    }

    char *acc = NULL;    /* accumulated result (host-owned) */
    char *cur = NULL;    /* transient payload for the next handler */
    for (size_t i = 0; i < n; i++) {
        HookSnap *h = &list[i];
        const char *payload = cur ? cur : base_payload;
        const char *who = owner_name(h->owner);
        /* An earlier handler may have unloaded this handler's extension: the
         * ExtRec is cleared, so skip the stale snapshot rather than calling a
         * callback whose userdata shutdown() may already have freed (D2). */
        if (h->owner != 0 && !who) continue;
        if (!who) who = "?";
        char *res = NULL;
        u64 saved_owner = owner_get();
        owner_set(h->owner);
        i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
        int rc = h->fn(h->ud, point, payload, &res);
        i64 t1 = os_now_ns(OS_CLOCK_MONOTONIC);
        owner_set(saved_owner);
        bool overran = t1 - t0 > AGENTC_EXT_BUDGET_NS;
        if (overran) {
            bool disabled_now = hook_overrun_seq(h->seq);
            agentc_logf(2, "ext: %s overran its budget at %s", who, point);
            if (disabled_now)
                agentc_logf(3, "ext: %s disabled at %s after %d consecutive overruns",
                            who, point, AGENTC_EXT_MAX_OVERRUNS);
            if (fail == FAIL_OPEN) {
                agentc_free(res);
                continue;
            }
            rc = -1;   /* fail-closed: the overrun blocks this occurrence too */
        } else {
            hook_note_ok_seq(h->seq);
        }
        if (rc < 0) {
            if (!overran) agentc_logf(3, "ext: %s failed at %s", who, point);
            agentc_free(res);
            if (fail == FAIL_CLOSED) { r.handled = 1; r.blocked = 1; break; }
            continue;
        }
        if (res && !json_object_ok(res)) {
            /* A handler result must be a JSON object (or absent). Anything else
             * is a handler failure: never merge a scalar/null into the chain. */
            agentc_logf(2, "ext: %s returned an invalid result at %s", who, point);
            agentc_free(res);
            res = NULL;
            if (fail == FAIL_CLOSED) { r.handled = 1; r.blocked = 1; break; }
            continue;
        }
        if (caps != AGENTC_HOOK_OVERRIDE) {
            /* observe handlers may not return a result or claim
             * handled; both are logged and ignored. */
            if (res) {
                agentc_logf(3, "ext: %s observe handler returned a result at %s, ignored",
                            who, point);
                agentc_free(res);
                res = NULL;
            }
            if (rc == 1) {
                agentc_logf(3, "ext: %s observe handler returned 1 at %s, ignored",
                            who, point);
                rc = 0;
            }
        }
        if (res) {
            if (merge == MERGE_REPLACE) {
                /* replace: the result is the whole new accumulator, and the next
                 * handler receives that accumulator instead of a merge with the
                 * base payload. */
                agentc_free(acc);
                acc = res;
                res = NULL;
                agentc_free(cur);
                cur = agentc_strdup(acc);
            } else {
                char *merged = json_fields_apply(acc ? acc : "{}", res, true);
                agentc_free(res);
                agentc_free(acc);
                acc = merged;
                agentc_free(cur);
                cur = json_fields_apply(base_payload, acc, false);
            }
            if (policy == POLICY_CHAIN_VETO && json_true_field(acc, "block")) {
                r.handled = 1;
                r.blocked = 1;
                break;
            }
        }
        if (rc == 1) {
            r.handled = 1;
            if (policy == POLICY_CHAIN_VETO) { r.blocked = 1; break; }
            if (policy == POLICY_FIRST) break;
        }
    }

    agentc_free(cur);
    r.result_json = acc;
    return r;
}

/* Nesting guard: a handler that emits a point recursively would otherwise walk
 * the stack until it dies. At the cap an overridden point answers blocked (the
 * operation is contained, fail-closed) and every observe point answers empty. */
static int g_emit_depth;

AgcExtResult agentc_ext_emit(const char *point, const char *payload_json) {
    AgcExtResult r;
    agentc_memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    if (!point) return r;
    if (g_emit_depth >= AGENTC_EXT_MAX_EMIT_DEPTH) {
        const PointInfo *pi = point_info(point);
        agentc_logf(3, "ext: emit recursion limit %d reached at %s",
                    AGENTC_EXT_MAX_EMIT_DEPTH, point);
        if (pi && pi->fail == FAIL_CLOSED) {
            r.handled = 1;
            r.blocked = 1;
        }
        return r;
    }
    g_emit_depth++;
    r = emit_walk(point, payload_json);
    g_emit_depth--;
    return r;
}

/* ------------------------------------------------------------ JSON scratch */
/* json_get_* returns host-owned strings valid until the next pump/shutdown. */
static AgcVec g_json_scratch;

static void json_scratch_clear(void) {
    for (size_t i = 0; i < g_json_scratch.len; i++)
        agentc_free(((char **)g_json_scratch.p)[i]);
    g_json_scratch.len = 0;
}

static void json_scratch_add(char *s) {
    *(char **)agentc_vec_push(&g_json_scratch, sizeof(char *)) = s;
}

static const AgcJson *json_walk(const AgcJson *v, const char *path) {
    if (!v || !path) return NULL;
    const char *p = path;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        size_t n = (size_t)(dot - p);
        if (n == 0) return NULL;
        int type = agentc_json_type(v);
        if (type == AGENTC_JSON_OBJ) {
            if (n >= 256) return NULL;
            char key[256];
            agentc_memcpy(key, p, n);
            key[n] = 0;
            v = agentc_json_get(v, key);
        } else if (type == AGENTC_JSON_ARR) {
            bool ok = false;
            u64 idx = agentc_parse_u64(p, n, &ok);
            if (!ok) return NULL;
            v = agentc_json_at(v, (size_t)idx);
        } else {
            return NULL;
        }
        if (!v) return NULL;
        p = *dot ? dot + 1 : dot;
    }
    return v;
}

static const AgcJson *json_parse_path(AgcJsonArena *a, const char *json, const char *path) {
    if (!json) return NULL;
    AgcJson *root = agentc_json_parse_in(a, json, agentc_strlen(json));
    return json_walk(root, path);
}

static const char *host_json_get_str(const char *json, const char *path,
                                     const char *dflt) {
    AgcJsonArena *a = agentc_json_arena_new(0);
    const AgcJson *v = json_parse_path(a, json, path);
    size_t len = 0;
    const char *s = agentc_json_str(v, &len);
    if (!s) {
        agentc_json_arena_free(a);
        return dflt;
    }
    char *copy = agentc_strdup_len(s, len);
    json_scratch_add(copy);
    agentc_json_arena_free(a);
    return copy;
}

static long long host_json_get_int(const char *json, const char *path,
                                   long long dflt) {
    AgcJsonArena *a = agentc_json_arena_new(0);
    const AgcJson *v = json_parse_path(a, json, path);
    if (agentc_json_type(v) != AGENTC_JSON_NUM) {
        agentc_json_arena_free(a);
        return dflt;
    }
    size_t len = 0;
    const char *num = agentc_json_num(v, &len);
    bool ok = false;
    i64 r = agentc_parse_i64(num, len, &ok);
    agentc_json_arena_free(a);
    return ok ? (long long)r : dflt;
}

static int host_json_get_bool(const char *json, const char *path, int dflt) {
    AgcJsonArena *a = agentc_json_arena_new(0);
    const AgcJson *v = json_parse_path(a, json, path);
    int r = dflt;
    if (agentc_json_type(v) == AGENTC_JSON_TRUE) r = 1;
    else if (agentc_json_type(v) == AGENTC_JSON_FALSE) r = 0;
    agentc_json_arena_free(a);
    return r;
}

static char *host_json_escape(const char *s) {
    AgcBuf b = { 0 };
    agentc_json_escape(&b, s ? s : "", s ? agentc_strlen(s) : 0);
    if (!b.p) {
        b.p = agentc_alloc(1);
        b.p[0] = 0;
        b.cap = 1;
    }
    return (char *)b.p;
}

/* -------------------------------------------------------------- host core */

static void *host_alloc(size_t n) { return agentc_alloc(n); }
static void host_free(void *p) { agentc_free(p); }
/* Extension log lines are attributed on the main loop: an active owner gets an
 * `[ext:<name>] ` prefix, so a log emitted from a hook/tool/command names its
 * source without the extension spelling it out. Workers must use defer, not
 * host->log. */
static void host_log(int level, const char *msg) {
    const char *who = owner_name(owner_get());
    if (who) agentc_logf(level, "[ext:%s] %s", who, msg ? msg : "");
    else agentc_logs(level, msg ? msg : "");
}
static char *host_strdup(const char *s) { return agentc_strdup(s ? s : ""); }

static void host_out_write(void *out, const char *bytes, size_t n) {
    if (out && bytes && n) agentc_buf_push((AgcBuf *)out, bytes, n);
}

static int host_is_cancelled(const AgcExtHost *host, const char *signal_token) {
    (void)host;
    if (!signal_token || !g_signal || !agentc_streq(g_signal, signal_token)) return 0;
    return (g_signal_cancel && *g_signal_cancel) ? 1 : 0;
}

static void host_emit(const AgcExtHost *host, const char *point,
                      const char *payload_json, AgcExtResult *out) {
    (void)host;
    AgcExtResult r = agentc_ext_emit(point, payload_json);
    if (!out) { agentc_free(r.result_json); return; }
    /* Honor the caller's struct_size: write only the fields it covers, and
     * never hand back a result the caller cannot hold. */
    uint32_t sz = out->struct_size;
    if (sz < (uint32_t)(offsetof(AgcExtResult, handled) + sizeof out->handled)) {
        agentc_logf(3, "ext: emit: result struct too small");
        agentc_free(r.result_json);
        return;
    }
    out->handled = r.handled;
    if (sz >= (uint32_t)(offsetof(AgcExtResult, blocked) + sizeof out->blocked))
        out->blocked = r.blocked;
    if (sz >= (uint32_t)(offsetof(AgcExtResult, result_json) + sizeof out->result_json)) {
        out->result_json = r.result_json;
    } else {
        agentc_logf(3, "ext: emit: result struct has no result_json field");
        agentc_free(r.result_json);
    }
}

/* ---------------------------------------------------------------- context */

static char *g_cwd;
static char *g_session_id;
static char *g_session_file;
static char *g_system_prompt;

static void set_ctx_str(char **slot, const char *v, bool clear_empty) {
    if (!v) return;
    agentc_free(*slot);
    *slot = (clear_empty && !v[0]) ? NULL : agentc_strdup(v);
}

void agentc_ext_set_context(const AgcExtContext *ctx) {
    if (!ctx) return;
    set_ctx_str(&g_cwd, ctx->cwd, false);
    set_ctx_str(&g_session_id, ctx->session_id, false);
    set_ctx_str(&g_session_file, ctx->session_file, true);
    set_ctx_str(&g_system_prompt, ctx->system_prompt, false);
}

static const char *host_cwd(const AgcExtHost *host) {
    (void)host;
    if (!g_cwd) {
        char buf[4096];
        if (os_getcwd(buf, sizeof buf) >= 0) g_cwd = agentc_strdup(buf);
    }
    return g_cwd ? g_cwd : "";
}

static const char *host_session_id(const AgcExtHost *host) {
    (void)host;
    return g_session_id ? g_session_id : "";
}

static const char *host_session_file(const AgcExtHost *host) {
    (void)host;
    return g_session_file;
}

static const char *host_system_prompt(const AgcExtHost *host) {
    (void)host;
    return g_system_prompt ? g_system_prompt : "";
}

static AgcExtEntrySink g_entry_sink;
static void *g_entry_ud;

void agentc_ext_set_entry_sink(AgcExtEntrySink sink, void *ud) {
    g_entry_sink = sink;
    g_entry_ud = ud;
}

void agentc_ext_append_entry(const char *custom_type, const char *data_json) {
    if (!custom_type) custom_type = "";
    if (!data_json) data_json = "{}";
    if (!ext_valid_charset(custom_type)) {
        agentc_logf(3, "ext: append_entry: invalid custom_type");
        return;
    }
    /* Bound the payload before parsing; the json-object check must
     * never walk an unbounded string. */
    if (agentc_strlen(data_json) > AGENTC_EXT_APPEND_ENTRY_MAX) {
        agentc_logf(3, "ext: append_entry: data_json over %llu bytes, rejected",
                    (unsigned long long)AGENTC_EXT_APPEND_ENTRY_MAX);
        return;
    }
    if (!json_object_ok(data_json)) {
        agentc_logf(3, "ext: append_entry: data_json is not a JSON object");
        return;
    }
    if (g_entry_sink)
        g_entry_sink(g_entry_ud, custom_type, data_json);
    else
        agentc_logf(0, "ext: append_entry %s", custom_type);
}

/* -------------------------------------------------------------------- UI */

static AgcExtUiSink g_ui;
static void *g_ui_ud;

void agentc_ext_set_ui_sink(const AgcExtUiSink *sink, void *ud) {
    if (sink) g_ui = *sink;
    else agentc_memset(&g_ui, 0, sizeof g_ui);
    g_ui_ud = ud;
}

static void host_notify(const char *message, int level) {
    if (g_ui.notify) g_ui.notify(g_ui_ud, message, level);
    else agentc_logf(level, "ext notify: %s", message ? message : "");
}

static void host_set_status(const char *key, const char *text) {
    /* The keyed segment table is the single mechanism, so the UI
     * sink's set_status slot is deliberately not used. */
    set_status_set(key, text);
}

static void host_set_title(const char *title) {
    if (g_ui.set_title) g_ui.set_title(g_ui_ud, title);
    else agentc_logf(0, "ext title: %s", title ? title : "");
}

static AgcExtModelFn g_model_fn;
static void *g_model_ud;
static AgcExtThinkingFn g_thinking_fn;
static void *g_thinking_ud;

void agentc_ext_set_model_sink(AgcExtModelFn cb, void *ud) {
    g_model_fn = cb;
    g_model_ud = ud;
}

void agentc_ext_set_thinking_sink(AgcExtThinkingFn cb, void *ud) {
    g_thinking_fn = cb;
    g_thinking_ud = ud;
}

static int host_set_model(const char *provider, const char *model) {
    if (g_model_fn) return g_model_fn(g_model_ud, provider, model);
    agentc_logf(2, "ext: set_model: no app sink installed");
    return -38;   /* ENOSYS: the front end has not installed a sink */
}

static void host_set_thinking(const char *level) {
    if (g_thinking_fn) g_thinking_fn(g_thinking_ud, level);
}

static void host_request_recompose(const AgcExtHost *host) {
    (void)host;
    /* The recomposed table is applied at the turn boundary; set the flag now so
     * request_recompose is never a lie. */
    g_dirty = true;
}

/* ------------------------------------------------------- deferred queue */

typedef struct {
    void (*cb)(void *);
    void *ud;
    u64 owner;
} DeferSlot;

static DeferSlot g_defer[AGENTC_EXT_MAX_DEFER];
static u32 g_defer_head, g_defer_tail;
static volatile int g_defer_lock;
static u32 g_defer_dropped;   /* overflow count; logged on the main loop only */

static void host_defer(const AgcExtHost *host, void (*cb)(void *userdata),
                       void *userdata) {
    (void)host;
    if (!cb) return;
    while (__atomic_test_and_set(&g_defer_lock, __ATOMIC_ACQUIRE)) {}
    u32 next = (g_defer_head + 1) % AGENTC_EXT_MAX_DEFER;
    if (next == g_defer_tail) {
        /* May run on a worker: never call host->log here. */
        __atomic_fetch_add(&g_defer_dropped, 1u, __ATOMIC_RELAXED);
        __atomic_clear(&g_defer_lock, __ATOMIC_RELEASE);
        return;
    }
    g_defer[g_defer_head].cb = cb;
    g_defer[g_defer_head].ud = userdata;
    g_defer[g_defer_head].owner = owner_get();
    g_defer_head = next;
    __atomic_clear(&g_defer_lock, __ATOMIC_RELEASE);
}

static bool defer_pop(DeferSlot *out) {
    while (__atomic_test_and_set(&g_defer_lock, __ATOMIC_ACQUIRE)) {}
    if (g_defer_tail == g_defer_head) {
        __atomic_clear(&g_defer_lock, __ATOMIC_RELEASE);
        return false;
    }
    *out = g_defer[g_defer_tail];
    g_defer_tail = (g_defer_tail + 1) % AGENTC_EXT_MAX_DEFER;
    __atomic_clear(&g_defer_lock, __ATOMIC_RELEASE);
    return true;
}

/* Drop one owner's queued callbacks without invoking them (unload/shutdown).
 * Compacting in place keeps the ring invariants and is safe while a worker is
 * enqueueing: the spinlock covers the whole rewrite. */
static void defer_remove_owner(u64 owner) {
    while (__atomic_test_and_set(&g_defer_lock, __ATOMIC_ACQUIRE)) {}
    DeferSlot keep[AGENTC_EXT_MAX_DEFER];
    u32 n = 0;
    for (u32 i = g_defer_tail; i != g_defer_head; i = (i + 1) % AGENTC_EXT_MAX_DEFER)
        if (g_defer[i].owner != owner) keep[n++] = g_defer[i];
    for (u32 i = 0; i < n; i++) g_defer[i] = keep[i];
    g_defer_tail = 0;
    g_defer_head = n % AGENTC_EXT_MAX_DEFER;
    __atomic_clear(&g_defer_lock, __ATOMIC_RELEASE);
}

static void defer_reset(void) {
    while (__atomic_test_and_set(&g_defer_lock, __ATOMIC_ACQUIRE)) {}
    g_defer_head = g_defer_tail = 0;
    __atomic_store_n(&g_defer_dropped, 0u, __ATOMIC_RELAXED);
    __atomic_clear(&g_defer_lock, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------- HTTP */

typedef struct {
    int used;
    volatile bool cancelled;   /* set by http_cancel; polled mid-request by wire/http.c */
    int in_flight;             /* being dispatched by http_pump_one right now */
    uint64_t id;
    char *method, *url, *headers;
    char *body;
    uint64_t body_len;
    void (*cb)(void *ud, int status, const char *headers_json,
               const char *body, uint64_t body_len);
    void *ud;
    u64 owner;
} ExtHttp;

static ExtHttp g_http[AGENTC_EXT_MAX_HTTP];
static u64 g_http_seq;

static u32 hdr_hex(char c) {
    if (c >= '0' && c <= '9') return (u32)(c - '0');
    if (c >= 'a' && c <= 'f') return (u32)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (u32)(c - 'A' + 10);
    return 0xff;
}

static void hdr_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static bool hdr_string(const char **pp, AgcBuf *out) {
    const char *p = *pp;
    if (*p != '"') return false;
    p++;
    while (*p && *p != '"') {
        u8 c = (u8)*p++;
        if (c == '\\') {
            char e = *p++;
            switch (e) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                u32 cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = p[i];
                    if (!h) return false;
                    u32 v = hdr_hex(h);
                    if (v == 0xff) return false;
                    cp = (cp << 4) | v;
                }
                p += 4;
                if (cp < 0x20 || cp >= 0x7f) continue;
                c = (u8)cp;
                break;
            }
            default: return false;
            }
        }
        if (c == '\r' || c == '\n') c = ' ';
        agentc_buf_byte(out, c);
    }
    if (*p != '"') return false;
    *pp = p + 1;
    return true;
}

/* RFC 7230 token (tchar): ! # $ % & ' * + - . ^ _ ` | ~ , DIGIT, ALPHA. Used
 * for provider-authored header names and before_provider_headers patch keys. */
static bool hdr_key_ok(const u8 *p, size_t n) {
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++) {
        u8 c = p[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '!' || c == '#' || c == '$' ||
                  c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
                  c == '-' || c == '.' || c == '^' || c == '_' || c == '`' ||
                  c == '|' || c == '~';
        if (!ok) return false;
    }
    return true;
}

/* G: an HTTP request method must be an RFC 7230 token. A conservative check
 * rejects CR/LF/space/controls (request-splitting) and every non-tchar byte;
 * empty and over-long methods are refused too. */
static bool http_method_ok(const char *m) {
    if (!m || !m[0]) return false;
    size_t n = 0;
    for (; m[n]; n++) {
        if (n >= 32) return false;   /* methods are short; bound the token */
        u8 c = (u8)m[n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '!' || c == '#' || c == '$' ||
                  c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
                  c == '-' || c == '.' || c == '^' || c == '_' || c == '`' ||
                  c == '|' || c == '~';
        if (!ok) return false;
    }
    return true;
}

static bool hdr_forbidden(const char *k, size_t n);

/* Parse an extension-supplied flat headers object into CRLF lines (the same
 * shape the provider header block uses). The forbidden trio
 * (Host/Content-Length/Transfer-Encoding), non-token names and a malformed
 * document are dropped with a log; a malformed document returns NULL and the
 * request still runs with no extra headers. */
static char *headers_json_to_lines(const char *json) {
    if (!json || !json[0]) return NULL;
    const char *p = json;
    hdr_ws(&p);
    if (*p != '{') {
        agentc_logf(2, "ext: http_request: malformed headers_json dropped");
        return NULL;
    }
    p++;
    AgcBuf out = { 0 };
    bool first = true;
    for (;;) {
        hdr_ws(&p);
        if (*p == '}') { p++; break; }
        if (!first) {
            if (*p != ',') goto malformed;
            p++;
            hdr_ws(&p);
        }
        AgcBuf key = { 0 }, val = { 0 };
        bool ok = hdr_string(&p, &key);
        hdr_ws(&p);
        if (ok && *p == ':') {
            p++;
            hdr_ws(&p);
            ok = hdr_string(&p, &val);
        } else {
            ok = false;
        }
        if (ok) {
            if (hdr_forbidden((const char *)key.p, key.len)) {
                agentc_logf(2, "ext: http_request: forbidden header dropped");
            } else if (hdr_key_ok(key.p, key.len)) {
                agentc_buf_push(&out, key.p, key.len);
                agentc_buf_cstr(&out, ": ");
                agentc_buf_push(&out, val.p, val.len);
                agentc_buf_cstr(&out, "\r\n");
            } else {
                agentc_logf(2, "ext: http_request: invalid header name dropped");
            }
        }
        agentc_buf_free(&key);
        agentc_buf_free(&val);
        if (!ok) goto malformed;
        first = false;
    }
    hdr_ws(&p);
    if (*p) goto malformed;
    return (char *)out.p;

malformed:
    agentc_buf_free(&out);
    agentc_logf(2, "ext: http_request: malformed headers_json dropped");
    return NULL;
}

/* ------------------------------------------------ header-block helpers
 *
 * Exposed through ext.h for the before_provider_headers override. They parse
 * the provider-built "Name: value\r\n" block; hdr_string supplies the JSON
 * string decoding and the CR/LF-to-space sanitizer, hdr_key_ok the token check.
 * Malformed lines (no colon / bad token) are skipped rather than kept: the
 * only produced block is the provider request head. */

typedef struct {
    const char *name;
    size_t name_len;
    const char *val;
    size_t val_len;
} HdrLine;

/* Consume one line at *pp (always advances past its newline). Returns true when
 * the line is a valid header and fills `out`; false means the caller skips it. */
static bool hdr_line_next(const char **pp, HdrLine *out) {
    const char *p = *pp;
    const char *e = p;
    while (*e && *e != '\n') e++;
    size_t len = (size_t)(e - p);
    *pp = (*e == '\n') ? e + 1 : e;
    if (len > 0 && p[len - 1] == '\r') len--;
    const char *colon = NULL;
    for (size_t i = 0; i < len; i++)
        if (p[i] == ':') { colon = p + i; break; }
    if (!colon) return false;
    const char *ns = p, *ne = colon;
    while (ne > ns && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
    const char *vs = colon + 1, *ve = p + len;
    while (vs < ve && (*vs == ' ' || *vs == '\t')) vs++;
    while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
    if (!hdr_key_ok((const u8 *)ns, (size_t)(ne - ns))) return false;
    out->name = ns;
    out->name_len = (size_t)(ne - ns);
    out->val = vs;
    out->val_len = (size_t)(ve - vs);
    return true;
}

static HdrLine *hdr_parse_lines(const char *head, size_t *out_n) {
    HdrLine *arr = NULL;
    size_t n = 0, cap = 0;
    const char *p = head ? head : "";
    while (*p) {
        HdrLine l;
        if (hdr_line_next(&p, &l)) {
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                arr = agentc_realloc(arr, cap * sizeof *arr);
            }
            arr[n++] = l;
        }
    }
    *out_n = n;
    return arr;
}

char *agentc_ext_headers_to_json(const char *head) {
    size_t n = 0;
    HdrLine *lines = hdr_parse_lines(head, &n);
    AgcBuf out = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &out);
    agentc_jsonw_obj(&w);
    for (size_t i = 0; i < n; i++) {
        bool later = false;
        for (size_t j = i + 1; j < n && !later; j++)
            later = agentc_str_ieq(lines[i].name, lines[i].name_len,
                                   lines[j].name, lines[j].name_len);
        if (later) continue;   /* duplicate names: last occurrence wins */
        AgcBuf k = { 0 };
        agentc_buf_push(&k, lines[i].name, lines[i].name_len);
        agentc_jsonw_key(&w, (const char *)k.p);
        agentc_buf_free(&k);
        agentc_jsonw_str(&w, lines[i].val, lines[i].val_len);
    }
    agentc_jsonw_end(&w);
    agentc_free(lines);
    return (char *)out.p;
}

typedef struct {
    char *key;
    size_t key_len;
    char *val;
    size_t val_len;
    bool del;
    bool matched;
} HdrPatchEntry;

static void hdr_patch_entries_free(HdrPatchEntry *es, size_t n) {
    for (size_t i = 0; i < n; i++) {
        agentc_free(es[i].key);
        agentc_free(es[i].val);
    }
    agentc_free(es);
}

static bool hdr_forbidden(const char *k, size_t n) {
    return agentc_str_ieq(k, n, "content-length", 14) ||
           agentc_str_ieq(k, n, "transfer-encoding", 17) ||
           agentc_str_ieq(k, n, "host", 4);
}

/* "null" at the start of a value position; the trailing characters are
 * validated by the caller's comma/brace check. */
static bool hdr_null_literal(const char *p) {
    return agentc_strlen(p) >= 4 && p[0] == 'n' && p[1] == 'u' && p[2] == 'l' &&
           p[3] == 'l';
}

static void hdr_buf_value(AgcBuf *out, const char *v, size_t n) {
    if (n) agentc_buf_push(out, v, n);
}

char *agentc_ext_headers_apply_patch(const char *head, const char *patch_json) {
    if (!patch_json) return NULL;
    const char *p = patch_json;
    hdr_ws(&p);
    if (*p != '{') return NULL;
    p++;

    HdrPatchEntry *es = NULL;
    size_t n = 0, cap = 0;
    bool first = true;
    for (;;) {
        hdr_ws(&p);
        if (*p == '}') { p++; break; }
        if (!first) {
            if (*p != ',') goto fail;
            p++;
            hdr_ws(&p);
        }
        AgcBuf key = { 0 }, val = { 0 };
        bool del = false;
        bool ok = hdr_string(&p, &key);
        hdr_ws(&p);
        if (ok && *p == ':') {
            p++;
            hdr_ws(&p);
            if (hdr_null_literal(p)) {
                p += 4;
                del = true;
            } else {
                ok = hdr_string(&p, &val);
            }
        } else {
            ok = false;
        }
        if (!ok || !hdr_key_ok(key.p, key.len)) {
            agentc_buf_free(&key);
            agentc_buf_free(&val);
            goto fail;
        }
        first = false;
        if (hdr_forbidden((const char *)key.p, key.len)) {
            agentc_logf(2, "ext: header patch: forbidden header dropped");
            agentc_buf_free(&key);
            agentc_buf_free(&val);
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            es = agentc_realloc(es, cap * sizeof *es);
            agentc_memset(&es[n], 0, sizeof es[n]);
        }
        es[n].key = (char *)key.p;
        es[n].key_len = key.len;
        es[n].val = (char *)val.p;
        es[n].val_len = val.len;
        es[n].del = del;
        es[n].matched = false;
        n++;
    }
    hdr_ws(&p);
    if (*p) goto fail;

    /* Rebuild the head. Every occurrence of a patched name is replaced in
     * place (or dropped for null), preserving order; unmatched string keys are
     * appended in patch order. Duplicate patch keys: the last occurrence
     * supplies the value and the append position. */
    size_t nlines = 0;
    HdrLine *lines = hdr_parse_lines(head, &nlines);
    AgcBuf out = { 0 };
    for (size_t i = 0; i < nlines; i++) {
        HdrPatchEntry *m = NULL;
        for (size_t j = 0; j < n; j++)
            if (agentc_str_ieq(lines[i].name, lines[i].name_len, es[j].key, es[j].key_len))
                m = &es[j];   /* last match wins */
        if (m) {
            m->matched = true;
            if (m->del) continue;
            agentc_buf_push(&out, lines[i].name, lines[i].name_len);
            agentc_buf_cstr(&out, ": ");
            hdr_buf_value(&out, m->val, m->val_len);
            agentc_buf_cstr(&out, "\r\n");
        } else {
            agentc_buf_push(&out, lines[i].name, lines[i].name_len);
            agentc_buf_cstr(&out, ":");
            if (lines[i].val_len) {
                agentc_buf_byte(&out, ' ');
                agentc_buf_push(&out, lines[i].val, lines[i].val_len);
            }
            agentc_buf_cstr(&out, "\r\n");
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (es[i].del || es[i].matched) continue;
        bool later_same = false;
        for (size_t j = i + 1; j < n && !later_same; j++)
            later_same = agentc_str_ieq(es[i].key, es[i].key_len, es[j].key, es[j].key_len);
        if (later_same) continue;
        agentc_buf_push(&out, es[i].key, es[i].key_len);
        agentc_buf_cstr(&out, ": ");
        hdr_buf_value(&out, es[i].val, es[i].val_len);
        agentc_buf_cstr(&out, "\r\n");
    }
    agentc_free(lines);
    hdr_patch_entries_free(es, n);
    if (!out.p) return agentc_strdup("");
    return (char *)out.p;

fail:
    hdr_patch_entries_free(es, n);
    return NULL;
}

static uint64_t host_http_request(const AgcExtHost *host, const char *method,
                                  const char *url, const char *headers_json,
                                  const char *body, uint64_t body_len,
                                  void (*cb)(void *ud, int status,
                                             const char *headers_json,
                                             const char *body, uint64_t body_len),
                                  void *ud) {
    (void)host;
    if (!http_method_ok(method)) {
        agentc_logf(2, "ext: http_request: rejected non-token method");
        return 0;
    }
    if (!url || !url[0] || !cb) return 0;
    if (!body && body_len > 0) {
        agentc_logf(3, "ext: http_request: NULL body with body_len > 0");
        return 0;
    }
    size_t slot = AGENTC_EXT_MAX_HTTP;
    for (size_t i = 0; i < AGENTC_EXT_MAX_HTTP; i++)
        if (!g_http[i].used) { slot = i; break; }
    if (slot == AGENTC_EXT_MAX_HTTP) {
        agentc_logf(3, "ext: http_request: queue full");
        return 0;
    }
    ExtHttp *r = &g_http[slot];
    agentc_memset(r, 0, sizeof *r);
    r->used = 1;
    r->id = ++g_http_seq;
    r->owner = owner_get();
    r->method = agentc_strdup(method);
    r->url = agentc_strdup(url);
    r->headers = headers_json_to_lines(headers_json);
    if (body && body_len) {
        r->body = agentc_alloc((size_t)body_len);
        agentc_memcpy(r->body, body, (size_t)body_len);
    }
    r->body_len = body_len;
    r->cb = cb;
    r->ud = ud;
    return r->id;
}

static void host_http_cancel(const AgcExtHost *host, uint64_t request) {
    (void)host;
    if (!request) return;
    for (size_t i = 0; i < AGENTC_EXT_MAX_HTTP; i++)
        if (g_http[i].used && g_http[i].id == request) {
            g_http[i].cancelled = 1;
            return;
        }
}

static void http_release(ExtHttp *r) {
    agentc_free(r->method);
    agentc_free(r->url);
    agentc_free(r->headers);
    agentc_free(r->body);
    agentc_memset(r, 0, sizeof *r);
}

static int http_body_cb(void *ud, const void *p, size_t n) {
    agentc_buf_push((AgcBuf *)ud, p, n);
    return 0;
}

/* Dispatch one queued request. The wire layer is not resumable
 * (agentc_http_run polls to completion), so a request occupies one pump call
 * for up to AGENTC_EXT_HTTP_TIMEOUT; the re-entrancy guard in
 * agentc_ext_pump keeps that bounded call from recursing into another
 * dispatch. `in_flight` is belt-and-braces: even a caller that reaches this
 * function while a slot is already running skips that slot instead of
 * duplicating its callback. The request's `cancelled` flag is handed to the
 * wire layer, so http_cancel can interrupt a request that is already inside
 * agentc_http_run: the run returns -ECANCELED on the next poll slice and the
 * callback sees that status with whatever partial body was decoded. */
static void http_pump_one(void) {
    for (size_t i = 0; i < AGENTC_EXT_MAX_HTTP; i++) {
        ExtHttp *r = &g_http[i];
        if (!r->used || r->in_flight) continue;
        if (r->cancelled) { http_release(r); return; }
        r->in_flight = 1;
        AgcHttp *h = agentc_http_new(r->method, r->url, r->headers, r->body,
                                     (size_t)r->body_len);
        if (h) agentc_http_set_cancel(h, &r->cancelled);
        AgcBuf resp = { 0 };
        int rc = h ? agentc_http_run(h, http_body_cb, &resp, AGENTC_EXT_HTTP_TIMEOUT)
                   : -1;
        int status = rc < 0 ? rc : agentc_http_status(h);
        const char *ct = h ? agentc_http_header(h, "content-type") : NULL;
        u64 saved_owner = owner_get();
        owner_set(r->owner);
        if (ct) {
            AgcBuf hj = { 0 };
            AgcJsonW w;
            jsonw_out(&w, &hj);
            agentc_jsonw_obj(&w);
            agentc_jsonw_key(&w, "content-type");
            agentc_jsonw_cstr(&w, ct);
            agentc_jsonw_end(&w);
            r->cb(r->ud, status, (const char *)hj.p, (const char *)resp.p,
                  (uint64_t)resp.len);
            agentc_buf_free(&hj);
        } else {
            r->cb(r->ud, status, "{}", (const char *)resp.p, (uint64_t)resp.len);
        }
        owner_set(saved_owner);
        agentc_buf_free(&resp);
        if (h) agentc_http_free(h);
        http_release(r);
        return;
    }
}

/* One main-loop duty. Re-entrant calls (the wire poll hook fires inside
 * agentc_http_run, and the TUI poll hook chains the mode pump) are dropped:
 * without the guard a pending HTTP request would be dispatched again while it
 * is already in flight, duplicating its callback. */
static bool g_pumping;

/* ------------------------------------------------ dynamic extension lifetime
 *
 * Each library registered by agentc_ext_register_dynamic() is recorded
 * here against its owner id. The handle is closed exactly once, only when the
 * extension is idle: registrations removed, shutdown() returned, no
 * live async job for the owner, and no extension callback on the stack
 * (owner_get()==0 and not pumping). A close requested while busy is deferred
 * to the last job unlink, the pump epilogue or agentc_ext_shutdown(). At
 * shutdown a mapping still needed by a live job (or by an extension callback
 * on the stack) is kept mapped with a warning and its record dropped
 * (fallback: never-dlclose is accepted for the process lifetime).
 */
typedef struct {
    u64 owner;             /* ExtRec owner id (monotonic, never reused) */
    void *handle;          /* NULL once closed */
    bool close_requested;  /* unload/init-failure asked to close but was busy */
} DynRec;

static AgcVec g_dyn;   /* DynRec */

static DynRec *dyn_find(u64 owner) {
    for (size_t i = 0; i < g_dyn.len; i++) {
        DynRec *d = &((DynRec *)g_dyn.p)[i];
        if (d->handle && d->owner == owner) return d;
    }
    return NULL;
}

/* A live async job holds extension code and its published call strings; the
 * mapping must stay until the job unlinks. Retired records are checked too:
 * agentc_ext_unload() moves a record with live jobs out of g_tools. */
static bool dyn_owner_busy(u64 owner) {
    for (size_t i = 0; i < g_tools.len; i++) {
        ToolRec *t = tool_at(i);
        if (t && t->owner == owner && t->jobs != NULL) return true;
    }
    for (size_t i = 0; i < g_retired.len; i++) {
        ToolRec *t = ((ToolRec **)g_retired.p)[i];
        if (t && t->owner == owner && t->jobs != NULL) return true;
    }
    return false;
}

static void dyn_close(DynRec *d) {
    const AgcExtDynLoader *loader = agentc_ext_dyn_loader();
    if (loader && loader->close) (void)loader->close(d->handle);
    d->handle = NULL;
    d->close_requested = false;
}

static void dyn_try_close(DynRec *d) {
    if (!d || !d->handle || !d->close_requested) return;
    if (owner_get() != 0 || g_pumping) return;   /* extension code on the stack */
    if (dyn_owner_busy(d->owner)) return;
    dyn_close(d);
}

static void dyn_request_close(u64 owner) {
    DynRec *d = dyn_find(owner);
    if (!d) return;
    d->close_requested = true;
    dyn_try_close(d);
}

static void dyn_notify_job_unlinked(u64 owner) {
    DynRec *d = dyn_find(owner);
    if (d && d->close_requested) dyn_try_close(d);
}

static void dyn_close_deferred(void) {
    for (size_t i = 0; i < g_dyn.len; i++) {
        DynRec *d = &((DynRec *)g_dyn.p)[i];
        if (d->close_requested) dyn_try_close(d);
    }
}

/* agentc_ext_shutdown(): close every idle mapping; a busy one stays mapped
 * (fallback). Called while g_tools/g_retired are still intact, so the live-job
 * check is exact. While any extension callback is on the stack no mapping can
 * be closed (the executing library must not be unloaded), so that case is one
 * warning for the whole sweep. */
static void dyn_shutdown_sweep(void) {
    bool in_callback = owner_get() != 0 || g_pumping;
    if (in_callback) {
        bool any = false;
        for (size_t i = 0; i < g_dyn.len; i++)
            if (((DynRec *)g_dyn.p)[i].handle) any = true;
        if (any)
            agentc_logf(2, "ext: dynamic libraries kept mapped at shutdown "
                           "(extension callback on the stack)");
    }
    for (size_t i = 0; i < g_dyn.len; i++) {
        DynRec *d = &((DynRec *)g_dyn.p)[i];
        if (!d->handle) continue;
        if (in_callback) continue;
        if (dyn_owner_busy(d->owner)) {
            agentc_logf(2, "ext: dynamic library kept mapped at shutdown "
                           "(live async job)");
            continue;
        }
        dyn_close(d);
    }
    agentc_vec_free(&g_dyn);
}

/* agentc_ext_register_dynamic(): one scan callback per candidate. Duplicates
 * are skipped before the library is opened; every failure path rolls the
 * owner's registrations back and closes the handle. The descriptor's
 * out.name must equal the file stem. */
static void dyn_register_cb(void *ud, const char *stem, const char *path) {
    size_t *count = ud;
    if (ext_find(stem)) {
        agentc_logf(1, "ext: %s already registered, dynamic copy skipped", stem);
        return;
    }
    /* Re-check the path policy here, not only in the scan: the stat that
     * decided scan membership happened in a different syscall window. */
    if (!agentc_ext_dyn_path_ok(path)) {
        agentc_logf(2, "ext: %s skipped: not a loadable regular file", stem);
        return;
    }
    const AgcExtDynLoader *loader = agentc_ext_dyn_loader();
    if (!loader || !loader->open || !loader->sym) return;
    char err[256];
    err[0] = 0;
    void *handle = NULL;
    int rc = loader->open(path, &handle, err, sizeof err);
    if (rc < 0 || !handle) {
        agentc_logf(3, "ext: %s: %s", stem, err[0] ? err : "open failed");
        return;
    }
    void *sym = NULL;
    err[0] = 0;
    rc = loader->sym(handle, "agentc_ext_init", &sym, err, sizeof err);
    if (rc < 0 || !sym) {
        agentc_logf(3, "ext: %s: %s", stem,
                    err[0] ? err : "agentc_ext_init not found");
        loader->close(handle);
        return;
    }
    /* The entry descriptor call is owner-tagged; anything it registers
     * is attributed to this extension and rolled back on refusal. */
    u64 owner = ++g_ext_seq;
    AgcExt out;
    agentc_memset(&out, 0, sizeof out);
    u64 saved = owner_get();
    owner_set(owner);
    int erc = ((int (*)(const AgcExtHost *, AgcExt *))sym)(agentc_ext_host(), &out);
    owner_set(saved);
    if (erc != 0) {
        agentc_logf(3, "ext: %s entry returned an error", stem);
        owner_remove(owner);
        loader->close(handle);
        return;
    }
    if (!ext_desc_valid(&out, stem)) {
        owner_remove(owner);
        loader->close(handle);
        return;
    }
    if (!out.name || !agentc_streq(out.name, stem)) {
        agentc_logf(3, "ext: %s: descriptor name '%s' does not match the file name",
                    stem, out.name ? out.name : "?");
        owner_remove(owner);
        loader->close(handle);
        return;
    }
    if (!ext_register_impl(&out, stem, owner, true)) {
        owner_remove(owner);
        loader->close(handle);
        return;
    }
    DynRec *d = agentc_vec_push(&g_dyn, sizeof *d);
    d->owner = owner;
    d->handle = handle;
    d->close_requested = false;
    if (count) (*count)++;
}

void agentc_ext_register_dynamic(void) {
    size_t registered = 0;
    (void)agentc_ext_dyn_scan(dyn_register_cb, &registered);
    if (registered)
        agentc_logf(0, "ext: %llu dynamic extension(s) pending",
                    (unsigned long long)registered);
}

void agentc_ext_pump(void) {
    if (g_pumping) return;
    g_pumping = true;
    json_scratch_clear();
    u32 dropped = __atomic_exchange_n(&g_defer_dropped, 0u, __ATOMIC_RELAXED);
    if (dropped)
        agentc_logf(2, "ext: defer queue full, %llu callback(s) dropped",
                    (unsigned long long)dropped);
    DeferSlot d;
    int budget = 1024;
    while (budget-- > 0 && defer_pop(&d)) {
        u64 saved = owner_get();
        owner_set(d.owner);
        d.cb(d.ud);
        owner_set(saved);
    }
    http_pump_one();
    /* periodic default-extension work (MCP connect/sync). Run each callback
     * with its owner active so tools it adds are owner-tagged and removable. */
    for (size_t i = 0; i < g_pumps.len; i++) {
        PumpRec *p = &((PumpRec *)g_pumps.p)[i];
        if (!p->fn) continue;
        u64 saved = owner_get();
        owner_set(p->owner);
        (void)p->fn(p->ud);
        owner_set(saved);
    }
    g_pumping = false;
    /* Deferred dynamic-library closes wait for this point, where no
     * extension callback is on the stack and a close cannot unload code that
     * is still executing. */
    dyn_close_deferred();
}

/* --------------------------------------------------------- custom providers
 *
 * host_add_provider copies an extension contribution into a stable
 * ProviderRec and registers an AgcProviderOps row whose callbacks are the
 * adapters below. Records live in a pointer vector so they never move; an
 * owner's rows are retired (alive=false, ops.retired=true) by owner_remove and
 * freed only by agentc_ext_shutdown, after the rows are dropped from the
 * provider registry. A retired row is skipped by name/api lookups but still
 * resolves through agentc_provider_ops(handle), so an in-flight agent gets a
 * clean -ENOSYS instead of a dangling callback.
 */
#define AGENTC_EXT_MAX_PROVIDERS    64
#define AGENTC_EXT_MAX_MODELS       256
#define AGENTC_EXT_MODEL_ID_MAX     256
#define AGENTC_EXT_AUTH_PREFIX_MAX  256
#define AGENTC_EXT_BASE_URL_MAX     1024
#define AGENTC_EXT_KEY_MAX          128

/* The extension provider struct carries all strings and arrays; the host copy
 * owns them here. `ext` is the view handed back to the extension callbacks. */
typedef struct {
    bool alive;
    u64 owner;
    AgcProviderOps ops;
    const AgcProvider *handle;
    AgcExtProvider ext;
    AgcExtProviderAuth auth;
    bool has_auth;
    u32 auth_kind;
    char *name, *label, *base_url, *path;
    char *env_keys[3];
    char *auth_header, *auth_prefix;
    AgcExtProviderModel *models;   /* copied array; id/name owned strings */
    size_t nmodels;
} ProviderRec;

/* Pointer vector: the records handed to the registry must not move. */
static AgcVec g_providers;

/* Per-stream adapter state (st->priv). The extension's AgcExtStream.ud is
 * extension-owned; the host only carries the pointer. */
typedef struct {
    ProviderRec *rec;
    AgcStreamState *st;
    AgcExtStream ext;
} ExtStreamRec;

/* Current stream while an extension callback runs. The sink trampolines only
 * receive AgcExtStream*, so the static sink struct below reads this context;
 * every entry point saves/restores it for re-entrancy. */
static ExtStreamRec *g_sink_ctx;

/* ------------------------------------------------------------ validation */

static bool prov_path_ok(const char *s) {
    if (!s || s[0] != '/') return false;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') return false;
        if (i >= 1023) return false;
    }
    return true;
}

static bool prov_url_ok(const char *s) {
    if (!s || !s[0]) return false;
    size_t n = agentc_strlen(s);
    if (!agentc_str_starts(s, n, "http://") &&
        !agentc_str_starts(s, n, "https://"))
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\r' || s[i] == '\n' || s[i] == ' ') return false;
    AgcUrl u;
    return agentc_url_parse(s, &u) == 0;
}

static bool prov_prefix_ok(const char *s) {
    if (!s) return true;
    size_t n = agentc_strlen(s);
    if (n > AGENTC_EXT_AUTH_PREFIX_MAX) return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\r' || s[i] == '\n') return false;
    return true;
}

static bool prov_model_ok(const AgcExtProviderModel *m) {
    if (!m || !AGENTC_EXT_FIELD_OK(m, AgcExtProviderModel, image)) return false;
    if (!m->id || !m->id[0]) return false;
    if (agentc_strlen(m->id) > AGENTC_EXT_MODEL_ID_MAX) return false;
    if (m->name && agentc_strlen(m->name) > AGENTC_EXT_MODEL_ID_MAX) return false;
    for (const char *s = m->id; *s; s++)
        if (*s == '\r' || *s == '\n') return false;
    for (const char *s = m->name; s && *s; s++)
        if (*s == '\r' || *s == '\n') return false;
    return true;
}

/* ------------------------------------------------------- record lifetime */

static void provider_rec_retire(ProviderRec *r) {
    if (!r) return;
    r->alive = false;
    r->ops.retired = true;
}

static void provider_rec_free(ProviderRec *r) {
    if (!r) return;
    /* The static model rows are part of this record's contribution. */
    agentc_model_clear_static(r->name);
    for (size_t i = 0; i < r->nmodels; i++) {
        agentc_free((char *)r->models[i].id);
        agentc_free((char *)r->models[i].name);
    }
    agentc_free(r->models);
    agentc_free(r->name);
    agentc_free(r->label);
    agentc_free(r->base_url);
    agentc_free(r->path);
    for (size_t i = 0; i < 3; i++) agentc_free(r->env_keys[i]);
    agentc_free(r->auth_header);
    agentc_free(r->auth_prefix);
    agentc_free((void *)r->handle);
    agentc_free(r);
}

/* ----------------------------------------------------------- auth helpers */

static const char *ext_auth_name(const ProviderRec *r) {
    if (!r) return NULL;
    if (r->auth_kind != AGENTC_EXT_AUTH_BEARER &&
        r->auth_kind != AGENTC_EXT_AUTH_HEADER)
        return NULL;
    return r->auth_header;
}

/* Copy a header field name/value onto the wire, dropping control bytes. A
 * CR/LF in any of these would forge an extra discovery/auth header line; the
 * builtin discovery hooks (google/anthropic) sanitize identically. */
static void ext_auth_append_sanitized(AgcBuf *out, const char *v) {
    for (const char *p = v; p && *p; p++) {
        u8 c = (u8)*p;
        if (c < 0x20 || c == 0x7f) continue;
        agentc_buf_byte(out, c);
    }
}

/* Append the core-owned auth line for the resolved key. The key is never part
 * of the view; this is the only place it reaches the wire. */
static void ext_auth_append(AgcBuf *out, const ProviderRec *r, const char *key) {
    if (!out || !r || !key || !key[0]) return;
    const char *name = ext_auth_name(r);
    if (!name) return;
    ext_auth_append_sanitized(out, name);
    agentc_buf_cstr(out, ": ");
    if (r->auth_prefix) ext_auth_append_sanitized(out, r->auth_prefix);
    ext_auth_append_sanitized(out, key);
    agentc_buf_cstr(out, "\r\n");
}

/* Internal discovery hook (discover.c calls this per row). */
static void ext_provider_auth_headers(const AgcProviderOps *self, AgcBuf *out,
                                      const char *api_key) {
    if (!out) return;
    ProviderRec *rec =
        (self && self->is_ext && !self->retired) ? (ProviderRec *)self->ext_rec : NULL;
    if (!rec || !rec->alive) return;
    ext_auth_append(out, rec, api_key);
}

/* ------------------------------------------------------- request adapter */

static bool ext_msg_include(const AgcMsg *m) {
    if (!m) return false;
    if (m->role != AGENTC_ROLE_ASSISTANT) return true;
    if (!agentc_provider_msg_serializable(m)) return false;
    for (size_t i = 0; i < m->nblocks; i++) {
        if (m->blocks[i].type == AGENTC_BLK_TOOLCALL) return true;
        if (m->blocks[i].type == AGENTC_BLK_TEXT && m->blocks[i].text_len) return true;
    }
    return false;
}

#define EXT_VIEW_ALIGN(n) (((n) + sizeof(void *) - 1) & ~(sizeof(void *) - 1))

/* Sanitize the extension head block into CRLF lines: CR/LF become spaces in
 * values, a line without a ':' or with a non-token key is dropped, the
 * forbidden trio is dropped, and every occurrence of the declared auth header
 * is dropped so the core's line is the only one. */
static void ext_head_sanitize(AgcBuf *dst, const char *head, size_t head_len,
                              const char *skip_name) {
    if (!head) return;
    size_t i = 0;
    while (i < head_len) {
        size_t start = i;
        while (i < head_len && head[i] != '\n') i++;
        size_t end = i;                    /* one past the line's last byte */
        if (i < head_len) i++;             /* skip the newline */
        if (end > start && head[end - 1] == '\r') end--;
        size_t kn = 0;
        while (start + kn < end && head[start + kn] != ':') kn++;
        if (kn == 0 || start + kn >= end) continue;
        size_t ke = kn;
        while (ke > 0 && (head[start + ke - 1] == ' ' || head[start + ke - 1] == '\t'))
            ke--;
        if (!hdr_key_ok((const u8 *)head + start, ke)) continue;
        if (hdr_forbidden(head + start, ke)) continue;
        if (skip_name &&
            agentc_str_ieq(head + start, ke, skip_name, agentc_strlen(skip_name)))
            continue;
        agentc_buf_push(dst, head + start, ke);
        agentc_buf_cstr(dst, ": ");
        size_t v = start + kn + 1;
        while (v < end && (head[v] == ' ' || head[v] == '\t')) v++;
        for (; v < end; v++) {
            char c = head[v];
            if (c == '\r' || c == '\n') c = ' ';
            agentc_buf_byte(dst, (u8)c);
        }
        agentc_buf_cstr(dst, "\r\n");
    }
}

static int ext_provider_build_request(AgcBuf *out, const AgcRequest *r,
                                      const char *url_host, const char *url_path) {
    (void)url_host;
    (void)url_path;
    if (!out) return -22;
    ProviderRec *rec = NULL;
    if (r && r->provider) {
        const AgcProviderOps *ops = agentc_provider_by_name(r->provider);
        if (ops && ops->is_ext && !ops->retired) rec = (ProviderRec *)ops->ext_rec;
    }
    if (!rec || !rec->alive) {
        agentc_logf(2, "ext: provider unloaded, request refused");
        return -38;                                /* ENOSYS */
    }

    /* Count first so the whole view is one allocation. */
    const AgcTranscript *t = r->transcript;
    size_t nm = 0, nb = 0;
    if (t) {
        for (size_t i = 0; i < t->n; i++) {
            const AgcMsg *m = &t->msgs[i];
            if (!ext_msg_include(m)) continue;
            nm++;
            nb += m->nblocks;
        }
    }
    size_t nt = agentc_request_visible_tools(r);

    size_t off_msgs = EXT_VIEW_ALIGN(sizeof(AgcExtRequestView));
    size_t off_blocks = off_msgs + EXT_VIEW_ALIGN(nm * sizeof(AgcExtMessageView));
    size_t off_tools = off_blocks + EXT_VIEW_ALIGN(nb * sizeof(AgcExtBlockView));
    size_t total = off_tools + EXT_VIEW_ALIGN(nt * sizeof(AgcExtToolView));
    u8 *mem = agentc_alloc(total ? total : 1);
    AgcExtRequestView *view = (AgcExtRequestView *)mem;
    AgcExtMessageView *msgs = (AgcExtMessageView *)(mem + off_msgs);
    AgcExtBlockView *blocks = (AgcExtBlockView *)(mem + off_blocks);
    AgcExtToolView *tools = (AgcExtToolView *)(mem + off_tools);

    view->struct_size = sizeof *view;
    view->provider = r->provider;
    view->model = r->model;
    view->system = r->system;
    view->thinking_level = r->thinking_level;
    view->max_tokens = r->max_tokens;
    view->messages = nm ? msgs : NULL;
    view->nmessages = nm;
    view->tools = nt ? tools : NULL;
    view->ntools = nt;

    size_t mi = 0, bi = 0;
    if (t) {
        for (size_t i = 0; i < t->n; i++) {
            const AgcMsg *m = &t->msgs[i];
            if (!ext_msg_include(m)) continue;
            AgcExtMessageView *mv = &msgs[mi++];
            mv->struct_size = sizeof *mv;
            mv->role = (uint32_t)m->role;
            mv->stop_reason = m->stop_reason;
            mv->blocks = m->nblocks ? &blocks[bi] : NULL;
            mv->nblocks = m->nblocks;
            for (size_t j = 0; j < m->nblocks; j++) {
                const AgcBlock *b = &m->blocks[j];
                AgcExtBlockView *bv = &blocks[bi++];
                bv->struct_size = sizeof *bv;
                bv->type = (uint32_t)b->type;
                if (b->type == AGENTC_BLK_TEXT || b->type == AGENTC_BLK_THINK) {
                    bv->text = b->text;
                    bv->text_len = b->text_len;
                } else if (b->type == AGENTC_BLK_TOOLCALL) {
                    bv->tool_id = b->tool_id;
                    bv->tool_name = b->tool_name;
                    bv->tool_args = b->tool_args;
                }
            }
        }
    }
    size_t ti = 0;
    for (size_t i = 0; r->tools && i < r->ntools; i++) {
        const AgcTool *tool = &r->tools[i];
        if (tool->flags & AGENTC_TOOL_HIDDEN) continue;
        AgcExtToolView *tv = &tools[ti++];
        tv->struct_size = sizeof *tv;
        tv->name = tool->name;
        tv->description = tool->desc;
        tv->parameters_json = (tool->params_json && tool->params_json[0])
                                  ? tool->params_json
                                  : "{\"type\":\"object\"}";
        tv->flags = tool->flags;
    }

    AgcBuf head = { 0 }, body = { 0 };
    u64 saved_owner = owner_get();
    owner_set(rec->owner);
    int rc = rec->ext.build_request(agentc_ext_host(), &rec->ext, view, &head, &body);
    owner_set(saved_owner);
    agentc_free(mem);
    if (rc != 0) {
        agentc_buf_free(&head);
        agentc_buf_free(&body);
        return rc;
    }

    const char *auth_name = ext_auth_name(rec);
    AgcBuf clean = { 0 };
    ext_head_sanitize(&clean, (const char *)head.p, head.len, auth_name);
    if (auth_name) ext_auth_append(&clean, rec, r->api_key);
    /* The core splits the request at the first "\r\n\r\n": emit the
     * sanitized lines, the blank line, then the body. With no lines at all the
     * blank line itself is the separator. */
    if (clean.len == 0) agentc_buf_cstr(out, "\r\n");
    if (clean.len) agentc_buf_push(out, clean.p, clean.len);
    agentc_buf_cstr(out, "\r\n");
    if (body.len) agentc_buf_push(out, body.p, body.len);
    agentc_buf_free(&clean);
    agentc_buf_free(&head);
    agentc_buf_free(&body);
    return 0;
}

/* -------------------------------------------------------- stream adapter */

static ExtStreamRec *ext_stream_rec(AgcStreamState *st) {
    return st ? (ExtStreamRec *)st->priv : NULL;
}

static bool provider_rec_dead(const ProviderRec *r) {
    return !r || !r->alive || r->ops.retired;
}

static int provider_dead_error(AgcStreamState *st) {
    if (st) {
        if (st->stop_reason == AGENTC_STOP_PENDING) st->stop_reason = AGENTC_STOP_ERROR;
        if (!st->error[0])
            agentc_snprintf(st->error, sizeof st->error, "provider unloaded");
    }
    return -1;
}

static AgcMsg *sink_msg(void) {
    return (g_sink_ctx && g_sink_ctx->st) ? g_sink_ctx->st->msg : NULL;
}

static AgcStreamState *sink_state(void) {
    return g_sink_ctx ? g_sink_ctx->st : NULL;
}

static void sink_text(AgcExtStream *st, const char *p, size_t n) {
    (void)st;
    AgcMsg *m = sink_msg();
    if (!m || !p || !n) return;
    AgcBlock *b = (m->nblocks && m->blocks[m->nblocks - 1].type == AGENTC_BLK_TEXT)
                      ? &m->blocks[m->nblocks - 1]
                      : agentc_msg_block_new(m, AGENTC_BLK_TEXT);
    agentc_msg_block_append(b, p, n);
}

static void sink_thinking(AgcExtStream *st, const char *p, size_t n) {
    (void)st;
    AgcMsg *m = sink_msg();
    if (!m || !p || !n) return;
    AgcBlock *b = (m->nblocks && m->blocks[m->nblocks - 1].type == AGENTC_BLK_THINK)
                      ? &m->blocks[m->nblocks - 1]
                      : agentc_msg_block_new(m, AGENTC_BLK_THINK);
    agentc_msg_block_append(b, p, n);
}

static void sink_tool_start(AgcExtStream *st, const char *id, const char *name) {
    (void)st;
    AgcMsg *m = sink_msg();
    if (!m) return;
    AgcBlock *b = agentc_msg_block_new(m, AGENTC_BLK_TOOLCALL);
    b->tool_id = agentc_strdup(id ? id : "");
    b->tool_name = agentc_strdup(name ? name : "");
}

static void sink_tool_args(AgcExtStream *st, const char *p, size_t n) {
    (void)st;
    AgcMsg *m = sink_msg();
    if (!m || !p || !n) return;
    AgcBlock *b = NULL;
    for (size_t i = m->nblocks; i > 0; i--) {
        if (m->blocks[i - 1].type == AGENTC_BLK_TOOLCALL) {
            b = &m->blocks[i - 1];
            break;
        }
    }
    if (!b) b = agentc_msg_block_new(m, AGENTC_BLK_TOOLCALL);
    agentc_msg_block_append(b, p, n);
}

static void sink_response_id(AgcExtStream *st, const char *id) {
    (void)st;
    AgcStreamState *s = sink_state();
    if (!s || !id) return;
    agentc_snprintf(s->response_id, sizeof s->response_id, "%s", id);
}

static void sink_usage(AgcExtStream *st, const AgcExtUsage *u) {
    (void)st;
    AgcStreamState *s = sink_state();
    /* The usage struct is handled as a whole and its last field is
     * `reasoning`; gate the full struct so a caller with a smaller
     * struct_size never has `fields` or any counter read out of bounds. */
    if (!s || !AGENTC_EXT_FIELD_OK(u, AgcExtUsage, reasoning)) return;
    if (u->fields & AGENTC_EXT_USAGE_INPUT) s->usage_input = u->input;
    if (u->fields & AGENTC_EXT_USAGE_OUTPUT) s->usage_output = u->output;
    if (u->fields & AGENTC_EXT_USAGE_CACHE_READ) s->usage_cache_read = u->cache_read;
    if (u->fields & AGENTC_EXT_USAGE_CACHE_WRITE) s->usage_cache_write = u->cache_write;
    if (u->fields & AGENTC_EXT_USAGE_REASONING) s->usage_reasoning = u->reasoning;
}

static void sink_stop(AgcExtStream *st, int reason) {
    (void)st;
    AgcStreamState *s = sink_state();
    if (!s) return;
    if (reason < AGENTC_EXT_STOP_PENDING || reason > AGENTC_EXT_STOP_ABORTED)
        reason = AGENTC_EXT_STOP_ERROR;
    s->stop_reason = reason;
    s->saw_stop = true;
}

static void sink_error(AgcExtStream *st, const char *message) {
    (void)st;
    AgcStreamState *s = sink_state();
    if (!s) return;
    if (message && message[0])
        agentc_snprintf(s->error, sizeof s->error, "%s", message);
    else if (!s->error[0])
        agentc_snprintf(s->error, sizeof s->error, "provider error");
    s->stop_reason = AGENTC_STOP_ERROR;
    s->saw_stop = true;
}

static const AgcExtStreamSink g_ext_sink = {
    .struct_size = sizeof(AgcExtStreamSink),
    .text = sink_text,
    .thinking = sink_thinking,
    .tool_start = sink_tool_start,
    .tool_args = sink_tool_args,
    .response_id = sink_response_id,
    .usage = sink_usage,
    .stop = sink_stop,
    .error = sink_error,
};

static int ext_provider_stream_open(AgcStreamState *st) {
    /* provider.c tracks the row before calling this, so the row is available
     * even though stream_open does not receive it. */
    const AgcProviderOps *ops = agentc_provider_open_ops(st);
    ProviderRec *rec = (ops && ops->is_ext) ? (ProviderRec *)ops->ext_rec : NULL;
    if (provider_rec_dead(rec)) return -38;            /* ENOSYS */
    ExtStreamRec *es = agentc_alloc(sizeof *es);
    es->rec = rec;
    es->st = st;
    es->ext.struct_size = sizeof es->ext;
    st->priv = es;
    if (rec->ext.stream_open) {
        u64 saved_owner = owner_get();
        ExtStreamRec *saved_ctx = g_sink_ctx;
        owner_set(rec->owner);
        g_sink_ctx = es;
        int rc = rec->ext.stream_open(agentc_ext_host(), &rec->ext, &es->ext);
        g_sink_ctx = saved_ctx;
        owner_set(saved_owner);
        if (rc != 0) {
            st->priv = NULL;
            agentc_free(es);
            return rc;
        }
    }
    return 0;
}

static void ext_provider_stream_close(AgcStreamState *st) {
    ExtStreamRec *es = ext_stream_rec(st);
    if (!es) return;
    /* Never touch extension ud after retirement (the extension's shutdown owns
     * any outstanding per-stream state at that point). */
    if (!provider_rec_dead(es->rec) && es->rec->ext.stream_close) {
        u64 saved_owner = owner_get();
        ExtStreamRec *saved_ctx = g_sink_ctx;
        owner_set(es->rec->owner);
        g_sink_ctx = es;
        es->rec->ext.stream_close(agentc_ext_host(), &es->rec->ext, &es->ext);
        g_sink_ctx = saved_ctx;
        owner_set(saved_owner);
    }
    agentc_free(es);
    st->priv = NULL;
}

static int ext_provider_map_sse(AgcStreamState *st, const AgcSseEvent *ev) {
    ExtStreamRec *es = ext_stream_rec(st);
    if (!es || provider_rec_dead(es->rec)) return provider_dead_error(st);
    if (!ev) return -1;
    AgcExtWireEvent we;
    agentc_memset(&we, 0, sizeof we);
    we.struct_size = sizeof we;
    we.event = ev->event ? ev->event : "";
    we.data = ev->data;
    we.data_len = ev->data_len;
    u64 saved_owner = owner_get();
    ExtStreamRec *saved_ctx = g_sink_ctx;
    owner_set(es->rec->owner);
    g_sink_ctx = es;
    int rc = es->rec->ext.stream_event(agentc_ext_host(), &es->rec->ext, &es->ext, &we,
                                       &g_ext_sink);
    g_sink_ctx = saved_ctx;
    owner_set(saved_owner);
    if (rc != 0) {
        if (st->stop_reason == AGENTC_STOP_PENDING) st->stop_reason = AGENTC_STOP_ERROR;
        if (!st->error[0])
            agentc_snprintf(st->error, sizeof st->error, "provider stream error");
        return -1;
    }
    return 0;
}

static int ext_provider_finish(AgcStreamState *st) {
    ExtStreamRec *es = ext_stream_rec(st);
    if (!es || provider_rec_dead(es->rec)) return provider_dead_error(st);
    if (es->rec->ext.stream_finish) {
        u64 saved_owner = owner_get();
        ExtStreamRec *saved_ctx = g_sink_ctx;
        owner_set(es->rec->owner);
        g_sink_ctx = es;
        int rc = es->rec->ext.stream_finish(agentc_ext_host(), &es->rec->ext, &es->ext,
                                            &g_ext_sink);
        g_sink_ctx = saved_ctx;
        owner_set(saved_owner);
        if (rc != 0) {
            if (!st->error[0])
                agentc_snprintf(st->error, sizeof st->error, "provider finish error");
            st->stop_reason = AGENTC_STOP_ERROR;
            return -1;
        }
    }
    /* Built-in resolve/validate: a stream that never signalled a stop is an
     * error; otherwise a pending stop resolves from the assembled message. */
    if (!st->saw_stop && st->stop_reason == AGENTC_STOP_PENDING) {
        agentc_snprintf(st->error, sizeof st->error, "stream ended before finish");
        st->stop_reason = AGENTC_STOP_ERROR;
        return -1;
    }
    if (st->stop_reason == AGENTC_STOP_PENDING)
        st->stop_reason = agentc_msg_count_tool_calls(st->msg) ? AGENTC_STOP_TOOLUSE
                                                              : AGENTC_STOP_STOP;
    return st->stop_reason == AGENTC_STOP_ERROR ? -1 : 0;
}

/* ------------------------------------------------------- host registration */

static void host_add_provider(const AgcExtProvider *p) {
    if (!p || p->struct_size <
                  (uint32_t)(offsetof(AgcExtProvider, stream_close) +
                             sizeof p->stream_close)) {
        agentc_logf(3, "ext: add_provider: struct too small");
        return;
    }
    if (!ext_valid_name(p->name)) {
        agentc_logf(3, "ext: add_provider: invalid provider name");
        return;
    }
    if (!prov_path_ok(p->path)) {
        agentc_logf(3, "ext: add_provider: invalid path");
        return;
    }
    if (p->default_base_url && agentc_strlen(p->default_base_url) > AGENTC_EXT_BASE_URL_MAX) {
        agentc_logf(3, "ext: add_provider: base url too long");
        return;
    }
    if (!prov_url_ok(p->default_base_url)) {
        agentc_logf(3, "ext: add_provider: invalid base url");
        return;
    }
    for (size_t i = 0; i < 3; i++) {
        if (p->env_keys[i] && agentc_strlen(p->env_keys[i]) > AGENTC_EXT_KEY_MAX) {
            agentc_logf(3, "ext: add_provider: env key too long");
            return;
        }
    }
    if (!p->build_request || !p->stream_event) {
        agentc_logf(3, "ext: add_provider: missing build_request/stream_event");
        return;
    }
    /* The public ABI range (AGENTC_EXT_DISCOVER_*) is one value shorter than the
     * internal enum: public NONE (=3) collides with the internal GOOGLE (=3),
     * so an extension declaring NONE must be translated to the internal NONE
     * rather than stored verbatim (which would silently probe as Google). */
    if (p->discover_style < AGENTC_EXT_DISCOVER_DEFAULT ||
        p->discover_style > AGENTC_EXT_DISCOVER_NONE) {
        agentc_logf(3, "ext: add_provider: invalid discover style");
        return;
    }
    int discover_style = p->discover_style == AGENTC_EXT_DISCOVER_NONE
                             ? AGENTC_DISCOVER_NONE
                             : p->discover_style;
    if (p->nmodels > AGENTC_EXT_MAX_MODELS) {
        agentc_logf(3, "ext: add_provider: too many models");
        return;
    }
    for (size_t i = 0; i < p->nmodels; i++) {
        if (!p->models || !prov_model_ok(&p->models[i])) {
            agentc_logf(3, "ext: add_provider: invalid model");
            return;
        }
    }
    const AgcExtProviderAuth *a = p->auth;
    u32 auth_kind = AGENTC_EXT_AUTH_NONE;
    if (a) {
        if (!AGENTC_EXT_FIELD_OK(a, AgcExtProviderAuth, prefix)) {
            agentc_logf(3, "ext: add_provider: auth struct too small");
            return;
        }
        auth_kind = a->kind;
        if (auth_kind == AGENTC_EXT_AUTH_QUERY) {
            agentc_logf(3, "ext: add_provider: query auth reserved");
            return;
        }
        if (auth_kind > AGENTC_EXT_AUTH_QUERY) {
            agentc_logf(3, "ext: add_provider: invalid auth kind");
            return;
        }
        if (auth_kind == AGENTC_EXT_AUTH_BEARER ||
            auth_kind == AGENTC_EXT_AUTH_HEADER) {
            const char *h = a->header;
            if ((!h || !h[0]) && auth_kind == AGENTC_EXT_AUTH_BEARER)
                h = "Authorization";
            if (h && agentc_strlen(h) > AGENTC_EXT_KEY_MAX) {
                agentc_logf(3, "ext: add_provider: auth header too long");
                return;
            }
            if (!h || !hdr_key_ok((const u8 *)h, agentc_strlen(h))) {
                agentc_logf(3, "ext: add_provider: invalid auth header");
                return;
            }
        }
        if (!prov_prefix_ok(a->prefix)) {
            agentc_logf(3, "ext: add_provider: invalid auth prefix");
            return;
        }
    }
    /* Duplicate against builtins, materialized presets and live extension rows
     * (agentc_provider_by_name skips retired rows). */
    if (agentc_provider_by_name(p->name)) {
        agentc_logf(2, "ext: add_provider: duplicate provider %s ignored", p->name);
        return;
    }
    if (g_providers.len >= AGENTC_EXT_MAX_PROVIDERS) {
        agentc_logf(2, "ext: add_provider: provider cap reached");
        return;
    }

    ProviderRec *r = agentc_alloc(sizeof *r);
    r->alive = true;
    r->owner = owner_get();
    r->name = agentc_strdup(p->name);
    r->label = (p->label && p->label[0]) ? agentc_strdup(p->label)
                                         : agentc_strdup(p->name);
    r->base_url = agentc_strdup(p->default_base_url);
    r->path = agentc_strdup(p->path);
    for (size_t i = 0; i < 3; i++)
        r->env_keys[i] = (p->env_keys[i] && p->env_keys[i][0])
                             ? agentc_strdup(p->env_keys[i])
                             : NULL;
    if (a) {
        r->has_auth = true;
        r->auth_kind = auth_kind;
        r->auth_header = (a->header && a->header[0]) ? agentc_strdup(a->header) : NULL;
        if (!r->auth_header && auth_kind == AGENTC_EXT_AUTH_BEARER)
            r->auth_header = agentc_strdup("Authorization");
        r->auth_prefix = a->prefix ? agentc_strdup(a->prefix) : NULL;
        if (!r->auth_prefix && auth_kind == AGENTC_EXT_AUTH_BEARER)
            r->auth_prefix = agentc_strdup("Bearer ");
    }
    if (p->nmodels) {
        r->models = agentc_alloc(p->nmodels * sizeof *r->models);
        r->nmodels = p->nmodels;
        for (size_t i = 0; i < p->nmodels; i++) {
            r->models[i] = p->models[i];
            r->models[i].struct_size = sizeof r->models[i];
            r->models[i].id = agentc_strdup(p->models[i].id);
            r->models[i].name = p->models[i].name ? agentc_strdup(p->models[i].name)
                                                  : NULL;
        }
    }
    r->auth.struct_size = sizeof r->auth;
    r->auth.kind = auth_kind;
    r->auth.header = r->auth_header;
    r->auth.prefix = r->auth_prefix;
    abi_copy(&r->ext, p, sizeof r->ext, p->struct_size);
    r->ext.struct_size = sizeof r->ext;
    r->ext.name = r->name;
    r->ext.label = r->label;
    r->ext.default_base_url = r->base_url;
    r->ext.path = r->path;
    r->ext.env_keys[0] = r->env_keys[0];
    r->ext.env_keys[1] = r->env_keys[1];
    r->ext.env_keys[2] = r->env_keys[2];
    r->ext.auth = r->has_auth ? &r->auth : NULL;
    r->ext.models = r->models;
    r->ext.nmodels = r->nmodels;
    r->ops.name = r->name;
    r->ops.api = r->name;                 /* custom rows use api = name */
    r->ops.path = r->path;
    r->ops.default_base_url = r->base_url;
    r->ops.env_keys[0] = r->env_keys[0];
    r->ops.env_keys[1] = r->env_keys[1];
    r->ops.env_keys[2] = r->env_keys[2];
    r->ops.needs_key = p->needs_key;
    r->ops.discover_style = discover_style;
    r->ops.max_tokens_key = NULL;         /* the extension writes its own body */
    r->ops.build_request = ext_provider_build_request;
    r->ops.map_sse = ext_provider_map_sse;
    r->ops.finish = ext_provider_finish;
    r->ops.stream_open = ext_provider_stream_open;
    r->ops.stream_close = ext_provider_stream_close;
    r->ops.handle = NULL;
    r->ops.is_ext = 1;
    r->ops.ext_rec = r;
    r->ops.retired = false;
    r->ops.auth_headers = ext_provider_auth_headers;
    if (agentc_provider_register(&r->ops) < 0) {
        agentc_logf(3, "ext: add_provider: registration failed");
        provider_rec_free(r);
        return;
    }
    ProviderRec **slot = agentc_vec_push(&g_providers, sizeof *slot);
    *slot = r;
    r->handle = agentc_provider_handle(&r->ops);
    for (size_t i = 0; i < r->nmodels; i++)
        agentc_model_register_static(r->name, r->models[i].id, r->name, r->base_url,
                                     r->models[i].ctx_window, r->models[i].max_tokens,
                                     r->models[i].reasoning != 0,
                                     r->models[i].image != 0);
}

/* ------------------------------------------------------ typed event bridge */

static int g_events_sub;

static void ext_typed_event(void *ud, int ev, const void *data) {
    (void)ud;
    agentc_ext_emit_agent_event(ev, data);
}

static void typed_sub_ensure(void) {
    if (g_events_sub == 0) g_events_sub = agentc_events_subscribe(ext_typed_event, NULL);
}

/* ------------------------------------------------------------ lifecycle */

void agentc_ext_register_defaults(bool no_mcp) {
    const AgcExt *defs[3] = { agentc_builtin_tools_ext(), agentc_builtin_context_ext(), NULL };
    if (!no_mcp) defs[2] = agentc_mcp_ext();
    for (size_t i = 0; i < 3; i++)
        if (defs[i]) agentc_ext_register(defs[i]);
}

void agentc_ext_apply_config(const AgcConfig *cfg) {
    if (!cfg) return;
    /* By name, default and linked alike: the app calls this after
     * agentc_ext_register_linked() and before load_all, so nothing disabled
     * here can initialize. A name that matches no registered extension is
     * logged once and kept as an unknown-disabled placeholder so the startup
     * summary can report it (a typo should not silently do nothing). */
    for (size_t k = 0; k < cfg->nextensions_disabled; k++) {
        const char *name = cfg->extensions_disabled[k];
        if (!name || !name[0]) continue;
        bool duplicate = false;
        for (size_t j = 0; j < k; j++) {
            const char *prev = cfg->extensions_disabled[j];
            if (prev && agentc_streq(prev, name)) { duplicate = true; break; }
        }
        if (duplicate) continue;
        ExtRec *e = ext_find(name);
        if (e) {
            if (!e->disabled) {
                agentc_logf(1, "ext: %s disabled by config", e->name);
                e->disabled = true;
                e->state = AGENTC_EXT_STATE_SKIPPED;
            }
            continue;
        }
        agentc_logf(2, "ext: disabled name %s matched nothing", name);
        size_t nlen = agentc_strlen(name);
        if (nlen > 64) nlen = 64;   /* bounded record; longer names can never match */
        e = agentc_vec_push(&g_exts, sizeof *e);
        e->used = 1;
        e->state = AGENTC_EXT_STATE_SKIPPED;
        e->disabled = true;
        e->known = false;
        e->name = agentc_strdup_len(name, nlen);
        e->owner = ++g_ext_seq;   /* never owner 0: unload must not touch core */
    }
}

size_t agentc_ext_describe(AgcExtInfo *out, size_t max) {
    size_t total = 0, wrote = 0;
    for (size_t i = 0; i < g_exts.len; i++) {
        ExtRec *e = &((ExtRec *)g_exts.p)[i];
        if (!e->used) continue;
        total++;
        if (!out || wrote >= max) continue;
        AgcExtInfo *d = &out[wrote++];
        d->name = e->name;
        d->version = e->version;
        d->order = e->order;
        d->state = e->state;
        d->disabled = e->disabled;
        d->known = e->known;
        d->dynamic = e->dynamic;
    }
    return out ? wrote : total;
}

bool agentc_ext_is_loaded(const char *name) {
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < g_exts.len; i++) {
        ExtRec *e = &((ExtRec *)g_exts.p)[i];
        if (e->used && e->state == AGENTC_EXT_STATE_LOADED && agentc_streq(e->name, name))
            return true;
    }
    return false;
}

int agentc_ext_adopt(const char *registry_name, int (*entry)(const AgcExtHost *, AgcExt *)) {
    const char *name = registry_name ? registry_name : "?";
    if (!entry) {
        agentc_logf(0, "ext: %s not linked", name);
        return 0;
    }
    ExtRec *existing = ext_find(name);
    if (existing && !(existing->disabled && !existing->known)) {
        agentc_logf(2, "ext: duplicate extension %s ignored", name);
        return 0;
    }
    /* The entry phase is owner-tagged so anything it registers is
     * attributed to this extension and rolled back if the descriptor is
     * refused. The entry must be descriptor-only; the owner id allocated here
     * is the same one load_all() later runs init() under. */
    u64 owner = ++g_ext_seq;
    AgcExt out;
    agentc_memset(&out, 0, sizeof out);
    u64 saved = owner_get();
    owner_set(owner);
    int rc = entry(agentc_ext_host(), &out);
    owner_set(saved);
    if (rc != 0) {
        agentc_logf(3, "ext: %s entry returned an error", name);
        owner_remove(owner);
        return -22;   /* EINVAL */
    }
    if (!ext_desc_valid(&out, name)) {
        owner_remove(owner);
        return -22;   /* EINVAL */
    }
    AgcExt reg = out;
    reg.name = name;
    if (out.struct_size < (uint32_t)(offsetof(AgcExt, order) + sizeof out.order))
        reg.order = 0;
    if (out.struct_size < (uint32_t)(offsetof(AgcExt, shutdown) + sizeof out.shutdown))
        reg.shutdown = NULL;
    if (!ext_register_impl(&reg, name, owner, false)) {
        owner_remove(owner);
        return -22;   /* EINVAL */
    }
    return 0;
}

static void owner_remove(u64 owner) {
    bool removed_tool = false;
    for (size_t i = 0; i < g_tools.len; i++) {
        ToolRec *t = tool_at(i);
        if (!t || t->owner != owner) continue;
        tool_remove_at(i);
        removed_tool = true;
    }
    /* A removed tool changes the collected table, so the registry must reach
     * the next recompose even without a fresh add. */
    if (removed_tool) g_dirty = true;
    for (size_t i = 0; i < g_cmds.len; i++) {
        CmdRec *c = &((CmdRec *)g_cmds.p)[i];
        if (c->owner != owner) continue;
        agentc_free(c->name);
        agentc_free(c->desc);
        agentc_memset(c, 0, sizeof *c);
    }
    for (size_t i = 0; i < g_sections.len; i++) {
        SecRec *s = &((SecRec *)g_sections.p)[i];
        if (s->owner != owner) continue;
        agentc_free(s->key);
        agentc_memset(s, 0, sizeof *s);
    }
    for (size_t i = 0; i < AGENTC_EXT_MAX_HOOKS; i++) {
        if (g_hooks[i].used && g_hooks[i].owner == owner) {
            agentc_free(g_hooks[i].point);
            agentc_memset(&g_hooks[i], 0, sizeof g_hooks[i]);
            if (g_hooks_live) g_hooks_live--;
        }
    }
    for (size_t i = 0; i < g_status_regs.len; i++) {
        StatusReg *r = ((StatusReg **)g_status_regs.p)[i];
        if (!r || r->owner != owner) continue;
        agentc_status_remove(host_status_call, r);
        agentc_free(r);
        ((StatusReg **)g_status_regs.p)[i] = NULL;
    }
    set_status_owner_remove(owner);
    defer_remove_owner(owner);
    for (size_t i = 0; i < AGENTC_EXT_MAX_HTTP; i++) {
        if (g_http[i].used && g_http[i].owner == owner) http_release(&g_http[i]);
    }
    for (size_t i = 0; i < g_pumps.len; i++) {
        PumpRec *p = &((PumpRec *)g_pumps.p)[i];
        if (p->owner == owner) agentc_memset(p, 0, sizeof *p);
    }
    /* Retire the owner's provider rows. The records stay allocated until
     * agentc_ext_shutdown, so a held handle still resolves to a dead row. */
    for (size_t i = 0; i < g_providers.len; i++) {
        ProviderRec *p = ((ProviderRec **)g_providers.p)[i];
        if (p && p->owner == owner) provider_rec_retire(p);
    }
}

void agentc_ext_load_all(void) {
    /* g_loading covers every round of this call and any nested load_all() an
     * init() starts: the outermost return is the only one that publishes the
     * registry as safe to tear down, so shutdown() from inside an init() is a
     * logged no-op (it would otherwise free g_exts under the running loop). */
    g_loading++;
    /* Bounded rounds: init() may register another extension, which lands after
     * the current tail. Each round initializes every still-pending record in
     * `order` order (stable); after AGENTC_EXT_LOAD_ROUNDS the cap is logged
     * and anything still pending stays pending. g_exts may reallocate inside
     * init(), so a round holds indices, never ExtRec pointers. */
    for (int round = 0; round < AGENTC_EXT_LOAD_ROUNDS; round++) {
        size_t n = g_exts.len;
        size_t *list = agentc_alloc((n ? n : 1) * sizeof *list);
        for (size_t i = 0; i < n; i++) list[i] = i;
        for (size_t i = 1; i < n; i++) {
            size_t key = list[i];
            size_t j = i;
            while (j > 0 &&
                   ((ExtRec *)g_exts.p)[list[j - 1]].order >
                       ((ExtRec *)g_exts.p)[key].order) {
                list[j] = list[j - 1];
                j--;
            }
            list[j] = key;
        }
        for (size_t i = 0; i < n; i++) {
            ExtRec *e = &((ExtRec *)g_exts.p)[list[i]];
            if (!e->used || e->state != 0) continue;
            if (!e->init) { e->state = 1; continue; }
            u64 owner = e->owner;
            int (*init)(const AgcExtHost *) = e->init;
            void (*shutdown)(void) = e->shutdown;
            owner_set(owner);
            int rc = init(agentc_ext_host());
            owner_set(0);
            /* Re-acquire: init may have grown the descriptor vector. */
            e = &((ExtRec *)g_exts.p)[list[i]];
            if (!e->used) continue;   /* a re-entrant unload removed the record */
            if (rc != 0) {
                agentc_logf(3, "ext: %s init failed", e->name);
                owner_remove(owner);
                if (shutdown) {
                    /* Attribute anything the failed init's shutdown registers to
                     * this owner so the sweep below removes it (owner is 0
                     * here after the init call). */
                    owner_set(owner);
                    shutdown();
                    owner_set(0);
                }
                owner_remove(owner);   /* sweep anything shutdown() registered */
                e = &((ExtRec *)g_exts.p)[list[i]];
                e->state = 2;
                /* idle now (owner restored, no jobs): close a dynamic
                 * library whose failed init left its registrations behind. */
                dyn_request_close(owner);
                continue;
            }
            e->state = 1;
            agentc_logf(0, "ext: %s loaded", e->name);
            typed_sub_ensure();
        }
        agentc_free(list);
        bool pending = false;
        for (size_t i = 0; i < g_exts.len; i++) {
            ExtRec *e = &((ExtRec *)g_exts.p)[i];
            if (e->used && e->state == 0) pending = true;
        }
        if (!pending) { g_loading--; return; }
    }
    size_t stuck = 0;
    for (size_t i = 0; i < g_exts.len; i++) {
        ExtRec *e = &((ExtRec *)g_exts.p)[i];
        if (e->used && e->state == 0) stuck++;
    }
    agentc_logf(2, "ext: load_all cap reached, %llu extension(s) remain pending",
                (unsigned long long)stuck);
    g_loading--;
}

void agentc_ext_unload(const char *name) {
    ExtRec *e = name ? ext_find(name) : NULL;
    if (!e) return;
    /* Drop the owner's registrations before shutdown(), matching
     * the failed-init path in load_all(). Copy the fields we need: shutdown()
     * may register something and move the descriptor vector. */
    u64 owner = e->owner;
    bool loaded = e->state == 1;
    void (*shutdown)(void) = e->shutdown;
    owner_remove(owner);
    if (loaded && shutdown) {
        owner_set(owner);
        shutdown();
        owner_set(0);
    }
    owner_remove(owner);   /* sweep anything shutdown() registered */
    /* idle now (owner restored and no callback running); close the
     * library, or defer to the last job unlink / pump epilogue / shutdown when
     * a live async job or an outer callback still references it. This runs
     * before the record is freed so the close can still see the owner. */
    dyn_request_close(owner);
    e = ext_find(name);
    if (!e) return;
    agentc_free(e->name);
    agentc_free(e->version);
    agentc_memset(e, 0, sizeof *e);
}

void agentc_ext_shutdown(void) {
    if (g_loading > 0) {
        /* An extension init() called us: g_exts is live under the load loop,
         * so freeing it here would leave the loop holding freed records. The
         * normal process-end shutdown still tears everything down. */
        agentc_logf(3, "ext: shutdown called during load; ignored");
        return;
    }
    if (g_events_sub != 0) {
        agentc_events_unsubscribe(g_events_sub);
        g_events_sub = 0;
    }
    /* Reverse teardown over the live records. shutdown() may register another
     * extension, so the loop cannot bound on a stale length: it re-reads the
     * live tail every iteration and clears each record as it is visited. A
     * shutdown that keeps appending is capped with a log (the cap allows the
     * initial records plus a grace window); the sweep after the loop still
     * frees every remaining record's strings, so nothing leaks even then. */
    size_t budget = g_exts.len + AGENTC_EXT_SHUTDOWN_ROUNDS;
    for (;;) {
        size_t idx = g_exts.len;
        while (idx > 0 && !((ExtRec *)g_exts.p)[idx - 1].used) idx--;
        if (idx == 0) break;
        if (budget-- == 0) {
            agentc_logf(2, "ext: shutdown cap reached, %llu record(s) left",
                        (unsigned long long)idx);
            break;
        }
        ExtRec *e = &((ExtRec *)g_exts.p)[idx - 1];
        u64 owner = e->owner;
        void (*shutdown)(void) = e->state == 1 ? e->shutdown : NULL;
        owner_remove(owner);
        if (shutdown) {
            /* Attribute logs and any set_status/defer made by shutdown(). */
            owner_set(owner);
            shutdown();
            owner_set(0);
        }
        /* shutdown() may have appended records, but this slot is stable (an
         * append only grows the vector). */
        e = &((ExtRec *)g_exts.p)[idx - 1];
        agentc_free(e->name);
        agentc_free(e->version);
        e->name = NULL;
        e->version = NULL;
        e->used = 0;   /* visited: the next iteration takes the next live tail */
    }
    /* Records appended past the cap still own their name/version strings. */
    for (size_t i = 0; i < g_exts.len; i++) {
        ExtRec *e = &((ExtRec *)g_exts.p)[i];
        if (!e->used) continue;
        agentc_free(e->name);
        agentc_free(e->version);
        e->name = NULL;
        e->version = NULL;
        e->used = 0;
    }
    agentc_vec_free(&g_exts);
    /* Close every dynamic library that is idle. A mapping still needed
     * by a live async job, or by an extension callback on the stack, stays
     * mapped (fallback); the sweep runs before g_tools/g_retired are freed so
     * the live-job check is exact. */
    dyn_shutdown_sweep();
    /* release every remaining tool record (owner-tagged ones were already
     * released by owner_remove; this catches owner==0/pump-added entries) */
    for (size_t i = 0; i < g_tools.len; i++)
        if (tool_at(i)) tool_remove_at(i);
    agentc_vec_free(&g_tools);
    /* Free the retired pile. A record still referenced by a live async
     * job survives shutdown; the last job to unlink releases it (free_pending),
     * so shutdown never leaves a dangling tool.ud behind. */
    for (size_t i = 0; i < g_retired.len; i++) {
        ToolRec *t = ((ToolRec **)g_retired.p)[i];
        if (!t) continue;
        if (t->jobs != NULL) t->free_pending = true;
        else tool_release(t);
    }
    agentc_vec_free(&g_retired);
    for (size_t i = 0; i < g_cmds.len; i++) {
        CmdRec *c = &((CmdRec *)g_cmds.p)[i];
        agentc_free(c->name);
        agentc_free(c->desc);
    }
    agentc_vec_free(&g_cmds);
    for (size_t i = 0; i < g_sections.len; i++) {
        SecRec *s = &((SecRec *)g_sections.p)[i];
        agentc_free(s->key);
    }
    agentc_vec_free(&g_sections);
    for (size_t i = 0; i < g_status_regs.len; i++)
        agentc_free(((StatusReg **)g_status_regs.p)[i]);
    agentc_vec_free(&g_status_regs);
    for (size_t i = 0; i < AGENTC_EXT_SET_STATUS_KEYS; i++)
        if (g_set_status[i].used) set_status_clear(&g_set_status[i]);
    agentc_status_reset();   /* no provider survives a full shutdown */
    agentc_vec_free(&g_pumps);
    for (size_t i = 0; i < AGENTC_EXT_MAX_HOOKS; i++)
        if (g_hooks[i].used) agentc_free(g_hooks[i].point);
    agentc_memset(g_hooks, 0, sizeof g_hooks);
    agentc_free(g_cwd);           g_cwd = NULL;
    agentc_free(g_session_id);    g_session_id = NULL;
    agentc_free(g_session_file);  g_session_file = NULL;
    agentc_free(g_system_prompt); g_system_prompt = NULL;
    agentc_memset(&g_ui, 0, sizeof g_ui);
    g_ui_ud = NULL;
    g_entry_sink = NULL;
    g_entry_ud = NULL;
    g_model_fn = NULL;
    g_model_ud = NULL;
    g_thinking_fn = NULL;
    g_thinking_ud = NULL;
    for (size_t i = 0; i < AGENTC_EXT_MAX_HTTP; i++)
        if (g_http[i].used) http_release(&g_http[i]);
    defer_reset();
    json_scratch_clear();
    agentc_vec_free(&g_json_scratch);
    /* Retire every extension row (owner_remove already covered the ones
     * owned by a live record; this catches core-added rows), drop the rows from
     * the provider registry, then free the stable records -- which also clears
     * their static model rows. Nothing ext-registered survives a full
     * shutdown. */
    agentc_provider_retire_ext();
    agentc_provider_registry_reset();
    for (size_t i = 0; i < g_providers.len; i++) {
        ProviderRec *p = ((ProviderRec **)g_providers.p)[i];
        if (p) provider_rec_free(p);
    }
    agentc_vec_free(&g_providers);
    g_ext_seq = 0;
    owner_set(0);
    g_dirty = false;
    g_hooks_live = 0;
    g_emit_depth = 0;
    g_pumping = false;
}

/* -------------------------------------------------------------- dirty flag */

bool agentc_ext_dirty(void) { return g_dirty; }
void agentc_ext_clear_dirty(void) { g_dirty = false; }

/* --------------------------------------------------------- agent event map */

/* Point mapping only. Used by the wants fast path so an event with no
 * subscriber never builds a payload. message_end/turn_end are deliberately
 * unmapped: they are OVERRIDE points consumed directly by the agent (the typed
 * event stays for the front ends). AGENTC_EV_ERROR and AGENTC_EV_COMPACT are
 * unmapped until their documented point names and payloads exist (neither
 * "error" nor "compact" is a hook point). */
static const char *agent_event_point(int ev) {
    switch (ev) {
    case AGENTC_EV_AGENT_START:      return "agent_start";
    case AGENTC_EV_TURN_START:       return "turn_start";
    case AGENTC_EV_MSG_START:        return "message_start";
    case AGENTC_EV_AGENT_END:        return "agent_end";
    case AGENTC_EV_TEXT_DELTA:
    case AGENTC_EV_THINK_DELTA:      return "message_update";
    /* A retry restarts the current message: observers that accumulate the
     * message_update deltas get the rollback signal on the same point. */
    case AGENTC_EV_MSG_RESET:       return "message_update";
    case AGENTC_EV_TOOL_EXEC_START:  return "tool_execution_start";
    case AGENTC_EV_TOOL_EXEC_END:    return "tool_execution_end";
    default:                         return NULL;
    }
}

static const char *ext_stop_name(int stop) {
    switch (stop) {
    case AGENTC_STOP_STOP:    return "stop";
    case AGENTC_STOP_LENGTH:  return "length";
    case AGENTC_STOP_TOOLUSE: return "tool_use";
    case AGENTC_STOP_ERROR:   return "error";
    case AGENTC_STOP_ABORTED: return "aborted";
    default:                  return "pending";
    }
}

void agentc_ext_emit_agent_event(int ev, const void *data) {
    const char *name = agent_event_point(ev);
    if (!name || !agentc_ext_wants(name)) return;
    AgcBuf p = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &p);
    switch (ev) {
    case AGENTC_EV_AGENT_START:
        name = "agent_start";
        agentc_jsonw_obj(&w);
        agentc_jsonw_end(&w);
        break;
    case AGENTC_EV_TURN_START: {
        /* data: const int *turn_index (NULL-safe, omitted when absent) */
        const int *idx = data;
        name = "turn_start";
        agentc_jsonw_obj(&w);
        if (idx) {
            agentc_jsonw_key(&w, "turn_index");
            agentc_jsonw_i64(&w, *idx);
        }
        agentc_jsonw_end(&w);
        break;
    }
    case AGENTC_EV_MSG_START: {
        const AgcMsg *m = data;
        name = "message_start";
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "role");
        agentc_jsonw_cstr(&w, m && m->role == AGENTC_ROLE_USER       ? "user"
                         : m && m->role == AGENTC_ROLE_ASSISTANT ? "assistant"
                         : m && m->role == AGENTC_ROLE_TOOL      ? "tool"
                                                             : "system");
        agentc_jsonw_end(&w);
        break;
    }
    case AGENTC_EV_AGENT_END: {
        /* data: const int *terminal stop reason (NULL-safe) */
        const int *stop = data;
        name = "agent_end";
        agentc_jsonw_obj(&w);
        if (stop) {
            agentc_jsonw_key(&w, "stop_reason");
            agentc_jsonw_cstr(&w, ext_stop_name(*stop));
        }
        agentc_jsonw_end(&w);
        break;
    }
    case AGENTC_EV_TEXT_DELTA:
    case AGENTC_EV_THINK_DELTA: {
        const AgcTextDelta *d = data;
        name = "message_update";
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, ev == AGENTC_EV_TEXT_DELTA ? "text_delta" : "thinking_delta");
        agentc_jsonw_key(&w, "text");
        if (d && d->text) agentc_jsonw_str(&w, d->text, d->len);
        else agentc_jsonw_cstr(&w, "");
        agentc_jsonw_end(&w);
        break;
    }
    case AGENTC_EV_MSG_RESET:
        name = "message_update";
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "type");
        agentc_jsonw_cstr(&w, "message_reset");
        agentc_jsonw_end(&w);
        break;
    case AGENTC_EV_TOOL_EXEC_START:
    case AGENTC_EV_TOOL_EXEC_END: {
        const AgcToolExec *e = data;
        bool end = ev == AGENTC_EV_TOOL_EXEC_END;
        name = end ? "tool_execution_end" : "tool_execution_start";
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "tool_call_id");
        agentc_jsonw_cstr(&w, e && e->call_id ? e->call_id : "");
        agentc_jsonw_key(&w, "tool_name");
        agentc_jsonw_cstr(&w, e && e->tool_name ? e->tool_name : "");
        agentc_jsonw_key(&w, "input");
        const char *args = e && e->args_json ? e->args_json : "{}";
        agentc_jsonw_raw(&w, args, agentc_strlen(args));
        if (end) {
            agentc_jsonw_key(&w, "result");
            const char *res = e && e->result ? e->result : "";
            agentc_jsonw_str(&w, res, agentc_strlen(res));
            agentc_jsonw_key(&w, "is_error");
            agentc_jsonw_bool(&w, e ? e->is_error : false);
            agentc_jsonw_key(&w, "duration_ms");
            agentc_jsonw_i64(&w, e ? e->duration_ms : 0);
        }
        agentc_jsonw_end(&w);
        break;
    }
    default:
        return;
    }
    AgcExtResult r = agentc_ext_emit(name, (const char *)p.p);
    agentc_free(r.result_json);
    agentc_buf_free(&p);
}

void agentc_ext_tool_veto(void *ud, const char *tool_call_id, const char *tool_name,
                          const char *args_json, AgcToolVetoDecision *out) {
    (void)ud;
    if (!out || !agentc_ext_wants("tool_call")) return;
    AgcBuf p = { 0 };
    AgcJsonW w;
    jsonw_out(&w, &p);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "tool_call_id");
    agentc_jsonw_cstr(&w, tool_call_id ? tool_call_id : "");
    agentc_jsonw_key(&w, "tool_name");
    agentc_jsonw_cstr(&w, tool_name ? tool_name : "");
    agentc_jsonw_key(&w, "input");
    const char *a = args_json ? args_json : "{}";
    agentc_jsonw_raw(&w, a, agentc_strlen(a));
    agentc_jsonw_end(&w);
    AgcExtResult r = agentc_ext_emit("tool_call", (const char *)p.p);
    agentc_buf_free(&p);
    out->block = (r.blocked || r.handled) ? 1 : 0;

    if (r.result_json) {
        AgcJsonArena *arena = agentc_json_arena_new(0);
        AgcJson *o = agentc_json_parse_in(arena, r.result_json, agentc_strlen(r.result_json));
        const char *reason = agentc_json_get_str(o, "reason");
        if (reason) out->reason = ext_dup(reason);
        const AgcJson *in = agentc_json_get(o, "input");
        if (in && agentc_json_type(in) != AGENTC_JSON_NULL) {
            AgcBuf b = { 0 };
            AgcJsonW iw;
            agentc_jsonw_init(&iw, &b);
            agentc_json_emit(&iw, in);
            if (b.p) out->args_json = ext_dup((const char *)b.p);
            agentc_buf_free(&b);
        }
        /* pi-exact terminate: only a blocked call may end the batch. A
         * terminate without block is a handler bug: log it (level 1) and drop
         * the hint rather than terminating a batch that keeps running. */
        if (agentc_json_get_bool(o, "terminate", false)) {
            if (out->block) out->terminate = 1;
            else agentc_logf(1, "ext: tool_call: terminate ignored without block");
        }
        agentc_json_arena_free(arena);
    }
    agentc_free(r.result_json);
}

/* --------------------------------------------------------------- vtable */

static const AgcExtHost g_host = {
    .abi_version = AGENTC_EXT_ABI,
    .struct_size = sizeof(AgcExtHost),
    .alloc = host_alloc,
    .free = host_free,
    .log = host_log,
    .out_write = host_out_write,
    .add_tool = host_add_tool,
    .add_command = host_add_command,
    .add_section = host_add_section,
    .add_status = host_add_status,
    .on = host_on,
    .off = host_off,
    .emit = host_emit,
    .defer = host_defer,
    .is_cancelled = host_is_cancelled,
    .cwd = host_cwd,
    .session_id = host_session_id,
    .session_file = host_session_file,
    .system_prompt = host_system_prompt,
    .append_entry = agentc_ext_append_entry,
    .http_request = host_http_request,
    .http_cancel = host_http_cancel,
    .notify = host_notify,
    .set_status = host_set_status,
    .set_model = host_set_model,
    .set_thinking = host_set_thinking,
    .request_recompose = host_request_recompose,
    .strdup_ = host_strdup,
    .json_get_str = host_json_get_str,
    .json_get_int = host_json_get_int,
    .json_get_bool = host_json_get_bool,
    .json_escape = host_json_escape,
    .set_title = host_set_title,
    .add_provider = host_add_provider,
};

const AgcExtHost *agentc_ext_host(void) { return &g_host; }
