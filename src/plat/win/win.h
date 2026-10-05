/* win.h — private Win32 declarations for the agentc Windows port (no windows.h).
 *
 * The Windows build has no CRT and no Windows SDK (see tools/build-windows.py
 * and the src/win .def import libraries). This header therefore declares the exact slice of the
 * Win32 ABI the port uses: types, struct layouts, constants, import prototypes
 * and the shared fd table. Layouts are the documented x86-64 ones.
 *
 * Error convention: Layer 0/1 return negative Linux errno. win_errno() maps
 * GetLastError() codes and win_wsa_errno() maps WSAGetLastError() codes; both
 * return the negative value directly, so call sites must not negate them.
 */
#ifndef AGENTC_PLAT_WIN_H
#define AGENTC_PLAT_WIN_H

#include "agentc.h"

/* ------------------------------------------------------------------ types */
typedef void *WinHandle;
typedef void *WinHModule;
typedef void *WinLocal;
typedef void *WinLPVOID;
typedef const void *WinLPCVOID;
typedef char *WinLPSTR;
typedef const char *WinLPCSTR;
typedef u16 *WinLPWSTR;
typedef const u16 *WinLPCWSTR;
typedef u16 WinWCHAR;
typedef u32 WinDWORD;
typedef u16 WinWORD;
typedef u8 WinBYTE;
typedef int WinBOOL;
typedef int WinLONG;
typedef u32 WinULONG;
typedef u64 WinULONG_PTR;
typedef u64 WinSIZE_T;
typedef i64 WinLONGLONG;
typedef u64 WinULONGLONG;
typedef u64 WinSOCKET;
typedef u64 WinDWORD_PTR;
typedef u32 WinATOM;
typedef int16_t WinSHORT;
typedef int16_t WinI16;
typedef int32_t WinI32;
typedef u32 WinUINT;

#define WIN_FALSE 0
#define WIN_TRUE 1
#define WIN_INVALID_HANDLE ((WinHandle)(i64)-1)
#define WIN_INVALID_HANDLE_VALUE ((WinHandle)(i64)-1)
#define WIN_INVALID_SOCKET ((WinSOCKET)~0ULL)
#define WIN_SOCKET_ERROR (-1)
#define WIN_INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define WIN_INVALID_SET_FILE_POINTER 0xFFFFFFFFu

#define WIN_STD_INPUT_HANDLE ((WinDWORD)-10)
#define WIN_STD_OUTPUT_HANDLE ((WinDWORD)-11)
#define WIN_STD_ERROR_HANDLE ((WinDWORD)-12)

#define WIN_FILE_TYPE_UNKNOWN 0
#define WIN_FILE_TYPE_DISK 1
#define WIN_FILE_TYPE_CHAR 2
#define WIN_FILE_TYPE_PIPE 3

/* --------------------------------------------------------------- win32 structs */
typedef struct {
    WinDWORD dwLowDateTime;
    WinDWORD dwHighDateTime;
} WinFILETIME;

typedef struct {
    WinDWORD nLength;
    WinLPVOID lpSecurityDescriptor;
    WinBOOL bInheritHandle;
} WinSecurityAttributes;

typedef struct {
    WinDWORD cb;
    WinLPWSTR lpReserved;
    WinLPWSTR lpDesktop;
    WinLPWSTR lpTitle;
    WinDWORD dwX;
    WinDWORD dwY;
    WinDWORD dwXSize;
    WinDWORD dwYSize;
    WinDWORD dwXCountChars;
    WinDWORD dwYCountChars;
    WinDWORD dwFillAttribute;
    WinDWORD dwFlags;
    WinWORD wShowWindow;
    WinWORD cbReserved2;
    WinBYTE *lpReserved2;
    WinHandle hStdInput;
    WinHandle hStdOutput;
    WinHandle hStdError;
} WinStartupInfoW;

typedef struct {
    WinHandle hProcess;
    WinHandle hThread;
    WinDWORD dwProcessId;
    WinDWORD dwThreadId;
} WinProcessInformation;

/* Job objects: the group-spawn path puts the child in a job with
 * KILL_ON_JOB_CLOSE, and os_kill(-pid) terminates the whole job. */
typedef struct {
    i64 PerProcessUserTimeLimit;
    i64 PerJobUserTimeLimit;
    WinDWORD LimitFlags;
    WinSIZE_T MinimumWorkingSetSize;
    WinSIZE_T MaximumWorkingSetSize;
    WinDWORD ActiveProcessLimit;
    WinULONG_PTR Affinity;
    WinDWORD PriorityClass;
    WinDWORD SchedulingClass;
} WinJobObjectBasicLimitInformation;

typedef struct {
    u64 ReadOperationCount;
    u64 WriteOperationCount;
    u64 OtherOperationCount;
    u64 ReadTransferCount;
    u64 WriteTransferCount;
    u64 OtherTransferCount;
} WinIoCounters;

typedef struct {
    WinJobObjectBasicLimitInformation BasicLimitInformation;
    WinIoCounters IoInfo;
    WinSIZE_T ProcessMemoryLimit;
    WinSIZE_T JobMemoryLimit;
    WinSIZE_T PeakProcessMemoryUsed;
    WinSIZE_T PeakJobMemoryUsed;
} WinJobObjectExtendedLimitInformation;

typedef struct {
    WinDWORD dwFileAttributes;
    WinFILETIME ftCreationTime;
    WinFILETIME ftLastAccessTime;
    WinFILETIME ftLastWriteTime;
    WinDWORD nFileSizeHigh;
    WinDWORD nFileSizeLow;
    WinDWORD dwReserved0;
    WinDWORD dwReserved1;
    WinWCHAR cFileName[260];
    WinWCHAR cAlternateFileName[14];
} Win32FindDataW;

typedef struct {
    WinDWORD dwFileAttributes;
    WinFILETIME ftCreationTime;
    WinFILETIME ftLastAccessTime;
    WinFILETIME ftLastWriteTime;
    WinDWORD dwVolumeSerialNumber;
    WinDWORD nFileSizeHigh;
    WinDWORD nFileSizeLow;
    WinDWORD nNumberOfLinks;
    WinDWORD nFileIndexHigh;
    WinDWORD nFileIndexLow;
} WinByHandleFileInformation;

typedef struct {
    WinDWORD dwFileAttributes;
    WinFILETIME ftCreationTime;
    WinFILETIME ftLastAccessTime;
    WinFILETIME ftLastWriteTime;
    WinDWORD nFileSizeHigh;
    WinDWORD nFileSizeLow;
} WinFileAttributeData;

typedef struct {
    WinI16 X;
    WinI16 Y;
} WinCoord;

typedef struct {
    WinI16 Left;
    WinI16 Top;
    WinI16 Right;
    WinI16 Bottom;
} WinSmallRect;

typedef struct {
    WinCoord dwSize;
    WinCoord dwCursorPosition;
    WinWORD wAttributes;
    WinSmallRect srWindow;
    WinCoord dwMaximumWindowSize;
} WinConsoleScreenBufferInfo;

typedef struct {
    WinDWORD bKeyDown;
    WinWORD wRepeatCount;
    WinWORD wVirtualKeyCode;
    WinWORD wVirtualScanCode;
    union {
        WinWCHAR UnicodeChar;
        char AsciiChar;
    } uChar;
    WinDWORD dwControlKeyState;
} WinKeyEventRecord;

typedef struct {
    WinWORD EventType;
    WinKeyEventRecord KeyEvent;
} WinInputRecord;

typedef struct {
    u16 wVersion;
    u16 wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
    u16 iMaxSockets;
    u16 iMaxUdpDg;
    void *lpVendorInfo;
} WinWsaData;

typedef struct {
    u16 sin_family;
    u16 sin_port;
    u32 sin_addr;
    u8 sin_zero[8];
} WinSockaddrIn;

typedef struct {
    WinSOCKET fd;
    WinI16 events;
    WinI16 revents;
} WinPollFd;

/* Winsock poll bits; values differ from plat.h's OS_POLL* (see os_poll in
 * sys.c). */
#define WIN_POLLRDNORM 0x0100
#define WIN_POLLWRNORM 0x0010
#define WIN_POLLERR    0x0001
#define WIN_POLLHUP    0x0002
#define WIN_POLLNVAL   0x0004

typedef union {
    struct {
        u32 LowPart;
        WinI32 HighPart;
    } u;
    i64 QuadPart;
} WinLargeInteger;

/* --------------------------------------------------------------- constants */
#define WIN_GENERIC_READ 0x80000000u
#define WIN_GENERIC_WRITE 0x40000000u
#define WIN_FILE_APPEND_DATA 0x00000004u
#define WIN_FILE_LIST_DIRECTORY 0x00000001u
#define WIN_FILE_SHARE_READ 0x00000001u
#define WIN_FILE_SHARE_WRITE 0x00000002u
#define WIN_FILE_SHARE_DELETE 0x00000004u
#define WIN_OPEN_EXISTING 3u
#define WIN_CREATE_ALWAYS 2u
#define WIN_OPEN_ALWAYS 4u
#define WIN_TRUNCATE_EXISTING 5u
#define WIN_FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define WIN_FILE_ATTRIBUTE_NORMAL 0x00000080u
#define WIN_FILE_ATTRIBUTE_REPARSE_POINT 0x00000400u
#define WIN_FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define WIN_FILE_FLAG_OPEN_REPARSE_POINT 0x00200000u
#define WIN_FILE_BEGIN 0
#define WIN_FILE_CURRENT 1
#define WIN_FILE_END 2

#define WIN_MEM_COMMIT 0x00001000u
#define WIN_MEM_RESERVE 0x00002000u
#define WIN_MEM_RELEASE 0x00008000u
#define WIN_PAGE_READWRITE 0x04u

#define WIN_HANDLE_FLAG_INHERIT 0x00000001u

#define WIN_WAIT_OBJECT_0 0x00000000u
#define WIN_WAIT_ABANDONED 0x00000080u
#define WIN_WAIT_TIMEOUT 0x00000102u
#define WIN_INFINITE 0xFFFFFFFFu
#define WIN_PROCESS_TERMINATE 0x0001u
#define WIN_PROCESS_QUERY_LIMITED_INFORMATION 0x1000u

#define WIN_CREATE_UNICODE_ENVIRONMENT 0x00000400u
#define WIN_CREATE_NEW_PROCESS_GROUP 0x00000200u
#define WIN_JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000u
#define WIN_JOB_OBJECT_EXTENDED_LIMIT_INFORMATION 9
#define WIN_STARTF_USESTDHANDLES 0x00000100u
#define WIN_SW_SHOWNORMAL 1

#define WIN_ENABLE_PROCESSED_INPUT 0x0001u
#define WIN_ENABLE_LINE_INPUT 0x0002u
#define WIN_ENABLE_ECHO_INPUT 0x0004u
#define WIN_ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200u
#define WIN_ENABLE_PROCESSED_OUTPUT 0x0001u
#define WIN_ENABLE_WRAP_AT_EOL_OUTPUT 0x0002u
#define WIN_ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004u
#define WIN_DISABLE_NEWLINE_AUTO_RETURN 0x0008u

#define WIN_ERROR_SUCCESS 0u
#define WIN_ERROR_FILE_NOT_FOUND 2u
#define WIN_ERROR_PATH_NOT_FOUND 3u
#define WIN_ERROR_TOO_MANY_OPEN_FILES 4u
#define WIN_ERROR_ACCESS_DENIED 5u
#define WIN_ERROR_INVALID_HANDLE 6u
#define WIN_ERROR_NOT_ENOUGH_MEMORY 8u
#define WIN_ERROR_BAD_LENGTH 24u
#define WIN_ERROR_NOT_READY 21u
#define WIN_ERROR_SHARING_VIOLATION 32u
#define WIN_ERROR_NOT_SUPPORTED 50u
#define WIN_ERROR_FILE_EXISTS 80u
#define WIN_ERROR_CANNOT_MAKE 82u
#define WIN_ERROR_INVALID_PARAMETER 87u
#define WIN_ERROR_BROKEN_PIPE 109u
#define WIN_ERROR_BUFFER_OVERFLOW 111u
#define WIN_ERROR_DISK_FULL 112u
#define WIN_ERROR_CALL_NOT_IMPLEMENTED 120u
#define WIN_ERROR_SEM_TIMEOUT 121u
#define WIN_ERROR_INSUFFICIENT_BUFFER 122u
#define WIN_ERROR_NO_MORE_FILES 18u
#define WIN_ERROR_HANDLE_EOF 38u
#define WIN_ERROR_DIR_NOT_EMPTY 145u
#define WIN_ERROR_ALREADY_EXISTS 183u
#define WIN_ERROR_FILENAME_EXCED_RANGE 206u
#define WIN_ERROR_PIPE_BUSY 231u
#define WIN_ERROR_NO_DATA 232u
#define WIN_ERROR_PIPE_NOT_CONNECTED 233u
#define WIN_ERROR_DIRECTORY 267u
#define WIN_ERROR_OPERATION_ABORTED 995u
#define WIN_ERROR_IO_INCOMPLETE 996u
#define WIN_ERROR_IO_PENDING 997u
#define WIN_WAIT_TIMEOUT_CODE 258u
#define WIN_ERROR_NOT_FOUND 1168u
#define WIN_ERROR_MOD_NOT_FOUND 126u
#define WIN_ERROR_PROC_NOT_FOUND 127u
#define WIN_ERROR_BAD_EXE_FORMAT 193u
#define WIN_ERROR_DLL_INIT_FAILED 1114u

/* LoadLibraryExW search flags: the DLL's own directory plus System32
 * only. Unsupported on pre-Win8 systems without KB2533623, where the call
 * fails with ERROR_INVALID_PARAMETER and the loader fails closed. */
#define WIN_LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100u
#define WIN_LOAD_LIBRARY_SEARCH_SYSTEM32     0x00000800u

#define WIN_DNS_TYPE_A 1u
#define WIN_DNS_QUERY_STANDARD 0u
#define WIN_DNS_FREE_RECORD_LIST 1

#define WIN_BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002u

typedef struct {
    WinHandle find;
    bool pending;
    bool done;
    Win32FindDataW data;
} WinFindCtx;

#define WIN_MOVEFILE_REPLACE_EXISTING 0x00000001u

/* ------------------------------------------------------------- fd table */
#define WIN_FD_MAX 1024

enum {
    WIN_FD_FREE = 0,
    WIN_FD_FILE,    /* regular file, opened O_RDONLY/O_WRONLY/O_RDWR */
    WIN_FD_DIR,     /* directory handle; path/aux drive FindFirstFileW */
    WIN_FD_PIPE,    /* anonymous pipe or console byte pipe */
    WIN_FD_CONSOLE, /* real console handle (stdout/stderr) */
    WIN_FD_SOCKET,  /* ws2_32 SOCKET */
};

#define WIN_FD_READ 0x01u
#define WIN_FD_WRITE 0x02u
#define WIN_FD_APPEND 0x04u
#define WIN_FD_STD 0x08u
#define WIN_FD_DIR_STARTED 0x10u

typedef struct {
    WinHandle handle;       /* read handle, or the only handle */
    WinHandle write_handle; /* distinct write handle, else NULL */
    u8 kind;
    u16 flags;
    u16 __pad;
    char *path;  /* UTF-8 directory path (WIN_FD_DIR), else NULL */
    void *aux;   /* WIN_FD_DIR: heap find-context; console: WinHandle */
} WinFd;

WinFd *win_fd(int fd);
int win_fd_alloc(WinHandle handle, WinHandle write_handle, int kind, u16 flags);
void win_fd_free(int fd);
WinHandle win_fd_handle(int fd);

/* UTF-8 <-> UTF-16 (MultiByteToWideChar/WideCharToMultiByte, agentc_alloc'd). */
u16 *win_utf8_to_wide(const char *s);
char *win_wide_to_utf8(const u16 *s);
size_t wide_len(const u16 *s);
/* Path conversion for Win32 *W calls: UTF-8 -> UTF-16, separators normalised,
 * long paths prefixed with \\?\. */
u16 *win_path_wide(const char *path);

int win_errno(WinDWORD err);
int win_wsa_errno(int err);

/* Console support (win/console.c). win_console_init() is called from os_init
 * and swaps fd 0 for a reader-thread byte pipe when stdin is a console. */
void win_console_init(void);
void win_console_poll_winch(void);
void win_console_set_handler(void (*handler)(void));

/* ws2_32 startup shared by plat/win (os_poll) and net/win. */
int win_ws_init(void);

/* ------------------------------------------------------- kernel32 imports */
__declspec(dllimport) WinWCHAR *GetCommandLineW(void);
__declspec(dllimport) WinWCHAR *GetEnvironmentStringsW(void);
__declspec(dllimport) WinBOOL FreeEnvironmentStringsW(WinWCHAR *);
__declspec(dllimport) WinLocal LocalFree(WinLocal);
__declspec(dllimport) WinHandle GetStdHandle(WinDWORD);
__declspec(dllimport) WinBOOL SetHandleInformation(WinHandle, WinDWORD, WinDWORD);
__declspec(dllimport) WinBOOL GetHandleInformation(WinHandle, WinDWORD *);
__declspec(dllimport) WinDWORD GetFileType(WinHandle);
__declspec(dllimport) WinHandle CreateFileW(WinLPCWSTR, WinDWORD, WinDWORD,
                                            WinSecurityAttributes *, WinDWORD,
                                            WinDWORD, WinHandle);
__declspec(dllimport) WinBOOL ReadFile(WinHandle, WinLPVOID, WinDWORD, WinDWORD *,
                                       void *);
__declspec(dllimport) WinBOOL WriteFile(WinHandle, WinLPCVOID, WinDWORD, WinDWORD *,
                                        void *);
__declspec(dllimport) WinBOOL CloseHandle(WinHandle);
__declspec(dllimport) WinBOOL PeekNamedPipe(WinHandle, WinLPVOID, WinDWORD,
                                            WinDWORD *, WinDWORD *, WinDWORD *);
__declspec(dllimport) WinBOOL CreatePipe(WinHandle *, WinHandle *,
                                         WinSecurityAttributes *, WinDWORD);
__declspec(dllimport) WinBOOL SetEndOfFile(WinHandle);
__declspec(dllimport) WinBOOL SetFilePointerEx(WinHandle, WinLargeInteger,
                                               WinLargeInteger *, WinDWORD);
__declspec(dllimport) WinBOOL GetFileInformationByHandle(
    WinHandle, WinByHandleFileInformation *);
__declspec(dllimport) WinBOOL GetFileAttributesExW(WinLPCWSTR, int, void *);
__declspec(dllimport) WinBOOL CreateDirectoryW(WinLPCWSTR, WinSecurityAttributes *);
__declspec(dllimport) WinBOOL RemoveDirectoryW(WinLPCWSTR);
__declspec(dllimport) WinBOOL DeleteFileW(WinLPCWSTR);
__declspec(dllimport) WinBOOL MoveFileExW(WinLPCWSTR, WinLPCWSTR, WinDWORD);
__declspec(dllimport) WinDWORD GetCurrentDirectoryW(WinDWORD, WinLPWSTR);
__declspec(dllimport) WinBOOL SetCurrentDirectoryW(WinLPCWSTR);
__declspec(dllimport) void *VirtualAlloc(void *, WinSIZE_T, WinDWORD, WinDWORD);
__declspec(dllimport) WinBOOL VirtualFree(void *, WinSIZE_T, WinDWORD);
__declspec(dllimport) WinBOOL CreateProcessW(WinLPCWSTR, WinLPWSTR,
                                              WinSecurityAttributes *,
                                              WinSecurityAttributes *, WinBOOL,
                                              WinDWORD, void *, WinLPCWSTR,
                                              WinStartupInfoW *,
                                              WinProcessInformation *);
__declspec(dllimport) WinHandle CreateJobObjectW(WinSecurityAttributes *, WinLPCWSTR);
__declspec(dllimport) WinBOOL SetInformationJobObject(WinHandle, int, void *, WinDWORD);
__declspec(dllimport) WinBOOL AssignProcessToJobObject(WinHandle, WinHandle);
__declspec(dllimport) WinBOOL TerminateJobObject(WinHandle, WinUINT);
__declspec(dllimport) WinBOOL GetExitCodeProcess(WinHandle, WinDWORD *);
__declspec(dllimport) WinDWORD WaitForSingleObject(WinHandle, WinDWORD);
__declspec(dllimport) WinDWORD WaitForMultipleObjects(WinDWORD,
                                                       const WinHandle *, WinBOOL,
                                                       WinDWORD);
__declspec(dllimport) WinHandle OpenProcess(WinDWORD, WinBOOL, WinDWORD);
__declspec(dllimport) WinBOOL TerminateProcess(WinHandle, WinUINT);
__declspec(dllimport) u64 GetTickCount64(void);
__declspec(dllimport) void GetSystemTimeAsFileTime(WinFILETIME *);
__declspec(dllimport) void Sleep(WinDWORD);
__declspec(dllimport) WinDWORD GetLastError(void);
__declspec(dllimport) WinHModule LoadLibraryExW(WinLPCWSTR, WinHandle, WinDWORD);
__declspec(dllimport) void *GetProcAddress(WinHModule, WinLPCSTR);
__declspec(dllimport) WinBOOL FreeLibrary(WinHModule);
__declspec(dllimport) WinBOOL GetConsoleMode(WinHandle, WinDWORD *);
__declspec(dllimport) WinBOOL SetConsoleMode(WinHandle, WinDWORD);
__declspec(dllimport) WinBOOL SetConsoleCtrlHandler(WinBOOL (*handler)(WinDWORD), WinBOOL add);
__declspec(dllimport) WinBOOL GetConsoleScreenBufferInfo(
    WinHandle, WinConsoleScreenBufferInfo *);
__declspec(dllimport) WinHandle FindFirstFileW(WinLPCWSTR, Win32FindDataW *);
__declspec(dllimport) WinBOOL FindNextFileW(WinHandle, Win32FindDataW *);
__declspec(dllimport) WinBOOL FindClose(WinHandle);
__declspec(dllimport) WinHandle CreateThread(WinSecurityAttributes *, WinSIZE_T,
                                             WinDWORD (*)(void *), void *, WinDWORD,
                                             WinDWORD *);
__declspec(dllimport) WinBOOL ReadConsoleInputW(WinHandle, WinInputRecord *,
                                                WinDWORD, WinDWORD *);
__declspec(dllimport) int MultiByteToWideChar(WinUINT, WinDWORD, WinLPCSTR, int,
                                              WinLPWSTR, int);
__declspec(dllimport) int WideCharToMultiByte(WinUINT, WinDWORD, WinLPCWSTR, int,
                                              WinLPSTR, int, WinLPCSTR *,
                                              WinBOOL *);
__attribute__((noreturn)) __declspec(dllimport) void ExitProcess(WinUINT);

/* ----------------------------------------------------------- shell32 import */
__declspec(dllimport) WinWCHAR **CommandLineToArgvW(WinLPCWSTR, int *);
__declspec(dllimport) WinHandle ShellExecuteW(WinHandle, WinLPCWSTR, WinLPCWSTR,
                                             WinLPCWSTR, WinLPCWSTR, int);

/* ------------------------------------------------------------ bcrypt import */
__declspec(dllimport) int BCryptGenRandom(void *, u8 *, u32, u32);

/* ------------------------------------------------------------- dnsapi types */
typedef struct WinDnsRecordA {
    struct WinDnsRecordA *pNext;
    WinWORD wType;
    WinWORD wDataLength;
    WinDWORD flags;
    WinDWORD dwTtl;
    WinDWORD dwReserved;
    struct {
        u32 IpAddress;
    } data;
} WinDnsRecordA;

__declspec(dllimport) int DnsQuery_A(const char *, WinWORD, WinDWORD, void *,
                                     WinDnsRecordA **, void **);
__declspec(dllimport) void DnsFree(void *, int);

/* ------------------------------------------------------------ ws2_32 imports */
__declspec(dllimport) int WSAStartup(WinWORD, WinWsaData *);
__declspec(dllimport) int WSAGetLastError(void);
__declspec(dllimport) int WSAPoll(WinPollFd *, WinULONG, int);

#endif /* AGENTC_PLAT_WIN_H */
