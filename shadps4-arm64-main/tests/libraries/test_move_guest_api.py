#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Any4Quest contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile real Move/VrTracker entry points with minimal platform/runtime doubles.

Run directly with Python 3 and a C++23 compiler (CXX, default g++). This checks guest
ABI/lifecycle/routing; it is not a hardware or full-emulator integration test.
If the runner uses ptrace, set ASAN_OPTIONS=detect_leaks=0 (ASan/UBSan still run).
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUBS = {
    "common/logging/log.h": r'''#pragma once
#include <cassert>
#include <bit>
#define LOG_TRACE(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define ASSERT_MSG(condition, ...) assert(condition)
#define UNREACHABLE() assert(false)
''',
    "core/libraries/libs.h": "#pragma once\n#define LIB_FUNCTION(...) ((void)0)\n",
    "core/libraries/kernel/time.h": r'''#pragma once
#include "common/types.h"
namespace Libraries::Kernel { inline u64 sceKernelGetProcessTime() { return 987654; } }
''',
    "core/known_title.h": r'''#pragma once
#include "core/vr/vr_runtime.h"
namespace Core::KnownTitle { inline void NoteView(const Core::Vr::Vec3&) {} }
''',
    "core/memory.h": r'''#pragma once
#include "common/types.h"
namespace Libraries::Kernel { struct OrbisVirtualQueryInfo { VAddr end; }; }
namespace Core { struct Memory {
    static Memory* Instance() { static Memory m; return &m; }
    s32 VirtualQuery(VAddr addr, int, Libraries::Kernel::OrbisVirtualQueryInfo* info) {
        info->end = addr + 0x4000000; return 0;
    }
}; }
''',
    "video_core/amdgpu/liverpool.h": r'''#pragma once
namespace AmdGpu { struct Liverpool {
    static constexpr unsigned NumComputePipes = 7;
    static constexpr unsigned NumQueuesPerPipe = 8;
}; }
''',
}

TEST = r'''
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
#include "core/vr/vr_runtime.h"
#include "core/libraries/move/move.h"
#include "core/libraries/move/move_error.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/libraries/vr_tracker/vr_tracker_error.h"

namespace Fixture {
std::array<Core::Vr::MoveState, 2> samples{};
std::array<std::vector<Core::Vr::MoveState>, 2> history;
std::array<Core::Vr::MoveFeedback, 2> feedback{};
}
namespace Core::Vr {
Runtime::Runtime() { config.headset_connected = true; }
Runtime& Runtime::Instance() { static Runtime r; return r; }
MoveState Runtime::GetMove(u32 hand) { return Fixture::samples.at(hand); }
u32 Runtime::ReadMoveRecent(u32 hand, u64 after, MoveState* out, u32 capacity) {
    u32 count = 0;
    for (const auto& s : Fixture::history.at(hand)) {
        if (s.timestamp_us > after && count < capacity) out[count++] = s;
    }
    return count;
}
void Runtime::SetMoveVibration(u32 hand, u8 value) { Fixture::feedback.at(hand).intensity = value; }
void Runtime::SetMoveLight(u32 hand, u8 r, u8 g, u8 b) {
    auto& f = Fixture::feedback.at(hand); f.red = r; f.green = g; f.blue = b;
}
void Runtime::SetPadLight(u8, u8, u8) {}
DeviceState Runtime::GetHead() { DeviceState d{}; d.pose.position.y = 1.6f; return d; }
DeviceState Runtime::GetPad() { DeviceState d{}; d.pose.position.z = 0.7f; return d; }
Vec3 Rotate(const Quat&, const Vec3& v) { return v; }
}

int main() {
    using namespace Libraries::Move;
    using namespace Libraries::VrTracker;
    OrbisMoveData latest{};
    assert(sceMoveOpen(1, 0, 0) == ORBIS_MOVE_ERROR_NOT_INIT);
    assert(sceMoveInit() == 0);
    assert(sceMoveInit() == ORBIS_MOVE_ERROR_ALREADY_INIT);
    assert(sceMoveOpen(-1, 0, 0) == ORBIS_MOVE_ERROR_INVALID_ARG);
    assert(sceMoveOpen(1, 1, 0) == ORBIS_MOVE_ERROR_INVALID_PORT);
    assert(sceMoveOpen(1, 0, 2) == ORBIS_MOVE_ERROR_INVALID_ARG);
    const s32 right = sceMoveOpen(1, 0, 1);  // Deliberately open in reverse hand order.
    const s32 left = sceMoveOpen(1, 0, 0);
    assert(left >= 0 && right >= 0 && left != right);
    assert(HandForHandle(left) == 0 && HandForHandle(right) == 1);
    assert(sceMoveOpen(1, 0, 0) == ORBIS_MOVE_ERROR_ALREADY_OPENED);
    assert(sceMoveOpen(2, 0, 0) == ORBIS_MOVE_ERROR_MAX_CONTROLLERS_EXCEEDED);
    assert(sceMoveReadStateLatest(-123, &latest) == ORBIS_MOVE_ERROR_INVALID_HANDLE);
    assert(sceMoveReadStateLatest(left, nullptr) == ORBIS_MOVE_ERROR_INVALID_ARG);
    assert(sceMoveReadStateLatest(left, &latest) == ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED);
    assert(latest.timestamp == 0 && latest.button_data.button_data == 0);
    assert(sceMoveResetLightSphere(left) == 0);

    auto& sample = Fixture::samples[0];
    sample.connected = true;
    sample.device.sequence = 7;
    sample.timestamp_us = 4200;
    sample.buttons = ORBIS_MOVE_BUTTON_MOVE | ORBIS_MOVE_BUTTON_T | ORBIS_MOVE_BUTTON_CROSS;
    sample.trigger = 173;
    sample.acceleration = {0.25f, 1.0f, -0.5f};
    sample.gyro = {-1.0f, 2.0f, 3.0f};
    assert(sceMoveReadStateLatest(left, &latest) == 0);
    assert(latest.timestamp == 4200 && latest.count == 7);
    assert(latest.button_data.button_data == 0x46 && latest.button_data.trigger_data == 173);
    assert(latest.accelerometer[0] == .25f && latest.gyro[2] == 3.0f);
    OrbisMoveData same{};
    assert(sceMoveReadStateLatest(left, &same) == 0);
    assert(std::memcmp(&same, &latest, sizeof(same)) == 0); // Polling does not invent samples.
    OrbisMoveDeviceInfo info{};
    assert(sceMoveGetDeviceInfo(left, &info) == 0 && info.sphere_radius == 22.5f);
    OrbisMoveExtensionPortInfo extension;
    std::memset(&extension, 0xff, sizeof(extension));
    assert(sceMoveGetExtensionPortInfo(left, &extension) == 0);
    assert(extension.extension_port_id == 0 && extension.device_info[37] == 0);

    for (u32 i = 1; i <= 32; ++i) {
        auto past = sample; past.timestamp_us = i * 100; past.device.sequence = i;
        Fixture::history[0].push_back(past);
    }
    struct { std::array<OrbisMoveData, 32> records; u64 guard = 0x5a5a5a5a; } output;
    s32 count = 99;
    assert(sceMoveReadStateRecent(left, 0, output.records.data(), &count) == 0 && count == 32);
    assert(output.records[0].timestamp == 100 && output.records[31].timestamp == 3200);
    assert(output.guard == 0x5a5a5a5a);
    assert(sceMoveReadStateRecent(left, 3100, output.records.data(), &count) == 0 && count == 1);
    assert(output.records[0].timestamp == 3200);
    assert(sceMoveReadStateRecent(left, 3200, output.records.data(), &count) == 0 && count == 0);
    assert(sceMoveReadStateRecent(left, -1, output.records.data(), &count) == ORBIS_MOVE_ERROR_INVALID_ARG);
    assert(count == 0);
    sample.device.sequence = static_cast<u64>(std::numeric_limits<s32>::max()) + 1;
    assert(sceMoveReadStateLatest(left, &latest) == 0 && latest.count == 1);
    sample.device.sequence = 7;

    assert(sceMoveSetVibration(left, 90) == 0 && Fixture::feedback[0].intensity == 90);
    assert(Fixture::feedback[1].intensity == 0);
    assert(sceMoveSetLightSphere(left, 1, 2, 3) == 0 && Fixture::feedback[0].blue == 3);

    assert(sceVrTrackerInit(nullptr) == ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
    OrbisVrTrackerInitParam init{};
    init.size = sizeof(init);
    init.direct_memory_garlic = reinterpret_cast<void*>(0x10000000);
    init.direct_memory_onion = reinterpret_cast<void*>(0x20000000);
    init.work_memory = reinterpret_cast<void*>(0x30000000);
    init.direct_memory_garlic_size = ORBIS_VR_TRACKER_GARLIC_SIZE;
    init.direct_memory_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    init.work_memory_size = ORBIS_VR_TRACKER_WORK_SIZE;
    init.direct_memory_garlic_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    init.direct_memory_onion_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    init.work_memory_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    assert(sceVrTrackerInit(&init) == 0);
    assert(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_MOVE, 123) == ORBIS_VR_TRACKER_ERROR_INVALID_DEVICE_HANDLE);
    assert(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_MOVE, left) == 0);
    assert(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_MOVE, right) == 0);
    assert(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_MOVE, left) == ORBIS_VR_TRACKER_ERROR_DEVICE_ALREADY_REGISTERED);
    OrbisVrTrackerGetResultParam param{};
    param.size = sizeof(param); param.handle = left; param.user_frame_number = 5;
    OrbisVrTrackerResultData result{};
    assert(sceVrTrackerGetResult(&param, &result) == 0);
    assert(result.connected == 1 && result.status == ORBIS_VR_TRACKER_STATUS_NOT_TRACKING);
    assert(result.position_quality == ORBIS_VR_TRACKER_QUALITY_NONE);
    sample.device.tracked = true;
    sample.device.pose.position = {-0.3f, 1.0f, 1.5f};
    sample.device.linear_velocity = {1, 2, 3}; sample.device.angular_velocity = {4, 5, 6};
    Fixture::samples[1] = sample; Fixture::samples[1].device.pose.position.x = 0.3f;
    assert(sceVrTrackerGetResult(&param, &result) == 0);
    assert(result.connected == 1 && result.status == ORBIS_VR_TRACKER_STATUS_TRACKING);
    assert(result.move_info.device_pose.position_x == -0.3f && result.device_timestamp == 4200);
    assert(result.velocity_x == 1 && result.angular_velocity_z == 6 && result.user_frame_number == 5);
    param.handle = right;
    assert(sceVrTrackerGetResult(&param, &result) == 0 && result.move_info.device_pose.position_x == .3f);
    Fixture::samples[1].connected = false;
    assert(sceVrTrackerGetResult(&param, &result) == 0 && result.connected == 0);
    assert(result.status == ORBIS_VR_TRACKER_STATUS_NOT_TRACKING && result.position_quality == 0);

    // Existing HMD and first-registered pad remain on their own result path.
    assert(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_HMD, 100) == 0);
    assert(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4, 200) == 0);
    assert(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_DUALSHOCK4, 201) == 0);
    param.handle = 200;
    assert(sceVrTrackerGetResult(&param, &result) == 0 && result.pad_info.device_pose.position_z == .7f);
    param.handle = 201;
    assert(sceVrTrackerGetResult(&param, &result) == 0 && result.status == ORBIS_VR_TRACKER_STATUS_NOT_TRACKING);
    param.handle = 100;
    assert(sceVrTrackerGetResult(&param, &result) == 0 && result.hmd_info.device_pose.position_y == 1.6f);

    assert(sceMoveClose(left) == 0 && Fixture::feedback[0].intensity == 0);
    assert(!HandForHandle(left));
    assert(sceMoveClose(left) == ORBIS_MOVE_ERROR_INVALID_HANDLE);
    param.handle = left;
    assert(sceVrTrackerGetResult(&param, &result) == ORBIS_VR_TRACKER_ERROR_INVALID_DEVICE_HANDLE);
    s32 reopened = sceMoveOpen(1, 0, 0);
    assert(reopened >= 0 && reopened != left && reopened != right);
    assert(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_MOVE, reopened) == 0);
    assert(sceVrTrackerUnregisterDevice(right) == 0);
    assert(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_MOVE, right) == 0);
    assert(sceVrTrackerTerm() == 0);
    assert(sceVrTrackerInit(&init) == 0);
    param.handle = right;
    assert(sceVrTrackerGetResult(&param, &result) == ORBIS_VR_TRACKER_ERROR_DEVICE_NOT_REGISTERED);
    assert(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_MOVE, right) == 0);
    assert(sceVrTrackerTerm() == 0);

    std::thread reader([&] {
        for (int i = 0; i < 1000; ++i) {
            OrbisMoveData out{};
            const auto code = sceMoveReadStateLatest(reopened, &out);
            assert(code == 0 || code == ORBIS_MOVE_ERROR_INVALID_HANDLE);
        }
    });
    assert(sceMoveClose(reopened) == 0);
    reader.join();
    assert(sceMoveTerm() == 0 && !HandForHandle(right));
    assert(sceMoveReadStateLatest(right, &latest) == ORBIS_MOVE_ERROR_NOT_INIT);
    assert(sceMoveInit() == 0);
    assert(sceMoveReadStateLatest(right, &latest) == ORBIS_MOVE_ERROR_INVALID_HANDLE);
    assert(sceMoveOpen(1, 0, 1) != right);
    assert(sceMoveTerm() == 0);
    std::cout << "PASS: Move ABI, lifecycle, 32-sample history, feedback, dual tracker routing, "
                 "tracking loss, stale handles, concurrent close/read, HMD/pad regression\n";
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="move-guest-api-") as tmp:
        work = Path(tmp)
        for path, content in STUBS.items():
            target = work / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
        test = work / "test.cpp"
        test.write_text(TEST)
        binary = work / "guest_test"
        subprocess.run([
            os.environ.get("CXX", "g++"), "-std=c++23", "-O1", "-g", "-pthread",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-Wno-unused-parameter",
            "-Wno-missing-field-initializers",  # Existing camera ABI header initializers
            "-I" + str(work), "-I" + str(ROOT / "src"), str(test),
            str(ROOT / "src/core/libraries/move/move.cpp"),
            str(ROOT / "src/core/libraries/vr_tracker/vr_tracker.cpp"), "-o", str(binary)
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
