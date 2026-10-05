// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <cstdlib>
#include <string_view>
#include <mutex>
#include <optional>
#include <vector>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/time.h"
#include "core/vr/move_capture.h"
#include "core/libraries/libs.h"
#include "core/libraries/move/move.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/known_title.h"
#include "core/libraries/vr_tracker/vr_tracker_error.h"
#include "core/memory.h"
#include "core/vr/vr_runtime.h"
#include "video_core/amdgpu/liverpool.h"

namespace Libraries::VrTracker {

// Serialize lifecycle and registration operations with reads. A recursive lock allows the
// existing lookup helpers to be used by public entry points while holding the same lock.
static std::recursive_mutex g_tracker_mutex;
static bool g_library_initialized = false;

// Internal memory
static void* g_garlic_memory_pointer = nullptr;
static u32 g_garlic_size = 0;
static void* g_onion_memory_pointer = nullptr;
static u32 g_onion_size = 0;
static void* g_work_memory_pointer = nullptr;
static u32 g_work_size = 0;

// Registered handles
// The DualShock 4s registered, in the order they were: the first is the player's, the one the
// host tracks. A console's tracker tells up to four apart by the colour of their light bars;
// a title registers the controller of every player logged in.
struct PadRegistration {
    s32 handle;
    OrbisVrTrackerLedColor color;
};
static std::vector<PadRegistration> g_pads;
struct MoveRegistration {
    s32 handle;
    u32 hand;
    OrbisVrTrackerLedColor color;
};
static std::vector<MoveRegistration> g_moves;
static s32 g_gun_handle = -1;
static s32 g_hmd_handle = -1;

/// The registration of a DualShock 4 handle, if it is one.
static std::optional<PadRegistration> FindPad(s32 handle) {
    std::scoped_lock lock{g_tracker_mutex};
    for (const PadRegistration& pad : g_pads) {
        if (pad.handle == handle) {
            return pad;
        }
    }
    return std::nullopt;
}

/// Whether a handle is the player's controller, the one the host tracks.
static bool IsPlayersPad(s32 handle) {
    std::scoped_lock lock{g_tracker_mutex};
    return !g_pads.empty() && g_pads.front().handle == handle;
}

static std::optional<MoveRegistration> FindMove(s32 handle) {
    std::scoped_lock lock{g_tracker_mutex};
    for (const auto& move : g_moves) {
        if (move.handle == handle) {
            return move;
        }
    }
    return std::nullopt;
}

static void ClearRegistrations() {
    g_pads.clear();
    g_moves.clear();
    g_hmd_handle = -1;
    g_gun_handle = -1;
}

static bool HeadsetConnected() {
    return Core::Vr::Runtime::Instance().IsHeadsetConnected();
}

static void WritePose(OrbisVrTrackerPoseData& out, const Core::Vr::Pose& pose) {
    out.position_x = pose.position.x;
    out.position_y = pose.position.y;
    out.position_z = pose.position.z;
    out.orientation_x = pose.orientation.x;
    out.orientation_y = pose.orientation.y;
    out.orientation_z = pose.orientation.z;
    out.orientation_w = pose.orientation.w;
}

s32 PS4_SYSV_ABI sceVrTrackerQueryMemory(const OrbisVrTrackerQueryMemoryParam* param,
                                         OrbisVrTrackerQueryMemoryResult* result) {
    LOG_DEBUG(Lib_VrTracker, "called");
    if (param == nullptr || result == nullptr ||
        param->size != sizeof(OrbisVrTrackerQueryMemoryParam) ||
        (param->profile != OrbisVrTrackerProfile::ORBIS_VR_TRACKER_PROFILE_000 &&
         param->profile != OrbisVrTrackerProfile::ORBIS_VR_TRACKER_PROFILE_100) ||
        param->calibration_settings.pad_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        // Hmd doesn't support auto calibration
        param->calibration_settings.hmd_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_MANUAL ||
        param->calibration_settings.move_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.gun_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Setting move_position to ORBIS_VR_TRACKER_CALIBRATION_AUTO doubles required onion memory.
    u32 required_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    if (param->calibration_settings.move_position ==
        OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        required_onion_size *= 2;
    }

    result->direct_memory_onion_size = required_onion_size;
    result->direct_memory_onion_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    result->direct_memory_garlic_size = ORBIS_VR_TRACKER_GARLIC_SIZE;
    result->direct_memory_garlic_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    result->work_memory_size = ORBIS_VR_TRACKER_WORK_SIZE;
    result->work_memory_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerInit(const OrbisVrTrackerInitParam* param) {
    std::scoped_lock lifecycle_lock{g_tracker_mutex};
    if (g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_ALREADY_INITIALIZED;
    }

    if (param == nullptr) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Calculate correct onion size for parameter checks
    u32 required_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    if (param->calibration_settings.move_position == ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        required_onion_size *= 2;
    }

    // Parameter checks are fairly thorough here.
    if (param->size != sizeof(OrbisVrTrackerInitParam) ||
        // Check garlic memory parameters
        param->direct_memory_garlic == nullptr ||
        param->direct_memory_garlic_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->direct_memory_garlic_size != ORBIS_VR_TRACKER_GARLIC_SIZE ||
        // Check onion memory parameters
        param->direct_memory_onion == nullptr ||
        param->direct_memory_onion_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->direct_memory_onion_size != required_onion_size ||
        // Check work memory parameters
        param->work_memory == nullptr ||
        param->work_memory_alignment != ORBIS_VR_TRACKER_MEMORY_ALIGNMENT ||
        param->work_memory_size != ORBIS_VR_TRACKER_WORK_SIZE ||
        // Check compute queue parameters
        param->gpu_pipe_id < 0 ||
        static_cast<u32>(param->gpu_pipe_id) >= AmdGpu::Liverpool::NumComputePipes ||
        param->gpu_queue_id < 0 ||
        static_cast<u32>(param->gpu_queue_id) >= AmdGpu::Liverpool::NumQueuesPerPipe ||
        // Check calibration settings
        param->calibration_settings.pad_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.hmd_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_MANUAL ||
        param->calibration_settings.move_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO ||
        param->calibration_settings.gun_position >
            OrbisVrTrackerCalibrationMode::ORBIS_VR_TRACKER_CALIBRATION_AUTO) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Real hardware will segfault if any of the supplied mappings aren't long enough,
    // Validate each of them to ensure nothing weird can occur when this library is implemented.
    auto* memory = Core::Memory::Instance();
    Libraries::Kernel::OrbisVirtualQueryInfo info;
    // The memory type for the whole range should be the same here,
    // so the memory should be contained in one VMA.
    VAddr addr_to_check = std::bit_cast<VAddr>(param->direct_memory_garlic);
    s32 result = memory->VirtualQuery(addr_to_check, 0, &info);
    ASSERT_MSG(result == 0 && info.end - addr_to_check >= param->direct_memory_garlic_size,
               "Insufficient garlic memory provided");

    g_garlic_memory_pointer = param->direct_memory_garlic;
    g_garlic_size = param->direct_memory_garlic_size;

    addr_to_check = std::bit_cast<VAddr>(param->direct_memory_onion);
    result = memory->VirtualQuery(addr_to_check, 0, &info);
    ASSERT_MSG(result == 0 && info.end - addr_to_check >= param->direct_memory_onion_size,
               "Insufficient onion memory provided");

    g_onion_memory_pointer = param->direct_memory_onion;
    g_onion_size = param->direct_memory_onion_size;

    addr_to_check = std::bit_cast<VAddr>(param->work_memory);
    result = memory->VirtualQuery(addr_to_check, 0, &info);
    ASSERT_MSG(result == 0 && info.end - addr_to_check >= param->work_memory_size,
               "Insufficient work memory provided");

    g_work_memory_pointer = param->work_memory;
    g_work_size = param->work_memory_size;

    // All initialization checks passed.
    LOG_INFO(Lib_VrTracker, "called, virtual headset connected = {}", HeadsetConnected());
    ClearRegistrations();
    g_library_initialized = true;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDevice(const OrbisVrTrackerDeviceType device_type,
                                            const s32 handle) {
    LOG_TRACE(Lib_VrTracker, "redirected to sceVrTrackerRegisterDeviceInternal");
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, -1, 0);
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDevice2(const OrbisVrTrackerDeviceType device_type,
                                             const s32 handle) {
    LOG_TRACE(Lib_VrTracker, "redirected to sceVrTrackerRegisterDeviceInternal");
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, -1, 1);
}

s32 PS4_SYSV_ABI sceVrTrackerRegisterDeviceInternal(const OrbisVrTrackerDeviceType device_type,
                                                    const s32 handle, s32 unk0, s32 unk1) {
    LOG_WARNING(Lib_VrTracker, "(STUBBED) called, device_type = {}, handle = {}",
                static_cast<u32>(device_type), handle);
    std::scoped_lock lifecycle_lock{g_tracker_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (device_type < OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_HMD ||
        device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN || unk0 < -1 ||
        unk0 > 4 || handle < 0) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Move handles are resolved through libSceMove; existing HMD/pad semantics are retained.
    switch (device_type) {
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_HMD: {
        if (g_hmd_handle != -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        g_hmd_handle = handle;
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4: {
        OrbisVrTrackerLedColor color;
        bool players;
        {
            std::scoped_lock lock{g_tracker_mutex};
            for (const PadRegistration& pad : g_pads) {
                if (pad.handle == handle) {
                    return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
                }
            }
            if (g_pads.size() >= 4) {
                return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
            }
            if (unk0 >= 0) {
                color = static_cast<OrbisVrTrackerLedColor>(unk0);
            } else {
                // The first colour no other controller has.
                s32 free = 0;
                while (std::ranges::any_of(g_pads, [&](const PadRegistration& pad) {
                    return static_cast<s32>(pad.color) == free;
                })) {
                    ++free;
                }
                color = static_cast<OrbisVrTrackerLedColor>(free);
            }
            g_pads.push_back({handle, color});
            players = g_pads.size() == 1;
        }
        if (!players) {
            LOG_INFO(Lib_VrTracker,
                     "Controller {} registered as well: it is not the player's, and nothing "
                     "tracks it",
                     handle);
            break;
        }
        // The camera tells controllers apart by the colour of their light bar, so the system
        // sets it, and titles refer to it ("move the red light bar..."). The real controller
        // should show the same colour.
        static constexpr u8 Colours[5][3] = {
            {0, 0, 255}, {255, 0, 0}, {0, 255, 0}, {255, 0, 255}, {255, 255, 0}};
        const auto& colour = Colours[std::clamp<s32>(static_cast<s32>(color), 0, 4)];
        Core::Vr::Runtime::Instance().SetPadLight(colour[0], colour[1], colour[2]);
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_MOVE: {
        const auto hand = Move::HandForHandle(handle);
        if (!hand) {
            return ORBIS_VR_TRACKER_ERROR_INVALID_DEVICE_HANDLE;
        }
        // Closing a Move invalidates its old registration even if the title has not yet
        // called UnregisterDevice. Never transfer that registration to the next handle.
        std::erase_if(g_moves, [](const MoveRegistration& move) {
            return !Move::HandForHandle(move.handle).has_value();
        });
        if (std::ranges::any_of(g_moves, [&](const MoveRegistration& move) {
                return move.handle == handle || move.hand == *hand;
            })) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        if (g_moves.size() >= 2) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_LIMIT;
        }
        const auto color = unk0 >= 0 ? static_cast<OrbisVrTrackerLedColor>(unk0)
                                    : (*hand == 0 ? ORBIS_VR_TRACKER_LED_COLOR_MAGENTA
                                                  : ORBIS_VR_TRACKER_LED_COLOR_CYAN);
        g_moves.push_back({handle, *hand, color});
        static constexpr u8 Colours[5][3] = {
            {0, 0, 255}, {255, 0, 0}, {0, 255, 255}, {255, 0, 255}, {255, 255, 0}};
        const auto& rgb = Colours[static_cast<s32>(color)];
        Core::Vr::Runtime::Instance().SetMoveLight(*hand, rgb[0], rgb[1], rgb[2]);
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN: {
        if (g_gun_handle != -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED;
        }
        g_gun_handle = handle;
        break;
    }
    default: {
        // Shouldn't be possible to hit this.
        UNREACHABLE();
    }
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuProcess(const OrbisVrTrackerCpuProcessParam* param) {
    LOG_TRACE(Lib_VrTracker, "called");
    std::scoped_lock lifecycle_lock{g_tracker_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    // Tracking is done by the host, there is no camera image to process.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetPlayAreaWarningInfo(OrbisVrTrackerPlayAreaWarningInfo* info) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetResult(const OrbisVrTrackerGetResultParam* param,
                                       OrbisVrTrackerResultData* result) {
    LOG_TRACE(Lib_VrTracker, "called");
    std::scoped_lock lifecycle_lock{g_tracker_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || result == nullptr ||
        param->size != sizeof(OrbisVrTrackerGetResultParam) || param->handle < 0) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    const s32 handle = param->handle;
    const bool is_hmd = handle == g_hmd_handle;
    const auto pad_registration = FindPad(handle);
    const auto move_registration = FindMove(handle);
    // (Another player's controller is registered, but nothing tracks it.)
    const bool is_pad = pad_registration.has_value() && IsPlayersPad(handle);
    if (!is_hmd && !pad_registration && !move_registration && handle != g_gun_handle) {
        return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
    }

    std::memset(result, 0, sizeof(OrbisVrTrackerResultData));
    result->handle = handle;
    result->user_frame_number = param->user_frame_number;
    result->camera_orientation_w = 1.0f;

    if (move_registration) {
        const auto hand = Move::HandForHandle(handle);
        if (!hand || *hand != move_registration->hand) {
            return ORBIS_VR_TRACKER_ERROR_INVALID_DEVICE_HANDLE;
        }
        const auto move = Core::Vr::Runtime::Instance().GetMove(*hand);
        result->connected = move.connected ? 1 : 0;
        result->timestamp = move.timestamp_us;
        result->device_timestamp = move.timestamp_us;
        if (Core::Vr::Diagnostics::Capture::Instance().Active()) {
            Core::Vr::Diagnostics::Record r{};
            r.kind = 3; r.hand = *hand;
            r.flags = move.connected | (move.device.tracked << 1);
            r.generation = move.diagnostic_reference_generation;
            r.receipt_us = Libraries::Kernel::sceKernelGetProcessTime();
            r.sequence = move.device.sequence; r.requested_us = param->prediction_time;
            r.returned_us = result->timestamp;
            Core::Vr::Diagnostics::PutPose(r, 0, move.device.pose);
            Core::Vr::Diagnostics::PutVector(r, 7, move.device.linear_velocity);
            Core::Vr::Diagnostics::PutVector(r, 10, move.device.angular_velocity);
            Core::Vr::Diagnostics::PutVector(r, 13, move.acceleration);
            Core::Vr::Diagnostics::PutVector(r, 16, move.gyro);
            r.values[19] = static_cast<float>(param->result_type);
            r.values[20] = static_cast<float>(param->orientation_type);
            r.values[21] = static_cast<float>(param->usage_type);
            Core::Vr::Diagnostics::Capture::Instance().Push(r);
        }
        static const bool diagnose = [] {
            const char* value = std::getenv("SHADPS4_MOVE_DIAGNOSTICS");
            return value && std::string_view{value} == "1";
        }();
        if (diagnose) {
            static std::array<u64, 2> calls{};
            if (calls[*hand]++ % 120 == 0) {
                const u64 now = Libraries::Kernel::sceKernelGetProcessTime();
                LOG_INFO(Lib_VrTracker, "MOVE_TRACKER_DIAG hand={} sequence={} tracked={} age_us={} "
                    "requested={} result_time={} result_type={} orientation_type={} usage_type={}",
                    *hand, move.device.sequence, move.device.tracked,
                    now >= move.timestamp_us ? now - move.timestamp_us : 0,
                    param->prediction_time, result->timestamp, static_cast<u32>(param->result_type),
                    static_cast<u32>(param->orientation_type), static_cast<u32>(param->usage_type));
            }
        }
        result->led_color = move_registration->color;
        result->status = ORBIS_VR_TRACKER_STATUS_NOT_TRACKING;
        // Connection and optical tracking are independent. Do not turn the last known
        // pose (or an identity pose for a never-seen hand) into a tracked controller.
        if (!move.connected || !move.device.tracked || move.device.sequence == 0) {
            return ORBIS_OK;
        }
        result->status = ORBIS_VR_TRACKER_STATUS_TRACKING;
        result->position_quality = ORBIS_VR_TRACKER_QUALITY_FULL;
        result->orientation_quality = ORBIS_VR_TRACKER_QUALITY_FULL;
        result->velocity_x = move.device.linear_velocity.x;
        result->velocity_y = move.device.linear_velocity.y;
        result->velocity_z = move.device.linear_velocity.z;
        result->angular_velocity_x = move.device.angular_velocity.x;
        result->angular_velocity_y = move.device.angular_velocity.y;
        result->angular_velocity_z = move.device.angular_velocity.z;
        // The libSceMove accelerometer includes gravity in device-local g. The public
        // tracker reconstruction names acceleration/angular-acceleration fields without
        // specifying their units or frame; leave them zero rather than assume a contract.
        WritePose(result->move_info.device_pose, move.device.pose);
        return ORBIS_OK;
    }

    // Preserve the existing headset/first-pad tracking path used by Astro Bot.
    if (!HeadsetConnected() || (!is_hmd && !is_pad)) {
        result->status = OrbisVrTrackerStatus::ORBIS_VR_TRACKER_STATUS_NOT_TRACKING;
        return ORBIS_OK;
    }

    auto& runtime = Core::Vr::Runtime::Instance();
    const Core::Vr::DeviceState state = is_hmd ? runtime.GetHead() : runtime.GetPad();

    static std::atomic<u32> result_calls{};
    if (const u32 call = result_calls.fetch_add(1); call < 4 || call % 1200 == 0) {
        LOG_DEBUG(Lib_VrTracker,
                  "call {}: handle = {:#x}, result_type = {}, prediction_time = {}, orientation_type "
                  "= {}, usage_type = {}, position = ({:.3f}, {:.3f}, {:.3f})",
                  call, handle, static_cast<u32>(param->result_type), param->prediction_time,
                  static_cast<u32>(param->orientation_type), static_cast<u32>(param->usage_type),
                  state.pose.position.x, state.pose.position.y, state.pose.position.z);
    }

    // The host already predicts poses for the moment its display lights up, which replaces the
    // prediction the guest asks for here.
    const u64 now = Libraries::Kernel::sceKernelGetProcessTime();
    result->connected = 1;
    result->timestamp = param->prediction_time != 0 ? param->prediction_time : now;
    result->device_timestamp = now;
    result->recalibrate_necessity =
        OrbisVrTrackerRecalibrateNecessityType::ORBIS_VR_TRACKER_RECALIBRATE_NECESSITY_NOTHING;
    result->playarea_brightness_risk =
        OrbisVrTrackerPlayareaBrightnessRiskType::ORBIS_VR_TRACKER_PLAYAREA_BRIGHTNESS_RISK_LOW;
    result->led_color = is_pad ? pad_registration->color
                               : OrbisVrTrackerLedColor::ORBIS_VR_TRACKER_LED_COLOR_BLUE;
    result->status = OrbisVrTrackerStatus::ORBIS_VR_TRACKER_STATUS_TRACKING;
    result->position_quality = OrbisVrTrackerQuality::ORBIS_VR_TRACKER_QUALITY_FULL;
    result->orientation_quality = OrbisVrTrackerQuality::ORBIS_VR_TRACKER_QUALITY_FULL;
    result->velocity_x = state.linear_velocity.x;
    result->velocity_y = state.linear_velocity.y;
    result->velocity_z = state.linear_velocity.z;
    result->angular_velocity_x = state.angular_velocity.x;
    result->angular_velocity_y = state.angular_velocity.y;
    result->angular_velocity_z = state.angular_velocity.z;

    if (is_pad) {
        WritePose(result->pad_info.device_pose, state.pose);
        return ORBIS_OK;
    }

    auto& hmd = result->hmd_info;
    WritePose(hmd.device_pose, state.pose);
    WritePose(hmd.head_pose, state.pose);
    // The eyes sit half the interpupillary distance either side of the headset centre.
    const float half_ipd = runtime.GetConfig().ipd * 0.5f;
    const Core::Vr::Vec3 right = Core::Vr::Rotate(state.pose.orientation, {half_ipd, 0.0f, 0.0f});
    Core::Vr::Pose eye = state.pose;
    eye.position = {state.pose.position.x - right.x, state.pose.position.y - right.y,
                    state.pose.position.z - right.z};
    WritePose(hmd.left_eye_pose, eye);
    eye.position = {state.pose.position.x + right.x, state.pose.position.y + right.y,
                    state.pose.position.z + right.z};
    WritePose(hmd.right_eye_pose, eye);
    hmd.rear_tracking_status =
        OrbisVrTrackerHmdRearTrackingStatus::ORBIS_VR_TRACKER_REAR_TRACKING_READY;
    hmd.sensor_read_system_timestamp = now;

    Core::KnownTitle::NoteView(state.pose.position);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetTime(u64* time) {
    LOG_TRACE(Lib_VrTracker, "called");
    std::scoped_lock lifecycle_lock{g_tracker_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (time == nullptr) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    *time = Libraries::Kernel::sceKernelGetProcessTime();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuSubmit(const OrbisVrTrackerGpuSubmitParam* param) {
    LOG_TRACE(Lib_VrTracker, "called");
    std::scoped_lock lifecycle_lock{g_tracker_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (!HeadsetConnected()) {
        // Impossible to submit valid data here since sceCameraGetFrameData returns an error.
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    if (param == nullptr || param->size != sizeof(OrbisVrTrackerGpuSubmitParam)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // The camera frame would be analysed on the GPU here. The host tracks the devices instead.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuWait(const OrbisVrTrackerGpuWaitParam* param) {
    LOG_TRACE(Lib_VrTracker, "called");
    std::scoped_lock lifecycle_lock{g_tracker_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || param->size != sizeof(OrbisVrTrackerGpuWaitParam)) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    if (HeadsetConnected()) {
        return ORBIS_OK;
    }

    // Impossible to perform GPU submits
    return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_GPU_SUBMIT;
}

s32 PS4_SYSV_ABI sceVrTrackerGpuWaitAndCpuProcess() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    std::scoped_lock lifecycle_lock{g_tracker_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (HeadsetConnected()) {
        return ORBIS_OK;
    }

    // Impossible to perform GPU submits
    return ORBIS_VR_TRACKER_ERROR_NOT_EXECUTE_GPU_SUBMIT;
}

s32 PS4_SYSV_ABI
sceVrTrackerNotifyEndOfCpuProcess(const OrbisVrTrackerNotifyEndOfCpuProcessParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerRecalibrate(const OrbisVrTrackerRecalibrateParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    std::scoped_lock lifecycle_lock{g_tracker_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (param == nullptr || param->size != sizeof(OrbisVrTrackerRecalibrateParam) ||
        param->device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    OrbisVrTrackerDeviceType device_type = param->device_type;
    switch (device_type) {
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_HMD: {
        // Seems like the lack of a connected hmd results in this?
        if (!HeadsetConnected() || g_hmd_handle == -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4: {
        std::scoped_lock lock{g_tracker_mutex};
        if (g_pads.empty()) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_MOVE: {
        if (std::ranges::none_of(g_moves, [](const MoveRegistration& move) {
                return Move::HandForHandle(move.handle).has_value();
            })) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    case OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN: {
        if (g_gun_handle == -1) {
            return ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED;
        }
        break;
    }
    default: {
        // Shouldn't be possible to hit this.
        UNREACHABLE();
    }
    }

    // TODO: handle internal recalibration behaviors.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerResetAll() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerResetOrientationRelative(const OrbisVrTrackerDeviceType device_type,
                                                      const s32 handle) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSaveInternalBuffers() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetDurationUntilStatusNotTracking(
    const OrbisVrTrackerDeviceType device_type, const u32 duration_camera_frames) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    std::scoped_lock lifecycle_lock{g_tracker_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (device_type > OrbisVrTrackerDeviceType::ORBIS_VR_TRACKER_DEVICE_GUN) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    // Seems to unconditionally return 0 when parameters are valid.
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetExtendedMode() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetLEDBrightness() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerSetRestingMode() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI
sceVrTrackerUpdateMotionSensorData(const OrbisVrTrackerUpdateMotionSensorDataParam* param) {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_0FA4C949F8D3024E() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_285C6AFC09C42F7E() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_9A6CDB2103664F8A() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_B4D26B7D8B18DF06(const OrbisVrTrackerDeviceType device_type, const s32 handle,
                                       const s32 led_color) {
    // Undocumented registration variant: the third argument picks the light bar colour the
    // tracker should assign to the device (0..4), instead of leaving the choice to the system.
    LOG_INFO(Lib_VrTracker, "called, device_type = {}, handle = {}, led_color = {}",
             static_cast<u32>(device_type), handle, led_color);
    return sceVrTrackerRegisterDeviceInternal(device_type, handle, led_color, 1);
}

s32 PS4_SYSV_ABI sceVrTrackerSetDeviceRejection() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_1119B0BE399F37E7() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_4928B43816BC440D() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_863EF32EFCB0FA9C() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_E6E726CBC85C48F9() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_F6407E46C66DF383() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuPopMarker() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerCpuPushMarker() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerGetLiveCaptureId() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerStartLiveCapture() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerStopLiveCapture() {
    LOG_ERROR(Lib_VrTracker, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerUnregisterDevice(const s32 handle) {
    LOG_DEBUG(Lib_VrTracker, "called");
    std::scoped_lock lifecycle_lock{g_tracker_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    if (handle < 0) {
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }
    // Since this function only takes a handle, compare the handle to registered handles.
    if (handle == g_hmd_handle) {
        g_hmd_handle = -1;
    } else if (FindPad(handle)) {
        // (The next registered, if any, is the player's from now on.)
        std::scoped_lock lock{g_tracker_mutex};
        std::erase_if(g_pads, [&](const PadRegistration& pad) { return pad.handle == handle; });
    } else if (FindMove(handle)) {
        std::erase_if(g_moves, [&](const MoveRegistration& move) { return move.handle == handle; });
    } else if (handle == g_gun_handle) {
        g_gun_handle = -1;
    } else {
        // If none of the handles match up, then return an error.
        return ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVrTrackerTerm() {
    LOG_DEBUG(Lib_VrTracker, "called");
    std::scoped_lock lifecycle_lock{g_tracker_mutex};
    if (!g_library_initialized) {
        return ORBIS_VR_TRACKER_ERROR_NOT_INIT;
    }
    ClearRegistrations();
    g_garlic_memory_pointer = nullptr;
    g_garlic_size = 0;
    g_onion_memory_pointer = nullptr;
    g_onion_size = 0;
    g_work_memory_pointer = nullptr;
    g_work_size = 0;
    g_library_initialized = false;
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("24kDA+A0Ox0", "libSceVrTrackerFourDeviceAllowed", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDevice2);
    LIB_FUNCTION("5IFOAYv-62g", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerCpuProcess);
    LIB_FUNCTION("zvyKP0Z3UvU", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerGetPlayAreaWarningInfo);
    LIB_FUNCTION("76OBvrrQXUc", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGetResult);
    LIB_FUNCTION("XoeWzXlrnMw", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGetTime);
    LIB_FUNCTION("TVegDMLaBB8", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGpuSubmit);
    LIB_FUNCTION("gkGuO9dd57M", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerGpuWait);
    LIB_FUNCTION("ARhgpXvwoR0", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerGpuWaitAndCpuProcess);
    LIB_FUNCTION("QkRl7pART9M", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerInit);
    LIB_FUNCTION("VItTwN8DmS8", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerNotifyEndOfCpuProcess);
    LIB_FUNCTION("K7yhYrsIBPc", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerQueryMemory);
    LIB_FUNCTION("EUCaQtXXYNI", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerRecalibrate);
    LIB_FUNCTION("sIh8GwcevaQ", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDevice);
    LIB_FUNCTION("ufexf4aNiwg", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerRegisterDeviceInternal);
    LIB_FUNCTION("CtWUbFgmq+I", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerResetAll);
    LIB_FUNCTION("E0P0sN-wy+4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerResetOrientationRelative);
    LIB_FUNCTION("bDGZVTwwZ1A", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSaveInternalBuffers);
    LIB_FUNCTION("qBjnR0HtMYI", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetDurationUntilStatusNotTracking);
    LIB_FUNCTION("NhPkY3V8E+8", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetExtendedMode);
    LIB_FUNCTION("vpsLLotiSUg", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetLEDBrightness);
    LIB_FUNCTION("lgWSHQ8p4i4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerSetRestingMode);
    LIB_FUNCTION("IBv4P3q1pQ0", "libSceVrTracker", 1, "libSceVrTracker", sceVrTrackerTerm);
    LIB_FUNCTION("Q8skQqEwn5c", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerUnregisterDevice);
    LIB_FUNCTION("9fvHMUbsom4", "libSceVrTracker", 1, "libSceVrTracker",
                 sceVrTrackerUpdateMotionSensorData);
    LIB_FUNCTION("D6TJSfjTAk4", "libSceVrTracker", 1, "libSceVrTracker", Func_0FA4C949F8D3024E);
    LIB_FUNCTION("KFxq-AnEL34", "libSceVrTracker", 1, "libSceVrTracker", Func_285C6AFC09C42F7E);
    LIB_FUNCTION("mmzbIQNmT4o", "libSceVrTracker", 1, "libSceVrTracker", Func_9A6CDB2103664F8A);
    LIB_FUNCTION("tNJrfYsY3wY", "libSceVrTracker", 1, "libSceVrTracker", Func_B4D26B7D8B18DF06);
    LIB_FUNCTION("jGqEkPy0iLU", "libSceVrTrackerDeviceRejection", 1, "libSceVrTracker",
                 sceVrTrackerSetDeviceRejection);
    LIB_FUNCTION("ERmwvjmfN+c", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_1119B0BE399F37E7);
    LIB_FUNCTION("SSi0OBa8RA0", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_4928B43816BC440D);
    LIB_FUNCTION("hj7zLvyw+pw", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_863EF32EFCB0FA9C);
    LIB_FUNCTION("5ucmy8hcSPk", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_E6E726CBC85C48F9);
    LIB_FUNCTION("9kB+RsZt84M", "libSceVrTrackerGpuTest", 1, "libSceVrTracker",
                 Func_F6407E46C66DF383);
    LIB_FUNCTION("sBkAqyF5Gns", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerCpuPopMarker);
    LIB_FUNCTION("rvCywCbc7Pk", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerCpuPushMarker);
    LIB_FUNCTION("lm6T1Ur6JRk", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerGetLiveCaptureId);
    LIB_FUNCTION("qa1+CeXKDPc", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerStartLiveCapture);
    LIB_FUNCTION("3YCwwpHkHIg", "libSceVrTrackerLiveCapture", 1, "libSceVrTracker",
                 sceVrTrackerStopLiveCapture);
};

} // namespace Libraries::VrTracker
