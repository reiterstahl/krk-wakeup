#include "diagnostics.hpp"

#include <windows.h>
#include <cstdio>
#include <mutex>

namespace {
std::mutex g_logMutex;
std::wstring g_logPath;
std::wstring g_previousPath;
constexpr DWORD kMaxLogBytes = 1024 * 1024;
}

void InitDiagnostics(const std::wstring& settingsPath) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    const std::wstring folder = settingsPath.substr(0, settingsPath.find_last_of(L"\\/"));
    g_logPath = folder + L"\\diagnostics.log";
    g_previousPath = folder + L"\\diagnostics.previous.log";
}

const std::wstring& DiagnosticsPath() { return g_logPath; }

void LogEvent(const char* event, const std::string& details) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logPath.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &info) &&
        (info.nFileSizeHigh || info.nFileSizeLow >= kMaxLogBytes)) {
        if (!MoveFileExW(g_logPath.c_str(), g_previousPath.c_str(), MOVEFILE_REPLACE_EXISTING))
            return;
    }
    HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    char prefix[160];
    std::snprintf(prefix, sizeof(prefix),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ tick_ms=%llu event=%s",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
        utc.wMilliseconds, static_cast<unsigned long long>(GetTickCount64()), event);
    const std::string line = std::string(prefix) +
        (details.empty() ? "" : " " + details) + "\r\n";
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}
