#pragma once

#include "audio.hpp"

#include <functional>

// Run the renderer in a hidden copy of this executable so a stalled audio
// driver cannot keep the scheduler blocked. The cancellation callback is polled
// at least every 100 ms while the helper is running.
PulseResult RenderPulseIsolated(const std::wstring& endpointId, unsigned durationMs,
                                unsigned frequencyHz, unsigned levelPercent,
                                const std::function<bool()>& cancelled);

// Call before creating the single-instance mutex, logger, COM apartment or UI.
// commandLine is wWinMain's command line, without the executable name.
// Returns true for helper invocations, with their process exit code in exitCode.
bool RunAudioHelperIfRequested(PWSTR commandLine, int& exitCode);
