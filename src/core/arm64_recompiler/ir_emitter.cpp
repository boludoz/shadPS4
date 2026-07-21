// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Translates decoded x86-64 basic blocks into LLVM IR. The IR is deliberately
// naive (every guest register access goes through the GuestContext), relying
// on LLVM's optimizer to promote registers and delete dead flag computations.
// Instructions outside the supported subset exit to the dispatcher, which
// forwards them to the fallback-JIT/interpreter connector.

#include <cstddef>
#include <optional>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>

#include "core/arm64_recompiler/ir_emitter.h"

namespace Core::Recompiler {

std::string BlockSymbol(u64 guest_rip) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "gb_%llx", static_cast<unsigned long long>(guest_rip));
    return buf;
}

namespace {

using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::Function;
using llvm::FunctionType;
using llvm::IRBuilder;
using llvm::Module;
using llvm::Type;
using llvm::Value;

struct RegLoc {
    u32 index;
    u32 bits;
    bool high8;
};

std::optional<RegLoc> MapGpr(ZydisRegister reg) {
    if (reg >= ZYDIS_REGISTER_RAX && reg <= ZYDIS_REGISTER_R15) {
        return RegLoc{static_cast<u32>(reg - ZYDIS_REGISTER_RAX), 64, false};
    }
    if (reg >= ZYDIS_REGISTER_EAX && reg <= ZYDIS_REGISTER_R15D) {
        return RegLoc{static_cast<u32>(reg - ZYDIS_REGISTER_EAX), 32, false};
    }
    if (reg >= ZYDIS_REGISTER_AX && reg <= ZYDIS_REGISTER_R15W) {
        return RegLoc{static_cast<u32>(reg - ZYDIS_REGISTER_AX), 16, false};
    }
    if (reg >= ZYDIS_REGISTER_AL && reg <= ZYDIS_REGISTER_BL) {
        return RegLoc{static_cast<u32>(reg - ZYDIS_REGISTER_AL), 8, false};
    }
    if (reg >= ZYDIS_REGISTER_AH && reg <= ZYDIS_REGISTER_BH) {
        return RegLoc{static_cast<u32>(reg - ZYDIS_REGISTER_AH), 8, true};
    }
    if (reg >= ZYDIS_REGISTER_SPL && reg <= ZYDIS_REGISTER_DIL) {
        return RegLoc{static_cast<u32>(reg - ZYDIS_REGISTER_SPL) + kRsp, 8, false};
    }
    if (reg >= ZYDIS_REGISTER_R8B && reg <= ZYDIS_REGISTER_R15B) {
        return RegLoc{static_cast<u32>(reg - ZYDIS_REGISTER_R8B) + kR8, 8, false};
    }
    return std::nullopt;
}

std::optional<u32> MapXmm(ZydisRegister reg) {
    if (reg >= ZYDIS_REGISTER_XMM0 && reg <= ZYDIS_REGISTER_XMM15) {
        return static_cast<u32>(reg - ZYDIS_REGISTER_XMM0);
    }
    return std::nullopt;
}

/// x86 condition codes, in terms of materialized flags.
enum class Cond {
    O,
    NO,
    B,
    NB,
    Z,
    NZ,
    BE,
    NBE,
    S,
    NS,
    P,
    NP,
    L,
    NL,
    LE,
    NLE,
};

std::optional<Cond> CondFromMnemonic(ZydisMnemonic m) {
    switch (m) {
    case ZYDIS_MNEMONIC_JO:
    case ZYDIS_MNEMONIC_SETO:
    case ZYDIS_MNEMONIC_CMOVO:
        return Cond::O;
    case ZYDIS_MNEMONIC_JNO:
    case ZYDIS_MNEMONIC_SETNO:
    case ZYDIS_MNEMONIC_CMOVNO:
        return Cond::NO;
    case ZYDIS_MNEMONIC_JB:
    case ZYDIS_MNEMONIC_SETB:
    case ZYDIS_MNEMONIC_CMOVB:
        return Cond::B;
    case ZYDIS_MNEMONIC_JNB:
    case ZYDIS_MNEMONIC_SETNB:
    case ZYDIS_MNEMONIC_CMOVNB:
        return Cond::NB;
    case ZYDIS_MNEMONIC_JZ:
    case ZYDIS_MNEMONIC_SETZ:
    case ZYDIS_MNEMONIC_CMOVZ:
        return Cond::Z;
    case ZYDIS_MNEMONIC_JNZ:
    case ZYDIS_MNEMONIC_SETNZ:
    case ZYDIS_MNEMONIC_CMOVNZ:
        return Cond::NZ;
    case ZYDIS_MNEMONIC_JBE:
    case ZYDIS_MNEMONIC_SETBE:
    case ZYDIS_MNEMONIC_CMOVBE:
        return Cond::BE;
    case ZYDIS_MNEMONIC_JNBE:
    case ZYDIS_MNEMONIC_SETNBE:
    case ZYDIS_MNEMONIC_CMOVNBE:
        return Cond::NBE;
    case ZYDIS_MNEMONIC_JS:
    case ZYDIS_MNEMONIC_SETS:
    case ZYDIS_MNEMONIC_CMOVS:
        return Cond::S;
    case ZYDIS_MNEMONIC_JNS:
    case ZYDIS_MNEMONIC_SETNS:
    case ZYDIS_MNEMONIC_CMOVNS:
        return Cond::NS;
    case ZYDIS_MNEMONIC_JP:
    case ZYDIS_MNEMONIC_SETP:
    case ZYDIS_MNEMONIC_CMOVP:
        return Cond::P;
    case ZYDIS_MNEMONIC_JNP:
    case ZYDIS_MNEMONIC_SETNP:
    case ZYDIS_MNEMONIC_CMOVNP:
        return Cond::NP;
    case ZYDIS_MNEMONIC_JL:
    case ZYDIS_MNEMONIC_SETL:
    case ZYDIS_MNEMONIC_CMOVL:
        return Cond::L;
    case ZYDIS_MNEMONIC_JNL:
    case ZYDIS_MNEMONIC_SETNL:
    case ZYDIS_MNEMONIC_CMOVNL:
        return Cond::NL;
    case ZYDIS_MNEMONIC_JLE:
    case ZYDIS_MNEMONIC_SETLE:
    case ZYDIS_MNEMONIC_CMOVLE:
        return Cond::LE;
    case ZYDIS_MNEMONIC_JNLE:
    case ZYDIS_MNEMONIC_SETNLE:
    case ZYDIS_MNEMONIC_CMOVNLE:
        return Cond::NLE;
    default:
        return std::nullopt;
    }
}

class BlockEmitter {
public:
    BlockEmitter(Module& module, const GuestBlock& block)
        : mod{module}, blk{block}, ctx{module.getContext()}, b{module.getContext()} {}

    Function* Emit();

private:
    Value* CtxGep(u64 offset) {
        return b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), ctx_arg, offset);
    }

    Value* LoadCtx(u64 offset, u32 bits) {
        return b.CreateLoad(b.getIntNTy(bits), CtxGep(offset));
    }

    void StoreCtx(u64 offset, Value* v) {
        b.CreateStore(v, CtxGep(offset));
    }

    Value* LoadGprRaw(u32 index) {
        return LoadCtx(offsetof(GuestContext, gpr) + index * 8, 64);
    }

    void StoreGprRaw(u32 index, Value* v64) {
        StoreCtx(offsetof(GuestContext, gpr) + index * 8, v64);
    }

    Value* LoadReg(const RegLoc& loc) {
        Value* full = LoadGprRaw(loc.index);
        if (loc.high8) {
            full = b.CreateLShr(full, 8);
        }
        return loc.bits == 64 ? full : b.CreateTrunc(full, b.getIntNTy(loc.bits));
    }

    void StoreReg(const RegLoc& loc, Value* v) {
        if (loc.bits == 64) {
            StoreGprRaw(loc.index, v);
            return;
        }
        if (loc.bits == 32) {
            // 32-bit writes zero the upper half.
            StoreGprRaw(loc.index, b.CreateZExt(v, b.getInt64Ty()));
            return;
        }
        Value* full = LoadGprRaw(loc.index);
        const u64 mask = loc.high8 ? ~u64{0xFF00} : ~((u64{1} << loc.bits) - 1);
        Value* ext = b.CreateZExt(v, b.getInt64Ty());
        if (loc.high8) {
            ext = b.CreateShl(ext, 8);
        }
        StoreGprRaw(loc.index, b.CreateOr(b.CreateAnd(full, mask), ext));
    }

    Value* LoadXmm(u32 index) {
        return LoadCtx(offsetof(GuestContext, xmm) + index * 16, 128);
    }

    void StoreXmm(u32 index, Value* v128) {
        StoreCtx(offsetof(GuestContext, xmm) + index * 16, v128);
    }

    // Flag accessors: flags live as 0/1 bytes in the context.
    Value* LoadFlag(u64 offset) {
        return b.CreateTrunc(LoadCtx(offset, 8), b.getInt1Ty());
    }

    void StoreFlag(u64 offset, Value* i1) {
        StoreCtx(offset, b.CreateZExt(i1, b.getInt8Ty()));
    }

    static constexpr u64 kZf = offsetof(GuestContext, zf);
    static constexpr u64 kSf = offsetof(GuestContext, sf);
    static constexpr u64 kCf = offsetof(GuestContext, cf);
    static constexpr u64 kOf = offsetof(GuestContext, of);
    static constexpr u64 kPf = offsetof(GuestContext, pf);
    static constexpr u64 kAf = offsetof(GuestContext, af);

    Value* Parity(Value* res) {
        Value* low = b.CreateTrunc(res, b.getInt8Ty());
        Value* pop = b.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, low);
        return b.CreateICmpEQ(b.CreateAnd(pop, 1), ConstantInt::get(b.getInt8Ty(), 0));
    }

    void SetResultFlags(Value* res) {
        StoreFlag(kZf, b.CreateICmpEQ(res, ConstantInt::get(res->getType(), 0)));
        StoreFlag(kSf, b.CreateICmpSLT(res, ConstantInt::get(res->getType(), 0)));
        StoreFlag(kPf, Parity(res));
    }

    void SetAddFlags(Value* a, Value* bv, Value* res) {
        SetResultFlags(res);
        StoreFlag(kCf, b.CreateICmpULT(res, a));
        Value* sign = b.CreateAnd(b.CreateXor(a, res), b.CreateNot(b.CreateXor(a, bv)));
        StoreFlag(kOf, b.CreateICmpSLT(sign, ConstantInt::get(sign->getType(), 0)));
        Value* af = b.CreateAnd(b.CreateXor(b.CreateXor(a, bv), res),
                                ConstantInt::get(a->getType(), 0x10));
        StoreFlag(kAf, b.CreateICmpNE(af, ConstantInt::get(a->getType(), 0)));
    }

    void SetSubFlags(Value* a, Value* bv, Value* res) {
        SetResultFlags(res);
        StoreFlag(kCf, b.CreateICmpULT(a, bv));
        Value* sign = b.CreateAnd(b.CreateXor(a, bv), b.CreateXor(a, res));
        StoreFlag(kOf, b.CreateICmpSLT(sign, ConstantInt::get(sign->getType(), 0)));
        Value* af = b.CreateAnd(b.CreateXor(b.CreateXor(a, bv), res),
                                ConstantInt::get(a->getType(), 0x10));
        StoreFlag(kAf, b.CreateICmpNE(af, ConstantInt::get(a->getType(), 0)));
    }

    void SetLogicFlags(Value* res) {
        SetResultFlags(res);
        StoreFlag(kCf, b.getFalse());
        StoreFlag(kOf, b.getFalse());
    }

    Value* EvalCond(Cond c) {
        switch (c) {
        case Cond::O:
            return LoadFlag(kOf);
        case Cond::NO:
            return b.CreateNot(LoadFlag(kOf));
        case Cond::B:
            return LoadFlag(kCf);
        case Cond::NB:
            return b.CreateNot(LoadFlag(kCf));
        case Cond::Z:
            return LoadFlag(kZf);
        case Cond::NZ:
            return b.CreateNot(LoadFlag(kZf));
        case Cond::BE:
            return b.CreateOr(LoadFlag(kCf), LoadFlag(kZf));
        case Cond::NBE:
            return b.CreateNot(b.CreateOr(LoadFlag(kCf), LoadFlag(kZf)));
        case Cond::S:
            return LoadFlag(kSf);
        case Cond::NS:
            return b.CreateNot(LoadFlag(kSf));
        case Cond::P:
            return LoadFlag(kPf);
        case Cond::NP:
            return b.CreateNot(LoadFlag(kPf));
        case Cond::L:
            return b.CreateICmpNE(LoadFlag(kSf), LoadFlag(kOf));
        case Cond::NL:
            return b.CreateICmpEQ(LoadFlag(kSf), LoadFlag(kOf));
        case Cond::LE:
            return b.CreateOr(LoadFlag(kZf), b.CreateICmpNE(LoadFlag(kSf), LoadFlag(kOf)));
        case Cond::NLE:
            return b.CreateAnd(b.CreateNot(LoadFlag(kZf)),
                               b.CreateICmpEQ(LoadFlag(kSf), LoadFlag(kOf)));
        }
        return b.getFalse();
    }

    /// Effective address of a memory operand as i64.
    Value* Ea(const DecodedInst& di, const ZydisDecodedOperand& op) {
        Value* ea = ConstantInt::get(b.getInt64Ty(), static_cast<u64>(op.mem.disp.value));
        if (op.mem.base == ZYDIS_REGISTER_RIP) {
            ea = b.CreateAdd(ea, ConstantInt::get(b.getInt64Ty(), di.NextAddress()));
        } else if (op.mem.base != ZYDIS_REGISTER_NONE) {
            const auto base = MapGpr(op.mem.base);
            ea = b.CreateAdd(ea, LoadGprRaw(base->index));
        }
        if (op.mem.index != ZYDIS_REGISTER_NONE && op.mem.scale != 0) {
            const auto index = MapGpr(op.mem.index);
            Value* idx = LoadGprRaw(index->index);
            idx = b.CreateMul(idx, ConstantInt::get(b.getInt64Ty(), op.mem.scale));
            ea = b.CreateAdd(ea, idx);
        }
        if (op.mem.segment == ZYDIS_REGISTER_FS) {
            ea = b.CreateAdd(ea, LoadCtx(offsetof(GuestContext, fs_base), 64));
        } else if (op.mem.segment == ZYDIS_REGISTER_GS) {
            ea = b.CreateAdd(ea, LoadCtx(offsetof(GuestContext, gs_base), 64));
        }
        return ea;
    }

    Value* LoadMem(Value* ea, u32 bits) {
        Value* ptr = b.CreateIntToPtr(ea, b.getPtrTy());
        auto* load = b.CreateLoad(b.getIntNTy(bits), ptr);
        load->setAlignment(llvm::Align(1));
        return load;
    }

    void StoreMem(Value* ea, Value* v) {
        Value* ptr = b.CreateIntToPtr(ea, b.getPtrTy());
        auto* store = b.CreateStore(v, ptr);
        store->setAlignment(llvm::Align(1));
    }

    /// Loads an operand as an integer of `bits` width. Immediates are
    /// sign-extended to the requested width, matching x86 semantics for
    /// imm8/imm32 forms.
    Value* LoadOp(const DecodedInst& di, const ZydisDecodedOperand& op, u32 bits) {
        switch (op.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER: {
            if (const auto gpr = MapGpr(op.reg.value)) {
                Value* v = LoadReg(*gpr);
                return AdjustWidth(v, bits);
            }
            if (const auto xmm = MapXmm(op.reg.value)) {
                Value* v = LoadXmm(*xmm);
                return AdjustWidth(v, bits);
            }
            return nullptr;
        }
        case ZYDIS_OPERAND_TYPE_MEMORY:
            return LoadMem(Ea(di, op), bits);
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            return ConstantInt::get(b.getIntNTy(bits),
                                    static_cast<u64>(op.imm.value.s) &
                                        (bits == 64 ? ~u64{0} : ((u64{1} << bits) - 1)));
        default:
            return nullptr;
        }
    }

    Value* AdjustWidth(Value* v, u32 bits) {
        const u32 have = v->getType()->getIntegerBitWidth();
        if (have == bits) {
            return v;
        }
        return have > bits ? b.CreateTrunc(v, b.getIntNTy(bits))
                           : b.CreateZExt(v, b.getIntNTy(bits));
    }

    bool StoreOp(const DecodedInst& di, const ZydisDecodedOperand& op, Value* v) {
        switch (op.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER: {
            if (const auto gpr = MapGpr(op.reg.value)) {
                StoreReg(*gpr, AdjustWidth(v, gpr->bits));
                return true;
            }
            if (const auto xmm = MapXmm(op.reg.value)) {
                StoreXmm(*xmm, AdjustWidth(v, 128));
                return true;
            }
            return false;
        }
        case ZYDIS_OPERAND_TYPE_MEMORY:
            StoreMem(Ea(di, op), v);
            return true;
        default:
            return false;
        }
    }

    void Push64(Value* v64) {
        Value* rsp = b.CreateSub(LoadGprRaw(kRsp), ConstantInt::get(b.getInt64Ty(), 8));
        StoreGprRaw(kRsp, rsp);
        StoreMem(rsp, v64);
    }

    Value* Pop64() {
        Value* rsp = LoadGprRaw(kRsp);
        Value* v = LoadMem(rsp, 64);
        StoreGprRaw(kRsp, b.CreateAdd(rsp, ConstantInt::get(b.getInt64Ty(), 8)));
        return v;
    }

    /// Ends the block with an "unsupported instruction" exit at `rip`.
    Value* EmitUnsupportedExit(u64 rip) {
        StoreCtx(offsetof(GuestContext, exit_reason),
                 ConstantInt::get(b.getInt64Ty(),
                                  static_cast<u64>(ExitReason::UnsupportedInstruction)));
        StoreCtx(offsetof(GuestContext, exit_arg), ConstantInt::get(b.getInt64Ty(), rip));
        return ConstantInt::get(b.getInt64Ty(), rip);
    }

    Type* FpType(u32 bits) {
        return bits == 64 ? b.getDoubleTy() : b.getFloatTy();
    }

    /// Loads the low scalar lane of an xmm reg/mem operand as float/double.
    Value* LoadScalarFp(const DecodedInst& di, const ZydisDecodedOperand& op, u32 bits) {
        Value* raw = nullptr;
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const auto xmm = MapXmm(op.reg.value);
            if (!xmm) {
                return nullptr;
            }
            raw = b.CreateTrunc(LoadXmm(*xmm), b.getIntNTy(bits));
        } else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            raw = LoadMem(Ea(di, op), bits);
        } else {
            return nullptr;
        }
        return b.CreateBitCast(raw, FpType(bits));
    }

    /// Stores into the low lane of an xmm register, preserving the upper bits.
    bool StoreScalarFpToXmm(const ZydisDecodedOperand& op, Value* fp, u32 bits) {
        const auto xmm = MapXmm(op.reg.value);
        if (!xmm) {
            return false;
        }
        Value* as_int = b.CreateZExt(b.CreateBitCast(fp, b.getIntNTy(bits)), b.getIntNTy(128));
        const u64 low_mask = bits == 64 ? ~u64{0} : 0xFFFFFFFFull;
        Value* mask =
            b.CreateNot(ConstantInt::get(b.getIntNTy(128), low_mask));
        Value* merged = b.CreateOr(b.CreateAnd(LoadXmm(*xmm), mask), as_int);
        StoreXmm(*xmm, merged);
        return true;
    }

    // Returns the value the function should return (next RIP) if the
    // instruction terminated the block, nullptr to continue, or sets
    // failed=true when the instruction is unsupported.
    Value* Translate(const DecodedInst& di, bool& failed);

    Module& mod;
    const GuestBlock& blk;
    llvm::LLVMContext& ctx;
    IRBuilder<> b;
    Value* ctx_arg = nullptr;
};

Value* BlockEmitter::Translate(const DecodedInst& di, bool& failed) {
    const auto& inst = di.inst;
    const auto* ops = di.operands;
    auto unsupported = [&]() -> Value* {
        failed = true;
        return EmitUnsupportedExit(di.address);
    };

    // LOCK-prefixed RMW needs host atomics; leave it to the fallback engine.
    if (inst.attributes & ZYDIS_ATTRIB_HAS_LOCK) {
        return unsupported();
    }
    // Legacy address-size overrides never show up in PS4 userland code.
    if (inst.address_width != 64 && inst.meta.category != ZYDIS_CATEGORY_NOP) {
        return unsupported();
    }

    const u32 width = ops[0].size ? ops[0].size : inst.operand_width;

    switch (inst.mnemonic) {
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_ENDBR64:
    case ZYDIS_MNEMONIC_PAUSE:
    case ZYDIS_MNEMONIC_PREFETCHT0:
    case ZYDIS_MNEMONIC_PREFETCHT1:
    case ZYDIS_MNEMONIC_PREFETCHT2:
    case ZYDIS_MNEMONIC_PREFETCHNTA:
        return nullptr;

    case ZYDIS_MNEMONIC_MOV: {
        Value* v = LoadOp(di, ops[1], width);
        if (!v || !StoreOp(di, ops[0], v)) {
            return unsupported();
        }
        return nullptr;
    }
    case ZYDIS_MNEMONIC_MOVZX:
    case ZYDIS_MNEMONIC_MOVSX:
    case ZYDIS_MNEMONIC_MOVSXD: {
        Value* src = LoadOp(di, ops[1], ops[1].size);
        if (!src) {
            return unsupported();
        }
        Value* ext = inst.mnemonic == ZYDIS_MNEMONIC_MOVZX
                         ? b.CreateZExt(src, b.getIntNTy(width))
                         : b.CreateSExt(src, b.getIntNTy(width));
        if (!StoreOp(di, ops[0], ext)) {
            return unsupported();
        }
        return nullptr;
    }
    case ZYDIS_MNEMONIC_LEA: {
        if (ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY) {
            return unsupported();
        }
        Value* ea = Ea(di, ops[1]);
        if (!StoreOp(di, ops[0], AdjustWidth(ea, width))) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR:
    case ZYDIS_MNEMONIC_XOR:
    case ZYDIS_MNEMONIC_CMP:
    case ZYDIS_MNEMONIC_TEST: {
        Value* a = LoadOp(di, ops[0], width);
        Value* c = LoadOp(di, ops[1], width);
        if (!a || !c) {
            return unsupported();
        }
        Value* res = nullptr;
        switch (inst.mnemonic) {
        case ZYDIS_MNEMONIC_ADD:
            res = b.CreateAdd(a, c);
            SetAddFlags(a, c, res);
            break;
        case ZYDIS_MNEMONIC_SUB:
        case ZYDIS_MNEMONIC_CMP:
            res = b.CreateSub(a, c);
            SetSubFlags(a, c, res);
            break;
        case ZYDIS_MNEMONIC_AND:
        case ZYDIS_MNEMONIC_TEST:
            res = b.CreateAnd(a, c);
            SetLogicFlags(res);
            break;
        case ZYDIS_MNEMONIC_OR:
            res = b.CreateOr(a, c);
            SetLogicFlags(res);
            break;
        case ZYDIS_MNEMONIC_XOR:
            res = b.CreateXor(a, c);
            SetLogicFlags(res);
            break;
        default:
            break;
        }
        const bool writes = inst.mnemonic != ZYDIS_MNEMONIC_CMP &&
                            inst.mnemonic != ZYDIS_MNEMONIC_TEST;
        if (writes && !StoreOp(di, ops[0], res)) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_INC:
    case ZYDIS_MNEMONIC_DEC: {
        Value* a = LoadOp(di, ops[0], width);
        if (!a) {
            return unsupported();
        }
        Value* one = ConstantInt::get(a->getType(), 1);
        const bool inc = inst.mnemonic == ZYDIS_MNEMONIC_INC;
        Value* res = inc ? b.CreateAdd(a, one) : b.CreateSub(a, one);
        // INC/DEC preserve CF.
        Value* old_cf = LoadFlag(kCf);
        if (inc) {
            SetAddFlags(a, one, res);
        } else {
            SetSubFlags(a, one, res);
        }
        StoreFlag(kCf, old_cf);
        if (!StoreOp(di, ops[0], res)) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_NEG: {
        Value* a = LoadOp(di, ops[0], width);
        if (!a) {
            return unsupported();
        }
        Value* zero = ConstantInt::get(a->getType(), 0);
        Value* res = b.CreateSub(zero, a);
        SetSubFlags(zero, a, res);
        if (!StoreOp(di, ops[0], res)) {
            return unsupported();
        }
        return nullptr;
    }
    case ZYDIS_MNEMONIC_NOT: {
        Value* a = LoadOp(di, ops[0], width);
        if (!a || !StoreOp(di, ops[0], b.CreateNot(a))) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_IMUL: {
        if (inst.operand_count_visible == 1) {
            return unsupported();
        }
        Value* a = nullptr;
        Value* c = nullptr;
        if (inst.operand_count_visible == 2) {
            a = LoadOp(di, ops[0], width);
            c = LoadOp(di, ops[1], width);
        } else {
            a = LoadOp(di, ops[1], width);
            c = LoadOp(di, ops[2], width);
        }
        if (!a || !c) {
            return unsupported();
        }
        Value* mul = b.CreateBinaryIntrinsic(llvm::Intrinsic::smul_with_overflow, a, c);
        Value* res = b.CreateExtractValue(mul, 0);
        Value* ovf = b.CreateExtractValue(mul, 1);
        SetResultFlags(res);
        StoreFlag(kCf, ovf);
        StoreFlag(kOf, ovf);
        if (!StoreOp(di, ops[0], res)) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_MUL:
    case ZYDIS_MNEMONIC_DIV:
    case ZYDIS_MNEMONIC_IDIV: {
        if (width < 16) {
            return unsupported();
        }
        Value* a = LoadOp(di, ops[0], width);
        if (!a) {
            return unsupported();
        }
        const RegLoc rax{kRax, width, false};
        const RegLoc rdx{kRdx, width, false};
        Type* wide = b.getIntNTy(width * 2);
        if (inst.mnemonic == ZYDIS_MNEMONIC_MUL) {
            Value* lo = LoadReg(rax);
            Value* full = b.CreateMul(b.CreateZExt(lo, wide), b.CreateZExt(a, wide));
            Value* res_lo = b.CreateTrunc(full, b.getIntNTy(width));
            Value* res_hi = b.CreateTrunc(b.CreateLShr(full, width), b.getIntNTy(width));
            StoreReg(rax, res_lo);
            StoreReg(rdx, res_hi);
            Value* hi_nz = b.CreateICmpNE(res_hi, ConstantInt::get(res_hi->getType(), 0));
            StoreFlag(kCf, hi_nz);
            StoreFlag(kOf, hi_nz);
            return nullptr;
        }
        // DIV/IDIV: no #DE emulation; division by zero is host UB, the same
        // trade-off the interpreter-free fast path takes elsewhere.
        Value* lo = b.CreateZExt(LoadReg(rax), wide);
        Value* hi = b.CreateZExt(LoadReg(rdx), wide);
        Value* dividend = b.CreateOr(b.CreateShl(hi, width), lo);
        Value* divisor = inst.mnemonic == ZYDIS_MNEMONIC_DIV ? b.CreateZExt(a, wide)
                                                             : b.CreateSExt(a, wide);
        Value* quot = inst.mnemonic == ZYDIS_MNEMONIC_DIV ? b.CreateUDiv(dividend, divisor)
                                                          : b.CreateSDiv(dividend, divisor);
        Value* rem = inst.mnemonic == ZYDIS_MNEMONIC_DIV ? b.CreateURem(dividend, divisor)
                                                         : b.CreateSRem(dividend, divisor);
        StoreReg(rax, b.CreateTrunc(quot, b.getIntNTy(width)));
        StoreReg(rdx, b.CreateTrunc(rem, b.getIntNTy(width)));
        return nullptr;
    }

    case ZYDIS_MNEMONIC_SHL:
    case ZYDIS_MNEMONIC_SHR:
    case ZYDIS_MNEMONIC_SAR: {
        Value* a = LoadOp(di, ops[0], width);
        Value* amt_raw = inst.operand_count_visible > 1 ? LoadOp(di, ops[1], width)
                                                        : ConstantInt::get(b.getIntNTy(width), 1);
        if (!a || !amt_raw) {
            return unsupported();
        }
        const u64 mask = width == 64 ? 63 : 31;
        Value* amt = b.CreateAnd(amt_raw, ConstantInt::get(a->getType(), mask));
        Value* res = nullptr;
        switch (inst.mnemonic) {
        case ZYDIS_MNEMONIC_SHL:
            res = b.CreateShl(a, amt);
            break;
        case ZYDIS_MNEMONIC_SHR:
            res = b.CreateLShr(a, amt);
            break;
        default:
            res = b.CreateAShr(a, amt);
            break;
        }
        // Flags are unchanged when the masked amount is zero.
        Value* is_zero = b.CreateICmpEQ(amt, ConstantInt::get(a->getType(), 0));
        Value* old_zf = LoadFlag(kZf);
        Value* old_sf = LoadFlag(kSf);
        Value* old_pf = LoadFlag(kPf);
        Value* old_cf = LoadFlag(kCf);
        Value* zero_c = ConstantInt::get(a->getType(), 0);
        Value* new_zf = b.CreateICmpEQ(res, zero_c);
        Value* new_sf = b.CreateICmpSLT(res, zero_c);
        Value* new_pf = Parity(res);
        // CF = last bit shifted out.
        Value* width_c = ConstantInt::get(a->getType(), width);
        Value* new_cf = nullptr;
        if (inst.mnemonic == ZYDIS_MNEMONIC_SHL) {
            Value* sh = b.CreateSub(width_c, amt);
            new_cf = b.CreateTrunc(b.CreateLShr(a, sh), b.getInt1Ty());
        } else {
            Value* sh = b.CreateSub(amt, ConstantInt::get(a->getType(), 1));
            new_cf = b.CreateTrunc(b.CreateLShr(a, sh), b.getInt1Ty());
        }
        StoreFlag(kZf, b.CreateSelect(is_zero, old_zf, new_zf));
        StoreFlag(kSf, b.CreateSelect(is_zero, old_sf, new_sf));
        StoreFlag(kPf, b.CreateSelect(is_zero, old_pf, new_pf));
        StoreFlag(kCf, b.CreateSelect(is_zero, old_cf, new_cf));
        if (!StoreOp(di, ops[0], res)) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_ROL:
    case ZYDIS_MNEMONIC_ROR: {
        Value* a = LoadOp(di, ops[0], width);
        Value* amt = inst.operand_count_visible > 1 ? LoadOp(di, ops[1], width)
                                                    : ConstantInt::get(b.getIntNTy(width), 1);
        if (!a || !amt) {
            return unsupported();
        }
        const auto intr = inst.mnemonic == ZYDIS_MNEMONIC_ROL ? llvm::Intrinsic::fshl
                                                              : llvm::Intrinsic::fshr;
        Value* res = b.CreateIntrinsic(a->getType(), intr, {a, a, amt});
        if (!StoreOp(di, ops[0], res)) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_POPCNT:
    case ZYDIS_MNEMONIC_TZCNT:
    case ZYDIS_MNEMONIC_LZCNT:
    case ZYDIS_MNEMONIC_BSF:
    case ZYDIS_MNEMONIC_BSR: {
        Value* src = LoadOp(di, ops[1], width);
        if (!src) {
            return unsupported();
        }
        Value* res = nullptr;
        switch (inst.mnemonic) {
        case ZYDIS_MNEMONIC_POPCNT:
            res = b.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, src);
            break;
        case ZYDIS_MNEMONIC_TZCNT:
        case ZYDIS_MNEMONIC_BSF:
            res = b.CreateIntrinsic(src->getType(), llvm::Intrinsic::cttz,
                                    {src, b.getFalse()});
            break;
        default: {
            Value* clz = b.CreateIntrinsic(src->getType(), llvm::Intrinsic::ctlz,
                                           {src, b.getFalse()});
            if (inst.mnemonic == ZYDIS_MNEMONIC_LZCNT) {
                res = clz;
            } else { // BSR = width-1-clz
                res = b.CreateSub(ConstantInt::get(src->getType(), width - 1), clz);
            }
            break;
        }
        }
        StoreFlag(kZf, b.CreateICmpEQ(src, ConstantInt::get(src->getType(), 0)));
        if (!StoreOp(di, ops[0], res)) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_XCHG: {
        Value* a = LoadOp(di, ops[0], width);
        Value* c = LoadOp(di, ops[1], width);
        if (!a || !c || !StoreOp(di, ops[0], c) || !StoreOp(di, ops[1], a)) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_CDQE: { // also CBW/CWDE by operand width
        const u32 half = width / 2;
        Value* low = LoadReg(RegLoc{kRax, half, false});
        StoreReg(RegLoc{kRax, width, false}, b.CreateSExt(low, b.getIntNTy(width)));
        return nullptr;
    }
    case ZYDIS_MNEMONIC_CQO:
    case ZYDIS_MNEMONIC_CDQ:
    case ZYDIS_MNEMONIC_CWD: {
        Value* a = LoadReg(RegLoc{kRax, width, false});
        Value* sign = b.CreateAShr(a, ConstantInt::get(a->getType(), width - 1));
        StoreReg(RegLoc{kRdx, width, false}, sign);
        return nullptr;
    }

    case ZYDIS_MNEMONIC_PUSH: {
        Value* v = LoadOp(di, ops[0], 64);
        if (!v) {
            return unsupported();
        }
        if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            v = ConstantInt::get(b.getInt64Ty(), static_cast<u64>(ops[0].imm.value.s));
        }
        Push64(v);
        return nullptr;
    }
    case ZYDIS_MNEMONIC_POP: {
        if (!StoreOp(di, ops[0], Pop64())) {
            return unsupported();
        }
        return nullptr;
    }
    case ZYDIS_MNEMONIC_LEAVE: {
        StoreGprRaw(kRsp, LoadGprRaw(kRbp));
        StoreGprRaw(kRbp, Pop64());
        return nullptr;
    }

    case ZYDIS_MNEMONIC_CALL: {
        Push64(ConstantInt::get(b.getInt64Ty(), di.NextAddress()));
        if (const auto target = X86Decoder::DirectTarget(di)) {
            return ConstantInt::get(b.getInt64Ty(), *target);
        }
        Value* t = LoadOp(di, ops[0], 64);
        return t ? t : unsupported();
    }
    case ZYDIS_MNEMONIC_RET: {
        Value* target = Pop64();
        if (inst.operand_count_visible > 0 &&
            ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            Value* rsp = LoadGprRaw(kRsp);
            StoreGprRaw(kRsp, b.CreateAdd(rsp, ConstantInt::get(b.getInt64Ty(),
                                                                ops[0].imm.value.u)));
        }
        return target;
    }
    case ZYDIS_MNEMONIC_JMP: {
        if (const auto target = X86Decoder::DirectTarget(di)) {
            return ConstantInt::get(b.getInt64Ty(), *target);
        }
        Value* t = LoadOp(di, ops[0], 64);
        return t ? t : unsupported();
    }

    case ZYDIS_MNEMONIC_SYSCALL: {
        StoreCtx(offsetof(GuestContext, exit_reason),
                 ConstantInt::get(b.getInt64Ty(), static_cast<u64>(ExitReason::Syscall)));
        StoreCtx(offsetof(GuestContext, exit_arg),
                 ConstantInt::get(b.getInt64Ty(), di.NextAddress()));
        return ConstantInt::get(b.getInt64Ty(), di.NextAddress());
    }

    // SSE data movement subset.
    case ZYDIS_MNEMONIC_MOVAPS:
    case ZYDIS_MNEMONIC_MOVUPS:
    case ZYDIS_MNEMONIC_MOVAPD:
    case ZYDIS_MNEMONIC_MOVUPD:
    case ZYDIS_MNEMONIC_MOVDQA:
    case ZYDIS_MNEMONIC_MOVDQU: {
        Value* v = LoadOp(di, ops[1], 128);
        if (!v || !StoreOp(di, ops[0], v)) {
            return unsupported();
        }
        return nullptr;
    }
    case ZYDIS_MNEMONIC_MOVQ:
    case ZYDIS_MNEMONIC_MOVD: {
        const u32 bits = inst.mnemonic == ZYDIS_MNEMONIC_MOVQ ? 64 : 32;
        Value* v = LoadOp(di, ops[1], bits);
        if (!v) {
            return unsupported();
        }
        if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && MapXmm(ops[0].reg.value)) {
            // Moves into xmm zero the upper lanes.
            v = b.CreateZExt(v, b.getIntNTy(128));
        }
        if (!StoreOp(di, ops[0], v)) {
            return unsupported();
        }
        return nullptr;
    }
    case ZYDIS_MNEMONIC_XORPS:
    case ZYDIS_MNEMONIC_XORPD:
    case ZYDIS_MNEMONIC_PXOR: {
        Value* a = LoadOp(di, ops[0], 128);
        Value* c = LoadOp(di, ops[1], 128);
        if (!a || !c || !StoreOp(di, ops[0], b.CreateXor(a, c))) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_MOVSD:
    case ZYDIS_MNEMONIC_MOVSS: {
        if (inst.meta.category == ZYDIS_CATEGORY_STRINGOP) {
            return unsupported(); // rep movsd string form
        }
        const u32 bits = inst.mnemonic == ZYDIS_MNEMONIC_MOVSD ? 64 : 32;
        if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            Value* v = LoadScalarFp(di, ops[1], bits);
            if (!v) {
                return unsupported();
            }
            StoreMem(Ea(di, ops[0]), b.CreateBitCast(v, b.getIntNTy(bits)));
            return nullptr;
        }
        if (ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            // Load from memory zeroes the upper lanes.
            Value* v = LoadMem(Ea(di, ops[1]), bits);
            const auto xmm = MapXmm(ops[0].reg.value);
            if (!xmm) {
                return unsupported();
            }
            StoreXmm(*xmm, b.CreateZExt(v, b.getIntNTy(128)));
            return nullptr;
        }
        // reg,reg keeps the upper lanes of the destination.
        Value* v = LoadScalarFp(di, ops[1], bits);
        if (!v || !StoreScalarFpToXmm(ops[0], v, bits)) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_ADDSD:
    case ZYDIS_MNEMONIC_SUBSD:
    case ZYDIS_MNEMONIC_MULSD:
    case ZYDIS_MNEMONIC_DIVSD:
    case ZYDIS_MNEMONIC_ADDSS:
    case ZYDIS_MNEMONIC_SUBSS:
    case ZYDIS_MNEMONIC_MULSS:
    case ZYDIS_MNEMONIC_DIVSS: {
        const bool is_double = inst.mnemonic == ZYDIS_MNEMONIC_ADDSD ||
                               inst.mnemonic == ZYDIS_MNEMONIC_SUBSD ||
                               inst.mnemonic == ZYDIS_MNEMONIC_MULSD ||
                               inst.mnemonic == ZYDIS_MNEMONIC_DIVSD;
        const u32 bits = is_double ? 64 : 32;
        Value* a = LoadScalarFp(di, ops[0], bits);
        Value* c = LoadScalarFp(di, ops[1], bits);
        if (!a || !c) {
            return unsupported();
        }
        Value* res = nullptr;
        switch (inst.mnemonic) {
        case ZYDIS_MNEMONIC_ADDSD:
        case ZYDIS_MNEMONIC_ADDSS:
            res = b.CreateFAdd(a, c);
            break;
        case ZYDIS_MNEMONIC_SUBSD:
        case ZYDIS_MNEMONIC_SUBSS:
            res = b.CreateFSub(a, c);
            break;
        case ZYDIS_MNEMONIC_MULSD:
        case ZYDIS_MNEMONIC_MULSS:
            res = b.CreateFMul(a, c);
            break;
        default:
            res = b.CreateFDiv(a, c);
            break;
        }
        if (!StoreScalarFpToXmm(ops[0], res, bits)) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_UCOMISD:
    case ZYDIS_MNEMONIC_UCOMISS:
    case ZYDIS_MNEMONIC_COMISD:
    case ZYDIS_MNEMONIC_COMISS: {
        const u32 bits = (inst.mnemonic == ZYDIS_MNEMONIC_UCOMISD ||
                          inst.mnemonic == ZYDIS_MNEMONIC_COMISD)
                             ? 64
                             : 32;
        Value* a = LoadScalarFp(di, ops[0], bits);
        Value* c = LoadScalarFp(di, ops[1], bits);
        if (!a || !c) {
            return unsupported();
        }
        Value* uno = b.CreateFCmpUNO(a, c);
        StoreFlag(kZf, b.CreateOr(b.CreateFCmpOEQ(a, c), uno));
        StoreFlag(kCf, b.CreateOr(b.CreateFCmpOLT(a, c), uno));
        StoreFlag(kPf, uno);
        StoreFlag(kSf, b.getFalse());
        StoreFlag(kOf, b.getFalse());
        StoreFlag(kAf, b.getFalse());
        return nullptr;
    }

    case ZYDIS_MNEMONIC_CVTSI2SD:
    case ZYDIS_MNEMONIC_CVTSI2SS: {
        const u32 fp_bits = inst.mnemonic == ZYDIS_MNEMONIC_CVTSI2SD ? 64 : 32;
        Value* src = LoadOp(di, ops[1], ops[1].size);
        if (!src) {
            return unsupported();
        }
        Value* fp = b.CreateSIToFP(src, FpType(fp_bits));
        if (!StoreScalarFpToXmm(ops[0], fp, fp_bits)) {
            return unsupported();
        }
        return nullptr;
    }
    case ZYDIS_MNEMONIC_CVTTSD2SI:
    case ZYDIS_MNEMONIC_CVTTSS2SI: {
        const u32 fp_bits = inst.mnemonic == ZYDIS_MNEMONIC_CVTTSD2SI ? 64 : 32;
        Value* src = LoadScalarFp(di, ops[1], fp_bits);
        if (!src) {
            return unsupported();
        }
        Value* res = b.CreateFPToSI(src, b.getIntNTy(width));
        if (!StoreOp(di, ops[0], res)) {
            return unsupported();
        }
        return nullptr;
    }
    case ZYDIS_MNEMONIC_CVTSD2SS:
    case ZYDIS_MNEMONIC_CVTSS2SD: {
        const bool to_single = inst.mnemonic == ZYDIS_MNEMONIC_CVTSD2SS;
        Value* src = LoadScalarFp(di, ops[1], to_single ? 64 : 32);
        if (!src) {
            return unsupported();
        }
        Value* res = to_single ? b.CreateFPTrunc(src, b.getFloatTy())
                               : b.CreateFPExt(src, b.getDoubleTy());
        if (!StoreScalarFpToXmm(ops[0], res, to_single ? 32 : 64)) {
            return unsupported();
        }
        return nullptr;
    }

    case ZYDIS_MNEMONIC_STD:
        StoreCtx(offsetof(GuestContext, df), ConstantInt::get(b.getInt8Ty(), 1));
        return nullptr;
    case ZYDIS_MNEMONIC_CLD:
        StoreCtx(offsetof(GuestContext, df), ConstantInt::get(b.getInt8Ty(), 0));
        return nullptr;

    default:
        break;
    }

    // Conditional families share operand handling.
    if (const auto cond = CondFromMnemonic(inst.mnemonic)) {
        if (inst.meta.category == ZYDIS_CATEGORY_COND_BR) {
            const auto target = X86Decoder::DirectTarget(di);
            if (!target) {
                return unsupported();
            }
            Value* c = EvalCond(*cond);
            return b.CreateSelect(c, ConstantInt::get(b.getInt64Ty(), *target),
                                  ConstantInt::get(b.getInt64Ty(), di.NextAddress()));
        }
        if (inst.meta.category == ZYDIS_CATEGORY_SETCC) {
            Value* c = b.CreateZExt(EvalCond(*cond), b.getInt8Ty());
            if (!StoreOp(di, ops[0], c)) {
                return unsupported();
            }
            return nullptr;
        }
        if (inst.meta.category == ZYDIS_CATEGORY_CMOV) {
            Value* a = LoadOp(di, ops[0], width);
            Value* c = LoadOp(di, ops[1], width);
            if (!a || !c) {
                return unsupported();
            }
            // CMOV always writes the destination (important for 32-bit
            // zero-extension semantics), selecting old vs new value.
            Value* res = b.CreateSelect(EvalCond(*cond), c, a);
            if (!StoreOp(di, ops[0], res)) {
                return unsupported();
            }
            return nullptr;
        }
    }

    return unsupported();
}

Function* BlockEmitter::Emit() {
    if (blk.insts.empty()) {
        return nullptr;
    }
    FunctionType* fn_ty = FunctionType::get(Type::getInt64Ty(ctx), {llvm::PointerType::get(ctx, 0)},
                                            false);
    Function* fn = Function::Create(fn_ty, Function::ExternalLinkage, BlockSymbol(blk.start), mod);
    ctx_arg = fn->getArg(0);
    fn->getArg(0)->addAttr(llvm::Attribute::NoAlias);
    BasicBlock* entry = BasicBlock::Create(ctx, "entry", fn);
    b.SetInsertPoint(entry);

    Value* next_rip = nullptr;
    for (const auto& di : blk.insts) {
        bool failed = false;
        next_rip = Translate(di, failed);
        if (next_rip) {
            break;
        }
    }
    if (!next_rip) {
        // Fell off the end (max_insts cap): continue at the next address.
        next_rip = ConstantInt::get(b.getInt64Ty(), blk.end);
    }
    StoreCtx(offsetof(GuestContext, rip), next_rip);
    b.CreateRet(next_rip);
    return fn;
}

} // namespace

Function* EmitBlock(Module& module, const GuestBlock& block) {
    return BlockEmitter{module, block}.Emit();
}

} // namespace Core::Recompiler
