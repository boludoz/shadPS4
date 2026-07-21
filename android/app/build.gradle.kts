// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

plugins {
    id("com.android.application") version "8.5.0"
    id("org.jetbrains.kotlin.android") version "2.0.0"
}

android {
    namespace = "net.shadps4.android"
    compileSdk = 35
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "net.shadps4.android"
        minSdk = 31 // Android 12+: 16KB-page ready, Vulkan 1.1 guaranteed
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0-arm64-preview"

        ndk {
            // x86-64 games can only be recompiled to arm64.
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    // Path to a prebuilt LLVM for Android arm64, see
                    // documents/building-android.md:
                    "-DLLVM_DIR=${System.getenv("SHADPS4_LLVM_ANDROID") ?: ""}/lib/cmake/llvm",
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
}
