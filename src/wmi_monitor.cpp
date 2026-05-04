#include "app_common.h"
#include <wbemidl.h>
#include <comdef.h>
#include <sstream>

/// Entry point for the background WMI-monitoring worker thread.
/// Listens for Discord.exe process-creation events and calls checkToMute().
static void WmiMonitorThreadProc() {
    HRESULT hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hres)) return;

    hres = CoInitializeSecurity(
        NULL, -1, NULL, NULL,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE, NULL);
    if (FAILED(hres)) {
        CoUninitialize();
        return;
    }

    IWbemLocator* pLoc = nullptr;
    hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, (LPVOID*)&pLoc);
    if (FAILED(hres)) { CoUninitialize(); return; }

    IWbemServices* pSvc = nullptr;
    hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
    pLoc->Release();
    if (FAILED(hres)) { CoUninitialize(); return; }

    hres = CoSetProxyBlanket(
        pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE);
    if (FAILED(hres)) { pSvc->Release(); CoUninitialize(); return; }

    IEnumWbemClassObject* pEnumerator = nullptr;
    hres = pSvc->ExecNotificationQuery(
        _bstr_t("WQL"),
        _bstr_t("SELECT * FROM __InstanceCreationEvent WITHIN 1 WHERE TargetInstance ISA 'Win32_Process' AND TargetInstance.Name = 'Discord.exe'"),
        WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY,
        NULL,
        &pEnumerator);
    if (FAILED(hres)) { pSvc->Release(); CoUninitialize(); return; }

    // Periodic re-check while Discord is running. There is no upper bound on
    // when the user starts streaming after launching Discord — the stream
    // subprocess may open new audio sessions any time later without spawning
    // a new Discord.exe (joining a voice call, starting Go Live on an
    // existing process). Process-creation events alone can't catch that, so
    // we tick checkToMute() on a fixed interval as a safety net. checkToMute
    // is idempotent — muting an already-muted session is a no-op.
    //
    // 5s is the worst-case latency between a stream session appearing and
    // mute being applied. Cost per tick when Discord isn't running is one
    // CreateToolhelp32Snapshot (~1-5ms); when it is running, an additional
    // device/session enumeration. Negligible CPU/battery impact.
    static const ULONGLONG kPeriodicCheckIntervalMs = 5000;
    ULONGLONG lastPeriodicCheckMs = GetTickCount64();

    if (isDiscordRunning()) {
        #ifdef DEBUG
        OutputDebugStringA("Discord is running.\n");
        #endif
        checkToMute();
        lastPeriodicCheckMs = GetTickCount64();
    }

    #ifdef DEBUG
    OutputDebugStringA("Listening for Discord.exe startup events...\n");
    #endif

    while (WaitForSingleObject(g_exitEvent, 0) == WAIT_TIMEOUT) {
        IWbemClassObject* pEvent = nullptr;
        ULONG uReturn = 0;
        HRESULT hr = pEnumerator->Next(500, 1, &pEvent, &uReturn);
        if (SUCCEEDED(hr) && uReturn != 0) {
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
                            lastPeriodicCheckMs = GetTickCount64();
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
        }
        if (pEvent) pEvent->Release();

        if (GetTickCount64() - lastPeriodicCheckMs >= kPeriodicCheckIntervalMs) {
            if (isDiscordRunning()) {
                #ifdef DEBUG
                OutputDebugStringA("Periodic re-check: calling checkToMute.\n");
                #endif
                checkToMute();
            }
            lastPeriodicCheckMs = GetTickCount64();
        }
    }

    // Release the enumerator promptly on shutdown instead of waiting for scope exit.
    if (pEnumerator) {
        pEnumerator->Release();
        pEnumerator = nullptr;
    }
    pSvc->Release();
    CoUninitialize();
}

DWORD WINAPI WorkerThreadProc(LPVOID) {
    WmiMonitorThreadProc();
    return 0;
}
