/* resources.c — context files, skills, prompt templates, named themes and the
 * resource root store.
 *
 * Discovery is deterministic and offline:
 *   - context files: AGENTC.md, AGENTS.override.md, AGENTS.md, CLAUDE.md from the
 *     user config dir and every cwd ancestor (root first, cwd last); ancestor
 *     files load only when the project is trusted.
 *   - skills: <config>/skills/ and <cwd>/.agentc/skills/, recursively; a directory holding
 *     SKILL.md is a skill root and is not descended into. Frontmatter is the
 *     minimal `---\nname: x\ndescription: y\n---` dialect.
 *   - templates: *.md in <config>/prompts/, <cwd>/.agentc/prompts/ and the
 *     resources_discover prompt roots; name = validated stem.
 *   - themes: *.jsonc in <config>/themes/, <cwd>/.agentc/themes/ and the
 *     resources_discover theme roots; name = validated stem.
 *
 * resources_discover contributes extra roots per kind into process-lifetime,
 * deduplicated stores; the loaders scan the standard roots first, then the
 * extras in order, so a later same-named resource wins.
 *
 * Invalid skill names produce a warning diagnostic but still load; invalid
 * template/theme stems are skipped with a warning.
 */
#include "agentc.h"
#include "config.h"
#include "base/limits.h"
#include "core/prompt.h"

typedef int (*AgcDirFn)(void *ud, const char *name, const char *full, bool is_dir);

extern char *agentc_read_file_owned(const char *path, size_t *len);
extern bool agentc_path_join(char *out, size_t cap, const char *dir, const char *name);
extern const char *agentc_config_home(char *buf, size_t cap);
extern bool agentc_path_is_dir(const char *path);
extern bool agentc_path_is_file(const char *path);
extern int agentc_dir_scan(const char *dir, AgcDirFn cb, void *ud);

#define PATH_MAX_ 4096
#define MAX_SCAN_DEPTH 8

/* ---------------------------------------------------------- resource roots */
/* Extra roots contributed by resources_discover. One store per kind, process
 * lifetime, deduplicated and capped; later roots scan later, so a same-named
 * resource in a later root wins. */
typedef struct {
    char *dirs[AGENTC_RESOURCE_ROOTS_MAX];
    size_t n;
} ResourceRoots;

static ResourceRoots g_skill_roots;
static ResourceRoots g_prompt_roots;
static ResourceRoots g_theme_roots;

static void roots_add(ResourceRoots *r, const char *kind, const char *dir) {
    if (!dir || !dir[0]) return;
    for (size_t i = 0; i < r->n; i++)
        if (agentc_streq(r->dirs[i], dir)) return;
    if (r->n >= AGENTC_RESOURCE_ROOTS_MAX) {
        agentc_logf(2, "resources_discover: too many %s roots; ignoring %s", kind, dir);
        return;
    }
    r->dirs[r->n++] = agentc_strdup(dir);
}

void agentc_resource_add_skill_root(const char *dir) { roots_add(&g_skill_roots, "skill", dir); }
void agentc_resource_add_prompt_root(const char *dir) { roots_add(&g_prompt_roots, "prompt", dir); }
void agentc_resource_add_theme_root(const char *dir) { roots_add(&g_theme_roots, "theme", dir); }

static size_t roots_get(const ResourceRoots *r, const char *const **out) {
    if (out) *out = r->n ? (const char *const *)r->dirs : NULL;
    return r->n;
}

size_t agentc_resource_skill_roots(const char *const **out) { return roots_get(&g_skill_roots, out); }
size_t agentc_resource_prompt_roots(const char *const **out) { return roots_get(&g_prompt_roots, out); }
size_t agentc_resource_theme_roots(const char *const **out) { return roots_get(&g_theme_roots, out); }

/* Prompt/template names and theme stems, shared with core/prompts.c. */
bool agentc_prompt_name_valid(const char *name) {
    if (!name) return false;
    size_t n = agentc_strlen(name);
    if (n == 0 || n > AGENTC_PROMPT_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '_' || c == ':' || c == '-'))
            return false;
    }
    return true;
}

static bool theme_name_valid(const char *name) {
    if (!name) return false;
    size_t n = agentc_strlen(name);
    if (n == 0 || n > AGENTC_THEME_NAME_MAX) return false;
    if ((n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.'))
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

/* ------------------------------------------------------------ frontmatter */

typedef struct {
    const char *name;
    size_t name_len;
    const char *description;
    size_t desc_len;
    const char *hint;
    size_t hint_len;
    const char *body;
    size_t body_len;
} Fm;

static const char *trim_left(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
    return p;
}

static const char *trim_right(const char *p, const char *end) {
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    return end;
}

/* Trim leading/trailing whitespace (including newlines) of the body. */
static void trim_body(const char *p, const char *end, const char **out, size_t *out_len) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        end--;
    *out = p;
    *out_len = (size_t)(end - p);
}

/* Parse optional `---` frontmatter. Pointers alias `text`; the caller copies
 * what it keeps. */
static void fm_parse(const char *text, size_t len, Fm *fm) {
    agentc_memset(fm, 0, sizeof *fm);
    if (len >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF) {
        text += 3;
        len -= 3;
    }
    if (len < 4 || text[0] != '-' || text[1] != '-' || text[2] != '-' ||
        (text[3] != '\n' && text[3] != '\r')) {
        trim_body(text, text + len, &fm->body, &fm->body_len);
        return;
    }
    size_t pos = 3;
    while (pos < len && text[pos] != '\n') pos++;
    if (pos >= len) {
        fm->body = text;
        fm->body_len = len;
        return;
    }
    pos++;
    size_t ystart = pos;
    size_t yend = 0;
    bool closed = false;
    while (pos < len) {
        size_t ls = pos;
        while (pos < len && text[pos] != '\n') pos++;
        size_t le = pos;
        if (pos < len) pos++;
        size_t te = le;
        if (te > ls && text[te - 1] == '\r') te--;
        if (te - ls == 3 && text[ls] == '-' && text[ls + 1] == '-' && text[ls + 2] == '-') {
            yend = ls;
            closed = true;
            break;
        }
    }
    if (!closed) {
        fm->body = text;
        fm->body_len = len;
        return;
    }

    size_t p = ystart;
    while (p < yend) {
        size_t ls = p;
        while (p < yend && text[p] != '\n') p++;
        size_t le = p;
        if (p < yend) p++;
        size_t te = le;
        if (te > ls && text[te - 1] == '\r') te--;
        const char *l = trim_left(text + ls, text + te);
        const char *r = trim_right(l, text + te);
        if (l >= r) continue;
        const char *colon = NULL;
        for (const char *q = l; q < r; q++)
            if (*q == ':') {
                colon = q;
                break;
            }
        if (!colon) continue;
        const char *k = trim_left(l, colon);
        const char *ke = trim_right(k, colon);
        const char *v = trim_left(colon + 1, r);
        const char *ve = trim_right(v, r);
        if (ve - v >= 2 && ((*v == '"' && ve[-1] == '"') || (*v == '\'' && ve[-1] == '\''))) {
            v++;
            ve--;
        }
        if (agentc_str_eq(k, (size_t)(ke - k), "name", 4)) {
            fm->name = v;
            fm->name_len = (size_t)(ve - v);
        } else if (agentc_str_eq(k, (size_t)(ke - k), "description", 11)) {
            fm->description = v;
            fm->desc_len = (size_t)(ve - v);
        } else if (agentc_str_eq(k, (size_t)(ke - k), "argument-hint", 13)) {
            fm->hint = v;
            fm->hint_len = (size_t)(ve - v);
        }
    }
    trim_body(text + pos, text + len, &fm->body, &fm->body_len);
}

/* Frontmatter-stripped body of one skill file; owned, NULL on read failure. */
char *agentc_skill_body_read(const char *path, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!path) return NULL;
    size_t len = 0;
    char *text = agentc_read_file_owned(path, &len);
    if (!text) return NULL;
    Fm fm;
    fm_parse(text, len, &fm);
    char *body = agentc_strdup_len(fm.body, fm.body_len);
    if (out_len) *out_len = fm.body_len;
    agentc_free(text);
    return body;
}

/* ---------------------------------------------------------------- context */

static const char *context_names[] = { "AGENTC.md", "AGENTS.override.md", "AGENTS.md", "CLAUDE.md" };

static void add_context_dir(AgcVec *paths, AgcVec *lens, AgcVec *texts, const char *dir) {
    for (size_t i = 0; i < sizeof context_names / sizeof context_names[0]; i++) {
        char full[PATH_MAX_];
        if (!agentc_path_join(full, sizeof full, dir, context_names[i])) continue;
        if (!agentc_path_is_file(full)) continue;
        size_t len = 0;
        char *text = agentc_read_file_owned(full, &len);
        if (!text) continue;
        *(char **)agentc_vec_push(paths, sizeof(char *)) = agentc_strdup(full);
        *(size_t *)agentc_vec_push(lens, sizeof(size_t)) = len;
        *(char **)agentc_vec_push(texts, sizeof(char *)) = text;
    }
}

AgcContextFiles *agentc_context_files_load(const char *cwd, bool trusted) {
    AgcVec paths = { 0 }, lens = { 0 }, texts = { 0 };

    char cfg[PATH_MAX_];
    if (agentc_config_home(cfg, sizeof cfg)) add_context_dir(&paths, &lens, &texts, cfg);

    if (trusted && cwd && cwd[0]) {
        AgcVec dirs = { 0 };
        size_t n = agentc_strlen(cwd);
        for (size_t i = 0; i < n; i++) {
            if (cwd[i] != '/' && i + 1 != n) continue;
            size_t end = (cwd[i] == '/') ? i : i + 1;
            if (end == 0) continue;
            *(char **)agentc_vec_push(&dirs, sizeof(char *)) = agentc_strdup_len(cwd, end);
        }
        for (size_t i = 0; i < dirs.len; i++)
            add_context_dir(&paths, &lens, &texts, ((char **)dirs.p)[i]);
        for (size_t i = 0; i < dirs.len; i++) agentc_free(((char **)dirs.p)[i]);
        agentc_vec_free(&dirs);
    }

    AgcContextFiles *cf = agentc_alloc(sizeof *cf);
    cf->n = paths.len;
    if (cf->n) {
        cf->paths = agentc_alloc(cf->n * sizeof(char *));
        cf->lens = agentc_alloc(cf->n * sizeof(size_t));
        cf->texts = agentc_alloc(cf->n * sizeof(char *));
        agentc_memcpy(cf->paths, paths.p, cf->n * sizeof(char *));
        agentc_memcpy(cf->lens, lens.p, cf->n * sizeof(size_t));
        agentc_memcpy(cf->texts, texts.p, cf->n * sizeof(char *));
    }
    agentc_vec_free(&paths);
    agentc_vec_free(&lens);
    agentc_vec_free(&texts);
    return cf;
}

void agentc_context_files_free(AgcContextFiles *cf) {
    if (!cf) return;
    for (size_t i = 0; i < cf->n; i++) {
        agentc_free(cf->paths[i]);
        agentc_free(cf->texts[i]);
    }
    agentc_free(cf->paths);
    agentc_free(cf->lens);
    agentc_free(cf->texts);
    agentc_free(cf);
}

/* ---------------------------------------------------------------- skills */

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = agentc_strlen(s), fl = agentc_strlen(suffix);
    return sl >= fl && agentc_memeq(s + sl - fl, suffix, fl);
}

static const char *base_name(const char *path, size_t *len) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') b = p + 1;
    if (len) *len = agentc_strlen(b);
    return b;
}

static void skill_add(AgcVec *v, const char *file, const char *fallback, size_t fallback_len) {
    size_t len = 0;
    char *text = agentc_read_file_owned(file, &len);
    if (!text) return;
    Fm fm;
    fm_parse(text, len, &fm);
    const char *name = fm.name ? fm.name : fallback;
    size_t name_len = fm.name ? fm.name_len : fallback_len;
    char *nm = agentc_strdup_len(name, name_len);
    if (!name_len) {
        agentc_free(nm);
        nm = agentc_strdup("skill");
    }
    bool valid = agentc_strlen(nm) <= 64;
    for (const char *p = nm; valid && *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
            valid = false;
    if (!valid)
        agentc_logf(2,
                "warning: skill %s has an invalid name (want <=64 chars of [a-z0-9-]); "
                "loading anyway",
                nm);
    char *desc =
        agentc_strdup_len(fm.description ? fm.description : "", fm.description ? fm.desc_len : 0);

    AgcSkill *skills = v->p;
    for (size_t i = 0; i < v->len; i++) {
        if (agentc_streq(skills[i].name, nm)) {
            agentc_free(skills[i].description);
            agentc_free(skills[i].path);
            skills[i].description = desc;
            skills[i].path = agentc_strdup(file);
            agentc_free(nm);
            agentc_free(text);
            return;
        }
    }
    AgcSkill *s = agentc_vec_push(v, sizeof(AgcSkill));
    s->name = nm;
    s->description = desc;
    s->path = agentc_strdup(file);
    agentc_free(text);
}

typedef struct {
    AgcVec *v;
    int depth;
} ScanCtx;

static void scan_skills(AgcVec *v, const char *dir, int depth);

static int scan_skills_cb(void *ud, const char *name, const char *full, bool is_dir) {
    ScanCtx *ctx = ud;
    if (is_dir) {
        char sm[PATH_MAX_];
        if (agentc_path_join(sm, sizeof sm, full, "SKILL.md") && agentc_path_is_file(sm)) {
            size_t blen = 0;
            const char *base = base_name(full, &blen);
            skill_add(ctx->v, sm, base, blen);
        } else if (ctx->depth < MAX_SCAN_DEPTH) {
            scan_skills(ctx->v, full, ctx->depth + 1);
        }
        return 0;
    }
    if (ends_with(name, ".md")) {
        size_t nlen = agentc_strlen(name);
        skill_add(ctx->v, full, name, nlen >= 3 ? nlen - 3 : nlen);
    }
    return 0;
}

static void scan_skills(AgcVec *v, const char *dir, int depth) {
    if (!agentc_path_is_dir(dir)) return;
    ScanCtx ctx = { v, depth };
    (void)agentc_dir_scan(dir, scan_skills_cb, &ctx);
}

static void sort_skills(AgcSkill *a, size_t n) {
    for (size_t i = 1; i < n; i++) {
        AgcSkill key = a[i];
        size_t j = i;
        while (j > 0) {
            size_t k = 0;
            while (a[j - 1].name[k] && a[j - 1].name[k] == key.name[k]) k++;
            if ((u8)a[j - 1].name[k] <= (u8)key.name[k]) break;
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

size_t agentc_skills_load_extra(const char *cwd, bool trusted,
                                const char *const *extra_roots, size_t nextra,
                                AgcSkill **out, size_t max) {
    if (out) *out = NULL;
    AgcVec v = { 0 };
    char cfg[PATH_MAX_];
    if (agentc_config_home(cfg, sizeof cfg)) {
        char dir[PATH_MAX_];
        if (agentc_path_join(dir, sizeof dir, cfg, "skills")) scan_skills(&v, dir, 0);
    }
    if (trusted && cwd && cwd[0]) {
        char dir[PATH_MAX_];
        if (agentc_path_join(dir, sizeof dir, cwd, ".agentc/skills")) scan_skills(&v, dir, 0);
    }
    /* resources_discover roots: validated by src/app/project.c before they
     * arrive here; a later root replaces an earlier skill of the same name. */
    for (size_t i = 0; i < nextra; i++)
        if (extra_roots && extra_roots[i] && extra_roots[i][0])
            scan_skills(&v, extra_roots[i], 0);
    size_t n = v.len;
    if (max && n > max) n = max;
    if (n == 0) {
        for (size_t i = 0; i < v.len; i++) {
            AgcSkill *s = &((AgcSkill *)v.p)[i];
            agentc_free(s->name);
            agentc_free(s->description);
            agentc_free(s->path);
        }
        agentc_vec_free(&v);
        return 0;
    }
    sort_skills(v.p, v.len);
    AgcSkill *arr = agentc_alloc(n * sizeof(AgcSkill));
    agentc_memcpy(arr, v.p, n * sizeof(AgcSkill));
    for (size_t i = n; i < v.len; i++) {
        agentc_free(((AgcSkill *)v.p)[i].name);
        agentc_free(((AgcSkill *)v.p)[i].description);
        agentc_free(((AgcSkill *)v.p)[i].path);
    }
    agentc_vec_free(&v);
    if (out) *out = arr;
    return n;
}

size_t agentc_skills_load(const char *cwd, bool trusted, AgcSkill **out, size_t max) {
    return agentc_skills_load_extra(cwd, trusted, NULL, 0, out, max);
}

void agentc_skills_free(AgcSkill *skills, size_t n) {
    if (!skills) return;
    for (size_t i = 0; i < n; i++) {
        agentc_free(skills[i].name);
        agentc_free(skills[i].description);
        agentc_free(skills[i].path);
    }
    agentc_free(skills);
}

/* --------------------------------------------------------------- templates */

static void template_add(AgcVec *v, const char *file, const char *name, size_t name_len) {
    char *nm = agentc_strdup_len(name, name_len);
    if (!agentc_prompt_name_valid(nm)) {
        agentc_logf(2,
                "warning: prompt template %s has an invalid name (want <=%d chars of "
                "[a-z0-9._:-]); skipping",
                file, AGENTC_PROMPT_NAME_MAX);
        agentc_free(nm);
        return;
    }
    size_t len = 0;
    char *text = agentc_read_file_owned(file, &len);
    if (!text) {
        agentc_free(nm);
        return;
    }
    Fm fm;
    fm_parse(text, len, &fm);

    char *description;
    if (fm.description && fm.desc_len) {
        description = agentc_strdup_len(fm.description, fm.desc_len);
    } else {
        const char *b = fm.body;
        size_t bl = fm.body_len;
        size_t ls = 0;
        while (ls < bl && (b[ls] == ' ' || b[ls] == '\t' || b[ls] == '\r' || b[ls] == '\n')) ls++;
        size_t le = ls;
        while (le < bl && b[le] != '\n') le++;
        while (le > ls && (b[le - 1] == ' ' || b[le - 1] == '\t' || b[le - 1] == '\r')) le--;
        description = agentc_strdup_len(b + ls, le - ls);
        if (agentc_strlen(description) > 60) {
            char *t = agentc_alloc(68);
            agentc_memcpy(t, description, 60);
            agentc_memcpy(t + 60, "...", 3);
            t[63] = 0;
            agentc_free(description);
            description = t;
        }
    }

    char *hint = agentc_strdup_len(fm.hint ? fm.hint : "", fm.hint ? fm.hint_len : 0);
    AgcPromptTemplate *ts = v->p;
    for (size_t i = 0; i < v->len; i++) {
        if (agentc_streq(ts[i].name, nm)) {
            agentc_free(ts[i].description);
            agentc_free(ts[i].hint);
            agentc_free(ts[i].path);
            ts[i].description = description;
            ts[i].hint = hint;
            ts[i].path = agentc_strdup(file);
            agentc_free(nm);
            agentc_free(text);
            return;
        }
    }
    AgcPromptTemplate *t = agentc_vec_push(v, sizeof(AgcPromptTemplate));
    t->name = nm;
    t->description = description;
    t->hint = hint;
    t->path = agentc_strdup(file);
    agentc_free(text);
}

static int template_cb(void *ud, const char *name, const char *full, bool is_dir) {
    AgcVec *v = ud;
    if (is_dir || !ends_with(name, ".md")) return 0;
    size_t nlen = agentc_strlen(name);
    template_add(v, full, name, nlen >= 3 ? nlen - 3 : nlen);
    return 0;
}

static void sort_templates(AgcPromptTemplate *a, size_t n) {
    for (size_t i = 1; i < n; i++) {
        AgcPromptTemplate key = a[i];
        size_t j = i;
        while (j > 0) {
            size_t k = 0;
            while (a[j - 1].name[k] && a[j - 1].name[k] == key.name[k]) k++;
            if ((u8)a[j - 1].name[k] <= (u8)key.name[k]) break;
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

size_t agentc_templates_load_extra(const char *cwd, bool trusted,
                                   const char *const *extra_roots, size_t nextra,
                                   AgcPromptTemplate **out, size_t max) {
    if (out) *out = NULL;
    AgcVec v = { 0 };
    char cfg[PATH_MAX_];
    if (agentc_config_home(cfg, sizeof cfg)) {
        char dir[PATH_MAX_];
        if (agentc_path_join(dir, sizeof dir, cfg, "prompts")) (void)agentc_dir_scan(dir, template_cb, &v);
    }
    if (trusted && cwd && cwd[0]) {
        char dir[PATH_MAX_];
        if (agentc_path_join(dir, sizeof dir, cwd, ".agentc/prompts"))
            (void)agentc_dir_scan(dir, template_cb, &v);
    }
    /* resources_discover roots, already validated by src/app/project.c */
    for (size_t i = 0; i < nextra; i++)
        if (extra_roots && extra_roots[i] && extra_roots[i][0])
            (void)agentc_dir_scan(extra_roots[i], template_cb, &v);
    if (max == 0) max = AGENTC_TEMPLATES_MAX;
    size_t n = v.len;
    if (n > max) n = max;
    if (n == 0) {
        for (size_t i = 0; i < v.len; i++) {
            agentc_free(((AgcPromptTemplate *)v.p)[i].name);
            agentc_free(((AgcPromptTemplate *)v.p)[i].description);
            agentc_free(((AgcPromptTemplate *)v.p)[i].hint);
            agentc_free(((AgcPromptTemplate *)v.p)[i].path);
        }
        agentc_vec_free(&v);
        return 0;
    }
    sort_templates(v.p, v.len);
    AgcPromptTemplate *arr = agentc_alloc(n * sizeof(AgcPromptTemplate));
    agentc_memcpy(arr, v.p, n * sizeof(AgcPromptTemplate));
    for (size_t i = n; i < v.len; i++) {
        agentc_free(((AgcPromptTemplate *)v.p)[i].name);
        agentc_free(((AgcPromptTemplate *)v.p)[i].description);
        agentc_free(((AgcPromptTemplate *)v.p)[i].hint);
        agentc_free(((AgcPromptTemplate *)v.p)[i].path);
    }
    agentc_vec_free(&v);
    if (out) *out = arr;
    return n;
}

size_t agentc_templates_load(const char *cwd, bool trusted, AgcPromptTemplate **out, size_t max) {
    return agentc_templates_load_extra(cwd, trusted, NULL, 0, out, max);
}

void agentc_templates_free(AgcPromptTemplate *t, size_t n) {
    if (!t) return;
    for (size_t i = 0; i < n; i++) {
        agentc_free(t[i].name);
        agentc_free(t[i].description);
        agentc_free(t[i].hint);
        agentc_free(t[i].path);
    }
    agentc_free(t);
}

/* ----------------------------------------------------------------- themes */

static void theme_add(AgcVec *v, const char *file, const char *name, size_t name_len) {
    char *nm = agentc_strdup_len(name, name_len);
    if (!theme_name_valid(nm)) {
        agentc_logf(2,
                "warning: theme %s has an invalid name (want <=%d chars of "
                "[A-Za-z0-9._-]); skipping",
                file, AGENTC_THEME_NAME_MAX);
        agentc_free(nm);
        return;
    }
    AgcThemeEntry *ts = v->p;
    for (size_t i = 0; i < v->len; i++) {
        if (agentc_streq(ts[i].name, nm)) {
            agentc_free(ts[i].path);
            ts[i].path = agentc_strdup(file);
            agentc_free(nm);
            return;
        }
    }
    AgcThemeEntry *t = agentc_vec_push(v, sizeof(AgcThemeEntry));
    t->name = nm;
    t->path = agentc_strdup(file);
}

static int theme_cb(void *ud, const char *name, const char *full, bool is_dir) {
    AgcVec *v = ud;
    if (is_dir || !ends_with(name, ".jsonc")) return 0;
    size_t nlen = agentc_strlen(name);
    theme_add(v, full, name, nlen >= 6 ? nlen - 6 : nlen);
    return 0;
}

static void sort_themes(AgcThemeEntry *a, size_t n) {
    for (size_t i = 1; i < n; i++) {
        AgcThemeEntry key = a[i];
        size_t j = i;
        while (j > 0) {
            size_t k = 0;
            while (a[j - 1].name[k] && a[j - 1].name[k] == key.name[k]) k++;
            if ((u8)a[j - 1].name[k] <= (u8)key.name[k]) break;
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

size_t agentc_themes_load(const char *cwd, bool trusted,
                          const char *const *extra_roots, size_t nextra,
                          AgcThemeEntry **out, size_t max) {
    if (out) *out = NULL;
    AgcVec v = { 0 };
    char cfg[PATH_MAX_];
    if (agentc_config_home(cfg, sizeof cfg)) {
        char dir[PATH_MAX_];
        if (agentc_path_join(dir, sizeof dir, cfg, "themes")) (void)agentc_dir_scan(dir, theme_cb, &v);
    }
    if (trusted && cwd && cwd[0]) {
        char dir[PATH_MAX_];
        if (agentc_path_join(dir, sizeof dir, cwd, ".agentc/themes"))
            (void)agentc_dir_scan(dir, theme_cb, &v);
    }
    /* resources_discover roots, already validated by src/app/project.c */
    for (size_t i = 0; i < nextra; i++)
        if (extra_roots && extra_roots[i] && extra_roots[i][0])
            (void)agentc_dir_scan(extra_roots[i], theme_cb, &v);
    if (max == 0) max = AGENTC_THEMES_MAX;
    size_t n = v.len;
    if (n > max) n = max;
    if (n == 0) {
        for (size_t i = 0; i < v.len; i++) {
            agentc_free(((AgcThemeEntry *)v.p)[i].name);
            agentc_free(((AgcThemeEntry *)v.p)[i].path);
        }
        agentc_vec_free(&v);
        return 0;
    }
    sort_themes(v.p, v.len);
    AgcThemeEntry *arr = agentc_alloc(n * sizeof(AgcThemeEntry));
    agentc_memcpy(arr, v.p, n * sizeof(AgcThemeEntry));
    for (size_t i = n; i < v.len; i++) {
        agentc_free(((AgcThemeEntry *)v.p)[i].name);
        agentc_free(((AgcThemeEntry *)v.p)[i].path);
    }
    agentc_vec_free(&v);
    if (out) *out = arr;
    return n;
}

void agentc_themes_free(AgcThemeEntry *t, size_t n) {
    if (!t) return;
    for (size_t i = 0; i < n; i++) {
        agentc_free(t[i].name);
        agentc_free(t[i].path);
    }
    agentc_free(t);
}

/* Named themes contributed by resources_discover are most specific, so they
 * are consulted before the config themes dir. */
int agentc_theme_path(const char *name, char *buf, size_t cap) {
    if (!theme_name_valid(name) || !buf || cap == 0) return -22;
    char file[AGENTC_THEME_NAME_MAX + 8];
    agentc_snprintf(file, sizeof file, "%s.jsonc", name);
    for (size_t i = g_theme_roots.n; i > 0; i--) {
        if (!agentc_path_join(buf, cap, g_theme_roots.dirs[i - 1], file)) continue;
        if (agentc_path_is_file(buf)) return 0;
    }
    char cfg[PATH_MAX_];
    if (agentc_config_home(cfg, sizeof cfg)) {
        char dir[PATH_MAX_];
        if (agentc_path_join(dir, sizeof dir, cfg, "themes") &&
            agentc_path_join(buf, cap, dir, file) && agentc_path_is_file(buf))
            return 0;
    }
    return -2;   /* -ENOENT */
}

/* ------------------------------------------------------------- expansion */

typedef struct {
    size_t off;
    size_t n;
} ArgSlice;

static size_t split_args(const char *s, AgcBuf *flat, ArgSlice *args, size_t max) {
    size_t n = 0;
    if (!s) return 0;
    bool inq = false;
    char q = 0;
    bool have = false;
    size_t start = 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (inq) {
            if (c == q) {
                inq = false;
            } else {
                if (!have) {
                    have = true;
                    start = flat->len;
                }
                agentc_buf_byte(flat, (u8)c);
            }
        } else if (c == '"' || c == '\'') {
            inq = true;
            q = c;
            if (!have) {
                have = true;
                start = flat->len;
            }
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (have && flat->len > start && n < max) {
                args[n].off = start;
                args[n].n = flat->len - start;
                n++;
            }
            have = false;
        } else {
            if (!have) {
                have = true;
                start = flat->len;
            }
            agentc_buf_byte(flat, (u8)c);
        }
    }
    if (have && flat->len > start && n < max) {
        args[n].off = start;
        args[n].n = flat->len - start;
        n++;
    }
    return n;
}

static void append_all(AgcBuf *out, const AgcBuf *flat, const ArgSlice *args, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (i) agentc_buf_byte(out, ' ');
        agentc_buf_push(out, flat->p + args[i].off, args[i].n);
    }
}

static bool digits(const char *p, size_t n, size_t *out) {
    if (n == 0) return false;
    size_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return false;
        v = v * 10 + (size_t)(p[i] - '0');
    }
    *out = v;
    return true;
}

char *agentc_template_expand(const AgcPromptTemplate *t, const char *args) {
    if (!t || !t->path) return agentc_strdup_len("", 0);
    size_t len = 0;
    char *text = agentc_read_file_owned(t->path, &len);
    if (!text) return agentc_strdup_len("", 0);
    Fm fm;
    fm_parse(text, len, &fm);
    const char *body = fm.body;
    size_t blen = fm.body_len;

    AgcBuf flat = { 0 };
    ArgSlice slices[64];
    size_t nargs = split_args(args ? args : "", &flat, slices, 64);

    AgcBuf out = { 0 };
    size_t i = 0;
    while (i < blen) {
        char c = body[i];
        if (c != '$') {
            agentc_buf_byte(&out, (u8)c);
            i++;
            continue;
        }
        size_t j = i + 1;
        if (j < blen && body[j] == '{') {
            size_t k = j + 1;
            while (k < blen && body[k] != '}') k++;
            if (k < blen) {
                const char *in = body + j + 1;
                size_t il = k - (j + 1);
                bool handled = false;
                /* ${T:-default} */
                size_t colon = (size_t)-1;
                for (size_t q = 0; q < il; q++)
                    if (in[q] == ':') {
                        colon = q;
                        break;
                    }
                if (colon != (size_t)-1 && colon + 1 < il && in[colon + 1] == '-') {
                    const char *tgt = in;
                    size_t tl = colon;
                    const char *def = in + colon + 2;
                    size_t dl = il - colon - 2;
                    if (tl == 1 && tgt[0] == '@') {
                        if (nargs) append_all(&out, &flat, slices, nargs);
                        else agentc_buf_push(&out, def, dl);
                        handled = true;
                    } else if (tl == 9 && agentc_memeq(tgt, "ARGUMENTS", 9)) {
                        if (nargs) append_all(&out, &flat, slices, nargs);
                        else agentc_buf_push(&out, def, dl);
                        handled = true;
                    } else {
                        size_t idx = 0;
                        if (digits(tgt, tl, &idx) && idx >= 1) {
                            if (idx <= nargs)
                                agentc_buf_push(&out, flat.p + slices[idx - 1].off,
                                            slices[idx - 1].n);
                            else
                                agentc_buf_push(&out, def, dl);
                            handled = true;
                        }
                    }
                }
                /* ${@:N} and ${@:N:L} */
                if (!handled && il >= 3 && in[0] == '@' && in[1] == ':') {
                    size_t q = 2;
                    size_t s1 = 0, s2 = 0;
                    size_t d1 = 0;
                    while (q + d1 < il && in[q + d1] != ':') d1++;
                    bool ok1 = digits(in + q, d1, &s1);
                    size_t off = q + d1;
                    bool ok2 = false;
                    if (ok1 && off < il && in[off] == ':') {
                        ok2 = digits(in + off + 1, il - off - 1, &s2);
                    }
                    if (ok1) {
                        size_t start = s1 > 0 ? s1 - 1 : 0;
                        if (start < nargs) {
                            size_t end = nargs;
                            if (ok2 && start + s2 < end) end = start + s2;
                            for (size_t a = start; a < end; a++) {
                                if (a > start) agentc_buf_byte(&out, ' ');
                                agentc_buf_push(&out, flat.p + slices[a].off, slices[a].n);
                            }
                        }
                        handled = true;
                    }
                }
                if (handled) {
                    i = k + 1;
                    continue;
                }
            }
            agentc_buf_byte(&out, '$');
            i++;
            continue;
        }
        if (j < blen && body[j] >= '0' && body[j] <= '9') {
            size_t k = j, v = 0;
            while (k < blen && body[k] >= '0' && body[k] <= '9') {
                v = v * 10 + (size_t)(body[k] - '0');
                k++;
            }
            if (v >= 1 && v <= nargs)
                agentc_buf_push(&out, flat.p + slices[v - 1].off, slices[v - 1].n);
            i = k;
            continue;
        }
        if (j < blen && body[j] == '@') {
            append_all(&out, &flat, slices, nargs);
            i = j + 1;
            continue;
        }
        if (blen - j >= 9 && agentc_memeq(body + j, "ARGUMENTS", 9)) {
            append_all(&out, &flat, slices, nargs);
            i = j + 9;
            continue;
        }
        agentc_buf_byte(&out, '$');
        i++;
    }
    agentc_free(text);
    agentc_buf_free(&flat);
    if (!out.p) return agentc_strdup_len("", 0);
    return (char *)out.p;
}
