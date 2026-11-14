#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

// A stream that stays open on the saved output and plays a continuous tone
// low enough in frequency and level to go unheard on small monitors.
//
// It exists because of how the hardware behaves. With no stream open, Windows
// stops feeding a USB DAC and the DAC mutes its output; the next short pulse
// then lands on a device that is still waking up. While this stream runs the
// DAC never idles, and the speakers receive signal all the time rather than
// for one second every few minutes.
//
// Windows treats an open audio stream as a reason to keep the computer awake:
// automatic sleep does not start while this is running. Turning the display
// off is not affected.

struct KeepAliveConfig {
    std::wstring endpointId;
    bool enabled = false;
    unsigned frequencyHz = 20;
    // 0 keeps the DAC running with inaudible dither only.
    unsigned levelPercent = 2;
    bool compensateVolume = true;
};

enum class KeepAliveState {
    Off,          // disabled, paused, no output selected, or Windows suspended
    Connecting,
    Running,
    Muted,        // running, but the output or this application is muted in Windows
    Unavailable,  // the saved output is not connected
    InUse,        // another application holds the output in exclusive mode
    Error,
    Stalled,      // an audio call has not returned
};

struct KeepAliveSnapshot {
    KeepAliveState state = KeepAliveState::Off;
    HRESULT error = S_OK;
    const char* stage = "off";
    uint64_t attemptStartedTickMs = 0;
    uint64_t lastProgressTickMs = 0;
    uint64_t runningSinceTickMs = 0;
    uint64_t framesWritten = 0;
    // Position reported by the device clock: audio Windows actually consumed.
    uint64_t playedMs = 0;
    bool playedKnown = false;
    unsigned sampleRate = 0;
    unsigned underruns = 0;
    unsigned connections = 0;
    unsigned failures = 0;
    unsigned frequencyHz = 0;
    unsigned levelPercent = 0;
    bool pathGainKnown = false;
    double pathGain = 1.0;
    // Level written to Windows, 0..1, after volume compensation.
    double digitalLevel = 0.0;
    // The compensation hit its cap: the signal leaving Windows is weaker than
    // requested because the Windows volume is very low.
    bool limited = false;
};

const char* KeepAliveStateName(KeepAliveState state);

class KeepAliveStream {
public:
    KeepAliveStream();
    ~KeepAliveStream();
    KeepAliveStream(const KeepAliveStream&) = delete;
    KeepAliveStream& operator=(const KeepAliveStream&) = delete;

    // Any change closes the stream with a short fade and opens it again.
    void Configure(const KeepAliveConfig& config);
    void Suspend();
    void Resume();
    // Also replaces a playback thread that has been stuck inside an audio
    // call for a long time, so call it periodically.
    KeepAliveSnapshot Snapshot();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
