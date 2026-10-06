/* fmt_test.c — formatted output (agentc_snprintf / agentc_vsnprintf, no libc).
 *
 * Covers the conversions the rest of the tree relies on, the flag/width
 * handling added for table output, and the truncation/return-value contract.
 */
#include "agentc.h"

static int fails;

static void eq(const char *label, const char *got, const char *want) {
    bool ok = agentc_streq(got, want);
    agentc_outf("%s=%d\n", label, ok ? 1 : 0);
    if (!ok) {
        agentc_outf("  got  \"%s\"\n  want \"%s\"\n", got, want);
        fails = 1;
    }
}

static void ok(const char *label, bool v) {
    agentc_outf("%s=%d\n", label, v ? 1 : 0);
    if (!v) fails = 1;
}

/* va_list wrapper so agentc_vsnprintf_used itself has golden coverage. */
static int fmt_used_va(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = agentc_vsnprintf_used(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

int agentc_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char b[128];

    agentc_snprintf(b, sizeof b, "%d|%d|%d", 0, -42, 2147483647);
    eq("int", b, "0|-42|2147483647");
    agentc_snprintf(b, sizeof b, "%d", -2147483647 - 1);
    eq("int_min", b, "-2147483648");
    agentc_snprintf(b, sizeof b, "%lld", -9223372036854775807LL - 1);
    eq("i64_min", b, "-9223372036854775808");
    agentc_snprintf(b, sizeof b, "%lld|%ld|%d", 9223372036854775807LL, 7L, -1);
    eq("length_modifiers", b, "9223372036854775807|7|-1");

    agentc_snprintf(b, sizeof b, "%u|%u", 0u, 4294967295u);
    eq("unsigned", b, "0|4294967295");
    agentc_snprintf(b, sizeof b, "%x|%X|%llx", 0u, 0xdeadbeefu, 0x123456789abuLL);
    eq("hex", b, "0|DEADBEEF|123456789ab");
    agentc_snprintf(b, sizeof b, "%zu", (size_t)12345);
    eq("size_t", b, "12345");

    agentc_snprintf(b, sizeof b, "[%5d][%-5d][%05d][%05d][%-05d]", 42, 42, 42, -42, 42);
    eq("int_widths", b, "[   42][42   ][00042][-0042][42   ]");
    agentc_snprintf(b, sizeof b, "[%8s][%-8s][%3s]", "hi", "hi", "hello");
    eq("str_widths", b, "[      hi][hi      ][hello]");
    agentc_snprintf(b, sizeof b, "[%3c][%-3c][%c]", 'A', 'B', 'C');
    eq("char_widths", b, "[  A][B  ][C]");
    agentc_snprintf(b, sizeof b, "%s|%s", (const char *)0, "");
    eq("null_str", b, "(null)|");
    agentc_snprintf(b, sizeof b, "%12llx|%-6llx|", 0x123456789abULL, 0x1fULL);
    eq("hex_widths", b, " 123456789ab|1f    |");

    agentc_snprintf(b, sizeof b, "100%%|%p", (void *)0);
    eq("percent_p", b, "100%|0x0");
    agentc_snprintf(b, sizeof b, "%q%d", 7);
    eq("unknown_spec", b, "%q7");

    /* truncation must still NUL-terminate and report the full length */
    agentc_memset(b, 'Z', sizeof b);
    int n = agentc_snprintf(b, 4, "abcdef");
    ok("trunc_return", n == 6 && agentc_streq(b, "abc"));
    n = agentc_snprintf(b, sizeof b, "a%db", 5);
    ok("return_len", n == 3 && agentc_streq(b, "a5b"));
    b[0] = 'X';
    n = agentc_snprintf(b, 1, "abc");
    ok("cap_one", n == 3 && b[0] == 0);
    n = agentc_snprintf(b, 0, "abc");
    ok("cap_zero", n == 3);
    agentc_snprintf(b, sizeof b, "%s", "0123456789");
    ok("exact_fit", agentc_streq(b, "0123456789"));

    /* the _used variants report what was written, never the would-be length */
    char big[4096];
    agentc_memset(big, 'x', sizeof big);
    big[sizeof big - 1] = 0;
    char small[16];
    agentc_memset(small, 'Z', sizeof small);
    int u = agentc_snprintf_used(small, sizeof small, "%s", big);
    ok("used_trunc", u == 15 && small[15] == 0 && agentc_strlen(small) == 15);
    u = agentc_snprintf_used(small, 0, "abc");
    ok("used_cap0", u == 0);
    u = agentc_snprintf_used(small, 4, "abc");
    ok("used_fit", u == 3 && agentc_streq(small, "abc"));
    u = fmt_used_va(small, 4, "%s", "abcdef");
    ok("used_v", u == 3 && agentc_streq(small, "abc"));

    agentc_outf("no_fmt_errors=%d\n", fails == 0 ? 1 : 0);
    return fails;
}
