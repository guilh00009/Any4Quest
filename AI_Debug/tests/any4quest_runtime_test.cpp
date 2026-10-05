// SPDX-FileCopyrightText: Copyright 2026 Any4Quest contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Compiles the actual vr_runtime.cpp with only its logging, platform, and host-link
// dependencies replaced. This is not a headset, full-emulator, or game compatibility test.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include "core/vr/vr_runtime.h"
#include "core/vr/vr_protocol.h"
#include "core/vr/vr_move_input.h"

static std::atomic<u64> test_time{1000000};
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return test_time.load(); }
}
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " #x " failed\n"; std::exit(1); } } while (0)
static bool Near(float a, float b, float epsilon = 0.0002f) { return std::abs(a - b) < epsilon; }
static void Mode(const char* value) {
#ifdef _WIN32
    _putenv_s("SHADPS4_VR_INPUT_MODE", value);
    _putenv_s("SHADPS4_VR", "0");
#else
    setenv("SHADPS4_VR_INPUT_MODE", value, 1);
    setenv("SHADPS4_VR", "0", 1);
#endif
    Core::Vr::Runtime::Instance().Configure(false, false);
}


static void LocomotionTests() {
    using namespace Core::Vr;
    using namespace Core::Vr::MoveInput;
    const auto buttons=LocomotionProfile::Buttons, directional=LocomotionProfile::Directional;
    LocomotionButtons map=DefaultLocomotionButtons;
    CHECK(ParseLocomotionButtons("4,32,128,64,128,32",map));
    const auto original=map;
    for (auto invalid : {"4,32,128,64,128", "4,32,128,64,128,32,4", "4,32,128,64,128,8",
                         "4,32,128,64,128,256", "4,32,128,64,128,nan", "4,32,128,64,128,-1"}) {
        CHECK(!ParseLocomotionButtons(invalid,map) && map==original);
    }
    CHECK(ParseLocomotion("typo")==LocomotionProfile::Legacy);
    StickLocomotion state;
    CHECK(state.Update(buttons,0,{.stick_y=1},0,true,0).buttons==0);
    state.Update(buttons,0,{},0,true,0);
    CHECK(state.Update(buttons,0,{.stick_y=.64f},0,true,0).buttons==0);
    CHECK(state.Update(buttons,0,{.stick_y=.65f},0,true,0).buttons==Move);
    CHECK(state.Update(buttons,0,{.stick_y=.4f},0,true,0).buttons==Move);
    CHECK(state.Update(buttons,0,{.stick_y=.35f},0,true,0).buttons==0);
    const std::array<TouchButtons,4> axes{{{.stick_y=1},{.stick_y=-1},{.stick_x=-1},{.stick_x=1}}};
    const std::array<float,4> yaw{0,3.14159265f,1.57079633f,-1.57079633f};
    for (unsigned i=0;i<4;++i) {
        state.Update(buttons,0,{},0,true,0);
        auto out=state.Update(buttons,0,axes[i],0,true,0);
        CHECK(out.buttons==map[i] && !out.orient);
        state.Update(directional,0,{},0,true,0);
        out=state.Update(directional,0,axes[i],0,true,0);
        CHECK(out.buttons==Move && out.orient && Near(std::abs(out.yaw),std::abs(yaw[i])));
    }
    for (int sign : {-1,1}) {
        state.Reset(); state.Update(directional,1,{},0,true,0);
        TouchButtons stick{.stick_x=float(sign)};
        CHECK(state.Update(directional,1,stick,0,true,1).buttons==0);
        auto out=state.Update(directional,1,stick,0,true,1.06);
        CHECK(out.buttons==Move && Near(out.yaw,-sign*1.57079633f));
        CHECK(state.Update(directional,1,stick,0,true,1.21).buttons==0);
        CHECK(state.Update(directional,1,stick,0,true,5).buttons==0);
        stick.stick_x=-stick.stick_x;
        CHECK(state.Update(directional,1,stick,0,true,6).buttons==0);
        state.Update(directional,1,{},0,true,7);
        CHECK(state.Update(buttons,1,stick,0,true,8).buttons==map[sign>0 ? 4 : 5]);
        CHECK(state.Update(buttons,1,stick,0,true,8.2).buttons==0);
    }
    for (unsigned hand=0;hand<2;++hand) {
        TouchButtons stick{.stick_x=1,.stick_y=1};
        state.Reset(); state.Update(directional,hand,{},0,true,0);
        CHECK(!state.Update(directional,hand,stick,1,true,1).orient);
        CHECK(!state.Update(directional,hand,stick,0,true,2).orient);
        state.Update(directional,hand,{},0,true,3);
        CHECK(state.Update(directional,hand,stick,0,true,4).orient);
        CHECK(!state.Update(directional,hand,stick,0,false,5).orient);
        CHECK(!state.Update(directional,hand,stick,0,true,6).orient);
        state.Update(directional,hand,{},0,true,7);
        stick.squeeze=1;
        auto out=state.Update(directional,hand,stick,0,true,8);
        CHECK(out.buttons==Move && !out.orient);
        stick.squeeze=0; stick.stick_x=std::numeric_limits<float>::quiet_NaN();
        CHECK(!state.Update(directional,hand,stick,0,true,9).orient);
        state.Reset();
        CHECK(!state.Update(directional,hand,{.stick_x=1},0,true,10).orient);
    }
    // Exercise actual runtime transforms, recenter, gyro and original physical controls.
    auto setProfile=[](const char* v) {
#ifdef _WIN32
        _putenv_s("SHADPS4_MOVE_LOCOMOTION",v);
#else
        setenv("SHADPS4_MOVE_LOCOMOTION",v,1);
#endif
        Mode("move");
    };
    auto& runtime=Runtime::Instance();
    setProfile("directional");
    DeviceState head{}; head.tracked=true; head.pose.orientation=FromYawPitch(.7f,0);
    runtime.UpdateHead(head); runtime.RecenterSeat();
    MoveHostState host{}; host.connected=host.device.tracked=host.touch_valid=true;
    host.device.pose.position={.25f,-.4f,-.5f};
    host.device.pose.orientation=FromYawPitch(.3f,.1f);
    runtime.UpdateMove(0,host);
    const auto physical=runtime.GetMove(0);
    const auto headBefore=runtime.GetHead();
    host.touch.stick_x=1; runtime.UpdateMove(0,host);
    auto moved=runtime.GetMove(0);
    CHECK(moved.buttons==Move && Near(moved.device.pose.orientation.y,-std::sqrt(.5f)));
    CHECK(Near(moved.device.pose.position.x,physical.device.pose.position.x));
    CHECK(Near(moved.device.pose.position.z,physical.device.pose.position.z));
    CHECK(Near(moved.gyro.y,0));
    CHECK(Near(runtime.GetHead().pose.orientation.y,headBefore.pose.orientation.y));
    host.touch={}; runtime.UpdateMove(0,host);
    CHECK(runtime.GetMove(0).buttons==0);
    CHECK(Near(runtime.GetMove(0).device.pose.orientation.y,physical.device.pose.orientation.y));
    CHECK(Near(runtime.GetMove(0).gyro.y,0));
    host.touch.stick_y=1; runtime.UpdateMove(0,host);
    runtime.RecenterSeat(); runtime.UpdateMove(0,host);
    CHECK(runtime.GetMove(0).buttons==0);
    host.touch={}; runtime.UpdateMove(0,host);
    host.touch.stick_y=1; runtime.UpdateMove(0,host);
    CHECK(runtime.GetMove(0).buttons==Move);
    host.trigger=1; runtime.UpdateMove(0,host);
    CHECK(runtime.GetMove(0).buttons==Trigger);
    CHECK(Near(runtime.GetMove(0).device.pose.orientation.y,physical.device.pose.orientation.y));
    runtime.ReleaseMoves(); host.trigger=0; runtime.UpdateMove(0,host);
    CHECK(runtime.GetMove(0).buttons==0);
    setProfile("buttons"); host.touch={}; runtime.UpdateMove(0,host);
    host.touch.stick_x=-1; runtime.UpdateMove(0,host);
    CHECK(runtime.GetMove(0).buttons==Square);
    CHECK(Near(runtime.GetMove(0).device.pose.orientation.y,physical.device.pose.orientation.y));
    Mode("gamepad"); runtime.UpdateMove(0,host);
    CHECK(!runtime.GetMove(0).connected && runtime.GetPad().tracked);
    setProfile("legacy");
}

int main() {
    using namespace Core::Vr;
    namespace MI = MoveInput;
    CHECK(sizeof(Protocol::MoveState) == 88);
    CHECK(sizeof(Protocol::MoveFeedback) == 20);
    CHECK(MI::MapTouchButtons({}) == 0);
    CHECK(MI::MapTouchButtons({.primary = true}) == MI::Cross);
    CHECK(MI::MapTouchButtons({.secondary = true}) == MI::Circle);
    CHECK(MI::MapTouchButtons({.stick_click = true}) == MI::Select);
    CHECK(MI::MapTouchButtons({.menu = true}) == MI::Start);
    CHECK(MI::MapTouchButtons({.squeeze = 0.9f}) == MI::Move);
    CHECK(MI::MapTouchButtons({.stick_x = -1}) == MI::Square);
    CHECK(MI::MapTouchButtons({.stick_y = 1}) == MI::Triangle);
    CHECK(MI::MapTouchButtons({.stick_y = -1}) == MI::Start);
    CHECK(MI::MapTouchButtons({.stick_x = .75f, .stick_y = .75f}) == 0);
    CHECK(MI::TriggerToByte(0) == 0 && MI::TriggerToByte(1) == 255);
    CHECK(MI::TriggerToByte(.5f) == 128);
    CHECK(MI::TriggerToByte(-1) == 0 && MI::TriggerToByte(2) == 255);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(MI::TriggerToByte(nan) == 0);
    CHECK(MI::MapTouchButtons({.squeeze = nan, .stick_x = nan, .stick_y = nan}) == 0);

    auto& runtime = Runtime::Instance();
    Mode("gamepad");
    CHECK(!runtime.IsMoveEnabled());
    MoveHostState host{};
    host.connected = true;
    host.device.tracked = true;
    host.device.pose.position = {.25f, -.2f, -.5f};
    host.trigger = .5f;
    host.buttons = MI::Cross;
    runtime.UpdateMove(0, host);
    CHECK(!runtime.GetMove(0).connected);
    Mode("typo");
    CHECK(!runtime.IsMoveEnabled());
    Mode("move");
    CHECK(runtime.IsMoveEnabled());
    runtime.FixSeat();
    runtime.UpdateMove(0, host);
    auto left = runtime.GetMove(0);
    CHECK(left.connected && left.device.tracked);
    CHECK(left.buttons == (MI::Cross | MI::Trigger) && left.trigger == 128);
    CHECK(Near(left.device.pose.position.z, 1.0f));
    CHECK(Near(left.acceleration.y, 1));
    CHECK(!runtime.GetMove(1).connected);
    CHECK(!runtime.GetMove(2).connected);
    const auto first_timestamp = left.timestamp_us;
    const auto first_sequence = left.device.sequence;
    CHECK(runtime.GetMove(0).timestamp_us == first_timestamp);
    CHECK(runtime.GetMove(0).device.sequence == first_sequence);
    runtime.UpdateMove(999, host);
    CHECK(runtime.GetMove(0).timestamp_us == first_timestamp);
    host.device.pose.position.x = -.25f;
    test_time += 10000;
    runtime.UpdateMove(1, host);
    CHECK(runtime.GetMove(1).device.pose.position.x < 0);
    CHECK(runtime.GetMove(0).device.pose.position.x > 0);

    MoveState history[64];
    CHECK(runtime.ReadMoveRecent(0, 0, history, 64) == 1);
    CHECK(runtime.ReadMoveRecent(0, first_timestamp, history, 64) == 0);
    CHECK(runtime.ReadMoveRecent(0, 0, nullptr, 1) == 0);
    CHECK(runtime.ReadMoveRecent(2, 0, history, 1) == 0);
    CHECK(runtime.ReadMoveRecent(0, 0, history, 0) == 0);
    for (unsigned i = 0; i < 40; ++i) {
        test_time += 10000;
        runtime.UpdateMove(0, host);
    }
    CHECK(runtime.ReadMoveRecent(0, 0, history, 64) == 32);
    for (unsigned i = 1; i < 32; ++i) CHECK(history[i].timestamp_us > history[i-1].timestamp_us);
    CHECK(runtime.ReadMoveRecent(0, history[28].timestamp_us, history, 1) == 1);
    CHECK(runtime.ReadMoveRecent(0, std::numeric_limits<u64>::max(), history, 64) == 0);

    unsigned feedback_calls = 0;
    runtime.SetMoveFeedbackListener([&](u32 hand, const MoveFeedback&) {
        ++feedback_calls;
        (void)runtime.GetMoveFeedback(hand); // Callback must run outside Runtime's lock.
    });
    runtime.SetMoveVibration(0, 201);
    runtime.SetMoveLight(0, 1, 2, 3);
    CHECK(runtime.GetMoveFeedback(0).intensity == 201);
    CHECK(runtime.GetMoveFeedback(0).green == 2);
    CHECK(runtime.GetMoveFeedback(1).intensity == 0);
    CHECK(feedback_calls == 4);
    runtime.SetMoveVibration(9, 255);
    CHECK(feedback_calls == 4);

    // World-to-seat transforms and velocities use the same frame as HMD/gamepad.
    DeviceState head{};
    head.tracked = true;
    head.pose.position = {1, 2, 3};
    head.pose.orientation = FromYawPitch(1.57079632679f, 0);
    runtime.UpdateHead(head);
    runtime.RecenterSeat();
    host.device.pose.position = {2, 2, 3};
    host.device.pose.orientation = head.pose.orientation;
    host.device.linear_velocity = {1, 0, 0};
    host.linear_velocity_valid = true;
    host.device.angular_velocity = {0, 1, 0};
    host.angular_velocity_valid = true;
    test_time += 10000;
    runtime.UpdateMove(0, host);
    left = runtime.GetMove(0);
    CHECK(Near(left.device.pose.position.x, 0));
    CHECK(Near(left.device.pose.position.y, 0));
    CHECK(Near(left.device.pose.position.z, 2.5f));
    CHECK(Near(left.device.pose.orientation.w, 1));
    CHECK(Near(left.device.linear_velocity.z, 1));
    CHECK(Near(left.device.angular_velocity.y, 1));
    const auto before_recenter = left.timestamp_us;
    head.pose.position.x = 2;
    runtime.UpdateHead(head);
    runtime.RecenterSeat();
    left = runtime.GetMove(0);
    CHECK(Near(left.device.pose.position.z, 1.5f));
    CHECK(left.timestamp_us == before_recenter); // Recenter does not invent sensor samples.

    runtime.ReleaseMoves();
    CHECK(!runtime.GetMove(0).connected && !runtime.GetMove(1).connected);
    CHECK(runtime.GetMoveFeedback(0).intensity == 0);
    CHECK(runtime.ReadMoveRecent(0, 0, history, 64) == 0);
    runtime.FixSeat();
    host = {};
    host.connected = host.device.tracked = true;
    test_time += 10000;
    runtime.UpdateMove(0, host);
    CHECK(Near(runtime.GetMove(0).acceleration.y, 1));
    host.device.pose.position.x = .01f;
    test_time += 10000;
    runtime.UpdateMove(0, host);
    CHECK(Near(runtime.GetMove(0).device.linear_velocity.x, 1));
    CHECK(Near(runtime.GetMove(0).acceleration.x, 0)); // First derivative has no previous velocity.
    host.device.pose.position.x = .03f;
    test_time += 10000;
    runtime.UpdateMove(0, host);
    CHECK(Near(runtime.GetMove(0).device.linear_velocity.x, 2));
    CHECK(Near(runtime.GetMove(0).acceleration.x, 100.f / 9.80665f, .002f));
    host.device.pose.orientation.w = -1;
    test_time += 10000;
    runtime.UpdateMove(0, host);
    CHECK(Near(runtime.GetMove(0).gyro.y, 0)); // q and -q do not cause a spin.
    runtime.SetMoveVibration(0, 123);
    CHECK(runtime.GetMoveFeedback(0).intensity == 123);
    host.device.tracked = false;
    test_time += 10000;
    runtime.UpdateMove(0, host);
    CHECK(runtime.GetMove(0).connected && !runtime.GetMove(0).device.tracked);
    CHECK(Near(runtime.GetMove(0).acceleration.y, 0));
    CHECK(runtime.GetMoveFeedback(0).intensity == 0);
    runtime.SetMoveVibration(0, 255);
    CHECK(runtime.GetMoveFeedback(0).intensity == 0);
    host.device.tracked = true;
    host.device.pose.position.x = 9;
    test_time += 10000;
    runtime.UpdateMove(0, host);
    CHECK(Near(runtime.GetMove(0).device.linear_velocity.x, 0));
    CHECK(Near(runtime.GetMove(0).acceleration.y, 1));
    host.device.pose.orientation = {nan, 0, 0, 1};
    host.trigger = nan;
    test_time += 10000;
    runtime.UpdateMove(0, host);
    CHECK(runtime.GetMove(0).connected && !runtime.GetMove(0).device.tracked);
    CHECK(runtime.GetMove(0).trigger == 0);
    host.device.pose.orientation = {};
    host.device.pose.position.x = std::numeric_limits<float>::infinity();
    runtime.UpdateMove(0, host);
    CHECK(!runtime.GetMove(0).device.tracked);
    host.connected = false;
    runtime.UpdateMove(0, host);
    CHECK(!runtime.GetMove(0).connected);
    CHECK(runtime.ReadMoveRecent(0, 0, history, 64) == 0);
    host.device.pose.position = {};
    host.connected = true;
    test_time += 10000;
    runtime.UpdateMove(0, host);
    CHECK(runtime.ReadMoveRecent(0, 0, history, 64) == 1);
    // Queued old, future and repeated source samples cannot renew a connection or counters.
    auto steady_ns = [] { return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()); };
    auto sequence = runtime.GetMove(0).device.sequence;
    host.sample_time_ns = steady_ns() - 300000000;
    runtime.UpdateMove(0, host);
    CHECK(runtime.GetMove(0).device.sequence == sequence);
    host.sample_time_ns = steady_ns() + 1000000000;
    runtime.UpdateMove(0, host);
    CHECK(runtime.GetMove(0).device.sequence == sequence);
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    host.sample_time_ns = steady_ns();
    test_time += 10000;
    runtime.UpdateMove(0, host);
    sequence = runtime.GetMove(0).device.sequence;
    runtime.UpdateMove(0, host);
    CHECK(runtime.GetMove(0).device.sequence == sequence);
    --host.sample_time_ns;
    runtime.UpdateMove(0, host);
    CHECK(runtime.GetMove(0).device.sequence == sequence);
    runtime.SetMoveVibration(0, 255);
    std::this_thread::sleep_for(std::chrono::milliseconds{270});
    CHECK(!runtime.GetMove(0).connected);
    CHECK(!runtime.GetMove(0).device.tracked);
    CHECK(runtime.GetMove(0).buttons == 0 && runtime.GetMove(0).trigger == 0);
    CHECK(runtime.GetMoveFeedback(0).intensity == 0);
    CHECK(runtime.ReadMoveRecent(0, 0, history, 64) == 0);
    runtime.SetMoveFeedbackListener({});
    Mode("gamepad");
    CHECK(!runtime.IsMoveEnabled());
    CHECK(runtime.GetPad().tracked); // Legacy fallback remains the legacy gamepad behavior.
    LocomotionTests();
    std::cout << checks << " runtime/input/protocol checks passed\n";
}
