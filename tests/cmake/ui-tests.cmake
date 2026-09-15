# Registered in the parent tests directory; relative sources stay anchored there.
if(TARGET redclaw_ui_terminal)
  add_executable(redclaw_terminal_view_integration_tests ui/terminal_view_integration_tests.cpp)
  target_link_libraries(redclaw_terminal_view_integration_tests PRIVATE redclaw_ui_terminal redclaw_workspace GTest::gtest)
  redclaw_apply_warnings(redclaw_terminal_view_integration_tests)
  add_test(NAME redclaw_terminal_view_integration_tests COMMAND redclaw_terminal_view_integration_tests)
  set_tests_properties(redclaw_terminal_view_integration_tests PROPERTIES TIMEOUT 60)
  if(TARGET Qt6::qmake)
    get_target_property(terminal_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(terminal_qt_bin "${terminal_qmake}" DIRECTORY)
    add_custom_command(TARGET redclaw_terminal_view_integration_tests POST_BUILD
      COMMAND "${terminal_qt_bin}/windeployqt.exe" --no-translations --no-compiler-runtime $<TARGET_FILE:redclaw_terminal_view_integration_tests>)
  endif()
endif()
if(WIN32)
  add_test(NAME redclaw_gui_metric_contract_tests
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gui_metric_contract_tests.ps1"
      -OutputRoot "${CMAKE_BINARY_DIR}/reports/gui-metric-contract-tests")
endif()
if(WIN32 AND TARGET redclaw_ui_playback)
  add_executable(redclaw_ui_d3d11_integration_tests ui/d3d11_playback_integration_tests.cpp)
  target_link_libraries(redclaw_ui_d3d11_integration_tests PRIVATE redclaw_ui_playback redclaw_helper Qt6::Widgets GTest::gtest d3d11)
  redclaw_apply_warnings(redclaw_ui_d3d11_integration_tests)
  add_test(NAME redclaw_ui_d3d11_integration_tests COMMAND redclaw_ui_d3d11_integration_tests)
  if(TARGET Qt6::qmake)
    get_target_property(redclaw_d3d_test_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(redclaw_d3d_test_qt_bin "${redclaw_d3d_test_qmake}" DIRECTORY)
    add_custom_command(TARGET redclaw_ui_d3d11_integration_tests POST_BUILD
      COMMAND "${redclaw_d3d_test_qt_bin}/windeployqt.exe" --no-translations --no-compiler-runtime $<TARGET_FILE:redclaw_ui_d3d11_integration_tests>)
  endif()
endif()
if(TARGET redclaw_ui_debug_control)
  find_package(Qt6 COMPONENTS Core REQUIRED)

  add_executable(redclaw_ui_debug_control_protocol_tests
    ui/debug_control_protocol_tests.cpp
  )

  target_link_libraries(redclaw_ui_debug_control_protocol_tests PRIVATE
    redclaw_ui_debug_control
    GTest::gtest
    GTest::gtest_main
  )

  add_custom_command(TARGET redclaw_ui_debug_control_protocol_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:Qt6::Core>
      $<TARGET_FILE_DIR:redclaw_ui_debug_control_protocol_tests>
  )

  redclaw_apply_warnings(redclaw_ui_debug_control_protocol_tests)

  add_test(
    NAME redclaw_ui_debug_control_protocol_tests
    COMMAND redclaw_ui_debug_control_protocol_tests
  )
endif()

if(TARGET redclaw_ui_agent_control)
  find_package(Qt6 COMPONENTS Core Network REQUIRED)

  add_executable(redclaw_ui_agent_control_server_tests
    ui/agent_control_server_tests.cpp
  )

  target_link_libraries(redclaw_ui_agent_control_server_tests PRIVATE
    redclaw_ui_agent_control
    redclaw_agent
    GTest::gtest
    GTest::gtest_main
  )

  add_custom_command(TARGET redclaw_ui_agent_control_server_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:Qt6::Core>
      $<TARGET_FILE:Qt6::Network>
      $<TARGET_FILE_DIR:redclaw_ui_agent_control_server_tests>
  )

  redclaw_apply_warnings(redclaw_ui_agent_control_server_tests)

  add_test(
    NAME redclaw_ui_agent_control_server_tests
    COMMAND redclaw_ui_agent_control_server_tests
  )
endif()

if(TARGET redclaw_ui_connection_flow)
  find_package(Qt6 COMPONENTS Core Gui Widgets REQUIRED)

  if(WIN32)
    add_executable(redclaw_agent_account_command_fixture
      ui/agent_account_command_fixture.cpp
    )
    redclaw_apply_warnings(redclaw_agent_account_command_fixture)
  endif()

  add_executable(redclaw_ui_connection_flow_tests
    ui/gui_latency_probe_tests.cpp
    ui/gui_diagnostic_writer_tests.cpp
    ui/runtime_control_queue_tests.cpp
    ui/runtime_log_view_tests.cpp
    ui/qa_input_probe_tests.cpp
    ui/agent_settings_dialog_tests.cpp
    ui/agent_conversation_panel_tests.cpp
    ui/connection_flow_tests.cpp
    ui/desktop_navigation_panel_tests.cpp
    ui/file_transfer_panel_tests.cpp
    ui/runtime_maintenance_context_tests.cpp
    ui/playback_frame_progress_tests.cpp
    ui/playback_geometry_transaction_tests.cpp
  )

  target_link_libraries(redclaw_ui_connection_flow_tests PRIVATE
    redclaw_ui_connection_flow
    redclaw_workspace
    redclaw_input
    GTest::gtest
  )

  if(WIN32)
    add_dependencies(redclaw_ui_connection_flow_tests redclaw_agent_account_command_fixture)
    target_compile_definitions(redclaw_ui_connection_flow_tests PRIVATE
      REDCLAW_AGENT_ACCOUNT_FIXTURE_PATH="$<TARGET_FILE:redclaw_agent_account_command_fixture>"
    )
  endif()

  if(WIN32 AND TARGET Qt6::qmake)
    get_target_property(redclaw_qt_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(redclaw_qt_bin_directory "${redclaw_qt_qmake_executable}" DIRECTORY)
    get_filename_component(redclaw_qt_root_directory "${redclaw_qt_bin_directory}" DIRECTORY)
    add_custom_command(TARGET redclaw_ui_connection_flow_tests POST_BUILD
      COMMAND "${redclaw_qt_bin_directory}/windeployqt.exe"
        --no-translations
        --no-compiler-runtime
        $<TARGET_FILE:redclaw_ui_connection_flow_tests>
      COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:redclaw_ui_connection_flow_tests>/platforms"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${redclaw_qt_root_directory}/plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
        "$<TARGET_FILE_DIR:redclaw_ui_connection_flow_tests>/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
    )
  else()
    add_custom_command(TARGET redclaw_ui_connection_flow_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Qt6::Core>
        $<TARGET_FILE:Qt6::Gui>
        $<TARGET_FILE:Qt6::Widgets>
        $<TARGET_FILE_DIR:redclaw_ui_connection_flow_tests>
    )
  endif()

  redclaw_apply_warnings(redclaw_ui_connection_flow_tests)

  add_test(
    NAME redclaw_ui_connection_flow_tests
    COMMAND redclaw_ui_connection_flow_tests
  )
endif()
