/* input.h — incremental terminal escape-sequence parser. */
#ifndef AGENTC_TUI_INPUT_H
#define AGENTC_TUI_INPUT_H

#include "agentc.h"

enum {
    K_NONE = 0,
    K_CHAR,
    K_ENTER,
    K_TAB,
    K_ESC,
    K_BACKSPACE,
    K_DELETE,
    K_UP,
    K_DOWN,
    K_LEFT,
    K_RIGHT,
    K_HOME,
    K_END,
    K_PGUP,
    K_PGDN,
    K_INSERT,
    K_PASTE,
    K_F1,
    K_F2,
    K_F3,
    K_F4,
};

enum {
    MOD_ALT = 1,
    MOD_CTRL = 2,
    MOD_SHIFT = 4,
};

typedef struct {
    int code;
    int mods;
    u32 cp;              /* K_CHAR */
    const u8 *paste;     /* K_PASTE: valid only during the callback */
    size_t paste_len;
} Key;

typedef void (*KeyFn)(void *ud, const Key *k);

typedef struct {
    int state;           /* internal */
    u8 seq[32];
    size_t seqlen;
    i64 esc_at_ns;
    AgcBuf paste;
    bool last_cr;
    int utf8_need;
    u32 utf8_cp;
    u32 utf8_min;
    int utf8_mods;
} Input;

void input_init(Input *in);
void input_free(Input *in);
/* Decode bytes; invokes cb for every complete key. Never blocks. */
void input_feed(Input *in, const u8 *p, size_t n, i64 now_ns, KeyFn cb, void *ud);
/* Call from the idle path: emits a lone ESC once `esc_timeout_ms` has passed
 * since the ESC byte arrived. */
void input_idle(Input *in, i64 now_ns, int esc_timeout_ms, KeyFn cb, void *ud);
bool input_busy(const Input *in);

#endif /* AGENTC_TUI_INPUT_H */
