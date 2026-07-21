// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

namespace llvm {
class Module;
class TargetMachine;
} // namespace llvm

namespace Core::Recompiler {

/// Runs the standard per-module optimization pipeline (O0..O3).
void OptimizeModule(llvm::Module& module, unsigned opt_level);

/// Creates a TargetMachine for the given triple ("native" = host process
/// triple). Initializes all LLVM targets on first use. Returns nullptr and
/// fills `error` on failure.
std::unique_ptr<llvm::TargetMachine> CreateTargetMachine(const std::string& triple,
                                                         const std::string& cpu,
                                                         const std::string& features,
                                                         std::string& error);

} // namespace Core::Recompiler
