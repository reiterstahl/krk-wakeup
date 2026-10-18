#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

enum class AudioActivityState {
    Disabled,
    Unknown,
    Quiet,
    Active,
    Unavailable,
};

enum class AudioMeterCoverage {
    Unknown,
    SharedModeOnly,
    SharedAndExclusive,
};

struct AudioActivitySnapshot {
    AudioActivityState state = AudioActivityState::Disabled;
    AudioMeterCoverage coverage = AudioMeterCoverage::Unknown;
    HRESULT error = S_OK;
    const char* stage = "disabled";
    float peak = 0.0f;
    bool hasLastActivity = false;
    // GetTickCount64 includes time spent asleep. UTC is for human-readable logs.
    uint64_t lastActivityTickMs = 0;
    std::string lastActivityUtc;
    // Progress lets callers identify a blocked activation/poll independently of
    // the last audio observation. Zero means no connection attempt yet.
    uint64_t attemptStartedTickMs = 0;
    uint64_t lastProgressTickMs = 0;
    uint64_t lastPollTickMs = 0;
    std::string lastPollUtc;
    bool endpointMuteKnown = false;
    bool endpointMuted = false;
    bool endpointVolumeKnown = false;
    float endpointVolume = 0.0f;
};

// Reads the selected render endpoint's peak meter; opens no audio stream and
// records no audio content. "Quiet" means this sample had no observed peak,
// not proof of continuous silence. A 100 ms poll can miss very short sounds.
// Peaks precede endpoint volume/mute; software meters miss exclusive streams.
class AudioActivityMonitor {
public:
    AudioActivityMonitor();
    ~AudioActivityMonitor();
    AudioActivityMonitor(const AudioActivityMonitor&) = delete;
    AudioActivityMonitor& operator=(const AudioActivityMonitor&) = delete;

    // Switching endpoint clears its history. Disabling stops endpoint queries;
    // history for the same endpoint remains available until the process exits.
    void Configure(const std::wstring& endpointId, bool enabled);
    // Recreate COM endpoint interfaces after resume; retain the last observation.
    void Resume();
    // Samples older than five seconds become Unknown rather than remaining
    // Quiet/Active when a device call stalls or the machine is suspended.
    AudioActivitySnapshot Snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
