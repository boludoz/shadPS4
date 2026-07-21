// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Fallback implementation of the recompiler interfaces for builds without
// LLVM (SHADPS4_NO_LLVM). It keeps the JNI/.so link-complete so the Android
// app builds out of the box; execution runs exclusively through the
// unsupported-instruction connector (i.e. an attached interpreter such as a
// FEXCore-based one). Attach a prebuilt LLVM (scripts/build_llvm_android.sh)
// to replace this with the real AOT + JIT pipeline.

#include <unordered_map>

#include "core/arm64_recompiler/aot_compiler.h"
#include "core/arm64_recompiler/jit_engine.h"

namespace Core::Recompiler {

// ---- AotCompiler ----------------------------------------------------------

AotCompiler::AotCompiler(Config config) : cfg{std::move(config)} {
    error = "shadPS4 was built without LLVM: AOT compilation unavailable";
}

std::optional<AotCompiler::Result> AotCompiler::CompileRegion(
    const MemoryReader&, u64, u64, const std::vector<u64>&, const std::filesystem::path&,
    const std::string&, const ProgressFn&) {
    error = "shadPS4 was built without LLVM: AOT compilation unavailable";
    return std::nullopt;
}

std::optional<std::vector<AotMapEntry>> ReadAotMap(const std::filesystem::path&) {
    return std::nullopt;
}

// ---- JitEngine ------------------------------------------------------------

struct JitEngine::Impl {
    std::unordered_map<u64, HleFn> hle;
    UnsupportedHandler unsupported_handler = nullptr;
    void* unsupported_user = nullptr;
};

JitEngine::JitEngine() : impl{std::make_unique<Impl>()} {}
JitEngine::~JitEngine() = default;

bool JitEngine::LoadAotObject(const std::filesystem::path&, const std::filesystem::path&) {
    error = "shadPS4 was built without LLVM: AOT objects cannot be loaded";
    return false;
}

void JitEngine::RegisterHleFunction(u64 guest_va, HleFn fn) {
    impl->hle[guest_va] = fn;
}

void JitEngine::SetUnsupportedHandler(UnsupportedHandler handler, void* user) {
    impl->unsupported_handler = handler;
    impl->unsupported_user = user;
}

void JitEngine::SetMemoryReader(MemoryReader) {}

BlockFn JitEngine::GetBlock(u64) {
    return nullptr; // no translation tier available
}

void JitEngine::InvalidateRange(u64, u64) {}

ExitReason JitEngine::Run(GuestContext& ctx) {
    // Interpreter-only dispatcher: every "block" is delegated to the
    // unsupported-instruction connector, one step at a time.
    ctx.exit_reason = ExitReason::None;
    while (true) {
        if (ctx.rip == kExitRip) {
            return ExitReason::None;
        }
        const auto it = impl->hle.find(ctx.rip);
        if (it != impl->hle.end()) {
            ctx.gpr[kRax] = it->second(ctx.gpr[kRdi], ctx.gpr[kRsi], ctx.gpr[kRdx],
                                       ctx.gpr[kRcx], ctx.gpr[kR8], ctx.gpr[kR9]);
            ctx.rip = *reinterpret_cast<const u64*>(ctx.gpr[kRsp]);
            ctx.gpr[kRsp] += 8;
            continue;
        }
        if (!impl->unsupported_handler ||
            !impl->unsupported_handler(ctx, impl->unsupported_user)) {
            ctx.exit_reason = ExitReason::UnsupportedInstruction;
            ctx.exit_arg = ctx.rip;
            return ExitReason::UnsupportedInstruction;
        }
    }
}

} // namespace Core::Recompiler
