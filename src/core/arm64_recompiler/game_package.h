// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"

namespace Core::Recompiler {

/// A dumped game as shadPS4 expects it on disk:
///   <dump>/eboot.bin              main executable
///   <dump>/sce_sys/param.sfo      title metadata
///   <dump>/sce_module/*.prx       game-bundled shared modules
///   <dump>/...                    assets, mounted as /app0
///
/// GamePackage mirrors the desktop Emulator::Run() front half: given either
/// the dump folder or the eboot.bin inside it, it resolves the executable,
/// reads param.sfo and enumerates the bundled modules, so the Android host
/// loads a copy exactly the way the desktop build does.
struct GamePackage {
    std::filesystem::path game_dir;   // dump root (mounted as /app0)
    std::filesystem::path eboot_path; // main executable inside game_dir
    std::vector<std::filesystem::path> sce_modules; // sce_module/*.prx|*.sprx

    // From sce_sys/param.sfo (empty/0 when the dump has none, e.g. homebrew).
    std::string title;
    std::string title_id;
    std::string app_version;
    std::string content_id;
    s32 system_version = 0;

    /// Accepts a dump folder, or a path to eboot.bin / an ELF inside one.
    /// Returns nullopt with `error` filled when no executable can be found.
    static std::optional<GamePackage> Resolve(const std::filesystem::path& path,
                                              std::string* error = nullptr);
};

/// Minimal reader for Sony's PSF format (sce_sys/param.sfo). Self-contained
/// (no dependency on the desktop core) so it links into the Android .so.
/// Layout mirrors core/file_format/psf.h.
class ParamSfo {
public:
    /// Parses the file; returns false if it is missing or malformed.
    bool Open(const std::filesystem::path& path);

    std::optional<std::string> GetString(const std::string& key) const;
    std::optional<s32> GetInteger(const std::string& key) const;

private:
    struct Value {
        u16 fmt; // raw param_fmt as stored (big-endian in file)
        std::vector<u8> data;
    };
    std::vector<std::pair<std::string, Value>> entries;
};

} // namespace Core::Recompiler
