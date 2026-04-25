#include "app_common.h"
#include <fstream>
#include <algorithm>
#include <sstream>

// HSTRING function pointer types
typedef HRESULT (WINAPI *FnRoInitialize)(UINT type);
typedef void    (WINAPI *FnRoUninitialize)();
typedef HRESULT (WINAPI *FnRoGetActivationFactory)(HSTRING classId, REFIID iid, void** factory);
typedef HRESULT (WINAPI *FnWindowsCreateString)(LPCWSTR src, UINT32 len, HSTRING* out);
typedef HRESULT (WINAPI *FnWindowsDeleteString)(HSTRING str);
typedef PCWSTR  (WINAPI *FnWindowsGetStringRawBuffer)(HSTRING str, UINT32* len);

// Per-app audio routing factory (Windows 10 1803+, Windows 11).
static const GUID IID_IAudioPolicyConfigFactory_Downlevel =
    { 0x2a59116d, 0x6c4f, 0x45e0, { 0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58 } };
static const GUID IID_IAudioPolicyConfigFactory_21H2 =
    { 0xab3d4648, 0xe242, 0x459f, { 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24 } };

static const int POLICY_CONFIG_INDEX_RELEASE = 2;
static const int POLICY_CONFIG_INDEX_SET_PERSISTED_DEFAULT_ENDPOINT = 25;
static const int POLICY_CONFIG_INDEX_GET_PERSISTED_DEFAULT_ENDPOINT = 26;
static const DWORD BUILD_WINDOWS_10_21H2_IID_SWITCH = 21390;

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

static void RemoveDiscordPersistedRenderEndpoints(const std::vector<std::wstring>& processPaths) {
    if (processPaths.empty()) return;

    std::vector<std::wstring> loweredPaths;
    loweredPaths.reserve(processPaths.size() * 2);
    for (const auto& path : processPaths) {
        if (path.empty()) continue;
        std::wstring lowerPath = ToLowerCopy(path);
        loweredPaths.push_back(lowerPath);

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

void RouteDiscordAudioOutput(const std::wstring& targetDeviceFriendlyName, IMMDeviceEnumerator* pEnum) {
    if (targetDeviceFriendlyName.empty()) return;

    {
        std::wofstream reset(L"route-debug.log", std::ios::trunc);
        if (reset) {
            reset << L"Route attempt target: " << targetDeviceFriendlyName << L"\n";
        }
    }

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

    bool needUninit = false;
    if (fpRoInitialize) {
        HRESULT hrInit = fpRoInitialize(1 /*RO_INIT_MULTITHREADED*/);
        if (hrInit == (HRESULT)0x80010106 /*RO_E_CHANGED_MODE*/) {
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
