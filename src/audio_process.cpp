#include "audio_process.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr wchar_t kHelperArgument[] = L"--audio-helper";
constexpr uint32_t kProtocolMagic = 0x4B524B41;  // KRKA
constexpr uint32_t kProtocolVersion = 1;
constexpr size_t kMaxEndpointChars = 4096;
constexpr DWORD kPipeBytes = 16384;
constexpr ULONGLONG kRendererOverheadMs = 10000;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    HANDLE Get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void Reset(HANDLE value = nullptr) {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

struct RequestHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t endpointChars;
    uint32_t durationMs;
    uint32_t frequencyHz;
    uint32_t levelPercent;
};

struct Reply {
    uint32_t magic = kProtocolMagic;
    uint32_t version = kProtocolVersion;
    uint32_t flags = 0;
    int32_t error = E_UNEXPECTED;
    char stage[64] = {};
};
static_assert(sizeof(RequestHeader) == 24, "The pipe protocol must have a fixed layout");
static_assert(sizeof(Reply) == 80, "The pipe protocol must have a fixed layout");
static_assert(sizeof(wchar_t) == 2, "The helper protocol uses Windows UTF-16");

PulseResult Failure(HRESULT error, const char* stage) {
    return {false, false, error, stage, false};
}

PulseResult Win32Failure(const char* stage) {
    const DWORD error = GetLastError();
    return Failure(HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE), stage);
}

bool ValidArguments(const std::wstring& endpointId, unsigned durationMs,
                    unsigned frequencyHz, unsigned levelPercent) {
    return !endpointId.empty() && endpointId.size() <= kMaxEndpointChars &&
        endpointId.find(L'\0') == std::wstring::npos &&
        durationMs >= 100 && durationMs <= 5000 &&
        frequencyHz >= 10 && frequencyHz <= 15000 &&
        levelPercent >= 1 && levelPercent <= 10;
}

bool MakePipe(Handle& read, Handle& write) {
    HANDLE rawRead = nullptr, rawWrite = nullptr;
    if (!CreatePipe(&rawRead, &rawWrite, nullptr, kPipeBytes)) return false;
    read.Reset(rawRead);
    write.Reset(rawWrite);
    return true;
}

bool ReadExactly(HANDLE pipe, void* destination, DWORD size) {
    auto* bytes = static_cast<BYTE*>(destination);
    while (size) {
        DWORD received = 0;
        if (!ReadFile(pipe, bytes, size, &received, nullptr) || !received) return false;
        bytes += received;
        size -= received;
    }
    return true;
}

std::wstring ExecutablePath() {
    std::wstring path(256, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!copied) return {};
        if (copied < path.size()) {
            path.resize(copied);
            return path;
        }
        if (path.size() >= 32768) {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            return {};
        }
        path.resize(std::min<size_t>(32768, path.size() * 2));
    }
}

class Attributes {
public:
    ~Attributes() { if (list_) DeleteProcThreadAttributeList(list_); }
    bool Init() {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        if (!size) return false;
        storage_.resize(size);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list, 2, 0, &size)) return false;
        list_ = list;
        return true;
    }
    LPPROC_THREAD_ATTRIBUTE_LIST Get() const { return list_; }
private:
    std::vector<BYTE> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

struct ChildProcess {
    Handle process;
    Handle thread;
    Handle job;
    ~ChildProcess() {
        if (process && WaitForSingleObject(process.Get(), 0) != WAIT_OBJECT_0) {
            // The job also covers unexpected parent termination. TerminateProcess
            // is a fallback if creation failed before job membership was applied.
            if (job) TerminateJobObject(job.Get(), ERROR_CANCELLED);
            TerminateProcess(process.Get(), ERROR_CANCELLED);
            WaitForSingleObject(process.Get(), 2000);
        }
    }
};

bool ParseHandle(const wchar_t* text, HANDLE& handle) {
    if (!text || !*text) return false;
    uintptr_t number = 0;
    for (const wchar_t* digit = text; *digit; ++digit) {
        if (*digit < L'0' || *digit > L'9') return false;
        const uintptr_t value = static_cast<uintptr_t>(*digit - L'0');
        if (number > (std::numeric_limits<uintptr_t>::max() - value) / 10) return false;
        number = number * 10 + value;
    }
    handle = reinterpret_cast<HANDLE>(number);
    return handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) == FILE_TYPE_PIPE;
}

bool ValidStage(const char* stage, size_t size) {
    const auto* end = static_cast<const char*>(std::memchr(stage, '\0', size));
    if (!end || end == stage) return false;
    return std::all_of(stage, end, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_';
    });
}

int RunHelper(HANDLE input, HANDLE output) {
    Handle request(input), response(output);
    RequestHeader header = {};
    if (!ReadExactly(request.Get(), &header, sizeof(header)) ||
        header.magic != kProtocolMagic || header.version != kProtocolVersion ||
        !header.endpointChars || header.endpointChars > kMaxEndpointChars) return 2;
    std::wstring endpoint(header.endpointChars, L'\0');
    if (!ReadExactly(request.Get(), endpoint.data(),
                     static_cast<DWORD>(endpoint.size() * sizeof(wchar_t))) ||
        !ValidArguments(endpoint, header.durationMs, header.frequencyHz, header.levelPercent)) return 2;
    request.Reset();
    PulseResult result;
    try {
        result = RenderPulse(endpoint, header.durationMs, header.frequencyHz, header.levelPercent);
    } catch (...) {
        result = Failure(E_UNEXPECTED, "renderer_exception");
    }
    Reply reply;
    reply.flags = (result.success ? 1u : 0u) | (result.unavailable ? 2u : 0u) |
                  (result.blockedByVolume ? 4u : 0u);
    reply.error = static_cast<int32_t>(result.error);
    const std::string stage = result.stage;
    if (stage.size() < sizeof(reply.stage) && ValidStage(stage.c_str(), stage.size() + 1)) {
        std::memcpy(reply.stage, stage.c_str(), stage.size() + 1);
    } else {
        reply.flags = 0;
        reply.error = E_UNEXPECTED;
        std::memcpy(reply.stage, "renderer_result", sizeof("renderer_result"));
    }
    DWORD written = 0;
    return WriteFile(response.Get(), &reply, sizeof(reply), &written, nullptr) &&
        written == sizeof(reply) ? 0 : 3;
}
}  // namespace

PulseResult RenderPulseIsolated(const std::wstring& endpointId, unsigned durationMs,
                                unsigned frequencyHz, unsigned levelPercent,
                                const std::function<bool()>& cancelled) {
    if (!ValidArguments(endpointId, durationMs, frequencyHz, levelPercent))
        return Failure(E_INVALIDARG, "renderer_request");
    if (cancelled && cancelled()) return Failure(HRESULT_FROM_WIN32(ERROR_CANCELLED), "cancelled");
    const ULONGLONG deadline = GetTickCount64() + durationMs + kRendererOverheadMs;

    Handle requestRead, requestWrite, responseRead, responseWrite;
    if (!MakePipe(requestRead, requestWrite) || !MakePipe(responseRead, responseWrite))
        return Win32Failure("renderer_pipe");
    if (!SetHandleInformation(requestRead.Get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ||
        !SetHandleInformation(responseWrite.Get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
        return Win32Failure("renderer_inherit");

    RequestHeader header = {kProtocolMagic, kProtocolVersion,
        static_cast<uint32_t>(endpointId.size()), durationMs, frequencyHz, levelPercent};
    std::vector<BYTE> request(sizeof(header) + endpointId.size() * sizeof(wchar_t));
    std::memcpy(request.data(), &header, sizeof(header));
    std::memcpy(request.data() + sizeof(header), endpointId.data(),
                endpointId.size() * sizeof(wchar_t));
    // The complete request must fit in an empty pipe. Prefilling it before
    // launching the helper avoids a blocking write if child startup stalls.
    DWORD capacity = 0;
    if (!GetNamedPipeInfo(requestRead.Get(), nullptr, nullptr, &capacity, nullptr))
        return Win32Failure("renderer_pipe_info");
    if (capacity < request.size()) return Failure(E_UNEXPECTED, "renderer_pipe_capacity");
    DWORD written = 0;
    if (!WriteFile(requestWrite.Get(), request.data(), static_cast<DWORD>(request.size()),
                   &written, nullptr)) return Win32Failure("renderer_request_write");
    if (written != request.size()) return Failure(E_UNEXPECTED, "renderer_request_write");
    requestWrite.Reset();

    ChildProcess child;
    child.job.Reset(CreateJobObjectW(nullptr, nullptr));
    if (!child.job) return Win32Failure("renderer_job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(child.job.Get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) return Win32Failure("renderer_job_limits");

    Attributes attributes;
    if (!attributes.Init()) return Win32Failure("renderer_attributes");
    HANDLE inherited[] = {requestRead.Get(), responseWrite.Get()};
    HANDLE job = child.job.Get();
    if (!UpdateProcThreadAttribute(attributes.Get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof(inherited), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attributes.Get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                   &job, sizeof(job), nullptr, nullptr))
        return Win32Failure("renderer_attributes_update");
    const std::wstring executable = ExecutablePath();
    if (executable.empty()) return Win32Failure("renderer_executable");
    std::wstring command = L"\"" + executable + L"\" " + kHelperArgument + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(requestRead.Get())) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(responseWrite.Get()));
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = attributes.Get();
    PROCESS_INFORMATION process = {};
    // Job membership is applied atomically at creation, before any child code
    // can execute; even a parent crash here cannot orphan a suspended helper.
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr, &startup.StartupInfo, &process)) return Win32Failure("renderer_create");
    child.process.Reset(process.hProcess);
    child.thread.Reset(process.hThread);
    requestRead.Reset();
    responseWrite.Reset();
    if (ResumeThread(child.thread.Get()) == static_cast<DWORD>(-1)) return Win32Failure("renderer_resume");
    child.thread.Reset();

    for (;;) {
        if (cancelled && cancelled()) return Failure(HRESULT_FROM_WIN32(ERROR_CANCELLED), "cancelled");
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return Failure(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "renderer_timeout");
        const DWORD waited = WaitForSingleObject(child.process.Get(),
            static_cast<DWORD>(std::min<ULONGLONG>(100, deadline - now)));
        if (waited == WAIT_OBJECT_0) break;
        if (waited != WAIT_TIMEOUT) return Win32Failure("renderer_wait");
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(child.process.Get(), &exitCode)) return Win32Failure("renderer_exit_query");
    if (exitCode != 0) return Failure(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED), "renderer_exit");
    DWORD available = 0;
    if (!PeekNamedPipe(responseRead.Get(), nullptr, 0, nullptr, &available, nullptr))
        return Win32Failure("renderer_response_peek");
    if (available != sizeof(Reply)) return Failure(E_UNEXPECTED, "renderer_response_size");
    Reply reply;
    if (!ReadExactly(responseRead.Get(), &reply, sizeof(reply))) return Win32Failure("renderer_response_read");
    if (reply.magic != kProtocolMagic || reply.version != kProtocolVersion || (reply.flags & ~7u) ||
        !ValidStage(reply.stage, sizeof(reply.stage))) return Failure(E_UNEXPECTED, "renderer_response");
    return {(reply.flags & 1u) != 0, (reply.flags & 2u) != 0,
        static_cast<HRESULT>(reply.error), reply.stage, (reply.flags & 4u) != 0};
}

bool RunAudioHelperIfRequested(PWSTR commandLine, int& exitCode) {
    if (!commandLine) return false;
    while (iswspace(*commandLine)) ++commandLine;
    if (!*commandLine) return false;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(commandLine, &count);
    if (!arguments) return false;
    struct ArgumentsScope { LPWSTR* data; ~ArgumentsScope() { LocalFree(data); } } scope{arguments};
    if (!count || std::wcscmp(arguments[0], kHelperArgument) != 0) return false;
    exitCode = 2;
    HANDLE input = nullptr, output = nullptr;
    if (count == 3 && ParseHandle(arguments[1], input) && ParseHandle(arguments[2], output) && input != output)
        exitCode = RunHelper(input, output);
    return true;
}
