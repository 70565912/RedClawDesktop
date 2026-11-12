#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "redclaw/helper/direct_frame_shared_memory.h"

#if defined(_WIN32)

namespace {

constexpr std::uint32_t kSlots = redclaw::helper::kDirectFrameSharedMemorySlotCount;

class DirectFrameReadPlanTest : public ::testing::Test {
protected:
    std::vector<std::uint64_t> mapping = std::vector<std::uint64_t>(
        (redclaw::helper::direct_frame_shared_mapping_size() + 7) / 8);
    redclaw::helper::DirectFrameSharedMemoryHeader* header =
        reinterpret_cast<redclaw::helper::DirectFrameSharedMemoryHeader*>(mapping.data());

    void SetUp() override { redclaw::helper::initialize_direct_frame_shared_memory(header); }
    void commit(std::uint64_t sequence, bool keyframe = false, std::uint64_t revision = 1,
                std::uint32_t format = redclaw::helper::kDirectFrameFormatH264) {
        auto* slot = redclaw::helper::direct_frame_shared_slot_header(mapping.data(),
            static_cast<std::uint32_t>((sequence - 1) % kSlots));
        slot->frame = {};
        slot->frame.width = 2;
        slot->frame.height = 2;
        slot->frame.format = format;
        slot->frame.flags = keyframe ? 1U : 0U;
        slot->frame.capture_region_revision = revision;
        slot->frame.payload_size = 1;
        *redclaw::helper::direct_frame_shared_slot_payload(slot) = static_cast<std::uint8_t>(sequence);
        redclaw::helper::direct_frame_atomic_store_i64(&slot->committed_sequence, sequence);
        redclaw::helper::direct_frame_atomic_store_i64(&header->latest_sequence, sequence);
    }
    auto select(std::uint64_t observed, bool recovering = false) {
        return redclaw::helper::plan_direct_frame_shared_read(mapping.data(), header, observed, recovering);
    }
};

TEST_F(DirectFrameReadPlanTest, BurstRetainsBothPredictiveDependenciesWithOneReusableCopy) {
    commit(101);
    commit(102);
    auto plan = select(100);
    EXPECT_EQ(plan.sequence, 101U);
    EXPECT_TRUE(plan.dependency_catchup);
    redclaw::helper::DirectFrameSharedSnapshot copy;
    ASSERT_TRUE(redclaw::helper::prepare_direct_frame_shared_snapshot(&copy));
    const std::uint8_t* buffer = nullptr;
    const auto capacity = copy.payload.capacity();
    for (std::uint64_t observed = 100; observed < 102; ++observed) {
        plan = select(observed);
        EXPECT_EQ(plan.sequence, observed + 1);
        ASSERT_EQ(redclaw::helper::copy_direct_frame_shared_sequence(
            mapping.data(), header, plan.sequence, &copy),
            redclaw::helper::DirectFrameSharedSnapshotStatus::kCopied);
        EXPECT_EQ(copy.payload[0], plan.sequence);
        if (buffer == nullptr) buffer = copy.payload.data();
        EXPECT_EQ(copy.payload.data(), buffer);
        EXPECT_EQ(copy.payload.capacity(), capacity);
        EXPECT_EQ(header->reader_active_sequence, 0);
    }
    EXPECT_EQ(select(102).sequence, 0U);
    EXPECT_EQ(header->slot_count, kSlots);
    EXPECT_EQ(header->version, 5U);
}

TEST_F(DirectFrameReadPlanTest, ReaderStallShorterThanTheRingCatchesUpWithoutAGap) {
    // A reader stalled for slot_count - 1 frames still finds every reference.
    for (std::uint64_t sequence = 101; sequence < 101 + kSlots; ++sequence) commit(sequence);
    redclaw::helper::DirectFrameSharedSnapshot copy;
    ASSERT_TRUE(redclaw::helper::prepare_direct_frame_shared_snapshot(&copy));
    for (std::uint64_t observed = 100; observed < 100 + kSlots; ++observed) {
        const auto plan = select(observed);
        ASSERT_EQ(plan.sequence, observed + 1);
        EXPECT_EQ(plan.dependency_catchup, plan.sequence != 100 + kSlots);
        ASSERT_EQ(redclaw::helper::copy_direct_frame_shared_sequence(
            mapping.data(), header, plan.sequence, &copy),
            redclaw::helper::DirectFrameSharedSnapshotStatus::kCopied);
        EXPECT_EQ(static_cast<std::uint64_t>(header->reader_sequence), plan.sequence);
    }
}

TEST_F(DirectFrameReadPlanTest, StallLongerThanTheRingIsAGapThatNeedsRecovery) {
    for (std::uint64_t sequence = 101; sequence <= 101 + kSlots; ++sequence) commit(sequence);
    const auto plan = select(100);
    EXPECT_EQ(plan.sequence, 101U + kSlots);
    EXPECT_FALSE(plan.dependency_catchup);
}

TEST_F(DirectFrameReadPlanTest, LatestIdrOrIndependentImageSkipsOldOutput) {
    commit(101);
    commit(102, true);
    EXPECT_EQ(select(100).sequence, 102U);
    EXPECT_TRUE(select(100).independent_skip);
    commit(103, false, 1, redclaw::helper::kDirectFrameFormatJpeg);
    EXPECT_EQ(select(100).sequence, 103U);
    EXPECT_TRUE(select(100).independent_skip);
}

TEST_F(DirectFrameReadPlanTest, RealOverwriteRequiresRecoveryButRetainedIdrCanRepair) {
    commit(102);
    commit(103);
    EXPECT_EQ(select(100).sequence, 103U);
    EXPECT_FALSE(select(100).dependency_catchup);
    commit(102, true);
    commit(103);
    EXPECT_EQ(select(100, true).sequence, 102U);
    EXPECT_TRUE(select(100, true).dependency_catchup);
    // A source/geometry boundary cannot borrow a reference from the old geometry.
    commit(103, false, 2);
    EXPECT_EQ(select(100, true).sequence, 103U);
}

TEST_F(DirectFrameReadPlanTest, PendingWriteAndPayloadRaceAreRevalidated) {
    commit(101);
    commit(102);
    const auto plan = select(100);
    commit(101 + kSlots); // Reuses 101's slot after planning, before payload copy.
    redclaw::helper::DirectFrameSharedSnapshot copy;
    ASSERT_TRUE(redclaw::helper::prepare_direct_frame_shared_snapshot(&copy));
    EXPECT_EQ(redclaw::helper::copy_direct_frame_shared_sequence(mapping.data(), header,
        plan.sequence, &copy), redclaw::helper::DirectFrameSharedSnapshotStatus::kMissingSequence);
    EXPECT_EQ(header->reader_active_sequence, 0);
    auto* newest = redclaw::helper::direct_frame_shared_slot_header(mapping.data(), 100 % kSlots);
    redclaw::helper::direct_frame_atomic_store_i64(&newest->committed_sequence, 0);
    EXPECT_EQ(select(100).sequence, 0U);
    EXPECT_EQ(header->reader_active_sequence, 0);
}

TEST(DirectFrameSharedSnapshotTest, ReleasesSharedSlotBeforeSlowDecodeWork) {
    const std::size_t mapping_words =
        (redclaw::helper::direct_frame_shared_mapping_size() + sizeof(std::uint64_t) - 1)
        / sizeof(std::uint64_t);
    std::vector<std::uint64_t> mapping(mapping_words);
    auto* mapping_view = mapping.data();
    auto* shared_header =
        reinterpret_cast<redclaw::helper::DirectFrameSharedMemoryHeader*>(mapping_view);
    redclaw::helper::initialize_direct_frame_shared_memory(shared_header);

    auto* slot = redclaw::helper::direct_frame_shared_slot_header(mapping_view, 0);
    std::memcpy(
        slot->frame.magic,
        redclaw::helper::kDirectFrameChannelMagic,
        sizeof(redclaw::helper::kDirectFrameChannelMagic));
    slot->frame.width = 2;
    slot->frame.height = 2;
    slot->frame.row_pitch = 8;
    slot->frame.format = redclaw::helper::kDirectFrameFormatBgra;
    slot->frame.frame_id = 17;
    slot->frame.payload_size = 16;
    slot->frame.capture_region_revision = 1;
    slot->frame.content_rect_width = 2;
    slot->frame.content_rect_height = 2;
    const std::uint8_t expected[16] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 9, 10, 11, 12, 13, 14, 15,
    };
    std::memcpy(
        redclaw::helper::direct_frame_shared_slot_payload(slot),
        expected,
        sizeof(expected));
    redclaw::helper::direct_frame_atomic_store_i64(&slot->committed_sequence, 1);
    redclaw::helper::direct_frame_atomic_store_i64(&shared_header->latest_sequence, 1);

    redclaw::helper::DirectFrameSharedSnapshot snapshot;
    ASSERT_TRUE(redclaw::helper::prepare_direct_frame_shared_snapshot(&snapshot));
    EXPECT_EQ(
        redclaw::helper::copy_direct_frame_shared_sequence(
            mapping_view,
            shared_header,
            1,
            &snapshot),
        redclaw::helper::DirectFrameSharedSnapshotStatus::kCopied);
    EXPECT_EQ(redclaw::helper::direct_frame_atomic_load_i64(
                  &shared_header->reader_active_sequence),
              0);
    EXPECT_EQ(redclaw::helper::direct_frame_atomic_load_i64(
                  &shared_header->reader_sequence),
              1);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(redclaw::helper::direct_frame_atomic_load_i64(
                  &shared_header->reader_active_sequence),
              0);
    EXPECT_EQ(snapshot.frame.frame_id, 17U);
    ASSERT_EQ(snapshot.payload.size(), sizeof(expected));
    EXPECT_EQ(0, std::memcmp(snapshot.payload.data(), expected, sizeof(expected)));
}

TEST(DirectFrameSharedSnapshotTest, RejectsUnpreparedBufferWithoutClaimingSlot) {
    const std::size_t mapping_words =
        (redclaw::helper::direct_frame_shared_mapping_size() + sizeof(std::uint64_t) - 1)
        / sizeof(std::uint64_t);
    std::vector<std::uint64_t> mapping(mapping_words);
    auto* shared_header = reinterpret_cast<redclaw::helper::DirectFrameSharedMemoryHeader*>(
        mapping.data());
    redclaw::helper::initialize_direct_frame_shared_memory(shared_header);
    redclaw::helper::DirectFrameSharedSnapshot snapshot;

    EXPECT_EQ(
        redclaw::helper::copy_direct_frame_shared_sequence(
            mapping.data(),
            shared_header,
            1,
            &snapshot),
        redclaw::helper::DirectFrameSharedSnapshotStatus::kBufferNotPrepared);
    EXPECT_EQ(redclaw::helper::direct_frame_atomic_load_i64(
                  &shared_header->reader_active_sequence),
              0);
}

}  // namespace

#endif

#if defined(_WIN32)
TEST(PlaybackDiscard, CapturesLatestWithoutInspectingInvalidOrHeldSlots) {
    using namespace redclaw::helper;
    DirectFrameSharedMemoryHeader header{};
    initialize_direct_frame_shared_memory(&header);
    direct_frame_atomic_store_i64(&header.latest_sequence, 100000);
    std::uint64_t observed = 7;
    EXPECT_EQ(discard_direct_frame_shared_latest(&header, observed), 99993U);
    EXPECT_EQ(observed, 100000U);
    EXPECT_EQ(direct_frame_atomic_load_i64(&header.reader_sequence), 100000);
    EXPECT_EQ(direct_frame_atomic_load_i64(&header.reader_active_sequence), 0);
    EXPECT_EQ(discard_direct_frame_shared_latest(&header, observed), 0U);
    publish_direct_frame_playback_state(&header, {.session=9, .revision=12, .paused=true,
        .reason=PlaybackPauseReason::kDisplayOff, .boundary=observed});
    const auto paused = read_direct_frame_playback_state(&header);
    EXPECT_TRUE(paused.paused); EXPECT_EQ(paused.session, 9U); EXPECT_EQ(paused.revision, 12U);
    EXPECT_EQ(paused.boundary, observed);
    direct_frame_atomic_increment_i64(&header.playback_guard);
    EXPECT_FALSE(read_direct_frame_playback_state(&header).valid);
    EXPECT_TRUE(read_direct_frame_playback_state(&header).paused);
}
#endif

TEST(PlaybackTransitionFence, DelayedDecodeAndQueuedRequestStayInvalidAfterRapidResumeOrReconnect) {
    using namespace redclaw::helper;
    PlaybackStateSnapshot submitted{.session=1, .revision=2};
    auto current=submitted;
    EXPECT_TRUE(playback_output_is_current(submitted, current));
    current.paused=true; ++current.revision;
    EXPECT_FALSE(playback_output_is_current(submitted, current));
    current.paused=false; ++current.revision;
    EXPECT_FALSE(playback_output_is_current(submitted, current));
    auto new_work=current;
    EXPECT_TRUE(playback_output_is_current(new_work, current));
    ++current.session;
    EXPECT_FALSE(playback_output_is_current(new_work, current));
}
