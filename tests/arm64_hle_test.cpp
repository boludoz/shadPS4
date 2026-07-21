// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Verifies the HLE bridge: NID-keyed native functions are reached through the
// register-marshaling adapter, audio/pad calls land in a backend, and a guest
// `call` translated by the JIT diverts into a native HLE implementation.

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/arm64_recompiler/guest_context.h"
#include "core/arm64_recompiler/hle/hle_audio.h"
#include "core/arm64_recompiler/hle/hle_pad.h"
#include "core/arm64_recompiler/hle/hle_registry.h"
#include "core/arm64_recompiler/jit_engine.h"

using namespace Core::Recompiler;
using namespace Core::Recompiler::Hle;

namespace {

int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

// Recording backends so we can observe what the HLE forwarded.
struct RecordingAudio final : AudioBackend {
    int opens = 0;
    u64 total_frames = 0;
    PortConfig last_config{};
    int Open(const PortConfig& c) override {
        last_config = c;
        return opens++;
    }
    int Output(int, const void*, u32 frames) override {
        total_frames += frames;
        return static_cast<int>(frames);
    }
    void SetVolume(int, const s32*, int) override {}
    void Close(int) override {}
};

struct RecordingPad final : PadBackend {
    PadState state;
    int polls = 0;
    void Poll() override {
        ++polls;
    }
    PadState Read(int) override {
        return state;
    }
    int Count() override {
        return 1;
    }
};

// NIDs, kept in one place matching the RegisterAudioOut/RegisterPad tables.
constexpr const char* kNidAudioInit = "JfEPXVxhFqA";
constexpr const char* kNidAudioOpen = "ekNvsmVLb5Y";
constexpr const char* kNidAudioOutput = "w3PdaSTSwGE";
constexpr const char* kNidPadOpen = "xk0AcarP3V4";
constexpr const char* kNidPadReadState = "YnEwlicDsPQ";

void TestAudioBridge() {
    auto* rec = new RecordingAudio();
    SetAudioBackend(std::unique_ptr<AudioBackend>(rec));

    auto& reg = HleRegistry::Instance();
    reg.RegisterBuiltins();

    HleFn init = reg.Find(kNidAudioInit);
    HleFn open = reg.Find(kNidAudioOpen);
    HleFn output = reg.Find(kNidAudioOutput);
    CHECK(init && open && output);
    if (!(init && open && output)) {
        return;
    }

    CHECK(init(0, 0, 0, 0, 0, 0) == 0);
    // sceAudioOutOpen(user, type, index, length=256, rate=48000, format=1 stereo)
    const u64 handle = open(1, 0, 0, 256, 48000, 1);
    CHECK(static_cast<s64>(handle) > 0);
    CHECK(rec->opens == 1);
    CHECK(rec->last_config.sample_rate == 48000);
    CHECK(rec->last_config.channels == 2);

    std::vector<s16> pcm(256 * 2, 0);
    const u64 wrote = output(handle, reinterpret_cast<u64>(pcm.data()), 0, 0, 0, 0);
    CHECK(wrote == 256);
    CHECK(rec->total_frames == 256);
}

void TestPadBridge() {
    auto* rec = new RecordingPad();
    rec->state.buttons = 0x4000 | 0x10; // Cross + Up
    rec->state.left_x = 200;
    SetPadBackend(std::unique_ptr<PadBackend>(rec));

    auto& reg = HleRegistry::Instance();
    HleFn open = reg.Find(kNidPadOpen);
    HleFn read = reg.Find(kNidPadReadState);
    CHECK(open && read);
    if (!(open && read)) {
        return;
    }

    const u64 handle = open(1, 0, 0, 0, 0, 0);
    CHECK(static_cast<s64>(handle) > 0);

    // OrbisPadData is large; give the HLE a generous zeroed buffer and read
    // the fields we filled back at their ABI offsets (buttons @0, lx @4).
    std::vector<u8> buffer(512, 0);
    const u64 rc = read(handle, reinterpret_cast<u64>(buffer.data()), 0, 0, 0, 0);
    CHECK(rc == 0);
    u32 buttons = 0;
    std::memcpy(&buttons, buffer.data(), 4);
    CHECK(buttons == (0x4000u | 0x10u));
    CHECK(buffer[4] == 200); // leftStick.x
    CHECK(rec->polls >= 1);
}

// Drive an audio call through the actual JIT: guest code does `call rax` into
// the HLE trampoline address, exactly as a translated game would.
void TestHleThroughJit() {
    auto* rec = new RecordingAudio();
    SetAudioBackend(std::unique_ptr<AudioBackend>(rec));
    auto& reg = HleRegistry::Instance();
    HleFn init = reg.Find(kNidAudioInit);
    CHECK(init != nullptr);
    if (!init) {
        return;
    }

    JitEngine jit;
    CHECK(jit.GetError().empty());

    // Register the native HLE at a chosen guest address.
    constexpr u64 kHleAddr = 0x5000'0000;
    jit.RegisterHleFunction(kHleAddr, reinterpret_cast<u64 (*)(u64, u64, u64, u64, u64, u64)>(init));

    // Guest: mov rax, kHleAddr ; call rax ; ret
    std::vector<u8> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // mov rax, imm64
        0xFF, 0xD0,                         // call rax
        0xC3,                               // ret
    };
    const u64 addr = kHleAddr;
    std::memcpy(code.data() + 2, &addr, sizeof(addr));
    code.resize(code.size() + 16, 0xCC);

    GuestContext ctx{};
    static std::vector<u8> stack(64 * 1024);
    u64 rsp = reinterpret_cast<u64>(stack.data() + stack.size()) & ~u64{15};
    rsp -= 8;
    std::memcpy(reinterpret_cast<void*>(rsp), &kExitRip, sizeof(kExitRip));
    ctx.gpr[kRsp] = rsp;
    ctx.rip = reinterpret_cast<u64>(code.data());

    const ExitReason reason = jit.Run(ctx);
    CHECK(reason == ExitReason::None);
    CHECK(ctx.gpr[kRax] == 0); // sceAudioOutInit returned ORBIS_OK through the bridge
}

} // namespace

int main() {
    TestAudioBridge();
    TestPadBridge();
    TestHleThroughJit();

    if (failures == 0) {
        std::printf("arm64_hle_test: all tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "arm64_hle_test: %d check(s) failed\n", failures);
    return 1;
}
