// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package net.shadps4.android

import android.app.Activity
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import kotlin.concurrent.thread

/**
 * Hosts the emulation SurfaceView and forwards the Android lifecycle to the
 * native host. The Surface can die and be reborn at any time (home button,
 * rotation, multi-window drag) while emulation keeps running, so surface and
 * emulation lifecycles are handled independently.
 */
class EmulationActivity : Activity(), SurfaceHolder.Callback {

    companion object {
        const val EXTRA_GAME_PATH = "game_path"
    }

    private lateinit var surfaceView: SurfaceView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        setContentView(surfaceView)

        val gamePath = intent.getStringExtra(EXTRA_GAME_PATH) ?: run {
            finish(); return
        }

        thread(name = "shadps4-boot") {
            if (!NativeLibrary.loadGame(gamePath)) {
                runOnUiThread { finish() }
                return@thread
            }
            // AOT pass: translate the game's x86-64 code to ARM64 before it
            // runs (cached after the first boot). Show progress in a real UI.
            NativeLibrary.precompileAot()
            NativeLibrary.startEmulation()
        }
    }

    // ---- Surface lifecycle -> native window ------------------------------

    override fun surfaceCreated(holder: SurfaceHolder) {
        NativeLibrary.setSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        NativeLibrary.surfaceChanged(width, height, format)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // Blocks until the native renderer released the window. Returning
        // earlier would let Android destroy a Surface the GPU still presents
        // to, which is a native crash.
        NativeLibrary.setSurface(null)
    }

    // ---- Activity lifecycle -> emulation state ---------------------------

    override fun onPause() {
        super.onPause()
        NativeLibrary.pauseEmulation()
    }

    override fun onResume() {
        super.onResume()
        NativeLibrary.resumeEmulation()
    }

    override fun onDestroy() {
        NativeLibrary.stopEmulation()
        super.onDestroy()
    }
}
