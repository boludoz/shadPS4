// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/arm64_recompiler/guest_context.h"
#include "core/arm64_recompiler/x86_decoder.h"

namespace Core::Recompiler {

/// Native function registered at a guest address (HLE import thunk). Called
/// with the guest's integer argument registers (SysV order rdi..r9); the
/// return value is placed in guest RAX.
using HleFn = u64 (*)(u64, u64, u64, u64, u64, u64);

/// Handler invoked when translated code exits on an instruction outside the
/// supported subset. Implementations interpret (or otherwise skip) at least
/// one instruction starting at ctx.rip and return true, or return false to
/// abort emulation. This is the connector where a full fallback engine such
/// as a FEXCore-based interpreter plugs in.
using UnsupportedHandler = bool (*)(GuestContext& ctx, void* user);

/// Handler invoked when the guest executes a `syscall` instruction. ctx.rip
/// already points past it; the handler emulates the call (number in RAX,
/// args in RDI,RSI,RDX,R10,R8,R9; result to RAX) and returns true, or false
/// to stop emulation.
using SyscallHandler = bool (*)(GuestContext& ctx, void* user);

/// Runtime translation engine: LLVM ORC based fallback JIT plus dispatcher.
///
/// Role in the architecture: the AOT pass covers everything reachable
/// statically before the game boots. Everything it could not see — code the
/// game generates at runtime (its internal shader/script JITs), unusual
/// indirect-branch targets, self-modifying code — funnels through this
/// engine, which translates one basic block at a time on demand.
class JitEngine {
public:
    JitEngine();
    ~JitEngine();

    JitEngine(const JitEngine&) = delete;
    JitEngine& operator=(const JitEngine&) = delete;

    /// Links an AOT-produced object (+ map) into the session so its blocks
    /// are found before any JIT translation happens.
    bool LoadAotObject(const std::filesystem::path& object_file,
                       const std::filesystem::path& map_file);

    /// Registers a native implementation for a guest address. The dispatcher
    /// diverts control to it instead of translating guest bytes, then
    /// resumes at the guest return address (emulating RET).
    void RegisterHleFunction(u64 guest_va, HleFn fn);

    void SetUnsupportedHandler(UnsupportedHandler handler, void* user);
    void SetSyscallHandler(SyscallHandler handler, void* user);

    /// Guest memory resolver used when translating at runtime. Defaults to
    /// the identity mapping (guest VA == host VA).
    void SetMemoryReader(MemoryReader reader);

    /// Runs guest code starting at ctx.rip until it returns to kExitRip or an
    /// unrecoverable exit occurs. Returns the final exit reason.
    ExitReason Run(GuestContext& ctx);

    /// Drops every cached translation overlapping [va, va+size). Connect this
    /// to memory-protection faults to support self-modifying code and the
    /// guest's own JITs.
    void InvalidateRange(u64 va, u64 size);

    /// Looks up (or translates) the block at va and returns its host entry.
    BlockFn GetBlock(u64 va);

    const std::string& GetError() const {
        return error;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::string error;
};

} // namespace Core::Recompiler
