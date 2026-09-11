#pragma once
#include "runtime_options.h"
#include "redclaw/helper/runtime_profile.h"
#include <string_view>

namespace redclaw::runtime {
bool parse_uint16(std::string_view value, std::uint16_t* out);
bool parse_uint32(std::string_view value, std::uint32_t* out);
bool append_ice_servers_from_file(const std::string& path, std::vector<std::string>* servers, std::string* error);
bool assign_runtime_role(std::string_view value, RuntimeOptions* options, std::string* error);
bool apply_runtime_profile(const redclaw::helper::RuntimeProfileData& profile, RuntimeOptions* options, std::string* error);
}  // namespace redclaw::runtime
