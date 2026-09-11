#include "ui/gui_latency_probe.h"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <memory>

TEST(GuiLatencyProbe, FirstHeartbeatIsNotARunningHeartbeatGap) {
    redclaw::ui::GuiLatencyProbe probe;
    probe.heartbeat();
    EXPECT_EQ(probe.snapshot()["totals"].toObject()["heartbeat_gap"].toObject()["count"].toInteger(), 0);
}

TEST(GuiLatencyProbe, AckTimestampsRetainMicrosecondPrecision) {
    redclaw::ui::GuiLatencyProbe probe;
    probe.input_ack({1, 1000123, 1000333, 1249999, redclaw::ui::InputAckKind::kBatch});
    const auto receipt = probe.snapshot()["recent_input_acks"].toArray().first().toObject();
    EXPECT_EQ(receipt["sent_us"].toInteger(), 1000123);
    EXPECT_EQ(receipt["runtime_received_us"].toInteger(), 1000333);
    EXPECT_EQ(receipt["gui_consumed_us"].toInteger(), 1249999);
}

TEST(GuiLatencyProbe, HistogramCountsEverySampleAndBoundsPercentiles) {
    redclaw::ui::GuiDurationHistogram histogram;
    for (std::uint64_t i = 1; i <= 10000; ++i) histogram.add(i);
    EXPECT_EQ(histogram.count, 10000);
    EXPECT_EQ(histogram.total_us, 50005000);
    EXPECT_EQ(histogram.max_us, 10000);
    for (const unsigned p : {50U, 95U, 99U}) {
        EXPECT_GE(histogram.percentile(p), p * 100);
        EXPECT_LE(histogram.percentile(p), std::min<std::uint64_t>(10000, p * 125));
    }
}

TEST(GuiLatencyProbe, WindowMaxDoesNotInheritEarlierStallAndSnapshotsAreReadOnly) {
    redclaw::ui::GuiLatencyProbe probe;
    probe.record(redclaw::ui::GuiStage::kDispatchWait, 2000000);
    probe.close_window();
    probe.record(redclaw::ui::GuiStage::kDispatchWait, 1000);
    const auto window = probe.close_window();
    EXPECT_EQ(window["stages"].toObject()["dispatch_wait"].toObject()["max_us"].toInteger(), 1000);
    const auto first = probe.snapshot(), second = probe.snapshot();
    EXPECT_EQ(first["totals"], second["totals"]);
    EXPECT_EQ(first["last_window"], second["last_window"]);
    EXPECT_EQ(first["totals"].toObject()["dispatch_wait"].toObject()["max_us"].toInteger(), 2000000);
}

TEST(GuiLatencyProbe, InputStagesCorrelateSequencesAndRetainOnlyBoundedReceipts) {
    redclaw::ui::GuiLatencyProbe probe;
    for (std::uint64_t i = 1; i <= 40; ++i) probe.input_ack({i, 100000, 110000, 130000,
        i % 2 == 0 ? redclaw::ui::InputAckKind::kBatch : redclaw::ui::InputAckKind::kEmptySync});
    const auto snapshot = probe.snapshot();
    const auto receipts = snapshot["recent_input_acks"].toArray();
    EXPECT_EQ(receipts.size(), 32);
    EXPECT_EQ(receipts.first().toObject()["sequence"].toInteger(), 9);
    EXPECT_EQ(receipts.last().toObject()["sequence"].toInteger(), 40);
    const auto totals = snapshot["totals"].toObject();
    EXPECT_EQ(totals["input_batch_ack"].toObject()["count"].toInteger(), 20);
    EXPECT_EQ(totals["input_sync_runtime_ack"].toObject()["max_us"].toInteger(), 10000);
    EXPECT_EQ(totals["input_batch_local_delivery"].toObject()["max_us"].toInteger(), 20000);
}

TEST(GuiLatencyProbe, StartupAndRunningGapsHaveSeparateExactIntervals) {
    auto probe = std::make_unique<redclaw::ui::GuiLatencyProbe>();
    probe->arm_trace();
    probe->begin_event_loop(4000000);
    probe->event_dispatched(4000123);
    probe->heartbeat(4300000);
    probe->heartbeat(4310000);
    probe->heartbeat(4610000);
    const auto frozen = probe->snapshot_work();
    const auto totals = frozen()["totals"].toObject();
    EXPECT_EQ(totals["startup_first_dispatch"].toObject()["max_us"].toInteger(), 123);
    EXPECT_EQ(totals["startup_first_heartbeat"].toObject()["max_us"].toInteger(), 300000);
    EXPECT_EQ(totals["heartbeat_gap"].toObject()["count"].toInteger(), 2);
    EXPECT_EQ(totals["heartbeat_gap"].toObject()["max_us"].toInteger(), 300000);
    EXPECT_EQ(frozen()["startup"].toObject()["event_loop_entered_us"].toInteger(), 4000000);
    QTemporaryDir directory; QString error;
    const auto path = directory.filePath("startup.bin");
    ASSERT_TRUE(probe->export_trace(path, &error));
    QFile file(path); ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    file.seek(28); QDataStream input(&file); input.setByteOrder(QDataStream::LittleEndian);
    quint32 stage; quint64 begin, end, cpu;
    input >> stage >> begin >> end >> cpu;
    EXPECT_EQ(stage, static_cast<quint32>(redclaw::ui::GuiStage::kFirstEventDispatch));
    EXPECT_EQ(begin, 4000000); EXPECT_EQ(end, 4000123);
}

TEST(GuiLatencyProbe, CumulativeAcksPreserveKindsAndDuplicatesDoNotAdvance) {
    using namespace redclaw::ui;
    InputAckTracker tracker; GuiLatencyProbe probe;
    ASSERT_TRUE(tracker.remember(10, 1000001, InputAckKind::kBatch));
    ASSERT_TRUE(tracker.remember(11, 1000002, InputAckKind::kEmptySync));
    ASSERT_TRUE(tracker.remember(12, 1000003, InputAckKind::kStateSync));
    const auto consume = [&](const InputAckTiming& sample) { probe.input_ack(sample); };
    EXPECT_EQ(tracker.acknowledge(12, 1000333, 1250332, consume), 3);
    EXPECT_EQ(tracker.acknowledge(12, 2000000, 2400000, consume), 0);
    const auto totals = probe.snapshot()["totals"].toObject();
    for (const auto* name : {"input_batch_local_delivery", "input_sync_local_delivery", "input_state_sync_local_delivery"}) {
        EXPECT_EQ(totals[name].toObject()["count"].toInteger(), 1);
        EXPECT_EQ(totals[name].toObject()["max_us"].toInteger(), 249999);
    }
    EXPECT_EQ(totals["input_ack_local_delivery"].toObject()["count"].toInteger(), 3);
    EXPECT_TRUE(tracker.remember(13, 1000004, InputAckKind::kBatch));
    tracker.acknowledge(13, 0, 2500000, consume);
    EXPECT_EQ(probe.snapshot()["trace"].toObject()["invalid_ack_timings"].toInteger(), 1);
}

TEST(GuiLatencyProbe, AckTrackerRemainsBoundedAndClearsOnLifecycleEnd) {
    redclaw::ui::InputAckTracker tracker;
    for (std::uint64_t i = 1; i <= 256; ++i) ASSERT_TRUE(tracker.remember(i, i, redclaw::ui::InputAckKind::kBatch));
    EXPECT_FALSE(tracker.remember(257, 257, redclaw::ui::InputAckKind::kBatch));
    tracker.clear();
    EXPECT_EQ(tracker.acknowledge(256, 1000, 2000, [](const auto&) { FAIL() << "Expired pending ACK"; }), 0);
    EXPECT_TRUE(tracker.remember(258, 258, redclaw::ui::InputAckKind::kEmptySync));
}

TEST(GuiLatencyProbe, TracePreservesIntervalsAndCpuAndReportsOverflow) {
    auto probe = std::make_unique<redclaw::ui::GuiLatencyProbe>();
    probe->arm_trace();
    for (std::uint64_t i = 0; i < 262145; ++i)
        probe->record(redclaw::ui::GuiStage::kEvent, 20, 3, 100 + i, 120 + i);
    const auto trace = probe->snapshot()["trace"].toObject();
    EXPECT_EQ(trace["count"].toInteger(), 262144);
    EXPECT_EQ(trace["overflow"].toInteger(), 1);
    QTemporaryDir directory;
    QString error;
    const auto path = directory.filePath("trace.bin");
    ASSERT_TRUE(probe->export_trace(path, &error)) << error.toStdString();
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.read(8), QByteArray("RCDTRC01"));
    QDataStream input(&file); input.setByteOrder(QDataStream::LittleEndian);
    quint32 count, stage; quint64 overflow, start, begin, end, cpu;
    input >> count >> overflow >> start >> stage >> begin >> end >> cpu;
    EXPECT_EQ(count, 262144); EXPECT_EQ(overflow, 1);
    EXPECT_EQ(begin, 100); EXPECT_EQ(end, 120); EXPECT_EQ(cpu, 3);
    EXPECT_FALSE(probe->snapshot()["trace"].toObject()["active"].toBool());
}

TEST(GuiLatencyProbe, ReportsBoundedRecordingAndThreadClockOverhead) {
    constexpr std::uint64_t count = 100000;
    auto probe = std::make_unique<redclaw::ui::GuiLatencyProbe>();
    auto measure = [&](bool detailed) {
        if (detailed) probe->arm_trace();
        const auto begin = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto start = redclaw::ui::gui_monotonic_us();
            const auto cpu = redclaw::diag::current_thread_cpu_us();
            const auto end = redclaw::ui::gui_monotonic_us();
            const auto cpu_end = redclaw::diag::current_thread_cpu_us();
            probe->record(redclaw::ui::GuiStage::kEvent, end-start, cpu_end>=cpu ? cpu_end-cpu : 0, start, end);
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count()/count;
    };
    RecordProperty("bounded_histogram_and_two_thread_clock_reads_ns_per_event", static_cast<int>(measure(false)));
    RecordProperty("detailed_trace_and_two_thread_clock_reads_ns_per_event", static_cast<int>(measure(true)));
    EXPECT_EQ(probe->snapshot()["trace"].toObject()["overflow"].toInteger(), 0);
}
