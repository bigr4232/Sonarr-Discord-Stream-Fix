#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <atlbase.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <tlhelp32.h>
#include <wbemidl.h>
#include <comdef.h>
#include <sstream>
#include <locale>
#include <codecvt>
#include <tlhelp32.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "shell32.lib")

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001

HINSTANCE g_hInst = nullptr;
HWND g_hWnd = nullptr;
NOTIFYICONDATAA g_nid = { 0 };
HANDLE g_workerThread = nullptr;
HANDLE g_exitEvent = nullptr;

// Forward declarations
DWORD WINAPI WorkerThreadProc(LPVOID);
LRESULT CALLBACK TrayWndProc(HWND, UINT, WPARAM, LPARAM);
void AddTrayIcon(HWND);
void RemoveTrayIcon(HWND);
void ShowTrayMenu(HWND, POINT);

bool debugMode = false;

std::wstring GetProcessName(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return L"";
    PROCESSENTRY32W entry = { sizeof(PROCESSENTRY32W) };
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                CloseHandle(snap);
                return entry.szExeFile;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return L"";
}

int MuteDiscordOnDevice(const std::wstring& deviceName) {
    HRESULT hr;
    CComPtr<IMMDeviceEnumerator> pEnum;
    CComPtr<IMMDeviceCollection> pDevices;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&pEnum));
    if (FAILED(hr)) return 0;

    hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pDevices);
    if (FAILED(hr)) return 0;

    UINT count = 0;
    pDevices->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        CComPtr<IMMDevice> pDevice;
        pDevices->Item(i, &pDevice);

        CComPtr<IPropertyStore> pProps;
        pDevice->OpenPropertyStore(STGM_READ, &pProps);

        PROPVARIANT varName;
        PropVariantInit(&varName);
        pProps->GetValue(PKEY_Device_FriendlyName, &varName);

        if (deviceName == varName.pwszVal) {
            #ifdef DEBUG
            std::wstring test = varName.pwszVal;
            std::wstring output = L"Found device: " + test + L"\n";
			OutputDebugStringW(output.c_str());
            #endif

            CComPtr<IAudioSessionManager2> pSessionMgr;
            hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                nullptr, (void**)&pSessionMgr);
            if (FAILED(hr)) {
                PropVariantClear(&varName);
                return 0;
            }

            CComPtr<IAudioSessionEnumerator> pEnumSessions;
            pSessionMgr->GetSessionEnumerator(&pEnumSessions);

            int sessionCount = 0;
            pEnumSessions->GetCount(&sessionCount);

            bool muted = false;
            for (int j = 0; j < sessionCount; j++) {
                CComPtr<IAudioSessionControl> pControl;
                pEnumSessions->GetSession(j, &pControl);

                CComQIPtr<IAudioSessionControl2> pControl2(pControl);
                if (!pControl2) continue;

                DWORD pid = 0;
                pControl2->GetProcessId(&pid);
                std::wstring procName = GetProcessName(pid);

                if (_wcsicmp(procName.c_str(), L"Discord.exe") == 0) {
                    CComQIPtr<ISimpleAudioVolume> pVolume(pControl);
                    if (pVolume) {
                        hr = pVolume->SetMute(TRUE, nullptr);
                        if (SUCCEEDED(hr)) {
                            #ifdef DEBUG
                            std::wstring output;
                            output = L"Muted Discord on: " + deviceName + L"\n";
							OutputDebugStringW(output.c_str());
                            #endif
                            muted = true;
                        }
                    }
                }
            }

            if (!muted) {
                #ifdef DEBUG
                std::string output = "Discord session not found on this device.\n";
				OutputDebugStringA(output.c_str());
                #endif
                PropVariantClear(&varName);
                return 2;
            }

            PropVariantClear(&varName);
            return 1;
        }

        PropVariantClear(&varName);
    }

        #ifdef DEBUG
        //setup converter
        std::wstring output = L"Device not found: " + deviceName + L"\n";
		OutputDebugStringW(output.c_str());
        #endif
    return 0;
}

std::vector<std::wstring> LoadDevicesFromFile(const std::wstring& filename) {
    std::vector<std::wstring> devices;
    std::wifstream file(filename);

    std::wstring line;
    while (std::getline(file, line)) {
        if (!line.empty()) devices.push_back(line);
    }
    return devices;
}

int checkToMute() {

    std::wstring configFile = L"devices.txt";
    auto devices = LoadDevicesFromFile(configFile);

    if (devices.empty()) {
        #ifdef DEBUG
        std::string output = "No devices found in devices.txt\n";
		OutputDebugStringA(output.c_str());
        #endif
        return 1;
    }

    int check = 0;
    for (int i = 0; i < (int)devices.size(); i++) {
        check = MuteDiscordOnDevice(devices[i]);
        if (check == 2) {
            i = -1;
            #ifdef DEBUG
            std::string output = "Sleeping for 5 seconds\n";
			OutputDebugStringA(output.c_str());
            #endif
            Sleep(5000);
        }
    }
    return 0;
}

bool isDiscordRunning() {
    bool found = false;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            std::wstring exeName = pe32.szExeFile;
            if (_wcsicmp(exeName.c_str(), L"Discord.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);
    return found;
}


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
    if (FAILED(hres)) {
        CoUninitialize();
        return 1;
    }

    IWbemServices* pSvc = nullptr;
    hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
    pLoc->Release();
    if (FAILED(hres)) {
        CoUninitialize();
        return 1;
    }

    hres = CoSetProxyBlanket(
        pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE);

    if (FAILED(hres)) {
        pSvc->Release();
        CoUninitialize();
        return 1;
    }

    IEnumWbemClassObject* pEnumerator = nullptr;
    hres = pSvc->ExecNotificationQuery(
        _bstr_t("WQL"),
        _bstr_t("SELECT * FROM __InstanceCreationEvent WITHIN 1 WHERE TargetInstance ISA 'Win32_Process'"),
        WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY,
        NULL,
        &pEnumerator);

    if (FAILED(hres)) {
        pSvc->Release();
        CoUninitialize();
        return 1;
    }

    if (isDiscordRunning()) {
        #ifdef DEBUG
        std::string output = "Discord is running.\n";
		OutputDebugStringA(output.c_str());
        #endif
		checkToMute();

    }

    #ifdef DEBUG
    std::string output = "Listening for Discord.exe startup events...\n";
	OutputDebugStringA(output.c_str());
    #endif

    while (WaitForSingleObject(g_exitEvent, 0) == WAIT_TIMEOUT) {
        IWbemClassObject* pEvent = nullptr;
        ULONG uReturn = 0;

        HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pEvent, &uReturn);

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
                        std::string output = "Discord.exe just started!\n";
						OutputDebugStringA(output.c_str());
                        #endif
                        checkToMute();
                    }
                }
                VariantClear(&vtName);
                // No manual Release(); CComPtr does it safely
            }
            #ifdef DEBUG
            else {
                std::stringstream ss;
                ss << "[Debug] QueryInterface failed, hr=0x" << std::hex << qi << std::endl;
                std::string output = ss.str();
				OutputDebugStringA(output.c_str());
                

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

// --- System Tray Implementation ---

DWORD WINAPI WorkerThreadProc(LPVOID lpParam) {
    // Call your main logic here, but replace the infinite loop with a check for g_exitEvent
    // For example, in your main event loop:
    // while (WaitForSingleObject(g_exitEvent, 0) == WAIT_TIMEOUT) { ... }
    // If g_exitEvent is signaled, break and clean up.
    main(__argc, __argv); // Or pass argc/argv as needed
    return 0;
}

void AddTrayIcon(HWND hWnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpyA(g_nid.szTip, "MuteDiscordDevice_Config");
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

void RemoveTrayIcon(HWND hWnd) {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

void ShowTrayMenu(HWND hWnd, POINT pt) {
    HMENU hMenu = CreatePopupMenu();
    InsertMenuA(hMenu, -1, MF_BYPOSITION, ID_TRAY_EXIT, "Exit");
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
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
            // Signal the worker thread to exit
            SetEvent(g_exitEvent);
            PostQuitMessage(0);
        }
        break;
    case WM_DESTROY:
        RemoveTrayIcon(hWnd);
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    g_hInst = hInstance;
    g_exitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    // Register a window class for tray icon messages
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "MuteDiscordTrayClass";
    RegisterClassA(&wc);

    g_hWnd = CreateWindowA(wc.lpszClassName, "", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    AddTrayIcon(g_hWnd);

    // Start your main logic in a worker thread
    g_workerThread = CreateThread(NULL, 0, WorkerThreadProc, NULL, 0, NULL);

    // Standard message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Wait for worker thread to finish
    WaitForSingleObject(g_workerThread, INFINITE);
    CloseHandle(g_workerThread);
    CloseHandle(g_exitEvent);

    return 0;
}
