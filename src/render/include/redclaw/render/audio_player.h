#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace redclaw::render {

// PCM audio format for playback.
struct AudioFormat {
    std::uint32_t sample_rate    = 44100;
    std::uint16_t channels       = 2;
    std::uint16_t bits_per_sample = 16;
};

// Real-time PCM audio player.
//
// On Windows, uses XAudio2 2.9 (Windows 8+, no separate COM init required).
// On other platforms, all methods succeed trivially (stub).
//
// Thread-safety: play() may be called from any thread after open() returns
// true. open() and close() must be called from the same thread.
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Opens the audio output device with the given PCM format.
    //
    // buffer_count : number of ring-buffer slots (must be >= 2).
    // buffer_bytes : capacity per slot in bytes. Each play() call must pass
    //               data whose byte_count <= buffer_bytes.
    //
    // Returns false (and fills error_detail if non-null) on failure.
    bool open(const AudioFormat& fmt,
              std::uint32_t buffer_count = 4,
              std::uint32_t buffer_bytes = 65536,
              std::string* error_detail = nullptr);

    // Submits a contiguous block of interleaved PCM samples for playback.
    //
    // byte_count must be <= buffer_bytes passed to open().
    // Returns false if the ring buffer is full (the chunk is dropped) or if
    // the player is not open.
    bool play(const std::uint8_t* data,
              std::uint32_t byte_count,
              std::string* error_detail = nullptr);

    // Stops playback and releases all audio resources.  Safe to call if
    // open() was never called or already closed.
    void close();

    [[nodiscard]] bool is_open() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::render
