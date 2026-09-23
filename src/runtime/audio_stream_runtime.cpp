#include "runtime/audio_stream_runtime.h"

#include "redclaw/capture/audio_encoder.h"
#include "redclaw/capture/audio_loopback.h"
#include "redclaw/protocol/audio_packet.h"
#include "redclaw/render/audio_decoder.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace redclaw::runtime {
namespace {

constexpr std::size_t kMaxQueuedPackets = 4;

bool secure_desktop_active() {
#if !defined(_WIN32)
    return false;
#else
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == nullptr) {
        return false;
    }
    char name[256] = {};
    DWORD bytes_needed = 0;
    const bool ok = GetUserObjectInformationA(
            desktop, UOI_NAME, name, static_cast<DWORD>(sizeof(name)), &bytes_needed) != FALSE;
    CloseDesktop(desktop);
    return ok && lstrcmpiA(name, "Winlogon") == 0;
#endif
}

}  // namespace

struct AudioStreamRuntime::Impl {
    bool host = false;
    std::function<bool(std::span<const std::uint8_t>)> send_packet;
    std::function<bool()> ensure_channel;
    mutable std::mutex mutex;
    std::uint32_t peer_version = 0;
    bool playback_requested = false;
    std::atomic<bool> channel_open{false};
    bool secure_desktop = false;
    std::chrono::steady_clock::time_point next_ensure{};
    std::chrono::steady_clock::time_point ensure_deadline{};
    std::atomic<bool> ensure_inflight{false};
    std::chrono::steady_clock::time_point next_secure_check{};
    redclaw::capture::AudioOpusEncoder encoder;
    std::unique_ptr<redclaw::capture::AudioLoopbackCapture> capture;
    redclaw::render::AudioPlayback playback;
    std::deque<std::vector<std::uint8_t>> pending;
    std::uint32_t sequence = 0;
    std::chrono::steady_clock::time_point capture_origin{};
    bool encoder_open = false;

    bool want_stream_locked() const {
        return redclaw::protocol::audio_stream_wanted(peer_version, playback_requested, secure_desktop);
    }

    void enqueue_locked(std::vector<std::uint8_t> packet) {
        pending.push_back(std::move(packet));
        while (pending.size() > kMaxQueuedPackets) {
            pending.pop_front();
        }
    }

    void flush() {
        while (channel_open.load()) {
            std::vector<std::uint8_t> packet;
            {
                std::lock_guard lock(mutex);
                if (pending.empty()) {
                    return;
                }
                packet = std::move(pending.front());
                pending.pop_front();
            }
            if (!send_packet || !send_packet(packet)) {
                std::lock_guard lock(mutex);
                pending.push_front(std::move(packet));
                while (pending.size() > kMaxQueuedPackets) {
                    pending.pop_front();
                }
                return;
            }
        }
    }

    void on_pcm(std::span<const std::int16_t> pcm) {
        if (!channel_open.load()) {
            return;
        }
        std::vector<std::uint8_t> encoded;
        std::uint32_t sequence_value = 0;
        std::uint64_t timestamp_us = 0;
        {
            std::lock_guard lock(mutex);
            if (!encoder_open) {
                return;
            }
            encoded = encoder.encode(pcm);
            if (encoded.empty()) {
                return;
            }
            sequence_value = ++sequence;
            timestamp_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - capture_origin).count());
        }
        redclaw::protocol::AudioPacketV1 packet;
        packet.sequence = sequence_value;
        packet.timestamp_us = timestamp_us;
        packet.payload = std::move(encoded);
        auto bytes = redclaw::protocol::serialize_audio_packet_v1(packet);
        if (bytes.empty()) {
            return;
        }
        {
            std::lock_guard lock(mutex);
            enqueue_locked(std::move(bytes));
        }
        flush();
    }

    void stop_capture() {
        if (capture) {
            capture->stop();
            capture.reset();
        }
        std::lock_guard lock(mutex);
        encoder.close();
        encoder_open = false;
        pending.clear();
        sequence = 0;
    }

    void start_capture() {
        if (capture && capture->running()) {
            return;
        }
        {
            std::lock_guard lock(mutex);
            encoder_open = encoder.open(nullptr);
            sequence = 0;
            pending.clear();
            capture_origin = std::chrono::steady_clock::now();
        }
        if (!encoder_open) {
            return;
        }
        capture = std::make_unique<redclaw::capture::AudioLoopbackCapture>([this](std::span<const std::int16_t> pcm) {
            on_pcm(pcm);
        });
        std::string error;
        if (!capture->start(&error)) {
            stop_capture();
        }
    }
};

AudioStreamRuntime::AudioStreamRuntime(
    bool host,
    std::function<bool(std::span<const std::uint8_t>)> send_packet,
    std::function<bool()> ensure_channel)
    : impl_(std::make_unique<Impl>()) {
    impl_->host = host;
    impl_->send_packet = std::move(send_packet);
    impl_->ensure_channel = std::move(ensure_channel);
}

AudioStreamRuntime::~AudioStreamRuntime() {
    impl_->stop_capture();
    impl_->playback.stop();
}

void AudioStreamRuntime::observe_peer(std::uint32_t audio_version, bool playback_requested, bool apply_request) {
    std::lock_guard lock(impl_->mutex);
    impl_->peer_version = audio_version;
    if (apply_request) {
        impl_->playback_requested = playback_requested;
    }
}

void AudioStreamRuntime::set_playback_requested(bool requested) {
    std::lock_guard lock(impl_->mutex);
    impl_->playback_requested = requested;
}

void AudioStreamRuntime::set_channel_open(bool open) {
    impl_->channel_open.store(open);
    impl_->ensure_inflight.store(false);
    if (!open) {
        std::lock_guard lock(impl_->mutex);
        impl_->pending.clear();
    }
}

void AudioStreamRuntime::on_packet(std::span<const std::uint8_t> packet) {
    bool enabled = false;
    {
        std::lock_guard lock(impl_->mutex);
        enabled = !impl_->host && impl_->playback_requested && !impl_->secure_desktop;
    }
    if (enabled && impl_->channel_open.load()) {
        impl_->playback.submit(packet);
    }
}

bool AudioStreamRuntime::playback_requested() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->playback_requested;
}

void AudioStreamRuntime::pump() {
    const auto now = std::chrono::steady_clock::now();
    if (now >= impl_->next_secure_check) {
        impl_->next_secure_check = now + std::chrono::milliseconds(200);
        const bool secure = secure_desktop_active();
        std::lock_guard lock(impl_->mutex);
        impl_->secure_desktop = secure;
    }
    bool want = false;
    bool open = impl_->channel_open.load();
    {
        std::lock_guard lock(impl_->mutex);
        want = impl_->want_stream_locked();
    }
    if (impl_->host) {
        if (!want) {
            impl_->stop_capture();
            return;
        }
        if (!open) {
            if (impl_->ensure_inflight.load() && now < impl_->ensure_deadline) {
                return;
            }
            impl_->ensure_inflight.store(false);
            if (now >= impl_->next_ensure && impl_->ensure_channel) {
                impl_->next_ensure = now + std::chrono::seconds(1);
                if (impl_->ensure_channel()) {
                    impl_->ensure_inflight.store(true);
                    impl_->ensure_deadline = now + std::chrono::seconds(3);
                }
            }
            return;
        }
        impl_->ensure_inflight.store(false);
        impl_->start_capture();
        return;
    }
    impl_->playback.set_enabled(want && open);
}

}  // namespace redclaw::runtime
