// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace Libraries::Hmd::Preview {
// Experimental desktop model: linear interpolation over a tangent-space annulus.
// The circular region is supported by public GDC material; the linear curve is
// an explicit approximation, not a recovered Sony compositor implementation.
inline float WideWeight(float x, float y, float start, float end) {
    return std::clamp((std::hypot(x,y)-start)/(end-start),0.0f,1.0f);
}
inline std::array<float,2> Tangent(const std::array<float,4>& view,float u,float v) {
    return {(u-view[2])/view[0],(v-view[3])/view[1]};
}
inline std::array<float,2> Uv(const std::array<float,4>& map,float x,float y) {
    return {x*map[0]+map[2],y*map[1]+map[3]};
}
inline bool ValidMap(const std::array<float,4>& map) {
    for(float v:map) if(!std::isfinite(v)) return false;
    return map[0]>0 && map[1]>0;
}
inline bool ValidBand(float start,float end) {
    return std::isfinite(start)&&std::isfinite(end)&&start>=0&&end>start;
}
} // namespace Libraries::Hmd::Preview
