/* (hookshot) The llvm-mingw build (HOOKSHOT_MINGW, as TotK Recomp builds): the static FFmpeg libraries in the CPM cache were built
   with Microsoft's compiler and name a few things of its C runtime that the MinGW one does not have: the stack-buffer
   check (/GS), the float marker and the range-check report. Here they are, as the MSVC runtime defines them. The MSVC build
   compiles nothing of this file. */
#if defined(__MINGW32__)
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>

int _fltused = 0x9875;   /* (the value MSVC's runtime gives it: only its presence matters) */

/* /GS: a cookie in each guarded frame, checked on return. The value is set once at start (a fixed one works too: a mismatch
   means the stack was overrun, and then the process ends, as with MSVC's runtime) */
uintptr_t __security_cookie = (uintptr_t)0x00002B992DDFA232ull;

__attribute__((constructor)) static void security_cookie_init(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    uintptr_t c = (uintptr_t)t.QuadPart ^ (uintptr_t)GetCurrentProcessId() ^ ((uintptr_t)GetCurrentThreadId() << 32);
    c &= 0x0000FFFFFFFFFFFFull;   /* (MSVC keeps the top 16 bits clear) */
    if (c) __security_cookie = c;
}

void __security_check_cookie(uintptr_t cookie) {
    if (cookie != __security_cookie) __fastfail(2);   /* FAST_FAIL_STACK_COOKIE_CHECK_FAILURE */
}

void __report_rangecheckfailure(void) { __fastfail(8); }   /* FAST_FAIL_RANGE_CHECK_FAILURE */

/* MSVC's stack probe for frames over a page: the same contract on x64 as MinGW's ___chkstk_ms (RAX = the size, every page
   touched, nothing else changed) */
__asm__(".text\n.globl __chkstk\n__chkstk:\n    jmp ___chkstk_ms\n");

/* the unwind handler of /GS frames: it checks the frame's cookie, then lets the search go on */
EXCEPTION_DISPOSITION __GSHandlerCheck(PEXCEPTION_RECORD record, void *frame, PCONTEXT context, void *dispatcher) {
    (void)record, (void)frame, (void)context, (void)dispatcher;
    return ExceptionContinueSearch;
}
#endif
