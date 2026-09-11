#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>
#include <functional>
#include <memory>

#include "redclaw/diag/operation_timing.h"
#include "input_ack_timing.h"

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>
#include <QFile>
#include <QDataStream>

namespace redclaw::ui {

inline std::uint64_t gui_monotonic_us() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

enum class GuiStage : std::size_t {
    kEvent, kPaint, kLayout, kMetaCall, kTimer, kHeartbeatGap,
    kAgentReceive, kAgentApply, kAgentLayout, kAgentText,
    kStdoutDrain, kLogFormat, kLogSink, kLogWidget, kStatusWrite,
    kDispatchWait, kFrameHandler, kFrameLock, kPresentPrepare,
    kVideoBlt, kPresent, kGeometry, kFrameRelease,
    kInputBatchAck, kInputSyncAck, kInputAckLocalDelivery,
    kEventWait, kPresentInitialize, kPresentSteady,
    kInputBatchRuntimeAck, kInputSyncRuntimeAck, kInputBatchLocalDelivery, kInputSyncLocalDelivery,
    kDisplayedFrame,
    kLogLock, kOutputWrite, kFileWrite, kLogFlush, kStatusSerialize, kStatusCommit,
    kControlDrain, kRedraw, kPresentFailure,
    kAgentPaint, kLogPaint, kAgentCreate, kAgentRowLayout, kControlWrite,
    kGuiInitialize, kWidgetPolish,
    kFirstEventDispatch, kFirstHeartbeat, kControlStatusLocalDelivery,
    kInputStateSyncAck, kInputStateSyncRuntimeAck, kInputStateSyncLocalDelivery,
    kCount
};

// Fixed storage. Percentiles are upper bounds of quarter-octave microsecond
// buckets, capped by the exact observed maximum. No frame/text payload retained.
struct GuiDurationHistogram {
    std::array<std::uint64_t, 256> bins{};
    std::uint64_t count = 0, total_us = 0, max_us = 0;
    static std::size_t bucket(std::uint64_t us) {
        if (us == 0) return 0;
        const auto exponent = std::bit_width(us) - 1;
        const auto base = std::uint64_t{1} << exponent;
        const auto fraction = exponent >= 2 ? (us - base) >> (exponent - 2)
            : (us - base) * 4 / base;
        return std::min<std::size_t>(255, exponent * 4 + fraction);
    }
    void add(std::uint64_t us) {
        ++bins[bucket(us)]; ++count; total_us += us; max_us = std::max(max_us, us);
    }
    std::uint64_t percentile(unsigned percent) const {
        if (count == 0) return 0;
        const auto rank = (count * percent + 99) / 100;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < bins.size(); ++i) {
            seen += bins[i];
            if (seen < rank) continue;
            const auto base = std::uint64_t{1} << (i / 4);
            const auto step = std::max<std::uint64_t>(1, base / 4);
            const auto tail = step * (i % 4 + 1) - 1;
            const auto upper = tail > std::numeric_limits<std::uint64_t>::max() - base
                ? std::numeric_limits<std::uint64_t>::max() : base + tail;
            return std::min(max_us, upper);
        }
        return max_us;
    }
    QJsonObject json(bool include_bins = false) const {
        QJsonObject result{{"count", static_cast<qint64>(count)},
            {"total_us", static_cast<qint64>(total_us)},
            {"max_us", static_cast<qint64>(max_us)},
            {"p50_us", static_cast<qint64>(percentile(50))},
            {"p95_us", static_cast<qint64>(percentile(95))},
            {"p99_us", static_cast<qint64>(percentile(99))}};
        if (include_bins) {
            QJsonObject nonzero;
            for (std::size_t i = 0; i < bins.size(); ++i)
                if (bins[i]) nonzero.insert(QString::number(i), static_cast<qint64>(bins[i]));
            result.insert("histogram", nonzero);
        }
        return result;
    }
};

class GuiLatencyProbe {
public:
    bool tracing() const { return trace_active_; }
    enum class Queue : std::uint32_t { kControlMessages, kControlBytes, kDiagnosticBytes, kDiagnosticWriteBytes, kCount };
    void queue(Queue kind, std::uint64_t value) {
        auto& previous = queue_values_[static_cast<unsigned>(kind)];
        if (previous == value) return;
        previous = value;
        if (!trace_active_) return;
        if (queue_trace_.size() == kTraceCapacity) { ++trace_overflow_; return; }
        queue_trace_.push_back({static_cast<std::uint32_t>(kind), gui_monotonic_us(), value});
    }
    void present_failure(const QString& detail) {
        if (first_present_error_.isEmpty()) first_present_error_ = detail.left(8192);
    }
    void input_ack(const InputAckTiming& ack) {
        if (!ack.valid()) { ++invalid_ack_timings_; return; }
        input_acks_[input_ack_count_++ % input_acks_.size()] = ack;
        const bool batch = ack.kind == InputAckKind::kBatch;
        const bool empty = ack.kind == InputAckKind::kEmptySync;
        record(batch ? GuiStage::kInputBatchAck : empty ? GuiStage::kInputSyncAck : GuiStage::kInputStateSyncAck,
            ack.consumed_us - ack.sent_us, 0, ack.sent_us, ack.consumed_us);
        record(batch ? GuiStage::kInputBatchRuntimeAck : empty ? GuiStage::kInputSyncRuntimeAck : GuiStage::kInputStateSyncRuntimeAck,
            ack.received_us - ack.sent_us, 0, ack.sent_us, ack.received_us);
        record(batch ? GuiStage::kInputBatchLocalDelivery : empty ? GuiStage::kInputSyncLocalDelivery : GuiStage::kInputStateSyncLocalDelivery,
            ack.consumed_us - ack.received_us, 0, ack.received_us, ack.consumed_us);
        record(GuiStage::kInputAckLocalDelivery, ack.consumed_us - ack.received_us, 0, ack.received_us, ack.consumed_us);
    }
    void record(GuiStage stage, std::uint64_t us, std::uint64_t cpu_us = 0,
        std::uint64_t begin_us = 0, std::uint64_t end_us = 0) {
        const auto i = static_cast<std::size_t>(stage);
        window_[i].add(us); totals_[i].add(us);
        cpu_window_[i].add(cpu_us); cpu_totals_[i].add(cpu_us);
        if (task_active_) task_stages_[i].add(us);
        if (trace_active_) {
            if (trace_.size() == kTraceCapacity) { ++trace_overflow_; return; }
            if (!end_us) end_us = gui_monotonic_us();
            if (!begin_us) begin_us = end_us >= us ? end_us - us : 0;
            trace_.push_back({static_cast<std::uint32_t>(i), begin_us, end_us, cpu_us});
        }
    }
    void arm_trace() {
        trace_active_ = false;
        trace_.clear(); trace_.reserve(kTraceCapacity); trace_overflow_ = 0; exported_trace_count_ = 0;
        queue_trace_.clear(); queue_trace_.reserve(kTraceCapacity);
        trace_started_us_ = gui_monotonic_us(); trace_active_ = true;
        for (unsigned i = 0; i < queue_values_.size(); ++i) queue_trace_.push_back({i, trace_started_us_, queue_values_[i]});
    }
    bool export_trace(const QString& path, QString* error) {
        return trace_export_work(path)(error);
    }
    std::function<bool(QString*)> trace_export_work(const QString& path) {
        trace_active_ = false;
        exported_trace_count_ = trace_.size();
        return [path, samples = std::move(trace_), queues = std::move(queue_trace_), overflow = trace_overflow_, start = trace_started_us_](QString* error) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            if (error) *error = file.errorString();
            return false;
        }
        QDataStream stream(&file); stream.setByteOrder(QDataStream::LittleEndian);
        stream.writeRawData("RCDTRC01", 8);
        stream << quint32(samples.size()) << quint64(overflow) << quint64(start);
        for (const auto& sample : samples)
            stream << quint32(sample.stage) << quint64(sample.begin_us) << quint64(sample.end_us) << quint64(sample.cpu_us);
        bool ok = stream.status() == QDataStream::Ok && file.flush();
        if (!ok && error) *error = file.errorString();
        QFile queue_file(path + ".queues.bin");
        if (!queue_file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { if (error) *error = queue_file.errorString(); return false; }
        QDataStream output(&queue_file); output.setByteOrder(QDataStream::LittleEndian);
        output.writeRawData("RCDQUE01", 8); output << quint32(queues.size()) << quint64(overflow) << quint64(start);
        for (const auto& sample : queues) output << quint32(sample.kind) << quint64(sample.at_us) << quint64(sample.value);
        ok = ok && output.status() == QDataStream::Ok && queue_file.flush();
        if (!ok && error && error->isEmpty()) *error = queue_file.errorString();
        return ok;
        };
    }
    static void observe_diagnostic(void* context, redclaw::diag::DiagnosticOperation operation,
        const redclaw::diag::OperationTimingSample& sample) {
        constexpr GuiStage mapping[]{GuiStage::kLogLock, GuiStage::kOutputWrite,
            GuiStage::kFileWrite, GuiStage::kLogFlush};
        if (static_cast<unsigned>(operation) >= std::size(mapping)) return;
        static_cast<GuiLatencyProbe*>(context)->record(mapping[static_cast<unsigned>(operation)],
            sample.end_us - sample.begin_us, sample.cpu_us, sample.begin_us, sample.end_us);
    }
    void begin_task(const QString& id) {
        task_id_ = id; task_start_us_ = gui_monotonic_us(); task_end_us_ = 0;
        task_stages_ = {}; task_active_ = true;
    }
    void finish_task(const QString& id) {
        if (task_active_ && task_id_ == id) { task_end_us_ = gui_monotonic_us(); task_active_ = false; }
    }
    QJsonObject task_snapshot(const QString& id) const {
        if (id != task_id_ || !task_start_us_) return {};
        QJsonObject stages;
        for (std::size_t i = 0; i < task_stages_.size(); ++i)
            if (task_stages_[i].count) stages.insert(QString::fromLatin1(names_[i]), task_stages_[i].json());
        const auto end = task_end_us_ ? task_end_us_ : gui_monotonic_us();
        const auto seconds = (end - task_start_us_) / 1000000.0;
        return {{"boundary", "first_gui_output_consumption_to_terminal_label_paint"},
            {"start_us", static_cast<qint64>(task_start_us_)}, {"end_us", static_cast<qint64>(end)},
            {"closed", !task_active_}, {"stages", stages},
            {"present_fps", seconds > 0 ? task_stages_[static_cast<std::size_t>(GuiStage::kDisplayedFrame)].count / seconds : 0}};
    }
    void slow_event(int type, const char* class_name, std::uint64_t us) {
        if (us <= slowest_event_us_) return;
        slowest_event_us_ = us;
        slowest_event_type_ = type;
        slowest_event_class_ = QString::fromLatin1(class_name);
    }
    void begin_event_loop(std::uint64_t now = gui_monotonic_us()) {
        if (!event_loop_at_us_) event_loop_at_us_ = now;
    }
    void event_dispatched(std::uint64_t now = gui_monotonic_us()) {
        if (!event_loop_at_us_ || first_dispatch_us_ || now < event_loop_at_us_) return;
        first_dispatch_us_ = now;
        record(GuiStage::kFirstEventDispatch, now - event_loop_at_us_, 0, event_loop_at_us_, now);
    }
    void heartbeat(std::uint64_t now = gui_monotonic_us()) {
        if (!event_loop_at_us_ || now < event_loop_at_us_) return;
        if (!heartbeat_at_us_) {
            first_heartbeat_us_ = now;
            record(GuiStage::kFirstHeartbeat, now - event_loop_at_us_, 0, event_loop_at_us_, now);
        } else if (now >= heartbeat_at_us_) {
            record(GuiStage::kHeartbeatGap, now - heartbeat_at_us_, 0, heartbeat_at_us_, now);
        }
        heartbeat_at_us_ = now;
    }
    void polished(const QString& owner, std::uint64_t begin, std::uint64_t end, std::uint64_t cpu) {
        if (end - begin <= slowest_polish_us_) return;
        slowest_polish_us_ = end - begin;
        slowest_polish_ = {{"owner", owner}, {"begin_us", qint64(begin)}, {"end_us", qint64(end)},
            {"wall_us", qint64(end - begin)}, {"cpu_us", qint64(cpu)}};
    }
    QJsonObject close_window() {
        const auto now = gui_monotonic_us();
        QJsonObject stages;
        QJsonObject cpu_stages;
        for (std::size_t i = 0; i < window_.size(); ++i)
            if (window_[i].count) {
                stages.insert(QString::fromLatin1(names_[i]), window_[i].json());
                cpu_stages.insert(QString::fromLatin1(names_[i]), cpu_window_[i].json());
            }
        last_window_ = {{"id", static_cast<qint64>(++window_id_)},
            {"start_us", static_cast<qint64>(window_start_us_)},
            {"end_us", static_cast<qint64>(now)}, {"stages", stages}, {"cpu_stages", cpu_stages},
            {"slowest_event_type", slowest_event_type_},
            {"slowest_event_class", slowest_event_class_},
            {"slowest_event_us", static_cast<qint64>(slowest_event_us_)}};
        window_ = {}; cpu_window_ = {}; window_start_us_ = now; slowest_event_us_ = 0;
        slowest_event_type_ = 0; slowest_event_class_.clear();
        return last_window_;
    }
    // Copy only bounded statistics on the GUI; build diagnostic JSON on the
    // writer. Detailed trace payloads never accompany ordinary status updates.
    std::function<QJsonObject()> snapshot_work() const {
        auto frozen = std::make_shared<GuiLatencyProbe>();
        frozen->totals_ = totals_; frozen->cpu_totals_ = cpu_totals_;
        frozen->last_window_ = last_window_; frozen->input_acks_ = input_acks_;
        frozen->input_ack_count_ = input_ack_count_; frozen->first_present_error_ = first_present_error_;
        frozen->slowest_polish_ = slowest_polish_;
        frozen->probe_created_us_ = probe_created_us_; frozen->event_loop_at_us_ = event_loop_at_us_;
        frozen->first_dispatch_us_ = first_dispatch_us_; frozen->first_heartbeat_us_ = first_heartbeat_us_;
        frozen->invalid_ack_timings_ = invalid_ack_timings_;
        frozen->trace_active_ = false;
        frozen->exported_trace_count_ = trace_active_ ? trace_.size() : exported_trace_count_;
        frozen->trace_overflow_ = trace_overflow_; frozen->trace_started_us_ = trace_started_us_;
        const auto sampled = gui_monotonic_us(); const bool active = trace_active_;
        return [frozen, sampled, active] {
            auto object = frozen->snapshot(); object.insert("sampled_at_us", qint64(sampled));
            auto trace = object["trace"].toObject(); trace.insert("active", active); object.insert("trace", trace);
            return object;
        };
    }
    QJsonObject snapshot() const {
        QJsonObject stages;
        QJsonObject cpu_stages;
        QJsonArray stage_names;
        for (std::size_t i = 0; i < totals_.size(); ++i)
            if (totals_[i].count) {
                stages.insert(QString::fromLatin1(names_[i]), totals_[i].json(true));
                cpu_stages.insert(QString::fromLatin1(names_[i]), cpu_totals_[i].json());
            }
        for (const auto* name : names_) stage_names.append(QString::fromLatin1(name));
        QJsonArray acknowledgements;
        const auto count = std::min<std::uint64_t>(input_ack_count_, input_acks_.size());
        for (auto i = input_ack_count_ - count; i < input_ack_count_; ++i) {
            const auto& ack = input_acks_[i % input_acks_.size()];
            acknowledgements.append(QJsonObject{{"sequence", static_cast<qint64>(ack.sequence)},
                {"kind", ack.kind == InputAckKind::kBatch ? "batch" : ack.kind == InputAckKind::kEmptySync ? "empty_sync" : "state_sync"},
                {"sent_us", static_cast<qint64>(ack.sent_us)},
                {"runtime_received_us", static_cast<qint64>(ack.received_us)},
                {"gui_consumed_us", static_cast<qint64>(ack.consumed_us)}});
        }
        return {{"schema", "redclaw.gui-scheduling.v2"},
            {"clock", "steady_microseconds"}, {"percentiles", "quarter_octave_upper_bound"},
            {"sampled_at_us", static_cast<qint64>(gui_monotonic_us())},
            {"last_window", last_window_}, {"totals", stages}, {"recent_input_acks", acknowledgements},
            {"cpu_totals", cpu_stages}, {"cpu_clock", "thread_user_plus_kernel_microseconds"},
            {"startup", QJsonObject{{"probe_created_us", qint64(probe_created_us_)},
                {"event_loop_entered_us", qint64(event_loop_at_us_)}, {"first_dispatch_us", qint64(first_dispatch_us_)},
                {"first_heartbeat_us", qint64(first_heartbeat_us_)},
                {"construction_to_first_heartbeat_us", first_heartbeat_us_ >= probe_created_us_
                    ? QJsonValue(qint64(first_heartbeat_us_ - probe_created_us_)) : QJsonValue()}}},
            {"trace", QJsonObject{{"active", trace_active_}, {"count", qint64(trace_active_ ? trace_.size() : exported_trace_count_)},
                {"metric_contract", "redclaw.gui-timing.v2"}, {"invalid_ack_timings", qint64(invalid_ack_timings_)},
                {"capacity", qint64(kTraceCapacity)}, {"overflow", qint64(trace_overflow_)},
                {"stage_names", stage_names}, {"start_us", qint64(trace_started_us_)}}},
            {"first_present_error", first_present_error_}, {"slowest_widget_polish", slowest_polish_}};
    }
private:
    struct QueueSample { std::uint32_t kind; std::uint64_t at_us, value; };
    std::array<std::uint64_t, static_cast<unsigned>(Queue::kCount)> queue_values_{};
    std::vector<QueueSample> queue_trace_;
    static constexpr std::array<const char*, static_cast<std::size_t>(GuiStage::kCount)> names_{
        "event", "paint", "layout", "meta_call", "timer", "heartbeat_gap",
        "agent_receive", "agent_apply", "agent_layout", "agent_text", "stdout_drain",
        "log_format", "log_sink", "log_widget", "status_write", "dispatch_wait",
        "frame_handler", "frame_lock", "present_prepare", "video_blt", "present",
        "geometry", "frame_release", "input_batch_ack", "input_sync_ack", "input_ack_local_delivery",
        "event_wait", "present_initialize", "present_steady", "input_batch_runtime_ack", "input_sync_runtime_ack",
        "input_batch_local_delivery", "input_sync_local_delivery", "displayed_frame",
        "log_lock", "output_write", "file_write", "log_flush", "status_serialize", "status_commit",
        "control_drain", "redraw", "present_failure",
        "agent_paint", "log_paint", "agent_create", "agent_row_layout", "control_write",
        "gui_initialize", "widget_polish", "startup_first_dispatch", "startup_first_heartbeat", "control_status_local_delivery",
        "input_state_sync_ack", "input_state_sync_runtime_ack", "input_state_sync_local_delivery"};
    struct TraceSample { std::uint32_t stage; std::uint64_t begin_us, end_us, cpu_us; };
    static constexpr std::size_t kTraceCapacity = 262144;
    std::vector<TraceSample> trace_;
    std::size_t exported_trace_count_ = 0;
    bool trace_active_ = false;
    std::uint64_t trace_overflow_ = 0, trace_started_us_ = 0;
    std::array<InputAckTiming, 32> input_acks_{};
    std::uint64_t input_ack_count_ = 0;
    std::uint64_t invalid_ack_timings_ = 0;
    std::array<GuiDurationHistogram, static_cast<std::size_t>(GuiStage::kCount)> window_{}, totals_{};
    std::array<GuiDurationHistogram, static_cast<std::size_t>(GuiStage::kCount)> cpu_window_{}, cpu_totals_{};
    std::array<GuiDurationHistogram, static_cast<std::size_t>(GuiStage::kCount)> task_stages_{};
    QString task_id_;
    QString first_present_error_;
    QJsonObject slowest_polish_;
    std::uint64_t slowest_polish_us_ = 0;
    std::uint64_t task_start_us_ = 0, task_end_us_ = 0;
    bool task_active_ = false;
    std::uint64_t window_start_us_ = gui_monotonic_us(), probe_created_us_ = window_start_us_;
    std::uint64_t event_loop_at_us_ = 0, first_dispatch_us_ = 0, first_heartbeat_us_ = 0, heartbeat_at_us_ = 0;
    std::uint64_t window_id_ = 0, slowest_event_us_ = 0;
    int slowest_event_type_ = 0;
    QString slowest_event_class_;
    QJsonObject last_window_;
};

class MeasuredGuiApplication : public QApplication {
public:
    MeasuredGuiApplication(int& argc, char** argv) : QApplication(argc, argv) {
        previous_observer_ = redclaw::diag::set_thread_timing_observer({&latency, &GuiLatencyProbe::observe_diagnostic});
        auto* dispatcher = QAbstractEventDispatcher::instance(thread());
        QObject::connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this,
            [this] { waiting_at_us_ = gui_monotonic_us(); });
        QObject::connect(dispatcher, &QAbstractEventDispatcher::awake, this, [this] {
            // This includes legitimate idle sleep; it is not GUI CPU time.
            if (waiting_at_us_) latency.record(GuiStage::kEventWait, gui_monotonic_us() - waiting_at_us_);
            waiting_at_us_ = 0;
        });
    }
    ~MeasuredGuiApplication() override { redclaw::diag::set_thread_timing_observer(previous_observer_); }
    GuiLatencyProbe latency;
    bool notify(QObject* receiver, QEvent* event) override {
        if (!receiver || !event || QThread::currentThread() != thread()) return QApplication::notify(receiver, event);
        const bool outer = depth_++ == 0;
        if (outer) latency.event_dispatched();
        const auto type = event->type();
        QString polish_owner;
        if (type == QEvent::Polish) {
            // Construction-only metadata, copied before notify can delete the object.
            // Never retain widget text or user input in timing evidence.
            auto* ancestor = receiver;
            for (unsigned i = 0; ancestor && i < 4; ++i, ancestor = ancestor->parent()) {
                if (i) polish_owner += QStringLiteral(" <- ");
                polish_owner += QString::fromLatin1(ancestor->metaObject()->className())
                    + ':' + ancestor->objectName().left(80);
            }
        }
        GuiStage paint_owner = GuiStage::kCount;
        if (type == QEvent::Paint && latency.tracing()) {
            // Classify only QA paints by stable widget identifiers. Do this
            // before notify, which may destroy the receiver and its parents.
            auto* ancestor = receiver;
            for (unsigned i = 0; ancestor && i < 5; ++i, ancestor = ancestor->parent()) {
                const auto name = ancestor->objectName();
                if (name == QStringLiteral("agentReplyText")) {
                    paint_owner = GuiStage::kAgentPaint; break;
                }
                if (name == QStringLiteral("runtimeSessionLog")) {
                    paint_owner = GuiStage::kLogPaint; break;
                }
            }
        }
        // notify may delete the receiver. Copy only static class metadata first.
        const char* class_name = receiver->metaObject()->className();
        const auto begin = gui_monotonic_us();
        const auto cpu_begin = redclaw::diag::current_thread_cpu_us();
        const bool result = QApplication::notify(receiver, event);
        const auto end = gui_monotonic_us();
        const auto duration = end - begin;
        const auto cpu_end = redclaw::diag::current_thread_cpu_us();
        const auto cpu = cpu_end >= cpu_begin ? cpu_end - cpu_begin : 0;
        --depth_;
        if (outer) {
            latency.record(GuiStage::kEvent, duration, cpu, begin, end);
            latency.slow_event(static_cast<int>(type), class_name, duration);
        }
        switch (type) {
        case QEvent::Paint:
            latency.record(GuiStage::kPaint, duration, cpu, begin, end);
            if (paint_owner != GuiStage::kCount) latency.record(paint_owner, duration, cpu, begin, end);
            break;
        case QEvent::LayoutRequest: latency.record(GuiStage::kLayout, duration, cpu, begin, end); break;
        case QEvent::MetaCall: latency.record(GuiStage::kMetaCall, duration, cpu, begin, end); break;
        case QEvent::Timer: latency.record(GuiStage::kTimer, duration, cpu, begin, end); break;
        case QEvent::Polish:
            latency.record(GuiStage::kWidgetPolish, duration, cpu, begin, end);
            latency.polished(polish_owner, begin, end, cpu);
            break;
        default: break;
        }
        return result;
    }
private:
    redclaw::diag::ThreadTimingObserver previous_observer_;
    unsigned depth_ = 0;
    std::uint64_t waiting_at_us_ = 0;
};

inline GuiLatencyProbe* gui_latency_probe() {
    auto* application = dynamic_cast<MeasuredGuiApplication*>(QCoreApplication::instance());
    return application && QThread::currentThread() == application->thread() ? &application->latency : nullptr;
}

class GuiLatencyScope {
public:
    explicit GuiLatencyScope(GuiStage stage) : probe_(gui_latency_probe()), stage_(stage) {}
    ~GuiLatencyScope() { finish(); }
    void finish() {
        if (probe_) {
            const auto end = gui_monotonic_us();
            const auto cpu = redclaw::diag::current_thread_cpu_us();
            probe_->record(stage_, end - begin_, cpu >= cpu_begin_ ? cpu - cpu_begin_ : 0, begin_, end);
            probe_ = nullptr;
        }
    }
private:
    GuiLatencyProbe* probe_;
    GuiStage stage_;
    std::uint64_t begin_ = gui_monotonic_us();
    std::uint64_t cpu_begin_ = probe_ ? redclaw::diag::current_thread_cpu_us() : 0;
};
} // namespace redclaw::ui
