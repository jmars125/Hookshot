"""The FPS overlay writes only after the game-image transfer has finished.

python -I -B tests/test_fps_overlay_order.py [--cxx g++] [--source path/to/vk_menu.cpp]
Executes the real menu_record with recording command stubs; no GPU or game data.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--source", type=Path, default=ROOT / "gpu/vk/vk_menu.cpp")
    a = p.parse_args()
    fn = block(a.source.read_text(encoding="utf-8"), "void menu_record(")
    fixture = r'''#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using VkCommandBuffer = int;
using VkImage = int;
constexpr int VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 = 1;
constexpr int VK_STRUCTURE_TYPE_DEPENDENCY_INFO = 2;
constexpr uint64_t VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT = 1;
constexpr uint64_t VK_ACCESS_2_TRANSFER_WRITE_BIT = 2;
struct VkMemoryBarrier2 {
    int sType; uint64_t srcStageMask = 0, dstStageMask = 0, srcAccessMask = 0, dstAccessMask = 0;
};
struct VkDependencyInfo {
    int sType; unsigned memoryBarrierCount = 0; const VkMemoryBarrier2* pMemoryBarriers = nullptr;
};
struct { bool show_fps = false; } opts;
struct Bitmap { uint32_t w, h; } fps_bmp{120, 54}, menu_bmp{360, 500};
struct Upload {} ups[2][3];
std::mutex bmp_mu;
std::atomic<bool> open_now{false};
int cur_scale = 1, copies = 0, barriers = 0;
bool transfer_pending = true;
void vkCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* di) {
    CHECK(cmd == 1 && di->sType == VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    CHECK(di->memoryBarrierCount == 1 && di->pMemoryBarriers);
    const auto& b = *di->pMemoryBarriers;
    CHECK(b.sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
    CHECK((b.srcStageMask & VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT) != 0);
    CHECK((b.dstStageMask & VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT) != 0);
    CHECK((b.srcAccessMask & VK_ACCESS_2_TRANSFER_WRITE_BIT) != 0);
    CHECK((b.dstAccessMask & VK_ACCESS_2_TRANSFER_WRITE_BIT) != 0);
    transfer_pending = false;
    ++barriers;
}
void copy_over(VkCommandBuffer cmd, VkImage dst, Bitmap&, Upload&, int, int, uint32_t, uint32_t) {
    CHECK(cmd == 1 && dst == 2);
    CHECK(!transfer_pending); // a previous blit/clear or overlay must finish first
    transfer_pending = true;
    ++copies;
}
''' + fn + r'''
int main() {
    for (bool fps : {false, true}) for (bool menu : {false, true}) {
        opts.show_fps = fps;
        open_now = menu;
        copies = barriers = 0;
        transfer_pending = true; // the game image has just been blitted/cleared
        menu_record(1, 2, 0, 1280, 720);
        CHECK(copies == int(fps) + int(menu));
        if (!fps && !menu) CHECK(barriers == 0);
    }
    std::puts("PASS: FPS-only, menu-only, both overlays and both disabled");
}
'''
    with tempfile.TemporaryDirectory(prefix="fps-overlay-order-") as tmp:
        cpp, exe = Path(tmp) / "fixture.cpp", Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
