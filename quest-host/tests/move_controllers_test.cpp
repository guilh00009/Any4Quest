// SPDX-License-Identifier: GPL-2.0-or-later
// Real OpenXR declarations, deterministic fake runtime. No headset is exercised.
#include "move_controllers.h"
#include "vr_move_input.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace Protocol = Core::Vr::Protocol;
namespace Buttons = Core::Vr::MoveInput;

namespace {
std::map<std::string, XrPath> paths;
std::map<std::string, XrAction> actions;
std::vector<std::pair<XrAction, XrPath>> bindings;
bool attached{};
XrResult sync_result{XR_SUCCESS};
std::array<bool, 2> connected{true, true};
std::array<XrSpaceLocationFlags, 2> flags{
    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
        XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT,
    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
        XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT};
std::array<bool, 2> finite_pose{true, true};
std::array<int, 2> applied{};
std::array<int, 2> stopped{};
std::array<float, 2> amplitudes{};
std::array<XrDuration, 2> durations{};
int destroyed_spaces{}, destroyed_sets{};

uint32_t Hand(XrPath path) {
    return path == paths.at("/user/hand/left") ? 0 : 1;
}

template <typename T> T Handle(uintptr_t value) {
    return reinterpret_cast<T>(value);
}
} // namespace

void HostLog(int, const char*, ...) {}

extern "C" {
XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance, const char* name, XrPath* result) {
    auto [it, inserted] = paths.try_emplace(name, paths.size() + 1);
    (void)inserted;
    *result = it->second;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance, const XrActionSetCreateInfo*,
                                               XrActionSet* result) {
    *result = Handle<XrActionSet>(1);
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet, const XrActionCreateInfo* info,
                                            XrAction* result) {
    assert(info->countSubactionPaths == 2);
    *result = actions[info->actionName] = Handle<XrAction>(actions.size() + 2);
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(
    XrInstance, const XrInteractionProfileSuggestedBinding* info) {
    assert(info->interactionProfile == paths.at("/interaction_profiles/oculus/touch_controller"));
    for (uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
        bindings.emplace_back(info->suggestedBindings[i].action,
                              info->suggestedBindings[i].binding);
    }
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo* info,
                                                 XrSpace* result) {
    assert(info->action == actions.at("grip"));
    assert(info->poseInActionSpace.orientation.w == 1.0f);
    *result = Handle<XrSpace>(Hand(info->subactionPath) + 1);
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(
    XrSession, const XrSessionActionSetsAttachInfo* info) {
    assert(info->countActionSets == 1);
    attached = true;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession, const XrActionsSyncInfo* info) {
    assert(attached && info->countActiveActionSets == 1);
    return sync_result;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(XrSession, const XrActionStateGetInfo* info,
                                                  XrActionStatePose* result) {
    result->isActive = connected[Hand(info->subactionPath)];
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace, XrTime time,
                                           XrSpaceLocation* result) {
    assert(time == 12345);
    const size_t hand = reinterpret_cast<uintptr_t>(space) - 1;
    result->locationFlags = flags[hand];
    result->pose.position = {hand == 0 ? -0.3f : 0.3f, 1.2f, -0.7f};
    result->pose.orientation = {0, 0, 0, finite_pose[hand] ? 1.0f :
                                                    std::numeric_limits<float>::quiet_NaN()};
    auto* velocity = static_cast<XrSpaceVelocity*>(result->next);
    velocity->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
    velocity->linearVelocity = {1, 2, 3};
    velocity->angularVelocity = {9, 9, 9}; // Without a valid bit this must not be sent.
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession, const XrActionStateGetInfo* info,
                                                     XrActionStateBoolean* result) {
    result->isActive = XR_TRUE;
    result->currentState = info->action == actions.at(Hand(info->subactionPath) == 0
                                                         ? "primary" : "secondary");
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession, const XrActionStateGetInfo* info,
                                                   XrActionStateFloat* result) {
    result->isActive = XR_TRUE;
    result->currentState = info->action == actions.at("trigger") ? 0.75f : 0.8f;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession, const XrActionStateGetInfo*,
                                                      XrActionStateVector2f* result) {
    result->isActive = XR_TRUE;
    result->currentState = {-0.9f, 0.9f};
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession, const XrHapticActionInfo* info,
                                                   const XrHapticBaseHeader* base) {
    const auto* vibration = reinterpret_cast<const XrHapticVibration*>(base);
    const auto hand = Hand(info->subactionPath);
    ++applied[hand];
    amplitudes[hand] = vibration->amplitude;
    durations[hand] = vibration->duration;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(XrSession, const XrHapticActionInfo* info) {
    ++stopped[Hand(info->subactionPath)];
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace) {
    ++destroyed_spaces;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet) {
    ++destroyed_sets;
    return XR_SUCCESS;
}
} // extern "C"

int main() {
    MoveControllers controllers;
    assert(controllers.Initialize(Handle<XrInstance>(1), Handle<XrSession>(1),
                                  Handle<XrSpace>(99)));
    assert(actions.size() == 9 && bindings.size() == 17 && attached);
    assert(!paths.contains("/user/hand/right/input/menu/click"));
    std::array<Protocol::MoveState, 2> state;
    std::array<std::optional<MoveControllers::Feedback>, 2> feedback;
    const auto send = [&](const Protocol::MoveState& message) { state.at(message.hand) = message; };
    const auto read = [&](uint32_t hand) { return feedback.at(hand); };
    const auto command = [&](uint32_t hand, uint8_t strength) {
        Protocol::MoveFeedback message;
        message.hand = hand;
        message.intensity = strength;
        feedback[hand] = MoveControllers::Feedback{message, MoveControllers::Clock::now()};
    };
    command(0, 255);
    command(1, 64);
    controllers.Update(12345, true, send, read);
    assert(state[0].position[0] == -0.3f && state[1].position[0] == 0.3f);
    assert(state[0].buttons == (Buttons::Cross | Buttons::Move | Buttons::Square | Buttons::Triangle));
    assert(state[1].buttons == (Buttons::Circle | Buttons::Move | Buttons::Square | Buttons::Triangle));
    assert(state[0].trigger == 0.75f);
    assert(state[0].sample_time_ns > 0);
    assert(state[0].flags == (Protocol::MoveState::Connected | Protocol::MoveState::Tracked |
                              Protocol::MoveState::LinearVelocityValid));
    assert(state[0].linear_velocity[2] == 3.0f && state[0].angular_velocity[2] == 0.0f);
    assert(applied[0] == 1 && applied[1] == 1);
    assert(amplitudes[0] == 1.0f && std::abs(amplitudes[1] - 64.0f / 255.0f) < 1e-6f);
    assert(durations[0] > 0 && durations[0] <= 1'000'000'000);
    controllers.Update(12345, true, send, read);
    assert(applied[0] == 1); // An unchanged received message is not restarted each frame.

    connected[0] = false;
    controllers.Update(12345, true, send, read);
    assert(state[0].flags == 0 && state[0].buttons == 0 && state[0].trigger == 0);
    assert(state[1].flags & Protocol::MoveState::Tracked);
    assert(stopped[0] == 1 && stopped[1] == 0);
    connected[0] = true;
    controllers.Update(12345, true, send, read);
    assert(applied[0] == 1); // Do not replay feedback from before disconnect.

    command(0, 127);
    controllers.Update(12345, true, send, read);
    assert(applied[0] == 2);
    flags[0] &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    controllers.Update(12345, true, send, read);
    assert(state[0].flags == Protocol::MoveState::Connected);
    assert((state[0].buttons & Buttons::Cross) != 0 && state[0].trigger == 0.75f);
    assert(state[0].position[0] == 0 && stopped[0] == 2);
    flags[0] |= XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    finite_pose[0] = false;
    controllers.Update(12345, true, send, read);
    assert(state[0].flags == Protocol::MoveState::Connected);
    finite_pose[0] = true;

    controllers.Update(12345, false, send, read);
    assert(state[0].flags == 0 && state[1].flags == 0 && stopped[1] == 1);
    command(0, 255);
    sync_result = XR_SESSION_NOT_FOCUSED; // This is positive; XR_FAILED alone is insufficient.
    controllers.Update(12345, true, send, read);
    assert(state[0].flags == 0 && applied[0] == 2);
    sync_result = XR_SUCCESS;

    command(0, 255);
    feedback[0]->received -= std::chrono::seconds{2};
    controllers.Update(12345, true, send, read);
    assert(applied[0] == 2); // Expired feedback cannot vibrate.
    command(0, 255);
    controllers.Update(12345, true, send, read);
    assert(applied[0] == 3);
    command(0, 0);
    controllers.Update(12345, true, send, read);
    assert(stopped[0] == 3);
    command(0, 255);
    controllers.Update(12345, true, send, read);
    feedback[0].reset(); // The socket closed.
    controllers.Update(12345, true, send, read);
    assert(stopped[0] == 4);
    controllers.Release(send);
    controllers.Destroy();
    controllers.Destroy(); // Partial/final teardown is safe to repeat.
    assert(destroyed_spaces == 2 && destroyed_sets == 1);
    puts("Quest mocked OpenXR Move actions: passed (poses, buttons, tracking/focus loss, haptics)");
}
