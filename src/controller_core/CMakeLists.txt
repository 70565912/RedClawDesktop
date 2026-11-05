# Portable Controller session core (Windows, Linux, Android NDK). Qt-free and
# capture/encode-free: it only composes the existing net/service/security/
# protocol contracts that the Windows Controller runtime already uses.
add_library(redclaw_controller_core STATIC
  src/controller_session.cpp
)

target_include_directories(redclaw_controller_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(redclaw_controller_core
  PUBLIC
    redclaw_net
  PRIVATE
    redclaw_protocol
    redclaw_security
    redclaw_service
)

target_compile_features(redclaw_controller_core PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_controller_core)
