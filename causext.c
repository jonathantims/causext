// causext — black-box causal profiling for one Windows command line.
// License: The Unlicense. See LICENSE. No warranty and no limits on use.
//
// Build:  build.bat          (config.bat sets VCVARS64; products land in out\)
// Check:  build.bat run      or  out\causext.exe --self-test
// Run:    out\causext.exe --runs 20 --samples 30 -- my.exe args
// Sample: out\causext.exe --sampling --runs 3 --samples 40 -- my.exe args
//
// Each causal run starts the command and records its wall-clock end-to-end
// time. During the run the profiler repeatedly suspends one of the command's
// threads, chosen uniformly, for a duration drawn uniformly from the
// configured range. It records the instruction pointer and the measured
// length of the suspension. That is the performance ledger.
//
// --sampling is the comparison mode. It suspends every thread only long
// enough to read the instruction pointer, then ranks locations, blocks, and
// functions by how often they were observed. That share is not an effect
// on end-to-end time. Both modes write the same file names under --out,
// which defaults to out.
//
// Debug symbols then build two derived ledgers. The block ledger sums
// pauses that land in the same source line (or, when the PDB has no line,
// the same 16-byte window of a function). The function ledger sums pauses
// that land in the same function. Instructions with no symbol stay their
// own columns, so unknown code is not poured into one bin.
//
// The same estimator is run on each ledger. It is specified in full in
// ESTIMATOR_DOC below, and that text is copied into every rank file.
// Rank files order columns by estimated impact on end-to-end time.
//
// This file has no #include. Windows entry points are declared by hand.
// The self-test fuzzes the estimator and also drives a real multi-thread
// child process.

typedef unsigned short wchar_t;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef long LONG;
typedef void *HANDLE;
typedef unsigned long long size_t;
typedef unsigned long long uint64_t;
typedef unsigned int uint32_t;

#define NULL 0
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_hack)-1)

/* intptr stand-in so INVALID_HANDLE_VALUE can be written before size types settle */
typedef long long intptr_hack;

/* Satisfies the reference the compiler emits for this file's floating-point
   constants when the CRT is not linked. The linker looks up this name with
   external linkage. This program never reads the value. */
int _fltused = 0;

/* Fill size bytes at dst with val. The compiler emits calls to this for
   zeroing structs. Returns dst. Linked by hand because there is no CRT. */
void *memset(void *dst, int val, size_t size) {
    unsigned char *d = (unsigned char *)dst;
    /* Store one byte per step, including a zero length which does nothing. */
    while (size--) *d++ = (unsigned char)val;
    return dst;
}

/* Copy size bytes from src to dst. The ranges must not overlap; overlapping
   copies use memmove. Returns dst. The compiler emits this for struct copies. */
void *memcpy(void *dst, const void *src, size_t size) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    /* Forward copy, one byte at a time. */
    while (size--) *d++ = *s++;
    return dst;
}

/* Copy size bytes from src to dst, correctly when the ranges overlap.
   Returns dst. A forward copy would clobber a destination that starts
   inside the source, so that case walks backward from the last byte. */
void *memmove(void *dst, const void *src, size_t size) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    /* Same address or an empty copy: nothing to move. */
    if (d == s || size == 0) return dst;
    /* Destination is before the source, so reading ahead of the write is safe. */
    if (d < s) {
        while (size--) *d++ = *s++;
    /* Destination overlaps the tail, so the copy starts at the last byte. */
    } else {
        d += size;
        s += size;
        while (size--) *--d = *--s;
    }
    return dst;
}

typedef char cx_ptr_ok[(sizeof(void *) == 8) ? 1 : -1];

#define CX_MAX_RUNS 128
#define CX_MAX_PER_RUN 128
#define CX_MAX_SAMPLES (CX_MAX_RUNS * CX_MAX_PER_RUN)
#define CX_MAX_LOCS 2048
#define CX_LOC_HASH 4096
#define CX_MAX_MODS 96
#define CX_MAX_JOINT 96
#define CX_MAX_THREADS 256
#define CX_NAME 160
#define CX_LABEL 240
#define CX_PATH 520
#define CX_BLOCK 16

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x00000001u
#define FILE_SHARE_WRITE 0x00000002u
#define OPEN_EXISTING 3u
#define CREATE_ALWAYS 2u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_RELEASE 0x8000u
#define PAGE_READWRITE 0x04u
#define CREATE_NO_WINDOW 0x08000000u
#define STARTF_USESTDHANDLES 0x00000100u
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define TH32CS_SNAPTHREAD 0x00000004u
#define TH32CS_SNAPMODULE 0x00000008u
#define THREAD_SUSPEND_RESUME 0x0002u
#define THREAD_GET_CONTEXT 0x0008u
#define THREAD_QUERY_INFORMATION 0x0040u
#define THREAD_QUERY_LIMITED 0x0800u
#define TOKEN_ADJUST_PRIVILEGES 0x0020u
#define TOKEN_QUERY 0x0008u
#define SE_PRIVILEGE_ENABLED 0x00000002u
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define CP_ACP 0u
#define FORMAT_MESSAGE_FROM_SYSTEM 0x00001000u
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x00000200u
#define ERROR_ALREADY_EXISTS 183u
#define CONTEXT_FLAGS_OFFSET 0x30
#define CONTEXT_RIP_OFFSET 0xF8
#define CONTEXT_BYTES 1232
#define CONTEXT_AMD64_CONTROL 0x00100001u

/* The 64-bit integer Windows uses for the performance counter. QueryPerformanceCounter
   and QueryPerformanceFrequency write one of these. This program only reads the
   full 64-bit view, then converts a difference of two readings into microseconds.
   The other views the real Windows union has are not declared, because nothing here
   reads the counter as two 32-bit halves. */
typedef union _LARGE_INTEGER {
    /* Tick count or ticks per second, depending on which API filled the union. */
    long long QuadPart;
} LARGE_INTEGER;

/* Passed to CreateFileW so a quiet child can inherit the NUL device as its
   standard handles. The record is zeroed first, then nLength and bInheritHandle
   are set. No security descriptor is attached. */
typedef struct _SECURITY_ATTRIBUTES {
    /* Byte size of this struct. CreateFileW uses it to know which version it was given. */
    DWORD nLength;
    /* Unused here. Left NULL, so the new handle keeps the default security. */
    void *lpSecurityDescriptor;
    /* Nonzero when the handle may be inherited by a child process. Set for the NUL handle. */
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES;

/* Startup block for CreateProcessW. It is zeroed, then cb is set. For a quiet
   child, dwFlags selects the standard-handle fields and those three handles
   are the NUL device. The window-position fields stay zero and are ignored
   unless the matching flag bit is set, which this program never sets. */
typedef struct _STARTUPINFOW {
    /* Byte size of this struct. CreateProcessW rejects the call when this is wrong. */
    DWORD cb;
    /* Reserved. Left NULL. */
    wchar_t *lpReserved;
    /* Desktop name. Left NULL, so the child uses the caller's desktop. */
    wchar_t *lpDesktop;
    /* Console title. Left NULL. A console child keeps its own title. */
    wchar_t *lpTitle;
    /* Window origin, in pixels. Unused because STARTF_USEPOSITION is not set. */
    DWORD dwX;
    /* Window origin, in pixels. Unused because STARTF_USEPOSITION is not set. */
    DWORD dwY;
    /* Window size, in pixels. Unused because STARTF_USESIZE is not set. */
    DWORD dwXSize;
    /* Window size, in pixels. Unused because STARTF_USESIZE is not set. */
    DWORD dwYSize;
    /* Console buffer width. Unused because STARTF_USECOUNTCHARS is not set. */
    DWORD dwXCountChars;
    /* Console buffer height. Unused because STARTF_USECOUNTCHARS is not set. */
    DWORD dwYCountChars;
    /* Console text and background colors. Unused because STARTF_USEFILLATTRIBUTE is not set. */
    DWORD dwFillAttribute;
    /* Bit flags saying which of the fields above CreateProcessW should honor.
       This program sets STARTF_USESTDHANDLES only, and only for a quiet child. */
    DWORD dwFlags;
    /* Show-window command. Unused because STARTF_USESHOWWINDOW is not set. */
    WORD wShowWindow;
    /* Byte length of the reserved block below. Left zero. */
    WORD cbReserved2;
    /* Reserved block. Left NULL. */
    unsigned char *lpReserved2;
    /* Standard input handed to the child. The NUL device when the child is quiet. */
    HANDLE hStdInput;
    /* Standard output handed to the child. The NUL device when the child is quiet. */
    HANDLE hStdOutput;
    /* Standard error handed to the child. The NUL device when the child is quiet. */
    HANDLE hStdError;
} STARTUPINFOW;

/* What CreateProcessW returns after it starts the child. The process handle
   is waited on, queried for the exit code, and closed. The thread handle is
   closed immediately. The process id is what toolhelp filters on. The thread
   id is not used after the call. */
typedef struct _PROCESS_INFORMATION {
    /* Handle of the new process. Signaled when the process exits. */
    HANDLE hProcess;
    /* Handle of the new process's first thread. Closed without waiting. */
    HANDLE hThread;
    /* Process id used to snapshot that process's threads and modules. */
    DWORD dwProcessId;
    /* Thread id of the primary thread. Not used after CreateProcessW returns. */
    DWORD dwThreadId;
} PROCESS_INFORMATION;

/* One row from a system-wide thread snapshot. Thread32First and Thread32Next
   fill it. Only rows whose owner process matches the child are kept, and only
   the thread id from those rows is used to open the thread. dwSize must be
   set to the byte size of this struct before the first call and again before
   each next call. */
typedef struct tagTHREADENTRY32 {
    /* Byte size of this struct. The API uses it as a version check. */
    DWORD dwSize;
    /* Reference count reported by toolhelp. Not read. */
    DWORD cntUsage;
    /* Thread id. Passed to OpenThread when the owner process is the child. */
    DWORD th32ThreadID;
    /* Process that owns the thread. Compared with the child's process id. */
    DWORD th32OwnerProcessID;
    /* Base scheduling priority. Not read. */
    LONG tpBasePri;
    /* Priority delta. Not read. */
    LONG tpDeltaPri;
    /* Toolhelp flags. Not read. */
    DWORD dwFlags;
} THREADENTRY32;

/* One loaded module from a snapshot of a single process. Module32FirstW and
   Module32NextW fill it. The base and size turn a raw instruction pointer into
   a module-relative offset, so a new address-space layout does not look like
   a new instruction. The path is the stable identity of the module. dwSize
   must be set before each call. */
typedef struct tagMODULEENTRY32W {
    /* Byte size of this struct. The API uses it as a version check. */
    DWORD dwSize;
    /* Module identifier from toolhelp. Not read. */
    DWORD th32ModuleID;
    /* Process the module is loaded in. The snapshot was already limited to one process. */
    DWORD th32ProcessID;
    /* Global load count. Not read. */
    DWORD GlblcntUsage;
    /* Per-process load count. Not read. */
    DWORD ProccntUsage;
    /* Base address of the module in the child. Subtracted from RIP to get the offset. */
    unsigned char *modBaseAddr;
    /* Size of the module image, in bytes. A RIP belongs to this module when it
       falls in [base, base + size). */
    DWORD modBaseSize;
    /* Module handle. Not read. The path is the identity used later. */
    void *hModule;
    /* File name of the module, not the full path. Used when the path has no usable name. */
    wchar_t szModule[256];
    /* Full path of the module. Modules with the same path are one module across runs. */
    wchar_t szExePath[260];
} MODULEENTRY32W;

/* One symbol from dbghelp. The caller sets SizeOfStruct and MaxNameLen before
   SymFromAddr, and points Name at extra bytes past this struct so the name
   can be longer than one character. On success, Address, Name, and NameLen
   describe the function that contains the queried address. The self-test
   requires sizeof(SYMBOL_INFO) to be 88, which is this layout without the
   extra name bytes. */
typedef struct _SYMBOL_INFO {
    /* Byte size of the fixed part of this struct, not counting extra name bytes. */
    unsigned long SizeOfStruct;
    /* Type index inside the PDB. Not read. */
    unsigned long TypeIndex;
    /* Reserved for dbghelp. Left zero. */
    unsigned long long Reserved[2];
    /* Unique symbol index. Not read. */
    unsigned long Index;
    /* Size of the symbol, in bytes, when dbghelp knows it. Not read. */
    unsigned long Size;
    /* Base address of the module dbghelp loaded this symbol from. */
    unsigned long long ModBase;
    /* Symbol flags. Not read. */
    unsigned long Flags;
    /* Value of the symbol for symbols that have one. Not read for code addresses. */
    unsigned long long Value;
    /* Start address of the function that contains the queried address. */
    unsigned long long Address;
    /* Register id for register-relative symbols. Not read. */
    unsigned long Register;
    /* Scope of the symbol. Not read. */
    unsigned long Scope;
    /* Kind of symbol. Not read. A named function is recognized by a nonempty Name. */
    unsigned long Tag;
    /* Number of characters dbghelp wrote into Name, not counting a NUL. */
    unsigned long NameLen;
    /* Capacity of Name, in characters, including the room past the fixed struct. */
    unsigned long MaxNameLen;
    /* First byte of the symbol name. Further bytes sit in the caller-provided tail. */
    char Name[1];
} SYMBOL_INFO;

/* One source line from dbghelp. SizeOfStruct is set before SymGetLineFromAddr64.
   On success, LineNumber and FileName name the source line of the instruction,
   and Address is the first byte of that line. The self-test requires
   sizeof(IMAGEHLP_LINE64) to be 40. */
typedef struct _IMAGEHLP_LINE64 {
    /* Byte size of this struct. Set before the line query. */
    DWORD SizeOfStruct;
    /* Opaque key dbghelp uses while walking lines. Not read. */
    void *Key;
    /* 1-based source line number. Zero means the query did not supply a line. */
    DWORD LineNumber;
    /* Pointer to the source file name. Owned by dbghelp until cleanup. */
    char *FileName;
    /* Address of the start of this source line inside the loaded module. */
    unsigned long long Address;
} IMAGEHLP_LINE64;

/* Locally unique identifier for a Windows privilege. LookupPrivilegeValueA
   fills it for SeDebugPrivilege. Both halves together are the id. Neither
   half is interpreted on its own. */
typedef struct _LUID {
    /* Low 32 bits of the privilege id. */
    DWORD LowPart;
    /* High 32 bits of the privilege id, signed because that is the Windows layout. */
    LONG HighPart;
} LUID;

/* One privilege and the attribute bits to apply to it. The only use here is
   enabling SeDebugPrivilege on the process token. */
typedef struct _LUID_AND_ATTRIBUTES {
    /* Which privilege, as returned by LookupPrivilegeValueA. */
    LUID Luid;
    /* Attribute bits. SE_PRIVILEGE_ENABLED asks the token to turn the privilege on. */
    DWORD Attributes;
} LUID_AND_ATTRIBUTES;

/* Variable-length privilege list for AdjustTokenPrivileges. This declaration
   holds one entry, which is all the debug-privilege call needs. PrivilegeCount
   says how many entries in Privileges are live. */
typedef struct _TOKEN_PRIVILEGES {
    /* Number of live entries in Privileges. Set to 1. */
    DWORD PrivilegeCount;
    /* The privileges to change. One slot is declared; the API reads PrivilegeCount of them. */
    LUID_AND_ATTRIBUTES Privileges[1];
} TOKEN_PRIVILEGES;

extern void __stdcall ExitProcess(DWORD code);
extern HANDLE __stdcall GetStdHandle(DWORD n);
extern BOOL __stdcall WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *wrote, void *ov);
extern BOOL __stdcall ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov);
extern DWORD __stdcall GetFileSize(HANDLE h, DWORD *high);
extern HANDLE __stdcall CreateFileW(const wchar_t *name, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disp, DWORD flags, HANDLE tmpl);
extern BOOL __stdcall CloseHandle(HANDLE h);
extern void *__stdcall VirtualAlloc(void *addr, size_t size, DWORD type, DWORD protect);
extern BOOL __stdcall VirtualFree(void *addr, size_t size, DWORD type);
extern BOOL __stdcall QueryPerformanceCounter(LARGE_INTEGER *c);
extern BOOL __stdcall QueryPerformanceFrequency(LARGE_INTEGER *f);
extern void __stdcall Sleep(DWORD ms);
extern DWORD __stdcall GetLastError(void);
extern DWORD __stdcall FormatMessageA(DWORD flags, const void *src, DWORD id, DWORD lang, char *buf, DWORD n, void *args);
extern wchar_t *__stdcall GetCommandLineW(void);
extern wchar_t **__stdcall CommandLineToArgvW(const wchar_t *cmd, int *argc);
extern void *__stdcall LocalFree(void *p);
extern DWORD __stdcall GetModuleFileNameW(void *mod, wchar_t *buf, DWORD n);
extern BOOL __stdcall CreateDirectoryW(const wchar_t *path, void *sa);
extern BOOL __stdcall RemoveDirectoryW(const wchar_t *path);
extern BOOL __stdcall DeleteFileW(const wchar_t *path);
extern DWORD __stdcall GetTempPathW(DWORD n, wchar_t *buf);
extern DWORD __stdcall GetCurrentProcessId(void);
extern void *__stdcall GetCurrentProcess(void);
extern BOOL __stdcall CreateProcessW(const wchar_t *app, wchar_t *cmd, void *pa, void *ta, BOOL inherit, DWORD flags, void *env, const wchar_t *cwd, STARTUPINFOW *si, PROCESS_INFORMATION *pi);
extern DWORD __stdcall WaitForSingleObject(HANDLE h, DWORD ms);
extern BOOL __stdcall TerminateProcess(HANDLE h, DWORD code);
extern BOOL __stdcall GetExitCodeProcess(HANDLE h, DWORD *code);
extern BOOL __stdcall IsWow64Process(HANDLE h, BOOL *wow);
extern HANDLE __stdcall CreateThread(void *sa, size_t stack, DWORD (__stdcall *fn)(void *), void *param, DWORD flags, DWORD *tid);
extern HANDLE __stdcall OpenThread(DWORD access, BOOL inherit, DWORD tid);
extern DWORD __stdcall SuspendThread(HANDLE h);
extern DWORD __stdcall ResumeThread(HANDLE h);
extern BOOL __stdcall GetThreadContext(HANDLE h, void *ctx);
extern HANDLE __stdcall CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
extern BOOL __stdcall Thread32First(HANDLE snap, THREADENTRY32 *te);
extern BOOL __stdcall Thread32Next(HANDLE snap, THREADENTRY32 *te);
extern BOOL __stdcall Module32FirstW(HANDLE snap, MODULEENTRY32W *me);
extern BOOL __stdcall Module32NextW(HANDLE snap, MODULEENTRY32W *me);
extern void *__stdcall LoadLibraryW(const wchar_t *name);
extern void *__stdcall GetProcAddress(void *mod, const char *name);
extern BOOL __stdcall FreeLibrary(void *mod);
extern int __stdcall WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *wide, int cch, char *multi, int cb, const char *def, BOOL *used);
extern BOOL __stdcall OpenProcessToken(HANDLE proc, DWORD access, HANDLE *tok);
extern BOOL __stdcall LookupPrivilegeValueA(const char *sys, const char *name, LUID *luid);
extern BOOL __stdcall AdjustTokenPrivileges(HANDLE tok, BOOL disable, TOKEN_PRIVILEGES *neu, DWORD len, void *old, DWORD *retlen);
extern BOOL __stdcall SetEnvironmentVariableA(const char *name, const char *value);

typedef BOOL (__stdcall *SymInitialize_fn)(HANDLE, const char *, BOOL);
typedef BOOL (__stdcall *SymCleanup_fn)(HANDLE);
typedef DWORD (__stdcall *SymSetOptions_fn)(DWORD);
typedef BOOL (__stdcall *SymSetSearchPath_fn)(HANDLE, const char *);
typedef unsigned long long (__stdcall *SymLoadModuleEx_fn)(HANDLE, HANDLE, const char *, const char *, unsigned long long, DWORD, void *, DWORD);
typedef BOOL (__stdcall *SymFromAddr_fn)(HANDLE, unsigned long long, unsigned long long *, SYMBOL_INFO *);
typedef BOOL (__stdcall *SymGetLine_fn)(HANDLE, unsigned long long, DWORD *, IMAGEHLP_LINE64 *);

#define SYMOPT_UNDNAME 0x00000002u
#define SYMOPT_LOAD_LINES 0x00000010u
#define SYMOPT_FAIL_CRITICAL_ERRORS 0x00000200u
#define SYMOPT_NO_PROMPTS 0x00080000u

/* -------------------------------------------------------------------------- */
/* Estimator specification. Written verbatim to every rank file.              */
/* -------------------------------------------------------------------------- */

/* Causal rank-file header. write_rank copies these bytes ahead of the FIT
   and TABLE sections. The self-test requires the worked example to remain,
   including the text "beta_fwl = 2". The array is the specification. */
static const char ESTIMATOR_DOC[] =
"Causal isolation of pause effects\n"
"---------------------------------\n"
"This file ranks code by how much the injected pauses at that code changed\n"
"the command's end-to-end runtime. The same estimator runs on each ledger.\n"
"Only the column definitions change.\n"
"\n"
"Experiment\n"
"Each run starts the command and waits until it exits. e2e_us is the wall\n"
"time from process creation to process exit, measured with the performance\n"
"counter. During the run the profiler repeatedly lists the command's\n"
"threads, picks one thread with equal probability, suspends it, reads the\n"
"instruction pointer, holds the suspension for a duration drawn uniformly\n"
"from the configured range, and resumes it. The ledger records the measured\n"
"length of that suspension, not the draw. Sleep granularity, the context\n"
"read, and the scheduler all sit inside the measured window, and that\n"
"window is the delay the thread actually took. One run contributes one\n"
"end-to-end time and many pauses.\n"
"\n"
"What a column is\n"
"Location ledger: one column per instruction, identified by module plus\n"
"offset from the module base, so address-space layout changes do not split\n"
"one instruction into several columns.\n"
"Block ledger: pauses are summed across instructions that debug symbols\n"
"place in the same block. A block is the source line of a function when the\n"
"PDB has a line for the instruction. When the PDB names a function and has\n"
"no line, a block is a 16-byte window measured from the start of that\n"
"function. An instruction with no symbol is its own block.\n"
"Function ledger: pauses are summed across instructions whose debug symbol\n"
"is the same function in the same module. An instruction with no symbol\n"
"stays its own column, so unknown code is not merged into one bin.\n"
"\n"
"Estimand\n"
"For each column j, beta_j is the linear effect of one extra microsecond\n"
"paused at j on e2e_us, holding the pause time recorded at every other\n"
"column fixed. impact_j = beta_j times the mean pause at j per completed\n"
"run. It is the average number of microseconds those pauses added to a\n"
"completed run.\n"
"A positive beta means slowing this code slowed the whole command. A beta\n"
"near 1 means a microsecond of pause became a microsecond of end-to-end\n"
"time: the sampled code was on the critical path. A beta near 0 means the\n"
"pause was absorbed by slack. A beta above 1 means the pause was amplified,\n"
"which happens when the suspended thread was holding a resource other\n"
"threads needed.\n"
"\n"
"Why the raw correlation is the wrong ranking\n"
"Every run's end-to-end time includes every pause from that run. A column\n"
"that received pause time in runs that were also paused heavily elsewhere\n"
"tracks end-to-end time even when delaying that column did nothing. Total\n"
"pause mass is a shared cause. Columns that are visited together have\n"
"correlated pause totals, so each one can look responsible for the other's\n"
"effect.\n"
"\n"
"What identifies one column\n"
"The instruction in the program counter is wherever the program happened to\n"
"be. The duration of the suspension is drawn independently of that state.\n"
"Conditional on which places were visited, the durations are an experiment.\n"
"Two columns visited on the same runs still receive independent durations,\n"
"and the partial regression uses that independent variation.\n"
"\n"
"Stage 1. Other-pause residualization\n"
"Let X[r,j] be the total measured microseconds column j was paused in\n"
"completed run r. Let T[r] be the sum of X[r,*] over every column, including\n"
"columns that do not enter the joint model. Let O[r,j] = T[r] - X[r,j].\n"
"Ordinary least squares fits\n"
"    e2e[r] = a + beta_fwl[j] * X[r,j] + gamma[j] * O[r,j] + error\n"
"When O does not vary, gamma is omitted and the fit is e2e = a + beta_fwl * X.\n"
"There is no other pause time to hold fixed, which is the one-column ledger.\n"
"beta_fwl is the effect of column j after a linear adjustment for how much\n"
"pause time landed everywhere else, lumped into one covariate. The reported\n"
"standard error is the ordinary least squares standard error of that\n"
"coefficient, with n - 3 degrees of freedom when gamma is present and n - 2\n"
"when it was omitted. The permutation p-value\n"
"reshuffles X[*,j] across runs, keeps each run's own O and e2e paired as\n"
"observed, refits, and reports (count of absolute permuted betas at least\n"
"as large as the real one, plus 1) / (permutations plus 1).\n"
"This stage does not separate two columns from each other beyond the\n"
"information left in X[*,j] after the sum of the others is removed. Stage 2\n"
"holds the other columns fixed one by one. When the two stages disagree,\n"
"the row is marked unstable.\n"
"\n"
"Stage 2. Joint ridge, the coefficient used for the ranking\n"
"Columns that were paused in enough completed runs enter one model:\n"
"    e2e = intercept + X beta + error\n"
"At most 96 such columns are admitted. If more qualify, the 96 with the\n"
"largest total measured pause are kept. The visit minimum is 4 when at\n"
"least 8 runs completed, and 2 otherwise. If that minimum admits nobody,\n"
"it drops to 1 and the FIT line says the visit minimum was relaxed.\n"
"Completed runs are centered. Each centered column is divided by its\n"
"Euclidean length, which puts the penalty on a comparable scale. Columns\n"
"whose centered length is 0 are left out of the solve and get beta 0.\n"
"The ridge problem is\n"
"    alpha = argmin ||Yc - Z alpha||^2 + lambda ||alpha||^2\n"
"and is solved from the normal equations (Z'Z + lambda I) alpha = Z'Yc\n"
"by Gaussian elimination with partial pivoting.\n"
"beta_j = alpha_j / scale_j, which converts the coefficient back to\n"
"microseconds of end-to-end time per microsecond of pause.\n"
"Lambda is chosen by 5-fold cross-validation over the grid\n"
"1e-6, 1e-4, 1e-3, 1e-2, 0.1, 1, 10. Fold f holds out runs whose index\n"
"modulo 5 equals f. Means, scales, and coefficients for a fold are computed\n"
"on that fold's training runs only. The prediction for a held-out run is\n"
"    yhat = mean_y_train + sum_j beta_j * (X[r,j] - mean_j_train)\n"
"The lambda with the smallest sum of squared prediction errors is refit on\n"
"every completed run. Ties keep the smaller lambda. When fewer than 8 runs\n"
"completed, cross-validation is skipped and lambda is 1e-4.\n"
"Ridge is the ranking coefficient because it holds the other admitted\n"
"columns fixed individually. When two columns always receive the same pause\n"
"time, the experiment does not separate them, and the penalty splits the\n"
"shared credit. Adding impact across rows counts that shared credit more\n"
"than once. Read the table as an order. Read one row's impact as that\n"
"column's partial contribution under this penalty.\n"
"\n"
"Stage 3. Support, stability, and order\n"
"A stage-1 fit counts as identified when the design is non-singular,\n"
"the column's pause time is not constant, and the coefficient is finite.\n"
"A constant other-pause total is the reduced two-parameter fit above.\n"
"A joint-model row is stable when it is identified and\n"
"    |beta_fwl - beta_ridge| <= 0.05 + 0.25 * max(|beta_fwl|, |beta_ridge|)\n"
"Unstable means the two ways of holding the other pauses fixed do not\n"
"agree, which is what lockstep columns look like. Unstable rows stay in\n"
"the table. The stable column says so.\n"
"TABLE rows are the joint-model columns, sorted by impact descending. The\n"
"first row is the code whose injected pauses account for the largest\n"
"adjusted increase in end-to-end time. OBSERVED rows are the columns that\n"
"did not enter the joint model, sorted by total measured pause.\n"
"\n"
"Worked checks, both asserted by --self-test\n"
"Other-pause residualization: X = 1, 2, 3 and O = 0, 0, 1 and Y = 2, 4, 9.\n"
"That is Y = 2*X + 3*O. beta_fwl = 2 and gamma = 3.\n"
"Ridge, one column, lambda 0: X = 1, 2, 3 and Y = 2, 4, 6. beta = 2.\n"
"With lambda 1 the same column gives beta = 1, because after scaling to\n"
"unit length the closed form is 2 / (1 + lambda).\n"
"\n"
"Limits\n"
"The experiment slowed code. A large positive impact is evidence that the\n"
"slowdown reached end-to-end time. That is the critical-path observation\n"
"this design can make. It is a measurement of slowdown pass-through.\n"
"Rewriting the code to be faster can move the critical path onto something\n"
"else; this experiment does not virtualize a speedup.\n"
"The program counter is read after the thread has been suspended. The\n"
"kernel delivers that suspend at its next safe point. The charged\n"
"instruction is the one in the register at that point. Inlined callees are\n"
"charged to the instruction that was actually there, and the function grain\n"
"then follows the debug symbol for that instruction.\n"
"Pauses change the schedule they measure. The betas are average effects\n"
"inside the random pause design that was actually run. Timed-out runs are\n"
"stored in the ledger and left out of the fit. A run that exits with a\n"
"non-zero status is still a completed run: its end-to-end time is the time\n"
"until that exit.\n";

/* -------------------------------------------------------------------------- */
/* Arena, strings, RNG, formatting                                            */
/* -------------------------------------------------------------------------- */

/* One VirtualAlloc reservation used as a bump allocator. profile_new asks
   for a single region large enough for the sample, location, module, and
   group tables. Every later allocation is a slice of that region, and
   slices are never freed one at a time. arena_free releases the whole
   reservation and zeros this header. A failed reservation leaves base NULL
   and cap 0, and later allocations from it fail. */
typedef struct Arena {
    /* Start of the reserved region. NULL when VirtualAlloc failed or after arena_free. */
    char *base;
    /* Byte capacity of the reservation. Zero when there is no region. */
    size_t cap;
    /* Offset of the next free byte. arena_alloc rounds this up to the requested alignment before handing out a slice. */
    size_t used;
} Arena;

/* State of one splitmix64 generator. Each draw adds a fixed odd constant
   to s and mixes the result, so the same seed always produces the same
   sequence. The experiment uses one generator for pause and gap draws.
   The self-test uses another for the estimator fuzz. */
typedef struct Rng {
    /* Current state. Advanced by every rng_u64, including the draws that rng_u32, rng_range, and rng_unit make. */
    unsigned long long s;
} Rng;

/* Buffered byte writer for one ledger or rank file. Bytes sit in buf
   until a flush, a close, or a write that does not fit. The first failed
   WriteFile, or an open against a NULL or invalid handle, sets err, and
   later writes become no-ops. writer_s turns each '\n' into "\r\n" before
   buffering. writer_close flushes, closes the handle, and reports err. */
typedef struct Writer {
    /* File handle from CreateFileW. INVALID_HANDLE_VALUE marks an open that already failed. */
    HANDLE h;
    /* Caller-owned buffer. The output writers share the static g_io array. */
    char *buf;
    /* Number of bytes currently sitting in buf and not yet written. */
    int n;
    /* Capacity of buf, in bytes. A write larger than the free space flushes first. */
    int cap;
    /* Sticky failure flag. Stays 1 after the first failed write or a bad open. */
    int err;
} Writer;

/* Ticks per second from QueryPerformanceFrequency. qpc_init stores it and
   replaces a failed or zero query with 1. qpc_to_us, the sleeps, the run
   timeout, and the burn calibration all scale counter deltas by this. */
static long long g_qpc_freq;
/* Stand-in for a 64-bit CONTEXT. Sixteen-byte aligned, 1232 bytes.
   read_rip and the context self-test write ContextFlags at offset 0x30
   and read Rip at offset 0xF8. One call uses it at a time. */
static __declspec(align(16)) unsigned char g_context[CONTEXT_BYTES];
/* Scratch for format_i64, format_fixed, and format_hex. Eighty bytes holds
   a 64-bit decimal or hex value. Callers share it, so a value still being
   appended must be copied out before the next format. */
static char g_fmt[80];
/* Byte buffer attached to every Writer. 8192 bytes. One writer is open
   at a time; a second writer would reuse these same bytes. */
static char g_io[8192];

/* Read the performance counter. The value is a tick count, not microseconds.
   qpc_init must have run before any conversion of a difference to time. */
static long long qpc_now(void) {
    LARGE_INTEGER c;
    c.QuadPart = 0;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

/* Store the counter frequency in ticks per second. A failed or zero query
   is forced to 1 so later divisions stay defined. Call this once at startup. */
static void qpc_init(void) {
    LARGE_INTEGER f;
    f.QuadPart = 0;
    QueryPerformanceFrequency(&f);
    g_qpc_freq = f.QuadPart;
    /* Keep a later division from using a zero frequency. */
    if (g_qpc_freq < 1) g_qpc_freq = 1;
}

/* Convert a tick difference into microseconds. Negative differences become
   zero. The split into seconds and a remainder keeps the multiply inside
   64 bits for the durations this program measures. */
static long long qpc_to_us(long long delta) {
    long long sec, rem;
    /* A backward clock reading is reported as no elapsed time. */
    if (delta < 0) delta = 0;
    sec = delta / g_qpc_freq;
    rem = delta % g_qpc_freq;
    return sec * 1000000LL + (rem * 1000000LL) / g_qpc_freq;
}

/* Reserve and commit one read-write region of cap bytes. The arena then
   hands out slices of that region and never frees them individually.
   Returns 1 when the region exists. */
static int arena_init(Arena *a, size_t cap) {
    a->base = (char *)VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    a->cap = a->base ? cap : 0;
    a->used = 0;
    return a->base != NULL;
}

/* Take n bytes from the arena, aligned up to align. align must be a power
   of two. Returns NULL when the request does not fit in what is left.
   The caller owns the bytes until arena_free releases the whole region. */
static void *arena_alloc(Arena *a, size_t n, size_t align) {
    size_t u;
    /* A zero or negative alignment would make the mask wrong. */
    if (align < 1) align = 1;
    /* Round the bump pointer up to the next multiple of align. */
    u = (a->used + (align - 1)) & ~(align - 1);
    /* Reject a request that would walk off the end, including overflow. */
    if (n > a->cap || u > a->cap - n) return NULL;
    a->used = u + n;
    return a->base + u;
}

/* arena_alloc followed by a zero fill. Returns NULL when the allocation
   does not fit. The zeroed block is the usual form for profile tables. */
static void *arena_zalloc(Arena *a, size_t n, size_t align) {
    void *p = arena_alloc(a, n, align);
    /* Only clear a block that was actually reserved. */
    if (p) memset(p, 0, n);
    return p;
}

/* Count bytes before the terminating NUL. A NULL pointer counts as empty. */
static int str_len(const char *s) {
    int n = 0;
    /* Treat a missing string as zero length rather than reading it. */
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* Copy src into dst, always leaving a NUL inside cap. A NULL src copies as
   an empty string. Characters that do not fit are dropped. */
static void str_copy(char *dst, int cap, const char *src) {
    int i = 0;
    /* No room even for the terminator. */
    if (cap < 1) return;
    /* A missing source is the empty string. */
    if (!src) src = "";
    /* Leave one byte for the terminator. */
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* Return 1 when a and b are the same sequence of bytes, including both
   empty. A NULL on either side is not equal to anything. */
static int str_eq(const char *a, const char *b) {
    /* A missing pointer cannot match. */
    if (!a || !b) return 0;
    /* Advance while the current bytes agree and the first string continues. */
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* Return 1 when needle occurs as a contiguous substring of hay. An empty
   needle is treated as present. Either NULL pointer is treated as absent. */
static int str_contains(const char *hay, const char *needle) {
    int n, h, i, j;
    /* A missing side cannot contain or be contained. */
    if (!hay || !needle) return 0;
    n = str_len(needle);
    h = str_len(hay);
    /* The empty needle is a substring of every string. */
    if (n == 0) return 1;
    /* Try each alignment where the needle still fits. */
    for (i = 0; i + n <= h; i++) {
        /* Compare this alignment and stop at the first mismatch. */
        for (j = 0; j < n; j++) if (hay[i + j] != needle[j]) break;
        /* The inner loop finished, so this alignment matched. */
        if (j == n) return 1;
    }
    return 0;
}

/* Append src to the NUL-terminated string already in dst, stopping before
   cap. A NULL src leaves dst unchanged. dst must already be terminated. */
static void str_append(char *dst, int cap, const char *src) {
    int n = str_len(dst);
    int i = 0;
    /* Nothing to add. */
    if (!src) return;
    /* Write until src ends or one byte remains for the new terminator. */
    while (src[i] && n + 1 < cap) dst[n++] = src[i++];
    dst[n] = 0;
}

/* Count wchar_t units before the terminating wide NUL. NULL is empty. */
static int w_len(const wchar_t *s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* Copy a wide string into dst with a wide NUL inside cap. A NULL src
   becomes empty. Units that do not fit are dropped. */
static void w_copy(wchar_t *dst, int cap, const wchar_t *src) {
    int i = 0;
    /* No room for a terminator. */
    if (cap < 1) return;
    if (!src) src = L"";
    /* Keep the last slot for the wide NUL. */
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* Return 1 when two wide strings are identical. NULL does not match. */
static int w_eq(const wchar_t *a, const wchar_t *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* Case-insensitive wide compare for A-Z only. Other characters compare
   as themselves. NULL does not match. Used for module path identity. */
static int w_eq_i(const wchar_t *a, const wchar_t *b) {
    if (!a || !b) return 0;
    /* Fold ASCII capitals on both sides and stop at the first difference. */
    while (*a && *b) {
        wchar_t ca = *a, cb = *b;
        /* Map A-Z onto a-z. Other code points stay as they are. */
        if (ca >= L'A' && ca <= L'Z') ca = (wchar_t)(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = (wchar_t)(cb - L'A' + L'a');
        /* This pair differs, so the strings differ. */
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/* Compare a wide string with a byte string of ASCII characters. Each wide
   unit must equal the unsigned byte. Useful for option names. NULL fails. */
static int w_eq_ascii(const wchar_t *w, const char *a) {
    int i = 0;
    if (!w || !a) return 0;
    /* Compare until either side ends. */
    while (w[i] && a[i]) {
        /* A wide unit above 255 cannot equal a single byte. */
        if (w[i] != (wchar_t)(unsigned char)a[i]) return 0;
        i++;
    }
    return w[i] == 0 && a[i] == 0;
}

/* Append an ASCII byte string onto a wide string, one byte per wide unit.
   Stops before cap. A NULL src does nothing. dst must already be terminated. */
static void w_append_ascii(wchar_t *dst, int cap, const char *src) {
    int n = w_len(dst);
    int i = 0;
    if (!src) return;
    /* Widen each byte and keep a slot for the terminator. */
    while (src[i] && n + 1 < cap) dst[n++] = (wchar_t)(unsigned char)src[i++];
    dst[n] = 0;
}

/* Convert a wide string to the process ANSI code page, NUL included.
   Returns 1 on success. On failure dst is emptied and the return is 0.
   cap is the byte capacity of dst. */
static int wide_to_acp(const wchar_t *w, char *dst, int cap) {
    int n;
    /* Nowhere to write the result. */
    if (cap < 1) return 0;
    dst[0] = 0;
    /* A missing wide string is a failed conversion, not an empty success. */
    if (!w) return 0;
    n = WideCharToMultiByte(CP_ACP, 0, w, -1, dst, cap, NULL, NULL);
    /* The API returns 0 when the buffer is too small or the input is bad. */
    if (n <= 0) {
        dst[0] = 0;
        return 0;
    }
    return 1;
}

/* Draw one 64-bit value from a splitmix64 generator and advance r->s.
   Every state has a defined next value. Pause draws, thread picks, and
   the estimator's permutation shuffles all come from this stream. */
static unsigned long long rng_u64(Rng *r) {
    unsigned long long z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Low 32 bits of one rng_u64 draw. The upper bits are discarded. */
static unsigned rng_u32(Rng *r) {
    return (unsigned)rng_u64(r);
}

/* Inclusive integer in [lo, hi]. A reversed or empty range returns lo, so
   the modulus is never zero. Spans that do not divide 2^32 are slightly
   biased; the spans used for pauses are small next to 2^32. */
static int rng_range(Rng *r, int lo, int hi) {
    unsigned span;
    /* Empty or reversed interval: the only legal value is the low end. */
    if (hi <= lo) return lo;
    span = (unsigned)(hi - lo) + 1u;
    return lo + (int)(rng_u32(r) % span);
}

/* Uniform double in [0, 1). Uses the top 53 bits so the result matches the
   precision of a double mantissa. */
static double rng_unit(Rng *r) {
    /* Drop 11 bits, then scale by 1/2^53. */
    return (double)(rng_u64(r) >> 11) * (1.0 / 9007199254740992.0);
}

/* Return 1 when v is a finite IEEE-754 double, and 0 for NaN or either
   infinity. The test reads the 11-bit exponent and ignores the sign, so
   both zeros pass and both infinities fail. v is not modified. This does
   not use libm and does not distinguish a quiet NaN from a signaling one. */
static int is_finite(double v) {
    /* Two views of the same 8 bytes. Assigning d and reading u reinterprets
       the IEEE-754 encoding so the exponent can be tested without libm. */
    union {
        /* Numeric view. Holds the value whose finiteness is in question. */
        double d;
        /* Integer view of those same bits. The exponent is bits 52 through 62. */
        unsigned long long u;
    } u;
    u.d = v;
    return ((u.u >> 52) & 0x7ffULL) != 0x7ffULL;
}

/* Absolute value of v. A negative finite value is negated; NaN compares
   false and is returned unchanged. There is no finiteness check and no
   libm call. The result has the same magnitude as v. */
static double dabs(double v) {
    return v < 0 ? -v : v;
}

/* Square root by a bit-level guess and Newton iteration. No libm.
   Zero, a negative, a NaN, or an infinity returns 0, so the caller can
   treat the result as a length. The guess is the exponent bits halved
   and biased, then six Newton steps refine it. A non-positive guess is
   replaced by v before dividing. The result is not rounded to a correctly
   rounded libm square root. */
static double sqrt_local(double v) {
    /* Two views of the radicand. d is loaded with v, u is rewritten as a
       halved-exponent guess, and d is read back as the Newton start. */
    union {
        /* Numeric view. Written with v, then read after the bit-level guess. */
        double d;
        /* Integer view. Shifted and rebiased to form the initial guess. */
        unsigned long long u;
    } g;
    double x;
    int i;
    /* No real square root is defined here for a non-positive or non-finite input. */
    if (!(v > 0) || !is_finite(v)) return 0;
    g.d = v;
    g.u = (g.u >> 1) + 0x1FF8000000000000ULL;
    x = g.d;
    /* A non-positive guess cannot be the divisor in the Newton step. */
    if (!(x > 0)) x = v;
    /* Six steps are enough for the magnitudes this program squares and roots. */
    for (i = 0; i < 6; i++) x = 0.5 * (x + v / x);
    return x;
}

/* Write v as a signed decimal into dst and always store a NUL when cap
   is at least 1. cap is the byte capacity. A cap below 2 produces an
   empty string when cap is 1 and writes nothing when cap is 0. The sign
   is emitted only for a negative value. The most negative long long is
   negated via -(v+1)+1 so the conversion does not overflow. Digits that
   do not fit are dropped from the low end. Does not use sprintf. */
static void format_i64(char *dst, int cap, long long v) {
    char tmp[32];
    int n = 0, i = 0, neg = 0;
    unsigned long long u;
    /* Not enough room for a digit and a terminator. */
    if (cap < 2) {
        /* A one-byte buffer can still hold the empty string. */
        if (cap == 1) dst[0] = 0;
        return;
    }
    /* Negate without overflowing the most negative long long. */
    if (v < 0) {
        neg = 1;
        u = (unsigned long long)(-(v + 1)) + 1ULL;
    } else /* A non-negative value is already an unsigned magnitude. */ {
        u = (unsigned long long)v;
    }
    /* Zero has no digits from the division loop, so store one explicitly. */
    if (u == 0) tmp[n++] = '0';
    /* Collect digits least-significant first. */
    while (u) {
        tmp[n++] = (char)('0' + (u % 10ULL));
        u /= 10ULL;
    }
    /* The sign occupies the first output byte when the value is negative. */
    if (neg) dst[i++] = '-';
    /* Copy most-significant digits first, leaving one byte for the NUL. */
    while (n > 0 && i + 1 < cap) dst[i++] = tmp[--n];
    dst[i] = 0;
}

/* Write v in fixed point with dec digits after the decimal point.
   cap is the byte capacity of dst. A cap below 4 empties dst when cap
   is positive and writes nothing when cap is 0. A non-finite v becomes
   "na". Rounding is half away from zero on the absolute value, by adding
   0.5 after scaling by 10^dec. A scaled magnitude that is not below 1e18
   becomes "huge" or "-huge". A negative value whose rounded digits are
   all zero is written without a sign. dec is not clamped. Digits that
   do not fit in dst are dropped. Does not use sprintf. */
static void format_fixed(char *dst, int cap, double v, int dec) {
    char tmp[64];
    int neg, i, k, nd;
    double scale, scaled, a;
    unsigned long long iv, ip, fp, div;
    /* "na" and "huge" need a few bytes, so a smaller cap only clears dst. */
    if (cap < 4) {
        /* Leave a terminator when there is any room at all. */
        if (cap > 0) dst[0] = 0;
        return;
    }
    /* NaN and infinities are reported as not available. */
    if (!is_finite(v)) {
        str_copy(dst, cap, "na");
        return;
    }
    neg = v < 0;
    a = neg ? -v : v;
    scale = 1;
    /* Build 10^dec. A negative dec leaves the scale at 1. */
    for (k = 0; k < dec; k++) scale *= 10.0;
    scaled = a * scale + 0.5;
    /* The rounded integer would not fit in the unsigned conversion. */
    if (!(scaled < 1e18)) {
        str_copy(dst, cap, neg ? "-huge" : "huge");
        return;
    }
    iv = (unsigned long long)scaled;
    div = 1;
    /* The divisor splits the integer part from the fractional digits. */
    for (k = 0; k < dec; k++) div *= 10ULL;
    ip = (dec > 0) ? iv / div : iv;
    fp = (dec > 0) ? iv - ip * div : 0;
    /* A negative zero, after rounding, stays unsigned. */
    if (neg && (ip || fp)) {
        tmp[0] = '-';
        nd = 1;
    /* No sign byte, so the digits start at the front of the scratch buffer. */
    } else nd = 0;
    /* Convert the integer part, least-significant digit first, then reverse it. */
    {
        char digs[32];
        int dn = 0;
        unsigned long long t = ip;
        /* Keep a zero integer part visible. */
        if (t == 0) digs[dn++] = '0';
        /* Peel off decimal digits of the integer part. */
        while (t) {
            digs[dn++] = (char)('0' + (t % 10ULL));
            t /= 10ULL;
        }
        /* Copy them back most-significant first, stopping before tmp fills. */
        while (dn > 0 && nd + 1 < (int)sizeof(tmp)) tmp[nd++] = digs[--dn];
    }
    /* Append the point and exactly dec fractional digits, high place first. */
    if (dec > 0 && nd + 1 < (int)sizeof(tmp)) {
        tmp[nd++] = '.';
        /* Each place is 10^k. Stop if the scratch buffer has no room left. */
        for (k = dec - 1; k >= 0 && nd + 1 < (int)sizeof(tmp); k--) {
            unsigned long long place = 1;
            int p;
            /* place is 10^k for this fractional digit. */
            for (p = 0; p < k; p++) place *= 10ULL;
            tmp[nd++] = (char)('0' + (fp / place) % 10ULL);
        }
    }
    tmp[nd] = 0;
    /* Copy into dst, leaving one byte for the terminator. */
    for (i = 0; tmp[i] && i + 1 < cap; i++) dst[i] = tmp[i];
    dst[i] = 0;
}

/* Write v as lowercase hex with a 0x prefix. Zero is "0x0". cap is the
   byte capacity of dst. A cap below 4 empties dst when cap is positive
   and writes nothing when cap is 0. Leading zeros are not emitted.
   Digits that do not fit are dropped from the low end, and a NUL is
   stored when cap is at least 4. Does not use sprintf. */
static void format_hex(char *dst, int cap, unsigned long long v) {
    char tmp[20];
    int n = 0, i = 0;
    /* The prefix alone needs three bytes plus a terminator. */
    if (cap < 4) {
        /* Still terminate a buffer that is too small to hold a value. */
        if (cap > 0) dst[0] = 0;
        return;
    }
    dst[i++] = '0';
    dst[i++] = 'x';
    /* Zero produces no digits from the shift loop. */
    if (v == 0) tmp[n++] = '0';
    /* Collect nibbles least-significant first. */
    while (v) {
        unsigned d = (unsigned)(v & 15ULL);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v >>= 4;
    }
    /* Copy most-significant nibbles first, leaving room for the NUL. */
    while (n > 0 && i + 1 < cap) dst[i++] = tmp[--n];
    dst[i] = 0;
}

/* Push w's buffered bytes to its handle. A NULL writer, a writer that
   has already failed, or an empty buffer returns without a write. A
   failed or short WriteFile sets w->err. The count is cleared either
   way, so bytes that did not land are dropped. Does not close the handle. */
static void writer_flush(Writer *w) {
    DWORD wrote = 0;
    /* Nothing queued, or this writer has already failed. */
    if (!w || w->err || w->n <= 0) return;
    /* A short write counts as failure even when the call returns true. */
    if (!WriteFile(w->h, w->buf, (DWORD)w->n, &wrote, NULL) || wrote != (DWORD)w->n) w->err = 1;
    w->n = 0;
}

/* Append one byte to w, flushing first when the next byte would fill
   the buffer. A NULL writer or an existing error returns immediately,
   and a failed flush drops this byte too. One slot stays unused so n
   remains strictly below cap. Does not flush a partial buffer otherwise. */
static void writer_ch(Writer *w, char c) {
    /* Ignore a missing writer and any writer that has already failed. */
    if (!w || w->err) return;
    /* Make room before storing the byte. */
    if (w->n + 1 >= w->cap) writer_flush(w);
    /* The flush failed, so the byte is discarded. */
    if (w->err) return;
    w->buf[w->n++] = c;
}

/* Write each byte of s. A newline is preceded by a carriage return so
   the file uses CRLF. A NULL s does nothing. Once w->err is set, later
   writer_ch calls no-op, but this loop still walks the rest of s.
   Does not add a trailing newline of its own. */
static void writer_s(Writer *w, const char *s) {
    /* A missing string is not an error. */
    if (!s) return;
    /* Translate each LF into CRLF as the bytes are copied. */
    while (*s) {
        /* A bare LF becomes CR LF. */
        if (*s == '\n') writer_ch(w, '\r');
        writer_ch(w, *s);
        s++;
    }
}

/* Attach w to handle h and the shared g_io buffer, with an empty count.
   err is set when h is NULL or INVALID_HANDLE_VALUE, so later writes
   no-op. Does not create or close the handle. Every writer uses g_io,
   so only one writer may hold unflushed bytes at a time. */
static void writer_open(Writer *w, HANDLE h) {
    w->h = h;
    w->buf = g_io;
    w->n = 0;
    w->cap = (int)sizeof(g_io);
    w->err = (h == INVALID_HANDLE_VALUE || h == NULL);
}

/* Flush w, close a real handle, and clear w->h. Returns 1 when no write
   error was recorded and 0 when one was. A NULL or invalid handle is not
   passed to CloseHandle. The shared buffer is left as it is. */
static int writer_close(Writer *w) {
    writer_flush(w);
    /* Only a live handle is closed. */
    if (w->h && w->h != INVALID_HANDLE_VALUE) CloseHandle(w->h);
    w->h = NULL;
    return w->err ? 0 : 1;
}

/* Write s to the standard handle std, inserting CR before each LF.
   A NULL s or a missing handle returns without writing. The text is
   copied through a 1200-byte stack buffer in chunks. WriteFile errors
   are ignored. Does not use the shared ledger Writer buffer. */
static void con_write(DWORD std, const char *s) {
    HANDLE h = GetStdHandle(std);
    DWORD wrote = 0;
    int n;
    char tmp[1200];
    int i = 0, j = 0;
    /* No text, or the standard handle is not available. */
    if (!s || h == INVALID_HANDLE_VALUE || h == NULL) return;
    n = str_len(s);
    /* Send the text in chunks that leave room for an inserted CR. */
    while (i < n) {
        j = 0;
        /* Fill one chunk, expanding LF to CRLF. */
        while (i < n && j + 2 < (int)sizeof(tmp)) {
            /* Keep a CR with the LF that follows it. */
            if (s[i] == '\n') tmp[j++] = '\r';
            tmp[j++] = s[i++];
        }
        WriteFile(h, tmp, (DWORD)j, &wrote, NULL);
    }
}

/* Write s to standard error. s may be NULL, which con_write ignores.
   No newline is added. Does not touch standard output. */
static void con_err(const char *s) { con_write(STD_ERROR_HANDLE, s); }

/* Write s to standard output. s may be NULL, which con_write ignores.
   No newline is added. Does not touch standard error. */
static void con_out(const char *s) { con_write(STD_OUTPUT_HANDLE, s); }

/* Format v as a signed decimal into the shared g_fmt buffer and write
   that text to standard error. g_fmt is 80 bytes, which holds any
   64-bit decimal. A later format into g_fmt overwrites this text.
   Does not append a newline. */
static void con_err_i64(long long v) {
    format_i64(g_fmt, (int)sizeof(g_fmt), v);
    con_err(g_fmt);
}

/* -------------------------------------------------------------------------- */
/* Linear algebra                                                             */
/* Stage 2 solves (Z'Z + lambda I) alpha = Z'Yc by elimination.              */
/* The workspace is static. Analysis runs on the sampler thread only.        */
/* -------------------------------------------------------------------------- */

/* Packed training design for one ridge solve. Each kept row is centered
   and divided by that column's scale. Row-major, at most CX_MAX_RUNS by
   CX_MAX_JOINT. Every ridge_fit_mask call, including each cross-validation
   fold, overwrites it. */
static double g_Z[CX_MAX_RUNS * CX_MAX_JOINT];
/* Gram matrix of g_Z with the ridge penalty added to the diagonal.
   solve_ge overwrites it during elimination. Same lifetime as g_Z. */
static double g_G[CX_MAX_JOINT * CX_MAX_JOINT];
/* g_Z transposed times the centered outcomes. solve_ge uses this as its
   working right-hand side and overwrites it. */
static double g_rhs[CX_MAX_JOINT];
/* Scaled ridge coefficients returned by solve_ge. Each is divided by the
   matching g_ridge_scale entry to produce a beta in original units. */
static double g_alpha[CX_MAX_JOINT];
/* Pause column of one stage-1 regression. fit_columns copies one admitted
   column here, one entry per completed run, and shuffles these entries
   for the permutation p-value. */
static double g_px[CX_MAX_RUNS];
/* Other-pause total paired with g_px. Each entry is that run's pause
   across every column minus the g_px entry. It stays with the run while
   g_px is shuffled. */
static double g_po[CX_MAX_RUNS];
/* End-to-end microseconds paired with g_px and g_po. One entry per
   completed run in the fit. */
static double g_py[CX_MAX_RUNS];

/* Solve a*x = b for an n-by-n row-major matrix by Gaussian elimination
   with partial pivoting. Returns 1 and writes x on success. Returns 0
   when n is not positive, every entry is zero or non-finite, a pivot is
   at most scale*1e-12, a back-substitution diagonal is zero, or an x
   entry is not finite. a and b are overwritten. A row swap copies only
   columns k through n-1 plus the matching b entry. Does not allocate.
   x entries past a failure are left untouched. */
static int solve_ge(double *a, double *b, int n, double *x) {
    int k, i, j, piv;
    double scale = 0, best, v, diag, f, s;
    /* An empty system has no solution vector to write. */
    if (n <= 0) return 0;
    /* Scale is the largest absolute entry, used to judge a tiny pivot. */
    for (i = 0; i < n * n; i++) {
        v = a[i];
        /* Compare magnitudes so a negative entry can set the scale. */
        if (v < 0) v = -v;
        /* Keep the biggest magnitude seen so far. */
        if (v > scale) scale = v;
    }
    /* An all-zero matrix, or a NaN scale, cannot be factored. */
    if (!(scale > 0)) return 0;
    /* Eliminate column k, pivoting on the largest remaining magnitude. */
    for (k = 0; k < n; k++) {
        piv = k;
        best = a[k * n + k];
        /* The diagonal candidate is compared by magnitude. */
        if (best < 0) best = -best;
        /* Search rows below k for a larger pivot in this column. */
        for (i = k + 1; i < n; i++) {
            v = a[i * n + k];
            /* Magnitude of this candidate pivot. */
            if (v < 0) v = -v;
            /* Remember the row with the largest magnitude. */
            if (v > best) {
                best = v;
                piv = i;
            }
        }
        /* A pivot this small relative to the matrix is treated as singular. */
        if (!(best > scale * 1e-12)) return 0;
        /* Move the chosen pivot onto the diagonal when it is not already there. */
        if (piv != k) {
            /* Swap the uneliminated part of the two rows. */
            for (j = k; j < n; j++) {
                double tmp = a[k * n + j];
                a[k * n + j] = a[piv * n + j];
                a[piv * n + j] = tmp;
            }
            /* Swap the matching right-hand side entries. */
            {
                double tb = b[k];
                b[k] = b[piv];
                b[piv] = tb;
            }
        }
        diag = a[k * n + k];
        /* Subtract a multiple of row k from each row below it. */
        for (i = k + 1; i < n; i++) {
            f = a[i * n + k] / diag;
            a[i * n + k] = 0;
            /* Update the remaining entries of this row. */
            for (j = k + 1; j < n; j++) a[i * n + j] -= f * a[k * n + j];
            b[i] -= f * b[k];
        }
    }
    /* Back-substitute from the last unknown up to the first. */
    for (i = n - 1; i >= 0; i--) {
        s = b[i];
        /* Remove the already-solved unknowns from this row. */
        for (j = i + 1; j < n; j++) s -= a[i * n + j] * x[j];
        /* A zero diagonal here means the elimination missed a singularity. */
        if (a[i * n + i] == 0) return 0;
        x[i] = s / a[i * n + i];
        /* A NaN or infinity is not a usable coefficient. */
        if (!is_finite(x[i])) return 0;
    }
    return 1;
}

/* Stage 1. Fit e2e = a + b * x + g * o. x, o, and y each have n rows,
   in microseconds. b is beta_fwl, written to b_out, and se_out is its
   ordinary least squares standard error. Returns 0 when the design is
   singular, n is below 2, any input is non-finite, x does not vary,
   the three-parameter fit has fewer than 3 rows, or |b| exceeds 1e6.
   A constant o is dropped: the fit becomes e2e = a + b * x and gamma
   is 0, because there is no other pause time to hold fixed. On a
   successful coefficient fit that cannot produce a standard error,
   se_out is -1 (or 0 when the residual is tiny and there are no
   degrees of freedom) and the return is still 1. The worked check in
   ESTIMATOR_DOC (X=1,2,3 O=0,0,1 Y=2,4,9) yields a=0, b=2, g=3 and is
   asserted by the self-test. Does not allocate. solve_ge overwrites G. */
static int ols_two(const double *x, const double *o, const double *y, int n,
                   double *a_out, double *b_out, double *g_out, double *se_out) {
    double sx = 0, so = 0, sy = 0, sxx = 0, soo = 0, sxo = 0, sxy = 0, soy = 0;
    double G[9], rhs[3], coef[3], sse, sigma2, vf;
    int i, r, dof, pdim;
    double xmin, xmax, omin, omax;
    /* Two points are the smallest line this fit will attempt. */
    if (n < 2) return 0;
    xmin = xmax = x[0];
    omin = omax = o[0];
    /* Accumulate the normal-equation sums and the ranges of x and o. */
    for (r = 0; r < n; r++) {
        double xv = x[r], ov = o[r], yv = y[r];
        /* One bad observation rejects the whole fit. */
        if (!is_finite(xv) || !is_finite(ov) || !is_finite(yv)) return 0;
        /* Track the range of the pause column. */
        if (xv < xmin) xmin = xv;
        /* Track the high end of the pause column. */
        if (xv > xmax) xmax = xv;
        /* Track the range of the other-pause column. */
        if (ov < omin) omin = ov;
        /* Track the high end of the other-pause column. */
        if (ov > omax) omax = ov;
        sx += xv; so += ov; sy += yv;
        sxx += xv * xv; soo += ov * ov; sxo += xv * ov;
        sxy += xv * yv; soy += ov * yv;
    }
    /* A constant pause column does not identify a slope. */
    if (!(xmax - xmin > 1e-12)) return 0;
    pdim = (omax - omin > 1e-12) ? 3 : 2;
    /* The three-parameter design needs a third row. */
    if (pdim == 3 && n < 3) return 0;
    /* Two columns: intercept and the pause itself. */
    if (pdim == 2) {
        G[0] = (double)n; G[1] = sx;
        G[2] = sx;        G[3] = sxx;
        rhs[0] = sy; rhs[1] = sxy;
    } else /* Three columns: intercept, pause, and other-pause. */ {
        G[0] = (double)n; G[1] = sx;  G[2] = so;
        G[3] = sx;        G[4] = sxx; G[5] = sxo;
        G[6] = so;        G[7] = sxo; G[8] = soo;
        rhs[0] = sy; rhs[1] = sxy; rhs[2] = soy;
    }
    /* A singular normal matrix is a failed stage-1 fit. */
    if (!solve_ge(G, rhs, pdim, coef)) return 0;
    /* The dropped other-pause coefficient is reported as zero. */
    if (pdim == 2) coef[2] = 0;
    /* Refuse a non-finite intercept, slope, or gamma. */
    if (!is_finite(coef[0]) || !is_finite(coef[1]) || !is_finite(coef[2])) return 0;
    /* An enormous slope is treated as unidentified. */
    if (dabs(coef[1]) > 1e6) return 0;
    *a_out = coef[0];
    *b_out = coef[1];
    *g_out = coef[2];
    sse = 0;
    /* Residual sum of squares of the fitted line. */
    for (r = 0; r < n; r++) {
        double pred = coef[0] + coef[1] * x[r] + coef[2] * o[r];
        double e = y[r] - pred;
        sse += e * e;
    }
    dof = n - pdim;
    /* No degrees of freedom left for a variance estimate. */
    if (dof <= 0) {
        *se_out = (sse < 1e-6) ? 0 : -1;
        return 1;
    }
    /* Rebuild G; solve_ge destroyed the first copy. Invert via unit solves. */
    if (pdim == 2) {
        G[0] = (double)n; G[1] = sx;
        G[2] = sx;        G[3] = sxx;
    } else /* Restore the three-parameter Gram matrix the same way. */ {
        G[0] = (double)n; G[1] = sx;  G[2] = so;
        G[3] = sx;        G[4] = sxx; G[5] = sxo;
        G[6] = so;        G[7] = sxo; G[8] = soo;
    }
    vf = -1;
    /* Each unit solve is one column of the inverse of G. */
    for (i = 0; i < pdim; i++) {
        double Gi[9], ei[3], col[3];
        int k;
        /* Copy G because solve_ge overwrites its matrix. */
        for (k = 0; k < pdim * pdim; k++) Gi[k] = G[k];
        ei[0] = ei[1] = ei[2] = 0;
        ei[i] = 1;
        /* A failed inverse column still returns the coefficients. */
        if (!solve_ge(Gi, ei, pdim, col)) {
            *se_out = -1;
            return 1;
        }
        /* Column 1 is the slope. Its diagonal inverse entry is the variance factor. */
        if (i == 1) vf = col[1];
    }
    sigma2 = sse / (double)dof;
    /* A non-positive or non-finite variance factor is not a standard error. */
    if (!(vf > 0) || !is_finite(vf) || !is_finite(sigma2)) {
        *se_out = -1;
        return 1;
    }
    *se_out = sqrt_local(sigma2 * vf);
    return 1;
}

/* Result of one centered ridge solve. Training rows are centered, each
   column is scaled to unit length, and the penalty is applied in that
   scaled space. beta converts the scaled coefficients back to original
   units: extra end-to-end microseconds per extra pause microsecond.
   The fitter keeps this in the static g_ridge_model. A stack copy inside
   cross-validation would cross the guard page, and this build has no
   chkstk. A rejected solve leaves ok at 0. */
typedef struct RidgeModel {
    /* Mean of the training outcomes, in end-to-end microseconds. */
    double mean_y;
    /* Mean of each training column, in pause microseconds. Entries at index p and above are unused. */
    double mean_x[CX_MAX_JOINT];
    /* Slope of each column in original units: the scaled coefficient divided by that column's scale. A zero-scale column stays 0. */
    double beta[CX_MAX_JOINT];
    /* In-sample fraction of centered outcome variance explained by this fit. Zero when the outcome does not vary. */
    double r2;
    /* Number of columns stored in mean_x and beta. At most CX_MAX_JOINT. */
    int p;
    /* 1 when the normal equations solved. 0 when the fit was rejected. */
    int ok;
} RidgeModel;

/* The one ridge result. About 1.5 KB. Cross-validation calls the fitter
   again for each fold, and a stack copy in that chain would step over
   the guard page: this build has no chkstk. Each fit overwrites it. */
static RidgeModel g_ridge_model;
/* Joint-column indexes whose centered training length is positive, packed
   to the front. Column a of g_Z is joint column g_ridge_map[a]. */
static int g_ridge_map[CX_MAX_JOINT];
/* Euclidean length of each centered joint column on the training rows.
   A zero marks a constant column: its beta stays 0 and it is left out
   of the solve. */
static double g_ridge_scale[CX_MAX_JOINT];

/* Stage 2 on one training mask. train == NULL means every row. x has
   `stride` columns; cols[0..p) selects the joint columns. y is end-to-end
   time. lambda is the ridge penalty on the scaled coefficients. beta is
   in original units: e2e microseconds per pause microsecond. Returns 0
   when n or p is outside the static workspace, a used y or x entry is
   non-finite, fewer than two training rows exist, or the normal equations
   do not solve. A column whose centered length is 0 stays at beta 0 and
   is left out of the solve. If every column is constant, the fit still
   returns 1 with r2 0. Means and scales use training rows only. The
   workspace g_Z, g_G, g_rhs, g_alpha, g_ridge_map, and g_ridge_scale is
   static and overwritten. The one-column closed form beta = 2/(1+lambda)
   for X=1,2,3 Y=2,4,6 is the worked check in ESTIMATOR_DOC. */
static int ridge_fit_mask(const double *x, int stride, const int *cols, int n, int p,
                          const double *y, const unsigned char *train, double lambda,
                          RidgeModel *m) {
    int r, j, a, b, q, ntrain;
    int *map = g_ridge_map;
    double *scale = g_ridge_scale;
    double ss_tot, ss_res;
    /* The static matrices cannot hold a larger problem. */
    if (n < 1 || n > CX_MAX_RUNS || p < 0 || p > CX_MAX_JOINT) return 0;
    memset(m, 0, sizeof(*m));
    m->p = p;
    ntrain = 0;
    m->mean_y = 0;
    /* Mean of y over the rows this mask keeps. */
    for (r = 0; r < n; r++) {
        /* A held-out row does not enter the training mean. */
        if (train && !train[r]) continue;
        /* A non-finite outcome rejects the fit. */
        if (!is_finite(y[r])) return 0;
        m->mean_y += y[r];
        ntrain++;
    }
    /* One training row cannot center a regression. */
    if (ntrain < 2) return 0;
    m->mean_y /= (double)ntrain;
    /* Mean of each selected column over the same training rows. */
    for (j = 0; j < p; j++) {
        double s = 0;
        int c = cols[j];
        /* Sum this column, skipping held-out rows. */
        for (r = 0; r < n; r++) {
            double v;
            /* Held-out rows stay out of the column mean. */
            if (train && !train[r]) continue;
            v = x[r * stride + c];
            /* A non-finite pause rejects the fit. */
            if (!is_finite(v)) return 0;
            s += v;
        }
        m->mean_x[j] = s / (double)ntrain;
    }
    q = 0;
    /* Scale each centered column by its Euclidean length and drop constants. */
    for (j = 0; j < p; j++) {
        double ss = 0;
        int c = cols[j];
        /* Sum of squared deviations on the training rows. */
        for (r = 0; r < n; r++) {
            double d;
            /* Held-out rows do not contribute to the length. */
            if (train && !train[r]) continue;
            d = x[r * stride + c] - m->mean_x[j];
            ss += d * d;
        }
        scale[j] = sqrt_local(ss);
        m->beta[j] = 0;
        /* A positive length is a column the penalty can see. */
        if (scale[j] > 0) map[q++] = j;
    }
    /* Nothing varied, so the intercept-only fit is already done. */
    if (q == 0) {
        m->ok = 1;
        m->r2 = 0;
        return 1;
    }
    /* Z rows are training rows, packed. Column a of Z corresponds to map[a]. */
    {
        int tr = 0;
        /* Pack one centered, scaled row per training observation. */
        for (r = 0; r < n; r++) {
            /* Held-out rows are not rows of Z. */
            if (train && !train[r]) continue;
            /* Write the scaled centered value into column a of this packed row. */
            for (a = 0; a < q; a++) {
                int src = map[a];
                double d = x[r * stride + cols[src]] - m->mean_x[src];
                g_Z[tr * q + a] = d / scale[src];
            }
            tr++;
        }
    }
    /* Build the Gram matrix of Z and the right-hand side Z'Yc. */
    for (a = 0; a < q; a++) {
        /* The Gram matrix is symmetric, so fill one triangle and mirror it. */
        for (b = a; b < q; b++) {
            double s = 0;
            int tr;
            /* Dot product of packed columns a and b. */
            for (tr = 0; tr < ntrain; tr++) s += g_Z[tr * q + a] * g_Z[tr * q + b];
            g_G[a * q + b] = s;
            g_G[b * q + a] = s;
        }
        g_G[a * q + a] += lambda;
        /* Right-hand side entry is the packed column dotted with centered y. */
        {
            double s = 0;
            int tr = 0;
            /* Walk the original rows so y stays aligned with the training mask. */
            for (r = 0; r < n; r++) {
                double yc;
                /* Skip a held-out row and do not advance the packed index. */
                if (train && !train[r]) continue;
                yc = y[r] - m->mean_y;
                s += g_Z[tr * q + a] * yc;
                tr++;
            }
            g_rhs[a] = s;
        }
    }
    /* A singular penalized system is a failed fit, not a zero model. */
    if (!solve_ge(g_G, g_rhs, q, g_alpha)) return 0;
    /* Convert each scaled coefficient back to pause-microsecond units. */
    for (a = 0; a < q; a++) {
        int src = map[a];
        /* A non-finite alpha is not a beta. */
        if (!is_finite(g_alpha[a])) return 0;
        m->beta[src] = g_alpha[a] / scale[src];
        /* Dividing by the scale can still produce a non-finite beta. */
        if (!is_finite(m->beta[src])) return 0;
    }
    ss_tot = 0;
    ss_res = 0;
    /* R-squared is computed on the training rows only. */
    for (r = 0; r < n; r++) {
        double pred, yc, e;
        /* Held-out rows are not part of this r2. */
        if (train && !train[r]) continue;
        pred = m->mean_y;
        /* Prediction uses every column, including ones left at beta 0. */
        for (j = 0; j < p; j++) pred += m->beta[j] * (x[r * stride + cols[j]] - m->mean_x[j]);
        yc = y[r] - m->mean_y;
        e = y[r] - pred;
        ss_tot += yc * yc;
        ss_res += e * e;
    }
    m->r2 = (ss_tot > 0) ? (1.0 - ss_res / ss_tot) : 0;
    /* A non-finite r2 is reported as zero rather than failing the fit. */
    if (!is_finite(m->r2)) m->r2 = 0;
    m->ok = 1;
    return 1;
}

/* Predict one row from a fitted ridge model. row already points at that
   run, so stride is the caller's indexing and is not used. cols selects
   the same joint columns the model was fit on, and m->p is how many of
   them to apply. Returns mean_y plus the sum of beta times the centered
   column. Does not check finiteness and does not allocate. */
static double ridge_predict(const RidgeModel *m, const double *row, int stride, const int *cols) {
    double yhat = m->mean_y;
    int j;
    /* row already points at one run, so the stride is the caller's indexing, not used here. */
    (void)stride;
    /* Add each column's contribution in original pause units. */
    for (j = 0; j < m->p; j++) yhat += m->beta[j] * (row[cols[j]] - m->mean_x[j]);
    return yhat;
}

/* Penalties choose_lambda tries, from smallest to largest. Ties keep the
   earlier entry. CX_NLAMBDA is the number of entries. A fit with fewer
   than eight rows skips the grid and uses 1e-4. */
static const double CX_LAMBDAS[] = {1e-6, 1e-4, 1e-3, 1e-2, 0.1, 1.0, 10.0};
#define CX_NLAMBDA 7

/* Choose the ridge penalty by 5-fold cross-validation. x, stride, cols,
   n, p, and y are the same joint design ridge_fit_mask expects. Folds
   are row index modulo 5. Returns the lambda on CX_LAMBDAS with the
   smallest summed squared prediction error. Ties keep the smaller
   lambda because the grid is scanned from low to high and an equal
   error does not replace the winner. Fewer than 8 rows, or no fold that
   scores, returns 1e-4 without claiming a search. The fit reuses the
   static g_ridge_model. Does not modify x or y. */
static double choose_lambda(const double *x, int stride, const int *cols, int n, int p, const double *y) {
    unsigned char train[CX_MAX_RUNS];
    double best_sse = 0;
    double best_l = CX_LAMBDAS[0];
    int have = 0;
    int li, fold, r;
    /* Cross-validation is skipped when there are too few completed runs. */
    if (n < 8) return 1e-4;
    /* Try each penalty on the fixed grid. */
    for (li = 0; li < CX_NLAMBDA; li++) {
        double sse = 0;
        int scored = 0;
        /* Each fold holds out rows whose index modulo 5 equals the fold. */
        for (fold = 0; fold < 5; fold++) {
            int ntrain = 0, ntest = 0;
            /* Mark training rows and count both sides of the split. */
            for (r = 0; r < n; r++) {
                train[r] = (unsigned char)((r % 5) != fold);
                /* This row stays in the training mask. */
                if (train[r]) ntrain++;
                /* This row is the held-out fold. */
                else ntest++;
            }
            /* A tiny train or an empty test fold is not scored. */
            if (ntrain < 3 || ntest < 1) continue;
            /* A failed fit contributes nothing for this fold. */
            if (!ridge_fit_mask(x, stride, cols, n, p, y, train, CX_LAMBDAS[li], &g_ridge_model)) continue;
            /* Add the squared error of every held-out row. */
            for (r = 0; r < n; r++) {
                double e;
                /* Training rows are not part of the prediction error. */
                if (train[r]) continue;
                e = y[r] - ridge_predict(&g_ridge_model, x + r * stride, stride, cols);
                sse += e * e;
                scored++;
            }
        }
        /* Skip a penalty that produced no finite score. */
        if (!scored || !is_finite(sse)) continue;
        /* Keep the first finite score, then only a strictly better one. */
        if (!have || sse < best_sse) {
            best_sse = sse;
            best_l = CX_LAMBDAS[li];
            have = 1;
        }
    }
    return have ? best_l : 1e-4;
}

/* One column after fit_columns. It holds the stage-1 slope, the ridge
   slope used to rank the column, the impact used as the sort key, and
   the support counts printed on a TABLE or OBSERVED line. index is the
   column's position in the grain. A column left out of the joint model
   still carries its pause totals. Stage-1 fields stay at their missing
   defaults until that column is admitted and its regression succeeds. */
typedef struct ColOut {
    /* Position of this column in the grain matrix, from 0 through ncols - 1. */
    int index;
    /* Sort key: ridge times mean pause per completed run. Impacts are not added across columns. */
    double impact;
    /* Stage-2 ridge slope, in end-to-end microseconds per pause microsecond. Zero when this column is outside the joint model. */
    double ridge;
    /* Stage-1 slope after the other pause mass in the same run is partialled out. Zero when that regression was not identified. */
    double fwl;
    /* Standard error of fwl. Stays -1 when the column was not identified or the error could not be formed. */
    double fwl_se;
    /* Slope on the other-pause total in the stage-1 regression. Zero when that total does not vary, and when the column was not identified. */
    double gamma;
    /* Permutation p-value, (exceedances + 1) / (permutations + 1). Stays -1 when no permutations were run. */
    double perm_p;
    /* Mean measured pause on this column across completed runs, including runs that missed it. */
    double mean_x;
    /* Sum of measured pause microseconds on this column, with each cell rounded to the nearest integer. */
    long long total_x;
    /* Number of completed runs whose pause on this column was greater than zero. */
    int runs_hit;
    /* 1 when this column was admitted to the ridge model. */
    int in_joint;
    /* 1 when the stage-1 regression for this column solved. */
    int identified;
    /* 1 when identified is set and fwl agrees with ridge under the rule in ESTIMATOR_DOC. */
    int stable;
} ColOut;

/* Fit-wide facts written on the FIT line. Counts describe the completed
   runs and the columns that entered the joint model. lambda, r2, and
   mean_y describe that model. The flags say whether the visit rule was
   relaxed, whether the 96-column cap bound the model, and whether the
   fit stopped before a ridge solve. fit_columns zeros this and then
   fills the fields it reached. */
typedef struct FitOut {
    /* Completed runs in the design. Timed-out runs are already excluded by the caller. */
    int nruns;
    /* Columns in the grain matrix passed to fit_columns, after the CX_MAX_LOCS clamp. */
    int ncols;
    /* Columns actually placed in the ridge model. At most CX_MAX_JOINT (96). */
    int njoint;
    /* 1 when more columns met the visit minimum than the joint model can hold. */
    int capped;
    /* 1 when no column met the ordinary visit minimum and the minimum was lowered to one visit. */
    int relaxed;
    /* Permutation count the caller asked for. The function does not clamp this field. */
    int permutations;
    /* 1 when there is nothing to fit: no columns, no rows, fewer than four rows, an empty joint set, or a failed ridge solve. Totals may still be filled. */
    int too_small;
    /* Penalty chosen by cross-validation, or the small-n default. Stays -1 when the fit returns before a model is chosen. */
    double lambda;
    /* In-sample R^2 of the joint ridge model. Zero when no model was stored. */
    double r2;
    /* Mean end-to-end microseconds across the completed runs in the fit. Zero when no model was stored. */
    double mean_y;
    /* Visit minimum actually applied. Four when nruns is at least 8, two otherwise, or one when relaxed is set. Zero if the fit returned before the minimum was chosen. */
    int min_hits;
} FitOut;

/* Return 1 when the stage-1 slope and the ridge slope agree. The rule
   is |fwl - ridge| <= 0.05 + 0.25 * max(|fwl|, |ridge|), as in
   ESTIMATOR_DOC. Does not check finiteness. A NaN comparison fails and
   the return is 0. Does not look at which column produced the pair. */
static int stable_pair(double fwl, double ridge) {
    double d = dabs(fwl - ridge);
    double mag = dabs(fwl);
    /* The tolerance grows with the larger of the two absolute slopes. */
    if (dabs(ridge) > mag) mag = dabs(ridge);
    return d <= 0.05 + 0.25 * mag;
}

/* Full estimator for one grain. x is row-major nruns * ncols of pause
   microseconds. y is e2e microseconds. permutations is the number of
   stage-1 shuffles; zero or a NULL rng leaves perm_p at -1. cols receives
   one ColOut per column and info the fit summary. Stage 1, stage 2, and
   the stability rule are the ones in ESTIMATOR_DOC. perm_p is -1 when it
   was not computed. ncols above CX_MAX_LOCS is truncated. A missing
   matrix, no rows, fewer than four rows, or no admitted column sets
   info->too_small. Descriptive totals are filled before the four-run
   check. Negative cells are not hits and count as zero. The order, total,
   and permutation buffers are static. Does not allocate or write files. */
static void fit_columns(const double *x, const double *y, int nruns, int ncols,
                        int permutations, Rng *rng, ColOut *cols, FitOut *info) {
    int j, r, k, min_hits, qual, admitted;
    /* Column indexes sorted by total pause, heaviest first. The array does
       not fit in a frame in this no-chkstk build. One fit uses it at a time. */
    static int order[CX_MAX_LOCS];
    /* Rounded pause total of each column, in the same order as the matrix.
       The joint model admits the largest of these that also meet the visit
       minimum. Static for the same frame limit as order. */
    static long long total_col_s[CX_MAX_LOCS];
    /* Pause summed across every column of one completed run. Stage 1 builds
       the other-pause column from this minus the column under test. */
    static double total_run[CX_MAX_RUNS];
    /* Shuffle map for the permutation test. perm_idx[r] is the run whose
       pause is paired with row r. Rebuilt from the identity before each
       shuffle. */
    static int perm_idx[CX_MAX_RUNS];
    int joint[CX_MAX_JOINT];
    long long *total_col = total_col_s;
    /* The static column tables cannot see past this many columns. */
    if (ncols > CX_MAX_LOCS) ncols = CX_MAX_LOCS;
    memset(info, 0, sizeof(*info));
    info->nruns = nruns;
    info->ncols = ncols;
    info->permutations = permutations;
    info->lambda = -1;
    /* Clear every output column and mark the optional fields as missing. */
    for (j = 0; j < ncols; j++) {
        memset(&cols[j], 0, sizeof(cols[j]));
        cols[j].index = j;
        cols[j].perm_p = -1;
        cols[j].fwl_se = -1;
    }
    /* No design to fit. */
    if (ncols < 1 || !x || !y) {
        info->too_small = 1;
        return;
    }
    /* No completed runs to sum. */
    if (nruns < 1) {
        info->too_small = 1;
        return;
    }
    /* Zero each run's pause total before the columns add into it. */
    for (r = 0; r < nruns; r++) total_run[r] = 0;
    /* Per-column hits, mean, and rounded total, plus the run totals. */
    for (j = 0; j < ncols; j++) {
        double sum = 0;
        int hits = 0;
        long long tot = 0;
        /* One cell is this column's pause in one completed run. */
        for (r = 0; r < nruns; r++) {
            double v = x[r * ncols + j];
            /* Only a positive pause counts as a visit. */
            if (v > 0) hits++;
            /* A negative cell does not enter the sums. */
            if (v < 0) v = 0;
            sum += v;
            tot += (long long)(v + 0.5);
            total_run[r] += v;
        }
        cols[j].runs_hit = hits;
        cols[j].mean_x = sum / (double)nruns;
        cols[j].total_x = tot;
        total_col[j] = tot;
    }
    /* Fewer than four completed runs cannot separate a pause effect from the intercept. */
    if (nruns < 4) {
        info->too_small = 1;
        return;
    }
    min_hits = (nruns >= 8) ? 4 : 2;
    info->min_hits = min_hits;
    qual = 0;
    /* Count columns that meet the ordinary visit minimum. */
    for (j = 0; j < ncols; j++) if (cols[j].runs_hit >= min_hits) qual++;
    /* Nobody qualified, so drop the minimum to one visit and say so. */
    if (qual == 0) {
        min_hits = 1;
        info->min_hits = 1;
        info->relaxed = 1;
        qual = 0;
        /* Recount against the relaxed minimum. */
        for (j = 0; j < ncols; j++) if (cols[j].runs_hit >= min_hits) qual++;
    }
    /* Identity order, then sorted by total pause. */
    for (j = 0; j < ncols; j++) order[j] = j;
    /* Insertion sort by total pause descending. n is at most a few thousand. */
    for (k = 1; k < ncols; k++) {
        int id = order[k];
        int t = k;
        /* Shift smaller totals right until this column's slot is open. */
        while (t > 0 && total_col[order[t - 1]] < total_col[id]) {
            order[t] = order[t - 1];
            t--;
        }
        order[t] = id;
    }
    admitted = 0;
    /* Admit the heaviest columns that meet the visit minimum, up to 96. */
    for (k = 0; k < ncols && admitted < CX_MAX_JOINT; k++) {
        int id = order[k];
        /* A column below the visit minimum stays out of the joint model. */
        if (cols[id].runs_hit < min_hits) continue;
        joint[admitted++] = id;
        cols[id].in_joint = 1;
    }
    /* More columns qualified than the joint model can hold. */
    if (qual > admitted) info->capped = 1;
    info->njoint = admitted;
    /* An empty joint model is recorded as too small to fit. */
    if (admitted == 0) {
        info->too_small = 1;
        return;
    }
    info->lambda = choose_lambda(x, ncols, joint, nruns, admitted, y);
    /* A failed ridge fit stops before stage 1. */
    if (!ridge_fit_mask(x, ncols, joint, nruns, admitted, y, NULL, info->lambda, &g_ridge_model)) {
        info->too_small = 1;
        return;
    }
    info->r2 = g_ridge_model.r2;
    info->mean_y = g_ridge_model.mean_y;
    /* Copy the ridge slope and the impact, which is slope times mean pause. */
    for (k = 0; k < admitted; k++) {
        int id = joint[k];
        cols[id].ridge = g_ridge_model.beta[k];
        cols[id].impact = g_ridge_model.beta[k] * cols[id].mean_x;
    }
    /* Stage 1 and the permutation p-value, one admitted column at a time. */
    for (k = 0; k < admitted; k++) {
        int id = joint[k];
        double a = 0, b = 0, g = 0, se = -1;
        int pi;
        /* X is this column, O is every other pause in the same run. */
        for (r = 0; r < nruns; r++) {
            g_px[r] = x[r * ncols + id];
            g_po[r] = total_run[r] - g_px[r];
            g_py[r] = y[r];
        }
        /* A successful stage-1 fit marks the row identified and tests stability. */
        if (ols_two(g_px, g_po, g_py, nruns, &a, &b, &g, &se)) {
            cols[id].identified = 1;
            cols[id].fwl = b;
            cols[id].gamma = g;
            cols[id].fwl_se = se;
            cols[id].stable = stable_pair(b, cols[id].ridge);
        }
        /* Permute only an identified column when the caller asked for shuffles. */
        if (cols[id].identified && permutations > 0 && rng) {
            int exceed = 0;
            double obs = dabs(cols[id].fwl);
            /* Start each permutation from the identity pairing. */
            for (r = 0; r < nruns; r++) perm_idx[r] = r;
            /* One shuffle and one refit per requested permutation. */
            for (pi = 0; pi < permutations; pi++) {
                double aa, bb, gg, ss;
                int rr;
                /* Shuffle which run's pause is paired with this run's outcome
                   and this run's other-pause total. O stays with the run. */
                for (rr = nruns - 1; rr > 0; rr--) {
                    int sw = (int)(rng_u64(rng) % (unsigned long long)(rr + 1));
                    int tmp = perm_idx[rr];
                    perm_idx[rr] = perm_idx[sw];
                    perm_idx[sw] = tmp;
                }
                /* Rebuild X from the shuffled runs. O and Y stay with the row. */
                for (rr = 0; rr < nruns; rr++) {
                    g_px[rr] = x[perm_idx[rr] * ncols + id];
                    g_po[rr] = total_run[rr] - x[rr * ncols + id];
                    g_py[rr] = y[rr];
                }
                /* A singular permuted fit does not count as an exceedance. */
                if (!ols_two(g_px, g_po, g_py, nruns, &aa, &bb, &gg, &ss)) continue;
                /* Count a permuted slope at least as large as the observed one. */
                if (dabs(bb) + 1e-12 >= obs) exceed++;
            }
            cols[id].perm_p = (double)(exceed + 1) / (double)(permutations + 1);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Profiles, grouping, ledgers                                                */
/* -------------------------------------------------------------------------- */

#define GRAIN_LOC 0
#define GRAIN_BLOCK 1
#define GRAIN_FUNC 2

#define MODE_PROFILE 0
#define MODE_SELF_TEST 1
#define MODE_BURN 2
#define MODE_HELP 3

/* Parsed command line. mode selects profile, self-test, burn, or help.
   The integer fields are the experiment knobs after parsing and, for a
   profile, after finish_options has applied the caps. command is the
   quoted child command line. warn and error are narrow messages for
   stderr. options_default fills the profile defaults; parse_options
   overlays argv and does not reset the struct first. */
typedef struct Options {
    /* What main will do. MODE_PROFILE is 0, MODE_SELF_TEST is 1, MODE_BURN is 2, and MODE_HELP is 3. */
    int mode;
    /* How many times the child command is launched. Default 20. finish_options caps it at CX_MAX_RUNS. */
    int runs;
    /* Causal pauses requested in each run, or sampling waves per run when sampling is set. Default 30. Capped at CX_MAX_PER_RUN. */
    int samples;
    /* Low end of the uniform pause draw, in microseconds. Default 200. The regression uses the measured duration, not this bound. */
    int pause_min_us;
    /* High end of the uniform pause draw, in microseconds. Default 4000. */
    int pause_max_us;
    /* Low end of the wait between causal injections or sampling waves, in microseconds. Default 500. */
    int gap_min_us;
    /* High end of that wait, in microseconds. Default 8000. */
    int gap_max_us;
    /* Delay after the child starts before the first injection or wave, in microseconds. Default 0. */
    int warmup_us;
    /* How long a run may last before it is killed, in milliseconds. Default 60000. */
    int timeout_ms;
    /* Stage-1 shuffles per admitted column. Default 80. finish_options reduces a value above 500. */
    int permutations;
    /* 1 when the child's standard handles go to NUL and the process is created with CREATE_NO_WINDOW. */
    int quiet_child;
    /* 1 when --sampling is set. Ranks are then sample shares, and every visible thread is read on each wave. */
    int sampling;
    /* 1 when --seed was present, or after run_profile has chosen a seed of its own. */
    int seed_set;
    /* 1 when finish_options forced a knob down to its cap. */
    int clamped;
    /* Milliseconds requested by --burn, before run_burn clamps the value to the range 50..20000. */
    int burn_ms;
    /* Seed for the experiment generator. Used only after seed_set is 1. */
    unsigned long long seed;
    /* Directory that receives the seven output files. Default L"out". */
    wchar_t out_dir[CX_PATH];
    /* Quoted child command line passed to CreateProcessW. Empty in help, burn, and self-test. */
    wchar_t command[8192];
    /* Warning written when a knob was clamped. Empty when nothing was reduced. */
    char warn[256];
    /* Reason parse_options or finish_options rejected the command line. Empty on success. */
    char error[256];
} Options;

/* One launch of the child. e2e_us is the wall time from just before
   CreateProcessW until the process is signaled. A run that exits with a
   non-zero status is still completed. A timed-out run is stored here and
   left out of the regression. samples counts what this run contributed
   to the ledger. */
typedef struct RunInfo {
    /* End-to-end microseconds, from the counter read before CreateProcessW until the process handle is signaled. */
    long long e2e_us;
    /* Process exit code. Unchanged when the process never started. */
    int exit_code;
    /* 1 when CreateProcessW succeeded. */
    int started;
    /* 1 when the process exited before the timeout. A non-zero exit code still counts as completed. */
    int completed;
    /* 1 when the run was killed because timeout_ms elapsed. Those runs stay in the ledger and stay out of the fit. */
    int timed_out;
    /* Causal pauses stored for this run, or thread samples stored for this run when the profile is in sampling mode. */
    int samples;
} RunInfo;

/* One observed instruction. Causal mode records the thread that was
   held and both the requested pause and the pause the counter actually
   measured. Sampling mode records every thread in the wave, with a
   requested duration of 0 and a measured duration equal to the time
   spent reading that thread's context. The regression and the sample
   share both use measured_us. */
typedef struct Sample {
    /* Index of the run this observation belongs to. */
    int run;
    /* Index into Profile.locs. Identifies the module and offset. */
    int loc;
    /* Thread id that was suspended to read the instruction pointer. */
    unsigned tid;
    /* Threads visible in the snapshot taken for this injection or wave. */
    int nthreads;
    /* Pause draw in microseconds. Zero for a sampling-wave observation, which does not sleep. */
    long long requested_us;
    /* Duration the ledger and the fit use, in microseconds. A negative counter delta is stored as 0. */
    long long measured_us;
} Sample;

/* One image seen in a child process, or the unmapped bucket. Identity
   is the full path, so a module that moves under ASLR stays one row.
   name is the short label used in ledgers. Symbolication loads the
   image at a synthetic base and records that base here. The unmapped
   row collects instruction pointers that missed every snapshot. */
typedef struct ModInfo {
    /* Full wide path. Empty for the unmapped bucket. This is the identity across runs. */
    wchar_t path[CX_PATH];
    /* Short name in the ANSI code page. A colliding leaf becomes name#N. */
    char name[128];
    /* Full path in the ANSI code page, passed to SymLoadModuleEx. Empty when the path did not convert. */
    char path_acp[CX_PATH];
    /* Image size in bytes, from the module snapshot. Zero when the size was not reported. */
    unsigned long size;
    /* Base address returned by SymLoadModuleEx. Zero until that module loads. Queries add the location offset to this base. */
    unsigned long long sym_base;
    /* 1 when SymLoadModuleEx returned a base for this module. */
    int sym_ok;
    /* Win32 error from a failed SymLoadModuleEx. Zero when the load was not attempted or it succeeded. */
    int sym_err;
    /* 1 for the single bucket that holds instruction pointers outside every snapshot. */
    int is_unmapped;
} ModInfo;

/* One distinct instruction, keyed by module index and offset. intern_loc
   inserts a row with the group ids at -1. symbolize fills the function
   and line fields from dbghelp. classify_groups then assigns block_id
   and func_id. An offset is RIP minus the module base, or the raw RIP
   when the location sits in the unmapped bucket. */
typedef struct LocInfo {
    /* Index into Profile.mods. */
    int module;
    /* Byte offset from the module base. The raw instruction pointer when the module is the unmapped bucket. */
    unsigned long long offset;
    /* 1 when SymFromAddr supplied a nonempty function name. */
    int has_func;
    /* 1 when SymGetLineFromAddr64 supplied a line number greater than zero. */
    int has_line;
    /* 1-based source line. Zero when has_line is 0. */
    int line;
    /* Offset of the function start within the module. Zero when the address was below the loaded base or no function was found. */
    unsigned long long func_start;
    /* Function name from the PDB, sanitized for a ledger label. Empty when has_func is 0. */
    char func[CX_NAME];
    /* Source file path from the PDB, sanitized. Empty when has_line is 0. */
    char file[CX_NAME];
    /* Index into Profile.blocks after classify_groups. -1 until then, and when the block table is full. */
    int block_id;
    /* Index into Profile.funcs after classify_groups. -1 until then, and when the function table is full. */
    int func_id;
} LocInfo;

/* One block column or one function column. classify_groups builds a raw
   label and intern_group matches on that string, then stores a sanitized
   copy. members counts instructions assigned to the group. total_us sums
   measured_us from completed runs. The same layout is used for
   Profile.blocks and Profile.funcs. */
typedef struct Group {
    /* Stored identity after sanitizing. Control bytes, tab, and DEL have been turned into spaces. Matching uses the raw label from before that pass. */
    char label[CX_LABEL];
    /* How many distinct instructions were assigned to this group. */
    int members;
    /* Sum of measured_us over samples from completed runs whose location maps to this group. */
    long long total_us;
} Group;

/* The whole experiment. opt is the parsed command line. The fixed runs
   array holds one RunInfo per launch. Samples, locations, modules, and
   both group tables live in the arena. loc_hash interns locations.
   The failure counters and the symbol note are what the console summary
   and symbols.txt report. profile_new zeros the profile after the arena
   allocations succeed. */
typedef struct Profile {
    /* Command line this experiment is running. Not owned. */
    Options *opt;
    /* One entry per launch, in order. Only the first nruns entries are live. */
    RunInfo runs[CX_MAX_RUNS];
    /* Launches stored in runs. At most CX_MAX_RUNS. */
    int nruns;
    /* Arena array of observations. NULL when the allocation failed. */
    Sample *samples;
    /* Observations stored in samples. */
    int nsamples;
    /* Capacity of samples. Causal profiles use runs times samples. Sampling profiles use CX_MAX_SAMPLES. */
    int sample_cap;
    /* Arena array of distinct instructions. */
    LocInfo *locs;
    /* Instructions stored in locs. */
    int nlocs;
    /* Open-addressed intern table. An empty slot is 0. A stored value is the location index plus one. */
    int loc_hash[CX_LOC_HASH];
    /* 1 when intern_loc refused a new instruction because the table or the hash was full. */
    int locs_capped;
    /* Arena array of images, including at most one unmapped bucket. */
    ModInfo *mods;
    /* Modules stored in mods. */
    int nmods;
    /* Arena array of block groups, filled by classify_groups. */
    Group *blocks;
    /* Block groups stored in blocks. */
    int nblocks;
    /* Arena array of function groups, filled by classify_groups. */
    Group *funcs;
    /* Function groups stored in funcs. */
    int nfuncs;
    /* OpenThread or SuspendThread calls that failed. */
    int suspend_failures;
    /* GetThreadContext calls that failed or returned a zero instruction pointer. */
    int context_failures;
    /* Observations that could not be stored because a module, location, or sample slot was unavailable. */
    int samples_dropped;
    /* 1 after SymInitialize succeeded, until SymCleanup clears it. */
    int sym_ready;
    /* First non-zero Win32 error recorded by note_err. Zero when no such error was kept. */
    DWORD first_api_err;
    /* Short symbolication status, such as "dbghelp ok" or the reason symbols were skipped. */
    char sym_note[160];
} Profile;

/* One module range from a single toolhelp snapshot. find_snap walks
   these ranges to turn an instruction pointer into a module id and an
   offset. The range is half-open: base through base + size. A later
   snapshot of the same process has its own array of these rows. */
typedef struct ModSnap {
    /* Index into Profile.mods for the image that owned this range. */
    int mod;
    /* Base address of the image in the child at the time of this snapshot. */
    unsigned long long base;
    /* Size of the image in bytes. A zero size is skipped and cannot contain an address. */
    unsigned long size;
} ModSnap;

/* Copy the last path component of path into dst. A NULL path is empty.
   Both '\\' and '/' count as separators, and the last one wins. A path
   with no separator is copied whole. cap is the byte capacity, and
   str_copy drops characters that do not fit. Returns the length stored,
   not counting the NUL. Does not strip a trailing separator. */
static int basename_of(const char *path, char *dst, int cap) {
    int i, slash = -1, n;
    /* A missing path contributes no characters. */
    if (!path) path = "";
    n = str_len(path);
    /* Remember the last slash of either Windows or POSIX form. */
    for (i = 0; i < n; i++) if (path[i] == '\\' || path[i] == '/') slash = i;
    str_copy(dst, cap, path + slash + 1);
    return str_len(dst);
}

/* Replace control bytes, tab, and DEL in s with spaces. Other bytes,
   including high bytes, are left as they are. s must be NUL-terminated.
   The length does not change. Does not allocate. */
static void sanitize_label(char *s) {
    int i;
    /* Walk every byte of the label. */
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        /* A control byte would break a one-line ledger field. */
        if (c < 32 || c == 127 || c == '\t') s[i] = ' ';
    }
}

/* Return the index of label in gs, inserting it when it is new. *count
   is the number of live groups and cap is the array capacity. The lookup
   compares the raw label. The stored copy is then sanitized and truncated
   to CX_LABEL. Returns -1 when the table is full and the label is new.
   Does not allocate. members and total_us of a new group are zero. */
static int intern_group(Group *gs, int *count, int cap, const char *label) {
    int i;
    /* Reuse an existing group with the same raw label. */
    for (i = 0; i < *count; i++) {
        /* An exact label match is the group already assigned. */
        if (str_eq(gs[i].label, label)) return i;
    }
    /* No room for a group that has not been seen. */
    if (*count >= cap) return -1;
    i = *count;
    memset(&gs[i], 0, sizeof(gs[i]));
    str_copy(gs[i].label, CX_LABEL, label);
    sanitize_label(gs[i].label);
    *count = i + 1;
    return i;
}

/* Write "module+offset" for loc into dst. cap is the byte capacity.
   A module index outside the profile's table uses the placeholder "mod".
   The offset is lowercase hex through the shared g_fmt buffer. Does not
   include a function or file name. Truncates when cap is short. */
static void loc_label(const Profile *p, const LocInfo *loc, char *dst, int cap) {
    const char *mod = "mod";
    /* Use the module's short name when the index is in range. */
    if (loc->module >= 0 && loc->module < p->nmods) mod = p->mods[loc->module].name;
    dst[0] = 0;
    str_append(dst, cap, mod);
    str_append(dst, cap, "+");
    format_hex(g_fmt, (int)sizeof(g_fmt), loc->offset);
    str_append(dst, cap, g_fmt);
}

/* Block and function identity, as specified in ESTIMATOR_DOC. Each
   location is assigned func_id and block_id by interning a label.
   A function label is module!func, or module+offset when there is no
   symbol. A block label is module!func file:line when both exist, a
   16-byte window from the function start when the function has no line,
   and module+offset otherwise. Counts are reset first, so a second call
   rebuilds the tables. An id is -1 when the group table is full. Uses
   g_fmt for the line number. Does not allocate. */
static void classify_groups(Profile *p) {
    int i;
    p->nblocks = 0;
    p->nfuncs = 0;
    /* One function group and one block group per recorded instruction. */
    for (i = 0; i < p->nlocs; i++) {
        LocInfo *loc = &p->locs[i];
        char label[CX_LABEL];
        char hex[40];
        const char *mod = "mod";
        int id;
        /* Fall back to "mod" when this location's module index is stale. */
        if (loc->module >= 0 && loc->module < p->nmods) mod = p->mods[loc->module].name;
        label[0] = 0;
        /* A named function is shared by every instruction with that symbol. */
        if (loc->has_func) {
            str_append(label, CX_LABEL, mod);
            str_append(label, CX_LABEL, "!");
            str_append(label, CX_LABEL, loc->func);
        } else /* No symbol, so this instruction is its own function column. */ {
            loc_label(p, loc, label, CX_LABEL);
        }
        id = intern_group(p->funcs, &p->nfuncs, CX_MAX_LOCS, label);
        loc->func_id = id;

        label[0] = 0;
        /* A source line is the block when the PDB provided one. */
        if (loc->has_func && loc->has_line) {
            char base[128];
            basename_of(loc->file, base, (int)sizeof(base));
            str_append(label, CX_LABEL, mod);
            str_append(label, CX_LABEL, "!");
            str_append(label, CX_LABEL, loc->func);
            str_append(label, CX_LABEL, " ");
            str_append(label, CX_LABEL, base);
            str_append(label, CX_LABEL, ":");
            format_i64(g_fmt, (int)sizeof(g_fmt), loc->line);
            str_append(label, CX_LABEL, g_fmt);
        } else if (loc->has_func) /* Function known, but no line: a 16-byte window. */ {
            unsigned long long delta = 0;
            unsigned long long bucket;
            /* An offset before the symbol start stays in the first window. */
            if (loc->offset >= loc->func_start) delta = loc->offset - loc->func_start;
            bucket = delta & ~((unsigned long long)CX_BLOCK - 1ULL);
            str_append(label, CX_LABEL, mod);
            str_append(label, CX_LABEL, "!");
            str_append(label, CX_LABEL, loc->func);
            str_append(label, CX_LABEL, "+");
            format_hex(hex, (int)sizeof(hex), bucket);
            str_append(label, CX_LABEL, hex);
        } else /* No symbol, so the instruction stays its own block. */ {
            loc_label(p, loc, label, CX_LABEL);
        }
        id = intern_group(p->blocks, &p->nblocks, CX_MAX_LOCS, label);
        loc->block_id = id;
    }
}

/* Return the column index of location loc at the requested grain.
   GRAIN_LOC is the location index itself, GRAIN_BLOCK is block_id, and
   any other grain is func_id. Returns -1 when loc is outside the table.
   Does not check that the group id is in range. */
static int col_of(const Profile *p, int loc, int grain) {
    /* A location index outside the table is not a column. */
    if (loc < 0 || loc >= p->nlocs) return -1;
    /* The location grain does not go through a group id. */
    if (grain == GRAIN_LOC) return loc;
    /* The block grain uses the id classify_groups stored. */
    if (grain == GRAIN_BLOCK) return p->locs[loc].block_id;
    return p->locs[loc].func_id;
}

/* Return how many columns the grain has. GRAIN_LOC is nlocs, GRAIN_BLOCK
   is nblocks, and any other grain is nfuncs. Does not allocate. */
static int ncols_of(const Profile *p, int grain) {
    /* One column per recorded instruction. */
    if (grain == GRAIN_LOC) return p->nlocs;
    /* One column per interned block. */
    if (grain == GRAIN_BLOCK) return p->nblocks;
    return p->nfuncs;
}

/* Return the display label for column col at grain. Block and function
   labels are pointers into the group tables. A location label is written
   into tmp, which must hold cap bytes, and tmp is returned. An out-of-range
   column writes "?" into tmp and returns tmp. Does not allocate. */
static const char *label_of(const Profile *p, int grain, int col, char *tmp, int cap) {
    /* Block labels live in the group table. */
    if (grain == GRAIN_BLOCK && col >= 0 && col < p->nblocks) return p->blocks[col].label;
    /* Function labels live in the group table. */
    if (grain == GRAIN_FUNC && col >= 0 && col < p->nfuncs) return p->funcs[col].label;
    /* A location label is formatted into the caller buffer. */
    if (grain == GRAIN_LOC && col >= 0 && col < p->nlocs) {
        loc_label(p, &p->locs[col], tmp, cap);
        return tmp;
    }
    str_copy(tmp, cap, "?");
    return tmp;
}

/* Map each run to a dense row of completed runs. row_of[r] is -1 when
   run r did not complete, otherwise the next dense index. y receives
   e2e_us as a double for those rows. Returns the number of completed
   runs. row_of must have nruns slots and y must hold the returned count.
   Does not treat a timeout as completed. */
static int prepare_rows(const Profile *p, int *row_of, double *y) {
    int r, n = 0;
    /* Completed runs are packed to the front of y. */
    for (r = 0; r < p->nruns; r++) {
        row_of[r] = -1;
        /* A run that did not finish stays out of the design matrix. */
        if (!p->runs[r].completed) continue;
        row_of[r] = n;
        y[n] = (double)p->runs[r].e2e_us;
        n++;
    }
    return n;
}

/* Sum measured pauses into a dense matrix. x is row-major nrows by ncols
   and is zeroed first. grain selects the column through col_of. row_of
   maps a run to a dense row. Timed-out runs stay at row -1 and add
   nothing. A sample with a bad run, a negative row, or a column outside
   the matrix is skipped. Adds measured_us, not the requested draw.
   Does not allocate. */
static void accumulate(const Profile *p, int grain, const int *row_of, double *x, int nrows, int ncols) {
    int i, cells;
    cells = nrows * ncols;
    /* Start from a zero matrix so repeated calls do not pile up. */
    for (i = 0; i < cells; i++) x[i] = 0;
    /* Each sample adds its measured pause into one cell. */
    for (i = 0; i < p->nsamples; i++) {
        const Sample *s = &p->samples[i];
        int row, col;
        /* Ignore a sample whose run index is outside the profile. */
        if (s->run < 0 || s->run >= p->nruns) continue;
        row = row_of[s->run];
        /* A negative row is a run left out of the fit. */
        if (row < 0) continue;
        col = col_of(p, s->loc, grain);
        /* Drop a sample whose column is missing or past the matrix. */
        if (col < 0 || col >= ncols) continue;
        x[row * ncols + col] += (double)s->measured_us;
    }
}

/* Counts and status returned by run_profile. The function zeros this
   before the runs. rc is also the function's return value. A NULL
   caller pointer is replaced with a local status that is discarded.
   The self-test reads these fields to decide whether the live smoke
   recorded enough samples and separated the burn functions. */
typedef struct ProfileStatus {
    /* 0 when at least one instruction pointer was stored. 2 when the runs finished and none was. 1 when setup, a run, or a write failed. */
    int rc;
    /* Runs that exited before the timeout, including runs with a non-zero exit code. */
    int runs_completed;
    /* Runs killed because the timeout elapsed. */
    int runs_timed_out;
    /* Observations stored across every run. */
    int samples;
    /* Distinct instructions interned. */
    int locations;
    /* Function groups after classification. Zero when a failure returns before symbolication. */
    int functions;
    /* Block groups after classification. Zero when a failure returns before symbolication. */
    int blocks;
    /* OpenThread or SuspendThread failures accumulated during the runs. */
    int suspend_failures;
    /* Threads whose instruction pointer could not be read. */
    int context_failures;
    /* First non-zero Win32 error kept for the profile. Zero when none was kept. */
    DWORD first_api_err;
} ProfileStatus;

/* Module handle from LoadLibraryW of dbghelp.dll. NULL until ensure_dbghelp
   loads it. Cleared again when a required export is missing. */
static void *g_dbghelp = NULL;
/* SymInitialize. Required before any module is loaded. NULL until resolved. */
static SymInitialize_fn pSymInitialize = NULL;
/* SymCleanup. Required. Called after the locations have been named. */
static SymCleanup_fn pSymCleanup = NULL;
/* SymSetOptions. Required. symbolize sets the undname, lines, and no-prompt flags. */
static SymSetOptions_fn pSymSetOptions = NULL;
/* SymSetSearchPath. Optional. A missing export still counts as a successful load. */
static SymSetSearchPath_fn pSymSetSearchPath = NULL;
/* SymLoadModuleEx. Required. Loads each image at a synthetic base. */
static SymLoadModuleEx_fn pSymLoadModuleEx = NULL;
/* SymFromAddr. Required. Supplies the function name and start address. */
static SymFromAddr_fn pSymFromAddr = NULL;
/* SymGetLineFromAddr64. Optional. Supplies the source file and line. Names
   are still recorded when this export is absent. */
static SymGetLine_fn pSymGetLine = NULL;

/* Module ranges from the latest toolhelp snapshot. find_snap maps an
   instruction pointer through this table. The next injection replaces it. */
static ModSnap g_snaps[CX_MAX_MODS];
/* Thread ids from the latest snapshot of the child. Causal mode picks one
   index. A sampling wave walks every entry. */
static DWORD g_tids[CX_MAX_THREADS];
/* Writable copy of the child command line. CreateProcessW may edit the
   buffer, so each run copies Options.command here first. The unprofiled
   warmup burn is launched from this buffer too. */
static wchar_t g_cmd_mut[8192];
/* Column indexes in display order. order_cols and order_sample_cols fill
   it. Rank writers, the console summary, and one self-test walk it.
   One ordering at a time. */
static int g_rank_idx[CX_MAX_LOCS];
/* Sample counts for the grain currently being ranked. count_grain zeros
   the prefix it uses. Sampling ranks and the console share read it.
   Slots past that prefix stay stale. */
static int g_sample_counts[CX_MAX_LOCS];
/* Storage for one SYMBOL_INFO plus 1024 extra name bytes. symbolize points
   the struct at the front and sets MaxNameLen so the name can extend into
   the tail. One query at a time. */
static unsigned char g_symstore[sizeof(SYMBOL_INFO) + 1024];

/* Release the arena's single reservation and clear the header. A NULL
   base is not passed to VirtualFree. Individual allocations are not
   freed; they die with the region. After this, arena_alloc must not
   be used until arena_init succeeds again. */
static void arena_free(Arena *a) {
    /* Only a region that was reserved has a base to release. */
    if (a->base) VirtualFree(a->base, 0, MEM_RELEASE);
    a->base = NULL;
    a->cap = 0;
    a->used = 0;
}

/* Delay the calling thread for about us microseconds. A non-positive
   us returns immediately. At 2000 microseconds or more, the whole
   milliseconds go to Sleep and the leftover microseconds are spun
   afterward. Shorter delays are spun entirely. The spin length is in
   performance-counter ticks and is at least one tick. qpc_init must
   already have run. Does not suspend another process. */
static void sleep_us(int us) {
    long long start, need;
    /* A zero or negative request is not a sleep. */
    if (us <= 0) return;
    /* Long enough that a millisecond Sleep is the bulk of the wait. */
    if (us >= 2000) {
        Sleep((DWORD)(us / 1000));
        us %= 1000;
        /* The request was an exact number of milliseconds. */
        if (us <= 0) return;
    }
    start = qpc_now();
    need = (g_qpc_freq * (long long)us) / 1000000LL;
    /* At least one tick, so a tiny remainder still waits. */
    if (need < 1) need = 1;
    /* Busy-wait the leftover microseconds. The body is intentionally empty. */
    while (qpc_now() - start < need) {
    }
}

/* Gap between injections. The child keeps running. A pause must not use
   this. us is the requested gap in microseconds. A non-positive us
   returns immediately. At 2000 microseconds or more the wait sleeps one
   millisecond at a time and returns early if proc is no longer running.
   Shorter gaps spin, also returning when proc has exited. qpc_init must
   have run. Does not suspend proc. */
static void wait_us_proc(HANDLE proc, int us) {
    long long start, need;
    /* No gap to wait. */
    if (us <= 0) return;
    start = qpc_now();
    need = (g_qpc_freq * (long long)us) / 1000000LL;
    /* At least one tick so the comparison can succeed. */
    if (need < 1) need = 1;
    /* A long gap yields the CPU, but still stops if the child exits. */
    if (us >= 2000) {
        /* Sleep in short steps until the tick budget is spent. */
        while (qpc_now() - start < need) {
            /* The process handle is signaled, so the gap is over. */
            if (WaitForSingleObject(proc, 0) != WAIT_TIMEOUT) return;
            Sleep(1);
        }
        return;
    }
    /* A short gap spins, and still stops when the child exits. */
    while (qpc_now() - start < need) {
        /* Leave as soon as the process is no longer running. */
        if (WaitForSingleObject(proc, 0) != WAIT_TIMEOUT) return;
    }
}

/* Return 1 when proc is still running. That is a zero-timeout wait
   reporting WAIT_TIMEOUT. Any other result, including a failed wait,
   returns 0. Does not block. */
static int process_alive(HANDLE proc) {
    return WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
}

/* Record the first non-zero GetLastError on the profile. If first_api_err
   is already set, this returns without reading the last error. A zero
   code is ignored. Does not format the message. */
static void note_err(Profile *p) {
    DWORD e;
    /* Keep the first failure rather than the most recent one. */
    if (p->first_api_err) return;
    e = GetLastError();
    /* A cleared last-error code is not a recorded failure. */
    if (e) p->first_api_err = e;
}

/* Write the system text for err into dst. cap is the byte capacity.
   A cap below 2 returns without writing. FormatMessageA failure stores
   "unknown error". Carriage returns and newlines in the system text
   become spaces so the result stays one line. Does not append the
   numeric code. Inserts in the message are ignored. */
static void format_winerr(DWORD err, char *dst, int cap) {
    DWORD n;
    int i;
    /* No room for a terminator and one character. */
    if (cap < 2) return;
    dst[0] = 0;
    n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, err, 0, dst, (DWORD)(cap - 1), NULL);
    /* The system has no text for this code. */
    if (!n) {
        str_copy(dst, cap, "unknown error");
        return;
    }
    /* Flatten the message onto one line. */
    for (i = 0; dst[i]; i++) {
        /* A CR or LF would split a console error line. */
        if (dst[i] == '\r' || dst[i] == '\n') dst[i] = ' ';
    }
}

/* Try to enable SeDebugPrivilege on the current process so thread and
   module snapshots of other processes can succeed. OpenProcessToken or
   LookupPrivilegeValue failure returns without reporting it. The result
   of AdjustTokenPrivileges is ignored. The token is closed. Does not
   check that the privilege was actually granted. */
static void enable_debug_priv(void) {
    HANDLE tok = NULL;
    TOKEN_PRIVILEGES tp;
    /* Without a token there is nothing to adjust. */
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    memset(&tp, 0, sizeof(tp));
    tp.PrivilegeCount = 1;
    /* The privilege name is missing, so the token must still be closed. */
    if (!LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &tp.Privileges[0].Luid)) {
        CloseHandle(tok);
        return;
    }
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(tok);
}

/* Load dbghelp.dll once and resolve the symbol exports. Returns 1 when
   SymInitialize, SymCleanup, SymSetOptions, SymLoadModuleEx, and
   SymFromAddr are all present. SymSetSearchPath and SymGetLineFromAddr64
   may be missing and the return is still 1. A later call returns whether
   SymFromAddr was resolved. A missing required export frees the library,
   clears g_dbghelp, and returns 0. Does not call SymInitialize. */
static int ensure_dbghelp(void) {
    /* Already loaded: success is whether the address lookup export exists. */
    if (g_dbghelp) return pSymFromAddr != NULL;
    g_dbghelp = LoadLibraryW(L"dbghelp.dll");
    /* The DLL itself did not load. */
    if (!g_dbghelp) return 0;
    pSymInitialize = (SymInitialize_fn)GetProcAddress(g_dbghelp, "SymInitialize");
    pSymCleanup = (SymCleanup_fn)GetProcAddress(g_dbghelp, "SymCleanup");
    pSymSetOptions = (SymSetOptions_fn)GetProcAddress(g_dbghelp, "SymSetOptions");
    pSymSetSearchPath = (SymSetSearchPath_fn)GetProcAddress(g_dbghelp, "SymSetSearchPath");
    pSymLoadModuleEx = (SymLoadModuleEx_fn)GetProcAddress(g_dbghelp, "SymLoadModuleEx");
    pSymFromAddr = (SymFromAddr_fn)GetProcAddress(g_dbghelp, "SymFromAddr");
    pSymGetLine = (SymGetLine_fn)GetProcAddress(g_dbghelp, "SymGetLineFromAddr64");
    /* The exports the symbol pass cannot run without are missing. */
    if (!pSymInitialize || !pSymCleanup || !pSymSetOptions || !pSymLoadModuleEx || !pSymFromAddr) {
        FreeLibrary(g_dbghelp);
        g_dbghelp = NULL;
        return 0;
    }
    return 1;
}

/* Copy the directory portion of path into dst, without the final
   separator. Both '\\' and '/' count. A NULL path, a missing separator,
   a separator at the first byte, or a cap below 2 leaves dst empty when
   cap is positive. A directory longer than cap is truncated. Does not
   keep a trailing slash. */
static void dir_of_path(const char *path, char *dst, int cap) {
    int n, i, slash = -1;
    n = str_len(path);
    /* The last separator is the cut between the directory and the file. */
    for (i = 0; i < n; i++) if (path[i] == '\\' || path[i] == '/') slash = i;
    /* No usable directory, or nowhere to write one. */
    if (slash <= 0 || cap < 2) {
        /* Still terminate a buffer that cannot hold the directory. */
        if (cap > 0) dst[0] = 0;
        return;
    }
    /* Leave one byte for the terminator. */
    if (slash >= cap) slash = cap - 1;
    /* Copy the bytes before the separator. */
    for (i = 0; i < slash; i++) dst[i] = path[i];
    dst[slash] = 0;
}

/* Return 1 when some module other than self already uses name. The
   compare is case-sensitive. self is skipped so a module can keep its
   own name. Returns 0 when no other module matches. */
static int name_taken(const Profile *p, const char *name, int self) {
    int i;
    /* Check every module recorded so far. */
    for (i = 0; i < p->nmods; i++) {
        /* The caller is allowed to match itself. */
        if (i == self) continue;
        /* Another module already published this short name. */
        if (str_eq(p->mods[i].name, name)) return 1;
    }
    return 0;
}

/* Find or add a module and return its index. A mapped module with a
   non-empty path matches an existing one by case-insensitive path.
   unmapped reuses the first unmapped entry. A full table returns -1.
   A new unmapped module is named "unmapped". Otherwise the short name
   is wname converted to the ANSI code page, else the path basename,
   else "module". A short name already used by another module gets
   "#2" through "#49" appended, and the search then stops. A NULL path
   is stored empty and is not matched to an existing module. Uses g_fmt.
   Does not load the image. */
static int intern_module(Profile *p, const wchar_t *path, const wchar_t *wname, int unmapped) {
    int i, ntry;
    char acp_name[160];
    char try_name[160];
    /* A real path can match a module already recorded from an earlier snap. */
    if (!unmapped && path && path[0]) {
        /* Skip the unmapped bucket; its path is not an identity. */
        for (i = 0; i < p->nmods; i++) {
            /* The unmapped placeholder is not a path match. */
            if (p->mods[i].is_unmapped) continue;
            /* Paths compare without regard to ASCII case. */
            if (w_eq_i(p->mods[i].path, path)) return i;
        }
    } else if (unmapped) /* Every unmapped address shares one module entry. */ {
        /* The first unmapped module is the only one. */
        for (i = 0; i < p->nmods; i++) if (p->mods[i].is_unmapped) return i;
    }
    /* No room for a module that did not match. */
    if (p->nmods >= CX_MAX_MODS) return -1;
    i = p->nmods++;
    memset(&p->mods[i], 0, sizeof(ModInfo));
    p->mods[i].is_unmapped = unmapped;
    /* The placeholder has a fixed name and no path. */
    if (unmapped) {
        str_copy(p->mods[i].name, (int)sizeof(p->mods[i].name), "unmapped");
        return i;
    }
    w_copy(p->mods[i].path, CX_PATH, path ? path : L"");
    wide_to_acp(p->mods[i].path, p->mods[i].path_acp, CX_PATH);
    acp_name[0] = 0;
    /* Fall back to the file name when the wide short name does not convert. */
    if (!wname || !wide_to_acp(wname, acp_name, (int)sizeof(acp_name)) || !acp_name[0]) {
        basename_of(p->mods[i].path_acp, acp_name, (int)sizeof(acp_name));
    }
    /* Still nothing usable, so use a generic label. */
    if (!acp_name[0]) str_copy(acp_name, (int)sizeof(acp_name), "module");
    str_copy(try_name, (int)sizeof(try_name), acp_name);
    ntry = 2;
    /* Disambiguate a short name another module already has. */
    while (name_taken(p, try_name, i) && ntry < 50) {
        try_name[0] = 0;
        str_append(try_name, (int)sizeof(try_name), acp_name);
        str_append(try_name, (int)sizeof(try_name), "#");
        format_i64(g_fmt, (int)sizeof(g_fmt), ntry);
        str_append(try_name, (int)sizeof(try_name), g_fmt);
        ntry++;
    }
    str_copy(p->mods[i].name, (int)sizeof(p->mods[i].name), try_name);
    return i;
}

/* Return the location id for module mod at offset, inserting it when new.
   The hash is FNV-style over mod+1 and offset into loc_hash, which must
   start zeroed. An empty slot is 0 and a stored id is the index plus one.
   Probing is linear. Returns -1 and sets locs_capped when the location
   table is full or every slot was probed. A new location has block_id
   and func_id of -1. Does not look up symbols. */
static int intern_loc(Profile *p, int mod, unsigned long long offset) {
    unsigned long long h = 14695981039346656037ULL;
    unsigned mask = CX_LOC_HASH - 1;
    unsigned slot;
    int probes = 0;
    h ^= (unsigned long long)(mod + 1);
    h *= 1099511628211ULL;
    h ^= offset;
    h *= 1099511628211ULL;
    slot = (unsigned)h & mask;
    /* Probe until the key is found, a free slot is filled, or the table wraps. */
    while (probes < CX_LOC_HASH) {
        int s = p->loc_hash[slot];
        /* A zero slot has never held a location. */
        if (s == 0) {
            int id;
            /* The location array itself is full. */
            if (p->nlocs >= CX_MAX_LOCS) {
                p->locs_capped = 1;
                return -1;
            }
            id = p->nlocs++;
            memset(&p->locs[id], 0, sizeof(LocInfo));
            p->locs[id].module = mod;
            p->locs[id].offset = offset;
            p->locs[id].block_id = -1;
            p->locs[id].func_id = -1;
            p->loc_hash[slot] = id + 1;
            return id;
        }
        /* This slot is the same module and offset. */
        if (p->locs[s - 1].module == mod && p->locs[s - 1].offset == offset) return s - 1;
        slot = (slot + 1) & mask;
        probes++;
    }
    p->locs_capped = 1;
    return -1;
}

/* List thread ids owned by pid into tids, up to cap. Uses one toolhelp
   snapshot of every thread on the system. A failed snapshot returns 0.
   A thread id of 0 is skipped. Returns how many ids were stored. The
   snapshot is closed. Does not open or suspend the threads. */
static int snap_threads(DWORD pid, DWORD *tids, int cap) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    int n = 0;
    /* No snapshot, so there is no thread list to trust. */
    if (snap == INVALID_HANDLE_VALUE) return 0;
    memset(&te, 0, sizeof(te));
    te.dwSize = sizeof(te);
    /* Walk the snapshot when the first record is readable. */
    if (Thread32First(snap, &te)) {
        /* Keep reading until toolhelp says the list is done. */
        do {
            /* This thread belongs to the target and there is still room. */
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != 0 && n < cap)
                tids[n++] = te.th32ThreadID;
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

/* Snapshot the modules of pid into snaps, up to cap entries. Each module
   is interned on p. A module already known keeps the larger reported
   size. A failed snapshot returns 0. A module that cannot be interned
   is skipped. Returns how many snaps were stored. The snapshot is
   closed. Does not load symbols. */
static int snap_modules(Profile *p, DWORD pid, ModSnap *snaps, int cap) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    MODULEENTRY32W me;
    int n = 0;
    /* No module list for this process. */
    if (snap == INVALID_HANDLE_VALUE) return 0;
    memset(&me, 0, sizeof(me));
    me.dwSize = sizeof(me);
    /* Walk the module list when the first record is readable. */
    if (Module32FirstW(snap, &me)) {
        /* Record modules until the list ends or the caller array is full. */
        do {
            int id;
            /* The caller array cannot hold another range. */
            if (n >= cap) break;
            id = intern_module(p, me.szExePath, me.szModule, 0);
            /* Only a module that got an index becomes a snap range. */
            if (id >= 0) {
                /* Keep the larger size if this module was seen before. */
                if (me.modBaseSize > p->mods[id].size) p->mods[id].size = me.modBaseSize;
                snaps[n].mod = id;
                snaps[n].base = (unsigned long long)me.modBaseAddr;
                snaps[n].size = me.modBaseSize;
                n++;
            }
            me.dwSize = sizeof(me);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return n;
}

/* Find which snap range contains rip. A range with size 0 is skipped.
   The end is base plus size and is exclusive. On a hit, *off is rip
   minus the base and the module id is returned. Returns -1 when no
   range contains rip. Does not intern a module. */
static int find_snap(const ModSnap *snaps, int n, unsigned long long rip, unsigned long long *off) {
    int i;
    /* Test each recorded range in order. */
    for (i = 0; i < n; i++) {
        unsigned long long b = snaps[i].base;
        unsigned long long e;
        /* An empty range cannot contain an address. */
        if (!snaps[i].size) continue;
        e = b + (unsigned long long)snaps[i].size;
        /* rip is inside this half-open range. */
        if (rip >= b && rip < e) {
            *off = rip - b;
            return snaps[i].mod;
        }
    }
    return -1;
}

/* Read the instruction pointer of a suspended thread into *rip. The
   static g_context buffer is cleared and marked CONTEXT_AMD64_CONTROL.
   Returns 0 when GetThreadContext fails or the pointer is 0. Returns 1
   otherwise. Does not suspend or resume th. This is a 64-bit context
   only. g_context is shared, so calls must not overlap. */
static int read_rip(HANDLE th, unsigned long long *rip) {
    unsigned int flags;
    memset(g_context, 0, sizeof(g_context));
    flags = CONTEXT_AMD64_CONTROL;
    memcpy(g_context + CONTEXT_FLAGS_OFFSET, &flags, sizeof(flags));
    /* The context call failed, so there is no pointer to trust. */
    if (!GetThreadContext(th, g_context)) return 0;
    memcpy(rip, g_context + CONTEXT_RIP_OFFSET, sizeof(*rip));
    return *rip != 0;
}

/* Append one sample for rip. nsnaps is the length of g_snaps used to
   turn rip into a module offset. A miss interns the unmapped module and
   stores the raw address as the offset. Returns 0 and increments
   samples_dropped when that module cannot be created, the location
   table is full, or the sample table is full. requested is stored as
   given. A negative measured time is stored as 0. Returns 1 when the
   sample was appended. Does not suspend a thread. */
static int remember_sample(Profile *p, int run_index, DWORD tid, int nthreads,
                           int nsnaps, unsigned long long rip, int requested, long long measured) {
    int mod, loc;
    unsigned long long off = 0;
    mod = find_snap(g_snaps, nsnaps, rip, &off);
    /* An address outside every module is charged to the unmapped bucket. */
    if (mod < 0) {
        mod = intern_module(p, L"", L"unmapped", 1);
        off = rip;
    }
    /* The unmapped bucket could not be created. */
    if (mod < 0) {
        p->samples_dropped++;
        return 0;
    }
    loc = intern_loc(p, mod, off);
    /* No location slot, or the sample table is already full. */
    if (loc < 0 || p->nsamples >= p->sample_cap) {
        p->samples_dropped++;
        return 0;
    }
    /* Fill the next sample row. */
    {
        Sample *s = &p->samples[p->nsamples++];
        s->run = run_index;
        s->loc = loc;
        s->tid = tid;
        s->nthreads = nthreads;
        s->requested_us = requested;
        s->measured_us = measured;
        /* A backward counter reading is stored as no measured delay. */
        if (s->measured_us < 0) s->measured_us = 0;
    }
    return 1;
}

/* One injection. Modules are snapshotted before the suspend so the
   measured window is only the suspension. The recorded duration is the
   performance counter around that window, which is the delay the thread
   took. pid is the child. rng picks the thread and the requested length
   inside the configured pause range. Returns 0 when no thread is listed,
   the thread cannot be opened or suspended, or the instruction pointer
   cannot be read. A failed resume is noted and tried once more. The
   module snapshot is retried once. Returns 1 only when the sample was
   stored. Does not kill the process. */
static int inject_pause(Profile *p, int run_index, DWORD pid, Rng *rng) {
    int nthreads, nsnaps, pick, requested, got_rip;
    DWORD access, prev;
    HANDLE th;
    long long a, b;
    unsigned long long rip = 0;
    nthreads = snap_threads(pid, g_tids, CX_MAX_THREADS);
    /* Nothing to suspend. */
    if (nthreads < 1) return 0;
    nsnaps = snap_modules(p, pid, g_snaps, CX_MAX_MODS);
    /* A first empty module list is tried once more before the suspend. */
    if (nsnaps < 1) nsnaps = snap_modules(p, pid, g_snaps, CX_MAX_MODS);
    pick = rng_range(rng, 0, nthreads - 1);
    access = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED;
    th = OpenThread(access, FALSE, g_tids[pick]);
    /* The chosen thread could not be opened. */
    if (!th) {
        p->suspend_failures++;
        note_err(p);
        return 0;
    }
    prev = SuspendThread(th);
    /* Suspend failed. The handle is closed and no pause is held. */
    if (prev == 0xFFFFFFFFu) {
        p->suspend_failures++;
        note_err(p);
        CloseHandle(th);
        return 0;
    }
    a = qpc_now();
    got_rip = read_rip(th, &rip);
    /* The pointer is missing, but the requested pause is still held. */
    if (!got_rip) {
        p->context_failures++;
        note_err(p);
    }
    requested = rng_range(rng, p->opt->pause_min_us, p->opt->pause_max_us);
    sleep_us(requested);
    b = qpc_now();
    /* One resume failure is noted and the resume is attempted again. */
    if (ResumeThread(th) == 0xFFFFFFFFu) {
        note_err(p);
        ResumeThread(th);
    }
    CloseHandle(th);
    /* Without a pointer there is no sample to charge. */
    if (!got_rip) return 0;
    return remember_sample(p, run_index, g_tids[pick], nthreads, nsnaps, rip, requested, qpc_to_us(b - a));
}

/* One wave of the sampling profiler. Every live thread is stopped only
   long enough to read its instruction pointer. pid is the child. There
   is no added pause; requested_us is stored as 0 and measured_us is the
   context-read window. Returns how many samples were stored, or 0 when
   no thread was listed. A thread that cannot be opened or suspended is
   counted as a failure and skipped. The module snapshot is retried once.
   Does not draw a pause length. */
static int inject_sample_wave(Profile *p, int run_index, DWORD pid) {
    int nthreads, nsnaps, i, got = 0;
    DWORD access;
    nthreads = snap_threads(pid, g_tids, CX_MAX_THREADS);
    /* No threads means this wave records nothing. */
    if (nthreads < 1) return 0;
    nsnaps = snap_modules(p, pid, g_snaps, CX_MAX_MODS);
    /* Retry once when the first module snapshot came back empty. */
    if (nsnaps < 1) nsnaps = snap_modules(p, pid, g_snaps, CX_MAX_MODS);
    access = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED;
    /* Stop each listed thread only for the instruction-pointer read. */
    for (i = 0; i < nthreads; i++) {
        HANDLE th;
        DWORD prev;
        long long a, b;
        unsigned long long rip = 0;
        int got_rip;
        th = OpenThread(access, FALSE, g_tids[i]);
        /* This thread could not be opened. The wave continues. */
        if (!th) {
            p->suspend_failures++;
            note_err(p);
            continue;
        }
        prev = SuspendThread(th);
        /* Suspend failed, so close the handle and try the next thread. */
        if (prev == 0xFFFFFFFFu) {
            p->suspend_failures++;
            note_err(p);
            CloseHandle(th);
            continue;
        }
        a = qpc_now();
        got_rip = read_rip(th, &rip);
        b = qpc_now();
        /* A missing pointer is counted, and the thread is still resumed. */
        if (!got_rip) {
            p->context_failures++;
            note_err(p);
        }
        /* One resume failure is noted and the resume is attempted again. */
        if (ResumeThread(th) == 0xFFFFFFFFu) {
            note_err(p);
            ResumeThread(th);
        }
        CloseHandle(th);
        /* No pointer, so this thread contributes no sample. */
        if (!got_rip) continue;
        /* requested_us is 0 because sampling does not hold a pause. */
        if (remember_sample(p, run_index, g_tids[i], nthreads, nsnaps, rip, 0, qpc_to_us(b - a))) got++;
    }
    return got;
}

/* Semicolon-separated directories passed to SymInitialize and written to
   _NT_SYMBOL_PATH. symbolize rebuilds this from the directories of the
   mapped modules and truncates it to this buffer. */
static char g_symsearch[4096];

/* Fill function and line fields on each location from dbghelp. Modules
   are loaded at synthetic bases, and a location address is that base
   plus its offset, not the child's real address. The search path is the
   unique directories of mapped modules, truncated to g_symsearch, and
   it is also written to _NT_SYMBOL_PATH when non-empty. A missing DLL
   or a failed SymInitialize stores a note and returns without symbols.
   The first API error is kept. SymGetLine may be absent; names still
   load. Line displacement is ignored. Names and file paths are
   sanitized into the location buffers. SymCleanup runs on success and
   clears sym_ready. Does not classify groups or write a file. */
static void symbolize(Profile *p) {
    char *search = g_symsearch;
    HANDLE proc;
    int i, m, nsearch = 0;
    /* No symbol engine, so every location stays unnamed. */
    if (!ensure_dbghelp()) {
        str_copy(p->sym_note, (int)sizeof(p->sym_note), "dbghelp.dll did not load");
        return;
    }
    search[0] = 0;
    /* Collect each mapped module's directory once. */
    for (m = 0; m < p->nmods; m++) {
        char dir[520];
        int exists = 0, k;
        /* An unmapped or pathless module adds no search directory. */
        if (!p->mods[m].path_acp[0] || p->mods[m].is_unmapped) continue;
        dir_of_path(p->mods[m].path_acp, dir, (int)sizeof(dir));
        /* A path with no directory is not a search entry. */
        if (!dir[0]) continue;
        /* See whether this directory is already in the search list. */
        for (k = 0; search[k]; ) {
            int dn = str_len(dir);
            int matched = 1;
            int t;
            /* Compare this list entry with the candidate directory. */
            for (t = 0; t < dn; t++) if (search[k + t] != dir[t]) matched = 0;
            /* A match ends at the NUL or at a semicolon separator. */
            if (matched && (search[k + dn] == 0 || search[k + dn] == ';')) exists = 1;
            /* Advance to the next semicolon-separated entry. */
            while (search[k] && search[k] != ';') k++;
            /* Step over the separator when there is another entry. */
            if (search[k] == ';') k++;
        }
        /* This directory is already listed. */
        if (exists) continue;
        /* Separate a further directory from the ones already stored. */
        if (nsearch && nsearch + 1 < (int)sizeof(search)) search[nsearch++] = ';';
        /* Append the directory until the static buffer is one byte from full. */
        for (k = 0; dir[k] && nsearch + 1 < (int)sizeof(search); k++) search[nsearch++] = dir[k];
        search[nsearch] = 0;
    }
    /* Publish the path for dbghelp when at least one directory was kept. */
    if (search[0]) SetEnvironmentVariableA("_NT_SYMBOL_PATH", search);
    pSymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    proc = GetCurrentProcess();
    /* Initialization failure leaves every module unloaded. */
    if (!pSymInitialize(proc, search[0] ? search : NULL, FALSE)) {
        str_copy(p->sym_note, (int)sizeof(p->sym_note), "SymInitialize failed");
        p->first_api_err = p->first_api_err ? p->first_api_err : GetLastError();
        return;
    }
    p->sym_ready = 1;
    /* Repeat the search path through the API when that export exists. */
    if (pSymSetSearchPath && search[0]) pSymSetSearchPath(proc, search);
    /* Load each mapped image at a synthetic base, one 4 GiB slot apart. */
    for (m = 0; m < p->nmods; m++) {
        unsigned long long base, got;
        /* The unmapped bucket and a module with no path have no PDB to open. */
        if (p->mods[m].is_unmapped || !p->mods[m].path_acp[0]) continue;
        base = 0x200000000ULL + (unsigned long long)(m + 1) * 0x100000000ULL;
        got = pSymLoadModuleEx(proc, NULL, p->mods[m].path_acp, NULL, base, p->mods[m].size, NULL, 0);
        /* Remember the load error and leave this module without symbols. */
        if (!got) {
            p->mods[m].sym_err = (int)GetLastError();
            p->mods[m].sym_ok = 0;
            continue;
        }
        p->mods[m].sym_base = got;
        p->mods[m].sym_ok = 1;
    }
    /* Resolve each location against the module that was loaded for it. */
    for (i = 0; i < p->nlocs; i++) {
        LocInfo *loc = &p->locs[i];
        SYMBOL_INFO *sym;
        unsigned long long disp = 0, addr;
        IMAGEHLP_LINE64 line;
        DWORD line_disp = 0;
        ModInfo *mod;
        /* A stale module index cannot be symbolized. */
        if (loc->module < 0 || loc->module >= p->nmods) continue;
        mod = &p->mods[loc->module];
        /* This module did not load, so the location stays unnamed. */
        if (!mod->sym_ok) continue;
        addr = mod->sym_base + loc->offset;
        memset(g_symstore, 0, sizeof(g_symstore));
        sym = (SYMBOL_INFO *)g_symstore;
        sym->SizeOfStruct = (unsigned long)sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 1024;
        /* A non-empty undecorated name becomes the function label. */
        if (pSymFromAddr(proc, addr, &disp, sym) && sym->Name[0]) {
            str_copy(loc->func, CX_NAME, sym->Name);
            sanitize_label(loc->func);
            loc->has_func = loc->func[0] ? 1 : 0;
            /* The symbol start is stored as an offset from the synthetic base. */
            if (sym->Address >= mod->sym_base) loc->func_start = sym->Address - mod->sym_base;
            /* An address below the base cannot form that offset. */
            else loc->func_start = loc->offset;
        }
        /* Line lookup is optional; a missing export skips it. */
        if (pSymGetLine) {
            memset(&line, 0, sizeof(line));
            line.SizeOfStruct = sizeof(line);
            /* A real file and a positive line number mark the location. */
            if (pSymGetLine(proc, addr, &line_disp, &line) && line.FileName && line.LineNumber > 0) {
                str_copy(loc->file, CX_NAME, line.FileName);
                sanitize_label(loc->file);
                loc->line = (int)line.LineNumber;
                loc->has_line = 1;
            }
        }
    }
    str_copy(p->sym_note, (int)sizeof(p->sym_note), "dbghelp ok");
    pSymCleanup(proc);
    p->sym_ready = 0;
}

/* Join dir and name into dst with a backslash between them. cap is the
   wide-character capacity. A NULL dir or name is empty. A dir that
   already ends in '\\' or '/' does not gain another separator. Returns
   0 when cap is below 2 or the result does not fit. On a name that does
   not fit, the partial text may lack a new terminator. Returns 1 when
   the joined path is NUL-terminated in dst. */
static int w_join(wchar_t *dst, int cap, const wchar_t *dir, const wchar_t *name) {
    int n, i;
    /* Not enough room for a wide NUL and one unit. */
    if (cap < 2) return 0;
    w_copy(dst, cap, dir ? dir : L"");
    n = w_len(dst);
    /* Insert a separator when the directory does not already end with one. */
    if (n > 0 && dst[n - 1] != L'\\' && dst[n - 1] != L'/') {
        /* The separator itself would not fit. */
        if (n + 1 >= cap) return 0;
        dst[n++] = L'\\';
        dst[n] = 0;
    }
    /* A missing file name adds nothing after the directory. */
    if (!name) name = L"";
    /* Copy the file name, stopping before the last slot. */
    for (i = 0; name[i]; i++) {
        /* This character would overwrite the room reserved for the NUL. */
        if (n + 1 >= cap) return 0;
        dst[n++] = name[i];
    }
    dst[n] = 0;
    return 1;
}

/* Open dir\name for writing, creating or truncating the file. Returns
   INVALID_HANDLE_VALUE when the joined path does not fit in 700 wide
   units or CreateFileW fails. Sharing allows readers. Does not write
   bytes and does not create missing parent directories. */
static HANDLE open_out(const wchar_t *dir, const wchar_t *name) {
    wchar_t path[700];
    /* A path that does not fit is not opened. */
    if (!w_join(path, 700, dir, name)) return INVALID_HANDLE_VALUE;
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* Grow w[col] so it is at least the length of s. A NULL s counts as
   empty. col is the caller's column index. Does not write. */
static void width_add(int *w, int col, const char *s) {
    int n = str_len(s ? s : "");
    /* A longer cell widens the column. */
    if (n > w[col]) w[col] = n;
}

/* Measure the decimal form of v into column col. Uses g_fmt. */
static void width_i64(int *w, int col, long long v) {
    format_i64(g_fmt, (int)sizeof(g_fmt), v);
    width_add(w, col, g_fmt);
}

/* Measure the fixed-point form of v, or "na" when present is 0 or v is
   not finite. dec is the number of digits after the decimal point.
   Uses g_fmt. */
static void width_fixed(int *w, int col, double v, int dec, int present) {
    /* A missing value occupies the same width as the token that will be written. */
    if (!present || !is_finite(v)) width_add(w, col, "na");
    else /* Measure the formatted number. */ {
        format_fixed(g_fmt, (int)sizeof(g_fmt), v, dec);
        width_add(w, col, g_fmt);
    }
}

/* Measure the hex form of v, including the 0x prefix. Uses g_fmt. */
static void width_hex(int *w, int col, unsigned long long v) {
    format_hex(g_fmt, (int)sizeof(g_fmt), v);
    width_add(w, col, g_fmt);
}

/* Set the first n column widths to zero before a table is measured. */
static void widths_clear(int *w, int n) {
    int i;
    /* Each column starts with no content. */
    for (i = 0; i < n; i++) w[i] = 0;
}

/* Write one table cell padded to width. right places the padding on
   the left so numbers share their last digit. Text is padded on the
   right. Two spaces follow, unless last is set, so columns stay apart
   when a cell fills its width. A cell longer than width is written in
   full and the later columns on this row shift right. Does not write
   a newline. */
static void writer_cell(Writer *w, const char *s, int width, int right, int last) {
    int n = str_len(s ? s : "");
    int pad = width - n;
    /* A short cell is the only one that needs padding. */
    if (pad < 0) pad = 0;
    /* Numbers pad on the left. */
    if (right) {
        /* One space per missing digit. */
        while (pad--) writer_ch(w, ' ');
        writer_s(w, s ? s : "");
    } else /* Text pads on the right. */ {
        writer_s(w, s ? s : "");
        /* One space per missing character. */
        while (pad--) writer_ch(w, ' ');
    }
    /* Separate this column from the next. */
    if (!last) {
        writer_ch(w, ' ');
        writer_ch(w, ' ');
    }
}

/* Write v as a right-aligned decimal cell. Uses g_fmt. last is passed
   to writer_cell. */
static void writer_cell_i64(Writer *w, long long v, int width, int last) {
    format_i64(g_fmt, (int)sizeof(g_fmt), v);
    writer_cell(w, g_fmt, width, 1, last);
}

/* Write v as a right-aligned fixed-point cell, or "na" when present is
   0 or v is not finite. dec is the number of digits after the point.
   Uses g_fmt. */
static void writer_cell_fixed(Writer *w, double v, int dec, int present, int width, int last) {
    /* The missing token is aligned like a number. */
    if (!present || !is_finite(v)) writer_cell(w, "na", width, 1, last);
    else /* Format, then align. */ {
        format_fixed(g_fmt, (int)sizeof(g_fmt), v, dec);
        writer_cell(w, g_fmt, width, 1, last);
    }
}

/* Write v as a right-aligned hex cell, including the 0x prefix. Uses
   g_fmt. */
static void writer_cell_hex(Writer *w, unsigned long long v, int width, int last) {
    format_hex(g_fmt, (int)sizeof(g_fmt), v);
    writer_cell(w, g_fmt, width, 1, last);
}

/* Write a newline. writer_s turns the LF into CRLF. Does not flush
   except when the writer buffer is full. */
static void writer_nl(Writer *w) { writer_s(w, "\n"); }

/* Write a left-aligned header row. cells has n names. colw[i] is the
   width of that column, already including the header text. Two spaces
   separate columns. Writes a newline. */
static void writer_header(Writer *w, const char **cells, const int *colw, int n) {
    int c;
    /* Each name sits at the left of its column. */
    for (c = 0; c < n; c++) writer_cell(w, cells[c], colw[c], 0, c == n - 1);
    writer_nl(w);
}

/* Write v as a signed decimal. The text is formatted into the shared
   g_fmt buffer, so a nested format would overwrite it. Does not add a
   newline. A failed writer drops the text. */
static void writer_i64(Writer *w, long long v) {
    format_i64(g_fmt, (int)sizeof(g_fmt), v);
    writer_s(w, g_fmt);
}

/* Write the RUN table: a header and one row per run that started.
   Columns are the index, e2e microseconds, exit code, completed flag,
   timeout flag, and sample count. Numbers are right-aligned under the
   header. Runs that never started are skipped. Does not close w. */
static void write_run_rows(Writer *w, const Profile *p) {
    int colw[7];
    int r;
    const char *head[7];
    head[0] = "RUN";
    head[1] = "index";
    head[2] = "e2e_us";
    head[3] = "exit_code";
    head[4] = "completed";
    head[5] = "timed_out";
    head[6] = "samples";
    widths_clear(colw, 7);
    /* The header names take part in the column widths. */
    for (r = 0; r < 7; r++) width_add(colw, r, head[r]);
    /* Measure every started run before any row is written. */
    for (r = 0; r < p->nruns; r++) {
        const RunInfo *run = &p->runs[r];
        /* A run that did not start has no row. */
        if (!run->started) continue;
        width_add(colw, 0, "RUN");
        width_i64(colw, 1, r);
        width_i64(colw, 2, run->e2e_us);
        width_i64(colw, 3, run->exit_code);
        width_i64(colw, 4, run->completed);
        width_i64(colw, 5, run->timed_out);
        width_i64(colw, 6, run->samples);
    }
    writer_header(w, head, colw, 7);
    /* One ledger row per run that CreateProcess actually started. */
    for (r = 0; r < p->nruns; r++) {
        const RunInfo *run = &p->runs[r];
        /* A run that did not start has no row. */
        if (!run->started) continue;
        writer_cell(w, "RUN", colw[0], 0, 0);
        writer_cell_i64(w, r, colw[1], 0);
        writer_cell_i64(w, run->e2e_us, colw[2], 0);
        writer_cell_i64(w, run->exit_code, colw[3], 0);
        writer_cell_i64(w, run->completed, colw[4], 0);
        writer_cell_i64(w, run->timed_out, colw[5], 0);
        writer_cell_i64(w, run->samples, colw[6], 1);
        writer_nl(w);
    }
}

/* Write ledger_locations.txt for a causal run. The file gets a header,
   the run options, one RUN row per started run, and one PAUSE row per
   sample whose location is in range. measured_us and requested_us are
   both stored. Returns 1 when the writer had no error and 0 otherwise,
   including a failed open. Uses g_fmt for the offset. Does not fit the
   estimator and does not skip timed-out runs' samples. */
static int write_raw_ledger(Profile *p) {
    Writer w;
    HANDLE h = open_out(p->opt->out_dir, L"ledger_locations.txt");
    int i;
    writer_open(&w, h);
    writer_s(&w,
             "# causext raw performance ledger\n"
             "# One RUN line is one execution of the command.\n"
             "# e2e_us is wall time from the start of CreateProcess to process exit.\n"
             "# One PAUSE line is one suspension. measured_us is the length of that\n"
             "# suspension on the performance counter, and it is the value the\n"
             "# estimator sums. requested_us is the uniform draw. The thread id and\n"
             "# the number of threads alive are recorded so a later split by thread\n"
             "# is possible; the estimator pools a location across threads.\n"
             "# Timed-out runs are stored and left out of the fit. A non-zero exit\n"
             "# code is still a completed run.\n"
             "# rank_locations.txt is the estimator on this ledger.\n");
    writer_s(&w, "# seed ");
    writer_i64(&w, (long long)p->opt->seed);
    writer_s(&w, "\n# runs ");
    writer_i64(&w, p->opt->runs);
    writer_s(&w, " samples ");
    writer_i64(&w, p->opt->samples);
    writer_s(&w, " pause_us ");
    writer_i64(&w, p->opt->pause_min_us);
    writer_s(&w, " ");
    writer_i64(&w, p->opt->pause_max_us);
    writer_s(&w, " gap_us ");
    writer_i64(&w, p->opt->gap_min_us);
    writer_s(&w, " ");
    writer_i64(&w, p->opt->gap_max_us);
    writer_s(&w, "\n");
    write_run_rows(&w, p);
    /* Measure the PAUSE table, then write the header and the rows. */
    {
        int colw[8];
        const char *head[8];
        head[0] = "PAUSE";
        head[1] = "run";
        head[2] = "tid";
        head[3] = "nthreads";
        head[4] = "module";
        head[5] = "offset";
        head[6] = "requested_us";
        head[7] = "measured_us";
        widths_clear(colw, 8);
        /* Header names take part in the column widths. */
        for (i = 0; i < 8; i++) width_add(colw, i, head[i]);
        /* One width contribution per sample that still points at a location. */
        for (i = 0; i < p->nsamples; i++) {
            Sample *s = &p->samples[i];
            const char *mod = "mod";
            /* A sample with no location cannot name a module. */
            if (s->loc < 0 || s->loc >= p->nlocs) continue;
            /* Use the module short name when the index is still in range. */
            if (p->locs[s->loc].module >= 0 && p->locs[s->loc].module < p->nmods)
                mod = p->mods[p->locs[s->loc].module].name;
            width_add(colw, 0, "PAUSE");
            width_i64(colw, 1, s->run);
            width_i64(colw, 2, (long long)s->tid);
            width_i64(colw, 3, s->nthreads);
            width_add(colw, 4, mod);
            width_hex(colw, 5, p->locs[s->loc].offset);
            width_i64(colw, 6, s->requested_us);
            width_i64(colw, 7, s->measured_us);
        }
        writer_header(&w, head, colw, 8);
        /* One PAUSE row per stored sample that still points at a location. */
        for (i = 0; i < p->nsamples; i++) {
            Sample *s = &p->samples[i];
            const char *mod = "mod";
            /* A sample with no location cannot name a module. */
            if (s->loc < 0 || s->loc >= p->nlocs) continue;
            /* Use the module short name when the index is still in range. */
            if (p->locs[s->loc].module >= 0 && p->locs[s->loc].module < p->nmods)
                mod = p->mods[p->locs[s->loc].module].name;
            writer_cell(&w, "PAUSE", colw[0], 0, 0);
            writer_cell_i64(&w, s->run, colw[1], 0);
            writer_cell_i64(&w, (long long)s->tid, colw[2], 0);
            writer_cell_i64(&w, s->nthreads, colw[3], 0);
            writer_cell(&w, mod, colw[4], 0, 0);
            writer_cell_hex(&w, p->locs[s->loc].offset, colw[5], 0);
            writer_cell_i64(&w, s->requested_us, colw[6], 0);
            writer_cell_i64(&w, s->measured_us, colw[7], 1);
            writer_nl(&w);
        }
    }
    return writer_close(&w);
}

/* Recompute member counts and measured-pause totals for every function
   and block group. Counts are cleared first. A location adds one member
   to its function and block when those ids are in range. Totals add
   measured_us only for samples in a completed run. Timed-out and unstarted
   runs add nothing. Does not create groups and does not allocate. */
static void tally_groups(Profile *p) {
    int i;
    /* Clear function tallies before locations are counted again. */
    for (i = 0; i < p->nfuncs; i++) {
        p->funcs[i].members = 0;
        p->funcs[i].total_us = 0;
    }
    /* Clear block tallies the same way. */
    for (i = 0; i < p->nblocks; i++) {
        p->blocks[i].members = 0;
        p->blocks[i].total_us = 0;
    }
    /* Each location is one member of its function and of its block. */
    for (i = 0; i < p->nlocs; i++) {
        int f = p->locs[i].func_id;
        int b = p->locs[i].block_id;
        /* Ignore a function id that is missing or out of range. */
        if (f >= 0 && f < p->nfuncs) p->funcs[f].members++;
        /* Ignore a block id that is missing or out of range. */
        if (b >= 0 && b < p->nblocks) p->blocks[b].members++;
    }
    /* Add measured pause time from completed runs only. */
    for (i = 0; i < p->nsamples; i++) {
        Sample *s = &p->samples[i];
        int f, b;
        /* A sample outside a completed run does not enter a total. */
        if (s->run < 0 || s->run >= p->nruns || !p->runs[s->run].completed) continue;
        /* A sample with no location has no group to charge. */
        if (s->loc < 0 || s->loc >= p->nlocs) continue;
        f = p->locs[s->loc].func_id;
        b = p->locs[s->loc].block_id;
        /* Charge the function when its id is live. */
        if (f >= 0 && f < p->nfuncs) p->funcs[f].total_us += s->measured_us;
        /* Charge the block when its id is live. */
        if (b >= 0 && b < p->nblocks) p->blocks[b].total_us += s->measured_us;
    }
}

/* Write the block or function ledger named by filename. GRAIN_FUNC uses
   the function groups and header; any other grain uses the block groups
   and header. x is the dense pause matrix, row_of maps runs onto its
   rows, and ncols is its width. nrows is unused. PAUSE rows are cells
   strictly above zero in completed runs, rounded to integer microseconds.
   MEMBER rows sum measured_us at each location over completed runs.
   Returns the writer status, 0 when the open or a write failed. Uses
   g_fmt for offsets. Does not fit the estimator. */
static int write_derived_ledger(Profile *p, int grain, const wchar_t *filename, const int *row_of,
                               const double *x, int nrows, int ncols) {
    Writer w;
    HANDLE h = open_out(p->opt->out_dir, filename);
    Group *gs = (grain == GRAIN_FUNC) ? p->funcs : p->blocks;
    int ng = (grain == GRAIN_FUNC) ? p->nfuncs : p->nblocks;
    int r, c, i;
    char tmp[CX_LABEL];
    writer_open(&w, h);
    /* The function grain gets the function header. */
    if (grain == GRAIN_FUNC) {
        writer_s(&w,
                 "# causext derived ledger: functions\n"
                 "# Debug symbols combine instructions. Every instruction whose symbol\n"
                 "# is the same function in the same module adds its measured pause into\n"
                 "# that column. An instruction with no symbol stays its own column, named\n"
                 "# module+offset, so unknown code is not merged into one bin.\n");
    } else /* Every other grain is described as the block ledger. */ {
        writer_s(&w,
                 "# causext derived ledger: blocks\n"
                 "# Debug symbols combine instructions. A block is the source line of a\n"
                 "# function when the PDB has a line. When the PDB names a function and\n"
                 "# has no line, a block is a 16-byte window measured from the function\n"
                 "# start. An instruction with no symbol stays its own block.\n");
    }
    writer_s(&w,
             "# PAUSE totals include completed runs only. Those totals are the columns\n"
             "# the estimator sees. Timed-out runs are listed and left out. MEMBER lines\n"
             "# audit the combination: their measured_us values, over completed runs,\n"
             "# add up to the GROUP total.\n");
    write_run_rows(&w, p);
    /* Measure the PAUSE table of positive cells, then write it. */
    {
        int colw[4];
        const char *head[4];
        head[0] = "PAUSE";
        head[1] = "run";
        head[2] = "column";
        head[3] = "measured_us";
        widths_clear(colw, 4);
        /* Header names take part in the column widths. */
        for (c = 0; c < 4; c++) width_add(colw, c, head[c]);
        /* A positive cell of a completed run is one output row. */
        for (r = 0; r < p->nruns; r++) {
            int row;
            /* Timed-out and unstarted runs are not columns of the matrix. */
            if (!p->runs[r].completed) continue;
            row = row_of[r];
            /* A completed run with no dense row is skipped. */
            if (row < 0) continue;
            /* One width contribution per column that actually received pause time. */
            for (c = 0; c < ncols; c++) {
                double v = x[row * ncols + c];
                /* A zero or negative cell is not a pause. */
                if (!(v > 0)) continue;
                width_add(colw, 0, "PAUSE");
                width_i64(colw, 1, r);
                width_i64(colw, 2, c);
                width_i64(colw, 3, (long long)(v + 0.5));
            }
        }
        writer_header(&w, head, colw, 4);
        /* Emit a row only for a positive cell of a completed run. */
        for (r = 0; r < p->nruns; r++) {
            int row;
            /* Timed-out and unstarted runs are not columns of the matrix. */
            if (!p->runs[r].completed) continue;
            row = row_of[r];
            /* A completed run with no dense row is skipped. */
            if (row < 0) continue;
            /* One output row per column that actually received pause time. */
            for (c = 0; c < ncols; c++) {
                double v = x[row * ncols + c];
                long long us;
                /* A zero or negative cell is not a pause. */
                if (!(v > 0)) continue;
                us = (long long)(v + 0.5);
                writer_cell(&w, "PAUSE", colw[0], 0, 0);
                writer_cell_i64(&w, r, colw[1], 0);
                writer_cell_i64(&w, c, colw[2], 0);
                writer_cell_i64(&w, us, colw[3], 1);
                writer_nl(&w);
            }
        }
    }
    /* Measure the GROUP table, then write it. */
    {
        int colw[5];
        const char *head[5];
        head[0] = "GROUP";
        head[1] = "id";
        head[2] = "members";
        head[3] = "total_us";
        head[4] = "label";
        widths_clear(colw, 5);
        /* Header names take part in the column widths. */
        for (c = 0; c < 5; c++) width_add(colw, c, head[c]);
        /* One width contribution per interned function or block. */
        for (c = 0; c < ng; c++) {
            width_add(colw, 0, "GROUP");
            width_i64(colw, 1, c);
            width_i64(colw, 2, gs[c].members);
            width_i64(colw, 3, gs[c].total_us);
            width_add(colw, 4, gs[c].label);
        }
        writer_header(&w, head, colw, 5);
        /* One GROUP row per interned function or block. */
        for (c = 0; c < ng; c++) {
            writer_cell(&w, "GROUP", colw[0], 0, 0);
            writer_cell_i64(&w, c, colw[1], 0);
            writer_cell_i64(&w, gs[c].members, colw[2], 0);
            writer_cell_i64(&w, gs[c].total_us, colw[3], 0);
            writer_cell(&w, gs[c].label, colw[4], 0, 1);
            writer_nl(&w);
        }
    }
    /* Measure the MEMBER table, then write it. The member total is computed twice so the width pass and the write pass agree. */
    {
        int colw[5];
        const char *head[5];
        head[0] = "MEMBER";
        head[1] = "group";
        head[2] = "module";
        head[3] = "offset";
        head[4] = "measured_us";
        widths_clear(colw, 5);
        /* Header names take part in the column widths. */
        for (c = 0; c < 5; c++) width_add(colw, c, head[c]);
        /* One width contribution per location that belongs to a group. */
        for (i = 0; i < p->nlocs; i++) {
            int g = (grain == GRAIN_FUNC) ? p->locs[i].func_id : p->locs[i].block_id;
            long long sum = 0;
            int s;
            const char *mod = "mod";
            /* A location that was not grouped has nothing to audit. */
            if (g < 0) continue;
            /* Sum this location's measured pause over completed runs. */
            for (s = 0; s < p->nsamples; s++) {
                /* This sample landed somewhere else. */
                if (p->samples[s].loc != i) continue;
                /* A sample with a bad run index is not part of the audit. */
                if (p->samples[s].run < 0 || p->samples[s].run >= p->nruns) continue;
                /* Timed-out runs are left out of the member total. */
                if (!p->runs[p->samples[s].run].completed) continue;
                sum += p->samples[s].measured_us;
            }
            /* Use the module short name when the index is still in range. */
            if (p->locs[i].module >= 0 && p->locs[i].module < p->nmods)
                mod = p->mods[p->locs[i].module].name;
            width_add(colw, 0, "MEMBER");
            width_i64(colw, 1, g);
            width_add(colw, 2, mod);
            width_hex(colw, 3, p->locs[i].offset);
            width_i64(colw, 4, sum);
        }
        writer_header(&w, head, colw, 5);
        /* One MEMBER row per location that belongs to a group. */
        for (i = 0; i < p->nlocs; i++) {
            int g = (grain == GRAIN_FUNC) ? p->locs[i].func_id : p->locs[i].block_id;
            long long sum = 0;
            int s;
            const char *mod = "mod";
            /* A location that was not grouped has nothing to audit. */
            if (g < 0) continue;
            /* Sum this location's measured pause over completed runs. */
            for (s = 0; s < p->nsamples; s++) {
                /* This sample landed somewhere else. */
                if (p->samples[s].loc != i) continue;
                /* A sample with a bad run index is not part of the audit. */
                if (p->samples[s].run < 0 || p->samples[s].run >= p->nruns) continue;
                /* Timed-out runs are left out of the member total. */
                if (!p->runs[p->samples[s].run].completed) continue;
                sum += p->samples[s].measured_us;
            }
            /* Use the module short name when the index is still in range. */
            if (p->locs[i].module >= 0 && p->locs[i].module < p->nmods)
                mod = p->mods[p->locs[i].module].name;
            writer_cell(&w, "MEMBER", colw[0], 0, 0);
            writer_cell_i64(&w, g, colw[1], 0);
            writer_cell(&w, mod, colw[2], 0, 0);
            writer_cell_hex(&w, p->locs[i].offset, colw[3], 0);
            writer_cell_i64(&w, sum, colw[4], 1);
            writer_nl(&w);
        }
    }
    (void)tmp;
    (void)nrows;
    return writer_close(&w);
}

/* Return the rank-file paragraph that names this grain. GRAIN_LOC and
   GRAIN_BLOCK have their own text. Any other grain, including
   GRAIN_FUNC, gets the function paragraph. The pointer is a literal and
   must not be freed. Does not look at the profile. */
static const char *grain_intro(int grain) {
    /* The location grain is one instruction per column. */
    if (grain == GRAIN_LOC)
        return "Grain: location. Each column is one instruction, named module+offset.\n\n";
    /* The block grain is a source line or a 16-byte window. */
    if (grain == GRAIN_BLOCK)
        return "Grain: block. A column sums instructions that share a source line in one\n"
               "function. With a function and no line, a column is a 16-byte window from\n"
               "the function start. With no symbol, the instruction stays alone.\n\n";
    return "Grain: function. A column sums instructions with the same function symbol\n"
           "in one module. With no symbol, the instruction stays alone.\n\n";
}

/* Fill idx with column indexes and set *n_out to how many. joint_only
   keeps columns in the joint model and sorts them by impact descending.
   Otherwise it keeps columns that are not in the joint model and sorts
   them by total pause descending. Equal keys keep the smaller index
   first. idx must hold ncols entries. Insertion sort. Does not allocate
   and does not modify cols. */
static void order_cols(const ColOut *cols, int ncols, int joint_only, int *idx, int *n_out) {
    int n = 0, k;
    /* Keep only the side of the table this call is ordering. */
    for (k = 0; k < ncols; k++) {
        /* The joint listing skips columns that were not admitted. */
        if (joint_only && !cols[k].in_joint) continue;
        /* The observed listing skips columns that were admitted. */
        if (!joint_only && cols[k].in_joint) continue;
        idx[n++] = k;
    }
    /* Insertion sort the selected indexes. */
    for (k = 1; k < n; k++) {
        int id = idx[k];
        int t = k;
        /* Shift indexes that should sort after id one slot to the right. */
        while (t > 0) {
            int prev = idx[t - 1];
            int move = 0;
            /* Joint rows order by impact, high first. */
            if (joint_only) {
                /* A smaller impact moves right. */
                if (cols[prev].impact < cols[id].impact) move = 1;
                /* Equal impact keeps the lower column index first. */
                else if (cols[prev].impact == cols[id].impact && prev > id) move = 1;
            } else /* Observed rows sort by total pause, not by impact. */ {
                /* A smaller total moves right. */
                if (cols[prev].total_x < cols[id].total_x) move = 1;
                /* Equal totals keep the lower column index first. */
                else if (cols[prev].total_x == cols[id].total_x && prev > id) move = 1;
            }
            /* This predecessor already belongs in front of id. */
            if (!move) break;
            idx[t] = prev;
            t--;
        }
        idx[t] = id;
    }
    *n_out = n;
}

/* Write one causal rank file. filename is under the profile output
   directory. The body is the grain introduction, ESTIMATOR_DOC, a FIT
   line, optional notes, a TABLE of joint columns by impact, and
   OBSERVED rows for columns left out of the joint model. ncols above
   CX_MAX_LOCS is truncated. Uses g_rank_idx and g_fmt through the
   writers. Returns the writer status. Does not refit. */
static int write_rank(Profile *p, int grain, const wchar_t *filename, const ColOut *cols,
                      const FitOut *info) {
    Writer w;
    HANDLE h = open_out(p->opt->out_dir, filename);
    int n, i, ncols;
    char tmp[CX_LABEL];
    writer_open(&w, h);
    writer_s(&w, grain_intro(grain));
    writer_s(&w, ESTIMATOR_DOC);
    /* One FIT header and one FIT row, padded to the same column widths. */
    {
        int colw[12];
        int c;
        const char *head[12];
        head[0] = "FIT";
        head[1] = "nruns";
        head[2] = "ncols";
        head[3] = "njoint";
        head[4] = "lambda";
        head[5] = "r2";
        head[6] = "mean_e2e_us";
        head[7] = "permutations";
        head[8] = "min_hits";
        head[9] = "capped";
        head[10] = "relaxed";
        head[11] = "too_small";
        widths_clear(colw, 12);
        /* Header names take part in the column widths. */
        for (c = 0; c < 12; c++) width_add(colw, c, head[c]);
        width_add(colw, 0, "FIT");
        width_i64(colw, 1, info->nruns);
        width_i64(colw, 2, info->ncols);
        width_i64(colw, 3, info->njoint);
        width_fixed(colw, 4, info->lambda, 6, info->lambda >= 0);
        width_fixed(colw, 5, info->r2, 4, !info->too_small);
        width_fixed(colw, 6, info->mean_y, 1, !info->too_small);
        width_i64(colw, 7, info->permutations);
        width_i64(colw, 8, info->min_hits);
        width_i64(colw, 9, info->capped);
        width_i64(colw, 10, info->relaxed);
        width_i64(colw, 11, info->too_small);
        writer_s(&w, "\n");
        writer_header(&w, head, colw, 12);
        writer_cell(&w, "FIT", colw[0], 0, 0);
        writer_cell_i64(&w, info->nruns, colw[1], 0);
        writer_cell_i64(&w, info->ncols, colw[2], 0);
        writer_cell_i64(&w, info->njoint, colw[3], 0);
        writer_cell_fixed(&w, info->lambda, 6, info->lambda >= 0, colw[4], 0);
        writer_cell_fixed(&w, info->r2, 4, !info->too_small, colw[5], 0);
        writer_cell_fixed(&w, info->mean_y, 1, !info->too_small, colw[6], 0);
        writer_cell_i64(&w, info->permutations, colw[7], 0);
        writer_cell_i64(&w, info->min_hits, colw[8], 0);
        writer_cell_i64(&w, info->capped, colw[9], 0);
        writer_cell_i64(&w, info->relaxed, colw[10], 0);
        writer_cell_i64(&w, info->too_small, colw[11], 1);
        writer_nl(&w);
    }
    /* Say why the joint model was skipped when the fit said it was too small. */
    if (info->too_small)
        writer_s(&w, "The joint model was not fit. Fewer than 4 completed runs were available, or no column carried a pause.\n");
    /* Say that admission stopped at the 96-column cap. */
    if (info->capped)
        writer_s(&w, "The joint model kept the 96 admitted columns with the largest total pause.\n");
    /* Say that the visit minimum dropped to one. */
    if (info->relaxed)
        writer_s(&w, "The visit minimum was relaxed to 1 because no column met the ordinary minimum.\n");
    /* Say that distinct instructions were dropped after the location cap. */
    if (p->locs_capped)
        writer_s(&w, "Location capacity was reached. Further distinct instructions were not recorded.\n");
    writer_s(&w, "TABLE\n");
    ncols = info->ncols;
    /* The shared rank index cannot address more columns than it holds. */
    if (ncols > CX_MAX_LOCS) ncols = CX_MAX_LOCS;
    order_cols(cols, ncols, 1, g_rank_idx, &n);
    /* Measure the joint-model table, then write the header and the rows. */
    {
        int colw[11];
        int hi;
        const char *head[11];
        head[0] = "rank";
        head[1] = "impact_us";
        head[2] = "ridge_beta";
        head[3] = "fwl_beta";
        head[4] = "fwl_se";
        head[5] = "perm_p";
        head[6] = "mean_pause_us";
        head[7] = "runs_hit";
        head[8] = "stable";
        head[9] = "in_joint";
        head[10] = "label";
        widths_clear(colw, 11);
        /* Header names take part in the column widths. */
        for (hi = 0; hi < 11; hi++) width_add(colw, hi, head[hi]);
        /* TABLE rows are the joint columns, already ordered by impact. */
        for (i = 0; i < n; i++) {
            const ColOut *c = &cols[g_rank_idx[i]];
            const char *lab = label_of(p, grain, c->index, tmp, CX_LABEL);
            width_i64(colw, 0, i + 1);
            width_fixed(colw, 1, c->impact, 2, 1);
            width_fixed(colw, 2, c->ridge, 4, 1);
            width_fixed(colw, 3, c->fwl, 4, c->identified);
            width_fixed(colw, 4, c->fwl_se, 4, c->identified && c->fwl_se >= 0);
            width_fixed(colw, 5, c->perm_p, 4, c->perm_p >= 0);
            width_fixed(colw, 6, c->mean_x, 2, 1);
            width_i64(colw, 7, c->runs_hit);
            width_i64(colw, 8, c->stable);
            width_i64(colw, 9, c->in_joint);
            width_add(colw, 10, lab);
        }
        writer_header(&w, head, colw, 11);
        /* Write the same rows in the same order. */
        for (i = 0; i < n; i++) {
            const ColOut *c = &cols[g_rank_idx[i]];
            const char *lab = label_of(p, grain, c->index, tmp, CX_LABEL);
            writer_cell_i64(&w, i + 1, colw[0], 0);
            writer_cell_fixed(&w, c->impact, 2, 1, colw[1], 0);
            writer_cell_fixed(&w, c->ridge, 4, 1, colw[2], 0);
            writer_cell_fixed(&w, c->fwl, 4, c->identified, colw[3], 0);
            writer_cell_fixed(&w, c->fwl_se, 4, c->identified && c->fwl_se >= 0, colw[4], 0);
            writer_cell_fixed(&w, c->perm_p, 4, c->perm_p >= 0, colw[5], 0);
            writer_cell_fixed(&w, c->mean_x, 2, 1, colw[6], 0);
            writer_cell_i64(&w, c->runs_hit, colw[7], 0);
            writer_cell_i64(&w, c->stable, colw[8], 0);
            writer_cell_i64(&w, c->in_joint, colw[9], 0);
            writer_cell(&w, lab, colw[10], 0, 1);
            writer_nl(&w);
        }
    }
    writer_s(&w, "ENDTABLE\n");
    order_cols(cols, ncols, 0, g_rank_idx, &n);
    /* Measure the columns the joint model left out, then write them. */
    {
        int colw[4];
        int hi;
        const char *head[4];
        head[0] = "OBSERVED";
        head[1] = "total_pause_us";
        head[2] = "runs_hit";
        head[3] = "label";
        widths_clear(colw, 4);
        /* Header names take part in the column widths. */
        for (hi = 0; hi < 4; hi++) width_add(colw, hi, head[hi]);
        /* OBSERVED rows are the columns the joint model did not admit. */
        for (i = 0; i < n; i++) {
            const ColOut *c = &cols[g_rank_idx[i]];
            const char *lab = label_of(p, grain, c->index, tmp, CX_LABEL);
            width_add(colw, 0, "OBSERVED");
            width_i64(colw, 1, c->total_x);
            width_i64(colw, 2, c->runs_hit);
            width_add(colw, 3, lab);
        }
        writer_header(&w, head, colw, 4);
        /* Write those rows in the measured order. */
        for (i = 0; i < n; i++) {
            const ColOut *c = &cols[g_rank_idx[i]];
            const char *lab = label_of(p, grain, c->index, tmp, CX_LABEL);
            writer_cell(&w, "OBSERVED", colw[0], 0, 0);
            writer_cell_i64(&w, c->total_x, colw[1], 0);
            writer_cell_i64(&w, c->runs_hit, colw[2], 0);
            writer_cell(&w, lab, colw[3], 0, 1);
            writer_nl(&w);
        }
    }
    return writer_close(&w);
}

/* Write symbols.txt: the dbghelp struct sizes, the symbol note, one
   MODULE row per module, and one LOC row per location. A missing note
   is written as "not run". A location with no function or no line writes
   a dash for that field. Returns the writer status. Uses g_fmt for the
   offset. Does not call dbghelp. */
static int write_symbols(Profile *p) {
    Writer w;
    HANDLE h = open_out(p->opt->out_dir, L"symbols.txt");
    int i;
    writer_open(&w, h);
    writer_s(&w, "sizeof_SYMBOL_INFO ");
    writer_i64(&w, (long long)sizeof(SYMBOL_INFO));
    writer_nl(&w);
    writer_s(&w, "sizeof_IMAGEHLP_LINE64 ");
    writer_i64(&w, (long long)sizeof(IMAGEHLP_LINE64));
    writer_nl(&w);
    writer_s(&w, "note ");
    writer_s(&w, p->sym_note[0] ? p->sym_note : "not run");
    writer_nl(&w);
    /* Measure the MODULE rows, then write them aligned. */
    {
        int colw[7];
        widths_clear(colw, 7);
        /* One width contribution per interned module. */
        for (i = 0; i < p->nmods; i++) {
            width_add(colw, 0, "MODULE");
            width_i64(colw, 1, i);
            width_add(colw, 2, p->mods[i].sym_ok ? "ok" : "unloaded");
            width_i64(colw, 3, p->mods[i].sym_err);
            width_i64(colw, 4, p->mods[i].size);
            width_add(colw, 5, p->mods[i].name);
            width_add(colw, 6, p->mods[i].path_acp);
        }
        /* One MODULE row for every module the profile interned. */
        for (i = 0; i < p->nmods; i++) {
            writer_cell(&w, "MODULE", colw[0], 0, 0);
            writer_cell_i64(&w, i, colw[1], 0);
            writer_cell(&w, p->mods[i].sym_ok ? "ok" : "unloaded", colw[2], 0, 0);
            writer_cell_i64(&w, p->mods[i].sym_err, colw[3], 0);
            writer_cell_i64(&w, p->mods[i].size, colw[4], 0);
            writer_cell(&w, p->mods[i].name, colw[5], 0, 0);
            writer_cell(&w, p->mods[i].path_acp, colw[6], 0, 1);
            writer_nl(&w);
        }
    }
    /* Measure the LOC rows, then write them aligned. */
    {
        int colw[9];
        widths_clear(colw, 9);
        /* One width contribution per recorded instruction. */
        for (i = 0; i < p->nlocs; i++) {
            LocInfo *loc = &p->locs[i];
            const char *mod = "mod";
            /* Use the module short name when the index is still in range. */
            if (loc->module >= 0 && loc->module < p->nmods) mod = p->mods[loc->module].name;
            width_add(colw, 0, "LOC");
            width_i64(colw, 1, i);
            width_add(colw, 2, mod);
            width_hex(colw, 3, loc->offset);
            width_add(colw, 4, loc->has_func ? loc->func : "-");
            width_add(colw, 5, loc->has_line ? loc->file : "-");
            width_i64(colw, 6, loc->has_line ? loc->line : 0);
            width_i64(colw, 7, loc->func_id);
            width_i64(colw, 8, loc->block_id);
        }
        /* One LOC row for every recorded instruction. */
        for (i = 0; i < p->nlocs; i++) {
            LocInfo *loc = &p->locs[i];
            const char *mod = "mod";
            /* Use the module short name when the index is still in range. */
            if (loc->module >= 0 && loc->module < p->nmods) mod = p->mods[loc->module].name;
            writer_cell(&w, "LOC", colw[0], 0, 0);
            writer_cell_i64(&w, i, colw[1], 0);
            writer_cell(&w, mod, colw[2], 0, 0);
            writer_cell_hex(&w, loc->offset, colw[3], 0);
            writer_cell(&w, loc->has_func ? loc->func : "-", colw[4], 0, 0);
            writer_cell(&w, loc->has_line ? loc->file : "-", colw[5], 0, 0);
            writer_cell_i64(&w, loc->has_line ? loc->line : 0, colw[6], 0);
            writer_cell_i64(&w, loc->func_id, colw[7], 0);
            writer_cell_i64(&w, loc->block_id, colw[8], 1);
            writer_nl(&w);
        }
    }
    return writer_close(&w);
}

/* Print up to eight joint-model rows for grain to standard output,
   highest impact first. A fit that is too small or has no joint columns
   prints nothing. The line names the rank, impact, ridge beta, whether
   the row is stable, and the label. Uses g_rank_idx and g_fmt. Does not
   write the rank file. */
static void print_top(const Profile *p, int grain, const ColOut *cols, const FitOut *info) {
    int n, i, shown = 0;
    char tmp[CX_LABEL];
    char line[480];
    /* Nothing was fit, so there is no ranking to show. */
    if (info->too_small || info->njoint < 1) return;
    order_cols(cols, info->ncols, 1, g_rank_idx, &n);
    con_out(grain == GRAIN_FUNC ? "Top functions by impact on end-to-end time:\n" :
            grain == GRAIN_BLOCK ? "Top blocks by impact on end-to-end time:\n" :
                                   "Top locations by impact on end-to-end time:\n");
    /* Show at most eight rows, in the impact order order_cols produced. */
    for (i = 0; i < n && shown < 8; i++) {
        const ColOut *c = &cols[g_rank_idx[i]];
        const char *lab = label_of(p, grain, c->index, tmp, CX_LABEL);
        line[0] = 0;
        str_append(line, (int)sizeof(line), "  ");
        format_i64(g_fmt, (int)sizeof(g_fmt), i + 1);
        str_append(line, (int)sizeof(line), g_fmt);
        str_append(line, (int)sizeof(line), "  impact_us ");
        format_fixed(g_fmt, (int)sizeof(g_fmt), c->impact, 1);
        str_append(line, (int)sizeof(line), g_fmt);
        str_append(line, (int)sizeof(line), "  beta ");
        format_fixed(g_fmt, (int)sizeof(g_fmt), c->ridge, 3);
        str_append(line, (int)sizeof(line), g_fmt);
        str_append(line, (int)sizeof(line), c->stable ? "  stable  " : "  unstable  ");
        str_append(line, (int)sizeof(line), lab);
        str_append(line, (int)sizeof(line), "\n");
        con_out(line);
        shown++;
    }
}

/* Sampling rank-file header. write_sample_rank copies these bytes ahead
   of the SAMPLES and TABLE sections. The self-test requires the text
   "Sampling profile" to remain. The array is the description of sample
   share, which is a different ranking from ESTIMATOR_DOC. */
static const char SAMPLER_DOC[] =
"Sampling profile\n"
"----------------\n"
"This file ranks code by how often a thread was observed there. It is the\n"
"ordinary sampling profiler, kept beside the causal ranks so the two can be\n"
"compared. A large share means threads were found at that code. It does not\n"
"mean the command's end-to-end time depends on that code. A slack thread\n"
"that spins for the whole run can take as many samples as the critical path\n"
"and still leave the end-to-end time unchanged when it is paused.\n"
"\n"
"Each wave lists the command's threads and suspends each one only long\n"
"enough to read the instruction pointer, then resumes it. There is no\n"
"added pause. Waiting threads are included, so a thread blocked in the\n"
"kernel is counted at the instruction where it waits. The percent column\n"
"is that column's samples divided by every sample in the completed runs.\n"
"Those percents add to 100. The causal impact column does not: several\n"
"columns can describe the same pause.\n"
"\n"
"Locations, blocks, and functions are the same grains as the causal ledgers.\n"
"A block is the source line of a function when the PDB has a line. With a\n"
"function and no line, a block is a 16-byte window from the function start.\n"
"An instruction with no symbol stays its own block and its own function.\n";

/* Count completed-run samples into g_sample_counts for grain. ncols is
   clamped to [0, CX_MAX_LOCS] and those slots are zeroed first. A sample
   whose run is out of range, not completed, or whose column is outside
   that range is ignored. The counts are static and overwritten. Does
   not allocate. */
static void count_grain(Profile *p, int grain, int ncols) {
    int i;
    /* A negative width is an empty count. */
    if (ncols < 0) ncols = 0;
    /* The static table cannot hold more columns than this. */
    if (ncols > CX_MAX_LOCS) ncols = CX_MAX_LOCS;
    /* Clear the slots this grain will use. */
    for (i = 0; i < ncols; i++) g_sample_counts[i] = 0;
    /* Each completed-run sample increments one column. */
    for (i = 0; i < p->nsamples; i++) {
        Sample *s = &p->samples[i];
        int id;
        /* Skip a sample that is not part of a completed run. */
        if (s->run < 0 || s->run >= p->nruns || !p->runs[s->run].completed) continue;
        id = col_of(p, s->loc, grain);
        /* Ignore a missing column and one past the clamped width. */
        if (id >= 0 && id < ncols) g_sample_counts[id]++;
    }
}

/* Sum g_sample_counts over ncols columns, clamped to CX_MAX_LOCS.
   Returns that sum. Does not check that count_grain ran. A negative
   ncols is not clamped and the loop does not run, so the return is 0. */
static int sample_total(int ncols) {
    int i, n = 0;
    /* Do not read past the static count table. */
    if (ncols > CX_MAX_LOCS) ncols = CX_MAX_LOCS;
    /* Add every counted column. A negative ncols adds nothing. */
    for (i = 0; i < ncols; i++) n += g_sample_counts[i];
    return n;
}

/* Fill idx with columns whose sample count is positive and set *n_out.
   ncols above CX_MAX_LOCS is clamped. The order is count descending, and
   equal counts keep the smaller index first. idx must hold ncols entries.
   Reads g_sample_counts. Insertion sort. Does not allocate. */
static void order_sample_cols(int ncols, int *idx, int *n_out) {
    int n = 0, k;
    /* Do not index past the static count table. */
    if (ncols > CX_MAX_LOCS) ncols = CX_MAX_LOCS;
    /* A column with no samples is left out of the rank. */
    for (k = 0; k < ncols; k++) if (g_sample_counts[k] > 0) idx[n++] = k;
    /* Insertion sort by sample count, high first. */
    for (k = 1; k < n; k++) {
        int id = idx[k];
        int t = k;
        /* Shift counts that belong after id one slot to the right. */
        while (t > 0) {
            int prev = idx[t - 1];
            int move = 0;
            /* A smaller count moves right. */
            if (g_sample_counts[prev] < g_sample_counts[id]) move = 1;
            /* Equal counts keep the lower column index first. */
            else if (g_sample_counts[prev] == g_sample_counts[id] && prev > id) move = 1;
            /* This predecessor already belongs in front of id. */
            if (!move) break;
            idx[t] = prev;
            t--;
        }
        idx[t] = id;
    }
    *n_out = n;
}

/* Write the sampling ledger_locations.txt. It has a header, the seed
   and wave counts, RUN rows, and one SAMPLE row per sample with a live
   location. measured_us is the pointer-read window. Returns the writer
   status. Uses g_fmt. Does not rank the samples. */
static int write_sample_raw(Profile *p) {
    Writer w;
    HANDLE h = open_out(p->opt->out_dir, L"ledger_locations.txt");
    int i;
    writer_open(&w, h);
    writer_s(&w,
             "# causext sampling ledger\n"
             "# One RUN line is one execution of the command.\n"
             "# e2e_us is wall time from CreateProcess to process exit. The\n"
             "# sampler does not hold threads, so this time is the command's own\n"
             "# length plus the cost of the snapshots.\n"
             "# One SAMPLE line is one thread observed in one wave. measured_us is\n"
             "# only the time spent reading the instruction pointer.\n"
             "# rank_locations.txt ranks these observations by sample share.\n");
    writer_s(&w, "# seed ");
    writer_i64(&w, (long long)p->opt->seed);
    writer_s(&w, "\n# runs ");
    writer_i64(&w, p->opt->runs);
    writer_s(&w, " waves ");
    writer_i64(&w, p->opt->samples);
    writer_s(&w, "\n");
    write_run_rows(&w, p);
    /* Measure the SAMPLE table, then write the header and the rows. */
    {
        int colw[7];
        const char *head[7];
        head[0] = "SAMPLE";
        head[1] = "run";
        head[2] = "tid";
        head[3] = "nthreads";
        head[4] = "module";
        head[5] = "offset";
        head[6] = "measured_us";
        widths_clear(colw, 7);
        /* Header names take part in the column widths. */
        for (i = 0; i < 7; i++) width_add(colw, i, head[i]);
        /* One width contribution per observation that still has a location. */
        for (i = 0; i < p->nsamples; i++) {
            Sample *s = &p->samples[i];
            const char *mod = "mod";
            /* A sample with no location cannot name a module. */
            if (s->loc < 0 || s->loc >= p->nlocs) continue;
            /* Use the module short name when the index is still in range. */
            if (p->locs[s->loc].module >= 0 && p->locs[s->loc].module < p->nmods)
                mod = p->mods[p->locs[s->loc].module].name;
            width_add(colw, 0, "SAMPLE");
            width_i64(colw, 1, s->run);
            width_i64(colw, 2, (long long)s->tid);
            width_i64(colw, 3, s->nthreads);
            width_add(colw, 4, mod);
            width_hex(colw, 5, p->locs[s->loc].offset);
            width_i64(colw, 6, s->measured_us);
        }
        writer_header(&w, head, colw, 7);
        /* One SAMPLE row per stored observation that still has a location. */
        for (i = 0; i < p->nsamples; i++) {
            Sample *s = &p->samples[i];
            const char *mod = "mod";
            /* A sample with no location cannot name a module. */
            if (s->loc < 0 || s->loc >= p->nlocs) continue;
            /* Use the module short name when the index is still in range. */
            if (p->locs[s->loc].module >= 0 && p->locs[s->loc].module < p->nmods)
                mod = p->mods[p->locs[s->loc].module].name;
            writer_cell(&w, "SAMPLE", colw[0], 0, 0);
            writer_cell_i64(&w, s->run, colw[1], 0);
            writer_cell_i64(&w, (long long)s->tid, colw[2], 0);
            writer_cell_i64(&w, s->nthreads, colw[3], 0);
            writer_cell(&w, mod, colw[4], 0, 0);
            writer_cell_hex(&w, p->locs[s->loc].offset, colw[5], 0);
            writer_cell_i64(&w, s->measured_us, colw[6], 1);
            writer_nl(&w);
        }
    }
    return writer_close(&w);
}

/* Write the sampling block or function ledger. GRAIN_FUNC selects
   ledger_functions.txt and the function groups. Any other grain selects
   ledger_blocks.txt and the block groups. GROUP rows use g_sample_counts
   and stop at CX_MAX_LOCS. MEMBER rows count completed-run samples at
   each grouped location. Returns the writer status. count_grain must
   already have filled the counts for this grain. */
static int write_sample_derived(Profile *p, int grain) {
    Writer w;
    const wchar_t *filename = (grain == GRAIN_FUNC) ? L"ledger_functions.txt" : L"ledger_blocks.txt";
    HANDLE h = open_out(p->opt->out_dir, filename);
    Group *gs = (grain == GRAIN_FUNC) ? p->funcs : p->blocks;
    int ng = ncols_of(p, grain);
    int c, i;
    writer_open(&w, h);
    /* The function grain gets the function header. */
    if (grain == GRAIN_FUNC) {
        writer_s(&w,
                 "# causext sampling ledger: functions\n"
                 "# samples is how many instruction observations fell in the function.\n"
                 "# rank_functions.txt sorts these rows by that count.\n");
    } else /* Every other grain is described as the block ledger. */ {
        writer_s(&w,
                 "# causext sampling ledger: blocks\n"
                 "# A block is the source line, or a 16-byte window when the PDB has\n"
                 "# a function and no line. samples counts observations in the block.\n");
    }
    /* Measure the GROUP table, then write it. */
    {
        int colw[5];
        const char *head[5];
        int shown = ng;
        head[0] = "GROUP";
        head[1] = "id";
        head[2] = "members";
        head[3] = "samples";
        head[4] = "label";
        /* The sample-count table cannot name a group past its last slot. */
        if (shown > CX_MAX_LOCS) shown = CX_MAX_LOCS;
        widths_clear(colw, 5);
        /* Header names take part in the column widths. */
        for (c = 0; c < 5; c++) width_add(colw, c, head[c]);
        /* One width contribution per group that fits in the sample-count table. */
        for (c = 0; c < shown; c++) {
            width_add(colw, 0, "GROUP");
            width_i64(colw, 1, c);
            width_i64(colw, 2, gs[c].members);
            width_i64(colw, 3, g_sample_counts[c]);
            width_add(colw, 4, gs[c].label);
        }
        writer_header(&w, head, colw, 5);
        /* One GROUP row per group that fits in the sample-count table. */
        for (c = 0; c < shown; c++) {
            writer_cell(&w, "GROUP", colw[0], 0, 0);
            writer_cell_i64(&w, c, colw[1], 0);
            writer_cell_i64(&w, gs[c].members, colw[2], 0);
            writer_cell_i64(&w, g_sample_counts[c], colw[3], 0);
            writer_cell(&w, gs[c].label, colw[4], 0, 1);
            writer_nl(&w);
        }
    }
    /* Measure the MEMBER table, then write it. */
    {
        int colw[5];
        const char *head[5];
        head[0] = "MEMBER";
        head[1] = "group";
        head[2] = "module";
        head[3] = "offset";
        head[4] = "samples";
        widths_clear(colw, 5);
        /* Header names take part in the column widths. */
        for (c = 0; c < 5; c++) width_add(colw, c, head[c]);
        /* One width contribution per location that belongs to a group. */
        for (i = 0; i < p->nlocs; i++) {
            int g = (grain == GRAIN_FUNC) ? p->locs[i].func_id : p->locs[i].block_id;
            int sum = 0, s;
            const char *mod = "mod";
            /* A location that was not grouped has nothing to list. */
            if (g < 0) continue;
            /* Count completed-run observations of this instruction. */
            for (s = 0; s < p->nsamples; s++) {
                /* This sample landed somewhere else. */
                if (p->samples[s].loc != i) continue;
                /* A sample with a bad run index is not counted. */
                if (p->samples[s].run < 0 || p->samples[s].run >= p->nruns) continue;
                /* Timed-out runs are left out of the member count. */
                if (!p->runs[p->samples[s].run].completed) continue;
                sum++;
            }
            /* Use the module short name when the index is still in range. */
            if (p->locs[i].module >= 0 && p->locs[i].module < p->nmods)
                mod = p->mods[p->locs[i].module].name;
            width_add(colw, 0, "MEMBER");
            width_i64(colw, 1, g);
            width_add(colw, 2, mod);
            width_hex(colw, 3, p->locs[i].offset);
            width_i64(colw, 4, sum);
        }
        writer_header(&w, head, colw, 5);
        /* One MEMBER row per location that belongs to a group. */
        for (i = 0; i < p->nlocs; i++) {
            int g = (grain == GRAIN_FUNC) ? p->locs[i].func_id : p->locs[i].block_id;
            int sum = 0, s;
            const char *mod = "mod";
            /* A location that was not grouped has nothing to list. */
            if (g < 0) continue;
            /* Count completed-run observations of this instruction. */
            for (s = 0; s < p->nsamples; s++) {
                /* This sample landed somewhere else. */
                if (p->samples[s].loc != i) continue;
                /* A sample with a bad run index is not counted. */
                if (p->samples[s].run < 0 || p->samples[s].run >= p->nruns) continue;
                /* Timed-out runs are left out of the member count. */
                if (!p->runs[p->samples[s].run].completed) continue;
                sum++;
            }
            /* Use the module short name when the index is still in range. */
            if (p->locs[i].module >= 0 && p->locs[i].module < p->nmods)
                mod = p->mods[p->locs[i].module].name;
            writer_cell(&w, "MEMBER", colw[0], 0, 0);
            writer_cell_i64(&w, g, colw[1], 0);
            writer_cell(&w, mod, colw[2], 0, 0);
            writer_cell_hex(&w, p->locs[i].offset, colw[3], 0);
            writer_cell_i64(&w, sum, colw[4], 1);
            writer_nl(&w);
        }
    }
    return writer_close(&w);
}

/* Print up to eight columns of grain by sample share. total is the
   denominator for the percent; zero yields 0 percent. Columns with no
   samples are omitted by order_sample_cols. Uses g_rank_idx, g_fmt, and
   g_sample_counts. Does not write the rank file. */
static void print_sample_top(const Profile *p, int grain, int total) {
    int n, i, shown = 0;
    char tmp[CX_LABEL];
    char line[480];
    order_sample_cols(ncols_of(p, grain), g_rank_idx, &n);
    con_out(grain == GRAIN_FUNC ? "Top functions by sample share:\n" :
            grain == GRAIN_BLOCK ? "Top blocks by sample share:\n" :
                                   "Top locations by sample share:\n");
    /* Show at most eight rows, highest share first. */
    for (i = 0; i < n && shown < 8; i++) {
        int id = g_rank_idx[i];
        const char *lab = label_of(p, grain, id, tmp, CX_LABEL);
        double pct = (total > 0) ? (100.0 * (double)g_sample_counts[id] / (double)total) : 0;
        line[0] = 0;
        str_append(line, (int)sizeof(line), "  ");
        format_i64(g_fmt, (int)sizeof(g_fmt), i + 1);
        str_append(line, (int)sizeof(line), g_fmt);
        str_append(line, (int)sizeof(line), "  samples ");
        format_i64(g_fmt, (int)sizeof(g_fmt), g_sample_counts[id]);
        str_append(line, (int)sizeof(line), g_fmt);
        str_append(line, (int)sizeof(line), "  percent ");
        format_fixed(g_fmt, (int)sizeof(g_fmt), pct, 1);
        str_append(line, (int)sizeof(line), g_fmt);
        str_append(line, (int)sizeof(line), "  ");
        str_append(line, (int)sizeof(line), lab);
        str_append(line, (int)sizeof(line), "\n");
        con_out(line);
        shown++;
    }
}

/* Write the sampling rank file for grain. The location grain uses
   rank_locations.txt, blocks use rank_blocks.txt, and functions use
   rank_functions.txt. total is the sample denominator for percents.
   The body is the grain introduction, SAMPLER_DOC, a SAMPLES line, an
   optional cap note, and a TABLE of columns with a positive count.
   Returns the writer status. Does not recount samples. */
static int write_sample_rank(Profile *p, int grain, int total) {
    Writer w;
    const wchar_t *filename = L"rank_locations.txt";
    HANDLE h;
    int n, i, ncols;
    char tmp[CX_LABEL];
    /* Blocks and functions each have their own rank file name. */
    if (grain == GRAIN_BLOCK) filename = L"rank_blocks.txt";
    /* The function grain replaces the location file name. */
    else if (grain == GRAIN_FUNC) filename = L"rank_functions.txt";
    h = open_out(p->opt->out_dir, filename);
    writer_open(&w, h);
    writer_s(&w, grain_intro(grain));
    writer_s(&w, SAMPLER_DOC);
    ncols = ncols_of(p, grain);
    /* One SAMPLES header and one SAMPLES row, padded to the same widths. */
    {
        int colw[4];
        int c;
        const char *head[4];
        head[0] = "SAMPLES";
        head[1] = "nruns";
        head[2] = "nsamples";
        head[3] = "ncols";
        widths_clear(colw, 4);
        /* Header names take part in the column widths. */
        for (c = 0; c < 4; c++) width_add(colw, c, head[c]);
        width_add(colw, 0, "SAMPLES");
        width_i64(colw, 1, p->nruns);
        width_i64(colw, 2, total);
        width_i64(colw, 3, ncols);
        writer_s(&w, "\n");
        writer_header(&w, head, colw, 4);
        writer_cell(&w, "SAMPLES", colw[0], 0, 0);
        writer_cell_i64(&w, p->nruns, colw[1], 0);
        writer_cell_i64(&w, total, colw[2], 0);
        writer_cell_i64(&w, ncols, colw[3], 1);
        writer_nl(&w);
    }
    /* Say that distinct instructions were dropped after the location cap. */
    if (p->locs_capped)
        writer_s(&w, "Location capacity was reached. Further distinct instructions were not recorded.\n");
    writer_s(&w, "TABLE\n");
    order_sample_cols(ncols, g_rank_idx, &n);
    /* Measure the sample-share table, then write the header and the rows. */
    {
        int colw[4];
        int c;
        const char *head[4];
        head[0] = "rank";
        head[1] = "samples";
        head[2] = "percent";
        head[3] = "label";
        widths_clear(colw, 4);
        /* Header names take part in the column widths. */
        for (c = 0; c < 4; c++) width_add(colw, c, head[c]);
        /* One width contribution per column that was observed at least once. */
        for (i = 0; i < n; i++) {
            int id = g_rank_idx[i];
            const char *lab = label_of(p, grain, id, tmp, CX_LABEL);
            double pct = (total > 0) ? (100.0 * (double)g_sample_counts[id] / (double)total) : 0;
            width_i64(colw, 0, i + 1);
            width_i64(colw, 1, g_sample_counts[id]);
            width_fixed(colw, 2, pct, 1, 1);
            width_add(colw, 3, lab);
        }
        writer_header(&w, head, colw, 4);
        /* One TABLE row per column that was observed at least once. */
        for (i = 0; i < n; i++) {
            int id = g_rank_idx[i];
            const char *lab = label_of(p, grain, id, tmp, CX_LABEL);
            double pct = (total > 0) ? (100.0 * (double)g_sample_counts[id] / (double)total) : 0;
            writer_cell_i64(&w, i + 1, colw[0], 0);
            writer_cell_i64(&w, g_sample_counts[id], colw[1], 0);
            writer_cell_fixed(&w, pct, 1, 1, colw[2], 0);
            writer_cell(&w, lab, colw[3], 0, 1);
            writer_nl(&w);
        }
    }
    writer_s(&w, "ENDTABLE\n");
    return writer_close(&w);
}

/* Write the sampling ledgers, symbols, and three rank files, and print
   the top of each grain. Returns 1 only when every write succeeded.
   The location grain has no derived ledger. A failed write does not stop
   the later files. Does not run the causal estimator. Groups are
   classified and tallied first. */
static int emit_sampling(Profile *p) {
    int grain, ok = 1;
    classify_groups(p);
    tally_groups(p);
    /* Keep going after a failed file so the other ledgers still appear. */
    if (!write_sample_raw(p)) ok = 0;
    /* Symbols are written even when the raw ledger failed. */
    if (!write_symbols(p)) ok = 0;
    /* Location, block, then function. */
    for (grain = 0; grain < 3; grain++) {
        int ncols = ncols_of(p, grain);
        int total;
        count_grain(p, grain, ncols);
        total = sample_total(ncols);
        /* The location grain is the raw ledger, not a derived one. */
        if (grain != GRAIN_LOC) {
            /* A failed derived ledger is remembered and the rank is still tried. */
            if (!write_sample_derived(p, grain)) ok = 0;
        }
        /* A failed rank is remembered too. */
        if (!write_sample_rank(p, grain, total)) ok = 0;
        print_sample_top(p, grain, total);
    }
    return ok;
}

/* Fit and write the causal ledgers and rank files for all three grains.
   Matrices and ColOut rows come from a. rng supplies the stage-1
   permutations. Returns 0 when those allocations fail, and otherwise 1
   only when every write succeeded. A grain with no columns still writes
   a too-small rank. The location grain does not get a derived ledger.
   Does not free the arena. */
static int emit_all(Profile *p, Arena *a, Rng *rng) {
    int row_of[CX_MAX_RUNS];
    double *y;
    double *x;
    ColOut *cols;
    int nrows, maxc, grain, ok = 1;
    FitOut info;
    classify_groups(p);
    tally_groups(p);
    maxc = p->nlocs;
    /* The matrix width has to cover the widest grain. */
    if (p->nblocks > maxc) maxc = p->nblocks;
    /* Functions can outnumber both locations and blocks. */
    if (p->nfuncs > maxc) maxc = p->nfuncs;
    /* Keep a one-column allocation even when nothing was recorded. */
    if (maxc < 1) maxc = 1;
    y = (double *)arena_zalloc(a, sizeof(double) * (size_t)(p->nruns > 0 ? p->nruns : 1), 8);
    x = (double *)arena_zalloc(a, sizeof(double) * (size_t)(p->nruns > 0 ? p->nruns : 1) * (size_t)maxc, 8);
    cols = (ColOut *)arena_zalloc(a, sizeof(ColOut) * (size_t)maxc, 8);
    /* Without the work matrices there is nothing to fit or write. */
    if (!y || !x || !cols) return 0;
    nrows = prepare_rows(p, row_of, y);
    /* A failed raw ledger does not skip the later grains. */
    if (!write_raw_ledger(p)) ok = 0;
    /* Symbols are written even when the raw ledger failed. */
    if (!write_symbols(p)) ok = 0;
    /* Location, block, then function. */
    for (grain = 0; grain < 3; grain++) {
        int ncols = ncols_of(p, grain);
        const wchar_t *ledger = L"ledger_locations.txt";
        const wchar_t *rank = L"rank_locations.txt";
        /* Do not ask the fit for more columns than were allocated. */
        if (ncols > maxc) ncols = maxc;
        /* Blocks use the block file names. */
        if (grain == GRAIN_BLOCK) {
            ledger = L"ledger_blocks.txt";
            rank = L"rank_blocks.txt";
        } else if (grain == GRAIN_FUNC) /* Functions use the function file names. */ {
            ledger = L"ledger_functions.txt";
            rank = L"rank_functions.txt";
        }
        /* An empty grain is reported as too small and is not fit. */
        if (ncols < 1) {
            memset(&info, 0, sizeof(info));
            info.too_small = 1;
            info.nruns = nrows;
            /* The location grain already has its raw ledger. */
            if (grain != GRAIN_LOC) {
                /* Write an empty derived ledger and remember a failure. */
                if (!write_derived_ledger(p, grain, ledger, row_of, x, nrows, 0)) ok = 0;
            }
            /* Still write the rank so the file exists. */
            if (!write_rank(p, grain, rank, cols, &info)) ok = 0;
            continue;
        }
        accumulate(p, grain, row_of, x, nrows, ncols);
        fit_columns(x, y, nrows, ncols, p->opt->permutations, rng, cols, &info);
        /* Derived ledgers exist only for blocks and functions. */
        if (grain != GRAIN_LOC) {
            /* A failed derived ledger is remembered and the rank is still tried. */
            if (!write_derived_ledger(p, grain, ledger, row_of, x, nrows, ncols)) ok = 0;
        }
        /* A failed rank is remembered too. */
        if (!write_rank(p, grain, rank, cols, &info)) ok = 0;
        print_top(p, grain, cols, &info);
    }
    (void)nrows;
    return ok;
}

/* Allocate a zeroed profile and its tables from a. opt is stored, not
   copied. Sampling reserves CX_MAX_SAMPLES rows because one wave records
   every thread. A causal run reserves runs times samples, at least 1 and
   at most CX_MAX_SAMPLES. Returns NULL when the profile or any table
   does not fit. Does not create the output directory. */
static Profile *profile_new(Arena *a, Options *opt) {
    Profile *p = (Profile *)arena_zalloc(a, sizeof(Profile), 16);
    int cap;
    /* The profile header itself did not fit. */
    if (!p) return NULL;
    p->opt = opt;
    /* A sampling wave records every thread, so the table has to hold more than one row per wave. */
    if (opt->sampling) cap = CX_MAX_SAMPLES;
    else /* Causal mode stores one row per requested pause. */ {
        cap = opt->runs * opt->samples;
        /* A zero product still needs a non-empty table. */
        if (cap < 1) cap = 1;
        /* Do not ask the arena for more than the sample ceiling. */
        if (cap > CX_MAX_SAMPLES) cap = CX_MAX_SAMPLES;
    }
    p->sample_cap = cap;
    p->samples = (Sample *)arena_zalloc(a, sizeof(Sample) * (size_t)cap, 8);
    p->locs = (LocInfo *)arena_zalloc(a, sizeof(LocInfo) * CX_MAX_LOCS, 8);
    p->mods = (ModInfo *)arena_zalloc(a, sizeof(ModInfo) * CX_MAX_MODS, 8);
    p->blocks = (Group *)arena_zalloc(a, sizeof(Group) * CX_MAX_LOCS, 8);
    p->funcs = (Group *)arena_zalloc(a, sizeof(Group) * CX_MAX_LOCS, 8);
    /* Any missing table makes the profile unusable. */
    if (!p->samples || !p->locs || !p->mods || !p->blocks || !p->funcs) return NULL;
    return p;
}

/* Create path, or succeed when it already exists. Returns 1 on either
   outcome and 0 on any other CreateDirectoryW failure. Does not create
   missing parents. */
static int ensure_dir(const wchar_t *path) {
    /* A newly created directory is success. */
    if (CreateDirectoryW(path, NULL)) return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Run the command once and record it at runs[index]. Returns 1 to keep
   going and 0 to stop the experiment. A failed CreateProcess or a 32-bit
   child is fatal. The child is sampled until the requested count, the
   attempt cap of five times that count, the timeout, or process exit.
   Sampling counts waves; causal mode counts successful pauses. A child
   still running after the injections is waited on for the time left in
   the timeout and then killed, which marks the run timed out and not
   completed. A non-zero exit is still completed. Uses g_cmd_mut. Does
   not symbolize or write ledgers. */
static int run_one(Profile *p, int index, Rng *rng) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE nul = NULL;
    long long t0, t1, limit;
    int got = 0, attempts = 0, fatal = 0, thread_samples = 0;
    RunInfo *run;
    DWORD wait_ms;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    w_copy(g_cmd_mut, 8192, p->opt->command);
    /* Hide the child console and point its standard handles at NUL. */
    if (p->opt->quiet_child) {
        memset(&sa, 0, sizeof(sa));
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        /* Only a real NUL handle is inherited. */
        if (nul && nul != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = si.hStdOutput = si.hStdError = nul;
        }
    }
    t0 = qpc_now();
    /* CreateProcess failed, so this run stops the experiment. */
    if (!CreateProcessW(NULL, g_cmd_mut, NULL, NULL, si.dwFlags ? TRUE : FALSE,
                        p->opt->quiet_child ? CREATE_NO_WINDOW : 0,
                        NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        char msg[256];
        format_winerr(err, msg, (int)sizeof(msg));
        con_err("CreateProcess failed: ");
        con_err(msg);
        con_err("\n");
        fatal = 1;
        goto done;
    }
    run = &p->runs[index];
    run->started = 1;
    /* Reject a 32-bit child. The context read is 64-bit only. */
    {
        BOOL wow = FALSE;
        /* WOW64 means the image is 32-bit on this 64-bit process. */
        if (IsWow64Process(pi.hProcess, &wow) && wow) {
            con_err("The command is a 32-bit process. causext reads 64-bit instruction pointers.\n");
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            fatal = 1;
            goto done;
        }
    }
    /* The primary thread handle is not needed after creation. */
    if (pi.hThread) {
        CloseHandle(pi.hThread);
        pi.hThread = NULL;
    }
    limit = t0 + (g_qpc_freq * (long long)p->opt->timeout_ms) / 1000;
    /* Optional delay before the first injection, aborted if the child exits. */
    if (p->opt->warmup_us > 0) wait_us_proc(pi.hProcess, p->opt->warmup_us);
    /* Keep injecting until the budget, the attempt cap, exit, or the timeout. */
    while (got < p->opt->samples && attempts < p->opt->samples * 5) {
        int gap;
        /* The child exited, so further injections would have no target. */
        if (!process_alive(pi.hProcess)) break;
        /* The run's timeout has already arrived. */
        if (qpc_now() >= limit) break;
        /* Later injections wait a random gap. The first one does not. */
        if (got > 0) {
            gap = rng_range(rng, p->opt->gap_min_us, p->opt->gap_max_us);
            wait_us_proc(pi.hProcess, gap);
            /* The child exited during the gap. */
            if (!process_alive(pi.hProcess)) break;
        }
        attempts++;
        /* Sampling mode counts one wave, not one thread. */
        if (p->opt->sampling) {
            int n = inject_sample_wave(p, index, pi.dwProcessId);
            /* A wave that stored at least one pointer counts toward the budget. */
            if (n > 0) {
                got++;
                thread_samples += n;
            }
        /* Causal mode counts one successful pause toward the sample budget. */
        } else if (inject_pause(p, index, pi.dwProcessId, rng)) got++;
    }
    /* Convert the time still left before the timeout into a wait in milliseconds. */
    {
        long long now = qpc_now();
        long long remain_us = 0;
        /* Only a clock still before the limit has time left. */
        if (now < limit) remain_us = qpc_to_us(limit - now);
        /* A negative conversion is treated as no time left. */
        if (remain_us < 0) remain_us = 0;
        /* Clamp to a value that fits in a signed 32-bit millisecond count. */
        if (remain_us > 2147483647LL) remain_us = 2147483647LL;
        wait_ms = (DWORD)(remain_us / 1000);
    }
    /* The child is still running, so wait out the remainder or kill it. */
    if (process_alive(pi.hProcess)) {
        DWORD w = WaitForSingleObject(pi.hProcess, wait_ms);
        t1 = qpc_now();
        /* The remainder expired, so the run is a timeout and not a completion. */
        if (w == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            run->timed_out = 1;
            run->completed = 0;
            run->exit_code = 1;
        } else /* The process exited inside the remaining timeout. */ {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            run->exit_code = (int)code;
            run->completed = 1;
        }
    } else /* The process already exited during the injections. */ {
        DWORD code = 0;
        t1 = qpc_now();
        GetExitCodeProcess(pi.hProcess, &code);
        run->exit_code = (int)code;
        run->completed = 1;
    }
    run->e2e_us = qpc_to_us(t1 - t0);
    run->samples = p->opt->sampling ? thread_samples : got;
    con_err("run ");
    con_err_i64(index + 1);
    con_err("/");
    con_err_i64(p->opt->runs);
    con_err("  e2e_ms ");
    con_err_i64(run->e2e_us / 1000);
    con_err(p->opt->sampling ? "  samples " : "  pauses ");
    con_err_i64(run->samples);
    con_err("  exit ");
    con_err_i64(run->exit_code);
    /* A killed run says so on the progress line. */
    if (run->timed_out) con_err("  timeout");
    con_err("\n");
done:
    /* Close whichever process handles were actually opened. */
    if (pi.hThread) CloseHandle(pi.hThread);
    /* The process handle is absent when CreateProcess never returned one. */
    if (pi.hProcess) CloseHandle(pi.hProcess);
    /* Close the NUL handle only when it was opened. */
    if (nul && nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    return fatal ? 0 : 1;
}

/* Run every requested execution, symbolize, and write the ledgers.
   opt supplies the command and the limits. st receives counts and the
   status; a NULL st uses a local one that is discarded. Returns 0 when
   at least one instruction pointer was recorded, 2 when the runs
   finished but none was, and 1 when setup, a run, or a write failed.
   A zero seed is replaced by 1. The arena is 64 MiB and is freed on
   every return. Does not parse the command line. */
static int run_profile(Options *opt, ProfileStatus *st) {
    Arena arena;
    Profile *p;
    Rng rng;
    int i, rc;
    ProfileStatus local;
    /* A caller that does not want the counts still needs a place to write them. */
    if (!st) st = &local;
    memset(st, 0, sizeof(*st));
    /* No seed was given, so mix the clock with the process id. */
    if (!opt->seed_set) {
        opt->seed = (unsigned long long)qpc_now() ^ ((unsigned long long)GetCurrentProcessId() << 32);
        opt->seed_set = 1;
    }
    rng.s = opt->seed ? opt->seed : 1;
    /* Without an output directory the ledgers cannot be written. */
    if (!ensure_dir(opt->out_dir)) {
        con_err("Could not create the output directory.\n");
        st->rc = 1;
        return 1;
    }
    /* The profile tables and the fit matrices come from this region. */
    if (!arena_init(&arena, 64ULL * 1024ULL * 1024ULL)) {
        con_err("Could not reserve the working set.\n");
        st->rc = 1;
        return 1;
    }
    enable_debug_priv();
    p = profile_new(&arena, opt);
    /* The tables did not fit, so the reservation is released. */
    if (!p) {
        con_err("Could not allocate the profile tables.\n");
        arena_free(&arena);
        st->rc = 1;
        return 1;
    }
    con_err("causext seed ");
    con_err_i64((long long)opt->seed);
    con_err("\n");
    /* Each index is one execution. A fatal run stops the series. */
    for (i = 0; i < opt->runs; i++) {
        p->nruns = i + 1;
        /* CreateProcess or a 32-bit child ends the experiment. */
        if (!run_one(p, i, &rng)) {
            arena_free(&arena);
            st->rc = 1;
            return 1;
        }
    }
    symbolize(p);
    /* Sampling writes share files. Causal mode fits the estimator. */
    if (p->opt->sampling) {
        /* A sampling write failure is fatal after the runs. */
        if (!emit_sampling(p)) {
            con_err("Could not write the ledgers.\n");
            st->rc = 1;
            arena_free(&arena);
            return 1;
        }
    } else if (!emit_all(p, &arena, &rng)) /* A causal write or allocation failure is fatal too. */ {
        con_err("Could not write the ledgers.\n");
        st->rc = 1;
        arena_free(&arena);
        return 1;
    }
    st->samples = p->nsamples;
    st->locations = p->nlocs;
    st->functions = p->nfuncs;
    st->blocks = p->nblocks;
    st->suspend_failures = p->suspend_failures;
    st->context_failures = p->context_failures;
    st->first_api_err = p->first_api_err;
    /* Count completed runs and timeouts separately. */
    for (i = 0; i < p->nruns; i++) {
        /* A completed run is one that exited on its own. */
        if (p->runs[i].completed) st->runs_completed++;
        /* A timeout is a run this process killed. */
        if (p->runs[i].timed_out) st->runs_timed_out++;
    }
    con_out("Wrote ");
    /* Show the output directory in the ANSI code page. */
    {
        char path[700];
        wide_to_acp(opt->out_dir, path, (int)sizeof(path));
        con_out(path);
        con_out("\n");
    }
    con_out("  ledger_locations.txt\n  ledger_blocks.txt\n  ledger_functions.txt\n");
    con_out("  rank_locations.txt\n  rank_blocks.txt\n  rank_functions.txt\n  symbols.txt\n");
    /* Surface the first suspend or context failure when either happened. */
    if (p->suspend_failures || p->context_failures) {
        con_err("suspend failures ");
        con_err_i64(p->suspend_failures);
        con_err("  context failures ");
        con_err_i64(p->context_failures);
        con_err("  first error ");
        con_err_i64(p->first_api_err);
        con_err("\n");
    }
    rc = (p->nsamples > 0) ? 0 : 2;
    /* The command ran, but no instruction pointer was stored. */
    if (rc == 2) con_err("The command ran, and no instruction pointer was recorded.\n");
    st->rc = rc;
    arena_free(&arena);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* Command line, burn workload, self-test                                     */
/* -------------------------------------------------------------------------- */

/* Two halves of a FILETIME inside the find-data record. FindFirstFileW
   writes both halves. delete_out does not read either one. The pair is
   declared so CxFindData has the same prefix as the system find record. */
typedef struct CxFileTime {
    /* Low 32 bits of the 64-bit file time. */
    DWORD lo;
    /* High 32 bits of the 64-bit file time. */
    DWORD hi;
} CxFileTime;

/* Buffer shaped like WIN32_FIND_DATAW for FindFirstFileW and
   FindNextFileW. delete_out uses it while removing an output directory
   and reads only name. The fields stay in system order so the API
   writes the file name at the offset this program reads. The record is
   larger than the system struct: the tail gives the call room past the
   named fields. */
typedef struct CxFindData {
    /* File attributes. Not read by the deleter. */
    DWORD attr;
    /* Creation time. Not read. */
    CxFileTime create_time;
    /* Last access time. Not read. */
    CxFileTime access_time;
    /* Last write time. Not read. */
    CxFileTime write_time;
    /* High 32 bits of the file size. Not read. */
    DWORD size_hi;
    /* Low 32 bits of the file size. Not read. */
    DWORD size_lo;
    /* Reserved system field. Not read. */
    DWORD reserved0;
    /* Reserved system field. Not read. */
    DWORD reserved1;
    /* File name written by the finder. The only field delete_out reads. "." and ".." are skipped. */
    wchar_t name[260];
    /* Short 8.3 name. Not read. */
    wchar_t alt[14];
    /* Legacy file-type field that follows the short name. Not read. */
    DWORD file_type;
    /* Legacy creator-type field. Not read. */
    DWORD creator_type;
    /* Legacy finder flags. Not read. */
    WORD finder_flags;
    /* Two bytes that keep the following tail on a 4-byte boundary. Not read. */
    WORD pad;
    /* Extra bytes past the named fields, so a system write of the find record fits in this buffer. Not read. */
    unsigned char tail[16];
} CxFindData;

extern HANDLE __stdcall FindFirstFileW(const wchar_t *pat, void *data);
extern BOOL __stdcall FindNextFileW(HANDLE h, void *data);
extern BOOL __stdcall FindClose(HANDLE h);

/* Command line of this process. causext_main fills it and the profile,
   burn, and help paths read it. */
static Options g_opt;
/* Scratch Options for the parser checks inside the self-test. Those
   checks reset and fill it. The live experiment uses g_opt. */
static Options g_test_opt;
/* Handle of out\self_test.txt while the self-test runs. NULL or invalid
   means test_log writes only to the console. Closed after the pass or
   the failing line is logged. */
static HANDLE g_test_log;
/* Find buffer for delete_out. FindFirstFileW and FindNextFileW write it.
   The deleter reads the name field. */
static CxFindData g_find;
/* Pattern delete_out passes to FindFirstFileW: the directory plus "\*". */
static wchar_t g_pat[CX_PATH];
/* Wide path built by w_join. delete_out uses it for each directory entry.
   The self-test also builds output paths here before reading them. */
static wchar_t g_wjoin[CX_PATH];
/* Bytes of one ledger or rank file during the self-test. read_all fills
   it, and the checks search this text. 256 KiB. */
static char g_file[262144];
/* Destination of the burn loops. burn_critical and burn_slack store their
   mixed totals here so those loops stay live. The stored value is not
   interpreted. volatile so the stores are not removed. */
static volatile unsigned long long g_burn_sink;
/* Non-zero while the slack thread should keep spinning. run_burn sets
   this before creating the thread and leaves it set. The process exit
   ends the thread. */
static volatile int g_slack_run;
/* Slack-loop iterations. run_burn waits until this leaves zero so the
   thread has entered its loop before calibration. The slack thread
   increments it. */
static volatile unsigned g_slack_spins;
/* Affinity mask of the critical burn thread: the lowest set bit of the
   process mask, or 1 when that mask is empty. run_burn replaces the
   initial 1. */
static unsigned long long g_aff_critical = 1;
/* Affinity mask of the slack thread: the highest set bit of the process
   mask, or 1 when that mask is empty. */
static unsigned long long g_aff_slack = 1;
/* Non-zero while spin_fn should keep counting. test_context sets this,
   suspends the thread and reads its instruction pointer, then clears it
   and waits for the thread to exit. */
static volatile int g_spin_run;
/* Iterations of spin_fn. test_context waits until this leaves zero before
   suspending, so the thread is inside the loop. */
static volatile unsigned g_spin_n;

#define CX_TEST_N 48
#define CX_TEST_P 200
/* Row-major pause matrix for the estimator self-tests. At most CX_TEST_N
   rows and CX_TEST_P columns. Each test zeros the cells it is about to use. */
static double g_tx[CX_TEST_N * CX_TEST_P];
/* End-to-end outcomes paired with the rows of g_tx. One entry per test
   row, up to CX_TEST_N. */
static double g_ty[CX_TEST_N];
/* Column results the tests pass to fit_columns. The extra slots past
   CX_TEST_P give the 200-column cap case a ColOut for every column. */
static ColOut g_cols[CX_TEST_P + 8];
/* Fit summary filled beside g_cols. */
static FitOut g_info;
/* Generator for the estimator tests. Each test assigns s before it draws.
   The fuzz starts from 0xC0FFEE. */
static Rng g_trng;
/* Set after the fuzz has fit a design with more columns than rows.
   test_fuzz fails while this is still 0. */
static int g_saw_p_gt_n;
/* Set after the fuzz has fit a design whose second column is constant. */
static int g_saw_constant;
/* Set after the fuzz has fit two columns that carry the same pauses.
   The duplicate-column oracle sets this as well. */
static int g_saw_duplicate;
/* Set after the fuzz has fit a column that is zero on every row. */
static int g_saw_all_zero;
/* Set after the fuzz has fit a negative end-to-end outcome. */
static int g_saw_negative_y;
/* Set after the fuzz has fit a design with a single varying column. */
static int g_saw_one_col;

static void test_fail_at_line(int line);

/* Write s to standard output and, when the self-test log is open, to
   that file as well. A newline is expanded to CRLF in the file copy
   only. A NULL s does nothing. The file copy stops at 1598 bytes of
   the stack buffer, so a longer message is truncated there. WriteFile
   errors are ignored. Does not add its own newline. */
static void test_log(const char *s) {
    char buf[1600];
    int i = 0, j = 0;
    DWORD wrote = 0;
    /* A missing message is not logged. */
    if (!s) return;
    con_out(s);
    /* No log file, so the console copy is the whole record. */
    if (!g_test_log || g_test_log == INVALID_HANDLE_VALUE) return;
    /* Copy into the stack buffer, leaving room for an inserted CR. */
    while (s[i] && j + 2 < (int)sizeof(buf)) {
        /* A bare LF becomes CR LF in the file. */
        if (s[i] == '\n') buf[j++] = '\r';
        buf[j++] = s[i++];
    }
    WriteFile(g_test_log, buf, (DWORD)j, &wrote, NULL);
}

/* Log "FAIL " and the source line, close the self-test log if it is
   open, and exit with that line number as the process code. line is
   the __LINE__ of the failing check. Does not return. Does not print
   the failed expression. */
static void test_fail_at_line(int line) {
    char b[32];
    test_log("FAIL ");
    format_i64(b, (int)sizeof(b), line);
    test_log(b);
    test_log("\n");
    /* Close the log so the failure line is flushed to disk. */
    if (g_test_log && g_test_log != INVALID_HANDLE_VALUE) CloseHandle(g_test_log);
    ExitProcess((DWORD)line);
}

#define crash_if(eval) do { if (eval) test_fail_at_line(__LINE__); } while (0)

/* Log v with four digits after the decimal point. Non-finite values
   are logged as format_fixed writes them. Does not add a newline.
   Uses a 64-byte stack buffer, not g_fmt. */
static void test_log_fixed(double v) {
    char b[64];
    format_fixed(b, (int)sizeof(b), v, 4);
    test_log(b);
}

/* Append one wide character to the NUL-terminated dst. cap is the wide
   capacity. Returns 0 when the character and a new NUL would not fit,
   leaving dst unchanged. Returns 1 after storing c and a NUL. */
static int w_append_ch(wchar_t *dst, int cap, wchar_t c) {
    int n = w_len(dst);
    /* No room for the character and the terminator. */
    if (n + 1 >= cap) return 0;
    dst[n] = c;
    dst[n + 1] = 0;
    return 1;
}

/* Append src to the NUL-terminated dst. cap is the wide capacity. A
   NULL src succeeds and changes nothing. Returns 0 when a character
   would not fit, after terminating at the last character that did fit.
   Returns 1 when all of src was copied. */
static int w_append_w(wchar_t *dst, int cap, const wchar_t *src) {
    int n = w_len(dst);
    int i;
    /* Nothing to append is success. */
    if (!src) return 1;
    /* Copy until src ends or the next unit would fill the buffer. */
    for (i = 0; src[i]; i++) {
        /* Keep a terminator on the partial result. */
        if (n + 1 >= cap) {
            dst[n] = 0;
            return 0;
        }
        dst[n++] = src[i];
    }
    dst[n] = 0;
    return 1;
}

/* Append one CommandLineToArgvW-quoted argument onto a wide command
   buffer. cap is the wide capacity of dst, which must already be
   NUL-terminated. A NULL arg is treated as empty. Empty arguments and
   arguments that contain whitespace or a quote are wrapped. Backslashes
   that precede a quote are doubled, and backslashes at the end of a
   quoted argument are doubled again before the closing quote. A
   non-empty dst gets one separating space first. Returns 0 if any
   character would not fit, leaving a partial command in dst. Returns 1
   when the argument was appended. Does not parse a command line. */
static int append_quoted(wchar_t *dst, int cap, const wchar_t *arg) {
    int i, need = 0;
    /* A missing argument is quoted as empty. */
    if (!arg) arg = L"";
    /* Separate this argument from text already in dst. */
    if (dst[0] && !w_append_ch(dst, cap, L' ')) return 0;
    /* An empty argument must be quoted. */
    if (!arg[0]) need = 1;
    /* Look for whitespace or a quote that forces quoting. */
    for (i = 0; arg[i]; i++) {
        wchar_t c = arg[i];
        /* Space, tab, LF, VT, or a quote requires a quoted argument. */
        if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\v' || c == L'"') need = 1;
    }
    /* A plain token is copied without quotes. */
    if (!need) return w_append_w(dst, cap, arg);
    /* Open the quoted argument. */
    if (!w_append_ch(dst, cap, L'"')) return 0;
    i = 0;
    /* Encode each run of backslashes and the character that follows. */
    for (;;) {
        int slashes = 0;
        int k;
        /* Count a run of backslashes before the next special character. */
        while (arg[i] == L'\\') {
            slashes++;
            i++;
        }
        /* Double trailing backslashes so the closing quote stays a closer. */
        if (arg[i] == 0) {
            /* Write each doubled trailing backslash, or stop if dst fills. */
            for (k = 0; k < slashes * 2; k++) if (!w_append_ch(dst, cap, L'\\')) return 0;
            break;
        }
        /* Double backslashes before a quote and escape the quote itself. */
        if (arg[i] == L'"') {
            /* Write the doubled backslashes plus the one that escapes the quote. */
            for (k = 0; k < slashes * 2 + 1; k++) if (!w_append_ch(dst, cap, L'\\')) return 0;
            /* Write the escaped quote. */
            if (!w_append_ch(dst, cap, L'"')) return 0;
            i++;
            continue;
        }
        /* A normal character keeps the preceding backslashes once. */
        for (k = 0; k < slashes; k++) if (!w_append_ch(dst, cap, L'\\')) return 0;
        /* Write that character after those backslashes. */
        if (!w_append_ch(dst, cap, arg[i])) return 0;
        i++;
    }
    return w_append_ch(dst, cap, L'"');
}

/* Parse a signed decimal integer from a wide string into *out. An
   optional leading '-' is accepted; a leading '+' is not. Returns 0,
   and does not write *out, when s is NULL or empty, when a non-digit
   appears, when there is no digit, or when the magnitude exceeds
   2000000000. On success stores the int value and returns 1. Does not
   skip whitespace. */
static int w_to_i(const wchar_t *s, int *out) {
    int i = 0, neg = 0, any = 0;
    long long v = 0;
    /* Reject a missing or empty string. */
    if (!s || !s[0]) return 0;
    /* Remember a leading minus and skip it. */
    if (s[0] == L'-') {
        neg = 1;
        i++;
    }
    /* Accumulate decimal digits. */
    for (; s[i]; i++) {
        /* A non-digit ends the parse as failure. */
        if (s[i] < L'0' || s[i] > L'9') return 0;
        any = 1;
        v = v * 10 + (s[i] - L'0');
        /* Stop before the magnitude leaves the accepted range. */
        if (v > 2000000000LL) return 0;
    }
    /* A lone minus has no digit. */
    if (!any) return 0;
    /* Apply the sign after the magnitude check. */
    if (neg) v = -v;
    *out = (int)v;
    return 1;
}

/* Parse an unsigned decimal integer from a wide string into *out.
   Returns 0, and does not write *out, when s is NULL or empty, when a
   non-digit appears, or when another digit would overflow an unsigned
   64-bit integer. A leading sign is not accepted. On success stores the
   value and returns 1. Does not skip whitespace. */
static int w_to_u64(const wchar_t *s, unsigned long long *out) {
    int i, any = 0;
    unsigned long long v = 0;
    /* Reject a missing or empty string. */
    if (!s || !s[0]) return 0;
    /* Accumulate digits, stopping on a non-digit or an overflow. */
    for (i = 0; s[i]; i++) {
        unsigned long long d;
        /* A non-digit ends the parse as failure. */
        if (s[i] < L'0' || s[i] > L'9') return 0;
        any = 1;
        d = (unsigned long long)(s[i] - L'0');
        /* Reject a digit that would not fit in 64 bits. */
        if (v > (0xFFFFFFFFFFFFFFFFULL - d) / 10ULL) return 0;
        v = v * 10ULL + d;
    }
    /* No digit was accepted. */
    if (!any) return 0;
    *out = v;
    return 1;
}

/* Fill o with the built-in profile defaults. Zeros the whole Options,
   then sets profile mode, 20 runs, 30 samples, pauses of 200 to 4000
   microseconds, gaps of 500 to 8000 microseconds, a 60000 millisecond
   timeout, 80 permutations, and the output directory L"out". Does not
   parse argv and does not validate the result. */
static void options_default(Options *o) {
    memset(o, 0, sizeof(*o));
    o->mode = MODE_PROFILE;
    o->runs = 20;
    o->samples = 30;
    o->pause_min_us = 200;
    o->pause_max_us = 4000;
    o->gap_min_us = 500;
    o->gap_max_us = 8000;
    o->timeout_ms = 60000;
    o->permutations = 80;
    w_copy(o->out_dir, CX_PATH, L"out");
}

/* Read the integer that follows the option at argv[*i]. argc and argv
   are the wide command line. On success stores the value in *dst,
   advances *i by two, and returns 1. Returns 0 and writes o->error when
   the value is missing or is not an integer that w_to_i accepts. Does
   not range-check the integer. */
static int take_int(int argc, wchar_t **argv, int *i, int *dst, Options *o) {
    int v;
    /* An option at the end of argv has no value. */
    if (*i + 1 >= argc) {
        str_copy(o->error, (int)sizeof(o->error), "Missing the value after an option.");
        return 0;
    }
    /* Reject a value that is not a signed decimal integer. */
    if (!w_to_i(argv[*i + 1], &v)) {
        str_copy(o->error, (int)sizeof(o->error), "An option value is not an integer.");
        return 0;
    }
    *dst = v;
    *i += 2;
    return 1;
}

/* Parse wide argv into o. Does not reset defaults. argv[0] is skipped.
   --help, -h, -?, --self-test, and --burn return as soon as they are
   seen: help and self-test return 0, and --burn returns 0 after storing
   a non-negative duration or 1 when that duration is missing, not an
   integer, or negative. --quiet-child and --sampling set flags.
   Numeric options use take_int. --seed uses w_to_u64 and sets seed_set.
   --out copies a non-empty path shorter than CX_PATH. A bare -- stops
   option parsing, and so does a token that does not start with '-'.
   Everything left is joined into o->command with append_quoted, using
   capacity 8192. Returns 1 and sets o->error on a bad or unknown option
   or a command that does not fit. A NULL argv sets an error and returns
   1. Returns 0 when parsing finished. Does not clamp or validate ranges. */
static int parse_options(int argc, wchar_t **argv, Options *o) {
    int i = 1;
    /* CommandLineToArgvW failed before this call. */
    if (!argv) {
        str_copy(o->error, (int)sizeof(o->error), "Could not read the command line.");
        return 1;
    }
    /* Walk options until a bare --, a non-option, or the end of argv. */
    while (i < argc) {
        const wchar_t *a = argv[i];
        /* A bare -- ends options and keeps the tokens after it as the command. */
        if (w_eq_ascii(a, "--")) {
            i++;
            break;
        }
        /* A token that does not start with '-' is the start of the command. */
        if (!a || a[0] != L'-') break;
        /* Help returns before any later token is read. */
        if (w_eq_ascii(a, "--help") || w_eq_ascii(a, "-h") || w_eq_ascii(a, "-?")) {
            o->mode = MODE_HELP;
            return 0;
        }
        /* Self-test returns before any later token is read. */
        if (w_eq_ascii(a, "--self-test")) {
            o->mode = MODE_SELF_TEST;
            return 0;
        }
        /* Hide the child window; keep scanning options. */
        if (w_eq_ascii(a, "--quiet-child")) {
            o->quiet_child = 1;
            i++;
            continue;
        }
        /* Sample every thread instead of injecting pauses. */
        if (w_eq_ascii(a, "--sampling")) {
            o->sampling = 1;
            i++;
            continue;
        }
        /* Burn takes one duration and returns as soon as it is accepted. */
        if (w_eq_ascii(a, "--burn")) {
            int v = 0;
            /* A missing or non-integer duration is an error. */
            if (!take_int(argc, argv, &i, &v, o)) return 1;
            /* A negative duration is rejected before the mode changes. */
            if (v < 0) {
                str_copy(o->error, (int)sizeof(o->error), "The burn duration is negative.");
                return 1;
            }
            o->mode = MODE_BURN;
            o->burn_ms = v;
            return 0;
        }
        /* Store the run count and keep scanning. */
        if (w_eq_ascii(a, "--runs")) {
            /* A missing or non-integer run count is an error. */
            if (!take_int(argc, argv, &i, &o->runs, o)) return 1;
            continue;
        }
        /* Store the sample count and keep scanning. */
        if (w_eq_ascii(a, "--samples")) {
            /* A missing or non-integer sample count is an error. */
            if (!take_int(argc, argv, &i, &o->samples, o)) return 1;
            continue;
        }
        /* Store the shortest requested pause. */
        if (w_eq_ascii(a, "--pause-min-us")) {
            /* A missing or non-integer pause minimum is an error. */
            if (!take_int(argc, argv, &i, &o->pause_min_us, o)) return 1;
            continue;
        }
        /* Store the longest requested pause. */
        if (w_eq_ascii(a, "--pause-max-us")) {
            /* A missing or non-integer pause maximum is an error. */
            if (!take_int(argc, argv, &i, &o->pause_max_us, o)) return 1;
            continue;
        }
        /* Store the shortest gap between pauses. */
        if (w_eq_ascii(a, "--gap-min-us")) {
            /* A missing or non-integer gap minimum is an error. */
            if (!take_int(argc, argv, &i, &o->gap_min_us, o)) return 1;
            continue;
        }
        /* Store the longest gap between pauses. */
        if (w_eq_ascii(a, "--gap-max-us")) {
            /* A missing or non-integer gap maximum is an error. */
            if (!take_int(argc, argv, &i, &o->gap_max_us, o)) return 1;
            continue;
        }
        /* Store the delay before the first pause. */
        if (w_eq_ascii(a, "--warmup-us")) {
            /* A missing or non-integer warmup is an error. */
            if (!take_int(argc, argv, &i, &o->warmup_us, o)) return 1;
            continue;
        }
        /* Store the per-run timeout in milliseconds. */
        if (w_eq_ascii(a, "--timeout-ms")) {
            /* A missing or non-integer timeout is an error. */
            if (!take_int(argc, argv, &i, &o->timeout_ms, o)) return 1;
            continue;
        }
        /* Store the stage-1 permutation count. */
        if (w_eq_ascii(a, "--permutations")) {
            /* A missing or non-integer permutation count is an error. */
            if (!take_int(argc, argv, &i, &o->permutations, o)) return 1;
            continue;
        }
        /* Store an unsigned seed and remember that the user set one. */
        if (w_eq_ascii(a, "--seed")) {
            unsigned long long s = 0;
            /* --seed at the end of argv has no value. */
            if (i + 1 >= argc) {
                str_copy(o->error, (int)sizeof(o->error), "Missing the value after an option.");
                return 1;
            }
            /* Reject a seed that is not an unsigned decimal integer. */
            if (!w_to_u64(argv[i + 1], &s)) {
                str_copy(o->error, (int)sizeof(o->error), "An option value is not an integer.");
                return 1;
            }
            o->seed = s;
            o->seed_set = 1;
            i += 2;
            continue;
        }
        /* Replace the default output directory. */
        if (w_eq_ascii(a, "--out")) {
            /* --out at the end of argv has no path. */
            if (i + 1 >= argc) {
                str_copy(o->error, (int)sizeof(o->error), "Missing the value after an option.");
                return 1;
            }
            /* Reject an empty path or one that does not fit in out_dir. */
            if (w_len(argv[i + 1]) < 1 || w_len(argv[i + 1]) >= CX_PATH) {
                str_copy(o->error, (int)sizeof(o->error), "The output path is empty or too long.");
                return 1;
            }
            w_copy(o->out_dir, CX_PATH, argv[i + 1]);
            i += 2;
            continue;
        }
        str_copy(o->error, (int)sizeof(o->error), "Unknown option: ");
        /* Convert the unknown token to ACP and append it to the error. */
        {
            char tmp[160];
            wide_to_acp(a, tmp, (int)sizeof(tmp));
            str_append(o->error, (int)sizeof(o->error), tmp);
        }
        return 1;
    }
    o->command[0] = 0;
    /* Join the remaining tokens into the child command line. */
    for (; i < argc; i++) {
        /* A command that does not fit in 8192 wide units is an error. */
        if (!append_quoted(o->command, 8192, argv[i])) {
            str_copy(o->error, (int)sizeof(o->error), "The command is too long.");
            return 1;
        }
    }
    return 0;
}

/* Check a profile command after parsing. Modes other than MODE_PROFILE
   return 0 immediately. Runs above CX_MAX_RUNS, samples above
   CX_MAX_PER_RUN, and permutations above 500 are reduced and o->clamped
   is set. Returns 1 and sets o->error when runs or samples are below 1,
   permutations are negative, a pause, gap, or warmup is negative, a min
   is above its max, timeout-ms is below 1, the output path is empty, or
   the command is empty. When something was clamped, copies a warning
   into o->warn. Returns 0 when the profile options are usable. Does not
   create the output directory. */
static int finish_options(Options *o) {
    /* Help, self-test, and burn are not profile commands. */
    if (o->mode != MODE_PROFILE) return 0;
    /* Cap the run count at the profile table size. */
    if (o->runs > CX_MAX_RUNS) {
        o->runs = CX_MAX_RUNS;
        o->clamped = 1;
    }
    /* Cap pauses or sample waves at the per-run table size. */
    if (o->samples > CX_MAX_PER_RUN) {
        o->samples = CX_MAX_PER_RUN;
        o->clamped = 1;
    }
    /* Cap the permutation count used by the stage-1 shuffle. */
    if (o->permutations > 500) {
        o->permutations = 500;
        o->clamped = 1;
    }
    /* A profile needs at least one run. */
    if (o->runs < 1) {
        str_copy(o->error, (int)sizeof(o->error), "runs must be at least 1.");
        return 1;
    }
    /* A profile needs at least one pause or sample wave per run. */
    if (o->samples < 1) {
        str_copy(o->error, (int)sizeof(o->error), "samples must be at least 1.");
        return 1;
    }
    /* A negative shuffle count is not a request for zero shuffles. */
    if (o->permutations < 0) {
        str_copy(o->error, (int)sizeof(o->error), "permutations is negative.");
        return 1;
    }
    /* Pause, gap, and warmup durations are unsigned in effect. */
    if (o->pause_min_us < 0 || o->pause_max_us < 0 || o->gap_min_us < 0 || o->gap_max_us < 0 || o->warmup_us < 0) {
        str_copy(o->error, (int)sizeof(o->error), "A duration is negative.");
        return 1;
    }
    /* The pause draw needs a non-empty inclusive range. */
    if (o->pause_min_us > o->pause_max_us) {
        str_copy(o->error, (int)sizeof(o->error), "pause-min-us is above pause-max-us.");
        return 1;
    }
    /* The gap draw needs a non-empty inclusive range. */
    if (o->gap_min_us > o->gap_max_us) {
        str_copy(o->error, (int)sizeof(o->error), "gap-min-us is above gap-max-us.");
        return 1;
    }
    /* The run watchdog needs a positive millisecond budget. */
    if (o->timeout_ms < 1) {
        str_copy(o->error, (int)sizeof(o->error), "timeout-ms must be at least 1.");
        return 1;
    }
    /* The ledger writer needs a directory name. */
    if (!o->out_dir[0]) {
        str_copy(o->error, (int)sizeof(o->error), "The output path is empty.");
        return 1;
    }
    /* A profile has nothing to launch without a command. */
    if (!o->command[0]) {
        str_copy(o->error, (int)sizeof(o->error), "Missing the command. Usage: causext [options] -- command args");
        return 1;
    }
    /* Tell the user which caps were applied. */
    if (o->clamped) {
        str_copy(o->warn, (int)sizeof(o->warn), "runs, samples, or permutations was above its cap and was reduced.");
    }
    return 0;
}

/* Write the usage text to standard output. The text names causal and
   sampling modes, each option and its default, and the ledger files
   written under --out. Returns nothing. Does not read Options and does
   not exit the process. */
static void print_help(void) {
    con_out(
        "causext — causal profiling of one Windows command\n"
        "\n"
        "  causext [options] -- command args\n"
        "  causext [options] command args\n"
        "\n"
        "Each run starts the command and records its end-to-end time. During the\n"
        "run, causext suspends one thread at a time, reads the instruction pointer,\n"
        "holds the suspend for a random measured duration, and resumes the thread.\n"
        "Debug symbols then fold those instructions into blocks and functions.\n"
        "The same estimator ranks each ledger by impact on end-to-end time.\n"
        "The specification of that estimator is the header of every rank file.\n"
        "\n"
        "  --sampling          sample every thread instead of injecting pauses.\n"
        "                      Ranks by the share of samples, for comparison.\n"
        "                      --samples is the number of sample waves per run.\n"
        "  --runs N            runs to perform (default 20, cap 128)\n"
        "  --samples N         pauses, or sample waves, per run (default 30, cap 128)\n"
        "  --pause-min-us N    shortest requested pause (default 200)\n"
        "  --pause-max-us N    longest requested pause (default 4000)\n"
        "  --gap-min-us N      shortest gap between pauses (default 500)\n"
        "  --gap-max-us N      longest gap (default 8000)\n"
        "  --warmup-us N       delay after start before the first pause (default 0)\n"
        "  --timeout-ms N      kill a run that is still going (default 60000)\n"
        "  --permutations N    stage-1 shuffles (default 80, cap 500)\n"
        "  --seed N            reproducible pause draws\n"
        "  --out DIR           output directory (default out)\n"
        "  --quiet-child       hide the command window and its standard streams\n"
        "  --burn N            internal workload of about N milliseconds (50..20000)\n"
        "  --self-test         oracles, fuzz, and a live profile of --burn\n"
        "  --help\n"
        "\n"
        "Options belong before the command. A non-option ends option parsing and\n"
        "starts the command. Use -- before a command whose first argument has a dash.\n"
        "A failing self-test exits with the source line number of the failing check.\n"
        "\n"
        "Output, in the directory named by --out:\n"
        "  ledger_locations.txt   every measured pause and the run's end-to-end time\n"
        "  ledger_blocks.txt      pauses summed by source line or 16-byte block\n"
        "  ledger_functions.txt   pauses summed by function\n"
        "  rank_locations.txt     locations ranked by impact on end-to-end time\n"
        "  rank_blocks.txt        blocks ranked the same way\n"
        "  rank_functions.txt     functions ranked the same way\n"
        "  symbols.txt            modules and the names dbghelp returned\n"
        "\n"
        "With --sampling the same names are a sample-share profile. The rank\n"
        "header states which kind of file it is.\n");
}

/*
 * The critical thread does a counted amount of work. A pause in that loop
 * delays the process exit by about the length of the pause. A wall-clock
 * deadline would hide that delay. The slack thread runs until the process
 * exits and is not joined: pausing it leaves the critical loop alone.
 * Optimization is off so both functions keep a stable instruction stream
 * and a debug symbol of their own.
 */
extern HANDLE __stdcall GetCurrentThread(void);
extern BOOL __stdcall SetPriorityClass(HANDLE proc, DWORD cls);
extern BOOL __stdcall SetThreadPriority(HANDLE th, int prio);
extern unsigned long long __stdcall SetThreadAffinityMask(HANDLE th, unsigned long long mask);
extern BOOL __stdcall GetProcessAffinityMask(HANDLE proc, unsigned long long *proc_mask, unsigned long long *sys_mask);

/* Return the least-significant set bit of mask as a one-bit affinity
   mask. A zero mask returns 1. The scan also returns 1 if it reaches
   the top bit without finding a set bit, so that bit is not shifted
   off the word. Does not call the scheduler. */
static unsigned long long affinity_lowest(unsigned long long mask) {
    unsigned long long bit = 1;
    /* An empty mask has no core bit to return. */
    if (!mask) return 1;
    /* Shift up until this bit is set in the mask. */
    while (!(mask & bit)) {
        /* Do not shift past the top bit. */
        if (bit & 0x8000000000000000ULL) return 1;
        bit <<= 1;
    }
    return bit;
}

/* Return the most-significant set bit of mask as a one-bit affinity
   mask. A zero mask returns 1, which is also the value kept when no
   bit is set. The scan stops on the top bit so that bit is not shifted
   off the word. Does not call the scheduler. */
static unsigned long long affinity_highest(unsigned long long mask) {
    unsigned long long bit = 1, best = 1;
    /* An empty mask has no core bit to return. */
    if (!mask) return 1;
    /* Remember each set bit, so the last one is the highest. */
    while (bit) {
        /* This set bit is the highest seen so far. */
        if (mask & bit) best = bit;
        /* Stop on the top bit instead of shifting it away. */
        if (bit & 0x8000000000000000ULL) break;
        bit <<= 1;
    }
    return best;
}

#pragma optimize("", off)
/* Counted work used as the delay-sensitive loop of the burn workload.
   iters is the number of add-and-xor steps. The result is stored in
   g_burn_sink so the loop has a live use. Optimization is off and the
   function is noinline so its instruction stream and symbol stay
   stable. Does not sleep and does not look at g_slack_run. */
__declspec(noinline) static void burn_critical(unsigned long long iters) {
    unsigned long long i;
    unsigned long long a = 0;
    /* Mix a running total so the iteration count is real work. */
    for (i = 0; i < iters; i++) {
        a += i * 3ULL + 1ULL;
        a ^= a >> 7;
    }
    g_burn_sink = a;
}

/* Thread procedure that spins until g_slack_run becomes zero. param is
   unused. Pins this thread to g_aff_slack and drops it to priority -1,
   then increments g_slack_spins on every iteration. The final mix is
   stored in g_burn_sink. Returns 0. Does not count toward the burn's
   timed iteration total. Optimization is off and the function is
   noinline. */
__declspec(noinline) static DWORD __stdcall burn_slack(void *param) {
    unsigned long long a = 1;
    (void)param;
    /* A different core from the critical loop, so suspending this thread
       cannot donate or steal a core, and so its spin does not share the
       critical loop's execution unit. */
    SetThreadAffinityMask(GetCurrentThread(), g_aff_slack);
    SetThreadPriority(GetCurrentThread(), -1);
    /* Spin while the burn still wants the slack thread alive. */
    while (g_slack_run) {
        a += 1ULL;
        a ^= a << 3;
        g_slack_spins++;
    }
    g_burn_sink = a;
    return 0;
}
#pragma optimize("", on)

/* Run the internal burn workload for about millis milliseconds, then
   end the process. Values below 50 become 50 and values above 20000
   become 20000. Raises the process to HIGH_PRIORITY_CLASS (0x80) and
   this thread to priority 1, pins the critical loop to the lowest
   process-affinity bit and the slack thread to the highest, and starts
   burn_slack. After a short wait for that thread to count a spin, warms
   the core for about half a second, times about four fifths of a second
   of 200000-iteration chunks (stopping early at 800000000 iterations),
   and scales that rate to millis. The timed count is clamped to
   1000000..5000000000. Does not return: it closes the slack thread
   handle without joining it, then TerminateProcess and ExitProcess with
   status 0. A failed CreateThread still runs the critical loop. */
__declspec(noreturn) static void run_burn(int millis) {
    HANDLE th;
    DWORD tid = 0;
    unsigned long long proc_mask = 0, sys_mask = 0;
    long long t0, dt, target, iters;
    /* Keep the burn long enough to calibrate. */
    if (millis < 50) millis = 50;
    /* Cap the requested duration at 20 seconds. */
    if (millis > 20000) millis = 20000;
    /* Keep other processes off this core, but stay at the same thread
       priority the timed loop will use. A faster priority during the clock
       sample over-predicts how fast the later loop runs. */
    SetPriorityClass(GetCurrentProcess(), 0x00000080u);
    SetThreadPriority(GetCurrentThread(), 1);
    /* Split the critical loop and the slack thread onto opposite affinity bits. */
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask)) {
        g_aff_critical = affinity_lowest(proc_mask);
        g_aff_slack = affinity_highest(proc_mask);
    }
    SetThreadAffinityMask(GetCurrentThread(), g_aff_critical);
    g_slack_run = 1;
    g_slack_spins = 0;
    th = CreateThread(NULL, 0, burn_slack, NULL, 0, &tid);
    /* Wait briefly until the slack thread has incremented its counter. */
    {
        int spins;
        /* Sleep 1 ms at a time until a slack spin is visible or 200 tries pass. */
        for (spins = 0; spins < 200 && g_slack_spins == 0; spins++) Sleep(1);
    }
    /* Spin until the core leaves its idle frequency, then time most of a
       second at the same priority and core as the timed loop. A short
       sample on a laptop turbo clock swings the iteration count by more
       than a later pause, and that swing is not one of the pause columns.
       The profiler's warmup has to cover this whole prefix. */
    t0 = qpc_now();
    /* Warm the core for about half a second before the timed sample. */
    while (qpc_now() - t0 < g_qpc_freq / 2) burn_critical(200000ULL);
    t0 = qpc_now();
    /* Time fixed chunks and scale them to the requested duration. */
    {
        unsigned long long counted = 0;
        /* Run 200000-iteration chunks for about 0.8 seconds, or until 800 million. */
        do {
            burn_critical(200000ULL);
            counted += 200000ULL;
            dt = qpc_now() - t0;
            /* Avoid dividing by a zero or negative tick delta. */
            if (dt < 1) dt = 1;
        } while (dt < (g_qpc_freq * 4) / 5 && counted < 800000000ULL);
        target = g_qpc_freq * (long long)millis / 1000;
        /* A tiny frequency still asks for at least one tick of work. */
        if (target < 1) target = 1;
        iters = (long long)((counted * (unsigned long long)target) / (unsigned long long)dt);
    }
    /* Do not run a calibrated loop shorter than one million iterations. */
    if (iters < 1000000LL) iters = 1000000LL;
    /* Do not run a calibrated loop longer than five billion iterations. */
    if (iters > 5000000000LL) iters = 5000000000LL;
    burn_critical((unsigned long long)iters);
    /* Drop the slack thread handle without waiting for it. */
    if (th) CloseHandle(th);
    /* TerminateProcess drops a thread that an outside profiler still has suspended. */
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0);
}

/* Check square root and number formatting, and that the estimator
   document still names the worked beta. Also checks that SYMBOL_INFO
   is 88 bytes and IMAGEHLP_LINE64 is 40 bytes. Expects sqrt_local(0)
   and sqrt_local of a negative to be 0, a few exact squares, "1.50",
   "-0.1", "0", "-42", "na" for a quiet NaN, "0x0", and "0x3000".
   Fails the process through crash_if on a mismatch. Does not write a
   file. */
static void test_sqrt_format(void) {
    char buf[64];
    /* Two views of one double, used to plant a quiet NaN. u receives the
       bit pattern and d is what format_fixed must print as "na". */
    union {
        /* Numeric view passed to the formatter. */
        double d;
        /* Integer view loaded with the quiet-NaN pattern 0x7ff8000000000001. */
        unsigned long long u;
    } nb;
    double s;
    /* The copied estimator text must still contain the worked example. */
    crash_if(!str_contains(ESTIMATOR_DOC, "beta_fwl = 2"));
    /* Confirm the hand-declared dbghelp structs match the sizes this file uses. */
    {
        int sym_bytes = (int)sizeof(SYMBOL_INFO);
        int line_bytes = (int)sizeof(IMAGEHLP_LINE64);
        crash_if(sym_bytes != 88);
        crash_if(line_bytes != 40);
    }
    crash_if(sqrt_local(0) != 0);
    crash_if(sqrt_local(-4) != 0);
    s = sqrt_local(1);
    crash_if(dabs(s - 1.0) > 1e-12);
    s = sqrt_local(4);
    crash_if(dabs(s - 2.0) > 1e-12);
    s = sqrt_local(2);
    crash_if(dabs(s - 1.41421356237) > 1e-9);
    s = sqrt_local(1000000.0);
    crash_if(dabs(s - 1000.0) > 1e-6);
    format_fixed(buf, (int)sizeof(buf), 1.5, 2);
    crash_if(!str_eq(buf, "1.50"));
    format_fixed(buf, (int)sizeof(buf), -0.1, 1);
    crash_if(!str_eq(buf, "-0.1"));
    format_i64(buf, (int)sizeof(buf), 0);
    crash_if(!str_eq(buf, "0"));
    format_i64(buf, (int)sizeof(buf), -42);
    crash_if(!str_eq(buf, "-42"));
    nb.u = 0x7ff8000000000001ULL;
    format_fixed(buf, (int)sizeof(buf), nb.d, 2);
    crash_if(!str_eq(buf, "na"));
    format_hex(buf, (int)sizeof(buf), 0);
    crash_if(!str_eq(buf, "0x0"));
    format_hex(buf, (int)sizeof(buf), 0x3000);
    crash_if(!str_eq(buf, "0x3000"));
}

/* Check solve_ge on a 1x1 system, a singular 2x2, and 30 random
   systems of size 1 through 6. The random systems build G as I plus
   M transpose M from integer entries in -2..2 and a right-hand side in
   -4..4, then require the residual sum of squares to be below 1e-6.
   Uses g_trng seeded to 7. The 1x1 check expects x = 2, and the
   singular pair must be rejected. Fails through crash_if. Does not
   restore A or b, which solve_ge overwrites. */
static void test_solve(void) {
    double A[8];
    double b[4];
    double x[4];
    double M[36];
    double G[36];
    double rhs[6];
    double sol[6];
    int trial;
    A[0] = 4;
    b[0] = 8;
    x[0] = 0;
    crash_if(!solve_ge(A, b, 1, x));
    crash_if(dabs(x[0] - 2.0) > 1e-9);
    A[0] = 1; A[1] = 2; A[2] = 1; A[3] = 2;
    b[0] = 3; b[1] = 3;
    crash_if(solve_ge(A, b, 2, x) != 0);
    g_trng.s = 7;
    /* Build and solve one random well-conditioned system. */
    for (trial = 0; trial < 30; trial++) {
        int n = 1 + (int)(rng_u32(&g_trng) % 6);
        int i, j, k;
        double sse = 0;
        /* Fill M with integers from -2 through 2. */
        for (i = 0; i < n * n; i++) {
            int v = (int)(rng_u32(&g_trng) % 5) - 2;
            M[i] = (double)v;
        }
        /* Build a right-hand side and G = I + M transpose M. */
        for (i = 0; i < n; i++) {
            rhs[i] = (double)((int)(rng_u32(&g_trng) % 9) - 4);
            /* Accumulate one row of the Gram matrix. */
            for (j = 0; j < n; j++) {
                double s = (i == j) ? 1.0 : 0.0;
                /* Add one column of the outer product into this Gram entry. */
                for (k = 0; k < n; k++) s += M[k * n + i] * M[k * n + j];
                G[i * n + j] = s;
            }
        }
        /* solve_ge overwrites its matrix, so the residual check uses the original G. */
        /* Solve a copy so the residual can use the original G. */
        {
            double Gc[36];
            double bc[6];
            /* Copy G before solve_ge overwrites it. */
            for (i = 0; i < n * n; i++) Gc[i] = G[i];
            /* Copy the right-hand side before solve_ge overwrites it. */
            for (i = 0; i < n; i++) bc[i] = rhs[i];
            crash_if(!solve_ge(Gc, bc, n, sol));
        }
        /* Sum squared residuals of G times the solution. */
        for (i = 0; i < n; i++) {
            double ax = 0;
            /* Accumulate one row of G times the solution. */
            for (j = 0; j < n; j++) ax += G[i * n + j] * sol[j];
            sse += (ax - rhs[i]) * (ax - rhs[i]);
        }
        crash_if(!(sse < 1e-6));
    }
}

/* Check the worked OLS example and the one-column ridge identities.
   X = 1,2,3, O = 0,0,1, Y = 2,4,9 must give a = 0, b = 2, g = 3 and a
   near-zero standard error. One column of Y = 2*X with lambda 0 must
   give beta 2, and lambda 1 must give beta 1. Two identical columns of
   that line must each receive 2/(2+lambda). Fails through crash_if.
   Uses g_tx, g_ty, and g_ridge_model. Does not call fit_columns. */
static void test_ols_ridge_oracles(void) {
    double x[4];
    double o[4];
    double y[4];
    double a = 9, b = 9, g = 9, se = 9;
    int cols[2];
    double expect;
    x[0] = 1; x[1] = 2; x[2] = 3;
    o[0] = 0; o[1] = 0; o[2] = 1;
    y[0] = 2; y[1] = 4; y[2] = 9;
    crash_if(!ols_two(x, o, y, 3, &a, &b, &g, &se));
    crash_if(dabs(a) > 1e-8);
    crash_if(dabs(b - 2.0) > 1e-8);
    crash_if(dabs(g - 3.0) > 1e-8);
    crash_if(dabs(se) > 1e-8);

    x[0] = 1; x[1] = 2; x[2] = 3;
    y[0] = 2; y[1] = 4; y[2] = 6;
    cols[0] = 0;
    crash_if(!ridge_fit_mask(x, 1, cols, 3, 1, y, NULL, 0.0, &g_ridge_model));
    crash_if(dabs(g_ridge_model.beta[0] - 2.0) > 1e-8);
    crash_if(!ridge_fit_mask(x, 1, cols, 3, 1, y, NULL, 1.0, &g_ridge_model));
    crash_if(dabs(g_ridge_model.beta[0] - 1.0) > 1e-8);

    /* Identical columns. Y = 2 * X0 and X1 = X0, so each ridge share is 2/(2+lambda). */
    g_tx[0] = 1; g_tx[1] = 1;
    g_tx[2] = 2; g_tx[3] = 2;
    g_tx[4] = 3; g_tx[5] = 3;
    g_ty[0] = 2; g_ty[1] = 4; g_ty[2] = 6;
    cols[0] = 0;
    cols[1] = 1;
    expect = 2.0 / (2.0 + 1e-6);
    crash_if(!ridge_fit_mask(g_tx, 2, cols, 3, 2, g_ty, NULL, 1e-6, &g_ridge_model));
    crash_if(dabs(g_ridge_model.beta[0] - expect) > 1e-5);
    crash_if(dabs(g_ridge_model.beta[1] - expect) > 1e-5);
    crash_if(dabs(g_ridge_model.beta[0] + g_ridge_model.beta[1] - 2.0 * expect) > 1e-5);
}

/* Zero the first n*p entries of g_tx and the first n entries of g_ty.
   n and p are the row and column counts the next fit will use. Does
   not clear the rest of those static buffers and does not touch g_cols. */
static void zero_used(int n, int p) {
    int i;
    /* Clear the pause matrix the next case will fill. */
    for (i = 0; i < n * p; i++) g_tx[i] = 0;
    /* Clear the end-to-end values the next case will fill. */
    for (i = 0; i < n; i++) g_ty[i] = 0;
}

/* Drive fit_columns through duplicate columns, a nearly collinear
   pair, three pass-through columns, a too-small design, a permutation
   test, and a 200-column cap. Sets g_saw_duplicate. On a failed
   isolation or pass-through check, logs the betas and fails at that
   source line. Uses g_tx, g_ty, g_trng, g_cols, and g_info. Does not
   write a ledger. */
static void test_fit_cases(void) {
    int r, j;
    /* Duplicate columns through the full estimator, including cross-validation. */
    zero_used(12, 2);
    /* Fill two identical columns whose end-to-end time is twice the column. */
    for (r = 0; r < 12; r++) {
        double v = (double)(r + 1);
        g_tx[r * 2 + 0] = v;
        g_tx[r * 2 + 1] = v;
        g_ty[r] = 2.0 * v;
    }
    g_trng.s = 99;
    fit_columns(g_tx, g_ty, 12, 2, 0, &g_trng, g_cols, &g_info);
    crash_if(g_info.too_small);
    crash_if(g_cols[0].identified);
    crash_if(g_cols[1].identified);
    crash_if(g_cols[0].stable);
    crash_if(g_cols[1].stable);
    crash_if(!(g_cols[0].ridge > 0.5));
    crash_if(!(g_cols[1].ridge > 0.5));
    crash_if(g_cols[0].ridge + g_cols[1].ridge < 1.5);
    crash_if(g_cols[0].ridge + g_cols[1].ridge > 2.05);
    g_saw_duplicate = 1;

    /* Y = 1000 + 3*X0, and X1 tracks X0 plus a small offset. Ridge keeps the effect on X0. */
    zero_used(48, 2);
    /* Build the nearly collinear pair and the matching end-to-end times. */
    for (r = 0; r < 48; r++) {
        double x0 = (double)(r + 1);
        double noise = (double)((r * 17) % 11) - 5.0;
        g_tx[r * 2 + 0] = x0;
        g_tx[r * 2 + 1] = 0.5 * x0 + noise;
        g_ty[r] = 1000.0 + 3.0 * x0;
    }
    fit_columns(g_tx, g_ty, 48, 2, 0, &g_trng, g_cols, &g_info);
    crash_if(g_info.too_small);
    /* Log the betas and stop when ridge did not keep the effect on the first column. */
    if (!(g_cols[0].ridge > 2.2) || dabs(g_cols[1].ridge) >= 1.0) {
        test_log("isolation ridge0 ");
        test_log_fixed(g_cols[0].ridge);
        test_log(" ridge1 ");
        test_log_fixed(g_cols[1].ridge);
        test_log(" lambda ");
        test_log_fixed(g_info.lambda);
        test_log("\n");
        test_fail_at_line(__LINE__);
    }

    /* Every pause passes through one-for-one, on three different columns. */
    zero_used(12, 3);
    /* Give each column its own pattern and add all three into Y. */
    for (r = 0; r < 12; r++) {
        g_tx[r * 3 + 0] = (double)(r + 1);
        g_tx[r * 3 + 1] = (double)((r * 3) % 7 + 1);
        g_tx[r * 3 + 2] = (double)((r * 5) % 11 + 2);
        g_ty[r] = 5000.0 + g_tx[r * 3 + 0] + g_tx[r * 3 + 1] + g_tx[r * 3 + 2];
    }
    fit_columns(g_tx, g_ty, 12, 3, 0, &g_trng, g_cols, &g_info);
    crash_if(g_info.too_small);
    /* Each pass-through column should land near beta 1. */
    for (j = 0; j < 3; j++) {
        /* Log this column and stop when its ridge beta is far from 1. */
        if (dabs(g_cols[j].ridge - 1.0) > 0.15) {
            test_log("pass-through beta ");
            test_log_fixed(g_cols[j].ridge);
            test_log("\n");
            test_fail_at_line(__LINE__);
        }
    }

    /* Descriptive totals exist even when there are too few runs to fit. */
    zero_used(3, 1);
    g_tx[0] = 1;
    g_tx[1] = 2;
    g_tx[2] = 3;
    g_ty[0] = 10;
    g_ty[1] = 20;
    g_ty[2] = 30;
    fit_columns(g_tx, g_ty, 3, 1, 0, &g_trng, g_cols, &g_info);
    crash_if(!g_info.too_small);
    crash_if(g_cols[0].total_x != 6);

    /* A strong single column survives the permutation test. */
    zero_used(20, 1);
    /* Make Y exactly 1000 + 4*X so the FWL slope is 4. */
    for (r = 0; r < 20; r++) {
        g_tx[r] = (double)(r * 10 + 5);
        g_ty[r] = 1000.0 + 4.0 * g_tx[r];
    }
    g_trng.s = 1234;
    fit_columns(g_tx, g_ty, 20, 1, 40, &g_trng, g_cols, &g_info);
    crash_if(!g_cols[0].identified);
    crash_if(!(g_cols[0].perm_p >= 0 && g_cols[0].perm_p < 0.05));
    crash_if(dabs(g_cols[0].fwl - 4.0) > 1e-6);

    /* The joint model keeps the 96 columns with the largest pause totals. */
    zero_used(10, 200);
    /* Give later columns larger totals so the cap keeps the high indexes. */
    for (r = 0; r < 10; r++) {
        g_ty[r] = 1000.0;
        /* Store a rising pause total in every column of this row. */
        for (j = 0; j < 200; j++) g_tx[r * 200 + j] = (double)(j + 1);
    }
    fit_columns(g_tx, g_ty, 10, 200, 0, &g_trng, g_cols, &g_info);
    crash_if(g_info.njoint != 96);
    crash_if(!g_info.capped);
    crash_if(!g_cols[199].in_joint);
    crash_if(!g_cols[104].in_joint);
    crash_if(g_cols[103].in_joint);
}

/* Fit g_tx and g_ty and require every column's ridge and impact to be
   finite. An identified column must also have a finite FWL beta. Joint
   columns ordered by impact must be nonincreasing within 1e-6. n and p
   are the row and column counts passed to fit_columns with zero
   permutations. Fails through crash_if. Uses g_trng, g_cols, g_info,
   and g_rank_idx. Does not check the beta values themselves. */
static void test_expect_finite(int n, int p) {
    int j, i, ord_n;
    fit_columns(g_tx, g_ty, n, p, 0, &g_trng, g_cols, &g_info);
    /* Reject a non-finite coefficient on any column. */
    for (j = 0; j < p; j++) {
        crash_if(!is_finite(g_cols[j].ridge));
        crash_if(!is_finite(g_cols[j].impact));
        /* An identified column must publish a finite FWL beta. */
        if (g_cols[j].identified) crash_if(!is_finite(g_cols[j].fwl));
    }
    order_cols(g_cols, p, 1, g_rank_idx, &ord_n);
    /* Reject a joint order whose impact rises as the index increases. */
    for (i = 1; i < ord_n; i++) {
        double hi = g_cols[g_rank_idx[i - 1]].impact;
        double lo = g_cols[g_rank_idx[i]].impact;
        crash_if(lo > hi + 1e-6);
    }
}

/* Run fixed awkward designs, then 24 random ones, and require finite
   fits. The fixed cases are more columns than rows, a constant second
   column, a duplicate column, an all-zero column, a negative Y, and
   one varying column. Each sets the matching g_saw_* flag. Random cases
   use 4..23 rows, 1..6 columns, Y in -500..3499, and cells in 0..39,
   from g_trng seeded to 0xC0FFEE. Fails if any flag stayed clear. Does
   not check that a particular beta was recovered. */
static void test_fuzz(void) {
    int iter, r, j;
    g_trng.s = 0xC0FFEEULL;
    zero_used(4, 6);
    /* Six columns and four rows, every cell positive. */
    for (r = 0; r < 4; r++) {
        g_ty[r] = (double)(100 + r);
        /* Fill this row with a rising pattern across the six columns. */
        for (j = 0; j < 6; j++) g_tx[r * 6 + j] = (double)(r + j + 1);
    }
    test_expect_finite(4, 6);
    g_saw_p_gt_n = 1;

    zero_used(8, 2);
    /* A varying column beside a column that is constant. */
    for (r = 0; r < 8; r++) {
        g_tx[r * 2 + 0] = (double)(r + 1);
        g_tx[r * 2 + 1] = 4;
        g_ty[r] = 10.0 + g_tx[r * 2 + 0];
    }
    test_expect_finite(8, 2);
    g_saw_constant = 1;

    zero_used(8, 2);
    /* Two columns that hold the same values on every row. */
    for (r = 0; r < 8; r++) {
        g_tx[r * 2 + 0] = (double)(r + 1);
        g_tx[r * 2 + 1] = g_tx[r * 2 + 0];
        g_ty[r] = 3.0 * g_tx[r * 2 + 0];
    }
    test_expect_finite(8, 2);
    g_saw_duplicate = 1;

    zero_used(6, 1);
    /* Leave the single column at zero and vary only Y. */
    for (r = 0; r < 6; r++) g_ty[r] = (double)(r + 1);
    test_expect_finite(6, 1);
    g_saw_all_zero = 1;

    zero_used(8, 1);
    /* A positive column with a negative end-to-end time. */
    for (r = 0; r < 8; r++) {
        g_tx[r] = (double)(r + 1);
        g_ty[r] = -100.0 - (double)r;
    }
    test_expect_finite(8, 1);
    g_saw_negative_y = 1;

    zero_used(8, 1);
    /* One column that repeats a short pattern, with Y twice that column. */
    for (r = 0; r < 8; r++) {
        g_tx[r] = (double)((r * 3) % 5 + 1);
        g_ty[r] = 50.0 + 2.0 * g_tx[r];
    }
    test_expect_finite(8, 1);
    g_saw_one_col = 1;

    /* Draw a fresh small design and require the same finite-fit checks. */
    for (iter = 0; iter < 24; iter++) {
        int n = 4 + (int)(rng_u32(&g_trng) % 20);
        int p = 1 + (int)(rng_u32(&g_trng) % 6);
        zero_used(n, p);
        /* Fill Y and every pause cell from the generator. */
        for (r = 0; r < n; r++) {
            g_ty[r] = (double)((int)(rng_u32(&g_trng) % 4000) - 500);
            /* Store an independent 0..39 pause in each column of this row. */
            for (j = 0; j < p; j++) g_tx[r * p + j] = (double)(rng_u32(&g_trng) % 40);
        }
        test_expect_finite(n, p);
    }
    crash_if(!g_saw_p_gt_n);
    crash_if(!g_saw_constant);
    crash_if(!g_saw_duplicate);
    crash_if(!g_saw_all_zero);
    crash_if(!g_saw_negative_y);
    crash_if(!g_saw_one_col);
}

/* Check append_quoted on a plain token, a spaced token, a token with
   quotes, an empty token, and a backslash before a quote. Then check
   parse_options and finish_options on help, self-test, burn, a normal
   command, sampling, clamping, an inverted pause range, a missing
   command, an unknown option, and a seed with no value. Uses
   g_test_opt. Fails through crash_if. Does not start a process. */
static void test_quote_parse(void) {
    char acp[512];
    wchar_t *helpv[] = {L"causext", L"--help"};
    wchar_t *stv[] = {L"causext", L"--self-test", L"--runs", L"nope"};
    wchar_t *burnv[] = {L"causext", L"--burn", L"350"};
    wchar_t *okv[] = {L"causext", L"--runs", L"10", L"--samples", L"4", L"--quiet-child",
                      L"--out", L"outdir", L"--", L"prog", L"a b", L"c"};
    wchar_t *clampv[] = {L"causext", L"--runs", L"9999", L"--permutations", L"900", L"tool"};
    wchar_t *badv[] = {L"causext", L"--pause-min-us", L"50", L"--pause-max-us", L"10", L"cmd"};
    wchar_t *missv[] = {L"causext", L"--runs", L"4"};
    wchar_t *unkv[] = {L"causext", L"--nope"};
    wchar_t *nov[] = {L"causext", L"--seed"};

    g_test_opt.command[0] = 0;
    crash_if(!append_quoted(g_test_opt.command, 8192, L"hello"));
    crash_if(!wide_to_acp(g_test_opt.command, acp, (int)sizeof(acp)));
    crash_if(!str_eq(acp, "hello"));

    g_test_opt.command[0] = 0;
    crash_if(!append_quoted(g_test_opt.command, 8192, L"a b"));
    crash_if(!wide_to_acp(g_test_opt.command, acp, (int)sizeof(acp)));
    crash_if(!str_eq(acp, "\"a b\""));

    g_test_opt.command[0] = 0;
    crash_if(!append_quoted(g_test_opt.command, 8192, L"say \"hi\""));
    crash_if(!wide_to_acp(g_test_opt.command, acp, (int)sizeof(acp)));
    crash_if(!str_eq(acp, "\"say \\\"hi\\\"\""));

    g_test_opt.command[0] = 0;
    crash_if(!append_quoted(g_test_opt.command, 8192, L""));
    crash_if(!wide_to_acp(g_test_opt.command, acp, (int)sizeof(acp)));
    crash_if(!str_eq(acp, "\"\""));

    /* Quote a backslash that sits immediately before a quote. */
    {
        wchar_t arg[5];
        arg[0] = L'a';
        arg[1] = L'\\';
        arg[2] = L'"';
        arg[3] = L'b';
        arg[4] = 0;
        g_test_opt.command[0] = 0;
        crash_if(!append_quoted(g_test_opt.command, 8192, arg));
        crash_if(!wide_to_acp(g_test_opt.command, acp, (int)sizeof(acp)));
        crash_if(acp[0] != '"');
        crash_if(acp[1] != 'a');
        crash_if(acp[2] != '\\');
        crash_if(acp[3] != '\\');
        crash_if(acp[4] != '\\');
        crash_if(acp[5] != '"');
        crash_if(acp[6] != 'b');
        crash_if(acp[7] != '"');
        crash_if(acp[8] != 0);
    }

    options_default(&g_test_opt);
    crash_if(parse_options(2, helpv, &g_test_opt) != 0);
    crash_if(g_test_opt.mode != MODE_HELP);

    options_default(&g_test_opt);
    crash_if(parse_options(4, stv, &g_test_opt) != 0);
    crash_if(g_test_opt.mode != MODE_SELF_TEST);

    options_default(&g_test_opt);
    crash_if(parse_options(3, burnv, &g_test_opt) != 0);
    crash_if(g_test_opt.mode != MODE_BURN);
    crash_if(g_test_opt.burn_ms != 350);

    options_default(&g_test_opt);
    crash_if(parse_options(12, okv, &g_test_opt) != 0);
    crash_if(finish_options(&g_test_opt) != 0);
    crash_if(g_test_opt.runs != 10);
    crash_if(g_test_opt.samples != 4);
    crash_if(!g_test_opt.quiet_child);
    crash_if(!w_eq_ascii(g_test_opt.out_dir, "outdir"));
    crash_if(!wide_to_acp(g_test_opt.command, acp, (int)sizeof(acp)));
    crash_if(!str_eq(acp, "prog \"a b\" c"));

    /* Sampling mode still takes a command after -- and keeps the default output directory. */
    {
        wchar_t *sampv[] = {L"causext", L"--sampling", L"--runs", L"2", L"--", L"prog"};
        options_default(&g_test_opt);
        crash_if(parse_options(6, sampv, &g_test_opt) != 0);
        crash_if(finish_options(&g_test_opt) != 0);
        crash_if(!g_test_opt.sampling);
        crash_if(g_test_opt.runs != 2);
        crash_if(!w_eq_ascii(g_test_opt.out_dir, "out"));
        crash_if(!w_eq_ascii(g_test_opt.command, "prog"));
    }

    options_default(&g_test_opt);
    crash_if(parse_options(6, clampv, &g_test_opt) != 0);
    crash_if(finish_options(&g_test_opt) != 0);
    crash_if(g_test_opt.runs != CX_MAX_RUNS);
    crash_if(g_test_opt.permutations != 500);
    crash_if(!g_test_opt.warn[0]);
    crash_if(!w_eq_ascii(g_test_opt.command, "tool"));

    options_default(&g_test_opt);
    crash_if(parse_options(6, badv, &g_test_opt) != 0);
    crash_if(finish_options(&g_test_opt) == 0);

    options_default(&g_test_opt);
    crash_if(parse_options(3, missv, &g_test_opt) != 0);
    crash_if(finish_options(&g_test_opt) == 0);

    options_default(&g_test_opt);
    crash_if(parse_options(2, unkv, &g_test_opt) == 0);

    options_default(&g_test_opt);
    crash_if(parse_options(2, nov, &g_test_opt) == 0);
}

/* Build a directory path under the process temp directory. leaf is an
   ASCII prefix, followed by '_' and the process id. If GetTempPathW
   fails or returns an empty path, dst starts as L".\\". cap is the wide
   capacity. Asks for cap-40 units when cap is above 40, otherwise cap,
   so the suffix still has room when the temp path is short. Does not
   create the directory. The local tmp buffer is unused. */
static void make_temp_dir(wchar_t *dst, int cap, const char *leaf) {
    wchar_t tmp[8];
    DWORD n;
    char num[32];
    (void)tmp;
    dst[0] = 0;
    n = GetTempPathW((DWORD)(cap > 40 ? cap - 40 : cap), dst);
    /* Fall back to the current directory when the temp path is unavailable. */
    if (n == 0 || dst[0] == 0) w_copy(dst, cap, L".\\");
    w_append_ascii(dst, cap, leaf);
    w_append_ascii(dst, cap, "_");
    format_i64(num, (int)sizeof(num), (long long)GetCurrentProcessId());
    w_append_ascii(dst, cap, num);
}

/* Delete files directly inside dir and then remove dir. Uses the static
   pattern g_pat and the static find data g_find. Skips "." and "..". A
   name that does not fit in g_wjoin is skipped. Does not recurse into
   subdirectories and does not report DeleteFileW or RemoveDirectoryW
   failures. A NULL or empty dir returns immediately. */
static void delete_out(const wchar_t *dir) {
    HANDLE h;
    /* Nothing to delete. */
    if (!dir || !dir[0]) return;
    w_copy(g_pat, CX_PATH, dir);
    w_append_ascii(g_pat, CX_PATH, "\\*");
    h = FindFirstFileW(g_pat, &g_find);
    /* Walk every directory entry the first find returned. */
    if (h != INVALID_HANDLE_VALUE && h != NULL) {
        /* Delete one file, skipping the dot entries. */
        do {
            /* Leave the directory entries themselves alone. */
            if (g_find.name[0] == L'.' && (g_find.name[1] == 0 || (g_find.name[1] == L'.' && g_find.name[2] == 0)))
                continue;
            /* Skip a name that does not fit in the join buffer. */
            if (!w_join(g_wjoin, CX_PATH, dir, g_find.name)) continue;
            DeleteFileW(g_wjoin);
        } while (FindNextFileW(h, &g_find));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* Read an existing file into dst and NUL-terminate it. cap is the byte
   capacity, including the terminator. Returns the byte count on
   success, -1 when the file cannot be opened, sized, or read, and -2
   when the file is at least cap bytes. Shares the file for read and
   write. Does not grow dst and does not strip a newline. */
static int read_all(const wchar_t *path, char *dst, int cap) {
    HANDLE h;
    DWORD sz, got = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    /* A missing or unreadable file is a read failure. */
    if (h == INVALID_HANDLE_VALUE || h == NULL) return -1;
    sz = GetFileSize(h, NULL);
    /* A failed size query is a read failure. */
    if (sz == 0xFFFFFFFFu) {
        CloseHandle(h);
        return -1;
    }
    /* Refuse a file that would not leave room for the terminator. */
    if (sz >= (DWORD)cap) {
        CloseHandle(h);
        return -2;
    }
    /* A failed read of a non-empty file is a read failure. */
    if (sz && !ReadFile(h, dst, sz, &got, NULL)) {
        CloseHandle(h);
        return -1;
    }
    dst[got] = 0;
    CloseHandle(h);
    return (int)got;
}

/* Return the index of the first group whose label equals lab, or -1
   when none of the n groups match. Comparison is str_eq, so it is
   exact rather than a substring. Does not allocate. */
static int find_group(const Group *gs, int n, const char *lab) {
    int i;
    /* Scan labels until one matches exactly. */
    for (i = 0; i < n; i++) if (str_eq(gs[i].label, lab)) return i;
    return -1;
}

/* Fill symbol fields on one location. loc is an index into p->locs.
   Always marks the location as having a function, copies func into the
   CX_NAME buffer, and stores func_start. When line is greater than 0,
   also marks a source line, stores line, and copies file. A
   non-positive line leaves has_line clear and does not copy file.
   Does not classify groups. */
static void set_sym(Profile *p, int loc, const char *func, int line, unsigned long long start, const char *file) {
    LocInfo *locp = &p->locs[loc];
    locp->has_func = 1;
    str_copy(locp->func, CX_NAME, func);
    locp->func_start = start;
    /* Record a source line only when the caller has a positive line number. */
    if (line > 0) {
        locp->has_line = 1;
        locp->line = line;
        str_copy(locp->file, CX_NAME, file);
    }
}

/* Append one sample to p. run and loc are stored as given, and both
   measured_us and requested_us are set to us. Increments that run's
   sample count. Fails through crash_if when nsamples is already at
   sample_cap. Does not check that run or loc are in range and does not
   set a thread id. */
static void add_sample(Profile *p, int run, int loc, long long us) {
    Sample *s;
    crash_if(p->nsamples >= p->sample_cap);
    s = &p->samples[p->nsamples++];
    memset(s, 0, sizeof(*s));
    s->run = run;
    s->loc = loc;
    s->measured_us = us;
    s->requested_us = us;
    p->runs[run].samples++;
}

/* Build a four-run profile in a temp directory and check function and
   block grouping. Two lines of foo stay one function and two blocks.
   bar is a different function. An unsymbolized offset is named by
   module plus offset. baz at offsets 0x4000 and 0x4008 share a 16-byte
   block and 0x4010 does not. Writes ledger_functions.txt and requires
   the foo label plus the pause totals 140 and 150 as their own columns.
   Frees the arena and deletes the directory. Uses g_test_opt, g_tx,
   g_ty, g_wjoin, and g_file. */
static void test_groups(void) {
    Arena a;
    Profile *p;
    int mod, foo0, foo1, bar, nosym, baz0, baz8, baz10;
    int row_of[8];
    int nrows, foo, i;
    memset(&a, 0, sizeof(a));
    options_default(&g_test_opt);
    g_test_opt.runs = 4;
    g_test_opt.samples = 8;
    make_temp_dir(g_test_opt.out_dir, CX_PATH, "causext_gst");
    delete_out(g_test_opt.out_dir);
    crash_if(!ensure_dir(g_test_opt.out_dir));
    crash_if(!arena_init(&a, 16ULL * 1024ULL * 1024ULL));
    p = profile_new(&a, &g_test_opt);
    crash_if(!p);
    mod = intern_module(p, L"C:\\app\\app.exe", L"app.exe", 0);
    crash_if(mod < 0);
    foo0 = intern_loc(p, mod, 0x1000);
    foo1 = intern_loc(p, mod, 0x1020);
    bar = intern_loc(p, mod, 0x2000);
    nosym = intern_loc(p, mod, 0x3000);
    baz0 = intern_loc(p, mod, 0x4000);
    baz8 = intern_loc(p, mod, 0x4008);
    baz10 = intern_loc(p, mod, 0x4010);
    crash_if(foo0 < 0 || foo1 < 0 || bar < 0 || nosym < 0 || baz0 < 0 || baz8 < 0 || baz10 < 0);
    set_sym(p, foo0, "foo", 10, 0x1000, "C:\\src\\app.c");
    set_sym(p, foo1, "foo", 11, 0x1000, "C:\\src\\app.c");
    set_sym(p, bar, "bar", 40, 0x2000, "C:\\src\\app.c");
    set_sym(p, baz0, "baz", 0, 0x4000, "");
    set_sym(p, baz8, "baz", 0, 0x4000, "");
    set_sym(p, baz10, "baz", 0, 0x4000, "");
    p->nruns = 4;
    /* Mark every run started and completed with a rising end-to-end time. */
    for (i = 0; i < 4; i++) {
        p->runs[i].started = 1;
        p->runs[i].completed = 1;
        p->runs[i].e2e_us = (long long)(i + 1) * 1000;
    }
    add_sample(p, 0, foo0, 100);
    add_sample(p, 0, foo1, 40);
    add_sample(p, 0, bar, 5);
    add_sample(p, 1, foo0, 10);
    add_sample(p, 1, nosym, 7);
    classify_groups(p);
    tally_groups(p);
    crash_if(p->locs[foo0].func_id != p->locs[foo1].func_id);
    crash_if(p->locs[foo0].func_id == p->locs[bar].func_id);
    crash_if(p->locs[foo0].func_id == p->locs[nosym].func_id);
    crash_if(p->locs[baz0].func_id != p->locs[baz8].func_id);
    crash_if(p->locs[baz0].func_id != p->locs[baz10].func_id);
    crash_if(p->locs[baz0].func_id == p->locs[foo0].func_id);
    crash_if(p->locs[foo0].block_id == p->locs[foo1].block_id);
    crash_if(p->locs[baz0].block_id != p->locs[baz8].block_id);
    crash_if(p->locs[baz0].block_id == p->locs[baz10].block_id);
    foo = find_group(p->funcs, p->nfuncs, "app.exe!foo");
    crash_if(foo < 0);
    crash_if(p->funcs[foo].total_us != 150);
    crash_if(p->funcs[foo].members != 2);
    crash_if(find_group(p->funcs, p->nfuncs, "app.exe!bar") < 0);
    crash_if(p->funcs[find_group(p->funcs, p->nfuncs, "app.exe!bar")].total_us != 5);
    crash_if(find_group(p->funcs, p->nfuncs, "app.exe+0x3000") < 0);
    crash_if(p->funcs[find_group(p->funcs, p->nfuncs, "app.exe+0x3000")].total_us != 7);
    crash_if(find_group(p->blocks, p->nblocks, "app.exe!baz+0x0") < 0);
    crash_if(find_group(p->blocks, p->nblocks, "app.exe!baz+0x10") < 0);
    nrows = prepare_rows(p, row_of, g_ty);
    crash_if(nrows != 4);
    accumulate(p, GRAIN_FUNC, row_of, g_tx, nrows, p->nfuncs);
    crash_if(!write_derived_ledger(p, GRAIN_FUNC, L"ledger_functions.txt", row_of, g_tx, nrows, p->nfuncs));
    crash_if(!w_join(g_wjoin, CX_PATH, g_test_opt.out_dir, L"ledger_functions.txt"));
    crash_if(read_all(g_wjoin, g_file, (int)sizeof(g_file)) < 0);
    crash_if(!str_contains(g_file, "app.exe!foo"));
    crash_if(!str_contains(g_file, " 140"));
    crash_if(!str_contains(g_file, " 150 "));
    arena_free(&a);
    delete_out(g_test_opt.out_dir);
}

/* Ask snap_threads for this process and require at least one thread
   id. Uses a 16-entry stack buffer. Fails through crash_if. Does not
   check which thread was returned. */
static void test_toolhelp(void) {
    DWORD tids[16];
    int n = snap_threads(GetCurrentProcessId(), tids, 16);
    crash_if(n < 1);
}

/* Snapshot this process's modules and require one whose name contains
   "causext". Uses a 16 MiB arena and g_snaps. Fails through crash_if
   when the snapshot is empty or this module is missing. Does not
   symbolize. */
static void test_modules(void) {
    Arena a;
    Profile *p;
    int n, i, saw = 0;
    memset(&a, 0, sizeof(a));
    options_default(&g_test_opt);
    g_test_opt.runs = 1;
    g_test_opt.samples = 1;
    crash_if(!arena_init(&a, 16ULL * 1024ULL * 1024ULL));
    p = profile_new(&a, &g_test_opt);
    crash_if(!p);
    n = snap_modules(p, GetCurrentProcessId(), g_snaps, CX_MAX_MODS);
    crash_if(n < 1);
    /* Look through interned module names for this executable. */
    for (i = 0; i < p->nmods; i++) {
        /* Remember that this module was in the snapshot. */
        if (str_contains(p->mods[i].name, "causext")) saw = 1;
    }
    crash_if(!saw);
    arena_free(&a);
}

/* Thread procedure that increments g_spin_n until g_spin_run is zero.
   param is unused. Returns 0. Does not sleep, so a suspend leaves the
   instruction pointer inside this loop. */
static DWORD __stdcall spin_fn(void *param) {
    (void)param;
    /* Keep counting while the test still wants this thread suspended. */
    while (g_spin_run) g_spin_n++;
    return 0;
}

/* Start spin_fn, suspend it, and read its instruction pointer through
   the same CONTEXT layout the profiler uses. Requires GetThreadContext
   to succeed and RIP to lie from 0x10000 up to but not including
   0x800000000000. Then resumes the thread, clears g_spin_run, and waits
   up to 3000 milliseconds for it to exit. Fails through crash_if. Does
   not symbolize the pointer. */
static void test_context(void) {
    HANDLE th, ot;
    DWORD tid = 0, prev;
    unsigned int flags;
    unsigned long long rip = 0;
    int spins;
    g_spin_run = 1;
    g_spin_n = 0;
    th = CreateThread(NULL, 0, spin_fn, NULL, 0, &tid);
    crash_if(!th);
    /* Wait until the new thread has incremented its counter. */
    for (spins = 0; spins < 1000 && g_spin_n == 0; spins++) Sleep(1);
    crash_if(g_spin_n == 0);
    ot = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED, FALSE, tid);
    crash_if(!ot);
    prev = SuspendThread(ot);
    crash_if(prev == 0xFFFFFFFFu);
    memset(g_context, 0, sizeof(g_context));
    flags = CONTEXT_AMD64_CONTROL;
    memcpy(g_context + CONTEXT_FLAGS_OFFSET, &flags, sizeof(flags));
    crash_if(!GetThreadContext(ot, g_context));
    memcpy(&rip, g_context + CONTEXT_RIP_OFFSET, sizeof(rip));
    crash_if(rip < 0x10000ULL);
    crash_if(rip >= 0x800000000000ULL);
    crash_if(ResumeThread(ot) == 0xFFFFFFFFu);
    CloseHandle(ot);
    g_spin_run = 0;
    crash_if(WaitForSingleObject(th, 3000) != WAIT_OBJECT_0);
    CloseHandle(th);
}

/* Copy one line from *pp into dst and advance *pp past it. cap is the
   byte capacity. Stops at LF, CR, or a full buffer, then skips one CR
   and one LF when present. Returns 0 when pp is NULL or the remaining
   text is empty, without writing dst. Returns 1 after storing a
   NUL-terminated line, which may be empty when the text starts with a
   break. A line longer than cap-1 is split across later calls. */
static int line_next(const char **pp, char *dst, int cap) {
    const char *p = *pp;
    int n = 0;
    /* No remaining text means there is no line. */
    if (!p || !*p) return 0;
    /* Copy characters until a line break or the last byte of dst. */
    while (*p && *p != '\n' && *p != '\r' && n + 1 < cap) dst[n++] = *p++;
    dst[n] = 0;
    /* Swallow a CR, including the CR of a CRLF pair. */
    if (*p == '\r') p++;
    /* Swallow a lone LF or the LF after that CR. */
    if (*p == '\n') p++;
    *pp = p;
    return 1;
}

/* Parse a simple decimal from s. An optional leading '-' is accepted.
   Digits before a dot are the integer part; digits after a dot are
   tenths, hundredths, and so on. Returns the value, or 0 when s is
   NULL. Stops at the first character that is not part of that form, so
   a trailing word is ignored and no exponent is accepted. Does not
   report a parse failure separately from the value 0. */
static double parse_double(const char *s) {
    int neg = 0, i = 0;
    double v = 0, place = 0.1;
    /* A missing string is the same number as a zero field. */
    if (!s) return 0;
    /* Remember a leading minus. */
    if (s[0] == '-') {
        neg = 1;
        i++;
    }
    /* Accumulate the integer part. */
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10.0 + (double)(s[i] - '0');
        i++;
    }
    /* Consume a fractional part after a decimal point. */
    if (s[i] == '.') {
        i++;
        /* Add each fractional digit at the next decimal place. */
        while (s[i] >= '0' && s[i] <= '9') {
            v += (double)(s[i] - '0') * place;
            place *= 0.1;
            i++;
        }
    }
    return neg ? -v : v;
}

/* Return 1 when the first character of s that is not a space is a digit.
   A NULL or blank s returns 0. Leading spaces are the padding of a
   right-aligned rank column. */
static int starts_with_digit(const char *s) {
    /* Rank numbers are padded on the left. */
    while (s && *s == ' ') s++;
    return s && *s >= '0' && *s <= '9';
}

/* Split line on runs of spaces into at most nfield fields. The last
   field is the remainder of the line, so a label may contain spaces.
   Returns how many fields were stored in f. A blank line returns 0.
   Overwrites the separating spaces with NULs. Does not allocate. */
static int split_fields(char *line, char **f, int nfield) {
    int i = 0, nf = 0;
    /* Skip padding before the first column. */
    while (line[i] == ' ') i++;
    /* A blank line has no fields. */
    if (!line[i] || nfield < 1) return 0;
    /* Take fields until the line ends or the last field absorbs the rest. */
    while (nf < nfield) {
        f[nf++] = line + i;
        /* The final field keeps its internal spaces. */
        if (nf == nfield) return nf;
        /* Walk to the gap before the next column. */
        while (line[i] && line[i] != ' ') i++;
        /* No further column. */
        if (!line[i]) return nf;
        line[i++] = 0;
        /* Padding between columns is more than one space. */
        while (line[i] == ' ') i++;
        /* A trailing gap is not another field. */
        if (!line[i]) return nf;
    }
    return nf;
}

/* Find a TABLE row in text whose label contains needle and write the
   ridge beta into *beta. Columns are space-aligned. The label is the
   eleventh field and may contain spaces. Scanning starts after a line
   equal to TABLE and stops at ENDTABLE, returning 0 with *beta
   unchanged. Rows that do not start with a digit are skipped.
   parse_double reads the third field. Returns 1 on the first match.
   Does not search past ENDTABLE. */
static int table_beta(const char *text, const char *needle, double *beta) {
    const char *p = text;
    char line[640];
    int in_table = 0;
    /* Read lines until the table ends or the text runs out. */
    while (line_next(&p, line, (int)sizeof(line))) {
        /* Start accepting rows after the table marker. */
        if (str_eq(line, "TABLE")) {
            in_table = 1;
            continue;
        }
        /* Stop with no match at the end of the table. */
        if (str_eq(line, "ENDTABLE")) return 0;
        /* Ignore prologue lines before TABLE. */
        if (!in_table) continue;
        /* Skip the header and other non-data rows. */
        if (!starts_with_digit(line)) continue;
        /* Split this row. The label is the eleventh field. */
        {
            char *f[11];
            int nf = split_fields(line, f, 11);
            /* This row names the needle, so take its beta. */
            if (nf >= 11 && str_contains(f[10], needle)) {
                *beta = parse_double(f[2]);
                return 1;
            }
        }
    }
    return 0;
}

/* Find a TABLE row in text whose label contains needle and write the
   sample percent into *percent. Columns are space-aligned. The label
   is the fourth field and may contain spaces. Scanning starts after a
   line equal to TABLE and stops at ENDTABLE, returning 0 with *percent
   unchanged. Rows that do not start with a digit are skipped.
   parse_double reads the third field. Returns 1 on the first match.
   Does not search past ENDTABLE. */
static int table_percent(const char *text, const char *needle, double *percent) {
    const char *p = text;
    char line[640];
    int in_table = 0;
    /* Read lines until the table ends or the text runs out. */
    while (line_next(&p, line, (int)sizeof(line))) {
        /* Start accepting rows after the table marker. */
        if (str_eq(line, "TABLE")) {
            in_table = 1;
            continue;
        }
        /* Stop with no match at the end of the table. */
        if (str_eq(line, "ENDTABLE")) return 0;
        /* Ignore prologue lines before TABLE. */
        if (!in_table) continue;
        /* Skip the header and other non-data rows. */
        if (!starts_with_digit(line)) continue;
        /* Split this row. The label is the fourth field. */
        {
            char *f[4];
            int nf = split_fields(line, f, 4);
            /* This row names the needle, so take its percent. */
            if (nf >= 4 && str_contains(f[3], needle)) {
                *percent = parse_double(f[2]);
                return 1;
            }
        }
    }
    return 0;
}

/* Log up to 24 lines from text that contain needle, "note", "sizeof",
   or "FIT". Each logged line is followed by a newline. Returns nothing.
   Does not fail the test by itself. A line longer than 299 bytes is
   split by line_next before the match is tested. */
static void test_log_hits(const char *text, const char *needle) {
    const char *p = text;
    char line[300];
    int shown = 0;
    /* Walk lines until 24 matches have been logged or the text ends. */
    while (line_next(&p, line, (int)sizeof(line)) && shown < 24) {
        /* Log a line that mentions the needle or a rank-header marker. */
        if (str_contains(line, needle) || str_contains(line, "note") || str_contains(line, "sizeof") ||
            str_contains(line, "FIT")) {
            test_log(line);
            test_log("\n");
            shown++;
        }
    }
}

/* Profile this executable running --burn 4500 and require the causal
   rank to separate burn_critical from burn_slack. Uses 16 runs, 6
   pauses, pauses of 120000 to 400000 microseconds, a 2500000
   microsecond warmup, a 20000 millisecond timeout, seed 42, and a temp
   directory. Requires return code 0, at least 80 samples, at least 14
   completed runs, and no timeouts. Requires both function rows, and a
   critical beta above 0.35 and at least 0.25 above the slack beta.
   Logs the status and the betas. Deletes the temp directory. Does not
   delete out\\self_test.txt. */
static void test_smoke(void) {
    ProfileStatus st;
    wchar_t self[CX_PATH];
    double bc = 0, bs = 0;
    int rc, n, got_c, got_s;
    DWORD nself;
    options_default(&g_test_opt);
    nself = GetModuleFileNameW(NULL, self, CX_PATH);
    crash_if(nself == 0 || nself >= CX_PATH);
    g_test_opt.command[0] = 0;
    crash_if(!append_quoted(g_test_opt.command, 8192, self));
    w_append_ascii(g_test_opt.command, 8192, " --burn 4500");
    /* The burn spends about 1.3s settling and calibrating before the timed
       loop. Warmup has to cover that prefix or a pause there changes how
       many iterations the loop runs. Each later injection also snapshots
       threads before the suspend, so the timed loop has to outlast the
       whole injection schedule. */
    g_test_opt.runs = 16;
    g_test_opt.samples = 6;
    g_test_opt.pause_min_us = 120000;
    g_test_opt.pause_max_us = 400000;
    g_test_opt.gap_min_us = 200;
    g_test_opt.gap_max_us = 800;
    g_test_opt.warmup_us = 2500000;
    g_test_opt.timeout_ms = 20000;
    g_test_opt.quiet_child = 1;
    g_test_opt.permutations = 10;
    g_test_opt.seed = 42;
    g_test_opt.seed_set = 1;
    make_temp_dir(g_test_opt.out_dir, CX_PATH, "causext_st");
    delete_out(g_test_opt.out_dir);
    /* One unprofiled burn brings the CPU out of idle before the first
       measured run. That first clock sample is otherwise still ramping. */
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);
        g_cmd_mut[0] = 0;
        /* Launch one unprofiled burn when the command fits. */
        if (append_quoted(g_cmd_mut, 8192, self)) {
            w_append_ascii(g_cmd_mut, 8192, " --burn 1500");
            /* Wait for that burn, then close its handles. */
            if (CreateProcessW(NULL, g_cmd_mut, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 20000);
                /* Close the thread handle when create returned one. */
                if (pi.hThread) CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }
    test_log("smoke profile\n");
    rc = run_profile(&g_test_opt, &st);
    test_log("smoke rc ");
    /* Log the profile status counts. */
    {
        char b[32];
        format_i64(b, (int)sizeof(b), rc);
        test_log(b);
        test_log(" samples ");
        format_i64(b, (int)sizeof(b), st.samples);
        test_log(b);
        test_log(" completed ");
        format_i64(b, (int)sizeof(b), st.runs_completed);
        test_log(b);
        test_log(" timeouts ");
        format_i64(b, (int)sizeof(b), st.runs_timed_out);
        test_log(b);
        test_log("\n");
    }
    /* Stop when the live profile missed the sample or timeout bar. */
    if (rc != 0 || st.samples < 80 || st.runs_completed < 14 || st.runs_timed_out != 0) {
        test_log("smoke status failed\n");
        test_fail_at_line(__LINE__);
    }
    crash_if(!w_join(g_wjoin, CX_PATH, g_test_opt.out_dir, L"symbols.txt"));
    n = read_all(g_wjoin, g_file, (int)sizeof(g_file));
    /* Note a missing symbols file, otherwise log lines that mention burn. */
    if (n < 0) test_log("symbols.txt missing\n");
    /* Log burn lines from the symbols file. */
    else test_log_hits(g_file, "burn");
    crash_if(!w_join(g_wjoin, CX_PATH, g_test_opt.out_dir, L"rank_functions.txt"));
    n = read_all(g_wjoin, g_file, (int)sizeof(g_file));
    crash_if(n < 0);
    got_c = table_beta(g_file, "burn_critical", &bc);
    got_s = table_beta(g_file, "burn_slack", &bs);
    test_log("critical ");
    test_log_fixed(bc);
    test_log(" slack ");
    test_log_fixed(bs);
    test_log("\n");
    /* Stop when either burn function is absent from the rank table. */
    if (!got_c || !got_s) {
        test_log("rank rows:\n");
        test_log_hits(g_file, "burn");
        test_fail_at_line(__LINE__);
    }
    /* Stop when the critical beta is weak or too close to slack. */
    if (!(bc > 0.35) || !(bc > bs + 0.25)) {
        test_log("causal contrast failed\n");
        test_fail_at_line(__LINE__);
    }
    delete_out(g_test_opt.out_dir);
}

/* Profile this executable in sampling mode with --burn 2500 and require
   both burn functions to hold more than 5 percent of samples. Uses 1
   run, 40 waves, gaps of 200 to 800 microseconds, a 2200000 microsecond
   warmup, a 15000 millisecond timeout, seed 42, and a temp directory.
   Requires return code 0, at least 16 samples, a completed run, no
   timeout, and the words "Sampling profile" in the rank file. Logs the
   shares. Deletes the temp directory. Does not inject pauses. */
static void test_sampling(void) {
    ProfileStatus st;
    wchar_t self[CX_PATH];
    double pc = 0, ps = 0;
    int rc, n, got_c, got_s;
    DWORD nself;
    options_default(&g_test_opt);
    g_test_opt.sampling = 1;
    nself = GetModuleFileNameW(NULL, self, CX_PATH);
    crash_if(nself == 0 || nself >= CX_PATH);
    g_test_opt.command[0] = 0;
    crash_if(!append_quoted(g_test_opt.command, 8192, self));
    w_append_ascii(g_test_opt.command, 8192, " --burn 2500");
    g_test_opt.runs = 1;
    g_test_opt.samples = 40;
    g_test_opt.gap_min_us = 200;
    g_test_opt.gap_max_us = 800;
    g_test_opt.warmup_us = 2200000;
    g_test_opt.timeout_ms = 15000;
    g_test_opt.quiet_child = 1;
    g_test_opt.seed = 42;
    g_test_opt.seed_set = 1;
    make_temp_dir(g_test_opt.out_dir, CX_PATH, "causext_sp");
    delete_out(g_test_opt.out_dir);
    test_log("sampling profile\n");
    rc = run_profile(&g_test_opt, &st);
    test_log("sampling rc ");
    /* Log the sampling status counts. */
    {
        char b[32];
        format_i64(b, (int)sizeof(b), rc);
        test_log(b);
        test_log(" samples ");
        format_i64(b, (int)sizeof(b), st.samples);
        test_log(b);
        test_log("\n");
    }
    /* Stop when the sampling run missed its sample or timeout bar. */
    if (rc != 0 || st.samples < 16 || st.runs_completed < 1 || st.runs_timed_out != 0) {
        test_log("sampling status failed\n");
        test_fail_at_line(__LINE__);
    }
    crash_if(!w_join(g_wjoin, CX_PATH, g_test_opt.out_dir, L"rank_functions.txt"));
    n = read_all(g_wjoin, g_file, (int)sizeof(g_file));
    crash_if(n < 0);
    crash_if(!str_contains(g_file, "Sampling profile"));
    got_c = table_percent(g_file, "burn_critical", &pc);
    got_s = table_percent(g_file, "burn_slack", &ps);
    test_log("sample critical ");
    test_log_fixed(pc);
    test_log(" slack ");
    test_log_fixed(ps);
    test_log("\n");
    /* Stop when either burn function is absent from the sample rank. */
    if (!got_c || !got_s) {
        test_log("sampling rank rows:\n");
        test_log_hits(g_file, "burn");
        test_fail_at_line(__LINE__);
    }
    /* Stop when either function has five percent of the samples or less. */
    if (!(pc > 5.0) || !(ps > 5.0)) {
        test_log("sampling share failed\n");
        test_fail_at_line(__LINE__);
    }
    delete_out(g_test_opt.out_dir);
}

/* Run every self-test and then exit. Creates the directory "out" and
   the log out\\self_test.txt, ignoring a failure to create either.
   Calls the oracle, fuzz, parse, group, snapshot, context, smoke, and
   sampling tests in that order. On success logs "ALL TESTS PASSED",
   closes the log when it opened, and ExitProcess with status 0. A
   failed check exits earlier from test_fail_at_line. Does not return. */
__declspec(noreturn) static void run_self_test(void) {
    CreateDirectoryW(L"out", NULL);
    g_test_log = CreateFileW(L"out\\self_test.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    test_log("causext self-test\n");
    test_sqrt_format();
    test_log("sqrt and format ok\n");
    test_solve();
    test_log("solve ok\n");
    test_ols_ridge_oracles();
    test_log("oracles ok\n");
    test_fit_cases();
    test_log("fit cases ok\n");
    test_fuzz();
    test_log("fuzz ok\n");
    test_quote_parse();
    test_log("parse ok\n");
    test_groups();
    test_log("groups ok\n");
    test_toolhelp();
    test_modules();
    test_log("snapshots ok\n");
    test_context();
    test_log("context ok\n");
    test_smoke();
    test_sampling();
    test_log("ALL TESTS PASSED\n");
    /* Close the log when CreateFileW returned a handle. */
    if (g_test_log && g_test_log != INVALID_HANDLE_VALUE) CloseHandle(g_test_log);
    ExitProcess(0);
}

/* Dispatch the process from the Windows command line. Converts the
   command line with CommandLineToArgvW, fills defaults, and parses
   options. A missing argv or a parse error prints g_opt.error and
   returns 1. MODE_HELP prints help and returns 0. MODE_SELF_TEST and
   MODE_BURN do not return. Profile mode validates options, prints
   g_opt.warn when that text is non-empty, and returns whatever
   run_profile returns. Frees the argv block after parsing. Does not
   call qpc_init. */
static int causext_main(void) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    /* CommandLineToArgvW failed, so there is nothing to parse. */
    if (!argv) return 1;
    options_default(&g_opt);
    /* Print the parse error and stop. */
    if (parse_options(argc, argv, &g_opt) != 0) {
        con_err(g_opt.error);
        con_err("\n");
        LocalFree(argv);
        return 1;
    }
    LocalFree(argv);
    /* Print usage and exit successfully. */
    if (g_opt.mode == MODE_HELP) {
        print_help();
        return 0;
    }
    /* Hand the process to the self-test, which does not return. */
    if (g_opt.mode == MODE_SELF_TEST) run_self_test();
    /* Hand the process to the burn workload, which does not return. */
    if (g_opt.mode == MODE_BURN) run_burn(g_opt.burn_ms);
    /* Print the validation error and stop. */
    if (finish_options(&g_opt) != 0) {
        con_err(g_opt.error);
        con_err("\n");
        return 1;
    }
    /* Print a clamp warning and still run the profile. */
    if (g_opt.warn[0]) {
        con_err(g_opt.warn);
        con_err("\n");
    }
    return run_profile(&g_opt, NULL);
}

/* Process entry used instead of the C runtime. Initializes the
   performance counter, then exits with the status causext_main returns.
   Does not return and does not install an exception filter. */
void __stdcall mainCRTStartup(void) {
    qpc_init();
    ExitProcess((DWORD)causext_main());
}


