#pragma once
#if defined(_WIN32)
#include "playback_canvas.h"
#include <functional>
#include <memory>

namespace redclaw::ui {
struct DirectFrameTransportStats {
  quint64 observed_sequences = 0;
  quint64 skipped_sequences = 0;
  quint64 published_frames = 0;
  quint64 decoded_frames = 0;
  quint64 decode_failures = 0;
  quint64 hardware_decode_runtime_fallbacks = 0;
  quint64 consecutive_decode_failures = 0;
  quint64 shared_memory_bgra_frames = 0;
  quint64 software_decode_bgra_frames = 0;
  quint64 hardware_decode_cpu_transfer_frames = 0;
  quint64 d3d11_decode_surface_frames = 0;
  quint64 d3d11_surface_fallbacks = 0;
  quint64 last_sequence = 0;
  quint64 latest_displayable_frame_id = 0;
  quint64 latest_displayable_keyframe_id = 0;
  redclaw::render::DecodedVideoFramePath last_output_path =
      redclaw::render::DecodedVideoFramePath::kUnknown;
  quint64 writer_frames = 0;
  quint64 writer_oversize_drops = 0;
  quint64 writer_busy_drops = 0;
  quint64 shared_snapshot_copies = 0;
  quint64 shared_snapshot_failures = 0;
  quint64 shared_snapshot_sequence_changes = 0;
  quint64 shared_snapshot_bytes = 0;
  quint64 shared_snapshot_total_us = 0;
  quint64 shared_snapshot_max_us = 0;
  quint64 shared_dependency_catchups = 0;
  quint64 shared_independent_skips = 0;
  quint64 shared_dependency_gaps = 0;
  QString decoder_backend = "unopened";
  QString last_decode_error;
  QString hardware_decode_fallback_from_backend;
  QString last_hardware_decode_error;
  redclaw::render::DecodedFrameBufferPoolTelemetry buffer_pool;
};

// Sole owner of mapping, event, decoder, worker and latest decoded frame.
// Destruction joins the worker before removing callbacks; no new queue/thread.
class DirectFramePipeServer final {
public:
    DirectFramePipeServer();
    ~DirectFramePipeServer();
    DirectFramePipeServer(const DirectFramePipeServer&) = delete;
    DirectFramePipeServer& operator=(const DirectFramePipeServer&) = delete;
    bool start(const QString& pipe_name, QString* error_detail);
    void configure_d3d11_surface_output(ID3D11Device* device);
    void request_cpu_decode_fallback();
    void stop();
    bool is_running() const;
    QString pipe_name() const;
    void set_on_frame_notify(std::function<void()> fn);
    void set_on_keyframe_request(std::function<void(QString)> fn);
    void request_keyframe(const QString& reason);
    bool take_latest_frame(DirectFrameData* frame, quint64* frame_count);
    void recycle_frame(DirectFrameData frame);
    bool has_pending_frame() const;
    void request_session_reset();
    DirectFrameTransportStats stats_snapshot() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace redclaw::ui
#endif
