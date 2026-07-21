// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/arm64_recompiler/x86_decoder.h"

namespace Core::Recompiler {

/// Ahead-of-time translation of a module's executable segments.
///
/// This is the "loading screen" pass: it walks every reachable basic block of
/// the guest ELF's code, lowers it to LLVM IR, optimizes, and emits a single
/// AArch64 (or any requested triple) object file plus a map of guest RIP ->
/// translated symbol. At runtime the object is linked into the same ORC
/// session used by the fallback JIT, so AOT and JIT blocks interoperate.
class AotCompiler {
public:
    struct Config {
        /// LLVM target triple to compile for. Defaults to the Android arm64
        /// platform; use "native" for the host (useful for tests/tools).
        std::string triple = "aarch64-unknown-linux-android31";
        std::string cpu = "generic";
        std::string features;
        /// 0..3 -> O0..O3.
        unsigned opt_level = 2;
        /// Upper bound of blocks to translate (safety valve).
        size_t max_blocks = 1'000'000;
    };

    struct Result {
        size_t blocks_translated = 0;
        size_t insts_translated = 0;
        /// Blocks that ended on an unsupported instruction. These still get a
        /// (partial) translation; execution exits to the fallback engine at
        /// the unsupported point.
        size_t partial_blocks = 0;
        std::filesystem::path object_file;
        std::filesystem::path map_file;
    };

    /// Progress callback: (blocks_done, blocks_discovered). Return false to
    /// cancel (e.g. frontend Activity going away).
    using ProgressFn = std::function<bool(size_t, size_t)>;

    explicit AotCompiler(Config config);

    /// Translates all code reachable from entry_points within [base, base+size)
    /// and writes "<name>.o" and "<name>.map" into cache_dir.
    /// read() must resolve guest VAs of that range to host memory.
    /// Returns std::nullopt on failure (message via GetError()).
    std::optional<Result> CompileRegion(const MemoryReader& read, u64 base, u64 size,
                                        const std::vector<u64>& entry_points,
                                        const std::filesystem::path& cache_dir,
                                        const std::string& name,
                                        const ProgressFn& progress = {});

    const std::string& GetError() const {
        return error;
    }

private:
    Config cfg;
    std::string error;
};

/// Entry of the sidecar map file (binary, little-endian u64 pairs after a
/// small header). Loaded at runtime to pre-populate the dispatcher cache.
struct AotMapEntry {
    u64 guest_rip;
    u64 block_end;
};

/// Reads a ".map" sidecar produced by AotCompiler.
std::optional<std::vector<AotMapEntry>> ReadAotMap(const std::filesystem::path& map_file);

} // namespace Core::Recompiler
