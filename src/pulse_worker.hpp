#pragma once

#include "audio_activity.hpp"
#include "audio_process.hpp"
#include "diagnostics.hpp"
#include "pulse_schedule.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace KrkWakeup {
using Clock = std::chrono::steady_clock;

struct Config {
    std::wstring endpointId;
    unsigned idleSeconds = 900;
    unsigned intervalSeconds = 300;
    unsigned durationMs = 1000;
    unsigned frequencyHz = 440;
    unsigned levelPercent = 1;
    bool enabled = true;
    bool startup = false;
    bool diagnostics = false;
};

inline std::string ConfigSummary(const Config& config) {
    return "enabled=" + std::to_string(config.enabled) +
        " selected=" + std::to_string(!config.endpointId.empty()) +
        " interval_s=" + std::to_string(config.intervalSeconds) +
        " duration_ms=" + std::to_string(config.durationMs) +
        " frequency_hz=" + std::to_string(config.frequencyHz) +
        " level_pct=" + std::to_string(config.levelPercent) +
        " diagnostics=" + std::to_string(config.diagnostics);
}

inline std::string UtcNow() {
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    char text[32];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        static_cast<unsigned>(utc.wYear), static_cast<unsigned>(utc.wMonth),
        static_cast<unsigned>(utc.wDay), static_cast<unsigned>(utc.wHour),
        static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond),
        static_cast<unsigned>(utc.wMilliseconds));
    return text;
}

struct Status {
    std::wstring message;
    Clock::time_point next;
    bool hasNext = false;
    bool busy = false;
    Clock::time_point started;
    bool hasSuccess = false;
    uint64_t lastSuccessTickMs = 0;
    std::string lastSuccessUtc;
    bool suspended = false;
    int displayState = -1;
    unsigned effectiveIntervalSeconds = 300;
    bool protectionRequired = true;
};

class PulseWorker {
public:
    enum class TestKind { None, Pulse, Tone };
    PulseWorker(Config config, std::function<AudioActivitySnapshot()> readActivity)
        : readActivity_(std::move(readActivity)), config_(std::move(config)), next_(Clock::now()) {
        LogEvent("worker_start", ConfigSummary(config_));
        thread_ = std::thread([this] { Loop(); });
    }
    ~PulseWorker() { Stop(); }
    PulseWorker(const PulseWorker&) = delete;
    PulseWorker& operator=(const PulseWorker&) = delete;

    void Configure(const Config& config, bool manualTestFollows = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool activate = (!config_.enabled || config_.endpointId.empty() ||
                               config_.endpointId != config.endpointId) &&
                              config.enabled && !config.endpointId.empty();
        const bool intervalChanged = config_.intervalSeconds != config.intervalSeconds;
        const bool endpointChanged = config_.endpointId != config.endpointId;
        const bool renderChanged = endpointChanged || config_.enabled != config.enabled ||
            config_.durationMs != config.durationMs || config_.frequencyHz != config.frequencyHz ||
            config_.levelPercent != config.levelPercent;
        if (endpointChanged) {
            hasSuccess_ = false;
            lastSuccessTickMs_ = 0;
            lastSuccessUtc_.clear();
            hasOwnAttempt_ = false;
            lastOwnAttemptEndedTickMs_ = 0;
        }
        if (renderChanged) ++renderGeneration_;
        config_ = config;
        ++generation_;
        next_ = PulseSchedule::Reconfigure(next_, Clock::now(), Interval(), activate, manualTestFollows);
        if (activate) forcePulse_ = true;
        if (intervalChanged && !manualTestFollows) next_ = Clock::now();
        status_ = config.endpointId.empty() ? L"Selecciona una salida de audio."
                  : config.enabled ? L"Activo." : L"Pausado.";
        LogEvent("configured", ConfigSummary(config));
        condition_.notify_one();
    }
    void SetDisplayState(unsigned state) {
        if (state > 2) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (displayState_ == static_cast<int>(state)) return;
        displayState_ = static_cast<int>(state);
        ++generation_;
        next_ = Clock::now();
        LogEvent("screen_policy", "display_state=" + std::to_string(displayState_) +
            " normal_interval_s=" + std::to_string(config_.intervalSeconds) +
            " effective_interval_s=" + std::to_string(Interval()));
        condition_.notify_one();
    }
    void SetSessionLocked(bool locked) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessionLocked_ == locked) return;
        sessionLocked_ = locked;
        ++generation_;
        next_ = Clock::now();
        LogEvent("lock_policy", "locked=" + std::to_string(locked) +
            " effective_interval_s=" + std::to_string(Interval()));
        condition_.notify_one();
    }
    void Suspend() {
        std::lock_guard<std::mutex> lock(mutex_);
        suspended_ = true;
        ++generation_;
        ++renderGeneration_;
        condition_.notify_one();
    }
    void Resume() {
        std::lock_guard<std::mutex> lock(mutex_);
        suspended_ = false;
        ++generation_;
        ++renderGeneration_;
        if (config_.enabled && !config_.endpointId.empty()) {
            next_ = Clock::now();
            forcePulse_ = true;
            status_ = L"Recuperando audio tras reanudar…";
            LogEvent("recovery_requested", "reason=resume");
        }
        condition_.notify_one();
    }
    void Test(TestKind kind) {
        std::lock_guard<std::mutex> lock(mutex_);
        testPending_ = kind;
        if (busy_) ++renderGeneration_;
        condition_.notify_one();
    }
    Status Snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return {status_, next_, config_.enabled && !config_.endpointId.empty() && !busy_ && !suspended_,
                busy_, started_, hasSuccess_, lastSuccessTickMs_, lastSuccessUtc_, suspended_, displayState_, Interval(), ProtectionRequired()};
    }
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            ++renderGeneration_;
            condition_.notify_one();
        }
        if (thread_.joinable()) thread_.join();
    }
private:
    bool ProtectionRequired() const {
        return PulseSchedule::ProtectionRequired(displayState_, sessionLocked_, defaultOutputKnown_, selectedIsDefault_);
    }
    unsigned Interval() const { return PulseSchedule::EffectiveInterval(config_.intervalSeconds, ProtectionRequired()); }

    void RefreshRoute(const AudioActivitySnapshot& audio) {
        const bool known = PulseSchedule::FreshDefaultOutput(audio.defaultOutputKnown,
            GetTickCount64(), audio.defaultOutputTickMs);
        const bool selected = known && audio.selectedIsDefault;
        if (known == defaultOutputKnown_ && selected == selectedIsDefault_) return;
        defaultOutputKnown_ = known;
        selectedIsDefault_ = selected;
        next_ = Clock::now();
        LogEvent("output_policy", "default_output_known=" + std::to_string(known) +
            " selected_is_default=" + std::to_string(selected) +
            " protection=" + std::to_string(ProtectionRequired()) +
            " effective_interval_s=" + std::to_string(Interval()));
    }

    void Loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            if (suspended_) {
                condition_.wait(lock, [this] { return stopping_ || !suspended_; });
                continue;
            }
            if (config_.endpointId.empty()) {
                status_ = L"Selecciona una salida de audio.";
                condition_.wait(lock, [this] { return stopping_ || !config_.endpointId.empty(); });
                continue;
            }
            if (!config_.enabled && testPending_ == TestKind::None) {
                if (status_ == L"Iniciando…") status_ = L"Pausado.";
                condition_.wait(lock, [this] { return stopping_ || testPending_ != TestKind::None || config_.enabled; });
                continue;
            }
            // Independently recheck routing every second, even while hidden or
            // locked. No COM call or UI timer is needed on this worker.
            const auto audio = readActivity_();
            RefreshRoute(audio);
            if (testPending_ == TestKind::None && Clock::now() < next_) {
                const auto generation = generation_;
                condition_.wait_until(lock, std::min(next_, Clock::now() + std::chrono::seconds(1)), [this, generation] {
                    return stopping_ || testPending_ != TestKind::None || generation_ != generation;
                });
                continue;
            }
            if (stopping_) break;
            if (testPending_ == TestKind::None) {
                const auto now = Clock::now();
                const auto baseline = !hasSuccess_ || forcePulse_ || retryPending_ ? next_
                    : PulseSchedule::AfterSuccess(lastPulseStarted_, lastPulseCompleted_, Interval());
                const bool reliable = (audio.state == AudioActivityState::Active || audio.state == AudioActivityState::Quiet) &&
                    audio.coverage != AudioMeterCoverage::Unknown &&
                    audio.endpointMuteKnown && !audio.endpointMuted && audio.endpointVolumeKnown && audio.endpointVolume > 0.0f;
                const auto decision = PulseSchedule::AutomaticDue(now, baseline, Interval(), GetTickCount64(),
                    reliable, audio.hasQualifyingActivity, audio.lastQualifyingActivityTickMs, audio.lastPollTickMs,
                    hasOwnAttempt_, lastOwnAttemptEndedTickMs_, ProtectionRequired());
                if (decision.due > now) {
                    next_ = decision.due;
                    if (decision.deferredByAudio) {
                        forcePulse_ = false;
                        retryPending_ = false;
                        status_ = L"Hay audio; pulso aplazado.";
                        LogEvent("pulse_deferred", "reason=observed_audio effective_interval_s=" + std::to_string(Interval()) +
                            " next_in_ms=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(next_ - now).count()));
                    }
                    continue;
                }
            }
            const TestKind test = testPending_;
            testPending_ = TestKind::None;
            const Config current = config_;
            const auto renderGeneration = renderGeneration_;
            started_ = Clock::now();
            const auto started = started_;
            busy_ = true;
            const auto late = test == TestKind::None
                ? std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(started - next_).count())
                : 0;
            LogEvent("pulse_start", std::string("kind=") +
                (test == TestKind::Tone ? "tone" : test == TestKind::Pulse ? "test" : "automatic") +
                " late_ms=" + std::to_string(late) +
                " duration_ms=" + std::to_string(test == TestKind::Tone ? 3000 : current.durationMs) +
                " frequency_hz=" + std::to_string(test == TestKind::Tone ? 440 : current.frequencyHz) +
                " level_pct=" + std::to_string(test == TestKind::Tone ? 5 : current.levelPercent) +
                " interval_s=" + std::to_string(current.intervalSeconds) +
                " effective_interval_s=" + std::to_string(Interval()) + " display_state=" + std::to_string(displayState_) +
                " protection=" + std::to_string(ProtectionRequired()));
            status_ = test == TestKind::Tone ? L"Reproduciendo tono de prueba…"
                    : test == TestKind::Pulse ? L"Enviando pulso de prueba…"
                    : L"Enviando pulso…";
            lock.unlock();
            const auto cancelled = [this, renderGeneration] {
                std::lock_guard<std::mutex> guard(mutex_);
                return stopping_ || renderGeneration_ != renderGeneration;
            };
            const PulseResult result = test == TestKind::Tone
                ? RenderPulseIsolated(current.endpointId, 3000, 440, 5, cancelled)
                : RenderPulseIsolated(current.endpointId, current.durationMs,
                                      current.frequencyHz, current.levelPercent, cancelled);
            lock.lock();
            busy_ = false;
            const auto completed = Clock::now();
            if (current.endpointId == config_.endpointId) {
                hasOwnAttempt_ = true;
                lastOwnAttemptEndedTickMs_ = GetTickCount64();
            }
            char errorCode[24];
            std::snprintf(errorCode, sizeof(errorCode), "0x%08lX", static_cast<unsigned long>(result.error));
            LogEvent(result.success ? "pulse_ok" : "pulse_error",
                std::string("stage=") + result.stage + " hr=" + errorCode +
                " elapsed_ms=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(completed - started).count()) +
                " superseded=" + std::to_string(renderGeneration != renderGeneration_));
            if (stopping_) break;
            if (renderGeneration != renderGeneration_) continue;
            forcePulse_ = false;
            if (result.success) {
                retryPending_ = false;
                hasSuccess_ = true;
                lastPulseStarted_ = started;
                lastPulseCompleted_ = completed;
                lastSuccessTickMs_ = GetTickCount64();
                lastSuccessUtc_ = UtcNow();
                status_ = test == TestKind::Tone ? L"Tono de prueba enviado."
                        : test == TestKind::Pulse ? L"Pulso de prueba enviado."
                        : L"Pulso enviado.";
                next_ = PulseSchedule::AfterSuccess(started, completed, Interval());
            } else {
                retryPending_ = true;
                if (result.blockedByVolume) {
                    status_ = L"Audio silenciado en Windows. Reintentando…";
                } else if (result.unavailable) {
                    status_ = L"Salida no disponible. Reintentando…";
                } else {
                    wchar_t errorText[96];
                    swprintf_s(errorText, L"Error de audio 0x%08lX. Reintentando…",
                               static_cast<unsigned long>(result.error));
                    status_ = errorText;
                }
                next_ = completed + std::chrono::seconds(5);
            }
        }
        LogEvent("worker_stop");
    }
    std::function<AudioActivitySnapshot()> readActivity_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
    Config config_;
    Clock::time_point next_;
    Clock::time_point started_;
    Clock::time_point lastPulseStarted_;
    Clock::time_point lastPulseCompleted_;
    uint64_t lastSuccessTickMs_ = 0;
    std::string lastSuccessUtc_;
    std::wstring status_ = L"Iniciando…";
    uint64_t generation_ = 0;
    uint64_t renderGeneration_ = 0;
    bool stopping_ = false;
    bool suspended_ = false;
    bool busy_ = false;
    bool hasSuccess_ = false;
    bool hasOwnAttempt_ = false;
    uint64_t lastOwnAttemptEndedTickMs_ = 0;
    bool forcePulse_ = true;
    bool retryPending_ = false;
    int displayState_ = -1;
    bool sessionLocked_ = false;
    bool defaultOutputKnown_ = false;
    bool selectedIsDefault_ = false;
    TestKind testPending_ = TestKind::None;
};

} // namespace KrkWakeup
