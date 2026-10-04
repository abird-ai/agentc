/* prompt.c — system prompt assembly and the builtin-context extension.
 *
 * The core renders four sections in their historical order: preamble (identity),
 * tools, rules, environment (cwd/platform). The builtin-context extension then
 * contributes the project-instructions and skills blocks through the ordinary
 * section registry, so the prompt is assembled in one place and an extension
 * can add its own sections after them.
 *
 * `--system` hard-replaces the whole render (the app passes a->system and the
 * agent never calls agentc_prompt_build); extension sections are skipped, which
 * matches pi's forceSystemPrompt.
 */
#include "agent.h"
#include "plat.h"
#include "config.h"
#include "ext.h"
#include "core/prompt.h"

static void xml_escape(AgcBuf *b, const char *s, size_t n) {
    if (!s) return;
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': agentc_buf_cstr(b, "&amp;"); break;
        case '<': agentc_buf_cstr(b, "&lt;"); break;
        case '>': agentc_buf_cstr(b, "&gt;"); break;
        case '"': agentc_buf_cstr(b, "&quot;"); break;
        case '\'': agentc_buf_cstr(b, "&apos;"); break;
        default: agentc_buf_byte(b, (u8)s[i]); break;
        }
    }
}

void agentc_prompt_add_project_context(AgcBuf *sys, const AgcContextFiles *cf) {
    if (!sys || !cf || cf->n == 0) return;
    for (size_t i = 0; i < cf->n; i++) {
        agentc_buf_cstr(sys, "\n<project_instructions path=\"");
        xml_escape(sys, cf->paths[i], agentc_strlen(cf->paths[i]));
        agentc_buf_cstr(sys, "\">\n");
        agentc_buf_push(sys, cf->texts[i], cf->lens[i]);
        agentc_buf_cstr(sys, "\n</project_instructions>\n");
    }
}

void agentc_prompt_add_skills(AgcBuf *sys, const AgcSkill *skills, size_t n) {
    if (!sys || n == 0) return;
    agentc_buf_cstr(sys,
                "\nThe following skills provide specialized instructions for specific tasks."
                "\nUse the read tool to load a skill's file when the task matches its description."
                "\n\n<available_skills>\n");
    for (size_t i = 0; i < n; i++) {
        const AgcSkill *s = &skills[i];
        agentc_buf_cstr(sys, "  <skill>\n    <name>");
        xml_escape(sys, s->name, agentc_strlen(s->name));
        agentc_buf_cstr(sys, "</name>\n    <description>");
        xml_escape(sys, s->description, agentc_strlen(s->description));
        agentc_buf_cstr(sys, "</description>\n    <location>");
        xml_escape(sys, s->path, agentc_strlen(s->path));
        agentc_buf_cstr(sys, "</location>\n  </skill>\n");
    }
    agentc_buf_cstr(sys, "</available_skills>\n");
}

/* The four core sections, unchanged in order and text. */
static void prompt_core(AgcBuf *b, const AgcTool *tools, size_t ntools) {
    char cwd[4096];
    if (os_getcwd(cwd, sizeof cwd) < 0) agentc_snprintf(cwd, sizeof cwd, "?");

    agentc_buf_cstr(b,
                "You are agentc, a coding agent working directly in the user's "
                "environment.\n"
                "Use the provided tools to inspect and modify files. Keep changes "
                "focused and verify your work.\n");

    if (ntools) {
        agentc_buf_cstr(b, "\n# Tools\n");
        for (size_t i = 0; i < ntools; i++) {
            const AgcTool *t = &tools[i];
            if (t->flags & AGENTC_TOOL_HIDDEN) continue;
            agentc_buf_printf(b, "- %s: %s\n", t->name, t->desc ? t->desc : t->label);
        }
    }

    agentc_buf_cstr(b,
                "\n# Rules\n"
                "- Read a file before editing it; make edits with exact, unique "
                "old text.\n"
                "- Prefer small, targeted commands; report failures instead of "
                "guessing.\n"
                "- Never invent tool output. If a command fails, say so and "
                "explain the next step.\n");

    agentc_buf_printf(b, "\n# Environment\n- cwd: %s\n- platform: %s\n", cwd, os_platform());
}

void agentc_ext_prompt_build(AgcBuf *out, const AgcTool *tools, size_t ntools) {
    if (!out) return;
    prompt_core(out, tools, ntools);
    agentc_ext_render_sections(out);
}

char *agentc_prompt_build(const AgcTool *tools, size_t ntools) {
    AgcBuf b = { 0 };
    agentc_ext_prompt_build(&b, tools, ntools);
    return (char *)b.p;
}

/* ------------------------------------------------------- builtin-context */

static char *g_ctx_cwd;
static bool g_ctx_trusted;

void agentc_builtin_context_set(const char *cwd, bool trusted) {
    agentc_free(g_ctx_cwd);
    g_ctx_cwd = cwd ? agentc_strdup(cwd) : NULL;
    g_ctx_trusted = trusted;
}

static int context_render(const AgcExtHost *host, void *ud, void *out) {
    (void)host;
    (void)ud;
    AgcBuf *b = (AgcBuf *)out;
    AgcContextFiles *cf = agentc_context_files_load(g_ctx_cwd, g_ctx_trusted);
    agentc_prompt_add_project_context(b, cf);
    agentc_context_files_free(cf);
    const char *const *roots = NULL;
    size_t nroots = agentc_resource_skill_roots(&roots);
    AgcSkill *skills = NULL;
    size_t nskills = agentc_skills_load_extra(g_ctx_cwd, g_ctx_trusted, roots, nroots,
                                              &skills, 256);
    agentc_prompt_add_skills(b, skills, nskills);
    agentc_skills_free(skills, nskills);
    return 0;
}

/* TUI `/skill:<name>`: the loaded skill table is the only place that maps a
 * name to a SKILL.md path, so load it, find the record and read its body. */
char *agentc_builtin_context_skill_body(const char *name, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!name || !name[0]) return NULL;
    const char *const *roots = NULL;
    size_t nroots = agentc_resource_skill_roots(&roots);
    AgcSkill *skills = NULL;
    size_t n = agentc_skills_load_extra(g_ctx_cwd, g_ctx_trusted, roots, nroots, &skills, 256);
    const char *path = NULL;
    for (size_t i = 0; i < n; i++) {
        if (agentc_streq(skills[i].name, name)) {
            path = skills[i].path;
            break;
        }
    }
    char *body = path ? agentc_skill_body_read(path, out_len) : NULL;
    agentc_skills_free(skills, n);
    return body;
}

static const AgcExtSection g_context_section = {
    .struct_size = sizeof(AgcExtSection),
    .key = "context",
    .priority = 100,
    .render = context_render,
};

static int context_init(const AgcExtHost *host) {
    host->add_section(&g_context_section);
    return 0;
}

static const AgcExt g_builtin_context = {
    .abi_version = AGENTC_EXT_ABI,
    .struct_size = sizeof(AgcExt),
    .name = "builtin-context",
    .version = "1",
    .order = 0,
    .init = context_init,
};

const AgcExt *agentc_builtin_context_ext(void) { return &g_builtin_context; }
