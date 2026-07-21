// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Loads a decrypted PS4 executable (eboot.bin / .elf) and boots it through the
// x86 -> ARM64 recompiler until it exits or hits something not yet emulated.
// This is the desktop driver for the boot chain; the Android app calls the
// same Booter through the JNI host.
//
//   boot_x86_elf /path/to/eboot.bin
//   boot_x86_elf --imports /path/to/homebrew.elf   # list unresolved imports

#include <cstdio>
#include <string>

#include "core/arm64_recompiler/boot.h"
#include "core/arm64_recompiler/guest_loader.h"
#include "core/arm64_recompiler/jit_engine.h"

using namespace Core::Recompiler;

int main(int argc, char** argv) {
    std::string path;
    bool list_imports = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--imports") {
            list_imports = true;
        } else if (arg == "--help" || arg == "-h") {
            std::puts("usage: boot_x86_elf [--imports] <eboot.bin|.elf>");
            return 0;
        } else {
            path = arg;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr, "error: no input file (see --help)\n");
        return 2;
    }

    GuestLoader loader;
    const auto module = loader.Load(path);
    if (!module) {
        std::fprintf(stderr, "load failed: %s\n", loader.GetError().c_str());
        return 1;
    }

    std::printf("loaded %s\n", path.c_str());
    std::printf("  entry      : %#llx\n", (unsigned long long)module->entry);
    std::printf("  image      : %#llx .. %#llx (%llu KiB)\n",
                (unsigned long long)module->image_low, (unsigned long long)module->image_high,
                (unsigned long long)((module->image_high - module->image_low) / 1024));
    std::printf("  imports    : %zu\n", module->imports.size());
    if (module->proc_param) {
        std::printf("  procparam  : %#llx\n", (unsigned long long)module->proc_param);
    }

    if (list_imports) {
        for (const auto& sym : module->imports) {
            std::printf("    %-12s %s\n", sym.nid.c_str(), sym.name.c_str());
        }
        return 0;
    }

    JitEngine jit;
    if (!jit.GetError().empty()) {
        std::fprintf(stderr, "jit init failed: %s\n", jit.GetError().c_str());
        return 1;
    }

    Booter booter{jit, loader};
    std::printf("booting...\n\n");
    const BootResult r = booter.Boot(*module);
    std::printf("\n---- boot result ----\n");

    const char* status = "?";
    switch (r.status) {
    case BootResult::Status::CleanExit:
        status = "clean exit";
        break;
    case BootResult::Status::SyscallStop:
        status = "exit() syscall";
        break;
    case BootResult::Status::MissingHle:
        status = "missing HLE";
        break;
    case BootResult::Status::UnsupportedInstruction:
        status = "unsupported instruction";
        break;
    case BootResult::Status::Error:
        status = "error";
        break;
    }
    std::printf("status : %s\n", status);
    std::printf("detail : %s\n", r.message.c_str());
    if (r.status == BootResult::Status::MissingHle) {
        std::printf("symbol : %s\n", r.symbol.c_str());
    }
    std::printf("rip    : %#llx\n", (unsigned long long)r.rip);
    if (r.status == BootResult::Status::SyscallStop ||
        r.status == BootResult::Status::CleanExit) {
        std::printf("exit   : %llu\n", (unsigned long long)r.exit_code);
    }
    return r.status == BootResult::Status::CleanExit ||
                   r.status == BootResult::Status::SyscallStop
               ? 0
               : 3;
}
