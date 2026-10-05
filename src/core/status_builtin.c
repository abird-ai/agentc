/* status_builtin.c — the built-in status segments.
 *
 * This is deliberately an ordinary provider: it reaches the status line only
 * through agentc_status_register(), exactly like a extension's status_register
 * callback. The TUI copies its state in with agentc_status_builtin_set()
 * whenever a footer-relevant value changes; the provider formats that snapshot
 * into the model/thinking/tokens/cost left segments and the state/ready right
 * segment. Text and styles are chosen so a built-in-only build renders the
 * historical footer byte for byte.
 */
#include "status.h"

static AgcStatusBuiltin g_state;

static bool builtin_eq(const AgcStatusBuiltin *a, const AgcStatusBuiltin *b) {
    return agentc_streq(a->model, b->model) && agentc_streq(a->thinking, b->thinking) &&
           a->tok_in == b->tok_in && a->tok_out == b->tok_out &&
           a->cost_micro == b->cost_micro && a->spinner == b->spinner &&
           a->elapsed_ms == b->elapsed_ms && a->running == b->running;
}

static void put(AgcExtStatusSegment *out, size_t *n, size_t max, uint32_t slot,
                int32_t priority, uint32_t style, const char *text) {
    if (*n == max) return;
    AgcExtStatusSegment *s = &out[(*n)++];
    s->struct_size = sizeof *s;
    s->slot = slot;
    s->priority = priority;
    s->style = style;
    s->text = text;
}

/* Appends a NUL-terminated copy to the provider's arena and returns its
 * address; once the arena is full the segment is simply invisible. */
static const char *arena_puts(char **p, size_t *left, const char *s) {
    static const char empty[] = "";
    if (*left == 0) return empty;
    size_t n = agentc_strlen(s);
    char *dst = *p;
    if (n + 1 > *left) n = *left - 1;
    agentc_memcpy(dst, s, n);
    dst[n] = 0;
    *p += n + 1;
    *left -= n + 1;
    return dst;
}

static const char *arena_printf(char **p, size_t *left, const char *fmt, ...) {
    char tmp[AGENTC_STATUS_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    agentc_vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    return arena_puts(p, left, tmp);
}

static size_t builtin_provider(void *userdata, AgcExtStatusSegment *out, size_t max,
                               char *text, size_t text_cap) {
    (void)userdata;
    size_t n = 0;
    char *p = text;
    size_t left = text_cap;

    put(out, &n, max, AGENTC_PSEG_SLOT_LEFT, 0, 0, g_state.model);
    put(out, &n, max, AGENTC_PSEG_SLOT_LEFT, 10, 0,
        arena_printf(&p, &left, "think:%s", g_state.thinking));
    put(out, &n, max, AGENTC_PSEG_SLOT_LEFT, 20, 0,
        arena_printf(&p, &left, "tok:%llu/%llu",
                     (unsigned long long)g_state.tok_in,
                     (unsigned long long)g_state.tok_out));
    /* A negative cost is "unknown / not applicable" (an unrated or local
     * model): omit the segment entirely rather than pretending it is free. */
    if (g_state.cost_micro >= 0) {
        i64 micro = g_state.cost_micro;
        put(out, &n, max, AGENTC_PSEG_SLOT_LEFT, 30, 0,
            arena_printf(&p, &left, "$%lld.%06llu", (long long)(micro / 1000000),
                         (unsigned long long)(micro % 1000000)));
    }

    if (g_state.running) {
        static const char spin[] = "|/-\\";
        i64 ms = g_state.elapsed_ms;
        const char *state;
        if (ms < 1000)
            state = arena_printf(&p, &left, "%c %lldms", spin[g_state.spinner & 3],
                                 (long long)ms);
        else
            state = arena_printf(&p, &left, "%c %lld.%01llds", spin[g_state.spinner & 3],
                                 (long long)(ms / 1000),
                                 (long long)((ms % 1000) / 100));
        put(out, &n, max, AGENTC_PSEG_SLOT_RIGHT, 0, AGENTC_PSEG_STYLE_ACCENT, state);
    } else {
        put(out, &n, max, AGENTC_PSEG_SLOT_RIGHT, 0, 0, "ready");
    }
    return n;
}

void agentc_status_register_builtin(void) {
    agentc_status_register(builtin_provider, NULL);
}

void agentc_status_builtin_set(const AgcStatusBuiltin *state) {
    if (!state || builtin_eq(state, &g_state)) return;
    g_state = *state;
    agentc_status_invalidate();
}
