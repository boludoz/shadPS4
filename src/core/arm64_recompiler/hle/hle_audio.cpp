// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sceAudioOut HLE for the ARM64 port. Guest audio calls are translated into
// AudioBackend operations. Ports and their configuration are tracked here so
// sceAudioOutOutput knows the frame size/format to hand the backend.

#include <array>
#include <mutex>

#include "common/types.h"
#include "core/arm64_recompiler/hle/hle_audio.h"
#include "core/arm64_recompiler/hle/hle_registry.h"

namespace Core::Recompiler::Hle {

namespace {

// PS4 sceAudioOut constants (subset; values from libSceAudioOut).
constexpr s32 kMaxPorts = 32;
constexpr u32 kFormatMono = 0;
constexpr u32 kFormatStereo = 1;
constexpr u32 kFormat8ch = 2;
constexpr u32 kFormatMonoFloat = 3;
constexpr u32 kFormatStereoFloat = 4;
constexpr u32 kFormat8chFloat = 5;

struct Port {
    bool open = false;
    int backend_handle = -1;
    u32 sample_rate = 48000;
    u32 samples = 256;
    u32 channels = 2;
    bool is_float = false;
};

struct AudioState {
    std::mutex mutex;
    std::unique_ptr<AudioBackend> backend;
    std::array<Port, kMaxPorts> ports{};

    AudioBackend& Backend() {
        if (!backend) {
            backend = CreateAudioBackend();
        }
        return *backend;
    }
};

AudioState& State() {
    static AudioState state;
    return state;
}

void DecodeFormat(u32 format, u32& channels, bool& is_float) {
    switch (format) {
    case kFormatMono:
        channels = 1;
        is_float = false;
        break;
    case kFormatMonoFloat:
        channels = 1;
        is_float = true;
        break;
    case kFormat8ch:
        channels = 8;
        is_float = false;
        break;
    case kFormat8chFloat:
        channels = 8;
        is_float = true;
        break;
    case kFormatStereoFloat:
        channels = 2;
        is_float = true;
        break;
    case kFormatStereo:
    default:
        channels = 2;
        is_float = false;
        break;
    }
}

// ---- sceAudioOut entry points (PS4 ABI) -----------------------------------

s32 PS4_SYSV_ABI sceAudioOutInit() {
    return 0; // ORBIS_OK
}

s32 PS4_SYSV_ABI sceAudioOutOpen(s32 user_id, s32 port_type, s32 index, u32 length,
                                 u32 sample_rate, u32 param_format) {
    (void)user_id;
    (void)port_type;
    (void)index;
    auto& st = State();
    std::scoped_lock lk{st.mutex};
    for (s32 h = 1; h < kMaxPorts; ++h) {
        Port& p = st.ports[h];
        if (p.open) {
            continue;
        }
        p.open = true;
        p.sample_rate = sample_rate ? sample_rate : 48000;
        p.samples = length ? length : 256;
        DecodeFormat(param_format, p.channels, p.is_float);
        p.backend_handle =
            st.Backend().Open({p.sample_rate, p.samples, p.channels, p.is_float});
        return h; // handle
    }
    return -1;
}

s32 PS4_SYSV_ABI sceAudioOutOutput(s32 handle, const void* ptr) {
    auto& st = State();
    Port snapshot;
    {
        std::scoped_lock lk{st.mutex};
        if (handle <= 0 || handle >= kMaxPorts || !st.ports[handle].open) {
            return -1;
        }
        snapshot = st.ports[handle];
    }
    if (!ptr) {
        return 0; // draining query
    }
    return st.Backend().Output(snapshot.backend_handle, ptr, snapshot.samples);
}

s32 PS4_SYSV_ABI sceAudioOutSetVolume(s32 handle, s32 flag, s32* vol) {
    (void)flag;
    auto& st = State();
    std::scoped_lock lk{st.mutex};
    if (handle <= 0 || handle >= kMaxPorts || !st.ports[handle].open) {
        return -1;
    }
    st.Backend().SetVolume(st.ports[handle].backend_handle, vol, 8);
    return 0;
}

s32 PS4_SYSV_ABI sceAudioOutClose(s32 handle) {
    auto& st = State();
    std::scoped_lock lk{st.mutex};
    if (handle <= 0 || handle >= kMaxPorts || !st.ports[handle].open) {
        return -1;
    }
    st.Backend().Close(st.ports[handle].backend_handle);
    st.ports[handle] = Port{};
    return 0;
}

s32 PS4_SYSV_ABI sceAudioOutGetPortState(s32 handle, void* state) {
    (void)handle;
    (void)state;
    return 0;
}

} // namespace

void SetAudioBackend(std::unique_ptr<AudioBackend> backend) {
    auto& st = State();
    std::scoped_lock lk{st.mutex};
    st.backend = std::move(backend);
}

void RegisterAudioOut(HleRegistry& registry) {
    // NIDs from libSceAudioOut (see core/libraries/audio/audioout.cpp).
    HLE_REGISTER(registry, "JfEPXVxhFqA", sceAudioOutInit);
    HLE_REGISTER(registry, "ekNvsmVLb5Y", sceAudioOutOpen);
    HLE_REGISTER(registry, "w3PdaSTSwGE", sceAudioOutOutput);
    HLE_REGISTER(registry, "Q3Rt0TadflU", sceAudioOutSetVolume);
    HLE_REGISTER(registry, "s1--uE9mBFw", sceAudioOutClose);
    HLE_REGISTER(registry, "9C3wp4Ck3E4", sceAudioOutGetPortState);
}

} // namespace Core::Recompiler::Hle
