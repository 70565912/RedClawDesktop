find_package(OpenSSL REQUIRED)

set(REDCLAW_SERVICE_SOURCES
  src/service_module.cpp
  src/dht_rendezvous.cpp
  src/dht_publication_transaction.cpp
  src/ipc_channel_contracts.cpp
  src/ipc_channel.cpp
  src/capability_registry.cpp
  src/capability_sync.cpp
  src/capability_downgrade.cpp
  src/helper_launcher.cpp
  src/session_detection.cpp
  src/connection_negotiation.cpp
  src/upnp_port_mapping.cpp
)

set(REDCLAW_SERVICE_PRIVATE_LIBS
  redclaw_diag
  redclaw_protocol
  OpenSSL::Crypto
)

if(REDCLAW_ENABLE_DHT_BACKENDS)
  find_package(LibtorrentRasterbar CONFIG REQUIRED)
  find_package(miniupnpc CONFIG REQUIRED)
  list(APPEND REDCLAW_SERVICE_SOURCES src/dht_rendezvous_libtorrent.cpp src/dht_listen_port_selection.cpp)
  list(APPEND REDCLAW_SERVICE_PRIVATE_LIBS
    LibtorrentRasterbar::torrent-rasterbar
    miniupnpc::miniupnpc
  )
  if(WIN32)
    list(APPEND REDCLAW_SERVICE_PRIVATE_LIBS ws2_32)
  endif()
endif()

add_library(redclaw_service STATIC
  ${REDCLAW_SERVICE_SOURCES}
)

target_include_directories(redclaw_service
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(redclaw_service
  PRIVATE
    ${REDCLAW_SERVICE_PRIVATE_LIBS}
)

if(REDCLAW_ENABLE_DHT_BACKENDS)
  target_compile_definitions(redclaw_service PUBLIC REDCLAW_ENABLE_DHT_BACKENDS=1)
endif()

target_compile_features(redclaw_service PUBLIC cxx_std_20)
redclaw_apply_warnings(redclaw_service)
