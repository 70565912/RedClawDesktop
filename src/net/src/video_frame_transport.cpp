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
using transport_detail::kFragmentHeaderBytes;
constexpr std::array<std::uint8_t, 4> kFragmentMagic = {'R', 'C', 'V', 'F'};
constexpr std::uint8_t kFragmentVersion = 4;
constexpr std::uint8_t kKeyframeFlag = 0x1;
void append_u16_le(std::vector<std::uint8_t>* output, std::uint16_t value) {
    output->push_back(static_cast<std::uint8_t>(value & 0xFFu));
    output->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
}

void append_u32_le(std::vector<std::uint8_t>* output, std::uint32_t value) {
    for (std::uint32_t shift = 0; shift < 32; shift += 8) {
        output->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void append_u64_le(std::vector<std::uint8_t>* output, std::uint64_t value) {
    for (std::uint32_t shift = 0; shift < 64; shift += 8) {
        output->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

bool resolve_content_rect(
    const EncodedVideoFrameView& frame,
    std::uint32_t* x,
    std::uint32_t* y,
    std::uint32_t* width,
    std::uint32_t* height) {
    *x = frame.content_rect_x;
    *y = frame.content_rect_y;
    *width = frame.content_rect_width == 0 ? frame.width : frame.content_rect_width;
    *height = frame.content_rect_height == 0 ? frame.height : frame.content_rect_height;
    return *width != 0 && *height != 0
        && *x + *width <= frame.width
        && *y + *height <= frame.height;
}

template <typename Value>
bool read_unsigned_le(std::span<const std::uint8_t> input, std::size_t* cursor, Value* value) {
    if (*cursor + sizeof(Value) > input.size()) {
        return false;
    }

    Value parsed = 0;
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        parsed |= static_cast<Value>(input[*cursor + index]) << (index * 8u);
    }
    *cursor += sizeof(Value);
    *value = parsed;
    return true;
}
}  // namespace

bool build_encoded_video_fragment_plan(
    const EncodedVideoFrameView& frame,
    std::size_t max_packet_bytes,
    EncodedVideoFragmentPlan* plan,
    std::string* error_detail) {
    if (plan == nullptr) {
        assign_error("encoded video fragment plan must be non-null", error_detail);
        return false;
    }
    *plan = EncodedVideoFragmentPlan{};

    std::uint32_t content_x = 0;
    std::uint32_t content_y = 0;
    std::uint32_t content_width = 0;
    std::uint32_t content_height = 0;
    if (frame.codec == 0
        || frame.rate_revision == 0
        || frame.capture_region_revision == 0
        || frame.width == 0
        || frame.height == 0
        || !resolve_content_rect(
            frame, &content_x, &content_y, &content_width, &content_height)) {
        assign_error("encoded video frame metadata is incomplete", error_detail);
        return false;
    }
    if (frame.payload.empty()) {
        assign_error("encoded video frame payload is empty", error_detail);
        return false;
    }
    if (frame.payload.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        assign_error("encoded video frame payload exceeds supported size", error_detail);
        return false;
    }
    if (max_packet_bytes <= kFragmentHeaderBytes) {
        assign_error("video fragment packet budget is too small", error_detail);
        return false;
    }

    const std::size_t max_payload_bytes = max_packet_bytes - kFragmentHeaderBytes;
    const std::size_t fragment_count =
        (frame.payload.size() + max_payload_bytes - 1) / max_payload_bytes;
    if (fragment_count == 0
        || fragment_count > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())) {
        assign_error("encoded video frame requires too many fragments", error_detail);
        return false;
    }

    plan->max_fragment_payload_bytes = max_payload_bytes;
    plan->fragment_count = static_cast<std::uint16_t>(fragment_count);
    plan->wire_bytes = frame.payload.size() + fragment_count * kFragmentHeaderBytes;
    if (error_detail != nullptr) {
        error_detail->clear();
    }
    return true;
}

bool serialize_encoded_video_fragment(
    const EncodedVideoFrameView& frame,
    const EncodedVideoFragmentPlan& plan,
    std::uint16_t fragment_index,
    std::uint64_t transport_sequence,
    std::vector<std::uint8_t>* packet,
    std::string* error_detail) {
    if (packet == nullptr
        || plan.max_fragment_payload_bytes == 0
        || fragment_index >= plan.fragment_count
        || transport_sequence == 0) {
        assign_error("encoded video fragment plan or index is invalid", error_detail);
        return false;
    }

    const std::size_t payload_offset = static_cast<std::size_t>(fragment_index)
        * plan.max_fragment_payload_bytes;
    if (payload_offset >= frame.payload.size()) {
        assign_error("encoded video fragment payload offset is invalid", error_detail);
        return false;
    }
    const std::size_t payload_bytes = std::min(
        frame.payload.size() - payload_offset,
        plan.max_fragment_payload_bytes);

    packet->clear();
    packet->reserve(kFragmentHeaderBytes + payload_bytes);
    packet->insert(packet->end(), kFragmentMagic.begin(), kFragmentMagic.end());
    packet->push_back(kFragmentVersion);
    packet->push_back(frame.codec);
    packet->push_back(frame.keyframe ? kKeyframeFlag : 0);
    packet->push_back(0);
    append_u64_le(packet, frame.frame_id);
    append_u64_le(packet, frame.timestamp_ms);
    append_u32_le(packet, frame.width);
    append_u32_le(packet, frame.height);
    append_u64_le(packet, frame.rate_revision);
    append_u64_le(packet, frame.capture_region_revision);
    std::uint32_t content_x = 0;
    std::uint32_t content_y = 0;
    std::uint32_t content_width = 0;
    std::uint32_t content_height = 0;
    if (!resolve_content_rect(
            frame, &content_x, &content_y, &content_width, &content_height)) {
        assign_error("encoded video content rect is invalid", error_detail);
        return false;
    }
    append_u32_le(packet, content_x);
    append_u32_le(packet, content_y);
    append_u32_le(packet, content_width);
    append_u32_le(packet, content_height);
    append_u64_le(packet, transport_sequence);
    append_u16_le(packet, fragment_index);
    append_u16_le(packet, plan.fragment_count);
    append_u32_le(packet, static_cast<std::uint32_t>(frame.payload.size()));
    packet->insert(
        packet->end(),
        frame.payload.begin() + static_cast<std::ptrdiff_t>(payload_offset),
        frame.payload.begin() + static_cast<std::ptrdiff_t>(payload_offset + payload_bytes));

    if (error_detail != nullptr) {
        error_detail->clear();
    }
    return true;
}

bool parse_encoded_video_fragment(
    std::span<const std::uint8_t> packet,
    EncodedVideoFragmentView* fragment,
    std::string* error_detail) {
    if (fragment == nullptr) {
        assign_error("encoded video fragment output must be non-null", error_detail);
        return false;
    }
    *fragment = EncodedVideoFragmentView{};

    if (packet.size() < kFragmentHeaderBytes) {
        assign_error("encoded video fragment packet is too small", error_detail);
        return false;
    }
    if (!std::equal(kFragmentMagic.begin(), kFragmentMagic.end(), packet.begin())) {
        assign_error("unexpected encoded video fragment header", error_detail);
        return false;
    }

    std::size_t cursor = kFragmentMagic.size();
    const std::uint8_t version = packet[cursor++];
    fragment->codec = packet[cursor++];
    const std::uint8_t flags = packet[cursor++];
    ++cursor;
    fragment->keyframe = (flags & kKeyframeFlag) != 0;
    if (version != kFragmentVersion) {
        assign_error("unsupported encoded video fragment version", error_detail);
        return false;
    }

    if (!read_unsigned_le(packet, &cursor, &fragment->frame_id)
        || !read_unsigned_le(packet, &cursor, &fragment->timestamp_ms)
        || !read_unsigned_le(packet, &cursor, &fragment->width)
        || !read_unsigned_le(packet, &cursor, &fragment->height)
        || !read_unsigned_le(packet, &cursor, &fragment->rate_revision)
        || !read_unsigned_le(packet, &cursor, &fragment->capture_region_revision)
        || !read_unsigned_le(packet, &cursor, &fragment->content_rect_x)
        || !read_unsigned_le(packet, &cursor, &fragment->content_rect_y)
        || !read_unsigned_le(packet, &cursor, &fragment->content_rect_width)
        || !read_unsigned_le(packet, &cursor, &fragment->content_rect_height)
        || !read_unsigned_le(packet, &cursor, &fragment->transport_sequence)
        || !read_unsigned_le(packet, &cursor, &fragment->fragment_index)
        || !read_unsigned_le(packet, &cursor, &fragment->fragment_count)
        || !read_unsigned_le(packet, &cursor, &fragment->total_payload_bytes)) {
        assign_error("encoded video fragment packet is truncated", error_detail);
        return false;
    }

    fragment->payload = packet.subspan(cursor);
    if (fragment->codec == 0
        || fragment->rate_revision == 0
        || fragment->capture_region_revision == 0
        || fragment->transport_sequence == 0
        || fragment->width == 0
        || fragment->height == 0
        || fragment->content_rect_width == 0
        || fragment->content_rect_height == 0
        || fragment->content_rect_x + fragment->content_rect_width > fragment->width
        || fragment->content_rect_y + fragment->content_rect_height > fragment->height
        || fragment->fragment_count == 0
        || fragment->fragment_index >= fragment->fragment_count
        || fragment->total_payload_bytes == 0
        || fragment->payload.empty()
        || fragment->payload.size() > fragment->total_payload_bytes) {
        *fragment = EncodedVideoFragmentView{};
        assign_error("encoded video fragment metadata or payload is invalid", error_detail);
        return false;
    }

    if (error_detail != nullptr) {
        error_detail->clear();
    }
    return true;
}

EncodedVideoReassemblyResult EncodedVideoFrameReassembler::push(
    const EncodedVideoFragmentView& fragment,
    std::uint64_t now_ms) {
    EncodedVideoReassemblyResult result;

    if (fragment.fragment_index == 0) {
        result.abandoned_incomplete_frame = has_incomplete_frame();
        if (result.abandoned_incomplete_frame) {
            result.dropped_frame_id = frame_.frame_id;
            result.dropped_incomplete_frames = 1;
            result.dropped_keyframes = frame_.keyframe ? 1U : 0U;
            awaiting_keyframe_ = true;
        }
        clear_current_frame();
        discarded_frame_id_ = 0;
        if (awaiting_keyframe_ && !fragment.keyframe) {
            discarded_frame_id_ = fragment.frame_id;
            result.status = EncodedVideoReassemblyStatus::kDiscarded;
            result.dropped_dependency_frames = 1;
            result.keyframe_required = true;
            return result;
        }
        frame_.frame_id = fragment.frame_id;
        frame_.rate_revision = fragment.rate_revision;
        frame_.capture_region_revision = fragment.capture_region_revision;
        frame_.codec = fragment.codec;
        frame_.width = fragment.width;
        frame_.height = fragment.height;
        frame_.timestamp_ms = fragment.timestamp_ms;
        frame_.keyframe = fragment.keyframe;
        frame_.content_rect_x = fragment.content_rect_x;
        frame_.content_rect_y = fragment.content_rect_y;
        frame_.content_rect_width = fragment.content_rect_width;
        frame_.content_rect_height = fragment.content_rect_height;
        fragment_count_ = fragment.fragment_count;
        total_payload_bytes_ = fragment.total_payload_bytes;
        started_at_ms_ = now_ms;
        frame_.payload.reserve(fragment.total_payload_bytes);
        result.started_frame = true;
    } else if (discarded_frame_id_ == fragment.frame_id) {
        result.status = EncodedVideoReassemblyStatus::kDiscarded;
        result.keyframe_required = awaiting_keyframe_;
        return result;
    } else if (!has_incomplete_frame()
               || frame_.frame_id != fragment.frame_id
               || frame_.rate_revision != fragment.rate_revision
               || frame_.capture_region_revision != fragment.capture_region_revision
               || fragment_count_ != fragment.fragment_count
               || next_fragment_index_ != fragment.fragment_index
               || frame_.codec != fragment.codec
               || frame_.width != fragment.width
               || frame_.height != fragment.height
               || frame_.timestamp_ms != fragment.timestamp_ms
               || frame_.keyframe != fragment.keyframe
               || frame_.content_rect_x != fragment.content_rect_x
               || frame_.content_rect_y != fragment.content_rect_y
               || frame_.content_rect_width != fragment.content_rect_width
               || frame_.content_rect_height != fragment.content_rect_height
               || total_payload_bytes_ != fragment.total_payload_bytes) {
        const std::uint64_t discarded_frame_id = fragment.frame_id;
        result.abandoned_incomplete_frame = has_incomplete_frame();
        result.dropped_frame_id = result.abandoned_incomplete_frame
            ? frame_.frame_id
            : fragment.frame_id;
        result.dropped_incomplete_frames = 1;
        result.dropped_keyframes = result.abandoned_incomplete_frame
            ? (frame_.keyframe ? 1U : 0U)
            : (fragment.keyframe ? 1U : 0U);
        clear_current_frame();
        discarded_frame_id_ = discarded_frame_id;
        awaiting_keyframe_ = true;
        result.status = EncodedVideoReassemblyStatus::kDiscarded;
        result.keyframe_required = true;
        result.error = "encoded video fragment sequence mismatch";
        return result;
    }

    if (next_fragment_index_ != fragment.fragment_index
        || frame_.payload.size() + fragment.payload.size() > total_payload_bytes_) {
        const std::uint64_t discarded_frame_id = fragment.frame_id;
        result.abandoned_incomplete_frame = has_incomplete_frame();
        result.dropped_frame_id = result.abandoned_incomplete_frame
            ? frame_.frame_id
            : fragment.frame_id;
        result.dropped_incomplete_frames = 1;
        result.dropped_keyframes = frame_.keyframe ? 1U : 0U;
        clear_current_frame();
        discarded_frame_id_ = discarded_frame_id;
        awaiting_keyframe_ = true;
        result.status = EncodedVideoReassemblyStatus::kDiscarded;
        result.keyframe_required = true;
        result.error = "encoded video fragment payload overflow";
        return result;
    }

    frame_.payload.insert(
        frame_.payload.end(),
        fragment.payload.begin(),
        fragment.payload.end());
    last_progress_ms_ = now_ms;
    ++next_fragment_index_;

    if (next_fragment_index_ != fragment_count_) {
        return result;
    }
    if (frame_.payload.size() != total_payload_bytes_) {
        const std::uint64_t discarded_frame_id = fragment.frame_id;
        result.dropped_frame_id = frame_.frame_id;
        result.dropped_incomplete_frames = 1;
        result.dropped_keyframes = frame_.keyframe ? 1U : 0U;
        clear_current_frame();
        discarded_frame_id_ = discarded_frame_id;
        awaiting_keyframe_ = true;
        result.status = EncodedVideoReassemblyStatus::kDiscarded;
        result.keyframe_required = true;
        result.error = "encoded video frame reassembly size mismatch";
        return result;
    }

    result.status = EncodedVideoReassemblyStatus::kComplete;
    result.frame = std::move(frame_);
    if (result.frame.keyframe) {
        awaiting_keyframe_ = false;
    }
    clear_current_frame();
    discarded_frame_id_ = 0;
    return result;
}

EncodedVideoDropSummary EncodedVideoFrameReassembler::require_keyframe() {
    EncodedVideoDropSummary summary;
    if (has_incomplete_frame()) {
        summary.dropped_frame_id = frame_.frame_id;
        summary.dropped_incomplete_frames = 1;
        summary.dropped_keyframes = frame_.keyframe ? 1U : 0U;
    }
    clear_current_frame();
    discarded_frame_id_ = 0;
    awaiting_keyframe_ = true;
    return summary;
}

void EncodedVideoFrameReassembler::clear_current_frame() {
    frame_ = ReassembledEncodedVideoFrame{};
    next_fragment_index_ = 0;
    fragment_count_ = 0;
    total_payload_bytes_ = 0;
    started_at_ms_ = 0;
    last_progress_ms_ = 0;
}

void EncodedVideoFrameReassembler::reset() {
    clear_current_frame();
    discarded_frame_id_ = 0;
    awaiting_keyframe_ = true;
}

bool EncodedVideoFrameReassembler::has_incomplete_frame() const {
    return frame_.frame_id != 0
        && next_fragment_index_ < fragment_count_;
}

bool EncodedVideoFrameReassembler::requires_keyframe() const {
    return awaiting_keyframe_;
}

std::uint64_t EncodedVideoFrameReassembler::started_at_ms() const {
    return started_at_ms_;
}

std::uint64_t EncodedVideoFrameReassembler::incomplete_frame_id() const {
    return has_incomplete_frame() ? frame_.frame_id : 0;
}

bool EncodedVideoFrameReassembler::expired(std::uint64_t now_steady_ms) const {
    const auto limit = frame_.keyframe ? kEncodedKeyframeReassemblyTimeoutMs
                                      : kEncodedVideoReassemblyTimeoutMs;
    return has_incomplete_frame() && now_steady_ms >= started_at_ms_
        && now_steady_ms - started_at_ms_ >= limit;
}

bool EncodedVideoFrameReassembler::keyframe_progressing(
    std::uint64_t now_steady_ms, std::uint32_t srtt_ms) const {
    const auto idle_ms = std::clamp<std::uint64_t>(2ULL * srtt_ms + 500, 1500, 5000);
    return has_incomplete_frame() && frame_.keyframe && !expired(now_steady_ms)
        && now_steady_ms >= last_progress_ms_ && now_steady_ms - last_progress_ms_ < idle_ms;
}

}  // namespace redclaw::net
