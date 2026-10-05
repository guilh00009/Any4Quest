// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <initializer_list>
#include "video_core/renderer_vulkan/conditional_buffer_profile.h"
using namespace Vulkan::ConditionalBuffers;
int main() {
    int failures = 0, checks = 0;
    const auto check = [&](bool ok) { ++checks; failures += !ok; };
    check(SelectorSharp(VertexHash) == 88);
    check(SelectorSharp(LaterVertexHash) == 36);
    check(SelectorSharp(LaterVertexHash ^ 1) == ~0u);
    check(HasUniformSelector(VertexHash, 10));
    check(HasUniformSelector(LaterVertexHash, 8));
    check(!HasUniformSelector(VertexHash, 8));
    check(!HasUniformSelector(LaterVertexHash, 10));
    check(!HasUniformSelector(LaterVertexHash ^ 1, 8));
    check(!HasUniformSelector(0, 8));
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
    // The production binder now uses the same compiler mask for uniform and
    // instance selectors. Exhaust all uniform values and fragment access masks.
    for (unsigned enable : {0u, 1u, 0xffffffffu}) {
        for (unsigned selector=0;selector<4;++selector) {
            const unsigned material=Material(enable,selector);
            const unsigned drawn=1u<<material;
            for (unsigned used=0;used<16;++used) {
                check(Shader::Liveness::Inactive(used,drawn,3,1u<<5,5,true) == !(used&drawn));
                check(!Shader::Liveness::Inactive(used,drawn,3,1u<<5,4,true));
                check(!Shader::Liveness::Inactive(used,drawn,3,1u<<5,5,false));
                check(!Shader::Liveness::Inactive(used,drawn,3,0,5,true));
            }
        }
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
