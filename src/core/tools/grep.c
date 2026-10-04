/* grep.c — the grep tool: own scanner (literal + a small back-tracking regex),
 * per-line 500-char cap, optional context and glob filter.
 *
 * Output is "path:line:text" for every match and context line. Directories are
 * walked with the shared .gitignore-aware walker from find.c (bounded to 8000
 * entries, symlinked directories are not followed). `limit` caps the number of
 * matches, not the number of printed context lines.
 */
#include "agent.h"
#include "plat.h"
#include "base/glob.h"
#include "base/limits.h"
#include "core/tools/args.h"
#include "core/tools/engine.h"

char *agentc_tool_read_err(bool *is_error, const char *fmt, ...);

/* shared with find.c (not part of a frozen header) */
typedef int (*AgcTreeFn)(void *ud, const char *full, const char *rel, const char *name,
                        bool is_dir);
int agentc_tree_walk(const char *root, AgcTreeFn cb, void *ud, bool *truncated);
char *agentc_read_file_owned(const char *path, size_t *len);

/* Backtracking steps allowed for one regex search. A hostile pattern (many
 * `a*` groups against a long run of `a`) is exponential without a budget; on
 * exhaustion the search reports "no match" instead of hanging. */
#define GREP_RE_STEP_BUDGET 1000000u

/* Defense in depth against a pattern that makes re_here recurse once per
 * quantifier: reject anything longer than this at match time and match it as a
 * plain substring, and cap the recursion depth inside re_here. */
#define GREP_RE_MAX_PATTERN 4096u
#define GREP_RE_MAX_DEPTH 512u

/* --------------------------------------------------------- regex engine */

static char lower_ch(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool ch_eq(char a, char b, bool icase) {
    return icase ? lower_ch(a) == lower_ch(b) : a == b;
}

/* p points at an atom start; returns the first byte after the atom. */
static const char *re_atom_end(const char *p) {
    if (p[0] == '\\' && p[1]) return p + 2;
    if (p[0] == '[') {
        const char *q = p + 1;
        if (*q == '^' || *q == '!') q++;
        bool first = true;
        while (*q && (*q != ']' || first)) {
            first = false;
            if (q[0] == '\\' && q[1]) q++;
            q++;
        }
        if (*q == ']') q++;
        return q;
    }
    return p + 1;
}

static bool re_atom_match(const char *p, const char *pe, char c, bool icase) {
    if (p[0] == '.') return true;
    if (p[0] == '\\' && p + 1 < pe) return ch_eq(p[1], c, icase);
    if (p[0] == '[') {
        const char *q = p + 1;
        bool neg = false;
        if (*q == '^' || *q == '!') {
            neg = true;
            q++;
        }
        bool hit = false;
        bool first = true;
        while (q < pe && (*q != ']' || first)) {
            first = false;
            char lo, hi;
            if (q[0] == '\\' && q + 1 < pe) q++;
            lo = *q++;
            hi = lo;
            if (q + 1 < pe && q[0] == '-') {
                q++;
                if (q[0] == '\\' && q + 1 < pe) q++;
                hi = *q++;
            }
            char cc = icase ? lower_ch(c) : c;
            if (cc >= lo && cc <= hi) hit = true;
        }
        return hit != neg;
    }
    return ch_eq(p[0], c, icase);
}

static bool re_here(const char *p, const char *t, bool icase, u32 *steps, u32 depth) {
    if (depth > GREP_RE_MAX_DEPTH) return false;
    for (;;) {
        if (*steps == 0) return false;
        (*steps)--;
        if (!*p) return true;
        if (p[0] == '$' && p[1] == 0) return !*t;
        const char *pe = re_atom_end(p);
        char quant = (*pe == '*' || *pe == '+' || *pe == '?') ? *pe : 0;
        const char *rest = quant ? pe + 1 : pe;
        if (quant == '*' || quant == '+') {
            const char *s = t;
            if (quant == '+') {
                if (!*s || !re_atom_match(p, pe, *s, icase)) return false;
                s++;
            }
            for (;;) {
                if (re_here(rest, s, icase, steps, depth + 1)) return true;
                if (*steps == 0) return false;
                if (!*s || !re_atom_match(p, pe, *s, icase)) return false;
                s++;
            }
        }
        if (quant == '?') {
            if (*t && re_atom_match(p, pe, *t, icase) &&
                re_here(rest, t + 1, icase, steps, depth + 1))
                return true;
            if (*steps == 0) return false;
            p = rest;
            continue;
        }
        if (!*t || !re_atom_match(p, pe, *t, icase)) return false;
        p = pe;
        t++;
    }
}

static bool re_search(const char *pat, const char *line, bool icase) {
    /* Bound backtracking work, not the linear scan: a late match on a long
     * single line must still be found. Scale the allowance with the line and
     * pattern lengths (the linear cost is bounded by their product) and cap it
     * so a pathological pattern cannot run away. */
    u64 need = (u64)agentc_strlen(line) * (u64)(agentc_strlen(pat) + 2) +
               GREP_RE_STEP_BUDGET;
    u32 steps = need > 256000000u ? 256000000u : (u32)need;
    if (pat[0] == '^') return re_here(pat + 1, line, icase, &steps, 0);
    for (const char *s = line;; s++) {
        if (steps == 0) return false;
        steps--;
        if (re_here(pat, s, icase, &steps, 0)) return true;
        if (!*s) return false;
    }
}

/* ----------------------------------------------------------- matching */

typedef struct {
    const char *pat;
    size_t pat_len;
    bool literal;
    bool icase;
} GrepPat;

static bool literal_search(const char *hay, size_t hn, const char *needle, size_t nn,
                           bool icase) {
    if (nn == 0) return true;
    if (nn > hn) return false;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        while (j < nn) {
            char a = hay[i + j], b = needle[j];
            if (icase) {
                a = lower_ch(a);
                b = lower_ch(b);
            }
            if (a != b) break;
            j++;
        }
        if (j == nn) return true;
    }
    return false;
}

/* line must be NUL-terminated */
static bool grep_match(const GrepPat *g, const char *line) {
    /* Over-long patterns skip the recursive engine entirely and are matched as
     * plain substrings. A pattern that long cannot be a useful regex and would
     * otherwise recurse once per atom; the literal path is linear and safe. */
    if (g->literal || g->pat_len > GREP_RE_MAX_PATTERN)
        return literal_search(line, agentc_strlen(line), g->pat, g->pat_len, g->icase);
    return re_search(g->pat, line, g->icase);
}

/* -------------------------------------------------------------- scanner */

typedef struct {
    size_t start, end;   /* byte range in the file buffer, without '\n' */
} GLine;

typedef struct {
    const GrepPat *pat;
    const char *glob;
    size_t limit;
    i64 context;
    size_t matches;
    bool limit_hit;
    AgcBuf out;
} GrepCtx;

static bool is_binary(const u8 *p, size_t n) {
    size_t cap = n < 8192 ? n : 8192;
    for (size_t i = 0; i < cap; i++)
        if (p[i] == 0) return true;
    return false;
}

static void emit_line(GrepCtx *c, const char *path, size_t lineno, const char *p,
                      size_t n) {
    agentc_buf_cstr(&c->out, path);
    agentc_buf_byte(&c->out, ':');
    agentc_buf_u64(&c->out, lineno);
    agentc_buf_byte(&c->out, ':');
    size_t take = n < AGENTC_LIMIT_GREP_LINE ? n : AGENTC_LIMIT_GREP_LINE;
    agentc_buf_push(&c->out, p, take);
    if (take < n) agentc_buf_cstr(&c->out, "...");
    agentc_buf_byte(&c->out, '\n');
}

static void grep_file(GrepCtx *c, const char *path, const char *name) {
    if (c->glob && !agentc_glob_match(c->glob, name)) return;
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text) return;
    if (len > AGENTC_LIMIT_GREP_FILE || is_binary((const u8 *)text, len)) {
        agentc_free(text);
        return;
    }

    AgcVec lines = { 0 };
    {
        size_t pos = 0;
        while (pos < len) {
            size_t s = pos;
            while (pos < len && text[pos] != '\n') pos++;
            size_t e = pos;
            if (pos < len) pos++;
            if (e > s && text[e - 1] == '\r') e--;
            GLine *gl = agentc_vec_push(&lines, sizeof *gl);
            gl->start = s;
            gl->end = e;
        }
    }
    size_t nlines = lines.len;
    bool *match = nlines ? agentc_alloc(nlines * sizeof(bool)) : NULL;
    if (match) agentc_memset(match, 0, nlines * sizeof(bool));

    AgcBuf lbuf = { 0 };
    for (size_t i = 0; i < nlines; i++) {
        GLine *gl = &((GLine *)lines.p)[i];
        agentc_buf_clear(&lbuf);
        agentc_buf_push(&lbuf, text + gl->start, gl->end - gl->start);
        agentc_buf_byte(&lbuf, 0);
        if (!grep_match(c->pat, (const char *)lbuf.p)) continue;
        if (c->matches >= c->limit) {
            c->limit_hit = true;
            break;
        }
        match[i] = true;
        c->matches++;
    }
    agentc_buf_free(&lbuf);

    size_t last = (size_t)-1;
    for (size_t i = 0; i < nlines; i++) {
        if (!match[i]) continue;
        i64 ctx = c->context;
        if (ctx > AGENTC_LIMIT_GREP_CONTEXT) ctx = AGENTC_LIMIT_GREP_CONTEXT;
        size_t from = i > (size_t)ctx ? i - (size_t)ctx : 0;
        if (last != (size_t)-1 && last + 1 > from) from = last + 1;
        size_t to = i + (size_t)ctx;
        if (to >= nlines) to = nlines - 1;
        for (size_t j = from; j <= to; j++) {
            GLine *gl = &((GLine *)lines.p)[j];
            emit_line(c, path, j + 1, text + gl->start, gl->end - gl->start);
        }
        last = to;
    }

    agentc_free(match);
    agentc_vec_free(&lines);
    agentc_free(text);
}

static int grep_tree_cb(void *ud, const char *full, const char *rel, const char *name,
                        bool is_dir) {
    (void)rel;
    GrepCtx *c = ud;
    if (is_dir) return 0;
    grep_file(c, full, name);
    return c->limit_hit ? 1 : 0;
}

char *agentc_tool_grep(const char *pattern, const char *path, const char *glob,
                   bool ignore_case, bool literal, i64 context, i64 limit,
                   bool *is_error) {
    if (is_error) *is_error = false;
    if (!pattern || !pattern[0])
        return agentc_tool_read_err(is_error, "error: grep: pattern is required");
    const char *root = (path && path[0]) ? path : ".";
    struct os_stat st;
    int sr = os_stat(root, &st);
    if (sr < 0)
        return agentc_tool_read_err(is_error, "error: grep: cannot access %s (errno %d)", root,
                                sr);

    GrepPat gp;
    gp.pat = pattern;
    gp.pat_len = agentc_strlen(pattern);
    gp.literal = literal;
    gp.icase = ignore_case;

    GrepCtx c;
    agentc_memset(&c, 0, sizeof c);
    c.pat = &gp;
    c.glob = (glob && glob[0]) ? glob : NULL;
    c.limit = limit > 0 ? (size_t)limit : AGENTC_LIMIT_GREP_MATCHES;
    if (c.limit > AGENTC_LIMIT_GREP_MATCHES_MAX) c.limit = AGENTC_LIMIT_GREP_MATCHES_MAX;
    c.context = context > 0 ? context : 0;

    bool tree_truncated = false;
    (void)agentc_tree_walk(root, grep_tree_cb, &c, &tree_truncated);

    if (c.matches == 0 && !c.limit_hit) {
        agentc_buf_cstr(&c.out, "no matches\n");
    }
    if (c.limit_hit)
        agentc_buf_printf(&c.out, "[grep truncated at %llu matches]\n",
                      (unsigned long long)c.limit);
    if (tree_truncated)
        agentc_buf_cstr(&c.out, "[grep stopped after 8000 entries]\n");
    if (!c.out.p) return agentc_strdup_len("", 0);
    return (char *)c.out.p;
}

/* Shared argument parser used by every grep backend so the accepted schema and
 * the error text stay byte-identical to the built-in. Returns 0 = proceed,
 * 1 = answered (out and *is_error hold the exact tool text). The parser owns a
 * small JSON arena; the string views point into it and stay valid until
 * `_free`. There is no fatal return.
 */
int agentc_tool_grep_args(const AgcToolCall *call, AgcGrepArgs *ga, AgcBuf *out,
                          bool *is_error) {
    if (is_error) *is_error = false;
    agentc_memset(ga, 0, sizeof *ga);
    const char *json = call != NULL ? call->args_json : NULL;
    size_t n = json != NULL ? agentc_strlen(json) : 0;
    AgcJsonArena *arena = NULL;
    AgcJson *root = NULL;
    if (json != NULL && n > 0) {
        arena = agentc_json_arena_new(AGENTC_LIMIT_TOOL_ARENA);
        if (arena == NULL) {
            /* OOM is not a parse error; report it distinctly. */
            agentc_buf_cstr(out, "error: grep: out of memory");
            if (is_error) *is_error = true;
            return 1;
        }
        AgcJson *r = agentc_json_parse_in(arena, json, n);
        if (agentc_json_type(r) == AGENTC_JSON_OBJ) root = r;
    }
    if (root == NULL) {
        agentc_json_arena_free(arena);
        agentc_buf_cstr(out, "error: grep: arguments must be a JSON object");
        if (is_error) *is_error = true;
        return 1;
    }
    const char *pattern = agentc_json_get_str(root, "pattern");
    if (pattern == NULL) {
        agentc_tool_args_missing(out, "grep", "pattern", is_error);
        agentc_json_arena_free(arena);
        return 1;
    }
    if (pattern[0] == 0) {
        /* The built-in rejects an empty pattern; without this the rg/POSIX
         * rungs would spawn `rg -- '' .`, which matches everything. */
        agentc_buf_cstr(out, "error: grep: pattern is required");
        if (is_error) *is_error = true;
        agentc_json_arena_free(arena);
        return 1;
    }
    const char *path = agentc_json_get_str(root, "path");
    const char *glob = agentc_json_get_str(root, "glob");
    bool icase = agentc_json_get_bool(root, "ignore_case", false);
    bool literal = agentc_json_get_bool(root, "literal", false);
    AgcJson *cv = agentc_json_get(root, "context");
    if (cv != NULL && agentc_json_type(cv) != AGENTC_JSON_NUM) {
        agentc_buf_cstr(out, "error: grep: context must be a number");
        if (is_error) *is_error = true;
        agentc_json_arena_free(arena);
        return 1;
    }
    AgcJson *lv = agentc_json_get(root, "limit");
    if (lv != NULL && agentc_json_type(lv) != AGENTC_JSON_NUM) {
        agentc_buf_cstr(out, "error: grep: limit must be a number");
        if (is_error) *is_error = true;
        agentc_json_arena_free(arena);
        return 1;
    }
    ga->arena = arena;
    ga->pattern = pattern;
    ga->path = path;
    ga->glob = glob;
    ga->icase = icase;
    ga->literal = literal;
    ga->context = agentc_json_get_int(root, "context", 0);
    ga->limit = agentc_json_get_int(root, "limit", 0);
    return 0;
}

void agentc_tool_grep_args_free(AgcGrepArgs *ga) {
    if (ga == NULL) return;
    agentc_json_arena_free(ga->arena);
    ga->arena = NULL;
}

int agentc_tool_grep_run(const AgcTool *self, const AgcToolCall *call, AgcBuf *out,
                         bool *is_error) {
    (void)self;
    AgcGrepArgs ga;
    if (agentc_tool_grep_args(call, &ga, out, is_error) != 0) {
        agentc_tool_grep_args_free(&ga);
        return 0;
    }
    bool err = false;
    char *res = agentc_tool_grep(ga.pattern, ga.path, ga.glob, ga.icase, ga.literal,
                                 ga.context, ga.limit, &err);
    if (res != NULL) {
        agentc_buf_cstr(out, res);
        agentc_free(res);
    }
    if (is_error) *is_error = err;
    agentc_tool_grep_args_free(&ga);
    return 0;
}
