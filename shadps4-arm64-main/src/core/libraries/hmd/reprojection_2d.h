// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Libraries::Hmd {
// ABI corroborated by Orbital db_types.json and ALVR-PS4 client/src/reproj.cpp.
// Independent implementation; see reprojection-2d-reference.txt for provenance.
struct Reprojection2dParam {
    std::uint64_t texture;
    std::uint64_t sampler;
    std::array<float, 4> uv;
    std::uint64_t label;
    std::uint32_t time_us;
    std::uint32_t padding;
    std::array<std::uint64_t, 4> reserved;
};
static_assert(sizeof(Reprojection2dParam) == 0x50);
static_assert(offsetof(Reprojection2dParam, uv) == 0x10);
static_assert(offsetof(Reprojection2dParam, label) == 0x20);
static_assert(offsetof(Reprojection2dParam, time_us) == 0x28);
inline bool Valid2dValues(const Reprojection2dParam& p) {
    if (p.time_us < 2000 || p.time_us >= 7000) return false;
    for (float value : p.uv) if (!std::isfinite(value)) return false;
    for (auto value : p.reserved) if (value != 0) return false;
    return true;
}
inline std::array<float, 2> Transform2dUv(const std::array<float, 4>& transform,
                                        float x, float y) {
    return {x * transform[0] + transform[2], y * transform[1] + transform[3]};
}
} // namespace Libraries::Hmd
