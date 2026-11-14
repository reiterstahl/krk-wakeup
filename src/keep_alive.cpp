#include "keep_alive.hpp"

#include "diagnostics.hpp"
#include "signal.hpp"
#include "wasapi_util.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
constexpr auto kWriteInterval = std::chrono::milliseconds(20);
constexpr auto kReconnectInterval = std::chrono::seconds(2);
// Room for the queue plus the whole closing fade, in 100 ns units.
constexpr REFERENCE_TIME kBufferDuration = 600 * 10000;
// Audio kept queued in Windows. Enough to ride out a busy scheduler, short
// enough that a level change is heard within a fraction of a second.
constexpr unsigned kQueuedMs = 120;
constexpr uint64_t kGainPollMs = 50;
constexpr uint64_t kHealthPollMs = 1000;
constexpr uint64_t kClockStallMs = 5000;
constexpr uint64_t kStalledAfterMs = 5000;
constexpr uint64_t kReplaceThreadAfterMs = 20000;

std::string Hex(HRESULT hr) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

KeepAliveState FailureState(HRESULT hr, const char* stage) {
    // Whatever the reason, an output that cannot be opened by its saved
    // identifier is not there: unplugged, disabled, or never existed.
    if (std::strcmp(stage, "keepalive_device") == 0 ||
        std::strcmp(stage, "keepalive_device_state") == 0) return KeepAliveState::Unavailable;
    if (hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
        return KeepAliveState::InUse;
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == E_NOTFOUND ||
        hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
        hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED))
        return KeepAliveState::Unavailable;
    return KeepAliveState::Error;
}
}  // namespace

const char* KeepAliveStateName(KeepAliveState state) {
    switch (state) {
    case KeepAliveState::Off: return "off";
    case KeepAliveState::Connecting: return "connecting";
    case KeepAliveState::Running: return "running";
    case KeepAliveState::Muted: return "muted";
    case KeepAliveState::Unavailable: return "unavailable";
    case KeepAliveState::InUse: return "in_use";
    case KeepAliveState::Error: return "error";
    case KeepAliveState::Stalled: return "stalled";
    }
    return "unknown";
}

struct KeepAliveStream::Impl {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        bool stop = false;
        bool finished = false;
        bool suspended = false;
        uint64_t revision = 0;
        KeepAliveConfig config;
        KeepAliveSnapshot snapshot;
        std::string lastLogged;

        bool Wanted() const {
            return config.enabled && !config.endpointId.empty() && !suspended;
        }

        // True when the stream must close: shutdown or a new configuration.
        bool Wait(uint64_t expected, std::chrono::milliseconds duration) {
            std::unique_lock<std::mutex> lock(mutex);
            return changed.wait_for(lock, duration,
                [&] { return stop || revision != expected; });
        }

        bool Current(uint64_t expected) {
            std::lock_guard<std::mutex> lock(mutex);
            return !stop && revision == expected;
        }

        void LogOnce(const std::string& line) {
            // A missing output is retried every two seconds; report each
            // distinct condition once instead of once per attempt.
            bool fresh = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                fresh = lastLogged != line;
                if (fresh) lastLogged = line;
            }
            if (fresh) LogEvent("keepalive_state", line);
        }

        void Failed(uint64_t expected, HRESULT hr, const char* stage) {
            const KeepAliveState failure = FailureState(hr, stage);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (revision != expected || stop) return;
                snapshot.state = failure;
                snapshot.stage = stage;
                snapshot.error = hr;
                snapshot.runningSinceTickMs = 0;
                snapshot.digitalLevel = 0.0;
                snapshot.limited = false;
                snapshot.playedKnown = false;
                ++snapshot.failures;
                snapshot.lastProgressTickMs = GetTickCount64();
            }
            LogOnce(std::string("state=") + KeepAliveStateName(failure) + " stage=" + stage +
                    " hr=" + Hex(hr));
        }

        HRESULT Play(const KeepAliveConfig& active, uint64_t current, const char*& stage) {
            ComPtr<IMMDeviceEnumerator> enumerator;
            stage = "keepalive_enumerator";
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(&enumerator));
            if (FAILED(hr)) return hr;
            ComPtr<IMMDevice> device;
            stage = "keepalive_device";
            hr = enumerator->GetDevice(active.endpointId.c_str(), &device);
            if (FAILED(hr)) return hr;
            DWORD deviceState = 0;
            stage = "keepalive_device_state";
            hr = device->GetState(&deviceState);
            if (FAILED(hr)) return hr;
            if (deviceState != DEVICE_STATE_ACTIVE)
                return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
            // Optional: an output without a volume control is still playable.
            ComPtr<IAudioEndpointVolume> endpointVolume;
            device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(endpointVolume.GetAddressOf()));
            ComPtr<IAudioClient> client;
            stage = "keepalive_activate";
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(client.GetAddressOf()));
            if (FAILED(hr)) return hr;
            WAVEFORMATEX* rawFormat = nullptr;
            stage = "keepalive_mix_format";
            hr = client->GetMixFormat(&rawFormat);
            if (FAILED(hr)) return hr;
            std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> wave(rawFormat, CoTaskMemFree);
            Signal::Format format;
            stage = "keepalive_decode_format";
            if (!WasapiUtil::DecodeFormat(wave.get(), format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
            stage = "keepalive_initialize";
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0,
                                    wave.get(), nullptr);
            if (FAILED(hr)) return hr;
            WasapiUtil::OptOutOfDucking(client.Get());
            ComPtr<ISimpleAudioVolume> sessionVolume;
            client->GetService(IID_PPV_ARGS(&sessionVolume));
            ComPtr<IAudioClock> clock;
            client->GetService(IID_PPV_ARGS(&clock));
            UINT64 clockFrequency = 0;
            if (clock && (FAILED(clock->GetFrequency(&clockFrequency)) || !clockFrequency))
                clock.Reset();
            ComPtr<IAudioRenderClient> render;
            stage = "keepalive_render_service";
            hr = client->GetService(IID_PPV_ARGS(&render));
            if (FAILED(hr)) return hr;
            UINT32 bufferFrames = 0;
            stage = "keepalive_buffer_size";
            hr = client->GetBufferSize(&bufferFrames);
            if (FAILED(hr)) return hr;
            if (!bufferFrames) return E_UNEXPECTED;

            const UINT32 queueFrames = static_cast<UINT32>(std::min<uint64_t>(
                bufferFrames, std::max<uint64_t>(1, Signal::FramesFor(kQueuedMs, format.sampleRate))));
            const double requested = active.levelPercent / 100.0;
            Signal::ContinuousTone tone;
            tone.Configure(format.sampleRate, active.frequencyHz, Signal::kContinuousLeadInMs);
            WasapiUtil::PathGain path =
                WasapiUtil::ReadPathGain(endpointVolume.Get(), sessionVolume.Get());
            tone.SetTarget(Signal::CompensatedLevel(requested, active.compensateVolume,
                                                    path.known, path.gain));
            uint64_t framesWritten = 0;
            unsigned underruns = 0;
            bool started = false;

            // Tops the queue up to its target. `minimum` forces at least that
            // many frames when there is room, used for the closing fade.
            const auto write = [&](UINT32 minimum) -> HRESULT {
                UINT32 padding = 0;
                stage = "keepalive_padding";
                HRESULT result = client->GetCurrentPadding(&padding);
                if (FAILED(result)) return result;
                if (started && padding == 0) ++underruns;
                const UINT32 room = bufferFrames > padding ? bufferFrames - padding : 0;
                UINT32 count = padding < queueFrames ? queueFrames - padding : 0;
                count = std::min(room, std::max(count, minimum));
                if (!count) return S_OK;
                BYTE* buffer = nullptr;
                stage = "keepalive_buffer";
                result = render->GetBuffer(count, &buffer);
                if (FAILED(result)) return result;
                for (UINT32 frame = 0; frame < count; ++frame) {
                    Signal::WriteFrame(buffer + static_cast<size_t>(frame) * format.bytesPerFrame,
                                       format, tone.Next());
                }
                stage = "keepalive_release";
                result = render->ReleaseBuffer(count, 0);
                if (FAILED(result)) return result;
                framesWritten += count;
                return S_OK;
            };

            hr = write(0);
            if (FAILED(hr)) return hr;
            stage = "keepalive_start";
            hr = client->Start();
            if (FAILED(hr)) return hr;
            started = true;
            const uint64_t startedTick = GetTickCount64();
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!stop && revision == current) ++snapshot.connections;
            }
            LogOnce("state=running frequency_hz=" + std::to_string(active.frequencyHz) +
                    " level_pct=" + std::to_string(active.levelPercent) +
                    " compensate=" + std::to_string(active.compensateVolume) +
                    " sample_rate=" + std::to_string(format.sampleRate) +
                    " channels=" + std::to_string(format.channels) +
                    " bits=" + std::to_string(format.bits) +
                    " float=" + std::to_string(format.floating) +
                    " buffer_frames=" + std::to_string(bufferFrames));

            uint64_t gainTick = startedTick;
            uint64_t healthTick = startedTick;
            uint64_t clockMovedTick = startedTick;
            UINT64 clockPosition = 0;
            bool playedKnown = false;
            HRESULT failure = S_OK;
            for (;;) {
                if (Wait(current, kWriteInterval)) break;
                const uint64_t now = GetTickCount64();
                if (now - gainTick >= kGainPollMs) {
                    gainTick = now;
                    path = WasapiUtil::ReadPathGain(endpointVolume.Get(), sessionVolume.Get());
                    tone.SetTarget(Signal::CompensatedLevel(requested, active.compensateVolume,
                                                            path.known, path.gain));
                }
                failure = write(0);
                if (FAILED(failure)) break;
                if (now - healthTick >= kHealthPollMs) {
                    healthTick = now;
                    stage = "keepalive_device_state";
                    failure = device->GetState(&deviceState);
                    if (SUCCEEDED(failure) && deviceState != DEVICE_STATE_ACTIVE)
                        failure = HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
                    if (FAILED(failure)) break;
                    UINT64 position = 0;
                    if (clock && SUCCEEDED(clock->GetPosition(&position, nullptr))) {
                        playedKnown = true;
                        if (position != clockPosition) {
                            clockPosition = position;
                            clockMovedTick = now;
                        } else if (now - clockMovedTick >= kClockStallMs) {
                            // Buffers are accepted but nothing is being played.
                            stage = "keepalive_clock_stalled";
                            failure = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                            break;
                        }
                    }
                }
                std::lock_guard<std::mutex> lock(mutex);
                if (stop || revision != current) continue;
                snapshot.state = path.muted ? KeepAliveState::Muted : KeepAliveState::Running;
                snapshot.stage = "keepalive_playing";
                snapshot.error = S_OK;
                snapshot.runningSinceTickMs = startedTick;
                snapshot.lastProgressTickMs = GetTickCount64();
                snapshot.framesWritten = framesWritten;
                snapshot.playedKnown = playedKnown;
                snapshot.playedMs = playedKnown ? clockPosition * 1000 / clockFrequency : 0;
                snapshot.sampleRate = format.sampleRate;
                snapshot.underruns = underruns;
                snapshot.frequencyHz = active.frequencyHz;
                snapshot.levelPercent = active.levelPercent;
                snapshot.pathGainKnown = path.known;
                snapshot.pathGain = path.gain;
                snapshot.digitalLevel = tone.Level();
                snapshot.limited = Signal::CompensationLimited(requested, active.compensateVolume,
                                                               path.known, path.gain);
            }
            if (SUCCEEDED(failure)) {
                // Closing on purpose: fade to silence slowly enough that the
                // low tone is not cut, then let Windows play what is queued.
                tone.Release();
                const UINT32 fade = static_cast<UINT32>(
                    Signal::FramesFor(Signal::kContinuousReleaseMs, format.sampleRate));
                if (SUCCEEDED(write(fade))) {
                    UINT32 padding = 0;
                    if (SUCCEEDED(client->GetCurrentPadding(&padding))) {
                        const DWORD queuedMs = static_cast<DWORD>(
                            static_cast<uint64_t>(padding) * 1000 / format.sampleRate);
                        Sleep(std::min<DWORD>(queuedMs + 20, 600));
                    }
                }
            }
            client->Stop();
            return failure;
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
                KeepAliveConfig active;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    if (!stop && !Wanted()) {
                        snapshot.state = KeepAliveState::Off;
                        snapshot.stage = "off";
                        snapshot.error = S_OK;
                        snapshot.runningSinceTickMs = 0;
                        snapshot.digitalLevel = 0.0;
                        snapshot.limited = false;
                        snapshot.playedKnown = false;
                        if (lastLogged != "state=off") {
                            lastLogged = "state=off";
                            lock.unlock();
                            LogEvent("keepalive_state", "state=off");
                            lock.lock();
                        }
                    }
                    changed.wait(lock, [&] { return stop || Wanted(); });
                    if (stop) break;
                    current = revision;
                    active = config;
                    snapshot.state = KeepAliveState::Connecting;
                    snapshot.stage = "keepalive_connecting";
                    snapshot.error = S_OK;
                    snapshot.attemptStartedTickMs = GetTickCount64();
                    snapshot.lastProgressTickMs = snapshot.attemptStartedTickMs;
                }
                const char* stage = "keepalive_com";
                const HRESULT hr = FAILED(com) ? com : Play(active, current, stage);
                if (FAILED(hr)) {
                    Failed(current, hr, stage);
                    Wait(current, kReconnectInterval);
                }
            }
        }

        void Entry() noexcept {
            try {
                Run();
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                snapshot.state = KeepAliveState::Error;
                snapshot.stage = "keepalive_exception";
                snapshot.error = E_FAIL;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                finished = true;
            }
            changed.notify_all();
        }
    };

    // The playback thread owns its state. A thread stuck inside a driver call
    // can therefore be abandoned, at exit or when it is replaced, without it
    // ever touching freed memory.
    std::mutex mutex;
    std::shared_ptr<State> state;
    std::thread worker;
    unsigned replacements = 0;

    void Launch(const KeepAliveConfig& config, bool suspended) {
        state = std::make_shared<State>();
        state->config = config;
        state->suspended = suspended;
        worker = std::thread([owned = state] { owned->Entry(); });
    }

    void Retire(std::chrono::milliseconds patience) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->stop = true;
        }
        state->changed.notify_all();
        std::unique_lock<std::mutex> lock(state->mutex);
        const bool finished = state->changed.wait_for(lock, patience,
                                                      [&] { return state->finished; });
        lock.unlock();
        if (finished) worker.join();
        else worker.detach();
    }

    Impl() { Launch({}, false); }
    ~Impl() { Retire(std::chrono::milliseconds(1500)); }
};

KeepAliveStream::KeepAliveStream() : impl_(std::make_unique<Impl>()) {}
KeepAliveStream::~KeepAliveStream() = default;

void KeepAliveStream::Configure(const KeepAliveConfig& config) {
    std::lock_guard<std::mutex> owner(impl_->mutex);
    auto& state = *impl_->state;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        const KeepAliveConfig& old = state.config;
        if (old.endpointId == config.endpointId && old.enabled == config.enabled &&
            old.frequencyHz == config.frequencyHz && old.levelPercent == config.levelPercent &&
            old.compensateVolume == config.compensateVolume) return;
        state.config = config;
        ++state.revision;
    }
    state.changed.notify_all();
}

void KeepAliveStream::Suspend() {
    std::lock_guard<std::mutex> owner(impl_->mutex);
    auto& state = *impl_->state;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.suspended = true;
        ++state.revision;
    }
    state.changed.notify_all();
}

void KeepAliveStream::Resume() {
    std::lock_guard<std::mutex> owner(impl_->mutex);
    auto& state = *impl_->state;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.suspended = false;
        // Reopen even if the stream survived: the device may have changed.
        ++state.revision;
    }
    state.changed.notify_all();
}

KeepAliveSnapshot KeepAliveStream::Snapshot() {
    std::lock_guard<std::mutex> owner(impl_->mutex);
    KeepAliveSnapshot snapshot;
    KeepAliveConfig config;
    bool suspended = false;
    {
        std::lock_guard<std::mutex> lock(impl_->state->mutex);
        snapshot = impl_->state->snapshot;
        config = impl_->state->config;
        suspended = impl_->state->suspended;
    }
    const bool busy = snapshot.state == KeepAliveState::Connecting ||
        snapshot.state == KeepAliveState::Running || snapshot.state == KeepAliveState::Muted;
    const uint64_t now = GetTickCount64();
    const uint64_t silent = now >= snapshot.lastProgressTickMs ? now - snapshot.lastProgressTickMs : 0;
    if (busy && silent >= kStalledAfterMs) {
        snapshot.state = KeepAliveState::Stalled;
        snapshot.stage = "keepalive_stalled";
        snapshot.error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        snapshot.digitalLevel = 0.0;
        if (silent >= kReplaceThreadAfterMs) {
            // The call may never return. Leave that thread behind and start
            // over; pulses keep going through their own isolated process.
            LogEvent("keepalive_state", "state=stalled action=replace_thread silent_ms=" +
                     std::to_string(silent));
            impl_->Retire(std::chrono::milliseconds(0));
            ++impl_->replacements;
            impl_->Launch(config, suspended);
        }
    }
    snapshot.failures += impl_->replacements;
    return snapshot;
}
