// Guest memory for the GPU: each NVN memory pool imported as host memory (VK_EXT_external_memory_host) with one
// buffer over it. The GPU reads the game's memory where it is and its writes land there: no mirrored copies, no
// write-backs, no upload streams. A GPU address (a guest address for the native pools) maps to (buffer, offset).
//
// Pools are imported the first time something on the GPU touches them, not all at once: MKW has ~2,400 pools at the
// title and importing each is a buffer, a memory object and a device address (tens of seconds taken at one submit,
// on the render thread, in the first frame). The table of pools is cheap to keep current (host->gpu_pools: two
// numbers each) and is refreshed when its generation changes, at a submit.
#include <cstring>
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <thread>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "vk_internal.h"
#include "vk_texture.h"   // (upload_command_buffer)
#include "import_hold.h"

namespace mkwvk {
namespace {

struct Pool {
    uint64_t lo = 0, hi = 0;   // guest range imported (aligned outwards)
    uint64_t range_lo = 0, range_hi = 0;   // (the game's pool range it was found for: pool_of's last hit)
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    // its VRAM mirror (see "the VRAM mirror" below)
    enum : uint8_t { MIRROR_NONE, MIRROR_WANTED, MIRROR_ON, MIRROR_REFUSED } mstate = MIRROR_NONE;
    VkBuffer mirror = VK_NULL_HANDLE;
    VmaAllocation mirror_alloc = nullptr;
    uint32_t synced_gen = 0;     // every page stamped up to this generation is in the mirror
    uint64_t synced_epoch = 0;   // the submission that last brought it up to date
    uint64_t sync_seg = 0;       // (and the parallel replay's segment whose uploads did)
    uint64_t last_value = 0;     // (the newest submission that bound it: its mirror is in use until that one is done)
    uint64_t direct_write_epoch = 0;   // (the part that last wrote it in guest memory, before it had a mirror)
    uint64_t retired_value = 0;  // (retired: the submission that may still use it)
    bool was_mirrored = false;   // (its mirror was given back: its pages stay in the captures, for the next one)
    // (EvilOOT) retired with its mirror on: the pool that took it in copies the GPU's contents from it when its own mirror is
    // made (mirror_make), and until then the mirror stays (mirrors_give_back)
    bool inherit_pending = false;
    uint64_t retired_epoch = 0;
};
uint64_t mem_rec_value = 0, mem_completed = 0;   // (the recording being made, and the GPU's progress)

struct GuestPool {
    uint64_t lo = 0, hi = 0;   // (aligned outwards the same way, so an import covers what the lookup found)
};

std::vector<GuestPool> guest_pools;   // by lo, from the host
std::atomic<uint64_t> guest_pools_gen{1};   // (bumped when that table is replaced: a span cached without the lock goes stale)
std::map<uint64_t, Pool> imported;    // by lo, made on first use
Pool* last_pool = nullptr;            // (the last pool_of hit: draws in a row bind buffers of the same pools)
extern Pool* pool_cache[4096];
void retire_pool(std::map<uint64_t, Pool>::iterator it);
Pool* pool_lookup(uint64_t guest, uint64_t size);
struct SrwMutex {   // (a slim reader/writer lock: half std::mutex's cost, taken for every buffer a draw binds)
    SRWLOCK l = SRWLOCK_INIT;
    void lock() { AcquireSRWLockExclusive(&l); }
    void unlock() { ReleaseSRWLockExclusive(&l); }
};
SrwMutex pools_mu;
uint32_t generation = ~0u;
uint64_t align = 4096;
uint64_t max_import_size = UINT64_MAX;
int failures = 0;
ImportHolds import_holds;          // (issue 218: a failed import is not tried again for ImportHolds::kGapMs)
std::mutex import_holds_mu;
thread_local bool import_cooling = false;
thread_local VkResult import_result = VK_SUCCESS;

int64_t import_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool import_on_hold(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(import_holds_mu);
    import_cooling = !import_holds.allow(lo, hi, import_now_ms());
    return import_cooling;
}
void import_remember_failure(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(import_holds_mu);
    import_holds.deny_for(lo, hi, import_now_ms());
}

constexpr VkBufferUsageFlags kUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

bool import_pool(uint64_t lo, uint64_t hi, Pool& p) {
    import_cooling = false;
    import_result = VK_SUCCESS;
    if (import_on_hold(lo, hi)) return false;   // (tried recently and failed: not again until the hold ends)
    if (hi <= lo || hi - lo > max_import_size) {
        logf("vk: import %#llx..%#llx exceeds the device's allocation/buffer limit", (unsigned long long)lo,
             (unsigned long long)hi);
        import_remember_failure(lo, hi);
        return false;
    }
    p.lo = lo;
    p.hi = hi;
    uint8_t* ptr = host->guest_base() + lo;
    const uint64_t bytes = hi - lo;
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    clk::time_point t1, t2, t3, t4;
    struct Report {   // (where an import's time goes: logged for slow ones, 12 times)
        const clk::time_point &a, &b, &c, &d, &e;
        ~Report() {
            if (e.time_since_epoch().count() == 0) return;
            const double total = std::chrono::duration<double, std::milli>(e - a).count();
            static int logged;
            if (total < 2.0 || logged++ >= 12) return;
            auto ms = [](clk::time_point x, clk::time_point y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
            logf("vk: import %.1f ms: commit+properties %.1f, buffer %.1f, allocate %.1f, bind+address %.1f", total, ms(a, b),
                 ms(b, c), ms(c, d), ms(d, e));
        }
    } report{t0, t1, t2, t3, t4};
    // (pages the game has not touched yet may be only reserved: committed now, as the pool's memory is usable; a range
    // committed already is left alone: the call took milliseconds in the game's huge reservation)
    {
        for (uint8_t* q = ptr; q < ptr + bytes;) {
            MEMORY_BASIC_INFORMATION mi{};
            if (VirtualQuery(q, &mi, sizeof mi) != sizeof mi) break;
            uint8_t* end = std::min((uint8_t*)mi.BaseAddress + mi.RegionSize, ptr + bytes);
            if (mi.State == MEM_RESERVE) VirtualAlloc(q, (size_t)(end - q), MEM_COMMIT, PAGE_READWRITE);
            q = end;
        }
    }
    // (the host allocations' memory type: the same for every pointer, asked once)
    static uint32_t type = UINT32_MAX;
    if (type == UINT32_MAX) {
        VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (vkGetMemoryHostPointerPropertiesEXT(dev.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr,
                                                &hp) != VK_SUCCESS) {
            import_remember_failure(lo, hi);
            return false;
        }
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)   // (host visible, coherent)
            if ((hp.memoryTypeBits >> i & 1) &&
                (dev.memory.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                type = i;
        if (type == UINT32_MAX) {
            import_remember_failure(lo, hi);
            return false;
        }
    }
    t1 = clk::now();
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &eb;
    bi.size = bytes;
    bi.usage = kUsage;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &p.buffer) != VK_SUCCESS) {
        import_remember_failure(lo, hi);
        return false;
    }
    t2 = clk::now();
    VkImportMemoryHostPointerInfoEXT im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    im.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    im.pHostPointer = ptr;
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    im.pNext = &fl;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &im;
    ai.allocationSize = bytes;
    ai.memoryTypeIndex = type;
    const VkResult allocated = vkAllocateMemory(dev.device, &ai, nullptr, &p.memory);
    if (allocated != VK_SUCCESS) {
        import_result = allocated;
        vkDestroyBuffer(dev.device, p.buffer, nullptr);
        p.buffer = VK_NULL_HANDLE;
        import_remember_failure(lo, hi);
        return false;
    }
    t3 = clk::now();
    vkBindBufferMemory(dev.device, p.buffer, p.memory, 0);
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = p.buffer;
    p.address = vkGetBufferDeviceAddress(dev.device, &da);
    if (getenv("RX_VK_LOG_ADDR"))   // (diagnostics: which buffer a device fault's address is in)
        logf("vk: addr import %#llx..%#llx at gpu %#llx..%#llx", (unsigned long long)lo, (unsigned long long)hi,
             (unsigned long long)p.address, (unsigned long long)(p.address + (hi - lo)));
    t4 = clk::now();
    return true;
}

} // namespace

// (hookshot) guest memory's page table, for the pointers shaders follow (spirv_emit_context.cpp): a page's device address, 0 for none. Written as
// pools come and go, before the submissions that can reach them
uint64_t* guest_pages = nullptr;
uint64_t guest_pages_address = 0;
VkBuffer guest_pages_buffer = VK_NULL_HANDLE;
// (hookshot, 11-07) A change to the table made while a submission is recorded goes into that submission's upload commands, after
// the mirror copies it records there, instead of into the table at once: the table is read by the GPU, and work submitted
// before (still running, or queued) read the new entries early. A pool's range zeroed at its merge read as unmapped (zero
// counts: the culling's foliage gone for a frame), and a mirror made but not yet filled read as garbage: the foliage
// culling 53caac5f looped on a count from it until Windows reset the driver (the device lost of ~1 start in 5, always
// after pool merges, 2 s after the GPU stopped). Off unless RX_VK_TABLE_DEFER=1 (see below)
struct TableRun { uint64_t first, count, address; };
thread_local std::vector<TableRun> table_pending;
thread_local bool table_defer = false;
void guest_pages_set(uint64_t lo, uint64_t hi, uint64_t address) {
    if (!guest_pages) return;
    // (off by default: measured 11-07, deferred changes gave 21 one-frame flashes and 17 fps in Kokiri against 6 and 23 written at
    // once. RX_VK_TABLE_DEFER=1: deferred, for the device-lost hunt)
    static const bool now = getenv("RX_VK_TABLE_DEFER") == nullptr;
    // (two windows, as spirv_emit_context.cpp reads them: 8 GB from 0x1000000000, then 4 GB from 0x7f00000000)
    const struct { uint64_t base, bytes, first; } windows[] = {{0x1000000000ull, 8ull << 30, 0}, {0x7f00000000ull, 4ull << 30, (8ull << 30) >> 12}};
    for (const auto& w : windows) {
        const uint64_t a0 = std::max(lo, w.base) & ~0xFFFull, a1 = std::min(hi, w.base + w.bytes);
        if (a0 >= a1) continue;
        const uint64_t first = w.first + ((a0 - w.base) >> 12), count = (a1 - a0 + 0xFFF) >> 12;
        const uint64_t at = address ? address + (a0 - lo) : 0;
        if (table_defer && !now) {
            table_pending.push_back({first, count, at});
            continue;
        }
        for (uint64_t i = 0; i < count; i++) guest_pages[first + i] = at ? at + (i << 12) : 0;
    }
}
void memory_table_begin() {
    if (!table_pending.empty()) {   // (left by a recording that never ended: written now)
        table_defer = false;
        for (const TableRun& r : table_pending)
            for (uint64_t i = 0; i < r.count; i++) guest_pages[r.first + i] = r.address ? r.address + (i << 12) : 0;
        table_pending.clear();
    }
    table_defer = guest_pages != nullptr;
}
// (at the end of the recording's uploads: the changes, the last one for each page, as transfers into the table; the
// upload command buffer's barriers order them after the work submitted before and before this submission's)
bool memory_table_flush() {
    table_defer = false;
    if (table_pending.empty()) return false;
    std::vector<std::pair<uint64_t, uint64_t>> pages;   // (page, value), in the order they were set
    for (const TableRun& r : table_pending)
        for (uint64_t i = 0; i < r.count; i++) pages.push_back({r.first + i, r.address ? r.address + (i << 12) : 0});
    table_pending.clear();
    std::stable_sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint64_t> run;
    VkCommandBuffer cmd = upload_command_buffer();
    uint64_t run_first = 0;
    const auto emit = [&] {
        if (!run.empty()) vkCmdUpdateBuffer(cmd, guest_pages_buffer, run_first * 8, run.size() * 8, run.data());
        run.clear();
    };
    for (size_t i = 0; i < pages.size(); i++) {
        if (i + 1 < pages.size() && pages[i + 1].first == pages[i].first) continue;   // (a later change of the same page wins)
        if (!run.empty() && (pages[i].first != run_first + run.size() || run.size() == 8192)) emit();
        if (run.empty()) run_first = pages[i].first;
        run.push_back(pages[i].second);
    }
    emit();
    return true;
}

uint64_t memory_guest_pages() {
    static std::mutex mu;
    std::lock_guard lk(mu);
    static bool tried = false;
    if (tried) return guest_pages_address;
    tried = true;
    if (getenv("RX_VK_NO_FOLLOW")) return 0;   // (on: Kokiri's lighting. The menus' blank text was the rule in graphics stages, now compute only)
    VkBuffer b = VK_NULL_HANDLE;
    VmaAllocation a = nullptr;
    void* mapped = nullptr;
    const uint64_t bytes = ((8ull << 30 >> 12) + (4ull << 30 >> 12)) * 8;
    if (!create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       true, b, a, &mapped) || !mapped) {
        logf("vk: no page table of guest memory: pointers in no bound buffer read as 0");
        return 0;
    }
    std::memset(mapped, 0, bytes);
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = b;
    guest_pages_address = vkGetBufferDeviceAddress(dev.device, &da);
    guest_pages_buffer = b;
    std::lock_guard pk(pools_mu);
    guest_pages = (uint64_t*)mapped;
    for (auto& kv : imported) guest_pages_set(kv.second.lo, kv.second.hi, kv.second.address);
    logf("vk: page table of guest memory at %#llx (%zu pools)", (unsigned long long)guest_pages_address, imported.size());
    return guest_pages_address;
}

bool memory_init() {
    if (!dev.external_memory_host) {
        logf("vk: no VK_EXT_external_memory_host: the guest memory cannot be imported (needed for now)");
        return false;
    }
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    hp.pNext = &p11;
    p11.pNext = &p13;
    p2.pNext = &hp;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    align = std::max<uint64_t>(hp.minImportedHostPointerAlignment, 4096);
    max_import_size = std::min(p11.maxMemoryAllocationSize, p13.maxBufferSize);
    logf("vk: guest memory imported in place (alignment %llu)", (unsigned long long)align);
    return true;
}

/// Refresh the guest's pool table (no Vulkan work: pools are imported as they are used)
void memory_sync() {
    static std::mutex sync_mu;   // (called from every thread that touches memory)
    std::lock_guard<std::mutex> sync_lk(sync_mu);
    const uint32_t g = host->maps_generation();
    if (g == generation) return;
    generation = g;
    static thread_local std::vector<std::array<uint64_t, 2>> list(8192);
    const int n = host->gpu_pools(reinterpret_cast<uint64_t(*)[2]>(list.data()), (int)list.size());
    std::vector<GuestPool> next;
    next.reserve(n > 0 ? (size_t)n : 0);
    for (int i = 0; i < n; i++) {
        if (!list[i][1]) continue;
        GuestPool p;
        p.lo = list[i][0] & ~(align - 1);
        p.hi = (list[i][0] + list[i][1] + align - 1) & ~(align - 1);
        next.push_back(p);
    }
    std::sort(next.begin(), next.end(), [](const GuestPool& a, const GuestPool& b) { return a.lo < b.lo; });
    std::lock_guard lk(pools_mu);
    guest_pools.swap(next);
    guest_pools_gen.fetch_add(1, std::memory_order_relaxed);
    last_pool = nullptr;
    std::fill(std::begin(pool_cache), std::end(pool_cache), nullptr);
    static int logged;
    if (logged++ < 8) logf("vk: %zu pools mapped", guest_pools.size());
}

// ---- page write generations ----
// Two levels over the guest's 512 GB (2^27 pages of 4 KB): chunks of 64K page generations made when a page of the
// chunk is first stamped, and the newest generation of each 2 MB region (512 pages), which lets a check skip a big
// texture's clean regions without looking at their pages. 0: never written.
namespace {
constexpr uint64_t kPageShift = 12, kChunkPages = 1u << 16, kRegionPages = 512;
constexpr uint64_t kPages = 1ull << 27;
std::vector<std::unique_ptr<uint32_t[]>> gen_chunks(kPages / kChunkPages);
std::vector<uint32_t> region_gen(kPages / kRegionPages);
// (and the generation every page of a region was written at, at least: a large stamp (a storage buffer a dispatch may
// write, hundreds of MB) marks whole regions, not 512 pages each; it took dispatches 8-150 ms)
std::vector<uint32_t> region_full(kPages / kRegionPages);
inline uint32_t load_u32(uint32_t& x) {
    return std::atomic_ref<uint32_t>(x).load(std::memory_order_acquire);
}
inline void store_u32(uint32_t& x, uint32_t v) {
    std::atomic_ref<uint32_t>(x).store(v, std::memory_order_release);
}
inline uint32_t page_gen(uint64_t p) {
    const auto& c = gen_chunks[p / kChunkPages];
    return std::max(c ? load_u32(c[p % kChunkPages]) : 0u, load_u32(region_full[p / kRegionPages]));
}
uint32_t gen_now = 1;
uint64_t pages_collected = 0;

void stamp_page(uint64_t p, uint32_t g) {
    if (p >= kPages) return;
    auto& c = gen_chunks[p / kChunkPages];
    if (!c) {
        c.reset(new uint32_t[kChunkPages]);
        std::fill(c.get(), c.get() + kChunkPages, 0u);
    }
    store_u32(c[p % kChunkPages], g);
    region_gen[p / kRegionPages] = g;
}
} // namespace

namespace { void cap_view_bump(); }   // (defined with cap_view_gen below, in this file's anonymous namespace)
bool capture_owns(uint64_t guest);
void memory_collect_written() {
    uint32_t* pages = nullptr;
    const int64_t n = host->collect_written(&pages);
    if (n > 0) {
        const uint32_t g = ++gen_now;
        for (int64_t i = 0; i < n; i++) {
            const uint64_t guest = (uint64_t)pages[i] << kPageShift;
            if (capture_owns(guest)) {   // (written after its submit's capture: the next capture takes it, as it is then)
                uint64_t avail = 0;
                if (const uint8_t* q = host->gpu_ptr(guest, &avail)) host->mark_written(q, 4096);
                continue;
            }
            stamp_page(pages[i], g);
        }
        pages_collected += (uint64_t)n;
        cap_view_bump();
    }
    if (pages) host->free_pages(pages);
}
uint32_t memory_generation() {
    return gen_now;
}
bool memory_written_since(uint64_t guest, uint64_t size, uint32_t gen) {
    if (!size) return false;
    const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
    for (uint64_t r = p0 / kRegionPages; r <= p1 / kRegionPages; r++) {
        if (region_gen[r] <= gen) continue;
        if (load_u32(region_full[r]) > gen) return true;
        const uint64_t a = std::max(p0, r * kRegionPages), b = std::min(p1, r * kRegionPages + kRegionPages - 1);
        for (uint64_t p = a; p <= b; p++)
            if (page_gen(p) > gen) return true;
    }
    return false;
}
void memory_written_runs(uint64_t guest, uint64_t size, uint32_t gen, std::vector<std::pair<uint64_t, uint64_t>>& out) {
    if (!size) return;
    const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
    for (uint64_t r = p0 / kRegionPages; r <= p1 / kRegionPages; r++) {
        if (region_gen[r] <= gen) continue;
        const uint64_t a = std::max(p0, r * kRegionPages), b = std::min(p1, r * kRegionPages + kRegionPages - 1);
        for (uint64_t p = a; p <= b; p++) {
            if (page_gen(p) <= gen) continue;
            const uint64_t lo = std::max(p << kPageShift, guest) - guest;
            const uint64_t hi = std::min((p + 1) << kPageShift, guest + size) - guest;
            if (!out.empty() && out.back().second == lo) out.back().second = hi;
            else out.push_back({lo, hi});
        }
    }
}
void memory_drop_shadow(uint64_t guest, uint64_t size);
// (a GPU write: RX_VK_GPU_PAGES=1 notes the pages, and the mirror syncs that copy the CPU's version of one over the
// mirror are counted (the GPU's bytes in that page replaced by what guest memory or a capture held: a page the CPU
// and the GPU both write). ~1M a race, no effect seen; off by default: its lock on every dirty page cost the replay)
std::mutex gpu_pages_mu;
std::unordered_map<uint32_t, uint32_t> gpu_pages;   // page -> the generation of the GPU's last write
int64_t st_sync_gpu_pages, st_sync_gpu_live;
bool gpu_pages_on() {
    static const bool v = getenv("RX_VK_GPU_PAGES") != nullptr;
    return v;
}
void memory_stamp_gpu(uint64_t guest, uint64_t size) {
    if (!size) return;
    memory_stamp(guest, size);
    if (!gpu_pages_on()) return;
    std::lock_guard lk(gpu_pages_mu);
    const uint64_t p0 = guest >> kPageShift, p1 = (guest + size - 1) >> kPageShift;
    if (p1 - p0 > 4096) return;   // (big copies: not tracked)
    for (uint64_t p = p0; p <= p1; p++) gpu_pages[(uint32_t)p] = gen_now;
    if (gpu_pages.size() > 2000000) gpu_pages.clear();
}
bool gpu_page(uint64_t page) {
    if (!gpu_pages_on()) return false;
    std::lock_guard lk(gpu_pages_mu);
    return gpu_pages.count((uint32_t)page) != 0;
}

void memory_stamp(uint64_t guest, uint64_t size) {
    if (!size) return;
    {   // (RX_VK_LOG_STAMP=<lo>:<hi> (hex guest addresses): the stamps there, 300 lines, with their callers)
        static const std::pair<uint64_t, uint64_t> range = [] {
            unsigned long long a = 0, b = 0;
            if (const char* e = getenv("RX_VK_LOG_STAMP")) sscanf(e, "%llx:%llx", &a, &b);
            return std::pair<uint64_t, uint64_t>{a, b};
        }();
        static int logged;
        if (range.second && guest < range.second && guest + size > range.first && logged++ < 300)
            logf("vk: stamp %#llx +%#llx (gen %u) from %p", (unsigned long long)guest, (unsigned long long)size, gen_now + 1,
                 __builtin_return_address(0));
    }
    memory_drop_shadow(guest, size);   // (a write the capture did not take: guest memory is newer than its copy)
    const uint32_t g = ++gen_now;
    const uint64_t p0 = guest >> kPageShift, p1 = std::min((guest + size - 1) >> kPageShift, kPages - 1);
    for (uint64_t p = p0; p <= p1;) {
        if (p % kRegionPages == 0 && p + kRegionPages - 1 <= p1) {   // (a whole region)
            store_u32(region_full[p / kRegionPages], g);
            region_gen[p / kRegionPages] = g;
            p += kRegionPages;
            continue;
        }
        stamp_page(p++, g);
    }
    cap_view_bump();   // (a replay read's cached page is this generation's answer, not the one from before the write)
}
uint64_t memory_pages_collected() {
    return pages_collected;
}

// ---- submit-time capture ----
// MKW writes some of a frame's data in place right after its submit (the console's GPU has read it by then). The
// replay records a submit later, and the mirrors were filled from guest memory as it was then: a frame's terrain
// tiles read the next one's constants and flashed as walls. The pages are now copied as they are at the submit.
namespace {
struct Capture {
    std::vector<uint32_t> pages;     // (written since the last submit)
    std::vector<uint32_t> copied;    // (those copied, in order)
    std::vector<uint32_t> slots;     // (and where each is in the buffer: 4 KB a slot, the page's index in pages)
    // (copied by the submitting thread straight into the buffer the GPU copies the mirrors' pages from, in cached host
    // memory: the replay's own copy of them into one was ~0.6 ms a submit once the game's small pools were captured)
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    uint64_t bsize = 0;
    size_t nbytes = 0;
};
std::mutex cap_mu;
std::deque<std::unique_ptr<Capture>> captures;
std::vector<std::unique_ptr<Capture>> spare;   // (taken ones, their buffers kept)
// ---- the captures' pages, for the mirror syncs ----
// A capture takes the pages written since the last submit; a pool its submission does not bind is synced at a later
// one, whose capture does not have those pages: copied from guest memory as it was at that replay, by when MKW had
// rewritten them for a later frame (it writes several submits ahead of the replay), a frame's objects drew with
// another one's transforms and instance data (the one-frame flashes). Each page a capture took is in a table by page:
// the capture (the generation its take stamped the pages with) and the page's slot in that capture's buffer. The
// entry holds while the page's generation is still that one (a later capture re-takes the page, a write the captures
// did not take (memory_stamp) makes guest memory newer) and the capture's buffer is kept (the last kKeep captures, and
// until the work that copies from it is done); a sync copies a dirty page from there, else from guest memory.
// (A flat table and generations: the hash maps' node allocations took ~0.7 ms a submit, the replay's.)
// (RX_VK_SYNC_LIVE=1: dirty pages not in their submission's capture read from guest memory, as before)
struct CapEntry { uint32_t gen, slot; };
constexpr uint64_t kCapChunkPages = 1u << 16;   // (a chunk of the table: 256 MB of guest memory, made when first used)
std::vector<std::unique_ptr<CapEntry[]>> cap_table;
inline CapEntry cap_entry(uint64_t page) {
    const uint64_t c = page / kCapChunkPages;
    if (c >= cap_table.size() || !cap_table[c]) return {0, 0};
    return cap_table[c][page % kCapChunkPages];
}
inline void cap_entry_set(uint64_t page, CapEntry e) {
    const uint64_t c = page / kCapChunkPages;
    if (c >= cap_table.size()) cap_table.resize(c + 1);
    if (!cap_table[c]) {
        cap_table[c].reset(new CapEntry[kCapChunkPages]);
        std::memset(cap_table[c].get(), 0, kCapChunkPages * sizeof(CapEntry));
    }
    cap_table[c][page % kCapChunkPages] = e;
}
// (each kept capture's copy on the GPU, by its generation)
struct CapBuf { uint64_t value; VkBuffer buffer; VmaAllocation alloc; uint64_t size; void* map; uint64_t last_use = 0; };
std::map<uint32_t, CapBuf> cap_bufs;
uint64_t cap_bytes = 0;
constexpr size_t kKeep = 24;
constexpr uint64_t kCapBytesMax = 768ull << 20;   // (and no more than this in them: loads capture hundreds of MB)
// (retired buffers, for the next captures: making and freeing a 5 MB buffer every submit cost the replay too)
struct FreeBuf { VkBuffer buffer; VmaAllocation alloc; void* map; uint64_t size; };
std::vector<FreeBuf> cap_free;
uint64_t cur_cap_value = 0;   // (the submission being recorded: what its syncs' copies wait for)
uint32_t cur_cap_gen = 0;     // (its capture's generation)
bool sync_live() {
    static const bool v = getenv("RX_VK_SYNC_LIVE") != nullptr;
    return v;
}
std::mutex cap_free_mu;   // (the submitting threads take buffers from cap_free, the replay gives them back)
void cap_buf_free(VkBuffer buffer, VmaAllocation alloc, void* map, uint64_t size) {
    std::lock_guard lk(cap_free_mu);
    if (cap_free.size() < 12 && size <= (64ull << 20)) cap_free.push_back({buffer, alloc, map, size});
    else destroy_buffer(buffer, alloc);
}
void cap_buf_retire(std::map<uint32_t, CapBuf>::iterator it) {
    cap_bytes -= it->second.size;
    cap_buf_free(it->second.buffer, it->second.alloc, it->second.map, it->second.size);
    cap_bufs.erase(it);
}
bool cap_buf_get(uint64_t need, VkBuffer& buffer, VmaAllocation& alloc, void*& map, uint64_t& size) {
    std::lock_guard lk(cap_free_mu);
    size_t best = cap_free.size();
    for (size_t i = 0; i < cap_free.size(); i++)
        if (cap_free[i].size >= need && (best == cap_free.size() || cap_free[i].size < cap_free[best].size)) best = i;
    if (best < cap_free.size()) {
        buffer = cap_free[best].buffer, alloc = cap_free[best].alloc, map = cap_free[best].map, size = cap_free[best].size;
        cap_free.erase(cap_free.begin() + (ptrdiff_t)best);
        return map != nullptr;
    }
    size = std::max<uint64_t>((need + (1u << 20) - 1) & ~((1ull << 20) - 1), 16ull << 20);
    map = nullptr;
    return create_buffer_cached(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, buffer, alloc, &map) && map;
}
std::vector<std::pair<uint64_t, uint64_t>> cap_ranges;   // (the mirrored pools the captures take, as of the last one)
std::mutex cap_ranges_mu;
uint64_t cap_ranges_ver = 1;   // (any of the captures' range lists changed: cap_ranges_mu)
// (and the game's pools up to RX_VK_CAPTURE_POOL_MB (default 64; 0: none) from when they are made, mirrored or not:
// pools are imported, and mirrored, at the replay's first binding of them, two presents behind the game, and the
// captures taken before that left them out. MK World moves its per-frame data to a new pool every so many frames:
// the new pool's mirror was filled and synced from the game's memory as it was at the replay, its next frames' data
// in it already, until a mirror made the slow way published the list again (seconds): shadows dark below a line,
// the picture cyan below one, in bursts every 30 or 60 frames. The bigger pools hold textures, whose storage the
// capture takes anyway.)
// (RX_VK_CAPTURE_EAGER=<n>: only for their first n captures. Off: pools made at the load are bound by the menus much
// later, and they flashed (30 in the main menu); and the pages the captures copy are almost all the mirrored pools'
// anyway)
std::vector<std::pair<uint64_t, uint64_t>> pool_cap_ranges;   // (merged; cap_ranges_mu)
uint32_t pool_cap_gen = 0;
uint64_t captures_made = 0, pool_cap_built = 0;              // (cap_ranges_mu)
std::unordered_map<uint64_t, uint64_t> pool_first_seen;     // (a pool's start -> the capture it was first seen at)
uint64_t capture_pool_max() {
    static const uint64_t v = (getenv("RX_VK_CAPTURE_POOL_MB") ? (uint64_t)atoll(getenv("RX_VK_CAPTURE_POOL_MB")) : 64) << 20;
    return v;
}
uint64_t eager_captures() {
    static const uint64_t v = getenv("RX_VK_CAPTURE_EAGER") ? (uint64_t)atoll(getenv("RX_VK_CAPTURE_EAGER")) : 0;
    return v;
}
void refresh_pool_ranges_locked() {   // (cap_ranges_mu held)
    if (!capture_pool_max()) return;
    const uint32_t g = host->maps_generation();
    if (g == pool_cap_gen && captures_made - pool_cap_built < 64) return;   // (and every 64: the old ones out)
    pool_cap_gen = g, pool_cap_built = captures_made;
    static std::vector<std::array<uint64_t, 2>> list(8192);
    const int n = host->gpu_pools(reinterpret_cast<uint64_t(*)[2]>(list.data()), (int)list.size());
    std::vector<std::pair<uint64_t, uint64_t>> v;
    std::unordered_map<uint64_t, uint64_t> seen;
    seen.reserve((size_t)std::max(n, 0));
    for (int i = 0; i < n; i++) {
        if (!list[i][1] || list[i][1] > capture_pool_max() || list[i][0] >= (1ull << 40)) continue;
        auto it = pool_first_seen.find(list[i][0]);
        const uint64_t first = it != pool_first_seen.end() ? it->second : captures_made;
        seen[list[i][0]] = first;
        if (!eager_captures() || captures_made - first < eager_captures())
            v.push_back({list[i][0] & ~4095ull, (list[i][0] + list[i][1] + 4095) & ~4095ull});
    }
    pool_first_seen.swap(seen);
    cap_ranges_ver++;
    std::sort(v.begin(), v.end());
    std::vector<std::pair<uint64_t, uint64_t>> m;
    for (const auto& r : v) {
        if (!m.empty() && r.first <= m.back().second) m.back().second = std::max(m.back().second, r.second);
        else m.push_back(r);
    }
    pool_cap_ranges.swap(m);
}
bool capture_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_CAPTURE");
        const bool v = e ? atoi(e) != 0 : true;
        logf("vk: submit-time capture of written pages %s", v ? "on" : "off");
        return v;
    }();
    return on;
}
std::atomic<bool> captures_seen{false};
std::atomic<uint64_t> cap_view_gen{1};   // (bumped when a capture is published or a page is stamped: cached reads go stale)
void cap_view_bump() { cap_view_gen.fetch_add(1, std::memory_order_release); }
double st_tk_all, st_tk_stamp, st_tk_copy;   // (memory_take_capture's parts, ms)   // (a capture was ever made: the replay thread's; without it nothing owns pages)

// ---- texture storage in the capture ----
// The textures' storage the replay uploads from joins the capture, and what the capture took of it is kept (the
// shadow) for the uploads to read instead of guest memory: the storage as it was at its submit. MKW writes its terrain's
// height tiles (68 x 68 RGBA16, at new addresses every frame) for a frame and then rewrites them right after the
// submit; uploaded when the replay recorded the submit, from guest memory as it was by then, a frame's tiles could take
// another's heights. Measured at a snowy spot it is rare: of 107,000 pages the uploads read from the copies, 13 had
// been rewritten since their submit. In a Grand Prix it is what flashed: ~100 one-frame flashes a minute (giant
// stretched shapes over most of the picture, black or fog-coloured bands, a 30-frame rhythm), 9-18 rewritten pages of
// 800,000, and none with it on. On by default now that the guarded copy holds (a call, not inlined). (A page the game
// wrote and then freed is still marked: the copy commits it again, zeros, on first touch (commit_on_touch), which is
// what the mirrors must see of it; clearing the marks at the free lost that, and a Grand Prix loaded to a black
// screen.) RX_VK_TEX_CAPTURE=0: off.
bool tex_capture_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_TEX_CAPTURE");
        const bool v = (e ? atoi(e) != 0 : true) && capture_on();
        logf("vk: texture storage in the submit-time capture %s", v ? "on" : "off");
        return v;
    }();
    return on;
}
std::vector<std::pair<uint64_t, uint64_t>> tex_cap_ranges;   // (texture storage the capture takes, merged; cap_ranges_mu)
std::mutex shadow_mu;   // (after pools_mu, before cap_ranges_mu)
struct ShadowPage {
    std::unique_ptr<uint8_t[]> b;
    uint64_t seq = 0;   // (the capture that took it)
};
std::unordered_map<uint32_t, ShadowPage> shadow;   // guest page -> its 4 KB as the last capture taken had it
std::unordered_map<uint32_t, uint32_t> shadow_regions;   // (2 MB region -> its pages in the shadow)
std::atomic<size_t> shadow_n{0};
uint64_t shadow_seq = 0;   // (captures taken)
// (256 MB: past it the pages no capture took in the last 64 go, the oldest first: those the game rewrites every frame
// stay; if none can go, a page is read from guest memory as before)
constexpr size_t kShadowMax = 65536;
std::map<uint64_t, std::pair<uint64_t, uint64_t>> tex_spans;   // lo -> (hi, the present a texture was last uploaded from it)
std::vector<std::pair<uint64_t, uint64_t>> tex_merged;          // (tex_spans merged; shadow_mu)
bool tex_spans_changed = false;
uint64_t tex_merged_bytes = 0;

bool in_tex_merged(uint64_t guest) {   // (shadow_mu held)
    auto it = std::upper_bound(tex_merged.begin(), tex_merged.end(), std::make_pair(guest, ~uint64_t(0)));
    return it != tex_merged.begin() && guest < std::prev(it)->second;
}
void shadow_erase(uint32_t page) {   // (shadow_mu held)
    if (!shadow.erase(page)) return;
    auto r = shadow_regions.find(page >> 9);
    if (r != shadow_regions.end() && --r->second == 0) shadow_regions.erase(r);
    shadow_n--;
}
void shadow_drop_range(uint64_t lo, uint64_t hi) {   // (shadow_mu held)
    if (hi <= lo) return;
    for (uint64_t r = lo >> 21; r <= (hi - 1) >> 21; r++) {
        if (!shadow_regions.count((uint32_t)r)) continue;
        const uint64_t p0 = std::max(lo >> 12, r << 9), p1 = std::min((hi + 4095) >> 12, (r + 1) << 9);
        for (uint64_t p = p0; p < p1; p++) shadow_erase((uint32_t)p);
    }
}
/// (the spans unused for 1800 presents dropped with their shadow pages; the rest published for the capture)
void publish_tex_spans(uint64_t frame) {
    std::lock_guard lk(shadow_mu);
    for (auto it = tex_spans.begin(); it != tex_spans.end();) {
        if (frame > it->second.second + 1800) {
            shadow_drop_range(it->first, it->second.first);
            it = tex_spans.erase(it);
            tex_spans_changed = true;
        } else {
            ++it;
        }
    }
    if (!tex_spans_changed) return;
    tex_spans_changed = false;
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (const auto& [lo, e] : tex_spans) {
        if (!v.empty() && lo <= v.back().second) v.back().second = std::max(v.back().second, e.first);
        else v.push_back({lo, e.first});
    }
    tex_merged = v;
    tex_merged_bytes = 0;
    for (const auto& r : v) tex_merged_bytes += r.second - r.first;
    std::lock_guard lk2(cap_ranges_mu);
    tex_cap_ranges.swap(v);
    cap_ranges_ver++;
}
} // namespace

void memory_note_texture_upload(uint64_t guest, uint64_t size) {
    if (!size || guest >= (1ull << 40) || !tex_capture_on()) return;
    uint64_t lo, hi;
    {   // (the pool it is in, when that is at most 256 MB; else the 2 MB regions it spans)
        std::lock_guard lk(pools_mu);
        auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), guest,
                                   [](uint64_t g, const GuestPool& p) { return g < p.lo; });
        if (it != guest_pools.begin() && guest >= std::prev(it)->lo && guest + size <= std::prev(it)->hi &&
            std::prev(it)->hi - std::prev(it)->lo <= (256ull << 20)) {
            lo = std::prev(it)->lo, hi = std::prev(it)->hi;
        } else {
            lo = guest & ~((2ull << 20) - 1), hi = (guest + size + (2ull << 20) - 1) & ~((2ull << 20) - 1);
        }
    }
    const uint64_t frame = texture_frame_no();
    std::lock_guard lk(shadow_mu);
    auto [it, fresh] = tex_spans.try_emplace(lo, hi, frame);
    if (!fresh) {
        it->second.second = frame;
        if (it->second.first >= hi) return;
        it->second.first = hi;
    }
    tex_spans_changed = true;
}

/// A replay read of one page can reuse what the last read of that page decided, while the page's write generation and
/// the capture table are the ones that decision was made from.
bool submitted_page_reuse(uint64_t page, uint32_t page_gen, uint64_t view, uint64_t cached_page, uint32_t cached_gen,
                          uint64_t cached_view) {
    return cached_view != 0 && page == cached_page && page_gen == cached_gen && view == cached_view;
}
struct SubmittedPage {
    uint64_t page = ~0ull;
    uint32_t pgen = 0;
    uint64_t view = 0;
    bool overlay = false;   // (bytes is the capture's copy of the page; otherwise guest memory stands)
    uint8_t bytes[4096]{};
};
thread_local SubmittedPage submitted_page;

bool memory_read_as_submitted(uint64_t gpu, void* dst, uint64_t n) {
    // (once a capture exists, every uniform word took pools_mu to ask whether its page was in one. The next words are
    // the same page: the answer is kept until a stamp or a new capture)
    if (n && capture_on() && captures_seen.load(std::memory_order_relaxed)) {
        const uint64_t guest = gpu < (1ull << 40) ? gpu : host->gpu_guest(gpu);
        if (guest) {
            const uint64_t page0 = guest >> kPageShift, page1 = (guest + n - 1) >> kPageShift;
            const uint64_t view = cap_view_gen.load(std::memory_order_acquire);
            if (page0 == page1 && submitted_page_reuse(page0, page_gen(page0), view, submitted_page.page,
                                                       submitted_page.pgen, submitted_page.view) &&
                submitted_page.overlay) {
                std::memcpy(dst, submitted_page.bytes + (guest & 4095), (size_t)n);
                return true;
            }
        }
    }
    if (!host->gpu_read(gpu, dst, n)) return false;
    if (!n || !capture_on() || !captures_seen.load(std::memory_order_relaxed)) return true;
    const uint64_t guest = gpu < (1ull << 40) ? gpu : host->gpu_guest(gpu);
    if (!guest) return true;
    const uint64_t page0 = guest >> kPageShift, page1 = (guest + n - 1) >> kPageShift;
    const uint64_t view = cap_view_gen.load(std::memory_order_acquire);
    if (page0 == page1 && submitted_page_reuse(page0, page_gen(page0), view, submitted_page.page, submitted_page.pgen,
                                               submitted_page.view))
        return true;
    static std::atomic<int64_t> reads{0}, pages_from{0};
    int64_t from = 0;
    {
        std::lock_guard lk(pools_mu);
        for (uint64_t page = guest >> kPageShift; page < (guest + n + 4095) >> kPageShift; page++) {
            const CapEntry e = cap_entry(page);
            if (!e.gen || e.gen != page_gen(page)) continue;
            auto b = cap_bufs.find(e.gen);
            if (b == cap_bufs.end() || !b->second.map) continue;
            const uint64_t a = std::max(page << kPageShift, guest), z = std::min((page + 1) << kPageShift, guest + n);
            std::memcpy((uint8_t*)dst + (a - guest), (const uint8_t*)b->second.map + (uint64_t)e.slot * 4096 + (a - (page << kPageShift)), z - a);
            from++;
        }
        if (page0 == page1) {
            submitted_page.page = page0;
            submitted_page.pgen = page_gen(page0);
            submitted_page.view = cap_view_gen.load(std::memory_order_relaxed);
            submitted_page.overlay = false;
            const CapEntry e = cap_entry(page0);
            if (e.gen && e.gen == submitted_page.pgen) {
                auto b = cap_bufs.find(e.gen);
                if (b != cap_bufs.end() && b->second.map) {
                    std::memcpy(submitted_page.bytes, (const uint8_t*)b->second.map + (uint64_t)e.slot * 4096, 4096);
                    submitted_page.overlay = true;
                }
            }
        }
    }
    pages_from += from;
    if (++reads % 20000 == 0)
        logf("vk: replay reads of guest memory as submitted: %lld (%lld pages from the captures)", (long long)reads.load(),
             (long long)pages_from.load());
    return true;
}

uint64_t memory_capture_view() { return cap_view_gen.load(std::memory_order_acquire); }
uint32_t memory_page_stamp(uint64_t gpu) {
    const uint64_t guest = gpu < (1ull << 40) ? gpu : host->gpu_guest(gpu);
    if (!guest) return 0;
    return page_gen(guest >> kPageShift);
}

bool memory_has_shadow(uint64_t guest, uint64_t n) {
    if (!n || !shadow_n.load(std::memory_order_relaxed)) return false;
    std::lock_guard lk(shadow_mu);
    for (uint64_t r = guest >> 21; r <= (guest + n - 1) >> 21; r++) {
        if (!shadow_regions.count((uint32_t)r)) continue;
        const uint64_t p0 = std::max(guest >> 12, r << 9), p1 = std::min((guest + n + 4095) >> 12, (r + 1) << 9);
        for (uint64_t p = p0; p < p1; p++)
            if (shadow.count((uint32_t)p)) return true;
    }
    return false;
}

/// (RX_VK_TEX_CAPTURE_OFF_FILE=<path> (A/B): while that file exists the uploads read guest memory as before)
bool overlay_off() {
    static const char* path = getenv("RX_VK_TEX_CAPTURE_OFF_FILE");
    if (!path) return false;
    static std::atomic<int64_t> next{0};
    static std::atomic<bool> off{false};
    const int64_t now = GetTickCount64();
    if (now >= next.load(std::memory_order_relaxed)) {
        next.store(now + 200, std::memory_order_relaxed);
        const bool v = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        if (v != off.exchange(v)) logf("vk: texture storage as submitted: %s (A/B file)", v ? "OFF" : "on");
    }
    return off.load(std::memory_order_relaxed);
}
std::atomic<int64_t> st_overlaid{0}, st_differed{0};   // (pages read from the shadow; of them, ones guest memory had changed)

const uint8_t* memory_as_submitted(uint64_t guest, const uint8_t* live, uint64_t n, std::vector<uint8_t>& scratch) {
    if (!n || !shadow_n.load(std::memory_order_relaxed) || overlay_off()) return live;
    std::lock_guard lk(shadow_mu);
    bool any = false;
    for (uint64_t r = guest >> 21; r <= (guest + n - 1) >> 21; r++) {
        if (!shadow_regions.count((uint32_t)r)) continue;
        const uint64_t p0 = std::max(guest >> 12, r << 9), p1 = std::min((guest + n + 4095) >> 12, (r + 1) << 9);
        for (uint64_t p = p0; p < p1; p++) {
            auto it = shadow.find((uint32_t)p);
            if (it == shadow.end()) continue;
            if (!any) {
                any = true;
                if (scratch.data() != live) scratch.assign(live, live + n);
            }
            const uint64_t a = std::max(p << 12, guest), b = std::min((p + 1) << 12, guest + n);
            st_overlaid++;
            if (std::memcmp(scratch.data() + (a - guest), it->second.b.get() + (a - (p << 12)), b - a)) st_differed++;
            std::memcpy(scratch.data() + (a - guest), it->second.b.get() + (a - (p << 12)), b - a);
        }
    }
    static std::atomic<int64_t> served{0};
    if (any && served.fetch_add(1) % 2000 == 0)
        logf("vk: texture storage read as submitted (%lld times; %lld pages from the copies, %lld of them rewritten in "
             "guest memory since their submit; %zu pages kept, %zu spans, %llu MB)", (long long)served.load(),
             (long long)st_overlaid.load(), (long long)st_differed.load(), shadow.size(), tex_spans.size(),
             (unsigned long long)(tex_merged_bytes >> 20));
    return any ? scratch.data() : live;
}

void memory_drop_shadow(uint64_t guest, uint64_t size) {
    if (!size || !shadow_n.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(shadow_mu);
    shadow_drop_range(guest, guest + size);
}

// ---- (EvilBoyce) GPU results by page: the submission that last wrote a page into guest memory ----
// For invalidate (vk_replay.cpp): a CPU read of GPU results waits until the submission that writes them there is done, as
// the console's coherent memory has them once that work is. Guest page number -> the recording's timeline value (write-backs
// from a mirror, and small bindings written in place). RX_VK_NO_READBACK_WAIT=1: none
namespace {
std::mutex wb_pages_mu;
std::unordered_map<uint32_t, uint64_t> wb_pages;
uint64_t wb_pruned_at = 0;
void wb_pages_note(uint64_t guest_lo, uint64_t guest_hi, uint64_t value) {
    static const bool off = getenv("RX_VK_NO_READBACK_WAIT") != nullptr;
    if (off || !value || guest_hi <= guest_lo) return;
    std::lock_guard lk(wb_pages_mu);
    for (uint64_t p = guest_lo >> kPageShift; p <= (guest_hi - 1) >> kPageShift; p++) wb_pages[(uint32_t)p] = value;
    if (wb_pages.size() > 200000 && mem_completed > wb_pruned_at) {   // (the ones done long ago: kept as "ever" with value 1)
        for (auto& kv : wb_pages)
            if (kv.second <= mem_completed) kv.second = 1;
        wb_pruned_at = mem_completed;
    }
}
} // namespace
uint64_t memory_writeback_value(uint64_t guest, uint64_t size, bool* ever) {
    uint64_t v = 0;
    if (ever) *ever = false;
    if (!size) return 0;
    std::lock_guard lk(wb_pages_mu);
    if (wb_pages.empty()) return 0;
    for (uint64_t p = guest >> kPageShift; p <= (guest + size - 1) >> kPageShift; p++) {
        auto it = wb_pages.find((uint32_t)p);
        if (it == wb_pages.end()) continue;
        if (ever) *ever = true;
        v = std::max(v, it->second);
    }
    return v;
}

/// A copy that fails instead of faulting on a page unmapped meanwhile (structured exception handling: the common case
/// costs a memcpy; ReadProcessMemory per page was ~2.3 us each). Not inlined, and the copy a call: without
/// -fasync-exceptions clang catches a fault in a __try block only across a call, and a 4 KB memcpy inlined into the
/// block (the per-page retry) faulted past the handler: the crash that kept the texture storage out of the capture.
void* (*volatile guarded_memcpy)(void*, const void*, size_t) = std::memcpy;
#ifdef _WIN32
__declspec(noinline) bool guarded_copy(void* dst, const void* src, size_t n) {
    __try {
        guarded_memcpy(dst, src, n);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}
#else
// (linux: the guest window is one mapping that stays readable, decommitted pages read as zero: nothing to fault on)
bool guarded_copy(void* dst, const void* src, size_t n) {
    guarded_memcpy(dst, src, n);
    return true;
}
#endif
// ---- a capture's copies: on the submitting thread and two helpers (RX_VK_CAPTURE_THREADS=<n>, 0-4): ~9 MB of the
// game's dynamic data a submit, four submits a frame, ~1.4 ms on the game's thread each with one ----
// (a page written and then unmapped before the submit faulted a plain copy: ReadProcessMemory fails on it instead.
// VirtualQuery per page took ~1 ms each in this address space: 22 ms a submit for 18 pages) (runs of adjacent pages
// in one copy each, page by page only where a run fails; page i into slot i)
struct CopyPart { const uint32_t* pages; int64_t i0, i1; uint8_t* out; uint8_t* ok; };
void copy_part(const CopyPart& w) {
    uint8_t* const base = (uint8_t*)host->guest_base();   // (guest addresses: no GPU translation per page)
    const uint64_t gsize = host->guest_size();
    for (int64_t i = w.i0; i < w.i1;) {
        const uint64_t ga = (uint64_t)w.pages[i] << kPageShift;
        if (ga + 4096 > gsize) { i++; continue; }
        int64_t j = i + 1;
        while (j < w.i1 && w.pages[j] == w.pages[j - 1] + 1 && ((uint64_t)w.pages[j] << kPageShift) + 4096 <= gsize) j++;
        if (guarded_copy(w.out + (uint64_t)i * 4096, base + ga, (SIZE_T)(j - i) * 4096)) {
            std::memset(w.ok + i, 1, (size_t)(j - i));
        } else {
            for (int64_t q = i; q < j; q++)
                w.ok[q] = guarded_copy(w.out + (uint64_t)q * 4096, base + ((uint64_t)w.pages[q] << kPageShift), 4096);
        }
        i = j;
    }
}
struct CopyHelpers {
    std::mutex mu;
    std::condition_variable cv, done;
    std::deque<CopyPart> parts;
    int pending = 0;
    bool started = false;
} copy_helpers;
void copy_helper_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk capture copy");
    for (;;) {
        CopyPart w;
        {
            std::unique_lock lk(copy_helpers.mu);
            copy_helpers.cv.wait(lk, [] { return !copy_helpers.parts.empty(); });
            w = copy_helpers.parts.front();
            copy_helpers.parts.pop_front();
        }
        copy_part(w);
        std::lock_guard lk(copy_helpers.mu);
        if (--copy_helpers.pending == 0) copy_helpers.done.notify_all();
    }
}
void copy_pages(const uint32_t* pages, int64_t n, uint8_t* out, uint8_t* ok) {   // (one capture at a time: submit_order_mu)
    static const int helpers = getenv("RX_VK_CAPTURE_THREADS") ? std::clamp(atoi(getenv("RX_VK_CAPTURE_THREADS")), 0, 4) : 2;
    if (!helpers || n < 128) return copy_part({pages, 0, n, out, ok});
    const int k = helpers + 1;
    {
        std::lock_guard lk(copy_helpers.mu);
        if (!copy_helpers.started) {
            copy_helpers.started = true;
            for (int i = 0; i < helpers; i++) std::thread(copy_helper_main).detach();
        }
        for (int i = 1; i < k; i++) copy_helpers.parts.push_back({pages, n * i / k, n * (i + 1) / k, out, ok});
        copy_helpers.pending += k - 1;
    }
    copy_helpers.cv.notify_all();
    copy_part({pages, 0, n / k, out, ok});
    std::unique_lock lk(copy_helpers.mu);
    copy_helpers.done.wait(lk, [] { return copy_helpers.pending == 0; });
}

void memory_capture_submit() {
    if (!capture_on()) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<Capture> c;
    {
        std::lock_guard lk(cap_mu);
        if (!spare.empty()) c = std::move(spare.back()), spare.pop_back();
    }
    if (!c) c = std::make_unique<Capture>();
    c->pages.clear(), c->copied.clear(), c->slots.clear(), c->nbytes = 0;
    if (c->map) cap_buf_free(c->buffer, c->alloc, c->map, c->bsize), c->buffer = VK_NULL_HANDLE, c->map = nullptr;
    // (only the pools mirrored in VRAM, the ones the mirrors are filled from, and only their part of the write marks:
    // every written page was taken at first (hundreds of MB a submit where the game streams its world), and the whole
    // collection, over every GPU range, is ~20 ms: 4 of them a frame on the game's thread made it crawl. The replay's
    // collection takes the rest, as before)
    // (the list the replay publishes when it makes mirrors: pools_mu here waited ~40 ms a submit for the replay)
    static thread_local std::vector<std::pair<uint64_t, uint64_t>> mirrored;
    {   // (and the texture storage the replay uploads from: see "texture storage in the capture"; and the small pools)
        std::lock_guard lk(cap_ranges_mu);
        captures_made++;
        refresh_pool_ranges_locked();
        static thread_local uint64_t ver = 0;
        if (ver != cap_ranges_ver) {   // (the three lists overlap: their union, sorted, once each time one changes)
            ver = cap_ranges_ver;
            std::vector<std::pair<uint64_t, uint64_t>> all = cap_ranges;
            all.insert(all.end(), tex_cap_ranges.begin(), tex_cap_ranges.end());
            all.insert(all.end(), pool_cap_ranges.begin(), pool_cap_ranges.end());
            std::sort(all.begin(), all.end());
            mirrored.clear();
            for (const auto& r : all) {
                if (!mirrored.empty() && r.first <= mirrored.back().second)
                    mirrored.back().second = std::max(mirrored.back().second, r.second);
                else
                    mirrored.push_back(r);
            }
        }
    }
    captures_seen.store(true, std::memory_order_relaxed);
    const auto t_lock = std::chrono::steady_clock::now();
    uint32_t* pages = nullptr;
    const int64_t n = mirrored.empty() || !host->collect_written_in ? 0
                    : host->collect_written_in(reinterpret_cast<const uint64_t(*)[2]>(mirrored.data()), (int)mirrored.size(), &pages);
    const auto t_scan = std::chrono::steady_clock::now();
    if (n > 0) {
        c->pages.assign(pages, pages + n);
        if ((uint64_t)n * 4096 <= (256ull << 20) &&   // (a load writes more: those are read later, as before)
            cap_buf_get((uint64_t)n * 4096, c->buffer, c->alloc, c->map, c->bsize)) {
            static thread_local std::vector<uint8_t> ok;
            ok.assign((size_t)n, 0);
            copy_pages(pages, n, (uint8_t*)c->map, ok.data());
            for (int64_t i = 0; i < n; i++)
                if (ok[i]) c->copied.push_back(pages[i]), c->slots.push_back((uint32_t)i);
            c->nbytes = (size_t)n * 4096;
            flush_mapped(c->alloc, c->nbytes);
        }
    }
    if (pages) host->free_pages(pages);
    {   // (what it cost, every 600 submits)
        static int64_t subs, written, copied;
        static double ms, ms_lock, ms_scan;
        static uint64_t span;
        subs++, written += n > 0 ? n : 0, copied += (int64_t)c->copied.size();
        const auto now = std::chrono::steady_clock::now();
        ms += std::chrono::duration<double, std::milli>(now - t0).count();
        ms_lock += std::chrono::duration<double, std::milli>(t_lock - t0).count();
        ms_scan += std::chrono::duration<double, std::milli>(t_scan - t_lock).count();
        if (subs % 600 == 0) {
            span = 0;
            for (const auto& r : mirrored) span += r.second - r.first;
            logf("vk: capture: a submit %.2f ms (lock %.2f, scan %.2f over %zu ranges, %llu MB), %.0f pages written, %.0f "
                 "copied (%.1f MB)", ms / 600, ms_lock / 600, ms_scan / 600, mirrored.size(),
                 (unsigned long long)(span >> 20), written / 600.0, copied / 600.0, copied * 4096.0 / 600 / 1048576);
            ms = ms_lock = ms_scan = 0, written = copied = 0;
        }
    }
    std::lock_guard lk(cap_mu);
    captures.push_back(std::move(c));
}

bool capture_owns(uint64_t guest) {
    // (no capture is ever made without the replay thread (RX_VK_ASYNC=0): the mirrored pools' writes were left for one
    // that never came, and the pools kept their first contents: a grey world)
    if (!capture_on() || !captures_seen.load(std::memory_order_relaxed)) return false;
    std::lock_guard lk(cap_ranges_mu);
    refresh_pool_ranges_locked();   // (a pool made since the last capture: the next one takes its pages, not this)
    auto in = [guest](const std::vector<std::pair<uint64_t, uint64_t>>& v) {
        auto it = std::upper_bound(v.begin(), v.end(), std::make_pair(guest, ~uint64_t(0)));
        return it != v.begin() && guest < std::prev(it)->second;
    };
    return in(cap_ranges) || in(tex_cap_ranges) || in(pool_cap_ranges);
}
void memory_recording_value(uint64_t value, uint64_t completed) {
    mem_rec_value = value;
    if (completed > mem_completed) mem_completed = completed;
}
bool memory_take_capture(uint64_t value, uint64_t completed) {
    if (completed > mem_completed) mem_completed = completed;
    if (!capture_on()) return false;
    std::unique_ptr<Capture> c;
    {
        std::lock_guard lk(cap_mu);
        if (captures.empty()) return false;
        c = std::move(captures.front());
        captures.pop_front();
    }
    const auto tk0 = std::chrono::steady_clock::now();
    std::lock_guard lk(pools_mu);
    cur_cap_value = value;
    cur_cap_gen = 0;
    // (captures past the last kKeep, or past the byte budget, retired once the work that read them is done)
    while (!cap_bufs.empty()) {
        auto oldest = cap_bufs.begin();
        const bool over = cap_bufs.size() > kKeep || cap_bytes > kCapBytesMax;
        if (!over || std::max(oldest->second.value, oldest->second.last_use) > completed) break;
        cap_buf_retire(oldest);
    }
    uint32_t g = 0;
    if (!c->pages.empty()) {
        g = ++gen_now;
        for (uint32_t p : c->pages) stamp_page(p, g);
        pages_collected += c->pages.size();
    }
    if (!c->pages.empty() && tex_capture_on()) {   // (the texture storage it took: the shadow, as of this submit)
        std::lock_guard sk(shadow_mu);
        shadow_seq++;
        if (!tex_merged.empty()) {
            // (every page written since the last capture: its old copy is stale; the ones copied now replace it)
            for (uint32_t p : c->pages)
                if (in_tex_merged((uint64_t)p << 12)) shadow_erase(p);
            bool swept = false;
            for (size_t i = 0; i < c->copied.size(); i++) {
                const uint32_t p = c->copied[i];
                if (!in_tex_merged((uint64_t)p << 12)) continue;
                if (shadow_n.load(std::memory_order_relaxed) >= kShadowMax) {
                    if (swept) break;
                    swept = true;
                    for (auto it = shadow.begin(); it != shadow.end();) {
                        if (it->second.seq + 64 >= shadow_seq) { ++it; continue; }
                        auto r = shadow_regions.find(it->first >> 9);
                        if (r != shadow_regions.end() && --r->second == 0) shadow_regions.erase(r);
                        it = shadow.erase(it);
                        shadow_n--;
                    }
                    if (shadow_n.load(std::memory_order_relaxed) >= kShadowMax) break;
                }
                ShadowPage& slot = shadow[p];
                if (!slot.b) {
                    slot.b.reset(new uint8_t[4096]);
                    shadow_regions[p >> 9]++;
                    shadow_n++;
                }
                slot.seq = shadow_seq;
                std::memcpy(slot.b.get(), (const uint8_t*)c->map + (uint64_t)c->slots[i] * 4096, 4096);
            }
        }
    }
    const auto tk1 = std::chrono::steady_clock::now();
    if (!c->copied.empty() && g && c->map) {   // (its buffer kept with the captures, for the mirror syncs)
        CapBuf b{value, c->buffer, c->alloc, c->bsize, c->map};
        c->buffer = VK_NULL_HANDLE, c->alloc = nullptr, c->map = nullptr, c->bsize = 0;
        cap_bufs.emplace(g, b);
        cap_bytes += b.size;
        cur_cap_gen = g;
        for (size_t i = 0; i < c->copied.size(); i++) cap_entry_set(c->copied[i], CapEntry{g, c->slots[i]});
    }
    cap_view_bump();
    if (c->map) cap_buf_free(c->buffer, c->alloc, c->map, c->bsize), c->buffer = VK_NULL_HANDLE, c->map = nullptr;
    {
        std::lock_guard lk(cap_mu);
        if (spare.size() < 8) spare.push_back(std::move(c));
    }
    {   // (every 600 takes: where a take's time went)
        const auto tk2 = std::chrono::steady_clock::now();
        static int64_t n;
        st_tk_stamp += std::chrono::duration<double, std::milli>(tk1 - tk0).count();
        st_tk_all += std::chrono::duration<double, std::milli>(tk2 - tk0).count();
        if (++n % 600 == 0) {
            logf("vk: a capture taken (ms): all %.3f; retires and stamps %.3f, bytes copied %.3f; %zu kept (%llu MB), %zu "
                 "spare buffers", st_tk_all / 600, st_tk_stamp / 600, st_tk_copy / 600, cap_bufs.size(),
                 (unsigned long long)(cap_bytes >> 20), cap_free.size());
            st_tk_all = st_tk_stamp = st_tk_copy = 0;
        }
    }
    return true;
}

namespace {

/// The imported pool holding [guest, guest + size) (imported now if it was not yet); null outside the pools.
/// pools_mu held.
Pool* pool_cache[4096];   // (by 64 KB page: the pools buffer bindings hit lately)
std::atomic<uint64_t> pools_gen{1};   // (bumped when a pool leaves the table: the recording threads' caches go stale)
struct TlPool {
    Pool* p;
    uint64_t gen;
};
thread_local TlPool tl_pools[1024];   // (a parallel replay thread's: by 64 KB page, read without the lock)

Pool* pool_of(uint64_t guest, uint64_t size) {
    if (!size || size > UINT64_MAX - guest) return nullptr;
    if (last_pool && guest >= last_pool->lo && guest + size <= last_pool->hi) return last_pool;
    Pool*& slot = pool_cache[(guest >> 16) & 4095];
    if (slot && guest >= slot->lo && guest + size <= slot->hi) return last_pool = slot;
    Pool* found = pool_lookup(guest, size);
    if (found) slot = found;
    return found;
}

// ---- pools imported in the background: an import took 5-45 ms (the driver locks the pages; the game's threads keep
// the address space busy committing theirs), and the game makes ~13 pools a second. The host says when the game makes
// one (memory_pool_created), a thread of ours imports it, and a lookup that finds one under way waits for it rather
// than importing it again. Imports made are handed over in a list (the lookup holds pools_mu, the thread never). ----
std::mutex bg_mu;
std::condition_variable bg_cv, bg_done_cv;
std::deque<std::pair<uint64_t, uint64_t>> bg_queue;
std::vector<uint64_t> bg_inflight;           // (lo of the ranges queued or being imported)
std::vector<std::pair<uint64_t, Pool>> bg_ready;
int64_t st_bg_imports, st_bg_waits;

void bg_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk imports");
    for (;;) {
        std::pair<uint64_t, uint64_t> r;
        {
            std::unique_lock lk(bg_mu);
            bg_cv.wait(lk, [] { return !bg_queue.empty(); });
            r = bg_queue.front();
            bg_queue.pop_front();
        }
        Pool p;
        const bool ok = import_pool(r.first, r.second, p);
        std::lock_guard lk(bg_mu);
        if (ok) bg_ready.push_back({r.first, p}), st_bg_imports++;
        std::erase(bg_inflight, r.first);
        bg_done_cv.notify_all();
    }
}

/// The imports the thread made, into the table (pools_mu held); one already there makes the new one redundant
void bg_drain() {
    std::vector<std::pair<uint64_t, Pool>> ready;
    {
        std::lock_guard lk(bg_mu);
        if (bg_ready.empty()) return;
        ready.swap(bg_ready);
    }
    for (auto& [lo, p] : ready) {
        auto imp = imported.find(lo);
        if (imp != imported.end() && imp->second.hi >= p.hi) {
            vkDestroyBuffer(dev.device, p.buffer, nullptr);
            vkFreeMemory(dev.device, p.memory, nullptr);
            continue;
        }
        if (imp != imported.end()) retire_pool(imp);
        imported.emplace(lo, p);
        guest_pages_set(p.lo, p.hi, p.address);
    }
}

Pool* pool_lookup(uint64_t guest, uint64_t size) {
    if (guest_pools.empty()) return nullptr;
    bg_drain();
    auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), guest,
                               [](uint64_t a, const GuestPool& p) { return a < p.lo; });
    if (it == guest_pools.begin()) return nullptr;
    --it;
    if (guest < it->lo || guest >= it->hi || !size || size > it->hi - guest) return nullptr;
    // (imports by 16 MB chunk (RX_VK_IMPORT_CHUNK_MB), not by pool: each import cost 5-45 ms in the driver, more as
    // their number grew, and the game makes ~13 pools a second; a chunk covers the pools after it at no cost)
    static const uint64_t chunk = (uint64_t)(getenv("RX_VK_IMPORT_CHUNK_MB") ? std::max(0, atoi(getenv("RX_VK_IMPORT_CHUNK_MB"))) : 16) << 20;
    const GuestPool range = chunk ? GuestPool{guest, guest + size} : *it;
    auto covering = [&]() {
        auto c = imported.upper_bound(range.lo);
        if (c == imported.begin()) return imported.end();
        --c;
        return c->second.lo <= range.lo && c->second.hi >= range.hi ? c : imported.end();
    };
    auto imp = chunk ? covering() : imported.find(range.lo);
    if (chunk && range.hi > UINT64_MAX - (chunk - 1)) return nullptr;
    const uint64_t want_lo = chunk ? range.lo / chunk * chunk : range.lo;
    const uint64_t want_hi = chunk ? (range.hi + chunk - 1) / chunk * chunk : range.hi;
    if (chunk && imp == imported.end()) imp = imported.find(want_lo);   // (a smaller one there: made again, larger)
    if (imp == imported.end() || imp->second.hi < range.hi) {   // (under way in the background: waited for)
        bool waited = false;
        {
            std::unique_lock lk(bg_mu);
            if (std::find(bg_inflight.begin(), bg_inflight.end(), range.lo) != bg_inflight.end()) {
                bg_done_cv.wait(lk, [&] {
                    return std::find(bg_inflight.begin(), bg_inflight.end(), range.lo) == bg_inflight.end();
                });
                waited = true;
                st_bg_waits++;
            }
        }
        if (waited) {
            bg_drain();
            imp = imported.find(range.lo);
        }
    }
    if (imp == imported.end() || imp->second.hi < range.hi || imp->second.lo > range.lo) {
        // (the new pool takes in every imported pool its range overlaps, and they are retired: the imported pools stay
        // disjoint. A range crossing a chunk's end made the chunk's pool again, larger, over the next chunk, whose own
        // pool stayed: two mirrors of the same memory. MKW's GPU culling wrote its instance data (a 2.4 MB storage
        // buffer) through one, the draws read it through the other, synced from guest memory before the submission's
        // write-back: the instances of a frame before or older, objects popping in and out, one-frame flashes.)
        uint64_t lo = want_lo, hi = std::max(want_hi, imp != imported.end() ? imp->second.hi : 0);
        std::vector<uint64_t> overlapped;
        for (bool grew = true; grew;) {
            grew = false;
            overlapped.clear();
            auto it = imported.lower_bound(hi);   // (the pools starting below hi, the last ones first)
            while (it != imported.begin()) {
                --it;
                if (it->second.hi <= lo) {
                    if (it->second.hi + (1ull << 30) < lo) break;   // (imports are at most 1 GB: none further down reaches lo)
                    continue;
                }
                overlapped.push_back(it->first);
                if (it->second.lo < lo) lo = it->second.lo, grew = true;
                if (it->second.hi > hi) hi = it->second.hi, grew = true;
            }
        }
        if (hi - lo > (1ull << 30)) {   // (no larger than 1 GB: then as before, the chunk's pool alone, overlaps logged)
            static int logged_big;
            if (logged_big++ < 8)
                logf("vk: pool %#llx..%#llx would take in %zu pools, %llu MB: left overlapping", (unsigned long long)lo,
                     (unsigned long long)hi, overlapped.size(), (unsigned long long)((hi - lo) >> 20));
            lo = want_lo, hi = std::max(want_hi, imp != imported.end() ? imp->second.hi : 0);
            overlapped.clear();
            if (imp != imported.end()) overlapped.push_back(imp->first);
        }
        Pool p;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = import_pool(lo, hi, p);
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            static double total_ms;
            static int n, logged;
            total_ms += ms, n++;
            if ((ms >= 2.0 && logged++ < 30) || n % 256 == 0)
                logf("vk: pool %#llx+%#llx imported in %.1f ms (%d imports, %.0f ms in all)", (unsigned long long)range.lo,
                     (unsigned long long)(range.hi - range.lo), ms, n, total_ms);
        }
        if (!ok) {
            if (!import_cooling && failures++ < 16) {
                if (import_result != VK_SUCCESS)
                    logf("vk: pool %#llx+%#llx not imported (VkResult %d)", (unsigned long long)range.lo,
                         (unsigned long long)(range.hi - range.lo), (int)import_result);
                else
                    logf("vk: pool %#llx+%#llx not imported", (unsigned long long)range.lo,
                         (unsigned long long)(range.hi - range.lo));
            }
            return nullptr;
        }
        for (uint64_t k : overlapped) {   // (the pools it took in)
            auto o = imported.find(k);
            if (o != imported.end()) retire_pool(o);
        }
        imp = imported.emplace(p.lo, p).first;
        guest_pages_set(p.lo, p.hi, p.address);
        if (overlapped.size() > 1) {
            static int logged_merge;
            if (logged_merge++ < 16)
                logf("vk: pool %#llx..%#llx took in %zu pools", (unsigned long long)p.lo, (unsigned long long)p.hi,
                     overlapped.size());
        }
        static int logged;
        if (logged++ < 8 || imported.size() % 256 == 0) logf("vk: %zu pools imported", imported.size());
    }
    imp->second.range_lo = range.lo, imp->second.range_hi = range.hi;
    return last_pool = &imp->second;
}

// ---- the VRAM mirror (after totk-pc-recomp's vk_memory.c) ----
// An imported pool keeps vertices, indices, constants and storage buffers in system memory, so a discrete GPU reads
// them over PCIe on every draw (TOTK measured ~110 ms of GPU time a frame on a 3090 Ti). A pool bound as a buffer
// gets a device-local copy, made at the start of the next submission, and from then on its bindings use the copy at
// the same offsets:
// - CPU -> mirror: at a submission's first use of a mirror, the pages the CPU wrote since its last sync (the page
//   write generations) are copied in, in the recording's upload command buffer (submitted right before it).
// - GPU -> guest: what the submission's shaders and copies may write into a mirror is copied back into the imported
//   pool at the submission's end, so the CPU (and everything that reads guest memory) sees it.
// RX_VK_MIRROR=0: no mirrors; RX_VK_MIRROR_MB: their budget (default 3072).
bool mirror_on() {
    static const int on = [] {
        const char* e = getenv("RX_VK_MIRROR");
        const bool discrete = dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        const int v = e ? atoi(e) != 0 : discrete;
        logf("vk: VRAM mirror of buffer pools %s", v ? "on" : "off");
        return v;
    }();
    return on != 0;
}
uint64_t mirror_budget() {
    // (a quarter of the card, at most the 3 GB it was for all: on a 6 GB laptop those and the textures were past what the card has)
    static const uint64_t b = [] {
        if (const char* e = getenv("RX_VK_MIRROR_MB")) return (uint64_t)atoi(e) << 20;
        uint64_t vram = 0;
        for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
            if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max(vram, (uint64_t)dev.memory.memoryHeaps[h].size);
        // (cards of 6 GB or less only: on 8 GB ones, a 3070 or a 4060, the game's memory read over PCIe instead left the GPU at a few
        // percent and the frame stuttering)
        if (vram && vram <= (6ull << 30) + (512ull << 20)) return vram / 8;
        return vram ? std::min<uint64_t>(vram / 4, 3072ull << 20) : (uint64_t)(3072ull << 20);
    }();
    return b;
}
uint64_t mirror_bytes = 0, mirror_epoch = 0;
bool mirror_upload = false;   // (a submission is being recorded: its upload command buffer takes the syncs)
std::vector<Pool*> mirror_wanted, mirror_synced;
struct WriteBack {
    Pool* pool;
    uint64_t lo, hi;   // offsets in the pool
};
std::vector<WriteBack> writebacks;
// (hookshot, 11-07) The counter reports the replay writes into guest memory on the CPU (vk_replay.cpp NOP_REPORT_COUNTER), by
// guest address: a mirrored pool's write-back copied the GPU's mirror over them at the end of the submission, the mirror's
// stale words over the replay's newer ones, and the next render condition read those: a lighting pass left out, the whole lit
// scene black for a frame (the one-frame dark flashes, every 15-30 frames; 17 in 40 s of Kokiri, 0 with mirrors off). They are
// left out of the write-back now. RX_VK_WB_REPORTS=1: written back with the rest, as before
std::mutex cpu_words_mu;
std::set<uint64_t> cpu_words;
} // namespace
void memory_cpu_owned(uint64_t guest, uint64_t size) {
    static const bool off = getenv("RX_VK_WB_REPORTS") != nullptr;
    if (off || !guest) return;
    std::lock_guard lk(cpu_words_mu);
    if (cpu_words.size() > 65536) cpu_words.clear();
    for (uint64_t a = guest & ~7ull; a < guest + size; a += 8) cpu_words.insert(a);
}
namespace {
// (a write-back's runs with the replay's own words cut out of them: runs of p, offsets in the pool)
void cut_cpu_words(const Pool& p, std::vector<VkBufferCopy>& runs) {
    std::lock_guard lk(cpu_words_mu);
    if (cpu_words.empty()) return;
    std::vector<VkBufferCopy> out;
    for (const VkBufferCopy& r : runs) {
        uint64_t at = r.srcOffset;
        const uint64_t end = r.srcOffset + r.size;
        for (auto it = cpu_words.lower_bound(p.lo + at); it != cpu_words.end() && *it < p.lo + end; ++it) {
            const uint64_t w = *it - p.lo;
            if (w > at) out.push_back({at, at, w - at});
            at = std::max(at, w + 8);
        }
        if (end > at) out.push_back({at, at, end - at});
    }
    runs.swap(out);
}

// ---- a pool remade larger (a 16 MB chunk the game's next pool runs past): the old one leaves the table but stays
// alive, as this submission's bindings, its mirror syncs and its write-backs still use it. Erasing it left those
// lists pointing into a freed map node: the write-back copied with garbage handles and the device was lost (the
// daytime title scene, whose pools cross chunk ends). Retired pools are kept (a few, never reused). ----
// (Extracted from the table, not erased: the node, and the Pool in it, stay where they were, so the mirror's lists and
// a parallel replay thread's pool cache still point at a live one.)
std::deque<std::map<uint64_t, Pool>::node_type> retired_pools;

void retire_pool(std::map<uint64_t, Pool>::iterator it) {
    Pool* old = &it->second;
    guest_pages_set(old->lo, old->hi, 0);
    old->retired_value = std::max(old->last_value, mem_rec_value);
    std::erase(mirror_wanted, old);   // (the new pool gets the mirror)
    if (old->mstate == Pool::MIRROR_WANTED) old->mstate = Pool::MIRROR_NONE;
    // (EvilOOT, 11-07) what the GPU wrote into its mirror and nowhere else (the culling's bare table entries past their first
    // 64 bytes, stores through the page table) was lost here: the new pool's mirror is filled from guest memory. Kokiri's
    // foliage records, written by the GPU while the scene loaded and its pools were remade larger, came out zero: every
    // count 0, the trees' leaf clusters never drawn. RX_VK_NO_INHERIT=1: as before
    static const bool no_inherit = getenv("RX_VK_NO_INHERIT") != nullptr;
    if (old->mstate == Pool::MIRROR_ON && !no_inherit) old->inherit_pending = true, old->retired_epoch = mirror_epoch;
    pools_gen++;
    retired_pools.push_back(imported.extract(it));
    last_pool = nullptr;
    std::fill(std::begin(pool_cache), std::end(pool_cache), nullptr);
    static int logged;
    if (logged++ < 8 || retired_pools.size() % 64 == 0)
        logf("vk: pool %#llx remade larger (%zu retired)", (unsigned long long)old->lo, retired_pools.size());
}
int64_t st_sync_bytes, st_sync_copies, st_wb_bytes, st_mirrors;
int64_t st_sync_live_pages, st_sync_cap_pages, st_sync_old_pages;   // (mirror syncs' dirty pages: live / this submit's capture / an earlier one's)
std::unordered_map<uint64_t, int64_t> live_by_pool;
std::unordered_map<uint64_t, std::pair<int64_t, int>> direct_by_pool;   // (bindings the GPU reads in guest memory: count, state)
int64_t live_seen = 0;   // (pool -> pages its syncs read live, since the last report)

void transfer_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

/// The pages of the pool the CPU wrote since its mirror's sync, copied in (the upload command buffer)
void mirror_sync(Pool& p) {
    p.synced_epoch = mirror_epoch;
    p.sync_seg = texture_segment();
    mirror_synced.push_back(&p);
    std::vector<VkBufferCopy> runs;   // (from guest memory)
    std::map<VkBuffer, std::vector<VkBufferCopy>> cap_runs;   // (from the captures' copies)
    uint64_t run_lo = 0, run_hi = 0;
    for (uint64_t page = p.lo >> kPageShift; page < (p.hi + 4095) >> kPageShift;) {
        if (page % kRegionPages == 0 && region_gen[page / kRegionPages] <= p.synced_gen) {
            page += kRegionPages;   // (a clean 2 MB region)
            continue;
        }
        const uint32_t pg = page_gen(page);
        const bool dirty = pg > p.synced_gen;
        if (dirty && gpu_page(page)) {   // (diagnostic: see memory_stamp_gpu)
            st_sync_gpu_pages++;
            const CapEntry e0 = cap_entry(page);
            const bool live = !(e0.gen && e0.gen == pg && cap_bufs.count(e0.gen));
            st_sync_gpu_live += live;
            static int logged;
            if (logged++ < 40)
                logf("vk: a mirror sync copies the CPU's version of page %#llx over the GPU's (pool %#llx, %s, page gen %u, "
                     "pool synced at %u, gen now %u)", (unsigned long long)(page << kPageShift), (unsigned long long)p.lo,
                     live ? "live" : "from a capture", pg, (unsigned)p.synced_gen, gen_now);
        }
        if (dirty && !sync_live()) {   // (as a capture took it, if the page was not written since)
            const CapEntry e = cap_entry(page);
            if (e.gen && e.gen == pg) {
                auto b = cap_bufs.find(e.gen);
                if (b != cap_bufs.end()) {
                    const uint64_t ga = std::max(page << kPageShift, p.lo), gb = std::min((page + 1) << kPageShift, p.hi);
                    cap_runs[b->second.buffer].push_back(
                        {(uint64_t)e.slot * 4096 + (ga - (page << kPageShift)), ga - p.lo, gb - ga});
                    b->second.last_use = std::max(b->second.last_use, cur_cap_value);
                    (e.gen == cur_cap_gen ? st_sync_cap_pages : st_sync_old_pages)++;
                    page++;
                    continue;
                }
            }
        }
        if (dirty) {
            st_sync_live_pages++;   // (dirty, not in this submit's capture: guest memory as it is now)
            live_by_pool[p.lo]++;
            {   // (RX_VK_LOG_LIVE=1: why, 300 lines: the page's generation, its entry's, the newest capture's)
                static const bool log_live = getenv("RX_VK_LOG_LIVE") != nullptr;
                static int logged;
                if (log_live && logged < 300 && (++live_seen % 97) == 0) {
                    logged++;
                    const CapEntry e = cap_entry(page);
                    logf("vk: live page %#llx: gen %u, entry gen %u (buffer %s), current capture %u, pool synced at %u, gen now %u",
                         (unsigned long long)(page << kPageShift), pg, e.gen,
                         e.gen ? (cap_bufs.count(e.gen) ? "kept" : "gone") : "-", cur_cap_gen, (unsigned)p.synced_gen, gen_now);
                }
            }
            const uint64_t a = std::max(page << kPageShift, p.lo) - p.lo, b = std::min((page + 1) << kPageShift, p.hi) - p.lo;
            if (run_hi == a && run_hi) run_hi = b;
            else {
                if (run_hi) runs.push_back({run_lo, run_lo, run_hi - run_lo});
                run_lo = a, run_hi = b;
            }
        }
        page++;
    }
    if (run_hi) runs.push_back({run_lo, run_lo, run_hi - run_lo});
    for (const auto& [buf, rs] : cap_runs) {
        vkCmdCopyBuffer(upload_command_buffer(), buf, p.mirror, (uint32_t)rs.size(), rs.data());
        for (const VkBufferCopy& r : rs) st_sync_bytes += (int64_t)r.size;
        st_sync_copies += (int64_t)rs.size();
    }
    if (runs.empty()) return;
    checkpoint_mark(upload_command_buffer(), p.lo, 'm');
    vkCmdCopyBuffer(upload_command_buffer(), p.buffer, p.mirror, (uint32_t)runs.size(), runs.data());
    for (const VkBufferCopy& r : runs) st_sync_bytes += (int64_t)r.size;
    st_sync_copies += (int64_t)runs.size();
}

} // namespace

BufferRef gpu_buffer(uint64_t gpu, uint64_t size) {
    uint64_t avail = 0;
    if (!host->gpu_ptr(gpu, &avail) || avail < size) return {};
    const uint64_t guest = host->gpu_guest(gpu);
    std::lock_guard lk(pools_mu);
    const Pool* p = pool_of(guest, size);
    if (!p) return {};
    return {p->buffer, guest - p->lo, size, p->address + (guest - p->lo)};
}

void memory_pool_created(uint64_t gpu, uint64_t size) {
    // (RX_VK_BG_IMPORT=1 only: imported eagerly, every pool the game made, most never used by the GPU, the thread's
    // page locking stalled the game's threads (3-7 fps); chunked imports at first use are the default)
    static const bool on = getenv("RX_VK_BG_IMPORT") != nullptr;
    if (!on || !size || gpu >= (1ull << 40) || !dev.external_memory_host) return;   // (virtual pools are not imported)
    const uint64_t lo = gpu & ~(align - 1), hi = (gpu + size + align - 1) & ~(align - 1);
    std::lock_guard lk(bg_mu);
    static bool started;
    if (!started) {
        started = true;
        std::thread(bg_main).detach();
    }
    if (std::find(bg_inflight.begin(), bg_inflight.end(), lo) != bg_inflight.end()) return;
    bg_inflight.push_back(lo);
    bg_queue.push_back({lo, hi});
    bg_cv.notify_one();
}

int memory_guest_imported(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(pools_mu);
    auto it = imported.upper_bound(hi);   // (pools by lo: those starting below hi, the last ones checked first)
    while (it != imported.begin()) {
        --it;
        if (it->second.hi > lo) return 1;
        if (it->second.hi + (1ull << 30) < lo) break;   // (imports are at most 1 GB: none further down reaches lo)
    }
    for (const auto& nh : retired_pools)
        if (nh.mapped().lo < hi && lo < nh.mapped().hi) return 1;
    return 0;
}

bool mirror_make(Pool& pool);   // (below)
/// The mirrored pools, for the submit-time capture (memory_capture_submit) (pools_mu held)
void publish_cap_ranges() {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (const auto& [lo, pool] : imported)
        if (pool.mstate == Pool::MIRROR_ON || pool.was_mirrored) v.push_back({pool.lo, pool.hi});
    std::lock_guard lk(cap_ranges_mu);
    cap_ranges.swap(v);
    cap_ranges_ver++;
}
// (EvilOOT, 11-07) the game's own memory pool holding a GPU address (below 1 TB, where the address is the guest's), not the
// import that happens to cover it: imports are 16 MB chunks around what was bound, and since upstream 1c7c9c7 only around
// the bound range, so the pool's end was a chunk's end: the pointer tables of the task and mesh stages (whole pools) and the
// bare entries stopped there, the grass's programs read their records 12-18 MB further on as 0 and culled every blade
static bool game_pool_of(uint64_t gpu, uint64_t& lo, uint64_t& hi) {   // (pools_mu held)
    if (gpu >= (1ull << 40) || guest_pools.empty()) return false;
    auto it = std::upper_bound(guest_pools.begin(), guest_pools.end(), gpu, [](uint64_t a, const GuestPool& p) { return a < p.lo; });
    if (it == guest_pools.begin()) return false;
    --it;
    if (gpu < it->lo || gpu >= it->hi) return false;
    lo = it->lo, hi = it->hi;
    return true;
}
/// True when gpu sits in [lo, hi) and that span was cached from the guest-pool table this generation still is.
bool guest_span_covers(uint64_t gpu, uint64_t lo, uint64_t hi, uint64_t cached_gen, uint64_t gen) {
    return cached_gen == gen && lo < hi && gpu >= lo && gpu < hi;
}
struct GuestSpan { uint64_t lo = 1, hi = 0, gen = 0; };
thread_local GuestSpan guest_span;
bool guest_span_cached(uint64_t gpu, uint64_t* lo, uint64_t* hi) {
    static const bool imported_only = getenv("RX_VK_POOL_END_IMPORTED") != nullptr;   // (A/B: the import's end, as before)
    if (imported_only || gpu >= (1ull << 40)) return false;
    const uint64_t gen = guest_pools_gen.load(std::memory_order_relaxed);
    if (!guest_span_covers(gpu, guest_span.lo, guest_span.hi, guest_span.gen, gen)) return false;
    *lo = guest_span.lo;
    *hi = guest_span.hi;
    return true;
}
void guest_span_store(uint64_t lo, uint64_t hi) {   // (pools_mu held: the generation matches the table just read)
    guest_span = {lo, hi, guest_pools_gen.load(std::memory_order_relaxed)};
}
uint64_t gpu_pool_end(uint64_t gpu) {
    uint64_t lo, hi;
    // (storage-buffer tables ask this per entry, and the entries of one table sit in one pool: the locked lookup of a
    // pool was ~8% of replay time before buffer binds cached it)
    if (guest_span_cached(gpu, &lo, &hi)) return hi;
    ParLock plk;   // (before pools_mu, always)
    std::lock_guard lk(pools_mu);
    static const bool imported_only = getenv("RX_VK_POOL_END_IMPORTED") != nullptr;   // (A/B: the import's end, as before)
    if (!imported_only && game_pool_of(gpu, lo, hi)) {
        guest_span_store(lo, hi);
        return hi;
    }
    const Pool* p = pool_of(gpu, 1);
    return p ? p->hi : 0;
}
uint64_t gpu_pool_start(uint64_t gpu) {   // (EvilOOT) where it begins
    uint64_t lo, hi;
    if (guest_span_cached(gpu, &lo, &hi)) return lo;
    ParLock plk;
    std::lock_guard lk(pools_mu);
    static const bool imported_only = getenv("RX_VK_POOL_END_IMPORTED") != nullptr;
    if (!imported_only && game_pool_of(gpu, lo, hi)) {
        guest_span_store(lo, hi);
        return lo;
    }
    const Pool* p = pool_of(gpu, 1);
    return p ? p->lo : 0;
}

// (pools leaving, a mirror made, or the next submission: a remembered read binding is looked up again)
uint64_t gpu_bind_stamp() {
    return pools_gen.load(std::memory_order_relaxed) ^ (mirror_epoch * 0x9E3779B97F4A7C15ull);
}

uint32_t bind_slot(uint64_t guest, uint64_t size) {
    // (a page-aligned range's low bits are zero: fold the rest in, or every one of them lands in one slot)
    uint64_t h = guest ^ (size * 0x9E3779B97F4A7C15ull);
    h *= 0x9E3779B97F4A7C15ull;
    h ^= h >> 32;
    h ^= h >> 16;
    return (uint32_t)(h & 255u);
}

bool bind_memo_hit(uint64_t guest, uint64_t size, uint64_t stamp, uint64_t cached_guest, uint64_t cached_size,
                   uint64_t cached_stamp, int has_buffer) {
    return has_buffer && size && guest == cached_guest && size == cached_size && stamp == cached_stamp;
}

struct BindMemo {
    uint64_t guest = 0, size = 0, stamp = 0;
    BufferRef ref{};
};
thread_local BindMemo bind_memo[256];

BufferRef gpu_buffer_bind(uint64_t gpu, uint64_t size, bool written) {
    if (written) texture_gpu_wrote(gpu, size);
    // (below 1 TB a GPU address is the guest address itself, and a pool holds the whole range or nothing: the host's
    // two translations are for the virtual pools above)
    uint64_t guest = gpu;
    if (gpu >= (1ull << 40)) {
        uint64_t avail = 0;
        if (!host->gpu_ptr(gpu, &avail) || avail < size) return {};
        guest = host->gpu_guest(gpu);
    }
    // (the same read, while that stamp still stands: the buffer and the offset are the ones found before. a miss
    // runs the lookup below, including a mirror sync, and remembers what it returned. a write is not remembered)
    auto remember = [&](BufferRef ref) {
        if (!written && ref.buffer != VK_NULL_HANDLE && size)
            bind_memo[bind_slot(guest, size)] = BindMemo{guest, size, gpu_bind_stamp(), ref};
        return ref;
    };
    if (!written) {
        const BindMemo& m = bind_memo[bind_slot(guest, size)];
        if (bind_memo_hit(guest, size, gpu_bind_stamp(), m.guest, m.size, m.stamp, m.ref.buffer != VK_NULL_HANDLE))
            return m.ref;
    }
    // (a read binding of a pool this thread found before and whose mirror is in use this submission or that has none,
    // without the lock: every thread's, the replay thread's too (each draw binds several uniform buffers: the pool's
    // lookup under the lock was ~8% of the replay's time in a race); the rest under it, as below)
    const bool par = par_active.load(std::memory_order_relaxed);
    TlPool& tl = tl_pools[(guest >> 16) & 1023];
    if (!written && tl.p && tl.gen == pools_gen.load(std::memory_order_relaxed) && guest >= tl.p->lo &&
        guest + size <= tl.p->hi) {
        Pool* p = tl.p;
        const uint64_t off = guest - p->lo;
        if (p->last_value < mem_rec_value) p->last_value = mem_rec_value;
        if (!mirror_on() || !mirror_upload || p->mstate == Pool::MIRROR_REFUSED || p->mstate == Pool::MIRROR_WANTED)
            return remember({p->buffer, off, size, 0});
        if (p->mstate == Pool::MIRROR_ON && p->synced_epoch == mirror_epoch && p->sync_seg <= texture_segment())
            return remember({p->mirror, off, size, 0});
    }
    ParLock plk;   // (before pools_mu, always: a mirror sync records into the shared upload command buffer)
    std::lock_guard lk(pools_mu);
    Pool* p = pool_of(guest, size);
    if (!p) return {};
    tl = {p, pools_gen.load(std::memory_order_relaxed)};
    (void)par;
    if (p->last_value < mem_rec_value) p->last_value = mem_rec_value;
    const uint64_t off = guest - p->lo;
    if (!mirror_on() || !mirror_upload || p->mstate == Pool::MIRROR_REFUSED) {
        if (mirror_on()) { auto& e = direct_by_pool[p->lo]; e.first++, e.second = mirror_upload ? (int)p->mstate : 9; }
        return remember({p->buffer, off, size, 0});
    }
    // (a pool's mirror is made at its first binding, in this part's uploads: bound in guest memory until the next
    // submission, the GPU read it there while the game's threads wrote it for their next frames, and a pass that drew
    // over the moment a write landed came out in two (shadows dark below a line for a frame, the line moving; a new
    // pool every so many frames as the per-frame data moves on). Not when this part already wrote it in guest memory:
    // the copy at the part's start would miss that. RX_VK_MIRROR_DEFER=1: at the next submission, as before)
    static const bool defer = getenv("RX_VK_MIRROR_DEFER") != nullptr;
    if (p->mstate == Pool::MIRROR_NONE && !defer && p->direct_write_epoch != mirror_epoch && mirror_make(*p)) {
        publish_cap_ranges();
        static int64_t made_now;
        if (++made_now % 200 == 1)
            logf("vk: pool %#llx (%llu MB): mirror made at its first binding (%lld so)", (unsigned long long)p->lo,
                 (unsigned long long)((p->hi - p->lo) >> 20), (long long)made_now);
    }
    if (p->mstate == Pool::MIRROR_NONE) {   // (made at the next submission's start)
        p->mstate = Pool::MIRROR_WANTED;
        mirror_wanted.push_back(p);
    }
    if (p->mstate != Pool::MIRROR_ON) {
        auto& e = direct_by_pool[p->lo];
        e.first++, e.second = (int)p->mstate;
        if (written) p->direct_write_epoch = mirror_epoch;
        if (written && size <= (1u << 20))   // (EvilBoyce: GPU results written in place; small ones: the readbacks are)
            wb_pages_note(p->lo + off, p->lo + std::min(off + size, p->hi - p->lo), mem_rec_value);
        return remember({p->buffer, off, size, 0});
    }
    if (p->synced_epoch != mirror_epoch || p->sync_seg > texture_segment()) mirror_sync(*p);
    if (written) writebacks.push_back({p, off, std::min(off + size, p->hi - p->lo)});
    return remember({p->mirror, off, size, 0});
}

// ---- mirrors given back ----
// A mirror was never freed: the retired pools' stayed (every pool remade larger, or taken in by a larger one, kept
// its VRAM), and pools the game no longer used kept theirs until the mirrors' budget was full; the pools after that
// stayed in system memory for good (the GPU read them over PCIe). Now a retired pool's mirror goes once the GPU is done
// with the submissions that bound it; a mirror not bound for kIdleEpochs submissions goes when a wanted one does not
// fit, or after kLongIdle; and a pool refused for the budget is tried again when it is next bound.
constexpr uint64_t kIdleEpochs = 600, kLongIdle = 6000;
int64_t st_mirrors_freed = 0;
uint64_t st_mirror_bytes_freed = 0;
// (hookshot) A freed mirror's buffer is destroyed only once the GPU is past everything recorded until then. Its pool's
// last_value counts the draws and dispatches that bound it, not the shaders that reach it through the guest page table:
// a culling or fog compute pass already running read a mirror destroyed under it (device lost in 53caac5f and ee7764a6,
// issues 132 and 195, AMD after a scene change). The table goes back to guest memory at once, as before
struct DeadMirror { VkBuffer buffer; VmaAllocation alloc; uint64_t value; };
std::deque<DeadMirror> dead_mirrors;
void reap_mirrors() {   // (pools_mu held)
    while (!dead_mirrors.empty() && dead_mirrors.front().value <= mem_completed) {
        if (getenv("RX_VK_LOG_ADDR")) {
            VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            da.buffer = dead_mirrors.front().buffer;
            logf("vk: addr mirror freed at gpu %#llx (value %llu, completed %llu)", (unsigned long long)vkGetBufferDeviceAddress(dev.device, &da),
                 (unsigned long long)dead_mirrors.front().value, (unsigned long long)mem_completed);
        }
        destroy_buffer(dead_mirrors.front().buffer, dead_mirrors.front().alloc);
        dead_mirrors.pop_front();
    }
}
void mirror_free(Pool& p) {
    p.was_mirrored = true;
    {   // (the table back to guest memory, for a pool still in use only: a retired one's range is another pool's now, and its
        // address there sent the foliage culling into freed memory, a device lost)
        const auto live = imported.find(p.lo);
        if (live != imported.end() && &live->second == &p) guest_pages_set(p.lo, p.hi, p.address);
    }
    dead_mirrors.push_back({p.mirror, p.mirror_alloc, std::max(mem_rec_value, p.last_value)});
    p.mirror = VK_NULL_HANDLE, p.mirror_alloc = nullptr;
    mirror_bytes -= p.hi - p.lo;
    st_mirror_bytes_freed += p.hi - p.lo;
    st_mirrors_freed++;
    p.mstate = Pool::MIRROR_NONE;
    pools_gen++;   // (a parallel replay thread's pool cache: looked up again)
}
void mirrors_give_back(uint64_t want_bytes) {   // (pools_mu held)
    static const bool keep = getenv("RX_VK_MIRROR_KEEP") != nullptr;   // (A/B: mirrors kept, as before)
    if (keep) return;
    for (auto& nh : retired_pools) {
        Pool& p = nh.mapped();
        // (EvilOOT: kept while the pool that took it in has not made its mirror, for 600 submissions at most)
        if (p.inherit_pending && mirror_epoch - p.retired_epoch > kIdleEpochs) p.inherit_pending = false;
        if (p.mstate == Pool::MIRROR_ON && p.retired_value <= mem_completed && !p.inherit_pending) mirror_free(p);
    }
    std::vector<Pool*> idle;
    for (auto& [lo, p] : imported)
        if (p.mstate == Pool::MIRROR_ON && mirror_epoch - p.synced_epoch > kIdleEpochs && p.last_value <= mem_completed)
            idle.push_back(&p);
    std::sort(idle.begin(), idle.end(), [](const Pool* a, const Pool* b) { return a->synced_epoch < b->synced_epoch; });
    for (Pool* p : idle)
        if (mirror_epoch - p->synced_epoch > kLongIdle || mirror_bytes + want_bytes > mirror_budget()) mirror_free(*p);
    static int64_t logged_at;
    if (st_mirrors_freed / 16 != logged_at) {
        logged_at = st_mirrors_freed / 16;
        logf("vk: %lld mirrors given back (%llu MB in all); %llu MB of mirrors now", (long long)st_mirrors_freed,
             (unsigned long long)(st_mirror_bytes_freed >> 20), (unsigned long long)(mirror_bytes >> 20));
    }
}

void memory_begin_submission() {
    std::lock_guard lk(pools_mu);
    reap_mirrors();
    mirror_epoch++;
    mirror_upload = true;
    mirror_synced.clear();
    writebacks.clear();
    if (tex_capture_on()) publish_tex_spans(texture_frame_no());
    if (!mirror_on()) return;
    {
        uint64_t want = 0;
        for (Pool* p : mirror_wanted) want += p->hi - p->lo;
        if (mirror_epoch % 64 == 0 || mirror_bytes + want > mirror_budget()) mirrors_give_back(want);
    }
    if (mirror_wanted.empty()) return;
    for (Pool* p : mirror_wanted) mirror_make(*p);
    mirror_wanted.clear();
    publish_cap_ranges();
    static uint64_t logged_at;
    if (mirror_bytes >> 28 != logged_at) {
        logged_at = mirror_bytes >> 28;
        logf("vk: %lld pools mirrored in VRAM (%llu MB)", (long long)st_mirrors, (unsigned long long)(mirror_bytes >> 20));
    }
}

/// (hookshot) Before a command buffer's parallel segments: every mirrored pool synced once, into the replay thread's
/// recording, ahead of all of them. Workers record out of order, so a pool a later segment bound first was synced in
/// that segment's upload, which runs after the earlier segments' work, and put the CPU's pages back over what the GPU
/// had just written in them (one frame of wrong lighting or exposure: 5x the flashes of serial replay). A segment
/// finds its pools synced now (synced_epoch, sync_seg 0). RX_VK_NO_PRESYNC=1: as before
void memory_sync_mirrors_before_segments() {
    static const bool off = getenv("RX_VK_NO_PRESYNC") != nullptr;
    if (off || !mirror_on() || !mirror_upload) return;
    std::lock_guard lk(pools_mu);
    for (auto& kv : imported) {
        Pool& p = kv.second;
        if (p.mstate != Pool::MIRROR_ON || p.synced_epoch == mirror_epoch) continue;
        mirror_sync(p);
        p.sync_seg = 0;
    }
}

/// A pool's mirror made, filled in the recording's upload command buffer: all of it, then the pages a capture holds
/// as they were at their submit (pools_mu held). False over the budget (tried again when next bound, as mirrors are
/// given back) or over 1 GB (never)
bool mirror_make(Pool& pool) {
    Pool* p = &pool;
    {
        const uint64_t n = p->hi - p->lo;
        // (hookshot: RX_VK_MIRROR_TIGHT=1, none while VRAM is 90% full. Off: 8 GB cards sit there all the time, and without their mirrors
        // the GPU read game memory over PCIe)
        static const bool tight_rule = getenv("RX_VK_MIRROR_TIGHT") != nullptr;
        uint64_t used = 0, allowed = 0;
        const bool tight = tight_rule && vram_usage(&used, &allowed) && allowed && used > allowed / 100 * 90;
        if (mirror_bytes + n > mirror_budget() || n > (1ull << 30) || tight) {
            p->mstate = n > (1ull << 30) ? Pool::MIRROR_REFUSED : Pool::MIRROR_NONE;
            static int logged;
            if (logged++ < 8)
                logf("vk: pool %#llx (%llu MB) stays in system memory: the mirrors have %llu MB of %llu", (unsigned long long)p->lo,
                     (unsigned long long)(n >> 20), (unsigned long long)(mirror_bytes >> 20),
                     (unsigned long long)(mirror_budget() >> 20));
            return false;
        }
        if (!create_device_buffer(n, kUsage, p->mirror, p->mirror_alloc)) {
            p->mstate = Pool::MIRROR_REFUSED;
            return false;
        }
        {   // (hookshot) the page table points at the mirror: what the GPU wrote is there, and guest memory has it only after the write-back
            VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            da.buffer = p->mirror;
            const uint64_t ma = vkGetBufferDeviceAddress(dev.device, &da);
            guest_pages_set(p->lo, p->hi, ma);
            if (getenv("RX_VK_LOG_ADDR"))
                logf("vk: addr mirror %#llx..%#llx at gpu %#llx..%#llx", (unsigned long long)p->lo, (unsigned long long)p->hi,
                     (unsigned long long)ma, (unsigned long long)(ma + n));
        }
        const VkBufferCopy all{0, 0, n};
        vkCmdCopyBuffer(upload_command_buffer(), p->buffer, p->mirror, 1, &all);   // (all of it, at this generation)
        {   // (EvilOOT) then the mirrors of the pools it took in, over their part: what the GPU wrote there and only there
            // (retire_pool). The captures' pages go over it after, as before: the CPU's newer writes win
            bool barrier_done = false;
            int inherited = 0;
            for (auto& nh : retired_pools) {
                Pool& r = nh.mapped();
                if (!r.inherit_pending || r.mstate != Pool::MIRROR_ON || !r.mirror || r.hi <= p->lo || r.lo >= p->hi) continue;
                if (!barrier_done) {   // (after the full copy, and after the shaders of earlier submissions wrote the old mirror)
                    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                    mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                    mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
                    vkCmdPipelineBarrier2(upload_command_buffer(), &di);
                    barrier_done = true;
                }
                const uint64_t a = std::max(r.lo, p->lo), b = std::min(r.hi, p->hi);
                const VkBufferCopy part{a - r.lo, a - p->lo, b - a};
                vkCmdCopyBuffer(upload_command_buffer(), r.mirror, p->mirror, 1, &part);
                r.inherit_pending = false;
                r.retired_value = std::max(r.retired_value, mem_rec_value);   // (in use by this upload until it is done)
                inherited++;
            }
            if (inherited) {
                static int logged;
                if (logged++ < 16)
                    logf("vk: pool %#llx..%#llx: mirror made with the GPU's contents of %d retired pool(s)", (unsigned long long)p->lo,
                         (unsigned long long)p->hi, inherited);
            }
        }
        {   // (then the pages a capture holds as they were at their submit: guest memory is the game's frame ahead)
            std::map<VkBuffer, std::vector<VkBufferCopy>> over;
            for (uint64_t page = p->lo >> kPageShift; page < (p->hi + 4095) >> kPageShift; page++) {
                const CapEntry e = cap_entry(page);
                if (!e.gen || e.gen != page_gen(page)) continue;
                auto b = cap_bufs.find(e.gen);
                if (b == cap_bufs.end()) continue;
                const uint64_t ga = std::max(page << kPageShift, p->lo), gb = std::min((page + 1) << kPageShift, p->hi);
                over[b->second.buffer].push_back({(uint64_t)e.slot * 4096 + (ga - (page << kPageShift)), ga - p->lo, gb - ga});
                b->second.last_use = std::max(b->second.last_use, mem_rec_value);
            }
            if (!over.empty()) {
                VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // (after the full copy)
                mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                di.memoryBarrierCount = 1;
                di.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(upload_command_buffer(), &di);
                for (const auto& [buf, rs] : over)
                    vkCmdCopyBuffer(upload_command_buffer(), buf, p->mirror, (uint32_t)rs.size(), rs.data());
            }
        }
        p->mstate = Pool::MIRROR_ON;
        pools_gen.fetch_add(1, std::memory_order_relaxed);   // (the binding's buffer changes: guest memory, then this mirror)
        p->synced_gen = gen_now;
        p->synced_epoch = mirror_epoch;
        p->sync_seg = texture_segment();
        mirror_synced.push_back(p);
        mirror_bytes += n;
        st_mirrors++;
        st_sync_bytes += (int64_t)n;
    }
    return true;
}

void memory_end_submission(VkCommandBuffer cmd) {
    std::lock_guard lk(pools_mu);
    {   // (every 600 submissions: how the mirror syncs' dirty pages were read)
        static int64_t n, l_live, l_cap;
        if (++n % 600 == 0) {
            static int64_t l_old;
            logf("vk: mirror syncs (600 submits): %lld dirty pages from their submits' captures, %lld from earlier ones', "
                 "%lld from guest memory as it was at the replay; %zu captures kept; %lld pages the GPU wrote copied over "
                 "(%lld live)", (long long)(st_sync_cap_pages - l_cap), (long long)(st_sync_old_pages - l_old),
                 (long long)(st_sync_live_pages - l_live), cap_bufs.size(), (long long)st_sync_gpu_pages,
                 (long long)st_sync_gpu_live);
            l_cap = st_sync_cap_pages, l_live = st_sync_live_pages, l_old = st_sync_old_pages;
            std::vector<std::pair<int64_t, uint64_t>> top;
            for (const auto& [lo, k] : live_by_pool) top.push_back({k, lo});
            std::sort(top.rbegin(), top.rend());
            std::string s;
            for (size_t i = 0; i < top.size() && i < 8; i++) {
                const auto it = imported.find(top[i].second);
                char b[160];
                bool captured = false;
                {
                    std::lock_guard ck(cap_ranges_mu);
                    for (const auto& r : cap_ranges) captured |= top[i].second >= r.first && top[i].second < r.second;
                }
                snprintf(b, sizeof b, " %#llx (%llu MB%s, state %d): %lld;", (unsigned long long)top[i].second,
                         it != imported.end() ? (unsigned long long)((it->second.hi - it->second.lo) >> 20) : 0ull,
                         captured ? ", captured" : ", NOT captured", it != imported.end() ? (int)it->second.mstate : -1,
                         (long long)top[i].first);
                s += b;
            }
            logf("vk:   read live, by pool:%s", s.c_str());
            live_by_pool.clear();
            // (and the bindings the GPU read in guest memory itself: pools with no mirror yet, or none (state 9: bound
            // outside a recording))
            std::vector<std::pair<int64_t, uint64_t>> dtop;
            for (const auto& [lo, e] : direct_by_pool) dtop.push_back({e.first, lo});
            std::sort(dtop.rbegin(), dtop.rend());
            std::string ds;
            for (size_t i = 0; i < dtop.size() && i < 8; i++) {
                const auto it = imported.find(dtop[i].second);
                char b[160];
                snprintf(b, sizeof b, " %#llx (%llu MB, state %d): %lld;", (unsigned long long)dtop[i].second,
                         it != imported.end() ? (unsigned long long)((it->second.hi - it->second.lo) >> 20) : 0ull,
                         direct_by_pool[dtop[i].second].second, (long long)dtop[i].first);
                ds += b;
            }
            if (!dtop.empty()) logf("vk:   bound in guest memory, by pool:%s", ds.c_str());
            direct_by_pool.clear();
        }
    }
    static const bool no_writeback = getenv("RX_VK_NO_WRITEBACK") != nullptr;   // (an experiment: none)
    if (no_writeback) writebacks.clear();
    if (!writebacks.empty()) {
        // per pool, the ranges merged, one copy each (mirror -> the guest's pool)
        std::sort(writebacks.begin(), writebacks.end(), [](const WriteBack& a, const WriteBack& b) {
            return a.pool != b.pool ? a.pool < b.pool : a.lo < b.lo;
        });
        transfer_barrier(cmd);
        std::vector<VkBufferCopy> runs;
        for (size_t i = 0; i < writebacks.size();) {
            Pool* p = writebacks[i].pool;
            runs.clear();
            for (; i < writebacks.size() && writebacks[i].pool == p; i++) {
                const WriteBack& w = writebacks[i];
                if (!runs.empty() && w.lo <= runs.back().srcOffset + runs.back().size)
                    runs.back().size = std::max(runs.back().size, w.hi - runs.back().srcOffset);
                else
                    runs.push_back({w.lo, w.lo, w.hi - w.lo});
            }
            cut_cpu_words(*p, runs);
            if (runs.empty()) continue;
            checkpoint_mark(cmd, p->lo, 'b');
            vkCmdCopyBuffer(cmd, p->mirror, p->buffer, (uint32_t)runs.size(), runs.data());
            for (const VkBufferCopy& r : runs) st_wb_bytes += (int64_t)r.size;
            for (const VkBufferCopy& r : runs) wb_pages_note(p->lo + r.dstOffset, p->lo + r.dstOffset + r.size, mem_rec_value);   // (EvilBoyce)
            {   // (hookshot) RX_VK_LOG_WRITEBACK=1: the pools written back the most, every 600 write-back submissions
                static const bool log_wb = getenv("RX_VK_LOG_WRITEBACK") != nullptr;
                if (log_wb) {
                    static std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> by_pool;   // lo -> bytes, runs
                    static int64_t subs;
                    auto& e = by_pool[p->lo];
                    for (const VkBufferCopy& r : runs) e.first += r.size;
                    e.second += runs.size();
                    if (i == writebacks.size() && ++subs % 600 == 0) {
                        std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> v(by_pool.begin(), by_pool.end());
                        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
                        std::string s;
                        for (size_t k = 0; k < v.size() && k < 8; k++) {
                            char b[120];
                            const auto it = imported.find(v[k].first);
                            snprintf(b, sizeof b, " %#llx (%llu MB pool): %.1f MB in %.0f runs;", (unsigned long long)v[k].first,
                                     it != imported.end() ? (unsigned long long)((it->second.hi - it->second.lo) >> 20) : 0ull,
                                     v[k].second.first / 600.0 / 1048576.0, v[k].second.second / 600.0);
                            s += b;
                        }
                        logf("vk: write-back a submission, by pool:%s", s.c_str());
                        by_pool.clear();
                    }
                }
            }
        }
        transfer_barrier(cmd);
        writebacks.clear();
    }
    // (what this submission synced is current as of now: the GPU's own writes, stamped while it was recorded, are
    // in the mirror and written back; CPU writes collected at the next submission get a newer generation)
    for (Pool* p : mirror_synced) p->synced_gen = gen_now;
    mirror_synced.clear();
    mirror_upload = false;
}

void memory_stats(int64_t* mirrors, uint64_t* mirror_mb, int64_t* sync_bytes, int64_t* sync_copies, int64_t* wb_bytes) {
    *mirrors = st_mirrors, *mirror_mb = mirror_bytes >> 20, *sync_bytes = st_sync_bytes, *sync_copies = st_sync_copies;
    *wb_bytes = st_wb_bytes;
}
} // namespace mkwvk

