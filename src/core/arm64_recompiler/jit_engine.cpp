// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_map>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>

#include "core/arm64_recompiler/aot_compiler.h"
#include "core/arm64_recompiler/ir_emitter.h"
#include "core/arm64_recompiler/jit_engine.h"
#include "core/arm64_recompiler/llvm_helpers.h"

namespace Core::Recompiler {

namespace {

void InitializeNativeJitTarget() {
    static const bool done = [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        return true;
    }();
    (void)done;
}

} // namespace

struct JitEngine::Impl {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    X86Decoder decoder;
    MemoryReader memory = IdentityMemoryReader();

    struct BlockRecord {
        BlockFn fn;
        u64 end;
        llvm::orc::ResourceTrackerSP tracker; // null for AOT blocks
    };
    // Ordered by start address so range invalidation can walk overlaps.
    std::map<u64, BlockRecord> blocks;
    std::mutex mutex;

    std::unordered_map<u64, HleFn> hle;
    UnsupportedHandler unsupported_handler = nullptr;
    void* unsupported_user = nullptr;
};

JitEngine::JitEngine() : impl{std::make_unique<Impl>()} {
    InitializeNativeJitTarget();
    auto jit = llvm::orc::LLJITBuilder().create();
    if (!jit) {
        error = llvm::toString(jit.takeError());
        return;
    }
    impl->jit = std::move(*jit);
}

JitEngine::~JitEngine() = default;

bool JitEngine::LoadAotObject(const std::filesystem::path& object_file,
                              const std::filesystem::path& map_file) {
    if (!impl->jit) {
        return false;
    }
    auto buffer = llvm::MemoryBuffer::getFile(object_file.string());
    if (!buffer) {
        error = "cannot read " + object_file.string();
        return false;
    }
    if (auto err = impl->jit->addObjectFile(std::move(*buffer))) {
        error = llvm::toString(std::move(err));
        return false;
    }
    const auto map = ReadAotMap(map_file);
    if (!map) {
        error = "cannot read AOT map " + map_file.string();
        return false;
    }
    // Resolve lazily: record the extent now, look the symbol up on first use.
    std::scoped_lock lk{impl->mutex};
    for (const AotMapEntry& entry : *map) {
        impl->blocks.emplace(entry.guest_rip,
                             Impl::BlockRecord{nullptr, entry.block_end, nullptr});
    }
    return true;
}

void JitEngine::RegisterHleFunction(u64 guest_va, HleFn fn) {
    std::scoped_lock lk{impl->mutex};
    impl->hle[guest_va] = fn;
}

void JitEngine::SetUnsupportedHandler(UnsupportedHandler handler, void* user) {
    impl->unsupported_handler = handler;
    impl->unsupported_user = user;
}

void JitEngine::SetMemoryReader(MemoryReader reader) {
    impl->memory = std::move(reader);
}

BlockFn JitEngine::GetBlock(u64 va) {
    if (!impl->jit) {
        return nullptr;
    }
    {
        std::scoped_lock lk{impl->mutex};
        const auto it = impl->blocks.find(va);
        if (it != impl->blocks.end() && it->second.fn) {
            return it->second.fn;
        }
    }

    // Known AOT block whose symbol has not been resolved yet?
    {
        std::scoped_lock lk{impl->mutex};
        const auto it = impl->blocks.find(va);
        if (it != impl->blocks.end() && !it->second.fn) {
            if (auto sym = impl->jit->lookup(BlockSymbol(va))) {
                it->second.fn = sym->toPtr<BlockFn>();
                return it->second.fn;
            } else {
                llvm::consumeError(sym.takeError());
                impl->blocks.erase(it);
            }
        }
    }

    // Fallback JIT path: translate one block on demand.
    const GuestBlock block = DecodeBlock(impl->decoder, impl->memory, va);
    if (block.insts.empty()) {
        error = "undecodable guest code at " + BlockSymbol(va);
        return nullptr;
    }

    auto llvm_ctx = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>(BlockSymbol(va), *llvm_ctx);
    module->setDataLayout(impl->jit->getDataLayout());
    if (!EmitBlock(*module, block)) {
        error = "translation failed at " + BlockSymbol(va);
        return nullptr;
    }
    // O1 keeps latency low; hot paths should have been covered by AOT.
    OptimizeModule(*module, 1);

    auto& jd = impl->jit->getMainJITDylib();
    auto tracker = jd.createResourceTracker();
    llvm::orc::ThreadSafeModule tsm{std::move(module),
                                    llvm::orc::ThreadSafeContext{std::move(llvm_ctx)}};
    if (auto err = impl->jit->addIRModule(tracker, std::move(tsm))) {
        error = llvm::toString(std::move(err));
        return nullptr;
    }
    auto sym = impl->jit->lookup(BlockSymbol(va));
    if (!sym) {
        error = llvm::toString(sym.takeError());
        return nullptr;
    }
    const auto fn = sym->toPtr<BlockFn>();
    std::scoped_lock lk{impl->mutex};
    impl->blocks.insert_or_assign(va, Impl::BlockRecord{fn, block.end, std::move(tracker)});
    return fn;
}

void JitEngine::InvalidateRange(u64 va, u64 size) {
    std::scoped_lock lk{impl->mutex};
    auto it = impl->blocks.begin();
    while (it != impl->blocks.end()) {
        const bool overlaps = it->first < va + size && it->second.end > va;
        if (!overlaps) {
            ++it;
            continue;
        }
        if (it->second.tracker) {
            llvm::consumeError(it->second.tracker->remove());
        }
        it = impl->blocks.erase(it);
    }
}

ExitReason JitEngine::Run(GuestContext& ctx) {
    ctx.exit_reason = ExitReason::None;
    while (true) {
        if (ctx.rip == kExitRip) {
            return ExitReason::None;
        }

        // HLE diversion: native implementation registered at this address.
        HleFn hle_fn = nullptr;
        {
            std::scoped_lock lk{impl->mutex};
            const auto it = impl->hle.find(ctx.rip);
            if (it != impl->hle.end()) {
                hle_fn = it->second;
            }
        }
        if (hle_fn) {
            ctx.gpr[kRax] = hle_fn(ctx.gpr[kRdi], ctx.gpr[kRsi], ctx.gpr[kRdx], ctx.gpr[kRcx],
                                   ctx.gpr[kR8], ctx.gpr[kR9]);
            // Emulate the RET the native function replaces.
            ctx.rip = *reinterpret_cast<const u64*>(ctx.gpr[kRsp]);
            ctx.gpr[kRsp] += 8;
            continue;
        }

        const BlockFn fn = GetBlock(ctx.rip);
        if (!fn) {
            ctx.exit_reason = ExitReason::UnsupportedInstruction;
            ctx.exit_arg = ctx.rip;
            return ExitReason::UnsupportedInstruction;
        }
        ctx.rip = fn(&ctx);

        if (ctx.exit_reason != ExitReason::None) {
            const ExitReason reason = ctx.exit_reason;
            if (reason == ExitReason::UnsupportedInstruction && impl->unsupported_handler) {
                ctx.exit_reason = ExitReason::None;
                if (impl->unsupported_handler(ctx, impl->unsupported_user)) {
                    continue;
                }
            }
            return reason;
        }
    }
}

} // namespace Core::Recompiler
