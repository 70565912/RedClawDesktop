add_library(redclaw_capture STATIC
  src/capture_module.cpp
  src/capture_recovery.cpp
  src/capture_failure_evidence.cpp
  src/capture_cursor.cpp
  src/capture_cursor_d3d11.cpp
  src/audio_mix.cpp
  src/audio_encoder.cpp
  src/audio_loopback.cpp
)

find_package(FFMPEG QUIET)
find_package(Opus CONFIG REQUIRED)

target_include_directories(redclaw_capture
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(redclaw_capture PUBLIC Opus::opus)

if(WIN32)
  target_link_libraries(redclaw_capture
    PRIVATE
      d3d11
      d3dcompiler
      dxgi
      mmdevapi
      ole32
      windowscodecs
      windowsapp
      wtsapi32
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

target_link_libraries(redclaw_capture PUBLIC redclaw_protocol)

target_compile_features(redclaw_capture PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_capture)
