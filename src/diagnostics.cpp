#include "diagnostics.hpp"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

namespace {
std::mutex g_logMutex;
std::wstring g_directory;
std::wstring g_logPath;
std::string g_instance;
bool g_enabled = false;
unsigned g_part = 0;
constexpr DWORD kMaxLogBytes = 1024 * 1024;
constexpr size_t kMaxFiles = 12;

void ChoosePath() {
    const std::wstring instance(g_instance.begin(), g_instance.end());
    g_logPath = g_directory + L"\\krk-wakeup-" + instance +
        (g_part ? L"-part" + std::to_wstring(g_part) : L"") + L".log";
}

void PruneLogs() {
    struct Entry { std::wstring path; ULONGLONG stamp; };
    std::vector<Entry> files;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((g_directory + L"\\krk-wakeup-*.log").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const ULONGLONG stamp = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                data.ftLastWriteTime.dwLowDateTime;
        files.push_back({g_directory + L"\\" + data.cFileName, stamp});
    } while (FindNextFileW(search, &data));
    FindClose(search);
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.stamp < b.stamp; });
    size_t retained = files.size();
    for (const auto& file : files) {
        if (retained <= kMaxFiles) break;
        if (file.path != g_logPath && DeleteFileW(file.path.c_str())) --retained;
    }
}

bool WriteLocked(const char* event, const std::string& details) {
    if (!g_enabled || g_directory.empty()) return true;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &info) &&
        (info.nFileSizeHigh || info.nFileSizeLow >= kMaxLogBytes)) {
        ++g_part;
        ChoosePath();
    }
    HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    char prefix[224];
    std::snprintf(prefix, sizeof(prefix),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ tick_ms=%llu instance=%s pid=%lu event=%s",
        static_cast<unsigned>(utc.wYear), static_cast<unsigned>(utc.wMonth),
        static_cast<unsigned>(utc.wDay), static_cast<unsigned>(utc.wHour),
        static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond),
        static_cast<unsigned>(utc.wMilliseconds), static_cast<unsigned long long>(GetTickCount64()),
        g_instance.c_str(), GetCurrentProcessId(), event);
    const std::string line = std::string(prefix) + (details.empty() ? "" : " " + details) + "\r\n";
    DWORD written = 0;
    const bool ok = WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) &&
                    written == line.size();
    CloseHandle(file);
    PruneLogs();
    return ok;
}
}  // namespace

void InitDiagnostics(const std::wstring& settingsPath) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    const std::wstring folder = settingsPath.substr(0, settingsPath.find_last_of(L"\\/"));
    g_directory = folder + L"\\Logs";
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    char id[80];
    std::snprintf(id, sizeof(id), "%04u%02u%02uT%02u%02u%02u%03uZ-%lu",
        static_cast<unsigned>(utc.wYear), static_cast<unsigned>(utc.wMonth),
        static_cast<unsigned>(utc.wDay), static_cast<unsigned>(utc.wHour),
        static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond),
        static_cast<unsigned>(utc.wMilliseconds), GetCurrentProcessId());
    g_instance = id;
    g_part = 0;
    g_enabled = false;
    ChoosePath();
}

bool SetDiagnosticsEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (enabled == g_enabled) return true;
    if (enabled) {
        if (!CreateDirectoryW(g_directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
        g_enabled = true;
        if (!WriteLocked("diagnostics_enabled", "version=" KRK_WAKEUP_VERSION)) {
            g_enabled = false;
            return false;
        }
    } else {
        WriteLocked("diagnostics_disabled", {});
        g_enabled = false;
    }
    return true;
}

bool DiagnosticsEnabled() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_enabled;
}

std::wstring DiagnosticsPath() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_logPath;
}

std::wstring DiagnosticsDirectory() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_directory;
}

void LogEvent(const char* event, const std::string& details) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    WriteLocked(event, details);
}
