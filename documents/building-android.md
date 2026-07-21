<!--
SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Building for Android

The Android app lives in `android/` and builds `libshadps4-android.so`
(arm64-v8a) — the JNI connectors plus the x86 → LLVM IR → ARM64 recompiler
described in [arm64-android-port.md](arm64-android-port.md).

## Requirements

- JDK 17+
- Android SDK with **NDK r26 or newer** and CMake 3.22+ (install both from
  Android Studio's SDK Manager, or `sdkmanager "ndk;27.0.12077973"
  "cmake;3.22.1"`)
- Nothing else for the default (stub) build — the Gradle wrapper downloads
  Gradle, and Zydis builds from `externals/zydis` (run
  `git submodule update --init --recursive externals/zydis` first).

## Quick build

```sh
cd android
# point Gradle at your SDK if ANDROID_HOME is not set:
echo "sdk.dir=$HOME/Android/Sdk" > local.properties
./gradlew assembleDebug
# -> app/build/outputs/apk/debug/app-debug.apk (contains libshadps4-android.so)
```

This default build is the **stub configuration**: the `.so` compiles and
links without LLVM, all JNI/lifecycle/AOT-cache plumbing works, but x86 code
can only execute through an attached interpreter connector
(`JitEngine::SetUnsupportedHandler`). It exists so the app scaffolding always
builds; games need the full configuration below.

## Full recompiler build (AOT + JIT)

The real pipeline needs LLVM static libraries compiled *for Android arm64*.
Build them once with the helper script (takes a while, ~1-2 h on a desktop):

```sh
ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/27.0.12077973 \
    ./scripts/build_llvm_android.sh
```

The output lands in `externals/llvm-android/install`, which the Gradle/CMake
build detects automatically on the next `./gradlew assembleDebug` (the
configure log prints `full recompiler` instead of the stub warning). A
prebuilt installed elsewhere can be pointed at with the
`SHADPS4_LLVM_ANDROID` environment variable or
`-DSHADPS4_LLVM_DIR=<prefix>`.

`LLVM_VERSION=llvmorg-19.1.7` (or newer) can be exported before running the
script to build a newer LLVM; the recompiler is tested against LLVM 18.

## What runs today

The `.so` contains the translation engine (validated by
`tests/arm64_recompiler_test.cpp` on desktop: real x86-64 code translated and
executed, AArch64 objects emitted and reloaded) and the Surface/lifecycle/
input connectors for the APK. The emulator core (ELF/SELF loader, HLE
libraries, video_core) does not compile against the NDK yet — it attaches
through the weak hooks in `src/android/android_host.cpp`
(`Android::Hooks::LoadGame/StartEmulator/StopEmulator`). Until that lands,
booting a commercial game on device is **not** expected to work; what you get
is the complete recompiler + frontend contract to build that port on.

## Troubleshooting

- *"no LLVM for Android found - building the STUB recompiler"*: expected
  unless you ran `scripts/build_llvm_android.sh`; see above.
- *`Zydis/Zydis.h` not found*: initialize the submodule:
  `git submodule update --init --recursive externals/zydis`.
- 16 KB page-size devices (Android 15+): already handled, the library links
  with `-Wl,-z,max-page-size=16384`.
