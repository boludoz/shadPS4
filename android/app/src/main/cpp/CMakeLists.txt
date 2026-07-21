# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# NDK entry point: builds libshadps4-android.so (JNI connectors + ARM64
# recompiler).
#
# Two configurations:
#  * Full: if an LLVM built for Android arm64 is found (see
#    scripts/build_llvm_android.sh, or pass -DSHADPS4_LLVM_DIR=<prefix>), the
#    real x86 -> LLVM IR -> ARM64 AOT + JIT pipeline is compiled in.
#  * Stub: without LLVM the library still builds and links; translation
#    requests route through the interpreter connector
#    (JitEngine::SetUnsupportedHandler). This keeps `gradlew assembleDebug`
#    working out of the box.

cmake_minimum_required(VERSION 3.22)
project(shadps4-android CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SHADPS4_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
cmake_path(NORMAL_PATH SHADPS4_ROOT)

# ---- Locate an optional LLVM for Android ------------------------------------
set(SHADPS4_LLVM_DIR "" CACHE PATH "Install prefix of an LLVM built for Android arm64")
set(_llvm_search ${SHADPS4_LLVM_DIR})
if (NOT _llvm_search AND DEFINED ENV{SHADPS4_LLVM_ANDROID})
    set(_llvm_search $ENV{SHADPS4_LLVM_ANDROID})
endif()
if (NOT _llvm_search)
    # Default output location of scripts/build_llvm_android.sh
    set(_llvm_search ${SHADPS4_ROOT}/externals/llvm-android/install)
endif()

set(SHADPS4_HAVE_LLVM OFF)
if (EXISTS ${_llvm_search}/lib/cmake/llvm/LLVMConfig.cmake)
    find_package(LLVM CONFIG PATHS ${_llvm_search}/lib/cmake/llvm NO_DEFAULT_PATH)
    if (LLVM_FOUND)
        set(SHADPS4_HAVE_LLVM ON)
        message(STATUS "shadps4-android: LLVM ${LLVM_PACKAGE_VERSION} found at ${_llvm_search} (full recompiler)")
    endif()
endif()
if (NOT SHADPS4_HAVE_LLVM)
    message(WARNING
        "shadps4-android: no LLVM for Android found - building the STUB recompiler. "
        "The .so builds and runs, but x86 code can only execute through an attached "
        "interpreter connector. Run scripts/build_llvm_android.sh (or set "
        "SHADPS4_LLVM_DIR / SHADPS4_LLVM_ANDROID) for the full AOT+JIT pipeline.")
endif()

# ---- Zydis (x86 decoder), static, pure C ------------------------------------
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
add_subdirectory(${SHADPS4_ROOT}/externals/zydis zydis EXCLUDE_FROM_ALL)

# ---- Native library ---------------------------------------------------------
set(SHADPS4_ANDROID_SOURCES
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/x86_decoder.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/recompiler_api.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/guest_loader.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/game_package.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/boot.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/hle/hle_registry.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/hle/hle_kernel.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/hle/hle_audio.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/hle/hle_audio_backend.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/hle/hle_pad.cpp
    ${SHADPS4_ROOT}/src/core/arm64_recompiler/hle/hle_pad_backend.cpp
    ${SHADPS4_ROOT}/src/core/aerolib/aerolib.cpp
    ${SHADPS4_ROOT}/src/android/android_host.cpp
    ${SHADPS4_ROOT}/src/android/jni_bridge.cpp
)
if (SHADPS4_HAVE_LLVM)
    list(APPEND SHADPS4_ANDROID_SOURCES
        ${SHADPS4_ROOT}/src/core/arm64_recompiler/ir_emitter.cpp
        ${SHADPS4_ROOT}/src/core/arm64_recompiler/aot_compiler.cpp
        ${SHADPS4_ROOT}/src/core/arm64_recompiler/jit_engine.cpp)
else()
    list(APPEND SHADPS4_ANDROID_SOURCES
        ${SHADPS4_ROOT}/src/core/arm64_recompiler/no_llvm_stubs.cpp)
endif()

add_library(shadps4-android SHARED ${SHADPS4_ANDROID_SOURCES})

target_include_directories(shadps4-android PRIVATE ${SHADPS4_ROOT}/src)
target_link_libraries(shadps4-android PRIVATE Zydis android log)

# Real audio/pad backends when their libraries are available. Both are already
# vendored in externals (openal-soft, sdl3); enable them for the APK to get
# sound and controller input. Left optional so the .so builds without them.
option(SHADPS4_ENABLE_OPENAL "Build the OpenAL audio backend" ON)
option(SHADPS4_ENABLE_SDL "Build the SDL gamepad backend" ON)
if (SHADPS4_ENABLE_OPENAL AND EXISTS ${SHADPS4_ROOT}/externals/openal-soft/CMakeLists.txt)
    set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
    set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${SHADPS4_ROOT}/externals/openal-soft openal EXCLUDE_FROM_ALL)
    target_compile_definitions(shadps4-android PRIVATE SHADPS4_HAVE_OPENAL)
    target_include_directories(shadps4-android PRIVATE ${SHADPS4_ROOT}/externals/openal-soft/include)
    target_link_libraries(shadps4-android PRIVATE OpenAL)
endif()
if (SHADPS4_ENABLE_SDL AND EXISTS ${SHADPS4_ROOT}/externals/sdl3/CMakeLists.txt)
    add_subdirectory(${SHADPS4_ROOT}/externals/sdl3 sdl3 EXCLUDE_FROM_ALL)
    target_compile_definitions(shadps4-android PRIVATE SHADPS4_HAVE_SDL)
    target_link_libraries(shadps4-android PRIVATE SDL3::SDL3)
endif()

if (SHADPS4_HAVE_LLVM)
    llvm_map_components_to_libnames(SHADPS4_LLVM_LIBS
        core orcjit executionengine passes support target mc
        aarch64codegen aarch64asmparser aarch64desc aarch64info)
    target_include_directories(shadps4-android PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(shadps4-android PRIVATE ${LLVM_DEFINITIONS})
    target_link_libraries(shadps4-android PRIVATE ${SHADPS4_LLVM_LIBS})
else()
    target_compile_definitions(shadps4-android PRIVATE SHADPS4_NO_LLVM)
endif()

# 16KB page alignment: required on Android 15+ devices.
target_link_options(shadps4-android PRIVATE "-Wl,-z,max-page-size=16384")

# TODO(arm64-port): add the emulator core (linker, HLE libraries, video_core
# with the Vulkan swapchain bound to ANativeWindow) once those components
# compile against the NDK; they override the weak hooks in android_host.cpp.
