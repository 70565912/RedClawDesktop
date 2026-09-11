#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QLocalSocket>
#include <QThread>
#include <QUuid>
#include <QTimer>
#include <QElapsedTimer>
#include <algorithm>
#include <cwctype>
#include "redclaw/agent/coordination.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "redclaw/protocol/agent_protocol.h"
#include "ui/agent_control_server.h"

namespace {

QCoreApplication* test_application() {
    static int argc = 1;
    static char name[] = "redclaw-agent-control-tests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication application(argc, argv);
    return &application;
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

redclaw::protocol::AgentMessageEnvelopeV1 remote_message(
    redclaw::protocol::AgentMessageTypeV1 type,
    std::uint64_t message_id) {
    redclaw::protocol::AgentMessageEnvelopeV1 message;
    message.session_epoch = "remote-epoch";
    message.message_id = message_id;
    message.sent_at_ms = now_ms();
    message.type = type;
    return message;
}

redclaw::protocol::AgentMessageEnvelopeV1 task_message(
    std::uint64_t message_id,
    std::string request_id,
    std::string task_id = "task-1") {
    redclaw::protocol::AgentMessageEnvelopeV1 message;
    message.session_epoch = "local-client";
    message.message_id = message_id;
    message.sent_at_ms = now_ms();
    message.type = redclaw::protocol::AgentMessageTypeV1::kTaskCreate;
    message.task_id = std::move(task_id);
    message.request_id = std::move(request_id);
    message.provider = redclaw::protocol::AgentProviderKindV1::kCodex;
    message.project_id = "opaque-project";
    message.work_directory_mode =
        redclaw::protocol::AgentWorkDirectoryModeV1::kIsolatedWorktree;
    message.text = "run one focused test";
    return message;
}

redclaw::protocol::ParseResult<redclaw::protocol::AgentMessageEnvelopeV1>
send_request(
    const QString& pipe_name,
    const redclaw::protocol::AgentMessageEnvelopeV1& message) {
    QLocalSocket socket;
    socket.connectToServer(pipe_name);
    EXPECT_TRUE(socket.waitForConnected(2000)) << socket.errorString().toStdString();
    const std::string frame =
        redclaw::protocol::serialize_local_runtime_agent_frame_v1(message);
    QByteArray wire(frame.data(), static_cast<qsizetype>(frame.size()));
    wire.push_back('\n');
    EXPECT_EQ(socket.write(wire), wire.size());
    socket.flush();
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    while (socket.bytesAvailable() == 0
           && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        socket.waitForReadyRead(10);
        QThread::msleep(1);
    }
    const QByteArray response = socket.readLine().trimmed();
    EXPECT_FALSE(response.isEmpty());
    return redclaw::protocol::parse_local_runtime_agent_frame_v1(
        response.toStdString());
}

TEST(AgentControlServer, GuiInterruptHasRequestIdentityAndReachesDispatcher) {
    (void)test_application();
    const auto directory = std::filesystem::temp_directory_path()
        / ("redclaw-gui-interrupt-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
    std::vector<redclaw::protocol::AgentMessageEnvelopeV1> sent;
    redclaw::ui::AgentControlServer server({.listen_external = false,
        .journal_path = directory / "coordination-v1.jsonl"},
        [&](auto message, QString*) {sent.push_back(std::move(message)); return true;},
        [](const QString&) {});
    ASSERT_TRUE(server.start());
    auto provider = remote_message(redclaw::protocol::AgentMessageTypeV1::kCapabilities, 1);
    provider.provider = redclaw::protocol::AgentProviderKindV1::kCodex;
    provider.provider_readiness = redclaw::protocol::AgentProviderReadinessV1::kReady;
    provider.available = true;
    ASSERT_TRUE(server.observe_remote(provider));
    auto project = remote_message(redclaw::protocol::AgentMessageTypeV1::kProjectCatalog, 2);
    project.project_id = "opaque-project";
    project.git_repository = true;
    ASSERT_TRUE(server.observe_remote(project));
    ASSERT_TRUE(server.submit(task_message(1, "create-request")));
    auto wait_for_count = [&](std::size_t count) {
        QElapsedTimer timer; timer.start();
        while (sent.size() < count && timer.elapsed() < 3000) {
            QCoreApplication::processEvents(); QThread::msleep(1);
        }
        return sent.size() == count;
    };
    ASSERT_TRUE(wait_for_count(1));
    auto interrupt = redclaw::ui::make_gui_agent_command(
        redclaw::protocol::AgentMessageTypeV1::kTurnInterrupt);
    EXPECT_FALSE(interrupt.request_id.empty());
    EXPECT_NE(interrupt.request_id, redclaw::ui::make_gui_agent_command(interrupt.type).request_id);
    interrupt.task_id = "task-1";
    interrupt.session_epoch = "local-client";
    interrupt.message_id = 2;
    ASSERT_TRUE(server.submit(interrupt));
    ASSERT_TRUE(wait_for_count(2));
    EXPECT_EQ(sent.back().type, interrupt.type);
    EXPECT_EQ(sent.back().request_id, interrupt.request_id);
}

TEST(AgentControlServer, UsesTypedEnvelopeAndRejectsReplayAndDuplicateTask) {
    (void)test_application();
    const QString unique = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString pipe_name = "RedClawDesktop.AgentControl.Tests." + unique;
    const auto directory = std::filesystem::temp_directory_path()
        / ("redclaw-agent-control-server-" + unique.toStdString());
    std::vector<redclaw::protocol::AgentMessageEnvelopeV1> sent;
    redclaw::ui::AgentControlServer server({
        .pipe_name = pipe_name,
        .journal_path = directory / "coordination-v1.jsonl",
        .git_sha = "0123456789abcdef0123456789abcdef01234567",
        .executable_sha256 = std::string(64, 'a'),
    }, [&](auto message, QString*) {
        sent.push_back(std::move(message));
        return true;
    }, [](const QString&) {});
    QString error;
    ASSERT_TRUE(server.start(&error)) << error.toStdString();

    auto provider = remote_message(
        redclaw::protocol::AgentMessageTypeV1::kCapabilities, 1);
    provider.provider = redclaw::protocol::AgentProviderKindV1::kCodex;
    provider.provider_readiness = redclaw::protocol::AgentProviderReadinessV1::kReady;
    provider.available = true;
    server.observe_remote(provider);
    auto project = remote_message(
        redclaw::protocol::AgentMessageTypeV1::kProjectCatalog, 2);
    project.project_id = "opaque-project";
    project.git_repository = true;
    server.observe_remote(project);

    const auto request = task_message(1, "request-1");
    const auto accepted = send_request(pipe_name, request);
    ASSERT_TRUE(accepted.ok) << accepted.error;
    EXPECT_EQ(accepted.value.type,
              redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot);
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent.front().type,
              redclaw::protocol::AgentMessageTypeV1::kTaskCreate);
    EXPECT_EQ(sent.front().text, "run one focused test");

    auto terminal = remote_message(
        redclaw::protocol::AgentMessageTypeV1::kTaskComplete, 3);
    terminal.task_id = "task-1";
    terminal.request_id = "request-1";
    terminal.event_sequence = 1;
    terminal.task_state = redclaw::protocol::AgentTaskStateV1::kCompleted;
    server.observe_remote(terminal);

    redclaw::protocol::AgentMessageEnvelopeV1 evidence;
    evidence.session_epoch = "local-client";
    evidence.message_id = 2;
    evidence.sent_at_ms = now_ms();
    evidence.type = redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest;
    evidence.task_id = "task-1";
    evidence.request_id = "request-evidence-1";
    evidence.event_kind = "coordination_evidence";
    evidence.evidence_manifest_name = "agent-evidence-v1.json";
    evidence.evidence_sha256 = std::string(64, 'b');
    const auto evidence_response = send_request(pipe_name, evidence);
    ASSERT_TRUE(evidence_response.ok) << evidence_response.error;
    EXPECT_EQ(
        evidence_response.value.task_state,
        redclaw::protocol::AgentTaskStateV1::kCompleted);
    EXPECT_EQ(
        evidence_response.value.evidence_manifest_name,
        evidence.evidence_manifest_name);
    EXPECT_EQ(evidence_response.value.evidence_sha256, evidence.evidence_sha256);
    ASSERT_EQ(sent.size(), 3U); // Request, durable terminal ACK, evidence request.
    EXPECT_EQ(
        sent.back().type,
        redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest);

    const auto replay = send_request(pipe_name, request);
    ASSERT_TRUE(replay.ok) << replay.error;
    EXPECT_EQ(replay.value.type,
              redclaw::protocol::AgentMessageTypeV1::kTaskError);
    EXPECT_EQ(replay.value.error_code, "replay_or_reordering");
    EXPECT_EQ(sent.size(), 3U);

    const auto duplicate_task = send_request(
        pipe_name, task_message(3, "request-2"));
    ASSERT_TRUE(duplicate_task.ok) << duplicate_task.error;
    EXPECT_EQ(duplicate_task.value.type,
              redclaw::protocol::AgentMessageTypeV1::kTaskError);
    EXPECT_EQ(duplicate_task.value.error_code, "coordination_rejected");
    EXPECT_EQ(sent.size(), 3U);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(AgentControlServer, RepeatedStatusDoesNotPublishOrRewriteJournalButExplicitSyncDoes) {
    (void)test_application();
    using Type = redclaw::protocol::AgentMessageTypeV1;
    const QString unique = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString pipe_name = "RedClawDesktop.AgentControl.Tests." + unique;
    const auto directory = std::filesystem::temp_directory_path()
        / ("redclaw-agent-status-" + unique.toStdString());
    const auto journal = directory / "coordination-v1.jsonl";
    std::vector<redclaw::protocol::AgentMessageEnvelopeV1> sent;
    redclaw::ui::AgentControlServer server({.pipe_name = pipe_name, .journal_path = journal},
        [&](auto message, QString*) { sent.push_back(std::move(message)); return true; },
        [](const QString&) {});
    ASSERT_TRUE(server.start());
    auto terminal = remote_message(Type::kTaskComplete, 1);
    terminal.task_id = "task-status";
    terminal.request_id = "terminal";
    terminal.event_sequence = 1;
    terminal.task_state = redclaw::protocol::AgentTaskStateV1::kCompleted;
    ASSERT_TRUE(server.observe_remote(terminal));
    QElapsedTimer timer;
    timer.start();
    while (sent.empty() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    ASSERT_EQ(sent.size(), 1U); // Durable terminal ACK, not a status query.
    EXPECT_EQ(sent.front().type, Type::kEventAck);
    const auto original_bytes = std::filesystem::file_size(journal);
    auto query = task_message(1, "status-1", terminal.task_id);
    query.type = Type::kTaskSyncRequest;
    query.event_kind = "coordination_status";
    query.text.clear();
    for (std::uint64_t i = 1; i <= 20; ++i) {
        query.message_id = i;
        query.request_id = "status-" + std::to_string(i);
        const auto response = send_request(pipe_name, query);
        ASSERT_TRUE(response.ok) << response.error;
        EXPECT_EQ(response.value.type, Type::kTaskSnapshot);
        EXPECT_EQ(response.value.event_kind, "local_snapshot");
        EXPECT_EQ(response.value.task_state, terminal.task_state);
        EXPECT_EQ(response.value.acknowledged_event_sequence, 1U);
        EXPECT_TRUE(response.value.text.empty());
    }
    EXPECT_EQ(sent.size(), 1U);
    EXPECT_EQ(std::filesystem::file_size(journal), original_bytes);
    query.message_id = 21;
    query.request_id = "explicit-sync";
    query.event_kind.clear();
    query.acknowledged_event_sequence = 1;
    ASSERT_TRUE(send_request(pipe_name, query).ok);
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent.back().type, Type::kTaskSyncRequest);
    EXPECT_EQ(sent.back().acknowledged_event_sequence, 1U);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(AgentControlServer, RejectsNonControllerEnvelopeWithoutForwarding) {
    (void)test_application();
    const QString unique = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString pipe_name = "RedClawDesktop.AgentControl.Tests." + unique;
    const auto directory = std::filesystem::temp_directory_path()
        / ("redclaw-agent-control-server-" + unique.toStdString());
    int send_count = 0;
    redclaw::ui::AgentControlServer server({
        .pipe_name = pipe_name,
        .journal_path = directory / "coordination-v1.jsonl",
    }, [&](auto, QString*) {
        ++send_count;
        return true;
    }, [](const QString&) {});
    QString error;
    ASSERT_TRUE(server.start(&error)) << error.toStdString();

    auto invalid = remote_message(redclaw::protocol::AgentMessageTypeV1::kEvent, 1);
    invalid.task_id = "task-1";
    invalid.event_kind = "shell";
    const auto response = send_request(pipe_name, invalid);
    ASSERT_TRUE(response.ok) << response.error;
    EXPECT_EQ(response.value.type,
              redclaw::protocol::AgentMessageTypeV1::kTaskError);
    EXPECT_EQ(response.value.error_code, "operation_not_allowed");
    EXPECT_EQ(send_count, 0);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

#ifdef _WIN32
TEST(AgentControlServer, FiveHundredMsStorageWaitDoesNotBlockGuiTimer) {
    (void)test_application();
    const auto unique = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto directory = std::filesystem::temp_directory_path()
        / ("redclaw-agent-storage-delay-" + unique.toStdString());
    const auto path = directory / "coordination-v1.jsonl";
    {
        redclaw::ui::AgentControlServer server({.listen_external = false, .journal_path = path},
            [](auto, QString*) { return true; }, [](const QString&) {});
        ASSERT_TRUE(server.start());
        QElapsedTimer startup;
        startup.start();
        while (server.authority_name() != "normal_agent" && startup.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        ASSERT_EQ(server.authority_name(), "normal_agent");
        auto normalized = std::filesystem::absolute(path).lexically_normal().wstring();
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](wchar_t value) { return std::towlower(value); });
        const auto digest = redclaw::agent::sha256_hex(std::string(
            reinterpret_cast<const char*>(normalized.data()), normalized.size() * sizeof(wchar_t)));
        const auto name = L"Local\\RedClawDesktop.CoordinationJournal." + std::wstring(digest.begin(), digest.end());
        HANDLE gate = CreateMutexW(nullptr, FALSE, name.c_str());
        ASSERT_NE(gate, nullptr);
        ASSERT_EQ(WaitForSingleObject(gate, 1000), WAIT_OBJECT_0);
        auto event = remote_message(redclaw::protocol::AgentMessageTypeV1::kEvent, 1);
        event.task_id = "storage-delay";
        event.event_sequence = 1;
        event.event_kind = "text_delta";
        event.task_state = redclaw::protocol::AgentTaskStateV1::kRunning;
        server.observe_remote(event);
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 prior = 0, max_gap = 0;
        int ticks = 0;
        QTimer timer;
        timer.setInterval(5);
        QObject::connect(&timer, &QTimer::timeout, [&] {
            const auto now = elapsed.elapsed();
            max_gap = std::max(max_gap, now - prior);
            prior = now;
            ++ticks;
        });
        timer.start();
        QEventLoop loop;
        QTimer::singleShot(500, &loop, [&] { ReleaseMutex(gate); });
        QTimer::singleShot(800, &loop, &QEventLoop::quit);
        loop.exec();
        CloseHandle(gate);
        EXPECT_GT(ticks, 20);
        EXPECT_LE(max_gap, 250);
        EXPECT_FALSE(server.take_durable_messages().empty());
        RecordProperty("storage_delay_ms", 500);
        RecordProperty("gui_max_timer_gap_ms", max_gap);
    }
    std::filesystem::remove_all(directory);
}
#endif

}  // namespace
