add_library(redclaw_net STATIC
  src/net_module.cpp
  src/stun_server_probe.cpp
  src/ice_candidate_diagnostics.cpp
  src/ice_udp_mapping.cpp
  src/video_frame_transport.cpp
  src/navigation_thumbnail_transport.cpp
  src/sent_video_frame_metadata.cpp
  src/media_transport_feedback.cpp
  src/media_congestion_controller.cpp
  src/media_pacing_budget.cpp
  src/desktop_media_send_pacer.cpp
  src/media_frame_trace.cpp
  src/receiver_stream_policy.cpp
)

find_package(LibDataChannel CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

target_include_directories(redclaw_net
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(redclaw_net
  PUBLIC
    LibDataChannel::LibDataChannel
  PRIVATE OpenSSL::Crypto
)

if(WIN32)
  target_link_libraries(redclaw_net PUBLIC ws2_32)
endif()

target_compile_features(redclaw_net PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_net)
