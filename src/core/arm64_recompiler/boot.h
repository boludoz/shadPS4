// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>

#include "core/arm64_recompiler/guest_context.h"
#include "core/arm64_recompiler/guest_loader.h"

namespace Core::Recompiler {

class JitEngine;

/// Result of trying to boot a guest module.
struct BootResult {
    enum class Status {
        CleanExit,            // guest returned to the exit sentinel
        MissingHle,           // called an import with no HLE implementation
        UnsupportedInstruction,
        SyscallStop,          // a syscall requested process exit
        Error,                // setup failure (see message)
    };
    Status status = Status::Error;
    std::string message;      // human-readable detail
    std::string symbol;       // for MissingHle: the function name hit
    u64 rip = 0;              // guest RIP where it stopped
    u64 exit_code = 0;        // for CleanExit / SyscallStop
    u64 blocks_executed = 0;  // rough progress metric
};

/// Owns the syscall/HLE glue for one guest process and runs it.
///
/// This is the piece that turns "a translator" into "boots a game": it lays
/// out the PS4 entry stack, installs a FreeBSD-flavored syscall handler (the
/// PS4 kernel ABI) and an unresolved-import reporter, and drives the JIT
/// dispatcher from the module entry point until the guest exits or hits
/// something not yet emulated.
class Booter {
public:
    Booter(JitEngine& jit, const GuestLoader& loader);

    /// Boots the module: sets up the guest thread and runs it.
    /// stack_size is the guest main-thread stack (default 8 MiB).
    BootResult Boot(const GuestLoader::LoadedModule& module, u64 stack_size = 8 * 1024 * 1024);

    /// Redirects guest stdout/stderr writes (SYS_write) somewhere; defaults to
    /// host stdout. Lets the frontend show a game's early logging.
    void SetWriteSink(std::function<void(int fd, const char* data, u64 len)> sink) {
        write_sink = std::move(sink);
    }

private:
    JitEngine& jit;
    const GuestLoader& loader;
    std::function<void(int fd, const char* data, u64 len)> write_sink;
};

} // namespace Core::Recompiler
