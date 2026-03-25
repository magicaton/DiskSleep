#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// IOCTL codes used in this module — all defined in <winioctl.h> which volume.c includes.
// For reference (Windows SDK 10.0.26100.0):
//   FSCTL_LOCK_VOLUME     = 0x00090018  CTL_CODE(FILE_DEVICE_FILE_SYSTEM=0x9, 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
//   FSCTL_UNLOCK_VOLUME   = 0x0009001C  CTL_CODE(FILE_DEVICE_FILE_SYSTEM=0x9, 7, METHOD_BUFFERED, FILE_ANY_ACCESS)
//   FSCTL_DISMOUNT_VOLUME = 0x00090020  CTL_CODE(FILE_DEVICE_FILE_SYSTEM=0x9, 8, METHOD_BUFFERED, FILE_ANY_ACCESS)
//   IOCTL_VOLUME_ONLINE   = 0x0056C008  CTL_CODE(IOCTL_VOLUME_BASE=0x56, 2, METHOD_BUFFERED, FILE_READ_ACCESS|FILE_WRITE_ACCESS)
//   IOCTL_VOLUME_OFFLINE  = 0x0056C00C  CTL_CODE(IOCTL_VOLUME_BASE=0x56, 3, METHOD_BUFFERED, FILE_READ_ACCESS|FILE_WRITE_ACCESS)

// Open a volume device for IOCTL operations.
// path: \\.\D:  or  \\.\Volume{guid}  (no trailing backslash).
// Returns INVALID_HANDLE_VALUE on failure (error already printed).
HANDLE vol_open(const wchar_t *path);

// Lock the volume (FSCTL_LOCK_VOLUME). Handle must remain open to hold the lock.
BOOL vol_lock(HANDLE h);
// Try to lock with retries (1 s interval). Prints error only on final failure.
BOOL vol_lock_wait(HANDLE h, DWORD timeoutMs);

// Unlock the volume (FSCTL_UNLOCK_VOLUME).
BOOL vol_unlock(HANDLE h);

// Dismount the filesystem (FSCTL_DISMOUNT_VOLUME). Flushes and invalidates cached data.
BOOL vol_dismount(HANDLE h);

// Take the volume offline (IOCTL_VOLUME_OFFLINE). Persistent across handle close.
BOOL vol_offline(HANDLE h);

// Bring the volume online (IOCTL_VOLUME_ONLINE).
BOOL vol_online(HANDLE h);

// Query the volume GUID path for a drive letter.
// letter: uppercase drive letter (e.g., L'D').
// out: receives \\?\Volume{guid}\ (with trailing backslash), must be MAX_PATH wchar_t.
// Returns FALSE if the letter is not a mounted volume.
BOOL vol_get_guid(wchar_t letter, wchar_t *out);

// Convert a GUID path \\?\Volume{...}\ to an IOCTL-compatible path \\.\Volume{...}.
// in and out may be the same buffer.
void vol_guid_to_ioctl(const wchar_t *in, wchar_t *out, size_t outLen);

// Build the volume device path for a drive letter: \\.\D:
void vol_letter_path(wchar_t letter, wchar_t *out, size_t outLen);

// Return TRUE if the volume device can be opened (rough "is accessible" check).
// Does NOT confirm the filesystem is online; just that the device exists.
BOOL vol_is_accessible(const wchar_t *path);

// Find the volume GUID path for a physical disk number by enumerating all volumes.
// Returns the first volume found on that disk (\\?\Volume{...}\ format).
// Returns FALSE if no volume found.
BOOL vol_find_by_disk(DWORD diskNum, wchar_t *guidOut, size_t outLen);
