// (EvilOOT) RX_VK_HANG_DUMP=<file>: a watchdog for freezes. When no frame has been presented for RX_VK_HANG_SEC seconds
// (default 20) after the first, every thread of the process is suspended in turn and its stack unwound with Windows'
// own unwinder (the modules' .pdata); the return addresses go to <file> as module+offset, with the thread's name, once
// all threads run again. hookshot.map / hookshot_vk.map turn the offsets into functions.
// (11-07: a freeze right after Load, every thread waiting, none spinning: which wait is it.)
// Nothing is allocated, locked or logged while a thread is suspended: it may hold the heap's or the log's lock.
#ifdef _WIN32   // (hookshot: thread stacks through the Win32 debug API; the Linux build has none, see the end)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace mkwvk {
extern int64_t presents_now;

namespace {

constexpr int kMaxThreads = 512;
constexpr int kMaxFrames = 40;

struct ThreadStack {
    DWORD id;
    int frames;
    DWORD64 pc[kMaxFrames];
};
ThreadStack stacks[kMaxThreads];

int unwind(HANDLE h, DWORD64* out) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(h, &ctx)) return 0;
    int n = 0;
    for (; n < kMaxFrames && ctx.Rip; n++) {
        out[n] = ctx.Rip;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
        if (!f) {   // (a leaf: the return address on the top of the stack)
            if (IsBadReadPtr((void*)ctx.Rsp, 8)) break;
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
        } else {
            void* handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &handler_data, &frame, nullptr);
        }
        if (!ctx.Rsp) break;
    }
    return n;
}

void dump(const char* path) {
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    int count = 0;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok && count < kMaxThreads; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        ThreadStack& s = stacks[count];
        s.id = te.th32ThreadID, s.frames = 0;
        if (SuspendThread(h) != (DWORD)-1) {
            s.frames = unwind(h, s.pc);
            ResumeThread(h);
        }
        CloseHandle(h);
        count++;
    }
    CloseHandle(snap);
    // (all running again: now the names, the modules and the file)
    using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const GetDesc get_desc = (GetDesc)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# hang dump: %d threads, %lld presents\n", count, (long long)presents_now);
    for (int i = 0; i < count; i++) {
        const ThreadStack& s = stacks[i];
        char name[128] = "";
        if (get_desc) {
            if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, s.id)) {
                PWSTR w = nullptr;
                if (SUCCEEDED(get_desc(h, &w)) && w) {
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, name, sizeof name, nullptr, nullptr);
                    LocalFree(w);
                }
                CloseHandle(h);
            }
        }
        fprintf(f, "\nthread %lu \"%s\" (%d frames)\n", (unsigned long)s.id, name, s.frames);
        for (int k = 0; k < s.frames; k++) {
            HMODULE m = nullptr;
            char mod[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)s.pc[k], &m) && m) {
                char full[MAX_PATH];
                if (GetModuleFileNameA(m, full, sizeof full)) {
                    const char* b = full;
                    for (const char* q = full; *q; q++)
                        if (*q == '\\' || *q == '/') b = q + 1;
                    snprintf(mod, sizeof mod, "%s", b);
                }
            }
            fprintf(f, "  %2d %s+%#llx (%#llx)\n", k, mod, (unsigned long long)(m ? s.pc[k] - (DWORD64)m : s.pc[k]),
                    (unsigned long long)s.pc[k]);
        }
    }
    fclose(f);
}

}  // namespace

/// (vk_replay.cpp, at the first present) starts the watchdog when RX_VK_HANG_DUMP names a file
void hang_watch_start() {
    static std::atomic<bool> started{false};
    const char* path = getenv("RX_VK_HANG_DUMP");
    if (!path || !*path || started.exchange(true)) return;
    const int sec = getenv("RX_VK_HANG_SEC") ? std::max(5, atoi(getenv("RX_VK_HANG_SEC"))) : 20;
    std::thread([path, sec] {
        SetThreadDescription(GetCurrentThread(), L"mkw_vk hang watch");
        int64_t last = -1;
        int still = 0;
        bool dumped = false;
        for (;;) {
            Sleep(1000);
            const int64_t now = presents_now;
            if (now != last) last = now, still = 0, dumped = false;
            else if (++still >= sec && !dumped) dump(path), dumped = true;
        }
    }).detach();
}

}  // namespace mkwvk
#else
namespace mkwvk {
void hang_watch_start() {}   // (linux: no hang dump)
}  // namespace mkwvk
#endif
