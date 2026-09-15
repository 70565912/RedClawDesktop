#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
struct ID3D11Device;
struct ID3D11Texture2D;
#endif

namespace redclaw::render {

enum class EncodedVideoCodec {
	kUnknown,
	kH264,
	kHevc,
};

struct EncodedVideoFrame {
	std::uint64_t frame_id = 0;
	EncodedVideoCodec codec = EncodedVideoCodec::kUnknown;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint64_t timestamp_ms = 0;
	bool keyframe = false;
	std::uint64_t capture_region_revision = 1;
	std::uint32_t content_rect_x = 0;
	std::uint32_t content_rect_y = 0;
	std::uint32_t content_rect_width = 0;
	std::uint32_t content_rect_height = 0;
	std::vector<std::uint8_t> payload;
};

enum class DecodedVideoFramePath {
	kUnknown,
	kSharedMemoryBgra,
	kSoftwareDecodeBgra,
	kHardwareDecodeCpuTransferBgra,
	kD3D11DecodeSurface,
};

#if defined(_WIN32)
struct D3D11DecodedSurface {
	ID3D11Device* device = nullptr;
	ID3D11Texture2D* texture = nullptr;
	std::uint32_t array_slice = 0;
	std::uint32_t dxgi_format = 0;
	std::shared_ptr<void> frame_lifetime;
};
#endif

struct DecodedVideoFrame {
	std::uint64_t frame_id = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t row_pitch = 0;
	std::uint64_t timestamp_ms = 0;
	bool keyframe = false;
	bool bgra = true;
	std::uint64_t capture_region_revision = 1;
	std::uint32_t content_rect_x = 0;
	std::uint32_t content_rect_y = 0;
	std::uint32_t content_rect_width = 0;
    std::uint32_t content_rect_height = 0;
    std::string display_id;
	DecodedVideoFramePath output_path = DecodedVideoFramePath::kUnknown;
	std::vector<std::uint8_t> pixels;
#if defined(_WIN32)
	std::shared_ptr<D3D11DecodedSurface> d3d11_surface;
#endif
	// Owner-local decode lifecycle, never sent over the network.
	std::uint64_t playback_generation = 0;
};

std::string_view decoded_video_frame_path_name(DecodedVideoFramePath path);

struct DecodedFrameBufferPoolConfig {
	std::size_t max_buffers = 2;
	std::size_t max_buffer_bytes = 32 * 1024 * 1024;
	std::size_t max_total_bytes = 64 * 1024 * 1024;
};

struct DecodedFrameBufferPoolTelemetry {
	std::uint64_t acquire_requests = 0;
	std::uint64_t reuse_hits = 0;
	std::uint64_t accepted_releases = 0;
	std::uint64_t discarded_releases = 0;
	std::uint64_t evicted_buffers = 0;
	std::size_t retained_buffers = 0;
	std::size_t retained_bytes = 0;
};

class DecodedFrameBufferPool {
public:
	explicit DecodedFrameBufferPool(DecodedFrameBufferPoolConfig config = {});
	~DecodedFrameBufferPool();

	DecodedFrameBufferPool(const DecodedFrameBufferPool&) = delete;
	DecodedFrameBufferPool& operator=(const DecodedFrameBufferPool&) = delete;

	DecodedVideoFrame acquire(std::size_t required_pixel_bytes);
	void release(DecodedVideoFrame frame);
	void clear();
	[[nodiscard]] DecodedFrameBufferPoolTelemetry telemetry() const;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

enum class ViewportScaleMode {
	kFit,
	kFill,
	kOneToOne,
};

struct ViewportRect {
	std::int32_t x = 0;
	std::int32_t y = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
};

struct RenderViewportConfig {
	std::uint32_t viewport_width = 0;
	std::uint32_t viewport_height = 0;
	ViewportScaleMode scale_mode = ViewportScaleMode::kFit;
	std::int32_t pan_x = 0;
	std::int32_t pan_y = 0;
};

struct RenderViewportLayout {
	ViewportRect source;
	ViewportRect destination;
	double scale = 1.0;
};

enum class RuntimeStatusSeverity {
	kInfo,
	kWarning,
	kError,
};

struct RuntimeStatusEvent {
	std::uint64_t timestamp_ms = 0;
	RuntimeStatusSeverity severity = RuntimeStatusSeverity::kInfo;
	std::string category;
	std::string message;
};

struct RuntimeStatusTimelineConfig {
	std::size_t max_events = 256;
};

class RuntimeStatusTimeline {
public:
	explicit RuntimeStatusTimeline(RuntimeStatusTimelineConfig config = {});

	void append(RuntimeStatusEvent event);
	void clear();
	[[nodiscard]] std::size_t size() const;
	[[nodiscard]] std::vector<RuntimeStatusEvent> latest(std::size_t max_events) const;

private:
	RuntimeStatusTimelineConfig config_;
	std::deque<RuntimeStatusEvent> events_;
};

struct RuntimeStatusTimelineUiModelConfig {
	std::size_t max_items = 12;
	std::string widget_title = "Runtime Status Timeline";
};

struct RuntimeStatusTimelineUiItem {
	std::uint64_t timestamp_ms = 0;
	RuntimeStatusSeverity severity = RuntimeStatusSeverity::kInfo;
	std::string title;
	std::string subtitle;
};

class RuntimeStatusTimelineUiModel {
public:
	explicit RuntimeStatusTimelineUiModel(RuntimeStatusTimelineUiModelConfig config = {});

	void bind(const RuntimeStatusTimeline* timeline);
	void refresh();
	void clear();

	[[nodiscard]] const std::string& widget_title() const;
	[[nodiscard]] const std::vector<RuntimeStatusTimelineUiItem>& items() const;

private:
	RuntimeStatusTimelineUiModelConfig config_;
	const RuntimeStatusTimeline* timeline_ = nullptr;
	std::vector<RuntimeStatusTimelineUiItem> items_;
};

struct RuntimeStatusTimelineWidgetState {
	std::string title;
	std::vector<RuntimeStatusTimelineUiItem> items;
	bool has_warning = false;
	bool has_error = false;
};

class RuntimeStatusTimelineWidgetComponent {
public:
	void bind(const RuntimeStatusTimelineUiModel* model);
	void refresh();
	void clear();

	[[nodiscard]] const RuntimeStatusTimelineWidgetState& state() const;

private:
	const RuntimeStatusTimelineUiModel* model_ = nullptr;
	RuntimeStatusTimelineWidgetState state_;
};

std::string render_runtime_status_timeline_text(
	const RuntimeStatusTimeline& timeline,
	std::size_t max_events = 12);

std::string render_runtime_status_timeline_widget_text(
	const RuntimeStatusTimelineUiModel& model);

bool compute_render_viewport_layout(
	std::uint32_t frame_width,
	std::uint32_t frame_height,
	const RenderViewportConfig& config,
	RenderViewportLayout* layout,
	std::string* error_detail = nullptr);

class IVideoFrameDecoder {
public:
	virtual ~IVideoFrameDecoder() = default;

	virtual bool decode_frame(
		const EncodedVideoFrame& input,
		DecodedVideoFrame* output,
		std::string* error_detail = nullptr) = 0;

	virtual void reset() = 0;
};

class FfmpegVideoFrameDecoder final : public IVideoFrameDecoder {
public:
	FfmpegVideoFrameDecoder();
	~FfmpegVideoFrameDecoder() override;

	bool decode_frame_view(
		EncodedVideoCodec codec,
		std::uint32_t width,
		std::uint32_t height,
		std::uint64_t timestamp_ms,
		bool keyframe,
		const std::uint8_t* payload_data,
		std::size_t payload_size,
		bool* frame_ready,
		DecodedVideoFrame* output,
		std::string* error_detail = nullptr);

	bool decode_frame(
		const EncodedVideoFrame& input,
		DecodedVideoFrame* output,
		std::string* error_detail = nullptr) override;

#if defined(_WIN32)
	void configure_d3d11_surface_output(ID3D11Device* device);
#endif

	void force_software_decode();
	[[nodiscard]] bool using_hardware_decode() const;
	[[nodiscard]] bool software_decode_forced() const;
	[[nodiscard]] std::string active_decoder_name() const;

	void reset() override;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool should_fallback_to_software_decode(
	bool using_hardware_decode,
	std::uint32_t consecutive_decode_failures,
	std::uint64_t decoded_frames);

struct DecodedFrameQueueConfig {
	std::size_t max_frames = 2;
	bool drop_oldest_on_overflow = true;
};

enum class QueuePushStatus {
	kAccepted,
	kDroppedOldest,
	kRejectedFull,
};

class DecodedFrameQueue {
public:
	explicit DecodedFrameQueue(DecodedFrameQueueConfig config = {});

	QueuePushStatus push(DecodedVideoFrame frame);
	bool pop(DecodedVideoFrame* frame);
	void clear();

	[[nodiscard]] std::size_t size() const;
	[[nodiscard]] bool empty() const;
	[[nodiscard]] std::size_t dropped_frame_count() const;

private:
	DecodedFrameQueueConfig config_;
	std::deque<DecodedVideoFrame> queue_;
	std::size_t dropped_frame_count_ = 0;
};

std::string_view module_name();
}
