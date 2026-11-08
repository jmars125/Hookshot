// NVN2 programs as SPIR-V modules (vk_shader.cpp) and what the pipelines need of them.
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/shader_info.h"

#include "vk_internal.h"
#include "vk_state.h"

namespace mkwvk {

struct Stage {
    int nvn_stage = 0;   // 0 vertex, 1 fragment, 5 compute (what its banks are read for)
    VkShaderStageFlagBits vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
    VkShaderModule module = VK_NULL_HANDLE;
    Shader::Info info;
    std::vector<uint8_t> c1;   // its c1 constants (the code area's tail)
};

/// A texture handle the stage reads from a storage buffer: its handle program (Shader::Info::handle_programs, Eden's
/// recompiler) run over the stage's constant banks and storage buffers as the draw has them
uint32_t stage_handle(const NvnState& s, const Stage& st, uint32_t program);

/// A translated program variant: its stages, in descriptor order. The pipeline layout and pipelines made for it live
/// in vk_pipeline.cpp (keyed by key).
struct Program {
    uint64_t key = 0;
    bool compute = false, failed = false;
    std::atomic<bool> ready{false};   // translated (graphics programs are translated on a worker: vk_jobs.h)
    std::vector<Stage> stages;
    std::array<uint32_t, 3> workgroup{1, 1, 1};
    uint32_t shared_memory = 0;
    // made by vk_pipeline.cpp at first use
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorType> binding_types;   // by binding (what each descriptor write is)
    VkPipeline compute_pipeline = VK_NULL_HANDLE;
    // (the compute pipeline's build: 0 not started, 1 on a worker, 2 done (compute_pipeline made, or failed): vk_draw.cpp dispatch)
    std::atomic<int> compute_state{0};
    bool push = false;             // its set is pushed (VK_KHR_push_descriptor)
    VkShaderStageFlags push_stages = 0;
    bool uses_render_area = false;
    bool uses_push = true;   // (a stage reads the push constants: render area or rescaling)
    bool mesh = false;              // task / mesh stages (VK_EXT_mesh_shader)
    // What the translator was told about the bound textures (when it asked: most texture instructions carry their
    // type themselves), by stage and texture descriptor: the variant stands while the textures still answer the same
    struct Answer {
        uint16_t stage, desc;
        uint8_t type;   // Shader::TextureType | 0x80 when the format is integer
        bool image = false;   // (hookshot) a storage image descriptor's: only whether its format is integer
    };
    std::vector<Answer> answers;
    uint64_t variant = 0;   // (a hash of the answers: key alone names all the variants of these stages)
};

enum class ProgramKind { Graphics, Compute, Mesh };

/// The program variant for the bound state (translated at first use; null when it does not translate).
/// color_types: the attachments' Shader::AttributeType (Float / SignedInt / UnsignedInt), for the fragment outputs.
/// Graphics programs may come back not ready yet (translation queued): the draw leaves itself out or program_wait()s.
const Program* program_for(const NvnState& s, ProgramKind kind, const uint8_t color_types[8]);
uint64_t compute_code_key(const NvnState& s);   // (hookshot) the bound compute program's code key, 0: none
std::string program_textures(const NvnState& s, const Program& p);   // (hookshot, for logs)
std::string program_code_keys(const NvnState& s);   // (hookshot) " stage:key" for each of the bound program's codes
const Program* program_pick(const NvnState& s, ProgramKind kind, const uint8_t color_types[8]);   // (program_for without its cache)
void program_wait(const Program* p);
uint64_t program_cache_stamp();   // (what the kept programs and pipelines are good for)
size_t program_cache_load_some(const std::unordered_set<uint64_t>& wanted, const std::function<void(Program&)>& fn);
/// The precompile of every shader of the game (hookshot_shaders.bin, from nx2/shader_census.py): how many programs it names, 0: none
size_t seed_open();
void seed_close();
/// Program i of the seed, translated and kept for the default state (null: kept already, or it does not translate); its modules are yours
std::unique_ptr<Program> seed_program(size_t i, int inputs = 0);
const Program* program_find(uint64_t key, uint64_t variant);
/// The NVN stages of the bound program, for logs (stage field of each control section)
void log_program_stages(const NvnState& s, const char* why);
Shader::AttributeType attribute_type_of(uint32_t vattr);
Shader::TextureType handle_texture_type(uint32_t handle);
void shader_stats(int64_t* translated, int64_t* failed);
double shader_translate_ms();
void program_released(const nvn_prog* p);

} // namespace mkwvk
