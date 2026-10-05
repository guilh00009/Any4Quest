// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdlib>
#include <iostream>
#define LOG_DEBUG(...) ((void)0)
#include "shader_recompiler/resource_proofs.h"
#include "video_core/renderer_vulkan/resource_liveness_routing.h"
#include "video_core/renderer_vulkan/vk_shader_program.h"
void assert_fail_impl() { std::abort(); }
[[noreturn]] void unreachable_impl() { std::abort(); }
static unsigned checks;
void Check(bool ok) {
  ++checks;
  if (!ok) {
    std::cerr << "FAIL " << checks << '\n';
    std::abort();
  }
}
std::unique_ptr<Shader::Info> Make(unsigned mask, bool used) {
  auto p = std::make_unique<Shader::Info>();
  p->stage = Shader::Stage::Fragment;
  p->l_stage = Shader::LogicalStage::Fragment;
  p->instance_export_mask = 16;
  p->buffers.emplace_back();
  p->buffers[0].instance_input = 2;
  p->buffers[0].instance_mask = mask;
  p->buffers[0].is_used = used;
  return p;
}
int main() {
  // The actual Program ownership path used by fresh compilation and preload.
  for (bool reverse : {false, true}) {
    Vulkan::Program p;
    std::array<Shader::Info *, 32> pointers{};
    for (unsigned i = 0; i < 32; ++i) {
      unsigned index = reverse ? 31 - i : i;
      auto metadata = Make(1u << (index % 4), index % 2);
      pointers[index] = metadata.get();
      Shader::StageSpecialization spec;
      spec.info = &p.info; // Must be rebound to module-owned metadata.
      if (reverse)
        p.InsertPermut({}, std::move(spec), index, std::move(metadata));
      else
        p.AddPermut({}, std::move(spec), std::move(metadata));
    }
    for (unsigned i = 0; i < 32; ++i) {
      Check(p.modules[i].info.get() == pointers[i]);
      Check(p.modules[i].spec.info == pointers[i]);
      Check(p.modules[i].info->buffers[0].instance_mask == (1u << (i % 4)));
      Check(p.modules[i].info->buffers[0].is_used == bool(i % 2));
    }
    // A live replacement invalidates only the selected module, retaining
    // pointers.
    Shader::InvalidateResourceProofs(*p.modules[0].info);
    Check(p.modules[0].info.get() == pointers[0]);
    Check(!p.modules[0].info->resource_proofs_valid);
    Check(p.modules[0].info->instance_export_mask == 0);
    Check(p.modules[0].info->buffers[0].is_used);
    Check(p.modules[0].info->buffers[0].instance_input == 255);
    Check(p.modules[0].info->buffers[0].instance_mask == 15);
    Check(p.modules[1].info->resource_proofs_valid);
    Check(p.modules[1].info->buffers[0].instance_mask == 2);
    // Draw state must refresh on the selected module, independently of
    // canonical info.
    std::array<u32, 16> ud{};
    ud[0] = 91;
    std::array<u32, 2> code{};
    Shader::ShaderParams params{ud, code, 123};
    Shader::RefreshModuleDrawState(*p.modules[2].info, params);
    Check(p.modules[2].info->pgm_base == params.Base());
    Check(p.modules[2].info->user_data.data() == ud.data());
    Check(p.modules[2].info->flattened_ud_buf[0] == 91);
    ud[0] = 123;
    Shader::RefreshModuleDrawState(*p.modules[2].info, params);
    Check(p.modules[2].info->flattened_ud_buf[0] == 123);
    Check(p.modules[3].info->flattened_ud_buf.empty());
  }
  using AmdGpu::PrimitiveType;
  Check(Vulkan::HasUnmodifiedVertexRouting(PrimitiveType::TriangleList, false,
                                           false, false));
  Check(!Vulkan::HasUnmodifiedVertexRouting(PrimitiveType::RectList, false,
                                            false, false));
  Check(!Vulkan::HasUnmodifiedVertexRouting(PrimitiveType::QuadList, false,
                                            false, false));
  Check(!Vulkan::HasUnmodifiedVertexRouting(PrimitiveType::TriangleList, true,
                                            false, false));
  Check(!Vulkan::HasUnmodifiedVertexRouting(PrimitiveType::TriangleList, false,
                                            true, false));
  Check(!Vulkan::HasUnmodifiedVertexRouting(PrimitiveType::TriangleList, false,
                                            false, true));
  std::cout << "PASS " << checks
            << " checks; per-module ownership, preload order, replacement, "
               "draw refresh, routing\n";
}
