#include "redclaw/capture/audio_encoder.h"
#include "redclaw/capture/audio_mix.h"
#include "redclaw/render/audio_decoder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

void append_u16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFF));
    out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void append_u32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

std::vector<std::uint8_t> wave_format(
    std::uint16_t tag, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits) {
    std::vector<std::uint8_t> bytes;
    append_u16(&bytes, tag);
    append_u16(&bytes, channels);
    append_u32(&bytes, rate);
    append_u32(&bytes, rate * channels * bits / 8);
    append_u16(&bytes, static_cast<std::uint16_t>(channels * bits / 8));
    append_u16(&bytes, bits);
    return bytes;
}

TEST(AudioMix, ConvertsPcm16Float24BitAndSurround) {
    const auto pcm_format_bytes = wave_format(1, 2, 48000, 16);
    redclaw::capture::AudioMixFormat pcm;
    ASSERT_TRUE(redclaw::capture::parse_mix_format(pcm_format_bytes, &pcm));
    const std::uint8_t pcm_bytes[] = {0x00, 0x40, 0x00, 0xC0};
    std::vector<float> stereo;
    ASSERT_TRUE(redclaw::capture::convert_to_stereo_float(pcm, pcm_bytes, &stereo));
    ASSERT_EQ(stereo.size(), 2U);
    EXPECT_NEAR(stereo[0], 0.5f, 0.01f);
    EXPECT_NEAR(stereo[1], -0.5f, 0.01f);

    redclaw::capture::AudioMixFormat mono_float;
    mono_float.kind = redclaw::capture::AudioSampleKind::kFloat32;
    mono_float.channels = 1;
    mono_float.sample_rate = 48000;
    const float input = 0.25f;
    std::vector<std::uint8_t> float_bytes(sizeof(float));
    std::memcpy(float_bytes.data(), &input, sizeof(input));
    ASSERT_TRUE(redclaw::capture::convert_to_stereo_float(mono_float, float_bytes, &stereo));
    EXPECT_NEAR(stereo[0], 0.25f, 0.0001f);
    EXPECT_NEAR(stereo[1], 0.25f, 0.0001f);

    redclaw::capture::AudioMixFormat packed;
    packed.kind = redclaw::capture::AudioSampleKind::kInt24Packed;
    packed.channels = 1;
    packed.sample_rate = 44100;
    const std::uint8_t sample24[] = {0x00, 0x00, 0x40};
    ASSERT_TRUE(redclaw::capture::convert_to_stereo_float(packed, sample24, &stereo));
    EXPECT_NEAR(stereo[0], 0.5f, 0.01f);
    EXPECT_NEAR(stereo[1], 0.5f, 0.01f);

    redclaw::capture::AudioMixFormat surround;
    surround.kind = redclaw::capture::AudioSampleKind::kFloat32;
    surround.channels = 6;
    surround.sample_rate = 48000;
    surround.channel_mask = 0x3F;
    std::vector<float> frame(6, 0.0f);
    frame[2] = 1.0f;
    std::vector<std::uint8_t> surround_bytes(sizeof(float) * frame.size());
    std::memcpy(surround_bytes.data(), frame.data(), surround_bytes.size());
    ASSERT_TRUE(redclaw::capture::convert_to_stereo_float(surround, surround_bytes, &stereo));
    EXPECT_NEAR(stereo[0], 0.7071f, 0.02f);
    EXPECT_NEAR(stereo[1], 0.7071f, 0.02f);
}

TEST(AudioMix, SilenceProducesNoSendableFrame) {
    std::vector<std::int16_t> silent(960 * 2, 0);
    EXPECT_TRUE(redclaw::capture::audio_int16_frame_is_silent(silent));
    silent[10] = 40;
    EXPECT_FALSE(redclaw::capture::audio_int16_frame_is_silent(silent));
    EXPECT_FALSE(redclaw::capture::audio_int16_frame_is_silent({}));
}

TEST(AudioMix, ResamplesHighRateStereoToward48k) {
    redclaw::capture::StereoResampler resampler;
    ASSERT_TRUE(resampler.reset(96000));
    std::vector<float> input(9600 * 2, 0.25f);
    std::vector<float> output;
    ASSERT_TRUE(resampler.process(input, &output));
    ASSERT_GT(output.size(), 4000U);
    double sum = 0.0;
    const std::size_t tail = std::min<std::size_t>(output.size(), 1000);
    for (std::size_t index = output.size() - tail; index < output.size(); ++index) {
        sum += output[index];
    }
    EXPECT_NEAR(sum / static_cast<double>(tail), 0.25, 0.05);
}

TEST(AudioOpus, RoundTripPreservesAudibleEnergy) {
    redclaw::capture::AudioOpusEncoder encoder;
    redclaw::render::AudioOpusDecoder decoder;
    ASSERT_TRUE(encoder.open());
    ASSERT_TRUE(decoder.open());
    std::vector<std::int16_t> pcm(960 * 2);
    for (std::size_t frame = 0; frame < 960; ++frame) {
        const auto sample = static_cast<std::int16_t>(std::sin(frame * 0.08) * 8000.0);
        pcm[frame * 2] = sample;
        pcm[frame * 2 + 1] = sample;
    }
    const auto encoded = encoder.encode(pcm);
    ASSERT_FALSE(encoded.empty());
    std::vector<std::int16_t> decoded;
    ASSERT_TRUE(decoder.decode(encoded, &decoded));
    ASSERT_EQ(decoded.size(), pcm.size());
    double energy = 0.0;
    for (const auto sample : decoded) {
        energy += static_cast<double>(sample) * sample;
    }
    EXPECT_GT(energy / static_cast<double>(decoded.size()), 1000.0);

    std::vector<std::int16_t> concealed;
    ASSERT_TRUE(decoder.decode({}, &concealed));
    EXPECT_EQ(concealed.size(), pcm.size());
}

}  // namespace
