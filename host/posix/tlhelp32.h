/* (linux) Tool Help: the thread snapshots the profiler and the stall reports walk. There is no snapshot here
   (CreateToolhelp32Snapshot fails), so those debugging aids find no threads. */
#ifndef HOOKSHOT_POSIX_TLHELP32_H
#define HOOKSHOT_POSIX_TLHELP32_H
#include <windows.h>
#define TH32CS_SNAPTHREAD 0x4
typedef struct {
    DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID;
    LONG tpBasePri, tpDeltaPri;
    DWORD dwFlags;
} THREADENTRY32;
static inline HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid) { (void)flags, (void)pid; return INVALID_HANDLE_VALUE; }
static inline BOOL Thread32First(HANDLE s, THREADENTRY32 *te) { (void)s, (void)te; return FALSE; }
static inline BOOL Thread32Next(HANDLE s, THREADENTRY32 *te) { (void)s, (void)te; return FALSE; }
#endif
