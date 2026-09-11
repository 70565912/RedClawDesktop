#include "runtime/input_qa_receipts.h"
#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <iterator>

namespace {
class InputQaReceiptsTest : public testing::Test {
protected:
    std::filesystem::path directory = std::filesystem::temp_directory_path()
        / ("redclaw-input-stages-" + std::to_string(redclaw::diag::monotonic_time_us()));
    void SetUp() override { std::filesystem::create_directories(directory); }
    void TearDown() override { std::filesystem::remove_all(directory); }
    boost::json::object exported(std::uint64_t generation) {
        std::ifstream file(directory / ("input-injections-" + std::to_string(generation) + ".json"));
        const auto manifest = boost::json::parse(std::string(std::istreambuf_iterator<char>(file), {})).as_object();
        std::ifstream stages(directory / std::string(manifest.at("stage_file").as_string()));
        return boost::json::parse(std::string(std::istreambuf_iterator<char>(stages), {})).as_object();
    }
};
}

TEST_F(InputQaReceiptsTest, RecordsOnlyExplicitInputIntervalAndExportsOnShutdown) {
    using namespace redclaw::runtime;
    redclaw::protocol::StreamControlMessageV1 message;
    {
        InputQaReceipts recorder(true, directory);
        message.type = redclaw::protocol::StreamControlMessageTypeV1::kInputBatch;
        message.input_sequence = 5;
        recorder.command(InputQaStage::kLocalRead, message, 100);
        message.type = redclaw::protocol::StreamControlMessageTypeV1::kInputControlRequest;
        message.input_requested_active = true;
        recorder.command(InputQaStage::kLocalRead, message);
        message.type = redclaw::protocol::StreamControlMessageTypeV1::kInputBatch;
        recorder.command(InputQaStage::kLocalRead, message, 200);
        recorder.command(InputQaStage::kSendBegin, message, 300);
        ASSERT_TRUE(recorder.request_export(1));
        recorder.command(InputQaStage::kSendEnd, message, 400);
    }
    const auto result = exported(1);
    const auto& stages = result.at("stages").as_array();
    ASSERT_EQ(stages.size(), 2U);
    EXPECT_EQ(stages[0].as_object().at("begin_us").as_int64(), 200);
    EXPECT_EQ(stages[1].as_object().at("sequence").as_int64(), 5);
    EXPECT_EQ(result.at("stage_overflow").as_int64(), 0);
}

TEST_F(InputQaReceiptsTest, OverflowRemainsVisibleInBoundedExport) {
    using namespace redclaw::runtime;
    {
        InputQaReceipts recorder(true, directory);
        redclaw::protocol::StreamControlMessageV1 message;
        message.type = redclaw::protocol::StreamControlMessageTypeV1::kInputControlRequest;
        message.input_requested_active = true;
        recorder.command(InputQaStage::kLocalRead, message);
        message.type = redclaw::protocol::StreamControlMessageTypeV1::kInputBatch;
        message.input_sequence = 1;
        for (std::size_t i = 0; i <= InputQaReceipts::kStageCapacity; ++i)
            recorder.command(InputQaStage::kLocalRead, message, 100 + i);
        ASSERT_TRUE(recorder.request_export(2));
    }
    const auto result = exported(2);
    EXPECT_EQ(result.at("stages").as_array().size(), InputQaReceipts::kStageCapacity);
    EXPECT_EQ(result.at("stage_overflow").as_int64(), 1);
}
