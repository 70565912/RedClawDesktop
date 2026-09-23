# Registered in the parent tests directory; relative sources stay anchored there.
add_executable(redclaw_audio_stream_tests protocol/audio_stream_tests.cpp)
target_link_libraries(redclaw_audio_stream_tests PRIVATE redclaw_protocol GTest::gtest_main)
redclaw_apply_warnings(redclaw_audio_stream_tests)
add_test(NAME redclaw_audio_stream_tests COMMAND redclaw_audio_stream_tests)

add_executable(redclaw_transfer_protocol_tests protocol/transfer_protocol_tests.cpp)
target_link_libraries(redclaw_transfer_protocol_tests PRIVATE redclaw_protocol GTest::gtest_main)
redclaw_apply_warnings(redclaw_transfer_protocol_tests)
add_test(NAME redclaw_transfer_protocol_tests COMMAND redclaw_transfer_protocol_tests)
add_executable(redclaw_terminal_protocol_tests protocol/terminal_protocol_tests.cpp)
target_link_libraries(redclaw_terminal_protocol_tests PRIVATE redclaw_protocol GTest::gtest_main)
redclaw_apply_warnings(redclaw_terminal_protocol_tests)
add_test(NAME redclaw_terminal_protocol_tests COMMAND redclaw_terminal_protocol_tests)
add_executable(redclaw_protocol_schema_v1_tests
  protocol/schema_v1_tests.cpp
)

target_link_libraries(redclaw_protocol_schema_v1_tests PRIVATE
  redclaw_protocol
)

redclaw_apply_warnings(redclaw_protocol_schema_v1_tests)

add_test(
  NAME redclaw_protocol_schema_v1_tests
  COMMAND redclaw_protocol_schema_v1_tests
)

add_executable(redclaw_protocol_stream_control_protocol_tests
  protocol/stream_control_protocol_tests.cpp
)

target_link_libraries(redclaw_protocol_stream_control_protocol_tests PRIVATE
  GTest::gtest_main
  redclaw_protocol
)

redclaw_apply_warnings(redclaw_protocol_stream_control_protocol_tests)

add_test(
  NAME redclaw_protocol_stream_control_protocol_tests
  COMMAND redclaw_protocol_stream_control_protocol_tests
)

add_executable(redclaw_protocol_agent_protocol_tests
  protocol/agent_protocol_tests.cpp
)

target_link_libraries(redclaw_protocol_agent_protocol_tests PRIVATE
  GTest::gtest_main
  redclaw_protocol
)

redclaw_apply_warnings(redclaw_protocol_agent_protocol_tests)
add_dependencies(redclaw_protocol_agent_protocol_tests redclaw_protocol_codec)

if(WIN32)
  add_test(NAME redclaw_protocol_agent_wire_cli_tests
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
      ${PROJECT_SOURCE_DIR}/scripts/service/test-agent-wire-codec.ps1
      -CodecPath $<TARGET_FILE:redclaw_protocol_codec>)
endif()

add_test(
  NAME redclaw_protocol_agent_protocol_tests
  COMMAND redclaw_protocol_agent_protocol_tests
)

add_executable(redclaw_protocol_debug_bridge_protocol_tests
  protocol/debug_bridge_protocol_tests.cpp
)

target_link_libraries(redclaw_protocol_debug_bridge_protocol_tests PRIVATE
  GTest::gtest_main
  redclaw_protocol
)

redclaw_apply_warnings(redclaw_protocol_debug_bridge_protocol_tests)

add_test(
  NAME redclaw_protocol_debug_bridge_protocol_tests
  COMMAND redclaw_protocol_debug_bridge_protocol_tests
)

add_executable(redclaw_protocol_sdp_signaling_tests
  protocol/sdp_signaling_tests.cpp
)

target_link_libraries(redclaw_protocol_sdp_signaling_tests PRIVATE
  GTest::gtest_main
  redclaw_protocol
)

redclaw_apply_warnings(redclaw_protocol_sdp_signaling_tests)

add_test(
  NAME redclaw_protocol_sdp_signaling_tests
  COMMAND redclaw_protocol_sdp_signaling_tests
)
