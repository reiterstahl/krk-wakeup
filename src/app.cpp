#include "audio.hpp"

#include <windows.h>
#include <audioclient.h>
#include <commctrl.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {
constexpr wchar_t kClassName[] = L"KRKWakeupSettings";
constexpr wchar_t kRunName[] = L"KRKWakeup";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTimerId = 1;
constexpr UINT kTrayId = 1;
constexpr int kDevice = 100;
constexpr int kRefresh = 101;
constexpr int kIdle = 102;
constexpr int kInterval = 103;
constexpr int kDuration = 104;
constexpr int kFrequency = 105;
constexpr int kLevel = 106;
constexpr int kEnabled = 107;
constexpr int kStartup = 108;
constexpr int kSave = 109;
constexpr int kTest = 110;
constexpr int kStatus = 111;
constexpr int kMenuOpen = 201;
constexpr int kMenuToggle = 202;
constexpr int kMenuTest = 203;
constexpr int kMenuExit = 204;

struct Config {
    std::wstring endpointId;
    unsigned idleSeconds = 900;
    unsigned intervalSeconds = 300;
    unsigned durationMs = 1000;
    unsigned frequencyHz = 440;
    unsigned levelPercent = 1;
    bool enabled = true;
    bool startup = false;
};

struct Endpoint {
    std::wstring id;
    std::wstring name;
};

struct Status {
    std::wstring message;
    Clock::time_point next;
    bool hasNext = false;
};

class PulseWorker {
public:
    explicit PulseWorker(Config config) : config_(std::move(config)), next_(Clock::now()) {
        thread_ = std::thread([this] { Loop(); });
    }
    ~PulseWorker() { Stop(); }
    PulseWorker(const PulseWorker&) = delete;
    PulseWorker& operator=(const PulseWorker&) = delete;

    void Configure(const Config& config, bool manualTestFollows = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool activate = (!config_.enabled || config_.endpointId.empty() ||
                               config_.endpointId != config.endpointId) &&
                              config.enabled && !config.endpointId.empty();
        config_ = config;
        ++generation_;
        next_ = Clock::now() + (activate && !manualTestFollows ? std::chrono::seconds(0)
                                                               : std::chrono::seconds(config.intervalSeconds));
        status_ = config.endpointId.empty() ? L"Selecciona una salida de audio."
                  : config.enabled ? L"Activo." : L"Pausado.";
        condition_.notify_one();
    }
    void Test() {
        std::lock_guard<std::mutex> lock(mutex_);
        testPending_ = true;
        condition_.notify_one();
    }
    Status Snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return {status_, next_, config_.enabled && !config_.endpointId.empty()};
    }
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            condition_.notify_one();
        }
        if (thread_.joinable()) thread_.join();
    }
private:
    void Loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            if (config_.endpointId.empty()) {
                status_ = L"Selecciona una salida de audio.";
                condition_.wait(lock, [this] { return stopping_ || !config_.endpointId.empty(); });
                continue;
            }
            if (!config_.enabled && !testPending_) {
                if (status_ == L"Iniciando…") status_ = L"Pausado.";
                condition_.wait(lock, [this] { return stopping_ || testPending_ || config_.enabled; });
                continue;
            }
            if (!testPending_ && Clock::now() < next_) {
                const auto generation = generation_;
                condition_.wait_until(lock, next_, [this, generation] {
                    return stopping_ || testPending_ || generation_ != generation;
                });
                continue;
            }
            if (stopping_) break;
            const bool manual = testPending_;
            testPending_ = false;
            const Config current = config_;
            const auto generation = generation_;
            status_ = manual ? L"Enviando pulso de prueba…" : L"Enviando pulso…";
            lock.unlock();
            const PulseResult result = RenderPulse(current.endpointId, current.durationMs,
                                                    current.frequencyHz, current.levelPercent);
            lock.lock();
            if (stopping_) break;
            if (generation != generation_) continue;
            if (result.success) {
                status_ = manual ? L"Pulso de prueba enviado." : L"Pulso enviado.";
                next_ = Clock::now() + std::chrono::seconds(current.intervalSeconds);
            } else if (result.unavailable) {
                status_ = L"La salida seleccionada no está disponible. Reintentando…";
                next_ = Clock::now() + std::chrono::seconds(15);
            } else {
                wchar_t errorText[96];
                swprintf_s(errorText, L"Error de audio 0x%08lX. Reintentando…",
                           static_cast<unsigned long>(result.error));
                status_ = errorText;
                next_ = Clock::now() + std::chrono::seconds(30);
            }
        }
    }
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
    Config config_;
    Clock::time_point next_;
    std::wstring status_ = L"Iniciando…";
    uint64_t generation_ = 0;
    bool stopping_ = false;
    bool testPending_ = false;
};

HWND g_window = nullptr;
Config g_config;
std::unique_ptr<PulseWorker> g_worker;
std::wstring g_settingsPath;
std::vector<Endpoint> g_endpoints;
UINT g_taskbarCreated = 0;
HICON g_icon = nullptr;

std::wstring ReadText(HWND control) {
    const int size = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(size) + 1, L'\0');
    GetWindowTextW(control, value.data(), size + 1);
    value.resize(size);
    return value;
}

bool ParseUnsigned(const std::wstring& text, unsigned& value) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), iswdigit)) return false;
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(text.c_str(), &end, 10);
    if (*end || parsed > 1000000) return false;
    value = static_cast<unsigned>(parsed);
    return true;
}

bool ParseTime(const std::wstring& text, unsigned& seconds) {
    const size_t colon = text.find(L':');
    if (colon == std::wstring::npos || text.find(L':', colon + 1) != std::wstring::npos)
        return false;
    unsigned minutes = 0, remainder = 0;
    if (!ParseUnsigned(text.substr(0, colon), minutes) ||
        !ParseUnsigned(text.substr(colon + 1), remainder) || remainder >= 60 ||
        minutes > 240) return false;
    seconds = minutes * 60 + remainder;
    return true;
}

std::wstring FormatTime(unsigned seconds) {
    wchar_t buffer[24];
    swprintf_s(buffer, L"%02u:%02u", seconds / 60, seconds % 60);
    return buffer;
}

unsigned ReadNumber(const wchar_t* key, unsigned fallback, unsigned low, unsigned high) {
    const unsigned value = GetPrivateProfileIntW(L"Settings", key, fallback,
                                                  g_settingsPath.c_str());
    return std::clamp(value, low, high);
}

bool InitSettingsPath() {
    PWSTR local = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                            nullptr, &local);
    if (FAILED(hr)) return false;
    std::wstring folder(local);
    CoTaskMemFree(local);
    folder += L"\\KRKWakeup";
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    g_settingsPath = folder + L"\\settings.ini";
    return true;
}

void LoadConfig() {
    wchar_t id[4096] = {};
    GetPrivateProfileStringW(L"Settings", L"EndpointId", L"", id,
                             static_cast<DWORD>(std::size(id)), g_settingsPath.c_str());
    g_config.endpointId = id;
    g_config.idleSeconds = ReadNumber(L"IdleSeconds", 900, 60, 14400);
    g_config.intervalSeconds = ReadNumber(L"IntervalSeconds", 300, 10, 3600);
    g_config.durationMs = ReadNumber(L"DurationMs", 1000, 100, 5000);
    g_config.frequencyHz = ReadNumber(L"FrequencyHz", 440, 80, 15000);
    g_config.levelPercent = ReadNumber(L"LevelPercent", 1, 1, 10);
    g_config.enabled = ReadNumber(L"Enabled", 1, 0, 1) == 1;
    g_config.startup = ReadNumber(L"Startup", 0, 0, 1) == 1;
}

bool SaveConfig() {
    const auto write = [](const wchar_t* key, const std::wstring& value) {
        return WritePrivateProfileStringW(L"Settings", key, value.c_str(),
                                          g_settingsPath.c_str()) != 0;
    };
    return write(L"EndpointId", g_config.endpointId) &&
           write(L"IdleSeconds", std::to_wstring(g_config.idleSeconds)) &&
           write(L"IntervalSeconds", std::to_wstring(g_config.intervalSeconds)) &&
           write(L"DurationMs", std::to_wstring(g_config.durationMs)) &&
           write(L"FrequencyHz", std::to_wstring(g_config.frequencyHz)) &&
           write(L"LevelPercent", std::to_wstring(g_config.levelPercent)) &&
           write(L"Enabled", g_config.enabled ? L"1" : L"0") &&
           write(L"Startup", g_config.startup ? L"1" : L"0");
}

bool SetStartup(bool enabled) {
    HKEY key = nullptr;
    const wchar_t* path = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0,
                                  KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return false;
    if (enabled) {
        wchar_t executable[MAX_PATH + 1] = {};
        if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) {
            RegCloseKey(key);
            return false;
        }
        const std::wstring command = std::wstring(L"\"") + executable + L"\"";
        result = RegSetValueExW(key, kRunName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kRunName);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

std::vector<Endpoint> EnumerateEndpoints() {
    std::vector<Endpoint> endpoints;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) return endpoints;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                               &collection))) return endpoints;
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) return endpoints;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId))) continue;
        std::wstring id(rawId);
        CoTaskMemFree(rawId);
        std::wstring name = id;
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal) name = value.pwszVal;
            PropVariantClear(&value);
        }
        endpoints.push_back({std::move(id), std::move(name)});
    }
    return endpoints;
}

void RefreshEndpoints() {
    HWND combo = GetDlgItem(g_window, kDevice);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    g_endpoints = EnumerateEndpoints();
    int selected = -1;
    for (size_t i = 0; i < g_endpoints.size(); ++i) {
        const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0,
                                           reinterpret_cast<LPARAM>(g_endpoints[i].name.c_str()));
        if (index != CB_ERR && g_endpoints[i].id == g_config.endpointId)
            selected = static_cast<int>(index);
    }
    if (selected >= 0) SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

void UpdateFields() {
    SetWindowTextW(GetDlgItem(g_window, kIdle), FormatTime(g_config.idleSeconds).c_str());
    SetWindowTextW(GetDlgItem(g_window, kInterval), FormatTime(g_config.intervalSeconds).c_str());
    SetWindowTextW(GetDlgItem(g_window, kDuration), std::to_wstring(g_config.durationMs).c_str());
    SetWindowTextW(GetDlgItem(g_window, kFrequency), std::to_wstring(g_config.frequencyHz).c_str());
    SetWindowTextW(GetDlgItem(g_window, kLevel), std::to_wstring(g_config.levelPercent).c_str());
    SendMessageW(GetDlgItem(g_window, kEnabled), BM_SETCHECK,
                 g_config.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(g_window, kStartup), BM_SETCHECK,
                 g_config.startup ? BST_CHECKED : BST_UNCHECKED, 0);
}

void ShowSettings() {
    RefreshEndpoints();
    UpdateFields();
    ShowWindow(g_window, SW_SHOWNORMAL);
    SetForegroundWindow(g_window);
}

void ShowInputError(const wchar_t* message) {
    MessageBoxW(g_window, message, L"KRK Wakeup", MB_OK | MB_ICONWARNING);
}

bool SaveFromWindow(bool manualTestFollows = false) {
    const LRESULT selected = SendMessageW(GetDlgItem(g_window, kDevice), CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR || static_cast<size_t>(selected) >= g_endpoints.size()) {
        ShowInputError(L"Selecciona la salida USB del SMSL SU-1.");
        return false;
    }
    Config candidate = g_config;
    candidate.endpointId = g_endpoints[static_cast<size_t>(selected)].id;
    if (!ParseTime(ReadText(GetDlgItem(g_window, kIdle)), candidate.idleSeconds) ||
        candidate.idleSeconds < 60 || candidate.idleSeconds > 14400) {
        ShowInputError(L"Introduce el reposo estimado como mm:ss, entre 01:00 y 240:00.");
        return false;
    }
    if (!ParseTime(ReadText(GetDlgItem(g_window, kInterval)), candidate.intervalSeconds) ||
        candidate.intervalSeconds < 10 || candidate.intervalSeconds > 3600) {
        ShowInputError(L"Introduce el intervalo como mm:ss, entre 00:10 y 60:00.");
        return false;
    }
    if (!ParseUnsigned(ReadText(GetDlgItem(g_window, kDuration)), candidate.durationMs) ||
        candidate.durationMs < 100 || candidate.durationMs > 5000) {
        ShowInputError(L"La duración debe estar entre 100 y 5000 ms.");
        return false;
    }
    if (!ParseUnsigned(ReadText(GetDlgItem(g_window, kFrequency)), candidate.frequencyHz) ||
        candidate.frequencyHz < 80 || candidate.frequencyHz > 15000) {
        ShowInputError(L"La frecuencia debe estar entre 80 y 15000 Hz.");
        return false;
    }
    if (!ParseUnsigned(ReadText(GetDlgItem(g_window, kLevel)), candidate.levelPercent) ||
        candidate.levelPercent < 1 || candidate.levelPercent > 10) {
        ShowInputError(L"El nivel digital debe estar entre 1 % y 10 %.");
        return false;
    }
    candidate.enabled = SendMessageW(GetDlgItem(g_window, kEnabled), BM_GETCHECK, 0, 0) == BST_CHECKED;
    candidate.startup = SendMessageW(GetDlgItem(g_window, kStartup), BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (candidate.startup != g_config.startup && !SetStartup(candidate.startup)) {
        ShowInputError(L"No se pudo actualizar el inicio con Windows.");
        return false;
    }
    const Config previous = g_config;
    g_config = candidate;
    if (!SaveConfig()) {
        g_config = previous;
        SetStartup(previous.startup);
        ShowInputError(L"No se pudo guardar la configuración.");
        return false;
    }
    g_worker->Configure(g_config, manualTestFollows);
    if (candidate.intervalSeconds >= candidate.idleSeconds)
        MessageBoxW(g_window, L"El intervalo iguala o supera el reposo estimado. Los parlantes podrían apagarse antes del siguiente pulso.",
                    L"KRK Wakeup", MB_OK | MB_ICONWARNING);
    return true;
}

void AddTrayIcon() {
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kTrayId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = g_icon;
    wcscpy_s(data.szTip, L"KRK Wakeup");
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void UpdateStatus() {
    if (!g_worker) return;
    const Status status = g_worker->Snapshot();
    std::wstring message = status.message;
    if (status.hasNext && g_config.enabled) {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(status.next - Clock::now()).count();
        if (remaining < 0) remaining = 0;
        message += L"  Próximo: " + FormatTime(static_cast<unsigned>(remaining));
    }
    SetWindowTextW(GetDlgItem(g_window, kStatus), message.c_str());
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kTrayId;
    data.uFlags = NIF_TIP;
    const std::wstring tip = L"KRK Wakeup — " + status.message;
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void ToggleEnabled() {
    if (g_config.endpointId.empty()) {
        ShowSettings();
        return;
    }
    g_config.enabled = !g_config.enabled;
    if (!SaveConfig()) ShowInputError(L"No se pudo guardar la configuración.");
    g_worker->Configure(g_config);
    SendMessageW(GetDlgItem(g_window, kEnabled), BM_SETCHECK,
                 g_config.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    UpdateStatus();
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Configuración…");
    AppendMenuW(menu, MF_STRING, kMenuToggle,
                g_config.enabled ? L"Pausar" : L"Activar");
    AppendMenuW(menu, MF_STRING, kMenuTest, L"Probar señal");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Salir");
    POINT point;
    GetCursorPos(&point);
    SetForegroundWindow(g_window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y,
                   0, g_window, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_window, WM_NULL, 0, 0);
}

void AddControl(const wchar_t* klass, const wchar_t* title, DWORD style,
                int x, int y, int width, int height, int id) {
    HWND control = CreateWindowExW(0, klass, title, WS_CHILD | WS_VISIBLE | style,
                                   x, y, width, height, g_window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

void BuildWindow() {
    AddControl(L"STATIC", L"Salida de audio para los KRK:", 0, 18, 17, 270, 20, 0);
    AddControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
               18, 40, 365, 240, kDevice);
    AddControl(L"BUTTON", L"Actualizar", WS_TABSTOP, 393, 39, 91, 26, kRefresh);
    AddControl(L"STATIC", L"Reposo estimado (mm:ss)", 0, 18, 84, 240, 20, 0);
    AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
               341, 80, 143, 24, kIdle);
    AddControl(L"STATIC", L"Intervalo entre pulsos (mm:ss)", 0, 18, 117, 270, 20, 0);
    AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
               341, 113, 143, 24, kInterval);
    AddControl(L"STATIC", L"Duración del pulso (ms)", 0, 18, 150, 250, 20, 0);
    AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
               341, 146, 143, 24, kDuration);
    AddControl(L"STATIC", L"Frecuencia (Hz)", 0, 18, 183, 250, 20, 0);
    AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
               341, 179, 143, 24, kFrequency);
    AddControl(L"STATIC", L"Nivel digital (%), comenzar en 1", 0, 18, 216, 280, 20, 0);
    AddControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
               341, 212, 143, 24, kLevel);
    AddControl(L"BUTTON", L"Mantener activos mientras se ejecuta",
               BS_AUTOCHECKBOX | WS_TABSTOP, 18, 254, 340, 25, kEnabled);
    AddControl(L"BUTTON", L"Iniciar con Windows",
               BS_AUTOCHECKBOX | WS_TABSTOP, 18, 282, 300, 25, kStartup);
    AddControl(L"BUTTON", L"Guardar", WS_TABSTOP, 281, 318, 95, 30, kSave);
    AddControl(L"BUTTON", L"Probar señal", WS_TABSTOP, 384, 318, 100, 30, kTest);
    AddControl(L"STATIC", L"", 0, 18, 367, 466, 35, kStatus);
    UpdateFields();
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g_taskbarCreated && g_taskbarCreated) {
        AddTrayIcon();
        return 0;
    }
    switch (message) {
    case WM_CREATE:
        g_window = window;
        BuildWindow();
        AddTrayIcon();
        SetTimer(window, kTimerId, 1000, nullptr);
        return 0;
    case WM_TIMER:
        if (wparam == kTimerId) UpdateStatus();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kRefresh: RefreshEndpoints(); return 0;
        case kSave: SaveFromWindow(); UpdateStatus(); return 0;
        case kTest: if (SaveFromWindow(true)) g_worker->Test(); return 0;
        case kMenuOpen: ShowSettings(); return 0;
        case kMenuToggle: ToggleEnabled(); return 0;
        case kMenuTest:
            if (g_config.endpointId.empty()) ShowSettings();
            else g_worker->Test();
            return 0;
        case kMenuExit: DestroyWindow(window); return 0;
        }
        break;
    case kTrayMessage:
        if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == NIN_KEYSELECT) {
            ShowTrayMenu();
            return 0;
        }
        if (LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == WM_LBUTTONDBLCLK) {
            ShowSettings();
            return 0;
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kTimerId);
        if (g_worker) g_worker->Stop();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\KRKWakeupSingleInstance");
    if (!instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"KRK Wakeup ya está en ejecución.", L"KRK Wakeup",
                    MB_OK | MB_ICONINFORMATION);
        if (instanceMutex) CloseHandle(instanceMutex);
        return 0;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        CloseHandle(instanceMutex);
        return 1;
    }
    if (!InitSettingsPath()) {
        MessageBoxW(nullptr, L"No se pudo abrir la carpeta de configuración.",
                    L"KRK Wakeup", MB_OK | MB_ICONERROR);
        CoUninitialize();
        CloseHandle(instanceMutex);
        return 1;
    }
    LoadConfig();
    g_worker = std::make_unique<PulseWorker>(g_config);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_icon = LoadIconW(nullptr, IDI_APPLICATION);
    WNDCLASSW klass = {};
    klass.lpfnWndProc = WindowProcedure;
    klass.hInstance = instance;
    klass.lpszClassName = kClassName;
    klass.hIcon = g_icon;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&klass)) {
        g_worker.reset();
        CoUninitialize();
        CloseHandle(instanceMutex);
        return 1;
    }
    HWND window = CreateWindowExW(0, kClassName, L"KRK Wakeup — configuración",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 520, 450,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) {
        g_worker.reset();
        CoUninitialize();
        CloseHandle(instanceMutex);
        return 1;
    }
    RefreshEndpoints();
    if (g_config.endpointId.empty()) ShowSettings();
    UpdateStatus();
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    g_worker.reset();
    CoUninitialize();
    CloseHandle(instanceMutex);
    return static_cast<int>(message.wParam);
}
