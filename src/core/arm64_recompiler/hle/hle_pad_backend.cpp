// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Pad backends. The default backend reports a connected, idle controller and
// accepts injected state (used by the Android on-screen overlay and by tests).
// The SDL backend (SHADPS4_HAVE_SDL) reads real gamepads and maps their
// buttons/axes to the PS4 layout.

#include <array>
#include <mutex>
#include <vector>

#include "core/arm64_recompiler/hle/hle_pad.h"

#ifdef SHADPS4_HAVE_SDL
#include <SDL3/SDL.h>
#endif

namespace Core::Recompiler::Hle {

namespace {

/// Reports a connected idle pad and stores states injected by the frontend
/// overlay. Always available so pad HLE never has a null target.
class InjectableBackend final : public PadBackend {
public:
    void Poll() override {}
    PadState Read(int index) override {
        std::scoped_lock lk{mutex};
        return states[index % kMax];
    }
    int Count() override {
        return kMax;
    }
    void Inject(int index, const PadState& s) {
        std::scoped_lock lk{mutex};
        states[index % kMax] = s;
    }

private:
    static constexpr int kMax = 4;
    std::mutex mutex;
    std::array<PadState, kMax> states{};
};

#ifdef SHADPS4_HAVE_SDL

/// Reads real controllers through SDL and maps them to the PS4 layout.
class SdlPadBackend final : public PadBackend {
public:
    SdlPadBackend() {
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        Refresh();
    }
    ~SdlPadBackend() override {
        for (auto* pad : pads) {
            if (pad) {
                SDL_CloseGamepad(pad);
            }
        }
    }

    void Poll() override {
        SDL_UpdateGamepads();
    }

    PadState Read(int index) override {
        PadState s{};
        if (index < 0 || index >= static_cast<int>(pads.size()) || !pads[index]) {
            s.connected = false;
            return s;
        }
        SDL_Gamepad* g = pads[index];
        auto btn = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(g, b); };
        u32 buttons = 0;
        buttons |= btn(SDL_GAMEPAD_BUTTON_DPAD_UP) ? 0x10 : 0;
        buttons |= btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? 0x20 : 0;
        buttons |= btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? 0x40 : 0;
        buttons |= btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? 0x80 : 0;
        buttons |= btn(SDL_GAMEPAD_BUTTON_WEST) ? 0x8000 : 0;   // Square
        buttons |= btn(SDL_GAMEPAD_BUTTON_SOUTH) ? 0x4000 : 0;  // Cross
        buttons |= btn(SDL_GAMEPAD_BUTTON_EAST) ? 0x2000 : 0;   // Circle
        buttons |= btn(SDL_GAMEPAD_BUTTON_NORTH) ? 0x1000 : 0;  // Triangle
        buttons |= btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 0x400 : 0;
        buttons |= btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 0x800 : 0;
        buttons |= btn(SDL_GAMEPAD_BUTTON_START) ? 0x8 : 0; // Options
        buttons |= btn(SDL_GAMEPAD_BUTTON_LEFT_STICK) ? 0x2 : 0;
        buttons |= btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK) ? 0x4 : 0;
        s.buttons = buttons;

        auto axis8 = [&](SDL_GamepadAxis a) {
            const int v = SDL_GetGamepadAxis(g, a); // -32768..32767
            return static_cast<u8>((v + 32768) >> 8);
        };
        s.left_x = axis8(SDL_GAMEPAD_AXIS_LEFTX);
        s.left_y = axis8(SDL_GAMEPAD_AXIS_LEFTY);
        s.right_x = axis8(SDL_GAMEPAD_AXIS_RIGHTX);
        s.right_y = axis8(SDL_GAMEPAD_AXIS_RIGHTY);
        const int lt = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        const int rt = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        s.l2 = static_cast<u8>(lt >> 7);
        s.r2 = static_cast<u8>(rt >> 7);
        if (s.l2 > 64) {
            s.buttons |= 0x100;
        }
        if (s.r2 > 64) {
            s.buttons |= 0x200;
        }
        s.connected = true;
        return s;
    }

    int Count() override {
        return static_cast<int>(pads.size());
    }

private:
    void Refresh() {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        pads.clear();
        for (int i = 0; i < count; ++i) {
            pads.push_back(SDL_OpenGamepad(ids[i]));
        }
        SDL_free(ids);
    }
    std::vector<SDL_Gamepad*> pads;
};

#endif // SHADPS4_HAVE_SDL

// The active injectable backend, if that is the one in use, so InjectPadState
// can reach it.
InjectableBackend* g_injectable = nullptr;

} // namespace

std::unique_ptr<PadBackend> CreatePadBackend() {
#ifdef SHADPS4_HAVE_SDL
    return std::make_unique<SdlPadBackend>();
#else
    auto backend = std::make_unique<InjectableBackend>();
    g_injectable = backend.get();
    return backend;
#endif
}

void InjectPadState(int index, const PadState& state) {
    if (g_injectable) {
        g_injectable->Inject(index, state);
    }
}

} // namespace Core::Recompiler::Hle
