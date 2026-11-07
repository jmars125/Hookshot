/* The Vulkan back end (mkw_vk.dll) and the game executable: what each gives the other.

   The executable records the game's NVN command buffers (host/nvn_native.c) and calls a renderer through the rx_*
   functions (gpu/render/rx_nvn.h). With RX_BACKEND=vulkan host/rx_backend.c loads mkw_vk.dll, hands it the host
   functions below and routes the rx_* calls to the table it returns; otherwise they go to the WebGPU renderer in
   the executable. The DLL carries Eden's shader recompiler (gpu/vk/eden, SPIR-V back end) and sirit: their C++
   namespaces are the same as those of the WebGPU renderer's suyu copy, so the two live in different binaries. */
#pragma once

#include <stdint.h>

#include "../render/rx_nvn.h"   /* (the NVN objects and command records: C) */

#ifdef __cplusplus
extern "C" {
#endif

#define MKWVK_API_VERSION 1

/* (hookshot) the controls' names (host/nn_hid.c, host/pad_sdl.c, the pad_ and key_ lines of hookshot_options.ini):
   the Switch's buttons in nn::hid's bit order, and a gamepad's inputs by position, SDL's order (the bottom face
   button first: A on an Xbox pad, cross on a DualSense) */
enum { HOS_SW_A, HOS_SW_B, HOS_SW_X, HOS_SW_Y, HOS_SW_LS, HOS_SW_RS, HOS_SW_L, HOS_SW_R, HOS_SW_ZL, HOS_SW_ZR,
       HOS_SW_PLUS, HOS_SW_MINUS, HOS_SW_LEFT, HOS_SW_UP, HOS_SW_RIGHT, HOS_SW_DOWN, HOS_SW_N };
enum { HOS_PAD_SOUTH, HOS_PAD_EAST, HOS_PAD_WEST, HOS_PAD_NORTH, HOS_PAD_BACK, HOS_PAD_GUIDE, HOS_PAD_START,
       HOS_PAD_LSTICK, HOS_PAD_RSTICK, HOS_PAD_LSHOULDER, HOS_PAD_RSHOULDER, HOS_PAD_UP, HOS_PAD_DOWN, HOS_PAD_LEFT,
       HOS_PAD_RIGHT, HOS_PAD_LTRIGGER, HOS_PAD_RTRIGGER, HOS_PAD_N };

/* what the executable gives the back end */
typedef struct MkwVkHost {
    uint32_t version;   /* MKWVK_API_VERSION */
    void (*log)(const char *line);
    /* guest memory: the window, and GPU addresses (pools, below 1 TB the guest address itself) */
    uint8_t *(*guest_base)(void);
    uint64_t (*guest_size)(void);
    uint8_t *(*gpu_ptr)(uint64_t gpu, uint64_t *avail);
    uint64_t (*gpu_guest)(uint64_t gpu);
    int (*gpu_read)(uint64_t gpu, void *dst, uint64_t n);
    int (*gpu_write)(uint64_t gpu, const void *src, uint64_t n);
    /* guest pages written since the last call (the soft write watch): count, page numbers in *pages (freed with
       free_pages) */
    int64_t (*collect_written)(uint32_t **pages);
    void (*free_pages)(uint32_t *pages);
    void (*mark_written)(const void *p, uint64_t n);
    /* clocks: the guest tick (31.25 MHz from the start), host ns */
    uint64_t (*ticks)(void);
    int64_t (*now_ns)(void);
    /* NVN texture layouts (nvn_tex.c) */
    void (*tex_layout)(const nvn_texdesc *d, nvn_texlayout *o);
    uint64_t (*tex_level_offset)(const nvn_texdesc *d, unsigned level);
    int (*fmt_bpb)(int format, int *bw, int *bh);
    void (*tex_level_tiles)(const nvn_texdesc *d, unsigned level, unsigned *tile_h, unsigned *tile_d);
    uint32_t (*tex_linear_stride)(const nvn_texdesc *d);
    int (*depth_minus_w)(void);
    /* the memory pools ({guest start, size} each; returns the count, at most max) and a number that changes with
       them (host/nvdrv.c) */
    int (*gpu_pools)(uint64_t (*out)[2], int max);
    uint32_t (*maps_generation)(void);
    /* a number that changes with every mapping of NVN's virtual pools' pages (MapVirtual: sparse textures) */
    uint32_t (*vmap_generation)(void);
    /* the logical processors kept from the game's threads, for the replay thread (0: none) */
    uint64_t (*reserved_cpus)(void);
    /// The soft write watch's marks within [ranges[i][0], ranges[i][1]) only, read and cleared (free with free_pages)
    int64_t (*collect_written_in)(const uint64_t (*ranges)[2], int n, uint32_t **pages);
    /// Zeroed guest memory of the renderer's own (address space above the heap, never freed; 0 when none is left):
    /// what the NVN driver keeps in GPU memory of its own, such as its constant banks (mkw_eden.dll)
    uint64_t (*guest_alloc)(uint64_t size);
    /// (debugging) CPU writes to [guest, guest + size) logged with the writer's guest call stack (a page watch)
    void (*watch_guest_writes)(uint64_t guest, uint64_t size);
} MkwVkHost;

/* what the back end gives the executable: the rx_* functions (gpu/render/rx_nvn.h, rx_core) */
typedef struct MkwVkRenderer {
    uint32_t version;
    int (*start)(void *hwnd);
    void (*resize)(int width, int height);
    const char *(*describe)(void);
    void (*stats)(int64_t *draws, int64_t *skipped, int64_t *pipelines);
    void (*nvn_submit)(const uint64_t *handles, int n);
    uint64_t (*nvn_fence)(void);
    uint64_t (*nvn_pushed)(void);
    uint64_t (*nvn_replayed)(void);
    int (*nvn_wait)(uint64_t fence, uint64_t timeout_ns);
    void (*wait_writebacks_reading)(const void *p, uint64_t n);
    void (*nvn_finish)(void);
    void (*nvn_present)(const nvn_tex *t, const int crop[4]);
    void (*nvn_register_texture)(int id, const nvn_tex *t, const nvn_view *v);
    void (*nvn_register_sampler)(int id, const nvn_samp_par *p);
    void (*nvn_set_texture_pool_size)(int n);
    void (*nvn_texture_released)(const nvn_tex *t);
    void (*nvn_program_released)(const nvn_prog *p);
    void (*nvn_read_texels)(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                            uint64_t row_pitch, uint64_t image_pitch);
    void (*nvn_write_texels)(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                             const uint8_t *src, uint64_t row_pitch, uint64_t image_pitch);
    void (*nvn_invalidate)(uint64_t gpu, uint64_t size);
    void (*nvn_pool_created)(uint64_t gpu, uint64_t size);   /* a memory pool made (non-virtual): imported early */
    /* whether the GPU may use guest memory in [lo, hi) through an import (the host keeps such pages committed) */
    int (*guest_imported)(uint64_t lo, uint64_t hi);
    /* virtual pages [gpu, gpu + size) mapped now are about to be unmapped or remapped (the old mapping still in place):
       what the GPU wrote into textures there goes into their memory first */
    void (*nvn_vmap_changing)(uint64_t gpu, uint64_t size);
    /* a texture initialized (its storage: whether the CPU's reads of a range may need a texture's texels) */
    void (*nvn_texture_created)(const nvn_tex *t);
    /* the CPU set an event's value: in order with the submits (the GPU's waits on it) */
    void (*nvn_event_signal)(uint64_t gpu, uint32_t value);
    int (*option)(int id);   /* (hookshot, the Insert menu) 0: game time follows the clock, 1: the menu is open, 2: dynamic resolution, 3: its fixed scale in percent */
    /* a submit with its queue */
    void (*nvn_submit_q)(const uint64_t *handles, int n, uint64_t queue);
} MkwVkRenderer;

/* the DLL's one export: null when Vulkan is not usable (the executable then falls back to WebGPU) */
typedef const MkwVkRenderer *(*mkwvk_init_fn)(const MkwVkHost *host);
#ifdef MKWVK_BUILDING
#ifdef _WIN32
__declspec(dllexport) const MkwVkRenderer *mkwvk_init(const MkwVkHost *host);
#else
__attribute__((visibility("default"))) const MkwVkRenderer *mkwvk_init(const MkwVkHost *host);
#endif
#endif

#ifndef _WIN32
/* (linux) what start() is given in place of a window handle (host/posix/display_sdl.c): the window makes the
   surface (SDL_Vulkan_CreateSurface: X11 or Wayland) and says its size in pixels */
#define MKWVK_WINDOW_VERSION 1
typedef struct MkwVkWindow {
    uint32_t version;   /* MKWVK_WINDOW_VERSION */
    int (*create_surface)(void *window, void *instance, void *surface_out);   /* (VkInstance, VkSurfaceKHR *): 1 if made */
    void (*drawable_size)(void *window, int *width, int *height);
    void *window;
} MkwVkWindow;
#endif

#ifdef __cplusplus
}
#endif
