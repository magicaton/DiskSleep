#include <wchar.h>
#include <windows.h>
#include "cmd.h"
#include "elevation.h"
#include "util.h"
#include "version.h"

static LONG WINAPI ExceptionFilter(EXCEPTION_POINTERS *info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    wchar_t msg[256];
    swprintf_s(msg, 256, L"Unhandled exception: 0x%08X", code);
    MessageBoxW(NULL, msg, L"DiskSleep", MB_APPLMODAL | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

int wmain(int argc, wchar_t **argv) {
    SetUnhandledExceptionFilter(ExceptionFilter);
    output_init();

    if (argc >= 2 && _wcsicmp(argv[1], L"--elevated-worker") == 0)
        return worker_main(argc - 1, argv + 1);

    if (argc >= 2 && _wcsicmp(argv[1], L"--version") == 0) {
#ifdef NOCRT
        PrintMsg(L"DiskSleep " VERSION_WSTR L" (noCRT)\n");
#else
        PrintMsg(L"DiskSleep " VERSION_WSTR L"\n");
#endif
        return 0;
    }

    if (argc < 2) {
        print_usage();
        return 1;
    }

    int userArgc = argc - 1;
    wchar_t **userArgv = argv + 1;

    if (!cmd_is_valid(userArgc, userArgv)) {
        PrintMsg(L"Error: unknown command '%ls'\n", userArgv[0]);
        print_usage();
        return 1;
    }

    HANDLE hMutex = CreateMutexW(NULL, FALSE, L"Global\\DiskSleep");
    if (!hMutex) {
        eprint(L"CreateMutexW", GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        PrintMsg(L"Error: another instance of DiskSleep is already running\n");
        CloseHandle(hMutex);
        return 1;
    }

    int result;
    if (!IsRunAsAdmin()) {
        BOOL noElevate = FALSE;
        for (int i = 0; i < argc; i++)
            if (_wcsicmp(argv[i], L"--no-elevate") == 0) noElevate = TRUE;
        if (noElevate) {
            PrintMsg(L"Error: administrator privileges required (use elevated prompt or remove --no-elevate)\n");
            result = 1;
        } else {
            result = elevate_and_run(argc, argv);
        }
    } else {
        result = cmd_dispatch(userArgc, userArgv);
        if (result == -1) {
            PrintMsg(L"Error: unknown command\n");
            print_usage();
            result = 1;
        }
    }

    CloseHandle(hMutex);
    return result;
}
