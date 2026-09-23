#include "redclaw/render/audio_jitter_buffer.h"

#include <gtest/gtest.h>

namespace {

redclaw::render::AudioJitterFrame frame(std::uint32_t sequence, std::uint64_t timestamp) {
    redclaw::render::AudioJitterFrame value;
    value.sequence = sequence;
    value.timestamp_us = timestamp;
    value.payload = {static_cast<std::uint8_t>(sequence)};
    return value;
}

TEST(AudioJitterBuffer, ShortGapRequestsConcealmentThenPlaysTheNextPacket) {
    redclaw::render::AudioJitterBuffer jitter(1);
    jitter.push(frame(1, 0), 0);
    auto pull = jitter.pull(0);
    ASSERT_EQ(pull.action, redclaw::render::AudioJitterAction::kPacket);
    EXPECT_EQ(pull.frame.sequence, 1U);

    jitter.push(frame(2, 20000), 20000);
    pull = jitter.pull(20000);
    ASSERT_EQ(pull.action, redclaw::render::AudioJitterAction::kPacket);
    EXPECT_EQ(pull.frame.sequence, 2U);

    jitter.push(frame(4, 60000), 60000);
    pull = jitter.pull(60000);
    EXPECT_EQ(pull.action, redclaw::render::AudioJitterAction::kPlc);
    pull = jitter.pull(80000);
    ASSERT_EQ(pull.action, redclaw::render::AudioJitterAction::kPacket);
    EXPECT_EQ(pull.frame.sequence, 4U);
}

TEST(AudioJitterBuffer, LongGapWithoutPacketsBecomesSilence) {
    redclaw::render::AudioJitterBuffer jitter(1);
    jitter.push(frame(1, 0), 0);
    ASSERT_EQ(jitter.pull(0).action, redclaw::render::AudioJitterAction::kPacket);
    auto pull = jitter.pull(120000);
    EXPECT_EQ(pull.action, redclaw::render::AudioJitterAction::kSilence);

    jitter.push(frame(2, 200000), 200000);
    pull = jitter.pull(200000);
    ASSERT_EQ(pull.action, redclaw::render::AudioJitterAction::kPacket);
    EXPECT_EQ(pull.frame.sequence, 2U);
}

TEST(AudioJitterBuffer, LargeSequenceJumpKeepsTheLaterPacket) {
    redclaw::render::AudioJitterBuffer jitter(1);
    jitter.push(frame(1, 0), 0);
    ASSERT_EQ(jitter.pull(0).action, redclaw::render::AudioJitterAction::kPacket);
    jitter.push(frame(10, 200000), 20000);
    EXPECT_EQ(jitter.pull(20000).action, redclaw::render::AudioJitterAction::kSilence);
    const auto pull = jitter.pull(20000);
    ASSERT_EQ(pull.action, redclaw::render::AudioJitterAction::kPacket);
    EXPECT_EQ(pull.frame.sequence, 10U);
}

TEST(AudioJitterBuffer, DropsPacketsThatArriveAfterTheyWereDue) {
    redclaw::render::AudioJitterBuffer jitter(1);
    jitter.push(frame(1, 0), 0);
    ASSERT_EQ(jitter.pull(0).frame.sequence, 1U);
    jitter.push(frame(1, 0), 1000);
    jitter.push(frame(2, 20000), 20000);
    const auto pull = jitter.pull(20000);
    ASSERT_EQ(pull.action, redclaw::render::AudioJitterAction::kPacket);
    EXPECT_EQ(pull.frame.sequence, 2U);
}

}  // namespace
