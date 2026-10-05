// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/libraries/hmd/wide_near_preview.h"

namespace Libraries::Hmd::Preview {
// The existing VR transport represents a common mirrored FOV. Reconstruct both
// eyes into a symmetric intersection of the supplied full-view frusta so that
// the reported projection exactly matches the pixels and never adds overscan.
inline bool CommonVrView(const std::array<float, 4>& left, const std::array<float, 4>& right,
                         std::array<float, 4>& view) {
    if (!ValidMap(left) || !ValidMap(right))
        return false;
    float x = 10, y = 10;
    for (const auto& map : {left, right}) {
        const float l = map[2] / map[0], r = (1 - map[2]) / map[0];
        const float t = map[3] / map[1], b = (1 - map[3]) / map[1];
        for (float v : {l, r, t, b})
            if (!std::isfinite(v) || v <= .05f || v >= 10)
                return false;
        x = std::min({x, l, r});
        y = std::min({y, t, b});
    }
    view = {1 / (2 * x), 1 / (2 * y), .5f, .5f};
    return true;
}
inline bool ValidRenderPose(const float* position, const float* orientation) {
    float norm{};
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(position[i]))
            return false;
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(orientation[i]))
            return false;
        norm += orientation[i] * orientation[i];
    }
    return norm > .25f && norm < 4;
}
} // namespace Libraries::Hmd::Preview
