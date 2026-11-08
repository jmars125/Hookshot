// The content rectangle for a window present, independent of Vulkan.
#pragma once

#include <cstdint>

namespace mkwvk {

struct PresentRect {
    uint32_t x, y, w, h;
};

struct PresentContentSize {
    uint32_t w, h;
};

constexpr PresentContentSize present_content_size(uint32_t sw, uint32_t sh, const int crop[4]) noexcept {
    if (!sw || !sh) return {0, 0};
    if (!crop || crop[2] <= 0 || crop[3] <= 0) return {sw, sh};
    if (crop[0] < 0 || crop[1] < 0 || (uint32_t)crop[0] >= sw || (uint32_t)crop[1] >= sh) return {0, 0};
    const uint32_t available_w = sw - (uint32_t)crop[0];
    const uint32_t available_h = sh - (uint32_t)crop[1];
    const uint32_t crop_w = (uint32_t)crop[2];
    const uint32_t crop_h = (uint32_t)crop[3];
    return {crop_w < available_w ? crop_w : available_w, crop_h < available_h ? crop_h : available_h};
}

constexpr PresentRect present_rect(uint32_t sw, uint32_t sh, const int crop[4], uint32_t dw, uint32_t dh,
                                   bool preserve_stretch = false) noexcept {
    if (!dw || !dh) return {0, 0, dw, dh};
    if (preserve_stretch) return {0, 0, dw, dh};
    const PresentContentSize content = present_content_size(sw, sh, crop);
    if (!content.w || !content.h) return {0, 0, dw, dh};

    // Fit the source content into the destination while preserving its aspect.
    if ((uint64_t)dw * content.h > (uint64_t)dh * content.w) {
        const uint32_t w = (uint32_t)((uint64_t)dh * content.w / content.h);
        return {(dw - w) / 2, 0, w ? w : 1, dh};
    }
    const uint32_t h = (uint32_t)((uint64_t)dw * content.h / content.w);
    return {0, (dh - h) / 2, dw, h ? h : 1};
}

}  // namespace mkwvk
