#include "pnp.h"
#include "util.h"
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>

// GUID_DEVINTERFACE_DISK = {53F56307-B6BF-11D0-94F2-00A0C91EFB8B}
static const GUID s_DiskInterfaceGuid =
    { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };


DWORD pnp_disk_number(const wchar_t *instanceId) {
    HDEVINFO devs = SetupDiGetClassDevsW(&s_DiskInterfaceGuid, NULL, NULL,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return (DWORD)-1;

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);
    DWORD result = (DWORD)-1;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs, NULL, &s_DiskInterfaceGuid, i, &iface); i++) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &iface, NULL, 0, &required, NULL);

        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)HeapAlloc(GetProcessHeap(), 0, required);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devInfo;
        devInfo.cbSize = sizeof(devInfo);

        if (!SetupDiGetDeviceInterfaceDetailW(devs, &iface, detail, required, NULL, &devInfo)) {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }

        wchar_t id[MAX_PATH];
        if (!SetupDiGetDeviceInstanceIdW(devs, &devInfo, id, MAX_PATH, NULL)) {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }

        if (_wcsicmp(id, instanceId) != 0) {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }

        HANDLE h = CreateFileW(detail->DevicePath, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        HeapFree(GetProcessHeap(), 0, detail);

        if (h != INVALID_HANDLE_VALUE) {
            STORAGE_DEVICE_NUMBER sdn = {0};
            DWORD bytes = 0;
            if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                NULL, 0, &sdn, sizeof(sdn), &bytes, NULL))
                result = sdn.DeviceNumber;
            CloseHandle(h);
        }
        break;
    }

    SetupDiDestroyDeviceInfoList(devs);
    return result;
}

BOOL pnp_get_instance_id(DWORD diskNum, wchar_t *buf, DWORD bufLen) {
    HDEVINFO devs = SetupDiGetClassDevsW(&s_DiskInterfaceGuid, NULL, NULL,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) {
        eprint(L"SetupDiGetClassDevsW", GetLastError());
        return FALSE;
    }

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);
    BOOL found = FALSE;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devs, NULL, &s_DiskInterfaceGuid, i, &iface); i++) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &iface, NULL, 0, &required, NULL);

        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)HeapAlloc(GetProcessHeap(), 0, required);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devInfo;
        devInfo.cbSize = sizeof(devInfo);

        if (!SetupDiGetDeviceInterfaceDetailW(devs, &iface, detail, required, NULL, &devInfo)) {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }

        HANDLE h = CreateFileW(detail->DevicePath, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        HeapFree(GetProcessHeap(), 0, detail);

        if (h == INVALID_HANDLE_VALUE) continue;

        STORAGE_DEVICE_NUMBER sdn = {0};
        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                  NULL, 0, &sdn, sizeof(sdn), &bytes, NULL);
        CloseHandle(h);

        if (ok && sdn.DeviceNumber == diskNum) {
            SetupDiGetDeviceInstanceIdW(devs, &devInfo, buf, bufLen, NULL);
            found = TRUE;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

static BOOL locate_devnode(const wchar_t *instanceId, DEVINST *devNode, BOOL phantom) {
    ULONG flags = phantom ? CM_LOCATE_DEVNODE_PHANTOM : CM_LOCATE_DEVNODE_NORMAL;
    CONFIGRET cr = CM_Locate_DevNodeW(devNode, (DEVINSTID_W)instanceId, flags);
    if (cr != CR_SUCCESS) {
        PrintMsg(L"Error: CM_Locate_DevNodeW failed (CR=0x%X) for %ls\n", cr, instanceId);
        return FALSE;
    }
    return TRUE;
}

BOOL pnp_enable(const wchar_t *instanceId) {
    DEVINST devNode;
    if (!locate_devnode(instanceId, &devNode, TRUE)) return FALSE;
    CONFIGRET cr = CM_Enable_DevNode(devNode, 0);
    if (cr != CR_SUCCESS)
        PrintMsg(L"Error: CM_Enable_DevNode failed (CR=0x%X)\n", cr);
    return cr == CR_SUCCESS;
}

BOOL pnp_disable(const wchar_t *instanceId, BOOL persist) {
    DEVINST devNode;
    if (!locate_devnode(instanceId, &devNode, FALSE)) return FALSE;
    CONFIGRET cr = CM_Disable_DevNode(devNode, persist ? CM_DISABLE_PERSIST : 0);
    if (cr != CR_SUCCESS)
        PrintMsg(L"Error: CM_Disable_DevNode failed (CR=0x%X)\n", cr);
    return cr == CR_SUCCESS;
}

BOOL pnp_is_disabled(const wchar_t *instanceId) {
    DEVINST devNode;
    if (!locate_devnode(instanceId, &devNode, TRUE)) return FALSE;
    ULONG status = 0, problem = 0;
    CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, devNode, 0);
    if (cr != CR_SUCCESS) return FALSE;
    return (status & DN_HAS_PROBLEM) && (problem == CM_PROB_DISABLED);
}

BOOL pnp_wait_ready(DWORD diskNum, DWORD timeoutMs) {
    wchar_t path[64];
    _snwprintf_s(path, 64, _TRUNCATE, L"\\\\.\\PhysicalDrive%u", diskNum);
    DWORD elapsed = 0;
    while (elapsed < timeoutMs) {
        HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return TRUE;
        }
        Sleep(500);
        elapsed += 500;
    }
    PrintMsg(L"Error: timeout waiting for PhysicalDrive%u to become ready\n", diskNum);
    return FALSE;
}

int pnp_enumerate_disks(PnpDiskInfo *out, int maxCount) {
    // Omit DIGCF_PRESENT to include disabled/not-present devices.
    HDEVINFO devs = SetupDiGetClassDevsW(&s_DiskInterfaceGuid, NULL, NULL,
                                          DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return 0;

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);
    int count = 0;

    for (DWORD i = 0; count < maxCount &&
         SetupDiEnumDeviceInterfaces(devs, NULL, &s_DiskInterfaceGuid, i, &iface); i++) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &iface, NULL, 0, &required, NULL);

        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)HeapAlloc(GetProcessHeap(), 0, required);
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devInfo;
        devInfo.cbSize = sizeof(devInfo);

        if (!SetupDiGetDeviceInterfaceDetailW(devs, &iface, detail, required, NULL, &devInfo)) {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }
        HeapFree(GetProcessHeap(), 0, detail);

        PnpDiskInfo *entry = &out[count];
        memset(entry, 0, sizeof(*entry));

        if (!SetupDiGetDeviceInstanceIdW(devs, &devInfo, entry->instanceId, MAX_PATH, NULL))
            continue;

        // Get friendly name, fall back to device description.
        if (!SetupDiGetDeviceRegistryPropertyW(devs, &devInfo, SPDRP_FRIENDLYNAME,
                                                NULL, (BYTE *)entry->description,
                                                sizeof(entry->description), NULL)) {
            SetupDiGetDeviceRegistryPropertyW(devs, &devInfo, SPDRP_DEVICEDESC,
                                               NULL, (BYTE *)entry->description,
                                               sizeof(entry->description), NULL);
        }

        ULONG status = 0, problem = 0;
        DEVINST dn;
        if (CM_Locate_DevNodeW(&dn, entry->instanceId, CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS) {
            if (CM_Get_DevNode_Status(&status, &problem, dn, 0) == CR_SUCCESS) {
                entry->disabled = (status & DN_HAS_PROBLEM) && (problem == CM_PROB_DISABLED);
                entry->present = (status & DN_DRIVER_LOADED) || !(status & DN_HAS_PROBLEM);
            }
        }

        count++;
    }

    SetupDiDestroyDeviceInfoList(devs);
    return count;
}

BOOL pnp_recover(const wchar_t *pnpIdHint,
                 wchar_t *instanceIdOut, DWORD bufLen) {
    if (!pnpIdHint || !pnpIdHint[0]) {
        PrintMsg(L"Error: no PnP instance ID available for recovery.\n"
                 L"  Save a mapping with PnP ID while the drive is accessible:\n"
                 L"  disksleep map set <letter> --file\n");
        return FALSE;
    }

    if (!pnp_is_disabled(pnpIdHint)) {
        PrintMsg(L"Error: device %ls is not disabled; cannot recover via PnP\n", pnpIdHint);
        return FALSE;
    }

    PrintMsg(L"PnP recovery: enabling stored device: %ls\n", pnpIdHint);
    wcscpy_s(instanceIdOut, bufLen, pnpIdHint);
    return pnp_enable(pnpIdHint);
}
