// GPU profile (vk_gprof.h): a query pool of timestamp pairs per recording slot.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "vk_gprof.h"

namespace mkwvk {
namespace {

constexpr int kSlots = 48;                // (vk_replay.cpp's kRecordings)
constexpr uint32_t kPairs = 1024;         // spans a recording can time
VkQueryPool pool = VK_NULL_HANDLE;
struct Slot {
    std::vector<std::string> labels;      // one a closed span
    bool open = false;
    bool used = false;
};
Slot slots[kSlots];
int cur = -1;
std::unordered_map<std::string, std::pair<double, int64_t>> totals;   // label -> (ns, spans)

} // namespace

bool gprof_on() {
    static const bool on = getenv("RX_VK_GPU_PROFILE") && atoi(getenv("RX_VK_GPU_PROFILE")) != 0;
    return on;
}

void gprof_begin_recording(int slot, VkCommandBuffer cmd) {
    if (!gprof_on() || slot < 0 || slot >= kSlots) return;
    if (!pool) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kSlots * kPairs * 2;
        if (vkCreateQueryPool(dev.device, &qi, nullptr, &pool) != VK_SUCCESS) return;
        logf("vk: GPU profile on (RX_VK_GPU_PROFILE)");
    }
    cur = slot;
    Slot& s = slots[slot];
    s.labels.clear();
    s.open = false;
    s.used = true;
    vkCmdResetQueryPool(cmd, pool, slot * kPairs * 2, kPairs * 2);
}

void gprof_begin(VkCommandBuffer cmd) {
    if (!pool || cur < 0) return;
    Slot& s = slots[cur];
    if (s.open || s.labels.size() >= kPairs) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, (uint32_t)(cur * kPairs + s.labels.size()) * 2);
    s.open = true;
}

void gprof_end(VkCommandBuffer cmd, const std::string& label) {
    if (!pool || cur < 0) return;
    Slot& s = slots[cur];
    if (!s.open) return;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool,
                         (uint32_t)(cur * kPairs + s.labels.size()) * 2 + 1);
    s.labels.push_back(label);
    s.open = false;
}

void gprof_collect(int slot) {
    if (!pool || slot < 0 || slot >= kSlots) return;
    Slot& s = slots[slot];
    if (!s.used || s.labels.empty()) return;
    std::vector<uint64_t> t(s.labels.size() * 2);
    if (vkGetQueryPoolResults(dev.device, pool, slot * kPairs * 2, (uint32_t)t.size(), t.size() * 8, t.data(), 8,
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    const double period = dev.props.limits.timestampPeriod;
    for (size_t i = 0; i < s.labels.size(); i++) {
        if (t[2 * i + 1] <= t[2 * i]) continue;
        auto& e = totals[s.labels[i]];
        e.first += (double)(t[2 * i + 1] - t[2 * i]) * period;
        e.second++;
    }
    s.labels.clear();
    s.used = false;
}

void gprof_report(int frames) {
    if (!pool || totals.empty()) return;
    std::vector<std::pair<std::string, std::pair<double, int64_t>>> v(totals.begin(), totals.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    double all = 0;
    for (const auto& e : v) all += e.second.first;
    logf("vk: GPU profile: %.2f ms a frame in %zu kinds of span; the most:", all / frames / 1e6, v.size());
    for (size_t i = 0; i < v.size() && i < 25; i++)
        logf("vk:   %6.3f ms  %5.1f a frame  %s", v[i].second.first / frames / 1e6, (double)v[i].second.second / frames,
             v[i].first.c_str());
    totals.clear();
}

} // namespace mkwvk
