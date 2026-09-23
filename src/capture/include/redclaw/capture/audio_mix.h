#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace redclaw::capture {

enum class AudioSampleKind {
    kInt16,
    kInt24Packed,
    kInt32,
    kFloat32,
};

struct AudioMixFormat {
    AudioSampleKind kind = AudioSampleKind::kFloat32;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t channel_mask = 0;
    // Zero uses the full container. 24 means a left-aligned sample in a 32-bit container.
    std::uint16_t valid_bits = 0;
};

[[nodiscard]] bool parse_mix_format(
    std::span<const std::uint8_t> wave_format,
    AudioMixFormat* format,
    std::string* error = nullptr);

[[nodiscard]] std::size_t audio_frame_stride_bytes(const AudioMixFormat& format);

// Interleaved source frames become stereo float at the source sample rate.
[[nodiscard]] bool convert_to_stereo_float(
    const AudioMixFormat& format,
    std::span<const std::uint8_t> interleaved,
    std::vector<float>* stereo,
    std::string* error = nullptr);

[[nodiscard]] bool audio_int16_frame_is_silent(std::span<const std::int16_t> interleaved) noexcept;

class StereoResampler {
public:
    StereoResampler();
    ~StereoResampler();

    StereoResampler(const StereoResampler&) = delete;
    StereoResampler& operator=(const StereoResampler&) = delete;
    StereoResampler(StereoResampler&&) noexcept;
    StereoResampler& operator=(StereoResampler&&) noexcept;

    [[nodiscard]] bool reset(std::uint32_t source_rate, std::string* error = nullptr);
    [[nodiscard]] bool process(
        std::span<const float> stereo_interleaved,
        std::vector<float>* output_48k,
        std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::capture
