#include "redclaw/diag/diag_module.h"
#include "redclaw/diag/operation_timing.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace redclaw::diag {

namespace {

std::uint64_t current_process_id() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

std::string local_timestamp_for_file() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &raw);
#else
    localtime_r(&raw, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d-%H%M%S");
    return out.str();
}

std::string local_timestamp_for_line() {
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &raw);
#else
    localtime_r(&raw, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << millis.count();
    return out.str();
}

std::string sanitize_stem(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        const bool allowed = std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-' || ch == '_';
        result.push_back(allowed ? ch : '-');
    }
    return result.empty() ? "redclaw-desktop" : result;
}


bool starts_with_redclaw_log_prefix(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return name.rfind("redclaw-", 0) == 0;
}

void cleanup_log_directory(const ProcessFileLoggerConfig& config) {
    std::error_code ec;
    if (!std::filesystem::is_directory(config.directory, ec)) {
        return;
    }

    struct LogFileEntry {
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
        std::uint64_t size = 0;
    };

    std::vector<LogFileEntry> files;
    const auto retention = std::chrono::hours(24ULL * config.retention_days);
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(config.directory, ec)) {
        if (ec || !entry.is_regular_file(ec) || !starts_with_redclaw_log_prefix(entry.path())) {
            ec.clear();
            continue;
        }
        const auto modified = entry.last_write_time(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (config.retention_days > 0 && now - modified > retention) {
            std::filesystem::remove(entry.path(), ec);
            ec.clear();
            continue;
        }
        const auto size = entry.file_size(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        files.push_back({entry.path(), modified, static_cast<std::uint64_t>(size)});
    }

    std::uint64_t total = 0;
    for (const auto& file : files) {
        total += file.size;
    }
    if (config.max_directory_bytes == 0 || total <= config.max_directory_bytes) {
        return;
    }

    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.modified < right.modified;
    });
    for (const auto& file : files) {
        if (total <= config.max_directory_bytes) {
            break;
        }
        if (std::filesystem::remove(file.path, ec)) {
            total = total >= file.size ? total - file.size : 0;
        }
        ec.clear();
    }
}

std::string_view level_to_string(LogLevel level) {
    switch (level) {
    case LogLevel::kDebug:
        return "debug";
    case LogLevel::kInfo:
        return "info";
    case LogLevel::kWarn:
        return "warn";
    case LogLevel::kError:
        return "error";
    }

    return "unknown";
}

std::string_view ice_state_to_string(net::IceConnectionState state) {
    switch (state) {
    case net::IceConnectionState::kNew:
        return "new";
    case net::IceConnectionState::kGathering:
        return "gathering";
    case net::IceConnectionState::kConnecting:
        return "connecting";
    case net::IceConnectionState::kConnected:
        return "connected";
    case net::IceConnectionState::kDisconnected:
        return "disconnected";
    case net::IceConnectionState::kFailed:
        return "failed";
    case net::IceConnectionState::kClosed:
        return "closed";
    }

    return "unknown";
}

std::string_view session_state_to_string(protocol::SessionStateV1 state) {
    switch (state) {
    case protocol::SessionStateV1::idle:
        return "idle";
    case protocol::SessionStateV1::offering:
        return "offering";
    case protocol::SessionStateV1::connecting:
        return "connecting";
    case protocol::SessionStateV1::established:
        return "established";
    case protocol::SessionStateV1::recovering:
        return "recovering";
    case protocol::SessionStateV1::terminated:
        return "terminated";
    }

    return "unknown";
}

bool contains_sensitive_fragment(std::string_view normalized_name) {
    constexpr std::array<std::string_view, 9> kSensitiveFragments = {
        "password",
        "passphrase",
        "token",
        "secret",
        "signature",
        "fingerprint",
        "auth",
        "proof",
        "key",
    };

    for (const auto fragment : kSensitiveFragments) {
        if (normalized_name.find(fragment) != std::string_view::npos) {
            return true;
        }
    }

    return false;
}

std::string normalize_ascii_lower(std::string_view value) {
    std::string normalized(value);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return normalized;
}

void append_sorted_map(
    std::ostringstream& out,
    const std::unordered_map<std::string, std::string>& fields,
    bool redact_values) {
    std::vector<std::pair<std::string, std::string>> items;
    items.reserve(fields.size());
    for (const auto& [name, value] : fields) {
        items.emplace_back(name, redact_values ? redact_field_value(name, value) : value);
    }

    std::sort(
        items.begin(),
        items.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });

    for (const auto& [name, value] : items) {
        out << name << '=' << value << '\n';
    }
}

}  // namespace

class ProcessFileLogger::Impl {
public:
    ~Impl() {
        stop();
    }

    bool start(const ProcessFileLoggerConfig& requested, std::string* error_detail) {
        stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            config_ = requested;
            memory_lines_.clear();
            memory_lines_bytes_ = 0;
            next_memory_cursor_ = 0;
            config_.file_stem = sanitize_stem(config_.file_stem);
            config_.role = sanitize_stem(config_.role);
            if (config_.directory.empty()) {
                if (error_detail != nullptr) {
                    *error_detail = "log directory is empty";
                }
                return false;
            }
            if (config_.max_file_bytes == 0 || config_.max_file_count == 0) {
                if (error_detail != nullptr) {
                    *error_detail = "log rotation limits must be non-zero";
                }
                return false;
            }

            std::error_code ec;
            std::filesystem::create_directories(config_.directory, ec);
            if (ec || !std::filesystem::is_directory(config_.directory, ec)) {
                if (error_detail != nullptr) {
                    *error_detail = "failed to create log directory: " + config_.directory.string()
                        + (ec ? " (" + ec.message() + ")" : std::string());
                }
                return false;
            }
            cleanup_log_directory(config_);

            const std::string filename = config_.file_stem + '-' + config_.role + '-'
                + local_timestamp_for_file() + '-' + std::to_string(current_process_id()) + ".log";
            path_ = config_.directory / filename;
            stream_.open(path_, std::ios::binary | std::ios::app);
            if (!stream_) {
                if (error_detail != nullptr) {
                    *error_detail = "failed to open log file: " + path_.string();
                }
                path_.clear();
                return false;
            }
            current_size_ = static_cast<std::uint64_t>(std::filesystem::file_size(path_, ec));
            if (ec) {
                current_size_ = 0;
            }
            stop_requested_ = false;
            dirty_ = false;
            last_flush_ = std::chrono::steady_clock::now();
        }
        try {
            flush_thread_ = std::thread([this]() { flush_loop(); });
        } catch (const std::system_error& exception) {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_locked();
            if (error_detail != nullptr) {
                *error_detail = "failed to start log flush worker: " + std::string(exception.what());
            }
            return false;
        }
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        flush_cv_.notify_all();
        if (flush_thread_.joinable()) {
            flush_thread_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        stop_locked();
    }

    void write_line(std::string_view source, std::string_view line, bool flush_immediately) {
        DiagnosticTimingScope lock_timing(DiagnosticOperation::kLogLock);
        std::lock_guard<std::mutex> lock(mutex_);
        lock_timing.finish();
        if (!stream_) {
            return;
        }
        const std::string redacted = redact_log_text(line);
        const std::string formatted = '[' + local_timestamp_for_line() + "] [pid="
            + std::to_string(current_process_id()) + "] [role=" + config_.role
            + "] [source=" + sanitize_stem(source) + "] " + redacted + '\n';
        if (current_size_ + formatted.size() > config_.max_file_bytes) {
            rotate_locked();
        }
        {
            DiagnosticTimingScope timing(DiagnosticOperation::kFileWrite);
            stream_.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
        }
        current_size_ += formatted.size();
        if (line.find("RCD-LOCAL-CONTROL-") == std::string_view::npos
            && line.find("remote_log_chunk") == std::string_view::npos) {
            append_memory_line_locked(formatted);
        }
        dirty_ = true;
        const auto now = std::chrono::steady_clock::now();
        if (flush_immediately || last_flush_ == std::chrono::steady_clock::time_point{}
            || now - last_flush_ >= std::chrono::seconds(1)) {
            DiagnosticTimingScope timing(DiagnosticOperation::kFlush);
            stream_.flush();
            last_flush_ = now;
            dirty_ = false;
        }
    }

    void flush() {
        DiagnosticTimingScope lock_timing(DiagnosticOperation::kLogLock);
        std::lock_guard<std::mutex> lock(mutex_);
        lock_timing.finish();
        if (stream_) {
            DiagnosticTimingScope timing(DiagnosticOperation::kFlush);
            stream_.flush();
            last_flush_ = std::chrono::steady_clock::now();
            dirty_ = false;
        }
    }

    bool is_open() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stream_.is_open();
    }

    bool is_healthy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stream_.is_open() && stream_.good();
    }

    std::filesystem::path log_path() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return path_;
    }

    ProcessLogSnapshot snapshot(
        std::uint64_t after_cursor,
        std::size_t max_lines,
        std::size_t max_bytes) const {
        std::lock_guard<std::mutex> lock(mutex_);
        ProcessLogSnapshot result;
        result.next_cursor = next_memory_cursor_;
        if (memory_lines_.empty() || max_lines == 0 || max_bytes == 0) {
            return result;
        }
        result.first_cursor = memory_lines_.front().first;
        std::size_t begin_index = 0;
        if (after_cursor == 0) {
            std::size_t bytes = 0;
            begin_index = memory_lines_.size();
            while (begin_index > 0 && memory_lines_.size() - begin_index < max_lines) {
                const std::size_t candidate = memory_lines_[begin_index - 1].second.size();
                if (bytes + candidate > max_bytes && bytes > 0) {
                    break;
                }
                bytes += candidate;
                --begin_index;
            }
        } else {
            result.gap = after_cursor + 1 < result.first_cursor;
            while (begin_index < memory_lines_.size()
                   && memory_lines_[begin_index].first <= after_cursor) {
                ++begin_index;
            }
        }
        std::size_t bytes = 0;
        for (std::size_t index = begin_index;
             index < memory_lines_.size() && result.lines.size() < max_lines;
             ++index) {
            const std::string& line = memory_lines_[index].second;
            if (bytes + line.size() > max_bytes && !result.lines.empty()) {
                break;
            }
            result.lines.push_back(line);
            bytes += line.size();
        }
        return result;
    }

private:
    void append_memory_line_locked(const std::string& line) {
        if (config_.memory_ring_max_lines == 0 || config_.memory_ring_max_bytes == 0) {
            return;
        }
        memory_lines_.emplace_back(++next_memory_cursor_, line);
        memory_lines_bytes_ += line.size();
        while (!memory_lines_.empty()
               && (memory_lines_.size() > config_.memory_ring_max_lines
                   || memory_lines_bytes_ > config_.memory_ring_max_bytes)) {
            memory_lines_bytes_ -= memory_lines_.front().second.size();
            memory_lines_.pop_front();
        }
    }

    void flush_loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_requested_) {
            flush_cv_.wait_for(lock, std::chrono::seconds(1), [this]() { return stop_requested_; });
            if (!stop_requested_ && stream_ && dirty_) {
                stream_.flush();
                last_flush_ = std::chrono::steady_clock::now();
                dirty_ = false;
            }
        }
    }

    void stop_locked() {
        if (stream_) {
            stream_.flush();
            stream_.close();
        }
        path_.clear();
        current_size_ = 0;
        last_flush_ = {};
        dirty_ = false;
    }

    void rotate_locked() {
        stream_.flush();
        stream_.close();
        std::error_code ec;
        if (config_.max_file_count > 1) {
            for (std::size_t index = config_.max_file_count - 1; index > 0; --index) {
                const auto destination = path_.string() + '.' + std::to_string(index);
                const auto source = index == 1
                    ? path_
                    : std::filesystem::path(path_.string() + '.' + std::to_string(index - 1));
                std::filesystem::remove(destination, ec);
                ec.clear();
                if (std::filesystem::exists(source, ec)) {
                    std::filesystem::rename(source, destination, ec);
                }
                ec.clear();
            }
        } else {
            std::filesystem::remove(path_, ec);
        }
        stream_.open(path_, std::ios::binary | std::ios::trunc);
        current_size_ = 0;
        dirty_ = false;
    }

    mutable std::mutex mutex_;
    ProcessFileLoggerConfig config_;
    std::filesystem::path path_;
    std::ofstream stream_;
    std::uint64_t current_size_ = 0;
    std::chrono::steady_clock::time_point last_flush_;
    std::condition_variable flush_cv_;
    std::thread flush_thread_;
    std::deque<std::pair<std::uint64_t, std::string>> memory_lines_;
    std::size_t memory_lines_bytes_ = 0;
    std::uint64_t next_memory_cursor_ = 0;
    bool stop_requested_ = false;
    bool dirty_ = false;
};

namespace {

class LineTeeStreamBuffer final : public std::streambuf {
public:
    LineTeeStreamBuffer(std::streambuf* original, ProcessFileLogger* logger, std::string source, bool error_stream)
        : original_(original), logger_(logger), source_(std::move(source)), error_stream_(error_stream) {
    }

    ~LineTeeStreamBuffer() override {
        flush_pending();
    }

protected:
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        const char ch = traits_type::to_char_type(value);
        DiagnosticTimingScope lock_timing(DiagnosticOperation::kLogLock);
        std::lock_guard<std::mutex> lock(mutex_);
        lock_timing.finish();
        append_locked(std::this_thread::get_id(), &ch, 1);
        return traits_type::not_eof(value);
    }

    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (data == nullptr || count <= 0) {
            return 0;
        }
        DiagnosticTimingScope lock_timing(DiagnosticOperation::kLogLock);
        std::lock_guard<std::mutex> lock(mutex_);
        lock_timing.finish();
        append_locked(std::this_thread::get_id(), data, count);
        return count;
    }

    int sync() override {
        std::lock_guard<std::mutex> lock(mutex_);
        flush_thread_pending_locked(std::this_thread::get_id());
        if (original_ != nullptr) {
            (void)original_->pubsync();
        }
        if (logger_ != nullptr) {
            logger_->flush();
        }
        return 0;
    }

private:
    void flush_pending() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [thread_id, pending] : pending_by_thread_) {
            (void)thread_id;
            write_line_locked(pending, false);
        }
        pending_by_thread_.clear();
    }

    void append_locked(
        const std::thread::id& thread_id,
        const char* data,
        std::streamsize count) {
        std::string& pending = pending_by_thread_[thread_id];
        for (std::streamsize index = 0; index < count; ++index) {
            const char ch = data[index];
            if (ch == '\n') {
                write_line_locked(pending, true);
                pending.clear();
            } else {
                pending.push_back(ch);
            }
        }
        if (pending.empty()) {
            pending_by_thread_.erase(thread_id);
        }
    }

    void flush_thread_pending_locked(const std::thread::id& thread_id) {
        const auto pending_it = pending_by_thread_.find(thread_id);
        if (pending_it == pending_by_thread_.end()) {
            return;
        }
        write_line_locked(pending_it->second, false);
        pending_by_thread_.erase(pending_it);
    }

    void write_line_locked(std::string_view line, bool append_newline) {
        if (line.empty() && !append_newline) {
            return;
        }
        if (original_ != nullptr) {
            DiagnosticTimingScope timing(DiagnosticOperation::kOutputWrite);
            if (!line.empty()) {
                (void)original_->sputn(line.data(), static_cast<std::streamsize>(line.size()));
            }
            if (append_newline) {
                (void)original_->sputc('\n');
            }
            (void)original_->pubsync();
        }
        if (logger_ != nullptr) {
            std::string logger_line(line);
            logger_line.erase(
                std::remove(logger_line.begin(), logger_line.end(), '\r'),
                logger_line.end());
            logger_->write_line(source_, logger_line, error_stream_);
        }
    }

    std::streambuf* original_ = nullptr;
    ProcessFileLogger* logger_ = nullptr;
    std::string source_;
    bool error_stream_ = false;
    std::mutex mutex_;
    std::unordered_map<std::thread::id, std::string> pending_by_thread_;
};

}  // namespace

class ScopedProcessStreamCapture::Impl {
public:
    bool start(ProcessFileLogger* logger, std::string* error_detail, bool diagnostics_to_stderr) {
        if (logger == nullptr || !logger->is_open()) {
            if (error_detail != nullptr) {
                *error_detail = "process file logger is not open";
            }
            return false;
        }
        if (stdout_buffer_ || stderr_buffer_) {
            if (error_detail != nullptr) {
                *error_detail = "process stream capture is already active";
            }
            return false;
        }
        original_stdout_ = std::cout.rdbuf();
        original_stderr_ = std::cerr.rdbuf();
        stdout_unitbuf_enabled_ = (std::cout.flags() & std::ios_base::unitbuf) != 0;
        stderr_unitbuf_enabled_ = (std::cerr.flags() & std::ios_base::unitbuf) != 0;
        stdout_buffer_ = std::make_unique<LineTeeStreamBuffer>(
            diagnostics_to_stderr ? original_stderr_ : original_stdout_, logger, "stdout", false);
        stderr_buffer_ = std::make_unique<LineTeeStreamBuffer>(original_stderr_, logger, "stderr", true);
        std::cout.rdbuf(stdout_buffer_.get());
        std::cerr.rdbuf(stderr_buffer_.get());
        std::cout.unsetf(std::ios_base::unitbuf);
        std::cerr.unsetf(std::ios_base::unitbuf);
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    void stop() {
        if (!stdout_buffer_ && !stderr_buffer_) {
            return;
        }
        if (stdout_buffer_) {
            std::cout.rdbuf(original_stdout_);
            stdout_buffer_.reset();
        }
        if (stderr_buffer_) {
            std::cerr.rdbuf(original_stderr_);
            stderr_buffer_.reset();
        }
        if (stdout_unitbuf_enabled_) {
            std::cout.setf(std::ios_base::unitbuf);
        } else {
            std::cout.unsetf(std::ios_base::unitbuf);
        }
        if (stderr_unitbuf_enabled_) {
            std::cerr.setf(std::ios_base::unitbuf);
        } else {
            std::cerr.unsetf(std::ios_base::unitbuf);
        }
        original_stdout_ = nullptr;
        original_stderr_ = nullptr;
        stdout_unitbuf_enabled_ = false;
        stderr_unitbuf_enabled_ = false;
    }

    ~Impl() {
        stop();
    }

private:
    std::streambuf* original_stdout_ = nullptr;
    std::streambuf* original_stderr_ = nullptr;
    std::unique_ptr<LineTeeStreamBuffer> stdout_buffer_;
    std::unique_ptr<LineTeeStreamBuffer> stderr_buffer_;
    bool stdout_unitbuf_enabled_ = false;
    bool stderr_unitbuf_enabled_ = false;
};

ProcessFileLogger::ProcessFileLogger() : impl_(std::make_unique<Impl>()) {
}

ProcessFileLogger::~ProcessFileLogger() = default;

bool ProcessFileLogger::start(const ProcessFileLoggerConfig& config, std::string* error_detail) {
    return impl_->start(config, error_detail);
}

void ProcessFileLogger::stop() {
    impl_->stop();
}

void ProcessFileLogger::write_line(std::string_view source, std::string_view line, bool flush_immediately) {
    impl_->write_line(source, line, flush_immediately);
}

void ProcessFileLogger::flush() {
    impl_->flush();
}

bool ProcessFileLogger::is_open() const {
    return impl_->is_open();
}

std::filesystem::path ProcessFileLogger::log_path() const {
    return impl_->log_path();
}

ProcessLogSnapshot ProcessFileLogger::snapshot(
    std::uint64_t after_cursor,
    std::size_t max_lines,
    std::size_t max_bytes) const {
    return impl_->snapshot(after_cursor, max_lines, max_bytes);
}

ScopedProcessStreamCapture::ScopedProcessStreamCapture() : impl_(std::make_unique<Impl>()) {
}

ScopedProcessStreamCapture::~ScopedProcessStreamCapture() = default;

bool ScopedProcessStreamCapture::start(ProcessFileLogger* logger, std::string* error_detail, bool diagnostics_to_stderr) {
    return impl_->start(logger, error_detail, diagnostics_to_stderr);
}

void ScopedProcessStreamCapture::stop() {
    impl_->stop();
}

static std::string redact_log_grammar_stage(std::string_view line, unsigned stage) {
    // This runs synchronously at GUI and stream-capture boundaries. Match the
    // fixed ASCII log grammar instead of constructing/scanning four
    // regular expressions for every line (including large protocol diagnostics).
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    const auto word = [&](char c) { return alnum(c) || c == '_'; };
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
    const auto matches = [&](std::size_t at, std::string_view key) {
        if (key.size() > line.size() - at) return false;
        for (std::size_t n = 0; n < key.size(); ++n) if (lower(line[at + n]) != key[n]) return false;
        return true;
    };
    const auto whitespace_end = [&](std::size_t at) { while (at < line.size() && space(line[at])) ++at; return at; };
    constexpr std::array code_keys{"session-code", "session_code", "session code", "sessioncode",
        "machine-code", "machine_code", "machine code", "machinecode",
        "peer-code", "peer_code", "peer code", "peercode", "session", "code"};
    constexpr std::array secret_keys{"passphrase", "password", "token", "secret", "private_key", "fingerprint", "auth", "proof"};
    std::string result;
    result.reserve(line.size());
    std::size_t copied = 0;
    const auto replace = [&](std::size_t begin, std::size_t end, std::string_view replacement) {
        result.append(line.substr(copied, begin - copied));
        result.append(replacement);
        copied = end;
    };
    for (std::size_t i = 0; i < line.size();) {
        if (stage == 0 && (matches(i, "turn:") || matches(i, "turns:"))) {
            const auto begin = i + (lower(line[i + 4]) == 's' ? 6 : 5);
            auto end = begin;
            while (end < line.size() && !space(line[end]) && line[end] != '/' && line[end] != '@') ++end;
            if (end > begin && end < line.size() && line[end] == '@') {
                replace(begin, end, "[REDACTED]"); i = end + 1; continue;
            }
            i = end; continue;
        }
        if (stage == 0) { ++i; continue; }
        if (!word(line[i]) || (i > 0 && word(line[i - 1]))) { ++i; continue; }
        bool replaced = false;
        for (const std::string_view key : secret_keys) {
            if (stage != 2) break;
            if (!matches(i, key)) continue;
            auto at = i + key.size();
            if (at < line.size() && word(line[at])) continue;
            at = whitespace_end(at);
            if (at == line.size() || (line[at] != '=' && line[at] != ':')) continue;
            const auto begin = whitespace_end(at + 1);
            auto end = begin;
            while (end < line.size() && !space(line[end]) && line[end] != ',') ++end;
            if (begin == end) continue;
            replace(begin, end, "[REDACTED]"); i = end; replaced = true; break;
        }
        if (replaced) continue;
        for (const std::string_view key : code_keys) {
            if (stage != 1) break;
            if (!matches(i, key)) continue;
            const auto key_end = i + key.size();
            if (key_end < line.size() && word(line[key_end])) continue;
            const auto after_space = whitespace_end(key_end);
            // Optional " is" is tried first, then its absence, matching the
            // original grammar even for an eight-letter value starting in "is".
            const std::array starts{after_space > key_end && matches(after_space, "is") ? after_space + 2 : key_end, key_end};
            for (auto begin : starts) {
                begin = whitespace_end(begin);
                if (begin < line.size() && (line[begin] == '=' || line[begin] == ':')) begin = whitespace_end(begin + 1);
                const auto end = begin + 8;
                if (end > line.size() || (end < line.size() && word(line[end]))) continue;
                if (!std::all_of(line.begin() + begin, line.begin() + end, alnum)) continue;
                replace(begin, end, "[REDACTED:8]"); i = end; replaced = true; break;
            }
            if (replaced) break;
        }
        if (!replaced) ++i;
    }
    result.append(line.substr(copied));
    return result;
}

bool ProcessFileLogger::is_healthy() const {
    return impl_->is_healthy();
}

std::string redact_log_text(std::string_view line) {
    // Preserve the established TURN -> code -> secret precedence, including
    // overlapping fields such as "password=code ABCDE123". Each scan is linear.
    std::string value(line);
    for (unsigned stage = 0; stage != 3; ++stage)
        value = redact_log_grammar_stage(value, stage);
    return value;
}

bool remote_diagnostics_allowed_by_policy(
    bool release_build,
    bool local_opt_in) noexcept {
    return !release_build || local_opt_in;
}


bool is_sensitive_field_name(std::string_view field_name) {
    if (field_name.empty()) {
        return false;
    }

    const std::string normalized = normalize_ascii_lower(field_name);

    if (normalized == "session_id" || normalized == "operator_id" || normalized == "component") {
        return false;
    }

    return contains_sensitive_fragment(normalized);
}

std::string redact_field_value(std::string_view field_name, std::string_view value) {
    if (!is_sensitive_field_name(field_name)) {
        return std::string(value);
    }

    if (value.empty()) {
        return "[REDACTED]";
    }

    return "[REDACTED:" + std::to_string(value.size()) + "]";
}

std::unordered_map<std::string, std::string> redact_fields(
    const std::unordered_map<std::string, std::string>& fields) {
    std::unordered_map<std::string, std::string> redacted;
    redacted.reserve(fields.size());
    for (const auto& [name, value] : fields) {
        redacted.emplace(name, redact_field_value(name, value));
    }
    return redacted;
}

std::string format_structured_log_line(const StructuredLogEvent& event) {
    std::vector<std::pair<std::string, std::string>> stable_fields;
    stable_fields.reserve(event.fields.size());

    const auto redacted = redact_fields(event.fields);
    for (const auto& [name, value] : redacted) {
        stable_fields.emplace_back(name, value);
    }

    std::sort(
        stable_fields.begin(),
        stable_fields.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.first < rhs.first;
        });

    std::ostringstream out;
    out << "ts=" << event.timestamp_unix;
    out << " level=" << level_to_string(event.level);
    out << " component=" << event.component;
    out << " event=" << event.event;

    for (const auto& [name, value] : stable_fields) {
        out << ' ' << name << '=' << value;
    }

    return out.str();
}

std::string export_support_bundle(const SupportBundleRequest& request) {
    std::ostringstream out;
    out << "support_bundle_version=1" << '\n';
    out << "bundle_id=" << (request.bundle_id.empty() ? "unknown" : request.bundle_id) << '\n';
    out << "module=" << (request.module.empty() ? "unknown" : request.module) << '\n';
    out << "generated_at_unix=" << request.generated_at_unix << '\n';
    out << "structured_log_count=" << request.structured_logs.size() << '\n';
    out << "raw_log_count=" << request.raw_log_lines.size() << '\n';

    out << "[metadata]" << '\n';
    append_sorted_map(out, request.metadata, true);

    out << "[metrics]" << '\n';
    append_sorted_map(out, request.metrics, false);

    out << "[structured_logs]" << '\n';
    for (const auto& event : request.structured_logs) {
        out << format_structured_log_line(event) << '\n';
    }

    out << "[raw_logs]" << '\n';
    for (const auto& line : request.raw_log_lines) {
        out << line << '\n';
    }

    return out.str();
}

std::string export_connectivity_diagnostics_snapshot(const ConnectivityDiagnosticsSnapshot& snapshot) {
    std::ostringstream out;
    out << "connectivity_snapshot_version=1" << '\n';
    out << "session_id=" << (snapshot.session_id.empty() ? "unknown" : snapshot.session_id) << '\n';
    out << "peer_id=" << (snapshot.peer_id.empty() ? "unknown" : snapshot.peer_id) << '\n';
    out << "collected_at_unix=" << snapshot.collected_at_unix << '\n';
    out << "ice_state=" << ice_state_to_string(snapshot.ice_state) << '\n';
    out << "session_state=" << session_state_to_string(snapshot.session_state) << '\n';
    out << "local_candidate_count=" << snapshot.local_candidate_count << '\n';
    out << "remote_candidate_count=" << snapshot.remote_candidate_count << '\n';
    out << "reconnect_attempt_count=" << snapshot.reconnect_attempt_count << '\n';
    out << "recent_error_count=" << snapshot.recent_error_count << '\n';
    out << "last_error=" << (snapshot.last_error.empty() ? std::string() : "[REDACTED:" + std::to_string(snapshot.last_error.size()) + "]") << '\n';

    out << "[details]" << '\n';
    append_sorted_map(out, snapshot.details, true);
    return out.str();
}
}
