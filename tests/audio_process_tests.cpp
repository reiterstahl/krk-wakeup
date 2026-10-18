#include "audio_process.hpp"

#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
constexpr wchar_t kEndpoint[] = L"round_trip_\u03bb_\" USB endpoint";

void Check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << description << '\n';
        std::exit(1);
    }
}

bool HasLiveHelpers() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    Check(snapshot != INVALID_HANDLE_VALUE, "Could not inspect helper cleanup");
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID != GetCurrentProcessId()) continue;
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, entry.th32ProcessID);
            if (!process) continue;
            found = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
            CloseHandle(process);
            if (found) break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

class StartedEvent {
public:
    explicit StartedEvent(const wchar_t* suffix) {
        const std::wstring name = L"Local\\KRKWakeupRendererTest-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + suffix;
        endpoint = L"hang|" + name;
        handle = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        Check(handle != nullptr, "Could not create helper test event");
    }
    ~StartedEvent() { CloseHandle(handle); }
    bool IsSet() const { return WaitForSingleObject(handle, 0) == WAIT_OBJECT_0; }
    HANDLE handle = nullptr;
    std::wstring endpoint;
};
}  // namespace

// This test replaces the real audio renderer. The exact same process isolation
// and IPC implementation can therefore be exercised without an audio device or
// any production-only fake-device switches.
PulseResult RenderPulse(const std::wstring& endpointId, unsigned durationMs,
                        unsigned frequencyHz, unsigned levelPercent) {
    if (durationMs != 100 || frequencyHz != 80 || levelPercent != 1)
        return {false, false, E_INVALIDARG, "stub_arguments", false};
    if (endpointId == kEndpoint || endpointId == std::wstring(4096, L'x'))
        return {true, false, S_OK, "complete", false};
    if (endpointId == L"failure")
        return {false, true, HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED), "device_state", false};
    if (endpointId == L"muted")
        return {false, false, HRESULT_FROM_WIN32(ERROR_NOT_READY), "endpoint_muted", true};
    if (endpointId == L"crash") ExitProcess(73);
    if (endpointId.rfind(L"hang|", 0) == 0) {
        HANDLE started = OpenEventW(EVENT_MODIFY_STATE, FALSE, endpointId.substr(5).c_str());
        if (!started) return {false, false, E_UNEXPECTED, "stub_event", false};
        SetEvent(started);
        CloseHandle(started);
        Sleep(INFINITE);
    }
    return {false, false, E_INVALIDARG, "stub_endpoint", false};
}

int main() {
    // The application's wWinMain receives the tail directly. Reconstruct the
    // helper's numeric argument tail for this console test executable.
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    Check(arguments != nullptr, "Could not parse test command line");
    std::wstring commandLine;
    for (int i = 1; i < argumentCount; ++i) {
        if (!commandLine.empty()) commandLine += L' ';
        commandLine += arguments[i];
    }
    LocalFree(arguments);
    int helperExit = 0;
    if (RunAudioHelperIfRequested(commandLine.data(), helperExit)) return helperExit;

    const auto neverCancel = [] { return false; };
    auto result = RenderPulseIsolated(kEndpoint, 100, 80, 1, neverCancel);
    Check(result.success && result.error == S_OK && result.stage == "complete",
          "Helper must preserve the selected endpoint and numeric arguments");
    Check(!HasLiveHelpers(), "Successful helper must be reaped");
    result = RenderPulseIsolated(std::wstring(4096, L'x'), 100, 80, 1, neverCancel);
    Check(result.success, "A maximum-length endpoint must fit the request pipe without truncation");
    result = RenderPulseIsolated(L"failure", 100, 80, 1, neverCancel);
    Check(!result.success && result.unavailable && !result.blockedByVolume &&
          result.error == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) && result.stage == "device_state",
          "Helper must preserve device error details");
    result = RenderPulseIsolated(L"muted", 100, 80, 1, neverCancel);
    Check(!result.success && !result.unavailable && result.blockedByVolume && result.stage == "endpoint_muted",
          "Helper must preserve mute detection");
    result = RenderPulseIsolated(L"crash", 100, 80, 1, neverCancel);
    Check(!result.success && result.stage == "renderer_exit", "Helper crash must become a retryable failure");
    Check(!HasLiveHelpers(), "Failed helper must be reaped");
    result = RenderPulseIsolated(std::wstring(4097, L'x'), 100, 80, 1, neverCancel);
    Check(!result.success && result.stage == "renderer_request", "Oversized endpoint must be rejected");
    result = RenderPulseIsolated(kEndpoint, 100, 80, 1, [] { return true; });
    Check(!result.success && result.stage == "cancelled" && !HasLiveHelpers(),
          "An already cancelled attempt must not launch a helper");

    StartedEvent cancellation(L"cancel");
    ULONGLONG cancellationStarted = 0;
    const ULONGLONG cancelCall = GetTickCount64();
    result = RenderPulseIsolated(cancellation.endpoint, 100, 80, 1, [&] {
        if (!cancellationStarted && cancellation.IsSet()) cancellationStarted = GetTickCount64();
        return cancellationStarted && GetTickCount64() - cancellationStarted >= 150;
    });
    Check(cancellationStarted && !result.success && result.stage == "cancelled" &&
          result.error == HRESULT_FROM_WIN32(ERROR_CANCELLED),
          "Cancellation must interrupt a renderer blocked inside its audio call");
    Check(GetTickCount64() - cancelCall < 5000 && !HasLiveHelpers(),
          "Cancelled helper must stop promptly and leave no live renderer");

    StartedEvent timeout(L"timeout");
    const ULONGLONG timeoutCall = GetTickCount64();
    result = RenderPulseIsolated(timeout.endpoint, 100, 80, 1, neverCancel);
    const ULONGLONG elapsed = GetTickCount64() - timeoutCall;
    Check(timeout.IsSet() && !result.success && result.stage == "renderer_timeout" &&
          result.error == HRESULT_FROM_WIN32(ERROR_TIMEOUT),
          "The overall deadline must interrupt an audio call which never returns");
    Check(elapsed >= 10000 && elapsed < 18000 && !HasLiveHelpers(),
          "Timed out helper must be reaped within the bounded deadline");
    result = RenderPulseIsolated(kEndpoint, 100, 80, 1, neverCancel);
    Check(result.success && !HasLiveHelpers(), "Rendering must work after a cancelled or stalled attempt");
    std::cout << "Isolated renderer IPC, error, crash, cancellation, timeout, and cleanup checks passed.\n";
}
