// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// End-to-end tests for the x86 -> LLVM IR -> native pipeline: real x86-64
// machine code is translated and *executed* through the fallback JIT (on the
// build host the JIT emits host code; on an arm64 device the same path emits
// arm64). The AOT object/map round trip is exercised as well.
//
// Deliberately assert-based instead of GTest so it can run in minimal
// cross-build environments (Android NDK, CI containers).

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "core/arm64_recompiler/aot_compiler.h"
#include "core/arm64_recompiler/jit_engine.h"
#include "core/arm64_recompiler/recompiler_api.h"

using namespace Core::Recompiler;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

/// Copies machine code into a buffer and runs it from entry with up to two
/// integer arguments, returning guest RAX.
u64 RunGuest(JitEngine& jit, const std::vector<u8>& code, u64 arg0 = 0, u64 arg1 = 0) {
    // Each snippet gets its own persistent host allocation so distinct code
    // never shares a guest address (which would collide in the block cache).
    // The buffers are intentionally leaked for the lifetime of the test.
    auto* image = new std::vector<u8>(code.begin(), code.end());
    image->resize(code.size() + 16, 0xCC);

    auto* ctx_opaque = shad_guest_context_create(reinterpret_cast<u64>(image->data()), 64 * 1024);
    auto* ctx = reinterpret_cast<GuestContext*>(ctx_opaque);
    ctx->gpr[kRdi] = arg0;
    ctx->gpr[kRsi] = arg1;
    const ExitReason reason = jit.Run(*ctx);
    CHECK(reason == ExitReason::None);
    const u64 rax = ctx->gpr[kRax];
    shad_guest_context_destroy(ctx_opaque);
    return rax;
}

void TestBasicArithmetic(JitEngine& jit) {
    // mov rax, rdi ; add rax, rsi ; ret
    const std::vector<u8> add_fn = {0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3};
    CHECK(RunGuest(jit, add_fn, 2, 40) == 42);
    CHECK(RunGuest(jit, add_fn, ~u64{0}, 1) == 0);

    // 32-bit dest zero-extends: mov eax, edi ; sub eax, esi ; ret
    const std::vector<u8> sub_fn = {0x89, 0xF8, 0x29, 0xF0, 0xC3};
    CHECK(RunGuest(jit, sub_fn, 7, 3) == 4);
    CHECK(RunGuest(jit, sub_fn, 3, 4) == 0xFFFFFFFFull); // borrow stays 32-bit
}

void TestBranchesAndLoops(JitEngine& jit) {
    // Sum 0..n-1:
    //   xor eax, eax ; xor ecx, ecx
    // loop: cmp rcx, rdi ; jge done ; add rax, rcx ; inc rcx ; jmp loop
    // done: ret
    const std::vector<u8> sum_fn = {
        // 0x00  xor eax, eax
        0x31, 0xC0,
        // 0x02  xor ecx, ecx
        0x31, 0xC9,
        // 0x04  cmp rcx, rdi   (loop head)
        0x48, 0x39, 0xF9,
        // 0x07  jge +8 -> 0x11 (ret)
        0x7D, 0x08,
        // 0x09  add rax, rcx
        0x48, 0x01, 0xC8,
        // 0x0C  inc rcx
        0x48, 0xFF, 0xC1,
        // 0x0F  jmp -13 -> 0x04 (loop head)
        0xEB, 0xF3,
        // 0x11  ret
        0xC3,
    };
    CHECK(RunGuest(jit, sum_fn, 10) == 45);
    CHECK(RunGuest(jit, sum_fn, 0) == 0);
    CHECK(RunGuest(jit, sum_fn, 1000) == 499500);
}

void TestCallRetStack(JitEngine& jit) {
    // Callee at +0x10 doubles rdi; caller calls it twice.
    //   call +0x10 ; mov rdi, rax ; call +0x08 ; ret ; (pad) ; lea rax,[rdi+rdi] ; ret
    const std::vector<u8> call_fn = {
        0xE8, 0x0B, 0x00, 0x00, 0x00, // call +0x0B -> callee (at 0x10)
        0x48, 0x89, 0xC7,             // mov rdi, rax
        0xE8, 0x03, 0x00, 0x00, 0x00, // call +0x03 -> callee
        0xC3,                         // ret
        0x90, 0x90,                   // pad
        0x48, 0x8D, 0x04, 0x3F,       // callee: lea rax, [rdi+rdi]
        0xC3,                         // ret
    };
    CHECK(RunGuest(jit, call_fn, 5) == 20); // 5*2*2
}

void TestFlagsAndCmov(JitEngine& jit) {
    // max(rdi, rsi): mov rax, rdi ; cmp rax, rsi ; cmovl rax, rsi ; ret
    const std::vector<u8> max_fn = {0x48, 0x89, 0xF8, 0x48, 0x39, 0xF0,
                                    0x48, 0x0F, 0x4C, 0xC6, 0xC3};
    CHECK(RunGuest(jit, max_fn, 3, 9) == 9);
    CHECK(RunGuest(jit, max_fn, 9, 3) == 9);
    CHECK(RunGuest(jit, max_fn, static_cast<u64>(-5), 2) == 2); // signed compare

    // setz al: xor eax,eax ; cmp rdi, rsi ; sete al ; ret
    const std::vector<u8> eq_fn = {0x31, 0xC0, 0x48, 0x39, 0xF7, 0x0F, 0x94, 0xC0, 0xC3};
    CHECK(RunGuest(jit, eq_fn, 7, 7) == 1);
    CHECK(RunGuest(jit, eq_fn, 7, 8) == 0);
}

void TestMulDivShift(JitEngine& jit) {
    // imul rax, rdi, 7 ; ret
    const std::vector<u8> imul_fn = {0x48, 0x6B, 0xC7, 0x07, 0xC3};
    CHECK(RunGuest(jit, imul_fn, 6) == 42);

    // div: mov rax, rdi ; xor edx, edx ; div rsi ; ret  (rax = rdi / rsi)
    const std::vector<u8> div_fn = {0x48, 0x89, 0xF8, 0x31, 0xD2, 0x48, 0xF7, 0xF6, 0xC3};
    CHECK(RunGuest(jit, div_fn, 42, 5) == 8);

    // shl rax: mov rax, rdi ; mov cl, sil ; shl rax, cl ; ret
    const std::vector<u8> shl_fn = {0x48, 0x89, 0xF8, 0x40, 0x88, 0xF1,
                                    0x48, 0xD3, 0xE0, 0xC3};
    CHECK(RunGuest(jit, shl_fn, 3, 4) == 48);
}

void TestMemoryAndStack(JitEngine& jit) {
    // push rdi ; pop rax ; ret
    const std::vector<u8> pushpop_fn = {0x57, 0x58, 0xC3};
    CHECK(RunGuest(jit, pushpop_fn, 0x123456789ABCDEFull) == 0x123456789ABCDEFull);

    // Read through a pointer: mov rax, [rdi] ; movzx ecx, byte [rdi+8] ; add rax, rcx ; ret
    const std::vector<u8> load_fn = {0x48, 0x8B, 0x07, 0x0F, 0xB6, 0x4F, 0x08,
                                     0x48, 0x01, 0xC8, 0xC3};
    struct {
        u64 value;
        u8 extra;
    } data{40, 2};
    CHECK(RunGuest(jit, load_fn, reinterpret_cast<u64>(&data)) == 42);
}

void TestScalarFloat(JitEngine& jit) {
    // double average: cvtsi2sd xmm0, rdi ; cvtsi2sd xmm1, rsi ; addsd xmm0, xmm1 ;
    // mov rax, 2 ; cvtsi2sd xmm2, rax ; divsd xmm0, xmm2 ; cvttsd2si rax, xmm0 ; ret
    const std::vector<u8> avg_fn = {
        0xF2, 0x48, 0x0F, 0x2A, 0xC7,             // cvtsi2sd xmm0, rdi
        0xF2, 0x48, 0x0F, 0x2A, 0xCE,             // cvtsi2sd xmm1, rsi
        0xF2, 0x0F, 0x58, 0xC1,                   // addsd xmm0, xmm1
        0x48, 0xC7, 0xC0, 0x02, 0x00, 0x00, 0x00, // mov rax, 2
        0xF2, 0x48, 0x0F, 0x2A, 0xD0,             // cvtsi2sd xmm2, rax
        0xF2, 0x0F, 0x5E, 0xC2,                   // divsd xmm0, xmm2
        0xF2, 0x48, 0x0F, 0x2C, 0xC0,             // cvttsd2si rax, xmm0
        0xC3,                                     // ret
    };
    CHECK(RunGuest(jit, avg_fn, 10, 32) == 21);
}

void TestHleBridge(JitEngine& jit) {
    // Guest calls an absolute address held in rax which we register as HLE:
    //   mov rax, imm64 ; call rax ; add rax, 1 ; ret
    static constexpr u64 kHleAddress = 0x10000000;
    jit.RegisterHleFunction(kHleAddress, [](u64 a, u64 b, u64, u64, u64, u64) -> u64 {
        return a * b;
    });
    std::vector<u8> call_hle = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // mov rax, kHleAddress
        0xFF, 0xD0,                         // call rax
        0x48, 0x83, 0xC0, 0x01,             // add rax, 1
        0xC3,                               // ret
    };
    const u64 addr = kHleAddress;
    std::memcpy(call_hle.data() + 2, &addr, sizeof(addr));
    CHECK(RunGuest(jit, call_hle, 6, 7) == 43);
}

void TestUnsupportedFallback(JitEngine& jit) {
    // cpuid is not in the supported subset; the handler must be invoked and
    // can skip the instruction.
    static bool handler_ran = false;
    jit.SetUnsupportedHandler(
        [](GuestContext& ctx, void*) {
            handler_ran = true;
            ctx.rip += 2; // skip cpuid (0F A2)
            ctx.gpr[kRax] = 0x1234;
            return true;
        },
        nullptr);
    // xor eax, eax ; cpuid ; ret
    const std::vector<u8> cpuid_fn = {0x31, 0xC0, 0x0F, 0xA2, 0xC3};
    CHECK(RunGuest(jit, cpuid_fn) == 0x1234);
    CHECK(handler_ran);
    jit.SetUnsupportedHandler(nullptr, nullptr);
}

void TestSmcInvalidation(JitEngine& jit) {
    // Translate a block, patch its bytes, invalidate, retranslate.
    static std::vector<u8> image = {0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, 0xC3}; // mov rax,1
    const u64 base = reinterpret_cast<u64>(image.data());

    auto run = [&] {
        auto* c = shad_guest_context_create(base, 16 * 1024);
        auto* ctx = reinterpret_cast<GuestContext*>(c);
        jit.Run(*ctx);
        const u64 rax = ctx->gpr[kRax];
        shad_guest_context_destroy(c);
        return rax;
    };
    CHECK(run() == 1);
    image[3] = 0x02; // mov rax, 2
    CHECK(run() == 1); // stale translation still cached
    jit.InvalidateRange(base, image.size());
    CHECK(run() == 2); // retranslated after invalidation
}

void TestAotRoundTrip() {
    // AOT-compile a small function for the *host* triple, then load the
    // object into a fresh engine and execute without runtime translation.
    static std::vector<u8> image = {0x48, 0x8D, 0x04, 0x77, 0xC3}; // lea rax,[rdi+rsi*2]; ret
    const u64 base = reinterpret_cast<u64>(image.data());

    AotCompiler::Config config{};
    config.triple = "native";
    AotCompiler compiler{config};
    const auto cache_dir = std::filesystem::temp_directory_path() / "shadps4_aot_test";
    const auto result = compiler.CompileRegion(IdentityMemoryReader(), base, image.size(), {base},
                                               cache_dir, "testmod");
    CHECK(result.has_value());
    if (!result) {
        std::fprintf(stderr, "AOT error: %s\n", compiler.GetError().c_str());
        return;
    }
    CHECK(result->blocks_translated == 1);

    JitEngine engine;
    CHECK(engine.GetError().empty());
    CHECK(engine.LoadAotObject(result->object_file, result->map_file));

    auto* c = shad_guest_context_create(base, 16 * 1024);
    auto* ctx = reinterpret_cast<GuestContext*>(c);
    ctx->gpr[kRdi] = 10;
    ctx->gpr[kRsi] = 16;
    CHECK(engine.Run(*ctx) == ExitReason::None);
    CHECK(ctx->gpr[kRax] == 42);
    shad_guest_context_destroy(c);
    std::filesystem::remove_all(cache_dir);
}

void TestArm64CodegenSmoke() {
    // Ensure the cross target actually emits an AArch64 object regardless of
    // the host: this is the exact path the loading-screen AOT pass uses on
    // device.
    static std::vector<u8> image = {0x48, 0x01, 0xF7, 0x48, 0x89, 0xF8, 0xC3};
    const u64 base = reinterpret_cast<u64>(image.data());

    AotCompiler compiler{{}}; // default triple: aarch64 android
    const auto cache_dir = std::filesystem::temp_directory_path() / "shadps4_aot_arm64";
    const auto result = compiler.CompileRegion(IdentityMemoryReader(), base, image.size(), {base},
                                               cache_dir, "arm64mod");
    CHECK(result.has_value());
    if (result) {
        CHECK(std::filesystem::file_size(result->object_file) > 0);
        // ELF magic + EM_AARCH64 (0xB7 at offset 18).
        std::vector<u8> header(20);
        auto* f = std::fopen(result->object_file.string().c_str(), "rb");
        CHECK(f && std::fread(header.data(), 1, header.size(), f) == header.size());
        if (f) {
            std::fclose(f);
        }
        CHECK(header[0] == 0x7F && header[1] == 'E' && header[2] == 'L' && header[3] == 'F');
        CHECK(header[18] == 0xB7);
    }
    std::filesystem::remove_all(cache_dir);
}

} // namespace

int main() {
    JitEngine jit;
    CHECK(jit.GetError().empty());

    TestBasicArithmetic(jit);
    TestBranchesAndLoops(jit);
    TestCallRetStack(jit);
    TestFlagsAndCmov(jit);
    TestMulDivShift(jit);
    TestMemoryAndStack(jit);
    TestScalarFloat(jit);
    TestHleBridge(jit);
    TestUnsupportedFallback(jit);
    TestSmcInvalidation(jit);
    TestAotRoundTrip();
    TestArm64CodegenSmoke();

    if (failures == 0) {
        std::printf("arm64_recompiler_test: all tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "arm64_recompiler_test: %d check(s) failed\n", failures);
    return 1;
}
