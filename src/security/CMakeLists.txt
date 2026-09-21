add_library(redclaw_security STATIC
  src/security_module.cpp
  src/connection_auth.cpp
)

target_include_directories(redclaw_security
  PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(redclaw_security PUBLIC cxx_std_20)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)
target_link_libraries(redclaw_security PUBLIC redclaw_protocol PRIVATE OpenSSL::Crypto Boost::json)
if(WIN32)
  target_link_libraries(redclaw_security PRIVATE crypt32)
endif()
redclaw_apply_warnings(redclaw_security)
