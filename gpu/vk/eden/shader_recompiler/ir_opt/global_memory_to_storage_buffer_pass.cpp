// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <optional>
#include <cstdlib>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <unordered_set>

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>

#include "common/alignment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/breadth_first_search.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {

/// (EvilOOT) RX_VK_PTR_DIAG=1: how this thread's last translations' global memory accesses were bound, read and reset by
/// gpu/vk/vk_shader.cpp around each program (no header: declared there by hand, so Eden's ~500 files do not rebuild).
///   tracked   bound to a constant buffer pointer, the address made from constant buffer words only
///   fallback  not tracked: the run-time search (NVN slots, tables, the mesh pointer table), table = a table was found
///   mixed     bound to a constant buffer pointer although part of the address comes from memory (often only an index)
///   split     bound to a constant buffer pointer while a select or phi gives the address either from constant buffer
///             words or from memory (a pointer loaded through it): EvilOOT's near-leaf bug (zortos293/Lichii #410), the
///             loaded pointer's reads go to the wrong buffer
///   fallback_split  the same pattern among the fallbacks
struct PointerDiag {
    u32 tracked, fallback, fallback_table, mixed, split, fallback_split;
};
thread_local PointerDiag g_pointer_diag{};
// (collect_shader_info_pass.cpp: NVN2 compute's storage buffer slots for the program translated on this thread, 0: not compute)
extern thread_local u32 g_nvn2_compute_ssbo_base;
extern thread_local u32 g_nvn2_compute_ssbo_count;
extern thread_local bool g_all_global_this;   // (RX_VK_ALL_GLOBAL_PROGS, vk_shader.cpp: no access of this program bound)

namespace {
bool PointerDiagOn() {
    static const bool on = std::getenv("RX_VK_PTR_DIAG") != nullptr && std::atoi(std::getenv("RX_VK_PTR_DIAG")) != 0;
    return on;
}
/// (EvilOOT) a split address (see PointerDiag) is not bound to the constant buffer pointer on one side of its select or phi;
/// it is left to the run-time search, which finds whichever bound buffer (or table entry, or pointer table pool) the
/// address really falls in. #410's bug class (EvilOOT's near leaves): bound to one side, the other side's reads went to
/// the wrong buffer. On by default since the search covers whole pools (OoT's culling: characters, fallen leaves and
/// fireflies came back). RX_VK_PTR_SPLIT_FALLBACK=0: bound as before.
bool SplitFallbackOn() {
    static const bool on = !std::getenv("RX_VK_PTR_SPLIT_FALLBACK") || std::atoi(std::getenv("RX_VK_PTR_SPLIT_FALLBACK")) != 0;
    return on;
}
/// Address in constant buffers to the storage buffer descriptor
struct StorageBufferAddr {
    auto operator<=>(const StorageBufferAddr&) const noexcept = default;

    u32 index;
    u32 offset;
};

/// Block iterator to a global memory instruction and the storage buffer it uses
struct StorageInst {
    StorageBufferAddr storage_buffer;
    IR::Inst* inst;
    IR::Block* block;
};

/// Bias towards a certain range of constant buffers when looking for storage buffers
struct Bias {
    u32 index;
    u32 offset_begin;
    u32 offset_end;
    u32 alignment;
};

using boost::container::flat_set;
using boost::container::small_vector;
using StorageBufferSet =
    flat_set<StorageBufferAddr, std::less<StorageBufferAddr>, small_vector<StorageBufferAddr, 16>>;
using StorageInstVector = small_vector<StorageInst, 24>;
using StorageWritesSet =
    flat_set<StorageBufferAddr, std::less<StorageBufferAddr>, small_vector<StorageBufferAddr, 16>>;

struct StorageInfo {
    StorageBufferSet set;
    StorageInstVector to_replace;
    StorageWritesSet writes;
    /// Track results by the instruction the search starts from, with and without the NVN bias:
    /// accesses through the same address share them, and a search can walk thousands of
    /// instructions (the IR does not change while storage buffers are collected).
    std::unordered_map<const IR::Inst*, std::optional<StorageBufferAddr>> biased_tracks;
    std::unordered_map<const IR::Inst*, std::optional<StorageBufferAddr>> unbiased_tracks;
    StorageBufferSet tables;   // (hookshot: TrackTable's, for the fallbacks)
};

/// Returns true when the instruction is a global memory instruction
bool IsGlobalMemory(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadGlobalS8:
    case IR::Opcode::LoadGlobalU8:
    case IR::Opcode::LoadGlobalS16:
    case IR::Opcode::LoadGlobalU16:
    case IR::Opcode::LoadGlobal32:
    case IR::Opcode::LoadGlobal64:
    case IR::Opcode::LoadGlobal128:
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
    case IR::Opcode::GlobalAtomicIAdd32:
    case IR::Opcode::GlobalAtomicSMin32:
    case IR::Opcode::GlobalAtomicUMin32:
    case IR::Opcode::GlobalAtomicSMax32:
    case IR::Opcode::GlobalAtomicUMax32:
    case IR::Opcode::GlobalAtomicInc32:
    case IR::Opcode::GlobalAtomicDec32:
    case IR::Opcode::GlobalAtomicAnd32:
    case IR::Opcode::GlobalAtomicOr32:
    case IR::Opcode::GlobalAtomicXor32:
    case IR::Opcode::GlobalAtomicExchange32:
    case IR::Opcode::GlobalAtomicIAdd64:
    case IR::Opcode::GlobalAtomicSMin64:
    case IR::Opcode::GlobalAtomicUMin64:
    case IR::Opcode::GlobalAtomicSMax64:
    case IR::Opcode::GlobalAtomicUMax64:
    case IR::Opcode::GlobalAtomicAnd64:
    case IR::Opcode::GlobalAtomicOr64:
    case IR::Opcode::GlobalAtomicXor64:
    case IR::Opcode::GlobalAtomicExchange64:
    case IR::Opcode::GlobalAtomicIAdd32x2:
    case IR::Opcode::GlobalAtomicSMin32x2:
    case IR::Opcode::GlobalAtomicUMin32x2:
    case IR::Opcode::GlobalAtomicSMax32x2:
    case IR::Opcode::GlobalAtomicUMax32x2:
    case IR::Opcode::GlobalAtomicAnd32x2:
    case IR::Opcode::GlobalAtomicOr32x2:
    case IR::Opcode::GlobalAtomicXor32x2:
    case IR::Opcode::GlobalAtomicExchange32x2:
    case IR::Opcode::GlobalAtomicAddF32:
    case IR::Opcode::GlobalAtomicAddF16x2:
    case IR::Opcode::GlobalAtomicAddF32x2:
    case IR::Opcode::GlobalAtomicMinF16x2:
    case IR::Opcode::GlobalAtomicMinF32x2:
    case IR::Opcode::GlobalAtomicMaxF16x2:
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return true;
    default:
        return false;
    }
}

/// Returns true when the instruction is a global memory instruction
bool IsGlobalMemoryWrite(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
    case IR::Opcode::GlobalAtomicIAdd32:
    case IR::Opcode::GlobalAtomicSMin32:
    case IR::Opcode::GlobalAtomicUMin32:
    case IR::Opcode::GlobalAtomicSMax32:
    case IR::Opcode::GlobalAtomicUMax32:
    case IR::Opcode::GlobalAtomicInc32:
    case IR::Opcode::GlobalAtomicDec32:
    case IR::Opcode::GlobalAtomicAnd32:
    case IR::Opcode::GlobalAtomicOr32:
    case IR::Opcode::GlobalAtomicXor32:
    case IR::Opcode::GlobalAtomicExchange32:
    case IR::Opcode::GlobalAtomicIAdd64:
    case IR::Opcode::GlobalAtomicSMin64:
    case IR::Opcode::GlobalAtomicUMin64:
    case IR::Opcode::GlobalAtomicSMax64:
    case IR::Opcode::GlobalAtomicUMax64:
    case IR::Opcode::GlobalAtomicAnd64:
    case IR::Opcode::GlobalAtomicOr64:
    case IR::Opcode::GlobalAtomicXor64:
    case IR::Opcode::GlobalAtomicExchange64:
    case IR::Opcode::GlobalAtomicIAdd32x2:
    case IR::Opcode::GlobalAtomicSMin32x2:
    case IR::Opcode::GlobalAtomicUMin32x2:
    case IR::Opcode::GlobalAtomicSMax32x2:
    case IR::Opcode::GlobalAtomicUMax32x2:
    case IR::Opcode::GlobalAtomicAnd32x2:
    case IR::Opcode::GlobalAtomicOr32x2:
    case IR::Opcode::GlobalAtomicXor32x2:
    case IR::Opcode::GlobalAtomicExchange32x2:
    case IR::Opcode::GlobalAtomicAddF32:
    case IR::Opcode::GlobalAtomicAddF16x2:
    case IR::Opcode::GlobalAtomicAddF32x2:
    case IR::Opcode::GlobalAtomicMinF16x2:
    case IR::Opcode::GlobalAtomicMinF32x2:
    case IR::Opcode::GlobalAtomicMaxF16x2:
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return true;
    default:
        return false;
    }
}

/// Converts a global memory opcode to its storage buffer equivalent
IR::Opcode GlobalToStorage(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::LoadGlobalS8:
        return IR::Opcode::LoadStorageS8;
    case IR::Opcode::LoadGlobalU8:
        return IR::Opcode::LoadStorageU8;
    case IR::Opcode::LoadGlobalS16:
        return IR::Opcode::LoadStorageS16;
    case IR::Opcode::LoadGlobalU16:
        return IR::Opcode::LoadStorageU16;
    case IR::Opcode::LoadGlobal32:
        return IR::Opcode::LoadStorage32;
    case IR::Opcode::LoadGlobal64:
        return IR::Opcode::LoadStorage64;
    case IR::Opcode::LoadGlobal128:
        return IR::Opcode::LoadStorage128;
    case IR::Opcode::WriteGlobalS8:
        return IR::Opcode::WriteStorageS8;
    case IR::Opcode::WriteGlobalU8:
        return IR::Opcode::WriteStorageU8;
    case IR::Opcode::WriteGlobalS16:
        return IR::Opcode::WriteStorageS16;
    case IR::Opcode::WriteGlobalU16:
        return IR::Opcode::WriteStorageU16;
    case IR::Opcode::WriteGlobal32:
        return IR::Opcode::WriteStorage32;
    case IR::Opcode::WriteGlobal64:
        return IR::Opcode::WriteStorage64;
    case IR::Opcode::WriteGlobal128:
        return IR::Opcode::WriteStorage128;
    case IR::Opcode::GlobalAtomicIAdd32:
        return IR::Opcode::StorageAtomicIAdd32;
    case IR::Opcode::GlobalAtomicSMin32:
        return IR::Opcode::StorageAtomicSMin32;
    case IR::Opcode::GlobalAtomicUMin32:
        return IR::Opcode::StorageAtomicUMin32;
    case IR::Opcode::GlobalAtomicSMax32:
        return IR::Opcode::StorageAtomicSMax32;
    case IR::Opcode::GlobalAtomicUMax32:
        return IR::Opcode::StorageAtomicUMax32;
    case IR::Opcode::GlobalAtomicInc32:
        return IR::Opcode::StorageAtomicInc32;
    case IR::Opcode::GlobalAtomicDec32:
        return IR::Opcode::StorageAtomicDec32;
    case IR::Opcode::GlobalAtomicAnd32:
        return IR::Opcode::StorageAtomicAnd32;
    case IR::Opcode::GlobalAtomicOr32:
        return IR::Opcode::StorageAtomicOr32;
    case IR::Opcode::GlobalAtomicXor32:
        return IR::Opcode::StorageAtomicXor32;
    case IR::Opcode::GlobalAtomicExchange32:
        return IR::Opcode::StorageAtomicExchange32;
    case IR::Opcode::GlobalAtomicIAdd64:
        return IR::Opcode::StorageAtomicIAdd64;
    case IR::Opcode::GlobalAtomicSMin64:
        return IR::Opcode::StorageAtomicSMin64;
    case IR::Opcode::GlobalAtomicUMin64:
        return IR::Opcode::StorageAtomicUMin64;
    case IR::Opcode::GlobalAtomicSMax64:
        return IR::Opcode::StorageAtomicSMax64;
    case IR::Opcode::GlobalAtomicUMax64:
        return IR::Opcode::StorageAtomicUMax64;
    case IR::Opcode::GlobalAtomicAnd64:
        return IR::Opcode::StorageAtomicAnd64;
    case IR::Opcode::GlobalAtomicOr64:
        return IR::Opcode::StorageAtomicOr64;
    case IR::Opcode::GlobalAtomicXor64:
        return IR::Opcode::StorageAtomicXor64;
    case IR::Opcode::GlobalAtomicExchange64:
        return IR::Opcode::StorageAtomicExchange64;
    case IR::Opcode::GlobalAtomicIAdd32x2:
        return IR::Opcode::StorageAtomicIAdd32x2;
    case IR::Opcode::GlobalAtomicSMin32x2:
        return IR::Opcode::StorageAtomicSMin32x2;
    case IR::Opcode::GlobalAtomicUMin32x2:
        return IR::Opcode::StorageAtomicUMin32x2;
    case IR::Opcode::GlobalAtomicSMax32x2:
        return IR::Opcode::StorageAtomicSMax32x2;
    case IR::Opcode::GlobalAtomicUMax32x2:
        return IR::Opcode::StorageAtomicUMax32x2;
    case IR::Opcode::GlobalAtomicAnd32x2:
        return IR::Opcode::StorageAtomicAnd32x2;
    case IR::Opcode::GlobalAtomicOr32x2:
        return IR::Opcode::StorageAtomicOr32x2;
    case IR::Opcode::GlobalAtomicXor32x2:
        return IR::Opcode::StorageAtomicXor32x2;
    case IR::Opcode::GlobalAtomicExchange32x2:
        return IR::Opcode::StorageAtomicExchange32x2;
    case IR::Opcode::GlobalAtomicAddF32:
        return IR::Opcode::StorageAtomicAddF32;
    case IR::Opcode::GlobalAtomicAddF16x2:
        return IR::Opcode::StorageAtomicAddF16x2;
    case IR::Opcode::GlobalAtomicMinF16x2:
        return IR::Opcode::StorageAtomicMinF16x2;
    case IR::Opcode::GlobalAtomicMaxF16x2:
        return IR::Opcode::StorageAtomicMaxF16x2;
    case IR::Opcode::GlobalAtomicAddF32x2:
        return IR::Opcode::StorageAtomicAddF32x2;
    case IR::Opcode::GlobalAtomicMinF32x2:
        return IR::Opcode::StorageAtomicMinF32x2;
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return IR::Opcode::StorageAtomicMaxF32x2;
    default:
        throw InvalidArgument("Invalid global memory opcode {}", opcode);
    }
}

/// Returns true when a storage buffer address satisfies a bias
bool MeetsBias(const StorageBufferAddr& storage_buffer, const Bias& bias) noexcept {
    return storage_buffer.index == bias.index && storage_buffer.offset >= bias.offset_begin &&
           storage_buffer.offset < bias.offset_end;
}

struct LowAddrInfo {
    IR::U32 value;
    s32 imm_offset;
};

/// Tries to track the first 32-bits of a global memory instruction
std::optional<LowAddrInfo> TrackLowAddress(IR::Inst* inst) {
    // The first argument is the low level GPU pointer to the global memory instruction
    const IR::Value addr{inst->Arg(0)};
    if (addr.IsImmediate()) {
        // Not much we can do if it's an immediate
        return std::nullopt;
    }
    // This address is expected to either be a PackUint2x32, a IAdd64, or a CompositeConstructU32x2
    IR::Inst* addr_inst{addr.InstRecursive()};
    s32 imm_offset{0};
    if (addr_inst->GetOpcode() == IR::Opcode::IAdd64) {
        // If it's an IAdd64, get the immediate offset it is applying and grab the address
        // instruction. This expects for the instruction to be canonicalized having the address on
        // the first argument and the immediate offset on the second one.
        const IR::U64 imm_offset_value{addr_inst->Arg(1)};
        if (!imm_offset_value.IsImmediate()) {
            return std::nullopt;
        }
        imm_offset = static_cast<s32>(static_cast<s64>(imm_offset_value.U64()));
        const IR::U64 iadd_addr{addr_inst->Arg(0)};
        if (iadd_addr.IsImmediate()) {
            return std::nullopt;
        }
        addr_inst = iadd_addr.InstRecursive();
    }
    // With IAdd64 handled, now PackUint2x32 is expected
    if (addr_inst->GetOpcode() == IR::Opcode::PackUint2x32) {
        // PackUint2x32 is expected to be generated from a vector
        const IR::Value vector{addr_inst->Arg(0)};
        if (vector.IsImmediate()) {
            return std::nullopt;
        }
        addr_inst = vector.InstRecursive();
    }
    // The vector is expected to be a CompositeConstructU32x2
    if (addr_inst->GetOpcode() != IR::Opcode::CompositeConstructU32x2) {
        return std::nullopt;
    }
    // Grab the first argument from the CompositeConstructU32x2, this is the low address.
    return LowAddrInfo{
        .value{IR::U32{addr_inst->Arg(0)}},
        .imm_offset = imm_offset,
    };
}

/// Tries to track the storage buffer address used by a global memory instruction
std::optional<StorageBufferAddr> Track(const IR::Value& value, const Bias* bias) {
    const auto pred{[bias](const IR::Inst* inst) -> std::optional<StorageBufferAddr> {
        if (inst->GetOpcode() != IR::Opcode::GetCbufU32 &&
            inst->GetOpcode() != IR::Opcode::GetCbufU32x2) {
            return std::nullopt;
        }
        const IR::Value index{inst->Arg(0)};
        const IR::Value offset{inst->Arg(1)};
        if (!index.IsImmediate()) {
            // Definitely not a storage buffer if it's read from a
            // non-immediate index
            return std::nullopt;
        }
        if (!offset.IsImmediate()) {
            // TODO: Support SSBO arrays
            return std::nullopt;
        }
        const StorageBufferAddr storage_buffer{
            .index = index.U32(),
            .offset = offset.U32(),
        };
        const u32 alignment{bias ? bias->alignment : 8U};
        if (!Common::IsAligned(storage_buffer.offset, alignment)) {
            // The SSBO pointer has to be aligned
            return std::nullopt;
        }
        if (bias && !MeetsBias(storage_buffer, *bias)) {
            // We have to blacklist some addresses in case we wrongly
            // point to them
            return std::nullopt;
        }
        return storage_buffer;
    }};
    // (hookshot, from Lichii) A pointer loaded from memory is a new address, not an offset into the buffer that supplied it: its
    // load is not followed back to that buffer's own constant-buffer descriptor.
    const auto stop_at_memory = [](IR::Value operand) {
        if (!operand.IsImmediate() && IsGlobalMemory(*operand.InstRecursive())) {
            return IR::Value{u32{0}};
        }
        return operand;
    };
    return BreadthFirstSearch(value, pred, stop_at_memory);
}

/// (hookshot) A low address read from a storage buffer table: GetCbuf(index, x * 16 + base), x a run-time value
std::optional<StorageBufferAddr> TrackTable(const IR::Value& value) {
    const auto pred{[](const IR::Inst* inst) -> std::optional<StorageBufferAddr> {
        if (inst->GetOpcode() != IR::Opcode::GetCbufU32 && inst->GetOpcode() != IR::Opcode::GetCbufU32x2) {
            return std::nullopt;
        }
        const IR::Value index{inst->Arg(0)};
        const IR::Value offset{inst->Arg(1)};
        if (!index.IsImmediate() || offset.IsImmediate()) {
            return std::nullopt;
        }
        const IR::Inst* const add{offset.InstRecursive()};
        if (add->GetOpcode() != IR::Opcode::IAdd32) {
            return std::nullopt;
        }
        for (size_t side = 0; side < 2; ++side) {
            const IR::Value base{add->Arg(side)};
            const IR::Value scaled{add->Arg(1 - side)};
            if (!base.IsImmediate() || scaled.IsImmediate() || base.U32() % 16 != 0) {
                continue;
            }
            const IR::Inst* const s{scaled.InstRecursive()};
            const bool by16{s->Arg(1).IsImmediate() &&
                            ((s->GetOpcode() == IR::Opcode::ShiftLeftLogical32 && s->Arg(1).U32() == 4) ||
                             (s->GetOpcode() == IR::Opcode::IMul32 && s->Arg(1).U32() == 16))};
            if (by16) {
                return StorageBufferAddr{.index = index.U32(), .offset = base.U32()};
            }
        }
        return std::nullopt;
    }};
    return BreadthFirstSearch(value, pred);
}

/// (EvilOOT, RX_VK_PTR_DIAG) Where an address comes from: constant buffer words, memory (a global load: a pointer loaded
/// from memory), and whether a select or phi joins values on the way. The walk stops at both kinds of source.
/// split: a select or phi with a side from constant buffer words only and a side from memory (#410's pattern: the
/// pointer is c0[x] or *c0[x], and only c0[x] gets bound). An index loaded from memory into a constant buffer
/// pointer's offset is not split.
struct AddrOrigins {
    bool cbuf{}, load{}, branch{}, split{};
};
/// (11-07) One walk for the whole program, each instruction's origins kept: the first version walked up to 1024
/// instructions per address and walked each side of every select or phi again, nested three deep, which on a compute
/// shader with many loop phis ran for minutes on the submit's thread (the freeze after Load, vk_hangdump.cpp found it).
/// Origins of an instruction = the union of its arguments' (a select's condition left out); split where a select or
/// phi joins a side from constant buffer words only with a side from memory, or below. A phi's argument still being
/// walked (a loop) adds nothing.
thread_local std::unordered_map<const IR::Inst*, AddrOrigins> g_origins;
AddrOrigins Origins(const IR::Value& root) {
    if (root.IsImmediate()) {
        return {};
    }
    const IR::Inst* const top{root.InstRecursive()};
    if (const auto it{g_origins.find(top)}; it != g_origins.end()) {
        return it->second;
    }
    const auto is_cbuf{[](IR::Opcode op) { return op == IR::Opcode::GetCbufU32 || op == IR::Opcode::GetCbufU32x2; }};
    const auto first_arg{[](IR::Opcode op) -> size_t {
        return op == IR::Opcode::SelectU32 || op == IR::Opcode::SelectU64 ? 1 : 0;   // (not the condition)
    }};
    struct Frame {
        const IR::Inst* inst;
        size_t next;
    };
    std::vector<Frame> stack{{top, first_arg(top->GetOpcode())}};
    std::unordered_set<const IR::Inst*> walking{top};
    while (!stack.empty()) {
        const IR::Inst* const inst{stack.back().inst};
        const IR::Opcode op{inst->GetOpcode()};
        const bool source{is_cbuf(op) || IsGlobalMemory(*inst)};
        const IR::Inst* child{};
        while (!source && !child && stack.back().next < inst->NumArgs()) {
            const IR::Value arg{inst->Arg(stack.back().next++)};
            if (arg.IsImmediate()) {
                continue;
            }
            const IR::Inst* const a{arg.InstRecursive()};
            if (!g_origins.contains(a) && !walking.contains(a)) {
                child = a;
            }
        }
        if (child) {
            stack.push_back({child, first_arg(child->GetOpcode())});
            walking.insert(child);
            continue;
        }
        AddrOrigins o;
        if (is_cbuf(op)) {
            o.cbuf = true;
        } else if (IsGlobalMemory(*inst)) {
            o.load = true;
        } else {
            const size_t first{first_arg(op)};
            const bool joins{op == IR::Opcode::Phi || first == 1};
            bool cbuf_only{}, from_memory{};
            o.branch = joins;
            for (size_t i = first; i < inst->NumArgs(); ++i) {
                const IR::Value arg{inst->Arg(i)};
                if (arg.IsImmediate()) {
                    continue;
                }
                const auto it{g_origins.find(arg.InstRecursive())};
                if (it == g_origins.end()) {
                    continue;   // (a loop's phi, still being walked)
                }
                const AddrOrigins& side{it->second};
                o.cbuf |= side.cbuf, o.load |= side.load, o.branch |= side.branch, o.split |= side.split;
                cbuf_only |= side.cbuf && !side.load;
                from_memory |= side.load;
            }
            o.split |= joins && cbuf_only && from_memory;
        }
        g_origins.emplace(inst, o);
        walking.erase(inst);
        stack.pop_back();
    }
    return g_origins[top];
}
/// (hookshot, from Lichii b8d81e20) Whether an address is a select or phi of which one input is a pointer loaded from memory: Ocarina's foliage
/// placement records are read from c0[12] or from *c0[12]. Tracked to the constant buffer pointer alone, the loaded path read out of bounds
/// as zeros, the near-leaf pass wrote NaN positions, and trees had no leaves. Such a load stays global: the backend's page table reads it
bool MixesLoadedPointer(const IR::Value& value) {
    if (value.IsImmediate()) {
        return false;
    }
    std::vector<const IR::Inst*> todo{value.InstRecursive()};
    std::unordered_set<const IR::Inst*> seen;
    bool merge{}, loaded{};
    while (!todo.empty() && seen.size() < 96) {
        const IR::Inst* const inst{todo.back()};
        todo.pop_back();
        if (!seen.insert(inst).second) {
            continue;
        }
        switch (inst->GetOpcode()) {
        case IR::Opcode::Phi:
        case IR::Opcode::SelectU32:
        case IR::Opcode::SelectU64:
            merge = true;
            break;
        case IR::Opcode::LoadStorage32:
        case IR::Opcode::LoadStorage64:
        case IR::Opcode::LoadStorage128:
        case IR::Opcode::LoadGlobal32:
        case IR::Opcode::LoadGlobal64:
        case IR::Opcode::LoadGlobal128:
            loaded = true;
            continue;   // (what it loads from is another matter)
        default:
            break;
        }
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            const IR::Value arg{inst->Arg(i)};
            if (!arg.IsImmediate()) {
                todo.push_back(arg.InstRecursive());
            }
        }
    }
    return merge && loaded;
}

thread_local bool g_follow_stage = false;   // (hookshot: the program being translated is compute, where the page table is)

/// Collects the storage buffer used by a global memory instruction and the instruction itself
void CollectStorageBuffers(IR::Block& block, IR::Inst& inst, StorageInfo& info) {
    if (g_all_global_this) {
        return;   // (EvilOOT, a test: every access left global, found by its address at run time: search, then the page table)
    }
    // NVN puts storage buffers in a specific range, we have to bias towards these addresses to
    // avoid getting false positives
    static constexpr Bias nvn_bias{
        .index = 0,
        .offset_begin = 0x100,
        .offset_end = 0x700,
        .alignment = 16,
    };
    // (EvilOOT, RX_VK_PTR_DIAG) counted on every way out of here
    const bool diag{PointerDiagOn()};
    const bool split_fallback{SplitFallbackOn()};
    const AddrOrigins origins{diag || split_fallback ? Origins(inst.Arg(0)) : AddrOrigins{}};
    const auto count_fallback{[&](bool table) {
        if (diag) {
            ++g_pointer_diag.fallback;
            g_pointer_diag.fallback_table += table ? 1 : 0;
            g_pointer_diag.fallback_split += origins.split ? 1 : 0;
        }
    }};
    // Track the low address of the instruction
    const std::optional<LowAddrInfo> low_addr_info{TrackLowAddress(&inst)};
    if (!low_addr_info) {
        // Failed to track the low address, use NVN fallbacks
        count_fallback(false);
        return;
    }
    // (EvilOOT) NVN2 compute: its own storage buffer slots are the bias (collect_shader_info_pass.cpp
    // g_nvn2_compute_ssbo_base), not Maxwell's 0x100..0x700, which never matched: every compute access went to the
    // unbiased track, which took the first constant buffer word it met
    const Bias compute_bias{
        .index = 0,
        .offset_begin = g_nvn2_compute_ssbo_base,
        .offset_end = g_nvn2_compute_ssbo_base + 16 * g_nvn2_compute_ssbo_count,
        .alignment = 16,
    };
    const Bias* const bias{g_nvn2_compute_ssbo_base ? &compute_bias : &nvn_bias};
    {
        // (only where the page table is: compute. A graphics stage reads such a load as 0, and that blanked every menu text)
        static const bool follow{std::getenv("RX_VK_NO_FOLLOW") == nullptr};
        const IR::Opcode op{inst.GetOpcode()};
        // (and the writes and 32-bit atomics through such an address: the culling's outputs and counters, which the backend's page
        // table now reaches too. Tracked to the constant buffer alone they went to a buffer at address 0, and every count stayed 0)
        const bool load{op == IR::Opcode::LoadGlobal32 || op == IR::Opcode::LoadGlobal64 || op == IR::Opcode::LoadGlobal128};
        const bool store{op == IR::Opcode::WriteGlobal32 || op == IR::Opcode::WriteGlobal64 || op == IR::Opcode::WriteGlobal128 ||
                         op == IR::Opcode::GlobalAtomicIAdd32 || op == IR::Opcode::GlobalAtomicUMax32 || op == IR::Opcode::GlobalAtomicUMin32 ||
                         op == IR::Opcode::GlobalAtomicSMax32 || op == IR::Opcode::GlobalAtomicSMin32 || op == IR::Opcode::GlobalAtomicAnd32 ||
                         op == IR::Opcode::GlobalAtomicOr32 || op == IR::Opcode::GlobalAtomicXor32 || op == IR::Opcode::GlobalAtomicExchange32};
        // (RX_VK_FOLLOW_STORES=1: the writes too. Off: they made the culling count but drew no leaves, and a GPU faulted in it, #132)
        static const bool no_store{std::getenv("RX_VK_FOLLOW_STORES") == nullptr};
        if (follow && g_follow_stage && (load || (store && !no_store)) && MixesLoadedPointer(low_addr_info->value)) {
            return;   // (left global: MixesLoadedPointer)
        }
    }
    // First try to find storage buffers in the NVN address
    const IR::U32 low_addr{low_addr_info->value};
    const auto cached_track = [&](const Bias* bias, auto& cache) {
        if (low_addr.IsImmediate()) {
            return Track(low_addr, bias);
        }
        const IR::Inst* const start{low_addr.InstRecursive()};
        if (const auto it = cache.find(start); it != cache.end()) {
            return it->second;
        }
        const std::optional<StorageBufferAddr> result{Track(low_addr, bias)};
        cache.emplace(start, result);
        return result;
    };
    std::optional<StorageBufferAddr> storage_buffer{cached_track(bias, info.biased_tracks)};
    if (!storage_buffer) {
        // If it fails, track without a bias
        storage_buffer = cached_track(nullptr, info.unbiased_tracks);
        // (EvilOOT) c1 is the program's own constants: no buffer's address is known when a program is built, so a word of
        // c1 in the address is an offset (OoT's culling passes: a pointer loaded from memory + c1[0] = 12, taken for the
        // base: their results went to a buffer at address 12). Left to the run-time search, which finds the real one.
        if (storage_buffer && storage_buffer->index == 1) {
            storage_buffer.reset();
        }
        if (!storage_buffer) {
            // If that also fails, use NVN fallbacks (and the table it reads the address from, if it does)
            bool table_found{};
            if (!low_addr.IsImmediate()) {
                if (const std::optional<StorageBufferAddr> table{TrackTable(low_addr)}) {
                    info.tables.insert(*table);
                    table_found = true;
                }
            }
            count_fallback(table_found);
            LOG_WARNING(Shader, "Storage buffer failed to track, using global memory fallbacks");
            return;
        }
        LOG_DEBUG(Shader, "Storage buffer tracked without bias, index {} offset {}",
                    storage_buffer->index, storage_buffer->offset);
    }
    if (split_fallback && origins.split) {
        count_fallback(false);   // (counted as a run-time search, split)
        return;
    }
    if (diag) {
        ++g_pointer_diag.tracked;
        g_pointer_diag.mixed += origins.load ? 1 : 0;
        g_pointer_diag.split += origins.split ? 1 : 0;
    }
    // Collect storage buffer and the instruction
    if (IsGlobalMemoryWrite(inst)) {
        info.writes.insert(*storage_buffer);
    }
    info.set.insert(*storage_buffer);
    info.to_replace.push_back(StorageInst{
        .storage_buffer{*storage_buffer},
        .inst = &inst,
        .block = &block,
    });
}

/// Returns the offset in indices (not bytes) for an equivalent storage instruction
IR::U32 StorageOffset(IR::Block& block, IR::Inst& inst, StorageBufferAddr buffer, u32 alignment) {
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    IR::U32 offset;
    if (const std::optional<LowAddrInfo> low_addr{TrackLowAddress(&inst)}) {
        offset = low_addr->value;
        if (low_addr->imm_offset != 0) {
            offset = ir.IAdd(offset, ir.Imm32(low_addr->imm_offset));
        }
    } else {
        offset = ir.UConvert(32, IR::U64{inst.Arg(0)});
    }
    // Subtract the least significant 32 bits from the guest offset. The result is the storage
    // buffer offset in bytes.
    IR::U32 low_cbuf{ir.GetCbuf(ir.Imm32(buffer.index), ir.Imm32(buffer.offset))};

    // Align the offset base to match the host alignment requirements
    low_cbuf = ir.BitwiseAnd(low_cbuf, ir.Imm32(~(alignment - 1U)));
    return ir.ISub(offset, low_cbuf);
}

/// Replace a global memory load instruction with its storage buffer equivalent
void ReplaceLoad(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
                 const IR::U32& offset) {
    const IR::Opcode new_opcode{GlobalToStorage(inst.GetOpcode())};
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    const IR::Value value{&*block.PrependNewInst(it, new_opcode, {storage_index, offset})};
    inst.ReplaceUsesWith(value);
}

/// Replace a global memory write instruction with its storage buffer equivalent
void ReplaceWrite(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
                  const IR::U32& offset) {
    const IR::Opcode new_opcode{GlobalToStorage(inst.GetOpcode())};
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    block.PrependNewInst(it, new_opcode, {storage_index, offset, inst.Arg(1)});
    inst.Invalidate();
}

/// Replace an atomic operation on global memory instruction with its storage buffer equivalent
void ReplaceAtomic(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
                   const IR::U32& offset) {
    const IR::Opcode new_opcode{GlobalToStorage(inst.GetOpcode())};
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    const IR::Value value{
        &*block.PrependNewInst(it, new_opcode, {storage_index, offset, inst.Arg(1)})};
    inst.ReplaceUsesWith(value);
}

/// Replace a global memory instruction with its storage buffer equivalent
void Replace(IR::Block& block, IR::Inst& inst, const IR::U32& storage_index,
             const IR::U32& offset) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadGlobalS8:
    case IR::Opcode::LoadGlobalU8:
    case IR::Opcode::LoadGlobalS16:
    case IR::Opcode::LoadGlobalU16:
    case IR::Opcode::LoadGlobal32:
    case IR::Opcode::LoadGlobal64:
    case IR::Opcode::LoadGlobal128:
        return ReplaceLoad(block, inst, storage_index, offset);
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
        return ReplaceWrite(block, inst, storage_index, offset);
    case IR::Opcode::GlobalAtomicIAdd32:
    case IR::Opcode::GlobalAtomicSMin32:
    case IR::Opcode::GlobalAtomicUMin32:
    case IR::Opcode::GlobalAtomicSMax32:
    case IR::Opcode::GlobalAtomicUMax32:
    case IR::Opcode::GlobalAtomicInc32:
    case IR::Opcode::GlobalAtomicDec32:
    case IR::Opcode::GlobalAtomicAnd32:
    case IR::Opcode::GlobalAtomicOr32:
    case IR::Opcode::GlobalAtomicXor32:
    case IR::Opcode::GlobalAtomicExchange32:
    case IR::Opcode::GlobalAtomicIAdd64:
    case IR::Opcode::GlobalAtomicSMin64:
    case IR::Opcode::GlobalAtomicUMin64:
    case IR::Opcode::GlobalAtomicSMax64:
    case IR::Opcode::GlobalAtomicUMax64:
    case IR::Opcode::GlobalAtomicAnd64:
    case IR::Opcode::GlobalAtomicOr64:
    case IR::Opcode::GlobalAtomicXor64:
    case IR::Opcode::GlobalAtomicExchange64:
    case IR::Opcode::GlobalAtomicIAdd32x2:
    case IR::Opcode::GlobalAtomicSMin32x2:
    case IR::Opcode::GlobalAtomicUMin32x2:
    case IR::Opcode::GlobalAtomicSMax32x2:
    case IR::Opcode::GlobalAtomicUMax32x2:
    case IR::Opcode::GlobalAtomicAnd32x2:
    case IR::Opcode::GlobalAtomicOr32x2:
    case IR::Opcode::GlobalAtomicXor32x2:
    case IR::Opcode::GlobalAtomicExchange32x2:
    case IR::Opcode::GlobalAtomicAddF32:
    case IR::Opcode::GlobalAtomicAddF16x2:
    case IR::Opcode::GlobalAtomicAddF32x2:
    case IR::Opcode::GlobalAtomicMinF16x2:
    case IR::Opcode::GlobalAtomicMinF32x2:
    case IR::Opcode::GlobalAtomicMaxF16x2:
    case IR::Opcode::GlobalAtomicMaxF32x2:
        return ReplaceAtomic(block, inst, storage_index, offset);
    default:
        throw InvalidArgument("Invalid global memory opcode {}", inst.GetOpcode());
    }
}
} // Anonymous namespace

void GlobalMemoryToStorageBufferPass(IR::Program& program, const HostTranslateInfo& host_info) {
    static const bool no_mesh{std::getenv("RX_VK_FOLLOW_MESH") == nullptr};
    g_follow_stage = program.stage == Stage::Compute || (!no_mesh && (program.stage == Stage::Task || program.stage == Stage::Mesh));
    g_origins.clear();   // (EvilOOT: Origins' walk, kept per program)
    StorageInfo info;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsGlobalMemory(inst)) {
                continue;
            }
            CollectStorageBuffers(*block, inst, info);
        }
    }
    for (const StorageBufferAddr& storage_buffer : info.set) {
        program.info.storage_buffers_descriptors.push_back({
            .cbuf_index = storage_buffer.index,
            .cbuf_offset = storage_buffer.offset,
            .count = 1,
            .is_written = info.writes.contains(storage_buffer),
        });
    }
    for (const StorageBufferAddr& table : info.tables) {
        if (program.info.num_global_tables < program.info.global_tables.size()) {
            program.info.global_tables[program.info.num_global_tables++] = {table.index, table.offset};
        }
    }
    for (const StorageInst& storage_inst : info.to_replace) {
        const StorageBufferAddr storage_buffer{storage_inst.storage_buffer};
        const auto it{info.set.find(storage_inst.storage_buffer)};
        const IR::U32 index{IR::Value{static_cast<u32>(info.set.index_of(it))}};
        IR::Block* const block{storage_inst.block};
        IR::Inst* const inst{storage_inst.inst};
        const IR::U32 offset{
            StorageOffset(*block, *inst, storage_buffer, u32(host_info.min_ssbo_alignment))};
        Replace(*block, *inst, index, offset);
    }
}

void JoinStorageInfo(Info& base, Info& source) {
    auto& descriptors = base.storage_buffers_descriptors;
    for (auto& desc : source.storage_buffers_descriptors) {
        auto it{std::ranges::find_if(descriptors, [&desc](const auto& existing) {
            return desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset && desc.count == existing.count;
        })};
        if (it != descriptors.end()) {
            it->is_written |= desc.is_written;
            continue;
        }
        descriptors.push_back(desc);
    }
}

} // namespace Shader::Optimization
