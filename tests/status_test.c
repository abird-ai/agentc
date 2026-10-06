/* status_test.c — the status-line provider contract.
 *
 * Pins the properties the refactor promised: a built-in-only line is byte for
 * byte the old footer, extension segments land in the right slot and priority
 * order, a narrow terminal clips instead of wrapping, malformed or overrunning
 * providers are dropped with a log line and no crash, providers are re-queried
 * only when the version changes, and the host->status_register ABI path (v2)
 * reaches the same table. TUI rendering goes through the in-memory harness.
 */
#include "agentc.h"
#include "status.h"
#include "plat.h"
#include "tui/tui_test.h"
#include "tui/render.h"
#include "ext.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) fails = 1;
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

/* ------------------------------------------------------------- providers */

typedef struct {
    const char *text;
    uint32_t slot;
    int32_t priority;
    uint32_t style;
} TestSeg;

static size_t emit(const TestSeg *src, size_t count, AgcExtStatusSegment *out, size_t max) {
    size_t n = count < max ? count : max;
    for (size_t i = 0; i < n; i++) {
        out[i].struct_size = sizeof out[i];
        out[i].slot = src[i].slot;
        out[i].priority = src[i].priority;
        out[i].style = src[i].style;
        out[i].text = src[i].text;
    }
    return n;
}

static const TestSeg g_a = { "alpha", AGENTC_PSEG_SLOT_LEFT, 25, 0 };
static const TestSeg g_b = { "bravo", AGENTC_PSEG_SLOT_LEFT, 15, 0 };
static const TestSeg g_c = { "charlie", AGENTC_PSEG_SLOT_LEFT, 15, 0 };
static const TestSeg g_right_hi = { "hot", AGENTC_PSEG_SLOT_RIGHT, -1, AGENTC_PSEG_STYLE_ACCENT };
static const TestSeg g_right_lo = { "extra", AGENTC_PSEG_SLOT_RIGHT, 10, 0 };

static size_t prov_a(void *ud, AgcExtStatusSegment *out, size_t max,
                     char *text, size_t text_cap) {
    (void)ud;
    (void)text;
    (void)text_cap;
    return emit(&g_a, 1, out, max);
}
static size_t prov_b(void *ud, AgcExtStatusSegment *out, size_t max,
                     char *text, size_t text_cap) {
    (void)ud;
    (void)text;
    (void)text_cap;
    return emit(&g_b, 1, out, max);
}
static size_t prov_c(void *ud, AgcExtStatusSegment *out, size_t max,
                     char *text, size_t text_cap) {
    (void)ud;
    (void)text;
    (void)text_cap;
    return emit(&g_c, 1, out, max);
}
static size_t prov_right_hi(void *ud, AgcExtStatusSegment *out, size_t max,
                     char *text, size_t text_cap) {
    (void)ud;
    (void)text;
    (void)text_cap;
    return emit(&g_right_hi, 1, out, max);
}
static size_t prov_right_lo(void *ud, AgcExtStatusSegment *out, size_t max,
                     char *text, size_t text_cap) {
    (void)ud;
    (void)text;
    (void)text_cap;
    return emit(&g_right_lo, 1, out, max);
}

static char g_chatty[400];

static size_t prov_chatty_left(void *ud, AgcExtStatusSegment *out, size_t max,
                     char *text, size_t text_cap) {
    (void)ud;
    (void)text;
    (void)text_cap;
    for (size_t i = 0; i < sizeof g_chatty - 1; i++) g_chatty[i] = 'x';
    g_chatty[sizeof g_chatty - 1] = 0;
    TestSeg s = { g_chatty, AGENTC_PSEG_SLOT_LEFT, 5, 0 };
    return emit(&s, 1, out, max);
}

static size_t prov_chatty_right(void *ud, AgcExtStatusSegment *out, size_t max,
                     char *text, size_t text_cap) {
    (void)ud;
    (void)text;
    (void)text_cap;
    for (size_t i = 0; i < sizeof g_chatty - 1; i++) g_chatty[i] = 'y';
    g_chatty[sizeof g_chatty - 1] = 0;
    TestSeg s = { g_chatty, AGENTC_PSEG_SLOT_RIGHT, -1, 0 };
    return emit(&s, 1, out, max);
}

/* One of every malformed shape the registry must drop without crashing. */
static size_t prov_bad(void *ud, AgcExtStatusSegment *out, size_t max,
                     char *text, size_t text_cap) {
    (void)ud;
    (void)text;
    (void)text_cap;
    (void)max;
    out[0].struct_size = sizeof out[0];
    out[0].slot = AGENTC_PSEG_SLOT_LEFT;
    out[0].priority = 0;
    out[0].style = 0;
    out[0].text = NULL;

    out[1] = out[0];
    out[1].text = "";

    out[2] = out[0];
    out[2].slot = 99;
    out[2].text = "bad-slot";

    out[3] = out[0];
    out[3].style = 0x80000000u;
    out[3].text = "bad-style";

    out[4] = out[0];
    out[4].text = "esc\x1b[31mred";

    out[5].struct_size = 4;   /* older/short struct: text must not be read */
    out[5].slot = AGENTC_PSEG_SLOT_LEFT;
    out[5].priority = 0;
    out[5].style = 0;
    out[5].text = "short";
    return 6;
}

static int g_capped_calls;
static size_t prov_capped(void *ud, AgcExtStatusSegment *out, size_t max,
                     char *text, size_t text_cap) {
    (void)ud;
    (void)text;
    (void)text_cap;
    g_capped_calls++;
    TestSeg many[AGENTC_STATUS_MAX_PROVIDER_SEGMENTS];
    for (size_t i = 0; i < sizeof many / sizeof many[0]; i++) {
        many[i].text = "many";
        many[i].slot = AGENTC_PSEG_SLOT_LEFT;
        many[i].priority = (int32_t)i;
        many[i].style = 0;
    }
    (void)emit(many, sizeof many / sizeof many[0], out, max);
    return max + 5;   /* lies about the count: host must clamp and log */
}

static int g_overrun_calls;
static size_t prov_overrun(void *ud, AgcExtStatusSegment *out, size_t max,
                     char *text, size_t text_cap) {
    (void)ud;
    (void)text;
    (void)text_cap;
    (void)out;
    (void)max;
    g_overrun_calls++;
    i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
    while (os_now_ns(OS_CLOCK_MONOTONIC) - t0 < 3000000LL) {
    }
    return 0;
}

static int g_count_calls;
static size_t prov_count(void *ud, AgcExtStatusSegment *out, size_t max,
                     char *text, size_t text_cap) {
    (void)ud;
    (void)text;
    (void)text_cap;
    (void)out;
    (void)max;
    g_count_calls++;
    return 0;
}

static const TestSeg g_host_seg = { "host-seg", AGENTC_PSEG_SLOT_LEFT, 0, 0 };
static size_t prov_host(void *ud, AgcExtStatusSegment *out, size_t max,
                     char *text, size_t text_cap) {
    (void)ud;
    (void)text;
    (void)text_cap;
    return emit(&g_host_seg, 1, out, max);
}

/* ---------------------------------------------------------------- layout */

static void footer_row(AgcTuiTest *t, int rows, char *out, size_t cap) {
    screen_line(agentc_tui_test_screen(t), rows - 1, out, cap);
}

static void test_builtin_bytes(void) {
    AgcTuiTest *t = agentc_tui_test_new(64, 8);
    agentc_tui_test_set_footer(t, "claude-sonnet-4-5", "high", 1234, 567, 12345);
    agentc_tui_test_feed(t, "hello\r", 6);
    char line[256];
    footer_row(t, 8, line, sizeof line);
    check("builtin_footer_bytes",
          agentc_streq(line,
                       "claude-sonnet-4-5 | think:high | tok:1234/567 | $0.012345  | 0ms"));
    agentc_tui_test_free(t);
}

/* An unknown / N/A cost (negative sentinel) omits the `$...` segment entirely;
 * the rest of the left slot stays byte-identical. */
static void test_builtin_unknown_cost(void) {
    AgcTuiTest *t = agentc_tui_test_new(64, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, -1);
    agentc_tui_test_feed(t, "hello\r", 6);
    char line[256];
    footer_row(t, 8, line, sizeof line);
    check("builtin_footer_omits_cost",
          agentc_str_str(line, "$") == NULL && agentc_str_str(line, "think:off") &&
              agentc_str_str(line, "tok:1/2") && agentc_str_str(line, "0ms"));
    agentc_tui_test_free(t);
}

static void test_slots_and_priority(void) {
    AgcTuiTest *t = agentc_tui_test_new(80, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    agentc_status_register(prov_a, NULL);
    agentc_status_register(prov_b, NULL);
    agentc_status_register(prov_c, NULL);
    char line[256];
    footer_row(t, 8, line, sizeof line);
    /* priority 15 before 20 (tokens) before 25; equal priorities keep
     * registration order (bravo then charlie) */
    check("ext_left_priority",
          agentc_str_str(line,
                         "m | think:off | bravo | charlie | tok:1/2 | alpha | $0.000003") !=
              NULL);
    agentc_tui_test_free(t);

    t = agentc_tui_test_new(80, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    agentc_status_register(prov_right_hi, NULL);
    footer_row(t, 8, line, sizeof line);
    check("ext_right_before_ready", agentc_str_str(line, "hot | ready") != NULL);
    agentc_tui_test_free(t);

    t = agentc_tui_test_new(80, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    agentc_status_register(prov_right_lo, NULL);
    footer_row(t, 8, line, sizeof line);
    check("ext_right_after_ready", agentc_str_str(line, "ready | extra") != NULL);
    agentc_tui_test_free(t);
}

static void test_truncation(void) {
    /* A 400-char left segment in a 20-column terminal: the left slot is clipped
     * to w-rl-1 and the built-in ready stays visible at the right edge. */
    AgcTuiTest *t = agentc_tui_test_new(20, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    agentc_status_register(prov_chatty_left, NULL);
    char line[256];
    footer_row(t, 8, line, sizeof line);
    check("truncate_left_len", agentc_strlen(line) == 20);
    check("truncate_left_ready", agentc_streq(line + 15, "ready"));
    agentc_tui_test_free(t);

    /* A chatty right slot fills the row; it is clipped, never wrapped. */
    t = agentc_tui_test_new(20, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    agentc_status_register(prov_chatty_right, NULL);
    footer_row(t, 8, line, sizeof line);
    check("truncate_right_len", agentc_strlen(line) == 20);
    agentc_tui_test_free(t);
}

static void test_bad_provider(void) {
    agentc_status_reset();
    agentc_status_register(prov_bad, NULL);
    AgcStatusValue segs[8];
    size_t n = agentc_status_snapshot(segs, 8);
    check("bad_provider_dropped", n == 0);
    agentc_status_reset();

    /* A provider that lies about its count is clamped, not trusted. */
    g_capped_calls = 0;
    agentc_status_register(prov_capped, NULL);
    n = agentc_status_snapshot(segs, AGENTC_STATUS_MAX_SEGMENTS);
    check("capped_provider", n == AGENTC_STATUS_MAX_PROVIDER_SEGMENTS && g_capped_calls == 1);
    agentc_status_reset();
}

static void test_overrun_provider(void) {
    agentc_status_reset();
    g_overrun_calls = 0;
    agentc_status_register(prov_overrun, NULL);
    AgcStatusValue segs[8];
    check("overrun_first", agentc_status_snapshot(segs, 8) == 0);
    check("overrun_second", agentc_status_snapshot(segs, 8) == 0);
    check("overrun_disabled", g_overrun_calls == 1);
    agentc_status_reset();
}

static void test_version_cache(void) {
    g_count_calls = 0;
    AgcTuiTest *t = agentc_tui_test_new(60, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    agentc_status_register(prov_count, NULL);
    (void)agentc_tui_test_screen(t);
    (void)agentc_tui_test_screen(t);
    (void)agentc_tui_test_screen(t);
    check("provider_called_once", g_count_calls == 1);
    agentc_tui_test_free(t);
}

static void test_host_register(void) {
    const AgcExtHost *h = agentc_ext_host();
    check("host_status_register_field", h && h->add_status != NULL);
    agentc_status_reset();
    h->add_status(prov_host, NULL);
    AgcStatusValue segs[8];
    size_t n = agentc_status_snapshot(segs, 8);
    bool found = false;
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(segs[i].text, "host-seg")) found = true;
    check("host_status_register_route", found);
    agentc_status_reset();
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    test_builtin_bytes();
    test_builtin_unknown_cost();
    test_slots_and_priority();
    test_truncation();
    test_bad_provider();
    test_overrun_provider();
    test_version_cache();
    test_host_register();
    agentc_outf("status tests: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
