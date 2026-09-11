#pragma once
#include <string>
#include <string_view>
#include <vector>
namespace redclaw::net {
// Internal synchronous readiness probe. Preserve server ordering and fallback;
// extracting it must not change ICE initialization or add background workers.
std::vector<std::string> select_ice_servers_for_gathering(
    const std::vector<std::string>& configured_servers, std::string_view bind_address);
}
