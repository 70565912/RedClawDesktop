add_library(redclaw_workspace STATIC
    src/terminal_session.cpp src/terminal_host.cpp src/terminal_controller.cpp src/terminal_shell_integration.cpp
    src/local_workspace_pipe.cpp src/terminal_runtime_bridge.cpp
    src/transfer_file_receiver.cpp src/transfer_file_source.cpp src/transfer_batch_receiver.cpp
    src/transfer_source_manifest.cpp src/transfer_batch_sender.cpp src/transfer_operation_gate.cpp
    src/transfer_worker.cpp src/transfer_runtime_bridge.cpp src/directory_browser.cpp src/transfer_result_journal.cpp
    src/clipboard_paste_guard.cpp src/clipboard_payload.cpp src/clipboard_host_paste.cpp src/clipboard_copy_store.cpp)
find_package(OpenSSL REQUIRED)
target_include_directories(redclaw_workspace PUBLIC include)
target_compile_features(redclaw_workspace PUBLIC cxx_std_20)
target_link_libraries(redclaw_workspace PUBLIC redclaw_protocol PRIVATE OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(redclaw_workspace PRIVATE user32 shell32)
endif()
redclaw_apply_warnings(redclaw_workspace)
