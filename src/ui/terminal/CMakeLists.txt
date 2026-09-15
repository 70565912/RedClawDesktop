find_package(unofficial-webview2 CONFIG REQUIRED)
file(READ "${PROJECT_SOURCE_DIR}/third_party/terminal/manifest.json" terminal_manifest)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/third_party/terminal/manifest.json")
foreach(asset xterm fit)
    string(JSON asset_version GET "${terminal_manifest}" ${asset} version)
    string(JSON asset_url GET "${terminal_manifest}" ${asset} url)
    string(JSON asset_hash GET "${terminal_manifest}" ${asset} sha256)
    set(asset_archive "${PROJECT_SOURCE_DIR}/build/dependencies/terminal/${asset}-${asset_version}.tgz")
    set(asset_directory "${PROJECT_SOURCE_DIR}/build/dependencies/terminal/${asset}-${asset_version}")
    if(NOT EXISTS "${asset_archive}")
        file(DOWNLOAD "${asset_url}" "${asset_archive}" EXPECTED_HASH "SHA256=${asset_hash}"
            TLS_VERIFY ON STATUS download_status)
        list(GET download_status 0 download_code)
        if(NOT download_code EQUAL 0)
            message(FATAL_ERROR "Terminal asset download failed: ${download_status}")
        endif()
    endif()
    file(SHA256 "${asset_archive}" actual_hash)
    if(NOT actual_hash STREQUAL asset_hash)
        message(FATAL_ERROR "Terminal asset hash mismatch: ${asset_archive}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${asset_archive}" DESTINATION "${asset_directory}")
    set(terminal_${asset}_directory "${asset_directory}/package")
endforeach()
configure_file(terminal_assets.qrc.in terminal_assets.qrc @ONLY)
qt6_add_resources(terminal_resources "${CMAKE_CURRENT_BINARY_DIR}/terminal_assets.qrc")
add_library(redclaw_ui_terminal STATIC terminal_view.cpp terminal_panel.cpp workspace_pipe_server.cpp ${terminal_resources})
target_include_directories(redclaw_ui_terminal PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(redclaw_ui_terminal PUBLIC cxx_std_20)
target_link_libraries(redclaw_ui_terminal PUBLIC Qt6::Widgets PRIVATE Qt6::Network redclaw_workspace unofficial::webview2::webview2)
redclaw_apply_warnings(redclaw_ui_terminal)
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:redclaw_desktop>/terminal-runtime.json"
    CONTENT "${terminal_manifest}\n")
get_target_property(terminal_sdk_include unofficial::webview2::webview2 INTERFACE_INCLUDE_DIRECTORIES)
get_filename_component(terminal_sdk_prefix "${terminal_sdk_include}" DIRECTORY)
add_custom_command(TARGET redclaw_ui_terminal POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:redclaw_desktop>/terminal-notices"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${terminal_xterm_directory}/LICENSE"
        "$<TARGET_FILE_DIR:redclaw_desktop>/terminal-notices/xterm-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${terminal_fit_directory}/LICENSE"
        "$<TARGET_FILE_DIR:redclaw_desktop>/terminal-notices/fit-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${terminal_sdk_prefix}/share/webview2/copyright"
        "$<TARGET_FILE_DIR:redclaw_desktop>/terminal-notices/WebView2-SDK-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${terminal_sdk_prefix}/share/webview2/NOTICE.txt"
        "$<TARGET_FILE_DIR:redclaw_desktop>/terminal-notices/WebView2-SDK-NOTICE.txt")
