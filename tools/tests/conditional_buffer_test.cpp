// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "video_core/renderer_vulkan/conditional_buffer_profile.h"
using namespace Vulkan::ConditionalBuffers;
int main() {
    int failures = 0, checks = 0;
    const auto check = [&](bool ok) { ++checks; failures += !ok; };
    check(Matches(VertexHash, FragmentHash, 5, true));
    check(!Matches(VertexHash ^ 1, FragmentHash, 5, true));
    check(!Matches(VertexHash, FragmentHash ^ 1, 5, true));
    check(!Matches(VertexHash, FragmentHash, 4, true));
    check(!Matches(VertexHash, FragmentHash, 5, false));
    for (unsigned material = 0; material < 4; ++material) {
        check(Material(0, material) == 0);
        check(Material(1, material | 0xffff0000) == material);
        check(IsInactive(FragmentHash, 6, 108, material) == (material != 3));
        check(IsInactive(FragmentHash, 7, 112, material) == (material != 2));
        check(!IsInactive(FragmentHash, 6, 112, material));
        check(!IsInactive(FragmentHash, 8, 116, material));
        check(!IsInactive(FragmentHash ^ 1, 6, 108, material));
    }
    check(!IsInactive(FragmentHash, 6, 108, 4));
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
