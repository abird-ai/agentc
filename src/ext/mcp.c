/* mcp.c — Model Context Protocol client (stdio + Streamable HTTP).
 *
 * Implements include/mcp.h. Design notes:
 *
 *   - Config is JSONC, parsed with agentc_json_parse_in into per-call arenas,
 *     plus a small raw-text scanner for object iteration. Each callback parses
 *     into a scratch arena so a sub-document that is still being scanned is
 *     never cleared by a nested parse.
 *   - stdio servers are spawned with the process environment plus the
 *     configured "env" overrides (${ENV} already expanded at parse time);
 *     requests are newline-delimited JSON-RPC 2.0.
 *   - HTTP servers use the wire layer: POST with Content-Type/Accept, either
 *     an application/json body or an SSE stream (the JSON-RPC message with the
 *     matching id is taken; reading stops as soon as it arrives).
 *   - initialize result.capabilities decides which lists a server speaks
 *     (tools, prompts, resources, resource templates, in that order) and
 *     whether a list_changed notification is honored for that kind. Missing,
 *     malformed or empty capabilities fall back to tools-only. A server that
 *     never advertises `resources` is never asked for either resource list.
 *   - Registry entries are stable heap blocks (McpToolEnt / McpPromptEnt);
 *     AgcTool.ud and the core prompt registry's expand userdata point at them.
 *     An entry that was ever handed out/listed is never freed or reused; a
 *     stale copy resolves to "no longer available". Resource/template rows
 *     are never handed out: the two generic list tools render them on demand,
 *     so a commit simply replaces one server's rows.
 *   - All memory is owned by the module and released by agentc_mcp_shutdown();
 *     load => tools => shutdown cycles return agentc_mem_live() to its baseline
 *     (core prompt registry records are permanent by design).
 *   - Hardening: READY stdio servers are idle-drained (bounded) so
 *     list_changed notifications do not wait for the next exchange; FAILED
 *     servers reconnect behind a capped exponential backoff; mcp_servers_change
 *     carries per-server status and (post-initialize) caps.
 */
#include "ext/mcp_int.h"
#include "base/limits.h"
#include "core/prompts.h"
#include "ext.h"
#include "ext/registry_int.h"

#define MC_EINVAL   (-22)
#define MC_ENOENT   (-2)
#define MC_EEXIST   (-17)
#define MC_EIO      (-5)
#define MC_E2BIG    (-7)
#define MC_EAGAIN   (-11)
#define MC_EPIPE    (-32)
#define MC_EINTR    (-4)
#define MC_ENOSPC   (-28)
#define MC_EFROZEN  (-31)   /* frozen retired-tool cap reached */
#define MC_EPROTO   (-71)
#define MC_ECONNRESET (-104)
#define MC_ETIMEDOUT (-110)
#define MC_ECANCELED (-125)

#define MCP_DEFAULT_TIMEOUT_MS 60000
#define MCP_CONNECT_TIMEOUT_MS 10000
/* Ceiling for a configured per-request timeout: 24h in ms. Stays well under
 * INT_MAX so the (int) casts and the timeout_ms*1000000 deadline math cannot
 * overflow or truncate. */
#define MCP_MAX_TIMEOUT_MS (24 * 60 * 60 * 1000)

/* Bounded tool table: a hostile or buggy server cannot pin memory with an
 * unbounded tools/list, and a list_changed that churns identities cannot grow
 * the frozen (retired-but-referenced) set forever. An entry that was ever
 * handed out is never freed or reused; `MCP_FROZEN_CAP` bounds how many such
 * retired entries may accumulate. At the cap a new identity is refused and
 * mcp_sync_commit fails the offending server (logged), never recycles. */
#define MCP_MAX_TOOLS_PER_SERVER 256
#define MCP_MAX_TOOLS_TOTAL      1024
#define MCP_MAX_TOOL_NAME        256
#define MCP_MAX_TOOL_DESC        8192
#define MCP_FROZEN_CAP           256
/* The extension registry accepts names up to 64 bytes (ext_valid_name), so
 * published MCP names are built to fit from the start. */
#define MCP_EXPOSED_NAME_MAX     64

/* MCP prompt bounds (values live in src/base/limits.h). The frozen pile holds
 * retired-but-listed prompt entries; like tools, they are never freed or
 * reused while a TUI menu may still reference the core registry record. */
#define MCP_MAX_PROMPTS_PER_SERVER AGENTC_LIMIT_MCP_PROMPTS_PER_SERVER
#define MCP_MAX_PROMPTS_TOTAL      AGENTC_LIMIT_MCP_PROMPTS_TOTAL
#define MCP_MAX_PROMPT_NAME        AGENTC_LIMIT_MCP_PROMPT_NAME
#define MCP_MAX_PROMPT_DESC        AGENTC_LIMIT_MCP_PROMPT_DESC
#define MCP_MAX_PROMPT_ARGS        AGENTC_LIMIT_MCP_PROMPT_ARGS
#define MCP_MAX_ARG_NAME           AGENTC_LIMIT_MCP_ARG_NAME
#define MCP_PROMPTS_FROZEN_CAP     AGENTC_LIMIT_MCP_PROMPTS_FROZEN_CAP
/* prompts/get is synchronous from the TUI (accepted UI bound). */
#define MCP_PROMPT_TIMEOUT_MS      10000

/* =========================================================================
 * Raw JSON span scanner (comments + nesting + escapes; no allocation).
 * ======================================================================= */
typedef struct {
    const char *p, *end;
} JSpan;

typedef int (*JPairFn)(void *ud, const char *key, size_t klen, JSpan val);
typedef int (*JValFn)(void *ud, JSpan val);

static bool j_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static void j_skip(const char **pp, const char *end) {
    const char *p = *pp;
    for (;;) {
        while (p < end && j_ws(*p)) p++;
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            p += 2;
            while (p < end && *p != '\n') p++;
            continue;
        }
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
            p = (p + 1 < end) ? p + 2 : end;
            continue;
        }
        break;
    }
    *pp = p;
}

static const char *j_string(const char *p, const char *end) {
    if (p >= end || *p != '"') return NULL;
    p++;
    while (p < end) {
        if (*p == '\\') {
            if (p + 1 >= end) return NULL;
            p += 2;
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return NULL;
}

static const char *j_value(const char *p, const char *end) {
    j_skip(&p, end);
    if (p >= end) return NULL;
    if (*p == '"') return j_string(p, end);
    if (*p == '{' || *p == '[') {
        char open = *p, close = (*p == '{') ? '}' : ']';
        int depth = 0;
        while (p < end) {
            if (*p == '"') {
                p = j_string(p, end);
                if (!p) return NULL;
                continue;
            }
            if (p + 1 < end && p[0] == '/' && (p[1] == '/' || p[1] == '*')) {
                j_skip(&p, end);
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return NULL;
    }
    const char *s = p;
    while (p < end && *p != ',' && *p != '}' && *p != ']' && !j_ws(*p)) p++;
    return p > s ? p : NULL;
}

/* Top-level member `key` of the object at [p,end). */
static bool j_find(const char *p, const char *end, const char *key, JSpan *out) {
    j_skip(&p, end);
    if (p >= end || *p != '{') return false;
    p++;
    size_t klen = agentc_strlen(key);
    for (;;) {
        j_skip(&p, end);
        if (p >= end || *p == '}') return false;
        if (*p != '"') return false;
        const char *ks = p + 1;
        const char *ke = j_string(p, end);
        if (!ke) return false;
        size_t this_len = (size_t)(ke - 1 - ks);
        p = ke;
        j_skip(&p, end);
        if (p >= end || *p != ':') return false;
        p++;
        j_skip(&p, end);
        const char *vs = p;
        const char *ve = j_value(p, end);
        if (!ve) return false;
        if (this_len == klen && agentc_memeq(ks, key, klen)) {
            out->p = vs;
            out->end = ve;
            return true;
        }
        p = ve;
        j_skip(&p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        return false;
    }
}

static int j_foreach_pair(const char *p, const char *end, JPairFn cb, void *ud) {
    j_skip(&p, end);
    if (p >= end || *p != '{') return MC_EPROTO;
    p++;
    for (;;) {
        j_skip(&p, end);
        if (p >= end) return MC_EPROTO;
        if (*p == '}') return 0;
        if (*p != '"') return MC_EPROTO;
        const char *ks = p + 1;
        const char *ke = j_string(p, end);
        if (!ke) return MC_EPROTO;
        size_t klen = (size_t)(ke - 1 - ks);
        p = ke;
        j_skip(&p, end);
        if (p >= end || *p != ':') return MC_EPROTO;
        p++;
        j_skip(&p, end);
        const char *vs = p;
        const char *ve = j_value(p, end);
        if (!ve) return MC_EPROTO;
        int rc = cb(ud, ks, klen, (JSpan){ vs, ve });
        if (rc != 0) return rc;
        p = ve;
        j_skip(&p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        if (p < end && *p == '}') return 0;
        return MC_EPROTO;
    }
}

static int j_foreach_elem(const char *p, const char *end, JValFn cb, void *ud) {
    j_skip(&p, end);
    if (p >= end || *p != '[') return MC_EPROTO;
    p++;
    for (;;) {
        j_skip(&p, end);
        if (p >= end) return MC_EPROTO;
        if (*p == ']') return 0;
        const char *vs = p;
        const char *ve = j_value(p, end);
        if (!ve) return MC_EPROTO;
        int rc = cb(ud, (JSpan){ vs, ve });
        if (rc != 0) return rc;
        p = ve;
        j_skip(&p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        if (p < end && *p == ']') return 0;
        return MC_EPROTO;
    }
}

/* =========================================================================
 * ${ENV} expansion
 * ======================================================================= */
char *agentc_mcp_expand_env(const char *s) {
    if (!s) return agentc_strdup_len("", 0);
    AgcBuf b = { 0 };
    for (const char *p = s; *p;) {
        if (p[0] == '$' && p[1] == '{') {
            const char *q = p + 2;
            const char *e = q;
            while ((*e >= 'A' && *e <= 'Z') || (*e >= 'a' && *e <= 'z') ||
                   (*e >= '0' && *e <= '9') || *e == '_')
                e++;
            if (e > q && *e == '}') {
                char name[160];
                size_t nl = (size_t)(e - q);
                if (nl >= sizeof name) nl = sizeof name - 1;
                agentc_memcpy(name, q, nl);
                name[nl] = 0;
                const char *v = agentc_env_get(name);
                if (v) agentc_buf_cstr(&b, v);
                p = e + 1;
                continue;
            }
        }
        agentc_buf_byte(&b, (u8)*p++);
    }
    if (!b.p) return agentc_strdup_len("", 0);
    return (char *)b.p;
}

/* =========================================================================
 * Config
 * ======================================================================= */
static void mcp_cfg_free_item(McpCfg *c) {
    if (!c) return;
    agentc_free(c->name);
    agentc_free(c->command);
    for (size_t i = 0; i < c->args.len; i++) agentc_free(((char **)c->args.p)[i]);
    agentc_vec_free(&c->args);
    for (size_t i = 0; i < c->env.len; i++) agentc_free(((char **)c->env.p)[i]);
    agentc_vec_free(&c->env);
    agentc_free(c->url);
    agentc_free(c->headers);
    agentc_memset(c, 0, sizeof *c);
}

void agentc_mcp_cfgs_free(AgcVec *v) {
    if (!v) return;
    for (size_t i = 0; i < v->len; i++) mcp_cfg_free_item(&((McpCfg *)v->p)[i]);
    agentc_vec_free(v);
}

typedef struct {
    McpCfg *c;
    AgcJsonArena *a;   /* value scratch; the server sub-doc stays live */
} EnvCtx;

typedef struct {
    AgcBuf *b;
    AgcJsonArena *a;   /* value scratch; the server sub-doc stays live */
} HeaderCtx;

typedef struct {
    AgcVec *out;
    AgcJsonArena *server;   /* one server object at a time */
    AgcJsonArena *value;    /* env/header values; distinct from `server` */
} CfgCtx;

static int cfg_env_cb(void *ud, const char *k, size_t klen, JSpan v) {
    EnvCtx *e = ud;
    McpCfg *c = e->c;
    if (klen == 0 || v.p >= v.end) return 0;
    AgcJson *sv = agentc_json_parse_in(e->a, v.p, (size_t)(v.end - v.p));
    size_t vlen = 0;
    const char *val = agentc_json_str(sv, &vlen);
    if (!val) return 0;
    char *exp = agentc_mcp_expand_env(val);
    size_t elen = agentc_strlen(exp);
    char *pair = agentc_alloc(klen + 1 + elen + 1);
    agentc_memcpy(pair, k, klen);
    pair[klen] = '=';
    agentc_memcpy(pair + klen + 1, exp, elen + 1);
    agentc_free(exp);
    *(char **)agentc_vec_push(&c->env, sizeof(char *)) = pair;
    return 0;
}

/* RFC 7230 token (tchar): any of ! # $ % & ' * + - . ^ _ ` | ~ , DIGIT, ALPHA.
 * A header name that is a token can never carry CR/LF/colon/whitespace, so a
 * JSON key can never split the emitted header block. */
static bool mcp_hdr_key_ok(const char *p, size_t n) {
    if (!p || n == 0) return false;
    for (size_t i = 0; i < n; i++) {
        u8 c = (u8)p[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '!' || c == '#' || c == '$' ||
                  c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
                  c == '-' || c == '.' || c == '^' || c == '_' || c == '`' ||
                  c == '|' || c == '~';
        if (!ok) return false;
    }
    return true;
}

static int cfg_header_cb(void *ud, const char *k, size_t klen, JSpan v) {
    HeaderCtx *h = ud;
    if (klen == 0 || v.p >= v.end) return 0;
    if (!mcp_hdr_key_ok(k, klen)) {
        agentc_logf(2, "mcp: ignoring header with a non-token name");
        return 0;
    }
    AgcJson *sv = agentc_json_parse_in(h->a, v.p, (size_t)(v.end - v.p));
    size_t vlen = 0;
    const char *val = agentc_json_str(sv, &vlen);
    if (!val) return 0;
    char *exp = agentc_mcp_expand_env(val);
    agentc_buf_push(h->b, k, klen);
    agentc_buf_cstr(h->b, ": ");
    for (const char *p = exp; *p; p++) agentc_buf_byte(h->b, (*p == '\r' || *p == '\n') ? ' ' : (u8)*p);
    agentc_buf_cstr(h->b, "\r\n");
    agentc_free(exp);
    return 0;
}

static int cfg_server_cb(void *ud, const char *k, size_t klen, JSpan v) {
    CfgCtx *ctx = ud;
    AgcVec *out = ctx->out;
    if (klen == 0 || klen > 200 || v.p >= v.end || *v.p != '{') return 0;
    AgcJson *so = agentc_json_parse_in(ctx->server, v.p, (size_t)(v.end - v.p));
    if (!so || agentc_json_type(so) != AGENTC_JSON_OBJ) return 0;
    McpCfg c;
    agentc_memset(&c, 0, sizeof c);
    c.name = agentc_strdup_len(k, klen);
    c.enabled = agentc_json_get_bool(so, "enabled", true);
    c.timeout_ms = agentc_json_get_int(so, "timeout_ms", MCP_DEFAULT_TIMEOUT_MS);
    if (c.timeout_ms < 100) c.timeout_ms = MCP_DEFAULT_TIMEOUT_MS;
    if (c.timeout_ms > MCP_MAX_TIMEOUT_MS) c.timeout_ms = MCP_MAX_TIMEOUT_MS;
    const char *cmd = agentc_json_get_str(so, "command");
    if (cmd) c.command = agentc_mcp_expand_env(cmd);
    const char *url = agentc_json_get_str(so, "url");
    if (url) c.url = agentc_mcp_expand_env(url);
    AgcJson *args = agentc_json_get(so, "args");
    if (args && agentc_json_type(args) == AGENTC_JSON_ARR) {
        for (size_t i = 0; i < agentc_json_len(args); i++) {
            const char *a = agentc_json_str(agentc_json_at(args, i), NULL);
            if (!a) continue;
            *(char **)agentc_vec_push(&c.args, sizeof(char *)) = agentc_mcp_expand_env(a);
        }
    }
    /* env/headers are decoded one value at a time into a separate arena so the
     * live server sub-document is never cleared mid-scan. */
    JSpan env;
    if (j_find(v.p, v.end, "env", &env) && env.p < env.end && *env.p == '{') {
        EnvCtx ectx = { &c, ctx->value };
        (void)j_foreach_pair(env.p, env.end, cfg_env_cb, &ectx);
    }
    JSpan hdrs;
    AgcBuf hb = { 0 };
    if (j_find(v.p, v.end, "headers", &hdrs) && hdrs.p < hdrs.end && *hdrs.p == '{') {
        HeaderCtx hctx = { &hb, ctx->value };
        (void)j_foreach_pair(hdrs.p, hdrs.end, cfg_header_cb, &hctx);
        if (hb.len) c.headers = (char *)hb.p;
    } else {
        agentc_buf_free(&hb);
    }
    *(McpCfg *)agentc_vec_push(out, sizeof c) = c;
    return 0;
}

int agentc_mcp_cfg_parse(const char *text, size_t n, AgcVec *out) {
    if (!out) return MC_EINVAL;
    if (!text || n == 0) return 0;
    AgcJsonArena *doc = agentc_json_arena_new(0);
    AgcJsonArena *srv = agentc_json_arena_new(0);
    AgcJsonArena *val = agentc_json_arena_new(0);
    int rc = MC_EINVAL;
    if (doc && srv && val) {
        AgcJson *root = agentc_json_parse_in(doc, text, n);
        if (root && agentc_json_type(root) == AGENTC_JSON_OBJ) {
            const char *end = text + n;
            JSpan servers;
            rc = 0;
            if (j_find(text, end, "servers", &servers) && servers.p < servers.end &&
                *servers.p == '{') {
                size_t base = out->len;
                CfgCtx ctx = { out, srv, val };
                rc = j_foreach_pair(servers.p, servers.end, cfg_server_cb, &ctx);
                if (rc != 0) {
                    for (size_t i = base; i < out->len; i++)
                        mcp_cfg_free_item(&((McpCfg *)out->p)[i]);
                    out->len = base;
                }
            }
        }
    }
    agentc_json_arena_free(doc);
    agentc_json_arena_free(srv);
    agentc_json_arena_free(val);
    return rc;
}

/* =========================================================================
 * Tool names and flags
 * ======================================================================= */
void agentc_mcp_name_sanitize(const char *s, size_t n, char *out, size_t cap) {
    if (!out || cap == 0) return;
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        char c = s ? s[i] : 0;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            c = '_';
        out[o++] = c;
    }
    out[o] = 0;
}

u32 agentc_mcp_name_hash(const char *server, const char *tool) {
    u32 h = 2166136261u;
    for (const char *p = server ? server : ""; *p; p++) {
        h ^= (u8)*p;
        h *= 16777619u;
    }
    h ^= 0x1fu;
    h *= 16777619u;
    for (const char *p = tool ? tool : ""; *p; p++) {
        h ^= (u8)*p;
        h *= 16777619u;
    }
    return h;
}

void agentc_mcp_tool_name(const char *server, const char *tool, char *out, size_t cap) {
    if (!out || cap == 0) return;
    char sb[96], tb[96];
    agentc_mcp_name_sanitize(server, agentc_strlen(server), sb, sizeof sb);
    agentc_mcp_name_sanitize(tool, agentc_strlen(tool), tb, sizeof tb);
    if (!sb[0]) agentc_snprintf(sb, sizeof sb, "s");
    if (!tb[0]) agentc_snprintf(tb, sizeof tb, "t");
    agentc_snprintf(out, cap, "mcp__%s__%s", sb, tb);
}

u32 agentc_mcp_flags_from_hints(bool read_only, bool destructive) {
    /* MCP schema defaults: readOnlyHint:true wins; otherwise the
     * destructive bit is set unless the server explicitly said false. */
    if (read_only) return AGENTC_TOOL_READONLY;
    return destructive ? AGENTC_TOOL_DESTRUCTIVE : 0;
}

/* =========================================================================
 * Capabilities and per-kind list sync identities
 * ======================================================================= */
enum {
    MCP_KIND_TOOLS = 0,
    MCP_KIND_PROMPTS,
    MCP_KIND_RESOURCES,
    MCP_KIND_RESOURCE_TEMPLATE,   /* resources/templates/list */
    MCP_KIND_COUNT,
};

static const char *mcp_kind_name(int kind) {
    switch (kind) {
    case MCP_KIND_TOOLS: return "tools";
    case MCP_KIND_PROMPTS: return "prompts";
    case MCP_KIND_RESOURCES: return "resources";
    default: return "resource templates";
    }
}

static u32 mcp_kind_cap(int kind) {
    switch (kind) {
    case MCP_KIND_TOOLS: return MCP_CAP_TOOLS;
    case MCP_KIND_PROMPTS: return MCP_CAP_PROMPTS;
    /* One server capability (`resources`) covers resources/list and
     * resources/templates/list; a server that never advertises it is never
     * called for either kind. */
    default: return MCP_CAP_RESOURCES;
    }
}

static const char *mcp_kind_method(int kind) {
    switch (kind) {
    case MCP_KIND_PROMPTS: return "prompts/list";
    case MCP_KIND_RESOURCES: return "resources/list";
    case MCP_KIND_RESOURCE_TEMPLATE: return "resources/templates/list";
    default: return "tools/list";
    }
}

/* Every kind is fetched, in enum order (tools -> prompts -> resources ->
 * resource templates) after notifications/initialized. */

/* One result.capabilities key per MCP_CAP_* base bit. Resource templates
 * share the `resources` capability, so this is not MCP_KIND_COUNT. */
#define MCP_CAP_KIND_COUNT 3

/* result.capabilities -> MCP_CAP_* bits. Absent, malformed or present-but-empty
 * capabilities fall back to tools-only. */
u32 agentc_mcp_caps_parse(const char *json, size_t n) {
    u32 caps = 0;
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *root = a ? agentc_json_parse_in(a, json, n) : NULL;
    AgcJson *result = root && agentc_json_type(root) == AGENTC_JSON_OBJ
                          ? agentc_json_get(root, "result") : NULL;
    AgcJson *c = result ? agentc_json_get(result, "capabilities") : NULL;
    if (c && agentc_json_type(c) == AGENTC_JSON_OBJ) {
        static const char *keys[MCP_CAP_KIND_COUNT] = { "tools", "prompts", "resources" };
        static const u32 base[MCP_CAP_KIND_COUNT] = {
            MCP_CAP_TOOLS, MCP_CAP_PROMPTS, MCP_CAP_RESOURCES,
        };
        static const u32 changed[MCP_CAP_KIND_COUNT] = {
            MCP_CAP_TOOLS_LIST_CHANGED, MCP_CAP_PROMPTS_LIST_CHANGED,
            MCP_CAP_RESOURCES_LIST_CHANGED,
        };
        for (int k = 0; k < MCP_CAP_KIND_COUNT; k++) {
            AgcJson *kind = agentc_json_get(c, keys[k]);
            if (agentc_json_type(kind) != AGENTC_JSON_OBJ) continue;
            caps |= base[k];
            if (agentc_json_get_bool(kind, "listChanged", false)) caps |= changed[k];
        }
    }
    agentc_json_arena_free(a);
    if (!(caps & (MCP_CAP_TOOLS | MCP_CAP_PROMPTS | MCP_CAP_RESOURCES)))
        caps = MCP_CAP_TOOLS;
    return caps;
}

/* =========================================================================
 * JSON-RPC framing
 * ======================================================================= */
char *agentc_mcp_rpc_request(u64 id, const char *method, const char *params_json) {
    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "jsonrpc");
    agentc_jsonw_cstr(&w, "2.0");
    agentc_jsonw_key(&w, "id");
    agentc_jsonw_u64(&w, id);
    agentc_jsonw_key(&w, "method");
    agentc_jsonw_cstr(&w, method ? method : "");
    if (params_json) {
        agentc_jsonw_key(&w, "params");
        agentc_jsonw_raw(&w, params_json, agentc_strlen(params_json));
    }
    agentc_jsonw_end(&w);
    agentc_buf_byte(&b, '\n');
    return (char *)b.p;
}

char *agentc_mcp_rpc_notify(const char *method, const char *params_json) {
    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "jsonrpc");
    agentc_jsonw_cstr(&w, "2.0");
    agentc_jsonw_key(&w, "method");
    agentc_jsonw_cstr(&w, method ? method : "");
    if (params_json) {
        agentc_jsonw_key(&w, "params");
        agentc_jsonw_raw(&w, params_json, agentc_strlen(params_json));
    }
    agentc_jsonw_end(&w);
    agentc_buf_byte(&b, '\n');
    return (char *)b.p;
}

int agentc_mcp_stdio_next(AgcBuf *in, char **out) {
    if (!in || !out) return MC_EINVAL;
    *out = NULL;
    for (;;) {
        if (in->len == 0) return 0;
        size_t i = 0;
        while (i < in->len && in->p[i] != '\n') i++;
        if (i == in->len) {
            if (in->len > AGENTC_LIMIT_MCP_LINE_BYTES) return MC_E2BIG;
            return 0;
        }
        size_t len = i;
        if (len > 0 && in->p[len - 1] == '\r') len--;
        if (len > AGENTC_LIMIT_MCP_LINE_BYTES) return MC_E2BIG;
        size_t consumed = i + 1;
        if (len > 0) *out = agentc_strdup_len((const char *)in->p, len);
        agentc_memmove(in->p, in->p + consumed, in->len - consumed);
        in->len -= consumed;
        if (in->p) in->p[in->len] = 0;
        if (len > 0) return 1;
        /* blank line: keep scanning */
    }
}

/* The per-line cap is enforced on the longest unterminated trailing line, not
 * on the total number of buffered bytes: a burst of many complete
 * newline-delimited messages may exceed the cap in aggregate while every
 * individual line stays within it. Returns false only when the pending
 * (unterminated) line has grown past the cap. */
static bool mcp_line_under_cap(const AgcBuf *in) {
    if (in == NULL || in->p == NULL || in->len == 0) return true;
    size_t start = 0;
    for (size_t i = in->len; i > 0; i--) {
        if (in->p[i - 1] == '\n') { start = i; break; }
    }
    return in->len - start <= AGENTC_LIMIT_MCP_LINE_BYTES;
}

/* True when `p` holds a line terminator, so the caller can hand the buffered
 * complete lines to agentc_mcp_stdio_next before reading more. */
static bool mcp_has_newline(const u8 *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] == '\n') return true;
    return false;
}

static int json_msg_id(const char *json, size_t n, u64 *id, bool *has_id) {
    if (has_id) *has_id = false;
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(a, json, n);
    int rc = MC_EPROTO;
    if (root && agentc_json_type(root) == AGENTC_JSON_OBJ) {
        AgcJson *v = agentc_json_get(root, "id");
        rc = 0;
        if (v && agentc_json_type(v) == AGENTC_JSON_NUM) {
            size_t l = 0;
            const char *num = agentc_json_num(v, &l);
            bool ok = false;
            u64 x = agentc_parse_u64(num, l, &ok);
            if (!ok) {
                rc = MC_EPROTO;
            } else {
                if (id) *id = x;
                if (has_id) *has_id = true;
            }
        }
    }
    agentc_json_arena_free(a);
    return rc;
}

int agentc_mcp_rpc_await(McpReader *rd, AgcBuf *in, u64 id, int timeout_ms,
                     const volatile bool *cancel,
                     void (*on_notify)(void *ud, const char *json, size_t n),
                     void *nud, char **out) {
    if (!rd || !rd->read || !in || !out) return MC_EINVAL;
    *out = NULL;
    if (timeout_ms > 3600000) timeout_ms = 3600000;
    i64 deadline = timeout_ms > 0 ? os_now_ns(OS_CLOCK_MONOTONIC) + (i64)timeout_ms * 1000000 : 0;
    bool eof = false;
    for (;;) {
        if (cancel && *cancel) return MC_ECANCELED;
        for (;;) {
            if (cancel && *cancel) return MC_ECANCELED;
            u8 tmp[8192];
            int n = rd->read(rd->ud, tmp, sizeof tmp);
            if (n > 0) {
                agentc_buf_push(in, tmp, (size_t)n);
                if (!mcp_line_under_cap(in)) return MC_E2BIG;
                /* Parse buffered complete lines before reading more so a burst of
                 * small messages cannot grow the buffer without bound. */
                if (mcp_has_newline(tmp, (size_t)n)) break;
                continue;
            }
            if (n == MC_EAGAIN) break;
            if (n == 0) {
                eof = true;
                break;
            }
            return n;
        }
        for (;;) {
            char *msg = NULL;
            int g = agentc_mcp_stdio_next(in, &msg);
            if (g < 0) return g;
            if (g == 0) break;
            u64 mid = 0;
            bool has = false;
            int pr = json_msg_id(msg, agentc_strlen(msg), &mid, &has);
            if (pr != 0) {
                agentc_free(msg);
                continue;
            }
            if (!has) {
                if (on_notify) on_notify(nud, msg, agentc_strlen(msg));
                agentc_free(msg);
                continue;
            }
            if (mid == id) {
                *out = msg;
                return 0;
            }
            agentc_free(msg);
        }
        if (*out) return 0;
        if (eof) return MC_ECONNRESET;
        int wait_ms = -1;
        if (timeout_ms > 0) {
            i64 rem = deadline - os_now_ns(OS_CLOCK_MONOTONIC);
            if (rem <= 0) return MC_ETIMEDOUT;
            wait_ms = (int)((rem + 999999) / 1000000);
        }
        if (rd->wait) {
            int w = rd->wait(rd->ud, wait_ms);
            if (w == MC_ECANCELED) return w;
            if (w != 0) return w;
        } else {
            os_sleep_ns(1000000);
        }
    }
}

/* =========================================================================
 * tools/list and tools/call parsing
 * ======================================================================= */
void agentc_mcp_parsed_free(AgcVec *v) {
    if (!v) return;
    for (size_t i = 0; i < v->len; i++) {
        McpParsedTool *t = &((McpParsedTool *)v->p)[i];
        agentc_free(t->name);
        agentc_free(t->desc);
        agentc_free(t->schema);
    }
    agentc_vec_free(v);
}

typedef struct {
    AgcVec *out;
    AgcJsonArena *a;   /* one element at a time; root doc stays live */
} ToolsCtx;

static int tool_elem_cb(void *ud, JSpan v) {
    ToolsCtx *ctx = ud;
    if (v.p >= v.end || *v.p != '{') return 0;
    AgcJson *t = agentc_json_parse_in(ctx->a, v.p, (size_t)(v.end - v.p));
    if (!t || agentc_json_type(t) != AGENTC_JSON_OBJ) return 0;
    const char *name = agentc_json_get_str(t, "name");
    if (!name || !name[0]) return 0;
    size_t nlen = agentc_strlen(name);
    if (nlen > MCP_MAX_TOOL_NAME) {
        /* A truncated remote name would call the wrong tool, so skip it. */
        agentc_logf(2, "mcp: ignoring tool with a %llu-byte name (cap %d)",
                    (unsigned long long)nlen, (int)MCP_MAX_TOOL_NAME);
        return 0;
    }
    McpParsedTool pt;
    agentc_memset(&pt, 0, sizeof pt);
    pt.name = agentc_strdup(name);
    const char *desc = agentc_json_get_str(t, "description");
    if (!desc) desc = "";
    size_t dlen = agentc_strlen(desc);
    if (dlen > MCP_MAX_TOOL_DESC) {
        agentc_logf(2, "mcp: tool %s description truncated to %d bytes", name,
                    (int)MCP_MAX_TOOL_DESC);
        dlen = MCP_MAX_TOOL_DESC;
    }
    pt.desc = agentc_strdup_len(desc, dlen);
    AgcJson *ann = agentc_json_get(t, "annotations");
    bool ann_obj = ann && agentc_json_type(ann) == AGENTC_JSON_OBJ;
    bool read_only = ann_obj && agentc_json_get_bool(ann, "readOnlyHint", false);
    /* The MCP schema defaults destructiveHint to true when absent, so an
     * annotation-free tool is destructive; explicit false is additive. */
    bool destructive = ann_obj ? agentc_json_get_bool(ann, "destructiveHint", true) : true;
    pt.flags = agentc_mcp_flags_from_hints(read_only, destructive);
    JSpan sc;
    if (j_find(v.p, v.end, "inputSchema", &sc)) {
        size_t slen = (size_t)(sc.end - sc.p);
        bool valid = false;
        if (slen > 0 && slen <= AGENTC_LIMIT_MCP_SCHEMA_BYTES) {
            AgcJson *js = agentc_json_parse_in(ctx->a, sc.p, slen);
            valid = js && agentc_json_type(js) == AGENTC_JSON_OBJ;
        }
        if (valid) {
            pt.schema = agentc_strdup_len(sc.p, slen);
        } else {
            /* Providers embed params_json verbatim, so never hand them JSON cut
             * mid-token or a non-object: substitute a minimal valid schema. */
            agentc_logf(2, "mcp: inputSchema for %s invalid or over %d bytes; using fallback",
                        pt.name, (int)AGENTC_LIMIT_MCP_SCHEMA_BYTES);
            pt.schema = agentc_strdup("{\"type\":\"object\"}");
        }
    }
    *(McpParsedTool *)agentc_vec_push(ctx->out, sizeof pt) = pt;
    return 0;
}

/* A cursor is meaningful only when it carries a non-whitespace token: an
 * empty/blank nextCursor means the server reached the end of the list, so it
 * must not be stored (storing it would re-fetch page 1 up to the page cap). */
static bool mcp_cursor_blank(const char *s) {
    if (!s) return true;
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') return false;
    return true;
}

int agentc_mcp_parse_tools(const char *json, size_t n, AgcVec *out, char **next_cursor) {
    if (!out) return MC_EINVAL;
    if (next_cursor) *next_cursor = NULL;
    AgcJsonArena *doc = agentc_json_arena_new(0);
    AgcJsonArena *scratch = agentc_json_arena_new(0);
    int rc = MC_EPROTO;
    if (doc && scratch) {
        AgcJson *root = agentc_json_parse_in(doc, json, n);
        if (root && agentc_json_type(root) == AGENTC_JSON_OBJ) {
            if (agentc_json_get(root, "error")) {
                rc = MC_EIO;
            } else {
                AgcJson *result = agentc_json_get(root, "result");
                if (!result) {
                    rc = MC_EIO;
                } else {
                    const char *nc = agentc_json_get_str(result, "nextCursor");
                    if (next_cursor && nc && !mcp_cursor_blank(nc))
                        *next_cursor = agentc_strdup(nc);
                    JSpan rs, ts;
                    rc = 0;
                    if (!j_find(json, json + n, "result", &rs)) {
                        rc = MC_EIO;
                    } else if (j_find(rs.p, rs.end, "tools", &ts) && ts.p < ts.end &&
                               *ts.p == '[') {
                        ToolsCtx ctx = { out, scratch };
                        int fr = j_foreach_elem(ts.p, ts.end, tool_elem_cb, &ctx);
                        if (fr < 0) rc = MC_EPROTO;
                    }
                }
            }
        }
    }
    agentc_json_arena_free(doc);
    agentc_json_arena_free(scratch);
    return rc;
}

typedef struct {
    AgcBuf *out;
    AgcJsonArena *a;   /* one element at a time; root doc stays live */
} CallCtx;

static int call_elem_cb(void *ud, JSpan v) {
    CallCtx *ctx = ud;
    AgcJson *el = agentc_json_parse_in(ctx->a, v.p, (size_t)(v.end - v.p));
    const char *txt = NULL;
    size_t tlen = 0;
    if (el && agentc_json_type(el) == AGENTC_JSON_OBJ) {
        const char *type = agentc_json_get_str(el, "type");
        if (type && agentc_streq(type, "text")) txt = agentc_json_str(agentc_json_get(el, "text"), &tlen);
    }
    if (ctx->out->len && ctx->out->p[ctx->out->len - 1] != '\n') agentc_buf_byte(ctx->out, '\n');
    if (txt) agentc_buf_push(ctx->out, txt, tlen);
    else agentc_buf_push(ctx->out, v.p, (size_t)(v.end - v.p));
    return 0;
}

int agentc_mcp_parse_call(const char *json, size_t n, char **text, bool *is_error) {
    if (text) *text = NULL;
    if (is_error) *is_error = false;
    AgcJsonArena *doc = agentc_json_arena_new(0);
    AgcJsonArena *scratch = agentc_json_arena_new(0);
    int rc = MC_EPROTO;
    if (doc && scratch) {
        AgcJson *root = agentc_json_parse_in(doc, json, n);
        if (root && agentc_json_type(root) == AGENTC_JSON_OBJ) {
            AgcJson *err_obj = agentc_json_get(root, "error");
            if (err_obj) {
                i64 code = agentc_json_get_int(err_obj, "code", 0);
                const char *msg = agentc_json_get_str(err_obj, "message");
                AgcBuf b = { 0 };
                agentc_buf_printf(&b, "MCP error %lld: %s", (long long)code, msg ? msg : "");
                if (text) *text = (char *)b.p;
                else agentc_buf_free(&b);
                if (is_error) *is_error = true;
                rc = 0;
            } else {
                AgcJson *result = agentc_json_get(root, "result");
                if (!result) {
                    rc = MC_EIO;
                } else {
                    bool err_flag = agentc_json_get_bool(result, "isError", false);
                    if (is_error) *is_error = err_flag;

                    AgcBuf out = { 0 };
                    JSpan rs;
                    if (j_find(json, json + n, "result", &rs)) {
                        JSpan cs;
                        if (j_find(rs.p, rs.end, "content", &cs) && cs.p < cs.end && *cs.p == '[') {
                            CallCtx ctx = { &out, scratch };
                            (void)j_foreach_elem(cs.p, cs.end, call_elem_cb, &ctx);
                        }
                        JSpan sc;
                        if (j_find(rs.p, rs.end, "structuredContent", &sc)) {
                            if (out.len && out.p[out.len - 1] != '\n') agentc_buf_byte(&out, '\n');
                            agentc_buf_push(&out, sc.p, (size_t)(sc.end - sc.p));
                        }
                    }
                    if (!out.p) out.p = (u8 *)agentc_strdup_len("", 0);
                    if (text) *text = (char *)out.p;
                    else agentc_buf_free(&out);
                    rc = 0;
                }
            }
        }
    }
    agentc_json_arena_free(doc);
    agentc_json_arena_free(scratch);
    return rc;
}

/* =========================================================================
 * prompts/list and prompts/get parsing
 * ======================================================================= */
static void prompt_args_free(McpParsedArg *args, size_t n) {
    for (size_t i = 0; i < n; i++) {
        agentc_free(args[i].name);
        agentc_free(args[i].description);
    }
}

static void prompt_parsed_free(McpParsedPrompt *p) {
    agentc_free(p->name);
    agentc_free(p->title);
    agentc_free(p->description);
    prompt_args_free(p->args, p->nargs);
    agentc_memset(p, 0, sizeof *p);
}

void agentc_mcp_parsed_prompts_free(AgcVec *v) {
    if (!v) return;
    for (size_t i = 0; i < v->len; i++) prompt_parsed_free(&((McpParsedPrompt *)v->p)[i]);
    agentc_vec_free(v);
}

typedef struct {
    AgcVec *out;
    AgcJsonArena *a;   /* one element at a time; root doc stays live */
} PromptsCtx;

static int prompt_elem_cb(void *ud, JSpan v) {
    PromptsCtx *ctx = ud;
    if (v.p >= v.end || *v.p != '{') return 0;
    AgcJson *t = agentc_json_parse_in(ctx->a, v.p, (size_t)(v.end - v.p));
    if (!t || agentc_json_type(t) != AGENTC_JSON_OBJ) return 0;
    const char *name = agentc_json_get_str(t, "name");
    if (!name || !name[0]) return 0;
    size_t nlen = agentc_strlen(name);
    if (nlen > MCP_MAX_PROMPT_NAME) {
        agentc_logf(2, "mcp: ignoring prompt with a %llu-byte name (cap %d)",
                    (unsigned long long)nlen, (int)MCP_MAX_PROMPT_NAME);
        return 0;
    }
    McpParsedPrompt p;
    agentc_memset(&p, 0, sizeof p);
    p.name = agentc_strdup_len(name, nlen);
    const char *title = agentc_json_get_str(t, "title");
    if (!title) title = "";
    size_t tlen = agentc_strlen(title);
    if (tlen > MCP_MAX_PROMPT_DESC) {
        agentc_logf(2, "mcp: prompt %s title truncated to %d bytes", name,
                    (int)MCP_MAX_PROMPT_DESC);
        tlen = MCP_MAX_PROMPT_DESC;
    }
    p.title = agentc_strdup_len(title, tlen);
    const char *desc = agentc_json_get_str(t, "description");
    if (!desc) desc = "";
    size_t dlen = agentc_strlen(desc);
    if (dlen > MCP_MAX_PROMPT_DESC) {
        agentc_logf(2, "mcp: prompt %s description truncated to %d bytes", name,
                    (int)MCP_MAX_PROMPT_DESC);
        dlen = MCP_MAX_PROMPT_DESC;
    }
    p.description = agentc_strdup_len(desc, dlen);
    AgcJson *args = agentc_json_get(t, "arguments");
    if (args && agentc_json_type(args) == AGENTC_JSON_ARR) {
        size_t alen = agentc_json_len(args);
        for (size_t i = 0; i < alen; i++) {
            if (p.nargs >= MCP_MAX_PROMPT_ARGS) {
                agentc_logf(2, "mcp: prompt %s: argument list truncated to %d", name,
                            (int)MCP_MAX_PROMPT_ARGS);
                break;
            }
            AgcJson *arg = agentc_json_at(args, i);
            if (!arg || agentc_json_type(arg) != AGENTC_JSON_OBJ) continue;
            const char *an = agentc_json_get_str(arg, "name");
            if (!an || !an[0]) continue;
            size_t anl = agentc_strlen(an);
            if (anl > MCP_MAX_ARG_NAME) {
                agentc_logf(2, "mcp: prompt %s: ignoring an argument with a %llu-byte "
                            "name (cap %d)", name, (unsigned long long)anl,
                            (int)MCP_MAX_ARG_NAME);
                continue;
            }
            McpParsedArg *pa = &p.args[p.nargs++];
            pa->name = agentc_strdup_len(an, anl);
            const char *ad = agentc_json_get_str(arg, "description");
            if (!ad) ad = "";
            size_t adl = agentc_strlen(ad);
            if (adl > MCP_MAX_PROMPT_DESC) {
                agentc_logf(2, "mcp: prompt %s: argument %s description truncated to %d bytes",
                            name, an, (int)MCP_MAX_PROMPT_DESC);
                adl = MCP_MAX_PROMPT_DESC;
            }
            pa->description = agentc_strdup_len(ad, adl);
            pa->required = agentc_json_get_bool(arg, "required", false);
        }
    }
    *(McpParsedPrompt *)agentc_vec_push(ctx->out, sizeof p) = p;
    return 0;
}

int agentc_mcp_parse_prompts(const char *json, size_t n, AgcVec *out, char **next_cursor) {
    if (!out) return MC_EINVAL;
    if (next_cursor) *next_cursor = NULL;
    AgcJsonArena *doc = agentc_json_arena_new(0);
    AgcJsonArena *scratch = agentc_json_arena_new(0);
    int rc = MC_EPROTO;
    if (doc && scratch) {
        AgcJson *root = agentc_json_parse_in(doc, json, n);
        if (root && agentc_json_type(root) == AGENTC_JSON_OBJ) {
            if (agentc_json_get(root, "error")) {
                rc = MC_EIO;
            } else {
                AgcJson *result = agentc_json_get(root, "result");
                if (!result) {
                    rc = MC_EIO;
                } else {
                    const char *nc = agentc_json_get_str(result, "nextCursor");
                    if (next_cursor && nc && !mcp_cursor_blank(nc))
                        *next_cursor = agentc_strdup(nc);
                    JSpan rs, ps;
                    rc = 0;
                    if (!j_find(json, json + n, "result", &rs)) {
                        rc = MC_EIO;
                    } else if (j_find(rs.p, rs.end, "prompts", &ps) && ps.p < ps.end &&
                               *ps.p == '[') {
                        PromptsCtx ctx = { out, scratch };
                        int fr = j_foreach_elem(ps.p, ps.end, prompt_elem_cb, &ctx);
                        if (fr < 0) rc = MC_EPROTO;
                    }
                }
            }
        }
    }
    agentc_json_arena_free(doc);
    agentc_json_arena_free(scratch);
    return rc;
}

typedef struct {
    AgcBuf *out;
    size_t blocks;        /* text blocks appended so far */
    bool over_cap;        /* a block would push past AGENTC_LIMIT_MCP_PROMPT_TEXT_BYTES */
    AgcJsonArena *a;      /* one content block at a time; root doc stays live */
} GetTextCtx;

static int prompt_content_cb(void *ud, JSpan v) {
    GetTextCtx *ctx = ud;
    if (ctx->over_cap) return 1;
    AgcJson *el = agentc_json_parse_in(ctx->a, v.p, (size_t)(v.end - v.p));
    if (!el || agentc_json_type(el) != AGENTC_JSON_OBJ) return 0;
    const char *type = agentc_json_get_str(el, "type");
    if (!type || !agentc_streq(type, "text")) {
        /* Names only: the block type, never its content. */
        agentc_logf(2, "mcp: prompt content block '%s' skipped (text only)",
                    type ? type : "?");
        return 0;
    }
    size_t tlen = 0;
    const char *txt = agentc_json_str(agentc_json_get(el, "text"), &tlen);
    if (!txt) return 0;
    size_t sep = ctx->blocks ? 2 : 0;
    if (ctx->out->len + sep + tlen > AGENTC_LIMIT_MCP_PROMPT_TEXT_BYTES) {
        ctx->over_cap = true;
        return 1;
    }
    if (sep) agentc_buf_cstr(ctx->out, "\n\n");
    agentc_buf_push(ctx->out, txt, tlen);
    ctx->blocks++;
    return 0;
}

static int prompt_msg_cb(void *ud, JSpan v) {
    GetTextCtx *ctx = ud;
    if (ctx->over_cap) return 1;
    JSpan cs;
    if (!j_find(v.p, v.end, "content", &cs)) return 0;
    if (cs.p < cs.end && *cs.p == '[') {
        (void)j_foreach_elem(cs.p, cs.end, prompt_content_cb, ctx);
    } else if (cs.p < cs.end && *cs.p == '{') {
        (void)prompt_content_cb(ctx, cs);
    }
    return ctx->over_cap ? 1 : 0;
}

int agentc_mcp_parse_prompt_get(const char *json, size_t n, char **text) {
    if (text) *text = NULL;
    AgcJsonArena *doc = agentc_json_arena_new(0);
    AgcJsonArena *scratch = agentc_json_arena_new(0);
    int rc = MC_EPROTO;
    if (doc && scratch) {
        AgcJson *root = agentc_json_parse_in(doc, json, n);
        if (root && agentc_json_type(root) == AGENTC_JSON_OBJ) {
            if (agentc_json_get(root, "error")) {
                rc = MC_EIO;
            } else if (!agentc_json_get(root, "result")) {
                rc = MC_EIO;
            } else {
                AgcBuf out = { 0 };
                GetTextCtx ctx = { &out, 0, false, scratch };
                JSpan rs;
                if (j_find(json, json + n, "result", &rs)) {
                    JSpan ms;
                    if (j_find(rs.p, rs.end, "messages", &ms) && ms.p < ms.end &&
                        *ms.p == '[')
                        (void)j_foreach_elem(ms.p, ms.end, prompt_msg_cb, &ctx);
                }
                if (ctx.over_cap) {
                    agentc_buf_free(&out);
                    rc = MC_E2BIG;
                } else {
                    if (!out.p) out.p = (u8 *)agentc_strdup_len("", 0);
                    if (text) *text = (char *)out.p;
                    else agentc_buf_free(&out);
                    rc = 0;
                }
            }
        }
    }
    agentc_json_arena_free(doc);
    agentc_json_arena_free(scratch);
    return rc;
}

/* =========================================================================
 * resources/list, resources/templates/list and resources/read parsing
 *
 * Names/URIs are the only bounded identity fields; server-provided contents
 * are never logged. An over-long uri/uriTemplate is skipped (a clipped
 * address would read the wrong resource), an over-long name/title/
 * description/mimeType is truncated.
 * ======================================================================= */
static char *resource_field_dup(const AgcJson *t, const char *key, size_t cap,
                                const char *kind) {
    const char *s = agentc_json_get_str(t, key);
    if (!s) s = "";
    size_t n = agentc_strlen(s);
    if (n > cap) {
        /* Byte count only: a resource field is never logged by value. */
        agentc_logf(2, "mcp: %s: %s field over %d bytes truncated", kind, key, (int)cap);
        n = cap;
    }
    return agentc_strdup_len(s, n);
}

void agentc_mcp_parsed_resources_free(AgcVec *v) {
    if (!v) return;
    for (size_t i = 0; i < v->len; i++) {
        McpParsedResource *r = &((McpParsedResource *)v->p)[i];
        agentc_free(r->uri);
        agentc_free(r->name);
        agentc_free(r->title);
        agentc_free(r->description);
        agentc_free(r->mime);
    }
    agentc_vec_free(v);
}

void agentc_mcp_parsed_resource_templates_free(AgcVec *v) {
    if (!v) return;
    for (size_t i = 0; i < v->len; i++) {
        McpParsedResourceTemplate *r = &((McpParsedResourceTemplate *)v->p)[i];
        agentc_free(r->uri_template);
        agentc_free(r->name);
        agentc_free(r->description);
        agentc_free(r->mime);
    }
    agentc_vec_free(v);
}

typedef struct {
    AgcVec *out;
    AgcJsonArena *a;   /* one element at a time; root doc stays live */
    bool templates;
} ResourcesCtx;

static int resource_elem_cb(void *ud, JSpan v) {
    ResourcesCtx *ctx = ud;
    if (v.p >= v.end || *v.p != '{') return 0;
    AgcJson *t = agentc_json_parse_in(ctx->a, v.p, (size_t)(v.end - v.p));
    if (!t || agentc_json_type(t) != AGENTC_JSON_OBJ) return 0;
    const char *name = agentc_json_get_str(t, "name");
    if (!name || !name[0]) return 0;
    const char *addr = agentc_json_get_str(t, ctx->templates ? "uriTemplate" : "uri");
    if (!addr || !addr[0]) return 0;
    size_t alen = agentc_strlen(addr);
    if (alen > AGENTC_LIMIT_MCP_RESOURCE_URI) {
        agentc_logf(2, "mcp: ignoring %s with a %llu-byte %s (cap %d)",
                    ctx->templates ? "resource template" : "resource",
                    (unsigned long long)alen, ctx->templates ? "uriTemplate" : "uri",
                    (int)AGENTC_LIMIT_MCP_RESOURCE_URI);
        return 0;
    }
    /* A display name longer than the cap is truncated, never dropped: unlike
     * a uri it is not an address. */
    if (ctx->templates) {
        McpParsedResourceTemplate r;
        agentc_memset(&r, 0, sizeof r);
        r.uri_template = agentc_strdup_len(addr, alen);
        r.name = resource_field_dup(t, "name", AGENTC_LIMIT_MCP_RESOURCE_NAME,
                                    "resource template");
        r.description = resource_field_dup(t, "description", AGENTC_LIMIT_MCP_RESOURCE_DESC,
                                           "resource template");
        r.mime = resource_field_dup(t, "mimeType", AGENTC_LIMIT_MCP_RESOURCE_MIME,
                                    "resource template");
        *(McpParsedResourceTemplate *)agentc_vec_push(ctx->out, sizeof r) = r;
    } else {
        McpParsedResource r;
        agentc_memset(&r, 0, sizeof r);
        r.uri = agentc_strdup_len(addr, alen);
        r.name = resource_field_dup(t, "name", AGENTC_LIMIT_MCP_RESOURCE_NAME,
                                    "resource");
        r.title = resource_field_dup(t, "title", AGENTC_LIMIT_MCP_RESOURCE_NAME,
                                     "resource");
        r.description = resource_field_dup(t, "description", AGENTC_LIMIT_MCP_RESOURCE_DESC,
                                           "resource");
        r.mime = resource_field_dup(t, "mimeType", AGENTC_LIMIT_MCP_RESOURCE_MIME,
                                    "resource");
        *(McpParsedResource *)agentc_vec_push(ctx->out, sizeof r) = r;
    }
    return 0;
}

static int parse_resource_list(const char *json, size_t n, AgcVec *out, char **next_cursor,
                               bool templates) {
    if (!out) return MC_EINVAL;
    if (next_cursor) *next_cursor = NULL;
    AgcJsonArena *doc = agentc_json_arena_new(0);
    AgcJsonArena *scratch = agentc_json_arena_new(0);
    int rc = MC_EPROTO;
    if (doc && scratch) {
        AgcJson *root = agentc_json_parse_in(doc, json, n);
        if (root && agentc_json_type(root) == AGENTC_JSON_OBJ) {
            if (agentc_json_get(root, "error")) {
                rc = MC_EIO;
            } else {
                AgcJson *result = agentc_json_get(root, "result");
                if (!result) {
                    rc = MC_EIO;
                } else {
                    const char *nc = agentc_json_get_str(result, "nextCursor");
                    if (next_cursor && nc && !mcp_cursor_blank(nc))
                        *next_cursor = agentc_strdup(nc);
                    JSpan rs, ls;
                    rc = 0;
                    if (!j_find(json, json + n, "result", &rs)) {
                        rc = MC_EIO;
                    } else if (j_find(rs.p, rs.end, templates ? "resourceTemplates" : "resources",
                                      &ls) &&
                               ls.p < ls.end && *ls.p == '[') {
                        ResourcesCtx ctx = { out, scratch, templates };
                        int fr = j_foreach_elem(ls.p, ls.end, resource_elem_cb, &ctx);
                        if (fr < 0) rc = MC_EPROTO;
                    }
                }
            }
        }
    }
    agentc_json_arena_free(doc);
    agentc_json_arena_free(scratch);
    return rc;
}

int agentc_mcp_parse_resources(const char *json, size_t n, AgcVec *out, char **next_cursor) {
    return parse_resource_list(json, n, out, next_cursor, false);
}

int agentc_mcp_parse_resource_templates(const char *json, size_t n, AgcVec *out,
                                        char **next_cursor) {
    return parse_resource_list(json, n, out, next_cursor, true);
}

typedef struct {
    AgcBuf *out;
    size_t blocks;       /* text blocks appended so far */
    bool over_cap;       /* a block would push past AGENTC_LIMIT_MCP_RESOURCE_TEXT_BYTES */
    bool binary;         /* a blob entry: binary resource not supported */
    AgcJsonArena *a;     /* one content entry at a time; root doc stays live */
} ReadCtx;

static int read_content_cb(void *ud, JSpan v) {
    ReadCtx *ctx = ud;
    if (ctx->over_cap || ctx->binary) return 1;
    AgcJson *el = agentc_json_parse_in(ctx->a, v.p, (size_t)(v.end - v.p));
    if (!el || agentc_json_type(el) != AGENTC_JSON_OBJ) return 0;
    JSpan blob;
    if (j_find(v.p, v.end, "blob", &blob)) {
        /* The whole read is rejected: a partially consumed binary resource is
         * worse than a clear refusal. Never log the payload. */
        ctx->binary = true;
        return 1;
    }
    size_t tlen = 0;
    const char *txt = agentc_json_str(agentc_json_get(el, "text"), &tlen);
    if (!txt) {
        agentc_logf(2, "mcp: resources/read content entry has neither text nor blob; skipped");
        return 0;
    }
    size_t sep = ctx->blocks ? 2 : 0;
    if (ctx->out->len + sep + tlen > AGENTC_LIMIT_MCP_RESOURCE_TEXT_BYTES) {
        ctx->over_cap = true;
        return 1;
    }
    if (sep) agentc_buf_cstr(ctx->out, "\n\n");
    agentc_buf_push(ctx->out, txt, tlen);
    ctx->blocks++;
    return 0;
}

int agentc_mcp_parse_resource_read(const char *json, size_t n, char **text) {
    if (text) *text = NULL;
    AgcJsonArena *doc = agentc_json_arena_new(0);
    AgcJsonArena *scratch = agentc_json_arena_new(0);
    int rc = MC_EPROTO;
    if (doc && scratch) {
        AgcJson *root = agentc_json_parse_in(doc, json, n);
        if (root && agentc_json_type(root) == AGENTC_JSON_OBJ) {
            if (agentc_json_get(root, "error")) {
                rc = MC_EIO;
            } else if (!agentc_json_get(root, "result")) {
                rc = MC_EIO;
            } else {
                AgcBuf out = { 0 };
                ReadCtx ctx = { &out, 0, false, false, scratch };
                JSpan rs;
                if (j_find(json, json + n, "result", &rs)) {
                    JSpan cs;
                    if (j_find(rs.p, rs.end, "contents", &cs) && cs.p < cs.end &&
                        *cs.p == '[')
                        (void)j_foreach_elem(cs.p, cs.end, read_content_cb, &ctx);
                }
                if (ctx.binary) {
                    agentc_buf_free(&out);
                    rc = MCP_ERR_BINARY;
                } else if (ctx.over_cap) {
                    agentc_buf_free(&out);
                    rc = MC_E2BIG;
                } else {
                    if (!out.p) out.p = (u8 *)agentc_strdup_len("", 0);
                    if (text) *text = (char *)out.p;
                    else agentc_buf_free(&out);
                    rc = 0;
                }
            }
        }
    }
    agentc_json_arena_free(doc);
    agentc_json_arena_free(scratch);
    return rc;
}

/* =========================================================================
 * Tool registry
 * ======================================================================= */
typedef struct {
    char *server;      /* original server name, owned */
    char *tool;        /* original tool name, owned */
    char *name;        /* exposed "mcp__…" name, owned */
    char *label;       /* "mcp <server>", owned */
    char *desc;        /* owned */
    char *schema;      /* raw inputSchema, owned or NULL */
    u32 flags;
    bool alive;
    bool seen;         /* tools/list re-sync bookkeeping */
    bool handed_out;   /* an AgcTool referencing this entry left the module */
    bool registered;   /* handed to the extension registry (agentc_ext_add_tool_internal) */
    bool publish_failed; /* the one publish attempt failed; never retried for this entry */
    int in_flight;     /* mcp_run() currently executing this entry (main loop) */
} McpToolEnt;

static AgcVec g_tools;    /* McpToolEnt* (stable heap blocks) */
/* True only while the mcp extension itself is loaded. The agentc_mcp_load()
 * test helper drives the same state machine but must not touch the extension
 * registry (it has no owner and the golden tests would leak its copies). */
static bool g_mcp_ext_loaded;
/* Set by the mcp extension's first pump: the server records are created there
 * (not in init) so trust has already been resolved. Cleared on shutdown. */
static bool g_mcp_started;

static bool registry_name_taken(const char *name, void *ud) {
    (void)ud;
    for (size_t i = 0; i < g_tools.len; i++) {
        McpToolEnt *e = ((McpToolEnt **)g_tools.p)[i];
        if (!e->alive || !e->name) continue;
        if (agentc_streq(e->name, name)) return true;
    }
    return false;
}

/* Build a unique exposed name: the base, or base + hash suffix when the base
 * is taken by another live entry. The suffix is never clipped, only the base,
 * so two long colliding identities stay distinct inside `cap`. `taken` checks
 * one registry's live names. */
typedef bool (*McpNameTakenFn)(const char *name, void *ud);

static void mcp_unique_name(char *out, size_t cap, const char *base,
                            const char *server, const char *tool,
                            McpNameTakenFn taken, void *ud) {
    if (out == NULL || cap == 0) return;
    agentc_snprintf(out, cap, "%s", base);
    out[cap - 1] = 0;
    if (!taken(out, ud)) return;
    u32 h = agentc_mcp_name_hash(server, tool);
    char suffix[32];
    for (int k = 0; k < 1000; k++) {
        if (k == 0) agentc_snprintf(suffix, sizeof suffix, "_%08x", (unsigned)h);
        else agentc_snprintf(suffix, sizeof suffix, "_%08x_%d", (unsigned)h, k);
        /* Keep the suffix inside the name cap: clip the base, never the hash. */
        size_t max = cap - 1;
        size_t sl = agentc_strlen(suffix);
        if (sl > max) sl = max;
        size_t keep = agentc_strlen(base);
        if (keep > max - sl) keep = max - sl;
        agentc_memcpy(out, base, keep);
        agentc_memcpy(out + keep, suffix, sl);
        out[keep + sl] = 0;
        if (!taken(out, ud)) return;
    }
}

/* Release an entry the module owns outright (never handed out, so no AgcTool
 * can reference it). */
static void registry_free_entry(McpToolEnt *e) {
    agentc_free(e->server);
    agentc_free(e->tool);
    agentc_free(e->name);
    agentc_free(e->label);
    agentc_free(e->desc);
    agentc_free(e->schema);
    agentc_memset(e, 0, sizeof *e);
}

/* Remove an entry from the live set. A published entry also has to disappear
 * from the extension registry, otherwise the old name stays advertised and a
 * re-published replacement collides with it (-17). Removal by name is safe:
 * the registry drops its own copies but never touches the MCP-owned entry, so
 * an in-flight transcript that still references it stays readable. */
static void registry_retire_entry(McpToolEnt *e) {
    if (e->registered) {
        if (g_mcp_ext_loaded && e->name)
            (void)agentc_ext_remove_tool_internal(e->name);
        e->registered = false;
    }
    if (e->handed_out) {
        e->alive = false;
        e->seen = false;
        return;
    }
    registry_free_entry(e);
}

/* Entries retired but still referenced by a table snapshot or an in-flight
 * transcript. */
static size_t registry_frozen_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < g_tools.len; i++) {
        McpToolEnt *e = ((McpToolEnt **)g_tools.p)[i];
        if (e && !e->alive && e->handed_out) n++;
    }
    return n;
}

/* Freezing is the only way an entry enters the frozen set, and a handed-out
 * entry may never be freed or reused. Once the pile is full the caller refuses
 * the new identity and fails the server instead of recycling a live block. */
static bool registry_frozen_room(void) {
    return registry_frozen_count() < MCP_FROZEN_CAP;
}

void agentc_mcp_registry_reset(void) {
    for (size_t i = 0; i < g_tools.len; i++) {
        McpToolEnt *e = ((McpToolEnt **)g_tools.p)[i];
        if (!e) continue;
        /* Retire the registry record first (mirrors registry_retire_entry): a
         * copy the agent still holds then resolves to the registry's clean
         * "no longer available" error and never calls mcp_run() with this block. */
        if (e->registered && g_mcp_ext_loaded && e->name)
            (void)agentc_ext_remove_tool_internal(e->name);
        e->registered = false;
        registry_free_entry(e);
        agentc_free(e);
    }
    agentc_vec_free(&g_tools);
}

static bool registry_schema_same(const char *a, const char *b) {
    if (a == NULL || b == NULL) return a == b;
    return agentc_streq(a, b);
}

int agentc_mcp_registry_add(const char *server, const char *tool, const char *desc,
                        const char *schema_json, u32 flags) {
    if (!server || !tool) return MC_EINVAL;
    size_t found = g_tools.len;
    McpToolEnt *e = NULL;
    for (size_t i = 0; i < g_tools.len; i++) {
        McpToolEnt *cand = ((McpToolEnt **)g_tools.p)[i];
        if (cand->alive && cand->server && cand->tool &&
            agentc_streq(cand->server, server) && agentc_streq(cand->tool, tool)) {
            found = i;
            e = cand;
            break;
        }
    }
    if (e) {
        bool same = agentc_streq(e->desc ? e->desc : "", desc ? desc : "") &&
                    registry_schema_same(e->schema, schema_json) && e->flags == flags;
        if (same) {
            e->seen = true;
            return 0;
        }
        if (!e->handed_out) {
            registry_free_entry(e);   /* refill the same, never-handed-out slot */
        } else {
            /* A caller already holds this identity and its strings: freeze the
             * old block and publish the updated tool as a fresh entry. When the
             * frozen pile is full, refuse (the caller fails the server): reusing
             * the block would redirect a stale AgcTool to a different identity. */
            if (!registry_frozen_room()) return MC_EFROZEN;
            registry_retire_entry(e);
            found = g_tools.len;
        }
    }
    /* A new identity counts against the per-server and total budgets; an
     * update to a live identity does not. The caller (mcp_sync_commit) turns
     * -ENOSPC/-EFROZEN into a logged server failure. */
    size_t per_server = 0, total = 0;
    for (size_t i = 0; i < g_tools.len; i++) {
        McpToolEnt *c = ((McpToolEnt **)g_tools.p)[i];
        if (!c || !c->alive) continue;
        total++;
        if (c->server && agentc_streq(c->server, server)) per_server++;
    }
    if (per_server >= MCP_MAX_TOOLS_PER_SERVER || total >= MCP_MAX_TOOLS_TOTAL)
        return MC_ENOSPC;
    size_t slot = found;
    if (slot == g_tools.len) {
        for (size_t i = 0; i < g_tools.len; i++) {
            McpToolEnt *cand = ((McpToolEnt **)g_tools.p)[i];
            if (!cand->alive && !cand->handed_out) {
                slot = i;
                break;
            }
        }
    }
    if (slot == g_tools.len) {
        e = agentc_alloc(sizeof *e);
        *(McpToolEnt **)agentc_vec_push(&g_tools, sizeof(McpToolEnt *)) = e;
    } else {
        e = ((McpToolEnt **)g_tools.p)[slot];
    }
    registry_free_entry(e);
    e->server = agentc_strdup(server);
    e->tool = agentc_strdup(tool);
    char base[160];
    agentc_mcp_tool_name(server, tool, base, sizeof base);
    char cand[MCP_EXPOSED_NAME_MAX];
    mcp_unique_name(cand, sizeof cand, base, server, tool, registry_name_taken, NULL);
    e->name = agentc_strdup(cand);
    AgcBuf lb = { 0 };
    agentc_buf_cstr(&lb, "mcp ");
    agentc_buf_cstr(&lb, server);
    e->label = (char *)lb.p;
    e->desc = agentc_strdup(desc ? desc : "");
    e->schema = schema_json ? agentc_strdup(schema_json) : NULL;
    e->flags = flags;
    e->alive = true;
    e->seen = true;
    return 0;
}

/* =========================================================================
 * Prompt registry
 *
 * MCP prompts live in two places: this module's stable McpPromptEnt records
 * (owner of the argument list and the expand callback's userdata) and the core
 * prompt registry in src/core/prompts.c, which the TUI menu reads. The core
 * copies names/descriptions and never frees a record, so an entry that was
 * ever listed is frozen here (alive=false, never freed or reused) and the
 * exposed name is retired through agentc_prompts_remove(). A full frozen pile
 * fails that server's prompt sync instead of recycling an entry whose core
 * record a menu may still reference.
 * ======================================================================= */
typedef struct {
    char *server;      /* original server name, owned */
    char *prompt;      /* remote prompt name, owned */
    char *name;        /* exposed "mcp__…" name, owned */
    char *desc;        /* owned (<= MCP_MAX_PROMPT_DESC) */
    McpParsedArg args[MCP_MAX_PROMPT_ARGS];  /* owned strings */
    size_t nargs;
    bool alive;
    bool seen;         /* prompts/list re-sync bookkeeping */
    bool listed;       /* handed to the core prompt registry at least once */
    bool registered;   /* currently live in the core prompt registry */
} McpPromptEnt;

static AgcVec g_prompts;    /* McpPromptEnt* (stable heap blocks) */

static void prompt_ent_free(McpPromptEnt *e) {
    agentc_free(e->server);
    agentc_free(e->prompt);
    agentc_free(e->name);
    agentc_free(e->desc);
    prompt_args_free(e->args, e->nargs);
    agentc_memset(e, 0, sizeof *e);
}

static size_t prompt_live_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < g_prompts.len; i++) {
        McpPromptEnt *e = ((McpPromptEnt **)g_prompts.p)[i];
        if (e && e->alive) n++;
    }
    return n;
}

static size_t prompt_live_count_server(const char *server) {
    size_t n = 0;
    for (size_t i = 0; i < g_prompts.len; i++) {
        McpPromptEnt *e = ((McpPromptEnt **)g_prompts.p)[i];
        if (e && e->alive && e->server && agentc_streq(e->server, server)) n++;
    }
    return n;
}

static size_t prompt_frozen_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < g_prompts.len; i++) {
        McpPromptEnt *e = ((McpPromptEnt **)g_prompts.p)[i];
        if (e && !e->alive && e->listed) n++;
    }
    return n;
}

static bool prompt_frozen_room(void) {
    return prompt_frozen_count() < MCP_PROMPTS_FROZEN_CAP;
}

static bool prompt_name_taken(const char *name, void *ud) {
    (void)ud;
    for (size_t i = 0; i < g_prompts.len; i++) {
        McpPromptEnt *e = ((McpPromptEnt **)g_prompts.p)[i];
        if (e && e->alive && e->name && agentc_streq(e->name, name)) return true;
    }
    return false;
}

static bool prompt_same(const McpPromptEnt *e, const McpParsedPrompt *p) {
    if (!agentc_streq(e->desc, p->description ? p->description : "")) return false;
    if (e->nargs != p->nargs) return false;
    for (size_t i = 0; i < e->nargs; i++) {
        if (!agentc_streq(e->args[i].name, p->args[i].name)) return false;
        if (!agentc_streq(e->args[i].description, p->args[i].description ? p->args[i].description : ""))
            return false;
        if (e->args[i].required != p->args[i].required) return false;
    }
    return true;
}

/* Fill `e` from `p`. The caller owns the slot choice and the exposed name. */
static void prompt_ent_set(McpPromptEnt *e, const char *server,
                           const McpParsedPrompt *p, const char *exposed) {
    prompt_ent_free(e);
    e->server = agentc_strdup(server);
    e->prompt = agentc_strdup(p->name);
    e->name = agentc_strdup(exposed);
    e->desc = agentc_strdup(p->description ? p->description : "");
    e->nargs = p->nargs < MCP_MAX_PROMPT_ARGS ? p->nargs : MCP_MAX_PROMPT_ARGS;
    for (size_t i = 0; i < e->nargs; i++) {
        e->args[i].name = agentc_strdup(p->args[i].name);
        e->args[i].description = agentc_strdup(p->args[i].description ? p->args[i].description : "");
        e->args[i].required = p->args[i].required;
    }
    e->alive = true;
    e->seen = true;
}

/* A slot that was never listed can be reused; a listed entry is frozen for
 * good (its strings and the core record it fed stay readable). */
static McpPromptEnt *prompt_slot_alloc(void) {
    for (size_t i = 0; i < g_prompts.len; i++) {
        McpPromptEnt *e = ((McpPromptEnt **)g_prompts.p)[i];
        if (e && !e->alive && !e->listed) return e;
    }
    McpPromptEnt *e = agentc_alloc(sizeof *e);
    *(McpPromptEnt **)agentc_vec_push(&g_prompts, sizeof(McpPromptEnt *)) = e;
    return e;
}

void agentc_mcp_prompt_registry_reset(void) {
    for (size_t i = 0; i < g_prompts.len; i++) {
        McpPromptEnt *e = ((McpPromptEnt **)g_prompts.p)[i];
        if (e->registered && e->name) (void)agentc_prompts_remove(e->name);
        prompt_ent_free(e);
        agentc_free(e);
    }
    agentc_vec_free(&g_prompts);
}

size_t agentc_mcp_prompt_count(void) { return prompt_live_count(); }

bool agentc_mcp_prompt_exposed(const char *server, const char *prompt, char *out, size_t cap) {
    for (size_t i = 0; i < g_prompts.len; i++) {
        McpPromptEnt *e = ((McpPromptEnt **)g_prompts.p)[i];
        if (!e->alive || !e->server || !e->prompt) continue;
        if (agentc_streq(e->server, server) && agentc_streq(e->prompt, prompt)) {
            if (out && cap) agentc_snprintf(out, cap, "%s", e->name);
            return true;
        }
    }
    return false;
}

/* Retire `e` from the live set: remove its core record (by exposed name) and
 * freeze it. Callers must have checked prompt_frozen_room(). */
static void prompt_ent_retire(McpPromptEnt *e) {
    if (e->registered) {
        (void)agentc_prompts_remove(e->name);
        e->registered = false;
    }
    e->alive = false;
    e->seen = false;
}

/* True when `e` is one of the entries created earlier in this commit. */
static bool prompt_added_contains(const AgcVec *added, const McpPromptEnt *e) {
    for (size_t i = 0; i < added->len; i++)
        if (((McpPromptEnt **)added->p)[i] == e) return true;
    return false;
}

/* Undo a failed multi-entry prompt commit. `added` holds entries this commit
 * created (registered in the core prompt registry) and `retired` holds entries
 * it retired. A live entry is always `listed`, so prompt_slot_alloc() never
 * reuses a slot retired in the same commit: the two sets are disjoint and
 * restore-in-place is safe. */
static void prompt_commit_rollback(const char *server, const AgcVec *added,
                                   const AgcVec *retired) {
    for (size_t i = 0; i < added->len; i++) {
        McpPromptEnt *e = ((McpPromptEnt **)added->p)[i];
        if (e->registered && e->name) (void)agentc_prompts_remove(e->name);
        prompt_ent_free(e);
    }
    for (size_t i = 0; i < retired->len; i++) {
        McpPromptEnt *e = ((McpPromptEnt **)retired->p)[i];
        e->alive = true;
        e->registered = false;
        if (agentc_prompts_register(e->name, e->desc, "", "mcp", e,
                                    agentc_mcp_prompt_expand) == 0)
            e->registered = true;
    }
    /* The previous table marked every live entry seen; restore that so a later
     * successful commit does not treat these entries as unseen. */
    for (size_t i = 0; i < g_prompts.len; i++) {
        McpPromptEnt *e = ((McpPromptEnt **)g_prompts.p)[i];
        if (e->alive && e->server && agentc_streq(e->server, server)) e->seen = true;
    }
}

int agentc_mcp_prompt_commit(const char *server, const AgcVec *parsed) {
    if (!server || !parsed) return MC_EINVAL;
    for (size_t i = 0; i < g_prompts.len; i++) {
        McpPromptEnt *e = ((McpPromptEnt **)g_prompts.p)[i];
        if (e->alive && e->server && agentc_streq(e->server, server)) e->seen = false;
    }
    /* A cap or core-registry refusal must keep the previous table intact, so
     * journal every mutation and roll it back on the first failure instead of
     * leaving a half-updated live table and core prompt registry. */
    AgcVec added = { 0 }, retired = { 0 };
    int fail = 0;
    for (size_t i = 0; i < parsed->len && fail == 0; i++) {
        const McpParsedPrompt *p = &((const McpParsedPrompt *)parsed->p)[i];
        McpPromptEnt *cur = NULL;
        for (size_t j = 0; j < g_prompts.len; j++) {
            McpPromptEnt *e = ((McpPromptEnt **)g_prompts.p)[j];
            if (e->alive && e->server && e->prompt && agentc_streq(e->server, server) &&
                agentc_streq(e->prompt, p->name)) {
                cur = e;
                break;
            }
        }
        if (cur && prompt_same(cur, p)) {
            cur->seen = true;
            continue;
        }
        if (cur) {
            /* A changed listed entry freezes; a never-listed one is reusable. */
            if (cur->listed && !prompt_frozen_room()) {
                agentc_logf(3, "mcp: server \"%s\": prompt frozen cap reached (%d); "
                            "failing prompt sync: %s", server,
                            (int)MCP_PROMPTS_FROZEN_CAP, p->name);
                fail = MC_EFROZEN;
                break;
            }
            /* An entry added earlier in this same commit is not part of the
             * pre-commit table: drop it via `added`, do not restore it. */
            if (!prompt_added_contains(&added, cur))
                *(McpPromptEnt **)agentc_vec_push(&retired, sizeof(McpPromptEnt *)) = cur;
            prompt_ent_retire(cur);
        } else {
            if (prompt_live_count_server(server) >= MCP_MAX_PROMPTS_PER_SERVER) {
                agentc_logf(3, "mcp: server \"%s\": prompts/list exceeds the per-server "
                            "cap (%d): %s", server, (int)MCP_MAX_PROMPTS_PER_SERVER, p->name);
                fail = MC_ENOSPC;
                break;
            }
            if (prompt_live_count() >= MCP_MAX_PROMPTS_TOTAL) {
                agentc_logf(3, "mcp: server \"%s\": prompts/list exceeds the total cap "
                            "(%d): %s", server, (int)MCP_MAX_PROMPTS_TOTAL, p->name);
                fail = MC_ENOSPC;
                break;
            }
        }
        char base[160];
        agentc_mcp_tool_name(server, p->name, base, sizeof base);
        char cand[MCP_EXPOSED_NAME_MAX];
        mcp_unique_name(cand, sizeof cand, base, server, p->name, prompt_name_taken, NULL);
        McpPromptEnt *e = prompt_slot_alloc();
        prompt_ent_set(e, server, p, cand);
        int rc = agentc_prompts_register(e->name, e->desc, "", "mcp", e,
                                         agentc_mcp_prompt_expand);
        if (rc != 0) {
            /* The core registry cap: fail the sync, keep whatever was live. */
            prompt_ent_free(e);
            fail = rc;
            break;
        }
        e->registered = true;
        e->listed = true;
        *(McpPromptEnt **)agentc_vec_push(&added, sizeof(McpPromptEnt *)) = e;
    }
    for (size_t i = 0; i < g_prompts.len && fail == 0; i++) {
        McpPromptEnt *e = ((McpPromptEnt **)g_prompts.p)[i];
        if (!(e->alive && e->server && agentc_streq(e->server, server) && !e->seen)) continue;
        if (e->listed && !prompt_frozen_room()) {
            agentc_logf(3, "mcp: server \"%s\": prompt frozen cap reached (%d); failing "
                        "prompt sync: %s", server, (int)MCP_PROMPTS_FROZEN_CAP,
                        e->name ? e->name : "?");
            fail = MC_EFROZEN;
            break;
        }
        *(McpPromptEnt **)agentc_vec_push(&retired, sizeof(McpPromptEnt *)) = e;
        prompt_ent_retire(e);
    }
    if (fail != 0) prompt_commit_rollback(server, &added, &retired);
    agentc_vec_free(&added);
    agentc_vec_free(&retired);
    return fail;
}

/* =========================================================================
 * Resource/template registry
 *
 * Unlike tools and prompts these records are never handed out as an AgcTool
 * or a core registry record: the two generic list tools build their JSON from
 * the live table on demand, so a committed list simply replaces the server's
 * previous entries (validated first, so a cap refusal keeps the old table).
 * Nothing outside this module holds a McpResourceEnt*, so unlike the frozen
 * tool/prompt piles no retired block has to survive.
 * ======================================================================= */
typedef struct {
    char *server;      /* original server name, owned */
    char *uri;         /* owned */
    char *name;        /* owned, "" when absent */
    char *title;       /* owned, "" when absent (resources only) */
    char *description; /* owned, "" when absent */
    char *mime;        /* owned, "" when absent */
} McpResourceEnt;

typedef struct {
    char *server;       /* original server name, owned */
    char *uri_template; /* owned */
    char *name;         /* owned, "" when absent */
    char *description;  /* owned, "" when absent */
    char *mime;         /* owned, "" when absent */
} McpResourceTemplateEnt;

static AgcVec g_resources;          /* McpResourceEnt* */
static AgcVec g_resource_templates; /* McpResourceTemplateEnt* */

static void resource_ent_free(McpResourceEnt *e) {
    agentc_free(e->server);
    agentc_free(e->uri);
    agentc_free(e->name);
    agentc_free(e->title);
    agentc_free(e->description);
    agentc_free(e->mime);
    agentc_memset(e, 0, sizeof *e);
}

static void resource_template_ent_free(McpResourceTemplateEnt *e) {
    agentc_free(e->server);
    agentc_free(e->uri_template);
    agentc_free(e->name);
    agentc_free(e->description);
    agentc_free(e->mime);
    agentc_memset(e, 0, sizeof *e);
}

size_t agentc_mcp_resource_count(bool templates) {
    return templates ? g_resource_templates.len : g_resources.len;
}

static size_t resource_count_server(const char *server, bool templates) {
    AgcVec *table = templates ? &g_resource_templates : &g_resources;
    size_t n = 0;
    for (size_t i = 0; i < table->len; i++) {
        const char *s = templates
                            ? ((McpResourceTemplateEnt **)table->p)[i]->server
                            : ((McpResourceEnt **)table->p)[i]->server;
        if (s && server && agentc_streq(s, server)) n++;
    }
    return n;
}

void agentc_mcp_resource_registry_reset(void) {
    for (size_t i = 0; i < g_resources.len; i++) {
        McpResourceEnt *e = ((McpResourceEnt **)g_resources.p)[i];
        resource_ent_free(e);
        agentc_free(e);
    }
    agentc_vec_free(&g_resources);
    for (size_t i = 0; i < g_resource_templates.len; i++) {
        McpResourceTemplateEnt *e = ((McpResourceTemplateEnt **)g_resource_templates.p)[i];
        resource_template_ent_free(e);
        agentc_free(e);
    }
    agentc_vec_free(&g_resource_templates);
}

int agentc_mcp_resource_commit(const char *server, const AgcVec *parsed, bool templates) {
    if (!server || !parsed) return MC_EINVAL;
    const char *kind = templates ? "resource templates/list" : "resources/list";
    if (parsed->len > AGENTC_LIMIT_MCP_RESOURCES_PER_SERVER) {
        agentc_logf(3, "mcp: server \"%s\": %s exceeds the per-server cap (%d)", server,
                    kind, (int)AGENTC_LIMIT_MCP_RESOURCES_PER_SERVER);
        return MC_ENOSPC;
    }
    AgcVec *table = templates ? &g_resource_templates : &g_resources;
    size_t live_this = resource_count_server(server, templates);
    if (table->len - live_this + parsed->len > AGENTC_LIMIT_MCP_RESOURCES_TOTAL) {
        agentc_logf(3, "mcp: server \"%s\": %s exceeds the total cap (%d)", server, kind,
                    (int)AGENTC_LIMIT_MCP_RESOURCES_TOTAL);
        return MC_ENOSPC;
    }
    /* Drop this server's previous table (nothing references it), then copy
     * the freshly committed list in page order. */
    size_t w = 0;
    for (size_t i = 0; i < table->len; i++) {
        void *e = ((void **)table->p)[i];
        const char *s = templates ? ((McpResourceTemplateEnt *)e)->server
                                  : ((McpResourceEnt *)e)->server;
        if (s && agentc_streq(s, server)) {
            if (templates) resource_template_ent_free(e);
            else resource_ent_free(e);
            agentc_free(e);
            continue;
        }
        ((void **)table->p)[w++] = e;
    }
    table->len = w;
    for (size_t i = 0; i < parsed->len; i++) {
        if (templates) {
            const McpParsedResourceTemplate *p =
                &((const McpParsedResourceTemplate *)parsed->p)[i];
            McpResourceTemplateEnt *e = agentc_alloc(sizeof *e);
            e->server = agentc_strdup(server);
            e->uri_template = agentc_strdup(p->uri_template ? p->uri_template : "");
            e->name = agentc_strdup(p->name ? p->name : "");
            e->description = agentc_strdup(p->description ? p->description : "");
            e->mime = agentc_strdup(p->mime ? p->mime : "");
            *(McpResourceTemplateEnt **)agentc_vec_push(table, sizeof e) = e;
        } else {
            const McpParsedResource *p = &((const McpParsedResource *)parsed->p)[i];
            McpResourceEnt *e = agentc_alloc(sizeof *e);
            e->server = agentc_strdup(server);
            e->uri = agentc_strdup(p->uri ? p->uri : "");
            e->name = agentc_strdup(p->name ? p->name : "");
            e->title = agentc_strdup(p->title ? p->title : "");
            e->description = agentc_strdup(p->description ? p->description : "");
            e->mime = agentc_strdup(p->mime ? p->mime : "");
            *(McpResourceEnt **)agentc_vec_push(table, sizeof e) = e;
        }
    }
    return 0;
}

/* One entry as a JSON object. `server` is always present so a merged page is
 * self-describing; the outer `server` field still names the single-server
 * case. Serialized into scratch first so a page can stop on a byte budget. */
static bool mcp_resource_server_live(const char *name);   /* defined with the server state */
static int mcp_resource_list_json(bool templates, const char *server, const char *cursor,
                                  bool live_only, char **out);
static void resource_entry_json(AgcBuf *b, const McpResourceEnt *r) {
    AgcJsonW w;
    agentc_jsonw_init(&w, b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "server");
    agentc_jsonw_cstr(&w, r->server ? r->server : "");
    agentc_jsonw_key(&w, "uri");
    agentc_jsonw_cstr(&w, r->uri ? r->uri : "");
    agentc_jsonw_key(&w, "name");
    agentc_jsonw_cstr(&w, r->name ? r->name : "");
    agentc_jsonw_key(&w, "title");
    agentc_jsonw_cstr(&w, r->title ? r->title : "");
    agentc_jsonw_key(&w, "description");
    agentc_jsonw_cstr(&w, r->description ? r->description : "");
    agentc_jsonw_key(&w, "mimeType");
    agentc_jsonw_cstr(&w, r->mime ? r->mime : "");
    agentc_jsonw_end(&w);
}

static void resource_template_entry_json(AgcBuf *b, const McpResourceTemplateEnt *r) {
    AgcJsonW w;
    agentc_jsonw_init(&w, b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "server");
    agentc_jsonw_cstr(&w, r->server ? r->server : "");
    agentc_jsonw_key(&w, "uriTemplate");
    agentc_jsonw_cstr(&w, r->uri_template ? r->uri_template : "");
    agentc_jsonw_key(&w, "name");
    agentc_jsonw_cstr(&w, r->name ? r->name : "");
    agentc_jsonw_key(&w, "description");
    agentc_jsonw_cstr(&w, r->description ? r->description : "");
    agentc_jsonw_key(&w, "mimeType");
    agentc_jsonw_cstr(&w, r->mime ? r->mime : "");
    agentc_jsonw_end(&w);
}

int agentc_mcp_resource_list_json(bool templates, const char *server, const char *cursor,
                                  char **out) {
    return mcp_resource_list_json(templates, server, cursor, false, out);
}

/* Shared page builder. live_only skips entries whose server has since left the
 * READY resource-capable set (a READY server can fail on a hostile tools/list
 * re-sync and keep its old resource rows). The public entry point above is the
 * pure table view used by the registry tests. */
static int mcp_resource_list_json(bool templates, const char *server, const char *cursor,
                                  bool live_only, char **out) {
    if (!out) return MC_EINVAL;
    *out = NULL;
    size_t start = 0;
    if (cursor && cursor[0]) {
        bool ok = false;
        u64 v = agentc_parse_u64(cursor, agentc_strlen(cursor), &ok);
        if (!ok) return MC_EINVAL;
        start = (size_t)v;
    }
    AgcVec *table = templates ? &g_resource_templates : &g_resources;
    AgcBuf buf = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &buf);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "server");
    if (server) agentc_jsonw_cstr(&w, server);
    else agentc_jsonw_null(&w);
    agentc_jsonw_key(&w, templates ? "templates" : "resources");
    agentc_jsonw_arr(&w);
    size_t filtered = 0, added = 0;
    bool more = false;
    int rc = 0;
    for (size_t i = 0; i < table->len; i++) {
        void *e = ((void **)table->p)[i];
        const char *s = templates ? ((McpResourceTemplateEnt *)e)->server
                                  : ((McpResourceEnt *)e)->server;
        if (server && (!s || !agentc_streq(s, server))) continue;
        if (live_only && (!s || !mcp_resource_server_live(s))) continue;
        if (filtered < start) {
            filtered++;
            continue;
        }
        AgcBuf eb = { 0 };
        if (templates) resource_template_entry_json(&eb, e);
        else resource_entry_json(&eb, e);
        /* +2 closes the array/object; the remaining reserve covers the largest
         * `,"nextCursor":"<20 digits>"` suffix (~38 bytes). */
        if (buf.len + eb.len + 40 > AGENTC_LIMIT_MCP_RESOURCES_JSON_BYTES) {
            if (added == 0) {
                agentc_buf_free(&eb);
                rc = MC_E2BIG;
                break;
            }
            agentc_buf_free(&eb);
            more = true;
            break;
        }
        agentc_jsonw_raw(&w, (const char *)eb.p, eb.len);
        agentc_buf_free(&eb);
        added++;
        filtered++;
    }
    if (rc == 0 && more) {
        char num[24];
        agentc_snprintf(num, sizeof num, "%llu", (unsigned long long)(start + added));
        agentc_jsonw_key(&w, "nextCursor");
        agentc_jsonw_cstr(&w, num);
    }
    agentc_jsonw_end(&w);   /* array */
    agentc_jsonw_end(&w);   /* object */
    if (rc != 0) {
        agentc_buf_free(&buf);
        return rc;
    }
    *out = (char *)buf.p;
    return 0;
}

/* =========================================================================
 * Servers and transports
 * ======================================================================= */
/* Per-server connect/re-sync state machine. Each agentc_mcp_pump() call
 * advances one state transition on one server (round-robin): one bounded stdio
 * write, one non-blocking stdio read attempt, or one short HTTP
 * request/response slice. */
enum {
    MCP_ST_NEW = 0,     /* configure/spawn the transport */
    MCP_ST_INIT_SEND,   /* send initialize */
    MCP_ST_INIT_WAIT,   /* await initialize response (stdio) */
    MCP_ST_NOTIFY,      /* send notifications/initialized */
    MCP_ST_SYNC_SEND,   /* send tools/list for the current cursor */
    MCP_ST_SYNC_WAIT,   /* await tools/list response (stdio) */
    MCP_ST_READY,
    MCP_ST_FAILED,
};

typedef struct {
    char *name;        /* points into cfg.name */
    int kind;          /* 0 = stdio, 1 = http */
    /* stdio */
    int wfd, rfd, pid;
    int hold_rfd;      /* read end kept open in the parent so writes never SIGPIPE */
    /* http */
    char *url, *headers, *session_id;
    /* common */
    i64 timeout_ms;
    u64 next_id;
    u32 caps;          /* MCP_CAP_* parsed from initialize */
    bool inited;       /* an initialize response was accepted (caps are real) */
    AgcBuf rbuf;        /* leftover stdio lines */
    char err[256];

    /* state machine */
    McpCfg cfg;         /* owns command/args/env; name/url/headers point here */
    int state;
    bool ready_once;    /* connected at least once: a later timeout aborts sync */
    i64 deadline;       /* ns; connect or per-kind wall-clock deadline */
    u64 pending_id;     /* id of the request currently awaited */

    /* reconnect: set when the server enters FAILED. retry_at is the
     * monotonic ns deadline after which the next pump tears the transport
     * down, clears session/sync/caps state and re-enters NEW; retry_count is
     * the number of consecutive failures, selecting the backoff shift. */
    int retry_count;
    i64 retry_at;

    /* per-kind list sync: one slot per MCP_KIND_*, walked in enum order
     * (tools -> prompts -> resources -> resource templates) after every
     * notifications/initialized. sync_kind is the kind in flight, -1 when
     * none. Each kind keeps its own pending/again latch, pagination cursor and
     * page count. */
    struct {
        bool pending;   /* a list is due (connect or list_changed) */
        bool again;     /* list_changed latched while this kind was in flight */
        char *cursor;   /* pagination cursor, owned */
        int pages;      /* pages consumed this round */
    } sync[MCP_KIND_COUNT];
    int sync_kind;                       /* MCP_KIND_* in flight, -1 when none */
    AgcVec sync_parsed[MCP_KIND_COUNT];  /* one parse accumulation per kind */
} McpServer;

static AgcVec g_servers;    /* McpServer* */
static size_t g_pump_cursor; /* round-robin: server index stepped by the next pump */

static const char *mcp_err_name(int rc) {
    switch (rc) {
    case MC_ETIMEDOUT: return "timeout";
    case MC_ECONNRESET: return "connection closed";
    case MC_EPIPE: return "broken pipe";
    case MC_ECANCELED: return "cancelled";
    case MC_EPROTO: return "protocol error";
    case MC_E2BIG: return "message too large";
    case MC_EINVAL: return "invalid config";
    case MC_ENOENT: return "not found";
    case MC_EEXIST: return "duplicate name";
    case MC_ENOSPC: return "resource cap reached";
    case MC_EFROZEN: return "frozen retired-entry cap reached";
    case MC_EIO: return "server error";
    case MCP_ERR_BINARY: return "binary resource not supported";
    default: return "transport error";
    }
}

static void mcp_set_err(McpServer *s, int rc, const char *detail) {
    if (!s) return;
    if (detail && detail[0]) agentc_snprintf(s->err, sizeof s->err, "%s", detail);
    else agentc_snprintf(s->err, sizeof s->err, "%s", mcp_err_name(rc));
}

static McpServer *mcp_server_by_name(const char *name) {
    for (size_t i = 0; i < g_servers.len; i++) {
        McpServer *s = ((McpServer **)g_servers.p)[i];
        if (s && s->name && agentc_streq(s->name, name)) return s;
    }
    return NULL;
}

/* A list_changed hint for one kind. When it arrives while that kind's sync is
 * already running, the commit/fail path is about to clear pending, so latch a
 * follow-up in again: the refresh would otherwise be swallowed. The kind's
 * base capability must be advertised; otherwise the
 * notification is ignored (debug-logged), never fetched. */
static void mcp_note_list_changed(McpServer *s, int kind) {
    if (!s || kind < 0 || kind >= MCP_KIND_COUNT) return;
    if (!(s->caps & mcp_kind_cap(kind))) {
        agentc_logf(0, "mcp: %s: %s list_changed ignored (capability not advertised)",
                    s->name ? s->name : "?", mcp_kind_name(kind));
        return;
    }
    if ((s->state == MCP_ST_SYNC_SEND || s->state == MCP_ST_SYNC_WAIT) &&
        s->sync_kind == kind)
        s->sync[kind].again = true;
    s->sync[kind].pending = true;
}

/* ----------------------------------------------------------- env + spawn */
static char *mcp_env_kv(const char *k, const char *v) {
    size_t kl = agentc_strlen(k), vl = agentc_strlen(v);
    char *s = agentc_alloc(kl + 1 + vl + 1);
    agentc_memcpy(s, k, kl);
    s[kl] = '=';
    agentc_memcpy(s + kl + 1, v, vl + 1);
    return s;
}

static void mcp_push_str(AgcVec *v, const char *s) {
    *(char **)agentc_vec_push(v, sizeof(char *)) = (char *)s;
}

/* `raw` and `owned` must outlive os_spawn (envp entries point into them). */
static void mcp_build_envp(const McpCfg *cfg, AgcVec *envp, AgcVec *owned, AgcBuf *raw) {
    int fd = os_open("/proc/self/environ", OS_O_RDONLY, 0);
    if (fd >= 0) {
        u8 tmp[4096];
        for (;;) {
            int n = os_read(fd, tmp, sizeof tmp);
            if (n <= 0) break;
            agentc_buf_push(raw, tmp, (size_t)n);
        }
        os_close(fd);
    }
    if (raw->len) {
        char *p = (char *)raw->p;
        char *e = p + raw->len;
        while (p < e) {
            size_t l = 0;
            while (p + l < e && p[l]) l++;
            if (l) mcp_push_str(envp, p);
            p += l + 1;
        }
    } else {
        static const char *keys[] = {
            "PATH", "HOME", "USER", "LOGNAME", "SHELL", "TERM", "TMPDIR",
            "LANG", "LC_ALL", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME",
            /* Windows essentials: the /proc path above never runs there, and a
             * child without SystemRoot/APPDATA cannot start most tools. */
            "SystemRoot", "USERPROFILE", "APPDATA", "LOCALAPPDATA", "TEMP", "TMP",
            "COMSPEC", "PATHEXT",
        };
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            const char *v = agentc_env_get(keys[i]);
            if (!v) continue;
            char *kv = mcp_env_kv(keys[i], v);
            mcp_push_str(owned, kv);
            mcp_push_str(envp, kv);
        }
    }
    for (size_t i = 0; i < cfg->env.len; i++) {
        const char *kv = ((char **)cfg->env.p)[i];
        size_t kl = 0;
        while (kv[kl] && kv[kl] != '=') kl++;
        for (size_t j = 0; j < envp->len; j++) {
            const char *e = ((char **)envp->p)[j];
            if (!e) continue;
            if (agentc_strlen(e) >= kl && agentc_str_eq(e, kl, kv, kl) && e[kl] == '=')
                ((char **)envp->p)[j] = NULL;
        }
        mcp_push_str(envp, kv);
    }
    size_t w = 0;
    for (size_t i = 0; i < envp->len; i++) {
        char *e = ((char **)envp->p)[i];
        if (e) ((char **)envp->p)[w++] = e;
    }
    envp->len = w;
    mcp_push_str(envp, NULL);
}

static int mcp_spawn_stdio(McpServer *s, const McpCfg *cfg) {
    int inpipe[2], outpipe[2];
    int rc = os_pipe(inpipe);
    if (rc < 0) return rc;
    rc = os_pipe(outpipe);
    if (rc < 0) {
        os_close(inpipe[0]);
        os_close(inpipe[1]);
        return rc;
    }
    int devnull = os_open("/dev/null", OS_O_WRONLY, 0);

    /* os_pipe marks the read end O_NONBLOCK; a server reading its stdin with
     * blocking semantics (python's sys.stdin) treats EAGAIN as EOF. Reopen the
     * read end through /proc to get a fresh, blocking file description. */
    char procfd[48];
    agentc_snprintf(procfd, sizeof procfd, "/proc/self/fd/%d", inpipe[0]);
    int child_in = os_open(procfd, OS_O_RDONLY, 0);
    if (child_in < 0) child_in = inpipe[0];

    AgcVec argv = { 0 };
    /* PATH lookup: execve does not search $PATH, so route bare command names
     * through sh with a fixed format string (no quoting of user data). */
    if (agentc_str_str(cfg->command, "/") == NULL) {
        mcp_push_str(&argv, "/bin/sh");
        mcp_push_str(&argv, "-c");
        mcp_push_str(&argv, "exec \"$0\" \"$@\"");
    }
    mcp_push_str(&argv, cfg->command);
    for (size_t i = 0; i < cfg->args.len; i++) mcp_push_str(&argv, ((char **)cfg->args.p)[i]);
    mcp_push_str(&argv, NULL);

    AgcVec envp = { 0 };
    AgcVec owned = { 0 };
    AgcBuf raw = { 0 };
    mcp_build_envp(cfg, &envp, &owned, &raw);

    int pid = os_spawn_group((char *const *)argv.p, (char *const *)envp.p, NULL,
                             child_in, outpipe[1], devnull, 0);
    if (child_in != inpipe[0]) os_close(child_in);
    /* inpipe[0] stays open as a dummy reader: a server that dies mid-flight
     * then makes write() buffer instead of delivering SIGPIPE. The child still
     * sees EOF when we close wfd, because EOF tracks the write ends. */
    if (pid < 0) os_close(inpipe[0]);
    os_close(outpipe[1]);
    if (devnull >= 0) os_close(devnull);
    for (size_t i = 0; i < owned.len; i++) agentc_free(((char **)owned.p)[i]);
    agentc_vec_free(&owned);
    agentc_vec_free(&envp);
    agentc_buf_free(&raw);
    agentc_vec_free(&argv);
    if (pid < 0) {
        os_close(inpipe[1]);
        os_close(outpipe[0]);
        return pid;
    }
    /* Reopen the write end non-blocking so a full pipe surfaces as EAGAIN and
     * the bounded writer polls instead of sleeping inside a blocking write().
     * Fall back to the original blocking end where /proc is unavailable. */
    char procwfd[48];
    agentc_snprintf(procwfd, sizeof procwfd, "/proc/self/fd/%d", inpipe[1]);
    int wfd = os_open(procwfd, OS_O_WRONLY | OS_O_NONBLOCK, 0);
    if (wfd >= 0) {
        os_close(inpipe[1]);
        inpipe[1] = wfd;
    }
    s->wfd = inpipe[1];
    s->rfd = outpipe[0];
    s->hold_rfd = inpipe[0];
    s->pid = pid;
    return 0;
}

/* ------------------------------------------------------------ stdio I/O */
typedef struct {
    McpServer *s;
    const volatile bool *cancel;
} StdioRd;

static int mcp_stdio_read(void *ud, void *p, size_t n) {
    StdioRd *r = ud;
    if (r->s->rfd < 0) return 0;
    return os_read(r->s->rfd, p, n);
}

static int mcp_stdio_wait(void *ud, int timeout_ms) {
    StdioRd *r = ud;
    if (r->cancel && *r->cancel) return MC_ECANCELED;
    if (r->s->rfd < 0) return MC_EPIPE;
    if (timeout_ms <= 0 || timeout_ms > 100) timeout_ms = 100;
    struct os_pollfd f = { r->s->rfd, OS_POLLIN, 0 };
    int pr = os_poll(&f, 1, timeout_ms);
    if (pr < 0 && pr != -4) return pr;
    return 0;
}

static void mcp_stdio_notify(void *ud, const char *json, size_t n) {
    McpServer *s = ud;
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(a, json, n);
    const char *m = root ? agentc_json_get_str(root, "method") : NULL;
    if (m && agentc_streq(m, "notifications/tools/list_changed"))
        mcp_note_list_changed(s, MCP_KIND_TOOLS);
    else if (m && agentc_streq(m, "notifications/prompts/list_changed"))
        mcp_note_list_changed(s, MCP_KIND_PROMPTS);
    else if (m && agentc_streq(m, "notifications/resources/list_changed")) {
        /* One `resources` capability and one notification cover both kinds. */
        mcp_note_list_changed(s, MCP_KIND_RESOURCES);
        mcp_note_list_changed(s, MCP_KIND_RESOURCE_TEMPLATE);
    }
    /* progress/log notifications are intentionally ignored */
    agentc_json_arena_free(a);
}

/* Bounded stdio write. The parent keeps a dummy read end open, so a dead child
 * does not raise SIGPIPE: without a bound a request larger than the pipe buffer
 * would block the main loop forever. The first chunk is attempted write-first:
 * an empty anonymous pipe accepts data even where os_poll() never reports
 * POLLOUT (Wine's write-only pipes match no poll branch), so polling before the
 * first byte would time a small request out. Only an EAGAIN/EINTR probe falls
 * into the bounded poll path; later chunks poll before writing, keeping a
 * blocking fd from sleeping past the deadline, and each write is capped at
 * PIPE_BUF so one atomic chunk fits the pipe's reserve. Returns
 * 0 | -ETIMEDOUT | -ECANCELED | -EPIPE | -errno. */
int agentc_mcp_write_all(int fd, int pid, const void *p, size_t n, int timeout_ms,
                         const volatile bool *cancel, size_t *written) {
    size_t done = 0;
    if (written) *written = 0;
    if (fd < 0) return MC_EPIPE;
    if (timeout_ms <= 0) timeout_ms = 1;
    const u8 *q = p;
    const size_t chunk = 4096;   /* POSIX PIPE_BUF: a single atomic write */
    i64 deadline = os_now_ns(OS_CLOCK_MONOTONIC) + (i64)timeout_ms * 1000000;
    bool probed = false;
    while (n) {
        if (cancel && *cancel) { if (written) *written = done; return MC_ECANCELED; }
        if (pid > 0 && os_wait(pid, true) != -1) { if (written) *written = done; return MC_EPIPE; }
        if (!probed) {
            probed = true;
            size_t want = n > chunk ? chunk : n;
            int w = os_write(fd, q, want);
            if (w > 0) {
                q += w;
                n -= (size_t)w;
                done += (size_t)w;
                continue;
            }
            if (w == 0) { if (written) *written = done; return MC_EPIPE; }
            if (w != MC_EAGAIN && w != MC_EINTR) { if (written) *written = done; return w; }
            /* Pipe full: fall through to the bounded wait and retry this chunk. */
        }
        for (;;) {
            if (cancel && *cancel) { if (written) *written = done; return MC_ECANCELED; }
            i64 now = os_now_ns(OS_CLOCK_MONOTONIC);
            if (now >= deadline) { if (written) *written = done; return MC_ETIMEDOUT; }
            int slice = (int)((deadline - now) / 1000000) + 1;
            if (slice > 100) slice = 100;
            struct os_pollfd f = { fd, OS_POLLOUT, 0 };
            int pr = os_poll(&f, 1, slice);
            if (pr < 0) {
                if (pr == MC_EINTR) continue;
                if (written) *written = done;
                return pr;
            }
            if (pr == 0) continue;                       /* slice elapsed */
            if (f.revents & (OS_POLLERR | OS_POLLHUP)) { if (written) *written = done; return MC_EPIPE; }
            if (f.revents & OS_POLLOUT) break;
        }
        size_t want = n > chunk ? chunk : n;
        int w = os_write(fd, q, want);
        if (w > 0) {
            q += w;
            n -= (size_t)w;
            done += (size_t)w;
            continue;
        }
        if (w == 0) { if (written) *written = done; return MC_EPIPE; }
        if (w == MC_EAGAIN || w == MC_EINTR) continue;
        if (written) *written = done;
        return w;
    }
    if (written) *written = done;
    return 0;
}

/* ------------------------------------------------------------- HTTP I/O */
typedef struct {
    AgcHttp *h;
    AgcSse sse;
    bool sse_checked, sse_mode, found;
    bool body_capped;   /* on_body aborted because the body cap was hit */
    u32 changed_mask;   /* MCP_CAP_* kinds seen in list_changed notifications */
    AgcBuf body, found_json;
    u64 want;
} HttpCtx;

static bool ci_contains(const char *hay, const char *needle) {
    if (!hay) return false;
    size_t nl = agentc_strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i]) {
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            i++;
        }
        if (i == nl) return true;
    }
    return false;
}

static int http_sse_ev(void *ud, const AgcSseEvent *ev) {
    HttpCtx *c = ud;
    if (c->found) return 1;
    u64 id = 0;
    bool has = false;
    if (json_msg_id(ev->data, ev->data_len, &id, &has) != 0) return 0;
    if (has) {
        if (id == c->want) {
            agentc_buf_push(&c->found_json, ev->data, ev->data_len);
            c->found = true;
            return 1;
        }
        return 0;
    }
    /* Server notifications interleaved in the stream; progress/logs ignored.
     * The mask is applied after the request returns (mcp_http_post), so an
     * in-flight sync for the matching kind latches its follow-up. */
    if (agentc_str_str(ev->data, "notifications/tools/list_changed") != NULL)
        c->changed_mask |= MCP_CAP_TOOLS;
    if (agentc_str_str(ev->data, "notifications/prompts/list_changed") != NULL)
        c->changed_mask |= MCP_CAP_PROMPTS;
    if (agentc_str_str(ev->data, "notifications/resources/list_changed") != NULL)
        c->changed_mask |= MCP_CAP_RESOURCES;
    return 0;
}

static int http_body_cb(void *ud, const void *p, size_t n) {
    HttpCtx *c = ud;
    if (!c->sse_checked) {
        c->sse_checked = true;
        c->sse_mode = ci_contains(agentc_http_header(c->h, "Content-Type"), "text/event-stream");
        if (c->sse_mode) agentc_sse_init(&c->sse);
    }
    if (c->sse_mode) return agentc_sse_feed(&c->sse, p, n, http_sse_ev, c);
    if (c->body.len + n > AGENTC_LIMIT_MCP_BODY_BYTES) {
        c->body_capped = true;
        return 1;
    }
    agentc_buf_push(&c->body, p, n);
    return 0;
}

static void mcp_err_http(McpServer *s, HttpCtx *c, int status, int rc, const char *http_err) {
    if (c->body_capped) {
        /* A 2xx transfer aborted by the response-body cap must name the real
         * cause instead of the misleading "HTTP 200". */
        agentc_snprintf(s->err, sizeof s->err,
                        "HTTP %d: response body exceeds %u KiB", status,
                        (unsigned)(AGENTC_LIMIT_MCP_BODY_BYTES / 1024));
        return;
    }
    if (status >= 200 && status < 300 && rc != 0) {
        /* The status line arrived but the transfer did not complete: report
         * the underlying transport/protocol cause, never the status alone. */
        mcp_set_err(s, rc, http_err);
        return;
    }
    if (status >= 100) {
        char extra[128];
        extra[0] = 0;
        if (c->body.len) {
            AgcJsonArena *a = agentc_json_arena_new(0);
            AgcJson *root = agentc_json_parse_in(a, (const char *)c->body.p, c->body.len);
            AgcJson *e = root ? agentc_json_get(root, "error") : NULL;
            const char *m = e ? agentc_json_get_str(e, "message") : NULL;
            if (m) agentc_snprintf(extra, sizeof extra, ": %s", m);
            agentc_json_arena_free(a);
        }
        agentc_snprintf(s->err, sizeof s->err, "HTTP %d%s", status, extra);
        return;
    }
    mcp_set_err(s, rc, http_err);
}

/* The session id is echoed verbatim into the next request's header block.
 * The HTTP tokenizer splits response headers only on CRLF pairs, so a lone
 * CR/LF (or any other C0 control) smuggled into the value would start a new
 * header line on the next request. Reject such a value outright. */
static bool mcp_session_id_valid(const char *sid) {
    if (!sid || !sid[0]) return false;
    for (const unsigned char *p = (const unsigned char *)sid; *p; p++)
        if (*p < 0x20) return false;
    return true;
}

static int mcp_http_post(McpServer *s, const char *body, size_t blen, u64 want,
                         bool notification, int timeout_ms, const volatile bool *cancel,
                         char **out) {
    if (out) *out = NULL;
    if (cancel && *cancel) return MC_ECANCELED;
    if (!s->url) return MC_EINVAL;
    AgcBuf hd = { 0 };
    agentc_buf_cstr(&hd, "Content-Type: application/json\r\n");
    agentc_buf_cstr(&hd, "Accept: application/json, text/event-stream\r\n");
    agentc_buf_cstr(&hd, "MCP-Protocol-Version: 2024-11-05\r\n");
    if (s->session_id && s->session_id[0]) {
        agentc_buf_cstr(&hd, "Mcp-Session-Id: ");
        agentc_buf_cstr(&hd, s->session_id);
        agentc_buf_cstr(&hd, "\r\n");
    }
    if (s->headers && s->headers[0]) agentc_buf_cstr(&hd, s->headers);
    AgcHttp *h = agentc_http_new("POST", s->url, (char *)hd.p, body, blen);
    agentc_buf_free(&hd);
    if (!h) return MC_EINVAL;
    agentc_http_set_cancel(h, cancel);

    HttpCtx c;
    agentc_memset(&c, 0, sizeof c);
    c.h = h;
    c.want = want;
    int rc = agentc_http_run(h, http_body_cb, &c, timeout_ms);
    /* An SSE response ending at EOF without a blank line still carries the
     * server's final event; flush it before any consumer reads the parser
     * state (found reply, list_changed mask). Only a clean transfer is
     * flushed; a truncated final field line is discarded by the parser. */
    if (c.sse_mode && rc == 0) agentc_sse_finish(&c.sse, http_sse_ev, &c);
    if (c.changed_mask) {
        for (int k = 0; k < MCP_KIND_COUNT; k++)
            if (c.changed_mask & mcp_kind_cap(k)) mcp_note_list_changed(s, k);
    }
    int status = agentc_http_status(h);
    const char *sid = agentc_http_header(h, "Mcp-Session-Id");
    if (sid && sid[0]) {
        if (mcp_session_id_valid(sid)) {
            agentc_free(s->session_id);
            s->session_id = agentc_strdup(sid);
        } else {
            agentc_logf(2, "mcp: %s: ignoring Mcp-Session-Id with a control "
                        "character", s->name ? s->name : "?");
        }
    }
    /* An SSE response ending at EOF without a blank line still carries the
     * server's final event; flush it before deciding whether the reply was
     * found. Only a clean transfer is flushed. */
    int result;
    if (c.found) {
        if (out) *out = (char *)c.found_json.p;
        else agentc_buf_free(&c.found_json);
        c.found_json.p = NULL;
        c.found_json.len = c.found_json.cap = 0;
        result = 0;
    } else if (notification) {
        result = (rc == 0 && status >= 200 && status < 300) ? 0 : (rc != 0 ? rc : MC_EIO);
    } else if (c.sse_mode) {
        result = rc == 0 ? MC_EPROTO : rc;
    } else if (rc == 0 && status >= 200 && status < 300) {
        if (c.body.len == 0) {
            result = MC_EPROTO;
        } else {
            u64 id = 0;
            bool has = false;
            if (json_msg_id((const char *)c.body.p, c.body.len, &id, &has) == 0 && has &&
                id == want) {
                if (out) *out = (char *)c.body.p;
                else agentc_buf_free(&c.body);
                c.body.p = NULL;
                c.body.len = c.body.cap = 0;
                result = 0;
            } else {
                result = MC_EPROTO;
            }
        }
    } else {
        result = rc != 0 ? rc : MC_EIO;
    }
    if (result != 0) mcp_err_http(s, &c, status, result, agentc_http_error(h));
    agentc_sse_free(&c.sse);
    agentc_buf_free(&c.body);
    agentc_buf_free(&c.found_json);
    agentc_http_free(h);
    if (result != 0 && !s->err[0]) mcp_set_err(s, result, NULL);
    return result;
}

/* --------------------------------------------------------------- exchange */
static void mcp_server_stop(McpServer *s);   /* defined below; tears the server down */
static int mcp_connect_timeout_ms(void);     /* defined with the public API */
static int mcp_server_exchange(McpServer *s, const char *body, u64 id, int timeout_ms,
                               const volatile bool *cancel, bool notification, char **out) {
    if (out) *out = NULL;
    s->err[0] = 0;
    if (s->kind == 0) {
        if (cancel && *cancel) return MC_ECANCELED;
        if (s->wfd < 0) return MC_EPIPE;
        if (s->pid > 0 && os_wait(s->pid, true) != -1) {
            mcp_set_err(s, MC_EPIPE, "server process exited");
            return MC_EPIPE;
        }
        size_t written = 0;
        int rc = agentc_mcp_write_all(s->wfd, s->pid, body, agentc_strlen(body),
                                      timeout_ms, cancel, &written);
        if (rc != 0) {
            mcp_set_err(s, rc, NULL);
            /* A partial write leaves a truncated JSON-RPC line in the pipe;
             * reusing it would desync every later request. Tear the connection
             * down (the next call then fails cleanly, not with garbage). */
            if (written > 0) mcp_server_stop(s);
            return rc;
        }
        if (notification) return 0;
        StdioRd ud = { s, cancel };
        McpReader rd = { mcp_stdio_read, mcp_stdio_wait, &ud };
        rc = agentc_mcp_rpc_await(&rd, &s->rbuf, id, timeout_ms, cancel, mcp_stdio_notify, s, out);
        if (rc != 0) mcp_set_err(s, rc, NULL);
        return rc;
    }
    return mcp_http_post(s, body, agentc_strlen(body), id, notification, timeout_ms, cancel, out);
}

/* =========================================================================
 * Server lifecycle
 * ======================================================================= */
static void mcp_server_stop(McpServer *s) {
    if (!s) return;
    if (s->kind != 0) return;
    if (s->wfd >= 0) {
        os_close(s->wfd);
        s->wfd = -1;
    }
    if (s->pid > 0) {
        bool exited = false;
        /* Let the server finish on stdin EOF first (the wfd close above), then
         * escalate: group SIGTERM, a bounded grace, group SIGKILL. Every wait
         * is the non-blocking form inside a poll slice, so a server that
         * ignores SIGTERM cannot wedge shutdown. */
        for (int i = 0; i < 100 && !exited; i++) {
            if (os_wait(s->pid, true) != -1) exited = true;
            else os_poll(NULL, 0, 5);
        }
        if (!exited) {
            os_kill(-s->pid, 15);
            for (int i = 0; i < 50 && !exited; i++) {
                if (os_wait(s->pid, true) != -1) exited = true;
                else os_poll(NULL, 0, 5);
            }
        }
        if (!exited) {
            os_kill(-s->pid, 9);
            for (int i = 0; i < 50 && !exited; i++) {
                if (os_wait(s->pid, true) != -1) exited = true;
                else os_poll(NULL, 0, 5);
            }
        }
        s->pid = 0;
    }
    if (s->hold_rfd >= 0) {
        os_close(s->hold_rfd);
        s->hold_rfd = -1;
    }
    if (s->rfd >= 0) {
        os_close(s->rfd);
        s->rfd = -1;
    }
}

static void mcp_server_free(McpServer *s) {
    if (!s) return;
    mcp_server_stop(s);
    agentc_free(s->session_id);
    agentc_buf_free(&s->rbuf);
    for (int k = 0; k < MCP_KIND_COUNT; k++) {
        if (k == MCP_KIND_PROMPTS) agentc_mcp_parsed_prompts_free(&s->sync_parsed[k]);
        else if (k == MCP_KIND_RESOURCES) agentc_mcp_parsed_resources_free(&s->sync_parsed[k]);
        else if (k == MCP_KIND_RESOURCE_TEMPLATE)
            agentc_mcp_parsed_resource_templates_free(&s->sync_parsed[k]);
        else agentc_mcp_parsed_free(&s->sync_parsed[k]);
        agentc_free(s->sync[k].cursor);
    }
    mcp_cfg_free_item(&s->cfg);
    agentc_free(s);
}

static char *mcp_initialize_params(void) {
    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "protocolVersion");
    agentc_jsonw_cstr(&w, "2024-11-05");
    agentc_jsonw_key(&w, "capabilities");
    agentc_jsonw_obj(&w);
    agentc_jsonw_end(&w);
    agentc_jsonw_key(&w, "clientInfo");
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "name");
    agentc_jsonw_cstr(&w, "agentc");
    agentc_jsonw_key(&w, "version");
    agentc_jsonw_cstr(&w, AGENTC_VERSION);
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    return (char *)b.p;
}

static char *mcp_list_params(const char *cursor) {
    if (!cursor || !cursor[0]) return agentc_strdup("{}");
    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "cursor");
    agentc_jsonw_cstr(&w, cursor);
    agentc_jsonw_end(&w);
    return (char *)b.p;
}

/* =========================================================================
 * Tool execution
 * ======================================================================= */
static char *mcp_exec_ent(McpToolEnt *t, const char *args_json, bool *is_error,
                          const volatile bool *cancel) {
    if (is_error) *is_error = false;
    if (!t || !t->alive || !t->server || !t->tool) {
        if (is_error) *is_error = true;
        return agentc_strdup("error: mcp: tool no longer available");
    }
    McpServer *s = mcp_server_by_name(t->server);
    if (!s) {
        if (is_error) *is_error = true;
        return agentc_strdup("error: mcp: server not connected");
    }
    if (cancel && *cancel) {
        if (is_error) *is_error = true;
        return agentc_strdup("error: mcp: cancelled");
    }

    AgcBuf pb = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &pb);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "name");
    agentc_jsonw_cstr(&w, t->tool);
    agentc_jsonw_key(&w, "arguments");
    if (args_json && args_json[0]) {
        agentc_jsonw_raw(&w, args_json, agentc_strlen(args_json));
    } else {
        agentc_jsonw_obj(&w);
        agentc_jsonw_end(&w);
    }
    agentc_jsonw_end(&w);

    u64 id = s->next_id++;
    char *body = agentc_mcp_rpc_request(id, "tools/call", (char *)pb.p);
    agentc_buf_free(&pb);
    char *resp = NULL;
    int rc = mcp_server_exchange(s, body, id, (int)s->timeout_ms, cancel, false, &resp);
    agentc_free(body);
    if (rc != 0) {
        if (is_error) *is_error = true;
        AgcBuf b = { 0 };
        agentc_buf_printf(&b, "error: mcp %s/%s: %s", t->server, t->tool,
                      s->err[0] ? s->err : mcp_err_name(rc));
        return (char *)b.p;
    }
    char *text = NULL;
    bool err = false;
    rc = agentc_mcp_parse_call(resp, agentc_strlen(resp), &text, &err);
    agentc_free(resp);
    if (rc != 0) {
        if (is_error) *is_error = true;
        AgcBuf b = { 0 };
        agentc_buf_printf(&b, "error: mcp %s/%s: malformed result", t->server, t->tool);
        agentc_free(text);
        return (char *)b.p;
    }
    if (is_error) *is_error = err;
    return text ? text : agentc_strdup_len("", 0);
}

/* v2 entry point: one function for every exported tool; the registry block
 * travels in AgcTool.ud, so a name collision can never make us cast a
 * foreign tool's identity. */
static int mcp_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                   bool *is_error) {
    if (is_error) *is_error = false;
    McpToolEnt *t = (self && self->run == mcp_run) ? (McpToolEnt *)self->ud : NULL;
    /* An executing entry is only reachable through a handed-out AgcTool, and
     * handed-out entries are never freed or reused; in_flight keeps that
     * invariant explicit for future retire paths. */
    if (t) t->in_flight++;
    char *res = mcp_exec_ent(t, call ? call->args_json : NULL, is_error,
                             call ? call->cancel : NULL);
    if (t) t->in_flight--;
    if (res) {
        agentc_buf_cstr(out, res);
        agentc_free(res);
    }
    return 0;
}

/* =========================================================================
 * Prompt expansion (registered with the core prompt registry)
 * ======================================================================= */
/* Parse `args` against the declared argument list: `key=value` matches by
 * declared name, a bare token fills the next unfilled argument positionally.
 * Unknown names and extra positionals are ignored (logged without any value).
 * Returns 0 or -EINVAL when a required argument is missing. */
static int prompt_build_args(const McpPromptEnt *e, const char *args, AgcBuf *out) {
    AgcJsonW w;
    agentc_jsonw_init(&w, out);
    agentc_jsonw_obj(&w);
    bool used[MCP_MAX_PROMPT_ARGS];
    agentc_memset(used, 0, sizeof used);
    const char *p = args ? args : "";
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        size_t tl = (size_t)(p - tok);
        const char *eq = NULL;
        for (size_t i = 0; i < tl; i++)
            if (tok[i] == '=') {
                eq = tok + i;
                break;
            }
        int idx = -1;
        if (eq) {
            size_t klen = (size_t)(eq - tok);
            for (size_t a = 0; a < e->nargs; a++) {
                if (agentc_strlen(e->args[a].name) == klen &&
                    agentc_memeq(e->args[a].name, tok, klen)) {
                    idx = (int)a;
                    break;
                }
            }
            if (idx < 0) {
                agentc_logf(2, "mcp: prompt %s: ignoring an unknown named argument",
                            e->prompt);
                continue;
            }
        } else {
            for (size_t a = 0; a < e->nargs; a++) {
                if (!used[a]) {
                    idx = (int)a;
                    break;
                }
            }
            if (idx < 0) {
                agentc_logf(2, "mcp: prompt %s: ignoring an extra positional argument",
                            e->prompt);
                continue;
            }
        }
        const char *val = eq ? eq + 1 : tok;
        size_t vlen = eq ? tl - (size_t)(eq - tok) - 1 : tl;
        agentc_jsonw_key(&w, e->args[idx].name);
        agentc_jsonw_str(&w, val, vlen);
        used[idx] = true;
    }
    agentc_jsonw_end(&w);
    for (size_t a = 0; a < e->nargs; a++) {
        if (e->args[a].required && !used[a]) {
            agentc_logf(2, "mcp: prompt %s: missing required argument '%s'", e->prompt,
                        e->args[a].name);
            return MC_EINVAL;
        }
    }
    return 0;
}

char *agentc_mcp_prompt_expand(void *ud, const char *args) {
    McpPromptEnt *e = ud;
    if (!e || !e->alive || !e->server || !e->prompt) return NULL;
    McpServer *s = mcp_server_by_name(e->server);
    if (!s) {
        agentc_logf(2, "mcp: prompt %s/%s: server not connected", e->server, e->prompt);
        return NULL;
    }
    AgcBuf aj = { 0 };
    if (prompt_build_args(e, args, &aj) != 0) {
        agentc_buf_free(&aj);
        return NULL;
    }
    AgcBuf pb = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &pb);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "name");
    agentc_jsonw_cstr(&w, e->prompt);
    agentc_jsonw_key(&w, "arguments");
    agentc_jsonw_raw(&w, (const char *)aj.p, aj.len);
    agentc_jsonw_end(&w);
    agentc_buf_free(&aj);
    u64 id = s->next_id++;
    char *body = agentc_mcp_rpc_request(id, "prompts/get", (char *)pb.p);
    agentc_buf_free(&pb);
    char *resp = NULL;
    int rc = mcp_server_exchange(s, body, id, MCP_PROMPT_TIMEOUT_MS, NULL, false, &resp);
    agentc_free(body);
    if (rc != 0) {
        agentc_logf(2, "mcp: prompt %s/%s: %s", e->server, e->prompt,
                    s->err[0] ? s->err : mcp_err_name(rc));
        return NULL;
    }
    char *text = NULL;
    rc = agentc_mcp_parse_prompt_get(resp, agentc_strlen(resp), &text);
    agentc_free(resp);
    if (rc == MC_E2BIG) {
        agentc_logf(2, "mcp: prompt %s/%s: result exceeds %u KiB", e->server, e->prompt,
                    (unsigned)(AGENTC_LIMIT_MCP_PROMPT_TEXT_BYTES / 1024));
        return NULL;
    }
    if (rc != 0) {
        agentc_logf(2, "mcp: prompt %s/%s: %s", e->server, e->prompt, mcp_err_name(rc));
        return NULL;
    }
    return text;
}

/* =========================================================================
 * connect/re-sync state machine
 *
 * agentc_mcp_start() only creates the server records. Each agentc_mcp_pump()
 * advances one server by one bounded step, rotating round-robin, so a stalled
 * transport cannot multiply its wait by the number of configured servers: a
 * single stdio write, a single non-blocking stdio read attempt, or one HTTP
 * request/response slice of MCP_STEP_SLICE_MS. Every kind's list round (and
 * the connect as a whole) is bounded by s->deadline, refreshed per kind. No
 * step blocks the main loop for longer than the slice.
 *
 * HTTP limitation: agentc_http_run() is not resumable, so an HTTP request is
 * rebuilt and retried from the start on the next pump when a slice times out
 * (the request is idempotent from our side and the server sees a fresh id).
 * This is the least-tested path but stays correct and bounded.
 * ======================================================================= */
#define MCP_STEP_SLICE_MS 250
/* idle drain: a READY stdio server can push notifications with no
 * request outstanding. One pump honors at most this many complete messages;
 * leftover bytes/messages stay buffered for the next pump. */
#define MCP_IDLE_DRAIN_MAX_MSGS 64

/* Free the parse accumulation, pagination cursor and page count of one kind. */
static void mcp_sync_clear_kind(McpServer *s, int kind) {
    if (!s || kind < 0 || kind >= MCP_KIND_COUNT) return;
    if (kind == MCP_KIND_PROMPTS) agentc_mcp_parsed_prompts_free(&s->sync_parsed[kind]);
    else if (kind == MCP_KIND_RESOURCES) agentc_mcp_parsed_resources_free(&s->sync_parsed[kind]);
    else if (kind == MCP_KIND_RESOURCE_TEMPLATE)
        agentc_mcp_parsed_resource_templates_free(&s->sync_parsed[kind]);
    else agentc_mcp_parsed_free(&s->sync_parsed[kind]);
    agentc_free(s->sync[kind].cursor);
    s->sync[kind].cursor = NULL;
    s->sync[kind].pages = 0;
}

/* Hand every newly discovered tool to the extension registry. Only runs while
 * the mcp extension itself is loaded; the agentc_mcp_load() test helper leaves
 * the registry untouched (and therefore its deep copies out of mem_live). */
static void mcp_publish_tools(void) {
    if (!g_mcp_ext_loaded) return;
    for (size_t i = 0; i < g_tools.len; i++) {
        McpToolEnt *e = ((McpToolEnt **)g_tools.p)[i];
        if (!e->alive || e->registered || e->publish_failed) continue;
        AgcTool t;
        agentc_memset(&t, 0, sizeof t);
        t.name = e->name;
        t.label = e->label;
        t.desc = e->desc ? e->desc : "";
        t.params_json = e->schema ? e->schema : "{\"type\":\"object\"}";
        t.flags = e->flags;
        t.ud = e;
        t.run = mcp_run;
        int rc = agentc_ext_add_tool_internal(&t);
        if (rc == 0) {
            e->registered = true;
            e->handed_out = true;   /* the registry keeps `ud` for good */
        } else {
            /* One attempt per entry: retrying would just repeat the registry's
             * duplicate-name rejection on every re-sync. A publish that failed
             * and was never snapshot-handed-out owns no outside reference, so
             * the entry stays free for reuse. */
            e->publish_failed = true;
            agentc_logf(2, "mcp: %s: cannot publish tool %s (%s); not retrying",
                        e->server ? e->server : "?", e->name, mcp_err_name(rc));
        }
    }
}

/* ---------------------------------------------------- generic resource tools
 *
 * The three process-lifetime read-only tools published at the first
 * successful resource-capable server's list sync. The two list tools read the
 * local committed table (already paginated through the sync); the read tool
 * performs one bounded synchronous resources/read through the existing
 * per-server exchange. `server` must be live with the resource capability;
 * `uri` is passed through (the server owns authorization). Resource contents
 * are never logged; names/URIs only ever appear in tool output, at level <= 2
 * in diagnostics. */
enum {
    MCP_RTOOL_LIST_RESOURCES = 0,
    MCP_RTOOL_LIST_TEMPLATES,
    MCP_RTOOL_READ,
};

typedef struct {
    int which;
} McpResourceTool;

static McpResourceTool g_rtool_defs[3] = {
    { MCP_RTOOL_LIST_RESOURCES },
    { MCP_RTOOL_LIST_TEMPLATES },
    { MCP_RTOOL_READ },
};

static McpServer *mcp_live_resource_server(const char *name) {
    McpServer *s = name ? mcp_server_by_name(name) : NULL;
    if (!s || s->state != MCP_ST_READY) return NULL;
    if (!(s->caps & MCP_CAP_RESOURCES)) return NULL;
    return s;
}

/* Liveness predicate usable before the server types are declared. */
static bool mcp_resource_server_live(const char *name) {
    return mcp_live_resource_server(name) != NULL;
}

/* Two optional string fields from the tool arguments. Both outputs are
 * owned; -EPROTO when the document is not a JSON object. */
static int rtool_args2(const char *args_json, const char *k1, char **v1,
                       const char *k2, char **v2) {
    *v1 = NULL;
    if (v2) *v2 = NULL;
    if (!args_json || !args_json[0]) return 0;
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *root = a ? agentc_json_parse_in(a, args_json, agentc_strlen(args_json)) : NULL;
    if (!root || agentc_json_type(root) != AGENTC_JSON_OBJ) {
        agentc_json_arena_free(a);
        return MC_EPROTO;
    }
    const char *s1 = agentc_json_get_str(root, k1);
    if (s1 && s1[0]) *v1 = agentc_strdup(s1);
    if (v2) {
        const char *s2 = agentc_json_get_str(root, k2);
        if (s2 && s2[0]) *v2 = agentc_strdup(s2);
    }
    agentc_json_arena_free(a);
    return 0;
}

static void rtool_fail(AgcBuf *out, bool *is_error, const char *msg) {
    if (is_error) *is_error = true;
    agentc_buf_cstr(out, "error: mcp: ");
    agentc_buf_cstr(out, msg);
}

static int mcp_resource_tool_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                                 bool *is_error) {
    if (is_error) *is_error = false;
    McpResourceTool *rt =
        (self && self->run == mcp_resource_tool_run) ? (McpResourceTool *)self->ud : NULL;
    if (!rt || !out) return MC_EINVAL;
    const bool read_tool = rt->which == MCP_RTOOL_READ;
    char *server = NULL, *second = NULL;
    int rc = rtool_args2(call ? call->args_json : NULL, "server", &server,
                         read_tool ? "uri" : "cursor", &second);
    if (rc != 0) {
        rtool_fail(out, is_error, "malformed tool arguments");
        return 0;
    }
    if (read_tool) {
        if (!server || !second) {
            rtool_fail(out, is_error, "mcp_read_resource needs server and uri");
        } else {
            McpServer *s = mcp_live_resource_server(server);
            if (!s) {
                AgcBuf b = { 0 };
                agentc_buf_printf(&b, "server \"%s\" is not connected with the "
                                     "resource capability", server);
                rtool_fail(out, is_error, (const char *)b.p);
                agentc_buf_free(&b);
            } else {
                AgcBuf pb = { 0 };
                AgcJsonW w;
                agentc_jsonw_init(&w, &pb);
                agentc_jsonw_obj(&w);
                agentc_jsonw_key(&w, "uri");
                agentc_jsonw_cstr(&w, second);
                agentc_jsonw_end(&w);
                u64 id = s->next_id++;
                char *body = agentc_mcp_rpc_request(id, "resources/read", (char *)pb.p);
                agentc_buf_free(&pb);
                char *resp = NULL;
                rc = mcp_server_exchange(s, body, id, (int)s->timeout_ms,
                                         call ? call->cancel : NULL, false, &resp);
                agentc_free(body);
                if (rc != 0) {
                    AgcBuf b = { 0 };
                    agentc_buf_printf(&b, "%s: %s", server,
                                      s->err[0] ? s->err : mcp_err_name(rc));
                    rtool_fail(out, is_error, (const char *)b.p);
                    agentc_buf_free(&b);
                } else {
                    char *text = NULL;
                    rc = agentc_mcp_parse_resource_read(resp, agentc_strlen(resp), &text);
                    agentc_free(resp);
                    if (rc == MCP_ERR_BINARY) {
                        rtool_fail(out, is_error, "binary resource not supported");
                    } else if (rc == MC_E2BIG) {
                        AgcBuf b = { 0 };
                        agentc_buf_printf(&b, "%s: result exceeds %u KiB", server,
                                          (unsigned)(AGENTC_LIMIT_MCP_RESOURCE_TEXT_BYTES / 1024));
                        rtool_fail(out, is_error, (const char *)b.p);
                        agentc_buf_free(&b);
                    } else if (rc != 0) {
                        AgcBuf b = { 0 };
                        agentc_buf_printf(&b, "%s: %s", server, mcp_err_name(rc));
                        rtool_fail(out, is_error, (const char *)b.p);
                        agentc_buf_free(&b);
                    } else {
                        agentc_buf_cstr(out, text ? text : "");
                        agentc_free(text);
                    }
                }
            }
        }
    } else {
        const bool templates = rt->which == MCP_RTOOL_LIST_TEMPLATES;
        bool any_live = false;
        if (server) {
            any_live = mcp_resource_server_live(server);
        } else {
            for (size_t i = 0; i < g_servers.len && !any_live; i++) {
                McpServer *s = ((McpServer **)g_servers.p)[i];
                any_live = s && s->state == MCP_ST_READY && (s->caps & MCP_CAP_RESOURCES);
            }
        }
        char *json = NULL;
        if (!any_live) {
            AgcBuf b = { 0 };
            if (server)
                agentc_buf_printf(&b, "server \"%s\" is not connected with the "
                                     "resource capability", server);
            else
                agentc_buf_cstr(&b, "no connected MCP server with the resource capability");
            rtool_fail(out, is_error, (const char *)b.p);
            agentc_buf_free(&b);
        } else {
            rc = mcp_resource_list_json(templates, server, second, true, &json);
            if (rc == MC_E2BIG) {
                rtool_fail(out, is_error, "resource list exceeds 64 KiB; use cursor");
            } else if (rc == MC_EINVAL) {
                rtool_fail(out, is_error, "invalid cursor");
            } else if (rc != 0) {
                rtool_fail(out, is_error, mcp_err_name(rc));
            } else {
                agentc_buf_cstr(out, json);
                agentc_free(json);
            }
        }
    }
    agentc_free(server);
    agentc_free(second);
    return 0;
}

/* Publish the three generic resource tools once, at the first successful
 * resource-capable list sync. `agentc_ext_add_tool_internal` marks the
 * registry dirty, so the app's next-turn recompose picks them up. They are
 * never removed or replaced: the server records they read are process-
 * lifetime. */
static bool g_resource_tools_attempted;

static void mcp_publish_resource_tools(void) {
    if (!g_mcp_ext_loaded || g_resource_tools_attempted) return;
    g_resource_tools_attempted = true;
    static const struct {
        const char *name;
        const char *label;
        const char *desc;
        const char *schema;
    } defs[3] = {
        { "mcp_list_resources", "mcp list resources",
          "List the resources advertised by connected MCP servers (JSON text).",
          "{\"type\":\"object\",\"properties\":{\"server\":{\"type\":\"string\"},"
          "\"cursor\":{\"type\":\"string\"}}}" },
        { "mcp_list_resource_templates", "mcp list resource templates",
          "List the resource templates advertised by connected MCP servers (JSON text).",
          "{\"type\":\"object\",\"properties\":{\"server\":{\"type\":\"string\"},"
          "\"cursor\":{\"type\":\"string\"}}}" },
        { "mcp_read_resource", "mcp read resource",
          "Read one MCP resource by uri (text contents only).",
          "{\"type\":\"object\",\"properties\":{\"server\":{\"type\":\"string\"},"
          "\"uri\":{\"type\":\"string\"}},\"required\":[\"server\",\"uri\"]}" },
    };
    for (int i = 0; i < 3; i++) {
        AgcTool t;
        agentc_memset(&t, 0, sizeof t);
        t.name = defs[i].name;
        t.label = defs[i].label;
        t.desc = defs[i].desc;
        t.params_json = defs[i].schema;
        t.flags = AGENTC_TOOL_READONLY;
        t.ud = &g_rtool_defs[i];
        t.run = mcp_resource_tool_run;
        int rc = agentc_ext_add_tool_internal(&t);
        if (rc != 0)
            agentc_logf(2, "mcp: cannot publish %s (%s); not retrying", defs[i].name,
                        mcp_err_name(rc));
    }
}

static const char *mcp_transport_name(int kind) { return kind == 1 ? "http" : "stdio"; }

/* status for the mcp_servers_change payload. Every state except READY
 * and FAILED is mid-connect/resync. */
static const char *mcp_status_name(int state) {
    if (state == MCP_ST_READY) return "ready";
    if (state == MCP_ST_FAILED) return "failed";
    return "connecting";
}

/* mcp_servers_change payload: every non-NULL server record with its transport,
 * state and -- only once initialize has completed -- the advertised capability
 * names. Emitted when records are created (the pump hook), when a server
 * becomes READY (mcp_sync_advance) and when one fails (mcp_server_fail), so a
 * front end can render connecting/ready/failed without polling the extension. */
static void mcp_emit_servers_change(void) {
    if (!g_mcp_ext_loaded || !agentc_ext_wants("mcp_servers_change")) return;
    AgcBuf b = { 0 };
    AgcJsonW w;
    agentc_jsonw_init(&w, &b);
    agentc_jsonw_obj(&w);
    agentc_jsonw_key(&w, "servers");
    agentc_jsonw_arr(&w);
    for (size_t i = 0; i < g_servers.len; i++) {
        McpServer *s = ((McpServer **)g_servers.p)[i];
        if (!s) continue;
        agentc_jsonw_obj(&w);
        agentc_jsonw_key(&w, "name");
        agentc_jsonw_cstr(&w, s->name ? s->name : "");
        agentc_jsonw_key(&w, "kind");
        agentc_jsonw_cstr(&w, mcp_transport_name(s->kind));
        agentc_jsonw_key(&w, "status");
        agentc_jsonw_cstr(&w, mcp_status_name(s->state));
        if (s->inited) {
            /* One `resources` capability covers both resource lists. */
            agentc_jsonw_key(&w, "caps");
            agentc_jsonw_arr(&w);
            if (s->caps & MCP_CAP_TOOLS) agentc_jsonw_cstr(&w, "tools");
            if (s->caps & MCP_CAP_PROMPTS) agentc_jsonw_cstr(&w, "prompts");
            if (s->caps & MCP_CAP_RESOURCES) {
                agentc_jsonw_cstr(&w, "resources");
                agentc_jsonw_cstr(&w, "resource_templates");
            }
            agentc_jsonw_end(&w);
        }
        agentc_jsonw_end(&w);
    }
    agentc_jsonw_end(&w);
    agentc_jsonw_end(&w);
    AgcExtResult r = agentc_ext_emit("mcp_servers_change", (const char *)b.p);
    agentc_free(r.result_json);
    agentc_buf_free(&b);
}

/* Commit a completed paginated tools/list: refresh seen flags, add the parsed
 * tools, retire any entry this server no longer advertises. A cap violation is
 * returned to the caller, which fails the server (logged, never READY) so a
 * hostile tools/list cannot pin memory. */
static void mcp_server_fail(McpServer *s, int rc);
static int mcp_sync_commit_tools(McpServer *s) {
    for (size_t i = 0; i < g_tools.len; i++) {
        McpToolEnt *e = ((McpToolEnt **)g_tools.p)[i];
        if (e->alive && e->server && agentc_streq(e->server, s->name)) e->seen = false;
    }
    for (size_t i = 0; i < s->sync_parsed[MCP_KIND_TOOLS].len; i++) {
        McpParsedTool *p = &((McpParsedTool *)s->sync_parsed[MCP_KIND_TOOLS].p)[i];
        int ar = agentc_mcp_registry_add(s->name, p->name, p->desc, p->schema, p->flags);
        if (ar == MC_ENOSPC || ar == MC_EFROZEN) {
            if (ar == MC_EFROZEN)
                agentc_logf(3, "mcp: server \"%s\": frozen retired-tool cap reached "
                            "(%d); failing server: %s",
                            s->name ? s->name : "?", (int)MCP_FROZEN_CAP, p->name);
            else
                agentc_logf(3, "mcp: server \"%s\": tools/list exceeds the tool caps "
                            "(%d per server, %d total): %s",
                            s->name ? s->name : "?", (int)MCP_MAX_TOOLS_PER_SERVER,
                            (int)MCP_MAX_TOOLS_TOTAL, p->name);
            return ar;
        }
        if (ar != 0)
            agentc_logf(2, "mcp: %s: cannot expose tool %s: %s", s->name, p->name,
                        mcp_err_name(ar));
    }
    for (size_t i = 0; i < g_tools.len; i++) {
        McpToolEnt *e = ((McpToolEnt **)g_tools.p)[i];
        if (!(e->alive && e->server && agentc_streq(e->server, s->name) && !e->seen))
            continue;
        /* Retiring a handed-out entry is the only way the frozen pile grows;
         * past the cap fail the server instead of recycling the block. */
        if (e->handed_out && !registry_frozen_room()) {
            agentc_logf(3, "mcp: server \"%s\": frozen retired-tool cap reached "
                        "(%d); failing server: %s",
                        s->name ? s->name : "?", (int)MCP_FROZEN_CAP,
                        e->name ? e->name : "?");
            return MC_EFROZEN;
        }
        registry_retire_entry(e);
    }
    return 0;
}

static int mcp_sync_commit_prompts(McpServer *s) {
    /* The commit owns mark-seen/add/retire-unseen and returns -ENOSPC/-EFROZEN
     * when a budget is exhausted; the caller abandons the refresh. */
    return agentc_mcp_prompt_commit(s->name, &s->sync_parsed[MCP_KIND_PROMPTS]);
}

/* Replace one server's committed resource table. A cap refusal abandons the
 * refresh (keep the previous table) and is logged by the commit. */
static int mcp_sync_commit_resource_kind(McpServer *s, int kind) {
    return agentc_mcp_resource_commit(s->name, &s->sync_parsed[kind],
                                      kind == MCP_KIND_RESOURCE_TEMPLATE);
}

/* First kind with a sync due (tools -> prompts -> resources -> templates),
 * or -1. */
static int mcp_sync_next_kind(McpServer *s) {
    for (int k = 0; k < MCP_KIND_COUNT; k++) {
        if (!(s->caps & mcp_kind_cap(k))) continue;
        if (s->sync[k].pending) return k;
    }
    return -1;
}

static void mcp_sync_begin(McpServer *s, int kind) {
    mcp_sync_clear_kind(s, kind);
    s->sync_kind = kind;
    s->deadline = os_now_ns(OS_CLOCK_MONOTONIC) + s->timeout_ms * 1000000;
    s->state = MCP_ST_SYNC_SEND;
}

/* Start the next pending kind; when none is left the round is done. */
static void mcp_sync_advance(McpServer *s) {
    int k = mcp_sync_next_kind(s);
    if (k < 0) {
        s->sync_kind = -1;
        s->state = MCP_ST_READY;
        s->ready_once = true;
        s->retry_count = 0;   /* a successful connect resets the backoff (C) */
        mcp_emit_servers_change();
        return;
    }
    mcp_sync_begin(s, k);
}

static int mcp_stdio_send(McpServer *s, const char *body, int timeout_ms) {
    if (s->wfd < 0) return MC_EPIPE;
    if (s->pid > 0 && os_wait(s->pid, true) != -1) {
        mcp_set_err(s, MC_EPIPE, "server process exited");
        return MC_EPIPE;
    }
    size_t written = 0;
    int rc = agentc_mcp_write_all(s->wfd, s->pid, body, agentc_strlen(body),
                                  timeout_ms, NULL, &written);
    if (rc != 0) {
        mcp_set_err(s, rc, NULL);
        /* A partial write desyncs the framing: tear the connection down. */
        if (written > 0) mcp_server_stop(s);
    }
    return rc;
}

/* Non-blocking stdio read: drain whatever is buffered, feed notifications and
 * return the message with `want` as the id. 1 = matched, 0 = need more,
 * <0 = error. Never waits. */
static int mcp_stdio_poll(McpServer *s, u64 want, char **out) {
    if (out) *out = NULL;
    if (s->rfd < 0) return MC_EPIPE;
    bool eof = false;
    for (;;) {
        u8 tmp[8192];
        int n = os_read(s->rfd, tmp, sizeof tmp);
        if (n > 0) {
            agentc_buf_push(&s->rbuf, tmp, (size_t)n);
            if (!mcp_line_under_cap(&s->rbuf)) return MC_E2BIG;
            if (mcp_has_newline(tmp, (size_t)n)) break;
            continue;
        }
        if (n == MC_EAGAIN) break;
        if (n == MC_EINTR) continue;
        if (n == 0) { eof = true; break; }
        return n;
    }
    for (;;) {
        char *msg = NULL;
        int g = agentc_mcp_stdio_next(&s->rbuf, &msg);
        if (g < 0) return g;
        if (g == 0) break;
        u64 mid = 0;
        bool has = false;
        int pr = json_msg_id(msg, agentc_strlen(msg), &mid, &has);
        if (pr != 0) { agentc_free(msg); continue; }
        if (!has) {
            mcp_stdio_notify(s, msg, agentc_strlen(msg));
            agentc_free(msg);
            continue;
        }
        if (mid == want) {
            if (out) *out = msg;
            else agentc_free(msg);
            return 1;
        }
        agentc_free(msg);
    }
    return eof ? MC_ECONNRESET : 0;
}

/* idle drain: a READY stdio server may push notifications (list_changed,
 * progress, logs) while no request is outstanding. Read whatever is buffered
 * and honor at most MCP_IDLE_DRAIN_MAX_MSGS complete messages, so one idle
 * notification is picked up on the next pump instead of waiting for the next
 * exchange. Leftover bytes/messages stay in rbuf for the following pump. EOF
 * is a dead server (the caller fails it, which arms the reconnect backoff);
 * HTTP has no idle channel because every HTTP message is scoped to a request,
 * so this runs only for stdio. 0 | -errno. */
static int mcp_stdio_idle_drain(McpServer *s) {
    if (s->kind != 0 || s->rfd < 0) return MC_EPIPE;
    bool eof = false;
    for (;;) {
        u8 tmp[8192];
        int n = os_read(s->rfd, tmp, sizeof tmp);
        if (n > 0) {
            agentc_buf_push(&s->rbuf, tmp, (size_t)n);
            if (!mcp_line_under_cap(&s->rbuf)) return MC_E2BIG;
            if (mcp_has_newline(tmp, (size_t)n)) break;
            continue;
        }
        if (n == MC_EAGAIN) break;
        if (n == MC_EINTR) continue;
        if (n == 0) { eof = true; break; }
        return n;
    }
    int parsed = 0;
    while (parsed < MCP_IDLE_DRAIN_MAX_MSGS) {
        char *msg = NULL;
        int g = agentc_mcp_stdio_next(&s->rbuf, &msg);
        if (g < 0) return g;
        if (g == 0) break;
        parsed++;
        u64 mid = 0;
        bool has = false;
        /* A response with an id here belongs to no pump-waited request (the
         * synchronous exchange paths drain their own replies): drop it. */
        if (json_msg_id(msg, agentc_strlen(msg), &mid, &has) == 0 && !has)
            mcp_stdio_notify(s, msg, agentc_strlen(msg));
        agentc_free(msg);
    }
    return eof ? MC_ECONNRESET : 0;
}

/* Parse protocolVersion + result.capabilities from an initialize response.
 * The capability bits gate every later kind sync and list_changed accept. */
static void mcp_handle_initialize(McpServer *s, const char *resp) {
    AgcJsonArena *a = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(a, resp, agentc_strlen(resp));
    if (root && agentc_json_get(root, "error")) {
        /* A JSON-RPC error is a failed handshake, never a successful connect. */
        agentc_json_arena_free(a);
        mcp_server_fail(s, MC_EIO);
        return;
    }
    s->caps = agentc_mcp_caps_parse(resp, agentc_strlen(resp));
    s->inited = true;   /* caps are now real; mcp_servers_change may publish them */
    AgcJson *result = root ? agentc_json_get(root, "result") : NULL;
    const char *pv = result ? agentc_json_get_str(result, "protocolVersion") : NULL;
    if (pv && !agentc_streq(pv, "2024-11-05"))
        agentc_logf(0, "mcp: %s negotiated protocol %s", s->name, pv);
    agentc_json_arena_free(a);
}

static void mcp_server_fail(McpServer *s, int rc);

/* AGENTC_MCP_RETRY_MS: reconnect base delay in milliseconds. 0 disables
 * reconnect (a FAILED server stays terminal); a positive value is clamped to
 * [1, AGENTC_MCP_RETRY_MAX_MS]; unset or malformed keeps the default base. */
static int mcp_retry_base_ms(void) {
    const char *v = agentc_env_get("AGENTC_MCP_RETRY_MS");
    if (v && v[0]) {
        bool ok = false;
        i64 n = agentc_parse_i64(v, agentc_strlen(v), &ok);
        if (ok) {
            if (n <= 0) return 0;
            if (n > AGENTC_MCP_RETRY_MAX_MS) n = AGENTC_MCP_RETRY_MAX_MS;
            return (int)n;
        }
    }
    return AGENTC_MCP_RETRY_BASE_MS;
}

int agentc_mcp_retry_delay_ms(int retry_count) {
    int base = mcp_retry_base_ms();
    if (base <= 0) return 0;
    int shift = retry_count < 0 ? 0 : (retry_count > 6 ? 6 : retry_count);
    i64 delay = (i64)base << shift;
    if (delay > AGENTC_MCP_RETRY_MAX_MS) delay = AGENTC_MCP_RETRY_MAX_MS;
    return (int)delay;
}

static void mcp_server_fail(McpServer *s, int rc) {
    mcp_set_err(s, rc, NULL);
    s->state = MCP_ST_FAILED;
    agentc_logf(2, "mcp: server \"%s\" failed: %s", s->name ? s->name : "?", s->err);
    int delay = agentc_mcp_retry_delay_ms(s->retry_count);
    if (delay > 0) {
        s->retry_at = os_now_ns(OS_CLOCK_MONOTONIC) + (i64)delay * 1000000;
        s->retry_count++;
    } else {
        s->retry_at = 0;   /* AGENTC_MCP_RETRY_MS=0: FAILED is terminal */
    }
    mcp_emit_servers_change();
}

/* Tear a FAILED server down to a clean NEW state for the next attempt: stop
 * the transport, drop the session/buffers and every negotiated or in-flight
 * sync bit, then restart the connect budget. retry_count is kept so the
 * backoff keeps growing across consecutive failures. While the server sits in
 * FAILED it is not pending (agentc_mcp_pending), so the startup budget is
 * unaffected; only the rearm makes it pending again. */
static void mcp_server_rearm(McpServer *s) {
    mcp_server_stop(s);
    agentc_free(s->session_id);
    s->session_id = NULL;
    agentc_buf_clear(&s->rbuf);
    for (int k = 0; k < MCP_KIND_COUNT; k++) {
        mcp_sync_clear_kind(s, k);
        s->sync[k].pending = false;
        s->sync[k].again = false;
    }
    s->sync_kind = -1;
    s->caps = MCP_CAP_TOOLS;   /* pre-initialize default; inited gates publishing */
    s->inited = false;
    s->ready_once = false;
    s->pending_id = 0;
    s->next_id = 1;
    s->err[0] = 0;
    s->state = MCP_ST_NEW;
    s->deadline = os_now_ns(OS_CLOCK_MONOTONIC) + (i64)mcp_connect_timeout_ms() * 1000000;
}

/* A re-sync failure on an already-connected server keeps the previous tables
 * (tools/prompts/resources) usable. A timeout re-arms the failed kind so the
 * next pump retries it; anything latched during the failed round stays
 * pending for the next pump. */
static void mcp_sync_fail(McpServer *s, int rc) {
    int kind = s->sync_kind;
    if (kind < 0 || kind >= MCP_KIND_COUNT) kind = MCP_KIND_TOOLS;
    if (s->ready_once) {
        agentc_logf(2, "mcp: %s: %s list re-sync failed: %s", s->name ? s->name : "?",
                    mcp_kind_name(kind), mcp_err_name(rc));
        bool again = s->sync[kind].again;
        mcp_sync_clear_kind(s, kind);
        s->sync[kind].again = false;
        /* An overlapping synchronous tool/prompt exchange steals the pump's
         * pending response (it arrives with an unknown id and is dropped), so
         * the pump surfaces it as a timeout. A ready server must re-arm the
         * kind instead of abandoning the refresh: the next pump re-enters
         * SEND/WAIT and the list is fetched again. A prompt cap refusal
         * likewise stays pending so a later list that fits can commit; a
         * resource cap refusal mirrors agentc_mcp_resource_commit and keeps the
         * previous table without a retry. Other failures keep the previous
         * table, with only an explicit list_changed latch retried. */
        bool prompt_cap = kind == MCP_KIND_PROMPTS &&
                          (rc == MC_ENOSPC || rc == MC_EFROZEN);
        s->sync[kind].pending = again || rc == MC_ETIMEDOUT || prompt_cap;
        s->sync_kind = -1;
        s->state = MCP_ST_READY;
        s->retry_count = 0;   /* a usable connection must not stay on the
                               * compounded connect backoff (C) */
    } else {
        mcp_server_fail(s, rc);
    }
}

static bool mcp_server_expired(McpServer *s) {
    return s->deadline > 0 && os_now_ns(OS_CLOCK_MONOTONIC) >= s->deadline;
}

/* Append one page of the in-flight kind's list; on the final page commit the
 * kind and start the next pending one. Returns 0 | -errno. */
static int mcp_sync_handle_response(McpServer *s, char *resp) {
    int kind = s->sync_kind;
    if (kind < 0 || kind >= MCP_KIND_COUNT) return MC_EPROTO;
    char *next = NULL;
    int rc;
    switch (kind) {
    case MCP_KIND_PROMPTS:
        rc = agentc_mcp_parse_prompts(resp, agentc_strlen(resp), &s->sync_parsed[kind], &next);
        break;
    case MCP_KIND_RESOURCES:
        rc = agentc_mcp_parse_resources(resp, agentc_strlen(resp), &s->sync_parsed[kind], &next);
        break;
    case MCP_KIND_RESOURCE_TEMPLATE:
        rc = agentc_mcp_parse_resource_templates(resp, agentc_strlen(resp),
                                                 &s->sync_parsed[kind], &next);
        break;
    default:
        rc = agentc_mcp_parse_tools(resp, agentc_strlen(resp), &s->sync_parsed[kind], &next);
        break;
    }
    if (rc != 0) { agentc_free(next); return rc; }
    agentc_free(s->sync[kind].cursor);
    s->sync[kind].cursor = next;
    if (next) {
        if (++s->sync[kind].pages >= AGENTC_LIMIT_MCP_PAGES) return MC_E2BIG;
        s->state = MCP_ST_SYNC_SEND;
        return 0;
    }
    switch (kind) {
    case MCP_KIND_PROMPTS:
        rc = mcp_sync_commit_prompts(s);
        break;
    case MCP_KIND_RESOURCES:
    case MCP_KIND_RESOURCE_TEMPLATE:
        rc = mcp_sync_commit_resource_kind(s, kind);
        break;
    default:
        rc = mcp_sync_commit_tools(s);
        break;
    }
    if (rc != 0) {
        if (kind == MCP_KIND_TOOLS && (rc == MC_ENOSPC || rc == MC_EFROZEN)) {
            /* A hostile tools/list fails the server, never READY. */
            mcp_sync_clear_kind(s, kind);
            mcp_server_fail(s, rc);
        } else {
            /* Cap or protocol failure on a prompt/resource (or non-cap tool)
             * sync: abandon the refresh, keeping the previous table. */
            mcp_sync_fail(s, rc);
        }
        return 0;
    }
    /* A notification that arrived while this kind was in flight survives the
     * commit as a follow-up fetch. */
    s->sync[kind].pending = s->sync[kind].again;
    s->sync[kind].again = false;
    mcp_sync_clear_kind(s, kind);
    s->ready_once = true;   /* the first committed page makes the server usable */
    if (kind == MCP_KIND_TOOLS) mcp_publish_tools();
    else if (kind == MCP_KIND_RESOURCES || kind == MCP_KIND_RESOURCE_TEMPLATE)
        mcp_publish_resource_tools();
    mcp_sync_advance(s);
    return 0;
}

static void mcp_server_step(McpServer *s) {
    if (!s) return;

    /* reconnect: a FAILED server stays terminal (and non-pending) until
     * its backoff deadline passes; the next pump then tears it down and
     * re-enters NEW with a fresh connect budget. */
    if (s->state == MCP_ST_FAILED) {
        if (s->retry_at <= 0 || os_now_ns(OS_CLOCK_MONOTONIC) < s->retry_at) return;
        mcp_server_rearm(s);
    }

    if (s->state == MCP_ST_READY) {
        /* Idle stdio drain: a list_changed (or log/progress) notification that
         * arrived while no request was in flight is honored before deciding
         * whether a re-sync is due. HTTP responses are request-scoped, so an
         * HTTP server has no idle channel to drain. */
        if (s->kind == 0) {
            int rc = mcp_stdio_idle_drain(s);
            if (rc != 0) { mcp_server_fail(s, rc); return; }
        }
        int k = mcp_sync_next_kind(s);
        if (k < 0) return;
        /* Re-sync: fresh deadline from the per-request timeout, then send. */
        mcp_sync_begin(s, k);
    }

    if (mcp_server_expired(s)) {
        if (s->state == MCP_ST_SYNC_SEND || s->state == MCP_ST_SYNC_WAIT)
            mcp_sync_fail(s, MC_ETIMEDOUT);
        else
            mcp_server_fail(s, MC_ETIMEDOUT);
        return;
    }

    switch (s->state) {
    case MCP_ST_NEW:
        if (s->kind == 0) {
            int rc = mcp_spawn_stdio(s, &s->cfg);
            if (rc != 0) { mcp_server_fail(s, rc); return; }
        }
        s->state = MCP_ST_INIT_SEND;
        return;

    case MCP_ST_INIT_SEND: {
        char *params = mcp_initialize_params();
        u64 id = s->next_id++;
        char *body = agentc_mcp_rpc_request(id, "initialize", params);
        agentc_free(params);
        if (s->kind == 0) {
            int rc = mcp_stdio_send(s, body, MCP_STEP_SLICE_MS);
            agentc_free(body);
            if (rc != 0) { mcp_server_fail(s, rc); return; }
            s->pending_id = id;
            s->state = MCP_ST_INIT_WAIT;
        } else {
            char *resp = NULL;
            int rc = mcp_http_post(s, body, agentc_strlen(body), id, false,
                                   MCP_STEP_SLICE_MS, NULL, &resp);
            agentc_free(body);
            if (rc == MC_ETIMEDOUT) return;   /* retry the request next pump */
            if (rc != 0) { mcp_server_fail(s, rc); return; }
            mcp_handle_initialize(s, resp);
            agentc_free(resp);
            if (s->state == MCP_ST_FAILED) return;
            s->state = MCP_ST_NOTIFY;
        }
        return;
    }

    case MCP_ST_INIT_WAIT: {
        char *resp = NULL;
        int rc = mcp_stdio_poll(s, s->pending_id, &resp);
        if (rc < 0) { mcp_server_fail(s, rc); return; }
        if (rc == 0) return;
        mcp_handle_initialize(s, resp);
        agentc_free(resp);
        if (s->state == MCP_ST_FAILED) return;
        s->state = MCP_ST_NOTIFY;
        return;
    }

    case MCP_ST_NOTIFY: {
        char *body = agentc_mcp_rpc_notify("notifications/initialized", NULL);
        int rc;
        if (s->kind == 0) {
            rc = mcp_stdio_send(s, body, MCP_STEP_SLICE_MS);
        } else {
            rc = mcp_http_post(s, body, agentc_strlen(body), 0, true,
                               MCP_STEP_SLICE_MS, NULL, NULL);
        }
        agentc_free(body);
        if (rc == MC_ETIMEDOUT) return;       /* retry the request next pump */
        if (rc != 0) { mcp_server_fail(s, rc); return; }
        /* The connect round fetches every advertised, synced kind in order
         * (tools -> prompts -> resources -> resource templates); an empty/
         * capability-less server becomes READY without a list. */
        for (int k = 0; k < MCP_KIND_COUNT; k++)
            if (s->caps & mcp_kind_cap(k)) s->sync[k].pending = true;
        mcp_sync_advance(s);
        return;
    }

    case MCP_ST_SYNC_SEND: {
        int kind = s->sync_kind;
        if (kind < 0 || kind >= MCP_KIND_COUNT) {
            mcp_sync_fail(s, MC_EPROTO);
            return;
        }
        char *params = mcp_list_params(s->sync[kind].cursor);
        u64 id = s->next_id++;
        char *body = agentc_mcp_rpc_request(id, mcp_kind_method(kind), params);
        agentc_free(params);
        if (s->kind == 0) {
            int rc = mcp_stdio_send(s, body, MCP_STEP_SLICE_MS);
            agentc_free(body);
            if (rc != 0) { mcp_sync_fail(s, rc); return; }
            s->pending_id = id;
            s->state = MCP_ST_SYNC_WAIT;
        } else {
            char *resp = NULL;
            int rc = mcp_http_post(s, body, agentc_strlen(body), id, false,
                                   MCP_STEP_SLICE_MS, NULL, &resp);
            agentc_free(body);
            if (rc == MC_ETIMEDOUT) return;   /* retry the request next pump */
            if (rc != 0) { mcp_sync_fail(s, rc); return; }
            rc = mcp_sync_handle_response(s, resp);
            agentc_free(resp);
            if (rc != 0) mcp_sync_fail(s, rc);
        }
        return;
    }

    case MCP_ST_SYNC_WAIT: {
        char *resp = NULL;
        int rc = mcp_stdio_poll(s, s->pending_id, &resp);
        if (rc < 0) { mcp_sync_fail(s, rc); return; }
        if (rc == 0) return;
        rc = mcp_sync_handle_response(s, resp);
        agentc_free(resp);
        if (rc != 0) mcp_sync_fail(s, rc);
        return;
    }

    default:
        return;
    }
}

void agentc_mcp_pump(void) {
    if (!g_servers.len) return;
    /* One server per call. Round-robin so every server still gets its
     * bounded step; a single-server setup is unaffected. */
    if (g_pump_cursor >= g_servers.len) g_pump_cursor = 0;
    McpServer *s = ((McpServer **)g_servers.p)[g_pump_cursor];
    g_pump_cursor = (g_pump_cursor + 1) % g_servers.len;
    if (s) mcp_server_step(s);
}

/* =========================================================================
 * Public API
 * ======================================================================= */
size_t agentc_mcp_tools(AgcTool *out, size_t max) {
    /* Pure snapshot: the pump owns every connect and re-sync, so this never
     * blocks or performs I/O. */
    size_t live = 0, wrote = 0;
    for (size_t i = 0; i < g_tools.len; i++) {
        McpToolEnt *t = ((McpToolEnt **)g_tools.p)[i];
        if (!t->alive) continue;
        live++;
        if (out && wrote < max) {
            AgcTool *ft = &out[wrote];
            agentc_memset(ft, 0, sizeof *ft);
            ft->name = t->name;
            ft->label = t->label;
            ft->desc = t->desc ? t->desc : "";
            ft->params_json = t->schema ? t->schema : "{\"type\":\"object\"}";
            ft->flags = t->flags;
            ft->ud = t;
            ft->run = mcp_run;
            ft->exec = NULL;
            t->handed_out = true;
            wrote++;
        }
    }
    return out ? wrote : live;
}

size_t agentc_mcp_server_count(void) { return g_servers.len; }

bool agentc_mcp_pending(void) {
    for (size_t i = 0; i < g_servers.len; i++) {
        McpServer *s = ((McpServer **)g_servers.p)[i];
        if (s && s->state != MCP_ST_READY && s->state != MCP_ST_FAILED) return true;
    }
    return false;
}

static void mcp_cfg_upsert(AgcVec *dst, const McpCfg *src) {
    for (size_t i = 0; i < dst->len; i++) {
        McpCfg *d = &((McpCfg *)dst->p)[i];
        if (agentc_streq(d->name, src->name)) {
            mcp_cfg_free_item(d);
            *d = *src;
            return;
        }
    }
    *(McpCfg *)agentc_vec_push(dst, sizeof(McpCfg)) = *src;
}

static bool mcp_read_config(const char *path, AgcVec *cfgs) {
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text) return false;
    int rc = agentc_mcp_cfg_parse(text, len, cfgs);
    if (rc != 0) agentc_logf(2, "mcp: ignoring malformed config: %s", path);
    agentc_free(text);
    return true;
}

static int mcp_connect_timeout_ms(void) {
    const char *v = agentc_env_get("AGENTC_MCP_CONNECT_TIMEOUT_MS");
    if (v && v[0]) {
        bool ok = false;
        i64 n = agentc_parse_i64(v, agentc_strlen(v), &ok);
        if (ok && n > 0) {
            if (n < 100) n = 100;
            if (n > 600000) n = 600000;
            return (int)n;
        }
    }
    return MCP_CONNECT_TIMEOUT_MS;
}

void agentc_mcp_start(const char *cwd, bool trusted) {
    agentc_mcp_shutdown();
    AgcVec cfgs = { 0 };
    char path[4096];
    const char *xdg = agentc_env_get("XDG_CONFIG_HOME");
    const char *home = agentc_env_get("HOME");
    if (xdg && xdg[0]) {
        int n = agentc_snprintf(path, sizeof path, "%s/agentc/mcp.jsonc", xdg);
        if (n > 0 && (size_t)n < sizeof path)
            (void)mcp_read_config(path, &cfgs);
    } else if (home && home[0]) {
        int n = agentc_snprintf(path, sizeof path, "%s/.config/agentc/mcp.jsonc", home);
        if (n > 0 && (size_t)n < sizeof path)
            (void)mcp_read_config(path, &cfgs);
    }
    if (trusted && cwd && cwd[0]) {
        int n = agentc_snprintf(path, sizeof path, "%s/.agentc/mcp.jsonc", cwd);
        if (n > 0 && (size_t)n < sizeof path) {
            AgcVec proj = { 0 };
            if (mcp_read_config(path, &proj)) {
                for (size_t i = 0; i < proj.len; i++)
                    mcp_cfg_upsert(&cfgs, &((McpCfg *)proj.p)[i]);
                agentc_vec_free(&proj);
            }
        }
    }

    int cto = mcp_connect_timeout_ms();
    for (size_t i = 0; i < cfgs.len; i++) {
        McpCfg *cfg = &((McpCfg *)cfgs.p)[i];
        if (!cfg->enabled) continue;
        bool http = cfg->url && cfg->url[0];
        bool stdio = cfg->command && cfg->command[0];
        if (!http && !stdio) {
            agentc_logf(2, "mcp: server \"%s\" has no command or url", cfg->name);
            continue;
        }
        McpServer *s = agentc_alloc(sizeof *s);
        s->wfd = s->rfd = s->hold_rfd = -1;
        s->kind = http ? 1 : 0;
        s->cfg = *cfg;                 /* move the parsed config into the server */
        agentc_memset(cfg, 0, sizeof *cfg);
        s->name = s->cfg.name;
        s->url = s->cfg.url;
        s->headers = s->cfg.headers;
        s->timeout_ms = s->cfg.timeout_ms;
        s->next_id = 1;
        s->caps = MCP_CAP_TOOLS;   /* until initialize says otherwise */
        s->inited = false;
        s->retry_count = 0;
        s->retry_at = 0;
        s->sync_kind = -1;
        s->state = MCP_ST_NEW;
        s->deadline = os_now_ns(OS_CLOCK_MONOTONIC) + (i64)cto * 1000000;
        *(McpServer **)agentc_vec_push(&g_servers, sizeof(McpServer *)) = s;
    }
    agentc_mcp_cfgs_free(&cfgs);
}

size_t agentc_mcp_load(const char *cwd, bool trusted) {
    agentc_mcp_start(cwd, trusted);
    /* Synchronous test/back-compat helper: pump to completion (bounded). The
     * app uses agentc_mcp_start() + the registry pump instead. */
    i64 cap = os_now_ns(OS_CLOCK_MONOTONIC) + 15000000000LL;
    for (int i = 0; i < 20000; i++) {
        size_t done = 0;
        for (size_t j = 0; j < g_servers.len; j++) {
            McpServer *s = ((McpServer **)g_servers.p)[j];
            if (s->state == MCP_ST_READY || s->state == MCP_ST_FAILED) done++;
        }
        if (done == g_servers.len) break;
        agentc_mcp_pump();
        os_poll(NULL, 0, 1);   /* yield to the child; never a long block */
        if (os_now_ns(OS_CLOCK_MONOTONIC) > cap) break;
    }
    size_t ready = 0;
    for (size_t j = 0; j < g_servers.len; j++) {
        McpServer *s = ((McpServer **)g_servers.p)[j];
        if (s->state == MCP_ST_READY) ready++;
    }
    return ready;
}

/* The process cwd context is a bounded static buffer owned by set_context:
 * the pump passes it straight into agentc_mcp_start(), which calls
 * agentc_mcp_shutdown(), so it must never be a pointer that shutdown frees. */
static char g_mcp_cwd[4096];

void agentc_mcp_shutdown(void) {
    for (size_t i = 0; i < g_servers.len; i++) mcp_server_free(((McpServer **)g_servers.p)[i]);
    agentc_vec_free(&g_servers);
    g_pump_cursor = 0;
    agentc_mcp_registry_reset();
    agentc_mcp_prompt_registry_reset();
    agentc_mcp_resource_registry_reset();
    g_resource_tools_attempted = false;   /* a reload may publish them again */
    g_mcp_started = false;
    g_mcp_ext_loaded = false;
}

/* ------------------------------------------------------- mcp extension */

/* cwd/trusted are set by the app before the first pump runs. The extension's
 * init only registers the pump; the pump's first call reads the config and
 * creates the server records, then connects asynchronously. */
static bool g_mcp_trusted;

void agentc_mcp_set_context(const char *cwd, bool trusted) {
    if (cwd && cwd[0]) {
        size_t n = agentc_strlen(cwd);
        if (n >= sizeof g_mcp_cwd) n = sizeof g_mcp_cwd - 1;
        agentc_memcpy(g_mcp_cwd, cwd, n);
        g_mcp_cwd[n] = 0;
    } else {
        g_mcp_cwd[0] = 0;
    }
    g_mcp_trusted = trusted;
}

static int mcp_pump_hook(void *ud) {
    (void)ud;
    if (!g_mcp_started) {
        /* First pump after load_all: create the server records now so trust
         * (and the final agentc_mcp_set_context) is already settled. */
        agentc_mcp_start(g_mcp_cwd[0] ? g_mcp_cwd : NULL, g_mcp_trusted);
        g_mcp_started = true;
        g_mcp_ext_loaded = true;   /* start() calls shutdown(), which clears it */
        /* Every record starts connecting; publish that once so a front end can
         * render the server list before the first successful initialize. */
        mcp_emit_servers_change();
    }
    agentc_mcp_pump();
    return 0;
}

static int mcp_ext_init(const AgcExtHost *host) {
    (void)host;
    /* Deliberately no agentc_mcp_start(): trust is resolved after load_all,
     * and no registry pump may run before the final set_context. */
    g_mcp_ext_loaded = true;
    agentc_ext_add_pump(mcp_pump_hook, NULL);
    return 0;
}

static const AgcExt g_mcp_ext = {
    .abi_version = AGENTC_EXT_ABI,
    .struct_size = sizeof(AgcExt),
    .name = "mcp",
    .version = "1",
    .order = 0,
    .init = mcp_ext_init,
    .shutdown = agentc_mcp_shutdown,
};

const AgcExt *agentc_mcp_ext(void) { return &g_mcp_ext; }
