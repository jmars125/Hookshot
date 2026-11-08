#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <spirv/unified1/spirv.hpp>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"

using namespace Shader;
using Shader::Backend::SPIRV::EmitSPIRV;
static std::filesystem::path output_dir{"work/global16-modules"};

struct TestEnvironment final : Environment {
    explicit TestEnvironment(Stage s = Stage::Compute) { stage = s; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return TextureType::Color2D; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return TexturePixelFormat::R8_UNORM; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

[[noreturn]] static void fail(const char* what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
}

static std::vector<u32> emit_program(bool is_signed, u64 address, bool support_int64,
                                     bool lower_address, Stage stage = Stage::Compute) {
    ObjectPool<IR::Inst> inst_pool;
    IR::Block block{inst_pool};
    IR::Program program;
    program.stage = stage;
    program.output_vertices = stage == Stage::Mesh ? 1 : 0;
    program.mesh.max_vertices = stage == Stage::Mesh ? 1 : 0;
    program.mesh.max_primitives = stage == Stage::Mesh ? 1 : 0;
    program.mesh.payload_words = stage == Stage::Mesh ? 1 : 0;
    program.workgroup_size = {1, 1, 1};
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    IR::AbstractSyntaxNode node{};
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = &block;
    program.syntax_list.push_back(node);
    IR::AbstractSyntaxNode ret{};
    ret.type = IR::AbstractSyntaxNode::Type::Return;
    program.syntax_list.push_back(ret);

    IR::IREmitter ir{block};
    IR::U64 guest_address;
    if (lower_address) {
        guest_address = ir.UConvert(64, ir.Imm32(static_cast<u32>(address)));
    } else {
        guest_address = ir.Imm64(address);
    }
    if (is_signed) {
        const IR::U32 value{ir.LoadGlobalS16(guest_address)};
        (void)ir.IAdd(value, ir.Imm32(1));
    } else {
        const IR::U32 value{ir.LoadGlobalU16(guest_address)};
        (void)ir.IAdd(value, ir.Imm32(1));
    }
    ir.Epilogue();

    TestEnvironment env{stage};
    if (lower_address) Optimization::LowerInt64ToInt32(program);
    Optimization::CollectShaderInfoPass(env, program);
    if (!program.info.uses_global_memory) fail("CollectShaderInfoPass did not see the actual global load");

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = support_int64;
    return EmitSPIRV(profile, program);
}

struct Instruction {
    spv::Op op;
    std::vector<u32> words;
};

struct ModuleInfo {
    std::vector<Instruction> instructions;
    std::map<u32, u32> integer_widths;
    std::map<u32, u64> constants;
};

static ModuleInfo inspect(const std::vector<u32>& code) {
    if (code.size() < 5 || code[0] != spv::MagicNumber) fail("bad SPIR-V header");
    ModuleInfo result;
    for (size_t at = 5; at < code.size();) {
        const u32 count = code[at] >> 16;
        if (count == 0 || at + count > code.size()) fail("bad SPIR-V instruction length");
        const auto op = static_cast<spv::Op>(code[at] & 0xffffu);
        std::vector<u32> words(code.begin() + at + 1, code.begin() + at + count);
        if (op == spv::Op::OpTypeInt && words.size() >= 3) {
            result.integer_widths[words[0]] = words[1];
        } else if (op == spv::Op::OpConstant && words.size() >= 3) {
            const u32 type = words[0], id = words[1];
            const auto width_it = result.integer_widths.find(type);
            const u32 width = width_it == result.integer_widths.end() ? 0 : width_it->second;
            u64 value = words[2];
            if (width == 64 && words.size() >= 4) value |= static_cast<u64>(words[3]) << 32;
            result.constants[id] = value;
        } else if (op == spv::Op::OpConstantNull && words.size() >= 2) {
            result.constants[words[1]] = 0;
        }
        result.instructions.push_back({op, std::move(words)});
        at += count;
    }
    return result;
}

static u64 constant(const ModuleInfo& module, u32 id, const char* name) {
    const auto it = module.constants.find(id);
    if (it == module.constants.end()) fail(name);
    return it->second;
}

static std::vector<u32> save_module(const std::string& name, std::vector<u32> code) {
    std::filesystem::create_directories(output_dir);
    std::ofstream file(output_dir / (name + ".spv"), std::ios::binary);
    if (!file) fail("could not create emitted SPIR-V module");
    file.write(reinterpret_cast<const char*>(code.data()),
               static_cast<std::streamsize>(code.size() * sizeof(u32)));
    if (!file) fail("could not write emitted SPIR-V module");
    return code;
}

static void check_case(bool is_signed, u64 address, const char* name, Stage stage = Stage::Compute) {
    std::string filename;
    for (const char* c = name; *c; ++c) filename += (*c == ' ' ? '-' : *c);
    const auto module = inspect(save_module(filename, emit_program(is_signed, address, true, false, stage)));
    const u64 aligned = address & ~u64{3};
    const u32 expected_bit_offset = static_cast<u32>((address & 3u) << 3);
    u32 address_id = 0, aligned_id = 0, low_lane_id = 0, low_id = 0, bit_offset_id = 0;
    bool saw_s_extract = false, saw_u_extract = false;
    u32 global_word_id = 0;

    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        const auto width_it = w.empty() ? module.integer_widths.end() : module.integer_widths.find(w[0]);
        if (inst.op == spv::Op::OpConstant && w.size() >= 3 && width_it != module.integer_widths.end() && width_it->second == 64 &&
            constant(module, w[1], "missing 64-bit guest address constant") == address) {
            address_id = w[1];
        }
    }
    if (!address_id) fail("actual IR address was not emitted as a 64-bit constant");

    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        const auto width_it = w.empty() ? module.integer_widths.end() : module.integer_widths.find(w[0]);
        if (inst.op == spv::Op::OpBitwiseAnd && w.size() >= 4 && width_it != module.integer_widths.end() && width_it->second == 64 &&
            w[2] == address_id) {
            const u64 mask = constant(module, w[3], "missing address mask");
            if (mask == ~u64{3}) {
                aligned_id = w[1];
                if ((constant(module, address_id, "missing source address") & ~u64{3}) != aligned)
                    fail("aligned helper base changed address high bits");
            } else if (mask == 3) {
                low_lane_id = w[1];
            }
        }
        const auto width32_it = w.empty() ? module.integer_widths.end() : module.integer_widths.find(w[0]);
        if (inst.op == spv::Op::OpUConvert && w.size() >= 3 && w[2] == low_lane_id &&
            width32_it != module.integer_widths.end() && width32_it->second == 32) low_id = w[1];
    }
    if (!aligned_id || !low_lane_id || !low_id)
        fail("missing full-U64 alignment or original-address lane extraction");

    u32 shifted_id = 0;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if (inst.op == spv::Op::OpShiftLeftLogical && w.size() >= 4 && w[2] == low_id &&
            constant(module, w[3], "missing byte-to-bit shift") == 3) shifted_id = w[1];
        const auto result_width = w.empty() ? module.integer_widths.end()
                                            : module.integer_widths.find(w[0]);
        if (inst.op == spv::Op::OpFunctionCall && w.size() >= 4 && w[3] == aligned_id &&
            result_width != module.integer_widths.end() && result_width->second == 32)
            global_word_id = w[1];
    }
    if (!shifted_id || !global_word_id)
        fail("global32 helper is not called with the aligned original U64 address");

    bit_offset_id = shifted_id;
    if (!bit_offset_id) fail("extract offset is not derived from the original address low bits");

    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if ((inst.op == spv::Op::OpBitFieldSExtract || inst.op == spv::Op::OpBitFieldUExtract) &&
            w.size() >= 5 && w[2] == global_word_id && w[3] == bit_offset_id &&
            constant(module, w[4], "missing 16-bit extract width") == 16) {
            if (inst.op == spv::Op::OpBitFieldSExtract) saw_s_extract = true;
            if (inst.op == spv::Op::OpBitFieldUExtract) saw_u_extract = true;
            if (expected_bit_offset != 0 && expected_bit_offset != 16) fail("test used non-halfword-aligned address");
        }
    }
    if (is_signed ? (!saw_s_extract || saw_u_extract) : (!saw_u_extract || saw_s_extract))
        fail("wrong signed/unsigned extraction operation for the requested IR opcode");

    std::printf("PASS: %s emitted aligned global32 helper + %s16 extraction (base=%016llx offset=%u)\n",
                name, is_signed ? "signed " : "unsigned ", static_cast<unsigned long long>(aligned),
                expected_bit_offset);
}

static std::vector<u32> emit_write_program(bool is_signed, u64 address, bool support_int64, u32 bits,
                                           Stage stage = Stage::Compute) {
    ObjectPool<IR::Inst> inst_pool;
    IR::Block block{inst_pool};
    IR::Program program;
    program.stage = stage;
    program.output_vertices = stage == Stage::Mesh ? 1 : 0;
    program.mesh.max_vertices = stage == Stage::Mesh ? 1 : 0;
    program.mesh.max_primitives = stage == Stage::Mesh ? 1 : 0;
    program.mesh.payload_words = stage == Stage::Mesh ? 1 : 0;
    program.workgroup_size = {1, 1, 1};
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    IR::AbstractSyntaxNode node{};
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = &block;
    program.syntax_list.push_back(node);
    IR::AbstractSyntaxNode ret{};
    ret.type = IR::AbstractSyntaxNode::Type::Return;
    program.syntax_list.push_back(ret);

    IR::IREmitter ir{block};
    const IR::U64 guest_address{ir.Imm64(address)};
    const IR::U32 value{ir.Imm32(bits == 8 ? 0xA5u : 0xA5A5u)};
    if (is_signed) {
        if (bits == 8) ir.WriteGlobalS8(guest_address, value);
        else ir.WriteGlobalS16(guest_address, value);
    } else {
        if (bits == 8) ir.WriteGlobalU8(guest_address, value);
        else ir.WriteGlobalU16(guest_address, value);
    }
    ir.Epilogue();

    TestEnvironment env{stage};
    Optimization::CollectShaderInfoPass(env, program);
    if (!program.info.stores_global_memory) fail("CollectShaderInfoPass did not see the actual global store");
    if (!program.info.uses_global_memory) fail("CollectShaderInfoPass did not see the global store's memory");

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = support_int64;
    return EmitSPIRV(profile, program);
}

static void check_write_case(bool is_signed, u64 address, u32 bits, const char* name,
                             Stage stage = Stage::Compute) {
    std::string filename;
    for (const char* c = name; *c; ++c) filename += (*c == ' ' ? '-' : *c);
    const auto module = inspect(save_module(filename, emit_write_program(is_signed, address, true, bits, stage)));
    const u32 expected_bit_offset = static_cast<u32>((address & 3u) << 3);
    bool saw_insert = false, saw_cas = false, saw_call = false, saw_shift = false;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if (inst.op == spv::Op::OpBitFieldInsert && w.size() >= 6) saw_insert = true;
        if (inst.op == spv::Op::OpAtomicCompareExchange) saw_cas = true;
        // (the compare-exchange helper's call: address, value, bit offset, width)
        if (inst.op == spv::Op::OpFunctionCall && w.size() >= 7 &&
            constant(module, w[6], "missing narrow-store width") == bits)
            saw_call = true;
        if (inst.op == spv::Op::OpShiftLeftLogical && w.size() >= 4 &&
            constant(module, w[3], "missing byte-to-bit shift") == 3)
            saw_shift = true;
    }
    if (!saw_insert) fail("narrow global store did not insert into its aligned word");
    if (!saw_cas) fail("narrow global store did not use a compare-exchange loop");
    if (!saw_call) fail("narrow global store did not call the compare-exchange helper with its width");
    if (!saw_shift) fail("narrow global store did not derive its bit offset from the address low bits");
    std::printf("PASS: %s emitted aligned-word compare-exchange + %u-bit insert (offset=%u)\n", name, bits,
                expected_bit_offset);
}

static void check_unsupported_profile() {
    const auto module = inspect(save_module(
        "unsupported-int64", emit_program(true, 0x0000000100000002ull, false, true)));
    u32 zero_id = 0;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if (inst.op == spv::Op::OpBitFieldSExtract || inst.op == spv::Op::OpBitFieldUExtract)
            fail("unsupported-int64 profile emitted a halfword extract instead of zero fallback");
        const auto type_width = w.empty() ? module.integer_widths.end()
                                          : module.integer_widths.find(w[0]);
        if (inst.op == spv::Op::OpConstant && w.size() >= 3 && w[2] == 0 &&
            type_width != module.integer_widths.end() && type_width->second == 32)
            zero_id = w[1];
    }
    bool zero_fallback_used = false;
    for (const auto& inst : module.instructions) {
        const auto& w = inst.words;
        if (inst.op == spv::Op::OpIAdd && w.size() >= 4 && w[2] == zero_id &&
            constant(module, w[3], "missing fallback's observable addend") == 1)
            zero_fallback_used = true;
    }
    if (!zero_id || !zero_fallback_used)
        fail("unsupported-int64 result was not the zero constant consumed by the fixture");
    std::puts("PASS: unsupported-int64 result is zero and emits no halfword extraction");
}

static void check_original_stub_failure() {
    try {
        (void)emit_program(true, 0x0000000100000000ull, true, false);
    } catch (const NotImplementedException& error) {
        if (std::string(error.what()).find("LoadGlobalS16") == std::string::npos)
            fail("original-stub negative control failed for the wrong IR opcode");
        std::puts("PASS: original S16 stub fails specifically at LoadGlobalS16");
        return;
    }
    fail("original-stub negative control unexpectedly emitted a module");
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--expect-original-stub") {
        check_original_stub_failure();
        return 0;
    }
    if (argc == 2) output_dir = argv[1];
    check_case(true, 0x0000000100000000ull, "signed offset 0 high address");
    check_case(false, 0x0000000100000002ull, "unsigned offset 2 high address");
    check_case(true, 0x0000007f00000ffeull, "signed guest-page end 0xffe");
    check_case(false, 0x0000007f00000ffeull, "unsigned guest-page end 0xffe");
    check_write_case(true, 0x0000000100000000ull, 16, "signed 16-bit store high address");
    check_write_case(false, 0x0000000100000002ull, 16, "unsigned 16-bit store offset 2");
    check_write_case(true, 0x0000000100000003ull, 8, "signed 8-bit store offset 3");
    check_write_case(false, 0x0000007f00000fffull, 8, "unsigned 8-bit store page end");
    check_case(true, 0x0000000100000000ull, "mesh signed offset 0 high address", Stage::Mesh);
    check_write_case(true, 0x0000000100000000ull, 16, "mesh signed 16-bit store high address", Stage::Mesh);
    check_unsupported_profile();
}
