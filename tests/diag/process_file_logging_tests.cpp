#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "redclaw/diag/diag_module.h"

TEST(ProcessFileLogging, RuntimeLogBurstDoesNotConsumeGuiHeartbeatBudget) {
    std::string line = "Runtime stats role=controller ";
    for (int i = 0; i < 160; ++i)
        line += "received=40 decoded=39 presented=38 decode_failures=0 queue_depth=0 ";
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(redclaw::diag::redact_log_text(line), line);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    RecordProperty("four_runtime_lines_ms", elapsed);
    EXPECT_LT(elapsed, 250);
}

TEST(ProcessFileLogging, RedactionPreservesGrammarAndBoundaries) {
    const std::pair<std::string, std::string> samples[]{
        {"TuRnS:user:password@relay/a", "TuRnS:[REDACTED]@relay/a"},
        {"turn:relay:3478", "turn:relay:3478"},
        {"turn:/path@host", "turn:/path@host"},
        {"--session-code Ab12Cd34", "--session-code [REDACTED:8]"},
        {"session code is ABCDE123", "session code is [REDACTED:8]"},
        {"machine_code:12345678 peer-code=abcdefgh", "machine_code:[REDACTED:8] peer-code=[REDACTED:8]"},
        {"sessioncode ABCDE123, code=isotopes", "sessioncode [REDACTED:8], code=[REDACTED:8]"},
        {"code ABCDE1234 code ABCDE123_", "code ABCDE1234 code ABCDE123_"},
        {"encoded=ABCDEFGH session_id=ABCDEFGH", "encoded=ABCDEFGH session_id=ABCDEFGH"},
        {"TOKEN = fixture, PASSWORD:other", "TOKEN = [REDACTED], PASSWORD:[REDACTED]"},
        {"secret=turn:user@relay auth=", "secret=[REDACTED] auth="},
        {"password=code ABCDE123", "password=[REDACTED] [REDACTED:8]"},
        {"token_count=10 secret_key=x", "token_count=10 secret_key=x"},
        {"private_key=x\nproof:y\tfingerprint=z", "private_key=[REDACTED]\nproof:[REDACTED]\tfingerprint=[REDACTED]"},
        {"token=[REDACTED] code=[REDACTED:8]", "token=[REDACTED] code=[REDACTED:8]"}
    };
    for (const auto& [input, expected] : samples) {
        EXPECT_EQ(redclaw::diag::redact_log_text(input), expected);
        EXPECT_EQ(redclaw::diag::redact_log_text(expected), expected);
    }
}

namespace {

std::filesystem::path make_test_directory(std::string_view suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("redclaw-diag-log-test-" + std::string(suffix) + '-' + std::to_string(stamp));
}

std::string read_all_logs(const std::filesystem::path& directory) {
    std::string result;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream input(entry.path(), std::ios::binary);
        result.append(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    return result;
}

TEST(ProcessFileLogger, RotatesAndRedactsSensitiveValues) {
    const auto directory = make_test_directory("rotate");
    redclaw::diag::ProcessFileLoggerConfig config;
    config.directory = directory;
    config.file_stem = "redclaw-test";
    config.role = "host";
    config.max_file_bytes = 220;
    config.max_file_count = 3;
    config.retention_days = 0;
    config.max_directory_bytes = 1024 * 1024;

    redclaw::diag::ProcessFileLogger logger;
    std::string error;
    ASSERT_TRUE(logger.start(config, &error)) << error;
    for (int index = 0; index < 20; ++index) {
        logger.write_line(
            "test",
            "session_code=RC7TST01 machine code AB12CD34 "
            "turn:debug-user:debug-password@relay.example.com:3478 token=abc123");
    }
    logger.stop();

    std::size_t file_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            ++file_count;
        }
    }
    EXPECT_LE(file_count, 3U);
    const std::string text = read_all_logs(directory);
    EXPECT_EQ(text.find("RC7TST01"), std::string::npos);
    EXPECT_EQ(text.find("AB12CD34"), std::string::npos);
    EXPECT_EQ(text.find("debug-password"), std::string::npos);
    EXPECT_EQ(text.find("abc123"), std::string::npos);
    EXPECT_NE(text.find("[REDACTED"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST(ProcessFileLogger, SupportsConcurrentWriters) {
    const auto directory = make_test_directory("concurrent");
    redclaw::diag::ProcessFileLoggerConfig config;
    config.directory = directory;
    config.file_stem = "redclaw-test";
    config.role = "controller";
    config.max_file_bytes = 1024 * 1024;
    config.retention_days = 0;

    redclaw::diag::ProcessFileLogger logger;
    std::string error;
    ASSERT_TRUE(logger.start(config, &error)) << error;
    std::vector<std::thread> writers;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        writers.emplace_back([&logger, thread_index]() {
            for (int line = 0; line < 100; ++line) {
                logger.write_line("worker", "thread=" + std::to_string(thread_index) + " line=" + std::to_string(line));
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    logger.stop();

    const std::string text = read_all_logs(directory);
    EXPECT_NE(text.find("thread=0 line=0"), std::string::npos);
    EXPECT_NE(text.find("thread=3 line=99"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST(ProcessFileLogger, FlushesBufferedLinesWithinOneSecond) {
    const auto directory = make_test_directory("periodic-flush");
    redclaw::diag::ProcessFileLoggerConfig config;
    config.directory = directory;
    config.file_stem = "redclaw-test";
    config.role = "host";
    config.retention_days = 0;

    redclaw::diag::ProcessFileLogger logger;
    std::string error;
    ASSERT_TRUE(logger.start(config, &error)) << error;
    logger.write_line("test", "periodic-flush-marker");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    EXPECT_NE(read_all_logs(directory).find("periodic-flush-marker"), std::string::npos);
    logger.stop();

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST(ProcessFileLogger, RejectsDirectoryThatIsAFile) {
    const auto root = make_test_directory("invalid");
    std::filesystem::create_directories(root);
    const auto file_path = root / "not-a-directory";
    {
        std::ofstream output(file_path);
        output << "x";
    }

    redclaw::diag::ProcessFileLoggerConfig config;
    config.directory = file_path;
    redclaw::diag::ProcessFileLogger logger;
    std::string error;
    EXPECT_FALSE(logger.start(config, &error));
    EXPECT_FALSE(error.empty());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

class FailingOutputStreamBuffer final : public std::streambuf {
protected:
    int_type overflow(int_type) override {
        return traits_type::eof();
    }

    std::streamsize xsputn(const char*, std::streamsize) override {
        return 0;
    }

    int sync() override {
        return -1;
    }
};

class ConcurrentCallDetectingStreamBuffer final : public std::streambuf {
public:
    [[nodiscard]] bool concurrent_call_detected() const {
        return concurrent_call_detected_.load();
    }

    [[nodiscard]] std::string text() const {
        std::lock_guard<std::mutex> lock(text_mutex_);
        return text_;
    }

protected:
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        enter_call();
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            text_.push_back(traits_type::to_char_type(value));
        }
        leave_call();
        return traits_type::not_eof(value);
    }

    std::streamsize xsputn(const char* data, std::streamsize count) override {
        enter_call();
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            text_.append(data, static_cast<std::size_t>(count));
        }
        leave_call();
        return count;
    }

    int sync() override {
        enter_call();
        leave_call();
        return 0;
    }

private:
    void enter_call() {
        if (call_active_.exchange(true)) {
            concurrent_call_detected_.store(true);
        }
        std::this_thread::yield();
    }

    void leave_call() {
        call_active_.store(false);
    }

    std::atomic_bool call_active_{false};
    std::atomic_bool concurrent_call_detected_{false};
    mutable std::mutex text_mutex_;
    std::string text_;
};

std::unordered_set<std::string> split_non_empty_lines(const std::string& text) {
    std::unordered_set<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) {
            lines.insert(std::move(line));
        }
    }
    return lines;
}

TEST(ProcessFileLogger, StreamCaptureSerializesConcurrentLineWriters) {
    const auto directory = make_test_directory("stream-concurrent");
    redclaw::diag::ProcessFileLoggerConfig config;
    config.directory = directory;
    config.file_stem = "redclaw-test";
    config.role = "controller";
    config.max_file_bytes = 8U * 1024U * 1024U;
    config.retention_days = 0;
    config.memory_ring_max_lines = 4096;
    config.memory_ring_max_bytes = 4U * 1024U * 1024U;

    redclaw::diag::ProcessFileLogger logger;
    std::string error;
    ASSERT_TRUE(logger.start(config, &error)) << error;

    ConcurrentCallDetectingStreamBuffer stdout_sink;
    ConcurrentCallDetectingStreamBuffer stderr_sink;
    std::streambuf* original_stdout = std::cout.rdbuf(&stdout_sink);
    std::streambuf* original_stderr = std::cerr.rdbuf(&stderr_sink);
    const bool stdout_unitbuf_before = (std::cout.flags() & std::ios_base::unitbuf) != 0;
    const bool stderr_unitbuf_before = (std::cerr.flags() & std::ios_base::unitbuf) != 0;

    bool capture_started = false;
    bool unitbuf_cleared = false;
    {
        redclaw::diag::ScopedProcessStreamCapture capture;
        capture_started = capture.start(&logger, &error);
        if (capture_started) {
            unitbuf_cleared = (std::cout.flags() & std::ios_base::unitbuf) == 0
                && (std::cerr.flags() & std::ios_base::unitbuf) == 0;
            std::atomic_bool start{false};
            std::vector<std::thread> writers;
            for (int thread_index = 0; thread_index < 8; ++thread_index) {
                writers.emplace_back([thread_index, &start]() {
                    while (!start.load()) {
                        std::this_thread::yield();
                    }
                    std::ostream& output = thread_index % 2 == 0 ? std::cout : std::cerr;
                    for (int line_index = 0; line_index < 100; ++line_index) {
                        output << "stream-thread=" << thread_index
                               << " line=" << line_index
                               << " marker=[T" << thread_index << "-L" << line_index << "]\n";
                        if (line_index % 23 == 0) {
                            output.flush();
                        }
                    }
                    if (thread_index == 0) {
                        output << "RCD-LOCAL-CONTROL-V1 " << std::string(60U * 1024U, 'x') << '\n';
                    }
                });
            }
            start.store(true);
            for (auto& writer : writers) {
                writer.join();
            }
            std::cout << "stdout-tail-without-newline";
            std::cerr << "stderr-tail-without-newline";
            capture.stop();
        }
    }

    const bool stdout_unitbuf_after = (std::cout.flags() & std::ios_base::unitbuf) != 0;
    const bool stderr_unitbuf_after = (std::cerr.flags() & std::ios_base::unitbuf) != 0;
    std::cout.rdbuf(original_stdout);
    std::cerr.rdbuf(original_stderr);
    std::cout.clear();
    std::cerr.clear();
    logger.stop();

    ASSERT_TRUE(capture_started) << error;
    EXPECT_TRUE(unitbuf_cleared);
    EXPECT_EQ(stdout_unitbuf_after, stdout_unitbuf_before);
    EXPECT_EQ(stderr_unitbuf_after, stderr_unitbuf_before);
    EXPECT_FALSE(stdout_sink.concurrent_call_detected());
    EXPECT_FALSE(stderr_sink.concurrent_call_detected());

    const std::string stdout_text = stdout_sink.text();
    const std::string stderr_text = stderr_sink.text();
    const auto stdout_lines = split_non_empty_lines(stdout_text);
    const auto stderr_lines = split_non_empty_lines(stderr_text);
    EXPECT_EQ(stdout_lines.size(), 402U);
    EXPECT_EQ(stderr_lines.size(), 401U);
    for (int thread_index = 0; thread_index < 8; ++thread_index) {
        const auto& lines = thread_index % 2 == 0 ? stdout_lines : stderr_lines;
        for (int line_index = 0; line_index < 100; ++line_index) {
            const std::string expected = "stream-thread=" + std::to_string(thread_index)
                + " line=" + std::to_string(line_index)
                + " marker=[T" + std::to_string(thread_index)
                + "-L" + std::to_string(line_index) + ']';
            EXPECT_EQ(lines.count(expected), 1U) << expected;
        }
    }
    const std::string protocol_line = "RCD-LOCAL-CONTROL-V1 " + std::string(60U * 1024U, 'x');
    EXPECT_EQ(stdout_lines.count(protocol_line), 1U);
    EXPECT_EQ(stdout_lines.count("stdout-tail-without-newline"), 1U);
    EXPECT_EQ(stderr_lines.count("stderr-tail-without-newline"), 1U);

    const std::string logged_text = read_all_logs(directory);
    EXPECT_NE(logged_text.find("marker=[T0-L0]"), std::string::npos);
    EXPECT_NE(logged_text.find("marker=[T7-L99]"), std::string::npos);
    EXPECT_NE(logged_text.find("stdout-tail-without-newline"), std::string::npos);
    EXPECT_NE(logged_text.find("stderr-tail-without-newline"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST(ProcessFileLogger, MemoryRingIsBoundedRedactedAndReportsGaps) {
    const auto directory = make_test_directory("memory-ring");
    redclaw::diag::ProcessFileLoggerConfig config;
    config.directory = directory;
    config.file_stem = "redclaw-test";
    config.role = "host";
    config.retention_days = 0;
    config.memory_ring_max_lines = 3;
    config.memory_ring_max_bytes = 64 * 1024;

    redclaw::diag::ProcessFileLogger logger;
    std::string error;
    ASSERT_TRUE(logger.start(config, &error)) << error;
    logger.write_line("test", "line=1 token=secret-one");
    logger.write_line("test", "line=2");
    logger.write_line("test", "line=3");
    logger.write_line("test", "line=4");
    logger.write_line("test", "line=5 password=secret-five");
    logger.write_line("stdout", "RCD-LOCAL-CONTROL-V1 protocol-self-echo");

    const auto latest = logger.snapshot(0, 2000, 512U * 1024U);
    ASSERT_EQ(latest.lines.size(), 3U);
    EXPECT_EQ(latest.first_cursor, 3U);
    EXPECT_EQ(latest.next_cursor, 5U);
    EXPECT_NE(latest.lines.front().find("line=3"), std::string::npos);
    EXPECT_NE(latest.lines.back().find("line=5"), std::string::npos);
    for (const auto& line : latest.lines) {
        EXPECT_EQ(line.find("secret-one"), std::string::npos);
        EXPECT_EQ(line.find("secret-five"), std::string::npos);
        EXPECT_EQ(line.find("RCD-LOCAL-CONTROL-V1"), std::string::npos);
    }

    const auto follow = logger.snapshot(1, 2000, 256U * 1024U);
    EXPECT_TRUE(follow.gap);
    EXPECT_EQ(follow.first_cursor, 3U);
    EXPECT_EQ(follow.next_cursor, 5U);
    logger.stop();

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST(ProcessFileLogger, StreamCaptureSurvivesUnavailableOriginalStdout) {
    const auto directory = make_test_directory("headless-stream");
    redclaw::diag::ProcessFileLoggerConfig config;
    config.directory = directory;
    config.file_stem = "redclaw-test";
    config.role = "host";
    config.retention_days = 0;

    redclaw::diag::ProcessFileLogger logger;
    std::string error;
    ASSERT_TRUE(logger.start(config, &error)) << error;

    FailingOutputStreamBuffer failing_output;
    std::streambuf* original_output = std::cout.rdbuf(&failing_output);
    std::cout.clear();
    bool capture_started = false;
    {
        redclaw::diag::ScopedProcessStreamCapture capture;
        capture_started = capture.start(&logger, &error);
        if (capture_started) {
            std::cout << "headless-remote-log-marker\n";
            std::cout.flush();
            capture.stop();
        }
    }
    std::cout.rdbuf(original_output);
    std::cout.clear();

    EXPECT_TRUE(capture_started) << error;
    const auto snapshot = logger.snapshot(0, 20, 64 * 1024);
    ASSERT_EQ(snapshot.lines.size(), 1U);
    EXPECT_NE(snapshot.lines.front().find("headless-remote-log-marker"), std::string::npos);
    logger.stop();

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST(RemoteDiagnosticsPolicy, DebugAllowsAndReleaseRequiresLocalOptIn) {
    EXPECT_TRUE(redclaw::diag::remote_diagnostics_allowed_by_policy(false, false));
    EXPECT_FALSE(redclaw::diag::remote_diagnostics_allowed_by_policy(true, false));
    EXPECT_TRUE(redclaw::diag::remote_diagnostics_allowed_by_policy(true, true));
}

TEST(DebugRuntimeStatus, ParsesDhtIceAndStreamStages) {
    redclaw::diag::DebugRuntimeStatus status;
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE config ice_server_count=1 enable_ice_tcp=true ice_udp_port=55000 port_mapping=requested network_bind_address=192.168.0.20 signal_timeout_seconds=90",
        &status));
    EXPECT_EQ(status.network_bind_address, "192.168.0.20");
    EXPECT_EQ(status.ice_udp_port, 55000U);

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE port mapping role=host status=mapped internal_port=55000 external_port=55009 external_ip=203.0.113.7 detail=mapped",
        &status));
    EXPECT_EQ(status.ice_port_mapping_status, "mapped");
    EXPECT_EQ(status.ice_mapped_internal_port, 55000U);
    EXPECT_EQ(status.ice_mapped_external_port, 55009U);
    EXPECT_FALSE(status.ice_mapped_external_ip.empty());

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime DHT signal published role=host revision=4 description_tag=abc123 candidates=5 phase=full full_candidate_published=true",
        &status));
    EXPECT_EQ(status.role, "host");
    EXPECT_EQ(status.local_description_tag, "abc123");

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime DHT stats role=host backend=libtorrent reachable=true publish_success=3 generation_publish_success=2 fetch_hits=2 generation=7 negotiation_phase=answer_applied answer_acknowledged=true local_revision=4 remote_revision=5 publisher_instance=12345678 last_host_instance=abcdef01 persistent_offer_adopted_initial_total=1 persistent_offer_adopted_host_restart_total=2 persistent_offer_same_instance_rejected_total=3 peer_instance_missing_total=4 duplicate_offer_application_suppressed_total=5 publish_revision=10 publish_expiry=1500 publish_expired_total=2 publish_late_results=3",
        &status));
    EXPECT_TRUE(status.dht_reachable);
    EXPECT_EQ(status.dht_publish_success, 3U);
    EXPECT_EQ(status.dht_generation_publish_success, 2U);
    EXPECT_EQ(status.dht_publish_revision, 10U);
    EXPECT_EQ(status.dht_publish_expiry, 1500U);
    EXPECT_EQ(status.dht_publish_expired_total, 2U);
    EXPECT_EQ(status.dht_publish_late_results, 3U);
    EXPECT_EQ(status.dht_generation, 7U);
    EXPECT_EQ(status.negotiation_phase, "answer_applied");
    EXPECT_TRUE(status.answer_acknowledged);
    EXPECT_EQ(status.dht_publisher_instance_summary, "12345678");
    EXPECT_EQ(status.dht_last_host_instance_summary, "abcdef01");
    EXPECT_EQ(status.persistent_offer_adopted_initial_total, 1U);
    EXPECT_EQ(status.persistent_offer_adopted_host_restart_total, 2U);
    EXPECT_EQ(status.persistent_offer_same_instance_rejected_total, 3U);
    EXPECT_EQ(status.peer_instance_missing_total, 4U);
    EXPECT_EQ(status.duplicate_offer_application_suppressed_total, 5U);

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime Controller adopted persistent Host offer role=controller adoption_reason=restarted_host publisher_instance=feedbeef generation=1 remote_revision=9",
        &status));
    EXPECT_EQ(status.dht_last_adoption_reason, "restarted_host");
    EXPECT_EQ(status.dht_last_host_instance_summary, "feedbeef");

    status.answer_acknowledged = false;
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime Host DHT answer acknowledgement accepted role=controller generation=7 answer_tag=answer7",
        &status));
    EXPECT_TRUE(status.answer_acknowledged);
    EXPECT_EQ(status.negotiation_phase, "answer_applied");

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime candidate stats role=host local_host=4 local_srflx=1 local_relay=1 remote_host=2 remote_srflx=1 remote_relay=1 "
        "local_dht_direct_published=2 local_dht_full_published=true remote_dht_latest_candidates=4",
        &status));
    EXPECT_EQ(status.local_relay, 1U);
    EXPECT_EQ(status.remote_relay, 1U);
    EXPECT_EQ(status.local_dht_direct_published, 2U);
    EXPECT_TRUE(status.full_candidates_published);
    EXPECT_EQ(status.remote_dht_latest_candidates, 4U);

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime remote description applied role=host (dht) remote_revision=5 remote_description_tag=def456 answered_description_tag=abc123",
        &status));
    EXPECT_TRUE(status.remote_description_applied);
    EXPECT_EQ(status.remote_description_tag, "def456");

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime state role=host state=3 remote_description_applied=true connected=true",
        &status));
    EXPECT_TRUE(status.connected);

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime desktop stream stats role=host channel_open=true captured=9 synthetic=0 capture_failures=0 encoded=8 encode_failures=0 transmitted=8 transmit_failures=0 received=0 decoded=0 decode_failures=0 rendered=0 render_failures=0",
        &status));
    EXPECT_TRUE(status.channel_open);
    EXPECT_EQ(status.phase, "streaming");
    EXPECT_EQ(status.synthetic, 0U);
}

TEST(DebugRuntimeStatus, KeepsAutomaticDhtRepairActiveInsteadOfFailed) {
    redclaw::diag::DebugRuntimeStatus status;
    status.role = "controller";
    status.phase = "ice_connecting";
    status.remote_description_applied = true;

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE state failed before DHT signaling completed; continuing role=controller local_revision=1",
        &status));
    EXPECT_EQ(status.phase, "dht_waiting");
    EXPECT_FALSE(status.connected);
    EXPECT_EQ(status.ice_state, "failed");

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE state failed; DHT retry remains active role=controller local_revision=3",
        &status));
    EXPECT_EQ(status.phase, "dht_waiting");
    EXPECT_FALSE(status.connected);
    EXPECT_EQ(status.ice_state, "failed");

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime rebuilding signaling session role=controller attempt=2 reason=peer republished its description",
        &status));
    EXPECT_EQ(status.phase, "dht_waiting");
    EXPECT_FALSE(status.remote_description_applied);
    EXPECT_FALSE(status.connected);
    EXPECT_FALSE(status.channel_open);
    EXPECT_EQ(status.ice_state, "new");
    EXPECT_TRUE(status.last_error.empty());

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime desktop stream stats role=host channel_open=false captured=9 synthetic=0 "
        "capture_failures=0 encoded=8 encode_failures=0 transmitted=8 transmit_failures=0 "
        "received=0 decoded=0 decode_failures=0 rendered=0 render_failures=0",
        &status));
    EXPECT_EQ(status.phase, "dht_waiting");
    EXPECT_FALSE(status.channel_open);

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE state failed",
        &status));
    EXPECT_EQ(status.phase, "failed");
}

TEST(ProcessFileLogger, BoundedTailSnapshotStaysUnderDebugControlBudget) {
    const auto directory = make_test_directory("bounded-tail-pressure");
    redclaw::diag::ProcessFileLoggerConfig config;
    config.directory = directory;
    config.file_stem = "redclaw-test";
    config.role = "controller";
    config.retention_days = 0;
    config.memory_ring_max_lines = 4096;
    config.memory_ring_max_bytes = 1024U * 1024U;

    redclaw::diag::ProcessFileLogger logger;
    std::string error;
    ASSERT_TRUE(logger.start(config, &error)) << error;
    const std::string payload(220, 'x');
    for (int index = 0; index < 4096; ++index) {
        logger.write_line("pressure", "line=" + std::to_string(index) + " " + payload);
    }

    const auto started_at = std::chrono::steady_clock::now();
    const auto snapshot = logger.snapshot(0, 200, 64U * 1024U);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    std::size_t bytes = 0;
    for (const auto& line : snapshot.lines) {
        bytes += line.size();
    }
    EXPECT_LE(snapshot.lines.size(), 200U);
    EXPECT_LE(bytes, 64U * 1024U);
    EXPECT_LT(elapsed, std::chrono::milliseconds(500));
    EXPECT_FALSE(snapshot.lines.empty());
    logger.stop();

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST(DebugRuntimeStatus, ParsesDecoderRecoveryAndReceiverCapacityTelemetry) {
    redclaw::diag::DebugRuntimeStatus status;
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime controller stream consumption stats role=controller "
        "decoder_recovery_state=2 decoder_recovery_generation=7 "
        "decoder_recovery_breaks_total=11 decoder_recovery_requests_total=4 "
        "decoder_recovery_retries_total=2 decoder_recovery_suppressed_total=8 "
        "decoder_recovery_displayable_acks_total=3 decoder_recovery_invalidated_total=1",
        &status));
    EXPECT_EQ(status.decoder_recovery_state, 2U);
    EXPECT_EQ(status.decoder_recovery_generation, 7U);
    EXPECT_EQ(status.decoder_recovery_breaks_total, 11U);
    EXPECT_EQ(status.decoder_recovery_requests_total, 4U);
    EXPECT_EQ(status.decoder_recovery_retries_total, 2U);
    EXPECT_EQ(status.decoder_recovery_suppressed_total, 8U);
    EXPECT_EQ(status.decoder_recovery_displayable_acks_total, 3U);
    EXPECT_EQ(status.decoder_recovery_invalidated_total, 1U);

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime stream adaptation stats role=host receiver_decode_fps=12 "
        "receiver_decode_pressure_windows=2 receiver_decode_stable_windows=0 "
        "receiver_decode_fps_decrease_total=3 receiver_decode_fps_increase_total=1",
        &status));
    EXPECT_EQ(status.receiver_decode_fps, 12U);
    EXPECT_EQ(status.receiver_decode_pressure_windows, 2U);
    EXPECT_EQ(status.receiver_decode_stable_windows, 0U);
    EXPECT_EQ(status.receiver_decode_fps_decrease_total, 3U);
    EXPECT_EQ(status.receiver_decode_fps_increase_total, 1U);
}

TEST(DebugRuntimeStatus, ParsesRemoteAgentChannelAndBrokerTelemetry) {
    redclaw::diag::DebugRuntimeStatus status;
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime remote agent stats role=host authorized=true channel_open=true "
        "channel_unavailable=false channel_open_total=2 channel_close_total=1 "
        "channel_rebuild_attempt_total=1 channel_rebuild_success_total=1 "
        "capability_refresh_total=3 task_create_total=4 task_complete_total=2 "
        "task_failed_total=1 duplicate_task_rejected_total=1 event_total=20 "
        "event_ack_total=15 replayed_event_total=3 gap_total=2 "
        "approval_request_total=4 approval_accept_total=2 approval_reject_total=1 "
        "approval_timeout_total=1 queue_peak=3 cached_event_bytes=1000 "
        "cached_event_bytes_peak=2000 outbound_queue_current=2 outbound_queue_peak=9",
        &status));
    EXPECT_TRUE(status.agent_authorized);
    EXPECT_TRUE(status.agent_channel_open);
    EXPECT_EQ(status.agent_channel_rebuild_success_total, 1U);
    EXPECT_EQ(status.agent_task_create_total, 4U);
    EXPECT_EQ(status.agent_event_ack_total, 15U);
    EXPECT_EQ(status.agent_replayed_event_total, 3U);
    EXPECT_EQ(status.agent_approval_timeout_total, 1U);
    EXPECT_EQ(status.agent_cached_event_bytes_peak, 2000U);
    EXPECT_EQ(status.agent_outbound_queue_peak, 9U);
}

TEST(DebugRuntimeStatus, ParsesControllerRemoteAgentAuthorizationAndChannel) {
    redclaw::diag::DebugRuntimeStatus status;
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime remote agent stats role=controller authorized=true channel_open=true "
        "channel_unavailable=false channel_open_total=1 channel_close_total=0 "
        "channel_rebuild_attempt_total=0 channel_rebuild_success_total=0 "
        "capability_refresh_total=0 task_create_total=0 task_complete_total=0 "
        "task_failed_total=0 duplicate_task_rejected_total=0 event_total=0 "
        "event_ack_total=0 replayed_event_total=0 gap_total=0 "
        "approval_request_total=0 approval_accept_total=0 approval_reject_total=0 "
        "approval_timeout_total=0 queue_peak=0 cached_event_bytes=0 "
        "cached_event_bytes_peak=0 outbound_queue_current=0 outbound_queue_peak=0",
        &status));
    EXPECT_TRUE(status.agent_authorized);
    EXPECT_TRUE(status.agent_channel_open);
    EXPECT_EQ(status.agent_channel_open_total, 1U);
}

TEST(DebugRuntimeStatus, SeparatesLocalExecutionFromRemoteRequestTelemetry) {
    redclaw::diag::DebugRuntimeStatus status;
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime remote agent stats role=host authorized=true local_authorized=false "
        "local_execution_state=0 local_execution_task=none local_execution_queued=0 "
        "request_queue_depth=2 result_queue_depth=3 sent_requests=9 sent_results=8 "
        "received_commands=7 received_results=6 request_queue_peak=64 result_queue_peak=256 "
        "peer_rejected_total=4 peer_stale_total=5 send_pump_max_us=901", &status));
    EXPECT_TRUE(status.agent_authorized);
    EXPECT_FALSE(status.agent_local_authorized);
    EXPECT_EQ(status.agent_sent_requests, 9U);
    EXPECT_EQ(status.agent_sent_results, 8U);
    EXPECT_EQ(status.agent_result_queue_peak, 256U);
    EXPECT_EQ(status.agent_send_pump_max_us, 901U);
}

TEST(DebugRuntimeStatus, ControllerStreamingRequiresGuiPresentationEvidence) {
    redclaw::diag::DebugRuntimeStatus status;
    status.role = "controller";
    status.phase = "channel_open";
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime desktop stream stats role=controller channel_open=true received=4 "
        "media_fragments_received=9 encoded_frames_reassembled=4 direct_pipe_written=4 "
        "decoded=0 decode_failures=0 rendered=0 render_failures=0",
        &status));
    EXPECT_EQ(status.phase, "channel_open");
    EXPECT_EQ(status.media_fragments_received, 9U);
    EXPECT_EQ(status.encoded_frames_reassembled, 4U);
    EXPECT_EQ(status.direct_pipe_written, 4U);
    EXPECT_EQ(status.decoded, 0U);
    EXPECT_EQ(status.rendered, 0U);
}

TEST(DebugRuntimeStatus, TransportOnlyConnectionBecomesExplicitMediaStartupFailure) {
    redclaw::diag::DebugRuntimeStatus status;
    status.role = "host";
    status.connected = true;
    status.phase = "channel_open";

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime desktop stream stats role=host channel_open=true "
        "captured=15 synthetic=0 capture_failures=0 encoded=0 encode_failures=0 transmitted=0 "
        "transmit_failures=0 received=0 decoded=0 decode_failures=0 rendered=0 render_failures=0",
        &status));
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime desktop stream rates role=host first_media_deadline_ms=10000 "
        "health=encoder_startup_stalled",
        &status));
    EXPECT_EQ(status.stream_health, "encoder_startup_stalled");
    EXPECT_EQ(status.phase, "failed");
    EXPECT_NE(status.last_error.find("encoder_startup_stalled"), std::string::npos);

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime state role=host state=3 remote_description_applied=true connected=true",
        &status));
    EXPECT_EQ(status.phase, "failed");

    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime rebuilding signaling session role=host attempt=1 reason=media startup failed",
        &status));
    EXPECT_EQ(status.phase, "dht_waiting");
    EXPECT_EQ(status.stream_health, "idle");
    EXPECT_TRUE(status.last_error.empty());
}

TEST(DebugRuntimeStatus, NativeTransportDiagnosticsRemainBoundedAndKeepFirstCause) {
    redclaw::diag::DebugRuntimeStatus status;
    for (unsigned i = 1; i <= 100; ++i) {
        EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
            "Runtime transport diagnostic sequence=" + std::to_string(i)
                + " steady_ms=100 peer_generation=1 channel_generation=2 layer=1 channel=99 native_state=4 failure=true reason=native_ice_state",
            &status));
    }
    EXPECT_EQ(status.transport_diagnostics.recent_events.size(), 64U);
    ASSERT_TRUE(status.transport_diagnostics.first_failure.has_value());
    EXPECT_EQ(status.transport_diagnostics.first_failure->sequence, 1U);
    EXPECT_FALSE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime transport diagnostic sequence=100 layer=1", &status));
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime transport diagnostic sequence=101 steady_ms=101 peer_generation=2 layer=0 failure=false reason=new",
        &status));
    EXPECT_FALSE(status.transport_diagnostics.first_failure.has_value());
}

TEST(DebugRuntimeStatus, IceChecksAreBoundedTypedAndGenerationScoped) {
    redclaw::diag::DebugRuntimeStatus status;
    EXPECT_FALSE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime transport diagnostic sequence=1 layer=4 native_state=255", &status));
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE check stats peer_generation=2 request_tx=7 response_rx=0 validation_failed=3", &status));
    EXPECT_EQ(status.transport_diagnostics.ice_check_totals[0], 7U);
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE check stats peer_generation=2 request_tx=2", &status));
    EXPECT_EQ(status.transport_diagnostics.ice_check_totals[0], 7U);
    for (unsigned i = 1; i <= 100; ++i) {
        EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
            "Runtime transport diagnostic sequence=" + std::to_string(i)
            + " steady_ms=100 peer_generation=2 layer=4 native_state=6 failure=false"
              " reason=check_timeout pair_id=-1 local_type=1 remote_type=2 occurrences=7", &status));
    }
    EXPECT_EQ(status.transport_diagnostics.recent_events.size(), 64U);
    EXPECT_FALSE(status.transport_diagnostics.first_failure.has_value());
    EXPECT_EQ(status.transport_diagnostics.recent_events.back().pair_id, -1);
    EXPECT_EQ(status.transport_diagnostics.recent_events.back().occurrences, 7U);
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE check stats peer_generation=5 request_tx=1", &status));
    EXPECT_EQ(status.transport_diagnostics.ice_check_totals[0], 1U);
    EXPECT_EQ(status.transport_diagnostics.ice_check_totals[4], 0U);
    EXPECT_FALSE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE check stats peer_generation=2 request_tx=999", &status));
    EXPECT_EQ(status.transport_diagnostics.ice_check_totals[0], 1U);
}

TEST(DebugRuntimeStatus, IgnoredUdpErrorsAndCandidateEvidenceSurviveOrdinaryRingChurn) {
    redclaw::diag::DebugRuntimeStatus status;
    const std::string reason="candidate_remote:0123456789abcdef0123456789abcdef:v4:host:100";
    ASSERT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime transport diagnostic sequence=1 steady_ms=200 peer_generation=4 layer=5 reason=" + reason, &status));
    ASSERT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE check stats peer_generation=4 udp_ignored_connreset=3 udp_ignored_netreset=2 udp_ignored_connrefused=1 udp_error_observation_ready=1", &status));
    using redclaw::net::IceCheckKind;
    EXPECT_EQ(status.transport_diagnostics.ice_check_totals[static_cast<std::size_t>(IceCheckKind::kUdpIgnoredConnReset)],3U);
    EXPECT_EQ(status.transport_diagnostics.ice_check_totals[static_cast<std::size_t>(IceCheckKind::kUdpIgnoredNetReset)],2U);
    EXPECT_EQ(status.transport_diagnostics.ice_check_totals[static_cast<std::size_t>(IceCheckKind::kUdpIgnoredConnRefused)],1U);
    for (unsigned i=2; i<100; ++i)
        ASSERT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
            "Runtime transport diagnostic sequence=" + std::to_string(i) + " peer_generation=4 layer=4 native_state=0 reason=request_tx", &status));
    ASSERT_EQ(status.transport_diagnostics.candidate_events.size(), 1U);
    EXPECT_EQ(status.transport_diagnostics.candidate_events[0].reason, reason);
    EXPECT_FALSE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime transport diagnostic sequence=100 peer_generation=4 layer=5 reason=candidate_local:192.0.2.1:v4:host:1", &status));
    ASSERT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE check stats peer_generation=5 udp_error_observation_ready=1", &status));
    EXPECT_TRUE(status.transport_diagnostics.candidate_events.empty());
    EXPECT_EQ(status.transport_diagnostics.ice_check_totals[static_cast<std::size_t>(IceCheckKind::kUdpIgnoredConnReset)],0U);
    EXPECT_FALSE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime transport diagnostic sequence=101 peer_generation=4 layer=5 reason=" + reason, &status));
}

TEST(DebugRuntimeStatus, ReceiveBoundaryCountersRemainDistinctAndRejectOldGenerations) {
    using redclaw::net::IceCheckKind;
    redclaw::diag::DebugRuntimeStatus status;
    const auto count = [&](IceCheckKind kind) {
        return status.transport_diagnostics.ice_check_totals[static_cast<std::size_t>(kind)];
    };
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE check stats peer_generation=4 socket_rx=100 socket_rx_server=90 socket_rx_peer=10"
        " stun_parse_failed=10 request_rx=0 local_candidate_socket_match=1 send_target_mismatch=0", &status));
    EXPECT_EQ(count(IceCheckKind::kSocketRx), 100U);
    EXPECT_EQ(count(IceCheckKind::kSocketRxServer), 90U);
    EXPECT_EQ(count(IceCheckKind::kStunParseFailed), 10U);
    EXPECT_EQ(count(IceCheckKind::kRequestRx), 0U);
    EXPECT_FALSE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE check stats peer_generation=3 socket_rx=99999", &status));
    EXPECT_EQ(count(IceCheckKind::kSocketRx), 100U);
    EXPECT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime ICE check stats peer_generation=5 socket_ready=1", &status));
    EXPECT_EQ(count(IceCheckKind::kSocketReady), 1U);
    EXPECT_EQ(count(IceCheckKind::kSocketRxServer), 0U);
}

TEST(DebugRuntimeStatus, ListenerReadinessIsSeparateFromDhtReachabilityAndBounded) {
    redclaw::diag::DebugRuntimeStatus status;
    ASSERT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime DHT backend diagnostics role=host listen_ready=true listen_startup_failed=false listen_port_probe_attempts=999", &status));
    EXPECT_TRUE(status.dht_listener_available);
    EXPECT_TRUE(status.dht_listener_ready);
    EXPECT_FALSE(status.dht_reachable);
    EXPECT_EQ(status.dht_listener_probe_attempts, 16U);
    ASSERT_TRUE(redclaw::diag::update_debug_runtime_status_from_line(
        "Runtime DHT backend diagnostics role=host listen_ready=false listen_startup_failed=true", &status));
    EXPECT_FALSE(status.dht_listener_ready);
    EXPECT_TRUE(status.dht_listener_startup_failed);
}
}  // namespace
