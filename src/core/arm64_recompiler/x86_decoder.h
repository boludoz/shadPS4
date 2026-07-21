// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <Zydis/Zydis.h>
#include "common/types.h"

namespace Core::Recompiler {

/// One decoded guest instruction plus its location.
struct DecodedInst {
    u64 address;
    ZydisDecodedInstruction inst;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

    u64 NextAddress() const {
        return address + inst.length;
    }
};

/// Resolves a guest virtual address range to host-readable bytes.
///
/// At runtime (fallback JIT) guest memory is the flat host address space, so
/// the identity resolver applies. During AOT compilation the module image
/// lives in a staging buffer, so the resolver maps guest VAs into it.
using MemoryReader = std::function<const u8*(u64 va, size_t len)>;

/// Returns a resolver that treats guest VAs as host pointers (runtime case).
MemoryReader IdentityMemoryReader();

/// Thin wrapper over Zydis configured for x86-64 long mode.
class X86Decoder {
public:
    X86Decoder();

    /// Decodes one instruction at va. Returns nullopt on invalid encodings or
    /// when the resolver cannot supply the bytes.
    std::optional<DecodedInst> Decode(const MemoryReader& read, u64 va) const;

    /// Formats an instruction for logs/tools.
    std::string Disassemble(const DecodedInst& di) const;

    /// True when the instruction unconditionally ends a basic block
    /// (ret/jmp/call/ud2/syscall...).
    static bool EndsBlock(const ZydisDecodedInstruction& inst);

    /// For direct branches/calls, computes the target address.
    static std::optional<u64> DirectTarget(const DecodedInst& di);

private:
    ZydisDecoder decoder;
    ZydisFormatter formatter;
};

/// A guest basic block: [start, end) with its decoded instructions.
struct GuestBlock {
    u64 start;
    u64 end; // address one past the last instruction
    std::vector<DecodedInst> insts;
};

/// Decodes a basic block starting at va. Stops at the first control-flow
/// instruction, at an undecodable byte sequence, or after max_insts.
GuestBlock DecodeBlock(const X86Decoder& dec, const MemoryReader& read, u64 va,
                       size_t max_insts = 512);

} // namespace Core::Recompiler
