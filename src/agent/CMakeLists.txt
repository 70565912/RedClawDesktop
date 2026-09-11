add_library(redclaw_agent STATIC
  src/agent_peer_session.cpp
  src/debug_fixture_agent_provider.cpp
  src/agent_executor.cpp
  src/agent_coordinator.cpp
  src/local_agent_pipe.cpp
  src/agent_process.cpp
  src/codex_app_server_provider.cpp
  src/coordination.cpp
  src/cursor_agent_provider.cpp
  src/remote_agent_broker.cpp
)

find_package(Boost REQUIRED COMPONENTS json)
find_package(OpenSSL REQUIRED)

target_include_directories(redclaw_agent
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(redclaw_agent
  PUBLIC redclaw_protocol
  PRIVATE Boost::json OpenSSL::Crypto
)

if(WIN32)
  target_link_libraries(redclaw_agent PRIVATE advapi32)
endif()

target_compile_features(redclaw_agent PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_agent)

add_executable(redclaw_coordinationctl
  src/coordinationctl_main.cpp
)

target_link_libraries(redclaw_coordinationctl PRIVATE redclaw_agent)
target_compile_features(redclaw_coordinationctl PRIVATE cxx_std_20)
redclaw_apply_warnings(redclaw_coordinationctl)
