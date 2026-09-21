#include "redclaw/net/video_frame_transport.h"
#include "redclaw/capture/capture_module.h"
#include "redclaw/render/render_module.h"
#include "redclaw/helper/direct_frame_shared_memory.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
namespace {
using namespace redclaw::net;
using namespace redclaw::helper;
using namespace std::chrono_literals;

TEST(AdaptiveSenderDecodeIntegration, OutputReservationRejectsBeforeAllocationAndRecoversWithIdr) {
    using namespace redclaw::capture;
    EncoderConfigProfile profile;
    EncoderExecutionSession encoder;
    std::string error;
    ASSERT_TRUE(build_low_latency_encoder_profile({.width = 320, .height = 180, .fps = 30}, &profile, &error));
    ASSERT_TRUE(encoder.start(profile, {.selected_backend = EncoderBackendType::kSoftware}, &error));
    CapturedFrame capture;
    capture.width = 320; capture.height = 180; capture.row_pitch = 1280; capture.bgra = true;
    capture.data.resize(1280 * 180, 127);
    EncodedFramePacket packet;
    packet.payload_limit_bytes = 1;
    ASSERT_FALSE(encoder.encode_bgra_frame(capture, 33, &packet, &error));
    EXPECT_EQ(encoder.diagnostics().last_failure, EncoderExecutionFailureCategory::kOutputResourceLimit);
    EXPECT_EQ(packet.payload.capacity(), 0U);
    packet.payload_limit_bytes = 8 * 1024 * 1024;
    ASSERT_TRUE(encoder.encode_bgra_frame(capture, 66, &packet, &error)) << error;
    ASSERT_TRUE(packet.keyframe);
    EXPECT_EQ(packet.timestamp_ms, 66U);
    EXPECT_LE(packet.payload.capacity(), packet.payload_limit_bytes);
    redclaw::render::FfmpegVideoFrameDecoder decoder;
    decoder.force_software_decode();
    redclaw::render::DecodedVideoFrame pixels;
    bool ready = false;
    ASSERT_TRUE(decoder.decode_frame_view(redclaw::render::EncodedVideoCodec::kH264, 320, 180, 66,
        true, packet.payload.data(), packet.payload.size(), &ready, &pixels, &error)) << error;
    EXPECT_TRUE(ready);
    EXPECT_FALSE(pixels.pixels.empty());
}

TEST(AdaptiveSenderDecodeIntegration, ReservedFifoReusesBuffersAndDecodesBothSharedSlots) {
    redclaw::capture::EncoderConfigProfile profile;
    std::string error;
    ASSERT_TRUE(redclaw::capture::build_low_latency_encoder_profile(
        {.width = 320, .height = 180, .fps = 30}, &profile, &error)) << error;
    redclaw::capture::EncoderExecutionSession encoder;
    ASSERT_TRUE(encoder.start(profile, {.selected_backend = redclaw::capture::EncoderBackendType::kSoftware}, &error)) << error;
    redclaw::capture::CapturedFrame capture;
    capture.width = 320; capture.height = 180; capture.row_pitch = 1280; capture.bgra = true;
    capture.data.resize(1280 * 180);
    for (std::size_t i = 0; i < capture.data.size(); ++i) capture.data[i] = static_cast<std::uint8_t>(i * 17);
    std::vector<std::uint64_t> mapping((direct_frame_shared_mapping_size() + 7) / 8);
    auto* header = reinterpret_cast<DirectFrameSharedMemoryHeader*>(mapping.data());
    initialize_direct_frame_shared_memory(header);
    DirectFrameSharedSnapshot snapshot;
    ASSERT_TRUE(prepare_direct_frame_shared_snapshot(&snapshot));
    redclaw::render::FfmpegVideoFrameDecoder decoder;
    decoder.force_software_decode();
    EncodedVideoFrameReassembler assembler;
    std::atomic<unsigned> completed{0}, decoded{0}, errors{0};
    std::uint64_t consumed = 0;
    std::mutex mutex;
    std::condition_variable cv;
    DesktopMediaSendPacer pacer;
    MediaCongestionDecision policy;
    policy.decision_revision = 1;
    policy.pacing_bitrate_kbps = 8000;
    policy.in_flight_limit_bytes = 128 * 1024;
    policy.feedback_horizon_us = 30000;
    ASSERT_TRUE(pacer.start([&](auto packet) {
        EncodedVideoFragmentView fragment;
        std::string detail;
        if (!parse_encoded_video_fragment(packet, &fragment, &detail)) {
            ++errors; return MediaPacerSendResult{.accepted = false};
        }
        if (fragment.fragment_index == 0 && fragment.frame_id % 7 == 0) std::this_thread::sleep_for(80ms);
        auto result = assembler.push(fragment, fragment.frame_id * 33);
        if (result.status == EncodedVideoReassemblyStatus::kComplete) {
            const auto& frame = result.frame;
            const auto sequence = frame.frame_id;
            auto* slot = direct_frame_shared_slot_header(mapping.data(), static_cast<std::uint32_t>((sequence - 1) % 2));
            slot->frame = {};
            std::memcpy(slot->frame.magic, kDirectFrameChannelMagic, sizeof(kDirectFrameChannelMagic));
            slot->frame.width = frame.width; slot->frame.height = frame.height;
            slot->frame.format = kDirectFrameFormatH264;
            slot->frame.flags = frame.keyframe ? 1U : 0U;
            slot->frame.payload_size = static_cast<std::uint32_t>(frame.payload.size());
            slot->frame.frame_id = sequence;
            std::memcpy(direct_frame_shared_slot_payload(slot), frame.payload.data(), frame.payload.size());
            direct_frame_atomic_store_i64(&slot->committed_sequence, sequence);
            direct_frame_atomic_store_i64(&header->latest_sequence, sequence);
            // Deliberately consume in pairs through the unmodified two-slot
            // read planner and snapshot API, then the real software decoder.
            if (sequence % 2 == 0) {
                for (unsigned j = 0; j < 2; ++j) {
                    const auto plan = plan_direct_frame_shared_read(mapping.data(), header, consumed, false);
                    if (plan.sequence != consumed + 1 || copy_direct_frame_shared_sequence(
                        mapping.data(), header, plan.sequence, &snapshot) != DirectFrameSharedSnapshotStatus::kCopied) {
                        ++errors; break;
                    }
                    bool ready = false;
                    redclaw::render::DecodedVideoFrame pixels;
                    if (!decoder.decode_frame_view(redclaw::render::EncodedVideoCodec::kH264,
                        snapshot.frame.width, snapshot.frame.height, plan.sequence * 33,
                        (snapshot.frame.flags & 1) != 0, snapshot.payload.data(), snapshot.payload.size(),
                        &ready, &pixels, &detail) || !ready || pixels.pixels.empty()) ++errors;
                    else ++decoded;
                    consumed = plan.sequence;
                }
            }
        }
        return MediaPacerSendResult{.accepted = true};
    }, [] { return MediaPacerTransportState{.open = true}; },
    [&](const auto&) { pacer.update_policy(policy, 10, 0); },
    [&](const auto& event) {
        if (event.type == MediaPacerFrameEventType::kSent) ++completed;
        if (event.type == MediaPacerFrameEventType::kDropped) ++errors;
        cv.notify_all();
    }, [&] { cv.notify_all(); }));
    pacer.update_policy(policy, 10, 0);
    bool reused = false;
    std::size_t max_pending = 0;
    for (std::uint64_t id = 1; id <= 24; ++id) {
        { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 3s, [&] { return pacer.can_accept_frame(); })); }
        auto reservation = pacer.reserve_encode();
        ASSERT_TRUE(reservation);
        reused |= reservation.payload.capacity() != 0;
        redclaw::capture::EncodedFramePacket encoded;
        encoded.payload = std::move(reservation.payload);
        encoded.payload_limit_bytes = reservation.payload_limit_bytes();
        capture.data[id % capture.data.size()] ^= 7;
        ASSERT_TRUE(encoder.encode_bgra_frame(capture, id * 33, &encoded, &error)) << error;
        PacedEncodedVideoFrame frame;
        frame.frame_id = id; frame.rate_revision = 1; frame.codec = 1;
        frame.width = 320; frame.height = 180; frame.target_fps = 30;
        frame.target_bitrate_kbps = profile.target_bitrate_kbps;
        frame.keyframe = encoded.keyframe; frame.payload = std::move(encoded.payload);
        ASSERT_TRUE(reservation.submit(std::move(frame)).ready());
        const auto telemetry = pacer.telemetry();
        max_pending = std::max(max_pending, telemetry.pending_depth);
        EXPECT_LE(telemetry.retained_bytes + telemetry.reserved_bytes, 24U * 1024U * 1024U);
        EXPECT_LE(telemetry.pending_depth, 32U);
    }
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 3s, [&] { return completed == 24; })); }
    pacer.stop();
    EXPECT_EQ(decoded, 24U);
    EXPECT_EQ(errors, 0U);
    EXPECT_TRUE(reused);
    EXPECT_EQ(pacer.telemetry().dependency_pending_drops, 0U);
    RecordProperty("maximum_pending_frames", static_cast<int>(max_pending));
}
}  // namespace
#endif
