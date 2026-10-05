// SPDX-License-Identifier: GPL-2.0-or-later

#include "move_controllers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include "log.h"
#include "vr_move_input.h"

namespace Protocol = Core::Vr::Protocol;

namespace {

bool Check(XrResult result, const char* operation) {
    if (XR_FAILED(result)) {
        LOGE("Move %s failed: %d", operation, static_cast<int>(result));
        return false;
    }
    return true;
}

bool Finite(const XrVector3f& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool Finite(const XrPosef& pose) {
    const auto& q = pose.orientation;
    const double length = double{q.x} * q.x + double{q.y} * q.y + double{q.z} * q.z +
                          double{q.w} * q.w;
    return Finite(pose.position) && std::isfinite(length) && length > 1e-12;
}

} // namespace

bool MoveControllers::Initialize(XrInstance instance, XrSession session_, XrSpace base_space_) {
    session = session_;
    base_space = base_space_;
    const auto path = [&](const char* name, XrPath& value) {
        return Check(xrStringToPath(instance, name, &value), name);
    };
    if (!path("/user/hand/left", hands[0]) || !path("/user/hand/right", hands[1])) {
        return false;
    }
    XrActionSetCreateInfo info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(info.actionSetName, sizeof(info.actionSetName), "ps_move");
    std::snprintf(info.localizedActionSetName, sizeof(info.localizedActionSetName), "PS Move");
    if (!Check(xrCreateActionSet(instance, &info, &action_set), "xrCreateActionSet")) {
        return false;
    }
    const auto action = [&](XrAction& output, const char* name, const char* label,
                            XrActionType type) {
        XrActionCreateInfo create{XR_TYPE_ACTION_CREATE_INFO};
        std::snprintf(create.actionName, sizeof(create.actionName), "%s", name);
        std::snprintf(create.localizedActionName, sizeof(create.localizedActionName), "%s", label);
        create.actionType = type;
        create.countSubactionPaths = static_cast<uint32_t>(hands.size());
        create.subactionPaths = hands.data();
        return Check(xrCreateAction(action_set, &create, &output), name);
    };
    if (!action(grip, "grip", "Grip pose", XR_ACTION_TYPE_POSE_INPUT) ||
        !action(trigger, "trigger", "Move trigger", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !action(squeeze, "squeeze", "Move button", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !action(stick, "stick", "Extra face buttons", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
        !action(stick_click, "stick_click", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !action(primary, "primary", "Cross", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !action(secondary, "secondary", "Circle", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !action(menu, "menu", "Start", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !action(haptic, "haptic", "Move vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT)) {
        return false;
    }

    std::vector<XrActionSuggestedBinding> bindings;
    const auto bind = [&](XrAction input, const char* name) {
        XrPath binding{};
        if (!path(name, binding)) {
            return false;
        }
        bindings.push_back({input, binding});
        return true;
    };
    for (const char* hand : {"left", "right"}) {
        for (const auto& entry : {
                 std::pair{grip, "input/grip/pose"}, {trigger, "input/trigger/value"},
                 {squeeze, "input/squeeze/value"}, {stick, "input/thumbstick"},
                 {stick_click, "input/thumbstick/click"}, {haptic, "output/haptic"}}) {
            char name[128];
            std::snprintf(name, sizeof(name), "/user/hand/%s/%s", hand, entry.second);
            if (!bind(entry.first, name)) {
                return false;
            }
        }
    }
    // The right system button is reserved by the runtime. Only the left menu is available.
    if (!bind(primary, "/user/hand/left/input/x/click") ||
        !bind(primary, "/user/hand/right/input/a/click") ||
        !bind(secondary, "/user/hand/left/input/y/click") ||
        !bind(secondary, "/user/hand/right/input/b/click") ||
        !bind(menu, "/user/hand/left/input/menu/click")) {
        return false;
    }
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    if (!path("/interaction_profiles/oculus/touch_controller", suggested.interactionProfile)) {
        return false;
    }
    suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggested.suggestedBindings = bindings.data();
    if (!Check(xrSuggestInteractionProfileBindings(instance, &suggested),
               "xrSuggestInteractionProfileBindings")) {
        return false;
    }
    for (uint32_t hand = 0; hand < hands.size(); ++hand) {
        XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space.action = grip;
        space.subactionPath = hands[hand];
        space.poseInActionSpace.orientation.w = 1.0f;
        if (!Check(xrCreateActionSpace(session, &space, &grips[hand]), "xrCreateActionSpace")) {
            return false;
        }
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &action_set;
    if (!Check(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets")) {
        return false;
    }
    LOGI("Touch actions attached: left and right PS Move enabled");
    return true;
}

void MoveControllers::Update(XrTime time, bool focused, const SendState& send,
                             const ReadFeedback& feedback) {
    if (!focused || action_set == XR_NULL_HANDLE) {
        Release(send);
        return;
    }
    XrActiveActionSet active{action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    // XR_SESSION_NOT_FOCUSED is a positive result, but its action states must not be used.
    if (xrSyncActions(session, &sync) != XR_SUCCESS) {
        Release(send);
        return;
    }
    const auto now = Clock::now();
    for (uint32_t hand = 0; hand < hands.size(); ++hand) {
        Protocol::MoveState message;
        message.hand = hand;
        message.sample_time_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.subactionPath = hands[hand];
        get.action = grip;
        XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};
        if (XR_FAILED(xrGetActionStatePose(session, &get, &pose_state)) ||
            pose_state.isActive != XR_TRUE) {
            StopHand(hand, now);
            send(message);
            continue;
        }
        message.flags = Protocol::MoveState::Connected;
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        location.next = &velocity;
        constexpr XrSpaceLocationFlags tracked = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                                XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
                                                XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        const bool is_tracked =
            XR_SUCCEEDED(xrLocateSpace(grips[hand], base_space, time, &location)) &&
            (location.locationFlags & tracked) == tracked && Finite(location.pose);
        if (!is_tracked) {
            // Connected does not imply tracked. Never label an old or inferred pose valid.
            StopHand(hand, now);
            // Buttons remain usable during an optical occlusion of a connected controller.
        } else {
            message.flags |= Protocol::MoveState::Tracked;
            message.position[0] = location.pose.position.x;
            message.position[1] = location.pose.position.y;
            message.position[2] = location.pose.position.z;
            message.orientation[0] = location.pose.orientation.x;
            message.orientation[1] = location.pose.orientation.y;
            message.orientation[2] = location.pose.orientation.z;
            message.orientation[3] = location.pose.orientation.w;
            if ((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0 &&
                Finite(velocity.linearVelocity)) {
                message.flags |= Protocol::MoveState::LinearVelocityValid;
                message.linear_velocity[0] = velocity.linearVelocity.x;
                message.linear_velocity[1] = velocity.linearVelocity.y;
                message.linear_velocity[2] = velocity.linearVelocity.z;
            }
            if ((velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0 &&
                Finite(velocity.angularVelocity)) {
                message.flags |= Protocol::MoveState::AngularVelocityValid;
                message.angular_velocity[0] = velocity.angularVelocity.x;
                message.angular_velocity[1] = velocity.angularVelocity.y;
                message.angular_velocity[2] = velocity.angularVelocity.z;
            }
        }
        const auto boolean = [&](XrAction action) {
            get.action = action;
            XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
            return XR_SUCCEEDED(xrGetActionStateBoolean(session, &get, &state)) &&
                   state.isActive == XR_TRUE && state.currentState == XR_TRUE;
        };
        const auto scalar = [&](XrAction action) {
            get.action = action;
            XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
            return XR_SUCCEEDED(xrGetActionStateFloat(session, &get, &state)) &&
                           state.isActive == XR_TRUE && std::isfinite(state.currentState)
                       ? std::clamp(state.currentState, 0.0f, 1.0f)
                       : 0.0f;
        };
        Core::Vr::MoveInput::TouchButtons buttons;
        buttons.primary = boolean(primary);
        buttons.secondary = boolean(secondary);
        buttons.stick_click = boolean(stick_click);
        buttons.menu = boolean(menu);
        buttons.squeeze = scalar(squeeze);
        get.action = stick;
        XrActionStateVector2f stick_state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &get, &stick_state)) &&
            stick_state.isActive == XR_TRUE && std::isfinite(stick_state.currentState.x) &&
            std::isfinite(stick_state.currentState.y)) {
            buttons.stick_x = stick_state.currentState.x;
            buttons.stick_y = stick_state.currentState.y;
        }
        message.buttons = Core::Vr::MoveInput::MapTouchButtons(buttons);
        message.trigger = scalar(trigger);
        send(message);
        if (is_tracked) {
            ApplyFeedback(hand, feedback, now);
        }
    }
}

void MoveControllers::StopHand(uint32_t hand, Clock::time_point now) {
    if (vibrating[hand] && session != XR_NULL_HANDLE && haptic != XR_NULL_HANDLE) {
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = haptic;
        info.subactionPath = hands[hand];
        xrStopHapticFeedback(session, &info);
    }
    vibrating[hand] = false;
    haptic_until[hand] = {};
    // Never replay a command received before a disconnect, tracking loss, or loss of focus.
    feedback_after[hand] = now;
}

void MoveControllers::ApplyFeedback(uint32_t hand, const ReadFeedback& feedback,
                                    Clock::time_point now) {
    if (vibrating[hand] && now >= haptic_until[hand]) {
        StopHand(hand, now);
    }
    const auto sample = feedback(hand);
    if (!sample) {
        StopHand(hand, now);
        return;
    }
    if (sample->received <= feedback_after[hand] ||
        sample->received <= feedback_seen[hand]) {
        return;
    }
    feedback_seen[hand] = sample->received;
    constexpr auto lifetime = std::chrono::seconds{1};
    if (sample->message.intensity == 0 || now - sample->received >= lifetime) {
        StopHand(hand, now);
        return;
    }
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = haptic;
    info.subactionPath = hands[hand];
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = static_cast<float>(sample->message.intensity) / 255.0f;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    vibration.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             sample->received + lifetime - now).count();
    if (XR_SUCCEEDED(xrApplyHapticFeedback(
            session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration)))) {
        vibrating[hand] = true;
        haptic_until[hand] = sample->received + lifetime;
    }
    // RGB is retained in the transport state. Touch has no application-controlled RGB orb.
}

void MoveControllers::Release(const SendState& send) {
    const auto now = Clock::now();
    for (uint32_t hand = 0; hand < hands.size(); ++hand) {
        Protocol::MoveState message;
        message.hand = hand;
        message.sample_time_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
        send(message);
        StopHand(hand, now);
    }
}

void MoveControllers::Destroy() {
    const auto now = Clock::now();
    for (uint32_t hand = 0; hand < hands.size(); ++hand) {
        StopHand(hand, now);
        if (grips[hand] != XR_NULL_HANDLE) {
            xrDestroySpace(grips[hand]);
            grips[hand] = XR_NULL_HANDLE;
        }
    }
    if (action_set != XR_NULL_HANDLE) {
        xrDestroyActionSet(action_set);
        action_set = XR_NULL_HANDLE;
    }
    session = XR_NULL_HANDLE;
    haptic = XR_NULL_HANDLE;
}
