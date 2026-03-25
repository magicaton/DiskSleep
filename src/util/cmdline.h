#pragma once
// cmdline: low-level command line utilities for Windows x86 and x64
// This is free and unencumbered software released into the public domain.
// https://github.com/skeeto/scratch/blob/master/parsers/cmdline.c
// Modified to work exclusively with UTF-16 input and output.

#define CMDLINE_CMD_MAX  32767  // max command line length on Windows
#define CMDLINE_ARGV_MAX_W (16384+(65534+(int)sizeof(wchar_t*))/(int)sizeof(wchar_t*))

// Convert a UTF-16 command line to a UTF-16 argv following
// field splitting semantics identical to CommandLineToArgvW, including
// undocumented behavior. Populates argv with pointers into itself and
// returns argc, which is always positive.
//
// Expects that cmd has no more than 32,767 (CMDLINE_CMD_MAX) elements
// including the null terminator, and argv has at least CMDLINE_ARGV_MAX_W
// elements. This covers the worst possible cases for a Windows command
// string, so no further allocation is ever necessary.
//
// Unlike CommandLineToArgvW, when the command line string is zero
// length this function does not invent an artificial argv[0] based on
// the calling module file name. To implement this behavior yourself,
// test if cmd[0] is zero and then act accordingly.
//
// This implementation follows CommandLineToArgvW's undocumented quoting
// behavior and its special first argument handling.
static int
cmdline_to_argvW(const wchar_t* cmd, wchar_t** argv)
{
    int argc = 1;  // worst case: argv[0] is an empty string
    int state = 6;  // special argv[0] state
    int slash = 0;
    wchar_t* buf = (wchar_t*)(argv + 16384);  // second half: wchar_t buffer

    argv[0] = buf;
    while (*cmd) {
        wchar_t c = *cmd++;

        switch (state) {
        case 0: switch (c) {  // outside token
        case 0x09:
        case 0x20: continue;
        case 0x22: argv[argc++] = buf;
            state = 2;
            continue;
        case 0x5c: argv[argc++] = buf;
            slash = 1;
            state = 3;
            break;
        default: argv[argc++] = buf;
            state = 1;
        } break;
        case 1: switch (c) {  // inside unquoted token
        case 0x09:
        case 0x20: *buf++ = 0;
            state = 0;
            continue;
        case 0x22: state = 2;
            continue;
        case 0x5c: slash = 1;
            state = 3;
            break;
        } break;
        case 2: switch (c) {  // inside quoted token
        case 0x22: state = 5;
            continue;
        case 0x5c: slash = 1;
            state = 4;
            break;
        } break;
        case 3:
        case 4: switch (c) {  // backslash sequence
        case 0x22: buf -= (1 + slash) >> 1;
            if (slash & 1) {
                state -= 2;
                break;
            }
            /* fallthrough */
        default: cmd -= 1;
            state -= 2;
            continue;
        case 0x5c: slash++;
        } break;
        case 5: switch (c) {  // quoted token exit
        default: cmd -= 1;
            state = 1;
            continue;
        case 0x22: state = 1;
        } break;
        case 6: switch (c) {  // begin argv[0]
        case 0x09:
        case 0x20: *buf++ = 0;
            state = 0;
            continue;
        case 0x22: state = 8;
            continue;
        default: state = 7;
        } break;
        case 7: switch (c) {  // unquoted argv[0]
        case 0x09:
        case 0x20: *buf++ = 0;
            state = 0;
            continue;
        } break;
        case 8: switch (c) {  // quoted argv[0]
        case 0x22: *buf++ = 0;
            state = 0;
            continue;
        } break;
        }

        // Direct copy since input and output are both UTF-16
        *buf++ = c;
    }

    *buf = 0;
    argv[argc] = 0;
    return argc;
}

// Convert a UTF-16 argv into a UTF-16 Windows command line string. Returns the
// length not including the null terminator, or zero if the command line
// does not fit. The output buffer length must be 1 < len <= 32,767. It
// computes an optimally-short encoding, and the smallest output length
// is 1.
//
// This function is essentially the inverse of CommandLineToArgvW.
static int
cmdline_from_argvW(wchar_t* cmd, int len, wchar_t** argv)
{
    wchar_t* p = cmd;
    wchar_t* e = cmd + len;

    for (wchar_t** arg = argv; *arg; arg++) {
        if (*arg != *argv) {
            *p++ = 0x20;
            if (p == e) return 0;
        }
        else if (!**arg) {
            continue;  // empty argv[0] special case
        }

        int quoted = !**arg;  // empty non-first args need quoting as ""
        for (wchar_t* s = *arg; *s && !quoted; s++) {
            quoted |= *s == 0x09;
            quoted |= *s == 0x20;
        }
        if (quoted) {
            *p++ = 0x22;
            if (p == e) return 0;
        }

        int state = 0;
        int slash = 0;
        for (wchar_t* s = *arg; *s; s++) {
            switch (state) {
            case 0: switch (*s) {  // passthrough
            case 0x22: *p++ = 0x5c;
                if (p == e) return 0;
                break;
            case 0x5c: slash = 1;
                state = 1;
            } break;
            case 1: switch (*s) {  // backslash sequence
            case 0x22: for (int i = 0; i < slash + 1; i++) {
                *p++ = 0x5c;
                if (p == e) return 0;
            }
                     /* fallthrough */
            default: state = 0;
                break;
            case 0x5c: slash++;
            } break;
            }

            // Direct copy since input and output are both UTF-16
            *p++ = *s;
            if (p == e) return 0;
        }

        if (quoted) {
            for (; state && slash; slash--) {
                *p++ = 0x5c;
                if (p == e) return 0;
            }
            *p++ = 0x22;
            if (p == e) return 0;
        }
    }

    if (p == cmd) {
        *p++ = 0x20;
    }
    *p = 0;
    return (int)(p - cmd);
}
