#include "audio_activity.hpp"

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
constexpr auto kPollInterval = std::chrono::milliseconds(100);
constexpr auto kReconnectInterval = std::chrono::seconds(1);

std::string UtcNow() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char text[32]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
                  static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
                  static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond),
                  static_cast<unsigned>(time.wMilliseconds));
    return text;
}

struct Meter {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioMeterInformation> peaks;
    ComPtr<IAudioEndpointVolume> volume;
    AudioMeterCoverage coverage = AudioMeterCoverage::Unknown;

    HRESULT Connect(const std::wstring& endpointId, const char*& stage) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        stage = "activity_enumerator";
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) return hr;
        stage = "activity_device";
        hr = enumerator->GetDevice(endpointId.c_str(), &device);
        if (FAILED(hr)) return hr;
        DWORD state = 0;
        stage = "activity_device_state";
        hr = device->GetState(&state);
        if (FAILED(hr)) return hr;
        if (state != DEVICE_STATE_ACTIVE)
            return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
        stage = "activity_meter";
        hr = device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(peaks.GetAddressOf()));
        if (FAILED(hr)) return hr;
        DWORD hardware = 0;
        if (SUCCEEDED(peaks->QueryHardwareSupport(&hardware))) {
            coverage = (hardware & ENDPOINT_HARDWARE_SUPPORT_METER)
                ? AudioMeterCoverage::SharedAndExclusive
                : AudioMeterCoverage::SharedModeOnly;
        }
        // These read-only controls qualify the observation. A driver that does
        // not expose them must not prevent its peak meter from being sampled.
        device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(volume.GetAddressOf()));
        return S_OK;
    }

    HRESULT Read(AudioActivitySnapshot& sample) const {
        sample.stage = "activity_device_state";
        DWORD state = 0;
        HRESULT hr = device->GetState(&state);
        if (FAILED(hr)) return hr;
        if (state != DEVICE_STATE_ACTIVE)
            return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
        sample.stage = "activity_peak";
        hr = peaks->GetPeakValue(&sample.peak);
        if (FAILED(hr)) return hr;
        if (!std::isfinite(sample.peak) || sample.peak < 0.0f || sample.peak > 1.0f)
            return E_UNEXPECTED;
        sample.coverage = coverage;
        sample.state = sample.peak > 0.0f
            ? AudioActivityState::Active : AudioActivityState::Quiet;
        sample.stage = "activity_sample";
        sample.lastPollTickMs = GetTickCount64();
        sample.lastPollUtc = UtcNow();
        if (volume) {
            BOOL muted = FALSE;
            sample.endpointMuteKnown = SUCCEEDED(volume->GetMute(&muted));
            sample.endpointMuted = sample.endpointMuteKnown && muted != FALSE;
            sample.endpointVolumeKnown = SUCCEEDED(
                volume->GetMasterVolumeLevelScalar(&sample.endpointVolume));
        }
        return S_OK;
    }
};
}  // namespace

struct AudioActivityMonitor::Impl {
    struct State {
        mutable std::mutex mutex;
        std::condition_variable changed;
        bool stop = false;
        bool finished = false;
        bool enabled = false;
        uint64_t revision = 0;
        std::wstring endpointId;
        AudioActivitySnapshot snapshot;

        void Invalidate() {
            snapshot.state = enabled ? AudioActivityState::Unknown : AudioActivityState::Disabled;
            snapshot.stage = enabled ? "activity_connecting" : "disabled";
            snapshot.error = S_OK;
            snapshot.coverage = AudioMeterCoverage::Unknown;
            snapshot.peak = 0.0f;
            snapshot.endpointMuteKnown = false;
            snapshot.endpointVolumeKnown = false;
            snapshot.attemptStartedTickMs = 0;
            snapshot.lastProgressTickMs = enabled ? GetTickCount64() : 0;
            ++revision;
        }

        bool Wait(uint64_t expected, std::chrono::milliseconds duration) {
            std::unique_lock<std::mutex> lock(mutex);
            return changed.wait_for(lock, duration,
                [&] { return stop || revision != expected; });
        }

        void Failed(uint64_t expected, HRESULT hr, const char* stage) {
            std::lock_guard<std::mutex> lock(mutex);
            if (revision != expected || stop) return;
            snapshot.state = AudioActivityState::Unavailable;
            snapshot.stage = stage;
            snapshot.error = hr;
            snapshot.peak = 0.0f;
            snapshot.coverage = AudioMeterCoverage::Unknown;
            snapshot.endpointMuteKnown = false;
            snapshot.endpointVolumeKnown = false;
            snapshot.lastProgressTickMs = GetTickCount64();
        }

        void Run() {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            struct ComScope {
                HRESULT result;
                ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
            } comScope{com};
            // All interfaces, including their final Release, remain on this thread.
            for (;;) {
                uint64_t current = 0;
                std::wstring endpoint;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock, [&] { return stop || (enabled && !endpointId.empty()); });
                    if (stop) break;
                    current = revision;
                    endpoint = endpointId;
                    snapshot.state = AudioActivityState::Unknown;
                    snapshot.stage = "activity_connecting";
                    snapshot.error = S_OK;
                    snapshot.attemptStartedTickMs = GetTickCount64();
                    snapshot.lastProgressTickMs = snapshot.attemptStartedTickMs;
                }
                if (FAILED(com)) {
                    Failed(current, com, "activity_com");
                    Wait(current, kReconnectInterval);
                    continue;
                }
                Meter meter;
                const char* stage = "activity_connecting";
                HRESULT hr = meter.Connect(endpoint, stage);
                if (FAILED(hr)) {
                    Failed(current, hr, stage);
                    Wait(current, kReconnectInterval);
                    continue;
                }
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (stop || revision != current) break;
                    }
                    AudioActivitySnapshot sample;
                    hr = meter.Read(sample);
                    if (FAILED(hr)) {
                        Failed(current, hr, sample.stage);
                        Wait(current, kReconnectInterval);
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (stop || revision != current) break;
                        sample.hasLastActivity = snapshot.hasLastActivity;
                        sample.lastActivityTickMs = snapshot.lastActivityTickMs;
                        sample.lastActivityUtc = snapshot.lastActivityUtc;
                        sample.attemptStartedTickMs = snapshot.attemptStartedTickMs;
                        sample.lastProgressTickMs = GetTickCount64();
                        if (sample.state == AudioActivityState::Active) {
                            sample.hasLastActivity = true;
                            sample.lastActivityTickMs = sample.lastPollTickMs;
                            sample.lastActivityUtc = sample.lastPollUtc;
                        }
                        snapshot = std::move(sample);
                    }
                    if (Wait(current, kPollInterval)) break;
                }
            }
        }

        void Entry() noexcept {
            try {
                Run();
            } catch (...) {
                // Keep an optional diagnostics failure from terminating playback.
                std::lock_guard<std::mutex> lock(mutex);
                snapshot.state = AudioActivityState::Unavailable;
                snapshot.stage = "activity_worker_exception";
                snapshot.error = E_FAIL;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                finished = true;
            }
            changed.notify_all();
        }
    };

    // The worker owns its state. A hung third-party COM call can therefore be
    // abandoned on exit without retaining a pointer to the destroyed monitor.
    std::shared_ptr<State> state = std::make_shared<State>();
    std::thread worker;

    Impl() : worker([owned = state] { owned->Entry(); }) {}

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->stop = true;
        }
        state->changed.notify_all();
        std::unique_lock<std::mutex> lock(state->mutex);
        const bool finished = state->changed.wait_for(lock, std::chrono::milliseconds(500),
                                                      [&] { return state->finished; });
        lock.unlock();
        if (finished) worker.join();
        else worker.detach();
    }
};

AudioActivityMonitor::AudioActivityMonitor() : impl_(std::make_unique<Impl>()) {}
AudioActivityMonitor::~AudioActivityMonitor() = default;

void AudioActivityMonitor::Configure(const std::wstring& endpointId, bool enabled) {
    auto& state = *impl_->state;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.endpointId == endpointId && state.enabled == enabled) return;
        if (state.endpointId != endpointId) state.snapshot = {};
        state.endpointId = endpointId;
        state.enabled = enabled;
        state.Invalidate();
        if (enabled && endpointId.empty()) state.snapshot.stage = "activity_no_endpoint";
    }
    state.changed.notify_all();
}

void AudioActivityMonitor::Resume() {
    auto& state = *impl_->state;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.Invalidate();
    }
    state.changed.notify_all();
}

AudioActivitySnapshot AudioActivityMonitor::Snapshot() const {
    const auto& state = *impl_->state;
    std::lock_guard<std::mutex> lock(state.mutex);
    auto snapshot = state.snapshot;
    const uint64_t now = GetTickCount64();
    if (state.enabled && !state.endpointId.empty() &&
        snapshot.state != AudioActivityState::Unavailable &&
        now - snapshot.lastProgressTickMs >= 5000) {
        snapshot.state = AudioActivityState::Unknown;
        snapshot.stage = "activity_stalled";
        snapshot.error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        snapshot.peak = 0.0f;
        snapshot.endpointMuteKnown = false;
        snapshot.endpointVolumeKnown = false;
    }
    return snapshot;
}
