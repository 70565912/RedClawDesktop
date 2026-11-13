#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "direct_frame_receiver.h"
#include "redclaw/helper/direct_frame_shared_memory.h"
#include "redclaw/protocol/hevc_access_unit.h"
#include "redclaw/render/d3d11_video_device.h"
#include "redclaw/render/playback_flow_control.h"
#include <QByteArray>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include "ui/gui_latency_probe.h"
#include <thread>
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

namespace redclaw::ui {
namespace {
std::uint64_t steady_now_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace

class DirectFramePipeServer::Impl final {
 public:
  ~Impl() {
    stop();
  }

  bool start(const QString& pipe_name, QString* error_detail) {
    stop();

    redclaw::helper::DirectFrameSharedSnapshot prepared_snapshot;
    if (!redclaw::helper::prepare_direct_frame_shared_snapshot(&prepared_snapshot)) {
      if (error_detail != nullptr) {
        *error_detail = "Unable to allocate the bounded shared-frame snapshot buffer.";
      }
      return false;
    }

    const std::string channel_name = pipe_name.toStdString();
    const std::wstring mapping_name = redclaw::helper::direct_frame_shared_mapping_name(channel_name);
    const std::size_t mapping_size = redclaw::helper::direct_frame_shared_mapping_size();
    HANDLE mapping_handle = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        static_cast<DWORD>(mapping_size >> 32),
        static_cast<DWORD>(mapping_size & 0xffffffffULL),
        mapping_name.c_str());
    if (mapping_handle == nullptr) {
      if (error_detail != nullptr) {
        *error_detail = QString("CreateFileMapping failed: %1").arg(GetLastError());
      }
      return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(mapping_handle);
      if (error_detail != nullptr) {
        *error_detail = "CreateFileMapping returned an existing frame channel name.";
      }
      return false;
    }

    void* mapping_view = MapViewOfFile(mapping_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (mapping_view == nullptr) {
      const DWORD last_error = GetLastError();
      CloseHandle(mapping_handle);
      if (error_detail != nullptr) {
        *error_detail = QString("MapViewOfFile failed: %1").arg(last_error);
      }
      return false;
    }

    const std::wstring event_name = redclaw::helper::direct_frame_shared_event_name(channel_name);
    HANDLE data_event = CreateEventW(nullptr, FALSE, FALSE, event_name.c_str());
    if (data_event == nullptr) {
      const DWORD last_error = GetLastError();
      UnmapViewOfFile(mapping_view);
      CloseHandle(mapping_handle);
      if (error_detail != nullptr) {
        *error_detail = QString("CreateEvent failed: %1").arg(last_error);
      }
      return false;
    }

    auto* shared_header = static_cast<redclaw::helper::DirectFrameSharedMemoryHeader*>(mapping_view);
    redclaw::helper::initialize_direct_frame_shared_memory(shared_header);
    for (quint32 slot_index = 0; slot_index < shared_header->slot_count; ++slot_index) {
      auto* slot_header = redclaw::helper::direct_frame_shared_slot_header(mapping_view, slot_index);
      redclaw::helper::direct_frame_atomic_store_i64(&slot_header->committed_sequence, 0);
      std::memset(&slot_header->frame, 0, sizeof(slot_header->frame));
    }

    {
      std::lock_guard<std::mutex> lock(handle_mutex_);
      mapping_handle_ = mapping_handle;
      mapping_view_ = mapping_view;
      shared_header_ = shared_header;
      data_event_ = data_event;
    }
    pipe_name_ = pipe_name;
    shared_snapshot_ = std::move(prepared_snapshot);

    decoder_reference_.invalidate();
    session_reset_requested_.store(false);
    playback_generation_.store(1);
    decoded_cpu_output_observed_ = false;
    decoder_.reset();
    frame_buffer_pool_.clear();
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      latest_frame_ = DirectFrameData{};
      latest_frame_count_ = 0;
      delivered_frame_count_ = 0;
    }
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_ = DirectFrameTransportStats{};
    }
    observed_sequence_ = 0;
    consecutive_decode_failures_ = 0;
    decoded_encoded_frames_ = 0;
    {
      std::lock_guard<std::mutex> lock(keyframe_request_mutex_);
      keyframe_limiter_.reset();
    }
    {
      std::lock_guard lock(frame_mutex_);
      playback_state_ = {.session = ++playback_session_counter_};
      desired_paused_ = false;
      playback_reason_ = "initial";
      worker_revision_ = 0;
      publish_direct_frame_playback_state(shared_header_, playback_state_);
    }
    running_.store(true);
    worker_ = std::thread([this]() {
      run();
    });
    return true;
  }

  bool configure_d3d11_decode_adapter(std::uint64_t adapter_luid, QString* detail) {
    disable_d3d11_surface_output();
    std::string error;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    device.Attach(redclaw::render::create_d3d11_video_device(adapter_luid, &error));
    if (!device) {
      if (detail != nullptr) *detail = QString::fromStdString(error);
      return false;
    }
    decode_device_ = device;
    surface_bridge_ = std::make_unique<redclaw::render::D3D11SharedSurfaceBridge>(device.Get());
    prefer_d3d11_surface_output_ = true;
    decoder_.configure_d3d11_surface_output(device.Get());
    decode_adapter_luid_.store(redclaw::render::d3d11_device_adapter_luid(device.Get()));
    return true;
  }

  void disable_d3d11_surface_output() {
    cpu_decode_fallback_requested_.store(false);
    prefer_d3d11_surface_output_ = false;
    decoded_cpu_output_observed_ = false;
    decoder_.configure_d3d11_surface_output(nullptr);
    surface_bridge_.reset();
    decode_device_.Reset();
    decode_adapter_luid_.store(0);
  }

  void request_cpu_decode_fallback() {
    if (!cpu_decode_fallback_requested_.exchange(true)) {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.d3d11_surface_fallbacks;
    }
  }

  void stop() {
    running_.store(false);
    {
      std::lock_guard lock(frame_mutex_);
      playback_state_.paused = true;
      playback_state_.reason = redclaw::helper::PlaybackPauseReason::kRecovery;
      advance_playback_revision_locked();
      if (shared_header_) publish_direct_frame_playback_state(shared_header_, playback_state_);
    }
    HANDLE data_event = nullptr;
    {
      std::lock_guard<std::mutex> lock(handle_mutex_);
      data_event = data_event_;
    }
    if (data_event != nullptr) {
      SetEvent(data_event);
    }

    if (worker_.joinable()) {
      worker_.join();
    }

    {
      std::lock_guard<std::mutex> lock(handle_mutex_);
      shared_header_ = nullptr;
      if (mapping_view_ != nullptr) {
        UnmapViewOfFile(mapping_view_);
        mapping_view_ = nullptr;
      }
      if (data_event_ != nullptr) {
        CloseHandle(data_event_);
        data_event_ = nullptr;
      }
      if (mapping_handle_ != nullptr) {
        CloseHandle(mapping_handle_);
        mapping_handle_ = nullptr;
      }
    }

    decoder_.reset();
    decoder_reference_.clear();
    session_reset_requested_.store(false);
    consecutive_decode_failures_ = 0;
    decoded_encoded_frames_ = 0;
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      latest_frame_ = DirectFrameData{};
      latest_frame_count_ = 0;
      delivered_frame_count_ = 0;
    }
    frame_buffer_pool_.clear();
    shared_snapshot_ = {};

    // Clear the notify callback after the worker has fully stopped so it can
    // never fire against destroyed UI objects.
    {
      std::lock_guard<std::mutex> lock(notify_mutex_);
      on_frame_notify_ = nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(keyframe_request_mutex_);
      on_keyframe_request_ = nullptr;
    }
  }

  bool is_running() const {
    return running_.load();
  }

  QString pipe_name() const {
    return pipe_name_;
  }

  // Set a lightweight callback invoked from the worker thread every time a
  // new frame is stored.  The callback must be cheap (just post a Qt event).
  // Thread-safe: may be called before start() or cleared after stop().
  void set_on_frame_notify(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(notify_mutex_);
    on_frame_notify_ = std::move(fn);
  }

  void set_on_keyframe_request(std::function<void(QString, redclaw::helper::PlaybackStateSnapshot)> fn) {
    std::lock_guard<std::mutex> lock(keyframe_request_mutex_);
    on_keyframe_request_ = std::move(fn);
  }

  redclaw::helper::PlaybackStateSnapshot playback_state() const {
    std::lock_guard lock(frame_mutex_);
    return playback_state_;
  }

  void set_playback_paused(bool paused, const QString& reason) {
    {
      std::lock_guard lock(frame_mutex_);
      if (!shared_header_ || (desired_paused_ == paused
          && (playback_reason_ == reason || (!paused && !playback_state_.paused)))) return;
      desired_paused_ = paused;
      playback_reason_ = reason;
      advance_playback_revision_locked();
      // Resume stays ineligible until the worker has discarded retained work.
      playback_state_.paused = true;
      playback_state_.resynchronizing = true;
      playback_state_.reason = reason == "display_off" ? redclaw::helper::PlaybackPauseReason::kDisplayOff
          : reason == "occluded" ? redclaw::helper::PlaybackPauseReason::kOccluded
          : paused && reason != "native_window" ? redclaw::helper::PlaybackPauseReason::kHidden
                                               : redclaw::helper::PlaybackPauseReason::kRecovery;
      publish_direct_frame_playback_state(shared_header_, playback_state_);
      latest_frame_ = {};
      delivered_frame_count_ = latest_frame_count_;
    }
    std::lock_guard lock(handle_mutex_);
    if (data_event_) SetEvent(data_event_);
  }

  // Every dependent frame after a gap asks for a keyframe; the limiter turns
  // that burst into one request now and at most one per interval after.
  void request_keyframe(const QString& reason, std::optional<std::uint64_t> resume_revision = std::nullopt) {
    redclaw::helper::PlaybackStateSnapshot state;
    std::function<void(QString, redclaw::helper::PlaybackStateSnapshot)> notify;
    std::optional<std::string> ready;
    {
      std::lock_guard frame_lock(frame_mutex_);
      state = playback_state_;
      if (state.paused || (resume_revision && *resume_revision != state.revision)) return;
      std::lock_guard request_lock(keyframe_request_mutex_);
      ready = keyframe_limiter_.request(steady_now_ms(), reason.toStdString());
      if (!ready) return;
      notify = on_keyframe_request_;
    }
    if (notify) notify(QString::fromStdString(*ready), state);
  }

  void poll_coalesced_keyframe_request() {
    redclaw::helper::PlaybackStateSnapshot state;
    std::function<void(QString, redclaw::helper::PlaybackStateSnapshot)> notify;
    std::optional<std::string> ready;
    {
      std::lock_guard frame_lock(frame_mutex_);
      state = playback_state_;
      if (state.paused) return;
      std::lock_guard request_lock(keyframe_request_mutex_);
      ready = keyframe_limiter_.poll(steady_now_ms());
      if (!ready) return;
      notify = on_keyframe_request_;
    }
    if (notify) notify(QString::fromStdString(*ready), state);
  }

  void keyframe_request_satisfied() {
    std::lock_guard<std::mutex> lock(keyframe_request_mutex_);
    keyframe_limiter_.satisfied();
  }

  bool take_latest_frame(DirectFrameData* frame, quint64* frame_count) {
    GuiLatencyScope timing(GuiStage::kFrameLock);
    std::lock_guard<std::mutex> lock(frame_mutex_);
    timing.finish();
    if (playback_state_.paused || !has_frame_content(latest_frame_) || delivered_frame_count_ == latest_frame_count_
        || latest_frame_.playback_generation != playback_generation_.load()) {
      return false;
    }
    if (frame != nullptr) {
      *frame = std::move(latest_frame_);
    }
    latest_frame_ = DirectFrameData{};
    if (frame_count != nullptr) {
      *frame_count = latest_frame_count_;
    }
    delivered_frame_count_ = latest_frame_count_;
    return true;
  }

  void recycle_frame(DirectFrameData frame) {
    GuiLatencyScope timing(GuiStage::kFrameRelease);
    frame_buffer_pool_.release(std::move(frame));
  }

  bool has_pending_frame() const {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    return has_frame_content(latest_frame_) && delivered_frame_count_ != latest_frame_count_;
  }

  quint64 request_session_reset() {
    session_reset_requested_.store(true);
    const auto generation = playback_generation_.fetch_add(1) + 1;
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      latest_frame_ = DirectFrameData{};
      delivered_frame_count_ = latest_frame_count_;
      playback_state_.session = ++playback_session_counter_;
      if (!desired_paused_) playback_reason_ = "session_reset";
      advance_playback_revision_locked();
      playback_state_.paused = true;
      playback_state_.resynchronizing = true;
      if (shared_header_) publish_direct_frame_playback_state(shared_header_, playback_state_);
    }
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      stats_.playback_generation = generation;
      stats_.latest_displayable_frame_id = stats_.latest_displayable_keyframe_id = 0;
    }
    HANDLE data_event = nullptr;
    {
      std::lock_guard<std::mutex> lock(handle_mutex_);
      data_event = data_event_;
    }
    if (data_event != nullptr) {
      SetEvent(data_event);
    }
    return generation;
  }

  DirectFrameTransportStats stats_snapshot() const {
    DirectFrameTransportStats snapshot;
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      snapshot = stats_;
    }
    const auto* shared_header = shared_header_;
    if (shared_header != nullptr) {
      snapshot.writer_frames = static_cast<quint64>(redclaw::helper::direct_frame_atomic_load_i64(&shared_header->writer_frame_count));
      snapshot.writer_oversize_drops = static_cast<quint64>(redclaw::helper::direct_frame_atomic_load_i64(&shared_header->writer_oversize_drop_count));
      snapshot.writer_busy_drops = static_cast<quint64>(redclaw::helper::direct_frame_atomic_load_i64(&shared_header->writer_busy_drop_count));
    }
    snapshot.buffer_pool = frame_buffer_pool_.telemetry();
    snapshot.decode_adapter_luid = decode_adapter_luid_.load();
    {
      std::lock_guard<std::mutex> lock(keyframe_request_mutex_);
      snapshot.keyframe_requests_sent = keyframe_limiter_.sent();
      snapshot.keyframe_requests_suppressed = keyframe_limiter_.suppressed();
    }
    return snapshot;
  }

 private:
  void advance_playback_revision_locked() {
    ++playback_state_.revision;
    std::lock_guard request_lock(keyframe_request_mutex_);
    keyframe_limiter_.cancel_pending();
  }

  static bool has_frame_content(const DirectFrameData& frame) {
    return !frame.pixels.empty() || frame.d3d11_surface != nullptr;
  }

  static bool is_encoded_format(quint32 format) {
    return format == redclaw::helper::kDirectFrameFormatH264 || format == redclaw::helper::kDirectFrameFormatHevc;
  }

  void run() {
    while (running_.load()) {
      HANDLE data_event = nullptr;
      {
        std::lock_guard<std::mutex> lock(handle_mutex_);
        data_event = data_event_;
      }
      if (data_event == nullptr) {
        break;
      }

      const bool pending = shared_header_ != nullptr
          && static_cast<quint64>(redclaw::helper::direct_frame_atomic_load_i64(
                 &shared_header_->latest_sequence)) > observed_sequence_;
      const DWORD wait_result = WaitForSingleObject(data_event, pending ? 0 : 50);
      if (!running_.load()) {
        break;
      }
      if (session_reset_requested_.exchange(false)) {
        decoder_reference_.invalidate();
        frame_buffer_pool_.clear();
        consecutive_decode_failures_ = 0;
        decoded_encoded_frames_ = 0;
        const auto* shared_header = shared_header_;
        if (shared_header != nullptr) {
          observed_sequence_ = static_cast<quint64>(
              redclaw::helper::direct_frame_atomic_load_i64(&shared_header->latest_sequence));
          redclaw::helper::direct_frame_atomic_store_i64(
              &shared_header_->reader_sequence, static_cast<LONG64>(observed_sequence_));
          redclaw::helper::direct_frame_atomic_store_i64(
              &shared_header_->reader_active_sequence, 0);
        }
        continue;
      }
      if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_TIMEOUT) {
        break;
      }

      QString resume_reason;
      {
        std::lock_guard lock(frame_mutex_);
        if (playback_state_.paused || worker_revision_ != playback_state_.revision) {
          const auto discarded = redclaw::helper::discard_direct_frame_shared_latest(shared_header_, observed_sequence_);
          decoder_reference_.invalidate();
          {
            std::lock_guard stats_lock(stats_mutex_);
            stats_.pause_discarded_sequences += discarded;
          }
          if (desired_paused_) { worker_revision_ = playback_state_.revision; continue; }
          // One deferred reset, on this worker, immediately before recovery IDR.
          worker_revision_ = playback_state_.revision;
          playback_state_.boundary = observed_sequence_;
          playback_state_.paused = false;
          playback_state_.reason = redclaw::helper::PlaybackPauseReason::kVisible;
          publish_direct_frame_playback_state(shared_header_, playback_state_);
          resume_reason = "presentation_resumed_" + playback_reason_;
        }
      }
      if (!resume_reason.isEmpty()) request_keyframe(resume_reason, worker_revision_);
      const auto before_consume = observed_sequence_;
      consume_latest_frame();
      poll_coalesced_keyframe_request();
      if (pending && observed_sequence_ == before_consume) {
        // A writer may still be committing the inspected header. Do not spin
        // on an unreadable slot when no progress was possible.
        (void)WaitForSingleObject(data_event, 50);
      }
    }

    running_.store(false);
  }

  void consume_latest_frame() {
    // At most the retained ring slots per turn, not an unbounded catch-up
    // queue. Unconsumed work is revisited by the bounded wait loop.
    for (std::uint32_t consumed = 0;
         running_.load() && consumed < redclaw::helper::kDirectFrameSharedMemorySlotCount;
         ++consumed) {
      const auto generation = playback_generation_.load();
      const auto transition = playback_state();
      if (transition.paused) return;
      if (session_reset_requested_.load()) return;
      const auto* shared_header = shared_header_;
      if (shared_header == nullptr) {
        return;
      }

      const auto read_plan = redclaw::helper::plan_direct_frame_shared_read(
          mapping_view_, shared_header_, observed_sequence_, decoder_reference_.needs_idr());
      if (read_plan.sequence == 0) {
        return;
      }

      const quint64 next_sequence = read_plan.sequence;
      const bool sequence_gap = next_sequence > observed_sequence_ + 1;
      if (sequence_gap) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.skipped_sequences += next_sequence - observed_sequence_ - 1;
      }

      DirectFrameData frame;
      const auto snapshot_started = std::chrono::steady_clock::now();
      const auto snapshot_status = redclaw::helper::copy_direct_frame_shared_sequence(
          mapping_view_, shared_header_, next_sequence, &shared_snapshot_);
      const quint64 snapshot_us = static_cast<quint64>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - snapshot_started)
              .count());
      observed_sequence_ = next_sequence;
      if (snapshot_status != redclaw::helper::DirectFrameSharedSnapshotStatus::kCopied) {
        redclaw::helper::direct_frame_atomic_store_i64(
            &shared_header_->reader_sequence, static_cast<LONG64>(next_sequence));
        {
          std::lock_guard<std::mutex> lock(stats_mutex_);
          ++stats_.shared_snapshot_failures;
          stats_.shared_snapshot_total_us += snapshot_us;
          stats_.shared_snapshot_max_us = (std::max)(stats_.shared_snapshot_max_us, snapshot_us);
          if (snapshot_status
              == redclaw::helper::DirectFrameSharedSnapshotStatus::kSequenceChanged
              || snapshot_status
                  == redclaw::helper::DirectFrameSharedSnapshotStatus::kMissingSequence) {
            ++stats_.shared_snapshot_sequence_changes;
          }
        }
        decoder_reference_.invalidate();
        request_keyframe("shared_memory_sequence_changed_during_snapshot");
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.shared_snapshot_copies;
        stats_.shared_snapshot_bytes += shared_snapshot_.payload.size();
        stats_.shared_snapshot_total_us += snapshot_us;
        stats_.shared_snapshot_max_us = (std::max)(stats_.shared_snapshot_max_us, snapshot_us);
        stats_.shared_dependency_catchups += read_plan.dependency_catchup ? 1 : 0;
        stats_.shared_independent_skips += read_plan.independent_skip ? 1 : 0;
      }
      const bool snapshot_keyframe = (shared_snapshot_.frame.flags & 1U) != 0U;
      const bool encoded_sequence_gap = sequence_gap && is_encoded_format(shared_snapshot_.frame.format);
      if (encoded_sequence_gap) decoder_reference_.invalidate();
      if (encoded_sequence_gap && !snapshot_keyframe) {
        {
          std::lock_guard<std::mutex> lock(stats_mutex_);
          ++stats_.shared_dependency_gaps;
        }
        request_keyframe("shared_memory_sequence_gap");
      }

      if (playback_state().revision != transition.revision || playback_state().paused) return;
      const bool decoded = decode_snapshot(shared_snapshot_, &frame);
      if (decoded) {
        frame.playback_generation = generation;
        publish_latest_frame(std::move(frame), next_sequence, transition, shared_snapshot_.frame.format);
      } else {
        frame_buffer_pool_.release(std::move(frame));
      }
    }
  }

  bool decode_snapshot(
      const redclaw::helper::DirectFrameSharedSnapshot& snapshot,
      DirectFrameData* frame) {
    if (frame == nullptr) {
      return false;
    }

    const auto& header = snapshot.frame;
    if (std::memcmp(header.magic,
                    redclaw::helper::kDirectFrameChannelMagic,
                    sizeof(redclaw::helper::kDirectFrameChannelMagic)) != 0
        || header.width == 0
        || header.height == 0
        || header.payload_size == 0
        || header.capture_region_revision == 0
        || header.content_rect_width == 0
        || header.content_rect_height == 0
        || header.content_rect_x + header.content_rect_width > header.width
        || header.content_rect_y + header.content_rect_height > header.height
        || header.payload_size > redclaw::helper::kDirectFrameMaxPayloadBytes
        || (!is_encoded_format(header.format)
            && header.format != redclaw::helper::kDirectFrameFormatJpeg
            && (header.format != redclaw::helper::kDirectFrameFormatBgra
                || header.row_pitch < header.width * 4U
                || static_cast<quint64>(header.payload_size)
                       < static_cast<quint64>(header.row_pitch) * header.height))) {
      decoder_reference_.invalidate();
      return false;
    }

    const auto* payload = snapshot.payload.data();

    if (header.format == redclaw::helper::kDirectFrameFormatJpeg) {
      const QByteArray encoded(
          reinterpret_cast<const char*>(payload),
          static_cast<qsizetype>(header.payload_size));
      const QImage decoded_image = QImage::fromData(encoded, "JPG").convertToFormat(QImage::Format_RGB32);
      if (decoded_image.isNull()
          || decoded_image.width() != static_cast<int>(header.width)
          || decoded_image.height() != static_cast<int>(header.height)) {
        return false;
      }
      frame->width = header.width;
      frame->height = header.height;
      frame->row_pitch = static_cast<std::uint32_t>(decoded_image.bytesPerLine());
      frame->timestamp_ms = header.timestamp_ms;
      frame->bgra = true;
      frame->capture_region_revision = header.capture_region_revision;
      frame->content_rect_width = header.width;
      frame->content_rect_height = header.height;
      frame->display_id.assign(
          header.display_id,
          strnlen_s(header.display_id, sizeof(header.display_id)));
      frame->output_path = redclaw::render::DecodedVideoFramePath::kSharedMemoryBgra;
      frame->pixels.assign(
          decoded_image.constBits(),
          decoded_image.constBits() + decoded_image.sizeInBytes());
      return true;
    }

    if (cpu_decode_fallback_requested_.exchange(false)) {
      // Stay on the presenter's adapter (D10): only the surface hand-over is
      // dropped. Hardware decode remains D3D11VA on decode_device_ with CPU
      // transfer, or software; never another GPU's default device.
      decoder_.set_d3d11_surface_output_enabled(false);
      prefer_d3d11_surface_output_ = false;
      decoded_cpu_output_observed_ = true;
      decoder_reference_.invalidate();
    }

    const quint64 required_pixel_bytes = header.format == redclaw::helper::kDirectFrameFormatBgra
        ? static_cast<quint64>(header.payload_size)
        : static_cast<quint64>(header.width) * header.height * 4U;
    if (required_pixel_bytes > static_cast<quint64>((std::numeric_limits<std::size_t>::max)())) {
      decoder_reference_.invalidate();
      return false;
    }
    const bool acquire_cpu_buffer = header.format == redclaw::helper::kDirectFrameFormatBgra
        || !prefer_d3d11_surface_output_
        || decoded_cpu_output_observed_;
    *frame = acquire_cpu_buffer
        ? frame_buffer_pool_.acquire(static_cast<std::size_t>(required_pixel_bytes))
        : DirectFrameData{};

    if (header.format == redclaw::helper::kDirectFrameFormatBgra) {
      frame->frame_id = header.frame_id;
      frame->width = header.width;
      frame->height = header.height;
      frame->row_pitch = header.row_pitch;
      frame->timestamp_ms = header.timestamp_ms;
      frame->bgra = true;
      frame->capture_region_revision = header.capture_region_revision;
      frame->content_rect_x = header.content_rect_x;
      frame->content_rect_y = header.content_rect_y;
      frame->content_rect_width = header.content_rect_width;
      frame->content_rect_height = header.content_rect_height;
      frame->output_path = redclaw::render::DecodedVideoFramePath::kSharedMemoryBgra;
      frame->pixels.assign(payload, payload + header.payload_size);

      return true;
    }

    const bool keyframe = header.format == redclaw::helper::kDirectFrameFormatHevc
        ? redclaw::protocol::hevc_annex_b_contains_idr({payload, header.payload_size})
        : (header.flags & 1u) != 0u;

    if (!decoder_reference_.prepare(header.format, header.width, header.height,
            header.capture_region_revision, keyframe)) {
      request_keyframe("latest_frame_missing_keyframe_dependency");
      return false;
    }
    if (decoder_reference_.needs_idr() && keyframe) decoder_.reset();

    redclaw::render::DecodedVideoFrame decoded = std::move(*frame);
    std::string decode_error;
    bool frame_ready = false;
    bool decode_ok = decoder_.decode_frame_view(
            header.format == redclaw::helper::kDirectFrameFormatH264
                ? redclaw::render::EncodedVideoCodec::kH264
                : redclaw::render::EncodedVideoCodec::kHevc,
            header.width,
            header.height,
            header.timestamp_ms,
            keyframe,
            payload,
            header.payload_size,
            &frame_ready,
            &decoded,
            &decode_error);
    if (!decode_ok) {
      ++consecutive_decode_failures_;
      const std::string failed_backend = decoder_.active_decoder_name();
      const bool fallback_to_software = redclaw::render::should_fallback_to_software_decode(
          decoder_.using_hardware_decode(),
          consecutive_decode_failures_,
          decoded_encoded_frames_);
      if (fallback_to_software) {
        const std::string hardware_error = decode_error;
        decoder_.force_software_decode();
        redclaw::helper::direct_frame_atomic_store_i64(&shared_header_->playback_software_fallback, 1);
        frame_ready = false;
        std::string software_error;
        decode_ok = decoder_.decode_frame_view(
            header.format == redclaw::helper::kDirectFrameFormatH264
                ? redclaw::render::EncodedVideoCodec::kH264
                : redclaw::render::EncodedVideoCodec::kHevc,
            header.width,
            header.height,
            header.timestamp_ms,
            keyframe,
            payload,
            header.payload_size,
            &frame_ready,
            &decoded,
            &software_error);
        {
          std::lock_guard<std::mutex> lock(stats_mutex_);
          ++stats_.hardware_decode_runtime_fallbacks;
          stats_.hardware_decode_fallback_from_backend =
              QString::fromStdString(failed_backend);
          stats_.last_hardware_decode_error =
              QString::fromStdString(hardware_error);
        }
        if (!decode_ok) {
          decode_error = "hardware runtime decode failed backend=" + failed_backend
              + ": " + hardware_error
              + "; software fallback failed backend=" + decoder_.active_decoder_name()
              + ": " + software_error;
        } else {
          decode_error.clear();
        }
      }
    }
    if (!decode_ok) {
      *frame = std::move(decoded);
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.decode_failures;
        redclaw::helper::direct_frame_atomic_increment_i64(&shared_header_->playback_failures);
        stats_.consecutive_decode_failures = consecutive_decode_failures_;
        stats_.decoder_backend = QString::fromStdString(decoder_.active_decoder_name());
        stats_.last_decode_error = QString::fromStdString(decode_error);
      }
      decoder_reference_.invalidate();
      request_keyframe("gui_decode_failure");
      return false;
    }

    // Successful IDR submission permits following dependencies even when the
    // decoder buffers output. Displayability is acknowledged only on output.
    decoder_reference_.submitted(keyframe);
    if (!frame_ready) {
      *frame = std::move(decoded);
      return false;
    }

    if (!decoder_.using_hardware_decode())
      redclaw::helper::direct_frame_atomic_store_i64(&shared_header_->playback_software_fallback, 1);
    consecutive_decode_failures_ = 0;
    ++decoded_encoded_frames_;
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.decoded_frames;
      stats_.consecutive_decode_failures = 0;
      stats_.decoder_backend = QString::fromStdString(decoder_.active_decoder_name());
      stats_.last_decode_error.clear();
      const bool surface = decoded.output_path == redclaw::render::DecodedVideoFramePath::kD3D11DecodeSurface;
      const quint64 pinned_luid = decode_adapter_luid_.load();
      stats_.decode_route = surface ? QStringLiteral("present_adapter_surface")
          : pinned_luid != 0       ? QStringLiteral("present_adapter_cpu")
                                   : QStringLiteral("default_adapter_cpu");
      stats_.hevc_decode_adapter_luid = header.format == redclaw::helper::kDirectFrameFormatHevc
              && (surface || decoder_.using_hardware_decode())
          ? pinned_luid
          : 0;
    }

    decoded_cpu_output_observed_ =
        decoded.output_path != redclaw::render::DecodedVideoFramePath::kD3D11DecodeSurface;
    decoded.frame_id = header.frame_id;
    decoded.keyframe = keyframe || decoder_reference_.awaiting_output();
    decoded.capture_region_revision = header.capture_region_revision;
    decoded.content_rect_x = header.content_rect_x;
    decoded.content_rect_y = header.content_rect_y;
    decoded.content_rect_width = header.content_rect_width;
    decoded.content_rect_height = header.content_rect_height;

    const bool displayable = hand_over_d3d11_surface(&decoded);
    if (displayable) {
      if (decoded.keyframe) keyframe_request_satisfied();
      (void)decoder_reference_.take_recovery_output();
    }
    *frame = std::move(decoded);
    return displayable;
  }

  // Copies a decoder pool surface into a shared slot and releases the pool
  // slice. Returns false when the frame is dropped for display only.
  bool hand_over_d3d11_surface(DirectFrameData* decoded) {
    auto& surface = decoded->d3d11_surface;
    if (decoded->output_path != redclaw::render::DecodedVideoFramePath::kD3D11DecodeSurface
        || surface == nullptr || surface->shared_handle != nullptr) {
      return true;
    }
    auto shared = std::make_shared<redclaw::render::D3D11DecodedSurface>();
    std::string error = "decode device has no shared-surface bridge";
    const auto status = surface_bridge_
        ? surface_bridge_->publish(surface->texture, surface->array_slice, shared.get(), &error)
        : redclaw::render::D3D11SharedSurfaceBridge::PublishStatus::kFailed;
    surface.reset();
    if (status == redclaw::render::D3D11SharedSurfaceBridge::PublishStatus::kPublished) {
      surface = std::move(shared);
      const std::string note = surface_bridge_->shared_texture_note();
      if (!note.empty()) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.shared_texture_bind = QString::fromStdString(note);
      }
      return true;
    }
    decoded->output_path = redclaw::render::DecodedVideoFramePath::kUnknown;
    std::lock_guard<std::mutex> lock(stats_mutex_);
    if (status == redclaw::render::D3D11SharedSurfaceBridge::PublishStatus::kAllSlotsBusy) {
      ++stats_.surface_bridge_busy_drops;
      return false;
    }
    ++stats_.surface_bridge_failures;
    stats_.last_surface_bridge_error = QString::fromStdString(error);
    if (!cpu_decode_fallback_requested_.exchange(true)) ++stats_.d3d11_surface_fallbacks;
    return false;
  }

  void publish_latest_frame(DirectFrameData frame, quint64 sequence, const redclaw::helper::PlaybackStateSnapshot& transition, quint32 encoded_format) {
    const auto generation = frame.playback_generation;
    const auto output_path = frame.output_path;
    const quint64 frame_id = frame.frame_id;
    const bool keyframe = frame.keyframe;
    DirectFrameData displaced_frame;
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      if (frame.playback_generation != playback_generation_.load() || !redclaw::helper::playback_output_is_current(transition, playback_state_)) {
        std::lock_guard stats_lock(stats_mutex_);
        ++stats_.stale_decoded_outputs;
        return;
      }
      if (playback_state_.resynchronizing) {
        playback_state_.resynchronizing = false;
        publish_direct_frame_playback_state(shared_header_, playback_state_);
      }
      displaced_frame = std::move(latest_frame_);
      latest_frame_ = std::move(frame);
      ++latest_frame_count_;

      std::lock_guard<std::mutex> stats_lock(stats_mutex_);
      if (generation != playback_generation_.load()) return;
      ++stats_.observed_sequences;
      ++stats_.published_frames;
      redclaw::helper::direct_frame_atomic_increment_i64(&shared_header_->playback_progress);
      if (encoded_format == redclaw::helper::kDirectFrameFormatHevc)
        redclaw::helper::direct_frame_atomic_increment_i64(&shared_header_->playback_hevc_progress);
      stats_.last_sequence = sequence;
      stats_.latest_displayable_frame_id = (std::max)(
          stats_.latest_displayable_frame_id, frame_id);
      if (keyframe) {
        stats_.latest_displayable_keyframe_id = (std::max)(
            stats_.latest_displayable_keyframe_id, frame_id);
      }
      stats_.last_output_path = output_path;
      switch (stats_.last_output_path) {
      case redclaw::render::DecodedVideoFramePath::kSharedMemoryBgra:
        ++stats_.shared_memory_bgra_frames;
        break;
      case redclaw::render::DecodedVideoFramePath::kSoftwareDecodeBgra:
        ++stats_.software_decode_bgra_frames;
        break;
      case redclaw::render::DecodedVideoFramePath::kHardwareDecodeCpuTransferBgra:
        ++stats_.hardware_decode_cpu_transfer_frames;
        break;
      case redclaw::render::DecodedVideoFramePath::kD3D11DecodeSurface:
        ++stats_.d3d11_decode_surface_frames;
        break;
      case redclaw::render::DecodedVideoFramePath::kUnknown:
        break;
      }
    }

    frame_buffer_pool_.release(std::move(displaced_frame));
    std::function<void()> notify;
    {
      std::lock_guard<std::mutex> lock(notify_mutex_);
      notify = on_frame_notify_;
    }
    if (notify) {
      notify();
    }
  }

  QString pipe_name_;
  std::atomic_bool running_{false};
  mutable std::mutex handle_mutex_;
  HANDLE mapping_handle_ = nullptr;
  HANDLE data_event_ = nullptr;
  void* mapping_view_ = nullptr;
  redclaw::helper::DirectFrameSharedMemoryHeader* shared_header_ = nullptr;
  std::thread worker_;
  quint64 observed_sequence_ = 0;
  redclaw::render::DecoderReferenceRecovery decoder_reference_;
  std::atomic_bool session_reset_requested_{false};
  std::atomic<quint64> playback_generation_{1};
  std::atomic_bool cpu_decode_fallback_requested_{false};
  bool prefer_d3d11_surface_output_ = false;
  bool decoded_cpu_output_observed_ = false;
  quint64 consecutive_decode_failures_ = 0;
  quint64 decoded_encoded_frames_ = 0;
  redclaw::helper::PlaybackStateSnapshot playback_state_;
  quint64 playback_session_counter_ = 0;
  bool desired_paused_ = false;
  QString playback_reason_ = "initial";
  quint64 worker_revision_ = 0;

  std::mutex notify_mutex_;
  std::function<void()> on_frame_notify_;
  mutable std::mutex keyframe_request_mutex_;
  std::function<void(QString, redclaw::helper::PlaybackStateSnapshot)> on_keyframe_request_;
  redclaw::render::KeyframeRequestLimiter keyframe_limiter_{500};

  mutable std::mutex stats_mutex_;
  DirectFrameTransportStats stats_;

  mutable std::mutex frame_mutex_;
  DirectFrameData latest_frame_;
  quint64 latest_frame_count_ = 0;
  quint64 delivered_frame_count_ = 0;
  redclaw::render::DecodedFrameBufferPool frame_buffer_pool_;
  redclaw::helper::DirectFrameSharedSnapshot shared_snapshot_;

  // In-process decoder for encoded frame pipe protocol (H264/HEVC).
  // Only used from the worker thread; no external synchronization needed.
  redclaw::render::FfmpegVideoFrameDecoder decoder_;
  Microsoft::WRL::ComPtr<ID3D11Device> decode_device_;
  std::unique_ptr<redclaw::render::D3D11SharedSurfaceBridge> surface_bridge_;
  std::atomic<std::uint64_t> decode_adapter_luid_{0};
};
DirectFramePipeServer::DirectFramePipeServer() : impl_(std::make_unique<Impl>()) {}
DirectFramePipeServer::~DirectFramePipeServer() = default;
bool DirectFramePipeServer::start(const QString& name, QString* error) { return impl_->start(name, error); }
bool DirectFramePipeServer::configure_d3d11_decode_adapter(std::uint64_t luid, QString* detail) {
  return impl_->configure_d3d11_decode_adapter(luid, detail);
}
void DirectFramePipeServer::disable_d3d11_surface_output() { impl_->disable_d3d11_surface_output(); }
void DirectFramePipeServer::request_cpu_decode_fallback() { impl_->request_cpu_decode_fallback(); }
void DirectFramePipeServer::stop() { impl_->stop(); }
bool DirectFramePipeServer::is_running() const { return impl_->is_running(); }
QString DirectFramePipeServer::pipe_name() const { return impl_->pipe_name(); }
void DirectFramePipeServer::set_on_frame_notify(std::function<void()> fn) { impl_->set_on_frame_notify(std::move(fn)); }
void DirectFramePipeServer::set_on_keyframe_request(std::function<void(QString, redclaw::helper::PlaybackStateSnapshot)> fn) { impl_->set_on_keyframe_request(std::move(fn)); }
void DirectFramePipeServer::set_playback_paused(bool paused, const QString& reason) { impl_->set_playback_paused(paused, reason); }
redclaw::helper::PlaybackStateSnapshot DirectFramePipeServer::playback_state() const { return impl_->playback_state(); }
void DirectFramePipeServer::request_keyframe(const QString& reason) { impl_->request_keyframe(reason); }
bool DirectFramePipeServer::take_latest_frame(DirectFrameData* frame, quint64* count) { return impl_->take_latest_frame(frame, count); }
void DirectFramePipeServer::recycle_frame(DirectFrameData frame) { impl_->recycle_frame(std::move(frame)); }
bool DirectFramePipeServer::has_pending_frame() const { return impl_->has_pending_frame(); }
quint64 DirectFramePipeServer::request_session_reset() { return impl_->request_session_reset(); }
DirectFrameTransportStats DirectFramePipeServer::stats_snapshot() const { return impl_->stats_snapshot(); }
}  // namespace redclaw::ui
#endif
