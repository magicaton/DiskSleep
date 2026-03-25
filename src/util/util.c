#include "util.h"
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

HANDLE g_OutHandle = INVALID_HANDLE_VALUE;
static BOOL g_OutIsConsole = FALSE;

void output_init(void) {
    g_OutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    g_OutIsConsole = GetConsoleMode(g_OutHandle, &mode);
}

void output_set_handle(HANDLE h) {
    g_OutHandle = h;
    DWORD mode;
    g_OutIsConsole = GetConsoleMode(h, &mode);
}

void PrintMsg(const wchar_t *fmt, ...) {
    wchar_t buf[4096];
    va_list args;
    va_start(args, fmt);
    int len = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    if (len > 0) {
        DWORD written;
        if (g_OutIsConsole) {
            WriteConsoleW(g_OutHandle, buf, (DWORD)len, &written, NULL);
        } else {
            // Convert to UTF-8 for pipes, files, and mintty/pty.
            char utf8[4096 * 4];
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, buf, len,
                                              utf8, sizeof(utf8), NULL, NULL);
            if (utf8Len > 0)
                WriteFile(g_OutHandle, utf8, (DWORD)utf8Len, &written, NULL);
        }
    }
}

void eprint(const wchar_t *context, DWORD err) {
    wchar_t *msg = NULL;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, err, 0, (LPWSTR)&msg, 0, NULL);
    if (msg) {
        size_t len = wcslen(msg);
        while (len > 0 && (msg[len - 1] == L'\r' || msg[len - 1] == L'\n'))
            msg[--len] = L'\0';
        PrintMsg(L"Error: %ls: %ls (0x%08X)\n", context, msg, err);
        LocalFree(msg);
    } else {
        PrintMsg(L"Error: %ls (0x%08X)\n", context, err);
    }
}

const wchar_t *exe_dir(void) {
    static wchar_t dir[MAX_PATH];
    if (dir[0] != L'\0') return dir;
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *sep = wcsrchr(dir, L'\\');
    if (sep) *sep = L'\0';
    return dir;
}

wchar_t norm_letter(wchar_t c) {
    if (c >= L'a' && c <= L'z') return (wchar_t)(c - L'a' + L'A');
    if (c >= L'A' && c <= L'Z') return c;
    return 0;
}

void format_size(ULONGLONG bytes, wchar_t *buf, size_t bufLen) {
    if (bytes >= 1024ULL * 1024 * 1024 * 1024) {
        ULONGLONG div = 1024ULL * 1024 * 1024 * 1024;
        ULONGLONG whole = bytes / div;
        ULONGLONG tenths = ((bytes % div) * 10 + (div / 2)) / div;
        if (tenths >= 10) { whole++; tenths -= 10; }
        _snwprintf_s(buf, bufLen, _TRUNCATE, L"%I64u.%I64u TB", whole, tenths);
    } else if (bytes >= 1024ULL * 1024 * 1024) {
        ULONGLONG div = 1024ULL * 1024 * 1024;
        ULONGLONG whole = bytes / div;
        ULONGLONG tenths = ((bytes % div) * 10 + (div / 2)) / div;
        if (tenths >= 10) { whole++; tenths -= 10; }
        _snwprintf_s(buf, bufLen, _TRUNCATE, L"%I64u.%I64u GB", whole, tenths);
    } else {
        ULONGLONG div = 1024ULL * 1024;
        ULONGLONG whole = (bytes + (div / 2)) / div;
        _snwprintf_s(buf, bufLen, _TRUNCATE, L"%I64u MB", whole);
    }
}
