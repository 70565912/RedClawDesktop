cmake_minimum_required(VERSION 3.20)
project(RedClawLatencyMinimal LANGUAGES CXX)
include(CTest)
find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(redclaw_repository "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
option(REDCLAW_LATENCY_CODEC_CHECK "Build the real software codec roundtrip" ON)
function(redclaw_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# Production transport logic only; no ICE sockets, GUI or application process.
set(net_sources video_frame_transport sent_video_frame_metadata
  media_transport_feedback media_congestion_controller media_pacing_budget
  media_pacer_wait desktop_media_send_pacer media_frame_trace receiver_stream_policy)
foreach(source IN LISTS net_sources)
  list(APPEND net_files "${redclaw_repository}/src/net/src/${source}.cpp")
endforeach()
add_library(redclaw_latency_transport STATIC ${net_files}
  "${redclaw_repository}/src/diag/src/host_frame_trace_file.cpp")
target_include_directories(redclaw_latency_transport PUBLIC
  "${redclaw_repository}/src/net/include" "${redclaw_repository}/src/diag/include")
target_link_libraries(redclaw_latency_transport PUBLIC Threads::Threads)
redclaw_apply_warnings(redclaw_latency_transport)

function(latency_test target source filter)
  add_executable(${target} "${redclaw_repository}/tests/${source}")
  target_link_libraries(${target} PRIVATE ${ARGN} GTest::gtest_main Threads::Threads)
  redclaw_apply_warnings(${target})
  add_test(NAME ${target} COMMAND ${target} "--gtest_filter=${filter}"
    "--gtest_output=xml:${CMAKE_CURRENT_BINARY_DIR}/${target}.xml")
  set_tests_properties(${target} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endfunction()
latency_test(redclaw_t30_expiry_tests diag/host_frame_trace_tests.cpp
  "HostFrameTrace.AdmissionProvenanceSeparatesAckExpiryAndMissingRefresh:HostFrameTrace.ExpiryRefreshAdvancesAcrossMultipleRetainedPackets:HostFrameTrace.RefreshCannotOverwriteConcurrentBudgetOrReset"
  redclaw_latency_transport)
latency_test(redclaw_t31_timing_tests net/sender_timing_provenance_tests.cpp "*"
  redclaw_latency_transport)

if(REDCLAW_LATENCY_CODEC_CHECK)
  list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
  if(NOT WIN32)
    set(Opus_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake")
  endif()
  find_package(FFMPEG REQUIRED)
  find_package(Protobuf REQUIRED)
  if(NOT TARGET protobuf::protoc)
    message(FATAL_ERROR "The codec check requires a usable Protobuf compiler (protoc).")
  endif()
  foreach(module protocol capture render)
    add_subdirectory("${redclaw_repository}/src/${module}" "modules/${module}")
  endforeach()
  latency_test(redclaw_t29_codec_integration_tests integration/desktop_budget_minimal_integration_tests.cpp
    "FfmpegRoundtripIntegration.DesktopBudgetSurvivesLowCadenceResizeAndReconnect"
    redclaw_capture redclaw_render)
endif()
