#pragma once

#include <cstdint>

// ReadPadding returns false on a device/API failure. Queued frames must reach
// zero before a pulse is considered consumed by Windows' audio engine.
enum class DrainResult { Complete, TimedOut, AudioError };

template<class ReadPadding, class Now, class Wait>
DrainResult DrainPlayback(ReadPadding readPadding, Now now, Wait wait,
                          uint64_t timeoutMs) {
    const uint64_t deadline = now() + timeoutMs;
    for (;;) {
        uint32_t padding = 0;
        if (!readPadding(padding)) return DrainResult::AudioError;
        if (padding == 0) return DrainResult::Complete;
        if (now() >= deadline) return DrainResult::TimedOut;
        wait();
    }
}
