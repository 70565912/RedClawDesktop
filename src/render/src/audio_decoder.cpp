#include "redclaw/render/audio_decoder.h"

#include "redclaw/protocol/audio_packet.h"
#include "redclaw/render/audio_jitter_buffer.h"
#include "redclaw/render/audio_player.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#include <opus.h>

namespace redclaw::render {
namespace {

constexpr std::size_t kSamples = redclaw::protocol::kAudioFrameSamples * redclaw::protocol::kAudioChannels;
constexpr std::uint32_t kPcmBytes = static_cast<std::uint32_t>(kSamples * sizeof(std::int16_t));

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
    std::mutex mutex;
    AudioJitterBuffer jitter;
    AudioOpusDecoder decoder;
    AudioPlayer player;
    std::atomic<bool> enabled{false};
    std::atomic<bool> running{false};
    std::thread worker;
    bool decoder_open = false;
    bool player_open = false;

    void thread_main() {
        auto next_frame = std::chrono::steady_clock::now();
        auto close_output = [&]() {
            if (player_open) {
                player.close();
                player_open = false;
            }
        };
        while (running.load()) {
            if (!enabled.load()) {
                close_output();
                next_frame = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            AudioJitterPull pull;
            const auto now_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            {
                std::lock_guard lock(mutex);
                pull = jitter.pull(now_us);
            }
            const bool audible = pull.action == AudioJitterAction::kPacket || pull.action == AudioJitterAction::kPlc;
            if (pull.action == AudioJitterAction::kSilence) {
                {
                    std::lock_guard lock(mutex);
                    decoder.reset();
                }
                close_output();
            } else if (audible) {
                std::vector<std::int16_t> pcm;
                bool decoded = false;
                {
                    std::lock_guard lock(mutex);
                    if (!decoder_open) {
                        decoder_open = decoder.open(nullptr);
                    }
                    decoded = decoder_open && decoder.decode(
                        pull.action == AudioJitterAction::kPlc
                            ? std::span<const std::uint8_t>{}
                            : std::span<const std::uint8_t>{pull.frame.payload},
                        &pcm,
                        nullptr);
                }
                if (decoded) {
                    if (!player_open) {
                        AudioFormat format;
                        format.sample_rate = redclaw::protocol::kAudioSampleRate;
                        format.channels = redclaw::protocol::kAudioChannels;
                        format.bits_per_sample = 16;
                        player_open = player.open(format, 8, kPcmBytes, nullptr);
                    }
                    if (player_open) {
                        (void)player.play(reinterpret_cast<const std::uint8_t*>(pcm.data()), kPcmBytes, nullptr);
                    }
                }
            }
            if (audible) {
                next_frame += std::chrono::milliseconds(20);
                const auto now = std::chrono::steady_clock::now();
                if (next_frame < now) {
                    next_frame = now;
                }
                std::this_thread::sleep_until(next_frame);
            } else {
                next_frame = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
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
        std::lock_guard lock(impl_->mutex);
        impl_->jitter.reset();
        impl_->decoder.reset();
        return;
    }
    if (impl_->running.load()) {
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
    AudioJitterFrame frame;
    frame.sequence = parsed.value.sequence;
    frame.timestamp_us = parsed.value.timestamp_us;
    frame.payload = parsed.value.payload;
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    std::lock_guard lock(impl_->mutex);
    impl_->jitter.push(std::move(frame), now);
}

void AudioPlayback::stop() {
    impl_->enabled.store(false);
    impl_->running.store(false);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    std::lock_guard lock(impl_->mutex);
    impl_->jitter.reset();
    impl_->decoder.reset();
}

}  // namespace redclaw::render
