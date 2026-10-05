// SPDX-FileCopyrightText: Copyright 2026 Any4Quest contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Fixture for test_pc_move_host.py. The runner substitutes production source at the
// markers below. This file is not a standalone test and does not compile the Win32 host.

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <vector>

#include <openxr/openxr.h>

#include "core/vr/vr_move_input.h"
#include "core/vr/vr_runtime.h"

#ifdef NDEBUG
#error "This fixture requires enabled assertions"
#endif

using namespace Core::Vr;
// Deterministic time avoids haptic refresh tests depending on scheduler delays.
struct Clock {
    using time_point = std::chrono::steady_clock::time_point;
    static inline time_point current{};
    static time_point now() { return current; }
};

template <typename... Args>
void IgnoreLog(const char*, Args&&...) {}
#define LOG_INFO(category, ...) IgnoreLog(__VA_ARGS__)

namespace Core::Vr::MoveInput {
std::vector<TouchButtons> seen;
u16 CapturingMapTouchButtons(const TouchButtons& value) {
    seen.push_back(value);
    return MapTouchButtons(value);
}
} // namespace Core::Vr::MoveInput

struct MockRuntime {
    bool enabled{true};
    int release_calls{};
    int update_calls{};
    int recenters{};
    std::array<MoveHostState, 2> samples{};
    std::array<MoveFeedback, 2> feedback{};

    static MockRuntime& Instance() {
        static MockRuntime runtime;
        return runtime;
    }
    bool IsMoveEnabled() const { return enabled; }
    void RequestRecenter() { ++recenters; }
    void ResetPadYaw() {}
    void ReleaseMoves() {
        ++release_calls;
        samples = {};
    }
    void UpdateMove(u32 hand, const MoveHostState& value) {
        samples[hand] = value;
        ++update_calls;
    }
    MoveFeedback GetMoveFeedback(u32 hand) { return feedback[hand]; }
};
#define Runtime MockRuntime

constexpr XrSpaceLocationFlags ValidPose =
    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
constexpr XrSpaceLocationFlags TrackedPose =
    XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
constexpr XrSpaceVelocityFlags ValidVelocity =
    XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;

struct XrMock {
    std::map<XrAction, bool> buttons;
    std::map<XrAction, float> floats;
    std::map<XrAction, XrVector2f> sticks;
    bool input_active{true};
    XrResult input_result{XR_SUCCESS};
    std::array<bool, 2> active{true, true};
    std::array<XrResult, 2> pose_result{XR_SUCCESS, XR_SUCCESS};
    std::array<XrResult, 2> locate_result{XR_SUCCESS, XR_SUCCESS};
    std::array<XrSpaceLocationFlags, 2> flags{ValidPose | TrackedPose, ValidPose | TrackedPose};
    std::array<XrSpaceVelocityFlags, 2> velocity_flags{ValidVelocity, ValidVelocity};
    std::array<int, 2> pose_queries{};
    std::array<int, 2> applies{};
    std::array<int, 2> stops{};
    std::array<float, 2> amplitudes{};
    XrResult haptic_result{XR_SUCCESS};
    XrResult sync_result{XR_SUCCESS};
    int sync_calls{};
} xr;

int HandOf(XrPath path) {
    assert(path == 10 || path == 20);
    return path == 10 ? 0 : 1;
}

template <typename Handle>
Handle FakeHandle(uintptr_t value) {
    return reinterpret_cast<Handle>(value);
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(
    XrSession, const XrActionStateGetInfo* get, XrActionStateBoolean* value) {
    value->isActive = xr.input_active;
    value->currentState = xr.buttons[get->action];
    return xr.input_result;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(
    XrSession, const XrActionStateGetInfo* get, XrActionStateFloat* value) {
    value->isActive = xr.input_active;
    value->currentState = xr.floats[get->action];
    return xr.input_result;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(
    XrSession, const XrActionStateGetInfo* get, XrActionStateVector2f* value) {
    value->isActive = xr.input_active;
    value->currentState = xr.sticks[get->action];
    return xr.input_result;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(
    XrSession, const XrActionStateGetInfo* get, XrActionStatePose* value) {
    const int hand = HandOf(get->subactionPath);
    ++xr.pose_queries[hand];
    value->isActive = xr.active[hand];
    return xr.pose_result[hand];
}

XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(
    XrSpace space, XrSpace, XrTime, XrSpaceLocation* location) {
    const auto hand = reinterpret_cast<uintptr_t>(space) - 30;
    assert(hand < 2);
    location->locationFlags = xr.flags[hand];
    location->pose.position = {static_cast<float>(hand + 1), 2.0f, 3.0f};
    // Runtime, not the host, owns normalization and coordinate transforms.
    location->pose.orientation = {0.0f, 0.0f, 0.0f, 2.0f};
    auto* velocity = static_cast<XrSpaceVelocity*>(location->next);
    velocity->velocityFlags = xr.velocity_flags[hand];
    velocity->linearVelocity = {4.0f, 5.0f, 6.0f};
    velocity->angularVelocity = {7.0f, 8.0f, 9.0f};
    return xr.locate_result[hand];
}

XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(
    XrSession, const XrHapticActionInfo* info, const XrHapticBaseHeader* base) {
    const int hand = HandOf(info->subactionPath);
    const auto* vibration = reinterpret_cast<const XrHapticVibration*>(base);
    assert(vibration->duration == 100'000'000);
    assert(vibration->frequency == XR_FREQUENCY_UNSPECIFIED);
    ++xr.applies[hand];
    xr.amplitudes[hand] = vibration->amplitude;
    return xr.haptic_result;
}

XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(
    XrSession, const XrHapticActionInfo* info) {
    ++xr.stops[HandOf(info->subactionPath)];
    return xr.haptic_result;
}

XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession, const XrActionsSyncInfo*) {
    ++xr.sync_calls;
    return xr.sync_result;
}

struct Host {
    bool actions_ready{true};
    bool session_running{true};
    bool actions_synced{};
    int gamepad_calls{};
    XrTime space_change_time{};
    XrSession session{FakeHandle<XrSession>(1)};
    XrActionSet action_set{FakeHandle<XrActionSet>(1)};
    XrSpace local_space{FakeHandle<XrSpace>(1)};
    XrPath hand_paths[2]{10, 20};
    XrSpace grip_spaces[2]{FakeHandle<XrSpace>(30), FakeHandle<XrSpace>(31)};
    XrAction act_move{FakeHandle<XrAction>(1)};
    XrAction act_finger{FakeHandle<XrAction>(2)};
    XrAction act_cross{FakeHandle<XrAction>(3)};
    XrAction act_square{FakeHandle<XrAction>(4)};
    XrAction act_circle{FakeHandle<XrAction>(5)};
    XrAction act_triangle{FakeHandle<XrAction>(6)};
    XrAction act_l1{FakeHandle<XrAction>(7)};
    XrAction act_r1{FakeHandle<XrAction>(8)};
    XrAction act_l2{FakeHandle<XrAction>(9)};
    XrAction act_r2{FakeHandle<XrAction>(10)};
    XrAction act_options{FakeHandle<XrAction>(11)};
    XrAction act_l3{FakeHandle<XrAction>(12)};
    XrAction act_finger_press{FakeHandle<XrAction>(13)};
    XrAction act_grip{FakeHandle<XrAction>(14)};
    XrAction act_rumble{FakeHandle<XrAction>(15)};
    std::array<bool, 2> moves_connected{};
    std::array<bool, 2> moves_tracked{};
    std::array<u8, 2> move_rumble_applied{};
    std::array<Clock::time_point, 2> move_rumble_time{};

    void ReleaseControllers(const char*) {}
    void ApplyRumble(bool) {}

// @HOST_MOVE_METHODS@
// @HOST_DISPATCH@
// @HOST_SPACE_CHANGE@
};

void Reset() {
    Clock::current = {};
    xr = {};
    MockRuntime::Instance() = {};
    MoveInput::seen.clear();
}

void TestHandRouting() {
    Reset();
    Host host;
    auto& runtime = MockRuntime::Instance();
    xr.buttons[host.act_circle] = true;
    xr.buttons[host.act_square] = true;
    xr.buttons[host.act_options] = true;
    xr.floats[host.act_l2] = 0.25f;
    xr.floats[host.act_r2] = 0.75f;
    xr.sticks[host.act_move] = {-0.4f, 0.6f};
    xr.sticks[host.act_finger] = {0.3f, -0.7f};
    host.UpdateControllers(42);
    const auto& left = runtime.samples[0];
    const auto& right = runtime.samples[1];
    assert(runtime.update_calls == 2 && host.gamepad_calls == 0 && host.actions_synced);
    assert(left.connected && right.connected && left.device.tracked && right.device.tracked);
    assert(left.device.pose.position.x == 1.0f && right.device.pose.position.x == 2.0f);
    assert(left.device.pose.orientation.w == 2.0f);
    assert(left.buttons == (MoveInput::Cross | MoveInput::Start));
    assert(right.buttons == MoveInput::Circle);
    assert(left.trigger == 0.25f && right.trigger == 0.75f);
    assert(MoveInput::seen[0].primary && !MoveInput::seen[0].secondary && MoveInput::seen[0].menu);
    assert(!MoveInput::seen[1].primary && MoveInput::seen[1].secondary && !MoveInput::seen[1].menu);
    assert(left.linear_velocity_valid && left.angular_velocity_valid);
    assert(right.device.angular_velocity.z == 9.0f);
    assert(left.sample_time_ns == 0 && right.sample_time_ns == 0);
    std::cout << "PASS independent hands, real mapping, raw pose/velocity and Move dispatch\n";
}

void TestPoseAvailability() {
    Reset();
    Host host;
    auto& runtime = MockRuntime::Instance();
    xr.flags[0] = ValidPose;
    xr.flags[1] = TrackedPose;
    xr.velocity_flags[0] = 0;
    host.UpdateMoves(43);
    assert(runtime.samples[0].connected && !runtime.samples[0].device.tracked);
    assert(!runtime.samples[0].linear_velocity_valid && !runtime.samples[0].angular_velocity_valid);
    assert(runtime.samples[1].connected && !runtime.samples[1].device.tracked);
    assert(runtime.samples[1].device.pose.orientation.w == 1.0f);

    xr.pose_result[0] = XR_ERROR_RUNTIME_FAILURE;
    xr.locate_result[1] = XR_ERROR_RUNTIME_FAILURE;
    host.UpdateMoves(44);
    assert(!runtime.samples[0].connected && runtime.samples[0].buttons == 0);
    assert(runtime.samples[0].trigger == 0.0f);
    assert(runtime.samples[1].connected && !runtime.samples[1].device.tracked);
    xr.pose_result[0] = XR_SUCCESS;
    xr.active[0] = false;
    host.UpdateMoves(45);
    assert(!runtime.samples[0].connected && runtime.samples[1].connected);

    host.grip_spaces[1] = XR_NULL_HANDLE;
    const int queries = xr.pose_queries[1];
    host.UpdateMoves(46);
    assert(!runtime.samples[1].connected && xr.pose_queries[1] == queries);
    std::cout << "PASS per-hand activity, missing space, pose/locate failure and tracking flags\n";
}

void TestInputSanitization() {
    Reset();
    Host host;
    auto& runtime = MockRuntime::Instance();
    xr.floats[host.act_l2] = std::numeric_limits<float>::quiet_NaN();
    xr.floats[host.act_r2] = 4.0f;
    xr.sticks[host.act_move] = {std::numeric_limits<float>::infinity(), 0.3f};
    xr.buttons[host.act_circle] = true;
    host.UpdateMoves(46);
    assert(runtime.samples[0].trigger == 0.0f && runtime.samples[1].trigger == 1.0f);
    assert(MoveInput::seen[0].stick_x == 0.0f && MoveInput::seen[0].stick_y == 0.0f);
    assert(runtime.samples[0].buttons == MoveInput::Cross);
    xr.input_active = false;
    host.UpdateMoves(47);
    assert(runtime.samples[0].connected && runtime.samples[0].buttons == 0);
    assert(runtime.samples[1].trigger == 0.0f);
    xr.input_active = true;
    xr.input_result = XR_ERROR_RUNTIME_FAILURE;
    host.UpdateMoves(48);
    assert(runtime.samples[0].buttons == 0 && runtime.samples[1].trigger == 0.0f);
    std::cout << "PASS analog finite checks/clamping and inactive/failed action reads\n";
}

void TestHapticRefreshAndStop() {
    Reset();
    Host host;
    auto& runtime = MockRuntime::Instance();
    host.UpdateMoves(46);
    runtime.feedback[0].intensity = 255;
    runtime.feedback[1].intensity = 64;
    host.ApplyMoveRumble(true);
    assert(xr.applies[0] == 1 && xr.applies[1] == 1);
    assert(xr.amplitudes[0] == 1.0f && xr.amplitudes[1] == 64.0f / 255.0f);
    host.ApplyMoveRumble(true);
    assert(xr.applies[0] == 1 && xr.applies[1] == 1);
    host.move_rumble_time[0] -= std::chrono::milliseconds{51};
    host.ApplyMoveRumble(true);
    assert(xr.applies[0] == 2 && xr.applies[1] == 1);
    xr.active[0] = false;
    host.UpdateMoves(47);
    assert(xr.stops[0] == 1 && xr.stops[1] == 0);
    runtime.feedback[1].intensity = 0;
    host.ApplyMoveRumble(true);
    assert(xr.stops[1] == 1);
    std::cout << "PASS independent haptic amplitude, refresh throttle, disconnect and zero stop\n";
}

void TestHapticFailureAndTrackingLoss() {
    Reset();
    Host host;
    auto& runtime = MockRuntime::Instance();
    host.UpdateMoves(46);
    runtime.feedback[1].intensity = 32;
    xr.haptic_result = XR_ERROR_RUNTIME_FAILURE;
    host.ApplyMoveRumble(true);
    assert(host.move_rumble_applied[1] == 0);
    const int attempts = xr.applies[1];
    xr.haptic_result = XR_SUCCESS;
    host.ApplyMoveRumble(true);
    assert(xr.applies[1] == attempts + 1 && host.move_rumble_applied[1] == 32);
    xr.flags[1] = ValidPose;
    host.UpdateMoves(47);
    assert(host.move_rumble_applied[1] == 0 && !host.moves_tracked[1]);
    assert(runtime.samples[1].connected && xr.stops[1] == 1);
    host.ReleaseMoveControllers("test");
    assert(!runtime.samples[0].connected && !runtime.samples[1].connected);
    assert(!host.moves_connected[0] && !host.moves_connected[1]);
    assert(!host.moves_tracked[0] && !host.moves_tracked[1]);
    std::cout << "PASS failed haptic retry, tracked-to-untracked stop and explicit release\n";
}

void TestDispatcherFailureAndDefault() {
    Reset();
    Host host;
    auto& runtime = MockRuntime::Instance();
    for (XrResult failure : {XR_SESSION_NOT_FOCUSED, XR_ERROR_RUNTIME_FAILURE}) {
        xr.sync_result = failure;
        const int releases = runtime.release_calls;
        host.UpdateControllers(48);
        assert(!host.actions_synced && runtime.release_calls == releases + 1);
        assert(host.gamepad_calls == 0);
    }
    xr.sync_result = XR_SUCCESS;
    host.session_running = false;
    const int syncs = xr.sync_calls;
    host.UpdateControllers(49);
    assert(xr.sync_calls == syncs && !host.actions_synced);
    host.session_running = true;
    host.actions_ready = false;
    host.UpdateControllers(50);
    assert(xr.sync_calls == syncs);
    host.actions_ready = true;
    runtime.enabled = false;
    host.UpdateControllers(51);
    assert(host.gamepad_calls == 1 && host.actions_synced);
    std::cout << "PASS sync failure/inactive session release and default gamepad dispatch\n";
}

void TestReferenceSpaceBoundary() {
    Reset();
    Host host;
    auto& runtime = MockRuntime::Instance();
    host.space_change_time = 100;
    host.CheckSpace(99);
    assert(runtime.release_calls == 0 && host.space_change_time == 100 && runtime.recenters == 0);
    host.CheckSpace(100);
    assert(runtime.release_calls == 1 && host.space_change_time == 0 && runtime.recenters == 1);
    host.CheckSpace(101);
    assert(runtime.release_calls == 1 && runtime.recenters == 1);
    runtime.enabled = false;
    host.space_change_time = 200;
    host.CheckSpace(200);
    assert(runtime.release_calls == 1 && runtime.recenters == 2);
    std::cout << "PASS reference-space release exactly at boundary; legacy recenter preserved\n";
}

int main() {
    TestHandRouting();
    TestPoseAvailability();
    TestInputSanitization();
    TestHapticRefreshAndStop();
    TestHapticFailureAndTrackingLoss();
    TestDispatcherFailureAndDefault();
    TestReferenceSpaceBoundary();
}
