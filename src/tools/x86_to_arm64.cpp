// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Interactive pipeline inspector: feed it raw x86-64 machine code bytes and
// it prints the disassembly, the LLVM IR the recompiler emits (before and
// after optimization), and the AArch64 assembly LLVM generates from it.
//
//   x86_to_arm64 "48 89 f8 48 01 f0 c3"
//   echo "31 c0 c3" | x86_to_arm64
//   x86_to_arm64 --triple native "89 f8 c3"

#include <cctype>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include "core/arm64_recompiler/ir_emitter.h"
#include "core/arm64_recompiler/llvm_helpers.h"
#include "core/arm64_recompiler/x86_decoder.h"

using namespace Core::Recompiler;

namespace {

std::vector<u8> ParseHex(const std::string& text) {
    std::vector<u8> bytes;
    std::string digits;
    for (char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            digits.push_back(c);
        }
    }
    for (size_t i = 0; i + 1 < digits.size(); i += 2) {
        bytes.push_back(static_cast<u8>(std::stoi(digits.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

} // namespace

int main(int argc, char** argv) {
    std::string triple = "aarch64-unknown-linux-android31";
    unsigned opt_level = 2;
    std::string hex;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--triple" && i + 1 < argc) {
            triple = argv[++i];
        } else if (arg == "-O0" || arg == "-O1" || arg == "-O2" || arg == "-O3") {
            opt_level = static_cast<unsigned>(arg[2] - '0');
        } else if (arg == "--help" || arg == "-h") {
            std::puts("usage: x86_to_arm64 [--triple <llvm-triple>|native] [-O0..-O3] [hex bytes]\n"
                      "Reads x86-64 machine code (hex) from arguments or stdin and prints the\n"
                      "disassembly, the recompiler's LLVM IR, and the target assembly.");
            return 0;
        } else {
            hex += arg + " ";
        }
    }
    if (hex.find_first_of("0123456789abcdefABCDEF") == std::string::npos) {
        std::stringstream ss;
        ss << std::cin.rdbuf();
        hex = ss.str();
    }

    std::vector<u8> code = ParseHex(hex);
    if (code.empty()) {
        std::fprintf(stderr, "no input bytes\n");
        return 1;
    }
    // Pad so the decoder can always read a full instruction window.
    code.resize(code.size() + ZYDIS_MAX_INSTRUCTION_LENGTH, 0xCC);

    const u64 base = 0x400000;
    const size_t real_size = code.size() - ZYDIS_MAX_INSTRUCTION_LENGTH;
    const MemoryReader read = [&](u64 va, size_t len) -> const u8* {
        if (va < base || va + len > base + code.size()) {
            return nullptr;
        }
        return code.data() + (va - base);
    };

    X86Decoder decoder;
    const GuestBlock block = DecodeBlock(decoder, read, base);
    if (block.insts.empty()) {
        std::fprintf(stderr, "could not decode any instruction\n");
        return 1;
    }

    std::printf("; ---- x86-64 input (%zu bytes) ----\n", real_size);
    for (const auto& di : block.insts) {
        std::printf("  %012llx  %s\n", static_cast<unsigned long long>(di.address),
                    decoder.Disassemble(di).c_str());
    }

    std::string error;
    auto tm = CreateTargetMachine(triple, "generic", "", error);
    if (!tm) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    llvm::LLVMContext ctx;
    llvm::Module module{"x86_to_arm64", ctx};
    module.setTargetTriple(tm->getTargetTriple().str());
    module.setDataLayout(tm->createDataLayout());
    if (!EmitBlock(module, block)) {
        std::fprintf(stderr, "translation failed\n");
        return 1;
    }

    std::printf("\n; ---- LLVM IR (unoptimized) ----\n");
    {
        std::string ir;
        llvm::raw_string_ostream os{ir};
        module.print(os, nullptr);
        std::fputs(os.str().c_str(), stdout);
    }

    OptimizeModule(module, opt_level);

    std::printf("\n; ---- LLVM IR (-O%u) ----\n", opt_level);
    {
        std::string ir;
        llvm::raw_string_ostream os{ir};
        module.print(os, nullptr);
        std::fputs(os.str().c_str(), stdout);
    }

    std::printf("\n; ---- %s assembly ----\n", tm->getTargetTriple().str().c_str());
    {
        llvm::SmallString<0> asm_out;
        llvm::raw_svector_ostream os{asm_out};
        llvm::legacy::PassManager pm;
        if (tm->addPassesToEmitFile(pm, os, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
            std::fprintf(stderr, "target cannot emit assembly\n");
            return 1;
        }
        pm.run(module);
        std::fputs(asm_out.c_str(), stdout);
    }
    return 0;
}
