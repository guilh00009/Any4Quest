// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include "core/libraries/hmd/wide_near_abi.h"
#include "core/libraries/hmd/wide_near_preview.h"
#include "core/libraries/hmd/wide_near_vr.h"
#include <mutex>

#include "common/logging/log.h"
#include "core/memory.h"
#include "core/libraries/hmd/reprojection_2d.h"
#include "core/guest_cpu/guest_watchdog.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_error.h"
#include "core/libraries/libs.h"
#include "core/libraries/videoout/video_out.h"
#include "core/vr/vr_runtime.h"

namespace Libraries::Hmd {

namespace {

struct UserEvent {
    Libraries::Kernel::OrbisKernelEqueue eq{};
    s32 id{};
    bool is_set{};

    void Trigger() const {
        if (!is_set) {
            return;
        }
        if (auto* equeue = Libraries::Kernel::GetEqueue(eq); equeue != nullptr) {
            equeue->TriggerEvent(id, Libraries::Kernel::OrbisKernelEvent::Filter::User, nullptr);
        }
    }
};

// On real hardware a system thread warps the latest submitted frame to the current head pose
// and scans it out on every panel refresh. Here the host compositor does the warping, so this
// only has to keep the guest-visible timing alive and pass submitted frames along.
struct Reprojection {
    std::mutex mutex;
    bool initialized{};
    s32 video_out_handle{-1};
    s32 display_index[2]{-1, -1};
    u32 submitted_frames{};
    UserEvent start_event;
    UserEvent end_event;
};

Reprojection g_reprojection;

Core::Vr::Fov FovFromUv(const OrbisHmdReprojectionEyeUv& left_eye) {
    // uv = tan * scale + offset, so the image edges (uv 0 and 1) give the half angles back.
    // On the left eye the left edge is the temple side.
    return {
        .tan_out = left_eye.offset_x / left_eye.scale_x,
        .tan_in = (1.0f - left_eye.offset_x) / left_eye.scale_x,
        .tan_top = left_eye.offset_y / left_eye.scale_y,
        .tan_bottom = (1.0f - left_eye.offset_y) / left_eye.scale_y,
    };
}

} // namespace

void OnVblank() {
    UserEvent start_event;
    UserEvent end_event;
    {
        std::scoped_lock lock{g_reprojection.mutex};
        if (!g_reprojection.initialized) {
            return;
        }
        start_event = g_reprojection.start_event;
        end_event = g_reprojection.end_event;
    }
    start_event.Trigger();
    end_event.Trigger();
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionAddDisplayBuffer() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventEnd() {
    LOG_INFO(Lib_Hmd, "called");
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.end_event = {};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionClearUserEventStart() {
    LOG_INFO(Lib_Hmd, "called");
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.start_event = {};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfo() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionDebugGetLastInfoMultilayer() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionFinalize() {
    LOG_INFO(Lib_Hmd, "called");
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.initialized = false;
    g_reprojection.video_out_handle = -1;
    g_reprojection.start_event = {};
    g_reprojection.end_event = {};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionFinalizeCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionInitialize(const OrbisHmdReprojectionResourceInfo* resource,
                                              s32 type, void* option) {
    if (resource == nullptr) {
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    }
    LOG_INFO(Lib_Hmd,
             "called, type = {}, thread_priority = {}, cpu_affinity_mask = {:#x}, pipe_id = {}, "
             "queue_id = {}",
             type, resource->thread_priority, resource->cpu_affinity_mask, resource->pipe_id,
             resource->queue_id);
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.initialized = true;
    g_reprojection.submitted_frames = 0;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionInitializeCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffAlign() {
    return 0x100;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryGarlicBuffSize() {
    return 0x100000;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffAlign() {
    return 0x100;
}

s32 PS4_SYSV_ABI sceHmdReprojectionQueryOnionBuffSize() {
    return 0x810;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetCallback() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetDisplayBuffers(s32 video_out_handle, s32 index0, s32 index1,
                                                     void* option) {
    LOG_INFO(Lib_Hmd, "called, video_out_handle = {}, index0 = {}, index1 = {}", video_out_handle,
             index0, index1);
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.video_out_handle = video_out_handle;
    g_reprojection.display_index[0] = index0;
    g_reprojection.display_index[1] = index1;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetOutputMinColor(float red, float green, float blue) {
    LOG_DEBUG(Lib_Hmd, "called, red = {}, green = {}, blue = {}", red, green, blue);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventEnd(Libraries::Kernel::OrbisKernelEqueue eq,
                                                   s32 id) {
    LOG_INFO(Lib_Hmd, "called, eq = {}, id = {}", eq, id);
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.end_event = {.eq = eq, .id = id, .is_set = true};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionSetUserEventStart(Libraries::Kernel::OrbisKernelEqueue eq,
                                                     s32 id) {
    LOG_INFO(Lib_Hmd, "called, eq = {}, id = {}", eq, id);
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.start_event = {.eq = eq, .id = id, .is_set = true};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStart(const OrbisHmdReprojectionParam* param,
                                         const OrbisHmdReprojectionTrackerState* tracker_state,
                                         s64 flip_arg, s32 option) {
    if (param == nullptr || tracker_state == nullptr || param->texture[0] == nullptr ||
        param->texture[1] == nullptr) {
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    }

    Libraries::VideoOut::HmdFrame frame{};
    std::memcpy(&frame.eye_textures[0], param->texture[0], sizeof(AmdGpu::Image));
    std::memcpy(&frame.eye_textures[1], param->texture[1], sizeof(AmdGpu::Image));
    frame.fov = FovFromUv(param->uv[0]);
    frame.render_pose = {
        .position{tracker_state->position[0], tracker_state->position[1],
                  tracker_state->position[2]},
        .orientation{tracker_state->orientation[0], tracker_state->orientation[1],
                     tracker_state->orientation[2], tracker_state->orientation[3]},
    };
    frame.flip_arg = flip_arg;

    s32 video_out_handle;
    u32 frame_number;
    {
        std::scoped_lock lock{g_reprojection.mutex};
        if (!g_reprojection.initialized || g_reprojection.video_out_handle < 0) {
            return ORBIS_HMD_ERROR_NOT_INITIALIZED;
        }
        video_out_handle = g_reprojection.video_out_handle;
        frame_number = g_reprojection.submitted_frames++;
        // The reprojection alternates between the two display buffers it was given.
        frame.display_index = g_reprojection.display_index[frame_number & 1];
    }

    if (frame_number < 4) {
        const auto& left = frame.eye_textures[0];
        LOG_INFO(Lib_Hmd,
                 "frame {}: flip_arg = {}, eye {}x{} at {:#x} / {:#x}, tan out/in/top/bottom = "
                 "{:.4f}/{:.4f}/{:.4f}/{:.4f}, unknown_40 = {:#x}, flags = {:#x}, option = {}",
                 frame_number, flip_arg, left.width + 1, left.height + 1, left.Address(),
                 frame.eye_textures[1].Address(), frame.fov.tan_out, frame.fov.tan_in,
                 frame.fov.tan_top, frame.fov.tan_bottom, param->unknown_40, param->flags, option);
    }

    Core::GuestCpu::NoteGuestProgress();

    // How fast the title delivers frames is the number that matters for comfort; report it now
    // and then.
    {
        using Clock = std::chrono::steady_clock;
        static constexpr auto ReportInterval = std::chrono::seconds{10};
        static Clock::time_point report_time = Clock::now();
        static u32 report_frame = 0;
        const auto now = Clock::now();
        if (now - report_time >= ReportInterval) {
            const float seconds = std::chrono::duration<float>(now - report_time).count();
            LOG_INFO(Lib_Hmd, "{} headset frames so far, {:.1f} per second", frame_number,
                     static_cast<float>(frame_number - report_frame) / seconds);
            report_time = now;
            report_frame = frame_number;
        }
    }

    const s32 result = Libraries::VideoOut::SubmitHmdFrame(video_out_handle, frame);
    if (result != ORBIS_OK) {
        LOG_ERROR(Lib_Hmd, "Could not queue headset frame: {:#x}", static_cast<u32>(result));
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStart2dVr(const Reprojection2dParam* param,
                                             s64 flip_arg, void* option) {
    auto* memory = Core::Memory::Instance();
    if (!param || !memory->IsValidMapping(reinterpret_cast<VAddr>(param), sizeof(*param)))
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    const auto p = *param;
    if (!p.texture || !p.sampler || !p.label)
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    if (option || !Valid2dValues(p) ||
        !memory->IsValidMapping(p.texture, sizeof(AmdGpu::Image)) ||
        !memory->IsValidMapping(p.sampler, 16) || !memory->IsValidMapping(p.label, 8))
        return ORBIS_HMD_ERROR_PARAMETER_INVALID;

    Libraries::VideoOut::HmdFrame frame{};
    std::memcpy(&frame.eye_textures[0], reinterpret_cast<const void*>(p.texture), sizeof(AmdGpu::Image));
    frame.eye_textures[1] = frame.eye_textures[0];
    frame.is_2d = true;
    frame.screen_uv = p.uv;
    frame.flip_arg = flip_arg;
    s32 handle;
    u32 number;
    {
        std::scoped_lock lock{g_reprojection.mutex};
        if (!g_reprojection.initialized || g_reprojection.video_out_handle < 0)
            return ORBIS_HMD_ERROR_NOT_INITIALIZED;
        handle = g_reprojection.video_out_handle;
        number = g_reprojection.submitted_frames++;
        frame.display_index = g_reprojection.display_index[number & 1];
    }
    if (number < 3)
        LOG_INFO(Lib_Hmd, "2D VR desktop preview frame={} texture={}x{} uv={}/{}/{}/{} time_us={}",
                 flip_arg, frame.eye_textures[0].width+1, frame.eye_textures[0].height+1,
                 p.uv[0],p.uv[1],p.uv[2],p.uv[3],p.time_us);
    // No speculative guest label writes. The presentation frame uses the existing
    // GPU timeline. Headset cinematic-plane geometry is a separate implementation.
    return Libraries::VideoOut::SubmitHmdFrame(handle, frame);
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartLiveCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartMultilayer2() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNear() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWideNearWithOverlay(const void* param,
    const OrbisHmdReprojectionTrackerState* tracker, s64 flip_arg, const void* overlay, void* option) {
    static const unsigned mode = [] {
        const char* vr = std::getenv("SHADPS4_EXPERIMENTAL_WIDE_NEAR_VR");
        if (vr && std::strcmp(vr, "1") == 0) return 3u;
        const char* value = std::getenv("SHADPS4_EXPERIMENTAL_WIDE_NEAR_PREVIEW");
        if (value && std::strcmp(value, "stereo") == 0) return 2u;
        return value && std::strcmp(value, "1") == 0 ? 1u : 0u;
    }();
    if (mode == 0) {
        LOG_ERROR(Lib_Hmd, "(STUBBED) called; experimental desktop preview disabled");
        return ORBIS_OK;
    }
    auto* memory = Core::Memory::Instance();
    if (!param || !overlay || !memory->IsValidMapping(reinterpret_cast<VAddr>(param), sizeof(ObservedAbi::WideNearPrefix)) ||
        !memory->IsValidMapping(reinterpret_cast<VAddr>(overlay), 0x38))
        return ORBIS_HMD_ERROR_PARAMETER_NULL;
    ObservedAbi::WideNearPrefix p{};
    std::memcpy(&p, param, sizeof(p));
    // Support only the observed shared-atlas layout. Flag bits remain uninterpreted.
    if (option || p.flags != 5 || !p.texture[0][0] ||
        p.texture[0][0] != p.texture[0][1] || p.texture[0][0] != p.texture[1][0] ||
        p.texture[0][0] != p.texture[1][1] ||
        !memory->IsValidMapping(p.texture[0][0], sizeof(AmdGpu::Image)) ||
        !Preview::ValidBand(p.transition_start,p.transition_end))
        return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
    Libraries::VideoOut::HmdFrame frame{};
    std::memcpy(&frame.eye_textures[0], reinterpret_cast<const void*>(p.texture[0][0]), sizeof(AmdGpu::Image));
    frame.eye_textures[1] = frame.eye_textures[0];
    frame.is_2d = mode != 3; // Headset export requires its own explicit experimental opt-in.
    std::memcpy(frame.screen_uv.data(), &p.uv[0][0], 16);
    std::memcpy(frame.preview_near_uv.data(), &p.uv[0][1], 16);
    std::memcpy(frame.preview_view_uv.data(), static_cast<const u8*>(overlay)+0x18, 16);
    frame.preview_band = {p.transition_start,p.transition_end,1,mode >= 2 ? 1.0f : 0.0f};
    // Observed caller writes right-eye full-view UV at overlay+0x28, left at +0x18.
    // Both happen to match in this title; preserve them independently.
    std::memcpy(frame.preview_right_uv[0].data(), &p.uv[1][0], 16);
    std::memcpy(frame.preview_right_uv[1].data(), &p.uv[1][1], 16);
    std::memcpy(frame.preview_right_uv[2].data(), static_cast<const u8*>(overlay)+0x28, 16);
    if (mode >= 2 && (!Preview::ValidMap(frame.preview_right_uv[0]) ||
                     !Preview::ValidMap(frame.preview_right_uv[1]) ||
                     !Preview::ValidMap(frame.preview_right_uv[2])))
        return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
    if (!Preview::ValidMap(frame.screen_uv) || !Preview::ValidMap(frame.preview_near_uv) ||
        !Preview::ValidMap(frame.preview_view_uv) || frame.eye_textures[0].width+1 != 1920 ||
        frame.eye_textures[0].height+1 != 1080)
        return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
    if (mode == 3) {
        if (!tracker || !memory->IsValidMapping(reinterpret_cast<VAddr>(tracker), sizeof(*tracker)))
            return ORBIS_HMD_ERROR_PARAMETER_NULL;
        OrbisHmdReprojectionTrackerState pose{};
        std::memcpy(&pose, tracker, sizeof(pose));
        std::array<float, 4> view{};
        if (!Preview::ValidRenderPose(pose.position, pose.orientation) ||
            !Preview::CommonVrView(frame.preview_view_uv, frame.preview_right_uv[2], view))
            return ORBIS_HMD_ERROR_UNSUPPORTED_FEATURE;
        frame.preview_view_uv = view;
        frame.preview_right_uv[2] = view;
        frame.fov = { .tan_out = .5f/view[0], .tan_in = .5f/view[0],
                      .tan_top = .5f/view[1], .tan_bottom = .5f/view[1] };
        frame.render_pose = {
            .position{pose.position[0], pose.position[1], pose.position[2]},
            .orientation{pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]},
        };
    }
    frame.flip_arg = flip_arg;
    s32 handle;
    {
        std::scoped_lock lock{g_reprojection.mutex};
        if (!g_reprojection.initialized || g_reprojection.video_out_handle < 0)
            return ORBIS_HMD_ERROR_NOT_INITIALIZED;
        handle = g_reprojection.video_out_handle;
        frame.display_index = g_reprojection.display_index[g_reprojection.submitted_frames++ & 1];
    }
    static bool logged{};
    if (!logged) {
        LOG_WARNING(Lib_Hmd, "Experimental WideNear mode={} (1=mono desktop, 2=stereo desktop, 3=VR export); linear radial blend; overlays omitted; no guest label writes", mode);
        logged = true;
    }
    return Libraries::VideoOut::SubmitHmdFrame(handle, frame);
}

s32 PS4_SYSV_ABI sceHmdReprojectionStartWithOverlay() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStop() {
    LOG_INFO(Lib_Hmd, "called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStopCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionStopLiveCapture() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionUnsetCallback() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHmdReprojectionUnsetDisplayBuffers() {
    LOG_INFO(Lib_Hmd, "called");
    std::scoped_lock lock{g_reprojection.mutex};
    g_reprojection.video_out_handle = -1;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_A31A0320D80EAD99() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Func_B9A6FA0735EC7E49() {
    LOG_ERROR(Lib_Hmd, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterReprojection(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("8gH1aLgty5I", "libsceHmdReprojectionMultilayer", 1, "libSceHmd",
                 sceHmdReprojectionStartMultilayer);
    LIB_FUNCTION("NTIbBpSH9ik", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionAddDisplayBuffer);
    LIB_FUNCTION("94+Ggm38KCg", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionClearUserEventEnd);
    LIB_FUNCTION("mdyFbaJj66M", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionClearUserEventStart);
    LIB_FUNCTION("MdV0akauNow", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionDebugGetLastInfo);
    LIB_FUNCTION("ymiwVjPB5+k", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionDebugGetLastInfoMultilayer);
    LIB_FUNCTION("ZrV5YIqD09I", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionFinalize);
    LIB_FUNCTION("utHD2Ab-Ixo", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionFinalizeCapture);
    LIB_FUNCTION("OuygGEWkins", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionInitialize);
    LIB_FUNCTION("BTrQnC6fcAk", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionInitializeCapture);
    LIB_FUNCTION("TkcANcGM0s8", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionQueryGarlicBuffAlign);
    LIB_FUNCTION("z0KtN1vqF2E", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryGarlicBuffSize);
    LIB_FUNCTION("IWybWbR-xvA", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryOnionBuffAlign);
    LIB_FUNCTION("kLUAkN6a1e8", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionQueryOnionBuffSize);
    LIB_FUNCTION("6CRWGc-evO4", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetCallback);
    LIB_FUNCTION("E+dPfjeQLHI", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetDisplayBuffers);
    LIB_FUNCTION("LjdLRysHU6Y", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetOutputMinColor);
    LIB_FUNCTION("knyIhlkpLgE", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetUserEventEnd);
    LIB_FUNCTION("7as0CjXW1B8", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionSetUserEventStart);
    LIB_FUNCTION("dntZTJ7meIU", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStart);
    LIB_FUNCTION("q3e8+nEguyE", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStart2dVr);
    LIB_FUNCTION("RrvyU1pjb9A", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartCapture);
    LIB_FUNCTION("XZ5QUzb4ae0", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartLiveCapture);
    LIB_FUNCTION("8gH1aLgty5I", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartMultilayer);
    LIB_FUNCTION("gqAG7JYeE7A", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartMultilayer2);
    LIB_FUNCTION("3JyuejcNhC0", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartWideNear);
    LIB_FUNCTION("mKa8scOc4-k", "libSceHmd", 1, "libSceHmd",
                 sceHmdReprojectionStartWideNearWithOverlay);
    LIB_FUNCTION("kcldQ7zLYQQ", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStartWithOverlay);
    LIB_FUNCTION("vzMEkwBQciM", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStop);
    LIB_FUNCTION("F7Sndm5teWw", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStopCapture);
    LIB_FUNCTION("PAa6cUL5bR4", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionStopLiveCapture);
    LIB_FUNCTION("0wnZViigP9o", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionUnsetCallback);
    LIB_FUNCTION("iGNNpDDjcwo", "libSceHmd", 1, "libSceHmd", sceHmdReprojectionUnsetDisplayBuffers);
    LIB_FUNCTION("oxoDINgOrZk", "libSceHmd", 1, "libSceHmd", Func_A31A0320D80EAD99);
    LIB_FUNCTION("uab6BzXsfkk", "libSceHmd", 1, "libSceHmd", Func_B9A6FA0735EC7E49);
}
} // namespace Libraries::Hmd
