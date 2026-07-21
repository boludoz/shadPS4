// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// scePad HLE for the ARM64 port. Translates the guest's scePad calls into
// PadBackend reads and formats the result into the guest's OrbisPadData. The
// OrbisPadData mirror below is laid out identically to the one in
// core/libraries/pad/pad.h so the bytes the guest reads are ABI-correct.

#include <array>
#include <cstring>
#include <mutex>

#include "common/types.h"
#include "core/arm64_recompiler/hle/hle_pad.h"
#include "core/arm64_recompiler/hle/hle_registry.h"

namespace Core::Recompiler::Hle {

namespace {

constexpr int kMaxHandles = 16;

// ---- ABI mirror of OrbisPadData (must match core/libraries/pad/pad.h) -----
struct AnalogStick {
    u8 x, y;
};
struct AnalogButtons {
    u8 l2, r2;
    u8 padding[2];
};
struct FQuaternion {
    float x, y, z, w;
};
struct FVector3 {
    float x, y, z;
};
struct PadTouch {
    u16 x, y;
    u8 id;
    u8 reserve[3];
};
struct PadTouchData {
    u8 touchNum;
    u8 reserve[3];
    u32 time;
    PadTouch touch[2];
};
struct PadExtensionUnitData {
    u32 extensionUnitId;
    u8 reserve[1];
    u8 dataLength;
    u8 data[10];
};
struct OrbisPadData {
    u32 buttons;
    AnalogStick leftStick;
    AnalogStick rightStick;
    AnalogButtons analogButtons;
    FQuaternion orientation;
    FVector3 acceleration;
    FVector3 angularVelocity;
    PadTouchData touchData;
    bool connected;
    u64 timestamp;
    PadExtensionUnitData extensionUnitData;
    u8 connectedCount;
    u8 reserve[2];
    u8 deviceUniqueDataLen;
    u8 deviceUniqueData[12];
};

struct Handle {
    bool open = false;
    int backend_index = 0;
};

struct PadHleState {
    std::mutex mutex;
    std::unique_ptr<PadBackend> backend;
    std::array<Handle, kMaxHandles> handles{};
    u64 timestamp = 0;

    PadBackend& Backend() {
        if (!backend) {
            backend = CreatePadBackend();
        }
        return *backend;
    }
};

PadHleState& State() {
    static PadHleState state;
    return state;
}

void FillPadData(OrbisPadData& out, const PadState& src, u64 timestamp) {
    std::memset(&out, 0, sizeof(out));
    out.buttons = src.buttons;
    out.leftStick = {src.left_x, src.left_y};
    out.rightStick = {src.right_x, src.right_y};
    out.analogButtons = {src.l2, src.r2, {0, 0}};
    out.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    out.connected = src.connected;
    out.connectedCount = src.connected ? 1 : 0;
    out.timestamp = timestamp;
}

// ---- scePad entry points (PS4 ABI) ----------------------------------------

s32 PS4_SYSV_ABI scePadInit() {
    return 0;
}

s32 PS4_SYSV_ABI scePadOpen(s32 user_id, s32 type, s32 index, const void* param) {
    (void)user_id;
    (void)type;
    (void)param;
    auto& st = State();
    std::scoped_lock lk{st.mutex};
    for (int h = 1; h < kMaxHandles; ++h) {
        if (!st.handles[h].open) {
            st.handles[h].open = true;
            st.handles[h].backend_index = index;
            return h;
        }
    }
    return -1;
}

s32 PS4_SYSV_ABI scePadClose(s32 handle) {
    auto& st = State();
    std::scoped_lock lk{st.mutex};
    if (handle <= 0 || handle >= kMaxHandles) {
        return -1;
    }
    st.handles[handle] = Handle{};
    return 0;
}

s32 PS4_SYSV_ABI scePadReadState(s32 handle, OrbisPadData* data) {
    auto& st = State();
    int index = 0;
    {
        std::scoped_lock lk{st.mutex};
        if (handle <= 0 || handle >= kMaxHandles || !st.handles[handle].open || !data) {
            return -1;
        }
        index = st.handles[handle].backend_index;
        st.timestamp += 1;
    }
    st.Backend().Poll();
    const PadState s = st.Backend().Read(index);
    FillPadData(*data, s, st.timestamp);
    return 0;
}

s32 PS4_SYSV_ABI scePadRead(s32 handle, OrbisPadData* data, s32 num) {
    if (num <= 0) {
        return 0;
    }
    // Report a single most-recent sample.
    if (scePadReadState(handle, data) != 0) {
        return -1;
    }
    return 1;
}

s32 PS4_SYSV_ABI scePadSetVibration(s32 handle, const void* param) {
    (void)handle;
    (void)param;
    return 0; // accepted; hooked to backend rumble when available
}

} // namespace

void SetPadBackend(std::unique_ptr<PadBackend> backend) {
    auto& st = State();
    std::scoped_lock lk{st.mutex};
    st.backend = std::move(backend);
}

void RegisterPad(HleRegistry& registry) {
    // NIDs from libScePad (see core/libraries/pad/pad.cpp).
    HLE_REGISTER(registry, "hv1luiJrqQM", scePadInit);
    HLE_REGISTER(registry, "xk0AcarP3V4", scePadOpen);
    HLE_REGISTER(registry, "Zmb2Sjt5c3E", scePadClose);
    HLE_REGISTER(registry, "YnEwlicDsPQ", scePadReadState);
    HLE_REGISTER(registry, "q1cHNfGycLI", scePadRead);
    HLE_REGISTER(registry, "yFVnOdGxvZY", scePadSetVibration);
}

} // namespace Core::Recompiler::Hle
