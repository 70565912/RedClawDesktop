#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "redclaw/agent/coordination.h"

namespace {

std::filesystem::path test_directory(std::string_view suffix) {
    return std::filesystem::temp_directory_path()
        / ("redclaw-coordination-" + std::string(suffix) + "-"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
}

redclaw::protocol::AgentMessageEnvelopeV1 remote_message(
    redclaw::protocol::AgentMessageTypeV1 type,
    std::uint64_t message_id) {
    redclaw::protocol::AgentMessageEnvelopeV1 message;
    message.session_epoch = "remote-epoch-1";
    message.message_id = message_id;
    message.sent_at_ms = 1000 + message_id;
    message.type = type;
    return message;
}

redclaw::protocol::AgentMessageEnvelopeV1 task_request(
    std::string task_id,
    std::string request_id,
    std::uint64_t message_id = 1) {
    redclaw::protocol::AgentMessageEnvelopeV1 message;
    message.session_epoch = "local-client-1";
    message.message_id = message_id;
    message.sent_at_ms = 2000 + message_id;
    message.type = redclaw::protocol::AgentMessageTypeV1::kTaskCreate;
    message.task_id = std::move(task_id);
    message.request_id = std::move(request_id);
    message.provider = redclaw::protocol::AgentProviderKindV1::kCodex;
    message.project_id = "project-opaque";
    message.work_directory_mode =
        redclaw::protocol::AgentWorkDirectoryModeV1::kIsolatedWorktree;
    message.text = "inspect one file and run one focused test";
    return message;
}

void publish_catalogs(redclaw::agent::NormalAgentControlStateV1* state) {
    std::string error;
    auto provider = remote_message(
        redclaw::protocol::AgentMessageTypeV1::kCapabilities, 1);
    provider.provider = redclaw::protocol::AgentProviderKindV1::kCodex;
    provider.provider_readiness = redclaw::protocol::AgentProviderReadinessV1::kReady;
    provider.available = true;
    ASSERT_TRUE(state->observe_remote(provider, 1001, &error)) << error;

    auto project = remote_message(
        redclaw::protocol::AgentMessageTypeV1::kProjectCatalog, 2);
    project.project_id = "project-opaque";
    project.display_name = "Project";
    project.git_repository = true;
    ASSERT_TRUE(state->observe_remote(project, 1002, &error)) << error;
}

redclaw::agent::NormalAgentControlStateV1 initialized_state(
    const std::filesystem::path& journal_path) {
    redclaw::agent::NormalAgentControlStateV1 state({
        .journal_path = journal_path,
        .git_sha = "0123456789abcdef0123456789abcdef01234567",
        .executable_sha256 = std::string(64, 'a'),
    });
    std::string error;
    EXPECT_TRUE(state.initialize(&error)) << error;
    EXPECT_TRUE(state.transfer_authority(
        redclaw::agent::CoordinationAuthorityV1::kNone,
        redclaw::agent::CoordinationAuthorityV1::kNormalAgent,
        true, true, false, "normal_selected", 1000, &error)) << error;
    return state;
}

TEST(CoordinationJournalV1, AppendsAndReplaysOnlyBoundedAuditMetadata) {
    const auto directory = test_directory("journal");
    const auto journal_path = directory / "coordination-v1.jsonl";
    redclaw::agent::CoordinationJournalV1 journal(journal_path);
    std::string error;
    ASSERT_TRUE(journal.reload(&error)) << error;

    redclaw::agent::CoordinationJournalRecordV1 record;
    record.recorded_at_ms = 1000;
    record.record_type = redclaw::agent::CoordinationRecordTypeV1::kRequest;
    record.authority = redclaw::agent::CoordinationAuthorityV1::kNormalAgent;
    record.request_state = redclaw::agent::CoordinationRequestStateV1::kAccepted;
    record.request_id = "request-1";
    record.task_id = "task-1";
    record.request_digest_sha256 = std::string(64, 'b');
    record.authorization_state = "scoped_request";
    record.git_sha = "0123456789abcdef0123456789abcdef01234567";
    record.executable_sha256 = std::string(64, 'c');
    record.evidence_manifest_name = "coordination-evidence-v1.json";
    record.evidence_sha256 = std::string(64, 'd');
    ASSERT_TRUE(journal.append(record, &error)) << error;

    std::ifstream input(journal_path, std::ios::binary);
    const std::string persisted(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_EQ(persisted.find("inspect one file"), std::string::npos);
    EXPECT_EQ(persisted.find("C:\\"), std::string::npos);

    redclaw::agent::CoordinationJournalV1 replayed(journal_path);
    ASSERT_TRUE(replayed.reload(&error)) << error;
    ASSERT_EQ(replayed.records().size(), 1U);
    EXPECT_EQ(replayed.records().front().request_id, "request-1");
    EXPECT_EQ(
        replayed.records().front().evidence_manifest_name,
        "coordination-evidence-v1.json");
    EXPECT_EQ(replayed.records().front().evidence_sha256, std::string(64, 'd'));
    EXPECT_EQ(replayed.next_sequence(), 2U);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(CoordinationJournalV1, TenThousandRecordsOnlyParseTheAppendedTail) {
    const auto directory = test_directory("incremental");
    const auto path = directory / "journal.jsonl";
    redclaw::agent::CoordinationJournalV1 writer(path);
    std::string error;
    ASSERT_TRUE(writer.append({}, &error)) << error;
    // Seed history in one fixture write. Durability is tested by append(), not
    // by forcing ten thousand disk barriers before testing incremental parsing.
    std::ifstream seed(path, std::ios::binary);
    std::string template_line;
    std::getline(seed, template_line);
    seed.close();
    const auto sequence_offset = template_line.find("\"sequence\":1");
    ASSERT_NE(sequence_offset, std::string::npos);
    {
        std::ofstream history(path, std::ios::binary | std::ios::app);
        for (std::uint64_t sequence = 2; sequence <= 10000; ++sequence) {
            auto line = template_line;
            line.replace(sequence_offset, std::string("\"sequence\":1").size(),
                "\"sequence\":" + std::to_string(sequence));
            history << line << '\n';
        }
    }
    EXPECT_EQ(writer.parsed_record_count(), 0U);
    redclaw::agent::CoordinationJournalV1 reader(path);
    ASSERT_TRUE(reader.reload(&error)) << error;
    EXPECT_EQ(reader.parsed_record_count(), 10000U);
    for (int index = 0; index < 10; ++index) ASSERT_TRUE(reader.reload(&error)) << error;
    EXPECT_EQ(reader.parsed_record_count(), 10000U);
    ASSERT_TRUE(writer.append({}, &error)) << error;
    EXPECT_EQ(writer.parsed_record_count(), 9999U);
    ASSERT_TRUE(reader.reload(&error)) << error;
    EXPECT_EQ(reader.parsed_record_count(), 10001U);
    std::filesystem::resize_file(path, 0);
    EXPECT_FALSE(reader.append({}, &error));
    EXPECT_FALSE(reader.reload(&error));
    std::filesystem::remove_all(directory);
}

TEST(CoordinationJournalV1, RejectsReplacementAfterInitialization) {
    const auto directory = test_directory("replacement");
    const auto path = directory / "journal.jsonl";
    redclaw::agent::CoordinationJournalV1 writer(path);
    std::string error;
    ASSERT_TRUE(writer.append({}, &error)) << error;
    std::filesystem::rename(path, directory / "original.jsonl");
    redclaw::agent::CoordinationJournalV1 replacement(path);
    ASSERT_TRUE(replacement.append({}, &error)) << error;
    EXPECT_FALSE(writer.reload(&error));
    EXPECT_EQ(error, "coordination journal was replaced");
    std::filesystem::remove_all(directory);
}

TEST(NormalAgentControlStateV1, ApprovalDecisionMatchesPendingTaskAndIsSingleUse) {
    const auto directory = test_directory("approval");
    auto state = initialized_state(directory / "journal.jsonl");
    publish_catalogs(&state);
    std::string error;
    auto request = task_request("task-approval", "request-create");
    ASSERT_TRUE(state.prepare_outbound(request, {}, "scoped_request", 2001, &error)) << error;
    auto pending = remote_message(redclaw::protocol::AgentMessageTypeV1::kApprovalRequest, 3);
    pending.task_id = request.task_id;
    pending.request_id = "codex-approval-1";
    pending.event_sequence = 1;
    pending.task_state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval;
    pending.text = "Read registered project status";
    ASSERT_TRUE(state.observe_remote(pending, 2002, &error)) << error;
    auto decision = request;
    decision.type = redclaw::protocol::AgentMessageTypeV1::kApprovalDecision;
    decision.request_id = pending.request_id;
    decision.approval_decision = redclaw::protocol::AgentApprovalDecisionV1::kReject;
    decision.task_id = "wrong-task";
    EXPECT_FALSE(state.prepare_outbound(decision, {}, "explicit_decision", 2003, &error));
    decision.task_id = request.task_id;
    ASSERT_TRUE(state.prepare_outbound(decision, {}, "explicit_decision", 2004, &error)) << error;
    EXPECT_FALSE(state.prepare_outbound(decision, {}, "explicit_decision", 2005, &error));
    ++pending.message_id;
    ASSERT_TRUE(state.observe_remote(pending, 2006, &error));
    EXPECT_FALSE(state.prepare_outbound(decision, {}, "explicit_decision", 2007, &error));
    std::filesystem::remove_all(directory);
}

TEST(CoordinationJournalV1, SerializesConcurrentWritersByJournalPath) {
    const auto directory = test_directory("concurrent");
    const auto journal_path = directory / "coordination-v1.jsonl";
    std::atomic<int> failures = 0;
    const auto writer = [&](std::string prefix) {
        redclaw::agent::CoordinationJournalV1 journal(journal_path);
        for (int index = 0; index < 10; ++index) {
            redclaw::agent::CoordinationJournalRecordV1 record;
            record.recorded_at_ms = static_cast<std::uint64_t>(1000 + index);
            record.record_type = redclaw::agent::CoordinationRecordTypeV1::kRequest;
            record.authority = redclaw::agent::CoordinationAuthorityV1::kNormalAgent;
            record.request_state = redclaw::agent::CoordinationRequestStateV1::kAccepted;
            record.request_id = prefix + std::to_string(index);
            record.request_digest_sha256 = std::string(64, 'a');
            record.authorization_state = "scoped_request";
            record.git_sha = "0123456789abcdef0123456789abcdef01234567";
            record.executable_sha256 = std::string(64, 'b');
            std::string error;
            if (!journal.append(std::move(record), &error)) {
                ++failures;
            }
        }
    };

    std::thread first(writer, "first-");
    std::thread second(writer, "second-");
    first.join();
    second.join();
    EXPECT_EQ(failures.load(), 0);

    redclaw::agent::CoordinationJournalV1 replayed(journal_path);
    std::string error;
    ASSERT_TRUE(replayed.reload(&error)) << error;
    EXPECT_EQ(replayed.records().size(), 20U);
    EXPECT_EQ(replayed.next_sequence(), 21U);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(CoordinationJournalV1, RejectsPreexistingJournalWithoutOwnerOnlyPermissions) {
    const auto directory = test_directory("permissive");
    std::filesystem::create_directories(directory);
    const auto journal_path = directory / "coordination-v1.jsonl";
    {
        std::ofstream unprotected(journal_path, std::ios::binary);
        ASSERT_TRUE(unprotected.is_open());
    }

    redclaw::agent::CoordinationJournalV1 journal(journal_path);
    std::string error;
    EXPECT_FALSE(journal.reload(&error));
    EXPECT_FALSE(error.empty());

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(NormalAgentControlStateV1, ValidatesCatalogAndRejectsDuplicateTaskAndRequest) {
    const auto directory = test_directory("duplicates");
    auto state = initialized_state(directory / "coordination-v1.jsonl");
    publish_catalogs(&state);

    std::string error;
    const auto first = task_request("task-1", "request-1");
    ASSERT_TRUE(state.prepare_outbound(first, {}, "scoped_request", 2000, &error))
        << error;
    EXPECT_FALSE(state.prepare_outbound(first, {}, "scoped_request", 2001, &error));
    EXPECT_NE(error.find("duplicate coordination request"), std::string::npos);

    auto duplicate_task = task_request("task-1", "request-2", 2);
    EXPECT_FALSE(state.prepare_outbound(
        duplicate_task, {}, "scoped_request", 2002, &error));
    EXPECT_NE(error.find("duplicate coordination task"), std::string::npos);

    auto unknown_project = task_request("task-2", "request-3", 3);
    unknown_project.project_id = "project-not-published";
    EXPECT_FALSE(state.prepare_outbound(
        unknown_project, {}, "scoped_request", 2003, &error));
    EXPECT_NE(error.find("opaque project catalog"), std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(NormalAgentControlStateV1, RestartSynchronizesFromLastAcknowledgedEventBeforeReplacement) {
    const auto directory = test_directory("restart");
    const auto journal_path = directory / "coordination-v1.jsonl";
    {
        auto state = initialized_state(journal_path);
        publish_catalogs(&state);
        std::string error;
        ASSERT_TRUE(state.prepare_outbound(
            task_request("task-1", "request-1"), {}, "scoped_request", 2000, &error))
            << error;
        auto event = remote_message(redclaw::protocol::AgentMessageTypeV1::kEvent, 3);
        event.task_id = "task-1";
        event.request_id = "request-1";
        event.event_kind = "tool_complete";
        event.task_state = redclaw::protocol::AgentTaskStateV1::kRunning;
        for (std::uint64_t sequence = 1; sequence <= 7; ++sequence) {
            event.event_sequence = sequence;
            event.message_id = sequence + 2;
            ASSERT_TRUE(state.observe_remote(event, 2100 + sequence, &error)) << error;
        }
    }

    redclaw::agent::NormalAgentControlStateV1 resumed({
        .journal_path = journal_path,
        .git_sha = "0123456789abcdef0123456789abcdef01234567",
        .executable_sha256 = std::string(64, 'a'),
    });
    std::string error;
    ASSERT_TRUE(resumed.initialize(&error)) << error;
    EXPECT_EQ(resumed.authority(),
              redclaw::agent::CoordinationAuthorityV1::kNormalAgent);
    EXPECT_TRUE(resumed.sync_required());
    publish_catalogs(&resumed);

    auto replacement = task_request("task-2", "request-2", 2);
    EXPECT_FALSE(resumed.prepare_outbound(
        replacement, "request-1", "scoped_request", 2200, &error));
    EXPECT_NE(error.find("synchronization must complete"), std::string::npos);

    std::uint64_t next_message_id = 10;
    const auto sync = resumed.take_sync_requests(
        "local-resume", &next_message_id, 2300);
    ASSERT_EQ(sync.size(), 1U);
    EXPECT_EQ(sync.front().task_id, "task-1");
    EXPECT_EQ(sync.front().acknowledged_event_sequence, 7U);

    auto snapshot = remote_message(
        redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot, 3);
    snapshot.task_id = "task-1";
    snapshot.request_id = "request-1";
    snapshot.event_sequence = 7;
    snapshot.task_state = redclaw::protocol::AgentTaskStateV1::kRunning;
    ASSERT_TRUE(resumed.observe_remote(snapshot, 2400, &error)) << error;
    EXPECT_FALSE(resumed.sync_required());
    ASSERT_TRUE(resumed.prepare_outbound(
        replacement, "request-1", "scoped_request", 2500, &error)) << error;

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(NormalAgentControlStateV1, AuthorityTransferFailsClosedAgainstHealthyPrimary) {
    const auto directory = test_directory("authority");
    const auto journal_path = directory / "coordination-v1.jsonl";
    auto state = initialized_state(journal_path);
    publish_catalogs(&state);
    std::string error;
    EXPECT_FALSE(state.transfer_authority(
        redclaw::agent::CoordinationAuthorityV1::kNormalAgent,
        redclaw::agent::CoordinationAuthorityV1::kDebugBridge,
        true, true, true, "recover_debug", 2000, &error));
    EXPECT_EQ(state.authority(),
              redclaw::agent::CoordinationAuthorityV1::kNormalAgent);

    redclaw::agent::NormalAgentControlStateV1 stale_normal_client({
        .journal_path = journal_path,
        .git_sha = "0123456789abcdef0123456789abcdef01234567",
        .executable_sha256 = std::string(64, 'a'),
    });
    ASSERT_TRUE(stale_normal_client.initialize(&error)) << error;
    publish_catalogs(&stale_normal_client);

    ASSERT_TRUE(state.transfer_authority(
        redclaw::agent::CoordinationAuthorityV1::kNormalAgent,
        redclaw::agent::CoordinationAuthorityV1::kDebugBridge,
        true, false, true, "recover_debug", 2001, &error)) << error;

    const auto request_after_transfer = task_request(
        "task-after-transfer", "request-after-transfer");
    EXPECT_FALSE(stale_normal_client.prepare_outbound(
        request_after_transfer, {}, "scoped_request", 2002, &error));
    EXPECT_NE(error.find("does not own coordination authority"), std::string::npos);

    EXPECT_FALSE(state.transfer_authority(
        redclaw::agent::CoordinationAuthorityV1::kDebugBridge,
        redclaw::agent::CoordinationAuthorityV1::kOutOfBand,
        true, false, true, "recover_mailbox", 2003, &error));
    ASSERT_TRUE(state.transfer_authority(
        redclaw::agent::CoordinationAuthorityV1::kDebugBridge,
        redclaw::agent::CoordinationAuthorityV1::kOutOfBand,
        true, false, false, "recover_mailbox", 2004, &error)) << error;

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(NormalAgentControlStateV1, FailedPersistenceNeverAdvancesAckOrTaskState) {
    const auto directory = test_directory("failed-durability");
    const auto path = directory / "coordination-v1.jsonl";
    auto state = initialized_state(path);
    publish_catalogs(&state);
    std::string error;
    ASSERT_TRUE(state.prepare_outbound(task_request("task", "request"), {}, "scoped_request", 2000, &error));
    std::filesystem::resize_file(path, 0);
    auto terminal = remote_message(redclaw::protocol::AgentMessageTypeV1::kTaskComplete, 3);
    terminal.task_id = "task";
    terminal.request_id = "request";
    terminal.event_sequence = 1;
    terminal.task_state = redclaw::protocol::AgentTaskStateV1::kCompleted;
    EXPECT_FALSE(state.observe_remote(terminal, 2100, &error));
    EXPECT_EQ(state.acknowledged_event_sequence("task"), 0U);
    EXPECT_NE(state.task_snapshot("task")->task_state, redclaw::protocol::AgentTaskStateV1::kCompleted);
    std::filesystem::remove_all(directory);
}

TEST(NormalAgentControlStateV1, AckCannotSkipMissingEvents) {
    const auto directory = test_directory("ack-gap");
    auto state = initialized_state(directory / "coordination-v1.jsonl");
    publish_catalogs(&state);
    auto event = remote_message(redclaw::protocol::AgentMessageTypeV1::kEvent, 3);
    event.task_id = "task";
    event.event_sequence = 3;
    event.event_kind = "text_delta";
    std::string error;
    ASSERT_TRUE(state.observe_remote(event, 2100, &error)) << error;
    EXPECT_EQ(state.acknowledged_event_sequence("task"), 0U);
    EXPECT_TRUE(state.sync_required());
    event.message_id++;
    event.event_sequence = 1;
    ASSERT_TRUE(state.observe_remote(event, 2101, &error));
    EXPECT_EQ(state.acknowledged_event_sequence("task"), 1U);
    std::filesystem::remove_all(directory);
}

TEST(NormalAgentControlStateV1, ReplayIsIgnoredAndGapProducesOneSyncUntilTimeout) {
    const auto directory = test_directory("replay-pressure");
    auto state = initialized_state(directory / "journal.jsonl");
    publish_catalogs(&state);
    auto event = remote_message(redclaw::protocol::AgentMessageTypeV1::kEvent, 3);
    event.task_id = "task";
    event.event_sequence = 1;
    event.task_state = redclaw::protocol::AgentTaskStateV1::kRunning;
    bool apply = false;
    ASSERT_TRUE(state.observe_remote(event, 2100, nullptr, &apply));
    ASSERT_TRUE(apply);
    const auto records = state.journal().records().size();
    std::uint64_t next_id = 1;
    for (int i = 0; i < 120; ++i) {
        ++event.message_id;
        ASSERT_TRUE(state.observe_remote(event, 2200 + i, nullptr, &apply));
        EXPECT_FALSE(apply);
    }
    EXPECT_EQ(state.journal().records().size(), records);
    for (std::uint64_t seq = 3; seq < 103; ++seq) {
        ++event.message_id;
        event.event_sequence = seq;
        ASSERT_TRUE(state.observe_remote(event, 2400 + seq, nullptr, &apply));
        EXPECT_FALSE(apply);
        const auto sync = state.take_sync_requests("local", &next_id, 2400 + seq);
        EXPECT_EQ(sync.size(), seq == 3 ? 1U : 0U);
    }
    EXPECT_EQ(state.journal().records().size(), records);
    EXPECT_EQ(state.acknowledged_event_sequence("task"), 1U);
    EXPECT_EQ(state.take_sync_requests("local", &next_id, 7403).size(), 1U);
    event.type = redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot;
    event.gap = true;
    event.event_sequence = 101;
    ++event.message_id;
    ASSERT_TRUE(state.observe_remote(event, 7500, nullptr, &apply));
    EXPECT_TRUE(apply);
    EXPECT_EQ(state.acknowledged_event_sequence("task"), 101U);
    event.type = redclaw::protocol::AgentMessageTypeV1::kTaskComplete;
    event.gap = false;
    event.event_sequence = 102;
    event.task_state = redclaw::protocol::AgentTaskStateV1::kCompleted;
    ++event.message_id;
    ASSERT_TRUE(state.observe_remote(event, 7501, nullptr, &apply));
    event.type = redclaw::protocol::AgentMessageTypeV1::kEvent;
    event.event_sequence = 3;
    event.task_state = redclaw::protocol::AgentTaskStateV1::kRunning;
    ++event.message_id;
    ASSERT_TRUE(state.observe_remote(event, 7502, nullptr, &apply));
    EXPECT_FALSE(apply);
    EXPECT_EQ(state.task_snapshot("task")->task_state, redclaw::protocol::AgentTaskStateV1::kCompleted);
    std::filesystem::remove_all(directory);
}

TEST(NormalAgentControlStateV1, SnapshotDoesNotAcknowledgeLostTerminalPayload) {
    const auto directory = test_directory("snapshot-missing-terminal");
    auto state = initialized_state(directory / "journal.jsonl");
    auto snapshot = remote_message(redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot, 1);
    snapshot.task_id = "task";
    snapshot.task_state = redclaw::protocol::AgentTaskStateV1::kCompleted;
    snapshot.event_sequence = 1;
    bool apply = true;
    ASSERT_TRUE(state.observe_remote(snapshot, 2000, nullptr, &apply));
    EXPECT_FALSE(apply);
    EXPECT_TRUE(state.sync_required());
    EXPECT_EQ(state.acknowledged_event_sequence("task"), 0U);
    std::filesystem::remove_all(directory);
}

TEST(NormalAgentControlStateV1, ExplicitMissingHistoryAdvancesOnlyDurableLossWatermarkOnce) {
    const auto directory = test_directory("explicit-history-gap");
    auto state = initialized_state(directory / "journal.jsonl");
    auto gap = remote_message(redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot, 1);
    gap.task_id = "task";
    gap.event_sequence = 9;
    gap.task_state = redclaw::protocol::AgentTaskStateV1::kCompleted;
    gap.gap = true;
    gap.error_code = "history_unavailable";
    gap.event_kind = "history_gap";
    bool apply = false;
    ASSERT_TRUE(state.observe_remote(gap, 2000, nullptr, &apply));
    EXPECT_TRUE(apply);
    EXPECT_EQ(state.acknowledged_event_sequence("task"), 9U);
    const auto record_count = state.journal().records().size();
    ++gap.message_id;
    ASSERT_TRUE(state.observe_remote(gap, 2001, nullptr, &apply));
    EXPECT_FALSE(apply);
    EXPECT_EQ(state.journal().records().size(), record_count);
    EXPECT_FALSE(state.sync_required());
    std::filesystem::remove_all(directory);
}

TEST(NormalAgentControlStateV1, UnknownSyncRequiresCorrelationAndPreservesCompletedHistory) {
    const auto directory = test_directory("unknown-sync");
    auto state = initialized_state(directory / "journal.jsonl");
    auto event = remote_message(redclaw::protocol::AgentMessageTypeV1::kTaskComplete, 1);
    event.task_id = "task";
    event.event_sequence = 1;
    event.task_state = redclaw::protocol::AgentTaskStateV1::kCompleted;
    ASSERT_TRUE(state.observe_remote(event, 2000));
    state.request_sync("task");
    state.request_sync("other-task");
    auto reply = event;
    reply.message_id = 2;
    reply.type = redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot;
    reply.task_state = redclaw::protocol::AgentTaskStateV1::kFailed;
    reply.error_code = "task_not_found";
    reply.event_kind = "sync_unavailable";
    reply.complete = true;
    reply.request_id = "wrong-request";
    bool apply = true;
    ASSERT_TRUE(state.observe_remote(reply, 2001, nullptr, &apply));
    EXPECT_FALSE(apply);
    EXPECT_TRUE(state.sync_required());
    ++reply.message_id;
    reply.request_id = "sync-task";
    reply.event_sequence = 2; // Cannot use not-found to invent a new ACK.
    ASSERT_TRUE(state.observe_remote(reply, 2002, nullptr, &apply));
    EXPECT_FALSE(apply);
    EXPECT_EQ(state.acknowledged_event_sequence("task"), 1U);
    ++reply.message_id;
    reply.event_sequence = 1;
    ASSERT_TRUE(state.observe_remote(reply, 2003, nullptr, &apply));
    EXPECT_TRUE(apply);
    EXPECT_TRUE(state.sync_required()); // The other task is still unresolved.
    EXPECT_EQ(state.task_snapshot("task")->task_state, redclaw::protocol::AgentTaskStateV1::kCompleted);
    ++reply.message_id;
    reply.task_id = "other-task";
    reply.request_id = "sync-other-task";
    reply.event_sequence = 0;
    ASSERT_TRUE(state.observe_remote(reply, 2004));
    EXPECT_FALSE(state.sync_required());
    EXPECT_EQ(state.acknowledged_event_sequence("other-task"), 0U);
    redclaw::agent::NormalAgentControlStateV1 restored({.journal_path = directory / "journal.jsonl"});
    ASSERT_TRUE(restored.initialize());
    EXPECT_FALSE(restored.sync_required());
    EXPECT_EQ(restored.task_snapshot("task")->task_state, redclaw::protocol::AgentTaskStateV1::kCompleted);
    std::filesystem::remove_all(directory);
}

TEST(NormalAgentControlStateV1, PendingSnapshotRestoresOneApprovalAtDurableWatermark) {
    const auto directory = test_directory("snapshot-pending-approval");
    auto state = initialized_state(directory / "journal.jsonl");
    auto event = remote_message(redclaw::protocol::AgentMessageTypeV1::kEvent, 1);
    event.task_id = "task";
    event.event_sequence = 1;
    event.task_state = redclaw::protocol::AgentTaskStateV1::kRunning;
    ASSERT_TRUE(state.observe_remote(event, 2000));
    auto snapshot = event;
    snapshot.message_id = 2;
    snapshot.type = redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot;
    snapshot.task_state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval;
    snapshot.event_kind = "approval_pending";
    snapshot.request_id = "pending";
    bool apply = false;
    ASSERT_TRUE(state.observe_remote(snapshot, 2001, nullptr, &apply));
    EXPECT_TRUE(apply);
    auto decision = task_request("task", "pending");
    decision.type = redclaw::protocol::AgentMessageTypeV1::kApprovalDecision;
    decision.approval_decision = redclaw::protocol::AgentApprovalDecisionV1::kReject;
    ASSERT_TRUE(state.prepare_outbound(decision, {}, "explicit_decision", 2002));
    ++snapshot.message_id;
    ASSERT_TRUE(state.observe_remote(snapshot, 2003, nullptr, &apply));
    EXPECT_FALSE(apply);
    EXPECT_FALSE(state.prepare_outbound(decision, {}, "explicit_decision", 2004));
    std::filesystem::remove_all(directory);
}

TEST(NormalAgentControlStateV1, LocalAckAndStatusPreservePeerStateAcrossJournalReload) {
    using Type = redclaw::protocol::AgentMessageTypeV1;
    using State = redclaw::protocol::AgentTaskStateV1;
    for (const auto observed : {State::kAwaitingApproval, State::kCompleted,
                               State::kPaused, State::kFailed, State::kInterrupted}) {
        SCOPED_TRACE(static_cast<int>(observed));
        const auto directory = test_directory("local-query-does-not-change-peer");
        const auto journal = directory / "journal.jsonl";
        auto state = initialized_state(journal);
        publish_catalogs(&state);
        ASSERT_TRUE(state.prepare_outbound(task_request("task", "create"), {}, {}, 2000));
        EXPECT_EQ(state.task_snapshot("task")->task_state, State::kQueued);
        auto event = remote_message(observed == State::kAwaitingApproval
            ? Type::kApprovalRequest : Type::kEvent, 3);
        event.task_id = "task";
        event.request_id = "approval";
        event.event_sequence = 1;
        event.task_state = observed;
        ASSERT_TRUE(state.observe_remote(event, 2001));
        for (const auto type : {Type::kEventAck, Type::kTaskSyncRequest}) {
            auto query = task_request("task", type == Type::kEventAck ? "ack" : "status");
            query.type = type;
            query.text.clear();
            query.acknowledged_event_sequence = 1;
            ASSERT_TRUE(state.prepare_outbound(query, {}, {}, 2002));
            EXPECT_EQ(state.task_snapshot("task")->task_state, observed);
        }
        redclaw::agent::NormalAgentControlStateV1 restored({.journal_path = journal});
        ASSERT_TRUE(restored.initialize());
        ASSERT_TRUE(restored.task_snapshot("task"));
        EXPECT_EQ(restored.task_snapshot("task")->task_state, observed);
        EXPECT_EQ(restored.acknowledged_event_sequence("task"), 1U);
        std::filesystem::remove_all(directory);
    }
}

TEST(AgentLifecycleApprovalGateV1, RejectsMissingGatesAndStaleIdentityAndConsumesOnce) {
    redclaw::agent::AgentLifecycleApprovalGateV1 gate;
    redclaw::agent::AgentLifecycleRequestV1 request;
    request.request_id = "lifecycle-1";
    request.action = redclaw::agent::AgentLifecycleActionV1::kRestart;
    request.expected_git_sha = "0123456789abcdef0123456789abcdef01234567";
    request.candidate_executable_sha256 = std::string(64, 'a');
    request.target_path_sha256 = std::string(64, 'b');
    request.target_pid = 4242;
    request.build_gate_passed = true;
    request.focused_test_gate_passed = false;

    redclaw::agent::AgentLifecycleApprovalV1 approval;
    std::string error;
    EXPECT_FALSE(gate.authorize(
        request, "approval-1", 1000, 30000, true, &approval, &error));
    request.focused_test_gate_passed = true;
    EXPECT_FALSE(gate.authorize(
        request, "approval-1", 1000, 30000, false, &approval, &error));
    ASSERT_TRUE(gate.authorize(
        request, "approval-1", 1000, 30000, true, &approval, &error)) << error;

    redclaw::agent::AgentLifecycleObservedStateV1 observed;
    observed.now_ms = 2000;
    observed.git_sha = request.expected_git_sha;
    observed.candidate_executable_sha256 = request.candidate_executable_sha256;
    observed.target_path_sha256 = request.target_path_sha256;
    observed.target_pid = 9999;
    EXPECT_FALSE(gate.consume(request, observed, approval.approval_id, &error));
    EXPECT_NE(error.find("stale"), std::string::npos);
    observed.target_pid = request.target_pid;
    ASSERT_TRUE(gate.consume(request, observed, approval.approval_id, &error)) << error;
    EXPECT_FALSE(gate.consume(request, observed, approval.approval_id, &error));
}

}  // namespace
