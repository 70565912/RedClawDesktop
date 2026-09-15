get_filename_component(_webview2_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT TARGET unofficial::webview2::webview2)
    add_library(unofficial::webview2::webview2 STATIC IMPORTED)
    set_target_properties(unofficial::webview2::webview2 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_webview2_prefix}/include"
        IMPORTED_LOCATION "${_webview2_prefix}/lib/WebView2LoaderStatic.lib"
        INTERFACE_LINK_LIBRARIES "version;ole32;advapi32;shlwapi")
endif()
unset(_webview2_prefix)
