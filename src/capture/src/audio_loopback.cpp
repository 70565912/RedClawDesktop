#include "redclaw/capture/audio_loopback.h"

#include "redclaw/capture/audio_mix.h"
#include "redclaw/protocol/audio_packet.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmreg.h>
#include <wrl/client.h>
#endif

namespace redclaw::capture {
namespace {

constexpr std::size_t kStereoFrame = redclaw::protocol::kAudioFrameSamples * redclaw::protocol::kAudioChannels;

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

std::vector<std::uint8_t> format_blob(const WAVEFORMATEX* format) {
    if (format == nullptr) {
        return {};
    }
    const std::size_t bytes = sizeof(WAVEFORMATEX) + format->cbSize;
    std::vector<std::uint8_t> blob(bytes);
    std::memcpy(blob.data(), format, bytes);
    return blob;
}

bool device_has_active_session(IMMDevice* device) {
    if (device == nullptr) {
        return false;
    }
    ComPtr<IAudioSessionManager2> manager;
    if (FAILED(device->Activate(
            __uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(manager.GetAddressOf())))) {
        return false;
    }
    ComPtr<IAudioSessionEnumerator> enumerator;
    if (FAILED(manager->GetSessionEnumerator(enumerator.GetAddressOf()))) {
        return false;
    }
    int count = 0;
    if (FAILED(enumerator->GetCount(&count))) {
        return false;
    }
    for (int index = 0; index < count; ++index) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(enumerator->GetSession(index, control.GetAddressOf()))) {
            continue;
        }
        AudioSessionState state = AudioSessionStateInactive;
        if (SUCCEEDED(control->GetState(&state)) && state == AudioSessionStateActive) {
            return true;
        }
    }
    return false;
}

struct Endpoint {
    std::wstring id;
    std::vector<std::uint8_t> format_bytes;
    AudioMixFormat format;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    StereoResampler resampler;
    std::vector<float> pending;

    bool open(IMMDevice* device, std::string* error) {
        ComPtr<IAudioClient> audio_client;
        HRESULT hr = device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audio_client.GetAddressOf()));
        if (FAILED(hr)) {
            if (error != nullptr) {
                *error = "audio endpoint activation failed";
            }
            return false;
        }
        WAVEFORMATEX* mix = nullptr;
        hr = audio_client->GetMixFormat(&mix);
        if (FAILED(hr) || mix == nullptr) {
            if (error != nullptr) {
                *error = "audio endpoint mix format is unavailable";
            }
            return false;
        }
        format_bytes = format_blob(mix);
        AudioMixFormat parsed;
        const bool parsed_ok = parse_mix_format(format_bytes, &parsed, error);
        CoTaskMemFree(mix);
        if (!parsed_ok) {
            return false;
        }
        hr = audio_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            1000000,
            0,
            reinterpret_cast<WAVEFORMATEX*>(format_bytes.data()),
            nullptr);
        if (FAILED(hr)) {
            if (error != nullptr) {
                *error = "audio loopback initialization failed";
            }
            return false;
        }
        ComPtr<IAudioCaptureClient> capture_client;
        hr = audio_client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture_client.GetAddressOf()));
        if (FAILED(hr) || FAILED(audio_client->Start())) {
            if (error != nullptr) {
                *error = "audio loopback capture failed to start";
            }
            return false;
        }
        if (!resampler.reset(parsed.sample_rate, error)) {
            audio_client->Stop();
            return false;
        }
        client = std::move(audio_client);
        capture = std::move(capture_client);
        format = parsed;
        pending.clear();
        return true;
    }

    void close() {
        if (client) {
            client->Stop();
        }
        capture.Reset();
        client.Reset();
        pending.clear();
    }

    void pull() {
        if (!capture) {
            return;
        }
        const std::size_t stride = audio_frame_stride_bytes(format);
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                break;
            }
            std::vector<float> stereo;
            const std::size_t bytes = static_cast<std::size_t>(frames) * stride;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                stereo.assign(static_cast<std::size_t>(frames) * 2, 0.0f);
            } else if (!convert_to_stereo_float(format, {data, bytes}, &stereo, nullptr)) {
                capture->ReleaseBuffer(frames);
                break;
            }
            capture->ReleaseBuffer(frames);
            std::vector<float> resampled;
            if (!resampler.process(stereo, &resampled, nullptr)) {
                continue;
            }
            pending.insert(pending.end(), resampled.begin(), resampled.end());
        }
        constexpr std::size_t kMaxPending = 48000 * 2;
        if (pending.size() > kMaxPending) {
            pending.erase(pending.begin(), pending.end() - static_cast<std::ptrdiff_t>(kStereoFrame * 4));
        }
    }
};

class LoopbackThread {
public:
    explicit LoopbackThread(AudioLoopbackCapture::FrameCallback callback)
        : callback_(std::move(callback)) {}

    bool start(std::string* error) {
        if (running_.load()) {
            return true;
        }
        running_.store(true);
        worker_ = std::thread([this] { run(); });
        (void)error;
        return true;
    }

    void stop() {
        running_.store(false);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool running() const {
        return running_.load();
    }

private:
    void run() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = com == S_OK || com == S_FALSE;
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.GetAddressOf())))) {
            running_.store(false);
            if (uninitialize) {
                CoUninitialize();
            }
            return;
        }
        std::vector<Endpoint> endpoints;
        auto next_refresh = std::chrono::steady_clock::now();
        while (running_.load()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_refresh) {
                refresh(enumerator.Get(), &endpoints);
                next_refresh = now + std::chrono::milliseconds(500);
            }
            std::vector<float> mix(kStereoFrame, 0.0f);
            bool mixed = false;
            for (auto& endpoint : endpoints) {
                endpoint.pull();
                if (endpoint.pending.size() < kStereoFrame) {
                    continue;
                }
                mixed = true;
                for (std::size_t index = 0; index < kStereoFrame; ++index) {
                    mix[index] = std::clamp(mix[index] + endpoint.pending[index], -1.0f, 1.0f);
                }
                endpoint.pending.erase(endpoint.pending.begin(), endpoint.pending.begin() + static_cast<std::ptrdiff_t>(kStereoFrame));
            }
            if (mixed && callback_) {
                std::vector<std::int16_t> pcm(kStereoFrame);
                for (std::size_t index = 0; index < kStereoFrame; ++index) {
                    pcm[index] = static_cast<std::int16_t>(std::clamp(mix[index], -1.0f, 1.0f) * 32767.0f);
                }
                if (!audio_int16_frame_is_silent(pcm)) {
                    callback_(pcm);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (auto& endpoint : endpoints) {
            endpoint.close();
        }
        if (uninitialize) {
            CoUninitialize();
        }
    }

    void refresh(IMMDeviceEnumerator* enumerator, std::vector<Endpoint>* endpoints) {
        ComPtr<IMMDeviceCollection> collection;
        if (enumerator == nullptr
            || FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.GetAddressOf()))) {
            endpoints->clear();
            return;
        }
        UINT count = 0;
        collection->GetCount(&count);
        std::vector<Endpoint> next;
        next.reserve(count);
        for (UINT index = 0; index < count; ++index) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(index, device.GetAddressOf())) || !device_has_active_session(device.Get())) {
                continue;
            }
            LPWSTR id_raw = nullptr;
            if (FAILED(device->GetId(&id_raw)) || id_raw == nullptr) {
                continue;
            }
            std::wstring id = id_raw;
            CoTaskMemFree(id_raw);
            WAVEFORMATEX* mix = nullptr;
            ComPtr<IAudioClient> probe;
            std::vector<std::uint8_t> blob;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(probe.GetAddressOf())))
                && SUCCEEDED(probe->GetMixFormat(&mix)) && mix != nullptr) {
                blob = format_blob(mix);
                CoTaskMemFree(mix);
            }
            auto existing = std::find_if(endpoints->begin(), endpoints->end(), [&](const Endpoint& endpoint) {
                return endpoint.id == id && endpoint.format_bytes == blob && endpoint.client;
            });
            if (existing != endpoints->end()) {
                next.push_back(std::move(*existing));
                endpoints->erase(existing);
                continue;
            }
            Endpoint opened;
            opened.id = id;
            if (opened.open(device.Get(), nullptr)) {
                next.push_back(std::move(opened));
            }
        }
        for (auto& endpoint : *endpoints) {
            endpoint.close();
        }
        *endpoints = std::move(next);
    }

    AudioLoopbackCapture::FrameCallback callback_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

#endif

}  // namespace

struct AudioLoopbackCapture::Impl {
#if defined(_WIN32)
    LoopbackThread thread;
    explicit Impl(FrameCallback callback) : thread(std::move(callback)) {}
#else
    FrameCallback callback;
    bool running = false;
    explicit Impl(FrameCallback frame_callback) : callback(std::move(frame_callback)) {}
#endif
};

AudioLoopbackCapture::AudioLoopbackCapture(FrameCallback callback)
    : impl_(std::make_unique<Impl>(std::move(callback))) {}

AudioLoopbackCapture::~AudioLoopbackCapture() {
    stop();
}

bool AudioLoopbackCapture::start(std::string* error) {
#if defined(_WIN32)
    return impl_->thread.start(error);
#else
    if (error != nullptr) {
        *error = "audio loopback capture is unavailable on this platform";
    }
    return false;
#endif
}

void AudioLoopbackCapture::stop() {
#if defined(_WIN32)
    impl_->thread.stop();
#endif
}

bool AudioLoopbackCapture::running() const {
#if defined(_WIN32)
    return impl_->thread.running();
#else
    return false;
#endif
}

}  // namespace redclaw::capture
