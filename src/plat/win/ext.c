/* plat/win/ext.c — Windows dynamic extension loader (LoadLibraryExW).
 *
 * The search flags are restricted to the loaded DLL's own directory plus
 * System32: no PATH, no current directory, no SetDllDirectory, no
 * AddDllDirectory. On a system where those flags are unsupported the call
 * fails with ERROR_INVALID_PARAMETER and we fail closed — we never retry with
 * the unsafe default search order. GetLastError is reported as both a small
 * text table and the numeric Win32 code; the mapped negative linux errno is
 * the return value. */
#include "agentc.h"
#include "plat.h"
#include "win.h"

static const char *win_ext_text(WinDWORD code) {
    switch (code) {
    case WIN_ERROR_SUCCESS:         return "no error";
    case WIN_ERROR_FILE_NOT_FOUND:  return "module file not found";
    case WIN_ERROR_PATH_NOT_FOUND:  return "module path not found";
    case WIN_ERROR_MOD_NOT_FOUND:   return "module not found";
    case WIN_ERROR_PROC_NOT_FOUND:  return "procedure not found";
    case WIN_ERROR_BAD_EXE_FORMAT:  return "not a valid Win32 application";
    case WIN_ERROR_DLL_INIT_FAILED: return "DLL initialization routine failed";
    case WIN_ERROR_ACCESS_DENIED:   return "access denied";
    case WIN_ERROR_INVALID_PARAMETER: return "invalid parameter (search flags unsupported?)";
    case WIN_ERROR_NOT_ENOUGH_MEMORY: return "not enough memory";
    default:                        return "dynamic loader error";
    }
}

static void win_ext_error(char *err, size_t cap, const char *what, WinDWORD code) {
    if (!err || cap == 0) return;
    (void)agentc_snprintf(err, cap, "%s: %s (error %u)", what, win_ext_text(code),
                          (unsigned)code);
    err[cap - 1] = 0;
}

int os_ext_open(const char *path, void **handle, char *err, size_t errcap) {
    if (handle) *handle = NULL;
    if (!path || !path[0]) {
        win_ext_error(err, errcap, "LoadLibraryExW", WIN_ERROR_INVALID_PARAMETER);
        return -22; /* EINVAL */
    }
    u16 *wpath = win_path_wide(path);
    if (!wpath) {
        win_ext_error(err, errcap, "LoadLibraryExW", WIN_ERROR_NOT_ENOUGH_MEMORY);
        return -12; /* ENOMEM */
    }
    WinHModule h = LoadLibraryExW(wpath, NULL,
                                  WIN_LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                      WIN_LOAD_LIBRARY_SEARCH_SYSTEM32);
    agentc_free(wpath);
    if (!h) {
        WinDWORD code = GetLastError();
        win_ext_error(err, errcap, "LoadLibraryExW", code);
        return win_errno(code);
    }
    if (handle) *handle = h;
    return 0;
}

int os_ext_sym(void *handle, const char *name, void **out, char *err, size_t errcap) {
    if (out) *out = NULL;
    if (!handle || !name || !name[0]) {
        win_ext_error(err, errcap, "GetProcAddress", WIN_ERROR_INVALID_PARAMETER);
        return -22; /* EINVAL */
    }
    void *sym = GetProcAddress((WinHModule)handle, name);
    if (!sym) {
        WinDWORD code = GetLastError();
        win_ext_error(err, errcap, "GetProcAddress", code);
        return win_errno(code);
    }
    if (out) *out = sym;
    return 0;
}

int os_ext_close(void *handle) {
    if (!handle) return 0;   /* idempotent NULL */
    if (!FreeLibrary((WinHModule)handle)) return win_errno(GetLastError());
    return 0;
}

bool os_ext_available(void) { return true; }

const char *os_ext_suffix(void) { return ".dll"; }
