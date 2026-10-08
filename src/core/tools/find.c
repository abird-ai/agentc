/* find.c — the find tool plus the shared recursive walker used by grep.
 *
 * The walker is deliberately dependency-free: a glob matcher (`*`, `?`, `**`,
 * `[class]`, `\` escapes), a simple .gitignore parser (blank/# comments,
 * `!` negation, trailing `/` directory-only, basename vs anchored patterns)
 * and agentc_dir_scan recursion. Symlinked directories are never followed because
 * the dirent type (DT_LNK) is not DT_DIR; recursion is bounded to 64 levels and
 * 8000 visited entries. Directory entries are sorted for deterministic output.
 */
#include "agent.h"
#include "plat.h"
#include "base/glob.h"
#include "base/limits.h"
#include "core/tools/args.h"
#include "core/tools/engine.h"

char *agentc_tool_read_err(bool *is_error, const char *fmt, ...);

/* internal helpers from config.c (not part of a frozen header) */
int agentc_dir_scan(const char *dir, int (*cb)(void *ud, const char *name, const char *full,
                                           bool is_dir),
                void *ud);
char *agentc_read_file_owned(const char *path, size_t *len);
bool agentc_path_join(char *out, size_t cap, const char *dir, const char *name);
bool agentc_path_is_dir(const char *path);

#define TREE_MAX_DEPTH 64

/* ----------------------------------------------------------- glob matcher */

bool agentc_tool_glob_match(const char *pattern, const char *text) {
    return agentc_glob_match(pattern, text);
}

/* --------------------------------------------------------- ignore stack */

typedef struct {
    char *pat;
    bool dir_only;
    bool neg;
} IgnoreRule;

typedef struct IgnoreLevel {
    struct IgnoreLevel *parent;
    IgnoreRule *rules;
    size_t nrules;
    char *base;
    char *rel;              /* walk-root-relative directory of this level */
    size_t rel_len;
} IgnoreLevel;

static void ignore_rules_free(IgnoreLevel *lv) {
    for (size_t i = 0; i < lv->nrules; i++) agentc_free(lv->rules[i].pat);
    agentc_free(lv->rules);
    agentc_free(lv->base);
    agentc_free(lv->rel);
    agentc_free(lv);
}

static void ignore_parse(IgnoreLevel *lv) {
    char path[4200];
    if (!agentc_path_join(path, sizeof path, lv->base, ".gitignore")) return;
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text) return;

    size_t cap = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t start = pos;
        while (pos < len && text[pos] != '\n') pos++;
        size_t end = pos;
        if (pos < len) pos++;
        if (end > start && text[end - 1] == '\r') end--;
        /* trim leading blanks */
        while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
        /* trailing spaces are part of the pattern unless escaped; trim anyway */
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
        if (start >= end || text[start] == '#') continue;
        size_t s = start;
        bool neg = false;
        if (text[s] == '!') {
            neg = true;
            s++;
        }
        size_t e = end;
        bool dir_only = false;
        if (e > s && text[e - 1] == '/') {
            dir_only = true;
            e--;
        }
        /* A line that is only `!` and/or `/` trims to nothing: there is no
         * pattern to match, so do not record an (empty) rule. */
        if (e <= s) continue;
        if (lv->nrules == cap) {
            cap = cap ? cap * 2 : 8;
            lv->rules = agentc_realloc(lv->rules, cap * sizeof(IgnoreRule));
        }
        IgnoreRule *r = &lv->rules[lv->nrules++];
        agentc_memset(r, 0, sizeof *r);
        r->neg = neg;
        r->dir_only = dir_only;
        r->pat = agentc_strdup_len(text + s, e - s);
    }
    agentc_free(text);
}

static IgnoreLevel *ignore_push(IgnoreLevel *parent, const char *dir, const char *rel) {
    IgnoreLevel *lv = agentc_alloc(sizeof *lv);
    agentc_memset(lv, 0, sizeof *lv);
    lv->parent = parent;
    lv->base = agentc_strdup(dir);
    lv->rel = agentc_strdup(rel ? rel : "");
    lv->rel_len = agentc_strlen(lv->rel);
    ignore_parse(lv);
    return lv;
}

static const char *path_basename(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/') b = q + 1;
    return b;
}

static bool ignore_rule_matches(const IgnoreRule *r, const char *sub, const char *base) {
    if (r->pat[0] == '/') return agentc_tool_glob_match(r->pat + 1, sub);
    if (agentc_str_str(r->pat, "/")) return agentc_tool_glob_match(r->pat, sub);
    return agentc_tool_glob_match(r->pat, base);
}

static int ignore_decide(const IgnoreLevel *lv, const char *rel, bool is_dir) {
    int d = lv->parent ? ignore_decide(lv->parent, rel, is_dir) : 0;
    const char *sub = rel;
    if (lv->rel_len && agentc_str_eq(rel, lv->rel_len, lv->rel, lv->rel_len)) {
        if (rel[lv->rel_len] == '/') sub = rel + lv->rel_len + 1;
        else if (rel[lv->rel_len] == 0) sub = "";
    }
    const char *base = path_basename(sub);
    for (size_t i = 0; i < lv->nrules; i++) {
        const IgnoreRule *r = &lv->rules[i];
        if (r->dir_only && !is_dir) continue;
        if (ignore_rule_matches(r, sub, base)) d = r->neg ? 2 : 1;
    }
    return d;
}

/* ------------------------------------------------------------- walker */

typedef int (*AgcTreeFn)(void *ud, const char *full, const char *rel, const char *name,
                        bool is_dir);

typedef struct {
    AgcTreeFn cb;
    void *ud;
    size_t visited;
    bool truncated;
    int stopped;
} TreeCtx;

typedef struct {
    char *name;
    bool is_dir;
} TreeEnt;

static int collect_ent(void *ud, const char *name, const char *full, bool is_dir) {
    (void)full;
    AgcVec *v = ud;
    TreeEnt *e = agentc_vec_push(v, sizeof *e);
    e->name = agentc_strdup(name);
    e->is_dir = is_dir;
    return 0;
}

static void sort_ents(TreeEnt *e, size_t n) {
    for (size_t i = 1; i < n; i++) {
        TreeEnt key = e[i];
        size_t j = i;
        while (j > 0) {
            const char *a = e[j - 1].name, *b = key.name;
            size_t k = 0;
            while (a[k] && a[k] == b[k]) k++;
            int c = (int)(unsigned char)a[k] - (int)(unsigned char)b[k];
            if (c <= 0) break;
            e[j] = e[j - 1];
            j--;
        }
        e[j] = key;
    }
}

static void tree_walk_dir(const char *dir, const char *rel_dir, int depth,
                          IgnoreLevel *parent, TreeCtx *ctx) {
    if (depth > TREE_MAX_DEPTH || ctx->truncated || ctx->stopped) return;
    IgnoreLevel *lv = ignore_push(parent, dir, rel_dir);

    AgcVec ents = { 0 };
    (void)agentc_dir_scan(dir, collect_ent, &ents);
    sort_ents((TreeEnt *)ents.p, ents.len);

    for (size_t i = 0; i < ents.len; i++) {
        TreeEnt *e = &((TreeEnt *)ents.p)[i];
        if (ctx->visited >= AGENTC_LIMIT_FIND_ENTRIES) {
            ctx->truncated = true;
            break;
        }
        ctx->visited++;
        if (agentc_streq(e->name, ".git")) continue;
        char full[4200];
        char rel[4200];
        if (!agentc_path_join(full, sizeof full, dir, e->name)) continue;
        if (rel_dir[0])
            agentc_snprintf(rel, sizeof rel, "%s/%s", rel_dir, e->name);
        else
            agentc_snprintf(rel, sizeof rel, "%s", e->name);
        if (ignore_decide(lv, rel, e->is_dir) == 1) continue;
        if (ctx->cb(ctx->ud, full, rel, e->name, e->is_dir)) {
            ctx->stopped = 1;
            break;
        }
        if (e->is_dir) tree_walk_dir(full, rel, depth + 1, lv, ctx);
    }

    for (size_t i = 0; i < ents.len; i++) agentc_free(((TreeEnt *)ents.p)[i].name);
    agentc_vec_free(&ents);
    ignore_rules_free(lv);
}

/* Walks `root` depth-first. cb is called for every non-ignored entry (files
 * and directories); returning nonzero stops the walk. Returns 1 when stopped
 * by the callback. */
int agentc_tree_walk(const char *root, AgcTreeFn cb, void *ud, bool *truncated) {
    TreeCtx ctx;
    agentc_memset(&ctx, 0, sizeof ctx);
    ctx.cb = cb;
    ctx.ud = ud;
    if (!agentc_path_is_dir(root)) {
        ctx.visited = 1;
        (void)cb(ud, root, path_basename(root), path_basename(root), false);
    } else {
        tree_walk_dir(root, "", 0, NULL, &ctx);
    }
    if (truncated) *truncated = ctx.truncated;
    return ctx.stopped;
}

/* ---------------------------------------------------------------- find */

typedef struct {
    const char *pattern;
    AgcVec out;
    size_t limit;
    bool limit_hit;
} FindCtx;

static int find_cb(void *ud, const char *full, const char *rel, const char *name,
                   bool is_dir) {
    (void)is_dir;
    FindCtx *c = ud;
    const char *target = agentc_str_str(c->pattern, "/") ? rel : name;
    if (!agentc_tool_glob_match(c->pattern, target)) return 0;
    if (c->out.len >= c->limit) {
        c->limit_hit = true;
        return 1;
    }
    *(char **)agentc_vec_push(&c->out, sizeof(char *)) = agentc_strdup(full);
    return 0;
}

char *agentc_tool_find(const char *pattern, const char *path, i64 limit, bool *is_error) {
    if (is_error) *is_error = false;
    if (!pattern || !pattern[0])
        return agentc_tool_read_err(is_error, "error: find: pattern is required");
    const char *root = (path && path[0]) ? path : ".";
    struct os_stat st;
    int sr = os_stat(root, &st);
    if (sr < 0)
        return agentc_tool_read_err(is_error, "error: find: cannot access %s (errno %d)", root,
                                sr);
    size_t lim = limit > 0 ? (size_t)limit : AGENTC_LIMIT_FIND_PATHS;
    if (lim > AGENTC_LIMIT_FIND_PATHS_MAX) lim = AGENTC_LIMIT_FIND_PATHS_MAX;

    FindCtx c;
    agentc_memset(&c, 0, sizeof c);
    c.pattern = pattern;
    c.limit = lim;
    bool tree_truncated = false;
    (void)agentc_tree_walk(root, find_cb, &c, &tree_truncated);

    AgcBuf out = { 0 };
    for (size_t i = 0; i < c.out.len; i++) {
        agentc_buf_cstr(&out, ((char **)c.out.p)[i]);
        agentc_buf_byte(&out, '\n');
        agentc_free(((char **)c.out.p)[i]);
    }
    agentc_vec_free(&c.out);
    if (c.limit_hit)
        agentc_buf_printf(&out, "[find truncated at %llu paths]\n", (unsigned long long)lim);
    if (tree_truncated)
        agentc_buf_cstr(&out, "[find stopped after 8000 entries]\n");
    if (!out.p) return agentc_strdup_len("no matches\n", 11);
    return (char *)out.p;
}

/* Shared argument parser used by every find backend so the accepted schema and
 * the error text stay byte-identical to the built-in. Returns 0 = proceed,
 * 1 = answered (out and *is_error hold the exact tool text). The parser owns a
 * small JSON arena; the string views point into it and stay valid until
 * `_free`. There is no fatal return.
 */
int agentc_tool_find_args(const AgcToolCall *call, AgcFindArgs *fa, AgcBuf *out,
                          bool *is_error) {
    if (is_error) *is_error = false;
    agentc_memset(fa, 0, sizeof *fa);
    const char *json = call != NULL ? call->args_json : NULL;
    size_t n = json != NULL ? agentc_strlen(json) : 0;
    AgcJsonArena *arena = NULL;
    AgcJson *root = NULL;
    if (json != NULL && n > 0) {
        arena = agentc_json_arena_new(AGENTC_LIMIT_TOOL_ARENA);
        if (arena == NULL) {
            /* OOM is not a parse error; report it distinctly. */
            agentc_buf_cstr(out, "error: find: out of memory");
            if (is_error) *is_error = true;
            return 1;
        }
        AgcJson *r = agentc_json_parse_in(arena, json, n);
        if (agentc_json_type(r) == AGENTC_JSON_OBJ) root = r;
    }
    if (root == NULL) {
        agentc_json_arena_free(arena);
        agentc_buf_cstr(out, "error: find: arguments must be a JSON object");
        if (is_error) *is_error = true;
        return 1;
    }
    const char *pattern = agentc_json_get_str(root, "pattern");
    if (pattern == NULL) {
        agentc_tool_args_missing(out, "find", "pattern", is_error);
        agentc_json_arena_free(arena);
        return 1;
    }
    if (pattern[0] == 0) {
        /* The built-in rejects an empty pattern; without this the fd/POSIX
         * rungs would pass `''` through, which matches everything. */
        agentc_buf_cstr(out, "error: find: pattern is required");
        if (is_error) *is_error = true;
        agentc_json_arena_free(arena);
        return 1;
    }
    AgcJson *lv = agentc_json_get(root, "limit");
    if (lv != NULL && agentc_json_type(lv) != AGENTC_JSON_NUM) {
        agentc_buf_cstr(out, "error: find: limit must be a number");
        if (is_error) *is_error = true;
        agentc_json_arena_free(arena);
        return 1;
    }
    fa->arena = arena;
    fa->pattern = pattern;
    fa->path = agentc_json_get_str(root, "path");
    fa->limit = agentc_json_get_int(root, "limit", 0);
    return 0;
}

void agentc_tool_find_args_free(AgcFindArgs *fa) {
    if (fa == NULL) return;
    agentc_json_arena_free(fa->arena);
    fa->arena = NULL;
}

int agentc_tool_find_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                         bool *is_error) {
    (void)self;
    AgcFindArgs fa;
    if (agentc_tool_find_args(call, &fa, out, is_error) != 0) {
        agentc_tool_find_args_free(&fa);
        return 0;
    }
    bool err = false;
    char *res = agentc_tool_find(fa.pattern, fa.path, fa.limit, &err);
    if (res != NULL) {
        agentc_buf_cstr(out, res);
        agentc_free(res);
    }
    if (is_error) *is_error = err;
    agentc_tool_find_args_free(&fa);
    return 0;
}
