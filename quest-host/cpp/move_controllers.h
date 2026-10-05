// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <optional>

#include <openxr/openxr.h>

#include "vr_protocol.h"

/// Touch controllers are real OpenXR actions, separate from the optional hand tracking
/// used to estimate a gamepad position. Every method runs on the XR session thread.
class MoveControllers {
public:
    using Clock = std::chrono::steady_clock;
    struct Feedback {
        Core::Vr::Protocol::MoveFeedback message;
        Clock::time_point received;
    };
    using SendState = std::function<void(const Core::Vr::Protocol::MoveState&)>;
    using ReadFeedback = std::function<std::optional<Feedback>(uint32_t)>;

    bool Initialize(XrInstance instance, XrSession session, XrSpace base_space);
    void Update(XrTime time, bool focused, const SendState& send,
                const ReadFeedback& feedback);
    void Release(const SendState& send);
    void Destroy();

private:
    void StopHand(uint32_t hand, Clock::time_point now);
    void ApplyFeedback(uint32_t hand, const ReadFeedback& feedback, Clock::time_point now);

    XrSession session{XR_NULL_HANDLE};
    XrSpace base_space{XR_NULL_HANDLE};
    XrActionSet action_set{XR_NULL_HANDLE};
    std::array<XrPath, 2> hands{};
    std::array<XrSpace, 2> grips{};
    XrAction grip{XR_NULL_HANDLE};
    XrAction trigger{XR_NULL_HANDLE};
    XrAction squeeze{XR_NULL_HANDLE};
    XrAction stick{XR_NULL_HANDLE};
    XrAction stick_click{XR_NULL_HANDLE};
    XrAction primary{XR_NULL_HANDLE};
    XrAction secondary{XR_NULL_HANDLE};
    XrAction menu{XR_NULL_HANDLE};
    XrAction haptic{XR_NULL_HANDLE};
    std::array<Clock::time_point, 2> feedback_after{};
    std::array<Clock::time_point, 2> feedback_seen{};
    std::array<Clock::time_point, 2> haptic_until{};
    std::array<bool, 2> vibrating{};
};
