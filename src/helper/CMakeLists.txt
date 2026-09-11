add_library(redclaw_helper STATIC
  src/helper_bootstrap.cpp
  src/helper_capabilities.cpp
  src/runtime_profile.cpp
)

target_include_directories(redclaw_helper
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(redclaw_helper
  PUBLIC
    redclaw_service
  PRIVATE
    Wtsapi32
)

target_compile_features(redclaw_helper PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_helper)

# Helper executable
add_executable(redclaw_user_session_helper
  src/helper_main.cpp
)

target_link_libraries(redclaw_user_session_helper
  PRIVATE
    redclaw_helper
)

redclaw_apply_warnings(redclaw_user_session_helper)

# Set subsystem to CONSOLE for debugging, can be changed to WINDOWS later
set_target_properties(redclaw_user_session_helper PROPERTIES
  WIN32_EXECUTABLE FALSE
)
