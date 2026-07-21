// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"
#include "core/arm64_recompiler/game_package.h"
#include "core/arm64_recompiler/guest_loader.h"

struct ANativeWindow;

namespace Core::Recompiler {
class JitEngine;
}

namespace Android {

/// Executable segment of the loaded guest module, as mapped in memory.
/// Produced by the loader hook; consumed by the AOT pass.
struct CodeRegion {
    u64 base;
    u64 size;
    std::vector<u64> entry_points;
};

/// Emulation lifecycle as driven by the Java Activity.
enum class State {
    Idle,        // nothing loaded
    Precompiling, // AOT pass running (loading screen)
    Running,
    Paused,      // Activity paused; guest threads parked between blocks
    Stopping,
};

/// Host-side singleton behind the JNI bridge. Owns the native window, the
/// recompiler engine and the emulation lifecycle. All methods are safe to
/// call from the JNI/UI thread; render/emulation threads synchronize through
/// it.
class Host {
public:
    static Host& Instance();

    // ---- Initialization -------------------------------------------------
    /// Called once from JNI_OnLoad / NativeLibrary.initialize().
    /// user_dir: app-private files dir (savedata, aot_cache, log).
    /// firmware_dir: extracted sys modules, fonts, etc.
    bool Initialize(std::string user_dir, std::string firmware_dir);

    // ---- Surface lifecycle ----------------------------------------------
    // Android owns the window: it can be destroyed and recreated at any
    // point (Activity minimized, rotation, multi-window). The renderer must
    // never present to a dead window, so SetSurface(nullptr) BLOCKS until
    // the render thread has released the old ANativeWindow.
    void SetSurface(ANativeWindow* window);
    void SurfaceChanged(int width, int height, int format);
    /// Current window for the Vulkan backend; call under AcquireSurface.
    ANativeWindow* GetWindow() const {
        return window.load(std::memory_order_acquire);
    }

    // ---- Emulation lifecycle --------------------------------------------
    /// Loads a dumped game the way the desktop build does: accepts the dump
    /// folder or its eboot.bin, reads sce_sys/param.sfo, loads the eboot plus
    /// every sce_module/*.prx and resolves imports across them, and mounts
    /// the dump as /app0. Does not start execution.
    bool LoadGame(const std::string& path);

    // Title metadata from param.sfo (empty for homebrew without one), valid
    // after LoadGame().
    const std::string& GetGameTitle() const {
        return game_title;
    }
    const std::string& GetGameTitleId() const {
        return game_title_id;
    }
    const std::string& GetGameVersion() const {
        return game_version;
    }

    /// Runs the AOT pass over the loaded module's code segments, writing to
    /// <user_dir>/aot_cache. progress in [0,100] is reported through the
    /// JNI progress callback. Safe to call again: cached results are reused.
    bool PrecompileAot();

    bool Start();
    /// Parks guest threads at the next dispatcher iteration; keeps all JIT
    /// and AOT state. Matches Activity.onPause().
    void Pause();
    /// Resumes parked guest threads. Matches Activity.onResume().
    void Resume();
    /// Full teardown. Matches Activity.onDestroy()/finish().
    void Stop();

    State GetState() const {
        return state.load(std::memory_order_acquire);
    }
    /// AOT progress in percent (valid while Precompiling).
    int GetAotProgress() const {
        return aot_progress.load(std::memory_order_relaxed);
    }

    // ---- Input ----------------------------------------------------------
    /// Bitmask uses the OrbisPadButtonDataOffset layout used by the pad HLE.
    void SetPadButtons(u32 buttons);
    void SetPadAxis(int axis, float value); // 0/1 = left x/y, 2/3 = right, 4/5 = triggers

    /// Called by emulation threads between translated blocks; blocks while
    /// paused, returns false when stopping.
    bool CheckRunGate();

    Core::Recompiler::JitEngine* GetJit() {
        return jit.get();
    }
    const std::filesystem::path& GetUserDir() const {
        return user_dir;
    }

private:
    Host() = default;

    std::filesystem::path user_dir;
    std::filesystem::path firmware_dir;
    std::filesystem::path game_path; // eboot.bin of the loaded dump
    std::filesystem::path game_dir;  // dump root, mounted as /app0
    std::string game_title;
    std::string game_title_id;
    std::string game_version;
    std::vector<CodeRegion> code_regions;

    // Loaded guest modules (eboot + sce_module .prx, in load order) and the
    // loader that owns their import trampolines (valid once LoadGame
    // succeeds).
    std::unique_ptr<Core::Recompiler::GuestLoader> guest_loader;
    std::optional<Core::Recompiler::GuestLoader::LoadedModule> loaded_module;
    std::vector<Core::Recompiler::GuestLoader::LoadedModule> preload_modules;

    int last_boot_status = -1;
    std::string last_boot_message;

public:
    /// Result of the last Start() boot attempt, for the frontend to display.
    int GetLastBootStatus() const {
        return last_boot_status;
    }
    std::string GetLastBootMessage() const {
        return last_boot_message;
    }

private:

    std::atomic<ANativeWindow*> window{nullptr};
    std::atomic<int> surface_generation{0};

    std::unique_ptr<Core::Recompiler::JitEngine> jit;
    std::atomic<State> state{State::Idle};
    std::atomic<int> aot_progress{0};

    std::mutex gate_mutex;
    std::condition_variable gate_cv;

    std::mutex surface_mutex;
    std::condition_variable surface_cv;
    std::atomic<int> surface_users{0};

    std::atomic<u32> pad_buttons{0};
    std::atomic<float> pad_axes[6]{};
};

/// Connectors the emulator core overrides (declared weak in android_host.cpp)
/// once Core::Emulator builds for Android. The JNI layer never talks to the
/// core directly, only through these.
namespace Hooks {
bool LoadGame(const std::filesystem::path& path, std::vector<CodeRegion>& out_regions);
bool StartEmulator();
void StopEmulator();
} // namespace Hooks

} // namespace Android
