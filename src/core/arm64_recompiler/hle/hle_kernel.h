// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::Recompiler::Hle {

class HleRegistry;

/// Registers the minimal libkernel HLE subset with the registry.
void RegisterKernel(HleRegistry& registry);

} // namespace Core::Recompiler::Hle
