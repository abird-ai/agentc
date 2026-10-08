/* glob.c — bounded, iterative glob matcher (see base/glob.h).
 *
 * The pattern compiles to a miniature NFA that is simulated over the text one
 * byte at a time, so there is no recursion and no exponential backtracking:
 * the worst case is bounded by AGENTC_GLOB_STEP_BUDGET transitions, after
 * which the match reports false. The states mirror the accepted glob language:
 *
 *   `*`    zero or more bytes excluding `/`      `**`  zero or more bytes
 *   `?`    one byte excluding `/`                `**` + `/`  empty or bytes then `/`
 *   `[..]` one byte in the class, excluding `/`  `\x`  literal x
 *
 * The double-star directory prefix needs two states because the
 * zero-directories alternative (skip the whole token) must not become
 * available after the star has consumed bytes.
 */
#include "base/glob.h"

enum {
    GLOB_LIT = 0,       /* one literal byte (also an escaped byte) */
    GLOB_ANY,           /* '?': any byte but '/' */
    GLOB_CLASS,         /* '[...]': one byte in the class, but not '/' */
    GLOB_STAR,          /* '*': zero or more bytes but '/' */
    GLOB_DSTAR,         /* '**': zero or more bytes */
    GLOB_DSTAR_START,   /* double-star-slash: skip the token, or enter GLOB_DSTAR_STAR */
    GLOB_DSTAR_STAR,    /* the star half of the directory prefix: zero or more bytes */
    GLOB_ACCEPT,
};

typedef struct {
    u8 op;
    u8 ch;              /* GLOB_LIT byte */
    u16 cls;            /* pattern offset of '[' for GLOB_CLASS */
} GlobInsn;

/* p points at '['; returns the position just past the class and reports
 * whether byte c matches it. The grammar mirrors the class parser this matcher
 * inherited: a leading `!`/`^` negates, `\` escapes, `lo-hi` ranges, and a
 * missing `]` runs to the end of the pattern. */
static const char *glob_class_scan(const char *p, unsigned char c, bool *hit) {
    p++;
    bool neg = false;
    if (*p == '!' || *p == '^') {
        neg = true;
        p++;
    }
    bool found = false;
    bool first = true;
    while (*p && (*p != ']' || first)) {
        first = false;
        /* Class endpoints are bytes, not signed chars: a pattern byte >= 0x80
         * must compare as 128..255 on every target, so `[a-\xff]` matches. */
        unsigned char lo, hi;
        if (p[0] == '\\' && p[1]) p++;
        lo = (unsigned char)*p++;
        hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            if (p[0] == '\\' && p[1]) p++;
            hi = (unsigned char)*p++;
        }
        if (c >= lo && c <= hi) found = true;
    }
    if (*p == ']') p++;
    *hit = found != neg;
    return p;
}

/* Compile pattern into insn[0..cap); returns the instruction count including
 * the trailing GLOB_ACCEPT, or 0 when the pattern does not fit. */
static size_t glob_compile(const char *pattern, GlobInsn *insn, size_t cap) {
    size_t n = 0;
    size_t i = 0;

    while (pattern[i] != '\0') {
        size_t need = 1;                 /* literals/classes emit one insn */
        if (pattern[i] == '*' && pattern[i + 1] == '*' && pattern[i + 2] == '/')
            need = 3;                    /* GLOB_DSTAR_START, GLOB_DSTAR_STAR, '/' */
        if (n + need + 1 > cap) return 0; /* atom plus the trailing GLOB_ACCEPT */
        insn[n].op = GLOB_LIT;
        insn[n].ch = 0;
        insn[n].cls = 0;
        if (pattern[i] == '*') {
            if (pattern[i + 1] == '*' && pattern[i + 2] == '/') {
                insn[n].op = GLOB_DSTAR_START;
                n++;
                insn[n].op = GLOB_DSTAR_STAR;
                n++;
                insn[n].op = GLOB_LIT;
                insn[n].ch = '/';
                n++;
                i += 3;
                continue;
            }
            if (pattern[i + 1] == '*') {
                insn[n].op = GLOB_DSTAR;
                n++;
                i += 2;
                continue;
            }
            insn[n].op = GLOB_STAR;
            n++;
            i++;
            continue;
        }
        if (pattern[i] == '?') {
            insn[n].op = GLOB_ANY;
            i++;
        } else if (pattern[i] == '[') {
            bool dummy = false;
            const char *after = glob_class_scan(pattern + i, 0, &dummy);
            insn[n].op = GLOB_CLASS;
            insn[n].cls = (u16)i;
            i = (size_t)(after - pattern);
        } else if (pattern[i] == '\\' && pattern[i + 1] != '\0') {
            insn[n].ch = (u8)pattern[i + 1];
            i += 2;
        } else {
            insn[n].ch = (u8)pattern[i];
            i++;
        }
        n++;
    }
    if (n + 1 > cap) return 0;
    insn[n].op = GLOB_ACCEPT;
    insn[n].ch = 0;
    insn[n].cls = 0;
    return n + 1;
}

/* Epsilon closure of `set`, whose ninsn entries include GLOB_ACCEPT. Every
 * epsilon edge moves forward, so one ascending pass reaches the fixpoint.
 * Returns false when the step budget runs out. */
static bool glob_closure(u8 *set, const GlobInsn *insn, size_t ninsn, u32 *budget) {
    for (size_t i = 0; i < ninsn; i++) {
        if (*budget == 0) return false;
        (*budget)--;
        if (!set[i]) continue;
        switch (insn[i].op) {
        case GLOB_STAR:
        case GLOB_DSTAR:
        case GLOB_DSTAR_STAR:
            set[i + 1] = 1;
            break;
        case GLOB_DSTAR_START:
            set[i + 1] = 1;             /* enter the star */
            set[i + 3] = 1;             /* skip the prefix (zero directories) */
            break;
        default:
            break;
        }
    }
    return true;
}

bool agentc_glob_match(const char *pattern, const char *text) {
    GlobInsn insn[AGENTC_GLOB_MAX_PATTERN + 2];
    u8 cur[AGENTC_GLOB_MAX_PATTERN + 2];
    u8 nxt[AGENTC_GLOB_MAX_PATTERN + 2];
    u32 budget = AGENTC_GLOB_STEP_BUDGET;
    size_t n, accept;

    if (!pattern || !text) return false;
    for (size_t plen = 0; pattern[plen]; plen++)
        if (plen >= AGENTC_GLOB_MAX_PATTERN) return false;

    n = glob_compile(pattern, insn, AGENTC_GLOB_MAX_PATTERN + 2);
    if (n == 0) return false;
    accept = n - 1;                     /* GLOB_ACCEPT is the last instruction */

    agentc_memset(cur, 0, n);
    agentc_memset(nxt, 0, n);
    cur[0] = 1;
    if (!glob_closure(cur, insn, n, &budget)) return false;

    for (const char *t = text; *t; t++) {
        if (budget == 0) return false;
        budget--;
        agentc_memset(nxt, 0, n);
        for (size_t i = 0; i < n; i++) {
            if (budget == 0) return false;
            budget--;
            if (!cur[i]) continue;
            switch (insn[i].op) {
            case GLOB_LIT:
                if (*t == (char)insn[i].ch) nxt[i + 1] = 1;
                break;
            case GLOB_ANY:
                if (*t != '/') nxt[i + 1] = 1;
                break;
            case GLOB_CLASS: {
                bool hit = false;
                (void)glob_class_scan(pattern + insn[i].cls, (unsigned char)*t, &hit);
                if (*t != '/' && hit) nxt[i + 1] = 1;
                break;
            }
            case GLOB_STAR:
                if (*t != '/') nxt[i] = 1;
                break;
            case GLOB_DSTAR:
            case GLOB_DSTAR_STAR:
                nxt[i] = 1;
                break;
            default:                    /* GLOB_DSTAR_START and GLOB_ACCEPT */
                break;
            }
        }
        if (!glob_closure(nxt, insn, n, &budget)) return false;
        agentc_memcpy(cur, nxt, n);
    }
    return cur[accept] != 0;
}
