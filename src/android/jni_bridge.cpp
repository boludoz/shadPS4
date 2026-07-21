// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// JNI connectors for the Android APK frontend. The Kotlin side lives in
// android/app/src/main/java/net/shadps4/android/NativeLibrary.kt and must
// keep these signatures in sync.
//
// Threading/lifecycle contract (mirrors EmulationActivity):
//   initialize()        once, from Application.onCreate
//   loadGame()          from a worker thread
//   precompileAot()     from a worker thread; poll getAotProgress() for UI
//   surfaceCreated ->   setSurface(surface)
//   surfaceChanged ->   surfaceChanged(w, h, format)
//   surfaceDestroyed -> setSurface(null)   [BLOCKS until renderer lets go]
//   onPause -> pauseEmulation() / onResume -> resumeEmulation()
//   onDestroy -> stopEmulation()

#ifdef __ANDROID__

#include <jni.h>

#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "android/android_host.h"

namespace {

std::string ToString(JNIEnv* env, jstring str) {
    if (!str) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(str, nullptr);
    std::string result{chars ? chars : ""};
    env->ReleaseStringUTFChars(str, chars);
    return result;
}

} // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}

JNIEXPORT jboolean JNICALL Java_net_shadps4_android_NativeLibrary_initialize(
    JNIEnv* env, jclass, jstring user_dir, jstring firmware_dir) {
    return Android::Host::Instance().Initialize(ToString(env, user_dir),
                                                ToString(env, firmware_dir))
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_net_shadps4_android_NativeLibrary_loadGame(JNIEnv* env, jclass,
                                                                           jstring path) {
    return Android::Host::Instance().LoadGame(ToString(env, path)) ? JNI_TRUE : JNI_FALSE;
}

// Title metadata from the dump's sce_sys/param.sfo, valid after loadGame().
JNIEXPORT jstring JNICALL Java_net_shadps4_android_NativeLibrary_getGameTitle(JNIEnv* env, jclass) {
    return env->NewStringUTF(Android::Host::Instance().GetGameTitle().c_str());
}

JNIEXPORT jstring JNICALL Java_net_shadps4_android_NativeLibrary_getGameTitleId(JNIEnv* env,
                                                                                jclass) {
    return env->NewStringUTF(Android::Host::Instance().GetGameTitleId().c_str());
}

JNIEXPORT jstring JNICALL Java_net_shadps4_android_NativeLibrary_getGameVersion(JNIEnv* env,
                                                                                jclass) {
    return env->NewStringUTF(Android::Host::Instance().GetGameVersion().c_str());
}

JNIEXPORT jboolean JNICALL Java_net_shadps4_android_NativeLibrary_precompileAot(JNIEnv*, jclass) {
    return Android::Host::Instance().PrecompileAot() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_net_shadps4_android_NativeLibrary_getAotProgress(JNIEnv*, jclass) {
    return Android::Host::Instance().GetAotProgress();
}

// ---- Surface lifecycle ---------------------------------------------------

JNIEXPORT void JNICALL Java_net_shadps4_android_NativeLibrary_setSurface(JNIEnv* env, jclass,
                                                                         jobject surface) {
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    // Passing null blocks until the render thread released the old window —
    // required because Android tears the Surface down right after
    // surfaceDestroyed() returns.
    Android::Host::Instance().SetSurface(window);
}

JNIEXPORT void JNICALL Java_net_shadps4_android_NativeLibrary_surfaceChanged(JNIEnv*, jclass,
                                                                             jint width,
                                                                             jint height,
                                                                             jint format) {
    Android::Host::Instance().SurfaceChanged(width, height, format);
}

// ---- Emulation lifecycle -------------------------------------------------

JNIEXPORT jboolean JNICALL Java_net_shadps4_android_NativeLibrary_startEmulation(JNIEnv*, jclass) {
    return Android::Host::Instance().Start() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_net_shadps4_android_NativeLibrary_pauseEmulation(JNIEnv*, jclass) {
    Android::Host::Instance().Pause();
}

JNIEXPORT void JNICALL Java_net_shadps4_android_NativeLibrary_resumeEmulation(JNIEnv*, jclass) {
    Android::Host::Instance().Resume();
}

JNIEXPORT void JNICALL Java_net_shadps4_android_NativeLibrary_stopEmulation(JNIEnv*, jclass) {
    Android::Host::Instance().Stop();
}

JNIEXPORT jint JNICALL Java_net_shadps4_android_NativeLibrary_getState(JNIEnv*, jclass) {
    return static_cast<jint>(Android::Host::Instance().GetState());
}

JNIEXPORT jint JNICALL Java_net_shadps4_android_NativeLibrary_getLastBootStatus(JNIEnv*, jclass) {
    return Android::Host::Instance().GetLastBootStatus();
}

JNIEXPORT jstring JNICALL Java_net_shadps4_android_NativeLibrary_getLastBootMessage(JNIEnv* env,
                                                                                    jclass) {
    return env->NewStringUTF(Android::Host::Instance().GetLastBootMessage().c_str());
}

// ---- Input ---------------------------------------------------------------

JNIEXPORT void JNICALL Java_net_shadps4_android_NativeLibrary_setPadButtons(JNIEnv*, jclass,
                                                                            jint buttons) {
    Android::Host::Instance().SetPadButtons(static_cast<u32>(buttons));
}

JNIEXPORT void JNICALL Java_net_shadps4_android_NativeLibrary_setPadAxis(JNIEnv*, jclass,
                                                                         jint axis, jfloat value) {
    Android::Host::Instance().SetPadAxis(axis, value);
}

} // extern "C"

#endif // __ANDROID__
