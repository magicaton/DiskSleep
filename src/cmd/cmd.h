#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ---------------------------------------------------------------------------
// Resolved target information
// ---------------------------------------------------------------------------

typedef struct {
    wchar_t letter;               // Uppercase drive letter, or 0 if target was a GUID path or PnP ID.
    wchar_t guidPath[MAX_PATH];   // \\?\Volume{...}\ (with trailing backslash), or empty.
    wchar_t ioctlPath[MAX_PATH];  // \\.\Volume{...} or \\.\D: — ready for CreateFileW.
    wchar_t instanceId[MAX_PATH]; // PnP device instance ID (from mapping or direct input), or empty.
    DWORD   diskNum;              // Physical disk number, or (DWORD)-1 if not yet resolved.
} Target;

// ---------------------------------------------------------------------------
// Command argument structures
// ---------------------------------------------------------------------------

typedef struct {
    Target target;
    BOOL noDismount;    // skip FSCTL_DISMOUNT_VOLUME
    BOOL noOffline;     // skip IOCTL_VOLUME_OFFLINE
    BOOL useApm;        // --apm: use APM instead of STANDBY IMMEDIATE
    BOOL noStandby;     // --no-standby: skip standby/APM step
    BOOL noPnp;         // skip PnP disable at the end
    BOOL force;         // skip state pre-check
    BOOL noPnpRecovery; // do not attempt PnP recovery if disk not found
    BOOL persist;       // use CM_DISABLE_PERSIST for PnP disable
} SleepArgs;

typedef struct {
    Target target;
    BOOL noPnp;         // skip PnP enable
    BOOL noOnline;      // skip IOCTL_VOLUME_ONLINE
    BOOL noSpinup;      // skip forced spin-up
    BOOL useApm;        // --apm: disable APM after waking
    BOOL force;         // skip state pre-check
} WakeArgs;

typedef struct {
    Target target;
} InfoArgs;

typedef struct {
    Target target;
} OfflineArgs;

typedef struct {
    Target target;
} OnlineArgs;


typedef enum { APM_GET, APM_ENABLE, APM_DISABLE } ApmSubCmd;
typedef struct {
    Target   target;
    ApmSubCmd sub;
    BYTE     level; // for APM_ENABLE; default 1
} ApmArgs;

typedef enum { PNP_ENABLE, PNP_DISABLE } PnpSubCmd;
typedef struct {
    Target    target;
    PnpSubCmd sub;
} PnpArgs;

typedef enum { MAP_SET, MAP_RM, MAP_SYNC, MAP_SHOW } MapSubCmd;
typedef struct {
    MapSubCmd sub;
    wchar_t   letter;   // normalized, or 0 if 'all'
    BOOL      useAll;
    BOOL      useFile;
    BOOL      useReg;
} MapArgs;

// ---------------------------------------------------------------------------
// Command entry points
// ---------------------------------------------------------------------------

int cmd_sleep(int argc, wchar_t **argv);
int cmd_wake(int argc, wchar_t **argv);
int cmd_info(int argc, wchar_t **argv);
int cmd_list(int argc, wchar_t **argv);
int cmd_offline(int argc, wchar_t **argv);
int cmd_online(int argc, wchar_t **argv);
int cmd_apm(int argc, wchar_t **argv);
int cmd_pnp(int argc, wchar_t **argv);
int cmd_map(int argc, wchar_t **argv);

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

// Dispatch a command. argv[0] = command name (e.g. "sleep", "map").
// Returns exit code, or -1 if command not found.
int cmd_dispatch(int argc, wchar_t **argv);

// Returns TRUE if argv[0] is a known command name.
BOOL cmd_is_valid(int argc, wchar_t **argv);

void print_usage(void);
