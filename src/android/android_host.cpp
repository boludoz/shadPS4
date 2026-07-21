// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>

#ifdef __ANDROID__
#include <android/native_window.h>
#endif

#include "android/android_host.h"
#include "core/arm64_recompiler/aot_compiler.h"
#include "core/arm64_recompiler/jit_engine.h"

// The full emulator core (Core::Emulator, linker, HLE libraries, Vulkan
// presenter) is wired in through these hooks once it builds for Android.
// They are weak so the recompiler/JNI layer links and is testable on its own;
// the emulator target overrides them with real implementations.
namespace Android::Hooks {

__attribute__((weak)) bool LoadGame(const std::filesystem::path& path,
                                    std::vector<Android::CodeRegion>& out_regions) {
    (void)out_regions;
    // Default: accept the path if it exists; no code regions known yet.
    return std::filesystem::exists(path);
}

__attribute__((weak)) bool StartEmulator() {
    return false; // core not linked in yet
}

__attribute__((weak)) void StopEmulator() {}

} // namespace Android::Hooks

namespace Android {

Host& Host::Instance() {
    static Host instance;
    return instance;
}

bool Host::Initialize(std::string user, std::string firmware) {
    user_dir = user;
    firmware_dir = firmware;
    std::filesystem::create_directories(user_dir / "aot_cache");
    if (!jit) {
        jit = std::make_unique<Core::Recompiler::JitEngine>();
    }
    return jit->GetError().empty();
}

void Host::SetSurface(ANativeWindow* new_window) {
#ifdef __ANDROID__
    ANativeWindow* old = window.exchange(new_window, std::memory_order_acq_rel);
    surface_generation.fetch_add(1, std::memory_order_release);
    if (old) {
        // Android invalidates the window as soon as surfaceDestroyed()
        // returns: wait until the render thread has dropped every reference
        // before releasing it back.
        std::unique_lock lk{surface_mutex};
        surface_cv.wait(lk, [&] { return surface_users.load(std::memory_order_acquire) == 0; });
        ANativeWindow_release(old);
    }
    if (new_window) {
        // The renderer observes the generation bump and recreates its
        // VkSurfaceKHR/swapchain from the new window.
        surface_cv.notify_all();
    }
#else
    (void)new_window;
#endif
}

void Host::SurfaceChanged(int width, int height, int format) {
    (void)width;
    (void)height;
    (void)format;
    // Geometry changes (rotation, multi-window resize) only require a
    // swapchain recreation; bumping the generation makes the renderer pick
    // the new extent up on its next frame.
    surface_generation.fetch_add(1, std::memory_order_release);
}

bool Host::LoadGame(const std::string& path) {
    if (GetState() != State::Idle) {
        return false;
    }
    std::vector<CodeRegion> regions;
    if (!Hooks::LoadGame(path, regions)) {
        return false;
    }
    game_path = path;
    code_regions = std::move(regions);
    return true;
}

bool Host::PrecompileAot() {
    if (!jit || game_path.empty()) {
        return false;
    }
    if (code_regions.empty()) {
        // Nothing mapped yet (core not linked): trivially done.
        aot_progress.store(100, std::memory_order_relaxed);
        return true;
    }
    state.store(State::Precompiling, std::memory_order_release);
    aot_progress.store(0, std::memory_order_relaxed);

    Core::Recompiler::AotCompiler::Config config{};
#ifndef __ANDROID__
    config.triple = "native"; // desktop test runs
#endif
    Core::Recompiler::AotCompiler compiler{config};
    const auto cache_dir = user_dir / "aot_cache";
    bool ok = true;
    for (size_t i = 0; i < code_regions.size(); ++i) {
        const CodeRegion& region = code_regions[i];
        const std::string name = game_path.stem().string() + "_" + std::to_string(i);
        const auto obj = cache_dir / (name + ".o");
        const auto map = cache_dir / (name + ".map");
        if (!std::filesystem::exists(obj) || !std::filesystem::exists(map)) {
            const auto result = compiler.CompileRegion(
                Core::Recompiler::IdentityMemoryReader(), region.base, region.size,
                region.entry_points, cache_dir, name, [&](size_t done, size_t total) {
                    const int pct = total ? static_cast<int>(done * 100 / total) : 100;
                    aot_progress.store(pct, std::memory_order_relaxed);
                    return GetState() == State::Precompiling; // cancel on Stop()
                });
            if (!result) {
                ok = false;
                continue;
            }
        }
        ok &= jit->LoadAotObject(obj, map);
    }
    aot_progress.store(100, std::memory_order_relaxed);
    state.store(State::Idle, std::memory_order_release);
    return ok;
}

bool Host::Start() {
    if (!Hooks::StartEmulator()) {
        return false;
    }
    state.store(State::Running, std::memory_order_release);
    gate_cv.notify_all();
    return true;
}

void Host::Pause() {
    State expected = State::Running;
    state.compare_exchange_strong(expected, State::Paused, std::memory_order_acq_rel);
}

void Host::Resume() {
    State expected = State::Paused;
    if (state.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel)) {
        gate_cv.notify_all();
    }
}

void Host::Stop() {
    state.store(State::Stopping, std::memory_order_release);
    gate_cv.notify_all();
    Hooks::StopEmulator();
    state.store(State::Idle, std::memory_order_release);
}

bool Host::CheckRunGate() {
    State s = GetState();
    if (s == State::Running) {
        return true;
    }
    if (s != State::Paused) {
        return false;
    }
    std::unique_lock lk{gate_mutex};
    gate_cv.wait(lk, [&] {
        const State now = GetState();
        return now == State::Running || now == State::Stopping || now == State::Idle;
    });
    return GetState() == State::Running;
}

void Host::SetPadButtons(u32 buttons) {
    pad_buttons.store(buttons, std::memory_order_relaxed);
}

void Host::SetPadAxis(int axis, float value) {
    if (axis >= 0 && axis < 6) {
        pad_axes[axis].store(value, std::memory_order_relaxed);
    }
}

} // namespace Android
