// SPDX-License-Identifier: GPL-3.0-or-later
// (hookshot) From Orchid (MythosOfs/Orchid, src/shader_recompiler/ir_opt/oot_visual_workaround.h, GPL-3.0), matched by
// the IR's shape here instead of Orchid's shader hashes: a visual workaround for Ocarina of Time, not a fix. The root
// cause of the washed-out outdoors is open there and here.
//  - fog: the one compute shader with an add of 2^x and -2^y, the fog integration: its slice distance scaled before
//    the extinction and scattering accumulation (Orchid: 0.25; 0.08 here; it thins interiors' fog too);
//  - exposure: a fragment shader with three loads of c3+0x38, the final pass's exposure before its filmic curve (0.65).
// RX_VK_OOT_VISUAL=0: off. RX_VK_OOT_FOG, RX_VK_OOT_EXPOSURE: the factors.
#pragma once

#include <utility>
#include <vector>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Optimization {

/// The edits made (0: not such a shader)
inline size_t ApplyOotVisualWorkaround(IR::Program& program, f32 fog_scale, f32 exposure_scale) {
    const bool fog = program.stage == Stage::Compute;
    const bool exposure = program.stage == Stage::Fragment;
    if (!fog && !exposure) return 0;
    const auto has_opcode = [](const IR::Value& value, IR::Opcode opcode) {
        return !value.IsImmediate() && value.InstRecursive()->GetOpcode() == opcode;
    };
    const auto negative_exp = [&](const IR::Value& value) {
        if (has_opcode(value, IR::Opcode::FPNeg32)) {
            return has_opcode(value.InstRecursive()->Arg(0), IR::Opcode::FPExp2);
        }
        if (!has_opcode(value, IR::Opcode::BitCastF32U32)) return false;
        const auto bits = value.InstRecursive()->Arg(0);
        if (!has_opcode(bits, IR::Opcode::BitwiseXor32)) return false;
        const auto* flip = bits.InstRecursive();
        const auto source = flip->Arg(0);
        return flip->Arg(1).IsImmediate() && flip->Arg(1).U32() == 0x80000000U &&
               has_opcode(source, IR::Opcode::BitCastU32F32) &&
               has_opcode(source.InstRecursive()->Arg(0), IR::Opcode::FPExp2);
    };
    std::vector<std::pair<IR::Block*, IR::Inst*>> matches;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (fog && inst.GetOpcode() == IR::Opcode::FPAdd32 &&
                ((negative_exp(inst.Arg(0)) && has_opcode(inst.Arg(1), IR::Opcode::FPExp2)) ||
                 (negative_exp(inst.Arg(1)) && has_opcode(inst.Arg(0), IR::Opcode::FPExp2)))) {
                matches.emplace_back(block, &inst);
            }
            if (exposure && inst.GetOpcode() == IR::Opcode::GetCbufF32 && inst.Arg(0).IsImmediate() &&
                inst.Arg(0).U32() == 3 && inst.Arg(1).IsImmediate() && inst.Arg(1).U32() == 0x38) {
                matches.emplace_back(block, &inst);
            }
        }
    }
    if (matches.size() != (fog ? 1u : 3u)) return 0;
    for (const auto& [block, inst] : matches) {
        const auto insertion = block->Instructions().iterator_to(*inst);
        const auto original = block->PrependNewInst(insertion, *inst);
        IR::IREmitter ir{*block, insertion};
        const auto scaled = ir.FPMul(IR::F32{IR::Value{&*original}}, ir.Imm32(fog ? fog_scale : exposure_scale));
        inst->ReplaceUsesWith(scaled);
    }
    return matches.size();
}

/// (hookshot) Not a NaN or an infinity into a float colour target. One such pixel in the scene went through the bloom's chain of
/// averages to the whole picture: the one-frame white flash, the HUD over it (RX_VK_FRAMELOG, RX_VK_PROBE=2 caught it: a NaN in the
/// scene target, the blur after it all at its top). What the console's blend does with one is unknown; nothing here wants it.
/// float_targets: bit i, target i is a float one (an integer target's bits go as they are)
inline size_t GuardFragmentOutputs(IR::Program& program, u32 float_targets) {
    if (program.stage != Stage::Fragment) return 0;
    std::vector<std::pair<IR::Block*, IR::Inst*>> writes;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetFragColor && inst.Arg(0).IsImmediate() &&
                (float_targets >> inst.Arg(0).U32() & 1) != 0 && !inst.Arg(2).IsImmediate()) {
                writes.emplace_back(block, &inst);
            }
        }
    }
    for (const auto& [block, inst] : writes) {
        IR::IREmitter ir{*block, block->Instructions().iterator_to(*inst)};
        const IR::F32 value{inst->Arg(2)};
        const IR::F32 limited{ir.FPClamp(value, ir.Imm32(-1e30f), ir.Imm32(1e30f))};
        const IR::F32 safe{ir.Select(ir.FPIsNan(value), ir.Imm32(0.0f), limited)};
        inst->SetArg(2, safe);
    }
    return writes.size();
}

} // namespace Shader::Optimization
