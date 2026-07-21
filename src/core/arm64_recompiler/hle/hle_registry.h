// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "common/types.h"
#include "core/arm64_recompiler/guest_context.h"

namespace Core::Recompiler::Hle {

/// Uniform HLE entry: receives the six guest SysV integer/pointer argument
/// registers (rdi, rsi, rdx, rcx, r8, r9) and returns the value for RAX. This
/// is the same shape JitEngine::RegisterHleFunction expects, so a registry
/// entry can be bound straight onto an import's trampoline address.
using HleFn = u64 (*)(u64, u64, u64, u64, u64, u64);

/// Registry of native HLE implementations keyed by PS4 NID.
///
/// This is the connector the recompiler was missing: the loader resolves an
/// imported NID here and, when present, binds the native function at the
/// import's call site instead of the fault trampoline. That is what lets a
/// translated game call sceAudioOutOutput / scePadReadState / sceKernel* and
/// reach real code.
class HleRegistry {
public:
    static HleRegistry& Instance();

    void Register(const std::string& nid, HleFn fn);
    HleFn Find(const std::string& nid) const;
    size_t Size() const {
        return table.size();
    }

    /// Registers all built-in modules (kernel, audio, pad, ...). Idempotent.
    void RegisterBuiltins();

private:
    std::unordered_map<std::string, HleFn> table;
    bool builtins_registered = false;
};

// ---- Typed native function -> HleFn adapter --------------------------------
//
// PS4 HLE functions have real C signatures (PS4_SYSV_ABI). Wrap<Func>() turns
// one into an HleFn: it reads the guest argument registers, casts each to the
// function's parameter type and calls it. Because the C++ compiler emits the
// native call, this marshals correctly across ABIs (guest SysV -> host AAPCS64
// on device) for integer and pointer arguments, which is what these APIs use.

namespace detail {

template <typename T>
T FromReg(u64 v) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(static_cast<uintptr_t>(v));
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<T>(v);
    } else {
        return static_cast<T>(v);
    }
}

template <typename R>
u64 ToReg(R value) {
    if constexpr (std::is_void_v<R>) {
        return 0;
    } else if constexpr (std::is_pointer_v<R>) {
        return static_cast<u64>(reinterpret_cast<uintptr_t>(value));
    } else {
        return static_cast<u64>(value);
    }
}

template <auto Func, typename Ret, typename... Args, size_t... I>
u64 CallImpl(u64 (&regs)[6], std::index_sequence<I...>) {
    if constexpr (std::is_void_v<Ret>) {
        Func(FromReg<Args>(regs[I])...);
        return 0;
    } else {
        return ToReg<Ret>(Func(FromReg<Args>(regs[I])...));
    }
}

template <auto Func, typename Ret, typename... Args>
u64 Dispatch(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    static_assert(sizeof...(Args) <= 6, "HLE bridge supports up to 6 register args");
    u64 regs[6] = {a0, a1, a2, a3, a4, a5};
    return CallImpl<Func, Ret, Args...>(regs, std::index_sequence_for<Args...>{});
}

template <auto Func, typename Ret, typename... Args>
HleFn MakeThunk(Ret (*)(Args...)) {
    return &Dispatch<Func, Ret, Args...>;
}

} // namespace detail

/// Produces an HleFn that calls the native function pointed to by the Func
/// template argument, marshaling guest argument registers to its parameters.
template <auto Func>
HleFn Wrap() {
    return detail::MakeThunk<Func>(Func);
}

/// Registers a native PS4_SYSV_ABI function under its NID.
#define HLE_REGISTER(reg, nid, func) (reg).Register((nid), ::Core::Recompiler::Hle::Wrap<func>())

} // namespace Core::Recompiler::Hle
