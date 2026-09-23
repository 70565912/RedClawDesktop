#include "redclaw/protocol/audio_packet.h"
#include "redclaw/protocol/stream_control_protocol.h"

#include <gtest/gtest.h>

namespace {

using redclaw::protocol::AudioPacketV1;
using redclaw::protocol::StreamControlMessageTypeV1;
using redclaw::protocol::StreamControlMessageV1;
using redclaw::protocol::audio_stream_wanted;
using redclaw::protocol::kAudioStreamCapabilityVersion;
using redclaw::protocol::parse_audio_packet_v1;
using redclaw::protocol::parse_stream_control_message_v1;
using redclaw::protocol::serialize_audio_packet_v1;
using redclaw::protocol::serialize_stream_control_message_v1;

TEST(AudioStreamNegotiation, PlaybackRequestAndPeerVersionOpenTheChannel) {
    EXPECT_FALSE(audio_stream_wanted(0, true, false));
    EXPECT_FALSE(audio_stream_wanted(kAudioStreamCapabilityVersion, false, false));
    EXPECT_FALSE(audio_stream_wanted(kAudioStreamCapabilityVersion, true, true));
    EXPECT_TRUE(audio_stream_wanted(kAudioStreamCapabilityVersion, true, false));
}

TEST(AudioStreamControl, MissingAudioFieldsStayClosed) {
    StreamControlMessageV1 capabilities;
    capabilities.type = StreamControlMessageTypeV1::kCapabilities;
    capabilities.session_epoch = "epoch";
    capabilities.message_id = 1;
    capabilities.sent_at_ms = 10;
    capabilities.terminal_version = 2;
    auto parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(capabilities));
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.value.terminal_version, 2U);
    EXPECT_EQ(parsed.value.audio_version, 0U);
    EXPECT_FALSE(parsed.value.audio_playback_requested);

    capabilities.audio_version = kAudioStreamCapabilityVersion;
    capabilities.audio_playback_requested = true;
    parsed = parse_stream_control_message_v1(serialize_stream_control_message_v1(capabilities));
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.value.audio_version, kAudioStreamCapabilityVersion);
    EXPECT_TRUE(parsed.value.audio_playback_requested);
    EXPECT_EQ(parsed.value.terminal_version, 2U);
}

TEST(AudioPacketV1, RoundTripKeepsTheOpusPayload) {
    AudioPacketV1 packet;
    packet.sequence = 7;
    packet.timestamp_us = 20000;
    packet.payload = {1, 2, 3, 4, 9};
    const auto bytes = serialize_audio_packet_v1(packet);
    ASSERT_FALSE(bytes.empty());
    const auto parsed = parse_audio_packet_v1(bytes);
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.value.sequence, 7U);
    EXPECT_EQ(parsed.value.timestamp_us, 20000U);
    EXPECT_EQ(parsed.value.sample_rate, 48000U);
    EXPECT_EQ(parsed.value.frame_samples, 960U);
    EXPECT_EQ(parsed.value.payload, packet.payload);
}

TEST(AudioPacketV1, RejectsSilentOrUnsupportedPackets) {
    AudioPacketV1 packet;
    packet.sequence = 1;
    EXPECT_TRUE(serialize_audio_packet_v1(packet).empty());
    packet.payload = {1};
    packet.version = 2;
    EXPECT_TRUE(serialize_audio_packet_v1(packet).empty());
    std::vector<std::uint8_t> truncated(10, 0);
    EXPECT_FALSE(parse_audio_packet_v1(truncated).ok);
}

}  // namespace
