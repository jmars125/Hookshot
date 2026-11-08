/* HookshotBuilder-Setup.exe: the one file players download. It reads the latest release's hookshot-setup.txt, downloads
   the parts it names (the builder and its tools from this repository's release, llvm-mingw from its own official
   release), checks each one's SHA-256, unpacks them with Windows' own tar.exe into %LOCALAPPDATA%\HookshotBuilder\<tag>
   and starts the builder there. Parts already downloaded and checked are used again; a release already installed just
   starts. Files fetched by a program carry no "downloaded from the internet" mark, and the download itself holds no
   scripts, which is what antivirus programs' machine-learning guesses (Trojan:Script/...!ml) fired on.

   hookshot-setup.txt, one item a line:
       tag <tag>
       part <sha256> <bytes> <folder under the install> <top folder in the zip to rename, or -> <new name, or -> <url>
       run <the builder, relative to the install>
   Build: clang -O2 -mwindows setup.c setup_rc.o -lwinhttp -lbcrypt -lcomctl32 -lshell32 -lole32 -o HookshotBuilder-Setup.exe */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WM_STATUS (WM_APP + 1)
#define WM_DONE (WM_APP + 2)
#define MAX_PARTS 16

static HWND g_win, g_text, g_bar;
static wchar_t g_status[512];
static volatile LONG g_percent = -1;
static DWORD g_http_status;   /* (the last answer: 0 when the server was not reached) */
static wchar_t g_cmdline[2048];

typedef struct {
    char sha[65];
    unsigned long long size;
    char dest[260], from[260], to[260], url[1024];
} Part;

static void status(const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vswprintf(g_status, 512, fmt, ap);
    va_end(ap);
    PostMessageW(g_win, WM_STATUS, 0, 0);
}

static void fail(const wchar_t *fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(msg, 1024, fmt, ap);
    va_end(ap);
    MessageBoxW(g_win, msg, L"Hookshot Builder setup", MB_OK | MB_ICONERROR);
    PostMessageW(g_win, WM_DONE, 1, 0);
    ExitThread(1);
}

/* ---- HTTPS GET into a sink, following redirects (GitHub's release links go to its download host) ---- */
typedef int (*Sink)(void *user, const void *data, DWORD n);

static int http_get(const char *url8, Sink sink, void *user, unsigned long long expect) {
    wchar_t url[1024];
    MultiByteToWideChar(CP_UTF8, 0, url8, -1, url, 1024);
    URL_COMPONENTS uc = {sizeof uc};
    wchar_t host[256], path[1024];
    uc.lpszHostName = host, uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path, uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
    HINTERNET s = WinHttpOpen(L"HookshotBuilder-Setup/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!s) s = WinHttpOpen(L"HookshotBuilder-Setup/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!s) return 0;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : NULL;
    int ok = 0;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &len, NULL);
        g_http_status = code;
        if (code == 200) {
            static char buf[1 << 16];
            unsigned long long got = 0;
            ok = 1;
            for (;;) {
                DWORD n = 0;
                if (!WinHttpReadData(r, buf, sizeof buf, &n)) { ok = 0; break; }
                if (!n) break;
                if (!sink(user, buf, n)) { ok = 0; break; }
                got += n;
                if (expect) InterlockedExchange(&g_percent, (LONG)(got * 100 / expect)), PostMessageW(g_win, WM_STATUS, 0, 0);
            }
            if (expect && got != expect) ok = 0;
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

typedef struct { char *p; size_t n, cap; } Mem;
static int mem_sink(void *u, const void *d, DWORD n) {
    Mem *m = (Mem *)u;
    if (m->n + n + 1 > m->cap) {
        m->cap = (m->n + n + 1) * 2;
        m->p = (char *)realloc(m->p, m->cap);
        if (!m->p) return 0;
    }
    memcpy(m->p + m->n, d, n);
    m->n += n;
    m->p[m->n] = 0;
    return 1;
}

typedef struct { HANDLE f; BCRYPT_HASH_HANDLE h; } FileSink;
static int file_sink(void *u, const void *d, DWORD n) {
    FileSink *s = (FileSink *)u;
    DWORD w = 0;
    if (!WriteFile(s->f, d, n, &w, NULL) || w != n) return 0;
    return BCryptHashData(s->h, (PUCHAR)d, n, 0) == 0;
}

static void hex(const unsigned char *d, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* the SHA-256 of a file already on disk (a part downloaded before) */
static int file_sha256(const wchar_t *path, char out[65]) {
    BCRYPT_ALG_HANDLE a;
    BCRYPT_HASH_HANDLE h;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return 0;
    BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    int ok = f != INVALID_HANDLE_VALUE;
    static unsigned char buf[1 << 20];
    DWORD n;
    while (ok && ReadFile(f, buf, sizeof buf, &n, NULL) && n) BCryptHashData(h, buf, n, 0);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    unsigned char d[32];
    BCryptFinishHash(h, d, 32, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    if (ok) hex(d, 32, out);
    return ok;
}

static int run_wait(wchar_t *cmd, const wchar_t *dir) {
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESHOWWINDOW, si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return (int)code;
}

static void mkdirs(const wchar_t *path) {
    wchar_t p[MAX_PATH];
    wcsncpy(p, path, MAX_PATH - 1);
    p[MAX_PATH - 1] = 0;
    for (wchar_t *q = p + 3; *q; q++)
        if (*q == L'\\') *q = 0, CreateDirectoryW(p, NULL), *q = L'\\';
    CreateDirectoryW(p, NULL);
}

/* a folder and everything in it (a version folder, a half-unpacked one) */
static void rmtree(const wchar_t *dir) {
    wchar_t pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            swprintf(p, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) rmtree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p);   /* (a link: never what it points to) */
            else SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL), DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void read_small(const wchar_t *path, char *out, int n) {
    out[0] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD got = 0;
    ReadFile(f, out, (DWORD)n - 1, &got, NULL);
    CloseHandle(f);
    out[got] = 0;
    for (char *q = out; *q; q++)
        if (*q == '\r' || *q == '\n') { *q = 0; break; }
}

static void write_small(const wchar_t *path, const char *text) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(f, text, (DWORD)strlen(text), &n, NULL);
    CloseHandle(f);
}

/* (as NX2navi's launcher keeps them: the version just installed and the one before it stay, older versions go, and so do downloads
   the list no longer names; a part unchanged since the last release keeps its checksum and stays cached) */
static void prune(const wchar_t *versions, const wchar_t *downloads, const char *tag, const Part *parts, int nparts) {
    wchar_t cur[MAX_PATH], prev[MAX_PATH], pat[MAX_PATH], p[MAX_PATH];
    char was[128], before[128];
    swprintf(cur, MAX_PATH, L"%ls\\current.txt", versions);
    swprintf(prev, MAX_PATH, L"%ls\\previous.txt", versions);
    read_small(cur, was, sizeof was);
    if (strcmp(was, tag)) {
        if (was[0]) write_small(prev, was);
        write_small(cur, tag);
    }
    read_small(prev, before, sizeof before);
    WIN32_FIND_DATAW fd;
    swprintf(pat, MAX_PATH, L"%ls\\*", versions);
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            char name[260];
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
            if (!strcmp(name, tag) || (before[0] && !strcmp(name, before))) continue;
            swprintf(p, MAX_PATH, L"%ls\\%ls", versions, fd.cFileName);
            rmtree(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    swprintf(pat, MAX_PATH, L"%ls\\*.zip", downloads);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char name[260];
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
            int keep = 0;
            for (int i = 0; i < nparts; i++)
                if (!strncmp(name, parts[i].sha, 64)) keep = 1;
            if (!keep) swprintf(p, MAX_PATH, L"%ls\\%ls", downloads, fd.cFileName), DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static void w(const char *s8, wchar_t *out, int n) { MultiByteToWideChar(CP_UTF8, 0, s8, -1, out, n); }

/* (HOOKSHOT_SETUP_LIST=<url>: another list, for testing a release before it is the latest) */
static const char *manifest_url(void) {
    static char u[1024];
    const char *e = getenv("HOOKSHOT_SETUP_LIST");
    snprintf(u, sizeof u, "%s", e && *e ? e : "https://github.com/NotRexed/Hookshot/releases/latest/download/hookshot-setup.txt");
    return u;
}

static DWORD WINAPI work(LPVOID unused) {
    (void)unused;
    /* (--repair, from the builder's Repair button: every part checked again and the version unpacked anew, after the builder that
       started this has closed. It is not passed on) */
    int repair = 0;
    wchar_t *flag = wcsstr(g_cmdline, L"--repair");
    if (flag) {
        repair = 1;
        wmemmove(flag, flag + 8, wcslen(flag + 8) + 1);
        Sleep(2000);
    }
    wchar_t base[MAX_PATH], downloads[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base) != S_OK) fail(L"No local application data folder.");
    wcscat(base, L"\\HookshotBuilder");
    swprintf(downloads, MAX_PATH, L"%ls\\downloads", base);
    mkdirs(downloads);

    status(L"Looking for the latest Hookshot Builder...");
    Mem man = {0};
    g_http_status = 0;
    if (!http_get(manifest_url(), mem_sink, &man, 0) || !man.p)
        fail(g_http_status == 404
                 ? L"The latest Hookshot release has no setup files yet. Download HookshotBuilder.zip from the releases page for now: "
                   L"https://github.com/NotRexed/Hookshot/releases/latest"
             : g_http_status ? L"GitHub answered with an error. Try again in a few minutes."
                             : L"Could not reach GitHub to get the Hookshot Builder. Check your internet connection and try again.");

    char tag[128] = "", run[260] = "";
    Part parts[MAX_PARTS];
    int nparts = 0;
    for (char *line = strtok(man.p, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strncmp(line, "tag ", 4)) sscanf(line + 4, "%127s", tag);
        else if (!strncmp(line, "run ", 4)) sscanf(line + 4, "%259s", run);
        else if (!strncmp(line, "part ", 5) && nparts < MAX_PARTS) {
            Part *p = &parts[nparts];
            if (sscanf(line + 5, "%64s %llu %259s %259s %259s %1023s", p->sha, &p->size, p->dest, p->from, p->to, p->url) == 6) nparts++;
        }
    }
    if (!tag[0] || !run[0] || !nparts) fail(L"The release's setup list could not be read. Try again later.");

    wchar_t install[MAX_PATH], marker[MAX_PATH], wtag[128], wrun[260], exe[MAX_PATH], versions[MAX_PATH], final_dir[MAX_PATH];
    w(tag, wtag, 128), w(run, wrun, 260);
    swprintf(versions, MAX_PATH, L"%ls\\versions", base);
    swprintf(final_dir, MAX_PATH, L"%ls\\%ls", versions, wtag);
    swprintf(install, MAX_PATH, L"%ls\\%ls", versions, wtag);
    swprintf(marker, MAX_PATH, L"%ls\\installed.txt", install);
    swprintf(exe, MAX_PATH, L"%ls\\%ls", install, wrun);
    for (wchar_t *q = exe; *q; q++) if (*q == L'/') *q = L'\\';

    if (repair || GetFileAttributesW(marker) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        /* (unpacked into <tag>.part and renamed once complete: a setup stopped half way never leaves a version that looks installed) */
        swprintf(install, MAX_PATH, L"%ls\\%ls.part", versions, wtag);
        rmtree(install);
        rmtree(final_dir);
        mkdirs(install);
        for (int i = 0; i < nparts; i++) {
            Part *p = &parts[i];
            wchar_t zip[MAX_PATH], wsha[65];
            w(p->sha, wsha, 65);
            swprintf(zip, MAX_PATH, L"%ls\\%ls.zip", downloads, wsha);
            char have[65] = "";
            if (!(GetFileAttributesW(zip) != INVALID_FILE_ATTRIBUTES && file_sha256(zip, have) && !strcmp(have, p->sha))) {
                status(L"Downloading part %d of %d (%llu MB)...", i + 1, nparts, p->size >> 20);
                wchar_t part[MAX_PATH];
                swprintf(part, MAX_PATH, L"%ls.part", zip);
                FileSink fs;
                BCRYPT_ALG_HANDLE a;
                fs.f = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
                if (fs.f == INVALID_HANDLE_VALUE) fail(L"Could not write to %ls.", downloads);
                BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0);
                BCryptCreateHash(a, &fs.h, NULL, 0, NULL, 0, 0);
                const int got = http_get(p->url, file_sink, &fs, p->size);
                CloseHandle(fs.f);
                unsigned char d[32];
                BCryptFinishHash(fs.h, d, 32, 0);
                BCryptDestroyHash(fs.h);
                BCryptCloseAlgorithmProvider(a, 0);
                InterlockedExchange(&g_percent, -1);
                hex(d, 32, have);
                if (!got) DeleteFileW(part), fail(L"Downloading part %d of %d failed. Check your connection and free space, then try again.", i + 1, nparts);
                if (strcmp(have, p->sha)) DeleteFileW(part), fail(L"Part %d of %d did not download correctly (its checksum differs). Try again.", i + 1, nparts);
                DeleteFileW(zip);
                MoveFileW(part, zip);
            }
            status(L"Unpacking part %d of %d...", i + 1, nparts);
            wchar_t dest[MAX_PATH], wdest[260], tarcmd[2 * MAX_PATH + 64], sys[MAX_PATH];
            w(p->dest, wdest, 260);
            if (!strcmp(p->dest, ".")) wcscpy(dest, install);
            else swprintf(dest, MAX_PATH, L"%ls\\%ls", install, wdest);
            for (wchar_t *q = dest; *q; q++) if (*q == L'/') *q = L'\\';
            mkdirs(dest);
            GetSystemDirectoryW(sys, MAX_PATH);
            swprintf(tarcmd, 2 * MAX_PATH + 64, L"\"%ls\\tar.exe\" -xf \"%ls\" -C \"%ls\"", sys, zip, dest);
            if (run_wait(tarcmd, dest) != 0) fail(L"Unpacking part %d of %d failed (Windows' tar.exe). Is there enough free space on C:?", i + 1, nparts);
            if (strcmp(p->from, "-") && strcmp(p->to, "-")) {   /* (llvm-mingw-<version>-ucrt-x86_64 -> llvm-mingw) */
                wchar_t from[MAX_PATH], to[MAX_PATH], wf[260], wt[260];
                w(p->from, wf, 260), w(p->to, wt, 260);
                swprintf(from, MAX_PATH, L"%ls\\%ls", dest, wf);
                swprintf(to, MAX_PATH, L"%ls\\%ls", dest, wt);
                if (GetFileAttributesW(to) == INVALID_FILE_ATTRIBUTES && !MoveFileW(from, to))
                    fail(L"Could not set up part %d of %d (renaming %ls).", i + 1, nparts, wf);
            }
        }
        wchar_t staged_exe[MAX_PATH];
        swprintf(staged_exe, MAX_PATH, L"%ls\\%ls", install, wrun);
        for (wchar_t *q = staged_exe; *q; q++) if (*q == L'/') *q = L'\\';
        if (GetFileAttributesW(staged_exe) == INVALID_FILE_ATTRIBUTES)
            fail(L"The builder is missing after unpacking (%ls). Your antivirus may have removed it: allow the folder %ls and run the setup again.", staged_exe, base);
        if (!MoveFileW(install, final_dir)) fail(L"Could not finish installing into %ls.", final_dir);
        wcscpy(install, final_dir);
        write_small(marker, tag);
    }
    prune(versions, downloads, tag, parts, nparts);

    status(L"Starting the Hookshot Builder...");
    wchar_t cmd[4096], dir[MAX_PATH];
    wcscpy(dir, exe);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    swprintf(cmd, 4096, L"\"%ls\" %ls", exe, g_cmdline);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) fail(L"Could not start %ls.", exe);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    PostMessageW(g_win, WM_DONE, 0, 0);
    return 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_STATUS: {
        SetWindowTextW(g_text, g_status);
        const LONG p = g_percent;
        if (p < 0) SendMessageW(g_bar, PBM_SETMARQUEE, TRUE, 30);
        else SendMessageW(g_bar, PBM_SETMARQUEE, FALSE, 0), SendMessageW(g_bar, PBM_SETPOS, (WPARAM)p, 0);
        return 0;
    }
    case WM_DONE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    (void)prev;
    wcsncpy(g_cmdline, cmdline ? cmdline : L"", 2047);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc, wc.hInstance = inst, wc.lpszClassName = L"HookshotSetup";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    const int ww = 460, wh = 140;
    g_win = CreateWindowW(L"HookshotSetup", L"Hookshot Builder setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, NULL, NULL, inst, NULL);
    g_text = CreateWindowW(L"STATIC", L"Starting...", WS_CHILD | WS_VISIBLE, 16, 16, ww - 48, 22, g_win, NULL, inst, NULL);
    g_bar = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 16, 48, ww - 48, 20, g_win, NULL, inst, NULL);
    HFONT f = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(g_text, WM_SETFONT, (WPARAM)f, TRUE);
    SendMessageW(g_bar, PBM_SETMARQUEE, TRUE, 30);
    ShowWindow(g_win, show);
    CloseHandle(CreateThread(NULL, 0, work, NULL, 0, NULL));
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) TranslateMessage(&m), DispatchMessageW(&m);
    return 0;
}
