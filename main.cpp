#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <atlbase.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <tlhelp32.h>
#include <wbemidl.h>
#include <comdef.h>
#include <psapi.h>
#include <sstream>
#include <iomanip>
#include <locale>
#include <codecvt>
#include <chrono>
#include <ctime>
#include "version.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "psapi.lib")

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001
#define ID_TRAY_CONFIGURE 1002
#define ID_TRAY_DIAGNOSE 1003

#define IDC_CONFIG_LIST 2001
#define IDC_CONFIG_OK   2002
#define IDC_CONFIG_CANCEL 2003
#define IDC_CONFIG_SESSION_BTN 2004

#define IDC_SESSION_OK            3001
#define IDC_SESSION_CANCEL        3002
#define IDC_SESSION_ALL           3003
#define IDC_SESSION_ORDINAL_RADIO 3004
#define IDC_SESSION_ORDINAL_EDIT  3005
#define IDC_SESSION_STREAM        3006
#define IDC_SESSION_FIRST_RADIO   3100  // +index per session
#define IDC_SESSION_FIRST_TESTBTN 3300  // +index per session

HINSTANCE g_hInst = nullptr;
HWND g_hWnd = nullptr;
NOTIFYICONDATAA g_nid = { 0 };
HANDLE g_workerThread = nullptr;
HANDLE g_exitEvent = nullptr;

HWND g_hConfigWnd = nullptr;

bool debugMode = false;

// ---------- Data model ----------

struct DeviceConfig {
    std::wstring name;
    // StreamOnly: mute only the Discord PID(s) that appear exclusively on this device
    // and not on any other render device. Reliably targets the stream audio session.
    enum class Filter { All, ByFingerprint, ByOrdinal, StreamOnly } filter = Filter::StreamOnly;
    std::wstring fingerprint;
    int ordinal = -1;
    bool offline = false;
};

struct DiscordSession {
    CComPtr<IAudioSessionControl> pControl;
    CComPtr<IAudioSessionControl2> pControl2;
    DWORD pid = 0;
    std::wstring sessionIdentifier;
    std::wstring sessionInstanceIdentifier;
    std::wstring displayName;
    std::wstring iconPath;
    GUID groupingParam = GUID_NULL;
    int ordinalOnDevice = 0;
};

// ---------- Forward declarations ----------

DWORD WINAPI WorkerThreadProc(LPVOID);
LRESULT CALLBACK TrayWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ConfigWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SessionPickerWndProc(HWND, UINT, WPARAM, LPARAM);
void AddTrayIcon(HWND);
void RemoveTrayIcon(HWND);
void ShowTrayMenu(HWND, POINT);
void ShowConfigDialog(HWND owner);
std::vector<std::wstring> EnumerateRenderDevices(IMMDeviceEnumerator* pEnum = nullptr);
std::vector<DeviceConfig> LoadDevicesFromFile(const std::wstring& filename);
bool SaveDevicesToFile(const std::wstring& filename, const std::vector<DeviceConfig>& devices);
std::vector<DiscordSession> EnumerateDiscordSessions(const std::wstring& deviceName, IMMDeviceEnumerator* pEnum = nullptr);
std::wstring StableSessionFingerprint(const std::wstring& sid);
bool isDiscordRunning();
int checkToMute();
void RunDiagnostic(HWND owner);
bool ShowSessionPicker(HWND owner, const std::wstring& deviceName, DeviceConfig& ioCfg);
static std::unordered_map<std::wstring, std::vector<DWORD>> BuildDiscordPidsByDevice(IMMDeviceEnumerator* pEnum = nullptr);

// ---------- Process / session helpers ----------

std::wstring GetProcessName(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return L"";
    wchar_t buf[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProc, 0, buf, &size);
    CloseHandle(hProc);
    if (!ok) return L"";
    const wchar_t* slash = wcsrchr(buf, L'\\');
    return slash ? slash + 1 : buf;
}

// SessionIdentifier format (typical):
//   {0.0.0.00000000}.{<mmdevice-guid>}|\Device\HarddiskVolumeN\...\app-1.2.3\Discord.exe%b{<session-guid>}%b<flags>
// The app-version path segment rotates on Discord updates, so strip the path and keep only the
// trailing GUID + flags tail, which should be stable across updates for a given session type.
std::wstring StableSessionFingerprint(const std::wstring& sid) {
    if (sid.empty()) return L"";
    size_t lastPct = sid.rfind(L"%b");
    std::wstring tail;
    if (lastPct != std::wstring::npos) {
        // Walk backwards to find the previous %b so we capture {guid}%b<flags>
        size_t prev = sid.rfind(L"%b", lastPct > 0 ? lastPct - 1 : 0);
        if (prev != std::wstring::npos && prev < lastPct) {
            tail = sid.substr(prev + 2);
        } else {
            tail = sid.substr(lastPct + 2);
        }
    }
    if (tail.empty()) {
        // Fallback: take substring after last '|' if present
        size_t bar = sid.rfind(L'|');
        tail = (bar == std::wstring::npos) ? sid : sid.substr(bar + 1);
    }
    return tail;
}

static CComPtr<IMMDevice> FindDeviceByName(const std::wstring& deviceName, IMMDeviceEnumerator* pEnumIn = nullptr) {
    CComPtr<IMMDeviceEnumerator> pLocalEnum;
    IMMDeviceEnumerator* pEnum = pEnumIn;
    if (!pEnum) {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&pLocalEnum)))) return nullptr;
        pEnum = pLocalEnum;
    }

    CComPtr<IMMDeviceCollection> pDevices;
    if (FAILED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pDevices))) return nullptr;

    UINT count = 0;
    pDevices->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        CComPtr<IMMDevice> pDevice;
        if (FAILED(pDevices->Item(i, &pDevice))) continue;
        CComPtr<IPropertyStore> pProps;
        if (FAILED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) continue;
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
            if (deviceName == varName.pwszVal) {
                PropVariantClear(&varName);
                return pDevice;
            }
        }
        PropVariantClear(&varName);
    }
    return nullptr;
}

std::vector<DiscordSession> EnumerateDiscordSessions(const std::wstring& deviceName, IMMDeviceEnumerator* pEnum) {
    std::vector<DiscordSession> sessions;
    CComPtr<IMMDevice> pDevice = FindDeviceByName(deviceName, pEnum);
    if (!pDevice) return sessions;

    CComPtr<IAudioSessionManager2> pMgr;
    if (FAILED(pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
        nullptr, (void**)&pMgr))) return sessions;

    CComPtr<IAudioSessionEnumerator> pEnumSessions;
    if (FAILED(pMgr->GetSessionEnumerator(&pEnumSessions))) return sessions;

    int sessionCount = 0;
    pEnumSessions->GetCount(&sessionCount);

    int ordinal = 0;
    for (int j = 0; j < sessionCount; j++) {
        CComPtr<IAudioSessionControl> pControl;
        if (FAILED(pEnumSessions->GetSession(j, &pControl))) continue;
        CComQIPtr<IAudioSessionControl2> pControl2(pControl);
        if (!pControl2) continue;

        DWORD pid = 0;
        pControl2->GetProcessId(&pid);
        if (_wcsicmp(GetProcessName(pid).c_str(), L"Discord.exe") != 0) continue;

        DiscordSession s;
        s.pControl = pControl;
        s.pControl2 = pControl2;
        s.pid = pid;

        LPWSTR sid = nullptr;
        if (SUCCEEDED(pControl2->GetSessionIdentifier(&sid)) && sid) {
            s.sessionIdentifier = sid;
            CoTaskMemFree(sid);
        }
        LPWSTR siid = nullptr;
        if (SUCCEEDED(pControl2->GetSessionInstanceIdentifier(&siid)) && siid) {
            s.sessionInstanceIdentifier = siid;
            CoTaskMemFree(siid);
        }
        LPWSTR dn = nullptr;
        if (SUCCEEDED(pControl->GetDisplayName(&dn)) && dn) {
            s.displayName = dn;
            CoTaskMemFree(dn);
        }
        LPWSTR ip = nullptr;
        if (SUCCEEDED(pControl->GetIconPath(&ip)) && ip) {
            s.iconPath = ip;
            CoTaskMemFree(ip);
        }
        pControl->GetGroupingParam(&s.groupingParam);
        s.ordinalOnDevice = ordinal++;
        sessions.push_back(std::move(s));
    }
    return sessions;
}

// Builds a map of { deviceName -> [Discord PIDs on that device] } for all active render devices.
// Called once per mute cycle instead of once per device, eliminating the O(N^2) re-enumeration.
static std::unordered_map<std::wstring, std::vector<DWORD>> BuildDiscordPidsByDevice(IMMDeviceEnumerator* pEnumIn) {
    std::unordered_map<std::wstring, std::vector<DWORD>> result;
    CComPtr<IMMDeviceEnumerator> pLocalEnum;
    IMMDeviceEnumerator* pEnum = pEnumIn;
    if (!pEnum) {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&pLocalEnum)))) return result;
        pEnum = pLocalEnum;
    }
    CComPtr<IMMDeviceCollection> pDevices;
    if (FAILED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pDevices))) return result;
    UINT count = 0; pDevices->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        CComPtr<IMMDevice> pDevice;
        if (FAILED(pDevices->Item(i, &pDevice))) continue;
        CComPtr<IPropertyStore> pProps;
        if (FAILED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) continue;
        PROPVARIANT varName; PropVariantInit(&varName);
        bool nameOk = SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal;
        std::wstring devName = nameOk ? varName.pwszVal : L"";
        PropVariantClear(&varName);
        if (devName.empty()) continue;

        CComPtr<IAudioSessionManager2> pMgr;
        if (FAILED(pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&pMgr))) continue;
        CComPtr<IAudioSessionEnumerator> pEnumSessions;
        if (FAILED(pMgr->GetSessionEnumerator(&pEnumSessions))) continue;
        int sc = 0; pEnumSessions->GetCount(&sc);
        for (int j = 0; j < sc; j++) {
            CComPtr<IAudioSessionControl> pCtl;
            if (FAILED(pEnumSessions->GetSession(j, &pCtl))) continue;
            CComQIPtr<IAudioSessionControl2> pCtl2(pCtl);
            if (!pCtl2) continue;
            DWORD pid = 0; pCtl2->GetProcessId(&pid);
            if (_wcsicmp(GetProcessName(pid).c_str(), L"Discord.exe") == 0)
                result[devName].push_back(pid);
        }
    }
    return result;}

// Returns: 0 = device not found, 1 = at least one matching session muted,
// 2 = device found but no matching Discord session (retry-worthy).
// otherPids: Discord PIDs on devices other than cfg.name (pre-computed by caller).
int MuteDiscordOnDevice(const DeviceConfig& cfg, const std::vector<DWORD>& otherPids, IMMDeviceEnumerator* pEnum = nullptr) {
    CComPtr<IMMDevice> pDevice = FindDeviceByName(cfg.name, pEnum);
    if (!pDevice) {
        #ifdef DEBUG
        std::wstring output = L"Device not found: " + cfg.name + L"\n";
        OutputDebugStringW(output.c_str());
        #endif
        return 0;
    }

    auto sessions = EnumerateDiscordSessions(cfg.name, pEnum);
    if (sessions.empty()) {
        #ifdef DEBUG
        OutputDebugStringA("Discord session not found on this device.\n");
        #endif
        return 2;
    }

    if (cfg.filter == DeviceConfig::Filter::StreamOnly && otherPids.empty()) {
        #ifdef DEBUG
        OutputDebugStringA("StreamOnly: no Discord sessions on other devices yet; deferring.\n");
        #endif
        return 2; // retry-worthy; main-audio device may not have opened its session yet
    }

    bool muted = false;
    bool anyFilterMatched = false;
    for (auto& s : sessions) {
        bool matches = false;
        switch (cfg.filter) {
            case DeviceConfig::Filter::All:
                matches = true;
                break;
            case DeviceConfig::Filter::ByFingerprint:
                matches = !cfg.fingerprint.empty() &&
                          StableSessionFingerprint(s.sessionIdentifier) == cfg.fingerprint;
                break;
            case DeviceConfig::Filter::ByOrdinal:
                matches = (s.ordinalOnDevice == cfg.ordinal);
                break;
            case DeviceConfig::Filter::StreamOnly:
                // Mute sessions whose PID IS shared with another device (stream duplicate).
                // The exclusive-to-this-device session is kept audible (voice chat routed here for capture).
                matches = std::find(otherPids.begin(), otherPids.end(), s.pid) != otherPids.end();
                break;
        }
        if (!matches) continue;
        anyFilterMatched = true;
        CComQIPtr<ISimpleAudioVolume> pVol(s.pControl);
        if (pVol && SUCCEEDED(pVol->SetMute(TRUE, nullptr))) {
            muted = true;
            #ifdef DEBUG
            std::wstring output = L"Muted Discord on: " + cfg.name + L"\n";
            OutputDebugStringW(output.c_str());
            #endif
        }
    }

    if (!anyFilterMatched) return 2; // filter-target not present yet, retry
    return muted ? 1 : 2;
}

std::vector<std::wstring> EnumerateRenderDevices(IMMDeviceEnumerator* pEnumIn) {
    std::vector<std::wstring> result;
    CComPtr<IMMDeviceEnumerator> pLocalEnum;
    IMMDeviceEnumerator* pEnum = pEnumIn;
    if (!pEnum) {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&pLocalEnum)))) return result;
        pEnum = pLocalEnum;
    }

    CComPtr<IMMDeviceCollection> pDevices;
    HRESULT hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pDevices);
    if (FAILED(hr)) return result;

    UINT count = 0;
    pDevices->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        CComPtr<IMMDevice> pDevice;
        if (FAILED(pDevices->Item(i, &pDevice))) continue;

        CComPtr<IPropertyStore> pProps;
        if (FAILED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) continue;

        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
            result.emplace_back(varName.pwszVal);
        }
        PropVariantClear(&varName);
    }
    return result;
}

// ---------- Config I/O ----------

static DeviceConfig ParseDeviceConfigLine(const std::wstring& rawLine) {
    std::wstring line = rawLine;
    if (!line.empty() && line.back() == L'\r') line.pop_back();

    DeviceConfig cfg;
    size_t bar = line.find(L'|');
    if (bar == std::wstring::npos) {
        cfg.name = line;
        // Bare lines from old installs: mute all (backwards compat)
        cfg.filter = DeviceConfig::Filter::All;
        return cfg;
    }
    cfg.name = line.substr(0, bar);
    std::wstring spec = line.substr(bar + 1);
    if (spec == L"stream") {
        cfg.filter = DeviceConfig::Filter::StreamOnly;
    } else if (spec.rfind(L"sid:", 0) == 0) {
        cfg.filter = DeviceConfig::Filter::ByFingerprint;
        cfg.fingerprint = spec.substr(4);
    } else if (spec.rfind(L"ord:", 0) == 0) {
        cfg.filter = DeviceConfig::Filter::ByOrdinal;
        try { cfg.ordinal = std::stoi(spec.substr(4)); } catch (...) { cfg.ordinal = -1; }
    }
    return cfg;
}

static std::wstring SerializeDeviceConfig(const DeviceConfig& cfg) {
    std::wstring line = cfg.name;
    switch (cfg.filter) {
        case DeviceConfig::Filter::StreamOnly:
            line += L"|stream";
            break;
        case DeviceConfig::Filter::ByFingerprint:
            if (!cfg.fingerprint.empty()) line += L"|sid:" + cfg.fingerprint;
            break;
        case DeviceConfig::Filter::ByOrdinal:
            if (cfg.ordinal >= 0) line += L"|ord:" + std::to_wstring(cfg.ordinal);
            break;
        default: break;
    }
    return line;
}

std::vector<DeviceConfig> LoadDevicesFromFile(const std::wstring& filename) {
    std::vector<DeviceConfig> result;
    std::wifstream file(filename);
    std::wstring line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        DeviceConfig cfg = ParseDeviceConfigLine(line);
        if (!cfg.name.empty()) result.push_back(std::move(cfg));
    }
    return result;
}

bool SaveDevicesToFile(const std::wstring& filename, const std::vector<DeviceConfig>& devices) {
    std::wstring tmp = filename + L".tmp";
    {
        std::wofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        for (const auto& d : devices) {
            out << SerializeDeviceConfig(d) << L"\n";
        }
        if (!out) return false;
    }
    if (!MoveFileExW(tmp.c_str(), filename.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

int checkToMute() {
    auto devices = LoadDevicesFromFile(L"devices.txt");

    if (devices.empty()) {
        #ifdef DEBUG
        OutputDebugStringA("No devices found in devices.txt\n");
        #endif
        return 1;
    }

    CComPtr<IMMDeviceEnumerator> pEnum;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&pEnum)))) return 1;

    // Compute Discord PIDs per device once; derive "other PIDs" per device from this map.
    auto buildOtherPids = [](const std::wstring& exclude,
                             const std::unordered_map<std::wstring, std::vector<DWORD>>& byDevice) {
        std::vector<DWORD> other;
        for (const auto& kv : byDevice)
            if (kv.first != exclude)
                other.insert(other.end(), kv.second.begin(), kv.second.end());
        return other;
    };

    auto pidsByDevice = BuildDiscordPidsByDevice(pEnum);

    int check = 0;
    for (int i = 0; i < (int)devices.size(); i++) {
        auto otherPids = buildOtherPids(devices[i].name, pidsByDevice);
        check = MuteDiscordOnDevice(devices[i], otherPids, pEnum);
        if (check == 2) {
            i = -1;
            #ifdef DEBUG
            OutputDebugStringA("Sleeping for 5 seconds\n");
            #endif
            Sleep(5000);
            pidsByDevice = BuildDiscordPidsByDevice(pEnum);
        }
    }
    return 0;
}

bool isDiscordRunning() {
    bool found = false;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, L"Discord.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    return found;
}

// ---------- Diagnostic ----------

static std::wstring GuidToWString(const GUID& g) {
    wchar_t buf[64];
    swprintf_s(buf, L"{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

void RunDiagnostic(HWND owner) {
    std::wstringstream ss;
    {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &t);
        wchar_t stamp[64];
        wcsftime(stamp, 64, L"%Y-%m-%d %H:%M:%S", &tm);
        ss << L"Discord session diagnostic - " << stamp << L"\n\n";
    }

    auto configs = LoadDevicesFromFile(L"devices.txt");
    std::vector<std::wstring> deviceNames;
    if (configs.empty()) {
        deviceNames = EnumerateRenderDevices();
    } else {
        for (const auto& c : configs) deviceNames.push_back(c.name);
    }

    // Build PID map once across all devices rather than re-enumerating per device.
    auto pidsByDevice = BuildDiscordPidsByDevice();

    for (const auto& devName : deviceNames) {
        ss << L"Device: " << devName << L"\n";
        auto sessions = EnumerateDiscordSessions(devName);
        if (sessions.empty()) {
            ss << L"  (no Discord sessions found)\n\n";
            continue;
        }
        // PIDs of Discord sessions on *other* devices — shared = main audio, exclusive = stream.
        std::vector<DWORD> otherPids;
        for (const auto& kv : pidsByDevice)
            if (kv.first != devName)
                otherPids.insert(otherPids.end(), kv.second.begin(), kv.second.end());
        for (const auto& s : sessions) {
            bool shared = std::find(otherPids.begin(), otherPids.end(), s.pid) != otherPids.end();
            ss << L"  Session #" << s.ordinalOnDevice << L"\n";
            ss << L"    PID:                       " << s.pid
               << (shared ? L"  (shared - stream candidate)" : L"  (exclusive - keep audible)") << L"\n";
            ss << L"    DisplayName:               " << (s.displayName.empty() ? L"(empty)" : s.displayName) << L"\n";
            ss << L"    IconPath:                  " << (s.iconPath.empty() ? L"(empty)" : s.iconPath) << L"\n";
            ss << L"    SessionIdentifier:         " << s.sessionIdentifier << L"\n";
            ss << L"    StableFingerprint:         " << StableSessionFingerprint(s.sessionIdentifier) << L"\n";
            ss << L"    SessionInstanceIdentifier: " << s.sessionInstanceIdentifier << L"\n";
            ss << L"    GroupingParam:             " << GuidToWString(s.groupingParam) << L"\n";
        }
        ss << L"\n";
    }

    std::wstring logPath = L"discord-sessions.log";
    {
        std::wofstream out(logPath, std::ios::trunc);
        if (!out) {
            MessageBoxW(owner, L"Failed to write discord-sessions.log", L"Diagnostic",
                MB_OK | MB_ICONERROR);
            return;
        }
        out << ss.str();
    }
    ShellExecuteW(owner, L"open", logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------- WMI main / WinMain entry (below) ----------

int main(int argc, char* argv[]) {
    #ifdef DEBUG
    if (argc > 1) {
        for (int i = 0; i < argc; i++) {
            if (strcmp(argv[i], "-d") == 0)
                debugMode = true;
        }
    }
    #endif

    HRESULT hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hres)) return 1;

    hres = CoInitializeSecurity(
        NULL, -1, NULL, NULL,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE, NULL);
    if (FAILED(hres)) {
        CoUninitialize();
        return 1;
    }

    IWbemLocator* pLoc = nullptr;
    hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, (LPVOID*)&pLoc);
    if (FAILED(hres)) { CoUninitialize(); return 1; }

    IWbemServices* pSvc = nullptr;
    hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
    pLoc->Release();
    if (FAILED(hres)) { CoUninitialize(); return 1; }

    hres = CoSetProxyBlanket(
        pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE);
    if (FAILED(hres)) { pSvc->Release(); CoUninitialize(); return 1; }

    IEnumWbemClassObject* pEnumerator = nullptr;
    hres = pSvc->ExecNotificationQuery(
        _bstr_t("WQL"),
        _bstr_t("SELECT * FROM __InstanceCreationEvent WITHIN 1 WHERE TargetInstance ISA 'Win32_Process' AND TargetInstance.Name = 'Discord.exe'"),
        WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY,
        NULL,
        &pEnumerator);
    if (FAILED(hres)) { pSvc->Release(); CoUninitialize(); return 1; }

    if (isDiscordRunning()) {
        #ifdef DEBUG
        OutputDebugStringA("Discord is running.\n");
        #endif
        checkToMute();
    }

    #ifdef DEBUG
    OutputDebugStringA("Listening for Discord.exe startup events...\n");
    #endif

    while (WaitForSingleObject(g_exitEvent, 0) == WAIT_TIMEOUT) {
        IWbemClassObject* pEvent = nullptr;
        ULONG uReturn = 0;
        HRESULT hr = pEnumerator->Next(500, 1, &pEvent, &uReturn);
        if (FAILED(hr) || uReturn == 0) {
            if (pEvent) pEvent->Release();
            continue;
        }

        VARIANT vt;
        VariantInit(&vt);
        hr = pEvent->Get(L"TargetInstance", 0, &vt, 0, 0);

        if (SUCCEEDED(hr) && vt.vt == VT_UNKNOWN && vt.punkVal) {
            CComPtr<IWbemClassObject> pTargetInstance;
            HRESULT qi = vt.punkVal->QueryInterface(IID_IWbemClassObject, (void**)&pTargetInstance);

            if (SUCCEEDED(qi) && pTargetInstance) {
                VARIANT vtName;
                VariantInit(&vtName);
                HRESULT hrName = pTargetInstance->Get(L"Name", 0, &vtName, 0, 0);
                if (SUCCEEDED(hrName) && vtName.vt == VT_BSTR) {
                    if (_wcsicmp(vtName.bstrVal, L"Discord.exe") == 0) {
                        #ifdef DEBUG
                        OutputDebugStringA("Discord.exe just started!\n");
                        #endif
                        checkToMute();
                    }
                }
                VariantClear(&vtName);
            }
            #ifdef DEBUG
            else {
                std::stringstream dss;
                dss << "[Debug] QueryInterface failed, hr=0x" << std::hex << qi << std::endl;
                OutputDebugStringA(dss.str().c_str());
            }
            #endif
        }
        VariantClear(&vt);
        if (pEvent) pEvent->Release();
    }

    pSvc->Release();
    CoUninitialize();
    return 0;
}

// ---------- System tray ----------

DWORD WINAPI WorkerThreadProc(LPVOID) {
    main(__argc, __argv);
    return 0;
}

void AddTrayIcon(HWND hWnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpyA(g_nid.szTip, "MuteDiscordDevice v" MDD_VERSION_STRING);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

void RemoveTrayIcon(HWND) {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

void ShowTrayMenu(HWND hWnd, POINT pt) {
    HMENU hMenu = CreatePopupMenu();
    InsertMenuA(hMenu, -1, MF_BYPOSITION, ID_TRAY_CONFIGURE, "Configure Devices...");
    InsertMenuA(hMenu, -1, MF_BYPOSITION, ID_TRAY_DIAGNOSE,  "Diagnose Discord Sessions...");
    InsertMenuA(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuA(hMenu, -1, MF_BYPOSITION, ID_TRAY_EXIT, "Exit");
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

// ---------- Config dialog ----------

struct ConfigState {
    HWND hList;
    HWND hOk;
    HWND hCancel;
    HWND hSessionBtn;
    HWND hOwner;
};

static std::wstring FilterSummary(const DeviceConfig& cfg) {
    switch (cfg.filter) {
        case DeviceConfig::Filter::StreamOnly:
            return L"Stream only (auto)";
        case DeviceConfig::Filter::All:
            return L"All Discord sessions";
        case DeviceConfig::Filter::ByFingerprint: {
            std::wstring s = L"Specific: ";
            s += (cfg.fingerprint.size() > 20 ? cfg.fingerprint.substr(0, 20) + L"..." : cfg.fingerprint);
            return s;
        }
        case DeviceConfig::Filter::ByOrdinal:
            return L"Ordinal #" + std::to_wstring(cfg.ordinal);
    }
    return L"";
}

static void DeleteRowConfigs(HWND hList) {
    int count = ListView_GetItemCount(hList);
    for (int i = 0; i < count; i++) {
        LVITEMW item = { 0 };
        item.iItem = i;
        item.mask = LVIF_PARAM;
        if (ListView_GetItem(hList, &item) && item.lParam) {
            delete reinterpret_cast<DeviceConfig*>(item.lParam);
        }
    }
}

static void PopulateConfigList(HWND hList) {
    DeleteRowConfigs(hList);
    ListView_DeleteAllItems(hList);

    std::vector<std::wstring> enumerated = EnumerateRenderDevices();
    std::vector<DeviceConfig> configured = LoadDevicesFromFile(L"devices.txt");

    auto findConfigured = [&](const std::wstring& name) -> DeviceConfig* {
        for (auto& c : configured) if (c.name == name) return &c;
        return nullptr;
    };

    int row = 0;
    for (const auto& name : enumerated) {
        DeviceConfig* existing = findConfigured(name);
        DeviceConfig* rowCfg = new DeviceConfig();
        if (existing) *rowCfg = *existing;
        else rowCfg->name = name;
        rowCfg->offline = false;

        LVITEMW item = { 0 };
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(name.c_str());
        item.lParam = reinterpret_cast<LPARAM>(rowCfg);
        ListView_InsertItem(hList, &item);

        std::wstring summary = FilterSummary(*rowCfg);
        ListView_SetItemText(hList, row, 1, const_cast<LPWSTR>(summary.c_str()));

        ListView_SetCheckState(hList, row, existing != nullptr);
        row++;
    }

    for (const auto& c : configured) {
        if (std::find(enumerated.begin(), enumerated.end(), c.name) != enumerated.end()) continue;
        DeviceConfig* rowCfg = new DeviceConfig(c);
        rowCfg->offline = true;
        std::wstring display = c.name + L" (offline)";

        LVITEMW item = { 0 };
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(display.c_str());
        item.lParam = reinterpret_cast<LPARAM>(rowCfg);
        ListView_InsertItem(hList, &item);

        std::wstring summary = FilterSummary(*rowCfg);
        ListView_SetItemText(hList, row, 1, const_cast<LPWSTR>(summary.c_str()));

        ListView_SetCheckState(hList, row, TRUE);
        row++;
    }
}

static std::vector<DeviceConfig> CollectCheckedConfigs(HWND hList) {
    std::vector<DeviceConfig> result;
    int count = ListView_GetItemCount(hList);
    for (int i = 0; i < count; i++) {
        if (!ListView_GetCheckState(hList, i)) continue;
        LVITEMW item = { 0 };
        item.iItem = i;
        item.mask = LVIF_PARAM;
        ListView_GetItem(hList, &item);
        if (!item.lParam) continue;
        DeviceConfig* cfg = reinterpret_cast<DeviceConfig*>(item.lParam);
        result.push_back(*cfg);
    }
    return result;
}

static DeviceConfig* GetRowConfig(HWND hList, int row) {
    LVITEMW item = { 0 };
    item.iItem = row;
    item.mask = LVIF_PARAM;
    if (!ListView_GetItem(hList, &item)) return nullptr;
    return reinterpret_cast<DeviceConfig*>(item.lParam);
}

// ---------- Session picker ----------

struct SessionPickerState {
    HWND hOwner;
    std::wstring deviceName;
    std::vector<DiscordSession> sessions;
    DeviceConfig* ioCfg;
    bool accepted = false;

    HWND hStreamRadio = nullptr;
    HWND hAllRadio = nullptr;
    std::vector<HWND> hSessionRadios;
    std::vector<HWND> hTestBtns;
    HWND hOrdinalRadio = nullptr;
    HWND hOrdinalEdit = nullptr;
    HWND hOk = nullptr;
    HWND hCancel = nullptr;
};

static void ToggleSessionMute(DiscordSession& s) {
    CComQIPtr<ISimpleAudioVolume> pVol(s.pControl);
    if (!pVol) return;
    BOOL currentlyMuted = FALSE;
    pVol->GetMute(&currentlyMuted);
    pVol->SetMute(currentlyMuted ? FALSE : TRUE, nullptr);
}

LRESULT CALLBACK SessionPickerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SessionPickerState* st = reinterpret_cast<SessionPickerState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SessionPickerState* s = reinterpret_cast<SessionPickerState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        int y = 10;

        std::wstring header = L"Device: " + s->deviceName;
        HWND hHeader = CreateWindowW(L"STATIC", header.c_str(),
            WS_CHILD | WS_VISIBLE,
            10, y, 660, 20, hWnd, NULL, g_hInst, NULL);
        SendMessageW(hHeader, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 26;

        s->hStreamRadio = CreateWindowW(L"BUTTON",
            L"Stream only (auto-detect) — mute Discord child-process sessions, leave main audio alone  [Recommended]",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            10, y, 660, 20, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_STREAM, g_hInst, NULL);
        SendMessageW(s->hStreamRadio, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 26;

        s->hAllRadio = CreateWindowW(L"BUTTON", L"Mute all Discord sessions (original behavior)",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            10, y, 660, 20, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_ALL, g_hInst, NULL);
        SendMessageW(s->hAllRadio, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 26;

        if (s->sessions.empty()) {
            HWND hInfo = CreateWindowW(L"STATIC",
                L"No Discord sessions found on this device.\n"
                L"Start Discord (and your stream if applicable) and reopen this picker.",
                WS_CHILD | WS_VISIBLE,
                30, y, 640, 40, hWnd, NULL, g_hInst, NULL);
            SendMessageW(hInfo, WM_SETFONT, (WPARAM)hFont, TRUE);
            y += 46;
        }

        for (size_t i = 0; i < s->sessions.size(); i++) {
            const auto& sess = s->sessions[i];
            std::wstring label = L"Session #" + std::to_wstring(sess.ordinalOnDevice) +
                L"  -  " + (sess.displayName.empty() ? L"(no name)" : sess.displayName) +
                L"  [PID " + std::to_wstring(sess.pid) + L"]";
            std::wstring fp = StableSessionFingerprint(sess.sessionIdentifier);
            if (!fp.empty()) {
                std::wstring shortFp = fp.size() > 40 ? fp.substr(0, 40) + L"..." : fp;
                label += L"\n    fp: " + shortFp;
            }

            HWND hRadio = CreateWindowW(L"BUTTON", label.c_str(),
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_MULTILINE,
                10, y, 560, 40,
                hWnd, (HMENU)(UINT_PTR)(IDC_SESSION_FIRST_RADIO + i), g_hInst, NULL);
            SendMessageW(hRadio, WM_SETFONT, (WPARAM)hFont, TRUE);
            s->hSessionRadios.push_back(hRadio);

            HWND hTest = CreateWindowW(L"BUTTON", L"Toggle mute",
                WS_CHILD | WS_VISIBLE,
                580, y + 8, 90, 24,
                hWnd, (HMENU)(UINT_PTR)(IDC_SESSION_FIRST_TESTBTN + i), g_hInst, NULL);
            SendMessageW(hTest, WM_SETFONT, (WPARAM)hFont, TRUE);
            s->hTestBtns.push_back(hTest);

            y += 46;
        }

        s->hOrdinalRadio = CreateWindowW(L"BUTTON", L"Ordinal fallback: mute the",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            10, y, 200, 22, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_ORDINAL_RADIO, g_hInst, NULL);
        SendMessageW(s->hOrdinalRadio, WM_SETFONT, (WPARAM)hFont, TRUE);

        s->hOrdinalEdit = CreateWindowW(L"EDIT", L"1",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            215, y, 40, 22, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_ORDINAL_EDIT, g_hInst, NULL);
        SendMessageW(s->hOrdinalEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hTail = CreateWindowW(L"STATIC", L"th Discord session (0-indexed)",
            WS_CHILD | WS_VISIBLE,
            260, y + 2, 250, 22, hWnd, NULL, g_hInst, NULL);
        SendMessageW(hTail, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 34;

        s->hOk = CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            490, y, 80, 28, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_OK, g_hInst, NULL);
        s->hCancel = CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE,
            580, y, 80, 28, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_CANCEL, g_hInst, NULL);
        SendMessageW(s->hOk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(s->hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Initialize selection from ioCfg
        if (s->ioCfg) {
            switch (s->ioCfg->filter) {
            case DeviceConfig::Filter::StreamOnly:
                SendMessageW(s->hStreamRadio, BM_SETCHECK, BST_CHECKED, 0);
                break;
            case DeviceConfig::Filter::All:
                SendMessageW(s->hAllRadio, BM_SETCHECK, BST_CHECKED, 0);
                break;
            case DeviceConfig::Filter::ByFingerprint: {
                bool found = false;
                for (size_t i = 0; i < s->sessions.size(); i++) {
                    if (StableSessionFingerprint(s->sessions[i].sessionIdentifier) == s->ioCfg->fingerprint) {
                        SendMessageW(s->hSessionRadios[i], BM_SETCHECK, BST_CHECKED, 0);
                        found = true;
                        break;
                    }
                }
                if (!found) SendMessageW(s->hStreamRadio, BM_SETCHECK, BST_CHECKED, 0);
                break;
            }
            case DeviceConfig::Filter::ByOrdinal:
                SendMessageW(s->hOrdinalRadio, BM_SETCHECK, BST_CHECKED, 0);
                if (s->ioCfg->ordinal >= 0) {
                    std::wstring v = std::to_wstring(s->ioCfg->ordinal);
                    SetWindowTextW(s->hOrdinalEdit, v.c_str());
                }
                break;
            }
        } else {
            SendMessageW(s->hStreamRadio, BM_SETCHECK, BST_CHECKED, 0);
        }

        if (s->hOwner) EnableWindow(s->hOwner, FALSE);
        return 0;
    }
    case WM_COMMAND: {
        if (!st) break;
        WORD id = LOWORD(wParam);
        if (id == IDC_SESSION_OK) {
            if (st->ioCfg) {
                if (SendMessageW(st->hStreamRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    st->ioCfg->filter = DeviceConfig::Filter::StreamOnly;
                    st->ioCfg->fingerprint.clear();
                    st->ioCfg->ordinal = -1;
                } else if (SendMessageW(st->hAllRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    st->ioCfg->filter = DeviceConfig::Filter::All;
                    st->ioCfg->fingerprint.clear();
                    st->ioCfg->ordinal = -1;
                } else if (SendMessageW(st->hOrdinalRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    wchar_t buf[32] = { 0 };
                    GetWindowTextW(st->hOrdinalEdit, buf, 32);
                    int ord = -1;
                    try { ord = std::stoi(buf); } catch (...) {}
                    if (ord < 0) {
                        MessageBoxW(hWnd, L"Ordinal must be a non-negative integer.", L"Session picker",
                            MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    st->ioCfg->filter = DeviceConfig::Filter::ByOrdinal;
                    st->ioCfg->ordinal = ord;
                    st->ioCfg->fingerprint.clear();
                } else {
                    int picked = -1;
                    for (size_t i = 0; i < st->hSessionRadios.size(); i++) {
                        if (SendMessageW(st->hSessionRadios[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
                            picked = (int)i;
                            break;
                        }
                    }
                    if (picked < 0) {
                        MessageBoxW(hWnd, L"Pick a session or one of the other options.", L"Session picker",
                            MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    std::wstring fp = StableSessionFingerprint(st->sessions[picked].sessionIdentifier);
                    if (fp.empty()) {
                        MessageBoxW(hWnd,
                            L"That session has no usable fingerprint. Use the ordinal option instead.",
                            L"Session picker", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    st->ioCfg->filter = DeviceConfig::Filter::ByFingerprint;
                    st->ioCfg->fingerprint = fp;
                    st->ioCfg->ordinal = -1;
                }
                st->accepted = true;
            }
            DestroyWindow(hWnd);
            return 0;
        }
        if (id == IDC_SESSION_CANCEL) {
            DestroyWindow(hWnd);
            return 0;
        }
        if (id >= IDC_SESSION_FIRST_TESTBTN &&
            id < IDC_SESSION_FIRST_TESTBTN + (int)st->sessions.size()) {
            int idx = id - IDC_SESSION_FIRST_TESTBTN;
            ToggleSessionMute(st->sessions[idx]);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        if (st && st->hOwner) {
            EnableWindow(st->hOwner, TRUE);
            SetForegroundWindow(st->hOwner);
        }
        PostMessage(hWnd, WM_NULL, 0, 0); // kick the inner loop
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void RegisterSessionPickerClassOnce() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = SessionPickerWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"MuteDiscordSessionPickerClass";
    RegisterClassW(&wc);
    registered = true;
}

bool ShowSessionPicker(HWND owner, const std::wstring& deviceName, DeviceConfig& ioCfg) {
    RegisterSessionPickerClassOnce();

    SessionPickerState state;
    state.hOwner = owner;
    state.deviceName = deviceName;
    state.sessions = EnumerateDiscordSessions(deviceName);
    state.ioCfg = &ioCfg;

    int width = 700;
    int height = 180 + (int)state.sessions.size() * 46 + (state.sessions.empty() ? 60 : 0);
    if (height < 260) height = 260;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    HWND hWnd = CreateWindowExW(WS_EX_DLGMODALFRAME,
        L"MuteDiscordSessionPickerClass",
        L"Pick Discord session to mute",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, width, height,
        owner, NULL, g_hInst, &state);
    if (!hWnd) return false;
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG msg;
    while (IsWindow(hWnd)) {
        BOOL r = GetMessageW(&msg, NULL, 0, 0);
        if (r == 0 || r == -1) break;
        if (IsDialogMessageW(hWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return state.accepted;
}

// ---------- Config dialog window proc ----------

LRESULT CALLBACK ConfigWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ConfigState* state = reinterpret_cast<ConfigState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ConfigState* s = new ConfigState{ 0 };
        s->hOwner = reinterpret_cast<HWND>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));

        RECT rc;
        GetClientRect(hWnd, &rc);

        s->hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            10, 10, rc.right - 20, rc.bottom - 60,
            hWnd, (HMENU)IDC_CONFIG_LIST, g_hInst, NULL);
        ListView_SetExtendedListViewStyle(s->hList,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);

        LVCOLUMNW col0 = { 0 };
        col0.mask = LVCF_TEXT | LVCF_WIDTH;
        col0.pszText = const_cast<LPWSTR>(L"Device");
        col0.cx = (rc.right - 50) * 2 / 3;
        ListView_InsertColumn(s->hList, 0, &col0);

        LVCOLUMNW col1 = { 0 };
        col1.mask = LVCF_TEXT | LVCF_WIDTH;
        col1.pszText = const_cast<LPWSTR>(L"Discord session");
        col1.cx = (rc.right - 50) / 3;
        ListView_InsertColumn(s->hList, 1, &col1);

        s->hSessionBtn = CreateWindowW(L"BUTTON", L"Session...",
            WS_CHILD | WS_VISIBLE,
            10, rc.bottom - 40, 100, 28,
            hWnd, (HMENU)IDC_CONFIG_SESSION_BTN, g_hInst, NULL);
        s->hOk = CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            rc.right - 180, rc.bottom - 40, 80, 28,
            hWnd, (HMENU)IDC_CONFIG_OK, g_hInst, NULL);
        s->hCancel = CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE,
            rc.right - 90, rc.bottom - 40, 80, 28,
            hWnd, (HMENU)IDC_CONFIG_CANCEL, g_hInst, NULL);

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(s->hList, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(s->hSessionBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(s->hOk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(s->hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        PopulateConfigList(s->hList);

        if (s->hOwner) EnableWindow(s->hOwner, FALSE);
        return 0;
    }
    case WM_SIZE: {
        if (!state) break;
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        SetWindowPos(state->hList, NULL, 10, 10, w - 20, h - 60, SWP_NOZORDER);
        SetWindowPos(state->hSessionBtn, NULL, 10, h - 40, 100, 28, SWP_NOZORDER);
        SetWindowPos(state->hOk, NULL, w - 180, h - 40, 80, 28, SWP_NOZORDER);
        SetWindowPos(state->hCancel, NULL, w - 90, h - 40, 80, 28, SWP_NOZORDER);
        ListView_SetColumnWidth(state->hList, 0, (w - 50) * 2 / 3);
        ListView_SetColumnWidth(state->hList, 1, (w - 50) / 3);
        return 0;
    }
    case WM_COMMAND: {
        if (!state) break;
        WORD id = LOWORD(wParam);
        if (id == IDC_CONFIG_OK) {
            std::vector<DeviceConfig> checked = CollectCheckedConfigs(state->hList);
            if (!SaveDevicesToFile(L"devices.txt", checked)) {
                MessageBoxW(hWnd, L"Failed to save devices.txt", L"Error",
                    MB_OK | MB_ICONERROR);
                return 0;
            }
            DestroyWindow(hWnd);
            if (isDiscordRunning()) {
                checkToMute();
            }
            return 0;
        }
        if (id == IDC_CONFIG_CANCEL) {
            DestroyWindow(hWnd);
            return 0;
        }
        if (id == IDC_CONFIG_SESSION_BTN) {
            int sel = ListView_GetNextItem(state->hList, -1, LVNI_SELECTED);
            if (sel < 0) {
                MessageBoxW(hWnd, L"Select a device row first.", L"Session picker",
                    MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            DeviceConfig* cfg = GetRowConfig(state->hList, sel);
            if (!cfg) return 0;
            if (cfg->offline) {
                MessageBoxW(hWnd,
                    L"This device is offline. Plug it in / enable it to pick sessions.",
                    L"Session picker", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (ShowSessionPicker(hWnd, cfg->name, *cfg)) {
                std::wstring summary = FilterSummary(*cfg);
                ListView_SetItemText(state->hList, sel, 1, const_cast<LPWSTR>(summary.c_str()));
                ListView_SetCheckState(state->hList, sel, TRUE);
            }
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        if (state) {
            DeleteRowConfigs(state->hList);
            if (state->hOwner) {
                EnableWindow(state->hOwner, TRUE);
                SetForegroundWindow(state->hOwner);
            }
            delete state;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        g_hConfigWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ShowConfigDialog(HWND owner) {
    if (g_hConfigWnd && IsWindow(g_hConfigWnd)) {
        SetForegroundWindow(g_hConfigWnd);
        return;
    }

    int width = 700;
    int height = 480;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    g_hConfigWnd = CreateWindowExW(0, L"MuteDiscordConfigClass",
        L"Configure Devices to Mute",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_THICKFRAME,
        x, y, width, height,
        owner, NULL, g_hInst, owner);
    if (g_hConfigWnd) {
        ShowWindow(g_hConfigWnd, SW_SHOW);
        UpdateWindow(g_hConfigWnd);
    }
}

LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            ShowTrayMenu(hWnd, pt);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_EXIT) {
            SetEvent(g_exitEvent);
            DestroyWindow(hWnd);
        }
        else if (LOWORD(wParam) == ID_TRAY_CONFIGURE) {
            ShowConfigDialog(hWnd);
        }
        else if (LOWORD(wParam) == ID_TRAY_DIAGNOSE) {
            RunDiagnostic(hWnd);
        }
        break;
    case WM_DESTROY:
        RemoveTrayIcon(hWnd);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    g_hInst = hInstance;
    g_exitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "MuteDiscordTrayClass";
    RegisterClassA(&wc);

    WNDCLASSW cwc = { 0 };
    cwc.lpfnWndProc = ConfigWndProc;
    cwc.hInstance = hInstance;
    cwc.hCursor = LoadCursor(NULL, IDC_ARROW);
    cwc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    cwc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    cwc.lpszClassName = L"MuteDiscordConfigClass";
    RegisterClassW(&cwc);

    g_hWnd = CreateWindowA(wc.lpszClassName, "", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    AddTrayIcon(g_hWnd);

    g_workerThread = CreateThread(NULL, 0, WorkerThreadProc, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (g_hConfigWnd && IsWindow(g_hConfigWnd) && IsDialogMessageW(g_hConfigWnd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    WaitForSingleObject(g_workerThread, INFINITE);
    CloseHandle(g_workerThread);
    CloseHandle(g_exitEvent);

    CoUninitialize();
    return 0;
}
