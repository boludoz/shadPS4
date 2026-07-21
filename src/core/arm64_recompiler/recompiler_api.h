// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Plain C connectors for embedding the recompiler into a frontend (Android
// APK via JNI, test harnesses, external tools). Everything here is a stable
// facade over AotCompiler/JitEngine so the frontend never needs LLVM or
// C++ types across the boundary.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ShadRecompilerEngine ShadRecompilerEngine;

/// Opaque guest thread state; layout matches Core::Recompiler::GuestContext.
typedef struct ShadGuestContext ShadGuestContext;

/// Progress callback for AOT compilation: done/total discovered blocks.
/// Return 0 to cancel.
typedef int (*ShadAotProgressFn)(size_t done, size_t total, void* user);

/// Fallback handler for unsupported instructions (see JitEngine).
typedef int (*ShadUnsupportedFn)(ShadGuestContext* ctx, void* user);

ShadRecompilerEngine* shad_recompiler_create(void);
void shad_recompiler_destroy(ShadRecompilerEngine* engine);

/// Last error message of the engine ("" if none). Owned by the engine.
const char* shad_recompiler_error(ShadRecompilerEngine* engine);

/// AOT-compiles guest code in [base, base+size) reachable from entry_points,
/// writing "<name>.o"/"<name>.map" into cache_dir. triple may be NULL for the
/// default Android arm64 target or "native" for the host.
/// Guest memory must be mapped at its runtime addresses when this is called.
int shad_recompiler_aot_compile(ShadRecompilerEngine* engine, uint64_t base, uint64_t size,
                                const uint64_t* entry_points, size_t num_entries,
                                const char* cache_dir, const char* name, const char* triple,
                                ShadAotProgressFn progress, void* user);

/// Links an AOT cache produced by shad_recompiler_aot_compile.
int shad_recompiler_load_aot(ShadRecompilerEngine* engine, const char* object_file,
                             const char* map_file);

/// Registers a native (HLE) function at a guest address. fn receives the six
/// SysV integer argument registers and its return value goes to guest RAX.
void shad_recompiler_register_hle(ShadRecompilerEngine* engine, uint64_t guest_va,
                                  uint64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t,
                                                 uint64_t, uint64_t));

void shad_recompiler_set_unsupported_handler(ShadRecompilerEngine* engine, ShadUnsupportedFn fn,
                                             void* user);

/// Creates a guest thread context. entry: initial RIP; stack_size bytes of
/// host memory are allocated for the guest stack and the exit sentinel is
/// pushed as the initial return address.
ShadGuestContext* shad_guest_context_create(uint64_t entry, size_t stack_size);
void shad_guest_context_destroy(ShadGuestContext* ctx);

/// Access to the guest integer registers (index 0..15 = rax..r15, Zydis
/// order) and RIP for argument setup / result readout.
void shad_guest_context_set_gpr(ShadGuestContext* ctx, unsigned index, uint64_t value);
uint64_t shad_guest_context_get_gpr(ShadGuestContext* ctx, unsigned index);
uint64_t shad_guest_context_get_rip(ShadGuestContext* ctx);

/// Runs the context until it returns to the exit sentinel. Returns the
/// ExitReason as an integer (0 = clean exit).
uint64_t shad_recompiler_run(ShadRecompilerEngine* engine, ShadGuestContext* ctx);

/// Drops translations overlapping [va, va+size). Hook this to memory writes
/// over code pages: it is the self-modifying-code / in-game-JIT connector.
void shad_recompiler_invalidate(ShadRecompilerEngine* engine, uint64_t va, uint64_t size);

#ifdef __cplusplus
} // extern "C"
#endif
