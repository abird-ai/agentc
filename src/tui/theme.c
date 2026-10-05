/* theme.c — built-in dark/light palettes, jsonc overrides, 24-bit -> 256/16
 * downgrade. Config is optional: a missing or malformed file keeps defaults.
 */
#include "render.h"
#include "config.h"
#include "core/prompt.h"
#include "plat.h"

/* internal config helper (src/core/config.c); not part of the frozen API */
bool agentc_path_is_file(const char *path);

/* Catppuccin-ish defaults. */
static const u32 dark_colors[TH_COUNT] = {
    [TH_FG] = 0xCDD6F4,
    [TH_BG] = 0x1E1E2E,
    [TH_MUTED] = 0x7F849C,
    [TH_ACCENT] = 0x89B4FA,
    [TH_OK] = 0xA6E3A1,
    [TH_WARN] = 0xF9E2AF,
    [TH_ERR] = 0xF38BA8,
    [TH_USER] = 0x89DCEB,
    [TH_ASSISTANT] = 0xCDD6F4,
    [TH_THINKING] = 0x6C7086,
    [TH_TOOL] = 0xCBA6F7,
    [TH_DIFF_ADD] = 0xA6E3A1,
    [TH_DIFF_DEL] = 0xF38BA8,
    [TH_CODE] = 0xF5C2E7,
    /* pi's toolSuccessBg/toolErrorBg/toolPendingBg: dark low-saturation tints
     * (10-20% HSL); body/header/diff foregrounds keep >=5.7 contrast. */
    [TH_TOOL_OK_BG] = 0x283228,
    [TH_TOOL_ERR_BG] = 0x3C2828,
    [TH_TOOL_BG] = 0x282832,
};

static const u32 light_colors[TH_COUNT] = {
    [TH_FG] = 0x1E1E2E,
    [TH_BG] = 0xEFF1F5,
    [TH_MUTED] = 0x6C6F85,
    [TH_ACCENT] = 0x1E66F5,
    [TH_OK] = 0x40A02B,
    [TH_WARN] = 0xDF8E1D,
    [TH_ERR] = 0xD20F39,
    [TH_USER] = 0x04A5E5,
    [TH_ASSISTANT] = 0x1E1E2E,
    [TH_THINKING] = 0x8C8FA1,
    [TH_TOOL] = 0x8839EF,
    [TH_DIFF_ADD] = 0x40A02B,
    [TH_DIFF_DEL] = 0xD20F39,
    [TH_CODE] = 0xEA76CB,
    /* pi's light tool tints. */
    [TH_TOOL_OK_BG] = 0xE8F0E8,
    [TH_TOOL_ERR_BG] = 0xF0E8E8,
    [TH_TOOL_BG] = 0xE8E8F0,
};

static const char *slot_names[TH_COUNT] = {
    "fg", "bg", "muted", "accent", "ok", "warn", "err",
    "user", "assistant", "thinking", "tool", "diff_add", "diff_del", "code",
    "tool_ok_bg", "tool_err_bg", "tool_bg",
};

bool theme_parse_color(const char *s, u32 *out) {
    if (!s) return false;
    if (s[0] == '#') s++;
    u32 v = 0;
    int n = 0;
    for (; s[n] && n < 8; n++) {
        char c = s[n];
        u32 d;
        if (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    if (n == 3) {
        u32 r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
        v = (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
    } else if (n != 6) {
        return false;
    }
    *out = v & 0xFFFFFF;
    return true;
}

int theme_detect_mode(void) {
    const char *ct = agentc_env_get("COLORTERM");
    if (ct && (agentc_str_str(ct, "truecolor") || agentc_str_str(ct, "24bit"))) return THEME_TRUE;
    const char *term = agentc_env_get("TERM");
    if (term && agentc_str_str(term, "256")) return THEME_256;
    if (ct && agentc_str_str(ct, "color")) return THEME_256;
    return THEME_16;
}

static char *read_text_file(const char *path) {
    /* Capped reader: a huge theme.jsonc must not be read into memory unbounded
     * (agentc_read_file_owned enforces the same 16 MiB cap as config). */
    return agentc_read_file_owned(path, NULL);
}

static const char *config_path(char *buf, size_t cap) {
    const char *xdg = agentc_env_get("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        if (agentc_snprintf(buf, cap, "%s/agentc/theme.jsonc", xdg) > 0) return buf;
        return NULL;
    }
    const char *home = agentc_env_get("HOME");
    if (home && home[0]) {
        if (agentc_snprintf(buf, cap, "%s/.config/agentc/theme.jsonc", home) > 0) return buf;
    }
    return NULL;
}

/* One parsed theme file: the optional base selector plus every slot actually
 * present. Separating base from slots is what makes slot-by-slot precedence
 * work when the most specific file also switches the palette. */
typedef struct {
    int base;                    /* -1 absent, 0 light, 1 dark */
    bool has[TH_COUNT];
    u32 rgb[TH_COUNT];
} ThemeFile;

/* Parse one JSONC theme file. Returns false when it is missing, unreadable or
 * not a JSON object. */
static bool theme_load_file(const char *path, ThemeFile *out) {
    agentc_memset(out, 0, sizeof *out);
    out->base = -1;
    if (!path) return false;
    char *text = read_text_file(path);
    if (!text) return false;
    AgcJsonArena *ja = agentc_json_arena_new(0);
    AgcJson *root = agentc_json_parse_in(ja, text, agentc_strlen(text));
    bool ok = agentc_json_type(root) == AGENTC_JSON_OBJ;
    if (ok) {
        const char *base = agentc_json_get_str(root, "base");
        if (base && agentc_streq(base, "light")) out->base = 0;
        else if (base && agentc_streq(base, "dark")) out->base = 1;
        for (int i = 0; i < TH_COUNT; i++) {
            const char *v = agentc_json_get_str(root, slot_names[i]);
            u32 c;
            if (v && theme_parse_color(v, &c)) {
                out->has[i] = true;
                out->rgb[i] = c;
            }
        }
    }
    agentc_json_arena_free(ja);
    agentc_free(text);
    return ok;
}

static void theme_set_base(Theme *t, int dark) {
    t->dark = dark ? 1 : 0;
    agentc_memcpy(t->rgb, dark ? dark_colors : light_colors, sizeof t->rgb);
}

static void theme_apply_slots(Theme *t, const ThemeFile *f) {
    for (int i = 0; i < TH_COUNT; i++)
        if (f->has[i]) t->rgb[i] = f->rgb[i];
}

static void load_config(Theme *t) {
    char path[4096];
    const char *p = config_path(path, sizeof path);
    if (!p) return;
    ThemeFile f;
    if (!theme_load_file(p, &f)) return;
    if (f.base >= 0) theme_set_base(t, f.base);
    theme_apply_slots(t, &f);
}

void theme_init(Theme *t, int dark, bool do_load) {
    t->dark = dark ? 1 : 0;
    t->force_bg = false;
    agentc_memcpy(t->rgb, dark ? dark_colors : light_colors, sizeof t->rgb);
    t->mode = theme_detect_mode();
    if (do_load) load_config(t);
}

/* $COLORFGBG is "fg;bg"; the last field is the background index. 0-6 and 8
 * are dark, 7 and 9-15 are light; anything unparseable falls back to dark.
 * A digit run is parsed with a bounded accumulator: three digits already
 * exceed every valid index, so any further digit only marks the value as out
 * of range instead of letting a long run overflow the signed int. */
static int system_dark(void) {
    const char *v = agentc_env_get("COLORFGBG");
    if (!v || !v[0]) return 1;
    int last = -1;
    for (const char *p = v; *p;) {
        if (*p < '0' || *p > '9') {
            p++;
            continue;
        }
        int n = 0;
        int digits = 0;
        bool over = false;
        while (*p >= '0' && *p <= '9') {
            if (digits == 3) {
                over = true;
            } else {
                n = n * 10 + (*p - '0');
                digits++;
            }
            p++;
        }
        last = over ? 16 : n;
    }
    if (last < 0 || last > 15) return 1;
    return !(last == 7 || last >= 9);
}

static bool g_unknown_theme_logged;

/* ------------------------------------------------------- project themes */
/* A trusted project's <cwd>/.agentc/themes is loaded by agentc_themes_load(),
 * but core's agentc_theme_path() only name-resolves the config dir and the
 * resources_discover roots. The TUI installs the resolved project root here
 * so `/theme <name>` can find it; untrusted clears it. */
static char *g_project_theme_cwd;
static bool g_project_theme_trusted;

void theme_set_project_root(const char *cwd, bool trusted) {
    agentc_free(g_project_theme_cwd);
    g_project_theme_cwd = NULL;
    g_project_theme_trusted = false;
    if (trusted && cwd && cwd[0]) {
        g_project_theme_cwd = agentc_strdup(cwd);
        g_project_theme_trusted = true;
    }
}

/* Strict theme stem, kept identical to the loader's validation
 * (core/resources.c): [A-Za-z0-9._-]{1,64}, no "." / "..". */
static bool theme_name_ok(const char *name) {
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

/* Resolve one named file in the loaders' precedence order: the extra
 * resources_discover roots (most specific first), then the trusted project
 * dir, then agentc_theme_path (the config themes dir). */
static bool theme_resolve_named(const char *name, char *path, size_t cap) {
    if (!theme_name_ok(name)) return false;
    char file[AGENTC_THEME_NAME_MAX + 8];
    agentc_snprintf(file, sizeof file, "%s.jsonc", name);
    const char *const *roots = NULL;
    size_t nroots = agentc_resource_theme_roots(&roots);
    for (size_t i = nroots; i > 0; i--) {
        if (!roots || !roots[i - 1] || !roots[i - 1][0]) continue;
        if (!agentc_path_join(path, cap, roots[i - 1], file)) continue;
        if (agentc_path_is_file(path)) return true;
    }
    if (g_project_theme_trusted && g_project_theme_cwd) {
        char dir[4096];
        if (agentc_path_join(dir, sizeof dir, g_project_theme_cwd, ".agentc/themes") &&
            agentc_path_join(path, cap, dir, file) && agentc_path_is_file(path))
            return true;
    }
    return agentc_theme_path(name, path, cap) == 0;
}

bool theme_apply_named(Theme *t, const char *name) {
    if (!t || !name || !name[0]) return false;
    int dark = 1;
    bool have_file = false;
    bool force_bg = false;   /* the fixed light/dark palettes own their bg */
    char path[4096];
    if (agentc_streq(name, "system")) {
        dark = system_dark();
    } else if (agentc_streq(name, "dark")) {
        dark = 1;
        force_bg = true;
    } else if (agentc_streq(name, "light")) {
        dark = 0;
        force_bg = true;
    } else if (theme_resolve_named(name, path, sizeof path)) {
        have_file = true;
    } else {
        if (!g_unknown_theme_logged) {
            g_unknown_theme_logged = true;
            agentc_logf(2, "theme: unknown theme '%s'; keeping the current theme", name);
        }
        return false;
    }
    /* Effective base first (built-in, then config, then the named file), then
     * the slot overrides at their own precedence, most specific last. */
    ThemeFile cfg_file, named_file;
    char cfg[4096];
    const char *cp = config_path(cfg, sizeof cfg);
    bool have_cfg = cp && theme_load_file(cp, &cfg_file);
    bool have_named = have_file && theme_load_file(path, &named_file);
    int base = dark;
    if (have_cfg && cfg_file.base >= 0) base = cfg_file.base;
    if (have_named && named_file.base >= 0) base = named_file.base;
    theme_set_base(t, base);
    if (have_cfg) theme_apply_slots(t, &cfg_file);
    if (have_named) theme_apply_slots(t, &named_file);
    t->force_bg = force_bg;
    return true;
}

void theme_set_mode(Theme *t, int mode) {
    if (mode < THEME_16) mode = THEME_16;
    if (mode > THEME_TRUE) mode = THEME_TRUE;
    t->mode = mode;
}

const char *theme_name(const Theme *t) { return t->dark ? "dark" : "light"; }

/* ------------------------------------------------------------- downgrade */

static int rgb_to_256(u32 c) {
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    /* The 6x6x6 cube's first non-zero level is #5F5F5F, so rounding a dark,
     * near-neutral colour (the base and every muted tool tint) to it would
     * paint a mid-grey band that drowns the foregrounds. Pick the nearest of
     * the 24 greyscale ramp entries instead; everything else keeps the cube. */
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx - mn <= 24 && mx < 96) {
        int best = 232;
        i64 bestd = -1;
        for (int i = 0; i < 24; i++) {
            int v = 8 + 10 * i;
            i64 d = (i64)(r - v) * (r - v) + (i64)(g - v) * (g - v) +
                    (i64)(b - v) * (b - v);
            if (bestd < 0 || d < bestd) { bestd = d; best = 232 + i; }
        }
        return best;
    }
    if (r == g && g == b) {
        if (r < 8) return 16;
        if (r > 248) return 231;
        int v = 232 + (r - 8) / 10;
        return v > 255 ? 255 : v;
    }
    int ri = (r * 5 + 127) / 255;
    int gi = (g * 5 + 127) / 255;
    int bi = (b * 5 + 127) / 255;
    return 16 + 36 * ri + 6 * gi + bi;
}

/* Approximate the 16 ANSI colors; picks the closest entry by squared distance. */
static int rgb_to_16(u32 c) {
    static const u32 pal[16] = {
        0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
        0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
    };
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    int best = 0;
    i64 bestd = -1;
    for (int i = 0; i < 16; i++) {
        int pr = (int)((pal[i] >> 16) & 0xFF), pg = (int)((pal[i] >> 8) & 0xFF),
            pb = (int)(pal[i] & 0xFF);
        i64 d = (i64)(r - pr) * (r - pr) + (i64)(g - pg) * (g - pg) + (i64)(b - pb) * (b - pb);
        if (bestd < 0 || d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static void emit_color(AgcBuf *out, const Theme *t, u16 slot, bool fg) {
    u32 c = t->rgb[slot < TH_COUNT ? slot : TH_FG];
    int n = fg ? 38 : 48;
    int d = fg ? 30 : 40;
    if (t->mode == THEME_TRUE) {
        agentc_buf_printf(out, "\x1b[%d;2;%u;%u;%um", n, (unsigned)((c >> 16) & 0xFF),
                      (unsigned)((c >> 8) & 0xFF), (unsigned)(c & 0xFF));
    } else if (t->mode == THEME_256) {
        agentc_buf_printf(out, "\x1b[%d;5;%dm", n, rgb_to_256(c));
    } else {
        /* Muted is the low-emphasis slot: pin it to the palette's bright-black
         * (gray) entry so a 16-colour terminal cannot render it as the default
         * or bright white. Every other slot keeps the nearest-colour match. */
        int idx = slot == TH_MUTED ? 8 : rgb_to_16(c);
        int code = idx < 8 ? d + idx : (d + 60) + (idx - 8);
        agentc_buf_printf(out, "\x1b[%dm", code);
    }
}

void theme_emit_sgr(AgcBuf *out, const Theme *t, u16 attrs, u16 fg, u16 bg) {
    agentc_buf_cstr(out, "\x1b[0m");
    if (attrs & A_BOLD) agentc_buf_cstr(out, "\x1b[1m");
    if (attrs & A_DIM) agentc_buf_cstr(out, "\x1b[2m");
    if (attrs & A_ITALIC) agentc_buf_cstr(out, "\x1b[3m");
    if (attrs & A_UNDERLINE) agentc_buf_cstr(out, "\x1b[4m");
    if (attrs & A_REVERSE) agentc_buf_cstr(out, "\x1b[7m");
    emit_color(out, t, fg, true);
    if (bg == TH_NO_BG) agentc_buf_cstr(out, "\x1b[49m");
    else emit_color(out, t, bg, false);
}
