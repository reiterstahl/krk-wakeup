#pragma once

#include <windows.h>
#include <string>

struct PulseResult {
    bool success = false;
    bool unavailable = false;
    HRESULT error = S_OK;
};

PulseResult RenderPulse(const std::wstring& endpointId, unsigned durationMs,
                        unsigned frequencyHz, unsigned levelPercent);
