#include "redclaw/protocol/video_codec_negotiation.h"

namespace redclaw::protocol {

std::optional<VideoCodecPreference> parse_video_codec_preference(std::string_view value) {
    if (value == "auto") return VideoCodecPreference::kAuto;
    if (value == "h264") return VideoCodecPreference::kH264;
    if (value == "hevc") return VideoCodecPreference::kHevc;
    return std::nullopt;
}

std::string_view to_string(VideoCodecPreference preference) {
    switch (preference) {
    case VideoCodecPreference::kAuto: return "auto";
    case VideoCodecPreference::kH264: return "h264";
    case VideoCodecPreference::kHevc: return "hevc";
    }
    return "auto";
}

std::string_view to_string(NegotiatedVideoCodec codec) {
    return codec == NegotiatedVideoCodec::kHevc ? "hevc" : "h264";
}

std::string describe_video_codec_mask(std::uint32_t mask) {
    std::string text;
    if ((mask & kVideoCodecMaskH264) != 0) text = "h264";
    if ((mask & kVideoCodecMaskHevc) != 0) text += text.empty() ? "hevc" : ",hevc";
    return text.empty() ? std::string("none") : text;
}

std::uint32_t video_codec_mask_bit(NegotiatedVideoCodec codec) noexcept {
    return codec == NegotiatedVideoCodec::kHevc ? kVideoCodecMaskHevc : kVideoCodecMaskH264;
}

PeerVideoDecodeCapabilities build_local_video_decode_capabilities(
    const LocalVideoDecodeSupport& support,
    VideoCodecPreference preference) noexcept {
    PeerVideoDecodeCapabilities report;
    report.version = kVideoCodecCapabilityVersion;
    if (support.h264_decoder) {
        report.decode_mask |= kVideoCodecMaskH264;
        if (support.h264_hardware) report.hardware_decode_mask |= kVideoCodecMaskH264;
    }
    // Software HEVC is not advertised. Without hardware decode on the display
    // adapter the Host must send H.264 (`peer_no_hevc_decoder`).
    if (preference != VideoCodecPreference::kH264 && support.hevc_decoder && support.hevc_hardware) {
        report.decode_mask |= kVideoCodecMaskHevc;
        report.hardware_decode_mask |= kVideoCodecMaskHevc;
    }
    return report;
}

bool peer_accepts_video_codec(
    const PeerVideoDecodeCapabilities& peer,
    NegotiatedVideoCodec codec) noexcept {
    if (codec == NegotiatedVideoCodec::kH264) return true;
    return peer.version >= 1 && (peer.decode_mask & kVideoCodecMaskHevc) != 0;
}

VideoCodecSelection select_video_codec(
    VideoCodecPreference preference,
    const PeerVideoDecodeCapabilities& peer,
    const HostVideoEncodeCapabilities& host) noexcept {
    const auto h264 = [](std::string_view reason) {
        return VideoCodecSelection{NegotiatedVideoCodec::kH264, true, reason};
    };
    if (preference == VideoCodecPreference::kH264) return h264("forced_h264_by_setting");
    if (host.hevc_encoder_open_failed) return h264("host_hevc_encoder_open_failed");
    if (peer.version == 0) return h264("peer_no_video_codec_report");
    if ((peer.decode_mask & kVideoCodecMaskHevc) == 0) return h264("peer_no_hevc_decoder");
    if (preference == VideoCodecPreference::kHevc) {
        if (host.hevc_hardware_encoder_available) {
            return {NegotiatedVideoCodec::kHevc, true, "forced_hevc_by_setting"};
        }
        if (host.hevc_software_encoder_available) {
            return {NegotiatedVideoCodec::kHevc, true, "forced_hevc_by_setting_software_encoder"};
        }
        return h264("host_no_hevc_encoder");
    }
    if ((peer.hardware_decode_mask & kVideoCodecMaskHevc) == 0) return h264("peer_hevc_decode_not_hardware");
    if (!host.hevc_hardware_encoder_available) return h264("host_no_hevc_hardware_encoder");
    return {NegotiatedVideoCodec::kHevc, false, "auto_hevc_hardware_on_both_sides"};
}

bool should_wait_for_peer_video_codec_report(
    bool peer_report_received,
    std::uint64_t required_channels_ready_at_ms,
    std::uint64_t now_ms,
    std::uint64_t grace_ms) noexcept {
    return !peer_report_received
        && required_channels_ready_at_ms != 0
        && now_ms >= required_channels_ready_at_ms
        && now_ms - required_channels_ready_at_ms < grace_ms;
}

HostVideoCodecNegotiatorV1::HostVideoCodecNegotiatorV1(VideoCodecPreference preference) noexcept
    : preference_(preference) {}

bool HostVideoCodecNegotiatorV1::observe_peer(
    std::uint32_t version,
    std::uint32_t decode_mask,
    std::uint32_t hardware_decode_mask,
    std::string_view change_reason) {
    PeerVideoDecodeCapabilities peer;
    if (version >= 1) {
        // Future codec bits are ignored; hardware support implies decode support.
        peer.version = version;
        peer.decode_mask = decode_mask & kKnownVideoCodecMask;
        peer.hardware_decode_mask = hardware_decode_mask & peer.decode_mask;
    }
    std::lock_guard lock(mutex_);
    const bool changed = !state_.peer_report_received
        || state_.peer.version != peer.version
        || state_.peer.decode_mask != peer.decode_mask
        || state_.peer.hardware_decode_mask != peer.hardware_decode_mask;
    state_.peer_report_received = true;
    state_.peer = peer;
    state_.peer_change_reason = sanitize_video_codec_change_reason(change_reason);
    if (changed) ++state_.revision;
    return changed;
}

void HostVideoCodecNegotiatorV1::reset_peer() {
    std::lock_guard lock(mutex_);
    state_.peer_report_received = false;
    state_.peer = {};
    state_.peer_change_reason.clear();
    ++state_.revision;
}

void HostVideoCodecNegotiatorV1::record_hevc_encoder_open_failure() {
    std::lock_guard lock(mutex_);
    if (!state_.hevc_encoder_open_failed) {
        state_.hevc_encoder_open_failed = true;
        ++state_.revision;
    }
}

HostVideoCodecNegotiatorV1::Snapshot HostVideoCodecNegotiatorV1::snapshot() const {
    std::lock_guard lock(mutex_);
    return state_;
}

VideoCodecSelection HostVideoCodecNegotiatorV1::select(HostVideoEncodeCapabilities host) const {
    const auto state = snapshot();
    host.hevc_encoder_open_failed = host.hevc_encoder_open_failed || state.hevc_encoder_open_failed;
    return select_video_codec(preference_, state.peer, host);
}

std::string sanitize_video_codec_change_reason(std::string_view reason) {
    if (reason.empty() || reason.size() > 64) return {};
    for (const char c : reason) {
        const bool token = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!token) return {};
    }
    return std::string(reason);
}

ControllerHevcDecodeOverloadMonitor::ControllerHevcDecodeOverloadMonitor(Config config) noexcept
    : config_(config) {
    if (config_.overrun_episodes == 0) config_.overrun_episodes = 1;
}

bool ControllerHevcDecodeOverloadMonitor::observe(const Observation& observation) {
    const std::uint64_t now = observation.now_ms;
    if (!loss_seen_ || observation.network_loss_total != last_loss_total_) {
        // The first observation only establishes the baseline.
        if (loss_seen_) {
            loss_changed_ = true;
            last_loss_change_ms_ = now;
        }
        loss_seen_ = true;
        last_loss_total_ = observation.network_loss_total;
    }
    if (triggered_) return false;
    if (observation.codec != NegotiatedVideoCodec::kHevc || observation.ring_slots == 0) {
        in_overrun_ = false;
        return false;
    }
    const bool overrun = observation.ring_backlog >= observation.ring_slots;
    if (!overrun) {
        in_overrun_ = false;
        return false;
    }
    if (in_overrun_) return false;
    in_overrun_ = true;
    const bool lossy = loss_changed_ && now >= last_loss_change_ms_
        && now - last_loss_change_ms_ < config_.clean_network_ms;
    if (lossy) {
        ++ignored_lossy_;
        return false;
    }
    while (!episodes_.empty() && now >= episodes_.front() && now - episodes_.front() > config_.window_ms) {
        episodes_.pop_front();
    }
    episodes_.push_back(now);
    ++total_counted_;
    if (episodes_.size() >= config_.overrun_episodes) {
        triggered_ = true;
        return true;
    }
    return false;
}

}  // namespace redclaw::protocol
