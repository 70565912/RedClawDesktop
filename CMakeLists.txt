cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
endif()

if(DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE STREQUAL "/scripts/buildsystems/vcpkg.cmake")
  unset(CMAKE_TOOLCHAIN_FILE)
  unset(CMAKE_TOOLCHAIN_FILE CACHE)
endif()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR CMAKE_TOOLCHAIN_FILE STREQUAL "")
  set(_redclaw_vcpkg_candidates)
  if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _redclaw_vcpkg_root)
    list(APPEND _redclaw_vcpkg_candidates "${_redclaw_vcpkg_root}")
  endif()
  list(APPEND _redclaw_vcpkg_candidates "${CMAKE_SOURCE_DIR}/vcpkg" "C:/vcpkg")

  foreach(_redclaw_vcpkg_root IN LISTS _redclaw_vcpkg_candidates)
    set(_redclaw_vcpkg_toolchain "${_redclaw_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
    if(EXISTS "${_redclaw_vcpkg_toolchain}")
      set(CMAKE_TOOLCHAIN_FILE "${_redclaw_vcpkg_toolchain}" CACHE FILEPATH "vcpkg toolchain file")
      break()
    endif()
  endforeach()
endif()

project(RedClawDesktop
  VERSION 0.1.1
  DESCRIPTION "Modular CMake skeleton for RedClawDesktop"
  LANGUAGES CXX
)

if(MSVC)
  # Avoid shared compile-time PDB writes in Ninja/MSVC builds; this prevents
  # mspdbsrv/PDB handle stalls from wedging focused target builds.
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif()

include(CTest)

option(
  REDCLAW_TESTS_IN_DEFAULT_BUILD
  "Include test executables in the default ALL build target"
  ON
)

option(
  REDCLAW_ENABLE_DHT_BACKENDS
  "Build libtorrent/miniupnpc backed DHT rendezvous support"
  OFF
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(redclaw_apply_warnings target_name)
  # Keep warning levels explicit for deterministic local and CI builds.
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

add_subdirectory(src)

if(WIN32)
  add_custom_target(redclaw_m03_trust_store_env_override_smoke
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
      -File "${CMAKE_SOURCE_DIR}/scripts/service/test-m03-trust-store-env-override.ps1"
      -HostServiceBinaryPath "$<TARGET_FILE:redclaw_host_service>"
      -OutputJsonPath "${CMAKE_BINARY_DIR}/reports/m03-trust-store-env-override-smoke.json"
    DEPENDS redclaw_host_service
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Run admin-gated M03 trust-store environment override smoke validation"
    VERBATIM
  )

  add_custom_target(redclaw_m07_t02_boot_validation
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
      -File "${CMAKE_SOURCE_DIR}/scripts/service/run-m07-t02-boot-validation.ps1"
      -HostServiceBinaryPath "$<TARGET_FILE:redclaw_host_service>"
      -OutputJsonPath "${CMAKE_BINARY_DIR}/reports/m07-t02-boot-validation.json"
    DEPENDS redclaw_host_service
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Run admin-gated M07-T02 boot startup validation"
    VERBATIM
  )
endif()

if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/installer/CMakeLists.txt")
  add_subdirectory(installer)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
