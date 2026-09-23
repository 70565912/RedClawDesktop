#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace redclaw::capture {

class AudioOpusEncoder {
public:
    AudioOpusEncoder();
    ~AudioOpusEncoder();

    AudioOpusEncoder(const AudioOpusEncoder&) = delete;
    AudioOpusEncoder& operator=(const AudioOpusEncoder&) = delete;

    [[nodiscard]] bool open(std::string* error = nullptr);
    void close();
    // Interleaved 48 kHz stereo, 960 frames (20 ms). Empty means the frame was not encoded.
    [[nodiscard]] std::vector<std::uint8_t> encode(std::span<const std::int16_t> interleaved_stereo);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::capture
