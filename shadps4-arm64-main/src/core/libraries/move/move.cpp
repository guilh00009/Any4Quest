// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/move/move.h"
#include "core/libraries/move/move_error.h"
#include "core/vr/vr_runtime.h"

namespace Libraries::Move {
namespace {

std::mutex g_mutex;
bool g_library_initialized{};
struct OpenDevice {
    s32 handle{-1};
    UserService::OrbisUserServiceUserId user_id{};
};
std::array<OpenDevice, 2> g_devices;
// Preserve the Move handle namespace used by the original stub. Do not recycle a closed
// handle: a VrTracker registration must not silently attach to a newly opened device.
u32 g_next_handle = 0x30b0100;

std::optional<u32> FindHandLocked(s32 handle) {
    for (u32 hand = 0; hand < g_devices.size(); ++hand) {
        if (g_devices[hand].handle == handle && handle >= 0) {
            return hand;
        }
    }
    return std::nullopt;
}

void WriteSample(OrbisMoveData& out, const Core::Vr::MoveState& sample) {
    std::memset(&out, 0, sizeof(out));
    out.accelerometer[0] = sample.acceleration.x;
    out.accelerometer[1] = sample.acceleration.y;
    out.accelerometer[2] = sample.acceleration.z;
    out.gyro[0] = sample.gyro.x;
    out.gyro[1] = sample.gyro.y;
    out.gyro[2] = sample.gyro.z;
    out.button_data.button_data = sample.buttons;
    out.button_data.trigger_data = std::min<u16>(sample.trigger, 255);
    out.timestamp = static_cast<s64>(sample.timestamp_us);
    // This is a sensor sample counter, not the number of times the title polled it.
    out.count = sample.device.sequence == 0 ? 0 : static_cast<s32>(
        (sample.device.sequence - 1) % static_cast<u64>(std::numeric_limits<s32>::max()) + 1);
    // No extension or temperature sensor is exposed by the virtual device.
}

} // namespace

std::optional<u32> HandForHandle(s32 handle) {
    std::scoped_lock lock{g_mutex};
    return g_library_initialized ? FindHandLocked(handle) : std::nullopt;
}

s32 PS4_SYSV_ABI sceMoveInit() {
    std::scoped_lock lock{g_mutex};
    if (g_library_initialized) {
        return ORBIS_MOVE_ERROR_ALREADY_INIT;
    }
    g_devices = {};
    g_library_initialized = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveOpen(UserService::OrbisUserServiceUserId user_id, s32 type, s32 index) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (user_id < 0) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    // Normal Move ports, with stable left/right assignment independent of open order.
    if (type != 0) {
        return ORBIS_MOVE_ERROR_INVALID_PORT;
    }
    if (index < 0 || index >= static_cast<s32>(g_devices.size())) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    auto& device = g_devices[index];
    if (device.handle != -1) {
        return device.user_id == user_id ? ORBIS_MOVE_ERROR_ALREADY_OPENED
                                         : ORBIS_MOVE_ERROR_MAX_CONTROLLERS_EXCEEDED;
    }
    if (g_next_handle > static_cast<u32>(std::numeric_limits<s32>::max()) - 0x100) {
        return ORBIS_MOVE_ERROR_MAX_CONTROLLERS_EXCEEDED;
    }
    device = {static_cast<s32>(g_next_handle), user_id};
    g_next_handle += 0x100;
    // Opening a disconnected device is supported; reads report its actual connection state.
    return device.handle;
}

s32 PS4_SYSV_ABI sceMoveGetDeviceInfo(s32 handle, OrbisMoveDeviceInfo* info) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (info == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    const auto hand = FindHandLocked(handle);
    if (!hand) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    std::memset(info, 0, sizeof(*info));
    if (!Core::Vr::Runtime::Instance().GetMove(*hand).connected) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    // Device info is in millimetres. The virtual sensor/sphere centre is the host grip
    // origin; no unmeasured physical accelerometer-to-sphere offset is introduced.
    info->sphere_radius = 22.5f;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveReadStateLatest(s32 handle, OrbisMoveData* data) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    const auto hand = FindHandLocked(handle);
    if (!hand) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    std::memset(data, 0, sizeof(*data));
    const auto sample = Core::Vr::Runtime::Instance().GetMove(*hand);
    if (!sample.connected || sample.device.sequence == 0) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    WriteSample(*data, sample);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveReadStateRecent(s32 handle, s64 timestamp, OrbisMoveData* data,
                                      s32* out_count) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (out_count != nullptr) {
        *out_count = 0;
    }
    if (timestamp < 0 || data == nullptr || out_count == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    const auto hand = FindHandLocked(handle);
    if (!hand) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    auto& runtime = Core::Vr::Runtime::Instance();
    if (!runtime.GetMove(*hand).connected) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    // sceMoveReadStateRecent requires storage for ORBIS_MOVE_MAX_STATE_NUM records.
    // Samples are returned oldest first and strictly newer than the supplied timestamp.
    std::array<Core::Vr::MoveState, ORBIS_MOVE_MAX_STATE_NUM> samples{};
    const u32 count = runtime.ReadMoveRecent(*hand, static_cast<u64>(timestamp), samples.data(),
                                            static_cast<u32>(samples.size()));
    for (u32 i = 0; i < count; ++i) {
        WriteSample(data[i], samples[i]);
    }
    *out_count = static_cast<s32>(count);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveGetExtensionPortInfo(s32 handle, OrbisMoveExtensionPortInfo* data) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    if (data == nullptr) {
        return ORBIS_MOVE_ERROR_INVALID_ARG;
    }
    const auto hand = FindHandLocked(handle);
    if (!hand) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    if (!Core::Vr::Runtime::Instance().GetMove(*hand).connected) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    // No peripheral attached to the deprecated Move extension port.
    std::memset(data, 0, sizeof(*data));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveSetVibration(s32 handle, u8 intensity) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    const auto hand = FindHandLocked(handle);
    if (!hand) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    auto& runtime = Core::Vr::Runtime::Instance();
    if (!runtime.GetMove(*hand).connected) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    runtime.SetMoveVibration(*hand, intensity);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveSetLightSphere(s32 handle, u8 red, u8 green, u8 blue) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    const auto hand = FindHandLocked(handle);
    if (!hand) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    auto& runtime = Core::Vr::Runtime::Instance();
    if (!runtime.GetMove(*hand).connected) {
        return ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED;
    }
    runtime.SetMoveLight(*hand, red, green, blue);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveResetLightSphere(s32 handle) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    const auto hand = FindHandLocked(handle);
    if (!hand) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    // As on the original implementation, resetting succeeds even while disconnected.
    Core::Vr::Runtime::Instance().SetMoveLight(*hand, 0, 0, 0);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveClose(s32 handle) {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    const auto hand = FindHandLocked(handle);
    if (!hand) {
        return ORBIS_MOVE_ERROR_INVALID_HANDLE;
    }
    auto& runtime = Core::Vr::Runtime::Instance();
    runtime.SetMoveVibration(*hand, 0);
    runtime.SetMoveLight(*hand, 0, 0, 0);
    g_devices[*hand] = {};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceMoveTerm() {
    std::scoped_lock lock{g_mutex};
    if (!g_library_initialized) {
        return ORBIS_MOVE_ERROR_NOT_INIT;
    }
    auto& runtime = Core::Vr::Runtime::Instance();
    for (u32 hand = 0; hand < g_devices.size(); ++hand) {
        runtime.SetMoveVibration(hand, 0);
        runtime.SetMoveLight(hand, 0, 0, 0);
    }
    g_devices = {};
    g_library_initialized = false;
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("j1ITE-EoJmE", "libSceMove", 1, "libSceMove", sceMoveInit);
    LIB_FUNCTION("HzC60MfjJxU", "libSceMove", 1, "libSceMove", sceMoveOpen);
    LIB_FUNCTION("GWXTyxs4QbE", "libSceMove", 1, "libSceMove", sceMoveGetDeviceInfo);
    LIB_FUNCTION("ttU+JOhShl4", "libSceMove", 1, "libSceMove", sceMoveReadStateLatest);
    LIB_FUNCTION("f2bcpK6kJfg", "libSceMove", 1, "libSceMove", sceMoveReadStateRecent);
    LIB_FUNCTION("y5h7f8H1Jnk", "libSceMove", 1, "libSceMove", sceMoveGetExtensionPortInfo);
    LIB_FUNCTION("IFQwtT2CeY0", "libSceMove", 1, "libSceMove", sceMoveSetVibration);
    LIB_FUNCTION("T8KYHPs1JE8", "libSceMove", 1, "libSceMove", sceMoveSetLightSphere);
    LIB_FUNCTION("zuxWAg3HAac", "libSceMove", 1, "libSceMove", sceMoveResetLightSphere);
    LIB_FUNCTION("XX6wlxpHyeo", "libSceMove", 1, "libSceMove", sceMoveClose);
    LIB_FUNCTION("tsZi60H4ypY", "libSceMove", 1, "libSceMove", sceMoveTerm);
};

} // namespace Libraries::Move
