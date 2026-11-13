#pragma once
#if defined(_WIN32)
#include "playback_canvas.h"
#include "redclaw/helper/playback_state.h"
#include <cstdint>
#include <functional>
#include <memory>

namespace redclaw::ui {
struct DirectFrameTransportStats {
  quint64 playback_generation = 1;
  quint64 pause_discarded_sequences = 0;
  quint64 stale_decoded_outputs = 0;
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
  quint64 keyframe_requests_sent = 0;
  quint64 keyframe_requests_suppressed = 0;
  // Decoded frames dropped for display only; decode and references continued.
  quint64 surface_bridge_busy_drops = 0;
  quint64 surface_bridge_failures = 0;
  // 0 when decoding to CPU frames instead of the shared-surface bridge.
  quint64 decode_adapter_luid = 0;
  // Equals decode_adapter_luid while HEVC frames are hardware-decoded on the
  // presenter's adapter (shared textures or CPU transfer); 0 otherwise. HEVC
  // is never decoded on a different adapter.
  quint64 hevc_decode_adapter_luid = 0;
  // `present_adapter_surface`, `present_adapter_cpu` (decode stays on the
  // presenter's adapter after a bridge fallback) or `default_adapter_cpu`
  // (no decode device was configured).
  QString decode_route = "none";
  QString last_surface_bridge_error;
  // bind_flags=... ,vp_input_view=ok|unavailable once the shared textures exist.
  QString shared_texture_bind;
  QString decoder_backend = "unopened";
  QString last_decode_error;
  QString hardware_decode_fallback_from_backend;
  QString last_hardware_decode_error;
  redclaw::render::DecodedFrameBufferPoolTelemetry buffer_pool;
};

// Sole owner of mapping, event, decoder, decode device, worker and latest
// decoded frame. Destruction joins the worker before removing callbacks.
// Decode runs on its own D3D11 device; frames reach the presenter through
// keyed-mutex shared textures, so a stalled presenter never blocks decode.
class DirectFramePipeServer final {
public:
    DirectFramePipeServer();
    ~DirectFramePipeServer();
    DirectFramePipeServer(const DirectFramePipeServer&) = delete;
    DirectFramePipeServer& operator=(const DirectFramePipeServer&) = delete;
    bool start(const QString& pipe_name, QString* error_detail);
    // Creates the decode device on the presenter's adapter (LUID 0 = default
    // adapter). H.264 and HEVC both decode there. On failure decode falls
    // back to CPU-transfer frames and the reason is written to `detail`.
    // Call before start().
    bool configure_d3d11_decode_adapter(std::uint64_t adapter_luid, QString* detail);
    void disable_d3d11_surface_output();
    void request_cpu_decode_fallback();
    void stop();
    bool is_running() const;
    QString pipe_name() const;
    void set_on_frame_notify(std::function<void()> fn);
    void set_on_keyframe_request(std::function<void(QString, redclaw::helper::PlaybackStateSnapshot)> fn);
    void request_keyframe(const QString& reason);
    void set_playback_paused(bool paused, const QString& reason);
    redclaw::helper::PlaybackStateSnapshot playback_state() const;
    bool take_latest_frame(DirectFrameData* frame, quint64* frame_count);
    void recycle_frame(DirectFrameData frame);
    bool has_pending_frame() const;
    quint64 request_session_reset();
    DirectFrameTransportStats stats_snapshot() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace redclaw::ui
#endif
