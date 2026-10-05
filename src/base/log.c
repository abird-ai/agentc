/* log.c — stdout/stderr writers, level logging, fatal errors.
 *
 * Every writer here goes through the terminal-safe boundary in base/out.h, so
 * diagnostic and model-facing text cannot carry control sequences to the
 * terminal; setup.c is the only caller of the raw writer.
 */
#include "agentc.h"
#include "base/out.h"
#include "plat.h"

void agentc_out(const void *p, size_t n) {
    (void)agentc_out_safe_write(1, p, n);
}
void agentc_outs(const char *s) {
    agentc_out(s, agentc_strlen(s));
}
void agentc_out_nl(void) {
    agentc_out("\n", 1);
}

void agentc_out_u64(u64 v) {
    char tmp[32];
    size_t n = agentc_fmt_u64(tmp, v);
    agentc_out(tmp, n);
}

void agentc_outf(const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = agentc_vsnprintf_used(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        (void)agentc_out_safe_write(1, tmp, (size_t)n);
}

static int g_log_level = -1;

void agentc_log_set_level(int level) {
    g_log_level = level;
}

static int log_threshold(void) {
    static int thr = -1;
    if (g_log_level >= 0)
        return g_log_level;
    if (thr < 0) {
        const char *v = os_getenv("AGENTC_LOG");
        thr = 1; /* info */
        if (v) {
            if (agentc_streq(v, "debug"))
                thr = 0;
            else if (agentc_streq(v, "info"))
                thr = 1;
            else if (agentc_streq(v, "warn") || agentc_streq(v, "warning"))
                thr = 2;
            else if (agentc_streq(v, "error"))
                thr = 3;
            else if (agentc_streq(v, "quiet"))
                thr = 4;
        }
    }
    return thr;
}

static const char *level_tag(int level) {
    switch (level) {
    case 0:
        return "debug: ";
    case 1:
        return "info: ";
    case 2:
        return "warn: ";
    default:
        return "error: ";
    }
}

static AgcLogFn g_log_fn;
static void *g_log_ud;

void agentc_log_install(AgcLogFn fn, void *ud) {
    g_log_fn = fn;
    g_log_ud = ud;
}

void agentc_logf(int level, const char *fmt, ...) {
    if (level < log_threshold())
        return;
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = agentc_vsnprintf_used(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if (g_log_fn) {
        g_log_fn(g_log_ud, level, tmp);
        return;
    }
    const char *tag = level_tag(level);
    (void)agentc_out_safe_write(2, tag, agentc_strlen(tag));
    if (n > 0)
        (void)agentc_out_safe_write(2, tmp, (size_t)n);
    (void)agentc_out_safe_write(2, "\n", 1);
}

void agentc_logs(int level, const char *msg) {
    agentc_logf(level, "%s", msg);
}

static void (*g_atexit[8])(void);
static int g_atexit_n;

void agentc_atexit(void (*cb)(void)) {
    if (cb == NULL)
        return;
    for (int i = 0; i < g_atexit_n; i++)
        if (g_atexit[i] == cb)
            return; /* idempotent: a repeated registration is a no-op */
    if (g_atexit_n < (int)(sizeof g_atexit / sizeof g_atexit[0]))
        g_atexit[g_atexit_n++] = cb;
}

static AgcFatalFn g_fatal_fn;
static void *g_fatal_ud;

void agentc_fatal_install(AgcFatalFn fn, void *ud) {
    g_fatal_fn = fn;
    g_fatal_ud = ud;
}

void agentc_die(const char *msg) {
    for (int i = g_atexit_n - 1; i >= 0; i--)
        if (g_atexit[i])
            g_atexit[i]();
    if (g_fatal_fn)
        g_fatal_fn(g_fatal_ud, msg ? msg : "fatal error");
    (void)agentc_out_safe_write(2, "agentc: ", 8);
    (void)agentc_out_safe_write(2, msg, agentc_strlen(msg));
    (void)agentc_out_safe_write(2, "\n", 1);
    os_exit(1);
}
