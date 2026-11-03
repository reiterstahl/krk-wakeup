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

struct Decision {
    Time due;
    bool deferredByAudio = false;
};

inline Decision AutomaticDue(Time now, Time baseline, unsigned intervalSeconds,
                              uint64_t nowTick, bool reliableAudio, bool hasActivity,
                              uint64_t activityTick, uint64_t pollTick,
                              bool hasOwnAttempt, uint64_t ownAttemptCompletedTick, bool protectionRequired = false) {
    // Protection mode sends on schedule even if a meter reports activity.
    // Unknown/stale metering must never suppress an automatic pulse. Ignore
    // the tail of our own pulse so it cannot extend its own regular interval.
    if (protectionRequired || !reliableAudio || !hasActivity || pollTick > nowTick || nowTick - pollTick > 2000 ||
        activityTick > nowTick ||
        (hasOwnAttempt && (activityTick <= ownAttemptCompletedTick || activityTick - ownAttemptCompletedTick <= 500)))
        return {baseline, false};
    const uint64_t age = nowTick - activityTick;
    const uint64_t intervalMs = static_cast<uint64_t>(intervalSeconds) * 1000;
    if (age >= intervalMs) return {baseline, false};
    const auto audioDue = now + std::chrono::milliseconds(intervalMs - age);
    return {std::max(baseline, audioDue), audioDue > baseline};
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
