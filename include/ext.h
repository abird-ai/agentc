/*
 * ext.h — app- and core-facing API of the agentc extension registry.
 *
 * The stable extension-side contract is include/agentc_ext.h. This header is
 * what the app, the TUI and the other core modules use to register default and
 * statically-linked extensions, collect their tools/commands/sections, emit
 * hook points and pump deferred work. It names internal types (AgcTool, AgcBuf)
 * and therefore is not part of the extension ABI.
 */
#ifndef AGENTC_EXT_REGISTRY_H
#define AGENTC_EXT_REGISTRY_H

#include "agentc.h"
#include "agent.h"
#include "agentc_ext.h"
#include "config.h"

/* The process-wide host vtable handed to agentc_ext_init. */
const AgcExtHost *agentc_ext_host(void);

/* Register the extensions the core ships (builtin-tools, builtin-context and,
 * unless no_mcp, mcp). They are ordinary extensions; being compiled in only
 * lets them include src/ext/registry_int.h and use the internal bus. */
void agentc_ext_register_defaults(bool no_mcp);
/* Copy one extension descriptor into the pending set (no init yet). */
void agentc_ext_register(const AgcExt *ext);
/* Adopt one entry point: call it to obtain the descriptor, then register it.
 * Used by the generated registry and the tests. Returns 0 | -errno. */
int  agentc_ext_adopt(const char *name, int (*entry)(const AgcExtHost *, AgcExt *));
/* Register the generated static-extension table (build/exts.c). No-op without. */
void agentc_ext_register_linked(void);
/* Scan <config home>/extensions/ through the platform loader and register
 * each candidate as a pending dynamic extension (duplicates are skipped before
 * the library is opened). A platform without a dynamic loader (the Linux static
 * build) is a no-op. Call after register_linked() and before apply_config(). */
void agentc_ext_register_dynamic(void);
/* Initialize every pending extension, lowest `order` first (stable). */
void agentc_ext_load_all(void);
/* Drop every registration of `name`, then call its shutdown(). */
void agentc_ext_unload(const char *name);
/* Exclude defaults and linked extensions named in cfg->extensions_disabled.
 * Call after registration and before load_all. */
void agentc_ext_apply_config(const AgcConfig *cfg);
/* Deferred callbacks and at most one pending extension HTTP request. */
void agentc_ext_pump(void);

/* Count-first collection of the active tool table (internal AgcTool). With
 * out == NULL returns the total; otherwise fills min(total, max). */
size_t agentc_ext_tools(AgcTool *out, size_t max);

/* One registered slash command, as the TUI menu needs it. */
typedef struct {
    const char *name;
    const char *description;
} AgcExtCommandInfo;
size_t agentc_ext_commands(AgcExtCommandInfo *out, size_t max);
char *agentc_ext_run_command(const char *name, const char *args);

/* Render the extension-contributed prompt sections into `out` (ascending
 * priority). The core sections are rendered by agentc_prompt_build. */
void agentc_ext_render_sections(AgcBuf *out);

/* Hook bus. `emit` returns a host-allocated result (free result_json with
 * agentc_free); `wants` is the hot-path guard. */
bool agentc_ext_wants(const char *point);
AgcExtResult agentc_ext_emit(const char *point, const char *payload_json);
/* Consumer side of a `fields`-merge hook result: apply `patch_json` to the
 * JSON object `base_json` (NULL -> "{}"). A key present in the patch replaces
 * the base key; a key present with JSON null deletes it; absent keys are
 * unchanged. Returns a host-allocated NUL-terminated JSON object (free with
 * agentc_free); never NULL. This is what an override calls to consume a
 * hook's result patch. */
char *agentc_ext_merge_fields(const char *base_json, const char *patch_json);
/* Installs as the agent tool veto: emits "tool_call" and fills the decision
 * (block and/or a replacement args_json). No-op unless the point is watched. */
void agentc_ext_tool_veto(void *ud, const char *tool_call_id, const char *tool_name,
                          const char *args_json, AgcToolVetoDecision *out);

/* before_provider_headers helpers. The header block is "Name: value\r\n"
 * lines.
 *   to_json     -> owned JSON object of the block; duplicate names: last wins;
 *                  malformed lines are skipped.
 *   apply_patch -> owned patched block. Patch keys must be RFC token-safe
 *                  (`hdr_key_ok`); keys failing the check make the whole patch
 *                  unparseable (NULL). content-length/transfer-encoding/host
 *                  are dropped with a log. Values are strings (CR/LF rewritten
 *                  to spaces) or JSON null (delete every occurrence). A
 *                  matching key replaces the value in place for every
 *                  occurrence, preserving order; unmatched keys are appended
 *                  in patch order; duplicate patch keys: last wins. Returns
 *                  NULL and leaves the caller's block untouched on a parse
 *                  error. */
char *agentc_ext_headers_to_json(const char *head);
char *agentc_ext_headers_apply_patch(const char *head, const char *patch_json);

/* Recompose bookkeeping. The active table is applied at the turn boundary. */
bool agentc_ext_dirty(void);
void agentc_ext_clear_dirty(void);

/* Descriptor-table introspection for front ends (`--list-extensions`, the
 * startup summary). Count-first like agentc_ext_tools: with out == NULL return
 * the total; otherwise fill min(total, max) and return the filled count.
 * Entries are in registration order (composition order) and the pointers are
 * borrowed until unload/shutdown. `state` is one of AgcExtState; `disabled`
 * marks a config exclusion, and `known == false` means that disabled name
 * matched no registered extension. */
typedef enum {
    AGENTC_EXT_STATE_PENDING = 0,   /* registered, load_all has not run */
    AGENTC_EXT_STATE_LOADED  = 1,   /* init() succeeded (or there was none) */
    AGENTC_EXT_STATE_FAILED  = 2,   /* init() returned an error */
    AGENTC_EXT_STATE_SKIPPED = 3,   /* excluded by extensions.disabled */
} AgcExtState;

typedef struct {
    const char *name;
    const char *version;   /* may be NULL */
    int32_t order;
    int state;             /* AgcExtState */
    bool disabled;         /* excluded by config */
    bool known;            /* false only for an unmatched disabled name */
    bool dynamic;          /* loaded from <config home>/extensions/ */
} AgcExtInfo;

size_t agentc_ext_describe(AgcExtInfo *out, size_t max);
/* True when `name` is registered and its init() succeeded. A config-disabled
 * or failed extension is not loaded; an unknown name is never loaded. */
bool agentc_ext_is_loaded(const char *name);

/* Maps one AGENTC_EV_* agent event to its hook point and JSON payload and
 * fans it out. Front ends call this from their agent event handler. */
void agentc_ext_emit_agent_event(int ev, const void *data);

void agentc_ext_shutdown(void);

/* Process context exposed to extensions. NULL fields keep the current value. */
typedef struct {
    const char *cwd;
    const char *session_id;
    const char *session_file;
    const char *system_prompt;
} AgcExtContext;
void agentc_ext_set_context(const AgcExtContext *ctx);

/* Sinks: append_entry (wire to the session), UI, model/thinking control. */
typedef void (*AgcExtEntrySink)(void *ud, const char *type, const char *data_json);
void agentc_ext_set_entry_sink(AgcExtEntrySink sink, void *ud);

/* Core-facing append_entry: same validation and delivery as the host vtable's
 * append_entry (RFC-token type, JSON object data, 8 KiB cap), for core code
 * that must not call through the process-wide host vtable. */
void agentc_ext_append_entry(const char *type, const char *data_json);

typedef struct {
    void (*notify)(void *ud, const char *message, int level);
    /* Reserved: host->set_status now renders through the status table, so the
     * registry no longer forwards to this slot. */
    void (*set_status)(void *ud, const char *key, const char *text);
    void (*set_title)(void *ud, const char *title);
} AgcExtUiSink;
void agentc_ext_set_ui_sink(const AgcExtUiSink *sink, void *ud);

typedef int (*AgcExtModelFn)(void *ud, const char *provider, const char *model);
typedef void (*AgcExtThinkingFn)(void *ud, const char *level);
void agentc_ext_set_model_sink(AgcExtModelFn cb, void *ud);
void agentc_ext_set_thinking_sink(AgcExtThinkingFn cb, void *ud);

/* Core default extensions (implemented in src/core and src/ext). */
const AgcExt *agentc_builtin_tools_ext(void);
const AgcExt *agentc_builtin_context_ext(void);
const AgcExt *agentc_mcp_ext(void);

#endif /* AGENTC_EXT_REGISTRY_H */
