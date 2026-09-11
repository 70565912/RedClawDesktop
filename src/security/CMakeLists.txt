add_library(redclaw_security STATIC
  src/security_module.cpp
)

target_include_directories(redclaw_security
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(redclaw_security PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_security)
