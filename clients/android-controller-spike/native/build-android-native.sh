#!/usr/bin/env bash
# Builds libredclaw_controller_jni.so for arm64-v8a without Gradle.
# Required: ANDROID_NDK_HOME (r26+), VCPKG_ROOT. Optional: VCPKG_INSTALLED_DIR,
# BUILD_DIR, CMAKE_BUILD_TYPE (default Release).
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to an Android NDK (r26 or newer)}"
: "${VCPKG_ROOT:?set VCPKG_ROOT to a vcpkg checkout}"
build_dir="${BUILD_DIR:-${script_dir}/build/arm64-v8a}"
installed_dir="${VCPKG_INSTALLED_DIR:-${script_dir}/vcpkg_installed}"
build_type="${CMAKE_BUILD_TYPE:-Release}"

cmake -S "${script_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${build_type}" \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-android \
  -DVCPKG_INSTALLED_DIR="${installed_dir}" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28 \
  -DANDROID_STL=c++_shared

cmake --build "${build_dir}" --target redclaw_controller_jni --parallel

so_path="${build_dir}/libredclaw_controller_jni.so"
echo "built ${so_path}"
"${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" -h "${so_path}" | grep -E "Class|Machine"
