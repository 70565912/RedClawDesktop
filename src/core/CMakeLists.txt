add_library(redclaw_core STATIC
  src/core_module.cpp
)

target_include_directories(redclaw_core
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(redclaw_core PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_core)
