/* Which renderer the rx_* calls go to (gpu/vk/vk_api.h): the WebGPU one in the executable (gpu/render, its
   functions renamed wgr_* by rx_wgpu_names.h) or, with RX_BACKEND=vulkan, the Vulkan one in mkw_vk.dll beside the
   executable. Decided at rx_start, before any other rx_* call; a DLL that is missing or finds no usable Vulkan
   device leaves the WebGPU renderer in place (logged). */
#include "hos.h"
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../gpu/vk/vk_api.h"

/* the WebGPU renderer (gpu/render, renamed) */
int wgr_start(void *hwnd);
void wgr_resize(int width, int height);
const char *wgr_describe(void);
void wgr_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines);
void wgr_nvn_submit(const uint64_t *handles, int n);
uint64_t wgr_nvn_fence(void);
uint64_t wgr_nvn_pushed(void);
uint64_t wgr_nvn_replayed(void);
int wgr_nvn_wait(uint64_t fence, uint64_t timeout_ns);
void wgr_wait_writebacks_reading(const void *p, uint64_t n);
void wgr_nvn_finish(void);
void wgr_nvn_present(const nvn_tex *t, const int crop[4]);
void wgr_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v);
void wgr_nvn_register_sampler(int id, const nvn_samp_par *p);
void wgr_nvn_set_texture_pool_size(int n);
void wgr_nvn_texture_released(const nvn_tex *t);
void wgr_nvn_program_released(const nvn_prog *p);
void wgr_nvn_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                         uint64_t row, uint64_t img);
void wgr_nvn_write_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r,
                          const uint8_t *src, uint64_t row, uint64_t img);
void wgr_nvn_invalidate(uint64_t gpu, uint64_t size);

/* what the Vulkan back end is given */
uint8_t *hos_guest_base(void);
uint64_t hos_guest_size(void);
uint8_t *hos_gpu_ptr(uint64_t va, uint64_t *avail);
uint64_t nvn_gpu_guest(uint64_t gpu);
int nvn_gpu_read(uint64_t gpu, void *dst, uint64_t n);
int nvn_gpu_write(uint64_t gpu, const void *src, uint64_t n);
uint32_t *rx_collect_written(int64_t *count);   /* gpu/render/rx_core.cpp: the soft write watch's pages */
void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
int nvn_fmt_bpb(int format, int *bw, int *bh);
int nvn_depth_minus_w(void);
int hos_gpu_pools(uint64_t (*out)[2], int max);
uint32_t hos_gpu_maps_generation(void);
uint32_t nvn_vmap_generation(void);
uint64_t hos_reserved_cpus(void);   /* hos_thread.c */
extern float hos_aspect_ratio;      /* hos_main.c: the guest camera aspect applied at startup, or 0 */
uint32_t hos_pad_raw(void);         /* nn_hid.c: the gamepads and the controls, for the Insert menu */
int hos_binding(int set, int button, int slot);
void hos_bind(int set, int button, int slot, int code);
#if defined(A64_WRITE_TRACK) && !defined(__EMSCRIPTEN__)
void hos_mark_written(const void *p, size_t n);
#endif

static void host_log(const char *line) { hos_log("%s", line); }
#if defined(A64_WRITE_TRACK) && !defined(__EMSCRIPTEN__)
int hos_gpu_guest_ranges(uint64_t (*out)[2], int max);
/* The soft write watch's marks (rt/a64.h A64_MARK, hos_mark_written) over the GPU-visible ranges, read and cleared:
   the Vulkan back end's own collection (gpu/render's rx_collect_written works only with the WebGPU renderer started).
   One consumer: with Vulkan the WebGPU renderer never runs. */
static int64_t collect_in(const uint64_t (*ranges)[2], int nranges, uint32_t **pages) {
    size_t n = 0, cap = 4096;
    uint32_t *out = malloc(cap * sizeof *out);
    for (int r = 0; r < nranges; r++) {
        const uint64_t lo = ranges[r][0], hi = ranges[r][1] < hos_guest_size() ? ranges[r][1] : hos_guest_size();
        uint64_t p1 = (hi + 4095) >> 12;
        if (p1 > A64_DIRTY_PAGES) p1 = A64_DIRTY_PAGES;
        for (uint64_t p = lo >> 12; p < p1;) {
            if (!(p & 31) && p + 32 <= p1) {   /* (32 pages at a time while none is marked) */
                uint64_t w[4];
                memcpy(w, &a64_dirty[p], 32);
                if (!(w[0] | w[1] | w[2] | w[3])) {
                    p += 32;
                    continue;
                }
            }
            if (!(p & 7) && p + 8 <= p1) {   /* (eight pages a load while none is marked) */
                uint64_t w;
                memcpy(&w, &a64_dirty[p], 8);
                if (!w) {
                    p += 8;
                    continue;
                }
            }
            if (__atomic_load_n(&a64_dirty[p], __ATOMIC_RELAXED) && __atomic_exchange_n(&a64_dirty[p], 0, __ATOMIC_ACQUIRE)) {
                if (n == cap) out = realloc(out, (cap *= 2) * sizeof *out);
                out[n++] = (uint32_t)p;
            }
            p++;
        }
    }
    *pages = out;
    return (int64_t)n;
}
static int64_t host_collect(uint32_t **pages) {
    static uint64_t (*ranges)[2];
    if (!ranges) ranges = malloc(sizeof *ranges * 4096);
    return collect_in(ranges, hos_gpu_guest_ranges(ranges, 4096), pages);
}
/* (only within the given guest ranges: the submit-time capture of the mirrored pools, a small part of the 20 ms the
   whole collection takes) */
static int64_t host_collect_in(const uint64_t (*ranges)[2], int n, uint32_t **pages) { return collect_in(ranges, n, pages); }
#else
static int64_t host_collect(uint32_t **pages) {
    int64_t n = -1;
    *pages = rx_collect_written(&n);
    return n;
}
static int64_t host_collect_in(const uint64_t (*ranges)[2], int n, uint32_t **pages) { (void)ranges, (void)n; *pages = 0; return 0; }
#endif
static void host_free_pages(uint32_t *pages) { free(pages); }
static void host_mark_written(const void *p, uint64_t n) {
#if defined(A64_WRITE_TRACK) && !defined(__EMSCRIPTEN__)
    hos_mark_written(p, (size_t)n);
#else
    (void)p, (void)n;
#endif
}

/* (the renderer's own guest memory: the address space above the heap, as the game's own regions, committed and
   zeroed; the region allocator never hands it to the game) */
static uint64_t host_guest_alloc(uint64_t size) {
    const uint64_t a = hos_region_alloc(size, 0x10000);
    if (a) memset(hos_guest_base() + a, 0, size);
    return a;
}

void hos_watch_guest_writes(const uint8_t *data, uint64_t size);   /* nvn_native.c */
static void host_watch_guest_writes(uint64_t guest, uint64_t size) { hos_watch_guest_writes(hos_guest_base() + guest, size); }
static float host_aspect_ratio(void) { return hos_aspect_ratio; }

static const MkwVkHost vk_host = {
    MKWVK_API_VERSION, host_log, hos_guest_base, hos_guest_size, hos_gpu_ptr, nvn_gpu_guest, nvn_gpu_read,
    nvn_gpu_write, host_collect, host_free_pages, host_mark_written, hos_ticks, hos_now_ns, nvn_tex_layout,
    nvn_tex_level_offset, nvn_fmt_bpb, nvn_tex_level_tiles, nvn_tex_linear_stride, nvn_depth_minus_w, hos_gpu_pools,
    hos_gpu_maps_generation, nvn_vmap_generation, hos_reserved_cpus, host_collect_in, host_guest_alloc,
    host_watch_guest_writes, hos_pad_raw, hos_binding, hos_bind, host_aspect_ratio,
};

static const MkwVkRenderer *vk;   /* the Vulkan back end, when it was chosen and started */

/* The game registers its first textures and samplers before the window's thread starts the renderer (NVN is up
   first): those calls are kept here and given to the renderer rx_start chose, in order. Without that the Vulkan back
   end never knew the first ~0x60 texture slots, and lit geometry sampled them as its black dummy. */
typedef struct {
    int kind;   /* 0 texture registration, 1 sampler registration, 2 texture pool size, 3 texture finalized */
    int id;
    nvn_tex t;
    nvn_view v;
    int has_view;
    nvn_samp_par p;
} early_call;
static early_call *early;
static size_t n_early, cap_early;
static volatile LONG started;   /* rx_start chose (the calls go straight to the renderer from then on) */
static SRWLOCK early_lock = SRWLOCK_INIT;

static void forward(const early_call *c);
/* Keeps a call made before rx_start; 0 when the renderer is chosen already (the caller forwards it) */
static int keep_early(const early_call *c) {
    if (started) return 0;
    AcquireSRWLockExclusive(&early_lock);
    if (started) {
        ReleaseSRWLockExclusive(&early_lock);
        return 0;
    }
    if (n_early == cap_early) {
        cap_early = cap_early ? cap_early * 2 : 1024;
        early = (early_call *)realloc(early, cap_early * sizeof *early);
    }
    early[n_early++] = *c;
    ReleaseSRWLockExclusive(&early_lock);
    return 1;
}
/* rx_start: the kept calls given to the renderer chosen, then the gate opened */
static void flush_early(void) {
    AcquireSRWLockExclusive(&early_lock);
    for (size_t i = 0; i < n_early; i++) forward(&early[i]);
    if (n_early) hos_log("rx: %zu texture and sampler calls made before the renderer started, given to it", n_early);
    free(early);
    early = NULL, n_early = cap_early = 0;
    InterlockedExchange(&started, 1);
    ReleaseSRWLockExclusive(&early_lock);
}

/* dll: mkw_vk.dll (our Vulkan back end) or mkw_eden.dll (Eden's renderer driven natively, gpu/eden): one export */
static const MkwVkRenderer *load_vulkan(const char *dll) {
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = n ? (strrchr(path, '\\') ? strrchr(path, '\\') : strrchr(path, '/')) : NULL;   /* (linux: '/') */
    if (!slash) return NULL;
    strcpy(slash + 1, dll);
    HMODULE m = LoadLibraryA(path);
    if (!m) {
        hos_log("rx: RX_BACKEND=vulkan, but %s did not load (%lu): WebGPU", path, GetLastError());
        return NULL;
    }
    mkwvk_init_fn init = (mkwvk_init_fn)(void *)GetProcAddress(m, "mkwvk_init");
    const MkwVkRenderer *r = init ? init(&vk_host) : NULL;
    if (!r || r->version != MKWVK_API_VERSION) {
        hos_log("rx: the Vulkan back end did not start: WebGPU");
        return NULL;
    }
    return r;
}

int rx_start(void *hwnd) {
    const char *b = getenv("RX_BACKEND");
    const int eden = b && !_stricmp(b, "eden");
    if (!b || !_stricmp(b, "vulkan") || eden) {   /* (hookshot: Vulkan unless told otherwise; a started game has no RX_BACKEND) */
        const char *dll = eden ? "mkw_eden.dll" : "hookshot_vk.dll";
        const MkwVkRenderer *r = load_vulkan(dll);
        if (r && r->start(hwnd)) {
            vk = r;
            hos_log("rx: renderer: %s (%s)", eden ? "Eden's" : "Vulkan", dll);
            flush_early();
            return 1;
        }
        if (r) hos_log("rx: the Vulkan back end could not start on the window: WebGPU");
    }
    const int ok = wgr_start(hwnd);
    flush_early();
    return ok;
}
/* (hookshot) hookshot --prepare-shaders: the Vulkan back end started with no window and no game, which prepares
   every shader in its start (gpu/vk/vk_main.cpp started_up) and is then let go. 0 when there is no usable Vulkan */
int rx_prepare_shaders(void) {
    const MkwVkRenderer *r = load_vulkan("hookshot_vk.dll");
    return r && r->start(NULL);
}
void rx_resize(int width, int height) { vk ? vk->resize(width, height) : wgr_resize(width, height); }
const char *rx_describe(void) { return vk ? vk->describe() : wgr_describe(); }
void rx_stats(int64_t *draws, int64_t *skipped, int64_t *pipelines) {
    vk ? vk->stats(draws, skipped, pipelines) : wgr_stats(draws, skipped, pipelines);
}
void rx_nvn_submit(const uint64_t *handles, int n) { vk ? vk->nvn_submit(handles, n) : wgr_nvn_submit(handles, n); }
void rx_nvn_submit_q(const uint64_t *handles, int n, uint64_t queue) {
    if (vk && vk->nvn_submit_q) vk->nvn_submit_q(handles, n, queue);
    else rx_nvn_submit(handles, n);
}
uint64_t rx_nvn_fence(void) { return vk ? vk->nvn_fence() : wgr_nvn_fence(); }
uint64_t rx_nvn_fence_q(uint64_t queue) { return vk && vk->nvn_fence_q ? vk->nvn_fence_q(queue) : rx_nvn_fence(); }
uint64_t rx_nvn_pushed(void) { return vk ? vk->nvn_pushed() : wgr_nvn_pushed(); }
uint64_t rx_nvn_replayed(void) { return vk ? vk->nvn_replayed() : wgr_nvn_replayed(); }
int rx_nvn_wait(uint64_t fence, uint64_t timeout_ns) {
    return vk ? vk->nvn_wait(fence, timeout_ns) : wgr_nvn_wait(fence, timeout_ns);
}
void rx_wait_writebacks_reading(const void *p, uint64_t n) {
    vk ? vk->wait_writebacks_reading(p, n) : wgr_wait_writebacks_reading(p, n);
}
void rx_nvn_finish(void) { vk ? vk->nvn_finish() : wgr_nvn_finish(); }
void rx_nvn_present(const nvn_tex *t, const int crop[4]) { vk ? vk->nvn_present(t, crop) : wgr_nvn_present(t, crop); }
static void forward(const early_call *c) {
    switch (c->kind) {
    case 0: {
        const nvn_view *v = c->has_view ? &c->v : NULL;
        vk ? vk->nvn_register_texture(c->id, &c->t, v) : wgr_nvn_register_texture(c->id, &c->t, v);
        break;
    }
    case 1: vk ? vk->nvn_register_sampler(c->id, &c->p) : wgr_nvn_register_sampler(c->id, &c->p); break;
    case 2: vk ? vk->nvn_set_texture_pool_size(c->id) : wgr_nvn_set_texture_pool_size(c->id); break;
    case 3: vk ? vk->nvn_texture_released(&c->t) : wgr_nvn_texture_released(&c->t); break;
    case 4: if (vk && vk->nvn_texture_created) vk->nvn_texture_created(&c->t); break;
    }
}
void rx_nvn_register_texture(int id, const nvn_tex *t, const nvn_view *v) {
    if (!started && t) {
        early_call c = {0, id};
        c.t = *t;
        if (v) c.v = *v, c.has_view = 1;
        if (keep_early(&c)) return;
    }
    vk ? vk->nvn_register_texture(id, t, v) : wgr_nvn_register_texture(id, t, v);
}
void rx_nvn_register_sampler(int id, const nvn_samp_par *p) {
    if (!started && p) {
        early_call c = {1, id};
        c.p = *p;
        if (keep_early(&c)) return;
    }
    vk ? vk->nvn_register_sampler(id, p) : wgr_nvn_register_sampler(id, p);
}
void rx_nvn_set_texture_pool_size(int n) {
    if (!started) {
        early_call c = {2, n};
        if (keep_early(&c)) return;
    }
    vk ? vk->nvn_set_texture_pool_size(n) : wgr_nvn_set_texture_pool_size(n);
}
void rx_nvn_texture_released(const nvn_tex *t) {
    if (!started && t) {
        early_call c = {3, 0};
        c.t = *t;
        if (keep_early(&c)) return;
    }
    vk ? vk->nvn_texture_released(t) : wgr_nvn_texture_released(t);
}
void rx_nvn_texture_created(const nvn_tex *t) {
    if (!started && t) {
        early_call c = {4, 0};
        c.t = *t;
        if (keep_early(&c)) return;
    }
    if (vk && vk->nvn_texture_created) vk->nvn_texture_created(t);
}
void rx_nvn_program_released(const nvn_prog *p) { vk ? vk->nvn_program_released(p) : wgr_nvn_program_released(p); }
void rx_nvn_read_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst,
                        size_t row, size_t img) {
    vk ? vk->nvn_read_texels(t, level, layer0, r, dst, row, img) : wgr_nvn_read_texels(t, level, layer0, r, dst, row, img);
}
void rx_nvn_write_texels(const nvn_tex *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src,
                         size_t row, size_t img) {
    vk ? vk->nvn_write_texels(t, level, layer0, r, src, row, img)
       : wgr_nvn_write_texels(t, level, layer0, r, src, row, img);
}
void rx_nvn_invalidate(uint64_t gpu, uint64_t size) { vk ? vk->nvn_invalidate(gpu, size) : wgr_nvn_invalidate(gpu, size); }
int rx_guest_imported(uint64_t lo, uint64_t hi) {
    /* (mkw-recomp 16bc3bc: it takes the back end's pool lock, which the replay holds at times: no virtual core held) */
    const int away = hos_vcore_away();
    const int r = vk && vk->guest_imported ? vk->guest_imported(lo, hi) : 0;
    if (away) hos_vcore_back();
    return r;
}
/* (nvn_native.c: the CPU set an event's value) in order with the submits, for the GPU's waits on it */
int rx_option(int id, int otherwise) { return vk && vk->option ? vk->option(id) : otherwise; }
void rx_nvn_event_signal(uint64_t gpu, uint32_t value) {
    if (vk && vk->nvn_event_signal) vk->nvn_event_signal(gpu, value);
}
/* (nvn_native.c: virtual pages about to be unmapped or remapped) the Vulkan back end writes what the GPU drew there into
   their memory first */
void rx_nvn_vmap_changing(uint64_t gpu, uint64_t size) {
    /* (mkw-recomp 16bc3bc: it may wait for the replay and the GPU's texels: no virtual core held meanwhile) */
    const int away = hos_vcore_away();
    if (vk && vk->nvn_vmap_changing) vk->nvn_vmap_changing(gpu, size);
    if (away) hos_vcore_back();
}
void rx_nvn_pool_created(uint64_t gpu, uint64_t size) {
    if (vk && vk->nvn_pool_created) vk->nvn_pool_created(gpu, size);
}
