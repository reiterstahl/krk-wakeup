#include "pulse_worker.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>

namespace {
std::atomic<unsigned> attempts{0};
std::atomic<bool> wrongDestination{false};
void Check(bool condition, const char* description) {
    if (!condition) { std::cerr << description << '\n'; std::exit(1); }
}
template<class Predicate>
void Wait(Predicate predicate, const char* description) {
    const auto deadline = KrkWakeup::Clock::now() + std::chrono::seconds(4);
    while (!predicate()) {
        Check(KrkWakeup::Clock::now() < deadline, description);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
}

// A simulated renderer lets the actual threaded scheduler run without any
// Windows audio device, default-device changes, UI timer, or speaker hardware.
PulseResult RenderPulseIsolated(const std::wstring& endpointId, unsigned, unsigned, unsigned,
                                const std::function<bool()>& cancelled) {
    if (endpointId != L"saved-DAC") wrongDestination = true;
    ++attempts;
    const bool stopped = cancelled();
    return {!stopped, false, stopped ? E_ABORT : S_OK, "simulated", false};
}
void LogEvent(const char*, const std::string&) {}

int main() {
    using namespace KrkWakeup;
    using namespace std::chrono;
    std::atomic<bool> selected{true};
    std::atomic<bool> known{true};
    Config config;
    config.endpointId = L"saved-DAC";
    config.intervalSeconds = 1080;
    PulseWorker worker(config, [&] {
        AudioActivitySnapshot audio;
        audio.state = AudioActivityState::Active;
        audio.coverage = AudioMeterCoverage::SharedModeOnly;
        audio.defaultOutputKnown = known.load();
        audio.selectedIsDefault = selected.load();
        audio.defaultOutputTickMs = GetTickCount64();
        // Simulate a meter continuously reporting activity: it must not
        // postpone protection pulses or change their saved destination.
        audio.endpointMuteKnown = audio.endpointVolumeKnown = true;
        audio.endpointVolume = 1;
        audio.hasQualifyingActivity = true;
        audio.lastQualifyingActivityTickMs = audio.lastPollTickMs = GetTickCount64();
        return audio;
    });
    Wait([&] { return worker.Snapshot().hasSuccess; }, "Initial protection pulse must complete");
    worker.SetDisplayState(1);
    Wait([&] { return worker.Snapshot().effectiveIntervalSeconds == 1080; }, "Normal mode must retain 18 minutes");
    selected = false; // Headphones become default while the screen stays on.
    Wait([&] { return worker.Snapshot().effectiveIntervalSeconds == 300; }, "Headphones must activate protection without a UI timer");
    auto status = worker.Snapshot();
    Check(status.next <= Clock::now() + seconds(300), "Headphones must shorten an already pending deadline");
    selected = true;
    Wait([&] { return worker.Snapshot().effectiveIntervalSeconds == 1080; }, "Returning to the DAC must restore the normal interval");
    worker.SetDisplayState(2);
    Check(worker.Snapshot().effectiveIntervalSeconds == 300, "Dim display must activate protection");
    worker.SetDisplayState(1);
    worker.SetSessionLocked(true);
    Check(worker.Snapshot().effectiveIntervalSeconds == 300, "Lock must protect even if display notifications are missing");
    worker.SetSessionLocked(false);
    known = false;
    Wait([&] { return worker.Snapshot().effectiveIntervalSeconds == 300; }, "Unknown routing must fail safe");
    known = true;
    selected = false;
    config.intervalSeconds = 1; // Accelerate two real worker deadlines in this fixture.
    worker.Configure(config);
    const unsigned before = attempts;
    Wait([&] { return attempts.load() >= before + 2; }, "Continuously active meter must not suppress protection pulses");
    config.enabled = false;
    worker.Configure(config);
    worker.SetSessionLocked(true);
    worker.SetDisplayState(0);
    std::this_thread::sleep_for(milliseconds(1200));
    const unsigned paused = attempts;
    std::this_thread::sleep_for(milliseconds(1200));
    Check(attempts == paused, "Route, lock, and display changes must not enable a paused worker");
    worker.Test(PulseWorker::TestKind::Pulse);
    Wait([&] { return attempts.load() > paused; }, "Manual test must still work while paused");
    worker.Stop();
    Check(!wrongDestination, "All attempts must stay on the saved DAC while headphones are selected");
    std::cout << "Threaded routing, deadline, protection, pause, and pinned-destination checks passed.\n";
}
