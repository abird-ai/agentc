/* tui_test.c — deterministic golden test for the terminal UI.
 *
 * No tty: the in-memory backend is fed scripted bytes and synthesized agent
 * events; every scenario dumps the visible grid as plain text. Time is fixed
 * unless a test ticks it, so spinner/elapsed output is byte-stable.
 */
#include "tui/tui.h"
#include "tui/tui_test.h"
#include "tui/components.h"
#include "tui/editor.h"
#include "plat.h"
#include "tui/render.h"
#include "tui/theme.h"
#include "base/limits.h"
#include "core/prompt.h"
#include "core/prompts.h"
#include "core/tools/engine.h"
#include "tui/pick.h"
#include "app/setup.h"

/* internal helpers (not in the frozen headers) */
void agentc_test_setenv(const char *name, const char *value);
void agentc_test_clearenv(void);
void agentc_rm_rf(const char *path);
int agentc_write_file_atomic(const char *path, const void *data, size_t len, int mode);
/* test-only hook into the TUI startup banner printer (src/tui/tui.c) */
void agentc_tui_test_print_banner(AgcTuiTest *t);

#define TUI_ROOT "/tmp/agentc-tui-test"
#define TUI_CONF TUI_ROOT "/config"
#define TUI_PROJ TUI_ROOT "/proj"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
}

static bool contains(const char *hay, const char *needle) {
    return hay && agentc_str_str(hay, needle) != NULL;
}

static int count_occurrences(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0]) return 0;
    int n = 0;
    for (const char *p = hay; (p = agentc_str_str(p, needle)) != NULL; p++) n++;
    return n;
}

static void tui_wf(const char *path, const void *data, size_t len) {
    if (agentc_write_file_atomic(path, data, len, 0644) != 0)
        agentc_logf(3, "cannot write %s", path);
}

static void tui_wf_text(const char *path, const char *text) {
    tui_wf(path, text, agentc_strlen(text));
}

/* `/greet` expansion fixture (a non-file registry record) */
static char *tui_reg_expand(void *ud, const char *args) {
    (void)ud;
    AgcBuf b = { 0 };
    agentc_buf_printf(&b, "fixture:%s", args ? args : "");
    return (char *)b.p;
}

/* Register the prompt/skill/theme fixtures before the leak baseline is taken:
 * prompt records are pointer-stable and never freed by design. */
static void tui_fixture_setup(void) {
    agentc_test_clearenv();
    agentc_test_setenv("HOME", TUI_ROOT "/home");
    agentc_test_setenv("XDG_CONFIG_HOME", TUI_CONF);
    agentc_rm_rf(TUI_ROOT);

    tui_wf_text(TUI_CONF "/agentc/skills/demo/SKILL.md",
               "---\nname: demo\ndescription: Demo skill\n---\nskill body here\n");
    AgcBuf big = { 0 };
    agentc_buf_cstr(&big, "---\nname: big\ndescription: Big skill\n---\n");
    for (size_t i = 0; i <= AGENTC_SKILL_SUBMIT_MAX; i++) agentc_buf_byte(&big, 'a');
    agentc_buf_byte(&big, '\n');
    tui_wf(TUI_CONF "/agentc/skills/big/SKILL.md", big.p, big.len);
    agentc_buf_free(&big);

    tui_wf_text(TUI_CONF "/agentc/prompts/hi.md",
                "---\ndescription: Hi fixture\nargument-hint: \"[name]\"\n---\nhello $1\n");
    tui_wf_text(TUI_CONF "/agentc/themes/sol.jsonc", "{ \"accent\": \"#333333\" }\n");

    agentc_builtin_context_set(TUI_PROJ, true);
    (void)agentc_prompts_load_file_templates(TUI_PROJ, true);
    (void)agentc_prompts_register("help", "shadowed fixture", "", "tui-test", NULL, tui_reg_expand);
    (void)agentc_prompts_register("greet", "greet fixture", "[name]", "tui-test", NULL, tui_reg_expand);
    /* the skill: prefix is reserved for /skill:<name> dispatch */
    (void)agentc_prompts_register("skill:x", "reserved fixture", "", "tui-test", NULL,
                                  tui_reg_expand);

    /* Warm the provider registry and one handle before the leak baseline: they
     * are process-lifetime, and the model-picker test needs a real provider. */
    (void)agentc_setup_provider("ollama");
}

/* Copy screen row `row` (0-based) into out, stripping the newline. */
static size_t screen_line(const char *screen, int row, char *out, size_t cap) {
    size_t i = 0;
    for (int r = 0; r < row && screen[i]; r++) {
        while (screen[i] && screen[i] != '\n') i++;
        if (screen[i] == '\n') i++;
    }
    size_t n = 0;
    while (screen[i] && screen[i] != '\n' && n + 1 < cap) out[n++] = screen[i++];
    out[n] = 0;
    return n;
}

/* 0-based row of the first line containing `needle`, or -1. */
static int find_row(const char *screen, const char *needle) {
    if (!screen || !needle) return -1;
    size_t i = 0;
    for (int r = 0; screen[i]; r++) {
        size_t start = i;
        while (screen[i] && screen[i] != '\n') i++;
        size_t n = i - start;
        char line[1024];
        if (n >= sizeof line) n = sizeof line - 1;
        agentc_memcpy(line, screen + start, n);
        line[n] = 0;
        if (contains(line, needle)) return r;
        if (screen[i] == '\n') i++;
    }
    return -1;
}

/* True when the row is exactly `cols` U+2500 rules: the composer's shape. */
static bool is_rule_row(const char *line, int cols) {
    static const char rule[] = "\xE2\x94\x80";
    if (agentc_strlen(line) != (size_t)cols * 3) return false;
    for (int c = 0; c < cols; c++)
        if (!agentc_memeq(line + (size_t)c * 3, rule, 3)) return false;
    return true;
}

/* True when `needle` appears in the frame between the cursor move to `row`
 * (1-based) and the next occurrence of `marker`. Lets a test inspect just one
 * row's SGR prefix, which the plain-text screen dump cannot see. */
static bool row_sgr_before(const char *out, int row, const char *marker, const char *needle) {
    char at[32];
    agentc_snprintf(at, sizeof at, "\x1b[%d;1H", row);
    const char *p = agentc_str_str(out ? out : "", at);
    if (!p) return false;
    p += agentc_strlen(at);
    const char *m = agentc_str_str(p, marker);
    if (!m) return false;
    size_t n = (size_t)(m - p);
    char tmp[1024];
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    agentc_memcpy(tmp, p, n);
    tmp[n] = 0;
    return contains(tmp, needle);
}

/* Copy the bytes of fullscreen row `row` (1-based) from its cursor move up to
 * the next row's cursor move (or the end of the frame). Lets a test inspect a
 * blank row's SGR when there is no text marker to anchor on. */
static void row_bytes(const char *out, int row, char *tmp, size_t cap) {
    tmp[0] = 0;
    char at[32];
    agentc_snprintf(at, sizeof at, "\x1b[%d;1H", row);
    const char *p = agentc_str_str(out ? out : "", at);
    if (!p) return;
    p += agentc_strlen(at);
    char next[32];
    agentc_snprintf(next, sizeof next, "\x1b[%d;1H", row + 1);
    const char *e = agentc_str_str(p, next);
    size_t n = e ? (size_t)(e - p) : agentc_strlen(p);
    if (n >= cap) n = cap - 1;
    agentc_memcpy(tmp, p, n);
    tmp[n] = 0;
}

static char *read_file(const char *path, size_t *len) {
    int fd = os_open(path, OS_O_RDONLY, 0);
    if (fd < 0) return NULL;
    AgcBuf b = { 0 };
    char tmp[4096];
    for (;;) {
        int n = os_read(fd, tmp, sizeof tmp);
        if (n <= 0) break;
        agentc_buf_push(&b, tmp, (size_t)n);
    }
    os_close(fd);
    if (len) *len = b.len;
    return (char *)b.p;
}

static void dump(const char *name, AgcTuiTest *t) {
    const char *screen = agentc_tui_test_screen(t);
    agentc_outf("--- %s ---\n%s", name, screen);
    char path[256];
    agentc_snprintf(path, sizeof path, "tests/data/tui_%s.expected", name);
    size_t len = 0;
    char *exp = read_file(path, &len);
    bool ok = exp && len == agentc_strlen(screen) && agentc_memeq(exp, screen, len);
    check(name, ok);
    agentc_free(exp);
}

static void td(AgcTuiTest *t, const char *s) {
    AgcTextDelta d = { s, agentc_strlen(s) };
    agentc_tui_test_event(t, AGENTC_EV_TEXT_DELTA, &d);
}

static void think_delta(AgcTuiTest *t, const char *s) {
    AgcTextDelta d = { s, agentc_strlen(s) };
    agentc_tui_test_event(t, AGENTC_EV_THINK_DELTA, &d);
}

static void tool_start(AgcTuiTest *t, const char *name, const char *args) {
    AgcToolExec e;
    agentc_memset(&e, 0, sizeof e);
    e.call_id = "call_1";
    e.tool_name = name;
    e.args_json = args;
    agentc_tui_test_event(t, AGENTC_EV_TOOL_EXEC_START, &e);
}

static void tool_end(AgcTuiTest *t, const char *name, bool is_error, const char *result,
                     i64 ms) {
    AgcToolExec e;
    agentc_memset(&e, 0, sizeof e);
    e.call_id = "call_1";
    e.tool_name = name;
    e.result = result;
    e.is_error = is_error;
    e.duration_ms = ms;
    agentc_tui_test_event(t, AGENTC_EV_TOOL_EXEC_END, &e);
}

/* ------------------------------------------------------------- scenarios */

static void test_basics(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 10);
    const char *out = agentc_tui_test_output(t);
    check("term_alt_screen", contains(out, "\x1b[?1049h"));
    check("term_bracketed_paste", contains(out, "\x1b[?2004h"));
    check("term_hide_cursor", contains(out, "\x1b[?25l"));

    check("wcwidth_ascii", agentc_wcwidth('a') == 1);
    check("wcwidth_han", agentc_wcwidth(0x4F60) == 2);
    check("wcwidth_combining", agentc_wcwidth(0x0301) == 0);

    agentc_tui_test_feed(t, "hi", 2);
    agentc_tui_test_frame(t);
    out = agentc_tui_test_output(t);
    check("frame_sync_begin", contains(out, "\x1b[?2026h"));
    check("frame_sync_end", contains(out, "\x1b[?2026l"));
    check("frame_clear_tail", contains(out, "\x1b[K") || contains(out, "hi"));
    dump("basics", t);

    agentc_tui_test_feed(t, "\r", 1);
    agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
    agentc_tui_test_free(t);
}

static void test_typing(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 10);
    agentc_tui_test_feed(t, "helo", 4);
    dump("typing-raw", t);
    agentc_tui_test_feed(t, "\x7f", 1);
    agentc_tui_test_feed(t, "lo world", 8);
    dump("typing-fixed", t);
    agentc_tui_test_feed(t, "\r", 1);
    check("typing_running", agentc_tui_test_running(t));
    dump("typing-submitted", t);
    td(t, "answer text");
    agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
    dump("typing-answered", t);
    agentc_tui_test_free(t);
}

static void test_composer(void) {
    /* The composer is bracketed by two full-width dim rules and the input text
     * starts at column 0: no prompt marker, no highlight. Rows are 0-based:
     * 6 top rule, 7 input, 8 bottom rule, 9 footer. */
    AgcTuiTest *t = agentc_tui_test_new(40, 10);
    const char *screen = agentc_tui_test_screen(t);
    char line[256];
    screen_line(screen, 6, line, sizeof line);
    check("composer_top_rule", is_rule_row(line, 40));
    screen_line(screen, 7, line, sizeof line);
    check("composer_empty_placeholder", agentc_streq(line, "Ready. Press Ctrl-C once to clear, twic"));
    screen_line(screen, 8, line, sizeof line);
    check("composer_bottom_rule", is_rule_row(line, 40));
    check("composer_no_marker", !contains(screen, "> "));
    agentc_tui_test_frame(t);
    check("composer_empty_cursor", contains(agentc_tui_test_output(t), "\x1b[8;1H"));

    agentc_tui_test_feed(t, "hi", 2);
    agentc_tui_test_frame(t);
    screen = agentc_tui_test_screen(t);
    screen_line(screen, 7, line, sizeof line);
    check("composer_text_at_col0", agentc_streq(line, "hi"));
    check("composer_cursor_after_text", contains(agentc_tui_test_output(t), "\x1b[8;3H"));
    /* unchanged rules stay out of the next keystroke's diff (no flicker) */
    agentc_tui_test_output_clear(t);
    agentc_tui_test_feed(t, "i", 1);
    agentc_tui_test_frame(t);
    check("composer_rule_not_redrawn",
          !contains(agentc_tui_test_output(t), "\xE2\x94\x80"));
    agentc_tui_test_free(t);

    /* A narrow terminal still gets full-width rules; long input wraps between
     * them without spilling the frame. */
    t = agentc_tui_test_new(10, 6);
    agentc_tui_test_feed(t, "abcdefghij", 10);
    agentc_tui_test_frame(t);
    screen = agentc_tui_test_screen(t);
    screen_line(screen, 1, line, sizeof line);
    check("composer_narrow_top_rule", is_rule_row(line, 10));
    screen_line(screen, 2, line, sizeof line);
    check("composer_narrow_first_row", agentc_streq(line, "abcdefghi"));
    screen_line(screen, 3, line, sizeof line);
    check("composer_narrow_wrapped", agentc_streq(line, "j"));
    screen_line(screen, 4, line, sizeof line);
    check("composer_narrow_bottom_rule", is_rule_row(line, 10));
    agentc_tui_test_free(t);

    /* Minimum-size fullscreen (4 rows): there is no room for both rules, so
     * the composer degrades to the input row plus its bottom rule. */
    t = agentc_tui_test_new(8, 4);
    screen = agentc_tui_test_screen(t);
    screen_line(screen, 1, line, sizeof line);
    check("composer_tiny_placeholder", agentc_streq(line, "Ready."));
    screen_line(screen, 2, line, sizeof line);
    check("composer_tiny_bottom_rule", is_rule_row(line, 8));
    agentc_tui_test_free(t);

    /* The rules must sit on the terminal's own background (SGR 49), while the
     * status line keeps the theme background (48;…). Pinned in the emitted bytes
     * because the plain-text screen dump cannot see either. Rows 0-based at
     * 20x6: 2 top rule, 3 input, 4 bottom rule, 5 status. */
    t = agentc_tui_test_new(20, 6);
    agentc_tui_test_output_clear(t);
    agentc_tui_test_frame(t);
    const char *frame = agentc_tui_test_output(t);
    check("composer_rule_no_bg", !row_sgr_before(frame, 3, "\xE2\x94\x80", "48;"));
    check("composer_rule_default_bg", row_sgr_before(frame, 3, "\xE2\x94\x80", "\x1b[49m"));
    check("status_line_has_bg", row_sgr_before(frame, 6, "ready", "48;"));
    agentc_tui_test_free(t);
}

/* The empty-composer placeholder: dim, muted, background-free decoration drawn
 * on the input row. It keeps the old trigger (empty transcript, no run in
 * flight — the sentence is about quitting, which is only true before anything
 * has happened), and the renderer only draws it while the buffer is empty. It
 * is never in the buffer, the kill ring or the cursor math. */
static void test_empty_hint(void) {
    static const char hint[] = "Ready. Press Ctrl-C once to clear, twice to exit";
    char row[4096];
    char line[256];
    int cx = -1, cy = -1;
    bool rev = false;

    /* Fullscreen: the hint is on the composer's input row with dim + muted fg
     * and no band; the caret is still at column 0 and reversed. */
    AgcTuiTest *t = agentc_tui_test_new(56, 12);
    agentc_tui_test_frame(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("hint_in_composer", agentc_streq(line, hint));
    check("hint_caret_col0", cx == 0 && rev);
    const char *out = agentc_tui_test_output(t);
    row_bytes(out, cy + 1, row, sizeof row);
    check("hint_muted_attrs",
          contains(row, "\x1b[2m") && contains(row, "38;2;127;132;156"));
    check("hint_no_background", contains(row, "\x1b[49m") && !contains(row, "48;2;"));

    /* Typing replaces it immediately; clearing brings it back. */
    agentc_tui_test_feed(t, "h", 1);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("hint_replaced_by_typing", agentc_streq(line, "h"));
    agentc_tui_test_feed(t, "\x7f", 1);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("hint_returns_when_empty", agentc_streq(line, hint));

    /* Decoration is not text: the readline commands (including kill/yank) do
     * not move it into the buffer and the caret stays at column 0. */
    agentc_tui_test_feed(t, "\x01\x05\x0b\x19", 4);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("hint_not_in_buffer", agentc_streq(line, hint) && cx == 0 && rev);

    /* Any transcript content hides it for good. */
    agentc_tui_test_feed(t, "hi\r", 3);
    check("hint_gone_after_message", !contains(agentc_tui_test_screen(t), hint));
    agentc_tui_test_free(t);

    /* A run in flight with an empty transcript must not show it. */
    t = agentc_tui_test_new(56, 12);
    agentc_tui_test_set_running(t, true);
    check("hint_hidden_while_running", !contains(agentc_tui_test_screen(t), hint));
    agentc_tui_test_frame(t);
    check("hint_hidden_while_running_frame", !contains(agentc_tui_test_screen(t), hint));
    agentc_tui_test_free(t);

    /* Inline: the hint row is gone, so the live region is exactly the composer
     * plus the footer (four rows); the placeholder sits on the input row. */
    t = agentc_tui_test_new_mode(56, 12, AGENTC_TUI_INLINE);
    const char *screen = agentc_tui_test_screen(t);
    int rows = 0, lcy = -1, off = -1;
    agentc_tui_test_live_metrics(t, &rows, &lcy, &off);
    check("hint_inline_rows", rows == 4 && off == 0);
    screen_line(screen, 0, line, sizeof line);
    check("hint_inline_top_rule", is_rule_row(line, 56));
    screen_line(screen, lcy, line, sizeof line);
    check("hint_inline_input_row", agentc_streq(line, hint));
    agentc_tui_test_set_running(t, true);
    check("hint_inline_hidden_while_running",
          !contains(agentc_tui_test_screen(t), hint));
    agentc_tui_test_set_running(t, false);
    agentc_tui_test_feed(t, "hi\r", 3);
    screen = agentc_tui_test_screen(t);
    check("hint_inline_gone", !contains(screen, hint));
    check("hint_inline_not_committed",
          !contains(agentc_tui_test_scrollback(t), hint) &&
              contains(agentc_tui_test_scrollback(t), "> hi"));
    agentc_tui_test_free(t);
}

/* The slash-command menu: fed by the built-in command table and the extension host
 * registry, filtered by prefix while the composer holds an unterminated command
 * word. Placement is mode-dependent: above the composer in fullscreen, below it
 * inline. */
static void test_command_menu(void) {
    char row[4096];
    char line[256];
    int cx = -1, cy = -1;
    bool rev = false;

    /* Fullscreen: '/' opens the list above the composer; the first entry is
     * selected (reverse video) and descriptions come from the table. */
    AgcTuiTest *t = agentc_tui_test_new(56, 16);
    agentc_tui_test_feed(t, "/", 1);
    const char *screen = agentc_tui_test_screen(t);
    check("menu_opens_on_slash", contains(screen, "/model") && contains(screen, "/quit"));
    check("menu_shows_description", contains(screen, "switch model"));
    int menu_row = find_row(screen, "/clear");
    int rule_row = find_row(screen, "\xE2\x94\x80");   /* top composer rule */
    check("menu_fullscreen_above_composer", menu_row >= 0 && rule_row > menu_row);
    agentc_tui_test_output_clear(t);
    agentc_tui_test_frame(t);
    row_bytes(agentc_tui_test_output(t), menu_row + 1, row, sizeof row);
    check("menu_selected_reverse", contains(row, "\x1b[7m"));
    check("menu_no_band", contains(row, "\x1b[49m") && !contains(row, "48;"));

    /* Down moves the selection to the next entry and Up brings it back. */
    agentc_tui_test_feed(t, "\x1b[B", 3);
    agentc_tui_test_output_clear(t);
    agentc_tui_test_frame(t);
    screen = agentc_tui_test_screen(t);
    int help_row = find_row(screen, "/help");
    int clear_row = find_row(screen, "/clear");
    const char *out = agentc_tui_test_output(t);
    row_bytes(out, help_row + 1, row, sizeof row);
    bool help_sel = contains(row, "\x1b[7m");
    row_bytes(out, clear_row + 1, row, sizeof row);
    check("menu_down_moves_selection", help_sel && !contains(row, "\x1b[7m"));
    agentc_tui_test_feed(t, "\x1b[A", 3);
    agentc_tui_test_output_clear(t);
    agentc_tui_test_frame(t);
    screen = agentc_tui_test_screen(t);
    row_bytes(agentc_tui_test_output(t), find_row(screen, "/clear") + 1, row, sizeof row);
    check("menu_up_moves_selection", contains(row, "\x1b[7m"));

    /* The typed word is a prefix filter: '/mo' narrows to /model. */
    agentc_tui_test_feed(t, "mo", 2);   /* "/mo" */
    screen = agentc_tui_test_screen(t);
    check("menu_filter_prefix", contains(screen, "/model") &&
                                    !contains(screen, "/clear") &&
                                    !contains(screen, "/quit"));

    /* Escape dismisses without touching the text; the next edit reopens it. */
    agentc_tui_test_feed(t, "\x1b", 1);
    agentc_tui_test_tick(t, 60);
    agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("menu_escape_keeps_text", agentc_streq(line, "/mo"));
    check("menu_escape_closes", !contains(agentc_tui_test_screen(t), "/model"));
    agentc_tui_test_feed(t, "d", 1);    /* "/mod" */
    check("menu_reopens_on_edit", contains(agentc_tui_test_screen(t), "/model"));
    agentc_tui_test_free(t);

    /* Tab completes the selected command and the trailing space closes the
     * menu into the argument phase. */
    t = agentc_tui_test_new(48, 10);
    agentc_tui_test_feed(t, "/mo", 3);
    agentc_tui_test_feed(t, "\t", 1);
    agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("menu_tab_completes", agentc_streq(line, "/model"));
    check("menu_tab_closes", find_row(agentc_tui_test_screen(t), "/quit") == -1);
    agentc_tui_test_free(t);

    /* Enter accepts and runs through the existing command path, not a second
     * one: selecting /clear clears a transcript that already has content. */
    t = agentc_tui_test_new(48, 10);
    agentc_tui_test_feed(t, "hi\r", 3);
    td(t, "some text");
    check("menu_enter_precondition", contains(agentc_tui_test_screen(t), "some text"));
    agentc_tui_test_feed(t, "/", 1);
    agentc_tui_test_feed(t, "\r", 1);
    check("menu_enter_runs_command",
          !contains(agentc_tui_test_screen(t), "some text"));
    agentc_tui_test_free(t);

    /* Precedence: while the menu is open, Up/Down stay in the menu — they do
     * not fall through to history. */
    const char *hpath = "/tmp/agentc-tui-menu-history";
    os_unlink(hpath);
    int fd = os_open(hpath, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, 0644);
    if (fd >= 0) {
        (void)os_write(fd, "older command\n", 14);
        os_close(fd);
    }
    t = agentc_tui_test_new(48, 10);
    agentc_tui_test_set_history(t, hpath);
    agentc_tui_test_feed(t, "/", 1);
    agentc_tui_test_feed(t, "\x1b[A", 3);
    agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("menu_up_not_history", agentc_streq(line, "/") &&
                                     contains(agentc_tui_test_screen(t), "/model"));
    os_unlink(hpath);
    agentc_tui_test_free(t);

    /* Precedence: every readline binding still reaches the editor while the
     * menu is open. Ctrl-B/F, Alt-B/F, Ctrl-A/E move the caret; Ctrl-W/U/K/Y
     * edit the text and close or keep the menu by changing its word. */
    t = agentc_tui_test_new(48, 10);
    agentc_tui_test_feed(t, "/mo", 3);
    agentc_tui_test_feed(t, "\x1b" "b", 2);    /* Alt-B */
    agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("menu_alt_b_reaches_editor", cx == 1);
    agentc_tui_test_feed(t, "\x1b" "f", 2);    /* Alt-F */
    agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("menu_alt_f_reaches_editor", cx == 3);
    agentc_tui_test_feed(t, "\x02", 1);         /* Ctrl-B */
    agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("menu_ctrl_b_reaches_editor", cx == 2);
    agentc_tui_test_feed(t, "\x06", 1);         /* Ctrl-F */
    agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("menu_ctrl_f_reaches_editor", cx == 3);
    agentc_tui_test_feed(t, "\x17", 1);         /* Ctrl-W kills "mo" */
    agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("menu_ctrl_w_reaches_editor", cx == 1 && agentc_streq(line, "/"));
    check("menu_reopens_after_kill", contains(agentc_tui_test_screen(t), "/quit"));
    agentc_tui_test_feed(t, "\x01", 1);         /* Ctrl-A */
    agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("menu_ctrl_a_reaches_editor", cx == 0);
    agentc_tui_test_feed(t, "\x05", 1);         /* Ctrl-E */
    agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("menu_ctrl_e_reaches_editor", cx == 1);
    agentc_tui_test_feed(t, "\x15", 1);         /* Ctrl-U kills "/" -> empty */
    agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("menu_ctrl_u_reaches_editor",
          cx == 0 && agentc_streq(line, "Ready. Press Ctrl-C once to clear, twice to exi"));
    agentc_tui_test_feed(t, "ab", 2);
    agentc_tui_test_feed(t, "\x01\x0b\x19", 3); /* Ctrl-K kills, Ctrl-Y yanks */
    agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("menu_ctrl_ky_reaches_editor", cx == 2 && agentc_streq(line, "ab"));
    agentc_tui_test_free(t);

    /* Inline: the menu is below the composer (after its bottom rule) and above
     * the footer. */
    t = agentc_tui_test_new_mode(40, 12, AGENTC_TUI_INLINE);
    agentc_tui_test_feed(t, "/", 1);
    screen = agentc_tui_test_screen(t);
    int rule_top = find_row(screen, "\xE2\x94\x80");
    int menu_inline = find_row(screen, "/model");
    int footer_row = find_row(screen, "ready");
    check("menu_inline_below_composer",
          rule_top >= 0 && menu_inline > rule_top && footer_row > menu_inline);

    /* Short terminal (80x6): the live row budget is five, so the list is
     * capped to one entry and scrolls with the selection; the frame stays
     * intact — editor, one menu row, footer. */
    int rows0 = 0, lcy = -1, off = -1;
    agentc_tui_test_live_metrics(t, &rows0, &lcy, &off);
    check("menu_inline_rows", rows0 > 0);
    agentc_tui_test_free(t);
    t = agentc_tui_test_new_mode(80, 6, AGENTC_TUI_INLINE);
    agentc_tui_test_feed(t, "/", 1);
    screen = agentc_tui_test_screen(t);
    agentc_tui_test_live_metrics(t, &rows0, &lcy, &off);
    check("menu_short_rows", rows0 == 6);
    check("menu_short_two_entries", find_row(screen, "/clear") >= 0 &&
                                      find_row(screen, "/help") >= 0 &&
                                      find_row(screen, "ready") == 5);
    agentc_tui_test_feed(t, "\x1b[B\x1b[B", 6);
    screen = agentc_tui_test_screen(t);
    check("menu_short_scrolls", find_row(screen, "/model") >= 0 &&
                                   find_row(screen, "/clear") == -1);
    screen_line(screen, 0, line, sizeof line);
    check("menu_short_frame", find_row(screen, "ready") == 5 && is_rule_row(line, 80));
    agentc_tui_test_free(t);

    /* Chrome never enters scrollback: the menu lives in the live region only,
     * and dismissal clears its rows. */
    t = agentc_tui_test_new_mode(48, 12, AGENTC_TUI_INLINE);
    agentc_tui_test_feed(t, "hi\r", 3);
    td(t, "some text\n");
    agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
    agentc_tui_test_feed(t, "/", 1);
    agentc_tui_test_frame(t);
    check("menu_inline_open", contains(agentc_tui_test_screen(t), "/model"));
    agentc_tui_test_feed(t, "\x1b", 1);
    agentc_tui_test_tick(t, 60);
    agentc_tui_test_frame(t);
    check("menu_inline_dismissed", !contains(agentc_tui_test_screen(t), "/model"));
    check("menu_never_committed",
          !contains(agentc_tui_test_scrollback(t), "/model") &&
              !contains(agentc_tui_test_scrollback(t), "/quit"));
    agentc_tui_test_free(t);
}

/* Ctrl-C truth-check: the hint says "once to clear, twice to exit". Pin both
 * halves, including the press right after a clear (which must quit, not merely
 * arm the window). */
static void test_ctrl_c_semantics(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 8);
    agentc_tui_test_feed(t, "draft", 5);
    agentc_tui_test_feed(t, "\x03", 1);
    check("ctrl_c_once_clears", !contains(agentc_tui_test_screen(t), "draft"));
    check("ctrl_c_once_no_quit", !agentc_tui_test_quit(t));
    agentc_tui_test_feed(t, "\x03", 1);
    check("ctrl_c_twice_quits", agentc_tui_test_quit(t));
    agentc_tui_test_free(t);

    /* The double-press window is one second: re-arming after a clear must not
     * make two slow presses quit. */
    t = agentc_tui_test_new(48, 8);
    agentc_tui_test_feed(t, "draft", 5);
    agentc_tui_test_feed(t, "\x03", 1);
    agentc_tui_test_tick(t, 2000);
    agentc_tui_test_feed(t, "\x03", 1);
    check("ctrl_c_slow_second_no_quit", !agentc_tui_test_quit(t));
    agentc_tui_test_free(t);
}

/* The interactive model picker: `/model` with no argument lists the current
 * provider's catalog (filtered to its wire), typing narrows it, Up/Down moves,
 * Enter switches, Escape closes. The selected row is one full-width band. */
static void test_model_picker(void) {
    const AgcProvider *prov = agentc_setup_provider("ollama");
    AgcAgent *a = prov ? agentc_agent_new(prov, "pick-one") : NULL;
    check("picker_agent", a != NULL);
    if (!a) return;
    agentc_model_register_dynamic("ollama", "pick-one", "openai-chat", "http://p.test/v1",
                                  4096, 512, false, false);
    agentc_model_register_dynamic("ollama", "pick-two", "openai-chat", "http://p.test/v1",
                                  8192, 1024, true, false);
    agentc_model_register_dynamic("ollama", "pick-three", "openai-chat", "http://p.test/v1",
                                  16384, 2048, false, true);

    AgcTuiTest *t = agentc_tui_test_new_mode(72, 16, AGENTC_TUI_INLINE);
    agentc_tui_test_set_agent(t, a);

    /* `/model` opens the picker; every model for the provider is listed and the
     * current one is marked. */
    agentc_tui_test_feed(t, "/model\r", 7);
    const char *screen = agentc_tui_test_screen(t);
    check("picker_opens", contains(screen, "pick-one") && contains(screen, "pick-two") &&
                             contains(screen, "pick-three"));
    check("picker_marks_current", contains(screen, "(current)"));
    check("picker_no_slash_prefix", !contains(screen, "/pick-one"));

    /* The selection is one full-width reverse band, in the inline region too. */
    int r = find_row(screen, "pick-one");
    bool whole = r >= 0;
    for (int x = 0; x < 72 && whole; x++)
        whole = (agentc_tui_test_cell_attrs(t, x, r) & A_REVERSE) != 0;
    check("picker_sel_full_row", whole);

    /* Typing narrows by substring; backspace widens it again. (The status line
     * still names the current model, so assert on entries that only the menu
     * can show.) */
    agentc_tui_test_feed(t, "two", 3);
    screen = agentc_tui_test_screen(t);
    check("picker_filter", contains(screen, "pick-two") && !contains(screen, "pick-three"));
    agentc_tui_test_feed(t, "\x7f\x7f\x7f", 3);
    screen = agentc_tui_test_screen(t);
    check("picker_backspace", contains(screen, "pick-one") && contains(screen, "pick-three"));

    /* Escape closes without switching. */
    agentc_tui_test_feed(t, "\x1b", 1);
    agentc_tui_test_tick(t, 60);
    const AgcTranscript *tr = agentc_agent_transcript(a);
    check("picker_escape_keeps_model", tr && agentc_streq(tr->model, "pick-one"));
    check("picker_escape_closes", !contains(agentc_tui_test_screen(t), "(current)"));

    /* Down then Enter switches and closes; a notice reports the new model. */
    agentc_tui_test_feed(t, "/model\r", 7);
    agentc_tui_test_feed(t, "\x1b[B", 3);
    agentc_tui_test_feed(t, "\r", 1);
    tr = agentc_agent_transcript(a);
    check("picker_select", tr && agentc_streq(tr->model, "pick-two"));
    screen = agentc_tui_test_screen(t);
    const char *back = agentc_tui_test_scrollback(t);
    check("picker_select_closes", !contains(screen, "(current)"));
    check("picker_notice", contains(screen, "model: pick-two") || contains(back, "model: pick-two"));

    /* Ctrl-C closes the picker; it must not quit the app. */
    agentc_tui_test_feed(t, "/model\r", 7);
    check("picker_reopen", contains(agentc_tui_test_screen(t), "(current)"));
    agentc_tui_test_feed(t, "\x03", 1);
    check("picker_ctrl_c", !agentc_tui_test_quit(t) &&
                               !contains(agentc_tui_test_screen(t), "(current)"));

    agentc_tui_test_free(t);
    agentc_agent_free(a);
    agentc_model_clear_dynamic("ollama");
}

/* The pre-TUI picker (session resume) shares the list chrome and key map. The
 * in-memory terminal lets the golden harness drive it without a tty. */
static void test_pick(void) {
    const char *names[] = { "now", "5m", "2h" };
    const char *descs[] = { "first session", "second session", "third session" };
    Terminal *term = term_open_memory(60, 12);

    /* Down then Enter selects the second row. */
    term_mem_feed(term, (const u8 *)"\x1b[B\r", 4);
    int pick = agentc_tui_pick(term, "Resume a session", names, descs, 3, 0);
    check("pick_select", pick == 1);
    const AgcBuf *o = term_output(term);
    const char *out = o && o->p ? (const char *)o->p : "";
    check("pick_title", contains(out, "Resume a session"));
    check("pick_lists", contains(out, "first session") && contains(out, "third session"));

    /* Typing narrows by description. */
    term_output_clear(term);
    term_mem_feed(term, (const u8 *)"third\r", 6);
    pick = agentc_tui_pick(term, "Resume a session", names, descs, 3, 0);
    check("pick_filter", pick == 2);

    /* Escape picks nothing (start a new session). */
    term_mem_feed(term, (const u8 *)"\x1b", 1);
    pick = agentc_tui_pick(term, "Resume a session", names, descs, 3, 0);
    check("pick_escape", pick == -1);

    term_close(term);
}

/* App-services stub: records that /new asked for a fresh session. */
static int g_new_session_calls;
static int test_new_session_cb(void *ud) {
    (void)ud;
    g_new_session_calls++;
    return 0;
}
static int test_new_session_fail_cb(void *ud) {
    (void)ud;
    return -1;
}

/* /thinking opens the same modal picker; Down+Enter sets the level. */
static void test_thinking_picker(void) {
    const AgcProvider *prov = agentc_setup_provider("ollama");
    AgcAgent *a = prov ? agentc_agent_new(prov, "think-model") : NULL;
    check("think_agent", a != NULL);
    if (!a) return;
    agentc_agent_set_thinking(a, 4);   /* high: the picker must mark this row */
    AgcTuiTest *t = agentc_tui_test_new_mode(56, 14, AGENTC_TUI_INLINE);
    agentc_tui_test_set_agent(t, a);
    agentc_tui_test_feed(t, "/thinking\r", 10);
    const char *screen = agentc_tui_test_screen(t);
    char line[128];
    int hr = find_row(screen, "high");
    if (hr >= 0) screen_line(screen, hr, line, sizeof line);
    check("think_picker_opens", hr >= 0 && contains(line, "(current)"));
    agentc_tui_test_feed(t, "\x1b[B\r", 4);
    check("think_picker_set", agentc_streq(agentc_agent_thinking(a), "low"));
    /* an explicit argument still sets directly */
    agentc_tui_test_feed(t, "/thinking off\r", 14);
    check("think_direct", agentc_streq(agentc_agent_thinking(a), "off"));
    agentc_tui_test_free(t);
    agentc_agent_free(a);
}

/* /new asks the app to start a session then clears the view; /compact reports. */
static void test_new_and_compact(void) {
    const AgcProvider *prov = agentc_setup_provider("ollama");
    AgcAgent *a = prov ? agentc_agent_new(prov, "new-model") : NULL;
    check("new_agent", a != NULL);
    if (!a) return;

    AgcTuiTest *t = agentc_tui_test_new(56, 14);
    agentc_tui_test_set_agent(t, a);
    AgcTuiApp app = { .ud = NULL, .new_session = test_new_session_cb };
    agentc_tui_test_set_app(t, &app);
    g_new_session_calls = 0;
    td(t, "some text");
    agentc_tui_test_feed(t, "/new\r", 5);
    check("new_calls_app", g_new_session_calls == 1);
    check("new_clears_view", !contains(agentc_tui_test_screen(t), "some text"));
    check("new_note", contains(agentc_tui_test_screen(t), "new session") ||
                          contains(agentc_tui_test_scrollback(t), "new session"));
    /* compaction with no transport reports an error, never crashes */
    agentc_tui_test_feed(t, "/compact\r", 9);
    check("compact_notice", contains(agentc_tui_test_screen(t), "compact") ||
                                contains(agentc_tui_test_scrollback(t), "compact"));
    agentc_tui_test_free(t);

    /* without app services /new is view-only */
    t = agentc_tui_test_new(56, 14);
    agentc_tui_test_set_agent(t, a);
    agentc_tui_test_feed(t, "/new\r", 5);
    check("new_no_app", contains(agentc_tui_test_screen(t), "chat view cleared") ||
                            contains(agentc_tui_test_scrollback(t), "chat view cleared"));
    agentc_tui_test_free(t);

    /* a vetoed swap reports and leaves the current view intact */
    t = agentc_tui_test_new(56, 14);
    agentc_tui_test_set_agent(t, a);
    AgcTuiApp failapp = { .ud = NULL, .new_session = test_new_session_fail_cb };
    agentc_tui_test_set_app(t, &failapp);
    td(t, "keep me");
    agentc_tui_test_feed(t, "/new\r", 5);
    check("new_fail_keeps_view", contains(agentc_tui_test_screen(t), "keep me"));
    check("new_fail_note", contains(agentc_tui_test_screen(t), "cannot start a new session"));
    agentc_tui_test_free(t);
    agentc_agent_free(a);
}

static void test_multiline(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 10);
    agentc_tui_test_feed(t, "line one", 8);
    agentc_tui_test_feed(t, "\x1b\r", 2); /* Alt+Enter */
    agentc_tui_test_feed(t, "line two", 8);
    dump("multiline-edit", t);
    agentc_tui_test_feed(t, "\r", 1);
    dump("multiline-submitted", t);
    agentc_tui_test_free(t);
}

static void test_editing(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 8);
    agentc_tui_test_feed(t, "hello world", 11);
    agentc_tui_test_feed(t, "\x17", 1); /* ctrl+w kill word */
    dump("editing-kill-word", t);
    agentc_tui_test_feed(t, "\x15", 1); /* ctrl+u kill to start */
    dump("editing-kill-start", t);
    agentc_tui_test_feed(t, "fresh", 5);
    agentc_tui_test_feed(t, "\x19", 1); /* ctrl+y yank */
    dump("editing-yank", t);
    agentc_tui_test_feed(t, "\x1b[H", 3); /* home */
    agentc_tui_test_feed(t, "X", 1);
    dump("editing-home", t);
    agentc_tui_test_feed(t, "\x1b[F", 3); /* end */
    agentc_tui_test_feed(t, "Y", 1);
    dump("editing-end", t);

    agentc_tui_test_feed(t, "\x03", 1); /* clear editor */
    agentc_tui_test_feed(t, "abc def", 7);
    agentc_tui_test_feed(t, "\x1b[1;3D", 6); /* alt+left -> before "def" */
    agentc_tui_test_feed(t, "\x0b", 1);      /* ctrl+k kill to end of line */
    dump("editing-kill-line", t);
    agentc_tui_test_free(t);
}

/* Readline/emacs keymap: feed each sequence and assert both the resulting text
 * (the plain dump cannot see attributes) and the caret position. The clear
 * helper ticks past the double-Ctrl+C quit window so clearing never sets quit. */
static void clear_ed(AgcTuiTest *t) {
    agentc_tui_test_tick(t, 2000);
    agentc_tui_test_feed(t, "\x03", 1);
}

/* Compose the frame (the memory backend only knows the caret after a layout)
 * and then report the caret position and the rendered fallback attribute. */
static void caret(AgcTuiTest *t, int *x, int *y, bool *rev) {
    (void)agentc_tui_test_screen(t);
    agentc_tui_test_cursor(t, x, y, rev);
}

static void test_readline(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 8);
    int cx = -1, cy = -1;
    bool rev = false;
    char line[256];

    /* Ctrl+B/F move by character, like the plain arrows. */
    agentc_tui_test_feed(t, "abc def", 7);
    agentc_tui_test_feed(t, "\x02\x02", 2);
    caret(t, &cx, &cy, &rev);
    check("readline_ctrl_b", cx == 5);
    agentc_tui_test_feed(t, "\x06", 1);
    caret(t, &cx, &cy, &rev);
    check("readline_ctrl_f", cx == 6);

    /* Alt+B/F are word motions and clamp at both ends. */
    agentc_tui_test_feed(t, "\x1b" "b", 2);
    caret(t, &cx, &cy, &rev);
    check("readline_alt_b", cx == 4);
    agentc_tui_test_feed(t, "\x1b" "b", 2);
    caret(t, &cx, &cy, &rev);
    check("readline_alt_b_start", cx == 0);
    agentc_tui_test_feed(t, "\x1b" "f\x1b" "f\x1b" "f", 6);
    caret(t, &cx, &cy, &rev);
    check("readline_alt_f_end", cx == 7);

    /* Ctrl+Left/Right in both encodings: `1;5D` and xterm's legacy `5D`. */
    agentc_tui_test_feed(t, "\x1b[1;5D", 6);
    caret(t, &cx, &cy, &rev);
    check("readline_ctrl_left", cx == 4);
    agentc_tui_test_feed(t, "\x1b[1;5C", 6);
    caret(t, &cx, &cy, &rev);
    check("readline_ctrl_right", cx == 7);
    agentc_tui_test_feed(t, "\x1b[5D", 4);
    caret(t, &cx, &cy, &rev);
    check("readline_ctrl_left_legacy", cx == 4);
    agentc_tui_test_feed(t, "\x1b[5C", 4);
    caret(t, &cx, &cy, &rev);
    check("readline_ctrl_right_legacy", cx == 7);

    /* Ctrl+A/E are line ends (single line here: same as absolute). */
    agentc_tui_test_feed(t, "\x01", 1);
    caret(t, &cx, &cy, &rev);
    check("readline_ctrl_a", cx == 0);
    agentc_tui_test_feed(t, "\x05", 1);
    caret(t, &cx, &cy, &rev);
    check("readline_ctrl_e", cx == 7);

    /* Delete word forward at the start of a line, then again across the gap. */
    clear_ed(t);
    agentc_tui_test_feed(t, "hello world", 11);
    agentc_tui_test_feed(t, "\x01", 1);
    agentc_tui_test_feed(t, "\x1b" "d", 2);
    caret(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("readline_alt_d_start", cx == 0 && agentc_streq(line, " world"));
    agentc_tui_test_feed(t, "\x1b" "d", 2);
    caret(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    /* the buffer is empty again, so the input row shows the clipped placeholder */
    check("readline_alt_d_gap",
          cx == 0 && agentc_streq(line, "Ready. Press Ctrl-C once to clear, twice to exi"));

    agentc_tui_test_feed(t, "hello world", 11);
    agentc_tui_test_feed(t, "\x01", 1);
    agentc_tui_test_feed(t, "\x1b[3;5~", 6);   /* Ctrl+Delete */
    caret(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("readline_ctrl_delete", cx == 0 && agentc_streq(line, " world"));

    /* Kill/yank round-trips through all three directions of the kill buffer. */
    clear_ed(t);
    agentc_tui_test_feed(t, "one two three", 13);
    agentc_tui_test_feed(t, "\x17", 1);
    agentc_tui_test_feed(t, "\x19", 1);
    caret(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("readline_kill_word_yank", cx == 13 && agentc_streq(line, "one two three"));
    agentc_tui_test_feed(t, "\x15\x19", 2);
    caret(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("readline_kill_line_yank", cx == 13 && agentc_streq(line, "one two three"));
    agentc_tui_test_feed(t, "\x01\x06\x02\x0b\x19", 5);
    caret(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("readline_kill_end_yank", cx == 13 && agentc_streq(line, "one two three"));

    /* Alt+Backspace in both encodings (ESC DEL and kitty CSI 127;3u). */
    clear_ed(t);
    agentc_tui_test_feed(t, "foo bar", 7);
    agentc_tui_test_feed(t, "\x1b\x7f", 2);
    caret(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("readline_alt_backspace", cx == 4 && agentc_streq(line, "foo"));
    agentc_tui_test_feed(t, "baz", 3);
    agentc_tui_test_feed(t, "\x1b[127;3u", 8);
    caret(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("readline_alt_backspace_kitty", cx == 4 && agentc_streq(line, "foo"));

    /* Ctrl+T transposes around the caret, and the two before it at the end. */
    clear_ed(t);
    agentc_tui_test_feed(t, "ab", 2);
    agentc_tui_test_feed(t, "\x14", 1);
    caret(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("readline_ctrl_t_end", cx == 2 && agentc_streq(line, "ba"));
    agentc_tui_test_feed(t, "\x03", 1);
    agentc_tui_test_feed(t, "abc", 3);
    agentc_tui_test_feed(t, "\x1b[H\x06\x14", 6);
    caret(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("readline_ctrl_t_mid", cx == 2 && agentc_streq(line, "bac"));

    /* Ctrl+D deletes under the caret, but on an empty line it is EOF. */
    clear_ed(t);
    agentc_tui_test_feed(t, "abc", 3);
    agentc_tui_test_feed(t, "\x1b[H\x04", 4);
    caret(t, &cx, &cy, &rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("readline_ctrl_d_delete",
          cx == 0 && agentc_streq(line, "bc") && !agentc_tui_test_quit(t));
    agentc_tui_test_feed(t, "\x03\x04", 2);
    check("readline_ctrl_d_eof", agentc_tui_test_quit(t));
    agentc_tui_test_free(t);
}

/* ESC is a prefix, not an action: the 50 ms idle window decides. */
static void test_escape_disambiguation(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 8);
    int cx = -1, cy = -1;
    bool rev = false;

    /* ESC + key is an Alt sequence and must never trigger the lone-ESC abort. */
    agentc_tui_test_feed(t, "one two", 7);
    agentc_tui_test_feed(t, "\x1b", 1);
    agentc_tui_test_feed(t, "b", 1);
    caret(t, &cx, &cy, &rev);
    check("escape_alt_word", !agentc_tui_test_aborted(t) && cx == 4);
    agentc_tui_test_tick(t, 60);
    check("escape_alt_no_abort", !agentc_tui_test_aborted(t));

    /* ESC + CSI is ordinary input too (Ctrl+Right then legacy Ctrl+Left). */
    agentc_tui_test_feed(t, "\x1b[1;5C", 6);
    caret(t, &cx, &cy, &rev);
    check("escape_csi_no_abort", !agentc_tui_test_aborted(t) && cx == 7);
    agentc_tui_test_feed(t, "\x1b[5D", 4);
    caret(t, &cx, &cy, &rev);
    check("escape_csi_legacy_mods", !agentc_tui_test_aborted(t) && cx == 4);

    /* A lone ESC keeps its existing meaning: it aborts an in-flight run, but
     * only once the parser's idle window has expired. */
    agentc_tui_test_set_running(t, true);
    agentc_tui_test_feed(t, "\x1b", 1);
    check("escape_lone_pending", !agentc_tui_test_aborted(t));
    agentc_tui_test_tick(t, 60);
    check("escape_lone_aborts", agentc_tui_test_aborted(t));
    agentc_tui_test_free(t);
}

/* The caret has two representations that must agree: the real terminal cursor
 * (the CUP/G sequences in the emitted frame) and the reverse-video fallback
 * cell in the grid, which the memory backend can actually observe. */
static void test_cursor(void) {
    AgcTuiTest *t = agentc_tui_test_new(20, 6);
    int cx = -1, cy = -1;
    bool rev = false;
    char want[32];
    char line[256];

    /* Empty composer: the caret is visible at column 0. The frame hides the
     * cursor while painting and restores it parked on the caret. */
    agentc_tui_test_frame(t);
    const char *out = agentc_tui_test_output(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("cursor_empty_at_origin", cx == 0 && cy >= 0 && rev);
    check("cursor_fallback_reverse_sgr", contains(out, "\x1b[7m"));
    check("cursor_frame_hides_then_shows",
          contains(out, "\x1b[?25l\x1b[?2026h") &&
              contains(out, "\x1b[?25h\x1b[?2026l"));
    agentc_snprintf(want, sizeof want, "\x1b[%d;%dH", cy + 1, cx + 1);
    check("cursor_empty_hardware_agrees", contains(out, want));

    /* Mid-line: the caret sits after the text on the same cell the hardware
     * cursor is parked on. */
    agentc_tui_test_output_clear(t);
    agentc_tui_test_feed(t, "hi", 2);
    agentc_tui_test_frame(t);
    out = agentc_tui_test_output(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("cursor_midline", cx == 2 && rev);
    agentc_snprintf(want, sizeof want, "\x1b[%d;%dH", cy + 1, cx + 1);
    check("cursor_midline_hardware_agrees", contains(out, want));
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("cursor_midline_text", agentc_streq(line, "hi"));
    agentc_tui_test_free(t);

    /* Right margin: 10 columns leaves 9 for text and the caret in column 9. */
    t = agentc_tui_test_new(10, 6);
    agentc_tui_test_feed(t, "abcdefghi", 9);
    agentc_tui_test_frame(t);
    out = agentc_tui_test_output(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("cursor_right_margin", cx == 9 && rev);
    agentc_snprintf(want, sizeof want, "\x1b[%d;%dH", cy + 1, cx + 1);
    check("cursor_right_margin_hardware_agrees", contains(out, want));
    agentc_tui_test_feed(t, "j", 1);
    agentc_tui_test_frame(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("cursor_after_wrap", cx == 1 && rev);
    agentc_tui_test_free(t);

    /* Wrapped multi-row input: the caret is on the second visual row. */
    t = agentc_tui_test_new(20, 8);
    agentc_tui_test_feed(t, "one\x1b\rtwo", 8);
    agentc_tui_test_frame(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("cursor_wrapped_row", cx == 3 && rev);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("cursor_wrapped_row_text", agentc_streq(line, "two"));
    agentc_tui_test_free(t);

    /* A wide glyph under the caret reverses as one glyph: the fallback sits on
     * its first column and so does the hardware cursor. */
    t = agentc_tui_test_new(20, 6);
    agentc_tui_test_feed(t, "\xE4\xBD\xA0\xE5\xA5\xBD", 6);
    agentc_tui_test_frame(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("cursor_wide_end", cx == 4 && rev);
    agentc_tui_test_feed(t, "\x1b[D", 3);
    agentc_tui_test_frame(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("cursor_wide_glyph", cx == 2 && rev);
    agentc_tui_test_free(t);

    /* A zero-width terminal must not crash: the caret clamps to a valid cell
     * (there is no column past the text to park in). */
    t = agentc_tui_test_new(20, 6);
    agentc_tui_test_resize(t, 0, 6);
    agentc_tui_test_frame(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("cursor_zero_width_empty", cx == 0 && cy >= 0 && cy < 6 && rev);
    agentc_tui_test_feed(t, "x", 1);
    agentc_tui_test_frame(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("cursor_zero_width_text", cx == 0 && cy >= 0 && cy < 6);
    check("cursor_zero_width_frame", contains(agentc_tui_test_output(t), "\x1b[?25h"));
    agentc_tui_test_free(t);

    /* Inline parks the hardware cursor (hidden) at the region's top-left: the
     * reverse-video cell is the visible caret, so there is exactly one cursor,
     * and the top-left anchor is what the relative resize erase uses. */
    t = agentc_tui_test_new_mode(40, 10, AGENTC_TUI_INLINE);
    agentc_tui_test_frame(t);
    const char *io = agentc_tui_test_output(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("inline_cursor_empty", cx == 0 && rev);
    check("inline_cursor_frame",
          contains(io, "\x1b[?25l\x1b[?7l") && contains(io, "\x1b[?25l\x1b[?7h"));
    check("inline_cursor_hardware_agrees",
          !contains(io, ";1H") && contains(io, "\x1b[1G\x1b[?25l\x1b[?7h"));
    agentc_tui_test_feed(t, "ab", 2);
    agentc_tui_test_frame(t);
    io = agentc_tui_test_output(t);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("inline_cursor_midline", cx == 2 && rev);
    check("inline_cursor_midline_hardware_agrees",
          !contains(io, ";1H") && contains(io, "\x1b[1G\x1b[?25l\x1b[?7h"));
    int rows = 0, lcy = -1, off = -1;
    agentc_tui_test_live_metrics(t, &rows, &lcy, &off);
    check("inline_cursor_row_agrees", rows > 0 && cy >= 0 && cy < rows && off == 0);
    agentc_tui_test_free(t);
}

static void test_wrap(void) {
    AgcTuiTest *t = agentc_tui_test_new(44, 12);
    agentc_tui_test_feed(t, "go\r", 3);
    td(t, "The quick brown fox jumps over the lazy dog and keeps running far away.");
    dump("wrap", t);
    agentc_tui_test_free(t);
}

static void test_markdown(void) {
    AgcTuiTest *t = agentc_tui_test_new(44, 22);
    agentc_tui_test_feed(t, "m\r", 2);
    td(t, "# Heading one\n\nSome **bold** and `code` and *italic* words in a paragraph "
          "that needs to wrap across lines.\n\n");
    td(t, "- first item\n- second item\n\n> not-a-quote\n");
    td(t, "```\nint main(void) { return 0; }\n```\n");
    dump("markdown", t);
    agentc_tui_test_free(t);
}

static void test_thinking(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 10);
    agentc_tui_test_feed(t, "t\r", 2);
    think_delta(t, "pondering the problem carefully");
    td(t, "the answer");
    agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
    dump("thinking", t);
    agentc_tui_test_free(t);
}

/* The configured level gates the view: a reasoning model can stream reasoning
 * with the level off, but the transcript stays answer-only (the agent
 * transcript still records the block for replay). */
static void test_thinking_off_hidden(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 10);
    agentc_tui_test_feed(t, "t\r", 2);
    think_delta(t, "pondering the problem carefully");
    td(t, "the answer");
    agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
    check("thinking_off_hidden", !contains(agentc_tui_test_screen(t), "pondering"));
    check("thinking_off_answer_shown", contains(agentc_tui_test_screen(t), "the answer"));
    agentc_tui_test_free(t);
}

/* A level above off shows the reasoning block. */
static void test_thinking_on_shown(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 10);
    agentc_tui_test_set_footer(t, "?", "high", 0, 0, 0);
    agentc_tui_test_feed(t, "t\r", 2);
    think_delta(t, "pondering the problem carefully");
    td(t, "the answer");
    agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
    check("thinking_on_shown", contains(agentc_tui_test_screen(t), "pondering"));
    agentc_tui_test_free(t);
}

/* AGENTC_EV_MSG_RESET rolls the chat back to the block watermark recorded at the
 * matching MSG_START and drops the coalesced deltas, so a retried assistant
 * turn replaces the abandoned attempt instead of appending to it. MSG_END
 * retires the watermark so a late reset cannot truncate unrelated content. */
static void test_msg_reset(void) {
    /* (a) deltas still coalesced in the pending buffer are discarded outright */
    AgcTuiTest *t = agentc_tui_test_new(48, 12);
    agentc_tui_test_feed(t, "hi\r", 3);
    agentc_tui_test_set_running(t, true);
    agentc_tui_test_event(t, AGENTC_EV_MSG_START, NULL);
    td(t, "draft-pending");
    agentc_tui_test_event(t, AGENTC_EV_MSG_RESET, NULL);
    td(t, "final-pending");
    const char *screen = agentc_tui_test_screen(t);
    check("msg_reset_pending_dropped",
          contains(screen, "final-pending") && !contains(screen, "draft-pending"));
    check("msg_reset_pending_single", count_occurrences(screen, "final-pending") == 1);
    agentc_tui_test_free(t);

    /* (b) a draft already flushed into the chat by an earlier frame is truncated
     * away, and the retry's replacement is the only block left */
    t = agentc_tui_test_new(48, 12);
    agentc_tui_test_feed(t, "hi\r", 3);
    agentc_tui_test_set_running(t, true);
    agentc_tui_test_event(t, AGENTC_EV_MSG_START, NULL);
    td(t, "draft-shown");
    agentc_tui_test_frame(t);   /* flush: the draft now lives in the chat */
    check("msg_reset_draft_seen", contains(agentc_tui_test_screen(t), "draft-shown"));
    agentc_tui_test_event(t, AGENTC_EV_MSG_RESET, NULL);
    td(t, "final-shown");
    screen = agentc_tui_test_screen(t);
    check("msg_reset_flushed_dropped",
          contains(screen, "final-shown") && !contains(screen, "draft-shown"));
    agentc_tui_test_free(t);

    /* (c) MSG_END clears the watermark, so a stray reset no longer truncates
     * content that was already finished. */
    t = agentc_tui_test_new(48, 12);
    agentc_tui_test_feed(t, "hi\r", 3);
    agentc_tui_test_set_running(t, true);
    agentc_tui_test_event(t, AGENTC_EV_MSG_START, NULL);
    td(t, "kept-text");
    agentc_tui_test_event(t, AGENTC_EV_MSG_END, NULL);   /* flushes kept-text */
    agentc_tui_test_frame(t);
    check("msg_end_content_present", contains(agentc_tui_test_screen(t), "kept-text"));
    agentc_tui_test_event(t, AGENTC_EV_MSG_RESET, NULL); /* no open message: no-op */
    screen = agentc_tui_test_screen(t);
    check("msg_end_clears_watermark", contains(screen, "kept-text"));
    agentc_tui_test_free(t);
}

static void test_tools(void) {
    AgcTuiTest *t = agentc_tui_test_new(56, 16);
    agentc_tui_test_feed(t, "run\r", 4);
    tool_start(t, "read", "{\"path\":\"src/base/mem.c\",\"limit\":80}");
    dump("tool-running", t);
    tool_end(t, "read", false,
             "line one\nline two\nline three\nline four\nline five\nline six\n", 12);
    dump("tool-done", t);
    agentc_tui_test_free(t);
}

static void test_tool_error(void) {
    AgcTuiTest *t = agentc_tui_test_new(56, 12);
    agentc_tui_test_feed(t, "boom\r", 5);
    tool_start(t, "bash", "{\"command\":\"false\"}");
    tool_end(t, "bash", true, "command failed\nexit code: 1\n", 33);
    dump("tool-error", t);
    agentc_tui_test_free(t);
}

static void test_diff(void) {
    AgcTuiTest *t = agentc_tui_test_new(56, 16);
    agentc_tui_test_feed(t, "edit\r", 5);
    tool_start(t, "edit", "{\"path\":\"a.c\"}");
    tool_end(t, "edit", false,
             "edited a.c: 1 edit(s)\n@@ -1,2 +1,2 @@\n-old line\n+new line\n context\n", 7);
    dump("diff", t);
    agentc_tui_test_free(t);
}

/* Tool-card outcome backgrounds. The plain-text goldens cannot see them, so
 * these assert the emitted SGR: every outcome carries its own 48;2;… value,
 * the rows around the card keep 49, and a diff's foreground tint draws on top
 * of the band instead of replacing it (the documented precedence rule). */
static void test_tool_bg(void) {
    char row[4096];

    /* Completed card: muted green band on every card row, default background
     * on the user block above it and on the gap row below it. */
    AgcTuiTest *t = agentc_tui_test_new(56, 16);
    agentc_tui_test_feed(t, "run\r", 4);
    tool_start(t, "read", "{\"path\":\"a.c\"}");
    tool_end(t, "read", false, "contents\n", 4);
    agentc_tui_test_frame(t);
    const char *out = agentc_tui_test_output(t);
    check("tool_ok_bg", row_sgr_before(out, 3, "[read]", "48;2;40;50;40"));
    check("tool_ok_bg_status", row_sgr_before(out, 3, "ok 4ms", "48;2;40;50;40"));
    check("tool_ok_bg_output", row_sgr_before(out, 4, "contents", "48;2;40;50;40"));
    check("tool_user_row_no_band",
          row_sgr_before(out, 1, "> run", "\x1b[49m") &&
              !row_sgr_before(out, 1, "> run", "48;2;"));
    row_bytes(out, 5, row, sizeof row);   /* gap after the card */
    check("tool_gap_row_no_band", contains(row, "\x1b[49m") && !contains(row, "48;2;"));
    agentc_tui_test_free(t);

    /* Failed card: muted red band, and the error output text keeps its fg. */
    t = agentc_tui_test_new(56, 16);
    agentc_tui_test_feed(t, "boom\r", 5);
    tool_start(t, "bash", "{\"command\":\"false\"}");
    tool_end(t, "bash", true, "command failed\nexit code: 1\n", 33);
    agentc_tui_test_frame(t);
    out = agentc_tui_test_output(t);
    check("tool_err_bg", row_sgr_before(out, 3, "[bash]", "48;2;60;40;40"));
    check("tool_err_bg_status", row_sgr_before(out, 3, "err 33ms", "48;2;60;40;40"));
    check("tool_err_fg_on_band",
          row_sgr_before(out, 3, "err 33ms", "38;2;243;139;168") &&
              row_sgr_before(out, 3, "err 33ms", "48;2;60;40;40"));
    check("tool_err_bg_output", row_sgr_before(out, 4, "command failed", "48;2;60;40;40"));
    agentc_tui_test_free(t);

    /* Running (and any card whose end never arrived): muted grey band. */
    t = agentc_tui_test_new(56, 16);
    agentc_tui_test_feed(t, "run\r", 4);
    tool_start(t, "read", "{\"path\":\"a.c\"}");
    agentc_tui_test_frame(t);
    out = agentc_tui_test_output(t);
    check("tool_running_bg", row_sgr_before(out, 3, "[read]", "48;2;40;40;50"));
    agentc_tui_test_free(t);

    /* A diff inside a completed card: the add/remove foreground tints stay on
     * top of the band, and the collapsed marker row is part of the band. */
    t = agentc_tui_test_new(56, 16);
    agentc_tui_test_feed(t, "edit\r", 5);
    tool_start(t, "edit", "{\"path\":\"a.c\"}");
    tool_end(t, "edit", false,
             "edited a.c: 1 edit(s)\n@@ -1,2 +1,2 @@\n-old line\n+new line\n context\n", 7);
    agentc_tui_test_frame(t);
    out = agentc_tui_test_output(t);
    check("tool_diff_marker_band", row_sgr_before(out, 4, "(+2 lines)", "48;2;40;50;40"));
    check("tool_diff_add_fg_on_band",
          row_sgr_before(out, 6, "+new line", "38;2;166;227;161") &&
              row_sgr_before(out, 6, "+new line", "48;2;40;50;40"));
    check("tool_diff_del_fg_on_band",
          row_sgr_before(out, 5, "-old line", "38;2;243;139;168") &&
              row_sgr_before(out, 5, "-old line", "48;2;40;50;40"));
    agentc_tui_test_free(t);

    /* Inline mode: the band is committed to scrollback with the card, and the
     * later repaint of the pinned composer must not carry it (no bleed). */
    t = agentc_tui_test_new_mode(44, 12, AGENTC_TUI_INLINE);
    agentc_tui_test_feed(t, "run\r", 4);
    tool_start(t, "read", "{\"path\":\"a.c\"}");
    tool_end(t, "read", false, "contents\n", 4);
    agentc_tui_test_frame(t);
    out = agentc_tui_test_output(t);
    check("tool_inline_band_committed", contains(out, "48;2;40;50;40"));
    agentc_tui_test_output_clear(t);
    agentc_tui_test_feed(t, "x", 1);
    agentc_tui_test_frame(t);
    out = agentc_tui_test_output(t);
    check("tool_inline_band_no_bleed", !contains(out, "48;2;40;50;40"));
    agentc_tui_test_free(t);
}

static void test_footer(void) {
    AgcTuiTest *t = agentc_tui_test_new(64, 8);
    agentc_tui_test_set_footer(t, "claude-sonnet-4-5", "high", 1234, 567, 12345);
    agentc_tui_test_feed(t, "hello\r", 6);
    dump("footer", t);
    agentc_tui_test_free(t);
}

static void test_scroll(void) {
    AgcTuiTest *t = agentc_tui_test_new(44, 10);
    agentc_tui_test_feed(t, "long\r", 5);
    for (int i = 0; i < 8; i++) {
        char line[64];
        agentc_snprintf(line, sizeof line, "paragraph %d with some text\n\n", i);
        td(t, line);
    }
    dump("scroll-bottom", t);
    agentc_tui_test_feed(t, "\x1b[5~", 4); /* PageUp */
    dump("scroll-pageup", t);
    agentc_tui_test_feed(t, "\x1b[6~", 4); /* PageDown */
    dump("scroll-pagedown", t);
    agentc_tui_test_free(t);
}

static void test_abort(void) {
    AgcTuiTest *t = agentc_tui_test_new(52, 12);
    agentc_tui_test_feed(t, "first\r", 6);
    check("abort_running", agentc_tui_test_running(t));
    agentc_tui_test_feed(t, "second message\r", 15);
    dump("abort-queued", t);
    agentc_tui_test_feed(t, "\x1b", 1);
    check("abort_esc_debounced", !agentc_tui_test_aborted(t));
    agentc_tui_test_tick(t, 60);
    check("abort_flag", agentc_tui_test_aborted(t));
    dump("abort-editor", t);
    agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
    dump("abort-finished", t);
    agentc_tui_test_free(t);
}

static void test_ctrl_c(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 8);
    agentc_tui_test_feed(t, "draft", 5);
    agentc_tui_test_feed(t, "\x03", 1); /* clears editor once */
    dump("ctrl-c-clear", t);
    agentc_tui_test_feed(t, "\x03", 1); /* the clear armed the window -> quit */
    check("ctrl_c_clear_then_quit", agentc_tui_test_quit(t));
    agentc_tui_test_free(t);
}

static void test_resize(void) {
    AgcTuiTest *t = agentc_tui_test_new(44, 8);
    agentc_tui_test_feed(t, "go\r", 3);
    td(t, "wrapping text before resize should reflow once the window grows wider");
    dump("resize-before", t);
    agentc_tui_test_resize(t, 56, 12);
    dump("resize-after", t);
    agentc_tui_test_free(t);
}

static void test_paste(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 12);
    AgcBuf b = { 0 };
    agentc_buf_cstr(&b, "\x1b[200~");
    for (int i = 1; i <= 12; i++) {
        char line[64];
        agentc_snprintf(line, sizeof line, "paste line %d\n", i);
        agentc_buf_cstr(&b, line);
    }
    agentc_buf_cstr(&b, "\x1b[201~");
    agentc_tui_test_feed(t, (const char *)b.p, b.len);
    agentc_buf_free(&b);
    dump("paste-marker", t);
    agentc_tui_test_feed(t, "\r", 1);
    dump("paste-expanded", t);
    agentc_tui_test_free(t);
}

static void test_history(void) {
    const char *path = "/tmp/agentc-tui-history-test";
    os_unlink(path);
    int fd = os_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, 0644);
    if (fd >= 0) {
        (void)os_write(fd, "older command\n", 14);
        os_close(fd);
    }
    AgcTuiTest *t = agentc_tui_test_new(48, 8);
    agentc_tui_test_set_history(t, path);
    agentc_tui_test_feed(t, "\x1b[A", 3);
    dump("history-up", t);
    agentc_tui_test_feed(t, "\r", 1);
    fd = os_open(path, OS_O_RDONLY, 0);
    char buf[256];
    int n = fd >= 0 ? os_read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) os_close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    check("history_appended", agentc_str_str(buf, "older command\nolder command\n") != NULL);
    dump("history-submitted", t);
    os_unlink(path);
    agentc_tui_test_free(t);
}

static void test_completion(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 6);
    agentc_tui_test_feed(t, "@sr\t", 4);
    dump("completion", t);
    agentc_tui_test_free(t);
}

static void test_unicode(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 8);
    agentc_tui_test_feed(t, "\xE4\xBD\xA0\xE5\xA5\xBD e\xCC\x81", 10);
    dump("unicode-edit", t);
    agentc_tui_test_feed(t, "\r", 1);
    dump("unicode", t);
    agentc_tui_test_free(t);
}

/* Overlong forms, surrogates and values above U+10FFFF are not scalar values:
 * both decoders must replace each offending byte with U+FFFD instead of
 * emitting CESU-8 (the old surrogate path) or dropping a counted column (the
 * old > U+10FFFF path). The terminal parser carries a bracketed paste verbatim,
 * so it reaches the editor's own decoder. */
static void test_unicode_invalid(void) {
    /* render.c utf8_decode via agentc_text_width: one U+FFFD per bad byte. */
    check("utf8_overlong_width", agentc_text_width("\xC0\xAF") == 2);
    check("utf8_surrogate_width", agentc_text_width("\xED\xA0\x80") == 3);
    check("utf8_above_max_width", agentc_text_width("\xF4\x90\x80\x80") == 4);
    check("utf8_emoji_still_wide", agentc_text_width("\xF0\x9F\x98\x80") == 2);

    /* editor.c editor_decode: a raw surrogate pasted into the composer renders
     * as three replacement glyphs, never the raw CESU-8 bytes. */
    AgcTuiTest *t = agentc_tui_test_new(48, 8);
    const char *paste = "\x1b[200~\xED\xA0\x80\x1b[201~";
    agentc_tui_test_feed(t, paste, agentc_strlen(paste));
    const char *screen = agentc_tui_test_screen(t);
    check("unicode_invalid_editor_replacement",
          contains(screen, "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"));
    check("unicode_invalid_editor_no_cesu8", !contains(screen, "\xED\xA0\x80"));
    dump("unicode-invalid", t);
    agentc_tui_test_free(t);
}

static void test_sanitizer(void) {
    /* C1 (UTF-8 C2 9B) and a raw OSC sequence inside model text: neither the
     * grid nor the emitted frame bytes may carry them (the grid path and the
     * frame writer each sanitize). */
    AgcTuiTest *t = agentc_tui_test_new(48, 8);
    agentc_tui_test_feed(t, "hi\r", 3);
    td(t, "a\xc2\x9b" "31m b\x1b]52;c;AAAA\x07 c");
    agentc_tui_test_frame(t);
    const char *screen = agentc_tui_test_screen(t);
    const char *out = agentc_tui_test_output(t);
    check("tui.sanitizer.grid_c1", !contains(screen, "\xc2\x9b"));
    check("tui.sanitizer.grid_osc", !contains(screen, "\x1b]"));
    check("tui.sanitizer.emit_c1", !contains(out, "\xc2\x9b"));
    check("tui.sanitizer.emit_osc", !contains(out, "\x1b]"));
    agentc_tui_test_free(t);
}

static void test_commands(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 12);
    agentc_tui_test_feed(t, "hi\r", 3);
    td(t, "some text");
    agentc_tui_test_feed(t, "/clear\r", 7);
    check("command_clear", !contains(agentc_tui_test_screen(t), "some text"));
    agentc_tui_test_feed(t, "/help\r", 6);
    check("command_help", contains(agentc_tui_test_screen(t), "- /quit /new /clear /help"));
    agentc_tui_test_feed(t, "/theme light\r", 13);
    check("command_theme", contains(agentc_tui_test_screen(t), "theme: light"));
    agentc_tui_test_feed(t, "/quit\r", 6);
    dump("commands", t);
    agentc_tui_test_free(t);
}

/* ------------------------------------------------------------ inline mode */

static void test_scrollback(void) {
    /* 1. scrollback mode: no alternate screen, append-only rendering */
    AgcTuiTest *t = agentc_tui_test_new_mode(44, 12, AGENTC_TUI_SCROLLBACK);
    const char *out = agentc_tui_test_output(t);
    check("inline_no_alt_screen", !contains(out, "\x1b[?1049h"));
    check("inline_paste_on", contains(out, "\x1b[?2004h"));
    check("inline_scrollback_empty", agentc_tui_test_scrollback(t)[0] == 0);

    /* 2. a finished turn is committed to scrollback exactly once */
    agentc_tui_test_feed(t, "hi\r", 3);
    td(t, "hello from the model\n");
    agentc_tui_test_event(t, AGENTC_EV_MSG_END, NULL);
    tool_start(t, "read", "{\"path\":\"a.c\"}");
    tool_end(t, "read", false, "file contents here", 3);
    agentc_tui_test_frame(t);
    const char *sb = agentc_tui_test_scrollback(t);
    check("inline_commits_user", contains(sb, "> hi"));
    check("inline_commits_answer", contains(sb, "hello from the model"));
    check("inline_commits_tool", contains(sb, "read") && contains(sb, "file contents here"));
    check("inline_commit_once", count_occurrences(sb, "hello from the model") == 1);

    /* a second frame must not reprint what is already committed */
    agentc_tui_test_frame(t);
    agentc_tui_test_frame(t);
    check("inline_no_duplicate_commit",
          count_occurrences(agentc_tui_test_scrollback(t), "hello from the model") == 1);

    /* 3. the live region is bounded: it holds no transcript but the editor+footer */
    const char *live = agentc_tui_test_screen(t);
    check("inline_live_footer", contains(live, "think:off"));
    agentc_tui_test_feed(t, "x", 1);
    live = agentc_tui_test_screen(t);
    /* the live composer: two full-width rules bracket the input, which sits on
     * its own row with no prompt marker */
    {
        char line[256];
        screen_line(live, 0, line, sizeof line);
        bool top = is_rule_row(line, 44);
        screen_line(live, 1, line, sizeof line);
        bool text = agentc_streq(line, "x");
        screen_line(live, 2, line, sizeof line);
        bool bottom = is_rule_row(line, 44);
        check("inline_live_editor", top && text && bottom && !contains(live, "> x"));
    }
    check("inline_live_no_transcript", !contains(live, "hello from the model"));
    check("inline_live_bounded", agentc_strlen(live) > 0);
    dump("inline_idle", t);
    agentc_tui_test_free(t);

    /* 4. streaming text taller than the screen commits the overflow and keeps
     *    only the tail live */
    t = agentc_tui_test_new_mode(40, 8, AGENTC_TUI_SCROLLBACK);
    agentc_tui_test_set_running(t, true);   /* streaming: the tail stays live */
    AgcBuf big = { 0 };
    for (int i = 0; i < 30; i++) agentc_buf_printf(&big, "line %02d\n", i);
    {
        AgcTextDelta d = { (const char *)big.p, big.len };
        agentc_tui_test_event(t, AGENTC_EV_TEXT_DELTA, &d);
    }
    agentc_buf_free(&big);
    agentc_tui_test_frame(t);
    sb = agentc_tui_test_scrollback(t);
    live = agentc_tui_test_screen(t);
    check("inline_overflow_commits_early", !contains(sb, "line 00"));
    check("inline_overflow_tail_live", contains(live, "line 29") || contains(live, "line 28"));
    check("inline_overflow_not_both", !contains(sb, "line 29"));
    /* the run finishes: the remaining live text is committed as well */
    agentc_tui_test_set_running(t, false);
    agentc_tui_test_frame(t);
    check("inline_overflow_commit_on_finish",
          contains(agentc_tui_test_scrollback(t), "line 29") &&
              !contains(agentc_tui_test_screen(t), "line 29"));
    dump("inline_overflow", t);
    agentc_tui_test_free(t);

    /* 5. cursor bookkeeping: the frame moves up by exactly the cursor row, so
     *    the region cannot drift down the screen (the old bug printed a new
     *    footer line on every keypress) */
    t = agentc_tui_test_new_mode(40, 10, AGENTC_TUI_SCROLLBACK);
    agentc_tui_test_feed(t, "x", 1);
    agentc_tui_test_frame(t);
    int rows0 = 0, cy = 0, off = 0;
    agentc_tui_test_live_metrics(t, &rows0, &cy, &off);
    check("inline_metrics_valid", rows0 > 0 && cy >= 0 && cy < rows0 && off == cy);
    agentc_tui_test_output_clear(t);
    agentc_tui_test_feed(t, "y", 1);
    agentc_tui_test_frame(t);
    int rows1 = 0, cy1 = 0, off1 = 0;
    agentc_tui_test_live_metrics(t, &rows1, &cy1, &off1);
    check("inline_metrics_stable", rows1 == rows0 && off1 == cy1);
    {
        const char *frame = agentc_tui_test_output(t);
        char want[32];
        agentc_snprintf(want, sizeof want, "\x1b[?7l\x1b[%dA", off);
        check("inline_frame_moves_up_by_cursor_row",
              off == 0 ? contains(frame, "\x1b[?7l") : contains(frame, want));
    }
    agentc_tui_test_free(t);

    /* a resize must drop the bookkeeping instead of moving into a reflowed row */
    t = agentc_tui_test_new_mode(40, 10, AGENTC_TUI_SCROLLBACK);
    agentc_tui_test_frame(t);
    agentc_tui_test_resize(t, 60, 14);
    agentc_tui_test_live_metrics(t, &rows0, &cy, &off);
    check("inline_resize_resets_region", rows0 == 0 && off == 0);
    agentc_tui_test_frame(t);
    agentc_tui_test_live_metrics(t, &rows1, &cy1, &off1);
    check("inline_resize_redraws", rows1 > 0 && off1 == cy1);
    agentc_tui_test_free(t);

    /* the scrollback-mode resize must erase the region it is abandoning before
     * re-anchoring; otherwise the placeholder (or any chrome) stays on screen
     * above the new frame — the double-hint defect visible after a resize. */
    t = agentc_tui_test_new_mode(40, 10, AGENTC_TUI_SCROLLBACK);
    agentc_tui_test_frame(t);
    int old_rows = 0, old_off = 0;
    agentc_tui_test_live_metrics(t, &old_rows, &cy, &old_off);
    agentc_tui_test_output_clear(t);
    agentc_tui_test_resize(t, 60, 14);
    out = agentc_tui_test_output(t);
    char up[32];
    agentc_snprintf(up, sizeof up, "\x1b[?7l\x1b[%dA", old_off);
    check("inline_resize_erases_old_region", contains(out, up));
    check("inline_resize_erases_all_rows",
          count_occurrences(out, "\r\x1b[2K") == old_rows);
    agentc_tui_test_frame(t);
    check("inline_resize_no_stale_hint",
          count_occurrences(agentc_tui_test_screen(t), "Ready. Press Ctrl-C") == 1);
    agentc_tui_test_free(t);

    /* 6. fullscreen harness keeps the alternate screen and no inline redraw */
    t = agentc_tui_test_new(40, 8);
    out = agentc_tui_test_output(t);
    check("fullscreen_alt_screen", contains(out, "\x1b[?1049h"));
    check("fullscreen_no_scrollback", agentc_tui_test_scrollback(t)[0] == 0);
    agentc_tui_test_free(t);
}

/* Mode selection: the spellings accepted by --tui-mode/config, the harness
 * defaults, and the auto/unknown resolution to inline. */
static void test_modes(void) {
    int m = -1;
    check("mode_scrollback", agentc_tui_mode_parse("scrollback", &m) == 0 &&
                                  m == AGENTC_TUI_SCROLLBACK);
    check("mode_inline", agentc_tui_mode_parse("inline", &m) == 0 && m == AGENTC_TUI_INLINE);
    check("mode_fullscreen", agentc_tui_mode_parse("fullscreen", &m) == 0 &&
                                    m == AGENTC_TUI_FULLSCREEN);
    check("mode_auto_inline", agentc_tui_mode_parse("auto", &m) == 0 && m == AGENTC_TUI_INLINE);
    check("mode_unknown", agentc_tui_mode_parse("bogus", &m) != 0);
    check("mode_name_scrollback",
          agentc_streq(agentc_tui_mode_name(AGENTC_TUI_SCROLLBACK), "scrollback"));
    check("mode_name_inline", agentc_streq(agentc_tui_mode_name(AGENTC_TUI_INLINE), "inline"));
    check("mode_name_fullscreen",
          agentc_streq(agentc_tui_mode_name(AGENTC_TUI_FULLSCREEN), "fullscreen"));
    check("mode_name_unknown_inline", agentc_streq(agentc_tui_mode_name(99), "inline"));

    AgcTuiTest *t = agentc_tui_test_new(20, 6);
    check("mode_harness_fullscreen", agentc_tui_test_mode(t) == AGENTC_TUI_FULLSCREEN);
    agentc_tui_test_free(t);
    t = agentc_tui_test_new_mode(20, 6, AGENTC_TUI_INLINE);
    check("mode_harness_inline", agentc_tui_test_mode(t) == AGENTC_TUI_INLINE);
    agentc_tui_test_free(t);
    t = agentc_tui_test_new_mode(20, 6, AGENTC_TUI_SCROLLBACK);
    check("mode_harness_scrollback", agentc_tui_test_mode(t) == AGENTC_TUI_SCROLLBACK);
    agentc_tui_test_free(t);
}

/* The owned-region inline renderer: bottom anchoring, region invariants, the
 * tiny-size degradation, the reflow-aware erase and the mid-frame discard. */
static void test_inline_owned(void) {
    char line[256];

    /* The region flows from the top: on the first frame it is drawn at the
     * cursor just below the prompt (no absolute bottom reservation) and the
     * hardware cursor is parked on the caret. */
    AgcTuiTest *t = agentc_tui_test_new_mode(40, 12, AGENTC_TUI_INLINE);
    agentc_tui_test_frame(t);
    int r0 = 0, cy = 0, off = 0, cx = 0;
    bool rev = false;
    agentc_tui_test_live_metrics(t, &r0, &cy, &off);
    agentc_tui_test_cursor(t, &cx, &cy, &rev);
    check("owned_rows", r0 == 4);
    check("owned_cleared", agentc_tui_test_region_cleared(t) == r0);
    check("owned_cursor_inside", cy >= 0 && cy < r0 && off == 0);
    const char *out = agentc_tui_test_output(t);
    check("owned_frame_cursor",
          contains(out, "\x1b[?25l\x1b[?7l") && contains(out, "\x1b[?25l\x1b[?7h"));
    check("owned_relative_repaint",
          !contains(out, ";1H") && contains(out, "\x1b[1G\x1b[?25l\x1b[?7h"));
    check("owned_cursor_parked", contains(out, "\x1b[3A\x1b[1G\x1b[?25l\x1b[?7h"));
    const char *live = agentc_tui_test_screen(t);
    screen_line(live, 0, line, sizeof line);
    check("owned_top_rule", is_rule_row(line, 40));
    screen_line(live, 1, line, sizeof line);
    check("owned_hint", agentc_streq(line, "Ready. Press Ctrl-C once to clear, twic"));
    screen_line(live, 2, line, sizeof line);
    check("owned_bottom_rule", is_rule_row(line, 40));
    check("owned_footer", contains(live, "ready"));
    dump("inline_owned", t);
    agentc_tui_test_free(t);

    /* Committing a block prints the finished rows at the cursor and then draws
     * the region below them, with no absolute bottom jump: the emitted order is
     * [committed rows][region rows][caret]. */
    t = agentc_tui_test_new_mode(40, 12, AGENTC_TUI_INLINE);
    agentc_tui_test_frame(t);
    agentc_tui_test_feed(t, "hi\r", 3);
    td(t, "hello from the model\n");
    agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
    agentc_tui_test_output_clear(t);
    agentc_tui_test_frame(t);
    out = agentc_tui_test_output(t);
    {
        const char *p_commit = agentc_str_str(out, "hello from the model");
        const char *p_region = agentc_str_str(out, "\xE2\x94\x80\xE2\x94\x80");
        const char *p_caret = agentc_str_str(out, "\x1b[?25l\x1b[?7h");
        check("owned_commit_flows_down",
              p_commit && p_region && p_caret && p_commit < p_region && p_region < p_caret);
    }
    check("owned_commit_relative", !contains(out, ";1H"));
    agentc_tui_test_free(t);

    /* Tiny terminals degrade to a usable composer + status line instead of a
     * corrupt frame: 4 rows keep rule/input/rule/footer, 1 row keeps status. */
    t = agentc_tui_test_new_mode(1, 1, AGENTC_TUI_INLINE);
    live = agentc_tui_test_screen(t);
    check("owned_1x1", agentc_strlen(live) > 0);
    agentc_tui_test_free(t);
    t = agentc_tui_test_new_mode(8, 4, AGENTC_TUI_INLINE);
    live = agentc_tui_test_screen(t);
    screen_line(live, 0, line, sizeof line);
    check("owned_4row_top_rule", is_rule_row(line, 8));
    screen_line(live, 1, line, sizeof line);
    check("owned_4row_input", agentc_streq(line, "Ready."));
    screen_line(live, 2, line, sizeof line);
    check("owned_4row_bottom_rule", is_rule_row(line, 8));
    check("owned_4row_footer", contains(live, "ready"));
    agentc_tui_test_free(t);
    t = agentc_tui_test_new_mode(8, 1, AGENTC_TUI_INLINE);
    live = agentc_tui_test_screen(t);
    check("owned_1row_footer", contains(live, "ready"));
    agentc_tui_test_free(t);
    t = agentc_tui_test_new_mode(1, 8, AGENTC_TUI_INLINE);
    live = agentc_tui_test_screen(t);
    check("owned_1col", agentc_strlen(live) > 0);
    agentc_tui_test_free(t);

    /* Commit once, then erase with the reflow estimate before re-anchoring. */
    t = agentc_tui_test_new_mode(40, 10, AGENTC_TUI_INLINE);
    agentc_tui_test_feed(t, "hi\r", 3);
    td(t, "hello from the model\n");
    agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
    agentc_tui_test_frame(t);
    agentc_tui_test_frame(t);
    check("owned_commit_once",
          count_occurrences(agentc_tui_test_scrollback(t), "hello from the model") == 1);
    check("owned_row_width", agentc_tui_test_region_row_width(t, 0) == 40);
    agentc_tui_test_live_metrics(t, &r0, &cy, &off);
    agentc_tui_test_output_clear(t);
    agentc_tui_test_resize(t, 12, 10);
    out = agentc_tui_test_output(t);
    int expect = 0;
    for (int i = 0; i < r0; i++) {
        int w = agentc_tui_test_region_row_width(t, i);
        expect += (w + 11) / 12;
    }
    if (expect > 10) expect = 10;   /* the erase is capped to the new height */
    check("owned_resize_erase_reflow", count_occurrences(out, "\r\x1b[2K") == expect);
    agentc_tui_test_frame(t);
    check("owned_resize_no_stale_hint",
          count_occurrences(agentc_tui_test_screen(t), "Ready. Press Ctrl-C") <= 1);
    check("owned_resize_commit_once",
          count_occurrences(agentc_tui_test_scrollback(t), "hello from the model") == 1);
    agentc_tui_test_live_metrics(t, &r0, &cy, &off);
    check("owned_resize_reanchor", r0 > 0 && off == 0);
    agentc_tui_test_free(t);

    /* A resize landing mid-frame is discarded; the following frame re-anchors
     * to the new geometry (no frame paints with stale rows). */
    t = agentc_tui_test_new_mode(40, 12, AGENTC_TUI_INLINE);
    agentc_tui_test_frame(t);
    agentc_tui_test_output_clear(t);
    agentc_tui_test_resize_mid_frame(t, 60, 16);
    agentc_tui_test_frame(t);
    check("owned_midframe_erases", count_occurrences(agentc_tui_test_output(t), "\r\x1b[2K") > 0);
    agentc_tui_test_frame(t);
    agentc_tui_test_live_metrics(t, &r0, &cy, &off);
    check("owned_midframe_reanchor", r0 > 0 && off == 0);
    agentc_tui_test_free(t);
}

static void test_theme(void) {
    Theme th;
    theme_init(&th, 1, false);
    check("theme_dark_name", agentc_streq(theme_name(&th), "dark"));
    u32 c = 0;
    check("theme_parse_hex", theme_parse_color("#89b4fa", &c) && c == 0x89B4FA);
    check("theme_parse_short", theme_parse_color("#abc", &c) && c == 0xAABBCC);
    check("theme_parse_bad", !theme_parse_color("nope", &c));
    theme_set_mode(&th, THEME_256);
    AgcBuf b = { 0 };
    theme_emit_sgr(&b, &th, A_BOLD, TH_ACCENT, TH_BG);
    check("theme_sgr_256", contains((const char *)b.p, "38;5;"));
    check("theme_sgr_bold", contains((const char *)b.p, "\x1b[1m"));
    agentc_buf_free(&b);

    /* Dark near-neutrals must stay in the dark end of the 256 palette: the
     * cube jumps from black to #5F5F5F, so they resolve to the greyscale ramp
     * (the base to #262626, the tool tints to #303030). */
    b = (AgcBuf){ 0 };
    theme_emit_sgr(&b, &th, 0, TH_FG, TH_BG);
    check("theme_bg_256_dark", contains((const char *)b.p, "48;5;235"));
    agentc_buf_free(&b);
    b = (AgcBuf){ 0 };
    theme_emit_sgr(&b, &th, 0, TH_FG, TH_TOOL_OK_BG);
    check("theme_tool_ok_bg_256", contains((const char *)b.p, "48;5;236"));
    agentc_buf_free(&b);
    b = (AgcBuf){ 0 };
    theme_emit_sgr(&b, &th, 0, TH_FG, TH_TOOL_ERR_BG);
    check("theme_tool_err_bg_256", contains((const char *)b.p, "48;5;236"));
    agentc_buf_free(&b);
    b = (AgcBuf){ 0 };
    theme_emit_sgr(&b, &th, 0, TH_FG, TH_TOOL_BG);
    check("theme_tool_bg_256", contains((const char *)b.p, "48;5;236"));
    agentc_buf_free(&b);

    /* On a 16-colour terminal the dark tints all land on ANSI black (SGR 40),
     * never on 49: the outcome is still carried by the status foreground. */
    theme_set_mode(&th, THEME_16);
    b = (AgcBuf){ 0 };
    theme_emit_sgr(&b, &th, 0, TH_FG, TH_TOOL_OK_BG);
    check("theme_tool_ok_bg_16", contains((const char *)b.p, "\x1b[40m"));
    agentc_buf_free(&b);
    b = (AgcBuf){ 0 };
    theme_emit_sgr(&b, &th, 0, TH_FG, TH_TOOL_ERR_BG);
    check("theme_tool_err_bg_16", contains((const char *)b.p, "\x1b[40m"));
    agentc_buf_free(&b);
    b = (AgcBuf){ 0 };
    theme_emit_sgr(&b, &th, 0, TH_FG, TH_TOOL_BG);
    check("theme_tool_bg_16", contains((const char *)b.p, "\x1b[40m"));
    agentc_buf_free(&b);

    /* The light palette goes through the same path: its tints are light, so
     * they stay in the cube's top row (256) and resolve to ANSI white (16). */
    theme_init(&th, 0, false);
    theme_set_mode(&th, THEME_256);
    b = (AgcBuf){ 0 };
    theme_emit_sgr(&b, &th, 0, TH_FG, TH_TOOL_OK_BG);
    check("theme_tool_ok_bg_256_light", contains((const char *)b.p, "48;5;231"));
    agentc_buf_free(&b);
    theme_set_mode(&th, THEME_16);
    b = (AgcBuf){ 0 };
    theme_emit_sgr(&b, &th, 0, TH_FG, TH_TOOL_OK_BG);
    check("theme_tool_ok_bg_16_light", contains((const char *)b.p, "\x1b[107m"));
    agentc_buf_free(&b);

    /* A near-white grey must not overflow the 24-entry greyscale ramp into a
     * bogus palette index (rgb 248 used to resolve to 232 + 24 = 256). */
    theme_init(&th, 1, false);
    theme_set_mode(&th, THEME_256);
    th.rgb[TH_FG] = 0xF8F8F8;
    b = (AgcBuf){ 0 };
    theme_emit_sgr(&b, &th, 0, TH_FG, TH_NO_BG);
    {
        const char *tag = agentc_str_str((const char *)b.p, "38;5;");
        int idx = -1;
        if (tag) {
            idx = 0;
            for (const char *q = tag + 5; *q >= '0' && *q <= '9'; q++)
                idx = idx * 10 + (int)(*q - '0');
        }
        check("theme_256_grey_ramp_valid", idx >= 0 && idx <= 255);
    }
    agentc_buf_free(&b);
}

/* -------------------------------------------------- review-fix regressions */

/* A finished block taller than GRID_MAX_ROWS (200) must be committed in bounded
 * chunks: every row printed exactly once, and none left live. Exercises both
 * the inline commit path and the scrollback printer. */
static void test_large_commit(void) {
    for (int mode = 0; mode < 2; mode++) {
        AgcTuiTest *t = agentc_tui_test_new_mode(
            40, 12, mode == 0 ? AGENTC_TUI_INLINE : AGENTC_TUI_SCROLLBACK);
        agentc_tui_test_feed(t, "big\r", 4);
        AgcBuf big = { 0 };
        /* A code block preserves the source lines, so the finished block is
         * really 250 rows tall and crosses GRID_MAX_ROWS. */
        agentc_buf_cstr(&big, "```\n");
        for (int i = 0; i < 250; i++) agentc_buf_printf(&big, "row %03d\n", i);
        agentc_buf_cstr(&big, "```\n");
        AgcTextDelta d = { (const char *)big.p, big.len };
        agentc_tui_test_event(t, AGENTC_EV_TEXT_DELTA, &d);
        agentc_buf_free(&big);
        agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
        agentc_tui_test_frame(t);
        const char *sb = agentc_tui_test_scrollback(t);
        bool once = true;
        for (int i = 0; i < 250; i++) {
            char want[16];
            agentc_snprintf(want, sizeof want, "row %03d", i);
            if (count_occurrences(sb, want) != 1) once = false;
        }
        check(mode == 0 ? "large_commit_every_row_once"
                        : "large_commit_scrollback_every_row_once", once);
        if (mode == 0)
            check("large_commit_live_empty",
                  !contains(agentc_tui_test_screen(t), "row 249"));
        agentc_tui_test_free(t);
    }
}

/* Vertical motion must not land inside a UTF-8 sequence: the byte goal column
 * snaps to a codepoint boundary, so the caret stays visible and typing cannot
 * split a CJK codepoint. */
static void test_unicode_vmove(void) {
    int cx = -1, cy = -1;
    bool rev = false;
    char line[256];
    AgcTuiTest *t = agentc_tui_test_new(24, 8);
    agentc_tui_test_feed(t, "a", 1);
    agentc_tui_test_feed(t, "\x1b\r", 2);                   /* Alt+Enter */
    agentc_tui_test_feed(t, "\xE4\xBD\xA0\xE4\xBD\xA0", 6); /* 你你 */
    agentc_tui_test_feed(t, "\x1b[A", 3);                   /* Up to line 1 */
    agentc_tui_test_feed(t, "\x1b[D\x1b[C", 6);             /* reset goal col */
    agentc_tui_test_feed(t, "\x1b[B", 3);                   /* Down, snap */
    caret(t, &cx, &cy, &rev);
    check("unicode_vmove_boundary_visible", rev && (cx == 0 || cx == 2));
    agentc_tui_test_feed(t, "X", 1);
    screen_line(agentc_tui_test_screen(t), cy, line, sizeof line);
    check("unicode_vmove_no_split", agentc_streq(line, "你X你") ||
                                        agentc_streq(line, "X你你"));
    dump("unicode-vmove", t);
    agentc_tui_test_free(t);
}

/* Streaming markdown: two appends that each end in '\n' form one paragraph;
 * only a real blank line completes it. */
static void test_markdown_stream(void) {
    Markdown m;
    md_init(&m, TH_FG, 0);
    md_set_width(&m, 40);
    size_t nb = 0;
    const MdBlock *b;

    md_append(&m, "line1\n", 6);
    b = md_blocks(&m, &nb);
    check("md_stream_first_open", nb == 1 && !b[0].complete && b[0].nrows == 1);

    md_append(&m, "line2\n", 6);
    b = md_blocks(&m, &nb);
    check("md_stream_one_paragraph", nb == 1 && !b[0].complete && b[0].nrows == 1);

    md_append(&m, "\n", 1);
    b = md_blocks(&m, &nb);
    check("md_stream_blank_completes", nb == 1 && b[0].complete && b[0].nrows == 1);

    md_append(&m, "line3\n\n", 7);
    b = md_blocks(&m, &nb);
    check("md_stream_next_paragraph",
          nb == 2 && b[0].complete && b[1].complete && b[1].nrows == 1);
    md_free(&m);
}

/* The same streaming paragraph through the TUI: two frames, each flushing one
 * delta that ends in '\n', must render as one paragraph (one gap row), not two
 * blocks with a blank row between them. */
static void test_markdown_stream_screen(void) {
    AgcTuiTest *t = agentc_tui_test_new(40, 10);
    agentc_tui_test_feed(t, "m\r", 2);
    td(t, "line1\n");
    agentc_tui_test_frame(t);   /* flush the first append */
    td(t, "line2\n");
    agentc_tui_test_frame(t);   /* second append re-parses the open block */
    dump("markdown-stream", t);
    agentc_tui_test_free(t);
}

/* More than 63 styled runs on one line: the tail is emitted unstyled, never
 * truncated. */
static void test_markdown_many_runs(void) {
    AgcBuf src = { 0 };
    for (int i = 0; i < 70; i++) {
        agentc_buf_byte(&src, '`');
        agentc_buf_byte(&src, (u8)('a' + i % 26));
        agentc_buf_byte(&src, '`');
    }
    agentc_buf_byte(&src, '\n');
    Markdown m;
    md_init(&m, TH_FG, 0);
    md_set_width(&m, 400);
    md_append(&m, (const char *)src.p, src.len);
    size_t nb = 0;
    const MdBlock *b = md_blocks(&m, &nb);
    bool kept = nb == 1 && b[0].nrows == 1 && b[0].rows[0].line.len > 63;
    if (kept) {
        const char *line = (const char *)b[0].rows[0].line.p;
        /* The tail keeps the backticks the styled runs stripped, so any '`' in
         * the rendered row proves the remainder was emitted, not truncated. */
        kept = agentc_str_str(line, "`") != NULL;
    }
    check("md_many_runs_tail_kept", kept);
    md_free(&m);
    agentc_buf_free(&src);
}

/* A code block ends only on its own opening fence char: a `~~~` line inside a
 * backtick fence is content, not a closing fence. */
static void test_markdown_fence_char(void) {
    Markdown m;
    md_init(&m, TH_FG, 0);
    md_set_width(&m, 40);
    const char *src = "```\n~~~\nstill inside\n```\n";
    md_append(&m, src, agentc_strlen(src));
    size_t nb = 0;
    const MdBlock *b = md_blocks(&m, &nb);
    bool ok = nb == 1 && b[0].kind == MD_CODE && b[0].nrows == 2;
    if (ok) {
        const char *r0 = (const char *)b[0].rows[0].line.p;
        const char *r1 = (const char *)b[0].rows[1].line.p;
        ok = agentc_streq(r0, "~~~") && agentc_streq(r1, "still inside");
    }
    check("md_code_other_fence_kept", ok);
    md_free(&m);
}

/* A tool name longer than the header buffer must clip, not overflow it. */
static void test_long_tool_name(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 10);
    agentc_tui_test_feed(t, "go\r", 3);
    char name[600];
    agentc_memset(name, 'a', sizeof name - 1);
    name[sizeof name - 1] = 0;
    tool_start(t, name, "{}");
    agentc_tui_test_frame(t);
    check("long_tool_name_clipped", contains(agentc_tui_test_screen(t), "[aaa"));
    agentc_tui_test_free(t);
}

/* A trailing wide glyph occupies both columns; the recorded owned-row width
 * (the resize reflow input) must include the continuation cell. */
static void test_wide_row_width(void) {
    AgcTuiTest *t = agentc_tui_test_new_mode(20, 8, AGENTC_TUI_INLINE);
    agentc_tui_test_feed(t, "\xE4\xBD\xA0\xE5\xA5\xBD", 6); /* 你好 */
    agentc_tui_test_feed(t, "\x1b[D", 3);                  /* caret onto 好 */
    agentc_tui_test_screen(t);
    check("wide_row_width", agentc_tui_test_region_row_width(t, 1) == 4);
    agentc_tui_test_free(t);
}

/* PageUp/PageDown must page in inline mode by the recorded viewport height. */
static void test_inline_paging(void) {
    AgcTuiTest *t = agentc_tui_test_new_mode(40, 10, AGENTC_TUI_INLINE);
    agentc_tui_test_set_running(t, true);
    AgcBuf big = { 0 };
    /* A code block preserves source lines, so each L%02d is exactly one owned
     * row and the paged window is deterministic. */
    agentc_buf_cstr(&big, "```\n");
    for (int i = 0; i < 60; i++) agentc_buf_printf(&big, "L%02d\n", i);
    agentc_buf_cstr(&big, "```\n");
    AgcTextDelta d = { (const char *)big.p, big.len };
    agentc_tui_test_event(t, AGENTC_EV_TEXT_DELTA, &d);
    agentc_buf_free(&big);
    agentc_tui_test_frame(t);
    const char *live = agentc_tui_test_screen(t);
    check("inline_page_bottom", contains(live, "L59") && !contains(live, "L54"));
    agentc_tui_test_feed(t, "\x1b[5~", 4);   /* PageUp */
    live = agentc_tui_test_screen(t);
    check("inline_page_up", !contains(live, "L59") && contains(live, "L54"));
    agentc_tui_test_feed(t, "\x1b[6~", 4);   /* PageDown */
    live = agentc_tui_test_screen(t);
    check("inline_page_down", contains(live, "L59"));
    agentc_tui_test_free(t);
}

/* Zero-width formatting codepoints must not spend a column. */
static void test_zero_width(void) {
    check("wcwidth_zwsp", agentc_wcwidth(0x200B) == 0);
    check("wcwidth_zwnj", agentc_wcwidth(0x200C) == 0);
    check("wcwidth_zwj", agentc_wcwidth(0x200D) == 0);
    check("wcwidth_bom", agentc_wcwidth(0xFEFF) == 0);
    check("wcwidth_word_joiner", agentc_wcwidth(0x2060) == 0);
    check("wcwidth_soft_hyphen", agentc_wcwidth(0x00AD) == 0);
    check("text_width_skip_zwj", agentc_text_width("a\xE2\x80\x8D" "b") == 2);
    /* Emoji ranges between the old 0x1F64F and 0x1F900 gaps are wide. */
    check("wcwidth_emoji_transport", agentc_wcwidth(0x1F680) == 2);
    check("wcwidth_emoji_colored_shape", agentc_wcwidth(0x1F7E0) == 2);
    /* Mahjong/domino/playing-card tiles and enclosed ideographs are wide. */
    check("wcwidth_mahjong", agentc_wcwidth(0x1F004) == 2);
    check("wcwidth_enclosed_ideograph", agentc_wcwidth(0x1F200) == 2);
    /* A combining mark after a wide glyph attaches to the base cell, not the
     * CELL_CONT continuation cell that the renderer skips. */
    {
        Grid g;
        grid_init(&g, 10, 1);
        grid_put(&g, 0, 0, 0, TH_FG, TH_NO_BG, "\xE5\xA5\xBD\xCC\x81", 5);
        Cell *base = grid_at(&g, 0, 0);
        Cell *cont = grid_at(&g, 1, 0);
        check("wide_combining_base", base && base->cp == 0x597D && base->comb == 0x0301);
        check("wide_combining_cont", cont && cont->cp == CELL_CONT);
        grid_free(&g);
    }
}

/* The footer's right slot is placed by display width, not byte count. */
static void test_footer_width(void) {
    Grid g;
    grid_init(&g, 20, 1);
    Theme th;
    theme_init(&th, 1, false);
    AgcStatusValue segs[2];
    agentc_memset(segs, 0, sizeof segs);
    segs[0].slot = AGENTC_PSEG_SLOT_LEFT;
    agentc_snprintf(segs[0].text, sizeof segs[0].text, "L");
    segs[1].slot = AGENTC_PSEG_SLOT_RIGHT;
    agentc_snprintf(segs[1].text, sizeof segs[1].text, "\xE6\x97\xA5\xE6\x9C\xAC"); /* 日本 */
    comp_footer(&g, &th, 0, 20, segs, 2);
    Cell *c0 = grid_at(&g, 16, 0);
    Cell *c1 = grid_at(&g, 17, 0);
    Cell *c2 = grid_at(&g, 18, 0);
    Cell *cl = grid_at(&g, 0, 0);
    check("footer_right_wide_start", c0 && c0->cp == 0x65E5);
    check("footer_right_wide_cont", c1 && c1->cp == CELL_CONT);
    check("footer_right_wide_end", c2 && c2->cp == 0x672C);
    check("footer_left_clear", cl && cl->cp == 'L');
    grid_free(&g);
}

/* A scrollback resize must recompute the committed boundary from the block
 * layout so nothing is appended (reprinted) after the width changes. */
static void test_scrollback_resize(void) {
    AgcTuiTest *t = agentc_tui_test_new_mode(40, 12, AGENTC_TUI_SCROLLBACK);
    agentc_tui_test_feed(t, "hi\r", 3);
    td(t, "alpha bravo charlie delta echo foxtrot golf hotel india\n\n");
    agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
    agentc_tui_test_frame(t);
    size_t before = agentc_strlen(agentc_tui_test_scrollback(t));
    check("scrollback_resize_committed", before > 0);
    agentc_tui_test_resize(t, 20, 12);
    agentc_tui_test_frame(t);
    size_t after = agentc_strlen(agentc_tui_test_scrollback(t));
    check("scrollback_resize_no_reprint", after == before);
    agentc_tui_test_free(t);
}

/* A still-streaming block is never partially committed, so changing the width
 * cannot add a stale old-width row count to the new layout and reprint the
 * transcript. */
static void test_scrollback_stream_resize(void) {
    AgcTuiTest *t = agentc_tui_test_new_mode(40, 12, AGENTC_TUI_SCROLLBACK);
    agentc_tui_test_set_running(t, true);
    AgcBuf big = { 0 };
    agentc_buf_cstr(&big, "ALPHASTART alpha bravo charlie delta echo foxtrot golf\n");
    for (int i = 0; i < 30; i++) agentc_buf_printf(&big, "filler line %02d\n", i);
    AgcTextDelta d = { (const char *)big.p, big.len };
    agentc_tui_test_event(t, AGENTC_EV_TEXT_DELTA, &d);
    agentc_buf_free(&big);
    agentc_tui_test_frame(t);
    check("scrollback_stream_resize_none_early",
          !contains(agentc_tui_test_scrollback(t), "ALPHASTART"));
    agentc_tui_test_resize(t, 20, 12);
    agentc_tui_test_frame(t);
    check("scrollback_stream_resize_no_stale",
          !contains(agentc_tui_test_scrollback(t), "ALPHASTART"));
    agentc_tui_test_set_running(t, false);
    agentc_tui_test_frame(t);
    check("scrollback_stream_resize_commit_once",
          count_occurrences(agentc_tui_test_scrollback(t), "ALPHASTART") == 1);
    agentc_tui_test_free(t);
}

/* ------------------------------------------- prompt/skill/theme dispatch */

static void test_theme_names(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 12);
    agentc_test_setenv("COLORFGBG", "0;15");
    const char *c = "/theme dark\r";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    check("theme_name.dark", contains(agentc_tui_test_screen(t), "theme: dark"));
    c = "/theme light\r";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    check("theme_name.light", contains(agentc_tui_test_screen(t), "theme: light"));
    c = "/theme sol\r";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    check("theme_name.named", contains(agentc_tui_test_screen(t), "theme: sol"));
    c = "/theme nope\r";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    check("theme_name.unknown", contains(agentc_tui_test_screen(t), "theme: unknown 'nope'"));
    agentc_test_setenv("COLORFGBG", NULL);
    agentc_tui_test_free(t);
}

static void test_skill_commands(void) {
    AgcTuiTest *t = agentc_tui_test_new(56, 16);
    const char *c = "/skill:demo run this\r";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    const char *screen = agentc_tui_test_screen(t);
    check("skill_command.body", contains(screen, "skill body here") && contains(screen, "run this"));
    check("skill_command.no_frontmatter", !contains(screen, "description: Demo skill"));
    c = "/skill:nope\r";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    check("skill_command.unknown", contains(agentc_tui_test_screen(t), "unknown skill 'nope'"));
    c = "/skill:big\r";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    check("skill_command.over_cap", contains(agentc_tui_test_screen(t), "too large"));
    /* an empty name is a clear notice, never a submitted message */
    c = "/skill:\r";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    screen = agentc_tui_test_screen(t);
    check("skill_command.empty_name_notice", contains(screen, "missing skill name"));
    check("skill_command.empty_name_not_submitted", !contains(screen, "> /skill:"));
    /* a registered prompt in the reserved namespace is not reachable through
     * /skill:; it goes to the skill handler like any other skill name */
    c = "/skill:x\r";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    screen = agentc_tui_test_screen(t);
    check("skill_command.reserved_unknown",
          contains(screen, "unknown skill 'x'") && !contains(screen, "fixture:x"));
    agentc_tui_test_free(t);
}

static void test_prompt_commands(void) {
    AgcTuiTest *t = agentc_tui_test_new(56, 16);
    const char *c = "/hi Alice\r";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    const char *screen = agentc_tui_test_screen(t);
    check("prompt_command.file", contains(screen, "hello Alice"));
    check("prompt_command.once", !contains(screen, "/hi Alice"));
    c = "/greet Bob\r";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    check("prompt_command.registered", contains(agentc_tui_test_screen(t), "fixture:Bob"));

    /* the menu lists live prompts after the eight built-ins; a prompt shadowed
     * by a built-in does not add a second entry. The eight built-ins fill the
     * 8-row window, so the prompts need a scroll to come into view. */
    agentc_tui_test_feed(t, "/h", 2);
    screen = agentc_tui_test_screen(t);
    check("prompt_command.shadow",
          count_occurrences(screen, "/help") == 1 && contains(screen, "/hi"));
    agentc_tui_test_feed(t, "\x7f", 1);   /* back to "/" for the full list */
    for (int i = 0; i < 8; i++) {
        c = "\x1b[B";
        agentc_tui_test_feed(t, c, 3);
    }
    screen = agentc_tui_test_screen(t);
    check("prompt_command.menu_hi", contains(screen, "/hi") && contains(screen, "Hi fixture"));
    c = "\x1b[B";
    agentc_tui_test_feed(t, c, 3);
    screen = agentc_tui_test_screen(t);
    check("prompt_command.menu_greet",
          contains(screen, "/greet") && contains(screen, "greet fixture"));
    agentc_tui_test_free(t);

    /* the skill: namespace is reserved: a registered prompt named skill:x is
     * routed to the skill handler, so the menu must not advertise it */
    t = agentc_tui_test_new(56, 16);
    c = "/skill";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    check("prompt_command.reserved_hidden",
          !contains(agentc_tui_test_screen(t), "reserved fixture"));
    agentc_tui_test_free(t);

    /* dispatch precedence: /help runs the built-in, not the shadowed prompt */
    t = agentc_tui_test_new(56, 16);
    c = "/help\r";
    agentc_tui_test_feed(t, c, agentc_strlen(c));
    screen = agentc_tui_test_screen(t);
    check("prompt_command.builtin_wins",
          contains(screen, "Ctrl+A/E line ends") && !contains(screen, "fixture:"));
    agentc_tui_test_free(t);
}

/* --------------------------------------------------- review-fix regressions */

/* A compaction event must reach the callback the front end installed: the TUI
 * owns no session, so dropping it left the session file's flush index stale and
 * skipped the first post-compaction message. */
static void compact_record(void *ud, const AgcCompactInfo *ci) {
    AgcCompactInfo *got = ud;
    *got = *ci;
}

static void test_compact_event(void) {
    AgcTuiTest *t = agentc_tui_test_new(40, 8);
    AgcCompactInfo got;
    agentc_memset(&got, 0, sizeof got);
    agentc_tui_test_set_compact_hook(t, compact_record, &got);
    AgcCompactInfo ci = { 1234, 7, true, "summary" };
    agentc_tui_test_event(t, AGENTC_EV_COMPACT, &ci);
    check("compact_hook_called",
          got.tokens_before == 1234 && got.kept_messages == 7 && got.automatic &&
              agentc_streq(got.summary, "summary"));
    agentc_tui_test_free(t);
}

/* Parallel tools finish out of order: the commit boundary must stop at the
 * FIRST still-live block. The old last-block-only rule committed a running tool
 * and then reprinted shifted rows, losing its output once and duplicating
 * another block. */
static void test_parallel_tools(void) {
    AgcTuiTest *t = agentc_tui_test_new_mode(60, 20, AGENTC_TUI_INLINE);
    agentc_tui_test_feed(t, "go\r", 3);
    tool_start(t, "AAA", "{\"x\":1}");
    tool_start(t, "BBB", "{\"x\":2}");
    tool_end(t, "BBB", false, "B-alpha\nB-bravo\nB-charlie", 5);
    agentc_tui_test_frame(t);
    tool_end(t, "AAA", false, "A-alpha\nA-bravo\nA-charlie", 5);
    agentc_tui_test_frame(t);
    const char *sb = agentc_tui_test_scrollback(t);
    check("parallel_tools_A_once",
          count_occurrences(sb, "A-alpha") == 1 &&
              count_occurrences(sb, "A-bravo") == 1 &&
              count_occurrences(sb, "A-charlie") == 1);
    check("parallel_tools_B_once",
          count_occurrences(sb, "B-alpha") == 1 &&
              count_occurrences(sb, "B-bravo") == 1 &&
              count_occurrences(sb, "B-charlie") == 1);
    agentc_tui_test_free(t);
}

static void test_parallel_tools_scrollback(void) {
    AgcTuiTest *t = agentc_tui_test_new_mode(60, 20, AGENTC_TUI_SCROLLBACK);
    agentc_tui_test_feed(t, "go\r", 3);
    tool_start(t, "AAA", "{\"x\":1}");
    tool_start(t, "BBB", "{\"x\":2}");
    tool_end(t, "BBB", false, "B-alpha\nB-bravo\nB-charlie", 5);
    agentc_tui_test_frame(t);
    /* AAA is still running when BBB finished: its rows must not be committed,
     * or the later growth would shift BBB and re-commit the wrong rows. */
    check("scroll_parallel_no_early_A",
          !contains(agentc_tui_test_scrollback(t), "AAA"));
    tool_end(t, "AAA", false, "A-alpha\nA-bravo\nA-charlie", 5);
    agentc_tui_test_frame(t);
    const char *sb = agentc_tui_test_scrollback(t);
    check("scroll_parallel_A_once",
          count_occurrences(sb, "A-alpha") == 1 &&
              count_occurrences(sb, "A-bravo") == 1 &&
              count_occurrences(sb, "A-charlie") == 1);
    check("scroll_parallel_B_once",
          count_occurrences(sb, "B-alpha") == 1 &&
              count_occurrences(sb, "B-bravo") == 1 &&
              count_occurrences(sb, "B-charlie") == 1);
    agentc_tui_test_free(t);
}

/* Ctrl+O on a tool card that is already committed must be ignored: expanding
 * it would change its height under the row boundary and the next frame would
 * append only the delta, leaving both the collapsed and expanded card in
 * scrollback. */
static void test_toggle_committed_tool(void) {
    AgcTuiTest *t = agentc_tui_test_new_mode(60, 16, AGENTC_TUI_SCROLLBACK);
    agentc_tui_test_feed(t, "run\r", 4);
    tool_start(t, "read", "{\"path\":\"a.c\"}");
    tool_end(t, "read", false,
             "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\n",
             5);
    agentc_tui_test_frame(t);
    AgcBuf before = { 0 };
    agentc_buf_cstr(&before, agentc_tui_test_scrollback(t));
    check("ctrl_o_committed_card", before.len > 0 && contains((const char *)before.p, "[read]"));
    agentc_tui_test_feed(t, "\x0f", 1);   /* Ctrl+O */
    agentc_tui_test_frame(t);
    const char *after = agentc_tui_test_scrollback(t);
    check("ctrl_o_committed_unchanged",
          agentc_strlen(after) == before.len && agentc_memeq(after, before.p, before.len));
    agentc_buf_free(&before);
    agentc_tui_test_free(t);
}

/* A caret sitting on the codepoint that wraps must stay inside the input rows:
 * the cur_row pass used to skip that codepoint, under-scroll the composer by a
 * row and push the caret onto the bottom rule. */
static void test_cursor_wrap_boundary(void) {
    AgcTuiTest *t = agentc_tui_test_new(20, 6);
    for (int i = 0; i < 39; i++) agentc_tui_test_feed(t, "a", 1);
    agentc_tui_test_feed(t, "\x1b[D", 3);   /* caret before the wrapping 'a' */
    int cx = -1, cy = -1;
    bool rev = false;
    caret(t, &cx, &cy, &rev);
    check("cursor_wrap_in_input", rev && cy == 3);
    agentc_tui_test_free(t);
}

/* A caret parked on a newline sits at the column where the newline ends the
 * row (col), not one left of it: the zero-width decrement is only for a
 * combining mark attached to the preceding cell. The fallback reverse cell is
 * written at col, so the reported cursor and the visible caret must agree. */
static void test_cursor_newline(void) {
    AgcTuiTest *t = agentc_tui_test_new(40, 8);
    agentc_tui_test_feed(t, "ab", 2);
    agentc_tui_test_feed(t, "\x1b\r", 2);   /* Alt+Enter: insert a newline */
    agentc_tui_test_feed(t, "\x1b[D", 3);  /* left onto the newline */
    int cx = -1, cy = -1;
    bool rev = false;
    caret(t, &cx, &cy, &rev);
    check("cursor_newline_at_end_of_row", rev && cx == 2);
    agentc_tui_test_free(t);
}

/* Paste markers must restart with the paste vector: after a submit clears the
 * bodies, the next large paste has to expand instead of emitting a literal
 * marker whose index is past the fresh vector. */
static void paste_big(AgcTuiTest *t, const char *tag) {
    AgcBuf b = { 0 };
    agentc_buf_cstr(&b, "\x1b[200~");
    for (int i = 0; i < 12; i++) agentc_buf_printf(&b, "%s line %d\n", tag, i);
    agentc_buf_cstr(&b, "\x1b[201~");
    agentc_tui_test_feed(t, (const char *)b.p, b.len);
    agentc_buf_free(&b);
}

static void test_paste_reuse(void) {
    AgcTuiTest *t = agentc_tui_test_new(60, 16);
    paste_big(t, "firstp");
    agentc_tui_test_feed(t, "\r", 1);
    agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
    paste_big(t, "secondp");
    agentc_tui_test_feed(t, "\r", 1);
    const char *screen = agentc_tui_test_screen(t);
    check("paste_reuse_second", contains(screen, "secondp line 11"));
    check("paste_reuse_no_marker", !contains(screen, "[Pasted text #"));
    agentc_tui_test_free(t);
}

/* A paste marker whose index is a long digit run must not overflow the signed
 * accumulator: it stays literal instead of indexing a nonexistent body. */
static void test_paste_marker_overflow(void) {
    Editor e;
    editor_init(&e);
    const char *marker = "[Pasted text #99999999999999999999999999 1 lines]";
    editor_set(&e, marker);
    char *out = editor_take(&e);
    check("paste_marker_overflow_literal", agentc_streq(out, marker));
    agentc_free(out);
    editor_free(&e);
}

/* A tab has width 0 in agentc_wcwidth but grid_put expands it to the next
 * 4-column stop, so a raw tab would desynchronize the wrap/caret math from the
 * rendered cells. The editor normalizes tabs on entry instead. */
static void test_tab_expansion(void) {
    Editor e;
    editor_init(&e);
    editor_set(&e, "a\tb");
    check("tab_expand_basic", agentc_streq(editor_text(&e), "a   b"));
    editor_set(&e, "abcd\te");
    check("tab_expand_stop", agentc_streq(editor_text(&e), "abcd    e"));
    editor_set(&e, "\tx");
    check("tab_expand_leading", agentc_streq(editor_text(&e), "    x"));
    editor_set(&e, "\xE4\xBD\xA0\tz"); /* a wide glyph advances two columns */
    check("tab_expand_after_wide", agentc_streq(editor_text(&e), "\xE4\xBD\xA0  z"));
    editor_free(&e);

    /* The bracketed-paste path normalizes too, so the caret lands after the
     * expanded text rather than at the raw tab's zero-width position. */
    AgcTuiTest *t = agentc_tui_test_new(40, 8);
    const char *paste = "\x1b[200~a\tb\x1b[201~";
    agentc_tui_test_feed(t, paste, agentc_strlen(paste));
    int cx = -1, cy = -1;
    bool rev = false;
    caret(t, &cx, &cy, &rev);
    check("paste_tab_caret_aligned", rev && cx == 5);
    agentc_tui_test_free(t);
}

/* While scrolled up in a streaming block, new rows must not slide the window
 * down: the scroll offset grows with the max scrollable amount, exactly like
 * chat_render() does for fullscreen. */
static void test_scroll_anchor(void) {
    AgcTuiTest *t = agentc_tui_test_new_mode(40, 10, AGENTC_TUI_INLINE);
    agentc_tui_test_set_running(t, true);
    AgcBuf big = { 0 };
    agentc_buf_cstr(&big, "```\n");
    for (int i = 0; i < 30; i++) agentc_buf_printf(&big, "S%02d\n", i);
    AgcTextDelta d = { (const char *)big.p, big.len };
    agentc_tui_test_event(t, AGENTC_EV_TEXT_DELTA, &d);
    agentc_buf_free(&big);
    agentc_tui_test_frame(t);
    agentc_tui_test_feed(t, "\x1b[5~", 4);   /* PageUp */
    const char *s0 = agentc_tui_test_screen(t);
    char top0[128];
    screen_line(s0, 0, top0, sizeof top0);
    AgcBuf more = { 0 };
    for (int i = 30; i < 40; i++) agentc_buf_printf(&more, "S%02d\n", i);
    AgcTextDelta d2 = { (const char *)more.p, more.len };
    agentc_tui_test_event(t, AGENTC_EV_TEXT_DELTA, &d2);
    agentc_buf_free(&more);
    agentc_tui_test_frame(t);
    const char *s1 = agentc_tui_test_screen(t);
    char top1[128];
    screen_line(s1, 0, top1, sizeof top1);
    check("scroll_anchor_top_stable", agentc_streq(top0, top1));
    agentc_tui_test_free(t);
}

/* A frame discarded by a mid-frame resize must not leave rows marked committed
 * or mirror text appended: those bytes were never written to the terminal. */
static void test_midframe_commit_rollback(void) {
    AgcTuiTest *t = agentc_tui_test_new_mode(40, 10, AGENTC_TUI_INLINE);
    agentc_tui_test_feed(t, "hi\r", 3);
    td(t, "rollback text\n");
    agentc_tui_test_event(t, AGENTC_EV_AGENT_END, NULL);
    agentc_tui_test_resize_mid_frame(t, 60, 14);
    agentc_tui_test_frame(t);   /* discarded frame: nothing may be recorded */
    check("midframe_commit_not_recorded",
          !contains(agentc_tui_test_scrollback(t), "rollback text"));
    agentc_tui_test_frame(t);
    check("midframe_commit_after_retry",
          count_occurrences(agentc_tui_test_scrollback(t), "rollback text") == 1);
    agentc_tui_test_free(t);
}

/* A buffer ending in a truncated UTF-8 sequence (reachable through bracketed
 * paste) must be measured and drawn the same way: both passes decode the lone
 * lead byte as one U+FFFD. The old visual-rows pass kept a partial codepoint
 * and counted it as zero width, under-allocating a row and scrolling the first
 * text row out of the composer. */
static void test_truncated_utf8(void) {
    Editor e;
    editor_init(&e);
    editor_set(&e, "ab\xE3");
    int rows = editor_visual_rows(&e, 3);   /* inner width 2: "ab" then FFFD wraps */
    check("truncated_utf8_rows", rows == 4);

    Grid g;
    grid_init(&g, 3, rows);
    int cx = -1, cy = -1;
    editor_render(&e, &g, NULL, 0, 0, 3, rows, NULL, &cx, &cy);
    Cell *first = grid_at(&g, 0, 1);   /* row below the top rule */
    check("truncated_utf8_first_row_visible", first && first->cp == 'a');
    check("truncated_utf8_caret_in_input", cx == 1 && cy >= 1 && cy <= 2);
    grid_free(&g);
    editor_free(&e);
}

/* A lone LF after the parser went idle (or after ESC broke the pair) is its own
 * Enter, not the second half of a CRLF: last_cr must not survive the idle
 * window, or a later Ctrl+J is swallowed. */
static void test_input_last_cr(void) {
    Input in;
    input_init(&in);
    input_feed(&in, (const u8 *)"\r", 1, 0, NULL, NULL);
    check("input_last_cr_set", in.last_cr);
    input_idle(&in, 100 * 1000000, 50, NULL, NULL);
    check("input_last_cr_idle_cleared", !in.last_cr);

    /* ESC resets the pair too, even before the idle timeout fires. */
    input_feed(&in, (const u8 *)"\r", 1, 0, NULL, NULL);
    input_feed(&in, (const u8 *)"\x1b", 1, 0, NULL, NULL);
    check("input_last_cr_esc_cleared", !in.last_cr);
    input_free(&in);
}

/* A history file saved with CRLF line endings must not leak the '\r' into the
 * composer: the loaded entry is trimmed to the bare line. */
static void test_history_crlf(void) {
    const char *path = "/tmp/agentc-tui-history-crlf";
    os_unlink(path);
    int fd = os_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC, 0644);
    if (fd >= 0) {
        const char *hist = "hello\r\nworld\r\n";
        (void)os_write(fd, hist, agentc_strlen(hist));
        os_close(fd);
    }
    Editor e;
    editor_init(&e);
    editor_history_load(&e, path);
    Key up;
    agentc_memset(&up, 0, sizeof up);
    up.code = K_UP;
    editor_key(&e, &up);   /* newest entry */
    editor_key(&e, &up);   /* older entry */
    check("history_crlf_trimmed", agentc_streq(editor_text(&e), "hello"));
    editor_free(&e);
    os_unlink(path);
}

/* The startup banner: one dim + muted line emitted before the first frame, so
 * it lands in the terminal's scrollback and is never part of a redraw. */
static void test_banner(void) {
    AgcTuiTest *t = agentc_tui_test_new(40, 10);
    agentc_tui_test_output_clear(t);
    agentc_tui_test_print_banner(t);
    const char *out = agentc_tui_test_output(t);
    check("banner_once", count_occurrences(out, "agentc " AGENTC_VERSION) == 1);
    check("banner_dim_muted", contains(out, "\x1b[2m") &&
          contains(out, "38;2;127;132;156") && !contains(out, "48;2;"));
    agentc_tui_test_frame(t);
    const char *all = agentc_tui_test_output(t);
    const char *b = agentc_str_str(all, "agentc " AGENTC_VERSION);
    const char *fr = agentc_str_str(all, "\x1b[?2026h");
    check("banner_before_first_frame", b && fr && b < fr);
    check("banner_not_repeated", count_occurrences(all, "agentc " AGENTC_VERSION) == 1);
    agentc_tui_test_free(t);
}

/* The banner carries the resolved core-tool summary on the line right after
 * the version, in the same muted style, and it stays succinct (binary names,
 * no backend descriptions). The engine is pinned to its built-ins so the probe
 * does not depend on the host PATH. */
static void test_banner_tools(void) {
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_INTERNAL, "");
    AgcTuiTest *t = agentc_tui_test_new(48, 10);
    agentc_tui_test_set_show_tools(t, true);
    agentc_tui_test_output_clear(t);
    agentc_tui_test_print_banner(t);
    const char *out = agentc_tui_test_output(t);
    const char *ver = agentc_str_str(out, "agentc " AGENTC_VERSION);
    const char *sum = agentc_str_str(out, "grep->built-in find->built-in ls->built-in");
    check("banner_tools_after_version", ver && sum && ver < sum);
    check("banner_tools_prefixed", contains(out, "tools: grep->"));
    check("banner_tools_succinct", !contains(out, "Rust regex") && !contains(out, "(glob)"));
    agentc_tui_test_free(t);
    agentc_tool_engine_reset();
}

/* --no-tools (or an otherwise empty selection) must not advertise engines the
 * model cannot call, so show_tools=false drops the summary line entirely. */
static void test_banner_no_tools(void) {
    agentc_tool_engine_init(AGENTC_TOOL_ENGINE_INTERNAL, "");
    AgcTuiTest *t = agentc_tui_test_new(48, 10);
    agentc_tui_test_set_show_tools(t, false);
    agentc_tui_test_output_clear(t);
    agentc_tui_test_print_banner(t);
    const char *out = agentc_tui_test_output(t);
    check("banner_no_tools_hides_summary", contains(out, "agentc " AGENTC_VERSION) &&
          !contains(out, "grep->") && !contains(out, "ls->"));
    agentc_tui_test_free(t);
    agentc_tool_engine_reset();
}

/* /thinking reports the configured level, sets a valid one, and rejects an
 * invalid one with a notice and no change. The status line follows the
 * configured level, not the transient stream state. */
static void test_thinking_command(void) {
    AgcTuiTest *t = agentc_tui_test_new(48, 12);
    const char *cmd;
    cmd = "/thinking\r";
    agentc_tui_test_feed(t, cmd, agentc_strlen(cmd));
    check("thinking_report_default", contains(agentc_tui_test_screen(t), "thinking: off"));
    cmd = "/thinking high\r";
    agentc_tui_test_feed(t, cmd, agentc_strlen(cmd));
    cmd = "hi\r";
    agentc_tui_test_feed(t, cmd, agentc_strlen(cmd));
    check("thinking_set_high", contains(agentc_tui_test_screen(t), "think:high"));
    cmd = "/thinking bogus\r";
    agentc_tui_test_feed(t, cmd, agentc_strlen(cmd));
    check("thinking_invalid_notice",
          contains(agentc_tui_test_screen(t), "thinking: unknown 'bogus'"));
    check("thinking_invalid_no_change",
          contains(agentc_tui_test_screen(t), "think:high"));
    cmd = "/thinking\r";
    agentc_tui_test_feed(t, cmd, agentc_strlen(cmd));
    check("thinking_report_after_set",
          contains(agentc_tui_test_screen(t), "thinking: high"));
    agentc_tui_test_free(t);
}

/* 16-colour downgrade of the placeholder: dim + the muted slot must be the
 * gray palette entry (bright black, 90), never white. */
static void test_hint_16color(void) {
    Theme th;
    theme_init(&th, 1, false);
    theme_set_mode(&th, THEME_16);
    Editor e;
    editor_init(&e);
    Grid g;
    grid_init(&g, 40, 1);
    int cx = -1, cy = -1;
    editor_render(&e, &g, &th, 0, 0, 40, 1, "Ready. X", &cx, &cy);
    AgcBuf b = { 0 };
    render_rows_ansi(&g, 0, 1, &b, &th, false);
    const char *bytes = b.p ? (const char *)b.p : "";
    check("hint16_dim", contains(bytes, "\x1b[2m"));
    check("hint16_muted_gray", contains(bytes, "\x1b[90m"));
    check("hint16_not_white", !contains(bytes, "\x1b[37m") && !contains(bytes, "\x1b[97m"));
    agentc_buf_free(&b);
    grid_free(&g);
    editor_free(&e);
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    tui_fixture_setup();

    /* The Windows bootstrap's argv/envp allocations are permanent; compare
     * against that baseline so this still catches a TUI leak there too. */
    size_t mem_base = agentc_mem_live();
    test_basics();
    test_typing();
    test_composer();
    test_empty_hint();
    test_command_menu();
    test_model_picker();
    test_pick();
    test_thinking_picker();
    test_new_and_compact();
    test_multiline();
    test_editing();
    test_readline();
    test_escape_disambiguation();
    test_cursor();
    test_wrap();
    test_markdown();
    test_thinking();
    test_thinking_off_hidden();
    test_thinking_on_shown();
    test_msg_reset();
    test_scrollback();
    test_inline_owned();
    test_modes();
    test_tools();
    test_tool_error();
    test_diff();
    test_tool_bg();
    test_footer();
    test_scroll();
    test_abort();
    test_ctrl_c();
    test_ctrl_c_semantics();
    test_resize();
    test_paste();
    test_history();
    test_completion();
    test_unicode();
    test_unicode_invalid();
    test_sanitizer();
    test_commands();
    test_theme();
    test_large_commit();
    test_unicode_vmove();
    test_markdown_stream();
    test_markdown_stream_screen();
    test_markdown_many_runs();
    test_markdown_fence_char();
    test_long_tool_name();
    test_wide_row_width();
    test_inline_paging();
    test_zero_width();
    test_footer_width();
    test_scrollback_resize();
    test_scrollback_stream_resize();
    test_theme_names();
    test_skill_commands();
    test_prompt_commands();
    test_compact_event();
    test_parallel_tools();
    test_parallel_tools_scrollback();
    test_toggle_committed_tool();
    test_cursor_wrap_boundary();
    test_cursor_newline();
    test_paste_reuse();
    test_paste_marker_overflow();
    test_tab_expansion();
    test_truncated_utf8();
    test_history_crlf();
    test_input_last_cr();
    test_scroll_anchor();
    test_midframe_commit_rollback();
    test_banner();
    test_banner_tools();
    test_banner_no_tools();
    test_thinking_command();
    test_hint_16color();

    check("tui_mem_live", agentc_mem_live() == mem_base);
    agentc_outf("tui tests: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
