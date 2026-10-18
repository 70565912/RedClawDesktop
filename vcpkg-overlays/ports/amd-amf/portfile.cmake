# AMD replaced the v1.5.3 asset on 2026-10-16. All 57 headers were verified
# against upstream commit 57ee0cfb0e1a08d9041f77435653794c1b012be0; see README.md.
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/GPUOpen-LibrariesAndSDKs/AMF/releases/download/v${VERSION}/AMF-headers-v${VERSION}.tar.gz"
    FILENAME "AMF-headers-v${VERSION}-648395569.tar.gz"
    SHA512 06fbb2269f15d9f0c98bc6158782a12d8a82e8b6a6335b42def14198476191a20ba85f72694fc91c7659ab7cec45c2a97c9e4dc3fd11c83c4ff9cdeaad33d112
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
)

vcpkg_download_distfile(LICENSE_FILE
    URLS "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/AMF/57ee0cfb0e1a08d9041f77435653794c1b012be0/LICENSE.txt"
    FILENAME "AMF-v${VERSION}-LICENSE.txt"
    SHA512 6b3261e5f38179c0d96483e44b339933a8eb0d9324784953eed74dfe2658ab9d94a9afb09d85ac1138300c8272ac73fb5e1e1f56ea26312f572453fab86f228a
)

file(INSTALL "${SOURCE_PATH}/AMF/" DESTINATION "${CURRENT_PACKAGES_DIR}/include/AMF")
vcpkg_install_copyright(FILE_LIST "${LICENSE_FILE}")
