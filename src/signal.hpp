#pragma once

// Portable signal maths shared by the one-shot pulse renderer and the
// continuous keep-alive stream. Nothing here touches Windows, so the
// behaviour is covered by tests that also build on other platforms.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace Signal {
constexpr double kTau = 6.2831853071795864769;

// A DAC that has no stream open stops receiving data and mutes its output.
// When a stream starts it needs time to lock and unmute, and it swallows the
// audio sent meanwhile. A pulse sent to an idle output therefore waits behind
// this much inaudible lead-in.
constexpr unsigned kColdLeadInMs = 1500;
// Inaudible tail so the fade-out is fully played before the stream stops.
constexpr unsigned kTailMs = 250;
constexpr unsigned kFadeMs = 30;
// The continuous tone waits this long, sending only the inaudible dither,
// before it fades in. A DAC that unmutes halfway through a cycle of a low tone
// turns that step into a thump; by the time the tone starts the output is
// already open.
constexpr unsigned kContinuousLeadInMs = 2000;
// Closing fade of the continuous tone. A 20 Hz cycle lasts 50 ms, so anything
// much shorter than this cuts the wave and is heard as a thump.
constexpr unsigned kContinuousReleaseMs = 300;

// Windows applies the endpoint (and session) volume after our samples. The
// compensation raises the digital level by at most this factor, so a sudden
// volume increase can never expose more than ten times the requested level
// during the few milliseconds it takes to react.
constexpr double kMaxBoost = 10.0;
// Headroom for other applications mixed into the same shared output.
constexpr double kMaxDigitalLevel = 0.5;
// Roughly one 16-bit step: keeps a DAC from treating the stream as digital
// silence without being audible.
constexpr double kDitherLevel = 1.0 / 32768.0;

struct Format {
    bool floating = false;
    unsigned bits = 0;
    unsigned channels = 0;
    unsigned bytesPerFrame = 0;
    unsigned sampleRate = 0;
};

inline void WriteSample(uint8_t* target, const Format& format, double value) {
    value = std::clamp(value, -1.0, 1.0);
    if (format.floating) {
        const float sample = static_cast<float>(value);
        std::memcpy(target, &sample, sizeof(sample));
    } else if (format.bits == 8) {
        target[0] = static_cast<uint8_t>(std::lround(128.0 + value * 127.0));
    } else if (format.bits == 16) {
        const int16_t sample = static_cast<int16_t>(std::lround(value * 32767.0));
        std::memcpy(target, &sample, sizeof(sample));
    } else if (format.bits == 24) {
        const int32_t sample = static_cast<int32_t>(std::lround(value * 8388607.0));
        target[0] = static_cast<uint8_t>(sample);
        target[1] = static_cast<uint8_t>(sample >> 8);
        target[2] = static_cast<uint8_t>(sample >> 16);
    } else {
        const int32_t sample = static_cast<int32_t>(std::llround(value * 2147483647.0));
        std::memcpy(target, &sample, sizeof(sample));
    }
}

inline void WriteFrame(uint8_t* frame, const Format& format, double value) {
    const unsigned sampleBytes = format.bits / 8;
    for (unsigned channel = 0; channel < format.channels; ++channel)
        WriteSample(frame + channel * sampleBytes, format, value);
}

inline uint64_t FramesFor(unsigned milliseconds, unsigned sampleRate) {
    return static_cast<uint64_t>(milliseconds) * sampleRate / 1000;
}

inline double Dither(uint64_t position) {
    return (position & 1) ? kDitherLevel : -kDitherLevel;
}

inline double DecibelsToLinear(double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

// Digital level to write so that `targetLevel` (0..1) is what leaves Windows
// after it applies `pathGain` (0..1, endpoint volume times session volume).
// An unknown or zero path gain is not compensated.
inline double CompensatedLevel(double targetLevel, bool compensate, bool pathGainKnown,
                               double pathGain) {
    targetLevel = std::clamp(targetLevel, 0.0, kMaxDigitalLevel);
    if (!compensate || !pathGainKnown || !std::isfinite(pathGain) || pathGain <= 0.0)
        return targetLevel;
    pathGain = std::min(pathGain, 1.0);
    return std::min({targetLevel / pathGain, targetLevel * kMaxBoost, kMaxDigitalLevel});
}

// True when the cap, not the volume, decided the level: the signal leaving
// Windows is then weaker than requested.
inline bool CompensationLimited(double targetLevel, bool compensate, bool pathGainKnown,
                                double pathGain) {
    if (!compensate || !pathGainKnown || !std::isfinite(pathGain) || pathGain <= 0.0)
        return false;
    targetLevel = std::clamp(targetLevel, 0.0, kMaxDigitalLevel);
    return targetLevel / std::min(pathGain, 1.0) >
        std::min(targetLevel * kMaxBoost, kMaxDigitalLevel) * 1.0001;
}

// One pulse: optional lead-in, a tone with raised-cosine edges, and a tail.
// Lead-in and tail carry only the inaudible dither.
struct PulseShape {
    uint64_t leadFrames = 0;
    uint64_t toneFrames = 0;
    uint64_t tailFrames = 0;
    uint64_t fadeFrames = 1;
    unsigned sampleRate = 0;
    unsigned frequencyHz = 0;

    uint64_t Total() const { return leadFrames + toneFrames + tailFrames; }

    // Envelope of the tone at an absolute frame position, 0..1.
    double Envelope(uint64_t position) const {
        if (position < leadFrames || position >= leadFrames + toneFrames) return 0.0;
        const uint64_t inTone = position - leadFrames;
        const uint64_t fromEnd = toneFrames - inTone - 1;
        const uint64_t edge = std::min(inTone, fromEnd);
        if (edge >= fadeFrames) return 1.0;
        return 0.5 - 0.5 * std::cos(kTau / 2.0 * static_cast<double>(edge) / fadeFrames);
    }

    double Sample(uint64_t position, double level) const {
        const double envelope = Envelope(position);
        double value = Dither(position);
        if (envelope > 0.0) {
            value += level * envelope * std::sin(kTau * frequencyHz *
                static_cast<double>(position - leadFrames) / sampleRate);
        }
        return value;
    }
};

inline PulseShape MakePulse(unsigned sampleRate, unsigned durationMs, unsigned frequencyHz,
                            bool coldOutput) {
    PulseShape shape;
    shape.sampleRate = sampleRate;
    shape.frequencyHz = frequencyHz;
    shape.leadFrames = coldOutput ? FramesFor(kColdLeadInMs, sampleRate) : 0;
    shape.toneFrames = std::max<uint64_t>(1, FramesFor(durationMs, sampleRate));
    shape.tailFrames = FramesFor(kTailMs, sampleRate);
    shape.fadeFrames = std::max<uint64_t>(1, std::min<uint64_t>(
        FramesFor(kFadeMs, sampleRate), shape.toneFrames / 4));
    return shape;
}

// Continuous tone whose phase and level survive from one buffer to the next.
// It starts after a silent lead-in and the level moves towards its target
// smoothly: quickly downwards, so a volume increase in Windows is answered at
// once, and slowly upwards. Release() fades it out without cutting the wave.
class ContinuousTone {
public:
    void Configure(unsigned sampleRate, unsigned frequencyHz, unsigned leadInMs = 0) {
        sampleRate_ = std::max(1u, sampleRate);
        step_ = kTau * frequencyHz / sampleRate_;
        down_ = 1.0 - std::exp(-1.0 / (0.005 * sampleRate_));
        up_ = 1.0 - std::exp(-1.0 / (0.300 * sampleRate_));
        // Six time constants: the fade ends 52 dB down.
        release_ = 1.0 - std::exp(-6.0 / std::max<double>(1.0,
            static_cast<double>(FramesFor(kContinuousReleaseMs, sampleRate_))));
        leadFrames_ = FramesFor(leadInMs, sampleRate_);
    }
    void SetTarget(double level) {
        if (!releasing_) target_ = std::clamp(level, 0.0, kMaxDigitalLevel);
    }
    void Release() {
        releasing_ = true;
        target_ = 0.0;
    }
    double Level() const { return level_; }
    double Next() {
        if (position_ < leadFrames_) return Dither(position_++);
        level_ += (target_ - level_) *
            (releasing_ ? release_ : target_ < level_ ? down_ : up_);
        const double value = level_ * std::sin(phase_) + Dither(position_++);
        phase_ += step_;
        if (phase_ >= kTau) phase_ -= kTau;
        return value;
    }
private:
    unsigned sampleRate_ = 48000;
    double step_ = 0.0;
    double down_ = 1.0;
    double up_ = 1.0;
    double release_ = 1.0;
    double phase_ = 0.0;
    double level_ = 0.0;
    double target_ = 0.0;
    uint64_t position_ = 0;
    uint64_t leadFrames_ = 0;
    bool releasing_ = false;
};
}  // namespace Signal
