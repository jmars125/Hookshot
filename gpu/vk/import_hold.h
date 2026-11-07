#pragma once
// (hookshot) A host-memory import that failed is not tried again for a short while. The same range was imported
// again on every use after vkAllocateMemory failed, each attempt 7-130 ms, about 40 times a second (issue 218).
#include <algorithm>
#include <cstdint>
#include <vector>

namespace mkwvk {

struct ImportHolds {
    static constexpr int64_t kGapMs = 2000;
    static constexpr size_t kMax = 256;
    struct Row {
        uint64_t lo, hi;
        int64_t until_ms;
    };
    std::vector<Row> rows;

    void drop_expired(int64_t now_ms) {
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& r) { return r.until_ms <= now_ms; }), rows.end());
    }
    // true: try the import. false: this range failed recently.
    bool allow(uint64_t lo, uint64_t hi, int64_t now_ms) {
        drop_expired(now_ms);
        for (const Row& r : rows)
            if (r.lo == lo && r.hi == hi) return false;
        return true;
    }
    void deny_for(uint64_t lo, uint64_t hi, int64_t now_ms) {
        drop_expired(now_ms);
        for (Row& r : rows)
            if (r.lo == lo && r.hi == hi) {
                r.until_ms = now_ms + kGapMs;
                return;
            }
        if (rows.size() >= kMax) rows.erase(rows.begin());
        rows.push_back(Row{lo, hi, now_ms + kGapMs});
    }
};

} // namespace mkwvk
