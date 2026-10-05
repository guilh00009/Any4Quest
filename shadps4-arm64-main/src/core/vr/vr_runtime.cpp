// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/libraries/system/systemservice.h"
#include "core/libraries/kernel/time.h"
#include "core/vr/vr_move_input.h"
#include "core/vr/vr_host_link.h"
#include "core/vr/vr_runtime.h"
#include "core/vr/move_capture.h"
#ifdef ENABLE_OPENXR_HOST
#include "core/vr/openxr_host.h"
#endif

namespace Core::Vr {

Quat Multiply(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat Conjugate(const Quat& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

Quat Normalize(const Quat& q) {
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length < 1e-6f) {
        return {};
    }
    return {q.x / length, q.y / length, q.z / length, q.w / length};
}

Vec3 Rotate(const Quat& q, const Vec3& v) {
    // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t{
        u.y * v.z - u.z * v.y + q.w * v.x,
        u.z * v.x - u.x * v.z + q.w * v.y,
        u.x * v.y - u.y * v.x + q.w * v.z,
    };
    return {
        v.x + 2.0f * (u.y * t.z - u.z * t.y),
        v.y + 2.0f * (u.z * t.x - u.x * t.z),
        v.z + 2.0f * (u.x * t.y - u.y * t.x),
    };
}

Quat FromYawPitch(float yaw, float pitch) {
    const Quat qy{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    const Quat qp{std::sin(pitch * 0.5f), 0.0f, 0.0f, std::cos(pitch * 0.5f)};
    return Multiply(qy, qp);
}

Quat FromYawPitchRoll(float yaw, float pitch, float roll) {
    const Quat qr{0.0f, 0.0f, std::sin(roll * 0.5f), std::cos(roll * 0.5f)};
    return Multiply(FromYawPitch(yaw, pitch), qr);
}

Quat YawOnly(const Quat& q) {
    // Which way it faces along the ground. Something that looks steeply up or down hardly
    // faces any way; its right-hand side still points somewhere along the ground then.
    const Vec3 forward = Rotate(q, {0.0f, 0.0f, -1.0f});
    float yaw = std::atan2(-forward.x, -forward.z);
    if (forward.x * forward.x + forward.z * forward.z < 0.25f) {
        const Vec3 right = Rotate(q, {1.0f, 0.0f, 0.0f});
        yaw = std::atan2(-right.z, right.x);
    }
    return {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
}

namespace {

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float Length(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

/// The rotation about `axis` (unit length) by `angle`.
Quat FromAxisAngle(const Vec3& axis, float angle) {
    const float s = std::sin(angle * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f)};
}

enum class HeadsetMode { Auto, On, Off };

struct FileConfig {
    HeadsetMode mode{HeadsetMode::Auto};
    Config config;
};

bool EnvFlag(const char* name, bool& out) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return false;
    }
    out = value[0] != '0';
    return true;
}

FileConfig LoadFileConfig() {
    FileConfig result;
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "vr.json";
    std::ifstream file{path};
    if (file) {
        const auto json = nlohmann::json::parse(file, nullptr, false);
        if (json.is_discarded()) {
            LOG_ERROR(Core_Vr, "Ignoring malformed {}", path.string());
        } else {
            const std::string mode = json.value("headset", "auto");
            result.mode = mode == "on"    ? HeadsetMode::On
                          : mode == "off" ? HeadsetMode::Off
                                          : HeadsetMode::Auto;
            auto& config = result.config;
            config.ipd = json.value("ipd_mm", config.ipd * 1000.0f) / 1000.0f;
            config.refresh_rate = json.value("refresh_rate", config.refresh_rate);
            config.demo_motion = json.value("demo_motion", config.demo_motion);
            const std::string input_mode = json.value("input_mode", "gamepad");
            config.move_enabled = input_mode == "move";
            if (input_mode != "gamepad" && input_mode != "move") {
                LOG_WARNING(Core_Vr, "Unknown VR input mode '{}'; using gamepad", input_mode);
            }
            if (const auto it = json.find("origin_offset");
                it != json.end() && it->is_array() && it->size() == 3) {
                config.origin_offset = {(*it)[0].get<float>(), (*it)[1].get<float>(),
                                        (*it)[2].get<float>()};
            }
            if (const auto it = json.find("pad_offset");
                it != json.end() && it->is_array() && it->size() == 3) {
                config.pad_offset = {(*it)[0].get<float>(), (*it)[1].get<float>(),
                                     (*it)[2].get<float>()};
            }
            config.pad_follows_head = json.value("pad_anchor", "head") != "seat";
            if (const auto it = json.find("fov_tan");
                it != json.end() && it->is_array() && it->size() == 4) {
                config.fov = {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>(),
                              (*it)[3].get<float>()};
            }
        }
    }

    if (const char* input = std::getenv("SHADPS4_VR_INPUT_MODE"); input && *input) {
        const std::string_view mode{input};
        result.config.move_enabled = mode == "move";
        if (mode != "gamepad" && mode != "move") {
            LOG_WARNING(Core_Vr, "Unknown SHADPS4_VR_INPUT_MODE '{}'; using gamepad", input);
        }
    }
    bool flag = false;
    if (EnvFlag("SHADPS4_VR", flag)) {
        result.mode = flag ? HeadsetMode::On : HeadsetMode::Off;
    }
    if (EnvFlag("SHADPS4_VR_DEMO", flag)) {
        result.config.demo_motion = flag;
    }
    if (const char* anchor = std::getenv("SHADPS4_VR_PAD_ANCHOR"); anchor != nullptr) {
        result.config.pad_follows_head = std::string_view{anchor} != "seat";
    }
    // A host whose display refreshes at another rate than the headset the title was made for
    // says so: the title then sees refreshes come at that rate, and has its frames follow them
    // the way it would have with the real thing (it takes two refreshes for one frame).
    if (const char* rate = std::getenv("SHADPS4_VR_REFRESH_RATE"); rate != nullptr) {
        result.config.refresh_rate = static_cast<u32>(std::atoi(rate));
    }
    if (result.config.refresh_rate < 60 || result.config.refresh_rate > 120) {
        result.config.refresh_rate = 120;
    }
    // SHADPS4_VR_FOV=<percent>: how much of a PlayStation VR's field of view (100 by 103
    // degrees an eye) the title is told its headset has. It draws that much into the same
    // picture, and the headset shows it over that much: fewer degrees for the same pixels is
    // a sharper picture, with a border around it where the rest would have been.
    // SHADPS4_VR_FOV_OF=headset: the percent is of what the host's headset shows (its whole
    // view at 100), not of a PlayStation VR's.
    if (const char* of = std::getenv("SHADPS4_VR_FOV_OF"); of != nullptr) {
        result.config.fov_from_headset = std::string_view{of} == "headset";
    }
    if (const char* fov = std::getenv("SHADPS4_VR_FOV"); fov != nullptr && *fov != '\0') {
        result.config.fov_scale =
            std::clamp(static_cast<float>(std::atof(fov)) / 100.0f, 0.5f, 1.2f);
    }
    const float scale = result.config.fov_scale;
    result.config.fov.tan_out *= scale;
    result.config.fov.tan_in *= scale;
    result.config.fov.tan_top *= scale;
    result.config.fov.tan_bottom *= scale;
    return result;
}

} // namespace

Runtime::Runtime() = default;

Runtime& Runtime::Instance() {
    static Runtime instance;
    return instance;
}

void Runtime::Configure(bool psvr_supported, bool psvr_required) {
    const FileConfig file_config = LoadFileConfig();
    ReleaseMoves();
    config = file_config.config;
    move_enabled.store(config.move_enabled, std::memory_order_relaxed);
    LOG_INFO(Core_Vr, "VR input profile: {}", config.move_enabled ? "two Move controllers (experimental)"
                                                               : "gamepad (default)");
    switch (file_config.mode) {
    case HeadsetMode::On:
        config.headset_connected = true;
        break;
    case HeadsetMode::Off:
        config.headset_connected = false;
        break;
    case HeadsetMode::Auto:
        // Titles that merely support PSVR boot fine flat, so only plug the headset in by
        // default when the title cannot run without one.
        config.headset_connected = psvr_required;
        break;
    }
    // An application driving a real headset on the other end of SHADPS4_VR_SOCKET is as
    // connected as a headset gets.
    if (file_config.mode != HeadsetMode::Off && HostLink::Instance().Start()) {
        config.headset_connected = true;
    }
#ifdef ENABLE_OPENXR_HOST
    // So is the headset of the machine itself, for a title that can do something with one. A
    // title that can do without only gets it if it is there already: one that comes later
    // would change what the title is in the middle of it.
    else if (file_config.mode != HeadsetMode::Off && (psvr_supported || psvr_required) &&
             OpenXrHost::Instance().Connect() &&
             (config.headset_connected || OpenXrHost::Instance().HasHeadset())) {
        config.headset_connected = true;
    }
#endif
    LOG_INFO(Core_Vr,
             "Virtual headset {} (title supports PSVR: {}, requires PSVR: {}), refresh {} Hz, "
             "ipd {:.1f} mm, demo motion {}",
             config.headset_connected ? "connected" : "not connected", psvr_supported,
             psvr_required, config.refresh_rate, config.ipd * 1000.0f, config.demo_motion);
}

Vec3 Runtime::PositionToTracker(const Vec3& host) const {
    const Vec3 seated = Rotate(Conjugate(seat_yaw), {host.x - seat_position.x,
                                                     host.y - seat_position.y,
                                                     host.z - seat_position.z});
    return {seated.x + config.origin_offset.x, seated.y + config.origin_offset.y,
            seated.z + config.origin_offset.z};
}

Vec3 Runtime::DirectionToTracker(const Vec3& host) const {
    return Rotate(Conjugate(seat_yaw), host);
}

void Runtime::PlaceHead() {
    const u64 sequence = head.sequence + 1;
    head = host_head;
    head.pose.position = PositionToTracker(host_head.pose.position);
    head.pose.orientation = Normalize(Multiply(Conjugate(seat_yaw), host_head.pose.orientation));
    head.linear_velocity = DirectionToTracker(host_head.linear_velocity);
    head.angular_velocity = DirectionToTracker(host_head.angular_velocity);
    head.sequence = sequence;
    head.tracked = true;
}

void Runtime::RecenterSeatLocked() {
    if (!host_head.tracked) {
        return;
    }
    const Quat yaw = YawOnly(host_head.pose.orientation);
    if (seat_valid && pad_attitude_valid) {
        // The controller keeps pointing where it points: its heading is counted from the new
        // straight ahead.
        const Quat turned = Multiply(Conjugate(yaw), seat_yaw);
        pad_attitude = Normalize(Multiply(turned, pad_attitude));
        pad.pose.orientation = pad_attitude;
    }
    previous_seat_position = seat_position;
    previous_seat_yaw = seat_yaw;
    seat_changed = std::chrono::steady_clock::now();
    if (Diagnostics::Capture::Enabled()) ++Diagnostics::seat_generation;
    seat_position = host_head.pose.position;
    seat_yaw = yaw;
    seat_valid = true;
    PlaceHead();
    // Where the controller was seen is of the old seat; the host says where it is every frame.
    pad_seen = false;
    pad_seen_offset_valid = false;
    pad_anchor_valid = false;
    pad_shown_valid = false;
    pad_yaw_reference_valid = false;
    LOG_INFO(Core_Vr,
             "Seat recentred: the head rests at {:.2f} {:.2f} {:.2f} of the headset's space, "
             "facing {:.0f} degrees to the left of its straight ahead",
             seat_position.x, seat_position.y, seat_position.z,
             2.0f * std::atan2(seat_yaw.y, seat_yaw.w) * 57.29578f);
}

void Runtime::RecenterSeat() {
    std::scoped_lock lock{mutex};
    RecenterSeatLocked();
}

void Runtime::FixSeat() {
    std::scoped_lock lock{mutex};
    if (Diagnostics::Capture::Enabled()) ++Diagnostics::seat_generation;
    seat_position = {};
    seat_yaw = {};
    seat_valid = true;
    title_asked = true;
}

void Runtime::RequestRecenter() {
    RecenterSeat();
    // What the console sends a title when the player asks for the view to be reset: the
    // title then counts positions from where the head is now.
    Libraries::SystemService::OrbisSystemServiceEvent event{};
    event.event_type = Libraries::SystemService::OrbisSystemServiceEventType::ResetVrPosition;
    Libraries::SystemService::PushSystemServiceEvent(event);
}

void Runtime::UpdateHead(const DeviceState& host_state) {
    std::scoped_lock lock{mutex};
    // How fast the head moves, by where it is from one time to the next: for a host that does
    // not say. A title takes a head that pushes into something for a push only when it comes
    // at some speed.
    const auto now = std::chrono::steady_clock::now();
    if (head_seen) {
        const float elapsed = std::chrono::duration<float>(now - head_seen_time).count();
        if (elapsed > 0.002f && elapsed < 0.1f) {
            static constexpr float Blend = 0.5f;
            const Vec3& at = host_state.pose.position;
            head_speed.x += ((at.x - head_seen_position.x) / elapsed - head_speed.x) * Blend;
            head_speed.y += ((at.y - head_seen_position.y) / elapsed - head_speed.y) * Blend;
            head_speed.z += ((at.z - head_seen_position.z) / elapsed - head_speed.z) * Blend;
        } else if (elapsed >= 0.1f) {
            head_speed = {};
        }
    }
    if (!head_seen || now - head_seen_time > std::chrono::milliseconds{2}) {
        head_seen_position = host_state.pose.position;
        head_seen_time = now;
        head_seen = true;
    }
    host_head = host_state;
    if (Length(host_state.linear_velocity) == 0.0f) {
        host_head.linear_velocity = head_speed;
    }
    host_head.tracked = true;
    if (!seat_valid) {
        RecenterSeatLocked();
        return;
    }
    PlaceHead();
}

void Runtime::UpdatePad(const DeviceState& host_state) {
    std::scoped_lock lock{mutex};
    const u64 sequence = pad.sequence + 1;
    pad = host_state;
    pad.pose.position = PositionToTracker(host_state.pose.position);
    pad.pose.orientation = Normalize(Multiply(Conjugate(seat_yaw), host_state.pose.orientation));
    pad.linear_velocity = DirectionToTracker(host_state.linear_velocity);
    pad.angular_velocity = DirectionToTracker(host_state.angular_velocity);
    pad.sequence = sequence;
    pad.tracked = true;
    pad_position_tracked = true;
}

void Runtime::ReleasePad() {
    std::scoped_lock lock{mutex};
    if (!pad_position_tracked) {
        return;
    }
    pad_position_tracked = false;
    // From where it was, it glides to where it is assumed to be; it points the way its own
    // sensors say, or straight ahead if it has none.
    pad_shown_position = pad.pose.position;
    pad_shown_valid = true;
    pad_shown_time = std::chrono::steady_clock::now();
    pad.pose.orientation = pad_attitude_valid ? pad_attitude : Quat{};
    pad.linear_velocity = {};
    pad.angular_velocity = {};
    ++pad.sequence;
}

void Runtime::UpdatePadOrientation(const Quat& orientation, const Vec3& angular_velocity) {
    std::scoped_lock lock{mutex};
    pad.pose.orientation = orientation;
    pad.angular_velocity = angular_velocity;
    ++pad.sequence;
    pad.tracked = true;
    pad_position_tracked = false;
}

void Runtime::UpdatePadAcceleration(const Vec3& acceleration) {
    std::scoped_lock lock{mutex};
    pad_acceleration = acceleration;
    pad_acceleration_valid = true;
}

void Runtime::UpdatePadGyro(const Vec3& angular_velocity) {
    static constexpr float Gravity = 9.80665f;
    // How fast the estimate leans towards what the accelerometer calls "up", per second.
    static constexpr float TiltGain = 2.0f;
    static constexpr Vec3 Up{0.0f, 1.0f, 0.0f};

    std::scoped_lock lock{mutex};
    const auto now = std::chrono::steady_clock::now();
    float elapsed = 0.0f;
    if (pad_attitude_valid) {
        elapsed = std::clamp(std::chrono::duration<float>(now - pad_motion_time).count(), 0.0f,
                             0.05f);
    }
    pad_motion_time = now;

    const float gravity = pad_acceleration_valid ? Length(pad_acceleration) : 0.0f;
    // Only trust the accelerometer for the direction of gravity while the controller is held
    // reasonably still.
    const bool steady = gravity > 0.8f * Gravity && gravity < 1.2f * Gravity;
    const Vec3 measured_up = steady ? Vec3{pad_acceleration.x / gravity,
                                           pad_acceleration.y / gravity,
                                           pad_acceleration.z / gravity}
                                    : Up;

    if (!pad_attitude_valid) {
        if (!steady) {
            return;
        }
        // Start level with the horizon and facing straight ahead.
        const Vec3 axis = Cross(measured_up, Up);
        const float sine = Length(axis);
        pad_attitude = sine > 1e-6f ? FromAxisAngle({axis.x / sine, axis.y / sine, axis.z / sine},
                                                    std::atan2(sine, measured_up.y))
                                    : Quat{};
        pad_attitude_valid = true;
    } else {
        // The gyroscope reports the turn in the controller's own frame.
        const float rate = Length(angular_velocity);
        if (rate > 1e-6f) {
            const Vec3 axis{angular_velocity.x / rate, angular_velocity.y / rate,
                            angular_velocity.z / rate};
            pad_attitude = Multiply(pad_attitude, FromAxisAngle(axis, rate * elapsed));
        }
        if (steady) {
            // Its drift in pitch and roll is taken out with gravity; heading has no reference.
            const Vec3 correction = Cross(Rotate(pad_attitude, measured_up), Up);
            const float error = Length(correction);
            if (error > 1e-6f) {
                const Vec3 axis{correction.x / error, correction.y / error, correction.z / error};
                pad_attitude = Multiply(
                    FromAxisAngle(axis, std::asin(std::min(error, 1.0f)) * TiltGain * elapsed),
                    pad_attitude);
            }
        }
        // Heading has no reference in the sensors. When the host can see which way the
        // controller points, lean towards that; not while it points nearly straight up or
        // down, where heading means little.
        if (pad_yaw_reference_valid &&
            now - pad_yaw_reference_time < std::chrono::milliseconds{500}) {
            const Vec3 forward = Rotate(pad_attitude, {0.0f, 0.0f, -1.0f});
            if (std::abs(forward.y) < 0.9f) {
                static constexpr float YawGain = 1.5f;
                static constexpr float Pi = 3.14159265f;
                float error = pad_yaw_reference - std::atan2(-forward.x, -forward.z);
                error -= 2.0f * Pi * std::round(error / (2.0f * Pi));
                pad_attitude = Multiply(
                    FromAxisAngle(Up, error * std::min(YawGain * elapsed, 1.0f)), pad_attitude);
            }
        }
        pad_attitude = Normalize(pad_attitude);
    }

    pad.pose.orientation = pad_attitude;
    pad.angular_velocity = Rotate(pad_attitude, angular_velocity);
    ++pad.sequence;
    pad.tracked = true;
    pad_position_tracked = false;
}

void Runtime::ResetPadYaw() {
    std::scoped_lock lock{mutex};
    if (!pad_attitude_valid) {
        return;
    }
    pad_attitude = Normalize(Multiply(Conjugate(YawOnly(pad_attitude)), pad_attitude));
    pad.pose.orientation = pad_attitude;
}

void Runtime::EnableViewGestures(bool enabled) {
    std::scoped_lock lock{gesture_mutex};
    if (enabled && !view_gestures.load(std::memory_order_relaxed)) {
        // A headset that has just come up: the player still has to settle.
        gesture_seat_taken = false;
    }
    view_gestures.store(enabled, std::memory_order_relaxed);
}

void Runtime::NotePadButton(PadButton button, bool pressed) {
    if (!view_gestures.load(std::memory_order_relaxed)) {
        return;
    }
    bool reset_seat = false;
    bool reset_view = false;
    {
        std::scoped_lock lock{gesture_mutex};
        switch (button) {
        case PadButton::Cross:
            if (pressed && !gesture_seat_taken) {
                gesture_seat_taken = true;
                reset_seat = true;
            }
            break;
        case PadButton::Options:
            if (pressed && !gesture_options_down) {
                gesture_options_since = std::chrono::steady_clock::now();
                gesture_options_fired = false;
            }
            gesture_options_down = pressed;
            break;
        case PadButton::Home:
            reset_view = pressed;
            break;
        }
    }
    if (reset_seat) {
        LOG_INFO(Core_Vr, "First press of X: the player's seat is where they are now");
        RequestRecenter();
    }
    if (reset_view) {
        LOG_INFO(Core_Vr, "PS button: view reset");
        RequestRecenter();
        ResetPadYaw();
    }
}

void Runtime::PollViewGestures() {
    if (!view_gestures.load(std::memory_order_relaxed)) {
        return;
    }
    {
        std::scoped_lock lock{gesture_mutex};
        if (!gesture_options_down || gesture_options_fired ||
            std::chrono::steady_clock::now() - gesture_options_since <
                std::chrono::milliseconds{1000}) {
            return;
        }
        gesture_options_fired = true;
        // Whoever resets the view has settled.
        gesture_seat_taken = true;
    }
    LOG_INFO(Core_Vr, "OPTIONS held: view reset");
    RequestRecenter();
    ResetPadYaw();
}

void Runtime::UpdatePadPosition(const Vec3& host_position, const Vec3& linear_velocity) {
    std::scoped_lock lock{mutex};
    pad_seen_position = PositionToTracker(host_position);
    pad_seen_velocity = DirectionToTracker(linear_velocity);
    pad_seen = true;
    pad_seen_time = std::chrono::steady_clock::now();
}

void Runtime::ClearPadPosition() {
    std::scoped_lock lock{mutex};
    pad_seen = false;
}

void Runtime::SetPadOffset(const Vec3& offset) {
    std::scoped_lock lock{mutex};
    config.pad_offset = offset;
    pad_seen_offset_valid = false;
}

void Runtime::UpdatePadYawReference(float yaw) {
    std::scoped_lock lock{mutex};
    // The host counts the heading from its own straight ahead.
    pad_yaw_reference = yaw - 2.0f * std::atan2(seat_yaw.y, seat_yaw.w);
    pad_yaw_reference_valid = true;
    pad_yaw_reference_time = std::chrono::steady_clock::now();
}

void Runtime::SetPadVibration(u8 small_motor, u8 large_motor) {
    std::function<void(const PadFeedback&)> listener;
    PadFeedback feedback;
    {
        std::scoped_lock lock{mutex};
        if (pad_feedback.small_motor == small_motor && pad_feedback.large_motor == large_motor) {
            return;
        }
        pad_feedback.small_motor = small_motor;
        pad_feedback.large_motor = large_motor;
        feedback = pad_feedback;
        listener = pad_feedback_listener;
    }
    if (listener) {
        listener(feedback);
    }
}

void Runtime::SetPadLight(u8 red, u8 green, u8 blue) {
    std::function<void(const PadFeedback&)> listener;
    PadFeedback feedback;
    {
        std::scoped_lock lock{mutex};
        if (pad_feedback.red == red && pad_feedback.green == green && pad_feedback.blue == blue) {
            return;
        }
        pad_feedback.red = red;
        pad_feedback.green = green;
        pad_feedback.blue = blue;
        feedback = pad_feedback;
        listener = pad_feedback_listener;
    }
    if (listener) {
        listener(feedback);
    }
}

void Runtime::SetPadFeedbackListener(std::function<void(const PadFeedback&)> listener) {
    PadFeedback feedback;
    {
        std::scoped_lock lock{mutex};
        pad_feedback_listener = listener;
        feedback = pad_feedback;
    }
    // Whoever starts listening late still learns the current state.
    if (listener) {
        listener(feedback);
    }
}

namespace {
bool Finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool ValidOrientation(const Quat& q) {
    const float length_squared = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return std::isfinite(length_squared) && length_squared > 1e-8f;
}
Vec3 Scaled(const Vec3& v, float scale) {
    return {v.x * scale, v.y * scale, v.z * scale};
}
Vec3 Difference(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
} // namespace

void Runtime::ExpireMoveLocked(u32 hand, std::chrono::steady_clock::time_point now) {
    auto& slot = moves[hand];
    if (slot.latest.connected && now - slot.received >= std::chrono::milliseconds{250}) {
        slot.latest.connected = false;
        slot.latest.device.tracked = false;
        slot.latest.buttons = slot.latest.trigger = 0;
        slot.latest.acceleration = slot.latest.gyro = {};
        slot.latest.device.linear_velocity = slot.latest.device.angular_velocity = {};
        slot.history_start = slot.history_count = 0;
        slot.velocity_known = false;
        slot.feedback = {};
        LOG_INFO(Core_Vr, "Move {} disconnected: host samples expired", hand);
    }
}

void Runtime::UpdateMove(u32 hand, const MoveHostState& host) {
    if (hand >= moves.size() || !IsMoveEnabled()) return;
    std::scoped_lock lock{mutex};
    const auto now = std::chrono::steady_clock::now();
    ExpireMoveLocked(hand, now);
    auto& slot = moves[hand];
    const u64 now_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count());
    const u64 capture_ns = host.sample_time_ns ? host.sample_time_ns : now_ns;
    // CLOCK_MONOTONIC is shared by the Quest host and Linux core. Never make queued old
    // data look newly connected or differentiate motion by socket receive jitter.
    if (capture_ns > now_ns || now_ns - capture_ns >= 250000000 ||
        (host.sample_time_ns && slot.source_time_ns && capture_ns <= slot.source_time_ns)) return;
    const u64 age_us = (now_ns - capture_ns) / 1000;
    const u64 process_now = Libraries::Kernel::sceKernelGetProcessTime();
    const u64 capture_us = process_now > age_us ? process_now - age_us : 1;
    MoveState sample{};
    sample.connected = host.connected;
    sample.timestamp_us = std::max(capture_us, slot.latest.timestamp_us + 1);
    sample.device.sequence = ++slot.sequence;
    const bool tracked = host.connected && host.device.tracked &&
                         Finite(host.device.pose.position) &&
                         ValidOrientation(host.device.pose.orientation);
    if (host.connected) {
        sample.buttons = host.buttons & (MoveInput::AllButtons & ~MoveInput::Trigger);
        sample.trigger = MoveInput::TriggerToByte(host.trigger);
        if (sample.trigger > 12) sample.buttons |= MoveInput::Trigger;
    }
    if (tracked) {
        sample.device.pose = host.device.pose;
        sample.device.pose.orientation = Normalize(host.device.pose.orientation);
        sample.device.tracked = true;
        const auto& previous = slot.latest;
        const double elapsed = sample.timestamp_us > previous.timestamp_us
                                   ? (sample.timestamp_us - previous.timestamp_us) / 1000000.0
                                   : 0.0;
        // Avoid tiny intervals, gaps and tracking reacquisition producing enormous derivatives.
        const bool consecutive = previous.connected && previous.device.tracked &&
                                 elapsed >= 0.002 && elapsed <= 0.1;
        bool velocity_known = host.linear_velocity_valid && Finite(host.device.linear_velocity);
        if (velocity_known) {
            sample.device.linear_velocity = host.device.linear_velocity;
        } else if (consecutive) {
            sample.device.linear_velocity = Scaled(
                Difference(sample.device.pose.position, previous.device.pose.position), 1.0f / elapsed);
            velocity_known = true;
        }
        if (host.angular_velocity_valid && Finite(host.device.angular_velocity)) {
            sample.device.angular_velocity = host.device.angular_velocity;
        } else if (consecutive) {
            // q_now * inverse(q_before) is a world-space rotation. q and -q are identical;
            // choose the short arc so quaternion sign changes do not create angular spikes.
            Quat delta = Normalize(Multiply(sample.device.pose.orientation,
                                            Conjugate(previous.device.pose.orientation)));
            if (delta.w < 0.0f) delta = {-delta.x, -delta.y, -delta.z, -delta.w};
            const float sine = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
            if (sine > 1e-6f) {
                sample.device.angular_velocity = Scaled({delta.x, delta.y, delta.z},
                    2.0f * std::atan2(sine, delta.w) / (sine * elapsed));
            }
        }
        Vec3 acceleration{};
        if (consecutive && velocity_known && slot.velocity_known) {
            acceleration = Scaled(Difference(sample.device.linear_velocity,
                                              previous.device.linear_velocity), 1.0f / elapsed);
        }
        // OpenXR exposes pose/velocity, not the controller's raw IMU. Synthesize specific
        // force from tracked velocity and gravity, in the same local frame as the grip pose.
        constexpr float Gravity = 9.80665f;
        acceleration.y += Gravity;
        sample.acceleration = Scaled(Rotate(Conjugate(sample.device.pose.orientation), acceleration),
                                     1.0f / Gravity);
        sample.gyro = Rotate(Conjugate(sample.device.pose.orientation), sample.device.angular_velocity);
        slot.velocity_known = velocity_known;
    } else {
        slot.velocity_known = false;
        // A controller outside tracking still supplies buttons, but never retains a motor
        // command that could unexpectedly resume when tracking is reacquired.
        slot.feedback.intensity = 0;
    }
    if (slot.latest.connected != sample.connected) {
        LOG_INFO(Core_Vr, "Move {} {}", hand, sample.connected ? "connected" : "disconnected");
    }
    if (!sample.connected) {
        slot.history_start = slot.history_count = 0;
        slot.feedback = {};
    } else {
        if (slot.history_count == slot.history.size()) {
            slot.history_start = (slot.history_start + 1) % slot.history.size();
            --slot.history_count;
        }
        slot.history[(slot.history_start + slot.history_count) % slot.history.size()] = sample;
        ++slot.history_count;
    }
    if (Diagnostics::Capture::Instance().Active()) {
        const auto guest = MoveToTracker(sample);
        Diagnostics::Record r{};
        r.kind = 2; r.hand = hand;
        r.flags = sample.connected | (sample.device.tracked << 1) |
                  (host.linear_velocity_valid << 2) | (host.angular_velocity_valid << 3);
        r.generation = guest.diagnostic_reference_generation;
        r.receipt_us = process_now; r.returned_us = sample.timestamp_us;
        r.sequence = sample.device.sequence; r.host_sequence = Diagnostics::host_sequence;
        Diagnostics::PutPose(r, 0, host.device.pose);
        Diagnostics::PutPose(r, 7, host_head.pose);
        Diagnostics::PutPose(r, 14, guest.device.pose);
        Diagnostics::PutVector(r, 21, guest.device.linear_velocity);
        Diagnostics::PutVector(r, 24, guest.device.angular_velocity);
        Diagnostics::PutVector(r, 27, sample.acceleration);
        Diagnostics::PutVector(r, 30, sample.gyro);
        Diagnostics::PutVector(r, 33, seat_position);
        r.values[36] = seat_yaw.x; r.values[37] = seat_yaw.y;
        r.values[38] = seat_yaw.z; r.values[39] = seat_yaw.w;
        Diagnostics::PutVector(r, 40, config.origin_offset);
        r.values[43] = static_cast<float>(age_us);
        Diagnostics::Capture::Instance().Push(r);
    }
    // Opt-in diagnostics compare raw grip and guest tracker coordinates without
    // changing offsets, smoothing or prediction. No pose logging by default.
    static const bool diagnose = [] {
        const char* value = std::getenv("SHADPS4_MOVE_DIAGNOSTICS");
        return value && std::string_view{value} == "1";
    }();
    if (diagnose) {
        static std::array<u64, 2> samples{}, tracked_samples{};
        static std::array<std::chrono::steady_clock::time_point, 2> reported{};
        ++samples[hand];
        tracked_samples[hand] += tracked;
        if (now - reported[hand] >= std::chrono::seconds{2} ||
            sample.device.tracked != slot.latest.device.tracked) {
            const auto guest = MoveToTracker(sample);
            LOG_INFO(Core_Vr, "MOVE_DIAG hand={} connected={} tracked={} samples={}/{} age_us={} "
                "grip=({:.3f},{:.3f},{:.3f}) head=({:.3f},{:.3f},{:.3f}) "
                "guest=({:.3f},{:.3f},{:.3f}) velocity=({:.3f},{:.3f},{:.3f}) "
                "grip_q=({:.3f},{:.3f},{:.3f},{:.3f}) guest_q=({:.3f},{:.3f},{:.3f},{:.3f}) buttons={:#x} trigger={}",
                hand, sample.connected, tracked, tracked_samples[hand], samples[hand], age_us,
                host.device.pose.position.x, host.device.pose.position.y, host.device.pose.position.z,
                host_head.pose.position.x, host_head.pose.position.y, host_head.pose.position.z,
                guest.device.pose.position.x, guest.device.pose.position.y, guest.device.pose.position.z,
                guest.device.linear_velocity.x, guest.device.linear_velocity.y, guest.device.linear_velocity.z,
                host.device.pose.orientation.x, host.device.pose.orientation.y,
                host.device.pose.orientation.z, host.device.pose.orientation.w,
                guest.device.pose.orientation.x, guest.device.pose.orientation.y,
                guest.device.pose.orientation.z, guest.device.pose.orientation.w, sample.buttons, sample.trigger);
            reported[hand] = now;
            samples[hand] = tracked_samples[hand] = 0;
        }
    }
    slot.latest = sample;
    slot.source_time_ns = capture_ns;
    slot.received = now - std::chrono::nanoseconds{now_ns - capture_ns};
}

MoveState Runtime::MoveToTracker(const MoveState& state) const {
    MoveState result = state;
    result.diagnostic_reference_generation = Diagnostics::seat_generation.load();
    if (result.device.tracked) {
        result.device.pose.position = PositionToTracker(state.device.pose.position);
        result.device.pose.orientation = Normalize(Multiply(Conjugate(seat_yaw), state.device.pose.orientation));
        result.device.linear_velocity = DirectionToTracker(state.device.linear_velocity);
        result.device.angular_velocity = DirectionToTracker(state.device.angular_velocity);
    }
    return result;
}

MoveState Runtime::GetMove(u32 hand) {
    if (hand >= moves.size() || !IsMoveEnabled()) return {};
    std::scoped_lock lock{mutex};
    ExpireMoveLocked(hand, std::chrono::steady_clock::now());
    return MoveToTracker(moves[hand].latest);
}

u32 Runtime::ReadMoveRecent(u32 hand, u64 after, MoveState* out, u32 capacity) {
    if (hand >= moves.size() || !out || !capacity || !IsMoveEnabled()) return 0;
    std::scoped_lock lock{mutex};
    ExpireMoveLocked(hand, std::chrono::steady_clock::now());
    const auto& slot = moves[hand];
    if (!slot.latest.connected) return 0;
    u32 count = 0;
    for (u32 i = 0; i < slot.history_count && count < capacity; ++i) {
        const auto& sample = slot.history[(slot.history_start + i) % slot.history.size()];
        if (sample.timestamp_us > after) out[count++] = MoveToTracker(sample);
    }
    return count;
}

void Runtime::ReleaseMoves() {
    std::function<void(u32, const MoveFeedback&)> listener;
    {
        std::scoped_lock lock{mutex};
        for (auto& slot : moves) {
            const auto sequence = slot.sequence;
            const auto timestamp = slot.latest.timestamp_us;
            slot = {};
            slot.sequence = sequence;
            slot.latest.timestamp_us = timestamp;
        }
        listener = move_feedback_listener;
    }
    if (listener) for (u32 hand = 0; hand < moves.size(); ++hand) listener(hand, {});
}

void Runtime::SetMoveVibration(u32 hand, u8 intensity) {
    if (hand >= moves.size() || !IsMoveEnabled()) return;
    MoveFeedback feedback;
    std::function<void(u32, const MoveFeedback&)> listener;
    {
        std::scoped_lock lock{mutex};
        ExpireMoveLocked(hand, std::chrono::steady_clock::now());
        auto& slot = moves[hand];
        slot.feedback.intensity = slot.latest.connected && slot.latest.device.tracked ? intensity : 0;
        feedback = slot.feedback;
        listener = move_feedback_listener;
    }
    if (listener) listener(hand, feedback);
}

void Runtime::SetMoveLight(u32 hand, u8 red, u8 green, u8 blue) {
    if (hand >= moves.size() || !IsMoveEnabled()) return;
    MoveFeedback feedback;
    std::function<void(u32, const MoveFeedback&)> listener;
    {
        std::scoped_lock lock{mutex};
        ExpireMoveLocked(hand, std::chrono::steady_clock::now());
        auto& slot = moves[hand];
        slot.feedback.red = red;
        slot.feedback.green = green;
        slot.feedback.blue = blue;
        feedback = slot.feedback;
        listener = move_feedback_listener;
    }
    if (listener) listener(hand, feedback);
}

MoveFeedback Runtime::GetMoveFeedback(u32 hand) {
    if (hand >= moves.size() || !IsMoveEnabled()) return {};
    std::scoped_lock lock{mutex};
    ExpireMoveLocked(hand, std::chrono::steady_clock::now());
    return moves[hand].latest.connected ? moves[hand].feedback : MoveFeedback{};
}

void Runtime::SetMoveFeedbackListener(std::function<void(u32, const MoveFeedback&)> listener) {
    std::array<MoveFeedback, 2> feedback;
    {
        std::scoped_lock lock{mutex};
        move_feedback_listener = listener;
        for (u32 hand = 0; hand < moves.size(); ++hand) {
            ExpireMoveLocked(hand, std::chrono::steady_clock::now());
            feedback[hand] = moves[hand].latest.connected ? moves[hand].feedback : MoveFeedback{};
        }
    }
    if (listener) for (u32 hand = 0; hand < moves.size(); ++hand) listener(hand, feedback[hand]);
}

void Runtime::UpdateOptics(const Fov& fov, float ipd) {
    std::scoped_lock lock{mutex};
    config.fov = fov;
    config.ipd = ipd;
}

namespace {

std::filesystem::path HeadsetFovPath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "vr_headset_fov.json";
}

float Degrees(float tangent) {
    return std::atan(tangent) * 57.29578f;
}

} // namespace

void Runtime::NoteHeadsetFov(const Fov& fov) {
    bool changed;
    {
        std::scoped_lock lock{mutex};
        const auto differs = [](float a, float b) { return std::abs(Degrees(a) - Degrees(b)) > 0.5f; };
        changed = !has_headset_fov || differs(fov.tan_out, headset_fov.tan_out) ||
                  differs(fov.tan_in, headset_fov.tan_in) ||
                  differs(fov.tan_top, headset_fov.tan_top) ||
                  differs(fov.tan_bottom, headset_fov.tan_bottom);
        headset_fov = fov;
        has_headset_fov = true;
    }
    headset_fov_known.notify_all();
    if (!changed) {
        return;
    }
    LOG_INFO(Core_Vr,
             "The headset shows {:.1f}/{:.1f}/{:.1f}/{:.1f} degrees an eye (out, in, up, down): "
             "{:.0f} by {:.0f}",
             Degrees(fov.tan_out), Degrees(fov.tan_in), Degrees(fov.tan_top),
             Degrees(fov.tan_bottom), Degrees(fov.tan_out) + Degrees(fov.tan_in),
             Degrees(fov.tan_top) + Degrees(fov.tan_bottom));
    // For the next start, when the title asks before the headset has said.
    std::ofstream file{HeadsetFovPath()};
    file << nlohmann::json{{"fov_tan", {fov.tan_out, fov.tan_in, fov.tan_top, fov.tan_bottom}}}
                .dump()
         << "\n";
}

Fov Runtime::TitleFov() {
    if (!config.fov_from_headset) {
        std::scoped_lock lock{mutex};
        return config.fov;
    }
    // The title asks once, when it opens the headset: what it is told is what it draws for
    // the rest of the session. The host learns what its headset shows once its session runs.
    // (Only for a headset that is there: a game on the monitor has nothing to wait for.)
    bool headset_there = false;
#ifdef ENABLE_OPENXR_HOST
    headset_there = OpenXrHost::Instance().IsAvailable();
#endif
    const auto longest_wait = headset_there ? std::chrono::seconds{10} : std::chrono::seconds{0};
    Fov base{};
    const char* from = "the headset's own";
    {
        std::unique_lock lock{mutex};
        if (!headset_fov_known.wait_for(lock, longest_wait, [&] { return has_headset_fov; })) {
            from = nullptr;
        } else {
            base = headset_fov;
        }
    }
    if (from == nullptr) {
        // As the headset showed it the last time.
        std::ifstream file{HeadsetFovPath()};
        const auto json = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json{};
        const auto usable = [](const nlohmann::json& tangents) {
            if (!tangents.is_array() || tangents.size() != 4) {
                return false;
            }
            for (const auto& tangent : tangents) {
                if (!tangent.is_number() || tangent.get<float>() < 0.1f ||
                    tangent.get<float>() > 10.0f) {
                    return false;
                }
            }
            return true;
        };
        if (json.is_object() && json.contains("fov_tan") && usable(json["fov_tan"])) {
            base = {json["fov_tan"][0].get<float>(), json["fov_tan"][1].get<float>(),
                    json["fov_tan"][2].get<float>(), json["fov_tan"][3].get<float>()};
            from = "the headset's, as it was the last time (it has not said yet)";
        } else {
            from = "a PlayStation VR's (the headset has not said what it shows)";
        }
    }
    const float scale = config.fov_scale;
    const Fov fov{base.tan_out * scale, base.tan_in * scale, base.tan_top * scale,
                  base.tan_bottom * scale};
    {
        std::scoped_lock lock{mutex};
        config.fov = fov;
    }
    LOG_INFO(Core_Vr,
             "The title draws {:.0f}% of {}: {:.1f}/{:.1f}/{:.1f}/{:.1f} degrees an eye (out, in, "
             "up, down)",
             scale * 100.0f, from, Degrees(fov.tan_out), Degrees(fov.tan_in), Degrees(fov.tan_top),
             Degrees(fov.tan_bottom));
    return fov;
}

void Runtime::NoteDisplayRefresh(float rate, u64 time) {
    // SHADPS4_VR_FOLLOW_DISPLAY=0: the emulated headset keeps to its own clock.
    static const bool follow = [] {
        const char* value = std::getenv("SHADPS4_VR_FOLLOW_DISPLAY");
        return value == nullptr || value[0] != '0';
    }();
    if (!follow) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    // A time that is not of this clock, or not of this moment, is no use.
    const Clock::time_point told{std::chrono::nanoseconds{static_cast<s64>(time)}};
    const bool usable = time != 0 && told <= now && now - told < std::chrono::milliseconds{100};
    std::scoped_lock lock{mutex};
    ++display_refresh.sequence;
    display_refresh.time = usable ? told : now;
    display_refresh.rate = rate;
}

Runtime::DisplayRefresh Runtime::GetDisplayRefresh() const {
    std::scoped_lock lock{mutex};
    return display_refresh;
}

float Runtime::HeadsetRefreshRate() const {
    std::scoped_lock lock{mutex};
    if (display_refresh.sequence != 0 && display_refresh.rate > 30.0f &&
        std::chrono::steady_clock::now() - display_refresh.time < std::chrono::milliseconds{500}) {
        return display_refresh.rate;
    }
    return static_cast<float>(config.refresh_rate);
}

DeviceState Runtime::DemoHead() const {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    const float t = duration<float>(steady_clock::now() - start).count();

    DeviceState state;
    const float yaw = config.demo_motion ? 0.45f * std::sin(t * 0.50f) : 0.0f;
    const float pitch = config.demo_motion ? 0.20f * std::sin(t * 0.31f) : 0.0f;
    state.pose.orientation = FromYawPitch(yaw, pitch);
    state.pose.position = config.origin_offset;
    if (config.demo_motion) {
        state.pose.position.x += 0.10f * std::sin(t * 0.23f);
        state.angular_velocity = {0.20f * 0.31f * std::cos(t * 0.31f),
                                  0.45f * 0.50f * std::cos(t * 0.50f), 0.0f};
        state.linear_velocity = {0.10f * 0.23f * std::cos(t * 0.23f), 0.0f, 0.0f};
    }
    state.tracked = true;
    return state;
}

DeviceState Runtime::GetHead() {
    std::scoped_lock lock{mutex};
    if (head.sequence == 0) {
        return DemoHead();
    }
    if (!title_asked) {
        // A title takes where the head is when it first looks for the place the player sits
        // at, and that is some time after the headset was put on.
        title_asked = true;
        RecenterSeatLocked();
    }
    return head;
}

DeviceState Runtime::GetPad() {
    std::scoped_lock lock{mutex};
    if (pad.sequence != 0 && pad_position_tracked) {
        return pad;
    }

    // Nothing locates the controller, so hold it at a fixed reach. Hanging it off the head
    // position (but not the head rotation, hands do not swing around when the player looks
    // about) keeps it in front of the player wherever they sit; the smoothing stops it from
    // mirroring every nod.
    const DeviceState current_head = head.sequence == 0 ? DemoHead() : head;
    const Vec3 target = config.pad_follows_head ? current_head.pose.position : config.origin_offset;
    const auto now = std::chrono::steady_clock::now();
    if (!pad_anchor_valid) {
        pad_anchor = target;
        pad_anchor_valid = true;
    } else {
        static constexpr float FollowTime = 0.25f;
        const float elapsed = std::chrono::duration<float>(now - pad_anchor_time).count();
        const float blend = 1.0f - std::exp(-std::clamp(elapsed, 0.0f, 1.0f) / FollowTime);
        pad_anchor.x += (target.x - pad_anchor.x) * blend;
        pad_anchor.y += (target.y - pad_anchor.y) * blend;
        pad_anchor.z += (target.z - pad_anchor.z) * blend;
    }
    pad_anchor_time = now;

    // The host seeing the controller beats assuming where it is. Either way the reported
    // position glides to its target: sight of the hands comes and goes, and a controller that
    // teleports is worse than one that lags a little.
    const bool seen = pad_seen && now - pad_seen_time < std::chrono::milliseconds{400};
    if (seen) {
        // People keep a controller where they hold it: out of sight, that is the best guess.
        pad_seen_offset = {pad_seen_position.x - pad_anchor.x, pad_seen_position.y - pad_anchor.y,
                           pad_seen_position.z - pad_anchor.z};
        pad_seen_offset_valid = true;
    }
    const Vec3& assumed = pad_seen_offset_valid ? pad_seen_offset : config.pad_offset;
    const Vec3 goal = seen ? pad_seen_position
                           : Vec3{pad_anchor.x + assumed.x, pad_anchor.y + assumed.y,
                                  pad_anchor.z + assumed.z};
    if (!pad_shown_valid) {
        pad_shown_position = goal;
        pad_shown_valid = true;
    } else {
        const float follow_time = seen ? 0.035f : 0.30f;
        const float elapsed = std::chrono::duration<float>(now - pad_shown_time).count();
        const float blend = 1.0f - std::exp(-std::clamp(elapsed, 0.0f, 1.0f) / follow_time);
        pad_shown_position.x += (goal.x - pad_shown_position.x) * blend;
        pad_shown_position.y += (goal.y - pad_shown_position.y) * blend;
        pad_shown_position.z += (goal.z - pad_shown_position.z) * blend;
    }
    pad_shown_time = now;

    DeviceState state;
    state.pose.position = pad_shown_position;
    if (seen) {
        state.linear_velocity = pad_seen_velocity;
    }
    if (pad.sequence != 0) {
        state.pose.orientation = pad.pose.orientation;
        state.angular_velocity = pad.angular_velocity;
    }
    state.sequence = pad.sequence;
    state.tracked = true;
    return state;
}

Pose Runtime::ToHostSpace(const Pose& tracker_pose) const {
    std::scoped_lock lock{mutex};
    const auto from_seat = [&](const Vec3& position, const Quat& yaw) {
        const Vec3 seated = Rotate(yaw, {tracker_pose.position.x - config.origin_offset.x,
                                         tracker_pose.position.y - config.origin_offset.y,
                                         tracker_pose.position.z - config.origin_offset.z});
        Pose pose;
        pose.position = {seated.x + position.x, seated.y + position.y, seated.z + position.z};
        pose.orientation = Normalize(Multiply(yaw, tracker_pose.orientation));
        return pose;
    };
    const Pose pose = from_seat(seat_position, seat_yaw);
    // A pose the title was given just before the view was reset was counted from the seat
    // before: taken from the new one it would point somewhere the head never was. The head
    // does not turn far in the time a frame takes, so the seat that puts the pose nearer to
    // where the head is now is the one it was counted from.
    if (host_head.tracked &&
        std::chrono::steady_clock::now() - seat_changed < std::chrono::milliseconds{250}) {
        const Pose before = from_seat(previous_seat_position, previous_seat_yaw);
        const auto nearness = [&](const Quat& q) {
            const Quat& head_now = host_head.pose.orientation;
            return std::abs(q.x * head_now.x + q.y * head_now.y + q.z * head_now.z +
                            q.w * head_now.w);
        };
        if (nearness(before.orientation) > nearness(pose.orientation)) {
            return before;
        }
    }
    return pose;
}

void Runtime::SetFrameListener(std::function<void(const PresentedFrame&)> listener) {
    std::scoped_lock lock{mutex};
    frame_listener = std::move(listener);
}

void Runtime::NotifyFramePresented(const PresentedFrame& frame) {
    std::function<void(const PresentedFrame&)> listener;
    {
        std::scoped_lock lock{mutex};
        listener = frame_listener;
    }
    if (listener) {
        listener(frame);
    }
}

} // namespace Core::Vr
