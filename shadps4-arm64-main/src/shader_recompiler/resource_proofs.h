// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "shader_recompiler/info.h"
namespace Shader {
inline void InvalidateResourceProofs(Info& info) {
    info.resource_proofs_valid = false;
    info.instance_export_mask = 0;
    info.uniform_selector = {};
    for (auto& buffer : info.buffers) {
        buffer.is_used = true;
        buffer.instance_input = 255;
        buffer.instance_mask = 15;
    }
}
inline void RefreshModuleDrawState(Info& info, const ShaderParams& params) {
    info.pgm_base = params.Base();
    info.user_data = params.user_data;
    info.RefreshFlatBuf();
}
} // namespace Shader
