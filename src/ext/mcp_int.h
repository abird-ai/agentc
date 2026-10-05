/* mcp_int.h — private interfaces shared by src/ext/mcp.c and tests/mcp_test.c.
 *
 * Not part of the frozen public API: everything here may change. The public
 * contract is include/mcp.h. Tests link mcp.c directly, so these hooks keep the
 * config parser, JSON-RPC framing, tool-name mapping and flag mapping
 * testable without spawning processes or touching the network.
 */
#ifndef AGENTC_MCP_INT_H
#define AGENTC_MCP_INT_H

#include "agentc.h"
#include "mcp.h"
#include "plat.h"
#include "wire.h"
#include "base/limits.h"

/* Shared core helpers (defined in src/core/config.c; other core modules
 * declare them extern as well). */
char *agentc_read_file_owned(const char *path, size_t *out_len);
const char *agentc_env_get(const char *name);

/* Registry entries are stable heap blocks (McpToolEnt*); AgcTool.ud points
 * at the entry, so one mcp_run() executes every exposed tool. */

/* ---------------------------------------------------------------- config */
typedef struct {
    char *name;         /* server key, owned */
    char *command;      /* stdio, ${ENV}-expanded, owned */
    AgcVec args;         /* char* strings, expanded, owned */
    AgcVec env;          /* "KEY=VALUE" strings, expanded, owned */
    char *url;          /* HTTP, expanded, owned */
    char *headers;      /* "Name: value\r\n" lines, expanded, owned */
    bool enabled;
    i64 timeout_ms;     /* default 60000 */
} McpCfg;

/* Parse a JSONC document. Appends zero or more servers to `out`. Returns 0
 * (including "no servers key" and malformed input is -EINVAL). */
int agentc_mcp_cfg_parse(const char *text, size_t n, AgcVec *out);
void agentc_mcp_cfgs_free(AgcVec *v);

/* ${NAME} expansion; unset variables expand to "". Returns owned. */
char *agentc_mcp_expand_env(const char *s);

/* ------------------------------------------------------------- tool names */
/* Lowercases A-Z, keeps [a-z0-9_-], maps everything else to '_'. */
void agentc_mcp_name_sanitize(const char *s, size_t n, char *out, size_t cap);
/* FNV-1a 32 over server + 0x1f + tool (collision suffix). */
u32 agentc_mcp_name_hash(const char *server, const char *tool);
/* "mcp__<server>__<tool>" sanitized, truncated to cap. */
void agentc_mcp_tool_name(const char *server, const char *tool, char *out, size_t cap);
u32 agentc_mcp_flags_from_hints(bool read_only, bool destructive);

/* ---------------------------------------------------------- capabilities */
/* Per-server capability bits parsed from the initialize result.capabilities
 * object. Missing or malformed capabilities fall back to MCP_CAP_TOOLS (the
 * default behavior, when every server was assumed to speak tools/list); a
 * present-but-empty object falls back the same way. The *_LIST_CHANGED bits
 * are recorded for diagnostics/future gating; a list_changed notification is
 * honored while the kind's base bit is advertised. */
#define MCP_CAP_TOOLS                  0x01u
#define MCP_CAP_PROMPTS                0x02u
#define MCP_CAP_RESOURCES              0x04u
#define MCP_CAP_TOOLS_LIST_CHANGED     0x10u
#define MCP_CAP_PROMPTS_LIST_CHANGED   0x20u
#define MCP_CAP_RESOURCES_LIST_CHANGED 0x40u

u32 agentc_mcp_caps_parse(const char *json, size_t n);

/* ------------------------------------------------------------ json-rpc */
typedef struct {
    /* >0 bytes, 0 eof, -EAGAIN, other -errno. */
    int (*read)(void *ud, void *p, size_t n);
    /* Wait up to timeout_ms for readability; 0 = retried, -ECANCELED/-errno. */
    int (*wait)(void *ud, int timeout_ms);
    void *ud;
} McpReader;

char *agentc_mcp_rpc_request(u64 id, const char *method, const char *params_json);
char *agentc_mcp_rpc_notify(const char *method, const char *params_json);

/* Consume one complete newline-delimited message from `in`.
 * 1 = *out set (owned), 0 = need more bytes, -E2BIG = line too long. */
int agentc_mcp_stdio_next(AgcBuf *in, char **out);

/* Wait for the response with `id` using `rd`. Notifications (no id) are passed
 * to on_notify; messages for other ids are dropped. On success *out is an
 * owned copy of the matching JSON-RPC message (result or error object).
 * Returns 0 | -ETIMEDOUT | -ECONNRESET | -ECANCELED | -errno. */
int agentc_mcp_rpc_await(McpReader *rd, AgcBuf *in, u64 id, int timeout_ms,
                     const volatile bool *cancel,
                     void (*on_notify)(void *ud, const char *json, size_t n),
                     void *nud, char **out);

/* Bounded, poll-driven stdio write used by the transport (tests link mcp.c
 * directly). Returns 0 | -ETIMEDOUT | -ECANCELED | -EPIPE | -errno. When the
 * caller passes `written`, it receives how many bytes reached the pipe before a
 * failure, so a partial write can be distinguished from a clean abort. */
int agentc_mcp_write_all(int fd, int pid, const void *p, size_t n, int timeout_ms,
                         const volatile bool *cancel, size_t *written);

/* Reconnect backoff delay for a server that has already failed `retry_count`
 * times: base << min(retry_count, 6), capped at AGENTC_MCP_RETRY_MAX_MS.
 * Returns 0 when AGENTC_MCP_RETRY_MS=0 disables reconnect. Exposed for tests;
 * the state machine uses it to arm the FAILED -> NEW rearm. */
int agentc_mcp_retry_delay_ms(int retry_count);

/* ------------------------------------------------------------- tools */
typedef struct {
    char *name;         /* remote tool name, owned */
    char *desc;         /* owned */
    char *schema;       /* raw inputSchema JSON span, owned or NULL */
    u32 flags;
} McpParsedTool;

/* Parse a tools/list JSON-RPC message into McpParsedTool entries.
 * `next_cursor` receives an owned cursor or NULL. 0 | -EPROTO | -EIO. */
int agentc_mcp_parse_tools(const char *json, size_t n, AgcVec *out, char **next_cursor);
void agentc_mcp_parsed_free(AgcVec *v);

/* Parse a tools/call JSON-RPC message. On success *text is owned tool output
 * ("" when the server returned no content) and *is_error maps result.isError
 * or a JSON-RPC error object. 0 | -EPROTO | -EIO. */
int agentc_mcp_parse_call(const char *json, size_t n, char **text, bool *is_error);

/* ------------------------------------------------------------- prompts */
typedef struct {
    char *name;         /* argument name, owned */
    char *description;  /* owned, "" when absent */
    bool required;
} McpParsedArg;

typedef struct {
    char *name;         /* remote prompt name, owned */
    char *title;        /* owned, "" when absent */
    char *description;  /* owned, "" when absent */
    McpParsedArg args[AGENTC_LIMIT_MCP_PROMPT_ARGS];
    size_t nargs;
} McpParsedPrompt;

/* Parse a prompts/list JSON-RPC message into McpParsedPrompt entries (bounded
 * by AGENTC_LIMIT_MCP_PROMPT_ARGS per prompt; over-long names are skipped,
 * descriptions/titles truncated). `next_cursor` receives an owned cursor or
 * NULL. 0 | -EPROTO | -EIO. */
int agentc_mcp_parse_prompts(const char *json, size_t n, AgcVec *out, char **next_cursor);
void agentc_mcp_parsed_prompts_free(AgcVec *v);

/* Parse a prompts/get JSON-RPC message. Text content blocks are joined with
 * "\n\n"; non-text blocks are skipped (logged) and an error object or missing
 * result is -EIO. Over AGENTC_LIMIT_MCP_PROMPT_TEXT_BYTES returns -E2BIG with
 * *text NULL (never a truncated string). 0 | -EPROTO | -EIO | -E2BIG. */
int agentc_mcp_parse_prompt_get(const char *json, size_t n, char **text);

/* ----------------------------------------------------------- resources */
/* The parser-level blob rejection code: resources/read returned a `blob`
 * entry, which this client does not consume. mcp_err_name() maps it to
 * "binary resource not supported". */
#define MCP_ERR_BINARY (-95)   /* -EOPNOTSUPP */

typedef struct {
    char *uri;         /* owned; over-long uris are skipped, never clipped */
    char *name;        /* owned, "" when absent */
    char *title;       /* owned, "" when absent */
    char *description; /* owned, "" when absent */
    char *mime;        /* mimeType, owned, "" when absent */
} McpParsedResource;

typedef struct {
    char *uri_template; /* uriTemplate, owned */
    char *name;         /* owned, "" when absent */
    char *description;  /* owned, "" when absent */
    char *mime;         /* mimeType, owned, "" when absent */
} McpParsedResourceTemplate;

/* Parse resources/list (uri/name/title/description/mimeType) and
 * resources/templates/list (uriTemplate/name/description/mimeType) JSON-RPC
 * messages. Entries without a uri (uriTemplate) or name are skipped, an
 * over-long uri is skipped and the other fields are truncated to their caps
 * with a log. `next_cursor` receives an owned cursor or NULL.
 * 0 | -EINVAL | -EPROTO | -EIO. */
int agentc_mcp_parse_resources(const char *json, size_t n, AgcVec *out, char **next_cursor);
void agentc_mcp_parsed_resources_free(AgcVec *v);
int agentc_mcp_parse_resource_templates(const char *json, size_t n, AgcVec *out,
                                        char **next_cursor);
void agentc_mcp_parsed_resource_templates_free(AgcVec *v);

/* Parse a resources/read JSON-RPC message ({contents:[{uri,mimeType?,text?|
 * blob?}]}). Text entries are joined with "\n\n"; a `blob` entry rejects the
 * whole read with MCP_ERR_BINARY. Over AGENTC_LIMIT_MCP_RESOURCE_TEXT_BYTES
 * is -E2BIG and *text stays NULL (never truncated).
 * 0 | -EPROTO | -EIO | -E2BIG | MCP_ERR_BINARY. */
int agentc_mcp_parse_resource_read(const char *json, size_t n, char **text);

/* ------------------------------------------------------------- registry */
/* Free every discovered MCP tool (shutdown calls this). */
void agentc_mcp_registry_reset(void);
/* Register/update a tool; computes the exposed, collision-free name. */
int agentc_mcp_registry_add(const char *server, const char *tool, const char *desc,
                        const char *schema_json, u32 flags);

/* Commit one completed (paginated) prompts/list for `server`: mark its live
 * entries unseen, create/update the parsed ones, then retire unseen entries
 * by exposed name. A changed or retired entry that was ever listed is frozen
 * (alive=false) and never reused; a full frozen pile or a full total/per-server
 * budget fails the commit (-EFROZEN / -ENOSPC). Returns 0 | -EINVAL | -ENOSPC |
 * -EFROZEN. Exposed for tests; the sync path calls it. */
int agentc_mcp_prompt_commit(const char *server, const AgcVec *parsed);
/* Live MCP prompt entries (tests). */
size_t agentc_mcp_prompt_count(void);
/* Exposed `mcp__<server>__<prompt>` name of a live entry (tests). */
bool agentc_mcp_prompt_exposed(const char *server, const char *prompt, char *out, size_t cap);
/* Retire every live MCP prompt record and free the table (shutdown calls this). */
void agentc_mcp_prompt_registry_reset(void);
/* AgcPromptExpandFn for one McpPromptEnt (registered with the core prompt
 * registry). Bounded synchronous prompts/get; NULL on any failure. */
char *agentc_mcp_prompt_expand(void *ud, const char *args);

/* Replace one server's live resource/template table with a committed list
 * (validated against the per-server/total caps first; the previous table
 * survives a cap refusal). `templates` selects the resources/templates/list
 * table. Exposed for tests; the sync path calls it.
 * 0 | -EINVAL | -ENOSPC. */
int agentc_mcp_resource_commit(const char *server, const AgcVec *parsed, bool templates);
/* Live resource/template entry count (tests). */
size_t agentc_mcp_resource_count(bool templates);
/* Retire every live resource/template entry and free both tables. */
void agentc_mcp_resource_registry_reset(void);
/* Build one page of the local resource/template table as the JSON text the
 * mcp_list_resources / mcp_list_resource_templates tools return. `server`
 * filters one server; NULL merges every table entry. `cursor` is the decimal
 * offset into the filtered sequence (NULL/"" = 0); when entries remain after
 * the page a "nextCursor" field carries the next offset. The page is capped
 * at AGENTC_LIMIT_MCP_RESOURCES_JSON_BYTES; a single entry that cannot fit an
 * otherwise empty page is -E2BIG. This entry point is the pure table view
 * (tests); the tool path first requires a live resource-capable server and
 * then filters non-live servers out of a merged page.
 * 0 | -EINVAL | -E2BIG. */
int agentc_mcp_resource_list_json(bool templates, const char *server, const char *cursor,
                                  char **out);

#endif /* AGENTC_MCP_INT_H */
