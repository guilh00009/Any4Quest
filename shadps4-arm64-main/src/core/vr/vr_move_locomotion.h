// SPDX-FileCopyrightText: Copyright 2026 Any4Quest contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <charconv>
#include <string_view>
#include "core/vr/vr_move_input.h"

namespace Core::Vr::MoveInput {
enum class LocomotionProfile { Legacy, Buttons, Directional };
inline LocomotionProfile ParseLocomotion(std::string_view value) {
    if (value == "buttons") return LocomotionProfile::Buttons;
    if (value == "directional") return LocomotionProfile::Directional;
    return LocomotionProfile::Legacy;
}
using LocomotionButtons = std::array<std::uint16_t, 6>;
inline constexpr LocomotionButtons DefaultLocomotionButtons{Move, Circle, Square, Cross,
                                                          Square, Circle};
inline bool ParseLocomotionButtons(std::string_view text, LocomotionButtons& output) {
    LocomotionButtons parsed{};
    for (unsigned i=0; i<parsed.size(); ++i) {
        const auto comma = text.find(',');
        const auto part = text.substr(0, comma);
        unsigned value{};
        const auto result = std::from_chars(part.data(), part.data()+part.size(), value);
        if (part.empty() || result.ec != std::errc{} || result.ptr != part.data()+part.size() ||
            value > AllButtons || (value & (Trigger | Start | Select))) return false;
        parsed[i] = static_cast<std::uint16_t>(value);
        if (i+1 == parsed.size()) { if (comma != text.npos) return false; }
        else { if (comma == text.npos) return false; text.remove_prefix(comma+1); }
    }
    output = parsed;
    return true;
}
struct LocomotionResult {
    std::uint16_t buttons{};
    bool orient{};
    float yaw{};
};
// No virtual head or hand position, no accumulated artificial body translation.
// A snap is one guest press per stick deflection; the guest owns its angle/speed.
class StickLocomotion {
public:
    void Reset() { *this = {}; }
    LocomotionResult Update(LocomotionProfile profile, unsigned hand, TouchButtons touch,
                            float trigger, bool tracked, double now,
                            const LocomotionButtons& mapping = DefaultLocomotionButtons) {
        if (profile == LocomotionProfile::Legacy) return {MapTouchButtons(touch)};
        const float x=touch.stick_x, y=touch.stick_y;
        touch.stick_x=touch.stick_y=0;
        LocomotionResult out{MapTouchButtons(touch)};
        const bool valid = hand<2 && tracked && std::isfinite(x) && std::isfinite(y) &&
            std::isfinite(trigger) && std::isfinite(touch.squeeze) && std::isfinite(now);
        const bool interaction = trigger > .05f || touch.squeeze > .5f || touch.primary ||
            touch.secondary || touch.menu || touch.stick_click;
        if (!valid || interaction) { Reset(); return out; }
        const float magnitude=hand==0 ? std::hypot(x,y) : std::abs(x);
        if (magnitude <= .35f) { ready=true; active=false; return out; }
        if (!ready) return out; // neutral required after reset, tracking loss or interaction
        if (!active && magnitude < .65f) return out;
        if (!active) { active=true; since=now; direction=x<0 ? -1 : 1; }
        if (hand == 0) {
            if (profile == LocomotionProfile::Buttons) {
                // Four-way digital movement. Dominant axis avoids unsupported diagonals.
                out.buttons |= std::abs(y)>=std::abs(x) ? mapping[y>0 ? 0 : 1]
                                                       : mapping[x<0 ? 2 : 3];
            } else {
                out.orient=true; out.yaw=std::atan2(-x,y); out.buttons |= Move;
            }
        } else {
            // Latch direction until neutral; reversing a held stick cannot repeat a snap.
            const double elapsed=now-since;
            if (elapsed<0) { Reset(); return out; }
            if (profile == LocomotionProfile::Directional) {
                out.orient=true; out.yaw=direction<0 ? 1.57079632679f : -1.57079632679f;
                // Give guest tracking one frame before its Move-button edge.
                if (elapsed>=.05 && elapsed<.20) out.buttons |= Move;
            } else if (elapsed<.15) out.buttons |= mapping[direction<0 ? 4 : 5];
        }
        return out;
    }
private:
    bool ready{}, active{};
    double since{};
    int direction{};
};
} // namespace Core::Vr::MoveInput
