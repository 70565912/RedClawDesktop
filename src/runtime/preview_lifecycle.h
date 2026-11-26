#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace redclaw::runtime {
struct PreviewLifecycle {
    static constexpr std::string_view prefix = "RC_FILE_PREVIEW_V1 ";
    std::uint64_t generation = 0;
    bool active = false;

    std::string encode() const {
        return generation ? std::string(prefix) + (active ? '+' : '-') + std::to_string(generation) : std::string{};
    }
    static bool parse(std::string_view line, PreviewLifecycle& state) {
        if (!line.starts_with(prefix)) return false;
        line.remove_prefix(prefix.size());
        if (line.size() < 2 || (line.front() != '+' && line.front() != '-')) return false;
        const auto end = line.data() + line.size();
        std::uint64_t generation = 0;
        const auto result = std::from_chars(line.data() + 1, end, generation);
        if (result.ec != std::errc{} || result.ptr != end || !generation) return false;
        state = {generation, line.front() == '+'};
        return true;
    }
};
}
