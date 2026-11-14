#pragma once

// Small WASAPI helpers shared by the pulse renderer and the keep-alive stream.

#include "signal.hpp"

#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

namespace WasapiUtil {
using Microsoft::WRL::ComPtr;

constexpr GUID kPcmFormat = {1, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
constexpr GUID kFloatFormat = {3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};

inline bool DecodeFormat(const WAVEFORMATEX* wave, Signal::Format& out) {
    if (!wave || !wave->nChannels || !wave->nSamplesPerSec) return false;
    bool floating = false;
    if (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (wave->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
        if (IsEqualGUID(ext->SubFormat, kFloatFormat)) floating = true;
        else if (!IsEqualGUID(ext->SubFormat, kPcmFormat)) return false;
    } else if (wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        floating = true;
    } else if (wave->wFormatTag != WAVE_FORMAT_PCM) {
        return false;
    }
    const unsigned bits = wave->wBitsPerSample;
    if (floating ? bits != 32 : (bits != 8 && bits != 16 && bits != 24 && bits != 32)) return false;
    if (wave->nBlockAlign != wave->nChannels * (bits / 8)) return false;
    out.floating = floating;
    out.bits = bits;
    out.channels = wave->nChannels;
    out.bytesPerFrame = wave->nBlockAlign;
    out.sampleRate = wave->nSamplesPerSec;
    return true;
}

// What Windows does to our samples after we write them.
struct PathGain {
    bool known = false;   // false when the output exposes no volume control
    bool muted = false;   // output or this application muted in Windows
    double gain = 1.0;    // linear, output volume times application volume
};

inline PathGain ReadPathGain(IAudioEndpointVolume* endpoint, ISimpleAudioVolume* session) {
    PathGain path;
    if (endpoint) {
        BOOL muted = FALSE;
        float decibels = 0.0f;
        float scalar = 1.0f;
        if (SUCCEEDED(endpoint->GetMute(&muted)) && muted) path.muted = true;
        if (SUCCEEDED(endpoint->GetMasterVolumeLevelScalar(&scalar)) && scalar <= 0.0f)
            path.muted = true;
        if (SUCCEEDED(endpoint->GetMasterVolumeLevel(&decibels)) && std::isfinite(decibels)) {
            path.known = true;
            path.gain = std::min(1.0, Signal::DecibelsToLinear(decibels));
        }
    }
    if (session) {
        BOOL muted = FALSE;
        float level = 1.0f;
        if (SUCCEEDED(session->GetMute(&muted)) && muted) path.muted = true;
        if (SUCCEEDED(session->GetMasterVolume(&level)) && std::isfinite(level)) {
            if (level <= 0.0f) path.muted = true;
            else path.gain *= std::min(1.0f, level);
        }
    }
    return path;
}

// Windows lowers every other stream while a call is in progress ("ducking"),
// by 80 % with its default setting. A keep-awake signal must not shrink
// because the user answered a call on a headset.
inline void OptOutOfDucking(IAudioClient* client) {
    ComPtr<IAudioSessionControl> control;
    if (FAILED(client->GetService(IID_PPV_ARGS(&control)))) return;
    ComPtr<IAudioSessionControl2> control2;
    if (SUCCEEDED(control.As(&control2))) control2->SetDuckingPreference(TRUE);
}

// True when some stream is already playing on this output, which means the
// audio engine and the DAC behind it are running.
inline bool HasActiveSession(IMMDevice* device) {
    ComPtr<IAudioSessionManager2> manager;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(manager.GetAddressOf())))) return false;
    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(manager->GetSessionEnumerator(&sessions))) return false;
    int count = 0;
    if (FAILED(sessions->GetCount(&count))) return false;
    for (int index = 0; index < count; ++index) {
        ComPtr<IAudioSessionControl> session;
        AudioSessionState state = AudioSessionStateInactive;
        if (SUCCEEDED(sessions->GetSession(index, &session)) &&
            SUCCEEDED(session->GetState(&state)) && state == AudioSessionStateActive) return true;
    }
    return false;
}
}  // namespace WasapiUtil
