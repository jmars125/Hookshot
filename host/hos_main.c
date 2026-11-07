/* Horizon OS layer, native host: process start and boot.

   What rtld and nn::init::Start do on hardware (see ports/zelda-totk/PORT.md, "How the game starts"), done here:
   map the guest window, load the relocated image (nx_image.py already linked it), make this host thread the
   guest main thread, run every module's DT_INIT in rtld's order (the SDK crt's _init: TLS registration with musl,
   then the module's constructors), then nninitStartup and nnMain.

   Usage: <game>.exe image.bin --romfs <dir> --user <dir>
   Environment: HOS_TRACE=1 (log HLE calls), HOS_MEMORY_MB (memory the game is told it has),
                HOS_NVN_CONTINUE=1 (go on past nvnBootstrapLoader). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef HOOKSHOT_VERSION   /* (nx2/build.py gives it: the release and a fingerprint of the sources, nx2/version.py) */
#define HOOKSHOT_VERSION "unknown"
#endif
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#else
#include <windows.h>   /* (linux: host/posix's) */
#include <sys/mman.h>
#endif
/* (linux) the separator of the paths the executable's folder is joined with */
#ifdef _WIN32
#define SEP 92
#define SEP_S "\\"
#else
#define SEP '/'
#define SEP_S "/"
#endif

#ifdef A64_MEM_ZERO
/* web: guest address = wasm address; the guest window is wasm memory below the host's data (-sGLOBAL_BASE) */
extern char __global_base;
uint64_t hos_space_end;
#define GUEST_SPACE (hos_space_end)
#else
#define GUEST_SPACE HOS_GUEST_SPACE
#endif
#define IMAGE_BASE hos_image_base   /* (the lift's: Eden's layout) */

#ifdef _WIN32
static void no_power_throttling(void) {
    const char *e = getenv("HOS_POWER_THROTTLING");
    if (e && atoi(e)) return;
    typedef BOOL(WINAPI * SetInfo)(HANDLE, int, LPVOID, DWORD);
    SetInfo set = (SetInfo)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    if (!set) return;
    struct { ULONG version, control, state; } s = {1, 0x1 | 0x4, 0};
    if (!set(GetCurrentProcess(), 4, &s, sizeof s)) {
        s.control = 0x1;
        set(GetCurrentProcess(), 4, &s, sizeof s);
    }
}
#endif

int hos_game_is_mkw(void) { return !strcmp(hos_main_build_id, "d00e4f399e411d0613aa8d26b1c02fef"); }

/* gen/hos_image.c (build.py lift) */
extern const uint64_t hos_main_stack_size;
extern const int hos_main_priority, hos_main_core, hos_module_count;
extern const uint64_t hos_module_init[];
extern const char *const hos_module_name[];
extern const uint64_t hos_entry_nninitStartup, hos_entry_nnMain;
extern const uint64_t hos_entry_init_libc0, hos_entry_init_libc1, hos_entry_init_libc2, hos_entry_nnosInitialize,
    hos_entry_nninitInitializeSdkModule;
/* nn_fs.c */
extern const char *hos_romfs_dir, *hos_user_dir;

/* HOS_LARGE_PAGES=1 (experiment): the guest window in 2 MB pages, for fewer TLB misses (the game touches hundreds of MB
   of scattered heap; 4 KB pages cover only ~12 MB of it in the CPU's TLB). The whole window is then physical memory at
   once (4 GB, locked), and Windows needs the account to hold "Lock pages in memory" (SeLockMemoryPrivilege, Local
   Security Policy > User Rights Assignment; log off and on after): without it this logs why and uses normal pages. */
int hos_large_pages;   /* (hos_mem.c: freed pages are not discarded then) */
#if defined(_WIN32) && !defined(A64_MEM_ZERO)
#pragma comment(lib, "advapi32.lib")   /* (the token privilege calls) */
static void *reserve_large(SIZE_T size) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return NULL;
    TOKEN_PRIVILEGES tp = {0};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL looked = LookupPrivilegeValueA(NULL, "SeLockMemoryPrivilege", &tp.Privileges[0].Luid);
    const BOOL adjusted = looked && AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    const DWORD err = GetLastError();
    CloseHandle(token);
    if (!adjusted || err == ERROR_NOT_ALL_ASSIGNED) {
        hos_log("mem: HOS_LARGE_PAGES: this account does not hold SeLockMemoryPrivilege (\"Lock pages in memory\"): "
                "normal pages");
        return NULL;
    }
    const SIZE_T large = GetLargePageMinimum();
    if (!large || size % large) return NULL;
    void *p = VirtualAlloc((void *)A64_HOST_BASE, size, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
    if (!p) hos_log("mem: HOS_LARGE_PAGES: %llu MB of large pages not available (error %lu): normal pages",
                    (unsigned long long)(size >> 20), GetLastError());
    return p;
}
#endif

/* The guest window lives at the constant A64_HOST_BASE (rt/a64.h), so lifted code never loads its base. */
static int reserve_guest(void) {
#if defined(A64_MEM_ZERO)
    hos_space_end = (uint64_t)(uintptr_t)&__global_base & ~0xFFFFULL;
    return hos_space_end >= (1ULL << 30);
#elif defined(_WIN32)
    /* Committed but untouched pages cost no physical memory. Write-watched: the renderer's buffer cache asks which
       pages changed (GetWriteWatch) so that it uploads only those (soft write watch builds mark pages themselves). */
#if defined(A64_WRITE_TRACK)
    const DWORD watch = 0;
    if (getenv("HOS_LARGE_PAGES") && reserve_large(GUEST_SPACE) == (void *)A64_HOST_BASE) {
        hos_large_pages = 1;
        hos_log("mem: the guest window is in large pages (%llu MB)", (unsigned long long)(GUEST_SPACE >> 20));
        return 1;
    }
#else
    const DWORD watch = MEM_WRITE_WATCH;
#endif
    /* mkw-recomp: 512 GB reserved (write-watched: the renderer's change tracking, a 16 MB bitmap), committed as the
       runtime hands memory out (hos_commit) or on first touch */
    void hos_commit_init(void);
    hos_commit_init();
    return VirtualAlloc((void *)A64_HOST_BASE, GUEST_SPACE, MEM_RESERVE | watch, PAGE_NOACCESS) == (void *)A64_HOST_BASE;
#else
    /* (linux) the whole window readable and writable at once, its pages made as they are first touched (no commit
       step: MAP_NORESERVE). Decommitting is madvise (host/posix/win32_posix.c VirtualFree): the pages read as zero. */
    void hos_commit_init(void);
    hos_commit_init();   /* (the crash report) */
    if (mmap((void *)A64_HOST_BASE, GUEST_SPACE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0) != (void *)A64_HOST_BASE)
        return 0;
    /* (issue 312) no transparent huge pages in the guest window: the renderer imports its pools as userptr memory, and every
       collapse khugepaged makes in them changes their mappings, which the kernel answers by pinning the pages again at the
       next submission: ~250 ms a submit on RADV while the GPU sat idle. HOS_THP=1: as before */
    if (!getenv("HOS_THP")) madvise((void *)A64_HOST_BASE, GUEST_SPACE, MADV_NOHUGEPAGE);
    return 1;
#endif
}

/* The guest window, for the renderer (gpu/render/rx_buffer.cpp). */
uint8_t *hos_guest_base(void) { return A64_MEM; }
uint64_t hos_guest_size(void) { return GUEST_SPACE; }

/* Call guest code at `addr` as a fresh call from the top of the main thread's stack, with whatever arguments the
   caller put in x0-x7 (call_guest clears x0 and x1 first; call_guest_args keeps them). */
static void call_guest_args(Ctx *c, uint64_t addr, uint64_t sp, const char *what) {
    A64Fn f = a64_lookup(addr);
    if (!addr || !f) hos_fatal(c, "%s: no function at %#llx", what, (unsigned long long)addr);
    hos_log("boot: %s (%#llx)", what, (unsigned long long)addr);
    c->x[30] = 0;
    c->x[31] = sp;
    void hos_mem_checkpoint(const char *what);
    hos_mem_checkpoint(what);
    f(c);
}
static void call_guest(Ctx *c, uint64_t addr, uint64_t sp, const char *what) {
    c->x[0] = c->x[1] = 0;
    call_guest_args(c, addr, sp, what);
}

void hos_update_check(void);   /* (host/hos_update.c) */
#ifndef _WIN32
/* (linux) Saves live in $XDG_DATA_HOME/hookshot (~/.local/share/hookshot), so a rebuilt game folder still finds them.
   A user/ folder beside the executable from before is moved there the first time, entry by entry. Anything that
   cannot be moved keeps the old place. --user still wins. */
static int is_dir(const char *p) {
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static int copy_tree(const char *from, const char *to) {
    if (!is_dir(from)) {
        FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
        int ok = in && out;
        char buf[1 << 16];
        for (size_t n; ok && (n = fread(buf, 1, sizeof buf, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
        ok = ok && in && !ferror(in);
        if (in) fclose(in);
        if (out && fclose(out)) ok = 0;
        return ok;
    }
    if (!CreateDirectoryA(to, NULL) && !is_dir(to)) return 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s" SEP_S "*", from);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    int ok = 1;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char a[MAX_PATH], b[MAX_PATH];
            snprintf(a, sizeof a, "%s" SEP_S "%s", from, fd.cFileName);
            snprintf(b, sizeof b, "%s" SEP_S "%s", to, fd.cFileName);
            ok = copy_tree(a, b);
        } while (ok && FindNextFileA(h, &fd));
        FindClose(h);
    }
    return ok;
}
static void delete_tree(const char *p) {
    if (!is_dir(p)) {
        DeleteFileA(p);
        return;
    }
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s" SEP_S "*", p);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char a[MAX_PATH];
            snprintf(a, sizeof a, "%s" SEP_S "%s", p, fd.cFileName);
            delete_tree(a);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(p);
}
static const char *saves_dir(const char *beside) {
    static char dir[MAX_PATH];
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0] == '/') snprintf(dir, sizeof dir, "%s", xdg);
    else if (home && home[0] == '/') {
        snprintf(dir, sizeof dir, "%s/.local", home);
        CreateDirectoryA(dir, NULL);
        snprintf(dir, sizeof dir, "%s/.local/share", home);
    } else return beside;
    CreateDirectoryA(dir, NULL);
    snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "/hookshot");
    CreateDirectoryA(dir, NULL);
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) return beside;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s" SEP_S "*", beside);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return dir;
    int moved = 0, kept = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char from[MAX_PATH], to[MAX_PATH];
        snprintf(from, sizeof from, "%s" SEP_S "%s", beside, fd.cFileName);
        snprintf(to, sizeof to, "%s" SEP_S "%s", dir, fd.cFileName);
        if (GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES) kept++;
        else if (MoveFileExA(from, to, 0)) moved++;
        else if (copy_tree(from, to)) delete_tree(from), moved++;
        else delete_tree(to), kept++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (kept) {
        hos_log("saves: %d of %s's entries could not go to %s: the saves stay in %s", kept, beside, dir, beside);
        return beside;
    }
    RemoveDirectoryA(beside);
    if (moved) {
        char note[MAX_PATH];
        const char *sep = strrchr(beside, SEP);
        snprintf(note, sizeof note, "%.*s" SEP_S "saves-moved.txt", sep ? (int)(sep - beside) : 0, beside);
        FILE *f = fopen(note, "w");
        if (f) {
            fprintf(f, "Hookshot's saves are now in\n%s\n\nThey moved there from the user folder that was here.\n", dir);
            fclose(f);
        }
        hos_log("saves: moved from %s to %s", beside, dir);
    }
    return dir;
}
#endif

/* hookshot_env.txt beside the game: NAME=VALUE lines set as environment variables before anything reads them, for the
   switches players are asked to test (RX_VK_NOOPT and the rest). Lines starting with # are notes. On Linux this runs
   before the window, so HOS_HEADLESS and HOS_WINDOW_SIZE are already set when the window is made. */
static void load_hookshot_env(void) {
    char path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n && path[n - 1] != 92 && path[n - 1] != 47) n--;
    memcpy(path + n, "hookshot_env.txt", 17);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;   /* (Notepad's BOM) */
        while (*s == ' ' || *s == 9) s++;
        size_t len = strlen(s);
        while (len && (s[len - 1] == 10 || s[len - 1] == 13 || s[len - 1] == ' ' || s[len - 1] == 9)) s[--len] = 0;
        if (!len || *s == '#' || !strchr(s, '=')) continue;
#ifdef _WIN32
        _putenv(s);
#else
        char *eq = strchr(s, '=');
        *eq = 0;
        setenv(s, eq + 1, 1);
#endif
    }
    fclose(f);
}

float hos_aspect_ratio = 0;   /* (the window's default shape: host/display.c) */
#ifdef _WIN32
/* (hookshot) On laptops with two GPUs, the drivers run a program on the dedicated one when it exports these (NVIDIA Optimus, AMD
   PowerXpress): without them a laptop could put the game on its integrated GPU, 5 fps (issue 176) */
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

int main(int argc, char **argv) {
#else
static int game_main(int argc, char **argv) {
#endif
#ifdef _WIN32
    load_hookshot_env();
    {   // (the whole process, not only the window's thread: the renderer asks the window's size from its own, and on a 4K screen at 150%
        // Windows answered such a thread 2560 x 1440: the picture was that size, in the corner of the window. Hookshot issues 15, 18, 20)
        typedef BOOL(WINAPI * SetAware)(void *);
        SetAware set = (SetAware)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
        if (set && !getenv("HOS_DPI_UNAWARE")) set((void *)(intptr_t)-4);
    }
#endif
    {   // (started from its icon the game has nowhere to log to: hookshot_log.txt beside it, last run only, for players to send in)
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (!err || err == INVALID_HANDLE_VALUE || GetFileType(err) == FILE_TYPE_UNKNOWN) {
            char path[MAX_PATH + 32];
            DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
            while (n && path[n - 1] != 92 && path[n - 1] != 47) n--;
            memcpy(path + n, "hookshot_log.txt", 17);
            if (freopen(path, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
        }
    }
    hos_log("hookshot %s", HOOKSHOT_VERSION);   /* (the log's first line: which build a report comes from) */
    const char *image = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--romfs") && i + 1 < argc) hos_romfs_dir = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) hos_user_dir = argv[++i];
        else if (!strcmp(argv[i], "--trace")) hos_trace = 1;
        else if (!strcmp(argv[i], "--prepare-shaders")) {   /* (hookshot) a build's last step: no game */
#ifdef _WIN32
            _putenv("HOOKSHOT_PREPARE_ONLY=1");
#else
            setenv("HOOKSHOT_PREPARE_ONLY", "1", 1);
#endif
            int rx_prepare_shaders(void);
            if (rx_prepare_shaders()) {
                printf("@@done shaders\n");
                return 0;
            }
            printf("@@error no usable Vulkan here: the game prepares its shaders at its first start\n");
            return 3;
        }
        else if (!image) image = argv[i];
    }
    static char beside_image[MAX_PATH], beside_romfs[MAX_PATH], beside_user[MAX_PATH];
    if (!image) {   /* (hookshot) started with nothing, as a game is: everything beside the executable (the builder's folder) */
        char dir[MAX_PATH];
        const DWORD len = GetModuleFileNameA(NULL, dir, MAX_PATH);
        char *slash = len ? strrchr(dir, SEP) : NULL;
        if (slash) {
            *slash = 0;
            SetCurrentDirectoryA(dir);
            snprintf(beside_image, MAX_PATH, "%s" SEP_S "image.bin", dir);
            snprintf(beside_romfs, MAX_PATH, "%s" SEP_S "romfs", dir);
            snprintf(beside_user, MAX_PATH, "%s" SEP_S "user", dir);
            if (GetFileAttributesA(beside_image) != INVALID_FILE_ATTRIBUTES) {
                hos_update_check();   /* (a game folder from the builder: is a newer release out? host/hos_update.c) */
                image = beside_image;
                if (!hos_romfs_dir) hos_romfs_dir = beside_romfs;
#ifndef _WIN32
                if (!hos_user_dir) hos_user_dir = saves_dir(beside_user);
#else
                if (!hos_user_dir) hos_user_dir = beside_user;
#endif
            }
        }
    }
    if (!image) {
        fprintf(stderr, "usage: %s image.bin --romfs <dir> --user <dir> [--trace]\n", argv[0]);
        return 2;
    }
    if (getenv("HOS_TRACE")) hos_trace = 1;
    if (!hos_user_dir) hos_user_dir = "user";
    CreateDirectoryA(hos_user_dir, NULL);
    if (!reserve_guest()) {
        fprintf(stderr, "cannot reserve the guest window\n");
        return 1;
    }
#ifdef A64_MEM_ZERO
    web_fs_init(NULL, hos_romfs_dir);
    /* (the whole file, no range: the server sends it brotli-compressed, 85 -> 26 MB) */
    const int got = web_fetch_all(image, A64_MEM + IMAGE_BASE, GUEST_SPACE - IMAGE_BASE);
    if (got < 0) {
        fprintf(stderr, "cannot fetch %s\n", image);
        return 1;
    }
    size_t n = (size_t)got;
#else
    FILE *f = fopen(image, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", image);
        return 1;
    }
    /* exactly the file's size: a read into "the rest of the guest window" (~4 GB) made Windows commit physical memory
       for ~2 GB of it before the game ran (the whole heap was resident from the start, ~1.3 GB of it never used) */
    fseek(f, 0, SEEK_END);
    const long long file_size = _ftelli64(f);
    fseek(f, 0, SEEK_SET);
    if (file_size > 0) hos_commit(IMAGE_BASE, (uint64_t)file_size);
    size_t n = file_size > 0 && (uint64_t)file_size <= GUEST_SPACE - IMAGE_BASE
                   ? fread(A64_MEM + IMAGE_BASE, 1, (size_t)file_size, f) : 0;
    fclose(f);
#endif
    if (IMAGE_BASE + n != hos_image_end) {
        fprintf(stderr, "%s: %zu bytes, the lifted code expects %#llx: lift and image do not match\n", image, n,
                (unsigned long long)(hos_image_end - IMAGE_BASE));
        return 1;
    }
    hos_mem_init(hos_image_end);
    {   /* (hookshot) HOS_ASPECT=<ratio> (2.3889 for 3440x1440): the one 16:9 constant in the game's data, read by the camera and the
           scene setup, at the window's shape. The picture stays 16:9 and is stretched to the window, so the 3D keeps its proportions
           and the HUD stretches with it. */
        const char *e = getenv("HOS_ASPECT");
        uint8_t *at = (uint8_t *)A64_MEM + 0x83e6131cull;
        float was, want = e ? (float)atof(e) : 0.0f;
        memcpy(&was, at, 4);
        if (!e) {   /* (the Insert menu's Ultrawide, off unless it says 1: the main monitor's shape) */
            int on = 0;
            char path[MAX_PATH];
            const DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
            char *slash = len ? strrchr(path, SEP) : NULL;
            if (slash) {
                snprintf(slash + 1, (size_t)(path + MAX_PATH - slash - 1), "hookshot_options.ini");
                FILE *o = fopen(path, "r");
                if (o) {
                    char line[80];
                    while (fgets(line, sizeof line, o))
                        if (!strncmp(line, "ultrawide=", 10)) on = atoi(line + 10) != 0;
                    fclose(o);
                }
            }
            const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
            if (on && sh > 0 && (float)sw / (float)sh > 1.8f) want = (float)sw / (float)sh;
        }
        if (want > 1.0f && was > 1.777f && was < 1.778f) {
            memcpy(at, &want, 4);
            hos_aspect_ratio = want;
            hos_log("boot: aspect ratio %.4f in place of %.4f", want, was);
        } else if (want > 1.0f) {
            hos_log("boot: aspect: the constant reads %.4f here: left alone", was);
        }
    }
    hos_log("boot: image %zu MB at %#llx, %llu MB of memory for the game", n >> 20, IMAGE_BASE,
            (unsigned long long)(hos_total_memory >> 20));
    void hos_mods_init(const char *image_path);
    hos_mods_init(image);   /* mods.c: <exe folder>\mods (RomFS files) */

    Ctx *c = hos_boot_main_thread(hos_main_stack_size);
    uint64_t sp = c->x[31];
    void hos_prof_start(void);
    hos_prof_start();   /* HOS_PROFILE=<seconds> */
#ifdef _WIN32
    timeBeginPeriod(1);   /* 1 ms timer resolution: the 60 Hz vsync and the game's frame pacing sleep in short steps */
    no_power_throttling();
#endif
    void hos_display_start(void);
    hos_display_start();  /* the game's window; HOS_HEADLESS=1 for none */
    /* nn::init::Start's order (sdk+0xfa060), with rtld's three callbacks done here: f1 only marks rtld initialized,
       f2 is every module's DT_INIT, f3 the DT_FINIs (not needed: we never return from nnMain to unload). */
    call_guest(c, hos_entry_init_libc0, sp, "_init_libc0");
    c->x[0] = hos_thread_handle(hos_self());   /* the main thread's handle */
    c->x[1] = 0;                               /* program arguments: none (an unmapped address) */
    call_guest_args(c, hos_entry_nnosInitialize, sp, "nnosInitialize");
    void hos_thread_adopt_guest(HosThread *t);
    hos_thread_adopt_guest(hos_self());
    call_guest(c, hos_entry_init_libc1, sp, "_init_libc1");
    /* nninitInitializeSdkModule (sdk+0xfa000) only opens the SDK's service clients (sm:, diag, oe, aoc, err, fs,
       pctl) over IPC; the HLE stands in for every one of them, so it is not run. */
    call_guest(c, hos_entry_nninitStartup, sp, "nninitStartup");
    call_guest(c, hos_entry_init_libc2, sp, "_init_libc2");
    for (int i = 0; i < hos_module_count; i++) {
        char what[64];
        snprintf(what, sizeof what, "%s DT_INIT", hos_module_name[i]);
        call_guest(c, hos_module_init[i], sp, what);
    }
    call_guest(c, hos_entry_nnMain, sp, "nnMain");
    hos_log("nnMain returned");
    return 0;
}

#ifndef _WIN32
/* (linux) The window belongs on the process's main thread (SDL), so the game runs on a thread of its own, with the
   stack the guest main thread's lifted code needs (the main thread has the system's 8 MB): host/posix/display_sdl.c */
static int game_argc;
static char **game_argv;
static DWORD WINAPI game_thread(void *p) {
    (void)p;
    ExitProcess((UINT)game_main(game_argc, game_argv));
}
int main(int argc, char **argv) {
    int hos_display_init(void);
    void hos_display_loop(void);
    load_hookshot_env();
    game_argc = argc, game_argv = argv;
    const int window = hos_display_init();
    HANDLE t = CreateThread(NULL, 64u << 20, game_thread, NULL, 0, NULL);
    if (!t) {
        fprintf(stderr, "cannot start the game's thread\n");
        return 1;
    }
    SetThreadDescription(t, L"Game");
    if (window) hos_display_loop();   /* (does not return: the window's events until the process ends) */
    WaitForSingleObject(t, INFINITE);
    return 0;
}
#endif
