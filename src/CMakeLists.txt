add_subdirectory(core)
add_subdirectory(protocol)
add_subdirectory(agent)
add_subdirectory(net)
add_subdirectory(security)
add_subdirectory(session)
add_subdirectory(capture)
add_subdirectory(render)
add_subdirectory(input)
add_subdirectory(service)
add_subdirectory(helper)
add_subdirectory(diag)
add_subdirectory(runtime)
add_subdirectory(workspace)

set(REDCLAW_DESKTOP_SOURCES
  main.cpp
  ui/gui_shell.cpp
)

set(REDCLAW_QT_GUI_ENABLED OFF)
set(REDCLAW_QT_OPENGL_ENABLED OFF)
find_package(Qt6 COMPONENTS Widgets Network OpenGLWidgets Concurrent QUIET)
if (Qt6_FOUND AND TARGET Qt6::Widgets AND TARGET Qt6::Network)
  set(REDCLAW_QT_GUI_ENABLED ON)
endif()
if (REDCLAW_QT_GUI_ENABLED AND TARGET Qt6::OpenGLWidgets)
  set(REDCLAW_QT_OPENGL_ENABLED ON)
endif()

if (WIN32)
  add_executable(redclaw_desktop WIN32
    ${REDCLAW_DESKTOP_SOURCES}
  )
else()
  add_executable(redclaw_desktop
    ${REDCLAW_DESKTOP_SOURCES}
  )
endif()

add_executable(redclaw_host_service
  host_service_main.cpp
)

add_executable(redclaw_rendezvous_service
  rendezvous_service_main.cpp
)

if(WIN32)
  find_package(Boost REQUIRED COMPONENTS json)
  add_executable(redclaw_debug_bridge
    debug_bridge_main.cpp
  )
  target_link_libraries(redclaw_debug_bridge PRIVATE
    redclaw_protocol
    redclaw_agent
    redclaw_net
    Boost::json
    crypt32
    advapi32
  )
  redclaw_apply_warnings(redclaw_debug_bridge)
endif()

if (REDCLAW_QT_GUI_ENABLED)
  if(WIN32)
    add_subdirectory(ui/terminal)
  endif()
  add_library(redclaw_ui_playback STATIC
    ui/playback/playback_canvas_factory.cpp
    ui/playback/d3d11_playback_canvas.cpp
    ui/playback/opengl_playback_canvas.cpp
    ui/playback/qt_playback_canvas.cpp
    ui/playback/direct_frame_receiver.cpp
  )
  target_include_directories(redclaw_ui_playback PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(redclaw_ui_playback PUBLIC Qt6::Widgets redclaw_render redclaw_diag PRIVATE redclaw_helper)
  target_compile_features(redclaw_ui_playback PUBLIC cxx_std_20)
  redclaw_apply_warnings(redclaw_ui_playback)
  if(WIN32)
    target_link_libraries(redclaw_ui_playback PRIVATE d3d11 dxgi dxguid)
  endif()
  if(REDCLAW_QT_OPENGL_ENABLED)
    target_link_libraries(redclaw_ui_playback PRIVATE Qt6::OpenGLWidgets)
    target_compile_definitions(redclaw_ui_playback PRIVATE REDCLAW_ENABLE_QT_OPENGL=1)
  endif()

  add_library(redclaw_ui_agent_control STATIC
    ui/agent_control_server.cpp
  )
  target_include_directories(redclaw_ui_agent_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(redclaw_ui_agent_control
    PUBLIC Qt6::Core Qt6::Network redclaw_protocol
    PRIVATE redclaw_agent
  )
  redclaw_apply_warnings(redclaw_ui_agent_control)

  add_library(redclaw_ui_debug_control STATIC
    ui/debug_control_protocol.cpp
  )
  target_include_directories(redclaw_ui_debug_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(redclaw_ui_debug_control PUBLIC Qt6::Core)
  redclaw_apply_warnings(redclaw_ui_debug_control)

  add_library(redclaw_ui_connection_flow STATIC
    ui/gui_diagnostic_writer.cpp
    ui/agent_conversation_panel.cpp
    ui/agent_conversation_viewport.cpp
    ui/agent_panel_presentation.cpp
    ui/desktop_navigation_panel.cpp
    ui/file_transfer_panel.cpp
    ui/file_transfer_results_dialog.cpp
    ui/clipboard_copies_dialog.cpp
    ui/runtime_maintenance_context.cpp
    ui/agent_settings_dialog.cpp
    ui/connection_entry_page.cpp
    ui/connection_flow_model.cpp
    ui/connection_progress_page.cpp
    ui/playback_control_hint_overlay.cpp
    ui/playback_geometry_transaction.cpp
    ui/playback_window_lifecycle.cpp
    ui/playback_window_geometry_controller.cpp
    ui/remote_input_capture.cpp
    ui/stun_server_policy.cpp
  )
  target_include_directories(redclaw_ui_connection_flow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(redclaw_ui_connection_flow
    PUBLIC Qt6::Widgets redclaw_protocol redclaw_diag
    PRIVATE Qt6::Concurrent redclaw_agent redclaw_input redclaw_workspace
  )
  if(WIN32)
    target_link_libraries(redclaw_ui_connection_flow PRIVATE dwmapi advapi32)
  endif()
  redclaw_apply_warnings(redclaw_ui_connection_flow)

  target_link_libraries(redclaw_desktop PRIVATE
    Qt6::Widgets
    Qt6::Network
    redclaw_ui_agent_control
    redclaw_ui_debug_control
    redclaw_ui_connection_flow
    redclaw_ui_playback
  )
  target_compile_definitions(redclaw_desktop PRIVATE REDCLAW_ENABLE_QT_GUI=1)
  if(WIN32)
    target_link_libraries(redclaw_desktop PRIVATE redclaw_ui_terminal)
  endif()
endif()

if (REDCLAW_QT_OPENGL_ENABLED)
  target_link_libraries(redclaw_desktop PRIVATE Qt6::OpenGLWidgets)
  target_compile_definitions(redclaw_desktop PRIVATE REDCLAW_ENABLE_QT_OPENGL=1)
endif()

target_link_libraries(redclaw_desktop PRIVATE
  redclaw_core
  redclaw_runtime_options
  redclaw_protocol
  redclaw_agent
  redclaw_net
  redclaw_workspace
  redclaw_security
  redclaw_session
  redclaw_capture
  redclaw_render
  redclaw_input
  redclaw_service
  redclaw_helper
  redclaw_diag
)

target_link_libraries(redclaw_host_service PRIVATE
  redclaw_core
  redclaw_service
)

target_link_libraries(redclaw_rendezvous_service PRIVATE
  redclaw_service
)

if (WIN32)
  target_link_libraries(redclaw_desktop PRIVATE ws2_32 winhttp xaudio2 dwmapi)
  target_link_libraries(redclaw_render   PRIVATE xaudio2)
  target_link_libraries(redclaw_host_service PRIVATE advapi32)
  target_link_libraries(redclaw_rendezvous_service PRIVATE ws2_32)
endif()

redclaw_apply_warnings(redclaw_desktop)
add_dependencies(redclaw_desktop redclaw_protocol_codec)
redclaw_apply_warnings(redclaw_host_service)
redclaw_apply_warnings(redclaw_rendezvous_service)
