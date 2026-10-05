// SPDX-FileCopyrightText: Copyright 2026 Any4Quest contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Shared by the PC OpenXR host and standalone Quest host. These are the compact
// libSceMove/GEM button bits, not the unrelated bit positions in raw Move USB reports.
namespace Core::Vr::MoveInput {
inline constexpr std::uint16_t Select = 0x01;
inline constexpr std::uint16_t Trigger = 0x02;
inline constexpr std::uint16_t Move = 0x04;
inline constexpr std::uint16_t Start = 0x08;
inline constexpr std::uint16_t Triangle = 0x10;
inline constexpr std::uint16_t Circle = 0x20;
inline constexpr std::uint16_t Cross = 0x40;
inline constexpr std::uint16_t Square = 0x80;
inline constexpr std::uint16_t AllButtons = 0xff;

struct TouchButtons {
    bool primary{};   // X (left), A (right).
    bool secondary{}; // Y (left), B (right).
    bool stick_click{};
    bool menu{};      // Only exposed on the left Touch controller.
    float squeeze{};
    float stick_x{};
    float stick_y{};
};

inline std::uint16_t MapTouchButtons(const TouchButtons& touch) {
    std::uint16_t buttons{};
    if (touch.primary) buttons |= Cross;
    if (touch.secondary) buttons |= Circle;
    if (touch.stick_click) buttons |= Select;
    if (touch.menu || (std::isfinite(touch.stick_y) && touch.stick_y < -0.75f)) buttons |= Start;
    if (std::isfinite(touch.squeeze) && touch.squeeze > 0.5f) buttons |= Move;
    if (std::isfinite(touch.stick_y) && touch.stick_y > 0.75f) buttons |= Triangle;
    if (std::isfinite(touch.stick_x) && touch.stick_x < -0.75f) buttons |= Square;
    return buttons;
}

inline std::uint8_t TriggerToByte(float trigger) {
    return std::isfinite(trigger)
               ? static_cast<std::uint8_t>(std::lround(std::clamp(trigger, 0.0f, 1.0f) * 255.0f))
               : 0;
}
} // namespace Core::Vr::MoveInput
