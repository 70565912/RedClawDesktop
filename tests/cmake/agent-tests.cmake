# Registered in the parent tests directory; relative sources stay anchored there.
add_executable(redclaw_agent_peer_session_tests agent/agent_peer_session_tests.cpp)
target_link_libraries(redclaw_agent_peer_session_tests PRIVATE redclaw_agent GTest::gtest_main)
redclaw_apply_warnings(redclaw_agent_peer_session_tests)
add_test(NAME redclaw_agent_peer_session_tests COMMAND redclaw_agent_peer_session_tests)

add_executable(redclaw_agent_isolation_tests agent/agent_isolation_tests.cpp)

target_link_libraries(redclaw_agent_isolation_tests PRIVATE redclaw_agent GTest::gtest_main)

redclaw_apply_warnings(redclaw_agent_isolation_tests)

add_test(NAME redclaw_agent_isolation_tests COMMAND redclaw_agent_isolation_tests)

add_executable(redclaw_agent_remote_agent_broker_tests
  agent/remote_agent_broker_tests.cpp
)

target_link_libraries(redclaw_agent_remote_agent_broker_tests PRIVATE
  GTest::gtest_main
  redclaw_agent
)

redclaw_apply_warnings(redclaw_agent_remote_agent_broker_tests)

add_test(
  NAME redclaw_agent_remote_agent_broker_tests
  COMMAND redclaw_agent_remote_agent_broker_tests
)

add_executable(redclaw_agent_coordination_tests
  agent/coordination_tests.cpp
)

target_link_libraries(redclaw_agent_coordination_tests PRIVATE
  GTest::gtest_main
  redclaw_agent
)

redclaw_apply_warnings(redclaw_agent_coordination_tests)

add_test(
  NAME redclaw_agent_coordination_tests
  COMMAND redclaw_agent_coordination_tests
)

add_executable(redclaw_agent_provider_tests
  agent/agent_provider_tests.cpp
)

target_link_libraries(redclaw_agent_provider_tests PRIVATE
  GTest::gtest_main
  redclaw_agent
)

redclaw_apply_warnings(redclaw_agent_provider_tests)

add_test(
  NAME redclaw_agent_provider_tests
  COMMAND redclaw_agent_provider_tests
)
