add_library(redclaw_capture STATIC
  src/capture_module.cpp
)

find_package(FFMPEG QUIET)

target_include_directories(redclaw_capture
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
  target_link_libraries(redclaw_capture
    PRIVATE
      d3d11
      dxgi
      ole32
      windowscodecs
      windowsapp
  )
endif()

if(FFMPEG_FOUND)
  target_include_directories(redclaw_capture
    PRIVATE
      ${FFMPEG_INCLUDE_DIRS}
  )

  target_link_directories(redclaw_capture
    PUBLIC
      ${FFMPEG_LIBRARY_DIRS}
  )

  target_link_libraries(redclaw_capture
    PUBLIC
      ${FFMPEG_LIBRARIES}
  )
endif()

target_compile_features(redclaw_capture PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_capture)
