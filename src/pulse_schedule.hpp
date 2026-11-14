#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>

namespace PulseSchedule {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;

constexpr unsigned kProtectionIntervalSeconds = 300;

inline bool FreshDefaultOutput(bool known, uint64_t nowTick, uint64_t readTick) {
    return known && readTick <= nowTick && nowTick - readTick <= 2000;
}

inline bool ProtectionRequired(int displayState, bool sessionLocked,
                               bool defaultOutputKnown, bool selectedIsDefault) {
    return displayState != 1 || sessionLocked || !defaultOutputKnown || !selectedIsDefault;
}

inline unsigned EffectiveInterval(unsigned normalSeconds, bool protectionRequired) {
    return protectionRequired ? std::min(normalSeconds, kProtectionIntervalSeconds) : normalSeconds;
}

inline bool QualifyingPeak(float peak, unsigned levelPercent) {
    return std::isfinite(peak) && peak >= static_cast<float>(levelPercent) / 100.0f;
}

inline Time AfterSuccess(Time started, Time completed, unsigned intervalSeconds) {
    // Measure between starts; never send a burst to catch up missed intervals.
    const auto interval = std::chrono::seconds(intervalSeconds);
    const auto due = started + interval;
    return due > completed ? due : completed + interval;
}

inline Time Reconfigure(Time pending, Time now, unsigned intervalSeconds,
                        bool activate, bool manualTestFollows) {
    if (activate && !manualTestFollows) return now;
    if (manualTestFollows) return now + std::chrono::seconds(intervalSeconds);
    // Saving unrelated settings must not postpone an already scheduled pulse.
    return std::min(pending, now + std::chrono::seconds(intervalSeconds));
}
}  // namespace PulseSchedule
