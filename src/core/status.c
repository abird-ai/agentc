/* status.c — the one status-line table behind every region.
 *
 * Built-in lines and extension lines take exactly the same route: register an
 * AgcExtStatusLine, get called when the version changes (chrome) or at a
 * step/turn boundary (event), have the output validated and copied. The table
 * is static and registration never allocates, so a line drop or an extension
 * shutdown cannot move the allocator baseline.
 *
 * A line that misses its time budget is disabled for the rest of the process:
 * it stalls at most one frame and every later snapshot skips it. Malformed rows
 * (control characters, empty text, unknown region/style) are dropped
 * individually so one bad line cannot hide a good one.
 */
#include "status.h"
#include "ext.h"
#include "plat.h"

#define AGENTC_STATUS_MAX_PROVIDERS 24
/* Per-line time budget. Formatting is microseconds; anything above this is
 * blocking and must not run again. */
#define AGENTC_STATUS_BUDGET_NS 2000000LL

typedef struct {
    AgcExtStatusLine line;
    void *ud;
    u64 id;
    u64 seq;
    bool disabled;
    bool emitted; /* ONCE lines */
} StatusLine;

static StatusLine g_lines[AGENTC_STATUS_MAX_PROVIDERS];
static size_t g_nlines;
static u64 g_version = 1;
static u64 g_next_id;
static u64 g_seq;

static bool region_valid(int region) {
    return region >= 0 && region < AGENTC_REGION_COUNT;
}

static bool lifetime_valid(int lifetime) {
    return lifetime == AGENTC_STATUS_PERSISTENT || lifetime == AGENTC_STATUS_PER_TURN ||
           lifetime == AGENTC_STATUS_ONCE;
}

void agentc_status_register(const AgcExtStatusLine *line) {
    if (!line ||
        line->struct_size < (uint32_t)(offsetof(AgcExtStatusLine, render) + sizeof line->render)) {
        agentc_logf(3, "status: short/absent line ignored");
        return;
    }
    if (!line->render) {
        agentc_logf(3, "status: line without render ignored");
        return;
    }
    if (!region_valid(line->region)) {
        agentc_logf(3, "status: line with unknown region %d ignored", line->region);
        return;
    }
    if (!lifetime_valid(line->lifetime)) {
        agentc_logf(3, "status: line with unknown lifetime %d ignored", line->lifetime);
        return;
    }
    for (size_t i = 0; i < g_nlines; i++) {
        StatusLine *p = &g_lines[i];
        if (p->line.region == line->region && p->line.render == line->render && p->ud == line->ud)
            return;
    }
    if (g_nlines == AGENTC_STATUS_MAX_PROVIDERS) {
        agentc_logf(3, "status: line table full, registration ignored");
        return;
    }
    StatusLine *p = &g_lines[g_nlines++];
    agentc_memset(p, 0, sizeof *p);
    p->line = *line;
    p->ud = line->ud;
    p->id = ++g_next_id;
    p->seq = ++g_seq;
    g_version++;
}

void agentc_status_remove(int region,
                          int (*render)(const AgcExtHost *, void *, const AgcExtStatusCtx *,
                                        void *),
                          void *userdata) {
    for (size_t i = 0; i < g_nlines; i++) {
        StatusLine *p = &g_lines[i];
        if (p->line.region != region || p->line.render != render || p->ud != userdata)
            continue;
        for (size_t j = i + 1; j < g_nlines; j++)
            g_lines[j - 1] = g_lines[j];
        g_nlines--;
        agentc_memset(&g_lines[g_nlines], 0, sizeof g_lines[0]);
        g_version++;
        return;
    }
}

void agentc_status_reset(void) {
    agentc_memset(g_lines, 0, sizeof g_lines);
    g_nlines = 0;
    g_next_id = 0;
    g_seq = 0;
    g_version++;
}

u64 agentc_status_version(void) {
    return g_version;
}

void agentc_status_invalidate(void) {
    g_version++;
}

/* Copies one rendered row into `v`. Returns the text length, or 0 when the row
 * is invisible or malformed (each drop is logged except empty text). */
static size_t row_copy(const char *text, int region, int32_t priority, uint32_t style,
                       AgcStatusValue *v, u64 id) {
    if ((style & ~(uint32_t)AGENTC_PSEG_STYLE_ALL) != 0) {
        agentc_logf(2, "status: line #%llu used an unknown style 0x%x, dropped",
                    (unsigned long long)id, style);
        return 0;
    }
    if (!text || !text[0])
        return 0; /* empty is invisible by contract */
    size_t n = 0;
    while (text[n] && n + 1 < AGENTC_STATUS_TEXT_MAX) {
        u8 c = (u8)text[n];
        if (c < 0x20 || c == 0x7f) {
            agentc_logf(2, "status: line #%llu embedded a control character, dropped",
                        (unsigned long long)id);
            return 0;
        }
        v->text[n] = text[n];
        n++;
    }
    v->text[n] = 0;
    v->region = region;
    v->priority = priority;
    v->style = style;
    return n;
}

static bool value_less(const AgcStatusValue *a, const AgcStatusValue *b) {
    if (a->region != b->region)
        return a->region < b->region;
    if (a->priority != b->priority)
        return a->priority < b->priority;
    return a->seq < b->seq;
}

/* Renders one line into a fresh buffer. Returns 0 on success, <0 on a disabled
 * or over-budget line. The line's own output rows are appended to `rows`. */
static int render_line(StatusLine *p, const AgcExtStatusCtx *ctx, AgcBuf *buf) {
    if (p->disabled)
        return -1;
    i64 t0 = os_now_ns(OS_CLOCK_THREAD_CPUTIME);
    agentc_memset(buf, 0, sizeof *buf);
    p->line.render(agentc_ext_host(), p->ud, ctx, buf);
    i64 t1 = os_now_ns(OS_CLOCK_THREAD_CPUTIME);
    /* An unreadable or non-monotonic clock cannot prove an overrun, and a
     * coarse process clock can quantize a healthy render past the budget on
     * some hosts; only a measurable delta disables the line. */
    if (t0 >= 0 && t1 >= t0 && t1 - t0 > AGENTC_STATUS_BUDGET_NS) {
        agentc_logf(2, "status: line #%llu overran the %lldms budget, dropped",
                    (unsigned long long)p->id, (long long)(AGENTC_STATUS_BUDGET_NS / 1000000));
        p->disabled = true;
        agentc_buf_free(buf);
        return -1;
    }
    return 0;
}

/* Splits a rendered buffer into rows, emitting at most
 * AGENTC_STATUS_MAX_LINES_PER_REGION rows. Returns the number of rows written. */
static size_t split_rows(StatusLine *p, AgcBuf *buf, AgcStatusValue *out, size_t max,
                         size_t *out_n) {
    const char *s = (const char *)buf->p;
    size_t len = buf->len;
    size_t rows = 0;
    size_t i = 0;
    int region = p->line.region;
    while (i < len) {
        size_t start = i;
        while (i < len && s[i] != '\n')
            i++;
        size_t seg = i - start;
        if (i < len)
            i++; /* skip '\n' */
        if (seg == 0)
            continue;
        if (rows >= AGENTC_STATUS_MAX_LINES_PER_REGION) {
            agentc_logf(2, "status: line #%llu exceeded %d rows in region %d, clipped",
                        (unsigned long long)p->id, AGENTC_STATUS_MAX_LINES_PER_REGION, region);
            break;
        }
        if (*out_n == max)
            break;
        char tmp[AGENTC_STATUS_TEXT_MAX];
        size_t copy = seg < AGENTC_STATUS_TEXT_MAX - 1 ? seg : AGENTC_STATUS_TEXT_MAX - 1;
        agentc_memcpy(tmp, s + start, copy);
        tmp[copy] = 0;
        if (row_copy(tmp, region, p->line.priority, p->line.style, &out[*out_n], p->id))
            (*out_n)++;
        rows++;
    }
    return rows;
}

/* True when `region` is a chrome region (has a persistent row). */
static bool chrome_region(int region) {
    return region == AGENTC_REGION_ABOVE_COMPOSER || region == AGENTC_REGION_BELOW_COMPOSER ||
           region == AGENTC_REGION_STATUS_LEFT || region == AGENTC_REGION_STATUS_RIGHT ||
           region == AGENTC_REGION_BELOW_STATUS;
}

size_t agentc_status_snapshot(int region, AgcStatusValue *out, size_t max) {
    if (!out || max == 0)
        return 0;
    g_seq = 0;
    size_t n = 0;
    bool dropped = false;

    /* Registration order is stable even when a snapshot spans regions. */
    for (size_t i = 0; i < g_nlines; i++) {
        StatusLine *p = &g_lines[i];
        if (p->disabled)
            continue;
        if (p->line.lifetime == AGENTC_STATUS_ONCE && p->emitted)
            continue;
        if (region != AGENTC_STATUS_ALL_REGIONS && p->line.region != region)
            continue;
        if (region == AGENTC_STATUS_ALL_REGIONS && !chrome_region(p->line.region))
            continue;
        AgcBuf buf = {0};
        if (render_line(p, NULL, &buf) < 0) {
            dropped = true;
            continue;
        }
        size_t before = n;
        split_rows(p, &buf, out, max, &n);
        agentc_buf_free(&buf);
        if (p->line.lifetime == AGENTC_STATUS_ONCE && n > before)
            p->emitted = true;
        if (n == max)
            break;
    }
    if (n == max && g_nlines) {
        /* A full result is not necessarily an overflow: the caller passed the
         * bound it owns, so remain silent rather than guess. */
    }
    if (dropped)
        agentc_status_invalidate();

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

size_t agentc_status_emit(int region, const AgcExtStatusCtx *ctx, AgcBuf *out) {
    if (!out || !region_valid(region))
        return 0;
    size_t rows = 0;
    for (size_t i = 0; i < g_nlines; i++) {
        StatusLine *p = &g_lines[i];
        if (p->disabled || p->line.region != region)
            continue;
        if (p->line.lifetime == AGENTC_STATUS_PERSISTENT)
            continue;
        if (p->line.lifetime == AGENTC_STATUS_ONCE && p->emitted)
            continue;
        AgcBuf buf = {0};
        if (render_line(p, ctx, &buf) < 0)
            continue;
        const char *s = (const char *)buf.p;
        size_t len = buf.len;
        size_t j = 0;
        while (j < len) {
            size_t start = j;
            while (j < len && s[j] != '\n')
                j++;
            size_t seg = j - start;
            if (j < len)
                j++;
            if (seg == 0)
                continue;
            if (rows >= AGENTC_STATUS_MAX_LINES_PER_REGION)
                break;
            if (rows)
                agentc_buf_byte(out, (u8)'\n');
            agentc_buf_push(out, s + start, seg);
            rows++;
        }
        agentc_buf_free(&buf);
        if (p->line.lifetime == AGENTC_STATUS_ONCE && rows)
            p->emitted = true;
    }
    return rows;
}

bool agentc_status_has_region(int region) {
    if (!region_valid(region))
        return false;
    for (size_t i = 0; i < g_nlines; i++) {
        if (g_lines[i].disabled)
            continue;
        if (g_lines[i].line.region == region)
            return true;
    }
    return false;
}
