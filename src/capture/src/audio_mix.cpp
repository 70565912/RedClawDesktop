#include "redclaw/capture/audio_mix.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if __has_include(<libswresample/swresample.h>) && __has_include(<libavutil/channel_layout.h>)
#define REDCLAW_HAS_SWRESAMPLE 1
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace redclaw::capture {
namespace {

constexpr std::uint16_t kWaveFormatPcm = 1;
constexpr std::uint16_t kWaveFormatIeeeFloat = 3;
constexpr std::uint16_t kWaveFormatExtensible = 0xFFFE;
constexpr float kCenter = 0.70710678f;

bool read_u16(std::span<const std::uint8_t> bytes, std::size_t offset, std::uint16_t* value) {
    if (offset + 2 > bytes.size()) {
        return false;
    }
    *value = static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
    return true;
}

bool read_u32(std::span<const std::uint8_t> bytes, std::size_t offset, std::uint32_t* value) {
    if (offset + 4 > bytes.size()) {
        return false;
    }
    *value = static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    return true;
}

bool guid_is(std::span<const std::uint8_t> bytes, std::size_t offset, std::uint32_t data1) {
    if (offset + 16 > bytes.size()) {
        return false;
    }
    std::uint32_t found = 0;
    if (!read_u32(bytes, offset, &found) || found != data1) {
        return false;
    }
    const std::uint8_t tail[] = {
        0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    return std::memcmp(bytes.data() + offset + 4, tail, sizeof(tail)) == 0;
}

float sample_at(const AudioMixFormat& format, std::span<const std::uint8_t> frame, std::uint16_t channel) {
    switch (format.kind) {
    case AudioSampleKind::kInt16: {
        const std::size_t offset = static_cast<std::size_t>(channel) * 2;
        const auto raw = static_cast<std::int16_t>(frame[offset] | (frame[offset + 1] << 8));
        return static_cast<float>(raw) / 32768.0f;
    }
    case AudioSampleKind::kInt24Packed: {
        const std::size_t offset = static_cast<std::size_t>(channel) * 3;
        std::int32_t raw = frame[offset] | (frame[offset + 1] << 8) | (frame[offset + 2] << 16);
        if ((raw & 0x800000) != 0) {
            raw |= ~0xFFFFFF;
        }
        return static_cast<float>(raw) / 8388608.0f;
    }
    case AudioSampleKind::kInt32: {
        const std::size_t offset = static_cast<std::size_t>(channel) * 4;
        std::int32_t raw = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(frame[offset])
            | (static_cast<std::uint32_t>(frame[offset + 1]) << 8)
            | (static_cast<std::uint32_t>(frame[offset + 2]) << 16)
            | (static_cast<std::uint32_t>(frame[offset + 3]) << 24));
        if (format.valid_bits == 24) {
            raw >>= 8;
            return static_cast<float>(raw) / 8388608.0f;
        }
        return static_cast<float>(raw) / 2147483648.0f;
    }
    case AudioSampleKind::kFloat32: {
        const std::size_t offset = static_cast<std::size_t>(channel) * 4;
        float value = 0.0f;
        std::memcpy(&value, frame.data() + offset, sizeof(value));
        return value;
    }
    }
    return 0.0f;
}

void accumulate_channel(std::uint32_t mask_bit, float sample, float* left, float* right) {
    constexpr std::uint32_t kFrontLeft = 0x1;
    constexpr std::uint32_t kFrontRight = 0x2;
    constexpr std::uint32_t kFrontCenter = 0x4;
    constexpr std::uint32_t kLowFrequency = 0x8;
    constexpr std::uint32_t kBackLeft = 0x10;
    constexpr std::uint32_t kBackRight = 0x20;
    constexpr std::uint32_t kFrontLeftOfCenter = 0x40;
    constexpr std::uint32_t kFrontRightOfCenter = 0x80;
    constexpr std::uint32_t kBackCenter = 0x100;
    constexpr std::uint32_t kSideLeft = 0x200;
    constexpr std::uint32_t kSideRight = 0x400;
    if (mask_bit == kLowFrequency) {
        return;
    }
    if (mask_bit == kFrontLeft || mask_bit == kFrontLeftOfCenter) {
        *left += sample;
    } else if (mask_bit == kFrontRight || mask_bit == kFrontRightOfCenter) {
        *right += sample;
    } else if (mask_bit == kFrontCenter || mask_bit == kBackCenter) {
        *left += sample * (mask_bit == kFrontCenter ? kCenter : 0.5f);
        *right += sample * (mask_bit == kFrontCenter ? kCenter : 0.5f);
    } else if (mask_bit == kBackLeft || mask_bit == kSideLeft) {
        *left += sample * kCenter;
    } else if (mask_bit == kBackRight || mask_bit == kSideRight) {
        *right += sample * kCenter;
    }
}

}  // namespace

bool parse_mix_format(std::span<const std::uint8_t> wave_format, AudioMixFormat* format, std::string* error) {
    if (format == nullptr) {
        if (error != nullptr) {
            *error = "audio mix format output is missing";
        }
        return false;
    }
    std::uint16_t tag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bits = 0;
    if (!read_u16(wave_format, 0, &tag)
        || !read_u16(wave_format, 2, &channels)
        || !read_u32(wave_format, 4, &sample_rate)
        || !read_u16(wave_format, 14, &bits)
        || channels == 0
        || channels > 8
        || sample_rate < 8000
        || sample_rate > 384000) {
        if (error != nullptr) {
            *error = "audio mix format is invalid";
        }
        return false;
    }
    AudioMixFormat parsed;
    parsed.channels = channels;
    parsed.sample_rate = sample_rate;
    std::uint16_t format_tag = tag;
    if (tag == kWaveFormatExtensible) {
        std::uint16_t valid_bits = 0;
        std::uint32_t mask = 0;
        if (!read_u16(wave_format, 18, &valid_bits) || !read_u32(wave_format, 20, &mask)) {
            if (error != nullptr) {
                *error = "extensible audio mix format is truncated";
            }
            return false;
        }
        parsed.channel_mask = mask;
        parsed.valid_bits = valid_bits;
        if (guid_is(wave_format, 24, kWaveFormatIeeeFloat)) {
            format_tag = kWaveFormatIeeeFloat;
        } else if (guid_is(wave_format, 24, kWaveFormatPcm)) {
            format_tag = kWaveFormatPcm;
        } else {
            if (error != nullptr) {
                *error = "extensible audio mix subtype is unsupported";
            }
            return false;
        }
    }
    if (format_tag == kWaveFormatIeeeFloat && bits == 32) {
        parsed.kind = AudioSampleKind::kFloat32;
    } else if (format_tag == kWaveFormatPcm && bits == 16) {
        parsed.kind = AudioSampleKind::kInt16;
    } else if (format_tag == kWaveFormatPcm && bits == 24) {
        parsed.kind = AudioSampleKind::kInt24Packed;
    } else if (format_tag == kWaveFormatPcm && bits == 32) {
        parsed.kind = AudioSampleKind::kInt32;
    } else {
        if (error != nullptr) {
            *error = "audio mix sample format is unsupported";
        }
        return false;
    }
    *format = parsed;
    return true;
}

std::size_t audio_frame_stride_bytes(const AudioMixFormat& format) {
    std::size_t sample_bytes = 0;
    switch (format.kind) {
    case AudioSampleKind::kInt16: sample_bytes = 2; break;
    case AudioSampleKind::kInt24Packed: sample_bytes = 3; break;
    case AudioSampleKind::kInt32:
    case AudioSampleKind::kFloat32: sample_bytes = 4; break;
    }
    return sample_bytes * format.channels;
}

bool convert_to_stereo_float(
    const AudioMixFormat& format,
    std::span<const std::uint8_t> interleaved,
    std::vector<float>* stereo,
    std::string* error) {
    if (stereo == nullptr || format.channels == 0) {
        if (error != nullptr) {
            *error = "stereo conversion input is invalid";
        }
        return false;
    }
    const std::size_t stride = audio_frame_stride_bytes(format);
    if (stride == 0 || interleaved.size() % stride != 0) {
        if (error != nullptr) {
            *error = "audio frame size does not match the mix format";
        }
        return false;
    }
    const std::size_t frames = interleaved.size() / stride;
    stereo->resize(frames * 2);
    for (std::size_t frame_index = 0; frame_index < frames; ++frame_index) {
        const auto frame = interleaved.subspan(frame_index * stride, stride);
        float left = 0.0f;
        float right = 0.0f;
        if (format.channel_mask == 0) {
            left = sample_at(format, frame, 0);
            right = format.channels == 1 ? left : sample_at(format, frame, 1);
        } else {
            std::uint16_t channel = 0;
            for (std::uint32_t bit = 1; channel < format.channels && bit != 0; bit <<= 1) {
                if ((format.channel_mask & bit) == 0) {
                    continue;
                }
                accumulate_channel(bit, sample_at(format, frame, channel), &left, &right);
                ++channel;
            }
        }
        (*stereo)[frame_index * 2] = std::clamp(left, -1.0f, 1.0f);
        (*stereo)[frame_index * 2 + 1] = std::clamp(right, -1.0f, 1.0f);
    }
    return true;
}

bool audio_int16_frame_is_silent(std::span<const std::int16_t> interleaved) noexcept {
    for (const std::int16_t sample : interleaved) {
        if (sample > 1 || sample < -1) {
            return false;
        }
    }
    return !interleaved.empty();
}

struct StereoResampler::Impl {
#if REDCLAW_HAS_SWRESAMPLE
    SwrContext* context = nullptr;
#endif
    std::uint32_t source_rate = 0;
    double position = 0.0;
    float previous_left = 0.0f;
    float previous_right = 0.0f;
    bool has_previous = false;

    ~Impl() {
#if REDCLAW_HAS_SWRESAMPLE
        swr_free(&context);
#endif
    }
};

StereoResampler::StereoResampler() : impl_(std::make_unique<Impl>()) {}

StereoResampler::~StereoResampler() = default;

StereoResampler::StereoResampler(StereoResampler&&) noexcept = default;

StereoResampler& StereoResampler::operator=(StereoResampler&&) noexcept = default;

bool StereoResampler::reset(std::uint32_t source_rate, std::string* error) {
    if (source_rate < 8000 || source_rate > 384000) {
        if (error != nullptr) {
            *error = "audio resample rate is unsupported";
        }
        return false;
    }
#if REDCLAW_HAS_SWRESAMPLE
    swr_free(&impl_->context);
    if (source_rate != 48000) {
        AVChannelLayout input{};
        AVChannelLayout output{};
        if (av_channel_layout_from_mask(&input, AV_CH_LAYOUT_STEREO) < 0
            || av_channel_layout_from_mask(&output, AV_CH_LAYOUT_STEREO) < 0) {
            av_channel_layout_uninit(&input);
            av_channel_layout_uninit(&output);
            if (error != nullptr) {
                *error = "stereo channel layout is unavailable";
            }
            return false;
        }
        const int created = swr_alloc_set_opts2(
            &impl_->context,
            &output,
            AV_SAMPLE_FMT_FLT,
            48000,
            &input,
            AV_SAMPLE_FMT_FLT,
            static_cast<int>(source_rate),
            0,
            nullptr);
        av_channel_layout_uninit(&input);
        av_channel_layout_uninit(&output);
        if (created < 0 || swr_init(impl_->context) < 0) {
            swr_free(&impl_->context);
            if (error != nullptr) {
                *error = "audio resampler failed to initialize";
            }
            return false;
        }
    }
#endif
    impl_->source_rate = source_rate;
    impl_->position = 0.0;
    impl_->has_previous = false;
    return true;
}

bool StereoResampler::process(
    std::span<const float> stereo_interleaved,
    std::vector<float>* output_48k,
    std::string* error) {
    if (output_48k == nullptr || impl_->source_rate == 0 || stereo_interleaved.size() % 2 != 0) {
        if (error != nullptr) {
            *error = "audio resampler input is invalid";
        }
        return false;
    }
    output_48k->clear();
    if (stereo_interleaved.empty()) {
        return true;
    }
#if REDCLAW_HAS_SWRESAMPLE
    if (impl_->context != nullptr) {
        const int input_frames = static_cast<int>(stereo_interleaved.size() / 2);
        const int output_capacity = swr_get_out_samples(impl_->context, input_frames);
        if (output_capacity < 0) {
            if (error != nullptr) {
                *error = "audio resampler rejected the input";
            }
            return false;
        }
        std::vector<float> converted(static_cast<std::size_t>(output_capacity) * 2);
        const uint8_t* input_data = reinterpret_cast<const uint8_t*>(stereo_interleaved.data());
        uint8_t* output_data = reinterpret_cast<uint8_t*>(converted.data());
        const int produced = swr_convert(
            impl_->context,
            &output_data,
            output_capacity,
            &input_data,
            input_frames);
        if (produced < 0) {
            if (error != nullptr) {
                *error = "audio resample conversion failed";
            }
            return false;
        }
        converted.resize(static_cast<std::size_t>(produced) * 2);
        *output_48k = std::move(converted);
        return true;
    }
#endif
    if (impl_->source_rate == 48000) {
        output_48k->assign(stereo_interleaved.begin(), stereo_interleaved.end());
        return true;
    }
    const double step = static_cast<double>(impl_->source_rate) / 48000.0;
    const std::size_t input_frames = stereo_interleaved.size() / 2;
    std::vector<float> extended;
    extended.reserve((input_frames + 1) * 2);
    if (impl_->has_previous) {
        extended.push_back(impl_->previous_left);
        extended.push_back(impl_->previous_right);
    }
    extended.insert(extended.end(), stereo_interleaved.begin(), stereo_interleaved.end());
    const std::size_t available = extended.size() / 2;
    while (impl_->position + 1.0 < static_cast<double>(available)) {
        const auto index = static_cast<std::size_t>(impl_->position);
        const float fraction = static_cast<float>(impl_->position - static_cast<double>(index));
        const float left = extended[index * 2] + (extended[(index + 1) * 2] - extended[index * 2]) * fraction;
        const float right = extended[index * 2 + 1]
            + (extended[(index + 1) * 2 + 1] - extended[index * 2 + 1]) * fraction;
        output_48k->push_back(left);
        output_48k->push_back(right);
        impl_->position += step;
    }
    const auto consumed = static_cast<std::size_t>(impl_->position);
    impl_->position -= static_cast<double>(consumed);
    impl_->previous_left = stereo_interleaved[stereo_interleaved.size() - 2];
    impl_->previous_right = stereo_interleaved[stereo_interleaved.size() - 1];
    impl_->has_previous = true;
    return true;
}

}  // namespace redclaw::capture
