// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <iostream>
#include "input/scripted_move_input.h"
int main() {
    using namespace Input::ScriptedMove;
    Step s;
    auto pose=[](const std::string& v)->std::optional<Core::Vr::Pose> {
        if(v!="pose") return std::nullopt;
        return Core::Vr::Pose{{1,2,3},{}};
    };
    assert(Allowed("1","0"));
    assert(!Allowed(nullptr,"0") && !Allowed("0","0") && !Allowed("1",nullptr));
    assert(!Allowed("1","1") && !Allowed("1","auto"));
    assert(Parse("move0=pose",s,pose) && s.poses[0] && !s.poses[1]);
    assert(Parse("move1=pose",s,pose) && s.poses[1]->position.y==2);
    assert(Parse("move0_grip=1",s,pose));
    assert(Core::Vr::MoveInput::MapTouchButtons(s.touch[0])==Core::Vr::MoveInput::Move);
    assert(Core::Vr::MoveInput::MapTouchButtons(s.touch[1])==0);
    assert(Parse("move1_primary",s,pose));
    assert(Core::Vr::MoveInput::MapTouchButtons(s.touch[1])==Core::Vr::MoveInput::Cross);
    assert(Parse("move1_trigger=0.5",s,pose));
    assert(Core::Vr::MoveInput::TriggerToByte(s.trigger[1])==128 && s.trigger[0]==0);
    assert(!Parse("move2_grip=1",s,pose) && !Parse("ly=0",s,pose));
    assert(Parse("move1=invalid",s,pose) && !s.poses[1]);
    std::cout<<"PASS script Move parsing, independent hands, real grip mapping, headset-disabled gate\n";
}
