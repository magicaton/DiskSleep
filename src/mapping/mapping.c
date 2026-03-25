#include "mapping.h"
#include "util.h"
#include <stdio.h>

#define REG_KEY L"Software\\DiskSleep\\Map"
#define MAP_FILE L"DiskSleep_map.ini"

void map_file_path(wchar_t *out, DWORD outLen) {
    _snwprintf_s(out, outLen, _TRUNCATE, L"%ls\\%ls", exe_dir(), MAP_FILE);
}

// Split a value string on '|' into guid and pnpId parts.
static void split_entry(wchar_t *value, wchar_t *guidOut, wchar_t *pnpIdOut) {
    wchar_t *pipe = wcschr(value, L'|');
    if (pipe) {
        *pipe = L'\0';
        wcscpy_s(guidOut, MAX_PATH, value);
        wcscpy_s(pnpIdOut, MAX_PATH, pipe + 1);
    } else {
        wcscpy_s(guidOut, MAX_PATH, value);
        pnpIdOut[0] = L'\0';
    }
}

// ---------------------------------------------------------------------------
// File I/O — KEY=VALUE format, one entry per line, no sections.
// noCRT variant uses Win32 APIs; CRT variant uses stdio.
// ---------------------------------------------------------------------------

#ifdef NOCRT

BOOL map_read_file(MapTable t) {
    memset(t, 0, sizeof(MapTable));

    wchar_t path[MAX_PATH];
    map_file_path(path, MAX_PATH);

    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            eprint(L"map_read_file: CreateFileW", GetLastError());
        return TRUE;
    }

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
        CloseHandle(hFile);
        return TRUE;
    }

    char *raw = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)fileSize + 1);
    if (!raw) { CloseHandle(hFile); return TRUE; }

    DWORD bytesRead;
    if (!ReadFile(hFile, raw, fileSize, &bytesRead, NULL)) {
        HeapFree(GetProcessHeap(), 0, raw);
        CloseHandle(hFile);
        return TRUE;
    }
    CloseHandle(hFile);
    raw[bytesRead] = '\0';

    // Skip UTF-8 BOM if present.
    char *p = raw;
    DWORD dataLen = bytesRead;
    if (dataLen >= 3 && (BYTE)p[0] == 0xEF && (BYTE)p[1] == 0xBB && (BYTE)p[2] == 0xBF) {
        p += 3;
        dataLen -= 3;
    }

    // Convert UTF-8 to UTF-16.
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, p, (int)dataLen, NULL, 0);
    if (wideLen <= 0) { HeapFree(GetProcessHeap(), 0, raw); return TRUE; }

    wchar_t *wide = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                                          ((SIZE_T)wideLen + 1) * sizeof(wchar_t));
    if (!wide) { HeapFree(GetProcessHeap(), 0, raw); return TRUE; }
    MultiByteToWideChar(CP_UTF8, 0, p, (int)dataLen, wide, wideLen);
    wide[wideLen] = L'\0';
    HeapFree(GetProcessHeap(), 0, raw);

    wchar_t *cur = wide;
    while (*cur) {
        wchar_t *eol = cur;
        while (*eol && *eol != L'\n') eol++;

        wchar_t *end = eol;
        while (end > cur && (end[-1] == L'\r' || end[-1] == L'\n' || end[-1] == L' '))
            end--;
        *end = L'\0';

        size_t len = (size_t)(end - cur);
        if (len > 0 && cur[0] != L';' && cur[0] != L'#') {
            wchar_t *eq = wcschr(cur, L'=');
            if (eq && eq != cur) {
                wchar_t key = norm_letter(cur[0]);
                if (key)
                    split_entry(eq + 1, t[key - L'A'].guid, t[key - L'A'].pnpId);
            }
        }

        cur = (*eol) ? eol + 1 : eol;
    }

    HeapFree(GetProcessHeap(), 0, wide);
    return TRUE;
}

BOOL map_write_file(const MapTable t) {
    wchar_t path[MAX_PATH];
    map_file_path(path, MAX_PATH);

    BOOL hasAny = FALSE;
    for (int i = 0; i < 26; i++) {
        if (t[i].guid[0] != L'\0') { hasAny = TRUE; break; }
    }
    if (!hasAny) {
        DeleteFileW(path);
        return TRUE;
    }

    wchar_t content[8192];
    int pos = 0;
    for (int i = 0; i < 26; i++) {
        if (t[i].guid[0] == L'\0') continue;
        int n;
        if (t[i].pnpId[0] != L'\0')
            n = _snwprintf_s(content + pos, 8192 - pos, _TRUNCATE,
                             L"%lc=%ls|%ls\n", (wchar_t)(L'A' + i), t[i].guid, t[i].pnpId);
        else
            n = _snwprintf_s(content + pos, 8192 - pos, _TRUNCATE,
                             L"%lc=%ls\n", (wchar_t)(L'A' + i), t[i].guid);
        if (n > 0) pos += n;
    }

    // Convert to UTF-8 with BOM.
    char utf8[16384];
    utf8[0] = (char)0xEF;
    utf8[1] = (char)0xBB;
    utf8[2] = (char)0xBF;
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, content, pos,
                                       utf8 + 3, (int)sizeof(utf8) - 3, NULL, NULL);
    if (utf8Len <= 0) return FALSE;

    HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        eprint(L"map_write_file: CreateFileW", GetLastError());
        return FALSE;
    }

    DWORD written;
    WriteFile(hFile, utf8, (DWORD)(utf8Len + 3), &written, NULL);
    CloseHandle(hFile);
    return TRUE;
}

#else

// ---------------------------------------------------------------------------
// File I/O — CRT variant (stdio)
// ---------------------------------------------------------------------------

BOOL map_read_file(MapTable t) {
    memset(t, 0, sizeof(MapTable));

    wchar_t path[MAX_PATH];
    map_file_path(path, MAX_PATH);

    FILE *f = NULL;
    if (_wfopen_s(&f, path, L"r, ccs=UTF-8") != 0 || !f) {
        // File not found is not an error (empty mapping).
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            eprint(L"map_read_file: fopen", GetLastError());
        return TRUE;
    }

    wchar_t line[MAX_PATH * 2];
    while (fgetws(line, ARRAYSIZE(line), f)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len - 1] == L'\r' || line[len - 1] == L'\n' || line[len - 1] == L' '))
            line[--len] = L'\0';

        if (len == 0 || line[0] == L';' || line[0] == L'#') continue; // comment / blank

        wchar_t *eq = wcschr(line, L'=');
        if (!eq || eq == line) continue;

        wchar_t key = norm_letter(line[0]);
        if (!key) continue;

        split_entry(eq + 1, t[key - L'A'].guid, t[key - L'A'].pnpId);
    }

    fclose(f);
    return TRUE;
}

BOOL map_write_file(const MapTable t) {
    wchar_t path[MAX_PATH];
    map_file_path(path, MAX_PATH);

    // If all entries are empty, delete the file rather than leaving it empty.
    BOOL hasAny = FALSE;
    for (int i = 0; i < 26; i++) {
        if (t[i].guid[0] != L'\0') { hasAny = TRUE; break; }
    }
    if (!hasAny) {
        DeleteFileW(path);
        return TRUE;
    }

    FILE *f = NULL;
    if (_wfopen_s(&f, path, L"w, ccs=UTF-8") != 0 || !f) {
        eprint(L"map_write_file: fopen", GetLastError());
        return FALSE;
    }

    for (int i = 0; i < 26; i++) {
        if (t[i].guid[0] != L'\0') {
            if (t[i].pnpId[0] != L'\0')
                fwprintf(f, L"%lc=%ls|%ls\n", (wchar_t)(L'A' + i), t[i].guid, t[i].pnpId);
            else
                fwprintf(f, L"%lc=%ls\n", (wchar_t)(L'A' + i), t[i].guid);
        }
    }

    fclose(f);
    return TRUE;
}

#endif // NOCRT

// ---------------------------------------------------------------------------
// Registry I/O
// ---------------------------------------------------------------------------

BOOL map_read_reg(MapTable t) {
    memset(t, 0, sizeof(MapTable));

    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return TRUE; // Key not present is not an error.

    wchar_t name[4];
    wchar_t value[MAX_PATH * 2]; // enough for GUID|PnpID
    DWORD nameLen, valueLen, idx = 0, type;

    while (TRUE) {
        nameLen  = ARRAYSIZE(name);
        valueLen = sizeof(value);
        LSTATUS st = RegEnumValueW(key, idx++, name, &nameLen, NULL, &type,
                                   (BYTE *)value, &valueLen);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS || type != REG_SZ) continue;

        wchar_t letter = norm_letter(name[0]);
        if (letter)
            split_entry(value, t[letter - L'A'].guid, t[letter - L'A'].pnpId);
    }

    RegCloseKey(key);
    return TRUE;
}

BOOL map_write_reg(const MapTable t) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_READ,
                        NULL, &key, NULL) != ERROR_SUCCESS) {
        eprint(L"map_write_reg: RegCreateKeyExW", GetLastError());
        return FALSE;
    }

    // Delete existing values first so that removed entries are actually removed.
    wchar_t name[4];
    DWORD nameLen;
    while (TRUE) {
        nameLen = ARRAYSIZE(name);
        if (RegEnumValueW(key, 0, name, &nameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        RegDeleteValueW(key, name);
    }

    for (int i = 0; i < 26; i++) {
        if (t[i].guid[0] != L'\0') {
            wchar_t letter[2] = { (wchar_t)(L'A' + i), L'\0' };
            wchar_t combined[MAX_PATH * 2];
            if (t[i].pnpId[0] != L'\0')
                _snwprintf_s(combined, ARRAYSIZE(combined), _TRUNCATE,
                             L"%ls|%ls", t[i].guid, t[i].pnpId);
            else
                wcscpy_s(combined, ARRAYSIZE(combined), t[i].guid);
            RegSetValueExW(key, letter, 0, REG_SZ,
                           (const BYTE *)combined,
                           (DWORD)((wcslen(combined) + 1) * sizeof(wchar_t)));
        }
    }

    RegCloseKey(key);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Lookup and display
// ---------------------------------------------------------------------------

const MapEntry *map_lookup(wchar_t letter, const MapTable fileT, const MapTable regT) {
    int idx = letter - L'A';
    if (idx < 0 || idx > 25) return NULL;
    if (fileT[idx].guid[0] != L'\0') return &fileT[idx];
    if (regT[idx].guid[0]  != L'\0') return &regT[idx];
    return NULL;
}

const MapEntry *map_lookup_by_pnp(const wchar_t *pnpId, const MapTable fileT,
                                   const MapTable regT, wchar_t *letterOut) {
    for (int i = 0; i < 26; i++) {
        if (fileT[i].pnpId[0] && _wcsicmp(fileT[i].pnpId, pnpId) == 0) {
            if (letterOut) *letterOut = (wchar_t)(L'A' + i);
            return &fileT[i];
        }
    }
    for (int i = 0; i < 26; i++) {
        if (regT[i].pnpId[0] && _wcsicmp(regT[i].pnpId, pnpId) == 0) {
            if (letterOut) *letterOut = (wchar_t)(L'A' + i);
            return &regT[i];
        }
    }
    return NULL;
}

void map_show(const MapTable fileT, const MapTable regT) {
    wchar_t filePath[MAX_PATH];
    map_file_path(filePath, MAX_PATH);
    PrintMsg(L"File (%ls):\n", filePath);
    BOOL anyFile = FALSE;
    for (int i = 0; i < 26; i++) {
        if (fileT[i].guid[0] != L'\0') {
            PrintMsg(L"  %lc = %ls", (wchar_t)(L'A' + i), fileT[i].guid);
            if (fileT[i].pnpId[0]) PrintMsg(L"  [%ls]", fileT[i].pnpId);
            PrintMsg(L"\n");
            anyFile = TRUE;
        }
    }
    if (!anyFile) PrintMsg(L"  (empty)\n");

    PrintMsg(L"Registry (HKCU\\%ls):\n", REG_KEY);
    BOOL anyReg = FALSE;
    for (int i = 0; i < 26; i++) {
        if (regT[i].guid[0] != L'\0') {
            PrintMsg(L"  %lc = %ls", (wchar_t)(L'A' + i), regT[i].guid);
            if (regT[i].pnpId[0]) PrintMsg(L"  [%ls]", regT[i].pnpId);
            PrintMsg(L"\n");
            anyReg = TRUE;
        }
    }
    if (!anyReg) PrintMsg(L"  (empty)\n");
}
