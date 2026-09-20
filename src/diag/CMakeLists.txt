add_library(redclaw_diag STATIC
  src/diag_module.cpp
  src/runtime_status_parser.cpp
  src/operation_timing.cpp
  src/host_frame_trace_file.cpp
)

target_link_libraries(redclaw_diag
  PUBLIC
    redclaw_net
    redclaw_protocol
)

target_include_directories(redclaw_diag
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(redclaw_diag PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_diag)
