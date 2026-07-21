// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/arm64_recompiler/hle/hle_audio.h"
#include "core/arm64_recompiler/hle/hle_kernel.h"
#include "core/arm64_recompiler/hle/hle_pad.h"
#include "core/arm64_recompiler/hle/hle_registry.h"

namespace Core::Recompiler::Hle {

HleRegistry& HleRegistry::Instance() {
    static HleRegistry registry;
    return registry;
}

void HleRegistry::Register(const std::string& nid, HleFn fn) {
    table[nid] = fn;
}

HleFn HleRegistry::Find(const std::string& nid) const {
    const auto it = table.find(nid);
    return it == table.end() ? nullptr : it->second;
}

void HleRegistry::RegisterBuiltins() {
    if (builtins_registered) {
        return;
    }
    RegisterKernel(*this);
    RegisterAudioOut(*this);
    RegisterPad(*this);
    builtins_registered = true;
}

} // namespace Core::Recompiler::Hle
