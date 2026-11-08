// Game-free regression for the production Vulkan presentation rectangle helper.
#include <cstdio>

#include "../vk_present_rect.h"

namespace {
int failures = 0;

void expect(const char* name, mkwvk::PresentRect got, mkwvk::PresentRect want) {
    if (got.x == want.x && got.y == want.y && got.w == want.w && got.h == want.h) return;
    std::fprintf(stderr, "%s: got {%u,%u,%u,%u}, wanted {%u,%u,%u,%u}\n", name, got.x, got.y, got.w, got.h,
                 want.x, want.y, want.w, want.h);
    failures++;
}

}  // namespace

int main() {
    using mkwvk::PresentRect;
    const PresentRect fit_16_10 = mkwvk::present_rect(1920, 1080, nullptr, 1600, 1000);
    expect("16:9 into 16:10", fit_16_10, {0, 50, 1600, 900});
    expect("16:9 same aspect", mkwvk::present_rect(1920, 1080, nullptr, 1920, 1080), {0, 0, 1920, 1080});
    expect("explicit ultrawide camera mode retains fill",
           mkwvk::present_rect(1920, 1080, nullptr, 3440, 1440, true), {0, 0, 3440, 1440});
    expect("ultrawide source aspect fits naturally",
           mkwvk::present_rect(3440, 1440, nullptr, 3440, 1440), {0, 0, 3440, 1440});
    expect("ultrawide source remains wider than 16:9",
           mkwvk::present_rect(3440, 1440, nullptr, 1600, 1000), {0, 165, 1600, 669});
    expect("portrait target", mkwvk::present_rect(1920, 1080, nullptr, 1000, 1600), {0, 519, 1000, 562});
    expect("odd resized destination", mkwvk::present_rect(1920, 1080, nullptr, 1601, 1001), {0, 50, 1601, 900});

    const int crop_16_9[4] = {160, 90, 2240, 1260};
    expect("cropped 16:9 content", mkwvk::present_rect(2560, 1440, crop_16_9, 1600, 1000), {0, 50, 1600, 900});
    const int crop_odd[4] = {7, 11, 1900, 1069};
    expect("odd crop and resized target", mkwvk::present_rect(1920, 1080, crop_odd, 1600, 1000), {0, 50, 1600, 900});
    const int crop_at_edge[4] = {90, 70, 20, 20};
    expect("small crop clamps to source edge", mkwvk::present_rect(100, 80, crop_at_edge, 160, 100), {30, 0, 100, 100});
    expect("zero source falls back to destination", mkwvk::present_rect(0, 0, nullptr, 160, 100), {0, 0, 160, 100});
    return failures ? 1 : 0;
}
