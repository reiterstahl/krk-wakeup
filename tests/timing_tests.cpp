#include "playback.hpp"
#include "pulse_schedule.hpp"

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
          "Only finite audio at least as strong as the configured pulse may postpone it");
    const auto nowTime = start + seconds(600);
    const auto oldPulseDue = PulseSchedule::AfterSuccess(start, start + seconds(5), 300);
    auto decision = PulseSchedule::AutomaticDue(nowTime, oldPulseDue, 300,
        1000000, false, false, 0, 0, true, 400000);
    Check(decision.due <= nowTime && !decision.deferredByAudio,
          "Turning off the screen after ten quiet minutes must send immediately");
    const auto screenOnDue = PulseSchedule::AfterSuccess(start, start + seconds(5), 900);
    decision = PulseSchedule::AutomaticDue(nowTime, screenOnDue, 900,
        1000000, false, false, 0, 0, true, 400000);
    Check(decision.due == start + seconds(900),
          "Returning to screen-on mode must restore the normal deadline from the last pulse");
    decision = PulseSchedule::AutomaticDue(nowTime, nowTime, 300,
        1000000, true, true, 999900, 999950, true, 400000);
    Check(decision.deferredByAudio && decision.due == nowTime + milliseconds(299900),
          "Recent qualifying music must postpone the automatic pulse");
    decision = PulseSchedule::AutomaticDue(nowTime, nowTime, 300,
        1000000, true, true, 880000, 999950, true, 400000);
    Check(decision.deferredByAudio && decision.due == nowTime + seconds(180),
          "When music ends, count silence from its last observation, not from the next check");
    for (const auto ownTail : {uint64_t(400000), uint64_t(400080), uint64_t(400500)}) {
        decision = PulseSchedule::AutomaticDue(nowTime, nowTime, 900,
            1000000, true, true, ownTail, 999950, true, 400000);
        Check(!decision.deferredByAudio && decision.due == nowTime,
              "Our own pulse and its meter tail must not postpone its next pulse");
    }
    decision = PulseSchedule::AutomaticDue(nowTime, nowTime, 300,
        1000000, true, true, 999900, 997999, true, 400000);
    Check(!decision.deferredByAudio, "A stale meter must not suppress a due pulse");
    decision = PulseSchedule::AutomaticDue(nowTime, nowTime, 300,
        1000000, false, true, 999900, 999950, true, 400000);
    Check(!decision.deferredByAudio, "Unknown or muted audio must not postpone a pulse");
    decision = PulseSchedule::AutomaticDue(nowTime, nowTime, 300,
        1000000, true, true, 699999, 999950, true, 400000);
    Check(!decision.deferredByAudio, "Old observations must not postpone a pulse indefinitely");
    decision = PulseSchedule::AutomaticDue(nowTime, nowTime, 300,
        1000000, true, true, 1000001, 999950, false, 0);
    Check(!decision.deferredByAudio, "Inconsistent future timestamps must fail open");
    decision = PulseSchedule::AutomaticDue(nowTime, nowTime, 300,
        1000000, true, true, 999900, 999950, false, 0);
    Check(decision.deferredByAudio, "Music already playing at startup may defer the first automatic pulse");
    std::cout << "Audio drain, screen policy, observed-audio deferral, and pulse timing checks passed.\n";
}
