#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace redclaw::runtime {

// Host captures and sends when the Controller has requested playback.
// Controller decodes and plays. The audio channel itself is opened by the Host.
class AudioStreamRuntime {
public:
    AudioStreamRuntime(
        bool host,
        std::function<bool(std::span<const std::uint8_t>)> send_packet,
        std::function<bool()> ensure_channel);
    ~AudioStreamRuntime();

    AudioStreamRuntime(const AudioStreamRuntime&) = delete;
    AudioStreamRuntime& operator=(const AudioStreamRuntime&) = delete;

    void observe_peer(std::uint32_t audio_version, bool playback_requested, bool apply_request);
    void set_playback_requested(bool requested);
    void set_channel_open(bool open);
    void on_packet(std::span<const std::uint8_t> packet);
    void pump();
    [[nodiscard]] bool playback_requested() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::runtime
