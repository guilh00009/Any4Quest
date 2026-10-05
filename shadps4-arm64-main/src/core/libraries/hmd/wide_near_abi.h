// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace Libraries::Hmd::ObservedAbi {

// Observed CUSA08291 01.00 call at eboot+0x3737d:
// (wide_near, tracker, flip_arg, overlay, option).
// This is a verified prefix, NOT a complete public SDK definition.
// Native-faithful submission still requires verified composition and slot semantics.
// The opt-in desktop preview is approximate and does not write these slots.
struct EyeUv {
    float scale_x, scale_y, offset_x, offset_y;
};
struct WideNearPrefix {
    std::uint64_t texture[2][2]; // [eye][wide=0, near=1], guest addresses
    std::uint64_t sampler;
    EyeUv uv[2][2];
    // Filled from tan_top * r.NearWideStartAmount / r.NearWideEndAmount.
    // The actual transition metric (radial, rectangular, etc.) is not yet verified.
    float transition_start;
    float transition_end;
    std::uint64_t slot_address; // six rotating 8-byte slots in the observed caller
    std::uint32_t unknown_78; // caller writes 4000; units/meaning not established
    std::uint32_t reserved_7c;
    std::uint64_t unknown_80;
    std::uint64_t unknown_88;
    std::uint64_t flags; // observed 5; bit semantics not established
};
static_assert(sizeof(EyeUv) == 0x10);
static_assert(offsetof(WideNearPrefix, sampler) == 0x20);
static_assert(offsetof(WideNearPrefix, uv) == 0x28);
static_assert(offsetof(WideNearPrefix, transition_start) == 0x68);
static_assert(offsetof(WideNearPrefix, slot_address) == 0x70);
static_assert(offsetof(WideNearPrefix, flags) == 0x90);
static_assert(sizeof(WideNearPrefix) == 0x98);

} // namespace Libraries::Hmd::ObservedAbi
