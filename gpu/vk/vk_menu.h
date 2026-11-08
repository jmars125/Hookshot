// The Insert menu (vk_menu.cpp): options that apply while the game runs, kept in hookshot_options.ini beside the executable.
#pragma once

#include "vk_internal.h"

namespace mkwvk {

// (out of the box every feature is off: what the game does on its own. The ini beside the executable keeps a player's choices)
struct Options {
    int framegen = 0;     // made frames: 0 off, or 2, 3, 4 times the game's frames
    int pace = 0;         // frames shown on an even line (vk_framegen.cpp)
    int resolution = 0;   // the 3D's: 0 Auto (the game's dynamic resolution), else 100%, 75%, 67%, 50% of its full scale (nx2/oot_patches.py)
    int upscale = 0;      // to the window: 0 off (the plain copy to the window), 1 Catmull-Rom, 2 Lanczos
    int sharpness = 0;    // 0 to 100
    int realtime = 1;     // game time follows the clock (host/nn_vi.c); 0: per frame (slower frames, slower game)
    int handheld = 0;     // the game's handheld profile: ~4 fps, softer; read once, at the start (host/nn_applet.c)
    int ultrawide = 0;    // the camera at the monitor's shape when it is wider than 16:9; read once, at the start (host/hos_main.c)
    int show_fps = 0;
    int fullscreen = 0;   // borderless, at the monitor's size: F11 is the same setting
    int pad_background = 0;   // the controllers count while another window has the focus (host/nn_hid.c; HOS_INPUT_BACKGROUND)
    int text_speed = 0;   // how fast dialogue prints: 0 the game's, 1 2x, 2 4x, 3 instant (host/nn_vi.c, nx2/oot_patches.py; HOS_TEXT_SPEED)
    int preset = 0;       // quality preset: 0 Custom, 1 Minimum (4GB), 2 Low (6GB), 3 Balanced (8GB), 4 High (12GB), 5 Ultra (16GB+)
    int vram_mb = 0;      // dynamic texture cache budget limit in MB
    int mirror_mb = 0;    // staging mirror limit in MB
};
extern Options opts;

void menu_init();             // (the options from the file, once)
void menu_tick();             // (each frame shown: the keys, while the game's window has the focus)
bool menu_is_open();
void menu_note_frame(bool game_frame);   // (the frame rates the FPS counter shows)
/// Over the window's image (TRANSFER_DST_OPTIMAL), w x h: the menu and the FPS counter. slot: the frame in flight, 0 to 2.
void menu_record(VkCommandBuffer cmd, VkImage dst, int slot, uint32_t w, uint32_t h);

} // namespace mkwvk
