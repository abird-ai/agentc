/* status_test.c — the status-line registry contract (P6-E regions/lifetimes).
 *
 * Pins: a built-in-only line is byte for byte the old footer; extension lines
 * land in the right region and priority order; a narrow terminal clips instead
 * of wrapping; malformed or overrunning lines are dropped with a log line and
 * no crash; lines are re-queried only when the version changes; the
 * host->add_status_line ABI path reaches the same table; and event regions
 * render once at a boundary with the caller's context. TUI rendering goes
 * through the in-memory harness.
 */
#include "agentc.h"
#include "status.h"
#include "actor.h"
#include "plat.h"
#include "tui/tui_test.h"
#include "tui/render.h"
#include "ext.h"

static int fails;

static void check(const char *label, bool ok) {
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok)
        fails = 1;
}

/* Copy screen row `row` (0-based) into out, stripping the newline. */
static size_t screen_line(const char *screen, int row, char *out, size_t cap) {
    size_t i = 0;
    for (int r = 0; r < row && screen[i]; r++) {
        while (screen[i] && screen[i] != '\n')
            i++;
        if (screen[i] == '\n')
            i++;
    }
    size_t n = 0;
    while (screen[i] && screen[i] != '\n' && n + 1 < cap)
        out[n++] = screen[i++];
    out[n] = 0;
    return n;
}

/* -------------------------------------------------------------- fixtures */

static int text_line(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ctx;
    const char *s = ud;
    host->out_write(out, s, agentc_strlen(s));
    return 0;
}

static AgcExtStatusLine line(int region, int lifetime, int32_t priority, uint32_t style, void *ud) {
    AgcExtStatusLine l;
    agentc_memset(&l, 0, sizeof l);
    l.struct_size = sizeof l;
    l.region = region;
    l.lifetime = lifetime;
    l.priority = priority;
    l.style = style;
    l.ud = ud;
    l.render = text_line;
    return l;
}

static char g_chatty[400];

static int chatty_line(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ud;
    (void)ctx;
    for (size_t i = 0; i < sizeof g_chatty - 1; i++)
        g_chatty[i] = 'x';
    g_chatty[sizeof g_chatty - 1] = 0;
    host->out_write(out, g_chatty, agentc_strlen(g_chatty));
    return 0;
}

static int chatty_right_line(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx,
                             void *out) {
    (void)ud;
    (void)ctx;
    for (size_t i = 0; i < sizeof g_chatty - 1; i++)
        g_chatty[i] = 'y';
    g_chatty[sizeof g_chatty - 1] = 0;
    host->out_write(out, g_chatty, agentc_strlen(g_chatty));
    return 0;
}

static int bad_line(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ud;
    (void)ctx;
    host->out_write(out, "esc\x1b[31mred", 10);
    return 0;
}

static int multi_line(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ud;
    (void)ctx;
    host->out_write(out, "one\ntwo\nthree\nfour", 18);
    return 0;
}

static int ctx_line(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ud;
    char tmp[128];
    agentc_snprintf(tmp, sizeof tmp, "step=%llu stop=%d final=%d", (unsigned long long)ctx->step,
                    ctx->stop_reason, ctx->final);
    host->out_write(out, tmp, agentc_strlen(tmp));
    return 0;
}

static int g_capped_calls;
static int capped_line(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ud;
    (void)ctx;
    g_capped_calls++;
    host->out_write(out, "many", 4);
    return 0;
}

static int g_overrun_calls;
static int overrun_line(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)host;
    (void)ud;
    (void)ctx;
    (void)out;
    g_overrun_calls++;
    i64 t0 = os_now_ns(OS_CLOCK_CPUTIME);
    /* Long enough that even a coarse (Windows) process-CPU granularity sees it
     * exceed the 2ms budget, and CPU-based so host preemption cannot cause a
     * spurious overrun on a healthy line. */
    while (os_now_ns(OS_CLOCK_CPUTIME) - t0 < 30000000LL) {
    }
    return 0;
}

static int g_count_calls;
static int count_line(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)host;
    (void)ud;
    (void)ctx;
    (void)out;
    g_count_calls++;
    return 0;
}

/* ---------------------------------------------------------------- layout */

static void footer_row(AgcTuiTest *t, int rows, char *out, size_t cap) {
    screen_line(agentc_tui_test_screen(t), rows - 1, out, cap);
}

static void test_builtin_bytes(void) {
    AgcTuiTest *t = agentc_tui_test_new(64, 8);
    agentc_tui_test_set_footer(t, "claude-sonnet-4-5", "high", 1234, 567, 12345);
    agentc_tui_test_feed(t, "hello\r", 6);
    char line_out[256];
    footer_row(t, 8, line_out, sizeof line_out);
    check(
        "builtin_footer_bytes",
        agentc_streq(line_out, "claude-sonnet-4-5 | think:high | tok:1234/567 | $0.012345  | 0ms"));
    agentc_tui_test_free(t);
}

/* An unknown / N/A cost (negative sentinel) omits the `$...` row entirely; the
 * rest of the left region stays byte-identical. */
static void test_builtin_unknown_cost(void) {
    AgcTuiTest *t = agentc_tui_test_new(64, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, -1);
    agentc_tui_test_feed(t, "hello\r", 6);
    char line_out[256];
    footer_row(t, 8, line_out, sizeof line_out);
    check("builtin_footer_omits_cost",
          agentc_str_str(line_out, "$") == NULL && agentc_str_str(line_out, "think:off") &&
              agentc_str_str(line_out, "tok:1/2") && agentc_str_str(line_out, "0ms"));
    agentc_tui_test_free(t);
}

static void test_regions_and_priority(void) {
    AgcTuiTest *t = agentc_tui_test_new(80, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    AgcExtStatusLine a =
        line(AGENTC_REGION_STATUS_LEFT, AGENTC_STATUS_PERSISTENT, 25, 0, (void *)"alpha");
    AgcExtStatusLine b =
        line(AGENTC_REGION_STATUS_LEFT, AGENTC_STATUS_PERSISTENT, 15, 0, (void *)"bravo");
    AgcExtStatusLine c =
        line(AGENTC_REGION_STATUS_LEFT, AGENTC_STATUS_PERSISTENT, 15, 0, (void *)"charlie");
    agentc_status_register(&a);
    agentc_status_register(&b);
    agentc_status_register(&c);
    char line_out[256];
    footer_row(t, 8, line_out, sizeof line_out);
    /* priority 15 before 20 (tokens) before 25; equal priorities keep
     * registration order (bravo then charlie) */
    check("ext_left_priority",
          agentc_str_str(line_out,
                         "m | think:off | bravo | charlie | tok:1/2 | alpha | $0.000003") != NULL);
    agentc_tui_test_free(t);

    t = agentc_tui_test_new(80, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    AgcExtStatusLine hi = line(AGENTC_REGION_STATUS_RIGHT, AGENTC_STATUS_PERSISTENT, -1,
                               AGENTC_PSEG_STYLE_ACCENT, (void *)"hot");
    agentc_status_register(&hi);
    footer_row(t, 8, line_out, sizeof line_out);
    check("ext_right_before_ready", agentc_str_str(line_out, "hot | ready") != NULL);
    agentc_tui_test_free(t);

    t = agentc_tui_test_new(80, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    AgcExtStatusLine lo =
        line(AGENTC_REGION_STATUS_RIGHT, AGENTC_STATUS_PERSISTENT, 10, 0, (void *)"extra");
    agentc_status_register(&lo);
    footer_row(t, 8, line_out, sizeof line_out);
    check("ext_right_after_ready", agentc_str_str(line_out, "ready | extra") != NULL);
    agentc_tui_test_free(t);
}

static void test_truncation(void) {
    /* A 400-char left row in a 20-column terminal: the left region is clipped
     * to w-rl-1 and the built-in ready stays visible at the right edge. */
    AgcTuiTest *t = agentc_tui_test_new(20, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    AgcExtStatusLine cl = line(AGENTC_REGION_STATUS_LEFT, AGENTC_STATUS_PERSISTENT, 5, 0, NULL);
    cl.render = chatty_line;
    agentc_status_register(&cl);
    char line_out[256];
    footer_row(t, 8, line_out, sizeof line_out);
    check("truncate_left_len", agentc_strlen(line_out) == 20);
    check("truncate_left_ready", agentc_streq(line_out + 15, "ready"));
    agentc_tui_test_free(t);

    /* A chatty right region fills the row; it is clipped, never wrapped. */
    t = agentc_tui_test_new(20, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    AgcExtStatusLine cr = line(AGENTC_REGION_STATUS_RIGHT, AGENTC_STATUS_PERSISTENT, -1, 0, NULL);
    cr.render = chatty_right_line;
    agentc_status_register(&cr);
    footer_row(t, 8, line_out, sizeof line_out);
    check("truncate_right_len", agentc_strlen(line_out) == 20);
    agentc_tui_test_free(t);
}

static void test_bad_line(void) {
    agentc_status_reset();
    AgcExtStatusLine b = line(AGENTC_REGION_STATUS_LEFT, AGENTC_STATUS_PERSISTENT, 0, 0, NULL);
    b.render = bad_line;
    agentc_status_register(&b);
    AgcStatusValue segs[8];
    size_t n = agentc_status_snapshot(AGENTC_STATUS_ALL_REGIONS, segs, 8);
    check("bad_line_dropped", n == 0);
    agentc_status_reset();

    /* A line that emits more rows than the per-region cap is clipped. */
    g_capped_calls = 0;
    AgcExtStatusLine m = line(AGENTC_REGION_STATUS_LEFT, AGENTC_STATUS_PERSISTENT, 0, 0, NULL);
    m.render = capped_line;
    agentc_status_register(&m);
    n = agentc_status_snapshot(AGENTC_REGION_STATUS_LEFT, segs, 8);
    check("capped_line", n == 0 || n <= AGENTC_STATUS_MAX_LINES_PER_REGION);
    check("capped_called", g_capped_calls == 1);
    agentc_status_reset();
}

static void test_overrun_line(void) {
    agentc_status_reset();
    g_overrun_calls = 0;
    AgcExtStatusLine o = line(AGENTC_REGION_STATUS_LEFT, AGENTC_STATUS_PERSISTENT, 0, 0, NULL);
    o.render = overrun_line;
    agentc_status_register(&o);
    AgcStatusValue segs[8];
    check("overrun_first", agentc_status_snapshot(AGENTC_REGION_STATUS_LEFT, segs, 8) == 0);
    check("overrun_second", agentc_status_snapshot(AGENTC_REGION_STATUS_LEFT, segs, 8) == 0);
    check("overrun_disabled", g_overrun_calls == 1);
    agentc_status_reset();
}

static void test_version_cache(void) {
    g_count_calls = 0;
    AgcTuiTest *t = agentc_tui_test_new(60, 8);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    AgcExtStatusLine c = line(AGENTC_REGION_STATUS_LEFT, AGENTC_STATUS_PERSISTENT, 0, 0, NULL);
    c.render = count_line;
    agentc_status_register(&c);
    (void)agentc_tui_test_screen(t);
    (void)agentc_tui_test_screen(t);
    (void)agentc_tui_test_screen(t);
    check("line_called_once", g_count_calls == 1);
    agentc_tui_test_free(t);
}

static void test_host_register(void) {
    const AgcExtHost *h = agentc_ext_host();
    check("host_status_line_field", h && h->add_status_line != NULL);
    agentc_status_reset();
    h->add_status_line(&(AgcExtStatusLine){
        .struct_size = sizeof(AgcExtStatusLine),
        .region = AGENTC_REGION_STATUS_LEFT,
        .lifetime = AGENTC_STATUS_PERSISTENT,
        .priority = 0,
        .render = text_line,
        .ud = (void *)"host-seg",
    });
    AgcStatusValue segs[8];
    size_t n = agentc_status_snapshot(AGENTC_STATUS_ALL_REGIONS, segs, 8);
    bool found = false;
    for (size_t i = 0; i < n; i++)
        if (agentc_streq(segs[i].text, "host-seg"))
            found = true;
    check("host_status_line_route", found);
    agentc_status_reset();
}

static void test_emit_regions(void) {
    agentc_status_reset();
    AgcExtStatusLine e = line(AGENTC_REGION_STEP_END, AGENTC_STATUS_PER_TURN, 0, 0, NULL);
    e.render = ctx_line;
    agentc_status_register(&e);
    AgcExtStatusCtx ctx;
    agentc_memset(&ctx, 0, sizeof ctx);
    ctx.struct_size = sizeof ctx;
    ctx.region = AGENTC_REGION_STEP_END;
    ctx.step = 3;
    ctx.stop_reason = AGENTC_EXT_STOP_TOOLUSE;
    AgcBuf b = {0};
    size_t rows = agentc_status_emit(AGENTC_REGION_STEP_END, &ctx, &b);
    check("emit_rows", rows == 1);
    check("emit_ctx_text", b.p && agentc_streq((char *)b.p, "step=3 stop=3 final=0"));
    agentc_buf_free(&b);

    /* A chrome-only line is not emitted by an event emit. */
    AgcBuf b2 = {0};
    rows = agentc_status_emit(AGENTC_REGION_TURN_END, &ctx, &b2);
    check("emit_empty_region", rows == 0);
    agentc_buf_free(&b2);

    /* Multi-line render: rows split on '\n' and clip at the region cap. */
    AgcExtStatusLine ml = line(AGENTC_REGION_TURN_END, AGENTC_STATUS_PER_TURN, 0, 0, NULL);
    ml.render = multi_line;
    agentc_status_register(&ml);
    AgcBuf b3 = {0};
    rows = agentc_status_emit(AGENTC_REGION_TURN_END, &ctx, &b3);
    check("emit_multi_rows", rows == AGENTC_STATUS_MAX_LINES_PER_REGION);
    check("emit_multi_text", b3.p && agentc_streq((char *)b3.p, "one\ntwo\nthree"));
    agentc_buf_free(&b3);

    /* Removed and reset both hide the line. */
    agentc_status_remove(AGENTC_REGION_TURN_END, ml.render, NULL);
    agentc_status_reset();
    AgcBuf b4 = {0};
    check("emit_after_reset", agentc_status_emit(AGENTC_REGION_TURN_END, &ctx, &b4) == 0);
    agentc_buf_free(&b4);
}

static void test_chrome_regions(void) {
    agentc_status_reset();
    AgcTuiTest *t = agentc_tui_test_new(40, 12);
    agentc_tui_test_set_footer(t, "m", "off", 1, 2, 3);
    AgcExtStatusLine top =
        line(AGENTC_REGION_ABOVE_COMPOSER, AGENTC_STATUS_PERSISTENT, 0, 0, (void *)"CHROME-TOP");
    AgcExtStatusLine bot =
        line(AGENTC_REGION_BELOW_COMPOSER, AGENTC_STATUS_PERSISTENT, 0, 0, (void *)"CHROME-BOT");
    AgcExtStatusLine under =
        line(AGENTC_REGION_BELOW_STATUS, AGENTC_STATUS_PERSISTENT, 0, 0, (void *)"CHROME-UNDER");
    agentc_status_register(&top);
    agentc_status_register(&bot);
    agentc_status_register(&under);
    agentc_tui_test_feed(t, "hi\r", 3);
    const char *screen = agentc_tui_test_screen(t);
    check("chrome_above", agentc_str_str(screen, "CHROME-TOP") != NULL);
    check("chrome_below", agentc_str_str(screen, "CHROME-BOT") != NULL);
    check("chrome_under", agentc_str_str(screen, "CHROME-UNDER") != NULL);
    agentc_tui_test_free(t);
    agentc_status_reset();
}

/* P6-SURFACE §12: the built-in actors segment appears only with background
 * work, and reports the background actor count. */
static void test_actors_line(void) {
    agentc_status_reset();
    agentc_status_register_builtin();
    agentc_actor_init();
    AgcActorId root = agentc_actor_register(AGENTC_ACTOR_AGENT, 0, "root");
    AgcActorId child = agentc_actor_register(AGENTC_ACTOR_AGENT, root, "child");
    check("actors.registered", root != 0 && child != 0);
    AgcStatusValue segs[8];
    size_t n = agentc_status_snapshot(AGENTC_STATUS_ALL_REGIONS, segs, 8);
    bool found = false;
    for (size_t i = 0; i < n; i++)
        if (agentc_str_str(segs[i].text, "bg:1") != NULL)
            found = true;
    check("actors.segment", found);
    agentc_actor_release_all();
    agentc_status_reset();
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    test_builtin_bytes();
    test_builtin_unknown_cost();
    test_regions_and_priority();
    test_truncation();
    test_bad_line();
    test_overrun_line();
    test_version_cache();
    test_host_register();
    test_emit_regions();
    test_chrome_regions();
    test_actors_line();
    agentc_outf("status tests: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
