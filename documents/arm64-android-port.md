<!--
SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# ARM64 / Android port: x86 → LLVM IR → ARM64 recompiler

This document describes the architecture introduced under
`src/core/arm64_recompiler/`, `src/android/` and `android/` to run PS4 (x86-64)
guest code on ARM64 devices, with Android as the first target.

## Why AOT + a fallback JIT

shadPS4 on x86-64 hosts runs guest code natively. On ARM64 that is impossible:
every guest instruction has to be translated. The design here splits the work
in two, following the reasoning that most of a game's CPU-side code is static
and can be translated **ahead of time**, while a small dynamic remainder cannot:

1. **AOT compiler (loading screen)** — `aot_compiler.{h,cpp}`
   walks every basic block reachable from the module's entry points, lowers
   x86-64 to LLVM IR, optimizes (O2), and emits a single **AArch64 object
   file** plus a `guest RIP → symbol` map. Both are cached on disk
   (`aot_cache/<module>_<n>.{o,map}`), so the cost is paid once per game.

2. **Fallback JIT** — `jit_engine.{h,cpp}`
   an LLVM **ORCv2** (`LLJIT`) engine that handles everything the AOT pass
   could not see:
   * code the game generates at runtime (its internal shader/script JITs —
     the PS4 compiles GNM shaders on device, that code does not exist in the
     ELF);
   * indirect branch targets that static discovery missed;
   * self-modifying code, via `InvalidateRange()` — each JIT block owns an
     ORCv2 `ResourceTracker`, so stale translations are removed and lazily
     retranslated.

   AOT objects are linked into the **same ORC session** (`addObjectFile`), so
   AOT and JIT blocks share one symbol namespace (`gb_<hex rip>`) and call
   into each other transparently through the dispatcher.

```
   game ELF ──► x86 decoder (Zydis) ──► LLVM IR ──► O2 ──► AArch64 .o ──┐
   (load screen, cached)                                                │
                                                                  ORCv2 session ──► dispatcher ──► GuestContext
   runtime code ─► x86 decoder ─► LLVM IR ─► O1 ─► in-memory code ──────┘   ▲
   (shader JITs, indirect jumps, SMC)                                       │
   HLE registry (native sceXxx implementations) ────────────────────────────┘
```

### Execution model

Translated blocks have the signature `u64 block(GuestContext*)`: they read and
write the guest register file (`guest_context.h`) and return the next guest
RIP. The dispatcher (`JitEngine::Run`) loops over that, diverting to:

* **HLE functions** registered at guest addresses (`RegisterHleFunction`):
  the six SysV integer argument registers are forwarded, the result goes to
  guest RAX and the emulated `RET` resumes guest code. This is the connector
  for shadPS4's existing HLE library surface.
* **The unsupported-instruction handler** (`SetUnsupportedHandler`): blocks
  containing instructions outside the supported subset exit cleanly at that
  point with `ExitReason::UnsupportedInstruction`. A fallback interpreter —
  or an engine like FEXCore — plugs in here; the default test handler shows
  the contract (interpret/skip, then return `true` to continue).

Flags are stored materialized (one byte per flag) and computed eagerly;
LLVM's optimizer deletes the dead computations. Guest memory accesses are
direct host loads/stores (flat address space, same trade-off the x86 build
makes).

### What is deliberately not finished

* The instruction subset covers the common integer/control-flow/scalar-SSE
  core (see `ir_emitter.cpp`); packed-vector, atomic/`LOCK` and string
  instructions currently exit to the fallback connector.
* `#DE`-style guest exceptions are not modeled.
* The emulator core itself (linker, HLE libraries, video_core) does not build
  against the NDK yet; the Android host exposes weak hooks
  (`Android::Hooks::*` in `src/android/android_host.cpp`) where it attaches.

## Trying the pipeline on a desktop

Requires LLVM dev libraries (>= 18) — e.g. `apt install llvm-18-dev libzstd-dev`.

```sh
cmake -B build -DENABLE_ARM64_RECOMPILER=ON -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build --target arm64_recompiler_test x86_to_arm64
./build/arm64_recompiler_test        # end-to-end: translates AND runs x86 code
```

The CLI tool shows every stage — paste x86 bytes, watch LLVM transform them:

```sh
$ ./build/x86_to_arm64 "48 89 f8 48 01 f0 c3"
; ---- x86-64 input ----
;   mov rax, rdi / add rax, rsi / ret
; ---- LLVM IR (unoptimized / -O2) ----
; ---- aarch64 assembly ----
;   ldp x11, x10, [x0, #48] ... adds x13, x11, x10 ...
```

Use `--triple native` to emit host assembly instead, `-O0..-O3` to compare
optimization levels.

## Android

`android/` contains a minimal Gradle project (arm64-v8a only) whose native
library is built from `android/app/src/main/cpp/CMakeLists.txt`. It needs a
prebuilt LLVM for Android arm64 (build once with the NDK toolchain file, or
use a distribution like the r28 prebuilts); point `SHADPS4_LLVM_ANDROID` at
its install prefix.

Connector layers:

* `src/android/jni_bridge.cpp` — JNI exports consumed by
  `NativeLibrary.kt`. Any frontend APK can bind to these.
* `src/android/android_host.{h,cpp}` — lifecycle owner. Notable contracts:
  * `setSurface(null)` **blocks** until the renderer releases the old
    `ANativeWindow` (Android destroys the Surface right after
    `surfaceDestroyed()` returns; presenting to it afterwards is a native
    crash). Surface death/rebirth is decoupled from emulation state, so
    minimize → restore does not tear the JIT down.
  * `surfaceChanged` only bumps a generation counter; the renderer recreates
    its swapchain with the new extent on the next frame.
  * `Pause()/Resume()` park guest threads **between** translated blocks
    (`CheckRunGate`), never mid-block.
  * The AOT pass (`precompileAot`) runs on a worker thread with a progress
    callback for the loading UI and honors cancellation.
* `src/core/arm64_recompiler/recompiler_api.h` — plain-C facade of the whole
  engine for any non-C++ embedder.
