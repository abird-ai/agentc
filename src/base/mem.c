/* mem.c — the allocator (power-of-two size classes + 1 MiB chunks), AgcBuf and
 * AgcVec. Rationale: .agents/design/00-architecture.md §5. */
#include "agentc.h"
#include "plat.h"

#define MEM_CLASSES 12
#define MEM_MAXSMALL 65536u
#define MEM_CHUNK (1u << 20)

static void *free_lists[MEM_CLASSES];
static u8 *chunk_ptr, *chunk_end;
static size_t live;

u8 *agentc_map(size_t n) { return os_map(n); }
void agentc_unmap(u8 *p, size_t n) { (void)os_unmap(p, n); }

void *agentc_alloc(size_t n) {
    /* n + 16 must not wrap: a wrapped total would take the small-class path and
     * hand back a block far smaller than the caller asked for. */
    if (n > (size_t)-1 - 16) agentc_die("allocation size overflow");
    size_t total = n + 16;
    u8 *blk;
    unsigned cls;
    if (total > MEM_MAXSMALL) {
        if (total > (size_t)-1 - 4095) agentc_die("allocation size overflow");
        size_t m = (total + 4095) & ~(size_t)4095;
        blk = os_map(m);
        ((size_t *)blk)[0] = m;
        ((size_t *)blk)[1] = n;
        live++;
        return blk + 16;
    }
    /* class = max(0, ceil(log2(total)) - 5) */
    size_t v = (total - 1) | 31;
    cls = (unsigned)(63 - __builtin_clzll(v)) - 4;
    blk = free_lists[cls];
    if (blk) {
        free_lists[cls] = *(void **)(blk + 16);
    } else {
        size_t bs = (size_t)32 << cls;
        if (!chunk_ptr || chunk_ptr + bs > chunk_end) {
            chunk_ptr = os_map(MEM_CHUNK);
            chunk_end = chunk_ptr + MEM_CHUNK;
        }
        blk = chunk_ptr;
        chunk_ptr += bs;
    }
    ((size_t *)blk)[0] = cls;
    ((size_t *)blk)[1] = n;
    agentc_memset(blk + 16, 0, ((size_t)32 << cls) - 16);
    live++;
    return blk + 16;
}

void *agentc_alloc_try(size_t n) {
    if (n > (size_t)-1 - 16) return NULL;
    size_t total = n + 16;
    if (total <= MEM_MAXSMALL) return agentc_alloc(n);
    if (total > (size_t)-1 - 4095) return NULL;
    size_t m = (total + 4095) & ~(size_t)4095;
    u8 *blk = os_map_try(m);
    if (!blk) return NULL;
    ((size_t *)blk)[0] = m;
    ((size_t *)blk)[1] = n;
    live++;
    return blk + 16;
}

void agentc_free(void *p) {
    if (!p) return;
    u8 *blk = (u8 *)p - 16;
    size_t cls = ((size_t *)blk)[0];
    live--;
    if (cls >= 64) {
        (void)os_unmap(blk, cls);
        return;
    }
    *(void **)(blk + 16) = free_lists[cls];
    free_lists[cls] = blk;
}

static size_t mem_capacity(void *p) {
    size_t cls = ((size_t *)((u8 *)p - 16))[0];
    if (cls >= 64) return cls - 16;
    return ((size_t)32 << cls) - 16;
}

void *agentc_realloc(void *p, size_t n) {
    if (!p) return agentc_alloc(n);
    if (mem_capacity(p) >= n) {
        ((size_t *)((u8 *)p - 16))[1] = n;
        return p;
    }
    void *np = agentc_alloc(n);
    size_t old = ((size_t *)((u8 *)p - 16))[1];
    agentc_memcpy(np, p, old < n ? old : n);
    agentc_free(p);
    return np;
}

size_t agentc_mem_live(void) { return live; }

/* ---------------------------------------------------------------- buffer */
u8 *agentc_buf_reserve(AgcBuf *b, size_t extra) {
    if (extra > (size_t)-1 - b->len - 1) agentc_die("buffer size overflow");
    size_t need = b->len + extra + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        if (cap < need || cap < b->cap) cap = need;   /* doubling wrapped: clamp */
        b->p = agentc_realloc(b->p, cap);
        b->cap = cap;
    }
    return b->p + b->len;
}

void agentc_buf_push(AgcBuf *b, const void *p, size_t n) {
    u8 *w = agentc_buf_reserve(b, n);
    agentc_memcpy(w, p, n);
    b->len += n;
    b->p[b->len] = 0;
}

void agentc_buf_cstr(AgcBuf *b, const char *s) { agentc_buf_push(b, s, agentc_strlen(s)); }

void agentc_buf_byte(AgcBuf *b, u8 c) {
    u8 *w = agentc_buf_reserve(b, 1);
    w[0] = c;
    b->len++;
    b->p[b->len] = 0;
}

void agentc_buf_u64(AgcBuf *b, u64 v) {
    char tmp[32];
    size_t n = agentc_fmt_u64(tmp, v);
    agentc_buf_push(b, tmp, n);
}

void agentc_buf_i64(AgcBuf *b, i64 v) {
    char tmp[32];
    size_t n = agentc_fmt_i64(tmp, v);
    agentc_buf_push(b, tmp, n);
}

int agentc_buf_printf(AgcBuf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = agentc_vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    /* agentc_vsnprintf follows C99 and returns the length the text would have
     * had, which is larger than the buffer when the text does not fit: pushing n
     * bytes would read past tmp. Only what actually fit goes in; callers that
     * need the whole text build it piecewise. */
    if (n > 0) agentc_buf_push(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
    return n;
}

void agentc_buf_clear(AgcBuf *b) {
    b->len = 0;
    if (b->p) b->p[0] = 0;
}

void agentc_buf_free(AgcBuf *b) {
    agentc_free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* ------------------------------------------------------------------- vec */
void *agentc_vec_push(AgcVec *v, size_t item) {
    if (item == 0) item = 1;
    if (v->len == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 8;
        if (cap < v->cap || cap > (size_t)-1 / item) agentc_die("vector size overflow");
        v->p = agentc_realloc(v->p, cap * item);
        v->cap = cap;
    }
    u8 *slot = (u8 *)v->p + v->len * item;
    v->len++;
    agentc_memset(slot, 0, item);
    return slot;
}

void agentc_vec_free(AgcVec *v) {
    agentc_free(v->p);
    v->p = NULL;
    v->len = v->cap = 0;
}
