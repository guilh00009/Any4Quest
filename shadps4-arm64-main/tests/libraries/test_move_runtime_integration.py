#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Any4Quest contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real vr_runtime.cpp -> real libSceMove/libSceVrTracker integration checks.

Platform/log/memory/host-transport dependencies only are replaced. Uses the pinned
nlohmann submodule. Run with ASAN_OPTIONS=detect_leaks=0 under ptrace runners.
No headset or game compatibility is asserted by these tests.
"""
from pathlib import Path
import os
import subprocess
import tempfile
from test_move_guest_api import ROOT, STUBS

TEST = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include "core/vr/vr_runtime.h"
#include "core/vr/vr_move_input.h"
#include "core/libraries/move/move.h"
#include "core/libraries/move/move_error.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/libraries/vr_tracker/vr_tracker_error.h"

static std::atomic<u64> process_time{1000000};
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return process_time.load(); }
}
static bool Near(float a, float b) { return std::abs(a - b) < .001f; }
static void Mode(const char* value) {
    setenv("SHADPS4_VR_INPUT_MODE", value, 1);
    setenv("SHADPS4_VR", "0", 1);
    Core::Vr::Runtime::Instance().Configure(false, false);
}

int main() {
    using namespace Core::Vr;
    using namespace Libraries::Move;
    using namespace Libraries::VrTracker;
    // PS4 reconstructed ABI masks, not the unrelated raw HID report bit positions.
    static_assert(ORBIS_MOVE_BUTTON_MOVE == MoveInput::Move);
    static_assert(ORBIS_MOVE_BUTTON_T == MoveInput::Trigger);
    static_assert(ORBIS_MOVE_BUTTON_CROSS == MoveInput::Cross);
    static_assert(ORBIS_MOVE_BUTTON_TRIANGLE == MoveInput::Triangle);
    auto& runtime = Runtime::Instance();
    Mode("gamepad");
    assert(sceMoveInit() == 0);
    const s32 left = sceMoveOpen(1, 0, 0), right = sceMoveOpen(1, 0, 1);
    OrbisMoveData data{}, previous{};
    MoveHostState host{};
    host.connected = host.device.tracked = true;
    host.device.pose.position = {-.25f, -.2f, -.5f};
    host.buttons = MoveInput::MapTouchButtons({.primary = true, .squeeze = 1.0f});
    host.trigger = .5f;
    runtime.UpdateMove(0, host);
    assert(sceMoveReadStateLatest(left, &data) == ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED);
    Mode("move");
    runtime.FixSeat();
    runtime.UpdateMove(0, host);
    assert(sceMoveReadStateLatest(left, &data) == 0);
    assert(data.timestamp == 1000000 && data.count > 0);
    assert(data.button_data.button_data == (ORBIS_MOVE_BUTTON_MOVE | ORBIS_MOVE_BUTTON_T | ORBIS_MOVE_BUTTON_CROSS));
    assert(data.button_data.trigger_data == 128 && Near(data.accelerometer[1], 1.0f));
    previous = data;
    assert(sceMoveReadStateLatest(left, &data) == 0 && std::memcmp(&previous, &data, sizeof(data)) == 0);
    assert(sceMoveReadStateLatest(right, &data) == ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED);
    host.device.pose.position.x = .25f;
    process_time += 10000;
    runtime.UpdateMove(1, host);

    OrbisVrTrackerInitParam init{};
    init.size = sizeof(init);
    init.direct_memory_garlic = reinterpret_cast<void*>(0x10000000);
    init.direct_memory_onion = reinterpret_cast<void*>(0x20000000);
    init.work_memory = reinterpret_cast<void*>(0x30000000);
    init.direct_memory_garlic_size = ORBIS_VR_TRACKER_GARLIC_SIZE;
    init.direct_memory_onion_size = ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    init.work_memory_size = ORBIS_VR_TRACKER_WORK_SIZE;
    init.direct_memory_garlic_alignment = init.direct_memory_onion_alignment =
        init.work_memory_alignment = ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    assert(sceVrTrackerInit(&init) == 0);
    assert(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_MOVE, right) == 0);
    assert(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_MOVE, left) == 0);
    OrbisVrTrackerGetResultParam param{};
    param.size = sizeof(param);
    param.handle = left;
    OrbisVrTrackerResultData result{};
    assert(sceVrTrackerGetResult(&param, &result) == 0);
    assert(result.connected == 1 && result.status == ORBIS_VR_TRACKER_STATUS_TRACKING);
    assert(Near(result.move_info.device_pose.position_x, -.25f));
    assert(Near(result.move_info.device_pose.position_z, 1.0f));
    assert(result.device_timestamp == static_cast<u64>(previous.timestamp));
    param.handle = right;
    assert(sceVrTrackerGetResult(&param, &result) == 0 && Near(result.move_info.device_pose.position_x, .25f));

    OrbisMoveData recent[32]{};
    s32 count{};
    assert(sceMoveReadStateRecent(left, previous.timestamp, recent, &count) == 0 && count == 0);
    for (u32 i = 0; i < 40; ++i) {
        process_time += 10000;
        runtime.UpdateMove(0, host);
    }
    assert(sceMoveReadStateRecent(left, previous.timestamp, recent, &count) == 0 && count == 32);
    for (s32 i = 1; i < count; ++i) assert(recent[i].timestamp > recent[i-1].timestamp && recent[i].count > recent[i-1].count);
    assert(sceMoveReadStateLatest(left, &data) == 0);
    assert(data.timestamp == recent[31].timestamp && data.count == recent[31].count);
    previous = data;

    // A recenter moves the tracker coordinate frame, not raw local IMU or sample times.
    DeviceState head{};
    head.tracked = true;
    head.pose.position = {1, 2, 3};
    head.pose.orientation = FromYawPitch(1.57079632679f, 0);
    runtime.UpdateHead(head);
    runtime.RecenterSeat();
    host.device.pose.position = {2, 2, 3};
    host.device.pose.orientation = head.pose.orientation;
    host.linear_velocity_valid = host.angular_velocity_valid = true;
    host.device.linear_velocity = {1, 0, 0};
    host.device.angular_velocity = {0, 1, 0};
    process_time += 10000;
    runtime.UpdateMove(0, host);
    param.handle = left;
    assert(sceVrTrackerGetResult(&param, &result) == 0);
    assert(Near(result.move_info.device_pose.position_x, 0) && Near(result.move_info.device_pose.position_z, 2.5f));
    assert(Near(result.velocity_z, 1) && Near(result.angular_velocity_y, 1));
    assert(sceMoveReadStateLatest(left, &previous) == 0);
    head.pose.position.x = 2;
    runtime.UpdateHead(head);
    runtime.RecenterSeat();
    assert(sceVrTrackerGetResult(&param, &result) == 0 && Near(result.move_info.device_pose.position_z, 1.5f));
    assert(result.device_timestamp == static_cast<u64>(previous.timestamp));
    assert(sceMoveReadStateLatest(left, &data) == 0 && std::memcmp(&previous, &data, sizeof(data)) == 0);

    assert(sceMoveSetVibration(left, 123) == 0 && runtime.GetMoveFeedback(0).intensity == 123);
    assert(runtime.GetMoveFeedback(1).intensity == 0);
    host.device.tracked = false;
    process_time += 10000;
    runtime.UpdateMove(0, host);
    assert(sceMoveReadStateLatest(left, &data) == 0 && data.button_data.trigger_data == 128);
    assert(sceVrTrackerGetResult(&param, &result) == 0 && result.connected == 1);
    assert(result.status == ORBIS_VR_TRACKER_STATUS_NOT_TRACKING && result.position_quality == 0);
    assert(runtime.GetMoveFeedback(0).intensity == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds{275});
    assert(sceMoveReadStateLatest(left, &data) == ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED);
    count = 99;
    assert(sceMoveReadStateRecent(left, 0, recent, &count) == ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED && count == 0);
    assert(sceVrTrackerGetResult(&param, &result) == 0 && result.connected == 0);

    host.device.tracked = true;
    process_time += 10000;
    runtime.UpdateMove(0, host);
    assert(sceMoveReadStateLatest(left, &data) == 0 && data.timestamp > previous.timestamp);
    assert(sceMoveReadStateRecent(left, 0, recent, &count) == 0 && count == 1); // Expired history stays cleared.
    assert(sceMoveSetVibration(left, 55) == 0);
    assert(sceMoveClose(left) == 0 && runtime.GetMoveFeedback(0).intensity == 0);
    assert(sceVrTrackerGetResult(&param, &result) == ORBIS_VR_TRACKER_ERROR_INVALID_DEVICE_HANDLE);
    assert(sceMoveTerm() == 0 && sceVrTrackerTerm() == 0);
    assert(sceMoveInit() == 0 && sceMoveReadStateLatest(right, &data) == ORBIS_MOVE_ERROR_INVALID_HANDLE);
    assert(sceMoveTerm() == 0);
    Mode("gamepad");
    assert(!runtime.GetMove(0).connected);
    std::cout << "PASS: real host state -> runtime -> Move/VrTracker; input masks, stable timestamps, "
                 "32-sample history, dual hands, recenter, feedback, tracking loss/staleness, lifecycle\n";
}
'''


def main():
    stubs = dict(STUBS)
    stubs["core/libraries/kernel/time.h"] = '''#pragma once
#include "common/types.h"
namespace Libraries::Kernel { u64 PS4_SYSV_ABI sceKernelGetProcessTime(); }
'''
    json_include = ROOT / "externals/json/single_include"
    if not (json_include / "nlohmann/json.hpp").is_file():
        raise SystemExit("Missing pinned nlohmann JSON submodule")
    with tempfile.TemporaryDirectory(prefix="move-runtime-integration-") as tmp:
        work = Path(tmp)
        for path, content in stubs.items():
            target = work / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
        test = work / "test.cpp"
        test.write_text(TEST)
        binary = work / "integration_test"
        subprocess.run([
            os.environ.get("CXX", "g++"), "-std=c++23", "-O1", "-g", "-pthread",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
            "-I" + str(work), "-I" + str(ROOT.parent / "AI_Debug/tests/any4quest_stubs"),
            "-I" + str(ROOT / "src"), "-I" + str(json_include), str(test),
            str(ROOT / "src/core/libraries/move/move.cpp"),
            str(ROOT / "src/core/libraries/vr_tracker/vr_tracker.cpp"),
            str(ROOT / "src/core/vr/vr_runtime.cpp"), "-o", str(binary)
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
