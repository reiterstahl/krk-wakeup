#include "audio.hpp"
#include "playback.hpp"
#include "signal.hpp"
#include "wasapi_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace {
PulseResult Failure(HRESULT hr, const char* stage, bool unavailable = false,
                    bool blockedByVolume = false) {
    return {false, unavailable, hr, stage, blockedByVolume};
}
}  // namespace

PulseResult RenderPulse(const std::wstring& endpointId, unsigned durationMs,
                        unsigned frequencyHz, unsigned levelPercent, unsigned flags) {
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
    device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                     reinterpret_cast<void**>(endpointVolume.GetAddressOf()));
    if (WasapiUtil::ReadPathGain(endpointVolume.Get(), nullptr).muted)
        return Failure(HRESULT_FROM_WIN32(ERROR_NOT_READY), "endpoint_muted", false, true);

    // Decide before opening our own stream: with nothing else playing, the
    // DAC is idle and would swallow the start of the pulse.
    const bool coldOutput = !WasapiUtil::HasActiveSession(device.Get());

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return Failure(hr, "activate");

    WAVEFORMATEX* rawFormat = nullptr;
    hr = client->GetMixFormat(&rawFormat);
    if (FAILED(hr)) return Failure(hr, "mix_format");
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> wave(rawFormat, CoTaskMemFree);
    Signal::Format format;
    if (!WasapiUtil::DecodeFormat(wave.get(), format))
        return Failure(AUDCLNT_E_UNSUPPORTED_FORMAT, "decode_format");

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, wave.get(), nullptr);
    if (FAILED(hr)) return Failure(hr, "initialize");
    WasapiUtil::OptOutOfDucking(client.Get());
    ComPtr<ISimpleAudioVolume> sessionVolume;
    client->GetService(IID_PPV_ARGS(&sessionVolume));
    WasapiUtil::PathGain path = WasapiUtil::ReadPathGain(endpointVolume.Get(), sessionVolume.Get());
    if (path.muted) return Failure(HRESULT_FROM_WIN32(ERROR_NOT_READY), "session_muted", false, true);
    ComPtr<IAudioRenderClient> render;
    hr = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(hr)) return Failure(hr, "render_service");
    UINT32 bufferFrames = 0;
    hr = client->GetBufferSize(&bufferFrames);
    if (FAILED(hr) || bufferFrames == 0)
        return Failure(FAILED(hr) ? hr : E_UNEXPECTED, "buffer_size");

    const Signal::PulseShape shape = Signal::MakePulse(format.sampleRate, durationMs,
                                                       frequencyHz, coldOutput);
    const uint64_t total = shape.Total();
    const bool compensate = (flags & kPulseCompensateVolume) != 0;
    double level = Signal::CompensatedLevel(levelPercent / 100.0, compensate, path.known, path.gain);
    ULONGLONG gainReadTick = GetTickCount64();
    const auto fill = [&](BYTE* buffer, UINT32 frames, uint64_t offset) {
        // Follow volume changes during the pulse; a reduction of the digital
        // level is what keeps a sudden volume increase from sounding loud.
        const ULONGLONG tick = GetTickCount64();
        if (compensate && tick - gainReadTick >= 50) {
            gainReadTick = tick;
            path = WasapiUtil::ReadPathGain(endpointVolume.Get(), sessionVolume.Get());
            level = Signal::CompensatedLevel(levelPercent / 100.0, true, path.known, path.gain);
        }
        for (UINT32 frame = 0; frame < frames; ++frame) {
            Signal::WriteFrame(buffer + static_cast<size_t>(frame) * format.bytesPerFrame,
                               format, shape.Sample(offset + frame, level));
        }
    };

    const UINT32 initial = static_cast<UINT32>(std::min<uint64_t>(bufferFrames, total));
    BYTE* initialBuffer = nullptr;
    hr = render->GetBuffer(initial, &initialBuffer);
    if (FAILED(hr)) return Failure(hr, "prefill_buffer");
    fill(initialBuffer, initial, 0);
    hr = render->ReleaseBuffer(initial, 0);
    if (FAILED(hr)) return Failure(hr, "prefill_release");
    hr = client->Start();
    if (FAILED(hr)) return Failure(hr, "start");

    uint64_t written = initial;
    const char* stage = "write";
    const ULONGLONG deadline = GetTickCount64() + total * 1000 / format.sampleRate + 5000;
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
        fill(buffer, count, written);
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
    return {true, false, S_OK, coldOutput ? "complete_cold" : "complete", false};
}
