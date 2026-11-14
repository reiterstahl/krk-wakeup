// Plays through a real Windows output and reads the result back from the
// output's own peak meter. It makes sound and changes the output volume, so
// it only runs when KRK_AUDIO_TEST=1 (the CI job with a virtual sound card).

#include "audio.hpp"
#include "keep_alive.hpp"
#include "signal.hpp"
#include "wasapi_util.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {
void Check(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        std::exit(1);
    }
}

// Highest peak the output's meter reports while `action` runs. The meter
// reads the mix before the output volume is applied.
template<class Action>
float PeakDuring(IAudioMeterInformation* meter, Action action) {
    std::atomic<bool> done{false};
    std::atomic<float> highest{0.0f};
    std::thread sampler([&] {
        while (!done) {
            float peak = 0.0f;
            if (SUCCEEDED(meter->GetPeakValue(&peak)) && peak > highest) highest = peak;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    action();
    done = true;
    sampler.join();
    return highest;
}

template<class Predicate>
bool WaitFor(Predicate predicate, int milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    while (!predicate()) {
        if (GetTickCount64() >= deadline) return false;
        Sleep(50);
    }
    return true;
}
}  // namespace

int main() {
    char* requested = nullptr;
    size_t length = 0;
    const bool enabled = _dupenv_s(&requested, &length, "KRK_AUDIO_TEST") == 0 && requested &&
        std::string(requested) == "1";
    free(requested);
    if (!enabled) {
        std::cout << "Skipped: set KRK_AUDIO_TEST=1 to play through the default output.\n";
        return 0;
    }
    Check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM");
    ComPtr<IMMDeviceEnumerator> enumerator;
    Check(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS(&enumerator))), "device enumerator");
    ComPtr<IMMDevice> device;
    Check(SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)),
          "KRK_AUDIO_TEST=1 needs a default output");
    LPWSTR rawId = nullptr;
    Check(SUCCEEDED(device->GetId(&rawId)), "output id");
    const std::wstring id(rawId);
    CoTaskMemFree(rawId);
    ComPtr<IAudioMeterInformation> meter;
    Check(SUCCEEDED(device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(meter.GetAddressOf()))), "peak meter");
    ComPtr<IAudioEndpointVolume> volume;
    Check(SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(volume.GetAddressOf()))), "output volume");
    Check(SUCCEEDED(volume->SetMute(FALSE, nullptr)) &&
          SUCCEEDED(volume->SetMasterVolumeLevelScalar(1.0f, nullptr)), "set full volume");

    // 1. A pulse on an idle output waits behind the lead-in and still arrives whole.
    Check(!WasapiUtil::HasActiveSession(device.Get()), "the output must start idle");
    PulseResult result;
    ULONGLONG started = GetTickCount64();
    float peak = PeakDuring(meter.Get(), [&] { result = RenderPulse(id, 400, 440, 5, 0); });
    ULONGLONG elapsed = GetTickCount64() - started;
    std::cout << "cold pulse: stage=" << result.stage << " elapsed_ms=" << elapsed << " peak=" << peak << '\n';
    Check(result.success && result.stage == "complete_cold", "an idle output must get the lead-in");
    Check(elapsed >= Signal::kColdLeadInMs + 400, "the lead-in must precede the tone");
    Check(peak > 0.035f && peak < 0.065f, "a 5 % pulse must reach the output at 5 %");

    // 2. The continuous stream plays, at its level, and Windows consumes it.
    KeepAliveStream stream;
    KeepAliveConfig config;
    config.endpointId = id;
    config.enabled = true;
    config.frequencyHz = 20;
    config.levelPercent = 2;
    config.compensateVolume = true;
    stream.Configure(config);
    Check(WaitFor([&] { return stream.Snapshot().state == KeepAliveState::Running; }, 8000),
          std::string("the continuous stream must start; state=") +
          KeepAliveStateName(stream.Snapshot().state) + " stage=" + stream.Snapshot().stage);
    Sleep(1500);
    const KeepAliveSnapshot before = stream.Snapshot();
    peak = PeakDuring(meter.Get(), [] { Sleep(2000); });
    const KeepAliveSnapshot after = stream.Snapshot();
    std::cout << "continuous: peak=" << peak << " played_ms=" << after.playedMs
              << " underruns=" << after.underruns << " path_gain=" << after.pathGain << '\n';
    Check(peak > 0.014f && peak < 0.026f, "the continuous tone must play at 2 %");
    Check(after.framesWritten > before.framesWritten + after.sampleRate, "the stream must keep writing");
    Check(after.playedKnown && after.playedMs >= before.playedMs + 1500,
          "Windows must be consuming the stream");
    Check(WasapiUtil::HasActiveSession(device.Get()), "the stream must keep the output active");

    // 3. With the stream open, a pulse needs no lead-in.
    started = GetTickCount64();
    peak = PeakDuring(meter.Get(), [&] { result = RenderPulse(id, 400, 440, 5, 0); });
    elapsed = GetTickCount64() - started;
    std::cout << "warm pulse: stage=" << result.stage << " elapsed_ms=" << elapsed << " peak=" << peak << '\n';
    Check(result.success && result.stage == "complete", "an output kept awake must not wait");
    Check(elapsed < Signal::kColdLeadInMs + 400, "a warm pulse must skip the lead-in");
    Check(peak > 0.035f, "the pulse must be mixed over the continuous tone");

    // 4. Lowering the Windows volume raises the digital level to match.
    Check(SUCCEEDED(volume->SetMasterVolumeLevelScalar(0.5f, nullptr)), "set half volume");
    float decibels = 0.0f;
    Check(SUCCEEDED(volume->GetMasterVolumeLevel(&decibels)), "read volume");
    const double gain = Signal::DecibelsToLinear(decibels);
    const double expected = Signal::CompensatedLevel(0.02, true, true, gain);
    Sleep(1500);
    peak = PeakDuring(meter.Get(), [] { Sleep(1500); });
    std::cout << "compensated: volume_db=" << decibels << " expected=" << expected << " peak=" << peak << '\n';
    Check(expected > 0.021, "half volume must attenuate on this output");
    Check(peak > expected * 0.8 && peak < expected * 1.25,
          "the digital level must offset the Windows volume");
    Check(SUCCEEDED(volume->SetMasterVolumeLevelScalar(1.0f, nullptr)), "restore volume");
    Check(WaitFor([&] { return stream.Snapshot().digitalLevel < 0.021; }, 2000),
          "raising the volume must lower the digital level at once");

    // 5. A muted output is reported, and the stream stops when disabled.
    Check(SUCCEEDED(volume->SetMute(TRUE, nullptr)), "mute");
    Check(WaitFor([&] { return stream.Snapshot().state == KeepAliveState::Muted; }, 2000),
          "a muted output must be reported");
    result = RenderPulse(id, 400, 440, 5, kPulseCompensateVolume);
    Check(!result.success && result.blockedByVolume, "a muted pulse must not count as sent");
    Check(SUCCEEDED(volume->SetMute(FALSE, nullptr)), "unmute");
    config.enabled = false;
    stream.Configure(config);
    Check(WaitFor([&] { return stream.Snapshot().state == KeepAliveState::Off; }, 3000),
          "disabling must stop the stream");
    Check(WaitFor([&] { return !WasapiUtil::HasActiveSession(device.Get()); }, 3000),
          "a stopped stream must release the output");

    // 6. An output that does not exist is reported and retried, never replaced.
    config.endpointId = L"not-a-real-output";
    config.enabled = true;
    stream.Configure(config);
    Check(WaitFor([&] { return stream.Snapshot().state == KeepAliveState::Unavailable; }, 3000),
          "a missing output must be reported as unavailable");
    Check(!WasapiUtil::HasActiveSession(device.Get()), "a missing output must not fall back to another");
    std::cout << "Real output: lead-in, continuous stream, compensation, mute, and stop checks passed.\n";
    return 0;
}
