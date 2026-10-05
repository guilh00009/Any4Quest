// SPDX-License-Identifier: GPL-2.0-or-later

// JNI surface of com.astrobotquest.vrhost.MainActivity. The activity owns the lifecycle and
// everything that is easier in Java (controller events, drawing text, unpacking the runtime);
// this side owns the OpenXR session and the emulator process.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <jni.h>
#include <sys/resource.h>

#include "core_process.h"
#include "log.h"
#include "xr_host.h"

namespace {

struct App {
    JavaVM* vm{};
    jobject activity{};
    CoreProcess core;
    StatusImage status;
    XrHostStatus xr_status;
    std::thread xr_thread;
    std::atomic<bool> quit{};
    std::atomic<uint32_t> recenter_requests{};
    std::mutex pad_mutex;
    PadState pad;
};

std::unique_ptr<App> g_app;

std::string ToString(JNIEnv* env, jstring text) {
    if (text == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(text, nullptr);
    std::string result{chars != nullptr ? chars : ""};
    env->ReleaseStringUTFChars(text, chars);
    return result;
}

std::vector<std::string> ToStrings(JNIEnv* env, jobjectArray array) {
    std::vector<std::string> result;
    const jsize count = array != nullptr ? env->GetArrayLength(array) : 0;
    for (jsize i = 0; i < count; ++i) {
        const auto element = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        result.push_back(ToString(env, element));
        env->DeleteLocalRef(element);
    }
    return result;
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeStartXr(
    JNIEnv* env, jobject activity, jfloat refresh_rate, jint eye_width, jint eye_height,
    jboolean track_hands, jint sharpen, jboolean cubic, jboolean show_stats, jfloat predict_ms,
    jint dynamic_resolution, jboolean cpu_boost, jboolean move_input, jboolean move_rumble) {
    if (g_app) {
        return;
    }
    g_app = std::make_unique<App>();
    env->GetJavaVM(&g_app->vm);
    g_app->activity = env->NewGlobalRef(activity);

    XrHostOptions options;
    options.refresh_rate = refresh_rate;
    options.eye_width = static_cast<uint32_t>(eye_width);
    options.eye_height = static_cast<uint32_t>(eye_height);
    options.track_hands = track_hands == JNI_TRUE;
    options.move_input = move_input == JNI_TRUE;
    options.move_rumble = move_rumble == JNI_TRUE;
    options.sharpen = sharpen;
    options.cubic = cubic == JNI_TRUE;
    options.show_stats = show_stats == JNI_TRUE;
    options.predict_ms = predict_ms;
    options.dynamic_resolution = dynamic_resolution != 0;
    options.follow_layer_size = dynamic_resolution == 2;
    options.cpu_boost = cpu_boost == JNI_TRUE;
    g_app->xr_thread = std::thread{[options] {
        App& app = *g_app;
        JNIEnv* thread_env = nullptr;
        app.vm->AttachCurrentThread(&thread_env, nullptr);
        // This thread has little to do, but it has to do it every refresh: it must not wait
        // for the emulator's threads, which outnumber the processor cores.
        ::setpriority(PRIO_PROCESS, 0, -16);
        RunXrHost(app.vm, app.activity, app.core, app.status, options, app.xr_status, app.quit,
                  app.recenter_requests);
        LOGI("XR thread finished");
        if (!app.quit) {
            // The system ended the session (the user quit from its menu), or there never was
            // one: without a headset to show it in there is no point in the game running on.
            const jclass activity_class = thread_env->GetObjectClass(app.activity);
            const jmethodID ended =
                thread_env->GetMethodID(activity_class, "onSessionEnded", "()V");
            if (ended != nullptr) {
                thread_env->CallVoidMethod(app.activity, ended);
            }
            if (thread_env->ExceptionCheck()) {
                thread_env->ExceptionClear();
            }
        }
        app.vm->DetachCurrentThread();
    }};
}

JNIEXPORT jboolean JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeStartCore(
    JNIEnv* env, jobject, jstring loader, jstring runtime_root, jstring storage_root, jstring game,
    jstring log_file, jobjectArray extra_args, jobjectArray extra_env) {
    if (!g_app) {
        return JNI_FALSE;
    }
    CoreLaunch launch;
    launch.loader = ToString(env, loader);
    launch.runtime_root = ToString(env, runtime_root);
    launch.storage_root = ToString(env, storage_root);
    launch.game = ToString(env, game);
    launch.log_file = ToString(env, log_file);
    launch.extra_args = ToStrings(env, extra_args);
    // NAME=value pairs.
    for (const std::string& entry : ToStrings(env, extra_env)) {
        const size_t equals = entry.find('=');
        if (equals != std::string::npos && equals > 0) {
            launch.extra_env.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
        }
    }
    return g_app->core.Start(launch) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeStop(JNIEnv* env, jobject) {
    if (!g_app) {
        return;
    }
    g_app->quit = true;
    g_app->core.Stop();
    if (g_app->xr_thread.joinable()) {
        g_app->xr_thread.join();
    }
    env->DeleteGlobalRef(g_app->activity);
    g_app.reset();
}

JNIEXPORT jint JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeCoreState(JNIEnv*,
                                                                                  jobject) {
    return g_app ? static_cast<jint>(g_app->core.GetState()) : 0;
}

JNIEXPORT jstring JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeCoreMessage(JNIEnv* env,
                                                                                       jobject) {
    return env->NewStringUTF(g_app ? g_app->core.GetMessage().c_str() : "");
}

JNIEXPORT jlong JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeFrameCount(JNIEnv*,
                                                                                    jobject) {
    return g_app ? static_cast<jlong>(g_app->core.GetPresentedFrames()) : 0;
}

/// `pixels` is a direct buffer of StatusImage::Width x Height RGBA pixels, top row first.
JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeSetStatusImage(
    JNIEnv* env, jobject, jobject pixels) {
    if (!g_app) {
        return;
    }
    const auto* data = static_cast<const uint8_t*>(env->GetDirectBufferAddress(pixels));
    const jlong size = env->GetDirectBufferCapacity(pixels);
    const jlong expected = jlong{StatusImage::Width} * StatusImage::Height * 4;
    if (data == nullptr || size < expected) {
        return;
    }
    std::scoped_lock lock{g_app->status.mutex};
    g_app->status.pixels.assign(data, data + expected);
    ++g_app->status.version;
}

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeInput(
    JNIEnv*, jobject, jint buttons, jint left_x, jint left_y, jint right_x, jint right_y,
    jint left_trigger, jint right_trigger, jboolean touch_down, jint touch_x, jint touch_y) {
    if (!g_app) {
        return;
    }
    std::scoped_lock lock{g_app->pad_mutex};
    PadState& pad = g_app->pad;
    pad.buttons = static_cast<uint32_t>(buttons);
    pad.left_x = static_cast<uint8_t>(left_x);
    pad.left_y = static_cast<uint8_t>(left_y);
    pad.right_x = static_cast<uint8_t>(right_x);
    pad.right_y = static_cast<uint8_t>(right_y);
    pad.left_trigger = static_cast<uint8_t>(left_trigger);
    pad.right_trigger = static_cast<uint8_t>(right_trigger);
    pad.touch_down = touch_down == JNI_TRUE;
    pad.touch_x = static_cast<uint16_t>(touch_x);
    pad.touch_y = static_cast<uint16_t>(touch_y);
    g_app->core.SetPad(pad);
}

/// Gyroscope in rad/s and accelerometer in m/s², in the controller's frame.
JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeMotion(
    JNIEnv*, jobject, jfloat gx, jfloat gy, jfloat gz, jfloat ax, jfloat ay, jfloat az) {
    if (!g_app) {
        return;
    }
    std::scoped_lock lock{g_app->pad_mutex};
    PadState& pad = g_app->pad;
    pad.has_motion = true;
    pad.gyro[0] = gx;
    pad.gyro[1] = gy;
    pad.gyro[2] = gz;
    pad.accel[0] = ax;
    pad.accel[1] = ay;
    pad.accel[2] = az;
    g_app->core.SetPad(pad);
}

/// What the game asks of the controller: see CoreProcess::GetPadFeedback for the layout.
JNIEXPORT jlong JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativePadFeedback(JNIEnv*,
                                                                                     jobject) {
    return g_app ? static_cast<jlong>(g_app->core.GetPadFeedback()) : 0;
}

/// The player asks for the view to be reset (see XrRecenter in xr_host.h for what of it).
JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeRecenter(JNIEnv*, jobject,
                                                                                 jint what) {
    if (g_app) {
        g_app->recenter_requests.fetch_or(static_cast<uint32_t>(what));
    }
}

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeSetMicrophone(
    JNIEnv*, jobject, jboolean enabled, jfloat gain) {
    if (g_app) {
        g_app->core.SetMicrophone(enabled == JNI_TRUE, gain);
    }
}

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeSetLogFile(JNIEnv* env,
                                                                                   jclass,
                                                                                   jstring path) {
    SetLogFile(ToString(env, path).c_str());
}

JNIEXPORT void JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeLog(JNIEnv* env, jclass,
                                                                            jint priority,
                                                                            jstring message) {
    HostLog(priority, "%s", ToString(env, message).c_str());
}

/// Bit 0: the headset session is running. Bit 1: the hands holding the controller are seen.
/// Bits 8 to 15: the display's refresh rate in Hz.
JNIEXPORT jint JNICALL Java_com_astrobotquest_vrhost_MainActivity_nativeXrStatus(JNIEnv*,
                                                                                 jobject) {
    if (!g_app) {
        return 0;
    }
    const int refresh_rate =
        std::clamp(static_cast<int>(g_app->xr_status.refresh_rate.load() + 0.5f), 0, 255);
    return (g_app->xr_status.session_running ? 1 : 0) | (g_app->xr_status.hands_tracked ? 2 : 0) |
           ((g_app->xr_status.move_tracked_mask.load() & 3) << 2) |
           (refresh_rate << 8);
}

} // extern "C"
