cmake_minimum_required(VERSION 3.22)

# S0.1 Android Controller spike: builds the shared repo modules in
# REDCLAW_CONTROLLER_CORE_ONLY mode plus a JNI shim. Dependencies come from
# the vcpkg manifest next to this file (triplet arm64-android).
project(redclaw_android_controller_spike LANGUAGES C CXX)

set(REDCLAW_CONTROLLER_CORE_ONLY ON CACHE BOOL "" FORCE)
set(REDCLAW_ENABLE_DHT_BACKENDS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# Static libdatachannel (vcpkg *-android triplets) links usrsctp/plog/libSRTP,
# but the overlay port config only re-finds the shared-build dependencies.
# Resolve them here so the imported names are visible to every subdirectory.
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(plog CONFIG REQUIRED)
find_package(libSRTP CONFIG REQUIRED)
find_package(unofficial-usrsctp CONFIG REQUIRED)
if(NOT TARGET Usrsctp::Usrsctp)
  add_library(Usrsctp::Usrsctp INTERFACE IMPORTED)
  target_link_libraries(Usrsctp::Usrsctp INTERFACE unofficial::usrsctp::usrsctp)
endif()
find_package(LibJuice CONFIG REQUIRED)
find_package(LibDataChannel CONFIG REQUIRED)

get_filename_component(REDCLAW_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_subdirectory("${REDCLAW_REPO_ROOT}" redclaw)

if(ANDROID)
  add_library(redclaw_controller_jni SHARED jni/redclaw_controller_jni.cpp)
  target_link_libraries(redclaw_controller_jni PRIVATE redclaw_controller_core android log)
  target_compile_features(redclaw_controller_jni PRIVATE cxx_std_20)
  redclaw_apply_warnings(redclaw_controller_jni)
  # Keep the exported surface to JNI entry points only.
  target_link_options(redclaw_controller_jni PRIVATE "-Wl,--exclude-libs,ALL" "-Wl,--gc-sections")
endif()
