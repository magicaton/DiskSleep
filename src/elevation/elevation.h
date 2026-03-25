#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Returns TRUE if the current process is running with administrator privileges.
BOOL IsRunAsAdmin(void);

// Attempt UAC elevation via Named Pipe proxy-worker architecture.
// Returns the worker's exit code, or 1 on failure.
// argc/argv are the original arguments to forward to the elevated worker.
int elevate_and_run(int argc, wchar_t **argv);

// Worker entry point: connect to parent's pipe, execute commands, send output.
// Called when --elevated-worker is detected.
// Returns the exit code of the executed commands.
int worker_main(int argc, wchar_t **argv);
