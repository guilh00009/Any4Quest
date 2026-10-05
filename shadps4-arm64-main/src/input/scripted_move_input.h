// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include "core/vr/vr_runtime.h"
#include "core/vr/vr_move_input.h"

namespace Input::ScriptedMove {
struct Step {
    std::array<std::optional<Core::Vr::Pose>, 2> poses;
    std::array<Core::Vr::MoveInput::TouchButtons, 2> touch{};
    std::array<float, 2> trigger{};
};
inline bool Allowed(const char* enabled, const char* openxr) {
    return enabled && openxr && std::string_view{enabled} == "1" &&
           std::string_view{openxr} == "0";
}
template <typename ParsePose>
bool Parse(const std::string& token, Step& step, ParsePose parse_pose) {
    for (unsigned hand=0;hand<2;++hand) {
        const std::string prefix="move"+std::to_string(hand);
        if (token.starts_with(prefix+"=")) {
            step.poses[hand]=parse_pose(token.substr(prefix.size()+1));
            return true;
        }
        if (token.starts_with(prefix+"_grip=")) {
            step.touch[hand].squeeze=std::stof(token.substr(prefix.size()+6));
            return true;
        }
        if (token.starts_with(prefix+"_trigger=")) {
            step.trigger[hand]=std::stof(token.substr(prefix.size()+9));
            return true;
        }
        if (token == prefix+"_secondary") {
            step.touch[hand].secondary=true;
            return true;
        }
        if (token == prefix+"_menu") {
            step.touch[hand].menu=true;
            return true;
        }
        if (token == prefix+"_stick_click") {
            step.touch[hand].stick_click=true;
            return true;
        }
        if (token == prefix+"_primary") {
            step.touch[hand].primary=true;
            return true;
        }
    }
    return false;
}
} // namespace Input::ScriptedMove
