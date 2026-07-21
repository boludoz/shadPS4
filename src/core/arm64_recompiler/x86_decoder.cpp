// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/arm64_recompiler/x86_decoder.h"

namespace Core::Recompiler {

MemoryReader IdentityMemoryReader() {
    return [](u64 va, size_t) -> const u8* { return reinterpret_cast<const u8*>(va); };
}

X86Decoder::X86Decoder() {
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
}

std::optional<DecodedInst> X86Decoder::Decode(const MemoryReader& read, u64 va) const {
    const u8* bytes = read(va, ZYDIS_MAX_INSTRUCTION_LENGTH);
    if (!bytes) {
        return std::nullopt;
    }
    DecodedInst di{};
    di.address = va;
    const ZyanStatus status = ZydisDecoderDecodeFull(&decoder, bytes, ZYDIS_MAX_INSTRUCTION_LENGTH,
                                                     &di.inst, di.operands);
    if (!ZYAN_SUCCESS(status)) {
        return std::nullopt;
    }
    return di;
}

std::string X86Decoder::Disassemble(const DecodedInst& di) const {
    char buffer[256];
    if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter, &di.inst, di.operands,
                                                      di.inst.operand_count_visible, buffer,
                                                      sizeof(buffer), di.address, nullptr))) {
        return "<format error>";
    }
    return buffer;
}

bool X86Decoder::EndsBlock(const ZydisDecodedInstruction& inst) {
    switch (inst.meta.category) {
    case ZYDIS_CATEGORY_RET:
    case ZYDIS_CATEGORY_CALL:
    case ZYDIS_CATEGORY_UNCOND_BR:
    case ZYDIS_CATEGORY_COND_BR:
    case ZYDIS_CATEGORY_SYSCALL:
    case ZYDIS_CATEGORY_INTERRUPT:
        return true;
    default:
        return inst.mnemonic == ZYDIS_MNEMONIC_UD2;
    }
}

std::optional<u64> X86Decoder::DirectTarget(const DecodedInst& di) {
    for (u8 i = 0; i < di.inst.operand_count; ++i) {
        const auto& op = di.operands[i];
        if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative) {
            u64 target = 0;
            if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&di.inst, &op, di.address, &target))) {
                return target;
            }
        }
    }
    return std::nullopt;
}

GuestBlock DecodeBlock(const X86Decoder& dec, const MemoryReader& read, u64 va, size_t max_insts) {
    GuestBlock block{};
    block.start = va;
    block.end = va;
    u64 cursor = va;
    for (size_t i = 0; i < max_insts; ++i) {
        auto di = dec.Decode(read, cursor);
        if (!di) {
            break;
        }
        cursor = di->NextAddress();
        const bool ends = X86Decoder::EndsBlock(di->inst);
        block.insts.push_back(std::move(*di));
        block.end = cursor;
        if (ends) {
            break;
        }
    }
    return block;
}

} // namespace Core::Recompiler
