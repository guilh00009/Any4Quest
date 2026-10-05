// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>

#include "common/types.h"
#include "core/libraries/system/userservice.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Move {

// Public PS4 reconstruction (button masks, geometry units and extension-port layout):
// https://github.com/nekohaku/REMovePS4/blob/7ffbb45bb0d9aa48a8906ebccf735b7212f6f0da/remove/scemove.h
// That project's remove_ctx.c returns up to 32 chronological samples; also documented by
// https://github.com/red-prig/fpPS4/blob/04cefd43e6fddd1ab033e7980cd356d14c964905/src/ps4_libscemove.pas
constexpr u32 ORBIS_MOVE_MAX_STATE_NUM = 32;

enum OrbisMoveButton : u16 {
    ORBIS_MOVE_BUTTON_SELECT = 1 << 0,
    ORBIS_MOVE_BUTTON_T = 1 << 1,
    ORBIS_MOVE_BUTTON_MOVE = 1 << 2,
    ORBIS_MOVE_BUTTON_START = 1 << 3,
    ORBIS_MOVE_BUTTON_TRIANGLE = 1 << 4,
    ORBIS_MOVE_BUTTON_CIRCLE = 1 << 5,
    ORBIS_MOVE_BUTTON_CROSS = 1 << 6,
    ORBIS_MOVE_BUTTON_SQUARE = 1 << 7,
    ORBIS_MOVE_BUTTON_INTERCEPTED = 1 << 15,
};

struct OrbisMoveDeviceInfo {
    float sphere_radius;           // Millimetres, unlike VrTracker's metre-space poses.
    float accelerometer_offset[3];  // Accelerometer-to-sphere offset in millimetres.
};

struct OrbisMoveExtensionPortInfo {
    u32 extension_port_id;
    u8 device_info[38];
};

struct OrbisMoveButtonData {
    u16 button_data;
    u16 trigger_data;
};

struct OrbisMoveExtensionPortData {
    u16 status;
    u16 digital0;
    u16 digital1;
    u16 analog_right_x;
    u16 analog_right_y;
    u16 analog_left_x;
    u16 analog_left_y;
    unsigned char custom[5];
};

struct OrbisMoveData {
    float accelerometer[3];
    float gyro[3];
    OrbisMoveButtonData button_data;
    OrbisMoveExtensionPortData extension_data;
    s64 timestamp;
    s32 count;
    float temperature;
};

// Keep the guest ABI independent of the host compiler and of the wire protocol.
static_assert(sizeof(OrbisMoveDeviceInfo) == 16);
static_assert(sizeof(OrbisMoveExtensionPortInfo) == 44);
static_assert(sizeof(OrbisMoveButtonData) == 4);
static_assert(sizeof(OrbisMoveExtensionPortData) == 20);
static_assert(sizeof(OrbisMoveData) == 64);
static_assert(offsetof(OrbisMoveData, button_data) == 24);
static_assert(offsetof(OrbisMoveData, timestamp) == 48);
static_assert(offsetof(OrbisMoveData, count) == 56);

/// Resolve a currently open handle to the host's fixed left (0) / right (1) hand.
/// Closed handles, including handles from a previous initialization, never resolve.
std::optional<u32> HandForHandle(s32 handle);

s32 PS4_SYSV_ABI sceMoveInit();
s32 PS4_SYSV_ABI sceMoveOpen(UserService::OrbisUserServiceUserId user_id, s32 type, s32 index);
s32 PS4_SYSV_ABI sceMoveGetDeviceInfo(s32 handle, OrbisMoveDeviceInfo* info);
s32 PS4_SYSV_ABI sceMoveReadStateLatest(s32 handle, OrbisMoveData* data);
s32 PS4_SYSV_ABI sceMoveReadStateRecent(s32 handle, s64 timestamp, OrbisMoveData* data,
                                      s32* out_count);
s32 PS4_SYSV_ABI sceMoveGetExtensionPortInfo(s32 handle, OrbisMoveExtensionPortInfo* data);
s32 PS4_SYSV_ABI sceMoveSetVibration(s32 handle, u8 intensity);
s32 PS4_SYSV_ABI sceMoveSetLightSphere(s32 handle, u8 red, u8 green, u8 blue);
s32 PS4_SYSV_ABI sceMoveResetLightSphere(s32 handle);
s32 PS4_SYSV_ABI sceMoveClose(s32 handle);
s32 PS4_SYSV_ABI sceMoveTerm();

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Move
