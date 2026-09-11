#include "redclaw/render/audio_player.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Windows / XAudio2 implementation
// ---------------------------------------------------------------------------
#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <xaudio2.h>
#include <wrl/client.h>

namespace redclaw::render {

namespace {

std::string hresult_str(HRESULT hr) {
    char buf[16];
    sprintf_s(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
    return buf;
}

}  // namespace

class AudioPlayer::Impl {
public:
    ~Impl() { close(); }

    bool open(const AudioFormat& fmt,
              std::uint32_t buffer_count,
              std::uint32_t buffer_bytes,
              std::string* error_detail) {
        close();

        if (buffer_count < 2) {
            if (error_detail != nullptr) {
                *error_detail = "buffer_count must be at least 2";
            }
            return false;
        }
        if (buffer_bytes == 0) {
            if (error_detail != nullptr) {
                *error_detail = "buffer_bytes must be > 0";
            }
            return false;
        }

        UINT32 flags = 0;
#if defined(_DEBUG)
        flags |= XAUDIO2_DEBUG_ENGINE;
#endif
        HRESULT hr = XAudio2Create(xaudio2_.GetAddressOf(), flags);
        if (FAILED(hr)) {
            if (error_detail != nullptr) {
                *error_detail = "XAudio2Create failed: " + hresult_str(hr);
            }
            return false;
        }

        hr = xaudio2_->CreateMasteringVoice(&mastering_voice_);
        if (FAILED(hr)) {
            if (error_detail != nullptr) {
                *error_detail = "CreateMasteringVoice failed: " + hresult_str(hr);
            }
            close();
            return false;
        }

        WAVEFORMATEX wfx{};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = fmt.channels;
        wfx.nSamplesPerSec  = fmt.sample_rate;
        wfx.wBitsPerSample  = fmt.bits_per_sample;
        wfx.nBlockAlign     = static_cast<WORD>((wfx.nChannels * wfx.wBitsPerSample) / 8);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        wfx.cbSize          = 0;

        hr = xaudio2_->CreateSourceVoice(&source_voice_, &wfx);
        if (FAILED(hr)) {
            if (error_detail != nullptr) {
                *error_detail = "CreateSourceVoice failed: " + hresult_str(hr);
            }
            close();
            return false;
        }

        // Start the source voice once; subsequent SubmitSourceBuffer calls
        // will be played immediately while the voice is running.
        hr = source_voice_->Start(0);
        if (FAILED(hr)) {
            if (error_detail != nullptr) {
                *error_detail = "IXAudio2SourceVoice::Start failed: " + hresult_str(hr);
            }
            close();
            return false;
        }

        // Allocate the ring-buffer slots.
        {
            std::lock_guard<std::mutex> lk(mutex_);
            buffer_count_ = buffer_count;
            buffer_bytes_ = buffer_bytes;
            slots_.resize(buffer_count);
            for (auto& slot : slots_) {
                slot.resize(buffer_bytes, 0);
            }
            submit_count_ = 0;
        }

        is_open_ = true;
        return true;
    }

    // Submit a PCM chunk for playback.
    //
    // Ring-buffer safety proof:
    //   Let N = buffer_count_, k = submit_count_ at call time.
    //   We want to write to slot k % N.
    //   XAudio2 currently holds slots [(k-queued)%N .. (k-1)%N].
    //   For k%N to collide with any of those, we'd need j*N == 0 for some
    //   j in [1, queued], i.e. j == N.  But queued < N, so no collision.
    bool play(const std::uint8_t* data,
              std::uint32_t byte_count,
              std::string* error_detail) {
        if (!is_open_ || source_voice_ == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "audio player is not open";
            }
            return false;
        }
        if (data == nullptr || byte_count == 0) {
            if (error_detail != nullptr) {
                *error_detail = "empty audio data";
            }
            return false;
        }

        std::lock_guard<std::mutex> lk(mutex_);

        if (byte_count > buffer_bytes_) {
            if (error_detail != nullptr) {
                *error_detail = "audio chunk size exceeds ring-buffer slot capacity";
            }
            return false;
        }

        // Poll how many buffers XAudio2 is still consuming.
        XAUDIO2_VOICE_STATE state{};
        source_voice_->GetState(&state);
        const UINT32 queued = state.BuffersQueued;

        if (queued >= buffer_count_) {
            // All slots are in flight; drop this chunk to avoid overwriting
            // data that XAudio2 is still reading.
            if (error_detail != nullptr) {
                *error_detail = "audio ring buffer full; chunk dropped";
            }
            return false;
        }

        const std::uint32_t slot = submit_count_ % buffer_count_;
        ++submit_count_;

        // Copy PCM data into the ring-buffer slot.
        auto& buf = slots_[slot];
        std::copy(data, data + byte_count, buf.data());

        XAUDIO2_BUFFER xbuf{};
        xbuf.pAudioData  = buf.data();
        xbuf.AudioBytes  = byte_count;
        xbuf.Flags       = 0;  // keep playing after this buffer

        HRESULT hr = source_voice_->SubmitSourceBuffer(&xbuf);
        if (FAILED(hr)) {
            --submit_count_;  // undo the increment on failure
            if (error_detail != nullptr) {
                *error_detail = "SubmitSourceBuffer failed: " + hresult_str(hr);
            }
            return false;
        }

        return true;
    }

    void close() {
        is_open_ = false;

        if (source_voice_ != nullptr) {
            source_voice_->Stop(0);
            source_voice_->FlushSourceBuffers();
            source_voice_->DestroyVoice();
            source_voice_ = nullptr;
        }
        if (mastering_voice_ != nullptr) {
            mastering_voice_->DestroyVoice();
            mastering_voice_ = nullptr;
        }
        xaudio2_.Reset();

        std::lock_guard<std::mutex> lk(mutex_);
        slots_.clear();
        buffer_count_ = 0;
        buffer_bytes_ = 0;
        submit_count_ = 0;
    }

    [[nodiscard]] bool is_open() const { return is_open_; }

private:
    Microsoft::WRL::ComPtr<IXAudio2> xaudio2_;
    IXAudio2MasteringVoice* mastering_voice_ = nullptr;
    IXAudio2SourceVoice*    source_voice_    = nullptr;

    std::mutex mutex_;
    std::vector<std::vector<std::uint8_t>> slots_;
    std::uint32_t buffer_count_ = 0;
    std::uint32_t buffer_bytes_ = 0;
    std::uint32_t submit_count_ = 0;

    bool is_open_ = false;
};

}  // namespace redclaw::render

// ---------------------------------------------------------------------------
// Non-Windows stub
// ---------------------------------------------------------------------------
#else

namespace redclaw::render {

class AudioPlayer::Impl {
public:
    bool open(const AudioFormat&, std::uint32_t, std::uint32_t,
              std::string* error_detail) {
        if (error_detail != nullptr) {
            *error_detail = "AudioPlayer is not available on this platform";
        }
        return false;
    }

    bool play(const std::uint8_t*, std::uint32_t,
              std::string* error_detail) {
        if (error_detail != nullptr) {
            *error_detail = "AudioPlayer is not available on this platform";
        }
        return false;
    }

    void close() {}

    [[nodiscard]] bool is_open() const { return false; }
};

}  // namespace redclaw::render

#endif

// ---------------------------------------------------------------------------
// Public interface - delegates to Impl
// ---------------------------------------------------------------------------
namespace redclaw::render {

AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}

AudioPlayer::~AudioPlayer() = default;

bool AudioPlayer::open(const AudioFormat& fmt,
                       std::uint32_t buffer_count,
                       std::uint32_t buffer_bytes,
                       std::string* error_detail) {
    return impl_->open(fmt, buffer_count, buffer_bytes, error_detail);
}

bool AudioPlayer::play(const std::uint8_t* data,
                       std::uint32_t byte_count,
                       std::string* error_detail) {
    return impl_->play(data, byte_count, error_detail);
}

void AudioPlayer::close() {
    impl_->close();
}

bool AudioPlayer::is_open() const {
    return impl_->is_open();
}

}  // namespace redclaw::render
