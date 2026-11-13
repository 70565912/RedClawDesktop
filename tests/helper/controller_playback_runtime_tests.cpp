#include "runtime/controller_playback_runtime.h"
#include <gtest/gtest.h>

TEST(ControllerPlaybackRuntimeTest, KeyframeRequestMustMatchActivePlaybackAtSend) {
    using Runtime = redclaw::runtime::ControllerPlaybackRuntime;
    using Type = redclaw::protocol::StreamControlMessageTypeV1;

    const redclaw::helper::PlaybackStateSnapshot submitted{.session = 4, .revision = 9};
    redclaw::protocol::StreamControlMessageV1 request;
    request.type = Type::kKeyframeRequest;
    Runtime::stamp_playback_identity(request, submitted);
    EXPECT_TRUE(Runtime::matches_feedback(request, submitted));

    auto current = submitted;
    ++current.session;
    EXPECT_FALSE(Runtime::matches_feedback(request, current));
    current = submitted;
    ++current.revision;
    EXPECT_FALSE(Runtime::matches_feedback(request, current));
    current = submitted;
    current.paused = true;
    EXPECT_FALSE(Runtime::matches_feedback(request, current));

    Runtime::stamp_playback_identity(request, current);
    EXPECT_FALSE(Runtime::matches_feedback(request, current));
    request.type = Type::kReceiverNetworkStats;
    EXPECT_TRUE(Runtime::matches_feedback(request, current));
}
