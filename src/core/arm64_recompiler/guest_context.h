// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include "common/types.h"

namespace Core::Recompiler {

/// Reason codes for why a translated block returned control to the dispatcher.
enum class ExitReason : u64 {
    None = 0,
    /// Block ended on an instruction the translator does not support yet.
    /// exit_arg holds the guest RIP of the offending instruction.
    UnsupportedInstruction = 1,
    /// Guest executed a syscall/int instruction. exit_arg holds the RIP after it.
    Syscall = 2,
    /// Explicit stop requested by the host (e.g. thread teardown).
    HostRequest = 3,
};

/// Guest x86-64 register file, in the order Zydis enumerates RAX..R15.
enum GuestGpr : u32 {
    kRax = 0,
    kRcx,
    kRdx,
    kRbx,
    kRsp,
    kRbp,
    kRsi,
    kRdi,
    kR8,
    kR9,
    kR10,
    kR11,
    kR12,
    kR13,
    kR14,
    kR15,
    kNumGprs,
};

/// Architectural state of one emulated x86-64 hardware thread.
///
/// Translated code receives a pointer to this struct as its only argument and
/// reads/writes guest registers through it. The layout is part of the ABI
/// between the IR emitter and the dispatcher: field offsets are baked into
/// generated code, so any change here must be mirrored in ir_emitter.cpp
/// (which uses offsetof on this type, so it stays in sync automatically).
struct GuestContext {
    u64 gpr[kNumGprs];
    u64 rip;

    // Status flags are stored materialized (0/1 each) rather than as a packed
    // RFLAGS image. Translated code computes them eagerly after every
    // arithmetic instruction; LLVM's optimizer removes the dead computations
    // once branches inside a superblock are resolved.
    u8 zf;
    u8 sf;
    u8 cf;
    u8 of;
    u8 pf;
    u8 af;
    u8 df;
    u8 pad0;

    u64 fs_base;
    u64 gs_base;

    alignas(16) u128 xmm[16];

    // Dispatcher communication area.
    ExitReason exit_reason;
    u64 exit_arg;

    // Host-side scratch: guest stack allocation owned by the thread object.
    void* host_stack_base;
    u64 host_stack_size;
};

static_assert(offsetof(GuestContext, rip) == 16 * 8);
static_assert(alignof(GuestContext) == 16);

/// RIP value that makes the dispatcher return from Run(). The initial call
/// frame of a guest thread has this pushed as its return address.
inline constexpr u64 kExitRip = ~u64{0};

/// Signature of a translated basic block: takes the guest context, returns the
/// guest RIP to continue at.
using BlockFn = u64 (*)(GuestContext*);

} // namespace Core::Recompiler
