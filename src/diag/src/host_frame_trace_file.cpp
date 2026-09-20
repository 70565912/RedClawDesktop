#include "redclaw/diag/host_frame_trace_file.h"
#include <fstream>
#include <string>

namespace redclaw::diag {
void write_host_frame_trace_csv(std::ostream& out, const net::MediaFrameTraceBatch& batch) {
    out << "# redclaw.host-frame-trace.v1 started_us=" << batch.started_us
        << " ended_us=" << batch.ended_us << " overflow=" << batch.overflow << '\n';
    out << "frame_id,rate_revision,capture_generation,capture_sequence,keyframe,outcome,width,height,target_fps,pacing_kbps,rtt_ms,"
        "capture_begin_us,capture_end_us,capture_ready_us,encode_begin_us,encode_end_us,enqueued_us,"
        "pacer_begin_us,first_send_us,last_send_us,finish_us,wire_bytes,sent_fragments,fragment_count,"
        "token_wait_us,in_flight_wait_us,buffered_wait_us,channel_wait_us,probe_wait_us,"
        "wait_requested_us,wait_elapsed_us,wait_overshoot_us,max_wait_overshoot_us,"
        "transport_state_us,send_call_us,max_send_call_us,callback_us\n";
    for (const auto& f : batch.frames) {
        out << f.frame_id << ',' << f.rate_revision << ',' << f.capture_generation << ',' << f.capture_sequence
            << ',' << f.keyframe << ',' << f.outcome << ',' << f.width << ',' << f.height << ',' << f.target_fps
            << ',' << f.pacing_kbps << ',' << f.rtt_ms << ',' << f.capture_begin_us << ',' << f.capture_end_us
            << ',' << f.capture_ready_us << ',' << f.encode_begin_us << ',' << f.encode_end_us << ',' << f.enqueued_us
            << ',' << f.pacer_begin_us << ',' << f.first_send_us << ',' << f.last_send_us << ',' << f.finish_us
            << ',' << f.wire_bytes << ',' << f.sent_fragments << ',' << f.fragment_count
            << ',' << f.token_wait_us << ',' << f.in_flight_wait_us << ',' << f.buffered_wait_us
            << ',' << f.channel_wait_us << ',' << f.probe_wait_us << ',' << f.wait_requested_us
            << ',' << f.wait_elapsed_us << ',' << f.wait_overshoot_us << ',' << f.max_wait_overshoot_us
            << ',' << f.transport_state_us << ',' << f.send_call_us << ',' << f.max_send_call_us
            << ',' << f.callback_us << '\n';
    }
}

HostFrameTraceFile::HostFrameTraceFile(net::MediaFrameTraceRecorder& recorder, std::filesystem::path runtime_log)
    : recorder_(recorder), runtime_log_(std::move(runtime_log)) {}

void HostFrameTraceFile::poll(std::uint64_t now_us) {
    if (runtime_log_.empty()) return;
    if (recording_) {
        auto batch = recorder_.take_completed(now_us);
        if (!batch) return;
        recording_ = false;
        auto destination = runtime_log_;
        destination += ".frame-trace-" + std::to_string(batch->started_us) + ".csv";
        std::error_code ec;
        if (std::filesystem::exists(destination, ec) || ec) return;
        std::ofstream out(destination, std::ios::binary);
        if (out) write_host_frame_trace_csv(out, *batch);
        return;
    }
    auto request = runtime_log_;
    request += ".frame-trace.request";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(request, ec) || ec) return;
    if (std::filesystem::file_size(request, ec) > 64 || ec) return;
    std::ifstream in(request);
    std::string schema, trailing;
    unsigned seconds = 0;
    const bool valid = (in >> schema >> seconds) && !(in >> trailing)
        && schema == "redclaw.host-frame-trace.v1" && seconds > 0 && seconds <= 120;
    in.close();
    if (!valid) return;
    auto consumed = request;
    consumed += ".consumed-" + std::to_string(now_us);
    std::filesystem::rename(request, consumed, ec);
    if (ec) return;
    recording_ = recorder_.arm(now_us, seconds);
}
} // namespace redclaw::diag
