// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/conditional_buffer_profile.h"
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
using namespace Vulkan::ConditionalBuffers;
int main() {
  unsigned checks = 0, failures = 0;
  auto check = [&](bool ok) {
    ++checks;
    failures += !ok;
  };
  check(MatchesInstance(InstanceVertexHash, InstanceFragmentHash, 4, true));
  check(
      !MatchesInstance(InstanceVertexHash ^ 1, InstanceFragmentHash, 4, true));
  check(
      !MatchesInstance(InstanceVertexHash, InstanceFragmentHash ^ 1, 4, true));
  check(!MatchesInstance(InstanceVertexHash, InstanceFragmentHash, 5, true));
  check(!MatchesInstance(InstanceVertexHash, InstanceFragmentHash, 4, false));
  for (std::uint32_t first :
       {0u, 1u, 2u, 3u, 4u, 7u, 15u, 0xfffffffdu, 0xfffffffeu, 0xffffffffu}) {
    for (std::uint32_t count = 0; count < 20; ++count) {
      unsigned expected = 0;
      for (std::uint32_t i = 0; i < count; ++i)
        expected |= 1u << ((std::uint64_t(first) + i) % 4);
      const auto mask = InstanceMask({first, count});
      check(mask == expected);
      check(IsInstanceInactive(InstanceFragmentHash, 7, 108, mask) ==
            !(expected & 8));
      check(IsInstanceInactive(InstanceFragmentHash, 8, 112, mask) ==
            !(expected & 4));
      check(!IsInstanceInactive(InstanceFragmentHash, 7, 112, mask));
      check(!IsInstanceInactive(InstanceFragmentHash, 8, 108, mask));
      check(!IsInstanceInactive(InstanceFragmentHash ^ 1, 7, 108, mask));
    }
  }
  check(InstanceMask({0, std::numeric_limits<std::uint32_t>::max()}) == 15);
  // Unknown/indirect range is conservatively represented by all active
  // branches.
  check(!IsInstanceInactive(InstanceFragmentHash, 7, 108, 15));
  check(!IsInstanceInactive(InstanceFragmentHash, 8, 112, 15));
  check(!IsInstanceInactive(InstanceFragmentHash, 7, 108, 16));
  // Old uniform-selector profile retains its distinct shader/binding contract.
  check(Matches(VertexHash, FragmentHash, 5, true));
  check(!MatchesInstance(VertexHash, FragmentHash, 4, true));
  check(!IsInstanceInactive(FragmentHash, 7, 108, 0));
  check(MatchesInstance(InstanceVertexHash2, InstanceFragmentHash2, 4, true));
  check(!MatchesInstance(InstanceVertexHash, InstanceFragmentHash2, 4, true));
  check(!MatchesInstance(InstanceVertexHash2, InstanceFragmentHash, 4, true));
  check(!MatchesInstance(InstanceVertexHash2, InstanceFragmentHash2, 4, false));
  check(!MatchesInstance(InstanceVertexHash2, InstanceFragmentHash2, 5, true));
  check(InstanceInput(InstanceFragmentHash) == 2 &&
        InstanceInput(InstanceFragmentHash2) == 3);
  for (unsigned mask = 0; mask < 16; ++mask) {
    check(IsInstanceInactive(InstanceFragmentHash2, 7, 112, mask) ==
          !(mask & 8));
    check(IsInstanceInactive(InstanceFragmentHash2, 8, 116, mask) ==
          !(mask & 4));
    check(!IsInstanceInactive(InstanceFragmentHash2, 7, 108, mask));
    check(!IsInstanceInactive(InstanceFragmentHash2, 8, 112, mask));
    check(!IsInstanceInactive(InstanceFragmentHash2, 9, 116, mask));
  }
  std::printf("%u checks, %u failures (direct ranges, wraparound, active and "
              "unknown paths)\n",
              checks, failures);
  return failures ? 1 : 0;
}
