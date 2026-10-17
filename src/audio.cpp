#include "audio.hpp"
#include "playback.hpp"

#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace {
constexpr double kTau = 6.2831853071795864769;
constexpr GUID kPcm = {1, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
constexpr GUID kFloat = {3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};

struct Format {
    bool floating = false;
    unsigned bits = 0;
    unsigned channels = 0;
    unsigned bytesPerFrame = 0;
    unsigned sampleRate = 0;
};

bool DecodeFormat(const WAVEFORMATEX* wave, Format& out) {
    if (!wave || !wave->nChannels || !wave->nSamplesPerSec) return false;
    bool floating = false;
    if (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (wave->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
        if (IsEqualGUID(ext->SubFormat, kFloat)) floating = true;
        else if (!IsEqualGUID(ext->SubFormat, kPcm)) return false;
    } else if (wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        floating = true;
    } else if (wave->wFormatTag != WAVE_FORMAT_PCM) {
        return false;
    }
    const unsigned bits = wave->wBitsPerSample;
    if (floating ? bits != 32 : (bits != 8 && bits != 16 && bits != 24 && bits != 32)) return false;
    if (wave->nBlockAlign != wave->nChannels * (bits / 8)) return false;
    out = {floating, bits, wave->nChannels, wave->nBlockAlign, wave->nSamplesPerSec};
    return true;
}

void WriteSample(BYTE* target, const Format& format, double value) {
    value = std::clamp(value, -1.0, 1.0);
    if (format.floating) {
        const float sample = static_cast<float>(value);
        std::memcpy(target, &sample, sizeof(sample));
    } else if (format.bits == 8) {
        target[0] = static_cast<BYTE>(std::lround(128.0 + value * 127.0));
    } else if (format.bits == 16) {
        const int16_t sample = static_cast<int16_t>(std::lround(value * 32767.0));
        std::memcpy(target, &sample, sizeof(sample));
    } else if (format.bits == 24) {
        const int32_t sample = static_cast<int32_t>(std::lround(value * 8388607.0));
        target[0] = static_cast<BYTE>(sample);
        target[1] = static_cast<BYTE>(sample >> 8);
        target[2] = static_cast<BYTE>(sample >> 16);
    } else {
        const int32_t sample = static_cast<int32_t>(std::llround(value * 2147483647.0));
        std::memcpy(target, &sample, sizeof(sample));
    }
}

void FillBuffer(BYTE* buffer, UINT32 frames, uint64_t offset, uint64_t total,
                const Format& format, unsigned frequency, unsigned level) {
    const uint64_t fadeFrames = std::max<uint64_t>(1, std::min<uint64_t>(
        format.sampleRate / 50, total / 10));
    const unsigned sampleBytes = format.bits / 8;
    for (UINT32 frame = 0; frame < frames; ++frame) {
        const uint64_t position = offset + frame;
        const double fadeIn = std::min(1.0, static_cast<double>(position) / fadeFrames);
        const double fadeOut = std::min(1.0, static_cast<double>(total - position - 1) / fadeFrames);
        const double gain = (level / 100.0) * std::min(fadeIn, fadeOut);
        const double value = gain * std::sin(kTau * frequency *
            static_cast<double>(position) / format.sampleRate);
        BYTE* samples = buffer + static_cast<size_t>(frame) * format.bytesPerFrame;
        for (unsigned channel = 0; channel < format.channels; ++channel) {
            WriteSample(samples + channel * sampleBytes, format, value);
        }
    }
}

PulseResult Failure(HRESULT hr, const char* stage, bool unavailable = false,
                    bool blockedByVolume = false) {
    return {false, unavailable, hr, stage, blockedByVolume};
}
}  // namespace

PulseResult RenderPulse(const std::wstring& endpointId, unsigned durationMs,
                        unsigned frequencyHz, unsigned levelPercent) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return Failure(com, "com");
    struct ComScope { ~ComScope() { CoUninitialize(); } } comScope;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return Failure(hr, "enumerator");

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDevice(endpointId.c_str(), &device);
    if (FAILED(hr)) return Failure(hr, "device", true);
    DWORD state = 0;
    hr = device->GetState(&state);
    if (FAILED(hr) || state != DEVICE_STATE_ACTIVE)
        return Failure(FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED),
                       "device_state", true);

    // A successfully consumed buffer can still be inaudible when Windows has
    // muted the endpoint or this app. Report that condition without changing it.
    ComPtr<IAudioEndpointVolume> endpointVolume;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(endpointVolume.GetAddressOf())))) {
        BOOL muted = FALSE;
        float level = 1.0f;
        endpointVolume->GetMute(&muted);
        endpointVolume->GetMasterVolumeLevelScalar(&level);
        if (muted || level <= 0.0f)
            return Failure(HRESULT_FROM_WIN32(ERROR_NOT_READY), "endpoint_muted", false, true);
    }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return Failure(hr, "activate");

    WAVEFORMATEX* rawFormat = nullptr;
    hr = client->GetMixFormat(&rawFormat);
    if (FAILED(hr)) return Failure(hr, "mix_format");
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> wave(rawFormat, CoTaskMemFree);
    Format format;
    if (!DecodeFormat(wave.get(), format)) return Failure(AUDCLNT_E_UNSUPPORTED_FORMAT, "decode_format");

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, wave.get(), nullptr);
    if (FAILED(hr)) return Failure(hr, "initialize");
    ComPtr<ISimpleAudioVolume> sessionVolume;
    if (SUCCEEDED(client->GetService(IID_PPV_ARGS(&sessionVolume)))) {
        BOOL muted = FALSE;
        float level = 1.0f;
        sessionVolume->GetMute(&muted);
        sessionVolume->GetMasterVolume(&level);
        if (muted || level <= 0.0f)
            return Failure(HRESULT_FROM_WIN32(ERROR_NOT_READY), "session_muted", false, true);
    }
    ComPtr<IAudioRenderClient> render;
    hr = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(hr)) return Failure(hr, "render_service");
    UINT32 bufferFrames = 0;
    hr = client->GetBufferSize(&bufferFrames);
    if (FAILED(hr) || bufferFrames == 0)
        return Failure(FAILED(hr) ? hr : E_UNEXPECTED, "buffer_size");

    const uint64_t total = std::max<uint64_t>(1,
        static_cast<uint64_t>(durationMs) * format.sampleRate / 1000);
    const UINT32 initial = static_cast<UINT32>(std::min<uint64_t>(bufferFrames, total));
    BYTE* initialBuffer = nullptr;
    hr = render->GetBuffer(initial, &initialBuffer);
    if (FAILED(hr)) return Failure(hr, "prefill_buffer");
    FillBuffer(initialBuffer, initial, 0, total, format, frequencyHz, levelPercent);
    hr = render->ReleaseBuffer(initial, 0);
    if (FAILED(hr)) return Failure(hr, "prefill_release");
    hr = client->Start();
    if (FAILED(hr)) return Failure(hr, "start");

    uint64_t written = initial;
    const char* stage = "write";
    const ULONGLONG deadline = GetTickCount64() + durationMs + 5000;
    while (written < total) {
        if (GetTickCount64() >= deadline) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            stage = "write_timeout";
            break;
        }
        UINT32 padding = 0;
        stage = "padding";
        hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr)) break;
        const UINT32 freeFrames = bufferFrames > padding ? bufferFrames - padding : 0;
        const UINT32 count = static_cast<UINT32>(std::min<uint64_t>(freeFrames, total - written));
        if (!count) {
            Sleep(5);
            continue;
        }
        BYTE* buffer = nullptr;
        stage = "buffer";
        hr = render->GetBuffer(count, &buffer);
        if (FAILED(hr)) break;
        FillBuffer(buffer, count, written, total, format, frequencyHz, levelPercent);
        stage = "release";
        hr = render->ReleaseBuffer(count, 0);
        if (FAILED(hr)) break;
        written += count;
    }
    if (SUCCEEDED(hr)) {
        stage = "drain";
        const auto drained = DrainPlayback(
            [&](uint32_t& padding) {
                hr = client->GetCurrentPadding(&padding);
                return SUCCEEDED(hr);
            }, [] { return GetTickCount64(); }, [] { Sleep(5); }, 1500);
        if (drained == DrainResult::TimedOut) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            stage = "drain_timeout";
        }
    }
    const HRESULT stopped = client->Stop();
    if (FAILED(hr)) return Failure(hr, stage);
    if (FAILED(stopped)) return Failure(stopped, "stop");
    return {true, false, S_OK, "complete", false};
}
