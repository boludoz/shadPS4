// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/types.h"

namespace Core::Recompiler::Hle {

class HleRegistry;

/// Normalized controller state the pad HLE reads from a backend and formats
/// into the guest's OrbisPadData. Sourced from SDL on desktop/Android, from an
/// on-screen overlay through the frontend, or injected by tests.
struct PadState {
    u32 buttons = 0;      // OrbisPadButtonDataOffset bitmask
    u8 left_x = 128;      // 0..255, 128 = centered
    u8 left_y = 128;
    u8 right_x = 128;
    u8 right_y = 128;
    u8 l2 = 0;            // analog triggers 0..255
    u8 r2 = 0;
    bool connected = true;
};

/// Backend that produces controller state. The null backend reports a
/// connected, idle pad; the SDL backend (SHADPS4_HAVE_SDL) reads real
/// gamepads.
class PadBackend {
public:
    virtual ~PadBackend() = default;
    virtual void Poll() = 0;
    virtual PadState Read(int index) = 0;
    virtual int Count() = 0;
};

std::unique_ptr<PadBackend> CreatePadBackend();

/// Overrides the process-wide pad backend (used by the frontend's on-screen
/// controls and by tests).
void SetPadBackend(std::unique_ptr<PadBackend> backend);

/// Injects a state for pad `index` into whatever backend is active if it
/// supports injection (the frontend/overlay path). No-op for hardware
/// backends.
void InjectPadState(int index, const PadState& state);

void RegisterPad(HleRegistry& registry);

} // namespace Core::Recompiler::Hle
