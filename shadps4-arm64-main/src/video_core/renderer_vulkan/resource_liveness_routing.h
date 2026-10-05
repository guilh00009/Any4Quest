// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/amdgpu/regs_vertex.h"
namespace Vulkan {
constexpr bool HasUnmodifiedVertexRouting(AmdGpu::PrimitiveType primitive, bool geometry,
                                          bool control, bool evaluation) {
    // Rect/quad pipelines inject auxiliary tessellation absent from guest stages.
    return !geometry && !control && !evaluation && primitive != AmdGpu::PrimitiveType::RectList &&
           primitive != AmdGpu::PrimitiveType::QuadList;
}
} // namespace Vulkan
