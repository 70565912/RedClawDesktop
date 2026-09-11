#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "redclaw/net/video_frame_transport.h"
#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/session/session_module.h"
#include "redclaw/service/dht_rendezvous.h"

namespace {

struct DeliveredFrameSummary {
    bool complete = false;
    bool keyframe = false;
    std::uint64_t frame_id = 0;
    std::uint32_t incomplete_drops = 0;
    std::uint32_t dependency_drops = 0;
};

class DeterministicMediaLink final {
public:
    using DropPredicate = std::function<bool(
        std::uint64_t frame_id,
        std::uint16_t fragment_index,
        std::uint16_t fragment_count)>;

    DeliveredFrameSummary send_frame(
        std::uint64_t frame_id,
        bool keyframe,
        std::size_t payload_bytes,
        const DropPredicate& should_drop) {
        std::vector<std::uint8_t> payload(payload_bytes);
        for (std::size_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<std::uint8_t>((frame_id * 17 + index * 13) & 0xffU);
        }

        redclaw::net::EncodedVideoFrameView frame;
        frame.frame_id = frame_id;
        frame.rate_revision = rate_revision_;
        frame.codec = 1;
        frame.width = 1280;
        frame.height = 720;
        frame.timestamp_ms = frame_id * 33;
        frame.keyframe = keyframe;
        frame.payload = payload;

        redclaw::net::EncodedVideoFragmentPlan plan;
        std::string error;
        DeliveredFrameSummary summary;
        if (!redclaw::net::build_encoded_video_fragment_plan(
                frame,
                16 * 1024,
                &plan,
                &error)) {
            ADD_FAILURE() << error;
            return summary;
        }

        redclaw::net::MediaTransportFeedbackBatch feedback;
        feedback.feedback_id = next_feedback_id_++;
        feedback.observed_rate_revision = rate_revision_;

        for (std::uint16_t fragment_index = 0;
             fragment_index < plan.fragment_count;
             ++fragment_index) {
            std::vector<std::uint8_t> packet;
            const std::uint64_t sequence = next_transport_sequence_++;
            if (!redclaw::net::serialize_encoded_video_fragment(
                    frame,
                    plan,
                    fragment_index,
                    sequence,
                    &packet,
                    &error)) {
                ADD_FAILURE() << error;
                return summary;
            }
            const std::uint64_t send_us = next_send_us_;
            next_send_us_ += 1000;
            EXPECT_TRUE(estimator_.record_sent({
                .transport_sequence = sequence,
                .frame_id = frame_id,
                .steady_send_us = send_us,
                .rate_revision = rate_revision_,
                .wire_bytes = packet.size(),
                .keyframe = keyframe,
            }));

            if (should_drop(frame_id, fragment_index, plan.fragment_count)) {
                ++dropped_fragments_;
                continue;
            }

            redclaw::net::EncodedVideoFragmentView fragment;
            if (!redclaw::net::parse_encoded_video_fragment(
                    packet,
                    &fragment,
                    &error)) {
                ADD_FAILURE() << error;
                return summary;
            }
            const std::uint64_t arrival_us = send_us + 40000;
            feedback.arrivals.push_back({sequence, arrival_us});
            const auto reassembly = reassembler_.push(fragment, arrival_us / 1000);
            summary.incomplete_drops += reassembly.dropped_incomplete_frames;
            summary.dependency_drops += reassembly.dropped_dependency_frames;
            if (reassembly.status == redclaw::net::EncodedVideoReassemblyStatus::kComplete) {
                summary.complete = true;
                summary.keyframe = reassembly.frame.keyframe;
                summary.frame_id = reassembly.frame.frame_id;
                latest_complete_frame_id_ = reassembly.frame.frame_id;
                if (reassembly.frame.keyframe) {
                    latest_complete_keyframe_id_ = reassembly.frame.frame_id;
                }
                ++completed_frames_;
            }
        }

        if (!feedback.arrivals.empty()) {
            const std::uint64_t host_feedback_us =
                feedback.arrivals.back().receiver_steady_us + 1000;
            EXPECT_TRUE(estimator_.apply_feedback(
                feedback,
                host_feedback_us,
                rate_revision_));
            latest_host_feedback_us_ = host_feedback_us;
        }
        return summary;
    }

    [[nodiscard]] redclaw::net::MediaTransportEstimate estimate() const {
        return estimator_.snapshot(latest_host_feedback_us_ + 1000, 80);
    }

    [[nodiscard]] std::uint64_t latest_complete_frame_id() const noexcept {
        return latest_complete_frame_id_;
    }

    [[nodiscard]] std::uint64_t latest_complete_keyframe_id() const noexcept {
        return latest_complete_keyframe_id_;
    }

    [[nodiscard]] std::uint64_t completed_frames() const noexcept {
        return completed_frames_;
    }

    [[nodiscard]] std::uint64_t dropped_fragments() const noexcept {
        return dropped_fragments_;
    }

    [[nodiscard]] bool requires_keyframe() const noexcept {
        return reassembler_.requires_keyframe();
    }

private:
    redclaw::net::EncodedVideoFrameReassembler reassembler_;
    redclaw::net::MediaTransportEstimator estimator_;
    std::uint64_t rate_revision_ = 1;
    std::uint64_t next_transport_sequence_ = 1;
    std::uint64_t next_feedback_id_ = 1;
    std::uint64_t next_send_us_ = 1000000;
    std::uint64_t latest_host_feedback_us_ = 1000000;
    std::uint64_t latest_complete_frame_id_ = 0;
    std::uint64_t latest_complete_keyframe_id_ = 0;
    std::uint64_t completed_frames_ = 0;
    std::uint64_t dropped_fragments_ = 0;
};

TEST(DesktopStreamSimulation, StaticSourceKeepsControlAliveWithoutFalseNetworkBackoff) {
    DeterministicMediaLink link;
    const auto first = link.send_frame(
        1,
        true,
        32000,
        [](std::uint64_t, std::uint16_t, std::uint16_t) { return false; });
    ASSERT_TRUE(first.complete);
    ASSERT_TRUE(first.keyframe);

    redclaw::session::DesktopSourceActivityTracker activity({.static_quiet_ms = 500});
    activity.reset(1, 1);
    activity.on_capture_poll_started(1);
    auto activity_update = activity.on_capture_frame(10, 1, 1);
    ASSERT_EQ(activity_update.snapshot.state,
        redclaw::session::DesktopSourceActivityState::kActive);
    for (std::uint64_t now_ms : {510ULL, 520ULL, 530ULL}) {
        activity.on_capture_poll_started(now_ms - 1);
        activity_update = activity.on_capture_timeout(now_ms);
    }
    ASSERT_EQ(activity_update.snapshot.state,
        redclaw::session::DesktopSourceActivityState::kStaticPending);
    activity_update = activity.on_reference_submitted(1, 1, 530, 1);
    ASSERT_EQ(activity_update.snapshot.reference_keyframe_id, 1U);

    redclaw::protocol::StreamControlMessageV1 source_state;
    source_state.type = redclaw::protocol::StreamControlMessageTypeV1::kSourceActivityState;
    source_state.session_epoch = "static-simulation-epoch";
    source_state.message_id = 1;
    source_state.sent_at_ms = 1000;
    source_state.source_activity_revision = activity_update.snapshot.revision;
    source_state.source_activity_state =
        redclaw::protocol::DesktopSourceActivityStateV1::kStaticPending;
    source_state.reference_frame_id = 1;
    source_state.reference_keyframe_id = 1;
    source_state.stream_geometry_revision = 1;
    source_state.rate_revision = 1;
    const auto parsed_source = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(source_state));
    ASSERT_TRUE(parsed_source.ok) << parsed_source.error;

    activity_update = activity.on_displayable_ack(
        activity_update.snapshot.revision, 1);
    ASSERT_EQ(activity_update.snapshot.state,
        redclaw::session::DesktopSourceActivityState::kStatic);

    redclaw::net::MediaCongestionController controller;
    redclaw::net::MediaCongestionSample sample;
    sample.encoder_target_bitrate_kbps = 8000;
    sample.transport = link.estimate();
    sample.transport.feedback_fresh = false;
    // Deliberately retain alarming old values to prove silence does not turn
    // stale transport history into a fresh weak-network decision.
    sample.transport.loss_per_mille = 800;
    sample.transport.queue_delay_ms = 900;

    std::uint64_t control_heartbeats = 0;
    redclaw::protocol::StreamControlEpochGuardV1 controller_guard;
    redclaw::protocol::StreamControlEpochGuardV1 host_guard;
    redclaw::protocol::StreamControlMessageV1 hello;
    hello.type = redclaw::protocol::StreamControlMessageTypeV1::kHello;
    hello.session_epoch = "static-simulation-epoch";
    hello.message_id = 2;
    hello.sent_at_ms = 1000;
    const auto parsed_hello = redclaw::protocol::parse_stream_control_message_v1(
        redclaw::protocol::serialize_stream_control_message_v1(hello));
    ASSERT_TRUE(parsed_hello.ok) << parsed_hello.error;
    std::string guard_error;
    ASSERT_TRUE(controller_guard.accept(parsed_hello.value, &guard_error)) << guard_error;
    ASSERT_TRUE(host_guard.accept(parsed_hello.value, &guard_error)) << guard_error;

    redclaw::net::MediaCongestionDecision decision;
    for (std::uint64_t now_ms = 1250; now_ms <= 60000; now_ms += 250) {
        ++control_heartbeats;
        redclaw::protocol::StreamControlMessageV1 ping;
        ping.type = redclaw::protocol::StreamControlMessageTypeV1::kPing;
        ping.session_epoch = "static-simulation-epoch";
        ping.message_id = control_heartbeats + 2;
        ping.sent_at_ms = now_ms;
        ping.log_cursor = control_heartbeats;
        const auto parsed_ping = redclaw::protocol::parse_stream_control_message_v1(
            redclaw::protocol::serialize_stream_control_message_v1(ping));
        ASSERT_TRUE(parsed_ping.ok) << parsed_ping.error;
        ASSERT_TRUE(controller_guard.accept(parsed_ping.value, &guard_error)) << guard_error;

        auto pong = ping;
        pong.type = redclaw::protocol::StreamControlMessageTypeV1::kPong;
        const auto parsed_pong = redclaw::protocol::parse_stream_control_message_v1(
            redclaw::protocol::serialize_stream_control_message_v1(pong));
        ASSERT_TRUE(parsed_pong.ok) << parsed_pong.error;
        ASSERT_TRUE(host_guard.accept(parsed_pong.value, &guard_error)) << guard_error;

        sample.now_steady_ms = now_ms;
        decision = controller.update(sample);
        EXPECT_FALSE(decision.backoff);
    }

    EXPECT_EQ(control_heartbeats, 236U);
    EXPECT_EQ(link.completed_frames(), 1U);
    EXPECT_EQ(decision.pacing_bitrate_kbps, 8000U);
    EXPECT_EQ(
        redclaw::net::resolve_available_encode_budget_gap(0, 0, 30),
        0U);

    activity_update = activity.on_viewport_change(60250, 2, 1);
    EXPECT_EQ(activity_update.snapshot.state,
        redclaw::session::DesktopSourceActivityState::kStaticPending);
    EXPECT_TRUE(activity_update.request_reference_frame);
}

TEST(DesktopStreamSimulation, RemoteLogRequestIsRestampedAndCompletesAfterControlReconnect) {
    redclaw::protocol::StreamControlEpochGuardV1 controller_guard;
    redclaw::protocol::StreamControlEpochGuardV1 host_guard;
    std::string guard_error;

    auto establish_epoch = [&](std::string_view epoch) {
        redclaw::protocol::StreamControlMessageV1 hello;
        hello.type = redclaw::protocol::StreamControlMessageTypeV1::kHello;
        hello.session_epoch = std::string(epoch);
        hello.message_id = 1;
        hello.sent_at_ms = 1000;
        ASSERT_TRUE(controller_guard.accept(hello, &guard_error)) << guard_error;
        ASSERT_TRUE(host_guard.accept(hello, &guard_error)) << guard_error;
    };

    establish_epoch("log-epoch-1");
    redclaw::protocol::StreamControlMessageV1 request;
    request.type = redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogRequest;
    request.session_epoch = "log-epoch-1";
    request.message_id = 2;
    request.sent_at_ms = 1001;
    request.request_id = "snapshot-across-reconnect";
    ASSERT_TRUE(host_guard.accept(request, &guard_error)) << guard_error;

    controller_guard.reset();
    host_guard.reset();
    establish_epoch("log-epoch-2");

    // Runtime retains the bounded request locally, then stamps the new epoch
    // and message id when the replacement control channel opens.
    EXPECT_FALSE(host_guard.accept(request, &guard_error));
    request.session_epoch = "log-epoch-2";
    request.message_id = 2;
    request.sent_at_ms = 2001;
    ASSERT_TRUE(host_guard.accept(request, &guard_error)) << guard_error;

    redclaw::protocol::StreamControlMessageV1 chunk;
    chunk.type = redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogChunk;
    chunk.session_epoch = "log-epoch-2";
    chunk.message_id = 2;
    chunk.sent_at_ms = 2002;
    chunk.request_id = request.request_id;
    chunk.chunk_index = 0;
    chunk.payload = "Runtime desktop stream stats role=host connected=true channel_open=true\n";
    ASSERT_TRUE(controller_guard.accept(chunk, &guard_error)) << guard_error;

    auto complete = chunk;
    complete.type = redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogComplete;
    complete.message_id = 3;
    complete.payload.clear();
    complete.complete = true;
    ASSERT_TRUE(controller_guard.accept(complete, &guard_error)) << guard_error;
    EXPECT_EQ(complete.request_id, "snapshot-across-reconnect");
    EXPECT_TRUE(complete.complete);
}

TEST(DesktopStreamSimulation, StickyViewportRequestAllowsBoundedReconnectReplay) {
    redclaw::protocol::StreamControlEpochGuardV1 host_guard;
    std::string guard_error;

    redclaw::protocol::StreamControlMessageV1 hello;
    hello.type = redclaw::protocol::StreamControlMessageTypeV1::kHello;
    hello.session_epoch = "viewport-epoch-2";
    hello.message_id = 1;
    hello.sent_at_ms = 2000;
    ASSERT_TRUE(host_guard.accept(hello, &guard_error)) << guard_error;

    redclaw::protocol::StreamControlMessageV1 viewport;
    viewport.type = redclaw::protocol::StreamControlMessageTypeV1::kViewportRequest;
    viewport.session_epoch = hello.session_epoch;
    viewport.viewport_width = 920;
    viewport.viewport_height = 373;
    viewport.log_cursor = 2;

    constexpr std::uint64_t kReplayAttempts = 3;
    for (std::uint64_t attempt = 0; attempt < kReplayAttempts; ++attempt) {
        viewport.message_id = 2 + attempt;
        viewport.sent_at_ms = 2001 + attempt;
        ASSERT_TRUE(host_guard.accept(viewport, &guard_error))
            << "attempt=" << attempt << " error=" << guard_error;
    }
}

TEST(DesktopStreamSimulation, ForcedActiveSourceContinuouslyTransfersCompleteFrames) {
    DeterministicMediaLink link;
    for (std::uint64_t frame_id = 1; frame_id <= 90; ++frame_id) {
        const auto delivered = link.send_frame(
            frame_id,
            frame_id == 1 || frame_id % 30 == 0,
            12000,
            [](std::uint64_t, std::uint16_t, std::uint16_t) { return false; });
        ASSERT_TRUE(delivered.complete) << "frame_id=" << frame_id;
    }

    const auto estimate = link.estimate();
    EXPECT_EQ(link.completed_frames(), 90U);
    EXPECT_EQ(link.latest_complete_frame_id(), 90U);
    EXPECT_EQ(link.latest_complete_keyframe_id(), 90U);
    EXPECT_EQ(estimate.in_flight_bytes, 0U);
    EXPECT_GT(estimate.acknowledged_bitrate_kbps, 0U);
}

TEST(DesktopStreamSimulation, SeededRandomFragmentLossRecoversAtLatestCompleteIdr) {
    DeterministicMediaLink link;
    std::mt19937 random(0x5EED1234U);
    std::bernoulli_distribution drop(0.12);
    std::uint64_t incomplete_or_dependency_drops = 0;

    for (std::uint64_t frame_id = 1; frame_id <= 48; ++frame_id) {
        const auto delivered = link.send_frame(
            frame_id,
            frame_id == 1 || frame_id % 8 == 0,
            48000,
            [&](std::uint64_t, std::uint16_t, std::uint16_t) {
                return drop(random);
            });
        incomplete_or_dependency_drops +=
            delivered.incomplete_drops + delivered.dependency_drops;
    }

    const auto recovery = link.send_frame(
        49,
        true,
        48000,
        [](std::uint64_t, std::uint16_t, std::uint16_t) { return false; });

    EXPECT_GT(link.dropped_fragments(), 0U);
    EXPECT_GT(incomplete_or_dependency_drops, 0U);
    EXPECT_TRUE(recovery.complete);
    EXPECT_TRUE(recovery.keyframe);
    EXPECT_EQ(link.latest_complete_frame_id(), 49U);
    EXPECT_EQ(link.latest_complete_keyframe_id(), 49U);
    EXPECT_FALSE(link.requires_keyframe());
}

TEST(DesktopStreamSimulation, WeakNetworkAndCapacityStepsRemainBoundedAndRecover) {
    redclaw::net::MediaPacingBudget budget;
    constexpr std::size_t kPacketBytes = 1200;
    std::uint64_t now_us = 1000000;
    budget.update_rate(800, now_us);

    std::uint64_t weak_bytes = 0;
    for (int tick = 0; tick < 500; ++tick) {
        while (budget.consume(kPacketBytes, now_us)) {
            weak_bytes += kPacketBytes;
        }
        now_us += 10000;
    }

    budget.update_rate(8000, now_us);
    std::uint64_t recovered_bytes = 0;
    for (int tick = 0; tick < 500; ++tick) {
        while (budget.consume(kPacketBytes, now_us)) {
            recovered_bytes += kPacketBytes;
        }
        now_us += 10000;
    }

    EXPECT_GT(weak_bytes, 0U);
    EXPECT_LT(weak_bytes, 700000U);
    EXPECT_GT(recovered_bytes, weak_bytes * 5U);

    redclaw::net::MediaCongestionController controller;
    redclaw::net::MediaCongestionSample sample;
    sample.encoder_target_bitrate_kbps = 12000;
    sample.rtt_fresh = true;
    sample.rtt_sample_id = 1;
    sample.transport.feedback_fresh = true;
    sample.transport.feedback_sample_id = 1;
    sample.now_steady_ms = 1000;
    auto decision = controller.update(sample);
    const std::uint32_t initial_rate = decision.pacing_bitrate_kbps;

    sample.transport.queue_delay_ms = 250;
    for (std::uint64_t now_ms : {1100ULL, 1200ULL, 1300ULL}) {
        sample.now_steady_ms = now_ms;
        ++sample.transport.feedback_sample_id;
        decision = controller.update(sample);
    }
    const std::uint32_t reduced_rate = decision.pacing_bitrate_kbps;
    EXPECT_LT(reduced_rate, initial_rate);
    EXPECT_GE(reduced_rate, 400U);

    sample.transport.queue_delay_ms = 0;
    sample.transport.loss_per_mille = 0;
    for (std::uint64_t now_ms : {2000ULL, 3000ULL, 4100ULL}) {
        sample.now_steady_ms = now_ms;
        ++sample.rtt_sample_id;
        ++sample.transport.feedback_sample_id;
        decision = controller.update(sample);
    }
    EXPECT_TRUE(decision.probe);
    EXPECT_GT(decision.pacing_bitrate_kbps, reduced_rate);
}

TEST(DesktopStreamSimulation, OpenChannelsWithoutFirstMediaFailBoundedlyThenRecover) {
    using redclaw::net::DesktopStreamEndpointRole;
    using redclaw::net::DesktopStreamHealth;

    auto host = redclaw::net::DesktopStreamHealthSample{};
    host.role = DesktopStreamEndpointRole::kHost;
    host.startup_tracking = true;
    host.startup_timeout_ms = 10000;
    host.direct_nat_path = true;
    host.captured_frames_total = 15;
    host.encode_submit_attempts_total = 1;

    host.startup_elapsed_ms = 9999;
    EXPECT_EQ(
        redclaw::net::classify_desktop_stream_health(host),
        DesktopStreamHealth::kIdle);

    host.startup_elapsed_ms = 10000;
    EXPECT_EQ(
        redclaw::net::classify_desktop_stream_health(host),
        DesktopStreamHealth::kEncoderStartupStalled);
    EXPECT_EQ(
        redclaw::net::desktop_stream_health_name(
            redclaw::net::classify_desktop_stream_health(host)),
        "encoder_startup_stalled");

    // Model the observable counters after an encoder fallback produces and
    // transmits the first keyframe. Transport-only success becomes media
    // success only here.
    host.encoded_frames_total = 1;
    host.transmitted_frames_total = 1;
    host.transmitted_frames = 1;
    EXPECT_EQ(
        redclaw::net::classify_desktop_stream_health(host),
        DesktopStreamHealth::kDirectNatPath);

    auto controller = redclaw::net::DesktopStreamHealthSample{};
    controller.role = DesktopStreamEndpointRole::kController;
    controller.startup_tracking = true;
    controller.startup_elapsed_ms = 10000;
    controller.startup_timeout_ms = 10000;
    controller.direct_nat_path = true;
    EXPECT_EQ(
        redclaw::net::classify_desktop_stream_health(controller),
        DesktopStreamHealth::kReceiveStartupStalled);

    controller.received_frames_total = 1;
    controller.reassembled_frames_total = 1;
    controller.delivered_frames_total = 1;
    controller.received_frames = 1;
    controller.direct_pipe_connected = true;
    controller.direct_pipe_writer_frames = 1;
    controller.direct_pipe_reader_frames = 1;
    EXPECT_EQ(
        redclaw::net::classify_desktop_stream_health(controller),
        DesktopStreamHealth::kDirectNatPath);
}

class FaultInjectingDhtStore final : public redclaw::service::IDhtRendezvousStore {
public:
    explicit FaultInjectingDhtStore(std::vector<std::uint64_t> publish_delays = {})
        : publish_delays_(std::move(publish_delays)) {}

    void drop_publish_attempts(std::set<std::uint64_t> attempts) {
        dropped_publish_attempts_ = std::move(attempts);
    }

    void miss_fetch_attempts(std::set<std::uint64_t> attempts) {
        missed_fetch_attempts_ = std::move(attempts);
    }

    void advance(std::uint64_t ticks = 1) {
        tick_ += ticks;
        promote_ready();
    }

    [[nodiscard]] redclaw::service::DhtPublishResult publish(
        const redclaw::service::DhtEncryptedRecord& record,
        std::string_view routing_token,
        std::string* error_detail) override {
        (void)routing_token;
        ++publish_attempts_;
        if (dropped_publish_attempts_.contains(publish_attempts_)) {
            if (error_detail != nullptr) {
                *error_detail = "simulated DHT publish loss";
            }
            return false;
        }
        const std::size_t delay_index = static_cast<std::size_t>(publish_attempts_ - 1);
        const std::uint64_t delay = delay_index < publish_delays_.size()
            ? publish_delays_[delay_index]
            : 0;
        pending_.push_back({record, tick_ + delay});
        promote_ready();
        return true;
    }

    [[nodiscard]] std::optional<redclaw::service::DhtEncryptedRecord> fetch(
        std::string_view topic_hex,
        std::string_view lane,
        std::uint64_t after_revision,
        std::string_view routing_token,
        std::string* error_detail) override {
        (void)routing_token;
        (void)error_detail;
        ++fetch_attempts_;
        if (missed_fetch_attempts_.contains(fetch_attempts_)) {
            return std::nullopt;
        }
        promote_ready();
        const auto found = visible_.find(make_key(topic_hex, lane));
        if (found == visible_.end() || found->second.revision <= after_revision) {
            return std::nullopt;
        }
        return found->second;
    }

    [[nodiscard]] std::uint64_t publish_attempts() const noexcept {
        return publish_attempts_;
    }

private:
    struct PendingRecord {
        redclaw::service::DhtEncryptedRecord record;
        std::uint64_t ready_tick = 0;
    };

    static std::string make_key(std::string_view topic_hex, std::string_view lane) {
        return std::string(topic_hex) + ":" + std::string(lane);
    }

    void promote_ready() {
        auto cursor = pending_.begin();
        while (cursor != pending_.end()) {
            if (cursor->ready_tick > tick_) {
                ++cursor;
                continue;
            }
            const std::string key = make_key(cursor->record.topic_hex, cursor->record.lane);
            const auto visible = visible_.find(key);
            if (visible == visible_.end()
                || visible->second.revision < cursor->record.revision) {
                visible_[key] = cursor->record;
            }
            cursor = pending_.erase(cursor);
        }
    }

    std::uint64_t tick_ = 0;
    std::uint64_t publish_attempts_ = 0;
    std::uint64_t fetch_attempts_ = 0;
    std::vector<std::uint64_t> publish_delays_;
    std::set<std::uint64_t> dropped_publish_attempts_;
    std::set<std::uint64_t> missed_fetch_attempts_;
    std::deque<PendingRecord> pending_;
    std::map<std::string, redclaw::service::DhtEncryptedRecord> visible_;
};

redclaw::service::DhtSignalSnapshot make_offer(
    std::uint64_t revision,
    std::uint64_t generation) {
    redclaw::service::DhtSignalSnapshot snapshot;
    snapshot.role = "host";
    snapshot.description_type = "offer";
    snapshot.description_sdp = "v=0\na=ice-ufrag:host\na=ice-pwd:host-password\n";
    snapshot.publisher_instance_id = "11111111111111111111111111111111";
    snapshot.generation = generation;
    snapshot.connection_request_tag = "request-tag-simulation";
    snapshot.candidates_complete = true;
    snapshot.candidate_lines = {
        "0\tcandidate:1 1 udp 2122260223 192.0.2.10 5000 typ host",
    };
    snapshot.revision = revision;
    return snapshot;
}

redclaw::service::DhtSignalSnapshot make_answer(
    const redclaw::service::DhtSignalSnapshot& offer,
    std::uint64_t revision) {
    redclaw::service::DhtSignalSnapshot snapshot;
    snapshot.role = "controller";
    snapshot.description_type = "answer";
    snapshot.description_sdp = "v=0\na=ice-ufrag:controller\na=ice-pwd:controller-password\n";
    snapshot.publisher_instance_id = "22222222222222222222222222222222";
    snapshot.generation = offer.generation;
    snapshot.connection_request_tag = offer.connection_request_tag;
    snapshot.answered_description_tag =
        redclaw::service::derive_dht_description_tag(offer.description_sdp);
    snapshot.candidates_complete = true;
    snapshot.candidate_lines = {
        "0\tcandidate:2 1 udp 2122260223 192.0.2.20 6000 typ host",
    };
    snapshot.revision = revision;
    return snapshot;
}

TEST(DesktopStreamSimulation, DhtOfferAnswerSurvivesPublishLossFetchMissAndDelay) {
    FaultInjectingDhtStore store({0, 0, 3, 2, 1});
    store.drop_publish_attempts({1, 2});
    store.miss_fetch_attempts({2});
    redclaw::service::DhtRendezvousClient host(store);
    redclaw::service::DhtRendezvousClient controller(store);
    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "SIMDHT01";
    config.pairing_secret = "simulation-pairing-secret";
    config.now_unix = 1000;
    config.ttl_seconds = redclaw::service::kDhtRecordTtlSeconds;
    std::string error;

    const auto offer = make_offer(1, 11);
    EXPECT_FALSE(host.publish_signal_snapshot(config, offer, &error));
    EXPECT_FALSE(host.publish_signal_snapshot(config, offer, &error));
    ASSERT_TRUE(host.publish_signal_snapshot(config, offer, &error)) << error;
    EXPECT_FALSE(controller.fetch_signal_snapshot(config, "host", 0, &error).has_value());
    store.advance(2);
    EXPECT_FALSE(controller.fetch_signal_snapshot(config, "host", 0, &error).has_value());
    store.advance();
    const auto fetched_offer = controller.fetch_signal_snapshot(config, "host", 0, &error);
    ASSERT_TRUE(fetched_offer.has_value()) << error;
    EXPECT_EQ(fetched_offer->generation, 11U);

    const auto answer = make_answer(*fetched_offer, 1);
    ASSERT_TRUE(controller.publish_signal_snapshot(config, answer, &error)) << error;
    EXPECT_FALSE(host.fetch_signal_snapshot(config, "controller", 0, &error).has_value());
    store.advance(2);
    const auto fetched_answer = host.fetch_signal_snapshot(config, "controller", 0, &error);
    ASSERT_TRUE(fetched_answer.has_value()) << error;
    EXPECT_EQ(fetched_answer->generation, offer.generation);
    EXPECT_EQ(
        fetched_answer->answered_description_tag,
        redclaw::service::derive_dht_description_tag(offer.description_sdp));

    auto acknowledgement = offer;
    acknowledgement.revision = 2;
    acknowledgement.acknowledged_answer_tag =
        redclaw::service::derive_dht_description_tag(answer.description_sdp);
    ASSERT_TRUE(host.publish_signal_snapshot(config, acknowledgement, &error)) << error;
    store.advance();
    const auto fetched_ack = controller.fetch_signal_snapshot(config, "host", 1, &error);
    ASSERT_TRUE(fetched_ack.has_value()) << error;
    EXPECT_EQ(fetched_ack->acknowledged_answer_tag, acknowledgement.acknowledged_answer_tag);
    EXPECT_EQ(store.publish_attempts(), 5U);
}

TEST(DesktopStreamSimulation, DhtOutOfOrderRecordsNeverRollBackRevisionOrGeneration) {
    FaultInjectingDhtStore store({5, 1});
    redclaw::service::DhtRendezvousClient host(store);
    redclaw::service::DhtRendezvousClient controller(store);
    redclaw::service::DhtRendezvousConfig config;
    config.session_code = "SIMDHT02";
    config.pairing_secret = "simulation-pairing-secret";
    config.now_unix = 2000;
    std::string error;

    ASSERT_TRUE(host.publish_signal_snapshot(config, make_offer(1, 20), &error)) << error;
    ASSERT_TRUE(host.publish_signal_snapshot(config, make_offer(2, 21), &error)) << error;
    store.advance();
    const auto latest = controller.fetch_signal_snapshot(config, "host", 0, &error);
    ASSERT_TRUE(latest.has_value()) << error;
    EXPECT_EQ(latest->revision, 2U);
    EXPECT_EQ(latest->generation, 21U);

    store.advance(10);
    EXPECT_FALSE(controller.fetch_signal_snapshot(config, "host", 2, &error).has_value());
    const auto still_latest = controller.fetch_signal_snapshot(config, "host", 0, &error);
    ASSERT_TRUE(still_latest.has_value()) << error;
    EXPECT_EQ(still_latest->revision, 2U);
    EXPECT_EQ(still_latest->generation, 21U);
}

}  // namespace
