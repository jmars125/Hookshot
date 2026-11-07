/* (linux) The game's window on SDL2 (libSDL2-2.0.so.0, loaded at run time as host/pad_sdl.c loads it): what
   host/display.c is on Windows. The window lives on the process's main thread, as SDL wants it (hos_main.c runs the
   game on a thread of its own on Linux); the Vulkan back end makes its surface through SDL (gpu/vk/vk_api.h
   MkwVkWindow), on a thread of its own so the window keeps answering while the shaders are prepared.

   The keyboard's state goes to hos_posix_keys (GetAsyncKeyState, for nn_hid.c and the Insert menu), the window's focus
   and the screen's size to hos_posix_window (GetForegroundWindow, GetSystemMetrics).
   HOS_HEADLESS=1: no window. HOS_WINDOW_SIZE=<w>x<h>: its size (default 1280x720). F11: fullscreen (HOS_FULLSCREEN=1 to
   start so). Mouse: a click in the window captures it (relative motion, for nn_hid.c), Esc or leaving releases it. */
#include "../hos.h"
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../../gpu/vk/vk_api.h"

int rx_start(void *hwnd);
void rx_resize(int width, int height);
void hos_sdl_pads_pump(void);   /* host/pad_sdl.c: controllers, on this thread, as SDL's queue wants */

volatile LONG64 hos_present_count;   /* (nvn_native.c: the game's presents) */
__attribute__((visibility("default"))) volatile uint8_t hos_posix_keys[256];
__attribute__((visibility("default"))) volatile int hos_posix_window[4];   /* focused, screen width, screen height, fullscreen */

static volatile LONG mouse_dx, mouse_dy, mouse_captured;
int hos_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = InterlockedExchange(&mouse_dx, 0);
    *dy = InterlockedExchange(&mouse_dy, 0);
    return mouse_captured;
}

/* ---- the SDL2 entry points used ---- */
typedef struct { uint32_t format; int w, h, refresh_rate; void *driverdata; } SdlDisplayMode;
typedef union { uint32_t type; uint8_t raw[56]; } SdlEvent;
static struct {
    int (*Init)(uint32_t);
    int (*InitSubSystem)(uint32_t);
    const char *(*GetError)(void);
    int (*SetHint)(const char *, const char *);
    void *(*CreateWindow)(const char *, int, int, int, int, uint32_t);
    int (*SetWindowFullscreen)(void *, uint32_t);
    int (*WaitEventTimeout)(SdlEvent *, int);
    int (*PollEvent)(SdlEvent *);
    const uint8_t *(*GetKeyboardState)(int *);
    uint32_t (*GetMouseState)(int *, int *);
    int (*SetRelativeMouseMode)(int);
    int (*GetDesktopDisplayMode)(int, SdlDisplayMode *);
    int (*Vulkan_CreateSurface)(void *, void *, void *);
    void (*Vulkan_GetDrawableSize)(void *, int *, int *);
    void (*SetWindowTitle)(void *, const char *);
} sdl;
enum { SDL_INIT_VIDEO = 0x20, SDL_INIT_EVENTS = 0x4000 };
enum { WIN_SHOWN = 0x4, WIN_RESIZABLE = 0x20, WIN_HIGHDPI = 0x2000, WIN_FULLSCREEN_DESKTOP = 0x1001, WIN_VULKAN = 0x10000000 };
enum { EV_QUIT = 0x100, EV_WINDOW = 0x200, EV_KEYDOWN = 0x300, EV_MOUSEMOTION = 0x400, EV_MOUSEDOWN = 0x401 };
enum { WE_SIZE_CHANGED = 6, WE_LEAVE = 11, WE_FOCUS_GAINED = 12, WE_FOCUS_LOST = 13, WE_CLOSE = 14 };

static int load_sdl(void) {
    char path[PATH_MAX];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    while (n && path[n - 1] != '/') n--;
    snprintf(path + n, sizeof path - n, "SDL2.dll");   /* (beside the executable first: LoadLibraryA makes it libSDL2-2.0.so.0) */
    void *m = LoadLibraryA(path);
    if (!m) m = LoadLibraryA("SDL2.dll");
    if (!m) {
        hos_log("display: libSDL2-2.0.so.0 not found: no window");
        return 0;
    }
#define GET(field, name) if (!(*(void **)&sdl.field = dlsym(m, name))) { hos_log("display: SDL2 has no %s", name); return 0; }
    GET(Init, "SDL_Init");
    GET(InitSubSystem, "SDL_InitSubSystem");
    GET(GetError, "SDL_GetError");
    GET(SetHint, "SDL_SetHint");
    GET(CreateWindow, "SDL_CreateWindow");
    GET(SetWindowFullscreen, "SDL_SetWindowFullscreen");
    GET(WaitEventTimeout, "SDL_WaitEventTimeout");
    GET(PollEvent, "SDL_PollEvent");
    GET(GetKeyboardState, "SDL_GetKeyboardState");
    GET(GetMouseState, "SDL_GetMouseState");
    GET(SetRelativeMouseMode, "SDL_SetRelativeMouseMode");
    GET(GetDesktopDisplayMode, "SDL_GetDesktopDisplayMode");
    GET(Vulkan_CreateSurface, "SDL_Vulkan_CreateSurface");
    GET(Vulkan_GetDrawableSize, "SDL_Vulkan_GetDrawableSize");
    GET(SetWindowTitle, "SDL_SetWindowTitle");
#undef GET
    return 1;
}

/* ---- keys: SDL's scancodes (USB HID usages) as Windows' virtual keys ---- */
static void keys_update(void) {
    int n = 0;
    const uint8_t *k = sdl.GetKeyboardState(&n);
    uint8_t v[256];
    memset(v, 0, sizeof v);
    if (k && hos_posix_window[0]) {
#define K(sc) ((sc) < n && k[sc])
        for (int i = 0; i < 26; i++) v['A' + i] = K(4 + i);
        for (int i = 0; i < 9; i++) v['1' + i] = K(30 + i);
        v['0'] = K(39);
        v[VK_RETURN] = K(40) || K(88), v[VK_ESCAPE] = K(41), v[VK_BACK] = K(42), v[VK_TAB] = K(43), v[VK_SPACE] = K(44);
        v[VK_OEM_MINUS] = K(45), v[VK_OEM_PLUS] = K(46), v[VK_OEM_1] = K(51), v[VK_OEM_3] = K(53);
        v[VK_OEM_COMMA] = K(54), v[VK_OEM_PERIOD] = K(55), v[VK_OEM_2] = K(56), v[VK_CAPITAL] = K(57);
        for (int i = 0; i < 12; i++) v[VK_F1 + i] = K(58 + i);
        v[VK_PAUSE] = K(72), v[VK_INSERT] = K(73), v[VK_HOME] = K(74), v[VK_PRIOR] = K(75), v[VK_DELETE] = K(76);
        v[VK_END] = K(77), v[VK_NEXT] = K(78), v[VK_RIGHT] = K(79), v[VK_LEFT] = K(80), v[VK_DOWN] = K(81), v[VK_UP] = K(82);
        v[VK_DIVIDE] = K(84), v[VK_MULTIPLY] = K(85), v[VK_SUBTRACT] = K(86), v[VK_ADD] = K(87);
        for (int i = 0; i < 9; i++) v[VK_NUMPAD0 + 1 + i] = K(89 + i);
        v[VK_NUMPAD0] = K(98), v[VK_DECIMAL] = K(99);
        v[VK_LCONTROL] = K(224), v[VK_LSHIFT] = K(225), v[VK_LMENU] = K(226);
        v[VK_RCONTROL] = K(228), v[VK_RSHIFT] = K(229), v[VK_RMENU] = K(230);
#undef K
        v[VK_CONTROL] = v[VK_LCONTROL] || v[VK_RCONTROL];
        v[VK_SHIFT] = v[VK_LSHIFT] || v[VK_RSHIFT];
        v[VK_MENU] = v[VK_LMENU] || v[VK_RMENU];
        const uint32_t b = sdl.GetMouseState(NULL, NULL);
        v[VK_LBUTTON] = (b & 1) != 0, v[VK_MBUTTON] = (b & 2) != 0, v[VK_RBUTTON] = (b & 4) != 0;
    }
    for (int i = 0; i < 256; i++) hos_posix_keys[i] = v[i];
}

/* ---- the window ---- */
static void *window;
static volatile int renderer;   /* the renderer presents into the window */
static HANDLE renderer_up;   /* (set once rx_start has returned, ready or not: hos_renderer_wait) */
static int fullscreen;

static int create_surface(void *w, void *instance, void *surface) { return sdl.Vulkan_CreateSurface(w, instance, surface); }
static void drawable_size(void *w, int *width, int *height) { sdl.Vulkan_GetDrawableSize(w, width, height); }
static MkwVkWindow vk_window = {MKWVK_WINDOW_VERSION, create_surface, drawable_size, NULL};

static void mouse_capture(int on) {
    if (on == (int)mouse_captured) return;
    mouse_captured = on;
    InterlockedExchange(&mouse_dx, 0);
    InterlockedExchange(&mouse_dy, 0);
    sdl.SetRelativeMouseMode(on);
}
static void set_fullscreen(int on) {
    if (on == fullscreen) return;
    fullscreen = on;
    hos_posix_window[3] = on;
    sdl.SetWindowFullscreen(window, on ? WIN_FULLSCREEN_DESKTOP : 0);
    hos_log("display: %s", on ? "fullscreen (F11 for a window)" : "windowed");
}

static DWORD WINAPI renderer_main(void *p) {
    (void)p;
    const int ok = rx_start(&vk_window);
    if (!ok) hos_log("display: the renderer did not start: the window stays black");
    renderer = ok;
    if (ok) {   /* (a size change while it started) */
        int w = 0, h = 0;
        sdl.Vulkan_GetDrawableSize(window, &w, &h);
        if (w > 0 && h > 0) rx_resize(w, h);
    }
    SetEvent(renderer_up);
    return 0;
}

/* (hookshot) host/display.c's: the game's first work for the GPU waits for the renderer (nvn_native.c), so nothing it submits while
   the shaders are prepared is lost. Without a window the renderer starts on the game's own thread, before this is asked */
void hos_renderer_wait(void) {
    { const char *e = getenv("HOS_NO_RENDERER_WAIT"); if (e && atoi(e) != 0) return; }
    if (renderer_up) WaitForSingleObject(renderer_up, INFINITE);
}

static volatile int start_asked;
static volatile int fullscreen_asked = -1;   /* (the Insert menu's row, from the renderer's thread: applied on this one) */

/* PostMessageA to the window (host/posix/win32_posix.c): WM_APP + 7, the Insert menu's Fullscreen row (host/display.c's) */
__attribute__((visibility("default"))) void hos_posix_post_message(uint32_t msg, uintptr_t wp, intptr_t lp) {
    (void)lp;
    if (msg == WM_APP + 7) __atomic_store_n(&fullscreen_asked, wp ? 1 : 0, __ATOMIC_RELEASE);
}
static int sdl_ready;
int hos_display_owns_sdl(void) { return sdl_ready; }   /* (controllers stay on this thread while it runs SDL) */

/* main(), before the game: SDL's video on this, the main, thread; the screen's size for GetSystemMetrics */
int hos_display_init(void) {
    if (getenv("HOS_HEADLESS")) return 0;
    if (!load_sdl()) return 0;
    sdl.SetHint("SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR", "0");
    sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    if (sdl.Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        hos_log("display: SDL_Init: %s: no window", sdl.GetError());
        return 0;
    }
    SdlDisplayMode m;
    if (!sdl.GetDesktopDisplayMode(0, &m)) hos_posix_window[1] = m.w, hos_posix_window[2] = m.h;
    sdl_ready = 1;
    return 1;
}

/* the game's thread, when NVN is up (hos_main.c): the window is made on the main thread, which waits for this */
void hos_display_start(void) {
    if (getenv("HOS_HEADLESS") || !sdl_ready) {   /* no window: the renderer still draws, offscreen (screenshots) */
        rx_start(NULL);
        return;
    }
    renderer_up = CreateEventA(NULL, TRUE, FALSE, NULL);
    __atomic_store_n(&start_asked, 1, __ATOMIC_RELEASE);
}

/* the main thread's loop, until the process ends: the window once asked for, then its events */
void hos_display_loop(void) {
    if (!sdl_ready) {
        for (;;) Sleep(1000);
    }
    SdlEvent e;
    while (!__atomic_load_n(&start_asked, __ATOMIC_ACQUIRE)) sdl.WaitEventTimeout(&e, 10);   /* (nothing to show yet) */
    extern float hos_aspect_ratio;
    int win_w = hos_aspect_ratio > 1.8f ? (int)(720 * hos_aspect_ratio + 0.5f) : 1280, win_h = 720;
    if (getenv("HOS_WINDOW_SIZE")) {
        int w = 0, h = 0;
        if (sscanf(getenv("HOS_WINDOW_SIZE"), "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) win_w = w, win_h = h;
    }
    window = sdl.CreateWindow(HOS_GAME_TITLE, 0x2FFF0000 /* centred */, 0x2FFF0000, win_w, win_h,
                              WIN_SHOWN | WIN_RESIZABLE | WIN_HIGHDPI | WIN_VULKAN);
    if (!window) {
        hos_log("display: SDL_CreateWindow: %s: no window", sdl.GetError());
        rx_start(NULL);
        SetEvent(renderer_up);
        for (;;) Sleep(1000);
    }
    vk_window.window = window;
    hos_posix_window[0] = 1;
    if (getenv("HOS_FULLSCREEN") && atoi(getenv("HOS_FULLSCREEN"))) set_fullscreen(1);
    CloseHandle(CreateThread(NULL, 0, renderer_main, NULL, 0, NULL));
    for (;;) {
        hos_sdl_pads_pump();
        if (sdl.WaitEventTimeout(&e, 4)) {
            do {
                switch (e.type) {
                case EV_QUIT:
                    hos_log("display: window closed, exiting");
                    hos_log_flush();
                    ExitProcess(0);
                case EV_WINDOW: {
                    const uint8_t what = e.raw[12];
                    if (what == WE_SIZE_CHANGED && renderer) {
                        int w = 0, h = 0;
                        sdl.Vulkan_GetDrawableSize(window, &w, &h);
                        rx_resize(w, h);
                    } else if (what == WE_FOCUS_GAINED) {
                        hos_posix_window[0] = 1;
                    } else if (what == WE_FOCUS_LOST || what == WE_LEAVE) {
                        if (what == WE_FOCUS_LOST) hos_posix_window[0] = 0;
                        mouse_capture(0);
                    } else if (what == WE_CLOSE) {
                        hos_log("display: window closed, exiting");
                        hos_log_flush();
                        ExitProcess(0);
                    }
                    break;
                }
                case EV_KEYDOWN: {
                    const int scancode = *(const int32_t *)(e.raw + 16), repeat = e.raw[13];
                    const uint16_t mod = *(const uint16_t *)(e.raw + 24);
                    if (scancode == 41) mouse_capture(0);   /* Esc */
                    if (!repeat && (scancode == 68 || (scancode == 40 && (mod & 0x300)))) set_fullscreen(!fullscreen);   /* F11, Alt+Enter */
                    break;
                }
                case EV_MOUSEDOWN:
                    if (renderer && !mouse_captured) mouse_capture(1);
                    break;
                case EV_MOUSEMOTION:
                    if (mouse_captured) {
                        InterlockedExchangeAdd(&mouse_dx, *(const int32_t *)(e.raw + 28));
                        InterlockedExchangeAdd(&mouse_dy, *(const int32_t *)(e.raw + 32));
                    }
                    break;
                }
            } while (sdl.PollEvent(&e));
        }
        const int asked = __atomic_exchange_n(&fullscreen_asked, -1, __ATOMIC_ACQ_REL);
        if (asked >= 0) set_fullscreen(asked);
        keys_update();
    }
}
