// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Boot glue: lays out the PS4 process entry stack, installs the kernel syscall
// handler (FreeBSD-flavored, as the PS4 kernel is a FreeBSD derivative) and an
// unresolved-import reporter, and runs the guest through the JIT dispatcher.
//
// The file syscalls serve the game's dump through a mount table (/app0 ->
// dump directory), the same view the desktop build's mount table provides.

#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "core/arm64_recompiler/boot.h"
#include "core/arm64_recompiler/hle/hle_registry.h"
#include "core/arm64_recompiler/jit_engine.h"

namespace Core::Recompiler {

namespace {

// PS4/FreeBSD syscall numbers actually reached during early boot. This is a
// deliberately small set: enough to get through crt0/libc init, open the
// game's own files from /app0, print and allocate before it needs a real HLE
// library.
constexpr u64 kSys_exit = 1;
constexpr u64 kSys_read = 3;
constexpr u64 kSys_write = 4;
constexpr u64 kSys_open = 5;
constexpr u64 kSys_close = 6;
constexpr u64 kSys_munmap = 73;
constexpr u64 kSys_lseek = 478;
constexpr u64 kSys_mmap = 477;       // FreeBSD/PS4 mmap
constexpr u64 kSys_fstat = 551;
constexpr u64 kSys_exit_group = 431; // sceKernelExit path on some SDKs

// FreeBSD errno values returned through the error path.
constexpr u64 kENOENT = 2;
constexpr u64 kEBADF = 9;
constexpr u64 kENOSYS = 78;

// Mirrors OrbisKernelStat (core/libraries/kernel/file_system.h): the FreeBSD
// 9 stat layout the PS4 kernel returns. Only the fields early loaders look at
// are filled (st_mode, st_size, st_blksize).
#pragma pack(push, 1)
struct GuestTimespec {
    s64 sec;
    s64 nsec;
};
struct GuestStat {
    u32 st_dev;
    u32 st_ino;
    u16 st_mode;
    u16 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u32 st_rdev;
    GuestTimespec st_atim;
    GuestTimespec st_mtim;
    GuestTimespec st_ctim;
    s64 st_size;
    s64 st_blocks;
    u32 st_blksize;
    u32 st_flags;
    u32 st_gen;
    s32 st_lspare;
    GuestTimespec st_birthtim;
};
#pragma pack(pop)

/// Per-process boot state threaded through the C-style handlers.
struct BootState {
    Booter* booter = nullptr;
    JitEngine* jit = nullptr;
    const GuestLoader* loader = nullptr;
    std::function<void(int, const char*, u64)>* write_sink = nullptr;
    const std::vector<std::pair<std::string, std::filesystem::path>>* mounts = nullptr;

    bool exited = false;
    u64 exit_code = 0;
    u64 blocks = 0;
    u64 bound_hle = 0;

    // Set when the guest jumped into an unresolved-import trampoline.
    bool missing_hle = false;
    std::string missing_symbol;
    u64 fault_rip = 0;

    // Bump allocator backing kSys_mmap(MAP_ANON) so early allocations succeed.
    u64 heap_cursor = 0x8'0000'0000ull;

    // Open-file table for the mount-backed file syscalls. Guest fds 0-2 are
    // the standard streams; files start at 3.
    std::unordered_map<int, std::FILE*> files;
    int next_fd = 3;

    ~BootState() {
        for (auto& [fd, f] : files) {
            std::fclose(f);
        }
    }
};

BootState g_state;

/// Guest path -> host path through the mount table (longest prefix wins),
/// e.g. "/app0/sound/bgm.at9" -> "<dump>/sound/bgm.at9".
bool TranslatePath(const BootState& st, const char* guest_path,
                   std::filesystem::path& out) {
    if (!st.mounts || !guest_path) {
        return false;
    }
    std::string path{guest_path};
    const std::pair<std::string, std::filesystem::path>* best = nullptr;
    for (const auto& mount : *st.mounts) {
        const std::string& prefix = mount.first;
        if (path.compare(0, prefix.size(), prefix) == 0 &&
            (path.size() == prefix.size() || path[prefix.size()] == '/')) {
            if (!best || prefix.size() > best->first.size()) {
                best = &mount;
            }
        }
    }
    if (!best) {
        return false;
    }
    std::string rest = path.substr(best->first.size());
    while (!rest.empty() && rest.front() == '/') {
        rest.erase(rest.begin());
    }
    out = rest.empty() ? best->second : best->second / rest;
    return true;
}

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
    case kSys_open: {
        std::filesystem::path host;
        if (!TranslatePath(*st, reinterpret_cast<const char*>(a0), host)) {
            SyscallReturn(ctx, kENOENT, true);
            return true;
        }
        // The dump is read-only; every open the loader path performs is a
        // read (O_RDONLY == 0 on FreeBSD).
        std::FILE* f = std::fopen(host.string().c_str(), "rb");
        if (!f) {
            SyscallReturn(ctx, kENOENT, true);
            return true;
        }
        const int fd = st->next_fd++;
        st->files.emplace(fd, f);
        SyscallReturn(ctx, static_cast<u64>(fd));
        return true;
    }
    case kSys_close: {
        const auto it = st->files.find(static_cast<int>(a0));
        if (it == st->files.end()) {
            SyscallReturn(ctx, kEBADF, true);
            return true;
        }
        std::fclose(it->second);
        st->files.erase(it);
        SyscallReturn(ctx, 0);
        return true;
    }
    case kSys_read: {
        const auto it = st->files.find(static_cast<int>(a0));
        if (it == st->files.end()) {
            SyscallReturn(ctx, 0); // stdin & unknown fds: EOF
            return true;
        }
        const size_t n = std::fread(reinterpret_cast<void*>(a1), 1, a2, it->second);
        SyscallReturn(ctx, n);
        return true;
    }
    case kSys_lseek: {
        const auto it = st->files.find(static_cast<int>(a0));
        if (it == st->files.end()) {
            SyscallReturn(ctx, kEBADF, true);
            return true;
        }
        // args: fd, offset (s64), whence — whence matches SEEK_SET/CUR/END.
        if (std::fseek(it->second, static_cast<long>(static_cast<s64>(a1)),
                       static_cast<int>(a2)) != 0) {
            SyscallReturn(ctx, kEBADF, true);
            return true;
        }
        SyscallReturn(ctx, static_cast<u64>(std::ftell(it->second)));
        return true;
    }
    case kSys_fstat: {
        const auto it = st->files.find(static_cast<int>(a0));
        auto* out = reinterpret_cast<GuestStat*>(a1);
        if (it == st->files.end() || !out) {
            SyscallReturn(ctx, kEBADF, true);
            return true;
        }
        const long pos = std::ftell(it->second);
        std::fseek(it->second, 0, SEEK_END);
        const long size = std::ftell(it->second);
        std::fseek(it->second, pos, SEEK_SET);
        std::memset(out, 0, sizeof(*out));
        out->st_mode = 0100000 | 0444; // S_IFREG, r--r--r--
        out->st_nlink = 1;
        out->st_size = size;
        out->st_blksize = 0x8000;
        out->st_blocks = (size + 511) / 512;
        SyscallReturn(ctx, 0);
        return true;
    }
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
        SyscallReturn(ctx, kENOSYS, true);
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
    return Boot(module, {}, stack_size);
}

BootResult Booter::Boot(const GuestLoader::LoadedModule& main_module,
                        const std::vector<const GuestLoader::LoadedModule*>& preload,
                        u64 stack_size) {
    BootResult result{};

    // ---- Guest main-thread stack ----------------------------------------
    auto* stack = static_cast<u8*>(std::malloc(stack_size));
    if (!stack) {
        result.status = BootResult::Status::Error;
        result.message = "failed to allocate guest stack";
        return result;
    }

    // ---- Install handlers ------------------------------------------------
    g_state = BootState{};
    g_state.booter = this;
    g_state.jit = &jit;
    g_state.loader = &loader;
    g_state.write_sink = &write_sink;
    g_state.mounts = &mounts;

    jit.SetSyscallHandler(&SyscallHandlerImpl, &g_state);
    jit.SetUnsupportedHandler(&UnsupportedHandlerImpl, &g_state);
    jit.SetMemoryReader(IdentityMemoryReader());

    // ---- Bind HLE --------------------------------------------------------
    // For every import the registry knows — across the eboot AND its .prx
    // modules — register the native implementation at the trampoline the
    // loader pointed the import slot at. Imports another module exports were
    // already re-pointed by GuestLoader::ResolveImports; imports with no
    // implementation keep the fault trampoline so they are reported by name.
    auto& registry = Hle::HleRegistry::Instance();
    registry.RegisterBuiltins();
    u64 bound = 0;
    auto bind_module = [&](const GuestLoader::LoadedModule& mod) {
        for (const auto& sym : mod.imports) {
            if (sym.resolved) {
                continue;
            }
            if (Hle::HleFn fn = registry.Find(sym.nid)) {
                jit.RegisterHleFunction(sym.trampoline, reinterpret_cast<HleFn>(fn));
                ++bound;
            }
        }
    };
    for (const auto* mod : preload) {
        bind_module(*mod);
    }
    bind_module(main_module);
    g_state.bound_hle = bound;

    // Builds a fresh entry context on the shared stack. The PS4 entry ABI
    // (see Core::RunMainEntry): 16-byte-aligned stack holding EntryParams,
    // rdi = &EntryParams, rsi = exit function; crt0 reads argc/argv there.
    struct EntryParams {
        int argc;
        u32 pad;
        const char* argv[4];
        u64 entry_addr;
    };
    auto make_ctx = [&](u64 entry, int argc, const char* argv0) {
        GuestContext ctx{};
        ctx.host_stack_base = stack;
        ctx.host_stack_size = stack_size;
        u64 rsp = reinterpret_cast<u64>(stack + stack_size);
        rsp &= ~u64{0xF};
        rsp -= sizeof(EntryParams);
        rsp &= ~u64{0xF};
        auto* params = reinterpret_cast<EntryParams*>(rsp);
        std::memset(params, 0, sizeof(*params));
        params->argc = argc;
        params->argv[0] = argv0;
        params->entry_addr = entry;
        rsp -= 8;
        std::memcpy(reinterpret_cast<void*>(rsp), &kExitRip, sizeof(kExitRip));
        ctx.gpr[kRsp] = rsp;
        ctx.gpr[kRdi] = reinterpret_cast<u64>(params);
        ctx.gpr[kRsi] = kExitRip; // exit function -> sentinel
        ctx.rip = entry;
        return ctx;
    };

    auto finish = [&](GuestContext& ctx, ExitReason reason, const std::string& in_module) {
        result.rip = g_state.fault_rip ? g_state.fault_rip : ctx.rip;
        result.blocks_executed = g_state.blocks;
        result.hle_bound = g_state.bound_hle;
        const std::string where = in_module.empty() ? "" : " [module " + in_module + "]";
        if (g_state.missing_hle) {
            result.status = BootResult::Status::MissingHle;
            result.symbol = g_state.missing_symbol;
            result.message =
                "guest called unimplemented HLE function: " + g_state.missing_symbol + where;
        } else if (g_state.exited) {
            result.status = BootResult::Status::SyscallStop;
            result.exit_code = g_state.exit_code;
            result.message = "guest requested exit" + where;
        } else if (reason == ExitReason::None) {
            result.status = BootResult::Status::CleanExit;
            result.exit_code = ctx.gpr[kRax];
            result.message = "guest returned from entry" + where;
        } else if (reason == ExitReason::UnsupportedInstruction) {
            result.status = BootResult::Status::UnsupportedInstruction;
            result.message = "hit an instruction outside the supported subset" + where;
        } else {
            result.status = BootResult::Status::Error;
            result.message = "emulation stopped" + where;
        }
    };

    // ---- Run module_start of each .prx, then the eboot -------------------
    // On real hardware libkernel runs every loaded module's module_start
    // before handing control to the main module; mirror that order.
    for (const auto* mod : preload) {
        if (!mod->entry) {
            continue;
        }
        GuestContext ctx = make_ctx(mod->entry, 0, nullptr);
        const ExitReason reason = jit.Run(ctx);
        const bool ok = !g_state.missing_hle && !g_state.exited &&
                        (reason == ExitReason::None);
        if (!ok) {
            finish(ctx, reason, mod->name);
            std::free(stack);
            return result;
        }
    }

    GuestContext ctx = make_ctx(main_module.entry, 1,
                                mounts.empty() ? "eboot.bin" : "/app0/eboot.bin");
    const ExitReason reason = jit.Run(ctx);
    finish(ctx, reason, {});

    std::free(stack);
    return result;
}

} // namespace Core::Recompiler
