// (hookshot) Every DEPBAR form lowers, to nothing. Until the AMD fixes the Ampere -> Maxwell lowering accepted
// DEPBAR.LE SB5, n alone and rejected the waits OoT's programs use, DEPBAR.LE SB0, 0x0, {1,2,3,4} ("native dependency
// barrier mode"), so three compute programs and a vertex program never ran. The encodings here are built from the
// fields (.LE bit 47, the scoreboard 44..46, the count 38..43, the set of scoreboards 32..37), not taken from a program.
//   lower_depbar_test      (built and run by tests/test_lower_depbar.py)
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using Shader::Ampere::Instruction;

namespace {

// @PT EXIT, unconditional: the end every captured program needs. The control word (bits 96..127) is not read.
constexpr Instruction kExit{{0x0000794d, 0x00000000, 0x03800000, 0x000fea00}};

// @PT DEPBAR with the given fields
Instruction depbar(bool le, u32 scoreboard, u32 count, u32 set) {
    return Instruction{{0x791a, (u32{le} << 15) | (scoreboard << 12) | (count << 6) | set, 0, 0x000fc600}};
}

int failed = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "lower_depbar: %s\n", what);
    failed++;
}

// The barrier then an EXIT lower, and the barrier adds no instruction.
void lowers(Instruction barrier, const char* what) {
    const std::vector<Instruction> code{barrier, kExit};
    const auto r = Shader::Ampere::LowerToMaxwell(code);
    char text[200];
    std::snprintf(text, sizeof text, "%s: %s", what, r ? "lowered to more than the EXIT" : std::string(r.error).c_str());
    check(r && r.instructions.size() == 1, text);
}

// A word with unused bits set is still a rejected mode, not a silent pass.
void rejected(Instruction barrier, const char* what) {
    const std::vector<Instruction> code{barrier, kExit};
    const auto r = Shader::Ampere::LowerToMaxwell(code);
    check(!r && r.error == std::string_view{"native dependency barrier mode"}, what);
}

} // namespace

int main() {
    lowers(depbar(true, 5, 1, 0), "DEPBAR.LE SB5, 0x1 (the one form accepted at first)");
    lowers(depbar(true, 0, 0, 0x1e), "DEPBAR.LE SB0, 0x0, {1,2,3,4} (OoT's compute programs)");
    lowers(depbar(true, 0, 0, 0x0e), "DEPBAR.LE SB0, 0x0, {1,2,3} (OoT's vertex program)");
    lowers(depbar(true, 0, 0, 0), "DEPBAR.LE SB0, 0x0");
    lowers(depbar(true, 2, 63, 0), "DEPBAR.LE SB2, 0x3f");
    lowers(depbar(false, 0, 0, 0x21), "DEPBAR {0,5} (no .LE: the set alone)");
    lowers(depbar(true, 0, 0, 0x3f), "DEPBAR.LE, every scoreboard");

    Instruction bad = depbar(true, 0, 0, 0);
    bad.words[0] |= 0x00010000;
    rejected(bad, "bits 16..31 set: not rejected");
    bad = depbar(true, 0, 0, 0);
    bad.words[1] |= 0x00010000;
    rejected(bad, "bits 48..63 set: not rejected");
    bad = depbar(true, 0, 0, 0);
    bad.words[2] = 1;
    rejected(bad, "bits 64..95 set: not rejected");

    if (failed) return 1;
    std::printf("lower_depbar: every dependency barrier form lowers to nothing\n");
    return 0;
}
