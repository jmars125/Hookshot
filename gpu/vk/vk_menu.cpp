// The Insert menu. Drawn on the CPU (GDI text into a bitmap) and copied over the window's image at the last step of a present,
// so it costs nothing while closed and it never touches the game's picture, nor what frame interpolation sees. Keys: Insert
// opens and closes it, Up and Down choose, Left and Right change, Esc closes; the game gets no keyboard input while it is open.
// (hookshot) A gamepad's Home button, or Select and Start together, opens it too; the D-pad chooses and changes, the bottom
// face button changes too, the right one closes. The game gets no gamepad input while it is open either.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "vk_menu.h"
#ifndef HOOKSHOT_VERSION   // (nx2/build.py gives it to this file: nx2/version.py)
#define HOOKSHOT_VERSION "unknown"
#endif

namespace mkwvk {
HWND app_window();

Options opts;

namespace {

struct Row {
    const char* name;
    int* value;
    int lo, hi, step;
    const char* (*text)(int v);
    const char* help;   // (shown under the list while the row is selected)
};
const char* tx_framegen(int v) {
    static const char* t[] = {"Off", "", "2x", "3x", "4x", "5x", "6x", "7x", "8x"};
    return t[v];
}
const char* tx_onoff(int v) { return v ? "On" : "Off"; }
const char* tx_upscale(int v) {
    static const char* t[] = {"Off", "Catmull-Rom", "Lanczos"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];   // (a setting past the row: the label, not the next bytes)
    return t[v];
}
const char* tx_percent(int v) {
    static char b[8];
    snprintf(b, sizeof b, "%d%%", v);
    return b;
}
const char* tx_resolution(int v) {
    static const char* t[] = {"Auto", "100%", "75%", "67%", "50%"};
    return t[v];
}
const char* tx_speed(int v) { return v ? "Real time" : "Per frame"; }
const char* tx_handheld(int v) { return v ? "On (restart)" : "Off (restart)"; }
const char* tx_ultrawide(int v) { return v ? "Monitor's shape (restart)" : "16:9 (restart)"; }

Row rows[] = {
    {"Fullscreen", &opts.fullscreen, 0, 1, 1, tx_onoff,
     "Fills the whole screen with the game, without the window's border. The same as F11."},
    {"Frame generation", &opts.framegen, 0, 8, 1, tx_framegen,
     "Makes extra frames between the game's own (2x up to 8x), for smoother motion. Adds a little input delay, and fast motion or camera cuts "
     "can show artifacts."},
    {"Frame pacing", &opts.pace, 0, 1, 1, tx_onoff,
     "Shows each frame at an even interval instead of as soon as it is ready. Less stutter, at up to one frame of delay."},
    {"Resolution", &opts.resolution, 0, 4, 1, tx_resolution,
     "How sharp the 3D is drawn. Auto lets the game lower it when the GPU is busy. A fixed percentage stays steady: lower is "
     "faster."},
    {"Upscaling", &opts.upscale, 0, 2, 1, tx_upscale,
     "The filter that scales the picture to your window. Off is the plain copy. Catmull-Rom is sharper, Lanczos sharper still."},
    {"Sharpness", &opts.sharpness, 0, 100, 10, tx_percent,
     "Sharpens the picture as it goes to the window, with or without an upscaling filter. Too much shows halos around edges."},
    {"Game speed", &opts.realtime, 0, 1, 1, tx_speed,
     "Per frame: the game slows down whenever it runs below 60 fps. Real time: it keeps normal speed down to 15 fps by taking "
     "longer steps."},
    {"Handheld profile", &opts.handheld, 0, 1, 1, tx_handheld,
     "Runs the game as if the console were undocked: lower resolutions, lighter on the GPU. Applies after a restart."},
    {"Ultrawide", &opts.ultrawide, 0, 1, 1, tx_ultrawide,
     "Monitor's shape widens the camera to fill a screen wider than 16:9. The HUD is still stretched. Applies after a restart."},
    {"FPS counter", &opts.show_fps, 0, 1, 1, tx_onoff,
     "Shows frames per second in the top right corner. With frame generation on, it also shows the game's own rate."},
};
constexpr int kRows = sizeof rows / sizeof rows[0];
constexpr int kMenuWidth = 360;   // (at a scale of 1: 720 lines)

std::atomic<bool> open_now{false};
int selected = 0;
bool menu_refresh = false;
std::string ini_path() {
    char exe[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + "hookshot_options.ini";
}
const char* ini_key(const Row& r) {
    static char k[32];
    size_t n = 0;
    for (const char* p = r.name; *p && n + 1 < sizeof k; p++) k[n++] = *p == ' ' ? '_' : (char)tolower(*p);
    k[n] = 0;
    return k;
}
void save() {
    // (hookshot) the lines that are not the menu's are kept: the controls, pad_ and key_ (host/nn_hid.c)
    std::string kept;
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "pad_", 4) || !strncmp(line, "key_", 4)) kept += line;
        fclose(f);
    }
    if (FILE* f = fopen(ini_path().c_str(), "w")) {
        for (const Row& r : rows) fprintf(f, "%s=%d\n", ini_key(r), *r.value);
        fprintf(f, "options_version=2\n");   // (game_speed below 2 was only ever the old default, see menu_init)
        fputs(kept.c_str(), f);
        fclose(f);
    }
}

// ---- what is drawn: bitmaps of the menu and the counter, in the window's byte order ----
struct Bitmap {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;
    uint64_t version = 0;
    bool keyed = false;                   // (black is see-through: copied as the runs of pixels in between)
    std::vector<VkBufferImageCopy> runs;
};
Bitmap menu_bmp, fps_bmp;
std::mutex bmp_mu;
int scale_of(uint32_t h) { return std::max(1u, h / 720); }

void gdi_draw(Bitmap& out, uint32_t w, uint32_t h, void (*paint)(HDC, uint32_t, uint32_t, int), int scale) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(dc); return; }
    HGDIOBJ old = SelectObject(dc, bmp);
    paint(dc, w, h, scale);
    GdiFlush();
    const bool rgba = dev.swap_format == VK_FORMAT_R8G8B8A8_UNORM || dev.swap_format == VK_FORMAT_R8G8B8A8_SRGB;
    std::lock_guard lk(bmp_mu);
    out.w = w, out.h = h;
    out.px.assign((const uint8_t*)bits, (const uint8_t*)bits + (size_t)w * h * 4);
    if (out.keyed) {
        out.runs.clear();
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w;) {
                const uint8_t* p = out.px.data() + ((size_t)y * w + x) * 4;
                if (!(p[0] | p[1] | p[2])) { x++; continue; }
                uint32_t e = x + 1;
                for (; e < w; e++) {
                    const uint8_t* q = out.px.data() + ((size_t)y * w + e) * 4;
                    if (!(q[0] | q[1] | q[2])) break;
                }
                VkBufferImageCopy c{};
                c.bufferOffset = ((uint64_t)y * w + x) * 4;
                c.bufferRowLength = w;
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.imageOffset = {(int32_t)x, (int32_t)y, 0};
                c.imageExtent = {e - x, 1, 1};
                out.runs.push_back(c);
                x = e;
            }
        }
    }
    for (size_t i = 0; i < out.px.size(); i += 4) {
        if (rgba) std::swap(out.px[i], out.px[i + 2]);
        out.px[i + 3] = 255;
    }
    out.version++;
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}
HFONT font(int px, int weight) {
    return CreateFontA(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, "Segoe UI");
}
int help_px = 0;   // (the help text's height: measure_help)
/// The help text word-wrapped to width, a TextOutA a line (the POSIX GDI stand-in has no DrawTextA), drawn at (x, y)
/// when draw: its height in pixels either way
int wrap_help(HDC dc, const char* text, int width, int x, int y, bool draw) {
    SIZE sz{};
    GetTextExtentPoint32A(dc, "Ag", 2, &sz);
    const int line_h = (int)sz.cy;
    int lines = 0;
    for (const char* p = text; *p;) {
        int n = 0;   // (the most whole words that fit; one word at least, however long)
        for (int end = 0;; end++) {
            while (p[end] && p[end] != ' ') end++;
            GetTextExtentPoint32A(dc, p, end, &sz);
            if (n && sz.cx > width) break;
            n = end;
            if (!p[end]) break;
        }
        if (draw) TextOutA(dc, x, y + lines * line_h, p, n);
        lines++;
        p += n;
        while (*p == ' ') p++;
    }
    return lines * line_h;
}
void fill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r{x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
void paint_menu(HDC dc, uint32_t w, uint32_t h, int s) {
    fill(dc, 0, 0, (int)w, (int)h, RGB(16, 20, 18));
    fill(dc, 0, 0, (int)w, 2 * s, RGB(96, 176, 112));
    SetBkMode(dc, TRANSPARENT);
    HFONT title = font(20 * s, FW_SEMIBOLD), body = font(16 * s, FW_NORMAL);
    HGDIOBJ old = SelectObject(dc, title);
    SetTextColor(dc, RGB(236, 240, 236));
    TextOutA(dc, 16 * s, 12 * s, "Hookshot", 8);
    {   // (which build, beside the title, so a report can say it without opening the log; the release alone when the whole
        // line would run into the title)
        SIZE tsz{};
        GetTextExtentPoint32A(dc, "Hookshot", 8, &tsz);
        HFONT small = font(12 * s, FW_NORMAL);
        SelectObject(dc, small);
        SetTextColor(dc, RGB(140, 150, 144));
        std::string ver = HOOKSHOT_VERSION;
        const int room = (int)w - 32 * s - tsz.cx - 12 * s;
        SIZE sz{};
        GetTextExtentPoint32A(dc, ver.c_str(), (int)ver.size(), &sz);
        if (sz.cx > room && ver.find(" (") != std::string::npos) {
            ver.resize(ver.find(" ("));
            GetTextExtentPoint32A(dc, ver.c_str(), (int)ver.size(), &sz);
        }
        if (sz.cx <= room) TextOutA(dc, (int)w - 16 * s - sz.cx, 18 * s, ver.c_str(), (int)ver.size());
        SelectObject(dc, title);
        DeleteObject(small);
    }
    SelectObject(dc, body);
    const int rh = 28 * s, top = 44 * s;
    for (int i = 0; i < kRows; i++) {
        const int y = top + i * rh;
        if (i == selected) fill(dc, 8 * s, y, (int)w - 16 * s, rh, RGB(44, 72, 54));
        SetTextColor(dc, i == selected ? RGB(255, 255, 255) : RGB(196, 204, 198));
        TextOutA(dc, 16 * s, y + 4 * s, rows[i].name, (int)strlen(rows[i].name));
        char v[40];
        snprintf(v, sizeof v, i == selected ? "<  %s  >" : "%s", rows[i].text(*rows[i].value));
        SIZE sz{};
        GetTextExtentPoint32A(dc, v, (int)strlen(v), &sz);
        TextOutA(dc, (int)w - 16 * s - sz.cx, y + 4 * s, v, (int)strlen(v));
    }
    const int help_top = top + kRows * rh + 8 * s;
    fill(dc, 16 * s, help_top - 4 * s, (int)w - 32 * s, std::max(1, s), RGB(52, 64, 56));
    HFONT help = font(14 * s, FW_NORMAL);
    SelectObject(dc, help);
    SetTextColor(dc, RGB(176, 186, 180));
    wrap_help(dc, rows[selected].help, (int)w - 32 * s, 16 * s, help_top + 4 * s, true);
    SelectObject(dc, body);
    SetTextColor(dc, RGB(140, 150, 144));
    const char* hint = "Arrows change   Insert, F1 or Home closes";
    TextOutA(dc, 16 * s, help_top + 4 * s + help_px + 10 * s, hint, (int)strlen(hint));
    SelectObject(dc, old);
    DeleteObject(title);
    DeleteObject(body);
    DeleteObject(help);
}
char fps_text[48] = "";
char fps_sub[32] = "";
/// The usual overlay: a bold green number with a dark edge, nothing behind it. Hard-edged letters: see-through pixels are copied or not.
void outlined(HDC dc, int x, int y, const char* text, COLORREF colour, int edge) {
    SetTextColor(dc, RGB(6, 8, 6));
    const int n = (int)strlen(text);
    for (int dy = -edge; dy <= edge; dy++)
        for (int dx = -edge; dx <= edge; dx++)
            if (dx || dy) TextOutA(dc, x + dx, y + dy, text, n);
    SetTextColor(dc, colour);
    TextOutA(dc, x, y, text, n);
}
HFONT hard_font(int px, int weight) {
    return CreateFontA(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                       DEFAULT_PITCH, "Segoe UI");
}
void paint_fps(HDC dc, uint32_t w, uint32_t h, int s) {
    fill(dc, 0, 0, (int)w, (int)h, RGB(0, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_RIGHT | TA_TOP);
    const COLORREF green = RGB(72, 255, 110);
    const int right = (int)w - 2 * s;
    HFONT label = hard_font(13 * s, FW_BOLD), big = hard_font(28 * s, FW_BOLD);
    HGDIOBJ old = SelectObject(dc, label);
    outlined(dc, right, 14 * s, "FPS", green, s);
    SelectObject(dc, big);
    outlined(dc, right - 30 * s, 0, fps_text, green, s);
    if (fps_sub[0]) {
        SelectObject(dc, label);
        outlined(dc, right, 32 * s, fps_sub, RGB(226, 232, 226), s);
    }
    SelectObject(dc, old);
    DeleteObject(label);
    DeleteObject(big);
}
/// The tallest help text at scale s, in pixels: the menu keeps that height whichever row is selected
int measure_help(int s) {
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT f = font(14 * s, FW_NORMAL);
    HGDIOBJ old = SelectObject(dc, f);
    int most = 0;
    for (const Row& r : rows) most = std::max(most, wrap_help(dc, r.help, (kMenuWidth - 32) * s, 0, 0, false));
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    return most;
}
void redraw_menu(int s) {
    static int measured_at = 0;
    if (measured_at != s) help_px = measure_help(s), measured_at = s;
    // (title and rows, the rule and the help text, the hint line)
    gdi_draw(menu_bmp, (uint32_t)(kMenuWidth * s), (uint32_t)((44 + kRows * 28 + 12 + 10 + 30) * s + help_px), paint_menu, s);
}

std::atomic<int64_t> game_frames{0}, shown_frames{0};
int cur_scale = 1;

struct Upload {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    uint64_t size = 0, version = 0;
};
Upload ups[2][3];   // (the menu's, the counter's; a frame in flight's each)

void copy_over(VkCommandBuffer cmd, VkImage dst, Bitmap& b, Upload& u, int x, int y, uint32_t w, uint32_t h) {
    std::lock_guard lk(bmp_mu);
    if (!b.w || (uint32_t)x + b.w > w || (uint32_t)y + b.h > h) return;
    const uint64_t size = (uint64_t)b.w * b.h * 4;
    if (u.size < size) {
        if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, u.buf, u.alloc, &u.map)) return;
        u.size = size, u.version = 0;
    }
    if (u.version != b.version) std::memcpy(u.map, b.px.data(), size), u.version = b.version;
    if (b.keyed) {
        static thread_local std::vector<VkBufferImageCopy> at;
        at = b.runs;
        for (auto& r : at) r.imageOffset.x += x, r.imageOffset.y += y;
        if (!at.empty()) vkCmdCopyBufferToImage(cmd, u.buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)at.size(), at.data());
        return;
    }
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {x, y, 0};
    c.imageExtent = {b.w, b.h, 1};
    vkCmdCopyBufferToImage(cmd, u.buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

} // namespace

void menu_init() {
    static bool done;
    if (done) return;
    done = true;
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        char line[80];
        bool has_resolution = false;
        bool v2 = false;
        int old_dynres = -1;   // (the row before Resolution: 0, the full scale, is its 100%; 1 is Auto)
        while (fgets(line, sizeof line, f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            for (Row& r : rows)
                if (!strcmp(line, ini_key(r))) *r.value = std::min(std::max(atoi(eq + 1), r.lo), r.hi);
            has_resolution |= !strcmp(line, "resolution");
            v2 |= !strcmp(line, "options_version");
            if (!strcmp(line, "dynamic_resolution")) old_dynres = atoi(eq + 1);
        }
        fclose(f);
        if (!has_resolution && old_dynres >= 0) opts.resolution = old_dynres ? 0 : 1;
        // (Game speed is Real time by default now: Per frame played at half speed at 30 fps, the "slow motion" players
        // reported. An options file from before kept the old default whether chosen or not: Real time for it too)
        if (!v2) opts.realtime = 1;
    }
    if (opts.framegen == 1) opts.framegen = 2;
    if (const char* e = getenv("RX_VK_FRAMEGEN_MULT")) opts.framegen = std::min(std::max(atoi(e), 2), 8);
    if (const char* e = getenv("RX_VK_PACE")) opts.pace = atoi(e) != 0;
    if (const char* e = getenv("RX_VK_UPSCALE"))
        for (Row& r : rows)
            if (r.value == &opts.upscale) *r.value = std::min(std::max(atoi(e), r.lo), r.hi);
    if (const char* e = getenv("HOS_REALTIME_STEP")) opts.realtime = atoi(e) != 0;
    if (const char* e = getenv("HOS_DYNRES")) opts.resolution = atoi(e) != 0 ? 0 : 1;
}

bool menu_is_open() { return open_now.load(std::memory_order_relaxed); }

void menu_note_frame(bool game_frame) {
    shown_frames.fetch_add(1, std::memory_order_relaxed);
    if (game_frame) game_frames.fetch_add(1, std::memory_order_relaxed);
}

// The Fullscreen row and F11 are one setting: the row asks the window's thread (host/display.c), F11 shows in the row
void fullscreen_tick() {
    static int applied = 0, settle = 0;
    const HWND main_window = app_window();
    if (!main_window) return;
    static const bool env = getenv("HOS_FULLSCREEN") != nullptr;   // (a test's own choice: the row is left alone)
    if (env) return;
    if (opts.fullscreen != applied) {
        applied = opts.fullscreen;
        settle = 60;
        PostMessageA(main_window, WM_APP + 7, (WPARAM)applied, 0);
        return;
    }
    if (settle > 0) { settle--; return; }
    const int now = !(GetWindowLongA(main_window, GWL_STYLE) & WS_CAPTION);
    if (now != applied) {
        opts.fullscreen = applied = now;
        menu_refresh = true;
        save();
    }
}

/// A rate over the half-second that just ended. The first call only starts that interval: the frames since launch,
/// divided by one second, were shown as the rate the moment the counter was turned on.
bool fps_take(int64_t now, int64_t shown, int64_t game, int64_t* next, int64_t* last_shown, int64_t* last_game,
              double* shown_rate, double* game_rate) {
    if (now < *next) return false;
    const bool have = *next != 0;
    if (have) {
        const double secs = (now - *next + 500000000) / 1e9;
        *shown_rate = (shown - *last_shown) / secs;
        *game_rate = (game - *last_game) / secs;
    }
    *last_shown = shown;
    *last_game = game;
    *next = now + 500000000;
    return have;
}

void menu_tick() {
    fullscreen_tick();
    enum { K_N = 8 };
    static bool held[K_N];
    static const int keys[K_N] = {VK_INSERT, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_ESCAPE, VK_F1,   // (F1: for keyboards without Insert)
                                  VK_RETURN};
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    const bool mine = pid == GetCurrentProcessId();
    bool press[K_N] = {};
    for (int i = 0; i < K_N; i++) {
        const bool down = mine && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        press[i] = down && !held[i];
        held[i] = down;
    }
    // (hookshot) the gamepads, by position: Home, or Select and Start together, as Insert; the D-pad as the arrows,
    // the bottom face button as Enter, the right one as Esc
    static uint32_t pad_held = 0;
    const uint32_t raw = mine && host && host->pad_raw ? host->pad_raw() : 0;
    const uint32_t pad_press = raw & ~pad_held, chord = 1u << HOS_PAD_BACK | 1u << HOS_PAD_START;
    const bool chord_now = (raw & chord) == chord && (pad_held & chord) != chord;
    pad_held = raw;
    auto pad = [&](int p) { return (pad_press >> p & 1) != 0; };
    if (pad(HOS_PAD_GUIDE) || chord_now) press[0] = true;
    if (open_now.load()) {
        press[1] |= pad(HOS_PAD_UP), press[2] |= pad(HOS_PAD_DOWN), press[3] |= pad(HOS_PAD_LEFT);
        press[4] |= pad(HOS_PAD_RIGHT), press[5] |= pad(HOS_PAD_EAST), press[7] |= pad(HOS_PAD_SOUTH);
    }
    const int scale = scale_of(dev.swap_extent.height);
    bool changed = scale != cur_scale || menu_refresh;
    menu_refresh = false;
    cur_scale = scale;
    if (press[0] || press[6]) open_now = !open_now.load(), changed = true;
    if (open_now.load()) {
        if (press[5]) open_now = false;
        if (press[1]) selected = (selected + kRows - 1) % kRows, changed = true;
        if (press[2]) selected = (selected + 1) % kRows, changed = true;
        if (press[3] || press[4] || press[7]) {   // (Enter, or the bottom face button, as Right)
            Row& r = rows[selected];
            if (press[7]) press[4] = true;
            int v = *r.value + (press[4] ? r.step : -r.step);
            if (r.value == &opts.framegen && v == 1) v = press[4] ? 2 : 0;   // (off, 2x ... 8x)
            *r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;
            changed = true;
            save();
        }
        if (changed) redraw_menu(scale);
    }
    if (opts.show_fps) {
        static int64_t next = 0, last_game = 0, last_shown = 0;
        const int64_t now = prof_now();
        double shown = 0, game = 0;
        if (fps_take(now, shown_frames.load(), game_frames.load(), &next, &last_shown, &last_game, &shown, &game)) {
            snprintf(fps_text, sizeof fps_text, "%.0f", shown);
            if (opts.framegen >= 2) snprintf(fps_sub, sizeof fps_sub, "game %.0f", game);
            else fps_sub[0] = 0;
            fps_bmp.keyed = true;
            gdi_draw(fps_bmp, 120u * scale, 54u * scale, paint_fps, scale);
        }
    }
}

void menu_record(VkCommandBuffer cmd, VkImage dst, int slot, uint32_t w, uint32_t h) {
    if (opts.show_fps) {
        // The game image was just blitted (or cleared) into dst. Its write must
        // finish before the FPS pixels overwrite it; command order alone does
        // not order these transfer writes. The menu below has its own barrier.
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
        std::unique_lock lk(bmp_mu);
        const uint32_t fw = fps_bmp.w;
        lk.unlock();
        if (fw) copy_over(cmd, dst, fps_bmp, ups[1][slot], (int)w - (int)fw - (int)(12 * cur_scale), (int)(10 * cur_scale), w, h);
    }
    if (open_now.load(std::memory_order_relaxed)) {
        std::unique_lock lk(bmp_mu);
        const uint32_t mw = menu_bmp.w, mh = menu_bmp.h;
        lk.unlock();
        // (one copy finished before the next: on a Radeon the menu came out with its top torn away beside the FPS counter)
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
        if (mw) copy_over(cmd, dst, menu_bmp, ups[0][slot], (int)(w - mw) / 2, (int)(h - mh) / 2, w, h);
    }
}

} // namespace mkwvk
