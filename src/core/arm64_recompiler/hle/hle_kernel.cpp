// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A minimal libkernel HLE: just enough of the memory/thread/error surface for
// a module's crt0 and libc init to make progress and reach application code.
// This is intentionally small — the full libkernel is large and lives in
// core/libraries/kernel; this subset lets the boot chain get past early init
// and demonstrates real HLE calls flowing through the recompiler bridge.

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "common/types.h"
#include "core/arm64_recompiler/hle/hle_kernel.h"
#include "core/arm64_recompiler/hle/hle_registry.h"

namespace Core::Recompiler::Hle {

namespace {

// Per-thread errno storage the guest reads through __error()/sceKernelError.
thread_local int t_errno = 0;

int* PS4_SYSV_ABI __error() {
    return &t_errno;
}

// Direct/flexible memory: back it with host anonymous allocations. Good enough
// for a game to allocate its heaps and keep booting.
s32 PS4_SYSV_ABI sceKernelAllocateDirectMemory(s64 search_start, s64 search_end, u64 len,
                                               u64 alignment, int mem_type, s64* phys_out) {
    (void)search_start;
    (void)search_end;
    (void)mem_type;
    if (!phys_out) {
        return -1;
    }
    void* p = std::aligned_alloc(alignment ? alignment : 0x4000, (len + 0x3FFF) & ~u64{0x3FFF});
    if (!p) {
        return -1;
    }
    *phys_out = reinterpret_cast<s64>(p);
    return 0;
}

s32 PS4_SYSV_ABI sceKernelMapDirectMemory(void** addr_inout, u64 len, int prot, int flags,
                                          s64 phys_addr, u64 alignment) {
    (void)len;
    (void)prot;
    (void)flags;
    (void)alignment;
    if (!addr_inout) {
        return -1;
    }
    // Identity: the physical allocation is already a usable host pointer.
    *addr_inout = reinterpret_cast<void*>(phys_addr);
    return 0;
}

s32 PS4_SYSV_ABI sceKernelReleaseDirectMemory(s64 start, u64 len) {
    (void)len;
    std::free(reinterpret_cast<void*>(start));
    return 0;
}

u64 PS4_SYSV_ABI sceKernelGetDirectMemorySize() {
    return 5376_MB; // PS4 game-usable direct memory
}

s32 PS4_SYSV_ABI sceKernelGetModuleInfo(s32 handle, void* info) {
    (void)handle;
    (void)info;
    return 0;
}

// Thread-local storage bootstrap: return a small per-thread block.
void* PS4_SYSV_ABI sceKernelGetTcbBase() {
    static thread_local u8 tcb[1024];
    return tcb;
}

s32 PS4_SYSV_ABI sceKernelGetCurrentCpu() {
    return 0;
}

} // namespace

void RegisterKernel(HleRegistry& registry) {
    // NIDs from libkernel (see core/libraries/kernel).
    HLE_REGISTER(registry, "9BcDykPmo1I", __error);
    HLE_REGISTER(registry, "rTXw65xmLIA", sceKernelAllocateDirectMemory);
    HLE_REGISTER(registry, "NcaWUxfMNIQ", sceKernelMapDirectMemory);
    HLE_REGISTER(registry, "rf6L165iRhI", sceKernelReleaseDirectMemory);
    HLE_REGISTER(registry, "pO96TwzOm5E", sceKernelGetDirectMemorySize);
    HLE_REGISTER(registry, "IH9AGb0dImA", sceKernelGetModuleInfo);
    HLE_REGISTER(registry, "n88vx3C5nW8", sceKernelGetTcbBase);
}

} // namespace Core::Recompiler::Hle
