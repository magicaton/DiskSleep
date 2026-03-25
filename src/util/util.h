#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Global output handle. All console/pipe output goes through this.
// Set to STD_OUTPUT_HANDLE at startup; worker mode redirects it to the pipe.
extern HANDLE g_OutHandle;

// Initialize g_OutHandle to STD_OUTPUT_HANDLE.
void output_init(void);

// Set a custom output handle (e.g., pipe handle for worker mode).
void output_set_handle(HANDLE h);

// Formatted Unicode output through g_OutHandle (WriteFile, no CRT buffering).
void PrintMsg(const wchar_t *fmt, ...);

// Print "context: Win32 error message" through g_OutHandle.
void eprint(const wchar_t *context, DWORD err);

// Return the directory containing the running executable (no trailing backslash).
// Returns a pointer to a static buffer; valid for the lifetime of the process.
const wchar_t *exe_dir(void);

// Normalize a drive letter character to uppercase.
// Accepts 'a'-'z' or 'A'-'Z'. Returns 0 if not a valid drive letter character.
wchar_t norm_letter(wchar_t c);

// Format byte count as human-readable size (e.g. "1.8 TB", "500 GB", "128 MB").
// Writes into buf (bufLen in wchar_t units).
void format_size(ULONGLONG bytes, wchar_t *buf, size_t bufLen);
