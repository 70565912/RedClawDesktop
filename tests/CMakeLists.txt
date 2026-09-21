find_package(GTest CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)

# CTest registers unattended cases only. Mixed GoogleTest executables exclude
# interactive/account/hardware-specific evaluations with explicit command filters.
# Developers may build/run those executables directly when they choose; they are
# not deferred release gates. See docs/testing/test-matrix.md.

# include() deliberately preserves DIRECTORY TESTS and target aggregation below.
# add_subdirectory() would silently omit child tests from redclaw_tests_unit/all.
include(cmake/service-tests.cmake)
include(cmake/net-tests.cmake)
include(cmake/agent-tests.cmake)
include(cmake/protocol-tests.cmake)
include(cmake/security-tests.cmake)
include(cmake/helper-tests.cmake)
include(cmake/capture-tests.cmake)
include(cmake/render-tests.cmake)
include(cmake/session-tests.cmake)
include(cmake/input-tests.cmake)
include(cmake/integration-tests.cmake)
include(cmake/diag-tests.cmake)
include(cmake/ui-tests.cmake)

add_executable(redclaw_runtime_options_tests helper/runtime_options_tests.cpp helper/local_control_output_tests.cpp helper/input_qa_receipts_tests.cpp helper/runtime_loop_wake_tests.cpp)
target_link_libraries(redclaw_runtime_options_tests PRIVATE redclaw_runtime_options redclaw_protocol redclaw_diag redclaw_input Boost::json GTest::gtest_main)
redclaw_apply_warnings(redclaw_runtime_options_tests)
add_test(NAME redclaw_runtime_options_tests COMMAND redclaw_runtime_options_tests)

add_executable(redclaw_terminal_session_integration_tests
  workspace/terminal_session_integration_tests.cpp workspace/terminal_host_integration_tests.cpp workspace/terminal_controller_tests.cpp)
target_link_libraries(redclaw_terminal_session_integration_tests PRIVATE redclaw_workspace GTest::gtest_main)
redclaw_apply_warnings(redclaw_terminal_session_integration_tests)
add_test(NAME redclaw_terminal_session_integration_tests COMMAND redclaw_terminal_session_integration_tests)
set_tests_properties(redclaw_terminal_session_integration_tests PROPERTIES TIMEOUT 90)

add_executable(redclaw_transfer_file_receiver_tests workspace/transfer_file_receiver_tests.cpp)
find_package(OpenSSL REQUIRED)
target_link_libraries(redclaw_transfer_file_receiver_tests PRIVATE redclaw_workspace OpenSSL::Crypto GTest::gtest_main)
redclaw_apply_warnings(redclaw_transfer_file_receiver_tests)
add_test(NAME redclaw_transfer_file_receiver_tests COMMAND redclaw_transfer_file_receiver_tests)

add_executable(redclaw_transfer_operation_gate_tests workspace/transfer_operation_gate_tests.cpp)
target_link_libraries(redclaw_transfer_operation_gate_tests PRIVATE redclaw_workspace GTest::gtest_main)
redclaw_apply_warnings(redclaw_transfer_operation_gate_tests)
add_test(NAME redclaw_transfer_operation_gate_tests COMMAND redclaw_transfer_operation_gate_tests)

add_executable(redclaw_clipboard_paste_guard_tests workspace/clipboard_paste_guard_tests.cpp)
target_link_libraries(redclaw_clipboard_paste_guard_tests PRIVATE redclaw_workspace GTest::gtest_main)
redclaw_apply_warnings(redclaw_clipboard_paste_guard_tests)
add_test(NAME redclaw_clipboard_paste_guard_tests COMMAND redclaw_clipboard_paste_guard_tests)

add_executable(redclaw_clipboard_payload_integration_tests workspace/clipboard_payload_integration_tests.cpp)
target_link_libraries(redclaw_clipboard_payload_integration_tests PRIVATE redclaw_workspace GTest::gtest_main)
redclaw_apply_warnings(redclaw_clipboard_payload_integration_tests)
add_test(NAME redclaw_clipboard_payload_integration_tests COMMAND redclaw_clipboard_payload_integration_tests
  --gtest_filter=-ClipboardNativeFocus.*:ClipboardPayloadFixture.*)
set_tests_properties(redclaw_clipboard_payload_integration_tests PROPERTIES TIMEOUT 60)

add_executable(redclaw_clipboard_copy_store_integration_tests workspace/clipboard_copy_store_tests.cpp)
target_link_libraries(redclaw_clipboard_copy_store_integration_tests PRIVATE redclaw_workspace GTest::gtest_main)
redclaw_apply_warnings(redclaw_clipboard_copy_store_integration_tests)
add_test(NAME redclaw_clipboard_copy_store_integration_tests COMMAND redclaw_clipboard_copy_store_integration_tests)

add_executable(redclaw_transfer_worker_integration_tests workspace/transfer_worker_tests.cpp)
target_link_libraries(redclaw_transfer_worker_integration_tests PRIVATE redclaw_workspace GTest::gtest_main)
redclaw_apply_warnings(redclaw_transfer_worker_integration_tests)
add_test(NAME redclaw_transfer_worker_integration_tests COMMAND redclaw_transfer_worker_integration_tests)

add_executable(redclaw_transfer_runtime_integration_tests workspace/transfer_runtime_bridge_tests.cpp)
target_link_libraries(redclaw_transfer_runtime_integration_tests PRIVATE redclaw_workspace GTest::gtest_main)
redclaw_apply_warnings(redclaw_transfer_runtime_integration_tests)
add_test(NAME redclaw_transfer_runtime_integration_tests COMMAND redclaw_transfer_runtime_integration_tests)

if(TARGET redclaw_ui_terminal)
  add_executable(redclaw_workspace_control_integration_tests ui/workspace_control_tests.cpp workspace/workspace_clipboard_control_tests.cpp)
  target_link_libraries(redclaw_workspace_control_integration_tests PRIVATE redclaw_ui_terminal redclaw_ui_connection_flow
    redclaw_workspace Qt6::Network GTest::gtest)
  redclaw_apply_warnings(redclaw_workspace_control_integration_tests)
  add_test(NAME redclaw_workspace_control_integration_tests COMMAND redclaw_workspace_control_integration_tests)
  set_tests_properties(redclaw_workspace_control_integration_tests PROPERTIES TIMEOUT 90)
  add_custom_command(TARGET redclaw_workspace_control_integration_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:redclaw_workspace_control_integration_tests>/maintenance"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/scripts/service/terminal-profile.ps1"
      "$<TARGET_FILE_DIR:redclaw_workspace_control_integration_tests>/maintenance/terminal-profile.ps1")
  if(TARGET Qt6::qmake)
    get_target_property(workspace_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(workspace_qt_bin "${workspace_qmake}" DIRECTORY)
    add_custom_command(TARGET redclaw_workspace_control_integration_tests POST_BUILD
      COMMAND "${workspace_qt_bin}/windeployqt.exe" --no-translations --no-compiler-runtime $<TARGET_FILE:redclaw_workspace_control_integration_tests>)
  endif()
endif()

get_property(redclaw_tests_registered_names DIRECTORY PROPERTY TESTS)

set(redclaw_tests_executable_targets "")
set(redclaw_tests_integration_targets "")
set(redclaw_tests_unit_targets "")

foreach(redclaw_target_name IN LISTS redclaw_tests_registered_names)
  if(NOT TARGET ${redclaw_target_name})
    continue()
  endif()

  list(APPEND redclaw_tests_executable_targets ${redclaw_target_name})

  if(redclaw_target_name MATCHES "_integration_|_e2e_")
    list(APPEND redclaw_tests_integration_targets ${redclaw_target_name})
  else()
    list(APPEND redclaw_tests_unit_targets ${redclaw_target_name})
  endif()
endforeach()

if(NOT REDCLAW_TESTS_IN_DEFAULT_BUILD)
  foreach(redclaw_target_name IN LISTS redclaw_tests_executable_targets)
    set_property(TARGET ${redclaw_target_name} PROPERTY EXCLUDE_FROM_ALL TRUE)
    set_property(TARGET ${redclaw_target_name} PROPERTY EXCLUDE_FROM_DEFAULT_BUILD TRUE)
  endforeach()
endif()

if(redclaw_tests_executable_targets)
  add_custom_target(redclaw_tests_all)
  add_dependencies(redclaw_tests_all ${redclaw_tests_executable_targets})

  add_custom_target(redclaw_tests_unit)
  if(redclaw_tests_unit_targets)
    add_dependencies(redclaw_tests_unit ${redclaw_tests_unit_targets})
  endif()

  add_custom_target(redclaw_tests_integration)
  if(redclaw_tests_integration_targets)
    add_dependencies(redclaw_tests_integration ${redclaw_tests_integration_targets})
  endif()
endif()
