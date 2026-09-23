#include "redclaw/render/audio_decoder.h"

#include "redclaw/protocol/audio_packet.h"
#include "redclaw/render/audio_jitter_buffer.h"
#include "redclaw/render/audio_player.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#include <opus.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace redclaw::render {
namespace {

constexpr std::size_t kSamples = redclaw::protocol::kAudioFrameSamples * redclaw::protocol::kAudioChannels;
constexpr std::uint32_t kPcmBytes = static_cast<std::uint32_t>(kSamples * sizeof(std::int16_t));
constexpr std::size_t kAudioCacheCapacity = 32;

// Fixed slots circulate from empty to filled, like a use/empty buffer list.
// One receiver thread publishes and the playback thread consumes.
class AudioFrameCache {
public:
    struct Frame {
        std::uint32_t sequence = 0;
        std::uint64_t timestamp_us = 0;
        std::vector<std::uint8_t> payload;
    };

    bool push(Frame frame) {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        if (write - read >= kAudioCacheCapacity) {
            return false;
        }
        slots_[write % kAudioCacheCapacity] = std::move(frame);
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    bool pop(Frame* frame) {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) {
            return false;
        }
        *frame = std::move(slots_[read % kAudioCacheCapacity]);
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    void clear() {
        Frame discarded;
        while (pop(&discarded)) {
        }
    }

    [[nodiscard]] bool empty() const {
        return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire);
    }

private:
    std::array<Frame, kAudioCacheCapacity> slots_{};
    std::atomic<std::size_t> write_{0};
    std::atomic<std::size_t> read_{0};
};

}  // namespace

struct AudioOpusDecoder::Impl {
    OpusDecoder* decoder = nullptr;

    ~Impl() {
        if (decoder != nullptr) {
            opus_decoder_destroy(decoder);
        }
    }
};

AudioOpusDecoder::AudioOpusDecoder() : impl_(std::make_unique<Impl>()) {}

AudioOpusDecoder::~AudioOpusDecoder() = default;

bool AudioOpusDecoder::open(std::string* error) {
    reset();
    if (impl_->decoder != nullptr) {
        opus_decoder_destroy(impl_->decoder);
        impl_->decoder = nullptr;
    }
    int opus_error = OPUS_OK;
    impl_->decoder = opus_decoder_create(
        static_cast<opus_int32>(redclaw::protocol::kAudioSampleRate),
        static_cast<int>(redclaw::protocol::kAudioChannels),
        &opus_error);
    if (impl_->decoder == nullptr || opus_error != OPUS_OK) {
        if (error != nullptr) {
            *error = "opus decoder failed to initialize";
        }
        return false;
    }
    return true;
}

void AudioOpusDecoder::reset() {
    if (impl_->decoder != nullptr) {
        opus_decoder_ctl(impl_->decoder, OPUS_RESET_STATE);
    }
}

bool AudioOpusDecoder::decode(
    std::span<const std::uint8_t> opus_payload,
    std::vector<std::int16_t>* interleaved_stereo,
    std::string* error) {
    if (impl_->decoder == nullptr || interleaved_stereo == nullptr) {
        if (error != nullptr) {
            *error = "opus decoder is not open";
        }
        return false;
    }
    interleaved_stereo->assign(kSamples, 0);
    const int produced = opus_decode(
        impl_->decoder,
        opus_payload.empty() ? nullptr : opus_payload.data(),
        static_cast<opus_int32>(opus_payload.size()),
        interleaved_stereo->data(),
        static_cast<int>(redclaw::protocol::kAudioFrameSamples),
        0);
    if (produced != static_cast<int>(redclaw::protocol::kAudioFrameSamples)) {
        if (error != nullptr) {
            *error = "opus decoder returned an unexpected frame";
        }
        interleaved_stereo->clear();
        return false;
    }
    return true;
}

struct AudioPlayback::Impl {
    AudioFrameCache cache;
    AudioJitterBuffer jitter{1};
    AudioOpusDecoder decoder;
    AudioPlayer player;
    std::mutex wait_mutex;
    std::condition_variable data_event;
    bool data_pending = false;
    std::atomic<bool> enabled{false};
    std::atomic<bool> running{false};
    std::thread worker;
    bool decoder_open = false;
    bool player_open = false;

    void signal_data() {
        std::lock_guard lock(wait_mutex);
        data_pending = true;
        data_event.notify_one();
    }

    void thread_main() {
#if defined(_WIN32)
        // CreateMasteringVoice returns CO_E_NOTINITIALIZED on a thread that has
        // not joined an apartment, even after XAudio2Create succeeds.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        struct ComApartmentGuard {
            bool active = false;
            ~ComApartmentGuard() {
                if (active) {
                    CoUninitialize();
                }
            }
        } com_guard{SUCCEEDED(com)};
#endif
        auto close_output = [&]() {
            if (player_open) {
                player.close();
                player_open = false;
            }
        };
        auto play_cached = [&]() {
            AudioFrameCache::Frame cached;
            const auto queued_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            while (cache.pop(&cached)) {
                AudioJitterFrame frame;
                frame.sequence = cached.sequence;
                frame.timestamp_us = cached.timestamp_us;
                frame.payload = std::move(cached.payload);
                jitter.push(std::move(frame), queued_us);
            }
            while (running.load() && enabled.load()) {
                const auto now_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                const auto pull = jitter.pull(now_us);
                if (pull.action == AudioJitterAction::kWaiting) {
                    return;
                }
                if (pull.action == AudioJitterAction::kSilence) {
                    decoder.reset();
                    continue;
                }
                std::vector<std::int16_t> pcm;
                if (!decoder_open) {
                    decoder_open = decoder.open(nullptr);
                }
                const bool decoded = decoder_open && decoder.decode(
                    pull.action == AudioJitterAction::kPlc
                        ? std::span<const std::uint8_t>{}
                        : std::span<const std::uint8_t>{pull.frame.payload},
                    &pcm,
                    nullptr);
                if (!decoded) {
                    continue;
                }
                if (!player_open) {
                    AudioFormat format;
                    format.sample_rate = redclaw::protocol::kAudioSampleRate;
                    format.channels = redclaw::protocol::kAudioChannels;
                    format.bits_per_sample = 16;
                    player_open = player.open(
                        format, static_cast<std::uint32_t>(kAudioCacheCapacity), kPcmBytes, nullptr);
                }
                if (player_open) {
                    (void)player.play(reinterpret_cast<const std::uint8_t*>(pcm.data()), kPcmBytes, nullptr);
                }
            }
        };
        while (running.load()) {
            if (!enabled.load()) {
                close_output();
                cache.clear();
                jitter.reset();
                decoder.reset();
                std::unique_lock lock(wait_mutex);
                data_event.wait(lock, [&] { return !running.load() || enabled.load(); });
                data_pending = false;
                continue;
            }
            play_cached();
            if (!running.load() || !enabled.load()) {
                continue;
            }
            std::unique_lock lock(wait_mutex);
            data_event.wait(lock, [&] {
                return data_pending || !running.load() || !enabled.load() || !cache.empty();
            });
            data_pending = false;
        }
        player.close();
        player_open = false;
    }
};

AudioPlayback::AudioPlayback() : impl_(std::make_unique<Impl>()) {}

AudioPlayback::~AudioPlayback() {
    stop();
}

void AudioPlayback::set_enabled(bool enabled) {
    impl_->enabled.store(enabled);
    if (!enabled) {
        impl_->signal_data();
        return;
    }
    if (impl_->running.load()) {
        impl_->signal_data();
        return;
    }
    impl_->running.store(true);
    impl_->worker = std::thread([this] { impl_->thread_main(); });
}

void AudioPlayback::submit(std::span<const std::uint8_t> packet) {
    if (!impl_->enabled.load() || packet.empty()) {
        return;
    }
    const auto parsed = redclaw::protocol::parse_audio_packet_v1(packet);
    if (!parsed.ok) {
        return;
    }
    AudioFrameCache::Frame frame;
    frame.sequence = parsed.value.sequence;
    frame.timestamp_us = parsed.value.timestamp_us;
    frame.payload = std::move(parsed.value.payload);
    if (!impl_->cache.push(std::move(frame))) {
        return;
    }
    impl_->signal_data();
}

void AudioPlayback::stop() {
    impl_->enabled.store(false);
    impl_->running.store(false);
    impl_->signal_data();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->cache.clear();
    impl_->jitter.reset();
    impl_->decoder.reset();
}

}  // namespace redclaw::render
