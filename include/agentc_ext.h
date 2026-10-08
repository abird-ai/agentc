/*
 * agentc_ext.h — stable C ABI for agentc extensions (C, Rust, Zig, ...).
 *
 * One model: built-in tools, MCP and statically-linked extensions are all
 * extensions. There is a single `abi_version`, one header, and append-only
 * structs from this baseline (no ABI_MIN, no reserved[] offsets).
 *
 * ABI rules:
 *   - All structs are append-only. The host checks `struct_size`; new fields go
 *     at the end. `abi_version` is bumped for every incompatible change.
 *   - Every `const char *` payload crossing this boundary is a NUL-terminated
 *     UTF-8 JSON string unless stated otherwise.
 *   - Memory returned to the host must come from host->alloc; the host frees
 *     it. Registered strings are copied by the host at the add_/on call, so the
 *     extension may reuse or free its own storage immediately.
 *   - The host is single-threaded. An extension may use threads, but any call
 *     into the host from a worker must go through host->defer. `host->defer`
 *     from a worker is captured with the owner active at that moment;
 *     attribution is best-effort, so extension `defer` should be called from
 *     the main loop (or from a callback whose owner is active). host->log calls
 *     made from an extension callback are prefixed with `[ext:<name>] `.
 *
 * Process-wide limits (fixed, not part of the ABI):
 *   - hook handlers: 128 registrations; a point name not in the hook-point
 *     table is rejected at `on` with a level-3 log;
 *   - status keys: 8 live `set_status` keys, keyed by (owning extension, key),
 *     so two extensions may use the same key without collision;
 *   - extension HTTP: 8 queued requests; at most one is dispatched per pump
 *     and the wire layer is not resumable, so a request may occupy one pump
 *     call for up to 10 seconds (see host->http_request);
 *   - deferred callbacks: 256 queued; overflow is counted and logged on the
 *     next pump;
 *   - append_entry: a custom type of at most 64 chars from [A-Za-z0-9_.:-]
 *     and a JSON object of at most 8192 bytes; anything else is rejected with
 *     a log before it can reach the session;
 *   - custom providers: 64 records process-wide; a name must not collide with
 *     any live provider row or preset.
 *
 * ABI growth is read through the gates below: AGENTC_EXT_FIELD_OK covers every
 * appended contribution field and AGENTC_EXT_HOST_HAS every appended host
 * service (both never read past the caller's struct_size, and HOST_HAS also
 * rejects a present-but-NULL slot).
 *
 * An extension is a static object (or shared library) exporting exactly one
 * canonical symbol:
 *
 *     int agentc_ext_init(const AgcExtHost *host, AgcExt *out);
 *
 * compiled per extension with -Dagentc_ext_init=agentc_ext_<ident>_init (the
 * same pattern the generated registry uses). Return 0 on success. `out` is
 * zeroed; fill abi_version, struct_size, name, version, order, init, shutdown
 * (and required_host_size only when a host service beyond the baseline is
 * needed).
 */
#ifndef AGENTC_EXT_H
#define AGENTC_EXT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENTC_EXT_ABI 1

/* Tool flags (the values match the internal AGENTC_TOOL_* bits).
 *
 * AGENTC_TOOL_HIDDEN means "never advertised": the prompt builder and every
 * provider request skip the tool unconditionally, so the model can neither see
 * nor call it. It is not the app's destructive-tool policy: that policy
 * keeps a tool registered but out of the default auto-selection until
 * --tools/default_tools names it, and a named destructive tool is advertised
 * normally. */
#define AGENTC_TOOL_READONLY    0x1u
#define AGENTC_TOOL_DESTRUCTIVE 0x2u
#define AGENTC_TOOL_SEQUENTIAL  0x4u
#define AGENTC_TOOL_HIDDEN      0x8u
#define AGENTC_TOOL_TIMEOUT_DEF_MS 120000

/* Hook capabilities. OBSERVE = side effects only; OVERRIDE = may return a
 * decision/replacement. Required capability is fixed per hook point. */
#define AGENTC_HOOK_OBSERVE  0x1u
#define AGENTC_HOOK_OVERRIDE 0x2u

/*
 * ABI capability gates. Every contribution field appended after the baseline
 * must be read behind AGENTC_EXT_FIELD_OK (the caller's struct_size may not
 * cover it), and every host service appended after the baseline must be
 * called behind AGENTC_EXT_HOST_HAS:
 *
 *   if (AGENTC_EXT_FIELD_OK(tool, AgcExtTool, new_field)) use(tool->new_field);
 *   if (AGENTC_EXT_HOST_HAS(host, new_service)) host->new_service(...);
 *
 * Both never read past the caller's declared struct_size. HOST_HAS also
 * rejects a present-but-NULL service, so a host may zero an unsupported slot.
 */
#define AGENTC_EXT_FIELD_OK(s, T, f) \
    ((s) != NULL && \
     (s)->struct_size >= (uint32_t)(offsetof(T, f) + sizeof((s)->f)))
#define AGENTC_EXT_HOST_HAS(h, f) \
    ((h) != NULL && \
     (h)->struct_size >= (uint32_t)(offsetof(AgcExtHost, f) + sizeof((h)->f)) && \
     (h)->f != NULL)

typedef struct AgcExtHost AgcExtHost;

/* ---------------------------------------------------------------- tools */

/* Published tool-call view (append-only). The old ABI passed these as separate
 * strings; a struct lets a tool read its arguments and cancellation token. */
typedef struct AgcExtToolCall {
    uint32_t    struct_size;
    const char *call_id;
    const char *name;
    const char *args_json;      /* validated arguments object */
    const char *signal_token;   /* pass to host->is_cancelled */
} AgcExtToolCall;

/* Async tool stop reasons, passed to `stop`. FINISHED means "no more
 * steps", not success: the result is `*is_error`. */
#define AGENTC_EXT_TOOL_FINISHED  0
#define AGENTC_EXT_TOOL_ERROR     1
#define AGENTC_EXT_TOOL_TIMEOUT   2
#define AGENTC_EXT_TOOL_CANCELLED 3
#define AGENTC_EXT_TOOL_UNLOAD    4

/* Async `timeout_ms` clamp; a larger value is clamped with a log. */
#define AGENTC_EXT_TOOL_TIMEOUT_MAX_MS 1800000

/* Tools are synchronous by default: append result text to
 * `out` with host->out_write and return 0 | -errno.
 *
 * An asynchronous tool leaves `run` NULL and provides the complete
 * start/step/stop trio; when `run` is non-NULL the trio is ignored (run wins).
 * Codes and contract:
 *
 *   start  -> 0 running, 1 complete, <0 fatal; `*state` is extension-owned
 *             (host->alloc in start, host->free in stop; the host never frees
 *             it). A start that returns <0 is never followed by stop.
 *   step   -> 0 running, 1 complete, <0 fatal.
 *   stop   -> called exactly once for every start that returned >= 0, with one
 *             of AGENTC_EXT_TOOL_FINISHED/ERROR/TIMEOUT/CANCELLED/UNLOAD.
 *
 * The host drives all three on the main loop; start/step must not block (I/O
 * goes through host->defer / host->http_request, serviced between steps). The
 * call's signal_token is valid from start through stop: poll cancellation with
 * host->is_cancelled and the host delivers stop(CANCELLED). stop must not
 * register contributions and should not block. Step output goes through
 * host->out_write into a per-step staging buffer the host appends to the job
 * under the streaming/truncation caps; `*is_error` set at completion is the
 * tool result. `call` and its strings stay valid through stop. */
typedef struct AgcExtTool {
    uint32_t    struct_size;
    uint32_t    flags;                 /* AGENTC_TOOL_* */
    const char *name;                  /* [a-z0-9_:.-]+, <=64, unique */
    const char *label;
    const char *description;
    const char *parameters_json;       /* JSON Schema object */
    const char *prompt_snippet;        /* optional # Tools line */
    const char *prompt_guidelines;     /* optional # Rules bullets */
    void       *ud;
    int         timeout_ms;            /* 0 => AGENTC_TOOL_TIMEOUT_DEF_MS */
    int (*run)(const AgcExtHost *host, const struct AgcExtTool *self,
               const AgcExtToolCall *call, void *out, bool *is_error);
    int  (*start)(const AgcExtHost *host, const struct AgcExtTool *self,
                  const AgcExtToolCall *call, void *out, bool *is_error,
                  void **state);
    int  (*step)(const AgcExtHost *host, const struct AgcExtTool *self,
                 const AgcExtToolCall *call, void *state, void *out,
                 bool *is_error);
    void (*stop)(const AgcExtHost *host, const struct AgcExtTool *self,
                 void *state, int reason);
} AgcExtTool;

typedef struct AgcExtCommand {
    uint32_t    struct_size;
    const char *name;
    const char *description;
    void       *ud;
    void (*run)(const AgcExtHost *host, void *ud, const char *args_json, void *out);
} AgcExtCommand;

/* One prompt section. Rendered in ascending priority order after the four core
 * sections; render() appends UTF-8 text via host->out_write and returns 0. */
typedef struct AgcExtSection {
    uint32_t    struct_size;
    const char *key;
    int32_t     priority;              /* lower first; ties keep registration order */
    void       *ud;
    int (*render)(const AgcExtHost *host, void *ud, void *out);
} AgcExtSection;

/* ------------------------------------------------------- status segments */

typedef struct AgcExtStatusSegment {
    uint32_t    struct_size;
    uint32_t    slot;                  /* AGENTC_PSEG_SLOT_LEFT / _RIGHT */
    int32_t     priority;
    uint32_t    style;                 /* AGENTC_PSEG_STYLE_* */
    const char *text;                  /* NUL-terminated UTF-8; copied by host */
} AgcExtStatusSegment;

#define AGENTC_PSEG_SLOT_LEFT  0u
#define AGENTC_PSEG_SLOT_RIGHT 1u

#define AGENTC_PSEG_STYLE_DIM    0x0001u
#define AGENTC_PSEG_STYLE_BOLD   0x0002u
#define AGENTC_PSEG_STYLE_ACCENT 0x0004u
#define AGENTC_PSEG_STYLE_WARN   0x0008u
#define AGENTC_PSEG_STYLE_ERROR  0x0010u
#define AGENTC_PSEG_STYLE_OK     0x0020u
#define AGENTC_PSEG_STYLE_ALL    0x003Fu

typedef size_t (*AgcExtStatusProvider)(void *ud, AgcExtStatusSegment *out,
                                       size_t max, char *arena, size_t arena_cap);

/* ----------------------------------------------------------------- hooks */

/*
 * Hook handler. `point` is the hook-point name, `payload_json` a JSON object.
 *
 * Return value by policy (policy and merge mode are fixed per point; see the
 * hook-point table):
 *
 *   return | chain                  | first              | chain-veto
 *   -------+------------------------+--------------------+-------------------
 *   0      | merge result, continue | pass to the next   | pass to the next
 *   1      | record `handled`,      | short-circuit,     | block the call and
 *          | later handlers still   | this result wins   | short-circuit
 *          | run and merge          |                    |
 *   <0     | fail-closed: block the point (the operation is refused)
 *
 * OBSERVE points allow only 0 or <0: an OBSERVE handler that returns 1 or
 * sets *result_json is logged and ignored. An observe failure is fail-open
 * (log and continue); an override failure is fail-closed (block).
 *
 * A handler that returns 1 or sets *result_json must have registered with
 * AGENTC_HOOK_OVERRIDE. `*result_json`, when set, must be a JSON object
 * allocated with host->alloc; the host frees it.
 *
 * Result shapes, with one worked example per merge mode:
 *
 *   fields (the default; e.g. `input`, `message_end`)
 *     payload `{"text":"hi","source":"user"}` + result `{"text":"BYE"}`
 *     -> the next handler sees `{"text":"BYE","source":"user"}`; the
 *        accumulated patch returned by emit is that same object.
 *
 *   fields with a JSON null delete (e.g. `message_end`)
 *     payload `{"stop_reason":"stop","error":"x"}` + result
 *     `{"stop_reason":null}` -> the next handler sees `{"error":"x"}`
 *     (the key is deleted).
 *
 *   replace (e.g. `project_trust`, `session_before_compact`; override + first)
 *     result `{"trusted":"yes"}` becomes the whole accumulator instead of a
 *     merge. A `first` point stops at the first handler that returns 1; a
 *     result alone does not stop the walk (a later replace can still win).
 *
 *   flat header patch (`before_provider_headers`)
 *     payload `{"headers":{"Accept":"x"}}` + result
 *     `{"Accept":"y","X-Ext":null}` -> the consumer patches the header
 *     block to `Accept: y` and deletes any `X-Ext`. Forbidden names (Host,
 *     Content-Length, Transfer-Encoding) are dropped with a log.
 *
 *   chain-veto (`tool_call` only)
 *     result `{block?, reason?, input?, terminate?}`: `block:true` (or a `1`
 *     return, or a handler failure) blocks and short-circuits; an `input`
 *     object rewrites the arguments for the next handler and for execution;
 *     `terminate` is honored only alongside `block` and asks the agent to end
 *     the submission after the batch when every call in the assistant message
 *     was blocked with it. A `terminate` without `block` is ignored with a log.
 */
typedef int (*AgcExtHookFn)(void *ud, const char *point,
                            const char *payload_json, char **result_json);

/* Dispatch result, shared by the internal emit and host->emit. `struct_size`
 * is honored by host->emit: only the covered fields are written, so a caller
 * compiled against an older/smaller result layout never sees an overrun. An
 * emit whose struct cannot hold result_json logs and drops the result. */
typedef struct AgcExtResult {
    uint32_t    struct_size;
    int         handled;       /* a handler returned 1 */
    int         blocked;       /* handled and the result asked to block/cancel */
    char       *result_json;   /* host-allocated; free with host->free */
} AgcExtResult;

/* ------------------------------------------------------- custom providers */

/* Contribution kinds and view values. Role/block/stop values mirror the
 * internal AGENTC_ROLE_*, AGENTC_BLK_* and AGENTC_STOP_* constants. Discover
 * styles are a distinct, one-value-shorter namespace: the host translates
 * AGENTC_EXT_DISCOVER_NONE to the internal no-listing value at registration, so
 * do not assume the public and internal AGENTC_DISCOVER_* numbers are equal. */
#define AGENTC_EXT_ROLE_SYSTEM    0u
#define AGENTC_EXT_ROLE_USER      1u
#define AGENTC_EXT_ROLE_ASSISTANT 2u
#define AGENTC_EXT_ROLE_TOOL      3u

#define AGENTC_EXT_BLK_TEXT     0u
#define AGENTC_EXT_BLK_THINK    1u
#define AGENTC_EXT_BLK_TOOLCALL 2u

#define AGENTC_EXT_STOP_PENDING 0
#define AGENTC_EXT_STOP_STOP    1
#define AGENTC_EXT_STOP_LENGTH  2
#define AGENTC_EXT_STOP_TOOLUSE 3
#define AGENTC_EXT_STOP_ERROR   4
#define AGENTC_EXT_STOP_ABORTED 5

#define AGENTC_EXT_DISCOVER_DEFAULT   0
#define AGENTC_EXT_DISCOVER_ANTHROPIC 1
#define AGENTC_EXT_DISCOVER_OLLAMA    2
#define AGENTC_EXT_DISCOVER_NONE      3

/* Provider auth styles. The core owns the credential: the key never appears in
 * the request view and never reaches the extension. QUERY is reserved and
 * rejected at registration. */
#define AGENTC_EXT_AUTH_NONE   0u
#define AGENTC_EXT_AUTH_BEARER 1u  /* <header>: <prefix><key>; header defaults to
                                      * Authorization, prefix to "Bearer " */
#define AGENTC_EXT_AUTH_HEADER 2u  /* <header>: <prefix><key>; header required,
                                      * prefix is prepended verbatim */
#define AGENTC_EXT_AUTH_QUERY  3u  /* reserved: rejected at registration */

#define AGENTC_EXT_USAGE_INPUT       0x1u
#define AGENTC_EXT_USAGE_OUTPUT      0x2u
#define AGENTC_EXT_USAGE_CACHE_READ  0x4u
#define AGENTC_EXT_USAGE_CACHE_WRITE 0x8u
#define AGENTC_EXT_USAGE_REASONING   0x10u

/* One transcript block in the request view. Every string is borrowed for the
 * duration of build_request only (copy anything that must outlive the call).
 * text/text_len cover TEXT and THINK; tool_id/tool_name/tool_args cover
 * TOOLCALL (tool_args is raw JSON text, possibly partial while streaming). */
typedef struct AgcExtBlockView {
    uint32_t    struct_size;
    uint32_t    type;              /* AGENTC_EXT_BLK_* */
    const char *text;
    size_t      text_len;
    const char *tool_id;
    const char *tool_name;
    const char *tool_args;
} AgcExtBlockView;

/* One transcript message. stop_reason is AGENTC_EXT_STOP_* (PENDING outside an
 * assistant turn). Failed/aborted assistant turns and empty assistant turns are
 * filtered out by the host before the view is built. */
typedef struct AgcExtMessageView {
    uint32_t    struct_size;
    uint32_t    role;              /* AGENTC_EXT_ROLE_* */
    int32_t     stop_reason;       /* AGENTC_EXT_STOP_* */
    const AgcExtBlockView *blocks;
    size_t      nblocks;
} AgcExtMessageView;

/* One model-visible tool. AGENTC_TOOL_HIDDEN tools never reach the view. */
typedef struct AgcExtToolView {
    uint32_t    struct_size;
    const char *name;
    const char *description;
    const char *parameters_json;   /* JSON Schema object */
    uint32_t    flags;             /* AGENTC_TOOL_* */
} AgcExtToolView;

/* The complete request: provider/model identity, the system prompt, the
 * filtered transcript and the visible tool list. system may be NULL/empty. */
typedef struct AgcExtRequestView {
    uint32_t    struct_size;
    const char *provider;
    const char *model;
    const char *system;
    int32_t     thinking_level;    /* 0=off .. 4=high */
    int64_t     max_tokens;        /* 0 = model default */
    const AgcExtMessageView *messages;
    size_t      nmessages;
    const AgcExtToolView *tools;
    size_t      ntools;
} AgcExtRequestView;

/* Authentication style declared at registration. `header` and `prefix` are
 * copied by the host; `prefix` is prepended to the resolved key verbatim
 * (include any separator space). */
typedef struct AgcExtProviderAuth {
    uint32_t    struct_size;
    uint32_t    kind;              /* AGENTC_EXT_AUTH_* */
    const char *header;
    const char *prefix;
} AgcExtProviderAuth;

/* One static model the provider ships. `id` is required; 0 ctx/max means
 * unknown. The host copies the id/name strings. */
typedef struct AgcExtProviderModel {
    uint32_t    struct_size;
    const char *id;
    const char *name;
    uint32_t    ctx_window;
    uint32_t    max_tokens;
    int         reasoning;
    int         image;
} AgcExtProviderModel;

/* Opaque per-stream handle. `ud` belongs to the extension: allocate it with
 * host->alloc in stream_open, use it in stream_event/stream_finish, and release
 * it in stream_close (once). The host never reads or frees it. */
typedef struct AgcExtStream {
    uint32_t    struct_size;
    void       *ud;
} AgcExtStream;

/* Usage report. `fields` is a bitmask of AGENTC_EXT_USAGE_*; only the set
 * counters are read. */
typedef struct AgcExtUsage {
    uint32_t    struct_size;
    uint32_t    fields;
    uint32_t    input, output, cache_read, cache_write, reasoning;
} AgcExtUsage;

/* One parsed SSE event, borrowed for the stream_event call only. `data` is the
 * event payload (NUL-terminated, may contain '\n'); data_len is its length. */
typedef struct AgcExtWireEvent {
    uint32_t    struct_size;
    const char *event;             /* "" when the SSE event: field was absent */
    const char *data;
    size_t      data_len;
} AgcExtWireEvent;

/* Typed stream sink. Every call applies immediately to the core stream state;
 * payload pointers are borrowed for the call only (the sink copies them), so
 * the extension may not retain the JSON scratch. `st` is the AgcExtStream
 * passed to stream_event/stream_finish. */
typedef struct AgcExtStreamSink {
    uint32_t    struct_size;
    void (*text)(AgcExtStream *st, const char *p, size_t n);
    void (*thinking)(AgcExtStream *st, const char *p, size_t n);
    void (*tool_start)(AgcExtStream *st, const char *id, const char *name);
    void (*tool_args)(AgcExtStream *st, const char *p, size_t n);
    void (*response_id)(AgcExtStream *st, const char *id);
    void (*usage)(AgcExtStream *st, const AgcExtUsage *u);
    void (*stop)(AgcExtStream *st, int reason);   /* AGENTC_EXT_STOP_* */
    void (*error)(AgcExtStream *st, const char *message);
} AgcExtStreamSink;

/* A custom provider contributed during init. The host copies every
 * string and both arrays at the add_provider call, so the extension may release
 * its own storage immediately.
 *
 *   name              [a-z0-9_.:-]{1,64}; unique against presets and live rows
 *   default_base_url  http:// or https://; the core owns transport
 *   path              starts with '/', no CR/LF/space
 *   auth              NULL = AUTH_NONE; QUERY is rejected
 *   build_request     mandatory; write the header block into head_out and the
 *                     JSON body into body_out with host->out_write. The core
 *                     inserts the blank line, sanitizes both sinks, adds
 *                     Content-Length and the auth line, and owns retries,
 *                     timeouts, cancellation and --record/--dump-wire.
 *   stream_event      mandatory; maps one AgcExtWireEvent to sink calls.
 *   stream_open/_finish/_close  optional (NULL = stateless/default).
 *
 * head_out/body_out are opaque output sinks for host->out_write only. The view
 * and the wire event are borrowed for their call. add_provider failure is
 * logged and does not fail init. */
typedef struct AgcExtProvider {
    uint32_t    struct_size;
    const char *name;
    const char *label;
    const char *default_base_url;
    const char *path;
    const char *env_keys[3];
    int         needs_key;
    int         discover_style;    /* AGENTC_EXT_DISCOVER_* */
    const AgcExtProviderAuth  *auth;
    const AgcExtProviderModel *models;
    size_t      nmodels;
    void       *ud;
    int  (*build_request)(const AgcExtHost *, const struct AgcExtProvider *,
                          const AgcExtRequestView *, void *head_out, void *body_out);
    int  (*stream_open)(const AgcExtHost *, const struct AgcExtProvider *,
                        AgcExtStream *);
    int  (*stream_event)(const AgcExtHost *, const struct AgcExtProvider *, AgcExtStream *,
                         const AgcExtWireEvent *, const AgcExtStreamSink *);
    int  (*stream_finish)(const AgcExtHost *, const struct AgcExtProvider *, AgcExtStream *,
                          const AgcExtStreamSink *);
    void (*stream_close)(const AgcExtHost *, const struct AgcExtProvider *, AgcExtStream *);
} AgcExtProvider;

/* ------------------------------------------------------------------ host */

struct AgcExtHost {
    uint32_t abi_version;              /* == AGENTC_EXT_ABI */
    uint32_t struct_size;

    /* memory — alloc zeroes, free(NULL) is a no-op */
    void  *(*alloc)(size_t n);
    void   (*free)(void *p);
    void   (*log)(int level, const char *msg);      /* 0=debug 1=info 2=warn 3=error */

    /* output sink for tool run/command/section (opaque; never expose AgcBuf) */
    void   (*out_write)(void *out, const char *bytes, size_t n);

    /* contributions */
    void   (*add_tool)(const AgcExtTool *tool);
    void   (*add_command)(const AgcExtCommand *cmd);
    void   (*add_section)(const AgcExtSection *section);
    void   (*add_status)(AgcExtStatusProvider provider, void *ud);

    /* hooks */
    uint64_t (*on)(const char *point, uint32_t caps, int priority,
                   AgcExtHookFn fn, void *ud);      /* 0 = rejected */
    void     (*off)(uint64_t handle);
    void     (*emit)(const AgcExtHost *host, const char *point,
                     const char *payload_json, AgcExtResult *out);

    /* services */
    void   (*defer)(const AgcExtHost *host, void (*fn)(void *ud), void *ud);
    int    (*is_cancelled)(const AgcExtHost *host, const char *signal_token);
    const char *(*cwd)(const AgcExtHost *host);
    const char *(*session_id)(const AgcExtHost *host);
    const char *(*session_file)(const AgcExtHost *host);
    const char *(*system_prompt)(const AgcExtHost *host);
    void   (*append_entry)(const char *custom_type, const char *data_json);

    /* http_request: the wire layer is not resumable, so one queued request is
     * dispatched per pump call and may block that call for up to 10 seconds.
     * At most 8 requests may be queued. A forbidden header (Host,
     * Content-Length, Transfer-Encoding) or a malformed headers_json is
     * dropped with a log; body == NULL with body_len > 0 is rejected. */
    uint64_t (*http_request)(const AgcExtHost *host, const char *method,
                             const char *url, const char *headers_json,
                             const char *body, uint64_t body_len,
                             void (*cb)(void *ud, int status,
                                        const char *headers_json,
                                        const char *body, uint64_t body_len),
                             void *ud);
    /* http_cancel: a queued request is dropped before dispatch. A request
     * already inside agentc_http_run is interrupted on its next poll slice
     * (the wire layer polls the flag), so its callback receives the negative
     * -ECANCELED status and whatever partial body was decoded so far. */
    void   (*http_cancel)(const AgcExtHost *host, uint64_t request);

    void   (*notify)(const char *message, int level);
    /* set_status: auto-clearing status segment keyed by (owning extension,
     * key); 8 live keys process-wide, a ninth is logged and ignored. NULL or
     * empty `text` clears the key. */
    void   (*set_status)(const char *key, const char *text);
    /* set_model/set_thinking apply only when the front end installed a sink;
     * with no sink set_model fails -ENOSYS and set_thinking no-ops. */
    int    (*set_model)(const char *provider, const char *model);
    void   (*set_thinking)(const char *level);
    void   (*request_recompose)(const AgcExtHost *host);

    /*
     * Appended ergonomics carried over from the old ABI. They are not part of
     * the minimal surface but are freestanding JSON string helpers many small
     * extensions want; keeping them costs nothing and avoids forcing every C
     * author to write a parser.
     *
     * Lifetime: strdup_/json_escape return host memory the caller owns and
     * frees with host->free. json_get_str returns a host-owned copy valid only
     * until the next pump (or shutdown); do not free it and do not retain it
     * past the pump that produced it.
     */
    char  *(*strdup_)(const char *s);
    const char *(*json_get_str)(const char *json, const char *path,
                                const char *dflt);
    long long   (*json_get_int)(const char *json, const char *path,
                                long long dflt);
    int         (*json_get_bool)(const char *json, const char *path, int dflt);
    char       *(*json_escape)(const char *s);
    void        (*set_title)(const char *title);

    /* Register a custom provider. Guard with
     * AGENTC_EXT_HOST_HAS(host, add_provider). The contribution is validated
     * and copied; a failure is logged and does not fail init. */
    void        (*add_provider)(const AgcExtProvider *provider);
};

/* ------------------------------------------------------------- extension */

typedef struct AgcExt {
    uint32_t abi_version;              /* = AGENTC_EXT_ABI */
    uint32_t struct_size;              /* = sizeof(AgcExt) */
    const char *name;
    const char *version;
    int32_t     order;                 /* lower first; ties by registration order */
    int  (*init)(const AgcExtHost *host);
    void (*shutdown)(void);
    /* Appended after the ABI-1 baseline: minimum AgcExtHost.struct_size this
     * extension needs. 0 (a zeroed or older descriptor) means no requirement;
     * a value above the host's sizeof(AgcExtHost) is refused at registration
     * with a clear log. Check host services with AGENTC_EXT_HOST_HAS. */
    uint32_t required_host_size;
} AgcExt;

/* The only exported symbol. Must return 0 and fill *out. */
int agentc_ext_init(const AgcExtHost *host, AgcExt *out);

/* Hook-point names (pi's names; caps/policy/merge are fixed per point). */
#define AGENTC_HEV_PROJECT_TRUST          "project_trust"
#define AGENTC_HEV_RESOURCES_DISCOVER     "resources_discover"
#define AGENTC_HEV_SESSION_START          "session_start"
#define AGENTC_HEV_SESSION_SHUTDOWN       "session_shutdown"
#define AGENTC_HEV_INPUT                  "input"
#define AGENTC_HEV_BEFORE_AGENT_START     "before_agent_start"
#define AGENTC_HEV_AGENT_START            "agent_start"
#define AGENTC_HEV_AGENT_BEFORE_SETTLE    "agent_before_settle"
#define AGENTC_HEV_AGENT_SETTLED          "agent_settled"
#define AGENTC_HEV_AGENT_END              "agent_end"
#define AGENTC_HEV_TURN_START             "turn_start"
#define AGENTC_HEV_TURN_END               "turn_end"
#define AGENTC_HEV_MESSAGE_START          "message_start"
#define AGENTC_HEV_MESSAGE_UPDATE         "message_update"
#define AGENTC_HEV_MESSAGE_END            "message_end"
#define AGENTC_HEV_TOOL_CALL              "tool_call"
#define AGENTC_HEV_TOOL_RESULT            "tool_result"
#define AGENTC_HEV_TOOL_EXEC_START        "tool_execution_start"
#define AGENTC_HEV_TOOL_EXEC_END          "tool_execution_end"
#define AGENTC_HEV_BEFORE_PROVIDER_HEADERS "before_provider_headers"
#define AGENTC_HEV_BEFORE_PROVIDER_REQ    "before_provider_request"
#define AGENTC_HEV_AFTER_PROVIDER_RES     "after_provider_response"
#define AGENTC_HEV_PROVIDER_STREAM        "provider_stream_event"
#define AGENTC_HEV_SESSION_BEFORE_COMPACT "session_before_compact"
#define AGENTC_HEV_SESSION_COMPACT        "session_compact"
#define AGENTC_HEV_SESSION_COMPACT_FAILED "session_compact_failed"
#define AGENTC_HEV_MCP_SERVERS_CHANGE     "mcp_servers_change"
#define AGENTC_HEV_MODEL_SELECT           "model_select"
#define AGENTC_HEV_THINKING_SELECT        "thinking_level_select"

#ifdef __cplusplus
}
#endif

#endif /* AGENTC_EXT_H */
