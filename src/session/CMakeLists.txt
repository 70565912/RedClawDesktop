add_library(redclaw_session STATIC
  src/session_module.cpp
)

target_link_libraries(redclaw_session
  PUBLIC
    redclaw_input
    redclaw_protocol
    redclaw_security
    redclaw_service
)

target_include_directories(redclaw_session
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(redclaw_session PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_session)
