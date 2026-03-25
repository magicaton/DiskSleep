// noCRT runtime.
// Provides implementations of CRT functions using Win32 / ntdll equivalents.
// Only compiled in noCRT configurations (NOCRT defined).
//
// WholeProgramOptimization must be disabled for this file in Release-noCRT
// because #pragma function(memset/memcpy) conflicts with LTCG (/GL).

#include <windows.h>
#include <stdarg.h>
#include "cmdline.h"

#ifdef NOCRT

typedef int errno_t;

// ---------------------------------------------------------------------------
// ntdll _vsnwprintf
// ---------------------------------------------------------------------------

typedef int (__cdecl *pfn_vsnwprintf)(wchar_t *, size_t, const wchar_t *, va_list);
static pfn_vsnwprintf s_vsnwprintf = NULL;

void nocrt_init(void) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
        s_vsnwprintf = (pfn_vsnwprintf)GetProcAddress(ntdll, "_vsnwprintf");
}

// ---------------------------------------------------------------------------
// Formatted string output
// ---------------------------------------------------------------------------
// The UCRT headers define _snwprintf_s / _vsnwprintf_s / swprintf_s as
// __inline wrappers that call __stdio_common_vsnwprintf_s (or _vswprintf_s).
// We implement the underlying __stdio_common_* functions instead so the
// CRT inline wrappers resolve correctly without linking the UCRT DLL.

static int nocrt_vformat(wchar_t *buf, size_t bufCount,
                         const wchar_t *fmt, va_list args) {
    if (!buf || bufCount == 0) return -1;
    if (!s_vsnwprintf) { buf[0] = L'\0'; return -1; }
    int r = s_vsnwprintf(buf, bufCount, fmt, args);
    // ntdll _vsnwprintf does not null-terminate on truncation.
    buf[bufCount - 1] = L'\0';
    if (r < 0) r = (int)lstrlenW(buf);
    return r;
}

int __cdecl __stdio_common_vsnwprintf_s(
        unsigned __int64 options,
        wchar_t *buf, size_t bufCount, size_t maxCount,
        const wchar_t *fmt, void *locale, va_list args) {
    return nocrt_vformat(buf, bufCount, fmt, args);
}

int __cdecl __stdio_common_vswprintf_s(
        unsigned __int64 options,
        wchar_t *buf, size_t bufCount,
        const wchar_t *fmt, void *locale, va_list args) {
    return nocrt_vformat(buf, bufCount, fmt, args);
}

// ---------------------------------------------------------------------------
// String functions
// ---------------------------------------------------------------------------

errno_t wcscpy_s(wchar_t *dst, size_t dstSize, const wchar_t *src) {
    if (!dst || dstSize == 0) return 22; // EINVAL
    if (!src) { dst[0] = L'\0'; return 22; }
    lstrcpynW(dst, src, (int)dstSize);
    return 0;
}

int _wcsicmp(const wchar_t *s1, const wchar_t *s2) {
    return lstrcmpiW(s1, s2);
}

#pragma function(wcslen)
size_t wcslen(const wchar_t *s) {
    return (size_t)lstrlenW(s);
}

wchar_t *wcschr(const wchar_t *s, wchar_t c) {
    for (; *s; s++)
        if (*s == c) return (wchar_t *)s;
    return (c == L'\0') ? (wchar_t *)s : NULL;
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
    const wchar_t *last = NULL;
    for (; *s; s++)
        if (*s == c) last = s;
    if (c == L'\0') return (wchar_t *)s;
    return (wchar_t *)last;
}

int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (s1[i] < s2[i]) ? -1 : 1;
        if (s1[i] == L'\0') return 0;
    }
    return 0;
}

unsigned long wcstoul(const wchar_t *s, wchar_t **endp, int base) {
    unsigned long result = 0;
    const wchar_t *p = s;
    while (*p == L' ' || *p == L'\t') p++;
    while (*p) {
        int d = -1;
        if (*p >= L'0' && *p <= L'9') d = *p - L'0';
        else if (base == 16 && *p >= L'a' && *p <= L'f') d = *p - L'a' + 10;
        else if (base == 16 && *p >= L'A' && *p <= L'F') d = *p - L'A' + 10;
        if (d < 0 || d >= base) break;
        result = result * (unsigned long)base + (unsigned long)d;
        p++;
    }
    if (endp) *endp = (wchar_t *)p;
    return result;
}

// ---------------------------------------------------------------------------
// Memory functions (compiler emits implicit calls for = {0}, struct copy)
// ---------------------------------------------------------------------------

#pragma function(memset)
void *memset(void *dst, int val, size_t count) {
    __stosb((unsigned char *)dst, (unsigned char)val, count);
    return dst;
}

#pragma function(memcpy)
void *memcpy(void *dst, const void *src, size_t count) {
    __movsb((unsigned char *)dst, (const unsigned char *)src, count);
    return dst;
}

// ---------------------------------------------------------------------------
// Heap allocation (replacing malloc / calloc / free)
// ---------------------------------------------------------------------------

void *malloc(size_t size) {
    return HeapAlloc(GetProcessHeap(), 0, size);
}

void *calloc(size_t count, size_t size) {
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * size);
}

void free(void *ptr) {
    if (ptr) HeapFree(GetProcessHeap(), 0, ptr);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int wmain(int argc, wchar_t **argv);

void wmainCRTStartup(void) {
    nocrt_init();

    const wchar_t *cmdline = GetCommandLineW();

    // CMDLINE_ARGV_MAX_W is ~196 KB — too large for the stack without __chkstk
    // having run yet.  Use the heap instead.
    wchar_t **argv = (wchar_t **)HeapAlloc(
        GetProcessHeap(), 0, CMDLINE_ARGV_MAX_W * sizeof(wchar_t *));
    if (!argv) ExitProcess(1);

    int argc = cmdline_to_argvW(cmdline, argv);
    int result = wmain(argc, argv);
    ExitProcess((UINT)result);
}

#endif // NOCRT
