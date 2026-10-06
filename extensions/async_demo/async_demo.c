/* async_demo.c — reference asynchronous extension tools.
 *
 * Four tools demonstrate the start/step/stop contract documented in
 * include/agentc_ext.h. None declares run(), so the host registers every one
 * of them as asynchronous:
 *
 *   async_demo   {steps:N, fail_at:K} — streams "step <i>\n" per step and
 *                completes after N steps. At step K it streams
 *                "error: demo failed at <i>\n", completes and sets
 *                *is_error = true: a tool-result error, not an agent failure.
 *   async_fatal  — returns -5 (EIO) on its third step. The host's generic
 *                failure path discards the staged output, answers with
 *                "error: tool 'async_fatal' failed to run (-5)" and calls
 *                stop(ERROR).
 *   async_slow   — timeout_ms = 1 and never completes on its own; the host
 *                deadline calls stop(TIMEOUT) after one step.
 *   async_cancel — never completes on its own. Every step polls
 *                host->is_cancelled(call->signal_token) and, when the token is
 *                cancelled, streams "cancelled at step <i>\n", completes and
 *                sets *is_error = true (partial output is kept). When the
 *                host's job driver observes the turn's abort flag first, it
 *                delivers stop(CANCELLED) itself, which is the ordinary batch
 *                abort path; both exits stop exactly once.
 *
 * State discipline: all per-call state is host->alloc'ed in start and
 * host->free'd in stop; the host never frees it. Step output goes through
 * host->out_write in bounded chunks into the host's per-step staging buffer.
 * start/step never block.
 *
 * The `async-demo-debug` command is a test surface (not part of the tool
 * contract): it renders the per-tool start/step/stop counters so a test can
 * observe exactly one stop and its reason without importing a link symbol.
 * `{"reset":true}` clears the counters first.
 */
#include "agentc_ext.h"

enum { AD_DEMO = 0, AD_FATAL, AD_SLOW, AD_CANCEL, AD_COUNT };

static const char *const ad_names[AD_COUNT] = {
    "async_demo", "async_fatal", "async_slow", "async_cancel",
};

/* Per-tool test counters (async-demo-debug). */
typedef struct {
    int starts;
    int steps;
    int stops;
    int last_reason;       /* -1 until the first stop */
    int polled_cancelled;  /* async_cancel: steps that observed a cancelled token */
} AdCounter;

static AdCounter g_count[AD_COUNT];

/* Per-job state, extension-owned: host->alloc in start, host->free in stop. */
typedef struct {
    int tool;     /* AD_* */
    int steps;    /* async_demo: total steps */
    int fail_at;  /* async_demo: step that fails, -1 = never */
    int i;        /* next step index */
} AdState;

/* ------------------------------------------------------------- small fmt */

/* Freestanding: a tiny append-only formatter for the demo's status lines. */
static size_t ad_put(char *dst, size_t n, const char *s) {
    while (*s) dst[n++] = *s++;
    return n;
}

static size_t ad_put_int(char *dst, size_t n, int v) {
    char tmp[12];
    size_t t = 0;
    bool neg = v < 0;
    unsigned u = neg ? (unsigned)(-(v + 1)) + 1u : (unsigned)v;
    do {
        tmp[t++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg) dst[n++] = '-';
    while (t > 0) dst[n++] = tmp[--t];
    return n;
}

/* "prefix<index>suffix" as one bounded write. `index` < 0 omits the digits. */
static void ad_line(const AgcExtHost *host, void *out, const char *prefix,
                    int index, const char *suffix) {
    char buf[64];
    size_t n = ad_put(buf, 0, prefix);
    if (index >= 0) n = ad_put_int(buf, n, index);
    n = ad_put(buf, n, suffix);
    host->out_write(out, buf, n);
}

/* ---------------------------------------------------------- lifecycle */

static int ad_start(const AgcExtHost *host, const AgcExtTool *self,
                    const AgcExtToolCall *call, void *out, bool *is_error,
                    void **state) {
    (void)out;
    int tool = *(const int *)self->ud;
    AdState *st = host->alloc(sizeof *st);
    if (!st) return -12;   /* ENOMEM: start < 0, the host never calls stop */
    const char *args = (call && call->args_json) ? call->args_json : "{}";
    st->tool = tool;
    st->i = 0;
    st->steps = tool == AD_DEMO ? host->json_get_int(args, "steps", 3) : 0;
    st->fail_at = tool == AD_DEMO ? host->json_get_int(args, "fail_at", -1) : -1;
    if (st->steps < 0) st->steps = 0;
    g_count[tool].starts++;
    if (is_error) *is_error = false;
    *state = st;
    /* A zero-step demo completes in start; the host calls stop(FINISHED). */
    return (tool == AD_DEMO && st->steps == 0) ? 1 : 0;
}

static int ad_step(const AgcExtHost *host, const AgcExtTool *self,
                   const AgcExtToolCall *call, void *state, void *out,
                   bool *is_error) {
    (void)self;
    AdState *st = state;
    if (!st) return -22;
    g_count[st->tool].steps++;
    switch (st->tool) {
    case AD_DEMO:
        if (st->fail_at >= 0 && st->i == st->fail_at) {
            ad_line(host, out, "error: demo failed at ", st->i, "\n");
            if (is_error) *is_error = true;
            return 1;
        }
        ad_line(host, out, "step ", st->i, "\n");
        st->i++;
        return st->i >= st->steps ? 1 : 0;

    case AD_FATAL:
        if (st->i == 2) return -5;   /* EIO: the generic failure path */
        ad_line(host, out, "fatal step ", st->i, "\n");
        st->i++;
        return 0;

    case AD_SLOW:
        ad_line(host, out, "slow step ", st->i, "\n");
        st->i++;
        return 0;

    case AD_CANCEL:
        if (host->is_cancelled(host, call ? call->signal_token : NULL)) {
            g_count[st->tool].polled_cancelled++;
            ad_line(host, out, "cancelled at step ", st->i, "\n");
            if (is_error) *is_error = true;
            return 1;   /* complete; the result is an error */
        }
        ad_line(host, out, "cancel step ", st->i, "\n");
        st->i++;
        return 0;
    }
    return -22;
}

static void ad_stop(const AgcExtHost *host, const AgcExtTool *self, void *state,
                    int reason) {
    (void)self;
    AdState *st = state;
    if (st) {
        if (st->tool >= 0 && st->tool < AD_COUNT) {
            g_count[st->tool].stops++;
            g_count[st->tool].last_reason = reason;
        }
        host->free(st);
    }
}

/* ---------------------------------------------------------- registration */

static const char ad_params_demo[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"steps\":{\"type\":\"integer\",\"description\":\"steps to stream\"},"
    "\"fail_at\":{\"type\":\"integer\",\"description\":\"step index that fails (result error)\"}},"
    "\"required\":[]}";

static const char ad_params_none[] = "{\"type\":\"object\",\"properties\":{}}";

static int ad_ids[AD_COUNT] = { AD_DEMO, AD_FATAL, AD_SLOW, AD_CANCEL };

static const AgcExtTool ad_tools[AD_COUNT] = {
    { .struct_size = sizeof(AgcExtTool), .flags = AGENTC_TOOL_READONLY,
      .name = "async_demo", .label = "Async demo",
      .description = "Stream N steps; optionally fail at step K with a tool-result error.",
      .parameters_json = ad_params_demo, .ud = &ad_ids[AD_DEMO],
      .start = ad_start, .step = ad_step, .stop = ad_stop },
    { .struct_size = sizeof(AgcExtTool), .flags = AGENTC_TOOL_READONLY,
      .name = "async_fatal", .label = "Async fatal",
      .description = "Fails fatally on its third step (generic failure path).",
      .parameters_json = ad_params_none, .ud = &ad_ids[AD_FATAL],
      .start = ad_start, .step = ad_step, .stop = ad_stop },
    { .struct_size = sizeof(AgcExtTool), .flags = AGENTC_TOOL_READONLY,
      .name = "async_slow", .label = "Async slow",
      .description = "Never completes; times out deterministically after 1 ms.",
      .parameters_json = ad_params_none, .ud = &ad_ids[AD_SLOW],
      .timeout_ms = 1, .start = ad_start, .step = ad_step, .stop = ad_stop },
    { .struct_size = sizeof(AgcExtTool), .flags = AGENTC_TOOL_READONLY,
      .name = "async_cancel", .label = "Async cancel",
      .description = "Runs until cancelled; polls the signal token every step.",
      .parameters_json = ad_params_none, .ud = &ad_ids[AD_CANCEL],
      .start = ad_start, .step = ad_step, .stop = ad_stop },
};

/* Test surface: one line per tool with the counters above. */
static void ad_debug_run(const AgcExtHost *host, void *ud, const char *args_json,
                         void *out) {
    (void)ud;
    if (host->json_get_bool(args_json, "reset", 0)) {
        for (int i = 0; i < AD_COUNT; i++) {
            g_count[i].starts = 0;
            g_count[i].steps = 0;
            g_count[i].stops = 0;
            g_count[i].last_reason = -1;
            g_count[i].polled_cancelled = 0;
        }
    }
    for (int i = 0; i < AD_COUNT; i++) {
        char buf[128];
        size_t n = ad_put(buf, 0, ad_names[i]);
        n = ad_put(buf, n, " starts=");
        n = ad_put_int(buf, n, g_count[i].starts);
        n = ad_put(buf, n, " steps=");
        n = ad_put_int(buf, n, g_count[i].steps);
        n = ad_put(buf, n, " stops=");
        n = ad_put_int(buf, n, g_count[i].stops);
        n = ad_put(buf, n, " reason=");
        n = ad_put_int(buf, n, g_count[i].last_reason);
        n = ad_put(buf, n, " polled=");
        n = ad_put_int(buf, n, g_count[i].polled_cancelled);
        n = ad_put(buf, n, "\n");
        host->out_write(out, buf, n);
    }
}

static int ad_init(const AgcExtHost *host) {
    for (size_t i = 0; i < AD_COUNT; i++) host->add_tool(&ad_tools[i]);
    static const AgcExtCommand debug = {
        .struct_size = sizeof(AgcExtCommand),
        .name = "async-demo-debug",
        .description = "async_demo counters (test surface)",
        .ud = NULL,
        .run = ad_debug_run,
    };
    host->add_command(&debug);
    return 0;
}

int agentc_ext_init(const AgcExtHost *host, AgcExt *out) {
    (void)host;
    out->abi_version = AGENTC_EXT_ABI;
    out->struct_size = sizeof *out;
    out->name = "async_demo";
    out->version = "0.1.0";
    out->order = 0;
    out->init = ad_init;
    out->shutdown = NULL;
    out->required_host_size = 0;
    return 0;
}
