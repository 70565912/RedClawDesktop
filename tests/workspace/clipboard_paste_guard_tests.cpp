#include "redclaw/workspace/clipboard_paste_guard.h"
#include <gtest/gtest.h>
namespace {
using namespace redclaw::workspace;
TEST(ClipboardPasteGuard, VerifiedCurrentTargetSubmitsOnceAndRejectsLateMessages) {
    ClipboardPasteGuard guard;
    ClipboardFocusToken focus{1, 2, 3, 4, 5, 6, 7};
    ASSERT_TRUE(guard.begin("epoch", "paste", focus, 11, true));
    EXPECT_FALSE(guard.begin("epoch", "second", focus, 11, true));
    EXPECT_FALSE(guard.verified("old-epoch", "paste"));
    EXPECT_EQ(guard.take("epoch", "old-paste", focus, 11, true), ClipboardPasteDecision::kStaleOperation);
    EXPECT_TRUE(guard.pending()); ASSERT_TRUE(guard.verified("epoch", "paste"));
    EXPECT_EQ(guard.take("epoch", "paste", focus, 11, true), ClipboardPasteDecision::kSubmitOnce);
    EXPECT_EQ(guard.take("epoch", "paste", focus, 11, true), ClipboardPasteDecision::kNoPendingPaste);
    EXPECT_FALSE(guard.verified("epoch", "paste"));
}
TEST(ClipboardPasteGuard, FocusReturnRevocationAndCancellationNeverSubmitPaste) {
    ClipboardPasteGuard guard;
    ClipboardFocusToken focus{1, 2, 3, 4, 5, 6, 7};
    ASSERT_TRUE(guard.begin("epoch", "focus", focus, 11, true)); ASSERT_TRUE(guard.verified("epoch", "focus"));
    auto returned = focus; ++returned.generation;
    EXPECT_EQ(guard.take("epoch", "focus", returned, 11, true), ClipboardPasteDecision::kFocusChanged);
    ASSERT_TRUE(guard.begin("epoch", "permission", focus, 11, true)); ASSERT_TRUE(guard.verified("epoch", "permission"));
    EXPECT_EQ(guard.take("epoch", "permission", focus, 12, true), ClipboardPasteDecision::kInputRevoked);
    ASSERT_TRUE(guard.begin("epoch", "partial", focus, 11, true));
    EXPECT_EQ(guard.take("epoch", "partial", focus, 11, true), ClipboardPasteDecision::kDataNotVerified);
    ASSERT_TRUE(guard.begin("epoch", "cancelled", focus, 11, true)); guard.cancel();
    EXPECT_EQ(guard.take("epoch", "cancelled", focus, 11, true), ClipboardPasteDecision::kNoPendingPaste);
    EXPECT_FALSE(guard.begin("epoch", "denied", focus, 11, false));
}
}
