// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "core/arm64_recompiler/guest_context.h"
#include "core/arm64_recompiler/x86_decoder.h"

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace Core::Recompiler {

/// Name of the translated function for a guest block ("gb_<hex rip>").
/// This naming is shared between the AOT object cache and the fallback JIT so
/// both can be linked into the same ORC session.
std::string BlockSymbol(u64 guest_rip);

/// Translates one guest basic block into an LLVM function inside module.
///
/// The produced function has C signature `u64 fn(GuestContext*)` and returns
/// the guest RIP execution should continue at. Instructions outside the
/// supported subset end the block: the function stores
/// ExitReason::UnsupportedInstruction into the context and returns the RIP of
/// the offending instruction so the dispatcher can hand it to the fallback
/// interpreter connector.
///
/// Returns nullptr only if the block is empty (first instruction undecodable).
llvm::Function* EmitBlock(llvm::Module& module, const GuestBlock& block);

} // namespace Core::Recompiler
