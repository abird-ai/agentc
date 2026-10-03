/* limits.h — hard caps for every accumulator fed by a remote peer or file.
 *
 * One place to read, one place to change: the parsers and tools check these on
 * append and return a typed error instead of growing without bound. Several
 * caps are intentionally tighter than the corresponding peer or format
 * default, as noted at their definitions below.
 */
#ifndef AGENTC_BASE_LIMITS_H
#define AGENTC_BASE_LIMITS_H

/* --- SSE (wire/sse.c) --------------------------------------------------- */
#define AGENTC_LIMIT_SSE_EVENT_BYTES        (1024u * 1024u)   /* sse.c SSE_CAP */

/* --- JSON (base/json.c, wire/jsonw.c) ----------------------------------- */
/* Maximum container nesting the JSONC parser accepts and the streaming writer
 * can hold. One value so a document the parser accepts is always writable. */
#define AGENTC_JSON_MAX_DEPTH               200

/* --- HTTP (wire/http.c) ------------------------------------------------- */
#define AGENTC_LIMIT_HTTP_HEADER_BYTES      (64u * 1024u)     /* http.c H_MAX_HEAD */
#define AGENTC_LIMIT_HTTP_HEADER_LINES      128               /* http.c H_MAX_HEADERS */
#define AGENTC_LIMIT_HTTP_CHUNK_LINE_BYTES  8192              /* http.c H_MAX_CHUNK_LINE */

/* --- MCP (ext/mcp.c) ---------------------------------------------------- */
#define AGENTC_LIMIT_MCP_LINE_BYTES         (1u << 20)        /* mcp.c stdio line cap */
#define AGENTC_LIMIT_MCP_BODY_BYTES         (8u << 20)        /* mcp.c HTTP response body cap */
#define AGENTC_LIMIT_MCP_SCHEMA_BYTES       2048              /* mcp.c inputSchema copy cap */
#define AGENTC_LIMIT_MCP_PAGES              100               /* mcp.c MCP_MAX_PAGES */
/* MCP prompts: the live prompt table (per server and total), the
 * remote-name/description/argument shapes, the text returned by prompts/get
 * and the frozen pile of retired-but-listed entries (never freed or reused; a
 * full pile fails the server's prompt sync instead of recycling). */
#define AGENTC_LIMIT_MCP_PROMPTS_PER_SERVER 128
#define AGENTC_LIMIT_MCP_PROMPTS_TOTAL      512
#define AGENTC_LIMIT_MCP_PROMPT_NAME        256
#define AGENTC_LIMIT_MCP_PROMPT_DESC        2048
#define AGENTC_LIMIT_MCP_PROMPT_ARGS        16
#define AGENTC_LIMIT_MCP_ARG_NAME           64
#define AGENTC_LIMIT_MCP_PROMPT_TEXT_BYTES  (64u * 1024u)
#define AGENTC_LIMIT_MCP_PROMPTS_FROZEN_CAP 128
/* MCP resources: the live resource/template tables (per server and
 * total), the remote field shapes, the JSON text one list tool page may
 * return and the text one resources/read may return. Over-long name/desc/
 * title/mime fields are truncated; an over-long uri/uriTemplate is skipped
 * (a clipped address would read the wrong resource). */
#define AGENTC_LIMIT_MCP_RESOURCES_PER_SERVER 256
#define AGENTC_LIMIT_MCP_RESOURCES_TOTAL      1024
#define AGENTC_LIMIT_MCP_RESOURCE_URI         2048
#define AGENTC_LIMIT_MCP_RESOURCE_NAME        256
#define AGENTC_LIMIT_MCP_RESOURCE_DESC        2048
#define AGENTC_LIMIT_MCP_RESOURCE_MIME        128
#define AGENTC_LIMIT_MCP_RESOURCES_JSON_BYTES (64u * 1024u)
#define AGENTC_LIMIT_MCP_RESOURCE_TEXT_BYTES  (256u * 1024u)
/* MCP reconnect: a FAILED server rearms after a capped exponential
 * backoff. AGENTC_MCP_RETRY_MS overrides the base delay (0 disables reconnect;
 * positive values are clamped to AGENTC_MCP_RETRY_MAX_MS). The base is doubled
 * per consecutive failure up to 2^6, then capped at the max. */
#define AGENTC_MCP_RETRY_BASE_MS            1000
#define AGENTC_MCP_RETRY_MAX_MS             60000

/* --- tools (core/tools) ------------------------------------------------- */
#define AGENTC_LIMIT_TOOL_BYTES             50000             /* bash.c OUT_MAX_BYTES; read.c READ_MAX_BYTES */
#define AGENTC_LIMIT_TOOL_LINES             2000              /* bash.c OUT_MAX_LINES; read.c READ_MAX_LINES */
#define AGENTC_LIMIT_READ_BYTES             (64u << 20)       /* read.c READ_MAX_FILE */
#define AGENTC_LIMIT_EDIT_BYTES             (8u << 20)        /* edit.c EDIT_MAX_BYTES */
/* Initial JSON-arena block for tool arguments and the rg line scratch. Small
 * on purpose: the arena doubles on demand, and a 1 MiB default would mmap per
 * tool call (agentc_alloc_try > MEM_MAXSMALL goes to os_map). */
#define AGENTC_LIMIT_TOOL_ARENA             16384

/* grep/find budgets. Shared by the built-in scanners and the external rungs
 * (core/tools/engine.c) so the two backends cannot drift: the same default
 * caps, the same hard ceilings and the same per-line truncation. */
#define AGENTC_LIMIT_GREP_LINE              500               /* grep.c GREP_MAX_LINE: per-match line cap */
#define AGENTC_LIMIT_GREP_MATCHES           100               /* grep.c GREP_DEFAULT_LIMIT */
#define AGENTC_LIMIT_GREP_MATCHES_MAX       100000            /* grep.c hard ceiling */
#define AGENTC_LIMIT_GREP_CONTEXT           1000              /* grep.c GREP_MAX_CONTEXT */
#define AGENTC_LIMIT_GREP_FILE              (16u << 20)       /* grep.c GREP_MAX_FILE: per-file scan cap */
#define AGENTC_LIMIT_GREP_PENDING           (16u << 20)       /* one grep stdout line before it is dropped (rg JSON carries the whole line) */
#define AGENTC_LIMIT_FIND_PATH              4096              /* one find output path before it is dropped */
#define AGENTC_LIMIT_FIND_PATHS             1000              /* find.c FIND_DEFAULT_LIMIT */
#define AGENTC_LIMIT_FIND_PATHS_MAX         100000            /* find.c hard ceiling */
#define AGENTC_LIMIT_FIND_ENTRIES           8000              /* find.c TREE_MAX_ENTRIES (built-in only) */

/* --- RPC (app/mode_rpc.c) ----------------------------------------------- */
#define AGENTC_LIMIT_RPC_LINE_BYTES         (1u << 20)        /* mode_rpc.c RPC_MAX_LINE */

/* --- resources (core/resources.c, core/prompts.c, app/project.c) -------- */
/* Extra roots contributed by resources_discover; one store per kind, deduped. */
#define AGENTC_RESOURCE_ROOTS_MAX           8
/* Paths consumed from one resources_discover list before the rest are logged
 * and ignored, so a hostile hook cannot burn the startup budget. */
#define AGENTC_RESOURCE_PATHS_MAX           32
/* Invocable prompt records (built-in/file/extension/MCP). Records are never
 * freed once registered; the cap is on total registrations. */
#define AGENTC_PROMPTS_MAX                  512
/* File prompt templates and named themes loaded per scan. */
#define AGENTC_TEMPLATES_MAX                64
#define AGENTC_THEMES_MAX                   64
/* Prompt/template names: [a-z0-9._:-]{1,64}; theme stems: [A-Za-z0-9._-]{1,64}. */
#define AGENTC_PROMPT_NAME_MAX              64
#define AGENTC_THEME_NAME_MAX               64
/* `/skill:<name>` submits at most this many bytes of stripped SKILL.md body. */
#define AGENTC_SKILL_SUBMIT_MAX             (256u * 1024u)

#endif /* AGENTC_BASE_LIMITS_H */
