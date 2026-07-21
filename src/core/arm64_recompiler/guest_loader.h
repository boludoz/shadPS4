// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace Core::Recompiler {

/// Loads a PS4 executable (eboot.bin / .elf / .prx) into host memory so the
/// recompiler can run it. Accepts:
///   * plain ELF (homebrew, OpenOrbis output),
///   * "fake"/decrypted SELF (fSELF) and already-decrypted SELF dumps of your
///     own games — the encrypted-segment path is intentionally not handled;
///     supply decrypted dumps.
///
/// The loader mirrors shadPS4's linker: it maps PT_LOAD segments at their
/// virtual addresses, parses the PS4 dynamic table (DT_SCE_*), applies
/// relocations, and resolves imported symbols. Imports are reported to the
/// caller by (nid, module, library) so the HLE layer can bind a native
/// implementation or a logging stub; unresolved imports get a trampoline that
/// raises a guest fault with the symbol name, which is what makes "boots until
/// the first missing HLE call" observable instead of a silent crash.
class GuestLoader {
public:
    struct Symbol {
        std::string nid;         // 11-char encoded NID (or plain name for homebrew)
        std::string name;        // demangled name if known via aerolib, else nid
        u64 stub_slot;           // guest address of the GOT/PLT slot to fill
    };

    struct LoadedModule {
        u64 base = 0;            // load bias applied to p_vaddr
        u64 entry = 0;           // guest RIP to start at
        u64 image_low = 0;       // lowest mapped guest address
        u64 image_high = 0;      // one past the highest mapped guest address
        u64 tls_vaddr = 0;
        u64 tls_filesz = 0;
        u64 tls_memsz = 0;
        u64 tls_align = 0;
        u64 proc_param = 0;      // PT_SCE_PROCPARAM vaddr, 0 if none
        std::vector<u64> entry_points; // entry + module_start etc., for AOT discovery
        std::vector<Symbol> imports;
        bool is_shared = false;
    };

    /// Host allocator for guest memory. Must return a host pointer where the
    /// range [vaddr, vaddr+size) is readable/writable/executable, or nullptr.
    /// Default (used by tools/tests) maps anonymous RWX memory near vaddr.
    using Allocator = std::function<void*(u64 vaddr, u64 size)>;

    explicit GuestLoader(Allocator allocator = {});

    /// Loads the file. On success returns the module; on failure returns
    /// nullopt and GetError() describes why.
    std::optional<LoadedModule> Load(const std::filesystem::path& path);

    /// Address of the guest fault trampoline installed for unresolved imports.
    /// The syscall/unsupported handler recognizes it to report which HLE
    /// symbol a game hit first.
    u64 GetUnresolvedTrampoline() const {
        return unresolved_trampoline;
    }
    /// Symbol name mapped to a trampoline slot value, for diagnostics.
    const std::string* LookupTrampoline(u64 value) const;

    const std::string& GetError() const {
        return error;
    }

private:
    void InstallTrampoline();

    Allocator allocator;
    std::string error;
    u64 unresolved_trampoline = 0;
    std::unordered_map<u64, std::string> trampoline_names;
    std::vector<u8> file_bytes; // backing store when reading fSELF/ELF
};

/// Decodes an aerolib-style NID back to a human-readable symbol name, or
/// returns the NID unchanged if unknown.
std::string ResolveNidName(const std::string& nid);

} // namespace Core::Recompiler
