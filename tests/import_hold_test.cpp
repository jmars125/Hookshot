#include "import_hold.h"
#include <cstdio>

static int fail(int n) {
    std::fprintf(stderr, "import hold check %d failed\n", n);
    return n;
}

int main() {
    mkwvk::ImportHolds h;
    if (!h.allow(1, 2, 0)) return fail(1);
    h.deny_for(1, 2, 0);
    if (h.allow(1, 2, mkwvk::ImportHolds::kGapMs - 1)) return fail(2);
    if (!h.allow(1, 2, mkwvk::ImportHolds::kGapMs)) return fail(3);
    if (!h.allow(3, 4, 100)) return fail(4);
    h.deny_for(1, 2, 5000);
    if (h.allow(1, 2, 5000 + mkwvk::ImportHolds::kGapMs - 1)) return fail(5);

    mkwvk::ImportHolds capped;
    for (uint64_t i = 0; i < mkwvk::ImportHolds::kMax + 6; i++) capped.deny_for(i, i + 1, 0);
    if (capped.rows.size() != mkwvk::ImportHolds::kMax) return fail(6);
    if (!capped.allow(0, 1, 0)) return fail(7);
    const uint64_t newest = mkwvk::ImportHolds::kMax + 5;
    if (capped.allow(newest, newest + 1, 0)) return fail(8);
    return 0;
}
