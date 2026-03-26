#include "elevation.h"
#include "cmd.h"
#include "cmdline.h"
#include "util.h"
#include <sddl.h>
#include <shellapi.h>
#include <stdlib.h>
#include <stdio.h>

BOOL IsRunAsAdmin(void) {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;

    if (!AllocateAndInitializeSid(&ntAuth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminGroup))
        return FALSE;

    CheckTokenMembership(NULL, adminGroup, &isAdmin);
    FreeSid(adminGroup);
    return isAdmin;
}


// ---------------------------------------------------------------------------
// Pipe helpers
// ---------------------------------------------------------------------------

// Build SDDL string "D:(A;;GA;;;<UserSID>)" using the current user's SID.
// Returns TRUE on success, fills sddl buffer.
static BOOL build_pipe_sddl(wchar_t *sddl, int sddlLen) {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        eprint(L"OpenProcessToken", GetLastError());
        return FALSE;
    }

    DWORD needed = 0;
    GetTokenInformation(hToken, TokenUser, NULL, 0, &needed);
    TOKEN_USER *tu = (TOKEN_USER *)malloc(needed);
    if (!tu) {
        CloseHandle(hToken);
        return FALSE;
    }

    BOOL ok = GetTokenInformation(hToken, TokenUser, tu, needed, &needed);
    CloseHandle(hToken);
    if (!ok) {
        eprint(L"GetTokenInformation", GetLastError());
        free(tu);
        return FALSE;
    }

    wchar_t *sidStr = NULL;
    if (!ConvertSidToStringSidW(tu->User.Sid, &sidStr)) {
        eprint(L"ConvertSidToStringSidW", GetLastError());
        free(tu);
        return FALSE;
    }
    free(tu);

    _snwprintf_s(sddl, sddlLen, _TRUNCATE, L"D:(A;;GA;;;%ls)", sidStr);
    LocalFree(sidStr);
    return TRUE;
}

static HANDLE create_output_pipe(wchar_t *pipeName, int pipeNameLen) {
    DWORD pid = GetCurrentProcessId();
    _snwprintf_s(pipeName, pipeNameLen, _TRUNCATE,
                 L"\\\\.\\pipe\\DiskSleep_%u", pid);

    // Build DACL restricted to the current user.
    wchar_t sddl[256];
    if (!build_pipe_sddl(sddl, _countof(sddl)))
        return INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    PSECURITY_DESCRIPTOR psd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &psd, NULL)) {
        eprint(L"ConvertStringSecurityDescriptorToSecurityDescriptorW", GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    sa.lpSecurityDescriptor = psd;

    HANDLE hPipe = CreateNamedPipeW(
        pipeName,
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 0, 4096, 0, &sa);

    if (hPipe == INVALID_HANDLE_VALUE)
        eprint(L"CreateNamedPipeW", GetLastError());

    LocalFree(psd);
    return hPipe;
}

// ---------------------------------------------------------------------------
// elevate_and_run — parent side
// ---------------------------------------------------------------------------

int elevate_and_run(int argc, wchar_t **argv) {
    wchar_t pipeName[256];
    HANDLE hPipe = create_output_pipe(pipeName, _countof(pipeName));
    if (hPipe == INVALID_HANDLE_VALUE)
        return 1;

    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    BOOL connected = ConnectNamedPipe(hPipe, &ov);
    DWORD connectErr = GetLastError();
    if (!connected && connectErr != ERROR_IO_PENDING && connectErr != ERROR_PIPE_CONNECTED) {
        eprint(L"ConnectNamedPipe", connectErr);
        CloseHandle(ov.hEvent);
        CloseHandle(hPipe);
        return 1;
    }

    DWORD parentPid = GetCurrentProcessId();
    int workerArgc = argc + 3;
    wchar_t **workerArgv = (wchar_t **)calloc((size_t)workerArgc + 1, sizeof(wchar_t *));
    if (!workerArgv) {
        CloseHandle(ov.hEvent);
        CloseHandle(hPipe);
        return 1;
    }

    wchar_t pidStr[32];
    _snwprintf_s(pidStr, _countof(pidStr), _TRUNCATE, L"%u", parentPid);

    workerArgv[0] = L"";
    workerArgv[1] = L"--elevated-worker";
    workerArgv[2] = pidStr;
    workerArgv[3] = pipeName;
    for (int i = 1; i < argc; i++)
        workerArgv[3 + i] = argv[i];
    workerArgv[workerArgc] = NULL;

    wchar_t params[CMDLINE_CMD_MAX];
    int paramLen = cmdline_from_argvW(params, CMDLINE_CMD_MAX, workerArgv);
    free(workerArgv);

    if (paramLen == 0) {
        PrintMsg(L"Error: command line too long for worker\n");
        CloseHandle(ov.hEvent);
        CloseHandle(hPipe);
        return 1;
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    SHELLEXECUTEINFOW sei = {0};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED)
            PrintMsg(L"Error: UAC elevation was cancelled by the user\n");
        else
            eprint(L"ShellExecuteExW", err);
        CancelIo(hPipe);
        CloseHandle(ov.hEvent);
        CloseHandle(hPipe);
        return 1;
    }

    // Wait for worker to connect (timeout 30s).
    if (connectErr != ERROR_PIPE_CONNECTED) {
        DWORD waitResult = WaitForSingleObject(ov.hEvent, 30000);
        if (waitResult != WAIT_OBJECT_0) {
            PrintMsg(L"Error: elevated worker did not connect (timeout or error)\n");
            TerminateProcess(sei.hProcess, 1);
            CloseHandle(sei.hProcess);
            CancelIo(hPipe);
            CloseHandle(ov.hEvent);
            CloseHandle(hPipe);
            return 1;
        }
    }
    CloseHandle(ov.hEvent);

    // Read from pipe and write to stdout (encoding-aware).
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD consoleMode;
    BOOL stdoutIsConsole = GetConsoleMode(hStdOut, &consoleMode);

    char readBuf[4096];
    DWORD bytesRead;
    while (ReadFile(hPipe, readBuf, sizeof(readBuf), &bytesRead, NULL) && bytesRead > 0) {
        DWORD written;
        if (stdoutIsConsole) {
            // Convert UTF-8 from worker back to UTF-16 for console.
            wchar_t wideBuf[4096];
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, readBuf, (int)bytesRead,
                                              wideBuf, _countof(wideBuf));
            if (wideLen > 0)
                WriteConsoleW(hStdOut, wideBuf, (DWORD)wideLen, &written, NULL);
        } else {
            // Pass through UTF-8 for pipes, files, mintty.
            WriteFile(hStdOut, readBuf, bytesRead, &written, NULL);
        }
    }

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);
    CloseHandle(hPipe);

    return (int)exitCode;
}

// ---------------------------------------------------------------------------
// worker_main — worker (elevated) side
// ---------------------------------------------------------------------------

static DWORD WINAPI monitor_parent_thread(LPVOID param) {
    HANDLE hParent = (HANDLE)param;
    WaitForSingleObject(hParent, INFINITE);
    CloseHandle(hParent);
    ExitProcess(1);
    return 0;
}

int worker_main(int argc, wchar_t **argv) {
    if (argc < 4)
        return 1;

    wchar_t *endptr = NULL;
    DWORD parentPid = (DWORD)wcstoul(argv[1], &endptr, 10);
    if (argv[1][0] == L'\0' || *endptr != L'\0')
        return 1;
    const wchar_t *pipeName = argv[2];

    if (!IsRunAsAdmin())
        return 1;

    HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!hParent)
        return 1;

    if (WaitForSingleObject(hParent, 0) == WAIT_OBJECT_0) {
        CloseHandle(hParent);
        return 1;
    }

    HANDLE hPipe = CreateFileW(
        pipeName, GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL);

    if (hPipe == INVALID_HANDLE_VALUE) {
        CloseHandle(hParent);
        return 1;
    }

    output_set_handle(hPipe);

    HANDLE hParentDup;
    if (DuplicateHandle(GetCurrentProcess(), hParent, GetCurrentProcess(), &hParentDup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        HANDLE hThread = CreateThread(NULL, 0, monitor_parent_thread, hParentDup, 0, NULL);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            CloseHandle(hParentDup);
        }
    }

    int result = cmd_dispatch(argc - 3, argv + 3);
    if (result == -1) {
        PrintMsg(L"Error: unknown command in worker\n");
        result = 1;
    }

    FlushFileBuffers(hPipe);
    CloseHandle(hPipe);
    CloseHandle(hParent);

    return result;
}
