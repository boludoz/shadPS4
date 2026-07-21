// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package net.shadps4.android

import android.view.Surface

/**
 * Kotlin side of the JNI connectors implemented in src/android/jni_bridge.cpp.
 * Signatures here and there must stay in sync.
 */
object NativeLibrary {
    init {
        System.loadLibrary("shadps4-android")
    }

    /** State values mirror Android::State in android_host.h. */
    const val STATE_IDLE = 0
    const val STATE_PRECOMPILING = 1
    const val STATE_RUNNING = 2
    const val STATE_PAUSED = 3
    const val STATE_STOPPING = 4

    external fun initialize(userDir: String, firmwareDir: String): Boolean

    external fun loadGame(path: String): Boolean

    /**
     * Runs the x86 -> LLVM IR -> ARM64 ahead-of-time pass ("loading screen").
     * Call from a coroutine/worker thread; poll [getAotProgress] for the UI.
     * Results are cached on disk, so subsequent boots of the same game skip
     * straight through.
     */
    external fun precompileAot(): Boolean
    external fun getAotProgress(): Int

    // Surface lifecycle -- see EmulationActivity for the exact mapping.
    external fun setSurface(surface: Surface?)
    external fun surfaceChanged(width: Int, height: Int, format: Int)

    external fun startEmulation(): Boolean
    external fun pauseEmulation()
    external fun resumeEmulation()
    external fun stopEmulation()
    external fun getState(): Int

    external fun setPadButtons(buttons: Int)
    external fun setPadAxis(axis: Int, value: Float)
}
