# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# NDK entry point: builds libshadps4-android.so with the JNI connectors and
# the ARM64 recompiler. LLVM for Android must be provided prebuilt (see
# documents/building-android.md); point LLVM_DIR at its cmake directory, e.g.
#   -DLLVM_DIR=/opt/llvm-android-arm64/lib/cmake/llvm

cmake_minimum_required(VERSION 3.22)
project(shadps4-android CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SHADPS4_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

find_package(LLVM REQUIRED CONFIG)
llvm_map_components_to_libnames(SHADPS4_LLVM_LIBS
    core orcjit executionengine passes support target mc
    aarch64codegen aarch64asmparser aarch64desc aarch64info)

# Zydis (x86 decoder), static.
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
add_subdirectory(${SHADPS4_ROOT}/externals/zydis zydis EXCLUDE_FROM_ALL)

add_library(shadps4-android SHARED
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/x86_decoder.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/ir_emitter.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/aot_compiler.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/jit_engine.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/recompiler_api.cpp
    ${SHADPS4_ROOT}/src/android/android_host.cpp
    ${SHADPS4_ROOT}/src/android/jni_bridge.cpp
)

target_include_directories(shadps4-android PRIVATE
    ${SHADPS4_ROOT}/src
    ${LLVM_INCLUDE_DIRS})
target_compile_definitions(shadps4-android PRIVATE ${LLVM_DEFINITIONS})

target_link_libraries(shadps4-android PRIVATE
    Zydis
    ${SHADPS4_LLVM_LIBS}
    android
    log)

# TODO(arm64-port): add the emulator core (linker, HLE libraries, video_core
# with the Vulkan swapchain bound to ANativeWindow) once those components
# compile against the NDK; they override the weak hooks in android_host.cpp.
