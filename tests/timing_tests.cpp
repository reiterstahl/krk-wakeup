#include "playback.hpp"
#include "pulse_schedule.hpp"

#include <cstdlib>
#include <iostream>

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
    std::cout << "Audio drain and pulse timing checks passed.\n";
}
