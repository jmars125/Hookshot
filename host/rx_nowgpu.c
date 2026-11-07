/* (EvilOOT, 11-06) Stand-ins for the WebGPU renderer (gpu/render, renamed wgr_* by rx_wgpu_names.h) when the build
   has no wgpu-native release (nx2/build.py: "building without the renderer"): host/rx_backend.c and nn_hid.c still
   name its functions, so the executable did not link. Built only then (build.py leaves this file out when
   wgpu-native is there). With RX_BACKEND=vulkan none of the wgr_* calls is made; wgr_start fails, so a missing or
   unusable hookshot_vk.dll shows as "no renderer" instead of a game that runs with nothing drawn. */
#include "hos.h"
#include <stdint.h>

int wgr_start(void *hwnd) {
    (void)hwnd;
    hos_log("rx: this build has no WebGPU renderer (no wgpu-native): only the Vulkan back end (RX_BACKEND=vulkan) draws");
    return 0;
}
void wgr_resize(int width, int height) { (void)width, (void)height; }
const char *wgr_describe(void) { return "none (built without wgpu-native)"; }
void wgr_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines) {
    if (draws) *draws = 0;
    if (skipped) *skipped = 0;
    if (pipelines) *pipelines = 0;
}
void wgr_nvn_submit(const uint64_t *handles, int n) { (void)handles, (void)n; }
uint64_t wgr_nvn_fence(void) { return 0; }
uint64_t wgr_nvn_pushed(void) { return 0; }
uint64_t wgr_nvn_replayed(void) { return 0; }
int wgr_nvn_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence, (void)timeout_ns; return 1; }
void wgr_wait_writebacks_reading(const void *p, uint64_t n) { (void)p, (void)n; }
void wgr_nvn_finish(void) {}
void wgr_nvn_present(const void *t, const int crop[4]) { (void)t, (void)crop; }
void wgr_nvn_register_texture(int id, const void *t, const void *v) { (void)id, (void)t, (void)v; }
void wgr_nvn_register_sampler(int id, const void *p) { (void)id, (void)p; }
void wgr_nvn_set_texture_pool_size(int n) { (void)n; }
void wgr_nvn_texture_released(const void *t) { (void)t; }
void wgr_nvn_program_released(const void *p) { (void)p; }
void wgr_nvn_read_texels(const void *t, unsigned level, unsigned layer0, const void *r, uint8_t *dst, uint64_t row,
                         uint64_t img) {
    (void)t, (void)level, (void)layer0, (void)r, (void)dst, (void)row, (void)img;
}
void wgr_nvn_write_texels(const void *t, unsigned level, unsigned layer0, const void *r, const uint8_t *src,
                          uint64_t row, uint64_t img) {
    (void)t, (void)level, (void)layer0, (void)r, (void)src, (void)row, (void)img;
}
void wgr_nvn_invalidate(uint64_t gpu, uint64_t size) { (void)gpu, (void)size; }

/* the live-input file's !commands that only the WebGPU renderer implements (nn_hid.c) */
void rx_request_frame_dump(void) { hos_log("rx: !dump needs the WebGPU renderer (Vulkan: RX_VK_DUMP)"); }
void rx_set_screenshot_ms(int64_t ms) { (void)ms; hos_log("rx: !shots needs the WebGPU renderer (Vulkan: RX_VK_SHOTS)"); }
void rx_request_rdoc_capture(int frames) { (void)frames; hos_log("rx: !rdoc needs the WebGPU renderer"); }
void rx_flash_dark_arm(void) { hos_log("rx: !flasharm needs the WebGPU renderer (Vulkan: RX_VK_FLASH)"); }
