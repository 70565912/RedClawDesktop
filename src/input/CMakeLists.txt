add_library(redclaw_input STATIC
  src/input_module.cpp
)

target_include_directories(redclaw_input
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(redclaw_input PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_input)
