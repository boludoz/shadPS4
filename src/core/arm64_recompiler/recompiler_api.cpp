// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <new>
#include <vector>

#include "core/arm64_recompiler/aot_compiler.h"
#include "core/arm64_recompiler/jit_engine.h"
#include "core/arm64_recompiler/recompiler_api.h"

using namespace Core::Recompiler;

struct ShadRecompilerEngine {
    JitEngine jit;
    std::string last_error;
};

extern "C" {

ShadRecompilerEngine* shad_recompiler_create(void) {
    return new (std::nothrow) ShadRecompilerEngine{};
}

void shad_recompiler_destroy(ShadRecompilerEngine* engine) {
    delete engine;
}

const char* shad_recompiler_error(ShadRecompilerEngine* engine) {
    if (!engine) {
        return "";
    }
    if (!engine->jit.GetError().empty()) {
        return engine->jit.GetError().c_str();
    }
    return engine->last_error.c_str();
}

int shad_recompiler_aot_compile(ShadRecompilerEngine* engine, uint64_t base, uint64_t size,
                                const uint64_t* entry_points, size_t num_entries,
                                const char* cache_dir, const char* name, const char* triple,
                                ShadAotProgressFn progress, void* user) {
    if (!engine || !cache_dir || !name) {
        return 0;
    }
    AotCompiler::Config config{};
    if (triple && *triple) {
        config.triple = triple;
    }
    AotCompiler compiler{config};
    std::vector<u64> entries(entry_points, entry_points + num_entries);
    AotCompiler::ProgressFn progress_fn;
    if (progress) {
        progress_fn = [progress, user](size_t done, size_t total) {
            return progress(done, total, user) != 0;
        };
    }
    const auto result = compiler.CompileRegion(IdentityMemoryReader(), base, size, entries,
                                               cache_dir, name, progress_fn);
    if (!result) {
        engine->last_error = compiler.GetError();
        return 0;
    }
    return 1;
}

int shad_recompiler_load_aot(ShadRecompilerEngine* engine, const char* object_file,
                             const char* map_file) {
    if (!engine || !object_file || !map_file) {
        return 0;
    }
    return engine->jit.LoadAotObject(object_file, map_file) ? 1 : 0;
}

void shad_recompiler_register_hle(ShadRecompilerEngine* engine, uint64_t guest_va,
                                  uint64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t,
                                                 uint64_t, uint64_t)) {
    if (engine && fn) {
        engine->jit.RegisterHleFunction(guest_va, fn);
    }
}

void shad_recompiler_set_unsupported_handler(ShadRecompilerEngine* engine, ShadUnsupportedFn fn,
                                             void* user) {
    if (!engine) {
        return;
    }
    // ShadGuestContext is layout-compatible with GuestContext by construction.
    engine->jit.SetUnsupportedHandler(reinterpret_cast<UnsupportedHandler>(fn), user);
}

ShadGuestContext* shad_guest_context_create(uint64_t entry, size_t stack_size) {
    auto* ctx = new (std::nothrow) GuestContext{};
    if (!ctx) {
        return nullptr;
    }
    if (stack_size < 4096) {
        stack_size = 4096;
    }
    auto* stack = new (std::nothrow) u8[stack_size];
    if (!stack) {
        delete ctx;
        return nullptr;
    }
    ctx->host_stack_base = stack;
    ctx->host_stack_size = stack_size;
    ctx->rip = entry;
    // 16-byte align and push the exit sentinel as the return address.
    u64 rsp = reinterpret_cast<u64>(stack + stack_size) & ~u64{15};
    rsp -= 8;
    std::memcpy(reinterpret_cast<void*>(rsp), &kExitRip, sizeof(kExitRip));
    ctx->gpr[kRsp] = rsp;
    return reinterpret_cast<ShadGuestContext*>(ctx);
}

void shad_guest_context_destroy(ShadGuestContext* opaque) {
    auto* ctx = reinterpret_cast<GuestContext*>(opaque);
    if (ctx) {
        delete[] static_cast<u8*>(ctx->host_stack_base);
        delete ctx;
    }
}

void shad_guest_context_set_gpr(ShadGuestContext* opaque, unsigned index, uint64_t value) {
    auto* ctx = reinterpret_cast<GuestContext*>(opaque);
    if (ctx && index < kNumGprs) {
        ctx->gpr[index] = value;
    }
}

uint64_t shad_guest_context_get_gpr(ShadGuestContext* opaque, unsigned index) {
    auto* ctx = reinterpret_cast<GuestContext*>(opaque);
    return (ctx && index < kNumGprs) ? ctx->gpr[index] : 0;
}

uint64_t shad_guest_context_get_rip(ShadGuestContext* opaque) {
    auto* ctx = reinterpret_cast<GuestContext*>(opaque);
    return ctx ? ctx->rip : 0;
}

uint64_t shad_recompiler_run(ShadRecompilerEngine* engine, ShadGuestContext* opaque) {
    auto* ctx = reinterpret_cast<GuestContext*>(opaque);
    if (!engine || !ctx) {
        return static_cast<uint64_t>(ExitReason::HostRequest);
    }
    return static_cast<uint64_t>(engine->jit.Run(*ctx));
}

void shad_recompiler_invalidate(ShadRecompilerEngine* engine, uint64_t va, uint64_t size) {
    if (engine) {
        engine->jit.InvalidateRange(va, size);
    }
}

} // extern "C"
