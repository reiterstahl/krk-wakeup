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

PulseResult RenderPulse(const std::wstring& endpointId, unsigned durationMs,
                        unsigned frequencyHz, unsigned levelPercent);
