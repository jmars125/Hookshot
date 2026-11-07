/* (linux) What the Windows build has and Linux does without:

   - Media Foundation's AV1 decoder (host/mf_av1.c): FFmpeg decodes AV1 itself here, with dav1d (host/nn_video.c asks
     FFmpeg first), so these are never reached with a dav1d build; without one, AV1 movies are skipped.
   - The WebGPU renderer (gpu/render, wgr_*): the Vulkan back end is the only renderer on Linux. Should it not start,
     these leave the window black, as a WebGPU renderer without a device does. */
#include "../hos.h"
#include "../../gpu/vk/vk_api.h"

typedef void (*mf_av1_picture_fn)(void *user, const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int64_t pts);
void *mf_av1_open(int w, int h) { (void)w, (void)h; return NULL; }
void mf_av1_decode(void *m, const uint8_t *data, size_t n, int64_t pts, mf_av1_picture_fn fn, void *user) {
    (void)m, (void)data, (void)n, (void)pts, (void)fn, (void)user;
}
void mf_av1_flush(void *m) { (void)m; }
void mf_av1_close(void *m) { (void)m; }

int wgr_start(void *hwnd) { (void)hwnd; hos_log("rx: no WebGPU renderer in the Linux build: nothing is drawn"); return 0; }
void wgr_resize(int width, int height) { (void)width, (void)height; }
const char *wgr_describe(void) { return "none"; }
void wgr_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines) { *draws = *skipped = *pipelines = 0; }
void wgr_nvn_submit(const uint64_t *handles, int n) { (void)handles, (void)n; }
uint64_t wgr_nvn_fence(void) { return 0; }
uint64_t wgr_nvn_pushed(void) { return 0; }
uint64_t wgr_nvn_replayed(void) { return 0; }
int wgr_nvn_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence, (void)timeout_ns; return 1; }
void wgr_wait_writebacks_reading(const void *p, uint64_t n) { (void)p, (void)n; }
void wgr_nvn_finish(void) {}
void wgr_nvn_present(const nvn_tex *t, const int crop[4]) { (void)t, (void)crop; }
void wgr_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v) { (void)id, (void)t, (void)v; }
void wgr_nvn_register_sampler(int id, const nvn_samp_par *p) { (void)id, (void)p; }
void wgr_nvn_set_texture_pool_size(int n) { (void)n; }
void wgr_nvn_texture_released(const nvn_tex *t) { (void)t; }
void wgr_nvn_program_released(const nvn_prog *p) { (void)p; }
void wgr_nvn_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                         uint64_t row, uint64_t img) {
    (void)t, (void)level, (void)layer0, (void)r, (void)dst, (void)row, (void)img;
}
void wgr_nvn_write_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src,
                          uint64_t row, uint64_t img) {
    (void)t, (void)level, (void)layer0, (void)r, (void)src, (void)row, (void)img;
}
void wgr_nvn_invalidate(uint64_t gpu, uint64_t size) { (void)gpu, (void)size; }
/* (gpu/render/rx_core.cpp's, for the soft write watch's other consumer) */
uint32_t *rx_collect_written(int64_t *count) { *count = -1; return NULL; }
/* (gpu/render's debugging commands: host/nn_hid.c's live input "!dump", "!shots", "!rdoc", "!flasharm") */
void rx_request_frame_dump(void) { hos_log("rx: frame dumps are the WebGPU renderer's: not in the Linux build"); }
void rx_set_screenshot_ms(int64_t ms) { (void)ms; hos_log("rx: !shots is the WebGPU renderer's: RX_VK_SHOTS instead"); }
void rx_request_rdoc_capture(int frames) { (void)frames; }
void rx_flash_dark_arm(void) {}
