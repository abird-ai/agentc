/* plat/win/console.c — Windows console: raw mode, size and the input thread.
 *
 * WSAPoll cannot see console input handles, so when stdin is a console we
 * create a byte pipe and a reader thread: ReadConsoleInputW events (key down,
 * window resize) are converted to xterm-style bytes and written to the pipe,
 * which then behaves like any other non-blocking fd 0. The real console handle
 * is kept in the fd table's aux slot so os_tty_raw/restore/size can still use
 * SetConsoleMode/GetConsoleScreenBufferInfo.
 *
 * Resize events set a flag (there are no signals on Windows); os_poll fires the
 * registered os_sig_winch() handler on the main thread. The TUI also compares
 * os_tty_size() each frame, so it does not depend on the handler.
 */
#include "agentc.h"
#include "plat.h"
#include "win.h"

#define WIN_KEY_EVENT 0x0001
#define WIN_WINDOW_BUFFER_SIZE_EVENT 0x0004

#define WIN_RIGHT_ALT_PRESSED 0x0001
#define WIN_LEFT_ALT_PRESSED 0x0002
#define WIN_RIGHT_CTRL_PRESSED 0x0004
#define WIN_LEFT_CTRL_PRESSED 0x0008
#define WIN_SHIFT_PRESSED 0x0010

static WinHandle g_con_in;
static WinHandle g_pipe_w;
static bool g_winch;
static void (*g_winch_handler)(void);

static void pipe_write_all(const u8 *p, size_t n) {
    while (n > 0) {
        WinDWORD w = 0;
        WinDWORD chunk = n > 0x10000 ? 0x10000 : (WinDWORD)n;
        if (!WriteFile(g_pipe_w, p, chunk, &w, NULL) || w == 0) return;
        p += w;
        n -= w;
    }
}

/* ---------------------------------------------------------- key translation */
static int mod_from(const WinKeyEventRecord *k) {
    int mod = 1;
    if (k->dwControlKeyState & WIN_SHIFT_PRESSED) mod += 1;
    if (k->dwControlKeyState & (WIN_LEFT_ALT_PRESSED | WIN_RIGHT_ALT_PRESSED)) mod += 2;
    if (k->dwControlKeyState & (WIN_LEFT_CTRL_PRESSED | WIN_RIGHT_CTRL_PRESSED)) mod += 4;
    return mod;
}

static int seq_arrow(u16 vk, int mod, u8 *out) {
    char final = vk == 0x26 ? 'A' : (vk == 0x28 ? 'B' : (vk == 0x27 ? 'C' : 'D'));
    int n = 0;
    out[n++] = 0x1b;
    out[n++] = '[';
    if (mod != 1) {
        out[n++] = '1';
        out[n++] = ';';
        out[n++] = (u8)('0' + mod);
    }
    out[n++] = (u8)final;
    return n;
}

static int seq_home_end(u16 vk, int mod, u8 *out) {
    char final = vk == 0x24 ? 'H' : 'F';
    int n = 0;
    out[n++] = 0x1b;
    out[n++] = '[';
    if (mod != 1) {
        out[n++] = '1';
        out[n++] = ';';
        out[n++] = (u8)('0' + mod);
    }
    out[n++] = (u8)final;
    return n;
}

static int seq_tilde(int num, int mod, u8 *out) {
    int n = 0;
    out[n++] = 0x1b;
    out[n++] = '[';
    if (num >= 10) {
        out[n++] = (u8)('0' + num / 10);
        out[n++] = (u8)('0' + num % 10);
    } else {
        out[n++] = (u8)('0' + num);
    }
    if (mod != 1) {
        out[n++] = ';';
        out[n++] = (u8)('0' + mod);
    }
    out[n++] = '~';
    return n;
}

static int seq_ss3(char final, int mod, u8 *out) {
    int n = 0;
    out[n++] = 0x1b;
    if (mod == 1) {
        out[n++] = 'O';
        out[n++] = (u8)final;
        return n;
    }
    out[n++] = '[';
    out[n++] = '1';
    out[n++] = ';';
    out[n++] = (u8)('0' + mod);
    out[n++] = (u8)final;
    return n;
}

/* Returns the number of bytes appended; 0 means "nothing for this event". */
static int key_to_bytes(const WinKeyEventRecord *k, u16 *pending_high, u8 *out) {
    int n = 0;
    u32 cp = (u32)k->uChar.UnicodeChar;
    int mod = mod_from(k);
    bool alt = (mod & 2) != 0;
    bool ctrl = (k->dwControlKeyState &
                 (WIN_LEFT_CTRL_PRESSED | WIN_RIGHT_CTRL_PRESSED)) != 0;
    u16 vk = k->wVirtualKeyCode;

    if (vk == 0x09 && (mod & 1)) { /* shift+tab */
        out[0] = 0x1b;
        out[1] = '[';
        out[2] = 'Z';
        return 3;
    }
    switch (vk) {
    case 0x25: case 0x26: case 0x27: case 0x28: return seq_arrow(vk, mod, out);
    case 0x23: case 0x24: return seq_home_end(vk, mod, out);
    case 0x21: return seq_tilde(5, mod, out);  /* page up */
    case 0x22: return seq_tilde(6, mod, out);  /* page down */
    case 0x2D: return seq_tilde(2, mod, out);  /* insert */
    case 0x2E: return seq_tilde(3, mod, out);  /* delete */
    case 0x70: return seq_ss3('P', mod, out);
    case 0x71: return seq_ss3('Q', mod, out);
    case 0x72: return seq_ss3('R', mod, out);
    case 0x73: return seq_ss3('S', mod, out);
    case 0x74: return seq_tilde(15, mod, out);
    case 0x75: return seq_tilde(17, mod, out);
    case 0x76: return seq_tilde(18, mod, out);
    case 0x77: return seq_tilde(19, mod, out);
    case 0x78: return seq_tilde(20, mod, out);
    case 0x79: return seq_tilde(21, mod, out);
    case 0x7A: return seq_tilde(23, mod, out);
    case 0x7B: return seq_tilde(24, mod, out);
    default: break;
    }

    if (cp != 0) {
        if (cp == 0x0D) {
            out[n++] = '\r';
            return n;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            *pending_high = (u16)cp;
            return 0;
        }
        if (cp >= 0xDC00 && cp <= 0xDFFF) {
            if (*pending_high == 0) return 0;
            u32 full = 0x10000 + ((((u32)*pending_high) - 0xD800) << 10) + (cp - 0xDC00);
            *pending_high = 0;
            if (alt) out[n++] = 0x1b;
            return n + (int)agentc_utf8_encode(full, out + n);
        }
        *pending_high = 0;
        if (alt && cp != 0x1b) out[n++] = 0x1b;
        return n + (int)agentc_utf8_encode(cp, out + n);
    }

    /* Ctrl+Space and Ctrl+A..Z can arrive without a UnicodeChar. */
    if (ctrl) {
        if (vk == 0x20) {
            out[0] = 0;
            return 1;
        }
        if (vk >= 'A' && vk <= 'Z') {
            out[0] = (u8)(vk - 'A' + 1);
            return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------- reader thread */
static WinDWORD console_reader(void *unused) {
    (void)unused;
    WinInputRecord recs[32];
    WinDWORD got = 0;
    u16 pending_high = 0;
    for (;;) {
        if (!ReadConsoleInputW(g_con_in, recs, 32, &got)) break;
        for (WinDWORD i = 0; i < got; i++) {
            if (recs[i].EventType == WIN_WINDOW_BUFFER_SIZE_EVENT) {
                g_winch = true;
                continue;
            }
            if (recs[i].EventType != WIN_KEY_EVENT) continue;
            WinKeyEventRecord *k = &recs[i].KeyEvent;
            if (!k->bKeyDown) continue;
            u8 bytes[32];
            int n = key_to_bytes(k, &pending_high, bytes);
            if (n > 0) pipe_write_all(bytes, (size_t)n);
        }
    }
    return 0;
}

void win_console_init(void) {
    WinHandle in = GetStdHandle(WIN_STD_INPUT_HANDLE);
    WinDWORD mode = 0;
    if (in == NULL || in == WIN_INVALID_HANDLE || !GetConsoleMode(in, &mode)) return;
    g_con_in = in;

    WinSecurityAttributes sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = WIN_FALSE;
    WinHandle pr = NULL, pw = NULL;
    if (!CreatePipe(&pr, &pw, &sa, 0)) return;
    g_pipe_w = pw;

    WinFd *f = win_fd(0);
    if (f != NULL) {
        f->handle = pr;
        f->kind = WIN_FD_PIPE;
        f->flags = WIN_FD_STD | WIN_FD_READ;
        f->aux = g_con_in;
    }
    WinDWORD tid = 0;
    CreateThread(NULL, 0, console_reader, NULL, 0, &tid);
}

void win_console_poll_winch(void) {
    if (!g_winch) return;
    g_winch = false;
    if (g_winch_handler != NULL) g_winch_handler();
}

void win_console_set_handler(void (*handler)(void)) { g_winch_handler = handler; }

/* ------------------------------------------------------------------ terminal */
struct win_tty_saved {
    WinHandle in;
    WinHandle out;
    WinDWORD in_mode;
    WinDWORD out_mode;
    bool have_in;
    bool have_out;
};

static WinHandle tty_in_handle(int fd) {
    WinFd *f = win_fd(fd);
    if (f != NULL && f->aux != NULL) return (WinHandle)f->aux;
    return g_con_in;
}

/* fd is console-backed directly (WIN_FD_CONSOLE) or, once win_console_init has
 * replaced fd 0 with the reader pipe, through the aux slot holding the real
 * console input handle. A redirected file/pipe has neither, so os_tty_raw
 * succeeding via a console *stdout* is not mistaken for an input tty. */
int os_tty_isatty(int fd) {
    WinFd *f = win_fd(fd);
    return f != NULL && (f->kind == WIN_FD_CONSOLE || f->aux != NULL);
}

int os_tty_raw(int fd, void **saved) {
    WinHandle in = tty_in_handle(fd);
    WinHandle out = GetStdHandle(WIN_STD_OUTPUT_HANDLE);
    if (out == WIN_INVALID_HANDLE) out = NULL;
    WinDWORD im = 0, om = 0;
    bool have_in = in != NULL && in != WIN_INVALID_HANDLE && GetConsoleMode(in, &im);
    bool have_out = out != NULL && GetConsoleMode(out, &om);
    if (!have_in && !have_out) return -25; /* ENOTTY */

    struct win_tty_saved *st = agentc_alloc(sizeof *st);
    st->in = in;
    st->out = out;
    st->in_mode = im;
    st->out_mode = om;
    st->have_in = have_in;
    st->have_out = have_out;

    if (have_in) {
        WinDWORD m = im & ~(WIN_ENABLE_PROCESSED_INPUT | WIN_ENABLE_LINE_INPUT |
                            WIN_ENABLE_ECHO_INPUT);
        if (!SetConsoleMode(in, m | WIN_ENABLE_VIRTUAL_TERMINAL_INPUT))
            SetConsoleMode(in, m); /* pre-VT console */
    }
    if (have_out) {
        WinDWORD m = om | WIN_ENABLE_PROCESSED_OUTPUT |
                     WIN_ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                     WIN_DISABLE_NEWLINE_AUTO_RETURN;
        if (!SetConsoleMode(out, m))
            SetConsoleMode(out, om | WIN_ENABLE_PROCESSED_OUTPUT);
    }
    *saved = st;
    return 0;
}

int os_tty_restore(int fd, void *saved) {
    (void)fd;
    if (saved == NULL) return -22;
    struct win_tty_saved *st = saved;
    if (st->have_in) SetConsoleMode(st->in, st->in_mode);
    if (st->have_out) SetConsoleMode(st->out, st->out_mode);
    agentc_free(st);
    return 0;
}

int os_tty_size(int fd, int *cols, int *rows) {
    WinConsoleScreenBufferInfo info;
    WinHandle h = tty_in_handle(fd);
    if (h == NULL || h == WIN_INVALID_HANDLE || !GetConsoleScreenBufferInfo(h, &info)) {
        h = GetStdHandle(WIN_STD_OUTPUT_HANDLE);
        if (h == NULL || h == WIN_INVALID_HANDLE || !GetConsoleScreenBufferInfo(h, &info))
            return -25;
    }
    *cols = (int)(info.srWindow.Right - info.srWindow.Left + 1);
    *rows = (int)(info.srWindow.Bottom - info.srWindow.Top + 1);
    return 0;
}

int os_sig_winch(void (*handler)(void)) {
    win_console_set_handler(handler);
    return 0;
}
