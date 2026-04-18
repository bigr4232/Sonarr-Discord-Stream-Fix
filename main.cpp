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
#pragma comment(lib, "comctl32.lib")

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001
#define ID_TRAY_CONFIGURE 1002

#define IDC_CONFIG_LIST 2001
#define IDC_CONFIG_OK   2002
#define IDC_CONFIG_CANCEL 2003

HINSTANCE g_hInst = nullptr;
HWND g_hWnd = nullptr;
NOTIFYICONDATAA g_nid = { 0 };
HANDLE g_workerThread = nullptr;
HANDLE g_exitEvent = nullptr;

// Forward declarations
DWORD WINAPI WorkerThreadProc(LPVOID);
LRESULT CALLBACK TrayWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ConfigWndProc(HWND, UINT, WPARAM, LPARAM);
void AddTrayIcon(HWND);
void RemoveTrayIcon(HWND);
void ShowTrayMenu(HWND, POINT);
void ShowConfigDialog(HWND owner);
std::vector<std::wstring> EnumerateRenderDevices();
bool SaveDevicesToFile(const std::wstring& filename, const std::vector<std::wstring>& devices);
bool isDiscordRunning();
int checkToMute();

HWND g_hConfigWnd = nullptr;

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

std::vector<std::wstring> EnumerateRenderDevices() {
    std::vector<std::wstring> result;
    CComPtr<IMMDeviceEnumerator> pEnum;
    CComPtr<IMMDeviceCollection> pDevices;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&pEnum));
    if (FAILED(hr)) return result;

    hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pDevices);
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

std::vector<std::wstring> LoadDevicesFromFile(const std::wstring& filename) {
    std::vector<std::wstring> devices;
    std::wifstream file(filename);

    std::wstring line;
    while (std::getline(file, line)) {
        if (!line.empty()) devices.push_back(line);
    }
    return devices;
}

bool SaveDevicesToFile(const std::wstring& filename, const std::vector<std::wstring>& devices) {
    std::wstring tmp = filename + L".tmp";
    {
        std::wofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        for (const auto& d : devices) {
            out << d << L"\n";
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
    InsertMenuA(hMenu, -1, MF_BYPOSITION, ID_TRAY_CONFIGURE, "Configure Devices...");
    InsertMenuA(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuA(hMenu, -1, MF_BYPOSITION, ID_TRAY_EXIT, "Exit");
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

struct ConfigState {
    HWND hList;
    HWND hOk;
    HWND hCancel;
    HWND hOwner;
};

static void PopulateConfigList(HWND hList) {
    ListView_DeleteAllItems(hList);

    std::vector<std::wstring> enumerated = EnumerateRenderDevices();
    std::vector<std::wstring> configured = LoadDevicesFromFile(L"devices.txt");

    int row = 0;
    for (const auto& name : enumerated) {
        LVITEMW item = { 0 };
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(name.c_str());
        ListView_InsertItem(hList, &item);

        bool checked = std::find(configured.begin(), configured.end(), name) != configured.end();
        ListView_SetCheckState(hList, row, checked);
        row++;
    }

    for (const auto& name : configured) {
        if (std::find(enumerated.begin(), enumerated.end(), name) != enumerated.end()) continue;
        std::wstring display = name + L" (offline)";
        LVITEMW item = { 0 };
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(display.c_str());
        item.lParam = 1; // offline marker
        ListView_InsertItem(hList, &item);
        ListView_SetCheckState(hList, row, TRUE);
        row++;
    }
}

static std::vector<std::wstring> CollectCheckedDevices(HWND hList) {
    std::vector<std::wstring> result;
    int count = ListView_GetItemCount(hList);
    wchar_t buf[512];
    for (int i = 0; i < count; i++) {
        if (!ListView_GetCheckState(hList, i)) continue;

        LVITEMW item = { 0 };
        item.iItem = i;
        item.iSubItem = 0;
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.pszText = buf;
        item.cchTextMax = ARRAYSIZE(buf);
        ListView_GetItem(hList, &item);

        std::wstring name = buf;
        if (item.lParam == 1) {
            const std::wstring suffix = L" (offline)";
            if (name.size() > suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                name.erase(name.size() - suffix.size());
            }
        }
        result.push_back(name);
    }
    return result;
}

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

        LVCOLUMNW col = { 0 };
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<LPWSTR>(L"Device");
        col.cx = rc.right - 50;
        ListView_InsertColumn(s->hList, 0, &col);

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
        SetWindowPos(state->hOk, NULL, w - 180, h - 40, 80, 28, SWP_NOZORDER);
        SetWindowPos(state->hCancel, NULL, w - 90, h - 40, 80, 28, SWP_NOZORDER);
        ListView_SetColumnWidth(state->hList, 0, w - 50);
        return 0;
    }
    case WM_COMMAND: {
        if (!state) break;
        WORD id = LOWORD(wParam);
        if (id == IDC_CONFIG_OK) {
            std::vector<std::wstring> checked = CollectCheckedDevices(state->hList);
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
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        if (state) {
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

    int width = 600;
    int height = 450;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    g_hConfigWnd = CreateWindowExW(0, L"MuteDiscordConfigClass",
        L"Configure Devices to Mute",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
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
            // Signal the worker thread to exit
            SetEvent(g_exitEvent);
            PostQuitMessage(0);
        }
        else if (LOWORD(wParam) == ID_TRAY_CONFIGURE) {
            ShowConfigDialog(hWnd);
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

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // Register a window class for tray icon messages
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "MuteDiscordTrayClass";
    RegisterClassA(&wc);

    // Register the config dialog window class
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

    // Start your main logic in a worker thread
    g_workerThread = CreateThread(NULL, 0, WorkerThreadProc, NULL, 0, NULL);

    // Standard message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (g_hConfigWnd && IsWindow(g_hConfigWnd) && IsDialogMessageW(g_hConfigWnd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Wait for worker thread to finish
    WaitForSingleObject(g_workerThread, INFINITE);
    CloseHandle(g_workerThread);
    CloseHandle(g_exitEvent);

    CoUninitialize();
    return 0;
}
