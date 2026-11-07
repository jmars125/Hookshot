/* nn::hid: one Pro Controller (FullKey style) on Npad No.1, from every XInput gamepad and the keyboard and mouse at
   once. Buttons map by position (the Xbox bottom face button is the Switch's bottom one, B).

   (hookshot) What presses each button, on a gamepad and on the keyboard, can be changed in hookshot_options.ini
   beside the game (binds_load). The defaults:
   Keyboard (totk-pc-recomp's layout): WASD left stick, arrows or IJKL right stick (camera), Space X (jump), E A
   (action), Shift B (run/cancel), Q L (abilities), R R, Z ZL, X or F ZR (bow), Ctrl left-stick click (crouch), C
   right-stick click (first person), Tab or Enter Plus, M Minus (map), 1 2 3 4 d-pad left, up, right, down. Only
   while the game's window is in front (the key state is global).
   Mouse (after a click in the window, display.c; Esc releases): its motion is the right stick (mouse panning,
   mouse_stick), left button Y (attack), right button ZL (shield, lock on). - and = change its sensitivity (x0.8 /
   x1.25, shown in the title bar, kept in <user>/mouse_sens.txt); HOS_MOUSE_SENS=<x> sets it, HOS_MOUSE_INVERT_Y=1
   flips up/down.

   HOS_INPUT_SCRIPT (test runs): timed presses added to the input, "<seconds>:<button>[+<button>...][/<ms held>],..."
   with buttons A B X Y L R ZL ZR PLUS MINUS LS RS LEFT UP RIGHT DOWN and the stick directions LUP LDOWN LLEFT
   LRIGHT RUP RDOWN RLEFT RRIGHT; held 200 ms unless given. Example: "50:A,52:A,80:LUP/4000".

   Npad states share one layout (NpadCommonState): { s64 sampling; u64 buttons; s32 lx, ly; s32 rx, ry; u32 attributes;
   pad } = 0x28 bytes. */
#include "hos.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include "../gpu/vk/vk_api.h"   /* (the controls' names: HOS_SW_*, HOS_PAD_*) */

enum { B_A = 0, B_B, B_X, B_Y, B_LS, B_RS, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN };
#define STATE_SIZE 0x28
#define NPAD_NO1 0
#define STYLE_FULLKEY 1u
#define ATTR_CONNECTED 1u

typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE *);
static XInputGetStateFn xinput_get;
static int64_t sampling;

int rx_option(int id, int otherwise);
/* (the Insert menu takes the keyboard while it is open) */
static int key(int vk) { return !rx_option(1, 0) && (GetAsyncKeyState(vk) & 0x8000) != 0; }

int hos_mouse_take(int32_t *dx, int32_t *dy);   /* display.c */

/* The captured mouse's sensitivity (mouse_stick below): - and = change it, kept in <user>/mouse_sens.txt */
double hos_mouse_sens = 0;   /* (display.c shows it in the title) */
extern const char *hos_user_dir;

static void mouse_sens_save(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/mouse_sens.txt", hos_user_dir ? hos_user_dir : "user");
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%.3f\n", hos_mouse_sens); fclose(f); }
}

static void mouse_sens_keys(void) {
    static int prev_minus, prev_plus;
    const int minus = key(VK_OEM_MINUS) || key(VK_SUBTRACT), plus = key(VK_OEM_PLUS) || key(VK_ADD);
    if (minus && !prev_minus) { hos_mouse_sens *= 0.8; mouse_sens_save(); }
    if (plus && !prev_plus) { hos_mouse_sens *= 1.25; mouse_sens_save(); }
    if (hos_mouse_sens < 0.05) hos_mouse_sens = 0.05;
    if (hos_mouse_sens > 20) hos_mouse_sens = 20;
    prev_minus = minus;
    prev_plus = plus;
}

/* The mouse as the right stick, "mouse panning" as MacNativeGames does it (games/totk/runtime/mouse_look.h,
   github.com/ly05010419/MacNativeGames, MIT; the idea is yuzu's): every movement adds to a held deflection, and a
   tick every 10 ms shows it to the game and then lets it decay. The camera keeps turning while the mouse moves and
   settles smoothly when it stops, whatever the mouse's report rate, the game's frame rate or how often it reads the
   pad (the old speed-per-8-ms stick jumped between reads). The decay grows with the deflection (1 - length^2 *
   decay), at least min_decay a tick; the held length is cut to 1.5 (past the stick's edge, so a steady turn is easy
   to hold) and shown at most 1. A movement that leaves less than 20% is lifted to 20%, past the game's dead zone.
   MacNativeGames' decay (18%, at least 6%) suits a trackpad: with a mouse the camera drifted on for 0.4 s after it
   stopped. Here 30%, at least 20% (HOS_MOUSE_DECAY / HOS_MOUSE_MIN_DECAY, percent): it stops within ~0.1 s, and one
   raw count moves the stick by sensitivity * 0.03, so at 1 the deflection follows 100-700 counts/s (0.2 to ~0.9)
   and is full from ~1000. */
enum { MOUSE_TICK_NS = 10000000 };
static double look_x, look_y;   /* (the held deflection, stick units, y up) */

static double mouse_percent(const char *name, double fallback) {
    const char *e = getenv(name);
    return e && atof(e) >= 0 && atof(e) <= 100 ? atof(e) / 100 : fallback;
}

static void mouse_look_move(double dx, double dy) {
    const double k = hos_mouse_sens * 0.03;
    look_x += dx * k;
    look_y += dy * k;
    const double length = sqrt(look_x * look_x + look_y * look_y), least = 0.2;
    if (length > 0 && length < least) look_x *= least / length, look_y *= least / length;
}

static void mouse_look_tick(void) {
    static double decay_rate = -1, min_decay;
    if (decay_rate < 0) {
        decay_rate = mouse_percent("HOS_MOUSE_DECAY", 0.30);
        min_decay = mouse_percent("HOS_MOUSE_MIN_DECAY", 0.20);
    }
    double length = sqrt(look_x * look_x + look_y * look_y);
    if (length > 1.5) look_x *= 1.5 / length, look_y *= 1.5 / length, length = 1.5;
    const double c = length < 1 ? length : 1, decay = 1 - c * c * decay_rate, most = 1 - min_decay;
    const double keep = decay < most ? (decay > 0 ? decay : 0) : most;
    look_x *= keep;
    look_y *= keep;
    if (fabs(look_x) < 1e-6 && fabs(look_y) < 1e-6) look_x = look_y = 0;
}

/* While the ability wheel is open (L held, Q on the keyboard), the mouse points instead of panning (MacNativeGames
   2d0f0f0, MIT): the wheel picks the ability the right stick points at, and with panning the stick settled back to
   the centre as soon as the mouse stopped, taking the pick with it. The stick starts at the centre, stays where the
   mouse puts it (cut to length 1) and is centred again when L is let go, so the camera does not turn by itself after
   the wheel closes. */
static int mouse_stick(int32_t *rx, int32_t *ry, int pointing) {
    static int64_t next_tick;
    static int32_t shown[2];
    static int captured, was_pointing;
    static int invert = -1;
    if (invert < 0) {
        invert = getenv("HOS_MOUSE_INVERT_Y") != NULL;
        hos_mouse_sens = 1.0;
        char path[512];
        snprintf(path, sizeof path, "%s/mouse_sens.txt", hos_user_dir ? hos_user_dir : "user");
        FILE *f = fopen(path, "r");
        if (f) {
            double v;
            if (fscanf(f, "%lf", &v) == 1 && v > 0) hos_mouse_sens = v;
            fclose(f);
        }
        const char *e = getenv("HOS_MOUSE_SENS");
        if (e && atof(e) > 0) hos_mouse_sens = atof(e);
    }
    mouse_sens_keys();
    const int64_t now = hos_now_ns();
    if (now >= next_tick) {
        int32_t dx, dy;
        captured = hos_mouse_take(&dx, &dy);
        if (pointing != was_pointing) look_x = look_y = 0, was_pointing = pointing;
        if (!captured) look_x = look_y = 0;
        else if (pointing) {
            const double k = hos_mouse_sens * 0.03;
            look_x += dx * k;
            look_y += (invert ? dy : -dy) * k;
            const double length = sqrt(look_x * look_x + look_y * look_y);
            if (length > 1) look_x /= length, look_y /= length;
        } else if (dx || dy) mouse_look_move(dx, invert ? dy : -dy);
        /* (the ticks since the last read: the first shows the deflection, the rest only decay it; at most 10, a
           read after a pause starts afresh) */
        const int64_t ticks = next_tick ? (now - next_tick) / MOUSE_TICK_NS + 1 : 1;
        double ox = look_x, oy = look_y;
        const double shown_length = sqrt(ox * ox + oy * oy);
        if (shown_length > 1) ox /= shown_length, oy /= shown_length;
        shown[0] = (int32_t)lround(ox * 32767);
        shown[1] = (int32_t)lround(oy * 32767);
        if (!pointing)
            for (int64_t i = 0; i < (ticks < 10 ? ticks : 10); i++) mouse_look_tick();
        next_tick = (next_tick && ticks <= 10 ? next_tick + ticks * MOUSE_TICK_NS : now + MOUSE_TICK_NS);
    }
    if (!captured) return 0;
    *rx = shown[0];
    *ry = shown[1];
    return 1;
}

static int game_in_front(void) {
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &pid);
    int front = pid == GetCurrentProcessId();
#ifndef __EMSCRIPTEN__
    static int last_normal_was_us = 1, was_front = 1;
    if (w && !front) {
        const LONG_PTR ex = GetWindowLongPtrA(w, GWL_EXSTYLE);
        const int overlay = (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0;
        if (overlay && last_normal_was_us) front = 1;
        else last_normal_was_us = 0;
    } else if (front) {
        last_normal_was_us = 1;
    }
    if (front != was_front) {
        char cls[64] = "";
        if (w) GetClassNameA(w, cls, sizeof cls);
        hos_log("hid: input %s (foreground window: process %lu, class \"%s\")", front ? "back" : "paused: the game "
                "window is not in front", (unsigned long)pid, cls);
        was_front = front;
    }
#endif
    return front;
}

/* HOS_INPUT_SCRIPT, and HOS_INPUT_LIVE=<file>: lines appended to that file while the game runs are presses in the
   script's syntax, their times counted from when the line is read ("0:LUP/2000,2:A"): a test can steer the game
   by looking at its screenshots. */
typedef struct { int64_t start_ms, end_ms; uint64_t buttons; int32_t stick[4]; } Press;
#define MAX_PRESSES 1024
static Press presses[MAX_PRESSES];
static int npresses = -1;
static int64_t script_t0, script_end_ms;

static int parse_presses(const char *s, int64_t base_ms, int live);

static void parse_script(void) {
    {   /* start SDL's controller thread now: pads are open by the time the game window comes to the front */
        int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]);
        uint64_t b = 0;
        int32_t st[4] = {0};
        hos_sdl_pads(&b, st);
    }
    npresses = 0;
    script_t0 = hos_now_ns();
    parse_presses(getenv("HOS_INPUT_SCRIPT"), 0, 0);
    if (npresses)
        hos_log("hid: input script of %d presses (last at %.0f s; the player's devices count too)", npresses,
                script_end_ms / 1000.0);
}

static void live_input(void) {
    static const char *path;
    static int64_t next_check, offset;
    static int looked;
    if (!looked) { looked = 1; path = getenv("HOS_INPUT_LIVE"); }
    if (!path) return;
    const int64_t now = hos_now_ns();
    if (now < next_check) return;
    next_check = now + 30000000;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, (long)offset, SEEK_SET) == 0) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            const size_t n = strlen(line);
            if (!n || line[n - 1] != '\n') break;   /* a line still being written: read it next time */
            offset += (int64_t)n;
            const int64_t at = (now - script_t0) / 1000000;
            if (line[0] == '!') {   /* commands: !dump = a frame dump of the next frame (as RX_DUMP_AT) */
                void rx_request_frame_dump(void);   /* gpu/render/rx_debug.cpp */
                if (!strncmp(line, "!dump", 5)) rx_request_frame_dump();
                extern volatile int hos_nvn_sync_log_on;   /* nvn_native.c: "!synclog" */
                if (!strncmp(line, "!synclog", 8)) hos_nvn_sync_log_on = 1;
                void rx_set_screenshot_ms(int64_t ms);   /* gpu/render/rx_core.cpp: "!shots <ms>" */
                if (!strncmp(line, "!shots ", 7)) rx_set_screenshot_ms(atoll(line + 7));
                void rx_request_rdoc_capture(int frames);   /* gpu/render/rx_debug.cpp: "!rdoc [frames]" */
                if (!strncmp(line, "!rdoc", 5)) rx_request_rdoc_capture(atoi(line + 5));
                void rx_flash_dark_arm(void);   /* gpu/render/rx_core.cpp: arms RX_FLASH_DARK_DUMP (RX_FLASH_DARK_AFTER=-1) */
                if (!strncmp(line, "!flasharm", 9)) rx_flash_dark_arm();
                hos_log("hid: live command at %.1f s: %.*s", at / 1000.0, (int)(n > 1 ? n - 1 : 0), line);
                continue;
            }
            const int got = parse_presses(line, at, 1);
            hos_log("hid: live input at %.1f s: %d presses (%.*s)", at / 1000.0, got, (int)(n > 1 ? n - 1 : 0), line);
        }
    }
    fclose(f);
}

static int parse_presses(const char *s, int64_t base_ms, int live) {
    static const struct { const char *name; int b; int axis; int dir; } names[] = {
        {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"LS", B_LS}, {"RS", B_RS}, {"L", B_L}, {"R", B_R},
        {"ZL", B_ZL}, {"ZR", B_ZR}, {"PLUS", B_PLUS}, {"MINUS", B_MINUS}, {"LEFT", B_LEFT}, {"UP", B_UP},
        {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN}, {"LUP", -1, 1, 1}, {"LDOWN", -1, 1, -1}, {"LLEFT", -1, 0, -1},
        {"LRIGHT", -1, 0, 1}, {"RUP", -1, 3, 1}, {"RDOWN", -1, 3, -1}, {"RLEFT", -1, 2, -1}, {"RRIGHT", -1, 2, 1}};
    int got = 0;
    const int64_t now_ms = (hos_now_ns() - script_t0) / 1000000;
    while (s && *s) {
        while (*s == ' ' || *s == '\t') s++;
        Press p;
        memset(&p, 0, sizeof p);
        char *end;
        double t = strtod(s, &end);
        if (end == s || *end != ':') break;
        s = end + 1;
        int64_t hold = 200;
        for (;;) {
            size_t n = strcspn(s, "+/,\r\n");
            for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
                if (strlen(names[i].name) == n && !strncmp(s, names[i].name, n)) {
                    if (names[i].b >= 0) p.buttons |= 1ULL << names[i].b;
                    else p.stick[names[i].axis] = names[i].dir * 32767;
                }
            s += n;
            if (*s != '+') break;
            s++;
        }
        if (*s == '/') hold = strtol(s + 1, (char **)&s, 10);
        p.start_ms = base_ms + (int64_t)(t * 1000);
        p.end_ms = p.start_ms + hold;
        int slot = npresses < MAX_PRESSES ? npresses : -1;
        if (slot < 0)   /* full: reuse a press that is over */
            for (int i = 0; i < MAX_PRESSES && slot < 0; i++)
                if (presses[i].end_ms < now_ms) slot = i;
        if (slot < 0) break;
        if (!live && p.end_ms > script_end_ms) script_end_ms = p.end_ms;
        presses[slot] = p;
        if (slot == npresses) npresses++;
        got++;
        if (*s == ',') s++;
    }
    return got;
}

static void scripted_input(uint64_t *buttons, int32_t stick[4]) {
    if (npresses < 0) parse_script();
    live_input();
    const int64_t ms = (hos_now_ns() - script_t0) / 1000000;
    for (int i = 0; i < npresses; i++)
        if (ms >= presses[i].start_ms && ms < presses[i].end_ms) {
            *buttons |= presses[i].buttons;
            for (int k = 0; k < 4; k++)
                if (presses[i].stick[k]) stick[k] = presses[i].stick[k];
        }
}

static void stick_max(int32_t stick[4], const int32_t v[4]) {
    for (int i = 0; i < 4; i++)
        if ((v[i] < 0 ? -v[i] : v[i]) > (stick[i] < 0 ? -stick[i] : stick[i])) stick[i] = v[i];
}

/* ---- (hookshot) the controls: the gamepad input (by position) and the keys that press each Switch button, from
   hookshot_options.ini beside the executable when it has them: pad_<button>=<HOS_PAD_* position, -1 none> and
   key_<button>=<key code>,<key code> (Windows key codes, 0 none), <button> one of bind_names. Read once, at the
   first use; what the file leaves out keeps its default ---- */
static const char *const bind_names[HOS_SW_N] = {"a", "b", "x", "y", "ls", "rs", "l", "r", "zl", "zr", "plus",
                                                 "minus", "left", "up", "right", "down"};
static const signed char pad_default[HOS_SW_N] = {
    HOS_PAD_EAST, HOS_PAD_SOUTH, HOS_PAD_NORTH, HOS_PAD_WEST, HOS_PAD_LSTICK, HOS_PAD_RSTICK, HOS_PAD_LSHOULDER,
    HOS_PAD_RSHOULDER, HOS_PAD_LTRIGGER, HOS_PAD_RTRIGGER, HOS_PAD_START, HOS_PAD_BACK, HOS_PAD_LEFT, HOS_PAD_UP,
    HOS_PAD_RIGHT, HOS_PAD_DOWN};
/* (the keys of totk-pc-recomp's docs/controls.md, github.com/valentderah/totk-pc-recomp, MIT; plus Enter for Plus) */
static const unsigned char key_default[HOS_SW_N][2] = {
    {'E'}, {VK_SHIFT}, {VK_SPACE}, {0}, {VK_CONTROL}, {'C'}, {'Q'}, {'R'}, {'Z'}, {'X', 'F'}, {VK_TAB, VK_RETURN},
    {'M'}, {'1'}, {'2'}, {'3'}, {'4'}};
static int pad_bind[HOS_SW_N], key_bind[HOS_SW_N][2];

static void binds_load(void) {
    static volatile LONG state;   /* 0 not read, 1 being read, 2 read */
    if (state == 2) return;
    if (InterlockedCompareExchange(&state, 1, 0) != 0) {
        while (state != 2) Sleep(0);
        return;
    }
    for (int i = 0; i < HOS_SW_N; i++) {
        pad_bind[i] = pad_default[i];
        for (int k = 0; k < 2; k++) key_bind[i][k] = key_default[i][k];
    }
    char path[MAX_PATH];
    const DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = len ? (strrchr(path, 92) ? strrchr(path, 92) : strrchr(path, 47)) : NULL;   /* (linux: '/') */
    FILE *f = NULL;
    if (slash) {
        snprintf(slash + 1, (size_t)(path + MAX_PATH - slash - 1), "hookshot_options.ini");
        f = fopen(path, "r");
    }
    int changed = 0;
    if (f) {
        char line[80];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const int pad = !strncmp(line, "pad_", 4), keys = !strncmp(line, "key_", 4);
            for (int i = 0; (pad || keys) && i < HOS_SW_N; i++) {
                if (strcmp(line + 4, bind_names[i])) continue;
                changed++;
                if (pad) {
                    const int v = atoi(eq + 1);
                    pad_bind[i] = v >= 0 && v < HOS_PAD_N ? v : -1;
                } else {
                    const char *comma = strchr(eq + 1, ',');
                    const int a = atoi(eq + 1), b = comma ? atoi(comma + 1) : 0;
                    key_bind[i][0] = a > 0 && a < 256 ? a : 0;
                    key_bind[i][1] = b > 0 && b < 256 ? b : 0;
                }
            }
        }
        fclose(f);
    }
    if (changed) hos_log("hid: %d controls from hookshot_options.ini", changed);
    InterlockedExchange(&state, 2);
}

static volatile LONG pad_raw_now;   /* the gamepads' inputs at the last read, 1 << HOS_PAD_* */
uint32_t hos_pad_raw(void) { return (uint32_t)pad_raw_now; }

static void pads_read(uint64_t *buttons, int32_t stick[4]) {
    static int64_t next_read, retry[XUSER_MAX_COUNT];
    static int was_connected[XUSER_MAX_COUNT];
    static uint64_t cached_buttons;
    static int32_t cached_stick[4];
    const int64_t now = hos_now_ns();
    if (now < next_read) {
        *buttons |= cached_buttons;
        stick_max(stick, cached_stick);
        return;
    }
    next_read = now + 2000000;
    uint64_t raw = 0;   /* (by position: 1 << HOS_PAD_*) */
    int32_t s[4] = {0};
    {   /* every controller SDL knows (DualSense, DualShock 4, Switch Pro, ...: hos/pad_sdl.c) */
        int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]);
        static int was;
        const int n = hos_sdl_pads(&raw, s);
        if (n != was) hos_log("hid: %d controller%s through SDL", n, n == 1 ? "" : "s");
        was = n;
    }
#ifdef __EMSCRIPTEN__
    xinput_get = XInputGetState;
#endif
    if (!xinput_get) {
        static int64_t load_retry;
        if (now >= load_retry) {
            load_retry = now + 5000000000LL;
            HMODULE x = LoadLibraryA("xinput1_4.dll");
            if (!x) x = LoadLibraryA("xinput9_1_0.dll");
            if (x) xinput_get = (XInputGetStateFn)GetProcAddress(x, "XInputGetState");
        }
    }
    static const struct { WORD x; int b; } map[] = {
        {XINPUT_GAMEPAD_A, HOS_PAD_SOUTH}, {XINPUT_GAMEPAD_B, HOS_PAD_EAST}, {XINPUT_GAMEPAD_X, HOS_PAD_WEST},
        {XINPUT_GAMEPAD_Y, HOS_PAD_NORTH}, {XINPUT_GAMEPAD_LEFT_THUMB, HOS_PAD_LSTICK},
        {XINPUT_GAMEPAD_RIGHT_THUMB, HOS_PAD_RSTICK}, {XINPUT_GAMEPAD_LEFT_SHOULDER, HOS_PAD_LSHOULDER},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, HOS_PAD_RSHOULDER}, {XINPUT_GAMEPAD_START, HOS_PAD_START},
        {XINPUT_GAMEPAD_BACK, HOS_PAD_BACK}, {XINPUT_GAMEPAD_DPAD_LEFT, HOS_PAD_LEFT},
        {XINPUT_GAMEPAD_DPAD_UP, HOS_PAD_UP}, {XINPUT_GAMEPAD_DPAD_RIGHT, HOS_PAD_RIGHT},
        {XINPUT_GAMEPAD_DPAD_DOWN, HOS_PAD_DOWN}};
    for (int p = 0; p < XUSER_MAX_COUNT && xinput_get; p++) {
        if (now < retry[p]) continue;
        XINPUT_STATE xs;
        if (xinput_get((DWORD)p, &xs) != ERROR_SUCCESS) {
            if (was_connected[p]) hos_log("hid: controller %d disconnected", p);
            was_connected[p] = 0;
            retry[p] = now + 1000000000LL;
            continue;
        }
        if (!was_connected[p]) hos_log("hid: controller %d connected", p);
        was_connected[p] = 1;
        const WORD w = xs.Gamepad.wButtons;
        for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
            if (w & map[i].x) raw |= 1ULL << map[i].b;
        if (xs.Gamepad.bLeftTrigger > 64) raw |= 1ULL << HOS_PAD_LTRIGGER;
        if (xs.Gamepad.bRightTrigger > 64) raw |= 1ULL << HOS_PAD_RTRIGGER;
        const int16_t thumb[4] = {xs.Gamepad.sThumbLX, xs.Gamepad.sThumbLY, xs.Gamepad.sThumbRX, xs.Gamepad.sThumbRY};
        int32_t v[4];
        for (int i = 0; i < 4; i++) {
            const int a = thumb[i] < -32767 ? -32767 : thumb[i];
            v[i] = (a > 7849 || a < -7849) ? a : 0;
        }
        stick_max(s, v);
    }
    InterlockedExchange(&pad_raw_now, (LONG)raw);
    /* Back and Start together open the Insert menu (gpu/vk/vk_menu.cpp, for pads whose Home button the system
       keeps): they reach the game only one at a time */
    const uint64_t chord = 1ULL << HOS_PAD_BACK | 1ULL << HOS_PAD_START;
    if ((raw & chord) == chord) raw &= ~chord;
    binds_load();
    uint64_t b = 0;
    for (int i = 0; i < HOS_SW_N; i++)
        if (pad_bind[i] >= 0 && (raw >> pad_bind[i] & 1)) b |= 1ULL << i;
    cached_buttons = b;
    memcpy(cached_stick, s, sizeof s);
    *buttons |= b;
    stick_max(stick, s);
}

static void read_input(uint64_t *buttons, int32_t stick[4]) {
    *buttons = 0;
    memset(stick, 0, 4 * sizeof *stick);
    scripted_input(buttons, stick);
    /* A scripted run (a test) takes no input from the player's devices until 5 s after its last press (a variable
       left set in a shell no longer leaves the game without controls), and otherwise the pads count only
       while the game's window is in front, like the keyboard: XInput is global, so a test run in the background
       used to follow the player's pad (and pressed things in its own menus). */
    /* (hookshot) The player's devices count during a script too (the user plays along in test runs, 2026-11-05):
       the presses add to theirs. HOS_INPUT_EXCLUSIVE=1: the script alone until 5 s after its last press, as before */
    static int exclusive = -1;
    if (exclusive < 0) exclusive = getenv("HOS_INPUT_EXCLUSIVE") != NULL;
    if ((exclusive && npresses > 0 && (hos_now_ns() - script_t0) / 1000000 < script_end_ms + 5000) || !game_in_front())
        return;
    {   /* (hookshot) while the Insert menu is open the pads are its own, as the keyboard is (key()), and after it
           closed until they are let go (the press that closed it never reaches the game) */
        uint64_t pb = 0;
        int32_t ps[4] = {0};
        pads_read(&pb, ps);
        static int after_menu;
        if (rx_option(1, 0)) after_menu = 1;
        else if (after_menu && !pad_raw_now) after_menu = 0;
        if (!after_menu) {
            *buttons |= pb;
            stick_max(stick, ps);
        }
    }
    binds_load();   /* (the keys: key_default's, or hookshot_options.ini's) */
    for (int i = 0; i < HOS_SW_N; i++)
        for (int k = 0; k < 2; k++)
            if (key_bind[i][k] && key(key_bind[i][k])) *buttons |= 1ULL << i;
    int32_t mouse[4] = {0};
    /* left button Y (attack), right button ZL (shield, lock on), while the mouse is captured; a button held when
       the capture began counts only after it was let go (the click that captures never reaches the game) */
    static int armed[2];
    if (mouse_stick(&mouse[2], &mouse[3], (int)(*buttons >> B_L & 1))) {
        stick_max(stick, mouse);
        const int vk[2] = {VK_LBUTTON, VK_RBUTTON}, b[2] = {B_Y, B_ZL};
        for (int i = 0; i < 2; i++) {
            if (!key(vk[i])) armed[i] = 1;
            else if (armed[i]) *buttons |= 1ULL << b[i];
        }
    } else {
        armed[0] = armed[1] = 0;
    }
    /* (WASD the left stick; the arrows or IJKL the camera) */
    const int cx = (key(VK_RIGHT) || key('L')) - (key(VK_LEFT) || key('J')),
              cy = (key(VK_UP) || key('I')) - (key(VK_DOWN) || key('K'));
    const int32_t keys[4] = {(key('D') - key('A')) * 32767, (key('W') - key('S')) * 32767, cx * 32767, cy * 32767};
    stick_max(stick, keys);
}

static void vibration_send(uint64_t handle, uint64_t value);
volatile int64_t hos_hid_last_read;

/* GetNpadStates(State* out, int count, unsigned int const& npadId): the latest state first */
static void npad_states(Ctx *c) {
    uint64_t out = ARG(0);
    int count = (int)ARG(1);
    uint32_t id = G32(ARG(2));
    uint64_t buttons = 0;
    int32_t stick[4] = {0};
    int connected = id == NPAD_NO1;
    {
        static int64_t last_read;
        static uint32_t seen_ids[16];
        static int nseen;
        const int64_t now = hos_now_ns();
        if (connected && last_read && now - last_read > 2000000000LL)
            hos_log("hid: the game read no controller for %.1f s (lr %#llx)", (now - last_read) / 1e9,
                    (unsigned long long)c->x[30]);
        if (connected) last_read = hos_hid_last_read = now;
        int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen_ids[i] == id;
        if (!known && nseen < 16) {
            seen_ids[nseen++] = id;
            hos_log("hid: the game reads npad %#x (%s, lr %#llx)", id, connected ? "No.1" : "not connected",
                    (unsigned long long)c->x[30]);
        }
    }
    if (connected) read_input(&buttons, stick);
    uint64_t hos_title_input(uint64_t buttons);   /* totk_title.c: the title menu's credits entries */
    if (connected) buttons = hos_title_input(buttons);
    uint64_t hos_frame_step_buttons(uint64_t buttons);   /* nn_vi.c: the frame step of the wheel and quick menus */
    if (connected) buttons = hos_frame_step_buttons(buttons);
    int64_t s = ++sampling;
    for (int i = 0; i < count; i++) {
        uint64_t e = out + (uint64_t)i * STATE_SIZE;
        memset(GP(e), 0, STATE_SIZE);
        G64(e) = (uint64_t)(s - i);
        G64(e + 8) = buttons;
        memcpy(GP(e + 16), stick, 16);
        G32(e + 32) = connected ? ATTR_CONNECTED : 0;
    }
    RET(count);
}
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj) { npad_states(c); }
HLE(_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj) { npad_states(c); }
/* GetNpadStyleSet(unsigned int const&) -> BitFlagSet<32, NpadStyleTag> */
HLE(_ZN2nn3hid15GetNpadStyleSetERKj) { RET(G32(ARG(0)) == NPAD_NO1 ? STYLE_FULLKEY : 0); }

HLE(_ZN2nn3hid14InitializeNpadEv) {}
HLE(_ZN2nn3hid18InitializeKeyboardEv) {}
HLE(_ZN2nn3hid15InitializeMouseEv) {}
HLE(_ZN2nn3hid14DisconnectNpadERKj) {}
HLE(_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE) {}
HLE(_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm) {}
HLE(_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {}
HLE(_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE) {}

/* ShowControllerSupport(ControllerSupportResultInfo*, ControllerSupportArg const&) -> Result: one player, No.1 */
HLE(_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE) {
    memset(GP(ARG(0)), 0, 8);
    *(uint8_t *)GP(ARG(0)) = 1;
    G32(ARG(0) + 4) = NPAD_NO1;
    RET(0);
}

/* Handles: six-axis ones are opaque u32s; a vibration handle is {u8 style type (3 = full key), u8 npad id, u8 device
   index (0 left, 1 right), u8 reserved}, which the SDK's vibration classes read. Vibration goes nowhere yet. */
HLE(_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    for (int i = 0; i < (int)ARG(1); i++) G32(ARG(0) + 4 * (uint64_t)i) = 0x100u + (uint32_t)i;
    RET((int)ARG(1));
}
HLE(_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE) {
    static int64_t six_sampling;
    const uint64_t out = ARG(0);
    const int count = (int)ARG(1) < 1 ? 0 : (int)ARG(1) > 32 ? 32 : (int)ARG(1);
    const int64_t s = ++six_sampling;
    static const float at_rest[18] = {0, 0, -1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < count; i++) {
        const uint64_t e = out + (uint64_t)i * 0x60;
        memset(GP(e), 0, 0x60);
        G64(e) = 5000000;
        G64(e + 8) = (uint64_t)(s - i);
        memcpy(GP(e + 0x10), at_rest, sizeof at_rest);
        G32(e + 0x58) = 1;
    }
    RET(count);
}
HLE(_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE) {
    int n = (int)ARG(1) < 2 ? (int)ARG(1) : 2;
    for (int i = 0; i < n; i++) G32(ARG(0) + 4 * (uint64_t)i) = 3u | (G32(ARG(2)) & 0xFF) << 8 | (uint32_t)i << 16;
    RET(n);
}
HLE(_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE) {}
HLE(_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE) { vibration_send(ARG(0), ARG(1)); }

/* nn::hid::detail vibration: what the SDK's own classes (VibrationNode, VibrationTarget, VibrationPlayer) call; on
   hardware each is an IPC to hid. A device is an LRA on its side of the controller, always mounted and permitted. */
/* A vibration value for one of the two motors of the pad (handle +2 = device index): the pads that rumble (SDL) get
   the stronger of the two sides' low and high band amplitudes. */
/* (issue 263) An HD rumble amplitude is not a motor's: OoT asks for 0.02-0.06 for most effects, a few percent of a DualSense's or an
   Xbox pad's motor, nothing you feel. Eden's SDL driver's mapping (Eglantine src/input_common/drivers/sdl_driver.cpp, GPL-3.0):
   (a + a^0.35) / 2, then less for higher frequencies, to give some feel of them */
static float rumble_motor(float amp, float freq, float start, float width) {
    if (amp <= 0.0f) return 0.0f;
    float scale = 1.0f;
    if (freq > start) {
        scale = 1.0f - (freq - start) / width;
        if (scale < 0.3f) scale = 0.3f;
    }
    const float boosted = (amp + powf(amp, 0.35f)) * 0.5f;
    return boosted * scale > 1.0f ? 1.0f : boosted * scale;
}
static void vibration_send(uint64_t handle, uint64_t value) {
    static float amp[2][2];
    float v[4];
    memcpy(v, GP(value), sizeof v);
    const int dev = (G32(handle) >> 16) & 1;
    amp[dev][0] = rumble_motor(v[0], v[1], 140.0f, 400.0f);
    amp[dev][1] = rumble_motor(v[2], v[3], 200.0f, 700.0f);
    {   /* (hookshot, issue 263) what the game asks for: the first values, then a count now and then */
        static long long n, strong;
        static float peak;
        if (v[0] > peak) peak = v[0];
        if (v[2] > peak) peak = v[2];
        if ((v[0] > 0.01f || v[2] > 0.01f) && strong++ < 6)
            hos_log("hid: vibration #%lld: handle %08x, low %.2f @ %.0f Hz, high %.2f @ %.0f Hz", n, G32(handle), v[0], v[1], v[2], v[3]);
        if ((++n & 4095) == 0) hos_log("hid: %lld vibration values, strongest %.2f since the last count", n, peak), peak = 0;
    }
    void hos_sdl_rumble(float low, float high);   /* pad_sdl.c */
    hos_sdl_rumble(amp[0][0] > amp[1][0] ? amp[0][0] : amp[1][0], amp[0][1] > amp[1][1] ? amp[0][1] : amp[1][1]);
}
static void vibration_value_idle(uint64_t v) {   /* VibrationValue {f32 amp_low, freq_low, amp_high, freq_high} */
    float idle[4] = {0.0f, 160.0f, 0.0f, 320.0f};
    memcpy(GP(v), idle, sizeof idle);
}
HLE(_ZN2nn3hid6detail15PermitVibrationEb) { RET(0); }
HLE(_ZN2nn3hid6detail20IsVibrationPermittedEPb) { *(uint8_t *)GP(ARG(0)) = 1; RET(0); }
HLE(_ZN2nn3hid6detail27BeginPermitVibrationSessionEv) { RET(0); }
HLE(_ZN2nn3hid6detail25EndPermitVibrationSessionEv) { RET(0); }
HLE(_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE) { RET(0); }
/* GetVibrationDeviceInfo(VibrationDeviceInfo* {u32 type; u32 position}, handle const&): LRA (1), left 1 / right 2 */
HLE(_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE) {
    G32(ARG(0)) = 1;
    G32(ARG(0) + 4) = ((G32(ARG(1)) >> 16) & 1) + 1;
    RET(0);
}
HLE(_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE) {
    vibration_send(ARG(0), ARG(1));
    RET(0);
}
HLE(_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi) {
    for (int i = 0; i < (int)ARG(2) && i < 8; i++) vibration_send(ARG(0) + 4 * (uint64_t)i, ARG(1) + 16 * (uint64_t)i);
    RET(0);
}
HLE(_ZN2nn3hid6detail23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE) {
    vibration_value_idle(ARG(0));
    RET(0);
}
HLE(_ZN2nn3hid6detail25SendVibrationGcErmCommandERKNS0_21VibrationDeviceHandleERKNS0_21VibrationGcErmCommandE) { RET(0); }
HLE(_ZN2nn3hid6detail30GetActualVibrationGcErmCommandEPNS0_21VibrationGcErmCommandERKNS0_21VibrationDeviceHandleE) {
    G32(ARG(0)) = 0;   /* Stop */
    RET(0);
}
HLE(_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE) { *(uint8_t *)GP(ARG(0)) = 1; RET(0); }
HLE(_ZN2nn3hid6detail21EnqueueVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE) {
    vibration_send(ARG(0), ARG(1));
    RET(0);
}
HLE(_ZN2nn3hid6detail28FlushEnqueuedVibrationValuesEv) { RET(0); }

/* ---- (mkw-recomp) the touch screen: none touched. GetTouchScreenStates<16>(TouchScreenState<16>*, int count) ->
   states written: one, with no touches (the lifted SDK version read the touch screen's shared memory, which this
   runtime does not have). TouchScreenState<16>: {s64 sampling number, s32 count, s32 reserved, TouchState[16]}. */
HLE(_ZN2nn3hid21InitializeTouchScreenEv) {}
HLE(_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi) {
    static volatile LONG64 sampling;
    if ((int)ARG(1) < 1) { RET(0); return; }
    memset(GP(ARG(0)), 0, 16 + 16 * 0x28);
    G64(ARG(0)) = (uint64_t)InterlockedIncrement64(&sampling);
    RET(1);
}
