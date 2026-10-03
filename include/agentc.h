/* agentc.h — core types and the base library contract.
 *
 * Freestanding C23: only <stddef.h>, <stdint.h>, <stdbool.h>, <stdarg.h> are
 * included (all freestanding headers). No libc, no stdio, no malloc.
 */
#ifndef AGENTC_H
#define AGENTC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;

/* ---------------------------------------------------------------- memory */
/* Power-of-two size classes, zeroed payloads. agentc_alloc aborts on OOM, so its
 * result is never NULL (annotated for analyzers). */
void *agentc_alloc(size_t n) __attribute__((returns_nonnull));
void *agentc_alloc_try(size_t n);                /* large blocks only: NULL on OOM */
void *agentc_realloc(void *p, size_t n) __attribute__((returns_nonnull));
void agentc_free(void *p);
size_t agentc_mem_live(void);                    /* live blocks, for tests */
u8 *agentc_map(size_t n);                        /* raw pages, zeroed */
void agentc_unmap(u8 *p, size_t n);

/* ---------------------------------------------------------------- buffer */
typedef struct {
    u8 *p;
    size_t len, cap;
} AgcBuf;

u8 *agentc_buf_reserve(AgcBuf *b, size_t extra);  /* ptr to write pos, does not bump len */
void agentc_buf_push(AgcBuf *b, const void *p, size_t n);
void agentc_buf_cstr(AgcBuf *b, const char *s);
void agentc_buf_byte(AgcBuf *b, u8 c);
void agentc_buf_u64(AgcBuf *b, u64 v);
void agentc_buf_i64(AgcBuf *b, i64 v);
/* C99 vsnprintf contract: returns the length the full output would need, even
 * when it exceeds the 1 KiB internal scratch, so the appended bytes may be
 * truncated. Build long text piecewise (or use the _used variants) when the
 * whole string must be appended. */
int  agentc_buf_printf(AgcBuf *b, const char *fmt, ...);
void agentc_buf_clear(AgcBuf *b);                 /* len = 0, keeps capacity */
void agentc_buf_free(AgcBuf *b);

/* ------------------------------------------------------------------- vec */
typedef struct {
    void *p;
    size_t len, cap;
} AgcVec;

void *agentc_vec_push(AgcVec *v, size_t item);    /* zeroed slot */
void agentc_vec_free(AgcVec *v);

/* ------------------------------------------------------------------ text */
size_t agentc_strlen(const char *s);
bool agentc_streq(const char *a, const char *b);
bool agentc_str_eq(const char *a, size_t alen, const char *b, size_t blen);
bool agentc_str_eq_cstr(const char *a, size_t alen, const char *cstr);
bool agentc_str_starts(const char *a, size_t alen, const char *prefix);
bool agentc_str_ieq(const char *a, size_t alen, const char *b, size_t blen);
char *agentc_strdup(const char *s) __attribute__((returns_nonnull));
char *agentc_strdup_len(const char *s, size_t n) __attribute__((returns_nonnull));
const char *agentc_str_str(const char *hay, const char *needle);
i64 agentc_str_find(const char *hay, size_t hlen, const char *needle, size_t nlen);
i64 agentc_parse_i64(const char *p, size_t n, bool *ok);
u64 agentc_parse_u64(const char *p, size_t n, bool *ok);
size_t agentc_fmt_u64(char *out, u64 v);         /* no NUL, returns length */
size_t agentc_fmt_i64(char *out, i64 v);
/* C99 snprintf return contract: the length the full output would need, even
 * when the buffer is too small; the output is still NUL-terminated when
 * cap > 0. */
int agentc_snprintf(char *out, size_t cap, const char *fmt, ...);
int agentc_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
/* Like agentc_snprintf/vsnprintf, but returns the number of bytes actually
 * written (excluding the NUL), never the would-be length. */
int agentc_snprintf_used(char *out, size_t cap, const char *fmt, ...);
int agentc_vsnprintf_used(char *out, size_t cap, const char *fmt, va_list ap);
void *agentc_memcpy(void *d, const void *s, size_t n);
void *agentc_memmove(void *d, const void *s, size_t n);
void *agentc_memset(void *d, int c, size_t n);
bool agentc_memeq(const void *a, const void *b, size_t n);
size_t agentc_utf8_encode(u32 cp, u8 *out);

/* --------------------------------------------------------------- logging */
/* levels: 0=debug 1=info 2=warn 3=error */
void agentc_logs(int level, const char *msg);
/* 0=debug 1=info 2=warn 3=error 4=quiet; default info (env AGENTC_LOG overrides) */
void agentc_log_set_level(int level);
void agentc_logf(int level, const char *fmt, ...);
void agentc_die(const char *msg) __attribute__((noreturn));
/* Register one cleanup callback run by agentc_die() before exiting (terminal
 * restore, temp files). Must not allocate; the last registration wins. */
void agentc_atexit(void (*cb)(void));
void agentc_out(const void *p, size_t n);        /* stdout; unsafe bytes -> U+FFFD */
void agentc_outs(const char *s);
void agentc_outf(const char *fmt, ...);
void agentc_out_u64(u64 v);
void agentc_out_nl(void);

/* ------------------------------------------------------------- json/jsonc */
typedef struct AgcJson AgcJson;

enum {
    AGENTC_JSON_NULL = 0,
    AGENTC_JSON_TRUE,
    AGENTC_JSON_FALSE,
    AGENTC_JSON_NUM,
    AGENTC_JSON_STR,
    AGENTC_JSON_ARR,
    AGENTC_JSON_OBJ,
};

/* Parse into the process default arena, cleared by every parse and by reset(),
 * so it holds one live document at a time; agentc_json_parse_in() is the
 * isolated-arena form. */
AgcJson *agentc_json_parse(const char *p, size_t n);
void agentc_json_reset(void);
AgcJson *agentc_json_get(const AgcJson *o, const char *key);
const char *agentc_json_get_str(const AgcJson *o, const char *key);        /* NUL-terminated or NULL */
i64 agentc_json_get_int(const AgcJson *o, const char *key, i64 dflt);
bool agentc_json_get_bool(const AgcJson *o, const char *key, bool dflt);
int agentc_json_type(const AgcJson *v);                                     /* AGENTC_JSON_* or -1 */
size_t agentc_json_len(const AgcJson *arr);
AgcJson *agentc_json_at(const AgcJson *arr, size_t i);
const char *agentc_json_str(const AgcJson *v, size_t *len);                 /* NULL unless string */
const char *agentc_json_num(const AgcJson *v, size_t *len);                 /* raw number text */
bool agentc_json_is(const AgcJson *v, const char *s);
/* Object iteration: pair i's key and value; NULL key past the end or when the
 * value is not an object. Lets callers re-emit or merge documents without a
 * schema. */
const AgcJson *agentc_json_key_at(const AgcJson *o, size_t i);
const AgcJson *agentc_json_val_at(const AgcJson *o, size_t i);

/* JSON arenas. A parsed document keeps every node and decoded string in one
 * arena; the pointers stay valid until that arena is cleared or freed.
 * agentc_json_parse() uses a process default arena (source-compatible with the
 * old one-document-at-a-time API); agentc_json_parse_in() lets independent
 * components own their document, so one parse can no longer invalidate
 * another's. parse_in clears the arena on entry and on failure, so a caller
 * re-parsing in the same arena never needs an explicit clear first. */
typedef struct AgcJsonArena AgcJsonArena;
AgcJsonArena *agentc_json_arena_new(size_t initial_bytes);  /* NULL on OOM; 0 = 1 MiB block */
void agentc_json_arena_free(AgcJsonArena *a);               /* NULL-safe */
void agentc_json_arena_clear(AgcJsonArena *a);              /* recycle, NULL-safe */
AgcJson *agentc_json_parse_in(AgcJsonArena *a, const char *p, size_t n);

/* The streaming JSON writer (AgcJsonW) is declared in wire.h. */

/* --------------------------------------------------------------- version */
#include "version.h"                            /* generated: AGENTC_VERSION */
const char *agentc_version(void);

#endif /* AGENTC_H */
