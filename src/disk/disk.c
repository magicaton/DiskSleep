#include "disk.h"
#include "util.h"
#include <stdio.h>

// Combined buffer for IDENTIFY DEVICE (APT header + 512-byte data).
typedef struct {
    ATA_PASS_THROUGH_EX apt;
    USHORT data[256];
} IdentifyBuf;

// Execute an ATA pass-through command using an already-open physical drive handle.
// Returns TRUE on success.
static BOOL ata_ioctl(HANDLE h, ATA_PASS_THROUGH_EX *apt, DWORD size) {
    DWORD bytes = 0;
    return DeviceIoControl(h, IOCTL_ATA_PASS_THROUGH,
                           apt, size, apt, size, &bytes, NULL);
}

HANDLE disk_open(DWORD diskNum) {
    wchar_t path[64];
    _snwprintf_s(path, 64, _TRUNCATE, L"\\\\.\\PhysicalDrive%u", diskNum);
    HANDLE h = CreateFileW(path,
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        eprint(path, GetLastError());
    return h;
}

DWORD disk_number_from_vol(const wchar_t *volPath) {
    HANDLE h = CreateFileW(volPath,
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        eprint(volPath, GetLastError());
        return (DWORD)-1;
    }
    STORAGE_DEVICE_NUMBER sdn = {0};
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                              NULL, 0, &sdn, sizeof(sdn), &bytes, NULL);
    CloseHandle(h);
    if (!ok) {
        eprint(L"IOCTL_STORAGE_GET_DEVICE_NUMBER", GetLastError());
        return (DWORD)-1;
    }
    return sdn.DeviceNumber;
}

BOOL disk_standby(DWORD diskNum) {
    HANDLE h = disk_open(diskNum);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    ATA_PASS_THROUGH_EX apt = {0};
    apt.Length         = sizeof(apt);
    apt.AtaFlags       = ATA_FLAGS_DRDY_REQUIRED;
    apt.TimeOutValue   = 20;
    apt.CurrentTaskFile[6] = 0xE0; // STANDBY IMMEDIATE

    BOOL ok = ata_ioctl(h, &apt, sizeof(apt));
    CloseHandle(h);
    if (!ok) eprint(L"STANDBY IMMEDIATE", GetLastError());
    return ok;
}

BOOL disk_spinup(DWORD diskNum) {
    HANDLE h = disk_open(diskNum);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    ATA_PASS_THROUGH_EX apt = {0};
    apt.Length         = sizeof(apt);
    apt.AtaFlags       = ATA_FLAGS_DRDY_REQUIRED;
    apt.TimeOutValue   = 30;
    apt.CurrentTaskFile[1] = 1;    // Sector Count = 1
    apt.CurrentTaskFile[6] = 0x40; // READ VERIFY SECTOR(S)

    BOOL ok = ata_ioctl(h, &apt, sizeof(apt));
    CloseHandle(h);
    if (!ok) eprint(L"READ VERIFY SECTOR(S)", GetLastError());
    return ok;
}

int disk_power_mode(DWORD diskNum) {
    HANDLE h = disk_open(diskNum);
    if (h == INVALID_HANDLE_VALUE) return -1;

    ATA_PASS_THROUGH_EX apt = {0};
    apt.Length         = sizeof(apt);
    apt.AtaFlags       = 0; // CHECK POWER MODE executes in all power states; DRDY not required.
    apt.TimeOutValue   = 10;
    apt.CurrentTaskFile[6] = 0xE5; // CHECK POWER MODE

    BOOL ok = ata_ioctl(h, &apt, sizeof(apt));
    CloseHandle(h);
    if (!ok) {
        eprint(L"CHECK POWER MODE", GetLastError());
        return -1;
    }
    // Sector Count register (index 1) holds the result:
    // 0x00 = Standby, 0x40/0x41 = NV Cache, 0x80 = Idle, 0xFF = Active
    BYTE mode = apt.CurrentTaskFile[1];
    return (mode == 0x00) ? 1 : 0;
}

BOOL disk_identify(DWORD diskNum, IdentifyData *data) {
    HANDLE h = disk_open(diskNum);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    IdentifyBuf buf = {0};
    buf.apt.Length              = sizeof(ATA_PASS_THROUGH_EX);
    buf.apt.AtaFlags            = ATA_FLAGS_DATA_IN;
    buf.apt.DataTransferLength  = sizeof(buf.data);
    buf.apt.TimeOutValue        = 10;
    buf.apt.DataBufferOffset    = sizeof(ATA_PASS_THROUGH_EX);
    buf.apt.CurrentTaskFile[6]  = 0xEC; // IDENTIFY DEVICE

    BOOL ok = ata_ioctl(h, &buf.apt, sizeof(buf));
    CloseHandle(h);
    if (!ok) return FALSE;
    memcpy(data->w, buf.data, sizeof(buf.data));
    return TRUE;
}

BOOL disk_apm_supported(const IdentifyData *data) {
    return (data->w[83] & (1 << 3)) != 0;
}

BOOL disk_apm_enabled(const IdentifyData *data) {
    return (data->w[86] & (1 << 3)) != 0;
}

USHORT disk_apm_level(const IdentifyData *data) {
    return data->w[91];
}

static BOOL ata_set_features(DWORD diskNum, BYTE feature, BYTE count) {
    HANDLE h = disk_open(diskNum);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    ATA_PASS_THROUGH_EX apt = {0};
    apt.Length         = sizeof(apt);
    apt.AtaFlags       = ATA_FLAGS_DRDY_REQUIRED;
    apt.TimeOutValue   = 10;
    apt.CurrentTaskFile[0] = feature; // Features register
    apt.CurrentTaskFile[1] = count;   // Count register
    apt.CurrentTaskFile[6] = 0xEF;    // SET FEATURES

    BOOL ok = ata_ioctl(h, &apt, sizeof(apt));
    CloseHandle(h);
    if (!ok) eprint(L"SET FEATURES", GetLastError());
    return ok;
}

BOOL disk_apm_enable(DWORD diskNum, BYTE level) {
    return ata_set_features(diskNum, 0x05, level); // 0x05 = Enable APM
}

BOOL disk_apm_disable(DWORD diskNum) {
    return ata_set_features(diskNum, 0x85, 0x00); // 0x85 = Disable APM
}

// ---------------------------------------------------------------------------
// Storage property queries
// ---------------------------------------------------------------------------

// Open PhysicalDriveN for read-only queries (no error printed on failure).
static HANDLE disk_open_query(DWORD diskNum) {
    wchar_t path[64];
    _snwprintf_s(path, 64, _TRUNCATE, L"\\\\.\\PhysicalDrive%u", diskNum);
    return CreateFileW(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

// STORAGE_PROPERTY_QUERY layout used for DeviceIoControl calls.
typedef struct {
    DWORD PropertyId;
    DWORD QueryType;
    BYTE  AdditionalParameters[1];
} StoragePropQuery;

int disk_type(DWORD diskNum) {
    HANDLE h = disk_open_query(diskNum);
    if (h == INVALID_HANDLE_VALUE) return -1;

    // Query StorageDeviceProperty (PropertyId 0) for BusType.
    // STORAGE_DEVICE_DESCRIPTOR.BusType is at offset 28 (DWORD).
    // BusTypeVirtual = 0x0E, BusTypeFileBackedVirtual = 0x0F.
    StoragePropQuery q0 = {0};
    BYTE raw[64];
    DWORD bytes = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                        &q0, sizeof(q0), raw, sizeof(raw), &bytes, NULL)
        && bytes >= 32) {
        DWORD busType = *(DWORD *)(raw + 28);
        if (busType == 0x0E || busType == 0x0F) { // virtual
            CloseHandle(h);
            return -1;
        }
    }

    // StorageDeviceSeekPenaltyProperty = 7, PropertyStandardQuery = 0
    StoragePropQuery q7 = {0};
    q7.PropertyId = 7;

    struct { DWORD Version; DWORD Size; BOOLEAN IncursSeekPenalty; } result = {0};
    bytes = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                              &q7, sizeof(q7),
                              &result, sizeof(result), &bytes, NULL);
    CloseHandle(h);

    if (ok && bytes >= sizeof(result))
        return result.IncursSeekPenalty ? 0 : 1;

    // Fallback: ATA IDENTIFY word 217 (Nominal Media Rotation Rate).
    // 0x0001 = non-rotating media (SSD).
    // 0x0000 = not reported (older drives that predate this field are HDDs).
    // 0x0401+ = rotation rate in RPM (HDD).
    // If IDENTIFY succeeds at all, treat anything that isn't SSD as HDD —
    // any ATA device old enough to not report word 217 predates consumer SSDs.
    IdentifyData id;
    if (disk_identify(diskNum, &id)) {
        if (id.w[217] == 0x0001) return 1;  // SSD
        return 0;                           // HDD (reported RPM or not reported)
    }

    return -1;
}

BOOL disk_get_model(DWORD diskNum, wchar_t *buf, DWORD bufLen) {
    HANDLE h = disk_open_query(diskNum);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    // StorageDeviceProperty = 0, PropertyStandardQuery = 0
    StoragePropQuery query = {0};

    BYTE raw[4096];
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                              &query, sizeof(query),
                              raw, sizeof(raw), &bytes, NULL);
    CloseHandle(h);
    if (!ok || bytes < 36) return FALSE;

    // STORAGE_DEVICE_DESCRIPTOR field offsets (verified against Windows SDK):
    //   12: VendorIdOffset (DWORD)
    //   16: ProductIdOffset (DWORD)
    DWORD vendorOff  = *(DWORD *)(raw + 12);
    DWORD productOff = *(DWORD *)(raw + 16);

    char model[256];
    int pos = 0;

    if (vendorOff && vendorOff < bytes) {
        const char *v = (const char *)(raw + vendorOff);
        while (*v && pos < 254) model[pos++] = *v++;
        while (pos > 0 && model[pos - 1] == ' ') pos--;
    }
    if (productOff && productOff < bytes) {
        const char *p = (const char *)(raw + productOff);
        if (pos > 0 && *p) model[pos++] = ' ';
        while (*p && pos < 254) model[pos++] = *p++;
        while (pos > 0 && model[pos - 1] == ' ') pos--;
    }
    model[pos] = '\0';

    if (!pos) return FALSE;
    MultiByteToWideChar(CP_ACP, 0, model, -1, buf, bufLen);
    return TRUE;
}

BOOL disk_get_size(DWORD diskNum, ULONGLONG *sizeBytes) {
    HANDLE h = disk_open_query(diskNum);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER len = {0};
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO,
                              NULL, 0, &len, sizeof(len), &bytes, NULL);
    CloseHandle(h);
    if (!ok) return FALSE;
    *sizeBytes = (ULONGLONG)len.QuadPart;
    return TRUE;
}
