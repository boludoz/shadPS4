#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds a minimal static LLVM (AArch64 backend + ORC JIT only) for Android
# arm64 with the NDK, for the full x86 -> LLVM IR -> ARM64 recompiler in the
# Android app. Output goes to externals/llvm-android/install, which
# android/app/src/main/cpp/CMakeLists.txt picks up automatically.
#
# Usage:
#   ANDROID_NDK_HOME=/path/to/ndk ./scripts/build_llvm_android.sh
#
# Tunables (env vars):
#   LLVM_VERSION   git tag to build (default: llvmorg-18.1.8, the version the
#                  recompiler is CI-tested against; newer releases work too)
#   ANDROID_API    minSdk level (default: 31, must match app/build.gradle.kts)
#   JOBS           parallel build jobs (default: nproc)

set -euo pipefail

LLVM_VERSION="${LLVM_VERSION:-llvmorg-18.1.8}"
ANDROID_API="${ANDROID_API:-31}"
JOBS="${JOBS:-$(nproc)}"

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "${NDK}" || ! -d "${NDK}" ]]; then
    echo "error: set ANDROID_NDK_HOME to your NDK (r26+)" >&2
    exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${ROOT}/externals/llvm-android"
SRC="${WORK}/llvm-project"
BUILD="${WORK}/build"
INSTALL="${WORK}/install"

mkdir -p "${WORK}"
if [[ ! -d "${SRC}" ]]; then
    git clone --depth 1 --branch "${LLVM_VERSION}" \
        https://github.com/llvm/llvm-project.git "${SRC}"
fi

# Only what the recompiler links against: AArch64 codegen + ORC JIT + passes.
cmake -S "${SRC}/llvm" -B "${BUILD}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${NDK}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-${ANDROID_API}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL}" \
    -DLLVM_HOST_TRIPLE=aarch64-unknown-linux-android \
    -DLLVM_TARGETS_TO_BUILD=AArch64 \
    -DLLVM_BUILD_TOOLS=OFF \
    -DLLVM_BUILD_UTILS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF \
    -DLLVM_ENABLE_ASSERTIONS=OFF

ninja -C "${BUILD}" -j "${JOBS}" install

echo
echo "LLVM for Android installed to ${INSTALL}"
echo "The Gradle build will find it automatically; alternatively export"
echo "  SHADPS4_LLVM_ANDROID=${INSTALL}"
