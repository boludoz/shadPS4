// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <fstream>

#include "core/arm64_recompiler/game_package.h"

namespace Core::Recompiler {

namespace {

// param.sfo stores param_fmt big-endian (see core/file_format/psf.h). These
// are the raw little-endian views of the on-disk bytes.
constexpr u16 kFmtTextRaw = 0x0402;    // PSFEntryFmt::Text 0x0204
constexpr u16 kFmtIntegerRaw = 0x0404; // PSFEntryFmt::Integer 0x0404

#pragma pack(push, 1)
struct PsfHeader {
    u8 magic[4]; // "\0PSF"
    u32 version;
    u32 key_table_offset;
    u32 data_table_offset;
    u32 index_table_entries;
};
struct PsfRawEntry {
    u16 key_offset;
    u16 param_fmt; // big-endian in file
    u32 param_len;
    u32 param_max_len;
    u32 data_offset;
};
#pragma pack(pop)

bool LooksLikeExecutable(const std::filesystem::path& p) {
    std::ifstream in{p, std::ios::binary};
    u8 magic[4]{};
    in.read(reinterpret_cast<char*>(magic), 4);
    if (!in) {
        return false;
    }
    const u8 elf[4] = {0x7F, 'E', 'L', 'F'};
    const u8 self[4] = {0x4F, 0x15, 0x3D, 0x1D}; // decrypted/fake SELF
    return std::memcmp(magic, elf, 4) == 0 || std::memcmp(magic, self, 4) == 0;
}

} // namespace

bool ParamSfo::Open(const std::filesystem::path& path) {
    entries.clear();
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return false;
    }
    std::vector<u8> buf{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (buf.size() < sizeof(PsfHeader)) {
        return false;
    }
    PsfHeader header;
    std::memcpy(&header, buf.data(), sizeof(header));
    const u8 magic[4] = {0x00, 'P', 'S', 'F'};
    if (std::memcmp(header.magic, magic, 4) != 0) {
        return false;
    }
    const u64 index_end = sizeof(PsfHeader) + u64{header.index_table_entries} * sizeof(PsfRawEntry);
    if (index_end > buf.size() || header.key_table_offset > buf.size() ||
        header.data_table_offset > buf.size()) {
        return false;
    }
    for (u32 i = 0; i < header.index_table_entries; ++i) {
        PsfRawEntry raw;
        std::memcpy(&raw, buf.data() + sizeof(PsfHeader) + i * sizeof(PsfRawEntry), sizeof(raw));
        const u64 key_off = u64{header.key_table_offset} + raw.key_offset;
        const u64 data_off = u64{header.data_table_offset} + raw.data_offset;
        if (key_off >= buf.size() || data_off + raw.param_len > buf.size()) {
            continue;
        }
        const char* key_begin = reinterpret_cast<const char*>(buf.data() + key_off);
        const size_t key_max = buf.size() - key_off;
        const size_t key_len = strnlen(key_begin, key_max);
        Value v;
        v.fmt = raw.param_fmt;
        v.data.assign(buf.begin() + data_off, buf.begin() + data_off + raw.param_len);
        entries.emplace_back(std::string{key_begin, key_len}, std::move(v));
    }
    return true;
}

std::optional<std::string> ParamSfo::GetString(const std::string& key) const {
    for (const auto& [k, v] : entries) {
        if (k == key && v.fmt == kFmtTextRaw) {
            const char* data = reinterpret_cast<const char*>(v.data.data());
            return std::string{data, strnlen(data, v.data.size())};
        }
    }
    return std::nullopt;
}

std::optional<s32> ParamSfo::GetInteger(const std::string& key) const {
    for (const auto& [k, v] : entries) {
        if (k == key && v.fmt == kFmtIntegerRaw && v.data.size() >= 4) {
            s32 value;
            std::memcpy(&value, v.data.data(), 4);
            return value;
        }
    }
    return std::nullopt;
}

std::optional<GamePackage> GamePackage::Resolve(const std::filesystem::path& path,
                                                std::string* error) {
    namespace fs = std::filesystem;
    GamePackage pkg;

    // Same resolution as desktop Emulator::Run(): a folder means
    // <folder>/eboot.bin; a file is used as-is with its parent as the dump
    // root.
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        pkg.game_dir = path;
        pkg.eboot_path = path / "eboot.bin";
    } else if (fs::is_regular_file(path, ec)) {
        pkg.eboot_path = path;
        pkg.game_dir = path.parent_path();
    } else {
        if (error) {
            *error = "path does not exist: " + path.string();
        }
        return std::nullopt;
    }
    if (!fs::is_regular_file(pkg.eboot_path, ec) || !LooksLikeExecutable(pkg.eboot_path)) {
        if (error) {
            *error = "no PS4 executable at " + pkg.eboot_path.string() +
                     " (expected a decrypted eboot.bin or ELF)";
        }
        return std::nullopt;
    }

    // Title metadata; absent for plain homebrew ELFs, which is fine.
    ParamSfo sfo;
    if (sfo.Open(pkg.game_dir / "sce_sys" / "param.sfo")) {
        pkg.title = sfo.GetString("TITLE").value_or("");
        pkg.title_id = sfo.GetString("TITLE_ID").value_or("");
        pkg.app_version = sfo.GetString("APP_VER").value_or("");
        pkg.content_id = sfo.GetString("CONTENT_ID").value_or("");
        pkg.system_version = sfo.GetInteger("SYSTEM_VER").value_or(0);
    }

    // Game-bundled shared modules, loaded before the eboot just like the
    // desktop linker does with its dependency list.
    const fs::path module_dir = pkg.game_dir / "sce_module";
    if (fs::is_directory(module_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(module_dir, ec)) {
            const auto ext = entry.path().extension().string();
            if (entry.is_regular_file() && (ext == ".prx" || ext == ".sprx")) {
                pkg.sce_modules.push_back(entry.path());
            }
        }
        std::sort(pkg.sce_modules.begin(), pkg.sce_modules.end());
    }
    return pkg;
}

} // namespace Core::Recompiler
