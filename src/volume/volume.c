#include "volume.h"
#include "util.h"
#include <winioctl.h>
#include <stdio.h>

HANDLE vol_open(const wchar_t *path) {
    HANDLE h = CreateFileW(path,
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        eprint(path, GetLastError());
    return h;
}

static BOOL vol_ioctl(HANDLE h, DWORD code, const wchar_t *name) {
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, code, NULL, 0, NULL, 0, &bytes, NULL);
    if (!ok) eprint(name, GetLastError());
    return ok;
}

BOOL vol_lock(HANDLE h)     { return vol_ioctl(h, FSCTL_LOCK_VOLUME,    L"FSCTL_LOCK_VOLUME"); }

BOOL vol_lock_wait(HANDLE h, DWORD timeoutMs) {
    DWORD bytes;
    for (DWORD ms = 0; ; ms += 1000) {
        if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytes, NULL))
            return TRUE;
        if (ms >= timeoutMs) break;
        Sleep(1000);
    }
    eprint(L"FSCTL_LOCK_VOLUME", GetLastError());
    return FALSE;
}
BOOL vol_unlock(HANDLE h)   { return vol_ioctl(h, FSCTL_UNLOCK_VOLUME,  L"FSCTL_UNLOCK_VOLUME"); }
BOOL vol_dismount(HANDLE h) { return vol_ioctl(h, FSCTL_DISMOUNT_VOLUME, L"FSCTL_DISMOUNT_VOLUME"); }
BOOL vol_offline(HANDLE h)  { return vol_ioctl(h, IOCTL_VOLUME_OFFLINE,  L"IOCTL_VOLUME_OFFLINE"); }
BOOL vol_online(HANDLE h)   { return vol_ioctl(h, IOCTL_VOLUME_ONLINE,   L"IOCTL_VOLUME_ONLINE"); }

BOOL vol_get_guid(wchar_t letter, wchar_t *out) {
    wchar_t mount[8];
    _snwprintf_s(mount, 8, _TRUNCATE, L"%lc:\\", letter);
    return GetVolumeNameForVolumeMountPointW(mount, out, MAX_PATH);
}

void vol_guid_to_ioctl(const wchar_t *in, wchar_t *out, size_t outLen) {
    // \\?\Volume{...}\ -> \\.\Volume{...}
    wchar_t tmp[MAX_PATH];
    wcscpy_s(tmp, MAX_PATH, in);

    size_t len = wcslen(tmp);
    if (len > 0 && tmp[len - 1] == L'\\')
        tmp[--len] = L'\0';

    // Rewrite \\?\ device namespace to \\.\ device namespace.
    if (wcsncmp(tmp, L"\\\\?\\", 4) == 0)
        tmp[2] = L'.';

    wcscpy_s(out, outLen, tmp);
}

void vol_letter_path(wchar_t letter, wchar_t *out, size_t outLen) {
    _snwprintf_s(out, outLen, _TRUNCATE, L"\\\\.\\%lc:", letter);
}

BOOL vol_is_accessible(const wchar_t *path) {
    if (!path || !path[0]) return FALSE;
    HANDLE h = CreateFileW(path,
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(h);
    return TRUE;
}

BOOL vol_find_by_disk(DWORD diskNum, wchar_t *guidOut, size_t outLen) {
    wchar_t vol[MAX_PATH];
    HANDLE hFind = FindFirstVolumeW(vol, MAX_PATH);
    if (hFind == INVALID_HANDLE_VALUE) return FALSE;

    BOOL found = FALSE;
    do {
        wchar_t ioctl[MAX_PATH];
        vol_guid_to_ioctl(vol, ioctl, MAX_PATH);

        HANDLE h = CreateFileW(ioctl, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        STORAGE_DEVICE_NUMBER sdn = {0};
        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                  NULL, 0, &sdn, sizeof(sdn), &bytes, NULL);
        CloseHandle(h);

        if (ok && sdn.DeviceNumber == diskNum) {
            wcscpy_s(guidOut, outLen, vol);
            found = TRUE;
            break;
        }
    } while (FindNextVolumeW(hFind, vol, MAX_PATH));

    FindClose(hFind);
    return found;
}
