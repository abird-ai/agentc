/* theme.h — color slots and terminal color-mode downgrade. */
#ifndef AGENTC_TUI_THEME_H
#define AGENTC_TUI_THEME_H

#include "agentc.h"

/* Theme slots stored in grid cells. The renderer maps a slot to a concrete
 * color using the active Theme + detected color mode. */
enum {
    TH_FG = 0,
    TH_BG,
    TH_MUTED,
    TH_ACCENT,
    TH_OK,
    TH_WARN,
    TH_ERR,
    TH_USER,
    TH_ASSISTANT,
    TH_THINKING,
    TH_TOOL,
    TH_DIFF_ADD,
    TH_DIFF_DEL,
    TH_CODE,
    TH_TOOL_OK_BG,
    TH_TOOL_ERR_BG,
    TH_TOOL_BG,
    TH_COUNT,
};

enum {
    THEME_16 = 0,
    THEME_256 = 1,
    THEME_TRUE = 2,
};

/* Background pseudo-slot: keep the terminal's own background. The SGR emitter
 * writes the default-background sequence (49) instead of a 48;… colour, which
 * is how the transcript/input rows sit on the terminal background. Only the
 * status line and tool blocks paint theme backgrounds. */
#define TH_NO_BG 0xFFFFu

typedef struct {
    u32 rgb[TH_COUNT];
    int mode;      /* THEME_* */
    int dark;      /* light/dark variant */
    /* Appended: a fixed light/dark theme owns its background, so the TUI may
     * push TH_BG to the terminal with OSC 11 instead of leaving the palette's
     * text on whatever background the terminal happens to have. `system` and
     * named themes keep the terminal's own background. */
    bool force_bg;
} Theme;

/* dark: nonzero for the dark builtin. load_config: read
 * $XDG_CONFIG_HOME/agentc/theme.jsonc (or ~/.config/agentc/theme.jsonc) overrides. */
void theme_init(Theme *t, int dark, bool load_config);
/* Resolve one theme name: built-in dark/light, "system" (COLORFGBG heuristic,
 * dark when absent), or a <name>.jsonc file from the theme resource roots and
 * <config>/agentc/themes. The effective base is chosen by the built-in name,
 * then the config theme.jsonc "base", then the named file's "base"; config
 * slot overrides apply next and the named file's slots last, so the most
 * specific file wins slot by slot. An unknown name logs once and keeps the
 * current theme. */
bool theme_apply_named(Theme *t, const char *name);
/* Install the trusted project root (<cwd>/.agentc/themes) so theme_apply_named
 * can name-resolve project themes; untrusted or NULL/empty clears it. The TUI
 * sets this before resolving a name. */
void theme_set_project_root(const char *cwd, bool trusted);
void theme_set_mode(Theme *t, int mode);
int theme_detect_mode(void);
const char *theme_name(const Theme *t);

/* Emit a full SGR state change (reset + attrs + colors). */
void theme_emit_sgr(AgcBuf *out, const Theme *t, u16 attrs, u16 fg, u16 bg);

/* Parse a "#rrggbb"/"rrggbb" string; returns false when malformed. */
bool theme_parse_color(const char *s, u32 *out);

#endif /* AGENTC_TUI_THEME_H */
