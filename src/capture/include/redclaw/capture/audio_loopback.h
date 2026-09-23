#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace redclaw::capture {

// Captures shared-mode output from every active render endpoint and emits
// non-silent 20 ms 48 kHz stereo frames. WASAPI exclusive and ASIO bypass the
// engine and are not visible here.
class AudioLoopbackCapture {
public:
    using FrameCallback = std::function<void(std::span<const std::int16_t> interleaved_stereo)>;

    explicit AudioLoopbackCapture(FrameCallback callback);
    ~AudioLoopbackCapture();

    AudioLoopbackCapture(const AudioLoopbackCapture&) = delete;
    AudioLoopbackCapture& operator=(const AudioLoopbackCapture&) = delete;

    [[nodiscard]] bool start(std::string* error = nullptr);
    void stop();
    [[nodiscard]] bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::capture
