vcpkg_download_distfile(ARCHIVE
    URLS "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${VERSION}/microsoft.web.webview2.${VERSION}.nupkg"
    FILENAME "microsoft.web.webview2.${VERSION}.zip"
    SHA512 adf91bda1a71d860c098cd0e426b5a238e187ea73c694832f8fde36809a9165fda8844fc9c2b9fc8a870b0da53cd9eb97ec140d9a2f104a228301ffae841db8c)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}" NO_REMOVE_ONE_LEVEL)
file(COPY "${SOURCE_PATH}/build/native/include/" DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(COPY "${SOURCE_PATH}/build/native/x64/WebView2LoaderStatic.lib" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
if(NOT VCPKG_BUILD_TYPE)
    file(COPY "${CURRENT_PACKAGES_DIR}/lib" DESTINATION "${CURRENT_PACKAGES_DIR}/debug")
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/unofficial-webview2-config.cmake"
    "${CURRENT_PACKAGES_DIR}/share/unofficial-webview2/unofficial-webview2-config.cmake" COPYONLY)
configure_file("${SOURCE_PATH}/LICENSE.txt" "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright" COPYONLY)
configure_file("${SOURCE_PATH}/NOTICE.txt" "${CURRENT_PACKAGES_DIR}/share/${PORT}/NOTICE.txt" COPYONLY)
set(VCPKG_POLICY_EMPTY_PACKAGE enabled)
