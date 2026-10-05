// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstring>
#include <cstdio>
#include "core/libraries/hmd/wide_near_abi.h"

int main() {
    using namespace Libraries::Hmd::ObservedAbi;
    // Synthetic byte fixture tests guest-address width and interleaved eye/layer order.
    std::array<unsigned char, 0x98> bytes{};
    auto put = [&](std::size_t offset, auto value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    for (unsigned i=0; i<4; ++i) {
        put(i*8, std::uint64_t{0x200000000} + i*0x100);
        put(0x28+i*0x10, float(i+1));
    }
    put(0x68, 0.375f); put(0x6c, 0.475f);
    put(0x70, std::uint64_t{0x200001000});
    put(0x90, std::uint64_t{5});
    WideNearPrefix param{};
    std::memcpy(&param, bytes.data(), bytes.size());
    for (unsigned eye=0; eye<2; ++eye)
        for (unsigned layer=0; layer<2; ++layer) {
            unsigned i=eye*2+layer;
            if (param.texture[eye][layer] != 0x200000000ULL+i*0x100 ||
                param.uv[eye][layer].scale_x != float(i+1)) return 1;
        }
    if (param.transition_start != 0.375f || param.transition_end != 0.475f ||
        param.slot_address != 0x200001000ULL || param.flags != 5) return 2;
    std::puts("PASS observed ABI prefix: 4 eye/layer pairs and control offsets");
}
