#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace redclaw::render {

class AudioOpusDecoder {
public:
    AudioOpusDecoder();
    ~AudioOpusDecoder();

    AudioOpusDecoder(const AudioOpusDecoder&) = delete;
    AudioOpusDecoder& operator=(const AudioOpusDecoder&) = delete;

    [[nodiscard]] bool open(std::string* error = nullptr);
    void reset();
    // An empty packet asks the decoder for one concealment frame.
    [[nodiscard]] bool decode(
        std::span<const std::uint8_t> opus_payload,
        std::vector<std::int16_t>* interleaved_stereo,
        std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();

    AudioPlayback(const AudioPlayback&) = delete;
    AudioPlayback& operator=(const AudioPlayback&) = delete;

    void set_enabled(bool enabled);
    void submit(std::span<const std::uint8_t> packet);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::render
