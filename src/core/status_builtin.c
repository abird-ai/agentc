/* status_builtin.c — the built-in status lines.
 *
 * These are deliberately ordinary AgcExtStatusLine registrations: they reach
 * the status row only through agentc_status_register(), exactly like an
 * extension's add_status_line. The TUI copies its state in with
 * agentc_status_builtin_set() whenever a footer-relevant value changes; the
 * render callbacks format that snapshot into the model/thinking/tokens/cost
 * left rows and the state/ready right row. Text and styles are chosen so a
 * built-in-only build renders the historical footer byte for byte.
 */
#include "status.h"

#include "actor.h"
#include "base/limits.h"
#include "message.h"

static AgcStatusBuiltin g_state;

static bool builtin_eq(const AgcStatusBuiltin *a, const AgcStatusBuiltin *b) {
    return agentc_streq(a->model, b->model) && agentc_streq(a->thinking, b->thinking) &&
           a->tok_in == b->tok_in && a->tok_out == b->tok_out && a->cost_micro == b->cost_micro &&
           a->spinner == b->spinner && a->elapsed_ms == b->elapsed_ms && a->running == b->running;
}

static void put(const AgcExtHost *host, void *out, const char *text) {
    if (text && text[0])
        host->out_write(out, text, agentc_strlen(text));
}

static int builtin_model(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ud;
    (void)ctx;
    put(host, out, g_state.model);
    return 0;
}

static int builtin_think(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ud;
    (void)ctx;
    char tmp[AGENTC_STATUS_TEXT_MAX];
    agentc_snprintf(tmp, sizeof tmp, "think:%s", g_state.thinking);
    put(host, out, tmp);
    return 0;
}

static int builtin_tok(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ud;
    (void)ctx;
    char tmp[AGENTC_STATUS_TEXT_MAX];
    agentc_snprintf(tmp, sizeof tmp, "tok:%llu/%llu", (unsigned long long)g_state.tok_in,
                    (unsigned long long)g_state.tok_out);
    put(host, out, tmp);
    return 0;
}

static int builtin_cost(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ud;
    (void)ctx;
    /* A negative cost is "unknown / not applicable" (an unrated or local
     * model): omit the row entirely rather than pretending it is free. */
    if (g_state.cost_micro < 0)
        return 0;
    char tmp[AGENTC_STATUS_TEXT_MAX];
    i64 micro = g_state.cost_micro;
    agentc_snprintf(tmp, sizeof tmp, "$%lld.%06llu", (long long)(micro / 1000000),
                    (unsigned long long)(micro % 1000000));
    put(host, out, tmp);
    return 0;
}

static int builtin_state(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ud;
    (void)ctx;
    char tmp[AGENTC_STATUS_TEXT_MAX];
    if (g_state.running) {
        static const char spin[] = "|/-\\";
        i64 ms = g_state.elapsed_ms;
        if (ms < 1000)
            agentc_snprintf(tmp, sizeof tmp, "%c %lldms", spin[g_state.spinner & 3], (long long)ms);
        else
            agentc_snprintf(tmp, sizeof tmp, "%c %lld.%01llds", spin[g_state.spinner & 3],
                            (long long)(ms / 1000), (long long)((ms % 1000) / 100));
    } else {
        agentc_snprintf(tmp, sizeof tmp, "ready");
    }
    put(host, out, tmp);
    return 0;
}

/* P6-SURFACE §12: the built-in `actors` segment reports background and stalled
 * actor counts plus queue depth and the current turn. It is hidden entirely
 * when there is no background work, so an idle footer is byte-identical. */
static int builtin_actors(const AgcExtHost *host, void *ud, const AgcExtStatusCtx *ctx, void *out) {
    (void)ud;
    (void)ctx;
    AgcActor rows[AGENTC_ACTOR_MAX];
    size_t n = agentc_actor_list(rows, AGENTC_ACTOR_MAX);
    size_t background = 0, stalled = 0, queued = 0;
    int64_t turn = 0;
    for (size_t i = 0; i < n; i++) {
        if (rows[i].kind != AGENTC_ACTOR_AGENT)
            continue;
        if (rows[i].parent != 0)
            background++;
        if (rows[i].state == AGENTC_ACTOR_STALLED)
            stalled++;
        queued += agentc_queue_count(rows[i].id);
        if (rows[i].turn_seq > turn)
            turn = rows[i].turn_seq;
    }
    if (background == 0 && stalled == 0 && queued == 0)
        return 0;
    char tmp[AGENTC_STATUS_TEXT_MAX];
    agentc_snprintf(tmp, sizeof tmp, "bg:%zu stall:%zu q:%zu t:%lld", background, stalled, queued,
                    (long long)turn);
    put(host, out, tmp);
    return 0;
}

static const AgcExtStatusLine g_builtin_lines[] = {
    {.struct_size = sizeof(AgcExtStatusLine),
     .region = AGENTC_REGION_STATUS_LEFT,
     .lifetime = AGENTC_STATUS_PERSISTENT,
     .priority = 0,
     .style = 0,
     .render = builtin_model},
    {.struct_size = sizeof(AgcExtStatusLine),
     .region = AGENTC_REGION_STATUS_LEFT,
     .lifetime = AGENTC_STATUS_PERSISTENT,
     .priority = 10,
     .style = 0,
     .render = builtin_think},
    {.struct_size = sizeof(AgcExtStatusLine),
     .region = AGENTC_REGION_STATUS_LEFT,
     .lifetime = AGENTC_STATUS_PERSISTENT,
     .priority = 20,
     .style = 0,
     .render = builtin_tok},
    {.struct_size = sizeof(AgcExtStatusLine),
     .region = AGENTC_REGION_STATUS_LEFT,
     .lifetime = AGENTC_STATUS_PERSISTENT,
     .priority = 30,
     .style = 0,
     .render = builtin_cost},
    {.struct_size = sizeof(AgcExtStatusLine),
     .region = AGENTC_REGION_STATUS_RIGHT,
     .lifetime = AGENTC_STATUS_PERSISTENT,
     .priority = 0,
     .style = AGENTC_PSEG_STYLE_ACCENT,
     .render = builtin_state},
    {.struct_size = sizeof(AgcExtStatusLine),
     .region = AGENTC_REGION_STATUS_RIGHT,
     .lifetime = AGENTC_STATUS_PERSISTENT,
     .priority = 40,
     .style = AGENTC_PSEG_STYLE_DIM,
     .render = builtin_actors},
};

void agentc_status_register_builtin(void) {
    /* Idempotent and self-healing: drop any prior built-in instance first so a
     * re-init cannot duplicate it or keep a stale entry disabled by a missed
     * time budget (agentc_status_register() would no-op on the same identity). */
    for (size_t i = 0; i < sizeof g_builtin_lines / sizeof g_builtin_lines[0]; i++)
        agentc_status_remove(g_builtin_lines[i].region, g_builtin_lines[i].render,
                             g_builtin_lines[i].ud);
    for (size_t i = 0; i < sizeof g_builtin_lines / sizeof g_builtin_lines[0]; i++)
        agentc_status_register(&g_builtin_lines[i]);
}

void agentc_status_builtin_set(const AgcStatusBuiltin *state) {
    if (!state || builtin_eq(state, &g_state))
        return;
    g_state = *state;
    agentc_status_invalidate();
}
