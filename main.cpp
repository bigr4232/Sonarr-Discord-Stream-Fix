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
#include "resource.h"
#include "mdd_pure.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "psapi.lib")

// HSTRING is defined in <winstring.h>; guard against redefinition
#if !defined(HSTRING_DEFINED)
typedef struct HSTRING__* HSTRING;
#define HSTRING_DEFINED
#endif

// Per-app audio routing factory (Windows 10 1803+, Windows 11).
// The activation-factory IID and method indexes differ on newer builds.
static const GUID IID_IAudioPolicyConfigFactory_Downlevel =
    { 0x2a59116d, 0x6c4f, 0x45e0, { 0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58 } };
static const GUID IID_IAudioPolicyConfigFactory_21H2 =
    { 0xab3d4648, 0xe242, 0x459f, { 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24 } };

static const int POLICY_CONFIG_INDEX_RELEASE = 2;
static const int POLICY_CONFIG_INDEX_SET_PERSISTED_DEFAULT_ENDPOINT = 25;
static const int POLICY_CONFIG_INDEX_GET_PERSISTED_DEFAULT_ENDPOINT = 26;
static const DWORD BUILD_WINDOWS_10_1803 = 17134;
static const DWORD BUILD_WINDOWS_10_21H2_IID_SWITCH = 21390;
static const wchar_t* MMDEVAPI_TOKEN = L"\\\\?\\SWD#MMDEVAPI#";
static const wchar_t* POLICY_RENDER_INTERFACE_SUFFIX = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
static const wchar_t* POLICY_CAPTURE_INTERFACE_SUFFIX = L"#{2eef81be-33fa-4800-9670-1cd474972c3f}";

typedef HRESULT (WINAPI *FnRoInitialize)(UINT type);
typedef void    (WINAPI *FnRoUninitialize)();
typedef HRESULT (WINAPI *FnRoGetActivationFactory)(HSTRING classId, REFIID iid, void** factory);
typedef HRESULT (WINAPI *FnWindowsCreateString)(LPCWSTR src, UINT32 len, HSTRING* out);
typedef HRESULT (WINAPI *FnWindowsDeleteString)(HSTRING str);
typedef PCWSTR  (WINAPI *FnWindowsGetStringRawBuffer)(HSTRING str, UINT32* len);

#define WM_TRAYICON (WM_USER + 1)
static UINT WM_TASKBARCREATED = 0;
static UINT WM_REQUEST_SHUTDOWN = 0;
#define ID_TRAY_EXIT 1001
#define ID_TRAY_CONFIGURE 1002
#define ID_TRAY_DIAGNOSE 1003
#define ID_TRAY_ROUTETARGET 1004

#define IDC_ROUTE_COMBO  4001
#define IDC_ROUTE_OK     4002
#define IDC_ROUTE_CANCEL 4003

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

static const wchar_t* kShutdownMessageName = L"MuteDiscordDevice_RequestShutdown";

static bool ShouldDisableOwnerWindow(HWND owner) {
    return owner && IsWindow(owner) && IsWindowVisible(owner);
}

static void RestoreOwnerWindow(HWND owner) {
    if (!ShouldDisableOwnerWindow(owner)) return;
    EnableWindow(owner, TRUE);
    if (IsIconic(owner)) ShowWindow(owner, SW_RESTORE);
    SetForegroundWindow(owner);
}

static bool RequestRunningInstanceShutdown() {
    UINT shutdownMessage = WM_REQUEST_SHUTDOWN;
    if (!shutdownMessage) {
        shutdownMessage = RegisterWindowMessageW(kShutdownMessageName);
    }
    if (!shutdownMessage) return false;

    bool signaled = false;

    HWND hTray = FindWindowA("MuteDiscordTrayClass", nullptr);
    while (hTray) {
        PostMessageW(hTray, shutdownMessage, 0, 0);
        signaled = true;
        hTray = FindWindowExA(nullptr, hTray, "MuteDiscordTrayClass", nullptr);
    }

    HWND hConfig = FindWindowW(L"MuteDiscordConfigClass", nullptr);
    while (hConfig) {
        PostMessageW(hConfig, WM_CLOSE, 0, 0);
        signaled = true;
        hConfig = FindWindowExW(nullptr, hConfig, L"MuteDiscordConfigClass", nullptr);
    }

    HWND hRoute = FindWindowW(L"MuteDiscordRouteTargetClass", nullptr);
    while (hRoute) {
        PostMessageW(hRoute, WM_CLOSE, 0, 0);
        signaled = true;
        hRoute = FindWindowExW(nullptr, hRoute, L"MuteDiscordRouteTargetClass", nullptr);
    }

    return signaled;
}

static void SetWorkingDirectoryToExeFolder() {
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) return;
    *slash = L'\0';
    SetCurrentDirectoryW(exePath);
}

static bool HasCommandLineArg(int argc, char* argv[], const char* arg) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], arg) == 0) {
            return true;
        }
    }
    return false;
}

// HResultToWString moved to mdd_pure.cpp

static void AppendRouteLog(const std::wstring& line) {
    std::wofstream out(L"route-debug.log", std::ios::app);
    if (!out) return;
    out << line << L"\n";
}

static DWORD GetWindowsBuildNumber() {
    typedef LONG (WINAPI *FnRtlGetVersion)(PRTL_OSVERSIONINFOW);

    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) return 0;

    auto fpRtlGetVersion = reinterpret_cast<FnRtlGetVersion>(GetProcAddress(hNtdll, "RtlGetVersion"));
    if (!fpRtlGetVersion) return 0;

    RTL_OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fpRtlGetVersion(&vi) != 0) return 0;
    return vi.dwBuildNumber;
}

static GUID ResolvePolicyConfigFactoryIid() {
    DWORD build = GetWindowsBuildNumber();
    if (build >= BUILD_WINDOWS_10_21H2_IID_SWITCH) {
        return IID_IAudioPolicyConfigFactory_21H2;
    }
    return IID_IAudioPolicyConfigFactory_Downlevel;
}

static std::wstring PackPolicyConfigDeviceId(const std::wstring& deviceId, EDataFlow flow) {
    if (deviceId.empty()) return L"";
    return std::wstring(MMDEVAPI_TOKEN) + deviceId +
        (flow == eCapture ? POLICY_CAPTURE_INTERFACE_SUFFIX : POLICY_RENDER_INTERFACE_SUFFIX);
}

// ---------- Data model ----------
// DeviceConfig is now defined in mdd_pure.h (included above).

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
void ShowRouteTargetDialog(HWND owner);
std::vector<std::wstring> EnumerateRenderDevices(IMMDeviceEnumerator* pEnum = nullptr);
std::wstring LoadRouteTargetFromFile();
bool SaveRouteTargetToFile(const std::wstring& deviceName);
static void RouteDiscordAudioOutput(const std::wstring& targetDeviceFriendlyName, IMMDeviceEnumerator* pEnum);
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

std::wstring GetProcessImagePath(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return L"";
    wchar_t buf[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProc, 0, buf, &size);
    CloseHandle(hProc);
    if (!ok) return L"";
    return buf;
}

// ToLowerCopy moved to mdd_pure.cpp

static void RemoveDiscordPersistedRenderEndpoints(const std::vector<std::wstring>& processPaths) {
    if (processPaths.empty()) return;

    std::vector<std::wstring> loweredPaths;
    loweredPaths.reserve(processPaths.size() * 2);
    for (const auto& path : processPaths) {
        if (path.empty()) continue;
        std::wstring lowerPath = ToLowerCopy(path);
        loweredPaths.push_back(lowerPath);

        // PolicyConfig stores executable paths in NT-style form
        // (\Device\HarddiskVolumeX\Users\...) rather than DOS form
        // (C:\Users\...). Matching the path suffix lets us identify the
        // current Discord build regardless of the volume prefix.
        if (lowerPath.size() > 3 && lowerPath[1] == L':' && lowerPath[2] == L'\\') {
            loweredPaths.push_back(lowerPath.substr(2));
        }
    }
    if (loweredPaths.empty()) return;

    HKEY hKey = nullptr;
    const wchar_t* subkey = L"Software\\Microsoft\\Internet Explorer\\LowRegistry\\Audio\\PolicyConfig\\PropertyStore";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0,
        KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE | DELETE, &hKey) != ERROR_SUCCESS) {
        return;
    }

    std::vector<std::wstring> keysToDelete;
    DWORD index = 0;
    for (;;) {
        wchar_t keyName[256] = {};
        DWORD keyNameLen = ARRAYSIZE(keyName);
        LONG enumResult = RegEnumKeyExW(hKey, index, keyName, &keyNameLen, nullptr, nullptr, nullptr, nullptr);
        if (enumResult == ERROR_NO_MORE_ITEMS) break;
        if (enumResult != ERROR_SUCCESS) {
            index++;
            continue;
        }

        wchar_t defaultValue[2048] = {};
        DWORD valueType = 0;
        DWORD valueSize = sizeof(defaultValue);
        LONG queryResult = RegGetValueW(hKey, keyName, nullptr, RRF_RT_REG_SZ, &valueType, defaultValue, &valueSize);
        if (queryResult == ERROR_SUCCESS) {
            std::wstring defaultLower = ToLowerCopy(defaultValue);
            bool matchesDiscordPath = false;
            for (const auto& path : loweredPaths) {
                if (defaultLower.find(path) != std::wstring::npos) {
                    matchesDiscordPath = true;
                    break;
                }
            }

            // Preserve capture/mic routes; we only want to clear stale render outputs.
            if (matchesDiscordPath &&
                defaultLower.find(L"capture") == std::wstring::npos &&
                defaultLower.find(L"microphone") == std::wstring::npos) {
                keysToDelete.push_back(keyName);
            }
        }
        index++;
    }

    for (const auto& keyName : keysToDelete) {
        RegDeleteTreeW(hKey, keyName.c_str());
    }

    RegCloseKey(hKey);
}

// StableSessionFingerprint moved to mdd_pure.cpp

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

    if (cfg.filter == DeviceConfig::Filter::StreamOnly) {
        // Stream audio only appears on this device while a Discord stream is live:
        // Discord then renders two sessions here (main voice + stream output).
        // Fewer than two means the stream hasn't started, and muting the lone
        // session would silence chat. Un-mute anything a prior pass muted so we
        // self-heal when a stream ends.
        if (sessions.size() < 2) {
            for (auto& s : sessions) {
                CComQIPtr<ISimpleAudioVolume> pVol(s.pControl);
                BOOL isMuted = FALSE;
                if (pVol && SUCCEEDED(pVol->GetMute(&isMuted)) && isMuted)
                    pVol->SetMute(FALSE, nullptr);
            }
            #ifdef DEBUG
            OutputDebugStringA("StreamOnly: <2 sessions on device; stream not live, deferring.\n");
            #endif
            return 2;
        }
        if (otherPids.empty()) {
            #ifdef DEBUG
            OutputDebugStringA("StreamOnly: no Discord sessions on other devices yet; deferring.\n");
            #endif
            return 2;
        }
        // Ambiguity guard: during reboots, Discord may transiently route every
        // child process through multiple devices, making ALL sessions look shared.
        // Only proceed when the heuristic picks exactly one — otherwise defer.
        int sharedCount = 0;
        for (auto& s : sessions)
            if (std::find(otherPids.begin(), otherPids.end(), s.pid) != otherPids.end())
                sharedCount++;
        if (sharedCount != 1) {
            #ifdef DEBUG
            OutputDebugStringA("StreamOnly: ambiguous (not exactly one shared session); deferring.\n");
            #endif
            return 2;
        }
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
        CComQIPtr<ISimpleAudioVolume> pVol(s.pControl);
        if (!matches) {
            // Under StreamOnly, we own the mute state — un-mute non-matching sessions
            // so a prior mis-mute (e.g. from a boot-race) self-heals on the next pass.
            if (cfg.filter == DeviceConfig::Filter::StreamOnly && pVol) {
                BOOL isMuted = FALSE;
                if (SUCCEEDED(pVol->GetMute(&isMuted)) && isMuted) {
                    pVol->SetMute(FALSE, nullptr);
                    #ifdef DEBUG
                    OutputDebugStringA("StreamOnly: un-muted a non-matching session (self-heal).\n");
                    #endif
                }
            }
            continue;
        }
        anyFilterMatched = true;
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

static void UnmuteDiscordSessionsOnDevice(const std::wstring& deviceName, IMMDeviceEnumerator* pEnum = nullptr) {
    auto sessions = EnumerateDiscordSessions(deviceName, pEnum);
    for (auto& s : sessions) {
        CComQIPtr<ISimpleAudioVolume> pVol(s.pControl);
        if (!pVol) continue;

        BOOL isMuted = FALSE;
        if (SUCCEEDED(pVol->GetMute(&isMuted)) && isMuted) {
            pVol->SetMute(FALSE, nullptr);
        }
    }
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

// ParseDeviceConfigLine and SerializeDeviceConfig moved to mdd_pure.cpp

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

// NameMatchesDefaultMute moved to mdd_pure.cpp

// First-run only: seed devices.txt with sensible defaults (Sonar mic + Arctis
// Nova headphones). Once the file exists we never touch it again, so any user
// edits — including unchecking these defaults — are preserved across launches.
static void SeedDefaultDevicesIfFirstRun() {
    auto devices = EnumerateRenderDevices();

    // Seed devices.txt with default mute targets (Sonar mic / Arctis Nova)
    if (GetFileAttributesW(L"devices.txt") == INVALID_FILE_ATTRIBUTES) {
        std::vector<DeviceConfig> defaults;
        for (const auto& name : devices) {
            if (!NameMatchesDefaultMute(name)) continue;
            DeviceConfig cfg;
            cfg.name = name;
            cfg.filter = DeviceConfig::Filter::All;
            defaults.push_back(std::move(cfg));
        }
        SaveDevicesToFile(L"devices.txt", defaults);
    }

    // Seed route_target.txt with the SteelSeries Sonar Chat device
    if (GetFileAttributesW(L"route_target.txt") == INVALID_FILE_ATTRIBUTES) {
        for (const auto& name : devices) {
            std::wstring lower = name;
            for (auto& c : lower) c = towlower(c);
            if (lower.find(L"sonar") != std::wstring::npos &&
                lower.find(L"chat") != std::wstring::npos) {
                SaveRouteTargetToFile(name);
                break;
            }
        }
        // If no Sonar Chat found, write empty file so we don't re-seed every launch
        if (GetFileAttributesW(L"route_target.txt") == INVALID_FILE_ATTRIBUTES)
            SaveRouteTargetToFile(L"");
    }
}

int checkToMute() {
    CComPtr<IMMDeviceEnumerator> pEnum;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&pEnum)))) return 1;

    // Route all Discord audio to the configured target device (primary feature)
    std::wstring routeTarget = LoadRouteTargetFromFile();
    if (!routeTarget.empty()) {
        RouteDiscordAudioOutput(routeTarget, pEnum);
    }

    // Mute Discord on configured devices (secondary/legacy feature)
    auto devices = LoadDevicesFromFile(L"devices.txt");
    if (devices.empty()) return 0;

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

    // Retry budget: when a device returns 2 (no matching session yet), we wait and
    // re-scan from the beginning. Limit total retries to avoid blocking the worker
    // thread indefinitely, and check g_exitEvent each wait so --shutdown still works.
    static const int MAX_RETRIES = 5;
    static const DWORD WAIT_MS = 1000;
    int retriesLeft = MAX_RETRIES;

    int check = 0;
    for (int i = 0; i < (int)devices.size(); i++) {
        // Check for shutdown request before processing each device
        if (g_exitEvent && WaitForSingleObject(g_exitEvent, 0) == WAIT_OBJECT_0) {
            #ifdef DEBUG
            OutputDebugStringA("checkToMute: shutdown requested, exiting early.\n");
            #endif
            return 0;
        }

        auto otherPids = buildOtherPids(devices[i].name, pidsByDevice);
        check = MuteDiscordOnDevice(devices[i], otherPids, pEnum);
        if (check == 2) {
            if (retriesLeft <= 0) {
                #ifdef DEBUG
                OutputDebugStringA("checkToMute: retry budget exhausted, giving up.\n");
                #endif
                break;
            }
            retriesLeft--;
            #ifdef DEBUG
            std::wstringstream dss;
            dss << L"checkToMute: device returned retry, " << retriesLeft << L" retries left.\n";
            OutputDebugStringW(dss.str().c_str());
            #endif
            // Wait with interruptibility so shutdown requests are not blocked.
            if (g_exitEvent && WaitForSingleObject(g_exitEvent, WAIT_MS) == WAIT_OBJECT_0) {
                #ifdef DEBUG
                OutputDebugStringA("checkToMute: shutdown requested during wait, exiting.\n");
                #endif
                return 0;
            }
            i = -1;
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

// ---------- Route-to-device config I/O ----------

std::wstring LoadRouteTargetFromFile() {
    std::wifstream file(L"route_target.txt");
    if (!file.is_open()) return L"";
    std::wstring line;
    if (std::getline(file, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        return line;
    }
    return L"";
}

bool SaveRouteTargetToFile(const std::wstring& deviceName) {
    const std::wstring tmp = L"route_target.txt.tmp";
    {
        std::wofstream f(tmp);
        if (!f.is_open()) return false;
        f << deviceName << L"\n";
    }
    return MoveFileExW(tmp.c_str(), L"route_target.txt", MOVEFILE_REPLACE_EXISTING) != 0;
}

// Routes all running Discord processes' render output to targetDeviceFriendlyName
// using the undocumented Windows per-app audio endpoint API (Windows 10 1803+).
static void RouteDiscordAudioOutput(const std::wstring& targetDeviceFriendlyName, IMMDeviceEnumerator* pEnum) {
    if (targetDeviceFriendlyName.empty()) return;

    {
        std::wofstream reset(L"route-debug.log", std::ios::trunc);
        if (reset) {
            reset << L"Route attempt target: " << targetDeviceFriendlyName << L"\n";
        }
    }

    // Find the device ID for the target friendly name
    std::wstring targetDeviceId;
    {
        CComPtr<IMMDeviceCollection> pCol;
        if (FAILED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol))) return;
        UINT count = 0; pCol->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            CComPtr<IMMDevice> pDev;
            if (FAILED(pCol->Item(i, &pDev))) continue;
            CComPtr<IPropertyStore> pProps;
            if (FAILED(pDev->OpenPropertyStore(STGM_READ, &pProps))) continue;
            PROPVARIANT varName; PropVariantInit(&varName);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                std::wstring nameLower = varName.pwszVal;
                std::wstring targetLower = targetDeviceFriendlyName;
                for (auto& c : nameLower) c = towlower(c);
                for (auto& c : targetLower) c = towlower(c);
                if (nameLower.find(targetLower) != std::wstring::npos) {
                    LPWSTR pwstrId = nullptr;
                    if (SUCCEEDED(pDev->GetId(&pwstrId)) && pwstrId) {
                        targetDeviceId = pwstrId;
                        CoTaskMemFree(pwstrId);
                    }
                    PropVariantClear(&varName);
                    break;
                }
            }
            PropVariantClear(&varName);
        }
    }
    AppendRouteLog(L"Target device ID: " + (targetDeviceId.empty() ? std::wstring(L"(not found)") : targetDeviceId));
    if (targetDeviceId.empty()) return;
    std::wstring packedTargetDeviceId = PackPolicyConfigDeviceId(targetDeviceId, eRender);
    AppendRouteLog(L"Packed target device ID: " + packedTargetDeviceId);

    // Collect all Discord process IDs
    std::vector<DWORD> pids;
    std::vector<std::wstring> processPaths;
    {
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return;
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"Discord.exe") == 0) {
                    pids.push_back(pe.th32ProcessID);
                    std::wstring path = GetProcessImagePath(pe.th32ProcessID);
                    if (!path.empty()) processPaths.push_back(path);
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
    if (pids.empty()) {
        AppendRouteLog(L"No Discord.exe processes found.");
        return;
    }

    std::sort(processPaths.begin(), processPaths.end());
    processPaths.erase(std::unique(processPaths.begin(), processPaths.end()), processPaths.end());
    for (size_t i = 0; i < processPaths.size(); i++) {
        AppendRouteLog(L"Discord path[" + std::to_wstring(i) + L"]: " + processPaths[i]);
    }
    RemoveDiscordPersistedRenderEndpoints(processPaths);
    AppendRouteLog(L"Cleared persisted Discord render endpoints for current paths.");

    HMODULE hComBase = LoadLibraryW(L"combase.dll");
    if (!hComBase) {
        AppendRouteLog(L"LoadLibraryW(combase.dll) failed.");
        return;
    }

    auto fpRoInitialize          = (FnRoInitialize)          GetProcAddress(hComBase, "RoInitialize");
    auto fpRoUninitialize        = (FnRoUninitialize)        GetProcAddress(hComBase, "RoUninitialize");
    auto fpRoGetActivationFactory= (FnRoGetActivationFactory)GetProcAddress(hComBase, "RoGetActivationFactory");
    auto fpWindowsCreateString   = (FnWindowsCreateString)  GetProcAddress(hComBase, "WindowsCreateString");
    auto fpWindowsDeleteString   = (FnWindowsDeleteString)  GetProcAddress(hComBase, "WindowsDeleteString");
    auto fpWindowsGetStringRawBuffer =
        (FnWindowsGetStringRawBuffer)GetProcAddress(hComBase, "WindowsGetStringRawBuffer");

    if (!fpRoGetActivationFactory || !fpWindowsCreateString || !fpWindowsDeleteString) {
        AppendRouteLog(L"Missing one or more WinRT exports.");
        FreeLibrary(hComBase);
        return;
    }

    // RoGetActivationFactory requires WinRT to be initialized on the calling thread.
    // Try MTA first (worker thread); if the thread is STA, fall back to single-threaded.
    // S_FALSE = already initialized (fine). S_OK = we initialized it (uninit later).
    bool needUninit = false;
    if (fpRoInitialize) {
        HRESULT hrInit = fpRoInitialize(1 /*RO_INIT_MULTITHREADED*/);
        if (hrInit == (HRESULT)0x80010106 /*RO_E_CHANGED_MODE - thread is STA*/) {
            hrInit = fpRoInitialize(0 /*RO_INIT_SINGLETHREADED*/);
        }
        needUninit = (hrInit == S_OK);
        AppendRouteLog(L"RoInitialize: " + HResultToWString(hrInit));
    }

    const wchar_t* classIdStr = L"Windows.Media.Internal.AudioPolicyConfig";
    GUID policyConfigIid = ResolvePolicyConfigFactoryIid();
    AppendRouteLog(L"Windows build: " + std::to_wstring(GetWindowsBuildNumber()));
    HSTRING classIdHStr = nullptr;
    HRESULT hrCreateClass = fpWindowsCreateString(classIdStr, (UINT32)wcslen(classIdStr), &classIdHStr);
    AppendRouteLog(L"WindowsCreateString(classId): " + HResultToWString(hrCreateClass));
    if (SUCCEEDED(hrCreateClass)) {
        void* pConfig = nullptr;
        HRESULT hr = fpRoGetActivationFactory(classIdHStr, policyConfigIid, (void**)&pConfig);
        fpWindowsDeleteString(classIdHStr);
        AppendRouteLog(L"RoGetActivationFactory: " + HResultToWString(hr));

        if (SUCCEEDED(hr) && pConfig) {
            void** vtable = *reinterpret_cast<void***>(pConfig);
            auto releaseFactory = reinterpret_cast<ULONG (WINAPI*)(void*)>(
                vtable[POLICY_CONFIG_INDEX_RELEASE]);
            auto setPersistedDefaultAudioEndpoint =
                reinterpret_cast<HRESULT (WINAPI*)(void*, UINT, EDataFlow, ERole, HSTRING)>(
                    vtable[POLICY_CONFIG_INDEX_SET_PERSISTED_DEFAULT_ENDPOINT]);
            auto getPersistedDefaultAudioEndpoint =
                reinterpret_cast<HRESULT (WINAPI*)(void*, UINT, EDataFlow, ERole, HSTRING*)>(
                    vtable[POLICY_CONFIG_INDEX_GET_PERSISTED_DEFAULT_ENDPOINT]);

            HSTRING deviceIdHStr = nullptr;
            HRESULT hrCreateDevice = fpWindowsCreateString(
                packedTargetDeviceId.c_str(), (UINT32)packedTargetDeviceId.length(), &deviceIdHStr);
            AppendRouteLog(L"WindowsCreateString(deviceId): " + HResultToWString(hrCreateDevice));
            if (SUCCEEDED(hrCreateDevice)) {
                for (DWORD pid : pids) {
                    AppendRouteLog(L"PID " + std::to_wstring(pid));

                    HRESULT hrConsole = setPersistedDefaultAudioEndpoint(pConfig, pid, eRender, eConsole, deviceIdHStr);
                    HRESULT hrComm = setPersistedDefaultAudioEndpoint(pConfig, pid, eRender, eCommunications, deviceIdHStr);
                    HRESULT hrMedia = setPersistedDefaultAudioEndpoint(pConfig, pid, eRender, eMultimedia, deviceIdHStr);

                    AppendRouteLog(L"  Set eRender/eConsole: " + HResultToWString(hrConsole));
                    AppendRouteLog(L"  Set eRender/eCommunications: " + HResultToWString(hrComm));
                    AppendRouteLog(L"  Set eRender/eMultimedia: " + HResultToWString(hrMedia));

                    if (fpWindowsGetStringRawBuffer) {
                        HSTRING currentId = nullptr;
                        HRESULT hrGet = getPersistedDefaultAudioEndpoint(pConfig, pid, eRender, eConsole, &currentId);
                        AppendRouteLog(L"  Get eRender/eConsole: " + HResultToWString(hrGet));
                        if (SUCCEEDED(hrGet) && currentId) {
                            UINT32 len = 0;
                            PCWSTR raw = fpWindowsGetStringRawBuffer(currentId, &len);
                            if (raw) {
                                AppendRouteLog(L"  Current eRender/eConsole device: " + std::wstring(raw, len));
                            }
                            fpWindowsDeleteString(currentId);
                        }
                    }
                }
                fpWindowsDeleteString(deviceIdHStr);
            }
            releaseFactory(pConfig);
        }
    }

    if (needUninit && fpRoUninitialize) fpRoUninitialize();
    FreeLibrary(hComBase);
}

// ---------- Diagnostic ----------

// GuidToWString moved to mdd_pure.cpp

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
    if (HasCommandLineArg(argc, argv, "--shutdown") || HasCommandLineArg(argc, argv, "--exit")) {
        return RequestRunningInstanceShutdown() ? 0 : 1;
    }

    #ifdef DEBUG
    if (HasCommandLineArg(argc, argv, "-d")) {
        debugMode = true;
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

static HICON LoadTrayIcon() {
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    HICON hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (hIcon) return hIcon;
    SHSTOCKICONINFO sii = { sizeof(SHSTOCKICONINFO) };
    if (SUCCEEDED(SHGetStockIconInfo(SIID_AUDIOFILES, SHGSI_ICON | SHGSI_SMALLICON, &sii))) {
        return sii.hIcon;
    }
    return LoadIcon(NULL, IDI_APPLICATION);
}

void AddTrayIcon(HWND hWnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadTrayIcon();
    lstrcpyA(g_nid.szTip, "MuteDiscordDevice v" MDD_VERSION_STRING);

    // Retry NIM_ADD in case the shell tray isn't fully initialized (login,
    // Explorer restart, or just-installed scenarios). Clear any ghost entry
    // left by a prior process that crashed without calling NIM_DELETE.
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (Shell_NotifyIconA(NIM_ADD, &g_nid)) return;
        Shell_NotifyIconA(NIM_DELETE, &g_nid);
        Sleep(500);
    }
}

void RemoveTrayIcon(HWND) {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

void ShowTrayMenu(HWND hWnd, POINT pt) {
    HMENU hMenu = CreatePopupMenu();
    InsertMenuA(hMenu, -1, MF_BYPOSITION, ID_TRAY_ROUTETARGET, "Route Discord Audio...");
    InsertMenuA(hMenu, -1, MF_BYPOSITION, ID_TRAY_CONFIGURE,   "Configure Mute Devices...");
    InsertMenuA(hMenu, -1, MF_BYPOSITION, ID_TRAY_DIAGNOSE,    "Diagnose Discord Sessions...");
    InsertMenuA(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuA(hMenu, -1, MF_BYPOSITION, ID_TRAY_EXIT, "Exit");
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
    PostMessage(hWnd, WM_NULL, 0, 0);
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

// FilterSummary moved to mdd_pure.cpp

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

        s->hAllRadio = CreateWindowW(L"BUTTON",
            L"Mute all Discord sessions on this device  [Recommended]",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            10, y, 660, 20, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_ALL, g_hInst, NULL);
        SendMessageW(s->hAllRadio, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 26;

        s->hStreamRadio = CreateWindowW(L"BUTTON",
            L"Stream only (auto-detect) — only mutes when a Discord stream is live",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            10, y, 660, 20, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_STREAM, g_hInst, NULL);
        SendMessageW(s->hStreamRadio, WM_SETFONT, (WPARAM)hFont, TRUE);
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
            SendMessageW(s->hAllRadio, BM_SETCHECK, BST_CHECKED, 0);
        }

        if (ShouldDisableOwnerWindow(s->hOwner)) EnableWindow(s->hOwner, FALSE);
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
        if (st) RestoreOwnerWindow(st->hOwner);
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
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
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

        if (ShouldDisableOwnerWindow(s->hOwner)) EnableWindow(s->hOwner, FALSE);
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
            std::vector<DeviceConfig> previous = LoadDevicesFromFile(L"devices.txt");
            std::vector<DeviceConfig> checked = CollectCheckedConfigs(state->hList);
            if (!SaveDevicesToFile(L"devices.txt", checked)) {
                MessageBoxW(hWnd, L"Failed to save devices.txt", L"Error",
                    MB_OK | MB_ICONERROR);
                return 0;
            }
            DestroyWindow(hWnd);
            if (isDiscordRunning()) {
                std::vector<std::wstring> devicesToReset;
                for (const auto& oldCfg : previous) {
                    auto it = std::find_if(checked.begin(), checked.end(),
                        [&](const DeviceConfig& cfg) { return cfg.name == oldCfg.name; });
                    if (it == checked.end() ||
                        SerializeDeviceConfig(*it) != SerializeDeviceConfig(oldCfg)) {
                        devicesToReset.push_back(oldCfg.name);
                    }
                }

                if (!devicesToReset.empty()) {
                    CComPtr<IMMDeviceEnumerator> pEnum;
                    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                        IID_PPV_ARGS(&pEnum)))) {
                        for (const auto& deviceName : devicesToReset) {
                            UnmuteDiscordSessionsOnDevice(deviceName, pEnum);
                        }
                    }
                }
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
            RestoreOwnerWindow(state->hOwner);
            delete state;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        return 0;
    case WM_NCDESTROY:
        g_hConfigWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------- Route target picker dialog ----------

struct RouteTargetState {
    HWND hCombo;
    HWND hOwner;
    bool accepted = false;
    std::wstring selected;
};

static LRESULT CALLBACK RouteTargetWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RouteTargetState* state = reinterpret_cast<RouteTargetState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        RouteTargetState* s = reinterpret_cast<RouteTargetState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));

        RECT rc; GetClientRect(hWnd, &rc);
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND hLabel = CreateWindowW(L"STATIC", L"Route all Discord audio to:",
            WS_CHILD | WS_VISIBLE, 10, 14, rc.right - 20, 20, hWnd, NULL, g_hInst, NULL);
        SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        s->hCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
            10, 40, rc.right - 20, 200, hWnd, (HMENU)IDC_ROUTE_COMBO, g_hInst, NULL);
        SendMessageW(s->hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Populate with "(none)" + all active render devices
        SendMessageW(s->hCombo, CB_ADDSTRING, 0, (LPARAM)L"(none - disable routing)");
        std::wstring current = LoadRouteTargetFromFile();
        int selIdx = 0;
        int idx = 1;
        for (const auto& name : EnumerateRenderDevices()) {
            SendMessageW(s->hCombo, CB_ADDSTRING, 0, (LPARAM)name.c_str());
            if (!current.empty() && name == current) selIdx = idx;
            idx++;
        }
        SendMessageW(s->hCombo, CB_SETCURSEL, selIdx, 0);

        HWND hOk = CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            rc.right - 180, rc.bottom - 40, 80, 28,
            hWnd, (HMENU)IDC_ROUTE_OK, g_hInst, NULL);
        HWND hCancel = CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE,
            rc.right - 90, rc.bottom - 40, 80, 28,
            hWnd, (HMENU)IDC_ROUTE_CANCEL, g_hInst, NULL);
        SendMessageW(hOk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        if (ShouldDisableOwnerWindow(s->hOwner)) EnableWindow(s->hOwner, FALSE);
        return 0;
    }
    case WM_COMMAND: {
        if (!state) break;
        WORD id = LOWORD(wParam);
        if (id == IDC_ROUTE_OK) {
            int sel = (int)SendMessageW(state->hCombo, CB_GETCURSEL, 0, 0);
            if (sel == 0) {
                state->selected = L"";
            } else {
                wchar_t buf[512] = {};
                SendMessageW(state->hCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
                state->selected = buf;
            }
            state->accepted = true;
            DestroyWindow(hWnd);
        } else if (id == IDC_ROUTE_CANCEL) {
            DestroyWindow(hWnd);
        }
        break;
    }
    case WM_DESTROY:
        if (state) RestoreOwnerWindow(state->hOwner);
        // No PostQuitMessage here — this runs in a nested message loop inside
        // ShowRouteTargetDialog. PostQuitMessage would propagate WM_QUIT to WinMain
        // and exit the whole application when the dialog closes.
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ShowRouteTargetDialog(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = RouteTargetWndProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wc.lpszClassName = L"MuteDiscordRouteTargetClass";
        RegisterClassW(&wc);
        registered = true;
    }

    RouteTargetState state;
    state.hOwner = owner;

    int width = 500, height = 150;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    HWND hWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MuteDiscordRouteTargetClass",
        L"Route Discord Audio",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, width, height, owner, NULL, g_hInst, &state);
    if (!hWnd) return;

    MSG msg;
    bool sawQuit = false;
    while (IsWindow(hWnd)) {
        BOOL r = GetMessageW(&msg, NULL, 0, 0);
        if (r == 0) {
            sawQuit = true;
            break;
        }
        if (r == -1) break;
        if (IsDialogMessageW(hWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (sawQuit) {
        PostQuitMessage(0);
    }

    if (state.accepted) {
        SaveRouteTargetToFile(state.selected);
        // Apply immediately to any running Discord instances
        if (!state.selected.empty()) {
            CComPtr<IMMDeviceEnumerator> pEnum;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                CLSCTX_ALL, IID_PPV_ARGS(&pEnum)))) {
                RouteDiscordAudioOutput(state.selected, pEnum);
            }
        }
    }
}

void ShowConfigDialog(HWND owner) {
    if (g_hConfigWnd && !IsWindow(g_hConfigWnd)) {
        g_hConfigWnd = nullptr;
    }

    if (g_hConfigWnd) {
        if (IsIconic(g_hConfigWnd)) {
            ShowWindow(g_hConfigWnd, SW_RESTORE);
        } else {
            ShowWindow(g_hConfigWnd, SW_SHOW);
        }
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
    if (WM_TASKBARCREATED != 0 && msg == WM_TASKBARCREATED) {
        AddTrayIcon(hWnd);
        return 0;
    }
    if (WM_REQUEST_SHUTDOWN != 0 && msg == WM_REQUEST_SHUTDOWN) {
        RemoveTrayIcon(hWnd);
        if (g_exitEvent) SetEvent(g_exitEvent);
        DestroyWindow(hWnd);
        return 0;
    }
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
        else if (LOWORD(wParam) == ID_TRAY_ROUTETARGET) {
            ShowRouteTargetDialog(hWnd);
        }
        else if (LOWORD(wParam) == ID_TRAY_CONFIGURE) {
            ShowConfigDialog(hWnd);
        }
        else if (LOWORD(wParam) == ID_TRAY_DIAGNOSE) {
            RunDiagnostic(hWnd);
        }
        break;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam) {
            RemoveTrayIcon(hWnd);
            if (g_exitEvent) SetEvent(g_exitEvent);
            DestroyWindow(hWnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon(hWnd);
        if (g_exitEvent) SetEvent(g_exitEvent);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    if (HasCommandLineArg(__argc, __argv, "--shutdown") || HasCommandLineArg(__argc, __argv, "--exit")) {
        return RequestRunningInstanceShutdown() ? 0 : 1;
    }

    SetWorkingDirectoryToExeFolder();

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
    cwc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    if (!cwc.hIcon) cwc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    cwc.lpszClassName = L"MuteDiscordConfigClass";
    RegisterClassW(&cwc);

    // Register window messages BEFORE creating the tray window so there is no
    // gap where an incoming shutdown broadcast could arrive with an unhandled
    // message ID. RegisterWindowMessageW is system-wide and thread-safe; all
    // instances of this app will see the same message ID for a given name.
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    WM_REQUEST_SHUTDOWN = RegisterWindowMessageW(kShutdownMessageName);

    // Title must be non-empty: on some Windows 11 builds, DefWindowProc returns
    // FALSE from WM_NCCREATE when lpszName is "" and the window is never
    // created. The string is not user-visible (the window is hidden).
    g_hWnd = CreateWindowA(wc.lpszClassName, "MuteDiscordTray", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_hWnd) {
        return 1;
    }

    // Allow the broadcast messages to reach us even when running unelevated
    // alongside an elevated Explorer (UIPI blocks WM_USER+ range by default).
    ChangeWindowMessageFilterEx(g_hWnd, WM_TASKBARCREATED, MSGFLT_ALLOW, NULL);
    if (WM_REQUEST_SHUTDOWN) {
        ChangeWindowMessageFilterEx(g_hWnd, WM_REQUEST_SHUTDOWN, MSGFLT_ALLOW, NULL);
    }

    AddTrayIcon(g_hWnd);

    SeedDefaultDevicesIfFirstRun();

    g_workerThread = CreateThread(NULL, 0, WorkerThreadProc, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (g_hConfigWnd && IsWindow(g_hConfigWnd) && IsDialogMessageW(g_hConfigWnd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Give the worker a brief window to unblock from WMI / WASAPI calls.
    // If it can't exit in time (e.g. Restart Manager is waiting on us during
    // an installer upgrade), ExitProcess so we don't hang the installer.
    if (WaitForSingleObject(g_workerThread, 2000) != WAIT_OBJECT_0) {
        ExitProcess(0);
    }
    CloseHandle(g_workerThread);
    CloseHandle(g_exitEvent);

    CoUninitialize();
    return 0;
}
