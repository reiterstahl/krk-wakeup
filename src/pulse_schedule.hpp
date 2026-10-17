#pragma once

#include <algorithm>
#include <chrono>

namespace PulseSchedule {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;

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
