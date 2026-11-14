#pragma once

#include <windows.h>
#include <string>

struct PulseResult {
    bool success = false;
    bool unavailable = false;
    HRESULT error = S_OK;
    std::string stage = "none";
    bool blockedByVolume = false;
};

// Raise the digital level to offset the Windows volume of the output and of
// this application, within Signal::kMaxBoost.
constexpr unsigned kPulseCompensateVolume = 1;
constexpr unsigned kPulseKnownFlags = kPulseCompensateVolume;

// Plays one pulse on the saved output and waits until Windows has consumed it.
// When nothing else is playing there the pulse is preceded by an inaudible
// lead-in, so that a DAC which was idle has unmuted before the tone starts;
// the result stage is then "complete_cold" instead of "complete".
PulseResult RenderPulse(const std::wstring& endpointId, unsigned durationMs,
                        unsigned frequencyHz, unsigned levelPercent, unsigned flags);
