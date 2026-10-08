/* prompts.c — invocable prompt registry (see prompts.h).
 *
 * Records live in a fixed, capped slot table. Retirement flips `live`; a slot
 * whose record is retired is REUSED in place by the next registration once the
 * table is full, because a full table must still be able to replace a prompt
 * (the MCP prompt commit retires before it registers). Reusing a slot frees the
 * previous record's strings, so pointers returned by agentc_prompts_list() are
 * valid only until the next registration of that slot: callers must copy any
 * string they need to keep (the in-tree TUI menu re-reads the list every
 * frame, so it never spans a registration). `ud` is borrowed from the
 * registrant and never freed here: its lifetime is the registrant's. A
 * registrant that allocates it should treat the slot lifetime as process-
 * bounded, because a reused slot overwrites the previous `ud` without a
 * destructor (the file-template loader's record is the one such case).
 */
#include "agentc.h"
#include "base/limits.h"
#include "config.h"
#include "core/prompt.h"
#include "core/prompts.h"

typedef struct {
    char *name;
    char *description;
    char *hint;
    char *source;
    void *ud;
    AgcPromptExpandFn expand;
    bool live;
} PromptRec;

static PromptRec g_prompts[AGENTC_PROMPTS_MAX];
static size_t g_prompt_count;

static PromptRec *find_live(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < g_prompt_count; i++)
        if (g_prompts[i].live && agentc_streq(g_prompts[i].name, name)) return &g_prompts[i];
    return NULL;
}

int agentc_prompts_register(const char *name, const char *desc, const char *hint,
                            const char *source, void *ud, AgcPromptExpandFn expand) {
    if (!agentc_prompt_name_valid(name)) {
        agentc_logf(2, "prompt: invalid name '%s' (want <=%d chars of [a-z0-9._:-])",
                    name ? name : "", AGENTC_PROMPT_NAME_MAX);
        return -22;
    }
    /* Duplicate the caller's strings BEFORE mutating any slot: a reused slot
     * may be the same record the caller passed a pointer into (the stable-
     * pointer contract), so freeing it first and then strdup'ing the argument
     * would read freed memory. */
    char *n2 = agentc_strdup(name);
    char *d2 = agentc_strdup(desc ? desc : "");
    char *h2 = agentc_strdup(hint ? hint : "");
    char *s2 = agentc_strdup(source ? source : "");
    PromptRec *old = find_live(name);
    PromptRec *r = NULL;
    if (g_prompt_count < AGENTC_PROMPTS_MAX) {
        r = &g_prompts[g_prompt_count++];
    } else {
        /* At the cap. Records are never freed, so a retired (!live) slot can be
         * overwritten in place; this lets a replacement land right after
         * agentc_prompts_remove() retires its predecessor (the MCP prompt
         * commit retires before it registers). Only a table with no dead slot
         * -- and no same-name live record to overwrite -- is out of space. */
        for (size_t i = 0; i < g_prompt_count; i++) {
            if (!g_prompts[i].live) { r = &g_prompts[i]; break; }
        }
        if (!r && old) r = old;
        if (!r) {
            agentc_logf(2, "prompt: registry full (%d); ignoring '%s'",
                        AGENTC_PROMPTS_MAX, name);
            agentc_free(n2);
            agentc_free(d2);
            agentc_free(h2);
            agentc_free(s2);
            return -28;
        }
    }
    /* A reused slot may still own the previous record's strings. Release them;
     * a caller must not hold pointers from agentc_prompts_list across a
     * registration (the in-tree menu re-reads the list every frame). */
    agentc_free(r->name);
    agentc_free(r->description);
    agentc_free(r->hint);
    agentc_free(r->source);
    agentc_memset(r, 0, sizeof *r);
    r->name = n2;
    r->description = d2;
    r->hint = h2;
    r->source = s2;
    r->ud = ud;
    r->expand = expand;
    r->live = true;
    if (old && old != r) old->live = false;   /* later same-name registration wins */
    return 0;
}

bool agentc_prompts_remove(const char *name) {
    PromptRec *r = find_live(name);
    if (!r) return false;
    r->live = false;
    return true;
}

size_t agentc_prompts_clear_source(const char *source) {
    if (!source) source = "";
    size_t n = 0;
    for (size_t i = 0; i < g_prompt_count; i++) {
        PromptRec *r = &g_prompts[i];
        if (r->live && agentc_streq(r->source, source)) {
            r->live = false;
            n++;
        }
    }
    return n;
}

size_t agentc_prompts_list(AgcPromptInfo *out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < g_prompt_count; i++) {
        PromptRec *r = &g_prompts[i];
        if (!r->live) continue;
        if (out && n < max) {
            out[n].name = r->name;
            out[n].description = r->description;
            out[n].hint = r->hint;
        }
        n++;
    }
    return out ? (n < max ? n : max) : n;
}

bool agentc_prompts_has(const char *name) { return find_live(name) != NULL; }

char *agentc_prompts_expand(const char *name, const char *args) {
    PromptRec *r = find_live(name);
    if (!r || !r->expand) return NULL;
    return r->expand(r->ud, args ? args : "");
}

/* ------------------------------------------------------- file templates */

static char *file_template_expand(void *ud, const char *args) {
    return agentc_template_expand((const AgcPromptTemplate *)ud, args);
}

static void file_template_free(AgcPromptTemplate *t) {
    if (!t) return;
    agentc_free(t->name);
    agentc_free(t->description);
    agentc_free(t->hint);
    agentc_free(t->path);
    agentc_free(t);
}

static AgcPromptTemplate *file_template_dup(const AgcPromptTemplate *src) {
    AgcPromptTemplate *d = agentc_alloc(sizeof *d);
    d->name = agentc_strdup(src->name);
    d->description = agentc_strdup(src->description ? src->description : "");
    d->hint = agentc_strdup(src->hint ? src->hint : "");
    d->path = agentc_strdup(src->path);
    return d;
}

size_t agentc_prompts_load_file_templates(const char *cwd, bool trusted) {
    const char *const *roots = NULL;
    size_t nroots = agentc_resource_prompt_roots(&roots);
    AgcPromptTemplate *tpls = NULL;
    size_t n = agentc_templates_load_extra(cwd, trusted, roots, nroots, &tpls,
                                           AGENTC_TEMPLATES_MAX);
    size_t registered = 0;
    for (size_t i = 0; i < n; i++) {
        AgcPromptTemplate *d = file_template_dup(&tpls[i]);
        if (agentc_prompts_register(d->name, d->description, d->hint, "file", d,
                                    file_template_expand) == 0) {
            registered++;
        } else {
            file_template_free(d);
        }
    }
    agentc_templates_free(tpls, n);
    return registered;
}
