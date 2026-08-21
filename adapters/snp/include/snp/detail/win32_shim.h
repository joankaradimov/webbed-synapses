/*
 * Minimal Win32 type shim, used only so the ABI header can be compiled and
 * layout-checked off Windows (see the `abi-check` CMake preset). Real builds
 * include <windows.h> instead -- this file is never part of a shipping module.
 *
 * Deliberately freestanding: it uses the compiler's own width macros rather
 * than <stdint.h>, so layout can be checked with a 32-bit-capable compiler on
 * a host that has no 32-bit libc headers installed.
 */
#ifndef SNP_DETAIL_WIN32_SHIM_H
#define SNP_DETAIL_WIN32_SHIM_H

#ifdef _WIN32
#error "win32_shim.h must not be used on Windows; include <windows.h>."
#endif

typedef __UINT32_TYPE__ DWORD;
typedef __UINT16_TYPE__ WORD;
typedef __UINT8_TYPE__  BYTE;
typedef int             BOOL;
typedef void*           HANDLE;
typedef void*           HWND;
typedef void*           LPVOID;
typedef const void*     LPCVOID;
typedef char*           LPSTR;
typedef const char*     LPCSTR;

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; __INT32_TYPE__ HighPart; } u;
    __INT64_TYPE__ QuadPart;
} LARGE_INTEGER;

#if defined(__i386__)
#define SNP_STDCALL __attribute__((stdcall))
#else
/* stdcall is meaningless off x86; layout checking still works. */
#define SNP_STDCALL
#endif

#define APIENTRY SNP_STDCALL
#define CALLBACK SNP_STDCALL

#endif /* SNP_DETAIL_WIN32_SHIM_H */
