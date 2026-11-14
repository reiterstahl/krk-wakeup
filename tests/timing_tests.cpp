#include "playback.hpp"
#include "pulse_schedule.hpp"
#include "signal.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>

void Check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << description << '\n';
        std::exit(1);
    }
}

int main() {
    uint64_t time = 0;
    auto now = [&] { return time; };
    auto wait = [&] { time += 5; };
    auto stalled = [](uint32_t& padding) { padding = 64; return true; };
    Check(DrainPlayback(stalled, now, wait, 1500) == DrainResult::TimedOut,
          "A stalled device must not count as a successful pulse");
    Check(time == 1500, "A stalled drain must have a finite deadline");
    time = 0;
    auto consuming = [&](uint32_t& padding) { padding = time < 20 ? 64 : 0; return true; };
    Check(DrainPlayback(consuming, now, wait, 1500) == DrainResult::Complete,
          "A fully consumed buffer should succeed");
    Check(time == 20, "A drained buffer should finish without waiting to timeout");
    auto failed = [](uint32_t&) { return false; };
    Check(DrainPlayback(failed, now, wait, 1500) == DrainResult::AudioError,
          "A device error must survive the drain operation");

    using namespace std::chrono;
    const auto start = PulseSchedule::Time{} + seconds(100);
    Check(PulseSchedule::AfterSuccess(start, start + seconds(3), 1080) == start + seconds(1080),
          "An 18-minute interval must be measured between pulse starts");
    const auto due = start + seconds(10);
    Check(PulseSchedule::Reconfigure(due, start + seconds(5), 1080, false, false) == due,
          "Saving settings must not postpone the next pulse");
    Check(PulseSchedule::Reconfigure(start + seconds(100), start, 10, false, false) == due,
          "Shortening the interval must bring the next pulse forward");
    Check(PulseSchedule::Reconfigure(due, start, 1080, true, false) == start,
          "Reactivation must send immediately");
    Check(PulseSchedule::AfterSuccess(start, start + seconds(2000), 1080) == start + seconds(3080),
          "A long stall must not queue a burst of catch-up pulses");
    Check(PulseSchedule::EffectiveInterval(900, false) == 900,
          "Screen-on mode must preserve the configured interval");
    Check(PulseSchedule::EffectiveInterval(900, true) == 300,
          "Screen-off mode must cap the interval at five minutes");
    Check(PulseSchedule::EffectiveInterval(60, true) == 60,
          "Screen-off mode must not lengthen a shorter configured interval");
    Check(PulseSchedule::QualifyingPeak(0.05f, 5) && !PulseSchedule::QualifyingPeak(0.049f, 5) &&
          !PulseSchedule::QualifyingPeak(0.0f, 1) &&
          !PulseSchedule::QualifyingPeak(std::numeric_limits<float>::quiet_NaN(), 5),
          "Only finite audio at least as strong as the configured pulse counts as qualifying");
    Check(!PulseSchedule::ProtectionRequired(1, false, true, true),
          "Only an on-screen, unlocked session with the saved default output uses normal mode");
    for (int screen : {-1, 0, 2})
        Check(PulseSchedule::ProtectionRequired(screen, false, true, true),
              "Unknown, off, and dim displays require protection");
    Check(PulseSchedule::ProtectionRequired(1, true, true, true) &&
          PulseSchedule::ProtectionRequired(1, false, true, false) &&
          PulseSchedule::ProtectionRequired(1, false, false, true),
          "Lock, another default output, or unknown routing must activate protection");
    Check(PulseSchedule::FreshDefaultOutput(true, 10000, 9000) &&
          !PulseSchedule::FreshDefaultOutput(true, 10000, 7999) &&
          !PulseSchedule::FreshDefaultOutput(true, 10000, 10001) &&
          !PulseSchedule::FreshDefaultOutput(false, 10000, 9999),
          "Stale, future, or unknown route samples must not disable protection");
    // Signal shape and volume compensation.
    using namespace Signal;
    Check(CompensatedLevel(0.02, true, true, 1.0) == 0.02, "Full volume needs no compensation");
    Check(std::abs(CompensatedLevel(0.02, true, true, 0.5) - 0.04) < 1e-12,
          "Half the volume must double the digital level");
    Check(std::abs(CompensatedLevel(0.02, true, true, 0.001) - 0.2) < 1e-12,
          "Compensation must stop at ten times the requested level");
    Check(CompensatedLevel(0.10, true, true, 0.01) == kMaxDigitalLevel,
          "Compensation must leave digital headroom");
    Check(CompensatedLevel(0.02, false, true, 0.1) == 0.02 &&
          CompensatedLevel(0.02, true, false, 0.1) == 0.02 &&
          CompensatedLevel(0.02, true, true, 0.0) == 0.02 &&
          CompensatedLevel(0.02, true, true, 4.0) == 0.02,
          "Disabled, unknown, zero, or above-unity gain must not change the level");
    Check(!CompensationLimited(0.02, true, true, 0.5) && CompensationLimited(0.02, true, true, 0.01) &&
          !CompensationLimited(0.02, false, true, 0.01),
          "The cap must be reported only when it weakens the signal");
    Check(std::abs(DecibelsToLinear(-20.0) - 0.1) < 1e-12 && DecibelsToLinear(0.0) == 1.0,
          "Decibels must convert to a linear gain");
    const PulseShape cold = MakePulse(48000, 1000, 440, true);
    const PulseShape warm = MakePulse(48000, 1000, 440, false);
    Check(cold.leadFrames == 72000 && warm.leadFrames == 0 && cold.toneFrames == 48000 &&
          cold.Total() == 72000 + 48000 + 12000,
          "An idle output must get a lead-in; a playing output must not wait");
    double leadPeak = 0, tonePeak = 0, tailPeak = 0;
    for (uint64_t frame = 0; frame < cold.Total(); ++frame) {
        const double value = std::abs(cold.Sample(frame, 0.05));
        if (frame < cold.leadFrames) leadPeak = std::max(leadPeak, value);
        else if (frame < cold.leadFrames + cold.toneFrames) tonePeak = std::max(tonePeak, value);
        else tailPeak = std::max(tailPeak, value);
    }
    Check(leadPeak <= kDitherLevel && tailPeak <= kDitherLevel && leadPeak > 0,
          "Lead-in and tail must carry only inaudible dither");
    Check(tonePeak > 0.049 && tonePeak < 0.0502, "The tone must reach the requested level");
    Check(std::abs(cold.Sample(cold.leadFrames, 0.05)) <= kDitherLevel * 1.01 &&
          cold.Envelope(cold.leadFrames + cold.toneFrames - 1) == 0.0 &&
          cold.Envelope(cold.leadFrames + cold.toneFrames / 2) == 1.0,
          "The tone must start and end at zero");
    Check(MakePulse(48000, 100, 440, false).fadeFrames == 1200,
          "A short pulse must still have a full-level section");
    ContinuousTone tone;
    tone.Configure(48000, 20);
    tone.SetTarget(0.02);
    double early = 0, late = 0, previous = 0, jump = 0;
    for (int frame = 0; frame < 48000 * 3; ++frame) {
        const double value = tone.Next();
        jump = std::max(jump, std::abs(value - previous));
        previous = value;
        if (frame < 480) early = std::max(early, std::abs(value));
        if (frame >= 48000 * 2) late = std::max(late, std::abs(value));
    }
    Check(early < 0.002, "The continuous tone must fade in");
    Check(late > 0.0198 && late < 0.0202, "The continuous tone must settle at its level");
    Check(jump < 0.001, "The continuous tone must keep its phase between buffers");
    tone.SetTarget(0.0);
    for (int frame = 0; frame < 2400; ++frame) tone.Next();
    Check(tone.Level() < 0.02 * 0.001, "Lowering the level must take effect within 50 ms");
    ContinuousTone gentle;
    gentle.Configure(48000, 20, kContinuousLeadInMs);
    gentle.SetTarget(0.2);
    double lead = 0;
    for (int frame = 0; frame < 48000 * 2; ++frame) lead = std::max(lead, std::abs(gentle.Next()));
    Check(lead <= kDitherLevel, "The continuous tone must stay silent during its lead-in");
    Check(std::abs(gentle.Next()) <= kDitherLevel * 1.5, "The tone must start at the zero of its wave");
    for (int frame = 0; frame < 48000 * 3; ++frame) gentle.Next();
    Check(gentle.Level() > 0.199, "The continuous tone must reach its level after the lead-in");
    gentle.Release();
    previous = gentle.Next();
    jump = 0;
    double afterCycle = 0;
    const int releaseFrames = static_cast<int>(FramesFor(kContinuousReleaseMs, 48000));
    for (int frame = 0; frame < releaseFrames; ++frame) {
        const double value = gentle.Next();
        jump = std::max(jump, std::abs(value - previous));
        previous = value;
        if (frame == 2400) afterCycle = gentle.Level();
    }
    Check(afterCycle > 0.2 * 0.3, "The closing fade must span several cycles of a 20 Hz tone");
    Check(gentle.Level() < 0.2 * 0.004, "The closing fade must end in silence");
    Check(jump < 0.001, "The closing fade must not cut the wave");
    gentle.SetTarget(0.2);
    for (int frame = 0; frame < 4800; ++frame) gentle.Next();
    Check(gentle.Level() < 0.2 * 0.004, "A released tone must stay silent");
    Format pcm16{false, 16, 2, 4, 48000};
    uint8_t frameBytes[4] = {};
    WriteFrame(frameBytes, pcm16, 0.5);
    int16_t left = 0, right = 0;
    std::memcpy(&left, frameBytes, 2);
    std::memcpy(&right, frameBytes + 2, 2);
    Check(left == 16384 && right == 16384, "Every channel must receive the sample");
    std::cout << "Audio drain, screen policy, pulse timing, signal shape, and volume compensation checks passed.\n";
}
