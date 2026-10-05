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
constexpr bool IsInactive(std::uint64_t fs, unsigned binding, unsigned sharp, unsigned material) {
    if (fs != FragmentHash || material > 3)
        return false;
    return (binding == 6 && sharp == 108 && material != 3) ||
           (binding == 7 && sharp == 112 && material != 2);
}
// Verified instanced variant: VS Param4.x carries (InstanceId & 3) plus a high
// flag bit. Flat FS input2 routes Param4; only its low two bits select resources.
// FS buffer7/sharp108 is read only for 3; buffer8/sharp112 only for 2.
inline constexpr std::uint64_t InstanceVertexHash = 0xc5c13d3c7614a1b7ULL;
inline constexpr std::uint64_t InstanceFragmentHash = 0xbe588d5ae93aadabULL;
// Second verified variant has identical normalized VS IR, with FS input3
// routed to Param4 and different flattened descriptor locations.
inline constexpr std::uint64_t InstanceVertexHash2 = 0x7a748a197614a1b7ULL;
inline constexpr std::uint64_t InstanceFragmentHash2 = 0x49dedda558135ca5ULL;
constexpr unsigned InstanceInput(std::uint64_t fs) {
    return fs == InstanceFragmentHash ? 2 : fs == InstanceFragmentHash2 ? 3 : 0;
}
constexpr unsigned InstanceSharp(std::uint64_t fs, unsigned binding) {
    if (fs == InstanceFragmentHash)
        return binding == 7 ? 108 : binding == 8 ? 112 : ~0u;
    if (fs == InstanceFragmentHash2)
        return binding == 7 ? 112 : binding == 8 ? 116 : ~0u;
    return ~0u;
}
struct InstanceRange {
    std::uint32_t first;
    std::uint32_t count;
};
constexpr bool MatchesInstance(std::uint64_t vs, std::uint64_t fs, unsigned offset, bool flat) {
    return ((vs == InstanceVertexHash && fs == InstanceFragmentHash) ||
            (vs == InstanceVertexHash2 && fs == InstanceFragmentHash2)) &&
           offset == 4 && flat;
}
// The complete set of low-two-bit selectors across a direct draw. No memory read
// or vertex-dependent value is involved; four consecutive IDs cover every branch.
constexpr unsigned InstanceMask(InstanceRange range) {
    if (range.count >= 4)
        return 0xf;
    unsigned mask = 0;
    for (unsigned i = 0; i < range.count; ++i)
        mask |= 1u << ((range.first + i) & 3u);
    return mask;
}
constexpr bool IsInstanceInactive(std::uint64_t fs, unsigned binding, unsigned sharp,
                                  unsigned mask) {
    if ((fs != InstanceFragmentHash && fs != InstanceFragmentHash2) || mask > 0xf)
        return false;
    if (sharp != InstanceSharp(fs, binding))
        return false;
    return (binding == 7 && !(mask & (1u << 3))) || (binding == 8 && !(mask & (1u << 2)));
}
} // namespace Vulkan::ConditionalBuffers
