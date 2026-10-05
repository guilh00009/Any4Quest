// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Vulkan::ConditionalBuffers {
// Verified uniform selector in this VS/PS pair: VS exports Param5.x as
// bitcast(enable ? selector : 0) | (enable << 31); PS input3 is flat Param5.
// PS buffer6 is read only for selector&3 == 3; buffer7 only for == 2.
inline constexpr std::uint64_t VertexHash = 0x37b889e3eb0d588fULL;
inline constexpr std::uint64_t FragmentHash = 0x3866c847a8b49ceaULL;
constexpr bool Matches(std::uint64_t vs, std::uint64_t fs, unsigned offset, bool flat) {
    return vs == VertexHash && fs == FragmentHash && offset == 5 && flat;
}
constexpr unsigned Material(std::uint32_t enable, std::uint32_t selector) {
    return enable ? selector & 3U : 0U;
}
constexpr bool IsInactive(std::uint64_t fs, unsigned binding, unsigned sharp,
                          unsigned material) {
    if (fs != FragmentHash || material > 3) return false;
    return (binding == 6 && sharp == 108 && material != 3) ||
           (binding == 7 && sharp == 112 && material != 2);
}
} // namespace Vulkan::ConditionalBuffers
