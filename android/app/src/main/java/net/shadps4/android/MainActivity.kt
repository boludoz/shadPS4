// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package net.shadps4.android

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle

/**
 * Minimal launcher: initializes the native side and forwards a picked game to
 * [EmulationActivity]. A real frontend (game grid, settings, firmware
 * management) replaces this Activity; the native connectors don't change.
 */
class MainActivity : Activity() {

    private val pickGame = 1001

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val userDir = filesDir.absolutePath
        val firmwareDir = getExternalFilesDir("firmware")?.absolutePath ?: userDir
        NativeLibrary.initialize(userDir, firmwareDir)

        startActivityForResult(
            Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "*/*"
            },
            pickGame,
        )
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != pickGame || resultCode != RESULT_OK) {
            finish(); return
        }
        val uri: Uri = data?.data ?: run { finish(); return }
        // Simplification: pass the raw path/URI through; production code
        // resolves SAF URIs to file descriptors instead.
        startActivity(
            Intent(this, EmulationActivity::class.java)
                .putExtra(EmulationActivity.EXTRA_GAME_PATH, uri.toString()),
        )
        finish()
    }
}
