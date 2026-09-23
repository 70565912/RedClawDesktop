#include "redclaw/capture/audio_encoder.h"

#include "redclaw/protocol/audio_packet.h"

#include <opus.h>

namespace redclaw::capture {

struct AudioOpusEncoder::Impl {
    OpusEncoder* encoder = nullptr;

    ~Impl() {
        if (encoder != nullptr) {
            opus_encoder_destroy(encoder);
        }
    }
};

AudioOpusEncoder::AudioOpusEncoder() : impl_(std::make_unique<Impl>()) {}

AudioOpusEncoder::~AudioOpusEncoder() = default;

bool AudioOpusEncoder::open(std::string* error) {
    close();
    int opus_error = OPUS_OK;
    impl_->encoder = opus_encoder_create(
        static_cast<opus_int32>(redclaw::protocol::kAudioSampleRate),
        static_cast<int>(redclaw::protocol::kAudioChannels),
        OPUS_APPLICATION_AUDIO,
        &opus_error);
    if (impl_->encoder == nullptr || opus_error != OPUS_OK) {
        if (error != nullptr) {
            *error = "opus encoder failed to initialize";
        }
        close();
        return false;
    }
    opus_encoder_ctl(impl_->encoder, OPUS_SET_BITRATE(redclaw::protocol::kAudioBitrateBps));
    opus_encoder_ctl(impl_->encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(impl_->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    return true;
}

void AudioOpusEncoder::close() {
    if (impl_->encoder != nullptr) {
        opus_encoder_destroy(impl_->encoder);
        impl_->encoder = nullptr;
    }
}

std::vector<std::uint8_t> AudioOpusEncoder::encode(std::span<const std::int16_t> interleaved_stereo) {
    constexpr std::size_t kSamples = redclaw::protocol::kAudioFrameSamples * redclaw::protocol::kAudioChannels;
    if (impl_->encoder == nullptr || interleaved_stereo.size() != kSamples) {
        return {};
    }
    std::vector<std::uint8_t> payload(redclaw::protocol::kMaxAudioPacketPayloadBytes);
    const int written = opus_encode(
        impl_->encoder,
        interleaved_stereo.data(),
        static_cast<int>(redclaw::protocol::kAudioFrameSamples),
        payload.data(),
        static_cast<opus_int32>(payload.size()));
    if (written <= 0) {
        return {};
    }
    payload.resize(static_cast<std::size_t>(written));
    return payload;
}

}  // namespace redclaw::capture
