#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Get the PnP device instance ID for a physical disk (by disk number).
// diskNum: physical disk number (from IOCTL_STORAGE_GET_DEVICE_NUMBER).
// buf / bufLen: output buffer for the instance ID string (recommend MAX_PATH).
// Returns FALSE if not found.
BOOL pnp_get_instance_id(DWORD diskNum, wchar_t *buf, DWORD bufLen);

// Get the physical disk number for a PnP instance ID.
// Returns (DWORD)-1 if the device is not present or has no disk number.
DWORD pnp_disk_number(const wchar_t *instanceId);

// Enable a PnP device by its instance ID.
// Works even if the device is currently disabled (phantom locate).
BOOL pnp_enable(const wchar_t *instanceId);

// Disable a PnP device.
// persist=TRUE uses CM_DISABLE_PERSIST (survives reboot); FALSE is session-only.
BOOL pnp_disable(const wchar_t *instanceId, BOOL persist);

// Return TRUE if the device with the given instance ID is currently disabled
// (has CM_PROB_DISABLED problem code).
BOOL pnp_is_disabled(const wchar_t *instanceId);

// Poll until the physical disk is accessible (CreateFileW succeeds) or timeout.
// diskNum: disk number to poll on.
// timeoutMs: maximum wait in milliseconds.
BOOL pnp_wait_ready(DWORD diskNum, DWORD timeoutMs);

// Re-enable a disabled PnP device using a stored instance ID.
// pnpIdHint: PnP instance ID (from mapping). Required — returns FALSE if NULL/empty.
BOOL pnp_recover(const wchar_t *pnpIdHint, wchar_t *instanceIdOut, DWORD bufLen);

// Information about a single disk PnP device (for enumeration).
typedef struct {
    wchar_t instanceId[MAX_PATH];
    wchar_t description[256];  // friendly name or device description
    BOOL    disabled;          // TRUE if device has CM_PROB_DISABLED
    BOOL    present;           // TRUE if device is currently present (DIGCF_PRESENT)
} PnpDiskInfo;

// Enumerate all disk PnP devices (including disabled/not-present).
// Fills the array up to maxCount entries. Returns the number of entries written.
int pnp_enumerate_disks(PnpDiskInfo *out, int maxCount);
