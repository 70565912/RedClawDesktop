#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstddef>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace redclaw::helper {

struct DirectFrameChannelFrameHeader {
    char magic[8];
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_pitch = 0;
    std::uint32_t format = 0;
    std::uint64_t timestamp_ms = 0;
    std::uint64_t frame_id = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t flags = 0;
    std::uint64_t capture_region_revision = 0;
    std::uint32_t content_rect_x = 0;
    std::uint32_t content_rect_y = 0;
    std::uint32_t content_rect_width = 0;
    std::uint32_t content_rect_height = 0;
    char display_id[128]{};
};

struct alignas(8) DirectFrameSharedMemoryHeader {
    char magic[8];
    std::uint32_t version = 0;
    std::uint32_t slot_count = 0;
    std::uint32_t slot_payload_bytes = 0;
    std::uint32_t reserved = 0;
    volatile LONG64 write_sequence = 0;
    volatile LONG64 latest_sequence = 0;
    volatile LONG64 reader_sequence = 0;
    volatile LONG64 reader_active_sequence = 0;
    volatile LONG64 writer_frame_count = 0;
    volatile LONG64 writer_oversize_drop_count = 0;
    volatile LONG64 writer_busy_drop_count = 0;
};

struct alignas(8) DirectFrameSharedMemorySlotHeader {
    volatile LONG64 committed_sequence = 0;
    DirectFrameChannelFrameHeader frame;
};

constexpr char kDirectFrameChannelMagic[8] = {'R', 'C', 'D', 'F', 'R', 'M', '2', '\0'};
constexpr char kDirectFrameSharedMemoryMagic[8] = {'R', 'C', 'D', 'S', 'H', 'M', '1', '\0'};

constexpr std::uint32_t kDirectFrameFormatBgra = 1;
constexpr std::uint32_t kDirectFrameFormatH264 = 2;
constexpr std::uint32_t kDirectFrameFormatHevc = 3;
constexpr std::uint32_t kDirectFrameFormatJpeg = 4;

constexpr std::uint32_t kDirectFrameSharedMemoryVersion = 3;
constexpr std::uint32_t kDirectFrameSharedMemorySlotCount = 2;
constexpr std::uint32_t kDirectFrameSharedMemorySlotPayloadBytes = 8U * 1024U * 1024U;
constexpr std::uint32_t kDirectFrameMaxPayloadBytes = kDirectFrameSharedMemorySlotPayloadBytes;

enum class DirectFrameSharedSnapshotStatus {
    kCopied,
    kMissingSequence,
    kInvalidPayload,
    kBufferNotPrepared,
    kSequenceChanged,
};

struct DirectFrameSharedSnapshot {
    DirectFrameChannelFrameHeader frame;
    std::vector<std::uint8_t> payload;
};

inline std::wstring widen_ascii(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

inline std::wstring direct_frame_shared_mapping_name(std::string_view channel_name) {
    return std::wstring(L"Local\\redclaw-frame-map-") + widen_ascii(channel_name);
}

inline std::wstring direct_frame_shared_event_name(std::string_view channel_name) {
    return std::wstring(L"Local\\redclaw-frame-event-") + widen_ascii(channel_name);
}

inline LONG64 direct_frame_atomic_load_i64(volatile LONG64* value) {
    return InterlockedCompareExchange64(value, 0, 0);
}

inline LONG64 direct_frame_atomic_load_i64(volatile const LONG64* value) {
    return InterlockedCompareExchange64(const_cast<volatile LONG64*>(value), 0, 0);
}

inline LONG64 direct_frame_atomic_store_i64(volatile LONG64* value, LONG64 new_value) {
    return InterlockedExchange64(value, new_value);
}

inline LONG64 direct_frame_atomic_increment_i64(volatile LONG64* value) {
    return InterlockedIncrement64(value);
}

inline std::size_t direct_frame_shared_slot_stride() {
    return sizeof(DirectFrameSharedMemorySlotHeader)
        + static_cast<std::size_t>(kDirectFrameSharedMemorySlotPayloadBytes);
}

inline std::size_t direct_frame_shared_mapping_size() {
    return sizeof(DirectFrameSharedMemoryHeader)
        + static_cast<std::size_t>(kDirectFrameSharedMemorySlotCount) * direct_frame_shared_slot_stride();
}

inline void initialize_direct_frame_shared_memory(DirectFrameSharedMemoryHeader* header) {
    if (header == nullptr) {
        return;
    }

    std::memcpy(header->magic, kDirectFrameSharedMemoryMagic, sizeof(kDirectFrameSharedMemoryMagic));
    header->version = kDirectFrameSharedMemoryVersion;
    header->slot_count = kDirectFrameSharedMemorySlotCount;
    header->slot_payload_bytes = kDirectFrameSharedMemorySlotPayloadBytes;
    header->reserved = 0;
    direct_frame_atomic_store_i64(&header->write_sequence, 0);
    direct_frame_atomic_store_i64(&header->latest_sequence, 0);
    direct_frame_atomic_store_i64(&header->reader_sequence, 0);
    direct_frame_atomic_store_i64(&header->reader_active_sequence, 0);
    direct_frame_atomic_store_i64(&header->writer_frame_count, 0);
    direct_frame_atomic_store_i64(&header->writer_oversize_drop_count, 0);
    direct_frame_atomic_store_i64(&header->writer_busy_drop_count, 0);
}

inline DirectFrameSharedMemorySlotHeader* direct_frame_shared_slot_header(void* mapping_view, std::uint32_t slot_index) {
    auto* base = static_cast<std::byte*>(mapping_view);
    return reinterpret_cast<DirectFrameSharedMemorySlotHeader*>(
        base + sizeof(DirectFrameSharedMemoryHeader)
        + static_cast<std::size_t>(slot_index) * direct_frame_shared_slot_stride());
}

inline const DirectFrameSharedMemorySlotHeader* direct_frame_shared_slot_header(const void* mapping_view, std::uint32_t slot_index) {
    auto* base = static_cast<const std::byte*>(mapping_view);
    return reinterpret_cast<const DirectFrameSharedMemorySlotHeader*>(
        base + sizeof(DirectFrameSharedMemoryHeader)
        + static_cast<std::size_t>(slot_index) * direct_frame_shared_slot_stride());
}

inline std::uint8_t* direct_frame_shared_slot_payload(DirectFrameSharedMemorySlotHeader* slot_header) {
    return reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::byte*>(slot_header) + sizeof(DirectFrameSharedMemorySlotHeader));
}

inline const std::uint8_t* direct_frame_shared_slot_payload(const DirectFrameSharedMemorySlotHeader* slot_header) {
    return reinterpret_cast<const std::uint8_t*>(reinterpret_cast<const std::byte*>(slot_header) + sizeof(DirectFrameSharedMemorySlotHeader));
}

struct DirectFrameSharedReadPlan {
    std::uint64_t sequence = 0;
    bool dependency_catchup = false;
    bool independent_skip = false;
};

// Metadata inspection is bounded to the two existing slots. The subsequent
// payload copy must still validate committed_sequence: inspection is not a lease.
inline DirectFrameSharedReadPlan plan_direct_frame_shared_read(
    const void* mapping_view,
    DirectFrameSharedMemoryHeader* header,
    std::uint64_t observed_sequence,
    bool awaiting_keyframe) {
    if (mapping_view == nullptr || header == nullptr
        || header->slot_count != kDirectFrameSharedMemorySlotCount) return {};
    const auto latest = static_cast<std::uint64_t>(direct_frame_atomic_load_i64(&header->latest_sequence));
    if (latest == 0 || latest <= observed_sequence) return {};

    struct RetainedFrame {
        std::uint64_t sequence = 0;
        DirectFrameChannelFrameHeader frame{};
        bool independent = false;
    };
    std::array<RetainedFrame, kDirectFrameSharedMemorySlotCount> retained{};
    for (std::size_t offset = 0; offset < retained.size() && latest > offset; ++offset) {
        const auto sequence = latest - offset;
        const auto* slot = direct_frame_shared_slot_header(mapping_view,
            static_cast<std::uint32_t>((sequence - 1) % header->slot_count));
        direct_frame_atomic_store_i64(&header->reader_active_sequence, static_cast<LONG64>(sequence));
        if (static_cast<std::uint64_t>(direct_frame_atomic_load_i64(&slot->committed_sequence)) == sequence) {
            retained[offset].frame = slot->frame;
            MemoryBarrier();
            if (static_cast<std::uint64_t>(direct_frame_atomic_load_i64(&slot->committed_sequence)) == sequence) {
                retained[offset].sequence = sequence;
                const auto& frame = retained[offset].frame;
                retained[offset].independent = frame.format == kDirectFrameFormatBgra
                    || frame.format == kDirectFrameFormatJpeg || (frame.flags & 1U) != 0;
            }
        }
        direct_frame_atomic_store_i64(&header->reader_active_sequence, 0);
    }
    const auto& newest = retained[0];
    if (newest.sequence == 0) return {};
    if (newest.independent) return {latest, false, latest - observed_sequence > 1};
    for (const auto& frame : retained) {
        if (frame.sequence <= observed_sequence
            || frame.frame.capture_region_revision != newest.frame.capture_region_revision
            || frame.frame.width != newest.frame.width || frame.frame.height != newest.frame.height
            || frame.frame.format != newest.frame.format) continue;
        if ((!awaiting_keyframe && frame.sequence == observed_sequence + 1) || frame.independent) {
            return {frame.sequence, frame.sequence != latest, false};
        }
    }
    // The dependency really is no longer retained. Let the recovery coordinator
    // handle this newest access unit; never decode a P frame across the gap.
    return {latest, false, false};
}

inline bool prepare_direct_frame_shared_snapshot(
    DirectFrameSharedSnapshot* snapshot,
    std::size_t payload_capacity = kDirectFrameMaxPayloadBytes) {
    if (snapshot == nullptr || payload_capacity == 0 || payload_capacity > kDirectFrameMaxPayloadBytes) {
        return false;
    }
    try {
        snapshot->payload.reserve(payload_capacity);
    } catch (...) {
        return false;
    }
    return snapshot->payload.capacity() >= payload_capacity;
}

inline DirectFrameSharedSnapshotStatus copy_direct_frame_shared_sequence(
    const void* mapping_view,
    DirectFrameSharedMemoryHeader* shared_header,
    std::uint64_t sequence,
    DirectFrameSharedSnapshot* snapshot) {
    if (mapping_view == nullptr || shared_header == nullptr || snapshot == nullptr || sequence == 0
        || shared_header->slot_count != kDirectFrameSharedMemorySlotCount
        || shared_header->slot_payload_bytes != kDirectFrameSharedMemorySlotPayloadBytes) {
        return DirectFrameSharedSnapshotStatus::kInvalidPayload;
    }
    if (snapshot->payload.capacity() < shared_header->slot_payload_bytes) {
        return DirectFrameSharedSnapshotStatus::kBufferNotPrepared;
    }

    const std::uint32_t slot_index =
        static_cast<std::uint32_t>((sequence - 1) % shared_header->slot_count);
    const auto* slot_header = direct_frame_shared_slot_header(mapping_view, slot_index);

    direct_frame_atomic_store_i64(
        &shared_header->reader_active_sequence,
        static_cast<LONG64>(sequence));
    const auto release_slot = [&]() {
        direct_frame_atomic_store_i64(&shared_header->reader_active_sequence, 0);
    };

    const LONG64 committed_before = direct_frame_atomic_load_i64(&slot_header->committed_sequence);
    if (static_cast<std::uint64_t>(committed_before) != sequence) {
        release_slot();
        return DirectFrameSharedSnapshotStatus::kMissingSequence;
    }

    const DirectFrameChannelFrameHeader frame = slot_header->frame;
    if (frame.payload_size == 0 || frame.payload_size > shared_header->slot_payload_bytes) {
        release_slot();
        return DirectFrameSharedSnapshotStatus::kInvalidPayload;
    }

    snapshot->payload.resize(frame.payload_size);
    const auto* payload = direct_frame_shared_slot_payload(slot_header);
    std::memcpy(snapshot->payload.data(), payload, frame.payload_size);
    MemoryBarrier();
    const LONG64 committed_after = direct_frame_atomic_load_i64(&slot_header->committed_sequence);
    release_slot();

    if (static_cast<std::uint64_t>(committed_after) != sequence) {
        snapshot->payload.clear();
        return DirectFrameSharedSnapshotStatus::kSequenceChanged;
    }

    snapshot->frame = frame;
    direct_frame_atomic_store_i64(
        &shared_header->reader_sequence,
        static_cast<LONG64>(sequence));
    return DirectFrameSharedSnapshotStatus::kCopied;
}

}  // namespace redclaw::helper

#endif
