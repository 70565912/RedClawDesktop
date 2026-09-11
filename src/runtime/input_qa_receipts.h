#pragma once
#include "redclaw/input/input_module.h"
#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/diag/operation_timing.h"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <filesystem>
#include <fstream>

namespace redclaw::runtime {

enum class InputQaStage {
    kLocalRead, kSendBegin, kSendLocked, kSendEncoded, kSendEnd,
    kPeerReceived, kPeerEnqueued, kHostConsume,
    kLoopSleep, kBeforeLocalInput, kLocalInput, kBeforeHostInput, kHostInput,
    kAgentPump, kAfterAgent, kMaintenance, kAdaptation, kOtherSignaling,
    kDht, kNegotiation, kPeriodicStats, kLoopTail,
    kLogLock, kOutputWrite, kFileWrite, kFlush, kDhtListenPort
};

// Explicit Debug fixture telemetry. Injection only appends bounded metadata;
// a requested export writes on this worker after the measurement window ends.
class InputQaReceipts final {
public:
    static constexpr std::size_t kCapacity = 32768;
    static constexpr std::size_t kStageCapacity = 262144;
    InputQaReceipts(bool enabled, std::filesystem::path directory) : enabled_(enabled), directory_(std::move(directory)) {
        if (!enabled_) return;
        active_.reserve(kCapacity); pending_.reserve(kCapacity);
        stages_.reserve(kStageCapacity); pending_stages_.reserve(kStageCapacity);
        worker_ = std::thread([this] { run(); });
    }
    ~InputQaReceipts() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        wake_.notify_one(); if (worker_.joinable()) worker_.join();
    }
    void record(const redclaw::input::InputInjectionReceipt& receipt) {
        if (!enabled_) return;
        std::lock_guard lock(mutex_);
        if (active_.size() == kCapacity) ++overflow_;
        else active_.push_back(receipt);
    }
    // Only the explicit fixture's consented input intervals record detailed stages.
    void command(InputQaStage stage, const redclaw::protocol::StreamControlMessageV1& message,
                 std::uint64_t at_us = 0) {
        using Type = redclaw::protocol::StreamControlMessageTypeV1;
        if (!enabled_) return;
        if (message.type == Type::kInputControlRequest && message.input_requested_active)
            recording_ = true;
        if (message.type != Type::kInputBatch && message.type != Type::kInputStateSync) return;
        append(stage, message.input_sequence, at_us ? at_us : redclaw::diag::monotonic_time_us(), 0, 0);
    }
    class LoopTiming final {
    public:
        explicit LoopTiming(InputQaReceipts& owner) : owner_(owner) {
            if (owner_.recording_) {
                observing_ = true;
                previous_ = redclaw::diag::set_thread_timing_observer({&owner_, [](void* context,
                    redclaw::diag::DiagnosticOperation operation, const redclaw::diag::OperationTimingSample& sample) {
                    // Keep the many uncontended stream insertions out of the bounded
                    // trace. Slow nested logging samples supplement the loop spans.
                    if (sample.end_us - sample.begin_us >= 1000)
                        static_cast<InputQaReceipts*>(context)->append(static_cast<InputQaStage>(
                            static_cast<unsigned>(InputQaStage::kLogLock) + static_cast<unsigned>(operation)),
                            0, sample.begin_us, sample.end_us, sample.cpu_us);
                }});
            }
            next(InputQaStage::kLoopSleep);
        }
        ~LoopTiming() {
            finish();
            if (observing_) redclaw::diag::set_thread_timing_observer(previous_);
        }
        void next(InputQaStage stage) {
            finish(); stage_ = stage;
            if (owner_.recording_) {
                begin_ = redclaw::diag::monotonic_time_us(); cpu_ = redclaw::diag::current_thread_cpu_us();
            }
        }
    private:
        void finish() {
            if (!begin_) return;
            const auto end = redclaw::diag::monotonic_time_us();
            const auto cpu = redclaw::diag::current_thread_cpu_us();
            owner_.append(stage_, 0, begin_, end, cpu >= cpu_ ? cpu - cpu_ : 0); begin_ = 0;
        }
        InputQaReceipts& owner_;
        redclaw::diag::ThreadTimingObserver previous_;
        bool observing_ = false;
        InputQaStage stage_ = InputQaStage::kLoopSleep;
        std::uint64_t begin_ = 0, cpu_ = 0;
    };
    bool request_export(std::uint64_t generation) {
        std::lock_guard lock(mutex_);
        if (!enabled_ || pending_export_ || exporting_) return false;
        active_.swap(pending_); pending_overflow_ = overflow_; overflow_ = 0;
        stages_.swap(pending_stages_); pending_stage_overflow_ = stage_overflow_; stage_overflow_ = 0;
        recording_ = false;
        generation_ = generation; pending_export_ = true; wake_.notify_one(); return true;
    }
private:
    struct StageSample { InputQaStage stage; std::uint64_t sequence, begin_us, end_us, cpu_us; };
    void append(InputQaStage stage, std::uint64_t sequence, std::uint64_t begin, std::uint64_t end, std::uint64_t cpu) {
        if (!recording_) return;
        std::lock_guard lock(mutex_);
        if (!recording_) return;
        if (stages_.size() == kStageCapacity) ++stage_overflow_;
        else stages_.push_back({stage, sequence, begin, end ? end : begin, cpu});
    }
    void run() {
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || pending_export_; });
                if (!pending_export_) return;
                pending_export_ = false; exporting_ = true;
            }
            const bool written = export_file();
            const auto complete = "QA input receipt export generation=" + std::to_string(generation_)
                + " count=" + std::to_string(pending_.size()) + " overflow=" + std::to_string(pending_overflow_)
                + " complete=" + (written ? "true\n" : "false\n");
            std::cout.write(complete.data(), static_cast<std::streamsize>(complete.size())); std::cout.flush();
            {
                std::lock_guard lock(mutex_); pending_.clear(); pending_stages_.clear(); exporting_ = false;
            }
        }
    }
    template<class Write>
    static bool write_atomically(const std::filesystem::path& path, Write write) {
        auto pending_path = path; pending_path += ".tmp";
        std::ofstream output(pending_path, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        write(output); output.flush();
        if (!output.good()) return false;
        output.close(); if (output.fail()) return false;
        std::error_code error; std::filesystem::rename(pending_path, path, error);
        return !error;
    }
    bool export_file() const {
        try {
            const auto stage_name = "input-stages-" + std::to_string(generation_) + ".json";
            if (!write_atomically(directory_ / stage_name, [this](std::ostream& output) {
                output << "{\"schema\":\"redclaw.qa-input-stages-raw.v1\",\"generation\":" << generation_
                       << ",\"stage_overflow\":" << pending_stage_overflow_ << ",\"stages\":[";
                bool first = true;
                for (const auto& s : pending_stages_) {
                    if (!first) output << ','; first = false;
                    output << "{\"stage\":" << static_cast<unsigned>(s.stage) << ",\"sequence\":" << s.sequence
                           << ",\"begin_us\":" << s.begin_us << ",\"end_us\":" << s.end_us
                           << ",\"cpu_us\":" << s.cpu_us << '}';
                }
                output << "]}";
            })) return false;
            // Publish the small receipt/manifest last. Existing receipt analysis
            // need not parse every runtime loop sample on each measurement window.
            const auto path = directory_ / ("input-injections-" + std::to_string(generation_) + ".json");
            return write_atomically(path, [this, &stage_name](std::ostream& output) {
            output << "{\"schema\":\"redclaw.qa-input-injections.v1\",\"generation\":" << generation_
                << ",\"overflow\":" << pending_overflow_ << ",\"receipts\":[";
            bool first = true;
            for (const auto& r : pending_) {
                if (!first) output << ','; first = false;
                output << "{\"sequence\":" << r.sequence << ",\"type\":" << static_cast<unsigned>(r.type)
                    << ",\"begin_us\":" << r.begin_us << ",\"end_us\":" << r.end_us
                    << ",\"injected\":" << (r.injected ? "true" : "false") << '}';
            }
            output << "],\"stage_overflow\":" << pending_stage_overflow_
                   << ",\"stage_count\":" << pending_stages_.size() << ",\"stage_file\":\"" << stage_name << "\"}";
            });
        } catch (...) { return false; }
    }
    bool enabled_, stopping_ = false, pending_export_ = false, exporting_ = false;
    std::filesystem::path directory_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<redclaw::input::InputInjectionReceipt> active_, pending_;
    std::vector<StageSample> stages_, pending_stages_;
    std::atomic_bool recording_{false};
    std::uint64_t stage_overflow_ = 0, pending_stage_overflow_ = 0;
    std::uint64_t generation_ = 0, overflow_ = 0, pending_overflow_ = 0;
    std::thread worker_;
};

}  // namespace redclaw::runtime
