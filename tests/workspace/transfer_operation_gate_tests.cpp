#include <gtest/gtest.h>
#include "redclaw/workspace/transfer_operation_gate.h"
#include <algorithm>
#include <array>

namespace {
using namespace redclaw::workspace;
TEST(TransferOperationGate, CompletionRequiresCleanupPeerResultAndSentNotificationInEveryOrder) {
    std::array order{0, 1, 2};
    do {
        TransferOperationGate gate;
        ASSERT_TRUE(gate.begin("epoch", "operation"));
        EXPECT_EQ(gate.revision(), 1U);
        EXPECT_TRUE(gate.blocks_mutation());
        EXPECT_FALSE(gate.begin("epoch", "other-direction"));
        ASSERT_TRUE(gate.start_transfer("epoch", "operation"));
        for (unsigned step = 0; step < order.size(); ++step) {
            switch (order[step]) {
            case 0: EXPECT_TRUE(gate.local_finished("epoch", "operation")); break;
            case 1: EXPECT_TRUE(gate.peer_finished("epoch", "operation")); break;
            case 2: EXPECT_TRUE(gate.completion_sent("epoch", "operation")); break;
            }
            EXPECT_EQ(gate.blocks_mutation(), step != order.size() - 1);
        }
        EXPECT_EQ(gate.phase(), TransferGatePhase::kIdle);
        ASSERT_TRUE(gate.begin("epoch", "next-operation"));
        EXPECT_EQ(gate.revision(), 2U);
        EXPECT_FALSE(gate.local_finished("epoch", "operation"));
        EXPECT_FALSE(gate.peer_finished("epoch", "operation"));
        EXPECT_FALSE(gate.completion_sent("epoch", "operation"));
        EXPECT_TRUE(gate.blocks_mutation());
    } while (std::next_permutation(order.begin(), order.end()));
}
TEST(TransferOperationGate, DisconnectNeverUnlocksWhileTheDiskWorkerStillOwnsPartialFiles) {
    TransferOperationGate gate;
    EXPECT_FALSE(gate.begin("", "operation"));
    ASSERT_TRUE(gate.begin("old-epoch", "operation"));
    ASSERT_TRUE(gate.cancel("old-epoch", "operation"));
    EXPECT_FALSE(gate.start_transfer("old-epoch", "operation"));
    gate.disconnected();
    EXPECT_TRUE(gate.blocks_mutation());
    EXPECT_FALSE(gate.begin("new-epoch", "new-operation"));
    EXPECT_FALSE(gate.local_finished("new-epoch", "operation"));
    EXPECT_TRUE(gate.local_finished("old-epoch", "operation"));
    EXPECT_FALSE(gate.blocks_mutation());
    ASSERT_TRUE(gate.begin("new-epoch", "new-operation"));
    EXPECT_FALSE(gate.cancel("old-epoch", "operation"));
    EXPECT_TRUE(gate.blocks_mutation());
}
}
