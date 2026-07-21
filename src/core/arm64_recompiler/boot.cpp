// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Boot glue: lays out the PS4 process entry stack, installs the kernel syscall
// handler (FreeBSD-flavored, as the PS4 kernel is a FreeBSD derivative) and an
// unresolved-import reporter, and runs the guest through the JIT dispatcher.

#include <cstdio>
#include <cstring>

#include "core/arm64_recompiler/boot.h"
#include "core/arm64_recompiler/jit_engine.h"

namespace Core::Recompiler {

namespace {

// PS4/FreeBSD syscall numbers actually reached during early boot. This is a
// deliberately small set: enough to get through crt0/libc init and see a game
// print and allocate before it needs a real HLE library.
constexpr u64 kSys_exit = 1;
constexpr u64 kSys_read = 3;
constexpr u64 kSys_write = 4;
constexpr u64 kSys_munmap = 73;
constexpr u64 kSys_mmap = 477;      // FreeBSD/PS4 mmap
constexpr u64 kSys_exit_group = 431; // sceKernelExit path on some SDKs

/// Per-process boot state threaded through the C-style handlers.
struct BootState {
    Booter* booter = nullptr;
    JitEngine* jit = nullptr;
    const GuestLoader* loader = nullptr;
    std::function<void(int, const char*, u64)>* write_sink = nullptr;

    bool exited = false;
    u64 exit_code = 0;
    u64 blocks = 0;

    // Set when the guest jumped into an unresolved-import trampoline.
    bool missing_hle = false;
    std::string missing_symbol;
    u64 fault_rip = 0;

    // Bump allocator backing kSys_mmap(MAP_ANON) so early allocations succeed.
    u64 heap_cursor = 0x8'0000'0000ull;
};

BootState g_state;

// FreeBSD returns errors via CF set + errno in rax; success clears CF. Our
// translated code materializes CF from the syscall's RAX high bit convention
// is not used here — we simply place the return value in RAX and clear CF.
void SyscallReturn(GuestContext& ctx, u64 value, bool error = false) {
    ctx.gpr[kRax] = value;
    ctx.cf = error ? 1 : 0;
}

bool SyscallHandlerImpl(GuestContext& ctx, void* user) {
    auto* st = static_cast<BootState*>(user);
    const u64 num = ctx.gpr[kRax];
    // FreeBSD syscall arg registers: rdi, rsi, rdx, r10, r8, r9.
    const u64 a0 = ctx.gpr[kRdi];
    const u64 a1 = ctx.gpr[kRsi];
    const u64 a2 = ctx.gpr[kRdx];

    switch (num) {
    case kSys_write: {
        const int fd = static_cast<int>(a0);
        const char* data = reinterpret_cast<const char*>(a1);
        const u64 len = a2;
        if (st->write_sink && *st->write_sink) {
            (*st->write_sink)(fd, data, len);
        } else {
            std::fwrite(data, 1, len, fd == 2 ? stderr : stdout);
        }
        SyscallReturn(ctx, len);
        return true;
    }
    case kSys_read:
        SyscallReturn(ctx, 0); // EOF
        return true;
    case kSys_mmap: {
        // Anonymous bump allocation; ignores fd-backed mappings for now.
        const u64 len = (a1 + 0xFFF) & ~u64{0xFFF};
        const u64 addr = st->heap_cursor;
        st->heap_cursor += len;
        SyscallReturn(ctx, addr);
        return true;
    }
    case kSys_munmap:
        SyscallReturn(ctx, 0);
        return true;
    case kSys_exit:
    case kSys_exit_group:
        st->exited = true;
        st->exit_code = a0;
        // Redirect to the exit sentinel so the dispatcher unwinds cleanly.
        ctx.rip = kExitRip;
        return true;
    default:
        // Unknown syscall: report ENOSYS but keep going so we learn the next
        // thing the game needs rather than stopping at the first.
        std::fprintf(stderr, "[boot] unhandled syscall %llu at rip=%#llx\n",
                     static_cast<unsigned long long>(num),
                     static_cast<unsigned long long>(ctx.rip));
        SyscallReturn(ctx, static_cast<u64>(-78) /* ENOSYS */, true);
        return true;
    }
}

bool UnsupportedHandlerImpl(GuestContext& ctx, void* user) {
    auto* st = static_cast<BootState*>(user);
    // Did we land inside an unresolved-import trampoline? That means the game
    // called an HLE function we don't implement yet.
    if (const std::string* name = st->loader->LookupTrampoline(ctx.rip)) {
        st->missing_hle = true;
        st->missing_symbol = *name;
        st->fault_rip = ctx.rip;
        return false; // stop; the caller reports which symbol was missing
    }
    // Genuinely unsupported instruction (no interpreter attached yet).
    st->fault_rip = ctx.rip;
    return false;
}

} // namespace

Booter::Booter(JitEngine& jit_, const GuestLoader& loader_) : jit{jit_}, loader{loader_} {}

BootResult Booter::Boot(const GuestLoader::LoadedModule& module, u64 stack_size) {
    BootResult result{};

    // ---- Guest main-thread stack ----------------------------------------
    auto* stack = static_cast<u8*>(std::malloc(stack_size));
    if (!stack) {
        result.status = BootResult::Status::Error;
        result.message = "failed to allocate guest stack";
        return result;
    }

    GuestContext ctx{};
    ctx.host_stack_base = stack;
    ctx.host_stack_size = stack_size;

    // PS4 entry ABI (see Core::RunMainEntry): the kernel hands control to the
    // module entry with a 16-byte-aligned stack holding EntryParams, and
    // rdi = &EntryParams, rsi = exit function. crt0 reads argc/argv there.
    struct EntryParams {
        int argc;
        u32 pad;
        const char* argv[4];
        u64 entry_addr;
    };
    u64 rsp = reinterpret_cast<u64>(stack + stack_size);
    rsp &= ~u64{0xF};

    // Reserve and fill EntryParams on the guest stack.
    rsp -= sizeof(EntryParams);
    rsp &= ~u64{0xF};
    auto* params = reinterpret_cast<EntryParams*>(rsp);
    std::memset(params, 0, sizeof(*params));
    params->argc = 1;
    params->argv[0] = "eboot.bin";
    params->entry_addr = module.entry;

    // Push the exit sentinel as the return address so a top-level `ret`
    // unwinds out of Run().
    rsp -= 8;
    std::memcpy(reinterpret_cast<void*>(rsp), &kExitRip, sizeof(kExitRip));

    ctx.gpr[kRsp] = rsp;
    ctx.gpr[kRdi] = reinterpret_cast<u64>(params);
    ctx.gpr[kRsi] = kExitRip; // exit function -> sentinel
    ctx.rip = module.entry;

    // ---- Install handlers ------------------------------------------------
    g_state = BootState{};
    g_state.booter = this;
    g_state.jit = &jit;
    g_state.loader = &loader;
    g_state.write_sink = &write_sink;

    jit.SetSyscallHandler(&SyscallHandlerImpl, &g_state);
    jit.SetUnsupportedHandler(&UnsupportedHandlerImpl, &g_state);
    jit.SetMemoryReader(IdentityMemoryReader());

    // ---- Run -------------------------------------------------------------
    const ExitReason reason = jit.Run(ctx);

    result.rip = g_state.fault_rip ? g_state.fault_rip : ctx.rip;
    result.blocks_executed = g_state.blocks;

    if (g_state.missing_hle) {
        result.status = BootResult::Status::MissingHle;
        result.symbol = g_state.missing_symbol;
        result.message = "guest called unimplemented HLE function: " + g_state.missing_symbol;
    } else if (g_state.exited) {
        result.status = BootResult::Status::SyscallStop;
        result.exit_code = g_state.exit_code;
        result.message = "guest requested exit";
    } else if (reason == ExitReason::None) {
        result.status = BootResult::Status::CleanExit;
        result.exit_code = ctx.gpr[kRax];
        result.message = "guest returned from entry";
    } else if (reason == ExitReason::UnsupportedInstruction) {
        result.status = BootResult::Status::UnsupportedInstruction;
        result.message = "hit an instruction outside the supported subset";
    } else {
        result.status = BootResult::Status::Error;
        result.message = "emulation stopped";
    }

    std::free(stack);
    return result;
}

} // namespace Core::Recompiler
