// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/types.h"

namespace Core::Recompiler::Hle {

class HleRegistry;

/// Backend an audio port streams PCM to. The HLE layer converts the guest's
/// sceAudioOut calls into these; the concrete backend is chosen at build time
/// (OpenAL on desktop/Android, a null sink otherwise).
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    struct PortConfig {
        u32 sample_rate;
        u32 samples;   // frames per Output call (granularity)
        u32 channels;  // 1, 2 or 8
        bool is_float; // sample format
    };

    /// Opens a port; returns a backend handle >= 0 or -1 on failure.
    virtual int Open(const PortConfig& config) = 0;
    /// Queues one buffer of `frames` frames and blocks until there is room,
    /// mirroring sceAudioOutOutput's back-pressure. Returns frames written.
    virtual int Output(int port, const void* pcm, u32 frames) = 0;
    virtual void SetVolume(int port, const s32* volumes, int count) = 0;
    virtual void Close(int port) = 0;
};

/// Selects the audio backend compiled into this build. Never returns null (a
/// silent null backend is the floor), so audio HLE always has a target.
std::unique_ptr<AudioBackend> CreateAudioBackend();

/// Overrides the process-wide backend (used by tests to observe output).
void SetAudioBackend(std::unique_ptr<AudioBackend> backend);

/// Registers the sceAudioOut* subset with the HLE registry.
void RegisterAudioOut(HleRegistry& registry);

} // namespace Core::Recompiler::Hle
