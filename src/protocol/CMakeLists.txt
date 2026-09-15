add_library(redclaw_protocol STATIC
  src/agent_protocol.cpp
  src/compressed_protobuf.cpp
  src/protobuf_messages.cpp
  src/debug_bridge_protocol.cpp
  src/protocol_module.cpp
  src/sdp_signaling.cpp
  src/stream_control_protocol.cpp
  src/terminal_protocol.cpp
  src/transfer_protocol.cpp
)

find_package(OpenSSL REQUIRED)
find_package(Protobuf REQUIRED)
find_package(zstd CONFIG REQUIRED)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/redclaw_wire.pb.cc ${CMAKE_CURRENT_BINARY_DIR}/redclaw_wire.pb.h
  COMMAND protobuf::protoc --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/schema
    ${CMAKE_CURRENT_SOURCE_DIR}/schema/redclaw_wire.proto
  DEPENDS schema/redclaw_wire.proto protobuf::protoc
  VERBATIM
)
target_sources(redclaw_protocol PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/redclaw_wire.pb.cc)

target_include_directories(redclaw_protocol
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_features(redclaw_protocol PUBLIC cxx_std_20)
target_link_libraries(redclaw_protocol PUBLIC protobuf::libprotobuf
  PRIVATE OpenSSL::Crypto $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>)
redclaw_apply_warnings(redclaw_protocol)

# The local PowerShell API uses the same production codec, not a second wire
# implementation or an old-text compatibility parser.
find_package(Boost REQUIRED COMPONENTS json)
add_executable(redclaw_protocol_codec tools/agent_wire_codec_main.cpp)
target_link_libraries(redclaw_protocol_codec PRIVATE redclaw_protocol Boost::json)
target_compile_features(redclaw_protocol_codec PRIVATE cxx_std_20)
set_target_properties(redclaw_protocol_codec PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src")
redclaw_apply_warnings(redclaw_protocol_codec)
