#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ATA_PASS_THROUGH_EX structure (matches Windows kernel ABI on x64).
// Defined here to avoid WDK dependency; layout verified against ntddscsi.h.
typedef struct {
    USHORT    Length;
    USHORT    AtaFlags;
    UCHAR     PathId;
    UCHAR     TargetId;
    UCHAR     Lun;
    UCHAR     ReservedAsUchar;
    ULONG     DataTransferLength;
    ULONG     TimeOutValue;
    ULONG     ReservedAsUlong;
    ULONG_PTR DataBufferOffset;   // offset from start of struct to data buffer
    UCHAR     PreviousTaskFile[8];
    UCHAR     CurrentTaskFile[8]; // [0]=Features, [1]=Count, [6]=Command
} ATA_PASS_THROUGH_EX;

// AtaFlags values
#define ATA_FLAGS_DRDY_REQUIRED 0x0001
#define ATA_FLAGS_DATA_IN       0x0002
#define ATA_FLAGS_DATA_OUT      0x0004

// IOCTL_ATA_PASS_THROUGH = CTL_CODE(4, 0x040B, 0, 3) = 0x0004D02C
#define IOCTL_ATA_PASS_THROUGH 0x0004D02C

// IOCTL_STORAGE_GET_DEVICE_NUMBER = CTL_CODE(0x2D, 0x0420, 0, 0) = 0x002D1080
#define IOCTL_STORAGE_GET_DEVICE_NUMBER 0x002D1080

typedef struct {
    DWORD DeviceType;
    DWORD DeviceNumber;
    DWORD PartitionNumber;
} STORAGE_DEVICE_NUMBER;

// ATA IDENTIFY DEVICE result (256 x USHORT words).
typedef struct {
    USHORT w[256];
} IdentifyData;

// Open \\.\PhysicalDriveN for read/write. Returns INVALID_HANDLE_VALUE on failure.
HANDLE disk_open(DWORD diskNum);

// Get physical disk number (DeviceNumber) from a volume IOCTL path (e.g. \\.\D:).
// Returns (DWORD)-1 on failure.
DWORD disk_number_from_vol(const wchar_t *volPath);

// Send ATA STANDBY IMMEDIATE (0xE0) to the physical disk.
BOOL disk_standby(DWORD diskNum);

// Force disk spin-up by issuing ATA READ VERIFY SECTOR(S) (0x40), count=1, LBA 0.
BOOL disk_spinup(DWORD diskNum);

// Send ATA CHECK POWER MODE (0xE5).
// Returns: 1 = standby, 0 = active/idle, -1 = error.
int disk_power_mode(DWORD diskNum);

// Send ATA IDENTIFY DEVICE (0xEC) and store the 256-word result in *data.
BOOL disk_identify(DWORD diskNum, IdentifyData *data);

// Return TRUE if APM is supported (IDENTIFY word[83] bit 3).
BOOL disk_apm_supported(const IdentifyData *data);

// Return TRUE if APM is currently enabled (IDENTIFY word[86] bit 3).
BOOL disk_apm_enabled(const IdentifyData *data);

// Return the current APM level from IDENTIFY word[91].
// 0x00 = disabled, 0x01-0xFE = level, 0xFF = max performance.
USHORT disk_apm_level(const IdentifyData *data);

// Enable APM via SET FEATURES (0xEF / sub 0x05) at the given level (1-254).
BOOL disk_apm_enable(DWORD diskNum, BYTE level);

// Disable APM via SET FEATURES (0xEF / sub 0x85).
BOOL disk_apm_disable(DWORD diskNum);

// ---------------------------------------------------------------------------
// Storage property queries (model, size, disk type detection)
// ---------------------------------------------------------------------------

// IOCTL_STORAGE_QUERY_PROPERTY = CTL_CODE(0x2D, 0x0500, 0, 0)
#define IOCTL_STORAGE_QUERY_PROPERTY 0x002D1400

// IOCTL_DISK_GET_LENGTH_INFO = CTL_CODE(0x07, 0x0017, 0, 1)
#define IOCTL_DISK_GET_LENGTH_INFO 0x0007405C

// Detect disk type: 0 = HDD, 1 = SSD, -1 = unknown/virtual.
int disk_type(DWORD diskNum);

// Get disk vendor+model string (trimmed). Returns FALSE if unavailable.
BOOL disk_get_model(DWORD diskNum, wchar_t *buf, DWORD bufLen);

// Get disk size in bytes. Returns FALSE if unavailable.
BOOL disk_get_size(DWORD diskNum, ULONGLONG *sizeBytes);
