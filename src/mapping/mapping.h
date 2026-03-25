#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Single mapping entry: volume GUID path and optional PnP instance ID.
typedef struct {
    wchar_t guid[MAX_PATH];    // \\?\Volume{...}\ or empty
    wchar_t pnpId[MAX_PATH];   // PnP device instance ID or empty
} MapEntry;

// Mapping table: index 0 = 'A', index 25 = 'Z'.
typedef MapEntry MapTable[26];

// Build the path to the mapping file (DiskSleep_map.ini next to the exe).
void map_file_path(wchar_t *out, DWORD outLen);

// Read the mapping file into t. Missing file is treated as empty table (returns TRUE).
BOOL map_read_file(MapTable t);

// Write t to the mapping file. Deletes the file if all entries are empty.
BOOL map_write_file(const MapTable t);

// Read the mapping from HKCU\Software\DiskSleep\Map into t.
BOOL map_read_reg(MapTable t);

// Write t to HKCU\Software\DiskSleep\Map.
BOOL map_write_reg(const MapTable t);

// Look up the entry for a letter in both tables; file takes priority.
// Returns a pointer into one of the tables, or NULL if not found in either.
const MapEntry *map_lookup(wchar_t letter, const MapTable fileT, const MapTable regT);

// Search both tables for an entry whose pnpId matches (case-insensitive).
// Returns a pointer, or NULL. Fills letterOut with the mapped drive letter.
const MapEntry *map_lookup_by_pnp(const wchar_t *pnpId, const MapTable fileT,
                                   const MapTable regT, wchar_t *letterOut);

// Print all entries from both file and registry tables.
void map_show(const MapTable fileT, const MapTable regT);
