#include "redclaw/net/video_frame_transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include "media_transport_limits.h"

namespace redclaw::net {
namespace {
using transport_detail::assign_error;
constexpr std::array<std::uint8_t, 8> kNavigationMagic = {
    'R', 'C', 'N', 'A', 'V', 'J', '1', '\0'};
constexpr std::uint16_t kNavigationVersion = 1;
constexpr std::size_t kNavigationHeaderBytes = 48;
constexpr std::size_t kMaxNavigationDisplayIdBytes = 128;
template <typename Value>
void write_fixed_unsigned_le(std::uint8_t* output, Value value) {
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        output[index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
}

template <typename Value>
Value read_fixed_unsigned_le(const std::uint8_t* input) {
    Value value = 0;
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        value |= static_cast<Value>(input[index]) << (index * 8U);
    }
    return value;
}
}  // namespace

bool serialize_navigation_thumbnail(
    const NavigationThumbnailView& thumbnail,
    std::vector<std::uint8_t>* packet,
    std::string* error_detail) {
    if (packet == nullptr) {
        assign_error("navigation thumbnail output packet is null", error_detail);
        return false;
    }
    packet->clear();
    if (thumbnail.catalog_revision == 0 || thumbnail.thumbnail_revision == 0
        || thumbnail.width == 0 || thumbnail.height == 0
        || thumbnail.width > 320 || thumbnail.height > 320
        || thumbnail.display_id.empty()
        || thumbnail.display_id.size() > kMaxNavigationDisplayIdBytes
        || thumbnail.jpeg.empty()
        || thumbnail.jpeg.size() > kMaxNavigationThumbnailJpegBytes) {
        assign_error("navigation thumbnail fields are outside protocol bounds", error_detail);
        return false;
    }
    packet->resize(kNavigationHeaderBytes + thumbnail.display_id.size() + thumbnail.jpeg.size());
    auto* bytes = packet->data();
    std::copy(kNavigationMagic.begin(), kNavigationMagic.end(), bytes);
    write_fixed_unsigned_le(bytes + 8, kNavigationVersion);
    write_fixed_unsigned_le(bytes + 10, static_cast<std::uint16_t>(kNavigationHeaderBytes));
    write_fixed_unsigned_le(bytes + 12, thumbnail.catalog_revision);
    write_fixed_unsigned_le(bytes + 20, thumbnail.thumbnail_revision);
    write_fixed_unsigned_le(bytes + 28, thumbnail.width);
    write_fixed_unsigned_le(bytes + 32, thumbnail.height);
    write_fixed_unsigned_le(bytes + 36, static_cast<std::uint32_t>(thumbnail.display_id.size()));
    write_fixed_unsigned_le(bytes + 40, static_cast<std::uint32_t>(thumbnail.jpeg.size()));
    write_fixed_unsigned_le(bytes + 44, std::uint32_t{0});
    std::memcpy(bytes + kNavigationHeaderBytes,
                thumbnail.display_id.data(), thumbnail.display_id.size());
    std::memcpy(bytes + kNavigationHeaderBytes + thumbnail.display_id.size(),
                thumbnail.jpeg.data(), thumbnail.jpeg.size());
    assign_error({}, error_detail);
    return true;
}

bool parse_navigation_thumbnail(
    std::span<const std::uint8_t> packet,
    NavigationThumbnail* thumbnail,
    std::string* error_detail) {
    if (thumbnail == nullptr) {
        assign_error("navigation thumbnail output is null", error_detail);
        return false;
    }
    *thumbnail = {};
    if (packet.size() < kNavigationHeaderBytes
        || !std::equal(kNavigationMagic.begin(), kNavigationMagic.end(), packet.begin())) {
        assign_error("navigation thumbnail header is invalid", error_detail);
        return false;
    }
    const auto version = read_fixed_unsigned_le<std::uint16_t>(packet.data() + 8);
    const auto header_size = read_fixed_unsigned_le<std::uint16_t>(packet.data() + 10);
    const auto display_id_size = read_fixed_unsigned_le<std::uint32_t>(packet.data() + 36);
    const auto jpeg_size = read_fixed_unsigned_le<std::uint32_t>(packet.data() + 40);
    if (version != kNavigationVersion || header_size != kNavigationHeaderBytes
        || display_id_size == 0 || display_id_size > kMaxNavigationDisplayIdBytes
        || jpeg_size == 0 || jpeg_size > kMaxNavigationThumbnailJpegBytes
        || packet.size() != kNavigationHeaderBytes + display_id_size + jpeg_size) {
        assign_error("navigation thumbnail packet is outside protocol bounds", error_detail);
        return false;
    }
    thumbnail->catalog_revision = read_fixed_unsigned_le<std::uint64_t>(packet.data() + 12);
    thumbnail->thumbnail_revision = read_fixed_unsigned_le<std::uint64_t>(packet.data() + 20);
    thumbnail->width = read_fixed_unsigned_le<std::uint32_t>(packet.data() + 28);
    thumbnail->height = read_fixed_unsigned_le<std::uint32_t>(packet.data() + 32);
    if (thumbnail->catalog_revision == 0 || thumbnail->thumbnail_revision == 0
        || thumbnail->width == 0 || thumbnail->height == 0
        || thumbnail->width > 320 || thumbnail->height > 320) {
        assign_error("navigation thumbnail dimensions or revisions are invalid", error_detail);
        return false;
    }
    const auto* id_begin = packet.data() + kNavigationHeaderBytes;
    thumbnail->display_id.assign(
        reinterpret_cast<const char*>(id_begin), display_id_size);
    thumbnail->jpeg.assign(id_begin + display_id_size,
                           id_begin + display_id_size + jpeg_size);
    assign_error({}, error_detail);
    return true;
}

}  // namespace redclaw::net
