#include "cmd.h"
#include "disk.h"
#include "volume.h"
#include "pnp.h"
#include "mapping.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Parse a drive letter, GUID path, or PnP instance ID from the target string.
// Returns FALSE and prints an error if the string is not a valid target.
static BOOL parse_target_str(const wchar_t *str, wchar_t *letterOut,
                              wchar_t *guidOut, wchar_t *pnpIdOut) {
    *letterOut = 0;
    guidOut[0] = L'\0';
    pnpIdOut[0] = L'\0';

    size_t len = wcslen(str);

    // GUID path: \\?\Volume{...}  or  \\.\Volume{...}
    if (wcsncmp(str, L"\\\\?\\Volume{", 11) == 0 ||
        wcsncmp(str, L"\\\\.\\Volume{", 11) == 0) {
        // Normalize to \\?\Volume{...}\ form (with trailing backslash).
        wcscpy_s(guidOut, MAX_PATH, str);
        if (guidOut[len - 1] != L'\\') {
            guidOut[len]     = L'\\';
            guidOut[len + 1] = L'\0';
        }
        // Ensure \\?\ prefix.
        guidOut[2] = L'?';
        return TRUE;
    }

    // Drive letter: single char, optionally followed by ':'.
    if (len == 1 || (len == 2 && str[1] == L':')) {
        wchar_t l = norm_letter(str[0]);
        if (!l) {
            PrintMsg(L"Error: invalid drive letter '%ls'\n", str);
            return FALSE;
        }
        *letterOut = l;
        return TRUE;
    }

    // PnP instance ID: anything containing a backslash (e.g. SCSI\DISK&...\...).
    if (wcschr(str, L'\\')) {
        wcscpy_s(pnpIdOut, MAX_PATH, str);
        return TRUE;
    }

    PrintMsg(L"Error: '%ls' is not a valid drive letter, volume GUID, or PnP instance ID\n", str);
    return FALSE;
}

// Resolve target: fill guidPath and ioctlPath.
// Attempts live lookup first, then falls back to mapping tables.
// For PnP-ID-only targets, returns TRUE even with partial resolution
// (instanceId set, ioctlPath may be empty if device is disabled).
static BOOL resolve_target(Target *t) {
    if (t->guidPath[0] != L'\0') {
        // Already have GUID (user provided it directly).
        vol_guid_to_ioctl(t->guidPath, t->ioctlPath, MAX_PATH);
        return TRUE;
    }

    if (t->letter) {
        // Try live lookup.
        wchar_t guid[MAX_PATH] = {0};
        if (vol_get_guid(t->letter, guid)) {
            wcscpy_s(t->guidPath, MAX_PATH, guid);
            vol_guid_to_ioctl(guid, t->ioctlPath, MAX_PATH);
            return TRUE;
        }

        // Fall back to mapping tables.
        MapTable fileT = {0}, regT = {0};
        map_read_file(fileT);
        map_read_reg(regT);
        const MapEntry *entry = map_lookup(t->letter, fileT, regT);
        if (entry) {
            wcscpy_s(t->guidPath, MAX_PATH, entry->guid);
            vol_guid_to_ioctl(entry->guid, t->ioctlPath, MAX_PATH);
            if (entry->pnpId[0] && !t->instanceId[0])
                wcscpy_s(t->instanceId, MAX_PATH, entry->pnpId);
            return TRUE;
        }

        PrintMsg(L"Error: cannot resolve volume for drive %lc:.\n"
                 L"  Run 'disksleep map set %lc --file' while the drive is accessible.\n",
                 t->letter, t->letter);
        return FALSE;
    }

    if (t->instanceId[0] != L'\0') {
        // PnP instance ID: try live resolution.
        DWORD dn = pnp_disk_number(t->instanceId);
        if (dn != (DWORD)-1) {
            t->diskNum = dn;
            wchar_t guid[MAX_PATH] = {0};
            if (vol_find_by_disk(dn, guid, MAX_PATH)) {
                wcscpy_s(t->guidPath, MAX_PATH, guid);
                vol_guid_to_ioctl(guid, t->ioctlPath, MAX_PATH);
                return TRUE;
            }
        }

        // Fall back to mapping: search for this PnP ID.
        MapTable fileT = {0}, regT = {0};
        map_read_file(fileT);
        map_read_reg(regT);
        wchar_t letter = 0;
        const MapEntry *entry = map_lookup_by_pnp(t->instanceId, fileT, regT, &letter);
        if (entry) {
            t->letter = letter;
            wcscpy_s(t->guidPath, MAX_PATH, entry->guid);
            vol_guid_to_ioctl(entry->guid, t->ioctlPath, MAX_PATH);
            return TRUE;
        }

        // Partial resolution — instanceId set but no volume path.
        // Callers can still use instanceId for direct PnP operations.
        return TRUE;
    }

    PrintMsg(L"Error: no target specified\n");
    return FALSE;
}

// Resolve the physical disk number for a target; ioctlPath must be set first.
static BOOL resolve_disk_num(Target *t) {
    if (t->diskNum != (DWORD)-1) return TRUE;
    t->diskNum = disk_number_from_vol(t->ioctlPath);
    return t->diskNum != (DWORD)-1;
}

// Parse target + resolve; diskNum not resolved here (done per-command as needed).
static BOOL init_target(const wchar_t *str, Target *t) {
    memset(t, 0, sizeof(*t));
    t->diskNum = (DWORD)-1;
    wchar_t letter = 0;
    wchar_t guid[MAX_PATH] = {0};
    wchar_t pnpId[MAX_PATH] = {0};
    if (!parse_target_str(str, &letter, guid, pnpId)) return FALSE;
    t->letter = letter;
    wcscpy_s(t->guidPath, MAX_PATH, guid);
    wcscpy_s(t->instanceId, MAX_PATH, pnpId);
    return resolve_target(t);
}

static BOOL has_flag(int argc, wchar_t **argv, const wchar_t *flag) {
    for (int i = 0; i < argc; i++)
        if (_wcsicmp(argv[i], flag) == 0) return TRUE;
    return FALSE;
}

// Return TRUE if disk is a confirmed HDD (or unresolved).
// Returns FALSE and prints an error if it is NOT a confirmed HDD.
static BOOL is_hdd(DWORD diskNum) {
    if (diskNum == (DWORD)-1) return TRUE;
    if (disk_type(diskNum) != 0) {
        PrintMsg(L"Error: PhysicalDrive%u is not detected as HDD. This command is only supported for HDD.\n", diskNum);
        return FALSE;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// cmd_sleep
// ---------------------------------------------------------------------------

int cmd_sleep(int argc, wchar_t **argv) {
    if (argc < 2) { print_usage(); return 1; }

    Target t;
    if (!init_target(argv[1], &t)) return 1;

    SleepArgs a = {0};
    a.target       = t;
    a.noDismount   = has_flag(argc - 2, argv + 2, L"--no-dismount");
    a.noOffline    = has_flag(argc - 2, argv + 2, L"--no-offline");
    a.useApm       = has_flag(argc - 2, argv + 2, L"--apm");
    a.noStandby    = has_flag(argc - 2, argv + 2, L"--no-standby");
    a.noPnp        = has_flag(argc - 2, argv + 2, L"--no-pnp");
    a.force        = has_flag(argc - 2, argv + 2, L"--force");
    a.noPnpRecovery= has_flag(argc - 2, argv + 2, L"--no-pnp-recovery");
    a.persist      = has_flag(argc - 2, argv + 2, L"--persist");

    BOOL didRecovery = FALSE;
    if (!resolve_disk_num(&a.target)) {
        if (a.noPnpRecovery) return 1;
        PrintMsg(L"Disk not accessible; attempting PnP recovery...\n");
        wchar_t instanceId[MAX_PATH];
        const wchar_t *pnpHint = a.target.instanceId[0] ? a.target.instanceId : NULL;
        if (!pnp_recover(pnpHint, instanceId, MAX_PATH)) return 1;
        PrintMsg(L"Waiting for volume to become accessible...\n");
        BOOL volReady = FALSE;
        for (DWORD ms = 0; ms < 15000 && !volReady; ms += 500) {
            if (vol_is_accessible(a.target.ioctlPath)) { volReady = TRUE; break; }
            Sleep(500);
        }
        if (!volReady) {
            PrintMsg(L"Error: timeout waiting for volume to become accessible\n");
            return 1;
        }
        if (!resolve_disk_num(&a.target)) return 1;
        didRecovery = TRUE;
    }

    if (!is_hdd(a.target.diskNum)) return 1;

    // State pre-check (skip if --force).
    if (!a.force) {
        BOOL needOffline = !a.noOffline;
        BOOL needSleep   = !a.noStandby;
        BOOL doneOffline = !needOffline;
        BOOL doneSleep   = !needSleep;

        if (needOffline) {
            // Volume is considered offline if it cannot be opened.
            doneOffline = !vol_is_accessible(a.target.ioctlPath);
        }
        if (needSleep) {
            int pm = disk_power_mode(a.target.diskNum);
            if (pm < 0) {
                // Can't determine; don't skip.
            } else if (pm == 1) {
                doneSleep = TRUE;
            } else if (a.useApm) {
                IdentifyData id;
                if (disk_identify(a.target.diskNum, &id) &&
                    disk_apm_supported(&id) &&
                    disk_apm_enabled(&id) &&
                    disk_apm_level(&id) <= 1) {
                    doneSleep = TRUE;
                }
            }
        }

        if (doneOffline && doneSleep) {
            PrintMsg(L"Already in desired state. Use --force to override.\n");
            return 0;
        }
    }

    wchar_t volPath[MAX_PATH];
    if (a.target.letter)
        vol_letter_path(a.target.letter, volPath, MAX_PATH);
    else
        wcscpy_s(volPath, MAX_PATH, a.target.ioctlPath);

    HANDLE hVol = vol_open(volPath);
    if (hVol == INVALID_HANDLE_VALUE) return 1;

    // After PnP recovery the OS auto-mounts the volume; other processes
    // (Explorer, indexer) may briefly hold it open.  Retry for up to 10 s.
    if (didRecovery) {
        PrintMsg(L"Waiting for volume lock...\n");
        if (!vol_lock_wait(hVol, 10000)) {
            CloseHandle(hVol);
            return 1;
        }
    } else {
        if (!vol_lock(hVol)) {
            CloseHandle(hVol);
            return 1;
        }
    }

    if (!a.noDismount)
        vol_dismount(hVol);

    if (!a.noOffline) {
        PrintMsg(L"Taking volume offline\n");
        vol_offline(hVol);
    }

    // Sleep: APM or STANDBY IMMEDIATE (after offline so the volume stays locked
    // through the entire prepare sequence).
    if (!a.noStandby) {
        IdentifyData id;
        BOOL useApm = FALSE;
        if (a.useApm && disk_identify(a.target.diskNum, &id) && disk_apm_supported(&id)) {
            useApm = TRUE;
        }
        if (useApm) {
            PrintMsg(L"Enabling APM (level 1) on PhysicalDrive%u\n", a.target.diskNum);
            disk_apm_enable(a.target.diskNum, 1);
        } else {
            PrintMsg(L"Sending STANDBY IMMEDIATE to PhysicalDrive%u\n", a.target.diskNum);
            disk_standby(a.target.diskNum);
        }
    }

    CloseHandle(hVol);

    if (!a.noPnp) {
        wchar_t instanceId[MAX_PATH] = {0};
        if (pnp_get_instance_id(a.target.diskNum, instanceId, MAX_PATH)) {
            PrintMsg(L"Disabling PnP device: %ls\n", instanceId);
            pnp_disable(instanceId, a.persist);
        } else if (a.target.instanceId[0]) {
            PrintMsg(L"Disabling PnP device (stored): %ls\n", a.target.instanceId);
            pnp_disable(a.target.instanceId, a.persist);
        } else {
            PrintMsg(L"Warning: could not find PnP instance ID; skipping PnP disable\n");
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// cmd_wake
// ---------------------------------------------------------------------------

int cmd_wake(int argc, wchar_t **argv) {
    if (argc < 2) { print_usage(); return 1; }

    WakeArgs a = {0};
    a.useApm     = has_flag(argc - 2, argv + 2, L"--apm");
    a.noPnp      = has_flag(argc - 2, argv + 2, L"--no-pnp");
    a.noOnline   = has_flag(argc - 2, argv + 2, L"--no-online");
    a.noSpinup   = has_flag(argc - 2, argv + 2, L"--no-spinup");
    a.force      = has_flag(argc - 2, argv + 2, L"--force");

    if (!init_target(argv[1], &a.target)) return 1;

    if (!a.noPnp && !vol_is_accessible(a.target.ioctlPath)) {
        wchar_t instanceId[MAX_PATH] = {0};
        const wchar_t *pnpHint = a.target.instanceId[0] ? a.target.instanceId : NULL;

        if (pnpHint && !a.target.ioctlPath[0]) {
            PrintMsg(L"Enabling PnP device: %ls\n", pnpHint);
            if (!pnp_enable(pnpHint)) return 1;
            wcscpy_s(instanceId, MAX_PATH, pnpHint);
        } else {
            PrintMsg(L"Attempting PnP enable (recovery)...\n");
            if (!pnp_recover(pnpHint, instanceId, MAX_PATH)) return 1;
        }

        PrintMsg(L"Waiting for volume to become accessible...\n");
        BOOL ready = FALSE;
        if (a.target.ioctlPath[0]) {
            for (DWORD ms = 0; ms < 15000 && !ready; ms += 500) {
                if (vol_is_accessible(a.target.ioctlPath)) { ready = TRUE; break; }
                Sleep(500);
            }
        } else {
            for (DWORD ms = 0; ms < 15000 && !ready; ms += 500) {
                DWORD dn = pnp_disk_number(instanceId);
                if (dn != (DWORD)-1) {
                    a.target.diskNum = dn;
                    wchar_t guid[MAX_PATH];
                    if (vol_find_by_disk(dn, guid, MAX_PATH)) {
                        wcscpy_s(a.target.guidPath, MAX_PATH, guid);
                        vol_guid_to_ioctl(guid, a.target.ioctlPath, MAX_PATH);
                        ready = TRUE;
                        break;
                    }
                }
                Sleep(500);
            }
        }
        if (!ready) {
            PrintMsg(L"Error: volume still inaccessible after waiting\n");
            return 1;
        }
    }

    if (!resolve_disk_num(&a.target)) return 1;
    if (!is_hdd(a.target.diskNum)) return 1;

    BOOL skipOnline = FALSE;
    if (!a.force && !a.noOnline) {
        if (vol_is_accessible(a.target.ioctlPath)) {
            int pm = disk_power_mode(a.target.diskNum);
            if (pm == 0) // active
                skipOnline = TRUE;
        }
    }

    if (!a.noOnline && !skipOnline) {
        HANDLE hVol = vol_open(a.target.ioctlPath);
        if (hVol == INVALID_HANDLE_VALUE) return 1;
        PrintMsg(L"Bringing volume online\n");
        vol_online(hVol);
        CloseHandle(hVol);
    }

    // Force spin-up via READ VERIFY SECTOR(S).
    if (!a.noSpinup) {
        PrintMsg(L"Spinning up PhysicalDrive%u\n", a.target.diskNum);
        if (!disk_spinup(a.target.diskNum)) return 1;
    }

    if (a.useApm) {
        IdentifyData id;
        if (disk_identify(a.target.diskNum, &id) &&
            disk_apm_supported(&id) && disk_apm_enabled(&id)) {
            PrintMsg(L"Disabling APM on PhysicalDrive%u\n", a.target.diskNum);
            disk_apm_disable(a.target.diskNum);
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// cmd_info
// ---------------------------------------------------------------------------

int cmd_info(int argc, wchar_t **argv) {
    if (argc < 2) { print_usage(); return 1; }

    Target t;
    // Try resolve; don't fail hard — show partial info.
    wchar_t letter = 0;
    wchar_t guid[MAX_PATH] = {0};
    wchar_t pnpId[MAX_PATH] = {0};
    if (!parse_target_str(argv[1], &letter, guid, pnpId)) return 1;
    memset(&t, 0, sizeof(t));
    t.diskNum = (DWORD)-1;
    t.letter  = letter;
    wcscpy_s(t.guidPath, MAX_PATH, guid);
    wcscpy_s(t.instanceId, MAX_PATH, pnpId);

    // Try live GUID.
    BOOL resolved = FALSE;
    if (t.letter && vol_get_guid(t.letter, t.guidPath))
        resolved = TRUE;

    if (!resolved && t.guidPath[0] != L'\0')
        resolved = TRUE;

    // PnP-ID-only: try live resolution.
    if (!resolved && t.instanceId[0]) {
        DWORD dn = pnp_disk_number(t.instanceId);
        if (dn != (DWORD)-1) {
            t.diskNum = dn;
            wchar_t g[MAX_PATH] = {0};
            if (vol_find_by_disk(dn, g, MAX_PATH)) {
                wcscpy_s(t.guidPath, MAX_PATH, g);
                resolved = TRUE;
            }
        }
    }

    if (resolved)
        vol_guid_to_ioctl(t.guidPath, t.ioctlPath, MAX_PATH);

    MapTable ft = {0}, rt = {0};
    map_read_file(ft);
    map_read_reg(rt);
    const MapEntry *fileEntry = (t.letter) ? &ft[t.letter - L'A'] : NULL;
    const MapEntry *regEntry  = (t.letter) ? &rt[t.letter - L'A'] : NULL;

    if (!resolved && t.letter) {
        const MapEntry *m = map_lookup(t.letter, ft, rt);
        if (m) {
            wcscpy_s(t.guidPath, MAX_PATH, m->guid);
            vol_guid_to_ioctl(m->guid, t.ioctlPath, MAX_PATH);
            if (m->pnpId[0] && !t.instanceId[0])
                wcscpy_s(t.instanceId, MAX_PATH, m->pnpId);
            resolved = TRUE;
        }
    }
    if (!resolved && t.instanceId[0]) {
        wchar_t foundLetter = 0;
        const MapEntry *m = map_lookup_by_pnp(t.instanceId, ft, rt, &foundLetter);
        if (m) {
            t.letter = foundLetter;
            wcscpy_s(t.guidPath, MAX_PATH, m->guid);
            vol_guid_to_ioctl(m->guid, t.ioctlPath, MAX_PATH);
            resolved = TRUE;
        }
    }

    BOOL accessible = resolved && vol_is_accessible(t.ioctlPath);

    if (accessible && t.diskNum == (DWORD)-1)
        t.diskNum = disk_number_from_vol(t.ioctlPath);

    PrintMsg(L"--- Disk Info ---\n");
    if (t.letter) PrintMsg(L"Drive letter : %lc:\n", t.letter);
    PrintMsg(L"Volume GUID  : %ls\n", t.guidPath[0] ? t.guidPath : L"(unknown)");
    PrintMsg(L"IOCTL path   : %ls\n", t.ioctlPath[0] ? t.ioctlPath : L"(unknown)");
    PrintMsg(L"Accessible   : %ls\n", accessible ? L"Yes" : L"No");
    if (t.diskNum != (DWORD)-1)
        PrintMsg(L"Physical disk: PhysicalDrive%u\n", t.diskNum);
    else
        PrintMsg(L"Physical disk: (unknown)\n");

    if (t.diskNum != (DWORD)-1) {
        int dt = disk_type(t.diskNum);
        PrintMsg(L"Disk type    : %ls\n", dt == 1 ? L"SSD" : dt == 0 ? L"HDD" : L"(unknown)");

        wchar_t model[128] = {0};
        if (disk_get_model(t.diskNum, model, 128))
            PrintMsg(L"Model        : %ls\n", model);

        ULONGLONG sizeBytes = 0;
        if (disk_get_size(t.diskNum, &sizeBytes)) {
            wchar_t sizeStr[32];
            format_size(sizeBytes, sizeStr, 32);
            PrintMsg(L"Size         : %ls\n", sizeStr);
        }
    }

    if (t.letter) {
        PrintMsg(L"Map (file)   : %ls\n",
                (fileEntry && fileEntry->guid[0]) ? fileEntry->guid : L"(none)");
        if (fileEntry && fileEntry->pnpId[0])
            PrintMsg(L"             : [%ls]\n", fileEntry->pnpId);
        PrintMsg(L"Map (registry): %ls\n",
                (regEntry && regEntry->guid[0]) ? regEntry->guid : L"(none)");
        if (regEntry && regEntry->pnpId[0])
            PrintMsg(L"             : [%ls]\n", regEntry->pnpId);
    }

    if (t.diskNum != (DWORD)-1) {
        wchar_t instanceId[MAX_PATH] = {0};
        if (pnp_get_instance_id(t.diskNum, instanceId, MAX_PATH))
            PrintMsg(L"PnP instance : %ls\n", instanceId);
        else if (t.instanceId[0])
            PrintMsg(L"PnP instance : %ls (stored)\n", t.instanceId);
        else
            PrintMsg(L"PnP instance : (not found)\n");

        int pm = disk_power_mode(t.diskNum);
        if (pm < 0)      PrintMsg(L"Power state  : (unable to query)\n");
        else if (pm == 1) PrintMsg(L"Power state  : Standby\n");
        else              PrintMsg(L"Power state  : Active/Idle\n");

        IdentifyData id;
        if (disk_identify(t.diskNum, &id)) {
            BOOL sup = disk_apm_supported(&id);
            BOOL ena = disk_apm_enabled(&id);
            USHORT lvl = disk_apm_level(&id);
            PrintMsg(L"APM supported: %ls\n", sup ? L"Yes" : L"No");
            if (sup) {
                PrintMsg(L"APM enabled  : %ls\n", ena ? L"Yes" : L"No");
                if (ena)
                    PrintMsg(L"APM level    : %u (0x%02X)\n", lvl, lvl);
            }
        } else {
            PrintMsg(L"APM          : (IDENTIFY DEVICE failed)\n");
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// cmd_list
// ---------------------------------------------------------------------------

int cmd_list(int argc, wchar_t **argv) {
    BOOL showPnp = FALSE;
    for (int a = 1; a < argc; a++) {
        if (_wcsicmp(argv[a], L"--pnp") == 0) showPnp = TRUE;
        else { print_usage(); return 1; }
    }

    // Build disk number -> drive letters mapping.
    // letterMap[diskNum] holds up to 8 drive letters for that physical disk.
    wchar_t letterMap[64][8];
    int     letterCount[64];
    memset(letterMap, 0, sizeof(letterMap));
    memset(letterCount, 0, sizeof(letterCount));

    wchar_t drives[256] = {0};
    GetLogicalDriveStringsW(255, drives);
    for (wchar_t *d = drives; *d; d += wcslen(d) + 1) {
        wchar_t letter = norm_letter(d[0]);
        if (!letter) continue;
        wchar_t volPath[MAX_PATH];
        vol_letter_path(letter, volPath, MAX_PATH);
        HANDLE h = CreateFileW(volPath, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER sdn = {0};
        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                  NULL, 0, &sdn, sizeof(sdn), &bytes, NULL);
        CloseHandle(h);
        if (ok && sdn.DeviceNumber < 64) {
            int idx = letterCount[sdn.DeviceNumber];
            if (idx < 8)
                letterMap[sdn.DeviceNumber][idx] = letter;
            letterCount[sdn.DeviceNumber]++;
        }
    }

    // If --pnp, pre-enumerate all PnP disk devices (including disabled).
    PnpDiskInfo pnpDevices[128];
    int pnpCount = 0;
    BOOL pnpShown[128];  // track which PnP entries matched an active disk
    if (showPnp) {
        pnpCount = pnp_enumerate_disks(pnpDevices, 128);
        memset(pnpShown, 0, sizeof(pnpShown));
    }

    PrintMsg(L"Disk  Type  %-10ls  %-40ls  Volumes\n", L"Size", L"Model");
    PrintMsg(L"----  ----  ----------  ----------------------------------------  -------\n");

    int consecutiveFail = 0;
    for (DWORD i = 0; i < 64 && consecutiveFail < 8; i++) {
        wchar_t drvPath[64];
        _snwprintf_s(drvPath, 64, _TRUNCATE, L"\\\\.\\PhysicalDrive%u", i);
        HANDLE h = CreateFileW(drvPath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            consecutiveFail++;
            continue;
        }
        CloseHandle(h);
        consecutiveFail = 0;

        int dt = disk_type(i);
        const wchar_t *typeStr = dt == 1 ? L"SSD" : dt == 0 ? L"HDD" : L"---";

        wchar_t model[128];
        wcscpy_s(model, 128, L"(unknown)");
        disk_get_model(i, model, 128);

        ULONGLONG sizeBytes = 0;
        wchar_t sizeStr[32];
        wcscpy_s(sizeStr, 32, L"-");
        if (disk_get_size(i, &sizeBytes))
            format_size(sizeBytes, sizeStr, 32);

        wchar_t letters[32] = {0};
        int lpos = 0;
        for (int j = 0; j < letterCount[i] && j < 8; j++) {
            if (lpos > 0) { letters[lpos++] = L','; letters[lpos++] = L' '; }
            letters[lpos++] = letterMap[i][j];
            letters[lpos++] = L':';
        }
        letters[lpos] = L'\0';

        PrintMsg(L"%4u  %-4ls  %-10ls  %-40ls  %ls\n",
                 i,
                 typeStr,
                 sizeStr,
                 model,
                 letters[0] ? letters : L"-");

        if (showPnp) {
            wchar_t pnpId[MAX_PATH] = {0};
            if (pnp_get_instance_id(i, pnpId, MAX_PATH)) {
                PrintMsg(L"      PnP:  %ls\n", pnpId);
                // Mark this PnP device as shown so we don't duplicate it below.
                for (int p = 0; p < pnpCount; p++) {
                    if (_wcsicmp(pnpDevices[p].instanceId, pnpId) == 0)
                        pnpShown[p] = TRUE;
                }
            }
        }
    }

    // Show disabled/not-present PnP disk devices that weren't listed above.
    if (showPnp) {
        BOOL headerPrinted = FALSE;
        for (int p = 0; p < pnpCount; p++) {
            if (pnpShown[p]) continue;
            if (!pnpDevices[p].disabled) continue;
            if (!headerPrinted) {
                PrintMsg(L"\nDisabled PnP disk devices:\n");
                headerPrinted = TRUE;
            }
            PrintMsg(L"  [DISABLED]  %ls\n", pnpDevices[p].instanceId);
            if (pnpDevices[p].description[0])
                PrintMsg(L"              %ls\n", pnpDevices[p].description);
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// cmd_offline  (lock + dismount + offline)
// ---------------------------------------------------------------------------

int cmd_offline(int argc, wchar_t **argv) {
    if (argc < 2) { print_usage(); return 1; }
    Target t;
    if (!init_target(argv[1], &t)) return 1;

    wchar_t volPath[MAX_PATH];
    if (t.letter) vol_letter_path(t.letter, volPath, MAX_PATH);
    else          wcscpy_s(volPath, MAX_PATH, t.ioctlPath);

    HANDLE hVol = vol_open(volPath);
    if (hVol == INVALID_HANDLE_VALUE) return 1;

    if (!vol_lock(hVol)) {
        CloseHandle(hVol);
        return 1;
    }
    vol_dismount(hVol);
    PrintMsg(L"Taking volume offline\n");
    BOOL ok = vol_offline(hVol);
    CloseHandle(hVol);
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// cmd_online
// ---------------------------------------------------------------------------

int cmd_online(int argc, wchar_t **argv) {
    if (argc < 2) { print_usage(); return 1; }
    Target t;
    if (!init_target(argv[1], &t)) return 1;

    HANDLE hVol = vol_open(t.ioctlPath);
    if (hVol == INVALID_HANDLE_VALUE) return 1;
    PrintMsg(L"Bringing volume online\n");
    BOOL ok = vol_online(hVol);
    CloseHandle(hVol);
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// cmd_apm
// ---------------------------------------------------------------------------

int cmd_apm(int argc, wchar_t **argv) {
    if (argc < 3) { print_usage(); return 1; }

    Target t;
    if (!init_target(argv[1], &t)) return 1;
    if (!resolve_disk_num(&t)) return 1;
    if (!is_hdd(t.diskNum)) return 1;

    const wchar_t *sub = argv[2];

    if (_wcsicmp(sub, L"get") == 0) {
        IdentifyData id;
        if (!disk_identify(t.diskNum, &id)) {
            eprint(L"IDENTIFY DEVICE", GetLastError());
            return 1;
        }
        BOOL sup = disk_apm_supported(&id);
        BOOL ena = disk_apm_enabled(&id);
        USHORT lvl = disk_apm_level(&id);
        PrintMsg(L"APM supported : %ls\n", sup ? L"Yes" : L"No");
        if (sup) {
            PrintMsg(L"APM enabled   : %ls\n", ena ? L"Yes" : L"No");
            if (ena) PrintMsg(L"APM level     : %u (0x%02X)\n", lvl, lvl);
        }
        return 0;
    }

    if (_wcsicmp(sub, L"enable") == 0) {
        BYTE level = 1;
        if (argc >= 4) {
            wchar_t *endptr = NULL;
            DWORD v = wcstoul(argv[3], &endptr, 10);
            if (argv[3][0] == L'\0' || *endptr != L'\0' || v < 1 || v > 254) {
                PrintMsg(L"Error: APM level must be 1-254\n");
                return 1;
            }
            level = (BYTE)v;
        }
        PrintMsg(L"Enabling APM (level %u) on PhysicalDrive%u\n", level, t.diskNum);
        return disk_apm_enable(t.diskNum, level) ? 0 : 1;
    }

    if (_wcsicmp(sub, L"disable") == 0) {
        PrintMsg(L"Disabling APM on PhysicalDrive%u\n", t.diskNum);
        return disk_apm_disable(t.diskNum) ? 0 : 1;
    }

    PrintMsg(L"Error: unknown apm subcommand '%ls'\n", sub);
    return 1;
}

// ---------------------------------------------------------------------------
// cmd_pnp
// ---------------------------------------------------------------------------

int cmd_pnp(int argc, wchar_t **argv) {
    if (argc < 3) { print_usage(); return 1; }

    wchar_t instanceId[MAX_PATH] = {0};
    const wchar_t *sub = argv[2];

    if (_wcsicmp(sub, L"enable") == 0) {
        Target t;
        if (!init_target(argv[1], &t)) return 1;

        if (t.instanceId[0] && !vol_is_accessible(t.ioctlPath)) {
            PrintMsg(L"Enabling PnP device: %ls\n", t.instanceId);
            if (!pnp_enable(t.instanceId)) return 1;
            wcscpy_s(instanceId, MAX_PATH, t.instanceId);
        } else if (vol_is_accessible(t.ioctlPath) && resolve_disk_num(&t)) {
            if (!pnp_get_instance_id(t.diskNum, instanceId, MAX_PATH)) {
                PrintMsg(L"Error: could not find PnP instance ID\n");
                return 1;
            }
            PrintMsg(L"Enabling PnP device: %ls\n", instanceId);
            if (!pnp_enable(instanceId)) return 1;
        } else {
            const wchar_t *pnpHint = t.instanceId[0] ? t.instanceId : NULL;
            if (!pnp_recover(pnpHint, instanceId, MAX_PATH)) return 1;
        }

        PrintMsg(L"Waiting for device to become ready...\n");
        if (t.ioctlPath[0]) {
            BOOL ready = FALSE;
            for (DWORD ms = 0; ms < 15000 && !ready; ms += 500) {
                if (vol_is_accessible(t.ioctlPath)) { ready = TRUE; break; }
                Sleep(500);
            }
            if (!ready)
                PrintMsg(L"Warning: timeout waiting for volume\n");
        } else {
            Sleep(2000);
        }
        return 0;
    }

    if (_wcsicmp(sub, L"disable") == 0) {
        Target t;
        if (!init_target(argv[1], &t)) return 1;
        BOOL persist = has_flag(argc - 3, argv + 3, L"--persist");

        if (t.instanceId[0] && !vol_is_accessible(t.ioctlPath)) {
            PrintMsg(L"Disabling PnP device: %ls\n", t.instanceId);
            return pnp_disable(t.instanceId, persist) ? 0 : 1;
        }

        if (!resolve_disk_num(&t)) return 1;
        if (pnp_get_instance_id(t.diskNum, instanceId, MAX_PATH)) {
            PrintMsg(L"Disabling PnP device: %ls\n", instanceId);
            return pnp_disable(instanceId, persist) ? 0 : 1;
        }
        if (t.instanceId[0]) {
            PrintMsg(L"Disabling PnP device (stored): %ls\n", t.instanceId);
            return pnp_disable(t.instanceId, persist) ? 0 : 1;
        }
        PrintMsg(L"Error: could not find PnP instance ID\n");
        return 1;
    }

    PrintMsg(L"Error: unknown pnp subcommand '%ls'\n", sub);
    return 1;
}

// ---------------------------------------------------------------------------
// cmd_map
// ---------------------------------------------------------------------------

int cmd_map(int argc, wchar_t **argv) {
    if (argc < 2) { print_usage(); return 1; }
    const wchar_t *sub = argv[1];

    if (_wcsicmp(sub, L"show") == 0) {
        MapTable ft = {0}, rt = {0};
        map_read_file(ft);
        map_read_reg(rt);
        map_show(ft, rt);
        return 0;
    }

    // set, rm, sync all require --file or --reg.
    BOOL useFile = has_flag(argc - 2, argv + 2, L"--file");
    BOOL useReg  = has_flag(argc - 2, argv + 2, L"--reg");
    if (!useFile && !useReg) {
        PrintMsg(L"Error: 'map %ls' requires --file or --reg\n", sub);
        return 1;
    }

    if (_wcsicmp(sub, L"set") == 0) {
        if (argc < 3) { print_usage(); return 1; }
        const wchar_t *target = argv[2];
        BOOL doAll = (_wcsicmp(target, L"all") == 0);
        wchar_t singleLetter = 0;
        if (!doAll) {
            singleLetter = norm_letter(target[0]);
            if (!singleLetter) {
                PrintMsg(L"Error: invalid letter '%ls'\n", target);
                return 1;
            }
        }

        MapTable t = {0};
        if (useFile) map_read_file(t);
        else         map_read_reg(t);

        if (doAll) {
            wchar_t drives[256] = {0};
            GetLogicalDriveStringsW(255, drives);
            for (wchar_t *d = drives; *d; d += wcslen(d) + 1) {
                wchar_t letter = norm_letter(d[0]);
                if (!letter) continue;
                wchar_t guid[MAX_PATH] = {0};
                if (vol_get_guid(letter, guid)) {
                    int idx = letter - L'A';
                    wcscpy_s(t[idx].guid, MAX_PATH, guid);
                    wchar_t volPath[MAX_PATH];
                    vol_letter_path(letter, volPath, MAX_PATH);
                    DWORD dn = disk_number_from_vol(volPath);
                    if (dn != (DWORD)-1) {
                        wchar_t pnp[MAX_PATH] = {0};
                        if (pnp_get_instance_id(dn, pnp, MAX_PATH))
                            wcscpy_s(t[idx].pnpId, MAX_PATH, pnp);
                    }
                    PrintMsg(L"  %lc = %ls", letter, guid);
                    if (t[idx].pnpId[0]) PrintMsg(L"  [%ls]", t[idx].pnpId);
                    PrintMsg(L"\n");
                }
            }
        } else {
            wchar_t guid[MAX_PATH] = {0};
            if (!vol_get_guid(singleLetter, guid)) {
                PrintMsg(L"Error: could not get GUID for drive %lc:\n", singleLetter);
                return 1;
            }
            int idx = singleLetter - L'A';
            wcscpy_s(t[idx].guid, MAX_PATH, guid);
            wchar_t volPath[MAX_PATH];
            vol_letter_path(singleLetter, volPath, MAX_PATH);
            DWORD dn = disk_number_from_vol(volPath);
            if (dn != (DWORD)-1) {
                wchar_t pnp[MAX_PATH] = {0};
                if (pnp_get_instance_id(dn, pnp, MAX_PATH))
                    wcscpy_s(t[idx].pnpId, MAX_PATH, pnp);
            }
            PrintMsg(L"  %lc = %ls", singleLetter, guid);
            if (t[idx].pnpId[0]) PrintMsg(L"  [%ls]", t[idx].pnpId);
            PrintMsg(L"\n");
        }

        return (useFile ? map_write_file(t) : map_write_reg(t)) ? 0 : 1;
    }

    if (_wcsicmp(sub, L"rm") == 0) {
        BOOL doAll = (argc < 3 || _wcsicmp(argv[2], L"all") == 0);
        wchar_t singleLetter = 0;
        if (!doAll) {
            singleLetter = norm_letter(argv[2][0]);
            if (!singleLetter) {
                PrintMsg(L"Error: invalid letter '%ls'\n", argv[2]);
                return 1;
            }
        }

        MapTable t = {0};
        if (!doAll) {
            if (useFile) map_read_file(t);
            else         map_read_reg(t);
            memset(&t[singleLetter - L'A'], 0, sizeof(MapEntry));
        }
        // doAll: t is zeroed = write empty table.

        return (useFile ? map_write_file(t) : map_write_reg(t)) ? 0 : 1;
    }

    if (_wcsicmp(sub, L"sync") == 0) {
        MapTable t = {0};
        if (useFile) map_read_file(t);
        else         map_read_reg(t);

        for (int i = 0; i < 26; i++) {
            if (t[i].guid[0] == L'\0') continue;
            wchar_t letter = (wchar_t)(L'A' + i);
            wchar_t guid[MAX_PATH] = {0};
            if (vol_get_guid(letter, guid)) {
                wcscpy_s(t[i].guid, MAX_PATH, guid);
                wchar_t volPath[MAX_PATH];
                vol_letter_path(letter, volPath, MAX_PATH);
                DWORD dn = disk_number_from_vol(volPath);
                if (dn != (DWORD)-1) {
                    wchar_t pnp[MAX_PATH] = {0};
                    if (pnp_get_instance_id(dn, pnp, MAX_PATH))
                        wcscpy_s(t[i].pnpId, MAX_PATH, pnp);
                }
                PrintMsg(L"  Updated %lc = %ls\n", letter, guid);
            } else {
                PrintMsg(L"  Warning: %lc: not accessible, keeping existing entry\n", letter);
            }
        }

        return (useFile ? map_write_file(t) : map_write_reg(t)) ? 0 : 1;
    }

    PrintMsg(L"Error: unknown map subcommand '%ls'\n", sub);
    return 1;
}

// ---------------------------------------------------------------------------
// Dispatch table
// ---------------------------------------------------------------------------

typedef int (*CmdHandler)(int, wchar_t **);

typedef struct {
    const wchar_t *name;
    CmdHandler     handler;
} CmdEntry;

static const CmdEntry g_commands[] = {
    { L"sleep",   cmd_sleep   },
    { L"wake",    cmd_wake    },
    { L"info",    cmd_info    },
    { L"list",    cmd_list    },
    { L"offline", cmd_offline },
    { L"online",  cmd_online  },
    { L"apm",     cmd_apm     },
    { L"pnp",     cmd_pnp     },
    { L"map",     cmd_map     },
    { NULL,       NULL        }
};

static const CmdEntry *find_command(const wchar_t *name) {
    for (const CmdEntry *e = g_commands; e->name; e++)
        if (_wcsicmp(name, e->name) == 0)
            return e;
    return NULL;
}

int cmd_dispatch(int argc, wchar_t **argv) {
    if (argc < 1) return -1;
    const CmdEntry *e = find_command(argv[0]);
    if (!e) return -1;
    return e->handler(argc, argv);
}

BOOL cmd_is_valid(int argc, wchar_t **argv) {
    return argc >= 1 && find_command(argv[0]) != NULL;
}

void print_usage(void) {
    PrintMsg(
        L"Usage: disksleep <command> <target> [options]\n"
        L"       disksleep list\n"
        L"       disksleep map <subcommand> [args]\n"
        L"\n"
        L"<target>  Drive letter (D or D:), volume GUID path (\\\\?\\Volume{...}\\),\n"
        L"          or PnP instance ID (SCSI\\DISK&VEN_...\\...)\n"
        L"\n"
        L"Commands:\n"
        L"  sleep   <target> [--no-dismount] [--no-offline] [--no-standby] [--apm]\n"
        L"                   [--no-pnp] [--force] [--no-pnp-recovery] [--persist]\n"
        L"  wake    <target> [--no-pnp] [--no-online] [--no-spinup] [--apm] [--force]\n"
        L"  info    <target>\n"
        L"  list    [--pnp]\n"
        L"  offline <target>\n"
        L"  online  <target>\n"
        L"  apm     <target> get | enable [1-254] | disable\n"
        L"  pnp     <target> enable | disable [--persist]\n"
        L"\n"
        L"Map commands (--file or --reg required for set/rm/sync):\n"
        L"  map set  <letter|all>  --file|--reg\n"
        L"  map rm   [letter|all]  --file|--reg\n"
        L"  map sync               --file|--reg\n"
        L"  map show\n"
        L"\n"
        L"Global options:\n"
        L"  --no-elevate  Do not auto-elevate via UAC; fail if not admin\n"
        L"  --version     Show version and exit\n"
    );
}
