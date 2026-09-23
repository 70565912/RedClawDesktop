#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace redclaw::render {

struct AudioJitterFrame {
    std::uint32_t sequence = 0;
    std::uint64_t timestamp_us = 0;
    std::vector<std::uint8_t> payload;
};

enum class AudioJitterAction {
    kWaiting,
    kPacket,
    kPlc,
    kSilence,
};

struct AudioJitterPull {
    AudioJitterAction action = AudioJitterAction::kWaiting;
    AudioJitterFrame frame;
};

// Reorders a short run of Opus packets. One or two missing sequences become PLC
// while audio is active. A 100 ms gap with no following packet is silence.
class AudioJitterBuffer {
public:
    explicit AudioJitterBuffer(std::uint32_t prebuffer_frames = 3);

    void push(AudioJitterFrame frame, std::uint64_t now_us);
    [[nodiscard]] AudioJitterPull pull(std::uint64_t now_us);
    void reset();

private:
    std::uint32_t prebuffer_frames_ = 3;
    std::map<std::uint32_t, AudioJitterFrame> frames_;
    std::uint32_t next_sequence_ = 0;
    bool primed_ = false;
    bool played_ = false;
    bool have_arrival_ = false;
    std::uint64_t first_arrival_us_ = 0;
    std::uint64_t last_arrival_us_ = 0;
    std::uint64_t last_timestamp_us_ = 0;
};

}  // namespace redclaw::render
