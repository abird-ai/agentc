/* plat/win/sys.c — Windows Layer 0 over kernel32 (no CRT, no SDK).
 *
 * Files, memory, processes, time, entropy and the WSAPoll/WaitFor* event loop.
 * Everything goes through the fd table declared in win.h:
 *
 *   - files: CreateFileW + ReadFile/WriteFile, SetFilePointerEx;
 *   - pipes: PeekNamedPipe makes reads non-blocking (-EAGAIN on empty) and
 *     ReadFile returns 0 on a broken pipe, mirroring POSIX;
 *   - directories: FindFirstFileW/FindNextFileW are converted to Linux
 *     dirent64 records so core code needs no Windows knowledge;
 *   - processes: CreateProcessW with temporary inheritable stdio handles; the
 *     child table remembers the process handle for os_wait/os_kill;
 *   - os_poll: sockets go to WSAPoll, pipes/console handles to
 *     WaitForSingleObject/WaitForMultipleObjects, and the two are combined.
 *
 * The portable struct os_stat the core reads (plat.h) is synthesised in
 * os_stat/os_fstat from BY_HANDLE_FILE_INFORMATION / WIN32_FILE_ATTRIBUTE_DATA.
 */
#include "agentc.h"
#include "plat.h"
#include "win.h"

/* ------------------------------------------------------------ file reading */
static int file_read(WinHandle h, void *buf, size_t n) {
    WinDWORD got = 0;
    if (!ReadFile(h, buf, (WinDWORD)n, &got, NULL)) {
        WinDWORD e = GetLastError();
        if (e == WIN_ERROR_BROKEN_PIPE || e == WIN_ERROR_PIPE_NOT_CONNECTED ||
            e == WIN_ERROR_HANDLE_EOF)
            return 0;
        return win_errno(e);
    }
    return (int)got;
}

static int pipe_read(WinHandle h, void *buf, size_t n) {
    WinDWORD avail = 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
        WinDWORD e = GetLastError();
        if (e == WIN_ERROR_BROKEN_PIPE || e == WIN_ERROR_PIPE_NOT_CONNECTED) return 0;
        return win_errno(e);
    }
    if (avail == 0) return -11; /* EAGAIN */
    if ((size_t)avail < n) n = avail;
    WinDWORD got = 0;
    if (!ReadFile(h, buf, (WinDWORD)n, &got, NULL)) {
        WinDWORD e = GetLastError();
        if (e == WIN_ERROR_BROKEN_PIPE || e == WIN_ERROR_PIPE_NOT_CONNECTED) return 0;
        return win_errno(e);
    }
    return (int)got;
}

int os_read(int fd, void *buf, size_t n) {
    WinFd *f = win_fd(fd);
    if (f == NULL) return -9; /* EBADF */
    if (!(f->flags & WIN_FD_READ)) return -9;
    if (n == 0) return 0;
    switch (f->kind) {
    case WIN_FD_FILE:
    case WIN_FD_CONSOLE: return file_read(f->handle, buf, n);
    case WIN_FD_PIPE: return pipe_read(f->handle, buf, n);
    case WIN_FD_SOCKET: return -88; /* ENOTSOCK: use agentc_net_recv */
    default: return -21;            /* EISDIR */
    }
}

int os_write(int fd, const void *buf, size_t n) {
    WinFd *f = win_fd(fd);
    if (f == NULL) return -9;
    if (!(f->flags & WIN_FD_WRITE)) return -9;
    if (f->kind == WIN_FD_DIR) return -21; /* EISDIR */
    if (f->kind == WIN_FD_SOCKET) return -88;
    if (n == 0) return 0;
    WinHandle h = f->write_handle != NULL ? f->write_handle : f->handle;
    WinDWORD chunk = n > 0x40000000u ? 0x40000000u : (WinDWORD)n;
    WinDWORD wrote = 0;
    if (!WriteFile(h, buf, chunk, &wrote, NULL)) {
        WinDWORD e = GetLastError();
        if (e == WIN_ERROR_BROKEN_PIPE || e == WIN_ERROR_NO_DATA ||
            e == WIN_ERROR_PIPE_NOT_CONNECTED)
            return -32; /* EPIPE */
        return win_errno(e);
    }
    return (int)wrote;
}

int os_close(int fd) {
    if (win_fd(fd) == NULL) return -9;
    win_fd_free(fd);
    return 0;
}

/* ------------------------------------------------------------------- files */
int os_open(const char *path, int flags, int mode) {
    (void)mode; /* Windows files get the directory's default ACLs */
    bool rd = (flags & 3) != OS_O_WRONLY;
    bool wr = (flags & 3) != OS_O_RDONLY;
    WinDWORD access;
    if (flags & OS_O_DIRECTORY) {
        access = WIN_FILE_LIST_DIRECTORY;
    } else {
        access = 0;
        if (rd) access |= WIN_GENERIC_READ;
        if (wr) access |= (flags & OS_O_APPEND) ? WIN_FILE_APPEND_DATA : WIN_GENERIC_WRITE;
    }
    WinDWORD disp;
    if ((flags & OS_O_CREAT) && (flags & OS_O_TRUNC))
        disp = WIN_CREATE_ALWAYS;
    else if (flags & OS_O_CREAT)
        disp = WIN_OPEN_ALWAYS;
    else if (flags & OS_O_TRUNC)
        disp = WIN_TRUNCATE_EXISTING;
    else
        disp = WIN_OPEN_EXISTING;
    WinDWORD attrs = WIN_FILE_ATTRIBUTE_NORMAL;
    if (flags & OS_O_DIRECTORY) attrs |= WIN_FILE_FLAG_BACKUP_SEMANTICS;

    u16 *wpath = win_path_wide(path);
    if (wpath == NULL) return -22;
    WinHandle h = CreateFileW(wpath, access,
                              WIN_FILE_SHARE_READ | WIN_FILE_SHARE_WRITE |
                                  WIN_FILE_SHARE_DELETE,
                              NULL, disp, attrs, NULL);
    agentc_free(wpath);
    if (h == WIN_INVALID_HANDLE_VALUE) return win_errno(GetLastError());

    u16 f = 0;
    if (rd) f |= WIN_FD_READ;
    if (wr || (flags & OS_O_APPEND)) f |= WIN_FD_WRITE;
    if (flags & OS_O_APPEND) f |= WIN_FD_APPEND;
    int kind = (flags & OS_O_DIRECTORY) ? WIN_FD_DIR : WIN_FD_FILE;
    int fd = win_fd_alloc(h, NULL, kind, f);
    if (fd < 0) {
        CloseHandle(h);
        return fd;
    }
    if (kind == WIN_FD_DIR) {
        WinFd *e = win_fd(fd);
        e->path = agentc_strdup(path);
        if (e->path == NULL) {
            win_fd_free(fd);
            return -12;
        }
    }
    return fd;
}

i64 os_lseek(int fd, i64 off, int whence) {
    WinFd *f = win_fd(fd);
    if (f == NULL) return -9;
    if (f->kind != WIN_FD_FILE) return -29; /* ESPIPE */
    WinLargeInteger dist;
    dist.QuadPart = off;
    WinLargeInteger out;
    out.QuadPart = 0;
    WinDWORD m = whence == 1 ? WIN_FILE_CURRENT
                             : (whence == 2 ? WIN_FILE_END : WIN_FILE_BEGIN);
    if (!SetFilePointerEx(f->handle, dist, &out, m)) return win_errno(GetLastError());
    return out.QuadPart;
}

int os_ftruncate(int fd, i64 len) {
    WinFd *f = win_fd(fd);
    if (f == NULL) return -9;
    if (f->kind != WIN_FD_FILE) return -29; /* ESPIPE */
    WinLargeInteger dist;
    dist.QuadPart = len;
    if (!SetFilePointerEx(f->handle, dist, NULL, WIN_FILE_BEGIN)) {
        return win_errno(GetLastError());
    }
    if (!SetEndOfFile(f->handle)) return win_errno(GetLastError());
    return 0;
}

static i64 filetime_ns(const WinFILETIME *ft) {
    u64 t = ((u64)ft->dwHighDateTime << 32) | (u64)ft->dwLowDateTime;
    return (i64)(t * 100u - 116444736000000000ull * 100u);
}

static void fill_stat(void *dst, u32 mode, u64 ino, u64 dev, u64 nlink, u64 size,
                      const WinFILETIME *atime, const WinFILETIME *mtime,
                      const WinFILETIME *ctime) {
    struct os_stat *o = dst;
    agentc_memset(o, 0, sizeof *o);
    o->st_dev = dev;
    o->st_ino = ino;
    o->st_nlink = nlink ? nlink : 1;
    o->st_mode = mode;
    o->st_size = (i64)size;
    o->st_blksize = 4096;
    o->st_blocks = (i64)((size + 511) / 512);
    if (atime != NULL) {
        i64 ns = filetime_ns(atime);
        o->st_atime = ns / 1000000000;
        o->st_atime_nsec = ns % 1000000000;
    }
    if (mtime != NULL) {
        i64 ns = filetime_ns(mtime);
        o->st_mtime = ns / 1000000000;
        o->st_mtime_nsec = ns % 1000000000;
    }
    if (ctime != NULL) {
        i64 ns = filetime_ns(ctime);
        o->st_ctime = ns / 1000000000;
        o->st_ctime_nsec = ns % 1000000000;
    }
}

int os_fstat(int fd, void *statbuf) {
    WinFd *f = win_fd(fd);
    if (f == NULL) return -9;
    if (f->kind == WIN_FD_FILE || f->kind == WIN_FD_DIR) {
        WinByHandleFileInformation info;
        if (GetFileInformationByHandle(f->handle, &info)) {
            u32 mode = (info.dwFileAttributes & WIN_FILE_ATTRIBUTE_DIRECTORY)
                           ? 0040755u
                           : 0100644u;
            u64 ino = ((u64)info.nFileIndexHigh << 32) | info.nFileIndexLow;
            u64 size = ((u64)info.nFileSizeHigh << 32) | info.nFileSizeLow;
            fill_stat(statbuf, mode, ino, info.dwVolumeSerialNumber,
                      info.nNumberOfLinks, size, &info.ftLastAccessTime,
                      &info.ftLastWriteTime, &info.ftCreationTime);
            return 0;
        }
        if (f->kind == WIN_FD_DIR) {
            fill_stat(statbuf, 0040755u, 0, 0, 1, 0, NULL, NULL, NULL);
            return 0;
        }
        return win_errno(GetLastError());
    }
    u32 mode;
    switch (f->kind) {
    case WIN_FD_PIPE: mode = 0010600u; break;    /* S_IFIFO */
    case WIN_FD_CONSOLE: mode = 0020600u; break; /* S_IFCHR */
    case WIN_FD_SOCKET: mode = 0140600u; break;  /* S_IFSOCK */
    default: mode = 0100600u; break;
    }
    fill_stat(statbuf, mode, 0, 0, 1, 0, NULL, NULL, NULL);
    return 0;
}

int os_stat(const char *path, void *statbuf) {
    u16 *wpath = win_path_wide(path);
    if (wpath == NULL) return -22;
    WinFileAttributeData data;
    WinBOOL ok = GetFileAttributesExW(wpath, 0 /*GetFileExInfoStandard*/, &data);
    agentc_free(wpath);
    if (!ok) return win_errno(GetLastError());
    u32 mode = (data.dwFileAttributes & WIN_FILE_ATTRIBUTE_DIRECTORY) ? 0040755u
                                                                      : 0100644u;
    u64 size = ((u64)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    fill_stat(statbuf, mode, 0, 0, 1, size, &data.ftLastAccessTime,
              &data.ftLastWriteTime, &data.ftCreationTime);
    return 0;
}

int os_mkdir(const char *path, int mode) {
    (void)mode;
    u16 *wpath = win_path_wide(path);
    if (wpath == NULL) return -22;
    WinBOOL ok = CreateDirectoryW(wpath, NULL);
    WinDWORD e = ok ? 0 : GetLastError();
    agentc_free(wpath);
    if (ok) return 0;
    if (e == WIN_ERROR_ALREADY_EXISTS) return -17; /* EEXIST */
    return win_errno(e);
}

int os_unlink(const char *path) {
    u16 *wpath = win_path_wide(path);
    if (wpath == NULL) return -22;
    WinBOOL ok = DeleteFileW(wpath);
    WinDWORD e = ok ? 0 : GetLastError();
    if (!ok)
        ok = RemoveDirectoryW(wpath); /* DeleteFileW refuses directories */
    if (!ok) e = GetLastError();
    agentc_free(wpath);
    return ok ? 0 : win_errno(e);
}

int os_rename(const char *from, const char *to) {
    u16 *wfrom = win_path_wide(from);
    u16 *wto = win_path_wide(to);
    if (wfrom == NULL || wto == NULL) {
        if (wfrom != NULL) agentc_free(wfrom);
        if (wto != NULL) agentc_free(wto);
        return -22;
    }
    WinBOOL ok = MoveFileExW(wfrom, wto, WIN_MOVEFILE_REPLACE_EXISTING);
    WinDWORD e = ok ? 0 : GetLastError();
    agentc_free(wfrom);
    agentc_free(wto);
    return ok ? 0 : win_errno(e);
}

int os_getcwd(char *buf, size_t len) {
    WinWCHAR wbuf[4096];
    WinDWORD n = GetCurrentDirectoryW(4096, wbuf);
    if (n == 0) return win_errno(GetLastError());
    if (n >= 4096) return -36;
    int u = WideCharToMultiByte(65001, 0, wbuf, (int)n + 1, buf, (int)len, NULL, NULL);
    if (u <= 0) return -36;
    return (int)agentc_strlen(buf) + 1;
}

/* ------------------------------------------------------------- PATH lookup */
/* Windows PATH semantics: ';' separates entries, empty entries mean the cwd,
 * and a name without an extension is retried with each PATHEXT suffix. A hit
 * is any existing non-directory (Windows marks executability by extension).
 * The result is absolute-ized with the cwd when needed. */
static bool win_which_abs(const char *path) {
    if (path[0] == '\\' || path[0] == '/') return true;
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':')
        return true;
    return false;
}

static int win_which_abs_copy(const char *path, char *out, size_t cap) {
    if (win_which_abs(path)) {
        size_t n = agentc_strlen(path);
        if (n + 1 > cap) return -34; /* ERANGE */
        agentc_memcpy(out, path, n + 1);
        return 0;
    }
    char cwd[4096];
    int r = os_getcwd(cwd, sizeof cwd);
    if (r < 0) return r;
    size_t cl = agentc_strlen(cwd);
    size_t pl = agentc_strlen(path);
    bool sep = cl > 0 && cwd[cl - 1] != '\\' && cwd[cl - 1] != '/';
    if (cl + (sep ? 1 : 0) + pl + 1 > cap) return -34;
    agentc_memcpy(out, cwd, cl);
    size_t pos = cl;
    if (sep) out[pos++] = '\\';
    agentc_memcpy(out + pos, path, pl + 1);
    return 0;
}

static int win_which_try(const char *cand, char *out, size_t cap) {
    struct os_stat st;
    if (os_stat(cand, &st) != 0) return -2;              /* ENOENT */
    if ((st.st_mode & 0170000u) == 0040000u) return -2; /* directory */
    return win_which_abs_copy(cand, out, cap);
}

static bool win_which_has_ext(const char *name) {
    const char *base = name;
    for (const char *p = name; *p != 0; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    for (const char *p = base; *p != 0; p++)
        if (*p == '.') return true;
    return false;
}

/* prefix is `dir\` (possibly empty); append name[+suffix] and test. */
static int win_which_cand(const char *prefix, size_t plen, const char *name,
                          const char *suffix, size_t slen, char *out, size_t cap) {
    size_t nl = agentc_strlen(name);
    char *cand = agentc_alloc(plen + nl + slen + 1);
    if (plen) agentc_memcpy(cand, prefix, plen);
    agentc_memcpy(cand + plen, name, nl);
    if (slen) agentc_memcpy(cand + plen + nl, suffix, slen);
    cand[plen + nl + slen] = 0;
    int r = win_which_try(cand, out, cap);
    agentc_free(cand);
    return r;
}

int os_which_in(const char *path_env, const char *name, char *out, size_t cap) {
    if (name == NULL || name[0] == 0) return -2; /* ENOENT */
    /* A name with a separator is checked as given, relative to the cwd. */
    for (const char *p = name; *p != 0; p++)
        if (*p == '/' || *p == '\\') return win_which_try(name, out, cap);
    if (path_env == NULL || path_env[0] == 0) return -2; /* no PATH search */

    bool has_ext = win_which_has_ext(name);
    const char *pathext = os_getenv("PATHEXT");
    if (pathext == NULL || pathext[0] == 0) pathext = ".COM;.EXE;.BAT;.CMD";

    for (const char *p = path_env;;) {
        const char *end = p;
        while (*end != 0 && *end != ';') end++;
        const char *dir = p;
        size_t dl = (size_t)(end - p);
        /* Some installers quote PATH entries; strip matching outer quotes. */
        if (dl >= 2 && dir[0] == '"' && dir[dl - 1] == '"') {
            dir++;
            dl -= 2;
        }
        char *prefix = agentc_alloc(dl + 2);
        size_t plen = 0;
        if (dl) {
            agentc_memcpy(prefix, dir, dl);
            plen = dl;
            if (prefix[plen - 1] != '\\' && prefix[plen - 1] != '/') prefix[plen++] = '\\';
        }
        int r = win_which_cand(prefix, plen, name, NULL, 0, out, cap);
        if (r != 0 && r != -34 && !has_ext) {
            const char *ext = pathext;
            while (*ext != 0) {
                const char *ee = ext;
                while (*ee != 0 && *ee != ';') ee++;
                size_t el = (size_t)(ee - ext);
                if (el) {
                    r = win_which_cand(prefix, plen, name, ext, el, out, cap);
                    if (r == 0 || r == -34) break;
                }
                if (*ee == 0) break;
                ext = ee + 1;
            }
        }
        agentc_free(prefix);
        if (r == 0 || r == -34) return r; /* hit, or the hit does not fit */
        if (*end == 0) break;
        p = end + 1;
    }
    return -2;
}

int os_which(const char *name, char *out, size_t cap) {
    return os_which_in(os_getenv("PATH"), name, out, cap);
}

/* ------------------------------------------------------------- directories */
int os_getdents(int fd, void *buf, size_t len) {
    WinFd *f = win_fd(fd);
    if (f == NULL) return -9;
    if (f->kind != WIN_FD_DIR) return -20; /* ENOTDIR */
    if (f->path == NULL) return -20;
    WinFindCtx *ctx = f->aux;
    if (ctx == NULL) {
        ctx = agentc_alloc(sizeof *ctx);
        agentc_memset(ctx, 0, sizeof *ctx);
        ctx->find = WIN_INVALID_HANDLE_VALUE;
        f->aux = ctx;
    }
    if (ctx->done) return 0;
    size_t used = 0;
    for (;;) {
        Win32FindDataW *d;
        if (ctx->pending) {
            d = &ctx->data;
        } else if (ctx->find == WIN_INVALID_HANDLE_VALUE) {
            size_t plen = agentc_strlen(f->path);
            char *pat = agentc_alloc(plen + 4);
            agentc_memcpy(pat, f->path, plen);
            if (plen == 0 || pat[plen - 1] != '\\') pat[plen++] = '\\';
            pat[plen++] = '*';
            pat[plen] = '\0';
            u16 *wp = win_path_wide(pat);
            agentc_free(pat);
            if (wp == NULL) return -22;
            ctx->find = FindFirstFileW(wp, &ctx->data);
            agentc_free(wp);
            if (ctx->find == WIN_INVALID_HANDLE_VALUE) {
                WinDWORD e = GetLastError();
                if (e == WIN_ERROR_FILE_NOT_FOUND) return 0;
                return win_errno(e);
            }
            ctx->pending = true;
            d = &ctx->data;
        } else {
            if (!FindNextFileW(ctx->find, &ctx->data)) {
                WinDWORD e = GetLastError();
                FindClose(ctx->find);
                ctx->find = WIN_INVALID_HANDLE_VALUE;
                ctx->done = true;
                if (e == WIN_ERROR_NO_MORE_FILES) return (int)used;
                return win_errno(e);
            }
            d = &ctx->data;
        }

        char name[1200];
        int u = WideCharToMultiByte(65001, 0, d->cFileName, -1, name, (int)sizeof name,
                                    NULL, NULL);
        if (u <= 0) {
            ctx->pending = false;
            continue; /* unrepresentable name: skip */
        }
        size_t nl = (size_t)u - 1;
        size_t reclen = (19 + nl + 1 + 7) & ~(size_t)7;
        if (used + reclen > len) {
            /* A staged FindNextFileW entry must not be lost on a retry: mark it
             * pending before either returning -22 or breaking. */
            ctx->pending = true;
            if (used == 0) return -22; /* buffer too small; entry stays pending */
            break;
        }
        u8 *rec = (u8 *)buf + used;
        u64 ino = 0;
        i64 off = 0;
        u16 rl = (u16)reclen;
        /* A reparse point (symlink/junction) is a link even when its target is
         * a directory: report DT_LNK so the core's tree walker lists it and
         * does not recurse, exactly as Linux's dirent64 does. */
        u8 type;
        if (d->dwFileAttributes & WIN_FILE_ATTRIBUTE_REPARSE_POINT)
            type = 10; /* DT_LNK */
        else
            type = (d->dwFileAttributes & WIN_FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;
        agentc_memcpy(rec + 0, &ino, 8);
        agentc_memcpy(rec + 8, &off, 8);
        agentc_memcpy(rec + 16, &rl, 2);
        rec[18] = type;
        agentc_memcpy(rec + 19, name, nl);
        rec[19 + nl] = 0;
        used += reclen;
        ctx->pending = false;
    }
    return (int)used;
}

/* ------------------------------------------------------------------ memory */
u8 *os_map_try(size_t n) {
    void *p = VirtualAlloc(NULL, n, WIN_MEM_COMMIT | WIN_MEM_RESERVE,
                           WIN_PAGE_READWRITE);
    return (u8 *)p;
}

u8 *os_map(size_t n) {
    u8 *p = os_map_try(n);
    if (p == NULL) agentc_die("out of memory");
    return p;
}

int os_unmap(u8 *p, size_t n) {
    (void)n;
    return VirtualFree(p, 0, WIN_MEM_RELEASE) ? 0 : -5;
}

/* ---------------------------------------------------------------- entropy */
int os_random(void *buf, size_t n) {
    u8 *p = buf;
    while (n > 0) {
        u32 chunk = n > 0x40000000u ? 0x40000000u : (u32)n;
        int st = BCryptGenRandom(NULL, p, chunk, WIN_BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (st < 0) return -5;
        p += chunk;
        n -= chunk;
    }
    return 0;
}

/* ------------------------------------------------------------------- time */
i64 os_now_ns(int clock) {
    if (clock == OS_CLOCK_MONOTONIC) return (i64)GetTickCount64() * 1000000;
    WinFILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return filetime_ns(&ft);
}

void os_sleep_ns(i64 ns) {
    if (ns <= 0) {
        Sleep(0);
        return;
    }
    u64 ms = ((u64)ns + 999999u) / 1000000u;
    while (ms > 0xFFFFFFFFu) {
        Sleep(0xFFFFFFFFu);
        ms -= 0xFFFFFFFFu;
    }
    Sleep((WinDWORD)ms);
}

/* --------------------------------------------------------------- processes */
#define WIN_PROC_MAX 256
static struct {
    WinDWORD pid;
    WinHandle h;
    WinHandle job;   /* group spawns only; NULL = plain child, leader kill */
    int killed;
} g_procs[WIN_PROC_MAX];

static int proc_slot(WinDWORD pid) {
    for (int i = 0; i < WIN_PROC_MAX; i++)
        if (g_procs[i].h != NULL && g_procs[i].pid == pid) return i;
    return -1;
}

static void wpush(AgcBuf *b, const u16 *s, size_t n) { agentc_buf_push(b, s, n * 2); }
static void wpush_ch(AgcBuf *b, u16 c) { agentc_buf_push(b, &c, 2); }

static void cmdline_arg(AgcBuf *b, const u16 *arg) {
    bool quote = (*arg == 0);
    for (const u16 *p = arg; *p != 0; p++) {
        if (*p == ' ' || *p == '\t' || *p == '"') {
            quote = true;
            break;
        }
    }
    if (b->len > 0) wpush_ch(b, ' ');
    if (!quote) {
        wpush(b, arg, wide_len(arg));
        return;
    }
    wpush_ch(b, '"');
    const u16 *p = arg;
    while (*p != 0) {
        unsigned bs = 0;
        while (*p == '\\') {
            bs++;
            p++;
        }
        if (*p == '"') {
            for (unsigned i = 0; i < bs * 2 + 1; i++) wpush_ch(b, '\\');
            wpush_ch(b, '"');
            p++;
        } else {
            for (unsigned i = 0; i < bs; i++) wpush_ch(b, '\\');
            if (*p != 0) {
                wpush_ch(b, *p);
                p++;
            }
        }
    }
    wpush_ch(b, '"');
}

static int wide_upper(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

static int wide_icmp(const u16 *a, const u16 *b) {
    while (*a && *b) {
        int x = wide_upper((int)*a);
        int y = wide_upper((int)*b);
        if (x != y) return x - y;
        a++;
        b++;
    }
    return (int)*a - (int)*b;
}

/* Windows requires a case-insensitively sorted double-NUL environment block. */
static u16 *build_env_block(char *const envp[]) {
    size_t n = 0;
    if (envp != NULL)
        while (envp[n] != NULL) n++;
    u16 **items = agentc_alloc((n > 0 ? n : 1) * sizeof *items);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        u16 *w = win_utf8_to_wide(envp[i]);
        if (w != NULL) items[k++] = w;
    }
    n = k;
    for (size_t i = 1; i < n; i++) {
        u16 *key = items[i];
        size_t j = i;
        while (j > 0 && wide_icmp(items[j - 1], key) > 0) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = key;
    }
    size_t total = 1;
    for (size_t i = 0; i < n; i++) total += wide_len(items[i]) + 1;
    u16 *block = agentc_alloc(total * 2);
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        size_t l = wide_len(items[i]);
        agentc_memcpy(block + pos, items[i], l * 2);
        pos += l;
        block[pos++] = 0;
        agentc_free(items[i]);
    }
    block[pos] = 0;
    agentc_free(items);
    return block;
}

/* Shared tail of every spawn: temporary inheritable stdio handles,
 * CreateProcessW and the process table. `cmdline` is a complete wide command
 * line including its terminating NUL; envp == NULL inherits the parent
 * environment (the shell tool wants the user's real environment).  `group`
 * puts the child in a new process group plus a KILL_ON_JOB_CLOSE job object so
 * os_kill(-pid) can take the whole tree down. */
static int win_spawn_cmdline(const u16 *cmdline, char *const envp[], const char *cwd,
                             int fd_in, int fd_out, int fd_err, bool group) {
    u16 *envblk = envp != NULL ? build_env_block(envp) : NULL;
    u16 *cwdw = cwd != NULL ? win_path_wide(cwd) : NULL;

    const int fds[3] = { fd_in, fd_out, fd_err };
    const WinDWORD std_ids[3] = { WIN_STD_INPUT_HANDLE, WIN_STD_OUTPUT_HANDLE,
                                  WIN_STD_ERROR_HANDLE };
    WinHandle sh[3] = { NULL, NULL, NULL };
    WinDWORD oldfl[3] = { 0, 0, 0 };
    bool have_old[3] = { false, false, false };
    int rc = 0;
    for (int i = 0; i < 3; i++) {
        if (fds[i] >= 0) {
            WinFd *f = win_fd(fds[i]);
            if (f == NULL) {
                rc = -9;
                break;
            }
            sh[i] = f->handle;
        } else {
            sh[i] = GetStdHandle(std_ids[i]);
            if (sh[i] == WIN_INVALID_HANDLE) sh[i] = NULL;
        }
        if (sh[i] != NULL) {
            have_old[i] = GetHandleInformation(sh[i], &oldfl[i]) != 0;
            SetHandleInformation(sh[i], WIN_HANDLE_FLAG_INHERIT,
                                 WIN_HANDLE_FLAG_INHERIT);
        }
    }

    WinBOOL ok = WIN_FALSE;
    WinDWORD err = 0;
    WinProcessInformation procinfo;
    agentc_memset(&procinfo, 0, sizeof procinfo);
    if (rc == 0) {
        WinStartupInfoW si;
        agentc_memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = WIN_STARTF_USESTDHANDLES;
        si.hStdInput = sh[0];
        si.hStdOutput = sh[1];
        si.hStdError = sh[2];
        WinDWORD flags = WIN_CREATE_UNICODE_ENVIRONMENT;
        if (group) flags |= WIN_CREATE_NEW_PROCESS_GROUP;
        ok = CreateProcessW(NULL, (WinLPWSTR)cmdline, NULL, NULL, WIN_TRUE,
                            flags, envblk, cwdw, &si, &procinfo);
        if (!ok) err = GetLastError();
    }
    for (int i = 0; i < 3; i++) {
        if (sh[i] != NULL && have_old[i])
            SetHandleInformation(sh[i], WIN_HANDLE_FLAG_INHERIT,
                                 oldfl[i] & WIN_HANDLE_FLAG_INHERIT);
    }
    if (envblk != NULL) agentc_free(envblk);
    if (cwdw != NULL) agentc_free(cwdw);
    if (rc != 0) return rc;
    if (!ok) return win_errno(err);
    if (procinfo.hThread != NULL) CloseHandle(procinfo.hThread);

    /* Group spawn: create the kill-on-close job before publishing the slot.
     * A failed job setup (e.g. an old Windows without nested jobs) degrades
     * to the leader-only fallback instead of failing the spawn. */
    WinHandle job = NULL;
    if (group) {
        job = CreateJobObjectW(NULL, NULL);
        if (job != NULL) {
            WinJobObjectExtendedLimitInformation info;
            agentc_memset(&info, 0, sizeof info);
            info.BasicLimitInformation.LimitFlags = WIN_JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job, WIN_JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                                         &info, (WinDWORD)sizeof info) ||
                !AssignProcessToJobObject(job, procinfo.hProcess)) {
                CloseHandle(job);
                job = NULL;
            }
        }
    }

    int slot = -1;
    for (int i = 0; i < WIN_PROC_MAX; i++) {
        if (g_procs[i].h == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (job != NULL) CloseHandle(job);
        TerminateProcess(procinfo.hProcess, 1);
        CloseHandle(procinfo.hProcess);
        return -24;
    }
    g_procs[slot].pid = procinfo.dwProcessId;
    g_procs[slot].h = procinfo.hProcess;
    g_procs[slot].job = job;
    g_procs[slot].killed = 0;
    return (int)procinfo.dwProcessId;
}

static int win_spawn_argv(char *const argv[], char *const envp[], const char *cwd,
                          int fd_in, int fd_out, int fd_err, bool group) {
    if (argv == NULL || argv[0] == NULL) return -22;

    AgcBuf cmd;
    agentc_memset(&cmd, 0, sizeof cmd);
    for (int i = 0; argv[i] != NULL; i++) {
        u16 *w = win_utf8_to_wide(argv[i]);
        if (w == NULL) {
            agentc_buf_free(&cmd);
            return -12;
        }
        cmdline_arg(&cmd, w);
        agentc_free(w);
    }
    u16 z2[2] = { 0, 0 };
    agentc_buf_push(&cmd, z2, 2); /* second NUL for the wide string terminator */
    int pid = win_spawn_cmdline((const u16 *)cmd.p, envp, cwd, fd_in, fd_out, fd_err,
                                group);
    agentc_buf_free(&cmd);
    return pid;
}

int os_spawn(char *const argv[], char *const envp[], const char *cwd, int fd_in,
             int fd_out, int fd_err, int ctty) {
    (void)ctty;
    return win_spawn_argv(argv, envp, cwd, fd_in, fd_out, fd_err, false);
}

int os_spawn_group(char *const argv[], char *const envp[], const char *cwd, int fd_in,
                   int fd_out, int fd_err, int ctty) {
    (void)ctty;
    return win_spawn_argv(argv, envp, cwd, fd_in, fd_out, fd_err, true);
}

/* ------------------------------------------------------------- shell tool */
/* The bash tool's Windows side. Resolution follows the contract in plat.h:
 * cmd.exe comes from %COMSPEC% then %SystemRoot%\System32\cmd.exe; PowerShell
 * prefers PowerShell 7 (pwsh.exe on PATH) and falls back to the built-in 5.1
 * at %SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe. Windows
 * always has cmd.exe, so "auto" resolves to it; a missing explicitly chosen
 * shell is an error and never falls back to the other one. */
#define WIN_SHELL_CAND 4096

static bool win_shell_exists(const char *path) {
    struct os_stat st;
    return os_stat(path, &st) == 0;
}

/* dir + '\' + name; false when the result does not fit. */
static bool win_shell_join(char *out, size_t cap, const char *dir, const char *name) {
    size_t dl = agentc_strlen(dir);
    bool sep = dl > 0 && dir[dl - 1] != '\\' && dir[dl - 1] != '/';
    int n = agentc_snprintf(out, cap, "%s%s%s", dir, sep ? "\\" : "", name);
    return n > 0 && (size_t)n < cap;
}

/* returns 0, -36 when `src` does not fit, -2 when the file does not exist */
static int win_shell_use(char *out, size_t cap, const char *src) {
    if (!win_shell_exists(src)) return -2;
    size_t n = agentc_strlen(src);
    if (n + 1 > cap) return -36;
    agentc_memcpy(out, src, n + 1);
    return 0;
}

/* pwsh.exe searched on PATH in order; a malformed or oversized entry is
 * skipped rather than failing the whole lookup. */
static bool win_shell_find_pwsh(char *out, size_t cap) {
    const char *path = os_getenv("PATH");
    if (path == NULL) return false;
    const char *p = path;
    while (*p != 0) {
        const char *end = p;
        while (*end != 0 && *end != ';') end++;
        size_t len = (size_t)(end - p);
        /* Some installers quote PATH entries; strip matching outer quotes. */
        if (len >= 2 && p[0] == '"' && p[len - 1] == '"') {
            p++;
            len -= 2;
        }
        if (len > 0 && len < WIN_SHELL_CAND) {
            char dir[WIN_SHELL_CAND];
            agentc_memcpy(dir, p, len);
            dir[len] = 0;
            char cand[WIN_SHELL_CAND];
            if (win_shell_join(cand, sizeof cand, dir, "pwsh.exe") &&
                win_shell_use(out, cap, cand) == 0)
                return true;
        }
        if (*end == 0) break;
        p = end + 1;
    }
    return false;
}

/* %SystemRoot%\... with %windir% as the near-universal fallback when a
 * stripped environment has no SystemRoot. */
static int win_shell_system_exe(char *out, size_t cap, const char *rest) {
    const char *root = os_getenv("SystemRoot");
    if (root == NULL || root[0] == 0) root = os_getenv("windir");
    if (root == NULL || root[0] == 0) return -2;
    char cand[WIN_SHELL_CAND];
    if (!win_shell_join(cand, sizeof cand, root, rest)) return -36;
    return win_shell_use(out, cap, cand);
}

const char *os_shell_kind(const char *kind) {
    if (kind == NULL || kind[0] == 0 || agentc_streq(kind, "auto")) return "cmd";
    if (agentc_streq(kind, "cmd")) return "cmd";
    if (agentc_streq(kind, "powershell")) return "powershell";
    if (agentc_streq(kind, "pwsh")) return "pwsh";
    return NULL;
}

int os_shell_resolve(const char *kind, char *out, size_t cap) {
    const char *k = os_shell_kind(kind);
    if (k == NULL) return -22; /* EINVAL */
    if (agentc_streq(k, "cmd")) {
        const char *comspec = os_getenv("COMSPEC");
        if (comspec != NULL && comspec[0] != 0) {
            int r = win_shell_use(out, cap, comspec);
            if (r == 0) return 0;
            if (r == -36) return -36;
        }
        return win_shell_system_exe(out, cap, "System32\\cmd.exe");
    }
    if (agentc_streq(k, "powershell")) {
        if (win_shell_find_pwsh(out, cap)) return 0;
        return win_shell_system_exe(
            out, cap, "System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    }
    /* pwsh: PowerShell 7 only, and only where it is on PATH. */
    return win_shell_find_pwsh(out, cap) ? 0 : -2;
}

/* Wide literal push, for the fixed ASCII switches. */
static void wpush_ascii(AgcBuf *b, const char *s) {
    for (; *s != 0; s++) wpush_ch(b, (u16)(u8)*s);
}

static int win_spawn_shell_common(const char *kind, const char *command, int fd_in,
                                  int fd_out, int fd_err, bool group) {
    if (command == NULL) return -22;
    char path[WIN_SHELL_CAND];
    int rc = os_shell_resolve(kind, path, sizeof path);
    if (rc < 0) return rc;
    const char *k = os_shell_kind(kind);

    u16 *wpath = win_utf8_to_wide(path);
    u16 *wcmd = win_utf8_to_wide(command);
    if (wpath == NULL || wcmd == NULL) {
        if (wpath != NULL) agentc_free(wpath);
        if (wcmd != NULL) agentc_free(wcmd);
        return -12;
    }

    AgcBuf line;
    agentc_memset(&line, 0, sizeof line);
    cmdline_arg(&line, wpath); /* quote the interpreter path if it needs it */
    if (agentc_streq(k, "cmd")) {
        /* cmd.exe parses its own command line: /s strips the first and last
         * quote, so wrapping the user's command verbatim hands the shell
         * exactly the text it was given. Escaping quotes the MSVC way (as
         * cmdline_arg would) instead leaves literal backslashes in the
         * command. */
        wpush_ascii(&line, " /d /s /c \"");
        wpush(&line, wcmd, wide_len(wcmd));
        wpush_ch(&line, '"');
    } else {
        /* PowerShell parses its arguments with the usual MSVC rules, so the
         * command goes through the same quoting as every other argv word. */
        wpush_ascii(&line,
                    " -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ");
        cmdline_arg(&line, wcmd);
    }
    u16 z2[2] = { 0, 0 };
    agentc_buf_push(&line, z2, 2);

    int pid = win_spawn_cmdline((const u16 *)line.p, NULL /* inherit env */, NULL,
                                fd_in, fd_out, fd_err, group);
    agentc_buf_free(&line);
    agentc_free(wpath);
    agentc_free(wcmd);
    return pid;
}

int os_spawn_shell(const char *kind, const char *command, int fd_in, int fd_out,
                   int fd_err) {
    return win_spawn_shell_common(kind, command, fd_in, fd_out, fd_err, false);
}

int os_spawn_shell_group(const char *kind, const char *command, int fd_in, int fd_out,
                         int fd_err) {
    return win_spawn_shell_common(kind, command, fd_in, fd_out, fd_err, true);
}

int os_wait(int pid, bool nohang) {
    int slot = proc_slot((WinDWORD)pid);
    if (slot < 0) return -2;
    WinDWORD w = WaitForSingleObject(g_procs[slot].h, nohang ? 0 : WIN_INFINITE);
    if (w == WIN_WAIT_TIMEOUT) return -1; /* still running */
    WinDWORD code = 0;
    GetExitCodeProcess(g_procs[slot].h, &code);
    int status = g_procs[slot].killed > 0 ? 128 + g_procs[slot].killed
                                          : (int)((code & 0xffu) << 8);
    CloseHandle(g_procs[slot].h);
    g_procs[slot].h = NULL;
    g_procs[slot].pid = 0;
    /* Releasing the job handle also kills any descendant that outlived the
     * leader (KILL_ON_JOB_CLOSE). */
    if (g_procs[slot].job != NULL) CloseHandle(g_procs[slot].job);
    g_procs[slot].job = NULL;
    g_procs[slot].killed = 0;
    return status;
}

int os_kill(int pid, int sig) {
    if (pid < 0) {
        /* Group kill: terminate the job object holding the whole tree; a
         * missing/failed job falls back to the leader pid below. */
        int gslot = proc_slot((WinDWORD)(-pid));
        if (gslot >= 0 && g_procs[gslot].job != NULL) {
            TerminateJobObject(g_procs[gslot].job, 1);
            g_procs[gslot].killed = sig;
            return 0;
        }
        pid = -pid;
    }
    int slot = proc_slot((WinDWORD)pid);
    if (slot >= 0) {
        TerminateProcess(g_procs[slot].h, 1);
        g_procs[slot].killed = sig;
        return 0;
    }
    WinHandle h = OpenProcess(WIN_PROCESS_TERMINATE, WIN_FALSE, (WinDWORD)pid);
    if (h == NULL) {
        WinDWORD e = GetLastError();
        return e == WIN_ERROR_INVALID_PARAMETER ? -3 : win_errno(e);
    }
    TerminateProcess(h, 1);
    CloseHandle(h);
    return 0;
}

int os_pipe(int fds[2]) {
    WinSecurityAttributes sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = WIN_FALSE;
    WinHandle r = NULL, w = NULL;
    if (!CreatePipe(&r, &w, &sa, 0)) return win_errno(GetLastError());
    int rfd = win_fd_alloc(r, NULL, WIN_FD_PIPE, WIN_FD_READ);
    int wfd = win_fd_alloc(w, NULL, WIN_FD_PIPE, WIN_FD_WRITE);
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0)
            win_fd_free(rfd);
        else
            CloseHandle(r);
        if (wfd >= 0)
            win_fd_free(wfd);
        else
            CloseHandle(w);
        return -24;
    }
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

/* ------------------------------------------------------------------ events */
int win_ws_init(void) {
    static int ready;
    if (ready) return 0;
    WinWsaData data;
    if (WSAStartup(0x0202, &data) != 0) return -5;
    ready = 1;
    return 0;
}

/* OS_POLL* (plat.h) and Winsock's POLL* bits do not overlap: copying one into
 * the other would ask WSAPoll for error events (POLLERR) on a read and for
 * POLLNVAL on a write, and no bit it returns would ever match an OS_POLL*
 * test. Translate both directions. POLLNVAL has no portable counterpart; it
 * means the descriptor cannot be polled at all, which is an error, so it maps
 * to OS_POLLERR. */
static short poll_events_to_wsa(short events) {
    short w = 0;
    if (events & OS_POLLIN) w |= WIN_POLLRDNORM;
    if (events & OS_POLLOUT) w |= WIN_POLLWRNORM;
    if (events & OS_POLLERR) w |= WIN_POLLERR;
    if (events & OS_POLLHUP) w |= WIN_POLLHUP;
    return w;
}

static short wsa_revents_to_poll(short revents) {
    short p = 0;
    if (revents & WIN_POLLRDNORM) p |= OS_POLLIN;
    if (revents & WIN_POLLWRNORM) p |= OS_POLLOUT;
    if (revents & (WIN_POLLERR | WIN_POLLNVAL)) p |= OS_POLLERR;
    if (revents & WIN_POLLHUP) p |= OS_POLLHUP;
    return p;
}

int os_poll(struct os_pollfd *fds, int nfds, int timeout_ms) {
    if (nfds <= 0) {
        if (timeout_ms > 0) os_sleep_ns((i64)timeout_ms * 1000000);
        return 0;
    }
    i64 deadline = timeout_ms >= 0 ? (i64)GetTickCount64() + timeout_ms : -1;
    WinPollFd *wp = agentc_alloc((size_t)nfds * sizeof *wp);
    WinHandle *waiters = agentc_alloc((size_t)nfds * sizeof *waiters);
    int *wait_index = agentc_alloc((size_t)nfds * sizeof *wait_index);
    int rc = 0;
    int ready = 0;

    for (;;) {
        win_console_poll_winch();
        int nwp = 0, nwait = 0, npipes = 0;
        ready = 0;
        for (int i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            WinFd *f = win_fd(fds[i].fd);
            if (f == NULL) {
                fds[i].revents = OS_POLLERR;
                ready++;
                continue;
            }
            if (f->kind == WIN_FD_SOCKET) {
                wp[nwp].fd = (WinSOCKET)(uintptr_t)f->handle;
                wp[nwp].events = poll_events_to_wsa(fds[i].events);
                wp[nwp].revents = 0;
                nwp++;
            } else if (f->kind == WIN_FD_PIPE && (f->flags & WIN_FD_READ) &&
                       (fds[i].events & (OS_POLLIN | OS_POLLERR | OS_POLLHUP))) {
                /* Anonymous pipe handles are not reliable waitable signals
                 * (WaitForSingleObject can stay signaled while the buffer is
                 * empty), so readiness comes from PeekNamedPipe and the wait
                 * below is a bounded sleep. A broken pipe means EOF, which is
                 * reported as readable so the caller's next read returns 0. */
                WinDWORD avail = 0;
                if (!PeekNamedPipe(f->handle, NULL, 0, NULL, &avail, NULL)) {
                    WinDWORD e = GetLastError();
                    if (e == WIN_ERROR_BROKEN_PIPE ||
                        e == WIN_ERROR_PIPE_NOT_CONNECTED)
                        fds[i].revents = OS_POLLIN | OS_POLLHUP;
                    else
                        fds[i].revents = OS_POLLERR;
                    ready++;
                } else if (avail > 0) {
                    fds[i].revents = OS_POLLIN;
                    ready++;
                } else {
                    npipes++;
                }
            } else if (nwait < 64 &&
                       (fds[i].events & (OS_POLLIN | OS_POLLERR | OS_POLLHUP))) {
                waiters[nwait] = f->handle;
                wait_index[nwait] = i;
                nwait++;
            }
        }

        if (nwp > 0) {
            win_ws_init();
            int r = WSAPoll(wp, (WinULONG)nwp, 0);
            if (r > 0) {
                int si = 0;
                for (int i = 0; i < nfds; i++) {
                    WinFd *f = win_fd(fds[i].fd);
                    if (f == NULL || f->kind != WIN_FD_SOCKET || si >= nwp) continue;
                    fds[i].revents = wsa_revents_to_poll(wp[si].revents);
                    if (fds[i].revents != 0) ready++;
                    si++;
                }
            } else if (r < 0) {
                rc = win_wsa_errno(WSAGetLastError());
                break;
            }
        }

        for (int k = 0; k < nwait; k++) {
            if (WaitForSingleObject(waiters[k], 0) == WIN_WAIT_OBJECT_0) {
                fds[wait_index[k]].revents |= OS_POLLIN;
                ready++;
            }
        }
        if (ready > 0) break;
        if (timeout_ms == 0) break;
        if (deadline >= 0 && (i64)GetTickCount64() >= deadline) break;

        WinDWORD wait_ms = WIN_INFINITE;
        if (deadline >= 0) {
            i64 rem = deadline - (i64)GetTickCount64();
            if (rem <= 0) wait_ms = 0;
            else wait_ms = rem > 0xFFFFFFFF ? 0xFFFFFFFFu : (WinDWORD)rem;
        }
        if (nwp > 0) {
            win_ws_init();
            /* Re-check pipes/console while sleeping on sockets. */
            if ((nwait > 0 || npipes > 0) && wait_ms > 16) wait_ms = 16;
            int r = WSAPoll(wp, (WinULONG)nwp, (int)wait_ms);
            if (r < 0) {
                rc = win_wsa_errno(WSAGetLastError());
                break;
            }
        } else if (nwait > 0) {
            if (npipes > 0 && wait_ms > 10) wait_ms = 10;
            if (nwait >= 64 && wait_ms > 10) wait_ms = 10;
            WaitForMultipleObjects((WinDWORD)nwait, waiters, WIN_FALSE, wait_ms);
        } else if (npipes > 0) {
            /* No waitable handle backs an empty anonymous pipe; sleep briefly
             * and peek again. Without this the loop would spin and starve the
             * child that is about to write. */
            if (wait_ms == WIN_INFINITE || wait_ms > 10) wait_ms = 10;
            os_sleep_ns((i64)wait_ms * 1000000);
        } else {
            os_sleep_ns(wait_ms == WIN_INFINITE ? 10 * 1000000
                                                : (i64)wait_ms * 1000000);
        }
    }

    agentc_free(wp);
    agentc_free(waiters);
    agentc_free(wait_index);
    /* POSIX poll contract: the ready count on success, negative errno on
     * failure. Callers (bash's read loop, the TUI, MCP) test the count. */
    return rc != 0 ? rc : ready;
}

/* --------------------------------------------------------------- open URL */
int os_open_url(const char *url) {
    u16 *wurl = win_utf8_to_wide(url);
    if (wurl == NULL) return -22;
    static const u16 verb[] = { 'o', 'p', 'e', 'n', 0 };
    WinHandle r = ShellExecuteW(NULL, verb, wurl, NULL, NULL, WIN_SW_SHOWNORMAL);
    agentc_free(wurl);
    return (u64)(uintptr_t)r > 32 ? 0 : -5;
}
