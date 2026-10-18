#include "diagnostics.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
constexpr uintmax_t kRotationBytes = 1024 * 1024;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct TempDirectory {
    fs::path path = fs::temp_directory_path() /
        (L"krk-wakeup-diagnostics-tests-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));

    TempDirectory() {
        Check(fs::create_directory(path), "Could not create a unique test directory");
    }
    ~TempDirectory() {
        SetDiagnosticsEnabled(false);
        std::error_code error;
        fs::remove_all(path, error);
        if (error) std::cerr << "Could not remove diagnostics test directory: "
                             << error.message() << '\n';
    }
};

std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    Check(file.is_open(), "Expected diagnostic file could not be opened");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    Check(file.is_open(), "Could not create a retention fixture");
    file << content;
    Check(file.good(), "Could not write a retention fixture");
}

size_t OwnLogCount(const fs::path& directory) {
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        const auto name = entry.path().filename().wstring();
        if (entry.is_regular_file() && name.rfind(L"krk-wakeup-", 0) == 0 &&
            entry.path().extension() == L".log") ++count;
    }
    return count;
}

std::string Instance(const std::string& contents) {
    const auto start = contents.find(" instance=");
    Check(start != std::string::npos, "Diagnostic event lacks an instance ID");
    const auto value = start + std::string(" instance=").size();
    const auto end = contents.find(' ', value);
    Check(end != std::string::npos && end > value, "Invalid diagnostic instance ID");
    return contents.substr(value, end - value);
}

void ExerciseDiagnostics() {
    TempDirectory temporary;
    const auto settings = temporary.path / L"settings.ini";
    const auto expectedLogs = temporary.path / L"Logs";
    InitDiagnostics(settings.wstring());
    Check(!DiagnosticsEnabled(), "Diagnostics must start disabled");
    Check(fs::path(DiagnosticsDirectory()) == expectedLogs,
          "Diagnostics should use the settings directory's Logs subdirectory");
    LogEvent("must_not_exist", "disabled=true");
    Check(SetDiagnosticsEnabled(false), "Disabling an inactive logger should succeed");
    Check(fs::is_empty(temporary.path),
          "Disabled diagnostics must not create a Logs directory or files");

    Check(SetDiagnosticsEnabled(true), "Could not enable diagnostics");
    Check(DiagnosticsEnabled(), "Enabled diagnostics state was not retained");
    const fs::path first = DiagnosticsPath();
    Check(first.parent_path() == expectedLogs && fs::is_regular_file(first),
          "Enabling diagnostics should create a file in Logs");
    LogEvent("test_probe", "key=value");
    const auto contents = Read(first);
    Check(contents.find("event=diagnostics_enabled version=") != std::string::npos,
          "An instance must report enabling and its version");
    Check(contents.find("event=test_probe key=value") != std::string::npos,
          "Diagnostic events must retain their event names and details");
    Check(contents.find("Z tick_ms=") != std::string::npos &&
          contents.find(" pid=") != std::string::npos,
          "Diagnostic events must carry UTC, elapsed time, and process identity");
    const auto firstInstance = Instance(contents);

    Check(SetDiagnosticsEnabled(false), "Could not disable diagnostics");
    Check(!DiagnosticsEnabled(), "Disabling diagnostics must clear its enabled state");
    const auto stopped = Read(first);
    Check(stopped.find("event=diagnostics_disabled") != std::string::npos,
          "Disabling should append its terminal event");
    LogEvent("must_not_be_written", "disabled=true");
    Check(Read(first) == stopped, "Disabled logging must stop appending events");

    // InitDiagnostics represents a fresh application instance. Real concurrent
    // processes also have distinct PIDs; leave one system-clock tick here.
    Sleep(20);
    InitDiagnostics(settings.wstring());
    Check(!DiagnosticsEnabled(), "A new instance must honor opt-in diagnostics");
    Check(SetDiagnosticsEnabled(true), "Could not enable the second instance");
    const fs::path second = DiagnosticsPath();
    Check(second != first && fs::is_regular_file(first),
          "Distinct instances must not overwrite the preceding instance's report");
    Check(Instance(Read(second)) != firstInstance,
          "Distinct diagnostic instances must carry distinct IDs");

    // Exercise rotation through normal events, preserving the completed part.
    const std::string payload(16 * 1024, 'x');
    for (unsigned attempt = 0; attempt < 100 && fs::path(DiagnosticsPath()) == second; ++attempt)
        LogEvent("rotation_probe", payload);
    const fs::path rotated = DiagnosticsPath();
    Check(rotated != second && fs::is_regular_file(second),
          "A report reaching one MiB must rotate without destroying its first part");
    Check(fs::file_size(second) >= kRotationBytes &&
          fs::file_size(second) < kRotationBytes + 32 * 1024,
          "Rotation must occur at one MiB, allowing the final event to finish");
    Check(Read(rotated).find("event=rotation_probe") != std::string::npos,
          "Rotation must continue writing into the new part");

    const auto unrelated = expectedLogs / L"another-app.log";
    const auto readme = expectedLogs / L"README.txt";
    const auto directory = expectedLogs / L"krk-wakeup-directory.log";
    Write(unrelated, "unrelated log\n");
    Write(readme, "keep this file\n");
    Check(fs::create_directory(directory), "Could not create the unrelated directory");
    for (unsigned index = 0; index < 16; ++index) {
        const auto oldLog = expectedLogs / (L"krk-wakeup-old-" + std::to_wstring(index) + L".log");
        Write(oldLog, "old report\n");
        fs::last_write_time(oldLog, fs::file_time_type::clock::now() -
                           std::chrono::hours(24 + index));
    }
    Check(OwnLogCount(expectedLogs) > 12, "Retention fixture must exceed twelve reports");
    LogEvent("retention_probe");
    Check(OwnLogCount(expectedLogs) <= 12,
          "Retention must keep at most twelve app diagnostic files");
    Check(fs::is_regular_file(rotated) &&
          Read(rotated).find("event=retention_probe") != std::string::npos,
          "Retention must preserve the current writable report");
    Check(!fs::exists(expectedLogs / L"krk-wakeup-old-15.log"),
          "Retention should remove the oldest completed reports first");
    Check(Read(unrelated) == "unrelated log\n" && Read(readme) == "keep this file\n" &&
          fs::is_directory(directory), "Retention must preserve unrelated files and directories");
}
}  // namespace

int main() {
    try {
        ExerciseDiagnostics();
        std::cout << "Opt-in diagnostics, instance identity, rotation, and retention passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Diagnostics test failed: " << error.what() << '\n';
        return 1;
    }
}
