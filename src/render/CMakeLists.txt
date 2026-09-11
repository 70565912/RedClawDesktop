add_library(redclaw_render STATIC
  src/render_module.cpp
  src/audio_player.cpp
)

find_package(FFMPEG QUIET)

target_include_directories(redclaw_render
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(FFMPEG_FOUND)
  target_include_directories(redclaw_render
    PRIVATE
      ${FFMPEG_INCLUDE_DIRS}
  )

  target_link_directories(redclaw_render
    PUBLIC
      ${FFMPEG_LIBRARY_DIRS}
  )

  target_link_libraries(redclaw_render
    PUBLIC
      ${FFMPEG_LIBRARIES}
  )
endif()

target_compile_features(redclaw_render PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_render)
