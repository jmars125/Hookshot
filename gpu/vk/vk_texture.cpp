// Textures: one VkImage per texture storage (vk_texture.h). After totk-pc-recomp's runtime/vk_texture.c (MIT,
// Copyright (c) 2026 the totk-pc-recomp authors): the cache keyed by storage address plus descriptor, uploads on first
// use and when page write generations show the storage written, the GPU-owned rule (an image the GPU wrote is the
// content), every image in GENERAL layout, deferred destruction. The GPU ASTC decode is Eden's
// (video_core/host_shaders/astc_decoder.comp and renderer_vulkan/vk_compute_pass.cpp ASTCDecoderPass, GPL), the CPU
// unswizzle suyu's (video_core/textures/decoders.cpp, with the Switch 2 GOB swizzle).
//
// Formats share images by class: an image is made in the format first asked for (sRGB as its UNORM twin), with
// MUTABLE_FORMAT, so that a texture rendered as RGBA8 and sampled as RGBA8_SRGB, or written as R32UI and read as
// RGBA8, is one image. A class is the bytes per block for uncompressed colour, the format family for BC and ASTC,
// the format itself for depth / stencil.
#include <algorithm>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "vk_texture.h"
#include "vk_menu.h"
#include "vk_draw.h"
#include "vk_dump.h"

#include <span>

#include "video_core/textures/astc.h"
#include "video_core/textures/decoders.h"

#include "host_shaders/astc_decoder_comp_spv.h"
#include "host_shaders/bc_encoder_comp_spv.h"
#include "host_shaders/block_linear_unswizzle_2d_comp_spv.h"

namespace mkwvk {
namespace {

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

// ---- formats ----
std::mutex fmt_mu;
std::unordered_map<int, VkFormatProperties> fmt_cache;
VkFormatProperties format_props(VkFormat f) {
    std::lock_guard lk(fmt_mu);
    auto it = fmt_cache.find((int)f);
    if (it != fmt_cache.end()) return it->second;
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, f, &p);
    fmt_cache[(int)f] = p;
    return p;
}
bool is_bc(VkFormat f) {
    return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
}
bool is_astc(VkFormat f) {
    return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
}
/// The UNORM twin of an sRGB format (the image's format: storage writes and every view format of the class)
VkFormat linear_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
    default:
        if (is_astc(f) && ((f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1)) return (VkFormat)(f - 1);
        return f;
    }
}
/// 3-component formats are not image formats on NVIDIA: the image holds 4 (alpha one), the upload widens them
VkFormat widened(VkFormat f, int* from_bpb) {
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: *from_bpb = 3; return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: *from_bpb = 6; return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R32G32B32_SFLOAT: *from_bpb = 12; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VK_FORMAT_R32G32B32_UINT: *from_bpb = 12; return VK_FORMAT_R32G32B32A32_UINT;
    case VK_FORMAT_R32G32B32_SINT: *from_bpb = 12; return VK_FORMAT_R32G32B32A32_SINT;
    default: *from_bpb = 0; return f;
    }
}
/// A storage-capable view format of the image's class (sRGB views cannot be storage)
VkFormat storage_view_format(VkFormat f) {
    f = linear_twin(f);
    if (format_props(f).optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) return f;
    switch (f) {   // (same class, a format storage can be)
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R32_UINT;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R32G32_UINT;
    case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_SNORM: return VK_FORMAT_R16_UINT;
    case VK_FORMAT_R8_SNORM: return VK_FORMAT_R8_UINT;
    default: return f;
    }
}
/// The class an image format shares images by (see the top of the file)
uint32_t format_class(VkFormat vf, int nvn_format, int bpb, bool astc_decoded) {
    if (astc_decoded) return 0x30000u | (uint32_t)(nvn_format - 0x79) % 14;
    if (is_depth_format(vf)) return 0x10000u | (uint32_t)vf;
    if (is_bc(vf) || is_astc(vf)) return 0x20000u | (uint32_t)linear_twin(vf);
    int from = 0;
    widened(vf, &from);
    return 0x40000u | (uint32_t)bpb | (from ? 0x100u : 0u);
}

VkComponentSwizzle component(unsigned s) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    case NVN_TEXTURE_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_A;
    default: return VK_COMPONENT_SWIZZLE_IDENTITY;
    }
}

// ---- the cache ----
struct Key {
    uint64_t gpu;
    uint32_t target, width, height, depth, levels, samples, layout, cls;
    bool operator==(const Key& o) const {
        return gpu == o.gpu && target == o.target && width == o.width && height == o.height && depth == o.depth &&
               levels == o.levels && samples == o.samples && layout == o.layout && cls == o.cls;
    }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        uint64_t h = mix(k.gpu, k.target);
        h = mix(h, (uint64_t)k.width << 32 | k.height);
        h = mix(h, (uint64_t)k.depth << 32 | k.levels);
        h = mix(h, (uint64_t)k.samples << 40 | (uint64_t)k.layout << 20 | k.cls);
        return (size_t)h;
    }
};
std::unordered_map<Key, std::unique_ptr<Texture>, KeyHash> textures;
std::unordered_multimap<uint64_t, Texture*> by_gpu;
uint64_t frame_no = 1;
uint64_t total_bytes = 0;

// (RX_VK_LOG_REMADE=1) Textures evicted and made again within 600 frames, and those of them that were then sampled
// empty (storage the GPU rendered, not uploaded): what the eviction's minimum age below is measured by
struct Evicted {
    uint64_t frame;
    bool rendered;
};
std::mutex evicted_mu;
std::unordered_map<uint64_t, Evicted> evicted_at;   // (storage, size and format -> when it went)
std::unordered_map<const Texture*, uint64_t> remade;   // (made again: how many frames after it went)
uint64_t evicted_key(uint64_t gpu, uint32_t w, uint32_t h, uint32_t fmt) {
    return gpu ^ ((uint64_t)w << 48) ^ ((uint64_t)h << 32) ^ ((uint64_t)fmt << 20);
}
bool remade_log_on() {
    static const bool on = getenv("RX_VK_LOG_REMADE") && atoi(getenv("RX_VK_LOG_REMADE")) != 0;
    return on;
}

struct Dead {
    uint64_t value;
    VkImage image;
    VmaAllocation allocation;
    std::vector<VkImageView> views;
    std::vector<VkDeviceMemory> memory;   // (a sparse image's)
};
std::deque<Dead> dead;

// ---- the recording's upload command buffer ----
thread_local VkCommandBuffer up_cmd = VK_NULL_HANDLE;   // (each recording thread's: a parallel replay's segments
uint64_t rec_value = 0;                                   // record their own uploads)
thread_local bool up_used = false;
thread_local uint64_t tl_seg = 0;
uint32_t epoch = 0;
int64_t st_uploads, st_astc, st_bytes, st_gpu_unswizzled;

VkCommandBuffer upload_cmd() {
    if (!up_used) {
        up_used = true;
        full_barrier(up_cmd);   // (after the work submitted before: it may still read what is replaced)
    }
    return up_cmd;
}

// ---- staging: host-visible arenas, reused once the submission that filled them completed ----
struct Arena {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    uint8_t* mapped = nullptr;
    uint64_t size = 0, used = 0, value = 0;   // value: the last submission that reads it
};
std::vector<Arena> arenas;
constexpr uint64_t kArena = 64ull << 20;

struct Staging {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0;
    uint8_t* data = nullptr;
};
uint64_t completed_value = 0;
// (GPU writes into guest memory by buffers: GPU address -> (end, the submission that wrote it last))
std::map<uint64_t, std::pair<uint64_t, uint64_t>> gpu_wrote;
std::mutex gpu_wrote_mu;
}  // namespace
bool gpu_wrote_into(Texture& tex);
namespace {
Staging staging(uint64_t n, uint64_t align = 256) {
    for (Arena& a : arenas) {   // the current one first, then any whose readers completed
        if (a.value == rec_value) {
            const uint64_t at = (a.used + align - 1) & ~(align - 1);
            if (at + n <= a.size) {
                a.used = at + n;
                return {a.buffer, at, a.mapped + at};
            }
        }
    }
    for (Arena& a : arenas) {
        if (a.value != rec_value && a.value <= completed_value && a.size >= n) {
            a.value = rec_value, a.used = n;
            return {a.buffer, 0, a.mapped};
        }
    }
    Arena a;
    a.size = std::max(kArena, uint64_t((n + 0xFFFFF) & ~0xFFFFFull));
    void* m = nullptr;
    if (!create_buffer(a.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, a.buffer,
                       a.allocation, &m) || !m)
        return {};
    a.mapped = (uint8_t*)m;
    a.value = rec_value, a.used = n;
    arenas.push_back(a);
    static int logged;
    if (logged++ < 4 || arenas.size() % 16 == 0)
        logf("vk: staging arenas: %zu (%llu MB in this one)", arenas.size(), (unsigned long long)(a.size >> 20));
    return {a.buffer, 0, a.mapped};
}

// ---- the GPU ASTC decoder (Eden's) ----
struct AstcPush {
    uint32_t block_dims[2], layer_stride, block_size, x_shift, block_height, block_height_mask, native_tiling;
};
VkDescriptorSetLayout astc_set = VK_NULL_HANDLE;
VkPipelineLayout astc_layout = VK_NULL_HANDLE;
VkPipeline astc_pipe = VK_NULL_HANDLE;
bool astc_ready() {
    static int state = 0;   // 0 untried, 1 ready, -1 unavailable
    if (state) return state > 0;
    state = -1;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR || getenv("RX_VK_ASTC_CPU")) return false;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &astc_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcPush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &astc_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &astc_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof ASTC_DECODER_COMP_SPV;
    mi.pCode = ASTC_DECODER_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = astc_layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &astc_pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return false;
    state = 1;
    logf("vk: ASTC is decoded on the GPU (Eden's decoder, Switch 2 tiling)");
    return true;
}

VkImageView make_view(Texture* tex, VkImageViewType type, VkFormat f, VkImageAspectFlags aspect, uint32_t level0,
                      uint32_t nlevels, uint32_t layer0, uint32_t nlayers, const VkComponentMapping& cm,
                      VkImageUsageFlags usage) {
    if (!usage || (usage & tex->usage) != usage) return VK_NULL_HANDLE;
    if (is_bc(tex->format) && !is_bc(f) && (nlevels != 1 || nlayers != 1)) return VK_NULL_HANDLE;
    VkImageViewUsageCreateInfo uci{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    uci.usage = usage & tex->usage;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.pNext = &uci;
    vi.image = tex->image;
    vi.viewType = type;
    vi.format = f;
    vi.components = cm;
    vi.subresourceRange = {aspect, level0, nlevels, layer0, nlayers};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

// ---- big ASTC textures kept as BC3 (8 bits a texel, a quarter of RGBA8): decoded into a scratch image a few
// layers at a time, re-encoded by Eden's encoder (host_shaders/bc_encoder.comp: bounding box along the colours'
// covariance, inset; GPL, as the decoder) into a buffer, copied into the image. Decoded RGBA8 took 6 GB of VRAM at
// the title (the terrain's material arrays, 2048 x 2048 x 68, 1.45 GB each): more than the driver allowed, which
// paged VRAM out and evicted textures that were made and uploaded again. RX_VK_ASTC_RGBA8=1: all as RGBA8.
struct BcPush {
    uint32_t blocks_x, blocks_y, width, height, bc3, out_offset;
};
VkDescriptorSetLayout bc_set = VK_NULL_HANDLE;
VkPipelineLayout bc_layout = VK_NULL_HANDLE;
VkPipeline bc_pipe = VK_NULL_HANDLE;
constexpr uint32_t kBcDim = 2048, kBcLayers = 8;   // the scratch: the largest level a BC3 texture may have
VkImage bc_scratch = VK_NULL_HANDLE;
VmaAllocation bc_scratch_alloc = nullptr;
VkImageView bc_scratch_view = VK_NULL_HANDLE;
VkBuffer bc_out = VK_NULL_HANDLE;
VmaAllocation bc_out_alloc = nullptr;
bool bc_scratch_fresh = false;   // (its layout still to be set, in the next upload command buffer)

bool bc_ready() {
    static int state = 0;
    if (state) return state > 0;
    state = -1;
    if (!astc_ready() || getenv("RX_VK_ASTC_RGBA8") || !dev.features.textureCompressionBC) return false;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, VK_FORMAT_BC3_UNORM_BLOCK, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return false;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &bc_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BcPush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &bc_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &bc_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof BC_ENCODER_COMP_SPV;
    mi.pCode = BC_ENCODER_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = bc_layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &bc_pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return false;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {kBcDim, kBcDim, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = kBcLayers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!create_image(ii, bc_scratch, bc_scratch_alloc)) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = bc_scratch;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kBcLayers};
    if (vkCreateImageView(dev.device, &vi, nullptr, &bc_scratch_view) != VK_SUCCESS) return false;
    const uint64_t out = (uint64_t)(kBcDim / 4) * (kBcDim / 4) * kBcLayers * 16;
    if (!create_device_buffer(out, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, bc_out,
                              bc_out_alloc))
        return false;
    bc_scratch_fresh = true;
    state = 1;
    logf("vk: big ASTC textures are kept as BC3 (re-encoded on the GPU; RX_VK_ASTC_RGBA8=1: as RGBA8)");
    return true;
}

/// Encodes scratch layers [0, n) (a w x h region at its corner) and copies the blocks into dst's level and layers
void bc_encode_into(VkCommandBuffer cmd, uint32_t w, uint32_t h, uint32_t n, Texture& dst, uint32_t level,
                    uint32_t layer, int dx, int dy) {
    full_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc_pipe);
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, bc_scratch_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi{bc_out, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    wr[0].dstBinding = 0, wr[0].descriptorCount = 1, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wr[0].pImageInfo = &ii;
    wr[1].dstBinding = 1, wr[1].descriptorCount = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr[1].pBufferInfo = &bi;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc_layout, 0, 2, wr);
    descriptors_disturbed();
    const BcPush pc{(w + 3) / 4, (h + 3) / 4, w, h, 1, 0};
    vkCmdPushConstants(cmd, bc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (pc.blocks_x + 7) / 8, (pc.blocks_y + 7) / 8, n);
    full_barrier(cmd);
    const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, n}, {dx, dy, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(cmd, bc_out, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(cmd);   // (the scratch and the buffer are the next part's)
}

void bc_scratch_layout(VkCommandBuffer cmd) {
    if (!bc_scratch_fresh) return;
    bc_scratch_fresh = false;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = bc_scratch;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kBcLayers};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

/// The storage view of one level (every layer) the ASTC decoder writes
VkImageView astc_target_view(Texture* tex, uint32_t level) {
    const uint64_t key = mix(0xA57C, level);
    auto it = tex->views.find(key);
    if (it != tex->views.end()) return it->second;
    const VkImageView v = make_view(tex, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                    level, 1, 0, tex->layers, {}, VK_IMAGE_USAGE_STORAGE_BIT);
    tex->views[key] = v;
    return v;
}

// ---- uploads ----
struct LevelGeo {
    uint64_t offset;            // from the storage's start
    uint32_t w, h, d;           // texels
    uint32_t tw, th, td;        // blocks
    unsigned tile_h, tile_d;    // block height / depth (log2 GOBs)
    uint64_t swizzled;          // bytes of one layer of it in the storage
};
LevelGeo level_geo(const Texture& tex, uint32_t level) {   // (level: the image's; the storage's is level + skip)
    LevelGeo g{};
    const uint32_t gl = level + tex.skip;
    g.offset = host->tex_level_offset(&tex.d, gl);
    const uint32_t w0 = std::max(1u, tex.d.width);
    const uint32_t h0 = tex.type == VK_IMAGE_TYPE_1D ? 1 : std::max(1u, tex.d.height);
    const uint32_t d0 = tex.type == VK_IMAGE_TYPE_3D ? std::max(1u, tex.d.depth) : 1;
    g.w = std::max(1u, w0 >> gl);
    g.h = std::max(1u, h0 >> gl);
    g.d = std::max(1u, d0 >> gl);
    g.tw = (g.w + tex.block_w - 1) / tex.block_w;
    g.th = (g.h + tex.block_h - 1) / tex.block_h;
    g.td = g.d;
    host->tex_level_tiles(&tex.d, gl, &g.tile_h, &g.tile_d);
    const uint64_t gobs_x = ((uint64_t)g.tw * tex.bpb + 63) / 64;
    const uint64_t rows_y = (g.th + (8ull << g.tile_h) - 1) >> (3 + g.tile_h);
    const uint64_t slices = (g.td + (1ull << g.tile_d) - 1) >> g.tile_d;
    g.swizzled = gobs_x * rows_y * slices * (512ull << (g.tile_h + g.tile_d));
    return g;
}

/// The guest bytes [gpu, gpu + n) in host memory: in place when contiguous, else copied into scratch
const uint8_t* guest_bytes(uint64_t gpu, uint64_t n, std::vector<uint8_t>& scratch) {
    uint64_t avail = 0;
    const uint8_t* p = host->gpu_ptr(gpu, &avail);
    // (below 1 TB the GPU address is the guest's: the storage as it was at the submit being replayed, where the game has
    // rewritten it since (vk_memory.cpp, "texture storage in the capture"))
    if (p && avail >= n) return gpu < (1ull << 40) ? memory_as_submitted(gpu, p, n, scratch) : p;
    if (n >= (32u << 20)) {   // (a big sparse texture's pages, gathered on several threads: the course maps' 349 MB)
        scratch.resize(n);
        const uint32_t nt = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
        const uint64_t per = ((n + nt - 1) / nt + 0xFFFF) & ~0xFFFFull;
        std::vector<std::thread> pool;
        auto part = [&](uint32_t t) {
            const uint64_t a = (uint64_t)t * per, b = std::min(n, a + per);
            for (uint64_t at = a; at < b; at += 0x10000)
                if (!host->gpu_read(gpu + at, scratch.data() + at, std::min<uint64_t>(0x10000, b - at)))
                    std::memset(scratch.data() + at, 0, std::min<uint64_t>(0x10000, b - at));
        };
        for (uint32_t t = 1; t < nt; t++) pool.emplace_back(part, t);
        part(0);
        for (std::thread& t : pool) t.join();
        return scratch.data();
    }
    scratch.assign(n, 0);
    if (!host->gpu_read(gpu, scratch.data(), n)) {
        for (uint64_t at = 0; at < n; at += 0x10000) host->gpu_read(gpu + at, scratch.data() + at, std::min<uint64_t>(0x10000, n - at));
    }
    return scratch.data();
}

void widen_rows(uint8_t* dst, const uint8_t* src, size_t texels, int from, int to) {
    const int comp = from / 3;
    for (size_t i = 0; i < texels; i++) {
        std::memcpy(dst + i * to, src + i * from, from);
        uint8_t* a = dst + i * to + from;
        if (comp == 1) a[0] = 0xFF;
        else if (comp == 2) a[0] = 0x00, a[1] = 0x3C;   // (half 1.0; UNORM16 would want 0xFFFF: close enough for alpha)
        else { const float one = 1.0f; std::memcpy(a, &one, 4); }
    }
}

/// Byte ranges of a texture's storage, from its start (what the CPU wrote since its last upload)
using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;
bool overlaps(const Ranges* dirty, uint64_t a, uint64_t b) {
    if (!dirty) return true;
    for (const auto& r : *dirty)
        if (r.first < b && a < r.second) return true;
    return false;
}

/// The view of one level the ASTC decoder writes: every layer, or one (layer)
VkImageView astc_level_view(Texture* tex, uint32_t level, uint32_t layer) {
    if (layer == ~0u) return astc_target_view(tex, level);
    const uint64_t key = mix(mix(0xA57D, level), layer);
    auto it = tex->views.find(key);
    if (it != tex->views.end()) return it->second;
    const VkImageView v = make_view(tex, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                                    level, 1, layer, 1, {}, VK_IMAGE_USAGE_STORAGE_BIT);
    tex->views[key] = v;
    return v;
}

/// ASTC decoded on the GPU (Eden's decoder): the layers dirty overlaps (all of them without dirty), each layer's
/// levels from the first one the image keeps (a texture that leaves out its top levels stages only the tail of
/// every layer's chain: Eden's AssembleInto does the same)
bool upload_astc_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {
    if (tex.type != VK_IMAGE_TYPE_2D || tex.samples != VK_SAMPLE_COUNT_1_BIT || !astc_ready()) return false;
    const uint64_t layer_size = tex.layers > 1 ? tex.L.layer_size : std::max<uint64_t>(tex.L.layer_size, total);
    const uint64_t tail0 = level_geo(tex, 0).offset;
    if (!layer_size || tail0 >= layer_size || (tail0 & 15) || (layer_size & 15)) return false;
    const uint64_t tail = layer_size - tail0;
    std::vector<uint32_t> layers;
    for (uint32_t layer = 0; layer < tex.layers; layer++)
        if (overlaps(dirty, (uint64_t)layer * layer_size + tail0, (uint64_t)(layer + 1) * layer_size)) layers.push_back(layer);
    if (layers.empty()) return true;
    const bool all = layers.size() == tex.layers;
    const Staging s = staging(tail * layers.size(), std::max<uint64_t>(256, dev.props.limits.minStorageBufferOffsetAlignment));
    if (!s.buffer) return false;
    std::vector<uint8_t> scratch;
    for (size_t k = 0; k < layers.size(); k++) {
        const uint64_t at = (uint64_t)layers[k] * layer_size + tail0;
        if (at >= total) continue;
        const uint64_t n = std::min(tail, total - at);
        std::memcpy(s.data + k * tail, guest_bytes(tex.gpu + at, n, scratch), n);
    }
    const uint64_t staged = tail * layers.size();
    st_bytes += (int64_t)staged;
    VkCommandBuffer cmd = upload_cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_pipe);
    const uint64_t align = std::max<uint64_t>(dev.props.limits.minStorageBufferOffsetAlignment, 4);
    if (tex.bc) {   // (decoded into the scratch a run of consecutive layers at a time, re-encoded, copied in)
        bc_scratch_layout(cmd);
        for (uint32_t level = 0; level < tex.levels; level++) {
            const LevelGeo g = level_geo(tex, level);
            const uint64_t off = g.offset - tail0;
            if (g.offset < tail0 || off >= tail || (off % align)) continue;
            const uint32_t iw = std::max(1u, tex.width >> level), ih = std::max(1u, tex.height >> level);
            AstcPush pc{};
            pc.block_dims[0] = (uint32_t)tex.block_w, pc.block_dims[1] = (uint32_t)tex.block_h;
            pc.layer_stride = (uint32_t)tail;
            const uint32_t gobs_x = (g.tw * 16 + 63) >> 6;
            pc.x_shift = 9 + g.tile_h + g.tile_d;
            pc.block_size = gobs_x << pc.x_shift;
            pc.block_height = g.tile_h;
            pc.block_height_mask = (1u << g.tile_h) - 1;
            pc.native_tiling = 1;
            for (size_t k0 = 0; k0 < layers.size();) {
                size_t k1 = k0 + 1;
                while (k1 < layers.size() && k1 - k0 < kBcLayers && layers[k1] == layers[k1 - 1] + 1) k1++;
                const uint32_t n = (uint32_t)(k1 - k0);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_pipe);
                const uint64_t from = k0 * tail + off;
                VkDescriptorBufferInfo bi{s.buffer, s.offset + from, staged - from};
                VkDescriptorImageInfo ii{VK_NULL_HANDLE, bc_scratch_view, VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
                w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[0].pBufferInfo = &bi;
                w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                w[1].pImageInfo = &ii;
                vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_layout, 0, 2, w);
                vkCmdPushConstants(cmd, astc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                vkCmdDispatch(cmd, (g.tw + 7) / 8, (g.th + 7) / 8, n);
                bc_encode_into(cmd, iw, ih, n, tex, level, layers[k0], 0, 0);
                k0 = k1;
            }
        }
        st_astc++;
        return true;
    }
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        const uint64_t off = g.offset - tail0;
        if (g.offset < tail0 || off >= tail || (off % align)) continue;
        AstcPush pc{};
        pc.block_dims[0] = (uint32_t)tex.block_w, pc.block_dims[1] = (uint32_t)tex.block_h;
        pc.layer_stride = (uint32_t)tail;
        const uint32_t gobs_x = (g.tw * 16 + 63) >> 6;
        pc.x_shift = 9 + g.tile_h + g.tile_d;
        pc.block_size = gobs_x << pc.x_shift;
        pc.block_height = g.tile_h;
        pc.block_height_mask = (1u << g.tile_h) - 1;
        pc.native_tiling = 1;
        for (size_t k = 0; k < (all ? 1 : layers.size()); k++) {
            const VkImageView view = astc_level_view(&tex, level, all ? ~0u : layers[k]);
            if (!view) return false;
            const uint64_t from = (all ? 0 : k * tail) + off;
            VkDescriptorBufferInfo bi{s.buffer, s.offset + from, staged - from};
            VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[0].pBufferInfo = &bi;
            w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[1].pImageInfo = &ii;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_layout, 0, 2, w);
            vkCmdPushConstants(cmd, astc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            vkCmdDispatch(cmd, (g.tw + 7) / 8, (g.th + 7) / 8, all ? tex.layers : 1);
        }
    }
    st_astc++;
    return true;
}

// ---- big block-linear textures unswizzled on the GPU (Eden's block_linear_unswizzle_2d.comp, GPL, with the Switch
// 2 tiling): the shader reads the storage where it is, in its imported pool, and writes the image. The course maps
// the title loads at each scene change (six of 349 MB) were copied into staging and unswizzled on the CPU: ~200 ms
// of the replay thread. RX_VK_GPU_UNSWIZZLE=0: on the CPU. ----
struct UnswizzlePush {
    uint32_t origin[3], pad0;
    int32_t destination[3];
    uint32_t bytes_per_block_log2, layer_stride, block_size, x_shift, block_height, block_height_mask, native_tiling;
};
VkDescriptorSetLayout unsw_set = VK_NULL_HANDLE;
VkPipelineLayout unsw_layout = VK_NULL_HANDLE;
VkPipeline unsw_pipe = VK_NULL_HANDLE;
bool unswizzle_ready() {
    static int state = 0;
    if (state) return state > 0;
    state = -1;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    if (getenv("RX_VK_GPU_UNSWIZZLE") && !atoi(getenv("RX_VK_GPU_UNSWIZZLE"))) return false;
    const VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &unsw_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UnswizzlePush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &unsw_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &unsw_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof BLOCK_LINEAR_UNSWIZZLE_2D_COMP_SPV;
    mi.pCode = BLOCK_LINEAR_UNSWIZZLE_2D_COMP_SPV;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = unsw_layout;
    const VkResult r = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &unsw_pipe);
    vkDestroyShaderModule(dev.device, m, nullptr);
    if (r != VK_SUCCESS) return false;
    state = 1;
    logf("vk: big block-linear textures are unswizzled on the GPU from their pools (RX_VK_GPU_UNSWIZZLE=0: the CPU)");
    return true;
}

/// The whole texture (every level and layer dirty overlaps) unswizzled on the GPU; false: not a texture for it
bool upload_unswizzle_gpu(Texture& tex, uint64_t total, const Ranges* dirty) {
    if (tex.type != VK_IMAGE_TYPE_2D || tex.astc || tex.bc || !(tex.usage & VK_IMAGE_USAGE_STORAGE_BIT) ||
        is_bc(tex.format) || is_astc(tex.format) || is_depth_format(tex.format) || nvn_tex_is_linear(&tex.d))
        return false;
    if (total < (8u << 20) || tex.block_w != 1 || tex.block_h != 1) return false;
    int widen_from = 0;
    widened(tex.d.format ? vk_format(tex.nvn_format).format : tex.format, &widen_from);
    if (widen_from) return false;
    VkFormat vf;
    switch (tex.bpb) {
    case 1: vf = VK_FORMAT_R8_UINT; break;
    case 2: vf = VK_FORMAT_R16_UINT; break;
    case 4: vf = VK_FORMAT_R32_UINT; break;
    case 8: vf = VK_FORMAT_R32G32_UINT; break;
    case 16: vf = VK_FORMAT_R32G32B32A32_UINT; break;
    default: return false;
    }
    if (!unswizzle_ready()) return false;
    const BufferRef src = gpu_buffer(tex.gpu, total);
    const uint64_t align = std::max<uint64_t>(dev.props.limits.minStorageBufferOffsetAlignment, 16);
    if (!src || (src.offset % align)) return false;
    for (uint32_t level = 0; level < tex.levels; level++) {   // (every level must be possible before any is done)
        const LevelGeo g = level_geo(tex, level);
        if (g.td != 1 || g.tile_d || (g.offset % align) || g.offset >= total) return false;
        // (the shader reads whole GOB-block rows: a level whose last one runs past the storage, which the CPU path
        // reads from a zero-padded copy, would read past the pool and fault the device)
        if (g.offset + (uint64_t)(tex.layers - 1) * tex.L.layer_size + g.swizzled > total) return false;
    }
    VkCommandBuffer cmd = upload_cmd();
    full_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, unsw_pipe);
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        bool any = false;
        for (uint32_t layer = 0; layer < tex.layers && !any; layer++)
            any = overlaps(dirty, g.offset + (uint64_t)layer * tex.L.layer_size,
                           g.offset + (uint64_t)layer * tex.L.layer_size + g.swizzled);
        if (!any) continue;
        const uint64_t key = mix(0x5B1Cu, mix(level, (uint64_t)vf));
        VkImageView view;
        if (auto it = tex.views.find(key); it != tex.views.end()) view = it->second;
        else view = tex.views[key] = make_view(&tex, VK_IMAGE_VIEW_TYPE_2D_ARRAY, vf, VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0,
                                               tex.layers, {}, VK_IMAGE_USAGE_STORAGE_BIT);
        if (!view) return false;
        UnswizzlePush pc{};
        pc.bytes_per_block_log2 = (uint32_t)std::countr_zero((uint32_t)tex.bpb);
        pc.layer_stride = (uint32_t)tex.L.layer_size;
        const uint32_t gobs_x = (uint32_t)(((uint64_t)g.tw * tex.bpb + 63) >> 6);
        pc.x_shift = 9 + g.tile_h;
        pc.block_size = gobs_x << pc.x_shift;
        pc.block_height = g.tile_h;
        pc.block_height_mask = (1u << g.tile_h) - 1;
        pc.native_tiling = 1;
        VkDescriptorBufferInfo bi{src.buffer, src.offset + g.offset, total - g.offset};
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo = &bi;
        w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &ii;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, unsw_layout, 0, 2, w);
        vkCmdPushConstants(cmd, unsw_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        checkpoint_mark(cmd, tex.gpu, 'g');
        vkCmdDispatch(cmd, (g.tw + 31) / 32, (g.th + 31) / 32, tex.layers);
        st_bytes += (int64_t)g.swizzled * tex.layers;
    }
    full_barrier(cmd);
    descriptors_disturbed();
    st_gpu_unswizzled++;
    return true;
}

/// The texture from guest memory into its image: all of it, or (dirty) the layers, levels and block rows the CPU
/// wrote. Block-linear levels are rows of blocks a GOB-block high, each self-contained: a written page re-uploads
/// the block rows it falls in.
uint64_t st_made;          // (textures made)
int64_t st_unswizzle_bytes, st_unswizzle_ns;   // (the CPU's unswizzling)
int64_t st_never_written;   // (new textures over storage render targets took, not written since: not uploaded)
std::map<uint64_t, std::pair<uint64_t, uint32_t>> gpu_owned;   // GPU address -> (end, memory generation then)
thread_local uint64_t tl_watch_prog;
/// (RX_VK_VERIFY_DIM=WxH: the guest memory of textures that size, hashed when uploaded and every 300 frames: a change
/// the write tracking did not report is logged as a missed write)
std::pair<uint32_t, uint32_t> verify_dim() {
    static const std::pair<uint32_t, uint32_t> d = [] {
        unsigned w = 0, h = 0;
        if (const char* e = getenv("RX_VK_VERIFY_DIM")) sscanf(e, "%ux%u", &w, &h);
        return std::pair<uint32_t, uint32_t>{w, h};
    }();
    return d;
}
std::unordered_map<uint32_t, std::vector<uint8_t>> verify_snap;   // (each verified texture's memory when uploaded)
uint64_t guest_hash(const Texture& tex, bool snapshot = false) {
    std::vector<uint8_t> b(tex.guest_size);
    if (!host->gpu_read(tex.gpu, b.data(), b.size())) return 0;
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
    if (snapshot) verify_snap[tex.id] = std::move(b);
    return h;
}
/// (a missed write: which 4 KB pages changed since the upload, and the CPU's next writes to them watched)
void verify_report(const Texture& tex) {
    const auto it = verify_snap.find(tex.id);
    if (it == verify_snap.end()) return;
    std::vector<uint8_t> now(tex.guest_size);
    if (!host->gpu_read(tex.gpu, now.data(), now.size())) return;
    std::string runs;
    int nruns = 0, watched = 0;
    uint64_t changed = 0;
    for (uint64_t off = 0; off < tex.guest_size; off += 4096) {
        const uint64_t n = std::min<uint64_t>(4096, tex.guest_size - off);
        if (!std::memcmp(now.data() + off, it->second.data() + off, n)) continue;
        changed++;
        if (nruns < 12) {
            char b[48];
            snprintf(b, sizeof b, " +%#llx", (unsigned long long)off);
            runs += b, nruns++;
        }
        static int watches;
        if (host->watch_guest_writes && watches < 4 && watched < 2) {
            const uint64_t g = host->gpu_guest(tex.gpu + off);
            if (g) host->watch_guest_writes(g, 4096), watches++, watched++;
        }
    }
    logf("vk: verify: texture %#llx: %llu pages changed since the upload:%s%s", (unsigned long long)tex.gpu,
         (unsigned long long)changed, runs.c_str(), watched ? " (the next CPU writes to two of them watched)" : "");
    {   // (the first changed page as the write tracking has it: ever written, written since the upload, and when)
        for (uint64_t off = 0; off < tex.guest_size; off += 4096) {
            const uint64_t n = std::min<uint64_t>(4096, tex.guest_size - off);
            if (!std::memcmp(now.data() + off, it->second.data() + off, n)) continue;
            const uint64_t g = host->gpu_guest(tex.gpu + off);
            std::vector<std::pair<uint64_t, uint64_t>> ever, since;
            memory_written_runs(g, 4096, 0, ever);
            memory_written_runs(g, 4096, tex.upload_gen, since);
            logf("vk: verify: its page +%#llx (guest %#llx): stamped ever %d, since the upload (gen %u) %d; generation now %u",
                 (unsigned long long)off, (unsigned long long)g, (int)!ever.empty(), tex.upload_gen, (int)!since.empty(),
                 memory_generation());
            break;
        }
    }
    it->second = std::move(now);
}   // (the draw or dispatch the replay is at, for watch_tex)
/// (RX_VK_WATCH_TEX=addr[,addr...]: what happens to the textures over those GPU addresses, 600 lines)
void watch_tex(const Texture* tex, const char* what) {
    static std::vector<uint64_t> addrs = [] {
        std::vector<uint64_t> v;
        if (const char* e = getenv("RX_VK_WATCH_TEX"))
            for (const char* p = e; *p;) {
                char* end;
                const uint64_t a = strtoull(p, &end, 16);
                if (end == p) break;
                v.push_back(a);
                p = *end ? end + 1 : end;
            }
        return v;
    }();
    static const std::pair<uint32_t, uint32_t> dim = [] {
        unsigned w = 0, h = 0;
        if (const char* e = getenv("RX_VK_WATCH_DIM")) sscanf(e, "%ux%u", &w, &h);
        return std::pair<uint32_t, uint32_t>{w, h};
    }();
    static const std::pair<uint64_t, uint64_t> range = [] {   // (RX_VK_WATCH_RANGE=<lo>:<hi>: textures overlapping it)
        unsigned long long a = 0, b = 0;
        if (const char* e = getenv("RX_VK_WATCH_RANGE")) sscanf(e, "%llx:%llx", &a, &b);
        return std::pair<uint64_t, uint64_t>{a, b};
    }();
    if (tex && range.second && tex->gpu < range.second && tex->gpu + std::max<uint64_t>(tex->guest_size, 1) > range.first) {
        static int rlogged;
        if (rlogged++ < 600)
            logf("vk: watch range frame %llu prog %016llx %s #%u %#llx %ux%ux%u x%u levels %u NVN %#x target %d size %llu%s%s",
                 (unsigned long long)frame_no, (unsigned long long)tl_watch_prog, what, tex->id, (unsigned long long)tex->gpu,
                 tex->width, tex->height, tex->depth, tex->layers, tex->levels, tex->nvn_format, tex->d.target,
                 (unsigned long long)tex->guest_size, tex->gpu_written ? " rendered" : "", tex->uploaded ? " uploaded" : "");
    }
    if ((addrs.empty() && !dim.first) || !tex) return;
    {   // (RX_VK_DUMP_ON_WATCH=<frames>: a watched texture made after boot dumps the frames after it, once)
        static const int on_watch = getenv("RX_VK_DUMP_ON_WATCH") ? atoi(getenv("RX_VK_DUMP_ON_WATCH")) : 0;
        static bool requested = false;
        if (on_watch && !requested && frame_no > 1000 && !strcmp(what, "made")) {
            for (const uint64_t a : addrs)
                if (a == tex->gpu) {
                    requested = true;
                    dump_request((int64_t)frame_no + 1, on_watch);
                    logf("vk: watch: texture %#llx made at frame %llu: the next %d frames dumped", (unsigned long long)a,
                         (unsigned long long)frame_no, on_watch);
                }
        }
    }
    static int logged;
    static std::unordered_map<uint32_t, uint64_t> told;   // (a texture's events once a frame)
    const bool by_dim = dim.first && tex->width == dim.first && tex->height == dim.second;
    if (by_dim && !strcmp(what, "written")) {   // ("written" once a frame; the rest each time)
        uint64_t& f = told[tex->id];
        if (f == frame_no || logged >= 20000) return;
        f = frame_no;
    }
    const uint64_t hi = tex->gpu + std::max<uint64_t>(tex->guest_size, 1);
    for (const uint64_t a : by_dim ? std::vector<uint64_t>{tex->gpu} : addrs) {
        if (a < tex->gpu || a >= hi || logged >= (by_dim ? 20000 : 600)) continue;
        logged++;
        logf("vk: watch tex frame %llu prog %016llx %s #%u %#llx %ux%ux%u x%u levels %u NVN %#x layout %#x class %#x target %d "
             "size %llu%s%s", (unsigned long long)frame_no, (unsigned long long)tl_watch_prog, what, tex->id, (unsigned long long)tex->gpu, tex->width,
             tex->height, tex->depth, tex->layers, tex->levels, tex->nvn_format, tex->key_layout, tex->key_cls,
             tex->d.target, (unsigned long long)tex->guest_size, tex->gpu_written ? " rendered" : "",
             tex->uploaded ? " uploaded" : "");
        return;
    }
}
/// Whether [gpu, gpu + n) is storage the GPU's images rendered last (covered by ranges render targets took), and the
/// generation from which the CPU's writes would count (0: not the GPU's)
uint32_t gpu_owned_since(uint64_t gpu, uint64_t n) {
    uint64_t covered = 0;
    uint32_t since = UINT32_MAX;
    auto it = gpu_owned.upper_bound(gpu);
    if (it != gpu_owned.begin()) --it;
    for (; it != gpu_owned.end() && it->first < gpu + n; ++it) {
        const uint64_t a = std::max(it->first, gpu), b = std::min(it->second.first, gpu + n);
        if (b <= a) continue;
        covered += b - a;
        since = std::min(since, it->second.second);
    }
    return covered >= n && since != UINT32_MAX ? std::max(since, 1u) : 0;
}

bool upload(Texture& tex, const Ranges* dirty) {
    PROF(PROF_UPLOAD);
    if (!tex.gpu || tex.samples != VK_SAMPLE_COUNT_1_BIT) return false;
    const uint64_t total = tex.guest_size;
    if (!total || (total > (1ull << 31) && !tex.sparse)) return false;   // (a sparse image: only what holds texels)
    // (depth-stencil: the depth aspect only; packed D24S8 storage is not copied into it as is: such targets are
    // rendered, not uploaded)
    if (tex.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return false;
    st_uploads++;
    {   // RX_VK_LOG_UPLOADS=<present>: the uploads from that present on (200 of them; what churns)
        static const int64_t from = getenv("RX_VK_LOG_UPLOADS") ? atoll(getenv("RX_VK_LOG_UPLOADS")) : -1;
        static int logged;
        if (from >= 0 && (int64_t)frame_no >= from && logged < 200) {
            logged++;
            uint64_t part = 0;
            if (dirty)
                for (const auto& r : *dirty) part += r.second - r.first;
            logf("vk: upload #%u %#llx %ux%ux%u x%u levels %u NVN %#x%s: %llu KB of %llu KB (%s), made at present %llu",
                 tex.id, (unsigned long long)tex.gpu, tex.width, tex.height, tex.depth, tex.layers, tex.levels,
                 tex.nvn_format, tex.astc ? " ASTC" : "", (unsigned long long)((dirty ? part : total) >> 10),
                 (unsigned long long)(total >> 10), !tex.uploaded ? "new" : dirty ? "written" : "whole",
                 (unsigned long long)tex.first_seen);
        }
    }
    if (tex.astc && upload_astc_gpu(tex, total, dirty)) return true;
    if (tex.bc) return false;   // (no CPU path into BC3)
    // (the GPU's unswizzle reads guest memory as it is: not where the capture kept the storage as it was at its submit)
    if (!(tex.gpu < (1ull << 40) && memory_has_shadow(tex.gpu, total)) && upload_unswizzle_gpu(tex, total, dirty)) return true;
    const bool linear = nvn_tex_is_linear(&tex.d);
    int widen_from = 0;
    widened(tex.d.format ? vk_format(tex.nvn_format).format : tex.format, &widen_from);
    const int src_bpb = tex.bpb;
    const int out_bpb = tex.astc ? 4 : widen_from ? (widen_from == 3 ? 4 : widen_from == 6 ? 8 : 16) : src_bpb;
    const uint32_t copy_layers = tex.type == VK_IMAGE_TYPE_3D ? 1 : tex.layers;
    const VkImageAspectFlags aspect = tex.aspect;
    std::vector<VkBufferImageCopy> copies;
    std::vector<uint8_t> scratch, lin;
    VkBuffer buffer = VK_NULL_HANDLE;
    for (uint32_t level = 0; level < tex.levels; level++) {
        const LevelGeo g = level_geo(tex, level);
        for (uint32_t layer = 0; layer < copy_layers; layer++) {
            const uint64_t src_off = g.offset + (uint64_t)layer * tex.L.layer_size;
            if (src_off >= total) continue;
            const uint32_t stride = linear ? (tex.d.stride ? tex.d.stride : host->tex_linear_stride(&tex.d)) : 0;
            const uint64_t span = linear ? (uint64_t)stride * g.th : std::max<uint64_t>(g.swizzled, 1);
            if (!overlaps(dirty, src_off, src_off + span)) continue;
            // the block rows to upload: all of them, or those a written page falls in (2D block-linear levels)
            const uint32_t rows_per = 8u << g.tile_h;   // (block rows of a GOB block)
            const uint32_t nrows = (g.th + rows_per - 1) / rows_per;
            const uint64_t gobs_x = ((uint64_t)g.tw * src_bpb + 63) / 64;
            const uint64_t row_bytes = gobs_x * (512ull << (g.tile_h + g.tile_d));
            std::vector<std::pair<uint32_t, uint32_t>> runs;   // (first row, rows)
            if (!dirty || linear || tex.astc || widen_from || g.td > 1 || g.tile_d) {
                runs.push_back({0, nrows});
            } else {
                std::vector<uint8_t> mark(nrows, 0);
                for (const auto& r : *dirty) {
                    const uint64_t lo = std::max(r.first, src_off), hi = std::min(r.second, src_off + span);
                    if (lo >= hi) continue;
                    const uint64_t r0 = (lo - src_off) / row_bytes, r1 = std::min<uint64_t>((hi - 1 - src_off) / row_bytes, nrows - 1);
                    for (uint64_t k = r0; k <= r1; k++) mark[k] = 1;
                }
                for (uint32_t k = 0; k < nrows;) {
                    if (!mark[k]) { k++; continue; }
                    uint32_t e = k;
                    while (e < nrows && mark[e]) e++;
                    runs.push_back({k, e - k});
                    k = e;
                }
            }
            for (const auto& [row0, nrow] : runs) {
                const uint32_t y0 = row0 * rows_per;                       // (blocks)
                const uint32_t th = std::min(g.th - y0, nrow * rows_per);   // (blocks)
                const uint64_t block_bytes = (uint64_t)g.tw * th * g.td * src_bpb;
                const uint32_t px_h = std::min(g.h - y0 * (uint32_t)tex.block_h, th * (uint32_t)tex.block_h);
                const uint64_t out_bytes = tex.astc ? (uint64_t)g.w * px_h * g.d * 4 : (uint64_t)g.tw * th * g.td * out_bpb;
                const Staging st = staging((out_bytes + 15) & ~15ull, 16);
                if (!st.buffer) return false;
                if (buffer && st.buffer != buffer && !copies.empty()) {   // (a new arena: copy what the last one holds)
                    texture_resident_flush(&tex);
                    checkpoint_mark(upload_cmd(), tex.gpu, 'u');
                    vkCmdCopyBufferToImage(upload_cmd(), buffer, tex.image, VK_IMAGE_LAYOUT_GENERAL,
                                           (uint32_t)copies.size(), copies.data());
                    copies.clear();
                }
                buffer = st.buffer;
                uint8_t* out = st.data;
                uint8_t* blocks = (tex.astc || widen_from) ? (lin.resize(block_bytes), lin.data()) : out;
                if (linear) {
                    const uint64_t row = (uint64_t)g.tw * src_bpb;
                    const uint8_t* src = guest_bytes(tex.gpu + src_off, (uint64_t)stride * (g.th - 1) + row, scratch);
                    for (uint32_t y = 0; y < g.th; y++) std::memcpy(blocks + y * row, src + (uint64_t)y * stride, row);
                } else {
                    // (suyu's unswizzle reads the whole swizzled span without bounds checks: a span the storage
                    // ends in is read from a zero-padded copy)
                    const uint64_t from = src_off + (uint64_t)row0 * row_bytes;
                    const uint64_t need = runs.size() == 1 && row0 == 0 && nrow == nrows ? span : (uint64_t)nrow * row_bytes;
                    // (a big 2D level: its GOB-block rows unswizzled on several threads, each row on its own. The
                    // course maps the title loads at each scene change, 6 of 349 MB, took ~100 ms each on one. Their
                    // storage is sparse: each thread gathers its own rows, 8 MB at a time, rather than all of it
                    // being copied first (a 349 MB scratch beside the 349 MB staged))
                    const uint32_t nthreads = block_bytes >= (16u << 20) && g.td == 1 && !g.tile_d
                                                  ? std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u) : 1u;
                    uint64_t avail = 0;
                    const uint8_t* direct = host->gpu_ptr(tex.gpu + from, &avail);
                    const bool gather_parts = nthreads > 1 && !(direct && avail >= need) && from < total;
                    const uint8_t* src = nullptr;
                    if (gather_parts) {
                    } else if (from < total && total - from >= need) {
                        src = guest_bytes(tex.gpu + from, need, scratch);
                    } else {
                        scratch.assign(need, 0);
                        if (from < total) host->gpu_read(tex.gpu + from, scratch.data(), total - from);
                        src = scratch.data();
                    }
                    const auto u0 = std::chrono::steady_clock::now();
                    if (nthreads > 1) {
                        const uint64_t out_row = (uint64_t)g.tw * rows_per * src_bpb;   // (bytes per GOB-block row)
                        const uint32_t per = (nrow + nthreads - 1) / nthreads;
                        const uint32_t chunk = (uint32_t)std::max<uint64_t>(1, (8u << 20) / row_bytes);
                        auto part = [&](uint32_t t) {
                            const uint32_t a0 = t * per, b0 = std::min(nrow, a0 + per);
                            std::vector<uint8_t> mine;
                            for (uint32_t a = a0; a < b0; a += chunk) {
                                const uint32_t b = std::min(b0, a + chunk);
                                const uint32_t h = std::min(th - a * rows_per, (b - a) * rows_per);
                                const uint8_t* rows;
                                uint64_t rows_n;
                                if (gather_parts) {   // (its rows, zero past the storage's end)
                                    rows_n = (uint64_t)(b - a) * row_bytes;
                                    mine.assign(rows_n, 0);
                                    const uint64_t at = from + (uint64_t)a * row_bytes;
                                    for (uint64_t o = 0; o < rows_n && at + o < total; o += 0x10000) {
                                        const uint64_t k = std::min<uint64_t>({0x10000, rows_n - o, total - at - o});
                                        host->gpu_read(tex.gpu + at + o, mine.data() + o, k);
                                    }
                                    rows = mine.data();
                                } else {
                                    rows = src + (uint64_t)a * row_bytes, rows_n = need - (uint64_t)a * row_bytes;
                                }
                                Tegra::Texture::UnswizzleTexture(
                                    std::span<uint8_t>(blocks + a * out_row, (uint64_t)g.tw * h * src_bpb),
                                    std::span<const uint8_t>(rows, rows_n), (u32)src_bpb, g.tw, h, 1, g.tile_h, 0, 1);
                            }
                        };
                        std::vector<std::thread> pool;
                        for (uint32_t t = 1; t < nthreads; t++) pool.emplace_back(part, t);
                        part(0);
                        for (std::thread& t : pool) t.join();
                        static const bool check = getenv("RX_VK_UNSWIZZLE_CHECK") != nullptr;   // (against one thread)
                        if (check && src) {
                            std::vector<uint8_t> ref(block_bytes);
                            Tegra::Texture::UnswizzleTexture(std::span<uint8_t>(ref.data(), block_bytes),
                                                             std::span<const uint8_t>(src, need), (u32)src_bpb, g.tw,
                                                             th, g.td, g.tile_h, g.tile_d, 1);
                            const bool same = !std::memcmp(ref.data(), blocks, block_bytes);
                            logf("vk: unswizzle check %#llx level %u rows %u+%u: %s", (unsigned long long)tex.gpu,
                                 level, row0, nrow, same ? "same" : "DIFFERENT");
                        }
                    } else {
                        Tegra::Texture::UnswizzleTexture(std::span<uint8_t>(blocks, block_bytes),
                                                         std::span<const uint8_t>(src, need), (u32)src_bpb, g.tw, th,
                                                         g.td, g.tile_h, g.tile_d, 1);
                    }
                    st_unswizzle_bytes += (int64_t)block_bytes;
                    st_unswizzle_ns +=
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - u0).count();
                }
                if (tex.astc) {
                    Tegra::Texture::ASTC::DecompressSerial(std::span<const uint8_t>(blocks, block_bytes), g.w, px_h, g.d,
                                                           (u32)tex.block_w, (u32)tex.block_h,
                                                           std::span<uint8_t>(out, (size_t)g.w * px_h * g.d * 4));
                } else if (widen_from) {
                    widen_rows(out, blocks, (size_t)g.tw * th * g.td, src_bpb, out_bpb);
                }
                st_bytes += (int64_t)out_bytes;
                // (a sparse image: only its blocks this holds texels other than zero get memory; the others read
                // zero unbound, and what the copy writes into them is dropped)
                if (tex.sparse) {
                    const VkExtent3D gr = tex.sparse_req.formatProperties.imageGranularity;
                    const uint32_t bw = (uint32_t)tex.block_w, bh = (uint32_t)tex.block_h;
                    const uint64_t row = (uint64_t)g.tw * out_bpb;   // (bytes of a row of blocks)
                    const uint32_t tbw = std::max(1u, gr.width / bw);   // (blocks across a sparse block)
                    const uint32_t ntx = (g.tw + tbw - 1) / tbw;
                    std::vector<uint8_t> hit;   // (sparse block rows x columns this reaches)
                    const uint32_t ty0 = y0 * bh / gr.height, ty1 = ((y0 + th) * bh - 1) / gr.height;
                    hit.assign((size_t)(ty1 - ty0 + 1) * ntx, 0);
                    if (level < tex.sparse_req.imageMipTailFirstLod) {
                        for (uint32_t br = 0; br < th; br++) {
                            const uint8_t* p = out + br * row;
                            const uint32_t ty = (y0 + br) * bh / gr.height - ty0;
                            for (uint32_t tx = 0; tx < ntx; tx++) {
                                uint8_t& h = hit[(size_t)ty * ntx + tx];
                                if (h) continue;
                                const uint64_t a = (uint64_t)tx * tbw * out_bpb, e = std::min<uint64_t>(row, a + (uint64_t)tbw * out_bpb);
                                uint64_t any = 0;
                                for (uint64_t i = a; i + 8 <= e; i += 8) {
                                    uint64_t v;
                                    std::memcpy(&v, p + i, 8);
                                    any |= v;
                                }
                                h = any != 0;
                            }
                        }
                    } else {
                        uint64_t any = 0;
                        for (uint64_t i = 0; i + 8 <= out_bytes && !any; i += 8) {
                            uint64_t v;
                            std::memcpy(&v, out + i, 8);
                            any |= v;
                        }
                        if (any) hit.assign(hit.size(), 1);
                    }
                    bool some = false;
                    for (uint32_t ty = 0; ty < ty1 - ty0 + 1; ty++)
                        for (uint32_t tx = 0; tx < ntx; tx++)
                            if (hit[(size_t)ty * ntx + tx]) {
                                some = true;
                                texture_resident_queue(&tex, level, layer, tx * gr.width, (ty0 + ty) * gr.height,
                                                       gr.width, gr.height);
                            }
                    if (!some) continue;
                }
                VkBufferImageCopy c{};
                c.bufferOffset = st.offset;
                c.bufferRowLength = tex.astc ? g.w : g.tw * tex.block_w;
                c.bufferImageHeight = tex.astc ? px_h : th * tex.block_h;
                c.imageSubresource = {aspect, level, tex.type == VK_IMAGE_TYPE_3D ? 0 : layer, 1};
                c.imageOffset = {0, (int32_t)(y0 * (uint32_t)tex.block_h), 0};
                c.imageExtent = {g.w, px_h, g.d};
                copies.push_back(c);
            }
        }
    }
    if (!copies.empty()) {
        texture_resident_flush(&tex);
        checkpoint_mark(upload_cmd(), tex.gpu, 'u');
        vkCmdCopyBufferToImage(upload_cmd(), buffer, tex.image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)copies.size(),
                               copies.data());
    }
    return true;
}

/// What the CPU wrote of a texture's storage since gen, as byte ranges from its start. A texture in a virtual
/// (sparse) pool is looked at page by page through its mapping, and is all dirty when its mapping changed.
bool texture_dirty(Texture& tex, uint32_t gen, Ranges& out) {
    out.clear();
    constexpr uint64_t kVirtual = 1ull << 40, kVPage = 1ull << 16;
    if (tex.gpu < kVirtual) {
        memory_written_runs(tex.guest, tex.guest_size, gen, out);
        return !out.empty();
    }
    // (the page walk only when the GPU mappings changed: the terrain's sparse arrays span over 1 GB, 64 KB pages,
    // and were walked every recording; between changes, runs of contiguous guest memory are checked)
    const uint32_t maps_gen = host->maps_generation() + host->vmap_generation();
    if (tex.runs_maps_gen != maps_gen) {
        tex.runs_maps_gen = maps_gen;
        tex.runs.clear();
        uint64_t sig = 0x5157;
        // (the pages whose mapping changed: only those are dirty. The course maps' sparse pools are remapped every
        // ~15 s at the title, a few pages at a time, and each remap re-uploaded all of six 349 MB maps)
        const size_t npages = (size_t)((tex.guest_size + kVPage - 1) / kVPage);
        const bool had_map = tex.page_map.size() == npages;
        if (!had_map) tex.page_map.assign(npages, 0);
        Ranges remapped;
        for (uint64_t off = 0; off < tex.guest_size; off += kVPage) {
            const uint64_t n = std::min(kVPage, tex.guest_size - off);
            const uint64_t g = host->gpu_guest(tex.gpu + off);
            sig = mix(sig, g);
            uint64_t& was = tex.page_map[(size_t)(off / kVPage)];
            if (had_map && was != g) {
                if (!remapped.empty() && remapped.back().second == off) remapped.back().second = off + n;
                else remapped.push_back({off, off + n});
            }
            was = g;
            if (!g) continue;
            Texture::Run& last = tex.runs.empty() ? tex.runs.emplace_back(Texture::Run{off, g, 0}) : tex.runs.back();
            if (last.off + last.n == off && last.guest + last.n == g) last.n += n;
            else tex.runs.push_back({off, g, n});
        }
        std::erase_if(tex.runs, [](const Texture::Run& r) { return r.n == 0; });
        {   // (RX_VK_LOG_REMAP=1: a big texture's pages mapped anew: to which guest memory, and what images are there)
            static const bool log_remap = getenv("RX_VK_LOG_REMAP") != nullptr;
            static int logged;
            if (log_remap && had_map && !remapped.empty() && tex.guest_size >= (32ull << 20) && logged < 120) {
                for (size_t i = 0; i < remapped.size() && i < 4 && logged < 120; i++, logged++) {
                    const uint64_t g = host->gpu_guest(tex.gpu + remapped[i].first);
                    std::string there;
                    for (const auto& kv : textures) {   // (images over that guest memory, by their storage)
                        const Texture* o = kv.second.get();
                        if (o == &tex || !g || o->gpu >= (1ull << 40) || g < o->gpu || g >= o->gpu + o->guest_size) continue;
                        char b[160];
                        snprintf(b, sizeof b, " #%u %#llx %ux%u x%u NVN %#x%s%s;", o->id, (unsigned long long)o->gpu, o->width,
                                 o->height, o->layers, o->nvn_format, o->gpu_written ? " rendered" : "",
                                 o->uploaded ? " uploaded" : "");
                        if (there.size() < 400) there += b;
                    }
                    logf("vk: remap of #%u %#llx %ux%u NVN %#x: +%#llx..+%#llx now at guest %#llx%s%s", tex.id,
                         (unsigned long long)tex.gpu, tex.width, tex.height, tex.nvn_format,
                         (unsigned long long)remapped[i].first, (unsigned long long)remapped[i].second, (unsigned long long)g,
                         g ? "" : " (unmapped)", there.empty() ? " (no image there)" : there.c_str());
                }
            }
        }
        if (sig != tex.map_sig) {   // (pages mapped or unmapped since; gen 0 asks what was ever written)
            tex.map_sig = sig;
            if (gen && !had_map) {
                out.assign(1, {0, tex.guest_size});
                return true;
            }
            if (gen) out = remapped;   // (and below, what the CPU wrote in the pages mapped all along)
        }
    }
    static thread_local Ranges in_run;
    const size_t remaps = out.size();
    for (const Texture::Run& r : tex.runs) {
        in_run.clear();
        memory_written_runs(r.guest, r.n, gen, in_run);
        for (const auto& [lo, hi] : in_run) {
            if (out.size() > remaps && out.back().second == r.off + lo) out.back().second = r.off + hi;
            else out.push_back({r.off + lo, r.off + hi});
        }
    }
    if (remaps && out.size() > remaps) {   // (the two lists in order, overlaps merged)
        std::sort(out.begin(), out.end());
        Ranges merged;
        for (const auto& r : out) {
            if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
            else merged.push_back(r);
        }
        out.swap(merged);
    }
    return !out.empty();
}

uint64_t destroy_gen = 1;   // (bumped when textures go: what was resolved to them goes stale)

void destroy_later(Texture& tex) {
    destroy_gen++;
    Dead d{tex.last_value, tex.image, tex.allocation, {}, {}};
    for (auto& kv : tex.views)
        if (kv.second) d.views.push_back(kv.second);
    d.memory = tex.sparse_chunks;
    dead.push_back(std::move(d));
    total_bytes -= std::min(total_bytes, tex.bytes);
}

void vw_forget(const Texture& t);   // (below: a destroyed texture leaves the list of GPU-written ones)
void forget_gpu(Texture* tex) {
    vw_forget(*tex);
    auto [a, b] = by_gpu.equal_range(tex->gpu);
    for (auto it = a; it != b; ++it)
        if (it->second == tex) {
            by_gpu.erase(it);
            break;
        }
}

Texture* create(const Key& key, const nvn_tex* t, VkFormat vf, bool astc, int bw, int bh, int bpb) {
    auto tex = std::make_unique<Texture>();
    Texture& x = *tex;
    x.gpu = t->gpu, x.d = t->d, x.L = t->L, x.nvn_format = t->d.format;
    x.guest = host->gpu_guest(t->gpu);
    x.guest_size = t->L.size;
    x.astc = astc, x.block_w = bw, x.block_h = bh, x.bpb = bpb;
    int from = 0;
    vf = widened(vf, &from);
    const bool depth = is_depth_format(vf);
    x.format = depth ? vf : astc ? VK_FORMAT_R8G8B8A8_UNORM : linear_twin(vf);
    x.aspect = !depth ? VK_IMAGE_ASPECT_COLOR_BIT
             : vf == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT
             : has_stencil(vf) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                               : VK_IMAGE_ASPECT_DEPTH_BIT;
    const nvn_texdesc& d = t->d;
    x.width = std::max(1u, d.width);
    x.height = std::max(1u, d.height);
    switch (d.target) {
    case NVN_TEXTURE_TARGET_1D: x.type = VK_IMAGE_TYPE_1D, x.height = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: x.type = VK_IMAGE_TYPE_1D, x.layers = std::max(1u, d.height), x.height = 1; break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: x.layers = std::max(1u, d.depth); break;
    case NVN_TEXTURE_TARGET_CUBEMAP: x.layers = 6; break;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: x.layers = std::max(6u, d.depth); break;
    case NVN_TEXTURE_TARGET_3D: x.type = VK_IMAGE_TYPE_3D, x.depth = std::max(1u, d.depth); break;
    default: break;
    }
    const uint32_t max_dim = std::max({x.width, x.height, x.depth});
    const uint32_t max_levels = 1 + (uint32_t)std::floor(std::log2((double)max_dim));
    x.levels = nvn_tex_is_linear(&d) ? 1 : std::clamp<uint32_t>(d.levels, 1, max_levels);
    {   // huge textures leave out their top levels: wider than RX_VK_TEX_MAX_DIM (default 2048) once they would take
        // 64 MB, and down to RX_VK_TEX_MAX_MB (default 512) whatever their size (MKW's terrain material arrays are
        // 8192 x 8192 x 68 ASTC: 24 GB as RGBA8)
        static const uint32_t max_dim = getenv("RX_VK_TEX_MAX_DIM") ? (uint32_t)atoi(getenv("RX_VK_TEX_MAX_DIM")) : 2048;
        static const uint64_t max_bytes = (uint64_t)(getenv("RX_VK_TEX_MAX_MB") ? atoi(getenv("RX_VK_TEX_MAX_MB")) : 1536) << 20;
        const uint32_t texel = astc ? 4 : std::max(1, bpb / std::max(1, bw * bh));
        auto bytes_at = [&](uint32_t skip) {
            uint64_t px = 0;
            for (uint32_t l = skip; l < x.levels; l++)
                px += (uint64_t)std::max(1u, x.width >> l) * std::max(1u, x.height >> l) * std::max(1u, x.depth >> l);
            return px * x.layers * texel * std::max(1, d.samples);
        };
        // (decoded ASTC only by size: uncompressed maps may be read at exact texel coordinates, and the GPU may
        // render into them, where a level left out is wrong)
        // (a 2D array too big to keep whole, in a format with sparse residency: kept whole, as a sparse image)
        if (!astc && !depth && dev.sparse_images && x.type == VK_IMAGE_TYPE_2D && x.layers > 1 && d.samples <= 1 &&
            bytes_at(0) > max_bytes) {
            VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            if (!is_bc(x.format)) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
            uint32_t np = 0;
            vkGetPhysicalDeviceSparseImageFormatProperties(dev.physical, x.format, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
                                                           u, VK_IMAGE_TILING_OPTIMAL, &np, nullptr);
            x.sparse = np > 0;
        }
        while (!x.sparse && astc && x.skip + 1 < x.levels && bytes_at(x.skip) > (64ull << 20) &&
               (std::max(x.width, x.height) >> x.skip) > max_dim)
            x.skip++;
        while (!x.sparse && x.skip + 1 < x.levels && bytes_at(x.skip) > max_bytes) x.skip++;
        if (x.skip) {
            static int logged;
            if (logged++ < 40)
                logf("vk: texture %#llx %ux%ux%u x%u (NVN %#x, %llu MB): its %u top level(s) left out (%llu MB)",
                     (unsigned long long)t->gpu, x.width, x.height, x.depth, x.layers, d.format,
                     (unsigned long long)(bytes_at(0) >> 20), x.skip, (unsigned long long)(bytes_at(x.skip) >> 20));
            x.width = std::max(1u, x.width >> x.skip), x.height = std::max(1u, x.height >> x.skip);
            x.depth = std::max(1u, x.depth >> x.skip);
            x.levels -= x.skip;
        }
    }
    x.samples = d.samples > 1 ? (VkSampleCountFlagBits)d.samples : VK_SAMPLE_COUNT_1_BIT;
    {   // (decoded ASTC as BC3: 2D, at most the scratch's size; from blocks of 6 x 6 texels up (2.25 bits a texel or
        // less: BC3's 8 keep what they have) from 2 MB as RGBA8 (RX_VK_ASTC_BC_MB), the finer ones (UI, characters)
        // only from 64 MB)
        static const uint64_t low = (uint64_t)(getenv("RX_VK_ASTC_BC_MB") ? atoi(getenv("RX_VK_ASTC_BC_MB")) : 2) << 20;
        const uint64_t min_bytes = bw * bh >= 36 ? low : 64ull << 20;
        uint64_t px = 0;
        for (uint32_t l = 0; l < x.levels; l++)
            px += (uint64_t)std::max(1u, x.width >> l) * std::max(1u, x.height >> l);
        // (not one with its top levels left out and a single layer: copies into those levels are blitted into its
        // first, which BC3 cannot take; trimmed, it is small)
        if (astc && !(x.skip && x.layers == 1) && x.type == VK_IMAGE_TYPE_2D && x.samples == VK_SAMPLE_COUNT_1_BIT && x.width <= kBcDim &&
            x.height <= kBcDim && px * x.layers * 4 >= min_bytes && bc_ready()) {
            x.bc = true;
            x.format = VK_FORMAT_BC3_UNORM_BLOCK;
        }
    }
    const VkFormatProperties fp = format_props(x.format);
    x.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) x.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageCreateFlags flags = 0;
    if (depth) {
        x.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else if (!is_bc(x.format) && !is_astc(x.format)) {
        // (any format of the class may be a view: attachments and storage where one of them can)
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        x.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (x.samples == VK_SAMPLE_COUNT_1_BIT) x.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    } else {
        // Raw storage views address one compressed block per texel (8 or 16 bytes).
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT |
                 VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        const VkFormat raw = bpb == 8 ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32B32A32_UINT;
        VkImageFormatProperties limits{};
        if ((format_props(raw).optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
            vkGetPhysicalDeviceImageFormatProperties(dev.physical, x.format, x.type, VK_IMAGE_TILING_OPTIMAL,
                x.usage | VK_IMAGE_USAGE_STORAGE_BIT, flags, &limits) == VK_SUCCESS)
            x.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (x.type == VK_IMAGE_TYPE_2D && x.layers >= 6 && x.width == x.height && x.samples == VK_SAMPLE_COUNT_1_BIT)
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, x.cube = true;
    if (x.type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = x.type;
    ci.format = x.format;
    ci.extent = {x.width, x.height, x.depth};
    ci.mipLevels = x.levels;
    ci.arrayLayers = x.layers;
    ci.samples = x.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = x.usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (x.sparse) {
        ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
        bool ok = vkCreateImage(dev.device, &ci, nullptr, &x.image) == VK_SUCCESS;
        if (ok) {
            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(dev.device, x.image, &mr);
            uint32_t n = 0;
            vkGetImageSparseMemoryRequirements(dev.device, x.image, &n, nullptr);
            std::vector<VkSparseImageMemoryRequirements> reqs(n);
            vkGetImageSparseMemoryRequirements(dev.device, x.image, &n, reqs.data());
            ok = false;
            for (const auto& r : reqs)
                if (r.formatProperties.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) x.sparse_req = r, ok = true;
            x.sparse_align = mr.alignment;
            x.sparse_type = UINT32_MAX;
            for (uint32_t i = 0; i < dev.memory.memoryTypeCount && x.sparse_type == UINT32_MAX; i++)
                if ((mr.memoryTypeBits >> i & 1) && (dev.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                    x.sparse_type = i;
            ok = ok && x.sparse_type != UINT32_MAX && x.sparse_align;
            if (!ok) vkDestroyImage(dev.device, x.image, nullptr), x.image = VK_NULL_HANDLE;
        }
        if (!ok) {
            logf("vk: sparse image %ux%u x%u levels %u format %d not made", x.width, x.height, x.layers, x.levels,
                 (int)x.format);
            return nullptr;
        }
        x.tiles.resize((size_t)x.layers * x.levels);
        x.tail_bound.assign(x.layers, 0);
        logf("vk: texture %#llx %ux%u x%u levels %u (NVN %#x): sparse, layers made resident as they are written "
             "(blocks %ux%u, mip tail from level %u, %llu KB%s)",
             (unsigned long long)t->gpu, x.width, x.height, x.layers, x.levels, d.format,
             x.sparse_req.formatProperties.imageGranularity.width, x.sparse_req.formatProperties.imageGranularity.height,
             x.sparse_req.imageMipTailFirstLod, (unsigned long long)(x.sparse_req.imageMipTailSize >> 10),
             x.sparse_req.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT ? ", one for all layers" : " a layer");
    } else if (!create_image(ci, x.image, x.allocation)) {
        static int logged;
        if (logged++ < 16)
            logf("vk: image %ux%ux%u x%u levels %u format %d (NVN %#x) not made", x.width, x.height, x.depth, x.layers,
                 x.levels, (int)x.format, d.format);
        return nullptr;
    }
    // what it costs: the level chain's texels
    uint64_t px = 0;
    for (uint32_t l = 0; l < x.levels; l++)
        px += (uint64_t)std::max(1u, x.width >> l) * std::max(1u, x.height >> l) * std::max(1u, x.depth >> l);
    const uint32_t texel = x.bc || is_bc(x.format) ? 1 : astc ? 4 : std::max(1, from ? (from == 3 ? 4 : from == 6 ? 8 : 16) : bpb);
    x.bytes = x.sparse ? 0 : px * x.layers * texel * x.samples;   // (a sparse image's: as its layers are bound)
    total_bytes += x.bytes;
    // into GENERAL, and cleared (a target the game renders into before writing it all reads zeros, not garbage)
    VkCommandBuffer cmd = upload_cmd();
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = x.image;
    b.subresourceRange = {x.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    if (depth) {
        const VkClearDepthStencilValue z{0.0f, 0};
        vkCmdClearDepthStencilImage(cmd, x.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &b.subresourceRange);
    } else if (!is_bc(x.format) && !is_astc(x.format)) {
        const VkClearColorValue z{};
        vkCmdClearColorImage(cmd, x.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &b.subresourceRange);
    }
    if (!is_bc(x.format) && !is_astc(x.format)) {
        // (the clear finished before anything after it in this command buffer writes the image: the upload of its contents
        // comes right after, a copy into the same image. Two transfer writes with no barrier between them may run in either
        // order; NVIDIA's ran in order, AMD's (a fast clear) landed last, so a texture uploaded in the recording that made it
        // read zeros there. The menus' text is that case: the CPU writes each string's letters into a new R8 texture, which is
        // drawn once into the string's label, so every menu and title text was blank on AMD cards)
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier2(cmd, &di);
    }
    Texture* p = tex.get();
    static uint32_t ids;
    p->id = ++ids;
    p->first_seen = frame_no;
    st_made++;
    p->key_layout = key.layout, p->key_cls = key.cls;
    {   // (another image at this storage: what tells them apart; content one renders the other does not see)
        static int told;
        auto [a, e] = by_gpu.equal_range(p->gpu);
        for (auto it = a; it != e && told < 80; ++it) {
            const Texture* o = it->second;
            told++;
            logf("vk: texture #%u at %#llx beside #%u: %ux%ux%u x%u levels %u layout %#x class %#x NVN %#x target %d, "
                 "the other %ux%ux%u x%u levels %u layout %#x class %#x NVN %#x target %d%s",
                 p->id, (unsigned long long)p->gpu, o->id, p->width, p->height, p->depth, p->layers, p->levels,
                 key.layout, key.cls, d.format, d.target, o->width, o->height, o->depth, o->layers, o->levels,
                 o->key_layout, o->key_cls, o->nvn_format, o->d.target, o->gpu_written ? " (rendered)" : "");
        }
    }
    textures.emplace(key, std::move(tex));
    by_gpu.emplace(p->gpu, p);
    watch_tex(p, "made");
    if (remade_log_on()) {   // (one evicted not long ago, made again)
        std::lock_guard lk(evicted_mu);
        auto ev = evicted_at.find(evicted_key(p->gpu, p->width, p->height, p->nvn_format));
        if (ev != evicted_at.end()) {
            const uint64_t ago = frame_no - ev->second.frame;
            if (ago < 600) {
                remade[p] = ago;
                static int told;
                if (told++ < 300)
                    logf("vk: texture #%u %#llx %ux%u NVN %#x made again %llu frames after it was evicted (%s then)", p->id,
                         (unsigned long long)p->gpu, p->width, p->height, p->nvn_format, (unsigned long long)ago,
                         ev->second.rendered ? "rendered" : "not rendered");
            }
            evicted_at.erase(ev);
        }
    }
    static int logged;
    if (logged++ < 40 || textures.size() % 500 == 0 || p->bytes > (64ull << 20))
        logf("vk: texture %#llx %ux%ux%u x%u levels %u NVN %#x -> vk %d%s (%zu textures, %llu MB)",
             (unsigned long long)p->gpu, p->width, p->height, p->depth, p->layers, p->levels, d.format, (int)p->format,
             astc ? " (ASTC decoded)" : "", textures.size(), (unsigned long long)(total_bytes >> 20));
    return p;
}

} // namespace

void full_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

void texture_begin_recording(VkCommandBuffer upload, uint64_t value) {
    up_cmd = upload;
    if (value != rec_value) epoch++;   // (a submission's parts check a texture's memory once: nothing is collected between)
    rec_value = value;
    up_used = false;
    memory_table_begin();
}
VkCommandBuffer upload_command_buffer() {
    return upload_cmd();
}
bool texture_uploads_recorded() {
    memory_table_flush();   // (vk_memory.cpp: the page table's changes, after this recording's mirror copies)
    if (up_used) full_barrier(up_cmd);   // (before the recording that reads them)
    return up_used;
}

int view_nvn_format(const nvn_tex* t, const nvn_view* v) {
    return v && v->set_format ? v->format : t->d.format;
}
unsigned view_level(const nvn_view* v) {
    return v && v->set_levels ? v->base_level : 0;
}
unsigned image_level(const Texture* tex, unsigned level) {
    return level > tex->skip ? std::min(level - tex->skip, tex->levels - 1) : 0;
}
unsigned view_layer(const nvn_view* v) {
    return v && v->set_layers ? v->min_layer : 0;
}

Texture* texture_get(const nvn_tex* t, bool for_sampling) {
    PROF(PROF_TEXGET);
    if (!t || !t->gpu || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return nullptr;
    const VkFormatInfo fi = vk_format(t->d.format);
    if (fi.format == VK_FORMAT_UNDEFINED) {
        static int seen[256];
        if (t->d.format >= 0 && t->d.format < 256 && seen[t->d.format]++ < 2)
            logf("vk: NVN format %#x has no Vulkan format: its textures are left out", t->d.format);
        return nullptr;
    }
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(t->d.format, &bw, &bh);
    if (bpb <= 0) return nullptr;
    Key k{};
    k.gpu = t->gpu;
    k.target = (uint32_t)t->d.target;
    // (a cube map and a 2D array of 6 layers over the same storage are one image; MS targets stay apart)
    if (k.target == NVN_TEXTURE_TARGET_CUBEMAP) k.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    if (k.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) k.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    if (k.target == NVN_TEXTURE_TARGET_RECTANGLE) k.target = NVN_TEXTURE_TARGET_2D;
    k.width = t->d.width, k.height = t->d.height;
    k.depth = t->d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 : t->d.depth;
    if (k.target == NVN_TEXTURE_TARGET_2D) k.depth = 1;
    k.levels = std::max(1u, t->d.levels);
    k.samples = std::max(1, t->d.samples);
    k.layout = nvn_tex_is_linear(&t->d) ? 0x80000000u | (t->d.stride ? t->d.stride : host->tex_linear_stride(&t->d))
                                        : t->L.tileH | t->L.tileD << 4;
    k.cls = format_class(fi.format, t->d.format, bpb, fi.astc);
    Texture* tex;
    auto it = textures.find(k);
    if (it == textures.end()) {
        tex = create(k, t, fi.format, fi.astc, bw, bh, bpb);
        if (!tex) return nullptr;
        static const int64_t log_from = getenv("RX_VK_LOG_MADE") ? atoll(getenv("RX_VK_LOG_MADE")) : -1;
        static int made_logged;
        if (log_from >= 0 && (int64_t)frame_no >= log_from && made_logged++ < 300) {
            auto [a, e] = by_gpu.equal_range(tex->gpu);
            int others = 0;
            for (auto it = a; it != e; ++it) others += it->second != tex;
            logf("vk: made #%u %#llx %ux%ux%u x%u levels %u NVN %#x target %d tileH %u, first %s; %d other images at it",
                 tex->id, (unsigned long long)tex->gpu, t->d.width, t->d.height, t->d.depth, tex->layers, t->d.levels,
                 t->d.format, t->d.target, t->L.tileH, for_sampling ? "sampled" : "rendered", others);
        }
    } else {
        tex = it->second.get();
    }
    return texture_touch(tex, for_sampling);
}

uint64_t texture_destroy_generation() {
    return destroy_gen;
}

uint32_t texture_epoch() {
    return epoch;
}

Texture* texture_touch(Texture* tex, bool for_sampling) {
    tex->last_frame = frame_no;
    tex->last_value = rec_value;
    if (!for_sampling) return tex;
    // contents: from guest memory unless the GPU wrote the image (then it is the content, unless the texture was
    // finalized since and the CPU writes its memory again). What the CPU writes into it after the GPU's write is taken
    // all the same, those ranges alone, the rest staying the GPU's: Eden's texture cache takes a CPU write over the
    // GPU's contents, and MKW's font cache is cleared on the GPU with its glyphs written by the CPU as text appears
    // (the menus were missing every letter added later). Only whole pages of its storage: a write to another variable
    // sharing an edge page once re-uploaded garbage over a drawn LUT (the rainbow sky). Opt-in (RX_VK_CPU_OVER_GPU=1): transient render targets share memory the CPU writes for other things, and took that over their texels, and the menus' letters stayed missing with it.
    if (tex->gpu_written && !tex->released) {
        // (hookshot) and a texture the GPU only cleared: the game's font cache, whose letters the CPU writes as text appears. Its menu text
        // went missing at random (Hookshot issues 40 and 44), whenever the clear reached the image first
        static const bool take = getenv("RX_VK_CPU_OVER_GPU") != nullptr;
        if ((!take && !tex->only_cleared) || tex->checked_epoch == epoch) return tex;
        tex->checked_epoch = epoch;
        static thread_local Ranges cpu, inner;
        if (!texture_dirty(*tex, std::max(tex->upload_gen, tex->gpu_gen), cpu)) return tex;
        const uint64_t first = (0x1000 - (tex->gpu & 0xFFF)) & 0xFFF;
        const uint64_t last = ((tex->gpu + tex->guest_size) & ~0xFFFull) - tex->gpu;
        inner.clear();
        uint64_t bytes = 0;
        for (const auto& r : cpu) {
            const uint64_t a = std::max<uint64_t>(r.first, first), b = std::min<uint64_t>(r.second, last);
            if (a < b) inner.push_back({a, b}), bytes += b - a;
        }
        const uint32_t gen = memory_generation();
        if (!inner.empty() && upload(*tex, &inner)) {
            tex->upload_value = rec_value, tex->upload_seg = tl_seg;
            static int logged;
            if (logged++ < 20)
                logf("vk: texture %#llx %ux%u x%u NVN %#x: %llu KB the CPU wrote after the GPU taken", (unsigned long long)tex->gpu,
                     tex->width, tex->height, tex->layers, tex->nvn_format, (unsigned long long)(bytes >> 10));
        }
        tex->upload_gen = gen;
        return tex;
    }
    {   // (RX_VK_REUPLOAD_DIM=<w>x<h> (debugging): textures that size uploaded whole again every frame: is what a
        // sampled texture misses in guest memory, unseen by the write tracking, or not there at all?)
        static uint32_t rw = 0, rh = 0;
        static const bool parsed = [] {
            if (const char* e = getenv("RX_VK_REUPLOAD_DIM")) sscanf(e, "%ux%u", &rw, &rh);
            return true;
        }();
        (void)parsed;
        static thread_local uint64_t last;
        if (rw && tex->width == rw && tex->height == rh && tex->uploaded && tex->checked_epoch != epoch &&
            last != frame_no) {
            last = frame_no;
            tex->checked_epoch = epoch;
            upload(*tex, nullptr);
            return tex;
        }
    }
    if (tex->uploaded && tex->upload_value == rec_value && tex->upload_seg > tl_seg) {
        // (a parallel replay: uploaded in this submission by a segment after this one, which reads it first in stream
        // order: uploaded again, the same data, in this segment's uploads)
        upload(*tex, nullptr);
        tex->upload_seg = tl_seg;
        return tex;
    }
    if (tex->uploaded && tex->checked_epoch == epoch) return tex;
    tex->checked_epoch = epoch;
    static thread_local Ranges dirty;
    static const bool no_partial = getenv("RX_VK_NO_PARTIAL") != nullptr;   // (A/B: whole re-uploads)
    // (a buffer the GPU wrote over its storage, done since: all of it again; the tiles of MKW's terrain are filled so
    // and stayed the flat placeholder they were made with)
    const bool gpu_dirty = tex->uploaded && gpu_wrote_into(*tex);
    const bool partial = !no_partial && !gpu_dirty && tex->uploaded && !tex->gpu_written && !tex->released;
    // (a texture the GPU drew and the game finalized: only what the CPU wrote since then. Its memory never got the
    // GPU's texels, and a write before (another variable in the same page) re-uploaded garbage over a drawn LUT:
    // the rainbow sky)
    const uint32_t since = tex->gpu_written && tex->released ? std::max(tex->upload_gen, tex->release_gen) : tex->upload_gen;
    if (tex->uploaded && !gpu_dirty && !texture_dirty(*tex, since, dirty)) return tex;
    const uint32_t gen = memory_generation();
    // (storage render targets took last, which the CPU has not written since: nothing to upload, the memory holds
    // nothing the GPU's images did not replace. The game's transient heaps move with the dynamic resolution, and a
    // render target sampled before it is rendered at its new address (the culling reads last frame's shadow map)
    // was unswizzled from that memory: 55 MB for the 3072 x 3072 x 3 shadow map, frame after frame. Not "storage
    // the write watch never saw written": the game fills memory before it makes a pool of it.)
    static const bool upload_all = getenv("RX_VK_UPLOAD_ALL") != nullptr;   // (A/B: such storage uploaded too)
    const uint32_t owned = !tex->uploaded && !upload_all ? gpu_owned_since(tex->gpu, tex->guest_size) : 0;
    watch_tex(tex, owned ? "sampled, over rendered storage" : "sampled, uploaded");
    if (owned && !texture_dirty(*tex, owned, dirty)) {
        tex->uploaded = true;
        tex->upload_gen = gen;
        st_never_written++;
        watch_tex(tex, "NOT uploaded (over storage the GPU rendered)");
        if (remade_log_on()) {   // (made again after its eviction, and sampled empty)
            std::lock_guard lk(evicted_mu);
            auto r = remade.find(tex);
            if (r != remade.end()) {
                static int told;
                if (told++ < 300)
                    logf("vk: texture #%u %#llx %ux%u NVN %#x, made again %llu frames after its eviction, sampled EMPTY "
                         "(storage the GPU rendered, not uploaded) at frame %llu", tex->id, (unsigned long long)tex->gpu,
                         tex->width, tex->height, tex->nvn_format, (unsigned long long)r->second,
                         (unsigned long long)frame_no);
                remade.erase(r);
            }
        }
        return tex;
    }
    if (partial && getenv("RX_VK_LOG_PARTIAL")) {
        static int logged;
        if (logged++ < 200) {
            char line[512];
            int n = snprintf(line, sizeof line, "vk: partial upload %#llx %ux%ux%u x%u levels %u NVN %#x size %llu flags %#x:",
                             (unsigned long long)tex->gpu, tex->width, tex->height, tex->depth, tex->layers, tex->levels,
                             tex->nvn_format, (unsigned long long)tex->guest_size, tex->d.flags);
            for (size_t i = 0; i < dirty.size() && i < 8 && n < 480; i++)
                n += snprintf(line + n, sizeof line - n, " [%#llx,%#llx)", (unsigned long long)dirty[i].first,
                              (unsigned long long)dirty[i].second);
            logf("%s", line);
        }
    }
    if (!tex->uploaded && tex->gpu >= (1ull << 40) && tex->page_map.empty()) {
        // (a sparse texture's mapping as it is uploaded first: its first check after does not take every page for
        // remapped, which uploaded the course maps twice)
        static thread_local Ranges first;
        texture_dirty(*tex, 0, first);
    }
    // (a sparse image, first or whole: only the storage ever written, not all of its gigabytes)
    const bool sparse_written = tex->sparse && !partial;
    if (sparse_written) texture_dirty(*tex, 0, dirty);
    if (tex->gpu_written) {   // (an image the GPU drew, finalized and sampled again: from guest memory, 400 lines)
        static int logged;
        uint64_t kb = 0;
        for (const auto& r : dirty) kb += (r.second - r.first) >> 10;
        if (logged++ < 400)
            logf("vk: upload over a drawn image: #%u %#llx %ux%ux%u x%u NVN %#x at present %llu (texture frame %llu), %s, "
                 "%llu KB dirty in %zu ranges%s", tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->depth,
                 tex->layers, tex->nvn_format, (unsigned long long)(frame_no - 1), (unsigned long long)frame_no,
                 tex->released ? "finalized" : "live", (unsigned long long)kb, dirty.size(), gpu_dirty ? ", a GPU buffer wrote it" : "");
    }
    const auto up0 = std::chrono::steady_clock::now();
    const bool up_ok = upload(*tex, partial || sparse_written ? &dirty : nullptr);
    {   // (an upload over 15 ms: what it was, 60 of them)
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - up0).count();
        static int slow_logged;
        if (ms > 15.0 && slow_logged++ < 60) {
            uint64_t part = 0;
            if (partial) for (const auto& r : dirty) part += r.second - r.first;
            logf("vk: slow upload %.1f ms: #%u %#llx %ux%ux%u x%u levels %u NVN %#x%s%s, %llu of %llu KB%s", ms, tex->id,
                 (unsigned long long)tex->gpu, tex->width, tex->height, tex->depth, tex->layers, tex->levels,
                 tex->nvn_format, tex->astc ? " ASTC" : "", tex->bc ? " (BC3)" : "",
                 (unsigned long long)((partial ? part : tex->guest_size) >> 10), (unsigned long long)(tex->guest_size >> 10),
                 tex->uploaded ? " again" : "");
        }
    }
    if (up_ok) {
        tex->upload_value = rec_value, tex->upload_seg = tl_seg;
        // (its storage joins the submit-time capture: what the game writes into it after a submit is not taken for it)
        // (any size: the capture copies only the pages the CPU wrote, and the big CPU-streamed textures were read as the
        // game had rewritten them a frame or two later. RX_VK_TEX_CAPTURE_MAX_MB=<n>: only up to that, 8 before)
        static const uint64_t cap_max = getenv("RX_VK_TEX_CAPTURE_MAX_MB") ? (uint64_t)atoi(getenv("RX_VK_TEX_CAPTURE_MAX_MB")) << 20 : (2ull << 30);
        if (tex->gpu < (1ull << 40) && tex->guest_size < cap_max) memory_note_texture_upload(tex->gpu, tex->guest_size);
        if (verify_dim().first && tex->width == verify_dim().first && tex->height == verify_dim().second)
            tex->verify_hash = guest_hash(*tex, true);
        if (tex->uploaded) {
            static int logged;
            static std::unordered_map<uint64_t, int> big;
            if (logged++ < 20 || (tex->guest_size > (4u << 20) && big[tex->gpu]++ < 3)) {
                uint64_t part = 0, nz = 0, readable = 0;
                for (const auto& r : dirty) part += r.second - r.first;
                if (partial) {   // (what the CPU wrote: how much of it is not zero, and in which levels)
                    std::vector<uint8_t> b;
                    char lv[96] = "";
                    int lvn = 0;
                    for (const auto& r : dirty) {
                        b.resize((size_t)(r.second - r.first));
                        if (!host->gpu_read(tex->gpu + r.first, b.data(), b.size())) continue;
                        readable += b.size();
                        for (uint8_t c : b) nz += c != 0;
                        for (uint32_t l = 0; l < tex->levels && lvn < 90; l++) {
                            const uint64_t lo = host->tex_level_offset(&tex->d, l);
                            const uint64_t hi = l + 1 < tex->levels ? host->tex_level_offset(&tex->d, l + 1) : tex->guest_size;
                            if (r.first < hi && r.second > lo) lvn += snprintf(lv + lvn, sizeof lv - lvn, " L%u", l);
                        }
                    }
                    logf("vk:   (its written part: %llu of %llu KB readable, %llu bytes not zero; levels%s)",
                         (unsigned long long)(readable >> 10), (unsigned long long)(part >> 10), (unsigned long long)nz, lv);
                }
                logf("vk: texture %#llx %ux%ux%u x%u levels %u NVN %#x (%llu KB of storage) re-uploaded (its memory was "
                     "written; flags %#x): %s, %llu KB of it%s%s", (unsigned long long)tex->gpu, tex->width, tex->height,
                     tex->depth, tex->layers, tex->levels, tex->nvn_format, (unsigned long long)(tex->guest_size >> 10),
                     tex->d.flags, gpu_dirty ? "a GPU write into its storage" : partial ? "what the CPU wrote" : "whole",
                     (unsigned long long)((partial || sparse_written ? part : tex->guest_size) >> 10),
                     tex->gpu_written ? ", over what the GPU wrote" : "", tex->released ? " (finalized since)" : "");
            }
        }
        tex->uploaded = true;
        tex->upload_gen = gen;
        tex->gpu_written = false;
        tex->released = false;
    } else {
        tex->uploaded = true;   // (not retried every draw; a write to its memory tries again)
        tex->upload_gen = gen;
    }
    return tex;
}

VkImageView texture_view(Texture* tex, const nvn_tex* t, const nvn_view* v, VkImageViewType type, ViewUse use,
                         VkFormat* format_out, VkFormat force_format) {
    if (!tex) return VK_NULL_HANDLE;
    const int nf = view_nvn_format(t, v);
    VkFormat f = vk_format(nf).format;
    int from = 0;
    f = widened(f, &from);
    const bool depth = is_depth_format(tex->format);
    if (depth || f == VK_FORMAT_UNDEFINED) f = tex->format;
    if (tex->astc) f = (nf >= 0x87 && nf <= 0x94) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (tex->bc) {
        if (use != ViewUse::Sample) return VK_NULL_HANDLE;
        f = f == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    }
    if (use == ViewUse::Storage) f = force_format != VK_FORMAT_UNDEFINED ? force_format : storage_view_format(f);
    if (use == ViewUse::Attachment && is_astc(f)) return VK_NULL_HANDLE;
    const uint32_t vl = view_level(v);
    uint32_t level0 = image_level(tex, vl);
    uint32_t nlevels = v && v->set_levels && v->nlevels ? (vl + v->nlevels > tex->skip ? vl + v->nlevels - tex->skip - level0 : 1)
                                                       : tex->levels - level0;
    nlevels = std::clamp<uint32_t>(nlevels, 1, tex->levels - level0);
    const uint32_t total_layers = tex->type == VK_IMAGE_TYPE_3D ? 1 : tex->layers;
    uint32_t layer0 = std::min(view_layer(v), total_layers - 1);
    uint32_t nlayers = v && v->set_layers && v->nlayers ? v->nlayers : total_layers - layer0;
    nlayers = std::clamp<uint32_t>(nlayers, 1, total_layers - layer0);
    VkImageAspectFlags aspect = tex->aspect;
    if (use != ViewUse::Attachment && aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (use == ViewUse::Attachment) {
        nlevels = 1, nlayers = 1;
        type = tex->type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
        // (a slice of a 3D target: a 2D view of it, 2D_ARRAY_COMPATIBLE, the slice as its layer)
        if (tex->type == VK_IMAGE_TYPE_3D) layer0 = std::min(view_layer(v), std::max(1u, tex->depth >> level0) - 1);
    } else if (use == ViewUse::Storage) {
        nlevels = 1;
        if (tex->type == VK_IMAGE_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_3D, layer0 = 0, nlayers = 1;
        else if (type == VK_IMAGE_VIEW_TYPE_2D && nlayers > 1) nlayers = 1;
        else if (type == VK_IMAGE_VIEW_TYPE_1D && nlayers > 1) nlayers = 1;
    }
    // the shader's type over the image's: what Vulkan allows of them
    switch (type) {
    case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
        if (tex->type != VK_IMAGE_TYPE_1D) return VK_NULL_HANDLE;
        if (type == VK_IMAGE_VIEW_TYPE_1D) nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_2D: case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        if (tex->type == VK_IMAGE_TYPE_1D) return VK_NULL_HANDLE;
        if (tex->type == VK_IMAGE_TYPE_3D) {
            // (a 2D sample instruction over a 3D texture, as Eden's recompiler keeps the instruction's type: its first
            // slice, one level (VK_EXT_image_2d_view_of_3d). MKW reads a 64 x 64 x 64 texture so, which was sampled
            // as the dummy)
            if (use == ViewUse::Sample && dev.view_2d_of_3d) {
                type = VK_IMAGE_VIEW_TYPE_2D, nlevels = 1, layer0 = 0, nlayers = 1;
                break;
            }
            if (use != ViewUse::Attachment) return VK_NULL_HANDLE;
        }
        if (type == VK_IMAGE_VIEW_TYPE_2D) nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_3D:
        if (tex->type != VK_IMAGE_TYPE_3D) return VK_NULL_HANDLE;
        layer0 = 0, nlayers = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE: case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        if (!tex->cube) return VK_NULL_HANDLE;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE) nlayers = 6;
        else nlayers = std::max(6u, nlayers / 6 * 6);
        if (layer0 + nlayers > tex->layers) return VK_NULL_HANDLE;
        break;
    default: break;
    }
    if (tex->type == VK_IMAGE_TYPE_3D && use == ViewUse::Attachment) type = VK_IMAGE_VIEW_TYPE_2D;
    VkComponentMapping cm{};
    if (use == ViewUse::Sample) {
        const nvn_texdesc& d = t->d;
        unsigned sw[4] = {d.swizzle_r, d.swizzle_g, d.swizzle_b, d.swizzle_a};
        if (v && v->set_swizzle)
            for (int i = 0; i < 4; i++) sw[i] = v->swz[i];
        // (X formats read alpha as one)
        switch (nf) {
        case NVN_FORMAT_RGBX8: case NVN_FORMAT_RGBX8SN: case NVN_FORMAT_RGBX8UI: case NVN_FORMAT_RGBX8I:
        case NVN_FORMAT_RGBX16F: case NVN_FORMAT_RGBX16: case NVN_FORMAT_RGBX16SN: case NVN_FORMAT_RGBX16UI:
        case NVN_FORMAT_RGBX16I: case NVN_FORMAT_RGBX32F: case NVN_FORMAT_RGBX32UI: case NVN_FORMAT_RGBX32I:
        case NVN_FORMAT_RGBX8_SRGB: case NVN_FORMAT_BGRX8: case NVN_FORMAT_BGRX8_SRGB:
            for (unsigned& s : sw)
                if (s == NVN_TEXTURE_SWIZZLE_A) s = NVN_TEXTURE_SWIZZLE_ONE;
            break;
        default: break;
        }
        static const bool swap_9e5 = getenv("RX_VK_9E5_SWAP") != nullptr;   // (hookshot, diagnosis: R and B swapped)
        if (swap_9e5 && nf == NVN_FORMAT_RGB9E5F) std::swap(sw[0], sw[2]);
        cm = {component(sw[0]), component(sw[1]), component(sw[2]), component(sw[3])};
        static const bool log_swizzle = getenv("RX_VK_LOG_SWIZZLE") != nullptr;   // (hookshot: RGB9E5 and R11G11B10 views)
        static int swizzle_logged;
        if (log_swizzle && nf == NVN_FORMAT_RGB9E5F && swizzle_logged++ < 12)
            logf("vk: sampled view of #%u NVN %#x as vk %d: swizzle %u %u %u %u (descriptor %u %u %u %u, view %d)", tex->id,
                 nf, (int)f, sw[0], sw[1], sw[2], sw[3], d.swizzle_r, d.swizzle_g, d.swizzle_b, d.swizzle_a,
                 v && v->set_swizzle ? 1 : 0);
    }
    uint64_t key = mix((uint64_t)use, (uint64_t)type);
    key = mix(key, (uint64_t)f << 32 | aspect);
    key = mix(key, (uint64_t)level0 << 48 | (uint64_t)nlevels << 32 | (uint64_t)layer0 << 16 | nlayers);
    key = mix(key, (uint64_t)cm.r << 24 | (uint64_t)cm.g << 16 | (uint64_t)cm.b << 8 | cm.a);
    if (format_out) *format_out = f;
    auto it = tex->views.find(key);
    if (it != tex->views.end()) return it->second;
    const VkImageUsageFlags usage = use == ViewUse::Sample ? VK_IMAGE_USAGE_SAMPLED_BIT
                                  : use == ViewUse::Storage ? VK_IMAGE_USAGE_STORAGE_BIT
                                  : depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                          : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const VkImageView view = make_view(tex, type, f, aspect, level0, nlevels, layer0, nlayers, cm, usage);
    if (!view) {
        static int logged;
        if (logged++ < 16)
            logf("vk: view of texture %#llx (vk %d) as vk %d type %d levels %u+%u layers %u+%u not made",
                 (unsigned long long)tex->gpu, (int)tex->format, (int)f, (int)type, level0, nlevels, layer0, nlayers);
    }
    tex->views[key] = view;
    return view;
}

bool texture_copy_into_bc(VkCommandBuffer cmd, Texture* src, uint32_t src_level, uint32_t src_layer, int sx, int sy,
                          Texture* dst, uint32_t dst_level, uint32_t dst_layer, int dx, int dy, uint32_t w, uint32_t h,
                          uint32_t layers) {
    if (!src || !dst || !dst->bc || !src->astc || src->bc || !bc_ready() || w > kBcDim || h > kBcDim || (dx & 3) || (dy & 3))
        return false;
    bc_scratch_layout(cmd);
    for (uint32_t l0 = 0; l0 < layers; l0 += kBcLayers) {
        const uint32_t n = std::min(kBcLayers, layers - l0);
        full_barrier(cmd);
        const VkImageCopy cp{{VK_IMAGE_ASPECT_COLOR_BIT, src_level, src_layer + l0, n}, {sx, sy, 0},
                             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, bc_scratch, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        bc_encode_into(cmd, w, h, n, *dst, dst_level, dst_layer + l0, dx, dy);
    }
    return true;
}

// ---- what the GPU wrote into textures of virtual pools, back into their memory ----
// MK World draws its course maps' tiles (the terrain's height and material maps, 16384 x 16384 R8, 8192 x 8192 R16
// and RG16, sparse in virtual pools) into small 512 x 512 render targets in virtual pools of their own, finalizes
// those, unmaps their pools and maps the physical pages into the course map: on the console the texels are in that
// memory. Here they were only in the small targets' images, the course maps read zeros, and the terrain they shape
// was missing in places (ground drawn nowhere, the sky through it, snow left floating). Before virtual pages are
// unmapped or remapped (nvnMemoryPoolMapVirtual, nvnMemoryPoolFinalize: host/nvn_native.c), the textures the GPU
// wrote there are read back and written into their memory, block linear: the course maps then upload the tiles as
// they are mapped. The same when the CPU is about to read memory the GPU may have written (nvnMemoryPoolInvalidate-
// MappedRange, nvnBufferInvalidateMappedRange): MK World reads back a 1 x 1 RGBA32F texture its GPU computes (the
// scene's luminance), which was never in memory. RX_VK_NO_VMAP_FLUSH=1: off.
namespace {
constexpr uint64_t kFlushMax = 16ull << 20;   // (the small ones: render targets that size and more are not)
std::mutex vw_mu;
std::map<uint64_t, uint64_t> vw;   // (storage start -> end of the GPU-written textures of virtual pools that may be flushed)
bool flushable(const Texture& t) {
    if (!t.guest_size || t.guest_size > kFlushMax || t.astc || t.bc || t.sparse || t.skip ||
        t.samples != VK_SAMPLE_COUNT_1_BIT || t.aspect != VK_IMAGE_ASPECT_COLOR_BIT || t.bpb <= 0 || !t.image ||
        (nvn_tex_is_linear(&t.d) && (t.levels != 1 || t.type == VK_IMAGE_TYPE_3D)))   // (linear: one level, rows)
        return false;
    int from = 0;
    widened(t.d.format ? vk_format(t.nvn_format).format : t.format, &from);
    return !from;   // (the image's texels as the storage's: no 3-byte formats kept as four)
}
void vw_note(Texture& t) {
    static const bool off = getenv("RX_VK_NO_VMAP_FLUSH") != nullptr;
    if (off || t.vw_listed || !flushable(t)) return;
    std::lock_guard lk(vw_mu);
    uint64_t& end = vw[t.gpu];
    end = std::max(end, t.gpu + t.guest_size);
    t.vw_listed = true;
}
void vw_forget(const Texture& t) {   // (the entry goes; other listed textures at that start stay listed)
    if (!t.vw_listed) return;
    std::lock_guard lk(vw_mu);
    vw.erase(t.gpu);
    auto [a, b] = by_gpu.equal_range(t.gpu);
    for (auto it = a; it != b; ++it)
        if (it->second != &t && it->second->vw_listed) {
            uint64_t& end = vw[t.gpu];
            end = std::max(end, t.gpu + it->second->guest_size);
        }
}
// (a command buffer of its own for the read-backs, submitted and waited for at once)
VkCommandPool rb_pool = VK_NULL_HANDLE;
VkCommandBuffer rb_cmd = VK_NULL_HANDLE;
VkFence rb_fence = VK_NULL_HANDLE;
bool readback_begin() {
    if (!rb_pool) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = dev.queue_family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(dev.device, &pi, nullptr, &rb_pool) != VK_SUCCESS) return rb_pool = VK_NULL_HANDLE, false;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = rb_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(dev.device, &ai, &rb_cmd) != VK_SUCCESS ||
            vkCreateFence(dev.device, &fi, nullptr, &rb_fence) != VK_SUCCESS)
            return false;
    }
    vkResetCommandPool(dev.device, rb_pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(rb_cmd, &bi) == VK_SUCCESS;
}
bool readback_submit_wait() {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};   // (the copies' writes, for the host)
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(rb_cmd, &d);
    if (vkEndCommandBuffer(rb_cmd) != VK_SUCCESS) return false;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = rb_cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    vkResetFences(dev.device, 1, &rb_fence);
    {
        std::lock_guard qk(dev.present_mu);   // (one queue: submits and presents take turns)
        queue_flush_locked();
        if (vkQueueSubmit2(dev.queue, 1, &si, rb_fence) != VK_SUCCESS) return false;
    }
    return vkWaitForFences(dev.device, 1, &rb_fence, VK_TRUE, 5000000000ull) == VK_SUCCESS;
}
/// The image's levels and layers read back, swizzled as the storage lays them out, and written into its memory
bool flush_texture(Texture& t, uint64_t clip_lo, uint64_t clip_hi) {
    struct Part { uint32_t level, layer; uint64_t at, n; LevelGeo g; };
    std::vector<Part> parts;
    uint64_t total = 0;
    const uint32_t layers = t.type == VK_IMAGE_TYPE_3D ? 1 : t.layers;
    for (uint32_t level = 0; level < t.levels; level++) {
        const LevelGeo g = level_geo(t, level);
        for (uint32_t layer = 0; layer < layers; layer++) {
            const uint64_t n = (uint64_t)g.tw * g.th * g.td * (uint64_t)t.bpb;
            parts.push_back({level, layer, total, n, g});
            total += (n + 15) & ~15ull;
        }
    }
    if (!total) return false;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    if (!create_buffer(total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, buf, alloc, &map) || !map) return false;
    bool ok = readback_begin();
    if (ok) {
        full_barrier(rb_cmd);   // (after what the submissions before wrote)
        std::vector<VkBufferImageCopy> c;
        for (const Part& p : parts) {
            VkBufferImageCopy r{};
            r.bufferOffset = p.at;
            r.bufferRowLength = p.g.tw * (uint32_t)t.block_w;
            r.bufferImageHeight = p.g.th * (uint32_t)t.block_h;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, p.level, t.type == VK_IMAGE_TYPE_3D ? 0 : p.layer, 1};
            r.imageExtent = {p.g.w, p.g.h, p.g.d};
            c.push_back(r);
        }
        vkCmdCopyImageToBuffer(rb_cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, buf, (uint32_t)c.size(), c.data());
        ok = readback_submit_wait();
    }
    if (ok && total <= 64) {   // (RX_VK_LOG_TINY_FLUSH=1: the values of tiny ones, 400 lines: a 1 x 1 luminance)
        static const bool log_tiny = getenv("RX_VK_LOG_TINY_FLUSH") != nullptr;
        static int logged;
        if (log_tiny && logged++ < 400) {
            const float* f = (const float*)map;
            const uint32_t* u = (const uint32_t*)map;
            logf("vk: tiny write-back #%u %#llx NVN %#x: %g %g %g %g (%08x %08x %08x %08x)", t.id, (unsigned long long)t.gpu,
                 t.nvn_format, f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3]);
        }
    }
    // (hookshot) pages the CPU wrote after the GPU last wrote the texture keep the CPU's bytes: on the console the GPU's
    // writes are in memory as it renders, and a later CPU write is what is there. OoT reuses a render target's memory
    // for scene data and then invalidates a range of it: this wrote the whole texture back over that data, and its
    // SceneLowPriority thread read texels as pointers (an access violation at 0xffffffffffffffff)
    std::vector<std::pair<uint64_t, uint64_t>> cpu_runs;   // (offsets from t.gpu)
    if (ok && t.gpu < (1ull << 40)) memory_written_runs(t.gpu, t.guest_size, t.gpu_gen, cpu_runs);
    uint64_t kept_cpu = 0;
    // (and only into the pages being remapped: those are what the texture's content would be lost from. The rest of it stays where
    // the game may have put other things since. The watch above does not catch all of those: OoT's SceneLowPriority crash, walking
    // in Kokiri Forest, was texels landing right beside the remapped pages, on scene data)
    const auto write_back = [&](uint64_t whole_gpu, const uint8_t* whole_src, uint64_t whole_n) {
        const uint64_t from = std::max(whole_gpu, clip_lo), to = std::min(whole_gpu + whole_n, clip_hi);
        if (from >= to) return true;
        const uint64_t gpu = from, n = to - from;
        const uint8_t* src = whole_src + (from - whole_gpu);
        const uint64_t lo = gpu - t.gpu, hi = lo + n;
        bool w = true;
        uint64_t at = lo;
        for (const auto& [a, b] : cpu_runs) {
            if (b <= at) continue;
            if (a >= hi) break;
            if (a > at) w &= host->gpu_write(t.gpu + at, src + (at - lo), a - at) != 0;
            kept_cpu += std::min(b, hi) - std::max(a, at);
            at = std::max(at, b);
            if (at >= hi) break;
        }
        if (at < hi) w &= host->gpu_write(t.gpu + at, src + (at - lo), hi - at) != 0;
        return w;
    };
    const auto log_kept = [&] {
        static int logged;
        if (kept_cpu && logged++ < 20)
            logf("vk: write-back of #%u %#llx: %llu KB the CPU wrote since the GPU kept", t.id, (unsigned long long)t.gpu,
                 (unsigned long long)(kept_cpu >> 10));
    };
    if (ok && nvn_tex_is_linear(&t.d)) {   // (a linear texture: its rows at the stride, one level)
        const uint64_t stride = t.d.stride ? t.d.stride : host->tex_linear_stride(&t.d);
        for (const Part& p : parts) {
            const uint64_t row = (uint64_t)p.g.tw * (uint64_t)t.bpb;
            const uint64_t base = (uint64_t)p.layer * t.L.layer_size;
            for (uint32_t y = 0; y < p.g.th; y++) {
                const uint64_t off = base + (uint64_t)y * stride;
                if (off + row > t.guest_size) break;
                if (!write_back(t.gpu + off, (const uint8_t*)map + p.at + (uint64_t)y * row, row)) ok = false;
            }
        }
        log_kept();
        destroy_buffer(buf, alloc);
        return ok;
    }
    if (ok) {
        std::vector<uint8_t> sw;
        for (const Part& p : parts) {
            const uint64_t at = t.gpu + p.g.offset + (uint64_t)p.layer * t.L.layer_size;
            if (p.g.offset + (uint64_t)p.layer * t.L.layer_size >= t.guest_size) continue;
            const uint64_t span = std::min<uint64_t>(p.g.swizzled, t.guest_size - p.g.offset - (uint64_t)p.layer * t.L.layer_size);
            sw.assign(std::max<uint64_t>(p.g.swizzled, span), 0);
            host->gpu_read(at, sw.data(), span);   // (what is there: the padding between texels kept)
            Tegra::Texture::SwizzleTexture(std::span<uint8_t>(sw.data(), sw.size()),
                                           std::span<const uint8_t>((const uint8_t*)map + p.at, p.n), (u32)t.bpb, p.g.tw,
                                           p.g.th, p.g.td, p.g.tile_h, p.g.tile_d, 1);
            if (!write_back(at, sw.data(), span)) ok = false;   // (marks the pages written: textures over them upload)
        }
        log_kept();
    }
    destroy_buffer(buf, alloc);
    return ok;
}
} // namespace

void texture_describe_written(uint64_t lo, uint64_t size, std::string& out) {
    const uint64_t hi = lo + size;
    int n = 0;
    for (auto& kv : textures) {
        const Texture* t = kv.second.get();
        if (t->gpu >= hi || t->gpu + t->guest_size <= lo || !t->gpu_written) continue;
        if (n++ < 6) {
            char b[160];
            snprintf(b, sizeof b, " #%u %#llx %ux%ux%u x%u NVN %#x %llu KB%s%s;", t->id, (unsigned long long)t->gpu, t->width,
                     t->height, t->depth, t->layers, t->nvn_format, (unsigned long long)(t->guest_size >> 10),
                     t->released ? " finalized" : "", flushable(*t) ? "" : " (not flushable)");
            out += b;
        }
    }
    if (n > 6) out += " (" + std::to_string(n) + " in all)";
}

bool texture_gpu_written_in(uint64_t lo, uint64_t size) {
    const uint64_t hi = lo + size;
    std::lock_guard lk(vw_mu);
    for (auto it = vw.lower_bound(lo > kFlushMax ? lo - kFlushMax : 0); it != vw.end() && it->first < hi; ++it)
        if (it->second > lo) return true;
    return false;
}

int texture_flush_to_guest(uint64_t lo, uint64_t size) {
    const uint64_t hi = lo + size;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Texture*> todo;
    for (auto& kv : textures) {
        Texture* t = kv.second.get();
        if (t->gpu >= hi || t->gpu + t->guest_size <= lo) continue;
        t->vw_listed = false;
        if (t->gpu_written && flushable(*t)) todo.push_back(t);
    }
    {
        std::lock_guard lk(vw_mu);
        for (auto it = vw.lower_bound(lo > kFlushMax ? lo - kFlushMax : 0); it != vw.end() && it->first < hi;)
            it = it->second > lo ? vw.erase(it) : std::next(it);
    }
    int done = 0;
    uint64_t bytes = 0;
    for (Texture* t : todo) {
        if (!flush_texture(*t, lo, hi)) continue;
        done++, bytes += t->guest_size;
        if (t->gpu < lo || t->gpu + t->guest_size > hi) continue;   // (a part of it: the rest is still only in the image)
        // (its memory is its content now: CPU writes to it count again, and an image made over it uploads it)
        t->gpu_written = false;
        t->uploaded = true;
        t->upload_gen = memory_generation();
        watch_tex(t, "written back into its memory");
    }
    static int64_t flushes, all_bytes;
    static double all_ms;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    flushes += done, all_bytes += (int64_t)bytes, all_ms += ms;
    static int logged;
    if (done && (logged++ < 12 || logged % 200 == 0))
        logf("vk: virtual pages %#llx..%#llx remapped: %d texture(s) the GPU wrote there written back into their memory "
             "(%llu KB, %.1f ms; %lld in all, %lld MB, %.0f ms)", (unsigned long long)lo, (unsigned long long)hi, done,
             (unsigned long long)(bytes >> 10), ms, (long long)flushes, (long long)(all_bytes >> 20), all_ms);
    return done;
}

void texture_written(Texture* tex) {
    if (!tex) return;
    if (tex->upload_value == rec_value && tex->upload_seg > tl_seg) {   // (a parallel replay's order broken: see dry_mark_written)
        static int64_t n;
        if (n++ < 40 || n % 500 == 0)
            logf("vk: parallel: #%u %#llx %ux%u x%u NVN %#x written in segment %llu, uploaded by the later segment %llu (%lld so)",
                 tex->id, (unsigned long long)tex->gpu, tex->width, tex->height, tex->layers, tex->nvn_format,
                 (unsigned long long)tl_seg, (unsigned long long)tex->upload_seg, (long long)n);
    }
    vw_note(*tex);
    tex->only_cleared = false;
    if (tex->sparse && !tex->gpu_written) {
        static int logged;
        if (logged++ < 20) logf("vk: sparse texture %#llx written by the GPU (#%u)", (unsigned long long)tex->gpu, tex->id);
    }
    watch_tex(tex, "written");
    if (!tex->gpu_written || tex->released) tex->first_written = frame_no;
    tex->last_written = frame_no;
    tex->gpu_written = true;
    tex->gpu_gen = memory_generation();
    tex->released = false;
    tex->uploaded = true;
}


bool texture_touch_quick(Texture* tex, bool for_sampling) {
    tex->last_frame = frame_no;   // (the same values whichever thread stores them)
    tex->last_value = rec_value;
    if (!for_sampling) return true;
    if (tex->gpu_written && !tex->released && !tex->only_cleared) return true;
    if (tex->upload_value == rec_value && tex->upload_seg > tl_seg) return false;
    return tex->uploaded && tex->checked_epoch == epoch;
}
void texture_gpu_wrote(uint64_t gpu, uint64_t size) {
    if (!gpu || !size) return;
    const uint64_t lo = gpu, hi = gpu + size;
    {   // (RX_VK_WATCH_GPU_WRITE=<lo hex>[:<hi hex>]: the GPU buffer writes reaching that range, by program, 60 lines)
        static uint64_t wlo = 0, whi = 0;
        static const bool parsed = [] {
            if (const char* e = getenv("RX_VK_WATCH_GPU_WRITE")) {
                unsigned long long a = 0, b = 0;
                if (sscanf(e, "%llx:%llx", &a, &b) < 2) b = a + 1;
                wlo = a, whi = b;
            }
            return true;
        }();
        (void)parsed;
        static int logged;
        if (wlo < hi && lo < whi && logged++ < 60)
            logf("vk: watch: GPU buffer write [%#llx, +%#llx) into [%#llx, %#llx), frame %llu, program %016llx",
                 (unsigned long long)lo, (unsigned long long)size, (unsigned long long)wlo, (unsigned long long)whi,
                 (unsigned long long)frame_no, (unsigned long long)tl_watch_prog);
    }
    std::lock_guard lk(gpu_wrote_mu);
    auto it = gpu_wrote.find(lo);
    if (it != gpu_wrote.end() && it->second.first >= hi) {
        it->second.second = rec_value;
        return;
    }
    for (auto e = gpu_wrote.lower_bound(lo); e != gpu_wrote.end() && e->first < hi;)   // (ranges it covers)
        e = e->second.first <= hi ? gpu_wrote.erase(e) : std::next(e);
    gpu_wrote[lo] = {hi, rec_value};
    if (gpu_wrote.size() > 65536) gpu_wrote.erase(gpu_wrote.begin());
}
/// A finished GPU write into the texture's storage it was not uploaded after (and notes it as seen)
bool gpu_wrote_into(Texture& tex) {
    std::lock_guard lk(gpu_wrote_mu);
    if (gpu_wrote.empty()) return false;
    const uint64_t lo = tex.gpu, hi = tex.gpu + std::max<uint64_t>(tex.guest_size, 1);
    uint64_t newest = 0;
    auto it = gpu_wrote.upper_bound(lo);
    if (it != gpu_wrote.begin()) --it;
    for (; it != gpu_wrote.end() && it->first < hi; ++it)
        if (it->second.first > lo && it->second.second <= completed_value && it->second.second > tex.gpu_wrote_seen)
            newest = std::max(newest, it->second.second);
    if (!newest) return false;
    tex.gpu_wrote_seen = newest;
    static int logged;
    if (logged++ < 40)
        logf("vk: texture #%u %#llx %ux%u NVN %#x: the GPU wrote its storage (submission %llu): uploaded again", tex.id,
             (unsigned long long)tex.gpu, tex.width, tex.height, tex.nvn_format, (unsigned long long)newest);
    return true;
}
void texture_watch_prog(uint64_t prog) {
    tl_watch_prog = prog;
}
void texture_set_segment(uint64_t seg) {
    tl_seg = seg;
}
uint64_t texture_segment() {
    return tl_seg;
}
bool texture_marked(const Texture* tex) {
    return !tex || (tex->gpu_written && !tex->released && tex->uploaded && tex->owned_frame == frame_no &&
                    tex->first_render_frame);
}

namespace {
std::mutex sparse_mu;
std::vector<VkSparseImageMemoryBind> sparse_binds;   // (what the next flush binds, under sparse_mu)
std::vector<VkSparseMemoryBind> sparse_opaque;
VkDeviceSize sparse_added = 0;

/// One sparse block's memory (or n bytes) from the texture's chunks; false when none can be had
bool sparse_take(Texture* tex, VkDeviceSize n, VkDeviceMemory& mem, VkDeviceSize& off) {
    constexpr VkDeviceSize kChunk = 64ull << 20;
    n = (n + tex->sparse_align - 1) / tex->sparse_align * tex->sparse_align;
    if (tex->sparse_chunks.empty() || tex->chunk_used + n > kChunk) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = std::max(kChunk, n);
        ai.memoryTypeIndex = tex->sparse_type;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (vkAllocateMemory(dev.device, &ai, nullptr, &m) != VK_SUCCESS) {
            static int logged;
            if (logged++ < 8) logf("vk: sparse texture %#llx: no device memory for its blocks", (unsigned long long)tex->gpu);
            return false;
        }
        tex->sparse_chunks.push_back(m);
        tex->chunk_used = 0;
        tex->bytes += ai.allocationSize;
        sparse_added += ai.allocationSize;
    }
    mem = tex->sparse_chunks.back();
    off = tex->chunk_used;
    tex->chunk_used += n;
    return true;
}

bool sparse_flush(Texture* tex) {
    if (sparse_binds.empty() && sparse_opaque.empty()) return true;
    VkSparseImageMemoryBindInfo ib{tex->image, (uint32_t)sparse_binds.size(), sparse_binds.data()};
    VkSparseImageOpaqueMemoryBindInfo ob{tex->image, (uint32_t)sparse_opaque.size(), sparse_opaque.data()};
    VkBindSparseInfo bi{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    bi.imageBindCount = sparse_binds.empty() ? 0 : 1;
    bi.pImageBinds = &ib;
    bi.imageOpaqueBindCount = sparse_opaque.empty() ? 0 : 1;
    bi.pImageOpaqueBinds = &ob;
    static VkFence fence = VK_NULL_HANDLE;
    if (!fence) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(dev.device, &fi, nullptr, &fence);
    }
    VkResult res;
    {   // (bound before any work that writes the blocks is submitted: waited for here, on the recording thread. And
        // after the work submitted before is done: a sparse bind is not ordered with command buffers by their
        // barriers, and one that landed while a pass sampled the image changed what the pass read part-way through
        // its tiles. Opt-in, RX_VK_SPARSE_IDLE=1: MKW binds hundreds of blocks a race, and the replay's stalls at
        // each made its other races worse)
        static const bool no_idle = getenv("RX_VK_SPARSE_IDLE") == nullptr;
        static int64_t binds;
        std::lock_guard qk(dev.present_mu);
        queue_flush_locked();
        if (!no_idle) vkQueueWaitIdle(dev.queue);
        res = vkQueueBindSparse(dev.queue, 1, &bi, fence);
        if (++binds % 200 == 0) logf("vk: %lld sparse binds", (long long)binds);
    }
    if (res == VK_SUCCESS) {
        vkWaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev.device, 1, &fence);
    } else {
        logf("vk: sparse texture %#llx: binding failed (%d)", (unsigned long long)tex->gpu, (int)res);
    }
    static int logged;
    static uint64_t next_mb = 0;
    if (logged < 20 || (tex->bytes >> 20) >= next_mb) {
        logged++;
        next_mb = (tex->bytes >> 20) + 256;
        logf("vk: sparse texture %#llx: %zu blocks bound (%llu MB of device memory)", (unsigned long long)tex->gpu,
             sparse_binds.size() + sparse_opaque.size(), (unsigned long long)(tex->bytes >> 20));
    }
    total_bytes += sparse_added;
    sparse_added = 0;
    sparse_binds.clear();
    sparse_opaque.clear();
    return res == VK_SUCCESS;
}

/// Queues the binds of what a rectangle reaches that is not bound yet (sparse_mu held)
bool sparse_queue(Texture* tex, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const VkSparseImageMemoryRequirements& r = tex->sparse_req;
    if (level >= tex->levels || layer >= tex->layers) return true;
    if (level >= r.imageMipTailFirstLod) {   // (the mip tail: bound whole)
        const bool single = r.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT;
        uint8_t& bound = tex->tail_bound[single ? 0 : layer];
        if (bound || !r.imageMipTailSize) return true;
        VkDeviceMemory m;
        VkDeviceSize off;
        if (!sparse_take(tex, r.imageMipTailSize, m, off)) return false;
        sparse_opaque.push_back({r.imageMipTailOffset + (single ? 0 : layer * r.imageMipTailStride), r.imageMipTailSize,
                                 m, off, 0});
        bound = 1;
        return true;
    }
    const VkExtent3D g = r.formatProperties.imageGranularity;
    const uint32_t lw = std::max(1u, tex->width >> level), lh = std::max(1u, tex->height >> level);
    if (x >= lw || y >= lh || !w || !h) return true;
    w = std::min(w, lw - x), h = std::min(h, lh - y);
    const uint32_t ntx = (lw + g.width - 1) / g.width, nty = (lh + g.height - 1) / g.height;
    std::vector<uint8_t>& map = tex->tiles[(size_t)layer * tex->levels + level];
    if (map.empty()) map.assign((size_t)ntx * nty, 0);
    bool ok = true;
    for (uint32_t ty = y / g.height; ty <= (y + h - 1) / g.height; ty++)
        for (uint32_t tx = x / g.width; tx <= (x + w - 1) / g.width; tx++) {
            uint8_t& bound = map[(size_t)ty * ntx + tx];
            if (bound) continue;
            VkDeviceMemory m;
            VkDeviceSize off;
            if (!sparse_take(tex, tex->sparse_align, m, off)) {
                ok = false;
                continue;
            }
            VkSparseImageMemoryBind b{};
            b.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer};
            b.offset = {(int32_t)(tx * g.width), (int32_t)(ty * g.height), 0};
            b.extent = {std::min(g.width, lw - tx * g.width), std::min(g.height, lh - ty * g.height), 1};
            b.memory = m;
            b.memoryOffset = off;
            sparse_binds.push_back(b);
            bound = 1;
        }
    return ok;
}
} // namespace

/// (the upload's: queued, and bound together before its copies are recorded)
void texture_resident_queue(Texture* tex, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    std::lock_guard lk(sparse_mu);
    sparse_queue(tex, level, layer, x, y, w, h);
}
void texture_resident_flush(Texture* tex) {
    if (!tex || !tex->sparse) return;
    std::lock_guard lk(sparse_mu);
    sparse_flush(tex);
}

bool texture_resident(Texture* tex, uint32_t level, uint32_t first, uint32_t count, uint32_t x, uint32_t y, uint32_t w,
                      uint32_t h) {
    if (!tex || !tex->sparse) return true;
    std::lock_guard lk(sparse_mu);
    bool ok = true;
    for (uint32_t layer = first; layer < std::min(tex->layers, first + std::max(1u, count)); layer++)
        ok &= sparse_queue(tex, level, layer, x, y, w, h);
    return sparse_flush(tex) && ok;
}

bool texture_resident(Texture* tex, uint32_t first, uint32_t count) {
    if (!tex || !tex->sparse) return true;
    std::lock_guard lk(sparse_mu);
    bool ok = true;
    for (uint32_t layer = first; layer < std::min(tex->layers, first + std::max(1u, count)); layer++)
        for (uint32_t level = 0; level < tex->levels; level++)
            ok &= sparse_queue(tex, level, layer, 0, 0, ~0u, ~0u);
    return sparse_flush(tex) && ok;
}

void texture_rendered(Texture* tex) {
    if (!tex) return;
    if (tex->sparse && !tex->first_render_frame) texture_resident(tex, 0, tex->layers);   // (rendered into: all of it)
    if (!tex->first_render_frame) tex->first_render_frame = frame_no;
    // (its storage's range as the GPU's, once a frame: render targets only. Copies and storage writes registered it
    // too, and a texture made later over such storage (MKW's terrain tiles reuse the slots of a pool) was not uploaded
    // and kept the placeholder it was made with)
    if (tex->owned_frame != frame_no) {
        tex->owned_frame = frame_no;
        const uint64_t lo = tex->gpu, hi = tex->gpu + std::max<uint64_t>(tex->guest_size, 1);
        for (auto it = gpu_owned.lower_bound(lo); it != gpu_owned.end() && it->first < hi;)
            it = it->second.first <= hi ? gpu_owned.erase(it) : std::next(it);
        gpu_owned[lo] = {hi, memory_generation()};
        if (gpu_owned.size() > 65536) gpu_owned.erase(gpu_owned.begin());   // (bounded; the oldest addresses go)
    }
}
uint64_t texture_frame_no() {
    return frame_no;
}

VkBufferView texel_buffer_view(const nvn_tex* t, const nvn_view* v, bool storage) {
    if (!t || !t->gpu) return VK_NULL_HANDLE;
    const int nf = view_nvn_format(t, v);
    VkFormat f = vk_format(nf).format;
    if (f == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(nf, &bw, &bh);
    if (bpb <= 0) return VK_NULL_HANDLE;
    const uint64_t size = (uint64_t)std::max(1u, t->d.width) * bpb;
    // (a formatted store into it: textures over that memory are uploaded again once the GPU is done)
    if (storage) texture_gpu_wrote(t->gpu, size);
    static std::unordered_map<uint64_t, VkBufferView> views;
    const uint64_t key = mix(mix(t->gpu, (uint64_t)f << 1 | storage), size);
    auto it = views.find(key);
    if (it != views.end()) return it->second;
    BufferRef b = gpu_buffer(t->gpu, size);
    VkBufferView bv = VK_NULL_HANDLE;
    if (b && b.offset % std::max<uint64_t>(dev.props.limits.minTexelBufferOffsetAlignment, 1) == 0) {
        VkBufferViewCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
        ci.buffer = b.buffer;
        ci.format = f;
        ci.offset = b.offset;
        ci.range = std::min<uint64_t>(size, (uint64_t)dev.props.limits.maxTexelBufferElements * bpb);
        vkCreateBufferView(dev.device, &ci, nullptr, &bv);
    }
    views[key] = bv;
    return bv;
}

Texture* texture_for_present(const nvn_tex* t) {
    Texture* tex = texture_get(t, false);
    if (tex && tex->gpu_written) return tex;
    Texture* best = nullptr;   // (one rendered at its storage in another description)
    auto [a, b] = by_gpu.equal_range(t ? t->gpu : 0);
    for (auto it = a; it != b; ++it)
        if (it->second->gpu_written && (it->second->aspect & VK_IMAGE_ASPECT_COLOR_BIT) &&
            (!best || it->second->last_frame > best->last_frame))
            best = it->second;
    return best ? best : tex;
}

void texture_frame(uint64_t completed) {
    completed_value = completed;
    frame_no++;
    {   // (staging arenas the GPU is done with, beyond 256 MB of them, and any over 64 MB: freed. A scene change stages
        // hundreds of MB to over a GB (the course maps, the terrain's ASTC arrays), and that host memory was kept)
        static const uint64_t keep = (uint64_t)(getenv("RX_VK_STAGING_KEEP_MB") ? atoi(getenv("RX_VK_STAGING_KEEP_MB")) : 256) << 20;
        uint64_t kept = 0;
        for (auto it = arenas.begin(); it != arenas.end();) {
            const bool idle = it->value <= completed && it->value != rec_value;
            if (idle && (it->size > kArena || kept + it->size > keep)) {
                destroy_buffer(it->buffer, it->allocation);
                it = arenas.erase(it);
                continue;
            }
            kept += it->size;
            ++it;
        }
        if (frame_no % 600 == 0) {   // (the process's memory now and then: what the host side holds)
            PROCESS_MEMORY_COUNTERS_EX pm{};
            pm.cb = sizeof pm;
            if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm))
                logf("vk: memory: working set %llu MB (peak %llu), private %llu MB, staging %llu MB in %zu arenas",
                     (unsigned long long)(pm.WorkingSetSize >> 20), (unsigned long long)(pm.PeakWorkingSetSize >> 20),
                     (unsigned long long)(pm.PrivateUsage >> 20), (unsigned long long)(kept >> 20), arenas.size());
        }
    }
    for (auto it = dead.begin(); it != dead.end();) {
        if (it->value > completed) {
            ++it;
            continue;
        }
        for (VkImageView v : it->views) vkDestroyImageView(dev.device, v, nullptr);
        destroy_image(it->image, it->allocation);
        for (VkDeviceMemory m : it->memory) vkFreeMemory(dev.device, m, nullptr);
        it = dead.erase(it);
    }
    // A budget: RX_VK_VRAM_MB of textures (default 4096), and less when the driver says the process is near what it
    // may have (other programs use the GPU too). The least recently used go first, down to 7/8 of it; nothing the
    // last 8 presents used, nothing recorded work may still read.
    static uint64_t own = 0;
    if (frame_no % 10) return;
    uint64_t used = 0, allowed = 0, block_free = 0;
    const bool known = vram_usage(&used, &allowed, &block_free);
    if (!own)   // (RX_VK_VRAM_MB, else 70% of what the driver allows the process at the first look)
        own = getenv("RX_VK_VRAM_MB") ? (uint64_t)atoi(getenv("RX_VK_VRAM_MB")) << 20
              : known                ? std::min<uint64_t>(allowed / 10 * 7, 6144ull << 20)   // (hookshot: at most 6 GB. A 16 GB
                                                                                             // card's 10.8 GB filled up, read as a leak: 105, 106)
                                     : 4096ull << 20;
    uint64_t budget = (opts.vram_mb > 0) ? ((uint64_t)opts.vram_mb << 20) : own;
    if (opts.vram_mb > 0 && known) {
        // Safety clamp: do not exceed 75% of driver-allowed memory on constrained VRAM (prevents VK_ERROR_DEVICE_LOST)
        budget = std::min(budget, (allowed / 4) * 3);
    }
    // (near what the driver allows, the budget shrinks by the excess. The excess of what the process holds less the
    // free space in VMA's blocks, which new images take before VMA makes a block: measured by the blocks alone, the
    // textures evicted freed space inside them that the driver still counted, so 10 frames later the budget shrank
    // again. A race kept ~3 GB free in the blocks and evicted and made again ~100 textures every 10 frames (1300 at
    // once), mostly the same ones: their uploads and decodes, and images made empty in the frame that drew them.
    // The driver's own count near its limit (97%) still squeezes, whatever the blocks hold free: then it pages VRAM
    // out. RX_VK_BUDGET_BY_BLOCKS=1: as before)
    static const bool by_blocks = getenv("RX_VK_BUDGET_BY_BLOCKS") != nullptr;
    const uint64_t pressure = by_blocks ? used : used - std::min(used, block_free);
    if (known && pressure > allowed / 10 * 9) {
        const uint64_t over = pressure - allowed / 10 * 9;
        budget = std::min(budget, total_bytes > over ? total_bytes - over : 0);
    }
    if (known && !by_blocks && used > allowed / 100 * 97) {
        const uint64_t over = used - allowed / 100 * 97;
        budget = std::min(budget, total_bytes > over ? total_bytes - over : 0);
    }
    if (frame_no % 600 == 0) {
        vma_log_heaps();
        logf("vk: VRAM: %llu MB used of %llu MB allowed (%llu MB of it free in the blocks); textures %llu MB (budget %llu "
             "MB), %zu textures", (unsigned long long)(used >> 20), (unsigned long long)(allowed >> 20),
             (unsigned long long)(block_free >> 20), (unsigned long long)(total_bytes >> 20),
             (unsigned long long)(budget >> 20), textures.size());
        // (what takes it: decoded ASTC, the GPU's images, the rest; the largest)
        uint64_t astc_b = 0, rt_b = 0, other_b = 0, recent_b = 0, baked_b = 0;
        size_t baked_n = 0;
        std::vector<const Texture*> big, baked;
        size_t bucket_n[4]{}, bucket_rel[4]{};
        uint64_t bucket_b[4]{};
        for (auto& kv : textures) {
            const Texture* t = kv.second.get();
            (t->astc ? astc_b : t->gpu_written ? rt_b : other_b) += t->bytes;
            if (frame_no - t->last_frame < 8) recent_b += t->bytes;
            if (t->gpu_written && t->first_written && t->first_written == t->last_written && t->bytes <= (4ull << 20)) {
                if (!t->released) baked_n++, baked_b += t->bytes;
                const int k = t->bytes <= (64u << 10) ? 0 : t->bytes <= (256u << 10) ? 1 : t->bytes <= (1u << 20) ? 2 : 3;
                bucket_n[k]++, bucket_b[k] += t->bytes, bucket_rel[k] += t->released;
                baked.push_back(t);
            }
            big.push_back(t);
        }
        if (getenv("RX_VK_LOG_BAKED")) {   // (what the textures drawn in one frame are: by size, finalized or not)
            logf("vk: drawn in one frame: <=64 KB %zu (%llu KB, %zu finalized), <=256 KB %zu (%llu KB, %zu), <=1 MB %zu "
                 "(%llu KB, %zu), <=4 MB %zu (%llu KB, %zu)", bucket_n[0], (unsigned long long)(bucket_b[0] >> 10),
                 bucket_rel[0], bucket_n[1], (unsigned long long)(bucket_b[1] >> 10), bucket_rel[1], bucket_n[2],
                 (unsigned long long)(bucket_b[2] >> 10), bucket_rel[2], bucket_n[3], (unsigned long long)(bucket_b[3] >> 10),
                 bucket_rel[3]);
            std::sort(baked.begin(), baked.end(), [](const Texture* a, const Texture* b) { return a->bytes > b->bytes; });
            for (size_t i = 0; i < baked.size() && i < 10; i++) {
                const Texture* t = baked[i];
                logf("vk:   one frame: #%u %#llx %ux%ux%u x%u levels %u NVN %#x %llu KB, written at %llu, made at %llu, last "
                     "used %llu%s%s", t->id, (unsigned long long)t->gpu, t->width, t->height, t->depth, t->layers, t->levels,
                     t->nvn_format, (unsigned long long)(t->bytes >> 10), (unsigned long long)t->first_written,
                     (unsigned long long)t->first_seen, (unsigned long long)t->last_frame, t->released ? " finalized" : "",
                     t->first_render_frame ? " rendered" : " copied or stored");
            }
        }
        std::sort(big.begin(), big.end(), [](const Texture* a, const Texture* b) { return a->bytes > b->bytes; });
        logf("vk: textures: decoded ASTC %llu MB, rendered %llu MB (%zu drawn in one frame, kept: %llu KB), other %llu MB; "
             "used in the last 8 presents %llu MB", (unsigned long long)(astc_b >> 20), (unsigned long long)(rt_b >> 20),
             baked_n, (unsigned long long)(baked_b >> 10), (unsigned long long)(other_b >> 20),
             (unsigned long long)(recent_b >> 20));
        for (size_t i = 0; i < big.size() && i < 8; i++)
            logf("vk:   %llu MB: %#llx %ux%ux%u x%u levels %u NVN %#x%s%s, last used %llu presents ago",
                 (unsigned long long)(big[i]->bytes >> 20), (unsigned long long)big[i]->gpu, big[i]->width, big[i]->height,
                 big[i]->depth, big[i]->layers, big[i]->levels, big[i]->nvn_format, big[i]->astc ? " ASTC" : "",
                 big[i]->gpu_written ? " rendered" : "", (unsigned long long)(frame_no - big[i]->last_frame));
    }
    if (verify_dim().first && frame_no % 300 == 0) {
        int checked = 0, changed = 0, missed = 0;
        for (auto& kv : textures) {
            Texture* t = kv.second.get();
            if (t->width != verify_dim().first || t->height != verify_dim().second || !t->verify_hash || t->gpu_written) continue;
            checked++;
            const uint64_t h = guest_hash(*t);
            if (h == t->verify_hash) continue;
            changed++;
            static thread_local Ranges d;
            if (!texture_dirty(*t, t->upload_gen, d)) {
                missed++;
                static int logged;
                if (logged++ < 40) {
                    logf("vk: MISSED WRITE: texture #%u %#llx %ux%u NVN %#x: its memory changed since its upload, the write "
                         "tracking saw nothing", t->id, (unsigned long long)t->gpu, t->width, t->height, t->nvn_format);
                    verify_report(*t);
                }
            }
        }
        logf("vk: verify %ux%u: %d textures, %d changed since uploaded, %d of them unnoticed", verify_dim().first,
             verify_dim().second, checked, changed, missed);
    }
    static const bool no_evict = getenv("RX_VK_NO_EVICT") != nullptr;   // (an experiment: nothing evicted)
    // (hookshot) under the budget, every 600 presents: the render targets drawn over since (below), unused for 120
    const bool sweep = total_bytes <= budget;
    if (no_evict || (sweep && frame_no % 600)) return;
    std::vector<std::pair<uint64_t, Texture*>> order;
    order.reserve(textures.size());
    for (auto& kv : textures) order.push_back({kv.second->last_frame, kv.second.get()});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Texture*> gone;
    // (over the budget, what goes has gone unused for RX_VK_EVICT_MIN_UNUSED frames, 60 by default; it was 8. On an
    // 8 GB laptop card the VRAM sits at the driver's limit, the budget shrinks and this runs every 10 frames: with 8,
    // ~300 textures in a few minutes of Kokiri Forest were evicted and made again, all of them within 60 frames (200
    // within 10), uploaded and decoded again each time; and a 260 x 260 one the GPU renders and the game samples every
    // 40 frames came back empty each time (22 times; the lit ground black for a frame, a tiled pattern over the ground).
    // With 60: 2. The textures that are really done with still go, 52 frames later)
    static const uint64_t min_unused =
        getenv("RX_VK_EVICT_MIN_UNUSED") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_EVICT_MIN_UNUSED"))) : 60;
    for (auto& [last, tex] : order) {
        if ((!sweep && total_bytes <= budget / 8 * 7) || frame_no - last < (sweep ? std::max<uint64_t>(120, min_unused) : min_unused)) break;
        if (tex->last_value > completed) continue;
        // (nor what the GPU wrote and the game still has: the image is its only copy, and one made again over that
        // storage started empty or from stale memory: a sky or course texture drawn once, garbage from then on.
        // RX_VK_EVICT_RENDERED=1: as before)
        static const bool evict_rendered = getenv("RX_VK_EVICT_RENDERED") != nullptr;
        // (one unused for 3600 presents (a minute) goes: a course left behind; the game draws it again when it comes
        // back to that course. Without it a course change kept 1.4 GB arrays and VRAM ran over)
        // (600 presents when the process is over what the driver allows: then the driver pages VRAM to system memory)
        const uint64_t keep_rendered = known && used > allowed ? 600 : 3600;
        // (hookshot) a render target another image has since been drawn over (same storage, another size or format:
        // OoT's dynamic resolution makes a new set at each size in the same pool memory) holds nothing any more: it
        // goes like any other. Kept, they were 3.6 GB of 4.5 GB rendered, VRAM near the driver's limit, and the
        // driver's paging made submissions of seconds (5-9 fps for a minute)
        bool overwritten = false;
        if (tex->gpu_written) {
            auto [a, e] = by_gpu.equal_range(tex->gpu);
            for (auto o = a; o != e && !overwritten; ++o)
                overwritten = o->second != tex && o->second->last_written > tex->last_frame;
        }
        if (!overwritten && tex->gpu_written && !tex->released && !evict_rendered && frame_no - last < keep_rendered) continue;
        if (sweep && !overwritten) continue;
        // (and a small one never: its texels exist nowhere else, and it is made once and kept by the game. The sky's
        // 256 x 256 lookup table, unused while a menu was up, went and came back from memory as garbage: the rainbow
        // sky. The big ones that may go are a course left behind)
        // (a render target, drawn in the frames it was used in, goes: the dynamic resolution makes a new set of ~20 at
        // each size, and the old ones are not used again)
        const bool drawn_once = tex->last_written + 8 < tex->last_frame;
        if (!overwritten && tex->gpu_written && !tex->released && !evict_rendered && tex->bytes < (64ull << 20) && drawn_once) continue;
        // (nor a small one the GPU wrote in one frame alone: a table the game bakes once and samples now and then. The
        // pause screen's colour grading, 8 x 8 x 8, its slices drawn at boot and sampled next when the game is paused,
        // went after a minute unused, and the one made again over storage only the GPU had written stayed empty: the
        // pause blur black. Not a finalized one: the transient targets the game makes and finalizes every frame are
        // each "written in one frame", and kept so they added up, ~20 MB a minute)
        const bool baked = tex->first_written && tex->first_written == tex->last_written && tex->bytes <= (4ull << 20);
        if (!overwritten && tex->gpu_written && !tex->released && !evict_rendered && baked) continue;
        watch_tex(tex, "evicted");
        if (remade_log_on()) {
            std::lock_guard lk(evicted_mu);
            if (evicted_at.size() > 20000) evicted_at.clear();   // (a bound: only the recent ones matter)
            evicted_at[evicted_key(tex->gpu, tex->width, tex->height, tex->nvn_format)] = {frame_no, tex->gpu_written};
            remade.erase(tex);
        }
        destroy_later(*tex);
        forget_gpu(tex);
        gone.push_back(tex);
    }
    for (auto it = textures.begin(); it != textures.end() && !gone.empty();) {
        auto g = std::find(gone.begin(), gone.end(), it->second.get());
        if (g != gone.end()) {
            gone.erase(g);
            it = textures.erase(it);
        } else {
            ++it;
        }
    }
    const size_t n = order.size() - textures.size();
    if (n) logf("vk: %zu textures retired (%zu left, %llu MB)", n, textures.size(), (unsigned long long)(total_bytes >> 20));
}

void texture_log_guest(const Texture* t) {
    if (!t || !t->guest_size) return;
    std::vector<uint8_t> b(65536);
    char lv[512] = "";
    int k = 0;
    uint64_t all_nz = 0, all_unmapped = 0;
    for (uint32_t l = 0; l < std::max(1u, t->d.levels) && l < 16; l++) {
        const uint64_t lo = host->tex_level_offset(&t->d, l);
        const uint64_t hi = l + 1 < std::max(1u, t->d.levels) ? host->tex_level_offset(&t->d, l + 1) : t->guest_size;
        uint64_t nz = 0, unmapped = 0, pages = 0, empty_pages = 0;
        for (uint64_t off = lo; off < hi; off += 65536) {
            const uint64_t n = std::min<uint64_t>(65536, hi - off);
            if (!host->gpu_read(t->gpu + off, b.data(), n)) { unmapped += n; continue; }
            uint64_t page_nz = 0;
            for (uint64_t i = 0; i < n; i++) page_nz += b[i] != 0;
            nz += page_nz, pages++, empty_pages += !page_nz;
        }
        all_nz += nz, all_unmapped += unmapped;
        if (k < 480)
            k += snprintf(lv + k, sizeof lv - k, " L%u: %llu KB, %llu not zero, %llu of %llu mapped pages empty%s;", l,
                          (unsigned long long)((hi - lo) >> 10), (unsigned long long)nz, (unsigned long long)empty_pages,
                          (unsigned long long)pages, unmapped ? " (part unmapped)" : "");
    }
    const uint64_t g0 = host->gpu_guest(t->gpu);
    logf("vk: dump guest memory of #%u %#llx %ux%u NVN %#x (guest %#llx%s): %llu bytes not zero, %llu KB unmapped;%s", t->id,
         (unsigned long long)t->gpu, t->width, t->height, t->nvn_format, (unsigned long long)g0,
         t->gpu >= (1ull << 40) ? ", a virtual pool's" : "", (unsigned long long)all_nz,
         (unsigned long long)(all_unmapped >> 10), lv);
}

void texture_released(const nvn_tex* t) {
    if (!t) return;
    auto [a, b] = by_gpu.equal_range(t->gpu);
    for (auto it = a; it != b; ++it)
        if (it->second->gpu_written && !it->second->released) {
            it->second->released = true, it->second->release_gen = memory_generation();
            watch_tex(it->second, "finalized");
            static int logged;   // (a big one: the course maps' kind)
            if (it->second->bytes >= (32ull << 20) && logged++ < 40)
                logf("vk: texture #%u %#llx %ux%u x%u NVN %#x (%llu MB, rendered or copied into) finalized by the game",
                     it->second->id, (unsigned long long)it->second->gpu, it->second->width, it->second->height,
                     it->second->layers, it->second->nvn_format, (unsigned long long)(it->second->bytes >> 20));
        }
}

uint64_t texture_made() {
    return st_made;
}
void texture_unswizzle_stats(int64_t* bytes, int64_t* ns) {
    *bytes = st_unswizzle_bytes, *ns = st_unswizzle_ns;
}
size_t texture_count() {
    return textures.size();
}
uint64_t texture_bytes() {
    return total_bytes;
}
void texture_stats(int64_t* uploads, int64_t* astc_gpu, int64_t* bytes_uploaded) {
    *uploads = st_uploads, *astc_gpu = st_astc, *bytes_uploaded = st_bytes;
}

/// ReadTexels / WriteTexels: the CPU's copy of a region in guest memory (block linear, the Switch 2 GOB swizzle).
/// A write also stamps the pages, so the image is uploaded again; a texture the GPU wrote is read from guest memory
/// as it is (its newest texels are in the image: not yet read back).
void texture_read_texels(const nvn_tex* t, unsigned level, unsigned layer0, const NVNcopyRegion* r, uint8_t* data,
                         uint64_t row, uint64_t img, bool write) {
    if (!t || !t->gpu || !r) return;
    {   // (RX_VK_WATCH_RANGE: ReadTexels / WriteTexels of textures overlapping it, 400 lines)
        static const std::pair<uint64_t, uint64_t> range = [] {
            unsigned long long a = 0, b = 0;
            if (const char* e = getenv("RX_VK_WATCH_RANGE")) sscanf(e, "%llx:%llx", &a, &b);
            return std::pair<uint64_t, uint64_t>{a, b};
        }();
        static int logged;
        if (range.second && t->gpu < range.second && t->gpu + std::max<uint64_t>(t->L.size, 1) > range.first && logged++ < 400)
            logf("vk: watch range %s %#llx %ux%u NVN %#x level %u layer %u region (%d,%d %dx%d) mapped %s", write ? "WriteTexels" : "ReadTexels",
                 (unsigned long long)t->gpu, t->d.width, t->d.height, t->d.format, level, layer0, r->xoffset, r->yoffset,
                 r->width, r->height, host->gpu_guest(t->gpu + host->tex_level_offset(&t->d, level)) ? "(level start yes)" : "(level start no)");
    }
    int bw = 1, bh = 1;
    const int bpb = nvn_block_size(t->d.format, &bw, &bh);
    if (bpb <= 0) return;
    const nvn_texdesc& d = t->d;
    const uint32_t lw = (std::max(1u, d.width >> level) + bw - 1) / bw;
    const uint32_t lh = d.target == NVN_TEXTURE_TARGET_1D || d.target == NVN_TEXTURE_TARGET_1D_ARRAY
                            ? 1 : (std::max(1u, d.height >> level) + bh - 1) / bh;
    const bool vol = d.target == NVN_TEXTURE_TARGET_3D;
    const uint64_t base = t->gpu + host->tex_level_offset(&d, level);
    unsigned th = 0, td = 0;
    host->tex_level_tiles(&d, level, &th, &td);
    const uint32_t x0 = (uint32_t)std::max(0, r->xoffset) / bw, y0 = (uint32_t)std::max(0, r->yoffset) / bh;
    const uint32_t w = (uint32_t)(std::max(0, r->width) + bw - 1) / bw, h = (uint32_t)(std::max(0, r->height) + bh - 1) / bh;
    const uint32_t z0 = (uint32_t)std::max(0, r->zoffset), dd = (uint32_t)std::max(1, r->depth);
    const uint64_t pitch = row ? row : (uint64_t)w * bpb, image = img ? img : pitch * h;
    const bool linear = nvn_tex_is_linear(&d);
    const uint32_t stride = linear ? (d.stride ? d.stride : host->tex_linear_stride(&d)) : 0;
    const uint64_t gobs_x = ((uint64_t)lw * bpb + 63) >> 6;
    const uint64_t rows_y = (lh + (8u << th) - 1) >> (3 + th);
    const uint64_t block = 512ull << (th + td);
    for (uint32_t k = 0; k < dd; k++) {
        const uint32_t z = vol ? z0 + k : 0;
        const uint64_t layer_base = base + (vol ? 0 : (uint64_t)(layer0 + z0 + k) * t->L.layer_size);
        for (uint32_t y = 0; y < h && y0 + y < lh; y++) {
            uint8_t* line = data + k * image + y * pitch;
            const uint32_t yy = y0 + y;
            for (uint32_t xb = 0; xb < (uint64_t)w * bpb;) {
                const uint32_t xbyte = x0 * bpb + xb;
                uint64_t off;
                uint32_t run;
                if (linear) {
                    off = (uint64_t)yy * stride + xbyte;
                    run = w * bpb - xb;
                } else {   // (16 bytes of a GOB row are contiguous)
                    const uint64_t gob = ((xbyte & 63) >> 5) << 8 | ((yy & 7) >> 2) << 7 | ((xbyte & 31) >> 4) << 6 |
                                         ((yy >> 1) & 1) << 5 | (yy & 1) << 4 | (xbyte & 15);
                    off = (uint64_t)(z >> td) * gobs_x * rows_y * block + (uint64_t)(yy >> (3 + th)) * gobs_x * block +
                          (uint64_t)(xbyte >> 6) * block + ((uint64_t)(z & ((1u << td) - 1)) << (9 + th)) +
                          ((uint64_t)((yy >> 3) & ((1u << th) - 1)) << 9) + gob;
                    run = std::min<uint32_t>(16 - (xbyte & 15), w * bpb - xb);
                }
                if (write) host->gpu_write(layer_base + off, line + xb, run);
                else host->gpu_read(layer_base + off, line + xb, run);
                xb += run;
            }
        }
    }
    if (write) {
        const uint64_t guest = host->gpu_guest(t->gpu);
        if (guest) memory_stamp(guest, t->L.size);
        auto [a, b] = by_gpu.equal_range(t->gpu);
        for (auto it = a; it != b; ++it) it->second->gpu_written = false, it->second->checked_epoch = 0;
    }
}

} // namespace mkwvk
