// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <deque>
#include <fstream>
#include <unordered_set>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm-c/Target.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include "core/arm64_recompiler/aot_compiler.h"
#include "core/arm64_recompiler/ir_emitter.h"
#include "core/arm64_recompiler/llvm_helpers.h"

namespace Core::Recompiler {

namespace {

constexpr u32 kMapMagic = 0x50414D41; // "AMAP"

void InitializeLlvmTargets() {
    // Only the backends this recompiler can target: AArch64 (device / Android
    // AOT + JIT) and X86 (build host, used by tests and the CLI tool). Pulling
    // in every LLVM backend would bloat the emulator binary for no benefit.
    static const bool done = [] {
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();
        LLVMInitializeAArch64AsmParser();
#if defined(__x86_64__) || defined(_M_X64)
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmPrinter();
        LLVMInitializeX86AsmParser();
#endif
        return true;
    }();
    (void)done;
}

llvm::OptimizationLevel ToOptLevel(unsigned level) {
    switch (level) {
    case 0:
        return llvm::OptimizationLevel::O0;
    case 1:
        return llvm::OptimizationLevel::O1;
    case 3:
        return llvm::OptimizationLevel::O3;
    default:
        return llvm::OptimizationLevel::O2;
    }
}

} // namespace

void OptimizeModule(llvm::Module& module, unsigned opt_level) {
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    if (opt_level == 0) {
        pb.buildO0DefaultPipeline(llvm::OptimizationLevel::O0).run(module, mam);
    } else {
        pb.buildPerModuleDefaultPipeline(ToOptLevel(opt_level)).run(module, mam);
    }
}

std::unique_ptr<llvm::TargetMachine> CreateTargetMachine(const std::string& triple_in,
                                                         const std::string& cpu,
                                                         const std::string& features,
                                                         std::string& error) {
    InitializeLlvmTargets();
    const std::string triple =
        triple_in == "native" ? llvm::sys::getProcessTriple() : triple_in;
    std::string lookup_error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookup_error);
    if (!target) {
        error = "LLVM target lookup failed for '" + triple + "': " + lookup_error;
        return nullptr;
    }
    llvm::TargetOptions options;
    auto* tm = target->createTargetMachine(triple, cpu, features, options, llvm::Reloc::PIC_,
                                           std::nullopt, llvm::CodeGenOptLevel::Default);
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

AotCompiler::AotCompiler(Config config) : cfg{std::move(config)} {}

std::optional<AotCompiler::Result> AotCompiler::CompileRegion(
    const MemoryReader& read, u64 base, u64 size, const std::vector<u64>& entry_points,
    const std::filesystem::path& cache_dir, const std::string& name, const ProgressFn& progress) {
    error.clear();

    auto tm = CreateTargetMachine(cfg.triple, cfg.cpu, cfg.features, error);
    if (!tm) {
        return std::nullopt;
    }

    llvm::LLVMContext llvm_ctx;
    auto module = std::make_unique<llvm::Module>(name, llvm_ctx);
    module->setTargetTriple(tm->getTargetTriple().str());
    module->setDataLayout(tm->createDataLayout());

    X86Decoder decoder;
    const auto in_region = [&](u64 va) { return va >= base && va < base + size; };

    // Worklist-driven block discovery: entry points plus every direct
    // branch/call target that lands inside the region. Indirect targets are
    // resolved at runtime by the fallback JIT.
    std::deque<u64> worklist;
    std::unordered_set<u64> seen;
    for (u64 entry : entry_points) {
        if (in_region(entry) && seen.insert(entry).second) {
            worklist.push_back(entry);
        }
    }

    Result result{};
    std::vector<AotMapEntry> map;
    while (!worklist.empty() && result.blocks_translated < cfg.max_blocks) {
        const u64 va = worklist.front();
        worklist.pop_front();

        const GuestBlock block = DecodeBlock(decoder, read, va);
        if (block.insts.empty()) {
            continue;
        }
        if (!EmitBlock(*module, block)) {
            continue;
        }
        result.blocks_translated++;
        result.insts_translated += block.insts.size();
        map.push_back({block.start, block.end});

        const auto& last = block.insts.back();
        if (const auto target = X86Decoder::DirectTarget(last)) {
            if (in_region(*target) && seen.insert(*target).second) {
                worklist.push_back(*target);
            }
        }
        // Fallthrough successor for conditional branches and calls.
        if (last.inst.meta.category == ZYDIS_CATEGORY_COND_BR ||
            last.inst.meta.category == ZYDIS_CATEGORY_CALL) {
            const u64 next = last.NextAddress();
            if (in_region(next) && seen.insert(next).second) {
                worklist.push_back(next);
            }
        }

        if (progress && (result.blocks_translated % 4096) == 0) {
            if (!progress(result.blocks_translated, result.blocks_translated + worklist.size())) {
                error = "cancelled";
                return std::nullopt;
            }
        }
    }

    std::string verify_error;
    llvm::raw_string_ostream verify_stream{verify_error};
    if (llvm::verifyModule(*module, &verify_stream)) {
        error = "IR verification failed: " + verify_stream.str();
        return std::nullopt;
    }

    OptimizeModule(*module, cfg.opt_level);

    std::filesystem::create_directories(cache_dir);
    result.object_file = cache_dir / (name + ".o");
    result.map_file = cache_dir / (name + ".map");

    {
        std::error_code ec;
        llvm::raw_fd_ostream out{result.object_file.string(), ec, llvm::sys::fs::OF_None};
        if (ec) {
            error = "cannot open " + result.object_file.string() + ": " + ec.message();
            return std::nullopt;
        }
        llvm::legacy::PassManager pm;
        if (tm->addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            error = "target cannot emit object files";
            return std::nullopt;
        }
        pm.run(*module);
    }

    {
        std::ofstream out{result.map_file, std::ios::binary};
        const u32 magic = kMapMagic;
        const u32 count = static_cast<u32>(map.size());
        out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        out.write(reinterpret_cast<const char*>(map.data()),
                  static_cast<std::streamsize>(map.size() * sizeof(AotMapEntry)));
        if (!out) {
            error = "cannot write " + result.map_file.string();
            return std::nullopt;
        }
    }

    if (progress) {
        progress(result.blocks_translated, result.blocks_translated);
    }
    return result;
}

std::optional<std::vector<AotMapEntry>> ReadAotMap(const std::filesystem::path& map_file) {
    std::ifstream in{map_file, std::ios::binary};
    u32 magic = 0;
    u32 count = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in || magic != kMapMagic) {
        return std::nullopt;
    }
    std::vector<AotMapEntry> map(count);
    in.read(reinterpret_cast<char*>(map.data()),
            static_cast<std::streamsize>(count * sizeof(AotMapEntry)));
    if (!in) {
        return std::nullopt;
    }
    return map;
}

} // namespace Core::Recompiler
