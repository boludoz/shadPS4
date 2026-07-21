// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Audio backends. The null backend is always available and paces output to
// real time so a game's audio thread is throttled correctly even without a
// device. The OpenAL backend (SHADPS4_HAVE_OPENAL) actually plays sound and is
// what the Android app and desktop builds link.

#include <chrono>
#include <thread>

#include "core/arm64_recompiler/hle/hle_audio.h"

#ifdef SHADPS4_HAVE_OPENAL
#include <array>
#include <vector>
#include <AL/al.h>
#include <AL/alc.h>
#endif

namespace Core::Recompiler::Hle {

namespace {

/// Silent sink that still blocks for the buffer's real duration, so guest
/// audio threads are paced as if a device were consuming samples.
class NullAudioBackend final : public AudioBackend {
public:
    int Open(const PortConfig& config) override {
        const int handle = next++;
        rates[handle % kMax] = config.sample_rate ? config.sample_rate : 48000;
        return handle;
    }
    int Output(int port, const void*, u32 frames) override {
        const u32 rate = rates[port % kMax];
        const auto ns = std::chrono::nanoseconds{static_cast<u64>(frames) * 1'000'000'000ull /
                                                 (rate ? rate : 48000)};
        std::this_thread::sleep_for(ns);
        return static_cast<int>(frames);
    }
    void SetVolume(int, const s32*, int) override {}
    void Close(int) override {}

private:
    static constexpr int kMax = 64;
    int next = 0;
    u32 rates[kMax] = {};
};

#ifdef SHADPS4_HAVE_OPENAL

/// Streams PCM through OpenAL with a small ring of buffers per port.
class OpenAlBackend final : public AudioBackend {
public:
    OpenAlBackend() {
        device = alcOpenDevice(nullptr);
        if (device) {
            context = alcCreateContext(device, nullptr);
            alcMakeContextCurrent(context);
        }
    }
    ~OpenAlBackend() override {
        if (context) {
            alcMakeContextCurrent(nullptr);
            alcDestroyContext(context);
        }
        if (device) {
            alcCloseDevice(device);
        }
    }

    int Open(const PortConfig& config) override {
        if (!device) {
            return -1;
        }
        const int id = static_cast<int>(ports.size());
        Port p{};
        p.config = config;
        alGenSources(1, &p.source);
        alGenBuffers(kRing, p.buffers.data());
        p.format = FormatFor(config);
        ports.push_back(p);
        return id;
    }

    int Output(int port, const void* pcm, u32 frames) override {
        if (port < 0 || port >= static_cast<int>(ports.size())) {
            return -1;
        }
        Port& p = ports[port];
        const u32 bytes = frames * p.config.channels * (p.config.is_float ? 4 : 2);

        // Reclaim any processed buffers.
        ALint processed = 0;
        alGetSourcei(p.source, AL_BUFFERS_PROCESSED, &processed);
        while (processed-- > 0) {
            ALuint b = 0;
            alSourceUnqueueBuffers(p.source, 1, &b);
            p.free.push_back(b);
        }
        if (p.free.empty() && p.queued < kRing) {
            p.free.push_back(p.buffers[p.queued]);
        }
        if (p.free.empty()) {
            // Ring full: wait for the source to drain one buffer.
            ALint state = 0;
            do {
                alGetSourcei(p.source, AL_BUFFERS_PROCESSED, &processed);
                if (processed > 0) {
                    ALuint b = 0;
                    alSourceUnqueueBuffers(p.source, 1, &b);
                    p.free.push_back(b);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                alGetSourcei(p.source, AL_SOURCE_STATE, &state);
            } while (true);
        }
        ALuint buf = p.free.back();
        p.free.pop_back();
        alBufferData(buf, p.format, pcm, static_cast<ALsizei>(bytes),
                     static_cast<ALsizei>(p.config.sample_rate));
        alSourceQueueBuffers(p.source, 1, &buf);
        p.queued++;

        ALint state = 0;
        alGetSourcei(p.source, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) {
            alSourcePlay(p.source);
        }
        return static_cast<int>(frames);
    }

    void SetVolume(int port, const s32* volumes, int count) override {
        if (port < 0 || port >= static_cast<int>(ports.size()) || !volumes || count <= 0) {
            return;
        }
        // Average the channel volumes into a single gain (0..1).
        s64 sum = 0;
        for (int i = 0; i < count; ++i) {
            sum += volumes[i];
        }
        const float gain = static_cast<float>(sum) / count / 32768.0f;
        alSourcef(ports[port].source, AL_GAIN, gain);
    }

    void Close(int port) override {
        if (port < 0 || port >= static_cast<int>(ports.size())) {
            return;
        }
        Port& p = ports[port];
        alSourceStop(p.source);
        alDeleteSources(1, &p.source);
        alDeleteBuffers(kRing, p.buffers.data());
        p = Port{};
    }

private:
    static constexpr int kRing = 4;
    struct Port {
        PortConfig config{};
        ALuint source = 0;
        std::array<ALuint, kRing> buffers{};
        std::vector<ALuint> free;
        ALenum format = AL_FORMAT_STEREO16;
        int queued = 0;
    };

    static ALenum FormatFor(const PortConfig& c) {
        if (c.channels == 1) {
            return AL_FORMAT_MONO16;
        }
        return AL_FORMAT_STEREO16; // 8ch downmix handled upstream if needed
    }

    ALCdevice* device = nullptr;
    ALCcontext* context = nullptr;
    std::vector<Port> ports;
};

#endif // SHADPS4_HAVE_OPENAL

} // namespace

std::unique_ptr<AudioBackend> CreateAudioBackend() {
#ifdef SHADPS4_HAVE_OPENAL
    auto backend = std::make_unique<OpenAlBackend>();
    return backend;
#else
    return std::make_unique<NullAudioBackend>();
#endif
}

} // namespace Core::Recompiler::Hle
