/* mcp.h — Model Context Protocol client (stdio + Streamable HTTP).
 *
 * Configuration, JSONC:
 *   ~/.config/agentc/mcp.jsonc  and  <cwd>/.agentc/mcp.jsonc (trusted project only)
 *   {
 *     "servers": {
 *       "files":  { "command": "npx", "args": ["-y", "@scope/server"],
 *                   "env": {"TOKEN": "${TOKEN}"}, "enabled": true },
 *       "remote": { "url": "https://example.com/mcp",
 *                   "headers": {"Authorization": "Bearer ${TOKEN}"} }
 *     }
 *   }
 * `${ENV}` expands from the environment. The mcp extension registers a pump;
 * its first invocation reads the config and calls agentc_mcp_start() with the
 * context installed by agentc_mcp_set_context(), and every later pump
 * advances one server. Each connect is bounded by
 * AGENTC_MCP_CONNECT_TIMEOUT_MS. tools are exposed as
 * `mcp__<server>__<tool>`, sanitized to [a-z0-9_-]. Server tools follow the
 * MCP annotation defaults: readOnlyHint true wins, otherwise a missing
 * destructiveHint means destructive (explicit false is additive).
 *
 * Capability negotiation: the initialize response's
 * result.capabilities decides which lists are fetched (tools, prompts,
 * resources and resource templates, in that order) and which
 * notifications/<kind>/list_changed methods trigger a re-sync; a server that
 * never advertises `resources` is never asked for either resource list.
 * Missing, malformed or empty capabilities fall back to tools-only. Prompts
 * are exposed as `mcp__<server>__<prompt>` in the core prompt registry
 * (src/core/prompts.h); invoking one performs a bounded synchronous
 * `prompts/get` and returns the joined text blocks. Resources are fetched
 * into per-server tables (bounded) and exposed through three generic
 * read-only tools published at the first successful resource list sync:
 * `mcp_list_resources` and `mcp_list_resource_templates` return paged JSON
 * text, `mcp_read_resource {server,uri}` performs a bounded synchronous
 * `resources/read` and returns text contents only (a `blob` entry is rejected
 * as binary).
 *
 * Hardening: a READY stdio server is drained on every pump, so an idle
 * `notifications/<kind>/list_changed` (or any other notification) is honored
 * without waiting for the next exchange; at most 64 complete messages are
 * consumed per pump and leftovers stay buffered. EOF from an idle server fails
 * it, which arms the reconnect below. HTTP servers have no idle channel: an
 * HTTP response is scoped to a request, so a stream notification can only be
 * seen inside the request that carries it. A FAILED server is reconnected with
 * a capped exponential backoff (AGENTC_MCP_RETRY_BASE_MS, doubled per
 * consecutive failure, capped at AGENTC_MCP_RETRY_MAX_MS); the base delay can
 * be overridden with AGENTC_MCP_RETRY_MS (0 disables reconnect entirely). A
 * FAILED server is not pending while it waits, so the startup pump budget is
 * unaffected. The `mcp_servers_change` extension hook receives
 * `{"servers":[{"name","kind","status","caps"?}]}` with status one of
 * `connecting`/`ready`/`failed` and caps present only after initialize. */
#ifndef AGENTC_MCP_H
#define AGENTC_MCP_H

#include "agentc.h"
#include "agent.h"

#define AGENTC_MCP_CONNECT_TIMEOUT_MS 10000

/* Load config and connect every enabled server synchronously. Returns the
 * number of servers connected (0 when no config exists). `trusted` gates the
 * project file. This is the legacy test/back-compat helper; the app uses
 * agentc_mcp_start() + the registry pump. */
size_t agentc_mcp_load(const char *cwd, bool trusted);
/* cwd/trusted for the mcp extension. The app calls this twice: a provisional
 * (false) before agentc_ext_load_all, then the final value after trust is
 * resolved and before the first registry pump (which is when the extension
 * actually reads the config and creates the server records). */
void agentc_mcp_set_context(const char *cwd, bool trusted);

/* Async surface. `start` reads config and creates the server records, then
 * returns without connecting; the mcp extension calls it from its first pump
 * (never from init, so trust ordering is settled first). `pump` advances one
 * server's connect/re-sync state machine by one bounded step per call,
 * rotating round-robin, so a call never blocks the main loop for more than
 * one MCP step slice; the registry calls it on every agentc_ext_pump().
 * `tools` is a pure snapshot of what is connected now. */
void agentc_mcp_start(const char *cwd, bool trusted);
void agentc_mcp_pump(void);
/* Append discovered tools to out (max entries). Returns the count appended.
 *
 * Lifetime: the returned AgcTool views (and their name/desc/params strings and
 * `ud`) are borrowed from the live server table. They stay valid until the next
 * agentc_mcp_shutdown()/agentc_mcp_start() reload, which frees that table; a
 * view must not be executed afterwards. The agent does not use this API (it
 * composes tools through the extension registry, whose internal_tool_run
 * trampoline resolves a retired tool to a clean error); it exists for embedders
 * that snapshot the live list. */
size_t agentc_mcp_tools(AgcTool *out, size_t max);
/* Number of server records created by start()/the first pump. A record stays
 * counted after it reaches a terminal state; this is the configured-server
 * count, not a readiness count. */
size_t agentc_mcp_server_count(void);
/* True while any server record is still working through its connect/re-sync
 * state machine (not READY and not FAILED). False when no record exists yet or
 * every record is terminal; the app's startup pump uses this to stop as soon as
 * a fast server has settled instead of spending the whole budget. A FAILED
 * server stays non-pending while it waits for its reconnect backoff, so the
 * budget is unaffected; the pump that rearms it makes it pending again. */
bool agentc_mcp_pending(void);
/* Shut down all servers (stdin close -> SIGTERM -> SIGKILL). Idempotent. */
void agentc_mcp_shutdown(void);

#endif /* AGENTC_MCP_H */
