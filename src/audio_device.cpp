#include "app_common.h"
#include <algorithm>

static CComPtr<IMMDevice> FindDeviceByName(const std::wstring& deviceName, IMMDeviceEnumerator* pEnumIn) {
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
        if (!IsDiscordProcess(pid)) continue;

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

// Builds a map of { deviceName -> [Discord PIDs on that device] }.
// If deviceNames is non-null, only enumerate devices whose name appears in the set.
std::unordered_map<std::wstring, std::vector<DWORD>> BuildDiscordPidsByDevice(
    IMMDeviceEnumerator* pEnumIn, const std::unordered_set<std::wstring>* deviceNames) {
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

        // skip devices not in the config filter set
        if (deviceNames && deviceNames->find(devName) == deviceNames->end()) continue;

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
            if (IsDiscordProcess(pid))
                result[devName].push_back(pid);
        }
    }
    return result;
}

// Returns: 0 = device not found, 1 = at least one matching session muted,
// 2 = device found but no matching Discord session (retry-worthy).
int MuteDiscordOnDevice(const DeviceConfig& cfg, const std::vector<DWORD>& otherPids, IMMDeviceEnumerator* pEnum) {
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
        std::unordered_set<DWORD> sharedPids;
        for (auto& s : sessions)
            if (std::find(otherPids.begin(), otherPids.end(), s.pid) != otherPids.end())
                sharedPids.insert(s.pid);
        if (sharedPids.size() != 1) {
            #ifdef DEBUG
            OutputDebugStringA("StreamOnly: ambiguous (not exactly one shared Discord process); deferring.\n");
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
                matches = std::find(otherPids.begin(), otherPids.end(), s.pid) != otherPids.end();
                break;
        }
        CComQIPtr<ISimpleAudioVolume> pVol(s.pControl);
        if (!matches) {
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

    if (!anyFilterMatched) return 2;
    return muted ? 1 : 2;
}

void UnmuteDiscordSessionsOnDevice(const std::wstring& deviceName, IMMDeviceEnumerator* pEnum) {
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
