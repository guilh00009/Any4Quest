// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
// Heap ownership keeps pipeline and specialization pointers stable when the
// module vector grows or cache entries arrive in a different order.
struct Program {
    struct Module {
        vk::ShaderModule module{};
        Shader::StageSpecialization spec;
        std::unique_ptr<Shader::Info> info;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;
    Shader::Info info; // Canonical resource layout used only for specialization lookup.
    ModuleList modules{};

    Program() = default;
    Program(Shader::Stage stage, Shader::LogicalStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                   std::unique_ptr<Shader::Info> compiled_info) {
        InsertPermut(module, std::move(spec), modules.size(), std::move(compiled_info));
    }
    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec, size_t perm_idx,
                      std::unique_ptr<Shader::Info> compiled_info) {
        modules.resize(std::max(modules.size(), perm_idx + 1));
        ASSERT(!modules[perm_idx].info); // Never invalidate an existing pipeline's pointer.
        spec.info = compiled_info.get();
        modules[perm_idx] = {module, std::move(spec), std::move(compiled_info)};
    }
};
} // namespace Vulkan
