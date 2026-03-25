#pragma once
// noCRT compatibility header.
// Force-included in noCRT configurations via ForcedIncludeFiles in vcxproj.
// Suppresses CRT dllimport so our implementations in nocrt.c link correctly.
// In CRT configurations this header is not force-included and has no effect.

#ifdef NOCRT

// Suppress __declspec(dllimport) on UCRT/CRT function declarations.
// Must be defined before any system or CRT header is included.
#ifndef _ACRTIMP
#define _ACRTIMP
#endif
#ifndef _DCRTIMP
#define _DCRTIMP
#endif
#ifndef _VCRT_DEFINED_CRTIMP
#define _VCRT_DEFINED_CRTIMP
#ifndef _CRTIMP
#define _CRTIMP
#endif
#endif

// Initialize noCRT runtime (resolve ntdll formatting functions).
// Called automatically from wmainCRTStartup before wmain.
void nocrt_init(void);

#endif // NOCRT
