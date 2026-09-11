# Qt-free startup contract, shared by the executable and regression tests.
add_library(redclaw_runtime_options STATIC
  runtime_options.cpp
  runtime_option_values.cpp
  runtime_options_validation.cpp
  runtime_profile_adapter.cpp
  runtime_usage.cpp
)
target_include_directories(redclaw_runtime_options PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(redclaw_runtime_options PRIVATE redclaw_helper redclaw_service)
if(WIN32)
  target_link_libraries(redclaw_runtime_options PRIVATE ws2_32)
endif()
target_compile_features(redclaw_runtime_options PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_runtime_options)
