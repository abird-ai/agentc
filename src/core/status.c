/* status.c — the one provider table behind the status line.
 *
 * Built-in segments and extension segments take exactly the same route: register
 * an AgcStatusProvider, get called when the version changes, have the output
 * validated and copied. The table is static and registration never allocates,
 * so a provider drop or a extension shutdown cannot move the allocator baseline.
 *
 * A provider that misses its time budget is disabled for the rest of the
 * process: it stalls at most one frame and every later snapshot skips it.
 * Malformed segments (NULL/empty text, unknown slot/style, control characters,
 * short struct) are dropped individually so one bad segment cannot hide a good
 * one.
 */
#include "status.h"
#include "plat.h"

#define AGENTC_STATUS_MAX_PROVIDERS 16
/* Per-provider time budget. Formatting is microseconds; anything above this is
 * blocking and must not run again. */
#define AGENTC_STATUS_BUDGET_NS 2000000LL

typedef struct {
    AgcExtStatusProvider provider;
    void *userdata;
    u64 id;
    bool disabled;
} StatusProvider;

static StatusProvider g_providers[AGENTC_STATUS_MAX_PROVIDERS];
static size_t g_nproviders;
static u64 g_version = 1;
static u64 g_next_id;
static u64 g_seq;

void agentc_status_register(AgcExtStatusProvider provider, void *userdata) {
    if (!provider) {
        agentc_logf(3, "status: NULL provider ignored");
        return;
    }
    for (size_t i = 0; i < g_nproviders; i++) {
        StatusProvider *p = &g_providers[i];
        if (p->provider == provider && p->userdata == userdata) return;
    }
    if (g_nproviders == AGENTC_STATUS_MAX_PROVIDERS) {
        agentc_logf(3, "status: provider table full, registration ignored");
        return;
    }
    StatusProvider *p = &g_providers[g_nproviders++];
    p->provider = provider;
    p->userdata = userdata;
    p->id = ++g_next_id;
    p->disabled = false;
    g_version++;
}

void agentc_status_remove(AgcExtStatusProvider provider, void *userdata) {
    for (size_t i = 0; i < g_nproviders; i++) {
        if (g_providers[i].provider != provider || g_providers[i].userdata != userdata)
            continue;
        /* shift the tail down so registration order is preserved */
        for (size_t j = i + 1; j < g_nproviders; j++) g_providers[j - 1] = g_providers[j];
        g_nproviders--;
        agentc_memset(&g_providers[g_nproviders], 0, sizeof g_providers[0]);
        g_version++;
        return;
    }
}

void agentc_status_reset(void) {
    agentc_memset(g_providers, 0, sizeof g_providers);
    g_nproviders = 0;
    g_next_id = 0;
    g_version++;
}

u64 agentc_status_version(void) { return g_version; }

void agentc_status_invalidate(void) { g_version++; }

/* Copies one provider segment into `v`. Returns the text length, or 0 when the
 * segment is invisible or malformed (each drop is logged except empty text). */
static size_t status_copy(const AgcExtStatusSegment *s, AgcStatusValue *v, u64 id) {
    if (!s ||
        s->struct_size < (uint32_t)(offsetof(AgcExtStatusSegment, text) + sizeof(s->text))) {
        agentc_logf(2, "status: provider #%llu returned a short segment, dropped",
                    (unsigned long long)id);
        return 0;
    }
    if (s->slot != AGENTC_PSEG_SLOT_LEFT && s->slot != AGENTC_PSEG_SLOT_RIGHT) {
        agentc_logf(2, "status: provider #%llu used an unknown slot %u, dropped",
                    (unsigned long long)id, s->slot);
        return 0;
    }
    if ((s->style & ~(uint32_t)AGENTC_PSEG_STYLE_ALL) != 0) {
        agentc_logf(2, "status: provider #%llu used an unknown style 0x%x, dropped",
                    (unsigned long long)id, s->style);
        return 0;
    }
    if (!s->text || !s->text[0]) return 0;   /* empty is invisible by contract */
    size_t n = 0;
    while (s->text[n] && n + 1 < AGENTC_STATUS_TEXT_MAX) {
        u8 c = (u8)s->text[n];
        if (c < 0x20 || c == 0x7f) {
            agentc_logf(2, "status: provider #%llu embedded a control character, dropped",
                        (unsigned long long)id);
            return 0;
        }
        v->text[n] = s->text[n];
        n++;
    }
    v->text[n] = 0;
    v->slot = s->slot;
    v->priority = s->priority;
    v->style = s->style;
    return n;
}

static bool value_less(const AgcStatusValue *a, const AgcStatusValue *b) {
    if (a->slot != b->slot) return a->slot < b->slot;
    if (a->priority != b->priority) return a->priority < b->priority;
    return a->seq < b->seq;
}

size_t agentc_status_snapshot(AgcStatusValue *out, size_t max) {
    if (!out || max == 0) return 0;
    g_seq = 0;
    size_t n = 0;
    size_t overflow = 0;
    bool dropped_provider = false;

    for (size_t i = 0; i < g_nproviders; i++) {
        StatusProvider *p = &g_providers[i];
        if (p->disabled) continue;
        AgcExtStatusSegment raw[AGENTC_STATUS_MAX_PROVIDER_SEGMENTS];
        char arena[AGENTC_STATUS_ARENA_CAP];
        i64 t0 = os_now_ns(OS_CLOCK_MONOTONIC);
        size_t got = p->provider(p->userdata, raw, AGENTC_STATUS_MAX_PROVIDER_SEGMENTS,
                                 arena, sizeof arena);
        i64 t1 = os_now_ns(OS_CLOCK_MONOTONIC);
        if (t1 - t0 > AGENTC_STATUS_BUDGET_NS) {
            agentc_logf(2,
                        "status: provider #%llu overran the %lldms budget, dropped",
                        (unsigned long long)p->id,
                        (long long)(AGENTC_STATUS_BUDGET_NS / 1000000));
            p->disabled = true;
            dropped_provider = true;
            continue;
        }
        if (got > AGENTC_STATUS_MAX_PROVIDER_SEGMENTS) {
            agentc_logf(2, "status: provider #%llu returned %llu segments, truncated",
                        (unsigned long long)p->id, (unsigned long long)got);
            got = AGENTC_STATUS_MAX_PROVIDER_SEGMENTS;
        }
        for (size_t k = 0; k < got; k++) {
            if (n == max) {
                overflow += got - k;
                break;
            }
            size_t len = status_copy(&raw[k], &out[n], p->id);
            if (len == 0) continue;
            out[n].seq = g_seq++;
            n++;
        }
    }
    if (overflow)
        agentc_logf(1, "status: %llu segment(s) over the %llu-segment cap, dropped",
                    (unsigned long long)overflow, (unsigned long long)max);
    if (dropped_provider) agentc_status_invalidate();

    /* insertion sort is fine at this size and keeps the sort dependency-free */
    for (size_t i = 1; i < n; i++) {
        AgcStatusValue key = out[i];
        size_t j = i;
        while (j > 0 && value_less(&key, &out[j - 1])) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return n;
}
