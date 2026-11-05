plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// The native core resolves its C++ dependencies through vcpkg manifest mode
// (native/vcpkg.json, triplet arm64-android) chained to the NDK toolchain.
val vcpkgRoot: String = (findProperty("redclaw.vcpkgRoot") as String?)
    ?: System.getenv("VCPKG_ROOT")
    ?: throw GradleException("Set -Predclaw.vcpkgRoot=<vcpkg checkout> or VCPKG_ROOT")
val vcpkgInstalledDir: String? = (findProperty("redclaw.vcpkgInstalledDir") as String?)
    ?: System.getenv("VCPKG_INSTALLED_DIR")

android {
    namespace = "com.redclaw.controllerspike"
    compileSdk = 34
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.redclaw.controllerspike"
        // vcpkg's arm64-android triplet targets API 28.
        minSdk = 28
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.11-spike-s0.1"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                val ndkToolchain = "${android.ndkDirectory}/build/cmake/android.toolchain.cmake"
                arguments += listOf(
                    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgRoot/scripts/buildsystems/vcpkg.cmake",
                    "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=$ndkToolchain",
                    "-DVCPKG_TARGET_TRIPLET=arm64-android",
                    "-DANDROID_STL=c++_shared",
                )
                if (vcpkgInstalledDir != null) {
                    arguments += "-DVCPKG_INSTALLED_DIR=$vcpkgInstalledDir"
                }
                targets += "redclaw_controller_jni"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}
